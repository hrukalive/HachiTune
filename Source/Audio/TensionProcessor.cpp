#include "TensionProcessor.h"

#include <algorithm>
#include <cmath>

namespace {

std::vector<float> makePeriodicHannTable(int size)
{
  if (size <= 0)
    return {};

  // JUCE's Hann table includes both endpoints. Build one extra sample and
  // drop the duplicate endpoint to preserve the existing periodic STFT window.
  std::vector<float> table(static_cast<size_t>(size + 1), 0.0f);
  juce::dsp::WindowingFunction<float>::fillWindowingTables(
      table.data(), table.size(), juce::dsp::WindowingFunction<float>::hann,
      false);
  table.resize(static_cast<size_t>(size));
  return table;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TensionProcessor::TensionProcessor()
    : fft(11) // log2(2048) = 11
{
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool TensionProcessor::hasActiveEdits(const float* voicingCurve,
                                      const float* breathCurve,
                                      const float* tensionCurve,
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

TensionProcessor::ProcessedHN TensionProcessor::processSegmentHN(
    const float* harmonicData,
    const float* noiseData,
    int numSamples,
    const float* voicingCurve,
    const float* breathCurve,
    const float* tensionCurve,
    int numFrames) const
{
  ProcessedHN result;
  if (numSamples <= 0 || numFrames <= 0)
    return result;

  result.harmonic.assign(static_cast<size_t>(numSamples), 0.0f);
  result.noise.assign(static_cast<size_t>(numSamples), 0.0f);

  bool hasAnyTension = false;
  for (int i = 0; i < numSamples; ++i)
  {
    const int frame = std::clamp(i / kHopSize, 0, numFrames - 1);
    const float voicingPct = voicingCurve ? voicingCurve[frame] : 100.0f;
    const float breathPct = breathCurve ? breathCurve[frame] : 100.0f;
    const float tension = tensionCurve ? tensionCurve[frame] : 0.0f;

    const float harmonicSample = harmonicData ? harmonicData[i] : 0.0f;
    const float noiseSample = noiseData ? noiseData[i] : 0.0f;
    result.harmonic[static_cast<size_t>(i)] =
        harmonicSample * (voicingPct / 100.0f);
    result.noise[static_cast<size_t>(i)] =
        noiseSample * (breathPct / 100.0f);
    hasAnyTension = hasAnyTension || std::abs(tension) > 0.001f;
  }

  if (hasAnyTension)
  {
    result.harmonic = preEmphasisBaseTensionSegment(
        result.harmonic, tensionCurve, numFrames);
  }

  return result;
}

std::vector<float> TensionProcessor::computeSTFT(
    const juce::AudioBuffer<float>& buffer)
{
  if (buffer.getNumChannels() == 0 || buffer.getNumSamples() == 0)
    return {};

  const float* data = buffer.getReadPointer(0);
  const int numSamples = buffer.getNumSamples();
  const int numSTFTFrames = (numSamples + kHopSize - 1) / kHopSize;

  juce::dsp::FFT stftFFT(11); // log2(2048) = 11
  const auto windowTable = makePeriodicHannTable(kWinSize);

  std::vector<float> stft(
      static_cast<size_t>(numSTFTFrames * kFFTBin * 2), 0.0f);
  std::vector<float> fftBuf(static_cast<size_t>(kFFTSize * 2), 0.0f);

  for (int f = 0; f < numSTFTFrames; ++f)
  {
    const int center = f * kHopSize;
    const int frameStart = center - kFFTSize / 2;
    std::fill(fftBuf.begin(), fftBuf.end(), 0.0f);

    for (int n = 0; n < kWinSize; ++n)
    {
      const int idx = frameStart + n;
      const float sample =
          (idx >= 0 && idx < numSamples) ? data[idx] : 0.0f;
      fftBuf[static_cast<size_t>(n)] =
          sample * windowTable[static_cast<size_t>(n)];
    }

    stftFFT.performRealOnlyForwardTransform(fftBuf.data());

    const int offset = f * kFFTBin * 2;
    for (int k = 0; k < kFFTBin; ++k)
    {
      stft[static_cast<size_t>(offset + k * 2)] =
          fftBuf[static_cast<size_t>(k * 2)];
      stft[static_cast<size_t>(offset + k * 2 + 1)] =
          fftBuf[static_cast<size_t>(k * 2 + 1)];
    }
  }

  return stft;
}

// ---------------------------------------------------------------------------
// Segment STFT processing
// ---------------------------------------------------------------------------

std::vector<float> TensionProcessor::preEmphasisBaseTensionSegment(
    const std::vector<float>& scaledHarmonic,
    const float* tensionCurve,
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
    originalEnergySum += static_cast<double>(sample) *
                         static_cast<double>(sample);
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
    padded[static_cast<size_t>(offset + i)] =
        scaledHarmonic[static_cast<size_t>(i)];

  std::vector<float> output(static_cast<size_t>(paddedLen), 0.0f);
  std::vector<float> windowSum(static_cast<size_t>(paddedLen), 0.0f);
  std::vector<float> fftBuf(static_cast<size_t>(kFFTSize * 2), 0.0f);
  const auto windowTable = makePeriodicHannTable(kWinSize);

  constexpr float maxTiltDb = 12.0f;

  for (int f = 0; f < stftFrames; ++f)
  {
    const int frameStart = f * kHopSize;
    const int curveFrame = std::clamp(f, 0, numFrames - 1);
    const float userTension = tensionCurve ? tensionCurve[curveFrame] : 0.0f;
    const float clampedTension = juce::jlimit(-100.0f, 100.0f, userTension);
    const float b = -clampedTension / 100.0f * maxTiltDb;

    std::fill(fftBuf.begin(), fftBuf.end(), 0.0f);
    for (int n = 0; n < kWinSize; ++n)
    {
      const int idx = frameStart + n;
      const float sample =
          idx < paddedLen ? padded[static_cast<size_t>(idx)] : 0.0f;
      fftBuf[static_cast<size_t>(n)] =
          sample * windowTable[static_cast<size_t>(n)];
    }

    fft.performRealOnlyForwardTransform(fftBuf.data());

    for (int k = 0; k < kFFTBin; ++k)
    {
      float filterDb = (-b / x0) * static_cast<float>(k) + b;
      filterDb = juce::jlimit(-maxTiltDb, maxTiltDb, filterDb);
      const float filterGain = std::pow(10.0f, filterDb / 20.0f);
      fftBuf[static_cast<size_t>(k * 2)] *= filterGain;
      fftBuf[static_cast<size_t>(k * 2 + 1)] *= filterGain;
    }

    fft.performRealOnlyInverseTransform(fftBuf.data());

    for (int n = 0; n < kWinSize; ++n)
    {
      const int idx = frameStart + n;
      if (idx >= paddedLen)
        continue;

      const float w = windowTable[static_cast<size_t>(n)];
      output[static_cast<size_t>(idx)] += fftBuf[static_cast<size_t>(n)] * w;
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

  double filteredEnergySum = 0.0;
  for (float sample : result)
    filteredEnergySum += static_cast<double>(sample) *
                         static_cast<double>(sample);
  const float filteredRms = static_cast<float>(
      std::sqrt(filteredEnergySum / std::max(1, numSamples)));

  if (filteredRms > 1e-10f)
  {
    const float scale = originalRms / filteredRms;
    for (float& sample : result)
      sample *= scale;
  }

  float renormalizedMax = 0.0f;
  for (float sample : result)
    renormalizedMax = std::max(renormalizedMax, std::abs(sample));

  if (originalMax > 1e-10f && renormalizedMax > originalMax * 1.5f)
  {
    const float peakScale = (originalMax * 1.5f) / renormalizedMax;
    for (float& sample : result)
      sample *= peakScale;
  }

  return result;
}
