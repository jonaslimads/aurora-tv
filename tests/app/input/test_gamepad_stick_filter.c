// Settings -> Input -> Stick drift correction has to keep a held direction alive
// through a bad report, keep a resting stick at zero, and still hand the game the
// whole travel. These pin that behaviour on the filter directly.

#include <stddef.h>

#include "unity.h"

#include "input/gamepad_stick_filter.h"

/** The default Aurora deadzone (7%) expressed the way the stream layer passes it. */
#define DZ_7 ((uint16_t) (GAMEPAD_STICK_FULL_SCALE * 7 / 100))

#define T0 1000u
#define T_STEP 8u

static gamepad_stick_filter_t filter;
static uint32_t now;
static int16_t out_x, out_y;

static gamepad_stick_filter_result_t feed(int16_t x, int16_t y) {
    now += T_STEP;
    return gamepad_stick_filter_feed(&filter, x, y, DZ_7, now, &out_x, &out_y);
}

/** Feed the same report until the dropout guard has made up its mind. */
static void feed_until_settled(int16_t x, int16_t y) {
    for (int i = 0; i < GAMEPAD_STICK_GUARD_CONFIRM_REPORTS; i++) {
        feed(x, y);
    }
}

void setUp(void) {
    gamepad_stick_filter_reset(&filter);
    now = T0;
    out_x = 12345;
    out_y = -12345;
}

void tearDown(void) {
}

/** A DS4 that sits slightly off centre must not move the game at all. */
void test_rest_drift_stays_at_zero(void) {
    /* Every pair inside the deadzone radius, the last one just barely. */
    const int16_t wander[][2] = {{600, -600}, {-1400, 700}, {1900, 0}, {-2100, 900}, {300, -300}};
    for (size_t i = 0; i < sizeof(wander) / sizeof(wander[0]); i++) {
        TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_ACCEPTED, feed(wander[i][0], wander[i][1]));
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, out_x, "drift leaked on the X axis");
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, out_y, "drift leaked on the Y axis");
    }
}

/** Diagonal drift is judged by the radius, and whatever survives is tiny. */
void test_diagonal_drift_is_cut_by_the_radius(void) {
    feed_until_settled(1900, 1900);
    TEST_ASSERT_TRUE_MESSAGE(out_x > 0 && out_x < 1900, "diagonal drift was not scaled down");
    TEST_ASSERT_TRUE_MESSAGE(out_x < GAMEPAD_STICK_FULL_SCALE / 20, "diagonal drift steps to a big value");
}

/** Cutting the deadzone must not cost the end of the travel. */
void test_full_deflection_is_still_full(void) {
    feed_until_settled(-GAMEPAD_STICK_FULL_SCALE, 0);
    TEST_ASSERT_EQUAL_INT(-GAMEPAD_STICK_FULL_SCALE, out_x);
    TEST_ASSERT_EQUAL_INT(0, out_y);

    feed_until_settled(0, GAMEPAD_STICK_FULL_SCALE);
    TEST_ASSERT_EQUAL_INT(0, out_x);
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FULL_SCALE, out_y);
}

/** Half travel in must beat a plain subtract and still stay under the raw value. */
void test_travel_above_deadzone_is_rescaled(void) {
    const int16_t half = (int16_t) (GAMEPAD_STICK_FULL_SCALE / 2);
    feed_until_settled(-half, 0);

    /* Plain deadzone would stop at half - DZ_7; rescaling pushes past it without
     * reaching the raw value, so the range is used but never clamped. */
    TEST_ASSERT_TRUE_MESSAGE(out_x < -(half - DZ_7) - 100, "output was not rescaled up");
    TEST_ASSERT_TRUE_MESSAGE(out_x > -half, "output overshot the raw value");
    TEST_ASSERT_EQUAL_INT(0, out_y);
}

/** Sitting on the deadzone edge must fade in, not step - that step is the chatter. */
void test_deadzone_edge_is_continuous(void) {
    feed_until_settled((int16_t) (DZ_7 + 100), 0);
    TEST_ASSERT_TRUE_MESSAGE(out_x > 0, "edge value was cut away");
    TEST_ASSERT_TRUE_MESSAGE(out_x < GAMEPAD_STICK_FULL_SCALE / 20, "edge jumps to a big value");
}

/** The reported bug: holding left, one report comes back centred. */
void test_held_direction_survives_a_dropout(void) {
    feed_until_settled(-GAMEPAD_STICK_FULL_SCALE, 0);
    TEST_ASSERT_EQUAL_INT(-GAMEPAD_STICK_FULL_SCALE, out_x);

    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_HELD, feed(0, 0));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-GAMEPAD_STICK_FULL_SCALE, out_x, "direction was dropped");
    TEST_ASSERT_EQUAL_INT(0, out_y);

    /* The pad is back on the held position; nothing ever reached the host. */
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_ACCEPTED, feed(-GAMEPAD_STICK_FULL_SCALE, 0));
    TEST_ASSERT_EQUAL_INT(-GAMEPAD_STICK_FULL_SCALE, out_x);
}

/** A real release is held back for a couple of reports, then sent. */
void test_confirmed_release_reaches_the_host(void) {
    feed_until_settled(-GAMEPAD_STICK_FULL_SCALE, 0);

    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_HELD, feed(0, 0));
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_HELD, feed(0, 0));
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_ACCEPTED, feed(0, 0));
    TEST_ASSERT_EQUAL_INT(0, out_x);
}

/** Reports only arrive when the stick moves, so a hold has to expire by itself. */
void test_expired_hold_cannot_stick_a_direction(void) {
    feed_until_settled(-GAMEPAD_STICK_FULL_SCALE, 0);
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_HELD, feed(0, 0));

    now += 10;
    TEST_ASSERT_FALSE(gamepad_stick_filter_tick(&filter, DZ_7, now, &out_x, &out_y));

    now += GAMEPAD_STICK_GUARD_HOLD_MAX_MS;
    TEST_ASSERT_TRUE(gamepad_stick_filter_tick(&filter, DZ_7, now, &out_x, &out_y));
    TEST_ASSERT_EQUAL_INT(0, out_x);

    /* Nothing left to expire afterwards. */
    now += 1000;
    TEST_ASSERT_FALSE(gamepad_stick_filter_tick(&filter, DZ_7, now, &out_x, &out_y));
}

/** Slow input is where drift lives; guarding it would eat real movement. */
void test_gentle_direction_change_is_not_held(void) {
    feed_until_settled((int16_t) (GAMEPAD_STICK_FULL_SCALE * 30 / 100), 0);
    TEST_ASSERT_TRUE(out_x > 0);

    const int16_t gentle = (int16_t) (GAMEPAD_STICK_FULL_SCALE * 8 / 100);
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_ACCEPTED, feed(gentle, 0));
    TEST_ASSERT_TRUE(out_x > 0);
    TEST_ASSERT_TRUE(out_x < gentle);
}

/** A snap to the other side is not a dropout and must not be delayed. */
void test_fast_turn_is_not_held(void) {
    feed_until_settled(-GAMEPAD_STICK_FULL_SCALE, 0);
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_ACCEPTED, feed(GAMEPAD_STICK_FULL_SCALE, 0));
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FULL_SCALE, out_x);
}

/** The guard must not invent a deadzone of its own. */
void test_zero_deadzone_passes_travel_through(void) {
    now += T_STEP;
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_ACCEPTED,
                          gamepad_stick_filter_feed(&filter, -12000, 4000, 0, now, &out_x, &out_y));
    TEST_ASSERT_EQUAL_INT(-12000, out_x);
    TEST_ASSERT_EQUAL_INT(4000, out_y);
}

/** Reconnecting a pad must not inherit a hold. */
void test_reset_clears_a_pending_hold(void) {
    feed_until_settled(-GAMEPAD_STICK_FULL_SCALE, 0);
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_HELD, feed(0, 0));

    gamepad_stick_filter_reset(&filter);
    TEST_ASSERT_EQUAL_INT(0, filter.out_x);
    TEST_ASSERT_EQUAL_INT(0, filter.pending_reports);

    now += T_STEP;
    TEST_ASSERT_EQUAL_INT(GAMEPAD_STICK_FILTER_ACCEPTED,
                          gamepad_stick_filter_feed(&filter, 16000, 0, DZ_7, now, &out_x, &out_y));
    TEST_ASSERT_TRUE(out_x > 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rest_drift_stays_at_zero);
    RUN_TEST(test_diagonal_drift_is_cut_by_the_radius);
    RUN_TEST(test_full_deflection_is_still_full);
    RUN_TEST(test_travel_above_deadzone_is_rescaled);
    RUN_TEST(test_deadzone_edge_is_continuous);
    RUN_TEST(test_held_direction_survives_a_dropout);
    RUN_TEST(test_confirmed_release_reaches_the_host);
    RUN_TEST(test_expired_hold_cannot_stick_a_direction);
    RUN_TEST(test_gentle_direction_change_is_not_held);
    RUN_TEST(test_fast_turn_is_not_held);
    RUN_TEST(test_zero_deadzone_passes_travel_through);
    RUN_TEST(test_reset_clears_a_pending_hold);
    return UNITY_END();
}
