// Packing, the old firmware's BLE format: the JSON length (4 bytes, big
// endian), profile.json as it is on the card, then avatar.bin as it is.
// The JSON goes across unchanged, so fields this firmware does not know
// (from a newer peer) are kept in the friend's copy.

#include <stdio.h>
#include <string.h>

#include "contacts.h"
#include "contacts_priv.h"
#include "esp_log.h"

static const char *TAG = "contacts";

#define HEADER_LEN 4

esp_err_t contacts_pack_self(uint8_t *out, size_t size, size_t *len)
{
    *len = 0;
    contacts_recover_file(SELF_JSON);
    contacts_recover_file(SELF_AVATAR);
    if (size < HEADER_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    // The receiver needs a uuid and an avatar of the size the JSON gives.
    static char text[PROFILE_JSON_MAX + 1];
    int         json_len = contacts_read_text(SELF_JSON, text, sizeof(text));
    if (json_len < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }
    if (json_len >= PROFILE_JSON_MAX)
    {
        ESP_LOGE(TAG, "Pack: profile.json too large");
        return ESP_ERR_INVALID_SIZE;
    }
    static contact_t self;  // 4.8 KB: not on the stack
    memset(&self, 0, sizeof(self));
    if (contacts_parse_profile(text, &self) != ESP_OK)
    {
        ESP_LOGE(TAG, "Pack: profile.json does not parse");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!self.uuid[0] || !self.avatar_res)
    {
        ESP_LOGE(TAG, "Pack: profile has no %s", self.uuid[0] ? "avatar" : "uuid");
        return ESP_ERR_INVALID_STATE;
    }
    size_t avatar_len = (size_t)self.avatar_res * self.avatar_res / 4;
    if (HEADER_LEN + (size_t)json_len + avatar_len > size)
    {
        ESP_LOGE(TAG, "Pack: %u bytes do not fit",
                 (unsigned)(HEADER_LEN + json_len + avatar_len));
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(SELF_AVATAR, "rb");
    if (!f)
    {
        ESP_LOGE(TAG, "Pack: no avatar.bin");
        return ESP_ERR_NOT_FOUND;
    }
    size_t got = fread(out + HEADER_LEN + json_len, 1, avatar_len, f);
    fclose(f);
    if (got != avatar_len)
    {
        ESP_LOGE(TAG, "Pack: avatar.bin has %u of %u bytes", (unsigned)got, (unsigned)avatar_len);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out + HEADER_LEN, text, json_len);
    out[0] = (uint8_t)(json_len >> 24);
    out[1] = (uint8_t)(json_len >> 16);
    out[2] = (uint8_t)(json_len >> 8);
    out[3] = (uint8_t)json_len;
    *len = HEADER_LEN + json_len + avatar_len;
    ESP_LOGI(TAG, "Packed self: %u bytes (JSON %d, avatar %u)", (unsigned)*len, json_len,
             (unsigned)avatar_len);
    return ESP_OK;
}

// The data comes from another device: every length is checked against the
// buffer, and the uuid against the path rules, before anything is written.
esp_err_t contacts_unpack_friend(const uint8_t *data, size_t len, char uuid[CONTACT_UUID_LEN])
{
    if (uuid)
    {
        uuid[0] = '\0';
    }
    if (contacts_friends_fake)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (len < HEADER_LEN)
    {
        ESP_LOGW(TAG, "Unpack: %u bytes, no header", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t json_len = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)
                        | ((uint32_t)data[2] << 8) | data[3];
    if (json_len == 0 || json_len > PROFILE_JSON_MAX || json_len > len - HEADER_LEN)
    {
        ESP_LOGW(TAG, "Unpack: JSON length %lu in %u bytes", (unsigned long)json_len,
                 (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }

    static char json[PROFILE_JSON_MAX + 1];
    memcpy(json, data + HEADER_LEN, json_len);
    json[json_len] = '\0';
    static contact_t peer;  // 4.8 KB: not on the stack
    memset(&peer, 0, sizeof(peer));
    if (contacts_parse_profile(json, &peer) != ESP_OK)
    {
        ESP_LOGW(TAG, "Unpack: JSON does not parse");
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (!peer.uuid[0])
    {
        ESP_LOGW(TAG, "Unpack: no valid uuid");
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint8_t *avatar = data + HEADER_LEN + json_len;
    size_t         avatar_len = len - HEADER_LEN - json_len;
    size_t         expected = (size_t)peer.avatar_res * peer.avatar_res / 4;
    if (avatar_len != expected)
    {
        ESP_LOGW(TAG, "Unpack: avatar %u bytes, %u px needs %u", (unsigned)avatar_len,
                 peer.avatar_res, (unsigned)expected);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = contacts_save_friend(peer.uuid, json, json_len, avatar, avatar_len);
    if (err != ESP_OK)
    {
        return err;
    }
    ESP_LOGI(TAG, "Unpacked %s (%s), avatar %u px", peer.name, peer.uuid, peer.avatar_res);
    contacts_list_friends();
    if (uuid)
    {
        memcpy(uuid, peer.uuid, CONTACT_UUID_LEN);
    }
    return ESP_OK;
}
