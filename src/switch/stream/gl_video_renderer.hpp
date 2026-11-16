#pragma once

// OpenGL ES 3.0 video renderer for Switch 2 (OunceVK via SDL2 GL context).
// Uploads pitch-linear NV12 frames from libounce's hardware VideoDecoder into
// R8/RG8 textures and performs YUV->RGB conversion + luma unsharp-masking +
// HUD overlay on the GPU.

#include <SDL2/SDL.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "decoded_frame.hpp"

struct _TTF_Font;
typedef struct _TTF_Font TTF_Font;

namespace gnx::stream {

class GlesVideoRenderer {
public:
    using LogFn = std::function<void(const char*)>;

    GlesVideoRenderer() = default;
    ~GlesVideoRenderer();

    GlesVideoRenderer(const GlesVideoRenderer&) = delete;
    GlesVideoRenderer& operator=(const GlesVideoRenderer&) = delete;

    void set_logger(LogFn fn) { log_ = std::move(fn); }

    void set_sharpness(int level) {
        sharpness_ = level < 0 ? 0 : (level > 3 ? 3 : level);
    }

    void set_hud_enabled(bool e) { hud_enabled_ = e; }

    void set_notice(const std::string& text) {
        std::lock_guard<std::mutex> lock(notice_mutex_);
        if (notice_ == text) return;
        notice_ = text;
        notice_on_.store(!text.empty(), std::memory_order_relaxed);
    }

    void set_net_stats(float mbps, float loss_pct, int buffer_ms) {
        net_mbps_.store(mbps, std::memory_order_relaxed);
        net_loss_.store(loss_pct, std::memory_order_relaxed);
        net_buffer_ms_.store(buffer_ms, std::memory_order_relaxed);
        net_valid_.store(true, std::memory_order_relaxed);
    }

    bool init(SDL_Window* window);
    void shutdown();

    bool render(const Switch2Frame* frame);
    bool initialized() const { return initialized_; }

private:
    void logf(const char* fmt, ...);
    void maybe_update_output_mode();
    bool ensure_textures(int width, int height);
    void update_hud(const Switch2Frame* frame);
    void rasterize_hud();
    void blit_text(const char* s, int x, int y);

    LogFn log_;
    bool initialized_ = false;
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_ctx_ = nullptr;

    uint32_t fb_width_ = 1280;
    uint32_t fb_height_ = 720;
    int fb_mode_ = -1;

    uint32_t video_prog_ = 0;
    uint32_t hud_prog_ = 0;
    uint32_t vao_ = 0;
    uint32_t vbo_ = 0;
    uint32_t hud_vbo_ = 0;
    uint32_t y_tex_ = 0;
    uint32_t uv_tex_ = 0;
    uint32_t hud_tex_ = 0;

    int loc_yuvmat_ = -1;
    int loc_offset_ = -1;
    int loc_sharp_ = -1;

    int tex_w_ = 0;
    int tex_h_ = 0;
    int sharpness_ = 0;
    bool hud_enabled_ = false;
    bool logged_surface_ = false;

    std::mutex notice_mutex_;
    std::string notice_;
    std::atomic<bool> notice_on_{false};

    static constexpr uint32_t kHudTexW = 512;
    static constexpr uint32_t kHudTexH = 160;
    TTF_Font* hud_font_ = nullptr;
    std::vector<uint32_t> hud_pixels_;
    std::string hud_text_cache_;
    bool hud_dirty_ = false;

    uint64_t fps_tick_ = 0;
    int fps_frames_ = 0;
    const Switch2Frame* fps_last_frame_ = nullptr;
    float fps_ = 0.0f;
    std::atomic<float> net_mbps_{0.0f};
    std::atomic<float> net_loss_{0.0f};
    std::atomic<int> net_buffer_ms_{0};
    std::atomic<bool> net_valid_{false};
};

}  // namespace gnx::stream
