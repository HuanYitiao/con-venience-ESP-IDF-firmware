#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "contacts.h"
#include "esp_err.h"
#include "sdcard.h"

#define SELF_DIR SDCARD_MOUNT_POINT "/self_profile"
#define SELF_JSON SELF_DIR "/profile.json"
#define SELF_AVATAR SELF_DIR "/avatar.bin"
#define FRIENDS_DIR SDCARD_MOUNT_POINT "/friends_profiles"
#define FRIENDS_DELETED_DIR SDCARD_MOUNT_POINT "/friends_deleted"
#define PATH_LEN 128
// Larger than any real profile (about 200 bytes); a bound for a hostile file.
#define PROFILE_JSON_MAX 2048
// Highest suffix tried for deleted friends and archives, as the old firmware.
#define SUFFIX_MAX 999

// A friend directory being written, and the one it replaces, while a save is
// in progress: FRIENDS_DIR/<uuid>.new, FRIENDS_DIR/<uuid>.old.
#define STAGING_NEW ".new"
#define STAGING_OLD ".old"

// ── contacts.c ───────────────────────────────────────────

// s is exactly 16 hex digits; out gets it in upper case (as the old
// firmware writes uuids). out may be NULL to only check.
bool contacts_uuid_normalize(const char *s, char out[CONTACT_UUID_LEN]);

void contacts_copy_utf8(char *dst, size_t size, const char *src);

// Fills out from profile JSON text (out zeroed by the caller). A uuid that
// is not 16 hex digits leaves out->uuid empty.
esp_err_t contacts_parse_profile(const char *json, contact_t *out);

// Reads a file into buf (at most size - 1 bytes, NUL-terminated). Returns
// the byte count, or -1 if it does not open.
int contacts_read_text(const char *path, char *buf, size_t size);

// The friend list, filled by contacts_list_friends().
extern char contacts_friend_uuids[CONTACTS_FRIENDS_MAX][CONTACT_UUID_LEN];
extern char contacts_friend_names[CONTACTS_FRIENDS_MAX][CONTACT_NAME_LEN];
extern int  contacts_friend_count;
extern bool contacts_friends_fake;

// ── contacts_store.c ─────────────────────────────────────

bool contacts_path_exists(const char *path);

// Writes path through path.tmp: the old file stays whole until the new one
// is complete on the card.
esp_err_t contacts_write_atomic(const char *path, const void *data, size_t len);

// After a reset during contacts_write_atomic(): path is missing but its
// complete .tmp is there. Puts it in place.
void contacts_recover_file(const char *path);

// Undoes what a reset in the middle of a friend save left behind in
// FRIENDS_DIR. Called before each scan.
void contacts_recover_friends(void);

// Writes FRIENDS_DIR/<uuid> as a whole: the old directory, if any, stays
// until the new one is complete. avatar may be NULL with avatar_len 0.
esp_err_t contacts_save_friend(const char *uuid, const char *json, size_t json_len,
                               const uint8_t *avatar, size_t avatar_len);

// ── contacts_fake.c ──────────────────────────────────────

// Made-up friend index of the fake list.
void contacts_fake_friend(int index, contact_t *out);
