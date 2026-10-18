#include "theme.h"
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QFileInfo>
#include <cmath>

namespace {
double luminance(const QColor &color) {
    auto linear = [](double channel) { return channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(color.redF()) + 0.7152 * linear(color.greenF()) + 0.0722 * linear(color.blueF());
}
QColor mix(const QColor &first, const QColor &second, double amount) {
    return QColor::fromRgbF(first.redF() * (1 - amount) + second.redF() * amount,
        first.greenF() * (1 - amount) + second.greenF() * amount,
        first.blueF() * (1 - amount) + second.blueF() * amount);
}
QColor readable(const QColor &color, const QColor &background) {
    if (Theme::contrast(color, background) >= 4.5) return color;
    const QColor target = Theme::contrast(Qt::black, background) > Theme::contrast(Qt::white, background) ? Qt::black : Qt::white;
    for (int step = 1; step <= 20; ++step) {
        const auto adjusted = mix(color, target, step / 20.0);
        if (Theme::contrast(adjusted, background) >= 4.5) return adjusted;
    }
    return target;
}
}

double Theme::contrast(const QColor &first, const QColor &second) {
    const auto a = luminance(first), b = luminance(second);
    return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}
Theme::Theme(const QString &colorsFile, QObject *parent) : QObject(parent),
    path(colorsFile.isEmpty() ? qEnvironmentVariable("LIVE_SUB_THEME_FILE", QDir::homePath() + "/.local/state/omarchy/current/theme/colors.toml") : colorsFile) {
    apply({{"background", QColor("#101418")}, {"foreground", QColor("#f1f4f6")}, {"accent", QColor("#FFD60A")}}, "dark");
    timer.setSingleShot(true); timer.setInterval(100);
    connect(&watcher, &QFileSystemWatcher::fileChanged, this, [this] { timer.start(); });
    connect(&watcher, &QFileSystemWatcher::directoryChanged, this, [this] { timer.start(); });
    connect(&timer, &QTimer::timeout, this, &Theme::reload);
    // Recover if a replaced symlink or temporarily missing directory drops a watch.
    poll.setInterval(1000);
    connect(&poll, &QTimer::timeout, this, &Theme::reload);
    poll.start();
    reload();
}
void Theme::reload() {
    const auto paths = watcher.files() + watcher.directories();
    if (!paths.isEmpty()) watcher.removePaths(paths);
    QStringList watchPaths{path};
    auto parent = QFileInfo(path).absoluteDir();
    for (int i = 0; i < 3; ++i) { watchPaths.append(parent.absolutePath()); parent.cdUp(); }
    const auto resolved = QFileInfo(path).canonicalFilePath();
    if (!resolved.isEmpty()) watchPaths << resolved << QFileInfo(resolved).absolutePath();
    watchPaths.removeDuplicates();
    for (const auto &item : watchPaths) if (QFileInfo::exists(item)) watcher.addPath(item);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024) return;
    const auto text = QString::fromUtf8(file.readAll());
    const QRegularExpression pattern(R"re(^\s*([a-z_]+)\s*=\s*["'](#[0-9a-fA-F]{6})["'])re", QRegularExpression::MultilineOption);
    QHash<QString, QColor> colors;
    auto matches = pattern.globalMatch(text);
    while (matches.hasNext()) { const auto match = matches.next(); colors.insert(match.captured(1), QColor(match.captured(2))); }
    // Keep the last valid palette during incomplete writes or a theme transition.
    if (!colors.contains("background") || !colors.contains("foreground")) return;
    const auto mode = QRegularExpression(R"re(^\s*mode\s*=\s*["'](light|dark)["'])re", QRegularExpression::MultilineOption).match(text);
    apply(colors, mode.hasMatch() ? mode.captured(1) : QString{});
}
void Theme::apply(const QHash<QString, QColor> &colors, const QString &mode) {
    const auto bg = colors.value("background"), fg = readable(colors.value("foreground"), bg);
    const bool nextLight = mode.isEmpty() ? luminance(bg) > 0.5 : mode == "light";
    const auto accent = colors.value("accent", fg);
    const auto panel = colors.value("lighter_background", mix(bg, fg, 0.05));
    auto onBoth = [&](const QColor &candidate) {
        const auto onBackground = readable(candidate, bg);
        const auto onPanel = readable(onBackground, panel);
        return contrast(onPanel, bg) >= 4.5 ? onPanel : fg;
    };
    const QHash<QString, QColor> next{{"background", bg}, {"foreground", fg}, {"accent", accent},
        {"accentForeground", contrast(Qt::black, accent) > contrast(Qt::white, accent) ? QColor(Qt::black) : QColor(Qt::white)},
        {"translationForeground", readable(accent, bg)}, {"panelBackground", panel},
        {"secondaryForeground", onBoth(colors.value("light_foreground", mix(fg, bg, 0.15)))},
        {"mutedForeground", onBoth(colors.value("dark_foreground", mix(fg, bg, 0.35)))},
        {"selection", colors.value("selection", mix(bg, fg, 0.1))}, {"border", mix(bg, fg, 0.28)},
        {"positive", readable(colors.value("green", QColor("#8ed8b5")), bg)},
        {"warning", readable(colors.value("yellow", QColor("#dfbc79")), bg)},
        {"error", onBoth(colors.value("red", QColor("#e59186")))}};
    if (next != palette || nextLight != light) { palette = next; light = nextLight; emit changed(); }
}
