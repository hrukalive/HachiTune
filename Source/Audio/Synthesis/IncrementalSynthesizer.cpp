#include "IncrementalSynthesizer.h"
#include "../TensionProcessor.h"
#include "../../Utils/CurveResampler.h"
#include "../../Utils/HNSepCurveProcessor.h"
#include "../../Utils/Localization.h"
#include "../../Utils/MelSpectrogram.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

IncrementalSynthesizer::IncrementalSynthesizer() = default;

// ---------------------------------------------------------------------------
// computeResynthRange: VAD-boundary-aware dirty range expansion.
// ---------------------------------------------------------------------------
IncrementalSynthesizer::ResynthRange
IncrementalSynthesizer::computeResynthRange()
{
  if (!project)
    return {};

  ResynthRange range;

  // 1. Collect dirty notes' frame ranges
  int dirtyStart = INT_MAX;
  int dirtyEnd = INT_MIN;
  for (const auto& note : project->getNotes())
  {
    if (note.isDirty() || note.isSynthDirty())
    {
      dirtyStart = std::min(dirtyStart, note.getStartFrame());
      dirtyEnd = std::max(dirtyEnd, note.getEndFrame());
    }
  }

  // 2. Merge f0 dirty range
  if (project->hasF0DirtyRange())
  {
    auto [f0s, f0e] = project->getF0DirtyRange();
    dirtyStart = std::min(dirtyStart, f0s);
    dirtyEnd = std::max(dirtyEnd, f0e);
  }

  // 3. Merge param dirty range (curve edits need mel update)
  if (project->hasParamDirtyRange())
  {
    auto [ps, pe] = project->getParamDirtyRange();
    dirtyStart = std::min(dirtyStart, ps);
    dirtyEnd = std::max(dirtyEnd, pe);
    range.needsMelUpdate = true;
  }

  if (dirtyStart >= dirtyEnd)
    return {};

  // 4-5. Expand to VAD=0 boundaries
  const auto& vadMask = project->getEditedData().vadMask;

  // If editedData.vadMask is empty, fall back to empty
  const auto& effectiveVadMask = vadMask;
  const int effectiveTotal = static_cast<int>(effectiveVadMask.size());

  if (effectiveTotal == 0)
  {
    range.startFrame = std::max(0, dirtyStart);
    range.endFrame = dirtyEnd;
    return range;
  }

  int start = dirtyStart;
  while (start > 0 && start < effectiveTotal &&
         static_cast<bool>(effectiveVadMask[static_cast<size_t>(start)]))
    --start;

  int end = dirtyEnd;
  while (end < effectiveTotal &&
         static_cast<bool>(effectiveVadMask[static_cast<size_t>(end)]))
    ++end;

  range.startFrame = std::max(0, start);
  range.endFrame = std::min(effectiveTotal, end);
  return range;
}

IncrementalSynthesizer::~IncrementalSynthesizer() { cancel(); }

void IncrementalSynthesizer::cancel() {
  std::lock_guard<std::mutex> lock(jobStateMutex);
  if (cancelFlag)
    cancelFlag->store(true);
}

bool IncrementalSynthesizer::isCurrentJob(
    uint64_t currentJobId,
    const std::shared_ptr<std::atomic<bool>>& jobCancelFlag) const {
  if (currentJobId != jobId.load())
    return false;
  if (activeJobId.load() != currentJobId)
    return false;
  return !jobCancelFlag || !jobCancelFlag->load();
}

bool IncrementalSynthesizer::finishJobIfCurrent(uint64_t currentJobId) {
  uint64_t expected = currentJobId;
  if (!activeJobId.compare_exchange_strong(expected, 0))
    return false;
  isBusy = false;
  return true;
}

namespace {
void expandDirtyRangeToWholeNotes(const Project *project, int &startFrame,
                                  int &endFrame) {
  if (!project || startFrame >= endFrame)
    return;

  bool expanded = false;
  do {
    expanded = false;
    for (const auto &note : project->getNotes()) {
      if (note.isRest())
        continue;
      if (note.getEndFrame() <= startFrame || note.getStartFrame() >= endFrame)
        continue;

      const int expandedStart = std::min(startFrame, note.getStartFrame());
      const int expandedEnd = std::max(endFrame, note.getEndFrame());
      if (expandedStart != startFrame || expandedEnd != endFrame) {
        startFrame = expandedStart;
        endFrame = expandedEnd;
        expanded = true;
      }
    }
  } while (expanded);
}
} // namespace

// ---------------------------------------------------------------------------
// computeSynthesisRange: find voiced segments overlapping dirty range,
// expand to include complete segments + padding.
// ---------------------------------------------------------------------------
std::pair<int, int>
IncrementalSynthesizer::computeSynthesisRange(int dirtyStart, int dirtyEnd) {
  if (!project)
    return {dirtyStart, dirtyEnd};

  auto &voicedMask = project->getEditedData().voicedMask;
  auto &vadMask = project->getEditedData().vadMask;
  const int totalFrames = static_cast<int>(voicedMask.size());
  const int totalVadFrames = static_cast<int>(vadMask.size());
  if (totalFrames == 0)
    return {dirtyStart, dirtyEnd};

  dirtyStart = std::max(0, dirtyStart);
  dirtyEnd = std::min(totalFrames, dirtyEnd);
  expandDirtyRangeToWholeNotes(project, dirtyStart, dirtyEnd);
  dirtyStart = std::max(0, dirtyStart);
  dirtyEnd = std::min(totalFrames, dirtyEnd);

  // Give the vocoder enough temporal context to stabilize local phase when
  // doing chunked re-synthesis.
  constexpr int kPadFrames = 24;
  // Bridge short UV gaps so adjacent notes around consonants are synthesized
  // together; this avoids junction phase resets between neighboring notes.
  constexpr int kGapBridgeFrames = 16;

  auto isSynthesisActive = [&](int idx) -> bool {
    const bool voiced =
        idx >= 0 && idx < totalFrames && static_cast<bool>(voicedMask[idx]);
    const bool vad =
        idx >= 0 && idx < totalVadFrames && static_cast<bool>(vadMask[idx]);
    return voiced || vad;
  };

  // Expand backward to include neighboring voiced / energetic UV segments
  // across short gaps. VAD-positive consonant heads and tails need to remain
  // inside the resynthesis context even when the vocoder later routes them
  // back to original audio via the blend mask.
  int start = dirtyStart;
  int backGap = 0;
  while (start > 0) {
    if (isSynthesisActive(start - 1)) {
      --start;
      backGap = 0;
      continue;
    }
    if (backGap < kGapBridgeFrames) {
      --start;
      ++backGap;
      continue;
    }
    break;
  }
  start = std::max(0, start - kPadFrames);

  // Expand forward to include neighboring voiced / energetic UV segments
  // across short gaps.
  int end = dirtyEnd;
  int fwdGap = 0;
  while (end < totalFrames) {
    if (isSynthesisActive(end)) {
      ++end;
      fwdGap = 0;
      continue;
    }
    if (fwdGap < kGapBridgeFrames) {
      ++end;
      ++fwdGap;
      continue;
    }
    break;
  }
  end = std::min(totalFrames, end + kPadFrames);

  return {start, end};
}

// ---------------------------------------------------------------------------
// generateBlendMask: per-sample blend factor from voicedMask.
// 1.0 = synthesized, 0.0 = original, smooth ramps at transitions.
// ---------------------------------------------------------------------------
std::vector<float>
IncrementalSynthesizer::generateBlendMask(int startFrame, int endFrame,
                                          int hopSize,
                                          std::vector<float> *frameMaskOut) {
  const int numFrames = endFrame - startFrame;
  const int numSamples = numFrames * hopSize;

  if (frameMaskOut != nullptr)
  {
    *frameMaskOut = std::vector<float>(static_cast<size_t>(numFrames), 1.0f);
  }

  std::vector<float> mask(static_cast<size_t>(numSamples), 1.0f);

  const int fadeLen = std::min(hopSize, numSamples);
  for (int s = 0; s < fadeLen; ++s)
    mask[static_cast<size_t>(s)] =
        static_cast<float>(s) / static_cast<float>(fadeLen);

  for (int s = 0; s < fadeLen; ++s)
    mask[static_cast<size_t>(numSamples - 1 - s)] =
        static_cast<float>(s) / static_cast<float>(fadeLen);

  return mask;
}

void IncrementalSynthesizer::blendSynthesizedRangeIntoFinalWaveform(
    Project& project,
    const std::vector<float>& synthesized,
    int startFrame,
    int endFrame,
    int hopSize)
{
  if (synthesized.empty() || hopSize <= 0)
    return;

  const int outputFrames = project.getFrameCount();
  const int requiredFrames = outputFrames > 0
                                 ? std::max(outputFrames, endFrame)
                                 : endFrame;
  const int requiredSamples = std::max(0, requiredFrames * hopSize);
  if (requiredSamples <= 0)
    return;

  auto& audioData = project.getAudioData();
  auto& finalWaveform = audioData.finalWaveform;
  const auto& sourceWaveform = audioData.waveform;
  const auto& originalWaveform = audioData.originalWaveform;
  const bool hasSourceBaseline =
      (sourceWaveform.getNumChannels() > 0 &&
       sourceWaveform.getNumSamples() > 0) ||
      (originalWaveform.getNumChannels() > 0 &&
       originalWaveform.getNumSamples() > 0);
  const auto* baselineWaveform =
      (originalWaveform.getNumChannels() > 0 &&
       originalWaveform.getNumSamples() > 0)
          ? &originalWaveform
          : &sourceWaveform;
  const bool needsFullBaseline =
      finalWaveform.getNumChannels() == 0 ||
      finalWaveform.getNumSamples() == 0;

  int numChannels = finalWaveform.getNumChannels();
  if (numChannels == 0)
  {
    if (sourceWaveform.getNumChannels() > 0 &&
        sourceWaveform.getNumSamples() > 0)
      numChannels = sourceWaveform.getNumChannels();
    else if (audioData.originalWaveform.getNumChannels() > 0 &&
             audioData.originalWaveform.getNumSamples() > 0)
      numChannels = audioData.originalWaveform.getNumChannels();
    else
      numChannels = 1;
  }

  if (numChannels == 0)
    return;

  if (finalWaveform.getNumChannels() != numChannels ||
      finalWaveform.getNumSamples() != requiredSamples)
    finalWaveform.setSize(numChannels, requiredSamples, true, true, false);

  auto refreshBaseline = [&](int sampleStart, int sampleCount)
  {
    if (!hasSourceBaseline || sampleCount <= 0)
      return;

    const int sourceChannels = baselineWaveform->getNumChannels();
    if (sourceChannels <= 0)
      return;

    for (int ch = 0; ch < numChannels; ++ch)
    {
      const int sourceChannel = std::min(ch, sourceChannels - 1);
      auto baseline = project.renderMappedSourceSegment(
          baselineWaveform->getReadPointer(sourceChannel),
          baselineWaveform->getNumSamples(), sampleStart, sampleCount);
      if (baseline.empty())
        continue;

      const int samplesToCopy =
          std::min(sampleCount, static_cast<int>(baseline.size()));
      float* dst = finalWaveform.getWritePointer(ch, sampleStart);
      std::copy_n(baseline.data(), samplesToCopy, dst);
    }
  };

  if (needsFullBaseline)
    refreshBaseline(0, requiredSamples);

  const int rawStartSample = startFrame * hopSize;
  const int rawEndSample = endFrame * hopSize;
  const int rangeSamples = rawEndSample - rawStartSample;
  if (rangeSamples <= 0)
    return;

  const int sourceSamples =
      std::min(rangeSamples, static_cast<int>(synthesized.size()));
  const int sourceOffset = std::max(0, -rawStartSample);
  if (sourceOffset >= sourceSamples)
    return;

  const int startSample = std::max(0, rawStartSample);
  const int endSample = std::min(finalWaveform.getNumSamples(), rawEndSample);
  if (endSample <= startSample)
    return;

  const int numSamples = std::min(
      endSample - startSample, sourceSamples - sourceOffset);
  const int fade = std::min(hopSize, sourceSamples / 2);

  refreshBaseline(startSample, numSamples);

  for (int ch = 0; ch < numChannels; ++ch)
  {
    float* dst = finalWaveform.getWritePointer(ch, startSample);
    for (int i = 0; i < numSamples; ++i)
    {
      const int sourceIndex = sourceOffset + i;
      float mix = 1.0f;
      if (fade > 0 && sourceIndex < fade)
        mix = static_cast<float>(sourceIndex) / static_cast<float>(fade);
      if (fade > 0 && sourceSamples - 1 - sourceIndex < fade)
        mix = std::min(mix,
                       static_cast<float>(sourceSamples - 1 - sourceIndex) /
                           static_cast<float>(fade));
      const float synth = synthesized[static_cast<size_t>(sourceIndex)];
      dst[i] = dst[i] + mix * (synth - dst[i]);
    }
  }
}

// ---------------------------------------------------------------------------
// synthesizeRegion: Voiced-Only Blend approach.
// ---------------------------------------------------------------------------
void IncrementalSynthesizer::synthesizeRegion(ProgressCallback onProgress,
                                              CompleteCallback onComplete) {
  if (!project || !vocoder) {
    if (onComplete)
      onComplete(false);
    return;
  }

  auto &audioData = project->getAudioData();
  auto& editedData = project->getEditedData();
  if (editedData.mel.empty() || editedData.f0.empty()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (!vocoder->isLoaded()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  auto [dirtyStart, dirtyEnd] = project->getDirtyFrameRange();

  const auto range = computeResynthRange();
  if (!range.isValid()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  int startFrame = range.startFrame;
  int endFrame = range.endFrame;
  startFrame = std::max(0, startFrame);
  endFrame = std::min(static_cast<int>(editedData.mel.size()), endFrame);
  if (dirtyStart < 0 || dirtyEnd < 0) {
    dirtyStart = startFrame;
    dirtyEnd = endFrame;
  }

  auto &debugInfo = audioData.incrementalDebug;
  debugInfo.clear();
  debugInfo.dirtyStartFrame = dirtyStart;
  debugInfo.dirtyEndFrame = dirtyEnd;

  const auto [f0DirtyStart, f0DirtyEnd] = project->getF0DirtyRange();
  debugInfo.f0DirtyStartFrame = f0DirtyStart;
  debugInfo.f0DirtyEndFrame = f0DirtyEnd;

  const auto [paramDirtyStart, paramDirtyEnd] = project->getParamDirtyRange();
  debugInfo.paramDirtyStartFrame = paramDirtyStart;
  debugInfo.paramDirtyEndFrame = paramDirtyEnd;

  for (const auto &note : project->getNotes()) {
    if (note.isDirty())
      debugInfo.dirtyNoteRanges.emplace_back(note.getStartFrame(),
                                             note.getEndFrame());
  }

  if (startFrame >= endFrame) {
    if (onComplete)
      onComplete(false);
    return;
  }

  debugInfo.synthesisStartFrame = startFrame;
  debugInfo.synthesisEndFrame = endFrame;
  const auto dirtySnapshot =
      project->captureDirtyStateSnapshotForRange(startFrame, endFrame);

  // Generate blend mask before async call (voicedMask is stable here)
  int hopSize = vocoder->getHopSize();
  std::vector<float> blendFrameMask;
  std::vector<float> blendMask =
      generateBlendMask(startFrame, endFrame, hopSize, &blendFrameMask);
  debugInfo.blendMaskFrames = std::move(blendFrameMask);

  // Early exit: if blend mask is all-zero, nothing to synthesize
  bool hasVoiced = std::any_of(blendMask.begin(), blendMask.end(),
                               [](float v) { return v > 0.0f; });
  if (!hasVoiced) {
    project->clearDirtyStateForCompletedSynthesis(dirtySnapshot);
    if (onComplete)
      onComplete(true);
    return;
  }

  HNSepCurveProcessor::rebuildCurvesForRange(*project, startFrame, endFrame);

  // If curve edits need mel update, recompute final output-timeline mel.
  const bool hasGlobalHNSep = audioData.harmonicWaveform.getNumSamples() > 0 &&
                               audioData.noiseWaveform.getNumSamples() > 0;
  if (range.needsMelUpdate &&
      hasGlobalHNSep &&
      !editedData.baseVoicing.empty() &&
      !editedData.baseBreath.empty() &&
      !editedData.baseTension.empty() &&
      HNSepCurveProcessor::hasActiveEdits(*project, startFrame, endFrame))
  {
    HNSepCurveProcessor::recomputeMelForRange(*project, startFrame, endFrame);
  }

  // Slice global mel for vocoder input
  std::vector<std::vector<float>> melRange;
  if (startFrame < static_cast<int>(editedData.mel.size()) &&
      endFrame <= static_cast<int>(editedData.mel.size()))
  {
    melRange.assign(editedData.mel.begin() + startFrame,
                    editedData.mel.begin() + endFrame);
  }

  std::vector<float> adjustedF0Range =
      project->getAdjustedF0ForRange(startFrame, endFrame);

  if (melRange.empty() || adjustedF0Range.empty()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (onProgress)
    onProgress(TR("progress.synthesizing"));

  // Cancel previous job
  uint64_t currentJobId = 0;
  std::shared_ptr<std::atomic<bool>> capturedCancelFlag;
  {
    std::lock_guard<std::mutex> lock(jobStateMutex);
    if (cancelFlag)
      cancelFlag->store(true);
    cancelFlag = std::make_shared<std::atomic<bool>>(false);
    currentJobId = ++jobId;
    activeJobId.store(currentJobId);
    isBusy = true;
    capturedCancelFlag = cancelFlag;
  }

  int capturedStartFrame = startFrame;
  int capturedEndFrame = endFrame;
  auto capturedProject = project;


  vocoder->inferAsync(
      std::move(melRange), std::move(adjustedF0Range),
      [this, capturedCancelFlag, capturedProject, capturedStartFrame,
       capturedEndFrame, hopSize, currentJobId, onComplete,
       dirtySnapshot](
          std::vector<float> synthesizedAudio) {
        auto failCurrentJob = [this, currentJobId, onComplete]() {
          if (!finishJobIfCurrent(currentJobId))
            return;
          if (onComplete)
            onComplete(false);
        };

        if (!isCurrentJob(currentJobId, capturedCancelFlag)) {
          if (currentJobId == jobId.load() && capturedCancelFlag->load())
            failCurrentJob();
          return;
        }
        if (synthesizedAudio.empty()) {
          failCurrentJob();
          return;
        }

        std::thread([this, capturedCancelFlag, capturedProject,
                     capturedStartFrame, capturedEndFrame, hopSize,
                     currentJobId, onComplete, dirtySnapshot,
                     synthesizedAudio = std::move(synthesizedAudio)]() mutable {
          auto failCurrentJobAsync = [this, currentJobId, onComplete]() {
            if (!finishJobIfCurrent(currentJobId))
              return;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
          };

          auto shouldAbort = [&]() {
            if (isCurrentJob(currentJobId, capturedCancelFlag))
              return false;
            if (currentJobId == jobId.load() && capturedCancelFlag->load())
              failCurrentJobAsync();
            return true;
          };

          if (shouldAbort())
            return;

          int expectedSamples =
              (capturedEndFrame - capturedStartFrame) * hopSize;
          if (expectedSamples <= 0) {
            failCurrentJobAsync();
            return;
          }

          synthesizedAudio.resize(static_cast<size_t>(expectedSamples), 0.0f);
          capturedProject->applyNoteVolumeToSynthesizedRange(
              synthesizedAudio, capturedStartFrame, capturedEndFrame, hopSize);
          if (shouldAbort())
            return;

          {
            std::lock_guard<std::mutex> lock(jobStateMutex);
            if (!isCurrentJob(currentJobId, capturedCancelFlag))
              return;
            IncrementalSynthesizer::blendSynthesizedRangeIntoFinalWaveform(
                *capturedProject, synthesizedAudio, capturedStartFrame,
                capturedEndFrame, hopSize);
            if (!finishJobIfCurrent(currentJobId))
              return;
          }
          juce::MessageManager::callAsync(
              [capturedProject, dirtySnapshot, onComplete]() {
                capturedProject->clearDirtyStateForCompletedSynthesis(
                    dirtySnapshot);
                capturedProject->notifyListeners(ProjectChangeType::SynthesisComplete);
                if (onComplete) onComplete(true);
              });
        }).detach();
      });
}
