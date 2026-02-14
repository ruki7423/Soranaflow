<p align="center">
  <img src="docs/images/app-icon.png" width="128" alt="Sorana Flow">
</p>

<h1 align="center">Sorana Flow</h1>

<p align="center">
  <b>Professional Hi-Fi Audio Player for macOS</b><br>
  Bit-perfect playback · DSD native · Advanced DSP · Apple Music integration
</p>

<p align="center">
  <a href="https://github.com/ruki7423/Soranaflow/releases/latest">
    <img src="https://img.shields.io/github/v/release/ruki7423/Soranaflow?style=flat-square&color=blue" alt="Release">
  </a>
  <a href="https://github.com/ruki7423/Soranaflow/releases">
    <img src="https://img.shields.io/github/downloads/ruki7423/Soranaflow/total?style=flat-square&color=green" alt="Downloads">
  </a>
  <img src="https://img.shields.io/badge/platform-macOS%2013%2B-lightgrey?style=flat-square" alt="Platform">
  <img src="https://img.shields.io/badge/arch-Apple%20Silicon-orange?style=flat-square" alt="Architecture">
  <a href="https://soranaflow.com">
    <img src="https://img.shields.io/badge/website-soranaflow.com-blue?style=flat-square" alt="Website">
  </a>
</p>

---

## What's New (v1.7.2)

### Bug Fixes
- Fixed VST3/VST2 plugins failing to load — missing codesign entitlements blocked macOS from loading unsigned third-party plugin libraries

---

## What's New (v1.7.0)

### Features
- Year display in Now Playing and Album Detail views
- Album Artist standalone sort option in Albums view
- Internet metadata lookup toggle in Settings

### Bug Fixes
- Fixed album artist and year not populated from file tags
- VST plugin loading now shows clear error messages on failure

---

## What's New (v1.6.1)

### Bug Fixes
- Fixed crash (deadlock) when enabling Upsampling in Settings
- Fixed Appearance tab highlighting in Settings

### Internal
- Phase 1 modularization: extracted MetadataFixService, CoverArtService, SignalPathBuilder, VolumeLevelingManager; split SettingsView into 6 focused tab widgets

---

## What's New (v1.6.0)

### Audio
- Fixed DSD DoP crackling, pops, and track transition noise
- Fixed VST3 plugin class selection causing silent playback
- Buffer size now filtered by device capability
- Gapless DoP marker state preserved across track boundaries

### Apple Music
- Fixed first-click playback failure with state machine + command queue
- Added loading indicator during MusicKit DRM handshake

### Library & UI
- New Album Artist column with sortable header
- Albums view: new sort options (Artist, Album Artist, Year, Title)
- Responsive layout: window resize adapts all views
- Artist page: album covers, popular tracks, clickable URLs
- Auto-organize confirmation warning

## Features

**Playback Engine**
- Bit-perfect playback via CoreAudio exclusive mode
- DSD native support (DoP / Native)
- Gapless playback with sample-accurate transitions
- Support for FLAC, ALAC, WAV, AIFF, DSD (DSF/DFF), MP3, AAC, OGG

**DSP Processing**
- 20-band parametric EQ with real-time frequency analyzer
- VST3 & VST2 plugin hosting with native editor UI
- Convolution engine for room correction (IR loading)
- HRTF binaural processing (libmysofa)
- Headroom management & limiter
- Full signal path visualization

**Library Management**
- Smart library scanning with metadata extraction
- Folder browser with tree-based navigation
- Album art discovery (folder + embedded)
- MusicBrainz / AcoustID integration for metadata lookup
- Library rollback — one-click restore after rescan
- Synced lyrics support (embedded + LRCLIB)

**Streaming**
- Apple Music integration via Native MusicKit

## Tech Stack

| Component | Technology |
|-----------|-----------|
| Framework | Qt 6 / C++17 |
| Audio | CoreAudio, FFmpeg, soxr |
| DSP | Custom pipeline, VST3 SDK, VST2 SDK |
| Metadata | TagLib, MusicBrainz, AcoustID, chromaprint |
| Streaming | Native MusicKit (Apple Music) |
| Spatial | libmysofa (HRTF) |
| Updates | Sparkle framework |
| Build | CMake, Apple Silicon native (arm64) |

## Installation

**Download** the latest DMG: **[Sorana Flow v1.7.2](https://github.com/ruki7423/Soranaflow/releases/download/v1.7.2/SoranaFlow-1.7.2.dmg)** or browse [all releases](https://github.com/ruki7423/Soranaflow/releases). Also available at [soranaflow.com/downloads](https://soranaflow.com/downloads).

Drag **Sorana Flow** to your Applications folder. The app is signed and notarized.

**Requirements:**
- macOS 13.0 (Ventura) or later
- Apple Silicon (M1 / M2 / M3 / M4)

## Contributors

<table>
  <tr>
    <td align="center">
      <a href="https://github.com/ruki7423">
        <img src="https://github.com/ruki7423.png" width="80" style="border-radius:50%"><br>
        <b>ruki7423</b>
      </a><br>
      Lead Developer
    </td>
    <td align="center">
      <a href="https://claude.ai">
        <img src="https://avatars.githubusercontent.com/u/76263028" width="80" style="border-radius:50%"><br>
        <b>Claude Code</b>
      </a><br>
      AI Pair Programmer
    </td>
  </tr>
</table>

> Built with [Claude Code](https://claude.ai/code) by Anthropic — AI-assisted architecture, DSP pipeline, cross-platform abstractions, and automated testing.

## Growth

| Metric | Count |
|--------|-------|
| GitHub Releases | 23 (v1.0.0 → v1.7.2) |
| Total Downloads | ![Downloads](https://img.shields.io/github/downloads/ruki7423/Soranaflow/total?style=flat-square&label=) |
| Latest Release | ![Latest](https://img.shields.io/github/downloads/ruki7423/Soranaflow/latest/total?style=flat-square&label=) |
| Stars | ![Stars](https://img.shields.io/github/stars/ruki7423/Soranaflow?style=flat-square&label=) |
| Commits | ![Commits](https://img.shields.io/github/commit-activity/m/ruki7423/Soranaflow?style=flat-square&label=) |

## Changelog

### v1.7.2 — VST Plugin Fix

- Fixed VST3/VST2 loading: missing codesign entitlements

### v1.7.0 — Year & Album Artist

- Year display, Album Artist sort, VST error feedback, internet metadata toggle

### v1.6.1 — Upsampling Crash Fix

- Fixed deadlock when enabling Upsampling, fixed Appearance tab highlighting

### v1.6.0 — DSD DoP Fix, Apple Music, Responsive UI

- Fixed DSD crackling, Apple Music first-click, VST3 class selection
- See [What's New (v1.6.0)](#whats-new-v160) for full details

### v1.5.4 — Performance & Column Fixes

- Fixed startup delay, column resize lag, and library display issues

### v1.5.3 — Header State Migration

- Fixed library columns not visible after updating from older versions

### v1.5.2 — Startup Performance Fix

- Fixed ~2 minute startup delay when external drives were disconnected

### v1.5.1 — Performance & DSD Fix

- Faster search, album browsing, and cover art loading
- Fixed DSD playback issues and silent Apple Music playback

### v1.5.0 — Architecture Refactoring

- Eliminated all synchronous DB calls from the main thread (swap-on-complete caching)
- SVG icon cache: 102 uncached renders → QHash lookup with theme-change invalidation
- Crash handler: POSIX sigaction + backtrace to crash.log, previous-crash detection on startup
- DB integrity: PRAGMA quick_check on open, auto-backup corrupt databases

### v1.4.6 — Freeze Hotfix

- Library reload: debounced view cascade (max once per 2s during scan)
- Album/Artist views: single allTracks() copy per reload (was 2+ full copies)
- Autoplay: O(1) hash lookup replaces O(n×m) nested loop
- Single-instance guard prevents dual-launch SQLite contention

### v1.4.5 — Performance & Reliability

- Volume slider: fixed drag lag — SVG icon tier caching + debounced settings save
- Language dialog: fixed text truncation
- Settings: fixed not saving on quit — flush debounce timers before shutdown
- Security: API keys moved to build-time injection (no keys in source)

### v1.4.4 — Apple Music & Stability

- Apple Music: eliminated auth popup, fixed first-play stall, ProcessTap audio capture
- Clean shutdown in ~1s — proper WKWebView, DB, scanner teardown
- Sidebar: Library Folders collapsible with scroll support
- Improved uninstaller with admin privileges and Apple Music data cleanup
- Database: fixed connection warnings on quit

### v1.4.3 — Auto-Scan on Folder Add

- Auto-scan now triggers immediately after adding a music folder
- Previously required manual "Scan Now" click after adding folders

### v1.4.2 — Scan & Auth Performance

- Apple Music auth popup now appears on top of main window
- Separate read/write DB connections (WAL concurrent reads)
- Scanner mini-batch commits reduce UI blocking during scan
- Fixed Apple Music beachball when connecting during library scan

### v1.4.1 — Stability & Polish

- Apple Music works in distributed builds (embedded JWT token)
- Faster library scan — adaptive thread count for external drives
- Fixed 6-minute beachball on first play (async queue save)
- Auth popup auto-closes after Apple Music connect
- Smoother scanning — reduced UI reloads during scan
- Fixed font warnings (Sans-serif, SF Pro Display)

### v1.4.0 — Library Performance & Stability

**Library Engine Overhaul**
- SQLite WAL mode + mmap 256MB + 64MB cache for faster DB access
- FTS5 full-text search — instant search across title, artist, album
- HybridTrackModel — lightweight in-memory index (~100 bytes/track) for instant startup, sort, and search even with 100K+ tracks
- String pooling for deduplicated metadata storage
- Batch INSERT with transactions for album/artist rebuild
- Async post-scan reload — no more UI freeze after library scan

**Stability**
- Thread-safe database access (QRecursiveMutex) — fixed potential crash when scanning and searching simultaneously

**Apple Music**
- Disconnect/reconnect functionality for Apple Music integration

See the full changelog at [soranaflow.com/changelog](https://soranaflow.com/changelog).

## Links

- [Website](https://soranaflow.com)
- [Downloads](https://soranaflow.com/downloads)
- [Changelog](https://soranaflow.com/changelog)
- [Report Issue](https://soranaflow.com/support)
- [Privacy Policy](https://soranaflow.com/privacy)

## License

**Proprietary** — Source code is viewable for reference only.
No permission to use, copy, modify, or distribute.
See [LICENSE](LICENSE) for full terms.

Official binaries available at [soranaflow.com](https://soranaflow.com/downloads).

---

<p align="center">
  Made with care for audiophiles
</p>
