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

**Download** the latest DMG from [Releases](https://github.com/ruki7423/Soranaflow/releases/latest) or [soranaflow.com/downloads](https://soranaflow.com/downloads).

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
| GitHub Releases | 7 (v1.0.0 → v1.3.1) |
| Total Downloads | ![Downloads](https://img.shields.io/github/downloads/ruki7423/Soranaflow/total?style=flat-square&label=) |
| Latest Release | ![Latest](https://img.shields.io/github/downloads/ruki7423/Soranaflow/latest/total?style=flat-square&label=) |
| Stars | ![Stars](https://img.shields.io/github/stars/ruki7423/Soranaflow?style=flat-square&label=) |
| Commits | ![Commits](https://img.shields.io/github/commit-activity/m/ruki7423/Soranaflow?style=flat-square&label=) |

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
