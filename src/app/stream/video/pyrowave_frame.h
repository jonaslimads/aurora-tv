#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)
extern "C" {
#endif

/**
 * Record-framed PyroWave frame parsing.
 *
 * A PyroWave frame arrives as the concatenation of the RTP payloads of one decode
 * unit: a sequence header record, then the 32x32 block records in any order, with
 * padding records anywhere between them. Every record is self-delimiting, which is
 * what lets a frame that lost packets still decode: the records that survived are
 * pushed to the decoder and the missing blocks simply come out blurred.
 *
 * See docs/PYROWAVE.md and the upstream bitstream definition in
 * third_party/pyrowave/pyrowave/bitstream/bitstream.md.
 *
 * This file is deliberately free of decoder and GPU dependencies so that the parsing
 * contract is unit-testable on its own.
 */

/** Largest frame we will assemble: the host caps a PyroWave frame at 4000 packets. */
#define PYROWAVE_MAX_PAYLOADS 4096
/** Gaps we can track in one frame. Beyond this the frame is not worth salvaging. */
#define PYROWAVE_MAX_LOST_RANGES 256

/** One RTP payload of a frame, in wire order, after the video packet header. */
typedef struct PyrowavePayload {
    const uint8_t *data;
    size_t size;
    /** False when forward error correction could not restore this payload. */
    bool intact;
    /** The host flagged this payload as starting on a record boundary. */
    bool recordStart;
} PyrowavePayload;

/** Stream parameters negotiated with the host, used to validate each frame. */
typedef struct PyrowaveStreamInfo {
    int width;
    int height;
    /** true for 4:4:4, false for 4:2:0 */
    bool chroma444;
} PyrowaveStreamInfo;

/** What a scan found. Reported even when the scan fails. */
typedef struct PyrowaveScanStats {
    /** A usable sequence header was seen, and it matches the negotiated stream. */
    bool sequenceHeaderOk;
    uint32_t width;
    uint32_t height;
    uint32_t totalBlocks;
    uint32_t sequence;
    bool chroma444;

    uint32_t recordsSeen;
    /** Records handed to the callback. */
    uint32_t recordsPushed;
    /** Records skipped because a byte of them never arrived. */
    uint32_t recordsSkipped;
    uint32_t paddingRecords;
    /** Records skipped while resynchronising on a record boundary after a lost header. */
    uint32_t recordsDesynchronized;

    /** At least one of the leading critical payloads was lost: the coarsest wavelet
     *  level is incomplete, so the frame cannot be decoded at all. */
    bool criticalLost;
    /** Parsing stopped before the end of the frame. */
    bool truncated;
    /** The frame breaks the protocol and must be dropped whole. */
    bool invalid;
    const char *invalidReason;
} PyrowaveScanStats;

/**
 * Called once per record that survived, in frame order, with the whole record
 * (header included) inside the scan buffer. Returning false aborts the scan.
 */
typedef bool (*PyrowaveRecordCallback)(void *userdata, const uint8_t *record, size_t size);

/**
 * Copies the payloads into scratch (zero-filling the payloads that were lost, so that
 * the byte offsets of everything after a hole stay correct), walks the records and
 * reports every intact record through cb.
 *
 * scratch must hold pyrowave_frame_bytes(payloads, count) bytes.
 *
 * Returns false when the frame must be dropped; stats then carries the reason.
 */
bool pyrowave_scan_frame(const PyrowavePayload *payloads, size_t payloadCount,
                         const PyrowaveStreamInfo *stream,
                         uint32_t criticalPackets,
                         PyrowaveRecordCallback cb, void *userdata,
                         uint8_t *scratch, size_t scratchSize,
                         PyrowaveScanStats *stats);

/** Total bytes of the frame, including the holes left by lost payloads. */
size_t pyrowave_frame_bytes(const PyrowavePayload *payloads, size_t payloadCount);

/**
 * Whether a scanned frame is worth handing to the decoder: it is well formed, the
 * coarsest wavelet level arrived, and enough of the remaining blocks came in that the
 * picture is better than the previous one. The codec is intra-only, so a frame that
 * fails here is simply the one frame that costs the loss.
 */
bool pyrowave_frame_worth_decoding(const PyrowaveScanStats *stats);

/**
 * Number of leading block records that hold the coarsest wavelet level, which the
 * decoder needs before it can produce anything. The host lays a frame out so these
 * come first, which is why losing only finer detail costs sharpness and not the frame.
 */
uint32_t pyrowave_coarse_block_limit(int width, int height);

#if defined(__cplusplus)
}
#endif
