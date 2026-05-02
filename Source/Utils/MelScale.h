#pragma once

#include <cmath>
#include <vector>
#include <algorithm>

namespace MelScale
{

constexpr float kFSp = 200.0f / 3.0f;
constexpr float kMinLogHz = 1000.0f;
constexpr float kMinLogMel = kMinLogHz / kFSp;  // 15.0
inline const float kLogStep = std::log(6.4f) / 27.0f;

inline float hzToMel(float hz)
{
  if (hz < kMinLogHz)
    return hz / kFSp;
  return kMinLogMel + std::log(hz / kMinLogHz) / kLogStep;
}

inline float melToHz(float mel)
{
  if (mel < kMinLogMel)
    return kFSp * mel;
  return kMinLogHz * std::exp(kLogStep * (mel - kMinLogMel));
}

inline std::vector<float> computeCenterFrequencies(int numMels,
                                                    float fMin = 40.0f,
                                                    float fMax = 16000.0f)
{
  const float melMin = hzToMel(fMin);
  const float melMax = hzToMel(fMax);

  std::vector<float> centers(numMels);
  for (int i = 0; i < numMels; ++i)
  {
    float melPoint = melMin + (melMax - melMin) * (i + 1) / (numMels + 1);
    centers[i] = melToHz(melPoint);
  }
  return centers;
}

inline float hzToMelBin(float hz, const std::vector<float>& centerFreqs)
{
  if (hz <= 0.0f || centerFreqs.empty())
    return 0.0f;

  const int n = static_cast<int>(centerFreqs.size());

  if (hz <= centerFreqs.front())
    return 0.0f;
  if (hz >= centerFreqs.back())
    return static_cast<float>(n - 1);

  auto it = std::lower_bound(centerFreqs.begin(), centerFreqs.end(), hz);
  int idx = static_cast<int>(it - centerFreqs.begin());
  if (idx == 0)
    return 0.0f;

  float lo = centerFreqs[idx - 1];
  float hi = centerFreqs[idx];
  float t = (hz - lo) / (hi - lo);
  return static_cast<float>(idx - 1) + t;
}

} // namespace MelScale
