#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"
#include "../Vocoder.h"
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

/**
 * Voiced-Only Blend synthesizer.
 * Resynthesizes dirty regions using vocoder, then blends:
 *   voiced frames  → synthesized audio
 *   unvoiced frames → original audio (preserves breathing)
 */
class IncrementalSynthesizer {
public:
  using ProgressCallback = std::function<void(const juce::String &message)>;
  using CompleteCallback = std::function<void(bool success)>;

  /**
   * Describes a range that needs resynthesis.
   */
  struct ResynthRange
  {
    int startFrame = -1;
    int endFrame = -1;
    bool needsMelUpdate = false;  // true = curve edit needs tension recomputation

    bool isValid() const { return startFrame >= 0 && endFrame > startFrame; }
  };

  /**
   * Compute the range needing resynthesis from dirty notes + dirty ranges.
   * Expands to VAD=0 boundaries for clean splice points.
   */
  ResynthRange computeResynthRange();

  IncrementalSynthesizer();
  ~IncrementalSynthesizer();

  void setVocoder(Vocoder *v) { vocoder = v; }
  void setProject(Project *p) { project = p; }

  void synthesizeRegion(ProgressCallback onProgress,
                        CompleteCallback onComplete);

  void cancel();
  bool isSynthesizing() const { return isBusy.load(); }

private:
  /// Compute synthesis range: find voiced segments overlapping dirty range,
  /// expand to include complete segments + padding frames.
  std::pair<int, int> computeSynthesisRange(int dirtyStart, int dirtyEnd);

  /// Generate per-sample blend mask: 1.0 throughout with hop_size-sample
  /// linear fade-in at start and fade-out at end. Since ResynthRange
  /// extends to VAD=0 boundaries, crossfade occurs at silence.
  std::vector<float> generateBlendMask(int startFrame, int endFrame,
                                       int hopSize,
                                       std::vector<float> *frameMaskOut = nullptr);

  Vocoder *vocoder = nullptr;
  Project *project = nullptr;

  std::shared_ptr<std::atomic<bool>> cancelFlag;
  std::atomic<uint64_t> jobId{0};
  std::atomic<bool> isBusy{false};

  std::thread applyThread;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(IncrementalSynthesizer)
};
