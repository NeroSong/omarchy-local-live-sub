#pragma once
#include <QStringList>

class AgentContext {
public:
    static QString prompt(const QString &configDirectory, const QString &projectDirectory, const QString &language);
    static QStringList arguments(const QString &configDirectory, const QString &projectDirectory, const QString &language);
};
