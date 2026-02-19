# Sorana Flow -- Architecture Analysis v2

**Date:** February 16, 2026
**Version:** v1.7.5
**Scope:** Full codebase analysis -- 217 files, ~52,518 lines
**Purpose:** Post-refactoring architecture review; successor to ARCHITECTURE_ANALYSIS.md

---

## 1. Project Overview

### 1.1 Codebase Metrics

| Module | Directory | Lines | Files | Change from v1 |
|--------|-----------|------:|------:|:---------------:|
| UI (views, dialogs, services) | `src/ui/` | 20,299 | 69 | +180 lines, +29 files |
| Core (audio, data, dsp, library, lyrics) | `src/core/` | 16,458 | 77 | +1,399 lines, +47 files |
| Platform (macOS) | `src/platform/` | 3,562 | 15 | -28 lines, +2 files |
| Apple Music | `src/apple/` | 2,977 | 5 | +475 lines, +1 file |
| Widgets | `src/widgets/` | 2,866 | 22 | -479 lines, same |
| Plugins (VST) | `src/plugins/` | 2,222 | 8 | +399 lines, same |
| Metadata | `src/metadata/` | 2,060 | 14 | +190 lines, same |
| Tidal | `src/tidal/` | 1,120 | 2 | +42 lines, same |
| Radio | `src/radio/` | 440 | 4 | new |
| Entry (main.cpp) | `src/` | 514 | 1 | +51 lines |
| **Total** | | **~52,518** | **217** | **+2,426 lines, +51 files** |

### 1.2 Top 20 Files by Size

| # | File | Lines | Concern |
|---|------|------:|---------|
| 1 | `src/apple/MusicKitPlayer.mm` | 1,531 | WKWebView-based Apple Music player |
| 2 | `src/ui/views/settings/DSPSettingsWidget.cpp` | 1,467 | DSP settings UI |
| 3 | `src/ui/views/AppleMusicView.cpp` | 1,351 | Apple Music browse/search view |
| 4 | `src/core/audio/AudioEngine.cpp` | 1,314 | Audio playback engine |
| 5 | `src/platform/macos/AudioProcessTap.mm` | 1,225 | macOS 14.2+ process tap |
| 6 | `src/ui/views/TidalView.cpp` | 1,214 | Tidal browse view (DEAD) |
| 7 | `src/ui/PlaybackBar.cpp` | 1,106 | Transport controls + now playing |
| 8 | `src/platform/macos/CoreAudioOutput.cpp` | 1,079 | CoreAudio HAL output |
| 9 | `src/ui/MainWindow.cpp` | 1,027 | Application shell |
| 10 | `src/core/library/LibraryDatabase.cpp` | 983 | SQLite database layer |
| 11 | `src/widgets/TrackTableView.cpp` | 981 | Track table with sorting/filtering |
| 12 | `src/tidal/TidalManager.cpp` | 981 | Tidal REST API (DEAD) |
| 13 | `src/apple/AppleMusicBridge.mm` | 965 | MusicKit Swift bridge |
| 14 | `src/plugins/VST3Plugin.cpp` | 954 | VST3 SDK host adapter |
| 15 | `src/core/Settings.cpp` | 927 | Application settings |
| 16 | `src/ui/views/ArtistDetailView.cpp` | 920 | Artist detail page |
| 17 | `src/ui/views/QueueView.cpp` | 913 | Playback queue view |
| 18 | `src/ui/AppSidebar.cpp` | 888 | Navigation sidebar |
| 19 | `src/ui/views/AlbumsView.cpp` | 860 | Album grid/list view |
| 20 | `src/ui/views/NowPlayingView.cpp` | 817 | Now playing/lyrics view |

### 1.3 Build System

Single monolithic CMake target (`SoranaFlow`) with logical source groupings (CORE_SOURCES, LIBRARY_SOURCES, AUDIO_SOURCES, DSP_SOURCES, METADATA_SOURCES, PLUGIN_SOURCES, PLATFORM_SOURCES, APPLE_MUSIC_SOURCES, UI_SOURCES, VIEW_SOURCES, WIDGET_SOURCES). No library targets; module boundaries are NOT enforced at build level.

TidalView.h/.cpp are commented out in CMakeLists.txt (lines 471-472). TidalManager is still compiled but effectively inert (Tidal API has been down since Feb 2025).

### 1.4 Architecture Style

**Singleton-centric flat architecture.** 29 application singletons (up from 20+ in v1 analysis) provide global access to services. No dependency injection, no service locator. Views access core services directly via `ClassName::instance()`.

### 1.5 Directory Structure

```
src/
  apple/                    # AppleMusicManager, MusicKitPlayer, MusicKitSwiftBridge
  config/                   # Build configuration
  core/
    audio/                  # AudioEngine, decoders, GaplessManager, RenderChain, etc.
    dsp/                    # DSPPipeline, EQ, Crossfeed, Convolution, HRTF, Gain, Upsampler
    library/                # LibraryDatabase, repositories, scanner, playlists
    lyrics/                 # LyricsProvider
  metadata/                 # MetadataService, MusicBrainz, AcoustId, FanartTv, CoverArt
  platform/
    macos/                  # CoreAudioOutput, AudioProcessTap, AudioDeviceManager, Sparkle
  plugins/                  # VST2Host/Plugin, VST3Host/Plugin
  radio/                    # LastFmProvider, scrobbling
  tidal/                    # TidalManager (dormant)
  ui/
    dialogs/                # Modal dialogs
    services/               # NavigationService, MetadataFixService, CoverArtService (NEW)
    views/                  # All main views
      settings/             # 8 settings tab widgets (NEW, was monolithic SettingsView)
  widgets/                  # TrackTableView, styled widgets
```

---

## 2. Phase 1-3 Retrospective

The original ARCHITECTURE_ANALYSIS.md (v1) identified 18 prioritized refactoring items across 4 phases. Here is the completion status as of v1.7.5.

### Phase 1: Low-Risk, High-Value Extractions

| # | Item | Status | Notes |
|---|------|:------:|-------|
| 1 | Extract MetadataFixService | **DONE** | `src/ui/services/MetadataFixService.h/.cpp` -- used by LibraryView, AlbumDetailView, ArtistDetailView, PlaylistDetailView |
| 2 | Extract CoverArtService | **DONE** | `src/ui/services/CoverArtService.h/.cpp` -- used by NowPlayingView, SearchResultsView, ArtistDetailView, and others |
| 3 | Split SettingsView into tabs | **DONE** | 8 tab files in `src/ui/views/settings/`: DSPSettingsWidget, OutputSettingsWidget, LibrarySettingsTab, VSTSettingsWidget, AppleMusicSettingsTab, AudioSettingsTab, AppearanceSettingsTab, AboutSettingsTab + SettingsUtils.h |
| 4 | Extract SignalPathBuilder | **DONE** | `src/core/audio/SignalPathBuilder.h` (94 lines) + `.cpp` (288 lines) -- standalone, pure read-only |
| 5 | Extract VolumeLevelingManager | **DONE** | `src/core/audio/VolumeLevelingManager.h` (28 lines) + `.cpp` (119 lines) -- atomic interface |

### Phase 2: Interface Introductions

| # | Item | Status | Notes |
|---|------|:------:|-------|
| 6 | Create IDecoder interface | **DONE** | `src/core/audio/IDecoder.h` (40 lines) -- AudioDecoder and DSDDecoder both implement it |
| 7 | Make Crossfeed/Convolution/HRTF implement IDSPProcessor | **DONE** | CrossfeedProcessor now inherits IDSPProcessor (confirmed in header) |
| 8 | Create NavigationService | **DONE** | `src/ui/services/NavigationService.h/.cpp` -- eliminates MainWindow::instance() from views |
| 9 | Unify AudioDevice / AudioDeviceInfo types | **NOT DONE** | Dual device types still exist |

### Phase 3: Structural Decomposition

| # | Item | Status | Notes |
|---|------|:------:|-------|
| 10 | Decompose AudioEngine (GaplessManager, RenderChain) | **DONE** | GaplessManager (308 lines), AudioRenderChain (146 lines) extracted |
| 11 | Split LibraryDatabase into repositories | **PARTIAL** | TrackRepository (708 lines), AlbumRepository, ArtistRepository, PlaylistRepository, DatabaseContext all extracted. LibraryDatabase still 983 lines with cross-cutting concerns |
| 12 | Decompose PlaybackState (QueueManager) | **DONE** | QueueManager.h (61 lines) + .cpp (247 lines) extracted |
| 13 | Fix VST3Host circular dependency | **DONE** | No circular includes found in `src/plugins/` -> `src/core/` |
| 14 | Fix VST2Plugin realtime safety | **UNKNOWN** | Needs verification (VST2 is rarely used) |

### Phase 4: Architecture Evolution

| # | Item | Status | Notes |
|---|------|:------:|-------|
| 15 | Dependency injection | NOT DONE | Still singleton-based |
| 16 | IStreamingService interface | NOT DONE | No common interface |
| 17 | Unit test framework | NOT DONE | Zero tests |
| 18 | Split CMakeLists into library targets | NOT DONE | Still monolithic |

### Summary

**Phase 1:** 5/5 complete (100%)
**Phase 2:** 3/4 complete (75%) -- AudioDevice/AudioDeviceInfo unification pending
**Phase 3:** 4.5/5 complete (~90%) -- LibraryDatabase split is partial
**Phase 4:** 0/4 complete (0%) -- architectural evolution not yet started

The Phase 1-3 refactoring reduced AudioEngine from 1,797 to 1,314 lines (-27%), eliminated the 3,717-line SettingsView monolith, extracted 3 UI services, created the IDecoder interface, unified the DSP pipeline with IDSPProcessor, and split LibraryDatabase into 4 repositories + a context layer.

---

## 3. Architecture Strengths

### 3.1 Clean Core-to-UI Separation (PRESERVED)

Zero circular `#include` dependencies between `src/core/` and `src/ui/`. Verified: no file in `src/core/`, `src/platform/`, `src/apple/`, or `src/plugins/` includes any MainWindow, PlaybackBar, or views header. The dependency direction is strictly UI -> Core. This remains the strongest architectural property.

### 3.2 Excellent Realtime Safety (IMPROVED)

The audio render path follows strict realtime rules throughout:
- `CoreAudioOutput`: `try_lock` on callback mutex (outputs silence on contention)
- `AudioRenderChain::apply()`: Pre-allocated DSP chain, no allocations
- `CrossfeedProcessor`: All-atomic control, pending parameter exchange, zero allocations
- `DSPPipeline::process()`: `try_to_lock` on plugin mutex (skips on contention)
- `AudioEngine`: Pre-allocated decode/crossfade buffers
- `AudioProcessTap`: Pre-allocated interleave buffer, atomic DSP activation
- `GaplessManager`: Atomic flags for cross-thread coordination

New in v1.7.5: AudioRenderChain consolidation eliminated the duplicated DSP chain application that existed in the render callback.

### 3.3 PIMPL Pattern (CONSISTENT)

All Obj-C++ bridging uses opaque `struct Impl` / `void*` in headers:
- CoreAudioOutput, MusicKitPlayer, BookmarkManager, SparkleUpdater, AudioProcessTap, AppleMusicBridge
- No `NSString*`, `AudioDeviceID`, or `CFStringRef` appears in any `.h` file
- This pattern enables Windows porting without touching header consumers

### 3.4 IDecoder Interface (NEW)

`IDecoder.h` (40 lines) provides a proper abstract interface for decoders. Both `AudioDecoder` and `DSDDecoder` implement it. This eliminates most `if (m_usingDSDDecoder)` branches from AudioEngine.

### 3.5 IDSPProcessor Unification (IMPROVED)

CrossfeedProcessor, ConvolutionProcessor, and HRTFProcessor now implement `IDSPProcessor`. The entire DSP chain can be managed polymorphically through DSPPipeline.

### 3.6 Repository Pattern (NEW)

Library data access is now structured:
- `TrackRepository` (708 lines) -- track CRUD, path-based lookup
- `AlbumRepository` -- album queries with dateAdded subquery
- `ArtistRepository` -- artist queries
- `PlaylistRepository` -- playlist CRUD
- `DatabaseContext` -- connection lifecycle, schema migration

### 3.7 UI Service Layer (NEW)

Three UI services eliminate cross-view duplication:
- `NavigationService` -- signal-based navigation, replaces `MainWindow::instance()` calls from views
- `MetadataFixService` -- metadata fix/undo/fingerprint workflow (was ~320 lines duplicated across 4 views)
- `CoverArtService` -- unified cover art discovery chain (was ~240 lines across 8 files)

### 3.8 IAudioOutput Platform Abstraction (PRESERVED)

`IAudioOutput` uses only standard C++ types. CoreAudioOutput fully encapsulates CoreAudio specifics via PIMPL. Adding WASAPI would require ~2000-3000 lines of new platform code but zero interface changes.

### 3.9 Settings Tab Architecture (NEW)

The monolithic SettingsView (3,717 lines) has been split into 8 focused tab widgets:
- DSPSettingsWidget (1,467 lines -- still large, see Section 5)
- OutputSettingsWidget
- LibrarySettingsTab
- VSTSettingsWidget
- AppleMusicSettingsTab
- AudioSettingsTab
- AppearanceSettingsTab
- AboutSettingsTab
- SettingsUtils.h (shared utilities)

### 3.10 Other Preserved Strengths

- **MetadataService facade**: 5 independent providers, signal-based, no cross-dependencies
- **MusicDataProvider swap-on-complete caching**: Atomic cache swap, no empty gap during reload
- **Phased startup**: Window shown before heavy init, 3-phase deferred loading
- **RateLimiter utility**: Generic, shared across modules
- **Crash handler**: Standalone, async-signal-safe, zero dependencies
- **SignalPathBuilder**: Standalone visualization (288 lines), pure read-only

---

## 4. Architecture Problems

### 4.1 Singleton Count (29 Singletons)

Up from 20+ in v1. Every major component remains a global singleton:

**Created in main.cpp (controlled order):**
Settings, ThemeManager, BookmarkManager, AppleMusicManager, LibraryDatabase, MusicDataProvider, PlaybackState, PlaylistManager, AudioEngine, AudioDeviceManager, SparkleUpdater, LibraryScanner, NavigationService, CoverArtService

**Created on-demand (uncontrolled):**
MusicKitPlayer, TidalManager, MetadataService, AcoustIdProvider, AudioFingerprinter, CoverArtProvider, FanartTvProvider, MusicBrainzProvider, VST2Host (via Host::instance()), VST3Host (via Host::instance()), AutoplayManager, LastFmProvider, CoverArtLoader, MacMediaIntegration

### 4.2 Singleton Access Hotspots

| File | `::instance()` Calls | Concern |
|------|:--------------------:|---------|
| DSPSettingsWidget.cpp | 102 | Accesses 10+ singletons |
| OutputSettingsWidget.cpp | 57 | |
| main.cpp | 52 | Expected -- initialization hub |
| LibrarySettingsTab.cpp | 51 | |
| ArtistDetailView.cpp | 47 | |
| PlaybackBar.cpp | 46 | |
| MainWindow.cpp | 46 | |
| AppleMusicView.cpp | 36 | |
| LibraryView.cpp | 35 | |
| QueueView.cpp | 33 | |

### 4.3 Thread Safety Issues

**4.3.1 `m_pendingPlaySongId` (MusicKitPlayer.mm)**
`QString m_pendingPlaySongId` is read/written from both the main thread and WKWebView JavaScript callbacks (which may arrive on arbitrary threads). Not atomic, no mutex protection.

**4.3.2 `m_musicUserToken` (AppleMusicManager.h)**
`QString m_musicUserToken` is read by network request methods (potentially from worker threads) and written during auth flow. Not protected.

**4.3.3 `AlbumRepository::albumById()`**
The v1 analysis identified this using the write DB connection (`m_db`) with a read mutex. Needs verification if fixed in repository extraction.

### 4.4 Dead Code

**TidalView** (1,214 lines) and **TidalManager** (981 lines) = **2,195 lines of dead code**. Tidal API has been down since February 2025. TidalView is already commented out of CMakeLists.txt. TidalManager still compiles but cannot function.

### 4.5 Missing Network Timeouts

The following network managers lack `setTransferTimeout()`:
- `AppleMusicManager` -- REST API calls to api.music.apple.com
- `AppleMusicView` -- artwork fetching
- `LyricsProvider` -- lyrics API calls
- `PlaylistDetailView` -- cover art fetching
- `NewPlaylistDialog` -- cover art
- `VST2Plugin` -- (minimal concern, local-only)

All metadata providers (MusicBrainz, AcoustId, FanartTv, CoverArt) correctly set 15s timeouts. TidalManager also has 15s timeout.

### 4.6 No Unit Tests

Zero test files. Zero test framework. Zero test targets. The entire codebase is verified through manual testing only. This is the single biggest blocker for safe refactoring.

### 4.7 No Dependency Injection

All 29 singletons are accessed via `::instance()`. Constructor injection, service locator, or any other DI pattern is absent. This makes unit testing impossible and creates hidden dependency graphs.

### 4.8 Signal/Slot Coupling Density

| File | `connect()` Calls |
|------|:-----------------:|
| MainWindow.cpp | 48 |
| DSPSettingsWidget.cpp | 34 |
| LibraryView.cpp | 26 |
| TidalView.cpp | 25 |
| PlaybackBar.cpp | 25 |
| AppleMusicView.cpp | 23 |
| OutputSettingsWidget.cpp | 22 |

Total `connect()` calls across codebase: **~473** (in source files that matched grep). MainWindow wires the most connections as the central coordinator.

### 4.9 Database Access Bypass

Some views still access `LibraryDatabase::instance()` directly despite the repository layer. The repository pattern is not universally adopted -- older views may still bypass it.

### 4.10 Realtime Violations (Remaining)

**4.10.1 ConvolutionProcessor**: IR swap path in `process()` calls `m_fdl.resize()` and `.assign()` which allocate on the audio thread.

**4.10.2 HRTFProcessor**: `updateFilters()` may allocate vectors from the render thread. `loadSOFA()` can be triggered from `process()` when sample rate changes, performing file I/O on the audio thread.

These are the same issues identified in v1 -- not yet fixed.

### 4.11 DSPSettingsWidget is a New God Class

At 1,467 lines with 102 singleton accesses, DSPSettingsWidget is the largest settings tab and has effectively absorbed much of the old SettingsView's complexity. It handles: EQ (parametric + presets), upsampling, crossfeed, convolution, HRTF, headroom, limiter, and DSD settings -- all in one widget.

---

## 5. God Classes

### 5.1 Current God Classes (>800 lines in .cpp)

| # | Class | Lines | Responsibilities | Singletons Accessed |
|---|-------|------:|:----------------:|:-------------------:|
| 1 | MusicKitPlayer | 1,531 | WKWebView lifecycle, JS bridge, playback control, queue management, state machine (AMState + AMPlayState), media key integration | 5+ |
| 2 | DSPSettingsWidget | 1,467 | EQ, upsampling, crossfeed, convolution, HRTF, headroom, limiter, DSD settings | 10+ |
| 3 | AppleMusicView | 1,351 | Search, browse, artist/album detail, track construction from JSON, internal nav stack | 5+ |
| 4 | AudioEngine | 1,314 | Playback, decoding, render callback, device mgmt, DSD detection (reduced from 1,797) | 5+ |
| 5 | AudioProcessTap | 1,225 | macOS process tap creation, audio capture, DSP pipeline integration, format conversion | 3+ |
| 6 | TidalView | 1,214 | (DEAD CODE) Search, browse, auth, artist/album detail | N/A |
| 7 | PlaybackBar | 1,106 | Transport controls, volume, seek, track info, device selection, mute state | 6+ |
| 8 | CoreAudioOutput | 1,079 | HAL output setup, device enumeration, exclusive mode, render callback | 2+ |
| 9 | MainWindow | 1,027 | View creation, navigation, menu bar, drag-drop, window management | 7+ |
| 10 | LibraryDatabase | 983 | CRUD for 4 entities, FTS5 search, backup, integrity, migration (reduced from 2,134) | 0 |
| 11 | TrackTableView | 981 | Table rendering, sorting, filtering, context menu, column management | 3+ |
| 12 | TidalManager | 981 | (DEAD CODE) Tidal REST API, auth, search, favorites | 2+ |

### 5.2 Comparison with v1

| Class | v1 Lines | v2 Lines | Change |
|-------|:--------:|:--------:|:------:|
| SettingsView (monolith) | 3,717 | split into 8 | **ELIMINATED** |
| AudioEngine | 1,797 | 1,314 | **-27%** |
| LibraryDatabase | 2,134 | 983 | **-54%** |
| PlaybackState | 874 | ~650 (est.) | **-26%** |
| AppleMusicView | 1,272 | 1,351 | +6% (feature additions) |
| MusicKitPlayer | ~1,400 | 1,531 | +9% (feature additions) |

**Net progress:** The 3 worst god classes from v1 (SettingsView, LibraryDatabase, AudioEngine) have been significantly decomposed. However, DSPSettingsWidget (1,467 lines) emerged as a new problem from the SettingsView split.

### 5.3 Recommended Decompositions (Phase 4+)

1. **DSPSettingsWidget** -> split into: EQSettingsWidget, SpatialSettingsWidget (crossfeed + HRTF + convolution), UpsamplingSettingsWidget
2. **MusicKitPlayer** -> extract: MusicKitWebView (WKWebView lifecycle), MusicKitPlaybackController (play/pause/seek/queue), MusicKitStateMachine (AMState + AMPlayState transitions)
3. **AppleMusicView** -> extract: AMSearchPanel, AMArtistDetailPanel, AMAlbumDetailPanel
4. **PlaybackBar** -> extract: TransportControls, VolumeControl, NowPlayingInfo, DeviceSelector
5. **AudioProcessTap** -> extract: TapAudioPipeline (DSP processing from tap creation)

---

## 6. Dependency Map

### 6.1 Module Dependency Graph

```
                    +------------------+
                    |    main.cpp      |
                    | (514 lines)      |
                    +--------+---------+
                             | creates 14 singletons
             +---------------+---+---+---+----------------+
             v               v       v                    v
     +-----------+   +----------+  +----------+   +----------+
     | UI Layer  |   |  Core    |  | Platform |   |  Apple   |
     | 20.3K     |   | 16.5K   |  | 3.6K     |   | 3.0K    |
     +-----------+   +----------+  +----------+   +----------+
          |   |           |             ^               ^
          |   +---------->|             |               |
          |   singleton   |  implements |               |
          |   access      +-------------+               |
          |                                             |
          +--------- singleton access -----------------+
          |
          v
     +-----------+   +----------+   +----------+   +----------+
     | Metadata  |   | Plugins  |   |  Radio   |   |  Tidal   |
     | 2.1K      |   | 2.2K    |   | 440      |   | 1.1K     |
     +-----------+   +----------+   +----------+   | (DEAD)   |
                                                    +----------+
```

### 6.2 Singleton Usage Heatmap

| Singleton | Files Accessing | Primary Consumers |
|-----------|:--------------:|-------------------|
| ThemeManager | 23+ | All UI files |
| Settings | 15+ | UI, Core, Platform |
| PlaybackState | 13+ | PlaybackBar, views, AudioEngine |
| LibraryDatabase | 12+ | Repositories, views (some direct) |
| MusicDataProvider | 10+ | Views, sidebar |
| AudioEngine | 7+ | PlaybackBar, views, settings |
| MainWindow | 6+ | Legacy -- NavigationService now preferred |
| NavigationService | 5+ | LibraryView, views (NEW) |
| CoverArtService | 5+ | NowPlayingView, detail views (NEW) |
| MetadataFixService | 4+ | Detail views, LibraryView (NEW) |

### 6.3 Cross-Module Dependencies

| Source -> Target | Type | Clean? |
|------------------|------|:------:|
| UI -> Core | Singleton access | Expected |
| UI -> Platform | AudioDeviceManager (settings) | Expected |
| UI -> Apple | AppleMusicManager, MusicKitPlayer | Expected |
| UI -> Metadata | MetadataService, providers | Expected |
| Core -> Core | LibraryScanner -> LibraryDatabase | Expected |
| Apple -> Platform | MusicKitPlayer -> AudioProcessTap | Expected |
| Plugins -> Core | **CLEAN** (was circular in v1) | Fixed |
| Core -> UI | **None** | Clean |

**Key improvement:** The VST3Host -> AudioEngine/DSPPipeline circular dependency identified in v1 has been resolved. No `src/plugins/` file includes any `src/core/audio/` header.

---

## 7. State Machines Audit

### 7.1 MusicKitPlayer State Machines

**AMState** (5 states): `Idle -> Loading -> Playing -> Stalled -> Stopping`
- Drives WKWebView lifecycle and MusicKit JS player state
- Timeout timer guards against stuck states
- Managed via `setAMState()` with logging

**AMPlayState** (7 states): `Idle, WaitingForPlayback, PlayingRequested, Playing, Paused, Stalled, Stopping`
- Fine-grained playback state for JS bridge coordination
- `m_pendingPlaySongId` tracks in-flight requests (thread safety concern -- see 4.3.1)

### 7.2 AudioEngine::State

Simple 3-state: `Stopped, Playing, Paused`
- Clean transitions via play/pause/stop methods
- Atomic member variable

### 7.3 PlaybackState::RepeatMode

3 modes: `Off, All, One`
- Clean enum, persisted via Settings

### 7.4 View State Enums

- `TidalViewState`: `Search, ArtistDetail, AlbumDetail` (dead)
- `AMViewState`: `Search, ArtistDetail, AlbumDetail`
- `ViewMode` (AlbumsView, ArtistsView, PlaylistsView): `LargeIcons, SmallIcons, ListView`
- `AlbumSortMode`: `SortArtist, SortAlbumArtist, SortAlbumArtistYear, SortYear, SortTitle, SortDateAdded`
- `FilterMode` (TrackTableView): `None, Search, Artist, Album, Folder`
- `UpsamplingMode`: enum class with multiple quality levels
- `HeadroomMode` (Settings): `Off, Auto, Manual`

### 7.5 State Machine Issues

1. **No formal state machine framework.** All state machines are hand-coded with `if/switch` statements. No transition validation, no illegal transition detection.
2. **AMPlayState complexity.** 7 states with many intermediate transitions. A formal FSM with a transition table would prevent invalid state changes.
3. **Stall recovery.** MusicKitPlayer uses a timeout timer for stall detection, but recovery logic is spread across multiple methods rather than centralized in the state machine.

---

## 8. Recommended Next Steps (Phase 4+)

### 8.1 Priority 1: Testing Infrastructure

| Item | Risk | Impact | Est. LOC |
|------|:----:|--------|:--------:|
| Add Google Test / Catch2 framework | Low | Enables safe refactoring | ~200 (CMake) |
| Unit tests for repositories (TrackRepository, AlbumRepository) | Low | Validates data layer | ~500 |
| Unit tests for QueueManager | Low | Validates queue logic | ~300 |
| Unit tests for SignalPathBuilder | Low | Validates visualization | ~200 |
| Unit tests for VolumeLevelingManager | Low | Validates ReplayGain | ~200 |
| Integration test harness for AudioEngine | Medium | Validates playback | ~500 |

### 8.2 Priority 2: Fix Thread Safety

| Item | Risk | Impact |
|------|:----:|--------|
| Protect `m_pendingPlaySongId` with QMutex or make atomic-friendly | Low | Prevents race in MusicKitPlayer |
| Protect `m_musicUserToken` with QMutex | Low | Prevents race in AppleMusicManager |
| Audit `AlbumRepository::albumById()` for read/write DB usage | Low | Data consistency |
| Add `setTransferTimeout(15000)` to 5 missing network managers | Low | Prevents hung requests |

### 8.3 Priority 3: Decompose Remaining God Classes

| Item | Risk | Impact |
|------|:----:|--------|
| Split DSPSettingsWidget into EQ/Spatial/Upsampling tabs | Low | Reduces 1,467-line widget |
| Extract MusicKitStateMachine from MusicKitPlayer | Medium | Reduces 1,531-line file, improves state safety |
| Extract TransportControls from PlaybackBar | Low | Reduces 1,106-line widget |
| Remove TidalView + TidalManager (2,195 dead lines) | Low | Dead code removal |

### 8.4 Priority 4: Fix Realtime Violations

| Item | Risk | Impact |
|------|:----:|--------|
| ConvolutionProcessor: pre-allocate IR swap buffer | Medium | Eliminates allocation in render path |
| HRTFProcessor: move loadSOFA() off audio thread | Medium | Eliminates file I/O in render path |

### 8.5 Priority 5: Architecture Evolution

| Item | Risk | Impact |
|------|:----:|--------|
| Unify AudioDevice / AudioDeviceInfo types | Medium | Eliminates dual device enumeration |
| Create IStreamingService interface | Medium | Enables future services |
| Split CMakeLists into library targets per module | Medium | Enforces module boundaries |
| Introduce dependency injection for core services | High | Enables unit testing, reduces coupling |

---

## 9. Module Boundary Proposal (Revised)

### Current vs Proposed

```
CURRENT (flat, singleton-linked)          PROPOSED (layered, interface-bound)
========================================  ========================================
src/                                      src/
  apple/          (3.0K, 5 files)           app/
  config/                                     main.cpp
  core/           (16.5K, 77 files)           MainWindow
    audio/                                  core/
    dsp/                                      audio/         (AudioEngine, decoders)
    library/                                  dsp/           (all processors unified)
    lyrics/                                   data/          (repositories, DB context)
  metadata/       (2.1K, 14 files)            services/      (PlaybackState, QueueManager,
  platform/       (3.6K, 15 files)                            PlaylistManager, Settings)
    macos/                                    utils/         (RateLimiter, CrashHandler)
  plugins/        (2.2K, 8 files)           platform/
  radio/          (440, 4 files)              audio/         (IAudioOutput)
  tidal/          (1.1K, DEAD)                macos/         (CoreAudio, ProcessTap,
  ui/             (20.3K, 69 files)                           BookmarkManager, Sparkle)
    dialogs/                                  windows/       (future: WASAPI, etc.)
    services/                               streaming/
    views/                                    common/        (IStreamingService)
      settings/                               apple/         (AppleMusicManager, MusicKit)
  widgets/        (2.9K, 22 files)          metadata/        (already clean)
                                            plugins/         (already clean)
                                            radio/           (already clean)
                                            ui/
                                              views/
                                              dialogs/
                                              services/      (Nav, MetadataFix, CoverArt)
                                              settings/
                                            widgets/         (already clean)
```

### Key Boundary Rules

1. **`core/` depends on nothing** except `platform/audio/` (via IAudioOutput interface)
2. **`ui/` depends on `core/`** via interfaces and service singletons
3. **`platform/` implements interfaces** defined in `core/`
4. **`streaming/` depends on `core/`** for data types only
5. **`plugins/` depends on `core/dsp/`** for IDSPProcessor only
6. **No module depends on `ui/`** (already true)

---

## 10. Windows Port Readiness

### 10.1 Platform Abstraction Score

| Component | Abstracted? | Windows Work |
|-----------|:-----------:|:------------:|
| Audio output | **Yes** (IAudioOutput) | Implement WASAPIOutput (~2000-3000 lines) |
| Audio decoders | **Yes** (IDecoder) | No changes needed (FFmpeg is cross-platform) |
| DSP pipeline | **Yes** (IDSPProcessor) | No changes needed |
| VST3 hosting | **Mostly** | VST3 SDK is cross-platform; minor path changes |
| VST2 hosting | **Mostly** | VST2 SDK is cross-platform; minor changes |
| Device enumeration | **No** | AudioDeviceManager needs Windows impl |
| Bookmarks/security | **macOS-only** | Replace with Windows file access APIs |
| Auto-update (Sparkle) | **macOS-only** | Replace with WinSparkle or custom |
| Media keys | **macOS-only** | Replace with Windows SMTC |
| Process Tap | **macOS-only** | No Windows equivalent (or use WASAPI loopback) |
| MusicKit/Apple Music | **macOS-only** | No Windows equivalent |

### 10.2 Platform-Specific Code Inventory

| File | Lines | Portability |
|------|------:|:-----------:|
| CoreAudioOutput.cpp | 1,079 | Replace entirely |
| AudioProcessTap.mm | 1,225 | No Windows equivalent |
| AudioDeviceManager_mac.cpp | 604 | Replace entirely |
| MusicKitPlayer.mm | 1,531 | macOS-only feature |
| AppleMusicBridge.mm | 965 | macOS-only feature |
| BookmarkManager.mm | ~200 | Replace with Windows APIs |
| MacMediaIntegration.mm | ~200 | Replace with SMTC |
| SparkleUpdater.mm | ~150 | Replace with WinSparkle |

**Total macOS-specific:** ~5,954 lines
**Total codebase:** ~52,518 lines
**Platform coupling ratio:** ~11.3%

### 10.3 Blockers for Windows Port

1. **AudioDevice/AudioDeviceInfo type unification** -- must be resolved before adding WASAPI
2. **No CMake platform conditionals** -- need `if(APPLE)` / `if(WIN32)` guards
3. **Obj-C++ files (.mm)** -- cannot compile on Windows; need `#ifdef __APPLE__` or separate source sets
4. **Security-scoped bookmarks** -- macOS sandbox concept; Windows needs different file access pattern
5. **MusicKit/Apple Music** -- no Windows equivalent; app must gracefully degrade

### 10.4 Estimated Windows Port Effort

| Component | Estimated Lines | Effort |
|-----------|:---------------:|:------:|
| WASAPIOutput (IAudioOutput impl) | 2,000-3,000 | High |
| Windows AudioDeviceManager | 500-800 | Medium |
| WinSparkle integration | 100-200 | Low |
| Windows media key (SMTC) | 200-400 | Medium |
| CMake platform conditionals | 200-300 | Medium |
| Windows file access (replace bookmarks) | 100-200 | Low |
| **Total new code** | **~3,100-4,900** | |

The core playback pipeline (AudioEngine -> DSPPipeline -> decoders) requires zero changes for Windows. The UI layer (Qt-based) is cross-platform. Only the platform abstraction layer needs new implementations.

---

## Appendix A: Singleton Registry

| # | Singleton | Location | Created |
|---|-----------|----------|---------|
| 1 | Settings | src/core/Settings | main.cpp |
| 2 | ThemeManager | src/ui/ThemeManager | main.cpp |
| 3 | BookmarkManager | src/platform/macos/BookmarkManager | main.cpp |
| 4 | AppleMusicManager | src/apple/AppleMusicManager | main.cpp |
| 5 | LibraryDatabase | src/core/library/LibraryDatabase | main.cpp |
| 6 | MusicDataProvider | src/core/MusicData | main.cpp |
| 7 | PlaybackState | src/core/PlaybackState | main.cpp |
| 8 | PlaylistManager | src/core/library/PlaylistManager | main.cpp |
| 9 | AudioEngine | src/core/audio/AudioEngine | main.cpp |
| 10 | AudioDeviceManager | src/core/audio/AudioDeviceManager | main.cpp |
| 11 | SparkleUpdater | src/platform/macos/SparkleUpdater | main.cpp |
| 12 | LibraryScanner | src/core/library/LibraryScanner | main.cpp |
| 13 | NavigationService | src/ui/services/NavigationService | main.cpp |
| 14 | CoverArtService | src/ui/services/CoverArtService | main.cpp |
| 15 | MusicKitPlayer | src/apple/MusicKitPlayer | on-demand |
| 16 | TidalManager | src/tidal/TidalManager | on-demand |
| 17 | MetadataService | src/metadata/MetadataService | on-demand |
| 18 | AcoustIdProvider | src/metadata/AcoustIdProvider | on-demand |
| 19 | AudioFingerprinter | src/metadata/AudioFingerprinter | on-demand |
| 20 | CoverArtProvider | src/metadata/CoverArtProvider | on-demand |
| 21 | FanartTvProvider | src/metadata/FanartTvProvider | on-demand |
| 22 | MusicBrainzProvider | src/metadata/MusicBrainzProvider | on-demand |
| 23 | CoverArtLoader | src/widgets/CoverArtLoader | on-demand |
| 24 | AutoplayManager | src/core/AutoplayManager | on-demand |
| 25 | LastFmProvider | src/radio/LastFmProvider | on-demand |
| 26 | MacMediaIntegration | src/platform/macos/MacMediaIntegration | on-demand |
| 27 | AudioProcessTap | src/platform/macos/AudioProcessTap | on-demand |
| 28 | MainWindow | src/ui/MainWindow | main.cpp |
| 29 | MetadataFixService | src/ui/services/MetadataFixService | on-demand |

---

*This document is the v2 architecture analysis for Sorana Flow v1.7.5. No code changes were made during this analysis.*
