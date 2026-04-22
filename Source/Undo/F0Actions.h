#pragma once

#include "UndoableAction.h"
#include "F0FrameEdit.h"
#include <vector>
#include <functional>
#include <limits>

/**
 * Action for changing multiple F0 values (hand-drawing).
 */
class F0EditAction : public UndoableAction
{
public:
    F0EditAction(std::vector<float>* f0Array,
                 std::vector<float>* deltaPitchArray,
                 std::vector<bool>* voicedMask,
                 std::vector<bool>* f0EditedMask,
                 std::vector<F0FrameEdit> edits,
                 std::function<void(int, int)> onF0Changed = nullptr)
        : f0Array(f0Array), deltaPitchArray(deltaPitchArray), voicedMask(voicedMask),
          f0EditedMask(f0EditedMask), edits(std::move(edits)), onF0Changed(onF0Changed) {}

    void undo() override
    {
        if (!f0Array) return;
        int minIdx = std::numeric_limits<int>::max();
        int maxIdx = std::numeric_limits<int>::min();
        for (const auto& e : edits)
        {
            if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size())) {
                (*f0Array)[e.idx] = e.oldF0;
                minIdx = std::min(minIdx, e.idx);
                maxIdx = std::max(maxIdx, e.idx);
            }
            if (deltaPitchArray && e.idx >= 0 && e.idx < static_cast<int>(deltaPitchArray->size()))
                (*deltaPitchArray)[e.idx] = e.oldDelta;
            if (voicedMask && e.idx >= 0 && e.idx < static_cast<int>(voicedMask->size()))
                (*voicedMask)[e.idx] = e.oldVoiced;
            if (f0EditedMask && e.idx >= 0 && e.idx < static_cast<int>(f0EditedMask->size()))
                (*f0EditedMask)[e.idx] = e.oldEdited;
        }
        if (onF0Changed && minIdx <= maxIdx)
            onF0Changed(minIdx, maxIdx);
    }

    void redo() override
    {
        if (!f0Array) return;
        int minIdx = std::numeric_limits<int>::max();
        int maxIdx = std::numeric_limits<int>::min();
        for (const auto& e : edits)
        {
            if (e.idx >= 0 && e.idx < static_cast<int>(f0Array->size())) {
                (*f0Array)[e.idx] = e.newF0;
                minIdx = std::min(minIdx, e.idx);
                maxIdx = std::max(maxIdx, e.idx);
            }
            if (deltaPitchArray && e.idx >= 0 && e.idx < static_cast<int>(deltaPitchArray->size()))
                (*deltaPitchArray)[e.idx] = e.newDelta;
            if (voicedMask && e.idx >= 0 && e.idx < static_cast<int>(voicedMask->size()))
                (*voicedMask)[e.idx] = e.newVoiced;
            if (f0EditedMask && e.idx >= 0 && e.idx < static_cast<int>(f0EditedMask->size()))
                (*f0EditedMask)[e.idx] = e.newEdited;
        }
        if (onF0Changed && minIdx <= maxIdx)
            onF0Changed(minIdx, maxIdx);
    }

    juce::String getName() const override { return "Edit Pitch Curve"; }

private:
    std::vector<float>* f0Array;
    std::vector<float>* deltaPitchArray;
    std::vector<bool>* voicedMask;
    std::vector<bool>* f0EditedMask;
    std::vector<F0FrameEdit> edits;
    std::function<void(int, int)> onF0Changed;
};
