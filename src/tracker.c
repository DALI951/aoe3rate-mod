/* tracker.c — per-slot ring sampling + EMA smoothing + spending-spike skip (R14)
 *
 * Samples the decrypted stock values at g_settings.sample_ms cadence.
 * Clock: realtime wall-clock (QueryPerformanceCounter -> ms, GetTickCount
 *   fallback). R16 Part A1: the DAL s_tick is a per-Present FRAME counter, not
 *   a millisecond game clock, so game-time-based rates scaled with FPS; the
 *   legacy UseGameTime setting is kept parsed-but-cosmetic (never used as a
 *   clock). A test-only override (tracker_set_clock_override) keeps the
 *   deterministic harness; it is never active inside the game.
 * Rate = (v[cur]-v[prev]) / elapsed_seconds, EMA-smoothed (0.1/0.2/0.4).
 * Pause/freeze rules:
 *   - the tick/clock not advancing frame-to-frame => global freeze.
 *   - a slot whose value is frozen across a full sample interval (e.g. ESC
 *     pause) => that slot's EMA freezes (no drift to zero). Resumes on the
 *     next value movement. (Engine behavior is live-checked per the plan.)
 * Spending spike: strongly negative rate vs current EMA by DiscontinuityRatio
 *  (~3.0 default) => EMA frozen (sample skipped), baseline kept fresh, resume.
 * Positive jumps (instant gains) counted normally per ShowGains.
 * Zero heap allocations per frame; all state static.
 */
#include "state.h"

static DWORD s_last_time = 0;
static int   s_have_time = 0;
static DWORD s_clock_override = 0;          /* test-only deterministic clock */
static int   s_clock_override_on = 0;

/* Harness hook (R16 A1): the rate-engine tests drive a synthetic timeline;
 * the shipped DLL never sets this because clock_now() below is always the
 * realtime QPC wall clock. */
void tracker_set_clock_override(DWORD ms) {
    s_clock_override = ms;
    s_clock_override_on = 1;
}

void tracker_init(void) {
    tracker_reset();
}

void tracker_reset(void) {
    for (int s = 0; s < (int)MAX_SLOTS; s++) {
        SlotTracker *st = &g_tracker[s];
        memset(st, 0, sizeof(SlotTracker));
        st->head = 0;
        st->count = 0;
        g_last_values[s] = 0.0f;
        g_last_ema[s] = 0.0f;
        g_slot_frozen[s] = 0;
        g_ema_valid[s] = 0;
    }
    g_last_sample_tick = 0;
    g_paused = 0;
    g_values_valid = 0;
    s_last_time = 0;
    s_have_time = 0;
    s_clock_override_on = 0;
}

static float ema_alpha(void) {
    if (g_settings.smoothing[0] == 'l' || g_settings.smoothing[0] == 'L')
        return 0.1f;
    if (g_settings.smoothing[0] == 'h' || g_settings.smoothing[0] == 'H')
        return 0.4f;
    return 0.2f; /* default: med */
}

static DWORD clock_now(void) {
    if (s_clock_override_on) return s_clock_override;
    /* realtime wall-clock ms (R16 A1): s_tick is a present-frame counter, NOT
     * a millisecond game clock — game-time deltas scaled the rate by FPS. */
    LARGE_INTEGER pc, freq;
    if (QueryPerformanceCounter(&pc) && QueryPerformanceFrequency(&freq) &&
        freq.QuadPart > 0)
        return (DWORD)((pc.QuadPart * 1000) / freq.QuadPart);
    return (DWORD)GetTickCount();
}

static void tracker_store_sample(int slot, DWORD clock, float value) {
    SlotTracker *st = &g_tracker[slot];
    st->ring[st->head].tick = clock;
    st->ring[st->head].value = value;
    st->head = (st->head + 1) % SAMPLE_RING_SIZE;
    if (st->count < SAMPLE_RING_SIZE) st->count++;
}

void tracker_sample(DWORD current_tick, const float *values, int num_slots) {
    (void)current_tick;
    if (values == NULL) return;
    if (num_slots > (int)MAX_SLOTS) num_slots = (int)MAX_SLOTS;

    DWORD now = clock_now();

    /* global pause: clock did not advance since last frame -> freeze */
    if (s_have_time && now == s_last_time) {
        g_paused = 1;
        return; /* no sampling, no EMA updates, no div-by-zero */
    }
    g_paused = 0;

    /* per-frame raw values for the overlay */
    for (int s = 0; s < num_slots; s++)
        g_last_values[s] = values[s];

    /* sample cadence */
    DWORD interval = (g_settings.sample_ms >= 100 && g_settings.sample_ms <= 1000)
                   ? (DWORD)g_settings.sample_ms : SAMPLE_MS_DEFAULT;
    int due = !s_have_time || (now - s_last_time) >= interval;
    if (!due) {
        /* R14: the sample-cadence accumulator must KEEP GROWING every non-due
         * frame until the interval elapses — s_last_time is only ever updated
         * on the DUE path below. The old `s_last_time = now;` here reset the
         * accumulator on EVERY non-due frame, so `due` could only ever fire on
         * the very first sample (!s_have_time) — live proof: exactly one
         * "RES t=1" line per 60s session, rates frozen at 0 forever. */
        g_values_valid = 1;
        return;
    }

    float alpha = ema_alpha();
    float ratio = (g_settings.discontinuity_ratio > 0.0f)
                ? g_settings.discontinuity_ratio : 3.0f;

    for (int s = 0; s < num_slots; s++) {
        float v = values[s];
        SlotTracker *st = &g_tracker[s];
        float inst = 0.0f;
        float dv = 0.0f;
        if (st->count >= 1) {
            unsigned int prev = (st->head + SAMPLE_RING_SIZE - 1) % SAMPLE_RING_SIZE;
            unsigned int dt = now - st->ring[prev].tick;
            dv = v - st->ring[prev].value;
            float elapsed_sec = (float)dt * 0.001f;
            if (dt > 0 && elapsed_sec > 0.0f)
                inst = dv / elapsed_sec;
        }

        /* slot-level freeze: value frozen across a full interval (ESC/pause) */
        if (st->count >= 1 && dv == 0.0f) {
            g_slot_frozen[s] = 1;
        } else {
            g_slot_frozen[s] = 0;
        }

        /* liveness: a slot is "alive" once its EMA has been bootstrapped
         * from a real sample. R16 A2: explicit valid-flag instead of an
         * EMA-magnitude heuristic (so a genuinely-zero-rate slot still counts
         * as alive and CAN be protected against spend spikes). */
        int alive_ema = g_ema_valid[s];

        /* spending-spike skip: strongly negative rate vs current EMA;
         * keep the ring baseline fresh so the next delta is measured
         * against the post-spike value, but freeze the EMA. */
        if (alive_ema && inst < 0.0f && (-inst) > g_last_ema[s] * ratio) {
            tracker_store_sample(s, now, v);
            continue;
        }

        /* R16 B8 (ShowGains): when instant gains are disabled, a strongly
         * positive jump is treated EXACTLY like a spend spike — the EMA does
         * not jump up (it would be a lie to report +2k/min gold when the +N
         * came in one frame from a trade/bonus). Baseline stays fresh.
         * R9: the reference is clamped to >= 0 so that a drained slot
         * (EMA <= 0) cannot make the skip match every positive sample
         * forever (inst > 0*ratio is always true when ref is 0). */
        float ref = g_last_ema[s] > 0.0f ? g_last_ema[s] : 0.0f;
        if (alive_ema && !g_settings.show_gains && inst > 0.0f &&
            inst > ref * ratio) {
            tracker_store_sample(s, now, v);
            continue;
        }

        if (g_slot_frozen[s]) {
            /* pause: keep the ring fresh, freeze the EMA (no drift) */
            tracker_store_sample(s, now, v);
            continue;
        }

        /* EMA update (positive jumps / instant gains counted normally) */
        if (alive_ema) {
            g_last_ema[s] += alpha * (inst - g_last_ema[s]);
        } else {
            g_last_ema[s] = inst; /* bootstrap first rate */
            /* only a REAL first rate (a delta exists in the ring) marks the
             * slot live — the very first sample (count==0, inst==0) just lays
             * down the baseline */
            if (st->count >= 1) g_ema_valid[s] = 1;
        }
        tracker_store_sample(s, now, v);
    }

    s_last_time = now;
    s_have_time = 1;
    g_values_valid = 1;
    g_last_sample_tick = current_tick;
}