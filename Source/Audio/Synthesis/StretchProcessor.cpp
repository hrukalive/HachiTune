#include "StretchProcessor.h"
#include <cmath>
#include <algorithm>

float StretchProcessor::mapFrame(const std::vector<Project::WarpMarker>& markers,
                                 float sourceFrame)
{
  if (markers.empty())
    return sourceFrame;
  if (markers.size() == 1)
    return static_cast<float>(markers[0].outputFrame);

  for (size_t i = 1; i < markers.size(); ++i)
  {
    if (sourceFrame <= static_cast<float>(markers[i].sourceFrame))
    {
      float srcStart = static_cast<float>(markers[i - 1].sourceFrame);
      float srcEnd = static_cast<float>(markers[i].sourceFrame);
      float outStart = static_cast<float>(markers[i - 1].outputFrame);
      float outEnd = static_cast<float>(markers[i].outputFrame);
      float segLen = srcEnd - srcStart;
      if (segLen <= 0.0f)
        return outStart;
      float t = (sourceFrame - srcStart) / segLen;
      return outStart + t * (outEnd - outStart);
    }
  }
  return static_cast<float>(markers.back().outputFrame);
}

float StretchProcessor::inverseMapFrame(
    const std::vector<Project::WarpMarker>& markers,
    float outputFrame)
{
  if (markers.empty())
    return outputFrame;
  if (markers.size() == 1)
    return static_cast<float>(markers[0].sourceFrame);

  for (size_t i = 1; i < markers.size(); ++i)
  {
    if (outputFrame <= static_cast<float>(markers[i].outputFrame))
    {
      float outStart = static_cast<float>(markers[i - 1].outputFrame);
      float outEnd = static_cast<float>(markers[i].outputFrame);
      float srcStart = static_cast<float>(markers[i - 1].sourceFrame);
      float srcEnd = static_cast<float>(markers[i].sourceFrame);
      float segLen = outEnd - outStart;
      if (segLen <= 0.0f)
        return srcStart;
      float t = (outputFrame - outStart) / segLen;
      return srcStart + t * (srcEnd - srcStart);
    }
  }
  return static_cast<float>(markers.back().sourceFrame);
}

void StretchProcessor::remapNoteFrames(
    std::vector<Note>& notes,
    const std::vector<Project::WarpMarker>& markers,
    int affectedSourceStart,
    int affectedSourceEnd)
{
  for (auto& note : notes)
  {
    int srcStart = note.getSrcStartFrame();
    int srcEnd = note.getSrcEndFrame();

    if (srcEnd <= affectedSourceStart || srcStart >= affectedSourceEnd)
      continue;

    int newStart = static_cast<int>(std::round(mapFrame(markers, static_cast<float>(srcStart))));
    int newEnd = static_cast<int>(std::round(mapFrame(markers, static_cast<float>(srcEnd))));
    if (newEnd <= newStart)
      newEnd = newStart + 1;

    const int oldStart = note.getStartFrame();
    const int oldEnd = note.getEndFrame();
    note.setStartFrame(newStart);
    note.setEndFrame(newEnd);
    if (oldStart != newStart || oldEnd != newEnd)
    {
      note.markDirty();
      note.markSynthDirty();
    }
  }
}

std::vector<std::vector<float>> StretchProcessor::stretchMel(
    const std::vector<std::vector<float>>& mel,
    const std::vector<Project::WarpMarker>& markers)
{
  if (mel.empty() || markers.size() < 2)
    return mel;

  int newLen = markers.back().outputFrame;
  if (newLen <= 0)
    return {};

  const int numMels = static_cast<int>(mel[0].size());
  std::vector<std::vector<float>> result(static_cast<size_t>(newLen));

  for (int outFrame = 0; outFrame < newLen; ++outFrame)
  {
    float srcF = inverseMapFrame(markers, static_cast<float>(outFrame));
    int srcIdx = static_cast<int>(std::floor(srcF));
    float frac = srcF - static_cast<float>(srcIdx);

    int srcMax = static_cast<int>(mel.size()) - 1;
    srcIdx = std::clamp(srcIdx, 0, srcMax);
    int srcNext = std::min(srcIdx + 1, srcMax);

    result[static_cast<size_t>(outFrame)].resize(static_cast<size_t>(numMels));
    for (int m = 0; m < numMels; ++m)
    {
      result[static_cast<size_t>(outFrame)][static_cast<size_t>(m)] =
          mel[static_cast<size_t>(srcIdx)][static_cast<size_t>(m)] * (1.0f - frac) +
          mel[static_cast<size_t>(srcNext)][static_cast<size_t>(m)] * frac;
    }
  }
  return result;
}

std::vector<std::vector<float>> StretchProcessor::buildOutputMel(
    const std::vector<std::vector<float>>& sourceMel,
    const std::vector<Project::WarpMarker>& warpMap,
    int outputFrameCount)
{
  if (sourceMel.empty() || outputFrameCount <= 0)
    return {};

  auto result = warpMap.size() < 2 ? sourceMel : stretchMel(sourceMel, warpMap);
  const std::vector<float> emptyFrame(
      sourceMel.front().size(), 0.0f);
  result.resize(static_cast<size_t>(outputFrameCount), emptyFrame);
  return result;
}

std::vector<float> StretchProcessor::buildOutputF0(
    const std::vector<float>& sourceF0,
    const std::vector<Project::WarpMarker>& warpMap,
    int outputFrameCount)
{
  if (sourceF0.empty() || outputFrameCount <= 0)
    return {};

  if (warpMap.size() < 2)
  {
    auto result = sourceF0;
    result.resize(static_cast<size_t>(outputFrameCount), 0.0f);
    return result;
  }

  std::vector<float> result(static_cast<size_t>(outputFrameCount), 0.0f);
  const int sourceMax = static_cast<int>(sourceF0.size()) - 1;
  for (int i = 0; i < outputFrameCount; ++i)
  {
    const float sourceFrame =
        inverseMapFrame(warpMap, static_cast<float>(i));
    int sourceIndex = static_cast<int>(std::floor(sourceFrame));
    const float frac = sourceFrame - static_cast<float>(sourceIndex);
    sourceIndex = std::clamp(sourceIndex, 0, std::max(0, sourceMax));
    const int nextIndex = std::min(sourceIndex + 1, std::max(0, sourceMax));

    const float a = sourceF0[static_cast<size_t>(sourceIndex)];
    const float b = sourceF0[static_cast<size_t>(nextIndex)];
    if (a > 0.0f && b > 0.0f)
    {
      result[static_cast<size_t>(i)] =
          std::exp(std::log(a) * (1.0f - frac) + std::log(b) * frac);
    }
    else
    {
      result[static_cast<size_t>(i)] = frac < 0.5f ? a : b;
    }
  }
  return result;
}

void StretchProcessor::stretchEditedData(
    EditedData& edited,
    const std::vector<Project::WarpMarker>& markers,
    int newTotalFrames)
{
  if (markers.size() < 2 || newTotalFrames <= 0)
    return;

  auto resampleLinear = [&](const std::vector<float>& src) {
    std::vector<float> dst(static_cast<size_t>(newTotalFrames));
    for (int i = 0; i < newTotalFrames; ++i)
    {
      float srcF = inverseMapFrame(markers, static_cast<float>(i));
      int srcIdx = static_cast<int>(std::floor(srcF));
      float frac = srcF - static_cast<float>(srcIdx);
      int srcMax = static_cast<int>(src.size()) - 1;
      srcIdx = std::clamp(srcIdx, 0, std::max(0, srcMax));
      int srcNext = std::min(srcIdx + 1, std::max(0, srcMax));
      if (src.empty())
        dst[static_cast<size_t>(i)] = 0.0f;
      else
        dst[static_cast<size_t>(i)] =
            src[static_cast<size_t>(srcIdx)] * (1.0f - frac) +
            src[static_cast<size_t>(srcNext)] * frac;
    }
    return dst;
  };

  auto resampleNearest = [&](const std::vector<float>& src) {
    std::vector<float> dst(static_cast<size_t>(newTotalFrames));
    for (int i = 0; i < newTotalFrames; ++i)
    {
      float srcF = inverseMapFrame(markers, static_cast<float>(i));
      int srcIdx = static_cast<int>(std::round(srcF));
      int srcMax = static_cast<int>(src.size()) - 1;
      srcIdx = std::clamp(srcIdx, 0, std::max(0, srcMax));
      dst[static_cast<size_t>(i)] = src.empty() ? 0.0f : src[static_cast<size_t>(srcIdx)];
    }
    return dst;
  };

  auto resampleNearestBool = [&](const std::vector<bool>& src) {
    std::vector<bool> dst(static_cast<size_t>(newTotalFrames));
    for (int i = 0; i < newTotalFrames; ++i)
    {
      float srcF = inverseMapFrame(markers, static_cast<float>(i));
      int srcIdx = static_cast<int>(std::round(srcF));
      int srcMax = static_cast<int>(src.size()) - 1;
      srcIdx = std::clamp(srcIdx, 0, std::max(0, srcMax));
      dst[static_cast<size_t>(i)] = src.empty() ? false : src[static_cast<size_t>(srcIdx)];
    }
    return dst;
  };

  // basePitch, masks -> nearest neighbor
  edited.basePitch = resampleNearest(edited.basePitch);
  edited.voicedMask = resampleNearestBool(edited.voicedMask);
  edited.vadMask = resampleNearestBool(edited.vadMask);

  // deltaPitch, compatibility curves -> linear interpolation
  edited.deltaPitch = resampleLinear(edited.deltaPitch);
  edited.voicingCurve =
      resampleLinear(!edited.baseVoicing.empty()
                         ? edited.baseVoicing
                         : edited.voicingCurve);
  edited.breathCurve =
      resampleLinear(!edited.baseBreath.empty()
                         ? edited.baseBreath
                         : edited.breathCurve);
  edited.tensionCurve =
      resampleLinear(!edited.baseTension.empty()
                         ? edited.baseTension
                         : edited.tensionCurve);

  const auto& sourceTunedF0 =
      !edited.tunedF0.empty() ? edited.tunedF0 : edited.f0;
  edited.f0 = buildOutputF0(sourceTunedF0, markers, newTotalFrames);
  if (!edited.adjustedMel.empty())
    edited.mel = buildOutputMel(edited.adjustedMel, markers, newTotalFrames);
}
