#pragma once
#include <QByteArray>
#include <QString>

// Parse complete SSE events before decoding JSON, so split UTF-8 stays intact.
class TranslationStream {
public:
    void feed(const QByteArray &bytes);
    QString text;
    QString error;
    bool complete() const { return done && stopped && error.isEmpty() && !text.trimmed().isEmpty(); }
private:
    void event();
    QByteArray pending, data;
    QString rawText;
    bool done = false, stopped = false;
};
