// Reading contacts: profile parsing, the own profile, and the friend list.

#include "contacts.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "contacts_priv.h"
#include "esp_log.h"

static const char *TAG = "contacts";

// Copies a UTF-8 string, cut on a character boundary if it does not fit.
void contacts_copy_utf8(char *dst, size_t size, const char *src)
{
    size_t n = src ? strlen(src) : 0;
    if (n >= size)
    {
        n = size - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80)
        {
            n--;  // do not split a multi-byte character
        }
    }
    if (n)
    {
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

static void copy_field(char *dst, size_t size, const cJSON *obj, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    contacts_copy_utf8(dst, size, cJSON_IsString(item) ? item->valuestring : "");
}

// The uuid ends up in paths (friends_profiles/<uuid>): exactly 16 hex digits.
bool contacts_uuid_normalize(const char *s, char out[CONTACT_UUID_LEN])
{
    if (!s || strlen(s) != CONTACT_UUID_LEN - 1)
    {
        return false;
    }
    for (int i = 0; i < CONTACT_UUID_LEN - 1; i++)
    {
        if (!isxdigit((unsigned char)s[i]))
        {
            return false;
        }
    }
    if (out)
    {
        for (int i = 0; i < CONTACT_UUID_LEN - 1; i++)
        {
            out[i] = (char)toupper((unsigned char)s[i]);
        }
        out[CONTACT_UUID_LEN - 1] = '\0';
    }
    return true;
}

esp_err_t contacts_parse_profile(const char *json, contact_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    copy_field(out->name, sizeof(out->name), root, "name");
    copy_field(out->species, sizeof(out->species), root, "species");
    copy_field(out->from, sizeof(out->from), root, "from");

    const cJSON *uuid = cJSON_GetObjectItemCaseSensitive(root, "uuid");
    if (!cJSON_IsString(uuid) || !contacts_uuid_normalize(uuid->valuestring, out->uuid))
    {
        out->uuid[0] = '\0';
    }

    const cJSON *links = cJSON_GetObjectItemCaseSensitive(root, "links");
    const cJSON *link;
    cJSON_ArrayForEach(link, links)
    {
        if (out->link_count >= CONTACT_LINKS_MAX)
        {
            break;
        }
        contact_link_t *l = &out->links[out->link_count++];
        // "tag" is what some early profiles call the platform (old firmware).
        const cJSON *platform = cJSON_GetObjectItemCaseSensitive(link, "platform");
        copy_field(l->platform, sizeof(l->platform), link, cJSON_IsString(platform) ? "platform" : "tag");
        copy_field(l->url, sizeof(l->url), link, "url");
    }

    const cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "avatar_res");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "avatar_mode");
    if (cJSON_IsNumber(res) && res->valueint > 0 && res->valueint <= CONTACT_AVATAR_RES_MAX
        && res->valueint % 4 == 0)
    {
        out->avatar_res = (uint8_t)res->valueint;
    }
    out->avatar_mode = cJSON_IsNumber(mode) ? (uint8_t)mode->valueint : 0;

    cJSON_Delete(root);
    return ESP_OK;
}

int contacts_read_text(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        return -1;
    }
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

// Reads exactly len bytes; anything shorter counts as no avatar.
static bool load_avatar(const char *path, uint8_t *buf, size_t len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        return false;
    }
    size_t n = fread(buf, 1, len, f);
    fclose(f);
    return n == len;
}

// profile.json and, with_avatar, avatar.bin from one profile directory.
static esp_err_t load_profile(const char *dir, contact_t *out, bool with_avatar)
{
    memset(out, 0, sizeof(*out));

    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/profile.json", dir);
    static char json[PROFILE_JSON_MAX + 1];
    if (contacts_read_text(path, json, sizeof(json)) < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = contacts_parse_profile(json, out);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "%s: profile does not parse", dir);
        return err;
    }
    if (!with_avatar)
    {
        return ESP_OK;
    }

    size_t avatar_len = (size_t)out->avatar_res * out->avatar_res / 4;
    snprintf(path, sizeof(path), "%s/avatar.bin", dir);
    if (avatar_len && !load_avatar(path, out->avatar, avatar_len))
    {
        ESP_LOGW(TAG, "%s: avatar missing or short, need %u bytes", dir, (unsigned)avatar_len);
        out->avatar_res = 0;
    }
    return ESP_OK;
}

esp_err_t contacts_load_self(contact_t *out)
{
    contacts_recover_file(SELF_JSON);
    contacts_recover_file(SELF_AVATAR);
    esp_err_t err = load_profile(SELF_DIR, out, true);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Self: %s, %s, %u links, avatar %u px", out->name,
                 out->uuid[0] ? out->uuid : "no uuid", out->link_count, out->avatar_res);
    }
    return err;
}

// ── Friends ──────────────────────────────────────────────
// One directory per friend, named after the uuid. The list keeps the uuid
// and name of each, in directory order (as the old firmware), and loads the
// rest on demand.

char contacts_friend_uuids[CONTACTS_FRIENDS_MAX][CONTACT_UUID_LEN];
char contacts_friend_names[CONTACTS_FRIENDS_MAX][CONTACT_NAME_LEN];
int  contacts_friend_count;
bool contacts_friends_fake;

int contacts_list_friends(void)
{
    contacts_friend_count = 0;
    contacts_friends_fake = false;
    contacts_recover_friends();
    DIR *dir = opendir(FRIENDS_DIR);
    if (!dir)
    {
        return 0;
    }
    static contact_t c;  // 4.8 KB: not on the stack
    struct dirent   *e;
    while (contacts_friend_count < CONTACTS_FRIENDS_MAX && (e = readdir(dir)) != NULL)
    {
        char uuid[CONTACT_UUID_LEN];
        if (e->d_type != DT_DIR || !contacts_uuid_normalize(e->d_name, uuid))
        {
            continue;
        }
        char path[PATH_LEN];
        snprintf(path, sizeof(path), "%s/%s", FRIENDS_DIR, uuid);
        if (load_profile(path, &c, false) != ESP_OK)
        {
            continue;
        }
        memcpy(contacts_friend_uuids[contacts_friend_count], uuid, CONTACT_UUID_LEN);
        contacts_copy_utf8(contacts_friend_names[contacts_friend_count], CONTACT_NAME_LEN, c.name);
        contacts_friend_count++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "%d friends", contacts_friend_count);
    return contacts_friend_count;
}

const char *contacts_friend_name(int index)
{
    return index >= 0 && index < contacts_friend_count ? contacts_friend_names[index] : "";
}

int contacts_find_friend(const char *uuid)
{
    char id[CONTACT_UUID_LEN];
    if (!contacts_uuid_normalize(uuid, id))
    {
        return -1;
    }
    for (int i = 0; i < contacts_friend_count; i++)
    {
        if (strcmp(contacts_friend_uuids[i], id) == 0)
        {
            return i;
        }
    }
    return -1;
}

esp_err_t contacts_load_friend(int index, contact_t *out)
{
    if (index < 0 || index >= contacts_friend_count)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (contacts_friends_fake)
    {
        contacts_fake_friend(index, out);
        return ESP_OK;
    }
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", FRIENDS_DIR, contacts_friend_uuids[index]);
    return load_profile(path, out, true);
}
