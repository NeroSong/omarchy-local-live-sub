#include "theme.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QFileInfo>
Theme::Theme(QObject *parent) : QObject(parent) {
    timer.setSingleShot(true); timer.setInterval(100);
    connect(&watcher, &QFileSystemWatcher::fileChanged, this, [this] { timer.start(); });
    connect(&watcher, &QFileSystemWatcher::directoryChanged, this, [this] { timer.start(); });
    connect(&timer, &QTimer::timeout, this, &Theme::reload);
    reload();
}
void Theme::reload() {
    const auto paths = watcher.files() + watcher.directories();
    if (!paths.isEmpty()) watcher.removePaths(paths);
    const auto base = QDir::homePath() + "/.local/state/omarchy/current";
    for (const auto &p : QStringList{QFileInfo(base).absolutePath(), base, base + "/theme", base + "/theme/colors.toml"})
        if (QFileInfo::exists(p)) watcher.addPath(p);
    QFile file(base + "/theme/colors.toml");
    QString next = "#FFD60A";
    if (file.open(QIODevice::ReadOnly)) {
        QRegularExpression pattern(R"re(^\s*accent\s*=\s*["'](#[0-9a-fA-F]{6})["'])re", QRegularExpression::MultilineOption);
        auto match = pattern.match(QString::fromUtf8(file.readAll()));
        if (match.hasMatch()) next = match.captured(1);
    }
    if (color != next) { color = next; emit changed(); }
}
