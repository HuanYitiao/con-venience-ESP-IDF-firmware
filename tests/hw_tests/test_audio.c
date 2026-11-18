// M7: startup beep and melodies; listen for clicks at start and end.
#include "hw_tests_priv.h"

#if HW_TEST == HW_TEST_AUDIO

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "max98357a.h"

static const char *TAG = "hw_test";

static void play(const char *what, const max98357a_note_t *notes, size_t count, uint32_t gap_ms)
{
    ESP_LOGI(TAG, "Audio: %s", what);
    esp_err_t err = max98357a_play(notes, count, gap_ms);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio: %s failed: %s", what, esp_err_to_name(err));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// Needs +3.3V_PERIPH, which display_start() switched on.
void test_audio_run(void)
{
    esp_err_t err = max98357a_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Audio init failed: %s", esp_err_to_name(err));
        return;
    }

    static const max98357a_note_t beep[] = {{2500, 50}};  // old startup sound
    static const max98357a_note_t melody[] = {{262, 140}, {294, 140}, {330, 400}};
    const size_t              melody_len = sizeof(melody) / sizeof(melody[0]);

    max98357a_set_waveform(MAX98357A_WAVE_SQUARE);
    play("startup beep", beep, 1, 15);
    play("melody, square", melody, melody_len, 15);
    // Small speakers barely reproduce a few hundred Hz; a square wave is
    // heard mostly through its harmonics, a sine has none. Same melody as a
    // sine at x1, x4 and x8 the frequency.
    static const max98357a_note_t melody_x4[] = {{1047, 140}, {1175, 140}, {1319, 400}};
    static const max98357a_note_t melody_x8[] = {{2093, 140}, {2349, 140}, {2637, 400}};
    max98357a_set_waveform(MAX98357A_WAVE_SINE);
    play("melody, sine", melody, melody_len, 15);
    play("melody, sine, 2 octaves up", melody_x4, melody_len, 15);
    play("melody, sine, 3 octaves up", melody_x8, melody_len, 15);

    err = max98357a_deinit();
    ESP_LOGI(TAG, "Audio: deinit %s", esp_err_to_name(err));
}

#endif
