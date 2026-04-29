# Project Data Model Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Remove editable pitch, curve, and mask fields from `AudioData`, make `AnalysisData`/`EditedData` the project data sources, migrate UI/Undo/synthesis callers, and add focused core tests.

**Architecture:** `AnalysisData` is the immutable reset baseline. `EditedData` is the only global editable per-frame state. `AudioData` keeps waveform, mel, HNSep waveform buffers, sample rate, segmentation debug, and incremental synthesis debug only.

**Tech Stack:** C++17, JUCE 8, CMake, existing `hachitune_core` static library, lightweight executable tests.

---

## File Structure

Create:

- `Source/Tests/ProjectDataModelTests.cpp`: standalone core test executable with simple assertions.

Modify:

- `CMakeLists.txt`: add `enable_testing()` and `HachiTuneCoreTests`.
- `Source/Models/EditedData.h`: add transient `f0EditedMask`; keep resize/clear consistent.
- `Source/Models/Project.h`: remove editable per-frame fields from `AudioData`; add frame validation and helper APIs.
- `Source/Models/Project.cpp`: use `EditedData` for adjusted F0, dirty-range bounds, note cache refresh, VAD rebuild destination, and validation.
- `Source/Models/ProjectSerializer.h/.cpp`: stop serializing note-local caches, remove `AudioData` from legacy pitch migration, load into `AnalysisData`/`EditedData`.
- `Source/Utils/PitchCurveProcessor.h/.cpp`: rewrite dense pitch operations to use `EditedData`.
- `Source/Utils/HNSepCurveProcessor.cpp`: rewrite curve storage to use `EditedData`; keep mel in `AudioData`.
- `Source/Utils/NoteEditUtils.cpp`: reset draw mask in `EditedData`.
- `Source/Undo/DragActions.h`, `Source/Undo/F0Actions.h`: restore snapshots into `EditedData`.
- `Source/Audio/EditorController.cpp`: use local analysis vectors and `EditedData` instead of removed `AudioData` fields.
- `Source/Audio/Synthesis/IncrementalSynthesizer.cpp`: read f0 and masks from `EditedData`.
- `Source/UI/MainComponent.cpp`, `Source/UI/PianoRollComponent.cpp`, `Source/UI/PianoRoll/States/DrawHandler.cpp`, `Source/UI/PianoRoll/States/SelectHandler.cpp`, `Source/UI/PianoRoll/States/StretchHandler.cpp`, `Source/UI/PianoRoll/NoteSplitter.cpp`, `Source/UI/PianoRoll/PitchToolController.cpp`: migrate UI reads/writes to `EditedData`.
- `Source/UI/Debug/ProjectTreeView.cpp`: show validation and `EditedData` lengths.

---

### Task 1: Add Failing Core Tests

**Files:**
- Create: `Source/Tests/ProjectDataModelTests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the test target to CMake**

Add this block after `add_library(hachitune_ui STATIC ${HACHITUNE_UI_SOURCES})` and before target link options:

```cmake
enable_testing()

add_executable(HachiTuneCoreTests
    Source/Tests/ProjectDataModelTests.cpp)

target_link_libraries(HachiTuneCoreTests PRIVATE
    hachitune_core
    juce::juce_core
    juce::juce_data_structures
    juce::juce_audio_basics)

add_test(NAME HachiTuneCoreTests COMMAND HachiTuneCoreTests)
```

- [ ] **Step 2: Add the failing tests**

Create `Source/Tests/ProjectDataModelTests.cpp`:

```cpp
#include "../JuceHeader.h"
#include "../Audio/Synthesis/StretchProcessor.h"
#include "../Models/Project.h"
#include "../Models/ProjectSerializer.h"
#include "../Utils/Constants.h"

#include <cmath>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

void expect(bool condition, const char* message)
{
  if (!condition)
  {
    ++failures;
    std::cerr << "FAIL: " << message << "\n";
  }
}

void expectNear(float actual, float expected, float tolerance,
                const char* message)
{
  if (std::abs(actual - expected) > tolerance)
  {
    ++failures;
    std::cerr << "FAIL: " << message << " actual=" << actual
              << " expected=" << expected << "\n";
  }
}

bool hasProperty(const juce::var& object, const juce::Identifier& name)
{
  return object.isObject() && !object.getProperty(name, juce::var()).isVoid();
}

Project makeProjectWithFrameData()
{
  Project project;
  project.setName("SerializerTest");
  project.getAudioData().sampleRate = 44100;
  project.getAudioData().melSpectrogram = {
      {0.1f, 0.2f}, {0.2f, 0.3f}, {0.3f, 0.4f}, {0.4f, 0.5f}};

  Note note(0, 4, 60.0f);
  note.setSrcStartFrame(10);
  note.setSrcEndFrame(14);
  note.setPitchOffset(1.0f);
  note.setOriginalDeltaPitch({0.1f, 0.2f, 0.3f, 0.4f});
  note.setDeltaPitch({0.2f, 0.2f, 0.2f, 0.2f});
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
  analysis.noteSegments.push_back({10, 14});

  auto& edited = project.getEditedData();
  edited.basePitch = {60.0f, 60.0f, 60.0f, 60.0f};
  edited.deltaPitch = {0.0f, 1.0f, 2.0f, 3.0f};
  edited.f0 = {261.63f, 277.18f, 293.66f, 311.13f};
  edited.voicedMask = {true, true, true, false};
  edited.vadMask = {true, true, true, false};
  edited.voicingCurve = {100.0f, 100.0f, 100.0f, 100.0f};
  edited.breathCurve = {100.0f, 100.0f, 100.0f, 100.0f};
  edited.tensionCurve = {0.0f, 0.0f, 0.0f, 0.0f};
  edited.f0EditedMask = {false, false, false, false};
  project.refreshNoteCaches();
  return project;
}

void testSerializerOmitsNoteCaches()
{
  auto project = makeProjectWithFrameData();
  const auto json = ProjectSerializer::toJson(project);
  const auto notes = json.getProperty("notes", juce::var());

  expect(notes.isArray(), "notes is an array");
  expect(notes.size() == 1, "one note serialized");
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
         "zero highPassFilterStrength is not saved");
  expect(!hasProperty(note, "lowPassFilterStrength"),
         "zero lowPassFilterStrength is not saved");

  expect(hasProperty(json, "analysisData"), "analysisData is saved");
  expect(hasProperty(json, "editedData"), "editedData is saved");
}

void testLoadRefreshesNoteCaches()
{
  auto project = makeProjectWithFrameData();
  const auto json = ProjectSerializer::toJson(project);

  Project loaded;
  const bool ok = ProjectSerializer::fromJson(loaded, json);
  expect(ok, "new schema loads");
  expect(loaded.getEditedData().getNumFrames() == 4,
         "editedData frame count loaded");
  expect(loaded.getNotes().size() == 1, "note count loaded");

  const auto& note = loaded.getNotes().front();
  expect(note.getSrcStartFrame() == 10, "source start restored from analysis");
  expect(note.getSrcEndFrame() == 14, "source end restored from analysis");
  expect(note.getBasePitch().size() == 4, "note basePitch cache refreshed");
  expect(note.getOriginalPitch().size() == 4,
         "note originalPitch cache refreshed");
  expectNear(note.getBasePitch()[1], 60.0f, 0.0001f,
             "note basePitch cache value");
  expectNear(note.getOriginalPitch()[2], 62.0f, 0.0001f,
             "note originalPitch cache value");
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

  std::vector<Project::WarpMarker> markers = {{0, 0}, {2, 4}};
  StretchProcessor::stretchEditedData(data, markers, 4);

  expect(data.basePitch.size() == 4, "stretched basePitch size");
  expect(data.deltaPitch.size() == 4, "stretched deltaPitch size");
  expect(data.voicedMask.size() == 4, "stretched voiced mask size");
  expectNear(data.basePitch[1], 62.0f, 0.0001f,
             "basePitch uses nearest interpolation");
  expectNear(data.deltaPitch[1], 0.5f, 0.0001f,
             "deltaPitch uses linear interpolation");
  expectNear(data.tensionCurve[2], 10.0f, 0.0001f,
             "tension uses linear interpolation");
  expectNear(data.f0[2], 440.0f * std::pow(2.0f, (63.0f - 69.0f) / 12.0f),
             0.001f, "f0 recomputed from base + delta");
}

void testValidateFrameData()
{
  auto project = makeProjectWithFrameData();
  auto result = project.validateFrameData();
  expect(result.isValid(), "valid project passes validation");

  project.getEditedData().deltaPitch.pop_back();
  result = project.validateFrameData();
  expect(!result.isValid(), "mismatched editedData fails validation");
  expect(result.messages.size() > 0, "validation reports messages");
}
} // namespace

int main()
{
  testSerializerOmitsNoteCaches();
  testLoadRefreshesNoteCaches();
  testStretchEditedData();
  testValidateFrameData();

  if (failures != 0)
  {
    std::cerr << failures << " test failure(s)\n";
    return 1;
  }

  std::cout << "All ProjectDataModelTests passed\n";
  return 0;
}
```

- [ ] **Step 3: Configure the existing build tree**

Run:

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Debug
```

Expected: configure completes. If CMake tries to download dependencies and fails because of sandbox/network permissions, rerun the same command with escalation.

- [ ] **Step 4: Run the tests and confirm RED**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: build fails because `Project::validateFrameData()` does not exist and serializer still emits note-local cache fields. This is the intentional RED state.

- [ ] **Step 5: Commit the failing tests**

```powershell
git add CMakeLists.txt Source/Tests/ProjectDataModelTests.cpp
git commit -m "test: add project data model tests"
```

---

### Task 2: Move Transient Edit Mask Into EditedData

**Files:**
- Modify: `Source/Models/EditedData.h`

- [ ] **Step 1: Update the failing test if it does not mention f0EditedMask**

Confirm `Source/Tests/ProjectDataModelTests.cpp` contains:

```cpp
edited.f0EditedMask = {false, false, false, false};
```

Expected: this fails to compile until `EditedData::f0EditedMask` is added.

- [ ] **Step 2: Add `f0EditedMask` to `EditedData`**

In `Source/Models/EditedData.h`, add the field after `tensionCurve`:

```cpp
std::vector<bool> f0EditedMask;  // [T] true = hand-drawn destructive pitch edit
```

Update `clear()`:

```cpp
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
  f0EditedMask.clear();
}
```

Update `resize(int numFrames)`:

```cpp
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
  f0EditedMask.resize(n, false);
}
```

- [ ] **Step 3: Build tests and confirm next failure**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: build still fails on missing `validateFrameData()` and/or serializer assertions once it compiles far enough.

- [ ] **Step 4: Commit**

```powershell
git add Source/Models/EditedData.h
git commit -m "feat: add edited f0 mask storage"
```

---

### Task 3: Add Project Frame Helpers and Validation

**Files:**
- Modify: `Source/Models/Project.h`
- Modify: `Source/Models/Project.cpp`

- [ ] **Step 1: Ensure tests require validation**

Confirm `testValidateFrameData()` calls:

```cpp
auto result = project.validateFrameData();
expect(result.isValid(), "valid project passes validation");
```

Expected: tests fail to compile because `validateFrameData` is missing.

- [ ] **Step 2: Add declarations to `Project.h`**

Add this nested struct inside `class Project` public section after `WarpMarker`:

```cpp
struct FrameDataValidation
{
  bool editedDataConsistent = true;
  bool analysisDataConsistent = true;
  bool melLengthConsistent = true;
  bool noteCachesConsistent = true;
  juce::StringArray messages;

  bool isValid() const
  {
    return editedDataConsistent && analysisDataConsistent &&
           melLengthConsistent && noteCachesConsistent;
  }
};
```

Add these public methods near the data accessors:

```cpp
int getFrameCount() const;
float getBaseF0ForFrame(int frame) const;
FrameDataValidation validateFrameData() const;
```

- [ ] **Step 3: Implement helpers in `Project.cpp`**

Add after `Project::Project()`:

```cpp
int Project::getFrameCount() const
{
  const int editedFrames = editedData.getNumFrames();
  if (editedFrames > 0)
    return editedFrames;
  return static_cast<int>(audioData.melSpectrogram.size());
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
  auto checkFloatSize = [&](const std::vector<float>& values,
                            const char* name,
                            bool required) {
    if (required && static_cast<int>(values.size()) != editedFrames)
    {
      result.editedDataConsistent = false;
      result.messages.add(juce::String(name) + " size " +
                          juce::String(static_cast<int>(values.size())) +
                          " != edited frame count " +
                          juce::String(editedFrames));
    }
  };
  auto checkBoolSize = [&](const std::vector<bool>& values,
                           const char* name,
                           bool required) {
    if (required && static_cast<int>(values.size()) != editedFrames)
    {
      result.editedDataConsistent = false;
      result.messages.add(juce::String(name) + " size " +
                          juce::String(static_cast<int>(values.size())) +
                          " != edited frame count " +
                          juce::String(editedFrames));
    }
  };

  if (editedFrames > 0)
  {
    checkFloatSize(editedData.basePitch, "editedData.basePitch", true);
    checkFloatSize(editedData.deltaPitch, "editedData.deltaPitch", true);
    checkBoolSize(editedData.voicedMask, "editedData.voicedMask", true);
    checkBoolSize(editedData.vadMask, "editedData.vadMask", true);
    checkFloatSize(editedData.voicingCurve, "editedData.voicingCurve", true);
    checkFloatSize(editedData.breathCurve, "editedData.breathCurve", true);
    checkFloatSize(editedData.tensionCurve, "editedData.tensionCurve", true);
    checkBoolSize(editedData.f0EditedMask, "editedData.f0EditedMask", false);
  }

  const int analysisFrames = analysisData.getNumFrames();
  auto checkAnalysisFloatSize = [&](const std::vector<float>& values,
                                    const char* name) {
    if (!values.empty() && static_cast<int>(values.size()) != analysisFrames)
    {
      result.analysisDataConsistent = false;
      result.messages.add(juce::String(name) + " size " +
                          juce::String(static_cast<int>(values.size())) +
                          " != analysis frame count " +
                          juce::String(analysisFrames));
    }
  };
  auto checkAnalysisBoolSize = [&](const std::vector<bool>& values,
                                   const char* name) {
    if (!values.empty() && static_cast<int>(values.size()) != analysisFrames)
    {
      result.analysisDataConsistent = false;
      result.messages.add(juce::String(name) + " size " +
                          juce::String(static_cast<int>(values.size())) +
                          " != analysis frame count " +
                          juce::String(analysisFrames));
    }
  };

  if (analysisFrames > 0)
  {
    checkAnalysisFloatSize(analysisData.originalPitch,
                           "analysisData.originalPitch");
    checkAnalysisFloatSize(analysisData.originalDeltaPitch,
                           "analysisData.originalDeltaPitch");
    checkAnalysisBoolSize(analysisData.originalVoicedMask,
                          "analysisData.originalVoicedMask");
    checkAnalysisBoolSize(analysisData.originalVADMask,
                          "analysisData.originalVADMask");
  }

  if (!audioData.melSpectrogram.empty() &&
      editedFrames > 0 &&
      static_cast<int>(audioData.melSpectrogram.size()) != editedFrames)
  {
    result.melLengthConsistent = false;
    result.messages.add("audioData.melSpectrogram length " +
                        juce::String(static_cast<int>(
                            audioData.melSpectrogram.size())) +
                        " != edited frame count " +
                        juce::String(editedFrames));
  }

  for (int i = 0; i < static_cast<int>(notes.size()); ++i)
  {
    const auto& note = notes[static_cast<size_t>(i)];
    if (note.isRest())
      continue;
    const int len = note.getDurationFrames();
    if ((note.hasBasePitch() &&
         static_cast<int>(note.getBasePitch().size()) != len) ||
        (note.hasDeltaPitch() &&
         static_cast<int>(note.getDeltaPitch().size()) != len))
    {
      result.noteCachesConsistent = false;
      result.messages.add("note " + juce::String(i) +
                          " pitch cache length != note duration");
    }
  }

  return result;
}
```

- [ ] **Step 4: Run tests and confirm validation test can pass later**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: build reaches later failures in serializer or `AudioData` field removal. If tests run, `testValidateFrameData` should pass.

- [ ] **Step 5: Commit**

```powershell
git add Source/Models/Project.h Source/Models/Project.cpp
git commit -m "feat: add project frame validation"
```

---

### Task 4: Remove Editable Fields From AudioData

**Files:**
- Modify: `Source/Models/Project.h`

- [ ] **Step 1: Confirm RED compile pressure**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected before this task: compile still references `audioData.f0`, `audioData.basePitch`, or related fields.

- [ ] **Step 2: Remove fields from `AudioData`**

In `Source/Models/Project.h`, delete this block from `struct AudioData`:

```cpp
std::vector<float> f0;
std::vector<float> baseF0;
std::vector<float> basePitch;
std::vector<float> deltaPitch;
std::vector<float> voicingCurve;
std::vector<float> breathCurve;
std::vector<float> tensionCurve;
std::vector<bool> voicedMask;
std::vector<bool> vadMask;
std::vector<bool> f0EditedMask;
```

Keep `melSpectrogram`, `segmentChunkRanges`, `segmentDebugChunks`, and `incrementalDebug`.

- [ ] **Step 3: Build and capture the caller migration list**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: many compile errors. This is the intentional call-site discovery point.

- [ ] **Step 4: Commit the field removal**

```powershell
git add Source/Models/Project.h
git commit -m "refactor: remove editable fields from audio data"
```

---

### Task 5: Update ProjectSerializer For New Data Sources

**Files:**
- Modify: `Source/Models/ProjectSerializer.h`
- Modify: `Source/Models/ProjectSerializer.cpp`

- [ ] **Step 1: Run serializer tests and confirm RED**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: serializer test fails because note-local fields are still serialized and legacy load references removed `AudioData` fields.

- [ ] **Step 2: Change the legacy helper signature**

In `ProjectSerializer.h`, replace:

```cpp
static bool legacyPitchDataFromJson(AudioData& audioData,
                                    EditedData& editedData,
                                    AnalysisData& analysisData,
                                    const juce::var& json);
```

with:

```cpp
static bool legacyPitchDataFromJson(EditedData& editedData,
                                    AnalysisData& analysisData,
                                    const juce::var& json);
```

- [ ] **Step 3: Stop writing note-local caches**

In `ProjectSerializer::noteToJson`, remove these writes:

```cpp
if (note.hasOriginalDeltaPitch())
    obj->setProperty("originalDeltaPitch", floatArrayToString(note.getOriginalDeltaPitch(), 4));
if (note.hasVoicingCurve())
    obj->setProperty("voicingCurve", floatArrayToString(note.getVoicingCurve(), 2));
if (note.hasBreathCurve())
    obj->setProperty("breathCurve", floatArrayToString(note.getBreathCurve(), 2));
if (note.hasTensionCurve())
    obj->setProperty("tensionCurve", floatArrayToString(note.getTensionCurve(), 2));
```

Keep non-zero filter strength writes only:

```cpp
if (std::abs(note.getHighPassFilterStrength()) > 0.0001f)
    obj->setProperty("highPassFilterStrength", note.getHighPassFilterStrength());
if (std::abs(note.getLowPassFilterStrength()) > 0.0001f)
    obj->setProperty("lowPassFilterStrength", note.getLowPassFilterStrength());
```

- [ ] **Step 4: Load source segments from analysis data**

In `ProjectSerializer::fromJson`, after `analysisDataFromJson` and `editedDataFromJson`, remove all assignments to `audioData.f0`, `audioData.basePitch`, `audioData.deltaPitch`, masks, and curves.

Add this after notes and data objects are loaded:

```cpp
for (int i = 0; i < static_cast<int>(project.getNotes().size()); ++i)
{
    auto& note = project.getNotes()[static_cast<size_t>(i)];
    if (i < static_cast<int>(project.getAnalysisData().noteSegments.size()))
    {
        const auto& seg = project.getAnalysisData().noteSegments[static_cast<size_t>(i)];
        note.setSrcStartFrame(seg.srcStartFrame);
        note.setSrcEndFrame(seg.srcEndFrame);
    }
    else
    {
        note.setSrcStartFrame(note.getStartFrame());
        note.setSrcEndFrame(note.getEndFrame());
    }
}

if (project.getEditedData().f0EditedMask.size() !=
    project.getEditedData().f0.size())
{
    project.getEditedData().f0EditedMask.assign(
        project.getEditedData().f0.size(), false);
}

project.refreshNoteCaches();
```

- [ ] **Step 5: Rewrite legacy pitch loading**

Change `legacyPitchDataFromJson` implementation to:

```cpp
bool ProjectSerializer::legacyPitchDataFromJson(EditedData& editedData,
                                                 AnalysisData& analysisData,
                                                 const juce::var& json)
{
    if (!json.isObject())
        return false;

    editedData.f0 = stringToFloatArray(json.getProperty("f0", "").toString());
    editedData.basePitch = stringToFloatArray(json.getProperty("basePitch", "").toString());
    editedData.deltaPitch = stringToFloatArray(json.getProperty("deltaPitch", "").toString());
    editedData.voicingCurve = stringToFloatArray(json.getProperty("voicingCurve", "").toString());
    editedData.breathCurve = stringToFloatArray(json.getProperty("breathCurve", "").toString());
    editedData.tensionCurve = stringToFloatArray(json.getProperty("tensionCurve", "").toString());
    editedData.voicedMask = stringToBoolArray(json.getProperty("voicedMask", "").toString());
    editedData.vadMask = stringToBoolArray(json.getProperty("vadMask", "").toString());
    editedData.f0EditedMask = stringToBoolArray(json.getProperty("f0EditedMask", "").toString());
    if (editedData.f0EditedMask.size() != editedData.f0.size())
        editedData.f0EditedMask.assign(editedData.f0.size(), false);

    analysisData.originalF0 = editedData.f0;
    analysisData.originalPitch = editedData.basePitch;
    analysisData.originalDeltaPitch = editedData.deltaPitch;
    analysisData.originalVoicedMask = editedData.voicedMask;
    analysisData.originalVADMask = editedData.vadMask;

    return true;
}
```

Update the call site:

```cpp
legacyPitchDataFromJson(project.getEditedData(),
                        project.getAnalysisData(),
                        pitchDataVar);
```

- [ ] **Step 6: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
ctest --test-dir build -C Debug --output-on-failure
```

Expected: serializer tests pass or fail only because other migrated code still does not compile.

- [ ] **Step 7: Commit**

```powershell
git add Source/Models/ProjectSerializer.h Source/Models/ProjectSerializer.cpp
git commit -m "refactor: serialize edited data as project source"
```

---

### Task 6: Migrate PitchCurveProcessor To EditedData

**Files:**
- Modify: `Source/Utils/PitchCurveProcessor.h`
- Modify: `Source/Utils/PitchCurveProcessor.cpp`

- [ ] **Step 1: Confirm RED**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: errors reference removed `audioData.basePitch`, `audioData.deltaPitch`, `audioData.f0`, `audioData.baseF0`, or `audioData.f0EditedMask` in `PitchCurveProcessor.cpp`.

- [ ] **Step 2: Update header comments**

In `PitchCurveProcessor.h`, replace references to `audioData.f0` with `editedData.f0`:

```cpp
/**
 * Rebuild base pitch (midi) from current notes and keep existing delta.
 * Ensures base/delta are dense and aligned to the project frame count,
 * then composes editedData.f0 (without applying the uv mask).
 */
void rebuildBaseFromNotes(Project& project);
```

and:

```cpp
/**
 * Convenience to update editedData.f0 in-place using composeF0.
 */
void composeF0InPlace(Project& project,
                      bool applyUvMask,
                      float globalPitchOffset = 0.0f);
```

- [ ] **Step 3: Change sizing helper**

Replace `ensureSizes(AudioData& audioData, int totalFrames)` with:

```cpp
void ensureSizes(EditedData& editedData, int totalFrames)
{
    if (totalFrames <= 0)
        return;

    if (editedData.basePitch.size() != static_cast<size_t>(totalFrames))
        editedData.basePitch.assign(static_cast<size_t>(totalFrames), 0.0f);
    if (editedData.deltaPitch.size() != static_cast<size_t>(totalFrames))
        editedData.deltaPitch.assign(static_cast<size_t>(totalFrames), 0.0f);
    if (editedData.f0.size() != static_cast<size_t>(totalFrames))
        editedData.f0.assign(static_cast<size_t>(totalFrames), 0.0f);
    if (editedData.voicedMask.size() != static_cast<size_t>(totalFrames))
        editedData.voicedMask.assign(static_cast<size_t>(totalFrames), true);
    if (editedData.f0EditedMask.size() != static_cast<size_t>(totalFrames))
        editedData.f0EditedMask.assign(static_cast<size_t>(totalFrames), false);
}
```

- [ ] **Step 4: Rewrite dense pitch functions**

Use these substitutions throughout `PitchCurveProcessor.cpp`:

```cpp
auto& editedData = project.getEditedData();
const auto& editedData = project.getEditedData();
const int totalFrames = project.getFrameCount();
```

Replace every old `audioData.basePitch`, `audioData.deltaPitch`, `audioData.f0`, `audioData.voicedMask`, and `audioData.f0EditedMask` access with the corresponding `editedData` field.

Remove `baseF0` cache writes entirely. For preview code that needs base Hz, use `midiToFreq(editedData.basePitch[index])`.

`composeF0` must become:

```cpp
std::vector<float> composeF0(const Project& project,
                             bool applyUvMask,
                             float globalPitchOffset)
{
    const auto& editedData = project.getEditedData();
    const int totalFrames = static_cast<int>(editedData.basePitch.size());
    std::vector<float> result(static_cast<size_t>(totalFrames), 0.0f);

    for (int i = 0; i < totalFrames; ++i)
    {
        const bool isVoiced =
            (i < static_cast<int>(editedData.voicedMask.size()))
                ? editedData.voicedMask[static_cast<size_t>(i)]
                : true;
        if (applyUvMask && !isVoiced)
            continue;

        const float base = editedData.basePitch[static_cast<size_t>(i)];
        const float delta =
            (i < static_cast<int>(editedData.deltaPitch.size()))
                ? editedData.deltaPitch[static_cast<size_t>(i)]
                : 0.0f;
        result[static_cast<size_t>(i)] =
            midiToFreq(base + delta + globalPitchOffset);
    }

    return result;
}
```

`composeF0InPlace` must become:

```cpp
void composeF0InPlace(Project& project,
                      bool applyUvMask,
                      float globalPitchOffset)
{
    auto composed = composeF0(project, applyUvMask, globalPitchOffset);
    project.getEditedData().f0 = std::move(composed);
    project.notifyListeners(ProjectChangeType::EditedDataChanged);
}
```

- [ ] **Step 5: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: `PitchCurveProcessor` no longer references removed `AudioData` fields.

- [ ] **Step 6: Commit**

```powershell
git add Source/Utils/PitchCurveProcessor.h Source/Utils/PitchCurveProcessor.cpp
git commit -m "refactor: move pitch curve processor to edited data"
```

---

### Task 7: Migrate Project Methods and Warp Helpers To EditedData

**Files:**
- Modify: `Source/Models/Project.cpp`

- [ ] **Step 1: Confirm RED**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: errors in `Project.cpp` reference removed `AudioData` fields.

- [ ] **Step 2: Rewrite adjusted F0 methods**

In `Project::getAdjustedF0()` and `Project::getAdjustedF0ForRange()`, replace `audioData.basePitch`, `audioData.deltaPitch`, and `audioData.voicedMask` with `editedData.basePitch`, `editedData.deltaPitch`, and `editedData.voicedMask`.

The guard should be:

```cpp
if (editedData.basePitch.empty() || editedData.deltaPitch.empty())
    return {};
```

The range clamp should be:

```cpp
endFrame = std::min(endFrame, static_cast<int>(editedData.basePitch.size()));
```

- [ ] **Step 3: Rewrite waveform VAD rebuild**

Change the helper currently named `rebuildVadMaskFromWaveform(AudioData& audioData)` to:

```cpp
void rebuildEditedVadMaskFromWaveform(AudioData& audioData,
                                      EditedData& editedData)
{
    constexpr float kVadThreshold = 0.008f;

    const int numFrames = static_cast<int>(audioData.melSpectrogram.size());
    editedData.vadMask.assign(static_cast<size_t>(numFrames), false);
    if (numFrames <= 0 || audioData.waveform.getNumSamples() <= 0)
        return;

    const float* samples = audioData.waveform.getReadPointer(0);
    const int numSamples = audioData.waveform.getNumSamples();
    for (int frame = 0; frame < numFrames; ++frame)
    {
        const int sampleStart = frame * HOP_SIZE;
        const int sampleEnd = std::min(sampleStart + HOP_SIZE, numSamples);
        if (sampleStart >= numSamples || sampleEnd <= sampleStart)
            continue;

        double sumSq = 0.0;
        for (int sample = sampleStart; sample < sampleEnd; ++sample)
        {
            const double value = samples[sample];
            sumSq += value * value;
        }

        const float rms = static_cast<float>(
            std::sqrt(sumSq /
                      static_cast<double>(sampleEnd - sampleStart)));
        editedData.vadMask[static_cast<size_t>(frame)] =
            rms > kVadThreshold;
    }
}
```

Update the call in `WarpMarkerProcessor::recomputeFromMarkers()`:

```cpp
rebuildEditedVadMaskFromWaveform(audioData, project.getEditedData());
```

- [ ] **Step 4: Keep warp edited state global**

In `WarpMarkerProcessor::recomputeFromMarkers()`, replace:

```cpp
audioData.voicedMask = std::move(newVoiced);
```

with:

```cpp
project.getEditedData().voicedMask = std::move(newVoiced);
```

Replace `buildSourceVoicedMask(audioData.f0, ...)` with:

```cpp
buildSourceVoicedMask(project.getAnalysisData().originalF0,
                      note.getSrcStartFrame(),
                      note.getSrcEndFrame())
```

- [ ] **Step 5: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: `Project.cpp` no longer references removed `AudioData` fields.

- [ ] **Step 6: Commit**

```powershell
git add Source/Models/Project.cpp
git commit -m "refactor: move project pitch state to edited data"
```

---

### Task 8: Migrate HNSepCurveProcessor To EditedData

**Files:**
- Modify: `Source/Utils/HNSepCurveProcessor.cpp`

- [ ] **Step 1: Confirm RED**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: errors in `HNSepCurveProcessor.cpp` reference removed `audioData.voicingCurve`, `audioData.breathCurve`, or `audioData.tensionCurve`.

- [ ] **Step 2: Change curve sizing helper**

Replace:

```cpp
void ensureCurveSizes(AudioData& audioData, int totalFrames)
```

with:

```cpp
void ensureCurveSizes(EditedData& editedData, int totalFrames)
{
    if (totalFrames <= 0)
        return;

    if (editedData.voicingCurve.size() != static_cast<size_t>(totalFrames))
        editedData.voicingCurve.assign(static_cast<size_t>(totalFrames),
                                       HNSepCurveProcessor::kDefaultVoicing);
    if (editedData.breathCurve.size() != static_cast<size_t>(totalFrames))
        editedData.breathCurve.assign(static_cast<size_t>(totalFrames),
                                      HNSepCurveProcessor::kDefaultBreath);
    if (editedData.tensionCurve.size() != static_cast<size_t>(totalFrames))
        editedData.tensionCurve.assign(static_cast<size_t>(totalFrames),
                                       HNSepCurveProcessor::kDefaultTension);
}
```

Delete `syncHNSepToEditedData(Project& project)` entirely.

- [ ] **Step 3: Rewrite public functions**

At the start of each function that edits curves, use:

```cpp
auto& editedData = project.getEditedData();
const int totalFrames = project.getFrameCount();
ensureCurveSizes(editedData, totalFrames);
```

Replace every `audioData.voicingCurve`, `audioData.breathCurve`, and `audioData.tensionCurve` with the matching `editedData` field.

Keep these `AudioData` fields in `recomputeMelForRange()`:

```cpp
audioData.melSpectrogram
audioData.harmonicWaveform
audioData.noiseWaveform
audioData.sampleRate
```

Use `const auto& editedData = project.getEditedData();` for active edit checks and `TensionProcessor` inputs:

```cpp
if (editedData.voicingCurve.empty() || editedData.breathCurve.empty() ||
    editedData.tensionCurve.empty())
    return;
```

- [ ] **Step 4: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: `HNSepCurveProcessor.cpp` no longer references removed `AudioData` curve fields.

- [ ] **Step 5: Commit**

```powershell
git add Source/Utils/HNSepCurveProcessor.cpp
git commit -m "refactor: store hnsep curves in edited data"
```

---

### Task 9: Migrate Undo and Snapshot Callers

**Files:**
- Modify: `Source/Undo/DragActions.h`
- Modify: `Source/Undo/F0Actions.h`
- Modify: `Source/Utils/NoteEditUtils.cpp`

- [ ] **Step 1: Confirm RED**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: errors in undo headers and note reset utilities reference removed `AudioData` fields.

- [ ] **Step 2: Update DragActions**

In `DragActions.h`, replace:

```cpp
auto& audioData = project.getAudioData();
SnapshotHelper::restoreFloatRange(audioData.f0, startFrame, beforeF0);
SnapshotHelper::restoreFloatRange(audioData.basePitch, startFrame, beforeBasePitch);
```

with:

```cpp
auto& editedData = project.getEditedData();
SnapshotHelper::restoreFloatRange(editedData.f0, startFrame, beforeF0);
SnapshotHelper::restoreFloatRange(editedData.basePitch, startFrame, beforeBasePitch);
```

Apply the same replacement for redo paths and resize actions.

- [ ] **Step 3: Update F0Actions**

In `F0Actions.h`, replace all restored ranges:

```cpp
auto& editedData = project.getEditedData();
SnapshotHelper::restoreFloatRange(editedData.f0, startFrame, beforeF0);
SnapshotHelper::restoreFloatRange(editedData.deltaPitch, startFrame, beforeDelta);
SnapshotHelper::restoreBoolRange(editedData.voicedMask, startFrame, beforeVoiced);
SnapshotHelper::restoreBoolRange(editedData.f0EditedMask, startFrame, beforeEdited);
```

Use the same fields for redo.

- [ ] **Step 4: Update NoteEditUtils**

In `NoteEditUtils.cpp`, replace:

```cpp
auto& audioData = project.getAudioData();
if (!audioData.f0EditedMask.empty())
```

with:

```cpp
auto& editedData = project.getEditedData();
if (!editedData.f0EditedMask.empty())
```

Replace `audioData.f0EditedMask[...] = false;` with `editedData.f0EditedMask[...] = false;`.

- [ ] **Step 5: Notify edited data changes**

After undo/redo restores ranges, ensure actions still call:

```cpp
project.refreshNoteCachesForRange(startFrame, startFrame + static_cast<int>(beforeF0.size()));
project.notifyListeners(ProjectChangeType::EditedDataChanged);
```

Use the restored range length for each action's stored snapshot.

- [ ] **Step 6: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: undo and note reset utilities no longer reference removed `AudioData` fields.

- [ ] **Step 7: Commit**

```powershell
git add Source/Undo/DragActions.h Source/Undo/F0Actions.h Source/Utils/NoteEditUtils.cpp
git commit -m "refactor: restore undo ranges into edited data"
```

---

### Task 10: Migrate IncrementalSynthesizer

**Files:**
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.cpp`

- [ ] **Step 1: Confirm RED**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: errors reference removed `project->getAudioData().voicedMask` or `vadMask`.

- [ ] **Step 2: Use EditedData in range computation**

In `computeResynthRange()`, replace the effective VAD logic with:

```cpp
const auto& effectiveVadMask = project->getEditedData().vadMask;
const int effectiveTotal = static_cast<int>(effectiveVadMask.size());
```

Remove fallback to `project->getAudioData().vadMask`.

- [ ] **Step 3: Use EditedData in synthesis range**

In `computeSynthesisRange()`, replace:

```cpp
auto &voicedMask = project->getAudioData().voicedMask;
auto &vadMask = project->getAudioData().vadMask;
```

with:

```cpp
const auto& editedData = project->getEditedData();
const auto& voicedMask = editedData.voicedMask;
const auto& vadMask = editedData.vadMask;
```

- [ ] **Step 4: Use EditedData f0 for synthesis preflight**

In `synthesizeRegion()`, replace:

```cpp
if (audioData.melSpectrogram.empty() || audioData.f0.empty()) {
```

with:

```cpp
if (audioData.melSpectrogram.empty() || project->getEditedData().f0.empty()) {
```

- [ ] **Step 5: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: `IncrementalSynthesizer.cpp` no longer references removed `AudioData` pitch/mask fields.

- [ ] **Step 6: Commit**

```powershell
git add Source/Audio/Synthesis/IncrementalSynthesizer.cpp
git commit -m "refactor: read synthesis masks from edited data"
```

---

### Task 11: Migrate EditorController Analysis Flow

**Files:**
- Modify: `Source/Audio/EditorController.cpp`

- [ ] **Step 1: Confirm RED**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: errors in `EditorController.cpp` reference removed `AudioData` fields.

- [ ] **Step 2: Use local analysis vectors during extraction**

Where analysis currently writes `audioData.f0`, introduce local vectors near the pitch extraction code:

```cpp
std::vector<float> analyzedF0;
std::vector<bool> analyzedVoicedMask;
std::vector<bool> analyzedVadMask;
```

Replace writes such as:

```cpp
audioData.f0.resize(targetFrames);
audioData.f0[i] = extractedF0[srcIdx];
```

with:

```cpp
analyzedF0.resize(static_cast<size_t>(targetFrames), 0.0f);
analyzedF0[static_cast<size_t>(i)] = extractedF0[static_cast<size_t>(srcIdx)];
```

Replace mask writes with `analyzedVoicedMask` and `analyzedVadMask`.

- [ ] **Step 3: Smooth local f0**

Replace:

```cpp
audioData.f0 = F0Smoother::smoothF0(audioData.f0, audioData.voicedMask);
audioData.f0 = PitchCurveProcessor::interpolateWithUvMask(
    audioData.f0, audioData.voicedMask);
```

with:

```cpp
analyzedF0 = F0Smoother::smoothF0(analyzedF0, analyzedVoicedMask);
analyzedF0 = PitchCurveProcessor::interpolateWithUvMask(
    analyzedF0, analyzedVoicedMask);
```

- [ ] **Step 4: Populate AnalysisData and EditedData**

After notes are available, replace assignments from `audioData` with:

```cpp
auto& edited = targetProject.getEditedData();
edited.voicedMask = analyzedVoicedMask;
edited.vadMask = analyzedVadMask;
PitchCurveProcessor::rebuildCurvesFromSource(targetProject, analyzedF0);
HNSepCurveProcessor::initializeCurves(targetProject);

auto& analysis = targetProject.getAnalysisData();
analysis.originalF0 = edited.f0;
analysis.originalPitch = edited.basePitch;
analysis.originalDeltaPitch = edited.deltaPitch;
analysis.originalVoicedMask = edited.voicedMask;
analysis.originalVADMask = edited.vadMask;

edited.f0EditedMask.assign(edited.f0.size(), false);
targetProject.refreshNoteCaches();
```

Ensure HNSep initialization happens after `EditedData` has a frame count.

- [ ] **Step 5: Migrate project-copy assignment**

Where the analyzed `projectCopy` is applied back to `project`, replace removed `AudioData` assignments with:

```cpp
project->getEditedData() = projectCopy->getEditedData();
project->getAnalysisData() = projectCopy->getAnalysisData();
```

Keep runtime audio assignments for waveform, originalWaveform, harmonic/noise waveforms, mel, segment debug, and sample rate.

- [ ] **Step 6: Update helper code later in the file**

For all later note segmentation or debug paths, replace:

```cpp
audioData.f0
audioData.voicedMask
audioData.vadMask
audioData.basePitch
audioData.deltaPitch
```

with:

```cpp
targetProject.getEditedData().f0
targetProject.getEditedData().voicedMask
targetProject.getEditedData().vadMask
targetProject.getEditedData().basePitch
targetProject.getEditedData().deltaPitch
```

Use local references to keep lines readable:

```cpp
auto& editedData = targetProject.getEditedData();
```

- [ ] **Step 7: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: `EditorController.cpp` no longer references removed `AudioData` fields.

- [ ] **Step 8: Commit**

```powershell
git add Source/Audio/EditorController.cpp
git commit -m "refactor: store analysis pitch data in edited data"
```

---

### Task 12: Migrate UI Read and Edit Paths

**Files:**
- Modify: `Source/UI/MainComponent.cpp`
- Modify: `Source/UI/PianoRollComponent.cpp`
- Modify: `Source/UI/PianoRoll/States/DrawHandler.cpp`
- Modify: `Source/UI/PianoRoll/States/SelectHandler.cpp`
- Modify: `Source/UI/PianoRoll/States/StretchHandler.cpp`
- Modify: `Source/UI/PianoRoll/NoteSplitter.cpp`
- Modify: `Source/UI/PianoRoll/PitchToolController.cpp`

- [ ] **Step 1: Confirm RED**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
```

Expected: UI files reference removed `AudioData` fields.

- [ ] **Step 2: Apply common read replacements**

In each file, keep `audioData` only for waveform, mel, sample rate, and segment debug. Add:

```cpp
auto& editedData = project->getEditedData();
const auto& editedData = project->getEditedData();
```

Use const when painting or reading. Replace:

```cpp
audioData.f0
audioData.basePitch
audioData.deltaPitch
audioData.voicedMask
audioData.vadMask
audioData.f0EditedMask
audioData.voicingCurve
audioData.breathCurve
audioData.tensionCurve
```

with the same field on `editedData`.

- [ ] **Step 3: Update availability checks**

Replace checks like:

```cpp
return audioData.waveform.getNumSamples() > 0 && !audioData.f0.empty();
```

with:

```cpp
return audioData.waveform.getNumSamples() > 0 &&
       !project->getEditedData().f0.empty();
```

- [ ] **Step 4: Update draw mode destructive edits**

In `DrawHandler.cpp`, replace the edit lambda body writes with:

```cpp
editedData.f0[static_cast<size_t>(idx)] = newFreq;
if (idx < static_cast<int>(editedData.deltaPitch.size()))
  editedData.deltaPitch[static_cast<size_t>(idx)] = newDelta;
if (idx < static_cast<int>(editedData.voicedMask.size()))
  editedData.voicedMask[static_cast<size_t>(idx)] = true;
if (idx < static_cast<int>(editedData.f0EditedMask.size()))
  editedData.f0EditedMask[static_cast<size_t>(idx)] = true;
```

Snapshot capture in DrawHandler must use:

```cpp
beforeF0 = SnapshotHelper::captureFloatRange(editedData.f0, 0, totalFrames);
beforeDelta = SnapshotHelper::captureFloatRange(editedData.deltaPitch, 0, totalFrames);
beforeVoiced = SnapshotHelper::captureBoolRange(editedData.voicedMask, 0, totalFrames);
beforeEdited = SnapshotHelper::captureBoolRange(editedData.f0EditedMask, 0, totalFrames);
```

- [ ] **Step 5: Update drag pitch preview**

In `SelectHandler.cpp`, replace base preview writes:

```cpp
editedData.basePitch[static_cast<size_t>(frame)] = baseMidi;
const float deltaMidi =
    (frame < static_cast<int>(editedData.deltaPitch.size()))
        ? editedData.deltaPitch[static_cast<size_t>(frame)]
        : 0.0f;
editedData.f0[static_cast<size_t>(frame)] =
    midiToFreq(baseMidi + deltaMidi);
```

For unvoiced preview:

```cpp
if (frame < static_cast<int>(editedData.voicedMask.size()) &&
    !editedData.voicedMask[static_cast<size_t>(frame)])
{
  editedData.f0[static_cast<size_t>(frame)] = 0.0f;
}
```

Remove all `baseF0` preview writes.

- [ ] **Step 6: Update dense delta access**

In `PitchToolController.cpp` and `MainComponent.cpp`, replace:

```cpp
const auto& denseDelta = project->getAudioData().deltaPitch;
```

with:

```cpp
const auto& denseDelta = project->getEditedData().deltaPitch;
```

- [ ] **Step 7: Run tests and build**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
cmake --build build --config Debug --target HachiTune
```

Expected: UI files no longer reference removed `AudioData` fields. Remaining failures should be in plugin or debug monitor files.

- [ ] **Step 8: Commit**

```powershell
git add Source/UI/MainComponent.cpp Source/UI/PianoRollComponent.cpp Source/UI/PianoRoll/States/DrawHandler.cpp Source/UI/PianoRoll/States/SelectHandler.cpp Source/UI/PianoRoll/States/StretchHandler.cpp Source/UI/PianoRoll/NoteSplitter.cpp Source/UI/PianoRoll/PitchToolController.cpp
git commit -m "refactor: migrate UI pitch state to edited data"
```

---

### Task 13: Enhance ProjectTreeView Validation Display

**Files:**
- Modify: `Source/UI/Debug/ProjectTreeView.cpp`

- [ ] **Step 1: Confirm RED or missing behavior**

Run:

```powershell
cmake --build build --config Debug --target HachiTune
```

Expected before implementation: debug tree compiles but does not show validation messages.

- [ ] **Step 2: Add validation entries**

In `ProjectTreeView::updatePropertyItems()`, after EditedData frame count, add:

```cpp
const auto validation = project->validateFrameData();
while (editedCat->getNumSubItems() > 2 + validation.messages.size())
  editedCat->removeSubItem(editedCat->getNumSubItems() - 1);

setOrUpdate(editedCat, 0, "Frames: " + juce::String(ed.getNumFrames()));
setOrUpdate(editedCat, 1, "Valid: " +
    juce::String(validation.isValid() ? "yes" : "no"));
for (int i = 0; i < validation.messages.size(); ++i)
{
  setOrUpdate(editedCat, 2 + i,
              "Warning: " + validation.messages[static_cast<int>(i)]);
}
```

Update the existing `while (editedCat->getNumSubItems() > 1)` block so it does not remove validation rows.

- [ ] **Step 3: Show all EditedData lengths**

Add rows after validation warnings:

```cpp
const int baseIndex = 2 + validation.messages.size();
setOrUpdate(editedCat, baseIndex + 0,
            "basePitch: " + juce::String(static_cast<int>(ed.basePitch.size())));
setOrUpdate(editedCat, baseIndex + 1,
            "deltaPitch: " + juce::String(static_cast<int>(ed.deltaPitch.size())));
setOrUpdate(editedCat, baseIndex + 2,
            "f0: " + juce::String(static_cast<int>(ed.f0.size())));
setOrUpdate(editedCat, baseIndex + 3,
            "voicedMask: " + juce::String(static_cast<int>(ed.voicedMask.size())));
setOrUpdate(editedCat, baseIndex + 4,
            "vadMask: " + juce::String(static_cast<int>(ed.vadMask.size())));
setOrUpdate(editedCat, baseIndex + 5,
            "voicingCurve: " + juce::String(static_cast<int>(ed.voicingCurve.size())));
setOrUpdate(editedCat, baseIndex + 6,
            "breathCurve: " + juce::String(static_cast<int>(ed.breathCurve.size())));
setOrUpdate(editedCat, baseIndex + 7,
            "tensionCurve: " + juce::String(static_cast<int>(ed.tensionCurve.size())));
setOrUpdate(editedCat, baseIndex + 8,
            "f0EditedMask: " + juce::String(static_cast<int>(ed.f0EditedMask.size())));
```

- [ ] **Step 4: Run build**

Run:

```powershell
cmake --build build --config Debug --target HachiTune
```

Expected: build succeeds for debug UI changes.

- [ ] **Step 5: Commit**

```powershell
git add Source/UI/Debug/ProjectTreeView.cpp
git commit -m "feat: show project frame validation in monitor"
```

---

### Task 14: Full Search Cleanup and Verification

**Files:**
- Modify any remaining files found by compile/search.

- [ ] **Step 1: Search for removed direct fields**

Run:

```powershell
Get-ChildItem -Path Source -Recurse -File -Include *.h,*.cpp |
  Select-String -Pattern 'audioData\.(f0|baseF0|basePitch|deltaPitch|voicingCurve|breathCurve|tensionCurve|voicedMask|vadMask|f0EditedMask)' -CaseSensitive:$false
```

Expected: no output except comments that explicitly describe removed legacy fields. If comments remain, update them to mention `EditedData`.

- [ ] **Step 2: Search for `getAudioData().` removed fields**

Run:

```powershell
Get-ChildItem -Path Source -Recurse -File -Include *.h,*.cpp |
  Select-String -Pattern 'getAudioData\(\)\.(f0|baseF0|basePitch|deltaPitch|voicingCurve|breathCurve|tensionCurve|voicedMask|vadMask|f0EditedMask)' -CaseSensitive:$false
```

Expected: no output.

- [ ] **Step 3: Run core tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `HachiTuneCoreTests` passes.

- [ ] **Step 4: Build standalone and plugin targets**

Run:

```powershell
cmake --build build --config Debug --target HachiTune
cmake --build build --config Debug --target HachiTunePlugin
```

Expected: both targets build. If plugin target name differs in the generated project, run:

```powershell
cmake --build build --config Debug --parallel
```

Expected: full build succeeds.

- [ ] **Step 5: Commit final cleanup**

If the search/build steps required extra fixes, commit them:

```powershell
git add Source CMakeLists.txt
git commit -m "refactor: finish edited data migration"
```

If no files changed, do not create an empty commit.

---

## Self-Review Checklist

- [ ] Every removed `AudioData` field is absent from `Source/Models/Project.h`.
- [ ] No production code reads or writes removed fields through `audioData` or `getAudioData()`.
- [ ] Serializer saves `analysisData` and `editedData`, not note-local caches.
- [ ] Legacy `pitchData` load fills `AnalysisData` and `EditedData`.
- [ ] `EditedData.f0EditedMask` exists and is not serialized in the new schema.
- [ ] `Project::validateFrameData()` reports size mismatches.
- [ ] Core tests fail before implementation and pass after implementation.
- [ ] Full build passes after migration.
