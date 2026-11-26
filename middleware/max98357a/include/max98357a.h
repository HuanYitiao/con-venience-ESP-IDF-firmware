#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// MAX98357A on I2S, mono, 16 bit. Tones only, generated on the fly.

typedef enum
{
    MAX98357A_WAVE_SINE,
    MAX98357A_WAVE_SQUARE,
    MAX98357A_WAVE_PULSE,  // 25 % duty
} max98357a_waveform_t;

typedef struct
{
    uint32_t freq_hz;  // 0 = rest
    uint32_t duration_ms;
} max98357a_note_t;

// Starts I2S, sending silence; the amp stays off. Needs +3.3V_PERIPH:
// driving the I2S pins into the unpowered amp would back-power it.
esp_err_t max98357a_init(void);

// Stops I2S, switches the amp off and leaves the I2S pins floating. Call
// before power_periph_off(). The I2S channel itself is kept for the next
// init: deleting it breaks Wi-Fi's crypto (see max98357a.c).
esp_err_t max98357a_deinit(void);

void max98357a_set_waveform(max98357a_waveform_t wave);

// Plays the notes with gap_ms of silence after each. Switches the amp on
// and off around them. Blocks until the last note has left the speaker.
esp_err_t max98357a_play(const max98357a_note_t *notes, size_t count, uint32_t gap_ms);
