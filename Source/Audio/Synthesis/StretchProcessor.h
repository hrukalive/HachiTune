#pragma once

#include "../../Models/Project.h"
#include "../../Models/EditedData.h"
#include <vector>
#include <climits>

/**
 * Pure algorithm for time-stretching operations.
 * Extracted from StretchHandler (which retains UI interaction only).
 * All methods are static — no state.
 */
class StretchProcessor
{
public:
  /**
   * Stretch mel spectrogram using warp markers (linear interpolation).
   * Returns new mel with length = markers.back().outputFrame.
   */
  static std::vector<std::vector<float>> stretchMel(
      const std::vector<std::vector<float>>& mel,
      const std::vector<Project::WarpMarker>& markers);

  /**
   * Build an output-timeline mel spectrogram from source-timeline mel.
   */
  static std::vector<std::vector<float>> buildOutputMel(
      const std::vector<std::vector<float>>& sourceMel,
      const std::vector<Project::WarpMarker>& warpMap,
      int outputFrameCount);

  /**
   * Stretch all arrays in editedData using warp markers.
   *   basePitch/masks -> nearest neighbor
   *   deltaPitch/curves -> linear interpolation
   *   Recomputes f0 from basePitch + deltaPitch.
   */
  static void stretchEditedData(
      EditedData& edited,
      const std::vector<Project::WarpMarker>& markers,
      int newTotalFrames);

  /**
   * Remap note startFrame/endFrame based on warp markers.
   */
  static void remapNoteFrames(
      std::vector<Note>& notes,
      const std::vector<Project::WarpMarker>& markers,
      int affectedSourceStart = 0,
      int affectedSourceEnd = INT_MAX);

  /** Map a source frame to an output frame (linear interpolation). */
  static float mapFrame(const std::vector<Project::WarpMarker>& markers,
                        float sourceFrame);

  /** Map an output frame back to a source frame (linear interpolation). */
  static float inverseMapFrame(const std::vector<Project::WarpMarker>& markers,
                               float outputFrame);
};
