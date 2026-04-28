# HachiTune Spec Compliance Fixes

**Date**: 2026-04-28
**Status**: Approved
**Baseline**: [2026-04-27-project-refactor-design.md](2026-04-27-project-refactor-design.md)

## Context

Audit of the codebase against the 2026-04-27 refactor spec found:

1. **Note reset bug** — after F0 drawing, reset doesn't restore from global `analysisData`; baked `originalDeltaPitch` is returned instead of true original
2. **Synthesis pop sounds** — mel assembled from per-note clipMel instead of global melSpectrogram; stale clipMel after stretch causes boundary discontinuities
3. **TreeValueMonitor unwired** — implementation complete but no menu item, command ID, or instantiation
4. **Dead code not removed** — `deltaScale/deltaOffset`, `sourceXxxCurve`, `f0Values`, `srcClipWaveform/clipWaveform`, `srcStartFrame/srcEndFrame` serialization, `F0FrameEdit.h` all still present
5. **Crossfade not simplified** — still using old complex blend mask instead of spec's 1-frame crossfade
6. **Note-local cache stale after stretch** — `basePitch` and other caches not refreshed after `recomputeFromMarkers()`

## Strategy

Complete the spec's replacement mechanisms first, then delete the old fields. Four phases, each verified independently before proceeding.

---

## Phase 1: Critical Bug Fixes

### 1a. Note Reset — Restore from Global AnalysisData

**Files:** `SelectHandler.cpp`, `DrawHandler.cpp`

**Current behavior:** `resetNoteToOriginal()` clears `note.deltaPitch` and rebuilds from `note.originalDeltaPitch`. After F0 drawing bakes the drawn curve into `originalDeltaPitch`, reset returns the drawn curve — not the analysis original.

**Fix:**

```
resetNoteToOriginal(Note& note):
  1. Slice analysisData.originalDeltaPitch[start..end) → note.setOriginalDeltaPitch()
  2. Slice analysisData.originalPitch[start..end) → note.setOriginalPitch()
  3. note.resetToolParams()
  4. note.setPitchOffset(0)
  5. Clear note.deltaPitch (rebuild picks up from restored originalDeltaPitch)
  6. Clear f0EditedMask for note's frame range
  7. rebuildAndNotify()
```

**Stretch awareness:** When note has been stretched (srcStartFrame != startFrame), the slice from analysisData uses the note's *source* frame range (`srcStartFrame..srcEndFrame`), then resampled to output duration via `CurveResampler::resampleLinear()`.

**Dedup:** Both `SelectHandler` and `DrawHandler` have identical `resetNoteToOriginal()`. Extract to a shared utility (e.g., a free function in a `NoteEditUtils` namespace or on `EditorController`) called by both handlers.

### 1b. TreeValueMonitor Wiring

**Files:** `Commands.h`, `MainComponent.h/.cpp`, `MenuHandler.cpp`

1. `Commands.h` — add `showTreeValueMonitor` command ID
2. `MainComponent` — add `std::unique_ptr<TreeValueMonitor>` member
3. `MenuHandler` — add "Project Monitor" item under View menu
4. `MainComponent::perform()` — on command, create (if null) and show the window, passing `project` pointer

---

## Phase 2: Synthesis Pipeline (Pop Sounds + Spec Compliance)

### 2a. Mel Workflow: Global-Only Source

**Current:** `IncrementalSynthesizer::synthesizeRegion()` starts from global mel, then iterates per-note and overwrites slices with per-note clipMel (via `resampleMelHybrid()`). After stretch, `recomputeFromMarkers()` reassembles global mel from per-note clips instead of stretching global mel.

**Target (matches spec):** Synthesis reads only global `melSpectrogram`. Curve edits and stretch write results back to global mel.

**Changes:**

Curve edit path:
- When voicing/breath/tension curves are edited, `EditorController` calls `TensionProcessor::processSegmentFromSTFT()` for each dirty note's frame range
- Result writes directly into `audioData.melSpectrogram[startFrame..endFrame)`
- IncrementalSynthesizer no longer does per-note mel override

Stretch path:
- `recomputeFromMarkers()` uses `StretchProcessor::stretchMel()` to interpolate global mel (instead of per-note reassembly)
- After stretch, global mel is continuous with no per-note boundary seams

**IncrementalSynthesizer simplification:**
- Remove the per-note mel override loop in `synthesizeRegion()`
- Flow becomes: slice global f0 + slice global mel → vocoder → write auditionBuffer

### 2b. Crossfade Simplification

**Current:** `generateBlendMask()` builds complex voiced/unvoiced mask with 512-sample smoothstep ramps.

**Target:** 1-frame (hop_size samples) linear fade at synthesis segment boundaries.

Justification: `ResynthRange` already extends to VAD=0 boundaries (silence), so crossfade occurs at silent regions — no audible artifacts from simple linear fade.

**Implementation:**
- Replace `generateBlendMask()` with a simple function that returns a ramp buffer: `hop_size` samples fade-in at start, `hop_size` samples fade-out at end, 1.0 in between
- Apply: `output = synth * ramp + auditionBuffer * (1 - ramp)` at boundaries only

### 2c. auditionBuffer Variable Bug

**File:** `IncrementalSynthesizer.cpp`

Line using `startSample` should use `startSamp` (the locally declared variable). One-line fix.

### 2d. Note-Local Cache Refresh After Stretch

**File:** `Project.cpp`

After `recomputeFromMarkers()` updates global data, call `refreshNoteCaches()` (see Phase 3d) to sync all note-local caches from global state. This fixes stale `basePitch`, `deltaPitch`, `f0`, and mel caches.

---

## Phase 3: Global Data Slice Replacement

### 3a. Note Segment Info in AnalysisData

**Current:** `Note::srcStartFrame/srcEndFrame` are per-note serialized fields representing GAME segmentation boundaries.

**Target:** Store in `AnalysisData` as immutable analysis result; Note keeps runtime cache only.

```cpp
// In AnalysisData
struct NoteSegment {
    int srcStartFrame;
    int srcEndFrame;
};
std::vector<NoteSegment> noteSegments;
```

- GAME analysis writes segments to `analysisData.noteSegments`
- Note's `srcStartFrame/srcEndFrame` initialized from `analysisData.noteSegments[noteIndex]` at cache refresh
- ProjectSerializer: serialize `noteSegments` in `analysisData` block; remove per-note `srcStartFrame/srcEndFrame` serialization (with legacy compat read)

### 3b. Replace note.f0Values

**Current:** `Note::f0Values` stores original detected F0 per note, set during analysis, used in `getAdjustedF0()`, NoteSplitter, PitchEditor.

**Target:** Slice from `analysisData.originalF0[srcStart..srcEnd)` on demand.

- Remove `Note::f0Values` field
- `getAdjustedF0()` → takes `const AnalysisData&` and frame range, computes on-the-fly
- AudioAnalyzer / EditorController: stop setting per-note f0Values; write only to global analysisData.originalF0
- NoteSplitter: slice from global analysisData instead of splitting per-note array
- PitchEditor: already reads from global audioData.f0

### 3c. Replace clipWaveform / srcClipWaveform

**Current:** `Note::clipWaveform` used for PianoRollComponent waveform rendering. `Note::srcClipWaveform` used for source audio reference.

**Target:** Slice from global buffers on demand.

- PianoRollComponent waveform rendering: read directly from `audioData.waveform` using `[startFrame*hopSize .. endFrame*hopSize)`. Use a lightweight view/slice utility, not a copy.
- `srcClipWaveform`: read from `audioData.originalWaveform` using source frame range when needed
- If per-note waveform caching is needed for render performance, keep as a runtime cache field (not serialized) populated by `refreshNoteCaches()`

### 3d. Unified refreshNoteCaches()

**File:** `Project.h/.cpp`

```cpp
void Project::refreshNoteCaches(int noteIndex = -1) {
    // For each note (or specified note):
    // From analysisData:
    //   originalDeltaPitch = analysisData.originalDeltaPitch[srcStart..srcEnd) (resampled if stretched)
    //   originalPitch = analysisData.originalPitch[srcStart..srcEnd) (resampled if stretched)
    // From editedData:
    //   basePitch = editedData.basePitch[start..end)
    //   deltaPitch = editedData.deltaPitch[start..end)
    //   f0 = editedData.f0[start..end)
    //   voicingCurve/breathCurve/tensionCurve = editedData.xxx[start..end)
    // From audioData:
    //   clipMel = audioData.melSpectrogram[start..end) (if cache retained)
    //   srcStartFrame/srcEndFrame from analysisData.noteSegments[i]
}
```

**Call sites:** After every operation that modifies global data:
- Stretch (`recomputeFromMarkers`)
- Pitch edit (drag, tool, draw)
- Curve edit
- Note split/merge
- Analysis complete
- Project load/deserialize

---

## Phase 4: Dead Code Cleanup

Execute after Phases 1-3 are complete and functionally verified.

| Delete | Files |
|--------|-------|
| `Note::deltaScale` / `Note::deltaOffset` | Note.h/cpp, TransformParams.h, ProjectSerializer.cpp, PitchCurveProcessor.cpp |
| `Note::sourceVoicingCurve/Breath/Tension` | Note.h/cpp, ProjectSerializer.cpp |
| `Note::f0Values` (replaced by 3b) | Note.h/cpp, AudioAnalyzer.cpp, EditorController.cpp, NoteSplitter.cpp |
| `Note::srcClipWaveform` / `clipWaveform` (replaced by 3c) | Note.h/cpp, PianoRollComponent.cpp |
| `srcStartFrame/srcEndFrame` serialization (replaced by 3a) | ProjectSerializer.cpp |
| `Undo/F0FrameEdit.h` | Undo/ directory |
| Duplicate `resetNoteToOriginal` (replaced by 1a) | DrawHandler.cpp (keep shared version) |
| Old `generateBlendMask()` (replaced by 2b) | IncrementalSynthesizer.cpp |

Legacy compat: ProjectSerializer must still READ old format fields (deltaScale, deltaOffset, sourceXxxCurve, per-note srcStartFrame/srcEndFrame, f0Values) for backward compatibility, but silently discard them (or migrate to new locations). New saves omit them.

---

## Implementation Order

```
Phase 1a: Note reset bug fix
Phase 1b: TreeValueMonitor wiring
  ↓ verify: build + manual test reset after draw
Phase 2a: Mel workflow → global-only
Phase 2b: Crossfade simplification
Phase 2c: auditionBuffer variable fix
Phase 2d: refreshNoteCaches after stretch
  ↓ verify: build + test stretch + curves + playback (no pops)
Phase 3a: noteSegments in AnalysisData
Phase 3b: Replace f0Values
Phase 3c: Replace clipWaveform/srcClipWaveform
Phase 3d: Unified refreshNoteCaches
  ↓ verify: build + full workflow test
Phase 4: Dead code deletion
  ↓ verify: build + serialization round-trip test (save/load)
```
