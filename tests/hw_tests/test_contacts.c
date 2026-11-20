// A2: contacts on a real card. Reads every friend and the own profile back,
// packs the own profile, then works on a test friend only (uuid
// A2A2A2A2A2A2A2A2): save, replace, a reset in the middle of a save, bad
// packages, delete, and an archive that is undone at the end. The card is
// left as it was, apart from a /friends_deleted directory if there was none.
#include "hw_tests_priv.h"

#if HW_TEST == HW_TEST_CONTACTS

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "contacts.h"
#include "esp_log.h"
#include "sdcard.h"

static const char *TAG = "hw_test";

#define TEST_UUID "A2A2A2A2A2A2A2A2"
#define TEST_RES 64
#define FRIENDS SDCARD_MOUNT_POINT "/friends_profiles"
#define DELETED SDCARD_MOUNT_POINT "/friends_deleted"

static int failures;

#define CHECK(cond, ...)                         \
    do                                           \
    {                                            \
        if (cond)                                \
        {                                        \
            ESP_LOGI(TAG, "PASS " __VA_ARGS__);  \
        }                                        \
        else                                     \
        {                                        \
            ESP_LOGE(TAG, "FAIL " __VA_ARGS__);  \
            failures++;                          \
        }                                        \
    } while (0)

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f)
    {
        fputs(text, f);
        fclose(f);
    }
}

// Removes a directory that holds only files.
static void remove_flat_dir(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
    {
        return;
    }
    char           names[8][64];
    int            n = 0;
    struct dirent *e;
    while (n < 8 && (e = readdir(dir)) != NULL)
    {
        if (strlen(e->d_name) < sizeof(names[0]))
        {
            strcpy(names[n++], e->d_name);
        }
    }
    closedir(dir);
    for (int i = 0; i < n; i++)
    {
        char p[160];
        snprintf(p, sizeof(p), "%s/%s", path, names[i]);
        unlink(p);
    }
    rmdir(path);
}

// The test friend's copies in /friends_deleted: <uuid> and <uuid>_N.
static void remove_deleted_copies(void)
{
    for (int pass = 0; pass < 8; pass++)
    {
        DIR *dir = opendir(DELETED);
        if (!dir)
        {
            return;
        }
        char           name[64] = "";
        struct dirent *e;
        while ((e = readdir(dir)) != NULL)
        {
            if (strncmp(e->d_name, TEST_UUID, strlen(TEST_UUID)) == 0
                && strlen(e->d_name) < sizeof(name))
            {
                strcpy(name, e->d_name);
                break;
            }
        }
        closedir(dir);
        if (!name[0])
        {
            return;
        }
        char path[160];
        snprintf(path, sizeof(path), DELETED "/%s", name);
        remove_flat_dir(path);
    }
}

static uint8_t pkg[CONTACTS_PACK_MAX];

// A package as an old-firmware peer sends it, with a gradient avatar.
static size_t make_package(const char *uuid, const char *name, size_t avatar_len)
{
    char json[256];
    int  n = snprintf(json, sizeof(json),
                      "{\"uuid\":\"%s\",\"name\":\"%s\",\"species\":\"Test\",\"from\":\"Bench\","
                       "\"links\":[{\"platform\":\"x\",\"url\":\"https://x.com/a2\"}],"
                       "\"avatar_res\":%d,\"avatar_mode\":1}",
                      uuid, name, TEST_RES);
    pkg[0] = 0;
    pkg[1] = 0;
    pkg[2] = (uint8_t)(n >> 8);
    pkg[3] = (uint8_t)n;
    memcpy(pkg + 4, json, n);
    for (size_t i = 0; i < avatar_len; i++)
    {
        pkg[4 + n + i] = (uint8_t)(((i % TEST_RES) / 16) * 0x55);
    }
    return 4 + n + avatar_len;
}

// Every friend on the card loads, with its avatar.
static int read_all(void)
{
    static contact_t c;
    int              count = contacts_list_friends();
    int              bad = 0;
    for (int i = 0; i < count; i++)
    {
        if (contacts_load_friend(i, &c) != ESP_OK)
        {
            ESP_LOGE(TAG, "Friend %d (%s) does not load", i, contacts_friend_name(i));
            bad++;
            continue;
        }
        ESP_LOGI(TAG, "Friend %d: %s, %s, %u links, avatar %u px", i, c.name, c.uuid,
                 c.link_count, c.avatar_res);
        if (!c.avatar_res)
        {
            bad++;
        }
    }
    CHECK(bad == 0, "all %d friends read back with avatars (%d bad)", count, bad);
    return count;
}

void test_contacts_run(void)
{
    if (!sdcard_present() || sdcard_mount() != ESP_OK)
    {
        ESP_LOGE(TAG, "Contacts: no card or mount failed");
        return;
    }
    static contact_t c;
    const size_t     avatar_len = TEST_RES * TEST_RES / 4;

    // 1. The card as it is.
    char      self_uuid[CONTACT_UUID_LEN];
    esp_err_t err = contacts_ensure_self_uuid(NULL, self_uuid);
    CHECK(err == ESP_OK, "own uuid %s (%s)", self_uuid, esp_err_to_name(err));
    err = contacts_load_self(&c);
    const bool had_self = err == ESP_OK;
    CHECK(err == ESP_OK && c.avatar_res, "own profile %s, avatar %u px", c.name, c.avatar_res);
    // A run cut short (a reset while flashing) leaves the test friend behind.
    bool deleted_existed = exists(DELETED);
    contacts_list_friends();
    int left = contacts_find_friend(TEST_UUID);
    if (left >= 0)
    {
        ESP_LOGW(TAG, "Test friend left from an earlier run, removing it");
        contacts_delete_friend(left);
        remove_deleted_copies();
    }
    const int count = read_all();

    size_t len = 0;
    err = contacts_pack_self(pkg, sizeof(pkg), &len);
    uint32_t json_len = ((uint32_t)pkg[2] << 8) | pkg[3];
    CHECK(err == ESP_OK && len == 4 + json_len + (size_t)c.avatar_res * c.avatar_res / 4,
          "pack self: %u bytes, JSON %lu (%s)", (unsigned)len, (unsigned long)json_len,
          esp_err_to_name(err));

    // 2. Save a new friend, lower case uuid as some peers may send it.
    char uuid[CONTACT_UUID_LEN];
    len = make_package("a2a2a2a2a2a2a2a2", "A2 Test", avatar_len);
    err = contacts_unpack_friend(pkg, len, uuid);
    int i = contacts_find_friend(TEST_UUID);
    CHECK(err == ESP_OK && strcmp(uuid, TEST_UUID) == 0 && contacts_list_friends() == count + 1,
          "unpack new friend: %s, %d friends", esp_err_to_name(err), contacts_list_friends());
    i = contacts_find_friend(TEST_UUID);
    err = contacts_load_friend(i, &c);
    CHECK(err == ESP_OK && strcmp(c.name, "A2 Test") == 0 && c.avatar_res == TEST_RES
              && c.avatar[TEST_RES - 1] == 0xFF && c.link_count == 1,
          "test friend reads back: %s, avatar %u px", c.name, c.avatar_res);

    // 3. The same uuid again replaces it.
    len = make_package(TEST_UUID, "A2 Again", avatar_len);
    err = contacts_unpack_friend(pkg, len, NULL);
    i = contacts_find_friend(TEST_UUID);
    contacts_load_friend(i, &c);
    CHECK(err == ESP_OK && contacts_list_friends() == count + 1 && strcmp(c.name, "A2 Again") == 0,
          "unpack again replaces: %s, %d friends", c.name, contacts_list_friends());
    CHECK(!exists(FRIENDS "/" TEST_UUID ".new") && !exists(FRIENDS "/" TEST_UUID ".old"),
          "no staging left after a save");

    // 4. A reset between the two moves of a save: only .old and a cut-short
    //    .new are on the card. The scan brings the friend back.
    rename(FRIENDS "/" TEST_UUID, FRIENDS "/" TEST_UUID ".old");
    mkdir(FRIENDS "/" TEST_UUID ".new", 0777);
    write_text(FRIENDS "/" TEST_UUID ".new/profile.json", "{\"uuid\":\"A2A2");
    contacts_list_friends();
    i = contacts_find_friend(TEST_UUID);
    CHECK(i >= 0 && contacts_load_friend(i, &c) == ESP_OK && strcmp(c.name, "A2 Again") == 0
              && !exists(FRIENDS "/" TEST_UUID ".new") && !exists(FRIENDS "/" TEST_UUID ".old"),
          "interrupted save recovered: %s", i >= 0 ? c.name : "missing");

    // 5. Bad packages are refused and write nothing.
    len = make_package("../../selfprofil", "Bad", avatar_len);
    CHECK(contacts_unpack_friend(pkg, len, NULL) != ESP_OK, "path in uuid refused");
    len = make_package("B2B2B2B2B2B2B2B2", "Short", avatar_len - 1);
    CHECK(contacts_unpack_friend(pkg, len, NULL) != ESP_OK, "short avatar refused");
    pkg[2] = 0x7F;
    CHECK(contacts_unpack_friend(pkg, len, NULL) != ESP_OK, "JSON length past the end refused");
    CHECK(contacts_unpack_friend(pkg, 3, NULL) != ESP_OK, "no header refused");
    CHECK(contacts_list_friends() == count + 1 && !exists(FRIENDS "/B2B2B2B2B2B2B2B2"),
          "nothing written by bad packages");

    // 6. Delete moves it to /friends_deleted.
    err = contacts_delete_friend(contacts_find_friend(TEST_UUID));
    CHECK(err == ESP_OK && contacts_find_friend(TEST_UUID) < 0 && contacts_list_friends() == count
              && exists(DELETED "/" TEST_UUID),
          "delete moved it aside: %s", esp_err_to_name(err));
    remove_deleted_copies();
    if (!deleted_existed)
    {
        rmdir(DELETED);
    }

    // 7. Archive, then undo it by hand as a user would on a PC.
    int n = 0;
    err = contacts_archive(&n);
    char self_n[64], friends_n[64];
    snprintf(self_n, sizeof(self_n), SDCARD_MOUNT_POINT "/self_profile_%d", n);
    snprintf(friends_n, sizeof(friends_n), FRIENDS "_%d", n);
    CHECK(err == ESP_OK && contacts_list_friends() == 0
              && contacts_load_self(&c) == ESP_ERR_NOT_FOUND && exists(self_n),
          "archive to _%d: no profile, no friends", n);
    if (err == ESP_OK)
    {
        rmdir(SDCARD_MOUNT_POINT "/self_profile");
        rmdir(FRIENDS);
        bool ok = rename(self_n, SDCARD_MOUNT_POINT "/self_profile") == 0;
        ok = (!exists(friends_n) || rename(friends_n, FRIENDS) == 0) && ok;
        CHECK(ok, "archive undone");
    }
    // Compared with the start, so a card without an own profile passes too.
    CHECK((contacts_load_self(&c) == ESP_OK) == had_self && contacts_list_friends() == count,
          "card back as it was: %s, %d friends", had_self ? c.name : "no own profile",
          contacts_list_friends());

    if (failures)
    {
        ESP_LOGE(TAG, "Contacts: %d checks FAILED", failures);
    }
    else
    {
        ESP_LOGI(TAG, "Contacts: all checks passed");
    }
}

#endif
