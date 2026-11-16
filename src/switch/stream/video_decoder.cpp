#include "video_decoder.hpp"

#include <cstdio>
#include <cstring>
#include <utility>

#ifdef __SWITCH2__
#include <switch/result.h>
#else
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}
#endif

namespace gnx::stream {

namespace {

#ifdef __SWITCH2__

constexpr uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};

// Unescape H.264 NAL RBSP (removes emulation prevention 0x03 after 0x00 0x00).
std::vector<uint8_t> nal_to_rbsp(const uint8_t* nal, size_t len) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(len);
    int zeros = 0;
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = nal[i];
        if (zeros >= 2 && b == 0x03) {
            zeros = 0;
            continue;
        }
        rbsp.push_back(b);
        zeros = (b == 0) ? (zeros + 1) : 0;
    }
    return rbsp;
}

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool read_bit(uint32_t& out) {
        if (byte_ >= size_) return false;
        out = (data_[byte_] >> (7 - bit_)) & 1u;
        if (++bit_ == 8) {
            bit_ = 0;
            ++byte_;
        }
        return true;
    }

    bool read_bits(int n, uint32_t& out) {
        out = 0;
        for (int i = 0; i < n; ++i) {
            uint32_t b = 0;
            if (!read_bit(b)) return false;
            out = (out << 1) | b;
        }
        return true;
    }

    bool skip_bits(int n) {
        uint32_t tmp = 0;
        for (int i = 0; i < n; ++i)
            if (!read_bit(tmp)) return false;
        return true;
    }

    bool read_ue(uint32_t& out) {
        int zeros = 0;
        uint32_t b = 0;
        while (read_bit(b) && b == 0) {
            if (++zeros > 30) return false;
        }
        if (b == 0) return false;
        uint32_t suffix = 0;
        if (zeros > 0 && !read_bits(zeros, suffix)) return false;
        out = ((1u << zeros) - 1u) + suffix;
        return true;
    }

    bool read_se() {
        uint32_t dummy = 0;
        return read_ue(dummy);
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t byte_ = 0;
    int bit_ = 0;
};

void skip_scaling_list(BitReader& br, int size) {
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0) {
            uint32_t v = 0;
            if (!br.read_ue(v)) return;
            int32_t delta = (v & 1u) ? static_cast<int32_t>((v + 1u) >> 1)
                                     : -static_cast<int32_t>(v >> 1);
            next = (last + delta + 256) % 256;
        }
        last = (next == 0) ? last : next;
    }
}

// Parse matrix_coefficients and video_full_range_flag from an H.264 SPS NAL.
void parse_sps_color(const uint8_t* nal, size_t len, int& colorspace,
                     bool& full_range) {
    if (len < 4) return;
    std::vector<uint8_t> rbsp = nal_to_rbsp(nal + 1, len - 1);
    BitReader br(rbsp.data(), rbsp.size());

    uint32_t profile_idc = 0, dummy = 0;
    if (!br.read_bits(8, profile_idc)) return;
    if (!br.skip_bits(16)) return;  // constraints + level_idc
    if (!br.read_ue(dummy)) return;  // seq_parameter_set_id

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134) {
        uint32_t chroma_format_idc = 0;
        if (!br.read_ue(chroma_format_idc)) return;
        if (chroma_format_idc == 3 && !br.skip_bits(1)) return;
        if (!br.read_ue(dummy) || !br.read_ue(dummy)) return;
        if (!br.skip_bits(1)) return;
        uint32_t seq_scaling_matrix_present = 0;
        if (!br.read_bit(seq_scaling_matrix_present)) return;
        if (seq_scaling_matrix_present) {
            int count = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < count; ++i) {
                uint32_t present = 0;
                if (!br.read_bit(present)) return;
                if (present) skip_scaling_list(br, i < 6 ? 16 : 64);
            }
        }
    }

    if (!br.read_ue(dummy)) return;  // log2_max_frame_num_minus4
    uint32_t pic_order_cnt_type = 0;
    if (!br.read_ue(pic_order_cnt_type)) return;
    if (pic_order_cnt_type == 0) {
        if (!br.read_ue(dummy)) return;
    } else if (pic_order_cnt_type == 1) {
        if (!br.skip_bits(1) || !br.read_se() || !br.read_se()) return;
        uint32_t n = 0;
        if (!br.read_ue(n)) return;
        for (uint32_t i = 0; i < n; ++i)
            if (!br.read_ue(dummy)) return;
    }

    if (!br.read_ue(dummy) || !br.skip_bits(1)) return;  // max_num_ref_frames, gaps
    if (!br.read_ue(dummy) || !br.read_ue(dummy)) return;  // width/height in mbs
    uint32_t frame_mbs_only = 0;
    if (!br.read_bit(frame_mbs_only)) return;
    if (!frame_mbs_only && !br.skip_bits(1)) return;
    if (!br.skip_bits(1)) return;  // direct_8x8_inference_flag

    uint32_t frame_cropping = 0;
    if (!br.read_bit(frame_cropping)) return;
    if (frame_cropping) {
        for (int i = 0; i < 4; ++i)
            if (!br.read_ue(dummy)) return;
    }

    uint32_t vui_present = 0;
    if (!br.read_bit(vui_present) || !vui_present) return;

    uint32_t aspect_ratio_info = 0;
    if (!br.read_bit(aspect_ratio_info)) return;
    if (aspect_ratio_info) {
        uint32_t idc = 0;
        if (!br.read_bits(8, idc)) return;
        if (idc == 255 && !br.skip_bits(32)) return;
    }
    uint32_t overscan_info = 0;
    if (!br.read_bit(overscan_info)) return;
    if (overscan_info && !br.skip_bits(1)) return;

    uint32_t video_signal_type = 0;
    if (!br.read_bit(video_signal_type) || !video_signal_type) return;
    uint32_t full = 0, colour_desc = 0;
    if (!br.skip_bits(3)) return;  // video_format
    if (!br.read_bit(full)) return;
    full_range = (full != 0);
    if (!br.read_bit(colour_desc) || !colour_desc) return;
    uint32_t matrix = 0;
    if (!br.skip_bits(16)) return;  // colour_primaries + transfer_characteristics
    if (!br.read_bits(8, matrix)) return;
    colorspace = static_cast<int>(matrix);
}

// Extract all SPS (type 7) and PPS (type 8) NAL units in an Annex-B buffer,
// emitting them with normalized 4-byte start codes (SPS before PPS).
bool extract_sps_pps(const uint8_t* data, size_t size,
                     std::vector<uint8_t>& out_config, int& colorspace,
                     bool& full_range) {
    std::vector<std::pair<const uint8_t*, size_t>> sps;
    std::vector<std::pair<const uint8_t*, size_t>> pps;

    size_t i = 0;
    auto find_start_code = [&](size_t pos, size_t& sc_pos, size_t& sc_len) {
        for (size_t p = pos; p + 2 < size; ++p) {
            if (data[p] == 0 && data[p + 1] == 0) {
                if (data[p + 2] == 1) {
                    sc_pos = p;
                    sc_len = 3;
                    return true;
                }
                if (p + 3 < size && data[p + 2] == 0 && data[p + 3] == 1) {
                    sc_pos = p;
                    sc_len = 4;
                    return true;
                }
            }
        }
        return false;
    };

    size_t sc_pos = 0, sc_len = 0;
    if (!find_start_code(0, sc_pos, sc_len)) return false;
    i = sc_pos + sc_len;
    while (i < size) {
        size_t next_pos = size, next_len = 0;
        bool has_next = find_start_code(i, next_pos, next_len);
        size_t nal_end = has_next ? next_pos : size;
        // Strip trailing zero bytes before the next 4-byte start code.
        while (nal_end > i && data[nal_end - 1] == 0 && has_next) --nal_end;
        if (nal_end > i) {
            uint8_t nal_type = data[i] & 0x1fu;
            if (nal_type == 7) {
                sps.emplace_back(data + i, nal_end - i);
                parse_sps_color(data + i, nal_end - i, colorspace, full_range);
            } else if (nal_type == 8) {
                pps.emplace_back(data + i, nal_end - i);
            }
        }
        if (!has_next) break;
        i = next_pos + next_len;
    }

    if (sps.empty() || pps.empty()) return false;
    out_config.clear();
    for (const auto& n : sps) {
        out_config.insert(out_config.end(), kStartCode, kStartCode + 4);
        out_config.insert(out_config.end(), n.first, n.first + n.second);
    }
    for (const auto& n : pps) {
        out_config.insert(out_config.end(), kStartCode, kStartCode + 4);
        out_config.insert(out_config.end(), n.first, n.first + n.second);
    }
    return true;
}

#elif defined(__SWITCH__)
enum AVPixelFormat pick_hw_format(AVCodecContext* context,
                                  const enum AVPixelFormat* formats) {
    for (const enum AVPixelFormat* p = formats; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_NVTEGRA) return *p;
    }
    (void)context;
    return formats[0];  // software fallback
}
#endif

}  // namespace

#ifdef __SWITCH2__

bool VideoDecoder::init(SDL_Renderer* renderer) {
    renderer_ = renderer;
    width_ = 0;
    height_ = 0;
    frame_index_ = 0;
    colorspace_ = 1;  // BT.709
    color_full_ = false;
    codec_config_.clear();
    held_frame_.reset();
    error_ = false;
    last_pixfmt_ = "?";
    return true;
}

void VideoDecoder::close_decoder() {
    if (hw_ready_) {
        videoDecoderClose(&decoder_);
        hw_ready_ = false;
    }
    codec_config_.clear();
}

void VideoDecoder::shutdown() {
    close_decoder();
    held_frame_.reset();
    if (texture_) SDL_DestroyTexture(texture_), texture_ = nullptr;
}

bool VideoDecoder::ensure_decoder(const uint8_t* data, size_t size) {
    std::vector<uint8_t> new_config;
    int cs = colorspace_;
    bool full = color_full_;
    if (extract_sps_pps(data, size, new_config, cs, full)) {
        colorspace_ = cs;
        color_full_ = full;
        if (!hw_ready_ || new_config != codec_config_) {
            close_decoder();
            VideoDecoderConfig cfg{};
            cfg.codec = ViddecCodec_H264;
            cfg.width = 1920;
            cfg.height = 1080;
            cfg.image_count = VIDEO_DECODER_DEFAULT_IMAGES;
            cfg.frame_rate = 60;
            cfg.tiling = ViddecTilingMode_PitchLinear;
            cfg.codec_config = new_config.data();
            cfg.codec_config_size = new_config.size();
            Result rc = videoDecoderCreate(&decoder_, &cfg);
            if (R_FAILED(rc)) {
                std::fprintf(stderr, "viddec: videoDecoderCreate failed 0x%x\n",
                             rc);
                error_ = true;
                return false;
            }
            hw_ready_ = true;
            codec_config_ = std::move(new_config);
        }
    }
    return hw_ready_;
}

bool VideoDecoder::decode(const uint8_t* data, size_t size) {
    if (!data || size == 0) return false;
    if (!ensure_decoder(data, size)) return false;

    Result rc = videoDecoderSubmit(&decoder_, data, size,
                                   frame_index_++ * 16666667LL, false,
                                   1000000000ULL);
    if (R_FAILED(rc)) {
        std::fprintf(stderr, "viddec: submit failed 0x%x\n", rc);
        error_ = true;
        return false;
    }

    bool rendered = false;
    VideoFrame vf{};
    rc = videoDecoderGetFrame(&decoder_, &vf, 0);
    if (rc == KERNELRESULT(TimedOut)) {
        // Low-latency H.264: wait briefly so a just-submitted frame renders in
        // this cycle instead of sitting until the next network access unit.
        rc = videoDecoderGetFrame(&decoder_, &vf, 5000000ULL);
    }

    while (R_SUCCEEDED(rc)) {
        if (vf.width > 0 && vf.height > 0 && vf.planes[0] && vf.planes[1]) {
            auto out = std::make_shared<Switch2Frame>();
            const int w = static_cast<int>(vf.width);
            const int h = static_cast<int>(vf.height);
            const int ch = (h + 1) / 2;
            out->width = w;
            out->height = h;
            out->y_pitch = w;
            out->uv_pitch = w;
            out->colorspace = colorspace_;
            out->color_full = color_full_;
            out->y.resize(static_cast<size_t>(w) * h);
            out->uv.resize(static_cast<size_t>(w) * ch);

            for (int r = 0; r < h; ++r) {
                std::memcpy(out->y.data() + static_cast<size_t>(r) * w,
                            vf.planes[0] + static_cast<size_t>(r) * vf.pitches[0],
                            w);
            }
            for (int r = 0; r < ch; ++r) {
                std::memcpy(out->uv.data() + static_cast<size_t>(r) * w,
                            vf.planes[1] + static_cast<size_t>(r) * vf.pitches[1],
                            w);
            }

            width_ = w;
            height_ = h;
            last_pixfmt_ = "nv12-viddec";
            held_frame_ = std::move(out);
            rendered = true;
        }
        videoDecoderReleaseFrame(&decoder_, &vf);
        rc = videoDecoderGetFrame(&decoder_, &vf, 0);
    }

    if (R_FAILED(rc) && rc != KERNELRESULT(TimedOut)) {
        std::fprintf(stderr, "viddec: getFrame failed 0x%x\n", rc);
        error_ = true;
    }
    return rendered;
}

#else

bool VideoDecoder::init(SDL_Renderer* renderer) {
    renderer_ = renderer;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return false;
    context_ = avcodec_alloc_context3(codec);
    if (!context_) return false;

    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context_->flags2 |= AV_CODEC_FLAG2_FAST;

#ifdef __SWITCH__
    if (av_hwdevice_ctx_create(&hw_device_, AV_HWDEVICE_TYPE_NVTEGRA, nullptr,
                               nullptr, 0) == 0) {
        context_->hw_device_ctx = av_buffer_ref(hw_device_);
        context_->get_format = pick_hw_format;
        // Headroom so the decoder's surface pool isn't starved by the frames we
        // hold outside it for zero-copy: the decode thread's held ref + the
        // shared hand-off frame + the render thread's in-flight present frame.
        context_->extra_hw_frames = 6;
    } else {
        std::fprintf(stderr, "nvtegra unavailable, software decode\n");
        context_->thread_type = FF_THREAD_SLICE;
        context_->thread_count = 3;
    }
#else
    context_->thread_count = 4;
#endif

    if (avcodec_open2(context_, codec, nullptr) != 0) return false;
    frame_ = av_frame_alloc();
    held_frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    return frame_ && held_frame_ && packet_;
}

void VideoDecoder::shutdown() {
    if (packet_) av_packet_free(&packet_);
    if (frame_) av_frame_free(&frame_);
    if (held_frame_) av_frame_free(&held_frame_);
    if (context_) avcodec_free_context(&context_);
    if (hw_device_) av_buffer_unref(&hw_device_);
    if (texture_) SDL_DestroyTexture(texture_), texture_ = nullptr;
}

bool VideoDecoder::ensure_texture(SDL_Renderer* renderer, int width,
                                  int height) {
    if (texture_ && width == width_ && height == height_) return true;
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV,
                                 SDL_TEXTUREACCESS_STREAMING, width, height);
    width_ = width;
    height_ = height;
    return texture_ != nullptr;
}

bool VideoDecoder::decode(const uint8_t* data, size_t size) {
    av_packet_unref(packet_);
    if (av_new_packet(packet_, static_cast<int>(size)) != 0) return false;
    std::memcpy(packet_->data, data, size);

    if (avcodec_send_packet(context_, packet_) != 0) {
        error_ = true;
        return false;
    }

    bool rendered = false;
    while (avcodec_receive_frame(context_, frame_) == 0) {
        if (frame_->decode_error_flags != 0 ||
            (frame_->flags & AV_FRAME_FLAG_CORRUPT))
            error_ = true;
#ifdef __SWITCH__
        // Zero-copy: keep the raw NVTEGRA surface for the deko3d renderer -- no
        // GPU->CPU transfer, no software colour conversion (both of which
        // corrupt on the Switch SDL path). We must ref it into held_frame_:
        // the next avcodec_receive_frame() unrefs frame_ (leaving fmt=-1), so
        // rendering frame_ after decode() returns would see an empty frame.
        const char* name = av_get_pix_fmt_name(
            static_cast<AVPixelFormat>(frame_->format));
        last_pixfmt_ = name ? name : "?";
        width_ = frame_->width;
        height_ = frame_->height;
        av_frame_unref(held_frame_);
        if (av_frame_ref(held_frame_, frame_) == 0) rendered = true;
#else
        if (upload(frame_)) rendered = true;
#endif
    }
    return rendered;
}

bool VideoDecoder::upload(AVFrame* frame) {
    const int w = frame->width, h = frame->height;
    if (w <= 0 || h <= 0) return false;
    if (!ensure_texture(renderer_, w, h)) return false;

    const char* name = av_get_pix_fmt_name(
        static_cast<AVPixelFormat>(frame->format));
    last_pixfmt_ = name ? name : "?";

    // Build a tightly-packed I420 image (Y plane, then U, then V), copying row
    // by row so the source stride (which is often padded/aligned) is handled
    // explicitly. Then hand SDL exact, tightly-packed pitches -- this avoids
    // both SDL's fragile NV12 path and any stride mismatch.
    const int cw = w / 2, ch = h / 2;
    i420_.resize(static_cast<size_t>(w) * h + 2 * static_cast<size_t>(cw) * ch);
    uint8_t* y = i420_.data();
    uint8_t* u = y + static_cast<size_t>(w) * h;
    uint8_t* v = u + static_cast<size_t>(cw) * ch;

    for (int r = 0; r < h; ++r)
        std::memcpy(y + static_cast<size_t>(r) * w,
                    frame->data[0] + static_cast<size_t>(r) * frame->linesize[0],
                    w);

    if (frame->format == AV_PIX_FMT_NV12) {
        // Interleaved UV -> separate U and V planes.
        for (int r = 0; r < ch; ++r) {
            const uint8_t* uv =
                frame->data[1] + static_cast<size_t>(r) * frame->linesize[1];
            uint8_t* ud = u + static_cast<size_t>(r) * cw;
            uint8_t* vd = v + static_cast<size_t>(r) * cw;
            for (int c = 0; c < cw; ++c) {
                ud[c] = uv[2 * c];
                vd[c] = uv[2 * c + 1];
            }
        }
    } else if (frame->format == AV_PIX_FMT_YUV420P ||
               frame->format == AV_PIX_FMT_YUVJ420P) {
        for (int r = 0; r < ch; ++r) {
            std::memcpy(u + static_cast<size_t>(r) * cw,
                        frame->data[1] + static_cast<size_t>(r) * frame->linesize[1], cw);
            std::memcpy(v + static_cast<size_t>(r) * cw,
                        frame->data[2] + static_cast<size_t>(r) * frame->linesize[2], cw);
        }
    } else {
        return false;
    }

    return SDL_UpdateYUVTexture(texture_, nullptr, y, w, u, cw, v, cw) == 0;
}

#endif

}  // namespace gnx::stream
