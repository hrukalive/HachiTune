# Foundations & pitchOffset Semantic Unification — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix notes ordering, monitor mel/f0 alignment, tension save/load, and unify pitchOffset semantics so midiNote is never mutated after analysis.

**Architecture:** Four independent changes applied sequentially. Notes ordering is foundational. pitchOffset unification touches ~10 files but the pattern is mechanical: every `setMidiNote(x)` in a drag/nudge/snap path becomes `setPitchOffset(x - midiNote)`.

**Tech Stack:** C++17, JUCE framework, CMake build system. No test infrastructure.

---

### Task 1: Sorted notes — `addNote`, `sortNotes`, `getNoteAtFrame`, `getNotesInRange`

**Files:**
- Modify: `Source/Models/Project.h:230-237`
- Modify: `Source/Models/Project.cpp:207-226`

- [ ] **Step 1: Change `addNote` to sorted insert and add `sortNotes`**

In `Source/Models/Project.h`, replace the inline `addNote` and add `sortNotes`:

```cpp
// Notes
std::vector<Note> &getNotes() { return notes; }
const std::vector<Note> &getNotes() const { return notes; }
void addNote(Note note);
void sortNotes();
void clearNotes() { notes.clear(); }
```

In `Source/Models/Project.cpp`, add the implementations (insert above `getNoteAtFrame`):

```cpp
void Project::addNote(Note note)
{
  auto it = std::lower_bound(notes.begin(), notes.end(), note,
      [](const Note& a, const Note& b)
      {
        if (a.getStartFrame() != b.getStartFrame())
          return a.getStartFrame() < b.getStartFrame();
        return a.getEndFrame() < b.getEndFrame();
      });
  notes.insert(it, std::move(note));
}

void Project::sortNotes()
{
  std::sort(notes.begin(), notes.end(),
      [](const Note& a, const Note& b)
      {
        if (a.getStartFrame() != b.getStartFrame())
          return a.getStartFrame() < b.getStartFrame();
        return a.getEndFrame() < b.getEndFrame();
      });
}
```

Add `#include <algorithm>` at the top of `Project.cpp` if not already present.

- [ ] **Step 2: Optimize `getNoteAtFrame` with binary search**

In `Source/Models/Project.cpp`, replace the existing `getNoteAtFrame`:

```cpp
Note *Project::getNoteAtFrame(int frame)
{
  // Binary search for the first note whose startFrame > frame
  auto it = std::upper_bound(notes.begin(), notes.end(), frame,
      [](int f, const Note& note) { return f < note.getStartFrame(); });

  // Walk backward to check notes that could contain this frame
  while (it != notes.begin())
  {
    --it;
    if (it->containsFrame(frame))
      return &(*it);
    // Once startFrame is too far left, no earlier note can contain frame
    if (it->getEndFrame() <= frame)
      break;
  }
  return nullptr;
}
```

- [ ] **Step 3: Optimize `getNotesInRange` with binary search start**

In `Source/Models/Project.cpp`, replace the existing `getNotesInRange`:

```cpp
std::vector<Note *> Project::getNotesInRange(int startFrame, int endFrame)
{
  std::vector<Note *> result;

  // Find first note that could overlap: need note.endFrame > startFrame.
  // Since notes are sorted by startFrame, a note can overlap even if its
  // startFrame < startFrame (it extends past). Walk back from the first
  // note with startFrame >= startFrame.
  auto it = std::lower_bound(notes.begin(), notes.end(), startFrame,
      [](const Note& note, int f) { return note.getStartFrame() < f; });

  // Walk backward to catch notes that start before startFrame but extend into the range
  while (it != notes.begin())
  {
    --it;
    if (it->getEndFrame() <= startFrame)
    {
      ++it;
      break;
    }
  }

  // Scan forward collecting overlapping notes
  for (; it != notes.end(); ++it)
  {
    if (it->getStartFrame() >= endFrame)
      break;
    if (it->getEndFrame() > startFrame)
      result.push_back(&(*it));
  }

  return result;
}
```

- [ ] **Step 4: Add `sortNotes()` call after `StretchProcessor::remapNoteFrames`**

In `Source/Utils/WarpMarkerProcessor.cpp`, after the `remapNoteFrames` call at line ~599, add:

```cpp
StretchProcessor::remapNoteFrames(project.getNotes(), warpMap);
project.sortNotes();
```

The second call site at line ~408 (`composeGlobalWaveform`) does not change note frames, so no sort is needed there.

- [ ] **Step 5: Commit**

```bash
git add Source/Models/Project.h Source/Models/Project.cpp Source/Utils/WarpMarkerProcessor.cpp
git commit -m "feat: maintain sorted notes vector with binary-search queries"
```

---

### Task 2: Remove redundant temporary sorts

**Files:**
- Modify: `Source/Utils/PitchCurveProcessor.cpp:309-323`
- Modify: `Source/Models/Project.cpp` (composeGlobalWaveform ~1131-1142, renderMappedBaseWaveformSegment ~829-840, renderMappedSourceSegment ~985-996)

- [ ] **Step 1: Replace temp sort in `PitchCurveProcessor::computeBoundarySmoothingSegments`**

In `Source/Utils/PitchCurveProcessor.cpp`, replace lines 309-323:

```cpp
        std::vector<const Note*> sortedNotes;
        sortedNotes.reserve(project.getNotes().size());
        for (const auto& note : project.getNotes())
        {
            if (!note.isRest())
                sortedNotes.push_back(&note);
        }

        std::sort(sortedNotes.begin(), sortedNotes.end(),
                  [](const Note* a, const Note* b)
                  {
                      if (a->getStartFrame() != b->getStartFrame())
                          return a->getStartFrame() < b->getStartFrame();
                      return a->getEndFrame() < b->getEndFrame();
                  });
```

With:

```cpp
        std::vector<const Note*> sortedNotes;
        sortedNotes.reserve(project.getNotes().size());
        for (const auto& note : project.getNotes())
        {
            if (!note.isRest())
                sortedNotes.push_back(&note);
        }
```

(Simply remove the `std::sort` call — the vector is already ordered because `Project::notes` is sorted.)

- [ ] **Step 2: Remove temp sort in `composeGlobalWaveform`**

In `Source/Models/Project.cpp`, in `composeGlobalWaveform()`, remove the sort block at ~lines 1138-1142:

```cpp
    std::sort(sortedNotes.begin(), sortedNotes.end(),
              [](const Note *a, const Note *b)
              {
                  return a->getStartFrame() < b->getStartFrame();
              });
```

Keep the `sortedNotes` construction loop (filtering out rests) — just remove the `std::sort` call.

- [ ] **Step 3: Remove temp sort in `renderMappedBaseWaveformSegment`**

Same pattern at ~lines 836-840 — remove the `std::sort` call, keep the filter loop.

- [ ] **Step 4: Remove temp sort in `renderMappedSourceSegment`**

Same pattern at ~lines 992-996 — remove the `std::sort` call, keep the filter loop.

- [ ] **Step 5: Commit**

```bash
git add Source/Utils/PitchCurveProcessor.cpp Source/Models/Project.cpp
git commit -m "chore: remove redundant sorts now that notes vector is always ordered"
```

---

### Task 3: Extract shared Slaney mel scale utility

**Files:**
- Create: `Source/Utils/MelScale.h`
- Modify: `Source/Utils/MelSpectrogram.cpp:20-44`

- [ ] **Step 1: Create `MelScale.h`**

Create `Source/Utils/MelScale.h`:

```cpp
#pragma once

#include <cmath>
#include <vector>
#include <algorithm>

namespace MelScale
{

constexpr float kFSp = 200.0f / 3.0f;
constexpr float kMinLogHz = 1000.0f;
constexpr float kMinLogMel = kMinLogHz / kFSp;  // 15.0
inline const float kLogStep = std::log(6.4f) / 27.0f;

inline float hzToMel(float hz)
{
  if (hz < kMinLogHz)
    return hz / kFSp;
  return kMinLogMel + std::log(hz / kMinLogHz) / kLogStep;
}

inline float melToHz(float mel)
{
  if (mel < kMinLogMel)
    return kFSp * mel;
  return kMinLogHz * std::exp(kLogStep * (mel - kMinLogMel));
}

inline std::vector<float> computeCenterFrequencies(int numMels,
                                                    float fMin = 40.0f,
                                                    float fMax = 16000.0f)
{
  const float melMin = hzToMel(fMin);
  const float melMax = hzToMel(fMax);

  std::vector<float> centers(numMels);
  for (int i = 0; i < numMels; ++i)
  {
    float melPoint = melMin + (melMax - melMin) * (i + 1) / (numMels + 1);
    centers[i] = melToHz(melPoint);
  }
  return centers;
}

inline float hzToMelBin(float hz, const std::vector<float>& centerFreqs)
{
  if (hz <= 0.0f || centerFreqs.empty())
    return 0.0f;

  const int n = static_cast<int>(centerFreqs.size());

  if (hz <= centerFreqs.front())
    return 0.0f;
  if (hz >= centerFreqs.back())
    return static_cast<float>(n - 1);

  auto it = std::lower_bound(centerFreqs.begin(), centerFreqs.end(), hz);
  int idx = static_cast<int>(it - centerFreqs.begin());
  if (idx == 0)
    return 0.0f;

  float lo = centerFreqs[idx - 1];
  float hi = centerFreqs[idx];
  float t = (hz - lo) / (hi - lo);
  return static_cast<float>(idx - 1) + t;
}

} // namespace MelScale
```

- [ ] **Step 2: Refactor `MelSpectrogram::createMelFilterbank` to use `MelScale`**

In `Source/Utils/MelSpectrogram.cpp`, replace lines 20-44 (the local lambda definitions) with:

```cpp
void MelSpectrogram::createMelFilterbank()
{
    float melMin = MelScale::hzToMel(fMin);
    float melMax = MelScale::hzToMel(fMax);
```

Add `#include "MelScale.h"` at the top of `MelSpectrogram.cpp`.

Remove the local `hzToMel` and `melToHz` lambdas. Replace all usages of the lambdas in the rest of the function with `MelScale::hzToMel(...)` and `MelScale::melToHz(...)`.

- [ ] **Step 3: Commit**

```bash
git add Source/Utils/MelScale.h Source/Utils/MelSpectrogram.cpp
git commit -m "refactor: extract shared Slaney mel scale into MelScale.h"
```

---

### Task 4: Fix monitor f0/mel alignment

**Files:**
- Modify: `Source/UI/Debug/MelViewComponent.h`
- Modify: `Source/UI/Debug/MelViewComponent.cpp:277-285` and line 188

- [ ] **Step 1: Update `MelViewComponent` header**

In `Source/UI/Debug/MelViewComponent.h`, replace the static `hzToMelBin` declaration and add the center frequency cache:

Replace:
```cpp
  static float hzToMelBin(float hz, int numMels = 128);
```

With:
```cpp
  std::vector<float> melCenterFreqs;
  int cachedNumMels = 0;
  void ensureMelCenterFreqs(int numMels);
```

- [ ] **Step 2: Update `MelViewComponent` implementation**

In `Source/UI/Debug/MelViewComponent.cpp`, add `#include "../../Utils/MelScale.h"` at the top.

Remove the old `hzToMelBin` function (lines 277-285).

Add the new method:

```cpp
void MelViewComponent::ensureMelCenterFreqs(int numMels)
{
  if (numMels == cachedNumMels && !melCenterFreqs.empty())
    return;
  melCenterFreqs = MelScale::computeCenterFrequencies(numMels);
  cachedNumMels = numMels;
}
```

In the `paint()` method, before the f0 drawing loop (~line 169), add:

```cpp
  ensureMelCenterFreqs(numMels);
```

Replace line 188-189:
```cpp
    const float melBin = hzToMelBin(freq, numMels);
    const float yPos = h - (melBin / static_cast<float>(numMels)) * h;
```

With:
```cpp
    const float melBin = MelScale::hzToMelBin(freq, melCenterFreqs);
    const float yPos = h - (melBin / static_cast<float>(numMels)) * h;
```

- [ ] **Step 3: Commit**

```bash
git add Source/UI/Debug/MelViewComponent.h Source/UI/Debug/MelViewComponent.cpp
git commit -m "fix: align f0 curve with mel spectrogram using Slaney scale"
```

---

### Task 5: Fix tension save/load — unconditional HNSep re-run

**Files:**
- Modify: `Source/UI/Main/MainComponent_ProjectIO.cpp:195-208`

- [ ] **Step 1: Remove `hasActiveEdits` guard and trigger mel recomputation**

In `Source/UI/Main/MainComponent_ProjectIO.cpp`, replace lines 195-208:

```cpp
                {
                  auto &ad = project->getAudioData();
                  const int tf = ad.getNumFrames();
                  if (tf > 0 &&
                      ad.harmonicWaveform.getNumSamples() == 0 &&
                      HNSepCurveProcessor::hasActiveEdits(*project, 0, tf))
                  {
                    safeThis->editorController->runHNSepSeparation(*project);
                    // Populate per-note H/N clips from the freshly
                    // separated global waveforms so that the synthesis
                    // path can apply tension/voicing/breath per note.
                    HNSepCurveProcessor::ensureNoteHNClips(*project);
                  }
                }
```

With:

```cpp
                {
                  auto &ad = project->getAudioData();
                  const int tf = ad.getNumFrames();
                  if (tf > 0 &&
                      ad.harmonicWaveform.getNumSamples() == 0)
                  {
                    safeThis->editorController->runHNSepSeparation(*project);
                    HNSepCurveProcessor::ensureNoteHNClips(*project);
                    HNSepCurveProcessor::recomputeMelForRange(
                        *project, 0, tf);
                  }
                }
```

- [ ] **Step 2: Commit**

```bash
git add Source/UI/Main/MainComponent_ProjectIO.cpp
git commit -m "fix: always re-run HNSep on load so tension edits work after save/load"
```

---

### Task 6: pitchOffset — update `DragActions.h`

**Files:**
- Modify: `Source/Undo/DragActions.h`

- [ ] **Step 1: Rewrite `MultiNoteMidiNudgeAction` to use pitchOffset**

In `Source/Undo/DragActions.h`, replace the entire `MultiNoteMidiNudgeAction` class (lines 12-59):

```cpp
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
```

- [ ] **Step 2: Rewrite `NotePitchDragAction` to use pitchOffset**

Replace the `NotePitchDragAction` class (lines 61-130):

```cpp
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
```

- [ ] **Step 3: Rewrite `MultiNotePitchDragAction` to use pitchOffset**

Replace the `MultiNotePitchDragAction` class (lines 132-211):

```cpp
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
```

- [ ] **Step 4: Commit**

```bash
git add Source/Undo/DragActions.h
git commit -m "refactor: DragActions use pitchOffset instead of midiNote"
```

---

### Task 7: pitchOffset — update `NoteActions.h` snap actions

**Files:**
- Modify: `Source/Undo/NoteActions.h:200-303`

- [ ] **Step 1: Rewrite `NoteSnapToSemitoneAction`**

Replace lines 200-244:

```cpp
class NoteSnapToSemitoneAction : public UndoableAction
{
public:
    NoteSnapToSemitoneAction(Note *note,
                             float oldOffset,
                             float newOffset,
                             std::function<void(Note *)> onNoteChanged = nullptr)
        : note(note), oldOffset(oldOffset),
          newOffset(newOffset), onNoteChanged(onNoteChanged) {}

    void undo() override
    {
        if (note)
        {
            note->setPitchOffset(oldOffset);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNoteChanged && note)
            onNoteChanged(note);
    }

    void redo() override
    {
        if (note)
        {
            note->setPitchOffset(newOffset);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNoteChanged && note)
            onNoteChanged(note);
    }

    juce::String getName() const override { return "Snap to Semitone"; }

private:
    Note *note;
    float oldOffset;
    float newOffset;
    std::function<void(Note *)> onNoteChanged;
};
```

- [ ] **Step 2: Rewrite `MultiNoteSnapToSemitoneAction`**

Replace lines 249-303:

```cpp
class MultiNoteSnapToSemitoneAction : public UndoableAction
{
public:
    MultiNoteSnapToSemitoneAction(const std::vector<Note *> &notes,
                                  std::vector<float> oldOffsets,
                                  std::vector<float> newOffsets,
                                  std::function<void(const std::vector<Note *> &)> onNotesChanged = nullptr)
        : notes(notes),
          oldOffsets(std::move(oldOffsets)),
          newOffsets(std::move(newOffsets)),
          onNotesChanged(onNotesChanged) {}

    void undo() override
    {
        for (size_t i = 0; i < notes.size(); ++i)
        {
            auto *note = notes[i];
            if (!note)
                continue;
            note->setPitchOffset(oldOffsets[i]);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    void redo() override
    {
        for (size_t i = 0; i < notes.size(); ++i)
        {
            auto *note = notes[i];
            if (!note)
                continue;
            note->setPitchOffset(newOffsets[i]);
            note->markDirty();
            note->markSynthDirty();
        }
        if (onNotesChanged)
            onNotesChanged(notes);
    }

    juce::String getName() const override { return "Snap Notes to Semitone"; }

private:
    std::vector<Note *> notes;
    std::vector<float> oldOffsets;
    std::vector<float> newOffsets;
    std::function<void(const std::vector<Note *> &)> onNotesChanged;
};
```

- [ ] **Step 3: Commit**

```bash
git add Source/Undo/NoteActions.h
git commit -m "refactor: snap-to-semitone actions use pitchOffset only"
```

---

### Task 8: pitchOffset — update `SelectHandler.cpp` single-note drag and double-click snap

**Files:**
- Modify: `Source/UI/PianoRoll/States/SelectHandler.cpp`

- [ ] **Step 1: Update `endNoteDrag` — remove midiNote bake**

In `SelectHandler.cpp`, find the single-note drag end block (~line 359-456). Apply these changes:

1. Replace the snap logic and bake block (lines 361-385):

```cpp
    float newOffset = draggedNote->getPitchOffset();
    if (owner_.snapToSemitoneDrag)
    {
      const float snappedMidi = ScaleUtils::snapMidiToSemitone(
          originalMidiNote + newOffset, owner_.pitchReferenceHz);
      newOffset = snappedMidi - originalMidiNote;
      draggedNote->setPitchOffset(newOffset);
    }

    // Check if there was any meaningful change
    constexpr float CHANGE_THRESHOLD = 0.001f;
    bool hasChange = std::abs(newOffset - originalPitchOffset) >= CHANGE_THRESHOLD;

    if (hasChange)
    {
      int startFrame = draggedNote->getStartFrame();
      int endFrame = draggedNote->getEndFrame();
      auto &audioData = project->getAudioData();
      auto &editedData = project->getEditedData();
      int f0Size = static_cast<int>(editedData.f0.size());

      draggedNote->markSynthDirty();
```

(Key change: compare `newOffset` against `originalPitchOffset` instead of against zero, and remove `setMidiNote`/`setPitchOffset(0)` lines.)

2. In the undo action creation (~line 433-456), change:

```cpp
        auto action = std::make_unique<NotePitchDragAction>(
            *project, noteIdx, originalPitchOffset, newOffset,
            startFrame, endFrame,
            std::vector<float>(originalF0Values),
            std::move(afterF0),
            std::move(dragBeforeBasePitch),
            std::move(afterBasePitch),
```

(Replace `originalMidiNote, finalMidiNote` with `originalPitchOffset, newOffset`.)

3. In the "no meaningful change" else branch (~line 465-478), change:

```cpp
      draggedNote->setPitchOffset(originalPitchOffset);
```

(Restore original offset, not zero.)

- [ ] **Step 2: Update double-click snap — single note**

Find single-note snap (~lines 1050-1071). Replace:

```cpp
    float oldMidi = note->getMidiNote();
    float oldOffset = note->getPitchOffset();
    float adjustedMidi = oldMidi + oldOffset;
    float snappedMidi = snapForDoubleClick(adjustedMidi);

    if (std::abs(snappedMidi - adjustedMidi) > 0.001f)
    {
      if (owner_.undoManager)
      {
        auto action =
            std::make_unique<NoteSnapToSemitoneAction>(
                note, oldMidi, oldOffset, snappedMidi,
                [this](Note *)
                { rebuildAndNotify(); });
        owner_.undoManager->addAction(std::move(action));
      }

      note->setMidiNote(snappedMidi);
      note->setPitchOffset(0.0f);
      note->markDirty();
      rebuildAndNotify();
    }
```

With:

```cpp
    float oldOffset = note->getPitchOffset();
    float adjustedMidi = note->getMidiNote() + oldOffset;
    float snappedMidi = snapForDoubleClick(adjustedMidi);
    float newOffset = snappedMidi - note->getMidiNote();

    if (std::abs(newOffset - oldOffset) > 0.001f)
    {
      if (owner_.undoManager)
      {
        auto action =
            std::make_unique<NoteSnapToSemitoneAction>(
                note, oldOffset, newOffset,
                [this](Note *)
                { rebuildAndNotify(); });
        owner_.undoManager->addAction(std::move(action));
      }

      note->setPitchOffset(newOffset);
      note->markDirty();
      rebuildAndNotify();
    }
```

- [ ] **Step 3: Update double-click snap — multi-note**

Find multi-note snap (~lines 990-1044). Replace the variable declarations and collection loop:

```cpp
        std::vector<Note *> notesToSnap;
        std::vector<float> oldOffsets;
        std::vector<float> newOffsets;

        notesToSnap.reserve(selectedNotes.size());
        oldOffsets.reserve(selectedNotes.size());
        newOffsets.reserve(selectedNotes.size());

        for (auto *selected : selectedNotes)
        {
          if (!selected || selected->isRest())
            continue;

          float oldOffset = selected->getPitchOffset();
          float adjustedMidi = selected->getMidiNote() + oldOffset;
          float snappedMidi = snapForDoubleClick(adjustedMidi);
          float newOffset = snappedMidi - selected->getMidiNote();

          if (std::abs(newOffset - oldOffset) <= 0.001f)
            continue;

          notesToSnap.push_back(selected);
          oldOffsets.push_back(oldOffset);
          newOffsets.push_back(newOffset);
        }
```

Replace the action creation:

```cpp
            auto action =
                std::make_unique<MultiNoteSnapToSemitoneAction>(
                    notesToSnap, oldOffsets, newOffsets,
                    [this](const std::vector<Note *> &)
                    {
                      rebuildAndNotify();
                    });
```

Replace the apply loop (lines 1036-1041):
```cpp
          for (size_t i = 0; i < notesToSnap.size(); ++i)
          {
            notesToSnap[i]->setPitchOffset(newOffsets[i]);
            notesToSnap[i]->markDirty();
          }
```

- [ ] **Step 4: Update cancel — restore original offset**

In `cancel()` (~line 1088), change:

```cpp
    draggedNote->setPitchOffset(0.0f);
```

To:

```cpp
    draggedNote->setPitchOffset(originalPitchOffset);
```

- [ ] **Step 5: Commit**

```bash
git add Source/UI/PianoRoll/States/SelectHandler.cpp
git commit -m "refactor: SelectHandler uses pitchOffset for drag and snap"
```

---

### Task 9: pitchOffset — update `PitchEditor.cpp` multi-note drag

**Files:**
- Modify: `Source/UI/PianoRoll/PitchEditor.cpp:461-593`

- [ ] **Step 1: Update `endMultiNoteDrag` — remove bake**

In `PitchEditor.cpp`, find `endMultiNoteDrag()` (~line 461). Apply these changes:

1. Replace the change detection (line 472-474):

```cpp
  float newOffset = draggedNotes[0]->getPitchOffset();
  constexpr float CHANGE_THRESHOLD = 0.001f;
  bool hasChange = std::abs(newOffset - originalPitchOffsets[0]) >= CHANGE_THRESHOLD;
```

(Compare against original offset, not zero. This requires saving `originalPitchOffsets` — see step 2.)

2. Replace the bake loop (lines 486-495):

```cpp
    for (size_t i = 0; i < draggedNotes.size(); ++i)
    {
      auto *note = draggedNotes[i];
      note->markSynthDirty();

      expandedStart = std::min(expandedStart, note->getStartFrame());
      expandedEnd = std::max(expandedEnd, note->getEndFrame());
    }
```

(Remove `setMidiNote`/`setPitchOffset(0)` — just keep `markSynthDirty` and dirty range.)

3. In undo action creation (~line 536-557), replace:

```cpp
      std::vector<float> currentOffsets;
      currentOffsets.reserve(draggedNotes.size());
      for (auto* note : draggedNotes)
        currentOffsets.push_back(note->getPitchOffset());

      auto action = std::make_unique<MultiNotePitchDragAction>(
          *project, std::move(noteIndices), originalPitchOffsets,
          std::move(currentOffsets),
          multiDragStartFrame, multiDragEndFrame,
          std::move(multiDragBeforeF0),
          std::move(afterF0),
          std::move(multiDragBeforeBasePitch),
          std::move(afterBasePitch),
```

4. In the "no change" else branch (lines 567-578), restore original offsets:

```cpp
    for (size_t i = 0; i < draggedNotes.size() && i < originalPitchOffsets.size(); ++i)
      draggedNotes[i]->setPitchOffset(originalPitchOffsets[i]);
```

- [ ] **Step 2: Save original pitch offsets in `beginMultiNoteDrag`**

In `PitchEditor.h`, check if there's an `originalPitchOffsets` member. If not, add it alongside `originalMidiNotes`:

```cpp
  std::vector<float> originalPitchOffsets;
```

In `PitchEditor.cpp`, in `beginMultiNoteDrag` (~line 375-432), after `originalMidiNotes` is populated, add:

```cpp
  originalPitchOffsets.clear();
  originalPitchOffsets.reserve(draggedNotes.size());
  for (auto *note : draggedNotes)
    originalPitchOffsets.push_back(note->getPitchOffset());
```

In `updateMultiNoteDrag` (~line 444-446), change the offset application:

```cpp
  for (size_t i = 0; i < draggedNotes.size(); ++i)
  {
    draggedNotes[i]->setPitchOffset(originalPitchOffsets[i] + deltaSemitones);
    draggedNotes[i]->markDirty();
  }
```

(Add `originalPitchOffsets[i]` as the base for each note's offset.)

In the cleanup section at the bottom of `endMultiNoteDrag` (~line 583), add:

```cpp
  originalPitchOffsets.clear();
```

- [ ] **Step 3: Commit**

```bash
git add Source/UI/PianoRoll/PitchEditor.h Source/UI/PianoRoll/PitchEditor.cpp
git commit -m "refactor: PitchEditor multi-note drag uses pitchOffset"
```

---

### Task 10: pitchOffset — update `PianoRollComponent.cpp` keyboard nudge

**Files:**
- Modify: `Source/UI/PianoRollComponent.cpp:3040-3125`

- [ ] **Step 1: Change `nudgeSelectedNotesBySemitones` to use pitchOffset**

In `PianoRollComponent.cpp`, update the nudge function (~lines 3054-3123).

Replace the per-note calculation (lines 3059-3068):

```cpp
    const float oldOffset = note->getPitchOffset();
    const float currentAdjusted = note->getAdjustedMidiNote();
    const float movedAdjusted =
        juce::jlimit(minMidi, maxMidi,
                     currentAdjusted + static_cast<float>(semitoneDelta));
    const float newOffset = oldOffset + (movedAdjusted - currentAdjusted);

    if (std::abs(newOffset - oldOffset) <= 1.0e-6f)
      continue;

    notesToMove.push_back(note);
    oldOffsets.push_back(oldOffset);
    newOffsets.push_back(newOffset);
```

Replace the variable declarations (lines 3044-3048):

```cpp
  std::vector<Note *> notesToMove;
  std::vector<float> oldOffsets;
  std::vector<float> newOffsets;
  notesToMove.reserve(selectedNotes.size());
  oldOffsets.reserve(selectedNotes.size());
  newOffsets.reserve(selectedNotes.size());
```

Replace the undo action creation (~lines 3107-3113):

```cpp
    auto action = std::make_unique<MultiNoteMidiNudgeAction>(
        notesToMove, oldOffsets, newOffsets,
        [rebuildAndNotify](const std::vector<Note *> &notes)
        {
          rebuildAndNotify(notes);
        });
```

Replace the apply loop (lines 3116-3121):

```cpp
  for (size_t i = 0; i < notesToMove.size(); ++i)
  {
    notesToMove[i]->setPitchOffset(newOffsets[i]);
    notesToMove[i]->markDirty();
    notesToMove[i]->markSynthDirty();
  }
```

- [ ] **Step 2: Commit**

```bash
git add Source/UI/PianoRollComponent.cpp
git commit -m "refactor: keyboard nudge uses pitchOffset instead of midiNote"
```

---

### Task 11: pitchOffset — update `TransformParams.h` and `PitchToolController.cpp`

**Files:**
- Modify: `Source/Utils/TransformParams.h`
- Modify: `Source/UI/PianoRoll/PitchToolController.cpp`

- [ ] **Step 1: Update `TransformParams` to capture/restore pitchOffset instead of midiNote**

In `Source/Utils/TransformParams.h`:

1. Replace the `midiNote` field (line 16) with `pitchOffset`:

```cpp
    float pitchOffset = 0.0f;
```

2. In `fromNote` (line 31), replace:

```cpp
        p.pitchOffset = note.getPitchOffset();
```

3. In `applyToNote` (line 40), replace:

```cpp
        note.setPitchOffset(pitchOffset);
```

4. In `operator==` (line 57), replace:

```cpp
               pitchOffset == other.pitchOffset &&
```

- [ ] **Step 2: Update `PitchToolController` tilt-mean compensation**

In `Source/UI/PianoRoll/PitchToolController.cpp`, the tilt tool currently compensates tilt mean into `midiNote`. Under the new semantics, it should use `pitchOffset`.

Replace all occurrences of the pattern `setMidiNote(origParams.midiNote + tiltMean)` with `setPitchOffset(origParams.pitchOffset + tiltMean)`. There are 5 locations:

1. `restoreOriginalState` lambda (~line 425):
```cpp
      affectedNotes[i]->setPitchOffset(origParams.pitchOffset + tiltMean);
```

2. TiltLeft case (~line 577):
```cpp
          note->setPitchOffset(origParams.pitchOffset + newTiltMean);
```

3. TiltRight case (~line 585):
```cpp
          note->setPitchOffset(origParams.pitchOffset + newTiltMean);
```

4. ReduceVariance case (~line 594):
```cpp
          note->setPitchOffset(origParams.pitchOffset + currentTiltMean);
```

5. `cancel()` (~line 644):
```cpp
      affectedNotes[i]->setPitchOffset(params.pitchOffset + tiltMean);
```

- [ ] **Step 3: Commit**

```bash
git add Source/Utils/TransformParams.h Source/UI/PianoRoll/PitchToolController.cpp
git commit -m "refactor: pitch tool tilt compensation uses pitchOffset"
```

---

### Task 12: Build and verify

**Files:** None (build check)

- [ ] **Step 1: Build the project**

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON -DUSE_ASIO=ON
cmake --build build --config Release --parallel
```

Expected: clean build with zero errors.

- [ ] **Step 2: Fix any compilation errors**

If any errors arise from the undo action signature changes (callers passing wrong argument types/counts), fix them. The most likely issues:
- `SelectHandler.cpp` constructing `NotePitchDragAction` with old `(oldMidi, newMidi)` — should be `(oldOffset, newOffset)`
- `PitchEditor.cpp` constructing `MultiNotePitchDragAction` with old `(oldMidis, pitchDelta)` — should be `(oldOffsets, newOffsets)`
- Multi-note snap in `SelectHandler.cpp` passing `(oldMidis, oldOffsets, newMidis)` — should be `(oldOffsets, newOffsets)`

- [ ] **Step 3: Commit any fixes**

```bash
git add -A
git commit -m "fix: resolve compilation errors from pitchOffset refactor"
```
