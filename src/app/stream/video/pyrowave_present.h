#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "pyrowave_decode.h"

/**
 * Where a decoded PyroWave frame goes.
 *
 * Aurora hands every other codec to SS4S, which is a bitstream sink: it takes an
 * encoded stream and presents it under the UI on a hardware overlay. There is no such
 * path for frames that the client decoded itself, so PyroWave frames are written out by
 * this sink instead of being displayed. See docs/PYROWAVE.md for what a real display
 * integration would take.
 *
 * Returns true when something consumed the frame.
 */
bool aurora_pyrowave_present(const aurora_pyrowave_frame_t *frame);

/** Where frames go, for the settings screen and the log. */
void aurora_pyrowave_sink_describe(char *out, size_t outSize);
