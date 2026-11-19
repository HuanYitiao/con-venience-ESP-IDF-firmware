#include "contacts.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "sdcard.h"

static const char *TAG = "contacts";

#define SELF_JSON SDCARD_MOUNT_POINT "/self_profile/profile.json"
#define SELF_AVATAR SDCARD_MOUNT_POINT "/self_profile/avatar.bin"
// Larger than any real profile (about 200 bytes); a bound for a hostile file.
#define PROFILE_JSON_MAX 2048

// Copies a UTF-8 string, cut on a character boundary if it does not fit.
static void copy_utf8(char *dst, size_t size, const char *src)
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
    copy_utf8(dst, size, cJSON_IsString(item) ? item->valuestring : "");
}

// The uuid ends up in paths (friends_profiles/<uuid>): exactly 16 hex digits.
static bool valid_uuid(const char *s)
{
    if (!s || strlen(s) != CONTACT_UUID_LEN - 1)
    {
        return false;
    }
    for (const char *p = s; *p; p++)
    {
        if (!isxdigit((unsigned char)*p))
        {
            return false;
        }
    }
    return true;
}

static esp_err_t parse_profile(const char *json, contact_t *out)
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
    if (cJSON_IsString(uuid) && valid_uuid(uuid->valuestring))
    {
        copy_utf8(out->uuid, sizeof(out->uuid), uuid->valuestring);
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
        copy_field(l->platform, sizeof(l->platform), link, "platform");
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

esp_err_t contacts_load_self(contact_t *out)
{
    memset(out, 0, sizeof(*out));

    FILE *f = fopen(SELF_JSON, "r");
    if (!f)
    {
        return ESP_ERR_NOT_FOUND;
    }
    static char json[PROFILE_JSON_MAX + 1];
    size_t      n = fread(json, 1, PROFILE_JSON_MAX, f);
    fclose(f);
    json[n] = '\0';

    esp_err_t err = parse_profile(json, out);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Self profile does not parse");
        return err;
    }

    size_t avatar_len = (size_t)out->avatar_res * out->avatar_res / 4;
    if (avatar_len && !load_avatar(SELF_AVATAR, out->avatar, avatar_len))
    {
        ESP_LOGW(TAG, "Self avatar missing or short, need %u bytes", (unsigned)avatar_len);
        out->avatar_res = 0;
    }
    ESP_LOGI(TAG, "Self: %s, %u links, avatar %u px", out->name, out->link_count, out->avatar_res);
    return ESP_OK;
}

void contacts_fake(contact_t *out)
{
    memset(out, 0, sizeof(*out));
    copy_utf8(out->name, sizeof(out->name), "Test Fox");
    copy_utf8(out->species, sizeof(out->species), "Red fox");
    copy_utf8(out->from, sizeof(out->from), "Malmö, Sweden");
    copy_utf8(out->uuid, sizeof(out->uuid), "0000000000000000");
    copy_utf8(out->links[0].platform, sizeof(out->links[0].platform), "telegram");
    copy_utf8(out->links[0].url, sizeof(out->links[0].url), "https://t.me/example");
    out->link_count = 1;

    // Diagonal bands through the four gray levels.
    out->avatar_res = CONTACT_AVATAR_RES_MAX;
    out->avatar_mode = 1;
    for (int page = 0; page < CONTACT_AVATAR_RES_MAX / 4; page++)
    {
        for (int x = 0; x < CONTACT_AVATAR_RES_MAX; x++)
        {
            uint8_t gray = ((x + page * 4) / 32) & 3;
            out->avatar[page * CONTACT_AVATAR_RES_MAX + x] = gray * 0x55;
        }
    }
}
