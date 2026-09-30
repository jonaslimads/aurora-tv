// The record framing contract between a PyroWave host and this client: a frame is a
// run of 32-bit little-endian records (sequence header, block records, padding), the
// records may straddle RTP payloads, and a frame that lost packets must still be
// parsed so the blocks that survived can be decoded. These tests build frames by hand
// from third_party/pyrowave/pyrowave/bitstream/bitstream.md and check what the scanner
// makes of them, including the protocol violations the host document lists as reasons
// to drop a frame.

#include <string.h>

#include "unity.h"

#include "stream/video/pyrowave_frame.h"

#define WIDTH  1920
#define HEIGHT 1080

#define WORDS_PER_RECORD_MAX 64

static uint8_t frame[16384];
static size_t frameSize;

static PyrowaveStreamInfo stream = {WIDTH, HEIGHT, false};

static uint8_t scratch[16384];
static PyrowaveScanStats stats;

/* Every record the scanner accepted, kept so the tests can look at them. */
static uint8_t pushed[32][WORDS_PER_RECORD_MAX * 4];
static size_t pushedSize[32];
static uint32_t pushCount;

void setUp(void) {
    frameSize = 0;
    memset(scratch, 0, sizeof(scratch));
    memset(&stats, 0, sizeof(stats));
    pushCount = 0;
    memset(pushedSize, 0, sizeof(pushedSize));
}

void tearDown(void) {
}

static void putWord(uint32_t word) {
    frame[frameSize++] = (uint8_t) (word & 0xFF);
    frame[frameSize++] = (uint8_t) ((word >> 8) & 0xFF);
    frame[frameSize++] = (uint8_t) ((word >> 16) & 0xFF);
    frame[frameSize++] = (uint8_t) ((word >> 24) & 0xFF);
}

/** BitstreamSequenceHeader, the one extended record the codec defines. */
static void putSequenceHeader(uint32_t sequence, uint32_t totalBlocks, int width, int height,
                              bool chroma444) {
    uint32_t word0 = (uint32_t) (width - 1) |
                     ((uint32_t) (height - 1) << 14) |
                     ((sequence & 0x7u) << 28) |
                     (1u << 31);
    uint32_t word1 = (totalBlocks & 0xFFFFFFu) | (chroma444 ? (1u << 26) : 0u);
    putWord(word0);
    putWord(word1);
}

/** BitstreamHeader plus a couple of payload words so the record has some body. */
static void putBlockRecord(uint32_t sequence, uint32_t blockIndex, uint32_t words) {
    uint32_t word0 = 0x1u | ((words & 0xFFFu) << 16) | ((sequence & 0x7u) << 28);
    putWord(word0);
    putWord(blockIndex << 8);
    for (uint32_t i = 2; i < words; i++) {
        putWord(0xA5A50000u + i);
    }
}

static void putPadding(uint32_t words) {
    putWord(0xFFFFFFFFu);
    putWord(words);
    for (uint32_t i = 0; i < words; i++) {
        putWord(0u);
    }
}

static bool collect(void *userdata, const uint8_t *record, size_t size) {
    (void) userdata;
    TEST_ASSERT_LESS_OR_EQUAL(sizeof(pushed[0]) , size);
    TEST_ASSERT_LESS_THAN(32, pushCount);
    memcpy(pushed[pushCount], record, size);
    pushedSize[pushCount] = size;
    pushCount++;
    return true;
}

/** The whole frame in one payload, which is how an intact frame arrives. */
static bool scanWholeFrame(uint32_t criticalPackets) {
    PyrowavePayload payload = {frame, frameSize, true, true};
    return pyrowave_scan_frame(&payload, 1, &stream, criticalPackets, collect, NULL,
                               scratch, sizeof(scratch), &stats);
}

/** A frame with the sequence header, some blocks and padding between them. */
static void buildCleanFrame(void) {
    putSequenceHeader(3, 4, WIDTH, HEIGHT, false);
    putBlockRecord(3, 0, 8);
    putPadding(2);
    putBlockRecord(3, 1, 6);
    putBlockRecord(3, 2, 4);
    putBlockRecord(3, 3, 12);
}

static void test_clean_frame_pushes_every_record_in_order(void) {
    buildCleanFrame();

    TEST_ASSERT_TRUE(scanWholeFrame(0));

    TEST_ASSERT_TRUE(stats.sequenceHeaderOk);
    TEST_ASSERT_EQUAL_UINT(WIDTH, stats.width);
    TEST_ASSERT_EQUAL_UINT(HEIGHT, stats.height);
    TEST_ASSERT_EQUAL_UINT(4, stats.totalBlocks);
    TEST_ASSERT_EQUAL_UINT(3, stats.sequence);
    TEST_ASSERT_FALSE(stats.chroma444);
    TEST_ASSERT_EQUAL_UINT(0, stats.recordsSkipped);
    TEST_ASSERT_EQUAL_UINT(1, stats.paddingRecords);
    TEST_ASSERT_FALSE(stats.invalid);
    TEST_ASSERT_FALSE(stats.truncated);

    /* The header and the four blocks, padding excluded, in the order they came. */
    TEST_ASSERT_EQUAL_UINT(5, pushCount);
    TEST_ASSERT_EQUAL_UINT(5, stats.recordsPushed);
    TEST_ASSERT_EQUAL_UINT(8 * 4, pushedSize[1]);
    TEST_ASSERT_EQUAL_UINT(6 * 4, pushedSize[2]);
    TEST_ASSERT_EQUAL_UINT(4 * 4, pushedSize[3]);
    TEST_ASSERT_EQUAL_UINT(12 * 4, pushedSize[4]);

    TEST_ASSERT_TRUE(pyrowave_frame_worth_decoding(&stats));
}

static void test_records_may_straddle_payloads(void) {
    buildCleanFrame();

    /* Chop the frame into payloads that land in the middle of records, the way the
     * host's 1376 byte shards do: the scanner must reassemble and still see all five. */
    PyrowavePayload payloads[16];
    size_t chunk = 20;
    size_t used = 0;
    size_t count = 0;
    while (used < frameSize) {
        size_t size = frameSize - used < chunk ? frameSize - used : chunk;
        payloads[count].data = frame + used;
        payloads[count].size = size;
        payloads[count].intact = true;
        payloads[count].recordStart = used == 0;
        used += size;
        count++;
    }

    TEST_ASSERT_TRUE(pyrowave_scan_frame(payloads, count, &stream, 0, collect, NULL,
                                         scratch, sizeof(scratch), &stats));
    TEST_ASSERT_EQUAL_UINT(5, pushCount);
    TEST_ASSERT_FALSE(stats.invalid);
}

static void test_block_smaller_than_its_header_drops_the_frame(void) {
    putSequenceHeader(1, 1, WIDTH, HEIGHT, false);
    putBlockRecord(1, 0, 1);

    TEST_ASSERT_FALSE(scanWholeFrame(0));
    TEST_ASSERT_TRUE(stats.invalid);
    TEST_ASSERT_EQUAL_STRING("block record smaller than its own header", stats.invalidReason);
}

static void test_record_past_the_end_drops_the_frame(void) {
    putSequenceHeader(1, 1, WIDTH, HEIGHT, false);
    /* Announce 40 words but only write 6. */
    putBlockRecord(1, 0, 40);
    frameSize = 8 + 24;

    TEST_ASSERT_FALSE(scanWholeFrame(0));
    TEST_ASSERT_TRUE(stats.invalid);
    TEST_ASSERT_EQUAL_STRING("record runs past the end of the frame", stats.invalidReason);
}

static void test_second_sequence_header_drops_the_frame(void) {
    putSequenceHeader(2, 2, WIDTH, HEIGHT, false);
    putBlockRecord(2, 0, 4);
    putSequenceHeader(2, 2, WIDTH, HEIGHT, false);

    TEST_ASSERT_FALSE(scanWholeFrame(0));
    TEST_ASSERT_TRUE(stats.invalid);
    TEST_ASSERT_EQUAL_STRING("second sequence header in one frame", stats.invalidReason);
}

static void test_block_before_sequence_header_drops_the_frame(void) {
    putBlockRecord(2, 0, 4);
    putSequenceHeader(2, 1, WIDTH, HEIGHT, false);

    TEST_ASSERT_FALSE(scanWholeFrame(0));
    TEST_ASSERT_TRUE(stats.invalid);
    TEST_ASSERT_EQUAL_STRING("block record before the sequence header", stats.invalidReason);
}

static void test_missing_sequence_header_drops_the_frame(void) {
    /* Padding is legal anywhere in a frame, so a frame that is only padding holds no
     * header without breaking any other rule. */
    putPadding(4);

    TEST_ASSERT_FALSE(scanWholeFrame(0));
    TEST_ASSERT_TRUE(stats.invalid);
    TEST_ASSERT_EQUAL_STRING("frame has no sequence header", stats.invalidReason);
}

static void test_sequence_header_from_another_size_drops_the_frame(void) {
    putSequenceHeader(2, 1, 1280, 720, false);
    putBlockRecord(2, 0, 4);

    TEST_ASSERT_FALSE(scanWholeFrame(0));
    TEST_ASSERT_TRUE(stats.invalid);
    TEST_ASSERT_EQUAL_STRING("sequence header does not match the negotiated stream",
                             stats.invalidReason);
}

static void test_444_header_matches_a_444_stream(void) {
    PyrowaveStreamInfo yuv444 = {WIDTH, HEIGHT, true};
    PyrowavePayload payload;

    putSequenceHeader(2, 1, WIDTH, HEIGHT, true);
    putBlockRecord(2, 0, 4);

    payload.data = frame;
    payload.size = frameSize;
    payload.intact = true;
    payload.recordStart = true;

    TEST_ASSERT_TRUE(pyrowave_scan_frame(&payload, 1, &yuv444, 0, collect, NULL,
                                         scratch, sizeof(scratch), &stats));
    TEST_ASSERT_TRUE(stats.chroma444);
}

static void test_block_record_of_another_frame_drops_the_frame(void) {
    putSequenceHeader(3, 2, WIDTH, HEIGHT, false);
    putBlockRecord(4, 0, 4);

    TEST_ASSERT_FALSE(scanWholeFrame(0));
    TEST_ASSERT_TRUE(stats.invalid);
    TEST_ASSERT_EQUAL_STRING("block record from another frame", stats.invalidReason);
}

static void test_block_index_outside_the_frame_drops_the_frame(void) {
    putSequenceHeader(3, 2, WIDTH, HEIGHT, false);
    putBlockRecord(3, 1u << 20, 4);

    TEST_ASSERT_FALSE(scanWholeFrame(0));
    TEST_ASSERT_TRUE(stats.invalid);
    TEST_ASSERT_EQUAL_STRING("block index outside the frame", stats.invalidReason);
}

static void test_coarse_block_limit_matches_the_host_layout(void) {
    /* The host protects the block indices below 12 * ceil(W/32) * ceil(H/32), with W
     * and H at least 128. For 1920x1080 that is 12 * 60 * 34. */
    TEST_ASSERT_EQUAL_UINT(12u * 60u * 34u, pyrowave_coarse_block_limit(WIDTH, HEIGHT));
    /* Below the 128 pixel floor the floor is what counts. */
    TEST_ASSERT_EQUAL_UINT(12u * 4u * 4u, pyrowave_coarse_block_limit(64, 64));
}


/**
 * A block record whose header arrived but whose body did not: the record has a known
 * end, so the scanner skips exactly that record and keeps parsing after it.
 */
static void test_lost_block_body_is_skipped_and_parsing_continues(void) {
    putSequenceHeader(5, 2, WIDTH, HEIGHT, false);   /* bytes  0.. 8 */
    putBlockRecord(5, 0, 4);                         /* bytes  8..24 */
    putBlockRecord(5, 1, 6);                         /* bytes 24..48 */
    putBlockRecord(5, 2, 4);                         /* bytes 48..64 */
    TEST_ASSERT_EQUAL_UINT(64, (unsigned) frameSize);

    PyrowavePayload payloads[3] = {
            /* The header, block 0 and the header of block 1 arrive. */
            {frame, 32, true, true},
            /* Eight bytes of block 1's body are lost. */
            {NULL, 8, false, false},
            /* The rest of block 1 and all of block 2 arrive. */
            {frame + 40, 24, true, false},
    };

    TEST_ASSERT_TRUE(pyrowave_scan_frame(payloads, 3, &stream, 0, collect, NULL,
                                         scratch, sizeof(scratch), &stats));
    TEST_ASSERT_FALSE(stats.invalid);
    TEST_ASSERT_FALSE(stats.truncated);
    TEST_ASSERT_EQUAL_UINT(3, stats.recordsPushed);
    TEST_ASSERT_EQUAL_UINT(1, stats.recordsSkipped);
    TEST_ASSERT_EQUAL_UINT(0, stats.recordsDesynchronized);
    /* Header, block 0 and block 2 out of three expected records: the area block 1
     * covers comes out blurred for this frame, which is the whole point of intra-only. */
    TEST_ASSERT_TRUE(pyrowave_frame_worth_decoding(&stats));
}

/**
 * When the record header itself is lost there is no length to trust, so parsing resumes
 * at the next payload the host flagged as starting with a record.
 */
static void test_lost_record_header_resyncs_on_the_record_start_flag(void) {
    putSequenceHeader(6, 2, WIDTH, HEIGHT, false);   /* bytes 0..8   */
    putBlockRecord(6, 0, 6);                         /* bytes 8..32  */
    putBlockRecord(6, 1, 6);                         /* bytes 32..56 */
    putBlockRecord(6, 2, 6);                         /* bytes 56..80 */
    TEST_ASSERT_EQUAL_UINT(80, (unsigned) frameSize);

    PyrowavePayload payloads[4] = {
            {frame, 8, true, true},
            /* Lost whole, together with its record header. */
            {NULL, 24, false, true},
            {frame + 32, 24, true, true},
            {frame + 56, 24, true, true},
    };

    TEST_ASSERT_TRUE(pyrowave_scan_frame(payloads, 4, &stream, 0, collect, NULL,
                                         scratch, sizeof(scratch), &stats));
    TEST_ASSERT_FALSE(stats.invalid);
    TEST_ASSERT_EQUAL_UINT(1, stats.recordsDesynchronized);
    TEST_ASSERT_EQUAL_UINT(3, stats.recordsPushed);
    TEST_ASSERT_TRUE(pyrowave_frame_worth_decoding(&stats));
}

/**
 * Without a record start flag to land on there is nothing to resync to and the rest of
 * the frame is given up, but what already arrived is still delivered.
 */
static void test_loss_without_a_record_start_gives_up_on_the_rest(void) {
    putSequenceHeader(6, 3, WIDTH, HEIGHT, false);
    putBlockRecord(6, 0, 6);
    putBlockRecord(6, 1, 6);
    putBlockRecord(6, 2, 6);

    PyrowavePayload payloads[2] = {
            {frame, 8, true, true},
            {NULL, frameSize - 8, false, false},
    };

    TEST_ASSERT_TRUE(pyrowave_scan_frame(payloads, 2, &stream, 0, collect, NULL,
                                         scratch, sizeof(scratch), &stats));
    TEST_ASSERT_FALSE(stats.invalid);
    TEST_ASSERT_TRUE(stats.truncated);
    TEST_ASSERT_EQUAL_UINT(1, stats.recordsPushed);
    TEST_ASSERT_EQUAL_UINT(0, stats.recordsDesynchronized);
}

/**
 * The host announces how many leading payloads hold the coarsest wavelet level. Losing
 * one of them means the frame cannot be decoded at all, so the caller must not try.
 */
static void test_loss_inside_the_critical_packets_makes_the_frame_undecodable(void) {
    putSequenceHeader(7, 2, WIDTH, HEIGHT, false);   /* payload 0 */
    putBlockRecord(7, 0, 6);                         /* payload 1 */
    putBlockRecord(7, 1, 6);                         /* payload 2 */

    PyrowavePayload payloads[3] = {
            {frame, 8, true, true},
            {NULL, 24, false, false},
            {frame + 32, 24, true, true},
    };

    /* Two critical payloads: the loss falls inside them. */
    TEST_ASSERT_TRUE(pyrowave_scan_frame(payloads, 3, &stream, 2, collect, NULL,
                                         scratch, sizeof(scratch), &stats));
    TEST_ASSERT_TRUE(stats.criticalLost);
    TEST_ASSERT_FALSE(pyrowave_frame_worth_decoding(&stats));
}

/** Loss after the critical packets costs detail, not the frame. */
static void test_loss_after_the_critical_packets_only_costs_detail(void) {
    putSequenceHeader(7, 1, WIDTH, HEIGHT, false);   /* payload 0, bytes  0.. 8 */
    putBlockRecord(7, 0, 8);                         /* payload 1, bytes  8..40 */
    putBlockRecord(7, 1, 6);                         /* payload 2, bytes 40..64 */
    TEST_ASSERT_EQUAL_UINT(64, (unsigned) frameSize);

    PyrowavePayload payloads[3] = {
            {frame, 8, true, true},
            {frame + 8, 32, true, true},
            {NULL, 24, false, false},
    };

    /* The two payloads that carry the coarse level both arrived. */
    TEST_ASSERT_TRUE(pyrowave_scan_frame(payloads, 3, &stream, 2, collect, NULL,
                                         scratch, sizeof(scratch), &stats));
    TEST_ASSERT_FALSE(stats.criticalLost);
    TEST_ASSERT_FALSE(stats.invalid);
    TEST_ASSERT_TRUE(stats.truncated);
    TEST_ASSERT_EQUAL_UINT(2, stats.recordsPushed);
    TEST_ASSERT_TRUE(pyrowave_frame_worth_decoding(&stats));
}

static void test_frame_without_blocks_is_still_a_picture(void) {
    /* A still picture can encode to nothing but the sequence header. */
    putSequenceHeader(0, 0, WIDTH, HEIGHT, false);

    TEST_ASSERT_TRUE(scanWholeFrame(0));
    TEST_ASSERT_FALSE(stats.invalid);
    TEST_ASSERT_EQUAL_UINT(1, stats.recordsPushed);
    TEST_ASSERT_TRUE(pyrowave_frame_worth_decoding(&stats));
}

static void test_padding_only_frame_between_records_is_ignored(void) {
    putPadding(3);
    putSequenceHeader(1, 1, WIDTH, HEIGHT, false);
    putPadding(1);
    putBlockRecord(1, 0, 4);
    putPadding(4);

    /* A padding record before the sequence header is not a header, and the frame must
     * not be rejected for it. */
    TEST_ASSERT_TRUE(scanWholeFrame(0));
    TEST_ASSERT_EQUAL_UINT(3, stats.paddingRecords);
    TEST_ASSERT_EQUAL_UINT(2, stats.recordsPushed);
}

static void test_frame_too_large_for_the_scan_buffer_is_refused(void) {
    buildCleanFrame();

    PyrowavePayload payload = {frame, frameSize, true, true};
    uint8_t tiny[8];

    TEST_ASSERT_FALSE(pyrowave_scan_frame(&payload, 1, &stream, 0, collect, NULL,
                                          tiny, sizeof(tiny), &stats));
    TEST_ASSERT_TRUE(stats.invalid);
    TEST_ASSERT_EQUAL_STRING("frame does not fit the scan buffer", stats.invalidReason);
}


int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clean_frame_pushes_every_record_in_order);
    RUN_TEST(test_records_may_straddle_payloads);
    RUN_TEST(test_block_smaller_than_its_header_drops_the_frame);
    RUN_TEST(test_record_past_the_end_drops_the_frame);
    RUN_TEST(test_second_sequence_header_drops_the_frame);
    RUN_TEST(test_block_before_sequence_header_drops_the_frame);
    RUN_TEST(test_missing_sequence_header_drops_the_frame);
    RUN_TEST(test_sequence_header_from_another_size_drops_the_frame);
    RUN_TEST(test_444_header_matches_a_444_stream);
    RUN_TEST(test_block_record_of_another_frame_drops_the_frame);
    RUN_TEST(test_block_index_outside_the_frame_drops_the_frame);
    RUN_TEST(test_coarse_block_limit_matches_the_host_layout);
    RUN_TEST(test_lost_block_body_is_skipped_and_parsing_continues);
    RUN_TEST(test_lost_record_header_resyncs_on_the_record_start_flag);
    RUN_TEST(test_loss_without_a_record_start_gives_up_on_the_rest);
    RUN_TEST(test_loss_inside_the_critical_packets_makes_the_frame_undecodable);
    RUN_TEST(test_loss_after_the_critical_packets_only_costs_detail);
    RUN_TEST(test_frame_without_blocks_is_still_a_picture);
    RUN_TEST(test_padding_only_frame_between_records_is_ignored);
    RUN_TEST(test_frame_too_large_for_the_scan_buffer_is_refused);
    return UNITY_END();
}
