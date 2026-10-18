#pragma once
#include <QJsonArray>
#include <QString>
#include <QVariantList>
class Terms {
public:
    struct Result { QString text; QVariantList corrections; };
    Result correct(const QString &text, const QString &context = {}) const;
    bool load(const QString &path, QString &error);
    QString normalize(QString text) const;
    QString prompt() const;
    QString translationHints(const QString &target) const;
    QString revision() const { return loadedRevision; }
private:
    QJsonArray entries;
    QString loadedRevision;
};
