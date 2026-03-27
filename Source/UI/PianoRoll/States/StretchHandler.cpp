#include "StretchHandler.h"

#if HACHITUNE_ENABLE_STRETCH

#include "../../PianoRollComponent.h"
#include "../../../Undo/TimingActions.h"
#include "../../../Utils/WarpMarkerProcessor.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr float kMarkerDeactivateZoneFraction = 0.20f;

bool markersEqual(const std::vector<Project::WarpMarker>& a,
                  const std::vector<Project::WarpMarker>& b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (a[i].sourceFrame != b[i].sourceFrame ||
        a[i].outputFrame != b[i].outputFrame)
    {
      return false;
    }
  }
  return true;
}

int findMarkerIndex(const std::vector<Project::WarpMarker>& markers, int sourceFrame)
{
  for (size_t i = 0; i < markers.size(); ++i)
  {
    if (markers[i].sourceFrame == sourceFrame)
      return static_cast<int>(i);
  }
  return -1;
}

void refreshDirtyRanges(Project* project)
{
  if (!project)
    return;

  const auto dirtyRange = project->getDirtyFrameRange();
  if (dirtyRange.first >= 0 && dirtyRange.second > dirtyRange.first)
  {
    const int f0Size = static_cast<int>(project->getAudioData().f0.size());
    project->setF0DirtyRange(std::max(0, dirtyRange.first - 60),
                             std::min(f0Size, dirtyRange.second + 60));
    project->setParamDirtyRange(dirtyRange.first, dirtyRange.second);
  }
  else
  {
    project->clearF0DirtyRange();
    project->clearParamDirtyRange();
  }
}
} // namespace

StretchHandler::StretchHandler(PianoRollComponent &owner)
    : InteractionHandler(owner)
{
}

bool StretchHandler::mouseDown(const juce::MouseEvent &e, float worldX,
                               float worldY)
{
  auto *project = owner_.project;
  if (!project)
    return false;

  const int boundaryIndex =
      findStretchBoundaryIndex(worldX, stretchHandleHitPadding);
  if (boundaryIndex >= 0)
  {
    auto boundaries = collectStretchBoundaries();
    if (boundaryIndex < static_cast<int>(boundaries.size()))
    {
      const auto &boundary = boundaries[static_cast<size_t>(boundaryIndex)];
      const float contentBottom =
          static_cast<float>(PianoRollComponent::headerHeight +
                             owner_.getVisibleContentHeight());
      const float deactivateZoneTop =
          contentBottom * (1.0f - kMarkerDeactivateZoneFraction) +
          static_cast<float>(PianoRollComponent::headerHeight) *
              kMarkerDeactivateZoneFraction;
      const bool clickedDeactivateZone =
          e.y >= deactivateZoneTop && e.y < contentBottom;
      if (boundary.active &&
          (e.mods.isShiftDown() || clickedDeactivateZone))
        return deactivateMarker(boundary);

      startStretchDrag(boundary);
      owner_.repaint();
      return true;
    }
  }

  Note *note = owner_.findNoteAt(worldX, worldY);
  if (note)
  {
    project->deselectAllNotes();
    note->setSelected(true);
    owner_.updatePitchToolHandlesFromSelection();
    if (owner_.onNoteSelected)
      owner_.onNoteSelected(note);
    owner_.repaint();
    return true;
  }

  project->deselectAllNotes();
  owner_.updatePitchToolHandlesFromSelection();
  owner_.boxSelector->startSelection(worldX, worldY);
  owner_.repaint();
  return true;
}

bool StretchHandler::mouseDrag(const juce::MouseEvent &e, float worldX,
                               float worldY)
{
  juce::ignoreUnused(e, worldY);
  if (!stretchDrag.active || !owner_.project)
    return false;

  const double time = owner_.xToTime(worldX);
  const int targetFrame =
      static_cast<int>(secondsToFrames(static_cast<float>(time)));
  updateStretchDrag(targetFrame);

  const auto now = juce::Time::currentTimeMillis();
  const bool shouldRepaint =
      (now - owner_.lastDragRepaintTime) >=
      PianoRollComponent::minDragRepaintInterval;
  if (shouldRepaint)
  {
    owner_.repaint();
    owner_.lastDragRepaintTime = now;
  }
  return true;
}

bool StretchHandler::mouseUp(const juce::MouseEvent &e, float worldX,
                             float worldY)
{
  juce::ignoreUnused(e, worldX, worldY);
  if (!stretchDrag.active)
    return false;

  finishStretchDrag();
  owner_.repaint();
  return true;
}

void StretchHandler::mouseMove(const juce::MouseEvent &e, float worldX,
                               float worldY)
{
  juce::ignoreUnused(worldY);
  int newHoveredIndex = -1;
  if (e.y >= PianoRollComponent::headerHeight &&
      e.x >= PianoRollComponent::pianoKeysWidth)
  {
    newHoveredIndex = findStretchBoundaryIndex(worldX, stretchHandleHitPadding);
    owner_.setMouseCursor(newHoveredIndex >= 0
                              ? juce::MouseCursor::LeftRightResizeCursor
                              : juce::MouseCursor::NormalCursor);
  }
  else
  {
    owner_.setMouseCursor(juce::MouseCursor::NormalCursor);
  }

  if (newHoveredIndex != hoveredStretchBoundaryIndex)
  {
    hoveredStretchBoundaryIndex = newHoveredIndex;
    owner_.repaint();
  }
}

void StretchHandler::draw(juce::Graphics &g)
{
  juce::ignoreUnused(g);
}

bool StretchHandler::isActive() const { return stretchDrag.active; }

void StretchHandler::cancel() { cancelStretchDrag(); }

void StretchHandler::invalidateBoundaryCache()
{
  boundaryCacheDirty = true;
}

std::vector<StretchHandler::StretchBoundary>
StretchHandler::collectStretchBoundaries()
{
  auto *project = owner_.project;
  if (!project)
  {
    cachedBoundaries.clear();
    boundaryCacheDirty = false;
    return {};
  }

  if (boundaryCacheDirty)
  {
    cachedBoundaries.clear();
    auto boundaries = WarpMarkerProcessor::collectBoundaries(*project);
    const int totalFrames = WarpMarkerProcessor::getSourceFrameLimit(*project);
    cachedBoundaries.reserve(boundaries.size());

    for (const auto &boundary : boundaries)
    {
      if (boundary.sourceFrame <= 0 || boundary.sourceFrame >= totalFrames)
        continue;

      StretchBoundary converted;
      converted.left = boundary.left;
      converted.right = boundary.right;
      converted.frame = boundary.currentFrame;
      converted.sourceFrame = boundary.sourceFrame;
      converted.active = boundary.active;
      cachedBoundaries.push_back(converted);
    }

    std::sort(cachedBoundaries.begin(), cachedBoundaries.end(),
              [](const auto &a, const auto &b)
              {
                if (a.frame != b.frame)
                  return a.frame < b.frame;
                return a.sourceFrame < b.sourceFrame;
              });
    boundaryCacheDirty = false;
  }

  auto result = cachedBoundaries;
  if (stretchDrag.active)
  {
    for (auto &boundary : result)
    {
      if (boundary.sourceFrame == stretchDrag.boundary.sourceFrame)
      {
        boundary.frame = stretchDrag.currentBoundary;
        boundary.active = true;
        break;
      }
    }

    std::sort(result.begin(), result.end(),
              [](const auto &a, const auto &b)
              {
                if (a.frame != b.frame)
                  return a.frame < b.frame;
                return a.sourceFrame < b.sourceFrame;
              });
  }

  return result;
}

int StretchHandler::findStretchBoundaryIndex(float worldX,
                                             float tolerancePx)
{
  const auto boundaries = collectStretchBoundaries();
  if (boundaries.empty())
    return -1;

  const int targetFrame =
      static_cast<int>(secondsToFrames(static_cast<float>(owner_.xToTime(worldX))));
  const auto it = std::lower_bound(
      boundaries.begin(), boundaries.end(), targetFrame,
      [](const StretchBoundary &boundary, int frame)
      { return boundary.frame < frame; });

  int bestIndex = -1;
  float bestDistance = tolerancePx;
  auto consider = [&](int index)
  {
    if (index < 0 || index >= static_cast<int>(boundaries.size()))
      return;

    const float boundaryX =
        framesToSeconds(boundaries[static_cast<size_t>(index)].frame) *
        owner_.pixelsPerSecond;
    const float distance = std::abs(worldX - boundaryX);
    if (distance <= bestDistance)
    {
      bestDistance = distance;
      bestIndex = index;
    }
  };

  if (it != boundaries.end())
    consider(static_cast<int>(std::distance(boundaries.begin(), it)));
  if (it != boundaries.begin())
    consider(static_cast<int>(std::distance(boundaries.begin(), it) - 1));

  return bestIndex;
}

void StretchHandler::startStretchDrag(const StretchBoundary &boundary)
{
  auto *project = owner_.project;
  if (!project)
    return;

  stretchDrag = {};
  stretchDrag.active = true;
  stretchDrag.boundary = boundary;
  stretchDrag.currentBoundary = boundary.frame;
  stretchDrag.originalMarkers =
      WarpMarkerProcessor::normalizeMarkers(*project, project->getWarpMarkers());
  stretchDrag.previewMarkers = stretchDrag.originalMarkers;

  const int existingIndex =
      findMarkerIndex(stretchDrag.previewMarkers, boundary.sourceFrame);
  if (existingIndex >= 0)
  {
    stretchDrag.previewMarkers[static_cast<size_t>(existingIndex)].outputFrame =
        boundary.frame;
  }
  else
  {
    stretchDrag.previewMarkers.push_back(
        {boundary.sourceFrame, boundary.frame});
  }
  stretchDrag.previewMarkers =
      WarpMarkerProcessor::normalizeMarkers(*project, stretchDrag.previewMarkers);

  const int markerIndex =
      findMarkerIndex(stretchDrag.previewMarkers, boundary.sourceFrame);
  if (markerIndex < 0)
  {
    stretchDrag = {};
    return;
  }

  const int totalFrames = WarpMarkerProcessor::getSourceFrameLimit(*project);
  const int prevSource = markerIndex > 0
                             ? stretchDrag.previewMarkers[static_cast<size_t>(markerIndex - 1)].sourceFrame
                             : 0;
  const int prevOutput = markerIndex > 0
                             ? stretchDrag.previewMarkers[static_cast<size_t>(markerIndex - 1)].outputFrame
                             : 0;
  const int nextSource =
      markerIndex + 1 < static_cast<int>(stretchDrag.previewMarkers.size())
          ? stretchDrag.previewMarkers[static_cast<size_t>(markerIndex + 1)].sourceFrame
          : totalFrames;
  const int nextOutput =
      markerIndex + 1 < static_cast<int>(stretchDrag.previewMarkers.size())
          ? stretchDrag.previewMarkers[static_cast<size_t>(markerIndex + 1)].outputFrame
          : totalFrames;

  stretchDrag.minFrame =
      prevOutput +
      WarpMarkerProcessor::computeSegmentMinimumOutputSpan(
          *project, prevSource, boundary.sourceFrame, minStretchNoteFrames);
  stretchDrag.maxFrame =
      nextOutput -
      WarpMarkerProcessor::computeSegmentMinimumOutputSpan(
          *project, boundary.sourceFrame, nextSource, minStretchNoteFrames);
  stretchDrag.minFrame = std::clamp(stretchDrag.minFrame, 0, totalFrames);
  stretchDrag.maxFrame = std::clamp(stretchDrag.maxFrame, 0, totalFrames);
  if (stretchDrag.maxFrame < stretchDrag.minFrame)
    stretchDrag.maxFrame = stretchDrag.minFrame;
}

void StretchHandler::updateStretchDrag(int targetFrame)
{
  auto *project = owner_.project;
  if (!stretchDrag.active || !project)
    return;

  const int clampedTarget =
      juce::jlimit(stretchDrag.minFrame, stretchDrag.maxFrame, targetFrame);
  if (clampedTarget == stretchDrag.currentBoundary)
    return;

  stretchDrag.currentBoundary = clampedTarget;
  stretchDrag.changed = true;

  const int markerIndex =
      findMarkerIndex(stretchDrag.previewMarkers, stretchDrag.boundary.sourceFrame);
  if (markerIndex < 0)
    return;

  stretchDrag.previewMarkers[static_cast<size_t>(markerIndex)].outputFrame =
      clampedTarget;
  stretchDrag.previewMarkers =
      WarpMarkerProcessor::normalizeMarkers(*project, stretchDrag.previewMarkers);
  applyMarkers(stretchDrag.previewMarkers, false);
}

void StretchHandler::applyMarkers(const std::vector<Project::WarpMarker> &markers,
                                  bool updateProjectMarkers)
{
  auto *project = owner_.project;
  if (!project)
    return;

  WarpMarkerProcessor::recomputeFromMarkers(*project, markers,
                                            updateProjectMarkers);
  invalidateBoundaryCache();
  owner_.invalidateBasePitchCache();
  owner_.invalidateWaveformCache();

  if (updateProjectMarkers)
    updateDirtyRanges();

  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
}

void StretchHandler::updateDirtyRanges()
{
  refreshDirtyRanges(owner_.project);
}

bool StretchHandler::deactivateMarker(const StretchBoundary &boundary)
{
  auto *project = owner_.project;
  if (!project)
    return false;

  auto oldMarkers =
      WarpMarkerProcessor::normalizeMarkers(*project, project->getWarpMarkers());
  auto newMarkers = oldMarkers;
  newMarkers.erase(std::remove_if(newMarkers.begin(), newMarkers.end(),
                                  [&boundary](const auto &marker)
                                  {
                                    return marker.sourceFrame ==
                                           boundary.sourceFrame;
                                  }),
                   newMarkers.end());
  newMarkers = WarpMarkerProcessor::normalizeMarkers(*project, newMarkers);

  if (markersEqual(oldMarkers, newMarkers))
    return false;

  applyMarkers(newMarkers, true);

  if (owner_.undoManager)
  {
    auto *ownerPtr = &owner_;
    auto action = std::make_unique<WarpMarkerStateAction>(
        project, oldMarkers, newMarkers,
        [ownerPtr](const std::vector<Project::WarpMarker> &markers)
        {
          if (!ownerPtr->project)
            return;
          WarpMarkerProcessor::recomputeFromMarkers(*ownerPtr->project, markers,
                                                    true);
          ownerPtr->invalidateBasePitchCache();
          ownerPtr->invalidateWaveformCache();
          refreshDirtyRanges(ownerPtr->project);
        });
    owner_.undoManager->addAction(std::move(action));
  }

  if (owner_.onPitchEditFinished)
    owner_.onPitchEditFinished();

  owner_.repaint();
  return true;
}

void StretchHandler::finishStretchDrag()
{
  auto *project = owner_.project;
  if (!stretchDrag.active || !project)
  {
    stretchDrag = {};
    return;
  }

  auto oldMarkers = WarpMarkerProcessor::normalizeMarkers(
      *project, stretchDrag.originalMarkers);
  auto newMarkers = WarpMarkerProcessor::normalizeMarkers(
      *project, stretchDrag.previewMarkers);

  if (!markersEqual(oldMarkers, newMarkers))
  {
    applyMarkers(newMarkers, true);

    if (owner_.undoManager)
    {
      auto *ownerPtr = &owner_;
      auto action = std::make_unique<WarpMarkerStateAction>(
          project, oldMarkers, newMarkers,
          [ownerPtr](const std::vector<Project::WarpMarker> &markers)
          {
            if (!ownerPtr->project)
              return;
            WarpMarkerProcessor::recomputeFromMarkers(*ownerPtr->project, markers,
                                                      true);
            ownerPtr->invalidateBasePitchCache();
            ownerPtr->invalidateWaveformCache();
            refreshDirtyRanges(ownerPtr->project);
          });
      owner_.undoManager->addAction(std::move(action));
    }

    if (owner_.onPitchEditFinished)
      owner_.onPitchEditFinished();
  }
  else
  {
    applyMarkers(oldMarkers, false);
  }

  stretchDrag = {};
}

void StretchHandler::cancelStretchDrag()
{
  auto *project = owner_.project;
  if (!stretchDrag.active || !project)
  {
    stretchDrag = {};
    return;
  }

  applyMarkers(stretchDrag.originalMarkers, false);
  stretchDrag = {};
}

#endif // HACHITUNE_ENABLE_STRETCH
