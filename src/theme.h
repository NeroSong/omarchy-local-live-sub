#pragma once
#include <QObject>
#include <QFileSystemWatcher>
#include <QTimer>
#include <QColor>
#include <QHash>
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor translationForeground READ translationForeground NOTIFY changed)
    Q_PROPERTY(QColor secondaryForeground READ secondaryForeground NOTIFY changed)
    Q_PROPERTY(QColor mutedForeground READ mutedForeground NOTIFY changed)
    Q_PROPERTY(QColor panelBackground READ panelBackground NOTIFY changed)
    Q_PROPERTY(QColor selection READ selection NOTIFY changed)
    Q_PROPERTY(QColor border READ border NOTIFY changed)
    Q_PROPERTY(QColor positive READ positive NOTIFY changed)
    Q_PROPERTY(QColor warning READ warning NOTIFY changed)
    Q_PROPERTY(QColor error READ error NOTIFY changed)
    Q_PROPERTY(bool lightMode READ lightMode NOTIFY changed)
public:
    explicit Theme(const QString &colorsFile = {}, QObject *parent = nullptr);
    QColor accent() const { return palette.value("accent"); }
    QColor accentForeground() const { return palette.value("accentForeground"); }
    QColor background() const { return palette.value("background"); }
    QColor foreground() const { return palette.value("foreground"); }
    QColor translationForeground() const { return palette.value("translationForeground"); }
    QColor secondaryForeground() const { return palette.value("secondaryForeground"); }
    QColor mutedForeground() const { return palette.value("mutedForeground"); }
    QColor panelBackground() const { return palette.value("panelBackground"); }
    QColor selection() const { return palette.value("selection"); }
    QColor border() const { return palette.value("border"); }
    QColor positive() const { return palette.value("positive"); }
    QColor warning() const { return palette.value("warning"); }
    QColor error() const { return palette.value("error"); }
    bool lightMode() const { return light; }
    static double contrast(const QColor &first, const QColor &second);
private:
    void reload();
    void apply(const QHash<QString, QColor> &colors, const QString &mode);
    QString path;
    QHash<QString, QColor> palette;
    bool light = false;
    QFileSystemWatcher watcher;
    QTimer timer, poll;
signals:
    void changed();
};
