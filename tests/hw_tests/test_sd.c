// M6: card detect, mount, old card read, write/read, display and SD in turns.
#include "hw_tests_priv.h"

#if HW_TEST == HW_TEST_SD

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "sdcard.h"
#include "st75256.h"

static const char *TAG = "hw_test";

// Reads a whole file into buf (truncated to len - 1, NUL-terminated).
// Returns the byte count, or -1 if it cannot be opened.
static int read_file(const char *path, char *buf, size_t len)
{
    FILE *f = fopen(path, "r");
    if (!f)
    {
        return -1;
    }
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (int)n;
}

// Card detect, mount, list the root, read the old firmware's own profile,
// write/read/delete a file, then alternate display and SD access.
void test_sd_run(bool display_ok)
{
    if (!sdcard_present())
    {
        ESP_LOGW(TAG, "SD: no card");
        return;
    }
    esp_err_t err = sdcard_mount();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        return;
    }

    DIR *dir = opendir(SDCARD_MOUNT_POINT);
    if (dir)
    {
        struct dirent *e;
        while ((e = readdir(dir)) != NULL)
        {
            ESP_LOGI(TAG, "SD: %s%s", e->d_name, e->d_type == DT_DIR ? "/" : "");
        }
        closedir(dir);
    }
    else
    {
        ESP_LOGE(TAG, "SD: opendir failed");
    }

    static char buf[512];
    int         n = read_file(SDCARD_MOUNT_POINT "/self_profile/profile.json", buf, sizeof(buf));
    if (n >= 0)
    {
        ESP_LOGI(TAG, "SD: self profile.json, %d bytes shown: %s", n, buf);
    }
    else
    {
        ESP_LOGW(TAG, "SD: no /self_profile/profile.json");
    }

    // Write, read back, compare, delete.
    const char *test_path = SDCARD_MOUNT_POINT "/m6_test.txt";
    const char *text = "con-venience M6 SD test\n";
    FILE       *f = fopen(test_path, "w");
    bool        ok = f && fputs(text, f) >= 0;
    if (f)
    {
        ok = (fclose(f) == 0) && ok;
    }
    ok = ok && read_file(test_path, buf, sizeof(buf)) >= 0 && strcmp(buf, text) == 0;
    ok = (remove(test_path) == 0) && ok;
    ESP_LOGI(TAG, "SD: write/read/delete %s", ok ? "OK" : "FAILED");

    // Display and SD in turns on the shared bus. The bottom page flips
    // between black and white, so the display side is visible too.
    if (display_ok)
    {
        static uint8_t page[ST75256_WIDTH];
        int            display_errors = 0;
        int            sd_errors = 0;
        for (int i = 0; i < 50; i++)
        {
            memset(page, (i & 1) ? 0xFF : 0x00, sizeof(page));
            if (st75256_write(0, ST75256_PAGES - 1, ST75256_WIDTH, 1, page) != ESP_OK)
            {
                display_errors++;
            }
            f = fopen(test_path, "w");
            if (!f || fprintf(f, "%d\n", i) < 0 || fclose(f) != 0
                || read_file(test_path, buf, sizeof(buf)) < 0 || atoi(buf) != i)
            {
                sd_errors++;
            }
        }
        remove(test_path);
        ESP_LOGI(TAG, "SD: 50 alternating rounds, display errors %d, SD errors %d", display_errors,
                 sd_errors);
    }

    err = sdcard_unmount();
    ESP_LOGI(TAG, "SD: unmount %s", esp_err_to_name(err));
}

#endif
