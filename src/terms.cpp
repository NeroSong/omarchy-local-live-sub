#include "terms.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>

namespace {
QString literalPattern(const QString &literal) {
    const auto words = literal.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    QStringList escaped;
    for (const auto &word : words) escaped.append(QRegularExpression::escape(word));
    // Han text often has no spaces around an English product name.
    return "(?<![\\p{Latin}\\p{N}_])" + escaped.join("\\s+") + "(?![\\p{Latin}\\p{N}_])";
}
bool nearSpelling(const QString &candidate, const QString &name) {
    const auto a = candidate.toLower(), b = name.toLower();
    if (a.size() < 6 || b.size() < 6 || std::abs(a.size() - b.size()) > 1 || a == b) return false;
    // One insertion, deletion, substitution, or adjacent transposition only.
    if (a.size() == b.size()) {
        QList<int> different;
        for (int i = 0; i < a.size(); ++i) if (a[i] != b[i]) different.append(i);
        return different.size() == 1 || (different.size() == 2 && different[1] == different[0] + 1
            && a[different[0]] == b[different[1]] && a[different[1]] == b[different[0]]);
    }
    const auto shorter = a.size() < b.size() ? a : b;
    const auto longer = a.size() < b.size() ? b : a;
    int i = 0, j = 0, skips = 0;
    while (i < shorter.size() && j < longer.size()) {
        if (shorter[i] == longer[j]) { ++i; ++j; }
        else { ++j; if (++skips > 1) return false; }
    }
    return true;
}
struct Match { int start, length; QString replacement, reason; int priority; };
}

bool Terms::load(const QString &path, QString &error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = "无法读取术语表：" + path; return false; }
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (!doc.isArray() || parse.error != QJsonParseError::NoError) {
        error = "术语表必须是 JSON 数组：" + parse.errorString(); return false;
    }
    const auto values = doc.array();
    for (const auto &value : values) {
        const auto item = value.toObject();
        if (item.value("term").toString().trimmed().isEmpty() || !item.value("aliases").isArray()) {
            error = "每个术语需要非空 term 和 aliases 数组"; return false;
        }
        for (const auto &field : {"aliases", "context_aliases", "context"}) {
            if (!item.contains(field)) continue;
            if (!item.value(field).isArray()) { error = QString(field) + " 必须是字符串数组"; return false; }
            for (const auto &alias : item.value(field).toArray())
                if (!alias.isString() || alias.toString().trimmed().isEmpty()) {
                    error = QString(field) + " 必须包含非空字符串"; return false;
                }
        }
        if (item.contains("fuzzy") && !item.value("fuzzy").isBool()) { error = "fuzzy 必须是布尔值"; return false; }
    }
    entries = values;
    error.clear();
    return true;
}

Terms::Result Terms::correct(const QString &text, const QString &context) const {
    Result result{text, {}};
    QList<Match> candidates;
    QList<QPair<int, int>> protectedSpans;
    // Preserve quoted code, URLs, email addresses, paths and shell flags verbatim.
    const QRegularExpression protectedPattern(
        "```[\\s\\S]*?(?:```|$)|`[^`\\n]*(?:`|$)|(?:https?://|www\\.)[^\\s]+|"
        "[\\w.+-]+@[\\w.-]+\\.[A-Za-z]+|(?<!\\w)(?:/|\\./|~/)[^\\s]+|(?<!\\w)--[A-Za-z][\\w-]*",
        QRegularExpression::CaseInsensitiveOption);
    auto protectedMatches = protectedPattern.globalMatch(text);
    while (protectedMatches.hasNext()) {
        const auto match = protectedMatches.next();
        protectedSpans.append({match.capturedStart(), match.capturedEnd()});
    }
    auto isProtected = [&](int start, int end) {
        for (const auto &span : protectedSpans) if (start < span.second && end > span.first) return true;
        return false;
    };
    const auto options = QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption;
    const QString surrounding = text + "\n" + context;
    for (const auto &value : entries) {
        const auto item = value.toObject();
        const auto name = item.value("term").toString();
        bool hasContext = false;
        auto contexts = item.value("context").toArray();
        contexts.append(name);
        for (const auto &word : contexts) {
            if (QRegularExpression(literalPattern(word.toString()), options).match(surrounding).hasMatch()) {
                hasContext = true; break;
            }
        }
        auto collect = [&](const QJsonArray &aliases, int priority, const QString &reason) {
            for (const auto &alias : aliases) {
                auto matches = QRegularExpression(literalPattern(alias.toString()), options).globalMatch(text);
                while (matches.hasNext()) {
                    const auto match = matches.next();
                    if (match.captured() != name && !isProtected(match.capturedStart(), match.capturedEnd()))
                        candidates.append({int(match.capturedStart()), int(match.capturedLength()), name, reason, priority});
                }
            }
        };
        auto aliases = item.value("aliases").toArray(); aliases.append(name);
        collect(aliases, 3, "alias");
        if (hasContext) collect(item.value("context_aliases").toArray(), 2, "context_alias");
        if (!hasContext || !item.value("fuzzy").toBool()) continue;
        const QRegularExpression wordPattern("(?<![\\p{Latin}\\p{N}_./:@-])[A-Za-z]{6,20}(?![\\p{Latin}\\p{N}_./:@-])", options);
        auto words = wordPattern.globalMatch(text);
        while (words.hasNext()) {
            const auto word = words.next();
            if (nearSpelling(word.captured(), name) && !isProtected(word.capturedStart(), word.capturedEnd()))
                candidates.append({int(word.capturedStart()), int(word.capturedLength()), name, "context_spelling", 1});
        }
    }
    // Resolve against the original text, so replacements cannot cascade into other rules.
    std::sort(candidates.begin(), candidates.end(), [](const Match &a, const Match &b) {
        if (a.priority != b.priority) return a.priority > b.priority;
        if (a.length != b.length) return a.length > b.length;
        return a.start < b.start;
    });
    QList<Match> accepted;
    for (const auto &candidate : candidates) {
        bool conflict = false;
        for (const auto &other : candidates)
            if (candidate.start == other.start && candidate.length == other.length
                && candidate.priority == other.priority && candidate.replacement != other.replacement) conflict = true;
        for (const auto &other : accepted)
            if (candidate.start < other.start + other.length && candidate.start + candidate.length > other.start) conflict = true;
        if (!conflict) accepted.append(candidate);
    }
    std::sort(accepted.begin(), accepted.end(), [](const Match &a, const Match &b) { return a.start < b.start; });
    for (const auto &match : accepted)
        result.corrections.append(QVariantMap{{"from", text.mid(match.start, match.length)}, {"to", match.replacement},
            {"reason", match.reason}, {"offset", match.start}});
    for (auto it = accepted.crbegin(); it != accepted.crend(); ++it)
        result.text.replace(it->start, it->length, it->replacement);
    return result;
}

QString Terms::normalize(QString text) const { return correct(text).text; }

QString Terms::prompt() const {
    QStringList names;
    for (const auto &value : entries) names.append(value.toObject().value("term").toString());
    return names.join(", ");
}
