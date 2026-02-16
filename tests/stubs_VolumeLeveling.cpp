// Stubs for VolumeLevelingManager test — satisfies linker for unused singletons.
// These replace the real implementations so we don't pull in the full dependency tree.

#include "library/LibraryDatabase.h"
#include "dsp/LoudnessAnalyzer.h"

// ── LibraryDatabase stub ────────────────────────────────────────────
static LibraryDatabase* s_dbStub = nullptr;

LibraryDatabase* LibraryDatabase::instance()
{
    if (!s_dbStub) {
        s_dbStub = new LibraryDatabase();
    }
    return s_dbStub;
}

LibraryDatabase::LibraryDatabase(QObject* parent) : QObject(parent) {}
LibraryDatabase::~LibraryDatabase() {}

std::optional<Track> LibraryDatabase::trackByPath(const QString&) const
{
    return std::nullopt;
}

void LibraryDatabase::updateR128Loudness(const QString&, double, double) {}

// ── LoudnessAnalyzer stub ───────────────────────────────────────────
LoudnessResult LoudnessAnalyzer::analyze(const QString&)
{
    return {};
}
