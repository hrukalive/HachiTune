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
      !project->getEditedData().voicingCurve.empty() &&
      !project->getEditedData().breathCurve.empty() &&
      !project->getEditedData().tensionCurve.empty() &&
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
            capturedProject->blendSynthesizedRangeIntoAuditionBuffer(
                synthesizedAudio, capturedStartFrame, capturedEndFrame, hopSize);
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
