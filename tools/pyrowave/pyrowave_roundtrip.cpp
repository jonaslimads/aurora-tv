// PyroWave round-trip verification tool.
//
// Proves, on a real Vulkan device, that a PyroWave frame can be produced by the
// vendored codec, record-framed exactly the way the host (Vibepollo) frames it,
// parsed back record by record by an independent parser, and decoded to pixels.
// It also proves the intra-only loss behaviour (missing blocks blur, they do not
// corrupt the frame). See third_party/pyrowave/VENDOR.txt and the host protocol
// doc "Record framing".
//
// This is a verification harness, not part of the streaming path. It exits 0 on
// success or when no Vulkan device is available (SKIPPED); any failed assertion
// exits 1 with a message.

// pyrowave.h refuses to compile unless the Vulkan core header precedes it.
#include <vulkan/vulkan_core.h>
#include <pyrowave.h>

// The client's own record scanner: this tool verifies the code that ships, not a copy.
#include "stream/video/pyrowave_frame.h"

#include <algorithm>
#include <set>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>

// tests/fixtures/pyrowave, injected by CMake so the tool can write fixtures
// regardless of the working directory ctest runs it from.
#ifndef PYROWAVE_FIXTURE_DIR
#define PYROWAVE_FIXTURE_DIR "."
#endif

#define PYROWAVE_BITSTREAM_ID "186f0393"

namespace
{
// ---- Bitstream record layout (32-bit little-endian words) -------------------
// Mirrors pyrowave_common.hpp's BitstreamHeader / BitstreamSequenceHeader, but
// packed manually so the tool does not depend on bit-field ABI details and does
// not include any header from inside the vendored library core.
//
// BitstreamHeader (a block record):
//   word0 = ballot(16) | payload_words(12) | sequence(3) | extended(1 << 31)
//   word1 = quant_code(8) | block_index(24)
// BitstreamSequenceHeader (extended == 1):
//   word0 = width_minus_1(14) | height_minus_1(14) | sequence(3) | extended(1 << 31)
//   word1 = total_blocks(24) | code(2) | chroma_resolution(1) | primaries(1) |
//           transfer(1) | ycbcr_transform(1) | ycbcr_range(1) | chroma_siting(1)
constexpr uint32_t PADDING_MARKER = 0xFFFFFFFFu;
constexpr uint32_t EXTENDED_BIT = 1u << 31;

// The encoder's per-block index into the raw bitstream, matching BitstreamPacket.
struct RawBlockPacket
{
    uint32_t offset_u32;
    uint32_t num_words;
};

// One parsed record from a serialized frame, as a byte range into that frame.
struct ParsedRecord
{
    const uint8_t *bytes;
    size_t size;      // bytes, payload_words * 4 for a block, 8 for the sequence header
    bool is_sequence_header;
    bool is_padding;
    uint32_t block_index;
    uint32_t payload_words;
};

struct FrameInfo
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t total_blocks = 0;
    uint32_t sequence = 0;
    uint32_t chroma_resolution = 0;
};

inline uint32_t le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = uint8_t(v & 0xff);
    p[1] = uint8_t((v >> 8) & 0xff);
    p[2] = uint8_t((v >> 16) & 0xff);
    p[3] = uint8_t((v >> 24) & 0xff);
}

// Build the 8-byte BitstreamSequenceHeader word pair the way the host's packager
// does (pyrowave_encoder.cpp::packetize), which the raw bitstream omits.
void build_sequence_header(uint8_t out[8], uint32_t width, uint32_t height, uint32_t sequence,
                           uint32_t total_blocks, uint32_t chroma_resolution)
{
    uint32_t w0 = ((width - 1) & 0x3FFFu) | (((height - 1) & 0x3FFFu) << 14) |
                  ((sequence & 0x7u) << 28) | EXTENDED_BIT;
    // code = 0 (BITSTREAM_EXTENDED_CODE_START_OF_FRAME), all colour fields 0 =>
    // BT709 / BT709 transfer / BT709 transform / full range / center siting,
    // matching the SDR BT709 stream this tool encodes.
    uint32_t w1 = (total_blocks & 0xFFFFFFu);
    w1 |= (chroma_resolution & 1u) << 26;
    put_le32(out + 0, w0);
    put_le32(out + 4, w1);
}

// Standalone record-framing parser. Walks a serialized frame, classifies every
// record, and applies the host's rejection rules (pyrowave-protocol.md "Record
// framing" / "Receivers must reject ..."). Returns false with a reason set.
bool parse_records(const std::vector<uint8_t> &frame, std::vector<ParsedRecord> &out, bool &saw_sequence_header,
                   size_t &padding_count, const char *&err)
{
    err = nullptr;
    out.clear();
    saw_sequence_header = false;
    bool saw_first_sequence_header = false;
    padding_count = 0;

    size_t words_total = frame.size() / 4;
    const uint8_t *base = frame.data();
    size_t w = 0;
    while (w < words_total)
    {
        const uint8_t *rec = base + w * 4;
        uint32_t w0 = le32(rec);

        // Padding record: 0xFFFFFFFF, a word count N, then N zero words.
        if (w0 == PADDING_MARKER)
        {
            if (w + 1 >= words_total + 1 && w + 1 >= words_total)
            {
                err = "padding marker at end of frame with no word count";
                return false;
            }
            uint32_t n = le32(rec + 4);
            if (w + 2 + n > words_total)
            {
                err = "padding record runs past the frame end";
                return false;
            }
            ParsedRecord r = {};
            r.bytes = rec;
            r.size = (2 + n) * 4;
            r.is_padding = true;
            out.push_back(r);
            padding_count++;
            w += 2 + n;
            continue;
        }

        bool extended = (w0 & EXTENDED_BIT) != 0;
        if (extended)
        {
            if (w + 2 > words_total)
            {
                err = "sequence header runs past the frame end";
                return false;
            }
            if (saw_first_sequence_header)
            {
                err = "second sequence header in one frame";
                return false;
            }
            ParsedRecord r = {};
            r.bytes = rec;
            r.size = 8;
            r.is_sequence_header = true;
            out.push_back(r);
            saw_first_sequence_header = true;
            saw_sequence_header = true;
            w += 2;
            continue;
        }

        // A block record: reject payload_words < 2 (smaller than the header), a
        // record running past the frame end, and a block before the sequence header.
        uint32_t payload_words = (w0 >> 16) & 0xFFFu;
        if (payload_words < 2)
        {
            err = "block record payload_words < 2";
            return false;
        }
        if (w + payload_words > words_total)
        {
            err = "block record runs past the frame end";
            return false;
        }
        if (!saw_sequence_header)
        {
            err = "block record before the sequence header";
            return false;
        }
        uint32_t w1 = le32(rec + 4);
        ParsedRecord r = {};
        r.bytes = rec;
        r.size = size_t(payload_words) * 4;
        r.block_index = w1 >> 8;
        r.payload_words = payload_words;
        out.push_back(r);
        w += payload_words;
    }
    return true;
}

// ---- Deterministic synthetic source image -----------------------------------
// Luma: a smooth horizontal + vertical gradient, two hard-edged shapes (a bright
// rectangle and a dark disc) for edge response, and a fine checkerboard region so
// mid-frequency retention is meaningful. Chroma carries a gentle ramp so Cb/Cr are
// provably non-blank.
constexpr int SRC_W = 1920;
constexpr int SRC_H = 1080;
constexpr int CB_X = 160, CB_Y = 160, CB_W = 160, CB_H = 160; // checkerboard region (luma px)

int clamp8(int v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return v;
}

void synth_luma(std::vector<uint8_t> &y)
{
    y.resize(size_t(SRC_W) * SRC_H);
    for (int j = 0; j < SRC_H; j++)
    {
        for (int i = 0; i < SRC_W; i++)
        {
            int g = 40 + (i * 120) / (SRC_W - 1) + (j * 50) / (SRC_H - 1); // 40..210 gradient
            // Hard-edged bright rectangle.
            if (i >= 1200 && i < 1650 && j >= 300 && j < 620)
                g = 245;
            // Hard-edged dark disc, centre (500, 780), radius 170.
            int dx = i - 500, dy = j - 780;
            if (dx * dx + dy * dy < 170 * 170)
                g = 12;
            // Checkerboard: 8-px cells, +/- 60 contrast around mid-grey.
            if (i >= CB_X && i < CB_X + CB_W && j >= CB_Y && j < CB_Y + CB_H)
            {
                int cell = ((i - CB_X) / 8 + (j - CB_Y) / 8) & 1;
                g = cell ? 190 : 70;
            }
            // A two-pixel dither everywhere. The codec will not build a partial frame
            // unless every block of the two coarsest wavelet levels was transmitted,
            // and it drops blocks whose coefficients are all zero — a perfectly smooth
            // area is simply not sent. The dither keeps every 32x32 block of the frame
            // alive, which is what real camera footage looks like and what makes the
            // loss case below representative.
            g += ((i & 1) && ((i >> 1) + j) & 1) ? 6 : -6;
            y[size_t(j) * SRC_W + i] = uint8_t(clamp8(g));
        }
    }
}

void synth_chroma(std::vector<uint8_t> &u, std::vector<uint8_t> &v)
{
    int cw = SRC_W / 2, ch = SRC_H / 2;
    u.resize(size_t(cw) * ch);
    v.resize(size_t(cw) * ch);
    for (int j = 0; j < ch; j++)
        for (int i = 0; i < cw; i++)
        {
            // Same reasoning as in synth_luma: the chroma components are part of the
            // coarse bands the codec insists on, so they need detail everywhere too.
            int d = (((i >> 1) + j) & 3) - 1;
            u[size_t(j) * cw + i] = uint8_t(clamp8(128 + (i * 40) / cw - 20 + d * 9));
            v[size_t(j) * cw + i] = uint8_t(clamp8(128 - (j * 40) / ch + 20 - d * 9));
        }
}

// ---- Metrics ----------------------------------------------------------------
double psnr_plane(const uint8_t *a, const uint8_t *b, size_t n)
{
    double se = 0.0;
    for (size_t i = 0; i < n; i++)
    {
        double d = double(a[i]) - double(b[i]);
        se += d * d;
    }
    if (se == 0.0)
        return 1e9; // lossless
    double mse = se / double(n);
    return 10.0 * std::log10((255.0 * 255.0) / mse);
}

double luma_stddev_region(const uint8_t *y, int w, int x0, int y0, int rw, int rh)
{
    double sum = 0.0, sum2 = 0.0;
    long n = 0;
    for (int j = y0; j < y0 + rh; j++)
        for (int i = x0; i < x0 + rw; i++)
        {
            double s = double(y[size_t(j) * w + i]);
            sum += s;
            sum2 += s * s;
            n++;
        }
    double mean = sum / n;
    double var = sum2 / n - mean * mean;
    return var > 0 ? std::sqrt(var) : 0.0;
}

bool plane_is_blank(const uint8_t *p, size_t n)
{
    bool all_zero = true, all_same = true;
    for (size_t i = 1; i < n; i++)
    {
        if (p[i] != 0)
            all_zero = false;
        if (p[i] != p[0])
            all_same = false;
    }
    if (all_zero)
        return true;
    // A completely flat plane carries no signal; treat as blank.
    return all_same;
}

// Append a padding record (marker, word count N, then N zero words).
void append_padding(std::vector<uint8_t> &frame, uint32_t zero_words)
{
    size_t off = frame.size();
    frame.resize(off + (2 + zero_words) * 4);
    put_le32(frame.data() + off + 0, PADDING_MARKER);
    put_le32(frame.data() + off + 4, zero_words);
    for (uint32_t i = 0; i < zero_words; i++)
        put_le32(frame.data() + off + 8 + i * 4, 0);
}

bool write_file(const char *path, const std::vector<uint8_t> &data)
{
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    size_t w = std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    return w == data.size();
}

#define CHECK(cond, msg)                                                    \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            std::fprintf(stderr, "PYROWAVE ROUNDTRIP FAILED: %s\n", (msg)); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

#define CHECK_RV(expr)                                                             \
    do                                                                             \
    {                                                                              \
        pyrowave_result r_ = (expr);                                              \
        if (r_ != PYROWAVE_SUCCESS)                                                \
        {                                                                          \
            std::fprintf(stderr, "PYROWAVE ROUNDTRIP FAILED: %s -> %d\n", #expr,   \
                         int(r_));                                                 \
            return 1;                                                             \
        }                                                                          \
    } while (0)
} // namespace

int main()
{
    std::printf("PyroWave round-trip tool (PYROWAVE_BITSTREAM_ID=%s)\n", PYROWAVE_BITSTREAM_ID);

    // 1. Bring up a device. Never fail for lack of a GPU.
    pyrowave_device device = nullptr;
    pyrowave_result dev_res = pyrowave_create_default_device(&device);
    if (dev_res != PYROWAVE_SUCCESS || !device)
    {
        std::printf("SKIPPED: pyrowave_create_default_device failed (%d): no Vulkan device or GPU\n", int(dev_res));
        return 0;
    }

    pyrowave_encoder encoder = nullptr;
    pyrowave_decoder decoder = nullptr;
    pyrowave_decoder loss_decoder = nullptr;

    pyrowave_encoder_create_info enc_info = {};
    enc_info.device = device;
    enc_info.width = SRC_W;
    enc_info.height = SRC_H;
    enc_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    CHECK_RV(pyrowave_encoder_create(&enc_info, &encoder));

    pyrowave_decoder_create_info dec_info = {};
    dec_info.device = device;
    dec_info.width = SRC_W;
    dec_info.height = SRC_H;
    dec_info.chroma = PYROWAVE_CHROMA_SUBSAMPLING_420;
    dec_info.fragment_path = false;
    CHECK_RV(pyrowave_decoder_create(&dec_info, &decoder));
    CHECK_RV(pyrowave_decoder_create(&dec_info, &loss_decoder));

    // 2. Deterministic source.
    std::vector<uint8_t> src_y, src_u, src_v;
    synth_luma(src_y);
    synth_chroma(src_u, src_v);

    // 3. Encode via the CPU path (~1.6 bits/pixel, 8-bit 4:2:0 SDR).
    pyrowave_cpu_buffer src_buf = {};
    src_buf.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    src_buf.width = SRC_W;
    src_buf.height = SRC_H;
    src_buf.data[0] = src_y.data();
    src_buf.data[1] = src_u.data();
    src_buf.data[2] = src_v.data();
    src_buf.row_stride_in_bytes[0] = SRC_W;
    src_buf.row_stride_in_bytes[1] = SRC_W / 2;
    src_buf.row_stride_in_bytes[2] = SRC_W / 2;
    src_buf.plane_size_in_bytes[0] = size_t(SRC_W) * SRC_H;
    src_buf.plane_size_in_bytes[1] = size_t(SRC_W) * SRC_H / 4;
    src_buf.plane_size_in_bytes[2] = size_t(SRC_W) * SRC_H / 4;

    // ~1.6 bits per luma pixel over the whole intra-only frame -> bit budget.
    // 1920*1080 * 1.6 / 8 = 414720 bytes, comfortably above the coarse level.
    const double target_bpp = 1.6;
    const size_t max_bitstream = size_t(double(SRC_W) * SRC_H * target_bpp / 8.0);
    pyrowave_rate_control rate = {};
    rate.maximum_bitstream_size = max_bitstream;
    CHECK(rate.maximum_bitstream_size > 0, "bit budget underflow");
    CHECK_RV(pyrowave_encoder_encode_cpu_synchronous(encoder, &src_buf, &rate));

    // 4. Pull the raw record bitstream + per-block index.
    const void *raw = nullptr;
    const void *raw_meta = nullptr;
    size_t raw_size = 0, raw_meta_size = 0;
    CHECK_RV(pyrowave_encoder_get_mapped_raw_bitstream(encoder, &raw, &raw_size, &raw_meta, &raw_meta_size));
    CHECK(raw && raw_meta && raw_size && raw_meta_size, "mapped raw bitstream is empty");

    const RawBlockPacket *meta = static_cast<const RawBlockPacket *>(raw_meta);
    const uint32_t *raw_words = static_cast<const uint32_t *>(raw);
    size_t num_blocks = raw_meta_size / sizeof(RawBlockPacket);

    // Collect the block records and derive the sequence value from block 0's header
    // (every block in the frame shares it), exactly like the host packager does.
    struct BlockRec
    {
        const uint32_t *words;
        uint32_t num_words;
    };
    std::vector<BlockRec> block_records;
    block_records.reserve(num_blocks);
    uint32_t seq_value = 0;
    bool seq_read = false;
    for (size_t i = 0; i < num_blocks; i++)
    {
        if (meta[i].num_words == 0)
            continue; // ballot==0 => not transmitted
        const uint32_t *words = raw_words + meta[i].offset_u32;
        if (!seq_read)
        {
            seq_value = (words[0] >> 28) & 0x7u;
            seq_read = true;
        }
        block_records.push_back({ words, meta[i].num_words });
    }
    CHECK(!block_records.empty(), "encoder produced no block records");
    CHECK(seq_read, "no sequence value found in any block record");
    const uint32_t total_blocks = uint32_t(block_records.size());

    // 5. Serialize the frame exactly like the host record framing: sequence header
    // first, block records after it, with padding records injected between them.
    std::vector<uint8_t> frame;
    // Where each record begins in the frame. The host flags the payloads that start on
    // one of these, and so does this tool.
    std::vector<size_t> record_offsets;
    uint8_t seq_hdr[8];
    build_sequence_header(seq_hdr, SRC_W, SRC_H, seq_value, total_blocks, /*420*/ 0);
    record_offsets.push_back(0);
    frame.insert(frame.end(), seq_hdr, seq_hdr + 8);

    for (size_t i = 0; i < block_records.size(); i++)
    {
        record_offsets.push_back(frame.size());
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(block_records[i].words);
        size_t nbytes = size_t(block_records[i].num_words) * 4;
        frame.insert(frame.end(), bytes, bytes + nbytes);
        // Inject padding: two records early on (after the first record and after a
        // middle record), and a minimal one between every subsequent pair.
        if (i == 0)
            append_padding(frame, 8);
        else if (i == block_records.size() / 2)
            append_padding(frame, 4);
        else if ((i & 7u) == 0)
            append_padding(frame, 1);
    }

    std::vector<ParsedRecord> records;
    size_t padding_count = 0;
    double partial_psnr = 0.0;

    // 6. Cut the frame into RTP-sized payloads exactly like the host packager does,
    //    flag the ones that start on a record boundary, and run them through the
    //    client's own record scanner. From here on this tool exercises the code that
    //    ships in src/app/stream/video/pyrowave_frame.c, not a copy of it.
    const size_t SHARD = 1376;
    std::set<size_t> record_starts;
    for (size_t offset : record_offsets)
        if (offset % SHARD == 0)
            record_starts.insert(offset / SHARD);

    std::vector<PyrowavePayload> payloads;
    std::vector<std::vector<uint8_t>> payload_bytes;
    for (size_t off = 0; off < frame.size(); off += SHARD)
    {
        size_t n = std::min(SHARD, frame.size() - off);
        payload_bytes.emplace_back(frame.begin() + long(off), frame.begin() + long(off + n));
    }
    for (size_t i = 0; i < payload_bytes.size(); i++)
    {
        PyrowavePayload pl = {};
        pl.data = payload_bytes[i].data();
        pl.size = payload_bytes[i].size();
        pl.intact = true;
        // The host sets this when its packager began a record here, which is what
        // lets a client resynchronize after a lost record header.
        pl.recordStart = record_starts.count(i) > 0;
        payloads.push_back(pl);
    }

    PyrowaveStreamInfo stream_info = {};
    stream_info.width = SRC_W;
    stream_info.height = SRC_H;
    stream_info.chroma444 = false;

    size_t scratch_size = frame.size() + (1u << 20);
    std::vector<uint8_t> scratch(scratch_size);

    struct Collect
    {
        std::vector<ParsedRecord> *records;
    } collect = {&records};

    auto scan = [&](const std::vector<PyrowavePayload> &pl, uint32_t critical, PyrowaveScanStats *out) {
        struct Cb
        {
            static bool push(void *userdata, const uint8_t *bytes, size_t size)
            {
                auto *into = static_cast<std::vector<ParsedRecord> *>(userdata);
                ParsedRecord r = {};
                r.bytes = bytes;
                r.size = size;
                into->push_back(r);
                return true;
            }
        };
        return pyrowave_scan_frame(pl.data(), pl.size(), &stream_info, critical, &Cb::push, &records,
                                   scratch.data(), scratch.size(), out);
    };

    PyrowaveScanStats stats = {};
    records.clear();
    CHECK(scan(payloads, 0, &stats), stats.invalidReason ? stats.invalidReason : "client scanner rejected a clean frame");
    CHECK(stats.sequenceHeaderOk, "client scanner saw no sequence header");
    CHECK(stats.width == uint32_t(SRC_W) && stats.height == uint32_t(SRC_H), "client scanner read wrong dimensions");
    CHECK(stats.totalBlocks == total_blocks, "client scanner read wrong total_blocks");
    CHECK(stats.recordsSkipped == 0, "client scanner skipped records of a clean frame");
    padding_count = stats.paddingRecords;
    CHECK(padding_count > 0, "no padding record was injected into the test frame");
    CHECK(stats.recordsPushed == total_blocks + 1, "scanner pushed record count mismatch");

    // The scanner hands over one record at a time; the codec is fed exactly those.
    for (auto &r : records)
        CHECK_RV(pyrowave_decoder_push_packet(decoder, r.bytes, r.size));

    {
        const ParsedRecord &hdr = records.front();
        uint32_t w0 = le32(hdr.bytes), w1 = le32(hdr.bytes + 4);
        CHECK(((w0 & 0x3FFF) + 1) == uint32_t(SRC_W), "sequence header width mismatch");
        CHECK((((w0 >> 14) & 0x3FFF) + 1) == uint32_t(SRC_H), "sequence header height mismatch");
        CHECK((w1 & 0xFFFFFF) == total_blocks, "sequence header total_blocks mismatch");
        CHECK(((w1 >> 26) & 1) == 0, "sequence header chroma mismatch");
    }

    // 7. Full frame must be ready, decode to CPU planes, check quality.
    CHECK(pyrowave_decoder_decode_is_ready(decoder, false), "full frame not decoded-ready after pushing all records");

    std::vector<uint8_t> dec_y(size_t(SRC_W) * SRC_H), dec_u(size_t(SRC_W) * SRC_H / 4),
        dec_v(size_t(SRC_W) * SRC_H / 4);
    pyrowave_cpu_buffer out_buf = {};
    out_buf.format = PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    out_buf.width = SRC_W;
    out_buf.height = SRC_H;
    out_buf.data[0] = dec_y.data();
    out_buf.data[1] = dec_u.data();
    out_buf.data[2] = dec_v.data();
    out_buf.row_stride_in_bytes[0] = SRC_W;
    out_buf.row_stride_in_bytes[1] = SRC_W / 2;
    out_buf.row_stride_in_bytes[2] = SRC_W / 2;
    out_buf.plane_size_in_bytes[0] = size_t(SRC_W) * SRC_H;
    out_buf.plane_size_in_bytes[1] = size_t(SRC_W) * SRC_H / 4;
    out_buf.plane_size_in_bytes[2] = size_t(SRC_W) * SRC_H / 4;
    CHECK_RV(pyrowave_decoder_decode_cpu_buffer_synchronous(decoder, &out_buf));

    double full_psnr = psnr_plane(src_y.data(), dec_y.data(), dec_y.size());
    CHECK(full_psnr >= 30.0, "full-frame luma PSNR below 30 dB");
    CHECK(!plane_is_blank(dec_u.data(), dec_u.size()), "decoded Cb is blank");
    CHECK(!plane_is_blank(dec_v.data(), dec_v.size()), "decoded Cr is blank");
    double cb_std = luma_stddev_region(dec_y.data(), SRC_W, CB_X, CB_Y, CB_W, CB_H);
    CHECK(cb_std >= 10.0, "checkerboard region blurred away (luma stddev too low)");

    // 8. Loss case, on the wire rather than on the record list: drop whole payloads
    //    from the end of the frame, which is where the finest wavelet detail lives.
    //    The client sees holes it cannot place records in, skips what it can and
    //    decodes the frame anyway; that is the whole argument for an intra-only codec.
    {
        std::vector<PyrowavePayload> damaged = payloads;
        size_t dropped = 0;
        // The tail of the frame carries the finest wavelet detail: losing it costs
        // sharpness, while the sequence header and coarse level in the front payloads
        // stay intact.
        for (size_t index = damaged.size() - 1; index > 0 && dropped < 2; index--)
        {
            damaged[index].intact = false;
            dropped++;
        }
        CHECK(dropped == 2, "could not find payloads to drop for the loss case");

        pyrowave_decoder_clear(loss_decoder);
        records.clear();
        PyrowaveScanStats loss_stats = {};
        CHECK(scan(damaged, 0, &loss_stats), loss_stats.invalidReason ? loss_stats.invalidReason : "scanner rejected the damaged frame");
        CHECK(loss_stats.recordsSkipped > 0 || loss_stats.truncated, "damaged frame looked clean to the scanner");
        CHECK(pyrowave_frame_worth_decoding(&loss_stats), "scanner refused a damaged frame it should decode");

        for (auto &r : records)
            CHECK_RV(pyrowave_decoder_push_packet(loss_decoder, r.bytes, r.size));

        CHECK(!pyrowave_decoder_decode_is_ready(loss_decoder, false), "damaged frame wrongly fully ready");

        /* The codec has the final say about whether a damaged frame is worth building:
         * it needs its two coarsest bands complete and more than 90 % of the blocks,
         * and it counts blocks its own way. Either answer is acceptable here — what has
         * to hold is that the client never fed it a malformed record, and that a frame
         * the codec does build is still a picture. If it refuses, the session drops one
         * frame, which is the cost of an intra-only codec. */
        if (pyrowave_decoder_decode_is_ready(loss_decoder, true))
        {
            std::vector<uint8_t> loss_y(size_t(SRC_W) * SRC_H), loss_u(size_t(SRC_W) * SRC_H / 4),
                loss_v(size_t(SRC_W) * SRC_H / 4);
            pyrowave_cpu_buffer loss_buf = out_buf;
            loss_buf.data[0] = loss_y.data();
            loss_buf.data[1] = loss_u.data();
            loss_buf.data[2] = loss_v.data();
            CHECK_RV(pyrowave_decoder_decode_cpu_buffer_synchronous(loss_decoder, &loss_buf));
            partial_psnr = psnr_plane(src_y.data(), loss_y.data(), loss_y.size());
            CHECK(partial_psnr >= 20.0, "partial-frame luma PSNR below 20 dB");
            std::printf("damaged frame decoded: psnr=%.1fdB (pushed=%u, skipped=%u)\n", partial_psnr,
                        loss_stats.recordsPushed, loss_stats.recordsSkipped);
        }
        else
        {
            std::printf("damaged frame refused by the codec (pushed=%u, skipped=%u); the session "
                        "would drop exactly this one frame\n",
                        loss_stats.recordsPushed, loss_stats.recordsSkipped);
        }
    }

    // 9. Write fixtures + report. The fixtures are a convenience for looking at a real
    // frame by hand, so a read-only source tree (a CI build mounted read-write nowhere,
    // or the binary run from elsewhere) is a notice and not a failure.
    std::vector<uint8_t> bin(frame);
    bool fixtures_written = write_file(PYROWAVE_FIXTURE_DIR "/frame_1080p420.bin", bin);
    if (!fixtures_written)
        std::printf("note: could not write fixtures to %s; the checks above still ran\n",
                    PYROWAVE_FIXTURE_DIR);
    if (fixtures_written)
    {
        char path[512];
        std::snprintf(path, sizeof(path), PYROWAVE_FIXTURE_DIR "/frame_1080p420.txt");
        FILE *txt = std::fopen(path, "wb");
        CHECK(txt != nullptr, "failed to open sidecar for write");
        std::fprintf(txt,
                     "PYROWAVE_BITSTREAM_ID=%s\n"
                     "width=%d\n"
                     "height=%d\n"
                     "chroma=420\n"
                     "bitdepth=8\n"
                     "record_count=%zu\n"
                     "block_records=%zu\n"
                     "padding_records=%zu\n"
                     "critical_packet_count=%zu\n"
                     "total_blocks=%u\n"
                     "frame_bytes=%zu\n"
                     "full_luma_psnr_dB=%.2f\n"
                     "partial_luma_psnr_dB=%.2f\n",
                     PYROWAVE_BITSTREAM_ID, SRC_W, SRC_H, records.size(), block_records.size(), padding_count,
                     /* the packets that carry the coarse level; this frame keeps them all in the
                        leading payloads, which is what the host would announce */
                     stats.recordsPushed, total_blocks, frame.size(), full_psnr, partial_psnr);
        std::fclose(txt);
    }

    std::printf("PYROWAVE ROUNDTRIP OK: full_psnr=%.1fdB partial_psnr=%.1fdB records=%zu\n", full_psnr, partial_psnr,
                (size_t) stats.recordsPushed);

    pyrowave_decoder_destroy(decoder);
    pyrowave_decoder_destroy(loss_decoder);
    pyrowave_encoder_destroy(encoder);
    pyrowave_device_destroy(device);
    return 0;
}
