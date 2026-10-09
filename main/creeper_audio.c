/*
 * Procedural Creeper sounds: a fuse-like hiss swelling up, then an explosion.
 * Everything is synthesized from white noise and a sine, so no sample assets
 * are needed and the cost is a handful of float ops per sample.
 */
#include <math.h>
#include <stdatomic.h>

#include "esp_log.h"
#include "sdkconfig.h"

#include "creeper_audio.h"

static const char *TAG = "creeper";

#define HISS_SAMPLES    ((int)(1.5f * CREEPER_SAMPLE_RATE))
#define GAP_SAMPLES     ((int)(0.06f * CREEPER_SAMPLE_RATE))
#define EXPLODE_SAMPLES ((int)(2.8f * CREEPER_SAMPLE_RATE))
#define TWO_PI          6.28318530718f

static atomic_bool s_trigger;
static atomic_int s_phase = CREEPER_IDLE;
static volatile float s_level;

/* Synth state, touched only from the audio callback. */
static int s_pos;
static uint32_t s_rng = 0x12345678;
static float s_hp_prev_in, s_hp_out;
static float s_lp1, s_lp2;
static float s_rumble_phase;
static float s_crackle;

static inline float noise(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(int32_t)s_rng * (1.0f / 2147483648.0f);
}

/* Smooth saturation, keeps peaks from wrapping. */
static inline float soft_clip(float x)
{
    if (x > 3.0f) {
        return 1.0f;
    }
    if (x < -3.0f) {
        return -1.0f;
    }
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

static float hiss_sample(int pos, float *env_out)
{
    float t = (float)pos / CREEPER_SAMPLE_RATE;
    float k = (float)pos / HISS_SAMPLES;

    /* Swell from quiet to loud, with a tremolo that speeds up like a fuse. */
    float env = 0.12f + 0.88f * k * sqrtf(k);
    float trem_hz = 5.0f + 15.0f * k;
    float trem = 0.8f + 0.2f * sinf(TWO_PI * trem_hz * t);
    if (pos < 400) {
        env *= pos / 400.0f;
    }

    /* High-passed noise (~2.5 kHz) gives the airy "tsss". */
    const float a = 0.73f;
    float x = noise();
    s_hp_out = a * (s_hp_out + x - s_hp_prev_in);
    s_hp_prev_in = x;

    /* A little sizzle: occasional short pops. */
    if ((s_rng & 0x3ff) == 0) {
        s_crackle = noise() * 0.6f;
    }
    s_crackle *= 0.9f;

    *env_out = env;
    return (s_hp_out * 0.9f + s_crackle) * env * trem * 0.55f;
}

static float explode_sample(int pos, float *env_out)
{
    float t = (float)pos / CREEPER_SAMPLE_RATE;

    /* Sharp attack, long exponential tail. */
    float attack = pos < 200 ? pos / 200.0f : 1.0f;
    float env = attack * expf(-t / 0.55f);

    /* Low-passed noise whose cutoff sweeps from bright to dull. */
    float fc = 7000.0f * expf(-t * 2.5f) + 180.0f;
    float alpha = 1.0f - expf(-TWO_PI * fc / CREEPER_SAMPLE_RATE);
    float x = noise();
    s_lp1 += alpha * (x - s_lp1);
    s_lp2 += alpha * (s_lp1 - s_lp2);
    float body = s_lp2 * 2.2f;

    /* Sub-bass thump dropping in pitch. */
    float f = 30.0f + 45.0f * expf(-t * 3.0f);
    s_rumble_phase += TWO_PI * f / CREEPER_SAMPLE_RATE;
    if (s_rumble_phase > TWO_PI) {
        s_rumble_phase -= TWO_PI;
    }
    float rumble = sinf(s_rumble_phase) * expf(-t / 0.8f) * attack;

    /* Debris crackles, getting rarer over time. */
    float p = 0.004f * expf(-t * 1.5f);
    if ((float)(s_rng >> 8) * (1.0f / 16777216.0f) < p) {
        s_crackle = noise();
    }
    s_crackle *= 0.97f;

    *env_out = env;
    return soft_clip((body * env + rumble * 0.9f + s_crackle * 0.5f) * 1.4f) * 0.95f;
}

bool creeper_trigger(void)
{
    if (atomic_load(&s_phase) != CREEPER_IDLE || atomic_load(&s_trigger)) {
        return false;
    }
    atomic_store(&s_trigger, true);
    return true;
}

creeper_phase_t creeper_phase(void)
{
    return (creeper_phase_t)atomic_load(&s_phase);
}

float creeper_level(void)
{
    return s_level;
}

void creeper_audio_fill(int16_t *buf, int frames)
{
    const float gain = 32767.0f * CONFIG_CREEPER_VOLUME / 100.0f;
    int phase = atomic_load(&s_phase);
    float env = 0.0f;

    if (phase == CREEPER_IDLE && atomic_exchange(&s_trigger, false)) {
        ESP_LOGI(TAG, "hisssss...");
        phase = CREEPER_HISS;
        s_pos = 0;
        s_hp_prev_in = s_hp_out = s_crackle = 0.0f;
    }

    for (int i = 0; i < frames; i++) {
        float y = 0.0f;

        if (phase == CREEPER_HISS) {
            if (s_pos < HISS_SAMPLES) {
                y = hiss_sample(s_pos, &env);
            } else {
                env = 0.0f; /* short breath before the bang */
            }
            if (++s_pos >= HISS_SAMPLES + GAP_SAMPLES) {
                ESP_LOGI(TAG, "BOOM");
                phase = CREEPER_EXPLODE;
                s_pos = 0;
                s_lp1 = s_lp2 = s_rumble_phase = s_crackle = 0.0f;
            }
        } else if (phase == CREEPER_EXPLODE) {
            y = explode_sample(s_pos, &env);
            if (++s_pos >= EXPLODE_SAMPLES) {
                ESP_LOGI(TAG, "done");
                phase = CREEPER_IDLE;
                env = 0.0f;
            }
        }

        int16_t s = (int16_t)(y * gain);
        buf[2 * i] = s;
        buf[2 * i + 1] = s;
    }

    s_level = env;
    atomic_store(&s_phase, phase);
}
