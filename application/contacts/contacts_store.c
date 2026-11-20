// Writing contacts. Nothing the user made is erased: a deleted friend and an
// archived profile are moved aside, to be restored by hand on a PC (as the
// old firmware). Every write survives a reset at any point (battery pulled,
// auto power-off): files go through a temporary copy, friend directories
// through a staging directory, and the next read finishes or undoes what
// a reset interrupted.
//
// FAT renames do not replace an existing target, so a replace is always
// "move the old one aside, move the new one in, drop the old one".

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "contacts.h"
#include "contacts_priv.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "contacts";

// snprintf into a PATH_LEN buffer; false if the path does not fit.
__attribute__((format(printf, 2, 3))) static bool make_path(char out[PATH_LEN], const char *fmt,
                                                             ...)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(out, PATH_LEN, fmt, args);
    va_end(args);
    return n >= 0 && n < PATH_LEN;
}

bool contacts_path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static esp_err_t ensure_dir(const char *path)
{
    if (mkdir(path, 0777) == 0 || errno == EEXIST || contacts_path_exists(path))
    {
        return ESP_OK;
    }
    ESP_LOGE(TAG, "mkdir %s failed (errno %d)", path, errno);
    return ESP_FAIL;
}

// Writes and syncs a whole file, so it is complete on the card on return.
static esp_err_t write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        ESP_LOGE(TAG, "Open %s for writing failed (errno %d)", path, errno);
        return ESP_FAIL;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok)
    {
        ESP_LOGE(TAG, "Write %s failed (errno %d)", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t contacts_write_atomic(const char *path, const void *data, size_t len)
{
    char tmp[PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    esp_err_t err = write_file(tmp, data, len);
    if (err != ESP_OK)
    {
        unlink(tmp);
        return err;
    }
    // From here the .tmp is complete: a reset before the rename leaves it for
    // contacts_recover_file().
    if (unlink(path) != 0 && errno != ENOENT)
    {
        ESP_LOGE(TAG, "Remove %s failed (errno %d)", path, errno);
        return ESP_FAIL;
    }
    if (rename(tmp, path) != 0)
    {
        ESP_LOGE(TAG, "Rename to %s failed (errno %d)", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// A .tmp next to an existing file may be cut short (the reset came while it
// was written) and is left for the next write to replace. Only without the
// file is it known to be complete.
void contacts_recover_file(const char *path)
{
    char tmp[PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (!contacts_path_exists(path) && contacts_path_exists(tmp))
    {
        int r = rename(tmp, path);
        ESP_LOGW(TAG, "Recovered %s from its .tmp: %s", path, r == 0 ? "ok" : "failed");
    }
}

// Removes a directory and everything in it. Restarts the listing after each
// removal, rather than changing a directory while it is being read.
static esp_err_t remove_tree(const char *path)
{
    for (;;)
    {
        DIR *dir = opendir(path);
        if (!dir)
        {
            return errno == ENOENT ? ESP_OK : ESP_FAIL;
        }
        struct dirent *e;
        char           child[PATH_LEN];
        bool           is_dir = false;
        child[0] = '\0';
        while ((e = readdir(dir)) != NULL)
        {
            if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0)
            {
                if (!make_path(child, "%s/%s", path, e->d_name))
                {
                    closedir(dir);
                    return ESP_ERR_INVALID_SIZE;
                }
                is_dir = e->d_type == DT_DIR;
                break;
            }
        }
        closedir(dir);
        if (!child[0])
        {
            break;
        }
        esp_err_t err = is_dir ? remove_tree(child) : (unlink(child) == 0 ? ESP_OK : ESP_FAIL);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Remove %s failed (errno %d)", child, errno);
            return err;
        }
    }
    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

static bool ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

// A save writes <uuid>.new, moves <uuid> to <uuid>.old, moves <uuid>.new to
// <uuid>, and removes <uuid>.old. After a reset:
//   <uuid>.new: may be incomplete; removed (the save is lost, the old
//     friend stays).
//   <uuid>.old with <uuid>: the save finished; removed.
//   <uuid>.old without <uuid>: the reset came between the two moves; moved
//     back, so the friend stays as it was.
void contacts_recover_friends(void)
{
    // Leftovers are rare and few: collect some, fix them, look again.
    for (int pass = 0; pass < 8; pass++)
    {
        DIR *dir = opendir(FRIENDS_DIR);
        if (!dir)
        {
            return;
        }
        char           found[4][PATH_LEN / 2];
        int            n = 0;
        struct dirent *e;
        while (n < 4 && (e = readdir(dir)) != NULL)
        {
            if (e->d_type == DT_DIR && strlen(e->d_name) < sizeof(found[0])
                && (ends_with(e->d_name, STAGING_NEW) || ends_with(e->d_name, STAGING_OLD)))
            {
                strcpy(found[n++], e->d_name);
            }
        }
        closedir(dir);
        if (n == 0)
        {
            return;
        }

        for (int i = 0; i < n; i++)
        {
            char path[PATH_LEN];
            snprintf(path, sizeof(path), "%s/%s", FRIENDS_DIR, found[i]);
            char base[PATH_LEN];
            snprintf(base, sizeof(base), "%s", path);
            base[strlen(base) - strlen(STAGING_OLD)] = '\0';  // both suffixes are 4 long

            if (ends_with(found[i], STAGING_OLD) && !contacts_path_exists(base))
            {
                int r = rename(path, base);
                ESP_LOGW(TAG, "Restored %s after an interrupted save: %s", base,
                         r == 0 ? "ok" : "failed");
                if (r != 0)
                {
                    return;
                }
            }
            else
            {
                esp_err_t err = remove_tree(path);
                ESP_LOGW(TAG, "Removed %s left by an interrupted save: %s", path,
                         err == ESP_OK ? "ok" : "failed");
                if (err != ESP_OK)
                {
                    return;
                }
            }
        }
    }
}

esp_err_t contacts_save_friend(const char *uuid, const char *json, size_t json_len,
                               const uint8_t *avatar, size_t avatar_len)
{
    char dir[PATH_LEN], staged[PATH_LEN], old[PATH_LEN], json_path[PATH_LEN], avatar_path[PATH_LEN];
    if (!make_path(dir, "%s/%s", FRIENDS_DIR, uuid)
        || !make_path(staged, "%s/%s" STAGING_NEW, FRIENDS_DIR, uuid)
        || !make_path(old, "%s/%s" STAGING_OLD, FRIENDS_DIR, uuid)
        || !make_path(json_path, "%s/profile.json", staged)
        || !make_path(avatar_path, "%s/avatar.bin", staged))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = ensure_dir(FRIENDS_DIR);
    if (err == ESP_OK)
    {
        contacts_recover_friends();  // no stale .new or .old in the way
        err = ensure_dir(staged);
    }
    if (err == ESP_OK)
    {
        err = write_file(json_path, json, json_len);
    }
    if (err == ESP_OK && avatar_len)
    {
        err = write_file(avatar_path, avatar, avatar_len);
    }
    if (err != ESP_OK)
    {
        remove_tree(staged);
        return err;
    }

    bool replacing = contacts_path_exists(dir);
    if (replacing && rename(dir, old) != 0)
    {
        ESP_LOGE(TAG, "Move %s aside failed (errno %d)", dir, errno);
        remove_tree(staged);
        return ESP_FAIL;
    }
    if (rename(staged, dir) != 0)
    {
        ESP_LOGE(TAG, "Move %s in failed (errno %d)", dir, errno);
        if (replacing)
        {
            rename(old, dir);
        }
        remove_tree(staged);
        return ESP_FAIL;
    }
    if (replacing && remove_tree(old) != ESP_OK)
    {
        ESP_LOGW(TAG, "Remove %s failed; the next scan retries", old);
    }
    ESP_LOGI(TAG, "%s friend %s", replacing ? "Updated" : "Saved", uuid);
    return ESP_OK;
}

// ── Delete and archive ───────────────────────────────────

esp_err_t contacts_delete_friend(int index)
{
    if (contacts_friends_fake)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (index < 0 || index >= contacts_friend_count)
    {
        return ESP_ERR_INVALID_ARG;
    }
    const char *uuid = contacts_friend_uuids[index];
    esp_err_t   err = ensure_dir(FRIENDS_DELETED_DIR);
    if (err != ESP_OK)
    {
        return err;
    }

    char src[PATH_LEN], dst[PATH_LEN];
    snprintf(src, sizeof(src), "%s/%s", FRIENDS_DIR, uuid);
    snprintf(dst, sizeof(dst), "%s/%s", FRIENDS_DELETED_DIR, uuid);
    for (int n = 1; contacts_path_exists(dst); n++)
    {
        if (n > SUFFIX_MAX)
        {
            ESP_LOGE(TAG, "Delete %s: no free name in " FRIENDS_DELETED_DIR, uuid);
            return ESP_ERR_NO_MEM;
        }
        snprintf(dst, sizeof(dst), "%s/%s_%d", FRIENDS_DELETED_DIR, uuid, n);
    }
    if (rename(src, dst) != 0)
    {
        ESP_LOGE(TAG, "Delete: move %s failed (errno %d)", src, errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Deleted %s to %s", src, dst);
    contacts_list_friends();
    return ESP_OK;
}

esp_err_t contacts_archive(int *suffix)
{
    char self_dst[PATH_LEN], friends_dst[PATH_LEN];
    int  n = 1;
    for (; n <= SUFFIX_MAX; n++)
    {
        snprintf(self_dst, sizeof(self_dst), "%s_%d", SELF_DIR, n);
        snprintf(friends_dst, sizeof(friends_dst), "%s_%d", FRIENDS_DIR, n);
        if (!contacts_path_exists(self_dst) && !contacts_path_exists(friends_dst))
        {
            break;
        }
    }
    if (n > SUFFIX_MAX)
    {
        ESP_LOGE(TAG, "Archive: no free suffix");
        return ESP_ERR_NO_MEM;
    }
    // A reset between the two moves leaves self archived and the friends in
    // place: the next boot provisions a new profile that keeps the old
    // friends. Nothing is lost either way.
    if (contacts_path_exists(SELF_DIR) && rename(SELF_DIR, self_dst) != 0)
    {
        ESP_LOGE(TAG, "Archive: move " SELF_DIR " failed (errno %d)", errno);
        return ESP_FAIL;
    }
    if (contacts_path_exists(FRIENDS_DIR) && rename(FRIENDS_DIR, friends_dst) != 0)
    {
        ESP_LOGE(TAG, "Archive: move " FRIENDS_DIR " failed (errno %d)", errno);
        return ESP_FAIL;
    }
    esp_err_t err = ensure_dir(SELF_DIR);
    if (err == ESP_OK)
    {
        err = ensure_dir(FRIENDS_DIR);
    }
    if (err != ESP_OK)
    {
        return err;
    }
    ESP_LOGI(TAG, "Archived to %s, %s", self_dst, friends_dst);
    contacts_list_friends();
    if (suffix)
    {
        *suffix = n;
    }
    return ESP_OK;
}

// ── Own uuid ─────────────────────────────────────────────

esp_err_t contacts_ensure_self_uuid(const char *prefer, char out[CONTACT_UUID_LEN])
{
    if (out)
    {
        out[0] = '\0';
    }
    contacts_recover_file(SELF_JSON);
    static char json[PROFILE_JSON_MAX + 1];
    if (contacts_read_text(SELF_JSON, json, sizeof(json)) < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // As the old firmware: the preferred id, else the file's, else a new one.
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "uuid");
    const char  *cur_s = cJSON_IsString(cur) ? cur->valuestring : NULL;
    char         id[CONTACT_UUID_LEN];
    if (!contacts_uuid_normalize(prefer, id) && !contacts_uuid_normalize(cur_s, id))
    {
        uint8_t r[8];
        esp_fill_random(r, sizeof(r));
        snprintf(id, sizeof(id), "%02X%02X%02X%02X%02X%02X%02X%02X", r[0], r[1], r[2], r[3], r[4],
                 r[5], r[6], r[7]);
    }

    esp_err_t err = ESP_OK;
    if (!cur_s || strcmp(cur_s, id) != 0)
    {
        const bool had = cur_s != NULL;  // cur_s goes with the replaced item
        cJSON *value = cJSON_CreateString(id);
        if (cJSON_HasObjectItem(root, "uuid"))
        {
            cJSON_ReplaceItemInObjectCaseSensitive(root, "uuid", value);
        }
        else
        {
            cJSON_AddItemToObject(root, "uuid", value);
        }
        char *text = cJSON_PrintUnformatted(root);
        if (text && strlen(text) <= PROFILE_JSON_MAX)
        {
            err = contacts_write_atomic(SELF_JSON, text, strlen(text));
        }
        else
        {
            err = ESP_ERR_NO_MEM;
        }
        cJSON_free(text);
        ESP_LOGI(TAG, "Self uuid %s %s: %s", had ? "changed to" : "set to", id,
                 esp_err_to_name(err));
    }
    cJSON_Delete(root);
    if (err == ESP_OK && out)
    {
        memcpy(out, id, CONTACT_UUID_LEN);
    }
    return err;
}
