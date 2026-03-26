#pragma once

#if HACHITUNE_ENABLE_STRETCH

#include "InteractionHandler.h"
#include "../../../Models/Project.h"

#include <vector>

class Note;

class StretchHandler : public InteractionHandler
{
public:
  explicit StretchHandler(PianoRollComponent &owner);

  bool mouseDown(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool mouseDrag(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool mouseUp(const juce::MouseEvent &e, float worldX,
               float worldY) override;
  void mouseMove(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  void draw(juce::Graphics &g) override;
  bool isActive() const override;
  void cancel() override;

  struct StretchBoundary
  {
    Note *left = nullptr;
    Note *right = nullptr;
    int frame = 0;
    int sourceFrame = 0;
    bool active = false;
  };

  struct StretchDragState
  {
    bool active = false;
    bool changed = false;
    StretchBoundary boundary;
    int currentBoundary = 0;
    int minFrame = 0;
    int maxFrame = 0;
    std::vector<Project::WarpMarker> originalMarkers;
    std::vector<Project::WarpMarker> previewMarkers;
  };

  int getHoveredBoundaryIndex() const { return hoveredStretchBoundaryIndex; }
  std::vector<StretchBoundary> collectStretchBoundaries() const;
  const StretchDragState &getDragState() const { return stretchDrag; }

private:
  int findStretchBoundaryIndex(float worldX, float tolerancePx) const;
  void startStretchDrag(const StretchBoundary &boundary);
  void updateStretchDrag(int targetFrame);
  void finishStretchDrag();
  void cancelStretchDrag();
  bool deactivateMarker(const StretchBoundary &boundary);
  void applyMarkers(const std::vector<Project::WarpMarker> &markers,
                    bool updateProjectMarkers);
  void updateDirtyRanges();

  StretchDragState stretchDrag;
  int hoveredStretchBoundaryIndex = -1;
  static constexpr float stretchHandleHitPadding = 6.0f;
  static constexpr int minStretchNoteFrames = 3;
};

#endif // HACHITUNE_ENABLE_STRETCH
