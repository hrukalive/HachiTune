#include "WarpMarkerProcessor.h"
#include "PitchCurveProcessor.h"
#include "../Audio/Synthesis/StretchProcessor.h"
#include "../Utils/Constants.h"
#include <algorithm>
#include <cmath>

namespace
{
    int getWaveformFrameCount(const juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0)
            return 0;
        return numSamples / HOP_SIZE + 1;
    }

    int getProjectSourceFrameLimit(const Project& project)
    {
        const auto& audioData = project.getAudioData();
        int limit = 0;
        if (audioData.originalWaveform.getNumSamples() > 0)
            limit = getWaveformFrameCount(audioData.originalWaveform);
        else if (project.getAnalysisData().getNumFrames() > 0)
            limit = project.getAnalysisData().getNumFrames();
        else if (audioData.waveform.getNumSamples() > 0)
            limit = getWaveformFrameCount(audioData.waveform);
        else
            limit = std::max(0, audioData.getNumFrames());

        for (const auto& note : project.getNotes())
            limit = std::max(limit, note.getSrcEndFrame());
        return limit;
    }

    std::vector<Project::WarpMarker> sortedUniqueMarkers(
        const std::vector<Project::WarpMarker>& markers)
    {
        std::vector<Project::WarpMarker> result = markers;
        std::sort(result.begin(), result.end(),
                  [](const auto& a, const auto& b)
                  {
                      if (a.sourceFrame != b.sourceFrame)
                          return a.sourceFrame < b.sourceFrame;
                      return a.outputFrame < b.outputFrame;
                  });

        result.erase(std::unique(result.begin(), result.end(),
                                 [](const auto& a, const auto& b)
                                 {
                                     return a.sourceFrame == b.sourceFrame;
                                 }),
                     result.end());
        return result;
    }

    bool hasEndpointMarker(const std::vector<Project::WarpMarker>& markers,
                           int sourceEnd)
    {
        return std::any_of(markers.begin(), markers.end(),
                           [sourceEnd](const auto& marker)
                           {
                               return marker.sourceFrame <= 0 ||
                                      marker.sourceFrame >= sourceEnd;
                           });
    }

    int getEndpointOutputFrame(const std::vector<Project::WarpMarker>& markers,
                               int sourceEnd)
    {
        int outputEnd = 0;
        for (const auto& marker : markers)
        {
            if (marker.sourceFrame == sourceEnd && marker.outputFrame > 0)
                outputEnd = std::max(outputEnd, marker.outputFrame);
        }
        return outputEnd;
    }

    int getOutputFrameLimit(const Project& project,
                            const std::vector<Project::WarpMarker>& markers,
                            int sourceEnd)
    {
        const int endpointOutput = getEndpointOutputFrame(markers, sourceEnd);
        if (endpointOutput > 0)
            return endpointOutput;

        const int projectFrames = project.getFrameCount();
        if (projectFrames > 0)
            return projectFrames;

        return sourceEnd;
    }

    std::vector<Project::WarpMarker> normalizeMarkersForBounds(
        const std::vector<Project::WarpMarker>& markers,
        int sourceEnd,
        int outputEnd,
        bool includeEndpoints)
    {
        if (sourceEnd <= 0 || outputEnd <= 0)
            return {};

        std::vector<Project::WarpMarker> result;
        if (includeEndpoints)
            result.push_back({0, 0});

        if (outputEnd > 1)
        {
            const auto sorted = sortedUniqueMarkers(markers);
            int previousSource = 0;
            int previousOutput = 0;
            for (auto marker : sorted)
            {
                if (marker.sourceFrame <= previousSource ||
                    marker.sourceFrame >= sourceEnd)
                {
                    continue;
                }

                marker.outputFrame =
                    std::clamp(marker.outputFrame, 1, outputEnd - 1);
                if (marker.outputFrame <= previousOutput ||
                    marker.outputFrame >= outputEnd)
                {
                    continue;
                }

                result.push_back(marker);
                previousSource = marker.sourceFrame;
                previousOutput = marker.outputFrame;
            }
        }

        if (includeEndpoints)
            result.push_back({sourceEnd, outputEnd});

        return result;
    }

    bool markersEqual(const std::vector<Project::WarpMarker>& a,
                      const std::vector<Project::WarpMarker>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].sourceFrame != b[i].sourceFrame ||
                a[i].outputFrame != b[i].outputFrame)
            {
                return false;
            }
        }
        return true;
    }

    float sampleLinear(const std::vector<float>& values, float frame)
    {
        if (values.empty())
            return 0.0f;

        const int maxIndex = static_cast<int>(values.size()) - 1;
        const float clamped = std::clamp(frame, 0.0f,
                                         static_cast<float>(maxIndex));
        const int index = static_cast<int>(std::floor(clamped));
        const int nextIndex = std::min(index + 1, maxIndex);
        const float frac = clamped - static_cast<float>(index);
        return values[static_cast<size_t>(index)] * (1.0f - frac) +
               values[static_cast<size_t>(nextIndex)] * frac;
    }

    float sampleNearest(const std::vector<float>& values, float frame)
    {
        if (values.empty())
            return 0.0f;

        const int maxIndex = static_cast<int>(values.size()) - 1;
        const int index =
            std::clamp(static_cast<int>(std::round(frame)), 0, maxIndex);
        return values[static_cast<size_t>(index)];
    }

    bool sampleNearest(const std::vector<bool>& values, float frame)
    {
        if (values.empty())
            return false;

        const int maxIndex = static_cast<int>(values.size()) - 1;
        const int index =
            std::clamp(static_cast<int>(std::round(frame)), 0, maxIndex);
        return values[static_cast<size_t>(index)];
    }

    std::vector<float> unwarpLinearToSource(
        const std::vector<float>& values,
        const std::vector<Project::WarpMarker>& currentMap,
        int sourceFrames)
    {
        if (values.empty() || sourceFrames <= 0)
            return {};

        std::vector<float> result(static_cast<size_t>(sourceFrames), 0.0f);
        for (int frame = 0; frame < sourceFrames; ++frame)
        {
            const float outputFrame =
                StretchProcessor::mapFrame(currentMap,
                                           static_cast<float>(frame));
            result[static_cast<size_t>(frame)] =
                sampleLinear(values, outputFrame);
        }
        return result;
    }

    std::vector<float> unwarpNearestToSource(
        const std::vector<float>& values,
        const std::vector<Project::WarpMarker>& currentMap,
        int sourceFrames)
    {
        if (values.empty() || sourceFrames <= 0)
            return {};

        std::vector<float> result(static_cast<size_t>(sourceFrames), 0.0f);
        for (int frame = 0; frame < sourceFrames; ++frame)
        {
            const float outputFrame =
                StretchProcessor::mapFrame(currentMap,
                                           static_cast<float>(frame));
            result[static_cast<size_t>(frame)] =
                sampleNearest(values, outputFrame);
        }
        return result;
    }

    std::vector<bool> unwarpNearestToSource(
        const std::vector<bool>& values,
        const std::vector<Project::WarpMarker>& currentMap,
        int sourceFrames)
    {
        if (values.empty() || sourceFrames <= 0)
            return {};

        std::vector<bool> result(static_cast<size_t>(sourceFrames), false);
        for (int frame = 0; frame < sourceFrames; ++frame)
        {
            const float outputFrame =
                StretchProcessor::mapFrame(currentMap,
                                           static_cast<float>(frame));
            result[static_cast<size_t>(frame)] =
                sampleNearest(values, outputFrame);
        }
        return result;
    }

    EditedData unwarpEditedDataToSource(
        const EditedData& editedData,
        const std::vector<Project::WarpMarker>& currentMap,
        int sourceFrames)
    {
        EditedData sourceData;
        sourceData.basePitch =
            unwarpNearestToSource(editedData.basePitch, currentMap, sourceFrames);
        sourceData.deltaPitch =
            unwarpLinearToSource(editedData.deltaPitch, currentMap, sourceFrames);
        sourceData.f0 =
            unwarpLinearToSource(editedData.f0, currentMap, sourceFrames);
        sourceData.voicedMask =
            unwarpNearestToSource(editedData.voicedMask, currentMap, sourceFrames);
        sourceData.vadMask =
            unwarpNearestToSource(editedData.vadMask, currentMap, sourceFrames);
        sourceData.voicingCurve =
            unwarpLinearToSource(editedData.voicingCurve, currentMap, sourceFrames);
        sourceData.breathCurve =
            unwarpLinearToSource(editedData.breathCurve, currentMap, sourceFrames);
        sourceData.tensionCurve =
            unwarpLinearToSource(editedData.tensionCurve, currentMap, sourceFrames);
        return sourceData;
    }

    std::vector<std::vector<float>> unwarpMelToSource(
        const std::vector<std::vector<float>>& mel,
        const std::vector<Project::WarpMarker>& currentMap,
        int sourceFrames)
    {
        if (mel.empty() || sourceFrames <= 0)
            return {};

        const int numMels = static_cast<int>(mel[0].size());
        std::vector<std::vector<float>> result(static_cast<size_t>(sourceFrames));
        for (int frame = 0; frame < sourceFrames; ++frame)
        {
            const float outputFrame =
                StretchProcessor::mapFrame(currentMap,
                                           static_cast<float>(frame));
            const int maxFrame = static_cast<int>(mel.size()) - 1;
            const float clamped = std::clamp(outputFrame, 0.0f,
                                             static_cast<float>(maxFrame));
            const int index = static_cast<int>(std::floor(clamped));
            const int nextIndex = std::min(index + 1, maxFrame);
            const float frac = clamped - static_cast<float>(index);
            result[static_cast<size_t>(frame)].resize(static_cast<size_t>(numMels));
            for (int melIndex = 0; melIndex < numMels; ++melIndex)
            {
                const float current =
                    melIndex < static_cast<int>(mel[static_cast<size_t>(index)].size())
                        ? mel[static_cast<size_t>(index)][static_cast<size_t>(melIndex)]
                        : 0.0f;
                const float next =
                    melIndex < static_cast<int>(mel[static_cast<size_t>(nextIndex)].size())
                        ? mel[static_cast<size_t>(nextIndex)][static_cast<size_t>(melIndex)]
                        : 0.0f;
                result[static_cast<size_t>(frame)][static_cast<size_t>(melIndex)] =
                    current * (1.0f - frac) + next * frac;
            }
        }
        return result;
    }

    void rebuildVadMaskFromWaveform(Project& project)
    {
        constexpr float kVadThreshold = 0.008f;

        auto& audioData = project.getAudioData();
        auto& editedData = project.getEditedData();
        const int numFrames = audioData.getNumFrames();
        editedData.vadMask.assign(static_cast<size_t>(numFrames), false);
        if (numFrames <= 0 || audioData.waveform.getNumSamples() <= 0)
            return;

        const float* samples = audioData.waveform.getReadPointer(0);
        const int numSamples = audioData.waveform.getNumSamples();
        for (int frame = 0; frame < numFrames; ++frame)
        {
            const int sampleStart = frame * HOP_SIZE;
            const int sampleEnd = std::min(sampleStart + HOP_SIZE, numSamples);
            if (sampleStart >= numSamples || sampleEnd <= sampleStart)
                continue;

            double sumSq = 0.0;
            for (int sample = sampleStart; sample < sampleEnd; ++sample)
            {
                const double value = samples[sample];
                sumSq += value * value;
            }

            const float rms = static_cast<float>(
                std::sqrt(sumSq /
                          static_cast<double>(sampleEnd - sampleStart)));
            editedData.vadMask[static_cast<size_t>(frame)] =
                rms > kVadThreshold;
        }
    }
} // namespace

namespace WarpMarkerProcessor
{
int getSourceFrameLimit(const Project& project)
{
    return getProjectSourceFrameLimit(project);
}

std::vector<Project::WarpMarker> normalizeMarkers(
    const Project& project,
    const std::vector<Project::WarpMarker>& markers)
{
    const int sourceEnd = getSourceFrameLimit(project);
    if (sourceEnd <= 0)
        return {};

    const int outputEnd = getOutputFrameLimit(project, markers, sourceEnd);
    return normalizeMarkersForBounds(markers, sourceEnd, outputEnd,
                                     hasEndpointMarker(markers, sourceEnd));
}

std::vector<Project::WarpMarker> buildWarpMapWithEndpoints(
    const Project& project,
    const std::vector<Project::WarpMarker>& markers)
{
    const int sourceEnd = getSourceFrameLimit(project);
    if (sourceEnd <= 0)
        return {};

    const int outputEnd = getOutputFrameLimit(project, markers, sourceEnd);
    return normalizeMarkersForBounds(markers, sourceEnd, outputEnd, true);
}

void rebuildSourceDerivedOutput(Project& project,
                                const std::vector<Project::WarpMarker>& markers)
{
    const auto warpMap = buildWarpMapWithEndpoints(project, markers);
    if (warpMap.size() < 2)
        return;

    auto& analysisData = project.getAnalysisData();
    auto& editedData = project.getEditedData();
    const int outputFrames = project.getFrameCount() > 0
        ? project.getFrameCount()
        : warpMap.back().outputFrame;
    const auto currentOutputMel = editedData.mel;
    if (editedData.adjustedMel.empty() && !currentOutputMel.empty())
    {
        const auto currentMap =
            buildWarpMapWithEndpoints(project, project.getWarpMarkers());
        const auto& sourceMap = currentMap.size() >= 2 ? currentMap : warpMap;
        editedData.adjustedMel =
            unwarpMelToSource(currentOutputMel, sourceMap,
                              warpMap.back().sourceFrame);
    }

    if (!editedData.adjustedMel.empty())
    {
        editedData.mel = StretchProcessor::buildOutputMel(
            editedData.adjustedMel, warpMap, outputFrames);
    }
    else if (!analysisData.originalMel.empty())
    {
        editedData.adjustedMel = analysisData.originalMel;
        editedData.mel = StretchProcessor::buildOutputMel(
            editedData.adjustedMel, warpMap, outputFrames);
    }

    project.composeGlobalWaveform();
}

bool hasMarkerAtSourceFrame(const std::vector<Project::WarpMarker>& markers,
                            int sourceFrame)
{
    return std::any_of(markers.begin(), markers.end(),
                       [sourceFrame](const auto& marker)
                       {
                           return marker.sourceFrame == sourceFrame;
                       });
}

std::vector<Boundary> collectBoundaries(Project& project)
{
    std::vector<Boundary> boundaries;
    std::vector<Note*> ordered;
    ordered.reserve(project.getNotes().size());
    for (auto& note : project.getNotes())
    {
        if (!note.isRest())
            ordered.push_back(&note);
    }

    if (ordered.empty())
        return boundaries;

    std::sort(ordered.begin(), ordered.end(),
              [](const Note* a, const Note* b)
              {
                  return a->getStartFrame() < b->getStartFrame();
              });

    constexpr int gapThreshold = 3;
    const auto markers = normalizeMarkers(project, project.getWarpMarkers());

    for (size_t i = 0; i < ordered.size(); ++i)
    {
        Note* current = ordered[i];
        Note* prev = (i > 0) ? ordered[i - 1] : nullptr;
        Note* next = (i + 1 < ordered.size()) ? ordered[i + 1] : nullptr;

        bool hasGapBefore = true;
        if (prev)
        {
            const int gap = current->getStartFrame() - prev->getEndFrame();
            hasGapBefore = gap > gapThreshold;
        }

        bool hasGapAfter = true;
        if (next)
        {
            const int gap = next->getStartFrame() - current->getEndFrame();
            hasGapAfter = gap > gapThreshold;
        }

        if (hasGapBefore)
        {
            const int sourceFrame = current->getSrcStartFrame();
            boundaries.push_back({nullptr, current, sourceFrame,
                                  current->getStartFrame(),
                                  hasMarkerAtSourceFrame(markers, sourceFrame)});
        }

        if (hasGapAfter)
        {
            const int sourceFrame = current->getSrcEndFrame();
            boundaries.push_back({current, nullptr, sourceFrame,
                                  current->getEndFrame(),
                                  hasMarkerAtSourceFrame(markers, sourceFrame)});
        }

        if (next && !hasGapAfter)
        {
            const int sourceFrame = current->getSrcEndFrame();
            boundaries.push_back({current, next, sourceFrame,
                                  current->getEndFrame(),
                                  hasMarkerAtSourceFrame(markers, sourceFrame)});
        }
    }

    std::sort(boundaries.begin(), boundaries.end(),
              [](const auto& a, const auto& b)
              {
                  if (a.currentFrame != b.currentFrame)
                      return a.currentFrame < b.currentFrame;
                  return a.sourceFrame < b.sourceFrame;
              });
    return boundaries;
}

int mapSourceFrame(const Project& project,
                   int sourceFrame,
                   const std::vector<Project::WarpMarker>& markers)
{
    const int totalFrames = getSourceFrameLimit(project);
    if (totalFrames <= 0)
        return 0;

    sourceFrame = std::clamp(sourceFrame, 0, totalFrames);
    const auto normalized = buildWarpMapWithEndpoints(project, markers);

    int prevSource = 0;
    int prevOutput = 0;
    for (const auto& marker : normalized)
    {
        if (sourceFrame <= marker.sourceFrame)
        {
            const int srcSpan = std::max(1, marker.sourceFrame - prevSource);
            const double ratio =
                static_cast<double>(sourceFrame - prevSource) /
                static_cast<double>(srcSpan);
            return prevOutput +
                   static_cast<int>(std::lround(
                       ratio *
                       static_cast<double>(marker.outputFrame - prevOutput)));
        }

        prevSource = marker.sourceFrame;
        prevOutput = marker.outputFrame;
    }

    const int srcSpan = std::max(1, totalFrames - prevSource);
    const double ratio =
        static_cast<double>(sourceFrame - prevSource) /
        static_cast<double>(srcSpan);
    return prevOutput + static_cast<int>(std::lround(
                            ratio *
                            static_cast<double>(totalFrames - prevOutput)));
}

int computeSegmentMinimumOutputSpan(const Project& project,
                                    int sourceStartFrame,
                                    int sourceEndFrame,
                                    int minNoteFrames)
{
    if (sourceEndFrame <= sourceStartFrame)
        return 0;

    const int sourceSpan = sourceEndFrame - sourceStartFrame;
    int minimumSpan = 1;

    for (const auto& note : project.getNotes())
    {
        if (note.isRest())
            continue;
        if (note.getSrcStartFrame() < sourceStartFrame ||
            note.getSrcEndFrame() > sourceEndFrame)
        {
            continue;
        }

        const int srcDuration = note.getSrcDurationFrames();
        if (srcDuration <= 0)
            continue;

        const double required =
            static_cast<double>(minNoteFrames) *
            static_cast<double>(sourceSpan) /
            static_cast<double>(srcDuration);
        minimumSpan = std::max(minimumSpan,
                               static_cast<int>(std::ceil(required)));
    }

    return minimumSpan;
}

void recomputeFromMarkers(Project& project,
                          const std::vector<Project::WarpMarker>& currentMarkers,
                          const std::vector<Project::WarpMarker>& markers,
                          bool updateProjectMarkers)
{
    const auto currentMap =
        buildWarpMapWithEndpoints(project, currentMarkers);
    const auto warpMap = buildWarpMapWithEndpoints(project, markers);
    if (warpMap.size() < 2)
        return;

    const bool mapsAlreadyMatch = markersEqual(currentMap, warpMap);

    const int newTotalFrames = warpMap.back().outputFrame;
    auto& editedData = project.getEditedData();
    const auto currentTunedF0 = editedData.tunedF0;
    const auto currentAdjustedMel = editedData.adjustedMel;
    const auto currentOutputMel = editedData.mel;
    if (!mapsAlreadyMatch)
    {
        const int sourceFrames = warpMap.back().sourceFrame;
        auto sourceEditedData =
            unwarpEditedDataToSource(editedData, currentMap, sourceFrames);
        if (!currentTunedF0.empty())
            sourceEditedData.tunedF0 = currentTunedF0;
        if (!currentAdjustedMel.empty())
        {
            sourceEditedData.adjustedMel = currentAdjustedMel;
        }
        else if (!currentOutputMel.empty())
        {
            sourceEditedData.adjustedMel =
                unwarpMelToSource(currentOutputMel, currentMap, sourceFrames);
        }
        StretchProcessor::stretchEditedData(sourceEditedData, warpMap,
                                            newTotalFrames);
        editedData = std::move(sourceEditedData);
    }
    StretchProcessor::remapNoteFrames(project.getNotes(), warpMap);
    project.sortNotes();

    auto& analysisData = project.getAnalysisData();
    if (editedData.adjustedMel.empty() &&
        !mapsAlreadyMatch &&
        !editedData.mel.empty())
    {
        const int sourceFrames = warpMap.back().sourceFrame;
        editedData.adjustedMel =
            unwarpMelToSource(editedData.mel, currentMap, sourceFrames);
    }

    if (editedData.adjustedMel.empty() && !analysisData.originalMel.empty())
        editedData.adjustedMel = analysisData.originalMel;

    if (!editedData.adjustedMel.empty())
    {
        editedData.mel =
            StretchProcessor::buildOutputMel(editedData.adjustedMel,
                                             warpMap, newTotalFrames);
    }

    project.refreshNoteCaches();
    PitchCurveProcessor::refreshNotePitchCachesFromFinalF0(
        project, 0, static_cast<int>(editedData.f0.size()));
    project.composeGlobalWaveform();
    rebuildVadMaskFromWaveform(project);

    if (updateProjectMarkers)
    {
        project.setWarpMarkers(warpMap);
        project.setModified(true);
    }

    project.notifyListeners(ProjectChangeType::WarpChanged);
}

void recomputeFromMarkers(Project& project,
                          const std::vector<Project::WarpMarker>& markers,
                          bool updateProjectMarkers)
{
    recomputeFromMarkers(project, project.getWarpMarkers(), markers,
                         updateProjectMarkers);
}
} // namespace WarpMarkerProcessor
