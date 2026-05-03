#pragma once

#include <vector>

/**
 * Global edit state — the single source of truth for all editable
 * per-frame data. Serialized to the project file.
 * Synthesis reads ONLY from this struct.
 */
struct EditedData
{
  std::vector<float> basePitch;       // [T] MIDI, affected by note drag
  std::vector<float> deltaPitch;      // [T] per-frame deviation (semitones)
  std::vector<float> tunedF0;         // source/non-stretched [T] Hz
  std::vector<float> f0;              // [T] Hz, composed: midiToFreq(basePitch + deltaPitch)
  std::vector<bool>  voicedMask;      // [T] voiced/unvoiced
  std::vector<bool>  vadMask;         // [T] energy-based VAD
  std::vector<float> voicingCurve;    // [T] 0..max (default 100)
  std::vector<float> breathCurve;     // [T] 0..max (default 100)
  std::vector<float> tensionCurve;    // [T] -100..100 (default 0)
  std::vector<float> baseVoicing;     // source/non-stretched [T], default 100
  std::vector<float> baseBreath;      // source/non-stretched [T], default 100
  std::vector<float> baseTension;     // source/non-stretched [T], default 0
  std::vector<float> adjustedSTFT;    // source/non-stretched interleaved STFT cache
  std::vector<std::vector<float>> adjustedMel; // source/non-stretched [T, NUM_MELS]
  std::vector<std::vector<float>> mel;         // output timeline [T, NUM_MELS]

  int getNumFrames() const
  {
    return static_cast<int>(f0.size());
  }

  bool isEmpty() const { return f0.empty(); }

  void clear()
  {
    basePitch.clear();
    deltaPitch.clear();
    tunedF0.clear();
    f0.clear();
    voicedMask.clear();
    vadMask.clear();
    voicingCurve.clear();
    breathCurve.clear();
    tensionCurve.clear();
    baseVoicing.clear();
    baseBreath.clear();
    baseTension.clear();
    adjustedSTFT.clear();
    adjustedMel.clear();
    mel.clear();
  }

  /** Resize all arrays to numFrames, preserving existing data. */
  void resize(int numFrames)
  {
    auto n = static_cast<size_t>(numFrames);
    basePitch.resize(n, 0.0f);
    deltaPitch.resize(n, 0.0f);
    tunedF0.resize(n, 0.0f);
    f0.resize(n, 0.0f);
    voicedMask.resize(n, false);
    vadMask.resize(n, false);
    voicingCurve.resize(n, 100.0f);
    breathCurve.resize(n, 100.0f);
    tensionCurve.resize(n, 0.0f);
    baseVoicing.resize(n, 100.0f);
    baseBreath.resize(n, 100.0f);
    baseTension.resize(n, 0.0f);
    adjustedMel.resize(n);
    mel.resize(n);
  }
};
