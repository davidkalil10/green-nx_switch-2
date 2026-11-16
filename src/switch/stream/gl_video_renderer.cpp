#include "gl_video_renderer.hpp"

#ifdef __SWITCH2__

#include <switch.h>
#include <GLES3/gl3.h>

#define SDL_MAIN_HANDLED
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace gnx::stream {

namespace {

struct Vertex {
    float x, y;
    float u, v;
};

constexpr Vertex kQuad[4] = {
    {-1.0f, +1.0f, 0.0f, 0.0f},
    {-1.0f, -1.0f, 0.0f, 1.0f},
    {+1.0f, +1.0f, 1.0f, 0.0f},
    {+1.0f, -1.0f, 1.0f, 1.0f},
};

constexpr Vertex kHudQuad[4] = {
    {-0.98f, +0.98f, 0.0f, 0.0f},
    {-0.98f, +0.66f, 0.0f, 1.0f},
    {-0.40f, +0.98f, 1.0f, 0.0f},
    {-0.40f, +0.66f, 1.0f, 1.0f},
};

const char* kVertexShaderSrc = R"(#version 300 es
layout(location = 0) in vec2 inPos;
layout(location = 1) in vec2 inUv;
out vec2 vUv;
void main() {
    gl_Position = vec4(inPos, 0.0, 1.0);
    vUv = inUv;
}
)";

const char* kVideoFragmentSrc = R"(#version 300 es
precision highp float;
in vec2 vUv;
out vec4 outColor;

uniform sampler2D plane0;
uniform sampler2D plane1;
uniform mat3 uYuvMat;
uniform vec3 uOffset;
uniform vec4 uSharpData;

void main() {
    vec2 px = uSharpData.zw;
    vec2 lmax = vec2(1.0) - 0.5 * px;
    vec2 cmax = vec2(1.0) - px;
    vec2 luv = clamp(vUv, 0.5 * px, lmax);

    float y = texture(plane0, luv).r;
    if (uSharpData.x > 0.0) {
        float yl = texture(plane0, clamp(luv - vec2(px.x, 0.0), 0.5 * px, lmax)).r;
        float yr = texture(plane0, clamp(luv + vec2(px.x, 0.0), 0.5 * px, lmax)).r;
        float yu = texture(plane0, clamp(luv - vec2(0.0, px.y), 0.5 * px, lmax)).r;
        float yd = texture(plane0, clamp(luv + vec2(0.0, px.y), 0.5 * px, lmax)).r;
        float average = 0.25 * (yl + yr + yu + yd);
        float lo = min(y, min(min(yl, yr), min(yu, yd)));
        float hi = max(y, max(max(yl, yr), max(yu, yd)));
        y = clamp(y + (y - average) * uSharpData.x,
                  max(0.0, lo - uSharpData.y),
                  min(1.0, hi + uSharpData.y));
    }

    vec2 cuv = clamp(vUv, px, cmax);
    vec2 uv_sample = texture(plane1, cuv).rg;
    vec3 yuv = vec3(y, uv_sample.r, uv_sample.g) - uOffset;
    vec3 rgb = uYuvMat * yuv;
    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)";

const char* kHudFragmentSrc = R"(#version 300 es
precision mediump float;
in vec2 vUv;
out vec4 outColor;
uniform sampler2D hudTex;
void main() {
    outColor = texture(hudTex, vUv);
}
)";

// Column-major YUV->RGB matrices matching shaders/video_fsh.glsl / DkVideoRenderer.
const float kBt601Lim[9] = {1.1644f, 1.1644f, 1.1644f, 0.0f,    -0.3917f,
                            2.0172f, 1.5960f, -0.8129f, 0.0f};
const float kBt601Full[9] = {1.0f, 1.0f, 1.0f, 0.0f,     -0.3441f,
                             1.7720f, 1.4020f, -0.7141f, 0.0f};
const float kBt709Lim[9] = {1.1644f, 1.1644f, 1.1644f, 0.0f,    -0.2132f,
                            2.1124f, 1.7927f, -0.5329f, 0.0f};
const float kBt709Full[9] = {1.0f, 1.0f, 1.0f, 0.0f,     -0.1873f,
                             1.8556f, 1.5748f, -0.4681f, 0.0f};
const float kBt2020Lim[9] = {1.1644f, 1.1644f, 1.1644f, 0.0f,    -0.1874f,
                             2.1418f, 1.6781f, -0.6505f, 0.0f};
const float kBt2020Full[9] = {1.0f, 1.0f, 1.0f, 0.0f,     -0.1646f,
                              1.8814f, 1.4746f, -0.5714f, 0.0f};

const float* color_matrix(int space, bool full) {
    switch (space) {
        case 5:  // BT.470BG
        case 6:  // SMPTE 170M
            return full ? kBt601Full : kBt601Lim;
        case 1:  // BT.709
            return full ? kBt709Full : kBt709Lim;
        case 9:   // BT.2020 NCL
        case 10:  // BT.2020 CL
            return full ? kBt2020Full : kBt2020Lim;
        default:
            return full ? kBt709Full : kBt709Lim;
    }
}

GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    if (!s) return 0;
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint build_program(const char* vsrc, const char* fsrc) {
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vsrc);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fsrc);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

}  // namespace

GlesVideoRenderer::~GlesVideoRenderer() { shutdown(); }

void GlesVideoRenderer::logf(const char* fmt, ...) {
    if (!log_) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_(buf);
}

void GlesVideoRenderer::maybe_update_output_mode() {
    int mode = appletGetOperationMode();
    if (mode == fb_mode_) return;
    uint32_t w = (mode == AppletOperationMode_Console) ? 1920 : 1280;
    uint32_t h = (mode == AppletOperationMode_Console) ? 1080 : 720;
    fb_mode_ = mode;
    if (w == fb_width_ && h == fb_height_) return;
    fb_width_ = w;
    fb_height_ = h;
    nwindowSetDimensions(nwindowGetDefault(), fb_width_, fb_height_);
    glViewport(0, 0, static_cast<GLsizei>(fb_width_),
               static_cast<GLsizei>(fb_height_));
    logf("gles: output mode changed -> %ux%u", fb_width_, fb_height_);
}

bool GlesVideoRenderer::init(SDL_Window* window) {
    if (initialized_) return true;
    if (!window) {
        logf("gles: null SDL_Window");
        return false;
    }
    window_ = window;

    fb_mode_ = appletGetOperationMode();
    fb_width_ = (fb_mode_ == AppletOperationMode_Console) ? 1920 : 1280;
    fb_height_ = (fb_mode_ == AppletOperationMode_Console) ? 1080 : 720;
    nwindowSetDimensions(nwindowGetDefault(), fb_width_, fb_height_);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    gl_ctx_ = SDL_GL_CreateContext(window_);
    if (!gl_ctx_) {
        logf("gles: SDL_GL_CreateContext failed: %s", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, gl_ctx_);
    SDL_GL_SetSwapInterval(1);

    video_prog_ = build_program(kVertexShaderSrc, kVideoFragmentSrc);
    hud_prog_ = build_program(kVertexShaderSrc, kHudFragmentSrc);
    if (!video_prog_ || !hud_prog_) {
        logf("gles: shader compile/link failed");
        shutdown();
        return false;
    }

    glUseProgram(video_prog_);
    glUniform1i(glGetUniformLocation(video_prog_, "plane0"), 0);
    glUniform1i(glGetUniformLocation(video_prog_, "plane1"), 1);
    loc_yuvmat_ = glGetUniformLocation(video_prog_, "uYuvMat");
    loc_offset_ = glGetUniformLocation(video_prog_, "uOffset");
    loc_sharp_ = glGetUniformLocation(video_prog_, "uSharpData");

    glUseProgram(hud_prog_);
    glUniform1i(glGetUniformLocation(hud_prog_, "hudTex"), 0);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);

    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad, GL_STATIC_DRAW);

    glGenBuffers(1, &hud_vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, hud_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kHudQuad), kHudQuad, GL_STATIC_DRAW);

    glGenTextures(1, &y_tex_);
    glBindTexture(GL_TEXTURE_2D, y_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &uv_tex_);
    glBindTexture(GL_TEXTURE_2D, uv_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &hud_tex_);
    glBindTexture(GL_TEXTURE_2D, hud_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    hud_pixels_.assign(kHudTexW * kHudTexH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kHudTexW, kHudTexH, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, hud_pixels_.data());

    hud_text_cache_.clear();
    hud_dirty_ = false;
    fps_tick_ = 0;
    fps_frames_ = 0;
    fps_last_frame_ = nullptr;
    net_valid_ = false;
    logged_surface_ = false;
    tex_w_ = tex_h_ = 0;

    if (!hud_font_) {
        PlFontData fd;
        if (R_SUCCEEDED(plGetSharedFontByType(&fd, PlSharedFontType_Standard))) {
            SDL_RWops* rw = SDL_RWFromConstMem(fd.address, fd.size);
            if (rw) hud_font_ = TTF_OpenFontRW(rw, 1, 28);
        }
        if (!hud_font_) logf("gles: HUD font unavailable (panel only)");
    }

    glViewport(0, 0, static_cast<GLsizei>(fb_width_),
               static_cast<GLsizei>(fb_height_));
    initialized_ = true;
    logf("gles: initialized (%ux%u, GL %s)", fb_width_, fb_height_,
         reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    return true;
}

void GlesVideoRenderer::shutdown() {
    if (hud_font_) {
        TTF_CloseFont(hud_font_);
        hud_font_ = nullptr;
    }
    if (gl_ctx_) {
        SDL_GL_MakeCurrent(window_, gl_ctx_);
        if (y_tex_) glDeleteTextures(1, &y_tex_), y_tex_ = 0;
        if (uv_tex_) glDeleteTextures(1, &uv_tex_), uv_tex_ = 0;
        if (hud_tex_) glDeleteTextures(1, &hud_tex_), hud_tex_ = 0;
        if (vbo_) glDeleteBuffers(1, &vbo_), vbo_ = 0;
        if (hud_vbo_) glDeleteBuffers(1, &hud_vbo_), hud_vbo_ = 0;
        if (vao_) glDeleteVertexArrays(1, &vao_), vao_ = 0;
        if (video_prog_) glDeleteProgram(video_prog_), video_prog_ = 0;
        if (hud_prog_) glDeleteProgram(hud_prog_), hud_prog_ = 0;
        SDL_GL_MakeCurrent(window_, nullptr);
        SDL_GL_DeleteContext(gl_ctx_);
        gl_ctx_ = nullptr;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    window_ = nullptr;
    initialized_ = false;
    tex_w_ = tex_h_ = 0;
}

bool GlesVideoRenderer::ensure_textures(int width, int height) {
    if (width <= 0 || height <= 0) return false;
    if (width == tex_w_ && height == tex_h_) return true;
    tex_w_ = width;
    tex_h_ = height;

    glBindTexture(GL_TEXTURE_2D, y_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED,
                 GL_UNSIGNED_BYTE, nullptr);

    glBindTexture(GL_TEXTURE_2D, uv_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, (width + 1) / 2, (height + 1) / 2, 0,
                 GL_RG, GL_UNSIGNED_BYTE, nullptr);
    return true;
}

void GlesVideoRenderer::blit_text(const char* s, int x, int y) {
    if (!hud_font_ || !s || !*s) return;
    SDL_Color white{255, 255, 255, 255};
    SDL_Surface* surf = TTF_RenderUTF8_Blended(hud_font_, s, white);
    if (!surf) return;
    SDL_LockSurface(surf);
    int bpp = surf->format->BytesPerPixel;
    for (int j = 0; j < surf->h; ++j) {
        int ty = y + j;
        if (ty < 0 || ty >= static_cast<int>(kHudTexH)) continue;
        auto* rowp = static_cast<uint8_t*>(surf->pixels) + j * surf->pitch;
        for (int i = 0; i < surf->w; ++i) {
            int tx = x + i;
            if (tx < 0 || tx >= static_cast<int>(kHudTexW)) continue;
            uint32_t p = 0;
            std::memcpy(&p, rowp + i * bpp, bpp < 4 ? bpp : 4);
            uint8_t r, g, b, a;
            SDL_GetRGBA(p, surf->format, &r, &g, &b, &a);
            if (a == 0) continue;
            uint32_t& dst = hud_pixels_[ty * kHudTexW + tx];
            uint8_t dr = dst & 0xFF, dg = (dst >> 8) & 0xFF,
                    db = (dst >> 16) & 0xFF, da = (dst >> 24) & 0xFF;
            float af = a / 255.0f, ia = 1.0f - af;
            uint8_t nr = static_cast<uint8_t>(r * af + dr * ia);
            uint8_t ng = static_cast<uint8_t>(g * af + dg * ia);
            uint8_t nb = static_cast<uint8_t>(b * af + db * ia);
            uint8_t na = static_cast<uint8_t>(a + da * ia);
            dst = nr | (ng << 8) | (nb << 16) | (static_cast<uint32_t>(na) << 24);
        }
    }
    SDL_UnlockSurface(surf);
    SDL_FreeSurface(surf);
}

void GlesVideoRenderer::rasterize_hud() {
    const uint32_t bg = static_cast<uint32_t>(140) << 24;
    std::fill(hud_pixels_.begin(), hud_pixels_.end(), bg);
    if (hud_font_) {
        int y = 8;
        int skip = TTF_FontLineSkip(hud_font_);
        size_t start = 0;
        while (start <= hud_text_cache_.size()) {
            size_t nl = hud_text_cache_.find('\n', start);
            std::string line = hud_text_cache_.substr(
                start, nl == std::string::npos ? std::string::npos : nl - start);
            blit_text(line.c_str(), 12, y);
            y += skip;
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }
    hud_dirty_ = true;
}

void GlesVideoRenderer::update_hud(const Switch2Frame* frame) {
    uint64_t now = armGetSystemTick();
    uint64_t freq = armGetSystemTickFreq();
    if (fps_tick_ == 0) fps_tick_ = now;
    if (frame != fps_last_frame_) {
        ++fps_frames_;
        fps_last_frame_ = frame;
    }
    uint64_t dt = now - fps_tick_;
    if (dt >= freq / 2) {
        fps_ = static_cast<float>(fps_frames_) * static_cast<float>(freq) /
               static_cast<float>(dt);
        fps_frames_ = 0;
        fps_tick_ = now;
    }
    char buf[160];
    if (net_valid_.load(std::memory_order_relaxed)) {
        std::snprintf(buf, sizeof(buf),
                      "%dx%d\n%.0f fps  %.1f Mbps\nloss %.1f%%  buf %dms",
                      frame->width, frame->height, fps_,
                      net_mbps_.load(std::memory_order_relaxed),
                      net_loss_.load(std::memory_order_relaxed),
                      net_buffer_ms_.load(std::memory_order_relaxed));
    } else {
        std::snprintf(buf, sizeof(buf), "%dx%d\n%.0f fps", frame->width,
                      frame->height, fps_);
    }
    std::string text;
    if (notice_on_.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lock(notice_mutex_);
        text = notice_;
        if (hud_enabled_) text += std::string("\n") + buf;
    } else {
        text = buf;
    }
    if (hud_text_cache_ == text) return;
    hud_text_cache_ = text;
    rasterize_hud();
}

bool GlesVideoRenderer::render(const Switch2Frame* frame) {
    if (!initialized_ || !frame) return false;
    maybe_update_output_mode();
    if (!ensure_textures(frame->width, frame->height)) return false;

    if (!logged_surface_) {
        logged_surface_ = true;
        logf("gles: streaming NV12 %dx%d cs=%d full=%d", frame->width,
             frame->height, frame->colorspace, (int)frame->color_full);
    }

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, y_tex_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->y_pitch);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame->width, frame->height, GL_RED,
                    GL_UNSIGNED_BYTE, frame->y.data());

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, uv_tex_);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, frame->uv_pitch / 2);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (frame->width + 1) / 2,
                    (frame->height + 1) / 2, GL_RG, GL_UNSIGNED_BYTE,
                    frame->uv.data());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(video_prog_);
    glUniformMatrix3fv(loc_yuvmat_, 1, GL_FALSE,
                       color_matrix(frame->colorspace, frame->color_full));
    float y_off = frame->color_full ? 0.0f : (16.0f / 255.0f);
    glUniform3f(loc_offset_, y_off, 128.0f / 255.0f, 128.0f / 255.0f);

    static constexpr float kSharpStrength[4] = {0.0f, 0.60f, 1.20f, 2.0f};
    static constexpr float kSharpOvershoot[4] = {0.0f, 0.02f, 0.035f, 0.05f};
    glUniform4f(loc_sharp_, kSharpStrength[sharpness_],
                kSharpOvershoot[sharpness_], 1.0f / (float)frame->width,
                1.0f / (float)frame->height);

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, x)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, u)));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (hud_enabled_ || notice_on_.load(std::memory_order_relaxed)) {
        update_hud(frame);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, hud_tex_);
        if (hud_dirty_) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kHudTexW, kHudTexH, GL_RGBA,
                            GL_UNSIGNED_BYTE, hud_pixels_.data());
            hud_dirty_ = false;
        }
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                            GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(hud_prog_);
        glBindBuffer(GL_ARRAY_BUFFER, hud_vbo_);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, x)));
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<void*>(offsetof(Vertex, u)));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    SDL_GL_SwapWindow(window_);
    return true;
}

}  // namespace gnx::stream

#endif  // __SWITCH2__
