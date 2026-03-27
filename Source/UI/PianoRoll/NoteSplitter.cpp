#include "NoteSplitter.h"
#include "../../Utils/Constants.h"
#include "../../Utils/CurveResampler.h"
#include "../../Utils/HNSepCurveProcessor.h"
#include "../../Utils/PitchCurveProcessor.h"
#include <algorithm>
#include <cmath>

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
    const double srcSplitRatio =
        computeSplitRatio(srcSplitFrame, srcStartFrame, srcEndFrame);

    // Store original note data for undo
    Note originalNote = *note;

    // Ensure clip waveform exists before splitting
    if (!note->hasClipWaveform()) {
        auto& audioData = project->getAudioData();
        if (audioData.waveform.getNumSamples() > 0) {
            int startSample = startFrame * HOP_SIZE;
            int endSample = endFrame * HOP_SIZE;
            startSample = std::max(0, std::min(startSample, audioData.waveform.getNumSamples()));
            endSample = std::max(startSample, std::min(endSample, audioData.waveform.getNumSamples()));
            std::vector<float> clip;
            clip.reserve(static_cast<size_t>(endSample - startSample));
            const float* src = audioData.waveform.getReadPointer(0);
            for (int i = startSample; i < endSample; ++i)
                clip.push_back(src[i]);
            note->setClipWaveform(std::move(clip));
        }
    }

    // Ensure source clip waveform exists before splitting
    if (!note->hasSrcClipWaveform()) {
        auto& audioData = project->getAudioData();
        if (audioData.originalWaveform.getNumSamples() > 0) {
            int srcStart = note->getSrcStartFrame() * HOP_SIZE;
            int srcEnd = note->getSrcEndFrame() * HOP_SIZE;
            srcStart = std::max(0, std::min(srcStart, audioData.originalWaveform.getNumSamples()));
            srcEnd = std::max(srcStart, std::min(srcEnd, audioData.originalWaveform.getNumSamples()));
            std::vector<float> srcClip;
            srcClip.reserve(static_cast<size_t>(srcEnd - srcStart));
            const float* origSrc = audioData.originalWaveform.getReadPointer(0);
            for (int i = srcStart; i < srcEnd; ++i)
                srcClip.push_back(origSrc[i]);
            note->setSrcClipWaveform(std::move(srcClip));
        }
    }

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

    // Split clip waveform if available
    if (note->hasClipWaveform()) {
        const auto& clip = note->getClipWaveform();
        std::vector<float> leftClip, rightClip;
        splitVectorAtRatio(clip, splitRatio, leftClip, rightClip);
        note->setClipWaveform(std::move(leftClip));
        secondNote.setClipWaveform(std::move(rightClip));
    }

    // Split source clip waveform if available
    if (note->hasSrcClipWaveform()) {
        const auto& srcClip = note->getSrcClipWaveform();
        std::vector<float> leftSrcClip, rightSrcClip;
        splitVectorAtRatio(srcClip, srcSplitRatio, leftSrcClip, rightSrcClip);
        note->setSrcClipWaveform(std::move(leftSrcClip));
        secondNote.setSrcClipWaveform(std::move(rightSrcClip));
    }

    // Split clip mel if available
    if (note->hasClipMel()) {
        const auto& mel = note->getClipMel();
        std::vector<std::vector<float>> leftMel, rightMel;
        splitVectorAtRatio(mel, srcSplitRatio, leftMel, rightMel);
        note->setClipMel(std::move(leftMel));
        secondNote.setClipMel(std::move(rightMel));
    }

    if (note->hasClipHarmonicWaveform()) {
        const auto& harmonicClip = note->getClipHarmonicWaveform();
        std::vector<float> leftClip, rightClip;
        splitVectorAtRatio(harmonicClip, srcSplitRatio, leftClip, rightClip);
        note->setClipHarmonicWaveform(std::move(leftClip));
        secondNote.setClipHarmonicWaveform(std::move(rightClip));
    }

    if (note->hasClipNoiseWaveform()) {
        const auto& noiseClip = note->getClipNoiseWaveform();
        std::vector<float> leftClip, rightClip;
        splitVectorAtRatio(noiseClip, srcSplitRatio, leftClip, rightClip);
        note->setClipNoiseWaveform(std::move(leftClip));
        secondNote.setClipNoiseWaveform(std::move(rightClip));
    }

    if (!note->getF0Values().empty()) {
        const auto& f0Values = note->getF0Values();
        std::vector<float> leftF0, rightF0;
        splitVectorAtRatio(f0Values, srcSplitRatio, leftF0, rightF0);
        note->setF0Values(std::move(leftF0));
        secondNote.setF0Values(std::move(rightF0));
    }

    if (note->hasDeltaPitch()) {
        const auto& delta = note->getDeltaPitch();
        std::vector<float> leftDelta, rightDelta;
        splitVectorAtRatio(delta, splitRatio, leftDelta, rightDelta);
        note->setDeltaPitch(std::move(leftDelta));
        secondNote.setDeltaPitch(std::move(rightDelta));
    }

    // Split originalDeltaPitch if available (pristine/source-domain curve)
    if (note->hasOriginalDeltaPitch()) {
        const auto& origDelta = note->getOriginalDeltaPitch();
        std::vector<float> leftDelta, rightDelta;
        splitVectorAtRatio(origDelta, srcSplitRatio, leftDelta, rightDelta);
        note->setOriginalDeltaPitch(std::move(leftDelta));
        secondNote.setOriginalDeltaPitch(std::move(rightDelta));
    } else if (originalNote.hasDeltaPitch()) {
        const int srcDuration = std::max(0, note->getSrcDurationFrames());
        if (srcDuration > 0) {
            const auto sourceDelta =
                CurveResampler::resampleLinear(originalNote.getDeltaPitch(),
                                               srcDuration);
            std::vector<float> leftDelta, rightDelta;
            splitVectorAtRatio(sourceDelta, srcSplitRatio, leftDelta, rightDelta);
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
    if (note->hasSourceVoicingCurve()) {
        std::vector<float> leftCurve, rightCurve;
        splitFloatCurve(note->getSourceVoicingCurve(), srcSplitRatio, leftCurve,
                        rightCurve);
        note->setSourceVoicingCurve(std::move(leftCurve));
        secondNote.setSourceVoicingCurve(std::move(rightCurve));
    }
    if (note->hasSourceBreathCurve()) {
        std::vector<float> leftCurve, rightCurve;
        splitFloatCurve(note->getSourceBreathCurve(), srcSplitRatio, leftCurve,
                        rightCurve);
        note->setSourceBreathCurve(std::move(leftCurve));
        secondNote.setSourceBreathCurve(std::move(rightCurve));
    }
    if (note->hasSourceTensionCurve()) {
        std::vector<float> leftCurve, rightCurve;
        splitFloatCurve(note->getSourceTensionCurve(), srcSplitRatio, leftCurve,
                        rightCurve);
        note->setSourceTensionCurve(std::move(leftCurve));
        secondNote.setSourceTensionCurve(std::move(rightCurve));
    }

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
