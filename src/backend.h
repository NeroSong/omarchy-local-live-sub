#pragma once
#include <QObject>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QVariantList>
#include <QElapsedTimer>
#include <QHash>
#include "segmenter.h"
#include "terms.h"

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool recording READ recording NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(bool demo READ demo CONSTANT)
    Q_PROPERTY(bool unsaved READ unsaved NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
    Q_PROPERTY(double level READ level NOTIFY changed)
    Q_PROPERTY(int backlog READ backlog NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(QVariantList captions READ captions NOTIFY captionsChanged)
    Q_PROPERTY(int captionCount READ captionCount NOTIFY captionsChanged)
public:
    explicit Backend(QString root, bool demo = false, QObject *parent = nullptr);
    ~Backend();
    bool recording() const { return active; }
    bool busy() const { return starting || capture.state() != QProcess::NotRunning || asrBusy || translationBusy || !audioQueue.isEmpty() || !textQueue.isEmpty(); }
    bool demo() const { return isDemo; }
    bool unsaved() const { return dirty; }
    QString status() const { return message; }
    QString error() const { return failure; }
    double level() const { return segmenter.levelDb; }
    int backlog() const { return audioQueue.size() + textQueue.size() + int(asrBusy) + int(translationBusy); }
    QVariantList devices() const { return inputs; }
    QVariantList captions() const { return rows; }
    int captionCount() const { return int(rows.size()); }
    Q_INVOKABLE QVariantMap captionAt(int index) const { return index >= 0 && index < rows.size() ? rows[index].toMap() : QVariantMap{}; }
    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void toggle(const QString &source, const QString &language, double threshold);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void exportSession();
    bool restoreSession(const QString &path);
private:
    struct AudioJob { QByteArray pcm; qint64 endMs; int utterance; bool final; };
    struct TextJob { int index; QString text; qint64 endMs; QString language; };
    void startCapture(const QString &source);
    void consume(const QByteArray &data);
    void queueAudio(Segmenter::Update update);
    void recognizeNext();
    void translateNext();
    void setFailure(const QString &text);
    QNetworkRequest request(const QString &base, const QString &path) const;
    QString root, sourceLanguage = "auto", asrBase, translationBase;
    QString message = "选择麦克风，开始本地双语字幕", failure;
    bool active = false, starting = false, asrBusy = false, translationBusy = false, isDemo = false, dirty = false;
    QProcess capture, enumerate;
    QNetworkAccessManager network;
    Segmenter segmenter;
    Terms terms;
    QVariantList inputs, rows;
    QQueue<AudioJob> audioQueue;
    QQueue<TextJob> textQueue;
    QHash<int, int> provisionalRows;
    QElapsedTimer clock;
    qint64 audioBytes = 0;
    QByteArray captureErrors;
signals:
    void changed();
    void devicesChanged();
    void captionsChanged();
    void captionUpdated(int index);
};
