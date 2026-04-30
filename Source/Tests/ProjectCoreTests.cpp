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
  testSerializerRestoresSourceRangesFromAnalysisSegments();
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
