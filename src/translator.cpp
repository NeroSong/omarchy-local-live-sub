#include "translator.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QLocale>

Translator::Translator(const QString &settingsPath, const QString &languagePath,
                       const QString &catalogDirectory, QObject *parent)
    : QTranslator(parent), languageFile(languagePath.isEmpty() ? QDir::homePath() + "/.config/omarchy/language" : languagePath),
      catalogDir(catalogDirectory), settings(settingsPath, QSettings::IniFormat) {
    selected = qEnvironmentVariable("LIVE_SUB_UI_LANGUAGE", settings.value("ui/language", "auto").toString());
    if (!QStringList{"auto", "zh-CN", "en"}.contains(selected)) selected = "auto";
    connect(&timer, &QTimer::timeout, this, &Translator::reload);
    timer.start(1000);
    reload();
}
void Translator::setSelection(const QString &selection) {
    if (!QStringList{"auto", "zh-CN", "en"}.contains(selection) || selection == selected) return;
    selected = selection;
    settings.setValue("ui/language", selected);
    settings.sync();
    const auto previous = effective;
    reload();
    if (previous == effective) emit changed();
}
void Translator::reload() {
    auto next = selected;
    if (next == "auto") {
        QFile file(languageFile);
        next = file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readLine()).trimmed() : QLocale::system().name();
    }
    next = next.startsWith("zh", Qt::CaseInsensitive) ? "zh-CN" : "en";
    if (next == effective) return;
    QFile file(catalogDir + "/" + next + ".json");
    catalog = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
    effective = next;
    QLocale::setDefault(QLocale(effective));
    emit changed();
}
QString Translator::translate(const char *, const char *source, const char *, int) const {
    return catalog.value(QString::fromUtf8(source)).toString();
}
