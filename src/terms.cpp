#include "terms.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

bool Terms::load(const QString &path, QString &error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = "无法读取术语表：" + path; return false; }
    QJsonParseError parse;
    auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (!doc.isArray() || parse.error != QJsonParseError::NoError) {
        error = "术语表必须是 JSON 数组：" + parse.errorString(); return false;
    }
    const auto values = doc.array();
    for (const auto &value : values) {
        const auto item = value.toObject();
        if (item.value("term").toString().trimmed().isEmpty() || !item.value("aliases").isArray()) {
            error = "每个术语需要非空 term 和 aliases 数组"; return false;
        }
        for (const auto &alias : item.value("aliases").toArray())
            if (!alias.isString() || alias.toString().trimmed().isEmpty()) {
                error = "术语别名必须是非空字符串"; return false;
            }
    }
    entries = values;
    return true;
}
QString Terms::normalize(QString text) const {
    // Replace only explicit aliases at word boundaries, never fuzzy-match common words.
    for (const auto &value : entries) {
        auto item = value.toObject();
        auto aliases = item.value("aliases").toArray();
        aliases.append(item.value("term"));
        for (const auto &alias : aliases) {
            QRegularExpression pattern("(?<![\\p{L}\\p{N}_])" + QRegularExpression::escape(alias.toString())
                + "(?![\\p{L}\\p{N}_])", QRegularExpression::CaseInsensitiveOption
                | QRegularExpression::UseUnicodePropertiesOption);
            // Literal replacement avoids interpreting backslashes in user terminology.
            auto matches = pattern.globalMatch(text);
            QList<QPair<int, int>> spans;
            while (matches.hasNext()) { auto m = matches.next(); spans.append({m.capturedStart(), m.capturedLength()}); }
            for (auto it = spans.crbegin(); it != spans.crend(); ++it)
                text.replace(it->first, it->second, item.value("term").toString());
        }
    }
    return text;
}
QString Terms::prompt() const {
    QStringList names;
    for (const auto &value : entries) names.append(value.toObject().value("term").toString());
    return names.join(", ");
}
