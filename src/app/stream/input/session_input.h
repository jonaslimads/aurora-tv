#pragma once

#include <stdbool.h>
#include <Limelight.h>
#include <SDL_events.h>
#include <SDL_timer.h>

#include "config.h"
#include "input/input_gamepad.h"

#if FEATURE_INPUT_EVMOUSE

#include "session_evmouse.h"

#endif

typedef struct app_input_t app_input_t;
typedef struct session_config_t session_config_t;
typedef struct session_t session_t;

typedef struct session_input_touchpad_t session_input_touchpad_t;

typedef struct session_input_vmouse_t {
    struct {
        bool active;
        short x, y;
        short scroll_x, scroll_y;
        bool l, r;
        bool modifier;
    } state;
    SDL_TimerID timer_id;
} session_input_vmouse_t;

typedef struct stream_input_t {
    session_t *session;
    app_input_t *input;
    bool started;
    uint16_t announcedGamepadMask;
    bool remoteOkPressed;
    uint32_t remoteOkPressedAt;
    char remoteOkModifiers;
    bool pointerGestureActive;
    bool pointerGestureDragging;
    bool pointerGestureLeftDown;
    uint32_t pointerGesturePressTime;
    int pointerGestureStartX;
    int pointerGestureStartY;
    bool view_only, no_sdl_mouse;
    uint8_t stick_deadzone;
    /** Settings -> Input -> Stick drift correction, see gamepad_stick_filter.h. */
    bool stick_drift_correction;
    /** Last time the held gamepad state was repeated to the host, see stream_input_update_gamepad_stability. */
    uint32_t gamepad_state_refresh_ms;
    bool report_gamepad_battery;
    uint8_t touchpad_mode;
    bool touchpad_multitouch;
    short touchpad_count;
    float touchpad_mouse_gain;
    float touchpad_scroll_scale;
    session_input_vmouse_t vmouse;
    session_input_touchpad_t *touchpads;
#if FEATURE_INPUT_EVMOUSE
    session_evmouse_t evmouse;
#endif
} stream_input_t;

void session_input_init(stream_input_t *input, session_t *session, app_input_t *app_input,
                        const session_config_t *settings);

void session_input_deinit(stream_input_t *input);

void session_input_interrupt(stream_input_t *input);

void session_input_started(stream_input_t *input);

void session_input_stopped(stream_input_t *input);

void stream_input_touchpad_mouse_init(stream_input_t *input);

void stream_input_touchpad_mouse_deinit(stream_input_t *input);

void session_input_screen_keyboard_opened(stream_input_t *input);

void session_input_screen_keyboard_closed(stream_input_t *input);

/** Release any keys still marked down (host + local state). Call after soft keyboard closes to fix stuck input. */
void stream_input_flush_pressed_keys(stream_input_t *input);

void stream_input_send_gamepad_arrive(stream_input_t *input, app_gamepad_state_t *gamepad);

void stream_input_send_gamepad_remove(stream_input_t *input, app_gamepad_state_t *gamepad);

void stream_input_handle_key(stream_input_t *input, const SDL_KeyboardEvent *event);

void stream_input_handle_text(stream_input_t *input, const SDL_TextInputEvent *event);

/** Send a keyboard event directly to the host (for soft keyboard). Bypasses SDL. */
void stream_input_send_key_event(stream_input_t *input, short keyCode, bool keyDown, char modifiers);

void stream_input_handle_cbutton(stream_input_t *input, const SDL_ControllerButtonEvent *event);

/**
 * Aurora's own gamepad shortcuts: L1+R3 toggles the virtual mouse, R1+R3 toggles
 * the on-screen keyboard. Call this before any other use of a button event -- the
 * chord has to be resolved and swallowed before forwarding, otherwise the same
 * press also acts inside the streamed game.
 *
 * @return true when Aurora consumed the event (do not forward, do not map to a key)
 */
bool stream_input_gamepad_hotkey(stream_input_t *input, const SDL_ControllerButtonEvent *event);

/**
 * Apply the stick filter chain (plain deadzone, or drift correction when enabled)
 * to the stick values currently held in the gamepad state. Called for every axis
 * report, before the values reach the virtual mouse or the host.
 */
void stream_input_filter_gamepad_sticks(stream_input_t *input, app_gamepad_state_t *gamepad);

/**
 * Keep the forwarded gamepad state honest while drift correction is on:
 * expire stick holds that never got confirmed, and repeat the state of a pad
 * that is being worked on. Axis reports only arrive when the stick moves, so
 * the main loop has to drive this. Safe to call every frame.
 */
void stream_input_update_gamepad_stability(stream_input_t *input);

void stream_input_handle_caxis(stream_input_t *input, const SDL_ControllerAxisEvent *event);

void stream_input_handle_csensor(stream_input_t *input, const SDL_ControllerSensorEvent *event);

void stream_input_handle_ctouchpad(stream_input_t *input, const SDL_ControllerTouchpadEvent *event);

void stream_input_handle_cdevice(stream_input_t *input, const SDL_ControllerDeviceEvent *event);

void stream_input_update_touchpad_tap_hold(stream_input_t *input);

void stream_input_handle_jdevice(stream_input_t *input, const SDL_JoyDeviceEvent *event);

void stream_input_handle_mmotion(stream_input_t *input, const SDL_MouseMotionEvent *event, bool hw_mouse);

void stream_input_handle_mbutton(stream_input_t *input, const SDL_MouseButtonEvent *event);

void stream_input_handle_mwheel(stream_input_t *input, const SDL_MouseWheelEvent *event);

void stream_input_handle_touch(const stream_input_t *input, const SDL_TouchFingerEvent *event);