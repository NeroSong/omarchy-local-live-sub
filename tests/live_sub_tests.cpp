#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QtEndian>
#include "segmenter.h"
#include "terms.h"
#include "translation.h"
#include "translationstream.h"
#include <QJsonArray>

class LiveSubTests : public QObject {
    Q_OBJECT
    QByteArray sound(int frames) {
        QByteArray data(frames * 640, '\0');
        for (int i = 0; i < data.size(); i += 2) qToLittleEndian<qint16>((i % 4) ? -5000 : 5000, data.data() + i);
        return data;
    }
private slots:
    void streamingKeepsFragmentedChineseIntact() {
        const auto wire = QString("data: {\"choices\":[{\"delta\":{\"content\":\"不要\"},\"finish_reason\":null}]}\r\n\r\n"
            "data: {\"choices\":[{\"delta\":{\"content\":\"禁用 Rust。\"},\"finish_reason\":\"stop\"}]}\n\n"
            "data: [DONE]\n\n").toUtf8();
        TranslationStream stream;
        for (const char c : wire) stream.feed(QByteArray(1, c));
        QCOMPARE(stream.text, QString("不要禁用 Rust。"));
        QVERIFY(stream.complete());
    }
    void streamingRejectsCutOffOutputAndKeepsPartialText() {
        TranslationStream stream;
        stream.feed("data: {\"choices\":[{\"delta\":{\"content\":\"Do not\"},\"finish_reason\":null}]}\n\n");
        QVERIFY(!stream.complete()); QCOMPARE(stream.text, QString("Do not"));
        stream.feed("data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}\n\ndata: [DONE]\n\n");
        QVERIFY(!stream.complete()); QVERIFY(!stream.error.isEmpty());
    }
    void translationDirectionHandlesTechnicalNames() {
        QCOMPARE(Translation::targetLanguage("Rust 的所有权机制可以避免数据竞争。", "auto"), QString("en"));
        QCOMPARE(Translation::targetLanguage("Let's configure Omarchy and Hyprland.", "auto"), QString("zh"));
        QCOMPARE(Translation::targetLanguage("Rust", "zh"), QString("en"));
        QCOMPARE(Translation::targetLanguage("中文", "en"), QString("zh"));
    }
    void translationUsesPlainTextAndRejectsTruncation() {
        const auto payload = Translation::payload("Do not enable it.", "zh", "Omarchy, Rust");
        QVERIFY(!payload.contains("response_format"));
        QCOMPARE(payload.value("messages").toArray().first().toObject().value("role").toString(), QString("user"));
        auto response = QJsonObject{{"choices", QJsonArray{QJsonObject{{"finish_reason", "stop"},
            {"message", QJsonObject{{"content", "不要启用它。"}}}}}}};
        QString error;
        QCOMPARE(Translation::result(response, error), QString("不要启用它。"));
        QVERIFY(error.isEmpty());
        response["choices"] = QJsonArray{QJsonObject{{"finish_reason", "length"}, {"message", QJsonObject{{"content", "不要"}}}}};
        QVERIFY(Translation::result(response, error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void silenceDoesNotBecomeSpeech() {
        Segmenter s;
        QVERIFY(s.feed(QByteArray(32000 * 30, '\0')).isEmpty());
        QVERIFY(s.flush().isEmpty());
    }
    void fragmentedReadsKeepSamplesAndPreroll() {
        Segmenter s;
        const auto input = QByteArray(6400, '\0') + sound(50) + QByteArray(25 * 640, '\0');
        QList<QByteArray> chunks;
        for (int i = 0; i < input.size(); i += 317) chunks += s.feed(input.mid(i, 317));
        QCOMPARE(chunks.size(), 1);
        QCOMPARE(chunks.first(), input);
    }
    void shortClickIsRejected() {
        Segmenter s;
        QVERIFY(s.feed(sound(2) + QByteArray(25 * 640, '\0')).isEmpty());
    }
    void longSpeechIsBoundedWithoutDroppingSamples() {
        Segmenter s;
        auto input = sound(700);
        auto chunks = s.feed(input);
        chunks.append(s.flush());
        QCOMPARE(chunks.size(), 3);
        QByteArray joined;
        for (const auto &chunk : chunks) { QVERIFY(chunk.size() <= 192000); joined += chunk; }
        QCOMPARE(joined, input);
    }
    void wavIsLittleEndianMono16k() {
        const auto pcm = sound(1);
        const auto wav = Segmenter::wav(pcm);
        QCOMPARE(wav.size(), 44 + pcm.size());
        QCOMPARE(wav.first(4), QByteArray("RIFF"));
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 24), 16000u);
        QCOMPARE(qFromLittleEndian<quint16>(wav.constData() + 22), 1u);
        QCOMPARE(qFromLittleEndian<quint32>(wav.constData() + 40), quint32(pcm.size()));
        QCOMPARE(wav.mid(44), pcm);
    }
    void shortTailAfterHardCutIsNotDiscarded() {
        Segmenter s;
        const auto input = sound(301);
        auto chunks = s.feed(input); chunks.append(s.flush());
        QCOMPARE(chunks.size(), 2);
        QCOMPARE(chunks[0] + chunks[1], input);
    }
    void glossaryPreservesOrdinaryWordsAndRawInput() {
        QTemporaryDir dir;
        QFile f(dir.filePath("terms.json")); QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"([{"term":"Hyprland","aliases":["hyper land"]},{"term":"Rust","aliases":[]}])"); f.close();
        Terms terms; QString error; QVERIFY(terms.load(f.fileName(), error));
        const QString raw = "hyper land, rust; rest, trust, hyper landscape.";
        QCOMPARE(terms.normalize(raw), QString("Hyprland, Rust; rest, trust, hyper landscape."));
        QCOMPARE(raw, QString("hyper land, rust; rest, trust, hyper landscape."));
    }
    void malformedGlossaryDoesNotEraseGoodEntries() {
        QTemporaryDir dir; QFile f(dir.filePath("terms.json"));
        QVERIFY(f.open(QIODevice::WriteOnly)); f.write(R"([{"term":"Omarchy","aliases":["Ohmarchy"]}])"); f.close();
        Terms terms; QString error; QVERIFY(terms.load(f.fileName(), error));
        QVERIFY(f.open(QIODevice::WriteOnly)); f.write(R"([{"term":"Rust","aliases":[""]}])"); f.close();
        QVERIFY(!terms.load(f.fileName(), error));
        QCOMPARE(terms.normalize("Ohmarchy"), QString("Omarchy"));
    }
    void glossaryRepairsReportedAliasAndUsesContextForGuesses() {
        Terms terms; QString error;
        QVERIFY(terms.load(QFINDTESTDATA("../glossary.json"), error));
        const auto corrected = terms.correct("O much 使用 Hyprlnd 作为桌面。", "Linux");
        QCOMPARE(corrected.text, QString("Omarchy 使用 Hyprland 作为桌面。"));
        QCOMPARE(corrected.corrections.size(), 2);
        QCOMPARE(corrected.corrections.first().toMap().value("from").toString(), QString("O much"));
        QCOMPARE(corrected.corrections.first().toMap().value("to").toString(), QString("Omarchy"));
        QCOMPARE(terms.correct("欧马奇的桌面使用海普兰德。").text, QString("Omarchy的桌面使用Hyprland。"));
        QCOMPARE(terms.correct("Hyprlnd 和欧马奇").text, QString("Hyprlnd 和欧马奇"));
        QCOMPARE(terms.correct("Hyprlnad", "Wayland compositor").text, QString("Hyprland"));
    }
    void glossaryPreservesCodePathsAndOrdinaryWords() {
        Terms terms; QString error;
        QVERIFY(terms.load(QFINDTESTDATA("../glossary.json"), error));
        const QString raw = "so much, too much, rest, trust, anarchy, hyper landscape; "
            "`O much` https://example.com/Hyprlnd /tmp/Hyprlnd --Hyprlnd "
            "```\nO much\n```";
        QCOMPARE(terms.correct(raw, "Linux desktop").text, raw);
        const auto corrected = terms.correct("配置Hyprland，O   much 和 Omarchyy。", "Linux");
        QCOMPARE(corrected.text, QString("配置Hyprland，Omarchy 和 Omarchy。"));
        QCOMPARE(terms.correct(corrected.text, "Linux").corrections.size(), 0);
    }
    void glossarySkipsAmbiguousSpellingCandidates() {
        QTemporaryDir dir;
        QFile file(dir.filePath("terms.json")); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(R"([{"term":"abcdefg","aliases":[],"context":["Linux"],"fuzzy":true},
                       {"term":"abcxefg","aliases":[],"context":["Linux"],"fuzzy":true}])"); file.close();
        Terms terms; QString error; QVERIFY(terms.load(file.fileName(), error));
        QCOMPARE(terms.correct("abcqefg", "Linux").text, QString("abcqefg"));
    }
};
QTEST_GUILESS_MAIN(LiveSubTests)
#include "live_sub_tests.moc"
