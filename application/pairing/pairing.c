// Pairing: ACOM until contact, then one BLE transfer each way. Runs in the
// driver tasks (ACOM, BLE) and an esp_timer for the progress; the caller
// hears about it through its callback and saves the result in its own task.
//
// pairing_stop() can race a driver callback that is about to start the next
// step. Both take the lock: a step starts only while the pairing is active,
// and stop clears that flag before it stops the drivers.

#include "pairing.h"

#include <string.h>

#include "acom.h"
#include "ble_link.h"
#include "contacts.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "pairing";

#define PROGRESS_US (100 * 1000)

static SemaphoreHandle_t  lock;
static esp_timer_handle_t progress_timer;
static bool               active;
static pairing_cb_t       notify;

static uint8_t own[6];
static uint8_t own_type;
static uint8_t peer[6];
static uint8_t peer_type;

static uint8_t        pack[CONTACTS_PACK_MAX];
static size_t         pack_len;
static const uint8_t *received;  // ble_link's memory, valid until its next start
static size_t         received_len;
static int            last_percent;

static void on_ble(bool success, const uint8_t *data, size_t len);

static void on_acom(acom_result_t result, const uint8_t addr[6], uint8_t type)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!active)
    {
        xSemaphoreGive(lock);
        return;
    }
    if (result == ACOM_TIMEOUT)
    {
        // Nobody there yet: keep listening, as the old firmware. The caller's
        // own timeout ends the wait.
        esp_err_t err = acom_start(own, own_type, on_acom);
        xSemaphoreGive(lock);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "ACOM restart failed: %s", esp_err_to_name(err));
        }
        return;
    }

    memcpy(peer, addr, sizeof(peer));
    peer_type = type;
    // As the old firmware: the lower address is the server.
    ble_link_role_t role = memcmp(own, peer, sizeof(own)) < 0 ? BLE_LINK_SERVER : BLE_LINK_CLIENT;
    ESP_LOGI(TAG, "Contact (%s), BLE as %s, sending %u bytes",
             result == ACOM_DONE_ACKED ? "acked" : "no ack",
             role == BLE_LINK_SERVER ? "server" : "client", (unsigned)pack_len);
    last_percent = -2;  // nothing reported yet
    esp_err_t err = ble_link_start(peer, peer_type, role, pack, pack_len, on_ble);
    if (err == ESP_OK)
    {
        esp_timer_start_periodic(progress_timer, PROGRESS_US);
    }
    xSemaphoreGive(lock);

    notify(PAIRING_EV_CONTACT, 1);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(err));
        notify(PAIRING_EV_DONE, 0);
    }
}

static void on_progress(void *arg)
{
    uint16_t        done = 0, total = 0;
    ble_link_prog_t p = ble_link_progress(&done, &total);
    int             percent;
    if (p == BLE_LINK_PROG_ACTIVE && total > 0)
    {
        percent = done >= total ? 100 : (int)((uint32_t)done * 100 / total);
    }
    else if (p == BLE_LINK_PROG_INDETERMINATE)
    {
        percent = -1;
    }
    else
    {
        return;  // still connecting
    }
    if (percent != last_percent)
    {
        last_percent = percent;
        notify(PAIRING_EV_PROGRESS, percent);
    }
}

static void on_ble(bool success, const uint8_t *data, size_t len)
{
    esp_timer_stop(progress_timer);
    xSemaphoreTake(lock, portMAX_DELAY);
    bool report = active;
    if (report)
    {
        received = success ? data : NULL;
        received_len = success ? len : 0;
    }
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "BLE %s, %u bytes", success ? "done" : "failed", (unsigned)len);
    if (report)
    {
        notify(PAIRING_EV_DONE, success && len > 0);
    }
}

esp_err_t pairing_init(void)
{
    lock = xSemaphoreCreateMutex();
    const esp_timer_create_args_t args = {.callback = on_progress, .name = "pairing"};
    if (!lock || esp_timer_create(&args, &progress_timer) != ESP_OK)
    {
        return ESP_ERR_NO_MEM;
    }
    int64_t   t0 = esp_timer_get_time();
    esp_err_t err = ble_link_init();
    if (err == ESP_OK)
    {
        err = ble_link_own_addr(own, &own_type);
    }
    if (err == ESP_OK)
    {
        err = acom_init();
    }
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Ready in %lld ms, own %02X:%02X:%02X:%02X:%02X:%02X",
                 (esp_timer_get_time() - t0) / 1000, own[0], own[1], own[2], own[3], own[4],
                 own[5]);
    }
    return err;
}

esp_err_t pairing_start(pairing_cb_t cb)
{
    if (!lock || !cb)
    {
        return ESP_FAIL;  // not initialised; INVALID_STATE means no profile
    }
    pairing_stop();  // anything left over

    esp_err_t err = contacts_pack_self(pack, sizeof(pack), &pack_len);
    if (err != ESP_OK)
    {
        return err == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_STATE : err;
    }

    xSemaphoreTake(lock, portMAX_DELAY);
    notify = cb;
    received = NULL;
    received_len = 0;
    active = true;
    err = acom_start(own, own_type, on_acom);
    if (err != ESP_OK)
    {
        active = false;
    }
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "Waiting for contact");
    return err;
}

void pairing_stop(void)
{
    if (!lock)
    {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    bool was_active = active;
    active = false;
    xSemaphoreGive(lock);
    if (!was_active)
    {
        return;
    }
    esp_timer_stop(progress_timer);
    acom_stop();
    ble_link_stop();
    ESP_LOGI(TAG, "Stopped");
}

int pairing_save(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    const uint8_t *data = received;
    size_t         len = received_len;
    received = NULL;
    xSemaphoreGive(lock);
    if (!data)
    {
        return -1;
    }
    char      uuid[CONTACT_UUID_LEN];
    esp_err_t err = contacts_unpack_friend(data, len, uuid);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Peer profile not saved: %s", esp_err_to_name(err));
        return -1;
    }
    return contacts_find_friend(uuid);
}
