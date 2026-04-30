#include "WarpMarkerProcessor.h"
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
    const int totalFrames = getSourceFrameLimit(project);
    if (totalFrames <= 1)
        return {};

    auto result = sortedUniqueMarkers(markers);
    result.erase(std::remove_if(result.begin(), result.end(),
                                [totalFrames](const auto& marker)
                                {
                                    return marker.sourceFrame <= 0 ||
                                           marker.sourceFrame >= totalFrames;
                                }),
                 result.end());

    for (auto& marker : result)
        marker.outputFrame = std::clamp(marker.outputFrame, 1, totalFrames - 1);

    int prevOut = 0;
    for (auto& marker : result)
    {
        marker.outputFrame = std::max(marker.outputFrame, prevOut + 1);
        prevOut = marker.outputFrame;
    }

    int nextOut = totalFrames;
    for (int i = static_cast<int>(result.size()) - 1; i >= 0; --i)
    {
        result[static_cast<size_t>(i)].outputFrame =
            std::min(result[static_cast<size_t>(i)].outputFrame, nextOut - 1);
        nextOut = result[static_cast<size_t>(i)].outputFrame;
    }

    return result;
}

std::vector<Project::WarpMarker> buildWarpMapWithEndpoints(
    const Project& project,
    const std::vector<Project::WarpMarker>& markers)
{
    const int sourceEnd = getSourceFrameLimit(project);
    if (sourceEnd <= 0)
        return {};

    int outputEnd = project.getFrameCount();
    if (outputEnd <= 0)
        outputEnd = sourceEnd;

    std::vector<Project::WarpMarker> result;
    result.push_back({0, 0});

    if (outputEnd <= 0)
        return {};

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

            marker.outputFrame = std::clamp(marker.outputFrame, 1, outputEnd - 1);
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

    result.push_back({sourceEnd, outputEnd});
    return result;
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
    const auto normalized = normalizeMarkers(project, markers);

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
                          const std::vector<Project::WarpMarker>& markers,
                          bool updateProjectMarkers)
{
    const auto warpMap = buildWarpMapWithEndpoints(project, markers);
    if (warpMap.size() < 2)
        return;

    if (updateProjectMarkers)
        project.setWarpMarkers(warpMap);

    const int newTotalFrames = warpMap.back().outputFrame;
    auto& editedData = project.getEditedData();
    StretchProcessor::stretchEditedData(editedData, warpMap, newTotalFrames);
    StretchProcessor::remapNoteFrames(project.getNotes(), warpMap);

    auto& audioData = project.getAudioData();
    if (!audioData.melSpectrogram.empty())
    {
        audioData.melSpectrogram =
            StretchProcessor::stretchMel(audioData.melSpectrogram, warpMap);
    }

    project.refreshNoteCaches();
    project.composeGlobalWaveform();
    rebuildVadMaskFromWaveform(project);

    if (updateProjectMarkers)
        project.setModified(true);

    project.notifyListeners(ProjectChangeType::WarpChanged);
}
} // namespace WarpMarkerProcessor
