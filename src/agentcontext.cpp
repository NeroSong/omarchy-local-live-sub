#include "agentcontext.h"
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>

QString AgentContext::prompt(const QString &configDirectory, const QString &projectDirectory, const QString &language) {
    const bool chinese = language.startsWith("zh", Qt::CaseInsensitive);
    const auto instruction = chinese
        ? QString("当前应用的界面语言是简体中文（zh-CN）。请从第一条回复开始，用简体中文与我沟通词表配置，提问、解释、变更摘要和测试建议都使用简体中文。除非我明确要求切换语言，否则保持中文；不要因为下方技术说明使用英文而切换语言。文件名、代码和专有名词保持原样。\n\n")
        : QString("The application's current interface language is English (en). Use English from your first reply, including questions, explanations, change summaries and test suggestions. Keep using English unless I explicitly request another language. Preserve filenames, code and proper names.\n\n");
    const QJsonObject locations{{"interface_language", chinese ? "zh-CN" : "en"}, {"configuration_directory", configDirectory},
        {"glossary", QDir(configDirectory).filePath("glossary.json")},
        {"recognition_config", QDir(configDirectory).filePath("asr-config.json")},
        {"project_readme", QDir(projectDirectory).filePath("README.md")}};
    return instruction + QString::fromUtf8(R"(Help me tune the topic vocabulary for Omarchy Local Live Sub, a local Chinese-English caption and translation app.
Follow the conversation-language instruction above. First read the two configuration files and the vocabulary section of the project README at these exact locations (JSON data):
%1
Ask which topic we are testing, which words were misrecognized, the intended spelling/translation, and which ordinary expressions must remain unchanged. Do not modify configuration until I describe the desired adjustment.

There are three separate layers:
1. asr-config.json: hotwords influence SenseVoice CTC recognition. Each item is {"term":"Beelink","score":3}. Terms must be unique ASCII words of 3-32 letters, scores in (0,8], at most 128 entries. Digits, spaces and Chinese are unsupported here. Keep decoder, threads and search parameters unchanged unless explicitly requested.
2. glossary.json: a JSON array of entries with nonempty term and aliases array. Add confirmed recognition errors to aliases. Optional context_aliases/context constrain ambiguous matches; preserve protects ordinary words. Existing force_phonetic rules can overcorrect ordinary speech: keep come protected and discuss any changes to aggressive Omarchy matching explicitly.
3. translations: optional zh/en target-language mappings on a glossary entry, e.g. {"term":"Beelink","aliases":["零刻","bee link"],"translations":{"zh":"零刻","en":"Beelink"}}. These guide the translation model; test results still need human review.

Only edit these two configuration files for this task. Keep existing unrelated rules. Do not read caption sessions, recordings, logs, account files or environment secrets; I will supply relevant examples manually. Do not edit system audio/input settings, source code or models, install packages, restart services, or commit/push changes.
Before saving, show the focused changes and validate JSON, field types, duplicate names, and hotword limits. Preserve a backup and save each file atomically. Each file is limited to 256 KiB. Explain any potentially ambiguous ordinary-word substitutions and suggest positive and negative test sentences.
Saving valid configuration automatically affects subsequent tasks. Invalid configuration leaves the previous valid version active. Completed captions are not rewritten. No model restart is needed for vocabulary changes. The files load independently, so avoid relying on simultaneous updates. Summarize what changed and the examples I should speak to verify it.
)").arg(QString::fromUtf8(QJsonDocument(locations).toJson(QJsonDocument::Indented)));
}
QStringList AgentContext::arguments(const QString &configDirectory, const QString &projectDirectory, const QString &language) {
    // Pass paths and prompt as argv, including through the terminal launcher; never construct shell code.
    return {"launch", "tui", "--app-id=org.omarchy.agent", "env", "--chdir=" + configDirectory,
        "omarchy", "agent", "prompt", "--inline", prompt(configDirectory, projectDirectory, language)};
}
