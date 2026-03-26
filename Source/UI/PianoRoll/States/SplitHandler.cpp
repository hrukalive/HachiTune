#include "SplitHandler.h"
#include "../../PianoRollComponent.h"
#include "../NoteSplitter.h"

#include <cmath>

SplitHandler::SplitHandler(PianoRollComponent &owner)
    : InteractionHandler(owner) {}

bool SplitHandler::mouseDown(const juce::MouseEvent &e, float worldX,
                             float worldY) {
  juce::ignoreUnused(e, worldY);

  Note *note = owner_.findNoteAt(worldX, worldY);
  if (note) {
    owner_.noteSplitter->splitNoteAtX(note, worldX);
    return true;
  }
  return false;
}

void SplitHandler::mouseMove(const juce::MouseEvent &e, float worldX,
                             float worldY) {
  juce::ignoreUnused(e, worldY);

  if (!owner_.project) {
    clearGuide();
    return;
  }

  Note *note = owner_.findNoteAt(worldX, worldY);
  const bool noteChanged = note != splitGuideNote;
  const bool guideMoved = std::abs(worldX - splitGuideX) > 0.5f;
  if (!noteChanged && (!note || !guideMoved))
    return;

  if (note)
  {
    splitGuideX = worldX;
    splitGuideNote = note;
  }
  else
  {
    splitGuideX = -1.0f;
    splitGuideNote = nullptr;
  }

  owner_.repaint();
}

void SplitHandler::cancel() { clearGuide(); }

void SplitHandler::clearGuide() {
  if (splitGuideX >= 0) {
    splitGuideX = -1.0f;
    splitGuideNote = nullptr;
    owner_.repaint();
  }
}
