#include "pyrowave_present.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logging.h"

/* PyroWave frames are decoded by the client, so they cannot go to SS4S: every SS4S video
 * driver is a bitstream sink that presents an encoded stream on a hardware overlay
 * underneath the UI. Until Aurora grows a path for client-decoded frames, this sink
 * writes them out so the codec path is still observable end to end, and says so. */

static FILE *dumpFile;
static char dumpPath[512];
static uint64_t presentedFrames;
static uint64_t droppedFrames;

static bool sink_open_once(void) {
    if (dumpFile || dumpPath[0]) {
        return dumpFile != NULL;
    }

    const char *prefix = getenv("AURORA_PYROWAVE_DUMP");
    if (prefix == NULL || prefix[0] == '\0') {
        dumpPath[0] = '\0';
        return false;
    }

    snprintf(dumpPath, sizeof(dumpPath), "%s.i420", prefix);
    dumpFile = fopen(dumpPath, "wb");
    if (!dumpFile) {
        commons_log_error("PyroWave", "Cannot open the dump file %s", dumpPath);
        return false;
    }

    commons_log_info("PyroWave", "Writing decoded frames to %s (raw yuv420p, play with "
                                 "ffplay -f rawvideo -pixel_format yuv420p -video_size WxH)", dumpPath);
    return true;
}

static bool write_plane(FILE *f, const uint8_t *plane, size_t stride, size_t width, size_t height) {
    if (stride == width) {
        return fwrite(plane, 1, width * height, f) == width * height;
    }
    for (size_t row = 0; row < height; row++) {
        if (fwrite(plane + row * stride, 1, width, f) != width) {
            return false;
        }
    }
    return true;
}

bool aurora_pyrowave_present(const aurora_pyrowave_frame_t *frame) {
    presentedFrames++;

    if (!sink_open_once()) {
        /* A frame every 5 s is worth a line; the rest would be noise. */
        if (droppedFrames % 300 == 0) {
            commons_log_warn("PyroWave", "decoded %llu frames but this build has no place to "
                                         "show client-decoded video (set AURORA_PYROWAVE_DUMP to "
                                         "write the frames out)", (unsigned long long) presentedFrames);
        }
        droppedFrames++;
        return false;
    }

    size_t chromaWidth = (size_t) frame->width / 2;
    size_t chromaHeight = (size_t) frame->height / 2;

    bool ok = write_plane(dumpFile, frame->planes[0], frame->strides[0], (size_t) frame->width,
                          (size_t) frame->height) &&
              write_plane(dumpFile, frame->planes[1], frame->strides[1], chromaWidth, chromaHeight) &&
              write_plane(dumpFile, frame->planes[2], frame->strides[2], chromaWidth, chromaHeight);

    if (!ok) {
        commons_log_error("PyroWave", "Failed writing a decoded frame to %s", dumpPath);
        fclose(dumpFile);
        dumpFile = NULL;
        return false;
    }

    return true;
}

void aurora_pyrowave_sink_describe(char *out, size_t outSize) {
    const char *prefix = getenv("AURORA_PYROWAVE_DUMP");
    if (prefix != NULL && prefix[0] != '\0') {
        snprintf(out, outSize, "decoded frames are written to %s.i420", prefix);
    } else {
        snprintf(out, outSize, "no sink for client-decoded video in this build");
    }
}
