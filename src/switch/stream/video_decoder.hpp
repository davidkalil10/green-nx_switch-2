#pragma once

#include <SDL2/SDL.h>

#include <cstdint>
#include <memory>
#include <vector>

#ifdef __SWITCH2__
extern "C" {
#include <switch2/video/decoder.h>
}
#include "decoded_frame.hpp"
#else
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}
#endif

namespace gnx::stream {

// H.264 decoder: GSV1/viddec on Switch 2, NVDEC (nvtegra hwaccel) on Switch 1,
// software elsewhere.
// Input: complete Annex-B access units from the RTP depacketizer.
// Output:
//  - Switch 2: pitch-linear NV12 frame (Switch2Frame) for GlesVideoRenderer.
//  - Switch 1: raw AV_PIX_FMT_NVTEGRA surface (AVFrame*) for DkVideoRenderer.
//  - PC: tightly-packed I420 SDL texture (SDL_PIXELFORMAT_IYUV).
class VideoDecoder {
public:
    bool init(SDL_Renderer* renderer);
    void shutdown();

    // Feed one access unit; returns true if a new frame was decoded.
    bool decode(const uint8_t* data, size_t size);

    // True (and reset) if a decode error/corrupt frame was seen since the last
    // call -- the caller should ask the server for a fresh keyframe.
    bool take_error() {
        bool e = error_;
        error_ = false;
        return e;
    }

    SDL_Texture* texture() { return texture_; }
#ifdef __SWITCH2__
    std::shared_ptr<const Switch2Frame> current_frame() const {
        return held_frame_;
    }
    bool hardware() const { return hw_ready_; }
#else
    // Latest decoded frame. On Switch 1 this is a held reference to the raw
    // NVTEGRA hardware surface (kept un-transferred for zero-copy deko3d
    // rendering, and ref'd so it survives the next avcodec_receive_frame which
    // would otherwise unref frame_).
    AVFrame* current_frame() { return held_frame_; }
    bool hardware() const { return hw_device_ != nullptr; }
#endif
    int width() const { return width_; }
    int height() const { return height_; }
    // Name of the last decoded frame's pixel format (diagnostics).
    const char* last_pixfmt() const { return last_pixfmt_; }

private:
#ifdef __SWITCH2__
    bool ensure_decoder(const uint8_t* data, size_t size);
    void close_decoder();

    ::VideoDecoder decoder_{};
    bool hw_ready_ = false;
    std::vector<uint8_t> codec_config_;
    std::shared_ptr<const Switch2Frame> held_frame_;
    s64 frame_index_ = 0;
    int colorspace_ = 1;  // BT.709 default for xCloud
    bool color_full_ = false;
#else
    bool ensure_texture(SDL_Renderer* renderer, int width, int height);
    bool upload(AVFrame* frame);

    AVCodecContext* context_ = nullptr;
    AVBufferRef* hw_device_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* held_frame_ = nullptr;  // Switch 1: surviving ref of the last frame
    AVPacket* packet_ = nullptr;
    std::vector<uint8_t> i420_;  // scratch for packed I420 upload
#endif

    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int width_ = 0, height_ = 0;
    bool error_ = false;
    const char* last_pixfmt_ = "?";
};

}  // namespace gnx::stream
