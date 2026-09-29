#pragma once

#include <stdbool.h>
#include <SDL_events.h>

typedef struct session_t session_t;

bool session_handle_input_event(session_t *session, const SDL_Event *event);

void session_update_touchpad_tap_hold(session_t *session);

/** Keep drift-corrected gamepad state fresh; call every frame while a session runs. */
void session_update_gamepad_stability(session_t *session);
