#include "button.h"

#include <stdbool.h>
#include <stdint.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mcp23017.h"

static const char *TAG = "button";

// A pin must read the same for this long before a press or release counts.
#define BUTTON_DEBOUNCE_MS 20
// Held at least this long: LONG_PRESS on release, otherwise CLICK.
#define BUTTON_LONG_PRESS_MS 300
// Poll rate while a button is held or bouncing. Polling instead of waiting
// for the next edge means a release lost in a bounce is still seen.
#define BUTTON_POLL_MS 10
// Poll rate while idle. INT is wire-ORed with the RTC: if it is ever left
// low, no further falling edge arrives, and this read is what recovers it.
#define BUTTON_IDLE_POLL_MS 1000

#define BUTTON_TASK_STACK 3072
#define BUTTON_TASK_PRIO 5

// Indexed by button_id_t.
static const uint8_t button_pins[BUTTON_COUNT] = {
    [BUTTON_UP] = PIN_MCP_BTN_UP,         [BUTTON_DOWN] = PIN_MCP_BTN_DOWN,
    [BUTTON_LEFT] = PIN_MCP_BTN_LEFT, [BUTTON_RIGHT] = PIN_MCP_BTN_RIGHT,
    [BUTTON_PAIR] = PIN_MCP_BTN_PAIR,
};

typedef struct
{
    bool       raw;         // last reading
    TickType_t raw_since;   // when raw last changed
    bool       stable;      // debounced state
    TickType_t pressed_at;  // when stable went to pressed
    bool       ignore;      // held at init: no event for this press
} button_state_t;

static button_state_t buttons[BUTTON_COUNT];
static button_cb_t    callback;
static TaskHandle_t   task;

// Buttons pull to ground: pressed = 0.
static bool is_pressed(uint8_t gpiob, button_id_t id)
{
    return !((gpiob >> button_pins[id]) & 1);
}

// Feeds one GPIOB reading into every button. Returns true while any button
// is held or not yet settled, i.e. while fast polling is needed.
static bool process(uint8_t gpiob, TickType_t now)
{
    bool busy = false;
    for (button_id_t id = 0; id < BUTTON_COUNT; id++)
    {
        button_state_t *b = &buttons[id];
        bool            raw = is_pressed(gpiob, id);
        if (raw != b->raw)
        {
            b->raw = raw;
            b->raw_since = now;
        }

        if (b->raw != b->stable && now - b->raw_since >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS))
        {
            b->stable = b->raw;
            if (b->stable)
            {
                b->pressed_at = now;
            }
            else if (b->ignore)
            {
                b->ignore = false;
            }
            else
            {
                bool long_press = now - b->pressed_at >= pdMS_TO_TICKS(BUTTON_LONG_PRESS_MS);
                callback(id, long_press ? BUTTON_LONG_PRESS : BUTTON_CLICK);
            }
        }

        busy |= b->stable || b->raw != b->stable;
    }
    return busy;
}

static void button_task(void *arg)
{
    bool busy = true;
    bool failing = false;
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(busy ? BUTTON_POLL_MS : BUTTON_IDLE_POLL_MS));

        // Reading GPIOB also releases INTB.
        uint8_t   gpiob = 0;
        esp_err_t err = mcp23017_read_reg(MCP_GPIOB, &gpiob);
        if (err != ESP_OK)
        {
            if (!failing)
            {
                ESP_LOGE(TAG, "Read GPIOB failed: %s", esp_err_to_name(err));
            }
            failing = true;
            busy = false;  // retry at the idle rate, not every 10 ms
            continue;
        }
        failing = false;
        busy = process(gpiob, xTaskGetTickCount());
    }
}

// Only wakes the task: I2C cannot be used from an ISR.
static void IRAM_ATTR int_isr(void *arg)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(task, &woken);
    portYIELD_FROM_ISR(woken);
}

esp_err_t button_init(button_cb_t cb)
{
    if (!cb)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (task)
    {
        return ESP_ERR_INVALID_STATE;
    }
    callback = cb;

    // 1. Port B: inputs with pull-ups, interrupt on any change. power_sleep()
    //    switches to compare-with-DEFVAL; INTCONB = 0 undoes that after a wake.
    esp_err_t err = mcp23017_update_reg(MCP_IODIRB, MCP_BTN_MASK, MCP_BTN_MASK);
    if (err == ESP_OK)
    {
        err = mcp23017_update_reg(MCP_GPPUB, MCP_BTN_MASK, MCP_BTN_MASK);
    }
    if (err == ESP_OK)
    {
        err = mcp23017_update_reg(MCP_INTCONB, MCP_BTN_MASK, 0);
    }
    if (err == ESP_OK)
    {
        err = mcp23017_update_reg(MCP_GPINTENB, MCP_BTN_MASK, MCP_BTN_MASK);
    }

    // 2. Start state, and clear a pending interrupt. A button still held from
    //    the wake press counts as pressed, but its release gives no event.
    uint8_t gpiob = 0;
    if (err == ESP_OK)
    {
        err = mcp23017_read_reg(MCP_GPIOB, &gpiob);
    }
    if (err != ESP_OK)
    {
        return err;
    }
    TickType_t now = xTaskGetTickCount();
    for (button_id_t id = 0; id < BUTTON_COUNT; id++)
    {
        bool pressed = is_pressed(gpiob, id);
        buttons[id] = (button_state_t){
            .raw = pressed, .raw_since = now, .stable = pressed, .ignore = pressed};
    }

    // 3. Task before ISR: the ISR notifies it.
    if (xTaskCreate(button_task, "button", BUTTON_TASK_STACK, NULL, BUTTON_TASK_PRIO, &task)
        != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    // 4. INT (GPIO5): falling edge. MCP INTB is open-drain, so it needs the pull-up.
    //    An edge missed between steps 2 and 4 leaves INT low; the idle poll
    //    clears it within a second.
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << PIN_MCP_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    err = gpio_config(&int_cfg);
    if (err == ESP_OK)
    {
        err = gpio_install_isr_service(0);
        if (err == ESP_ERR_INVALID_STATE)  // already installed by someone else
        {
            err = ESP_OK;
        }
    }
    if (err == ESP_OK)
    {
        err = gpio_isr_handler_add(PIN_MCP_INT, int_isr, NULL);
    }
    if (err != ESP_OK)
    {
        // The task keeps working at the idle poll rate, just with slow response.
        ESP_LOGE(TAG, "INT setup failed: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}
