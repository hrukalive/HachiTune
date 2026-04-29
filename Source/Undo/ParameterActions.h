#pragma once

#include "UndoableAction.h"
#include <vector>
#include <functional>
#include <limits>

/**
 * Parameter type for hnsep curve edits (voicing, breath, tension).
 */
enum class HNSepParamType
{
  Voicing, // Harmonic energy (0..maxVoicing, default 100)
  Breath,  // Noise energy (0..maxBreath, default 100)
  Tension  // Spectral tilt (-100..100, default 0)
};

/**
 * Represents a single frame edit in an hnsep parameter curve.
 * Captures both old and new values for undo/redo support.
 *
 * noteIndex: Index into the project's note vector.
 * frameOffset: Note-local frame offset (0-based, relative to note start).
 * oldValue / newValue: The parameter value before/after the edit.
 */
struct ParameterFrameEdit
{
  int noteIndex = -1;
  int frameOffset = -1;
  float oldValue = 0.0f;
  float newValue = 0.0f;
};

/**
 * Undoable action for editing hnsep parameter curves (voicing, breath, tension).
 *
 * Stores a vector of per-frame edits for a single parameter type.
 * On undo/redo, directly writes old/new values into the note's curve vector.
 *
 * The callback is invoked after undo/redo with the affected noteIndex range
 * so the caller can mark dirty ranges and trigger resynthesis.
 *
 * Usage pattern (from HNSepLaneComponent):
 *   1. During mouse drag, accumulate ParameterFrameEdit entries
 *   2. On mouse up, create ParameterCurveAction and add to PitchUndoManager
 *   3. The action stores a getCurve lambda that retrieves the note's curve vector
 *
 * @see F0DrawAction for the analogous pitch curve undo action.
 */
class ParameterCurveAction : public UndoableAction
{
public:
  /**
   * @param paramType      Which parameter was edited (Voicing/Breath/Tension).
   * @param edits          Vector of per-frame edits (noteIndex + frameOffset + old/new).
   * @param getCurveFunc   Lambda: (int noteIndex, HNSepParamType) -> std::vector<float>*
   *                       Returns a pointer to the note's curve vector, or nullptr if invalid.
   * @param onChanged      Callback invoked after undo/redo with (minNoteIndex, maxNoteIndex).
   */
  ParameterCurveAction(
      HNSepParamType paramType,
      std::vector<ParameterFrameEdit> edits,
      std::function<std::vector<float> *(int, HNSepParamType)> getCurveFunc,
      std::function<void(int, int)> onChanged = nullptr)
      : paramType(paramType),
        edits(std::move(edits)),
        getCurveFunc(std::move(getCurveFunc)),
        onChanged(std::move(onChanged))
  {
  }

  void undo() override
  {
    int minNote = std::numeric_limits<int>::max();
    int maxNote = std::numeric_limits<int>::min();

    for (const auto &e : edits)
    {
      auto *curve = getCurveFunc ? getCurveFunc(e.noteIndex, paramType) : nullptr;
      if (curve && e.frameOffset >= 0 &&
          e.frameOffset < static_cast<int>(curve->size()))
      {
        (*curve)[static_cast<size_t>(e.frameOffset)] = e.oldValue;
        minNote = std::min(minNote, e.noteIndex);
        maxNote = std::max(maxNote, e.noteIndex);
      }
    }

    if (onChanged && minNote <= maxNote)
      onChanged(minNote, maxNote);
  }

  void redo() override
  {
    int minNote = std::numeric_limits<int>::max();
    int maxNote = std::numeric_limits<int>::min();

    for (const auto &e : edits)
    {
      auto *curve = getCurveFunc ? getCurveFunc(e.noteIndex, paramType) : nullptr;
      if (curve && e.frameOffset >= 0 &&
          e.frameOffset < static_cast<int>(curve->size()))
      {
        (*curve)[static_cast<size_t>(e.frameOffset)] = e.newValue;
        minNote = std::min(minNote, e.noteIndex);
        maxNote = std::max(maxNote, e.noteIndex);
      }
    }

    if (onChanged && minNote <= maxNote)
      onChanged(minNote, maxNote);
  }

  juce::String getName() const override
  {
    switch (paramType)
    {
    case HNSepParamType::Voicing:
      return "Edit Voicing Curve";
    case HNSepParamType::Breath:
      return "Edit Breath Curve";
    case HNSepParamType::Tension:
      return "Edit Tension Curve";
    }
    return "Edit Parameter Curve";
  }

private:
  HNSepParamType paramType;
  std::vector<ParameterFrameEdit> edits;
  std::function<std::vector<float> *(int, HNSepParamType)> getCurveFunc;
  std::function<void(int, int)> onChanged;
};
