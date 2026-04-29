#pragma once

#include "InteractionHandler.h"
#include "../../../Undo/SnapshotHelper.h"
#include "../../../Utils/UI/DrawCurve.h"

#include <deque>
#include <limits>
#include <memory>
#include <unordered_set>
#include <vector>

class Note;

/**
 * Handles pitch curve drawing interactions in Draw edit mode.
 * Manages freehand drawing state and applies pitch edits to F0 data.
 */
class DrawHandler : public InteractionHandler {
public:
  explicit DrawHandler(PianoRollComponent &owner);

  bool mouseDown(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool mouseDrag(const juce::MouseEvent &e, float worldX,
                 float worldY) override;
  bool mouseUp(const juce::MouseEvent &e, float worldX,
               float worldY) override;
  bool isActive() const override;
  void cancel() override;

  // Accessors for rendering
  bool getIsDrawing() const { return isDrawing; }
  const std::deque<std::unique_ptr<DrawCurve>> &getDrawCurves() const {
    return drawCurves;
  }

private:
  void applyPitchDrawing(float x, float y);
  void commitPitchDrawing();
  void applyPitchPoint(int frameIndex, int midiCents);
  void startNewPitchCurve(int frameIndex, int midiCents);
  void bakeNoteToolParams(Note &note);
  void showNoteResetMenu(float worldX, float worldY);
  void resetNoteToOriginal(Note &note);

  bool isDrawing = false;
  bool isPendingDraw = false;
  float pendingDrawStartX = 0.0f;
  float pendingDrawStartY = 0.0f;
  int snapshotStartFrame = -1;
  int snapshotEndFrame = -1;
  std::vector<float> beforeF0;
  std::vector<float> beforeDelta;
  std::vector<bool> beforeVoiced;
  std::vector<bool> beforeEdited;
  int minEditedFrame = std::numeric_limits<int>::max();
  int maxEditedFrame = std::numeric_limits<int>::min();
  std::unordered_set<Note *> bakedNotes; // Notes already baked in this session
  int lastDrawFrame = -1;
  int lastDrawValueCents = 0;
  DrawCurve *activeDrawCurve = nullptr;
  std::deque<std::unique_ptr<DrawCurve>> drawCurves;
};
