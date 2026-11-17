#include "media/VideoDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <cstring>

namespace rdc {

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::close()
{
    sws_freeContext(sws_);
    sws_ = nullptr;
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    av_frame_free(&cpuFrame_);
    av_frame_free(&bgrx_);
    avcodec_free_context(&ctx_);
    av_buffer_unref(&device_);
    hardwareFormat_ = -1;
    hardwareFrames_ = 0;
    width_ = height_ = 0;
    format_ = colors_ = -1;
}

// the GPU's format when offered; FFmpeg drops it and asks again if the GPU can't take this stream
static AVPixelFormat chooseFormat(AVCodecContext* ctx, const AVPixelFormat* offered)
{
    const auto wanted = AVPixelFormat(reinterpret_cast<intptr_t>(ctx->opaque));
    for (const AVPixelFormat* f = offered; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == wanted) return *f;
    for (const AVPixelFormat* f = offered; *f != AV_PIX_FMT_NONE; ++f)
        if (!(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f;
    return offered[0];
}

bool VideoDecoder::open(Codec codec, bool hardware, std::string* error)
{
    close(); // a second stream on the same decoder
    const AVCodecID id = codec == Codec::H264 ? AV_CODEC_ID_H264 : codec == Codec::H265 ? AV_CODEC_ID_HEVC : AV_CODEC_ID_AV1;
    const AVCodec* decoder = avcodec_find_decoder(id);
    if (!decoder) {
        *error = std::string("FFmpeg has no decoder for ") + avcodec_get_name(id);
        return false;
    }
    ctx_ = avcodec_alloc_context3(decoder);
    frame_ = av_frame_alloc();
    bgrx_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!ctx_ || !frame_ || !packet_ || !bgrx_) {
        *error = "Out of memory";
        return false;
    }
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->thread_count = 0;                 // slice threads keep the latency low
    ctx_->thread_type = FF_THREAD_SLICE;
#if defined(_WIN32)
    const AVHWDeviceType type = AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__APPLE__)
    const AVHWDeviceType type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
    const AVHWDeviceType type = AV_HWDEVICE_TYPE_VAAPI;
#endif
    for (int i = 0; hardware && hardwareFormat_ < 0; ++i) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, i);
        if (!config) break;
        if (config->device_type == type && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            av_hwdevice_ctx_create(&device_, type, nullptr, nullptr, 0) == 0) {
            hardwareFormat_ = config->pix_fmt;
            ctx_->hw_device_ctx = av_buffer_ref(device_);
            ctx_->opaque = reinterpret_cast<void*>(intptr_t(hardwareFormat_));
            ctx_->get_format = chooseFormat;
            ctx_->thread_count = 1; // the GPU decodes; threads only add latency
            cpuFrame_ = av_frame_alloc();
        }
    }
    if (int rc = avcodec_open2(ctx_, decoder, nullptr); rc < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE];
        *error = std::string("Could not open the video decoder: ") + av_make_error_string(text, sizeof text, rc);
        return false;
    }
    return true;
}

int VideoDecoder::decode(const uint8_t* data, size_t size, int64_t pts, Framebuffer& fb)
{
    if (!ctx_ || av_new_packet(packet_, int(size)) < 0) return 0; // adds the padding FFmpeg needs
    std::memcpy(packet_->data, data, size);
    packet_->pts = pts;
    const int sent = avcodec_send_packet(ctx_, packet_);
    av_packet_unref(packet_);
    if (sent < 0) return 0;

    int frames = 0;
    while (avcodec_receive_frame(ctx_, frame_) == 0) {
        if (frame_->format == hardwareFormat_) { // on the GPU: copy it back (NV12, P010…)
            av_frame_unref(cpuFrame_);
            if (av_hwframe_transfer_data(cpuFrame_, frame_, 0) < 0) {
                av_frame_unref(frame_);
                continue;
            }
            av_frame_copy_props(cpuFrame_, frame_);
            av_frame_unref(frame_);
            av_frame_move_ref(frame_, cpuFrame_);
            ++hardwareFrames_;
        }
        const int w = frame_->width, h = frame_->height;
        if (w != width_ || h != height_ || frame_->format != format_) {
            width_ = w;
            height_ = h;
            format_ = frame_->format;
            fb.resize(w, h);
            av_frame_unref(bgrx_); // sws_scale_frame allocates it, padded for its SIMD writes
            sws_freeContext(sws_);
            sws_ = sws_alloc_context();
            for (auto [key, value] : {std::pair{"srcw", w}, {"srch", h}, {"src_format", format_}, {"dstw", w}, {"dsth", h},
                                      {"dst_format", int(AV_PIX_FMT_BGR0)}, {"sws_flags", SWS_BILINEAR}, {"threads", 0}}) // 0: a thread per core
                av_opt_set_int(sws_, key, value, 0);
            if (sws_init_context(sws_, nullptr, nullptr) < 0) sws_freeContext(sws_), sws_ = nullptr;
            colors_ = -1;
        }
        const int colors = frame_->colorspace * 2 + (frame_->color_range == AVCOL_RANGE_JPEG);
        if (sws_ && colors != colors_) {
            const int space = frame_->colorspace == AVCOL_SPC_UNSPECIFIED ? SWS_CS_DEFAULT : frame_->colorspace;
            sws_setColorspaceDetails(sws_, sws_getCoefficients(space), frame_->color_range == AVCOL_RANGE_JPEG,
                                     sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
            colors_ = colors;
        }
        if (sws_) {
            bgrx_->format = AV_PIX_FMT_BGR0;
            bgrx_->width = w;
            bgrx_->height = h;
            if (sws_scale_frame(sws_, bgrx_, frame_) >= 0) {
                fb.update({0, 0, w, h}, bgrx_->data[0], bgrx_->linesize[0]);
                ++frames;
            }
        }
        av_frame_unref(frame_);
    }
    return frames;
}

} // namespace rdc
