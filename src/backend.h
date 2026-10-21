#pragma once
#include <QObject>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QQueue>
#include <QVariantList>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include "segmenter.h"
#include "terms.h"

class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool recording READ recording NOTIFY changed)
    Q_PROPERTY(bool starting READ startingCapture NOTIFY changed)
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
    Q_PROPERTY(QString configDirectory READ configDirectory CONSTANT)
    Q_PROPERTY(QString configStatus READ configStatus NOTIFY changed)
public:
    explicit Backend(QString root, bool demo = false, QObject *parent = nullptr);
    ~Backend();
    bool recording() const { return active; }
    bool startingCapture() const { return starting; }
    bool busy() const { return starting || capture.state() != QProcess::NotRunning || asrBusy || translationBusy || !audioQueue.isEmpty() || !textQueue.isEmpty(); }
    bool demo() const { return isDemo; }
    bool unsaved() const { return dirty; }
    QString status() const;
    QString error() const;
    double level() const { return segmenter.levelDb; }
    int backlog() const { return audioQueue.size() + textQueue.size() + int(asrBusy) + int(translationBusy); }
    QVariantList devices() const { return inputs; }
    QVariantList captions() const { return rows; }
    int captionCount() const { return int(rows.size()); }
    QString configDirectory() const { return configDir; }
    QString configStatus() const;
    Q_INVOKABLE QVariantMap captionAt(int index) const;
    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void toggle(const QString &source, const QString &language, double threshold);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void clear();
    Q_INVOKABLE void exportSession();
    Q_INVOKABLE void openVocabularyAgent(const QString &language);
    bool restoreSession(const QString &path);
private:
    struct AudioJob { QByteArray pcm; qint64 endMs; int utterance; bool final; };
    struct TextJob { int index; QString text; qint64 endMs; QString language; };
    void startCapture(const QString &source);
    void consume(const QByteArray &data);
    void queueAudio(Segmenter::Update update);
    void recognizeNext();
    void translateNext();
    void setFailure(const QString &text, const QStringList &arguments = {});
    void setMessage(const QString &text, const QStringList &arguments = {});
    void refreshTerms();
    QNetworkRequest request(const QString &base, const QString &path) const;
    QString root, sourceLanguage = "auto", asrBase, translationBase;
    QString configDir, termsWarning, asrConfigWarning, asrConfigRevision;
    QString agentLanguage;
    QString message = "Choose a microphone to start local bilingual captions", failure;
    QStringList messageArguments, failureArguments;
    bool active = false, starting = false, asrBusy = false, translationBusy = false, isDemo = false, dirty = false;
    QProcess capture, enumerate, agentCheck;
    QNetworkAccessManager network;
    QPointer<QNetworkReply> startupReply;
    quint64 startupGeneration = 0;
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
