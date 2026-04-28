#include "TensionProcessor.h"
#include "../Utils/MelSpectrogram.h"

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TensionProcessor::TensionProcessor()
{
  hannWindow.resize(static_cast<size_t>(kWinSize));
  const double twoPi = 2.0 * 3.14159265358979323846;
  for (int n = 0; n < kWinSize; ++n)
  {
    hannWindow[static_cast<size_t>(n)] =
        static_cast<float>(0.5 * (1.0 - std::cos(twoPi * n / kWinSize)));
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool TensionProcessor::hasActiveEdits(const float *voicingCurve,
                                      const float *breathCurve,
                                      const float *tensionCurve,
                                      int numFrames) const
{
  for (int i = 0; i < numFrames; ++i)
  {
    const float voicing = voicingCurve ? voicingCurve[i] : 100.0f;
    const float breath = breathCurve ? breathCurve[i] : 100.0f;
    const float tension = tensionCurve ? tensionCurve[i] : 0.0f;
    if (std::abs(voicing - 100.0f) > 0.001f ||
        std::abs(breath - 100.0f) > 0.001f ||
        std::abs(tension) > 0.001f)
      return true;
  }

  return false;
}

std::vector<float> TensionProcessor::processSegment(const float *harmonicData,
                                                    const float *noiseData,
                                                    int numSamples,
                                                    const float *voicingCurve,
                                                    const float *breathCurve,
                                                    const float *tensionCurve,
                                                    int numFrames) const
{
  if (numSamples <= 0 || numFrames <= 0)
    return {};

  std::vector<float> scaledHarmonic(static_cast<size_t>(numSamples), 0.0f);
  std::vector<float> scaledNoise(static_cast<size_t>(numSamples), 0.0f);

  bool hasAnyTension = false;
  for (int i = 0; i < numSamples; ++i)
  {
    const int frame = std::clamp(i / kHopSize, 0, numFrames - 1);
    const float voicingPct = voicingCurve ? voicingCurve[frame] : 100.0f;
    const float breathPct = breathCurve ? breathCurve[frame] : 100.0f;
    const float tension = tensionCurve ? tensionCurve[frame] : 0.0f;

    scaledHarmonic[static_cast<size_t>(i)] =
        harmonicData[i] * (voicingPct / 100.0f);
    scaledNoise[static_cast<size_t>(i)] =
        noiseData[i] * (breathPct / 100.0f);
    hasAnyTension = hasAnyTension || std::abs(tension) > 0.001f;
  }

  std::vector<float> processedHarmonic =
      hasAnyTension ? preEmphasisBaseTensionSegment(scaledHarmonic,
                                                    tensionCurve,
                                                    numFrames)
                    : scaledHarmonic;

  std::vector<float> result(static_cast<size_t>(numSamples), 0.0f);
  for (int i = 0; i < numSamples; ++i)
  {
    result[static_cast<size_t>(i)] =
        scaledNoise[static_cast<size_t>(i)] +
        processedHarmonic[static_cast<size_t>(i)];
  }

  return result;
}

// ---------------------------------------------------------------------------
// Segment STFT processing
// ---------------------------------------------------------------------------

std::vector<float> TensionProcessor::preEmphasisBaseTensionSegment(
    const std::vector<float> &scaledHarmonic,
    const float *tensionCurve,
    int numFrames) const
{
  const int numSamples = static_cast<int>(scaledHarmonic.size());
  if (numSamples <= 0)
    return {};

  float originalMax = 0.0f;
  double originalEnergySum = 0.0;
  for (float sample : scaledHarmonic)
  {
    originalMax = std::max(originalMax, std::abs(sample));
    originalEnergySum += static_cast<double>(sample) * static_cast<double>(sample);
  }
  const float originalRms = static_cast<float>(
      std::sqrt(originalEnergySum / std::max(1, numSamples)));

  if (originalMax < 1e-10f)
    return scaledHarmonic;

  const float nyquist = static_cast<float>(kSampleRate) / 2.0f;
  const float x0 = static_cast<float>(kFFTBin) / (nyquist / 1500.0f);

  const int stftFrames = (numSamples + kHopSize - 1) / kHopSize;
  const int paddedLen = stftFrames * kHopSize + kWinSize;
  const int offset = kWinSize / 2;

  std::vector<float> padded(static_cast<size_t>(paddedLen), 0.0f);
  for (int i = 0; i < numSamples; ++i)
    padded[static_cast<size_t>(offset + i)] = scaledHarmonic[static_cast<size_t>(i)];

  std::vector<float> output(static_cast<size_t>(paddedLen), 0.0f);
  std::vector<float> windowSum(static_cast<size_t>(paddedLen), 0.0f);
  std::vector<float> frame(static_cast<size_t>(kFFTSize), 0.0f);
  std::vector<float> fftReal(static_cast<size_t>(kFFTBin), 0.0f);
  std::vector<float> fftImag(static_cast<size_t>(kFFTBin), 0.0f);
  std::vector<float> outFrame(static_cast<size_t>(kFFTSize), 0.0f);

  float maxAmpCorrection = 1.0f;
  float maxTiltDb = 12.0f;

  for (int f = 0; f < stftFrames; ++f)
  {
    const int frameStart = f * kHopSize;
    const int curveFrame = std::clamp(f, 0, numFrames - 1);
    const float userTension = tensionCurve ? tensionCurve[curveFrame] : 0.0f;
    const float clampedTension = juce::jlimit(-100.0f, 100.0f, userTension);
    const float b = -clampedTension / 100.0f * maxTiltDb;

    maxAmpCorrection = std::max(
        maxAmpCorrection,
        juce::jlimit(0.0f, 0.33f, b / -15.0f) + 1.0f);

    for (int n = 0; n < kFFTSize; ++n)
    {
      const int idx = frameStart + n;
      frame[static_cast<size_t>(n)] =
          idx < paddedLen ? padded[static_cast<size_t>(idx)] *
                                hannWindow[static_cast<size_t>(n)]
                          : 0.0f;
    }

    forwardFFT(frame.data(), fftReal.data(), fftImag.data());

    for (int k = 0; k < kFFTBin; ++k)
    {
      float filterDb = (-b / x0) * static_cast<float>(k) + b;
      filterDb = juce::jlimit(-maxTiltDb, maxTiltDb, filterDb);
      const float filterGain = std::pow(10.0f, filterDb / 20.0f);
      fftReal[static_cast<size_t>(k)] *= filterGain;
      fftImag[static_cast<size_t>(k)] *= filterGain;
    }

    inverseFFT(fftReal.data(), fftImag.data(), outFrame.data());

    for (int n = 0; n < kFFTSize; ++n)
    {
      const int idx = frameStart + n;
      if (idx >= paddedLen)
        continue;

      const float w = hannWindow[static_cast<size_t>(n)];
      output[static_cast<size_t>(idx)] += outFrame[static_cast<size_t>(n)] * w;
      windowSum[static_cast<size_t>(idx)] += w * w;
    }
  }

  for (int i = 0; i < paddedLen; ++i)
  {
    if (windowSum[static_cast<size_t>(i)] > 1e-8f)
      output[static_cast<size_t>(i)] /= windowSum[static_cast<size_t>(i)];
  }

  std::vector<float> result(static_cast<size_t>(numSamples), 0.0f);
  for (int i = 0; i < numSamples; ++i)
    result[static_cast<size_t>(i)] = output[static_cast<size_t>(offset + i)];

  float filteredMax = 0.0f;
  double filteredEnergySum = 0.0;
  for (float sample : result)
  {
    filteredMax = std::max(filteredMax, std::abs(sample));
    filteredEnergySum += static_cast<double>(sample) * static_cast<double>(sample);
  }
  const float filteredRms = static_cast<float>(
      std::sqrt(filteredEnergySum / std::max(1, numSamples)));

  if (filteredRms > 1e-10f)
  {
    const float scale = originalRms / filteredRms;
    for (float &sample : result)
      sample *= scale;
  }

  // Guard against large overs introduced by RMS normalization on strongly
  // peaked content. This keeps the output in the same peak ballpark while
  // still primarily matching energy, not peak.
  float renormalizedMax = 0.0f;
  for (float sample : result)
    renormalizedMax = std::max(renormalizedMax, std::abs(sample));

  if (originalMax > 1e-10f && renormalizedMax > originalMax * 1.5f)
  {
    const float peakScale = (originalMax * 1.5f) / renormalizedMax;
    for (float &sample : result)
      sample *= peakScale;
  }

  return result;
}

// ---------------------------------------------------------------------------
// STFT-based processing (reads from precomputed cache)
// ---------------------------------------------------------------------------

TensionProcessor::TensionResult TensionProcessor::processSegmentFromSTFT(
    const std::vector<float>& harmonicSTFT,
    const std::vector<float>& noiseSTFT,
    int totalSTFTFrames,
    int startFrame, int endFrame,
    const std::vector<float>& voicingCurve,
    const std::vector<float>& breathCurve,
    const std::vector<float>& tensionCurve) const
{
  TensionResult result;
  const int numFrames = endFrame - startFrame;
  if (numFrames <= 0 || totalSTFTFrames <= 0)
    return result;

  const int numSamples = numFrames * kHopSize;
  const float maxTiltDb = 12.0f;
  const float nyquist = static_cast<float>(kSampleRate) / 2.0f;
  const float x0 = static_cast<float>(kFFTBin) / (nyquist / 1500.0f);

  // Overlap-add buffers
  const int paddedLen = numFrames * kHopSize + kWinSize;
  const int offset = kWinSize / 2;

  std::vector<float> harmonicOut(static_cast<size_t>(paddedLen), 0.0f);
  std::vector<float> noiseOut(static_cast<size_t>(paddedLen), 0.0f);
  std::vector<float> windowSum(static_cast<size_t>(paddedLen), 0.0f);

  std::vector<float> fftReal(static_cast<size_t>(kFFTBin));
  std::vector<float> fftImag(static_cast<size_t>(kFFTBin));
  std::vector<float> outFrame(static_cast<size_t>(kFFTSize));

  const int binsPerFrame = kFFTBin * 2; // interleaved real/imag

  for (int f = 0; f < numFrames; ++f)
  {
    const int globalFrame = startFrame + f;
    if (globalFrame < 0 || globalFrame >= totalSTFTFrames)
      continue;

    const int curveIdx = std::clamp(globalFrame,
                                     0, static_cast<int>(voicingCurve.size()) - 1);
    const float voicingPct = curveIdx < static_cast<int>(voicingCurve.size())
                                 ? voicingCurve[static_cast<size_t>(curveIdx)]
                                 : 100.0f;
    const float breathPct = curveIdx < static_cast<int>(breathCurve.size())
                                ? breathCurve[static_cast<size_t>(curveIdx)]
                                : 100.0f;
    const float userTension = curveIdx < static_cast<int>(tensionCurve.size())
                                  ? tensionCurve[static_cast<size_t>(curveIdx)]
                                  : 0.0f;
    const float clampedTension = juce::jlimit(-100.0f, 100.0f, userTension);
    const float b = -clampedTension / 100.0f * maxTiltDb;

    const int stftOffset = globalFrame * binsPerFrame;

    // --- Process harmonic: apply voicing + tension tilt ---
    for (int k = 0; k < kFFTBin; ++k)
    {
      float re = harmonicSTFT[static_cast<size_t>(stftOffset + k * 2)];
      float im = harmonicSTFT[static_cast<size_t>(stftOffset + k * 2 + 1)];

      // Voicing scale
      re *= (voicingPct / 100.0f);
      im *= (voicingPct / 100.0f);

      // Tension tilt (same formula as preEmphasisBaseTensionSegment)
      if (std::abs(clampedTension) > 0.001f)
      {
        float filterDb = (-b / x0) * static_cast<float>(k) + b;
        filterDb = juce::jlimit(-maxTiltDb, maxTiltDb, filterDb);
        const float filterGain = std::pow(10.0f, filterDb / 20.0f);
        re *= filterGain;
        im *= filterGain;
      }

      fftReal[static_cast<size_t>(k)] = re;
      fftImag[static_cast<size_t>(k)] = im;
    }

    inverseFFT(fftReal.data(), fftImag.data(), outFrame.data());

    const int frameStart = f * kHopSize;
    for (int n = 0; n < kFFTSize; ++n)
    {
      const int idx = frameStart + n;
      if (idx >= paddedLen) continue;
      const float w = hannWindow[static_cast<size_t>(n)];
      harmonicOut[static_cast<size_t>(idx)] += outFrame[static_cast<size_t>(n)] * w;
      windowSum[static_cast<size_t>(idx)] += w * w;
    }

    // --- Process noise: apply breath scale ---
    for (int k = 0; k < kFFTBin; ++k)
    {
      float re = noiseSTFT[static_cast<size_t>(stftOffset + k * 2)];
      float im = noiseSTFT[static_cast<size_t>(stftOffset + k * 2 + 1)];
      re *= (breathPct / 100.0f);
      im *= (breathPct / 100.0f);
      fftReal[static_cast<size_t>(k)] = re;
      fftImag[static_cast<size_t>(k)] = im;
    }

    inverseFFT(fftReal.data(), fftImag.data(), outFrame.data());

    for (int n = 0; n < kFFTSize; ++n)
    {
      const int idx = frameStart + n;
      if (idx >= paddedLen) continue;
      const float w = hannWindow[static_cast<size_t>(n)];
      noiseOut[static_cast<size_t>(idx)] += outFrame[static_cast<size_t>(n)] * w;
      // windowSum already accumulated from harmonic pass
    }
  }

  // Normalize by window sum
  for (int i = 0; i < paddedLen; ++i)
  {
    if (windowSum[static_cast<size_t>(i)] > 1e-8f)
    {
      harmonicOut[static_cast<size_t>(i)] /= windowSum[static_cast<size_t>(i)];
      noiseOut[static_cast<size_t>(i)] /= windowSum[static_cast<size_t>(i)];
    }
  }

  // Extract result and mix
  result.mixedWaveform.resize(static_cast<size_t>(numSamples), 0.0f);
  for (int i = 0; i < numSamples; ++i)
  {
    result.mixedWaveform[static_cast<size_t>(i)] =
        harmonicOut[static_cast<size_t>(offset + i)] +
        noiseOut[static_cast<size_t>(offset + i)];
  }

  // Compute mel from mixed waveform
  MelSpectrogram melComputer(kSampleRate);
  result.mel = melComputer.compute(result.mixedWaveform.data(), numSamples);

  return result;
}

// ---------------------------------------------------------------------------
// Simple DFT / IDFT (radix-2 Cooley-Tukey FFT)
// ---------------------------------------------------------------------------

static int bitReverse(int x, int log2n)
{
  int result = 0;
  for (int i = 0; i < log2n; ++i)
  {
    result = (result << 1) | (x & 1);
    x >>= 1;
  }
  return result;
}

void TensionProcessor::forwardFFT(const float *frame,
                                  float *outReal, float *outImag) const
{
  const int N = kFFTSize;
  const int log2N = static_cast<int>(std::round(std::log2(static_cast<double>(N))));

  std::vector<float> re(static_cast<size_t>(N));
  std::vector<float> im(static_cast<size_t>(N), 0.0f);

  for (int i = 0; i < N; ++i)
  {
    const int j = bitReverse(i, log2N);
    re[static_cast<size_t>(j)] = frame[i];
  }

  for (int s = 1; s <= log2N; ++s)
  {
    const int m = 1 << s;
    const int halfM = m >> 1;
    const double angle = -2.0 * 3.14159265358979323846 / m;
    const float wRe = static_cast<float>(std::cos(angle));
    const float wIm = static_cast<float>(std::sin(angle));

    for (int k = 0; k < N; k += m)
    {
      float tRe = 1.0f;
      float tIm = 0.0f;
      for (int j = 0; j < halfM; ++j)
      {
        const size_t u = static_cast<size_t>(k + j);
        const size_t v = static_cast<size_t>(k + j + halfM);

        const float tmpRe = tRe * re[v] - tIm * im[v];
        const float tmpIm = tRe * im[v] + tIm * re[v];

        re[v] = re[u] - tmpRe;
        im[v] = im[u] - tmpIm;
        re[u] = re[u] + tmpRe;
        im[u] = im[u] + tmpIm;

        const float newTRe = tRe * wRe - tIm * wIm;
        const float newTIm = tRe * wIm + tIm * wRe;
        tRe = newTRe;
        tIm = newTIm;
      }
    }
  }

  for (int k = 0; k < kFFTBin; ++k)
  {
    outReal[k] = re[static_cast<size_t>(k)];
    outImag[k] = im[static_cast<size_t>(k)];
  }
}

void TensionProcessor::inverseFFT(const float *inReal, const float *inImag,
                                  float *outFrame) const
{
  const int N = kFFTSize;
  const int log2N = static_cast<int>(std::round(std::log2(static_cast<double>(N))));

  std::vector<float> re(static_cast<size_t>(N), 0.0f);
  std::vector<float> im(static_cast<size_t>(N), 0.0f);

  for (int k = 0; k < kFFTBin; ++k)
  {
    re[static_cast<size_t>(k)] = inReal[k];
    im[static_cast<size_t>(k)] = -inImag[k];
  }

  for (int k = 1; k < kFFTBin - 1; ++k)
  {
    re[static_cast<size_t>(N - k)] = inReal[k];
    im[static_cast<size_t>(N - k)] = inImag[k];
  }

  std::vector<float> reP(static_cast<size_t>(N), 0.0f);
  std::vector<float> imP(static_cast<size_t>(N), 0.0f);
  for (int i = 0; i < N; ++i)
  {
    const int j = bitReverse(i, log2N);
    reP[static_cast<size_t>(j)] = re[static_cast<size_t>(i)];
    imP[static_cast<size_t>(j)] = im[static_cast<size_t>(i)];
  }

  for (int s = 1; s <= log2N; ++s)
  {
    const int m = 1 << s;
    const int halfM = m >> 1;
    const double angle = -2.0 * 3.14159265358979323846 / m;
    const float wRe = static_cast<float>(std::cos(angle));
    const float wIm = static_cast<float>(std::sin(angle));

    for (int k = 0; k < N; k += m)
    {
      float tRe = 1.0f;
      float tIm = 0.0f;
      for (int j = 0; j < halfM; ++j)
      {
        const size_t u = static_cast<size_t>(k + j);
        const size_t v = static_cast<size_t>(k + j + halfM);

        const float tmpRe = tRe * reP[v] - tIm * imP[v];
        const float tmpIm = tRe * imP[v] + tIm * reP[v];

        reP[v] = reP[u] - tmpRe;
        imP[v] = imP[u] - tmpIm;
        reP[u] = reP[u] + tmpRe;
        imP[u] = imP[u] + tmpIm;

        const float newTRe = tRe * wRe - tIm * wIm;
        const float newTIm = tRe * wIm + tIm * wRe;
        tRe = newTRe;
        tIm = newTIm;
      }
    }
  }

  const float invN = 1.0f / static_cast<float>(N);
  for (int i = 0; i < N; ++i)
    outFrame[i] = reP[static_cast<size_t>(i)] * invN;
}
