/* rate.c — rates from tracker; /s vs /min conversion; game-time vs realtime (R14) */
#include "state.h"

/* EMA-smoothed rate for one slot (value/sec). */
float rate_get_ema(int slot) {
    if (slot < 0 || (DWORD)slot >= MAX_SLOTS) return 0.0f;
    return g_last_ema[slot];
}

/* Instantaneous raw rate from the last two ring samples (value/sec).
 * Fallback to EMA when the ring isn't yet primed. */
float rate_get_raw(int slot) {
    if (slot < 0 || (DWORD)slot >= MAX_SLOTS) return 0.0f;
    SlotTracker *st = &g_tracker[slot];
    if (st == NULL || st->count < 2) return g_last_ema[slot];
    unsigned int idx = (st->head + SAMPLE_RING_SIZE - 1) % SAMPLE_RING_SIZE;
    unsigned int prev = (st->head + SAMPLE_RING_SIZE - 2) % SAMPLE_RING_SIZE;
    unsigned int dt = st->ring[idx].tick - st->ring[prev].tick;
    if (dt == 0) return 0.0f;
    float dv = st->ring[idx].value - st->ring[prev].value;
    float elapsed = (float)dt * 0.001f; /* 1 tick = 1 ms game-time convention */
    if (elapsed <= 0.0f) return 0.0f;
    return dv / elapsed;
}

/* value/sec or /min display multiplier + label */
float rate_display(int slot) {
    float r = rate_get_ema(slot);
    if (g_settings.use_unit_min) r *= 60.0f;
    return r;
}

const char *rate_unit_label(void) {
    return g_settings.use_unit_min ? "/min" : "/s";
}