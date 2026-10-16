#pragma once
#include <QJsonObject>
#include <QString>

class Translation {
public:
    static QString targetLanguage(const QString &text, const QString &sourceLanguage);
    static QJsonObject payload(const QString &text, const QString &target, const QString &terms);
    static QString result(const QJsonObject &response, QString &error);
};
