#pragma once
#include <QJsonObject>
#include <QSettings>
#include <QTimer>
#include <QTranslator>

class Translator : public QTranslator {
    Q_OBJECT
    Q_PROPERTY(QString selection READ selection NOTIFY changed)
    Q_PROPERTY(QString language READ language NOTIFY changed)
public:
    explicit Translator(const QString &settingsPath, const QString &languagePath = {},
                        const QString &catalogDirectory = ":/i18n", QObject *parent = nullptr);
    QString selection() const { return selected; }
    QString language() const { return effective; }
    bool isEmpty() const override { return false; }
    QString translate(const char *, const char *source, const char * = nullptr, int = -1) const override;
    Q_INVOKABLE void setSelection(const QString &selection);
signals:
    void changed();
private:
    void reload();
    QString selected, effective, languageFile, catalogDir;
    QSettings settings;
    QJsonObject catalog;
    QTimer timer;
};
