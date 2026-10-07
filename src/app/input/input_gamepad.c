#include "input_gamepad.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <SDL_timer.h>
#include <SDL_version.h>
#include <Limelight.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>

#include "logging.h"
#include "app_input.h"
#include "util/bus.h"
#include "util/user_event.h"
#if FEATURE_GAMEPAD_TOUCHPAD_GRAB
#include "gamepad_touchpad.h"

/**
 * How long a pad must stay put before we take its touchpad node. The removals seen on a
 * C5 with a GameSir G8 landed 1.0-2.0 s after the connect that grabbed the node at +30 ms,
 * so this waits past that window. Long enough that webOS finishes with the device, short
 * enough that nobody notices a swipe being eaten at the very start of plugging it in.
 */
#define TOUCHPAD_GRAB_SETTLE_MS 3000
#endif

#ifdef TARGET_WEBOS
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include <linux/input.h>
#endif

static int new_gamepad_state_index(app_input_t *input, SDL_GameController *controller);

static short next_gamepad_gs_id(app_input_t *input);

static bool is_same_gamepad(const app_gamepad_state_t *state, SDL_GameController *controller);

static void app_input_notify_gamepad_presence(short gs_id, bool present);

static void app_input_log_removal_context(const app_gamepad_state_t *state, SDL_JoystickID sdl_id);

#ifdef TARGET_WEBOS
static bool str_contains_ci(const char *haystack, const char *needle);
static bool webos_name_is_non_gamepad(const char *name);
static bool webos_should_ignore_controller_like_device(const char *name, const char *guidstr, SDL_Joystick *joystick);
#endif

bool app_input_init_gamepad(app_input_t *input, int device_index, bool notify) {
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(device_index);
    char guidstr[33];
    SDL_JoystickGetGUIDString(guid, guidstr, 33);
    const char *name = SDL_JoystickNameForIndex(device_index);
#if SDL_VERSION_ATLEAST(2, 0, 6)
    /* Scan at init/stream start plus JOY/CONTROLLER DEVICEADDED can all see the
     * same pad. Bail before Open if we already track this instance. */
    {
        SDL_JoystickID existing_id = SDL_JoystickGetDeviceInstanceID(device_index);
        if (existing_id >= 0 && app_input_gamepad_state_by_instance_id(input, existing_id) != NULL) {
            return false;
        }
    }
#endif
    if (SDL_IsGameController(device_index)) {
        SDL_GameController *controller = SDL_GameControllerOpen(device_index);
        if (!controller) {
            commons_log_error("Input", "Could not open gamecontroller %i. GUID: %s, error: %s", device_index, guidstr,
                              SDL_GetError());
            return false;
        }
        SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
#ifdef TARGET_WEBOS
        if (webos_should_ignore_controller_like_device(name, guidstr, joystick)) {
            SDL_GameControllerClose(controller);
            return false;
        }
#endif
#if SDL_VERSION_ATLEAST(2, 0, 6)
        /* SDL_GameControllerOpen on an already-open device returns the same
         * handle. Without this check we allocate a second gs_id for one pad
         * (host shows P1 twice; P2 needs double reconnect). Do not Close. */
        {
            SDL_JoystickID sdl_id = SDL_JoystickInstanceID(joystick);
            if (sdl_id >= 0 && app_input_gamepad_state_by_instance_id(input, sdl_id) != NULL) {
                return false;
            }
        }
#endif

        app_gamepad_state_t *state = app_input_gamepad_state_init(input, controller);
        if (state == NULL) {
            SDL_GameControllerClose(controller);
            return false;
        }
        assert(state->gs_id >= 0);
#if FEATURE_GAMEPAD_TOUCHPAD_GRAB
        // Keep the platform's input stack away from the controller touchpad, so it
        // can't consume swipes as system gestures. SDL's own touchpad events are
        // unaffected.
        //
        // Deliberately not taken here. An EVIOCGRAB issued within ~100 ms of the device
        // appearing lands while webOS' own input service is still enumerating that same
        // node, and on a pad that borrowed its descriptors from someone else (a GameSir
        // G8 reporting itself as a Sony DualSense) the device then re-enumerates on the
        // bus 1-2 s later: connect, grab, gone, connect. Take it once the pad has proven
        // it is staying - see app_input_update_gamepad_touchpad_grabs().
        state->touchpad_grab_due_ms = SDL_GetTicks() + TOUCHPAD_GRAB_SETTLE_MS;
        commons_log_info("Input", "Controller #%d touchpad grab deferred %d ms", state->gs_id,
                         TOUCHPAD_GRAB_SETTLE_MS);
#endif
        input->activeGamepadMask |= 1 << state->gs_id;
        input->gamepads_count++;
        if (notify) {
            /* Only hotplug is news; the scans at startup and at stream start walk in
             * pads that were already in the TV, and toasting those is noise. */
            app_input_notify_gamepad_presence(state->gs_id, true);
        }
        return true;
    } else {
        commons_log_warn("Input", "Unrecognized game controller %s. GUID: %s", name, guidstr);
    }
    return false;
}

/**
 * Take touchpad grabs that have matured. Called from the main loop, so the deferral works
 * in the launcher as well as in a stream. A pad that re-enumerated before its delay
 * elapsed is never grabbed while it is unstable: its new connect arms a fresh delay.
 */
void app_input_update_gamepad_touchpad_grabs(app_input_t *input) {
#if FEATURE_GAMEPAD_TOUCHPAD_GRAB
    if (input->gamepads_count == 0) {
        return;
    }
    Uint32 now = SDL_GetTicks();
    for (int i = 0; i < input->max_num_gamepads; i++) {
        app_gamepad_state_t *state = &input->gamepads[i];
        if (state->controller == NULL || state->touchpad != NULL || state->touchpad_grab_due_ms == 0) {
            continue;
        }
        if ((Sint32) (now - state->touchpad_grab_due_ms) < 0) {
            continue;
        }
        state->touchpad_grab_due_ms = 0;
        state->touchpad = gamepad_touchpad_grab(state->controller);
        commons_log_info("Input", "Controller #%d touchpad grab taken %d ms after it appeared", state->gs_id,
                         TOUCHPAD_GRAB_SETTLE_MS);
    }
#else
    (void) input;
#endif
}

int app_input_scan_gamepads(app_input_t *input) {
    int opened = 0;
    int num = SDL_NumJoysticks();
    for (int device_index = 0; device_index < num; device_index++) {
        if (app_input_get_gamepads_count(input) >= app_input_get_max_gamepads(input)) {
            break;
        }
#if SDL_VERSION_ATLEAST(2, 0, 6)
        SDL_JoystickID instance_id = SDL_JoystickGetDeviceInstanceID(device_index);
        if (instance_id >= 0 && app_input_gamepad_state_by_instance_id(input, instance_id) != NULL) {
            continue;
        }
#endif
        if (app_input_init_gamepad(input, device_index, false)) {
            opened++;
        }
    }
    /* Drop ADDED events queued while we opened during scan — otherwise the
     * event loop re-opens / re-announces the same DualSense as a second pad. */
    SDL_FlushEvent(SDL_JOYDEVICEADDED);
    SDL_FlushEvent(SDL_CONTROLLERDEVICEADDED);
    if (opened > 0) {
        commons_log_info("Input", "Gamepad scan opened %d additional controller(s); mask=0x%x count=%d",
                         opened, input->activeGamepadMask, app_input_get_gamepads_count(input));
    }
    return opened;
}

void app_input_close_gamepad(app_input_t *input, SDL_JoystickID sdl_id) {
    app_gamepad_state_t *state = app_input_gamepad_state_by_instance_id(input, sdl_id);
    if (!state) {
        return;
    }
    assert(state->gs_id >= 0);
    if (!state->controller) {
        commons_log_warn("Input", "Could not find gamecontroller %i: %s", sdl_id, SDL_GetError());
        return;
    }
    // Reduce number of connected gamepads
    input->gamepads_count--;
    // Remove gamepad mask
    input->activeGamepadMask &= ~(1 << state->gs_id);
#if !SDL_VERSION_ATLEAST(2, 0, 9)
    if (state->haptic) {
        SDL_HapticClose(state->haptic);
    }
#endif
#if FEATURE_GAMEPAD_TOUCHPAD_GRAB
    gamepad_touchpad_release(state->touchpad);
    state->touchpad = NULL;
#endif
    app_input_log_removal_context(state, sdl_id);
    SDL_GameControllerClose(state->controller);
    commons_log_info("Input", "Controller #%d disconnected, sdl_id: %d", state->gs_id, sdl_id);
    app_input_notify_gamepad_presence(state->gs_id, false);
    app_input_gamepad_state_deinit(state);
}

/** Tell every UI fragment that this controller slot gained or lost a device. */
static void app_input_notify_gamepad_presence(short gs_id, bool present) {
    bus_pushevent(USER_GAMEPAD_PRESENT, (void *) (intptr_t) gs_id, (void *) (intptr_t) (present ? 1 : 0));
}

#ifdef TARGET_WEBOS
/** Every /dev/input node the app can reach, with the HID name behind it. */
static void app_input_log_dev_input_nodes(void) {
    DIR *dir = opendir("/dev/input");
    if (dir == NULL) {
        commons_log_warn("Input", "Removal context: /dev/input is not readable (%s)", strerror(errno));
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "event", 5) != 0) {
            continue;
        }
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        struct stat st;
        if (stat(path, &st) != 0) {
            commons_log_warn("Input", "Removal context: %s cannot be stat'ed (%s)", path, strerror(errno));
            continue;
        }
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            commons_log_warn("Input", "Removal context: %s exists (dev %u:%u) but will not open (%s)", path,
                             major(st.st_rdev), minor(st.st_rdev), strerror(errno));
            continue;
        }
        char name[128];
        memset(name, 0, sizeof(name));
        if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0) {
            snprintf(name, sizeof(name), "(no name)");
        }
        close(fd);
        commons_log_info("Input", "Removal context: %s exists (dev %u:%u) name='%s'", path, major(st.st_rdev),
                         minor(st.st_rdev), name);
    }
    closedir(dir);
}
#endif

/**
 * What SDL still sees, taken while the removal is being handled.
 *
 * A pad that flaps needs one question answered before anything is fixed: did the
 * device node go away (USB/HID re-enumeration, which the app can only paper over),
 * or is the node untouched and SDL alone decided the pad died (a read error or a
 * udev event, which is ours to handle differently)? Both answers come out of this
 * snapshot, and a flapping pad repeats it every few seconds on its own.
 *
 * Only cached SDL state is read here. SDL_JoystickNameForIndex is deliberately not
 * called: it can briefly touch the device, which would perturb what we measure.
 */
static void app_input_log_removal_context(const app_gamepad_state_t *state, SDL_JoystickID sdl_id) {
    int num = SDL_NumJoysticks();
    bool still_listed = false;
    char listed[192];
    size_t used = 0;
    listed[0] = '\0';
    for (int i = 0; i < num; i++) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        still_listed = still_listed || id == sdl_id;
        int written = snprintf(listed + used, sizeof(listed) - used, " [%d] id=%d%s", i, (int) id,
                              SDL_IsGameController(i) ? "" : " (no gamepad mapping)");
        if (written < 0 || (size_t) written >= sizeof(listed) - used) {
            break;
        }
        used += (size_t) written;
    }
    commons_log_warn("Input",
                     "Controller #%d (sdl_id %d, node %s) removal: SDL lists %d device(s), this id is %s%s",
                     state->gs_id, (int) sdl_id, state->device_path[0] != '\0' ? state->device_path : "(unknown)", num,
                     still_listed ? "STILL PRESENT" : "gone", listed);
#if FEATURE_GAMEPAD_TOUCHPAD_GRAB
    if (state->touchpad != NULL) {
        commons_log_warn("Input", "Controller #%d held an exclusive touchpad grab when it was removed", state->gs_id);
    } else if (state->touchpad_grab_due_ms != 0) {
        commons_log_info("Input", "Controller #%d was removed before its deferred touchpad grab was taken", state->gs_id);
    }
#endif
#ifdef TARGET_WEBOS
    app_input_log_dev_input_nodes();
#endif
}

app_gamepad_state_t *app_input_gamepad_state_init(app_input_t *input, SDL_GameController *controller) {
    int index = new_gamepad_state_index(input, controller);
    if (index < 0) {
        return NULL;
    }
    app_gamepad_state_t *state = &input->gamepads[index];
    if (state->gs_id < 0) {
        short gsId = next_gamepad_gs_id(input);
        if (gsId < 0) {
            return NULL;
        }
        state->gs_id = gsId;
    }
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    SDL_JoystickID sdl_id = SDL_JoystickInstanceID(joystick);

    SDL_Haptic *haptic = SDL_HapticOpenFromJoystick(joystick);
    unsigned int haptic_bits = SDL_HapticQuery(haptic);
    commons_log_debug("Input", "Controller #%d has supported haptic bits: %x", state->gs_id, haptic_bits);
    if (haptic && (haptic_bits & SDL_HAPTIC_LEFTRIGHT) == 0) {
        SDL_HapticClose(haptic);
        haptic = NULL;
    }
    state->instance_id = sdl_id;
    state->controller = controller;
    state->guid = SDL_JoystickGetGUID(joystick);
#if SDL_VERSION_ATLEAST(2, 24, 0)
    {
        const char *path = SDL_JoystickPath(joystick);
        if (path != NULL) {
            snprintf(state->device_path, sizeof(state->device_path), "%s", path);
        }
    }
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const char *serial = SDL_JoystickGetSerial(joystick);
    state->serial_crc = serial != NULL ? SDL_crc32(0, (const void *) serial, strlen(serial)) : 0;
#endif
#if SDL_VERSION_ATLEAST(2, 0, 12)
    SDL_GameControllerSetPlayerIndex(controller, state->gs_id);
#endif
#if !SDL_VERSION_ATLEAST(2, 0, 9)
    state->haptic = haptic;
    state->haptic_effect_id = -1;
#endif
#if TARGET_WEBOS
    state->ds_usb = dualsense_usb_open(controller);
#endif
    commons_log_info("Input", "Controller #%d (%s) connected", state->gs_id,
                     SDL_JoystickName(joystick));
#if SDL_VERSION_ATLEAST(2, 24, 0)
    const char *path = SDL_JoystickPath(joystick);
    commons_log_info("Input", "Controller #%d device: %s, rumble: %s, writable: %s", state->gs_id,
                     path != NULL ? path : "(unknown)",
                     SDL_GameControllerHasRumble(controller) ? "yes" : "no",
                     path == NULL ? "unknown" : (access(path, W_OK) == 0 ? "yes" : "NO"));
#endif
    return state;
}

void app_input_gamepad_state_deinit(app_gamepad_state_t *state) {
#if TARGET_WEBOS
    dualsense_usb_close(state->ds_usb);
    state->ds_usb = NULL;
#endif
    short gsId = state->gs_id;
    SDL_JoystickGUID guid = state->guid;
    uint32_t serialCrc = state->serial_crc;
    memset(state, 0, sizeof(app_gamepad_state_t));
    // Set to invalid ID so it can be reused
    state->instance_id = -1;
    // Restore the ID so if the same controller is reconnected it will be assigned the same ID
    state->gs_id = gsId;
    state->guid = guid;
    state->serial_crc = serialCrc;
}

app_gamepad_state_t *app_input_gamepad_state_by_index(app_input_t *input, int index) {
    if (index < 0 || index >= input->max_num_gamepads || input->gamepads[index].instance_id == -1) {
        return NULL;
    }
    return &input->gamepads[index];
}

app_gamepad_state_t *app_input_gamepad_state_by_instance_id(app_input_t *input, SDL_JoystickID instance_id) {
    for (short i = 0; i < (short) input->max_num_gamepads; i++) {
        app_gamepad_state_t *gamepad = &input->gamepads[i];
        if (gamepad->instance_id == instance_id) {
            return gamepad;
        }
    }
    return NULL;
}

app_gamepad_state_t *app_input_gamepad_state_by_gs_id(app_input_t *input, unsigned short gs_id) {
    for (short i = 0; i < (short) input->max_num_gamepads; i++) {
        app_gamepad_state_t *gamepad = &input->gamepads[i];
        if (gamepad->instance_id != -1 && gamepad->gs_id == (short) gs_id) {
            return gamepad;
        }
    }
    return NULL;
}

int app_input_get_gamepads_count(app_input_t *input) {
    return (int) input->gamepads_count;
}

short app_input_get_max_gamepads(app_input_t *input) {
    return (short) input->max_num_gamepads;
}

short app_input_gamepads_mask(app_input_t *input) {
    return input->activeGamepadMask;
}

void app_input_gamepad_rumble(app_input_t *input, unsigned short controller_id,
                              unsigned short low_freq_motor, unsigned short high_freq_motor) {
    app_gamepad_state_t *state = app_input_gamepad_state_by_gs_id(input, controller_id);
    if (state == NULL || state->controller == NULL) {
        return;
    }

#if SDL_VERSION_ATLEAST(2, 0, 9)
    if (!state->rumble_requested) {
        state->rumble_requested = true;
        commons_log_info("Input", "Controller #%d first rumble request: low %u, high %u", state->gs_id,
                         low_freq_motor, high_freq_motor);
    }
    if (SDL_GameControllerRumble(state->controller, low_freq_motor, high_freq_motor, SDL_HAPTIC_INFINITY) != 0 &&
        !state->rumble_failed) {
        state->rumble_failed = true;
        commons_log_warn("Input", "Controller #%d cannot rumble: %s", state->gs_id, SDL_GetError());
    }
#else
    SDL_Haptic *haptic = state->haptic;
    if (!haptic) {
        return;
    }

    if (state->haptic_effect_id >= 0) {
        SDL_HapticDestroyEffect(haptic, state->haptic_effect_id);
    }

    if (low_freq_motor == 0 && high_freq_motor == 0) {
        return;
    }

    SDL_HapticEffect effect;
    SDL_memset(&effect, 0, sizeof(effect));
    effect.type = SDL_HAPTIC_LEFTRIGHT;
    effect.leftright.length = SDL_HAPTIC_INFINITY;

    // SDL haptics range from 0-32767 but XInput uses 0-65535, so divide by 2 to correct for SDL's scaling
    effect.leftright.large_magnitude = low_freq_motor / 2;
    effect.leftright.small_magnitude = high_freq_motor / 2;

    state->haptic_effect_id = SDL_HapticNewEffect(haptic, &effect);
    if (state->haptic_effect_id >= 0) {
        SDL_HapticRunEffect(haptic, state->haptic_effect_id, 1);
    }
#endif
}


void app_input_gamepad_rumble_triggers(app_input_t *input, unsigned short controllerNumber, unsigned short leftTrigger,
                                       unsigned short rightTrigger) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    app_gamepad_state_t *state = app_input_gamepad_state_by_gs_id(input, controllerNumber);
    if (state == NULL || state->controller == NULL) {
        return;
    }
    SDL_GameControllerRumbleTriggers(state->controller, leftTrigger, rightTrigger, SDL_HAPTIC_INFINITY);
#else
    (void) input;
    (void) controllerNumber;
    (void) leftTrigger;
    (void) rightTrigger;
#endif
}

void app_input_gamepad_set_motion_event_state(app_input_t *input, unsigned short controllerNumber, uint8_t motionType,
                                              uint16_t reportRateHz) {
#if SDL_VERSION_ATLEAST(2, 0, 14)
    app_gamepad_state_t *gamepad = app_input_gamepad_state_by_gs_id(input, controllerNumber);
    if (gamepad == NULL || gamepad->controller == NULL) {
        return;
    }
    SDL_SensorType sensor_type = SDL_SENSOR_INVALID;
    int rate_slot = -1;
    app_gamepad_sensor_state_t *sensor_state = NULL;
    switch (motionType) {
        case LI_MOTION_TYPE_ACCEL:
            sensor_type = SDL_SENSOR_ACCEL;
            rate_slot = 0;
            sensor_state = &gamepad->accelState;
            break;
        case LI_MOTION_TYPE_GYRO:
            sensor_type = SDL_SENSOR_GYRO;
            rate_slot = 1;
            sensor_state = &gamepad->gyroState;
            break;
        default:
            break;
    }
    if (sensor_type == SDL_SENSOR_INVALID) {
        return;
    }
    if (gamepad->motion_rate_hz_applied[rate_slot] == (int16_t) reportRateHz) {
        /* The host re-announces the same rate on a timer (Sunshine does it every few
         * seconds). There is nothing to apply, and saying so at INFO twice every five
         * seconds buries the lines that explain a controller dropping off the bus. */
        commons_log_debug("Input", "Motion state unchanged for controller %d, motionType: %d, reportRateHz: %d",
                          controllerNumber, motionType, reportRateHz);
        return;
    }
    gamepad->motion_rate_hz_applied[rate_slot] = (int16_t) reportRateHz;
    sensor_state->periodMs = reportRateHz > 0 ? 1000 / reportRateHz : 0;
    SDL_GameControllerSetSensorEnabled(gamepad->controller, sensor_type, reportRateHz > 0 ? SDL_TRUE : SDL_FALSE);
    commons_log_info("Input", "Motion state for controller %d: motionType: %d, reportRateHz: %d (%s)",
                     controllerNumber, motionType, reportRateHz, reportRateHz > 0 ? "on" : "off");
#else
    (void) input;
    (void) controllerNumber;
    (void) motionType;
    (void) reportRateHz;
#endif
}


void app_input_gamepad_set_controller_led(app_input_t *input, unsigned short controllerNumber, uint8_t r, uint8_t g,
                                          uint8_t b) {
    app_gamepad_state_t *state = app_input_gamepad_state_by_gs_id(input, controllerNumber);
    if (state == NULL || state->controller == NULL) {
        return;
    }
#if TARGET_WEBOS
    if (state->ds_usb != NULL && dualsense_usb_set_lightbar(state->ds_usb, r, g, b)) {
        return;
    }
#endif
#if SDL_VERSION_ATLEAST(2, 0, 14)
    SDL_GameControllerSetLED(state->controller, r, g, b);
#else
    (void) r;
    (void) g;
    (void) b;
#endif
}

static bool app_input_gamepad_is_ps5(SDL_GameController *controller) {
#if SDL_VERSION_ATLEAST(2, 0, 16)
    return SDL_GameControllerGetType(controller) == SDL_CONTROLLER_TYPE_PS5;
#else
    (void) controller;
    return false;
#endif
}

static int app_input_gamepad_send_ps5_effect(app_input_t *input, unsigned short controllerNumber,
                                             const void *data, int size) {
#if SDL_VERSION_ATLEAST(2, 0, 18)
    app_gamepad_state_t *state = app_input_gamepad_state_by_gs_id(input, controllerNumber);
    if (state == NULL || state->controller == NULL || !app_input_gamepad_is_ps5(state->controller)) {
        return -1;
    }
    return SDL_GameControllerSendEffect(state->controller, data, size);
#else
    (void) input;
    (void) controllerNumber;
    (void) data;
    (void) size;
    return -1;
#endif
}

void app_input_gamepad_set_adaptive_triggers(app_input_t *input, unsigned short controllerNumber, uint8_t eventFlags,
                                             uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right) {
#if TARGET_WEBOS
    app_gamepad_state_t *state = app_input_gamepad_state_by_gs_id(input, controllerNumber);
    if (state != NULL && state->ds_usb != NULL &&
        dualsense_usb_set_adaptive_triggers(state->ds_usb, eventFlags, typeLeft, typeRight, left, right)) {
        return;
    }
#endif
    /* Fallback: SDL SendEffect (often a no-op on webOS Bluetooth DualSense). */
    uint8_t report[2 + 1 + DS_EFFECT_PAYLOAD_SIZE + 1 + DS_EFFECT_PAYLOAD_SIZE];
    memset(report, 0, sizeof(report));
    report[0] = 0x02;
    report[1] = 0x00;
    int offset = 2;
    if (eventFlags & DS_EFFECT_LEFT_TRIGGER) {
        report[1] |= 0x08;
        report[offset++] = typeLeft;
        if (left != NULL) {
            memcpy(&report[offset], left, DS_EFFECT_PAYLOAD_SIZE);
        }
        offset += DS_EFFECT_PAYLOAD_SIZE;
    }
    if (eventFlags & DS_EFFECT_RIGHT_TRIGGER) {
        report[1] |= 0x04;
        report[offset++] = typeRight;
        if (right != NULL) {
            memcpy(&report[offset], right, DS_EFFECT_PAYLOAD_SIZE);
        }
        offset += DS_EFFECT_PAYLOAD_SIZE;
    }
    if (offset > 2) {
        app_input_gamepad_send_ps5_effect(input, controllerNumber, report, offset);
    }
}

void app_input_gamepad_set_player_led(app_input_t *input, unsigned short controllerNumber, uint8_t ledValue) {
#if TARGET_WEBOS
    app_gamepad_state_t *state = app_input_gamepad_state_by_gs_id(input, controllerNumber);
    if (state != NULL && state->ds_usb != NULL && dualsense_usb_set_player_led(state->ds_usb, ledValue)) {
        return;
    }
#endif
    uint8_t report[2] = {0x05, ledValue};
    app_input_gamepad_send_ps5_effect(input, controllerNumber, report, sizeof(report));
}

void app_input_gamepad_set_mic_led(app_input_t *input, unsigned short controllerNumber, uint8_t ledState) {
#if TARGET_WEBOS
    app_gamepad_state_t *state = app_input_gamepad_state_by_gs_id(input, controllerNumber);
    if (state != NULL && state->ds_usb != NULL && dualsense_usb_set_mic_led(state->ds_usb, ledState)) {
        return;
    }
#endif
    uint8_t report[2] = {0x06, ledState};
    app_input_gamepad_send_ps5_effect(input, controllerNumber, report, sizeof(report));
}

int new_gamepad_state_index(app_input_t *input, SDL_GameController *controller) {
    int index = -1;
    for (short i = 0, j = app_input_get_max_gamepads(input); i < j; i++) {
        if (input->gamepads[i].instance_id != -1) {
            continue;
        }
        if (index == -1) {
            index = i;
        }
        if (is_same_gamepad(&input->gamepads[i], controller)) {
            return i;
        }
    }
    return index;
}

static short next_gamepad_gs_id(app_input_t *input) {
    for (short i = 0, j = app_input_get_max_gamepads(input); i < j; i++) {
        if ((input->activeGamepadMask & (1 << i)) == 0) {
            return i;
        }
    }
    return -1;
}

static bool is_same_gamepad(const app_gamepad_state_t *state, SDL_GameController *controller) {
    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    SDL_JoystickGUID guid = SDL_JoystickGetGUID(joystick);
    if (memcmp(&state->guid, &guid, sizeof(SDL_JoystickGUID)) != 0) {
        return false;
    }
#if SDL_VERSION_ATLEAST(2, 0, 14)
    const char *serial = SDL_JoystickGetSerial(joystick);
    /* DualSense on webOS often has a NULL serial. Matching by GUID alone would
     * make two identical pads share reconnect identity and steal each other's
     * gs_id. Only reuse a slot when we have a real serial to key on. */
    if (serial == NULL || serial[0] == '\0') {
        return false;
    }
    return state->serial_crc == SDL_crc32(0, (const void *) serial, strlen(serial));
#else
    return false;
#endif
}

#ifdef TARGET_WEBOS

static bool str_contains_ci(const char *haystack, const char *needle) {
    if (haystack == NULL || needle == NULL || *needle == '\0') {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *cur = haystack; *cur != '\0'; cur++) {
        size_t i = 0;
        while (i < needle_len && cur[i] != '\0' &&
               tolower((unsigned char) cur[i]) == tolower((unsigned char) needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static bool webos_name_is_non_gamepad(const char *name) {
    return str_contains_ci(name, "remote") ||
           str_contains_ci(name, "keyboard") ||
           str_contains_ci(name, "mouse") ||
           str_contains_ci(name, "pointer") ||
           str_contains_ci(name, "touch") ||
           str_contains_ci(name, "air mouse") ||
           str_contains_ci(name, "webos") ||
           str_contains_ci(name, "lge") ||
           str_contains_ci(name, "lg electronics");
}

static bool webos_should_ignore_controller_like_device(const char *name, const char *guidstr, SDL_Joystick *joystick) {
    const char *display_name = name != NULL ? name : "Unknown";
    int axes = SDL_JoystickNumAxes(joystick);
    int buttons = SDL_JoystickNumButtons(joystick);
    int hats = SDL_JoystickNumHats(joystick);
    commons_log_info("Input", "Controller-like device candidate: %s. GUID: %s, axes: %d, buttons: %d, hats: %d",
                     display_name, guidstr, axes, buttons, hats);
    if (webos_name_is_non_gamepad(name)) {
        commons_log_info("Input", "Ignoring non-gamepad webOS input device by name: %s. GUID: %s", display_name,
                         guidstr);
        return true;
    }
    if (axes < 4 || buttons < 8) {
        commons_log_info("Input",
                         "Ignoring controller-like device with weak gamepad shape: %s. GUID: %s, axes: %d, buttons: %d, hats: %d",
                         display_name, guidstr, axes, buttons, hats);
        return true;
    }
    return false;
}

#endif