# HachiTune: Undo Snapshot Refactor + TreeValueMonitor Enhancement

**Date**: 2026-04-29
**Status**: Draft
**Scope**: Two independent sub-projects (implemented sequentially)

---

## Sub-Project A: Undo System Snapshot Refactoring

### Goals

1. Replace per-frame `F0FrameEdit` undo with range-based snapshot undo
2. Remove raw pointer storage from undo actions (use Project reference)
3. Delete `F0FrameEdit.h` after migration
4. Simplify construction sites in UI handlers

### Design

#### Current State

- `F0FrameEdit` stores per-frame old/new values: `{idx, oldF0, newF0, oldDelta, newDelta, oldVoiced, newVoiced, oldEdited, newEdited}`
- Three action classes use it:
  - `F0EditAction` (draw mode F0 edits)
  - `NotePitchDragAction` (single note vertical drag)
  - `MultiNotePitchDragAction` (multi-note vertical drag)
- Actions store **raw pointers** to `audioData.f0`, `audioData.deltaPitch`, `audioData.voicedMask`, `audioData.f0EditedMask`
- UI handlers (PitchEditor, DrawHandler, SelectHandler) build `std::vector<F0FrameEdit>` per-frame during edits

#### New Design: Range Snapshot Actions

All three actions become range-snapshot-based. `SnapshotHelper` (already exists) provides `captureFloatRange()` / `captureBoolRange()` / `restoreFloatRange()` / `restoreBoolRange()`.

**New `F0DrawAction`** (replaces `F0EditAction`):

```cpp
class F0DrawAction : public UndoableAction {
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
               std::function<void(int, int)> onChanged = nullptr);

  void undo() override;  // restore before slices, rebuild, notify
  void redo() override;  // restore after slices, rebuild, notify
};
```

**New `NotePitchDragAction`** (replaces old version):

```cpp
class NotePitchDragAction : public UndoableAction {
public:
  NotePitchDragAction(Project& project,
                      int noteIndex,
                      float oldMidi, float newMidi,
                      int startFrame, int endFrame,
                      std::vector<float> beforeF0,
                      std::vector<float> afterF0,
                      std::vector<float> beforeBasePitch,
                      std::vector<float> afterBasePitch,
                      std::function<void()> onChanged = nullptr);

  void undo() override;
  void redo() override;
};
```

**New `MultiNotePitchDragAction`** (replaces old version):

```cpp
class MultiNotePitchDragAction : public UndoableAction {
public:
  MultiNotePitchDragAction(Project& project,
                           std::vector<int> noteIndices,
                           std::vector<float> oldMidis,
                           float pitchDelta,
                           int startFrame, int endFrame,
                           std::vector<float> beforeF0,
                           std::vector<float> afterF0,
                           std::vector<float> beforeBasePitch,
                           std::vector<float> afterBasePitch,
                           std::function<void()> onChanged = nullptr);

  void undo() override;
  void redo() override;
};
```

#### Undo/Redo Logic Pattern

```
undo():
  1. restoreFloatRange(audioData.f0, startFrame, beforeF0)
  2. restoreFloatRange(audioData.deltaPitch, startFrame, beforeDelta) // F0Draw only
  3. restoreBoolRange(audioData.voicedMask, startFrame, beforeVoiced) // F0Draw only
  4. restoreBoolRange(audioData.f0EditedMask, startFrame, beforeEdited) // F0Draw only
  5. restoreFloatRange(audioData.basePitch, startFrame, beforeBasePitch) // Drag only
  6. For drag: restore note(s) midiNote to oldMidi
  7. Sync editedData from audioData
  8. refreshNoteCachesForRange(startFrame, endFrame)
  9. Mark affected notes dirty + synthDirty
  10. Call onChanged callback

redo():
  Same pattern but with "after" slices and newMidi
```

#### Construction Site Changes

**PitchEditor / SelectHandler (note drag):**
- Before drag: `beforeF0 = captureFloatRange(audioData.f0, start, end)`
- After drag: `afterF0 = captureFloatRange(audioData.f0, start, end)`
- Create action with both snapshots

**DrawHandler / PitchEditor (F0 draw):**
- On mouse-down: capture `before*` slices for the note's frame range
- On mouse-up: capture `after*` slices
- Create F0DrawAction with both sets

**No more per-frame loop** building F0FrameEdit vectors.

#### Files Changed

| File | Change |
|------|--------|
| `Source/Undo/F0Actions.h` | Rewrite F0EditAction → F0DrawAction |
| `Source/Undo/DragActions.h` | Rewrite NotePitchDragAction, MultiNotePitchDragAction |
| `Source/UI/PianoRoll/PitchEditor.h` | Remove `drawingEdits` member, add before-snapshot members |
| `Source/UI/PianoRoll/PitchEditor.cpp` | Rewrite endNoteDrag, endMultiNoteDrag, endDrawing |
| `Source/UI/PianoRoll/States/DrawHandler.h` | Remove `drawingEdits`, add before-snapshot members |
| `Source/UI/PianoRoll/States/DrawHandler.cpp` | Rewrite commitPitchDrawing, capture on mouse-down |
| `Source/UI/PianoRoll/States/SelectHandler.cpp` | Rewrite drag finalization |
| `Source/Undo/F0FrameEdit.h` | DELETE |
| `Source/Undo/UndoActions.h` | Remove F0FrameEdit include |
| `Source/Utils/UndoManager.h` | Remove F0FrameEdit reference in comment |

#### drawingEdits Replacement

Currently `DrawHandler` and `PitchEditor` accumulate `drawingEdits` during drawing for undo. In the new system:
- `beforeF0`/`beforeDelta`/`beforeVoiced`/`beforeEdited` captured once on mouse-down
- Individual frame writes go directly to audioData (as they do now)
- On commit: capture `after*` slices, create F0DrawAction

The `drawingEditIndexByFrame` map (used to detect re-edits of same frame during drawing) is no longer needed for undo — but it may still be needed for the drawing algorithm itself. Check if it's used elsewhere; if only for undo dedup, remove it.

---

## Sub-Project B: TreeValueMonitor Enhancement

### Goals

1. Wire `notifyListeners()` into all 11 ProjectChangeType paths
2. Rewrite TreeValueMonitor UI: TextEditor → left/right split with TreeView + mel/F0 visualization
3. Mel spectrogram heatmap rendering (time × mel-bin)
4. F0 curve overlay (red solid for voiced, red dashed for unvoiced)

### Design

#### B.1: Wire notifyListeners (全面接入)

Add `project.notifyListeners(type, ...)` calls at these locations:

| Event Type | Where to Add |
|------------|--------------|
| `NoteListChanged` | After note add/remove/split in EditorController + NoteSplitter |
| `NotePitchChanged` | After `PitchCurveProcessor::rebuildBaseFromNotes()` calls |
| `NoteCurveChanged` | After `HNSepCurveProcessor::rebuildCurvesForRange/FromNotes()` |
| `NotePropertyChanged` | After note property setters (vibrato, volumeDb) in ParameterPanel callbacks |
| `NoteSelectionChanged` | After `project.selectAllNotes()`, and after select/deselect in SelectHandler |
| `WarpChanged` | After `recomputeFromMarkers()` |
| `GlobalParamChanged` | After global pitch offset / formant shift / volume changes |
| `EditedDataChanged` | After `composeF0InPlace()` and after stretch global data updates |
| `SettingsChanged` | After scale/timeline/loop setting changes |
| `AudioDataChanged` | After audio load complete, after analysis complete |
| `SynthesisComplete` | After IncrementalSynthesizer completes (already has callback) |

#### B.2: TreeValueMonitor UI Rewrite

**Window Layout:**
```
┌─────────────────────────────────────────────────┐
│  TreeValueMonitor (DocumentWindow, 1200×700)    │
├──────────┬──────────────────────────────────────┤
│ TreeView │  Mel Spectrogram + F0 Overlay        │
│ (300px)  │  (fills remaining width)             │
│          │                                      │
│ ▼Project │  [heatmap: dark blue → yellow]       │
│   Name   │  [red curve: F0 overlay]             │
│   Offset │                                      │
│ ▼Analysis│  Y: mel bins 0-128 (low→high)        │
│   Frames │  X: time (all frames)                │
│ ▼Edited  │                                      │
│   Frames │  Scroll: horizontal for long audio   │
│   ▼f0    │  Zoom: mouse wheel                   │
│   ▼base..│                                      │
│ ▼Notes   │                                      │
│   [0]... │                                      │
│   [1]... │                                      │
│ ▼Markers │                                      │
│   [0]... │                                      │
└──────────┴──────────────────────────────────────┘
```

**Left Panel: TreeView**
- JUCE `TreeView` with `TreeViewItem` subclasses
- Two-column display: property name | value
- Expandable nodes for: Project, AnalysisData, EditedData (with sub-arrays), Notes (each note expandable), WarpMarkers
- Array values show summary when collapsed (e.g., "float[1024]"), individual values when expanded (or first N elements)
- Auto-refreshes on `onProjectChanged`

**Right Panel: MelViewComponent**
- Custom `juce::Component` with `paint()` override
- Renders `audioData.melSpectrogram[T][128]` as heatmap image
- Color map: viridis-like (dark purple/blue → green → yellow for high energy)
- X axis: frame index (scrollable/zoomable)
- Y axis: mel bin 0 (bottom, low freq) → 127 (top, high freq)
- Cached as `juce::Image` for performance (re-rendered only when data changes)

**F0 Overlay on Mel:**
- Bright red curve (`juce::Colours::red`, 2px stroke)
- Maps `editedData.f0[frame]` to Y position: `freqToMel(f0) / maxMelBin * height`
- Voiced frames: solid line
- Unvoiced frames: dashed line (4px dash, 3px gap)
- Connects consecutive frames with `lineTo()`

#### B.3: Performance Considerations

- Mel image re-rendered to cached `juce::Image` only on `AudioDataChanged`, `EditedDataChanged`, `SynthesisComplete`
- F0 overlay re-rendered on any pitch-related event (`NotePitchChanged`, `EditedDataChanged`)
- TreeView text updated on any event (cheap operation)
- Horizontal scroll + zoom via mouse wheel / drag (only repaints visible region)

#### Files Changed

| File | Change |
|------|--------|
| `Source/UI/Debug/TreeValueMonitor.h` | Complete rewrite: split layout, new components |
| `Source/UI/Debug/TreeValueMonitor.cpp` | Complete rewrite: TreeView + MelViewComponent |
| `Source/Models/Project.cpp` | (notifyListeners already exists, just needs call sites) |
| `Source/Audio/EditorController.cpp` | Add notifyListeners calls after key operations |
| `Source/Utils/PitchCurveProcessor.cpp` | Add notifyListeners after rebuildBaseFromNotes |
| `Source/Utils/HNSepCurveProcessor.cpp` | Add notifyListeners after curve rebuilds |
| `Source/Audio/Synthesis/IncrementalSynthesizer.cpp` | Add SynthesisComplete notification |
| `Source/UI/PianoRoll/States/SelectHandler.cpp` | Add NoteSelectionChanged |
| `Source/UI/PianoRoll/NoteSplitter.cpp` | Add NoteListChanged |
| `Source/UI/MainComponent.cpp` | Add notifications for settings/global param changes |
| New: `Source/UI/Debug/MelViewComponent.h/.cpp` | Mel heatmap + F0 overlay rendering |
| New: `Source/UI/Debug/ProjectTreeView.h/.cpp` | TreeView with property nodes |

---

## Implementation Order

1. **Sub-Project A first** (Undo refactor) — smaller scope, removes tech debt
2. **Sub-Project B second** (TreeValueMonitor) — larger scope, benefits from A being done (undo actions properly notify)

Each gets its own plan → implementation cycle.
