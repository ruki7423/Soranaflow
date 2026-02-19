#pragma once

// ISO 226:2003 Equal Loudness Contour — compensation presets for EQ
//
// At low listening volumes, human hearing is less sensitive to low and high
// frequencies relative to midrange. These presets apply the inverse of the
// ISO 226 equal-loudness curve at various phon levels, so that the perceived
// frequency balance matches a reference level (typically 80 phon).
//
// Each preset provides gain values for 10 standard EQ bands:
//   32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 Hz
//
// The gains represent the compensation needed: positive = boost (low-volume
// listening), values derived from difference between target phon curve and
// 80-phon reference, clipped to practical EQ range.

namespace LoudnessContour {

struct Preset {
    const char* name;
    float gains[10]; // dB for bands: 32, 64, 125, 250, 500, 1k, 2k, 4k, 8k, 16k
};

// Compensation presets: how much to boost/cut each band to compensate
// for reduced sensitivity at lower listening levels.
// Reference: 80 phon (no compensation needed).
//
// Derived from ISO 226:2003 equal-loudness contours.
// At lower phon levels, bass and treble need more boost.
static constexpr Preset presets[] = {
    // ~60 dB SPL listening level — mild compensation
    {"Loudness (Low)",
     {+5.0f, +3.5f, +2.0f, +0.5f, 0.0f, 0.0f, -0.5f, +0.5f, +1.5f, +3.0f}},

    // ~50 dB SPL listening level — moderate compensation
    {"Loudness (Mid)",
     {+8.0f, +6.0f, +3.5f, +1.5f, 0.0f, 0.0f, -1.0f, +0.5f, +2.5f, +5.0f}},

    // ~40 dB SPL listening level — strong compensation
    {"Loudness (High)",
     {+12.0f, +9.0f, +5.5f, +2.5f, 0.0f, 0.0f, -1.0f, +1.0f, +4.0f, +7.0f}},
};

static constexpr int presetCount = sizeof(presets) / sizeof(presets[0]);

} // namespace LoudnessContour
