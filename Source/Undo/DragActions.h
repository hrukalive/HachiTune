#pragma once

#include "UndoableAction.h"
#include "SnapshotHelper.h"
#include "../Models/Note.h"
#include <vector>
#include <functional>

/**
 * Action for nudging selected notes by keyboard (up/down, octave).
 */
class MultiNoteMidiNudgeAction : public UndoableAction
{
public:
    MultiNoteMidiNudgeAction(std::vector<Note *> notes,
                             std::vector<float> oldOffsets,
                             std::vector<float> newOffsets,
                             std::function<void(const std::vector<Note *> &)> onNotesChanged = nullptr)
        : notes(std::move(notes)),
          oldOffsets(std::move(oldOffsets)),
          newOffsets(std::move(newOffsets)),
          onNotesChanged(std::move(onNotesChanged)) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size() && i < oldOffsets.size(); ++i)
        {
            if (!notes[i])
                continue;
            notes[i]->setPitchOffset(oldOffsets[i]);
            notes[i]->markDirty();
            notes[i]->markSynthDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size() && i < newOffsets.size(); ++i)
        {
            if (!notes[i])
                continue;
            notes[i]->setPitchOffset(newOffsets[i]);
            notes[i]->markDirty();
            notes[i]->markSynthDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    juce::String getName() const override { return "Nudge Note Pitch"; }

private:
    std::vector<Note *> notes;
    std::vector<float> oldOffsets;
    std::vector<float> newOffsets;
    std::function<void(const std::vector<Note *> &)> onNotesChanged;
};

class NotePitchDragAction : public UndoableAction
{
public:
  NotePitchDragAction(Project& project,
                              int noteIndex,
                              float oldOffset, float newOffset,
                              int startFrame, int endFrame,
                              std::vector<float> beforeF0,
                              std::vector<float> afterF0,
                              std::vector<float> beforeBasePitch,
                              std::vector<float> afterBasePitch,
                              std::function<void()> onChanged = nullptr)
      : project(project),
        noteIndex(noteIndex),
        oldOffset(oldOffset), newOffset(newOffset),
        startFrame(startFrame), endFrame(endFrame),
        beforeF0(std::move(beforeF0)), afterF0(std::move(afterF0)),
        beforeBasePitch(std::move(beforeBasePitch)),
        afterBasePitch(std::move(afterBasePitch)),
        onChanged(std::move(onChanged)) {}

  void undo() override
  {
    auto& editedData = project.getEditedData();
    SnapshotHelper::restoreFloatRange(editedData.f0, startFrame, beforeF0);
    SnapshotHelper::restoreFloatRange(editedData.basePitch, startFrame, beforeBasePitch);
    auto& notes = project.getNotes();
    if (noteIndex >= 0 && noteIndex < static_cast<int>(notes.size()))
    {
      notes[noteIndex].setPitchOffset(oldOffset);
      notes[noteIndex].markDirty();
      notes[noteIndex].markSynthDirty();
    }
    SnapshotHelper::refreshNoteCache(project, startFrame, endFrame);
    if (onChanged)
      onChanged();
  }

  void redo() override
  {
    auto& editedData = project.getEditedData();
    SnapshotHelper::restoreFloatRange(editedData.f0, startFrame, afterF0);
    SnapshotHelper::restoreFloatRange(editedData.basePitch, startFrame, afterBasePitch);
    auto& notes = project.getNotes();
    if (noteIndex >= 0 && noteIndex < static_cast<int>(notes.size()))
    {
      notes[noteIndex].setPitchOffset(newOffset);
      notes[noteIndex].markDirty();
      notes[noteIndex].markSynthDirty();
    }
    SnapshotHelper::refreshNoteCache(project, startFrame, endFrame);
    if (onChanged)
      onChanged();
  }

  juce::String getName() const override { return "Drag Note Pitch"; }

private:
  Project& project;
  int noteIndex;
  float oldOffset;
  float newOffset;
  int startFrame;
  int endFrame;
  std::vector<float> beforeF0;
  std::vector<float> afterF0;
  std::vector<float> beforeBasePitch;
  std::vector<float> afterBasePitch;
  std::function<void()> onChanged;
};

class MultiNotePitchDragAction : public UndoableAction
{
public:
  MultiNotePitchDragAction(Project& project,
                                   std::vector<int> noteIndices,
                                   std::vector<float> oldOffsets,
                                   std::vector<float> newOffsets,
                                   int startFrame, int endFrame,
                                   std::vector<float> beforeF0,
                                   std::vector<float> afterF0,
                                   std::vector<float> beforeBasePitch,
                                   std::vector<float> afterBasePitch,
                                   std::function<void()> onChanged = nullptr)
      : project(project),
        noteIndices(std::move(noteIndices)),
        oldOffsets(std::move(oldOffsets)),
        newOffsets(std::move(newOffsets)),
        startFrame(startFrame), endFrame(endFrame),
        beforeF0(std::move(beforeF0)), afterF0(std::move(afterF0)),
        beforeBasePitch(std::move(beforeBasePitch)),
        afterBasePitch(std::move(afterBasePitch)),
        onChanged(std::move(onChanged)) {}

  void undo() override
  {
    auto& editedData = project.getEditedData();
    SnapshotHelper::restoreFloatRange(editedData.f0, startFrame, beforeF0);
    SnapshotHelper::restoreFloatRange(editedData.basePitch, startFrame, beforeBasePitch);
    auto& notes = project.getNotes();
    for (size_t i = 0; i < noteIndices.size() && i < oldOffsets.size(); ++i)
    {
      int idx = noteIndices[i];
      if (idx >= 0 && idx < static_cast<int>(notes.size()))
      {
        notes[idx].setPitchOffset(oldOffsets[i]);
        notes[idx].markDirty();
        notes[idx].markSynthDirty();
      }
    }
    SnapshotHelper::refreshNoteCache(project, startFrame, endFrame);
    if (onChanged)
      onChanged();
  }

  void redo() override
  {
    auto& editedData = project.getEditedData();
    SnapshotHelper::restoreFloatRange(editedData.f0, startFrame, afterF0);
    SnapshotHelper::restoreFloatRange(editedData.basePitch, startFrame, afterBasePitch);
    auto& notes = project.getNotes();
    for (size_t i = 0; i < noteIndices.size() && i < newOffsets.size(); ++i)
    {
      int idx = noteIndices[i];
      if (idx >= 0 && idx < static_cast<int>(notes.size()))
      {
        notes[idx].setPitchOffset(newOffsets[i]);
        notes[idx].markDirty();
        notes[idx].markSynthDirty();
      }
    }
    SnapshotHelper::refreshNoteCache(project, startFrame, endFrame);
    if (onChanged)
      onChanged();
  }

  juce::String getName() const override { return "Drag Multiple Notes"; }

private:
  Project& project;
  std::vector<int> noteIndices;
  std::vector<float> oldOffsets;
  std::vector<float> newOffsets;
  int startFrame;
  int endFrame;
  std::vector<float> beforeF0;
  std::vector<float> afterF0;
  std::vector<float> beforeBasePitch;
  std::vector<float> afterBasePitch;
  std::function<void()> onChanged;
};
