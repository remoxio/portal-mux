#include "app/AudioPlayer.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>

void AudioPlayer::play(const QByteArray& pcm, int sampleRate, int channels)
{
    if (!sink_ || sampleRate != sampleRate_ || channels != channels_) {
        delete sink_;
        QAudioFormat format;
        format.setSampleRate(sampleRate);
        format.setChannelCount(channels);
        format.setSampleFormat(QAudioFormat::Int16);
        sink_ = new QAudioSink(QMediaDevices::defaultAudioOutput(), format, this);
        sink_->setBufferSize(format.bytesForDuration(120'000));
        device_ = sink_->start();
        sampleRate_ = sampleRate;
        channels_ = channels;
    }
    if (device_) device_->write(pcm); // writes only what fits into the sink's buffer
}
