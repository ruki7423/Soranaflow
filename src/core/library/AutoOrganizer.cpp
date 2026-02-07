#include "AutoOrganizer.h"
#include "../audio/TagWriter.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDebug>

// ═══════════════════════════════════════════════════════════════════════
//  preview
// ═══════════════════════════════════════════════════════════════════════

QVector<OrganizeAction> AutoOrganizer::preview(const QStringList& filePaths)
{
    QVector<OrganizeAction> actions;

    for (const QString& path : filePaths) {
        QString destRelative = applyPattern(path);
        if (destRelative.isEmpty())
            continue;

        QFileInfo fi(path);
        QString destFull = m_destRoot + QStringLiteral("/") + destRelative + QStringLiteral(".") + fi.suffix();

        if (destFull == path)
            continue; // Already in the right place

        OrganizeAction action;
        action.sourcePath = path;
        action.destPath = destFull;
        actions.append(action);
    }

    return actions;
}

// ═══════════════════════════════════════════════════════════════════════
//  execute
// ═══════════════════════════════════════════════════════════════════════

bool AutoOrganizer::execute(const QVector<OrganizeAction>& actions)
{
    m_lastActions.clear();
    int successCount = 0;

    for (const auto& action : actions) {
        QFileInfo destInfo(action.destPath);
        QDir destDir = destInfo.absoluteDir();

        // Create destination directory if needed
        if (!destDir.exists()) {
            if (!destDir.mkpath(QStringLiteral("."))) {
                qWarning() << "AutoOrganizer: Failed to create directory" << destDir.absolutePath();
                continue;
            }
        }

        // Move the file
        if (QFile::rename(action.sourcePath, action.destPath)) {
            m_lastActions.append(action);
            successCount++;
            qDebug() << "AutoOrganizer: Moved" << action.sourcePath << "->" << action.destPath;
        } else {
            qWarning() << "AutoOrganizer: Failed to move" << action.sourcePath;
        }
    }

    qDebug() << "AutoOrganizer: Moved" << successCount << "of" << actions.size() << "files";
    return successCount == actions.size();
}

// ═══════════════════════════════════════════════════════════════════════
//  undo
// ═══════════════════════════════════════════════════════════════════════

bool AutoOrganizer::undo()
{
    if (m_lastActions.isEmpty())
        return false;

    int successCount = 0;

    // Undo in reverse order
    for (int i = m_lastActions.size() - 1; i >= 0; --i) {
        const auto& action = m_lastActions[i];

        // Ensure source directory exists
        QFileInfo srcInfo(action.sourcePath);
        QDir srcDir = srcInfo.absoluteDir();
        if (!srcDir.exists()) {
            srcDir.mkpath(QStringLiteral("."));
        }

        if (QFile::rename(action.destPath, action.sourcePath)) {
            successCount++;
        } else {
            qWarning() << "AutoOrganizer: Failed to undo move for" << action.destPath;
        }
    }

    qDebug() << "AutoOrganizer: Undid" << successCount << "of" << m_lastActions.size() << "moves";
    m_lastActions.clear();
    return successCount > 0;
}

// ═══════════════════════════════════════════════════════════════════════
//  applyPattern
// ═══════════════════════════════════════════════════════════════════════

QString AutoOrganizer::applyPattern(const QString& filePath)
{
    TrackMetadata meta;
    if (!TagWriter::readTags(filePath, meta))
        return {};

    QString result = m_pattern;

    // Replace tokens
    result.replace(QStringLiteral("%artist%"),
                   sanitizeFilename(meta.artist.isEmpty() ? QStringLiteral("Unknown Artist") : meta.artist));
    result.replace(QStringLiteral("%album%"),
                   sanitizeFilename(meta.album.isEmpty() ? QStringLiteral("Unknown Album") : meta.album));
    result.replace(QStringLiteral("%title%"),
                   sanitizeFilename(meta.title.isEmpty() ? QFileInfo(filePath).completeBaseName() : meta.title));
    result.replace(QStringLiteral("%track%"),
                   meta.trackNumber > 0 ? QString::number(meta.trackNumber).rightJustified(2, '0') : QStringLiteral("00"));
    result.replace(QStringLiteral("%year%"),
                   meta.year > 0 ? QString::number(meta.year) : QStringLiteral("0000"));
    result.replace(QStringLiteral("%genre%"),
                   sanitizeFilename(meta.genre.isEmpty() ? QStringLiteral("Unknown") : meta.genre));

    return result;
}

// ═══════════════════════════════════════════════════════════════════════
//  sanitizeFilename
// ═══════════════════════════════════════════════════════════════════════

QString AutoOrganizer::sanitizeFilename(const QString& name)
{
    QString result = name;
    // Remove characters not allowed in filenames
    static const QRegularExpression invalidChars(QStringLiteral("[<>:\"/\\\\|?*]"));
    result.replace(invalidChars, QStringLiteral("_"));
    // Trim dots and spaces from ends
    result = result.trimmed();
    while (result.endsWith('.'))
        result.chop(1);
    return result.isEmpty() ? QStringLiteral("_") : result;
}
