// M9: ACOM, then a BLE transfer of a test profile in the old packing format.
#include "hw_tests_priv.h"

#if HW_TEST == HW_TEST_BLE

#include <string.h>

#include "acom.h"
#include "ble_link.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "hw_test";

static SemaphoreHandle_t step_done;
static uint8_t           peer[6];
static uint8_t           peer_type;
static bool              peer_ok;
static bool              ble_ok;

static void on_acom(acom_result_t result, const uint8_t addr[6], uint8_t type)
{
    peer_ok = result != ACOM_TIMEOUT;
    if (peer_ok)
    {
        memcpy(peer, addr, 6);
        peer_type = type;
    }
    xSemaphoreGive(step_done);
}

// The old packing format: [json length, u32 BE][json][avatar, 2bpp].
static void on_ble(bool success, const uint8_t *data, size_t len)
{
    ble_ok = success;
    if (success && len >= 4)
    {
        uint32_t json_len = (uint32_t)data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
        if (json_len + 4 <= len)
        {
            ESP_LOGI(TAG, "BLE: peer profile %.*s, avatar %u bytes", (int)json_len, data + 4,
                     (unsigned)(len - 4 - json_len));
        }
        else
        {
            ESP_LOGW(TAG, "BLE: %u bytes, not in packing format", (unsigned)len);
        }
    }
    xSemaphoreGive(step_done);
}

// A test profile the old firmware accepts: uuid of 16 hex digits, 128x128
// avatar in 2bpp (4096 bytes) with a diagonal gray gradient.
static size_t build_test_profile(uint8_t *out, size_t out_len, const uint8_t mac[6])
{
    char   json[256];
    size_t json_len = (size_t)snprintf(
        json, sizeof(json),
        "{\"name\":\"M9 Test\",\"species\":\"Test\",\"from\":\"ESP-IDF\",\"links\":[],"
        "\"avatar_res\":128,\"avatar_mode\":1,\"uuid\":\"0000%02X%02X%02X%02X%02X%02X\"}",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const size_t avatar_len = 128 * 128 / 4;
    if (4 + json_len + avatar_len > out_len)
    {
        return 0;
    }
    out[0] = 0;
    out[1] = 0;
    out[2] = json_len >> 8;
    out[3] = json_len & 0xFF;
    memcpy(out + 4, json, json_len);
    uint8_t *avatar = out + 4 + json_len;
    for (size_t i = 0; i < avatar_len; i++)
    {
        size_t  x = i % 128;
        size_t  page = i / 128;
        uint8_t gray = ((x + page * 4) / 64) & 0x3;
        avatar[i] = gray << 6 | gray << 4 | gray << 2 | gray;
    }
    return 4 + json_len + avatar_len;
}

void test_ble_run(void)
{
    step_done = xSemaphoreCreateBinary();
    esp_err_t err = step_done ? ble_link_init() : ESP_ERR_NO_MEM;
    uint8_t   own[6];
    uint8_t   own_type = 0;
    if (err == ESP_OK)
    {
        err = ble_link_own_addr(own, &own_type);
    }
    if (err == ESP_OK)
    {
        err = acom_init();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE setup failed: %s", esp_err_to_name(err));
        return;
    }

    // M8 sent esp_read_mac(ESP_MAC_BT); it must be the address NimBLE uses.
    uint8_t bt_mac[6];
    esp_read_mac(bt_mac, ESP_MAC_BT);
    ESP_LOGI(TAG, "BLE: own %02X:%02X:%02X:%02X:%02X:%02X type %u, BT MAC %s", own[0], own[1],
             own[2], own[3], own[4], own[5], own_type,
             memcmp(own, bt_mac, 6) == 0 ? "matches" : "DIFFERS");

    static uint8_t profile[BLE_LINK_MAX_LEN];
    size_t         profile_len = build_test_profile(profile, sizeof(profile), own);

    for (int round = 1;; round++)
    {
        ESP_LOGI(TAG, "BLE: round %d, ACOM", round);
        if (acom_start(own, own_type, on_acom) != ESP_OK)
        {
            return;
        }
        xSemaphoreTake(step_done, portMAX_DELAY);
        if (!peer_ok)
        {
            continue;
        }

        // As the old firmware: the lower address is the server.
        ble_link_role_t role = memcmp(own, peer, 6) < 0 ? BLE_LINK_SERVER : BLE_LINK_CLIENT;
        ESP_LOGI(TAG, "BLE: %s, sending %u bytes", role == BLE_LINK_SERVER ? "server" : "client",
                 (unsigned)profile_len);
        TickType_t t0 = xTaskGetTickCount();
        err = ble_link_start(peer, peer_type, role, profile, profile_len, on_ble);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(err));
            continue;
        }
        // The old firmware gave up after BLE_CONNECT_TIMEOUT_MS; 20 s here.
        if (xSemaphoreTake(step_done, pdMS_TO_TICKS(20000)) != pdTRUE)
        {
            ESP_LOGW(TAG, "BLE: no result in 20 s, stopping");
            ble_link_stop();
            continue;
        }
        ESP_LOGI(TAG, "BLE: %s in %lu ms", ble_ok ? "OK" : "FAILED",
                 (unsigned long)pdTICKS_TO_MS(xTaskGetTickCount() - t0));
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

#endif
