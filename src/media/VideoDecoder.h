#pragma once

#include "core/Framebuffer.h"

#include <cstdint>
#include <string>
#include <vector>

struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace rdc {

// H.264/H.265/AV1 to BGRX; on the GPU (VA-API, D3D11VA, VideoToolbox) when asked and it can, else in software.
// shortcut: GPU frames are copied back and converted on the CPU; zero-copy needs GL/Vulkan rendering
class VideoDecoder {
public:
    enum class Codec { H264, H265, AV1 };

    VideoDecoder() = default;
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    ~VideoDecoder();

    bool open(Codec codec, bool hardware, std::string* error);
    bool hardwareFrames() const { return hardwareFrames_ > 0; } // the GPU decoded some

    // corrupt packets give 0 frames, not an error
    int decode(const uint8_t* data, size_t size, int64_t pts, Framebuffer& fb);

    int width() const { return width_; }
    int height() const { return height_; }

private:
    void close();
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* cpuFrame_ = nullptr; // a GPU frame copied back
    AVBufferRef* device_ = nullptr;
    int hardwareFormat_ = -1;
    long hardwareFrames_ = 0;
    AVPacket* packet_ = nullptr;
    SwsContext* sws_ = nullptr;
    AVFrame* bgrx_ = nullptr; // the converted picture
    int width_ = 0, height_ = 0, format_ = -1, colors_ = -1;
};

} // namespace rdc
