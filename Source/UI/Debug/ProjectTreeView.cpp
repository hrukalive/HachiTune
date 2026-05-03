#include "ProjectTreeView.h"
#include <algorithm>

namespace
{
juce::String formatMatrixSize(const std::vector<std::vector<float>>& matrix)
{
  const int rows = static_cast<int>(matrix.size());
  const int cols = rows > 0 ? static_cast<int>(matrix.front().size()) : 0;
  return juce::String(rows) + " x " + juce::String(cols);
}
} // namespace

// =============================================================================
// TreeViewItem subclasses
// =============================================================================

class ProjectTreeView::PropertyItem : public juce::TreeViewItem
{
public:
  PropertyItem(const juce::String& text) : displayText(text) {}
  bool mightContainSubItems() override { return false; }
  juce::String getUniqueName() const override { return displayText; }

  void setText(const juce::String& text)
  {
    if (displayText != text)
    {
      displayText = text;
      repaintItem();
    }
  }

  void paintItem(juce::Graphics& g, int width, int height) override
  {
    g.setColour(juce::Colours::white);
    g.setFont(juce::Font(12.0f));
    g.drawText(displayText, 4, 0, width - 4, height,
               juce::Justification::centredLeft);
  }

  int getItemHeight() const override { return 18; }

private:
  juce::String displayText;
};

class ProjectTreeView::CategoryItem : public juce::TreeViewItem
{
public:
  CategoryItem(const juce::String& name) : categoryName(name) {}
  bool mightContainSubItems() override { return true; }
  juce::String getUniqueName() const override { return categoryName; }

  void setName(const juce::String& name)
  {
    if (categoryName != name)
    {
      categoryName = name;
      repaintItem();
    }
  }

  void paintItem(juce::Graphics& g, int width, int height) override
  {
    g.setColour(juce::Colours::lightskyblue);
    g.setFont(juce::Font(juce::Font::getDefaultSansSerifFontName(), 13.0f, juce::Font::bold));
    g.drawText(categoryName, 4, 0, width - 4, height,
               juce::Justification::centredLeft);
  }

  int getItemHeight() const override { return 20; }

private:
  juce::String categoryName;
};

class ProjectTreeView::NoteItem : public juce::TreeViewItem
{
public:
  NoteItem(int index) : noteIndex(index) {}

  bool mightContainSubItems() override { return true; }
  juce::String getUniqueName() const override { return "note_" + juce::String(noteIndex); }

  void updateFrom(const Note& note)
  {
    bool changed = false;
    auto check = [&](auto& field, auto val) {
      if (field != val) { field = val; changed = true; }
    };
    check(midiNote, note.getMidiNote());
    check(startFrame, note.getStartFrame());
    check(endFrame, note.getEndFrame());
    check(pitchOffset, note.getPitchOffset());
    check(rest, note.isRest());
    check(dirty, note.isDirty());
    check(selected, note.isSelected());
    check(varianceScale, note.getVarianceScale());
    check(tiltLeft, note.getTiltLeft());
    check(tiltRight, note.getTiltRight());
    check(smoothLeft, note.getSmoothLeftFrames());
    check(smoothRight, note.getSmoothRightFrames());
    check(basePitchSize, static_cast<int>(note.getBasePitch().size()));
    check(deltaPitchSize, static_cast<int>(note.getDeltaPitch().size()));
    check(originalDeltaPitchSize,
          static_cast<int>(note.getOriginalDeltaPitch().size()));
    check(voicingCurveSize, static_cast<int>(note.getVoicingCurve().size()));
    check(breathCurveSize, static_cast<int>(note.getBreathCurve().size()));
    check(tensionCurveSize, static_cast<int>(note.getTensionCurve().size()));

    if (changed)
    {
      repaintItem();
      if (isOpen())
        rebuildSubItems();
    }
  }

  void paintItem(juce::Graphics& g, int width, int height) override
  {
    g.setColour(juce::Colours::lightgreen);
    g.setFont(juce::Font(12.0f));
    juce::String text = "[" + juce::String(noteIndex) + "] midi=" +
                        juce::String(midiNote, 2) +
                        " [" + juce::String(startFrame) + ".." +
                        juce::String(endFrame) + ")";
    if (rest) text += " REST";
    if (dirty) text += " DIRTY";
    if (selected) text += " SEL";
    g.drawText(text, 4, 0, width - 4, height,
               juce::Justification::centredLeft);
  }

  void itemOpennessChanged(bool isNowOpen) override
  {
    if (isNowOpen)
      rebuildSubItems();
    else
      clearSubItems();
  }

  int getItemHeight() const override { return 18; }

private:
  void rebuildSubItems()
  {
    clearSubItems();
    addSubItem(new PropertyItem("startFrame: " + juce::String(startFrame)));
    addSubItem(new PropertyItem("endFrame: " + juce::String(endFrame)));
    addSubItem(new PropertyItem("midiNote: " + juce::String(midiNote, 2)));
    addSubItem(new PropertyItem("pitchOffset: " + juce::String(pitchOffset, 2)));
    addSubItem(new PropertyItem("isRest: " + juce::String(rest ? "yes" : "no")));
    addSubItem(new PropertyItem("isDirty: " + juce::String(dirty ? "yes" : "no")));
    addSubItem(new PropertyItem("isSelected: " + juce::String(selected ? "yes" : "no")));
    addSubItem(new PropertyItem("varianceScale: " + juce::String(varianceScale, 2)));
    addSubItem(new PropertyItem("tiltLeft: " + juce::String(tiltLeft, 2)));
    addSubItem(new PropertyItem("tiltRight: " + juce::String(tiltRight, 2)));
    addSubItem(new PropertyItem("smoothLeft: " + juce::String(smoothLeft) + " frames"));
    addSubItem(new PropertyItem("smoothRight: " + juce::String(smoothRight) + " frames"));
    addSubItem(new PropertyItem("basePitch cache: " + juce::String(basePitchSize)));
    addSubItem(new PropertyItem("deltaPitch cache: " + juce::String(deltaPitchSize)));
    addSubItem(new PropertyItem("originalDelta cache: " + juce::String(originalDeltaPitchSize)));
    addSubItem(new PropertyItem("voicing cache: " + juce::String(voicingCurveSize)));
    addSubItem(new PropertyItem("breath cache: " + juce::String(breathCurveSize)));
    addSubItem(new PropertyItem("tension cache: " + juce::String(tensionCurveSize)));
  }

  int noteIndex;
  float midiNote = 60.0f;
  int startFrame = 0;
  int endFrame = 0;
  float pitchOffset = 0.0f;
  bool rest = false;
  bool dirty = false;
  bool selected = false;
  float varianceScale = 1.0f;
  float tiltLeft = 0.0f;
  float tiltRight = 0.0f;
  int smoothLeft = 0;
  int smoothRight = 0;
  int basePitchSize = 0;
  int deltaPitchSize = 0;
  int originalDeltaPitchSize = 0;
  int voicingCurveSize = 0;
  int breathCurveSize = 0;
  int tensionCurveSize = 0;
};

// =============================================================================
// RootItem
// =============================================================================

class ProjectTreeView::RootItem : public juce::TreeViewItem
{
public:
  RootItem() {}
  bool mightContainSubItems() override { return true; }
  juce::String getUniqueName() const override { return "root"; }
  void paintItem(juce::Graphics&, int, int) override {}
  int getItemHeight() const override { return 0; }
};

// =============================================================================
// ProjectTreeView implementation
// =============================================================================

ProjectTreeView::ProjectTreeView()
{
  treeView.setColour(juce::TreeView::backgroundColourId, juce::Colour(0xff1e1e2e));
  treeView.setDefaultOpenness(false);
  treeView.setMultiSelectEnabled(false);
  addAndMakeVisible(treeView);
}

ProjectTreeView::~ProjectTreeView()
{
  treeView.setRootItem(nullptr);
}

void ProjectTreeView::setProject(Project* proj)
{
  project = proj;
  buildTree();
}

void ProjectTreeView::buildTree()
{
  treeView.setRootItem(nullptr);
  rootItem.reset();
  projCat = analysisCat = editedCat = audioCat = notesCat = markersCat = nullptr;

  if (!project)
    return;

  rootItem = std::make_unique<RootItem>();

  projCat = new CategoryItem("Project");
  rootItem->addSubItem(projCat);

  analysisCat = new CategoryItem("AnalysisData");
  rootItem->addSubItem(analysisCat);

  editedCat = new CategoryItem("EditedData");
  rootItem->addSubItem(editedCat);

  audioCat = new CategoryItem("AudioData");
  rootItem->addSubItem(audioCat);

  notesCat = new CategoryItem("Notes (0)");
  rootItem->addSubItem(notesCat);

  markersCat = new CategoryItem("WarpMarkers (0)");
  rootItem->addSubItem(markersCat);

  treeView.setRootItem(rootItem.get());
  treeView.setRootItemVisible(false);
  projCat->setOpen(true);

  refresh();
}

void ProjectTreeView::refresh()
{
  if (!project || !rootItem)
    return;

  updatePropertyItems();
  updateNoteItems();
}

void ProjectTreeView::refreshNotes()
{
  if (!project || !rootItem)
    return;
  updateNoteItems();
}

void ProjectTreeView::refreshProject()
{
  if (!project || !rootItem)
    return;
  updatePropertyItems();
}

void ProjectTreeView::updatePropertyItems()
{
  if (!project)
    return;

  // --- Project category ---
  auto setOrUpdate = [](CategoryItem* cat, int index, const juce::String& text) {
    if (!cat) return;
    if (index < cat->getNumSubItems())
    {
      auto* item = dynamic_cast<PropertyItem*>(cat->getSubItem(index));
      if (item)
        item->setText(text);
    }
    else
    {
      cat->addSubItem(new PropertyItem(text));
    }
  };

  auto setRows = [&](CategoryItem* cat, const std::vector<juce::String>& rows) {
    if (!cat)
      return;
    while (cat->getNumSubItems() > static_cast<int>(rows.size()))
      cat->removeSubItem(cat->getNumSubItems() - 1);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
      setOrUpdate(cat, i, rows[static_cast<size_t>(i)]);
  };

  setRows(projCat,
          {"Name: " + project->getName(),
           "GlobalPitchOffset: " +
               juce::String(project->getGlobalPitchOffset(), 2),
           "FormantShift: " + juce::String(project->getFormantShift(), 2),
           "Volume: " + juce::String(project->getVolume(), 2) + " dB",
           "Modified: " + juce::String(project->isModified() ? "yes" : "no")});

  // --- AnalysisData ---
  const auto& ad = project->getAnalysisData();
  {
    int voicedCount = 0;
    for (auto v : ad.originalVoicedMask)
      if (v)
        ++voicedCount;
    int nonZeroF0 = 0;
    for (auto v : ad.originalF0)
      if (v > 0.0f)
        ++nonZeroF0;

    setRows(analysisCat,
            {"Frames: " + juce::String(ad.getNumFrames()),
             "isEmpty: " + juce::String(ad.isEmpty() ? "yes" : "no"),
             "analysis.originalF0: " + juce::String(static_cast<int>(ad.originalF0.size())),
             "analysis.originalPitch: " + juce::String(static_cast<int>(ad.originalPitch.size())),
             "analysis.originalDeltaPitch: " + juce::String(static_cast<int>(ad.originalDeltaPitch.size())),
             "analysis.originalVoicedMask: " + juce::String(static_cast<int>(ad.originalVoicedMask.size())),
             "analysis.originalVADMask: " + juce::String(static_cast<int>(ad.originalVADMask.size())),
             "analysis.noteSegments: " + juce::String(static_cast<int>(ad.noteSegments.size())),
             "VoicedFrames: " + juce::String(voicedCount),
             "NonZeroF0: " + juce::String(nonZeroF0)});
  }

  // --- EditedData ---
  const auto& ed = project->getEditedData();
  {
    int voicedCount = 0;
    for (auto v : ed.voicedMask)
      if (v)
        ++voicedCount;
    int nonZeroF0 = 0;
    for (auto v : ed.f0)
      if (v > 0.0f)
        ++nonZeroF0;
    int nonZeroBP = 0;
    for (auto v : ed.basePitch)
      if (v != 0.0f)
        ++nonZeroBP;
    int nonZeroDP = 0;
    for (auto v : ed.deltaPitch)
      if (v != 0.0f)
        ++nonZeroDP;

    const auto validation = project->validateFrameData();
    std::vector<juce::String> rows = {
        "Valid: " + juce::String(validation.isValid() ? "yes" : "no")};
    for (const auto& message : validation.messages)
      rows.push_back("Validation: " + message);
    rows.push_back("Frames: " + juce::String(ed.getNumFrames()));
    rows.push_back("isEmpty: " + juce::String(ed.isEmpty() ? "yes" : "no"));
    rows.push_back("basePitch size: " +
                   juce::String(static_cast<int>(ed.basePitch.size())));
    rows.push_back("deltaPitch size: " +
                   juce::String(static_cast<int>(ed.deltaPitch.size())));
    rows.push_back("f0 size: " +
                   juce::String(static_cast<int>(ed.f0.size())));
    rows.push_back("voicedMask size: " +
                   juce::String(static_cast<int>(ed.voicedMask.size())));
    rows.push_back("vadMask size: " +
                   juce::String(static_cast<int>(ed.vadMask.size())));
    rows.push_back("voicingCurve size: " +
                   juce::String(static_cast<int>(ed.voicingCurve.size())));
    rows.push_back("breathCurve size: " +
                   juce::String(static_cast<int>(ed.breathCurve.size())));
    rows.push_back("tensionCurve size: " +
                   juce::String(static_cast<int>(ed.tensionCurve.size())));
    rows.push_back("VoicedFrames: " + juce::String(voicedCount));
    rows.push_back("NonZeroF0: " + juce::String(nonZeroF0));
    rows.push_back("NonZeroBasePitch: " + juce::String(nonZeroBP));
    rows.push_back("NonZeroDeltaPitch: " + juce::String(nonZeroDP));
    setRows(editedCat, rows);
  }

  // --- AudioData ---
  const auto& audioData = project->getAudioData();
  setRows(audioCat,
          {"SampleRate: " + juce::String(audioData.sampleRate),
           "Waveform: " +
               juce::String(audioData.waveform.getNumSamples()) + " samples",
           "analysis.originalMel: " +
               formatMatrixSize(project->getAnalysisData().originalMel),
           "edited.adjustedMel: " +
               formatMatrixSize(project->getEditedData().adjustedMel),
           "edited.mel: " + formatMatrixSize(project->getEditedData().mel),
           "audio.harmonicWaveform: " +
               juce::String(audioData.harmonicWaveform.getNumSamples()) +
               " samples",
           "audio.noiseWaveform: " +
               juce::String(audioData.noiseWaveform.getNumSamples()) +
               " samples",
           "project.harmonicSTFT: " +
               juce::String(static_cast<int>(project->getHarmonicSTFT().size())),
           "project.noiseSTFT: " +
               juce::String(static_cast<int>(project->getNoiseSTFT().size()))});

  // --- WarpMarkers ---
  const auto& markers = project->getWarpMarkers();
  bool stretchActive = markers.size() > 0;
  markersCat->setName("WarpMarkers (" + juce::String(static_cast<int>(markers.size())) +
                       ")" + (stretchActive ? " [STRETCH ACTIVE]" : ""));

  // Adjust marker count
  while (markersCat->getNumSubItems() > static_cast<int>(markers.size()))
    markersCat->removeSubItem(markersCat->getNumSubItems() - 1);

  for (int i = 0; i < static_cast<int>(markers.size()); ++i)
  {
    juce::String text = "[" + juce::String(i) + "] src=" +
                        juce::String(markers[i].sourceFrame) +
                        " out=" + juce::String(markers[i].outputFrame);
    setOrUpdate(markersCat, i, text);
  }
}

void ProjectTreeView::updateNoteItems()
{
  if (!project || !notesCat)
    return;

  const auto& notes = project->getNotes();
  const int noteCount = static_cast<int>(notes.size());
  int dirtyCount = 0, selectedCount = 0;
  for (const auto& n : notes)
  {
    if (n.isDirty()) ++dirtyCount;
    if (n.isSelected()) ++selectedCount;
  }

  notesCat->setName("Notes (" + juce::String(noteCount) +
                     ", " + juce::String(dirtyCount) + " dirty, " +
                     juce::String(selectedCount) + " sel)");

  // Remove excess items
  while (notesCat->getNumSubItems() > noteCount)
    notesCat->removeSubItem(notesCat->getNumSubItems() - 1);

  // Add missing items
  while (notesCat->getNumSubItems() < noteCount)
  {
    int idx = notesCat->getNumSubItems();
    notesCat->addSubItem(new NoteItem(idx));
  }

  // Update all note items in-place
  for (int i = 0; i < noteCount; ++i)
  {
    auto* noteItem = dynamic_cast<NoteItem*>(notesCat->getSubItem(i));
    if (noteItem)
      noteItem->updateFrom(notes[i]);
  }
}

void ProjectTreeView::resized()
{
  treeView.setBounds(getLocalBounds());
}
