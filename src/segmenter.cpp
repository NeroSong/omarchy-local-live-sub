#include "segmenter.h"
#include <QtEndian>
#include <cmath>

QList<QByteArray> Segmenter::feed(const QByteArray &data) {
    QList<QByteArray> result;
    for (auto &update : feedUpdates(data))
        if (update.final) result.append(std::move(update.pcm));
    return result;
}

QList<Segmenter::Update> Segmenter::feedUpdates(const QByteArray &data) {
    pending += data;
    QList<Update> result;
    while (pending.size() >= 640) {
        const auto frame = pending.first(640);
        pending.remove(0, 640);
        double square = 0;
        for (int i = 0; i < 640; i += 2) {
            const double value = qFromLittleEndian<qint16>(frame.constData() + i) / 32768.0;
            square += value * value;
        }
        levelDb = 20 * std::log10(std::max(1e-5, std::sqrt(square / 320)));
        const bool voice = levelDb >= thresholdDb;
        if (speech.isEmpty()) {
            if (!voice) {
                preroll += frame;
                if (preroll.size() > 6400) preroll.remove(0, preroll.size() - 6400);
                if (preroll.size() >= 6400) continuation = false;
                continue;
            }
            speech = preroll;
            preroll.clear();
            ++utterance;
            nextPreview = previewBytes;
        }
        speech += frame;
        if (voice) { quietFrames = 0; ++voicedFrames; }
        else ++quietFrames;
        // Preview the cumulative audio without cutting it; translate only the final update.
        if (quietFrames >= silenceFrames || speech.size() >= maxSpeechBytes) {
            const bool hardLimit = speech.size() >= maxSpeechBytes;
            auto completed = flushUpdate();
            continuation = hardLimit;
            if (!completed.pcm.isEmpty()) result.append(std::move(completed));
        } else if (previewBytes > 0 && speech.size() >= nextPreview && voicedFrames >= 8) {
            result.append({speech, utterance, false});
            nextPreview = int(speech.size()) + previewBytes;
        }
    }
    return result;
}

QByteArray Segmenter::flush() {
    return flushUpdate().pcm;
}
Segmenter::Update Segmenter::flushUpdate() {
    Update result{voicedFrames >= 8 || (continuation && voicedFrames > 0) ? speech : QByteArray{}, utterance, true};
    speech.clear();
    voicedFrames = quietFrames = 0;
    continuation = false;
    return result;
}
void Segmenter::reset() { pending.clear(); preroll.clear(); flush(); levelDb = -90; }

QByteArray Segmenter::wav(const QByteArray &pcm) {
    QByteArray result;
    auto u32 = [&result](quint32 n) { char b[4]; qToLittleEndian(n, b); result.append(b, 4); };
    auto u16 = [&result](quint16 n) { char b[2]; qToLittleEndian(n, b); result.append(b, 2); };
    result += "RIFF"; u32(36 + pcm.size()); result += "WAVEfmt "; u32(16);
    u16(1); u16(1); u32(16000); u32(32000); u16(2); u16(16);
    result += "data"; u32(pcm.size()); result += pcm;
    return result;
}
