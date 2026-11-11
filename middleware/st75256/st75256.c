#include "st75256.h"

#include <stdbool.h>
#include <stddef.h>

#include "board_pins.h"
#include "board_spi.h"
#include "driver/ledc.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mcp23017.h"

// Same bus settings as the old firmware.
#define ST75256_PCLK_HZ (20 * 1000 * 1000)
#define ST75256_SPI_MODE 0
#define ST75256_RESET_MS 100
// Wait between the power control command and display on (old firmware).
#define ST75256_POWER_UP_MS 100
// A full frame takes about 3.3 ms at 20 MHz.
#define ST75256_WRITE_TIMEOUT_MS 1000

// Commands. EXT selects one of two command sets; 0x20 means something
// different in each.
#define CMD_EXT0 0x30
#define CMD_EXT1 0x31
#define CMD_COLUMN_ADDR 0x15  // EXT0
#define CMD_PAGE_ADDR 0x75    // EXT0
#define CMD_WRITE_RAM 0x5C    // EXT0
#define CMD_DISPLAY_OFF 0xAE  // EXT0
#define CMD_DISPLAY_ON 0xAF   // EXT0
#define CMD_SLEEP_IN 0x95     // EXT0

// LEDC for the backlight. BL is active-low, so the output is inverted and
// the duty equals the brightness.
#define BL_SPEED_MODE LEDC_LOW_SPEED_MODE
#define BL_TIMER LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_FREQ_HZ 5000
#define BL_RES LEDC_TIMER_8_BIT
#define BL_MAX_DUTY ((1 << 8) - 1)

typedef struct
{
    uint8_t cmd;
    uint8_t len;
    uint8_t data[16];
} init_cmd_t;

// Init sequence from the old firmware, which is known to work on this panel.
// The tunable commands (0x32, 0xCA, 0x81) are sent from the tuning struct
// below, in apply_tuning().
static const init_cmd_t init_seq[] = {
    {CMD_EXT0, 0, {0}},
    {0x94, 0, {0}},  // sleep out
    {CMD_EXT1, 0, {0}},
    {0xD7, 1, {0x9F}},  // auto read off
    // Gray level set: the 16 steps the 4 gray levels are picked from.
    {0x20,
     16,
     {0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F, 0x11, 0x13, 0x15, 0x17, 0x19, 0x1B, 0x1D,
      0x1F}},
    {CMD_EXT0, 0, {0}},
    {CMD_PAGE_ADDR, 2, {0x00, ST75256_PAGES - 1}},
    {CMD_COLUMN_ADDR, 2, {0x00, ST75256_WIDTH - 1}},
    {0xBC, 2, {0x00, 0xA6}},  // data scan direction
    {0xF0, 1, {0x11}},        // display mode: 4 gray levels
};

// Power control (EXT0): booster, regulator, follower on. Last, after tuning.
#define CMD_POWER_CONTROL 0x20
#define POWER_CONTROL_ALL_ON 0x0B

#define CMD_ANALOG_SET 0x32     // EXT1
#define CMD_FRAME_RATE 0xF0     // EXT1 (EXT0 0xF0 is display mode)
#define CMD_DISPLAY_CTRL 0xCA   // EXT0
#define CMD_VOP 0x81            // EXT0
#define DISPLAY_CTRL_DUTY 0x7F  // 1/128

#define VOP_MAX 511
#define BIAS_MAX 5
#define BOOSTER_MAX 3
#define FRAME_RATE_MAX 31

static st75256_tuning_t tuning = {
    // Tuned on the panel with M5_DISPLAY_TUNE (2026-11-11); the old firmware
    // had 313 (0x81 0x39 0x04). The others are the old values.
    .vop = 318,
    .bias = 2,                  // 1/12
    .booster = 1,
    .line_inv = 0x20,
    .frame_rate = ST75256_FRAME_RATE_DEFAULT,
};

static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t         write_done;
static bool                      backlight_ready;

// Runs in the SPI ISR when a tx_color transfer has finished.
static bool on_write_done(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata,
                          void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(write_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t cmd(uint8_t c, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(io, c, data, len);
}

static esp_err_t apply_tuning(void)
{
    const uint8_t analog[] = {0x00, tuning.booster, tuning.bias};
    const uint8_t ctrl[] = {0x00, DISPLAY_CTRL_DUTY, tuning.line_inv};
    const uint8_t vop[] = {tuning.vop & 0x3F, (tuning.vop >> 6) & 0x07};

    esp_err_t err = cmd(CMD_EXT1, NULL, 0);
    if (err == ESP_OK)
    {
        err = cmd(CMD_ANALOG_SET, analog, sizeof(analog));
    }
    if (err == ESP_OK && tuning.frame_rate != ST75256_FRAME_RATE_DEFAULT)
    {
        // One rate per temperature range; the same for all four.
        const uint8_t fr[] = {tuning.frame_rate, tuning.frame_rate, tuning.frame_rate,
                              tuning.frame_rate};
        err = cmd(CMD_FRAME_RATE, fr, sizeof(fr));
    }
    if (err == ESP_OK)
    {
        err = cmd(CMD_EXT0, NULL, 0);
    }
    if (err == ESP_OK)
    {
        err = cmd(CMD_DISPLAY_CTRL, ctrl, sizeof(ctrl));
    }
    if (err == ESP_OK)
    {
        err = cmd(CMD_VOP, vop, sizeof(vop));
    }
    return err;
}

st75256_tuning_t st75256_get_tuning(void)
{
    return tuning;
}

esp_err_t st75256_set_tuning(const st75256_tuning_t *t)
{
    if (!io)
    {
        return ESP_ERR_INVALID_STATE;
    }
    tuning = *t;
    if (tuning.vop > VOP_MAX)
    {
        tuning.vop = VOP_MAX;
    }
    if (tuning.bias > BIAS_MAX)
    {
        tuning.bias = BIAS_MAX;
    }
    if (tuning.booster > BOOSTER_MAX)
    {
        tuning.booster = BOOSTER_MAX;
    }
    if (tuning.frame_rate != ST75256_FRAME_RATE_DEFAULT && tuning.frame_rate > FRAME_RATE_MAX)
    {
        tuning.frame_rate = FRAME_RATE_MAX;
    }
    return apply_tuning();
}

static esp_err_t set_reset(bool active)
{
    return mcp23017_update_reg(MCP_OLATA, 1 << PIN_MCP_LCD_RST, active ? 0 : 1 << PIN_MCP_LCD_RST);
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BL_SPEED_MODE,
        .duty_resolution = BL_RES,
        .timer_num = BL_TIMER,
        .freq_hz = BL_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK)
    {
        return err;
    }
    const ledc_channel_config_t channel = {
        .gpio_num = PIN_DISPLAY_BL,
        .speed_mode = BL_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,
        .flags.output_invert = 1,
    };
    err = ledc_channel_config(&channel);
    if (err == ESP_OK)
    {
        backlight_ready = true;
    }
    return err;
}

static esp_err_t clear(void)
{
    static const uint8_t zeros[ST75256_WIDTH] = {0};
    for (int page = 0; page < ST75256_PAGES; page++)
    {
        esp_err_t err = st75256_write(0, page, ST75256_WIDTH, 1, zeros);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t st75256_init(void)
{
    if (io)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = backlight_init();
    if (err != ESP_OK)
    {
        return err;
    }

    write_done = xSemaphoreCreateBinary();
    if (!write_done)
    {
        return ESP_ERR_NO_MEM;
    }

    // Also drives CS high, so the display stays off the bus while the SD
    // card is used.
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = PIN_DISPLAY_CS,
        .dc_gpio_num = PIN_DISPLAY_DC,
        .spi_mode = ST75256_SPI_MODE,
        .pclk_hz = ST75256_PCLK_HZ,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_write_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_SPI_HOST, &io_config, &io);
    if (err != ESP_OK)
    {
        io = NULL;
        return err;
    }

    // Hardware reset. LCD_RST is not in power's Port A mask: latch first,
    // direction second, as in power_early_init().
    err = set_reset(true);
    if (err == ESP_OK)
    {
        err = mcp23017_update_reg(MCP_IODIRA, 1 << PIN_MCP_LCD_RST, 0);
    }
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ST75256_RESET_MS));
    err = set_reset(false);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ST75256_RESET_MS));

    for (size_t i = 0; i < sizeof(init_seq) / sizeof(init_seq[0]); i++)
    {
        err = cmd(init_seq[i].cmd, init_seq[i].len ? init_seq[i].data : NULL, init_seq[i].len);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    err = apply_tuning();
    if (err == ESP_OK)
    {
        const uint8_t power = POWER_CONTROL_ALL_ON;
        err = cmd(CMD_POWER_CONTROL, &power, 1);
    }
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ST75256_POWER_UP_MS));

    // Clear before display on, so RAM garbage never shows.
    err = clear();
    if (err != ESP_OK)
    {
        return err;
    }
    return cmd(CMD_DISPLAY_ON, NULL, 0);
}

esp_err_t st75256_write(int x, int page, int w, int pages, const uint8_t *data)
{
    if (!io)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || x < 0 || page < 0 || w <= 0 || pages <= 0 || x + w > ST75256_WIDTH
        || page + pages > ST75256_PAGES)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t cols[] = {(uint8_t)x, (uint8_t)(x + w - 1)};
    const uint8_t rows[] = {(uint8_t)page, (uint8_t)(page + pages - 1)};
    esp_err_t     err = cmd(CMD_EXT0, NULL, 0);
    if (err == ESP_OK)
    {
        err = cmd(CMD_COLUMN_ADDR, cols, sizeof(cols));
    }
    if (err == ESP_OK)
    {
        err = cmd(CMD_PAGE_ADDR, rows, sizeof(rows));
    }
    if (err == ESP_OK)
    {
        // Queued for DMA; on_write_done() fires once it is out.
        err = esp_lcd_panel_io_tx_color(io, CMD_WRITE_RAM, data, (size_t)w * pages);
    }
    if (err != ESP_OK)
    {
        return err;
    }
    if (xSemaphoreTake(write_done, pdMS_TO_TICKS(ST75256_WRITE_TIMEOUT_MS)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t st75256_set_backlight(uint8_t percent)
{
    if (!backlight_ready)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (percent > 100)
    {
        percent = 100;
    }
    esp_err_t err = ledc_set_duty(BL_SPEED_MODE, BL_CHANNEL, (uint32_t)percent * BL_MAX_DUTY / 100);
    if (err != ESP_OK)
    {
        return err;
    }
    return ledc_update_duty(BL_SPEED_MODE, BL_CHANNEL);
}

esp_err_t st75256_sleep(void)
{
    if (backlight_ready)
    {
        st75256_set_backlight(0);
    }
    if (io)
    {
        // Errors ignored: holding reset below still keeps it quiet.
        cmd(CMD_EXT0, NULL, 0);
        cmd(CMD_DISPLAY_OFF, NULL, 0);
        cmd(CMD_SLEEP_IN, NULL, 0);
    }
    return set_reset(true);
}
