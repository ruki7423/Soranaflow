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
