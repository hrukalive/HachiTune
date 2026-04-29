#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Models/ProjectListener.h"
#include "ProjectTreeView.h"
#include "MelViewComponent.h"

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
    void resized() override;

    ProjectTreeView treeView;
    MelViewComponent melView;
  };

  Project* project = nullptr;
  ContentComponent content;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TreeValueMonitor)
};
