#include "backend.h"
#include "translation.h"
#include "translationstream.h"
#include "agentcontext.h"
#include <memory>
#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QTimer>

namespace {
QString localized(const QString &source, const QStringList &arguments = {}) {
    auto text = QCoreApplication::translate("Backend", source.toUtf8().constData());
    for (const auto &argument : arguments) text = text.arg(argument);
    return text;
}
}

Backend::Backend(QString directory, bool demo, QObject *parent)
    : QObject(parent), root(std::move(directory)), isDemo(demo) {
    asrBase = qEnvironmentVariable("LIVE_SUB_ASR_URL", "http://127.0.0.1:8178");
    translationBase = qEnvironmentVariable("LIVE_SUB_TRANSLATION_URL", "http://127.0.0.1:8179");
    if (asrBase.endsWith('/')) asrBase.chop(1);
    if (translationBase.endsWith('/')) translationBase.chop(1);
    network.setProxy(QNetworkProxy::NoProxy);
    segmenter.silenceFrames = 45; // Allow a short breath inside the same sentence.
    segmenter.maxSpeechBytes = 18 * 32000; // Bound very long monologues without cutting every three seconds.
    segmenter.previewBytes = 2 * 32000;
    configDir = QDir(qEnvironmentVariable("LIVE_SUB_CONFIG_DIR", root)).absolutePath();
    refreshTerms();
    auto *configTimer = new QTimer(this);
    configTimer->setInterval(1000);
    connect(configTimer, &QTimer::timeout, this, &Backend::refreshTerms);
    configTimer->start();
    connect(&agentCheck, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) setFailure("Could not launch Omarchy Agent: %1", {agentCheck.errorString()});
    });
    connect(&agentCheck, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
        const auto agent = QString::fromUtf8(agentCheck.readAllStandardOutput()).trimmed();
        if (code != 0 || agent.isEmpty() || QStandardPaths::findExecutable(agent).isEmpty()) {
            setFailure("Choose an installed default Agent in Omarchy → Setup → Defaults → Agent."); return;
        }
        if (!QProcess::startDetached(QStandardPaths::findExecutable("omarchy"),
                AgentContext::arguments(configDir, qEnvironmentVariable("LIVE_SUB_PROJECT", root), agentLanguage), configDir)) {
            setFailure("Could not open the Agent terminal."); return;
        }
        setMessage("Opening the default Agent with vocabulary guidance…"); emit changed();
    });
    connect(&capture, &QProcess::readyReadStandardOutput, this, [this] { consume(capture.readAllStandardOutput()); });
    connect(&capture, &QProcess::readyReadStandardError, this, [this] {
        captureErrors += capture.readAllStandardError(); captureErrors = captureErrors.right(2048);
    });
    connect(&capture, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { active = false; setFailure("Could not start ffmpeg: %1", {capture.errorString()}); }
    });
    connect(&capture, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int, QProcess::ExitStatus) {
        consume(capture.readAllStandardOutput());
        if (active) { active = false; setFailure("Microphone capture stopped unexpectedly: %1", {QString::fromUtf8(captureErrors)}); }
        queueAudio(segmenter.flushUpdate());
        setMessage("Recording stopped · finishing pending captions"); emit changed();
    });
    connect(&enumerate, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
        inputs.clear();
        if (code != 0) { emit devicesChanged(); setFailure("Cannot connect to audio. Run in the desktop session and check pactl."); return; }
        const auto sources = QJsonDocument::fromJson(enumerate.readAllStandardOutput()).array();
        for (const auto &value : sources) {
            auto item = value.toObject(); auto name = item.value("name").toString();
            if (name.endsWith(".monitor")) continue;
            inputs.append(QVariantMap{{"name", name}, {"label", item.value("description").toString(name)}});
        }
        if (inputs.isEmpty()) setFailure("No microphone found. Connect one and refresh.");
        emit devicesChanged();
        emit changed();
    });
    connect(&enumerate, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) setFailure("pactl is missing; cannot list microphones.");
    });
    if (isDemo) {
        setMessage("Demo · sample text · no recording or model inference");
        rows = {
            QVariantMap{{"original", "Let's talk about Omarchy and the Hyprland configuration."},
                        {"raw", "Let's talk about Omarchy and the Hyprland configuration."},
                        {"translation", "我们来聊聊 Omarchy 和 Hyprland 的配置。"}, {"time", "00:04"}, {"meta", "EN → 中文 · 示例"}},
            QVariantMap{{"original", "Rust 的所有权机制可以帮助我们避免数据竞争。"},
                        {"raw", "Rust 的所有权机制可以帮助我们避免数据竞争。"},
                        {"translation", "Rust's ownership system can help us avoid data races."}, {"time", "00:10"}, {"meta", "中文 → EN · 示例"}}
        };
    } else refreshDevices();
}
Backend::~Backend() {
    for (auto *reply : network.findChildren<QNetworkReply *>()) {
        reply->disconnect(this);
        reply->abort();
    }
    capture.disconnect(this); enumerate.disconnect(this); agentCheck.disconnect(this);
    for (auto *p : {&capture, &enumerate, &agentCheck}) {
        if (p->state() != QProcess::NotRunning) { p->terminate(); if (!p->waitForFinished(1500)) { p->kill(); p->waitForFinished(500); } }
    }
}
QNetworkRequest Backend::request(const QString &base, const QString &path) const {
    QNetworkRequest req{QUrl(base + path)};
    req.setTransferTimeout(60000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    return req;
}
QString Backend::status() const { return localized(message, messageArguments); }
QVariantMap Backend::captionAt(int index) const {
    if (index < 0 || index >= rows.size()) return {};
    auto row = rows[index].toMap();
    row["translation"] = Translation::cleanOutput(row.value("translation").toString());
    return row;
}
QString Backend::error() const { return localized(failure, failureArguments); }
void Backend::setFailure(const QString &text, const QStringList &arguments) { failure = text; failureArguments = arguments; emit changed(); }
void Backend::setMessage(const QString &text, const QStringList &arguments) { message = text; messageArguments = arguments; }
QString Backend::configStatus() const {
    auto status = terms.revision().isEmpty() ? tr("Vocabulary has not loaded") : tr("Vocabulary loaded · %1").arg(terms.revision().left(8));
    if (!asrConfigRevision.isEmpty()) status += tr(" · ASR hotwords %1").arg(asrConfigRevision.left(8));
    if (!termsWarning.isEmpty()) status += "\n" + termsWarning + (terms.revision().isEmpty() ? QString{} : tr("; keeping the previous valid vocabulary."));
    if (!asrConfigWarning.isEmpty()) status += "\n" + tr("ASR vocabulary update failed: %1; keeping the previous valid configuration.").arg(asrConfigWarning);
    return status;
}
void Backend::refreshTerms() {
    const auto previous = configStatus();
    terms.load(configDir + "/glossary.json", termsWarning);
    if (previous != configStatus()) emit changed();
}
void Backend::openVocabularyAgent(const QString &language) {
    if (agentCheck.state() != QProcess::NotRunning) return;
    const auto omarchy = QStandardPaths::findExecutable("omarchy");
    if (omarchy.isEmpty()) { setFailure("Omarchy is required to open the default Agent."); return; }
    if (!QDir(configDir).exists()) { setFailure("Configuration directory is unavailable: %1", {configDir}); return; }
    agentLanguage = language;
    agentCheck.setWorkingDirectory(configDir);
    agentCheck.start(omarchy, {"default", "agent"});
}
void Backend::refreshDevices() {
    if (active || busy() || enumerate.state() != QProcess::NotRunning || isDemo) return;
    failure.clear(); enumerate.start("pactl", {"-f", "json", "list", "sources"});
}
void Backend::toggle(const QString &source, const QString &language, double threshold) {
    if (active) { stop(); return; }
    if (busy() || capture.state() != QProcess::NotRunning || isDemo) return;
    failure.clear();
    refreshTerms();
    if (terms.revision().isEmpty()) { setFailure(termsWarning); return; }
    for (const auto &base : {asrBase, translationBase}) {
        QUrl url(base);
        if (url.scheme() != "http" || url.host() != "127.0.0.1" || !url.userInfo().isEmpty()
            || !url.query().isEmpty() || (!url.path().isEmpty() && url.path() != "/")) {
            setFailure("Model addresses must use http://127.0.0.1:port to keep audio local."); return;
        }
    }
    if (source.isEmpty()) { setFailure("Choose an available microphone first."); return; }
    if (!QStringList{"auto", "en", "zh"}.contains(language)) return;
    sourceLanguage = language; segmenter.reset(); segmenter.thresholdDb = threshold;
    starting = true; setMessage("Checking local recognition and translation services…"); emit changed();
    // Probe both models before acquiring the microphone.
    auto *asr = network.get(request(asrBase, "/"));
    connect(asr, &QNetworkReply::finished, this, [this, asr, source] {
        const bool ok = asr->error() == QNetworkReply::NoError;
        asr->deleteLater();
        if (!ok) { starting = false; setFailure("Recognition is not ready. Run bin/models-start first."); return; }
        auto *tr = network.get(request(translationBase, "/health"));
        connect(tr, &QNetworkReply::finished, this, [this, tr, source] {
            const bool ready = tr->error() == QNetworkReply::NoError;
            tr->deleteLater(); starting = false;
            if (!ready) { setFailure("Translation is not ready. Check the models-start logs."); return; }
            startCapture(source);
        });
    });
}
void Backend::startCapture(const QString &source) {
    audioBytes = 0; if (!clock.isValid()) clock.start(); captureErrors.clear(); active = true;
    capture.start("ffmpeg", {"-nostdin", "-hide_banner", "-loglevel", "error", "-f", "pulse", "-i", source,
        "-ac", "1", "-ar", "16000", "-f", "s16le", "pipe:1"});
    setMessage("Listening · original first, translation follows"); emit changed();
}
void Backend::consume(const QByteArray &data) {
    audioBytes += data.size();
    for (auto &update : segmenter.feedUpdates(data)) queueAudio(std::move(update));
    emit changed();
}
void Backend::queueAudio(Segmenter::Update update) {
    if (update.pcm.isEmpty()) return;
    // New cumulative audio supersedes queued previews, but never a completed utterance.
    for (int i = audioQueue.size() - 1; i >= 0; --i)
        if (audioQueue[i].utterance == update.utterance && !audioQueue[i].final) audioQueue.removeAt(i);
    audioQueue.enqueue({std::move(update.pcm), clock.isValid() ? clock.elapsed() : 0, update.utterance, update.final});
    // Pause intake rather than silently dropping speech when inference falls behind.
    if (backlog() >= 10 && active) {
        stop(); setFailure("Processing is falling behind. Recording stopped to preserve queued audio. Wait for completion before restarting.");
    }
    recognizeNext(); emit changed();
}
void Backend::stop() {
    if (!active) return;
    active = false; capture.terminate();
    QTimer::singleShot(1500, this, [this] { if (!active && capture.state() != QProcess::NotRunning) capture.kill(); });
    setMessage("Recording stopped · finishing pending captions"); emit changed();
}
void Backend::recognizeNext() {
    if (asrBusy || audioQueue.isEmpty()) return;
    asrBusy = true;
    refreshTerms();
    const auto jobTerms = terms;
    const auto job = audioQueue.dequeue();
    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    auto field = [multi](const QString &key, const QByteArray &body) {
        QHttpPart part; part.setHeader(QNetworkRequest::ContentDispositionHeader,
                                      "form-data; name=\"" + key + "\""); part.setBody(body); multi->append(part);
    };
    QHttpPart file; file.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"file\"; filename=\"speech.wav\"");
    file.setHeader(QNetworkRequest::ContentTypeHeader, "audio/wav"); file.setBody(Segmenter::wav(job.pcm)); multi->append(file);
    field("response_format", "json"); field("language", sourceLanguage.toUtf8());
    field("translate", "false"); field("temperature", "0.0"); field("temperature_inc", "0.0");
    field("prompt", ("Discussion about open-source software: " + jobTerms.prompt()).toUtf8());
    auto *reply = network.post(request(asrBase, "/inference"), multi); multi->setParent(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, job, jobTerms] {
        asrBusy = false;
        auto body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        const auto doc = QJsonDocument::fromJson(body);
        asrConfigWarning = doc.object().value("config_error").toString();
        asrConfigRevision = doc.object().value("config_revision").toString();
        const auto raw = doc.object().value("text").toString().trimmed();
        if (!ok || !doc.isObject() || !doc.object().value("text").isString()) {
            rows.append(QVariantMap{{"original", tr("[Recognition failed for this segment]")}, {"raw", ""},
                {"translation", tr("No translation generated")}, {"time", "—"}, {"meta", "Error"}}); dirty = true;
            stop(); setFailure("Recognition failed. Recording stopped; completed captions are preserved. Check model logs.");
        } else {
            const auto time = QString("%1:%2").arg(job.endMs / 60000, 2, 10, QChar('0')).arg(job.endMs / 1000 % 60, 2, 10, QChar('0'));
            int index = provisionalRows.value(job.utterance, -1);
            if (!raw.isEmpty()) {
                QStringList context;
                // Use only nearby raw speech, avoiding feedback from earlier guessed corrections.
                for (int i = std::max(0, int(rows.size()) - 3); i < rows.size(); ++i)
                    if (i != index) context.append(rows[i].toMap().value("raw").toString().left(1000));
                const auto corrected = jobTerms.correct(raw, context.join("\n"));
                const auto text = corrected.text;
                if (index < 0) {
                    index = int(rows.size());
                    rows.append(QVariantMap{});
                    provisionalRows.insert(job.utterance, index);
                }
                rows[index] = QVariantMap{{"original", text}, {"raw", raw}, {"translation", ""},
                    {"time", time}, {"meta", job.final ? "Recognized" : "Speaking"}, {"corrections", corrected.corrections},
                    {"asr_decoder", doc.object().value("decoder").toString()},
                    {"asr_greedy", doc.object().value("greedy_text").toString()},
                    {"asr_inference_ms", doc.object().value("inference_ms").toDouble()},
                    {"asr_decode_ms", doc.object().value("decode_ms").toDouble()},
                    {"glossary_revision", jobTerms.revision()}, {"asr_config_revision", asrConfigRevision}};
                dirty = true; emit captionUpdated(index);
            }
            if (job.final && index >= 0) {
                auto row = rows[index].toMap();
                row["translation"] = "Translating locally…";
                rows[index] = row;
                QString detected = doc.object().value("language").toString();
                detected.remove("<|"); detected.remove("|>");
                if (sourceLanguage != "auto") detected = sourceLanguage;
                if (detected != "zh" && detected != "en") detected = "auto";
                textQueue.enqueue({index, row.value("original").toString(), job.endMs, detected});
                provisionalRows.remove(job.utterance);
                emit captionUpdated(index); translateNext();
            }
        }
        recognizeNext(); emit captionsChanged(); emit changed();
    });
}
void Backend::translateNext() {
    if (translationBusy || textQueue.isEmpty()) return;
    translationBusy = true;
    const auto job = textQueue.dequeue();
    const auto language = Translation::targetLanguage(job.text, job.language);
    refreshTerms();
    auto payload = Translation::payload(job.text, language, terms.prompt(), terms.translationHints(language));
    auto row = rows[job.index].toMap();
    row["translation_glossary_revision"] = terms.revision();
    rows[job.index] = row;
    payload["stream"] = true;
    auto req = request(translationBase, "/v1/chat/completions"); req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *reply = network.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    auto stream = std::make_shared<TranslationStream>();
    auto *paint = new QTimer(reply);
    paint->setSingleShot(true); paint->setInterval(32);
    auto publish = [this, stream, job] {
        if (stream->text.isEmpty()) return;
        auto row = rows[job.index].toMap();
        row["translation"] = stream->text;
        row["meta"] = "Translating";
        rows[job.index] = row; dirty = true; emit captionUpdated(job.index); emit captionsChanged();
    };
    connect(paint, &QTimer::timeout, this, publish);
    connect(reply, &QNetworkReply::readyRead, this, [reply, stream, paint] {
        stream->feed(reply->readAll());
        if (!paint->isActive()) paint->start();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, job, language, stream, paint] {
        paint->stop(); stream->feed(reply->readAll());
        translationBusy = false;
        auto row = rows[job.index].toMap();
        if (reply->error() != QNetworkReply::NoError || !stream->complete()) {
            row["translation"] = stream->text.isEmpty() ? tr("[Translation failed; original preserved]") : stream->text + "\n" + tr("[Translation incomplete]");
            row["meta"] = "Error";
            stop(); setFailure("Translation failed or was incomplete. Recording stopped. Check the local translation service.");
        } else {
            row["translation"] = stream->text.trimmed();
            const double latency = std::max<qint64>(0, clock.elapsed() - job.endMs) / 1000.0;
            row["meta"] = (language == "zh" ? "EN → ZH" : "ZH → EN") + QString(" · %1s").arg(latency, 0, 'f', 1);
        }
        rows[job.index] = row; dirty = true; reply->deleteLater();
        translateNext(); emit captionUpdated(job.index); emit captionsChanged(); emit changed();
    });
}
void Backend::clear() {
    if (active || busy()) return;
    rows.clear(); provisionalRows.clear(); clock.invalidate(); dirty = false; failure.clear(); emit captionsChanged(); emit changed();
}
bool Backend::restoreSession(const QString &path) {
    if (active || busy() || isDemo) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024) {
        setFailure("Cannot read caption session: %1", {path}); return false;
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto object = doc.object();
    if (object.value("format_version").toInt() != 1 || !object.value("captions").isArray()) {
        setFailure("Invalid caption session format"); return false;
    }
    const auto values = object.value("captions").toArray();
    for (const auto &value : values) {
        const auto row = value.toObject();
        if (!row.value("original").isString() || !row.value("translation").isString()) {
            setFailure("The caption session is missing original text or translation"); return false;
        }
    }
    rows = values.toVariantList(); dirty = false;
    setMessage("Captions restored · ready to resume recording");
    emit captionsChanged(); emit changed(); return true;
}
void Backend::exportSession() {
    if (rows.isEmpty() || isDemo) return;
    const auto defaultDir = root == "/usr/share/live-sub"
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/sessions" : root + "/sessions";
    const auto dir = qEnvironmentVariable("LIVE_SUB_SESSIONS", defaultDir);
    if (!QDir().mkpath(dir)) { setFailure("Cannot create export directory: %1", {dir}); return; }
    const auto name = "session-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + "-" + QUuid::createUuid().toString(QUuid::Id128).left(8) + ".json";
    QSaveFile file(dir + "/" + name);
    const QJsonObject output{{"exported_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"captions", QJsonArray::fromVariantList(rows)}, {"format_version", 1}};
    const auto bytes = QJsonDocument(output).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        setFailure("Export failed: %1", {file.errorString()}); return;
    }
    dirty = false; setMessage("Exported: %1", {file.fileName()}); emit changed();
}
