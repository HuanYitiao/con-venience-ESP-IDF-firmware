#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Contacts on the SD card, in the old firmware's layout:
//   /self_profile/profile.json, /self_profile/avatar.bin
//   /friends_profiles/<uuid>/profile.json, avatar.bin
//   /friends_deleted/<uuid>[_N]/                   deleted friends
//   /self_profile_N/, /friends_profiles_N/          archived profiles
// Every write survives a reset at any point; see contacts_store.c.
// All calls need the SD card mounted and come from one task.
// Field sizes as in the old firmware (bytes, including the terminator; text
// is UTF-8 and is cut on a character boundary).

#define CONTACT_UUID_LEN 17  // 16 hex digits
#define CONTACT_NAME_LEN 32
#define CONTACT_SPECIES_LEN 64
#define CONTACT_FROM_LEN 64
#define CONTACT_PLATFORM_LEN 16
#define CONTACT_URL_LEN 128
#define CONTACT_LINKS_MAX 5
#define CONTACT_AVATAR_RES_MAX 128
// 2bpp in the ST75256's own layout: pages of 4 rows, one byte per column.
#define CONTACT_AVATAR_LEN_MAX (CONTACT_AVATAR_RES_MAX * CONTACT_AVATAR_RES_MAX / 4)

typedef struct
{
    char platform[CONTACT_PLATFORM_LEN];
    char url[CONTACT_URL_LEN];
} contact_link_t;

typedef struct
{
    char           uuid[CONTACT_UUID_LEN];
    char           name[CONTACT_NAME_LEN];
    char           species[CONTACT_SPECIES_LEN];
    char           from[CONTACT_FROM_LEN];
    contact_link_t links[CONTACT_LINKS_MAX];
    uint8_t        link_count;
    uint8_t        avatar_res;   // side length in pixels, 0 = no avatar
    uint8_t        avatar_mode;  // as in profile.json; the data is 2bpp either way
    uint8_t        avatar[CONTACT_AVATAR_LEN_MAX];
} contact_t;

// Our own profile. ESP_ERR_NOT_FOUND without a
// profile.json, ESP_ERR_INVALID_RESPONSE if it does not parse. A missing or
// short avatar.bin only leaves avatar_res at 0.
esp_err_t contacts_load_self(contact_t *out);

// Makes sure our profile.json has a uuid, as the old firmware: prefer if it
// is valid (the portal keeps the old id across a new upload), else the one
// in the file, else a new random one. Rewrites the file only on a change.
// out (may be NULL) gets the uuid. ESP_ERR_NOT_FOUND without a profile.json.
esp_err_t contacts_ensure_self_uuid(const char *prefer, char out[CONTACT_UUID_LEN]);

// Friends, in /friends_profiles/<uuid>/ (old firmware layout).
#define CONTACTS_FRIENDS_MAX 64  // as the old firmware

// Scans the friends' directories and keeps each uuid and name. Returns the
// number of friends. Saves, deletes and archives scan again by themselves,
// so indexes change after them.
int contacts_list_friends(void);

// Index in the last list, or -1.
int contacts_find_friend(const char *uuid);

// Size of the last list.
int contacts_count_friends(void);

// From the last list; "" out of range.
const char *contacts_friend_name(int index);

// Loads friend index of the last list.
esp_err_t contacts_load_friend(int index, contact_t *out);

// Moves friend index to /friends_deleted, where it can be restored by hand.
esp_err_t contacts_delete_friend(int index);

// Demo reset, as the old firmware: moves /self_profile and /friends_profiles
// to the first free _N (same N for both) and makes them again, empty, so the
// next boot provisions a new profile. suffix (may be NULL) gets N.
esp_err_t contacts_archive(int *suffix);

// ── Packing ──────────────────────────────────────────────
// The old firmware's BLE format: JSON length (4 bytes, big endian), our
// profile.json as on the card, avatar.bin.

#define CONTACTS_PACK_MAX 5000  // as the old PACKING_BUF_MAX

// ESP_ERR_INVALID_STATE without a uuid or avatar (the peer would reject it).
esp_err_t contacts_pack_self(uint8_t *out, size_t size, size_t *len);

// Checks a peer's package and saves it as a friend, replacing an earlier
// copy of the same uuid. uuid (may be NULL) gets the peer's.
esp_err_t contacts_unpack_friend(const uint8_t *data, size_t len, char uuid[CONTACT_UUID_LEN]);

// ── Without a card ───────────────────────────────────────

// A made-up contact for UI work without an SD card.
void contacts_fake(contact_t *out);

// Replaces the list with up to count made-up friends (accented names,
// varied avatars, some without). Returns the number. Writes refuse a fake
// list (ESP_ERR_INVALID_STATE).
int contacts_fake_friends(int count);
