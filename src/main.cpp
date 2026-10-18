#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QStandardPaths>
#include "backend.h"
#include "theme.h"
#include "translator.h"
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("live-sub"); app.setApplicationVersion("0.1.0");
    app.setDesktopFileName("live-sub");
    QQuickStyle::setStyle("Material");
    const auto args = app.arguments();
    const auto project = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..");
    const auto defaultRoot = QFileInfo::exists(project + "/glossary.json") ? project : QString("/usr/share/live-sub");
    const auto root = qEnvironmentVariable("LIVE_SUB_ROOT", defaultRoot);
    const auto settingsPath = qEnvironmentVariable("LIVE_SUB_UI_SETTINGS", root == "/usr/share/live-sub"
        ? QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/ui.ini" : root + "/runtime/ui.ini");
    Translator translator(settingsPath);
    app.installTranslator(&translator);
    Backend backend(root, args.contains("--demo"));
    const int restore = args.indexOf("--restore-session");
    if (restore >= 0 && (restore + 1 >= args.size() || !backend.restoreSession(args[restore + 1])))
        return 2;
    Theme theme;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("theme", &theme);
    engine.rootContext()->setContextProperty("translator", &translator);
    QObject::connect(&translator, &Translator::changed, &engine, &QQmlEngine::retranslate);
    QObject::connect(&translator, &Translator::changed, &backend, &Backend::changed);
    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 1;
    if (args.contains("--reading-preview") && args.contains("--demo"))
        engine.rootObjects().first()->setProperty("previewReading", true);
    if (args.contains("--settings") || (args.contains("--settings-preview") && args.contains("--demo")))
        QMetaObject::invokeMethod(engine.rootObjects().first(), "showSettings");
    if (args.contains("--screenshot")) {
        QTimer::singleShot(1200, &app, [&] {
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            const bool ok = window && window->grabWindow().save(root + "/build/preview.png");
            app.exit(ok ? 0 : 2);
        });
    }
    return app.exec();
}
