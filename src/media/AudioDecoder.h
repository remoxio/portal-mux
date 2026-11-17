#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace rdc {

class AudioDecoder {
public:
    enum class Codec { Opus, AAC, FLAC, Raw };

    AudioDecoder() = default;
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    ~AudioDecoder();

    bool open(Codec codec, int sampleRate, int channels, std::string* error);

    // corrupt packets are skipped
    void decode(const uint8_t* data, size_t size, std::vector<int16_t>& out);

    int sampleRate() const { return sampleRate_; }
    int channels() const { return channels_; }

private:
    bool raw_ = false;
    int sampleRate_ = 0, channels_ = 0;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
};

} // namespace rdc
