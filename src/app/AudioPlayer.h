#pragma once

#include <QByteArray>
#include <QObject>

class QAudioSink;
class QIODevice;

// Plays 16-bit PCM; UI thread only
// Queue capped at ~120 ms: excess is dropped, so latency can't grow
class AudioPlayer : public QObject {
public:
    explicit AudioPlayer(QObject* parent = nullptr) : QObject(parent) {}
    void play(const QByteArray& pcm, int sampleRate, int channels);

private:
    QAudioSink* sink_ = nullptr;
    QIODevice* device_ = nullptr;
    int sampleRate_ = 0, channels_ = 0;
};
