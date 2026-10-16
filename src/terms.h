#pragma once
#include <QJsonArray>
#include <QString>
class Terms {
public:
    bool load(const QString &path, QString &error);
    QString normalize(QString text) const;
    QString prompt() const;
private:
    QJsonArray entries;
};
