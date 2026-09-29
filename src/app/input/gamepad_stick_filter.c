#include "gamepad_stick_filter.h"

#include <stddef.h>

/**
 * Integer square root rounded down, by testing one root bit at a time. The axis
 * bound keeps the argument under 2 * 32767^2, so the root fits in 16 bits. This
 * keeps the filter free of libm and of float rounding, which keeps the axis
 * maths reproducible in tests.
 */
static uint32_t gamepad_stick_magnitude(int32_t x, int32_t y);

static int16_t gamepad_stick_clamp(int32_t value);

/** Deadzone cut plus range rescale, so the surviving travel still reaches full scale. */
static void gamepad_stick_scale(int32_t raw_x, int32_t raw_y, uint16_t deadzone,
                                int16_t *out_x, int16_t *out_y);

/** True when the report has collapsed onto the middle of a stick held out wide. */
static bool gamepad_stick_collapsed(uint32_t magnitude, uint32_t held_magnitude);

/** Elapsed test on a wrapping millisecond clock, valid for spans under ~24 days. */
static bool gamepad_stick_elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t limit_ms);

void gamepad_stick_filter_reset(gamepad_stick_filter_t *filter) {
    if (filter == NULL) {
        return;
    }
    filter->out_x = 0;
    filter->out_y = 0;
    filter->pending_x = 0;
    filter->pending_y = 0;
    filter->pending_since_ms = 0;
    filter->pending_reports = 0;
}

gamepad_stick_filter_result_t gamepad_stick_filter_feed(gamepad_stick_filter_t *filter,
                                                        int16_t raw_x, int16_t raw_y,
                                                        uint16_t deadzone, uint32_t now_ms,
                                                        int16_t *out_x, int16_t *out_y) {
    if (filter == NULL) {
        return GAMEPAD_STICK_FILTER_ACCEPTED;
    }
    const int32_t x = gamepad_stick_clamp(raw_x);
    const int32_t y = gamepad_stick_clamp(raw_y);
    const uint32_t magnitude = gamepad_stick_magnitude(x, y);
    const uint32_t held_magnitude = gamepad_stick_magnitude(filter->out_x, filter->out_y);
    bool accept_despite_hold = false;

    if (filter->pending_reports > 0) {
        if (gamepad_stick_collapsed(magnitude, held_magnitude)) {
            filter->pending_x = (int16_t) x;
            filter->pending_y = (int16_t) y;
            filter->pending_reports++;
            /* Three reports in a row is a release, not one bad report. */
            if (filter->pending_reports < GAMEPAD_STICK_GUARD_CONFIRM_REPORTS &&
                !gamepad_stick_elapsed(now_ms, filter->pending_since_ms, GAMEPAD_STICK_GUARD_HOLD_MAX_MS)) {
                if (out_x != NULL) {
                    *out_x = filter->out_x;
                }
                if (out_y != NULL) {
                    *out_y = filter->out_y;
                }
                return GAMEPAD_STICK_FILTER_HELD;
            }
        }
        /* Either the pad came back to the held position, or the collapse has been
         * confirmed. Either way the guard is over and this report is used. */
        filter->pending_reports = 0;
        accept_despite_hold = true;
    }

    if (!accept_despite_hold && held_magnitude >= GAMEPAD_STICK_GUARD_ARM_MAGNITUDE &&
        gamepad_stick_collapsed(magnitude, held_magnitude)) {
        filter->pending_x = (int16_t) x;
        filter->pending_y = (int16_t) y;
        filter->pending_since_ms = now_ms;
        filter->pending_reports = 1;
        if (out_x != NULL) {
            *out_x = filter->out_x;
        }
        if (out_y != NULL) {
            *out_y = filter->out_y;
        }
        return GAMEPAD_STICK_FILTER_HELD;
    }

    int16_t scaled_x = 0;
    int16_t scaled_y = 0;
    gamepad_stick_scale(x, y, deadzone, &scaled_x, &scaled_y);
    filter->out_x = scaled_x;
    filter->out_y = scaled_y;
    if (out_x != NULL) {
        *out_x = scaled_x;
    }
    if (out_y != NULL) {
        *out_y = scaled_y;
    }
    return GAMEPAD_STICK_FILTER_ACCEPTED;
}

bool gamepad_stick_filter_tick(gamepad_stick_filter_t *filter, uint16_t deadzone, uint32_t now_ms,
                               int16_t *out_x, int16_t *out_y) {
    if (filter == NULL || filter->pending_reports == 0) {
        return false;
    }
    if (!gamepad_stick_elapsed(now_ms, filter->pending_since_ms, GAMEPAD_STICK_GUARD_HOLD_MAX_MS)) {
        return false;
    }
    filter->pending_reports = 0;
    gamepad_stick_scale(filter->pending_x, filter->pending_y, deadzone, &filter->out_x, &filter->out_y);
    if (out_x != NULL) {
        *out_x = filter->out_x;
    }
    if (out_y != NULL) {
        *out_y = filter->out_y;
    }
    return true;
}

static uint32_t gamepad_stick_magnitude(int32_t x, int32_t y) {
    const uint32_t square = (uint32_t) ((int64_t) x * x + (int64_t) y * y);
    uint32_t root = 0;
    for (uint32_t bit = 1u << 15; bit != 0; bit >>= 1) {
        const uint32_t candidate = root + bit;
        if (candidate * candidate <= square) {
            root = candidate;
        }
    }
    return root;
}

static int16_t gamepad_stick_clamp(int32_t value) {
    if (value > GAMEPAD_STICK_FULL_SCALE) {
        return (int16_t) GAMEPAD_STICK_FULL_SCALE;
    }
    if (value < -GAMEPAD_STICK_FULL_SCALE) {
        return (int16_t) -GAMEPAD_STICK_FULL_SCALE;
    }
    return (int16_t) value;
}

static void gamepad_stick_scale(int32_t raw_x, int32_t raw_y, uint16_t deadzone,
                                int16_t *out_x, int16_t *out_y) {
    int16_t x = gamepad_stick_clamp(raw_x);
    int16_t y = gamepad_stick_clamp(raw_y);
    if (deadzone == 0) {
        if (out_x != NULL) {
            *out_x = x;
        }
        if (out_y != NULL) {
            *out_y = y;
        }
        return;
    }
    if (deadzone >= GAMEPAD_STICK_FULL_SCALE) {
        if (out_x != NULL) {
            *out_x = 0;
        }
        if (out_y != NULL) {
            *out_y = 0;
        }
        return;
    }

    const uint32_t magnitude = gamepad_stick_magnitude(x, y);
    if (magnitude <= deadzone) {
        if (out_x != NULL) {
            *out_x = 0;
        }
        if (out_y != NULL) {
            *out_y = 0;
        }
        return;
    }

    /* Stretch what the deadzone left over the whole range. The output is
     * continuous at the deadzone edge, so a stick resting on the edge fades
     * instead of stepping between idle and full strength. */
    int32_t scaled = (int32_t) (((int64_t) (magnitude - deadzone) * GAMEPAD_STICK_FULL_SCALE) /
                                (GAMEPAD_STICK_FULL_SCALE - deadzone));
    if (scaled > GAMEPAD_STICK_FULL_SCALE) {
        scaled = GAMEPAD_STICK_FULL_SCALE;
    }
    if (out_x != NULL) {
        *out_x = gamepad_stick_clamp((int32_t) ((int64_t) x * scaled / (int32_t) magnitude));
    }
    if (out_y != NULL) {
        *out_y = gamepad_stick_clamp((int32_t) ((int64_t) y * scaled / (int32_t) magnitude));
    }
}

static bool gamepad_stick_collapsed(uint32_t magnitude, uint32_t held_magnitude) {
    return held_magnitude >= GAMEPAD_STICK_GUARD_ARM_MAGNITUDE &&
           magnitude <= GAMEPAD_STICK_GUARD_COLLAPSE_MAGNITUDE &&
           magnitude * 2 <= held_magnitude;
}

static bool gamepad_stick_elapsed(uint32_t now_ms, uint32_t since_ms, uint32_t limit_ms) {
    return (uint32_t) (now_ms - since_ms) >= limit_ms;
}
