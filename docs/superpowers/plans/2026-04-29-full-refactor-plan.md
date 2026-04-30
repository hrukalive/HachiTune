# Full Refactor Implementation Plan

**Date**: 2026-04-29
**Design Spec**: `docs/superpowers/specs/2026-04-29-full-refactor-design.md`
**Approach**: Bottom-up, 4 phases

---

## Phase 1: Data Model Migration (AudioData → EditedData)

### Task 1.1: Remove redundant fields from AudioData

**Files**: `Source/Models/Project.h`
**What**: Delete from `AudioData`:
- `f0` (line 89)
- `baseF0` (line 90)
- `basePitch` (line 91)
- `deltaPitch` (line 92)
- `voicingCurve` (line 93)
- `breathCurve` (line 94)
- `tensionCurve` (line 95)
- `voicedMask` (line 96)
- `vadMask` (line 97)
- `f0EditedMask` (line 98)

**Keep in AudioData** (runtime cache, never serialized):
- `waveform`, `originalWaveform`
- `harmonicWaveform`, `noiseWaveform`
- `melSpectrogram`
- `sampleRate`
- `segmentChunkRanges`, `segmentDebugChunks`, `incrementalDebug`

**Verification**: Build will fail with ~hundreds of errors pointing to all call sites.

---

### Task 1.2: Migrate all `audioData.xxx` references to `editedData.xxx`

**Pattern**: Search for `audioData.basePitch`, `audioData.deltaPitch`, `audioData.f0`,
`audioData.voicedMask`, `audioData.vadMask`, `audioData.voicingCurve`,
`audioData.breathCurve`, `audioData.tensionCurve`, `audioData.f0EditedMask`.

Replace with:
- Read paths → `editedData.basePitch`, etc.
- The few analysis write paths → `analysisData.originalXxx`

**Key call sites to fix**:
1. `PitchCurveProcessor::composeF0()` (line 870-889): reads `audioData.basePitch/deltaPitch/voicedMask` → change to `editedData`
2. `PitchCurveProcessor::composeF0InPlace()` (line 891-906): writes to `audioData.f0` → write to `editedData.f0` only
3. `Project::getAdjustedF0()` (line 254-290): reads `audioData.basePitch/deltaPitch/voicedMask` → `editedData`
4. `Project::getAdjustedF0ForRange()` (line 292-343): same
5. `WarpMarkerProcessor::recomputeFromMarkers()` (line 1660-1749): writes `audioData.voicedMask`, `audioData.melSpectrogram`
6. `HNSepCurveProcessor` (all curve reads/writes)
7. All UI components that display pitch/curves

**Verification**: After fixing all sites, build should compile. TreeValueMonitor should show data in EditedData.

---

### Task 1.3: Update Note vibrato model

**Files**: `Source/Models/Note.h`, `Source/Models/Note.cpp`
**What**: Replace vibrato fields:
```cpp
// REMOVE:
float vibratoFadeInMs = 0.0f;
float vibratoFadeOutMs = 0.0f;

// ADD:
int vibratoStartFrame = 0;      // offset from note start
int vibratoLengthFrames = 0;    // duration of vibrato segment
int vibratoFadeInFrames = 0;    // fade within vibrato start
int vibratoFadeOutFrames = 0;   // fade within vibrato end
```

Update getters/setters accordingly.

**Dependent sites**: `Project::getAdjustedF0()`, `Project::getAdjustedF0ForRange()`,
serializer, undo snapshot, vibrato UI panel.

---

### Task 1.4: Update ProjectSerializer for new format

**Files**: `Source/Models/ProjectSerializer.cpp`
**What**:
- Save `analysisData` and `editedData` as separate JSON objects
- Add vibrato new fields (startFrame, lengthFrames, fadeInFrames, fadeOutFrames, mix)
- Remove `deltaScale`, `deltaOffset` from note saves
- Always serialize `highPassFilterStrength`, `lowPassFilterStrength` (even at 0)
- Don't save `srcStartFrame`/`srcEndFrame` in notes (they come from analysisData)
  - Actually KEEP them for now since they represent warp-independent source positions
- Format version bump to 2
- Backward compat: can still load version 1 by converting old format

**Verification**: Save/load roundtrip preserves all data.

---

### Task 1.5: Update SnapshotHelper for new data layout

**Files**: `Source/Undo/SnapshotHelper.h`, `Source/Undo/SnapshotHelper.cpp`
**What**: Undo snapshots must capture/restore `editedData` instead of `audioData` curves.

---

## Phase 2: Algorithm Module Extraction

### Task 2.1: Extract WarpMarkerProcessor from Project.cpp

**Files**: Create `Source/Utils/WarpMarkerProcessor.cpp` (the .h already exists)
**What**: Move the ~300 lines of implementation (Project.cpp:1451-1749) into its own .cpp file.
Also extract the helper functions it depends on:
- `getProjectSourceFrameLimit()` (currently anonymous namespace in Project.cpp)
- `sortedUniqueMarkers()` (same)
- `fitFloatCurve()`, `fitBoolCurve()` (same)
- `buildSourceVoicedMask()` (same)
- `rebuildVadMaskFromWaveform()` (same)

Move these to either WarpMarkerProcessor.cpp or a shared utility header.

**Verification**: Build compiles, stretch operations still work identically.

---

### Task 2.2: Rewrite TensionProcessor FFT with JUCE DSP

**Files**: `Source/Audio/TensionProcessor.cpp`, `Source/Audio/TensionProcessor.h`
**What**: Replace the hand-rolled Cooley-Tukey FFT (lines 376-508) with:
```cpp
juce::dsp::FFT fft{11}; // log2(2048) = 11
juce::dsp::WindowingFunction<float> window{2048, juce::dsp::WindowingFunction<float>::hann};
```

Replace:
- `forwardFFT()` → use `juce::dsp::FFT::performRealOnlyForwardTransform()`
- `inverseFFT()` → use `juce::dsp::FFT::performRealOnlyInverseTransform()`
- `hannWindow` vector → `juce::dsp::WindowingFunction<float>`

The JUCE FFT uses an interleaved format. Adapt the spectral tilt loop accordingly.
The algorithm logic (spectral tilt, RMS normalization, overlap-add) stays the same.

**Also**: Remove `forwardFFT` from the public API since the STFT cache path
(`processSegmentFromSTFT`) already receives pre-computed spectra.

**Verification**: Tension processing produces same audible result (can test with a known signal).

---

### Task 2.3: Fix composeF0 to read from EditedData exclusively

**Files**: `Source/Utils/PitchCurveProcessor.cpp`
**What**: `composeF0()` currently reads `project.getAudioData().basePitch`. Change to:
```cpp
const auto& ed = project.getEditedData();
const int totalFrames = static_cast<int>(ed.basePitch.size());
// ... use ed.basePitch, ed.deltaPitch, ed.voicedMask
```

`composeF0InPlace()` currently writes back to both audioData and editedData. Change to:
```cpp
void composeF0InPlace(Project& project, bool applyUvMask, float globalPitchOffset)
{
    auto composed = composeF0(project, applyUvMask, globalPitchOffset);
    project.getEditedData().f0 = std::move(composed);
    project.notifyListeners(ProjectChangeType::EditedDataChanged);
}
```

Similarly update `rebuildBaseFromNotes()` — it should write to `editedData.basePitch`
and `editedData.deltaPitch` directly instead of `audioData`.

---

### Task 2.4: Fix WarpMarkerProcessor::recomputeFromMarkers to use EditedData

**What**: Currently writes `audioData.voicedMask` (line 1730) and
stretches `audioData.melSpectrogram` (line 1731-1736). Change to:
- Update `editedData.voicedMask` instead
- Keep mel stretch on `audioData.melSpectrogram` (mel is a runtime cache in audioData)

Also: `rebuildVadMaskFromWaveform()` should write to `editedData.vadMask`.

---

## Phase 3: Synthesis Pipeline Refactor

### Task 3.1: Define clear mel computation pipeline

**Current flow** (broken):
- `WarpMarkerProcessor::recomputeFromMarkers()` stretches mel from audioData
- TensionProcessor computes mel from mixed waveform after tension
- These two are conflated

**Target flow**:
1. HNSep → produces `harmonicWaveform` + `noiseWaveform` (source domain)
2. TensionProcessor → applies voicing/breath/tension → produces `processedHarmonic` + `processedNoise`
3. Compute mel from (processedH + processedN) in source domain
4. StretchProcessor::stretchMel() → stretches mel to output domain

**Implementation**:
- Add helper function: `computeProcessedMel(project, startFrame, endFrame)` that:
  1. Reads STFT cache or raw waveforms for [startFrame, endFrame)
  2. Applies tension/voicing/breath
  3. Computes mel from result
  4. Returns source-domain mel
- The stretch step happens separately when warp markers exist

---

### Task 3.2: Simplify audition buffer blending

**Current**: Complex blending logic in `composeGlobalWaveform()` with multiple strategies.
**Target**: Simple crossfade-based blend:
- `auditionBuffer` starts as copy of `originalWaveform`
- When resynthesis produces output for a range, overwrite with 1-frame crossfade at edges
- DirtyRange → expand to VAD=0 boundaries → ResynthRange

---

### Task 3.3: Ensure synthesis reads only from global EditedData

**Current**: `getAdjustedF0()` reads from audioData, and the incremental synthesizer
uses per-note data in some cases.
**Target**: Synthesis pipeline ONLY reads `editedData.f0` (plus globalPitchOffset and vibrato).
Per-note caches are display-only.

---

## Phase 4: Vibrato & UI Integration

### Task 4.1: Implement new vibrato model in getAdjustedF0

**Current**: Vibrato applies across entire note duration, fadeIn/fadeOut in ms.
**Target**: Vibrato applies only to [note.startFrame + vibratoStartFrame, 
+vibratoLengthFrames), with fadeInFrames/fadeOutFrames within that range.

```cpp
const int vibStart = note.getStartFrame() + note.getVibratoStartFrame();
const int vibEnd = vibStart + note.getVibratoLengthFrames();
// only apply vibrato in [vibStart, vibEnd)
// fadeIn from vibStart over fadeInFrames
// fadeOut ending at vibEnd over fadeOutFrames
```

---

### Task 4.2: Update TreeValueMonitor to show EditedData

**Files**: `Source/UI/Debug/TreeValueMonitor.h`, `Source/UI/Debug/TreeValueMonitor.cpp`
**What**: Ensure the monitor displays:
- `editedData` frame count, non-zero frames
- `analysisData` frame count
- Which note caches are in sync with global data
- Warp marker count and status

---

### Task 4.3: Update all UI to read from EditedData for display

**Ensure**:
- Piano roll pitch display reads `editedData.basePitch + editedData.deltaPitch`
- Curve editors read/write `editedData.voicingCurve/breathCurve/tensionCurve`
- Note local caches are refreshed from editedData after any global change

---

## Execution Order (Dependencies)

```
1.1 (remove fields) ──┐
                       ├─→ 1.2 (fix all refs) ──→ 2.3 (composeF0) ──→ 2.4 (warp)
1.3 (vibrato model) ──┘                                                    │
                                                                            v
2.1 (extract WarpMarkerProcessor) ────────────────────────────────→ 3.1 (mel pipeline)
2.2 (TensionProcessor JUCE FFT) ──────────────────────────────────→ 3.1
                                                                            │
1.4 (serializer) ─────────────────────────────────────────────────→ 4.2 (monitor)
1.5 (undo) ───────────────────────────────────────────────────────→ 4.3 (UI)
                                                                            │
4.1 (vibrato implementation) ←─── depends on 1.3 + 2.3                     │
3.2 (blend simplification) ←── depends on 3.1                              │
3.3 (synth reads editedData) ←── depends on 2.3 + 3.1                     │
```

**Recommended batch order**:
1. **Batch A** (parallel): 1.1 + 1.3 + 2.1 + 2.2
2. **Batch B** (parallel after A): 1.2 + 2.3 + 2.4
3. **Batch C** (parallel after B): 1.4 + 1.5 + 3.1
4. **Batch D** (parallel after C): 3.2 + 3.3 + 4.1 + 4.2 + 4.3

---

## Risk Mitigation

1. **Build breakage**: Task 1.1 intentionally breaks the build. Task 1.2 fixes it.
   Do them in a single commit if possible, or use a compatibility shim temporarily.
   
2. **FFT precision**: JUCE's FFT may produce slightly different floating-point results.
   This is acceptable — audio quality is perceptually identical.

3. **Serialization backward compat**: Version 1 files must still load. The serializer
   should detect format version and convert on load.

4. **Undo**: Snapshot capture/restore must be updated alongside data model changes.
   If they get out of sync, undo will crash.

---

## Success Criteria

- [ ] `AudioData` contains ONLY: waveforms, melSpectrogram, sampleRate, debug info
- [ ] All pitch/curve data lives exclusively in `EditedData`
- [ ] `AnalysisData` is never mutated after initial analysis
- [ ] `composeF0()` reads from `editedData` only
- [ ] WarpMarkerProcessor is in its own .cpp file
- [ ] TensionProcessor uses `juce::dsp::FFT`
- [ ] New vibrato model with startFrame/lengthFrames/fadeInFrames/fadeOutFrames
- [ ] Save/load roundtrip works with new format
- [ ] Undo/redo works correctly
- [ ] TreeValueMonitor shows EditedData state
- [ ] Build compiles on all platforms (Windows minimum for dev)
