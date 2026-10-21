#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include <QtEndian>
#include "segmenter.h"
#include "terms.h"
#include "translation.h"
#include "translationstream.h"
#include "backend.h"
#include "translator.h"
#include "agentcontext.h"
#include "theme.h"
#include <QScopeGuard>
#include <QSaveFile>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <sys/stat.h>
#include "privatestorage.h"
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QQuickStyle>

class TestEnvironment {
public:
    void set(const QByteArray &key, const QByteArray &value) {
        remember(key); qputenv(key.constData(), value);
    }
    void unset(const QByteArray &key) {
        remember(key); qunsetenv(key.constData());
    }
    ~TestEnvironment() {
        for (auto it = old.cbegin(); it != old.cend(); ++it)
            if (present.contains(it.key())) qputenv(it.key().constData(), it.value());
            else qunsetenv(it.key().constData());
    }
private:
    void remember(const QByteArray &key) {
        if (old.contains(key)) return;
        old.insert(key, qgetenv(key.constData()));
        if (qEnvironmentVariableIsSet(key.constData())) present.insert(key);
    }
    QHash<QByteArray, QByteArray> old;
    QSet<QByteArray> present;
};

class LiveSubTests : public QObject {
    Q_OBJECT
    QByteArray sound(int frames) {
        QByteArray data(frames * 640, '\0');
        for (int i = 0; i < data.size(); i += 2) qToLittleEndian<qint16>((i % 4) ? -5000 : 5000, data.data() + i);
        return data;
    }
    bool loadForcedTerms(Terms &terms, QString &error) {
        QFile source(QFINDTESTDATA("../glossary.json"));
        if (!source.open(QIODevice::ReadOnly)) return false;
        auto entries = QJsonDocument::fromJson(source.readAll()).array();
        auto omarchy = entries.first().toObject();
        omarchy["force_phonetic"] = true;
        entries[0] = omarchy;
        QTemporaryDir dir;
        QFile fixture(dir.filePath("forced.json"));
        if (!fixture.open(QIODevice::WriteOnly)) return false;
        fixture.write(QJsonDocument(entries).toJson()); fixture.close();
        return terms.load(fixture.fileName(), error);
    }
private slots:
    void initTestCase() {
        QGuiApplication::setQuitOnLastWindowClosed(false);
        QQuickStyle::setStyle("Material");
    }
    void windowCanCancelStartup_data() {
        QTest::addColumn<bool>("translationProbe");
        QTest::addColumn<bool>("cancelButton");
        QTest::newRow("close-during-asr") << false << false;
        QTest::newRow("close-during-translation") << true << false;
        QTest::newRow("cancel-button-during-asr") << false << true;
        QTest::newRow("cancel-button-during-translation") << true << true;
    }
    void windowCanCancelStartup() {
        QFETCH(bool, translationProbe);
        QFETCH(bool, cancelButton);
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QByteArray &bytes, bool executable = false) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return false;
            file.close();
            return !executable || file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        };
        QVERIFY(write("glossary.json", "[]"));
        QVERIFY(write("ffmpeg", "#!/bin/sh\nprintf started > \"$LIVE_SUB_TEST_CAPTURE_MARKER\"\n", true));
        QVERIFY(write("pactl", "#!/bin/sh\nprintf '[]\\n'\n", true));
        QTcpServer asr, translation;
        QVERIFY(asr.listen(QHostAddress::LocalHost));
        QVERIFY(translation.listen(QHostAddress::LocalHost));
        TestEnvironment env;
        env.set("PATH", dir.path().toUtf8());
        env.set("LIVE_SUB_CONFIG_DIR", dir.path().toUtf8());
        env.set("LIVE_SUB_TEST_CAPTURE_MARKER", dir.filePath("captured").toUtf8());
        env.set("LIVE_SUB_ASR_URL", ("http://127.0.0.1:" + QString::number(asr.serverPort())).toUtf8());
        env.set("LIVE_SUB_TRANSLATION_URL", ("http://127.0.0.1:" + QString::number(translation.serverPort())).toUtf8());
        auto healthy = [](QTcpSocket *socket) {
            if (socket->state() != QAbstractSocket::ConnectedState) return;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
            socket->disconnectFromHost();
        };
        Backend backend(dir.path());
        Theme theme(dir.filePath("colors.toml"));
        Translator translator(dir.filePath("ui.ini"), dir.filePath("language"));
        QQmlApplicationEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("translator", &translator);
        engine.load(QUrl("qrc:/Main.qml"));
        QVERIFY(!engine.rootObjects().isEmpty());
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        QVERIFY(window); QVERIFY(window->isVisible());
        backend.toggle("test-microphone", "auto", -42);
        QTRY_VERIFY(asr.hasPendingConnections());
        auto *pending = asr.nextPendingConnection();
        if (translationProbe) {
            healthy(pending);
            QTRY_VERIFY(translation.hasPendingConnections());
            pending = translation.nextPendingConnection();
        }
        if (cancelButton) {
            auto *button = window->findChild<QObject *>("stopCaptionsButton");
            QVERIFY(button); QVERIFY(button->property("enabled").toBool());
            QCOMPARE(button->property("text").toString(), QString("Cancel startup"));
            QVERIFY(QMetaObject::invokeMethod(button, "clicked"));
            QVERIFY(!backend.startingCapture());
        }
        QVERIFY(window->close());
        QVERIFY(!window->isVisible());
        healthy(pending);
        QTest::qWait(100);
        QVERIFY(!backend.startingCapture()); QVERIFY(!backend.recording()); QVERIFY(!backend.busy());
        QVERIFY(!QFileInfo::exists(dir.filePath("captured")));
    }
    void startupCancellation_data() {
        QTest::addColumn<bool>("translationProbe");
        QTest::addColumn<bool>("toggleAgain");
        QTest::newRow("stop-during-asr") << false << false;
        QTest::newRow("stop-during-translation") << true << false;
        QTest::newRow("toggle-during-asr") << false << true;
        QTest::newRow("toggle-during-translation") << true << true;
    }
    void startupCancellation() {
        QFETCH(bool, translationProbe);
        QFETCH(bool, toggleAgain);
        QTemporaryDir dir;
        auto write = [&](const QString &name, const QByteArray &bytes, bool executable = false) {
            QFile file(dir.filePath(name));
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return false;
            file.close();
            return !executable || file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        };
        QVERIFY(write("glossary.json", "[]"));
        // A regression must never acquire a real microphone, even on developer machines.
        QVERIFY(write("ffmpeg", "#!/bin/sh\nprintf started > \"$LIVE_SUB_TEST_CAPTURE_MARKER\"\n", true));
        QVERIFY(write("pactl", "#!/bin/sh\nprintf '[]\\n'\n", true));
        QTcpServer asr, translation;
        QVERIFY(asr.listen(QHostAddress::LocalHost));
        QVERIFY(translation.listen(QHostAddress::LocalHost));
        TestEnvironment env;
        env.set("PATH", dir.path().toUtf8());
        env.set("LIVE_SUB_CONFIG_DIR", dir.path().toUtf8());
        env.set("LIVE_SUB_TEST_CAPTURE_MARKER", dir.filePath("captured").toUtf8());
        env.set("LIVE_SUB_ASR_URL", ("http://127.0.0.1:" + QString::number(asr.serverPort())).toUtf8());
        env.set("LIVE_SUB_TRANSLATION_URL", ("http://127.0.0.1:" + QString::number(translation.serverPort())).toUtf8());
        auto healthy = [](QTcpSocket *socket) {
            if (socket->state() != QAbstractSocket::ConnectedState) return;
            socket->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
            socket->disconnectFromHost();
        };
        Backend backend(dir.path());
        backend.toggle("test-microphone", "auto", -42);
        QVERIFY(backend.startingCapture()); QVERIFY(backend.busy()); QVERIFY(!backend.recording());
        QTRY_VERIFY(asr.hasPendingConnections());
        auto *pending = asr.nextPendingConnection();
        if (translationProbe) {
            healthy(pending);
            QTRY_VERIFY(translation.hasPendingConnections());
            pending = translation.nextPendingConnection();
        }
        if (toggleAgain) backend.toggle("test-microphone", "auto", -42);
        else backend.stop();
        QVERIFY(!backend.startingCapture()); QVERIFY(!backend.recording()); QVERIFY(!backend.busy());
        healthy(pending); // A late successful response cannot restart capture.
        QTest::qWait(100);
        QVERIFY(!QFileInfo::exists(dir.filePath("captured")));
        QVERIFY(!backend.startingCapture()); QVERIFY(!backend.recording());

        // A deliberate new start still works; an old cancellation is not sticky.
        backend.toggle("test-microphone", "auto", -42);
        QTRY_VERIFY(asr.hasPendingConnections());
        healthy(asr.nextPendingConnection());
        QTRY_VERIFY(translation.hasPendingConnections());
        healthy(translation.nextPendingConnection());
        QTRY_VERIFY(QFileInfo::exists(dir.filePath("captured")));
        QTRY_VERIFY(!backend.busy());
    }
    void privateStorageDoesNotFollowLinksOrChangeOverrideDirectories() {
        QTemporaryDir dir;
        QString error;
        const auto target = dir.filePath("private");
        QVERIFY(PrivateStorage::ensureDirectory(target, error));
        struct stat info {};
        QVERIFY(::stat(QFile::encodeName(target).constData(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0700));
        const auto link = dir.filePath("link");
        QVERIFY(QFile::link(target, link));
        QVERIFY(!PrivateStorage::ensureDirectory(link, error));
        QVERIFY(::chmod(QFile::encodeName(target).constData(), 0755) == 0);
        QVERIFY(!PrivateStorage::ensureDirectory(target, error, false));
        QVERIFY(::stat(QFile::encodeName(target).constData(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0755));
        QVERIFY(PrivateStorage::ensureDirectory(target, error));
        QVERIFY(!PrivateStorage::ensureDirectory("", error));
        QVERIFY(!PrivateStorage::ensureDirectory(".", error));
    }
    void exportedCaptionsArePrivateAndUseUserDataDirectory() {
        QTemporaryDir dir;
        const auto originalMask = ::umask(0022);
        const auto restoreMask = qScopeGuard([&] { ::umask(originalMask); });
        QFile fixture(dir.filePath("input.json"));
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write(R"({"format_version":1,"captions":[{"original":"Private original","translation":"Private translation","raw":"Raw input"}]})");
        fixture.close();
        TestEnvironment env;
        env.set("LIVE_SUB_DATA_DIR", dir.filePath("user-data").toUtf8());
        env.set("LIVE_SUB_CONFIG_DIR", dir.path().toUtf8());
        env.unset("LIVE_SUB_SESSIONS");
        Backend backend(dir.path());
        QVERIFY(backend.restoreSession(fixture.fileName()));
        backend.exportSession();
        const auto sessions = dir.filePath("user-data/sessions");
        const auto exported = QDir(sessions).entryList({"*.json"}, QDir::Files);
        QCOMPARE(exported.size(), 1);
        QVERIFY(!QDir(dir.filePath("sessions")).exists());
        struct stat info {};
        QVERIFY(::stat(QFile::encodeName(sessions).constData(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0700));
        QVERIFY(::stat(QFile::encodeName(sessions + "/" + exported.first()).constData(), &info) == 0);
        QCOMPARE(info.st_mode & 0777, mode_t(0600));
        QFile output(sessions + "/" + exported.first());
        QVERIFY(output.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(output.readAll()).object()["captions"].toArray().first().toObject()["raw"].toString(), QString("Raw input"));
        QVERIFY(!backend.unsaved());
    }
    void themeFollowsAtomicLightDarkSwitchAndKeepsCaptionsReadable() {
        QTemporaryDir dir;
        const auto path = dir.filePath("colors.toml");
        auto write = [&](const QByteArray &bytes) {
            QSaveFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(bytes); QVERIFY(file.commit());
        };
        write("mode='dark'\nbackground='#1a1b26'\nforeground='#a9b1d6'\naccent='#7aa2f7'\n");
        Theme theme(path);
        QVERIFY(!theme.lightMode()); QCOMPARE(theme.background(), QColor("#1a1b26"));
        QVERIFY(Theme::contrast(theme.foreground(), theme.background()) >= 4.5);
        QVERIFY(Theme::contrast(theme.translationForeground(), theme.background()) >= 4.5);
        write("mode='light'\nbackground='#faf4ed'\nforeground='#575279'\naccent='#56949f'\ndark_foreground='#9893a5'\nlighter_background='#f2e9e1'\n");
        QTRY_VERIFY_WITH_TIMEOUT(theme.lightMode(), 3000);
        QCOMPARE(theme.background(), QColor("#faf4ed")); QCOMPARE(theme.accent(), QColor("#56949f"));
        QVERIFY(Theme::contrast(theme.translationForeground(), theme.background()) >= 4.5);
        QVERIFY(Theme::contrast(theme.mutedForeground(), theme.panelBackground()) >= 4.5);
        write("background='#101418'\nforeground='#eeeeee'\naccent='#202020'\n");
        QTRY_COMPARE_WITH_TIMEOUT(theme.background(), QColor("#101418"), 3000);
        QVERIFY(!theme.lightMode());
        QVERIFY(Theme::contrast(theme.translationForeground(), theme.background()) >= 4.5);
        write("background='invalid'\nforeground='#000000'\n");
        QTest::qWait(200); QCOMPARE(theme.background(), QColor("#101418"));
    }
    void themeRecoversAfterPaletteSymlinkReplacement() {
        QTemporaryDir dir;
        const auto first = dir.filePath("dark.toml"), second = dir.filePath("light.toml"), link = dir.filePath("colors.toml");
        QFile dark(first); QVERIFY(dark.open(QIODevice::WriteOnly)); dark.write("background='#101418'\nforeground='#eeeeee'\naccent='#ffd60a'\n"); dark.close();
        QFile light(second); QVERIFY(light.open(QIODevice::WriteOnly)); light.write("background='#ffffff'\nforeground='#111111'\naccent='#0066cc'\n"); light.close();
        QVERIFY(QFile::link(first, link));
        Theme theme(link); QVERIFY(!theme.lightMode());
        QVERIFY(QFile::remove(link)); QVERIFY(QFile::link(second, link));
        QTRY_COMPARE_WITH_TIMEOUT(theme.background(), QColor("#ffffff"), 3000);
        QVERIFY(theme.lightMode());
        QSaveFile replacement(second); QVERIFY(replacement.open(QIODevice::WriteOnly)); replacement.write("background='#eeeeee'\nforeground='#222222'\naccent='#5522aa'\n"); QVERIFY(replacement.commit());
        QTRY_COMPARE_WITH_TIMEOUT(theme.background(), QColor("#eeeeee"), 3000);
    }
    void interfaceLanguagePersistsAndFollowsOmarchyWithoutChangingSpeech() {
        QTemporaryDir dir;
        const auto languagePath = dir.filePath("language");
        QFile language(languagePath); QVERIFY(language.open(QIODevice::WriteOnly)); language.write("en\n"); language.close();
        const auto catalog = QFileInfo(QFINDTESTDATA("../src/i18n/zh-CN.json")).absolutePath();
        Translator translator(dir.filePath("ui.ini"), languagePath, catalog);
        translator.setSelection("auto");
        QCOMPARE(translator.language(), QString("en"));
        Backend backend(dir.path(), true);
        QVERIFY(QCoreApplication::installTranslator(&translator));
        const auto uninstall = qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
        const auto caption = backend.captionAt(0);
        translator.setSelection("zh-CN");
        QVERIFY(AgentContext::arguments(dir.path(), dir.path(), translator.language()).last().startsWith("当前应用的界面语言是简体中文"));
        QCOMPARE(QCoreApplication::translate("ui", "Settings"), QString("设置"));
        QVERIFY(backend.status().startsWith("演示模式"));
        Translator restored(dir.filePath("ui.ini"), languagePath, catalog);
        QCOMPARE(restored.selection(), QString("zh-CN"));
        translator.setSelection("en");
        QVERIFY(AgentContext::arguments(dir.path(), dir.path(), translator.language()).last().startsWith("The application's current interface language is English"));
        QCOMPARE(QCoreApplication::translate("ui", "Settings"), QString("Settings"));
        QVERIFY(backend.status().startsWith("Demo"));
        translator.setSelection("auto");
        QVERIFY(language.open(QIODevice::WriteOnly | QIODevice::Truncate)); language.write("zh-CN\n"); language.close();
        QTRY_COMPARE_WITH_TIMEOUT(translator.language(), QString("zh-CN"), 3000);
        translator.setSelection("invalid"); QCOMPARE(translator.selection(), QString("auto"));
        QCOMPARE(backend.captionAt(0), caption);
    }
    void vocabularyAgentGetsLiteralPathsAndGuidanceWithoutCaptionData() {
        const QString directory = "/tmp/topic with spaces;$(do-not-run)";
        const auto arguments = AgentContext::arguments(directory, "/tmp/project", "en");
        QCOMPARE(arguments.mid(0, 4), QStringList({"launch", "tui", "--app-id=org.omarchy.agent", "env"}));
        QCOMPARE(arguments[4], "--chdir=" + directory);
        QCOMPARE(arguments.mid(5, 4), QStringList({"omarchy", "agent", "prompt", "--inline"}));
        const auto prompt = arguments.last();
        QVERIFY(prompt.contains(directory + "/glossary.json"));
        QVERIFY(prompt.contains(directory + "/asr-config.json"));
        QVERIFY(prompt.contains("/tmp/project/README.md"));
        QVERIFY(prompt.contains("come protected"));
        QVERIFY(prompt.contains("Do not modify configuration until"));
        QVERIFY(prompt.contains("save each file atomically"));
        QVERIFY(!prompt.contains("Let's talk about Omarchy"));
        QVERIFY(prompt.contains("Use English from your first reply"));
        const auto chinese = AgentContext::arguments(directory, "/tmp/project", "zh-CN").last();
        QVERIFY(chinese.startsWith("当前应用的界面语言是简体中文"));
        QVERIFY(chinese.contains("从第一条回复开始"));
        QVERIFY(chinese.contains("\"interface_language\": \"zh-CN\""));
    }
    void backendReloadsTopicDirectoryWithoutChangingExistingCaptions() {
        QTemporaryDir dir; QFile f(dir.filePath("glossary.json"));
        auto write = [&](const QByteArray &data) { QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(data); f.close(); };
        write("[]");
        const bool hadOverride = qEnvironmentVariableIsSet("LIVE_SUB_CONFIG_DIR");
        const auto previous = qgetenv("LIVE_SUB_CONFIG_DIR");
        qputenv("LIVE_SUB_CONFIG_DIR", dir.path().toUtf8());
        Backend backend("/unused-packaged-root", true);
        if (hadOverride) qputenv("LIVE_SUB_CONFIG_DIR", previous); else qunsetenv("LIVE_SUB_CONFIG_DIR");
        QCOMPARE(backend.configDirectory(), dir.path());
        const auto firstStatus = backend.configStatus();
        const auto original = backend.captionAt(0);
        write(R"([{"term":"Beelink","aliases":["bee link"]}])");
        QTRY_VERIFY_WITH_TIMEOUT(backend.configStatus() != firstStatus, 3000);
        const auto validStatus = backend.configStatus();
        write("[");
        QTRY_VERIFY_WITH_TIMEOUT(backend.configStatus().contains("previous valid"), 3000);
        write(R"([{"term":"Beelink","aliases":["bee link"]}])");
        QTRY_COMPARE_WITH_TIMEOUT(backend.configStatus(), validStatus, 3000);
        QCOMPARE(backend.captionAt(0), original);
        QVERIFY(!backend.recording()); QVERIFY(backend.error().isEmpty());
    }
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
    void translationLabelsAreHiddenEvenWhenStreamedCharacterByCharacter() {
        TranslationStream stream;
        auto feed = [&](const QString &text, const QString &finish = {}) {
            const QJsonObject choice{{"delta", QJsonObject{{"content", text}}}, {"finish_reason", finish}};
            stream.feed("data: " + QJsonDocument(QJsonObject{{"choices", QJsonArray{choice}}}).toJson(QJsonDocument::Compact) + "\n\n");
        };
        for (const auto c : QString("[转录]\n[TrAnSlAtE] ")) {
            feed(QString(c)); QVERIFY2(stream.text.isEmpty(), qPrintable(stream.text));
        }
        feed("Hello [Rust]. The literal [translate] stays inside this sentence.", "stop");
        stream.feed("data: [DONE]\n\n");
        QVERIFY(stream.complete());
        QCOMPARE(stream.text, QString("Hello [Rust]. The literal [translate] stays inside this sentence."));
        QCOMPARE(Translation::cleanOutput(" [Transcript]:\n[翻译]：你好。\n[/translate]"), QString("你好。"));
        QCOMPARE(Translation::cleanOutput("[Rust] is unchanged."), QString("[Rust] is unchanged."));
        QCOMPARE(Translation::cleanOutput("[transfer] is unchanged."), QString("[transfer] is unchanged."));
        QCOMPARE(Translation::cleanOutput("[tra", false), QString());
        QCOMPARE(Translation::cleanOutput("[tra", true), QString("[tra"));
    }
    void nonStreamingTranslationRemovesLabelsAndRejectsLabelOnlyOutput() {
        QString error;
        auto response = [](const QString &text) {
            return QJsonObject{{"choices", QJsonArray{QJsonObject{{"finish_reason", "stop"}, {"message", QJsonObject{{"content", text}}}}}}};
        };
        QCOMPARE(Translation::result(response("[translation] Hello."), error), QString("Hello."));
        QVERIFY(error.isEmpty());
        QVERIFY(Translation::result(response("[转录]\n[translate]"), error).isEmpty());
        QVERIFY(!error.isEmpty());
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
    void liveGlossaryUpdatesAreTransactionalAndSnapshotsStayStable() {
        QTemporaryDir dir; QFile f(dir.filePath("terms.json"));
        auto write = [&](const QByteArray &data) { QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate)); f.write(data); f.close(); };
        write(R"([{"term":"Beelink","aliases":["零刻"],"translations":{"zh":"零刻","en":"Beelink"}}])");
        Terms terms; QString error; QVERIFY(terms.load(f.fileName(), error));
        const auto snapshot = terms, before = terms;
        const auto revision = terms.revision();
        QCOMPARE(terms.normalize("零刻"), QString("Beelink"));
        QCOMPARE(QJsonDocument::fromJson(terms.translationHints("zh").toUtf8()).object().value("Beelink").toString(), QString("零刻"));
        write(R"([{"term":"Beelink","aliases":["零刻","bee link"],"translations":{"zh":"零刻主机"}}])");
        QVERIFY(terms.load(f.fileName(), error)); QVERIFY(terms.revision() != revision);
        QCOMPARE(terms.normalize("bee link"), QString("Beelink"));
        QCOMPARE(snapshot.normalize("bee link"), QString("bee link"));
        QCOMPARE(snapshot.revision(), before.revision());
        const auto goodRevision = terms.revision();
        for (const auto &invalid : {QByteArray("["), QByteArray(R"([{"term":"Beelink","aliases":[],"translations":{"zh":42}}])"),
             QByteArray(R"([{"term":"Beelink","aliases":[]},{"term":"beelink","aliases":[]}])")}) {
            write(invalid); QVERIFY(!terms.load(f.fileName(), error)); QVERIFY(!error.isEmpty());
            QCOMPARE(terms.revision(), goodRevision);
            QCOMPARE(terms.normalize("bee link"), QString("Beelink"));
        }
        QVERIFY(f.remove()); QVERIFY(!terms.load(f.fileName(), error));
        QCOMPARE(terms.revision(), goodRevision);
        write("[]"); QVERIFY(terms.load(f.fileName(), error)); QVERIFY(error.isEmpty());
        QCOMPARE(terms.normalize("bee link"), QString("bee link"));
        QVERIFY(terms.translationHints("zh").isEmpty());
    }
    void translationMappingsUseTargetLanguageAndOverridePreservation() {
        auto payload = Translation::payload("Beelink is ready", "zh", "Beelink", R"({"Beelink":"零刻"})");
        const auto prompt = payload.value("messages").toArray().first().toObject().value("content").toString();
        QVERIFY(prompt.contains("override name preservation")); QVERIFY(prompt.contains(R"({"Beelink":"零刻"})"));
        QVERIFY(prompt.endsWith("[Transcript]\nBeelink is ready"));
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
        const QString raw = "rest, trust, hyper landscape; "
            "`hyper land` https://example.com/Hyprlnd /tmp/Hyprlnd --Hyprlnd "
            "```\nhyper land\n```";
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
    void defaultOmarchyRulesPreserveOrdinarySpeechWithAndWithoutContext() {
        Terms terms; QString error;
        QVERIFY(terms.load(QFINDTESTDATA("../glossary.json"), error));
        for (const auto &context : {QString(), QString("Linux desktop Omarchy")}) {
            for (const auto &text : {"How much does it cost?", "HOW MUCH is enough?", "I love how much it helps.",
                     "Thank you so much.", "That is too much.", "No, match the colors.",
                     "Oh, Mary is here.", "monarchy and anarchy", "Come on.", "I come here.",
                     "come", "COME", "Please come back."}) {
                const auto corrected = terms.correct(text, context);
                QCOMPARE(corrected.text, QString(text));
                QVERIFY(corrected.corrections.isEmpty());
            }
        }
        QCOMPARE(terms.correct("O much is great.").text, QString("O much is great."));
        QCOMPARE(terms.correct("Ohmarchy is great.").text, QString("Omarchy is great."));
        QCOMPARE(terms.correct("O much is great.", "Linux desktop").text, QString("Omarchy is great."));
    }
    void reportedOmarchyAliasesWorkWithoutRewritingCommonPhrases() {
        Terms terms; QString error;
        QVERIFY(terms.load(QFINDTESTDATA("../glossary.json"), error));
        for (const auto &context : {QString(), QString("Linux desktop")}) {
            QCOMPARE(terms.correct("o match", context).text, QString("Omarchy"));
            QCOMPARE(terms.correct("omachi", context).text, QString("Omarchy"));
            QCOMPARE(terms.correct("O MATCH and OMACHI.", context).text, QString("Omarchy and Omarchy."));
            const auto mixed = terms.correct("How much does o match cost? I like omachi so much.", context);
            QCOMPARE(mixed.text, QString("How much does Omarchy cost? I like Omarchy so much."));
            QCOMPARE(mixed.corrections.size(), 2);
            QCOMPARE(terms.correct("Come to omachi; that is too much.", context).text,
                QString("Come to Omarchy; that is too much."));
            const QString protectedText = "`o match` /tmp/omachi https://example.com/omachi omachine";
            QCOMPARE(terms.correct(protectedText, context).text, protectedText);
        }
    }
    void screenshotOmarchyAliasesAreCorrectedWithoutSwallowingSurroundingWords() {
        Terms terms; QString error;
        QVERIFY(terms.load(QFINDTESTDATA("../glossary.json"), error));
        const QList<QPair<QString, QString>> examples{
            {"Oh much is amazing.", "Omarchy is amazing."},
            {"嗯，我很喜欢我match这个系统。", "嗯，我很喜欢Omarchy这个系统。"},
            {"我很好奇我marcha未来会不会退出手机版。", "我很好奇Omarchy未来会不会退出手机版。"},
            {"这个世界不能缺少么 much。", "这个世界不能缺少Omarchy。"}
        };
        for (const auto &example : examples) {
            const auto result = terms.correct(example.first);
            QCOMPARE(result.text, example.second);
            QCOMPARE(result.corrections.size(), 1);
        }
        for (const auto &text : {"Actually, I love so much.", "As a system, I really love how much.",
                "How much do you like Omarchy?", "Come on, that is too much."}) {
            const auto result = terms.correct(text, "Linux desktop Omarchy");
            QCOMPARE(result.text, QString(text));
            QVERIFY(result.corrections.isEmpty());
        }
        const QString code = "`Oh much` /tmp/我marcha https://example.com/我match";
        QCOMPARE(terms.correct(code).text, code);
    }
    void omarchyForceModeNeedsNoContextAndPreservesFollowingWords() {
        Terms terms; QString error;
        QVERIFY(loadForcedTerms(terms, error));
        QCOMPARE(terms.correct("Oh, matching is great. Oh, Martin is a great sister. Oh much is great.").text,
            QString("Omarchy is great. Omarchy is a great sister. Omarchy is great."));
        QCOMPARE(terms.correct("O mar key is great. Oh my key is great.").text,
            QString("Omarchy is great. Omarchy is great."));
        QCOMPARE(terms.correct("欧玛琪很好，哦马基也是。").text, QString("Omarchy很好，Omarchy也是。"));
        QCOMPARE(terms.correct("`O much` /tmp/Ohmarchy monarchy anarchy").text,
            QString("`Omarchy` /tmp/Omarchy Omarchy Omarchy"));
        QCOMPARE(terms.correct("Omarchy is great.").text, QString("Omarchy is great."));
        QVERIFY(!terms.correct("Oh, Martin").corrections.isEmpty());
        QCOMPARE(terms.correct("No, match is great. I love how much. As the system how much is great. Oh mary. Or or ma.").text,
            QString("Omarchy is great. I love how much. As the system how much is great. Omarchy. Omarchy."));
        QCOMPARE(terms.correct("so much and too much").text, QString("so much and too much"));
    }
    void comeIsExemptEvenInsideForcedMultiwordCandidates() {
        Terms terms; QString error;
        QVERIFY(loadForcedTerms(terms, error));
        for (const auto &text : {"Come on.", "I come here.", "come", "COME", "Please come back."})
            QCOMPARE(terms.correct(text).text, QString(text));
        QCOMPARE(terms.correct("Come to O much. Oh much is great.").text,
            QString("Come to Omarchy. Omarchy is great."));
    }
};
QTEST_MAIN(LiveSubTests)
#include "live_sub_tests.moc"
