#include "battery.h"

#include "board_pins.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mcp23017.h"
#include "nvs.h"

static const char *TAG = "battery";

// R14/R15 RC needs this long after BAT_MON_EN goes high (value from the old firmware).
#define BAT_MON_SETTLE_MS 450
#define BAT_SAMPLES 8
// VBAT -> R14/R15 (10k/10k) -> ADC.
#define BAT_DIVIDER 2
#define BAT_ATTEN ADC_ATTEN_DB_12

// Lowest VBAT (mV) for each level 1..BATTERY_LEVELS (index 0 unused), roughly
// 1.0..1.47 V per alkaline cell under light load. Tune against real discharge data.
static const uint16_t level_mv[BATTERY_LEVELS + 1] = {0,    3000, 3200, 3400, 3600,
                                                      3800, 4000, 4200, 4400};
// A reading must be this far below a step to count: load sag and rest
// recovery move VBAT by tens of mV.
#define BAT_LEVEL_HYST_MV 80
#define BAT_LEVEL_UNKNOWN 0xFF
// Same namespace and key as the old firmware.
#define BAT_NVS_NAMESPACE "power"
#define BAT_NVS_KEY "batLevel"

static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t         cali;
static adc_channel_t             chan;
static uint8_t                   level = BAT_LEVEL_UNKNOWN;

esp_err_t battery_init(void)
{
    esp_err_t  err;
    adc_unit_t unit;

    err = adc_oneshot_io_to_channel(PIN_VBAT_ADC, &unit, &chan);
    if (err != ESP_OK)
    {
        return err;
    }

    err = adc_oneshot_new_unit(&(adc_oneshot_unit_init_cfg_t){.unit_id = unit}, &adc);
    if (err != ESP_OK)
    {
        return err;
    }

    err = adc_oneshot_config_channel(adc, chan,
                                     &(adc_oneshot_chan_cfg_t){
                                         .atten = BAT_ATTEN,
                                         .bitwidth = ADC_BITWIDTH_DEFAULT,
                                     });
    if (err != ESP_OK)
    {
        return err;
    }

    err = adc_cali_create_scheme_curve_fitting(
        &(adc_cali_curve_fitting_config_t){
            .unit_id = unit,
            .chan = chan,
            .atten = BAT_ATTEN,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        },
        &cali);
    if (err != ESP_OK)
    {
        return err;
    }
    return ESP_OK;
}

esp_err_t battery_read_mv(uint16_t *mv)
{
    if (!adc || !cali)
    {
        return ESP_ERR_INVALID_STATE;
    }

    const uint8_t mon = 1 << PIN_MCP_BAT_MON_EN;
    esp_err_t     err = mcp23017_update_reg(MCP_OLATA, mon, mon);
    if (err != ESP_OK)
    {
        return err;  // divider unpowered: a reading would look like an empty battery
    }
    vTaskDelay(pdMS_TO_TICKS(BAT_MON_SETTLE_MS));

    uint32_t sum = 0;
    for (int i = 0; i < BAT_SAMPLES; i++)
    {
        int sample_mv = 0;
        err = adc_oneshot_get_calibrated_result(adc, cali, chan, &sample_mv);
        if (err != ESP_OK)
        {
            break;  // still switch the divider off below
        }
        sum += sample_mv;
    }

    // Always switch off, the divider drains the battery while on.
    esp_err_t off_err = mcp23017_update_reg(MCP_OLATA, mon, 0);
    if (err != ESP_OK)
    {
        return err;
    }
    if (off_err != ESP_OK)
    {
        return off_err;
    }

    *mv = (uint16_t)(sum / BAT_SAMPLES * BAT_DIVIDER);
    return ESP_OK;
}

static uint8_t level_from_mv(uint32_t mv)
{
    uint8_t l = 0;
    while (l < BATTERY_LEVELS && mv >= level_mv[l + 1])
    {
        l++;
    }
    return l;
}

// Errors are only logged: losing the stored level just means it is measured
// again from scratch after the next wake.
static void level_store(uint8_t l)
{
    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(BAT_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK)
    {
        err = nvs_set_u8(nvs, BAT_NVS_KEY, l);
        if (err == ESP_OK)
        {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Storing the level failed: %s", esp_err_to_name(err));
    }
}

void battery_level_init(bool powered_up)
{
    level = BAT_LEVEL_UNKNOWN;
    if (powered_up)
    {
        level_store(BAT_LEVEL_UNKNOWN);
        ESP_LOGI(TAG, "Powered up, level forgotten");
        return;
    }

    nvs_handle_t nvs;
    esp_err_t    err = nvs_open(BAT_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_OK)
    {
        err = nvs_get_u8(nvs, BAT_NVS_KEY, &level);
        nvs_close(nvs);
    }
    // NOT_FOUND (namespace or key) just means nothing was stored yet.
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGE(TAG, "Loading the level failed: %s", esp_err_to_name(err));
    }
    if (err != ESP_OK || level > BATTERY_LEVELS)
    {
        level = BAT_LEVEL_UNKNOWN;
    }

    if (level == BAT_LEVEL_UNKNOWN)
    {
        ESP_LOGI(TAG, "No stored level");
    }
    else
    {
        ESP_LOGI(TAG, "Stored level %u", level);
    }
}

void battery_level_update(uint16_t mv)
{
    uint8_t next;
    if (level == BAT_LEVEL_UNKNOWN)
    {
        next = level_from_mv(mv);
    }
    else
    {
        // Only down, and at most one step per reading.
        if (level == 0 || level_from_mv((uint32_t)mv + BAT_LEVEL_HYST_MV) >= level)
        {
            return;
        }
        next = level - 1;
    }

    ESP_LOGI(TAG, "Level %u -> %u (%u mV)", level == BAT_LEVEL_UNKNOWN ? BATTERY_LEVELS : level,
             next, mv);
    level = next;
    level_store(next);
}

uint8_t battery_level(void)
{
    // Nothing measured yet (e.g. on USB without a battery): show full.
    return level == BAT_LEVEL_UNKNOWN ? BATTERY_LEVELS : level;
}
