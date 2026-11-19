#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Contacts on the SD card, in the old firmware's layout:
//   /self_profile/profile.json, /self_profile/avatar.bin
//   /friends_profiles/<uuid>/profile.json, avatar.bin
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

// Our own profile. Needs the SD card mounted. ESP_ERR_NOT_FOUND without a
// profile.json, ESP_ERR_INVALID_RESPONSE if it does not parse. A missing or
// short avatar.bin only leaves avatar_res at 0.
esp_err_t contacts_load_self(contact_t *out);

// A made-up contact for UI work without an SD card.
void contacts_fake(contact_t *out);
