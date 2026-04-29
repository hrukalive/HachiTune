#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"

class ProjectTreeView : public juce::Component
{
public:
  ProjectTreeView();
  ~ProjectTreeView() override;

  void setProject(Project* project);
  void refresh();
  void refreshNotes();
  void refreshProject();

  void resized() override;

private:
  class RootItem;
  class CategoryItem;
  class NoteItem;
  class PropertyItem;

  void buildTree();
  void updateNoteItems();
  void updatePropertyItems();

  Project* project = nullptr;
  juce::TreeView treeView;
  std::unique_ptr<RootItem> rootItem;

  CategoryItem* projCat = nullptr;
  CategoryItem* analysisCat = nullptr;
  CategoryItem* editedCat = nullptr;
  CategoryItem* audioCat = nullptr;
  CategoryItem* notesCat = nullptr;
  CategoryItem* markersCat = nullptr;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProjectTreeView)
};
