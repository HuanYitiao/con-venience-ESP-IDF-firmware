#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

// Soft AP with a captive portal: WPA2 with a fresh random password on every
// start, one client at a time, DNS answering every name with the AP address,
// and an HTTP server that redirects every unknown path to "/". Phones probe
// a known URL after joining; the redirect makes them open a sign-in window
// on the AP. The pages themselves are registered by the caller.

#define WIFI_AP_IP "192.168.4.1"

// Brings the AP, DNS and HTTP server up. Needs NVS (PHY calibration).
esp_err_t wifi_ap_start(void);

// Takes everything down again and frees the Wi-Fi driver.
void wifi_ap_stop(void);

// Adds a page or endpoint. Call after each wifi_ap_start(); the handler
// runs in the HTTP server task.
esp_err_t wifi_ap_register(const httpd_uri_t *uri);

// Valid while the AP is up.
const char *wifi_ap_ssid(void);      // "con-venience-XXXX", from the soft-AP MAC
const char *wifi_ap_password(void);  // 16 hex digits
const char *wifi_ap_url(void);       // "http://192.168.4.1/"

bool wifi_ap_client_connected(void);

// Activity tracking for an idle timeout: a client joining and every request
// to an unknown path count; registered handlers call wifi_ap_touch().
void     wifi_ap_touch(void);
uint32_t wifi_ap_idle_ms(void);
