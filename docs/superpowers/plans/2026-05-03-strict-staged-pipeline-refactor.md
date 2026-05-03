# Strict Staged Pipeline Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move HachiTune to a strict staged pitch/mel/synthesis pipeline where source data, editable global state, and final playback audio have separate owners.

**Architecture:** Introduce the new data ownership fields first, then migrate pitch and mel processing to forward-only helpers, and finally isolate final waveform writes inside `IncrementalSynthesizer`. Each task leaves the project buildable and extends `HachiTuneCoreTests` before changing production behavior.

**Tech Stack:** C++17, JUCE 8, CMake, HachiTune core test harness in `Source/Tests/ProjectCoreTests.cpp`.

---

## Scope Check

This plan stays as one sequential plan because the subsystems are dependent:

- Data ownership must exist before pitch/mel call sites can move.
- Pitch and mel final arrays must exist before stretch can write them.
- Final waveform isolation depends on `EditedData.f0` and `EditedData.mel`
  being synthesis inputs.

Do not start with synthesis output isolation. It will be easier to review once
the final `f0` and `mel` ownership is already in place.

## File Structure

Core model files:

- `Source/Models/AnalysisData.h` owns immutable original analysis arrays,
  including new `originalMel`.
- `Source/Models/EditedData.h` owns editable pipeline arrays, including
  `tunedF0`, `baseVoicing/baseBreath/baseTension`, `adjustedSTFT`,
  `adjustedMel`, and final `mel`.
- `Source/Models/Project.h/.cpp` own validation, cache refresh, final waveform
  routing, dirty range helpers, and compatibility wrappers during migration.
- `Source/Models/ProjectSerializer.h/.cpp` own JSON save/load and legacy
  migration.

Pipeline files:

- `Source/Utils/PitchCurveProcessor.h/.cpp` builds `EditedData.tunedF0` and
  final `EditedData.f0`.
- `Source/Utils/HNSepCurveProcessor.h/.cpp` owns source-timeline HNSep base
  curves and `adjustedMel` recomputation.
- `Source/Audio/Synthesis/StretchProcessor.h/.cpp` maps source pipeline arrays
  to final output-timeline arrays.
- `Source/Utils/WarpMarkerProcessor.h/.cpp` applies stretch and dirty range
  mapping.
- `Source/Audio/Synthesis/IncrementalSynthesizer.h/.cpp` becomes the only
  writer of `AudioData::finalWaveform`.

UI and debug files:

- `Source/UI/HNSepLaneComponent.cpp` updates local caches during interaction
  and commits to global source base curves.
- `Source/UI/PianoRoll/States/DrawHandler.cpp`,
  `Source/UI/PianoRoll/States/SelectHandler.cpp`,
  `Source/UI/PianoRoll/PitchEditor.cpp`, and
  `Source/UI/PianoRollComponent.cpp` refresh local pitch caches from final
  global `f0` after commit/recompute.
- `Source/UI/Debug/MelViewComponent.cpp`,
  `Source/UI/Debug/ProjectTreeView.cpp`, and
  `Source/UI/Debug/TreeValueMonitor.cpp` read the new owners.
- `Source/UI/Main/MainComponent_ProjectIO.cpp` loads source waveform data and
  uses final waveform fallback for playback.
- `Source/UI/Main/ExportHelper.cpp` exports `finalWaveform` when present.

Tests:

- `Source/Tests/ProjectCoreTests.cpp` is the main regression harness.

Verification commands:

```powershell
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DUSE_DIRECTML=ON
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Debug --target HachiTune --parallel
```

Expected successful test output includes:

```text
All ProjectCoreTests passed
```

---

### Task 1: Add Pipeline Ownership Fields

**Files:**
- Modify: `Source/Models/AnalysisData.h`
- Modify: `Source/Models/EditedData.h`
- Modify: `Source/Models/Project.h`
- Modify: `Source/Models/Project.cpp`
- Modify: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Add the failing data ownership test**

In `Source/Tests/ProjectCoreTests.cpp`, update `resizeEditedData()` so future
tests initialize the new arrays:

```cpp
void resizeEditedData(EditedData& edited, int frames)
{
  edited.basePitch.resize(static_cast<size_t>(frames), 60.0f);
  edited.deltaPitch.resize(static_cast<size_t>(frames), 0.0f);
  edited.tunedF0.resize(static_cast<size_t>(frames), 261.63f);
  edited.f0.resize(static_cast<size_t>(frames), 261.63f);
  edited.voicedMask.resize(static_cast<size_t>(frames), true);
  edited.vadMask.resize(static_cast<size_t>(frames), true);
  edited.voicingCurve.resize(static_cast<size_t>(frames), 100.0f);
  edited.breathCurve.resize(static_cast<size_t>(frames), 100.0f);
  edited.tensionCurve.resize(static_cast<size_t>(frames), 0.0f);
  edited.baseVoicing.resize(static_cast<size_t>(frames), 100.0f);
  edited.baseBreath.resize(static_cast<size_t>(frames), 100.0f);
  edited.baseTension.resize(static_cast<size_t>(frames), 0.0f);
  edited.adjustedMel.assign(static_cast<size_t>(frames), {0.0f, 0.0f});
  edited.mel.assign(static_cast<size_t>(frames), {0.0f, 0.0f});
}
```

In `makeProject()`, replace the current mel initialization:

```cpp
  project.getAudioData().sourceMelSpectrogram = {
      {0.1f, 0.2f}, {0.2f, 0.3f}, {0.3f, 0.4f}, {0.4f, 0.5f}};
  project.getAudioData().melSpectrogram = {
      {0.1f, 0.2f}, {0.2f, 0.3f}, {0.3f, 0.4f}, {0.4f, 0.5f}};
```

with:

```cpp
  const std::vector<std::vector<float>> originalMel = {
      {0.1f, 0.2f}, {0.2f, 0.3f}, {0.3f, 0.4f}, {0.4f, 0.5f}};
```

Then set the new owners after `analysis` and `edited` are created:

```cpp
  analysis.originalMel = originalMel;
  edited.adjustedMel = originalMel;
  edited.mel = originalMel;
  edited.tunedF0 = edited.f0;
  edited.baseVoicing = edited.voicingCurve;
  edited.baseBreath = edited.breathCurve;
  edited.baseTension = edited.tensionCurve;
```

Add this test before `testSerializerOmitsNoteCaches()`:

```cpp
void testPipelineOwnershipFields()
{
  auto project = makeProject();
  const auto& analysis = project.getAnalysisData();
  const auto& edited = project.getEditedData();
  auto& audioData = project.getAudioData();

  expectMelNear(analysis.originalMel,
                {{0.1f, 0.2f}, {0.2f, 0.3f}, {0.3f, 0.4f}, {0.4f, 0.5f}},
                0.0001f,
                "analysis owns immutable original mel");
  expectVectorNear(edited.tunedF0, edited.f0, 0.0001f,
                   "edited owns tunedF0 source pitch stage");
  expectMelNear(edited.mel, analysis.originalMel, 0.0001f,
                "edited owns final mel stage");
  expect(edited.baseVoicing.size() == edited.f0.size(),
         "edited owns source base voicing curve");
  expect(edited.baseBreath.size() == edited.f0.size(),
         "edited owns source base breath curve");
  expect(edited.baseTension.size() == edited.f0.size(),
         "edited owns source base tension curve");

  audioData.finalWaveform.setSize(1, 8);
  audioData.finalWaveform.clear();
  expect(audioData.finalWaveform.getNumSamples() == 8,
         "audio data has separate final waveform buffer");
}
```

Call it first in `main()`:

```cpp
  testPipelineOwnershipFields();
```

- [ ] **Step 2: Run the test target and confirm the compile failure**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
```

Expected: compile fails because `AnalysisData::originalMel`,
`EditedData::tunedF0`, `EditedData::baseVoicing`, `EditedData::baseBreath`,
`EditedData::baseTension`, `EditedData::adjustedMel`, `EditedData::mel`, and
`AudioData::finalWaveform` do not exist.

- [ ] **Step 3: Add `AnalysisData::originalMel`**

In `Source/Models/AnalysisData.h`, add the matrix field after
`originalVADMask`:

```cpp
  std::vector<std::vector<float>> originalMel; // source timeline [T, NUM_MELS]
```

Update `clear()`:

```cpp
    originalMel.clear();
```

Leave `getNumFrames()` based on `originalF0`. Mel can be absent until mel
analysis has run.

- [ ] **Step 4: Add new `EditedData` fields**

In `Source/Models/EditedData.h`, add these members after `deltaPitch`:

```cpp
  std::vector<float> tunedF0;         // source/non-stretched [T] Hz
```

Add these after the existing HNSep curves:

```cpp
  std::vector<float> baseVoicing;     // source/non-stretched [T], default 100
  std::vector<float> baseBreath;      // source/non-stretched [T], default 100
  std::vector<float> baseTension;     // source/non-stretched [T], default 0
  std::vector<float> adjustedSTFT;    // source/non-stretched interleaved STFT cache
  std::vector<std::vector<float>> adjustedMel; // source/non-stretched [T, NUM_MELS]
  std::vector<std::vector<float>> mel;         // output timeline [T, NUM_MELS]
```

Update `clear()`:

```cpp
    tunedF0.clear();
    baseVoicing.clear();
    baseBreath.clear();
    baseTension.clear();
    adjustedSTFT.clear();
    adjustedMel.clear();
    mel.clear();
```

Update `resize(int numFrames)`:

```cpp
    tunedF0.resize(n, 0.0f);
    baseVoicing.resize(n, 100.0f);
    baseBreath.resize(n, 100.0f);
    baseTension.resize(n, 0.0f);
    adjustedMel.resize(n);
    mel.resize(n);
```

- [ ] **Step 5: Add `AudioData::finalWaveform` and frame-count fallback**

In `Source/Models/Project.h`, add this member after `originalWaveform`:

```cpp
    juce::AudioBuffer<float> finalWaveform; // final playback/export output
```

Change `AudioData::getNumFrames()`:

```cpp
    int getNumFrames() const
    {
        if (waveform.getNumSamples() == 0)
            return 0;
        return waveform.getNumSamples() / HOP_SIZE + 1;
    }
```

Add `#include "../Utils/Constants.h"` is not acceptable in `Project.h` because
it already includes many UI-facing types and would introduce a dependency
cycle. Instead, keep `AudioData::getNumFrames()` as a declaration in
`Project.h`:

```cpp
    int getNumFrames() const;
```

and implement it in `Source/Models/Project.cpp` near `Project::Project()`:

```cpp
int AudioData::getNumFrames() const
{
    if (waveform.getNumSamples() == 0)
        return 0;
    return waveform.getNumSamples() / HOP_SIZE + 1;
}
```

The implementation can use `HOP_SIZE` because `Project.cpp` already includes
`../Utils/Constants.h`.

- [ ] **Step 6: Update `Project::getFrameCount()` to prefer final edited data**

In `Source/Models/Project.cpp`, replace `Project::getFrameCount()` with:

```cpp
int Project::getFrameCount() const
{
    if (!editedData.f0.empty())
        return static_cast<int>(editedData.f0.size());
    if (!editedData.mel.empty())
        return static_cast<int>(editedData.mel.size());
    if (!editedData.tunedF0.empty())
        return static_cast<int>(editedData.tunedF0.size());
    if (!analysisData.originalMel.empty())
        return static_cast<int>(analysisData.originalMel.size());
    return audioData.getNumFrames();
}
```

- [ ] **Step 7: Run the focused test target**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `All ProjectCoreTests passed`. If production compile fails because
`AudioData::getNumFrames()` references `HOP_SIZE`, fix the declaration split
from Step 5 exactly as written.

- [ ] **Step 8: Commit**

Run:

```powershell
git add Source/Models/AnalysisData.h Source/Models/EditedData.h Source/Models/Project.h Source/Models/Project.cpp Source/Tests/ProjectCoreTests.cpp
git commit -m "feat: add staged pipeline data owners"
```

---

### Task 2: Migrate Serializer And Validation To New Owners

**Files:**
- Modify: `Source/Models/ProjectSerializer.cpp`
- Modify: `Source/Models/ProjectSerializer.h`
- Modify: `Source/Models/Project.cpp`
- Modify: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Add failing serializer tests for new fields**

In `Source/Tests/ProjectCoreTests.cpp`, add this test after
`testPipelineOwnershipFields()`:

```cpp
void testSerializerSavesCompactPipelineState()
{
  auto project = makeProject();
  const auto json = ProjectSerializer::toJson(project);
  const auto analysis = json.getProperty("analysisData", juce::var());
  const auto edited = json.getProperty("editedData", juce::var());

  require(analysis.isObject(), "analysisData is an object");
  require(edited.isObject(), "editedData is an object");

  expect(!hasProperty(analysis, "originalMel"),
         "analysis originalMel is not serialized as a large matrix");
  expect(hasProperty(edited, "tunedF0"), "edited tunedF0 is serialized");
  expect(hasProperty(edited, "baseVoicing"), "edited baseVoicing is serialized");
  expect(hasProperty(edited, "baseBreath"), "edited baseBreath is serialized");
  expect(hasProperty(edited, "baseTension"), "edited baseTension is serialized");
  expect(hasProperty(edited, "f0"), "edited final f0 is serialized");
  expect(!hasProperty(edited, "adjustedSTFT"),
         "edited adjustedSTFT is not serialized");
  expect(!hasProperty(edited, "adjustedMel"),
         "edited adjustedMel is not serialized as a large matrix");
  expect(!hasProperty(edited, "mel"),
         "edited final mel is not serialized as a large matrix");
}
```

Add this test after the new serializer test:

```cpp
void testSerializerLoadsCompactPipelineState()
{
  auto* root = new juce::DynamicObject();
  root->setProperty("name", "PipelineLoad");
  root->setProperty("sampleRate", 44100);

  auto* analysis = new juce::DynamicObject();
  analysis->setProperty("originalF0", "100 110 120");
  analysis->setProperty("originalPitch", "60 61 62");
  analysis->setProperty("originalDeltaPitch", "0 0.1 0.2");
  analysis->setProperty("originalVoicedMask", "111");
  analysis->setProperty("originalVADMask", "111");
  root->setProperty("analysisData", juce::var(analysis));

  auto* edited = new juce::DynamicObject();
  edited->setProperty("basePitch", "60 60 60");
  edited->setProperty("deltaPitch", "0 0.1 0.2");
  edited->setProperty("tunedF0", "100 110 120");
  edited->setProperty("f0", "100 110 120");
  edited->setProperty("voicedMask", "111");
  edited->setProperty("vadMask", "111");
  edited->setProperty("voicingCurve", "100 100 100");
  edited->setProperty("breathCurve", "100 100 100");
  edited->setProperty("tensionCurve", "0 0 0");
  edited->setProperty("baseVoicing", "90 91 92");
  edited->setProperty("baseBreath", "80 81 82");
  edited->setProperty("baseTension", "1 2 3");
  root->setProperty("editedData", juce::var(edited));

  Project project;
  require(ProjectSerializer::fromJson(project, juce::var(root)),
          "compact pipeline json loads");

  expectVectorNear(project.getEditedData().tunedF0,
                   {100.0f, 110.0f, 120.0f}, 0.0001f,
                   "load restores tunedF0");
  expectVectorNear(project.getEditedData().baseVoicing,
                   {90.0f, 91.0f, 92.0f}, 0.0001f,
                   "load restores baseVoicing");
  expectVectorNear(project.getEditedData().baseBreath,
                   {80.0f, 81.0f, 82.0f}, 0.0001f,
                   "load restores baseBreath");
  expectVectorNear(project.getEditedData().baseTension,
                   {1.0f, 2.0f, 3.0f}, 0.0001f,
                   "load restores baseTension");
}
```

Call both near the top of `main()`:

```cpp
  testSerializerSavesCompactPipelineState();
  testSerializerLoadsCompactPipelineState();
```

- [ ] **Step 2: Run tests and confirm failure**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: test executable runs and reports missing `tunedF0`/base curve JSON
fields, or compile fails because serializer helpers do not reference the new
members.

- [ ] **Step 3: Update `editedDataToJson()`**

In `Source/Models/ProjectSerializer.cpp`, update
`ProjectSerializer::editedDataToJson()`:

```cpp
juce::var ProjectSerializer::editedDataToJson(const EditedData& data)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("basePitch", floatArrayToString(data.basePitch, 4));
    obj->setProperty("deltaPitch", floatArrayToString(data.deltaPitch, 4));
    obj->setProperty("tunedF0", floatArrayToString(data.tunedF0, 2));
    obj->setProperty("f0", floatArrayToString(data.f0, 2));
    obj->setProperty("voicedMask", boolArrayToString(data.voicedMask));
    obj->setProperty("vadMask", boolArrayToString(data.vadMask));
    obj->setProperty("voicingCurve", floatArrayToString(data.voicingCurve, 2));
    obj->setProperty("breathCurve", floatArrayToString(data.breathCurve, 2));
    obj->setProperty("tensionCurve", floatArrayToString(data.tensionCurve, 2));
    obj->setProperty("baseVoicing", floatArrayToString(data.baseVoicing, 2));
    obj->setProperty("baseBreath", floatArrayToString(data.baseBreath, 2));
    obj->setProperty("baseTension", floatArrayToString(data.baseTension, 2));
    return juce::var(obj);
}
```

Do not serialize `adjustedSTFT`, `adjustedMel`, or `mel` in this task.

- [ ] **Step 4: Update `editedDataFromJson()`**

In `Source/Models/ProjectSerializer.cpp`, update
`ProjectSerializer::editedDataFromJson()`:

```cpp
bool ProjectSerializer::editedDataFromJson(EditedData& data, const juce::var& json)
{
    if (!json.isObject())
        return false;
    data.basePitch = stringToFloatArray(json.getProperty("basePitch", "").toString());
    data.deltaPitch = stringToFloatArray(json.getProperty("deltaPitch", "").toString());
    data.tunedF0 = stringToFloatArray(json.getProperty("tunedF0", "").toString());
    data.f0 = stringToFloatArray(json.getProperty("f0", "").toString());
    data.voicedMask = stringToBoolArray(json.getProperty("voicedMask", "").toString());
    data.vadMask = stringToBoolArray(json.getProperty("vadMask", "").toString());
    data.voicingCurve = stringToFloatArray(json.getProperty("voicingCurve", "").toString());
    data.breathCurve = stringToFloatArray(json.getProperty("breathCurve", "").toString());
    data.tensionCurve = stringToFloatArray(json.getProperty("tensionCurve", "").toString());
    data.baseVoicing = stringToFloatArray(json.getProperty("baseVoicing", "").toString());
    data.baseBreath = stringToFloatArray(json.getProperty("baseBreath", "").toString());
    data.baseTension = stringToFloatArray(json.getProperty("baseTension", "").toString());

    if (data.tunedF0.empty())
        data.tunedF0 = data.f0;
    if (data.baseVoicing.empty())
        data.baseVoicing = data.voicingCurve;
    if (data.baseBreath.empty())
        data.baseBreath = data.breathCurve;
    if (data.baseTension.empty())
        data.baseTension = data.tensionCurve;
    return true;
}
```

- [ ] **Step 5: Update legacy pitch-data migration**

In `ProjectSerializer::legacyPitchDataFromJson()`, after existing fields are
loaded, add:

```cpp
    editedData.tunedF0 = editedData.f0;
    editedData.baseVoicing = editedData.voicingCurve;
    editedData.baseBreath = editedData.breathCurve;
    editedData.baseTension = editedData.tensionCurve;
```

- [ ] **Step 6: Update frame validation**

In `Source/Models/Project.cpp`, update `Project::validateFrameData()`:

1. Add these to `useEditedFrameCount(...)`:

```cpp
    useEditedFrameCount(editedData.tunedF0.size());
    useEditedFrameCount(editedData.baseVoicing.size());
    useEditedFrameCount(editedData.baseBreath.size());
    useEditedFrameCount(editedData.baseTension.size());
```

2. Add these checks inside `if (editedFrames > 0)`:

```cpp
        checkFloat(editedData.tunedF0, "editedData.tunedF0", false);
        checkFloat(editedData.baseVoicing, "editedData.baseVoicing", false);
        checkFloat(editedData.baseBreath, "editedData.baseBreath", false);
        checkFloat(editedData.baseTension, "editedData.baseTension", false);
```

3. Replace the old ragged `audioData.melSpectrogram` validation with this:

```cpp
    auto checkMelMatrix = [&](const std::vector<std::vector<float>>& matrix,
                              const char* name) {
        if (matrix.empty())
            return;
        const auto bins = matrix.front().size();
        for (const auto& row : matrix)
        {
            if (row.size() != bins)
            {
                result.messages.push_back(juce::String(name) + " ragged rows");
                break;
            }
        }
    };

    checkMelMatrix(analysisData.originalMel, "analysisData.originalMel");
    checkMelMatrix(editedData.adjustedMel, "editedData.adjustedMel");
    checkMelMatrix(editedData.mel, "editedData.mel");
```

- [ ] **Step 7: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `All ProjectCoreTests passed`.

- [ ] **Step 8: Commit**

Run:

```powershell
git add Source/Models/ProjectSerializer.cpp Source/Models/ProjectSerializer.h Source/Models/Project.cpp Source/Tests/ProjectCoreTests.cpp
git commit -m "feat: serialize compact staged pipeline state"
```

---

### Task 3: Move Mel Read/Write Call Sites To New Owners

**Files:**
- Modify: `Source/Audio/Analysis/AudioAnalyzer.cpp`
- Modify: `Source/Audio/EditorController.cpp`
- Modify: `Source/Audio/RealtimePitchProcessor.cpp`
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.cpp`
- Modify: `Source/Models/Project.cpp`
- Modify: `Source/UI/Debug/MelViewComponent.cpp`
- Modify: `Source/UI/Debug/ProjectTreeView.cpp`
- Modify: `Source/UI/Main/MainComponent_ProjectIO.cpp`
- Modify: `Source/UI/PianoRoll/NoteSplitter.cpp`
- Modify: `Source/Utils/HNSepCurveProcessor.cpp`
- Modify: `Source/Utils/WarpMarkerProcessor.cpp`
- Modify: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Add a test that old `AudioData` mel fields are unused by core helpers**

In `Source/Tests/ProjectCoreTests.cpp`, rename
`testRecomputeFromMarkersBuildsMelFromSourceCache()` to
`testRecomputeFromMarkersBuildsMelFromEditedAdjustedMel()`, and replace its
body with:

```cpp
void testRecomputeFromMarkersBuildsMelFromEditedAdjustedMel()
{
  auto project = makeProject();
  const std::vector<std::vector<float>> adjustedMel = {
      {0.0f}, {10.0f}, {20.0f}, {30.0f}};
  const std::vector<Project::WarpMarker> current = {
      {0, 0}, {2, 3}, {4, 5}};
  const std::vector<Project::WarpMarker> target = {
      {0, 0}, {2, 4}, {4, 6}};

  project.getAnalysisData().originalMel = adjustedMel;
  project.getEditedData().adjustedMel = adjustedMel;
  project.getEditedData().mel.assign(5, {999.0f});

  WarpMarkerProcessor::recomputeFromMarkers(project, current, target, false);

  const auto targetMap =
      WarpMarkerProcessor::buildWarpMapWithEndpoints(project, target);
  const auto expected =
      StretchProcessor::buildOutputMel(adjustedMel, targetMap,
                                       static_cast<int>(
                                           project.getEditedData().f0.size()));
  expectMelNear(project.getEditedData().mel, expected, 0.0001f,
                "recompute rebuilds final mel from adjusted source mel");
  expectMelNear(project.getEditedData().adjustedMel, adjustedMel,
                0.0001f, "recompute preserves adjusted source mel");
  expect(project.getWarpMarkers().empty(),
         "mel recompute preview does not commit project markers");
}
```

Update the call in `main()`:

```cpp
  testRecomputeFromMarkersBuildsMelFromEditedAdjustedMel();
```

- [ ] **Step 2: Run the tests and confirm old owner failures**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: tests fail because `WarpMarkerProcessor` still writes
`audioData.melSpectrogram`.

- [ ] **Step 3: Migrate analysis mel writes**

In `Source/Audio/Analysis/AudioAnalyzer.cpp`, replace each pair:

```cpp
  audioData.sourceMelSpectrogram = melComputer.compute(samples, numSamples);
  audioData.melSpectrogram = audioData.sourceMelSpectrogram;
  int targetFrames = static_cast<int>(audioData.melSpectrogram.size());
```

with:

```cpp
  auto& analysisData = project.getAnalysisData();
  analysisData.originalMel = melComputer.compute(samples, numSamples);
  editedData.adjustedMel = analysisData.originalMel;
  editedData.mel = editedData.adjustedMel;
  int targetFrames = static_cast<int>(editedData.mel.size());
```

For all remaining reads in `AudioAnalyzer.cpp`, replace:

```cpp
audioData.melSpectrogram
```

with:

```cpp
editedData.mel
```

when the code needs final output-timeline mel, and with:

```cpp
project.getAnalysisData().originalMel
```

when the code needs source-timeline mel for note source slices.

- [ ] **Step 4: Migrate `EditorController` mel writes**

In `Source/Audio/EditorController.cpp`, replace recompute blocks that assign
`audioData.sourceMelSpectrogram` and `audioData.melSpectrogram`:

```cpp
  auto& analysis = targetProject.getAnalysisData();
  analysis.originalMel = melComputer.compute(samples, numSamples);
  editedData.adjustedMel = analysis.originalMel;
  editedData.mel = editedData.adjustedMel;
  int targetFrames = static_cast<int>(editedData.mel.size());
```

Replace synthesis readiness checks:

```cpp
  if (audioData.melSpectrogram.empty() || project.getEditedData().f0.empty())
```

with:

```cpp
  if (project.getEditedData().mel.empty() || project.getEditedData().f0.empty())
```

Replace async copy-back lines:

```cpp
      projectShared->getAudioData().melSpectrogram =
          projectCopy->getAudioData().melSpectrogram;
      projectShared->getAudioData().sourceMelSpectrogram =
          projectCopy->getAudioData().sourceMelSpectrogram;
```

with:

```cpp
      projectShared->getAnalysisData().originalMel =
          projectCopy->getAnalysisData().originalMel;
      projectShared->getEditedData().adjustedMel =
          projectCopy->getEditedData().adjustedMel;
      projectShared->getEditedData().mel =
          projectCopy->getEditedData().mel;
```

- [ ] **Step 5: Migrate synthesis mel reads**

In `Source/Audio/Synthesis/IncrementalSynthesizer.cpp`, replace:

```cpp
  auto &audioData = project->getAudioData();
  if (audioData.melSpectrogram.empty() || project->getEditedData().f0.empty()) {
```

with:

```cpp
  auto &audioData = project->getAudioData();
  auto& editedData = project->getEditedData();
  if (editedData.mel.empty() || editedData.f0.empty()) {
```

Replace final range clamp:

```cpp
  endFrame =
      std::min(static_cast<int>(audioData.melSpectrogram.size()), endFrame);
```

with:

```cpp
  endFrame = std::min(static_cast<int>(editedData.mel.size()), endFrame);
```

Replace mel slice:

```cpp
  if (startFrame < static_cast<int>(audioData.melSpectrogram.size()) &&
      endFrame <= static_cast<int>(audioData.melSpectrogram.size()))
  {
    melRange.assign(audioData.melSpectrogram.begin() + startFrame,
                    audioData.melSpectrogram.begin() + endFrame);
  }
```

with:

```cpp
  if (startFrame < static_cast<int>(editedData.mel.size()) &&
      endFrame <= static_cast<int>(editedData.mel.size()))
  {
    melRange.assign(editedData.mel.begin() + startFrame,
                    editedData.mel.begin() + endFrame);
  }
```

- [ ] **Step 6: Migrate warp/stretch mel writes**

In `Source/Utils/WarpMarkerProcessor.cpp`, replace mel ownership in
`rebuildSourceDerivedOutput()`:

```cpp
    auto& analysisData = project.getAnalysisData();
    auto& editedData = project.getEditedData();
    if (!editedData.adjustedMel.empty())
    {
        editedData.mel = StretchProcessor::buildOutputMel(
            editedData.adjustedMel, warpMap, outputFrames);
    }
    else if (!analysisData.originalMel.empty())
    {
        editedData.adjustedMel = analysisData.originalMel;
        editedData.mel = StretchProcessor::buildOutputMel(
            editedData.adjustedMel, warpMap, outputFrames);
    }
```

In `recomputeFromMarkers()`, replace source mel unwarp/write logic with:

```cpp
    auto& analysisData = project.getAnalysisData();
    if (editedData.adjustedMel.empty() &&
        !mapsAlreadyMatch &&
        !editedData.mel.empty())
    {
        const int sourceFrames = warpMap.back().sourceFrame;
        editedData.adjustedMel =
            unwarpMelToSource(editedData.mel, currentMap, sourceFrames);
    }

    if (editedData.adjustedMel.empty() && !analysisData.originalMel.empty())
        editedData.adjustedMel = analysisData.originalMel;

    if (!editedData.adjustedMel.empty())
    {
        editedData.mel =
            StretchProcessor::buildOutputMel(editedData.adjustedMel,
                                             warpMap, newTotalFrames);
    }
```

- [ ] **Step 7: Migrate HNSep mel writes**

In `Source/Utils/HNSepCurveProcessor.cpp`, replace `audioData.sourceMelSpectrogram`
with `project.getAnalysisData().originalMel` for immutable source fallback, and
replace the mutable source mel target with `editedData.adjustedMel`.

Use this ownership inside `recomputeMelForRange()`:

```cpp
        auto& analysisData = project.getAnalysisData();
        auto& editedData = project.getEditedData();
        if (editedData.adjustedMel.empty())
            editedData.adjustedMel = analysisData.originalMel;
        if (editedData.adjustedMel.empty())
            return;
```

When writing note source mel frames, replace:

```cpp
                    audioData.sourceMelSpectrogram[static_cast<size_t>(f)] =
                        srcMel[static_cast<size_t>(noteLocal)];
```

with:

```cpp
                    editedData.adjustedMel[static_cast<size_t>(f)] =
                        srcMel[static_cast<size_t>(noteLocal)];
```

When rebuilding final mel, replace:

```cpp
            audioData.melSpectrogram = StretchProcessor::buildOutputMel(
                audioData.sourceMelSpectrogram, warpMap, project.getFrameCount());
```

with:

```cpp
            editedData.mel = StretchProcessor::buildOutputMel(
                editedData.adjustedMel, warpMap, project.getFrameCount());
```

- [ ] **Step 8: Migrate UI/debug reads**

In `Source/UI/Debug/MelViewComponent.cpp`, replace:

```cpp
  const auto& mel = audioData.melSpectrogram;
```

with:

```cpp
  const auto& mel = project->getEditedData().mel;
```

In `Source/UI/Debug/ProjectTreeView.cpp`, replace audio mel rows:

```cpp
"audio.sourceMelSpectrogram: ..."
"audio.melSpectrogram: ..."
```

with:

```cpp
"analysis.originalMel: " + formatMatrixSize(project.getAnalysisData().originalMel)
"edited.adjustedMel: " + formatMatrixSize(project.getEditedData().adjustedMel)
"edited.mel: " + formatMatrixSize(project.getEditedData().mel)
```

- [ ] **Step 9: Remove old mel fields from `AudioData`**

In `Source/Models/Project.h`, delete:

```cpp
    std::vector<std::vector<float>> sourceMelSpectrogram;
    std::vector<std::vector<float>> melSpectrogram;
```

- [ ] **Step 10: Use `rg` to catch remaining old-owner references**

Run:

```powershell
rg -n "sourceMelSpectrogram|melSpectrogram" Source
```

Expected: no production matches. If tests contain old names, update them to
`analysis.originalMel`, `edited.adjustedMel`, or `edited.mel` according to the
timeline they assert.

- [ ] **Step 11: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `All ProjectCoreTests passed`.

- [ ] **Step 12: Commit**

Run:

```powershell
git add Source
git commit -m "refactor: move mel pipeline ownership out of AudioData"
```

---

### Task 4: Split Source `tunedF0` From Final `f0`

**Files:**
- Modify: `Source/Utils/PitchCurveProcessor.h`
- Modify: `Source/Utils/PitchCurveProcessor.cpp`
- Modify: `Source/Audio/Synthesis/StretchProcessor.h`
- Modify: `Source/Audio/Synthesis/StretchProcessor.cpp`
- Modify: `Source/Utils/WarpMarkerProcessor.cpp`
- Modify: `Source/Models/Project.cpp`
- Modify: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Add failing tunedF0 stretch test**

In `Source/Tests/ProjectCoreTests.cpp`, update `testStretchEditedData()` so it
initializes and verifies `tunedF0`:

```cpp
  data.tunedF0 = {100.0f, 200.0f, 400.0f};
```

After `StretchProcessor::stretchEditedData(...)`, add:

```cpp
  expect(data.tunedF0.size() == 3,
         "stretch keeps tunedF0 on source timeline");
  expect(data.f0.size() == 4, "stretch writes output f0 size");
  expectNear(data.f0[1], 141.42136f, 0.001f,
             "final f0 is stretched from tunedF0 in log frequency space");
```

This expected value is geometric interpolation halfway between 100 Hz and
200 Hz.

- [ ] **Step 2: Run tests and confirm failure**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `testStretchEditedData` fails because `stretchEditedData()` currently
resamples `basePitch/deltaPitch` and recomputes `f0` from them.

- [ ] **Step 3: Add pitch composition helpers**

In `Source/Utils/PitchCurveProcessor.h`, add declarations:

```cpp
    /**
     * Compose source/non-stretched tunedF0 from current basePitch/deltaPitch
     * and note tools. Does not apply stretch.
     */
    void composeTunedF0InPlace(Project& project,
                               bool applyUvMask,
                               float globalPitchOffset = 0.0f);

    /**
     * Reload note-local display pitch caches from final EditedData.f0.
     */
    void refreshNotePitchCachesFromFinalF0(Project& project,
                                           int startFrame,
                                           int endFrame);
```

In `Source/Utils/PitchCurveProcessor.cpp`, implement:

```cpp
    void composeTunedF0InPlace(Project& project,
                               bool applyUvMask,
                               float globalPitchOffset)
    {
        auto composed = composeF0(project, applyUvMask, globalPitchOffset);
        auto& editedData = project.getEditedData();
        editedData.tunedF0 = std::move(composed);
        if (editedData.f0.empty())
            editedData.f0 = editedData.tunedF0;
        project.notifyListeners(ProjectChangeType::EditedDataChanged);
    }

    void refreshNotePitchCachesFromFinalF0(Project& project,
                                           int startFrame,
                                           int endFrame)
    {
        auto& editedData = project.getEditedData();
        if (editedData.f0.empty())
            return;

        const int totalFrames = static_cast<int>(editedData.f0.size());
        const int clampedStart = std::max(0, startFrame);
        const int clampedEnd = std::min(totalFrames, endFrame);
        if (clampedEnd <= clampedStart)
            return;

        for (auto& note : project.getNotes())
        {
            if (note.isRest())
                continue;
            if (note.getEndFrame() <= clampedStart ||
                note.getStartFrame() >= clampedEnd)
                continue;

            const int noteStart = note.getStartFrame();
            const int noteEnd = std::min(note.getEndFrame(), totalFrames);
            const int len = noteEnd - noteStart;
            if (len <= 0)
                continue;

            std::vector<float> finalDelta(static_cast<size_t>(len), 0.0f);
            for (int i = 0; i < len; ++i)
            {
                const int frame = noteStart + i;
                const float base = frame < static_cast<int>(editedData.basePitch.size())
                    ? editedData.basePitch[static_cast<size_t>(frame)]
                    : note.getAdjustedMidiNote();
                const float freq = editedData.f0[static_cast<size_t>(frame)];
                finalDelta[static_cast<size_t>(i)] =
                    freq > 0.0f ? freqToMidi(freq) - base : 0.0f;
            }
            note.setDeltaPitch(std::move(finalDelta));
        }
    }
```

- [ ] **Step 4: Change `composeF0InPlace()` to maintain both stages**

In `PitchCurveProcessor::composeF0InPlace()`, replace:

```cpp
        editedData.f0 = std::move(composed);
```

with:

```cpp
        editedData.tunedF0 = composed;
        editedData.f0 = std::move(composed);
```

This keeps non-stretched projects compatible. Stretch replaces `f0` from
`tunedF0` when a warp map is applied.

- [ ] **Step 5: Stretch final f0 from source tunedF0**

In `Source/Audio/Synthesis/StretchProcessor.cpp`, add a helper inside
`stretchEditedData()`:

```cpp
  auto resampleFrequencyLog = [&](const std::vector<float>& src) {
    std::vector<float> dst(static_cast<size_t>(newTotalFrames), 0.0f);
    for (int i = 0; i < newTotalFrames; ++i)
    {
      float srcF = inverseMapFrame(markers, static_cast<float>(i));
      int srcIdx = static_cast<int>(std::floor(srcF));
      float frac = srcF - static_cast<float>(srcIdx);
      int srcMax = static_cast<int>(src.size()) - 1;
      if (src.empty())
        continue;
      srcIdx = std::clamp(srcIdx, 0, std::max(0, srcMax));
      int srcNext = std::min(srcIdx + 1, std::max(0, srcMax));
      const float a = src[static_cast<size_t>(srcIdx)];
      const float b = src[static_cast<size_t>(srcNext)];
      if (a > 0.0f && b > 0.0f)
      {
        dst[static_cast<size_t>(i)] =
            std::exp(std::log(a) * (1.0f - frac) + std::log(b) * frac);
      }
      else
      {
        dst[static_cast<size_t>(i)] = frac < 0.5f ? a : b;
      }
    }
    return dst;
  };
```

Change the end of `stretchEditedData()`:

```cpp
  const auto sourceTunedF0 =
      !edited.tunedF0.empty() ? edited.tunedF0 : edited.f0;
  edited.f0 = resampleFrequencyLog(sourceTunedF0);
```

Do not assign `edited.tunedF0` in `stretchEditedData()`. It stays source
timeline.

- [ ] **Step 6: Refresh note caches from final f0 after stretch**

In `Source/Utils/WarpMarkerProcessor.cpp`, after `project.refreshNoteCaches();`
in `recomputeFromMarkers()`, add:

```cpp
    PitchCurveProcessor::refreshNotePitchCachesFromFinalF0(
        project, 0, static_cast<int>(editedData.f0.size()));
```

Add this include at the top of `Source/Utils/WarpMarkerProcessor.cpp`:

```cpp
#include "PitchCurveProcessor.h"
```

- [ ] **Step 7: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `All ProjectCoreTests passed`.

- [ ] **Step 8: Commit**

Run:

```powershell
git add Source/Utils/PitchCurveProcessor.h Source/Utils/PitchCurveProcessor.cpp Source/Audio/Synthesis/StretchProcessor.h Source/Audio/Synthesis/StretchProcessor.cpp Source/Utils/WarpMarkerProcessor.cpp Source/Models/Project.cpp Source/Tests/ProjectCoreTests.cpp
git commit -m "refactor: separate source tunedF0 from stretched final f0"
```

---

### Task 5: Move HNSep Curves To Source Base Curves

**Files:**
- Modify: `Source/Utils/HNSepCurveProcessor.h`
- Modify: `Source/Utils/HNSepCurveProcessor.cpp`
- Modify: `Source/UI/HNSepLaneComponent.cpp`
- Modify: `Source/Audio/Synthesis/StretchProcessor.cpp`
- Modify: `Source/Utils/WarpMarkerProcessor.cpp`
- Modify: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Add failing base-curve inverse mapping test**

In `Source/Tests/ProjectCoreTests.cpp`, add:

```cpp
void testHNSepBaseCurvesStaySourceTimelineDuringStretch()
{
  EditedData data;
  data.basePitch = {60.0f, 60.0f, 60.0f};
  data.deltaPitch = {0.0f, 0.0f, 0.0f};
  data.tunedF0 = {100.0f, 200.0f, 400.0f};
  data.f0 = data.tunedF0;
  data.voicedMask = {true, true, true};
  data.vadMask = {true, true, true};
  data.baseVoicing = {100.0f, 80.0f, 60.0f};
  data.baseBreath = {50.0f, 70.0f, 90.0f};
  data.baseTension = {0.0f, 10.0f, 20.0f};
  data.adjustedMel = {{0.0f}, {10.0f}, {20.0f}};

  const std::vector<Project::WarpMarker> markers = {{0, 0}, {2, 4}};
  StretchProcessor::stretchEditedData(data, markers, 4);

  expectVectorNear(data.baseVoicing, {100.0f, 80.0f, 60.0f}, 0.0001f,
                   "base voicing remains source timeline");
  expectVectorNear(data.baseBreath, {50.0f, 70.0f, 90.0f}, 0.0001f,
                   "base breath remains source timeline");
  expectVectorNear(data.baseTension, {0.0f, 10.0f, 20.0f}, 0.0001f,
                   "base tension remains source timeline");
  require(data.mel.size() == 4, "final mel stretches to output timeline");
  expectVectorNear(data.mel[1], {5.0f}, 0.0001f,
                   "final mel stretches from adjusted source mel");
}
```

Call it after `testStretchEditedData()` in `main()`:

```cpp
  testHNSepBaseCurvesStaySourceTimelineDuringStretch();
```

- [ ] **Step 2: Run tests and confirm failure**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: fails because `stretchEditedData()` currently stretches
`voicingCurve/breathCurve/tensionCurve` and does not stretch `adjustedMel` into
`mel`.

- [ ] **Step 3: Update `StretchProcessor::stretchEditedData()` for HNSep stages**

In `Source/Audio/Synthesis/StretchProcessor.cpp`, remove these lines from
`stretchEditedData()`:

```cpp
  edited.voicingCurve = resampleLinear(edited.voicingCurve);
  edited.breathCurve = resampleLinear(edited.breathCurve);
  edited.tensionCurve = resampleLinear(edited.tensionCurve);
```

Replace them with compatibility output curve generation from source base curves:

```cpp
  edited.voicingCurve =
      resampleLinear(!edited.baseVoicing.empty()
                         ? edited.baseVoicing
                         : edited.voicingCurve);
  edited.breathCurve =
      resampleLinear(!edited.baseBreath.empty()
                         ? edited.baseBreath
                         : edited.breathCurve);
  edited.tensionCurve =
      resampleLinear(!edited.baseTension.empty()
                         ? edited.baseTension
                         : edited.tensionCurve);
```

Then add final mel stretch after final f0 stretch:

```cpp
  if (!edited.adjustedMel.empty())
    edited.mel = buildOutputMel(edited.adjustedMel, markers, newTotalFrames);
```

Do not change `baseVoicing/baseBreath/baseTension` inside this function.

- [ ] **Step 4: Add HNSep base-curve helpers**

In `Source/Utils/HNSepCurveProcessor.h`, add declarations:

```cpp
    void rebuildBaseCurvesFromNotes(Project& project);
    void rebuildBaseCurvesForRange(Project& project, int startFrame, int endFrame);
    void extractNoteCurvesFromBaseCurves(Project& project);
```

In `Source/Utils/HNSepCurveProcessor.cpp`, implement
`rebuildBaseCurvesFromNotes()` by copying the existing
`rebuildCurvesFromNotes()` body and changing destinations:

```cpp
        std::fill(editedData.baseVoicing.begin(), editedData.baseVoicing.end(),
                  kDefaultVoicing);
        std::fill(editedData.baseBreath.begin(), editedData.baseBreath.end(),
                  kDefaultBreath);
        std::fill(editedData.baseTension.begin(), editedData.baseTension.end(),
                  kDefaultTension);
```

and write:

```cpp
            writeCurveRange(editedData.baseVoicing, note.getVoicingCurve(),
                            note.getSrcStartFrame(), note.getSrcEndFrame(),
                            kDefaultVoicing);
            writeCurveRange(editedData.baseBreath, note.getBreathCurve(),
                            note.getSrcStartFrame(), note.getSrcEndFrame(),
                            kDefaultBreath);
            writeCurveRange(editedData.baseTension, note.getTensionCurve(),
                            note.getSrcStartFrame(), note.getSrcEndFrame(),
                            kDefaultTension);
```

Use `note.getSrcStartFrame()/getSrcEndFrame()` because base curves are source
timeline.

- [ ] **Step 5: Update active HNSep commit path**

In `Source/UI/HNSepLaneComponent.cpp`, inside `commitEdits()` replace:

```cpp
        HNSepCurveProcessor::rebuildCurvesForRange(*project, minFrame, maxFrame);
        project->setParamDirtyRange(minFrame, maxFrame);
```

with:

```cpp
        HNSepCurveProcessor::rebuildBaseCurvesForRange(*project, minFrame, maxFrame);
        HNSepCurveProcessor::rebuildCurvesForRange(*project, minFrame, maxFrame);
        project->setParamDirtyRange(minFrame, maxFrame);
```

The first call updates source-timeline base curves. The second keeps existing
output curve drawing compatibility until the UI reads base curves directly.

- [ ] **Step 6: Update HNSep mel recomputation to read base curves**

In `Source/Utils/HNSepCurveProcessor.cpp`, inside `recomputeMelForRange()`,
replace checks for output curves:

```cpp
        if (editedData.voicingCurve.empty() || editedData.breathCurve.empty() ||
            editedData.tensionCurve.empty())
            return;
```

with:

```cpp
        if (editedData.baseVoicing.empty() ||
            editedData.baseBreath.empty() ||
            editedData.baseTension.empty())
            return;
```

When building `srcVoicing`, `srcBreath`, and `srcTension`, slice from
`editedData.baseVoicing/baseBreath/baseTension` over the note source range:

```cpp
            const int sourceStart = note.getSrcStartFrame();
            const int sourceEnd = note.getSrcEndFrame();
            const int sourceFrames = static_cast<int>(editedData.baseVoicing.size());
            const int writeStart = std::max(0, sourceStart);
            const int writeEnd = std::min(sourceEnd, sourceFrames);
            if (writeEnd <= writeStart)
                continue;

            srcVoicing.assign(editedData.baseVoicing.begin() + writeStart,
                              editedData.baseVoicing.begin() + writeEnd);
            srcBreath.assign(editedData.baseBreath.begin() + writeStart,
                             editedData.baseBreath.begin() + writeEnd);
            srcTension.assign(editedData.baseTension.begin() + writeStart,
                              editedData.baseTension.begin() + writeEnd);
```

Then fit each curve to `srcDurationFrames` with `CurveResampler::resampleLinear`
when needed.

- [ ] **Step 7: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `All ProjectCoreTests passed`.

- [ ] **Step 8: Commit**

Run:

```powershell
git add Source/Utils/HNSepCurveProcessor.h Source/Utils/HNSepCurveProcessor.cpp Source/UI/HNSepLaneComponent.cpp Source/Audio/Synthesis/StretchProcessor.cpp Source/Utils/WarpMarkerProcessor.cpp Source/Tests/ProjectCoreTests.cpp
git commit -m "refactor: route HNSep edits through source base curves"
```

---

### Task 6: Final Waveform Isolation

**Files:**
- Modify: `Source/Models/Project.h`
- Modify: `Source/Models/Project.cpp`
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.h`
- Modify: `Source/Audio/Synthesis/IncrementalSynthesizer.cpp`
- Modify: `Source/Audio/AudioEngine.cpp`
- Modify: `Source/Audio/EditorController.cpp`
- Modify: `Source/UI/Main/MainComponent_ProjectIO.cpp`
- Modify: `Source/UI/Main/ExportHelper.cpp`
- Modify: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Replace old blend tests with final-waveform test**

In `Source/Tests/ProjectCoreTests.cpp`, add this include:

```cpp
#include "../Audio/Synthesis/IncrementalSynthesizer.h"
```

Delete `testComposeWaveformFollowsOutputFrameCount()` and remove its call from
`main()`. Project-level final waveform composition is no longer a supported
operation.

Replace
`testBlendSynthesizedRangeIntoAuditionBuffer()` with:

```cpp
void testBlendSynthesizedRangeWritesFinalWaveformOnly()
{
  Project project;
  auto& audioData = project.getAudioData();
  audioData.waveform.setSize(1, 16);
  audioData.finalWaveform.setSize(1, 16);
  for (int i = 0; i < 16; ++i)
  {
    audioData.waveform.setSample(0, i, 1.0f);
    audioData.finalWaveform.setSample(0, i, 1.0f);
  }

  resizeEditedData(project.getEditedData(), 4);
  IncrementalSynthesizer::blendSynthesizedRangeIntoFinalWaveform(
      project, std::vector<float>(8, 5.0f), 1, 3, 4);

  expectNear(audioData.waveform.getSample(0, 8), 1.0f, 0.0001f,
             "source waveform is not overwritten by final blend");
  expectNear(audioData.finalWaveform.getSample(0, 8), 4.0f, 0.0001f,
             "final waveform receives blended synthesized range");
}
```

Rename the remaining blend tests so they call
`IncrementalSynthesizer::blendSynthesizedRangeIntoFinalWaveform()` and assert
`audioData.finalWaveform`, not `audioData.waveform`.

- [ ] **Step 2: Run tests and confirm failure**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: compile fails because
`IncrementalSynthesizer::blendSynthesizedRangeIntoFinalWaveform()` does not
exist.

- [ ] **Step 3: Add final waveform blend helper to `IncrementalSynthesizer`**

In `Source/Audio/Synthesis/IncrementalSynthesizer.h`, add this public static
method after `isSynthesizing()`:

```cpp
  static void blendSynthesizedRangeIntoFinalWaveform(
      Project& project,
      const std::vector<float>& synthesized,
      int startFrame,
      int endFrame,
      int hopSize);
```

In `Source/Audio/Synthesis/IncrementalSynthesizer.cpp`, add the helper above
`synthesizeRegion()`:

```cpp
void IncrementalSynthesizer::blendSynthesizedRangeIntoFinalWaveform(
    Project& project,
    const std::vector<float>& synthesized,
    int startFrame,
    int endFrame,
    int hopSize)
{
  if (synthesized.empty() || hopSize <= 0)
    return;

  const int outputFrames = project.getFrameCount();
  const int requiredFrames = outputFrames > 0
                                 ? std::max(outputFrames, endFrame)
                                 : endFrame;
  const int requiredSamples = std::max(0, requiredFrames * hopSize);
  if (requiredSamples <= 0)
    return;

  auto& audioData = project.getAudioData();
  auto& finalWaveform = audioData.finalWaveform;
  const auto& sourceWaveform = audioData.waveform;
  if (finalWaveform.getNumChannels() == 0)
  {
    if (sourceWaveform.getNumChannels() > 0 &&
        sourceWaveform.getNumSamples() > 0)
      finalWaveform.makeCopyOf(sourceWaveform);
    else if (audioData.originalWaveform.getNumChannels() > 0)
      finalWaveform.makeCopyOf(audioData.originalWaveform);
  }

  const int numChannels = finalWaveform.getNumChannels();
  if (numChannels == 0)
    return;

  if (finalWaveform.getNumSamples() != requiredSamples)
    finalWaveform.setSize(numChannels, requiredSamples, true, true, false);

  const int rawStartSample = startFrame * hopSize;
  const int rawEndSample = endFrame * hopSize;
  const int rangeSamples = rawEndSample - rawStartSample;
  if (rangeSamples <= 0)
    return;

  const int sourceSamples =
      std::min(rangeSamples, static_cast<int>(synthesized.size()));
  const int sourceOffset = std::max(0, -rawStartSample);
  if (sourceOffset >= sourceSamples)
    return;

  const int startSample = std::max(0, rawStartSample);
  const int endSample = std::min(finalWaveform.getNumSamples(), rawEndSample);
  if (endSample <= startSample)
    return;

  const int numSamples = std::min(
      endSample - startSample, sourceSamples - sourceOffset);
  const int fade = std::min(hopSize, sourceSamples / 2);

  for (int ch = 0; ch < numChannels; ++ch)
  {
    float* dst = finalWaveform.getWritePointer(ch, startSample);
    for (int i = 0; i < numSamples; ++i)
    {
      const int sourceIndex = sourceOffset + i;
      float mix = 1.0f;
      if (fade > 0 && sourceIndex < fade)
        mix = static_cast<float>(sourceIndex) / static_cast<float>(fade);
      if (fade > 0 && sourceSamples - 1 - sourceIndex < fade)
        mix = std::min(mix,
                       static_cast<float>(sourceSamples - 1 - sourceIndex) /
                           static_cast<float>(fade));
      const float synth = synthesized[static_cast<size_t>(sourceIndex)];
      dst[i] = dst[i] + mix * (synth - dst[i]);
    }
  }
}
```

Do not copy `finalWaveform` back into `audioData.waveform`.

- [ ] **Step 4: Remove the old Project blend helper**

In `Source/Models/Project.h`, delete the declaration:

```cpp
    void blendSynthesizedRangeIntoAuditionBuffer(
        const std::vector<float>& synthesized,
        int startFrame,
        int endFrame,
        int hopSize);
```

In `Source/Models/Project.cpp`, delete the full
`Project::blendSynthesizedRangeIntoAuditionBuffer(...)` function body.

- [ ] **Step 5: Remove Project global waveform composition**

In `Source/Models/Project.h`, delete the declaration:

```cpp
    void composeGlobalWaveform();
```

In `Source/Models/Project.cpp`, delete the full
`Project::composeGlobalWaveform()` function body. Keep
`renderMappedBaseWaveformSegment()` and `renderMappedSourceSegment()` because
they return source-mapped temporary buffers and do not write final waveform
audio.

- [ ] **Step 6: Update `IncrementalSynthesizer` call site**

In `Source/Audio/Synthesis/IncrementalSynthesizer.cpp`, replace:

```cpp
            capturedProject->blendSynthesizedRangeIntoAuditionBuffer(
                synthesizedAudio, capturedStartFrame, capturedEndFrame, hopSize);
```

with:

```cpp
            IncrementalSynthesizer::blendSynthesizedRangeIntoFinalWaveform(
                *capturedProject, synthesizedAudio, capturedStartFrame,
                capturedEndFrame, hopSize);
```

- [ ] **Step 7: Stop stretch from composing final waveform**

In `Source/Utils/WarpMarkerProcessor.cpp`, remove calls to:

```cpp
    project.composeGlobalWaveform();
```

After stretch recomputation, ensure final synthesis dirty ranges are marked:

```cpp
    project.setF0DirtyRange(0, static_cast<int>(project.getEditedData().f0.size()));
    project.setParamDirtyRange(0, static_cast<int>(project.getEditedData().mel.size()));
```

- [ ] **Step 8: Route playback to final waveform fallback**

In `Source/Audio/EditorController.cpp` and
`Source/UI/Main/MainComponent_ProjectIO.cpp`, replace direct playback loads:

```cpp
engine->loadWaveform(audioData.waveform, audioData.sampleRate);
```

with:

```cpp
const auto& playbackWaveform =
    audioData.finalWaveform.getNumSamples() > 0
        ? audioData.finalWaveform
        : audioData.waveform;
engine->loadWaveform(playbackWaveform, audioData.sampleRate);
```

- [ ] **Step 9: Update export fallback**

In `Source/UI/Main/ExportHelper.cpp`, find the buffer passed to file writing.
Use this selection:

```cpp
const auto& exportWaveform =
    audioData.finalWaveform.getNumSamples() > 0
        ? audioData.finalWaveform
        : audioData.waveform;
```

Write `exportWaveform`, not `audioData.waveform`.

- [ ] **Step 10: Run source write search**

Run:

```powershell
rg -n "waveform\\.makeCopyOf\\(|audioData\\.waveform\\s*=|composeGlobalWaveform\\(|blendSynthesizedRangeIntoAuditionBuffer" Source
```

Expected:

- Import/load code may still assign `audioData.waveform`.
- No stretch/synthesis-output path calls `composeGlobalWaveform()`.
- No code calls `blendSynthesizedRangeIntoAuditionBuffer()`.

- [ ] **Step 11: Run tests**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: `All ProjectCoreTests passed`.

- [ ] **Step 12: Commit**

Run:

```powershell
git add Source
git commit -m "refactor: isolate final waveform writes in synthesis output"
```

---

### Task 7: Refresh UI Caches From Final Global Data

**Files:**
- Modify: `Source/UI/PianoRoll/States/DrawHandler.cpp`
- Modify: `Source/UI/PianoRoll/States/SelectHandler.cpp`
- Modify: `Source/UI/PianoRoll/PitchEditor.cpp`
- Modify: `Source/UI/PianoRollComponent.cpp`
- Modify: `Source/Undo/F0Actions.h`
- Modify: `Source/Undo/DragActions.h`
- Modify: `Source/Undo/ParameterActions.h`
- Modify: `Source/Utils/NoteEditUtils.cpp`
- Modify: `Source/Tests/ProjectCoreTests.cpp`

- [ ] **Step 1: Add cache-refresh test**

In `Source/Tests/ProjectCoreTests.cpp`, add:

```cpp
void testNotePitchCacheReloadsFromFinalF0()
{
  auto project = makeProject();
  auto& edited = project.getEditedData();
  edited.basePitch = {60.0f, 60.0f, 60.0f, 60.0f};
  edited.f0 = {261.63f, 293.66f, 329.63f, 349.23f};

  project.getNotes()[0].setDeltaPitch({9.0f, 9.0f, 9.0f, 9.0f});
  PitchCurveProcessor::refreshNotePitchCachesFromFinalF0(project, 0, 4);

  expectVectorNear(project.getNotes()[0].getDeltaPitch(),
                   {0.0f, 2.0f, 4.0f, 5.0f}, 0.02f,
                   "note display cache reloads from final f0");
}
```

Add include:

```cpp
#include "../Utils/PitchCurveProcessor.h"
```

Call it in `main()` after cache tests:

```cpp
  testNotePitchCacheReloadsFromFinalF0();
```

- [ ] **Step 2: Run tests and confirm failure or pass**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: passes if Task 4 already added the helper correctly. If it fails,
fix `refreshNotePitchCachesFromFinalF0()` before editing UI call sites.

- [ ] **Step 3: Add a shared commit-refresh helper in `PianoRollComponent`**

In `Source/UI/PianoRollComponent.cpp`, add a small local helper near other
pitch edit helpers:

```cpp
void PianoRollComponent::refreshPitchCachesAfterGlobalEdit(int startFrame,
                                                           int endFrame)
{
  if (!project)
    return;
  PitchCurveProcessor::refreshNotePitchCachesFromFinalF0(*project,
                                                         startFrame,
                                                         endFrame);
  project->refreshNoteCachesForRange(startFrame, endFrame);
}
```

Declare it in `Source/UI/PianoRollComponent.h`:

```cpp
  void refreshPitchCachesAfterGlobalEdit(int startFrame, int endFrame);
```

- [ ] **Step 4: Update draw commit**

In `Source/UI/PianoRoll/States/DrawHandler.cpp`, after:

```cpp
  if (owner_.project)
    owner_.project->setF0DirtyRange(rangeStart, rangeEnd);
```

add:

```cpp
  owner_.refreshPitchCachesAfterGlobalEdit(rangeStart, rangeEnd);
```

- [ ] **Step 5: Update undo actions**

In `Source/Undo/F0Actions.h`, after `SnapshotHelper::refreshNoteCache(...)`,
add:

```cpp
    PitchCurveProcessor::refreshNotePitchCachesFromFinalF0(project,
                                                           startFrame,
                                                           endFrame);
```

Add include:

```cpp
#include "../Utils/PitchCurveProcessor.h"
```

In `Source/Undo/DragActions.h`, add the same refresh after each
`SnapshotHelper::refreshNoteCache(project, startFrame, endFrame);`.

- [ ] **Step 6: Update selection and editor commit paths**

In `Source/UI/PianoRoll/States/SelectHandler.cpp`, after calls to
`PitchCurveProcessor::rebuildBaseFromNotes(*project);` in drag/snap commit
paths, add:

```cpp
owner_.refreshPitchCachesAfterGlobalEdit(startFrame, endFrame);
```

Use the local dirty range already computed in each block.

In `Source/UI/PianoRoll/PitchEditor.cpp`, after multi-note drag commit
rebuilds global pitch data, add this before `onPitchEdited` is invoked:

```cpp
  PitchCurveProcessor::refreshNotePitchCachesFromFinalF0(
      *project, multiDragStartFrame, multiDragEndFrame);
  project->refreshNoteCachesForRange(multiDragStartFrame,
                                     multiDragEndFrame);
```

- [ ] **Step 7: Run tests and build UI target**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Debug --target HachiTune --parallel
```

Expected: tests pass and app target builds.

- [ ] **Step 8: Commit**

Run:

```powershell
git add Source
git commit -m "refactor: reload note caches from final global pitch"
```

---

### Task 8: Update Debug Monitor And Final Verification

**Files:**
- Modify: `Source/UI/Debug/ProjectTreeView.cpp`
- Modify: `Source/UI/Debug/TreeValueMonitor.cpp`
- Modify: `Source/UI/Debug/MelViewComponent.cpp`
- Modify: `Source/Tests/ProjectCoreTests.cpp`
- Modify: `docs/superpowers/specs/2026-05-03-strict-staged-pipeline-refactor-design.md`

- [ ] **Step 1: Add validation test for new arrays**

In `Source/Tests/ProjectCoreTests.cpp`, extend `testValidation()`:

```cpp
  project = makeProject();
  project.getEditedData().baseVoicing.pop_back();
  result = project.validateFrameData();
  expect(!result.isValid(), "mismatched baseVoicing fails validation");

  project = makeProject();
  project.getEditedData().mel.push_back({1.0f, 2.0f, 3.0f});
  result = project.validateFrameData();
  expect(!result.isValid(), "ragged edited mel fails validation");
```

- [ ] **Step 2: Run tests and confirm validation failure**

Run:

```powershell
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Expected: second assertion fails until `validateFrameData()` checks mel
matrices and base curves.

- [ ] **Step 3: Update monitor rows**

In `Source/UI/Debug/ProjectTreeView.cpp`, add rows in the edited data section:

```cpp
rows.push_back("tunedF0 size: " +
               juce::String(static_cast<int>(ed.tunedF0.size())));
rows.push_back("baseVoicing size: " +
               juce::String(static_cast<int>(ed.baseVoicing.size())));
rows.push_back("baseBreath size: " +
               juce::String(static_cast<int>(ed.baseBreath.size())));
rows.push_back("baseTension size: " +
               juce::String(static_cast<int>(ed.baseTension.size())));
rows.push_back("adjustedMel: " + formatMatrixSize(ed.adjustedMel));
rows.push_back("mel: " + formatMatrixSize(ed.mel));
```

Add rows in the analysis/audio section:

```cpp
rows.push_back("analysis.originalMel: " +
               formatMatrixSize(project.getAnalysisData().originalMel));
rows.push_back("audio.finalWaveform samples: " +
               juce::String(audioData.finalWaveform.getNumSamples()));
```

- [ ] **Step 4: Ensure monitor refreshes on synthesis completion**

In `Source/UI/Debug/TreeValueMonitor.cpp`, verify the existing listener case
for `ProjectChangeType::SynthesisComplete` keeps these three calls:

```cpp
case ProjectChangeType::SynthesisComplete:
  safeThis->content.treeView.refresh();
  safeThis->content.melView.rebuildMelImage();
  safeThis->content.melView.rebuildF0Path();
  break;
```

- [ ] **Step 5: Update the spec status note**

Append this section to
`docs/superpowers/specs/2026-05-03-strict-staged-pipeline-refactor-design.md`:

```markdown
## Implementation Notes

Implementation is tracked by
`docs/superpowers/plans/2026-05-03-strict-staged-pipeline-refactor.md`.
Large mel/STFT matrices are in-memory pipeline fields and are recomputed after
load in the initial implementation.
```

- [ ] **Step 6: Final verification**

Run:

```powershell
rg -n "sourceMelSpectrogram|melSpectrogram|blendSynthesizedRangeIntoAuditionBuffer" Source
cmake --build build --config Debug --target HachiTuneCoreTests --parallel
ctest --test-dir build -C Debug --output-on-failure
cmake --build build --config Debug --target HachiTune --parallel
```

Expected:

- `rg` prints no matches for old mel/blend names.
- `ctest` prints `All ProjectCoreTests passed`.
- `HachiTune` app target builds successfully.

- [ ] **Step 7: Commit**

Run:

```powershell
git add Source docs/superpowers/specs/2026-05-03-strict-staged-pipeline-refactor-design.md
git commit -m "test: verify strict staged pipeline invariants"
```

---

## Execution Notes

Use one task per branch commit. If a task touches many call sites, run `rg`
before editing and again after editing. The old-owner search commands are part
of the acceptance criteria and are required cleanup.

Do not remove `Note::synthWaveform` in this plan. Treat it as a temporary cache
until final waveform isolation is complete and playback/export no longer
depends on it.

Do not persist large mel/STFT matrices in JSON during this plan. Save compact
edit state and recompute large derived data after load.
