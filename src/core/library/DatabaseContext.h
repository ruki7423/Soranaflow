#pragma once

#include <QSqlDatabase>
#include <QRecursiveMutex>
#include "../MusicData.h"

class QSqlQuery;

// Shared database infrastructure passed to all repository classes.
// Holds references to the read/write connections and mutexes
// owned by LibraryDatabase, plus shared helper methods.
struct DatabaseContext {
    QSqlDatabase* writeDb = nullptr;
    QSqlDatabase* readDb = nullptr;
    QRecursiveMutex* writeMutex = nullptr;
    QRecursiveMutex* readMutex = nullptr;

    // Shared helpers
    QString generateId() const;
    Track trackFromQuery(const QSqlQuery& query) const;
    AudioFormat audioFormatFromString(const QString& str) const;
    QString audioFormatToString(AudioFormat fmt) const;
};
