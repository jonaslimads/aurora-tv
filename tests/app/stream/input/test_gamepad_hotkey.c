// Aurora keeps the L1+R3 / R1+R3 chords for itself: they must neither reach the
// host nor be mapped onto a UI key. These tests pin that contract, including the
// bookkeeping that stops the host from being left holding the shoulder button
// that opened the chord.

#include <SDL.h>
#include "unity.h"

#include "app.h"
#include "input/app_input.h"
#include "stream/input/session_input.h"
#include "util/bus.h"
#include "util/user_event.h"

#define TEST_INSTANCE_ID 7

static app_input_t app_input;
static stream_input_t session_input;

void setUp(void) {
    if (SDL_WasInit(SDL_INIT_EVENTS) == 0) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, SDL_Init(SDL_INIT_EVENTS), SDL_GetError());
    }
    SDL_PumpEvents();
    SDL_FlushEvents(SDL_USEREVENT, SDL_USEREVENT);

    SDL_memset(&app_input, 0, sizeof(app_input));
    app_input.max_num_gamepads = 4;
    for (size_t i = 0; i < app_input.max_num_gamepads; i++) {
        app_input.gamepads[i].instance_id = -1;
        app_input.gamepads[i].gs_id = -1;
    }
    app_input.gamepads[0].instance_id = TEST_INSTANCE_ID;
    app_input.gamepads[0].gs_id = 0;

    SDL_memset(&session_input, 0, sizeof(session_input));
    session_input.input = &app_input;
    /* View-only keeps every Limelight send off the floor: what matters here is
     * which events Aurora keeps, and the button mask it leaves for the host. */
    session_input.view_only = true;
}

void tearDown(void) {
}

/** Feed one button event to the hotkey handler the way the event funnel does. */
static bool send_button(Uint8 button, Uint8 state) {
    SDL_ControllerButtonEvent event;
    SDL_memset(&event, 0, sizeof(event));
    event.type = state == SDL_PRESSED ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
    event.which = TEST_INSTANCE_ID;
    event.button = button;
    event.state = state;
    return stream_input_gamepad_hotkey(&session_input, &event);
}

/** The user event the shortcut asked the UI to run, or -1 when none was queued. */
static int pop_hotkey_user_event(void) {
    SDL_Event event;
    int code = -1;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_USEREVENT, SDL_USEREVENT) > 0) {
        if (event.user.code == BUS_INT_EVENT_ACTION) {
            continue;
        }
        code = (int) event.user.code;
    }
    return code;
}

/** Mirror what stream_input_handle_cbutton does with a button it forwards. */
static void host_forward_press(int button_flag) {
    app_input.gamepads[0].buttons |= button_flag;
}

void test_single_r3_is_not_a_shortcut(void) {
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_PRESSED));
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_RELEASED));
    TEST_ASSERT_EQUAL_INT(-1, pop_hotkey_user_event());
}

void test_single_shoulder_is_not_a_shortcut(void) {
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_PRESSED));
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_PRESSED));
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_RELEASED));
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_RELEASED));
    TEST_ASSERT_EQUAL_INT(-1, pop_hotkey_user_event());
}

void test_unrelated_button_is_not_consumed(void) {
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_A, SDL_PRESSED));
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_A, SDL_RELEASED));
    TEST_ASSERT_EQUAL_INT(-1, pop_hotkey_user_event());
}

void test_l1_r3_toggles_virtual_mouse(void) {
    host_forward_press(LB_FLAG);
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_PRESSED));

    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_PRESSED));
    TEST_ASSERT_EQUAL_INT(USER_TOGGLE_VMOUSE, pop_hotkey_user_event());

    /* The rest of the chord is Aurora's as well. */
    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_RELEASED));
    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_RELEASED));
    TEST_ASSERT_EQUAL_INT(-1, pop_hotkey_user_event());
}

void test_r1_r3_toggles_soft_keyboard(void) {
    host_forward_press(RB_FLAG);
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_PRESSED));

    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_PRESSED));
    TEST_ASSERT_EQUAL_INT(USER_TOGGLE_SOFT_KEYBOARD, pop_hotkey_user_event());

    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_RELEASED));
    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_RELEASED));
    TEST_ASSERT_EQUAL_INT(-1, pop_hotkey_user_event());
}

void test_chord_does_not_leave_shoulder_held(void) {
    host_forward_press(RB_FLAG);
    send_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_PRESSED);
    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_PRESSED));

    /* Releases are consumed, so nothing else would lift the button: the shortcut
     * itself has to leave the host with no shoulder button down. */
    TEST_ASSERT_EQUAL_INT(0, app_input.gamepads[0].buttons & (LB_FLAG | RB_FLAG));
    TEST_ASSERT_EQUAL_INT(0, app_input.gamepads[0].buttons & RS_CLK_FLAG);
}

void test_l1_wins_when_both_shoulders_are_held(void) {
    send_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_PRESSED);
    send_button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_PRESSED);

    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_PRESSED));
    TEST_ASSERT_EQUAL_INT(USER_TOGGLE_VMOUSE, pop_hotkey_user_event());
}

void test_chord_repeats_and_rearm(void) {
    send_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_PRESSED);
    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_PRESSED));
    TEST_ASSERT_EQUAL_INT(USER_TOGGLE_VMOUSE, pop_hotkey_user_event());

    /* A held chord that SDL reports twice must not fire a second toggle. */
    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_PRESSED));
    TEST_ASSERT_EQUAL_INT(-1, pop_hotkey_user_event());

    /* Once every chord button is up the shortcut is rearmed. */
    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_RELEASED));
    TEST_ASSERT_TRUE(send_button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_RELEASED));
    TEST_ASSERT_FALSE(send_button(SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_PRESSED));
    TEST_ASSERT_EQUAL_INT(-1, pop_hotkey_user_event());
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_single_r3_is_not_a_shortcut);
    RUN_TEST(test_single_shoulder_is_not_a_shortcut);
    RUN_TEST(test_unrelated_button_is_not_consumed);
    RUN_TEST(test_l1_r3_toggles_virtual_mouse);
    RUN_TEST(test_r1_r3_toggles_soft_keyboard);
    RUN_TEST(test_chord_does_not_leave_shoulder_held);
    RUN_TEST(test_l1_wins_when_both_shoulders_are_held);
    RUN_TEST(test_chord_repeats_and_rearm);
    const int result = UNITY_END();
    SDL_Quit();
    return result;
}
