#include "ProjectTreeView.h"
#include <algorithm>

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

  // Ensure projCat has 5 property items
  while (projCat->getNumSubItems() > 5)
    projCat->removeSubItem(projCat->getNumSubItems() - 1);

  setOrUpdate(projCat, 0, "Name: " + project->getName());
  setOrUpdate(projCat, 1, "GlobalPitchOffset: " + juce::String(project->getGlobalPitchOffset(), 2));
  setOrUpdate(projCat, 2, "FormantShift: " + juce::String(project->getFormantShift(), 2));
  setOrUpdate(projCat, 3, "Volume: " + juce::String(project->getVolume(), 2) + " dB");
  setOrUpdate(projCat, 4, "Modified: " + juce::String(project->isModified() ? "yes" : "no"));

  // --- AnalysisData ---
  const auto& ad = project->getAnalysisData();
  {
    int voicedCount = 0;
    for (auto v : ad.originalVoicedMask) if (v) ++voicedCount;
    int nonZeroF0 = 0;
    for (auto v : ad.originalF0) if (v > 0.0f) ++nonZeroF0;

    const int adProps = 5;
    while (analysisCat->getNumSubItems() > adProps)
      analysisCat->removeSubItem(analysisCat->getNumSubItems() - 1);
    setOrUpdate(analysisCat, 0, "Frames: " + juce::String(ad.getNumFrames()));
    setOrUpdate(analysisCat, 1, "isEmpty: " + juce::String(ad.isEmpty() ? "yes" : "no"));
    setOrUpdate(analysisCat, 2, "VoicedFrames: " + juce::String(voicedCount));
    setOrUpdate(analysisCat, 3, "NonZeroF0: " + juce::String(nonZeroF0));
    setOrUpdate(analysisCat, 4, "NoteSegments: " + juce::String(static_cast<int>(ad.noteSegments.size())));
  }

  // --- EditedData ---
  const auto& ed = project->getEditedData();
  {
    int voicedCount = 0;
    for (auto v : ed.voicedMask) if (v) ++voicedCount;
    int nonZeroF0 = 0;
    for (auto v : ed.f0) if (v > 0.0f) ++nonZeroF0;
    int nonZeroBP = 0;
    for (auto v : ed.basePitch) if (v != 0.0f) ++nonZeroBP;
    int nonZeroDP = 0;
    for (auto v : ed.deltaPitch) if (v != 0.0f) ++nonZeroDP;

    const int edProps = 7;
    while (editedCat->getNumSubItems() > edProps)
      editedCat->removeSubItem(editedCat->getNumSubItems() - 1);
    setOrUpdate(editedCat, 0, "Frames: " + juce::String(ed.getNumFrames()));
    setOrUpdate(editedCat, 1, "isEmpty: " + juce::String(ed.isEmpty() ? "yes" : "no"));
    setOrUpdate(editedCat, 2, "VoicedFrames: " + juce::String(voicedCount));
    setOrUpdate(editedCat, 3, "NonZeroF0: " + juce::String(nonZeroF0));
    setOrUpdate(editedCat, 4, "NonZeroBasePitch: " + juce::String(nonZeroBP));
    setOrUpdate(editedCat, 5, "NonZeroDeltaPitch: " + juce::String(nonZeroDP));
    setOrUpdate(editedCat, 6, "VoicingCurve size: " + juce::String(static_cast<int>(ed.voicingCurve.size())));
  }

  // --- AudioData ---
  const auto& audioData = project->getAudioData();
  while (audioCat->getNumSubItems() > 3)
    audioCat->removeSubItem(audioCat->getNumSubItems() - 1);
  setOrUpdate(audioCat, 0, "SampleRate: " + juce::String(audioData.sampleRate));
  setOrUpdate(audioCat, 1, "Waveform: " + juce::String(audioData.waveform.getNumSamples()) + " samples");
  setOrUpdate(audioCat, 2, "MelSpectrogram: " +
      juce::String(static_cast<int>(audioData.melSpectrogram.size())) + " x " +
      juce::String(audioData.melSpectrogram.empty() ? 0 : static_cast<int>(audioData.melSpectrogram[0].size())));

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
