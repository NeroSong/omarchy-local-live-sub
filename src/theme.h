#pragma once
#include <QObject>
#include <QFileSystemWatcher>
#include <QTimer>
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
public:
    explicit Theme(QObject *parent = nullptr);
    QString accent() const { return color; }
private:
    void reload();
    QString color = "#FFD60A";
    QFileSystemWatcher watcher;
    QTimer timer;
signals:
    void changed();
};
