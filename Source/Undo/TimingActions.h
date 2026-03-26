#pragma once

#include "UndoableAction.h"
#include "../Models/Note.h"
#include "../Models/Project.h"
#include <vector>
#include <functional>

/**
 * Action for stretching note timing between two adjacent notes.
 */
class NoteTimingStretchAction : public UndoableAction
{
public:
    NoteTimingStretchAction(Note* leftNote,
                            Note* rightNote,
                            std::vector<float>* deltaPitchArray,
                            std::vector<bool>* voicedMaskArray,
                            std::vector<std::vector<float>>* melSpectrogram,
                            int rangeStart,
                            int rangeEnd,
                            int oldLeftStart, int oldLeftEnd,
                            int oldRightStart, int oldRightEnd,
                            int newLeftStart, int newLeftEnd,
                            int newRightStart, int newRightEnd,
                            std::vector<float> oldDelta,
                            std::vector<float> newDelta,
                            std::vector<bool> oldVoiced,
                            std::vector<bool> newVoiced,
                            std::vector<std::vector<float>> oldMel,
                            std::vector<std::vector<float>> newMel,
                            std::function<void(int, int)> onRangeChanged = nullptr)
        : left(leftNote), right(rightNote),
          deltaPitchArray(deltaPitchArray), voicedMaskArray(voicedMaskArray),
          melSpectrogram(melSpectrogram),
          rangeStart(rangeStart), rangeEnd(rangeEnd),
          oldLeftStart(oldLeftStart), oldLeftEnd(oldLeftEnd),
          oldRightStart(oldRightStart), oldRightEnd(oldRightEnd),
          newLeftStart(newLeftStart), newLeftEnd(newLeftEnd),
          newRightStart(newRightStart), newRightEnd(newRightEnd),
          oldDelta(std::move(oldDelta)), newDelta(std::move(newDelta)),
          oldVoiced(std::move(oldVoiced)), newVoiced(std::move(newVoiced)),
          oldMel(std::move(oldMel)), newMel(std::move(newMel)),
          onRangeChanged(std::move(onRangeChanged)) {}

    void undo() override
    {
        applyState(oldLeftStart, oldLeftEnd, oldRightStart, oldRightEnd,
                   oldDelta, oldVoiced, oldMel);
    }

    void redo() override
    {
        applyState(newLeftStart, newLeftEnd, newRightStart, newRightEnd,
                   newDelta, newVoiced, newMel);
    }

    juce::String getName() const override { return "Stretch Note Timing"; }

private:
    void applyState(int leftStart, int leftEnd,
                    int rightStart, int rightEnd,
                    const std::vector<float>& delta,
                    const std::vector<bool>& voiced,
                    const std::vector<std::vector<float>>& mel)
    {
        if (left) {
            left->setStartFrame(leftStart);
            left->setEndFrame(leftEnd);
            left->markDirty();
            left->markSynthDirty();
        }
        if (right) {
            right->setStartFrame(rightStart);
            right->setEndFrame(rightEnd);
            right->markDirty();
            right->markSynthDirty();
        }

        if (deltaPitchArray && rangeEnd > rangeStart &&
            delta.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (deltaPitchArray->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*deltaPitchArray)[static_cast<size_t>(i)] =
                        delta[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (voicedMaskArray && rangeEnd > rangeStart &&
            voiced.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (voicedMaskArray->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*voicedMaskArray)[static_cast<size_t>(i)] =
                        voiced[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (melSpectrogram && rangeEnd > rangeStart &&
            mel.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (melSpectrogram->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*melSpectrogram)[static_cast<size_t>(i)] =
                        mel[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (onRangeChanged && rangeEnd > rangeStart)
            onRangeChanged(rangeStart, rangeEnd);
    }

    Note* left = nullptr;
    Note* right = nullptr;
    std::vector<float>* deltaPitchArray = nullptr;
    std::vector<bool>* voicedMaskArray = nullptr;
    std::vector<std::vector<float>>* melSpectrogram = nullptr;
    int rangeStart = 0;
    int rangeEnd = 0;
    int oldLeftStart = 0;
    int oldLeftEnd = 0;
    int oldRightStart = 0;
    int oldRightEnd = 0;
    int newLeftStart = 0;
    int newLeftEnd = 0;
    int newRightStart = 0;
    int newRightEnd = 0;
    std::vector<float> oldDelta;
    std::vector<float> newDelta;
    std::vector<bool> oldVoiced;
    std::vector<bool> newVoiced;
    std::vector<std::vector<float>> oldMel;
    std::vector<std::vector<float>> newMel;
    std::function<void(int, int)> onRangeChanged;
};

/**
 * Action for ripple-stretching note timing (left note resampled, right side shifted).
 */
class NoteTimingRippleAction : public UndoableAction
{
public:
    NoteTimingRippleAction(Note* leftNote,
                           Note* rightNote,
                           std::vector<Note*> rippleNotes,
                           std::vector<float>* deltaPitchArray,
                           std::vector<bool>* voicedMaskArray,
                           std::vector<std::vector<float>>* melSpectrogram,
                           int rangeStart,
                           int rangeEnd,
                           int oldLeftStart, int oldLeftEnd,
                           int newLeftStart, int newLeftEnd,
                           std::vector<int> oldNoteStarts,
                           std::vector<int> oldNoteEnds,
                           std::vector<int> newNoteStarts,
                           std::vector<int> newNoteEnds,
                           std::vector<float> oldDelta,
                           std::vector<float> newDelta,
                           std::vector<bool> oldVoiced,
                           std::vector<bool> newVoiced,
                           std::vector<std::vector<float>> oldMel,
                           std::vector<std::vector<float>> newMel,
                           std::function<void(int, int)> onRangeChanged = nullptr)
        : left(leftNote), right(rightNote), rippleNotes(std::move(rippleNotes)),
          deltaPitchArray(deltaPitchArray), voicedMaskArray(voicedMaskArray),
          melSpectrogram(melSpectrogram),
          rangeStart(rangeStart), rangeEnd(rangeEnd),
          oldLeftStart(oldLeftStart), oldLeftEnd(oldLeftEnd),
          newLeftStart(newLeftStart), newLeftEnd(newLeftEnd),
          oldNoteStarts(std::move(oldNoteStarts)),
          oldNoteEnds(std::move(oldNoteEnds)),
          newNoteStarts(std::move(newNoteStarts)),
          newNoteEnds(std::move(newNoteEnds)),
          oldDelta(std::move(oldDelta)), newDelta(std::move(newDelta)),
          oldVoiced(std::move(oldVoiced)), newVoiced(std::move(newVoiced)),
          oldMel(std::move(oldMel)), newMel(std::move(newMel)),
          onRangeChanged(std::move(onRangeChanged)) {}

    void undo() override {
        applyState(oldLeftStart, oldLeftEnd, oldNoteStarts, oldNoteEnds,
                   oldDelta, oldVoiced, oldMel);
    }
    void redo() override {
        applyState(newLeftStart, newLeftEnd, newNoteStarts, newNoteEnds,
                   newDelta, newVoiced, newMel);
    }

    juce::String getName() const override { return "Ripple Stretch Timing"; }

private:
    void applyState(int leftStart, int leftEnd,
                    const std::vector<int>& noteStarts,
                    const std::vector<int>& noteEnds,
                    const std::vector<float>& delta,
                    const std::vector<bool>& voiced,
                    const std::vector<std::vector<float>>& mel)
    {
        if (left) {
            left->setStartFrame(leftStart);
            left->setEndFrame(leftEnd);
            left->markDirty();
            left->markSynthDirty();
        }
        if (right) {
            right->markDirty();
            right->markSynthDirty();
        }

        for (size_t i = 0; i < rippleNotes.size() && i < noteStarts.size() && i < noteEnds.size(); ++i) {
            if (rippleNotes[i]) {
                rippleNotes[i]->setStartFrame(noteStarts[i]);
                rippleNotes[i]->setEndFrame(noteEnds[i]);
                rippleNotes[i]->markDirty();
                rippleNotes[i]->markSynthDirty();
            }
        }

        if (deltaPitchArray && rangeEnd > rangeStart &&
            delta.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (deltaPitchArray->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*deltaPitchArray)[static_cast<size_t>(i)] =
                        delta[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (voicedMaskArray && rangeEnd > rangeStart &&
            voiced.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (voicedMaskArray->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*voicedMaskArray)[static_cast<size_t>(i)] =
                        voiced[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (melSpectrogram && rangeEnd > rangeStart &&
            mel.size() == static_cast<size_t>(rangeEnd - rangeStart)) {
            if (melSpectrogram->size() >= static_cast<size_t>(rangeEnd)) {
                for (int i = rangeStart; i < rangeEnd; ++i)
                    (*melSpectrogram)[static_cast<size_t>(i)] =
                        mel[static_cast<size_t>(i - rangeStart)];
            }
        }

        if (onRangeChanged && rangeEnd > rangeStart)
            onRangeChanged(rangeStart, rangeEnd);
    }

    Note* left = nullptr;
    Note* right = nullptr;
    std::vector<Note*> rippleNotes;
    std::vector<float>* deltaPitchArray = nullptr;
    std::vector<bool>* voicedMaskArray = nullptr;
    std::vector<std::vector<float>>* melSpectrogram = nullptr;
    int rangeStart = 0;
    int rangeEnd = 0;
    int oldLeftStart = 0;
    int oldLeftEnd = 0;
    int newLeftStart = 0;
    int newLeftEnd = 0;
    std::vector<int> oldNoteStarts;
    std::vector<int> oldNoteEnds;
    std::vector<int> newNoteStarts;
    std::vector<int> newNoteEnds;
    std::vector<float> oldDelta;
    std::vector<float> newDelta;
    std::vector<bool> oldVoiced;
    std::vector<bool> newVoiced;
    std::vector<std::vector<float>> oldMel;
    std::vector<std::vector<float>> newMel;
    std::function<void(int, int)> onRangeChanged;
};

class WarpMarkerStateAction : public UndoableAction
{
public:
    WarpMarkerStateAction(Project* project,
                          std::vector<Project::WarpMarker> oldMarkers,
                          std::vector<Project::WarpMarker> newMarkers,
                          std::function<void(const std::vector<Project::WarpMarker>&)> onApply = nullptr)
        : project(project),
          oldMarkers(std::move(oldMarkers)),
          newMarkers(std::move(newMarkers)),
          onApply(std::move(onApply)) {}

    void undo() override { apply(oldMarkers); }
    void redo() override { apply(newMarkers); }
    juce::String getName() const override { return "Edit Warp Markers"; }

private:
    void apply(const std::vector<Project::WarpMarker>& markers)
    {
        if (!project)
            return;
        project->setWarpMarkers(markers);
        if (onApply)
            onApply(markers);
    }

    Project* project = nullptr;
    std::vector<Project::WarpMarker> oldMarkers;
    std::vector<Project::WarpMarker> newMarkers;
    std::function<void(const std::vector<Project::WarpMarker>&)> onApply;
};
