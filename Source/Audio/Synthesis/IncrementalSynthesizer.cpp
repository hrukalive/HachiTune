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
  if (cancelFlag)
    cancelFlag->store(true);
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
  if (audioData.melSpectrogram.empty() || project->getEditedData().f0.empty()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (!vocoder->isLoaded()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  if (!project->hasDirtyNotes() && !project->hasF0DirtyRange() &&
      !project->hasParamDirtyRange()) {
    if (onComplete)
      onComplete(false);
    return;
  }

  auto [dirtyStart, dirtyEnd] = project->getDirtyFrameRange();
  if (dirtyStart < 0 || dirtyEnd < 0) {
    if (onComplete)
      onComplete(false);
    return;
  }

  // Compute synthesis range (voiced segments + padding)
  auto [startFrame, endFrame] = computeSynthesisRange(dirtyStart, dirtyEnd);
  startFrame = std::max(0, startFrame);
  endFrame =
      std::min(static_cast<int>(audioData.melSpectrogram.size()), endFrame);

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
    project->clearAllDirty();
    if (onComplete)
      onComplete(true);
    return;
  }

  int startSample = startFrame * hopSize;
  int numSynthSamples = (endFrame - startFrame) * hopSize;
  std::vector<float> originalSegment =
      project->renderMappedBaseWaveformSegment(startSample, numSynthSamples);
  if (static_cast<int>(originalSegment.size()) != numSynthSamples)
    originalSegment.resize(static_cast<size_t>(numSynthSamples), 0.0f);

  HNSepCurveProcessor::rebuildCurvesForRange(*project, startFrame, endFrame);

  // If curve edits need mel update, recompute mel in global melSpectrogram
  const bool hasGlobalHNSep = audioData.harmonicWaveform.getNumSamples() > 0 &&
                               audioData.noiseWaveform.getNumSamples() > 0;
  if (hasGlobalHNSep &&
      !project->getEditedData().voicingCurve.empty() &&
      !project->getEditedData().breathCurve.empty() &&
      !project->getEditedData().tensionCurve.empty() &&
      HNSepCurveProcessor::hasActiveEdits(*project, startFrame, endFrame))
  {
    HNSepCurveProcessor::recomputeMelForRange(*project, startFrame, endFrame);
  }

  // Slice global mel for vocoder input
  std::vector<std::vector<float>> melRange;
  if (startFrame < static_cast<int>(audioData.melSpectrogram.size()) &&
      endFrame <= static_cast<int>(audioData.melSpectrogram.size()))
  {
    melRange.assign(audioData.melSpectrogram.begin() + startFrame,
                    audioData.melSpectrogram.begin() + endFrame);
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
  if (cancelFlag)
    cancelFlag->store(true);
  cancelFlag = std::make_shared<std::atomic<bool>>(false);
  uint64_t currentJobId = ++jobId;
  isBusy = true;

  int capturedStartFrame = startFrame;
  int capturedEndFrame = endFrame;
  auto capturedCancelFlag = cancelFlag;
  auto capturedProject = project;


  vocoder->inferAsync(
      std::move(melRange), std::move(adjustedF0Range),
      [this, capturedCancelFlag, capturedProject, capturedStartFrame,
       capturedEndFrame, hopSize, currentJobId, onComplete,
       blendMask = std::move(blendMask),
       originalSegment = std::move(originalSegment)](
          std::vector<float> synthesizedAudio) {
        if (currentJobId != jobId.load())
          return;
        if (capturedCancelFlag->load()) {
          isBusy = false;
          if (onComplete)
            onComplete(false);
          return;
        }
        if (synthesizedAudio.empty()) {
          isBusy = false;
          if (onComplete)
            onComplete(false);
          return;
        }

        std::thread([this, capturedCancelFlag, capturedProject,
                     capturedStartFrame, capturedEndFrame, hopSize,
                     currentJobId, onComplete, blendMask, originalSegment,
                     synthesizedAudio = std::move(synthesizedAudio)]() mutable {
          if (currentJobId != jobId.load())
            return;
          if (capturedCancelFlag->load()) {
            isBusy = false;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
            return;
          }

          auto &audioData = capturedProject->getAudioData();
          int totalSamples = audioData.waveform.getNumSamples();
          const float *currentWavePtr =
              (audioData.waveform.getNumChannels() > 0 && totalSamples > 0)
                  ? audioData.waveform.getReadPointer(0)
                  : nullptr;
          const auto &origWaveform =
              audioData.originalWaveform.getNumSamples() > 0
                  ? audioData.originalWaveform
                  : audioData.waveform;
          const int origSamples = origWaveform.getNumSamples();
          const float *origWavePtr =
              (origWaveform.getNumChannels() > 0 && origSamples > 0)
                  ? origWaveform.getReadPointer(0)
                  : nullptr;
          int startSample = capturedStartFrame * hopSize;
          int expectedSamples =
              (capturedEndFrame - capturedStartFrame) * hopSize;

          if (expectedSamples <= 0) {
            isBusy = false;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
            return;
          }

          // Resize synthesized audio to match expected
          synthesizedAudio.resize(static_cast<size_t>(expectedSamples), 0.0f);

          int samplesToWrite =
              std::min(expectedSamples, totalSamples - startSample);
          if (samplesToWrite <= 0) {
            isBusy = false;
            if (onComplete)
              juce::MessageManager::callAsync(
                  [onComplete]() { onComplete(false); });
            return;
          }

          // Build blended target from model/original.
          std::vector<float> targetSegment(samplesToWrite, 0.0f);
          for (int i = 0; i < samplesToWrite; ++i) {
            const float b =
                (i < static_cast<int>(blendMask.size())) ? blendMask[i] : 0.0f;
            const float synth = synthesizedAudio[static_cast<size_t>(i)];
            const float orig = originalSegment[static_cast<size_t>(i)];
            targetSegment[static_cast<size_t>(i)] =
                b * synth + (1.0f - b) * orig;
          }

          // Apply per-note gain on top of the blended target.
          std::vector<float> sampleGain(static_cast<size_t>(samplesToWrite),
                                        1.0f);
          for (const auto &note : capturedProject->getNotes()) {
            if (note.isRest())
              continue;
            if (std::abs(note.getVolumeDb()) < 0.001f)
              continue;

            const int noteStart = note.getStartFrame();
            const int noteEnd = note.getEndFrame();
            const int overlapStart = std::max(capturedStartFrame, noteStart);
            const int overlapEnd = std::min(capturedEndFrame, noteEnd);
            if (overlapEnd <= overlapStart)
              continue;

            const int localStart = (overlapStart - capturedStartFrame) * hopSize;
            const int localEnd = (overlapEnd - capturedStartFrame) * hopSize;
            if (localStart >= samplesToWrite)
              continue;

            const float gain =
                juce::Decibels::decibelsToGain(note.getVolumeDb(), -60.0f);
            const int clampedStart = std::max(0, localStart);
            const int clampedEnd = std::min(samplesToWrite, localEnd);
            for (int i = clampedStart; i < clampedEnd; ++i) {
              sampleGain[static_cast<size_t>(i)] *= gain;
            }
          }
          for (int i = 0; i < samplesToWrite; ++i) {
            targetSegment[static_cast<size_t>(i)] *=
                sampleGain[static_cast<size_t>(i)];
          }

          // Distribute synthesized audio into per-note synthWaveforms.
          // Each note gets the slice of targetSegment corresponding to its
          // output frame range [startFrame, endFrame), PLUS margin samples on
          // each side so that composeGlobalWaveform() can crossfade with real
          // audio instead of held-value extrapolation at note boundaries.
          constexpr int kSynthMarginSamples = 256; // margin each side

          for (auto &note : capturedProject->getNotes()) {
            if (note.isRest())
              continue;

            const int noteStart = note.getStartFrame();
            const int noteEnd = note.getEndFrame();
            const int overlapStart = std::max(capturedStartFrame, noteStart);
            const int overlapEnd = std::min(capturedEndFrame, noteEnd);
            if (overlapEnd <= overlapStart)
              continue;

            // Full note range in samples (the "body")
            const int noteStartSample = noteStart * hopSize;
            const int noteEndSample = noteEnd * hopSize;
            const int noteSamples = noteEndSample - noteStartSample;
            if (noteSamples <= 0)
              continue;

            // Compute margin: how far we can extend into targetSegment
            // beyond the note's body on each side.
            const int targetStartSample = capturedStartFrame * hopSize;
            const int targetEndSample = targetStartSample + samplesToWrite;

            // Left margin: extend before noteStartSample
            const int leftMarginAvail = noteStartSample - targetStartSample;
            const int leftMargin = std::max(0, std::min(kSynthMarginSamples, leftMarginAvail));

            // Right margin: extend after noteEndSample
            const int rightMarginAvail = targetEndSample - noteEndSample;
            const int rightMargin = std::max(0, std::min(kSynthMarginSamples, rightMarginAvail));

            // Total synth vector: [preroll | body | postroll]
            const int totalSynthLen = leftMargin + noteSamples + rightMargin;
            std::vector<float> noteSynth(static_cast<size_t>(totalSynthLen), 0.0f);
            std::vector<bool> noteSynthFilled(static_cast<size_t>(totalSynthLen),
                                              false);

            // Copy from targetSegment: the extended region
            // [noteStartSample - leftMargin, noteEndSample + rightMargin) in global coords
            // maps to targetSegment[(noteStartSample - leftMargin - targetStartSample) ..]
            const int extGlobalStart = noteStartSample - leftMargin;
            const int extLocalSrc = extGlobalStart - targetStartSample;
            const int srcStartSample = note.getSrcStartFrame() * hopSize;
            const int srcEndSample = note.getSrcEndFrame() * hopSize;

            // Seed from the currently audible waveform so a first-time partial
            // resynthesis does not leave uncovered regions as zeros.
            if (currentWavePtr != nullptr) {
              const int seededStart = std::max(0, extGlobalStart);
              const int seededEnd =
                  std::min(totalSamples, extGlobalStart + totalSynthLen);
              for (int globalSample = seededStart;
                   globalSample < seededEnd; ++globalSample) {
                const int dstIdx = globalSample - extGlobalStart;
                if (dstIdx >= 0 && dstIdx < totalSynthLen) {
                  noteSynth[static_cast<size_t>(dstIdx)] =
                      currentWavePtr[globalSample];
                  noteSynthFilled[static_cast<size_t>(dstIdx)] = true;
                }
              }
            }

            // Preserve any previously synthesized samples that fall outside the
            // newly rendered overlap. Incremental resynthesis often synthesizes
            // a larger chunk for model context, but that chunk does not always
            // fully cover every overlapping note body/margin.
            if (note.hasSynthWaveform()) {
              const auto &existingSynth = note.getSynthWaveform();
              const int existingGlobalStart =
                  noteStartSample - note.getSynthPreroll();
              const int existingGlobalEnd =
                  existingGlobalStart +
                  static_cast<int>(existingSynth.size());
              const int preservedStart =
                  std::max(extGlobalStart, existingGlobalStart);
              const int preservedEnd = std::min(
                  extGlobalStart + totalSynthLen, existingGlobalEnd);

              for (int globalSample = preservedStart;
                   globalSample < preservedEnd; ++globalSample) {
                const int dstIdx = globalSample - extGlobalStart;
                const int srcIdx = globalSample - existingGlobalStart;
                if (dstIdx >= 0 && dstIdx < totalSynthLen &&
                    srcIdx >= 0 &&
                    srcIdx < static_cast<int>(existingSynth.size())) {
                  noteSynth[static_cast<size_t>(dstIdx)] =
                      existingSynth[static_cast<size_t>(srcIdx)];
                  noteSynthFilled[static_cast<size_t>(dstIdx)] = true;
                }
              }
            }

            // The overlap between [extGlobalStart, noteEndSample+rightMargin) and
            // [capturedStartFrame*hopSize, capturedStartFrame*hopSize + samplesToWrite)
            // determines what we can actually copy from targetSegment.
            const int copyStart = std::max(0, extLocalSrc);
            const int copyEnd = std::min(samplesToWrite,
                extLocalSrc + totalSynthLen);
            const int dstOffset = copyStart - extLocalSrc;

            const int copiedSamples = copyEnd - copyStart;
            const int copyDstStart = dstOffset;
            const int copyDstEnd = copyDstStart + copiedSamples;
            const int regionSpliceSamples = std::max(256, hopSize * 2);
            const int leftSplice = std::max(
                0, std::min({regionSpliceSamples, copiedSamples, copyDstStart}));
            const int rightSplice = std::max(
                0, std::min({regionSpliceSamples, copiedSamples,
                             totalSynthLen - copyDstEnd}));

            for (int i = copyStart; i < copyEnd; ++i) {
              const int localCopyIdx = i - copyStart;
              const int dstIdx = copyDstStart + localCopyIdx;
              if (dstIdx < 0 || dstIdx >= totalSynthLen)
                continue;

              float blend = 1.0f;
              if (leftSplice > 1 && localCopyIdx < leftSplice) {
                const float t = static_cast<float>(localCopyIdx) /
                                static_cast<float>(leftSplice);
                const float ramp = t * t * (3.0f - 2.0f * t);
                blend = std::min(blend, ramp);
              }
              if (rightSplice > 1 &&
                  copiedSamples - 1 - localCopyIdx < rightSplice) {
                const int fromEnd = copiedSamples - 1 - localCopyIdx;
                const float t = static_cast<float>(fromEnd) /
                                static_cast<float>(rightSplice);
                const float ramp = t * t * (3.0f - 2.0f * t);
                blend = std::min(blend, ramp);
              }

              const float existing = noteSynth[static_cast<size_t>(dstIdx)];
              const float target = targetSegment[static_cast<size_t>(i)];
              noteSynth[static_cast<size_t>(dstIdx)] =
                  existing + blend * (target - existing);
              noteSynthFilled[static_cast<size_t>(dstIdx)] = true;
            }

            // Fill any still-uncovered samples from immutable source audio.
            // This covers first-time partial updates where no prior synth cache
            // exists, including short preroll/postroll margins around the note.
            if (origWavePtr != nullptr) {
              const int srcFrames =
                  note.getSrcEndFrame() - note.getSrcStartFrame();
              const int dstFrames = note.getEndFrame() - note.getStartFrame();
              const int srcBodySamples =
                  std::max(0, srcEndSample - srcStartSample);

              for (int dstIdx = 0; dstIdx < totalSynthLen; ++dstIdx) {
                if (noteSynthFilled[static_cast<size_t>(dstIdx)])
                  continue;

                if (dstIdx < leftMargin) {
                  const int srcIdx = srcStartSample - leftMargin + dstIdx;
                  if (srcIdx >= 0 && srcIdx < origSamples) {
                    noteSynth[static_cast<size_t>(dstIdx)] = origWavePtr[srcIdx];
                    noteSynthFilled[static_cast<size_t>(dstIdx)] = true;
                  }
                  continue;
                }

                if (dstIdx >= leftMargin + noteSamples) {
                  const int marginOffset = dstIdx - (leftMargin + noteSamples);
                  const int srcIdx = srcEndSample + marginOffset;
                  if (srcIdx >= 0 && srcIdx < origSamples) {
                    noteSynth[static_cast<size_t>(dstIdx)] = origWavePtr[srcIdx];
                    noteSynthFilled[static_cast<size_t>(dstIdx)] = true;
                  }
                  continue;
                }

                const int bodyIdx = dstIdx - leftMargin;
                int srcIdx = -1;
                if (srcBodySamples > 0) {
                  if (dstFrames > 0 && srcFrames > 0) {
                    const float srcPos =
                        static_cast<float>(bodyIdx) *
                        static_cast<float>(srcBodySamples) /
                        static_cast<float>(noteSamples);
                    srcIdx = srcStartSample + static_cast<int>(srcPos);
                  } else {
                    srcIdx = srcStartSample + bodyIdx;
                  }
                }

                if (srcIdx >= 0 && srcIdx < origSamples) {
                  noteSynth[static_cast<size_t>(dstIdx)] = origWavePtr[srcIdx];
                  noteSynthFilled[static_cast<size_t>(dstIdx)] = true;
                }
              }
            }

            note.setSynthWaveform(std::move(noteSynth), leftMargin);
            note.setSynthPassId(currentJobId);
          }

          // Compose the global waveform from per-note synthWaveforms
          capturedProject->composeGlobalWaveform();

          // Sync composed waveform to auditionBuffer
          auto& audition = capturedProject->getAuditionBuffer();
          const auto& composed = capturedProject->getAudioData().waveform;
          if (composed.getNumSamples() > 0 &&
              audition.getNumSamples() >= composed.getNumSamples())
          {
            // Copy only the synthesized region
            const int startSamp = capturedStartFrame * hopSize;
            const int endSamp = std::min(capturedEndFrame * hopSize,
                                         composed.getNumSamples());
            if (startSamp < endSamp && composed.getNumChannels() > 0 &&
                audition.getNumChannels() > 0)
            {
              const int numToCopy = endSamp - startSamp;
              audition.copyFrom(0, startSamp,
                                composed.getReadPointer(0, startSamp),
                                numToCopy);
            }
          }

          isBusy = false;
          juce::MessageManager::callAsync(
              [capturedProject, onComplete]() {
                capturedProject->clearAllDirty();
                capturedProject->notifyListeners(ProjectChangeType::SynthesisComplete);
                if (onComplete) onComplete(true);
              });
        }).detach();
      });
}
