#include "HNSepCurveProcessor.h"
#include "CurveResampler.h"
#include "MelSpectrogram.h"
#include "Constants.h"
#include "WarpMarkerProcessor.h"
#include "../Audio/Synthesis/StretchProcessor.h"
#include "../Audio/TensionProcessor.h"

#include <algorithm>
#include <cmath>

namespace
{
    bool hasIdentityWarpMap(Project& project)
    {
        const auto warpMap = WarpMarkerProcessor::buildWarpMapWithEndpoints(
            project, project.getWarpMarkers());
        return warpMap.size() == 2 &&
               warpMap.front().sourceFrame == 0 &&
               warpMap.front().outputFrame == 0 &&
               warpMap.back().sourceFrame == warpMap.back().outputFrame;
    }

    std::vector<Project::WarpMarker> buildCurrentOutputWarpMap(Project& project)
    {
        const auto committedMap = WarpMarkerProcessor::buildWarpMapWithEndpoints(
            project, project.getWarpMarkers());
        const int outputFrames = project.getFrameCount();

        std::vector<Project::WarpMarker> noteMarkers;
        bool hasNotes = false;
        bool committedMapMatchesNotes = committedMap.size() >= 2 &&
                                        committedMap.back().outputFrame ==
                                            outputFrames;
        for (const auto& note : project.getNotes())
        {
            if (note.isRest())
                continue;
            hasNotes = true;
            noteMarkers.push_back({note.getSrcStartFrame(), note.getStartFrame()});
            noteMarkers.push_back({note.getSrcEndFrame(), note.getEndFrame()});

            const int mappedStart = static_cast<int>(std::round(
                StretchProcessor::mapFrame(
                    committedMap, static_cast<float>(note.getSrcStartFrame()))));
            const int mappedEnd = static_cast<int>(std::round(
                StretchProcessor::mapFrame(
                    committedMap, static_cast<float>(note.getSrcEndFrame()))));
            if (mappedStart != note.getStartFrame() ||
                mappedEnd != note.getEndFrame())
            {
                committedMapMatchesNotes = false;
            }
        }

        if (!hasNotes || committedMapMatchesNotes)
            return committedMap;

        return WarpMarkerProcessor::buildWarpMapWithEndpoints(project,
                                                              noteMarkers);
    }

    void ensureCurveSizes(EditedData& editedData, int totalFrames)
    {
        if (totalFrames <= 0)
            return;

        if (editedData.voicingCurve.size() != static_cast<size_t>(totalFrames))
            editedData.voicingCurve.assign(static_cast<size_t>(totalFrames),
                                          HNSepCurveProcessor::kDefaultVoicing);
        if (editedData.breathCurve.size() != static_cast<size_t>(totalFrames))
            editedData.breathCurve.assign(static_cast<size_t>(totalFrames),
                                         HNSepCurveProcessor::kDefaultBreath);
        if (editedData.tensionCurve.size() != static_cast<size_t>(totalFrames))
            editedData.tensionCurve.assign(static_cast<size_t>(totalFrames),
                                          HNSepCurveProcessor::kDefaultTension);
    }

    std::vector<float> fitCurveToLength(const std::vector<float>& source,
                                        int targetLength,
                                        float defaultValue)
    {
        if (targetLength <= 0)
            return {};
        if (source.empty())
            return std::vector<float>(static_cast<size_t>(targetLength), defaultValue);
        if (static_cast<int>(source.size()) == targetLength)
            return source;
        return CurveResampler::resampleLinear(source, targetLength);
    }

    void writeCurveRange(std::vector<float>& dest,
                         const std::vector<float>& source,
                         int startFrame,
                         int endFrame,
                         float defaultValue)
    {
        const int totalFrames = static_cast<int>(dest.size());
        const int clampedStart = std::max(0, startFrame);
        const int clampedEnd = std::min(totalFrames, endFrame);
        if (clampedEnd <= clampedStart)
            return;

        std::fill(dest.begin() + clampedStart, dest.begin() + clampedEnd,
                  defaultValue);

        const int length = clampedEnd - clampedStart;
        const auto fitted = fitCurveToLength(source, length, defaultValue);
        for (int i = 0; i < length; ++i)
            dest[static_cast<size_t>(clampedStart + i)] = fitted[static_cast<size_t>(i)];
    }

    bool curveDiffersFrom(const std::vector<float>& curve,
                          int startFrame,
                          int endFrame,
                          float defaultValue)
    {
        const int totalFrames = static_cast<int>(curve.size());
        const int clampedStart = std::max(0, startFrame);
        const int clampedEnd = std::min(totalFrames, endFrame);
        for (int i = clampedStart; i < clampedEnd; ++i)
        {
            if (std::abs(curve[static_cast<size_t>(i)] - defaultValue) > 0.001f)
                return true;
        }
        return false;
    }
} // namespace

namespace HNSepCurveProcessor
{
    void initializeCurves(Project& project)
    {
        auto& editedData = project.getEditedData();
        const int totalFrames = project.getAudioData().getNumFrames();
        ensureCurveSizes(editedData, totalFrames);

        for (auto& note : project.getNotes())
        {
            if (note.isRest())
                continue;

            const int noteLength = note.getDurationFrames();
            if (noteLength <= 0)
                continue;

            const int startFrame = note.getStartFrame();
            const int endFrame = std::min(startFrame + noteLength, totalFrames);
            const int sliceLength = std::max(0, endFrame - startFrame);

            if (!note.hasVoicingCurve())
            {
                if (sliceLength > 0 &&
                    startFrame >= 0 &&
                    endFrame <= static_cast<int>(editedData.voicingCurve.size()))
                {
                    note.setVoicingCurve(std::vector<float>(
                        editedData.voicingCurve.begin() + startFrame,
                        editedData.voicingCurve.begin() + endFrame));
                }
                else
                {
                    note.setVoicingCurve(std::vector<float>(
                        static_cast<size_t>(noteLength), kDefaultVoicing));
                }
            }

            if (!note.hasBreathCurve())
            {
                if (sliceLength > 0 &&
                    startFrame >= 0 &&
                    endFrame <= static_cast<int>(editedData.breathCurve.size()))
                {
                    note.setBreathCurve(std::vector<float>(
                        editedData.breathCurve.begin() + startFrame,
                        editedData.breathCurve.begin() + endFrame));
                }
                else
                {
                    note.setBreathCurve(std::vector<float>(
                        static_cast<size_t>(noteLength), kDefaultBreath));
                }
            }

            if (!note.hasTensionCurve())
            {
                if (sliceLength > 0 &&
                    startFrame >= 0 &&
                    endFrame <= static_cast<int>(editedData.tensionCurve.size()))
                {
                    note.setTensionCurve(std::vector<float>(
                        editedData.tensionCurve.begin() + startFrame,
                        editedData.tensionCurve.begin() + endFrame));
                }
                else
                {
                    note.setTensionCurve(std::vector<float>(
                        static_cast<size_t>(noteLength), kDefaultTension));
                }
            }
        }

        rebuildCurvesFromNotes(project);
    }

    void rebuildCurvesFromNotes(Project& project)
    {
        auto& editedData = project.getEditedData();
        const int totalFrames = project.getAudioData().getNumFrames();
        ensureCurveSizes(editedData, totalFrames);

        std::fill(editedData.voicingCurve.begin(), editedData.voicingCurve.end(),
                  kDefaultVoicing);
        std::fill(editedData.breathCurve.begin(), editedData.breathCurve.end(),
                  kDefaultBreath);
        std::fill(editedData.tensionCurve.begin(), editedData.tensionCurve.end(),
                  kDefaultTension);

        for (const auto& note : project.getNotes())
        {
            if (note.isRest())
                continue;

            writeCurveRange(editedData.voicingCurve, note.getVoicingCurve(),
                            note.getStartFrame(), note.getEndFrame(),
                            kDefaultVoicing);
            writeCurveRange(editedData.breathCurve, note.getBreathCurve(),
                            note.getStartFrame(), note.getEndFrame(),
                            kDefaultBreath);
            writeCurveRange(editedData.tensionCurve, note.getTensionCurve(),
                            note.getStartFrame(), note.getEndFrame(),
                            kDefaultTension);
        }

        project.notifyListeners(ProjectChangeType::NoteCurveChanged);
    }

    void rebuildCurvesForRange(Project& project, int startFrame, int endFrame)
    {
        auto& editedData = project.getEditedData();
        const int totalFrames = project.getAudioData().getNumFrames();
        ensureCurveSizes(editedData, totalFrames);

        const int clampedStart = std::max(0, startFrame);
        const int clampedEnd = std::min(totalFrames, endFrame);
        if (clampedEnd <= clampedStart)
            return;

        std::fill(editedData.voicingCurve.begin() + clampedStart,
                  editedData.voicingCurve.begin() + clampedEnd, kDefaultVoicing);
        std::fill(editedData.breathCurve.begin() + clampedStart,
                  editedData.breathCurve.begin() + clampedEnd, kDefaultBreath);
        std::fill(editedData.tensionCurve.begin() + clampedStart,
                  editedData.tensionCurve.begin() + clampedEnd, kDefaultTension);

        for (const auto& note : project.getNotes())
        {
            if (note.isRest())
                continue;

            const int overlapStart = std::max(clampedStart, note.getStartFrame());
            const int overlapEnd = std::min(clampedEnd, note.getEndFrame());
            if (overlapEnd <= overlapStart)
                continue;

            const int noteLength = note.getDurationFrames();
            if (noteLength <= 0)
                continue;

            const auto voicing = fitCurveToLength(note.getVoicingCurve(),
                                                   noteLength, kDefaultVoicing);
            const auto breath = fitCurveToLength(note.getBreathCurve(),
                                                  noteLength, kDefaultBreath);
            const auto tension = fitCurveToLength(note.getTensionCurve(),
                                                   noteLength, kDefaultTension);

            for (int frame = overlapStart; frame < overlapEnd; ++frame)
            {
                const int localFrame = frame - note.getStartFrame();
                editedData.voicingCurve[static_cast<size_t>(frame)] =
                    voicing[static_cast<size_t>(localFrame)];
                editedData.breathCurve[static_cast<size_t>(frame)] =
                    breath[static_cast<size_t>(localFrame)];
                editedData.tensionCurve[static_cast<size_t>(frame)] =
                    tension[static_cast<size_t>(localFrame)];
            }
        }

        project.notifyListeners(ProjectChangeType::NoteCurveChanged, -1, startFrame, endFrame);
    }

    void extractNoteCurvesFromMaster(Project& project)
    {
        auto& editedData = project.getEditedData();
        const int totalFrames = project.getAudioData().getNumFrames();
        ensureCurveSizes(editedData, totalFrames);

        for (auto& note : project.getNotes())
        {
            if (note.isRest())
                continue;

            const int startFrame = std::max(0, note.getStartFrame());
            const int endFrame = std::min(totalFrames, note.getEndFrame());
            const int length = std::max(0, endFrame - startFrame);
            if (length <= 0)
                continue;

            note.setVoicingCurve(std::vector<float>(
                editedData.voicingCurve.begin() + startFrame,
                editedData.voicingCurve.begin() + endFrame));
            note.setBreathCurve(std::vector<float>(
                editedData.breathCurve.begin() + startFrame,
                editedData.breathCurve.begin() + endFrame));
            note.setTensionCurve(std::vector<float>(
                editedData.tensionCurve.begin() + startFrame,
                editedData.tensionCurve.begin() + endFrame));
        }
    }

    bool hasActiveEdits(const Project& project, int startFrame, int endFrame)
    {
        const auto& editedData = project.getEditedData();
        return curveDiffersFrom(editedData.voicingCurve, startFrame, endFrame,
                                kDefaultVoicing) ||
               curveDiffersFrom(editedData.breathCurve, startFrame, endFrame,
                                kDefaultBreath) ||
               curveDiffersFrom(editedData.tensionCurve, startFrame, endFrame,
                                kDefaultTension);
    }

    void ensureNoteHNClips(Project& project)
    {
        const auto& audioData = project.getAudioData();
        const int hSamples = audioData.harmonicWaveform.getNumSamples();
        const int nSamples = audioData.noiseWaveform.getNumSamples();
        if (hSamples == 0 && nSamples == 0)
            return;

        const float* harmonicPtr = hSamples > 0
            ? audioData.harmonicWaveform.getReadPointer(0) : nullptr;
        const float* noisePtr = nSamples > 0
            ? audioData.noiseWaveform.getReadPointer(0) : nullptr;

        for (auto& note : project.getNotes())
        {
            if (note.isRest())
                continue;

            const int srcStart = note.getSrcStartFrame() * HOP_SIZE;
            const int srcEnd = note.getSrcEndFrame() * HOP_SIZE;

            if (!note.hasClipHarmonicWaveform() && harmonicPtr != nullptr)
            {
                const int s = std::max(0, std::min(srcStart, hSamples));
                const int e = std::max(s, std::min(srcEnd, hSamples));
                if (e > s)
                {
                    note.setClipHarmonicWaveform(
                        std::vector<float>(harmonicPtr + s, harmonicPtr + e));
                }
            }

            if (!note.hasClipNoiseWaveform() && noisePtr != nullptr)
            {
                const int s = std::max(0, std::min(srcStart, nSamples));
                const int e = std::max(s, std::min(srcEnd, nSamples));
                if (e > s)
                {
                    note.setClipNoiseWaveform(
                        std::vector<float>(noisePtr + s, noisePtr + e));
                }
            }
        }
    }

    void recomputeMelForRange(Project& project, int startFrame, int endFrame)
    {
        auto& audioData = project.getAudioData();
        const auto& editedData = project.getEditedData();
        if (audioData.sourceMelSpectrogram.empty() &&
            !audioData.melSpectrogram.empty() &&
            hasIdentityWarpMap(project))
        {
            audioData.sourceMelSpectrogram = audioData.melSpectrogram;
        }
        if (audioData.sourceMelSpectrogram.empty())
            return;

        const bool hasGlobalHNSep =
            audioData.harmonicWaveform.getNumSamples() > 0 &&
            audioData.noiseWaveform.getNumSamples() > 0;
        if (!hasGlobalHNSep)
            return;
        if (editedData.voicingCurve.empty() || editedData.breathCurve.empty() ||
            editedData.tensionCurve.empty())
            return;
        if (!hasActiveEdits(project, startFrame, endFrame))
            return;

        const int numMels =
            static_cast<int>(audioData.sourceMelSpectrogram.front().size());

        TensionProcessor tensionProc;
        MelSpectrogram melComputer(audioData.sampleRate);
        bool updatedSourceMel = false;

        for (const auto& note : project.getNotes())
        {
            if (note.isRest())
                continue;

            const int noteOutStart = note.getStartFrame();
            const int noteOutEnd = note.getEndFrame();
            if (noteOutEnd <= startFrame || noteOutStart >= endFrame)
                continue;

            if (!note.hasClipHarmonicWaveform() || !note.hasClipNoiseWaveform())
                continue;

            const auto& clipH = note.getClipHarmonicWaveform();
            const auto& clipN = note.getClipNoiseWaveform();
            const int srcDurationFrames = note.getSrcDurationFrames();
            const int outDurationFrames = note.getDurationFrames();
            if (srcDurationFrames <= 0 || outDurationFrames <= 0)
                continue;
            const int srcSamples = static_cast<int>(clipH.size());
            if (srcSamples <= 0)
                continue;

            std::vector<float> srcVoicing, srcBreath, srcTension;
            if (note.hasVoicingCurve())
                srcVoicing = CurveResampler::resampleLinear(
                    note.getVoicingCurve(), srcDurationFrames);
            else
                srcVoicing.assign(static_cast<size_t>(srcDurationFrames),
                                  kDefaultVoicing);

            if (note.hasBreathCurve())
                srcBreath = CurveResampler::resampleLinear(
                    note.getBreathCurve(), srcDurationFrames);
            else
                srcBreath.assign(static_cast<size_t>(srcDurationFrames),
                                 kDefaultBreath);

            if (note.hasTensionCurve())
                srcTension = CurveResampler::resampleLinear(
                    note.getTensionCurve(), srcDurationFrames);
            else
                srcTension.assign(static_cast<size_t>(srcDurationFrames),
                                  kDefaultTension);

            if (!tensionProc.hasActiveEdits(
                    srcVoicing.data(), srcBreath.data(), srcTension.data(),
                    srcDurationFrames))
                continue;

            srcVoicing.resize(static_cast<size_t>(srcDurationFrames),
                              kDefaultVoicing);
            srcBreath.resize(static_cast<size_t>(srcDurationFrames),
                             kDefaultBreath);
            srcTension.resize(static_cast<size_t>(srcDurationFrames),
                              kDefaultTension);

            const int noiseSamples = static_cast<int>(clipN.size());
            auto processedSrc = tensionProc.processSegment(
                clipH.data(), clipN.data(),
                std::min(srcSamples, noiseSamples),
                srcVoicing.data(), srcBreath.data(), srcTension.data(),
                srcDurationFrames);

            auto srcMel = melComputer.compute(
                processedSrc.data(), static_cast<int>(processedSrc.size()));
            if (srcMel.empty())
                continue;

            if (static_cast<int>(srcMel.size()) != srcDurationFrames)
                srcMel = CurveResampler::resampleLinear2D(srcMel,
                                                          srcDurationFrames);
            srcMel.resize(static_cast<size_t>(srcDurationFrames),
                          std::vector<float>(static_cast<size_t>(numMels),
                                             0.0f));

            const int sourceStart = note.getSrcStartFrame();
            const int sourceEnd = note.getSrcEndFrame();
            const int writeStart = std::max(0, sourceStart);
            const int writeEnd = std::min(sourceEnd,
                                          static_cast<int>(
                                              audioData.sourceMelSpectrogram.size()));
            for (int f = writeStart; f < writeEnd; ++f)
            {
                const int noteLocal = f - sourceStart;
                if (noteLocal >= 0 &&
                    noteLocal < static_cast<int>(srcMel.size()))
                {
                    audioData.sourceMelSpectrogram[static_cast<size_t>(f)] =
                        srcMel[static_cast<size_t>(noteLocal)];
                    updatedSourceMel = true;
                }
            }
        }

        if (!updatedSourceMel)
            return;

        const auto warpMap = buildCurrentOutputWarpMap(project);
        if (warpMap.size() >= 2)
        {
            audioData.melSpectrogram = StretchProcessor::buildOutputMel(
                audioData.sourceMelSpectrogram, warpMap, project.getFrameCount());
        }
        else
        {
            audioData.melSpectrogram = audioData.sourceMelSpectrogram;
        }
    }
} // namespace HNSepCurveProcessor
