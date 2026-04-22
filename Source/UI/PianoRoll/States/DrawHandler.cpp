#include "DrawHandler.h"
#include "../../PianoRollComponent.h"
#include "../../../Utils/Constants.h"
#include "../../../Utils/PitchCurveProcessor.h"
#include "../../../Utils/PitchToolOperations.h"
#include "../../../Utils/FourierPitchFilter.h"
#include "../../../Utils/CurveResampler.h"

DrawHandler::DrawHandler(PianoRollComponent &owner)
    : InteractionHandler(owner) {}

bool DrawHandler::mouseDown(const juce::MouseEvent &e, float worldX,
                            float worldY) {
  // Right-click: show context menu for note reset
  if (e.mods.isRightButtonDown()) {
    showNoteResetMenu(worldX, worldY);
    return true;
  }

  isDrawing = false;
  isPendingDraw = true;
  drawingEdits.clear();
  drawingEditIndexByFrame.clear();
  bakedNotes.clear();
  drawCurves.clear();
  activeDrawCurve = nullptr;
  lastDrawFrame = -1;
  lastDrawValueCents = 0;
  pendingDrawStartX = worldX;
  pendingDrawStartY = worldY;
  return true;
}

bool DrawHandler::mouseDrag(const juce::MouseEvent &e, float worldX,
                            float worldY) {
  juce::ignoreUnused(e);

  if (isPendingDraw) {
    isPendingDraw = false;
    isDrawing = true;
    applyPitchDrawing(pendingDrawStartX, pendingDrawStartY);
  }

  if (!isDrawing)
    return false;

  applyPitchDrawing(worldX, worldY);

  if (owner_.onPitchEdited)
    owner_.onPitchEdited();

  owner_.repaint();
  return true;
}

bool DrawHandler::mouseUp(const juce::MouseEvent &e, float worldX,
                          float worldY) {
  juce::ignoreUnused(e, worldX, worldY);

  if (isPendingDraw) {
    isPendingDraw = false;
    drawingEdits.clear();
    drawingEditIndexByFrame.clear();
    lastDrawFrame = -1;
    lastDrawValueCents = 0;
    activeDrawCurve = nullptr;
    drawCurves.clear();
    return true;
  }

  if (!isDrawing)
    return false;

  isDrawing = false;
  commitPitchDrawing();
  owner_.repaint();
  return true;
}

bool DrawHandler::isActive() const { return isDrawing || isPendingDraw; }

void DrawHandler::cancel() {
  if (isPendingDraw) {
    isPendingDraw = false;
    drawingEdits.clear();
    drawingEditIndexByFrame.clear();
    bakedNotes.clear();
    lastDrawFrame = -1;
    lastDrawValueCents = 0;
    activeDrawCurve = nullptr;
    drawCurves.clear();
    owner_.repaint();
    return;
  }

  if (!isDrawing)
    return;

  // Restore original F0 values from drawing edits
  if (owner_.project && !drawingEdits.empty()) {
    auto &audioData = owner_.project->getAudioData();
    for (const auto &e : drawingEdits) {
      if (e.idx >= 0 && e.idx < static_cast<int>(audioData.f0.size())) {
        audioData.f0[e.idx] = e.oldF0;
      }
      if (e.idx >= 0 &&
          e.idx < static_cast<int>(audioData.deltaPitch.size())) {
        audioData.deltaPitch[e.idx] = e.oldDelta;
      }
      if (e.idx >= 0 &&
          e.idx < static_cast<int>(audioData.voicedMask.size())) {
        audioData.voicedMask[e.idx] = e.oldVoiced;
      }
      if (e.idx >= 0 &&
          e.idx < static_cast<int>(audioData.f0EditedMask.size())) {
        audioData.f0EditedMask[e.idx] = e.oldEdited;
      }
    }
  }

  isDrawing = false;
  isPendingDraw = false;
  drawingEdits.clear();
  drawingEditIndexByFrame.clear();
  bakedNotes.clear();
  lastDrawFrame = -1;
  lastDrawValueCents = 0;
  activeDrawCurve = nullptr;
  drawCurves.clear();

  owner_.repaint();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void DrawHandler::applyPitchDrawing(float x, float y) {
  if (!owner_.project)
    return;

  auto &audioData = owner_.project->getAudioData();
  if (audioData.f0.empty())
    return;

  double time = owner_.xToTime(x);
  // Compensate for centering offset used in display
  float midi = owner_.yToMidi(y - owner_.pixelsPerSemitone * 0.5f);
  // Remove global pitch offset so drawing maps to what is shown on screen
  if (owner_.project)
    midi -= owner_.project->getGlobalPitchOffset();
  int frameIndex =
      static_cast<int>(secondsToFrames(static_cast<float>(time)));
  int midiCents = static_cast<int>(std::round(midi * 100.0f));
  applyPitchPoint(frameIndex, midiCents);
}

void DrawHandler::commitPitchDrawing() {
  if (drawingEdits.empty())
    return;

  // Calculate the dirty frame range from the changes
  int minFrame = std::numeric_limits<int>::max();
  int maxFrame = std::numeric_limits<int>::min();
  for (const auto &e : drawingEdits) {
    minFrame = std::min(minFrame, e.idx);
    maxFrame = std::max(maxFrame, e.idx);
  }

  // Clear deltaPitch for notes in the edited range so they use the drawn F0
  if (owner_.project && minFrame <= maxFrame) {
    const int maxFrameExclusive = maxFrame + 1;
    auto &notes = owner_.project->getNotes();
    auto &audioData = owner_.project->getAudioData();

    for (auto &note : notes) {
      if (note.isRest())
        continue;
      if (note.getEndFrame() <= minFrame ||
          note.getStartFrame() >= maxFrameExclusive)
        continue;

      // Bake the current audioData.deltaPitch into this note's
      // originalDeltaPitch so that subsequent note-tool transforms
      // (tilt, variance, etc.) operate on the drawn curve.
      const int noteStart = note.getStartFrame();
      const int noteEnd = note.getEndFrame();
      const int noteLen = noteEnd - noteStart;
      if (noteLen <= 0)
        continue;

      const int totalFrames = static_cast<int>(audioData.deltaPitch.size());
      std::vector<float> destDelta(static_cast<size_t>(noteLen), 0.0f);
      for (int i = 0; i < noteLen; ++i) {
        const int g = noteStart + i;
        if (g >= 0 && g < totalFrames)
          destDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(g)];
      }

      // Resample to source duration if stretched, then store as original
      const int srcLen = note.getSrcDurationFrames();
      if (srcLen > 0 && srcLen != noteLen) {
        note.setOriginalDeltaPitch(
            CurveResampler::resampleLinear(destDelta, srcLen));
      } else {
        note.setOriginalDeltaPitch(std::move(destDelta));
      }

      // Clear the note's dest-domain deltaPitch; it will be rebuilt
      // from the freshly baked originalDeltaPitch by rebuildBaseFromNotes.
      if (note.hasDeltaPitch())
        note.setDeltaPitch(std::vector<float>());

      // Clear f0EditedMask for this note's frames — the drawn delta
      // is now part of originalDeltaPitch and the normal rebuild
      // pipeline will reproduce it correctly.
      if (!audioData.f0EditedMask.empty()) {
        const int maskSize = static_cast<int>(audioData.f0EditedMask.size());
        for (int i = noteStart; i < noteEnd && i < maskSize; ++i) {
          if (i >= 0)
            audioData.f0EditedMask[static_cast<size_t>(i)] = false;
        }
      }
    }
  }

  // Set F0 dirty range in project for incremental synthesis
  if (owner_.project && minFrame <= maxFrame) {
    owner_.project->setF0DirtyRange(minFrame, maxFrame + 1);
  }

  // Create undo action
  if (owner_.undoManager && owner_.project) {
    auto &audioData = owner_.project->getAudioData();
    auto action = std::make_unique<F0EditAction>(
        &audioData.f0, &audioData.deltaPitch, &audioData.voicedMask,
        &audioData.f0EditedMask,
        drawingEdits, [this](int minFrame, int maxFrame) {
          if (owner_.project) {
            owner_.project->setF0DirtyRange(minFrame, maxFrame + 1);
            if (owner_.onPitchEditFinished)
              owner_.onPitchEditFinished();
          }
        });
    owner_.undoManager->addAction(std::move(action));
  }

  drawingEdits.clear();
  drawingEditIndexByFrame.clear();
  bakedNotes.clear();
  lastDrawFrame = -1;
  lastDrawValueCents = 0;
  activeDrawCurve = nullptr;
  drawCurves.clear();

  // Trigger synthesis
  if (owner_.onPitchEditFinished)
    owner_.onPitchEditFinished();
}

void DrawHandler::applyPitchPoint(int frameIndex, int midiCents) {
  if (!owner_.project)
    return;

  auto &audioData = owner_.project->getAudioData();
  if (audioData.f0.empty())
    return;

  const int f0Size = static_cast<int>(audioData.f0.size());
  if (audioData.deltaPitch.size() < audioData.f0.size())
    audioData.deltaPitch.resize(audioData.f0.size(), 0.0f);
  if (audioData.basePitch.size() < audioData.f0.size())
    audioData.basePitch.resize(audioData.f0.size(), 0.0f);
  if (audioData.f0EditedMask.size() < audioData.f0.size())
    audioData.f0EditedMask.resize(audioData.f0.size(), false);
  if (frameIndex < 0 || frameIndex >= f0Size)
    return;

  // Bake-before-draw: if the frame falls inside a note with non-default
  // tool params, bake the transforms into its originalDeltaPitch and reset
  // the tool params so that subsequent rebuilds don't overwrite drawn frames.
  {
    auto &notes = owner_.project->getNotes();
    for (auto &note : notes) {
      if (note.isRest())
        continue;
      if (note.getStartFrame() <= frameIndex && note.getEndFrame() > frameIndex) {
        if (note.hasNonDefaultToolParams() && bakedNotes.count(&note) == 0) {
          bakeNoteToolParams(note);
          bakedNotes.insert(&note);
        }
        break;
      }
    }
  }

  // Lambda to apply a single frame edit (shared between first-point and interpolated paths)
  auto applyFrame = [&](int idx, int cents) {
    if (idx < 0 || idx >= f0Size)
      return;

    const float newFreq = midiToFreq(static_cast<float>(cents) / 100.0f);
    const float oldF0 = audioData.f0[idx];
    const float oldDelta = (idx < static_cast<int>(audioData.deltaPitch.size()))
                               ? audioData.deltaPitch[idx]
                               : 0.0f;
    const bool oldVoiced = (idx < static_cast<int>(audioData.voicedMask.size()))
                               ? audioData.voicedMask[idx]
                               : false;
    const bool oldEdited = (idx < static_cast<int>(audioData.f0EditedMask.size()))
                               ? audioData.f0EditedMask[idx]
                               : false;

    float baseMidi = (idx < static_cast<int>(audioData.basePitch.size()))
                         ? audioData.basePitch[static_cast<size_t>(idx)]
                         : 0.0f;
    float newMidi = static_cast<float>(cents) / 100.0f;
    float newDelta = newMidi - baseMidi;

    auto it = drawingEditIndexByFrame.find(idx);
    if (it == drawingEditIndexByFrame.end()) {
      drawingEditIndexByFrame.emplace(idx, drawingEdits.size());
      drawingEdits.push_back(F0FrameEdit{idx, oldF0, newFreq, oldDelta,
                                         newDelta, oldVoiced, true,
                                         oldEdited, true});

      // Clear deltaPitch for any note containing this frame
      auto &notes = owner_.project->getNotes();
      for (auto &note : notes) {
        if (note.getStartFrame() <= idx && note.getEndFrame() > idx &&
            note.hasDeltaPitch()) {
          note.setDeltaPitch(std::vector<float>());
          break;
        }
      }
    } else {
      auto &e = drawingEdits[it->second];
      e.newF0 = newFreq;
      e.newDelta = newDelta;
      e.newVoiced = true;
      e.newEdited = true;
    }

    audioData.f0[idx] = newFreq;
    if (idx < static_cast<int>(audioData.deltaPitch.size()))
      audioData.deltaPitch[static_cast<size_t>(idx)] = newDelta;
    if (idx < static_cast<int>(audioData.voicedMask.size()))
      audioData.voicedMask[idx] = true;
    if (idx < static_cast<int>(audioData.f0EditedMask.size()))
      audioData.f0EditedMask[idx] = true;
  };

  // Only start a new curve if there's no active curve (first point of drawing)
  if (!activeDrawCurve) {
    startNewPitchCurve(frameIndex, midiCents);
    applyFrame(frameIndex, midiCents);
    return;
  }

  auto appendValue = [&](int idx, int cents) {
    if (!activeDrawCurve)
      return;

    const int curveStart = activeDrawCurve->localStart();
    auto &vals = activeDrawCurve->mutableValues();

    // Handle backward drawing: prepend values if idx < curveStart
    if (idx < curveStart) {
      const int prependCount = curveStart - idx;
      std::vector<int> newVals(static_cast<size_t>(prependCount), cents);
      newVals.insert(newVals.end(), vals.begin(), vals.end());
      activeDrawCurve->setValues(std::move(newVals));
      activeDrawCurve->setLocalStart(idx);
      return;
    }

    const int offset = idx - curveStart;
    if (offset < static_cast<int>(vals.size())) {
      vals[static_cast<std::size_t>(offset)] = cents;
      return;
    }

    while (static_cast<int>(vals.size()) < offset) {
      int fill = vals.empty() ? cents : vals.back();
      vals.push_back(fill);
    }
    vals.push_back(cents);
  };

  if (lastDrawFrame < 0) {
    appendValue(frameIndex, midiCents);
    applyFrame(frameIndex, midiCents);
  } else {
    int start = lastDrawFrame;
    int end = frameIndex;
    int startVal = lastDrawValueCents;
    int endVal = midiCents;

    if (start == end) {
      appendValue(frameIndex, midiCents);
      applyFrame(frameIndex, midiCents);
    } else {
      int step = (end > start) ? 1 : -1;
      int length = std::abs(end - start);
      for (int i = 0; i <= length; ++i) {
        int idx = start + i * step;
        float t = length == 0
                      ? 0.0f
                      : static_cast<float>(i) / static_cast<float>(length);
        float v = juce::jmap(t, 0.0f, 1.0f, static_cast<float>(startVal),
                             static_cast<float>(endVal));
        int cents = static_cast<int>(std::round(v));
        appendValue(idx, cents);
        applyFrame(idx, cents);
      }
    }
  }

  lastDrawFrame = frameIndex;
  lastDrawValueCents = midiCents;
}

void DrawHandler::showNoteResetMenu(float worldX, float worldY) {
  if (!owner_.project)
    return;

  Note *note = owner_.findNoteAt(worldX, worldY);
  if (!note || note->isRest())
    return;

  juce::PopupMenu menu;
  menu.addItem(1, "Reset Note to Original");

  menu.showMenuAsync(juce::PopupMenu::Options(),
                     [this, note](int result) {
                       if (result == 1)
                         resetNoteToOriginal(*note);
                     });
}

void DrawHandler::resetNoteToOriginal(Note &note) {
  if (!owner_.project)
    return;

  // Reset tool params
  note.resetToolParams();

  // Clear working deltaPitch so rebuild picks up from originalDeltaPitch
  note.setDeltaPitch({});

  // Clear f0EditedMask for this note's frame range
  auto &audioData = owner_.project->getAudioData();
  const int startFrame = note.getStartFrame();
  const int endFrame = note.getEndFrame();
  if (!audioData.f0EditedMask.empty()) {
    for (int i = startFrame;
         i < endFrame &&
         i < static_cast<int>(audioData.f0EditedMask.size());
         ++i) {
      if (i >= 0)
        audioData.f0EditedMask[static_cast<size_t>(i)] = false;
    }
  }

  // Rebuild and notify
  PitchCurveProcessor::rebuildBaseFromNotes(*owner_.project);
  if (owner_.onPitchEdited)
    owner_.onPitchEdited();
  if (owner_.onPitchEditFinished)
    owner_.onPitchEditFinished();
  owner_.repaint();
}

void DrawHandler::bakeNoteToolParams(Note &note) {
  // Mirrors the transform pipeline in composeRawDeltaFromNotes so that
  // the baked originalDeltaPitch produces the same output as the current
  // non-destructive tool params would.
  if (!owner_.project)
    return;

  // 1. Get source delta (same logic as getNoteSourceDelta in PitchCurveProcessor)
  const auto &rawSourceData = note.hasOriginalDeltaPitch()
      ? note.getOriginalDeltaPitch()
      : note.getDeltaPitch();
  if (rawSourceData.empty())
    return;

  const int outFrames = note.getDurationFrames();
  if (outFrames <= 0)
    return;

  // Resample to output (destination) length if stretched
  std::vector<float> transformedDelta =
      (static_cast<int>(rawSourceData.size()) == outFrames)
          ? rawSourceData
          : CurveResampler::resampleLinear(rawSourceData, outFrames);

  // 2. Apply FFT filters (high-pass / low-pass) if non-zero
  if (!transformedDelta.empty() &&
      (note.getHighPassFilterStrength() > 0.0001f ||
       note.getLowPassFilterStrength() > 0.0001f)) {
    const float sampleRate =
        owner_.project->getAudioData().sampleRate > 0
            ? static_cast<float>(owner_.project->getAudioData().sampleRate)
            : static_cast<float>(SAMPLE_RATE);
    const float frameRateHz = sampleRate / static_cast<float>(HOP_SIZE);
    const float nyquistHz = frameRateHz * 0.5f;

    const float lowpassHz =
        note.getLowPassFilterStrength() > 0.0001f
            ? FourierPitchFilter::lowpassStrengthToCutoffHz(
                  note.getLowPassFilterStrength(), frameRateHz)
            : nyquistHz;
    const float highpassHz =
        note.getHighPassFilterStrength() > 0.0001f
            ? FourierPitchFilter::highpassStrengthToCutoffHz(
                  note.getHighPassFilterStrength(), frameRateHz)
            : 0.0f;

    // Use the note-local delta as both context and crop for simplicity.
    // The full-context approach (buildPitchFilterNoteContextFromDense)
    // requires the dense source delta which is expensive to build here.
    // The result is close enough for a bake operation.
    transformedDelta =
        FourierPitchFilter::filterPitchCurve(
            transformedDelta, lowpassHz, highpassHz, frameRateHz,
            /*cropStartFrame=*/0,
            /*cropFrameCount=*/static_cast<int>(transformedDelta.size()))
            .filteredPitch;
  }

  // 3. Apply note-local transformations (variance, tilt, scale, offset)
  transformedDelta =
      PitchToolOperations::applyNoteLocalTransformations(
          transformedDelta, note);

  // 4. Store result back as originalDeltaPitch in source domain
  const int srcFrames = note.getSrcDurationFrames();
  if (srcFrames > 0 && srcFrames != outFrames)
    note.setOriginalDeltaPitch(
        CurveResampler::resampleLinear(transformedDelta, srcFrames));
  else
    note.setOriginalDeltaPitch(std::move(transformedDelta));

  // 5. Reset all non-destructive tool params and clear working deltaPitch
  note.resetToolParams();
  note.setDeltaPitch({});
}

void DrawHandler::startNewPitchCurve(int frameIndex, int midiCents) {
  drawCurves.push_back(std::make_unique<DrawCurve>(frameIndex, 1));
  activeDrawCurve = drawCurves.back().get();
  activeDrawCurve->appendValue(midiCents);
  lastDrawFrame = frameIndex;
  lastDrawValueCents = midiCents;
}
