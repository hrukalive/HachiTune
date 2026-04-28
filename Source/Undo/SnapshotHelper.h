#pragma once

#include "../Models/Project.h"
#include <vector>

namespace SnapshotHelper
{
  std::vector<float> captureFloatRange(const std::vector<float>& global,
                                       int start, int end);

  std::vector<bool> captureBoolRange(const std::vector<bool>& global,
                                      int start, int end);

  void restoreFloatRange(std::vector<float>& global,
                         int start,
                         const std::vector<float>& snapshot);

  void restoreBoolRange(std::vector<bool>& global,
                        int start,
                        const std::vector<bool>& snapshot);

  /** Refresh note local caches for all notes overlapping [startFrame, endFrame). */
  void refreshNoteCache(Project& project, int startFrame, int endFrame);
} // namespace SnapshotHelper
