#include "NoteSplitter.h"
#include "../../Utils/Constants.h"
#include "../../Utils/CurveResampler.h"
#include "../../Utils/HNSepCurveProcessor.h"
#include "../../Utils/PitchCurveProcessor.h"
#include <algorithm>
#include <cmath>
#include <numeric>

namespace
{
double computeSplitRatio(int splitFrame, int startFrame, int endFrame)
{
    const int durationFrames = endFrame - startFrame;
    if (durationFrames <= 0)
        return 0.5;

    const double ratio =
        static_cast<double>(splitFrame - startFrame) /
        static_cast<double>(durationFrames);
    return std::clamp(ratio, 0.0, 1.0);
}

int computeSplitIndex(double splitRatio, int size)
{
    if (size <= 0)
        return 0;

    const int splitIndex =
        static_cast<int>(std::lround(splitRatio * static_cast<double>(size)));
    return std::clamp(splitIndex, 0, size);
}

int computeMappedSplitFrame(double splitRatio, int startFrame, int endFrame)
{
    const int durationFrames = endFrame - startFrame;
    if (durationFrames <= 0)
        return startFrame;

    const int mappedFrame =
        startFrame +
        static_cast<int>(std::lround(splitRatio * static_cast<double>(durationFrames)));

    if (durationFrames > 1)
        return std::clamp(mappedFrame, startFrame + 1, endFrame - 1);

    return std::clamp(mappedFrame, startFrame, endFrame);
}

template <typename T>
void splitVectorAtRatio(const std::vector<T>& source,
                        double ratio,
                        std::vector<T>& left,
                        std::vector<T>& right)
{
    const int splitOffset =
        computeSplitIndex(ratio, static_cast<int>(source.size()));
    left.assign(source.begin(), source.begin() + splitOffset);
    right.assign(source.begin() + splitOffset, source.end());
}

template <typename T>
void splitVectorAtIndex(const std::vector<T>& source,
                        int splitIndex,
                        std::vector<T>& left,
                        std::vector<T>& right)
{
    const int clamped =
        std::clamp(splitIndex, 0, static_cast<int>(source.size()));
    left.assign(source.begin(), source.begin() + clamped);
    right.assign(source.begin() + clamped, source.end());
}

void refreshProjectAfterSplit(Project* project,
                              int dirtyStartFrame,
                              int dirtyEndFrame)
{
    if (!project)
        return;

    PitchCurveProcessor::rebuildBaseFromNotes(*project);
    HNSepCurveProcessor::rebuildCurvesFromNotes(*project);

    if (dirtyEndFrame > dirtyStartFrame)
    {
        const int f0Size = static_cast<int>(project->getAudioData().f0.size());
        if (f0Size > 0)
        {
            const int smoothStart = std::max(0, dirtyStartFrame - 60);
            const int smoothEnd = std::min(f0Size, dirtyEndFrame + 60);
            if (smoothEnd > smoothStart)
                project->setF0DirtyRange(smoothStart, smoothEnd);
        }

        project->setParamDirtyRange(dirtyStartFrame, dirtyEndFrame);
    }

    project->setModified(true);
}

void recenterNotePitchToAverageActualF0(Note& note)
{
    if (note.isRest())
        return;

    const auto& sourceCurve =
        note.hasOriginalDeltaPitch() ? note.getOriginalDeltaPitch()
                                     : note.getDeltaPitch();
    if (sourceCurve.empty())
        return;

    const float meanDelta =
        std::accumulate(sourceCurve.begin(), sourceCurve.end(), 0.0f) /
        static_cast<float>(sourceCurve.size());
    if (std::abs(meanDelta) <= 1.0e-5f)
        return;

    note.setMidiNote(note.getMidiNote() + meanDelta);

    if (note.hasOriginalDeltaPitch())
    {
        auto recentered = note.getOriginalDeltaPitch();
        for (auto& value : recentered)
            value -= meanDelta;
        note.setOriginalDeltaPitch(std::move(recentered));
    }

    if (note.hasDeltaPitch())
    {
        auto recentered = note.getDeltaPitch();
        for (auto& value : recentered)
            value -= meanDelta;
        note.setDeltaPitch(std::move(recentered));
    }
}
}

Note* NoteSplitter::findNoteAt(float x, float y) {
    if (!project || !coordMapper)
        return nullptr;

    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    float pixelsPerSemitone = coordMapper->getPixelsPerSemitone();

    for (auto& note : project->getNotes()) {
        if (note.isRest())
            continue;

        float noteX = framesToSeconds(note.getStartFrame()) * pixelsPerSecond;
        float noteW = framesToSeconds(note.getDurationFrames()) * pixelsPerSecond;
        float noteY = coordMapper->midiToY(note.getAdjustedMidiNote());
        float noteH = pixelsPerSemitone;

        if (x >= noteX && x < noteX + noteW && y >= noteY && y < noteY + noteH) {
            return &note;
        }
    }

    return nullptr;
}

bool NoteSplitter::splitNoteAtFrame(Note* note, int splitFrame) {
    if (!note || !project)
        return false;

    int startFrame = note->getStartFrame();
    int endFrame = note->getEndFrame();
    const int srcStartFrame = note->getSrcStartFrame();
    const int srcEndFrame = note->getSrcEndFrame();

    // Ensure split point is within note bounds (with margin)
    if (splitFrame <= startFrame + 5 || splitFrame >= endFrame - 5)
        return false;

    const double splitRatio = computeSplitRatio(splitFrame, startFrame, endFrame);
    const int srcSplitFrame =
        computeMappedSplitFrame(splitRatio, srcStartFrame, srcEndFrame);
    // Deterministic integer index for source-domain array splitting,
    // avoiding the double-rounding bug of ratio → lround → ratio → lround.
    const int srcSplitIndex = srcSplitFrame - srcStartFrame;

    // Store original note data for undo
    Note originalNote = *note;

    // Ensure harmonic/noise source clips exist before splitting
    if (!note->hasClipHarmonicWaveform()) {
        auto& audioData = project->getAudioData();
        if (audioData.harmonicWaveform.getNumSamples() > 0) {
            int srcStart = note->getSrcStartFrame() * HOP_SIZE;
            int srcEnd = note->getSrcEndFrame() * HOP_SIZE;
            srcStart = std::max(0, std::min(srcStart, audioData.harmonicWaveform.getNumSamples()));
            srcEnd = std::max(srcStart, std::min(srcEnd, audioData.harmonicWaveform.getNumSamples()));
            std::vector<float> hClip;
            hClip.reserve(static_cast<size_t>(srcEnd - srcStart));
            const float* harmonicPtr = audioData.harmonicWaveform.getReadPointer(0);
            for (int i = srcStart; i < srcEnd; ++i)
                hClip.push_back(harmonicPtr[i]);
            note->setClipHarmonicWaveform(std::move(hClip));
        }
    }

    if (!note->hasClipNoiseWaveform()) {
        auto& audioData = project->getAudioData();
        if (audioData.noiseWaveform.getNumSamples() > 0) {
            int srcStart = note->getSrcStartFrame() * HOP_SIZE;
            int srcEnd = note->getSrcEndFrame() * HOP_SIZE;
            srcStart = std::max(0, std::min(srcStart, audioData.noiseWaveform.getNumSamples()));
            srcEnd = std::max(srcStart, std::min(srcEnd, audioData.noiseWaveform.getNumSamples()));
            std::vector<float> nClip;
            nClip.reserve(static_cast<size_t>(srcEnd - srcStart));
            const float* noisePtr = audioData.noiseWaveform.getReadPointer(0);
            for (int i = srcStart; i < srcEnd; ++i)
                nClip.push_back(noisePtr[i]);
            note->setClipNoiseWaveform(std::move(nClip));
        }
    }

    // Ensure clip mel exists before splitting
    if (!note->hasClipMel()) {
        auto& audioData = project->getAudioData();
        if (!audioData.melSpectrogram.empty()) {
            int melSize = static_cast<int>(audioData.melSpectrogram.size());
            int melStart = std::max(0, std::min(srcStartFrame, melSize));
            int melEnd = std::max(melStart, std::min(srcEndFrame, melSize));
            if (melEnd > melStart) {
                std::vector<std::vector<float>> melClip(
                    audioData.melSpectrogram.begin() + melStart,
                    audioData.melSpectrogram.begin() + melEnd);
                note->setClipMel(std::move(melClip));
            }
        }
    }

    // Create the second note (right part)
    Note secondNote = originalNote;
    secondNote.setStartFrame(splitFrame);
    secondNote.setEndFrame(endFrame);
    secondNote.setSrcStartFrame(srcSplitFrame);
    secondNote.setSrcEndFrame(srcEndFrame);

    // Split clip mel if available (source-domain: use integer index)
    if (note->hasClipMel()) {
        const auto& mel = note->getClipMel();
        std::vector<std::vector<float>> leftMel, rightMel;
        splitVectorAtIndex(mel, srcSplitIndex, leftMel, rightMel);
        note->setClipMel(std::move(leftMel));
        secondNote.setClipMel(std::move(rightMel));
    }

    if (note->hasClipHarmonicWaveform()) {
        const auto& harmonicClip = note->getClipHarmonicWaveform();
        const int srcSampleIndex = srcSplitIndex * HOP_SIZE;
        std::vector<float> leftClip, rightClip;
        splitVectorAtIndex(harmonicClip, srcSampleIndex, leftClip, rightClip);
        note->setClipHarmonicWaveform(std::move(leftClip));
        secondNote.setClipHarmonicWaveform(std::move(rightClip));
    }

    if (note->hasClipNoiseWaveform()) {
        const auto& noiseClip = note->getClipNoiseWaveform();
        const int srcSampleIndex = srcSplitIndex * HOP_SIZE;
        std::vector<float> leftClip, rightClip;
        splitVectorAtIndex(noiseClip, srcSampleIndex, leftClip, rightClip);
        note->setClipNoiseWaveform(std::move(leftClip));
        secondNote.setClipNoiseWaveform(std::move(rightClip));
    }

    if (note->hasDeltaPitch()) {
        const auto& delta = note->getDeltaPitch();
        std::vector<float> leftDelta, rightDelta;
        splitVectorAtRatio(delta, splitRatio, leftDelta, rightDelta);
        note->setDeltaPitch(std::move(leftDelta));
        secondNote.setDeltaPitch(std::move(rightDelta));
    }

    // Split originalDeltaPitch if available (pristine/source-domain curve: use integer index)
    if (note->hasOriginalDeltaPitch()) {
        const auto& origDelta = note->getOriginalDeltaPitch();
        std::vector<float> leftDelta, rightDelta;
        splitVectorAtIndex(origDelta, srcSplitIndex, leftDelta, rightDelta);
        note->setOriginalDeltaPitch(std::move(leftDelta));
        secondNote.setOriginalDeltaPitch(std::move(rightDelta));
    } else if (originalNote.hasDeltaPitch()) {
        const int srcDuration = std::max(0, note->getSrcDurationFrames());
        if (srcDuration > 0) {
            const auto sourceDelta =
                CurveResampler::resampleLinear(originalNote.getDeltaPitch(),
                                               srcDuration);
            std::vector<float> leftDelta, rightDelta;
            splitVectorAtIndex(sourceDelta, srcSplitIndex, leftDelta, rightDelta);
            note->setOriginalDeltaPitch(std::move(leftDelta));
            secondNote.setOriginalDeltaPitch(std::move(rightDelta));
        }
    }

    auto splitFloatCurve = [](const std::vector<float>& curve,
                              double ratio,
                              std::vector<float>& leftCurve,
                              std::vector<float>& rightCurve)
    {
        splitVectorAtRatio(curve, ratio, leftCurve, rightCurve);
    };

    if (note->hasVoicingCurve()) {
        std::vector<float> leftCurve, rightCurve;
        splitFloatCurve(note->getVoicingCurve(), splitRatio, leftCurve, rightCurve);
        note->setVoicingCurve(std::move(leftCurve));
        secondNote.setVoicingCurve(std::move(rightCurve));
    }
    if (note->hasBreathCurve()) {
        std::vector<float> leftCurve, rightCurve;
        splitFloatCurve(note->getBreathCurve(), splitRatio, leftCurve, rightCurve);
        note->setBreathCurve(std::move(leftCurve));
        secondNote.setBreathCurve(std::move(rightCurve));
    }
    if (note->hasTensionCurve()) {
        std::vector<float> leftCurve, rightCurve;
        splitFloatCurve(note->getTensionCurve(), splitRatio, leftCurve, rightCurve);
        note->setTensionCurve(std::move(leftCurve));
        secondNote.setTensionCurve(std::move(rightCurve));
    }

    // NOTE: Do NOT recenter notes after split.  recenterNotePitchToAverageActualF0
    // would assign different midiNote values to each half, and the basePitch
    // cosine-smoothing in BasePitchCurve::generateForNotes would create a
    // pitch transition at the split boundary that did not exist before,
    // producing audible crackle.  Keeping the same midiNote ensures the
    // effective f0 (basePitch + deltaPitch) is unchanged by the split.

    // Modify the first note (left part)
    note->setEndFrame(splitFrame);
    note->setSrcEndFrame(srcSplitFrame);
    note->markDirty();
    note->markSynthDirty();
    secondNote.markDirty();
    secondNote.markSynthDirty();

    // Save first note BEFORE addNote (addNote may invalidate note pointer due to vector reallocation)
    Note firstNote = *note;

    // Add the second note to project
    project->addNote(secondNote);

    const int dirtyStartFrame = std::min(originalNote.getStartFrame(), firstNote.getStartFrame());
    const int dirtyEndFrame = std::max(originalNote.getEndFrame(), secondNote.getEndFrame());
    refreshProjectAfterSplit(project, dirtyStartFrame, dirtyEndFrame);

    auto onSplitStateApplied = [project = project,
                                dirtyStartFrame,
                                dirtyEndFrame]() mutable
    {
        refreshProjectAfterSplit(project, dirtyStartFrame, dirtyEndFrame);
    };

    if (undoManager) {
        auto action = std::make_unique<NoteSplitAction>(
            project, originalNote, firstNote, secondNote, onSplitStateApplied);
        undoManager->addAction(std::move(action));
    }

    if (onNoteSplit)
        onNoteSplit();

    return true;
}

bool NoteSplitter::splitNoteAtX(Note* note, float x) {
    if (!note || !coordMapper)
        return false;

    // Convert X coordinate to frame
    float pixelsPerSecond = coordMapper->getPixelsPerSecond();
    double time = x / pixelsPerSecond;
    int frame = static_cast<int>(time * SAMPLE_RATE / HOP_SIZE);

    return splitNoteAtFrame(note, frame);
}
