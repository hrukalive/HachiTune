For very large projects, break the scope into smaller plans to avoid hitting output token maximum issues.

# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

> Full agent guidelines (naming conventions, patterns, platform notes) are in [AGENTS.md](AGENTS.md).

## What HachiTune Is

A JUCE-based C++17 vocal pitch editor (VST3/AU/AAX plugin + standalone app). It uses neural models (ONNX Runtime) for pitch detection (RMVPE/FCPE), note segmentation (GAME), and audio resynthesis (pc_nsf_hifigan vocoder). ARA integration enables direct waveform access from DAW hosts.

## Build Commands

Requires CMake 3.22+. No test infrastructure exists.

```bash
# Initial setup
git submodule update --init --recursive

# Windows – DirectML (recommended)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON -DUSE_ASIO=ON
cmake --build build --config Release --parallel

# Windows – CUDA (mutually exclusive with DirectML)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_CUDA=ON -DUSE_ASIO=ON
cmake --build build --config Release --parallel

# macOS
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --config Release --parallel

# Linux
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

Outputs:
- Standalone: `build/HachiTune_artefacts/Release/HachiTune[.exe|.app]`
- VST3: `build/HachiTunePlugin_artefacts/Release/VST3/HachiTune.vst3`

Models in `Resources/models/` are auto-downloaded (GAME) or must be placed manually (`rmvpe.onnx`, `fcpe.onnx`, `pc_nsf_hifigan.onnx`, `cent_table.bin`, `mel_filterbank.bin`).

### CMake options

| Flag | Effect |
|------|--------|
| `-DUSE_CUDA=OFF` | CUDA execution provider (Windows only, mutually exclusive with DirectML) |
| `-DUSE_DIRECTML=ON` | DirectML execution provider (Windows only) |
| `-DUSE_ASIO=OFF` | ASIO audio I/O (Windows) |
| `-DUSE_BUNDLED_CUDA_RUNTIME=OFF` | Bundle CUDA redistributables |
| `-DUSE_BUNDLED_DIRECTML_RUNTIME=OFF` | Bundle DirectML runtime |
| `-DENABLE_NOTE_STRETCH=ON/OFF` | Experimental timing stretch feature

Build targets: `HachiTune` (standalone), `HachiTunePlugin` (VST3/AU/AAX). Both link internal libraries `hachitune_core` (Audio, Models, Undo, Utils) and `hachitune_ui` (UI components). `BinaryData` embeds SVG icons and localization JSON.

## Architecture

```
Source/
  Audio/          # AI models + audio engine
    Analysis/     # AudioAnalyzer (entry point), pitch detection, GAME segmentation
    Engine/       # PlaybackController, HostSyncService (DAW transport)
    Synthesis/    # IncrementalSynthesizer (only re-synthesizes dirty regions),
                  # StretchProcessor (warp-marker-based time stretching)
    IO/           # AudioFileManager, MidiExporter
    Vocoder.*     # pc_nsf_hifigan via ONNX Runtime
    RMVPEPitchDetector.*, FCPEPitchDetector.*
    GAMEDetector.*
  Models/         # Data layer: Project (container), Note (segment), ProjectSerializer
  UI/             # All JUCE Components
    PianoRollComponent.*   # Main editor (~127 KB – central UI file)
    MainComponent.*        # Top-level orchestrator
    PianoRoll/     # Sub-components and tool states
    Workspace/     # Layout management
  Plugin/         # PluginProcessor (juce::AudioProcessor), PluginEditor,
                  # ARADocumentController, NonAraCaptureController
  Undo/           # UndoableAction base + derived: NoteActions, F0Actions,
                  # PitchToolAction, TimingActions, PitchUndoManager
  Utils/          # Constants, Localization, Curves, Resampling, NoteEditUtils
  JuceHeader.h    # Umbrella JUCE include
  Main.cpp        # Standalone entry point
```

### MVC pattern

- **Model**: `Project` (container) owns `Note` objects (segments) and global audio data (waveform, F0, mel).
- **View**: JUCE Components — `PianoRollComponent` is the main editor canvas.
- **Controller**: `EditorController` owns the `Project` and orchestrates all processing: analysis, synthesis, playback, model loading. It communicates with views through an `IMainView` interface and `juce::MessageManager::callAsync` callbacks.

### Pitch model (Note)

Each `Note` has two pitch layers:

- **midiNote**: Base pitch in MIDI semitones (default 60 = middle C). Changed by vertical note drag. Affects all frames equally — no re-synthesis needed, just a global frequency shift.
- **deltaPitch**: Per-frame deviation in semitones from `midiNote`. Extracted from neural F0; modified by pitch tool parameters (tilt, variance, smoothing, scale, offset). Original preserved in `originalDeltaPitch`.
- **pitchOffset**: Overall offset in semitones (supports reset to 0). Changed by note drag, resets during note reset.

Actual F0 per frame = `midiToFreq(midiNote + deltaPitch[frame])`. Only `deltaPitch` changes trigger full vocoder re-synthesis.

Vibrato parameters per note:
- **mix**: 0..1, blend between original delta and vibrato signal
- **fadeInMs** / **fadeOutMs**: Fade durations for vibrato onset/offset

### Stretch / timing model

- **WarpMarkers**: First-class citizens stored on Project. Always at least two markers (head at `{0, 0}` and tail at `{lastSourceFrame, lastOutputFrame}`). Additional markers define stretch control points.
- **`StretchProcessor`** (`Source/Audio/Synthesis/StretchProcessor.*`): Pure algorithm class with static methods:
  - `stretchMel()` — interpolates global mel spectrogram using warp markers
  - `stretchEditedData()` — stretches global editedData arrays (basePitch, deltaPitch, masks, curves)
  - `remapNoteFrames()` — maps note start/end frames from source to output timeline
  - `mapFrame()` / `inverseMapFrame()` — single-frame source↔output mapping
- **Stretch flow** (coordinated by `WarpMarkerProcessor::recomputeFromMarkers()`):
  1. Map note frames from source to output via warp markers
  2. Resample per-note deltaPitch and HNSep curves to new duration
  3. `StretchProcessor::stretchMel()` interpolates global mel
  4. Rebuild global basePitch, deltaPitch, f0 from notes
  5. Rebuild global HNSep curves from notes
  6. Compose global waveform
  7. `refreshNoteCaches()` syncs all note-local caches from global data

### Thread model

| Thread | Purpose | Data access |
|--------|---------|-------------|
| Message/UI thread | JUCE Component callbacks, user input | Direct access to Project via setters |
| Audio playback thread | `AudioEngine::getNextAudioBlock()` | Lock-free via `std::atomic`; `juce::SpinLock` for waveform buffer swaps |
| Worker threads | Model loading, analysis, synthesis (separate `std::thread` per task) | Stack-local copies; results dispatched to UI via `MessageManager::callAsync` |

**Critical rules**:
- Audio callbacks must be real-time safe: no allocations, no locks (except `SpinLock`), no blocking.
- Worker threads use `std::atomic` flags (`isLoadingAudio`, `cancelLoadingFlag`, `isRenderingFlag`) for cancellation.
- Old worker threads are joined in a background "joiner thread" to avoid blocking the message thread.
- Position callbacks use `std::atomic_store` / `std::atomic_load` for lock-free handoff between threads.

### Undo system

`PitchUndoManager` maintains two `std::vector<std::unique_ptr<UndoableAction>>` stacks (undo/redo, max 100 entries). Every user edit is an `UndoableAction` subclass:

- `NoteActions` — generic setter-based (uses member function pointer `void (Note::*)(float)`)
- `F0Actions` — per-frame F0 curve edits
- `DragActions` — note move/resize
- `TimingActions` — stretch operations
- `PitchToolAction` — tilt/variance/smoothing parameter changes
- `ParameterActions` — voicing/breath/tension curve edits

Every undo/redo calls `note->markDirty()`, triggering incremental synthesis. The `onHistoryChanged` callback notifies the UI.

### Incremental synthesis

`IncrementalSynthesizer` avoids full re-synthesis by tracking dirty ranges:

1. **Dirty range computation**: Union all dirty notes' frame ranges + `f0DirtyRange` (draw mode) + `paramDirtyRange` (curve edits). Expand to VAD=0 boundaries for clean splice points.
2. **Mel update**: If curves changed, `HNSepCurveProcessor::recomputeMelForRange()` writes updated mel directly into global `audioData.melSpectrogram`.
3. **Synthesis**: Slice global `editedData.f0` and global `melSpectrogram` for the dirty range, feed to Vocoder.
4. **Blend**: 1-frame (hop_size samples) linear crossfade at segment boundaries. Since ResynthRange extends to VAD=0 (silence), crossfade occurs at near-silent regions.
5. **auditionBuffer**: `juce::AudioBuffer<float>` initialized from `originalWaveform`. AudioEngine reads it for playback. Each synthesis overwrites only the dirty segment.
6. **Cancellation**: `pendingRerun` atomic flag queues edits arriving during synthesis.

### GPU provider selection

GPU backend is chosen at **compile time**, not runtime:

1. CMake flags `-DUSE_CUDA=ON` or `-DUSE_DIRECTML=ON` (mutually exclusive)
2. `EditorController::reloadInferenceModels()` maps device string to `GPUProvider` enum
3. Each detector (RMVPE, FCPE, GAME) and the Vocoder configure their ONNX Runtime session with the corresponding ExecutionProvider
4. macOS forces CPU (no GPU provider supported)

### ARA integration

Conditional on `third_party/ARA_SDK` submodule presence. When enabled, `ARADocumentController` provides direct waveform access from DAW hosts. Non-ARA fallback: `NonAraCaptureController` auto-captures audio.

## Code Style (summary — see AGENTS.md for full details)

- 2-space indentation, opening brace on same line.
- `PascalCase` classes, `camelCase` functions/variables, `SCREAMING_SNAKE_CASE` constants.
- No `m_` or `_` member prefixes.
- `#pragma once` for include guards.
- `std::unique_ptr` / `std::shared_ptr` for ownership; `std::atomic` for lock-free state.
- `juce::Result` for fallible operations; `jassert` for debug invariants; no exceptions.
- `JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR` at end of every Component class.
