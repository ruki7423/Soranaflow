# CLAUDE.md — Sorana Flow Prompt & Response Rules

## Project

- Path: `/Users/haruki/Documents/Sorana flow/qt-output`
- Build: `/Users/haruki/Documents/Sorana flow/qt-output/build`
- Stack: Qt/C++, FFmpeg, CoreAudio, MusicKit
- DSP chain: Source → Decode → Upsample → Headroom → Crossfeed → Convolution → EQ → Volume Leveling → Limiter → Output

## Prompt Format (English, plain text codeblock)

All prompts MUST be written as a single English plain text codeblock with the following sections in order:

```
⑯ Feature/Fix Name — Short description

Project: /Users/haruki/Documents/Sorana flow/qt-output
Build: /Users/haruki/Documents/Sorana flow/qt-output/build

================================================================================
GREP FIRST
================================================================================

grep commands to understand existing code before making changes.
Always grep for relevant symbols, signals, classes.

================================================================================
ANALYSIS (if debugging)
================================================================================

Stack trace, log analysis, root cause identification.

================================================================================
FIX / IMPLEMENTATION — File: path/to/file.h
================================================================================

Header changes first.

================================================================================
FIX / IMPLEMENTATION — File: path/to/file.cpp
================================================================================

Implementation changes.
Must include WRONG/CORRECT code comparison.
Must include A/B fallback options when root cause is uncertain.

================================================================================
qDebug VERIFICATION
================================================================================

Expected debug output after fix.
What logs should appear, what should NOT appear.

================================================================================
BUILD AND TEST
================================================================================

cd "/Users/haruki/Documents/Sorana flow/qt-output/build"
cmake .. && make -j$(sysctl -n hw.ncpu)

cd '/Users/haruki/Documents/Sorana flow/qt-output/build' && ./SoranaFlow.app/Contents/MacOS/SoranaFlow 2>&1 | tee /tmp/sorana-test.log

| # | Test | Expected | Pass |
|---|------|----------|------|
| 1 | ... | ... | |

grep commands to verify in log.

================================================================================
DO NOT
================================================================================

List of things that must NOT be changed or done.
```

## Mandatory Rules

### 1. English plain text codeblock
- ALL prompts in English inside a single ``` codeblock
- Conversations/responses in English

### 2. Rigorous + concise, no fluff
- No unnecessary explanations or filler text
- Get straight to the point
- Every line must be actionable

### 3. WRONG / CORRECT
- Always show the broken code (WRONG) vs fixed code (CORRECT)
- Side-by-side comparison so the difference is obvious

### 4. grep first
- Every prompt starts with grep commands
- Understand the existing code before proposing changes
- grep for relevant symbols, signal connections, class members

### 5. A/B fallbacks
- When root cause is uncertain, provide Option A (preferred) and Option B (fallback)
- Label clearly: "Option A (preferred)", "Option B (fallback)"
- Both must be complete and buildable

### 6. .h + .cpp
- ALWAYS specify changes for BOTH header and implementation files
- Even if header has no changes, state: "No changes needed — verify with grep"

### 7. qDebug
- Every fix must include qDebug() verification points
- Format: `[Component] Action: details`
- Example: `[Lyrics] Source: LRCLIB synced, 89 lines`
- State what logs SHOULD and SHOULD NOT appear

### 8. BUILD / TEST
- Always include exact build commands
- Always include test table with columns: #, Test, Expected, Pass
- Always include grep command to verify log output
- Test table must cover: happy path, edge cases, regression

### 9. DO NOT
- Every prompt ends with a DO NOT section
- List things that must not be changed (AudioEngine, DSP, etc.)
- List anti-patterns specific to the fix

### 10. English responses, table checklists
- All conversational responses in English
- Test results presented as table checklists
- Status updates in English with checkmarks

## Key Technical Principles

- Mid-playback application: ALL settings apply immediately via atomic variables
- Thread safety: Main thread sets atomic flags, render thread consumes
- No allocation in render path: All buffers pre-allocated
- Smooth transitions: Fade-in/out for enable/disable
- Drag-drop safety: Clear graphics effects BEFORE widget deletion
- Nested event loop awareness: drag->exec(), QDialog::exec() process deferred deletions
- Qt signal timing: abort() can fire finished() synchronously — null pointers BEFORE abort
- DSF/DSD safety: Skip FFmpeg stream access without avformat_find_stream_info
- Always bounds-check array indices in paint/animation callbacks

## Coding Rules

These rules are mandatory for ALL code changes. They prevent the classes of bugs
found in the v1.5.x audit (67 issues). Every rule maps to a real crash or bug.

### 1. Container Access — ALWAYS check before access

WRONG:
    auto item = array[0];
    auto first = list.first();
    auto front = vec.front();

CORRECT:
    if (array.isEmpty()) return;
    auto item = array[0];

Applies to: [0], .first(), .last(), .front(), .back(), .at(0)
No exceptions. Even if "it should never be empty" — check anyway.

### 2. Pointer Dereference — ALWAYS null-check

WRONG:
    m_view->update();
    m_decoder->read(buf, frames);

CORRECT:
    if (!m_view) return;
    m_view->update();

Applies to: every raw pointer dereference, especially:
- View pointers (m_albumsView, m_artistsView, etc.)
- DSP processor pointers (m_eq, m_gain, m_convolution, m_hrtf)
- Decoder pointers (m_decoder, m_nextDecoder)
- WebView pointers (m_wk)

### 3. Audio Realtime Thread — ZERO allocations, ZERO locks, ZERO I/O

The audio render callback runs on a realtime thread. These are FORBIDDEN inside it:

FORBIDDEN:
- new / delete / malloc / free
- std::vector::resize() / push_back() / append()
- QString (heap-allocates)
- qDebug() (uses QString internally)
- QMutex::lock() / std::lock_guard (use try_lock or lock-free)
- QFile / fopen / fread / any disk I/O
- QSettings
- QNetworkRequest

ALLOWED:
- memcpy, memset
- std::atomic reads/writes
- Pre-allocated buffer access
- Simple arithmetic
- try_lock (skip frame on failure, output silence)
- fprintf(stderr, ...) for emergency debug only

Pre-allocate all buffers in loadTrack() or init(). Never in the render path.

### 4. Thread Shared State — atomic or mutex, no exceptions

Any variable read by one thread and written by another MUST be:
- std::atomic<T> for simple types (bool, int, double, pointer)
- Protected by QMutex for complex types (QString, QHash, QList, QCache)
- Communicated via Qt::QueuedConnection signal/slot

WRONG:
    // Header:
    bool m_isPlaying;         // written by main, read by audio thread
    double m_duration;        // written by decoder, read by UI

CORRECT:
    std::atomic<bool> m_isPlaying{false};
    std::atomic<double> m_duration{0.0};

QCache, QHash, QSet, QList are NOT thread-safe. If shared, add QMutex.

### 5. File I/O — ALWAYS check open() return

WRONG:
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    auto data = file.readAll();

CORRECT:
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to open:" << path;
        return {};
    }
    auto data = file.readAll();

Applies to: QFile, std::ifstream, fopen, any file operation.

### 6. Database Operations — check results, use transactions for mutations

WRONG:
    db.exec("DELETE FROM tracks WHERE folder_id = ?");
    db.exec("ALTER TABLE tracks ADD COLUMN format TEXT");

CORRECT:
    db.transaction();
    auto q = db.exec("DELETE FROM tracks WHERE folder_id = ?");
    if (q.lastError().isValid()) {
        qWarning() << "[DB]" << q.lastError().text();
        db.rollback();
        return;
    }
    db.commit();

Single SELECTs don't need transactions. INSERT/UPDATE/DELETE batches do.

### 7. Shutdown / Cleanup — RAII, release everything

Every resource acquired MUST be released in the destructor or cleanup:
- Security-scoped URLs → stopAccessingSecurityScopedResource
- WKWebView → removeFromSuperview + stopLoading before delete
- Threads → quit() + wait(5000) before delete
- Timers → invalidate before freeing resources they reference
- Mutexes → never hold during destructor of the protected object
- Audio callbacks → fully stopped before resetting decoder/buffers

Order: stop callbacks → wait for completion → release resources.

WRONG:
    ~MyClass() {
        m_decoder.reset();    // callback may still be using this!
    }

CORRECT:
    ~MyClass() {
        stop();               // signal callback to stop
        m_mutex.lock();       // wait for callback to finish
        m_decoder.reset();    // now safe
        m_mutex.unlock();
    }

### 8. Division — ALWAYS check denominator

WRONG:
    float result = total / count;

CORRECT:
    if (count <= 0) return 0.0f;
    float result = total / count;

### 9. macOS API Availability — guard version-specific APIs

WRONG:
    AudioHardwareDestroyProcessTap(tapID);  // macOS 14.2+ only

CORRECT:
    if (@available(macOS 14.2, *)) {
        AudioHardwareDestroyProcessTap(tapID);
    }

### 10. DSP Parameter Validation — clamp inputs

WRONG:
    setBiquadCoeffs(frequency, gainDb, q);

CORRECT:
    gainDb = qBound(-30.0, gainDb, 30.0);
    frequency = qBound(20.0, frequency, sampleRate * 0.49);
    if (sampleRate <= 0) return;
    setBiquadCoeffs(frequency, gainDb, q);

### 11. Network — set timeouts

WRONG:
    m_nam->get(request);  // may hang forever

CORRECT:
    m_nam->setTransferTimeout(15000);  // 15 seconds
    m_nam->get(request);

### 12. QSettings — never in hot paths

WRONG:
    // In sectionResized signal (fires per pixel):
    connect(header, &QHeaderView::sectionResized, [this]() {
        settings.setValue("width", header->saveState());  // disk I/O per pixel
    });

CORRECT:
    // Debounce:
    connect(header, &QHeaderView::sectionResized, [this]() {
        m_saveTimer->start();  // 300ms debounce, saves once after drag
    });
