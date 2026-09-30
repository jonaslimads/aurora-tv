#include "pyrowave_decode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

#if ENABLE_PYROWAVE

#include <vulkan/vulkan_core.h>
#include <pyrowave.h>

/* PyroWave owns one Vulkan device for the whole session: creating a Granite device is
 * expensive and the codec keeps every allocation under it. There is no teardown entry
 * point in the codec's C API, so the device is created once and lives for the process.
 * Probing it here is also what tells the settings screen whether PyroWave can be
 * offered to the host at all. */
static pyrowave_device pyrowave_shared_device;
static bool pyrowave_device_probed;
static bool pyrowave_device_ok;
static char pyrowave_unavailable[160];

static bool ensure_pyrowave_device(void) {
    if (pyrowave_device_probed) {
        return pyrowave_device_ok;
    }

    pyrowave_device_probed = true;

    pyrowave_result result = pyrowave_create_default_device(&pyrowave_shared_device);
    if (result != PYROWAVE_SUCCESS) {
        snprintf(pyrowave_unavailable, sizeof(pyrowave_unavailable),
                 "PyroWave needs a Vulkan device this machine supports (codec error %d)", (int) result);
        commons_log_warn("PyroWave", "%s", pyrowave_unavailable);
        pyrowave_device_ok = false;
        return false;
    }

    pyrowave_device_ok = true;
    return true;
}

struct aurora_pyrowave_decoder {
    PyrowaveStreamInfo stream;
    bool tenBit;

    /* PyroWave decodes whole 32x32 blocks, so it can write up to a row of blocks past
     * the requested picture. The decoder and the planes are sized for that and the
     * caller crops with the negotiated size. */
    int decodeWidth;
    int decodeHeight;

    /* The size the planes are big enough for, whether or not the codec was created
     * with it yet. */
    int allocWidth;
    int allocHeight;
    /** Set once a frame's sequence header has told us what the host really encodes. */
    bool dimsLocked;
    bool fragmentPath;

    pyrowave_decoder decoder;
    pyrowave_cpu_buffer buffers;
    uint8_t *planes[3];
    size_t planeSize[3];

    uint8_t *scratch;
    size_t scratchSize;

    aurora_pyrowave_stats_t stats;
};

bool aurora_pyrowave_available(void) {
    return ensure_pyrowave_device();
}

const char *aurora_pyrowave_unavailable_reason(void) {
    if (ensure_pyrowave_device()) {
        return NULL;
    }
    return pyrowave_unavailable[0] ? pyrowave_unavailable : "PyroWave is unavailable";
}

/* The worst frame the host can hand us: PyroWave caps a frame at 4000 packets and the
 * bitstream is never denser than a byte per sample for the shapes it targets, so the
 * planar size with room to spare covers it. Anything bigger is refused rather than
 * allowed to overrun. */
static size_t worst_case_frame_bytes(const PyrowaveStreamInfo *stream, int width, int height) {
    size_t pixels = (size_t) width * (size_t) height;
    size_t chroma = stream->chroma444 ? pixels : pixels / 2;
    return (pixels + chroma) * 3 + (size_t) 4000 * 1400;
}

static size_t plane_bytes(int width, int height) {
    /* Stride is the plane width, plus a page so a caller that rounds a stride up to a
     * cache line still stays inside the allocation. */
    return (size_t) width * (size_t) height + 4096;
}

/* The codec insists that a frame's sequence header matches the extents it was created
 * with, so the decoder is (re)built here whenever those extents change. */
static pyrowave_result aurora_pyrowave_recreate_decoder(aurora_pyrowave_decoder_t *decoder,
                                                        int width, int height) {
    pyrowave_decoder_create_info info = {0};

    if (decoder->decoder) {
        pyrowave_decoder_destroy(decoder->decoder);
        decoder->decoder = NULL;
    }

    info.device = pyrowave_shared_device;
    info.width = width;
    info.height = height;
    info.chroma = decoder->stream.chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444
                                             : PYROWAVE_CHROMA_SUBSAMPLING_420;
    /* Mobile GPUs with weak compute support decode through the render pass path. */
    info.fragment_path = pyrowave_decoder_device_prefers_fragment_path(pyrowave_shared_device);
    decoder->fragmentPath = info.fragment_path;

    pyrowave_result result = pyrowave_decoder_create(&info, &decoder->decoder);
    if (result != PYROWAVE_SUCCESS) {
        return result;
    }

    decoder->decodeWidth = width;
    decoder->decodeHeight = height;
    decoder->buffers.width = width;
    decoder->buffers.height = height;

    return PYROWAVE_SUCCESS;
}

aurora_pyrowave_decoder_t *aurora_pyrowave_decoder_create(const PyrowaveStreamInfo *stream, bool tenBit) {
    aurora_pyrowave_decoder_t *decoder;
    pyrowave_result result;

    if (!stream || stream->width <= 0 || stream->height <= 0) {
        return NULL;
    }

    if (!ensure_pyrowave_device()) {
        return NULL;
    }

    decoder = calloc(1, sizeof(*decoder));
    if (!decoder) {
        return NULL;
    }

    decoder->stream = *stream;
    decoder->tenBit = tenBit;

    /* 4:2:0 needs even dimensions; the block grid needs the height rounded up to 32. */
    decoder->decodeWidth = (stream->width + 1) & ~1;
    decoder->decodeHeight = (stream->height + 31) & ~31;

    decoder->scratchSize = worst_case_frame_bytes(stream, decoder->decodeWidth, decoder->decodeHeight);
    decoder->scratch = malloc(decoder->scratchSize);
    if (!decoder->scratch) {
        free(decoder);
        return NULL;
    }

    for (int plane = 0; plane < 3; plane++) {
        int planeWidth = decoder->decodeWidth;
        int planeHeight = decoder->decodeHeight;
        if (plane != 0 && !stream->chroma444) {
            planeWidth /= 2;
            planeHeight /= 2;
        }

        decoder->planeSize[plane] = plane_bytes(planeWidth, planeHeight);
        decoder->planes[plane] = malloc(decoder->planeSize[plane]);
        if (!decoder->planes[plane]) {
            aurora_pyrowave_decoder_destroy(decoder);
            return NULL;
        }

        decoder->buffers.data[plane] = decoder->planes[plane];
        decoder->buffers.row_stride_in_bytes[plane] = (size_t) planeWidth;
        decoder->buffers.plane_size_in_bytes[plane] = decoder->planeSize[plane];
    }

    decoder->allocWidth = decoder->decodeWidth;
    decoder->allocHeight = decoder->decodeHeight;

    decoder->buffers.width = decoder->decodeWidth;
    decoder->buffers.height = decoder->decodeHeight;
    decoder->buffers.format = stream->chroma444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P
                                                : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;

    result = aurora_pyrowave_recreate_decoder(decoder, decoder->decodeWidth, decoder->decodeHeight);
    if (result != PYROWAVE_SUCCESS) {
        commons_log_warn("PyroWave", "PyroWave: cannot create a decoder for %dx%d (codec error %d)",
               decoder->decodeWidth, decoder->decodeHeight, (int) result);
        aurora_pyrowave_decoder_destroy(decoder);
        return NULL;
    }

    commons_log_info("PyroWave", "PyroWave: decoding %dx%d %s%s through %s",
           stream->width, stream->height, stream->chroma444 ? "4:4:4" : "4:2:0",
           tenBit ? ", 10-bit" : "",
           decoder->fragmentPath ? "render passes" : "compute");

    return decoder;
}

void aurora_pyrowave_decoder_destroy(aurora_pyrowave_decoder_t *decoder) {
    if (!decoder) {
        return;
    }

    if (decoder->decoder) {
        pyrowave_decoder_destroy(decoder->decoder);
    }
    for (int plane = 0; plane < 3; plane++) {
        free(decoder->planes[plane]);
    }
    free(decoder->scratch);
    free(decoder);
}

static bool discard_record(void *userdata, const uint8_t *record, size_t size) {
    (void) userdata;
    (void) record;
    (void) size;
    return true;
}

static bool push_record(void *userdata, const uint8_t *record, size_t size) {
    pyrowave_decoder decoder = (pyrowave_decoder) userdata;
    return pyrowave_decoder_push_packet(decoder, record, size) == PYROWAVE_SUCCESS;
}

bool aurora_pyrowave_decoder_submit(aurora_pyrowave_decoder_t *decoder,
                                    const PyrowavePayload *payloads, size_t payloadCount,
                                    uint32_t criticalPackets,
                                    aurora_pyrowave_frame_t *out,
                                    PyrowaveScanStats *stats) {
    PyrowaveScanStats scan;
    pyrowave_result result;

    if (decoder->decoder == NULL) {
        memset(stats, 0, sizeof(*stats));
        stats->invalid = true;
        stats->invalidReason = "no PyroWave decoder";
        return false;
    }

    /* The host encodes whole blocks, so what it sends can be a few pixels wider or
     * taller than the session negotiated. The codec refuses a frame whose header
     * disagrees with the decoder, so the first frame is read once without pushing
     * anything to find the extents it really carries. */
    if (!decoder->dimsLocked) {
        PyrowaveScanStats probe;
        if (!pyrowave_scan_frame(payloads, payloadCount, &decoder->stream, criticalPackets,
                                 discard_record, NULL, decoder->scratch, decoder->scratchSize,
                                 &probe)) {
            decoder->stats.framesInvalid++;
            *stats = probe;
            commons_log_warn("PyroWave", "PyroWave: frame dropped: %s",
                   probe.invalidReason ? probe.invalidReason : "record framing error");
            return false;
        }

        if (probe.width != (uint32_t) decoder->decodeWidth ||
                probe.height != (uint32_t) decoder->decodeHeight) {
            if (probe.width > (uint32_t) decoder->allocWidth ||
                    probe.height > (uint32_t) decoder->allocHeight) {
                decoder->stats.framesInvalid++;
                *stats = probe;
                commons_log_error("PyroWave", "PyroWave: host encodes %ux%u, which does not fit the "
                                  "%dx%d picture this session set up",
                       probe.width, probe.height, decoder->allocWidth, decoder->allocHeight);
                return false;
            }

            if (aurora_pyrowave_recreate_decoder(decoder, (int) probe.width, (int) probe.height) != PYROWAVE_SUCCESS) {
                decoder->stats.decodeFailures++;
                *stats = probe;
                commons_log_error("PyroWave", "PyroWave: cannot rebuild the decoder for %ux%u",
                       probe.width, probe.height);
                return false;
            }

            commons_log_info("PyroWave", "PyroWave: host encodes %ux%u for a %dx%d session",
                   probe.width, probe.height, decoder->stream.width, decoder->stream.height);
        }

        decoder->dimsLocked = true;
    }

    /* Packets must be cleared before every frame, including a re-sent one. Records are
     * pushed while the frame is scanned; a frame that turns out to be unusable leaves
     * records behind, which is harmless because the next frame clears them first. */
    pyrowave_decoder_clear(decoder->decoder);

    if (!pyrowave_scan_frame(payloads, payloadCount, &decoder->stream, criticalPackets,
                             push_record, (void *) decoder->decoder,
                             decoder->scratch, decoder->scratchSize, &scan)) {
        decoder->stats.framesInvalid++;
        *stats = scan;
        commons_log_warn("PyroWave", "PyroWave: frame dropped: %s",
               scan.invalidReason ? scan.invalidReason : "record framing error");
        return false;
    }

    *stats = scan;
    decoder->stats.recordsSkipped += scan.recordsSkipped;

    if (!pyrowave_frame_worth_decoding(&scan)) {
        decoder->stats.framesUndecodable++;
        commons_log_debug("PyroWave", "PyroWave: frame too damaged to decode (%u of %u records)",
               scan.recordsPushed, scan.totalBlocks);
        return false;
    }

    /* Every PyroWave frame is an IDR, so a frame the codec cannot finish costs exactly
     * one frame and needs no keyframe request. A frame that lost packets outside its
     * critical packets still decodes, with the missing blocks blurred. */
    if (!pyrowave_decoder_decode_is_ready(decoder->decoder, scan.recordsSkipped > 0)) {
        decoder->stats.framesUndecodable++;
        commons_log_debug("PyroWave", "PyroWave: codec will not build this frame");
        return false;
    }

    result = pyrowave_decoder_decode_cpu_buffer_synchronous(decoder->decoder, &decoder->buffers);
    if (result != PYROWAVE_SUCCESS) {
        decoder->stats.decodeFailures++;
        commons_log_warn("PyroWave", "PyroWave: decode failed (codec error %d)", (int) result);
        return false;
    }

    decoder->stats.framesDecoded++;

    out->width = decoder->stream.width;
    out->height = decoder->stream.height;
    out->partial = scan.recordsSkipped > 0 || scan.truncated;
    for (int plane = 0; plane < 3; plane++) {
        out->planes[plane] = decoder->planes[plane];
        out->strides[plane] = decoder->buffers.row_stride_in_bytes[plane];
    }

    return true;
}

void aurora_pyrowave_decoder_release(aurora_pyrowave_decoder_t *decoder) {
    /* The readback planes are the decoder's own storage and stay valid until the next
     * submit, which is the lifetime documented for aurora_pyrowave_frame_t. */
    (void) decoder;
}

void aurora_pyrowave_decoder_get_stats(const aurora_pyrowave_decoder_t *decoder,
                                       aurora_pyrowave_stats_t *stats) {
    memset(stats, 0, sizeof(*stats));
    if (decoder) {
        *stats = decoder->stats;
    }
}

#else /* !ENABLE_PYROWAVE */

bool aurora_pyrowave_available(void) {
    return false;
}

const char *aurora_pyrowave_unavailable_reason(void) {
    return "This build has no PyroWave codec";
}

aurora_pyrowave_decoder_t *aurora_pyrowave_decoder_create(const PyrowaveStreamInfo *stream, bool tenBit) {
    (void) stream;
    (void) tenBit;
    return NULL;
}

void aurora_pyrowave_decoder_destroy(aurora_pyrowave_decoder_t *decoder) {
    (void) decoder;
}

bool aurora_pyrowave_decoder_submit(aurora_pyrowave_decoder_t *decoder,
                                    const PyrowavePayload *payloads, size_t payloadCount,
                                    uint32_t criticalPackets,
                                    aurora_pyrowave_frame_t *out,
                                    PyrowaveScanStats *stats) {
    (void) decoder;
    (void) payloads;
    (void) payloadCount;
    (void) criticalPackets;
    (void) out;
    memset(stats, 0, sizeof(*stats));
    stats->invalid = true;
    stats->invalidReason = "this client was built without the PyroWave codec";
    return false;
}

void aurora_pyrowave_decoder_release(aurora_pyrowave_decoder_t *decoder) {
    (void) decoder;
}

void aurora_pyrowave_decoder_get_stats(const aurora_pyrowave_decoder_t *decoder,
                                       aurora_pyrowave_stats_t *stats) {
    (void) decoder;
    memset(stats, 0, sizeof(*stats));
}

#endif /* ENABLE_PYROWAVE */
