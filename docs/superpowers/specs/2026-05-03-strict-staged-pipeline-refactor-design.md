# Strict Staged Pipeline Refactor Design

**Date:** 2026-05-03
**Scope:** Refactor pitch, mel, stretch, and synthesis ownership so every edit
flows through explicit staged data, final synthesis reads only global edited
data, and only the incremental synthesis engine writes final playback audio.

## Context

The current codebase already has `AnalysisData`, `EditedData`, `AudioData`,
`StretchProcessor`, HNSep curve processing, and `IncrementalSynthesizer`.
However, mel and waveform ownership is still mixed:

- `AudioData::sourceMelSpectrogram` is used as source mel.
- `AudioData::melSpectrogram` is used as final output mel.
- `Project::composeGlobalWaveform()` and
  `Project::blendSynthesizedRangeIntoAuditionBuffer()` can write final audio
  outside the incremental synthesizer.
- GUI note-local pitch and HNSep curves are used both as display caches and as
  inputs to global recomputation.

This refactor makes data ownership strict and makes the processing order match
the intended pipeline.

## Goals

1. Separate source analysis data, editable pipeline state, and final playback
   audio.
2. Make note-local GUI caches interaction-only. Global `EditedData` is the
   authority after commit, undo, redo, cancel, or stretch completion.
3. Route pitch through:
   `AnalysisData.originalPitch/originalDeltaPitch -> EditedData.tunedF0 ->
   stretch -> EditedData.f0 -> synthesis`.
4. Route mel through:
   `AudioData` source H/N data plus `EditedData` base HNSep curves ->
   `EditedData.adjustedSTFT` -> `EditedData.adjustedMel` -> stretch ->
   `EditedData.mel -> synthesis`.
5. Ensure final waveform output comes only from `IncrementalSynthesizer`.

## Non-Goals

- This spec does not redesign model inference or vocoder internals.
- This spec does not change ONNX model formats.
- This spec does not require removing all note-local caches immediately, but it
  does require treating them as non-authoritative display/edit caches.
- This spec does not require all stages to run full-project recomputation when
  dirty ranges are available.

## Data Ownership

### AnalysisData

`AnalysisData` remains immutable after analysis or load migration.

It stores:

- `originalF0`
- `originalPitch`
- `originalDeltaPitch`
- `originalVoicedMask`
- `originalVADMask`
- `originalMel`
- source note segments

It does not store HNSep base curves. `baseTension`, `baseVoicing`, and
`baseBreath` are not analysis outputs; they are source-timeline editable
pipeline buffers and belong only in `EditedData`.

### AudioData

`AudioData` stores source and runtime audio assets only.

It keeps:

- source/imported waveform
- pristine original waveform if still needed as a source fallback
- source harmonic/noise waveforms or source STFT caches
- segmentation/debug data
- incremental synthesis debug info
- `finalWaveform`, the separate playback/export output buffer

It does not own mel pipeline outputs. The old mel ownership migrates as:

- `AudioData::sourceMelSpectrogram` -> `AnalysisData::originalMel`
- `AudioData::melSpectrogram` -> `EditedData::mel`

`AudioData::waveform` is source audio. It is written by import/load/setup
paths, not by stretch or synthesis output paths.

`AudioData::finalWaveform` is final playback audio. It is written only by
`IncrementalSynthesizer`.

### EditedData

`EditedData` is the authoritative editable and synthesis-input state.

It stores source/non-stretched pipeline state:

- `tunedF0`
- `baseTension`
- `baseVoicing`
- `baseBreath`
- `adjustedSTFT`
- `adjustedMel`

It stores final output-timeline synthesis inputs:

- `f0`
- `mel`

Existing editable pitch and mask state remains in `EditedData` unless replaced
by a later focused change:

- `basePitch`
- `deltaPitch`
- `voicedMask`
- `vadMask`

Existing output-timeline HNSep curves may remain during migration as
compatibility fields, but the target authority for pre-stretch HNSep editing is
`baseTension/baseVoicing/baseBreath`.

## Pitch Pipeline

1. Analysis writes immutable `AnalysisData.originalF0`,
   `AnalysisData.originalPitch`, and `AnalysisData.originalDeltaPitch`.
2. Note pitch tools and destructive pitch edits rebuild source/non-stretched
   `EditedData.tunedF0`.
3. During active GUI interaction, the UI updates only note-local display caches
   for responsiveness. Those caches are not authoritative.
4. On commit, cancel, undo, redo, or stretch completion, global `EditedData`
   is recomputed first.
5. Stretch maps `EditedData.tunedF0` into final output-timeline
   `EditedData.f0`.
6. Note-local GUI caches are reloaded from final `EditedData.f0`, so visible
   note data, global final f0, and synthesis input are synchronized.
7. `IncrementalSynthesizer` reads only final `EditedData.f0`.

## Mel And HNSep Pipeline

1. Analysis writes immutable `AnalysisData.originalMel`.
2. GUI edits for tension, voicing, and breath update note-local display caches
   for responsiveness during interaction.
3. On commit, the edit is written or inverse-mapped into source-timeline
   `EditedData.baseTension`, `EditedData.baseVoicing`, and
   `EditedData.baseBreath`.
4. Tension processing uses source harmonic/noise data plus those base curves to
   produce `EditedData.adjustedSTFT`.
5. The adjusted STFT is converted to source-timeline
   `EditedData.adjustedMel`.
6. Stretch maps `EditedData.adjustedMel` into final output-timeline
   `EditedData.mel`.
7. Note-local GUI caches are reloaded from the global data after the forward
   recompute completes.
8. `IncrementalSynthesizer` reads only final `EditedData.mel`.

## Reverse Mapping Rule

When a GUI curve edit is made on the stretched/output timeline, the UI may
update the note-local visible cache immediately. On commit, the edit is
inverse-mapped through the current warp map into the matching source-timeline
base curve in `EditedData`. After that, processing continues forward:

`base curve -> adjustedSTFT -> adjustedMel -> stretch -> final mel`.

No later stage writes backward into earlier stages except this explicit
inverse-map operation for output-timeline GUI edits.

## Stretch Contract

Stretch processing is a pure forward stage.

For pitch:

- Input: source/non-stretched `EditedData.tunedF0`
- Output: final output-timeline `EditedData.f0`

For mel:

- Input: source/non-stretched `EditedData.adjustedMel`
- Output: final output-timeline `EditedData.mel`

Stretch also updates note timing and marks final synthesis dirty ranges. It
does not write final waveform audio.

## Final Waveform Isolation

The source waveform and final playback waveform are separate buffers.

`AudioData::waveform`:

- imported/source waveform
- used for waveform display, analysis, HNSep generation, and source fallback
- not overwritten by stretch or final synthesis

`AudioData::finalWaveform`:

- final playback/export waveform
- written only by `IncrementalSynthesizer`
- preferred by playback/export when available, with fallback to source waveform

Current direct writers should be removed, moved, or demoted:

- `Project::composeGlobalWaveform()` must stop writing final audio. If needed,
  retain only source-mapping helpers that return temporary buffers.
- `Project::blendSynthesizedRangeIntoAuditionBuffer()` should move into
  `IncrementalSynthesizer` or become a private helper callable only by it.
- `Note::synthWaveform` can remain temporarily as debug/cache data, but final
  output authority is the global final waveform buffer.

The synthesis contract is:

`IncrementalSynthesizer` reads `EditedData.f0` and `EditedData.mel`, synthesizes
dirty output ranges, blends them into `AudioData::finalWaveform`, clears dirty
state, and notifies playback/UI.

## Dirty Ranges

Dirty ranges have explicit timeline meaning.

- Pitch edits dirty source `tunedF0` ranges and corresponding final `f0`
  output ranges.
- HNSep edits dirty source base curve ranges, source adjusted mel ranges, and
  final mel output ranges.
- Synthesis dirty ranges are always final/output timeline ranges.

Dirty range conversion must use the current warp map. Source dirty ranges should
not be passed directly to final synthesis without mapping.

## Serialization And Migration

Field ownership and JSON persistence are separate decisions. The in-memory
owner of mel/STFT pipeline data is `AnalysisData` or `EditedData`, never
`AudioData`, but large derived matrices are runtime caches in the initial
implementation.

New saves persist compact user-edit state:

- `editedData.tunedF0`
- `editedData.baseTension`
- `editedData.baseVoicing`
- `editedData.baseBreath`
- `editedData.f0`
- existing compact pitch, mask, note, warp, and project settings data

New saves do not persist these large derived matrices in the initial
implementation:

- `analysisData.originalMel`
- `editedData.adjustedSTFT`
- `editedData.adjustedMel`
- `editedData.mel`

Those matrices are recomputed after load from source audio, source H/N data,
base curves, and the current warp map before synthesis needs them. If profiling
later shows load-time recomputation is too expensive, matrix persistence should
be added as a separate save-format change with tests for file size and backward
compatibility.

Legacy runtime migration moves existing fields to the new owners:

- old `AudioData::sourceMelSpectrogram` into `AnalysisData::originalMel`
- old `AudioData::melSpectrogram` into `EditedData::mel`
- old output HNSep curves into source base curves using inverse mapping when a
  non-identity warp map exists, or direct copy when identity mapping is valid

After load, derived matrices are recomputed as needed and note-local caches are
rebuilt from global final data.

## Implementation Stages

### Stage 1: Data Model And Serialization

- Add `AnalysisData::originalMel`.
- Add `EditedData::tunedF0`, base HNSep curves, adjusted buffers, and `mel`.
- Add `AudioData::finalWaveform`.
- Migrate old mel field reads and writes to the new owners.
- Update serializer and legacy loader.
- Update debug monitor rows and validation messages.

### Stage 2: Pitch Pipeline

- Refactor pitch rebuild helpers so committed edits produce
  `EditedData.tunedF0`.
- Refactor stretch so final `EditedData.f0` is produced from `tunedF0`.
- Reload note-local pitch caches from final `EditedData.f0` after global
  recompute.
- Update undo/redo and direct draw paths to restore global data first, then
  refresh caches.

### Stage 3: Mel And HNSep Pipeline

- Refactor HNSep curve edits to source-timeline
  `baseVoicing/baseBreath/baseTension`.
- Recompute `adjustedSTFT` and `adjustedMel` from source H/N data plus base
  curves.
- Refactor stretch so final `EditedData.mel` is produced from `adjustedMel`.
- Add inverse mapping for output-timeline GUI curve drawing.

### Stage 4: Final Waveform Isolation

- Route playback/export to `finalWaveform` when available.
- Move final range blending under `IncrementalSynthesizer`.
- Remove direct final waveform writes from stretch/project helpers.
- Keep source waveform display on `AudioData::waveform`.

## Verification

Extend `HachiTuneCoreTests` to cover:

1. The data model owns mel in `AnalysisData` and `EditedData`, not
   `AudioData`, while JSON save omits large derived matrices.
2. Legacy runtime mel fields migrate to `AnalysisData::originalMel` and
   `EditedData::mel`.
3. Stretch maps `EditedData.tunedF0` to final `EditedData.f0`.
4. Stretch maps `EditedData.adjustedMel` to final `EditedData.mel`.
5. Active edit caches are non-authoritative and are reloaded from final global
   `f0` after commit/recompute.
6. Output-timeline HNSep curve drawing inverse-maps to source base curves.
7. Only `IncrementalSynthesizer` writes `AudioData::finalWaveform`.

Build verification:

- Build and run `HachiTuneCoreTests`.
- Build the app target when practical.

Manual verification:

- Import/load audio and confirm source waveform display remains stable.
- Edit pitch, stretch notes, and confirm GUI pitch caches reload from final
  `EditedData.f0`.
- Edit tension/voicing/breath on stretched material and confirm edits affect
  source base curves and then final `EditedData.mel`.
- Confirm playback uses final synthesized audio when available and source audio
  fallback otherwise.

## Success Criteria

The refactor is complete when:

- `AnalysisData` owns immutable original mel and pitch analysis data.
- `EditedData` owns source pipeline state and final synthesis inputs.
- `AudioData::waveform` is source audio and is not overwritten by stretch or
  synthesis output.
- `AudioData::finalWaveform` is the playback/export output buffer.
- Only `IncrementalSynthesizer` writes final waveform audio.
- Stretch writes final `EditedData.f0` and `EditedData.mel`, not final
  waveform audio.
- GUI note caches update locally during interaction but reload from global
  final data after commit/recompute.
- New and legacy project files load into the new ownership model.
- Core tests cover the new ownership and processing invariants.
