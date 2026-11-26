#include "wifi_ap.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
// After esp_netif.h: the copied header uses esp_ip4_addr_t without including it.
#include "dns_server.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "wifi_ap";

#define WIFI_AP_SSID_PREFIX "con-venience-"
#define WIFI_AP_CHANNEL 1
// The portal is for the owner's phone only.
#define WIFI_AP_MAX_CLIENTS 1
#define WIFI_AP_URL "http://" WIFI_AP_IP "/"

// Phones open several connections at once; needs CONFIG_LWIP_MAX_SOCKETS 16.
#define HTTPD_MAX_SOCKETS 13
#define HTTPD_MAX_URI_HANDLERS 16
// Room for streaming files from SD later (portal).
#define HTTPD_STACK_SIZE 6144

// DHCP option 114 (RFC 8910) hands the portal URI to the client directly.
// The RFC wants an HTTPS captive-portal API there; the IDF example puts the
// page URL in it, as here. Set to 0 if phones turn out to dislike it.
#define WIFI_AP_DHCP_CAPTIVE_URI 1

static esp_netif_t        *ap_netif;
static httpd_handle_t      server;
static dns_server_handle_t dns;
static bool                running;

static char ssid[sizeof(WIFI_AP_SSID_PREFIX) + 4];
static char password[17];

static volatile bool       client_connected;
static volatile TickType_t last_activity;

void wifi_ap_touch(void)
{
    last_activity = xTaskGetTickCount();
}

uint32_t wifi_ap_idle_ms(void)
{
    return pdTICKS_TO_MS(xTaskGetTickCount() - last_activity);
}

bool wifi_ap_client_connected(void)
{
    return client_connected;
}

const char *wifi_ap_ssid(void)
{
    return ssid;
}

const char *wifi_ap_password(void)
{
    return password;
}

const char *wifi_ap_url(void)
{
    return WIFI_AP_URL;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED)
    {
        const wifi_event_ap_staconnected_t *e = data;
        client_connected = true;
        wifi_ap_touch();
        ESP_LOGI(TAG, "Client " MACSTR " joined", MAC2STR(e->mac));
    }
    else if (id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        const wifi_event_ap_stadisconnected_t *e = data;
        client_connected = false;
        ESP_LOGI(TAG, "Client " MACSTR " left, reason %u", MAC2STR(e->mac), e->reason);
    }
    else if (id == WIFI_EVENT_AP_WRONG_PASSWORD)
    {
        // Mostly a phone that saved the network from an earlier start: the
        // password is new every time.
        const wifi_event_ap_wrong_password_t *e = data;
        ESP_LOGW(TAG, "Client " MACSTR " sent a wrong password", MAC2STR(e->mac));
    }
}

// Every unknown path, including the phones' connectivity probes
// (/generate_204, /hotspot-detect.html, /connecttest.txt, ...), goes to the
// portal. iOS only treats it as a portal if the redirect has a body.
static esp_err_t redirect_to_portal(httpd_req_t *req, httpd_err_code_t err)
{
    wifi_ap_touch();
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", WIFI_AP_URL);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_send(req, "Redirect to the con-venience portal", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void make_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, sizeof(ssid), WIFI_AP_SSID_PREFIX "%02X%02X", mac[4], mac[5]);
}

// esp_fill_random() only gives true random numbers while Wi-Fi or BT has the
// RF on. Run Wi-Fi as an idle station for a moment (RF on, nothing on air
// from us, no AP yet), take the password, stop again. The AP then starts
// with it from the first beacon.
static esp_err_t make_password(void)
{
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK)
    {
        err = esp_wifi_start();
    }
    if (err != ESP_OK)
    {
        return err;
    }
    uint8_t r[8];
    esp_fill_random(r, sizeof(r));
    snprintf(password, sizeof(password), "%02X%02X%02X%02X%02X%02X%02X%02X", r[0], r[1], r[2],
             r[3], r[4], r[5], r[6], r[7]);
    return esp_wifi_stop();
}

#if WIFI_AP_DHCP_CAPTIVE_URI
static void set_dhcp_captive_uri(void)
{
    static const char uri[] = WIFI_AP_URL;
    esp_netif_dhcps_stop(ap_netif);
    esp_err_t err = esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                                           (void *)uri, strlen(uri));
    esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "DHCP option 114 not set: %s", esp_err_to_name(err));
    }
}
#endif

static esp_err_t start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = HTTPD_MAX_SOCKETS;
    cfg.max_uri_handlers = HTTPD_MAX_URI_HANDLERS;
    cfg.stack_size = HTTPD_STACK_SIZE;
    // Phones keep idle connections; drop the oldest instead of refusing new ones.
    cfg.lru_purge_enable = true;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK)
    {
        server = NULL;
        return err;
    }
    return httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_to_portal);
}

esp_err_t wifi_ap_start(void)
{
    if (running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Probes and redirected junk requests would flood the log otherwise.
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    // The copied DNS server logs two lines per query.
    esp_log_level_set("example_dns_redirect_server", ESP_LOG_WARN);

    // Once per boot; both report INVALID_STATE when already done.
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
    {
        err = esp_event_loop_create_default();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif)
    {
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK)
    {
        esp_netif_destroy_default_wifi(ap_netif);
        ap_netif = NULL;
        return err;
    }
    running = true;  // from here on wifi_ap_stop() cleans up

    // The password is new on every start; keep it out of NVS.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK)
    {
        err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    }
    if (err == ESP_OK)
    {
        make_ssid();
        err = make_password();
    }

    if (err == ESP_OK)
    {
        wifi_config_t cfg = {
            .ap =
                {
                    .channel = WIFI_AP_CHANNEL,
                    .authmode = WIFI_AUTH_WPA2_PSK,
                    .max_connection = WIFI_AP_MAX_CLIENTS,
                },
        };
        memcpy(cfg.ap.ssid, ssid, strlen(ssid));
        cfg.ap.ssid_len = strlen(ssid);
        memcpy(cfg.ap.password, password, strlen(password));
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err == ESP_OK)
        {
            err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
        }
    }
    if (err == ESP_OK)
    {
        err = esp_wifi_start();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "AP start failed: %s", esp_err_to_name(err));
        wifi_ap_stop();
        return err;
    }

#if WIFI_AP_DHCP_CAPTIVE_URI
    set_dhcp_captive_uri();
#endif

    dns_server_config_t dns_cfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns = start_dns_server(&dns_cfg);
    err = dns ? start_http() : ESP_FAIL;
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "DNS or HTTP start failed: %s", esp_err_to_name(err));
        wifi_ap_stop();
        return err;
    }

    client_connected = false;
    wifi_ap_touch();
    ESP_LOGI(TAG, "Up: %s, %s", ssid, WIFI_AP_URL);
    return ESP_OK;
}

void wifi_ap_stop(void)
{
    if (!running)
    {
        return;
    }
    if (server)
    {
        httpd_stop(server);
        server = NULL;
    }
    if (dns)
    {
        stop_dns_server(dns);
        dns = NULL;
    }
    esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event);
    esp_wifi_stop();
    // Frees the driver's buffers; the display and BLE want the RAM back.
    esp_wifi_deinit();
    esp_netif_destroy_default_wifi(ap_netif);
    ap_netif = NULL;
    client_connected = false;
    running = false;
    memset(password, 0, sizeof(password));
    ESP_LOGI(TAG, "Down");
}

esp_err_t wifi_ap_register(const httpd_uri_t *uri)
{
    if (!server)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return httpd_register_uri_handler(server, uri);
}
