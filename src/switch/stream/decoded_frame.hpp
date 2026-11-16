#pragma once

#include <cstdint>
#include <vector>

namespace gnx::stream {

// Pitch-linear NV12 frame produced by libounce's hardware VideoDecoder on
// Switch 2 and consumed by GlesVideoRenderer. Copying out of the decoder's
// small image pool on the decode thread keeps all viddec state single-threaded
// and ensures Smooth pacing never starves the decoder's output slots.
struct Switch2Frame {
    int width = 0;
    int height = 0;
    int y_pitch = 0;
    int uv_pitch = 0;
    int colorspace = 0;  // AVColorSpace-compatible IDs (1=BT709, 5/6=BT601, 9=BT2020)
    bool color_full = false;
    std::vector<uint8_t> y;
    std::vector<uint8_t> uv;
};

}  // namespace gnx::stream
