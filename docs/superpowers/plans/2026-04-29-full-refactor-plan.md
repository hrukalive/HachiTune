# Full Refactor Follow-Up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the refactor from the current `StretchAndDraw` HEAD by fixing the remaining data-model, serialization, warp, mel, synthesis, monitor, and plugin-cache gaps.

**Architecture:** Keep `Project` as the domain model with listener notifications. `AnalysisData` remains immutable analysis state, `EditedData` remains the editable per-frame source of truth, and `AudioData` remains runtime cache only. Move remaining algorithm behavior into pure processors and keep GUI handlers as interaction code.

**Tech Stack:** C++17, JUCE 8, CMake, JUCE JSON/var, JUCE DSP FFT/windowing, existing HachiTune model classes.

---

## Current Baseline

Base all work on the current workspace, not the old phase-one worktree.

Useful audit commands:

```powershell
git status --short --branch
git log --oneline --decorate --max-count=10
git diff --name-only 6b9b050b1147a8304cea79233766496c24d4b894..HEAD
```

If executing in a new worktree, initialize required submodules and copy
untracked runtime resources before configuring:

```powershell
git submodule update --init --recursive
Copy-Item -Recurse -Force C:\Users\Reon\Desktop\HachiTune\Resources .\Resources
Test-Path third_party\JUCE\CMakeLists.txt
Test-Path third_party\ARA_SDK\ARA_API
Test-Path Resources\models\pc_nsf_hifigan.onnx
```

Do not commit or delete unrelated untracked files already present in the main
workspace.

---

## File Map

Modify:

- `CMakeLists.txt`: add a core test executable and `ctest` entry.
- `Source/Tests/ProjectCoreTests.cpp`: new core tests.
- `Source/Models/Project.h`: add validation helpers and source/output mel cache
  comments in `AudioData`.
- `Source/Models/Project.cpp`: implement validation, frame helpers, direct
  audition-buffer range blending, and cache refresh fixes.
- `Source/Models/ProjectSerializer.h/.cpp`: align format v2 with target schema.
- `Source/Audio/Synthesis/StretchProcessor.h/.cpp`: add endpoint-aware mel
  builder and robust marker utilities.
- `Source/Utils/WarpMarkerProcessor.h/.cpp`: use endpoint-aware maps,
  `stretchEditedData()`, and source/output mel caches.
- `Source/Audio/TensionProcessor.h/.cpp`: return processed H/N slices and use
  JUCE windowing.
- `Source/Utils/HNSepCurveProcessor.h/.cpp`: rebuild source mel, then output mel.
- `Source/Audio/Synthesis/IncrementalSynthesizer.h/.cpp`: simplify range
  synthesis and audition-buffer blending.
- `Source/UI/Debug/ProjectTreeView.h/.cpp`: show validation and full layer
  details.
- `Source/UI/Debug/TreeValueMonitor.cpp`: refresh monitor sections for all
  relevant listener events.
- `Source/Plugin/PluginProcessor.h/.cpp`: own plugin project/runtime cache.
- `Source/Plugin/PluginEditor.cpp`: attach UI to processor-owned project.
- `Source/UI/IMainView.h`, `Source/UI/MainComponent.h`,
  `Source/UI/MainComponent.cpp`, `Source/UI/Main/MainComponent_ProjectIO.cpp`:
  bind UI to externally owned plugin project while preserving standalone mode.
- Comment-only cleanup in `Source/Utils/PitchCurveProcessor.h`,
  `Source/Utils/HNSepCurveProcessor.h`,
  `Source/UI/PianoRoll/PitchToolController.h`, and undo headers.

Do not reintroduce `AudioData` pitch/curve/mask fields.

---

### Task 1: Add Core Regression Tests

**Files:**
- Modify: `CMakeLists.txt`
- Create: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Add the test source file**

Create `Source/Tests/ProjectCoreTests.cpp`:

```cpp
#include "../JuceHeader.h"
#include "../Audio/Synthesis/StretchProcessor.h"
#include "../Models/Project.h"
#include "../Models/ProjectSerializer.h"
#include "../Utils/WarpMarkerProcessor.h"

#include <cmath>
#include <iostream>

namespace {
int failures = 0;

void expect(bool condition, const char* message)
{
  if (!condition)
  {
    ++failures;
    std::cerr << "FAIL: " << message << "\n";
  }
}

bool require(bool condition, const char* message)
{
  expect(condition, message);
  return condition;
}

bool hasProperty(const juce::var& object, const juce::Identifier& name)
{
  return object.isObject() &&
         !object.getProperty(name, juce::var()).isVoid();
}

void expectNear(float actual, float expected, float tolerance,
                const char* message)
{
  expect(std::isfinite(actual) &&
             std::isfinite(expected) &&
             std::abs(actual - expected) <= tolerance,
         message);
}

Project makeProject()
{
  Project project;
  project.setName("CoreTest");
  project.getAudioData().sampleRate = 44100;
  project.getAudioData().melSpectrogram = {
      {0.1f, 0.2f}, {0.2f, 0.3f}, {0.3f, 0.4f}, {0.4f, 0.5f}};

  Note note(0, 4, 60.0f);
  note.setSrcStartFrame(0);
  note.setSrcEndFrame(4);
  note.setOriginalDeltaPitch({0.0f, 0.1f, 0.2f, 0.3f});
  note.setVoicingCurve({100.0f, 90.0f, 80.0f, 70.0f});
  note.setBreathCurve({100.0f, 110.0f, 120.0f, 130.0f});
  note.setTensionCurve({0.0f, 1.0f, 2.0f, 3.0f});
  note.setHighPassFilterStrength(0.0f);
  note.setLowPassFilterStrength(0.0f);
  project.addNote(std::move(note));

  auto& analysis = project.getAnalysisData();
  analysis.originalF0 = {261.63f, 277.18f, 293.66f, 311.13f};
  analysis.originalPitch = {60.0f, 61.0f, 62.0f, 63.0f};
  analysis.originalDeltaPitch = {0.0f, 0.1f, 0.2f, 0.3f};
  analysis.originalVoicedMask = {true, true, true, true};
  analysis.originalVADMask = {true, true, true, false};
  analysis.noteSegments.push_back({0, 4});

  auto& edited = project.getEditedData();
  edited.basePitch = {60.0f, 60.0f, 60.0f, 60.0f};
  edited.deltaPitch = {0.0f, 1.0f, 2.0f, 3.0f};
  edited.f0 = {261.63f, 277.18f, 293.66f, 311.13f};
  edited.voicedMask = {true, true, true, false};
  edited.vadMask = {true, true, true, false};
  edited.voicingCurve = {100.0f, 100.0f, 100.0f, 100.0f};
  edited.breathCurve = {100.0f, 100.0f, 100.0f, 100.0f};
  edited.tensionCurve = {0.0f, 0.0f, 0.0f, 0.0f};

  project.refreshNoteCaches();
  return project;
}

void testSerializerOmitsNoteCaches()
{
  auto project = makeProject();
  project.setWarpMarkers({{2, 3}});
  const auto json = ProjectSerializer::toJson(project);
  const auto notes = json.getProperty("notes", juce::var());
  require(notes.isArray(), "notes is an array");
  require(notes.size() == 1, "one note serialized");

  const auto note = notes[0];
  expect(!hasProperty(note, "srcStartFrame"), "srcStartFrame is not saved");
  expect(!hasProperty(note, "srcEndFrame"), "srcEndFrame is not saved");
  expect(!hasProperty(note, "originalDeltaPitch"),
         "note originalDeltaPitch cache is not saved");
  expect(!hasProperty(note, "voicingCurve"),
         "note voicingCurve cache is not saved");
  expect(!hasProperty(note, "breathCurve"),
         "note breathCurve cache is not saved");
  expect(!hasProperty(note, "tensionCurve"),
         "note tensionCurve cache is not saved");
  expect(!hasProperty(note, "highPassFilterStrength"),
         "default highPassFilterStrength is not saved");
  expect(!hasProperty(note, "lowPassFilterStrength"),
         "default lowPassFilterStrength is not saved");

  expect(hasProperty(json, "analysisData"), "analysisData is saved");
  expect(hasProperty(json, "editedData"), "editedData is saved");
  expect(!hasProperty(json.getProperty("editedData", juce::var()),
                      "f0EditedMask"),
         "f0EditedMask remains absent");
}

void testValidation()
{
  auto project = makeProject();
  auto result = project.validateFrameData();
  expect(result.isValid(), "valid project passes validation");

  project.getEditedData().deltaPitch.pop_back();
  result = project.validateFrameData();
  expect(!result.isValid(), "mismatched editedData fails validation");
  expect(!result.messages.empty(), "validation reports messages");
}

void testStretchEditedData()
{
  EditedData data;
  data.basePitch = {60.0f, 62.0f, 64.0f};
  data.deltaPitch = {0.0f, 1.0f, 2.0f};
  data.f0 = {261.63f, 293.66f, 329.63f};
  data.voicedMask = {true, false, true};
  data.vadMask = {true, true, false};
  data.voicingCurve = {100.0f, 80.0f, 60.0f};
  data.breathCurve = {50.0f, 70.0f, 90.0f};
  data.tensionCurve = {0.0f, 10.0f, 20.0f};

  const std::vector<Project::WarpMarker> markers = {
      {0, 0}, {2, 4}};
  StretchProcessor::stretchEditedData(data, markers, 4);

  expect(data.basePitch.size() == 4, "stretched basePitch size");
  expect(data.deltaPitch.size() == 4, "stretched deltaPitch size");
  expect(data.f0.size() == 4, "stretched f0 size");
  expect(data.voicedMask.size() == 4, "stretched voiced size");
  expect(data.vadMask.size() == 4, "stretched vad size");
  expect(data.voicingCurve.size() == 4, "stretched voicing size");
  expect(data.breathCurve.size() == 4, "stretched breath size");
  expect(data.tensionCurve.size() == 4, "stretched tension size");
  expectNear(data.deltaPitch[1], 0.5f, 0.0001f,
             "deltaPitch uses linear interpolation");
  expect(data.basePitch[1] == 62.0f, "basePitch uses nearest interpolation");
}

void testWarpEndpoints()
{
  auto project = makeProject();
  const auto markers = WarpMarkerProcessor::buildWarpMapWithEndpoints(
      project, {{2, 3}});
  require(markers.size() == 3, "warp map includes endpoints");
  expect(markers.front().sourceFrame == 0, "warp starts at source 0");
  expect(markers.front().outputFrame == 0, "warp starts at output 0");
  expect(markers.back().sourceFrame == 4, "warp ends at source end");
  expect(markers.back().outputFrame == 4, "warp ends at output end");
}
} // namespace

int main()
{
  testSerializerOmitsNoteCaches();
  testValidation();
  testStretchEditedData();
  testWarpEndpoints();

  if (failures != 0)
  {
    std::cerr << failures << " test failure(s)\n";
    return 1;
  }

  std::cout << "All ProjectCoreTests passed\n";
  return 0;
}
```

- [ ] **Step 2: Add the CMake test target**

Add after `add_library(hachitune_ui STATIC ${HACHITUNE_UI_SOURCES})`:

```cmake
enable_testing()

add_executable(HachiTuneCoreTests
    Source/Tests/ProjectCoreTests.cpp)

target_link_libraries(HachiTuneCoreTests PRIVATE
    hachitune_core
    juce::juce_gui_basics
    juce::juce_dsp)

target_compile_features(HachiTuneCoreTests PRIVATE cxx_std_17)

add_test(NAME HachiTuneCoreTests COMMAND HachiTuneCoreTests)
```

- [ ] **Step 3: Run the expected failing test build**

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected before later tasks: compile fails on missing
`Project::validateFrameData()` and
`WarpMarkerProcessor::buildWarpMapWithEndpoints()`.

- [ ] **Step 4: Commit**

```powershell
git add CMakeLists.txt Source/Tests/ProjectCoreTests.cpp
git commit -m "test: add current refactor core tests"
```

---

### Task 2: Add Project Frame Helpers And Validation

**Files:**
- Modify: `Source/Models/Project.h`
- Modify: `Source/Models/Project.cpp`
- Test: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Add declarations to `Project.h`**

Add after `struct WarpMarker`:

```cpp
struct FrameDataValidation
{
  std::vector<juce::String> messages;
  bool isValid() const { return messages.empty(); }
};
```

Add public methods near `getAdjustedF0()`:

```cpp
int getFrameCount() const;
float getBaseF0ForFrame(int frame) const;
FrameDataValidation validateFrameData() const;
```

- [ ] **Step 2: Implement helpers in `Project.cpp`**

Add after `Project::Project()`:

```cpp
int Project::getFrameCount() const
{
  if (!editedData.f0.empty())
    return static_cast<int>(editedData.f0.size());
  if (!audioData.melSpectrogram.empty())
    return static_cast<int>(audioData.melSpectrogram.size());
  return audioData.getNumFrames();
}

float Project::getBaseF0ForFrame(int frame) const
{
  if (frame < 0 || frame >= static_cast<int>(editedData.basePitch.size()))
    return 0.0f;
  return midiToFreq(editedData.basePitch[static_cast<size_t>(frame)]);
}

Project::FrameDataValidation Project::validateFrameData() const
{
  FrameDataValidation result;
  const int editedFrames = editedData.getNumFrames();

  auto checkFloat = [&](const std::vector<float>& values,
                        const char* name,
                        bool required) {
    if (required && values.empty())
      result.messages.push_back(juce::String(name) + " is empty");
    if (!values.empty() && static_cast<int>(values.size()) != editedFrames)
      result.messages.push_back(juce::String(name) + " size mismatch");
  };

  auto checkBool = [&](const std::vector<bool>& values,
                       const char* name,
                       bool required) {
    if (required && values.empty())
      result.messages.push_back(juce::String(name) + " is empty");
    if (!values.empty() && static_cast<int>(values.size()) != editedFrames)
      result.messages.push_back(juce::String(name) + " size mismatch");
  };

  if (editedFrames > 0)
  {
    checkFloat(editedData.basePitch, "editedData.basePitch", true);
    checkFloat(editedData.deltaPitch, "editedData.deltaPitch", true);
    checkFloat(editedData.f0, "editedData.f0", true);
    checkBool(editedData.voicedMask, "editedData.voicedMask", true);
    checkBool(editedData.vadMask, "editedData.vadMask", true);
    checkFloat(editedData.voicingCurve, "editedData.voicingCurve", true);
    checkFloat(editedData.breathCurve, "editedData.breathCurve", true);
    checkFloat(editedData.tensionCurve, "editedData.tensionCurve", true);
  }

  const int analysisFrames = analysisData.getNumFrames();
  auto checkAnalysisFloat = [&](const std::vector<float>& values,
                                const char* name) {
    if (!values.empty() && static_cast<int>(values.size()) != analysisFrames)
      result.messages.push_back(juce::String(name) + " size mismatch");
  };
  auto checkAnalysisBool = [&](const std::vector<bool>& values,
                               const char* name) {
    if (!values.empty() && static_cast<int>(values.size()) != analysisFrames)
      result.messages.push_back(juce::String(name) + " size mismatch");
  };

  checkAnalysisFloat(analysisData.originalF0, "analysisData.originalF0");
  checkAnalysisFloat(analysisData.originalPitch, "analysisData.originalPitch");
  checkAnalysisFloat(analysisData.originalDeltaPitch,
                     "analysisData.originalDeltaPitch");
  checkAnalysisBool(analysisData.originalVoicedMask,
                    "analysisData.originalVoicedMask");
  checkAnalysisBool(analysisData.originalVADMask,
                    "analysisData.originalVADMask");

  const int projectFrames = getFrameCount();
  int nonRestNotes = 0;
  for (const auto& note : notes)
  {
    if (!note.isRest())
      ++nonRestNotes;

    if (note.getStartFrame() < 0 ||
        note.getEndFrame() <= note.getStartFrame() ||
        (projectFrames > 0 && note.getEndFrame() > projectFrames))
      result.messages.push_back("invalid note output range");

    if (!note.isRest() && analysisFrames > 0 &&
        (note.getSrcStartFrame() < 0 ||
         note.getSrcEndFrame() <= note.getSrcStartFrame() ||
         note.getSrcEndFrame() > analysisFrames))
      result.messages.push_back("invalid note source range");
  }

  if (!analysisData.noteSegments.empty() &&
      static_cast<int>(analysisData.noteSegments.size()) != nonRestNotes)
    result.messages.push_back("analysisData.noteSegments count mismatch");

  if (!audioData.melSpectrogram.empty())
  {
    const auto bins = audioData.melSpectrogram.front().size();
    for (const auto& row : audioData.melSpectrogram)
    {
      if (row.size() != bins)
      {
        result.messages.push_back("audioData.melSpectrogram ragged rows");
        break;
      }
    }
  }

  int prevSource = -1;
  int prevOutput = -1;
  for (const auto& marker : warpMarkers)
  {
    if (marker.sourceFrame <= prevSource ||
        marker.outputFrame <= prevOutput)
      result.messages.push_back("warpMarkers are not strictly increasing");
    prevSource = marker.sourceFrame;
    prevOutput = marker.outputFrame;
  }

  return result;
}
```

- [ ] **Step 3: Run tests**

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
ctest --test-dir build -C Debug --output-on-failure
```

Expected after Task 2: remaining failures are from serializer and warp endpoint
tests, not validation.

- [ ] **Step 4: Commit**

```powershell
git add Source/Models/Project.h Source/Models/Project.cpp
git commit -m "feat: add project frame validation"
```

---

### Task 3: Align Serializer With Format Version 2

**Files:**
- Modify: `Source/Models/ProjectSerializer.cpp`
- Modify: `Source/Models/ProjectSerializer.h`
- Test: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Keep `FORMAT_VERSION = 2`**

Confirm `Source/Models/ProjectSerializer.h` contains:

```cpp
static constexpr int FORMAT_VERSION = 2;
```

- [ ] **Step 2: Stop saving note-local caches**

In `ProjectSerializer::noteToJson()`, remove new-format writes for:

```cpp
srcStartFrame
srcEndFrame
originalDeltaPitch
voicingCurve
breathCurve
tensionCurve
```

Keep backward-compatible reads in `noteFromJson()`.

- [ ] **Step 3: Save default filter strengths only when non-zero**

Replace unconditional filter writes with:

```cpp
if (std::abs(note.getHighPassFilterStrength()) > 0.0001f)
  obj->setProperty("highPassFilterStrength",
                   note.getHighPassFilterStrength());
if (std::abs(note.getLowPassFilterStrength()) > 0.0001f)
  obj->setProperty("lowPassFilterStrength",
                   note.getLowPassFilterStrength());
```

- [ ] **Step 4: Restore source ranges from `analysisData.noteSegments`**

After loading notes and analysis data, add:

```cpp
int segIndex = 0;
for (auto& note : project.getNotes())
{
  if (note.isRest())
    continue;
  if (segIndex < static_cast<int>(project.getAnalysisData().noteSegments.size()))
  {
    const auto& seg =
        project.getAnalysisData().noteSegments[static_cast<size_t>(segIndex)];
    note.setSrcStartFrame(seg.srcStartFrame);
    note.setSrcEndFrame(seg.srcEndFrame);
  }
  ++segIndex;
}
```

Keep the existing legacy path that reads old `srcStartFrame`/`srcEndFrame` from
note JSON so older files still load.

- [ ] **Step 5: Save endpoint-inclusive warp markers**

Use `WarpMarkerProcessor::buildWarpMapWithEndpoints()` when serializing:

```cpp
const auto markersToSave =
    WarpMarkerProcessor::buildWarpMapWithEndpoints(
        project, project.getWarpMarkers());
for (const auto& marker : markersToSave)
{
  auto* markerObj = new juce::DynamicObject();
  markerObj->setProperty("sourceFrame", marker.sourceFrame);
  markerObj->setProperty("outputFrame", marker.outputFrame);
  warpMarkersArray.add(juce::var(markerObj));
}
```

- [ ] **Step 6: Run tests**

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
ctest --test-dir build -C Debug --output-on-failure
```

Expected after Task 3: serializer test passes; endpoint test still fails until
Task 4 if the helper is not implemented yet.

- [ ] **Step 7: Commit**

```powershell
git add Source/Models/ProjectSerializer.h Source/Models/ProjectSerializer.cpp Source/Tests/ProjectCoreTests.cpp
git commit -m "fix: align project serializer with v2 schema"
```

---

### Task 4: Make Warp Endpoint-Aware And Use StretchProcessor

**Files:**
- Modify: `Source/Utils/WarpMarkerProcessor.h`
- Modify: `Source/Utils/WarpMarkerProcessor.cpp`
- Modify: `Source/Audio/Synthesis/StretchProcessor.h`
- Modify: `Source/Audio/Synthesis/StretchProcessor.cpp`
- Test: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Add helper declaration**

In `Source/Utils/WarpMarkerProcessor.h`:

```cpp
std::vector<Project::WarpMarker> buildWarpMapWithEndpoints(
    const Project& project,
    const std::vector<Project::WarpMarker>& markers);
```

- [ ] **Step 2: Implement endpoint map**

In `WarpMarkerProcessor.cpp`, implement:

```cpp
std::vector<Project::WarpMarker> buildWarpMapWithEndpoints(
    const Project& project,
    const std::vector<Project::WarpMarker>& markers)
{
  const int sourceEnd = getSourceFrameLimit(project);
  if (sourceEnd <= 0)
    return {};

  auto result = normalizeMarkers(project, markers);
  result.insert(result.begin(), {0, 0});

  int outputEnd = sourceEnd;
  if (!result.empty())
  {
    const auto& lastInterior = result.back();
    if (lastInterior.sourceFrame > 0 && lastInterior.sourceFrame < sourceEnd)
    {
      const int remainingSource = sourceEnd - lastInterior.sourceFrame;
      outputEnd = lastInterior.outputFrame + remainingSource;
    }
  }

  if (result.back().sourceFrame != sourceEnd)
    result.push_back({sourceEnd, outputEnd});

  return result;
}
```

- [ ] **Step 3: Rewrite `recomputeFromMarkers()` ordering**

Inside `recomputeFromMarkers()`:

```cpp
const auto warpMap = buildWarpMapWithEndpoints(project, markers);
if (warpMap.size() < 2)
  return;

if (updateProjectMarkers)
  project.setWarpMarkers(warpMap);

auto& editedData = project.getEditedData();
const int newTotalFrames = warpMap.back().outputFrame;
StretchProcessor::stretchEditedData(editedData, warpMap, newTotalFrames);
StretchProcessor::remapNoteFrames(project.getNotes(), warpMap);
```

Then rebuild note caches and output mel. Do not call
`PitchCurveProcessor::rebuildBaseFromNotes()` as the primary stretch mechanism.

- [ ] **Step 4: Keep `StretchHandler` UI-only**

Verify `Source/UI/PianoRoll/States/StretchHandler.cpp` only calls
`WarpMarkerProcessor::recomputeFromMarkers()` and does not manipulate mel or
global pitch arrays directly.

- [ ] **Step 5: Run tests**

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
ctest --test-dir build -C Debug --output-on-failure
```

Expected after Task 4: endpoint and `stretchEditedData()` tests pass.

- [ ] **Step 6: Commit**

```powershell
git add Source/Utils/WarpMarkerProcessor.h Source/Utils/WarpMarkerProcessor.cpp Source/Audio/Synthesis/StretchProcessor.h Source/Audio/Synthesis/StretchProcessor.cpp
git commit -m "fix: make warp processing endpoint aware"
```

---

### Task 5: Split Source Mel And Output Mel Caches

**Files:**
- Modify: `Source/Models/Project.h`
- Modify: `Source/Audio/EditorController.cpp`
- Modify: `Source/UI/Main/MainComponent_ProjectIO.cpp`
- Modify: `Source/Utils/HNSepCurveProcessor.h`
- Modify: `Source/Utils/HNSepCurveProcessor.cpp`
- Modify: `Source/Utils/WarpMarkerProcessor.cpp`
- Modify: `Source/Audio/Synthesis/StretchProcessor.h`
- Modify: `Source/Audio/Synthesis/StretchProcessor.cpp`

- [ ] **Step 1: Add `sourceMelSpectrogram` to `AudioData`**

In `AudioData`:

```cpp
std::vector<std::vector<float>> sourceMelSpectrogram; // source timeline
std::vector<std::vector<float>> melSpectrogram;       // output timeline
```

- [ ] **Step 2: Initialize both caches during analysis and project load**

When computing mel from raw samples:

```cpp
audioData.sourceMelSpectrogram = melComputer.compute(samples, numSamples);
audioData.melSpectrogram = audioData.sourceMelSpectrogram;
```

Apply this in:

- `EditorController::analyzeAudio()`
- `MainComponent::openProjectFile()` when recomputing mel from audio

- [ ] **Step 3: Add output-mel builder**

In `StretchProcessor.h`:

```cpp
static std::vector<std::vector<float>> buildOutputMel(
    const std::vector<std::vector<float>>& sourceMel,
    const std::vector<Project::WarpMarker>& warpMap,
    int outputFrameCount);
```

In `StretchProcessor.cpp`:

```cpp
std::vector<std::vector<float>> StretchProcessor::buildOutputMel(
    const std::vector<std::vector<float>>& sourceMel,
    const std::vector<Project::WarpMarker>& warpMap,
    int outputFrameCount)
{
  if (sourceMel.empty() || outputFrameCount <= 0)
    return {};
  if (warpMap.size() < 2)
    return sourceMel;

  auto result = stretchMel(sourceMel, warpMap);
  result.resize(static_cast<size_t>(outputFrameCount),
                sourceMel.empty() ? std::vector<float>{}
                                  : std::vector<float>(sourceMel.front().size(), 0.0f));
  return result;
}
```

- [ ] **Step 4: Make warp rebuild output mel from source mel**

In `WarpMarkerProcessor::recomputeFromMarkers()`:

```cpp
if (!audioData.sourceMelSpectrogram.empty())
{
  audioData.melSpectrogram = StretchProcessor::buildOutputMel(
      audioData.sourceMelSpectrogram, warpMap, newTotalFrames);
}
```

Remove direct stretching of `audioData.melSpectrogram`.

- [ ] **Step 5: Make HNSep edits update source mel first**

In `HNSepCurveProcessor::recomputeMelForRange()`, write processed mel into
`audioData.sourceMelSpectrogram` using source-frame indices. After the source
write, rebuild output mel:

```cpp
const auto warpMap = WarpMarkerProcessor::buildWarpMapWithEndpoints(
    project, project.getWarpMarkers());
if (warpMap.size() >= 2)
{
  audioData.melSpectrogram = StretchProcessor::buildOutputMel(
      audioData.sourceMelSpectrogram, warpMap, project.getFrameCount());
}
else
{
  audioData.melSpectrogram = audioData.sourceMelSpectrogram;
}
```

- [ ] **Step 6: Run build**

```powershell
cmake --build build --config Debug --target HachiTune
```

Expected: build succeeds; if not, failures point to old assumptions that
`melSpectrogram` is both source and output.

- [ ] **Step 7: Commit**

```powershell
git add Source/Models/Project.h Source/Audio/EditorController.cpp Source/UI/Main/MainComponent_ProjectIO.cpp Source/Utils/HNSepCurveProcessor.h Source/Utils/HNSepCurveProcessor.cpp Source/Utils/WarpMarkerProcessor.cpp Source/Audio/Synthesis/StretchProcessor.h Source/Audio/Synthesis/StretchProcessor.cpp
git commit -m "fix: separate source and output mel caches"
```

---

### Task 6: Make TensionProcessor A Pure HN Processor

**Files:**
- Modify: `Source/Audio/TensionProcessor.h`
- Modify: `Source/Audio/TensionProcessor.cpp`
- Modify: `Source/Utils/HNSepCurveProcessor.cpp`
- Modify: `Source/Audio/EditorController.cpp`

- [ ] **Step 1: Add processed HN result type**

In `TensionProcessor.h`:

```cpp
struct ProcessedHN
{
  std::vector<float> harmonic;
  std::vector<float> noise;
};
```

Keep `TensionResult` only if a temporary adapter is required by existing code.

- [ ] **Step 2: Replace hand-built window table**

Use JUCE windowing:

```cpp
juce::dsp::WindowingFunction<float> window{
    static_cast<size_t>(kWinSize),
    juce::dsp::WindowingFunction<float>::hann,
    false};
```

Use `window.multiplyWithWindowingTable(fftBuf.data(), kWinSize)` for frame
windowing. Remove the manual cosine table.

- [ ] **Step 3: Return processed harmonic and noise separately**

Change the core function to:

```cpp
ProcessedHN processSegmentHN(const float* harmonicData,
                             const float* noiseData,
                             int numSamples,
                             const float* voicingCurve,
                             const float* breathCurve,
                             const float* tensionCurve,
                             int numFrames) const;
```

It should:

- scale harmonic by voicing;
- scale noise by breath;
- apply tension spectral tilt only to harmonic;
- return `{processedHarmonic, processedNoise}`;
- not compute mel;
- not write project state.

- [ ] **Step 4: Move STFT computation into TensionProcessor**

Move the local `computeSTFT` lambda from `EditorController::runHNSepSeparation()`
into:

```cpp
static std::vector<float> computeSTFT(const juce::AudioBuffer<float>& buffer);
```

Then call:

```cpp
proj.getHarmonicSTFT() = TensionProcessor::computeSTFT(audioData.harmonicWaveform);
proj.getNoiseSTFT() = TensionProcessor::computeSTFT(audioData.noiseWaveform);
```

- [ ] **Step 5: Compute mel in HNSepCurveProcessor**

Replace `tensionProc.processSegment()` usage with:

```cpp
auto hn = tensionProc.processSegmentHN(
    clipH.data(), clipN.data(), std::min(srcSamples, noiseSamples),
    srcVoicing.data(), srcBreath.data(), srcTension.data(),
    srcDurationFrames);

std::vector<float> mixed(hn.harmonic.size(), 0.0f);
for (size_t i = 0; i < mixed.size(); ++i)
  mixed[i] = hn.harmonic[i] + (i < hn.noise.size() ? hn.noise[i] : 0.0f);

auto srcMel = melComputer.compute(mixed.data(),
                                  static_cast<int>(mixed.size()));
```

- [ ] **Step 6: Build**

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
cmake --build build --config Debug --target HachiTune
```

- [ ] **Step 7: Commit**

```powershell
git add Source/Audio/TensionProcessor.h Source/Audio/TensionProcessor.cpp Source/Utils/HNSepCurveProcessor.cpp Source/Audio/EditorController.cpp
git commit -m "refactor: make tension processing pure"
```

---

### Task 7: Simplify Incremental Synthesis Blending

**Files:**
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.h`
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.cpp`
- Modify: `Source/Models/Project.h`
- Modify: `Source/Models/Project.cpp`

- [ ] **Step 1: Add direct audition-buffer blend helper**

In `Project.h`:

```cpp
void blendSynthesizedRangeIntoAuditionBuffer(
    const std::vector<float>& synthesized,
    int startFrame,
    int endFrame,
    int hopSize);
```

In `Project.cpp`:

```cpp
void Project::blendSynthesizedRangeIntoAuditionBuffer(
    const std::vector<float>& synthesized,
    int startFrame,
    int endFrame,
    int hopSize)
{
  if (synthesized.empty() || hopSize <= 0)
    return;
  if (auditionBuffer.getNumSamples() == 0)
    initAuditionBufferFromOriginal();
  if (auditionBuffer.getNumChannels() == 0)
    return;

  const int startSample = std::max(0, startFrame * hopSize);
  const int endSample = std::min(auditionBuffer.getNumSamples(),
                                 endFrame * hopSize);
  if (endSample <= startSample)
    return;

  const int numSamples = std::min(
      endSample - startSample, static_cast<int>(synthesized.size()));
  float* dst = auditionBuffer.getWritePointer(0, startSample);
  const int fade = std::min(hopSize, numSamples / 2);

  for (int i = 0; i < numSamples; ++i)
  {
    float mix = 1.0f;
    if (fade > 0 && i < fade)
      mix = static_cast<float>(i) / static_cast<float>(fade);
    if (fade > 0 && numSamples - 1 - i < fade)
      mix = std::min(mix, static_cast<float>(numSamples - 1 - i) /
                          static_cast<float>(fade));
    dst[i] = dst[i] + mix * (synthesized[static_cast<size_t>(i)] - dst[i]);
  }

  audioData.waveform.makeCopyOf(auditionBuffer);
}
```

- [ ] **Step 2: Use `computeResynthRange()`**

In `IncrementalSynthesizer::synthesizeRegion()`, replace the old
`computeSynthesisRange()` path with:

```cpp
const auto range = computeResynthRange();
int startFrame = range.startFrame;
int endFrame = range.endFrame;
```

Keep `range.needsMelUpdate` to decide whether to call
`HNSepCurveProcessor::recomputeMelForRange()`.

- [ ] **Step 3: Remove per-note synthWaveform write path from incremental flow**

After vocoder output returns, call:

```cpp
capturedProject->blendSynthesizedRangeIntoAuditionBuffer(
    synthesizedAudio, capturedStartFrame, capturedEndFrame, hopSize);
```

Then clear dirty state and notify `SynthesisComplete`.

Leave `composeGlobalWaveform()` in place for now as a compatibility fallback,
but incremental synthesis should not depend on per-note `synthWaveform`.

- [ ] **Step 4: Build**

```powershell
cmake --build build --config Debug --target HachiTune
```

- [ ] **Step 5: Commit**

```powershell
git add Source/Audio/Synthesis/IncrementalSynthesizer.h Source/Audio/Synthesis/IncrementalSynthesizer.cpp Source/Models/Project.h Source/Models/Project.cpp
git commit -m "refactor: blend incremental synthesis into audition buffer"
```

---

### Task 8: Upgrade TreeValueMonitor Diagnostics

**Files:**
- Modify: `Source/UI/Debug/ProjectTreeView.h`
- Modify: `Source/UI/Debug/ProjectTreeView.cpp`
- Modify: `Source/UI/Debug/TreeValueMonitor.cpp`

- [ ] **Step 1: Display validation**

In `ProjectTreeView::updatePropertyItems()`, add validation rows under
`EditedData`:

```cpp
const auto validation = project->validateFrameData();
setOrUpdate(editedCat, 0,
            "Valid: " + juce::String(validation.isValid() ? "yes" : "no"));
int row = 1;
for (const auto& message : validation.messages)
  setOrUpdate(editedCat, row++, "Validation: " + message);
```

Then append array size rows for every `EditedData` vector.

- [ ] **Step 2: Display analysis/audio cache dimensions**

Add rows for:

```text
analysis.originalF0
analysis.originalPitch
analysis.originalDeltaPitch
analysis.originalVoicedMask
analysis.originalVADMask
analysis.noteSegments
audio.sourceMelSpectrogram rows x cols
audio.melSpectrogram rows x cols
audio.harmonicWaveform samples
audio.noiseWaveform samples
project harmonicSTFT size
project noiseSTFT size
```

- [ ] **Step 3: Display note cache sizes**

In `NoteItem::rebuildSubItems()`, add:

```cpp
addSubItem(new PropertyItem("basePitch cache: " + juce::String(basePitchSize)));
addSubItem(new PropertyItem("deltaPitch cache: " + juce::String(deltaPitchSize)));
addSubItem(new PropertyItem("originalDelta cache: " + juce::String(originalDeltaSize)));
addSubItem(new PropertyItem("voicing cache: " + juce::String(voicingSize)));
addSubItem(new PropertyItem("breath cache: " + juce::String(breathSize)));
addSubItem(new PropertyItem("tension cache: " + juce::String(tensionSize)));
```

Store those sizes in `NoteItem::updateFrom()`.

- [ ] **Step 4: Refresh correctly for all data events**

In `TreeValueMonitor::onProjectChanged()`, call `treeView.refresh()` for:

```cpp
ProjectChangeType::WarpChanged
ProjectChangeType::EditedDataChanged
ProjectChangeType::AudioDataChanged
ProjectChangeType::SynthesisComplete
```

Keep note-only refresh for selection and simple property changes.

- [ ] **Step 5: Build**

```powershell
cmake --build build --config Debug --target HachiTune
```

- [ ] **Step 6: Commit**

```powershell
git add Source/UI/Debug/ProjectTreeView.h Source/UI/Debug/ProjectTreeView.cpp Source/UI/Debug/TreeValueMonitor.cpp
git commit -m "feat: expand project monitor diagnostics"
```

---

### Task 9: Make PluginProcessor Own The Runtime Project

**Files:**
- Modify: `Source/Plugin/PluginProcessor.h`
- Modify: `Source/Plugin/PluginProcessor.cpp`
- Modify: `Source/Plugin/PluginEditor.cpp`
- Modify: `Source/UI/IMainView.h`
- Modify: `Source/UI/MainComponent.h`
- Modify: `Source/UI/MainComponent.cpp`
- Modify: `Source/Audio/EditorController.h`
- Modify: `Source/Audio/EditorController.cpp`

- [ ] **Step 1: Add processor-owned project**

In `PluginProcessor.h`:

```cpp
std::shared_ptr<Project> pluginProject = std::make_shared<Project>();

std::shared_ptr<Project> getPluginProject() const { return pluginProject; }
```

- [ ] **Step 2: Let `EditorController` attach an external project**

In `EditorController.h`:

```cpp
void setExternalProject(std::shared_ptr<Project> externalProject);
std::shared_ptr<Project> getSharedProject() const { return projectShared; }
```

Change internal storage from only `std::unique_ptr<Project>` to:

```cpp
std::shared_ptr<Project> projectShared;
```

Update `getProject()`:

```cpp
Project* getProject() const { return projectShared.get(); }
```

In the constructor:

```cpp
projectShared = std::make_shared<Project>();
```

In `setExternalProject()`:

```cpp
projectShared = std::move(externalProject);
```

- [ ] **Step 3: Attach plugin UI to processor project**

In `PluginEditor.cpp`, after creating `mainView`, call:

```cpp
if (auto* main = dynamic_cast<MainComponent*>(mainView.get()))
  main->attachExternalProject(audioProcessor.getPluginProject());
```

If `MainComponent` is not visible as a concrete type at this include site, add
the method to `IMainView`:

```cpp
#include <memory>

virtual void attachExternalProject(std::shared_ptr<Project> project) = 0;
```

and implement it in `MainComponent`.

- [ ] **Step 4: Serialize from processor project when editor is closed**

In `PluginProcessor::getStateInformation()`, if `mainComponent` is null, use:

```cpp
if (pluginProject)
{
  auto projectState = ProjectSerializer::toJson(*pluginProject);
  envelope->setProperty("projectState", projectState);
}
```

In `setStateInformation()`, if editor is closed, load into `pluginProject`
instead of only `pendingStateJson`:

```cpp
if (projectState.isObject() && pluginProject)
  ProjectSerializer::fromJson(*pluginProject, projectState);
```

Keep `pendingStateJson` only for legacy UI restoration paths that cannot attach
immediately.

- [ ] **Step 5: Reopen GUI without analysis**

When the editor is created, it attaches to `pluginProject`, binds realtime
processor, refreshes UI components, and does not call `setHostAudio()` or
`analyzeAudio()` if `pluginProject->getAudioData().waveform` and
`pluginProject->getEditedData().f0` are already populated.

- [ ] **Step 6: Build plugin**

```powershell
cmake --build build --config Debug --target HachiTunePlugin
```

- [ ] **Step 7: Commit**

```powershell
git add Source/Plugin/PluginProcessor.h Source/Plugin/PluginProcessor.cpp Source/Plugin/PluginEditor.cpp Source/UI/IMainView.h Source/UI/MainComponent.h Source/UI/MainComponent.cpp Source/Audio/EditorController.h Source/Audio/EditorController.cpp
git commit -m "fix: keep plugin project cache in processor"
```

---

### Task 10: Remove Stale Comments And Dead Undo Placeholders

**Files:**
- Modify: `Source/Utils/PitchCurveProcessor.h`
- Modify: `Source/Utils/HNSepCurveProcessor.h`
- Modify: `Source/UI/PianoRoll/PitchToolController.h`
- Modify: `Source/UI/PianoRoll/States/DrawHandler.cpp`
- Modify: `Source/UI/PianoRoll/PitchEditor.cpp`
- Modify: `Source/Undo/F0Actions.h`

- [ ] **Step 1: Replace stale `audioData` comments**

Run:

```powershell
Get-ChildItem -Path Source -Recurse -File -Include *.h,*.cpp |
  Select-String -Pattern 'audioData\.(f0|baseF0|basePitch|deltaPitch|voicingCurve|breathCurve|tensionCurve|voicedMask|vadMask|f0EditedMask)' -CaseSensitive:$false
```

Expected current output is comments only. Rewrite them to `editedData`.

- [ ] **Step 2: Remove `F0DrawAction` edited-mask constructor parameters**

In `F0Actions.h`, remove:

```cpp
std::vector<bool> beforeEdited,
std::vector<bool> afterEdited,
```

and the corresponding member fields. Update call sites in `DrawHandler.cpp`
and `PitchEditor.cpp` to stop passing empty placeholder vectors.

- [ ] **Step 3: Run search again**

```powershell
Get-ChildItem -Path Source -Recurse -File -Include *.h,*.cpp |
  Select-String -Pattern 'f0EditedMask|audioData\.f0|audioData\.baseF0|audioData\.basePitch|audioData\.deltaPitch|audioData\.voicedMask|audioData\.vadMask|audioData\.voicingCurve|audioData\.breathCurve|audioData\.tensionCurve' -CaseSensitive:$false
```

Expected: no output.

- [ ] **Step 4: Build**

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
cmake --build build --config Debug --target HachiTune
```

- [ ] **Step 5: Commit**

```powershell
git add Source/Utils/PitchCurveProcessor.h Source/Utils/HNSepCurveProcessor.h Source/UI/PianoRoll/PitchToolController.h Source/UI/PianoRoll/States/DrawHandler.cpp Source/UI/PianoRoll/PitchEditor.cpp Source/Undo/F0Actions.h
git commit -m "chore: remove stale audioData pitch references"
```

---

## Final Verification

- [ ] **Step 1: Removed-field search**

```powershell
Get-ChildItem -Path Source -Recurse -File -Include *.h,*.cpp |
  Select-String -Pattern 'audioData\.(f0|baseF0|basePitch|deltaPitch|voicingCurve|breathCurve|tensionCurve|voicedMask|vadMask|f0EditedMask)' -CaseSensitive:$false
```

Expected: no output.

- [ ] **Step 2: Core tests**

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `HachiTuneCoreTests` passes.

- [ ] **Step 3: App build**

```powershell
cmake --build build --config Debug --target HachiTune
```

Expected: target builds.

- [ ] **Step 4: Plugin build**

```powershell
cmake --build build --config Debug --target HachiTunePlugin
```

Expected: target builds.

- [ ] **Step 5: Manual smoke checks**

Run standalone:

```powershell
.\build\HachiTune_artefacts\Debug\HachiTune.exe
```

Smoke checklist:

- load an audio file and analyze once;
- open TreeValueMonitor and confirm validation is valid;
- draw pitch and undo/redo;
- edit HN curve and confirm mel view updates;
- move a warp marker and confirm note caches and mel dimensions remain valid;
- save and reopen the project;
- confirm saved JSON has no note-local cache arrays;
- in plugin mode, close and reopen editor without reanalysis.
