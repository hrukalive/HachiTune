# HachiTune Project Refactor Design

**Date**: 2026-04-27
**Status**: Draft
**Approach**: Incremental refactoring (Approach A)

## Goals

1. Decouple core algorithms from GUI — clear inputs/outputs, independently testable
2. Establish global editedData as the single source of truth; Note-local data is cache only
3. Simplify synthesis pipeline — mel from tension, f0 from pitch edits, warp stretches both
4. Clean up dead code, dead properties, outdated flags
5. New Project serialization format with separated analysisData / editedData
6. Listener system for reactive UI updates + TreeValueMonitor debug window
7. Snapshot-based Undo with per-action minimal data

## Design Decisions (from brainstorming)

| Decision | Choice |
|----------|--------|
| Undo system | Per-action snapshot classes (each stores only needed data) |
| HNSep caching | In-memory only (Project object); not persisted to disk |
| Mel storage | Runtime cache only; not serialized |
| Listener granularity | Categorized event types (PitchChanged, CurveChanged, etc.) |
| Vibrato mix | New `mix` parameter (0..1); controlled via Slider in note property panel |

---

## Section 1: Data Model Refactoring (Project + Note)

### Core Principle

Global data is authoritative. Note-local data is cache.
Edits write to both local and global. Synthesis reads only global.

### AudioData — Split into Three Layers

```
AudioData (runtime container, NOT serialized)
├── waveform / originalWaveform / harmonicWaveform / noiseWaveform  (audio buffers)
├── harmonicSTFT[][]        (runtime cache, [T_stft, kFFTBin] complex, precomputed after HNSep)
├── noiseSTFT[][]           (runtime cache, [T_stft, kFFTBin] complex, precomputed after HNSep)
├── melSpectrogram[][]      (runtime cache, computed from HNSep + tension)
├── renderedWaveform        (runtime cache, final synthesized waveform)
├── auditionBuffer          (juce::AudioBuffer<float>, initialized from originalWaveform)
├── sampleRate
├── segmentChunkRanges      (GAME analysis result, runtime)
│
├── AnalysisData (immutable after analysis, never modified)
│   ├── originalF0[]
│   ├── originalPitch[]        (MIDI note numbers)
│   ├── originalDeltaPitch[]
│   ├── originalVoicedMask[]
│   └── originalVADMask[]
│
└── EditedData (global edit state, serialized to project file)
    ├── basePitch[]            (MIDI, affected by note drag up/down)
    ├── deltaPitch[]           (per-frame deviation)
    ├── f0[]                   (computed: midiToFreq(basePitch + deltaPitch))
    ├── voicedMask[]
    ├── vadMask[]
    ├── voicingCurve[]
    ├── breathCurve[]
    └── tensionCurve[]
```

### Note Structure — Slimmed Down

**Serialized fields:**
- `startFrame`, `endFrame` — output timeline position
- `midiNote` — base pitch (MIDI)
- `pitchOffset` — NEW, overall offset (supports reset)
- `volumeDb` — volume
- `rest` — silence marker
- `vibrato` — with NEW fields: `mix`, `fadeInMs`, `fadeOutMs`
- `tiltLeft`, `tiltRight`, `varianceScale`, `smoothLeftFrames`, `smoothRightFrames` — non-destructive pitch tool params
- `highPassFilterStrength`, `lowPassFilterStrength` — runtime only, NOT serialized

**Removed (dead properties):**
- `deltaScale`, `deltaOffset` — marked dead
- `srcStartFrame`, `srcEndFrame` — no longer serialized (warp markers replace this)
- `f0Values` — global analysisData has original data
- `srcClipWaveform`, `clipWaveform` — runtime slicing from global waveform
- Per-note `sourceVoicingCurve` / `sourceBreathCurve` / `sourceTensionCurve` — use global analysisData for reset

**Runtime cache (NOT serialized):**
- `deltaPitch[]` — sliced from global editedData
- `originalDeltaPitch[]` — sliced from global analysisData
- `basePitch[]` — sliced from global editedData (NEW)
- `originalPitch[]` — sliced from global analysisData
- `voicingCurve[]` / `breathCurve[]` / `tensionCurve[]` — sliced from global editedData
- `clipHarmonicWaveform` / `clipNoiseWaveform` — sliced from global HNSep waveform
- `clipMel` — sliced from global mel
- `f0[]` — sliced from global editedData.f0 (for GUI display)
- `synthWaveform`, `synthPreroll`, `synthPassId`, `synthDirty` — synthesis result cache
- `selected`, `dirty` — GUI state

### WarpMarkers — First-Class Citizens

```cpp
struct WarpMarker {
    int sourceFrame;   // position in original audio
    int outputFrame;   // position on output timeline
};
```

- Always at least two markers (head and tail)
- Head: `{0, 0}`
- Tail: `{lastSourceFrame, lastOutputFrame}`
- No stretching when `sourceFrame == outputFrame` for all markers

### Project Serialization Format

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
    "notes": [
        {
            "startFrame": 112,
            "endFrame": 159,
            "midiNote": 65.016,
            "pitchOffset": 0.0,
            "volumeDb": 0.0,
            "rest": false,
            "vibrato": {
                "enabled": false,
                "rateHz": 5.0,
                "depthSemitones": 0.0,
                "phaseRadians": 0.0,
                "mix": 0.0,
                "fadeInMs": 0.0,
                "fadeOutMs": 0.0
            },
            "tiltLeft": 0.0,
            "tiltRight": 0.0,
            "varianceScale": 0.0,
            "smoothLeftFrames": 0,
            "smoothRightFrames": 0
        }
    ],
    "warpMarkers": [
        { "sourceFrame": 0, "outputFrame": 0 },
        { "sourceFrame": 350, "outputFrame": 456 },
        { "sourceFrame": 999, "outputFrame": 999 }
    ],
    "analysisData": {
        "originalF0": "349.56 349.56 ...",
        "originalPitch": "65.01 65.01 ...",
        "originalDeltaPitch": "-0.00 -0.00 ...",
        "originalVoicedMask": "00000001111000...",
        "originalVADMask": "0000000011110000..."
    },
    "editedData": {
        "basePitch": "65.01 65.01 ...",
        "deltaPitch": "-0.00 -0.00 ...",
        "f0": "349.56 349.56 ...",
        "voicedMask": "00000001111000...",
        "vadMask": "0000000011110000...",
        "voicingCurve": "100.00 100.00 ...",
        "breathCurve": "100.00 100.00 ...",
        "tensionCurve": "0.00 0.00 ..."
    }
}
```

---

## Section 2: Project Listener System

### Event Types

```cpp
enum class ProjectChangeType {
    NoteListChanged,      // note add/remove/split
    NotePitchChanged,     // midiNote / pitchOffset / deltaPitch modification
    NoteCurveChanged,     // voicing / breath / tension curve modification
    NotePropertyChanged,  // vibrato, tool params, volumeDb, etc.
    NoteSelectionChanged, // selection state change
    WarpChanged,          // warp marker modification
    GlobalParamChanged,   // globalPitchOffset / formantShift / volume
    EditedDataChanged,    // global editedData bulk update (e.g. after stretch)
    SettingsChanged,      // scale / timeline / loop UI settings
    AudioDataChanged,     // audio load / analysis complete
    SynthesisComplete     // incremental synthesis complete, waveform updated
};
```

### Listener Interface

```cpp
class ProjectListener {
public:
    virtual ~ProjectListener() = default;
    virtual void onProjectChanged(ProjectChangeType type,
                                   int affectedNoteIndex = -1,
                                   int rangeStart = -1,
                                   int rangeEnd = -1) {}
};
```

- `affectedNoteIndex`: index for single-note operations (-1 = all)
- `rangeStart` / `rangeEnd`: frame range (for dirty range propagation)
- Project maintains `std::vector<ProjectListener*>` listener list
- Setter methods trigger corresponding events; UI components register as needed

### TreeValueMonitor Window

A standalone `juce::DocumentWindow` containing a tree view that displays:
- Project properties (name, globalPitchOffset, formantShift, ...)
- EditedData statistics (f0 length, basePitch range, curve ranges)
- Each Note's key properties (startFrame, endFrame, midiNote, pitchOffset, dirty state)
- WarpMarkers list

Implemented as a `ProjectListener`; refreshes the relevant section on any `ProjectChangeType`. Available in Debug builds or via menu toggle.

---

## Section 3: Core Processor Decoupling

### 3.1 TensionProcessor (Refactored)

**Current state:** Already exists, reasonable interface. Accepts harmonic/noise pointers + 3 curves + frame count, returns mixed waveform.

**Changes:**
- New `processSegmentFromSTFT()` — accepts precomputed STFT cache, skips forward FFT
- Original `processSegment()` retained as fallback when no STFT cache
- New output: mel segment (extracted from mixed waveform)

```cpp
struct TensionResult {
    std::vector<float> mixedWaveform;
    std::vector<std::vector<float>> mel; // [T, NUM_MELS]
};

// Process [startFrame, endFrame) range
TensionResult processSegmentFromSTFT(
    const std::vector<std::complex<float>>& harmonicSTFT, // global [T_stft * kFFTBin]
    const std::vector<std::complex<float>>& noiseSTFT,    // global
    int startFrame, int endFrame,                          // frame range
    const std::vector<float>& voicingCurve,                // global
    const std::vector<float>& breathCurve,                 // global
    const std::vector<float>& tensionCurve) const;         // global
```

The processor internally locates STFT frames and curve indices from startFrame/endFrame. Callers pass global data + range only.

**Responsibility boundary:** TensionProcessor handles "spectral adjustment -> ISTFT -> mix -> extract mel" only. Segment range determination, dirty range calculation, and final blend are NOT its responsibility.

### 3.2 StretchProcessor (New)

Pure algorithm extracted from StretchHandler. StretchHandler retains UI interaction state management only.

```cpp
class StretchProcessor {
public:
    // Stretch mel spectrogram: linear interpolation
    static std::vector<std::vector<float>> stretchMel(
        const std::vector<std::vector<float>>& mel,
        const std::vector<WarpMarker>& markers);

    // Stretch global editedData:
    //   basePitch/masks → nearest neighbor interpolation
    //   deltaPitch/curves → linear interpolation
    //   then recompute f0 = midiToFreq(basePitch + deltaPitch)
    static void stretchEditedData(
        EditedData& edited,
        const std::vector<WarpMarker>& markers,
        int newTotalFrames);

    // Remap note startFrame/endFrame based on warp markers
    static void remapNoteFrames(
        std::vector<Note>& notes,
        const std::vector<WarpMarker>& markers,
        int affectedSourceStart = 0,
        int affectedSourceEnd = INT_MAX);

    // Single frame mapping: source → output (linear interpolation)
    static float mapFrame(const std::vector<WarpMarker>& markers, float sourceFrame);
    // Inverse: output → source
    static float inverseMapFrame(const std::vector<WarpMarker>& markers, float outputFrame);
};
```

**Stretch flow (coordinated by EditorController):**
1. `StretchProcessor::stretchEditedData()` — update global f0/pitch/curves/masks
2. `StretchProcessor::stretchMel()` — update global mel cache
3. `StretchProcessor::remapNoteFrames()` — update each note's output position
4. Refill each Note's local cache from global data
5. Trigger `ProjectChangeType::WarpChanged`

### 3.3 IncrementalSynthesizer (Refactored)

**Core changes:**
- Synthesis input reads ONLY from global `editedData.f0` and global `melSpectrogram` (no longer assembled from per-Note local data)
- Blend simplified: rendered output crossfaded with `auditionBuffer` (initialized = originalWaveform) using 1-frame-width linear crossfade

**New synthesis flow:**
1. Compute DirtyRange: collect all dirty notes' frame ranges
2. Expand to ResynthRange: extend forward/backward to VAD=0 boundaries
3. Extract ResynthRange slices from global `editedData.f0` and `melSpectrogram`
4. Feed to Vocoder for synthesis
5. Write result into `auditionBuffer`, 1-frame crossfade at boundaries
6. Update affected Notes' `synthWaveform` cache (for GUI waveform display)
7. Clear dirty flags

---

## Section 4: Undo System (Per-Action Snapshot)

### Action Class Hierarchy

Each Action class stores only the data it affects:

| Action Class | Snapshot Data |
|---|---|
| `NoteDragAction` | basePitch slice + note midiNote/pitchOffset |
| `PitchToolAction` | note tool params + deltaPitch slice + f0 slice |
| `F0DrawAction` | editedData f0/deltaPitch/basePitch slice |
| `CurveEditAction` | the specific curve's slice (voicing OR breath OR tension) |
| `NoteListAction` | affected notes vector segment + corresponding global data slice |
| `WarpAction` | warpMarkers + editedData full (total frames change) + note frames |
| `NotePropertyAction` | note properties only (volumeDb, vibrato, rest, etc.) |

### Shared Helper

```cpp
namespace SnapshotHelper {
    std::vector<float> captureFloatRange(const std::vector<float>& global, int start, int end);
    std::vector<bool> captureBoolRange(const std::vector<bool>& global, int start, int end);
    void restoreFloatRange(std::vector<float>& global, int start, const std::vector<float>& snapshot);
    void restoreBoolRange(std::vector<bool>& global, int start, const std::vector<bool>& snapshot);
    void refreshNoteCache(Project& project, int startFrame, int endFrame);
}
```

### Usage Pattern

```cpp
// Before edit
auto before = captureSnapshot(project, startFrame, endFrame, affectedNoteIndices);

// ... execute edit ...

auto after = captureSnapshot(project, startFrame, endFrame, affectedNoteIndices);
undoManager.addAction(std::make_unique<SomeAction>(
    "Edit Name", &project, std::move(before), std::move(after)));
```

### Special Cases

- **Stretch:** Total frame count changes. Before/after have different endFrame. Snapshot records warpMarkers + total frame count; undo restores editedData + markers + note frames entirely.
- **Note add/remove:** NoteSnapshot list lengths differ. Undo/redo replaces affected segment of notes vector.
- **Continuous edits (draw mode):** Capture `before` on mouse-down, `after` on mouse-up. No intermediate snapshots.

### Undo/Redo Execution

Restore global data slice → refresh affected Note local caches → mark dirty → trigger the appropriate `ProjectChangeType` event (e.g., `NotePitchChanged` for pitch actions, `NoteCurveChanged` for curve actions, `WarpChanged` for warp actions).

`PitchUndoManager` stack mechanism unchanged; only internal action types unified.

---

## Section 5: Synthesis Pipeline & Dirty Range

### 5.1 Edit → Synthesis Data Flow

```
User Edit
  │
  ├─ Pitch (drag/tool/draw)
  │   └─ Modify editedData.basePitch / deltaPitch / f0
  │       └─ Only re-run Vocoder (mel unchanged)
  │
  ├─ Curves (voicing/breath/tension)
  │   └─ Modify editedData curves → TensionProcessor recomputes local mel
  │       └─ Re-run Vocoder
  │
  └─ Stretch (warp)
      └─ StretchProcessor globally recomputes editedData + mel
          └─ Re-run Vocoder (affected segments)
```

### 5.2 Dirty Range Calculation

```cpp
struct ResynthRange {
    int startFrame;       // synthesis start (expanded to VAD=0 boundary)
    int endFrame;         // synthesis end
    bool needsMelUpdate;  // true = curve/stretch change, needs mel recomputation
};

ResynthRange computeResynthRange(const Project& project);
```

**Computation logic:**
1. Collect all dirty Notes' `[startFrame, endFrame)` → union
2. Merge `f0DirtyRange` (draw mode) and `paramDirtyRange` (curve edits)
3. Take total union
4. Expand start backward (toward frame 0) until `vadMask[frame] == false`
5. Expand end forward (toward last frame) until `vadMask[frame] == false`
6. Result is the ResynthRange

`needsMelUpdate` is true when `paramDirtyRange` is non-empty (curve edits trigger tension recomputation of local mel segment).

No more 24-frame padding or 16-frame gap bridging from the old system.

### 5.3 Synthesis Execution Flow

```
computeResynthRange()
  │
  ├─ if needsMelUpdate:
  │   TensionProcessor.processSegmentFromSTFT(startFrame, endFrame, ...)
  │     → write into global melSpectrogram[startFrame..endFrame)
  │
  ├─ Extract global editedData.f0[startFrame..endFrame)
  ├─ Extract global melSpectrogram[startFrame..endFrame)
  │
  ├─ Vocoder.synthesize(f0Slice, melSlice)
  │     → synthWaveform
  │
  ├─ Write into auditionBuffer[startSample..endSample)
  │   1-frame (hop_size samples) linear crossfade at boundaries
  │
  ├─ Update affected Notes' synthWaveform cache (GUI waveform display)
  │
  └─ Clear all dirty flags
      Trigger ProjectChangeType::SynthesisComplete
```

### 5.4 auditionBuffer

- `juce::AudioBuffer<float>`, held by Project at runtime
- Initialized from `originalWaveform` after load/analysis
- AudioEngine reads `auditionBuffer` directly for playback
- Each incremental synthesis overwrites only the dirty segment, crossfade at boundaries
- Reallocated when total length changes (after stretch)

### 5.5 Crossfade Simplification

**Old:** Complex voiced/unvoiced blend mask + multi-level ramp.

**New:** 1-frame (hop_size samples) linear fade-in/fade-out at start/end of synthesis segment, blended with existing `auditionBuffer` content. Since ResynthRange extends to VAD=0 boundaries (silence), crossfade occurs at silent or near-silent regions — no audible splicing artifacts.

---

## Section 6: Pitch Editing Model (Non-Destructive / Destructive)

### 6.1 Per-Note Local Pitch Data Layers

```
Note local cache (NOT serialized, filled from global data):
├── originalPitch[]       // from analysisData.originalPitch slice
├── originalDeltaPitch[]  // from analysisData.originalDeltaPitch slice
├── basePitch[]           // from editedData.basePitch slice
├── deltaPitch[]          // from editedData.deltaPitch slice
└── f0[]                  // from editedData.f0 slice (GUI display only)
```

### 6.2 Non-Destructive Edits

**Note drag (up/down):**
- Modify `note.midiNote` → update `note.pitchOffset`
- Global `editedData.basePitch[start..end)` shifted by offset
- Global `editedData.f0` recomputed from `basePitch + deltaPitch`
- Local cache synchronized
- Does NOT affect `originalPitch` or `deltaPitch`

**Pitch tool parameters (tilt/variance/smooth/highpass/lowpass):**
- Modify note's tool parameters
- From `note.originalDeltaPitch` apply parameter transform → write to `note.deltaPitch` and global `editedData.deltaPitch[start..end)`
- Global `editedData.f0` recomputed from `basePitch + deltaPitch`
- Can reset anytime: reapply from `originalDeltaPitch` with parameters

**Vibrato:**
- Superimposed on `deltaPitch` after pitch tool parameter transform
- `mix` parameter: `finalDelta = originalDelta * (1 - mix) + vibratoSignal * mix`
- `fadeIn` / `fadeOut` in ms, converted to frames at application time
- Non-destructive; controlled via Sliders in note property panel
- Serialized in Note's vibrato object

### 6.3 Destructive Edits

**F0 Drawing:**
1. First "bake" current non-destructive modifications: `originalDeltaPitch = deltaPitch` (current tool-transformed result), tool parameters reset to defaults
2. Draw directly writes to `note.originalDeltaPitch`, `note.deltaPitch`, and global `editedData.deltaPitch[start..end)`
3. Recompute global `editedData.f0`
4. After drawing, non-destructive edits can continue (on new original base)

### 6.4 Note Reset

Restore from global `analysisData`:
```
note.originalDeltaPitch = analysisData.originalDeltaPitch[start..end)
note.originalPitch = analysisData.originalPitch[start..end)
note tool parameters reset to defaults
note.pitchOffset = 0
Recompute basePitch / deltaPitch / f0 and write back to global
```

### 6.5 Note Split

Pure GUI/control-level operation:
- Global `editedData` unaffected (mel unchanged, f0 unchanged)
- One Note becomes two; each gets updated `startFrame` / `endFrame`
- Local caches refilled from global data
- Tool parameters independently inherited or reset (UX choice)

---

## Section 7: File Structure Changes & Cleanup

### 7.1 New Files

```
Source/
  Audio/
    Synthesis/
      StretchProcessor.h/.cpp       // Pure algorithm from StretchHandler
  Models/
    AnalysisData.h                  // struct AnalysisData (separated from AudioData)
    EditedData.h                    // struct EditedData (separated from AudioData)
    ProjectListener.h               // ProjectListener interface + ProjectChangeType enum
  Undo/
    SnapshotHelper.h/.cpp           // Snapshot capture/restore utilities
  UI/
    Debug/
      TreeValueMonitor.h/.cpp       // Debug project state monitor window
```

### 7.2 Major Modifications

| File | Changes |
|------|---------|
| `Models/Project.h/.cpp` | AudioData split into AnalysisData + EditedData + runtime cache; Listener system; auditionBuffer; harmonicSTFT/noiseSTFT cache |
| `Models/Note.h/.cpp` | Remove deltaScale/deltaOffset; srcStartFrame/srcEndFrame no longer serialized; remove f0Values/clipWaveform/srcClipWaveform/sourceVoicing/sourceBreath/sourceTension (become runtime cache or removed); vibrato adds mix/fadeInMs/fadeOutMs |
| `Models/ProjectSerializer.h/.cpp` | New format: pitchData → analysisData + editedData; Note serialization slimmed; no more per-note curves/source curves/f0Values |
| `Audio/TensionProcessor.h/.cpp` | New processSegmentFromSTFT(), accepts global data + startFrame/endFrame |
| `Audio/Synthesis/IncrementalSynthesizer.h/.cpp` | Reads only global data; simplified blend to 1-frame crossfade; new ResynthRange computation |
| `Audio/EditorController.h/.cpp` | Coordinates StretchProcessor; computes STFT cache after HNSep; stretch flow rewrite |
| `UI/PianoRoll/States/StretchHandler.h/.cpp` | Algorithm logic moved to StretchProcessor; only UI interaction remains |
| `Undo/*.h/.cpp` | All Action classes converted to per-action snapshot style; new SnapshotHelper |
| `CMakeLists.txt` | Remove ENABLE_NOTE_STRETCH flag + conditional compilation; add StretchProcessor/AnalysisData/EditedData/ProjectListener/SnapshotHelper/TreeValueMonitor to build targets |

### 7.3 Deletions

| Item | Reason |
|------|--------|
| `ENABLE_NOTE_STRETCH` CMake option + `HACHITUNE_ENABLE_STRETCH` define | Stretch is no longer experimental; always enabled |
| `#if HACHITUNE_ENABLE_STRETCH` guards in StretchHandler.h/.cpp and elsewhere | Same as above |
| `Note::deltaScale` / `Note::deltaOffset` fields | Dead properties |
| `Note::sourceVoicingCurve` / `sourceBreathCurve` / `sourceTensionCurve` | Replaced by global analysisData reset |
| `ProjectSerializer` serialization of deltaScale/deltaOffset/sourceXxxCurve | No longer needed |
| `Undo/F0FrameEdit.h` | Merged into new F0DrawAction |

### 7.4 CLAUDE.md / AGENTS.md Updates

- Replace "Stretch / timing model" OUTDATED paragraphs with warp markers + StretchProcessor description
- Replace "Incremental synthesis" OUTDATED paragraphs with auditionBuffer + 1-frame crossfade description
- Remove all references to `ENABLE_NOTE_STRETCH`
- Update Pitch model description (add pitchOffset, vibrato mix)
- Update Architecture file structure diagram

---

## Implementation Phases (Incremental Approach A)

### Phase 1: Data Layer
- Rewrite Project/Note data model with AnalysisData + EditedData separation
- Implement ProjectSerializer for new format (with backward compatibility: read old format, migrate to new on save)
- Implement ProjectListener system
- Build TreeValueMonitor debug window

### Phase 2: Core Processor Decoupling
- Refactor TensionProcessor with processSegmentFromSTFT()
- Create StretchProcessor (extract from StretchHandler)
- Refactor IncrementalSynthesizer (global data only, simplified blend)
- Implement STFT caching after HNSep

### Phase 3: UI Adaptation
- PianoRoll tool states adapted to new data model
- Per-action snapshot Undo classes + SnapshotHelper
- Note property panel: vibrato mix/fadeIn/fadeOut Sliders
- Pitch editing flow (non-destructive / destructive / reset)

### Phase 4: Cleanup
- Remove dead code, dead properties, ENABLE_NOTE_STRETCH flag
- Update CLAUDE.md / AGENTS.md
- Verify all data flows end-to-end
