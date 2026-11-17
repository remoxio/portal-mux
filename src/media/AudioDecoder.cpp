#include "media/AudioDecoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rdc {

AudioDecoder::~AudioDecoder()
{
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    avcodec_free_context(&ctx_);
}

bool AudioDecoder::open(Codec codec, int sampleRate, int channels, std::string* error)
{
    av_packet_free(&packet_); // a second stream on the same decoder
    av_frame_free(&frame_);
    avcodec_free_context(&ctx_);
    raw_ = false;
    sampleRate_ = sampleRate;
    channels_ = channels;
    if (codec == Codec::Raw) {
        raw_ = true;
        return true;
    }
    const AVCodecID id = codec == Codec::Opus ? AV_CODEC_ID_OPUS : codec == Codec::AAC ? AV_CODEC_ID_AAC : AV_CODEC_ID_FLAC;
    const AVCodec* decoder = avcodec_find_decoder(id);
    if (!decoder) {
        *error = std::string("FFmpeg has no decoder for ") + avcodec_get_name(id);
        return false;
    }
    ctx_ = avcodec_alloc_context3(decoder);
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!ctx_ || !frame_ || !packet_) {
        *error = "Out of memory";
        return false;
    }
    ctx_->sample_rate = sampleRate;
    av_channel_layout_default(&ctx_->ch_layout, channels);
    if (codec == Codec::FLAC) ctx_->sample_fmt = AV_SAMPLE_FMT_S16; // FLAC's decoder needs to be told
    if (int rc = avcodec_open2(ctx_, decoder, nullptr); rc < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE];
        *error = std::string("Could not open the audio decoder: ") + av_make_error_string(text, sizeof text, rc);
        return false;
    }
    return true;
}

static int16_t toS16(float f) { return int16_t(std::lround(std::clamp(f, -1.0f, 1.0f) * 32767.0f)); }

void AudioDecoder::decode(const uint8_t* data, size_t size, std::vector<int16_t>& out)
{
    if (raw_) {
        const size_t n = size / 2;
        const size_t at = out.size();
        out.resize(at + n);
        std::memcpy(out.data() + at, data, n * 2);
        return;
    }
    if (!ctx_ || av_new_packet(packet_, int(size)) < 0) return;
    std::memcpy(packet_->data, data, size);
    const int sent = avcodec_send_packet(ctx_, packet_);
    av_packet_unref(packet_);
    if (sent < 0) return;

    while (avcodec_receive_frame(ctx_, frame_) == 0) {
        const int frames = frame_->nb_samples;
        const int inChannels = frame_->ch_layout.nb_channels;
        const auto fmt = AVSampleFormat(frame_->format);
        const bool planar = av_sample_fmt_is_planar(fmt);
        const size_t at = out.size();
        out.resize(at + size_t(frames) * channels_);
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < channels_; ++c) {
                const int src = std::min(c, inChannels - 1);
                const uint8_t* base = planar ? frame_->extended_data[src] : frame_->extended_data[0];
                const size_t index = planar ? size_t(i) : size_t(i) * inChannels + src;
                int16_t s = 0;
                switch (fmt) {
                case AV_SAMPLE_FMT_S16:
                case AV_SAMPLE_FMT_S16P: s = reinterpret_cast<const int16_t*>(base)[index]; break;
                case AV_SAMPLE_FMT_S32:
                case AV_SAMPLE_FMT_S32P: s = int16_t(reinterpret_cast<const int32_t*>(base)[index] >> 16); break;
                case AV_SAMPLE_FMT_FLT:
                case AV_SAMPLE_FMT_FLTP: s = toS16(reinterpret_cast<const float*>(base)[index]); break;
                case AV_SAMPLE_FMT_DBL:
                case AV_SAMPLE_FMT_DBLP: s = toS16(float(reinterpret_cast<const double*>(base)[index])); break;
                default: break;
                }
                out[at + size_t(i) * channels_ + c] = s;
            }
        }
        av_frame_unref(frame_);
    }
}

} // namespace rdc
