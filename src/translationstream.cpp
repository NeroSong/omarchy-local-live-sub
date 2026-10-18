#include "translationstream.h"
#include "translation.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

void TranslationStream::feed(const QByteArray &bytes) {
    pending += bytes;
    qsizetype end;
    while ((end = pending.indexOf('\n')) >= 0) {
        auto line = pending.first(end);
        pending.remove(0, end + 1);
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) event();
        else if (line.startsWith("data:")) {
            if (!data.isEmpty()) data += '\n';
            data += line.mid(5).trimmed();
        }
    }
}
void TranslationStream::event() {
    const auto body = data; data.clear();
    if (body.isEmpty() || done || !error.isEmpty()) return;
    if (body == "[DONE]") { done = true; text = Translation::cleanOutput(rawText); return; }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(body, &parse);
    if (!doc.isObject() || parse.error != QJsonParseError::NoError) { error = "翻译流数据无效"; return; }
    const auto object = doc.object();
    if (object.contains("error")) { error = "翻译服务返回错误"; return; }
    const auto choices = object.value("choices").toArray();
    if (choices.isEmpty()) return;
    const auto choice = choices.first().toObject();
    rawText += choice.value("delta").toObject().value("content").toString();
    const auto reason = choice.value("finish_reason").toString();
    if (!reason.isEmpty()) {
        stopped = reason == "stop";
        if (!stopped) error = "译文输出未正常结束";
    }
    text = Translation::cleanOutput(rawText, !reason.isEmpty());
}
