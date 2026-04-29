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
layers and removes editable pitch, curve, and mask storage from `AudioData`.
The phase also migrates UI, undo, pitch processing, and synthesis call sites
that currently read or write those `AudioData` fields. It updates the project
serializer to the new JSON shape, improves the debug monitor for validation,
and adds a minimal core test target for the new boundaries.

## Non-Goals

This phase does not rewrite the whole synthesis engine.

This phase does not replace `TensionProcessor` with JUCE DSP. That is a later
phase because it changes audio behavior and should be tested independently.

This phase may temporarily break buildability while the migration is in
progress, but it must not finish with any production reads or writes of
editable pitch, curve, or mask fields on `AudioData`.

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

`EditedData` is the authoritative editable per-frame state. Synthesis,
serialization, UI edit paths, and undo paths must use this data instead of
`AudioData`.

It stores:

- `basePitch`
- `deltaPitch`
- `f0`
- `voicedMask`
- `vadMask`
- `voicingCurve`
- `breathCurve`
- `tensionCurve`

It may also hold transient edit metadata such as `f0EditedMask` if draw-mode
rebuild logic still needs it. Transient edit metadata is not serialized unless
it becomes part of the approved save schema.

`EditedData` is serialized under `editedData`.

### AudioData

`AudioData` remains in phase 1, but only as runtime cache. It is not a
compatibility bridge for editable pitch, curve, or mask state.

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

The following fields must be removed from `AudioData` during this phase:

- `f0`
- `baseF0`
- `basePitch`
- `deltaPitch`
- `voicedMask`
- `vadMask`
- `voicingCurve`
- `breathCurve`
- `tensionCurve`
- `f0EditedMask`

Their authoritative homes are `AnalysisData` and `EditedData`. `baseF0` is not
stored; it is computed from `EditedData.basePitch` when needed.

## Project API Changes

Add explicit data-layer helpers to `Project`:

- `getBaseF0ForFrame(frame)` or an equivalent helper: compute base frequency
  from `EditedData.basePitch` without storing a dense `baseF0` cache.
- `getFrameCount()`: return the authoritative edited frame count from
  `EditedData`, falling back only to runtime mel length before analysis data has
  been initialized.
- `refreshNoteCaches()`: fill note-local cache data from `EditedData` and
  `AnalysisData`.
- `refreshNoteCachesForRange(startFrame, endFrame)`: update only overlapping
  note caches from `EditedData`.
- `validateFrameData()`: return a result object describing per-frame array
  length consistency across `AnalysisData`, `EditedData`, runtime mel, and note
  caches.

The listener system remains `ProjectListener` in phase 1. Project setters and
data-layer helpers notify:

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
- any removed `AudioData` pitch/curve/mask fields outside `editedData`

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
5. `Project::refreshNoteCaches()` fills note-local display/edit caches.
6. `Project::validateFrameData()` is used by tests and the monitor to expose
   mismatches.

Legacy `pitchData` remains read-only compatibility. Legacy load should populate
both `AnalysisData` and `EditedData` with the best available data, then use the
same cache refresh path as new-format load.

## Editing Rules

Pitch edits modify `EditedData` as the authoritative state.

The intended end state is:

- note movement modifies `editedData.basePitch` and recomposes `editedData.f0`.
- pitch tools modify `editedData.deltaPitch` and recomposes `editedData.f0`.
- draw edits bake non-destructive note edits first, then write destructive
  pitch edits into `editedData.deltaPitch`, `editedData.f0`, and masks.
- reset reads from `AnalysisData` and writes into `EditedData`.
- note-local pitch and curve vectors are caches for GUI display/editing.

During phase 1, existing edit paths that write `AudioData.f0`,
`AudioData.basePitch`, `AudioData.deltaPitch`, `AudioData.f0EditedMask`, masks,
or HNSep curves must be migrated to `EditedData`. Snapshot and undo helpers
must capture and restore `EditedData` ranges directly.

HNSep curve edits modify note-local curves for GUI interaction, then write the
affected region into `EditedData.voicingCurve`, `EditedData.breathCurve`, and
`EditedData.tensionCurve`. Mel regeneration is not redesigned in this phase.

## Synthesis Contract

The desired synthesis contract is that vocoder input comes from:

- f0 from `EditedData.f0`
- mel from runtime mel cache after HNSep and stretch processing

Phase 1 moves `Project::getAdjustedF0()` and
`Project::getAdjustedF0ForRange()` to `EditedData` as the source.

Incremental synthesis range computation should prefer `EditedData.vadMask` and
`EditedData.voicedMask`. It should not depend on `AudioData` masks after this
phase.

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
2. Serializer loads new schema, fills `EditedData`, and refreshes note caches.
3. Legacy `pitchData` load populates `AnalysisData` and `EditedData`.
4. `StretchProcessor::stretchEditedData()` applies nearest-neighbor and linear
   interpolation rules and recomputes `f0`.
5. `Project::refreshNoteCaches()` copies slices from `EditedData` and
   `AnalysisData` without mutating `AnalysisData`.
6. `Project::validateFrameData()` reports mismatched frame array lengths.
7. UI and undo-facing helpers mutate `EditedData` ranges, not removed
   `AudioData` fields.

## Implementation Phases After This Spec

### Phase 1A: Data Removal and Invariants

Remove editable pitch, curve, and mask fields from `AudioData`. Implement
Project helpers, validation, and tests around `AnalysisData` and `EditedData`.

### Phase 1B: Caller Migration

Migrate Project, UI, Undo, pitch processor, HNSep curve processor, and
incremental synthesis reads/writes from removed `AudioData` fields to
`EditedData`.

### Phase 1C: Debug Monitor

Expose validation and richer data-layer information in `TreeValueMonitor`.

## Risks

The largest risk is missing a caller that still expects removed `AudioData`
fields. Full-project build failures are useful during this phase because they
identify every call site that must migrate to `EditedData`.

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
- `AudioData` no longer contains editable pitch, curve, or mask fields.
- UI, undo, pitch processing, HNSep curve processing, and synthesis read or
  write editable per-frame data through `EditedData`.
- `EditedData` is the authoritative source for project serialization and
  Project read paths.
- Note caches can be refreshed from global data.
- `TreeValueMonitor` shows frame-data consistency and updates via listeners.
- The new core tests pass.
