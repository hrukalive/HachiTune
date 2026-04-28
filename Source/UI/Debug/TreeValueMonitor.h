#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../../Models/ProjectListener.h"
#include <algorithm>

/**
 * Debug window that displays the Project's live state as a tree.
 * Implements ProjectListener to auto-refresh on changes.
 * Open via menu or programmatically during development.
 */
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
    void paint(juce::Graphics& g) override;
    void resized() override;
    void setText(const juce::String& text);

  private:
    juce::TextEditor textEditor;
  };

  Project* project = nullptr;
  ContentComponent content;

  juce::String buildDisplayText() const;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TreeValueMonitor)
};
