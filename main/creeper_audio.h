#pragma once

#include <stdbool.h>
#include <stdint.h>

#define CREEPER_SAMPLE_RATE 44100

typedef enum {
    CREEPER_IDLE = 0,
    CREEPER_HISS,
    CREEPER_EXPLODE,
} creeper_phase_t;

/* Request a hiss -> explosion sequence. Returns false if one is already running. */
bool creeper_trigger(void);

/* Fill an interleaved 16-bit stereo PCM buffer. Called from the A2DP source task. */
void creeper_audio_fill(int16_t *buf, int frames);

/* Current phase and loudness envelope (0..1) of the generated audio, for the LEDs. */
creeper_phase_t creeper_phase(void);
float creeper_level(void);
