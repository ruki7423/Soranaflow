#include "BookmarkManager.h"
#include "../../core/Settings.h"
#include <QSettings>
#include <QDebug>

#import <Foundation/Foundation.h>

static BookmarkManager* s_instance = nullptr;

BookmarkManager* BookmarkManager::instance()
{
    if (!s_instance)
        s_instance = new BookmarkManager();
    return s_instance;
}

BookmarkManager::BookmarkManager() {}
BookmarkManager::~BookmarkManager() {}

QString BookmarkManager::bookmarkKey(const QString& path) const
{
    return QStringLiteral("SecurityBookmarks/") + QString::fromUtf8(path.toUtf8().toBase64());
}

bool BookmarkManager::saveBookmark(const QString& folderPath)
{
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:folderPath.toNSString()];
        if (!url) return false;

        NSError* error = nil;
        NSData* bookmarkData = [url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                             includingResourceValuesForKeys:nil
                                              relativeToURL:nil
                                                      error:&error];

        if (error || !bookmarkData) {
            qWarning() << "BookmarkManager: Failed to create bookmark for" << folderPath
                       << (error ? QString::fromNSString(error.localizedDescription) : QString());
            return false;
        }

        QByteArray data = QByteArray::fromNSData(bookmarkData);

        QSettings settings(Settings::settingsPath(), QSettings::IniFormat);
        settings.setValue(bookmarkKey(folderPath), data);
        settings.sync();  // Force write to disk immediately

        qDebug() << "BookmarkManager: Saved bookmark for" << folderPath
                 << "(" << data.size() << "bytes)";
        return true;
    }
}

void BookmarkManager::restoreAllBookmarks()
{
    @autoreleasepool {
        QSettings settings(Settings::settingsPath(), QSettings::IniFormat);
        settings.beginGroup(QStringLiteral("SecurityBookmarks"));
        const QStringList keys = settings.childKeys();
        settings.endGroup();

        int restored = 0;

        for (const QString& key : keys) {
            QByteArray data = settings.value(QStringLiteral("SecurityBookmarks/") + key).toByteArray();
            if (data.isEmpty()) continue;

            NSData* bookmarkData = data.toNSData();

            BOOL isStale = NO;
            NSError* error = nil;
            NSURL* url = [NSURL URLByResolvingBookmarkData:bookmarkData
                                                   options:NSURLBookmarkResolutionWithSecurityScope
                                             relativeToURL:nil
                                       bookmarkDataIsStale:&isStale
                                                     error:&error];

            if (error || !url) {
                qWarning() << "BookmarkManager: Failed to resolve bookmark for key" << key;
                continue;
            }

            if ([url startAccessingSecurityScopedResource]) {
                restored++;
                qDebug() << "BookmarkManager: Restored access to" << QString::fromNSString(url.path);

                // Re-save if the bookmark data became stale
                if (isStale) {
                    saveBookmark(QString::fromNSString(url.path));
                }
            } else {
                qWarning() << "BookmarkManager: startAccessingSecurityScopedResource failed for"
                           << QString::fromNSString(url.path);
            }
        }

        qDebug() << "BookmarkManager: Restored" << restored << "of" << keys.size() << "bookmarks";
    }
}

bool BookmarkManager::hasBookmark(const QString& folderPath) const
{
    QSettings settings(Settings::settingsPath(), QSettings::IniFormat);
    return settings.contains(bookmarkKey(folderPath));
}

void BookmarkManager::removeBookmark(const QString& folderPath)
{
    QSettings settings(Settings::settingsPath(), QSettings::IniFormat);
    settings.remove(bookmarkKey(folderPath));
    qDebug() << "BookmarkManager: Removed bookmark for" << folderPath;
}

void BookmarkManager::ensureBookmarks(const QStringList& folders)
{
    int saved = 0;
    for (const QString& folder : folders) {
        if (!hasBookmark(folder)) {
            if (saveBookmark(folder)) {
                saved++;
            }
        }
    }
    if (saved > 0) {
        qDebug() << "BookmarkManager: Saved" << saved << "new bookmarks for existing folders";
    }
}
