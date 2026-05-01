#include "TreeValueMonitor.h"

// --- ContentComponent ---
TreeValueMonitor::ContentComponent::ContentComponent()
{
  addAndMakeVisible(treeView);
  addAndMakeVisible(melView);
}

void TreeValueMonitor::ContentComponent::resized()
{
  auto bounds = getLocalBounds();
  const int treeWidth = 300;
  treeView.setBounds(bounds.removeFromLeft(treeWidth));
  melView.setBounds(bounds);
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
  setSize(1200, 700);
  setVisible(true);

  content.treeView.setProject(project);
  content.melView.setProject(project);

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

void TreeValueMonitor::onProjectChanged(ProjectChangeType type,
                                        int /*affectedNoteIndex*/,
                                        int /*rangeStart*/,
                                        int /*rangeEnd*/)
{
  juce::Component::SafePointer<TreeValueMonitor> safeThis(this);
  juce::MessageManager::callAsync([safeThis, type]() {
    if (safeThis == nullptr)
      return;

    switch (type)
    {
    case ProjectChangeType::NoteListChanged:
      safeThis->content.treeView.refresh();
      break;
    case ProjectChangeType::NotePitchChanged:
    case ProjectChangeType::NoteCurveChanged:
    case ProjectChangeType::NotePropertyChanged:
    case ProjectChangeType::NoteSelectionChanged:
      safeThis->content.treeView.refreshNotes();
      break;
    case ProjectChangeType::AudioDataChanged:
      safeThis->content.treeView.refresh();
      safeThis->content.melView.rebuildMelImage();
      safeThis->content.melView.rebuildF0Path();
      break;
    case ProjectChangeType::EditedDataChanged:
      safeThis->content.treeView.refresh();
      safeThis->content.melView.rebuildMelImage();
      safeThis->content.melView.rebuildF0Path();
      break;
    case ProjectChangeType::SynthesisComplete:
      safeThis->content.treeView.refresh();
      safeThis->content.melView.rebuildMelImage();
      safeThis->content.melView.rebuildF0Path();
      break;
    case ProjectChangeType::WarpChanged:
      safeThis->content.treeView.refresh();
      safeThis->content.melView.rebuildMelImage();
      safeThis->content.melView.rebuildF0Path();
      break;
    case ProjectChangeType::GlobalParamChanged:
    case ProjectChangeType::SettingsChanged:
      safeThis->content.treeView.refreshProject();
      break;
    }
  });
}

void TreeValueMonitor::refresh()
{
  content.treeView.refresh();
  content.melView.rebuildMelImage();
  content.melView.repaint();
}
