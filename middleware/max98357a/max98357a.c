#include "max98357a.h"

#include <math.h>
#include <stdbool.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "power.h"

// Same sound as the old firmware.
#define MAX98357A_SAMPLE_RATE 44100
#define MAX98357A_AMPLITUDE 8000
// Fade in/out of each tone, so it starts and stops at zero (no click).
#define MAX98357A_FADE_MS 5
// Silence before the amp goes on, and between amp on and the first note.
#define MAX98357A_LEAD_IN_MS 20
#define MAX98357A_AMP_SETTLE_MS 5

#define MAX98357A_BUF_SAMPLES 256
#define TWO_PI_F 6.28318530718f

static i2s_chan_handle_t tx;  // made once, kept: see make_channel()
static bool              running;  // enabled, pins on I2S
static max98357a_waveform_t  wave = MAX98357A_WAVE_SINE;
// Samples the DMA buffers hold. Writing this much silence after the last
// note means the note has been played when the write returns.
static size_t dma_samples;

static size_t ms_to_samples(uint32_t ms)
{
    return (size_t)((uint64_t)MAX98357A_SAMPLE_RATE * ms / 1000);
}

static float sample_at(float phase)
{
    switch (wave)
    {
        case MAX98357A_WAVE_SQUARE:
            return phase < TWO_PI_F * 0.5f ? 1.0f : -1.0f;
        case MAX98357A_WAVE_PULSE:
            return phase < TWO_PI_F * 0.25f ? 1.0f : -1.0f;
        case MAX98357A_WAVE_SINE:
        default:
            return sinf(phase);
    }
}

static esp_err_t write_samples(const int16_t *buf, size_t n)
{
    size_t written = 0;
    return i2s_channel_write(tx, buf, n * sizeof(int16_t), &written, portMAX_DELAY);
}

static esp_err_t write_silence(size_t samples)
{
    static const int16_t zeros[MAX98357A_BUF_SAMPLES] = {0};
    while (samples > 0)
    {
        size_t    n = samples < MAX98357A_BUF_SAMPLES ? samples : MAX98357A_BUF_SAMPLES;
        esp_err_t err = write_samples(zeros, n);
        if (err != ESP_OK)
        {
            return err;
        }
        samples -= n;
    }
    return ESP_OK;
}

static esp_err_t write_tone(uint32_t freq_hz, uint32_t duration_ms)
{
    const size_t total = ms_to_samples(duration_ms);
    const float  step = TWO_PI_F * (float)freq_hz / MAX98357A_SAMPLE_RATE;
    size_t       fade = ms_to_samples(MAX98357A_FADE_MS);
    if (fade * 2 > total)
    {
        fade = total / 2;
    }

    int16_t buf[MAX98357A_BUF_SAMPLES];
    float   phase = 0.0f;
    size_t  done = 0;
    while (done < total)
    {
        size_t n = total - done < MAX98357A_BUF_SAMPLES ? total - done : MAX98357A_BUF_SAMPLES;
        for (size_t i = 0; i < n; i++)
        {
            size_t idx = done + i;
            float  env = 1.0f;
            if (idx < fade)
            {
                env = (float)idx / fade;
            }
            else if (idx >= total - fade)
            {
                env = (float)(total - 1 - idx) / fade;
            }
            buf[i] = (int16_t)(sample_at(phase) * MAX98357A_AMPLITUDE * env);
            phase += step;
            if (phase >= TWO_PI_F)
            {
                phase -= TWO_PI_F;
            }
        }
        esp_err_t err = write_samples(buf, n);
        if (err != ESP_OK)
        {
            return err;
        }
        done += n;
    }
    return ESP_OK;
}

static const i2s_std_gpio_config_t pins_cfg = {
    .mclk = I2S_GPIO_UNUSED,
    .bclk = (gpio_num_t)PIN_I2S_BCLK,
    .ws = (gpio_num_t)PIN_I2S_LRC,
    .dout = (gpio_num_t)PIN_I2S_DIN,
    .din = I2S_GPIO_UNUSED,
};

// The channel is made once and never deleted. Deleting it (i2s_del_channel)
// before Wi-Fi first used the AES/SHA accelerators broke them: every
// WPA2 handshake message 3 left with a wrong MIC and no phone could join
// the portal (IDF v6.1, ESP32-C6; both use GDMA). Keeping it costs one GDMA
// channel and the DMA buffers.
static esp_err_t make_channel(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // Without this the DMA keeps replaying its last buffers once writes stop.
    chan_cfg.auto_clear_after_cb = true;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx, NULL);
    if (err != ESP_OK)
    {
        tx = NULL;
        return err;
    }
    dma_samples = chan_cfg.dma_desc_num * chan_cfg.dma_frame_num;

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MAX98357A_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = pins_cfg,
    };
    return i2s_channel_init_std_mode(tx, &std_cfg);
}

esp_err_t max98357a_init(void)
{
    if (running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    // A channel from an earlier init: deinit took the pins away from it.
    esp_err_t err = tx ? i2s_channel_reconfig_std_gpio(tx, &pins_cfg) : make_channel();
    if (err == ESP_OK)
    {
        err = i2s_channel_enable(tx);
    }
    if (err == ESP_OK)
    {
        running = true;
        err = power_speaker_set(false);
    }
    if (err != ESP_OK)
    {
        max98357a_deinit();
    }
    return err;
}

esp_err_t max98357a_deinit(void)
{
    esp_err_t err = power_speaker_set(false);
    if (running)
    {
        i2s_channel_disable(tx);
        running = false;
    }
    // Floating, no pulls: a high pin would back-power the amp once
    // +3.3V_PERIPH is down. Also takes the pins from the I2S signals.
    const gpio_config_t pins = {
        .pin_bit_mask = (1ULL << PIN_I2S_BCLK) | (1ULL << PIN_I2S_LRC) | (1ULL << PIN_I2S_DIN),
        .mode = GPIO_MODE_INPUT,
    };
    esp_err_t pin_err = gpio_config(&pins);
    return err != ESP_OK ? err : pin_err;
}

void max98357a_set_waveform(max98357a_waveform_t w)
{
    wave = w;
}

esp_err_t max98357a_play(const max98357a_note_t *notes, size_t count, uint32_t gap_ms)
{
    if (!running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!notes && count > 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Amp on only while I2S is already sending silence.
    esp_err_t err = write_silence(ms_to_samples(MAX98357A_LEAD_IN_MS));
    if (err == ESP_OK)
    {
        err = power_speaker_set(true);
    }
    if (err == ESP_OK)
    {
        err = write_silence(ms_to_samples(MAX98357A_AMP_SETTLE_MS));
    }

    for (size_t i = 0; i < count && err == ESP_OK; i++)
    {
        if (notes[i].freq_hz > 0)
        {
            err = write_tone(notes[i].freq_hz, notes[i].duration_ms);
        }
        else
        {
            err = write_silence(ms_to_samples(notes[i].duration_ms));
        }
        if (err == ESP_OK && gap_ms > 0)
        {
            err = write_silence(ms_to_samples(gap_ms));
        }
    }

    // A write returns once its data is in the DMA buffers, not once it has
    // been played. A full buffer of silence pushes the last note out before
    // the amp goes off; cutting it short would click.
    if (err == ESP_OK)
    {
        err = write_silence(dma_samples);
    }
    esp_err_t off_err = power_speaker_set(false);
    return err != ESP_OK ? err : off_err;
}
