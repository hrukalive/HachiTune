#pragma once

#include "../JuceHeader.h"
#include <cmath>
#include <vector>

/**
 * Segment-level hnsep timbre processor.
 *
 * The editor stores voicing/breath/tension as frame-domain control curves.
 * This processor applies those curves to harmonic/noise waveform branches:
 *   1. Sample-hold the frame curves across a synthesis segment.
 *   2. Scale harmonic/noise components by voicing/breath.
 *   3. Apply tension as a per-STFT-frame spectral tilt on the harmonic part.
 *
 * The spectral tilt is applied in true decibel space:
 *   ampDb = 20 * log10(amp)
 *   amp   = 10^(ampDb / 20)
 *
 * After filtering, the harmonic branch is RMS-normalized so the timbre shift
 * changes spectral balance without introducing large loudness drift.
 */
class TensionProcessor
{
public:
  TensionProcessor();
  ~TensionProcessor() = default;

  /**
   * Processed harmonic/noise waveform branches.
   */
  struct ProcessedHN
  {
    std::vector<float> harmonic;
    std::vector<float> noise;
  };

  /**
   * Process a synthesis segment using dense frame-domain hnsep curves.
   *
   * @param harmonicData  Pointer to harmonic waveform samples
   * @param noiseData     Pointer to noise waveform samples
   * @param numSamples    Number of samples in each buffer
   * @param voicingCurve  Per-frame voicing values in percent
   * @param breathCurve   Per-frame breath values in percent
   * @param tensionCurve  Per-frame tension values in [-100, 100]
   * @param numFrames     Number of control frames in the segment
   * @return              Processed harmonic and noise branches
   */
  ProcessedHN processSegmentHN(const float* harmonicData,
                               const float* noiseData,
                               int numSamples,
                               const float* voicingCurve,
                               const float* breathCurve,
                               const float* tensionCurve,
                               int numFrames) const;

  /**
   * Compute interleaved real/imag STFT bins for a waveform buffer.
   */
  static std::vector<float> computeSTFT(
      const juce::AudioBuffer<float>& buffer);

  /**
   * True when any frame in the control block departs from the neutral defaults.
   */
  bool hasActiveEdits(const float* voicingCurve,
                      const float* breathCurve,
                      const float* tensionCurve,
                      int numFrames) const;

private:
  static constexpr int kFFTSize = 2048;
  static constexpr int kHopSize = 512;
  static constexpr int kWinSize = 2048;
  static constexpr int kSampleRate = 44100;
  static constexpr int kFFTBin = kFFTSize / 2 + 1;

  juce::dsp::FFT fft;

  std::vector<float> preEmphasisBaseTensionSegment(
      const std::vector<float>& scaledHarmonic,
      const float* tensionCurve,
      int numFrames) const;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TensionProcessor)
};
