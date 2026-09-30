#include "pyrowave_frame.h"

#include <string.h>

/* Record layout, from third_party/pyrowave/pyrowave/bitstream/bitstream.md.
 * Everything is 32-bit little-endian words. */

/* BitstreamHeader (a block record) */
#define PYROWAVE_HEADER_EXTENDED      (1u << 31)
#define PYROWAVE_BLOCK_SEQUENCE_SHIFT 28
#define PYROWAVE_BLOCK_WORDS_SHIFT    16
#define PYROWAVE_BLOCK_WORDS_MASK     0xFFFu
#define PYROWAVE_BLOCK_INDEX_SHIFT    8

/* BitstreamSequenceHeader (the extended record) */
#define PYROWAVE_SEQ_WIDTH_SHIFT      0
#define PYROWAVE_SEQ_WIDTH_MASK       0x3FFFu
#define PYROWAVE_SEQ_HEIGHT_SHIFT     14
#define PYROWAVE_SEQ_HEIGHT_MASK      0x3FFFu
#define PYROWAVE_SEQ_SEQUENCE_SHIFT   28
#define PYROWAVE_SEQ_SEQUENCE_MASK    0x7u
#define PYROWAVE_SEQ_TOTAL_BLOCKS_MASK 0xFFFFFFu
#define PYROWAVE_SEQ_CODE_SHIFT       24
#define PYROWAVE_SEQ_CODE_MASK        0x3u
#define PYROWAVE_SEQ_CHROMA_444_BIT   (1u << 26)

#define PYROWAVE_CODE_START_OF_FRAME  0u

/* Both record kinds carry 8 bytes of header, so 2 words is the smallest legal record. */
#define PYROWAVE_MIN_RECORD_WORDS     2u
#define PYROWAVE_HEADER_BYTES         8u

/* An in-band padding record: 0xFFFFFFFF, a word count N, then N zero words. Read as a
 * sequence header it would claim a 16384 wide image with code 3, so it can never be
 * confused with a real record. */
#define PYROWAVE_PADDING_MAGIC        0xFFFFFFFFu

/* A frame is tiled by 32x32 blocks of each component and sub-band. Six decomposition
 * levels and up to three components is the widest grid a frame can hold; the coarse
 * level alone is 12 grids wide, so 19 grids is comfortably past any index this frame
 * can carry while still catching nonsense. PyroWave applies the exact rule itself. */
#define PYROWAVE_BLOCK_GRID_LIMIT     19u
#define PYROWAVE_COARSE_GRIDS         12u

typedef struct LostRanges {
    size_t begin[PYROWAVE_MAX_LOST_RANGES];
    size_t end[PYROWAVE_MAX_LOST_RANGES];
    size_t count;
} LostRanges;

typedef struct RecordStarts {
    size_t offset[PYROWAVE_MAX_PAYLOADS];
    size_t count;
} RecordStarts;

static uint32_t load_word(const uint8_t *p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static bool overlaps_lost(const LostRanges *lost, size_t begin, size_t end) {
    for (size_t i = 0; i < lost->count; i++) {
        if (begin < lost->end[i] && lost->begin[i] < end) {
            return true;
        }
    }
    return false;
}

/* First record boundary at or after `from`, or SIZE_MAX when there is none. */
static size_t next_record_start(const RecordStarts *starts, size_t from) {
    for (size_t i = 0; i < starts->count; i++) {
        if (starts->offset[i] >= from) {
            return starts->offset[i];
        }
    }
    return SIZE_MAX;
}

static uint32_t grid_blocks(int width, int height) {
    /* Same rounding the host uses for the critical layout: at least 128 pixels wide
     * and high, then rounded up to a 32 pixel block. */
    uint32_t w = (uint32_t) width < 128u ? 128u : (uint32_t) width;
    uint32_t h = (uint32_t) height < 128u ? 128u : (uint32_t) height;
    return ((w + 31u) / 32u) * ((h + 31u) / 32u);
}

size_t pyrowave_frame_bytes(const PyrowavePayload *payloads, size_t payloadCount) {
    size_t total = 0;
    for (size_t i = 0; i < payloadCount; i++) {
        total += payloads[i].size;
    }
    return total;
}

uint32_t pyrowave_coarse_block_limit(int width, int height) {
    /* The host keeps the coarsest wavelet level in the block indices below
     * 12 * ceil(W / 32) * ceil(H / 32) and PyroWave indexes those blocks first, which
     * is why losing only finer detail costs sharpness and not the whole frame. */
    if (width <= 0 || height <= 0) {
        return 0;
    }
    return PYROWAVE_COARSE_GRIDS * grid_blocks(width, height);
}

static uint32_t block_index_limit(int width, int height) {
    if (width <= 0 || height <= 0) {
        return 0;
    }
    return PYROWAVE_BLOCK_GRID_LIMIT * grid_blocks(width, height);
}

bool pyrowave_scan_frame(const PyrowavePayload *payloads, size_t payloadCount,
                         const PyrowaveStreamInfo *stream,
                         uint32_t criticalPackets,
                         PyrowaveRecordCallback cb, void *userdata,
                         uint8_t *scratch, size_t scratchSize,
                         PyrowaveScanStats *stats) {
    LostRanges lost;
    RecordStarts starts;
    size_t criticalEnd = 0;
    size_t frameSize;
    size_t offset = 0;
    size_t pos = 0;
    bool headerSeen = false;
    bool truncated = false;
    uint32_t headerSequence = 0;

    memset(&lost, 0, sizeof(lost));
    memset(&starts, 0, sizeof(starts));
    memset(stats, 0, sizeof(*stats));

    frameSize = pyrowave_frame_bytes(payloads, payloadCount);
    if (frameSize == 0) {
        stats->invalid = true;
        stats->invalidReason = "empty frame";
        return false;
    }
    if (frameSize > scratchSize) {
        stats->invalid = true;
        stats->invalidReason = "frame does not fit the scan buffer";
        return false;
    }

    /* Lay the frame out. Payloads that were lost stay zeroed, so the offsets of every
     * record after a hole are still the ones the encoder wrote. */
    for (size_t i = 0; i < payloadCount; i++) {
        const PyrowavePayload *payload = &payloads[i];

        if (payload->recordStart && payload->intact && starts.count < PYROWAVE_MAX_PAYLOADS) {
            starts.offset[starts.count++] = offset;
        }

        if (payload->intact) {
            memcpy(scratch + offset, payload->data, payload->size);
        } else {
            memset(scratch + offset, 0, payload->size);
            if (lost.count < PYROWAVE_MAX_LOST_RANGES) {
                lost.begin[lost.count] = offset;
                lost.end[lost.count] = offset + payload->size;
                lost.count++;
            }
        }

        offset += payload->size;

        /* The host counts critical packets in RTP payloads, so the boundary that
         * matters is the last byte of the payload that count points at. */
        if (criticalPackets > 0 && i + 1 == criticalPackets) {
            criticalEnd = offset;
        }
    }

    while (pos < frameSize) {
        size_t remaining = frameSize - pos;
        size_t recordSize;
        bool isSequenceHeader = false;
        bool isPadding = false;
        uint32_t word0;
        uint32_t word1;

        if (remaining < PYROWAVE_HEADER_BYTES) {
            truncated = true;
            break;
        }

        /* A lost header cannot be read at all: the bytes standing in for it are zeros,
         * which would parse as a nonsense record. Resume at the next payload the host
         * flagged as starting on a record boundary. */
        if (overlaps_lost(&lost, pos, pos + PYROWAVE_HEADER_BYTES)) {
            size_t resume = next_record_start(&starts, pos + 1);
            if (resume == SIZE_MAX || resume >= frameSize) {
                truncated = true;
                break;
            }
            stats->recordsDesynchronized++;
            pos = resume;
            continue;
        }

        word0 = load_word(scratch + pos);
        word1 = load_word(scratch + pos + 4);

        if (word0 == PYROWAVE_PADDING_MAGIC) {
            recordSize = PYROWAVE_HEADER_BYTES + 4u * (size_t) word1;
            isPadding = true;
            stats->paddingRecords++;
        } else if (word0 & PYROWAVE_HEADER_EXTENDED) {
            recordSize = PYROWAVE_HEADER_BYTES;
            isSequenceHeader = true;
        } else {
            uint32_t payloadWords = (word0 >> PYROWAVE_BLOCK_WORDS_SHIFT) & PYROWAVE_BLOCK_WORDS_MASK;
            if (payloadWords < PYROWAVE_MIN_RECORD_WORDS) {
                stats->invalid = true;
                stats->invalidReason = "block record smaller than its own header";
                return false;
            }
            recordSize = 4u * (size_t) payloadWords;
        }

        if (recordSize > remaining) {
            stats->invalid = true;
            stats->invalidReason = "record runs past the end of the frame";
            return false;
        }

        stats->recordsSeen++;

        /* A record that lost a byte is not pushed to the decoder. When its header
         * survived we know where it ends, so the next one can be parsed right away. */
        if (overlaps_lost(&lost, pos, pos + recordSize)) {
            stats->recordsSkipped++;
            pos += recordSize;
            continue;
        }

        if (isSequenceHeader) {
            uint32_t code = (word1 >> PYROWAVE_SEQ_CODE_SHIFT) & PYROWAVE_SEQ_CODE_MASK;

            if (headerSeen) {
                stats->invalid = true;
                stats->invalidReason = "second sequence header in one frame";
                return false;
            }
            if (code != PYROWAVE_CODE_START_OF_FRAME) {
                stats->invalid = true;
                stats->invalidReason = "unknown extended header code";
                return false;
            }

            stats->width = ((word0 >> PYROWAVE_SEQ_WIDTH_SHIFT) & PYROWAVE_SEQ_WIDTH_MASK) + 1u;
            stats->height = ((word0 >> PYROWAVE_SEQ_HEIGHT_SHIFT) & PYROWAVE_SEQ_HEIGHT_MASK) + 1u;
            stats->chroma444 = (word1 & PYROWAVE_SEQ_CHROMA_444_BIT) != 0;

            /* The host encodes whole 32x32 blocks and an even frame for 4:2:0, so the
             * header can be a little larger than what the session negotiated. The real
             * size is the display size and the padding is cropped; anything beyond that
             * room would decode past the picture, which is the failure the codec's own
             * contract warns about. */
            if (stats->width < (uint32_t) stream->width ||
                    stats->width > (uint32_t) stream->width + 31u ||
                    stats->height < (uint32_t) stream->height ||
                    stats->height > (uint32_t) stream->height + 8192u + 31u ||
                    stats->chroma444 != stream->chroma444) {
                stats->invalid = true;
                stats->invalidReason = "sequence header does not match the negotiated stream";
                return false;
            }

            headerSeen = true;
            headerSequence = (word0 >> PYROWAVE_SEQ_SEQUENCE_SHIFT) & PYROWAVE_SEQ_SEQUENCE_MASK;

            stats->sequenceHeaderOk = true;
            stats->sequence = headerSequence;
            stats->totalBlocks = (word1 >> 0) & PYROWAVE_SEQ_TOTAL_BLOCKS_MASK;
        } else if (!isPadding) {
            /* A block record. Padding is never pushed to the codec, which is why it is
             * identified by its magic and counted separately. */
            uint32_t blockIndex;

            if (!headerSeen) {
                stats->invalid = true;
                stats->invalidReason = "block record before the sequence header";
                return false;
            }

            if (((word0 >> PYROWAVE_BLOCK_SEQUENCE_SHIFT) & 0x7u) != headerSequence) {
                stats->invalid = true;
                stats->invalidReason = "block record from another frame";
                return false;
            }

            blockIndex = word1 >> PYROWAVE_BLOCK_INDEX_SHIFT;
            if (blockIndex >= block_index_limit(stream->width, stream->height)) {
                stats->invalid = true;
                stats->invalidReason = "block index outside the frame";
                return false;
            }
        }

        /* Padding is dropped rather than pushed: the codec reads a record whose top
         * bit is set as a sequence header, so an in-band padding record left in the
         * stream would be parsed as one and could reset the decoder. */
        if (!isPadding) {
            if (!cb(userdata, scratch + pos, recordSize)) {
                truncated = true;
                break;
            }
            stats->recordsPushed++;
        }

        pos += recordSize;
    }

    if (!headerSeen) {
        stats->invalid = true;
        stats->invalidReason = "frame has no sequence header";
        return false;
    }

    /* Records tile the frame exactly, so a short end means the last record was cut
     * off. The frame is still worth decoding if the header and coarse level arrived,
     * which is the whole point of an intra-only codec. */
    if (pos != frameSize) {
        truncated = true;
    }

    stats->truncated = truncated;

    if (criticalEnd > 0) {
        for (size_t i = 0; i < lost.count; i++) {
            if (lost.begin[i] < criticalEnd) {
                stats->criticalLost = true;
                break;
            }
        }
    }

    return !stats->invalid;
}

bool pyrowave_frame_worth_decoding(const PyrowaveScanStats *stats) {
    uint64_t arrived;

    if (stats->invalid || stats->criticalLost || stats->recordsPushed == 0) {
        return false;
    }

    if (stats->totalBlocks == 0) {
        /* A still picture can encode to nothing but the sequence header. */
        return true;
    }

    /* The codec refuses a frame built out of 90% of its blocks or fewer, so neither do
     * we: asking it anyway means the same refusal twice, once after the memcpy work.
     * This is only a cheap pre-filter though — the codec has the final say through
     * pyrowave_decoder_decode_is_ready(), which also wants its coarsest bands complete
     * and counts blocks its own way. Refusing here is never a wrong answer: every
     * PyroWave frame stands alone, so the cost is one frame. */
    arrived = stats->recordsPushed - 1u;

    return arrived * 10u > (uint64_t) stats->totalBlocks * 9u;
}
