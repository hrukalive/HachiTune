# HachiTune Current Refactor Follow-Up Design

**Date:** 2026-04-30
**Status:** Regenerated from current `StretchAndDraw` HEAD
**Audit range:** `6b9b050b1147a8304cea79233766496c24d4b894..HEAD`

---

## Current Code Audit

The branch has already implemented a meaningful part of the old plan:

- `AudioData` no longer stores editable pitch, curve, or mask fields. It now
  keeps waveform, original waveform, harmonic/noise waveforms, mel cache,
  sample rate, segmentation debug, and incremental synthesis debug.
- `EditedData` is the active owner of `basePitch`, `deltaPitch`, `f0`,
  `voicedMask`, `vadMask`, `voicingCurve`, `breathCurve`, and `tensionCurve`.
- `WarpMarkerProcessor.cpp` exists and removed the large implementation from
  `Project.cpp`.
- `TensionProcessor` uses `juce::dsp::FFT`, but it still owns a hand-built Hann
  table and still computes mixed waveform plus mel in the same module.
- The serializer writes `analysisData` and `editedData`, and `FORMAT_VERSION`
  is now `2`.
- `TreeValueMonitor` exists and listens to `ProjectListener` events.

Remaining gaps are concrete:

- There is no core test target in `CMakeLists.txt`; `Source/Tests` is empty.
- `Project::validateFrameData()` and `Project::getFrameCount()` do not exist.
- New-format note serialization still saves cache-like data:
  `srcStartFrame`, `srcEndFrame`, `originalDeltaPitch`,
  `voicingCurve`, `breathCurve`, and `tensionCurve`.
- Default filter strengths are always saved, although zero/default values
  should not be written.
- The warp path does not use `StretchProcessor::stretchEditedData()`.
  It remaps notes, rebuilds pitch from note caches, and stretches
  `audioData.melSpectrogram` directly.
- Warp markers are stored as user markers only. Endpoint markers are not
  normalized into the saved/project-visible representation.
- The mel pipeline is still ambiguous. `audioData.melSpectrogram` is both
  recomputed from raw audio and later stretched or patched by HNSep edits.
  There is no explicit source-domain mel cache.
- Incremental synthesis still goes through per-note `synthWaveform` caches and
  `composeGlobalWaveform()` before copying a region into `auditionBuffer`.
- `ProjectTreeView` displays only summary counts and no validation result or
  detailed array sizes.
- In plugin mode, the editor still owns the `Project` through
  `EditorController`. Closing and reopening the GUI can discard runtime caches
  that the processor should preserve.
- Several comments still reference removed `audioData` pitch/curve fields.

---

## Updated Architecture

The current branch should continue from the code that already exists rather
than replaying the old phase-one plan.

`Project` remains a C++ domain object with a JUCE-style listener interface.
It should not be converted wholesale into a `juce::ValueTree` in this refactor.
Instead, add validation and a debug `ValueTree`/tree-view projection so the
model can be inspected without making every algorithm depend on JUCE UI state.

The core rule is unchanged:

- `AnalysisData` is the immutable analysis snapshot.
- `EditedData` is the only editable per-frame source of truth.
- `AudioData` contains runtime caches only.
- Note-local pitch and HN curves are UI/edit caches, not save-format state.
- Synthesis consumes global `EditedData.f0` and the global output mel cache.

`f0EditedMask` stays removed. The current code moved draw-mode behavior toward
bake-then-write semantics, so a separate transient mask is no longer required.
Undo actions should remove the leftover placeholder parameters instead of
reintroducing a mask.

---

## Project Data Contract

### AudioData

Keep:

- `waveform`
- `originalWaveform`
- `harmonicWaveform`
- `noiseWaveform`
- `melSpectrogram`
- `sampleRate`
- `segmentChunkRanges`
- `segmentDebugChunks`
- `incrementalDebug`

Add:

- `sourceMelSpectrogram`

`sourceMelSpectrogram` is the source-timeline mel after HNSep curve processing.
`melSpectrogram` is the output-timeline mel consumed by the vocoder.

### AnalysisData

Keep:

- `originalF0`
- `originalPitch`
- `originalDeltaPitch`
- `originalVoicedMask`
- `originalVADMask`
- `noteSegments`

After analysis or legacy load conversion, analysis arrays are not mutated by
editing tools.

### EditedData

Keep:

- `basePitch`
- `deltaPitch`
- `f0`
- `voicedMask`
- `vadMask`
- `voicingCurve`
- `breathCurve`
- `tensionCurve`

`EditedData::resize()` and loader normalization must keep all arrays aligned to
the same frame count unless a field is intentionally empty before analysis.

### Project Helpers

Add:

```cpp
struct FrameDataValidation
{
  std::vector<juce::String> messages;
  bool isValid() const { return messages.empty(); }
};

int getFrameCount() const;
float getBaseF0ForFrame(int frame) const;
FrameDataValidation validateFrameData() const;
```

`getFrameCount()` returns `editedData.getNumFrames()` when available, otherwise
the output mel frame count, otherwise the audio/mel cache frame count.

`validateFrameData()` checks:

- every non-empty `EditedData` array length equals `editedData.getNumFrames()`;
- every non-empty `AnalysisData` array length equals `analysisData.getNumFrames()`;
- `analysisData.noteSegments.size()` is compatible with non-rest note count;
- note output ranges are valid and within project frame count;
- source ranges are valid and within analysis frame count when analysis exists;
- every mel row in `audioData.melSpectrogram` has the same mel dimension;
- warp markers are sorted by source and output frame after normalization.

---

## Save Format

The current implementation uses `formatVersion = 2`; keep that version for this
new schema. Treat version `1` and legacy `pitchData` as load-only compatibility.

Top-level fields:

```json
{
  "formatVersion": 2,
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
  "loop": { "enabled": false, "start": 0.0, "end": 0.0 },
  "audioPath": "...",
  "audioSha256": "...",
  "sampleRate": 44100,
  "notes": [],
  "warpMarkers": [],
  "analysisData": {},
  "editedData": {}
}
```

New-format note fields:

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
  "smoothRightFrames": 0
}
```

Only non-default note properties are saved:

- `pitchOffset` when non-zero;
- `volumeDb` when non-zero;
- `rest` when true;
- vibrato object when any vibrato parameter is active or non-default;
- pitch-tool parameters when non-default;
- `highPassFilterStrength` and `lowPassFilterStrength` only when non-zero.

Never save in new-format notes:

- `srcStartFrame`
- `srcEndFrame`
- `originalDeltaPitch`
- `deltaPitch`
- `basePitch`
- `originalPitch`
- `voicingCurve`
- `breathCurve`
- `tensionCurve`
- `f0Values`
- `deltaScale`
- `deltaOffset`

Source ranges are restored from `analysisData.noteSegments` first. If a legacy
file lacks `noteSegments`, load code may read old `srcStartFrame` and
`srcEndFrame` from notes, then write them into `analysisData.noteSegments`.

`warpMarkers` should save normalized endpoint-inclusive markers:

```json
[
  { "sourceFrame": 0, "outputFrame": 0 },
  { "sourceFrame": 350, "outputFrame": 456 },
  { "sourceFrame": 1200, "outputFrame": 1330 }
]
```

The final marker is the source/output end frame. Load accepts old files that
stored only interior markers.

---

## Warp And Stretch

All stretch logic should flow through pure functions in `StretchProcessor` and
orchestration in `WarpMarkerProcessor`.

Add a shared endpoint-aware marker builder:

```cpp
std::vector<Project::WarpMarker> buildWarpMapWithEndpoints(
    const Project& project,
    const std::vector<Project::WarpMarker>& markers);
```

The returned map:

- includes `{0, 0}`;
- includes the final source/output frame;
- is sorted and unique by `sourceFrame`;
- has strictly increasing `outputFrame`;
- is safe to pass to `StretchProcessor`.

`WarpMarkerProcessor::recomputeFromMarkers()` should:

1. build the endpoint-aware map;
2. call `StretchProcessor::stretchEditedData()` for global pitch, masks, and
   HN curves;
3. remap note output frames from source frames;
4. rebuild note-local caches from global data;
5. rebuild output mel from source mel and the same warp map;
6. notify `WarpChanged` and `EditedDataChanged`.

It should not stretch an already output-domain `melSpectrogram`.

---

## Mel And HNSep Pipeline

The target pipeline is:

```text
audio waveform
  -> HNSepModel
  -> harmonicWaveform + noiseWaveform
  -> TensionProcessor with EditedData voicing/breath/tension curves
  -> processed harmonic/noise waveform or mixed source waveform
  -> sourceMelSpectrogram
  -> StretchProcessor::buildOutputMel()
  -> melSpectrogram
  -> Vocoder with EditedData.f0
```

Pitch edits affect only `EditedData.f0`.

HN curve edits affect only source mel and output mel.

Stretch affects both `EditedData` frame arrays and mel mapping.

`TensionProcessor` should not decide dirty ranges, write project state, or mix
into `auditionBuffer`. It receives slices and returns processed audio data.

---

## Incremental Synthesis

Dirty range rules:

- pitch move, pitch draw, and pitch tools dirty the affected note range;
- smoothing expands to immediate neighbor notes;
- HN curve edits dirty the affected note range and mark mel update required;
- stretch dirties the warped segment between adjacent markers.

Resynthesis range expands dirty range to nearest `vadMask == false` boundary.

The output path should be simplified:

1. initialize `auditionBuffer` from `originalWaveform`;
2. synthesize the resynthesis range with `EditedData.f0` and output mel;
3. blend directly into `auditionBuffer` using one hop-length crossfade at each
   edge;
4. keep `audioData.waveform` synchronized from `auditionBuffer` for existing
   playback code until playback is also cleaned up.

Per-note `synthWaveform` should not be required for incremental playback.

---

## Plugin Mode Cache

Plugin runtime ownership should move from the editor to the processor:

- `HachiTuneAudioProcessor` owns the current `Project`.
- `PluginEditor`/`MainComponent` attaches to the processor-owned project.
- Closing the editor detaches the view but keeps `Project`, HNSep buffers, STFT
  cache, mel cache, audition buffer, and `EditedData` alive.
- Reopening the editor reuses the existing project and does not analyze again.
- Explicit re-analyze clears and rebuilds analysis/runtime caches.

State serialization still stores only the portable project JSON plus APVTS
state, not HNSep waveform caches.

---

## TreeValueMonitor

`TreeValueMonitor` should show:

- project metadata and dirty ranges;
- `validateFrameData()` result and messages;
- all `AnalysisData` array sizes;
- all `EditedData` array sizes;
- `AudioData` runtime cache sizes, including source/output mel dimensions and
  HNSep/STFT presence;
- per-note output range, source range, cache sizes, dirty flags, and tool
  parameters;
- endpoint-inclusive warp markers.

The monitor already listens to `ProjectListener`; keep that mechanism and make
updates range-aware where practical.

---

## Success Criteria

1. No production code reads or writes removed pitch/curve/mask fields through
   `AudioData`.
2. No current-format note save contains local pitch or HN curve caches.
3. Default zero filter strengths are not saved.
4. New saves contain endpoint-inclusive warp markers.
5. `validateFrameData()` reports mismatched global arrays and invalid ranges.
6. `StretchProcessor` is the only implementation of frame-array stretching.
7. `audioData.sourceMelSpectrogram` is source-domain; `audioData.melSpectrogram`
   is output-domain.
8. `TensionProcessor` uses JUCE FFT and JUCE windowing and returns processed
   audio slices without writing project state.
9. Incremental synthesis reads `EditedData.f0` and output mel only, then blends
   into `auditionBuffer`.
10. Plugin editor reopen does not trigger analysis when processor-owned project
    caches are already available.
11. Core tests cover serializer schema, validation, stretch, and core HN/mel
    boundaries.
