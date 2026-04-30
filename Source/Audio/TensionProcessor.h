#pragma once

#include "../JuceHeader.h"
#include <vector>
#include <cmath>

/**
 * Segment-level hnsep timbre processor.
 *
 * The editor stores voicing/breath/tension as frame-domain control curves,
 * but the vocoder ultimately consumes an audio-derived mel spectrogram. This
 * processor is the bridge between the two:
 *   1. Sample-hold the frame curves across a synthesis segment.
 *   2. Scale harmonic/noise components by voicing/breath.
 *   3. Apply tension as a per-STFT-frame spectral tilt on the harmonic part.
 *   4. Recombine the processed harmonic and noise signals.
 *
 * This keeps the waveform edit, mel replacement, and vocoder input aligned to
 * the same segment-level source instead of mixing per-note/per-frame ad hoc in
 * the synthesizer.
 *
 * The spectral tilt is now applied in true decibel space:
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
   * Process a synthesis segment using dense frame-domain hnsep curves.
   *
   * @param harmonicData  Pointer to harmonic waveform samples
   * @param noiseData     Pointer to noise waveform samples
   * @param numSamples    Number of samples in each buffer
   * @param voicingCurve  Per-frame voicing values in percent
   * @param breathCurve   Per-frame breath values in percent
   * @param tensionCurve  Per-frame tension values in [-100, 100]
   * @param numFrames     Number of control frames in the segment
   * @return              Mixed output waveform
   */
  std::vector<float> processSegment(const float *harmonicData,
                                    const float *noiseData,
                                    int numSamples,
                                    const float *voicingCurve,
                                    const float *breathCurve,
                                    const float *tensionCurve,
                                    int numFrames) const;

  /**
   * Result of STFT-based tension processing.
   */
  struct TensionResult
  {
    std::vector<float> mixedWaveform;
    std::vector<std::vector<float>> mel; // [T, NUM_MELS]
  };

  /**
   * Process [startFrame, endFrame) using precomputed STFT cache.
   * Skips forward FFT — reads directly from cached frequency-domain data.
   * Applies voicing/breath scaling and tension spectral tilt, then ISTFT.
   *
   * The STFT cache stores interleaved [real, imag] pairs for each bin,
   * laid out as: harmonicSTFT[stftFrame * kFFTBin * 2 + bin * 2 + 0] = real
   *              harmonicSTFT[stftFrame * kFFTBin * 2 + bin * 2 + 1] = imag
   *
   * @param harmonicSTFT  Global precomputed harmonic STFT (interleaved complex)
   * @param noiseSTFT     Global precomputed noise STFT (interleaved complex)
   * @param totalSTFTFrames  Total number of STFT frames in the cache
   * @param startFrame    Start of processing range (hop-aligned frame index)
   * @param endFrame      End of processing range (exclusive)
   * @param voicingCurve  Global voicing curve (indexed by frame)
   * @param breathCurve   Global breath curve
   * @param tensionCurve  Global tension curve
   * @return              TensionResult with mixed waveform and mel for the range
   */
  TensionResult processSegmentFromSTFT(
      const std::vector<float>& harmonicSTFT,
      const std::vector<float>& noiseSTFT,
      int totalSTFTFrames,
      int startFrame, int endFrame,
      const std::vector<float>& voicingCurve,
      const std::vector<float>& breathCurve,
      const std::vector<float>& tensionCurve) const;

  /**
   * True when any frame in the control block departs from the neutral defaults.
   */
  bool hasActiveEdits(const float *voicingCurve,
                      const float *breathCurve,
                      const float *tensionCurve,
                      int numFrames) const;

private:
  // STFT parameters
  static constexpr int kFFTSize = 2048;
  static constexpr int kHopSize = 512;
  static constexpr int kWinSize = 2048;
  static constexpr int kSampleRate = 44100;
  static constexpr int kFFTBin = kFFTSize / 2 + 1;

  // Pre-computed Hann window (for per-sample access in overlap-add)
  std::vector<float> windowTable;

  // JUCE FFT engine
  juce::dsp::FFT fft;

  std::vector<float> preEmphasisBaseTensionSegment(
      const std::vector<float> &scaledHarmonic,
      const float *tensionCurve,
      int numFrames) const;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TensionProcessor)
};
