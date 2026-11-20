// The setup portal, as the old firmware's wifi_config and set_time, on
// wifi_ap's HTTP server (which already answers every other path with a
// redirect to "/").

#include "portal.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "pcf85063a.h"
#include "portal_pages.h"
#include "sdcard.h"
#include "wifi_ap.h"

static const char *TAG = "portal";

#define WEB_PAGE SDCARD_MOUNT_POINT "/web/profile-generator.html"
#define WEB_JSZIP SDCARD_MOUNT_POINT "/web/jszip.min.js"
// The whole upload is held in RAM: profile.json (at most 2 KB, as contacts
// takes it), a 128 px avatar (4 KB) and the multipart framing.
#define UPLOAD_MAX 8192
#define SETTIME_MAX 256
#define CHUNK 1024

static volatile bool saved;
static char          saved_uuid[CONTACT_UUID_LEN];

// ── Files from the card ──────────────────────────────────

// Streams a file in chunks; the HTTP task has one request at a time, so one
// buffer does.
static esp_err_t send_file(httpd_req_t *req, const char *path, const char *type)
{
    wifi_ap_touch();
    FILE *f = fopen(path, "rb");
    if (!f)
    {
        ESP_LOGW(TAG, "%s: not on the card", path);
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_send(req, "not on the SD card", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    static char buf[CHUNK];
    size_t      n;
    esp_err_t   err = ESP_OK;
    while (err == ESP_OK && (n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        err = httpd_resp_send_chunk(req, buf, n);
    }
    fclose(f);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "%s: send broke off", path);
        return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t get_profile_page(httpd_req_t *req)
{
    return send_file(req, WEB_PAGE, "text/html");
}

static esp_err_t get_jszip(httpd_req_t *req)
{
    return send_file(req, WEB_JSZIP, "application/javascript");
}

static esp_err_t get_profile_json(httpd_req_t *req)
{
    return send_file(req, CONTACTS_SELF_JSON, "application/json");
}

static esp_err_t get_avatar(httpd_req_t *req)
{
    return send_file(req, CONTACTS_SELF_AVATAR, "application/octet-stream");
}

// ── Pages in flash ───────────────────────────────────────

// Android's sign-in window is a WebView ("; wv)" in its user agent) and
// cannot pick files: say so there.
static esp_err_t get_index(httpd_req_t *req)
{
    wifi_ap_touch();
    char ua[256] = "";
    httpd_req_get_hdr_value_str(req, "User-Agent", ua, sizeof(ua));
    bool webview = strstr(ua, "; wv)") != NULL;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, portal_index_page, HTTPD_RESP_USE_STRLEN);
    if (err == ESP_OK && webview)
    {
        err = httpd_resp_send_chunk(req, portal_index_webview_note, HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_OK)
    {
        err = httpd_resp_send_chunk(req, portal_index_end, HTTPD_RESP_USE_STRLEN);
    }
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t get_settime(httpd_req_t *req)
{
    wifi_ap_touch();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, portal_settime_page, HTTPD_RESP_USE_STRLEN);
}

// ── Request bodies ───────────────────────────────────────

// Reads the whole body into buf (len bytes, bounded by the caller).
static bool recv_body(httpd_req_t *req, char *buf, size_t len)
{
    size_t got = 0;
    while (got < len)
    {
        int n = httpd_req_recv(req, buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
        {
            continue;
        }
        if (n <= 0)
        {
            ESP_LOGW(TAG, "Body broke off after %u of %u bytes", (unsigned)got, (unsigned)len);
            return false;
        }
        got += n;
        wifi_ap_touch();
    }
    return true;
}

static esp_err_t reply(httpd_req_t *req, const char *status, const char *type, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, type);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// ── Time sync ────────────────────────────────────────────

static int days_in_month(int year, int month)
{
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool             leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : days[month - 1];
}

static bool field(const cJSON *root, const char *key, int lo, int hi, int *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble != (double)item->valueint
        || item->valueint < lo || item->valueint > hi)
    {
        return false;
    }
    *out = item->valueint;
    return true;
}

// As the old firmware: all six fields, each checked, or nothing is written.
static esp_err_t post_settime(httpd_req_t *req)
{
    wifi_ap_touch();
    static const char json[] = "application/json";
    if (req->content_len == 0 || req->content_len > SETTIME_MAX)
    {
        return reply(req, "400 Bad Request", json, "{\"ok\":false,\"err\":\"json\"}");
    }
    char body[SETTIME_MAX + 1];
    if (!recv_body(req, body, req->content_len))
    {
        return ESP_FAIL;
    }
    body[req->content_len] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return reply(req, "400 Bad Request", json, "{\"ok\":false,\"err\":\"json\"}");
    }
    int  year, month, day, hour, minute, second;
    bool ok = field(root, "year", 2000, 2099, &year) && field(root, "month", 1, 12, &month)
              && field(root, "day", 1, days_in_month(year, month), &day)
              && field(root, "hour", 0, 23, &hour) && field(root, "minute", 0, 59, &minute)
              && field(root, "second", 0, 59, &second);
    cJSON_Delete(root);
    if (!ok)
    {
        return reply(req, "400 Bad Request", json, "{\"ok\":false,\"err\":\"field\"}");
    }

    const pcf85063a_time_t t = {
        .year = (uint16_t)year,
        .month = (uint8_t)month,
        .day = (uint8_t)day,
        .hours = (uint8_t)hour,
        .minutes = (uint8_t)minute,
        .seconds = (uint8_t)second,
    };
    esp_err_t err = pcf85063a_set_time(&t);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "RTC write failed: %s", esp_err_to_name(err));
        return reply(req, "500 Internal Server Error", json, "{\"ok\":false,\"err\":\"rtc\"}");
    }
    ESP_LOGI(TAG, "Time set to %04d-%02d-%02d %02d:%02d:%02d", year, month, day, hour, minute,
             second);
    return reply(req, "200 OK", json, "{\"ok\":true}");
}

// ── Profile upload ───────────────────────────────────────

typedef struct
{
    const char *data;
    size_t      len;
} part_t;

// The boundary from "multipart/form-data; boundary=...", quotes removed.
static bool get_boundary(httpd_req_t *req, char *out, size_t size)
{
    char type[128];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", type, sizeof(type)) != ESP_OK
        || strncmp(type, "multipart/form-data", 19) != 0)
    {
        return false;
    }
    const char *b = strstr(type, "boundary=");
    if (!b)
    {
        return false;
    }
    b += 9;
    if (*b == '"')
    {
        b++;
    }
    size_t n = strcspn(b, "\";");
    if (n == 0 || n + 1 > size)
    {
        return false;
    }
    memcpy(out, b, n);
    out[n] = '\0';
    return true;
}

// The part's field name, from Content-Disposition's name="..." (not
// filename="...").
static bool part_name(const char *headers, size_t len, char *out, size_t size)
{
    for (const char *p = headers; p + 6 < headers + len; p++)
    {
        if ((p == headers || p[-1] == ' ' || p[-1] == ';') && memcmp(p, "name=\"", 6) == 0)
        {
            const char *start = p + 6;
            const char *end = memchr(start, '"', headers + len - start);
            if (!end || (size_t)(end - start) + 1 > size)
            {
                return false;
            }
            memcpy(out, start, end - start);
            out[end - start] = '\0';
            return true;
        }
    }
    return false;
}

// Splits body into its parts and picks "profile" and "avatar"; any other
// field is ignored, as the old firmware.
static bool parse_multipart(const char *body, size_t len, const char *boundary, part_t *profile,
                            part_t *avatar)
{
    char   delim[80];
    int    dlen = snprintf(delim, sizeof(delim), "\r\n--%s", boundary);
    // The body starts with the delimiter without its CRLF.
    if (dlen <= 2 || (size_t)dlen >= sizeof(delim) || len < (size_t)dlen - 2
        || memcmp(body, delim + 2, dlen - 2) != 0)
    {
        return false;
    }
    const char *p = body + dlen - 2;
    const char *end = body + len;
    while (end - p >= 2)
    {
        if (p[0] == '-' && p[1] == '-')
        {
            return true;  // closing delimiter
        }
        if (p[0] != '\r' || p[1] != '\n')
        {
            return false;
        }
        p += 2;
        const char *head_end = memmem(p, end - p, "\r\n\r\n", 4);
        if (!head_end)
        {
            return false;
        }
        const char *data = head_end + 4;
        const char *next = memmem(data, end - data, delim, dlen);
        if (!next)
        {
            return false;
        }
        char name[16];
        if (part_name(p, head_end - p, name, sizeof(name)))
        {
            part_t part = {.data = data, .len = (size_t)(next - data)};
            if (strcmp(name, "profile") == 0)
            {
                *profile = part;
            }
            else if (strcmp(name, "avatar") == 0)
            {
                *avatar = part;
            }
        }
        p = next + dlen;
    }
    return false;
}

static esp_err_t post_upload(httpd_req_t *req)
{
    wifi_ap_touch();
    static const char text[] = "text/plain";
    char              boundary[72];
    if (!get_boundary(req, boundary, sizeof(boundary)))
    {
        return reply(req, "400 Bad Request", text, "expected multipart/form-data");
    }
    if (req->content_len > UPLOAD_MAX)
    {
        ESP_LOGW(TAG, "Upload of %u bytes refused", (unsigned)req->content_len);
        return reply(req, "413 Payload Too Large", text, "upload too large");
    }
    static char body[UPLOAD_MAX];
    if (!recv_body(req, body, req->content_len))
    {
        return ESP_FAIL;
    }

    part_t profile = {0}, avatar = {0};
    if (!parse_multipart(body, req->content_len, boundary, &profile, &avatar) || !profile.data
        || !avatar.data)
    {
        ESP_LOGW(TAG, "Upload without both parts");
        return reply(req, "400 Bad Request", text, "incomplete upload");
    }
    ESP_LOGI(TAG, "Upload: profile %u bytes, avatar %u bytes", (unsigned)profile.len,
             (unsigned)avatar.len);

    char      uuid[CONTACT_UUID_LEN];
    esp_err_t err = contacts_save_self(profile.data, profile.len, (const uint8_t *)avatar.data,
                                       avatar.len, uuid);
    if (err == ESP_ERR_INVALID_ARG)
    {
        return reply(req, "400 Bad Request", text, "profile or avatar not valid");
    }
    if (err != ESP_OK)
    {
        return reply(req, "500 Internal Server Error", text, "could not save to the SD card");
    }
    memcpy(saved_uuid, uuid, sizeof(saved_uuid));
    saved = true;  // after the uuid: portal_saved() reads them the other way round
    return reply(req, "200 OK", text, "ok");
}

// ── Start and stop ───────────────────────────────────────

static const httpd_uri_t routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = get_index},
    {.uri = "/profile", .method = HTTP_GET, .handler = get_profile_page},
    {.uri = "/jszip.min.js", .method = HTTP_GET, .handler = get_jszip},
    {.uri = "/profile.json", .method = HTTP_GET, .handler = get_profile_json},
    {.uri = "/avatar.bin", .method = HTTP_GET, .handler = get_avatar},
    {.uri = "/upload", .method = HTTP_POST, .handler = post_upload},
    {.uri = "/settime", .method = HTTP_GET, .handler = get_settime},
    {.uri = "/settime", .method = HTTP_POST, .handler = post_settime},
};

esp_err_t portal_start(void)
{
    saved = false;
    saved_uuid[0] = '\0';
    esp_err_t err = wifi_ap_start();
    for (size_t i = 0; err == ESP_OK && i < sizeof(routes) / sizeof(routes[0]); i++)
    {
        err = wifi_ap_register(&routes[i]);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Start failed: %s", esp_err_to_name(err));
        wifi_ap_stop();
        return err;
    }
    ESP_LOGI(TAG, "Up: %s, %s", wifi_ap_ssid(), wifi_ap_url());
    return ESP_OK;
}

void portal_stop(void)
{
    wifi_ap_stop();
}

bool portal_saved(char uuid[CONTACT_UUID_LEN])
{
    if (!saved)
    {
        return false;
    }
    if (uuid)
    {
        memcpy(uuid, saved_uuid, CONTACT_UUID_LEN);
    }
    return true;
}
