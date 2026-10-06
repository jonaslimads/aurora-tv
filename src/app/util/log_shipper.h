#pragma once

#include "logging.h"

#include <stdbool.h>

/**
 * Copies every log line to a UDP sink so a webOS TV's log can be read from another
 * machine without a serial console or SSH.
 *
 * Two switches: the build has to be configured with -DAURORA_LOG_SHIP_TARGET=host:port,
 * and log_ship_enabled has to be on. Either one off means no socket is opened and the
 * only cost on the log path is one bool test per line.
 */

/** Start or stop shipping; safe to call from the settings pane on every change. */
void log_shipper_set_enabled(bool enabled);

/** True while a sink is live. */
bool log_shipper_is_enabled(void);

/** True when this build was compiled with a sink address at all. */
bool log_shipper_has_target(void);

/** The compiled-in host:port, for showing in the settings pane; "" when there is none. */
const char *log_shipper_target(void);

/** Stop the writer thread at shutdown. */
void log_shipper_deinit(void);

/**
 * Queue one formatted line for shipping. Called from the single log listener, so it
 * runs on whatever thread produced the line and must never block on the network.
 */
void log_shipper_write(commons_log_level level, const char *tag, const char *message);
