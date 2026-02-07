#pragma once

#include <QString>
#include <QVector>
#include <QStringList>

struct OrganizeAction {
    QString sourcePath;
    QString destPath;
    bool isFolder = false;
};

class AutoOrganizer {
public:
    // Pattern tokens: %artist%, %album%, %title%, %track%, %year%, %genre%
    // Default: "%artist%/%album%/%track% - %title%"

    void setPattern(const QString& pattern) { m_pattern = pattern; }
    QString pattern() const { return m_pattern; }

    void setDestinationRoot(const QString& path) { m_destRoot = path; }
    QString destinationRoot() const { return m_destRoot; }

    QVector<OrganizeAction> preview(const QStringList& filePaths);
    bool execute(const QVector<OrganizeAction>& actions);
    bool undo();

    const QVector<OrganizeAction>& lastActions() const { return m_lastActions; }

private:
    QString m_pattern = QStringLiteral("%artist%/%album%/%track% - %title%");
    QString m_destRoot;
    QVector<OrganizeAction> m_lastActions;

    QString applyPattern(const QString& filePath);
    static QString sanitizeFilename(const QString& name);
};
