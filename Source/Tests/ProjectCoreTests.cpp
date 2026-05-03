#include "../JuceHeader.h"
#include "../Audio/TensionProcessor.h"
#include "../Audio/Synthesis/StretchProcessor.h"
#include "../Models/Project.h"
#include "../Models/ProjectSerializer.h"
#include "../Utils/Constants.h"
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

bool markersEqual(const std::vector<Project::WarpMarker>& a,
                  const std::vector<Project::WarpMarker>& b)
{
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); ++i)
  {
    if (a[i].sourceFrame != b[i].sourceFrame ||
        a[i].outputFrame != b[i].outputFrame)
    {
      return false;
    }
  }
  return true;
}

void expectNear(float actual, float expected, float tolerance,
                const char* message)
{
  expect(std::isfinite(actual) &&
             std::isfinite(expected) &&
             std::abs(actual - expected) <= tolerance,
         message);
}

void expectVectorNear(const std::vector<float>& actual,
                      const std::vector<float>& expected,
                      float tolerance,
                      const char* message)
{
  if (!require(actual.size() == expected.size(), message))
    return;

  for (size_t i = 0; i < actual.size(); ++i)
  {
    if (!std::isfinite(actual[i]) ||
        !std::isfinite(expected[i]) ||
        std::abs(actual[i] - expected[i]) > tolerance)
    {
      ++failures;
      std::cerr << "FAIL: " << message << " at " << i << "\n";
      return;
    }
  }
}

void expectMelNear(const std::vector<std::vector<float>>& actual,
                   const std::vector<std::vector<float>>& expected,
                   float tolerance,
                   const char* message)
{
  if (!require(actual.size() == expected.size(), message))
    return;

  for (size_t i = 0; i < actual.size(); ++i)
  {
    expectVectorNear(actual[i], expected[i], tolerance, message);
  }
}

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

Project makeProject()
{
  Project project;
  project.setName("CoreTest");
  project.getAudioData().sampleRate = 44100;
  const std::vector<std::vector<float>> originalMel = {
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
  analysis.originalMel = originalMel;
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
  edited.adjustedMel = originalMel;
  edited.mel = originalMel;
  edited.tunedF0 = edited.f0;
  edited.baseVoicing = edited.voicingCurve;
  edited.baseBreath = edited.breathCurve;
  edited.baseTension = edited.tensionCurve;

  project.refreshNoteCaches();
  return project;
}

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
                      juce::Identifier("f0" "EditedMask")),
         "removed edited mask remains absent");

  project = makeProject();
  project.getNotes()[0].setHighPassFilterStrength(0.25f);
  project.getNotes()[0].setLowPassFilterStrength(-0.5f);
  const auto filterJson = ProjectSerializer::toJson(project);
  const auto filterNotes = filterJson.getProperty("notes", juce::var());
  require(filterNotes.isArray(), "filter notes is an array");
  require(filterNotes.size() == 1, "one filter note serialized");
  expect(hasProperty(filterNotes[0], "highPassFilterStrength"),
         "non-zero highPassFilterStrength is saved");
  expect(hasProperty(filterNotes[0], "lowPassFilterStrength"),
         "non-zero lowPassFilterStrength is saved");
}

void testSerializerRestoresSourceRangesFromAnalysisSegments()
{
  auto* root = new juce::DynamicObject();
  root->setProperty("name", "SourceRangeLoad");
  root->setProperty("sampleRate", 44100);

  juce::Array<juce::var> notes;
  auto* note = new juce::DynamicObject();
  note->setProperty("startFrame", 10);
  note->setProperty("endFrame", 20);
  note->setProperty("srcStartFrame", 100);
  note->setProperty("srcEndFrame", 120);
  note->setProperty("midiNote", 60.0);
  note->setProperty("rest", false);
  notes.add(juce::var(note));
  root->setProperty("notes", notes);

  auto* analysis = new juce::DynamicObject();
  analysis->setProperty("originalF0", "100 110 120 130");
  analysis->setProperty("originalPitch", "60 61 62 63");
  analysis->setProperty("originalDeltaPitch", "0 0.1 0.2 0.3");
  analysis->setProperty("originalVoicedMask", "1111");
  analysis->setProperty("originalVADMask", "1111");
  juce::var segments;
  auto* segment = new juce::DynamicObject();
  segment->setProperty("srcStartFrame", 2);
  segment->setProperty("srcEndFrame", 6);
  segments.append(juce::var(segment));
  analysis->setProperty("noteSegments", segments);
  root->setProperty("analysisData", juce::var(analysis));

  auto* edited = new juce::DynamicObject();
  edited->setProperty("basePitch", "60 60 60 60");
  edited->setProperty("deltaPitch", "0 0 0 0");
  edited->setProperty("f0", "100 110 120 130");
  edited->setProperty("voicedMask", "1111");
  edited->setProperty("vadMask", "1111");
  root->setProperty("editedData", juce::var(edited));

  Project project = makeProject();
  require(ProjectSerializer::fromJson(project, juce::var(root)),
          "project loads from source range json");
  require(project.getNotes().size() == 1, "source range project has one note");
  expect(project.getAnalysisData().noteSegments.size() == 1,
         "analysis note segments are replaced on load");
  expect(project.getNotes()[0].getSrcStartFrame() == 2,
         "source start restored from analysis segment");
  expect(project.getNotes()[0].getSrcEndFrame() == 6,
         "source end restored from analysis segment");
}

void testLegacyLoadClearsAnalysisSegments()
{
  auto* root = new juce::DynamicObject();
  root->setProperty("name", "LegacySourceRangeLoad");
  root->setProperty("sampleRate", 44100);

  juce::Array<juce::var> notes;
  auto* note = new juce::DynamicObject();
  note->setProperty("startFrame", 10);
  note->setProperty("endFrame", 20);
  note->setProperty("srcStartFrame", 100);
  note->setProperty("srcEndFrame", 120);
  note->setProperty("midiNote", 60.0);
  note->setProperty("rest", false);
  notes.add(juce::var(note));
  root->setProperty("notes", notes);

  auto* pitchData = new juce::DynamicObject();
  pitchData->setProperty("f0", "100 110 120 130");
  pitchData->setProperty("basePitch", "60 60 60 60");
  pitchData->setProperty("deltaPitch", "0 0 0 0");
  pitchData->setProperty("voicingCurve", "100 100 100 100");
  pitchData->setProperty("breathCurve", "100 100 100 100");
  pitchData->setProperty("tensionCurve", "0 0 0 0");
  pitchData->setProperty("voicedMask", "1111");
  pitchData->setProperty("vadMask", "1111");
  root->setProperty("pitchData", juce::var(pitchData));

  Project project = makeProject();
  require(ProjectSerializer::fromJson(project, juce::var(root)),
          "legacy project loads from source range json");
  require(project.getNotes().size() == 1, "legacy source range project has one note");
  expect(project.getAnalysisData().noteSegments.empty(),
         "legacy load clears stale analysis note segments");
  expect(project.getNotes()[0].getSrcStartFrame() == 100,
         "legacy source start remains from note json");
  expect(project.getNotes()[0].getSrcEndFrame() == 120,
         "legacy source end remains from note json");
}

void testLoadWithoutPitchPayloadClearsProjectData()
{
  auto* root = new juce::DynamicObject();
  root->setProperty("name", "NoPitchPayloadLoad");
  root->setProperty("sampleRate", 44100);

  juce::Array<juce::var> notes;
  auto* note = new juce::DynamicObject();
  note->setProperty("startFrame", 10);
  note->setProperty("endFrame", 20);
  note->setProperty("srcStartFrame", 100);
  note->setProperty("srcEndFrame", 120);
  note->setProperty("midiNote", 60.0);
  note->setProperty("rest", false);
  notes.add(juce::var(note));
  root->setProperty("notes", notes);

  Project project = makeProject();
  require(ProjectSerializer::fromJson(project, juce::var(root)),
          "project loads without pitch payload");
  require(project.getNotes().size() == 1, "no-payload project has one note");
  expect(project.getAnalysisData().noteSegments.empty(),
         "no-payload load clears stale analysis note segments");
  expect(project.getEditedData().f0.empty(),
         "no-payload load clears stale edited f0");
  expect(project.getEditedData().deltaPitch.empty(),
         "no-payload load clears stale edited delta pitch");
  expect(project.getNotes()[0].getSrcStartFrame() == 100,
         "no-payload source start remains from note json");
  expect(project.getNotes()[0].getSrcEndFrame() == 120,
         "no-payload source end remains from note json");
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

  project = makeProject();
  project.getEditedData().f0.clear();
  result = project.validateFrameData();
  expect(!result.isValid(), "empty editedData.f0 with other arrays fails validation");

  project = makeProject();
  project.getAnalysisData().noteSegments.clear();
  result = project.validateFrameData();
  expect(!result.isValid(), "missing analyzed note segments fails validation");

  project = makeProject();
  project.getAnalysisData().originalPitch.clear();
  result = project.validateFrameData();
  expect(!result.isValid(), "missing required analysis arrays fail validation");

  project = makeProject();
  project.getAnalysisData().clear();
  project.getNotes()[0].setSrcStartFrame(-1);
  project.getNotes()[0].setSrcEndFrame(0);
  result = project.validateFrameData();
  expect(!result.isValid(), "invalid source range fails without analysis frames");
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

void testBuildOutputMelUsesRequestedFrameCount()
{
  const std::vector<std::vector<float>> sourceMel = {
      {0.0f, 1.0f}, {10.0f, 11.0f}, {20.0f, 21.0f}};
  const std::vector<Project::WarpMarker> markers = {{0, 0}, {2, 3}};

  const auto output =
      StretchProcessor::buildOutputMel(sourceMel, markers, 5);

  if (!require(output.size() == 5, "output mel uses requested frame count"))
    return;
  if (!require(output[0].size() == 2, "output mel preserves bin count"))
    return;
  expectVectorNear(output[0], {0.0f, 1.0f}, 0.0001f,
                   "output mel starts from source");
  expectVectorNear(output[3], {0.0f, 0.0f}, 0.0001f,
                   "output mel pads extra frames with zeros");
  expectVectorNear(output[4], {0.0f, 0.0f}, 0.0001f,
                   "output mel pads final extra frame with zeros");

  const auto noMapOutput =
      StretchProcessor::buildOutputMel(sourceMel, {}, 5);
  if (!require(noMapOutput.size() == 5,
               "output mel honors requested frame count without markers"))
    return;
  expectVectorNear(noMapOutput[0], {0.0f, 1.0f}, 0.0001f,
                   "no-map output keeps source frame");
  expectVectorNear(noMapOutput[3], {0.0f, 0.0f}, 0.0001f,
                   "no-map output pads extra frame");
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

void testRecomputeFromMarkersBuildsMelFromSourceCache()
{
  auto project = makeProject();
  const std::vector<std::vector<float>> sourceMel = {
      {0.0f}, {10.0f}, {20.0f}, {30.0f}};
  const std::vector<Project::WarpMarker> current = {
      {0, 0}, {2, 3}, {4, 5}};
  const std::vector<Project::WarpMarker> target = {
      {0, 0}, {2, 4}, {4, 6}};

  project.getAudioData().sourceMelSpectrogram = sourceMel;
  project.getAudioData().melSpectrogram.assign(5, {999.0f});

  WarpMarkerProcessor::recomputeFromMarkers(project, current, target, false);

  const auto targetMap =
      WarpMarkerProcessor::buildWarpMapWithEndpoints(project, target);
  const auto expected =
      StretchProcessor::buildOutputMel(sourceMel, targetMap,
                                       static_cast<int>(
                                           project.getEditedData().f0.size()));
  expectMelNear(project.getAudioData().melSpectrogram, expected, 0.0001f,
                "recompute rebuilds output mel from source cache");
  expectMelNear(project.getAudioData().sourceMelSpectrogram, sourceMel,
                0.0001f, "recompute preserves source mel cache");
  expect(project.getWarpMarkers().empty(),
         "source-mel recompute preview does not commit project markers");
}

void testNormalizePreservesEndpointOutputLength()
{
  auto project = makeProject();
  project.getEditedData().f0.resize(6, 100.0f);

  const auto markers = WarpMarkerProcessor::normalizeMarkers(
      project, {{0, 0}, {2, 4}, {4, 6}});

  require(markers.size() == 3, "normalization keeps endpoint map");
  expect(markers.front().sourceFrame == 0, "normalized map starts at source 0");
  expect(markers.front().outputFrame == 0, "normalized map starts at output 0");
  expect(markers[1].sourceFrame == 2, "normalized map keeps interior source");
  expect(markers[1].outputFrame == 4, "normalized map keeps stretched output");
  expect(markers.back().sourceFrame == 4, "normalized map keeps source end");
  expect(markers.back().outputFrame == 6, "normalized map keeps output end");
}

void testRecomputeFromMarkersIsIdempotent()
{
  auto project = makeProject();
  const std::vector<Project::WarpMarker> target = {{0, 0}, {2, 3}, {4, 6}};

  WarpMarkerProcessor::recomputeFromMarkers(project, target, true);
  const auto firstBasePitch = project.getEditedData().basePitch;
  const auto firstDeltaPitch = project.getEditedData().deltaPitch;
  const auto firstMel = project.getAudioData().melSpectrogram;
  const auto firstMarkers = project.getWarpMarkers();

  WarpMarkerProcessor::recomputeFromMarkers(project, target, true);

  expectVectorNear(project.getEditedData().basePitch, firstBasePitch, 0.0001f,
                   "recompute does not restretch basePitch");
  expectVectorNear(project.getEditedData().deltaPitch, firstDeltaPitch, 0.0001f,
                   "recompute does not restretch deltaPitch");
  expectVectorNear(project.getNotes().front().getOriginalDeltaPitch(),
                   std::vector<float>{0.0f, 0.1f, 0.2f, 0.3f},
                   0.0001f,
                   "recompute preserves source original delta cache");
  expect(project.getNotes().front().getDeltaPitch().size() == 6,
         "recompute refreshes output delta cache to stretched length");
  require(project.getAudioData().melSpectrogram.size() == firstMel.size(),
          "recompute does not change mel length");
  if (!project.getAudioData().melSpectrogram.empty() && !firstMel.empty())
  {
    expectVectorNear(project.getAudioData().melSpectrogram[1], firstMel[1],
                     0.0001f, "recompute does not restretch mel");
  }
  expect(markersEqual(project.getWarpMarkers(), firstMarkers),
         "recompute keeps target markers stable");
}

void testPreviewRecomputeCanAdvanceAndCancel()
{
  auto directProject = makeProject();
  auto previewProject = makeProject();
  const std::vector<Project::WarpMarker> original =
      {{0, 0}, {4, 4}};
  const std::vector<Project::WarpMarker> previewA =
      {{0, 0}, {2, 3}, {4, 5}};
  const std::vector<Project::WarpMarker> previewB =
      {{0, 0}, {2, 4}, {4, 6}};

  WarpMarkerProcessor::recomputeFromMarkers(directProject, previewB, false);

  auto currentMarkers = original;
  WarpMarkerProcessor::recomputeFromMarkers(previewProject, currentMarkers,
                                            previewA, false);
  currentMarkers =
      WarpMarkerProcessor::buildWarpMapWithEndpoints(previewProject, previewA);
  WarpMarkerProcessor::recomputeFromMarkers(previewProject, currentMarkers,
                                            previewB, false);
  currentMarkers =
      WarpMarkerProcessor::buildWarpMapWithEndpoints(previewProject, previewB);

  expectVectorNear(previewProject.getEditedData().deltaPitch,
                   directProject.getEditedData().deltaPitch,
                   0.0001f,
                   "preview recompute advances from previous preview map");
  expectVectorNear(previewProject.getNotes().front().getOriginalDeltaPitch(),
                   std::vector<float>{0.0f, 0.1f, 0.2f, 0.3f},
                   0.0001f,
                   "preview recompute preserves source original delta cache");
  expect(previewProject.getWarpMarkers().empty(),
         "preview recompute does not commit project markers");

  WarpMarkerProcessor::recomputeFromMarkers(previewProject, currentMarkers,
                                            original, false);
  expect(previewProject.getEditedData().deltaPitch.size() == 4,
         "preview cancel returns edited data to original length");
  expect(markersEqual(WarpMarkerProcessor::buildWarpMapWithEndpoints(
                          previewProject, previewProject.getWarpMarkers()),
                      original),
         "preview cancel keeps project markers on original map");
}

void testRefreshNoteCachesUsesNonRestAnalysisSegments()
{
  Project project;
  project.getAudioData().sampleRate = 44100;

  Note rest(0, 1, 60.0f);
  rest.setRest(true);
  project.addNote(std::move(rest));

  Note note(1, 3, 60.0f);
  note.setSrcStartFrame(1);
  note.setSrcEndFrame(3);
  project.addNote(std::move(note));

  auto& analysis = project.getAnalysisData();
  analysis.originalF0 = {100.0f, 110.0f, 120.0f, 130.0f};
  analysis.originalPitch = {50.0f, 51.0f, 52.0f, 53.0f};
  analysis.originalDeltaPitch = {0.0f, 1.0f, 2.0f, 3.0f};
  analysis.originalVoicedMask = {true, true, true, true};
  analysis.originalVADMask = {true, true, true, true};
  analysis.noteSegments.push_back({1, 3});

  resizeEditedData(project.getEditedData(), 3);
  project.refreshNoteCaches();

  const auto& refreshedNote = project.getNotes()[1];
  expect(refreshedNote.getSrcStartFrame() == 1,
         "refresh caches uses first non-rest source start");
  expect(refreshedNote.getSrcEndFrame() == 3,
         "refresh caches uses first non-rest source end");
  expectVectorNear(refreshedNote.getOriginalDeltaPitch(),
                   std::vector<float>{1.0f, 2.0f},
                   0.0001f,
                   "refresh caches slices original delta from non-rest segment");
}

void testComposeWaveformFollowsOutputFrameCount()
{
  auto project = makeProject();
  constexpr int sourceFrames = 4;
  auto& audioData = project.getAudioData();
  audioData.originalWaveform.setSize(1, sourceFrames * HOP_SIZE);
  audioData.waveform.setSize(1, sourceFrames * HOP_SIZE);
  for (int i = 0; i < sourceFrames * HOP_SIZE; ++i)
  {
    audioData.originalWaveform.setSample(0, i,
                                         static_cast<float>(i % 97) / 97.0f);
  }

  resizeEditedData(project.getEditedData(), 6);
  project.getNotes().front().setStartFrame(0);
  project.getNotes().front().setEndFrame(6);
  project.composeGlobalWaveform();
  expect(audioData.waveform.getNumSamples() == 6 * HOP_SIZE,
         "compose waveform grows to warped endpoint");

  resizeEditedData(project.getEditedData(), 3);
  project.getNotes().front().setEndFrame(3);
  project.composeGlobalWaveform();
  expect(audioData.waveform.getNumSamples() == 3 * HOP_SIZE,
         "compose waveform shrinks to warped endpoint");
}

void testBlendSynthesizedRangeIntoAuditionBuffer()
{
  Project project;
  auto& audioData = project.getAudioData();
  audioData.originalWaveform.setSize(1, 16);
  audioData.waveform.setSize(1, 16);
  for (int i = 0; i < 16; ++i)
  {
    audioData.originalWaveform.setSample(0, i, 1.0f);
    audioData.waveform.setSample(0, i, 1.0f);
  }

  resizeEditedData(project.getEditedData(), 4);
  project.blendSynthesizedRangeIntoAuditionBuffer(
      std::vector<float>(8, 5.0f), 1, 3, 4);

  const auto& audition = project.getAuditionBuffer();
  require(audition.getNumSamples() == 16, "blend initializes audition buffer");
  expectNear(audition.getSample(0, 3), 1.0f, 0.0001f,
             "blend leaves samples before range unchanged");
  expectNear(audition.getSample(0, 4), 1.0f, 0.0001f,
             "blend fades in from existing sample");
  expectNear(audition.getSample(0, 5), 2.0f, 0.0001f,
             "blend applies fade-in mix");
  expectNear(audition.getSample(0, 7), 4.0f, 0.0001f,
             "blend reaches high mix before center");
  expectNear(audition.getSample(0, 8), 4.0f, 0.0001f,
             "blend fades out after center");
  expectNear(audition.getSample(0, 10), 2.0f, 0.0001f,
             "blend applies fade-out mix");
  expectNear(audition.getSample(0, 11), 1.0f, 0.0001f,
             "blend fades out to existing sample");
  expectNear(audition.getSample(0, 12), 1.0f, 0.0001f,
             "blend leaves samples after range unchanged");
  expectNear(audioData.waveform.getSample(0, 8), audition.getSample(0, 8),
             0.0001f, "blend syncs audio waveform from audition buffer");
}

void testBlendSynthesizedRangeResizesToOutputDuration()
{
  Project project;
  auto& audioData = project.getAudioData();
  audioData.originalWaveform.setSize(1, 8);
  audioData.waveform.setSize(1, 8);
  for (int i = 0; i < 8; ++i)
  {
    audioData.originalWaveform.setSample(0, i, 1.0f);
    audioData.waveform.setSample(0, i, 1.0f);
  }

  resizeEditedData(project.getEditedData(), 4);
  project.blendSynthesizedRangeIntoAuditionBuffer(
      std::vector<float>(16, 5.0f), 0, 4, 4);

  expect(project.getAuditionBuffer().getNumSamples() == 16,
         "blend grows audition buffer to output duration");
  expect(audioData.waveform.getNumSamples() == 16,
         "blend grows audio waveform to output duration");

  resizeEditedData(project.getEditedData(), 2);
  project.blendSynthesizedRangeIntoAuditionBuffer(
      std::vector<float>(8, 2.0f), 0, 2, 4);

  expect(project.getAuditionBuffer().getNumSamples() == 8,
         "blend shrinks audition buffer to output duration");
  expect(audioData.waveform.getNumSamples() == 8,
         "blend removes stale waveform tail after shrink");
}

void testBlendSynthesizedRangeWritesAllChannels()
{
  Project project;
  auto& audioData = project.getAudioData();
  audioData.originalWaveform.setSize(2, 12);
  audioData.waveform.setSize(2, 12);
  for (int i = 0; i < 12; ++i)
  {
    audioData.originalWaveform.setSample(0, i, 1.0f);
    audioData.originalWaveform.setSample(1, i, 2.0f);
    audioData.waveform.setSample(0, i, 1.0f);
    audioData.waveform.setSample(1, i, 2.0f);
  }

  resizeEditedData(project.getEditedData(), 3);
  project.blendSynthesizedRangeIntoAuditionBuffer(
      std::vector<float>(12, 5.0f), 0, 3, 4);

  const auto& audition = project.getAuditionBuffer();
  expectNear(audition.getSample(0, 4), 5.0f, 0.0001f,
             "blend writes synthesized audio to channel 0");
  expectNear(audition.getSample(1, 4), 5.0f, 0.0001f,
             "blend duplicates synthesized audio to channel 1");
  expectNear(audioData.waveform.getSample(1, 4), 5.0f, 0.0001f,
             "blend syncs all channels to audio waveform");
}

void testBlendSynthesizedRangeOffsetsClampedNegativeStart()
{
  Project project;
  auto& audioData = project.getAudioData();
  audioData.originalWaveform.setSize(1, 4);
  audioData.waveform.setSize(1, 4);
  for (int i = 0; i < 4; ++i)
  {
    audioData.originalWaveform.setSample(0, i, 1.0f);
    audioData.waveform.setSample(0, i, 1.0f);
  }

  resizeEditedData(project.getEditedData(), 1);
  project.blendSynthesizedRangeIntoAuditionBuffer(
      {10.0f, 20.0f, 30.0f, 40.0f, 50.0f, 60.0f, 70.0f, 80.0f},
      -1, 1, 4);

  const auto& audition = project.getAuditionBuffer();
  expectNear(audition.getSample(0, 0), 37.75f, 0.0001f,
             "negative start skips offscreen synthesized samples");
  expectNear(audition.getSample(0, 1), 30.5f, 0.0001f,
             "negative start preserves fade alignment");
  expectNear(audition.getSample(0, 3), 1.0f, 0.0001f,
             "negative start uses aligned fade-out sample");
}

void testBlendUsesCurrentWaveformWhenAuditionBufferIsStale()
{
  Project project;
  auto& audioData = project.getAudioData();
  audioData.originalWaveform.setSize(1, 8);
  audioData.waveform.setSize(1, 8);
  project.getAuditionBuffer().setSize(1, 8);
  for (int i = 0; i < 8; ++i)
  {
    audioData.originalWaveform.setSample(0, i, 1.0f);
    audioData.waveform.setSample(0, i, 2.0f);
    project.getAuditionBuffer().setSample(0, i, 99.0f);
  }

  resizeEditedData(project.getEditedData(), 2);
  project.blendSynthesizedRangeIntoAuditionBuffer(
      std::vector<float>(4, 6.0f), 1, 2, 4);

  expectNear(project.getAuditionBuffer().getSample(0, 0), 2.0f, 0.0001f,
             "blend baseline uses current waveform before range");
  expectNear(audioData.waveform.getSample(0, 0), 2.0f, 0.0001f,
             "blend does not copy stale audition data to waveform");
}

void testApplyNoteVolumeToSynthesizedRangeBeforeBlend()
{
  Project project;
  auto& audioData = project.getAudioData();
  audioData.originalWaveform.setSize(1, 12);
  audioData.waveform.setSize(1, 12);
  for (int i = 0; i < 12; ++i)
  {
    audioData.originalWaveform.setSample(0, i, 1.0f);
    audioData.waveform.setSample(0, i, 1.0f);
  }

  Note note(1, 2, 60.0f);
  note.setVolumeDb(-6.0206f);
  project.addNote(std::move(note));
  resizeEditedData(project.getEditedData(), 3);

  std::vector<float> synthesized(12, 5.0f);
  project.applyNoteVolumeToSynthesizedRange(synthesized, 0, 3, 4);
  expectNear(synthesized[3], 5.0f, 0.0001f,
             "note volume leaves non-overlapping sample unchanged");
  expectNear(synthesized[4], 2.5f, 0.0001f,
             "note volume scales overlapping synthesized sample");

  project.blendSynthesizedRangeIntoAuditionBuffer(synthesized, 0, 3, 4);

  expectNear(project.getAuditionBuffer().getSample(0, 4), 2.5f, 0.0001f,
             "note volume affects direct blended amplitude");
}

void testClearSynthesisDirtyForRangeOnlyClearsOverlappingSynthDirty()
{
  Project project;
  Note dirtyNote(0, 4, 60.0f);
  dirtyNote.setDirty(true);
  dirtyNote.setSynthDirty(true);
  project.addNote(std::move(dirtyNote));

  Note outsideNote(5, 6, 60.0f);
  outsideNote.setSynthDirty(true);
  project.addNote(std::move(outsideNote));

  project.clearSynthesisDirtyForRange(0, 4);

  expect(!project.getNotes()[0].isSynthDirty(),
         "synthesis cleanup clears overlapping synthDirty");
  expect(project.getNotes()[0].isDirty(),
         "synthesis cleanup preserves display dirty flag");
  expect(project.getNotes()[1].isSynthDirty(),
         "synthesis cleanup leaves non-overlapping synthDirty");
}

void testCompletedSynthesisDirtyCleanupUsesCapturedSnapshot()
{
  Project project;
  Note note(0, 4, 60.0f);
  note.setDirty(true);
  note.setSynthDirty(true);
  project.addNote(std::move(note));
  project.setF0DirtyRange(0, 4);
  project.setParamDirtyRange(0, 4);

  auto snapshot = project.captureDirtyStateSnapshotForRange(0, 4);
  project.clearDirtyStateForCompletedSynthesis(snapshot);

  expect(!project.getNotes()[0].isDirty(),
         "snapshot cleanup clears consumed note dirty");
  expect(!project.getNotes()[0].isSynthDirty(),
         "snapshot cleanup clears consumed synthDirty");
  expect(!project.hasF0DirtyRange(),
         "snapshot cleanup clears consumed f0 range");
  expect(!project.hasParamDirtyRange(),
         "snapshot cleanup clears consumed param range");

  project.getNotes()[0].setDirty(true);
  project.getNotes()[0].setSynthDirty(true);
  project.setF0DirtyRange(0, 4);
  project.setParamDirtyRange(0, 4);
  snapshot = project.captureDirtyStateSnapshotForRange(0, 4);

  project.getNotes()[0].markDirty();
  project.getNotes()[0].markSynthDirty();
  project.setF0DirtyRange(0, 4);
  project.setParamDirtyRange(0, 4);

  project.clearDirtyStateForCompletedSynthesis(snapshot);

  expect(project.getNotes()[0].isDirty(),
         "snapshot cleanup preserves newer note dirty");
  expect(project.getNotes()[0].isSynthDirty(),
         "snapshot cleanup preserves newer synthDirty");
  expect(project.hasF0DirtyRange(),
         "snapshot cleanup preserves newer f0 dirty range");
  expect(project.hasParamDirtyRange(),
         "snapshot cleanup preserves newer param dirty range");
}

void testDirtySnapshotDoesNotClearReplacementNoteAtSameIndex()
{
  Project project;
  Note original(0, 4, 60.0f);
  original.setSrcStartFrame(0);
  original.setSrcEndFrame(4);
  original.setDirty(true);
  original.setSynthDirty(true);
  project.addNote(std::move(original));

  const auto snapshot = project.captureDirtyStateSnapshotForRange(0, 4);

  Note replacement(10, 14, 60.0f);
  replacement.setSrcStartFrame(10);
  replacement.setSrcEndFrame(14);
  replacement.setDirty(true);
  replacement.setSynthDirty(true);
  project.getNotes()[0] = std::move(replacement);

  project.clearDirtyStateForCompletedSynthesis(snapshot);

  expect(project.getNotes()[0].isDirty(),
         "snapshot cleanup preserves replacement note dirty");
  expect(project.getNotes()[0].isSynthDirty(),
         "snapshot cleanup preserves replacement note synthDirty");
}

void testTensionProcessorReturnsSeparateHarmonicAndNoise()
{
  const std::vector<float> harmonic = {1.0f, -2.0f, 3.0f, -4.0f};
  const std::vector<float> noise = {10.0f, -20.0f, 30.0f, -40.0f};
  const float voicing[] = {50.0f};
  const float breath[] = {25.0f};
  const float tension[] = {0.0f};

  const TensionProcessor processor;
  const auto result = processor.processSegmentHN(
      harmonic.data(), noise.data(), static_cast<int>(harmonic.size()),
      voicing, breath, tension, 1);

  expectVectorNear(result.harmonic, {0.5f, -1.0f, 1.5f, -2.0f},
                   0.0001f,
                   "tension processor scales harmonic separately");
  expectVectorNear(result.noise, {2.5f, -5.0f, 7.5f, -10.0f},
                   0.0001f,
                   "tension processor scales noise separately");
}

void testTensionProcessorComputesSTFTCache()
{
  juce::AudioBuffer<float> buffer(1, HOP_SIZE + 1);
  buffer.clear();
  buffer.setSample(0, HOP_SIZE - 1, 1.0f);

  const auto stft = TensionProcessor::computeSTFT(buffer);
  const int expectedFrames = 2;
  const int expectedBins = 1025;
  expect(stft.size() ==
             static_cast<size_t>(expectedFrames * expectedBins * 2),
         "tension processor STFT cache has expected dimensions");
  const int windowIndex = 2048 / 2 + HOP_SIZE - 1;
  const float expectedDc =
      static_cast<float>(0.5 * (1.0 - std::cos(
          juce::MathConstants<double>::twoPi * windowIndex / 2048.0)));
  expectNear(stft[0], expectedDc, 0.0001f,
             "tension processor STFT uses periodic Hann window");
  expect(TensionProcessor::computeSTFT(juce::AudioBuffer<float>{}).empty(),
         "tension processor STFT cache is empty for empty input");
}
} // namespace

int main()
{
  testPipelineOwnershipFields();
  testSerializerSavesCompactPipelineState();
  testSerializerLoadsCompactPipelineState();
  testSerializerOmitsNoteCaches();
  testSerializerRestoresSourceRangesFromAnalysisSegments();
  testLegacyLoadClearsAnalysisSegments();
  testLoadWithoutPitchPayloadClearsProjectData();
  testValidation();
  testStretchEditedData();
  testBuildOutputMelUsesRequestedFrameCount();
  testWarpEndpoints();
  testRecomputeFromMarkersBuildsMelFromSourceCache();
  testNormalizePreservesEndpointOutputLength();
  testRecomputeFromMarkersIsIdempotent();
  testPreviewRecomputeCanAdvanceAndCancel();
  testRefreshNoteCachesUsesNonRestAnalysisSegments();
  testComposeWaveformFollowsOutputFrameCount();
  testBlendSynthesizedRangeIntoAuditionBuffer();
  testBlendSynthesizedRangeResizesToOutputDuration();
  testBlendSynthesizedRangeWritesAllChannels();
  testBlendSynthesizedRangeOffsetsClampedNegativeStart();
  testBlendUsesCurrentWaveformWhenAuditionBufferIsStale();
  testApplyNoteVolumeToSynthesizedRangeBeforeBlend();
  testClearSynthesisDirtyForRangeOnlyClearsOverlappingSynthDirty();
  testCompletedSynthesisDirtyCleanupUsesCapturedSnapshot();
  testDirtySnapshotDoesNotClearReplacementNoteAtSameIndex();
  testTensionProcessorReturnsSeparateHarmonicAndNoise();
  testTensionProcessorComputesSTFTCache();

  if (failures != 0)
  {
    std::cerr << failures << " test failure(s)\n";
    return 1;
  }

  std::cout << "All ProjectCoreTests passed\n";
  return 0;
}
