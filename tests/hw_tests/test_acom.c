// M8: repeated ACOM exchanges with our BT MAC.
#include "hw_tests_priv.h"

#if HW_TEST == HW_TEST_ACOM

#include "acom.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "hw_test";

static SemaphoreHandle_t acom_done;

static void on_acom_done(acom_result_t result, const uint8_t peer[6], uint8_t peer_type)
{
    // acom logs the peer itself; just let the test loop go on.
    xSemaphoreGive(acom_done);
}

// Our address as the old firmware sends it: the BT MAC, MSB first, type 0
// (public). M9 checks that NimBLE advertises the same.
void test_acom_run(void)
{
    acom_done = xSemaphoreCreateBinary();
    esp_err_t err = acom_done ? acom_init() : ESP_ERR_NO_MEM;
    uint8_t   mac[6];
    if (err == ESP_OK)
    {
        err = esp_read_mac(mac, ESP_MAC_BT);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "ACOM setup failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "ACOM: own BT MAC %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);

    for (int round = 1;; round++)
    {
        ESP_LOGI(TAG, "ACOM: exchange %d, 8 s", round);
        err = acom_start(mac, 0, on_acom_done);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "ACOM start failed: %s", esp_err_to_name(err));
            return;
        }
        xSemaphoreTake(acom_done, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

#endif
