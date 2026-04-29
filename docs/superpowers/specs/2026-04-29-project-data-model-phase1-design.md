# Project Data Model Phase 1 Design

## Context

HachiTune already has the main pieces needed for the new project model:

- `AnalysisData` stores immutable original analysis curves.
- `EditedData` stores global editable per-frame curves.
- `Note` stores note-local caches for GUI display and editing.
- `ProjectListener`, `TreeValueMonitor`, and `ProjectTreeView` already exist.
- `StretchProcessor` exists as a pure algorithm class, while
  `WarpMarkerProcessor` is declared separately but implemented inside
  `Project.cpp`.

The remaining problem is that `AudioData` still acts as both runtime audio
cache and editable pitch/curve data source. Many UI, undo, pitch, and synthesis
paths still read or write `audioData.f0`, `audioData.basePitch`,
`audioData.deltaPitch`, masks, and HNSep curves directly. This makes project
state hard to reason about, hard to serialize cleanly, and hard to test in
isolation.

CLAUDE.md is not a source of truth for this work. It still contains stale
entries such as `ENABLE_NOTE_STRETCH`. The current repository state and
AGENTS.md are the source of truth.

## Goal

Phase 1 makes `AnalysisData` and `EditedData` the authoritative project data
layers while keeping `AudioData` as a temporary runtime compatibility cache.
The phase also updates the project serializer to the new JSON shape, improves
the debug monitor for validation, and adds a minimal core test target for the
new boundaries.

## Non-Goals

This phase does not rewrite the whole synthesis engine.

This phase does not replace `TensionProcessor` with JUCE DSP. That is a later
phase because it changes audio behavior and should be tested independently.

This phase does not remove every legacy read of `AudioData` in UI and undo code.
Instead, it introduces a clear bridge so behavior stays stable while later
phases migrate callers to `EditedData`.

This phase does not move all `WarpMarkerProcessor` implementation out of
`Project.cpp`. The phase can add tests for `StretchProcessor`, but full stretch
pipeline cleanup belongs to the next phase.

## Data Ownership

### Project

`Project` owns the persistent user-facing state and runtime caches:

- Root properties: name, audio path, audio hash, sample rate, global pitch
  offset, formant shift, volume, scale settings, timeline settings, loop
  settings.
- `AnalysisData`: immutable original analysis data.
- `EditedData`: global editable data and synthesis input.
- `notes`: note timing and per-note edit parameters.
- `warpMarkers`: source-to-output timeline mapping.
- `AudioData`: runtime audio/model/cache data only.

### AnalysisData

`AnalysisData` is the reset baseline. It is written after analysis or legacy
load migration and must not be mutated by pitch editing, HNSep editing, or
stretch editing.

It stores:

- `originalF0`
- `originalPitch`
- `originalDeltaPitch`
- `originalVoicedMask`
- `originalVADMask`
- original source note segments

`AnalysisData` is serialized under `analysisData`.

### EditedData

`EditedData` is the authoritative editable per-frame state. Synthesis and
serialization should prefer this data over `AudioData`.

It stores:

- `basePitch`
- `deltaPitch`
- `f0`
- `voicedMask`
- `vadMask`
- `voicingCurve`
- `breathCurve`
- `tensionCurve`

`EditedData` is serialized under `editedData`.

### AudioData

`AudioData` remains in phase 1, but its role changes to runtime cache and
compatibility bridge.

It should keep:

- `waveform`
- `originalWaveform`
- `harmonicWaveform`
- `noiseWaveform`
- `melSpectrogram`
- `segmentChunkRanges`
- `segmentDebugChunks`
- `incrementalDebug`
- `sampleRate` for now, until sample rate is moved fully to `Project`
- `f0EditedMask` for now, because draw/undo still use it

The following fields are temporary mirrors and should not be treated as
authoritative after this phase:

- `f0`
- `baseF0`
- `basePitch`
- `deltaPitch`
- `voicedMask`
- `vadMask`
- `voicingCurve`
- `breathCurve`
- `tensionCurve`

New code should not introduce additional dependencies on these mirror fields.
Later phases will migrate old callers and remove the mirrors.

## Project API Changes

Add explicit synchronization helpers to `Project`:

- `syncRuntimePitchCacheFromEditedData()`: copy `EditedData` into legacy
  `AudioData` mirrors and recompute `baseF0`. This keeps existing UI/undo paths
  alive while `EditedData` becomes authoritative.
- `syncEditedDataFromRuntimePitchCache()`: temporary adapter for existing edit
  paths that still write `AudioData`. It should be called only at known bridge
  points and marked as transitional.
- `refreshNoteCaches()`: fill note-local cache data from `EditedData` and
  `AnalysisData`.
- `refreshNoteCachesForRange(startFrame, endFrame)`: update only overlapping
  note caches from `EditedData`.
- `validateFrameData()`: return a result object describing per-frame array
  length consistency across `AnalysisData`, `EditedData`, and runtime mirrors.

The listener system remains `ProjectListener` in phase 1. Project setters and
sync helpers notify:

- `EditedDataChanged` for global per-frame edit changes.
- `AudioDataChanged` for analysis/load/runtime cache replacement.
- `NotePitchChanged`, `NoteCurveChanged`, `NotePropertyChanged`,
  `NoteListChanged`, and `WarpChanged` for existing user edits.

## Save Format

The serializer writes the new JSON shape with `formatVersion: 1`.

Root-level persisted fields:

- `formatVersion`
- `name`
- `globalPitchOffset`
- `formantShift`
- `volume`
- `scaleMode`
- `scaleRootNote`
- `pitchReferenceHz`
- `showScaleColors`
- `snapToSemitones`
- `doubleClickSnapMode`
- `timelineDisplayMode`
- `timelineBeatNumerator`
- `timelineBeatDenominator`
- `timelineTempoBpm`
- `timelineGridDivision`
- `timelineSnapCycle`
- `loop`
- `audioPath`
- `audioSha256`
- `sampleRate`
- `notes`
- `warpMarkers`
- `analysisData`
- `editedData`

Per-note persisted fields:

- `startFrame`
- `endFrame`
- `midiNote`
- `pitchOffset`
- `volumeDb`
- `rest`
- `vibrato`
- `tiltLeft`
- `tiltRight`
- `varianceScale`
- `smoothLeftFrames`
- `smoothRightFrames`

Per-note optional metadata may remain if already supported:

- `lyric`
- `phoneme`

The serializer must not write:

- `srcStartFrame`
- `srcEndFrame`
- note-local `basePitch`
- note-local `deltaPitch`
- note-local `originalPitch`
- note-local `originalDeltaPitch`
- note-local `voicingCurve`
- note-local `breathCurve`
- note-local `tensionCurve`
- note-local waveform or mel clips
- `deltaScale`
- `deltaOffset`
- `highPassFilterStrength` when zero
- `lowPassFilterStrength` when zero
- any `AudioData` mirror pitch/curve/mask fields outside `editedData`

Non-zero high-pass and low-pass strengths can remain supported for backward
compatibility if the current UI still exposes them, but defaults must not be
written.

## Load Format

The loader first reads the new `analysisData` and `editedData` objects.

After loading:

1. `AnalysisData` is populated from `analysisData`.
2. `EditedData` is populated from `editedData`.
3. Notes are loaded without source-frame persistence.
4. Source note segments are restored from `analysisData.noteSegments` when
   available.
5. `Project::syncRuntimePitchCacheFromEditedData()` updates temporary
   `AudioData` mirrors.
6. `Project::refreshNoteCaches()` fills note-local display/edit caches.
7. `Project::validateFrameData()` is used by tests and the monitor to expose
   mismatches.

Legacy `pitchData` remains read-only compatibility. Legacy load should populate
both `AnalysisData` and `EditedData` with the best available data, then use the
same sync and cache refresh path as new-format load.

## Editing Rules

Pitch edits modify `EditedData` as the authoritative state.

The intended end state is:

- note movement modifies `editedData.basePitch` and recomposes `editedData.f0`.
- pitch tools modify `editedData.deltaPitch` and recomposes `editedData.f0`.
- draw edits bake non-destructive note edits first, then write destructive
  pitch edits into `editedData.deltaPitch`, `editedData.f0`, and masks.
- reset reads from `AnalysisData` and writes into `EditedData`.
- note-local pitch and curve vectors are caches for GUI display/editing.

During phase 1, existing edit paths may still write `AudioData` mirrors. Those
paths must cross through explicit sync helpers so the ownership violation is
visible and easy to remove later.

HNSep curve edits modify note-local curves for GUI interaction, then write the
affected region into `EditedData.voicingCurve`, `EditedData.breathCurve`, and
`EditedData.tensionCurve`. Mel regeneration is not redesigned in this phase.

## Synthesis Contract

The desired synthesis contract is that vocoder input comes from:

- f0 from `EditedData.f0`
- mel from runtime mel cache after HNSep and stretch processing

Phase 1 moves `Project::getAdjustedF0()` and
`Project::getAdjustedF0ForRange()` toward `EditedData` as the primary source.
Existing `AudioData` fallback can remain during migration.

Incremental synthesis range computation should prefer `EditedData.vadMask` and
`EditedData.voicedMask`. Existing fallbacks to `AudioData` can remain for
legacy intermediate states.

## Stretch Contract

`StretchProcessor::stretchEditedData()` is treated as a pure algorithm:

- `basePitch`, `voicedMask`, and `vadMask` use nearest-neighbor interpolation.
- `deltaPitch`, `voicingCurve`, `breathCurve`, and `tensionCurve` use linear
  interpolation.
- `f0` is recomputed from `basePitch + deltaPitch`.

Phase 1 adds tests for this behavior but does not fully rewrite
`WarpMarkerProcessor::recomputeFromMarkers()`. The later stretch phase will
make `WarpMarkerProcessor` depend on `StretchProcessor` directly, move its
implementation out of `Project.cpp`, and fix mel stretch behavior.

## TreeValueMonitor

`TreeValueMonitor` remains a standalone debug window and listens through
`ProjectListener`.

It should display:

- root Project properties
- `AnalysisData` array lengths and empty state
- `EditedData` array lengths and consistency status
- `AudioData` runtime cache lengths
- notes with timing, source timing, pitch, cache sizes, dirty flags, and
  selected state
- warp markers
- validation warnings from `validateFrameData()`

It refreshes on:

- `AudioDataChanged`
- `EditedDataChanged`
- `NoteListChanged`
- `NotePitchChanged`
- `NoteCurveChanged`
- `NotePropertyChanged`
- `NoteSelectionChanged`
- `WarpChanged`
- `GlobalParamChanged`
- `SettingsChanged`
- `SynthesisComplete`

## Tests

Add a small CMake test target for core behavior. It should avoid loading ONNX
models and avoid GUI windows.

Initial tests:

1. Serializer writes the new schema and excludes removed note-local fields.
2. Serializer loads new schema, fills `EditedData`, syncs runtime mirrors, and
   refreshes note caches.
3. Legacy `pitchData` load populates `AnalysisData` and `EditedData`.
4. `StretchProcessor::stretchEditedData()` applies nearest-neighbor and linear
   interpolation rules and recomputes `f0`.
5. `Project::refreshNoteCaches()` copies slices from `EditedData` and
   `AnalysisData` without mutating `AnalysisData`.
6. `Project::validateFrameData()` reports mismatched frame array lengths.

## Implementation Phases After This Spec

### Phase 1A: Persistence and Invariants

Implement serializer cleanup, sync helpers, validation, and tests.

### Phase 1B: Debug Monitor

Expose validation and richer data-layer information in `TreeValueMonitor`.

### Phase 1C: First Caller Migration

Move low-risk `Project` and synthesis read paths to prefer `EditedData` while
keeping `AudioData` compatibility mirrors.

## Risks

The largest risk is accidentally leaving `EditedData` and `AudioData` mirrors
out of sync. Explicit sync helpers and tests reduce this risk.

The second risk is changing save/load behavior for existing projects. Legacy
load compatibility remains, and tests cover both new schema and legacy
`pitchData`.

The third risk is over-expanding scope into stretch and tension behavior.
Those are intentionally deferred to later phases with their own tests.

## Success Criteria

Phase 1 is complete when:

- New saves use the approved schema.
- Removed note-local and dead properties are absent from saves.
- New-format and legacy-format loads populate `AnalysisData` and `EditedData`.
- `EditedData` is the authoritative source for project serialization and
  selected Project read paths.
- Note caches can be refreshed from global data.
- `TreeValueMonitor` shows frame-data consistency and updates via listeners.
- The new core tests pass.
