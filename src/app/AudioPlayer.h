#pragma once

#include <QByteArray>
#include <QObject>

class QAudioSink;
class QIODevice;

// 16-bit PCM, UI thread only; beyond ~120 ms queued is dropped
class AudioPlayer : public QObject {
public:
    explicit AudioPlayer(QObject* parent = nullptr) : QObject(parent) {}
    void play(const QByteArray& pcm, int sampleRate, int channels);

private:
    QAudioSink* sink_ = nullptr;
    QIODevice* device_ = nullptr;
    int sampleRate_ = 0, channels_ = 0;
};
