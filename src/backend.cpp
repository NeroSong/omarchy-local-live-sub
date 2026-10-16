#include "backend.h"
#include "translation.h"
#include "translationstream.h"
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
    QString err;
    if (!terms.load(root + "/glossary.json", err)) failure = err;
    connect(&capture, &QProcess::readyReadStandardOutput, this, [this] { consume(capture.readAllStandardOutput()); });
    connect(&capture, &QProcess::readyReadStandardError, this, [this] {
        captureErrors += capture.readAllStandardError(); captureErrors = captureErrors.right(2048);
    });
    connect(&capture, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) { active = false; setFailure("无法启动 ffmpeg：" + capture.errorString()); }
    });
    connect(&capture, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int, QProcess::ExitStatus) {
        consume(capture.readAllStandardOutput());
        if (active) { active = false; setFailure("麦克风采集意外停止：" + QString::fromUtf8(captureErrors)); }
        queueAudio(segmenter.flushUpdate());
        message = "采集已停止，正在完成剩余字幕"; emit changed();
    });
    connect(&enumerate, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus) {
        inputs.clear();
        if (code != 0) { emit devicesChanged(); setFailure("无法连接音频服务。请在桌面会话中运行，并检查 pactl。"); return; }
        const auto sources = QJsonDocument::fromJson(enumerate.readAllStandardOutput()).array();
        for (const auto &value : sources) {
            auto item = value.toObject(); auto name = item.value("name").toString();
            if (name.endsWith(".monitor")) continue;
            inputs.append(QVariantMap{{"name", name}, {"label", item.value("description").toString(name)}});
        }
        if (inputs.isEmpty()) setFailure("未找到麦克风输入。请连接麦克风后刷新。");
        emit devicesChanged();
        emit changed();
    });
    connect(&enumerate, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart) setFailure("缺少 pactl，无法列出麦克风。");
    });
    if (isDemo) {
        message = "演示模式 · 示例文字 · 未采集音频，未运行模型";
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
    capture.disconnect(this); enumerate.disconnect(this);
    for (auto *p : {&capture, &enumerate}) {
        if (p->state() != QProcess::NotRunning) { p->terminate(); if (!p->waitForFinished(1500)) { p->kill(); p->waitForFinished(500); } }
    }
}
QNetworkRequest Backend::request(const QString &base, const QString &path) const {
    QNetworkRequest req{QUrl(base + path)};
    req.setTransferTimeout(60000);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    return req;
}
void Backend::setFailure(const QString &text) { failure = text; emit changed(); }
void Backend::refreshDevices() {
    if (active || busy() || enumerate.state() != QProcess::NotRunning || isDemo) return;
    failure.clear(); enumerate.start("pactl", {"-f", "json", "list", "sources"});
}
void Backend::toggle(const QString &source, const QString &language, double threshold) {
    if (active) { stop(); return; }
    if (busy() || capture.state() != QProcess::NotRunning || isDemo) return;
    failure.clear();
    QString err;
    if (!terms.load(root + "/glossary.json", err)) { setFailure(err); return; }
    for (const auto &base : {asrBase, translationBase}) {
        QUrl url(base);
        if (url.scheme() != "http" || url.host() != "127.0.0.1" || !url.userInfo().isEmpty()
            || !url.query().isEmpty() || (!url.path().isEmpty() && url.path() != "/")) {
            setFailure("模型地址只允许 http://127.0.0.1:端口，确保音频留在本机。"); return;
        }
    }
    if (source.isEmpty()) { setFailure("请先选择可用麦克风。"); return; }
    if (!QStringList{"auto", "en", "zh"}.contains(language)) return;
    sourceLanguage = language; segmenter.reset(); segmenter.thresholdDb = threshold;
    starting = true; message = "检查本地识别和翻译服务…"; emit changed();
    // Probe both models before acquiring the microphone.
    auto *asr = network.get(request(asrBase, "/"));
    connect(asr, &QNetworkReply::finished, this, [this, asr, source] {
        const bool ok = asr->error() == QNetworkReply::NoError;
        asr->deleteLater();
        if (!ok) { starting = false; setFailure("识别服务尚未就绪。请先运行 bin/models-start。"); return; }
        auto *tr = network.get(request(translationBase, "/health"));
        connect(tr, &QNetworkReply::finished, this, [this, tr, source] {
            const bool ready = tr->error() == QNetworkReply::NoError;
            tr->deleteLater(); starting = false;
            if (!ready) { setFailure("翻译服务尚未就绪。请查看 models-start 日志。"); return; }
            startCapture(source);
        });
    });
}
void Backend::startCapture(const QString &source) {
    audioBytes = 0; if (!clock.isValid()) clock.start(); captureErrors.clear(); active = true;
    capture.start("ffmpeg", {"-nostdin", "-hide_banner", "-loglevel", "error", "-f", "pulse", "-i", source,
        "-ac", "1", "-ar", "16000", "-f", "s16le", "pipe:1"});
    message = "正在聆听 · 原文先显示，译文随后补齐"; emit changed();
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
        stop(); setFailure("模型处理积压，已停止收音以保留现有片段。完成后可重新开始；请换用更快模型。");
    }
    recognizeNext(); emit changed();
}
void Backend::stop() {
    if (!active) return;
    active = false; capture.terminate();
    QTimer::singleShot(1500, this, [this] { if (!active && capture.state() != QProcess::NotRunning) capture.kill(); });
    message = "采集已停止，正在完成剩余字幕"; emit changed();
}
void Backend::recognizeNext() {
    if (asrBusy || audioQueue.isEmpty()) return;
    asrBusy = true;
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
    field("prompt", ("Discussion about open-source software. 专业讨论：" + terms.prompt()).toUtf8());
    auto *reply = network.post(request(asrBase, "/inference"), multi); multi->setParent(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, job] {
        asrBusy = false;
        auto body = reply->readAll();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        const auto doc = QJsonDocument::fromJson(body);
        const auto raw = doc.object().value("text").toString().trimmed();
        if (!ok || !doc.isObject() || !doc.object().value("text").isString()) {
            rows.append(QVariantMap{{"original", "[这一段识别失败]"}, {"raw", ""},
                {"translation", "未生成译文"}, {"time", "—"}, {"meta", "错误"}}); dirty = true;
            stop(); setFailure("识别请求失败，已停止收音。已完成字幕保留，请检查模型日志。");
        } else {
            const auto time = QString("%1:%2").arg(job.endMs / 60000, 2, 10, QChar('0')).arg(job.endMs / 1000 % 60, 2, 10, QChar('0'));
            int index = provisionalRows.value(job.utterance, -1);
            if (!raw.isEmpty()) {
                const auto text = terms.normalize(raw);
                if (index < 0) {
                    index = int(rows.size());
                    rows.append(QVariantMap{});
                    provisionalRows.insert(job.utterance, index);
                }
                rows[index] = QVariantMap{{"original", text}, {"raw", raw}, {"translation", ""},
                    {"time", time}, {"meta", job.final ? "识别完成" : "正在说话"}};
                dirty = true; emit captionUpdated(index);
            }
            if (job.final && index >= 0) {
                auto row = rows[index].toMap();
                row["translation"] = "正在本地翻译…";
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
    auto payload = Translation::payload(job.text, language, terms.prompt());
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
        row["meta"] = "正在翻译";
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
            row["translation"] = stream->text.isEmpty() ? "[翻译失败，原文已保留]" : stream->text + "\n[译文未完成]";
            row["meta"] = "错误";
            stop(); setFailure("翻译请求失败或输出不完整，已停止收音。请检查本地翻译服务。");
        } else {
            row["translation"] = stream->text.trimmed();
            const double latency = std::max<qint64>(0, clock.elapsed() - job.endMs) / 1000.0;
            row["meta"] = (language == "zh" ? "EN → 中文" : "中文 → EN") + QString(" · 处理 %1s").arg(latency, 0, 'f', 1);
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
        setFailure("无法读取字幕记录：" + path); return false;
    }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto object = doc.object();
    if (object.value("format_version").toInt() != 1 || !object.value("captions").isArray()) {
        setFailure("字幕记录格式无效"); return false;
    }
    const auto values = object.value("captions").toArray();
    for (const auto &value : values) {
        const auto row = value.toObject();
        if (!row.value("original").isString() || !row.value("translation").isString()) {
            setFailure("字幕记录缺少原文或译文"); return false;
        }
    }
    rows = values.toVariantList(); dirty = false;
    message = "已恢复字幕记录 · 可以继续开始收音";
    emit captionsChanged(); emit changed(); return true;
}
void Backend::exportSession() {
    if (rows.isEmpty() || isDemo) return;
    const auto defaultDir = root == "/usr/share/live-sub"
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/sessions" : root + "/sessions";
    const auto dir = qEnvironmentVariable("LIVE_SUB_SESSIONS", defaultDir);
    if (!QDir().mkpath(dir)) { setFailure("无法创建导出目录：" + dir); return; }
    const auto name = "session-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + "-" + QUuid::createUuid().toString(QUuid::Id128).left(8) + ".json";
    QSaveFile file(dir + "/" + name);
    const QJsonObject output{{"exported_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
        {"captions", QJsonArray::fromVariantList(rows)}, {"format_version", 1}};
    const auto bytes = QJsonDocument(output).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        setFailure("导出失败：" + file.errorString()); return;
    }
    dirty = false; message = "已导出：" + file.fileName(); emit changed();
}
