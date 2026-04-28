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
  text << "=== WARP MARKERS (" << static_cast<int>(markers.size()) << ") ===\n";
  for (size_t i = 0; i < markers.size(); ++i)
  {
    text << "  [" << static_cast<int>(i) << "] src=" << markers[i].sourceFrame
         << " out=" << markers[i].outputFrame << "\n";
  }
  text << "\n";

  // Notes
  const auto& notes = project->getNotes();
  text << "=== NOTES (" << static_cast<int>(notes.size()) << ") ===\n";
  for (size_t i = 0; i < notes.size(); ++i)
  {
    const auto& n = notes[i];
    text << "  [" << static_cast<int>(i) << "] frames=[" << n.getStartFrame() << ".."
         << n.getEndFrame() << ") midi=" << juce::String(n.getMidiNote(), 2)
         << " offset=" << juce::String(n.getPitchOffset(), 2)
         << (n.isRest() ? " REST" : "")
         << (n.isDirty() ? " DIRTY" : "")
         << (n.isSelected() ? " SEL" : "")
         << "\n";
  }

  return text;
}
