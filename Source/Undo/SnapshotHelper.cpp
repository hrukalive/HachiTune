#include "SnapshotHelper.h"
#include <algorithm>

namespace SnapshotHelper
{

std::vector<float> captureFloatRange(const std::vector<float>& global,
                                     int start, int end)
{
  const int total = static_cast<int>(global.size());
  const int clampedStart = std::max(0, start);
  const int clampedEnd = std::min(total, end);
  if (clampedEnd <= clampedStart)
    return {};

  return std::vector<float>(
      global.begin() + clampedStart,
      global.begin() + clampedEnd);
}

std::vector<bool> captureBoolRange(const std::vector<bool>& global,
                                    int start, int end)
{
  const int total = static_cast<int>(global.size());
  const int clampedStart = std::max(0, start);
  const int clampedEnd = std::min(total, end);
  if (clampedEnd <= clampedStart)
    return {};

  return std::vector<bool>(
      global.begin() + clampedStart,
      global.begin() + clampedEnd);
}

void restoreFloatRange(std::vector<float>& global,
                       int start,
                       const std::vector<float>& snapshot)
{
  for (size_t i = 0; i < snapshot.size(); ++i)
  {
    const int gi = start + static_cast<int>(i);
    if (gi >= 0 && gi < static_cast<int>(global.size()))
      global[static_cast<size_t>(gi)] = snapshot[i];
  }
}

void restoreBoolRange(std::vector<bool>& global,
                      int start,
                      const std::vector<bool>& snapshot)
{
  for (size_t i = 0; i < snapshot.size(); ++i)
  {
    const int gi = start + static_cast<int>(i);
    if (gi >= 0 && gi < static_cast<int>(global.size()))
      global[static_cast<size_t>(gi)] = snapshot[i];
  }
}

void refreshNoteCache(Project& project, int startFrame, int endFrame)
{
  project.refreshNoteCachesForRange(startFrame, endFrame);
}

} // namespace SnapshotHelper
