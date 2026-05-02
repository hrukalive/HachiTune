# Spec 1: Foundations & pitchOffset Semantic Unification

**Date:** 2026-05-02
**Branch:** StretchAndDraw
**Scope:** Four independent fixes that improve data integrity, display correctness, load reliability, and pitch editing semantics.

## 1. Notes List Ordering

### Problem

`Project::addNote()` appends to the end of `std::vector<Note>`. After `NoteSplitter::splitNoteAtFrame()` calls `addNote`, the second half lands at the vector tail instead of its timeline position. `getNoteAtFrame()` and `getNotesInRange()` do linear scans in insertion order, and multiple consumers (`PitchCurveProcessor`, `composeGlobalWaveform`, etc.) create temporary sorted copies on every call.

### Design

**Invariant:** `Project::notes` is always sorted by `(startFrame, endFrame)` ascending.

**Changes:**

1. **`Project::addNote(Note note)`** — use `std::lower_bound` to find the insertion position by `startFrame` (tie-break by `endFrame`), then `vector::insert`. The comparator matches the one in `PitchCurveProcessor::computeBoundarySmoothingSegments()`.

2. **`Project::getNoteAtFrame(int frame)`** — binary search for the first note with `startFrame > frame`, then walk backward checking `containsFrame(frame)`. Worst-case remains O(N) for overlapping notes, but typical case (non-overlapping) is O(log N).

3. **`Project::getNotesInRange(int start, int end)`** — `lower_bound` to find the first note whose `endFrame > start`, then linear scan forward until `startFrame >= end`.

4. **Remove temporary sorts** — all call sites that create local `sortedNotes` vectors (PitchCurveProcessor line 309-323, composeGlobalWaveform line 1138-1142, renderMappedBaseWaveformSegment line 836-840, renderMappedSourceSegment line 992-996) can work directly on `notes`. Replace sorts with `jassert` that verifies ordering in debug builds.

5. **NoteSplitter** — no change needed; it calls `addNote` which will insert at the correct position. The pointer invalidation guard (saving `firstNote` before `addNote`) remains necessary since `insert` can reallocate.

6. **Any code that mutates `startFrame`/`endFrame`** (e.g., drag resize, warp remap) must maintain sort order. Two mechanisms:
   - **Single-note:** `Project::updateNotePosition(Note* note, int newStart, int newEnd)` — removes the note from the vector, updates its frames, and re-inserts at the correct sorted position. Use for drag resize and individual frame changes.
   - **Batch:** `Project::sortNotes()` — re-sorts the entire vector. Use after bulk operations that change many notes' frames simultaneously (e.g., `StretchProcessor::remapNoteFrames` after warp marker changes).

**Files affected:**
- `Source/Models/Project.h` — addNote signature, add sortNotes/updateNotePosition
- `Source/Models/Project.cpp` — addNote, getNoteAtFrame, getNotesInRange, sortNotes
- `Source/Utils/PitchCurveProcessor.cpp` — remove temp sort (~line 309)
- `Source/Models/Project.cpp` — remove temp sorts in composeGlobalWaveform, renderMapped*

---

## 2. Monitor Mel/F0 Alignment

### Problem

`MelViewComponent::hzToMelBin()` uses the HTK mel scale formula (`2595 * log10(1 + hz/700)`), but `MelSpectrogram` computes filterbank center frequencies using the Slaney (librosa default) mel scale (piecewise linear below 1000 Hz, logarithmic above). This mismatch causes the F0 overlay curve to be visually offset from the fundamental frequency visible in the mel spectrogram.

### Design

**Fix:** Replace `MelViewComponent::hzToMelBin()` with a mapping that matches the filterbank bins.

**Approach:**

1. **Precompute filterbank center frequencies** — At construction or when mel data changes, compute the `numMels` center frequencies using the same Slaney formula from `MelSpectrogram::computeFilterbank()`:
   - Below 1000 Hz: `f = f_min + f_sp * mel_index` where `f_sp = 200/3`
   - Above 1000 Hz: `f = min_log_hz * exp(logstep * (mel_index - min_log_mel))` where `logstep = ln(6.4)/27`, `min_log_mel = 15.0`

2. **`hzToMelBin(float hz)`** — Given an f0 frequency in Hz, find its position among the precomputed center frequencies via linear interpolation. This gives a fractional bin index in `[0, numMels]` that exactly corresponds to the mel spectrogram's vertical axis.

3. **Y mapping** — `yPos = h - (melBin / numMels) * h` remains unchanged, since the mel spectrogram image already maps bin 0 to bottom and bin N-1 to top.

4. **Extract shared utility** — The Slaney `hzToMel`/`melToHz` functions are duplicated between `MelSpectrogram.cpp` (inline in `computeFilterbank`) and now `MelViewComponent`. Extract them into `Utils/MelScale.h` as free functions (or a small struct) so both files share the same implementation.

**Files affected:**
- `Source/UI/Debug/MelViewComponent.h/.cpp` — replace hzToMelBin, cache center frequencies
- `Source/Utils/MelScale.h` (new) — shared Slaney mel scale utilities
- `Source/Utils/MelSpectrogram.cpp` — use shared MelScale instead of inline computation

---

## 3. Tension Save/Load Fix

### Problem

After save/load, tension adjustments silently stop working. Root cause chain:

1. `harmonicWaveform` and `noiseWaveform` are not persisted (too large).
2. On load, HNSep re-run is guarded by `HNSepCurveProcessor::hasActiveEdits()` — if all curves are at default values (voicing=1.0, breath=0.0, tension=0.0), HNSep is skipped.
3. Even if HNSep runs, per-note clips (`clipHarmonicWaveform`, `clipNoiseWaveform`) must be sliced from the global buffers via `ensureNoteHNClips()`.
4. Without clips, `recomputeMelForRange()` skips notes, and subsequent tension edits produce no audible change.

### Design

**Fix:** Make HNSep re-run unconditional on load when harmonic data is missing.

1. **Remove the `hasActiveEdits()` guard** in `MainComponent_ProjectIO.cpp` (line 200). The condition becomes simply:
   ```
   if (totalFrames > 0 && audioData.harmonicWaveform.getNumSamples() == 0)
   ```
   HNSep separation is a prerequisite for any tension/voicing/breath editing, so it must always be available after load, not just when edits already exist.

2. **Ensure `ensureNoteHNClips()` is always called after HNSep completes** — verify this is already in the callback chain, or add it explicitly.

3. **Trigger `recomputeMelForRange(0, totalFrames)` after clips are ready** — this applies any saved curve values to the mel spectrogram, restoring the exact audible state from before save.

4. **Add early validation** — after load, assert that if any note has non-default curve values, its clips are non-empty. Log a warning in debug builds if this invariant is violated.

**Files affected:**
- `Source/UI/Main/MainComponent_ProjectIO.cpp` — remove hasActiveEdits guard, ensure full recomputation chain
- `Source/Utils/HNSepCurveProcessor.cpp` — possibly add post-load validation

---

## 4. pitchOffset Semantic Unification

### Problem

Currently `pitchOffset` is a transient drag-time variable: during drag it accumulates the vertical displacement, on mouse-up it is baked into `midiNote` and reset to 0. This means:
- After dragging, `midiNote` no longer reflects the analysis-time pitch.
- Reset (`NoteEditUtils`) clears `pitchOffset` but cannot restore the original `midiNote` because it has been overwritten.
- The user cannot "return to original position" without full undo.

### Design

**Core invariant:** `midiNote` is set once during analysis (GAME detector output) and never modified afterward. All user-initiated pitch displacement accumulates in `pitchOffset`. `getAdjustedMidiNote()` = `midiNote + pitchOffset` gives the effective pitch.

**Changes by file:**

### SelectHandler.cpp — Single note drag

**`endNoteDrag()` (~line 359-390):**
- Remove: `draggedNote->setMidiNote(finalMidiNote); draggedNote->setPitchOffset(0.0f);`
- Keep pitchOffset as-is after drag ends.
- Snap-to-semitone: `pitchOffset = round(midiNote + pitchOffset) - midiNote` (offset such that the adjusted pitch lands on a semitone).
- Undo action records `(oldPitchOffset, newPitchOffset)` instead of `(oldMidiNote, newMidiNote)`.

**Double-click snap (~line 1050-1069):**
- Same snap formula: `pitchOffset = ScaleUtils::snapMidiToSemitone(midiNote + pitchOffset) - midiNote`.
- `midiNote` unchanged.

**Drag cancel (~line 1088):**
- Restore `pitchOffset` to value saved at drag start (already the case).

### PitchEditor.cpp — Multi-note drag

**`endNotesDrag()` (~line 461-560):**
- Remove bake: no `setMidiNote`, no `setPitchOffset(0)`.
- `MultiNotePitchDragAction` stores `(oldPitchOffsets[], newPitchOffsets[])` per note.

### DragActions.h — Undo actions

**`NotePitchDragAction`:**
- Fields: `oldPitchOffset`, `newPitchOffset` (remove `oldMidi`, `newMidi`).
- Undo: `note->setPitchOffset(oldPitchOffset)`.
- Redo: `note->setPitchOffset(newPitchOffset)`.

**`MultiNotePitchDragAction`:**
- Fields: `std::vector<float> oldPitchOffsets`, `std::vector<float> newPitchOffsets`.
- Same pattern, per note.

### NoteActions.h — Snap actions

**`NoteSnapToSemitoneAction`:**
- Undo: restore `oldPitchOffset`.
- Redo: `note->setPitchOffset(snapped - midiNote)`.
- No `setMidiNote` calls.

**`MultiNoteSnapToSemitoneAction`:**
- Same pattern for vector of notes.

### PianoRollComponent.cpp — Note movement (~line 3118)

- Replace `setMidiNote(newValue)` with `setPitchOffset(currentOffset + delta)`.

### PitchToolController.cpp

- Review all `setMidiNote` calls (lines 425, 577, 585, 594, 644). Tilt operations work through `deltaPitch` and should not modify `midiNote`. If any of these calls are changing `midiNote` for tilt/variance purposes, they need to be rerouted through `pitchOffset` or removed.

### TransformParams.h (~line 40)

- If it modifies `midiNote`, change to `pitchOffset`.

### NoteEditUtils.cpp — Reset

- `note.setPitchOffset(0.0f)` — already correct. With the new semantics, this returns the note to its analysis-time position because `midiNote` is unchanged.

### NoteSplitter.cpp

- No change. Split notes inherit both `midiNote` and `pitchOffset` from the original.

### Note.h / Note.cpp

- `computeF0FromDelta()` — already uses `midiNote + pitchOffset + delta`. No change.
- `getAdjustedMidiNote()` — already returns `midiNote + pitchOffset`. No change.

### PitchCurveProcessor.cpp

- `computeBoundarySmoothingSegments()` — already uses `note.getMidiNote() + note.getPitchOffset()`. No change.

### ProjectSerializer.cpp

- Already saves and loads both `midiNote` and `pitchOffset`. No structural change. The semantic meaning of `pitchOffset` changes from "transient drag state" to "persistent user offset", but the serialization format is identical.

### PianoRollRenderer.cpp

- Already renders using `getAdjustedMidiNote()` and separate offset pixel calculation. No change.

**Files affected (writes):**
- `Source/UI/PianoRoll/States/SelectHandler.cpp`
- `Source/UI/PianoRoll/PitchEditor.cpp`
- `Source/Undo/DragActions.h`
- `Source/Undo/NoteActions.h`
- `Source/UI/PianoRollComponent.cpp`
- `Source/UI/PianoRoll/PitchToolController.cpp`
- `Source/Utils/TransformParams.h`

---

## Implementation Order

1. **Notes ordering** — foundational, other changes benefit from sorted notes
2. **Monitor mel/f0 alignment** — independent, small scope
3. **Tension save/load fix** — independent, small scope
4. **pitchOffset unification** — largest change, benefits from ordered notes being already done

## Out of Scope (Spec 2)

The following are deferred to the second spec (synthesis pipeline refactoring):
- TensionProcessor interface simplification
- StretchProcessor decoupling from UI
- mel path / pitch path separation
- Dead code removal (getAdjustedF0, composeGlobalWaveform, renderMapped*, etc.)
- Incremental synthesis blending simplification
- Action-driven dirty range and synthesis triggering
