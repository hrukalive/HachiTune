# HachiTune Project Refactor — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decouple core algorithms from GUI by establishing global editedData as single source of truth, simplifying the synthesis pipeline, and cleaning up dead code.

**Architecture:** Split AudioData into AnalysisData (immutable) + EditedData (global edit state) + runtime cache. Note-local data becomes a cache of global data. Introduce ProjectListener for reactive UI. Extract StretchProcessor from StretchHandler. Simplify IncrementalSynthesizer to use global data + 1-frame crossfade.

**Tech Stack:** C++17, JUCE 8.x, ONNX Runtime, CMake 3.22+

**Spec:** `docs/superpowers/specs/2026-04-27-project-refactor-design.md`

**Build command (Windows PowerShell):**
```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON -DUSE_ASIO=ON; cmake --build build --config Release --parallel
```

**IMPORTANT:** This project has NO test infrastructure. Verification for each task is: the project compiles and links cleanly. After all tasks, do a manual run to verify the application launches.

---

## Task 1: Create AnalysisData and EditedData Structs

**Files:**
- Create: `Source/Models/AnalysisData.h`
- Create: `Source/Models/EditedData.h`

These are pure data structs with no dependencies on other project files (only STL). They will be consumed by the refactored Project/AudioData.

- [ ] **Step 1: Create `Source/Models/AnalysisData.h`**

```cpp
#pragma once

#include <vector>

/**
 * Immutable analysis results. Set once during pitch detection,
 * never modified after analysis completes. Used as the
 * "original" baseline for resets.
 */
struct AnalysisData
{
  std::vector<float> originalF0;          // [T] Hz
  std::vector<float> originalPitch;       // [T] MIDI note number
  std::vector<float> originalDeltaPitch;  // [T] semitone deviation from base
  std::vector<bool>  originalVoicedMask;  // [T] true = voiced
  std::vector<bool>  originalVADMask;     // [T] true = has audio energy

  int getNumFrames() const
  {
    return static_cast<int>(originalF0.size());
  }

  bool isEmpty() const { return originalF0.empty(); }

  void clear()
  {
    originalF0.clear();
    originalPitch.clear();
    originalDeltaPitch.clear();
    originalVoicedMask.clear();
    originalVADMask.clear();
  }
};
```

- [ ] **Step 2: Create `Source/Models/EditedData.h`**

```cpp
#pragma once

#include <vector>

/**
 * Global edit state — the single source of truth for all editable
 * per-frame data. Serialized to the project file.
 * Synthesis reads ONLY from this struct.
 */
struct EditedData
{
  std::vector<float> basePitch;       // [T] MIDI, affected by note drag
  std::vector<float> deltaPitch;      // [T] per-frame deviation (semitones)
  std::vector<float> f0;              // [T] Hz, composed: midiToFreq(basePitch + deltaPitch)
  std::vector<bool>  voicedMask;      // [T] voiced/unvoiced
  std::vector<bool>  vadMask;         // [T] energy-based VAD
  std::vector<float> voicingCurve;    // [T] 0..max (default 100)
  std::vector<float> breathCurve;     // [T] 0..max (default 100)
  std::vector<float> tensionCurve;    // [T] -100..100 (default 0)

  int getNumFrames() const
  {
    return static_cast<int>(f0.size());
  }

  bool isEmpty() const { return f0.empty(); }

  void clear()
  {
    basePitch.clear();
    deltaPitch.clear();
    f0.clear();
    voicedMask.clear();
    vadMask.clear();
    voicingCurve.clear();
    breathCurve.clear();
    tensionCurve.clear();
  }

  /** Resize all arrays to numFrames, preserving existing data. */
  void resize(int numFrames)
  {
    auto n = static_cast<size_t>(numFrames);
    basePitch.resize(n, 0.0f);
    deltaPitch.resize(n, 0.0f);
    f0.resize(n, 0.0f);
    voicedMask.resize(n, false);
    vadMask.resize(n, false);
    voicingCurve.resize(n, 100.0f);
    breathCurve.resize(n, 100.0f);
    tensionCurve.resize(n, 0.0f);
  }
};
```

- [ ] **Step 3: Commit**

```
git add Source/Models/AnalysisData.h Source/Models/EditedData.h
git commit -m "Add AnalysisData and EditedData structs"
```

---

## Task 2: Create ProjectListener System

**Files:**
- Create: `Source/Models/ProjectListener.h`

- [ ] **Step 1: Create `Source/Models/ProjectListener.h`**

```cpp
#pragma once

/**
 * Event types for Project state changes.
 */
enum class ProjectChangeType
{
  NoteListChanged,       // note add/remove/split
  NotePitchChanged,      // midiNote / pitchOffset / deltaPitch modification
  NoteCurveChanged,      // voicing / breath / tension curve modification
  NotePropertyChanged,   // vibrato, tool params, volumeDb, etc.
  NoteSelectionChanged,  // selection state change
  WarpChanged,           // warp marker modification
  GlobalParamChanged,    // globalPitchOffset / formantShift / volume
  EditedDataChanged,     // global editedData bulk update (e.g. after stretch)
  SettingsChanged,       // scale / timeline / loop UI settings
  AudioDataChanged,      // audio load / analysis complete
  SynthesisComplete      // incremental synthesis complete, waveform updated
};

/**
 * Interface for objects that want to be notified of Project changes.
 * Register with Project::addListener() / removeListener().
 */
class ProjectListener
{
public:
  virtual ~ProjectListener() = default;

  /**
   * Called when the project state changes.
   *
   * @param type               Category of change
   * @param affectedNoteIndex  Index for single-note ops (-1 = all/N/A)
   * @param rangeStart         Start frame of affected range (-1 = N/A)
   * @param rangeEnd           End frame of affected range (-1 = N/A)
   */
  virtual void onProjectChanged(ProjectChangeType type,
                                int affectedNoteIndex = -1,
                                int rangeStart = -1,
                                int rangeEnd = -1) = 0;
};
```

- [ ] **Step 2: Commit**

```
git add Source/Models/ProjectListener.h
git commit -m "Add ProjectListener interface and ProjectChangeType enum"
```

---

## Task 3: Refactor Note Model

**Files:**
- Modify: `Source/Models/Note.h`
- Modify: `Source/Models/Note.cpp`

Goal: Remove dead properties (deltaScale, deltaOffset, sourceVoicingCurve, sourceBreathCurve, sourceTensionCurve, f0Values, clipWaveform, srcClipWaveform). Add vibrato mix/fadeIn/fadeOut. Add basePitch cache + originalPitch cache. Keep srcStartFrame/srcEndFrame as runtime-only (no longer serialized — handled in Task 5).

- [ ] **Step 1: Add vibrato mix/fadeIn/fadeOut fields to Note.h**

In `Source/Models/Note.h`, after the existing vibrato getters/setters (around line 111), add:

```cpp
    float getVibratoMix() const { return vibratoMix; }
    void setVibratoMix(float mix) { vibratoMix = mix; }
    float getVibratoFadeInMs() const { return vibratoFadeInMs; }
    void setVibratoFadeInMs(float ms) { vibratoFadeInMs = ms; }
    float getVibratoFadeOutMs() const { return vibratoFadeOutMs; }
    void setVibratoFadeOutMs(float ms) { vibratoFadeOutMs = ms; }
```

In the private section (after line 292 `float vibratoPhaseRadians = 0.0f;`), add:

```cpp
    float vibratoMix = 0.0f;            // 0..1: 0=pure delta, 1=pure vibrato
    float vibratoFadeInMs = 0.0f;       // Fade-in duration in ms
    float vibratoFadeOutMs = 0.0f;      // Fade-out duration in ms
```

- [ ] **Step 2: Add basePitch and originalPitch cache fields**

In `Source/Models/Note.h`, add public getters/setters after the originalDeltaPitch section (around line 79):

```cpp
    // Base pitch cache (from global editedData, note-local)
    const std::vector<float>& getBasePitch() const { return basePitch; }
    void setBasePitch(std::vector<float> bp) { basePitch = std::move(bp); }
    bool hasBasePitch() const { return !basePitch.empty(); }

    // Original pitch cache (from global analysisData, note-local)
    const std::vector<float>& getOriginalPitch() const { return originalPitch; }
    void setOriginalPitch(std::vector<float> op) { originalPitch = std::move(op); }
    bool hasOriginalPitch() const { return !originalPitch.empty(); }
```

In the private section, add after `originalDeltaPitch` (around line 274):

```cpp
    std::vector<float> basePitch;            // Cache from editedData.basePitch
    std::vector<float> originalPitch;        // Cache from analysisData.originalPitch
```

- [ ] **Step 3: Remove dead properties**

In `Source/Models/Note.h`:

1. **Remove deltaScale/deltaOffset** — delete getters/setters (lines 93-97) and private members (lines 284-285). Also remove them from `hasNonDefaultToolParams()` (lines 238-239) and `resetToolParams()` (lines 254-255).

2. **Remove sourceVoicingCurve/sourceBreathCurve/sourceTensionCurve** — delete getters/setters (lines 129-131, 137-139, 145-147) and private members (lines 306-308).

3. **Remove f0Values** — delete getters/setters (lines 160-162) and private member (line 294).

4. **Remove clipWaveform/srcClipWaveform** — delete getters/setters (lines 168-175) and private members (lines 295-296).

- [ ] **Step 4: Update Note.cpp**

In `Source/Models/Note.cpp`, remove the `getAdjustedF0()` method that uses the now-deleted `f0Values`. Remove `computeF0FromDelta()` if it references deleted fields. Update the constructor if needed.

- [ ] **Step 5: Build and fix compilation errors**

Run:
```powershell
cmake --build build --config Release --parallel 2>&1
```

There will be compilation errors in files that reference the removed fields. **Do NOT fix them in this step** — just note them. They will be fixed in subsequent tasks as we update ProjectSerializer, PitchCurveProcessor, HNSepCurveProcessor, Undo actions, and UI components. The key references to fix are:
- `ProjectSerializer.cpp` — references to deltaScale, deltaOffset, f0Values, sourceVoicingCurve, etc.
- `PitchCurveProcessor.cpp` — may reference f0Values
- `HNSepCurveProcessor.cpp` — references sourceVoicingCurve, etc.
- `Undo/*.h` — references to removed fields
- `PianoRollComponent.cpp/h` — references to clipWaveform, srcClipWaveform
- `ToolbarComponent.cpp/h` — references to removed fields
- Various UI files

For now, **stub out** the removed fields with temporary `[[deprecated]]` wrappers to keep compilation going while other tasks catch up. This is a pragmatic approach for incremental refactoring:

```cpp
    // DEPRECATED: Remove after all callers are updated
    [[deprecated("Use analysisData for reset instead")]]
    const std::vector<float>& getSourceVoicingCurve() const { return voicingCurve; }
    // ... similar for other removed fields
```

Actually, **the cleaner approach**: keep the removed fields but mark them with a comment `// TODO(refactor): remove after all callers updated`. Remove them fully in Task 13 (Cleanup). This prevents a cascade of compilation errors blocking progress.

- [ ] **Step 6: Commit**

```
git add Source/Models/Note.h Source/Models/Note.cpp
git commit -m "Refactor Note: add vibrato mix/fade, basePitch/originalPitch cache, mark dead fields"
```

---

## Task 4: Integrate AnalysisData and EditedData into Project

**Files:**
- Modify: `Source/Models/Project.h`
- Modify: `Source/Models/Project.cpp`

Goal: Add AnalysisData, EditedData, auditionBuffer, STFT cache, and Listener system to Project. Keep the existing AudioData struct for now (it still holds waveforms, mel, etc.) but add the new structs alongside. Gradually migrate data out of AudioData's flat arrays into the new structs.

- [ ] **Step 1: Add includes and new members to Project.h**

At the top of `Source/Models/Project.h`, add includes:

```cpp
#include "AnalysisData.h"
#include "EditedData.h"
#include "ProjectListener.h"
#include <algorithm>
#include <complex>
```

In the `Project` class public section, add:

```cpp
    // --- Analysis & Edited Data (new architecture) ---
    AnalysisData& getAnalysisData() { return analysisData; }
    const AnalysisData& getAnalysisData() const { return analysisData; }
    EditedData& getEditedData() { return editedData; }
    const EditedData& getEditedData() const { return editedData; }

    // --- Audition buffer ---
    juce::AudioBuffer<float>& getAuditionBuffer() { return auditionBuffer; }
    const juce::AudioBuffer<float>& getAuditionBuffer() const { return auditionBuffer; }
    void initAuditionBufferFromOriginal();

    // --- STFT cache ---
    std::vector<std::complex<float>>& getHarmonicSTFT() { return harmonicSTFT; }
    const std::vector<std::complex<float>>& getHarmonicSTFT() const { return harmonicSTFT; }
    std::vector<std::complex<float>>& getNoiseSTFT() { return noiseSTFT; }
    const std::vector<std::complex<float>>& getNoiseSTFT() const { return noiseSTFT; }

    // --- Listener system ---
    void addListener(ProjectListener* listener);
    void removeListener(ProjectListener* listener);
    void notifyListeners(ProjectChangeType type,
                         int affectedNoteIndex = -1,
                         int rangeStart = -1,
                         int rangeEnd = -1);

    // --- Note cache refresh ---
    void refreshNoteCaches();
    void refreshNoteCachesForRange(int startFrame, int endFrame);
```

In the private section, add:

```cpp
    AnalysisData analysisData;
    EditedData editedData;
    juce::AudioBuffer<float> auditionBuffer;
    std::vector<std::complex<float>> harmonicSTFT;
    std::vector<std::complex<float>> noiseSTFT;
    std::vector<ProjectListener*> listeners;
```

- [ ] **Step 2: Implement listener methods and cache refresh in Project.cpp**

Add to `Source/Models/Project.cpp`:

```cpp
void Project::addListener(ProjectListener* listener)
{
  if (listener && std::find(listeners.begin(), listeners.end(), listener) == listeners.end())
    listeners.push_back(listener);
}

void Project::removeListener(ProjectListener* listener)
{
  listeners.erase(std::remove(listeners.begin(), listeners.end(), listener), listeners.end());
}

void Project::notifyListeners(ProjectChangeType type,
                              int affectedNoteIndex,
                              int rangeStart,
                              int rangeEnd)
{
  for (auto* l : listeners)
    l->onProjectChanged(type, affectedNoteIndex, rangeStart, rangeEnd);
}

void Project::initAuditionBufferFromOriginal()
{
  const auto& orig = audioData.originalWaveform;
  if (orig.getNumSamples() > 0)
  {
    auditionBuffer.makeCopyOf(orig);
  }
}

void Project::refreshNoteCaches()
{
  const int totalFrames = editedData.getNumFrames();
  if (totalFrames == 0)
    return;

  for (auto& note : notes)
  {
    if (note.isRest())
      continue;

    const int start = note.getStartFrame();
    const int end = note.getEndFrame();
    const int len = end - start;
    if (len <= 0)
      continue;

    // Slice from editedData
    auto sliceFloat = [&](const std::vector<float>& global) {
      std::vector<float> slice(static_cast<size_t>(len));
      for (int i = 0; i < len; ++i)
      {
        int gi = start + i;
        if (gi >= 0 && gi < totalFrames)
          slice[static_cast<size_t>(i)] = global[static_cast<size_t>(gi)];
      }
      return slice;
    };

    note.setBasePitch(sliceFloat(editedData.basePitch));
    note.setDeltaPitch(sliceFloat(editedData.deltaPitch));

    // Voicing/breath/tension
    if (!editedData.voicingCurve.empty())
      note.setVoicingCurve(sliceFloat(editedData.voicingCurve));
    if (!editedData.breathCurve.empty())
      note.setBreathCurve(sliceFloat(editedData.breathCurve));
    if (!editedData.tensionCurve.empty())
      note.setTensionCurve(sliceFloat(editedData.tensionCurve));

    // From analysisData
    const int analysisFrames = analysisData.getNumFrames();
    if (analysisFrames > 0)
    {
      auto sliceAnalysis = [&](const std::vector<float>& global) {
        std::vector<float> slice(static_cast<size_t>(len));
        for (int i = 0; i < len; ++i)
        {
          int gi = start + i;
          if (gi >= 0 && gi < analysisFrames)
            slice[static_cast<size_t>(i)] = global[static_cast<size_t>(gi)];
        }
        return slice;
      };

      if (!analysisData.originalDeltaPitch.empty())
        note.setOriginalDeltaPitch(sliceAnalysis(analysisData.originalDeltaPitch));
      if (!analysisData.originalPitch.empty())
        note.setOriginalPitch(sliceAnalysis(analysisData.originalPitch));
    }
  }
}

void Project::refreshNoteCachesForRange(int startFrame, int endFrame)
{
  for (auto& note : notes)
  {
    if (note.isRest())
      continue;
    // Only refresh notes that overlap the range
    if (note.getEndFrame() <= startFrame || note.getStartFrame() >= endFrame)
      continue;

    const int noteStart = note.getStartFrame();
    const int noteEnd = note.getEndFrame();
    const int len = noteEnd - noteStart;
    if (len <= 0)
      continue;

    const int totalFrames = editedData.getNumFrames();
    auto sliceFloat = [&](const std::vector<float>& global) {
      std::vector<float> slice(static_cast<size_t>(len));
      for (int i = 0; i < len; ++i)
      {
        int gi = noteStart + i;
        if (gi >= 0 && gi < totalFrames)
          slice[static_cast<size_t>(i)] = global[static_cast<size_t>(gi)];
      }
      return slice;
    };

    note.setBasePitch(sliceFloat(editedData.basePitch));
    note.setDeltaPitch(sliceFloat(editedData.deltaPitch));
    if (!editedData.voicingCurve.empty())
      note.setVoicingCurve(sliceFloat(editedData.voicingCurve));
    if (!editedData.breathCurve.empty())
      note.setBreathCurve(sliceFloat(editedData.breathCurve));
    if (!editedData.tensionCurve.empty())
      note.setTensionCurve(sliceFloat(editedData.tensionCurve));
  }
}
```

- [ ] **Step 3: Build and verify**

```powershell
cmake --build build --config Release --parallel 2>&1
```

Expected: compiles cleanly (new code only adds, doesn't remove anything yet).

- [ ] **Step 4: Commit**

```
git add Source/Models/Project.h Source/Models/Project.cpp
git commit -m "Add AnalysisData, EditedData, Listener system, and auditionBuffer to Project"
```

---

## Task 5: Refactor ProjectSerializer

**Files:**
- Modify: `Source/Models/ProjectSerializer.h`
- Modify: `Source/Models/ProjectSerializer.cpp`

Goal: Write new format (analysisData + editedData sections). Read old format (pitchData) for backward compatibility, converting to new on save. Stop serializing dead Note fields. Add vibrato mix/fadeIn/fadeOut.

- [ ] **Step 1: Update ProjectSerializer.h**

Replace the private methods `pitchDataToJson` / `pitchDataFromJson` with new ones:

```cpp
private:
    // Note serialization
    static juce::var noteToJson(const Note& note);
    static bool noteFromJson(Note& note, const juce::var& json);

    // New: analysisData + editedData serialization
    static juce::var analysisDataToJson(const AnalysisData& data);
    static bool analysisDataFromJson(AnalysisData& data, const juce::var& json);
    static juce::var editedDataToJson(const EditedData& data);
    static bool editedDataFromJson(EditedData& data, const juce::var& json);

    // Legacy: pitchData backward compat
    static bool legacyPitchDataFromJson(AudioData& audioData,
                                        EditedData& editedData,
                                        AnalysisData& analysisData,
                                        const juce::var& json);

    // Array helpers
    static juce::String floatArrayToString(const std::vector<float>& arr, int precision = 4);
    static std::vector<float> stringToFloatArray(const juce::String& str);
    static juce::String boolArrayToString(const std::vector<bool>& arr);
    static std::vector<bool> stringToBoolArray(const juce::String& str);

    ProjectSerializer() = delete;
```

- [ ] **Step 2: Update `toJson` — save new format**

In `ProjectSerializer::toJson()`, replace `pitchDataToJson` call with:

```cpp
    // Analysis data (original, immutable after detection)
    obj->setProperty("analysisData", analysisDataToJson(project.getAnalysisData()));

    // Edited data (global edit state)
    obj->setProperty("editedData", editedDataToJson(project.getEditedData()));
```

- [ ] **Step 3: Update `noteToJson` — remove dead fields, add vibrato extras**

Remove from `noteToJson`:
- `srcStartFrame`, `srcEndFrame` serialization
- `f0Values` serialization
- `deltaScale`, `deltaOffset` serialization
- `sourceVoicingCurve`, `sourceBreathCurve`, `sourceTensionCurve` serialization
- Per-note `voicingCurve`, `breathCurve`, `tensionCurve` serialization (curves are now global)

Add to vibrato object:
```cpp
    vibrato->setProperty("mix", note.getVibratoMix());
    vibrato->setProperty("fadeInMs", note.getVibratoFadeInMs());
    vibrato->setProperty("fadeOutMs", note.getVibratoFadeOutMs());
```

Remove the `highPassFilterStrength` and `lowPassFilterStrength` serialization (runtime only per spec).

- [ ] **Step 4: Update `noteFromJson` — backward compat + new fields**

Keep reading `srcStartFrame`/`srcEndFrame` for backward compat but don't require them:
```cpp
    note.setSrcStartFrame(static_cast<int>(json.getProperty("srcStartFrame", startFrame)));
    note.setSrcEndFrame(static_cast<int>(json.getProperty("srcEndFrame", endFrame)));
```

Read new vibrato fields with defaults:
```cpp
    note.setVibratoMix(static_cast<float>(vibratoVar.getProperty("mix", 0.0)));
    note.setVibratoFadeInMs(static_cast<float>(vibratoVar.getProperty("fadeInMs", 0.0)));
    note.setVibratoFadeOutMs(static_cast<float>(vibratoVar.getProperty("fadeOutMs", 0.0)));
```

Remove reading `deltaScale`, `deltaOffset`, `highPassFilterStrength`, `lowPassFilterStrength`, `f0Values`, `originalDeltaPitch` (from per-note — now in global analysisData), all `source*Curve` and per-note curve reads.

- [ ] **Step 5: Implement `analysisDataToJson` / `analysisDataFromJson`**

```cpp
juce::var ProjectSerializer::analysisDataToJson(const AnalysisData& data)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("originalF0", floatArrayToString(data.originalF0, 2));
    obj->setProperty("originalPitch", floatArrayToString(data.originalPitch, 4));
    obj->setProperty("originalDeltaPitch", floatArrayToString(data.originalDeltaPitch, 4));
    obj->setProperty("originalVoicedMask", boolArrayToString(data.originalVoicedMask));
    obj->setProperty("originalVADMask", boolArrayToString(data.originalVADMask));
    return juce::var(obj);
}

bool ProjectSerializer::analysisDataFromJson(AnalysisData& data, const juce::var& json)
{
    if (!json.isObject())
        return false;
    data.originalF0 = stringToFloatArray(json.getProperty("originalF0", "").toString());
    data.originalPitch = stringToFloatArray(json.getProperty("originalPitch", "").toString());
    data.originalDeltaPitch = stringToFloatArray(json.getProperty("originalDeltaPitch", "").toString());
    data.originalVoicedMask = stringToBoolArray(json.getProperty("originalVoicedMask", "").toString());
    data.originalVADMask = stringToBoolArray(json.getProperty("originalVADMask", "").toString());
    return true;
}
```

- [ ] **Step 6: Implement `editedDataToJson` / `editedDataFromJson`**

```cpp
juce::var ProjectSerializer::editedDataToJson(const EditedData& data)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("basePitch", floatArrayToString(data.basePitch, 4));
    obj->setProperty("deltaPitch", floatArrayToString(data.deltaPitch, 4));
    obj->setProperty("f0", floatArrayToString(data.f0, 2));
    obj->setProperty("voicedMask", boolArrayToString(data.voicedMask));
    obj->setProperty("vadMask", boolArrayToString(data.vadMask));
    obj->setProperty("voicingCurve", floatArrayToString(data.voicingCurve, 2));
    obj->setProperty("breathCurve", floatArrayToString(data.breathCurve, 2));
    obj->setProperty("tensionCurve", floatArrayToString(data.tensionCurve, 2));
    return juce::var(obj);
}

bool ProjectSerializer::editedDataFromJson(EditedData& data, const juce::var& json)
{
    if (!json.isObject())
        return false;
    data.basePitch = stringToFloatArray(json.getProperty("basePitch", "").toString());
    data.deltaPitch = stringToFloatArray(json.getProperty("deltaPitch", "").toString());
    data.f0 = stringToFloatArray(json.getProperty("f0", "").toString());
    data.voicedMask = stringToBoolArray(json.getProperty("voicedMask", "").toString());
    data.vadMask = stringToBoolArray(json.getProperty("vadMask", "").toString());
    data.voicingCurve = stringToFloatArray(json.getProperty("voicingCurve", "").toString());
    data.breathCurve = stringToFloatArray(json.getProperty("breathCurve", "").toString());
    data.tensionCurve = stringToFloatArray(json.getProperty("tensionCurve", "").toString());
    return true;
}
```

- [ ] **Step 7: Implement `legacyPitchDataFromJson` for backward compat**

```cpp
bool ProjectSerializer::legacyPitchDataFromJson(AudioData& audioData,
                                                 EditedData& editedData,
                                                 AnalysisData& analysisData,
                                                 const juce::var& json)
{
    if (!json.isObject())
        return false;

    // Old format stored everything flat in pitchData
    editedData.f0 = stringToFloatArray(json.getProperty("f0", "").toString());
    editedData.basePitch = stringToFloatArray(json.getProperty("basePitch", "").toString());
    editedData.deltaPitch = stringToFloatArray(json.getProperty("deltaPitch", "").toString());
    editedData.voicingCurve = stringToFloatArray(json.getProperty("voicingCurve", "").toString());
    editedData.breathCurve = stringToFloatArray(json.getProperty("breathCurve", "").toString());
    editedData.tensionCurve = stringToFloatArray(json.getProperty("tensionCurve", "").toString());
    editedData.voicedMask = stringToBoolArray(json.getProperty("voicedMask", "").toString());
    editedData.vadMask = stringToBoolArray(json.getProperty("vadMask", "").toString());

    // Also populate AudioData for backward compat with existing code paths
    audioData.f0 = editedData.f0;
    audioData.baseF0 = editedData.f0;
    audioData.basePitch = editedData.basePitch;
    audioData.deltaPitch = editedData.deltaPitch;
    audioData.voicingCurve = editedData.voicingCurve;
    audioData.breathCurve = editedData.breathCurve;
    audioData.tensionCurve = editedData.tensionCurve;
    audioData.voicedMask = editedData.voicedMask;
    audioData.vadMask = editedData.vadMask;
    audioData.f0EditedMask = stringToBoolArray(json.getProperty("f0EditedMask", "").toString());

    // For legacy files, analysis data = initial edited data
    analysisData.originalF0 = editedData.f0;
    analysisData.originalPitch = editedData.basePitch;
    analysisData.originalDeltaPitch = editedData.deltaPitch;
    analysisData.originalVoicedMask = editedData.voicedMask;
    analysisData.originalVADMask = editedData.vadMask;

    return true;
}
```

- [ ] **Step 8: Update `fromJson` to handle both formats**

In `ProjectSerializer::fromJson`, replace the pitchData loading section with:

```cpp
    // Try new format first
    auto analysisDataVar = json.getProperty("analysisData", juce::var());
    auto editedDataVar = json.getProperty("editedData", juce::var());

    if (analysisDataVar.isObject() && editedDataVar.isObject())
    {
        // New format
        analysisDataFromJson(project.getAnalysisData(), analysisDataVar);
        editedDataFromJson(project.getEditedData(), editedDataVar);

        // Sync to AudioData for backward compat with existing code
        auto& ad = project.getAudioData();
        const auto& ed = project.getEditedData();
        ad.f0 = ed.f0;
        ad.baseF0 = ed.f0;
        ad.basePitch = ed.basePitch;
        ad.deltaPitch = ed.deltaPitch;
        ad.voicingCurve = ed.voicingCurve;
        ad.breathCurve = ed.breathCurve;
        ad.tensionCurve = ed.tensionCurve;
        ad.voicedMask = ed.voicedMask;
        ad.vadMask = ed.vadMask;
    }
    else
    {
        // Legacy format
        auto pitchDataVar = json.getProperty("pitchData", juce::var());
        if (pitchDataVar.isObject())
        {
            legacyPitchDataFromJson(audioData, project.getEditedData(),
                                    project.getAnalysisData(), pitchDataVar);
        }
    }
```

- [ ] **Step 9: Build and verify**

```powershell
cmake --build build --config Release --parallel 2>&1
```

- [ ] **Step 10: Commit**

```
git add Source/Models/ProjectSerializer.h Source/Models/ProjectSerializer.cpp
git commit -m "Refactor ProjectSerializer: new analysisData/editedData format with legacy compat"
```

---

## Task 6: Create SnapshotHelper

**Files:**
- Create: `Source/Undo/SnapshotHelper.h`
- Create: `Source/Undo/SnapshotHelper.cpp`

- [ ] **Step 1: Create `Source/Undo/SnapshotHelper.h`**

```cpp
#pragma once

#include "../Models/Project.h"
#include <vector>

namespace SnapshotHelper
{
  std::vector<float> captureFloatRange(const std::vector<float>& global,
                                       int start, int end);

  std::vector<bool> captureBoolRange(const std::vector<bool>& global,
                                      int start, int end);

  void restoreFloatRange(std::vector<float>& global,
                         int start,
                         const std::vector<float>& snapshot);

  void restoreBoolRange(std::vector<bool>& global,
                        int start,
                        const std::vector<bool>& snapshot);

  /** Refresh note local caches for all notes overlapping [startFrame, endFrame). */
  void refreshNoteCache(Project& project, int startFrame, int endFrame);
} // namespace SnapshotHelper
```

- [ ] **Step 2: Create `Source/Undo/SnapshotHelper.cpp`**

```cpp
#include "SnapshotHelper.h"
#include <algorithm>

namespace SnapshotHelper
{

std::vector<float> captureFloatRange(const std::vector<float>& global,
                                     int start, int end)
{
  const int total = static_cast<int>(global.size());
  const int clampedStart = std::max(0, start);
  const int clampedEnd = std::min(total, end);
  if (clampedEnd <= clampedStart)
    return {};

  return std::vector<float>(
      global.begin() + clampedStart,
      global.begin() + clampedEnd);
}

std::vector<bool> captureBoolRange(const std::vector<bool>& global,
                                    int start, int end)
{
  const int total = static_cast<int>(global.size());
  const int clampedStart = std::max(0, start);
  const int clampedEnd = std::min(total, end);
  if (clampedEnd <= clampedStart)
    return {};

  return std::vector<bool>(
      global.begin() + clampedStart,
      global.begin() + clampedEnd);
}

void restoreFloatRange(std::vector<float>& global,
                       int start,
                       const std::vector<float>& snapshot)
{
  for (size_t i = 0; i < snapshot.size(); ++i)
  {
    const int gi = start + static_cast<int>(i);
    if (gi >= 0 && gi < static_cast<int>(global.size()))
      global[static_cast<size_t>(gi)] = snapshot[i];
  }
}

void restoreBoolRange(std::vector<bool>& global,
                      int start,
                      const std::vector<bool>& snapshot)
{
  for (size_t i = 0; i < snapshot.size(); ++i)
  {
    const int gi = start + static_cast<int>(i);
    if (gi >= 0 && gi < static_cast<int>(global.size()))
      global[static_cast<size_t>(gi)] = snapshot[i];
  }
}

void refreshNoteCache(Project& project, int startFrame, int endFrame)
{
  project.refreshNoteCachesForRange(startFrame, endFrame);
}

} // namespace SnapshotHelper
```

- [ ] **Step 3: Commit**

```
git add Source/Undo/SnapshotHelper.h Source/Undo/SnapshotHelper.cpp
git commit -m "Add SnapshotHelper utilities for undo snapshot capture/restore"
```

---

## Task 7: Create StretchProcessor

**Files:**
- Create: `Source/Audio/Synthesis/StretchProcessor.h`
- Create: `Source/Audio/Synthesis/StretchProcessor.cpp`

- [ ] **Step 1: Create `Source/Audio/Synthesis/StretchProcessor.h`**

```cpp
#pragma once

#include "../../Models/Project.h"
#include "../../Models/EditedData.h"
#include <vector>

/**
 * Pure algorithm for time-stretching operations.
 * Extracted from StretchHandler (which retains UI interaction only).
 * All methods are static — no state.
 */
class StretchProcessor
{
public:
  /**
   * Stretch mel spectrogram using warp markers (linear interpolation).
   * Returns new mel with length = markers.back().outputFrame.
   */
  static std::vector<std::vector<float>> stretchMel(
      const std::vector<std::vector<float>>& mel,
      const std::vector<Project::WarpMarker>& markers);

  /**
   * Stretch all arrays in editedData using warp markers.
   *   basePitch/masks → nearest neighbor
   *   deltaPitch/curves → linear interpolation
   *   Recomputes f0 from basePitch + deltaPitch.
   */
  static void stretchEditedData(
      EditedData& edited,
      const std::vector<Project::WarpMarker>& markers,
      int newTotalFrames);

  /**
   * Remap note startFrame/endFrame based on warp markers.
   */
  static void remapNoteFrames(
      std::vector<Note>& notes,
      const std::vector<Project::WarpMarker>& markers,
      int affectedSourceStart = 0,
      int affectedSourceEnd = INT_MAX);

  /** Map a source frame to an output frame (linear interpolation). */
  static float mapFrame(const std::vector<Project::WarpMarker>& markers,
                        float sourceFrame);

  /** Map an output frame back to a source frame (linear interpolation). */
  static float inverseMapFrame(const std::vector<Project::WarpMarker>& markers,
                               float outputFrame);
};
```

- [ ] **Step 2: Create `Source/Audio/Synthesis/StretchProcessor.cpp`**

```cpp
#include "StretchProcessor.h"
#include "../../Utils/CurveResampler.h"
#include "../../Utils/Constants.h"
#include <cmath>
#include <algorithm>
#include <climits>

float StretchProcessor::mapFrame(const std::vector<Project::WarpMarker>& markers,
                                 float sourceFrame)
{
  if (markers.empty())
    return sourceFrame;
  if (markers.size() == 1)
    return static_cast<float>(markers[0].outputFrame);

  // Find the segment containing sourceFrame
  for (size_t i = 1; i < markers.size(); ++i)
  {
    if (sourceFrame <= static_cast<float>(markers[i].sourceFrame))
    {
      float srcStart = static_cast<float>(markers[i - 1].sourceFrame);
      float srcEnd = static_cast<float>(markers[i].sourceFrame);
      float outStart = static_cast<float>(markers[i - 1].outputFrame);
      float outEnd = static_cast<float>(markers[i].outputFrame);
      float segLen = srcEnd - srcStart;
      if (segLen <= 0.0f)
        return outStart;
      float t = (sourceFrame - srcStart) / segLen;
      return outStart + t * (outEnd - outStart);
    }
  }
  // Beyond last marker
  return static_cast<float>(markers.back().outputFrame);
}

float StretchProcessor::inverseMapFrame(
    const std::vector<Project::WarpMarker>& markers,
    float outputFrame)
{
  if (markers.empty())
    return outputFrame;
  if (markers.size() == 1)
    return static_cast<float>(markers[0].sourceFrame);

  for (size_t i = 1; i < markers.size(); ++i)
  {
    if (outputFrame <= static_cast<float>(markers[i].outputFrame))
    {
      float outStart = static_cast<float>(markers[i - 1].outputFrame);
      float outEnd = static_cast<float>(markers[i].outputFrame);
      float srcStart = static_cast<float>(markers[i - 1].sourceFrame);
      float srcEnd = static_cast<float>(markers[i].sourceFrame);
      float segLen = outEnd - outStart;
      if (segLen <= 0.0f)
        return srcStart;
      float t = (outputFrame - outStart) / segLen;
      return srcStart + t * (srcEnd - srcStart);
    }
  }
  return static_cast<float>(markers.back().sourceFrame);
}

void StretchProcessor::remapNoteFrames(
    std::vector<Note>& notes,
    const std::vector<Project::WarpMarker>& markers,
    int affectedSourceStart,
    int affectedSourceEnd)
{
  for (auto& note : notes)
  {
    int srcStart = note.getSrcStartFrame();
    int srcEnd = note.getSrcEndFrame();

    if (srcEnd <= affectedSourceStart || srcStart >= affectedSourceEnd)
      continue;

    int newStart = static_cast<int>(std::round(mapFrame(markers, static_cast<float>(srcStart))));
    int newEnd = static_cast<int>(std::round(mapFrame(markers, static_cast<float>(srcEnd))));
    if (newEnd <= newStart)
      newEnd = newStart + 1;

    note.setStartFrame(newStart);
    note.setEndFrame(newEnd);
  }
}

std::vector<std::vector<float>> StretchProcessor::stretchMel(
    const std::vector<std::vector<float>>& mel,
    const std::vector<Project::WarpMarker>& markers)
{
  if (mel.empty() || markers.size() < 2)
    return mel;

  int newLen = markers.back().outputFrame;
  if (newLen <= 0)
    return {};

  // Use CurveResampler-style linear interpolation per marker segment
  std::vector<std::vector<float>> result(static_cast<size_t>(newLen));
  const int numMels = static_cast<int>(mel[0].size());

  for (int outFrame = 0; outFrame < newLen; ++outFrame)
  {
    float srcF = inverseMapFrame(markers, static_cast<float>(outFrame));
    int srcIdx = static_cast<int>(std::floor(srcF));
    float frac = srcF - static_cast<float>(srcIdx);

    int srcMax = static_cast<int>(mel.size()) - 1;
    srcIdx = std::clamp(srcIdx, 0, srcMax);
    int srcNext = std::min(srcIdx + 1, srcMax);

    result[static_cast<size_t>(outFrame)].resize(static_cast<size_t>(numMels));
    for (int m = 0; m < numMels; ++m)
    {
      result[static_cast<size_t>(outFrame)][static_cast<size_t>(m)] =
          mel[static_cast<size_t>(srcIdx)][static_cast<size_t>(m)] * (1.0f - frac) +
          mel[static_cast<size_t>(srcNext)][static_cast<size_t>(m)] * frac;
    }
  }
  return result;
}

void StretchProcessor::stretchEditedData(
    EditedData& edited,
    const std::vector<Project::WarpMarker>& markers,
    int newTotalFrames)
{
  if (markers.size() < 2 || newTotalFrames <= 0)
    return;

  auto resampleLinear = [&](const std::vector<float>& src) {
    std::vector<float> dst(static_cast<size_t>(newTotalFrames));
    for (int i = 0; i < newTotalFrames; ++i)
    {
      float srcF = inverseMapFrame(markers, static_cast<float>(i));
      int srcIdx = static_cast<int>(std::floor(srcF));
      float frac = srcF - static_cast<float>(srcIdx);
      int srcMax = static_cast<int>(src.size()) - 1;
      srcIdx = std::clamp(srcIdx, 0, std::max(0, srcMax));
      int srcNext = std::min(srcIdx + 1, std::max(0, srcMax));
      if (src.empty())
      {
        dst[static_cast<size_t>(i)] = 0.0f;
      }
      else
      {
        dst[static_cast<size_t>(i)] =
            src[static_cast<size_t>(srcIdx)] * (1.0f - frac) +
            src[static_cast<size_t>(srcNext)] * frac;
      }
    }
    return dst;
  };

  auto resampleNearest = [&](const std::vector<float>& src) {
    std::vector<float> dst(static_cast<size_t>(newTotalFrames));
    for (int i = 0; i < newTotalFrames; ++i)
    {
      float srcF = inverseMapFrame(markers, static_cast<float>(i));
      int srcIdx = static_cast<int>(std::round(srcF));
      int srcMax = static_cast<int>(src.size()) - 1;
      srcIdx = std::clamp(srcIdx, 0, std::max(0, srcMax));
      dst[static_cast<size_t>(i)] = src.empty() ? 0.0f : src[static_cast<size_t>(srcIdx)];
    }
    return dst;
  };

  auto resampleNearestBool = [&](const std::vector<bool>& src) {
    std::vector<bool> dst(static_cast<size_t>(newTotalFrames));
    for (int i = 0; i < newTotalFrames; ++i)
    {
      float srcF = inverseMapFrame(markers, static_cast<float>(i));
      int srcIdx = static_cast<int>(std::round(srcF));
      int srcMax = static_cast<int>(src.size()) - 1;
      srcIdx = std::clamp(srcIdx, 0, std::max(0, srcMax));
      dst[static_cast<size_t>(i)] = src.empty() ? false : src[static_cast<size_t>(srcIdx)];
    }
    return dst;
  };

  // basePitch, masks → nearest neighbor
  edited.basePitch = resampleNearest(edited.basePitch);
  edited.voicedMask = resampleNearestBool(edited.voicedMask);
  edited.vadMask = resampleNearestBool(edited.vadMask);

  // deltaPitch, curves → linear interpolation
  edited.deltaPitch = resampleLinear(edited.deltaPitch);
  edited.voicingCurve = resampleLinear(edited.voicingCurve);
  edited.breathCurve = resampleLinear(edited.breathCurve);
  edited.tensionCurve = resampleLinear(edited.tensionCurve);

  // Recompute f0 from basePitch + deltaPitch
  edited.f0.resize(static_cast<size_t>(newTotalFrames));
  for (int i = 0; i < newTotalFrames; ++i)
  {
    float midi = edited.basePitch[static_cast<size_t>(i)] +
                 edited.deltaPitch[static_cast<size_t>(i)];
    edited.f0[static_cast<size_t>(i)] =
        440.0f * std::pow(2.0f, (midi - 69.0f) / 12.0f);
  }
}
```

- [ ] **Step 3: Build and verify**

```powershell
cmake --build build --config Release --parallel 2>&1
```

Note: You may need to add `StretchProcessor.cpp` to CMakeLists.txt. The project uses source globbing (`file(GLOB_RECURSE ...)`), so new `.cpp` files in existing `Source/` subdirectories should be picked up automatically. If not, re-run cmake configure:

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON -DUSE_ASIO=ON
cmake --build build --config Release --parallel
```

- [ ] **Step 4: Commit**

```
git add Source/Audio/Synthesis/StretchProcessor.h Source/Audio/Synthesis/StretchProcessor.cpp
git commit -m "Add StretchProcessor: pure algorithm for warp-based time stretching"
```

---

## Task 8: Create TreeValueMonitor Debug Window

**Files:**
- Create: `Source/UI/Debug/TreeValueMonitor.h`
- Create: `Source/UI/Debug/TreeValueMonitor.cpp`

- [ ] **Step 1: Create `Source/UI/Debug/TreeValueMonitor.h`**

```cpp
#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Models/ProjectListener.h"

/**
 * Debug window that displays the Project's live state as a tree.
 * Implements ProjectListener to auto-refresh on changes.
 * Open via menu or programmatically during development.
 */
class TreeValueMonitor : public juce::DocumentWindow,
                         public ProjectListener
{
public:
  explicit TreeValueMonitor(Project* project);
  ~TreeValueMonitor() override;

  void closeButtonPressed() override;

  // ProjectListener
  void onProjectChanged(ProjectChangeType type,
                        int affectedNoteIndex,
                        int rangeStart,
                        int rangeEnd) override;

  void refresh();

private:
  class ContentComponent : public juce::Component
  {
  public:
    ContentComponent();
    void paint(juce::Graphics& g) override;
    void resized() override;
    void setText(const juce::String& text);

  private:
    juce::TextEditor textEditor;
  };

  Project* project = nullptr;
  ContentComponent content;

  juce::String buildDisplayText() const;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TreeValueMonitor)
};
```

- [ ] **Step 2: Create `Source/UI/Debug/TreeValueMonitor.cpp`**

```cpp
#include "TreeValueMonitor.h"

// --- ContentComponent ---
TreeValueMonitor::ContentComponent::ContentComponent()
{
  textEditor.setMultiLine(true);
  textEditor.setReadOnly(true);
  textEditor.setScrollbarsShown(true);
  textEditor.setFont(juce::Font(juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::plain));
  addAndMakeVisible(textEditor);
}

void TreeValueMonitor::ContentComponent::paint(juce::Graphics& g)
{
  g.fillAll(juce::Colours::black);
}

void TreeValueMonitor::ContentComponent::resized()
{
  textEditor.setBounds(getLocalBounds().reduced(2));
}

void TreeValueMonitor::ContentComponent::setText(const juce::String& text)
{
  textEditor.setText(text, false);
}

// --- TreeValueMonitor ---
TreeValueMonitor::TreeValueMonitor(Project* proj)
    : juce::DocumentWindow("Project Monitor",
                           juce::Colours::darkgrey,
                           juce::DocumentWindow::allButtons),
      project(proj)
{
  setContentNonOwned(&content, true);
  setResizable(true, false);
  setSize(500, 700);
  setVisible(true);

  if (project)
    project->addListener(this);

  refresh();
}

TreeValueMonitor::~TreeValueMonitor()
{
  if (project)
    project->removeListener(this);
}

void TreeValueMonitor::closeButtonPressed()
{
  setVisible(false);
}

void TreeValueMonitor::onProjectChanged(ProjectChangeType /*type*/,
                                        int /*affectedNoteIndex*/,
                                        int /*rangeStart*/,
                                        int /*rangeEnd*/)
{
  juce::MessageManager::callAsync([this]() { refresh(); });
}

void TreeValueMonitor::refresh()
{
  content.setText(buildDisplayText());
}

juce::String TreeValueMonitor::buildDisplayText() const
{
  if (!project)
    return "No project loaded.";

  juce::String text;
  text << "=== PROJECT ===\n";
  text << "Name: " << project->getName() << "\n";
  text << "AudioPath: " << project->getFilePath().getFullPathName() << "\n";
  text << "GlobalPitchOffset: " << juce::String(project->getGlobalPitchOffset(), 2) << "\n";
  text << "FormantShift: " << juce::String(project->getFormantShift(), 2) << "\n";
  text << "Volume: " << juce::String(project->getVolume(), 2) << " dB\n";
  text << "Modified: " << (project->isModified() ? "yes" : "no") << "\n\n";

  // AnalysisData
  const auto& ad = project->getAnalysisData();
  text << "=== ANALYSIS DATA ===\n";
  text << "Frames: " << ad.getNumFrames() << "\n";
  text << "isEmpty: " << (ad.isEmpty() ? "yes" : "no") << "\n\n";

  // EditedData
  const auto& ed = project->getEditedData();
  text << "=== EDITED DATA ===\n";
  text << "Frames: " << ed.getNumFrames() << "\n";
  if (!ed.basePitch.empty())
  {
    float minBP = *std::min_element(ed.basePitch.begin(), ed.basePitch.end());
    float maxBP = *std::max_element(ed.basePitch.begin(), ed.basePitch.end());
    text << "BasePitch range: [" << juce::String(minBP, 2) << ", "
         << juce::String(maxBP, 2) << "]\n";
  }
  text << "\n";

  // WarpMarkers
  const auto& markers = project->getWarpMarkers();
  text << "=== WARP MARKERS (" << (int)markers.size() << ") ===\n";
  for (size_t i = 0; i < markers.size(); ++i)
  {
    text << "  [" << (int)i << "] src=" << markers[i].sourceFrame
         << " out=" << markers[i].outputFrame << "\n";
  }
  text << "\n";

  // Notes
  const auto& notes = project->getNotes();
  text << "=== NOTES (" << (int)notes.size() << ") ===\n";
  for (size_t i = 0; i < notes.size(); ++i)
  {
    const auto& n = notes[i];
    text << "  [" << (int)i << "] frames=[" << n.getStartFrame() << ".."
         << n.getEndFrame() << ") midi=" << juce::String(n.getMidiNote(), 2)
         << " offset=" << juce::String(n.getPitchOffset(), 2)
         << (n.isRest() ? " REST" : "")
         << (n.isDirty() ? " DIRTY" : "")
         << (n.isSelected() ? " SEL" : "")
         << "\n";
  }

  return text;
}
```

- [ ] **Step 3: Build and verify**

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON -DUSE_ASIO=ON
cmake --build build --config Release --parallel
```

- [ ] **Step 4: Commit**

```
git add Source/UI/Debug/TreeValueMonitor.h Source/UI/Debug/TreeValueMonitor.cpp
git commit -m "Add TreeValueMonitor debug window for live project state inspection"
```

---

## Task 9: Wire Up Analysis → AnalysisData + EditedData

**Files:**
- Modify: `Source/Audio/EditorController.cpp`
- Modify: `Source/Utils/PitchCurveProcessor.cpp`
- Modify: `Source/Utils/HNSepCurveProcessor.cpp`

Goal: After pitch detection and note segmentation complete, populate both the old AudioData arrays AND the new AnalysisData/EditedData. This is the bridge that keeps both systems in sync during incremental migration.

- [ ] **Step 1: After analysis completes in EditorController, populate AnalysisData**

In `Source/Audio/EditorController.cpp`, find the `analyzeAudio` method where pitch detection results are stored into `audioData`. After the existing code that populates `audioData.f0`, `audioData.basePitch`, etc., add:

```cpp
    // Populate AnalysisData (immutable copy of initial analysis)
    auto& analysis = targetProject.getAnalysisData();
    analysis.originalF0 = audioData.f0;
    analysis.originalPitch = audioData.basePitch;
    analysis.originalDeltaPitch = audioData.deltaPitch;
    analysis.originalVoicedMask = audioData.voicedMask;
    analysis.originalVADMask = audioData.vadMask;

    // Initialize EditedData as copy of analysis
    auto& edited = targetProject.getEditedData();
    edited.basePitch = audioData.basePitch;
    edited.deltaPitch = audioData.deltaPitch;
    edited.f0 = audioData.f0;
    edited.voicedMask = audioData.voicedMask;
    edited.vadMask = audioData.vadMask;
    edited.voicingCurve = audioData.voicingCurve;
    edited.breathCurve = audioData.breathCurve;
    edited.tensionCurve = audioData.tensionCurve;
```

- [ ] **Step 2: After note segmentation, refresh note caches**

After `segmentIntoNotes` completes and notes are created, add:

```cpp
    targetProject.refreshNoteCaches();
```

- [ ] **Step 3: Initialize auditionBuffer after analysis**

After audio loading and analysis, before playback is enabled:

```cpp
    targetProject.initAuditionBufferFromOriginal();
```

- [ ] **Step 4: Build and verify**

```powershell
cmake --build build --config Release --parallel 2>&1
```

- [ ] **Step 5: Commit**

```
git add Source/Audio/EditorController.cpp
git commit -m "Wire analysis results into AnalysisData and EditedData"
```

---

## Task 10: Update Undo Actions for Global Data

**Files:**
- Modify: `Source/Undo/DragActions.h`
- Modify: `Source/Undo/F0Actions.h`
- Modify: `Source/Undo/NoteActions.h`
- Modify: `Source/Undo/ParameterActions.h`
- Modify: `Source/Undo/PitchToolAction.h`
- Modify: `Source/Undo/TimingActions.h`

Goal: Each undo action must now also capture/restore the corresponding slice of global EditedData alongside the per-note modifications. This is the minimal change to make undo work with the new architecture — we're NOT replacing the classes yet, just augmenting them.

- [ ] **Step 1: Update DragActions.h — NotePitchDragAction**

Add `EditedData*` parameter. In undo/redo, after modifying note midiNote and f0Array, also restore the corresponding basePitch slice in editedData. Use `SnapshotHelper::captureFloatRange` for before/after basePitch snapshots.

The key change pattern for each action:

```cpp
    // In constructor, capture editedData basePitch before snapshot
    // In undo(), restore before basePitch + old f0 + old midiNote
    // In redo(), restore after basePitch + new f0 + new midiNote
    // Then call project.refreshNoteCachesForRange(start, end)
```

This is a large but mechanical change. Each action class follows the same pattern:
1. Add `Project*` pointer if not already present
2. Add before/after snapshot of affected editedData slices (using SnapshotHelper)
3. In undo/redo, restore the slice then call `refreshNoteCachesForRange`

- [ ] **Step 2: Update remaining actions following the same pattern**

Apply the same augmentation to F0Actions, ParameterActions, PitchToolAction. TimingActions (WarpMarkerStateAction) is more complex — it needs to store before/after editedData + note frames for stretch undo.

- [ ] **Step 3: Build and verify**

```powershell
cmake --build build --config Release --parallel 2>&1
```

- [ ] **Step 4: Commit**

```
git add Source/Undo/
git commit -m "Augment undo actions to capture/restore global EditedData slices"
```

---

## Task 11: Adapt PianoRoll and UI for New Data Model

**Files:**
- Modify: `Source/UI/PianoRollComponent.h`
- Modify: `Source/UI/PianoRollComponent.cpp`
- Modify: `Source/UI/ToolbarComponent.h`
- Modify: `Source/UI/ToolbarComponent.cpp`
- Modify: Various PianoRoll state handlers

This is the largest task. The key changes:
1. Where UI reads note pitch data for display, use the existing getters (they now return cached data from global — no logic change needed)
2. Where UI writes pitch edits, also write to global editedData
3. Where UI references removed fields (clipWaveform, srcClipWaveform, f0Values, sourceXxxCurve, deltaScale, deltaOffset), replace with alternatives

- [ ] **Step 1: Fix references to removed Note fields in PianoRollComponent**

Search for uses of:
- `getClipWaveform()` / `getSrcClipWaveform()` — replace with direct slicing from `audioData.waveform` / `audioData.originalWaveform` using note frame ranges
- `getF0Values()` — replace with slicing from `editedData.f0` or `analysisData.originalF0`
- `getDeltaScale()` / `getDeltaOffset()` — remove usage or replace with 1.0/0.0 constants
- `getSourceVoicingCurve()` etc. — replace with slicing from `analysisData`

- [ ] **Step 2: Fix references in ToolbarComponent**

Search for `HACHITUNE_ENABLE_STRETCH` guards — these will be removed in Task 13, but for now ensure the code inside compiles with the new data model.

- [ ] **Step 3: Fix references in PianoRoll state handlers**

Files in `Source/UI/PianoRoll/States/` — update handlers that reference removed fields.

- [ ] **Step 4: Fix references in HNSepCurveProcessor**

Remove references to `sourceVoicingCurve`, `sourceBreathCurve`, `sourceTensionCurve`. The `extractNoteCurvesFromMaster` and `rebuildCurvesFromNotes` functions need to work with global editedData curves instead.

- [ ] **Step 5: Fix references in PitchCurveProcessor**

Ensure `rebuildBaseFromNotes` and `rebuildCurvesFromSource` also update `editedData` alongside the existing `audioData` arrays.

- [ ] **Step 6: Build and fix all remaining compilation errors**

```powershell
cmake --build build --config Release --parallel 2>&1
```

Iterate: fix errors → rebuild → repeat until clean.

- [ ] **Step 7: Commit**

```
git add -A
git commit -m "Adapt UI components and processors for new data model"
```

---

## Task 12: Refactor TensionProcessor — Add STFT-based Path

**Files:**
- Modify: `Source/Audio/TensionProcessor.h`
- Modify: `Source/Audio/TensionProcessor.cpp`
- Modify: `Source/Audio/EditorController.cpp` (STFT cache computation after HNSep)

Goal: Add `processSegmentFromSTFT()` that accepts precomputed STFT cache + frame range, skipping forward FFT. Also compute and cache STFT results after HNSep separation in EditorController.

- [ ] **Step 1: Add `processSegmentFromSTFT` declaration to TensionProcessor.h**

After the existing `processSegment` method (around line 54), add:

```cpp
  /**
   * Result of tension processing: mixed waveform + mel spectrogram.
   */
  struct TensionResult
  {
    std::vector<float> mixedWaveform;
    std::vector<std::vector<float>> mel; // [T, NUM_MELS]
  };

  /**
   * Process [startFrame, endFrame) using precomputed STFT cache.
   * Skips forward FFT; applies tension tilt in frequency domain,
   * then ISTFT + mix + mel extraction.
   *
   * @param harmonicSTFT  Global precomputed harmonic STFT [T_stft * kFFTBin] interleaved real/imag
   * @param noiseSTFT     Global precomputed noise STFT
   * @param startFrame    Start of processing range (in hop-aligned frames)
   * @param endFrame      End of processing range (exclusive)
   * @param voicingCurve  Global voicing curve
   * @param breathCurve   Global breath curve
   * @param tensionCurve  Global tension curve
   * @return              TensionResult with mixed waveform and mel for the range
   */
  TensionResult processSegmentFromSTFT(
      const std::vector<std::complex<float>>& harmonicSTFT,
      const std::vector<std::complex<float>>& noiseSTFT,
      int startFrame, int endFrame,
      const std::vector<float>& voicingCurve,
      const std::vector<float>& breathCurve,
      const std::vector<float>& tensionCurve) const;
```

- [ ] **Step 2: Implement `processSegmentFromSTFT` in TensionProcessor.cpp**

This method extracts the STFT frames for [startFrame, endFrame), applies the same spectral tilt logic as the existing `preEmphasisBaseTensionSegment` (but working directly in the frequency domain), then ISTFT + overlap-add. The mel extraction uses the same logic as the vocoder's mel filterbank.

The implementation follows the same pattern as the existing `processSegment` but receives pre-computed STFT bins instead of raw waveform, allowing it to skip `forwardFFT`. For each STFT frame in the range:
1. Apply voicing scale to harmonic, breath scale to noise
2. Apply tension tilt to harmonic spectral magnitudes
3. ISTFT both components
4. Mix and produce output waveform
5. Compute mel from mixed waveform using filterbank

The detailed implementation depends on the exact STFT storage format. The key signature change is confirmed; implementation details should match the existing `processSegment` logic with the FFT step removed.

- [ ] **Step 3: Add STFT cache computation in EditorController after HNSep**

In `Source/Audio/EditorController.cpp`, in the `runHNSepSeparation` method, after harmonic/noise waveforms are populated, add:

```cpp
    // Compute and cache STFT for TensionProcessor
    // This allows processSegmentFromSTFT to skip forward FFT
    auto& harmonicSTFT = project.getHarmonicSTFT();
    auto& noiseSTFT = project.getNoiseSTFT();
    // ... compute STFT of harmonicWaveform and noiseWaveform
    // Store as interleaved complex<float> arrays
```

The STFT computation uses the same parameters as TensionProcessor (kFFTSize=2048, kHopSize=512, kWinSize=2048) with the Hann window.

- [ ] **Step 4: Build and verify**

```powershell
cmake --build build --config Release --parallel 2>&1
```

- [ ] **Step 5: Commit**

```
git add Source/Audio/TensionProcessor.h Source/Audio/TensionProcessor.cpp Source/Audio/EditorController.cpp
git commit -m "Add STFT-based TensionProcessor path and cache STFT after HNSep"
```

---

## Task 13: Refactor IncrementalSynthesizer — Global Data + Simplified Blend

**Files:**
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.h`
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.cpp`

Goal: Synthesizer reads only from global `editedData.f0` and global `melSpectrogram`. Blend simplified to 1-frame crossfade with auditionBuffer. New ResynthRange computation.

- [ ] **Step 1: Add ResynthRange struct to IncrementalSynthesizer.h**

```cpp
  struct ResynthRange
  {
    int startFrame = -1;
    int endFrame = -1;
    bool needsMelUpdate = false;  // true = curve edit, needs tension recomputation

    bool isValid() const { return startFrame >= 0 && endFrame > startFrame; }
  };

  /** Compute the range that needs resynthesis from dirty notes + dirty ranges. */
  ResynthRange computeResynthRange();
```

- [ ] **Step 2: Implement `computeResynthRange` in IncrementalSynthesizer.cpp**

Replace the old `computeSynthesisRange` with:

```cpp
IncrementalSynthesizer::ResynthRange IncrementalSynthesizer::computeResynthRange()
{
  if (!project)
    return {};

  ResynthRange range;

  // 1. Collect dirty notes' frame ranges
  int dirtyStart = INT_MAX;
  int dirtyEnd = INT_MIN;
  for (const auto& note : project->getNotes())
  {
    if (note.isDirty() || note.isSynthDirty())
    {
      dirtyStart = std::min(dirtyStart, note.getStartFrame());
      dirtyEnd = std::max(dirtyEnd, note.getEndFrame());
    }
  }

  // 2. Merge f0 dirty range
  if (project->hasF0DirtyRange())
  {
    auto [f0s, f0e] = project->getF0DirtyRange();
    dirtyStart = std::min(dirtyStart, f0s);
    dirtyEnd = std::max(dirtyEnd, f0e);
  }

  // 3. Merge param dirty range
  if (project->hasParamDirtyRange())
  {
    auto [ps, pe] = project->getParamDirtyRange();
    dirtyStart = std::min(dirtyStart, ps);
    dirtyEnd = std::max(dirtyEnd, pe);
    range.needsMelUpdate = true;
  }

  if (dirtyStart >= dirtyEnd)
    return {};

  // 4. Expand to VAD=0 boundaries
  const auto& vadMask = project->getEditedData().vadMask;
  const int totalFrames = static_cast<int>(vadMask.size());

  int start = dirtyStart;
  while (start > 0 && (start >= totalFrames || vadMask[static_cast<size_t>(start)]))
    --start;

  int end = dirtyEnd;
  while (end < totalFrames && vadMask[static_cast<size_t>(end)])
    ++end;

  range.startFrame = std::max(0, start);
  range.endFrame = std::min(totalFrames, end);
  return range;
}
```

- [ ] **Step 3: Refactor `synthesizeRegion` to use global data + simple crossfade**

The new flow:
1. Call `computeResynthRange()`
2. If `needsMelUpdate`, call TensionProcessor to update mel for the range
3. Extract f0 and mel slices from global editedData and melSpectrogram
4. Call vocoder.synthesize()
5. Write result into auditionBuffer with 1-frame (HOP_SIZE samples) linear crossfade at boundaries
6. Update affected notes' synthWaveform cache
7. Clear dirty flags

Replace the old complex blend mask with:

```cpp
    // Simple 1-frame crossfade at boundaries
    const int crossfadeSamples = HOP_SIZE; // 512
    auto& audition = project->getAuditionBuffer();

    for (int i = 0; i < numSynthSamples; ++i)
    {
      int sampleIdx = startSample + i;
      if (sampleIdx < 0 || sampleIdx >= audition.getNumSamples())
        continue;

      float synthSample = synthResult[static_cast<size_t>(i)];
      float origSample = audition.getSample(0, sampleIdx);

      // Crossfade ramp at start
      float blend = 1.0f;
      if (i < crossfadeSamples)
        blend = static_cast<float>(i) / static_cast<float>(crossfadeSamples);
      // Crossfade ramp at end
      int fromEnd = numSynthSamples - 1 - i;
      if (fromEnd < crossfadeSamples)
        blend = std::min(blend, static_cast<float>(fromEnd) / static_cast<float>(crossfadeSamples));

      audition.setSample(0, sampleIdx, origSample * (1.0f - blend) + synthSample * blend);
    }
```

- [ ] **Step 4: Remove old `generateBlendMask` method**

Delete the `generateBlendMask` declaration from the header and its implementation from the cpp file.

- [ ] **Step 5: Build and verify**

```powershell
cmake --build build --config Release --parallel 2>&1
```

- [ ] **Step 6: Commit**

```
git add Source/Audio/Synthesis/IncrementalSynthesizer.h Source/Audio/Synthesis/IncrementalSynthesizer.cpp
git commit -m "Refactor IncrementalSynthesizer: global data, ResynthRange, 1-frame crossfade"
```

---

## Task 14: Remove ENABLE_NOTE_STRETCH and Dead Code

**Files:**
- Modify: `CMakeLists.txt`
- Modify: All files with `#if HACHITUNE_ENABLE_STRETCH` guards
- Modify: `Source/Models/Note.h` — finally remove deprecated fields

- [ ] **Step 1: Remove CMake option**

In `CMakeLists.txt`, find and remove the `ENABLE_NOTE_STRETCH` option and the `target_compile_definitions(hachitune_core PUBLIC HACHITUNE_ENABLE_STRETCH=1)` line (around line 578).

- [ ] **Step 2: Remove `#if HACHITUNE_ENABLE_STRETCH` guards from all files**

Files to modify (keep the code inside the guards, remove the `#if` / `#endif`):
- `Source/UI/PianoRollComponent.h` (9 locations)
- `Source/UI/PianoRollComponent.cpp` (7 locations)
- `Source/UI/ToolbarComponent.h` (6 locations)
- `Source/UI/ToolbarComponent.cpp` (24 locations)
- `Source/UI/PianoRoll/States/StretchHandler.h` (file-level guard)
- `Source/UI/PianoRoll/States/StretchHandler.cpp` (file-level guard)
- `Source/UI/MainComponent.cpp` (1 location)

For each file, remove the `#if HACHITUNE_ENABLE_STRETCH` and corresponding `#endif` lines, keeping the code between them.

- [ ] **Step 3: Remove dead fields from Note.h**

Now that all callers are updated (Task 11), fully remove from Note.h:
- `deltaScale`, `deltaOffset` — fields + getters/setters
- `f0Values` — field + getters/setters
- `clipWaveform`, `srcClipWaveform` — fields + getters/setters
- `sourceVoicingCurve`, `sourceBreathCurve`, `sourceTensionCurve` — fields + getters/setters

- [ ] **Step 4: Build and verify**

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON -DUSE_ASIO=ON
cmake --build build --config Release --parallel
```

- [ ] **Step 5: Commit**

```
git add -A
git commit -m "Remove ENABLE_NOTE_STRETCH flag and dead Note fields"
```

---

## Task 15: Update CMakeLists.txt for New Files

**Files:**
- Modify: `CMakeLists.txt`

The project uses `file(GLOB_RECURSE ...)` for source files, so new files in existing directories should be auto-discovered. However, verify the new directories are covered:
- `Source/UI/Debug/` — check if the glob pattern covers this
- `Source/Undo/SnapshotHelper.cpp` — check if `.cpp` files in Undo are included

- [ ] **Step 1: Verify glob patterns cover new files**

Check `CMakeLists.txt` for the glob patterns. If `Source/UI/Debug/` or `Source/Undo/*.cpp` are not covered, add them.

- [ ] **Step 2: Build and verify**

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON -DUSE_ASIO=ON
cmake --build build --config Release --parallel
```

- [ ] **Step 3: Commit (if changes needed)**

```
git add CMakeLists.txt
git commit -m "Update CMakeLists.txt glob patterns for new source files"
```

---

## Task 16: Update CLAUDE.md and AGENTS.md

**Files:**
- Modify: `CLAUDE.md` (if it exists)
- Modify: `AGENTS.md`

- [ ] **Step 1: Update AGENTS.md**

Update the following sections:
- **Directory Structure**: Add `Models/AnalysisData.h`, `Models/EditedData.h`, `Models/ProjectListener.h`, `Undo/SnapshotHelper.h/.cpp`, `UI/Debug/TreeValueMonitor.h/.cpp`, `Audio/Synthesis/StretchProcessor.h/.cpp`
- **Architecture notes**: Replace OUTDATED paragraphs about stretch model and incremental synthesis
- Remove references to `ENABLE_NOTE_STRETCH`
- Update Pitch model to mention `pitchOffset`, vibrato `mix`

- [ ] **Step 2: Update CLAUDE.md if present**

Check if `CLAUDE.md` exists and has outdated sections. Update similarly.

- [ ] **Step 3: Commit**

```
git add AGENTS.md CLAUDE.md
git commit -m "Update agent guidelines for new architecture"
```

---

## Task 17: Final Build Verification

- [ ] **Step 1: Clean build**

```powershell
Remove-Item -Recurse -Force build
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON -DUSE_ASIO=ON
cmake --build build --config Release --parallel
```

Expected: builds cleanly with zero errors, zero warnings related to our changes.

- [ ] **Step 2: Run the standalone app**

```powershell
.\build\HachiTune_artefacts\Release\HachiTune.exe
```

Verify:
- App launches without crash
- Can load an audio file
- Piano roll displays correctly
- Can edit notes (pitch drag, tools)
- Can save/load project files

- [ ] **Step 3: Test backward compatibility**

Load an old `.hachi` project file (if available). Verify it loads correctly with the legacy pitchData format.

- [ ] **Step 4: Final commit**

```
git add -A
git commit -m "Complete project refactor: data model, processors, undo, cleanup"
```
