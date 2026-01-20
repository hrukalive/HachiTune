#pragma once

#include <cmath>

// Audio constants
constexpr int SAMPLE_RATE = 44100;
constexpr int HOP_SIZE = 512;
constexpr int WIN_SIZE = 2048;
constexpr int N_FFT = 2048;
constexpr int NUM_MELS = 128;
constexpr float FMIN = 40.0f;
constexpr float FMAX = 16000.0f;

// MIDI constants
constexpr int MIN_MIDI_NOTE = 24; // C1
constexpr int MAX_MIDI_NOTE = 96; // C7
constexpr int MIDI_A4 = 69;
constexpr float FREQ_A4 = 440.0f;

// UI constants
constexpr float DEFAULT_PIXELS_PER_SECOND = 100.0f;
constexpr float DEFAULT_PIXELS_PER_SEMITONE = 45.0f;
constexpr float MIN_PIXELS_PER_SECOND = 20.0f;
constexpr float MAX_PIXELS_PER_SECOND = 500.0f;
constexpr float MIN_PIXELS_PER_SEMITONE = 8.0f;
constexpr float MAX_PIXELS_PER_SEMITONE = 120.0f;

// Colors (ARGB format) - Modern dark theme
// Using APP_ prefix to avoid Windows macro conflicts
constexpr unsigned int APP_COLOR_BACKGROUND = 0xFF2A2A35u;  // Dark gray-blue
constexpr unsigned int APP_COLOR_GRID = 0xFF3A3A45u;        // Subtle grid
constexpr unsigned int APP_COLOR_GRID_BAR = 0xFF4A4A55u;    // Bar lines
constexpr unsigned int APP_COLOR_PITCH_CURVE = 0xFFFFFFFFu; // White pitch curve
constexpr unsigned int APP_COLOR_NOTE_NORMAL = 0xFF6B5BFFu; // Blue-purple (like reference)
constexpr unsigned int APP_COLOR_NOTE_SELECTED = 0xFF8B7BFFu; // Lighter purple when selected
constexpr unsigned int APP_COLOR_NOTE_HOVER = 0xFF7B6BFFu; // Hover state
constexpr unsigned int APP_COLOR_PRIMARY = 0xFF6B5BFFu;    // Primary accent
constexpr unsigned int APP_COLOR_WAVEFORM = 0xFF353540u; // Background waveform (very subtle)

// Utility functions
inline float midiToFreq(float midi) {
  return FREQ_A4 * std::pow(2.0f, (midi - MIDI_A4) / 12.0f);
}

inline float freqToMidi(float freq) {
  if (freq <= 0.0f)
    return 0.0f;
  return 12.0f * std::log2(freq / FREQ_A4) + MIDI_A4;
}

inline int secondsToFrames(float seconds) {
  return static_cast<int>(seconds * SAMPLE_RATE / HOP_SIZE);
}

inline float framesToSeconds(int frames) {
  return static_cast<float>(frames) * HOP_SIZE / SAMPLE_RATE;
}
