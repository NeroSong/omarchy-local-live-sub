#pragma once
#include <QByteArray>
#include <QList>

// PCM is mono, signed little endian, 16 kHz. Frames are 20 ms.
class Segmenter {
public:
    struct Update { QByteArray pcm; int utterance; bool final; };
    double thresholdDb = -42;
    double levelDb = -90;
    int silenceFrames = 25;
    int maxSpeechBytes = 192000;
    int previewBytes = 0;
    QList<Update> feedUpdates(const QByteArray &data);
    Update flushUpdate();
    QList<QByteArray> feed(const QByteArray &data);
    QByteArray flush();
    void reset();
    static QByteArray wav(const QByteArray &pcm);
private:
    QByteArray pending, preroll, speech;
    int quietFrames = 0;
    int voicedFrames = 0;
    bool continuation = false;
    int utterance = 0;
    int nextPreview = 0;
};
