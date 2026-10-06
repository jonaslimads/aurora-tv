#pragma once

#include <stdint.h>

/**
 * Small banner for app-wide news, on LVGL's system layer so it draws over any
 * fragment: the launcher, a dialog, the settings list. The streaming screen has its
 * own notice with the same look, so a message that belongs to the stream should go
 * there instead of here.
 */
void ui_notice_show_timed(const char *message, uint32_t duration_ms);
