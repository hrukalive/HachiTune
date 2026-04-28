# Spec Compliance Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix bugs and bring the codebase into compliance with the 2026-04-27 refactor spec — note reset, synthesis mel pipeline, TreeValueMonitor wiring, and dead code cleanup.

**Architecture:** Four phases — bug fixes first (Phase 1), then synthesis pipeline (Phase 2), then global data slice replacement (Phase 3), then dead code deletion (Phase 4). Each phase produces a compilable, functional build.

**Tech Stack:** C++17, JUCE framework, ONNX Runtime (vocoder), CMake build system.

**No test infrastructure exists.** Verification is compile + manual test. Each task ends with a build command.

**Build command (Windows, DirectML):**
```bash
cmake --build build --config Release --parallel
```

---

## Phase 1: Critical Bug Fixes

### Task 1: Extract shared resetNoteToOriginal utility

Both `SelectHandler::resetNoteToOriginal()` and `DrawHandler::resetNoteToOriginal()` are near-identical. Extract a shared free function that both call, then fix the bug (restoring from analysisData) in one place.

**Files:**
- Create: `Source/Utils/NoteEditUtils.h`
- Create: `Source/Utils/NoteEditUtils.cpp`
- Modify: `Source/UI/PianoRoll/States/SelectHandler.cpp:1138-1168`
- Modify: `Source/UI/PianoRoll/States/DrawHandler.cpp:441-472`
- Modify: `CMakeLists.txt` (add new files to `hachitune_core` sources if not GLOB)

- [ ] **Step 1: Create `NoteEditUtils.h`**

```cpp
#pragma once

class Note;
class Project;

namespace NoteEditUtils {

// Reset a note's pitch data to original analysis values.
// Restores originalDeltaPitch and originalPitch from global analysisData,
// resets tool params, clears f0EditedMask, and rebuilds global pitch curves.
void resetNoteToOriginal(Project& project, Note& note);

}
```

- [ ] **Step 2: Create `NoteEditUtils.cpp`**

```cpp
#include "NoteEditUtils.h"
#include "../Models/Project.h"
#include "../Models/Note.h"
#include "PitchCurveProcessor.h"
#include "CurveResampler.h"

namespace NoteEditUtils {

void resetNoteToOriginal(Project& project, Note& note)
{
  const int startFrame = note.getStartFrame();
  const int endFrame = note.getEndFrame();
  const int len = endFrame - startFrame;
  if (len <= 0)
    return;

  // 1. Restore originalDeltaPitch and originalPitch from global analysisData.
  //    When the note is stretched (src range differs from output range),
  //    slice from the source range and resample to output duration.
  const auto& analysisData = project.getAnalysisData();
  const int analysisFrames = analysisData.getNumFrames();

  if (analysisFrames > 0)
  {
    const int srcStart = note.getSrcStartFrame();
    const int srcEnd = note.getSrcEndFrame();
    const int srcLen = srcEnd - srcStart;

    auto sliceAnalysis = [&](const std::vector<float>& global) {
      const int sliceLen = (srcLen > 0) ? srcLen : len;
      const int sliceStart = (srcLen > 0) ? srcStart : startFrame;
      std::vector<float> slice(static_cast<size_t>(sliceLen));
      for (int i = 0; i < sliceLen; ++i)
      {
        int gi = sliceStart + i;
        if (gi >= 0 && gi < analysisFrames)
          slice[static_cast<size_t>(i)] = global[static_cast<size_t>(gi)];
      }
      return slice;
    };

    if (!analysisData.originalDeltaPitch.empty())
    {
      auto srcSlice = sliceAnalysis(analysisData.originalDeltaPitch);
      if (srcLen > 0 && srcLen != len)
        note.setOriginalDeltaPitch(
            CurveResampler::resampleLinear(srcSlice, len));
      else
        note.setOriginalDeltaPitch(std::move(srcSlice));
    }

    if (!analysisData.originalPitch.empty())
    {
      auto srcSlice = sliceAnalysis(analysisData.originalPitch);
      if (srcLen > 0 && srcLen != len)
        note.setOriginalPitch(
            CurveResampler::resampleLinear(srcSlice, len));
      else
        note.setOriginalPitch(std::move(srcSlice));
    }
  }

  // 2. Reset tool parameters to defaults
  note.resetToolParams();

  // 3. Reset pitch offset
  note.setPitchOffset(0.0f);

  // 4. Clear working deltaPitch so rebuild picks up from restored originalDeltaPitch
  note.setDeltaPitch({});

  // 5. Clear f0EditedMask for this note's frame range
  auto& audioData = project.getAudioData();
  if (!audioData.f0EditedMask.empty())
  {
    for (int i = startFrame;
         i < endFrame &&
         i < static_cast<int>(audioData.f0EditedMask.size());
         ++i)
    {
      if (i >= 0)
        audioData.f0EditedMask[static_cast<size_t>(i)] = false;
    }
  }

  // 6. Mark dirty for resynthesis
  note.markDirty();
  note.markSynthDirty();

  // 7. Rebuild global pitch curves from notes
  PitchCurveProcessor::rebuildBaseFromNotes(project);
}

}
```

- [ ] **Step 3: Update `SelectHandler::resetNoteToOriginal()` to delegate**

Replace `SelectHandler.cpp:1138-1168` with:

```cpp
void SelectHandler::resetNoteToOriginal(Note &note)
{
  if (!owner_.project)
    return;

  NoteEditUtils::resetNoteToOriginal(*owner_.project, note);

  owner_.updatePitchToolHandlesFromSelection();

  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
  if (owner_.onPitchEditFinished)
    owner_.onPitchEditFinished();
  owner_.repaint();
}
```

Add `#include "../../Utils/NoteEditUtils.h"` at the top of `SelectHandler.cpp`.

- [ ] **Step 4: Update `DrawHandler::resetNoteToOriginal()` to delegate**

Replace `DrawHandler.cpp:441-472` with:

```cpp
void DrawHandler::resetNoteToOriginal(Note &note)
{
  if (!owner_.project)
    return;

  NoteEditUtils::resetNoteToOriginal(*owner_.project, note);

  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
  if (owner_.onPitchEditFinished)
    owner_.onPitchEditFinished();
  owner_.repaint();
}
```

Add `#include "../../Utils/NoteEditUtils.h"` at the top of `DrawHandler.cpp`.

- [ ] **Step 5: Add files to CMake if needed**

Check if `CMakeLists.txt` uses `GLOB_RECURSE` for `Source/Utils/`. If so, no change needed. If it lists files explicitly, add `Source/Utils/NoteEditUtils.h` and `Source/Utils/NoteEditUtils.cpp` to the `hachitune_core` source list.

- [ ] **Step 6: Build and verify**

```bash
cmake --build build --config Release --parallel
```

Expected: compiles with no errors.

- [ ] **Step 7: Commit**

```bash
git add Source/Utils/NoteEditUtils.h Source/Utils/NoteEditUtils.cpp \
        Source/UI/PianoRoll/States/SelectHandler.cpp \
        Source/UI/PianoRoll/States/DrawHandler.cpp
git commit -m "Fix note reset: restore from global analysisData, extract shared utility"
```

---

### Task 2: Wire TreeValueMonitor into the application

The `TreeValueMonitor` class is fully implemented in `Source/UI/Debug/TreeValueMonitor.h/.cpp` but never instantiated. Wire it into the command system and View menu.

**Files:**
- Modify: `Source/UI/Commands.h:11-44`
- Modify: `Source/UI/MainComponent.h:1-226`
- Modify: `Source/UI/MainComponent.cpp:2012-2293`
- Modify: `Source/UI/Main/MenuHandler.cpp:31-91`

- [ ] **Step 1: Add command ID to `Commands.h`**

Add to the `CommandIDs` enum, in the View Menu Commands section (after `showBasePitch`):

```cpp
        showProjectMonitor  = 0x2023,
```

- [ ] **Step 2: Add member and include to `MainComponent.h`**

Add forward declaration after line 25 (after `class PitchFilterDebugWindow;`):

```cpp
class TreeValueMonitor;
```

Add member after line 194 (after `std::unique_ptr<PitchFilterDebugWindow> pitchFilterDebugWindow;`):

```cpp
  std::unique_ptr<TreeValueMonitor> treeValueMonitor;
```

- [ ] **Step 3: Add include to `MainComponent.cpp`**

Add at the top of `MainComponent.cpp`, after the existing includes:

```cpp
#include "Debug/TreeValueMonitor.h"
```

- [ ] **Step 4: Register command in `getAllCommands()`**

In `MainComponent.cpp`, in the `commandArray` at line 2015, add after `CommandIDs::showBasePitch`:

```cpp
      CommandIDs::showProjectMonitor,
```

- [ ] **Step 5: Add command info in `getCommandInfo()`**

Add a new case after the `showBasePitch` case (after line 2126):

```cpp
  case CommandIDs::showProjectMonitor:
    result.setInfo("Project Monitor", "Show debug project state monitor", "View", 0);
    result.addDefaultKeypress('m', primaryModifier | juce::ModifierKeys::shiftModifier);
    result.setActive(getProject() != nullptr);
    break;
```

- [ ] **Step 6: Handle command in `perform()`**

Add a new case after the `showBasePitch` case (after line 2250):

```cpp
  case CommandIDs::showProjectMonitor:
  {
    auto* project = getProject();
    if (project)
    {
      if (!treeValueMonitor)
        treeValueMonitor = std::make_unique<TreeValueMonitor>(project);
      treeValueMonitor->setVisible(true);
      treeValueMonitor->toFront(true);
    }
    return true;
  }
```

- [ ] **Step 7: Add menu item in `MenuHandler.cpp`**

In the standalone View menu section (line 81-84), add the new command item after `showBasePitch`:

```cpp
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::showProjectMonitor);
```

In the plugin View menu section (line 33-36), add the same:

```cpp
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::showProjectMonitor);
```

- [ ] **Step 8: Build and verify**

```bash
cmake --build build --config Release --parallel
```

Expected: compiles with no errors.

- [ ] **Step 9: Commit**

```bash
git add Source/UI/Commands.h Source/UI/MainComponent.h Source/UI/MainComponent.cpp \
        Source/UI/Main/MenuHandler.cpp
git commit -m "Wire TreeValueMonitor into View menu with Ctrl+Shift+M shortcut"
```

---

## Phase 2: Synthesis Pipeline Fixes

### Task 3: Fix auditionBuffer sync variable name bug

One-line fix for a variable name mismatch in the audition buffer sync code.

**Files:**
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.cpp:991-1009`

- [ ] **Step 1: Verify the bug exists**

Read `IncrementalSynthesizer.cpp:998-1006`. The code defines `startSamp` at line 998 but the `audition.copyFrom` at line 1005-1006 also uses `startSamp` (both the offset parameter and the `getReadPointer` parameter). Actually looking at the code more carefully:

```cpp
const int startSamp = capturedStartFrame * hopSize;     // line 998
...
audition.copyFrom(0, startSamp,                          // line 1005
                  composed.getReadPointer(0, startSamp),  // line 1006
                  numToCopy);                              // line 1007
```

This code is actually correct — both uses reference `startSamp`. The agent's earlier report was a false positive. **Skip this task if the code compiles and runs without error.** Verify by reading the actual line and confirming both references match.

- [ ] **Step 2: Build to confirm**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 3: Commit (only if a fix was made)**

---

### Task 4: Refactor mel workflow — write HNSep results to global melSpectrogram

The core change: when curve edits (voicing/breath/tension) trigger mel recomputation, write the results directly to global `audioData.melSpectrogram` instead of letting the synthesizer assemble mel per-note at synthesis time.

This task adds a function to `HNSepCurveProcessor` that recomputes mel for a frame range and writes it to the global mel array. The synthesizer will be simplified in the next task to read only from global mel.

**Files:**
- Modify: `Source/Utils/HNSepCurveProcessor.h`
- Modify: `Source/Utils/HNSepCurveProcessor.cpp`

- [ ] **Step 1: Add `recomputeMelForRange()` declaration to `HNSepCurveProcessor.h`**

Add inside the `HNSepCurveProcessor` namespace:

```cpp
// Recompute mel spectrogram for [startFrame, endFrame) using STFT cache
// or per-note harmonic/noise clips. Writes results directly into
// audioData.melSpectrogram. Call when voicing/breath/tension curves change.
void recomputeMelForRange(Project& project, int startFrame, int endFrame);
```

- [ ] **Step 2: Implement `recomputeMelForRange()` in `HNSepCurveProcessor.cpp`**

This function processes each note overlapping the range: runs TensionProcessor on per-note harmonic/noise clips at source rate, computes mel, resamples to output duration, and writes into global `audioData.melSpectrogram`.

```cpp
void recomputeMelForRange(Project& project, int startFrame, int endFrame)
{
    auto& audioData = project.getAudioData();
    if (audioData.melSpectrogram.empty())
        return;

    const bool hasGlobalHNSep =
        audioData.harmonicWaveform.getNumSamples() > 0 &&
        audioData.noiseWaveform.getNumSamples() > 0;
    if (!hasGlobalHNSep)
        return;
    if (audioData.voicingCurve.empty() || audioData.breathCurve.empty() ||
        audioData.tensionCurve.empty())
        return;
    if (!hasActiveEdits(project, startFrame, endFrame))
        return;

    const int numMels =
        static_cast<int>(audioData.melSpectrogram.front().size());

    TensionProcessor tensionProc;
    MelSpectrogram melComputer(audioData.sampleRate);

    for (const auto& note : project.getNotes())
    {
        if (note.isRest())
            continue;

        const int noteOutStart = note.getStartFrame();
        const int noteOutEnd = note.getEndFrame();
        if (noteOutEnd <= startFrame || noteOutStart >= endFrame)
            continue;

        if (!note.hasClipHarmonicWaveform() || !note.hasClipNoiseWaveform())
            continue;

        const auto& clipH = note.getClipHarmonicWaveform();
        const auto& clipN = note.getClipNoiseWaveform();
        const int srcDurationFrames = note.getSrcDurationFrames();
        const int outDurationFrames = note.getDurationFrames();
        if (srcDurationFrames <= 0 || outDurationFrames <= 0)
            continue;
        const int srcSamples = static_cast<int>(clipH.size());
        if (srcSamples <= 0)
            continue;

        // Get source-duration curves
        std::vector<float> srcVoicing, srcBreath, srcTension;
        if (note.hasSourceVoicingCurve())
            srcVoicing = note.getSourceVoicingCurve();
        else if (note.hasVoicingCurve())
            srcVoicing = CurveResampler::resampleLinear(
                note.getVoicingCurve(), srcDurationFrames);
        else
            srcVoicing.assign(static_cast<size_t>(srcDurationFrames),
                              kDefaultVoicing);

        if (note.hasSourceBreathCurve())
            srcBreath = note.getSourceBreathCurve();
        else if (note.hasBreathCurve())
            srcBreath = CurveResampler::resampleLinear(
                note.getBreathCurve(), srcDurationFrames);
        else
            srcBreath.assign(static_cast<size_t>(srcDurationFrames),
                             kDefaultBreath);

        if (note.hasSourceTensionCurve())
            srcTension = note.getSourceTensionCurve();
        else if (note.hasTensionCurve())
            srcTension = CurveResampler::resampleLinear(
                note.getTensionCurve(), srcDurationFrames);
        else
            srcTension.assign(static_cast<size_t>(srcDurationFrames),
                              kDefaultTension);

        if (!tensionProc.hasActiveEdits(
                srcVoicing.data(), srcBreath.data(), srcTension.data(),
                srcDurationFrames))
            continue;

        srcVoicing.resize(static_cast<size_t>(srcDurationFrames),
                          kDefaultVoicing);
        srcBreath.resize(static_cast<size_t>(srcDurationFrames),
                         kDefaultBreath);
        srcTension.resize(static_cast<size_t>(srcDurationFrames),
                          kDefaultTension);

        // Process at source rate
        const int noiseSamples = static_cast<int>(clipN.size());
        auto processedSrc = tensionProc.processSegment(
            clipH.data(), clipN.data(),
            std::min(srcSamples, noiseSamples),
            srcVoicing.data(), srcBreath.data(), srcTension.data(),
            srcDurationFrames);

        // Compute mel at source rate
        auto srcMel = melComputer.compute(
            processedSrc.data(), static_cast<int>(processedSrc.size()));
        if (srcMel.empty())
            continue;

        // Resample to output duration
        std::vector<std::vector<float>> outMel;
        if (srcDurationFrames == outDurationFrames)
            outMel = std::move(srcMel);
        else
            outMel = CurveResampler::resampleLinear2D(srcMel, outDurationFrames);

        outMel.resize(static_cast<size_t>(outDurationFrames),
                      std::vector<float>(static_cast<size_t>(numMels), 0.0f));

        // Write into global melSpectrogram
        const int writeStart = std::max(startFrame, noteOutStart);
        const int writeEnd = std::min(endFrame, noteOutEnd);
        for (int f = writeStart; f < writeEnd; ++f)
        {
            const int noteLocal = f - noteOutStart;
            if (noteLocal >= 0 &&
                noteLocal < static_cast<int>(outMel.size()) &&
                f < static_cast<int>(audioData.melSpectrogram.size()))
            {
                audioData.melSpectrogram[static_cast<size_t>(f)] =
                    outMel[static_cast<size_t>(noteLocal)];
            }
        }
    }

    // Sync to editedData
    syncHNSepToEditedData(project);
}
```

- [ ] **Step 3: Add necessary includes to `HNSepCurveProcessor.cpp`**

Ensure these includes are present at the top:

```cpp
#include "../Audio/TensionProcessor.h"
#include "../Utils/MelSpectrogram.h"
#include "../Utils/CurveResampler.h"
```

(Check which are already there; add any that are missing.)

- [ ] **Step 4: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 5: Commit**

```bash
git add Source/Utils/HNSepCurveProcessor.h Source/Utils/HNSepCurveProcessor.cpp
git commit -m "Add HNSepCurveProcessor::recomputeMelForRange for global mel updates"
```

---

### Task 5: Simplify IncrementalSynthesizer — remove per-note mel override

Remove the per-note mel assembly loop from `synthesizeRegion()`. Instead, call `HNSepCurveProcessor::recomputeMelForRange()` before vocoder inference to update global mel, then slice global mel directly.

**Files:**
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.cpp:391-583`

- [ ] **Step 1: Add include for HNSepCurveProcessor**

At the top of `IncrementalSynthesizer.cpp`, add if not already present:

```cpp
#include "../../Utils/HNSepCurveProcessor.h"
```

- [ ] **Step 2: Replace the per-note mel override block**

Replace lines 391-583 (the entire HNSep section from `HNSepCurveProcessor::rebuildCurvesForRange` through the `if (melRange.empty())` fallback) with:

```cpp
  // Rebuild dense HNSep curves for the synthesis range
  HNSepCurveProcessor::rebuildCurvesForRange(*project, startFrame, endFrame);

  // If curve edits need mel update, recompute mel in global melSpectrogram
  const bool hasGlobalHNSep = audioData.harmonicWaveform.getNumSamples() > 0 &&
                              audioData.noiseWaveform.getNumSamples() > 0;
  if (hasGlobalHNSep &&
      !audioData.voicingCurve.empty() &&
      !audioData.breathCurve.empty() &&
      !audioData.tensionCurve.empty() &&
      HNSepCurveProcessor::hasActiveEdits(*project, startFrame, endFrame))
  {
    HNSepCurveProcessor::recomputeMelForRange(*project, startFrame, endFrame);
  }

  // Slice global mel for vocoder input
  std::vector<std::vector<float>> melRange;
  if (startFrame < static_cast<int>(audioData.melSpectrogram.size()) &&
      endFrame <= static_cast<int>(audioData.melSpectrogram.size()))
  {
    melRange.assign(audioData.melSpectrogram.begin() + startFrame,
                    audioData.melSpectrogram.begin() + endFrame);
  }
```

The existing code after this point (`adjustedF0Range`, vocoder inference, composition) stays unchanged.

- [ ] **Step 3: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 4: Commit**

```bash
git add Source/Audio/Synthesis/IncrementalSynthesizer.cpp
git commit -m "Simplify synthesizer: read mel from global melSpectrogram only"
```

---

### Task 6: Simplify crossfade to 1-frame linear fade

Replace the complex `generateBlendMask()` with a simple 1-frame (hop_size samples) linear fade at synthesis segment boundaries. Since ResynthRange already extends to VAD=0 (silence), the crossfade occurs at near-silent regions.

**Files:**
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.cpp:205-293`
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.h:58-62`

- [ ] **Step 1: Update the `generateBlendMask` signature comment in header**

In `IncrementalSynthesizer.h`, update the comment at line 58:

```cpp
  /// Generate per-sample blend mask: 1.0 throughout with hop_size-sample
  /// linear fade-in at start and fade-out at end. Since ResynthRange
  /// extends to VAD=0 boundaries, crossfade occurs at silence.
  std::vector<float> generateBlendMask(int startFrame, int endFrame,
                                       int hopSize,
                                       std::vector<float> *frameMaskOut = nullptr);
```

- [ ] **Step 2: Replace `generateBlendMask()` implementation**

Replace `IncrementalSynthesizer.cpp:205-293` with:

```cpp
std::vector<float>
IncrementalSynthesizer::generateBlendMask(int startFrame, int endFrame,
                                          int hopSize,
                                          std::vector<float> *frameMaskOut) {
  const int numFrames = endFrame - startFrame;
  const int numSamples = numFrames * hopSize;

  if (frameMaskOut != nullptr)
  {
    // Frame-level mask: all 1.0 (use synthesized audio everywhere in range)
    *frameMaskOut = std::vector<float>(static_cast<size_t>(numFrames), 1.0f);
  }

  // Sample-level mask: 1.0 throughout, with linear fade at boundaries
  std::vector<float> mask(static_cast<size_t>(numSamples), 1.0f);

  // Fade-in at start (hop_size samples)
  const int fadeLen = std::min(hopSize, numSamples);
  for (int s = 0; s < fadeLen; ++s)
    mask[static_cast<size_t>(s)] =
        static_cast<float>(s) / static_cast<float>(fadeLen);

  // Fade-out at end (hop_size samples)
  for (int s = 0; s < fadeLen; ++s)
    mask[static_cast<size_t>(numSamples - 1 - s)] =
        static_cast<float>(s) / static_cast<float>(fadeLen);

  return mask;
}
```

- [ ] **Step 3: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 4: Commit**

```bash
git add Source/Audio/Synthesis/IncrementalSynthesizer.cpp \
        Source/Audio/Synthesis/IncrementalSynthesizer.h
git commit -m "Simplify crossfade to 1-frame linear fade at VAD=0 boundaries"
```

---

### Task 7: Use StretchProcessor::stretchMel for stretch operations

Replace the per-note mel reassembly in `recomputeFromMarkers()` with a call to `StretchProcessor::stretchMel()` that interpolates the global mel.

**Files:**
- Modify: `Source/Models/Project.cpp:1684-1774`

- [ ] **Step 1: Read `StretchProcessor::stretchMel` signature**

Check `Source/Audio/Synthesis/StretchProcessor.h` for the exact signature. It should accept global mel + markers and return stretched mel.

- [ ] **Step 2: Modify `recomputeFromMarkers()` mel section**

Replace the mel assembly logic. Currently (lines 1684-1686, 1766-1774):

```cpp
// OLD: per-note mel assembly
std::vector<std::vector<float>> newMel(...);
// ... for each note: resampleMelHybrid → place into newMel
audioData.melSpectrogram = std::move(newMel);
```

Replace with:

```cpp
// Stretch global mel using warp markers
if (!audioData.melSpectrogram.empty())
{
    audioData.melSpectrogram =
        StretchProcessor::stretchMel(audioData.melSpectrogram,
                                     normalizedMarkers);
}
```

The `newMel` variable and the per-note mel loop (lines 1766-1770) are removed. Keep the voiced mask computation (lines 1756-1763) since that still needs per-note iteration.

The existing mel will also need to be resized to totalFrames if stretchMel doesn't handle that. Check `StretchProcessor::stretchMel` implementation — if it returns the correct size based on markers, no resize needed.

- [ ] **Step 3: Add include for StretchProcessor**

Add at the top of `Project.cpp` if not already present:

```cpp
#include "../Audio/Synthesis/StretchProcessor.h"
```

- [ ] **Step 4: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 5: Commit**

```bash
git add Source/Models/Project.cpp
git commit -m "Use StretchProcessor::stretchMel for global mel stretching"
```

---

### Task 8: Call refreshNoteCaches after recomputeFromMarkers

Ensure all note-local caches are synced after stretch operations.

**Files:**
- Modify: `Source/Models/Project.cpp:1776-1783` (end of `recomputeFromMarkers`)

- [ ] **Step 1: Add refreshNoteCaches call**

After `rebuildVadMaskFromWaveform(audioData)` (line 1779), add:

```cpp
    project.refreshNoteCaches();
```

This ensures all note-local caches (basePitch, deltaPitch, originalDeltaPitch, originalPitch, curves) are re-sliced from the freshly-rebuilt global data.

- [ ] **Step 2: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 3: Commit**

```bash
git add Source/Models/Project.cpp
git commit -m "Refresh note caches after recomputeFromMarkers"
```

---

## Phase 3: Global Data Slice Replacement

### Task 9: Add NoteSegment to AnalysisData

Store GAME segmentation results (source frame ranges) in AnalysisData as immutable analysis output.

**Files:**
- Modify: `Source/Models/AnalysisData.h`

- [ ] **Step 1: Add NoteSegment struct and vector to AnalysisData**

Add inside `AnalysisData`, after the existing fields:

```cpp
  struct NoteSegment {
    int srcStartFrame = 0;
    int srcEndFrame = 0;
  };
  std::vector<NoteSegment> noteSegments;
```

Update `clear()` to also clear noteSegments:

```cpp
  void clear()
  {
    originalF0.clear();
    originalPitch.clear();
    originalDeltaPitch.clear();
    originalVoicedMask.clear();
    originalVADMask.clear();
    noteSegments.clear();
  }
```

- [ ] **Step 2: Populate noteSegments during analysis**

In `Source/Audio/Analysis/AudioAnalyzer.cpp` and `Source/Audio/EditorController.cpp`, wherever notes are created from GAME detection results, also push a `NoteSegment{srcStart, srcEnd}` into `analysisData.noteSegments`.

Search for code that calls `Note(startFrame, endFrame, midiNote)` during analysis/segmentation, and after note creation add:

```cpp
analysisData.noteSegments.push_back({startFrame, endFrame});
```

- [ ] **Step 3: Serialize noteSegments in ProjectSerializer**

In `ProjectSerializer.cpp`, in the analysisData serialization section, serialize noteSegments as a JSON array:

```cpp
// In serialization (analysisData block)
juce::var segments;
for (const auto& seg : analysisData.noteSegments)
{
    auto obj = new juce::DynamicObject();
    obj->setProperty("srcStartFrame", seg.srcStartFrame);
    obj->setProperty("srcEndFrame", seg.srcEndFrame);
    segments.append(juce::var(obj));
}
analysisObj->setProperty("noteSegments", segments);
```

In deserialization:

```cpp
if (auto* segsArray = analysisObj["noteSegments"].getArray())
{
    for (const auto& item : *segsArray)
    {
        AnalysisData::NoteSegment seg;
        seg.srcStartFrame = item.getProperty("srcStartFrame", 0);
        seg.srcEndFrame = item.getProperty("srcEndFrame", 0);
        analysisData.noteSegments.push_back(seg);
    }
}
```

- [ ] **Step 4: Initialize note srcStartFrame/srcEndFrame from analysisData during refreshNoteCaches**

In `Project::refreshNoteCaches()`, after restoring pitch data from analysisData (line 1867-1870), add:

```cpp
    // Sync srcStartFrame/srcEndFrame from analysisData if available
    if (noteIdx < static_cast<int>(analysisData.noteSegments.size()))
    {
        note.setSrcStartFrame(analysisData.noteSegments[noteIdx].srcStartFrame);
        note.setSrcEndFrame(analysisData.noteSegments[noteIdx].srcEndFrame);
    }
```

This requires adding a `noteIdx` counter to the loop. Change the loop to use index-based iteration.

- [ ] **Step 5: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 6: Commit**

```bash
git add Source/Models/AnalysisData.h Source/Audio/Analysis/AudioAnalyzer.cpp \
        Source/Audio/EditorController.cpp Source/Models/ProjectSerializer.cpp \
        Source/Models/Project.cpp
git commit -m "Add NoteSegment to AnalysisData, populate during analysis"
```

---

### Task 10: Replace Note::f0Values with on-demand global slicing

Remove the per-note `f0Values` field. All consumers that read `note.getF0Values()` will instead slice from `analysisData.originalF0`.

**Files:**
- Modify: `Source/Models/Note.h` (remove f0Values field, getF0Values, setF0Values, getAdjustedF0)
- Modify: `Source/Models/Note.cpp` (remove getAdjustedF0 implementation)
- Modify: `Source/Audio/Analysis/AudioAnalyzer.cpp` (remove setF0Values calls)
- Modify: `Source/Audio/EditorController.cpp` (remove setF0Values calls)
- Modify: `Source/UI/PianoRoll/NoteSplitter.cpp` (slice from analysisData instead)
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.cpp` (remove getF0Values usage in mel resampling voiced mask)
- Modify: `Source/Models/ProjectSerializer.cpp` (remove f0Values serialization — keep deserialization for backward compat, discard value)

- [ ] **Step 1: Remove f0Values field from Note.h**

Remove the following from `Note.h`:
- The `f0Values` private member (line 315)
- `getF0Values()` getter (line 176)
- `setF0Values()` setter (line 177)
- `getAdjustedF0()` declaration (line 178)

- [ ] **Step 2: Remove getAdjustedF0 from Note.cpp**

Remove the `getAdjustedF0()` implementation (lines 10-30).

- [ ] **Step 3: Update AudioAnalyzer.cpp**

Remove all `note.setF0Values(...)` calls. Search for `setF0Values` in this file and remove those lines.

- [ ] **Step 4: Update EditorController.cpp**

Remove all `note.setF0Values(...)` calls. Same approach as Step 3.

- [ ] **Step 5: Update NoteSplitter.cpp**

Where it splits `f0Values` between two notes (lines 322-327), replace with slicing from `analysisData.originalF0`. The split code should access the project's analysisData and slice the appropriate range for each child note.

- [ ] **Step 6: Update IncrementalSynthesizer.cpp**

In the mel resampling section that was already removed in Task 5, this is no longer needed. But verify no remaining references to `getF0Values()` exist.

- [ ] **Step 7: Update ProjectSerializer.cpp**

In `noteToJson()`: remove serialization of f0Values (lines 362-363).

In `noteFromJson()`: keep reading f0Values for backward compatibility but discard the value (don't call setF0Values).

- [ ] **Step 8: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 9: Commit**

```bash
git add Source/Models/Note.h Source/Models/Note.cpp \
        Source/Audio/Analysis/AudioAnalyzer.cpp Source/Audio/EditorController.cpp \
        Source/UI/PianoRoll/NoteSplitter.cpp Source/Audio/Synthesis/IncrementalSynthesizer.cpp \
        Source/Models/ProjectSerializer.cpp
git commit -m "Replace Note::f0Values with on-demand slicing from analysisData"
```

---

### Task 11: Replace clipWaveform/srcClipWaveform with global buffer slicing

Replace per-note waveform copies with on-demand slicing from global audio buffers.

**Files:**
- Modify: `Source/Models/Note.h` (remove clipWaveform, srcClipWaveform fields)
- Modify: `Source/UI/PianoRollComponent.cpp` (slice from global audioData.waveform for rendering)
- Modify: `Source/Models/Project.cpp` (remove clipWaveform usage in recomputeFromMarkers)
- Modify: `Source/Models/ProjectSerializer.cpp` (these fields were never serialized — verify)

- [ ] **Step 1: Remove clipWaveform and srcClipWaveform from Note.h**

Remove:
- `clipWaveform` private member (line 316)
- `srcClipWaveform` private member (line 317)
- `getClipWaveform()`, `setClipWaveform()`, `hasClipWaveform()` (lines 184-186)
- `getSrcClipWaveform()`, `setSrcClipWaveform()`, `hasSrcClipWaveform()` (lines 189-191)

- [ ] **Step 2: Update PianoRollComponent waveform rendering**

In `PianoRollComponent.cpp` where it reads `note.getClipWaveform()` for rendering (lines 1633-1640), replace with direct slicing from global audioData:

```cpp
const auto& waveform = project->getAudioData().waveform;
const int startSample = note.getStartFrame() * HOP_SIZE;
const int endSample = std::min(note.getEndFrame() * HOP_SIZE,
                                waveform.getNumSamples());
if (startSample < endSample && waveform.getNumChannels() > 0)
{
    // Use waveform.getReadPointer(0) + startSample for rendering
    // with length (endSample - startSample)
}
```

Adapt the existing rendering code to use this slice approach instead of the per-note buffer.

- [ ] **Step 3: Update Project.cpp recomputeFromMarkers**

Remove lines 1746-1754 (clipWaveform resample from srcClipWaveform). After stretch, waveform rendering will slice from global data anyway.

- [ ] **Step 4: Search for remaining references**

```bash
grep -rn "ClipWaveform\|SrcClipWaveform\|clipWaveform\|srcClipWaveform" Source/
```

Fix any remaining references.

- [ ] **Step 5: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 6: Commit**

```bash
git add Source/Models/Note.h Source/UI/PianoRollComponent.cpp \
        Source/Models/Project.cpp
git commit -m "Replace per-note clipWaveform with global buffer slicing"
```

---

## Phase 4: Dead Code Cleanup

### Task 12: Remove deltaScale and deltaOffset

These fields are marked dead in the spec. Remove from Note, TransformParams, and ProjectSerializer.

**Files:**
- Modify: `Source/Models/Note.h` (remove fields, getters/setters, usage in hasNonDefaultToolParams and resetToolParams)
- Modify: `Source/Utils/TransformParams.h` (remove deltaScale/deltaOffset fields)
- Modify: `Source/Models/ProjectSerializer.cpp` (remove serialization; keep deserialization for compat, discard values)
- Modify: `Source/Utils/PitchCurveProcessor.cpp` (verify no usage)

- [ ] **Step 1: Remove from Note.h**

Remove:
- `deltaScale` member (line 302) and `deltaOffset` member (line 303)
- `getDeltaScale()`, `setDeltaScale()` getters/setters (lines 104-105)
- `getDeltaOffset()`, `setDeltaOffset()` getters/setters
- References in `hasNonDefaultToolParams()` (line 254-255) — remove the `deltaScale != 1.0f || deltaOffset != 0.0f` checks
- References in `resetToolParams()` (line 270-271) — remove the `deltaScale = 1.0f; deltaOffset = 0.0f;` lines

- [ ] **Step 2: Remove from TransformParams.h**

Remove:
- `deltaScale` field (line 17)
- `deltaOffset` field (line 18)
- Their capture in `fromNote()` (lines 34-35)
- Their application in `applyToNote()` (lines 50-51)
- Their comparison in `operator==()` (lines 64-65)
- Their check in `isIdentity()` (line 82)

- [ ] **Step 3: Update ProjectSerializer.cpp**

In `noteToJson()`: remove serialization of deltaScale/deltaOffset (lines 370-373).

In `noteFromJson()`: keep reading them for backward compat but discard (don't call setters).

- [ ] **Step 4: Search for remaining references**

```bash
grep -rn "deltaScale\|deltaOffset\|DeltaScale\|DeltaOffset" Source/
```

Fix any remaining references.

- [ ] **Step 5: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 6: Commit**

```bash
git add Source/Models/Note.h Source/Utils/TransformParams.h \
        Source/Models/ProjectSerializer.cpp
git commit -m "Remove dead Note::deltaScale and deltaOffset fields"
```

---

### Task 13: Remove sourceVoicingCurve, sourceBreathCurve, sourceTensionCurve

These per-note "source curves" are replaced by the pattern of fitting output curves to source duration at stretch time (already handled in recomputeFromMarkers).

**Files:**
- Modify: `Source/Models/Note.h`
- Modify: `Source/Models/Project.cpp` (recomputeFromMarkers curve handling)
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.cpp` (if any remaining refs after Task 5)
- Modify: `Source/Models/ProjectSerializer.cpp`

- [ ] **Step 1: Remove from Note.h**

Remove:
- `sourceVoicingCurve` member (line 327) and getter/setter/has (lines 145-147)
- `sourceBreathCurve` member (line 328) and getter/setter/has (lines 153-155)
- `sourceTensionCurve` member (line 329) and getter/setter/has (lines 161-163)

- [ ] **Step 2: Update recomputeFromMarkers in Project.cpp**

In lines 1722-1743, the `selectSourceCurve` pattern uses source curves when available. After removal, always resample the output curve to source duration:

Replace:
```cpp
        const auto sourceVoicing =
            selectSourceCurve(note.getVoicingCurve(),
                              note.getSourceVoicingCurve());
```
With:
```cpp
        const auto sourceVoicing = note.getVoicingCurve();
```

(Same for breath and tension.)

Remove lines 1739-1744 (the `setSourceXxxCurve` calls).

- [ ] **Step 3: Update HNSepCurveProcessor::recomputeMelForRange**

In the function added in Task 4, replace `note.hasSourceVoicingCurve()` / `getSourceVoicingCurve()` patterns with direct use of `note.getVoicingCurve()` resampled to source duration.

- [ ] **Step 4: Update ProjectSerializer.cpp**

In `noteToJson()`: remove serialization of sourceVoicingCurve, sourceBreathCurve, sourceTensionCurve (lines 386-394).

In `noteFromJson()`: keep reading for backward compat, discard values.

- [ ] **Step 5: Search for remaining references**

```bash
grep -rn "sourceVoicingCurve\|sourceBreathCurve\|sourceTensionCurve\|SourceVoicingCurve\|SourceBreathCurve\|SourceTensionCurve" Source/
```

Fix any remaining references.

- [ ] **Step 6: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 7: Commit**

```bash
git add Source/Models/Note.h Source/Models/Project.cpp \
        Source/Utils/HNSepCurveProcessor.cpp \
        Source/Audio/Synthesis/IncrementalSynthesizer.cpp \
        Source/Models/ProjectSerializer.cpp
git commit -m "Remove per-note source curves, use output curve resample at stretch"
```

---

### Task 14: Remove srcStartFrame/srcEndFrame from serialization

Keep the runtime fields (needed for stretch calculations) but stop serializing them per-note. They're now populated from `analysisData.noteSegments` (Task 9).

**Files:**
- Modify: `Source/Models/ProjectSerializer.cpp`

- [ ] **Step 1: Remove from noteToJson**

Remove lines 330-331 (serialization of srcStartFrame, srcEndFrame).

- [ ] **Step 2: Update noteFromJson for backward compat**

Keep reading srcStartFrame/srcEndFrame from old project files and setting them on the Note, so old files still load correctly. But new saves won't include them.

- [ ] **Step 3: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 4: Commit**

```bash
git add Source/Models/ProjectSerializer.cpp
git commit -m "Stop serializing per-note srcStartFrame/srcEndFrame"
```

---

### Task 15: Delete F0FrameEdit.h and final cleanup

**Files:**
- Delete: `Source/Undo/F0FrameEdit.h`
- Modify: any files that include it

- [ ] **Step 1: Search for F0FrameEdit usage**

```bash
grep -rn "F0FrameEdit" Source/
```

If nothing includes it (likely, since the new F0DrawAction should have replaced it), delete the file.

- [ ] **Step 2: Delete the file**

```bash
rm Source/Undo/F0FrameEdit.h
```

- [ ] **Step 3: If there are references, update them**

Replace any `F0FrameEdit` usage with the current undo mechanism.

- [ ] **Step 4: Build and verify**

```bash
cmake --build build --config Release --parallel
```

- [ ] **Step 5: Commit**

```bash
git add -A Source/Undo/
git commit -m "Remove unused F0FrameEdit.h"
```

---

### Task 16: Update CLAUDE.md architecture documentation

Update the OUTDATED sections in CLAUDE.md with the current architecture.

**Files:**
- Modify: `CLAUDE.md`

- [ ] **Step 1: Replace "Stretch / timing model" section**

Replace the section currently marked "OUTDATED, WAITING TO BE REFACTORED!!!" with a description of:
- WarpMarkers (first-class citizens, always at least two)
- `StretchProcessor` pure algorithm (stretchMel, stretchEditedData, remapNoteFrames)
- Stretch flow: global mel interpolation → editedData stretch → note frame remap → refreshNoteCaches

- [ ] **Step 2: Replace "Incremental synthesis" section**

Replace the OUTDATED section with:
- auditionBuffer (initialized from originalWaveform, AudioEngine reads it)
- Global mel + global f0 as synthesis input
- 1-frame crossfade at VAD=0 boundaries
- HNSepCurveProcessor::recomputeMelForRange for curve edits

- [ ] **Step 3: Update Pitch model description**

Add `pitchOffset` and vibrato `mix`/`fadeIn`/`fadeOut` to the Note section.

- [ ] **Step 4: Update file structure diagram**

Add new files: `NoteEditUtils`, `StretchProcessor`. Remove deleted files from the tree.

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md
git commit -m "Update CLAUDE.md: replace OUTDATED architecture sections"
```
