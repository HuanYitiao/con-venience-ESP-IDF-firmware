#include "acom.h"

#include <stdbool.h>
#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "acom";

// Protocol constants, unchanged from the old firmware: both ends must agree.
#define ACOM_BAUD 4800
#define ACOM_PROBE_MS 200
#define ACOM_TIMEOUT_MS 8000
// After seeing the peer, how long to wait for its ACK before proceeding anyway.
#define ACOM_ACK_WAIT_MS 1500
// Frames sent after finishing, so a peer still waiting for our ACK gets it.
#define ACOM_FINAL_FRAMES 5

// Two-byte header: a lone header byte matches random address data far too often.
#define ACOM_HEADER_0 0xA5
#define ACOM_HEADER_1 0x5A
#define ACOM_HEADER_LEN 2
#define ACOM_PAYLOAD_LEN 7  // addr[6] + type
#define ACOM_FRAME_LEN (ACOM_HEADER_LEN + ACOM_PAYLOAD_LEN + 1)
#define ACOM_FLAG_ACK 0x80

#define ADDR_OFS ACOM_HEADER_LEN
#define TYPE_OFS (ACOM_HEADER_LEN + 6)
#define CRC_OFS (ACOM_FRAME_LEN - 1)

#define ACOM_UART UART_NUM_1
#define ACOM_UART_RX_BUF 256
#define ACOM_TASK_STACK 3072
#define ACOM_TASK_PRIO 5

static TaskHandle_t task;

// Set by acom_start() before the task is notified; read-only while running.
static uint8_t        own_addr[6];
static uint8_t        own_type;
static acom_done_cb_t done_cb;

static volatile bool running;
static volatile bool stop_requested;

// Exchange state, task only.
static uint8_t rx_buf[ACOM_FRAME_LEN];
static int     rx_len;
static bool    peer_seen;
static uint8_t peer_addr[6];
static uint8_t peer_type;

static void set_pull_up(bool on)
{
    gpio_set_level(PIN_ACOM_PU, on);
}

static uint8_t frame_crc(const uint8_t *frame)
{
    return esp_rom_crc8_le(0, frame + ADDR_OFS, ACOM_PAYLOAD_LEN);
}

static void send_own_frame(void)
{
    uint8_t frame[ACOM_FRAME_LEN];
    frame[0] = ACOM_HEADER_0;
    frame[1] = ACOM_HEADER_1;
    memcpy(frame + ADDR_OFS, own_addr, 6);
    frame[TYPE_OFS] = own_type | (peer_seen ? ACOM_FLAG_ACK : 0);
    frame[CRC_OFS] = frame_crc(frame);
    uart_write_bytes(ACOM_UART, frame, ACOM_FRAME_LEN);
    // No RX flush here: our own echo is dropped by the parser (own-address
    // filter), while flushing would also drop a peer frame that just arrived.
    uart_wait_tx_done(ACOM_UART, pdMS_TO_TICKS(100));
}

// Random 0.5x..1.5x interval: with a fixed period, two devices whose probes
// happen to overlap keep colliding on the shared wire for the whole attempt.
static TickType_t probe_interval(void)
{
    return pdMS_TO_TICKS(ACOM_PROBE_MS / 2 + esp_random() % ACOM_PROBE_MS);
}

// Feeds one byte into the frame parser; true once a valid peer frame is in rx_buf.
static bool parse_byte(uint8_t b)
{
    rx_buf[rx_len++] = b;
    if (rx_len < ACOM_FRAME_LEN)
    {
        return false;
    }

    bool valid = rx_buf[0] == ACOM_HEADER_0 && rx_buf[1] == ACOM_HEADER_1
                 && frame_crc(rx_buf) == rx_buf[CRC_OFS];
    if (!valid)
    {
        memmove(rx_buf, rx_buf + 1, ACOM_FRAME_LEN - 1);
        rx_len = ACOM_FRAME_LEN - 1;
        return false;
    }

    rx_len = 0;
    // Our own echo on the single wire.
    return memcmp(rx_buf + ADDR_OFS, own_addr, 6) != 0;
}

// Keeps sending ACK frames for longer than the peer's longest probe
// interval, in case it is still waiting for ours.
static void send_final_frames(void)
{
    for (int i = 0; i < ACOM_FINAL_FRAMES && !stop_requested; i++)
    {
        vTaskDelay(pdMS_TO_TICKS(50 + esp_random() % 30));
        send_own_frame();
    }
}

// One exchange. Returns false if stopped.
static bool run_exchange(acom_result_t *result)
{
    rx_len = 0;
    peer_seen = false;
    TickType_t peer_seen_at = 0;
    TickType_t start = xTaskGetTickCount();
    // The first probe after a random delay also lets the line settle after
    // the pull-up turns on.
    TickType_t next_probe = start + probe_interval();

    set_pull_up(true);
    uart_flush_input(ACOM_UART);

    while (!stop_requested)
    {
        // Read until the next probe is due (at most ACOM_PROBE_MS * 1.5).
        TickType_t now = xTaskGetTickCount();
        TickType_t wait = (int32_t)(next_probe - now) > 0 ? next_probe - now : 0;
        uint8_t    buf[32];
        int        n = uart_read_bytes(ACOM_UART, buf, sizeof(buf), wait);

        for (int i = 0; i < n; i++)
        {
            if (!parse_byte(buf[i]))
            {
                continue;
            }
            // Lock onto the first peer; ignore anyone else on the wire.
            if (peer_seen && memcmp(peer_addr, rx_buf + ADDR_OFS, 6) != 0)
            {
                continue;
            }
            if (!peer_seen)
            {
                memcpy(peer_addr, rx_buf + ADDR_OFS, 6);
                peer_type = rx_buf[TYPE_OFS] & ~ACOM_FLAG_ACK;
                peer_seen = true;
                peer_seen_at = xTaskGetTickCount();
                // Tell the peer right away that we have its address.
                next_probe = peer_seen_at;
            }
            // The peer has our address too: both sides are done.
            if (rx_buf[TYPE_OFS] & ACOM_FLAG_ACK)
            {
                send_final_frames();
                *result = ACOM_DONE_ACKED;
                return !stop_requested;
            }
        }

        now = xTaskGetTickCount();
        // We have the peer's address but never heard its ACK: proceed rather than fail.
        if (peer_seen && now - peer_seen_at > pdMS_TO_TICKS(ACOM_ACK_WAIT_MS))
        {
            send_final_frames();
            *result = ACOM_DONE_NO_ACK;
            return !stop_requested;
        }
        if (!peer_seen && now - start > pdMS_TO_TICKS(ACOM_TIMEOUT_MS))
        {
            *result = ACOM_TIMEOUT;
            return true;
        }
        if ((int32_t)(now - next_probe) >= 0)
        {
            send_own_frame();
            next_probe = xTaskGetTickCount() + probe_interval();
        }
    }
    return false;
}

static void acom_task(void *arg)
{
    while (true)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        acom_result_t result = ACOM_TIMEOUT;
        bool          finished = run_exchange(&result);
        set_pull_up(false);
        running = false;

        if (finished && done_cb)
        {
            if (result == ACOM_TIMEOUT)
            {
                ESP_LOGI(TAG, "No peer");
            }
            else
            {
                ESP_LOGI(TAG, "Peer %02X:%02X:%02X:%02X:%02X:%02X type %u, %s", peer_addr[0],
                         peer_addr[1], peer_addr[2], peer_addr[3], peer_addr[4], peer_addr[5],
                         peer_type, result == ACOM_DONE_ACKED ? "acked" : "no ack");
            }
            done_cb(result, peer_addr, peer_type);
        }
    }
}

esp_err_t acom_init(void)
{
    if (task)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // Pull-up off until an exchange starts.
    const gpio_config_t pu = {
        .pin_bit_mask = 1ULL << PIN_ACOM_PU,
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&pu);
    if (err != ESP_OK)
    {
        return err;
    }
    set_pull_up(false);

    const uart_config_t cfg = {
        .baud_rate = ACOM_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_driver_install(ACOM_UART, ACOM_UART_RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK)
    {
        err = uart_param_config(ACOM_UART, &cfg);
    }
    // Open-drain before the pad is routed to UART TX: TX and RX share the
    // wire, and two push-pull ends must never drive against each other.
    if (err == ESP_OK)
    {
        err = gpio_od_enable(PIN_ACOM_OD);
    }
    if (err == ESP_OK)
    {
        err = uart_set_pin(ACOM_UART, PIN_ACOM_OD, PIN_ACOM_OD, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK)
    {
        return err;
    }

    if (xTaskCreate(acom_task, "acom", ACOM_TASK_STACK, NULL, ACOM_TASK_PRIO, &task) != pdPASS)
    {
        task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t acom_start(const uint8_t addr[6], uint8_t type, acom_done_cb_t cb)
{
    if (!task)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!addr || !cb || (type & ACOM_FLAG_ACK))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(own_addr, addr, 6);
    own_type = type;
    done_cb = cb;
    stop_requested = false;
    running = true;
    xTaskNotifyGive(task);
    return ESP_OK;
}

void acom_stop(void)
{
    if (!task)
    {
        return;
    }
    stop_requested = true;
    // The task checks the flag at least once per probe interval.
    while (running)
    {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
