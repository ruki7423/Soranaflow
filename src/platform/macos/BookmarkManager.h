#pragma once

#include <QString>
#include <QStringList>

class BookmarkManager
{
public:
    static BookmarkManager* instance();

    // Save a security-scoped bookmark for a folder (call after user selects via NSOpenPanel/QFileDialog)
    bool saveBookmark(const QString& folderPath);

    // Restore all saved bookmarks on app launch — re-establishes sandbox access
    void restoreAllBookmarks();

    // Save bookmarks for any folders that don't have one yet (call after restore)
    void ensureBookmarks(const QStringList& folders);

    // Check if a bookmark exists for the given path
    bool hasBookmark(const QString& folderPath) const;

    // Remove a bookmark
    void removeBookmark(const QString& folderPath);

private:
    BookmarkManager();
    ~BookmarkManager();

    QString bookmarkKey(const QString& path) const;

    void* m_restoredURLs = nullptr;  // NSMutableArray<NSURL*>*
};
