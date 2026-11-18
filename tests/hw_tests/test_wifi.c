// M10: soft AP with captive portal and a test page with a file upload.
#include "hw_tests_priv.h"

#if HW_TEST == HW_TEST_WIFI

#include <stdio.h>

#include "ble_link.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_ap.h"

static const char *TAG = "hw_test";

static volatile bool wifi_toggle;

static const char WIFI_TEST_PAGE[] =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>con-venience M10</title></head><body style=\"font-family:sans-serif\">"
    "<h2>con-venience M10</h2><p>The portal page opened.</p>"
    "<p><input type=\"file\" id=\"f\" accept=\"image/*\"></p><p id=\"r\"></p>"
    "<script>"
    "document.getElementById('f').onchange=function(){"
    "var f=this.files[0],r=document.getElementById('r');if(!f)return;"
    "r.textContent='Uploading '+f.size+' bytes...';"
    "fetch('/upload',{method:'POST',body:f}).then(function(x){return x.text()})"
    ".then(function(t){r.textContent=t}).catch(function(e){r.textContent='Failed: '+e})};"
    "</script></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    wifi_ap_touch();
    ESP_LOGI(TAG, "WiFi: page served");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, WIFI_TEST_PAGE, HTTPD_RESP_USE_STRLEN);
}

// Counts the body and throws it away: only whether the phone can upload
// from the portal window is tested here.
static esp_err_t upload_post(httpd_req_t *req)
{
    wifi_ap_touch();
    char   buf[512];
    size_t total = 0;
    int    n;
    while ((n = httpd_req_recv(req, buf, sizeof(buf))) > 0)
    {
        total += n;
    }
    if (n < 0 && n != HTTPD_SOCK_ERR_TIMEOUT)
    {
        ESP_LOGW(TAG, "WiFi: upload broke off after %u bytes", (unsigned)total);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "WiFi: upload %u of %u bytes", (unsigned)total, (unsigned)req->content_len);
    char msg[64];
    snprintf(msg, sizeof(msg), "Board got %u of %u bytes", (unsigned)total,
             (unsigned)req->content_len);
    return httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
}

static void wifi_up(void)
{
    size_t    before = esp_get_free_heap_size();
    esp_err_t err = wifi_ap_start();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return;
    }
    static const httpd_uri_t page = {.uri = "/", .method = HTTP_GET, .handler = page_get};
    static const httpd_uri_t upload = {.uri = "/upload", .method = HTTP_POST, .handler = upload_post};
    wifi_ap_register(&page);
    wifi_ap_register(&upload);
    ESP_LOGI(TAG, "WiFi: SSID %s, password %s, %s", wifi_ap_ssid(), wifi_ap_password(),
             wifi_ap_url());
    ESP_LOGI(TAG, "WiFi: free heap %u -> %u bytes", (unsigned)before,
             (unsigned)esp_get_free_heap_size());
}

void test_wifi_run(void)
{
    size_t    boot_heap = esp_get_free_heap_size();
    esp_err_t err = ble_link_init();  // as in the real flow: BLE is up first
    ESP_LOGI(TAG, "WiFi: BLE %s, free heap %u -> %u bytes", esp_err_to_name(err),
             (unsigned)boot_heap, (unsigned)esp_get_free_heap_size());

    bool up = true;
    wifi_up();
    while (true)
    {
        for (int i = 0; i < 100 && !wifi_toggle; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (wifi_toggle)
        {
            wifi_toggle = false;
            up = !up;
            if (up)
            {
                wifi_up();
            }
            else
            {
                wifi_ap_stop();
                ESP_LOGI(TAG, "WiFi: off, free heap %u bytes", (unsigned)esp_get_free_heap_size());
            }
            continue;
        }
        if (up)
        {
            ESP_LOGI(TAG, "WiFi: client %s, idle %lu s, free heap %u, min %u",
                     wifi_ap_client_connected() ? "yes" : "no",
                     (unsigned long)(wifi_ap_idle_ms() / 1000), (unsigned)esp_get_free_heap_size(),
                     (unsigned)esp_get_minimum_free_heap_size());
        }
    }
}


void test_wifi_on_button(button_id_t id, button_action_t action)
{
    if (id == BUTTON_PAIR && action == BUTTON_LONG_PRESS)
    {
        wifi_toggle = true;
    }
}

#endif
