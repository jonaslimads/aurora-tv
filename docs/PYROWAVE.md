# PyroWave in Aurora

PyroWave is an intra-only wavelet video codec
([Themaister/pyrowave](https://github.com/Themaister/pyrowave)) that decodes with
Vulkan compute. Aurora can negotiate it with a PyroWave-capable host (Vibepollo) and
decode it on a machine with Vulkan.

## What the codec buys

Every PyroWave frame is an IDR. There are no P-frames, so:

* a lost packet costs **one frame**, not a GOP — no keyframe request, no second of
  artifacts, recovery on the next frame;
* there is no IDR-refresh interval to tune, and no `DR_NEED_IDR` recovery path;
* a frame that lost packets still decodes: the blocks whose packets never arrived
  simply come out blurred for that frame.

The trade is bitrate: intra-only costs more of it at the same quality.

## Platform support

| Target | Codec built | Offered to host | Why |
|---|---|---|---|
| webOS TV | no | no | The webOS SDK sysroot ships EGL 1.4 and GLES 1/2 (`libEGL.so.1.4.0`, `libGLESv2.so`), no `libvulkan` and no GLES 3.x. PyroWave needs Vulkan compute (or, on the mobile path, float render targets), neither of which exists on the TV. |
| Steam Link | no | no | The compositor owns the GPU; no usable Vulkan device for the codec. |
| Native desktop | yes (`ENABLE_PYROWAVE=ON`) | only with Settings → Experimental → PyroWave codec on | A Vulkan device is probed at start-up (`aurora_pyrowave_available()`). No device, no offer. |

`ENABLE_PYROWAVE` is forced OFF for webOS and Steam Link in the root `CMakeLists.txt`,
and the client code compiles both ways: without the codec,
`aurora_pyrowave_available()` returns false and the settings checkbox shows why.

## Presentation, and its limit today

Every SS4S video driver is a **bitstream sink**: it takes an encoded stream and presents
it on a hardware overlay under the UI. PyroWave frames are decoded by the client into
plain YUV planes, and no SS4S driver accepts client-decoded frames — there is no raw
plane feed in `SS4S_PlayerVideoFeed`. So decoded PyroWave frames currently go to
`pyrowave_present.c`, which writes them out when `AURORA_PYROWAVE_DUMP=<prefix>` is set
(raw `yuv420p`, playable with
`ffplay -f rawvideo -pixel_format yuv420p -video_size WxH <prefix>.i420`) and otherwise
logs that there is nowhere to show them.

A real display integration needs one of:

* a raw-plane sink module in SS4S (`SS4S_PlayerVideoFeedPlanes`), presenting planes
  through the same path the decoders use; or
* a Vulkan importer so Aurora's own textures can be PyroWave's output images, which is
  what the codec's GPU buffer API is for and which requires the compositor to be Vulkan
  too.

The negotiation, depacketization, loss handling and decode path below are complete and
independent of which of those two lands later.

## Negotiation

Codec bits are client-local (`Limelight.h`): `VIDEO_FORMAT_PYROWAVE`,
`VIDEO_FORMAT_PYROWAVE_444`, `VIDEO_FORMAT_PYROWAVE_HDR10`, `VIDEO_FORMAT_PYROWAVE_HDR10_444`,
grouped by `VIDEO_FORMAT_MASK_PYROWAVE`, and folded into `VIDEO_FORMAT_MASK_10BIT` and
`VIDEO_FORMAT_MASK_YUV444` as appropriate. On the wire they map to the host's
`SCM_PYROWAVE*` bits in `serverCodecModeSupport`:

1. The client asks for PyroWave by setting its bits in `supportedVideoFormats`
   (only when the user enabled it and a decoder exists — see `session.c`).
2. `RTSP DESCRIBE` answers with `a=rtpmap:99 PYROWAVE/90000` and
   `a=x-ss-pyrowave.bitstream:<id>`. `RtspConnection.c` picks the profile out of the
   host's `SCM_PYROWAVE*` bits and compares the bitstream id.
3. The **bitstream id must match**. PyroWave's bitstream carries no version field, so a
   host and a client built from different pyrowave commits decode garbage that looks
   like video. Aurora ships `PYROWAVE_BITSTREAM_ID "186f0393"` (pyrowave commit
   `186f0393b77f7755953b5ecde994bb1cec2e4155`, see `third_party/pyrowave/VENDOR.txt`)
   and fails the session on a mismatch instead of showing a corrupt picture. If a host
   simply does not offer PyroWave, the session falls back to the normal codecs.
4. `RTSP ANNOUNCE` selects it with `x-nv-vqos[0].bitStreamFormat=3` and advertises the
   features this client implements:
   `x-ss-video[0].pyrowaveFeatures=1` (record framing), with adaptive FEC and adaptive
   bitrate off. Slice-count attributes are not sent: they belong to HEVC.

## Record framing

PyroWave's bitstream is a run of 32-bit little-endian records, and its own parser needs
one record at a time. RTP payloads do not respect record boundaries — a payload can hold
three records or half of one, and a record can straddle two payloads — so the client
reassembles records itself. `src/app/stream/video/pyrowave_frame.c` is that parser: a
pure C scanner, free of session state, driven by a callback, which is why it is unit
tested against hand-built frames (`tests/app/stream/video/test_pyrowave_frame.c`).

Rules it enforces, each of them from `bitstream/bitstream.md` and each one a frame drop
rather than a partial decode:

* a block record must be at least as long as its own 8-byte header — otherwise the
  codec's parser cannot advance;
* a record must fit inside the frame;
* exactly one sequence header, first, and never a second one;
* the header must match the negotiated chroma resolution, and the encoded size may only
  exceed the negotiated size by the padding the codec's 32x32 blocks require (the host
  encodes whole blocks and an even frame for 4:2:0);
* a block record's `sequence` must agree with the header's, and its `block_index` must
  be inside the frame — the codec indexes a per-block table with it, so a wild index is
  an out-of-bounds write;
* records tile the frame exactly, so a short end means the last record was cut off.

What it does with loss:

* a record whose **header** survived has a known length: it is skipped and parsing
  continues at the next record;
* a record whose header was lost has no known length: parsing resumes at the next
  payload the host flagged as starting on a record boundary
  (`BUFFER_TYPE_RECORD_START`, from the host's `extraFlags & 0x80`), and if there is no
  such flag the rest of the frame is given up;
* **padding records are dropped, never pushed**: read as a record header, a padding
  record's top bit looks like a sequence header and would reset the decoder;
* the frame's critical packets (`pyrowave_critical_packets` in the host's short frame
  header) cover the coarsest wavelet level. A loss inside them means the frame cannot be
  decoded at all; a loss outside them only costs sharpness.

`pyrowave_frame_worth_decoding()` is the cheap pre-filter in front of that: well formed,
no critical loss, and more than 90 % of the blocks the header expected — the same floor
the codec applies. It is a pre-filter, not the authority: `pyrowave_decoder_decode_is_ready()`
also wants the codec's coarsest bands complete and counts blocks its own way, so it
decides. Refusing early is never wrong here — every frame stands alone, so the cost of a
"no" is one frame.

## Decode path

`src/app/stream/video/pyrowave_decode.c` owns one PyroWave decoder per session:

* one Vulkan device for the process (the codec has no device teardown entry point);
* the decoder is sized to the padded extent the codec can write (whole 32-row blocks,
  even width for 4:2:0) and rebuilt once if the first frame's sequence header says the
  host encodes something slightly different, which the codec requires to match exactly;
* `pyrowave_decoder_clear()` before every frame — without it the 3-bit sequence counter
  starts discarding frames after four consecutive drops;
* `pyrowave_decoder_decode_cpu_buffer_synchronous()` for readback. That path stores one
  byte per sample whatever the stream depth, so a 10-bit (HDR10) session arrives reduced
  to 8 bits; the GPU image path is what a real display integration would use to keep the
  extra bits;
* `allow_partial_frame` is passed to the codec when the scanner reports missing records.

`vdec_delegate_submit()` routes PyroWave frames to this path and never touches
`SS4S_PlayerVideoFeed`. `PLENTRY.bufferType` carries `BUFFER_TYPE_LOST` and
`BUFFER_TYPE_RECORD_START` for the cases the core can report them; today the core drops
an unrecoverable frame instead of delivering a partial one, which costs one frame and is
the safe behaviour for an intra-only codec.

## Verifying it

```bash
# The record framing scanner (no GPU needed).
cmake -B build -GNinja -DTARGET_DESKTOP=ON -DBUILD_TESTS=ON -DENABLE_PYROWAVE=ON
cmake --build build && ctest --test-dir build -R pyrowave

# Encode → RTP-sized shards → scanner → decode, on a Vulkan machine.
cmake --build build --target pyrowave-roundtrip && ./build/tools/pyrowave/pyrowave-roundtrip
```

`pyrowave-roundtrip` drives the vendored encoder, cuts the bitstream the way a host does
(1376 byte shards, record-start flags on the payloads that begin a record, padding
records injected between blocks) and then runs the result through **Aurora's own
scanner** — it links `pyrowave_frame.c` rather than repeating the rules — before checking
the decoded planes against the source. It reports `SKIPPED` and exits 0 where there is no
Vulkan device, so it is safe in a build container.

Measured on an AMD GPU with the pinned codec: full frame decodes at 33.8 dB luma PSNR
across 1288 records; a frame with two payloads dropped at the tail is parsed by the
scanner (one record skipped, the rest of the frame given up after the hole) and the codec
declines to build it, which costs the session exactly one frame.

## Files

| File | Role |
|---|---|
| `core/moonlight-common-c/src/{Limelight,RtspConnection,SdpGenerator,VideoDepacketizer}.c/h` | codec bits, DESCRIBE/ANNOUNCE negotiation, record-start and critical-packet metadata |
| `src/app/stream/video/pyrowave_frame.{c,h}` | record scanner, loss rules, worth-decoding rule |
| `src/app/stream/video/pyrowave_decode.{c,h}` | Vulkan device, decoder lifetime, partial-frame decode |
| `src/app/stream/video/pyrowave_present.{c,h}` | where decoded frames go, and what is missing for display |
| `src/app/stream/video/session_video.c` | routes PyroWave frames away from SS4S |
| `src/app/stream/session.c` | advertises PyroWave only when it can be used |
| `third_party/pyrowave/` | vendored codec, pinned commit in `VENDOR.txt` |
| `tests/app/stream/video/test_pyrowave_frame.c` | framing contract, protocol violations, loss behaviour |

## Branches, and one thing to remember when pushing

The desktop target needs SDL >= 2.0.24: upstream's own settings pane reads
`SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_*`, so an older distro package (Ubuntu 22.04
ships SDL 2.0.20) fails to build the unmodified tree.

The RTSP, SDP and depacketizer half of this feature is not in this repository — it is in
the `core/moonlight-common-c` submodule, on its `aurora-pyrowave` branch. A branch that
carries PyroWave is only complete when that submodule branch is pushed too, otherwise the
submodule pointer in the superproject names a commit nobody else can fetch. Its remote is
the upstream `GuiDev1994/moonlight-common-c`, so publishing it needs a fork of that
repository first, and `.gitmodules` should then point at it.

## Where this came from

Nothing about PyroWave was invented here; it is already shipping on both ends of a
Vibepollo session, and this port follows those choices.

**Host — Vibepollo** (`/home/jonas/projects/gaming/Vibepollo`):

* `docs/pyrowave-protocol.md` — the negotiation and framing contract this client
  implements: the `SCM_PYROWAVE*` bits, `a=rtpmap:99 PYROWAVE/90000`,
  `a=x-ss-pyrowave.bitstream:<id>`, `x-nv-vqos[0].bitStreamFormat=3`, the
  `x-ss-video[0].pyrowave*` ANNOUNCE attributes, and why the bitstream id has to match.
* `src/pyrowave_protocol.h` — the constants, mirrored in `Limelight.h`.
* `third-party/pyrowave/` — the vendored codec, and the commit this client pins to
  (`186f0393b77f7755953b5ecde994bb1cec2e4155`, id `186f0393`).
* Its record packager is what the record-start flag and the critical packet count exist
  for: the host frames records inside RTP payloads because PyroWave's own parser needs
  one record at a time.

**Client — moonlight-qt** (`/home/jonas/projects/gaming/moonlight-qt`). The PyroWave
work is not on `origin/pyrowave`, which carries only the first five commits; the complete
history (26 commits) is on `origin/vrr17` and `origin/release/6.1.0-vrr18`. The parts that
map onto this port:

* `app/streaming/video/pyrowave/pyrowaveframing.{h,cpp}` — the record reassembly that
  `pyrowave_frame.c` corresponds to.
* `app/streaming/video/pyrowave/pyrowavedecoder.{h,cpp}` — the decode stage, which is
  `pyrowave_decode.c` here.
* `app/streaming/video/ffmpeg-renderers/d3d11pyrowave.{h,cpp}` and
  `app/streaming/video/pyrowave/pyrowaveplacebo.cpp` — presentation. That is the piece
  Aurora cannot copy: moonlight-qt owns its renderer, so it uploads the decoded planes
  into its own D3D11/Placebo pipeline, while Aurora hands video to SS4S's overlay.
* `app/backend/systemproperties.{h,cpp}` and `app/settings/streamingpreferences.{h,cpp}` —
  advertising the codec only when the machine can take it, and a user switch, which
  `aurora_pyrowave_available()` plus Settings → Experimental mirror.
* `app/streaming/video/pyrowave/pyrowavecalibrator.{h,cpp}` — bitrate calibrated against
  measured host-to-client throughput. Aurora does not port that; its adaptive bitrate
  loop is tuned for interlaced codecs where the cost of a lost packet is a long recovery,
  which is precisely what PyroWave removes.
