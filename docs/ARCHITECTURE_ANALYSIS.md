# Sorana Flow -- Architecture Analysis for Modularization

**Date:** February 2026
**Scope:** Full codebase analysis -- 166 files, ~50,000 lines
**Purpose:** Reference document for modularization planning

---

## 1. Project Overview

### Codebase Metrics

| Module | Directory | Lines | Files |
|--------|-----------|------:|------:|
| UI (views, dialogs) | `src/ui/` | 20,120 | 40 |
| Core (audio, data, services) | `src/core/` | 15,059 | 30 |
| Platform (macOS) | `src/platform/` | 3,590 | 13 |
| Widgets | `src/widgets/` | 3,345 | 22 |
| Metadata | `src/metadata/` | 1,870 | 14 |
| Apple Music | `src/apple/` | 2,502 | 4 |
| Plugins (VST) | `src/plugins/` | 1,823 | 8 |
| Tidal | `src/tidal/` | 1,078 | 2 |
| Build / Entry | `src/main.cpp`, `CMakeLists.txt` | ~1,000 | 2 |
| **Total** | | **~50,092** | **166** |

### Build System

Single monolithic CMake target with logical source groupings (CORE_SOURCES, LIBRARY_SOURCES, AUDIO_SOURCES, DSP_SOURCES, METADATA_SOURCES, PLUGIN_SOURCES, PLATFORM_SOURCES, APPLE_MUSIC_SOURCES, UI_SOURCES, VIEW_SOURCES, WIDGET_SOURCES). No library targets, no module boundaries enforced at build level.

### Architecture Style

**Singleton-centric flat architecture.** 20+ singletons provide global access to services. No dependency injection, no service locator, no controller/mediator layer between UI and core services. Views access core services directly via `ClassName::instance()`.

### Startup Model

Phased initialization in `main.cpp` (463 lines):
- Phase 0: Lightweight init (Settings, Theme, Database, Bookmarks) -- synchronous
- Window show: MainWindow constructed and painted
- Phase 1: Audio engine (deferred via QTimer::singleShot(0))
- Phase 2: Library data load (nested deferred)
- Phase 3: Background tasks (500ms delay -- auto-scan, file watcher)
- Shutdown: 5-second watchdog, ordered teardown

---

## 2. Architecture Strengths

### 2.1 Clean Core-to-UI Separation at Include Level

Zero circular `#include` dependencies between `src/core/` and `src/ui/`. The dependency direction is strictly UI -> Core. No core class includes any UI header. This is the strongest architectural property of the codebase and makes modularization feasible.

### 2.2 IAudioOutput Platform Abstraction

`IAudioOutput` (69 lines) is a clean interface that could support WASAPI/ALSA without modification. It uses only standard C++ types (`std::function`, `std::vector`, `std::string`, `uint32_t`). CoreAudioOutput fully encapsulates all CoreAudio specifics via PIMPL -- no platform types leak into headers. Adding Windows support would require ~2000-3000 lines of new platform code but zero changes to the interface.

### 2.3 Realtime Thread Discipline

The audio render path follows strict realtime rules:
- `CoreAudioOutput`: `try_lock` on callback mutex (outputs silence on contention)
- `CrossfeedProcessor`: All-atomic control, pending parameter exchange pattern, zero allocations
- `DSPPipeline::process()`: `try_to_lock` on plugin mutex (skips plugins on contention)
- `AudioEngine`: Pre-allocated decode/crossfade buffers, no allocations in render path
- `AudioProcessTap`: Pre-allocated interleave buffer, atomic DSP activation flag

### 2.4 PIMPL Pattern for Obj-C++

Consistent use of opaque `struct Impl` / `void*` in headers to hide Objective-C types from C++ consumers. CoreAudioOutput, MusicKitPlayer, BookmarkManager, SparkleUpdater, AudioProcessTap all follow this pattern. No `NSString*`, `AudioDeviceID`, or `CFStringRef` appears in any header file.

### 2.5 IDSPProcessor Interface

Clean abstract interface for audio processors. Both VST2Plugin and VST3Plugin implement it, allowing polymorphic management through DSPPipeline. Hosts return `shared_ptr<IDSPProcessor>`, decoupling plugin format from the processing chain.

### 2.6 MetadataService Facade

Well-structured facade over 5 independent providers (MusicBrainz, AcoustId, AudioFingerprinter, CoverArtArchive, FanartTv). Each provider is an independent singleton with its own QNetworkAccessManager and rate limiter. Providers communicate via Qt signals -- no cross-provider dependencies.

### 2.7 MusicDataProvider Swap-on-Complete Caching

Background reload runs DB queries on a worker thread, then atomically swaps results under a QWriteLocker. The stale cache remains visible during loading -- no "empty gap" that would cause blank UI. The TrackIndex lightweight struct (~100 bytes/track vs ~400 bytes for full Track) enables instant startup with 100K+ track libraries.

### 2.8 Phased Startup

Window is shown before heavy initialization (audio engine, library load). The user sees the UI within milliseconds while audio subsystems initialize in the background.

### 2.9 RateLimiter Utility

Generic, 35-line QTimer-based rate limiter with zero domain knowledge. Already shared across modules (metadata providers + radio/LastFm). Could be extracted to a shared utility location without any code changes.

### 2.10 Crash Handler

Completely standalone. Uses only POSIX async-signal-safe functions (`open`, `write`, `backtrace`, `_exit`). Zero dependencies on any Sorana Flow code at runtime.

---

## 3. Architecture Problems

### 3.1 Singleton Overuse (20+ Singletons)

Every major component is a global singleton. This creates hidden dependencies, makes unit testing impossible, and means initialization order is implicitly managed.

**Singletons created in main.cpp (controlled order):**
Settings, ThemeManager, BookmarkManager, AppleMusicManager, LibraryDatabase, MusicDataProvider, PlaybackState, PlaylistManager, AudioEngine, AudioDeviceManager, SparkleUpdater, LibraryScanner

**Singletons created on-demand (uncontrolled):**
MusicKitPlayer, TidalManager, MetadataService, all Metadata Providers, VST2Host, VST3Host, AutoplayManager, LastFmProvider, CoverArtLoader

### 3.2 No Controller/Mediator Layer

Views reach directly into 15+ singletons. There is no controller, presenter, or mediator between UI and services. The architecture is flat: View -> Singleton::instance() -> Core.

**Singleton access density across UI files:**
- ThemeManager: 23 files
- PlaybackState: 13 files
- MusicDataProvider: 10 files
- LibraryDatabase: 9 files (direct, bypassing MusicDataProvider)
- MetadataReader: 8 files
- Settings: 5 files
- MainWindow::instance(): 6 files (from child views -- circular logical dependency)
- AudioEngine: 4 files

### 3.3 Database Bypassed by 60% of Views

9 of 15 data-consuming views access `LibraryDatabase::instance()` directly instead of going through `MusicDataProvider`. The "fallback to LibraryDatabase when MusicDataProvider is empty" pattern in AlbumsView and ArtistsView suggests MusicDataProvider's data availability is unreliable.

**Direct DB access:** MainWindow, SettingsView, LibraryView, AlbumsView, ArtistsView, AlbumDetailView, ArtistDetailView, SearchResultsView, PlaylistDetailView
**Proper access through MusicDataProvider:** FolderBrowserView, NowPlayingView, QueueView, PlaylistsView

### 3.4 Duplicated Business Logic in Views

| Pattern | Files | Lines Duplicated |
|---------|-------|-----------------|
| Metadata fix/undo/fingerprint workflow | LibraryView, AlbumDetailView, ArtistDetailView, PlaylistDetailView | ~80 x 4 = ~320 |
| Cover art discovery chain | NowPlayingView, QueueView, AlbumsView, ArtistsView, AlbumDetailView, ArtistDetailView, SearchResultsView, PlaybackBar | ~30 x 8 = ~240 |
| Track table context menu | LibraryView, AlbumDetailView, ArtistDetailView, PlaylistDetailView, NowPlayingView | ~40 x 5 = ~200 |
| MusicDataProvider fallback to LibraryDatabase | AlbumsView, ArtistsView | ~15 x 2 = ~30 |

### 3.5 PlaybackBar Bypasses PlaybackState

PlaybackBar calls `AudioEngine::instance()->setVolume(0)` directly for mute, storing pre-mute volume locally. PlaybackState is never informed. Any view reading volume from PlaybackState sees incorrect values during mute. Device selection also bypasses PlaybackState.

### 3.6 MainWindow Circular Dependency

Multiple views call `MainWindow::instance()` for navigation (AlbumsView, ArtistsView, LibraryView, PlaylistsView, AppleMusicView). MainWindow owns and creates these views. This creates a bidirectional logical dependency that prevents view reuse and testing in isolation.

### 3.7 No Shared Streaming Interface

AppleMusicManager and TidalManager have no common `IStreamingService` abstraction. They share similar patterns (singleton, QNetworkAccessManager, QJsonArray results) but this is coincidental, not designed. MusicKitPlayer and any future Tidal player have no common `IStreamingPlayer` interface.

### 3.8 DSP Chain Split: Pipeline vs Bypass

Only 3 of 7 DSP processors implement `IDSPProcessor` and run inside DSPPipeline. The other 4 bypass it entirely:

| Processor | In Pipeline? | Interface |
|-----------|:---:|-----------|
| GainProcessor | Yes | IDSPProcessor |
| EqualizerProcessor | Yes | IDSPProcessor |
| UpsamplerProcessor | Yes* | IDSPProcessor (but `process()` is a no-op) |
| VST Plugins | Yes | IDSPProcessor |
| CrossfeedProcessor | **No** | Own `process(float*, int)` |
| ConvolutionProcessor | **No** | Own `process(float*, int, int)` |
| HRTFProcessor | **No** | Own `process(float*, int)` |

*UpsamplerProcessor inherits IDSPProcessor but its `process()` override is a no-op. Actual upsampling uses `processUpsampling()` with separate I/O buffers. This violates Liskov Substitution Principle.

The actual render chain is hardcoded in AudioEngine:
```
Source -> Decode -> [Upsampler] -> DSPPipeline(Gain -> EQ -> VST) -> Crossfeed -> Convolution -> HRTF -> Output
```

### 3.9 VST3Host Circular Dependency

`VST3Host::openPluginEditor()` includes `AudioEngine.h` and `DSPPipeline.h` to find the active plugin. This creates a circular dependency: AudioEngine -> Plugins (processing) and Plugins -> AudioEngine (editor opening). The scan and load paths are clean; only editor opening has this coupling.

### 3.10 VST2Plugin Realtime Violation

`VST2Plugin::process()` uses `QMutex::lock()` (blocking), while `VST3Plugin::process()` correctly uses `std::mutex` with `try_lock()` (non-blocking). The VST2 approach risks priority inversion on the audio thread.

### 3.11 Realtime Violations in IR/HRTF Loading

- **ConvolutionProcessor**: IR swap path in `process()` calls `m_fdl.resize()` and `.assign()` which allocate on the audio thread.
- **HRTFProcessor**: `updateFilters()` allocates vectors from the render thread. Worse, `loadSOFA()` can be called from `process()` when sample rate changes, performing file I/O on the audio thread.

### 3.12 Parallel Device Management

`AudioDeviceManager` and `CoreAudioOutput::enumerateDevicesStatic()` both query CoreAudio for device lists independently. They return different types (`AudioDeviceInfo` with `QString` vs `AudioDevice` with `std::string`). This duplication means device enumeration is implemented twice with potential for inconsistency.

### 3.13 Zero Unit Tests

No test files, no test framework, no test targets in CMakeLists.txt. The entire codebase is verified only through manual testing.

---

## 4. God Classes

### 4.1 AudioEngine (1,797 lines, 37+ member variables)

The central god class. 27+ distinct responsibilities:

| # | Responsibility | Lines |
|---|----------------|------:|
| 1 | Singleton lifecycle | ~5 |
| 2 | Constructor: settings, signals, DSP init | ~178 |
| 3 | Shutdown/destruction: ordered teardown | ~52 |
| 4 | File loading & decoder selection (PCM vs DSD) | ~242 |
| 5 | Playback control (play/pause/stop/seek) | ~96 |
| 6 | Volume control | ~6 |
| 7 | Position tracking | ~7 |
| 8 | Device management (set device, enumerate, buffer, rate) | ~153 |
| 9 | Bit-perfect mode | ~8 |
| 10 | Auto sample rate | ~24 |
| 11 | Exclusive mode | ~17 |
| 12 | Upsampling orchestration | ~34 |
| 13 | Volume leveling (ReplayGain, R128) | ~123 |
| 14 | Headroom management | ~29 |
| 15 | Gapless playback (prepare, cancel, swap) | ~93 |
| 16 | Crossfade duration | ~5 |
| 17 | Render callback (realtime audio thread) | ~391 |
| 18 | Render: upsampler path | ~89 |
| 19 | Render: normal path | ~155 |
| 20 | Render: crossfade mixing | ~91 |
| 21 | Render: DSP chain application | ~90 (x2 duplicated) |
| 22 | Render: gapless transition | ~73 |
| 23 | Render: fade-in ramp | ~21 |
| 24 | Render: DoP end-of-track | ~13 |
| 25 | Position timer callback | ~26 |
| 26 | Signal path visualization | ~310 |
| 27 | DSD format detection | ~13 |

**Key issues:**
- The DSP chain (headroom -> crossfeed -> convolution -> HRTF -> EQ -> leveling -> limiter) is written **twice** in renderAudio() -- once for the upsampler path and once for the normal path (~80 lines duplicated).
- Gapless decoder swap logic is **duplicated** between crossfade completion and end-of-track paths.
- `if (m_usingDSDDecoder)` appears **12+ times** because AudioDecoder and DSDDecoder have no shared interface.

**High-value extractions:**
1. **GaplessPlaybackManager** -- preparation, crossfade, decoder swap (~300 lines)
2. **AudioRenderChain** -- eliminate DSP chain duplication (~160 lines)
3. **VolumeLevelingManager** -- ReplayGain/R128 calculation + LoudnessAnalyzer dispatch (~120 lines)
4. **SignalPathBuilder** -- pure read-only visualization logic (~310 lines)
5. **DecoderManager** -- decoder selection, DSD detection, fallback (~100 lines)

### 4.2 SettingsView (3,717 lines)

The single largest file in the project. Handles ALL application settings across 7+ distinct tabs:

1. **LibrarySettingsTab** (~600 lines) -- folders, bookmarks, scan
2. **PlaybackDSPSettingsTab** (~500 lines) -- upsampling, DSD, DSP, buffers
3. **VSTPluginSettingsTab** (~500 lines) -- VST3/VST2 scan, load/unload, chain
4. **AudioOutputSettingsTab** (~400 lines) -- device, exclusive mode, audio tap
5. **AppleMusicSettingsTab** (~300 lines) -- authentication, MusicKit token
6. **MetadataSettingsTab** (~200 lines) -- metadata service config
7. **UpdatesAboutTab** (~200 lines) -- Sparkle, version info

Accesses **18 distinct singletons** -- the highest coupling in the codebase. The `refreshTheme()` method destroys and rebuilds the **entire** UI rather than updating styles in-place.

### 4.3 LibraryDatabase (2,134 lines, ~50 public methods)

God object for all database access. Handles CRUD for 4 entity types (tracks, albums, artists, playlists) plus cross-cutting concerns (play history, metadata backup/undo, FTS5 search, incremental rebuild, database backup/restore, integrity checking).

**Issues:**
- `albumById()` uses the write connection (`m_db`) for track sub-queries -- inconsistent with read/write split
- `removeDuplicates()` lacks a transaction for its multi-step cleanup
- ~50 public methods on a single class

### 4.4 PlaybackState (874 lines, 15 responsibilities)

1. Play/pause control (source-aware: Local vs Apple Music)
2. Track navigation (next/previous with 3s restart threshold)
3. Seek control
4. Volume management
5. Shuffle management (Fisher-Yates)
6. Repeat mode management
7. Queue management (add, insert, remove, move, clear)
8. Display queue (shuffle-aware ordering)
9. Gapless playback scheduling
10. Queue persistence (debounced async save)
11. Autoplay integration
12. Source switching (Local <-> Apple Music)
13. Track info management
14. Signal emission hub (8 distinct signals)
15. Shutdown flush

### 4.5 ArtistDetailView (1,063 lines)

Largest view after SettingsView. Contains biography fetching via its own QNetworkAccessManager (Last.fm API), MusicBrainz annotation sanitization, metadata fix/undo/fingerprint workflow, and cover art discovery -- all inline.

### 4.6 AppleMusicView (1,272 lines)

Tightly coupled to AppleMusicManager and MusicKitPlayer. Constructs Track objects from JSON, manages music user token flow, maintains its own internal navigation stack. Not replaceable without near-complete rewrite.

---

## 5. Coupling Map

### Module Dependency Graph

```
                    ┌─────────────┐
                    │   main.cpp  │
                    └──────┬──────┘
                           │ creates all singletons
           ┌───────────────┼───────────────┐
           v               v               v
    ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
    │  UI Layer   │ │ Core Layer  │ │  Platform   │
    │ (20K lines) │ │ (15K lines) │ │ (3.6K lines)│
    └──────┬──────┘ └──────┬──────┘ └──────┬──────┘
           │               │               │
           │  direct calls │               │
           ├──────────────>│               │
           │               │  implements   │
           │               ├──────────────>│
           │               │               │
           │   bypasses    │               │
           ├───────────────┼──────────────>│
           │  (4 views call AudioEngine/   │
           │   AudioDeviceManager directly)│
           v               v               v
    ┌─────────────┐ ┌─────────────┐ ┌─────────────┐
    │  Metadata   │ │   Plugins   │ │ Apple Music │
    │ (1.9K lines)│ │ (1.8K lines)│ │ (2.5K lines)│
    └─────────────┘ └─────────────┘ └─────────────┘
```

### Cross-Module Coupling Table

| Source Module | Target Module | Coupling Type |
|---------------|---------------|---------------|
| UI -> Core | PlaybackState, LibraryDatabase, MusicDataProvider, Settings, AudioEngine | Direct singleton access |
| UI -> Platform | AudioDeviceManager (PlaybackBar, SettingsView) | Direct singleton access |
| UI -> Apple | AppleMusicManager, MusicKitPlayer (AppleMusicView) | Direct singleton access |
| UI -> Metadata | MetadataService, providers (detail views) | Direct singleton access |
| UI -> Plugins | VST3Host, VST2Host (SettingsView, MainWindow) | Direct singleton access |
| Core -> Core | LibraryScanner -> LibraryDatabase (direct) | Tight coupling |
| Apple -> Platform | MusicKitPlayer -> AudioDeviceManager, AudioProcessTap | Direct singleton access |
| Plugins -> Core | VST3Host -> AudioEngine, DSPPipeline (editor only) | Circular dependency |

### Singleton Usage Heatmap (top consumers)

| Singleton | Files Accessing |
|-----------|:--------------:|
| ThemeManager | 23 |
| LibraryDatabase | 16 |
| PlaybackState | 13 |
| Settings | 11+ |
| MusicDataProvider | 10 |
| AudioEngine | 7 |
| MainWindow (from children) | 6 |

---

## 6. Missing Abstractions

### 6.1 IDecoder Interface

AudioDecoder and DSDDecoder have duck-typed APIs with matching method names (`open`, `close`, `read`, `seek`, `format`, `isOpen`, `currentPositionSecs`) but no shared base class. An `IDecoder` interface would eliminate the 12+ `if (m_usingDSDDecoder)` branches in AudioEngine. The different `open()` signatures (DSDDecoder takes a `dopMode` parameter) would be handled at the factory level.

### 6.2 IStreamingService / IStreamingPlayer

No common abstraction between Apple Music and Tidal. Both have similar patterns (singleton, REST API, JSON results, auth tokens) but share no interface. Adding Spotify or any future service requires starting from scratch with no reusable patterns.

### 6.3 NavigationService

Views call `MainWindow::instance()->navigateToAlbum()` etc., creating circular logical dependencies. Should be a `NavigationService` or signal-based navigation where views emit `navigateToAlbumRequested(albumId)` signals.

### 6.4 MetadataFixService

The metadata fix/undo/fingerprint workflow is duplicated across 4 views (~320 lines). Should be a standalone service that views delegate to.

### 6.5 CoverArtService (unified)

Cover art discovery logic is duplicated across 8 files (~240 lines). `CoverArtLoader` exists but views bypass it with inline discovery chains. Additionally, `MetadataReader::extractCoverArt()` (returns QPixmap) and `TagWriter::readAlbumArt()` (returns QImage) have ~90% identical implementations.

### 6.6 View Base Class / Interface

No base class defines what a view must provide. All views inherit directly from QWidget. MainWindow hard-codes view construction. Views are not replaceable without modifying MainWindow.

### 6.7 Unified DSP Processor Interface

CrossfeedProcessor, ConvolutionProcessor, and HRTFProcessor do not implement IDSPProcessor. They have incompatible `process()` signatures (stereo-only variants). This prevents unified management and reordering through the pipeline.

### 6.8 Unified Device Info Type

`AudioDevice` (std::string, used by IAudioOutput) and `AudioDeviceInfo` (QString, used by AudioDeviceManager) represent the same concept with different types. Device enumeration is implemented twice.

---

## 7. Recommended Module Structure

### Target Module Boundaries

```
sorana-flow/
├── core/
│   ├── audio/          # AudioEngine (decomposed), IDecoder, decoders
│   ├── dsp/            # IDSPProcessor, DSPPipeline, all processors
│   ├── data/           # MusicData, LibraryDatabase (split), MusicDataProvider
│   ├── services/       # PlaybackState (decomposed), PlaylistManager, Settings
│   └── utils/          # RateLimiter, CrashHandler, AutoOrganizer
├── platform/
│   ├── audio/          # IAudioOutput, AudioDevice (unified type)
│   ├── macos/          # CoreAudioOutput, AudioProcessTap, BookmarkManager
│   └── macos-ui/       # MacMediaIntegration, SparkleUpdater
├── streaming/
│   ├── common/         # IStreamingService, IStreamingPlayer
│   ├── apple/          # AppleMusicManager, MusicKitPlayer
│   └── tidal/          # TidalManager
├── metadata/           # MetadataService, all providers (already clean)
├── plugins/            # VST2Host, VST2Plugin, VST3Host, VST3Plugin
├── widgets/            # All styled widgets, TrackTableView (already clean)
├── ui/
│   ├── views/          # All views
│   ├── dialogs/        # All dialogs
│   ├── navigation/     # NavigationService (new)
│   └── services/       # MetadataFixService, CoverArtService (new)
└── app/
    ├── main.cpp        # Entry point, phased init
    └── MainWindow       # Shell, navigation, view creation
```

### Key Structural Changes

1. **Split LibraryDatabase** into: TrackRepository, AlbumRepository, ArtistRepository, PlaylistRepository, SearchRepository, DatabaseManager (lifecycle, backup, integrity)
2. **Decompose AudioEngine** into: AudioEngine (slim coordinator), GaplessManager, RenderChain, VolumeLevelingManager, SignalPathBuilder, DecoderFactory
3. **Decompose SettingsView** into 7 tab widgets
4. **Decompose PlaybackState** into: PlaybackController (play/pause/seek), QueueManager, QueuePersistence
5. **Extract UI services**: MetadataFixService, CoverArtService, NavigationService
6. **Unify device types**: Single `AudioDevice` type with string-based ID (for future WASAPI)
7. **Create IDecoder interface**: Eliminate duck-typed decoder branching

---

## 8. Refactoring Priority Order

### Phase 1: Low-Risk, High-Value Extractions

These changes reduce complexity without altering runtime behavior.

| Priority | Change | Risk | Impact | LOC Affected |
|:--------:|--------|:----:|--------|:------------:|
| 1 | Extract MetadataFixService from 4 views | Low | Eliminates ~320 lines of duplication | ~400 |
| 2 | Extract CoverArtService (unify discovery) | Low | Eliminates ~240 lines of duplication | ~300 |
| 3 | Split SettingsView into tab widgets | Low | 3717-line monolith becomes 7 files | ~3717 |
| 4 | Extract SignalPathBuilder from AudioEngine | Low | Pure read-only, no state changes | ~310 |
| 5 | Extract VolumeLevelingManager from AudioEngine | Low | Self-contained concern with atomic interface | ~120 |

### Phase 2: Interface Introductions

These add abstractions without changing implementations.

| Priority | Change | Risk | Impact | LOC Affected |
|:--------:|--------|:----:|--------|:------------:|
| 6 | Create IDecoder interface | Medium | Eliminates 12+ `if (m_usingDSDDecoder)` branches | ~200 |
| 7 | Make Crossfeed/Convolution/HRTF implement IDSPProcessor | Medium | Enables unified pipeline management | ~150 |
| 8 | Create NavigationService (eliminate MainWindow::instance() from views) | Medium | Breaks circular dependency | ~100 |
| 9 | Unify AudioDevice / AudioDeviceInfo types | Medium | Eliminates dual device enumeration | ~200 |

### Phase 3: Structural Decomposition

These are larger changes requiring careful testing.

| Priority | Change | Risk | Impact | LOC Affected |
|:--------:|--------|:----:|--------|:------------:|
| 10 | Decompose AudioEngine (GaplessManager, RenderChain) | High | Reduces 1797-line god class | ~1000 |
| 11 | Split LibraryDatabase into repositories | High | Reduces 2134-line god class | ~2134 |
| 12 | Decompose PlaybackState (QueueManager, Persistence) | Medium | Reduces 874 lines, 15 responsibilities | ~874 |
| 13 | Fix VST3Host circular dependency | Low | Move editor opening to a mediator | ~50 |
| 14 | Fix VST2Plugin realtime safety (QMutex -> try_lock) | Low | Prevents priority inversion | ~20 |

### Phase 4: Architecture Evolution

These enable future capabilities (Windows support, unit testing).

| Priority | Change | Risk | Impact | LOC Affected |
|:--------:|--------|:----:|--------|:------------:|
| 15 | Add dependency injection (at least for core services) | High | Enables unit testing | ~500 |
| 16 | Create IStreamingService interface | Medium | Enables future services | ~100 |
| 17 | Add unit test framework and first tests | Medium | Safety net for refactoring | ~1000+ |
| 18 | Split CMakeLists into library targets per module | Medium | Enforces module boundaries at build level | ~200 |

---

## 9. Risk Assessment

### High Risk

| Risk | Probability | Impact | Mitigation |
|------|:-----------:|:------:|------------|
| AudioEngine decomposition breaks realtime audio | Medium | Critical | Keep render callback as thin dispatcher; move logic to pre/post-render phases; extensive playback testing across formats |
| LibraryDatabase split causes data inconsistencies | Medium | High | Add integration tests; keep shared transaction manager; split read-only queries first |
| No unit tests means regressions are invisible | High | High | Add test framework before starting Phase 3; write tests for extracted components |

### Medium Risk

| Risk | Probability | Impact | Mitigation |
|------|:-----------:|:------:|------------|
| SettingsView tab split breaks signal connections | Low | Medium | Each tab keeps same singleton access initially; refactor access patterns later |
| IDecoder interface changes DSD behavior | Low | High | Extensive DSD/DoP testing; keep fallback paths; test all DSD rates |
| NavigationService breaks view navigation flows | Low | Medium | Start with signal-based navigation; keep MainWindow as coordinator |

### Low Risk

| Risk | Probability | Impact | Mitigation |
|------|:-----------:|:------:|------------|
| Extracting MetadataFixService | Very Low | Low | Pure code extraction, no behavioral change |
| Extracting CoverArtService | Very Low | Low | Pure code extraction, no behavioral change |
| Extracting SignalPathBuilder | Very Low | Low | Read-only visualization logic, no state mutation |

### Critical Constraints

1. **Never allocate in the render callback.** Any refactoring of AudioEngine must preserve the zero-allocation property in the realtime path.
2. **Maintain atomic variable patterns.** Cross-thread state (m_usingDSDDecoder, m_nextTrackReady, m_bitPerfect, etc.) must remain atomic with correct memory ordering.
3. **Preserve phased startup order.** Settings must load before ThemeManager; Database must open before MusicDataProvider; AudioEngine must exist before restoring playback state.
4. **Test with DSD files.** DSD/DoP handling spans AudioEngine, DSDDecoder, CoreAudioOutput, and SignalPathBuilder. Any decomposition touching these paths requires testing with DSF/DFF files at all DSD rates (64/128/256/512).
5. **Test gapless transitions.** The crossfade and gapless swap paths are the most complex parts of renderAudio(). They must be tested with: same-format transitions, cross-format transitions (PCM -> DSD), and crossfade-disabled gapless.

---

*This document serves as the reference for modularization planning. No code changes were made during this analysis.*
