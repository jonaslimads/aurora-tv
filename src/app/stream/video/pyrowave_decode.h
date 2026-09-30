#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pyrowave_frame.h"

/**
 * PyroWave decoding for Aurora.
 *
 * The codec (third_party/pyrowave) is a Vulkan compute implementation, so this only
 * works on a platform that has both Vulkan and a video sink Aurora can draw into. It
 * is compiled in when ENABLE_PYROWAVE is on and aurora_pyrowave_available() reports
 * whether this machine can actually run it; the caller must not offer the codec to the
 * host unless that returns true.
 */

typedef struct aurora_pyrowave_decoder aurora_pyrowave_decoder_t;

/** One decoded picture. The planes belong to the decoder and are valid until released. */
typedef struct aurora_pyrowave_frame_t {
    int width;
    int height;
    /** This frame lost packets; its missing blocks come out blurred. */
    bool partial;
    /** Y, Cb, Cr as single channel 8-bit planes. PyroWave defines its output in
     *  floating point and the CPU readback path stores it as R8 UNORM whatever the
     *  stream's depth, so a 10-bit stream arrives here reduced to 8 bits. Feeding a
     *  10-bit stream to a display needs the codec's GPU image path instead, which
     *  needs an importer for Aurora's own textures. See docs/PYROWAVE.md. */
    const uint8_t *planes[3];
    /** Row stride of each plane in bytes. */
    size_t strides[3];
} aurora_pyrowave_frame_t;

/** Counters for the performance overlay and the log. */
typedef struct aurora_pyrowave_stats_t {
    uint32_t framesDecoded;
    /** Frames dropped because they broke the record framing contract. */
    uint32_t framesInvalid;
    /** Frames dropped although they parsed: the coarse level or too many blocks were missing. */
    uint32_t framesUndecodable;
    /** Records that were not pushed to the codec because they lost a byte. */
    uint32_t recordsSkipped;
    /** Decode submissions the codec refused. */
    uint32_t decodeFailures;
} aurora_pyrowave_stats_t;

/** True when this build can decode PyroWave: the codec is compiled in and a Vulkan
 *  device PyroWave supports was found. Cached, cheap to call from the UI. */
bool aurora_pyrowave_available(void);

/** Why the codec is unavailable, for the settings screen. NULL when available. */
const char *aurora_pyrowave_unavailable_reason(void);

/** Creates a decoder for a negotiated stream. tenBit selects a 10-bit stream, which
 *  this build decodes to 8-bit planes (see aurora_pyrowave_frame_t). Returns NULL when
 *  the decoder cannot be created. */
aurora_pyrowave_decoder_t *aurora_pyrowave_decoder_create(const PyrowaveStreamInfo *stream, bool tenBit);

void aurora_pyrowave_decoder_destroy(aurora_pyrowave_decoder_t *decoder);

/**
 * Scans one frame, pushes the records that survived to the codec and decodes the frame
 * when enough of it arrived.
 *
 * Returns true when out carries a picture. Returns false when the frame was dropped or
 * the codec refused; stats always describes what happened.
 */
bool aurora_pyrowave_decoder_submit(aurora_pyrowave_decoder_t *decoder,
                                    const PyrowavePayload *payloads, size_t payloadCount,
                                    uint32_t criticalPackets,
                                    aurora_pyrowave_frame_t *out,
                                    PyrowaveScanStats *stats);

/** The codec keeps a decoded picture alive until the next submit; this ends the frame. */
void aurora_pyrowave_decoder_release(aurora_pyrowave_decoder_t *decoder);

void aurora_pyrowave_decoder_get_stats(const aurora_pyrowave_decoder_t *decoder,
                                       aurora_pyrowave_stats_t *stats);
