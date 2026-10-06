#pragma once

#include <stdbool.h>
#include <SDL_joystick.h>
#include <SDL_gamecontroller.h>

typedef struct app_input_t app_input_t;
typedef struct app_gamepad_state_t app_gamepad_state_t;

/**
 * Bits of app_settings_t::gamepad_compat. Each one drops a single writer that talks the
 * DualSense protocol to a pad which only copied its descriptors (054c:0ce6). On such a
 * pad any of these can make the firmware re-enumerate on the USB bus a second later,
 * which looks exactly like a controller that keeps disconnecting on its own.
 */
typedef enum gamepad_compat_t {
    /** Skip our SDL_GameControllerSetPlayerIndex() call for this pad. */
    GAMEPAD_COMPAT_NO_PLAYER_LED = 0x01,
    /** Forbid SDL's HIDAPI PS5 driver from sending rumble/player-LED output reports. */
    GAMEPAD_COMPAT_NO_SDL_DUALSENSE_REPORTS = 0x02,
    /** Skip our own /dev/hidraw DualSense feedback handle (rumble, lightbar, triggers). */
    GAMEPAD_COMPAT_NO_USB_FEEDBACK = 0x04,
    /** Skip the exclusive EVIOCGRAB on the controller's touchpad node. */
    GAMEPAD_COMPAT_NO_TOUCHPAD_GRAB = 0x08,
    GAMEPAD_COMPAT_ALL = 0x0f,
} gamepad_compat_t;

/**
 * Push the compat bits into the SDL hints SDL consults when it opens a pad, and log the
 * active mask. Call before the first device open and again per hotplug: the HIDAPI PS5
 * driver reads the hints while opening, so setting them later would not un-write a pad
 * that is already bound.
 */
void app_input_apply_gamepad_compat(void);

/** True when the compat mask asks us not to touch @p flag for this pad. */
bool app_input_gamepad_compat(int flag);

/**
 * Open the SDL joystick at @p device_index and give it a controller slot.
 *
 * @param notify raise the on-screen notice for the new controller. Hotplug does;
 *               the scans at startup and at stream start do not, because those pads
 *               were already in the room and toasting them is noise, not news.
 */
bool app_input_init_gamepad(app_input_t *input, int device_index, bool notify);

void app_input_close_gamepad(app_input_t *input, SDL_JoystickID sdl_id);

/**
 * Open any SDL joysticks that are present but not tracked yet.
 * Needed when JOYDEVICEADDED was missed (common with a second BT DualSense on webOS).
 * @return number of newly opened gamepads
 */
int app_input_scan_gamepads(app_input_t *input);

int app_input_get_gamepads_count(app_input_t *input);

short app_input_get_max_gamepads(app_input_t *input);

short app_input_gamepads_mask(app_input_t *input);

void app_input_gamepad_rumble(app_input_t *input, unsigned short controllerNumber, unsigned short lowFreqMotor,
                              unsigned short highFreqMotor);

void app_input_gamepad_rumble_triggers(app_input_t *input, unsigned short controllerNumber, unsigned short leftTrigger,
                                       unsigned short rightTrigger);

void app_input_gamepad_set_motion_event_state(app_input_t *input, unsigned short controllerNumber, uint8_t motionType,
                                              uint16_t reportRateHz);

void app_input_gamepad_set_controller_led(app_input_t *input, unsigned short controllerNumber, uint8_t r, uint8_t g,
                                          uint8_t b);

void app_input_gamepad_set_adaptive_triggers(app_input_t *input, unsigned short controllerNumber, uint8_t eventFlags,
                                             uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right);

void app_input_gamepad_set_player_led(app_input_t *input, unsigned short controllerNumber, uint8_t ledValue);

void app_input_gamepad_set_mic_led(app_input_t *input, unsigned short controllerNumber, uint8_t ledState);

app_gamepad_state_t * app_input_gamepad_state_init(app_input_t *input, SDL_GameController *controller);
void app_input_gamepad_state_deinit(app_gamepad_state_t *state);

app_gamepad_state_t *app_input_gamepad_state_by_index(app_input_t *input, int index);

app_gamepad_state_t *app_input_gamepad_state_by_instance_id(app_input_t *input, SDL_JoystickID instance_id);

/** Host controllerNumber is gs_id, not the sparse gamepads[] slot index. */
app_gamepad_state_t *app_input_gamepad_state_by_gs_id(app_input_t *input, unsigned short gs_id);