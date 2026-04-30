# HachiTune Full Refactoring Design

**Date**: 2026-04-29
**Status**: Draft
**Approach**: Bottom-up, 4 phases

---

## Executive Summary

Decouple core algorithms from GUI, establish EditedData as the single source
of truth, clean up the synthesis pipeline (mel vs f0 separation), and make key
modules independently testable. The refactoring proceeds bottom-up: lowest-level
algorithm modules first, then data model cleanup, then serialization and
pipeline integration, then UI/Undo adaptation.

---

## Phase 1: Algorithm Module Extraction and Cleanup

### 1A. Move WarpMarkerProcessor out of Project.cpp

**Current state**: `WarpMarkerProcessor` namespace is declared in
`Source/Utils/WarpMarkerProcessor.h` but its ~300-line implementation lives in
`Source/Models/Project.cpp` (lines ~1451–1749).

**Target**:
- Move full implementation to `Source/Utils/WarpMarkerProcessor.cpp`.
- `WarpMarkerProcessor::recomputeFromMarkers()` accepts `Project&` — this
  dependency on Project is acceptable for orchestration, but all pure algorithm
  calls should route through `StretchProcessor`.
- Remove stretch-related code from `Project.cpp`.

### 1B. TensionProcessor Rewrite with JUCE DSP

**Current state**: Custom FFT implementation with hand-written Hann window,
2048-sample FFT, manual overlap-add. Works from precomputed STFT cache stored
in `Project.harmonicSTFT` / `Project.noiseSTFT`.

**Target**:
- Replace custom FFT with `juce::dsp::FFT` (size 2048).
- Replace hand-written Hann with `juce::dsp::WindowingFunction<float>`.
- Keep the same STFT cache strategy (forward FFT done once at HNSep time,
  cached as interleaved real/imag pairs in Project).
- Keep the same API: `processSegmentFromSTFT(harmonicSTFT, noiseSTFT,
  totalSTFTFrames, startFrame, endFrame, voicingCurve, breathCurve,
  tensionCurve, numFrames)` → returns output waveform.
- Internal changes only: same spectral tilt algorithm (per-bin dB ramp ×
  tension/100 × maxTiltDb=12), same RMS normalization after tilt.
- `processSegment()` variant (operates from raw waveforms) also uses
  `juce::dsp::FFT` for forward+inverse.

**Input/Output contract** (unchanged):
```
Input:
  - harmonicSTFT: interleaved [real, imag] × bins × frames
  - noiseSTFT: same layout
  - voicingCurve[numFrames]: 0..100 (harmonic gain %)
  - breathCurve[numFrames]: 0..100 (noise gain %)
  - tensionCurve[numFrames]: -100..+100 (spectral tilt)
Output:
  - std::vector<float> waveform (overlap-add synthesis)
```

### 1C. StretchProcessor Mel Stretch Fix

**Current state**: `StretchProcessor::stretchMel()` exists and uses linear
interpolation. However `WarpMarkerProcessor::recomputeFromMarkers()` calls it
on the global `audioData.melSpectrogram` which at that point may contain
partially-updated mel from HNSep processing.

**Target pipeline order** (mel generation):
1. Start from original HNSep decomposition (harmonicWaveform + noiseWaveform).
2. Apply tension/breath/voicing curves via `TensionProcessor` in source domain.
3. Compute mel from processed audio (per-note, in source timeline).
4. Stretch mel to output timeline using warp markers.

This means mel stretch must operate on a **source-domain mel** (before stretch),
not an output-domain mel that has already been partially written.

**New function**:
```cpp
// Builds output mel from per-note source-domain mels + warp markers
static std::vector<std::vector<float>> StretchProcessor::buildOutputMel(
    const std::vector<std::vector<float>>& sourceMel,  // [sourceFrames][numMels]
    const std::vector<WarpMarker>& markers,
    int outputFrameCount);
```

The existing `stretchMel()` can be kept as a low-level utility.

---

## Phase 2: AudioData Slimming + EditedData as Single Source of Truth

This phase corresponds to the existing Phase 1 design doc
(`2026-04-29-project-data-model-phase1-design.md`). Summary:

### 2A. Remove from AudioData

Delete these fields from `AudioData`:
- `f0`, `baseF0`, `basePitch`, `deltaPitch`
- `voicedMask`, `vadMask`
- `voicingCurve`, `breathCurve`, `tensionCurve`
- `f0EditedMask` (abolished — the new reset/bake flow makes it unnecessary)

Retain in `AudioData` (runtime cache only, never serialized):
- `waveform`, `originalWaveform`
- `harmonicWaveform`, `noiseWaveform`
- `melSpectrogram` (source-domain mel, rebuilt from HNSep + tension)
- `sampleRate`
- `segmentChunkRanges`, `segmentDebugChunks`
- `incrementalDebug`

### 2B. Migrate All Callers to EditedData

Every read/write of the removed AudioData fields must migrate:

| Module | Current reads audioData.X | Target reads editedData.X |
|--------|---------------------------|---------------------------|
| PitchCurveProcessor | basePitch, deltaPitch, voicedMask | editedData.basePitch, .deltaPitch, .voicedMask |
| composeF0 / composeF0InPlace | audioData.basePitch + deltaPitch | editedData.basePitch + deltaPitch |
| IncrementalSynthesizer | audioData.f0, vadMask, voicedMask | editedData.f0, .vadMask, .voicedMask |
| PitchEditor / DrawHandler | audioData.f0, deltaPitch, f0EditedMask | editedData.f0, .deltaPitch |
| SelectHandler (drag) | audioData.basePitch, baseF0, f0 | editedData.basePitch, computed baseF0, .f0 |
| PianoRollRenderer | audioData.f0, basePitch, deltaPitch | editedData.f0, .basePitch, .deltaPitch |
| Undo actions (F0Actions, DragActions) | audioData.f0, deltaPitch, masks | editedData ranges |
| HNSepCurveProcessor | audioData.voicingCurve, etc. | editedData.voicingCurve, etc. |

### 2C. Project Helpers

Add to `Project`:
- `getFrameCount()`: from `editedData.f0.size()` (or mel length before analysis)
- `refreshNoteCaches()`: fill note-local caches from EditedData + AnalysisData
- `refreshNoteCachesForRange(start, end)`: partial refresh
- `validateFrameData()`: consistency check across all arrays

### 2D. Note Cache Semantics

Each Note contains local caches (not serialized):
- `basePitch[]` — slice of editedData.basePitch for this note's range
- `deltaPitch[]` — slice of editedData.deltaPitch
- `originalPitch[]` — slice of analysisData.originalPitch
- `originalDeltaPitch[]` — slice of analysisData.originalDeltaPitch
- `voicingCurve[]`, `breathCurve[]`, `tensionCurve[]` — slices of editedData curves

**Edit flow** (non-destructive: tilt, variance, smooth, filter):
1. Modify note-local deltaPitch (apply transforms on originalDeltaPitch)
2. Write transformed result into both note-local deltaPitch AND global
   editedData.deltaPitch[startFrame..endFrame]
3. Recompose editedData.f0 for affected range

**Edit flow** (destructive: draw mode):
1. First bake non-destructive transforms into note's originalDeltaPitch
   (originalDeltaPitch = current deltaPitch)
2. Reset non-destructive parameters (tilt=0, variance=1, smooth=0, filter=0)
3. Draw directly into note.originalDeltaPitch + note.deltaPitch + global editedData

**Reset**:
1. Copy from analysisData.originalDeltaPitch[noteRange] → note.originalDeltaPitch
2. Reset all non-destructive parameters
3. Recompute note.deltaPitch = note.originalDeltaPitch (no transforms)
4. Write to global editedData.deltaPitch + recompose f0

---

## Phase 3: Serialization Format Update + Pipeline Integration

### 3A. Updated Save Format

```json
{
    "formatVersion": 1,
    "name": "Untitled",
    "globalPitchOffset": 0.0,
    "formantShift": 0.0,
    "volume": 0.0,
    "scaleMode": -1,
    "scaleRootNote": -1,
    "pitchReferenceHz": 440,
    "showScaleColors": true,
    "snapToSemitones": false,
    "doubleClickSnapMode": 0,
    "timelineDisplayMode": 0,
    "timelineBeatNumerator": 4,
    "timelineBeatDenominator": 4,
    "timelineTempoBpm": 120.0,
    "timelineGridDivision": 4,
    "timelineSnapCycle": false,
    "loop": {
        "enabled": false,
        "start": 0.0,
        "end": 0.0
    },
    "audioPath": "...",
    "audioSha256": "...",
    "sampleRate": 44100,
    "notes": [...],
    "warpMarkers": [...],
    "analysisData": {...},
    "editedData": {...}
}
```

**Per-note** (saved):
```json
{
    "startFrame": 112,
    "endFrame": 159,
    "midiNote": 65.016,
    "pitchOffset": 0.0,
    "volumeDb": 0.0,
    "rest": false,
    "vibrato": {
        "enabled": false,
        "startFrame": 0,
        "lengthFrames": 0,
        "rateHz": 5.0,
        "depthSemitones": 0.0,
        "phaseRadians": 0.0,
        "mix": 0.0,
        "fadeInFrames": 0,
        "fadeOutFrames": 0
    },
    "tiltLeft": 0.0,
    "tiltRight": 0.0,
    "varianceScale": 0.0,
    "smoothLeftFrames": 0,
    "smoothRightFrames": 0,
    "highPassFilterStrength": 0,
    "lowPassFilterStrength": 0
}
```

**NOT saved in notes**: srcStartFrame, srcEndFrame, local pitch arrays, local
curves, waveform clips, mel clips, deltaScale, deltaOffset.

**Always saved** (even at default): highPassFilterStrength, lowPassFilterStrength
(always serialized, including when zero).

**vibratoMix**: Retained as a 0..1 blend control between original delta and
vibrato signal. Serialized in saves under `vibrato.mix`.

### 3B. Synthesis Pipeline (Clarified Order)

```
┌─────────────────────────────────────────────────────────────┐
│ MEL PIPELINE (produces mel for vocoder)                     │
│                                                             │
│  1. HNSep model → harmonicWaveform, noiseWaveform (source)  │
│  2. TensionProcessor: apply voicing/breath/tension curves   │
│     on source-domain H/N clips → processed audio            │
│  3. Compute mel from processed audio (source timeline)      │
│  4. StretchProcessor::buildOutputMel() → stretched mel      │
│                                                             │
│ F0 PIPELINE (produces f0 for vocoder)                       │
│                                                             │
│  editedData.f0 (already composed from basePitch+deltaPitch) │
│  If stretch: StretchProcessor::stretchEditedData() already  │
│  handles f0 recomposition after resampling basePitch+delta  │
│                                                             │
│ VOCODER                                                     │
│                                                             │
│  Input: f0[range] from editedData + mel[range] from cache   │
│  Output: synthesized waveform segment                       │
│                                                             │
│ BLEND                                                       │
│                                                             │
│  auditionBuffer = originalWaveform (initially)              │
│  On synthesis complete:                                     │
│    1-frame linear crossfade at segment boundaries           │
│    Overwrite dirty region in auditionBuffer                 │
└─────────────────────────────────────────────────────────────┘
```

**Blend simplification**: Current blending uses complex blend masks with smooth
ramps. Replace with simple 1-frame (hop_size samples = 512) linear crossfade at
the start and end of each resynthesized segment. Since ResynthRange extends to
VAD=0 boundaries (silence), crossfade occurs at near-silent regions and
artifacts are negligible.

### 3C. Dirty Range and Resynthesis Scope

- **Pitch edit** → dirty range = affected note(s) frame range
- **Pitch smoothing** → dirty range = current note ± 1 neighbor
- **Tension/breath/voicing edit** → dirty range = affected note's frame range
- **Stretch** → dirty range = [prevMarker.outputFrame, nextMarker.outputFrame]

In all cases: expand dirty range outward to nearest VAD=0 frame → this is the
ResynthRange. Resynthesize this range, crossfade into auditionBuffer.

### 3D. Plugin Mode Cache

In plugin mode (`PluginProcessor`):
- `Project` persists in `PluginProcessor` across GUI open/close.
- HNSep results (harmonicSTFT, noiseSTFT, harmonicWaveform, noiseWaveform)
  are cached in Project/AudioData.
- Reopening GUI does NOT trigger re-analysis.
- Only explicit user action (re-analyze button) triggers new analysis.

---

## Phase 4: UI and Undo Adaptation

### 4A. Undo System Migration

Replace per-frame F0FrameEdit with range-snapshot undo (already designed in
`2026-04-29-undo-and-monitor-design.md`):

- `F0RangeSnapshot`: captures editedData.deltaPitch + .f0 + .voicedMask for
  [start, end) range.
- `CurveRangeSnapshot`: captures editedData curves for a range.
- Remove raw pointer storage; use Project& reference.
- All undo/redo operates on EditedData ranges + calls refreshNoteCachesForRange.

### 4B. UI Handler Migration

**SelectHandler (drag)**:
- Read basePitch from editedData, not audioData.
- Write pitch changes to editedData.basePitch + recompose editedData.f0.
- Build drag preview from editedData.

**DrawHandler / PitchEditor**:
- Write to editedData.deltaPitch + .f0 directly.
- Undo snapshots capture editedData ranges.

**StretchHandler**:
- Remove custom stretch logic from StretchHandler.
- StretchHandler only handles mouse interaction and delegates to
  `WarpMarkerProcessor::recomputeFromMarkers()` which uses the new pipeline.
- No direct mel manipulation in UI code.

### 4C. TreeValueMonitor Enhancement

Display:
- Root project properties (editable in monitor for testing)
- AnalysisData array lengths + non-empty flags
- EditedData array lengths + first/last few values
- AudioData cache state (mel shape, waveform length, STFT cached)
- Per-note: startFrame, endFrame, midiNote, cache sizes, dirty state
- Warp markers list
- Validation result from `validateFrameData()`
- Dirty range indicators

Listens to all ProjectChangeType events and refreshes affected sections.

### 4D. composeF0 and Related Functions

- `PitchCurveProcessor::composeF0()` reads from `editedData` (not audioData).
- `composeF0InPlace()` writes into `editedData.f0` only.
- `rebuildBaseFromNotes()` writes into `editedData.basePitch`, then
  recomposes `editedData.f0`.
- `rebuildDeltaFromNotes()` writes into `editedData.deltaPitch`, then
  recomposes `editedData.f0`.
- `baseF0` is never stored — compute on-the-fly from editedData.basePitch
  via `midiToFreq()`.

---

## Cross-Cutting Concerns

### Thread Safety

- EditedData is only mutated on the message thread (same as current AudioData).
- Audio playback thread reads `auditionBuffer` (lock-free swap via SpinLock).
- Synthesis worker reads EditedData snapshot (stack-local copy of range).
- No change to threading model needed.

### Note Split/Merge

Note splitting is purely a GUI/control concept:
- Splitting a note creates two notes with updated startFrame/endFrame.
- Global mel is unaffected.
- Note-local caches are re-sliced from global editedData.
- No re-synthesis needed unless pitch parameters differ.

### Backward Compatibility

- Legacy `pitchData` load still supported → populates AnalysisData + EditedData.
- Old-format notes with `srcStartFrame` → computed from warpMarkers on load.
- vibratoMix → if present in file, map to vibrato.enabled (mix > 0 → enabled).
- deltaScale/deltaOffset → silently ignored.

---

## Implementation Order (Bottom-Up)

```
Phase 1A: WarpMarkerProcessor → own .cpp file
Phase 1B: TensionProcessor → JUCE DSP (FFT + Window)
Phase 1C: StretchProcessor mel fix + buildOutputMel()
     ↓
Phase 2A: Remove AudioData editable fields
Phase 2B: Migrate all callers to EditedData
Phase 2C: Project helpers (refreshNoteCaches, validateFrameData)
Phase 2D: Note cache semantics enforcement
     ↓
Phase 3A: Serialization format update
Phase 3B: Synthesis pipeline integration (new mel flow)
Phase 3C: Dirty range simplification
Phase 3D: Plugin mode cache rules
     ↓
Phase 4A: Undo system migration
Phase 4B: UI handler migration
Phase 4C: TreeValueMonitor enhancement
Phase 4D: composeF0 and friends final cleanup
```

Each phase should result in a compilable (though possibly partially broken)
state. Phase boundaries are commit points.

---

## Success Criteria (Overall)

1. AudioData contains ONLY runtime cache (waveform, mel, STFT, segments).
2. EditedData is the sole source for synthesis f0 and curves.
3. AnalysisData is immutable after analysis.
4. TensionProcessor uses juce::dsp::FFT and WindowingFunction.
5. WarpMarkerProcessor lives in its own .cpp, not in Project.cpp.
6. Mel pipeline: tension(source) → mel(source) → stretch(output).
7. Blend is simple 1-frame crossfade at VAD=0 boundaries.
8. Serialization matches the target format (no dead properties).
9. TreeValueMonitor displays all data layers with validation.
10. Undo operates on EditedData ranges via snapshots.
11. Note-local data is explicitly a cache of global data.
12. Plugin mode preserves HNSep cache across GUI lifecycle.
