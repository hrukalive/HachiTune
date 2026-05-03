#pragma once

#include <vector>

/**
 * Immutable analysis results. Set once during pitch detection,
 * never modified after analysis completes. Used as the
 * "original" baseline for resets.
 */
struct AnalysisData
{
  std::vector<float> originalF0;          // [T] Hz
  std::vector<float> originalPitch;       // [T] MIDI note number
  std::vector<float> originalDeltaPitch;  // [T] semitone deviation from base
  std::vector<bool>  originalVoicedMask;  // [T] true = voiced
  std::vector<bool>  originalVADMask;     // [T] true = has audio energy
  std::vector<std::vector<float>> originalMel; // source timeline [T, NUM_MELS]

  struct NoteSegment {
    int srcStartFrame = 0;
    int srcEndFrame = 0;
  };
  std::vector<NoteSegment> noteSegments;

  int getNumFrames() const
  {
    return static_cast<int>(originalF0.size());
  }

  bool isEmpty() const { return originalF0.empty(); }

  void clear()
  {
    originalF0.clear();
    originalPitch.clear();
    originalDeltaPitch.clear();
    originalVoicedMask.clear();
    originalVADMask.clear();
    originalMel.clear();
    noteSegments.clear();
  }
};
