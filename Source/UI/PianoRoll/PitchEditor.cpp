#include "PitchEditor.h"
#include "../../Utils/ScaleUtils.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

bool usesBoundarySmoothingPreview(Project& project,
                                  const std::vector<Note*>& notes) {
  const auto dependentNotes =
      PitchCurveProcessor::collectDependentNotes(project, notes);
  for (const auto* note : dependentNotes) {
    if (note != nullptr &&
        (note->getSmoothLeftFrames() > 0 ||
         note->getSmoothRightFrames() > 0)) {
      return true;
    }
  }
  return false;
}

void rebuildBoundarySmoothingPreview(Project& project,
                                     const std::vector<Note*>& notes) {
  const auto dependentNotes =
      PitchCurveProcessor::collectDependentNotes(project, notes);
  PitchCurveProcessor::rebuildBaseFromNotesForDrag(project, dependentNotes);
}

}  // namespace

PitchEditor::PitchEditor() = default;

Note *PitchEditor::findNoteAt(float x, float y)
{
  if (!project || !coordMapper)
    return nullptr;

  for (auto &note : project->getNotes())
  {
    if (note.isRest())
      continue;

    float noteX = framesToSeconds(note.getStartFrame()) *
                  coordMapper->getPixelsPerSecond();
    float noteW = framesToSeconds(note.getDurationFrames()) *
                  coordMapper->getPixelsPerSecond();
    float noteY = coordMapper->midiToY(note.getAdjustedMidiNote());
    float noteH = coordMapper->getPixelsPerSemitone();

    if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH)
    {
      return &note;
    }
  }

  return nullptr;
}

void PitchEditor::startNoteDrag(Note *note, float y)
{
  if (!note || !project)
    return;

  // Capture delta slice from global dense deltaPitch
  auto &editedData = project->getEditedData();
  int startFrame = note->getStartFrame();
  int endFrame = note->getEndFrame();
  int numFrames = endFrame - startFrame;

  std::vector<float> delta(numFrames, 0.0f);
  for (int i = 0; i < numFrames; ++i)
  {
    int globalFrame = startFrame + i;
    if (globalFrame >= 0 &&
        globalFrame < static_cast<int>(editedData.deltaPitch.size()))
      delta[i] = editedData.deltaPitch[static_cast<size_t>(globalFrame)];
  }
  note->setDeltaPitch(std::move(delta));

  isDragging = true;
  draggedNote = note;
  dragStartY = y;
  originalPitchOffset = note->getPitchOffset();
  originalMidiNote = note->getMidiNote();

  // Save boundary F0 values
  int f0Size = static_cast<int>(editedData.f0.size());
  boundaryF0Start = (startFrame > 0 && startFrame - 1 < f0Size)
                        ? editedData.f0[startFrame - 1]
                        : 0.0f;
  boundaryF0End = (endFrame < f0Size) ? editedData.f0[endFrame] : 0.0f;

  // Save original F0 values for undo
  originalF0Values.clear();
  for (int i = startFrame; i < endFrame && i < f0Size; ++i)
    originalF0Values.push_back(editedData.f0[i]);

  dragBeforeBasePitch = SnapshotHelper::captureFloatRange(
      editedData.basePitch, startFrame, endFrame);

  prepareDragBasePreview();

  if (onNoteSelected)
    onNoteSelected(note);
}

void PitchEditor::endDrawing()
{
  if (drawMinEditedFrame > drawMaxEditedFrame || drawSnapshotStartFrame < 0)
  {
    isDrawing = false;
    return;
  }

  const int rangeStart = drawMinEditedFrame;
  const int rangeEnd = drawMaxEditedFrame + 1;

  // Clear deltaPitch for notes in edited range
  if (project && rangeStart < rangeEnd)
  {
    auto &notes = project->getNotes();
    for (auto &note : notes)
    {
      if (note.getEndFrame() > rangeStart &&
          note.getStartFrame() < rangeEnd)
      {
        if (note.hasDeltaPitch())
          note.setDeltaPitch(std::vector<float>());
      }
    }
    project->setF0DirtyRange(rangeStart, rangeEnd);
  }

  // Create undo action
  if (undoManager && project)
  {
    auto &audioData = project->getAudioData();
  auto &editedData = project->getEditedData();

    auto afterF0 = SnapshotHelper::captureFloatRange(editedData.f0, rangeStart, rangeEnd);
    auto afterDelta = SnapshotHelper::captureFloatRange(editedData.deltaPitch, rangeStart, rangeEnd);
    auto afterVoiced = SnapshotHelper::captureBoolRange(editedData.voicedMask, rangeStart, rangeEnd);

    auto slicedBeforeF0 = SnapshotHelper::captureFloatRange(drawBeforeF0, rangeStart - drawSnapshotStartFrame,
                                                             rangeEnd - drawSnapshotStartFrame);
    auto slicedBeforeDelta = SnapshotHelper::captureFloatRange(drawBeforeDelta, rangeStart - drawSnapshotStartFrame,
                                                               rangeEnd - drawSnapshotStartFrame);
    auto slicedBeforeVoiced = SnapshotHelper::captureBoolRange(drawBeforeVoiced, rangeStart - drawSnapshotStartFrame,
                                                                rangeEnd - drawSnapshotStartFrame);
    auto slicedBeforeEdited = SnapshotHelper::captureBoolRange(drawBeforeEdited, rangeStart - drawSnapshotStartFrame,
                                                                 rangeEnd - drawSnapshotStartFrame);

    // afterEdited: f0EditedMask removed, provide empty placeholder for undo action
    auto afterEdited = std::vector<bool>(static_cast<size_t>(rangeEnd - rangeStart), false);

    auto action = std::make_unique<F0DrawAction>(
        *project,
        rangeStart, rangeEnd,
        std::move(slicedBeforeF0), std::move(afterF0),
        std::move(slicedBeforeDelta), std::move(afterDelta),
        std::move(slicedBeforeVoiced), std::move(afterVoiced),
        std::move(slicedBeforeEdited), std::move(afterEdited),
        [this](int minFrame, int maxFrame) {
          if (project) {
            project->setF0DirtyRange(minFrame, maxFrame + 1);
            if (onPitchEditFinished)
              onPitchEditFinished();
          }
        });
    undoManager->addAction(std::move(action));
  }

  drawSnapshotStartFrame = -1;
  drawSnapshotEndFrame = -1;
  drawBeforeF0.clear();
  drawBeforeDelta.clear();
  drawBeforeVoiced.clear();
  drawBeforeEdited.clear();
  drawMinEditedFrame = std::numeric_limits<int>::max();
  drawMaxEditedFrame = std::numeric_limits<int>::min();
  lastDrawFrame = -1;
  lastDrawValueCents = 0;
  activeDrawCurve = nullptr;
  drawCurves.clear();

  isDrawing = false;

  if (onPitchEditFinished)
    onPitchEditFinished();
}

void PitchEditor::applyPitchPoint(int frameIndex, int midiCents)
{
  if (!project)
    return;

  auto &audioData = project->getAudioData();
  auto &editedData = project->getEditedData();
  if (editedData.f0.empty())
    return;

  const int f0Size = static_cast<int>(editedData.f0.size());
  if (editedData.deltaPitch.size() < editedData.f0.size())
    editedData.deltaPitch.resize(editedData.f0.size(), 0.0f);
  if (editedData.basePitch.size() < editedData.f0.size())
    editedData.basePitch.resize(editedData.f0.size(), 0.0f);
  if (frameIndex < 0 || frameIndex >= f0Size)
    return;

  auto applyFrame = [&](int idx, int cents)
  {
    if (idx < 0 || idx >= f0Size)
      return;

    const float newFreq = midiToFreq(static_cast<float>(cents) / 100.0f);

    // Lazy snapshot capture on first frame edit
    if (drawSnapshotStartFrame < 0) {
      const int totalFrames = static_cast<int>(editedData.f0.size());
      drawSnapshotStartFrame = 0;
      drawSnapshotEndFrame = totalFrames;
      drawBeforeF0 = SnapshotHelper::captureFloatRange(editedData.f0, 0, totalFrames);
      drawBeforeDelta = SnapshotHelper::captureFloatRange(editedData.deltaPitch, 0, totalFrames);
      drawBeforeVoiced = SnapshotHelper::captureBoolRange(editedData.voicedMask, 0, totalFrames);
    }

    drawMinEditedFrame = std::min(drawMinEditedFrame, idx);
    drawMaxEditedFrame = std::max(drawMaxEditedFrame, idx);

    float baseMidi = (idx < static_cast<int>(editedData.basePitch.size()))
                         ? editedData.basePitch[static_cast<size_t>(idx)]
                         : 0.0f;
    float newMidi = static_cast<float>(cents) / 100.0f;
    float newDelta = newMidi - baseMidi;

    // Clear deltaPitch for notes containing this frame
    auto &notes = project->getNotes();
    for (auto &note : notes)
    {
      if (note.getStartFrame() <= idx && note.getEndFrame() > idx &&
          note.hasDeltaPitch())
      {
        note.setDeltaPitch(std::vector<float>());
        break;
      }
    }

    editedData.f0[idx] = newFreq;
    if (idx < static_cast<int>(editedData.deltaPitch.size()))
    {
      editedData.deltaPitch[static_cast<size_t>(idx)] = newDelta;
    }
    if (idx < static_cast<int>(editedData.voicedMask.size()))
      editedData.voicedMask[idx] = true;
  };

  // Only start a new curve if there's no active curve (first point of drawing)
  if (!activeDrawCurve)
  {
    startNewPitchCurve(frameIndex, midiCents);
    applyFrame(frameIndex, midiCents);
    return;
  }

  // Helper to append/prepend value to the active curve
  auto appendValue = [&](int idx, int cents)
  {
    if (!activeDrawCurve)
      return;

    const int curveStart = activeDrawCurve->localStart();
    auto &vals = activeDrawCurve->mutableValues();

    // Handle backward drawing: prepend values if idx < curveStart
    if (idx < curveStart)
    {
      const int prependCount = curveStart - idx;
      std::vector<int> newVals(static_cast<size_t>(prependCount), cents);
      newVals.insert(newVals.end(), vals.begin(), vals.end());
      activeDrawCurve->setValues(std::move(newVals));
      activeDrawCurve->setLocalStart(idx);
      return;
    }

    const int offset = idx - curveStart;
    if (offset < static_cast<int>(vals.size()))
    {
      vals[static_cast<std::size_t>(offset)] = cents;
      return;
    }

    while (static_cast<int>(vals.size()) < offset)
    {
      int fill = vals.empty() ? cents : vals.back();
      vals.push_back(fill);
    }
    vals.push_back(cents);
  };

  if (lastDrawFrame < 0)
  {
    appendValue(frameIndex, midiCents);
    applyFrame(frameIndex, midiCents);
  }
  else
  {
    int start = lastDrawFrame;
    int end = frameIndex;
    int startVal = lastDrawValueCents;
    int endVal = midiCents;

    if (start == end)
    {
      appendValue(frameIndex, midiCents);
      applyFrame(frameIndex, midiCents);
    }
    else
    {
      int step = (end > start) ? 1 : -1;
      int length = std::abs(end - start);
      for (int i = 0; i <= length; ++i)
      {
        int idx = start + i * step;
        float t = length == 0
                      ? 0.0f
                      : static_cast<float>(i) / static_cast<float>(length);
        float v = juce::jmap(t, 0.0f, 1.0f, static_cast<float>(startVal),
                             static_cast<float>(endVal));
        int cents = static_cast<int>(std::round(v));
        appendValue(idx, cents);
        applyFrame(idx, cents);
      }
    }
  }

  lastDrawFrame = frameIndex;
  lastDrawValueCents = midiCents;
}

void PitchEditor::startNewPitchCurve(int frameIndex, int midiCents)
{
  drawCurves.push_back(std::make_unique<DrawCurve>(frameIndex, 1));
  activeDrawCurve = drawCurves.back().get();
  activeDrawCurve->appendValue(midiCents);
  lastDrawFrame = frameIndex;
  lastDrawValueCents = midiCents;
}

void PitchEditor::snapNoteToSemitone(Note *note)
{
  if (!note || !project)
    return;

  const float currentOffset = note->getPitchOffset();
  const float adjustedMidi = note->getMidiNote() + currentOffset;
  const float snappedMidi = static_cast<float>(ScaleUtils::snapMidiToScale(
      adjustedMidi, project->getScaleMode(), project->getScaleRootNote(),
      project->getPitchReferenceHz()));
  const float snappedOffset = snappedMidi - note->getMidiNote();

  if (std::abs(snappedOffset - currentOffset) > 0.001f)
  {
    if (undoManager)
    {
      auto action = std::make_unique<NoteFloatPropertyAction>(
          note, currentOffset, snappedOffset,
          &Note::setPitchOffset, "Change Pitch Offset");
      undoManager->addAction(std::move(action));
    }

    note->setPitchOffset(snappedOffset);
    note->markDirty();

    if (onPitchEdited)
      onPitchEdited();
    if (onPitchEditFinished)
      onPitchEditFinished();
  }
}

void PitchEditor::startMultiNoteDrag(const std::vector<Note *> &notes,
                                     float y)
{
  if (notes.empty() || !project)
    return;

  draggedNotes = notes;
  originalMidiNotes.clear();
  originalF0ValuesMulti.clear();
  dragStartY = y;

  auto &audioData = project->getAudioData();
  auto &editedData = project->getEditedData();
  int f0Size = static_cast<int>(editedData.f0.size());

  for (auto *note : draggedNotes)
  {
    originalMidiNotes.push_back(note->getMidiNote());

    // Capture delta slice for each note
    int startFrame = note->getStartFrame();
    int endFrame = note->getEndFrame();
    int numFrames = endFrame - startFrame;

    std::vector<float> delta(numFrames, 0.0f);
    for (int i = 0; i < numFrames; ++i)
    {
      int globalFrame = startFrame + i;
      if (globalFrame >= 0 &&
          globalFrame < static_cast<int>(editedData.deltaPitch.size()))
        delta[i] = editedData.deltaPitch[static_cast<size_t>(globalFrame)];
    }
    note->setDeltaPitch(std::move(delta));

    // Save original F0 values
    std::vector<float> f0Values;
    for (int i = startFrame; i < endFrame && i < f0Size; ++i)
      f0Values.push_back(editedData.f0[i]);
    originalF0ValuesMulti.push_back(std::move(f0Values));
  }

  prepareDragBasePreview();

  int overallStart = std::numeric_limits<int>::max();
  int overallEnd = std::numeric_limits<int>::min();
  for (auto* note : draggedNotes) {
    overallStart = std::min(overallStart, note->getStartFrame());
    overallEnd = std::max(overallEnd, note->getEndFrame());
  }
  multiDragStartFrame = overallStart;
  multiDragEndFrame = overallEnd;
  multiDragBeforeF0 = SnapshotHelper::captureFloatRange(
      editedData.f0, overallStart, overallEnd);
  multiDragBeforeBasePitch = SnapshotHelper::captureFloatRange(
      editedData.basePitch, overallStart, overallEnd);

  isMultiDragging = true;
}

void PitchEditor::updateMultiNoteDrag(float y)
{
  if (!isMultiDragging || draggedNotes.empty() || !coordMapper)
    return;

  float deltaY = dragStartY - y;
  float deltaSemitones = deltaY / coordMapper->getPixelsPerSemitone();
  if (snapToSemitoneDragEnabled)
    deltaSemitones = std::round(deltaSemitones);

  for (auto *note : draggedNotes)
  {
    note->setPitchOffset(deltaSemitones);
    note->markDirty();
  }

  if (project != nullptr &&
      usesBoundarySmoothingPreview(*project, draggedNotes))
  {
    rebuildBoundarySmoothingPreview(*project, draggedNotes);
  }
  else
  {
    applyDragBasePreview(deltaSemitones);
  }
}

void PitchEditor::endMultiNoteDrag()
{
  if (!isMultiDragging || draggedNotes.empty() || !project)
  {
    isMultiDragging = false;
    draggedNotes.clear();
    originalMidiNotes.clear();
    originalF0ValuesMulti.clear();
    return;
  }

  float newOffset = draggedNotes[0]->getPitchOffset();
  constexpr float CHANGE_THRESHOLD = 0.001f;
  bool hasChange = std::abs(newOffset) >= CHANGE_THRESHOLD;

  if (hasChange)
  {
    auto &audioData = project->getAudioData();
  auto &editedData = project->getEditedData();
    int f0Size = static_cast<int>(editedData.f0.size());

    int expandedStart = std::numeric_limits<int>::max();
    int expandedEnd = std::numeric_limits<int>::min();

    // Bake pitchOffset into midiNote for all notes
    for (size_t i = 0; i < draggedNotes.size(); ++i)
    {
      auto *note = draggedNotes[i];
      note->setMidiNote(originalMidiNotes[i] + newOffset);
      note->setPitchOffset(0.0f);
      note->markSynthDirty();

      expandedStart = std::min(expandedStart, note->getStartFrame());
      expandedEnd = std::max(expandedEnd, note->getEndFrame());
    }

    // Find adjacent notes to expand dirty range
    const auto &allNotes = project->getNotes();
    for (const auto &note : allNotes)
    {
      if (note.getEndFrame() > expandedStart - 30 &&
          note.getEndFrame() <= expandedStart)
        expandedStart = std::min(expandedStart, note.getStartFrame());
      if (note.getStartFrame() < expandedEnd + 30 &&
          note.getStartFrame() >= expandedEnd)
        expandedEnd = std::max(expandedEnd, note.getEndFrame());
    }

    // Rebuild pitch curves
    PitchCurveProcessor::rebuildBaseFromNotes(*project);

    if (onBasePitchCacheInvalidated)
      onBasePitchCacheInvalidated();

    // Mark dirty range
    int smoothStart = std::max(0, expandedStart - 60);
    int smoothEnd = std::min(f0Size, expandedEnd + 60);
    project->setF0DirtyRange(smoothStart, smoothEnd);

    // Create undo action for multi-note drag
    if (undoManager)
    {
      auto afterF0 = SnapshotHelper::captureFloatRange(
          editedData.f0, multiDragStartFrame, multiDragEndFrame);
      auto afterBasePitch = SnapshotHelper::captureFloatRange(
          editedData.basePitch, multiDragStartFrame, multiDragEndFrame);

      std::vector<int> noteIndices;
      noteIndices.reserve(draggedNotes.size());
      for (auto* note : draggedNotes)
        noteIndices.push_back(project->getNoteIndex(note));

      int capturedExpandedStart = expandedStart;
      int capturedExpandedEnd = expandedEnd;
      int capturedF0Size = f0Size;
      auto action = std::make_unique<MultiNotePitchDragAction>(
          *project, std::move(noteIndices), originalMidiNotes, newOffset,
          multiDragStartFrame, multiDragEndFrame,
          std::move(multiDragBeforeF0),
          std::move(afterF0),
          std::move(multiDragBeforeBasePitch),
          std::move(afterBasePitch),
          [this, capturedExpandedStart, capturedExpandedEnd,
           capturedF0Size]()
          {
            if (project)
            {
              PitchCurveProcessor::rebuildBaseFromNotes(*project);
              if (onBasePitchCacheInvalidated)
                onBasePitchCacheInvalidated();
              int smoothStart = std::max(0, capturedExpandedStart - 60);
              int smoothEnd =
                  std::min(capturedF0Size, capturedExpandedEnd + 60);
              project->setF0DirtyRange(smoothStart, smoothEnd);
            }
          });
      undoManager->addAction(std::move(action));
    }

    if (onPitchEdited)
      onPitchEdited();
    if (onPitchEditFinished)
      onPitchEditFinished();
  }
  else
  {
    // No meaningful change: reset pitchOffset
    for (auto *note : draggedNotes)
      note->setPitchOffset(0.0f);
    if (project != nullptr &&
        usesBoundarySmoothingPreview(*project, draggedNotes))
    {
      rebuildBoundarySmoothingPreview(*project, draggedNotes);
    }
    else
    {
      restoreDragBasePreview();
    }
  }

  isMultiDragging = false;
  draggedNotes.clear();
  originalMidiNotes.clear();
  originalF0ValuesMulti.clear();
  dragPreviewStartFrame = -1;
  dragPreviewEndFrame = -1;
  dragPreviewWeights.clear();
  dragBasePitchSnapshot.clear();
  dragF0Snapshot.clear();
  multiDragStartFrame = -1;
  multiDragEndFrame = -1;
  multiDragBeforeF0.clear();
  multiDragBeforeBasePitch.clear();
}

void PitchEditor::prepareDragBasePreview()
{
  if (!project)
    return;

  auto &audioData = project->getAudioData();
  auto &editedData = project->getEditedData();
  if (editedData.basePitch.empty() || editedData.f0.empty())
    return;

  std::vector<Note *> selectedNotes = draggedNotes;
  if (selectedNotes.empty() && draggedNote)
    selectedNotes.push_back(draggedNote);

  if (selectedNotes.empty())
    return;

  auto range = computeBasePitchPreviewRange(
      project->getNotes(), static_cast<int>(editedData.basePitch.size()),
      [&selectedNotes](const Note &note)
      {
        return std::find(selectedNotes.begin(), selectedNotes.end(), &note) !=
               selectedNotes.end();
      });

  if (range.startFrame < 0 || range.endFrame <= range.startFrame ||
      range.weights.empty())
  {
    return;
  }

  dragPreviewStartFrame = range.startFrame;
  dragPreviewEndFrame = range.endFrame;
  dragPreviewWeights = std::move(range.weights);

  const int count = dragPreviewEndFrame - dragPreviewStartFrame;
  dragBasePitchSnapshot.resize(static_cast<size_t>(count));
  dragF0Snapshot.resize(static_cast<size_t>(count));

  for (int i = 0; i < count; ++i)
  {
    const int frame = dragPreviewStartFrame + i;
    dragBasePitchSnapshot[static_cast<size_t>(i)] =
        editedData.basePitch[static_cast<size_t>(frame)];
    dragF0Snapshot[static_cast<size_t>(i)] =
        editedData.f0[static_cast<size_t>(frame)];
  }

  lastDragPitchOffset = 0.0f;
}

void PitchEditor::applyDragBasePreview(float pitchOffsetSemitones)
{
  if (std::abs(pitchOffsetSemitones - lastDragPitchOffset) < 0.0001f)
    return;

  lastDragPitchOffset = pitchOffsetSemitones;
  if (!project || dragPreviewStartFrame < 0 ||
      dragPreviewEndFrame <= dragPreviewStartFrame ||
      dragPreviewWeights.empty() || dragBasePitchSnapshot.empty())
    return;

  auto &audioData = project->getAudioData();
  auto &editedData = project->getEditedData();
  const int count = dragPreviewEndFrame - dragPreviewStartFrame;

  if (editedData.basePitch.size() < static_cast<size_t>(dragPreviewEndFrame))
    return;


  for (int i = 0; i < count; ++i)
  {
    const int frame = dragPreviewStartFrame + i;
    const float baseMidi =
        dragBasePitchSnapshot[static_cast<size_t>(i)] +
        pitchOffsetSemitones * dragPreviewWeights[static_cast<size_t>(i)];
    editedData.basePitch[static_cast<size_t>(frame)] = baseMidi;

    const float deltaMidi =
        (frame < static_cast<int>(editedData.deltaPitch.size()))
            ? editedData.deltaPitch[static_cast<size_t>(frame)]
            : 0.0f;
    if (frame < static_cast<int>(editedData.voicedMask.size()) &&
        !editedData.voicedMask[static_cast<size_t>(frame)])
    {
      editedData.f0[static_cast<size_t>(frame)] = 0.0f;
    }
    else
    {
      editedData.f0[static_cast<size_t>(frame)] = midiToFreq(baseMidi + deltaMidi);
    }
  }
}

void PitchEditor::restoreDragBasePreview()
{
  if (!project || dragPreviewStartFrame < 0 ||
      dragPreviewEndFrame <= dragPreviewStartFrame ||
      dragBasePitchSnapshot.empty() || dragF0Snapshot.empty())
    return;

  auto &audioData = project->getAudioData();
  auto &editedData = project->getEditedData();
  const int count = dragPreviewEndFrame - dragPreviewStartFrame;

  if (editedData.basePitch.size() < static_cast<size_t>(dragPreviewEndFrame))
    return;

  for (int i = 0; i < count; ++i)
  {
    const int frame = dragPreviewStartFrame + i;
    editedData.basePitch[static_cast<size_t>(frame)] =
        dragBasePitchSnapshot[static_cast<size_t>(i)];
          midiToFreq(editedData.basePitch[static_cast<size_t>(frame)]);
    editedData.f0[static_cast<size_t>(frame)] =
        dragF0Snapshot[static_cast<size_t>(i)];
  }
  lastDragPitchOffset = 0.0f;
}
