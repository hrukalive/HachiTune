#pragma once

#include "UndoableAction.h"
#include "SnapshotHelper.h"
#include <vector>
#include <functional>
#include <limits>

class F0DrawAction : public UndoableAction
{
public:
  F0DrawAction(Project& project,
               int startFrame, int endFrame,
               std::vector<float> beforeF0,
               std::vector<float> afterF0,
               std::vector<float> beforeDelta,
               std::vector<float> afterDelta,
               std::vector<bool> beforeVoiced,
               std::vector<bool> afterVoiced,
               std::vector<bool> beforeEdited,
               std::vector<bool> afterEdited,
               std::function<void(int, int)> onChanged = nullptr)
      : project(project),
        startFrame(startFrame), endFrame(endFrame),
        beforeF0(std::move(beforeF0)), afterF0(std::move(afterF0)),
        beforeDelta(std::move(beforeDelta)), afterDelta(std::move(afterDelta)),
        beforeVoiced(std::move(beforeVoiced)), afterVoiced(std::move(afterVoiced)),
        beforeEdited(std::move(beforeEdited)), afterEdited(std::move(afterEdited)),
        onChanged(std::move(onChanged)) {}

  void undo() override
  {
    auto& editedData = project.getEditedData();
    SnapshotHelper::restoreFloatRange(editedData.f0, startFrame, beforeF0);
    SnapshotHelper::restoreFloatRange(editedData.deltaPitch, startFrame, beforeDelta);
    SnapshotHelper::restoreBoolRange(editedData.voicedMask, startFrame, beforeVoiced);
    SnapshotHelper::refreshNoteCache(project, startFrame, endFrame);
    if (onChanged)
      onChanged(startFrame, endFrame);
  }

  void redo() override
  {
    auto& editedData = project.getEditedData();
    SnapshotHelper::restoreFloatRange(editedData.f0, startFrame, afterF0);
    SnapshotHelper::restoreFloatRange(editedData.deltaPitch, startFrame, afterDelta);
    SnapshotHelper::restoreBoolRange(editedData.voicedMask, startFrame, afterVoiced);
    SnapshotHelper::refreshNoteCache(project, startFrame, endFrame);
    if (onChanged)
      onChanged(startFrame, endFrame);
  }

  juce::String getName() const override { return "Edit Pitch Curve"; }

private:
  Project& project;
  int startFrame;
  int endFrame;
  std::vector<float> beforeF0;
  std::vector<float> afterF0;
  std::vector<float> beforeDelta;
  std::vector<float> afterDelta;
  std::vector<bool> beforeVoiced;
  std::vector<bool> afterVoiced;
  std::vector<bool> beforeEdited;
  std::vector<bool> afterEdited;
  std::function<void(int, int)> onChanged;
};
