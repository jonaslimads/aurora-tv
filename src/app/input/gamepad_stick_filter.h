#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Stabilisation for analog sticks that report badly, opt-in through
 * Settings -> Input -> Stick drift correction.
 *
 * webOS has no DualShock driver: the pad is read through the generic HID path,
 * which neither applies the controller's own centre calibration nor the evdev
 * "flat" filter Linux uses. Two things come out of that on a real DS4, and both
 * were reported on the LG C5:
 *
 *  - the resting value wanders a few percent off centre, so the stick drifts
 *    slowly left and right on its own;
 *  - while a direction is held at the end of its travel, individual reports
 *    sometimes come back near the centre, so the game loses the direction for
 *    one report and the character hitches.
 *
 * The plain deadzone Aurora has always applied covers neither: it cuts inside
 * the radius and passes everything outside through unchanged, so a value that
 * hovers on the edge steps between 0 and full strength, and a held direction is
 * still dropped the moment one bad report arrives.
 *
 * This filter does two jobs:
 *
 *  1. a circular deadzone that *rescales* what is left, so the stick still
 *     reaches full deflection and crossing the edge ramps instead of stepping;
 *  2. a dropout guard: once a stick is held far from the centre, a report that
 *     collapses back to the middle is held off the host until the pad confirms
 *     it, or until the hold expires. Confirmation keeps a real release working,
 *     the expiry keeps a dropped last report from sticking a direction forever.
 *
 * Values are SDL axis units, [-32767, 32767]. A zeroed struct is a valid idle
 * filter, which is what the gamepad state arrays start at and what a
 * disconnected pad is reset to.
 */

/** SDL axis full-scale deflection. */
#define GAMEPAD_STICK_FULL_SCALE 32767

/** Arm the dropout guard only while held this far out (40% of full scale). */
#define GAMEPAD_STICK_GUARD_ARM_MAGNITUDE 13107

/** A report counts as a collapse only inside this radius (30% of full scale). */
#define GAMEPAD_STICK_GUARD_COLLAPSE_MAGNITUDE 9830

/** Collapsing reports in a row needed before the collapse is taken as real. */
#define GAMEPAD_STICK_GUARD_CONFIRM_REPORTS 3

/** Longest a direction is held without confirmation, in milliseconds. */
#define GAMEPAD_STICK_GUARD_HOLD_MAX_MS 40

typedef enum gamepad_stick_filter_result_t {
    /** The report was used; *out_x / *out_y carry the new value. */
    GAMEPAD_STICK_FILTER_ACCEPTED = 0,
    /** The report looked like a dropout; the previous value was kept. */
    GAMEPAD_STICK_FILTER_HELD = 1,
} gamepad_stick_filter_result_t;

typedef struct gamepad_stick_filter_t {
    /** Last value handed out, i.e. what the host currently holds. */
    int16_t out_x, out_y;
    /** Collapsing report kept apart from the output until it is confirmed. */
    int16_t pending_x, pending_y;
    /** SDL ticks of the first collapsing report, 0 when nothing is pending. */
    uint32_t pending_since_ms;
    /** Collapsing reports seen in a row, including the first one. */
    uint8_t pending_reports;
} gamepad_stick_filter_t;

/** Forget everything; call when a controller comes and goes. */
void gamepad_stick_filter_reset(gamepad_stick_filter_t *filter);

/**
 * Feed one stick report through the filter.
 *
 * @param deadzone radius in axis units, 0 to filter nothing but dropouts
 * @param now_ms   monotonic clock in ms (SDL_GetTicks)
 * @param out_x    value to send for X, valid on every return
 * @param out_y    value to send for Y, valid on every return
 */
gamepad_stick_filter_result_t gamepad_stick_filter_feed(gamepad_stick_filter_t *filter,
                                                        int16_t raw_x, int16_t raw_y,
                                                        uint16_t deadzone, uint32_t now_ms,
                                                        int16_t *out_x, int16_t *out_y);

/**
 * Let an unconfirmed hold expire. Reports only arrive when the stick moves, so
 * a pad that stops talking must not leave a direction held: call this on every
 * frame with the current tick.
 *
 * @return true when the hold expired and *out_x / *out_y now carry a new value
 */
bool gamepad_stick_filter_tick(gamepad_stick_filter_t *filter, uint16_t deadzone, uint32_t now_ms,
                               int16_t *out_x, int16_t *out_y);
