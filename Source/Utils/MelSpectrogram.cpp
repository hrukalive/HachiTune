#include "MelSpectrogram.h"
#include "MelScale.h"
#include <cmath>
#include <algorithm>

MelSpectrogram::MelSpectrogram(int sampleRate, int nFft, int hopSize,
                               int numMels, float fMin, float fMax)
    : sampleRate(sampleRate), nFft(nFft), hopSize(hopSize),
      numMels(numMels), fMin(fMin), fMax(fMax),
      fft(static_cast<int>(std::log2(nFft)))
{
    // Create Hann window (periodic, matches librosa default)
    window.resize(nFft);
    for (int i = 0; i < nFft; ++i)
    {
        window[i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * i / nFft));
    }
    
    createMelFilterbank();
}

void MelSpectrogram::createMelFilterbank()
{
    float melMin = MelScale::hzToMel(fMin);
    float melMax = MelScale::hzToMel(fMax);

    // Create mel points (numMels + 2 points for the triangular filters)
    std::vector<float> melPoints(numMels + 2);
    for (int i = 0; i <= numMels + 1; ++i)
    {
        melPoints[i] = melMin + (melMax - melMin) * i / (numMels + 1);
    }

    // Convert to Hz
    std::vector<float> hzPoints(numMels + 2);
    for (int i = 0; i <= numMels + 1; ++i)
    {
        hzPoints[i] = MelScale::melToHz(melPoints[i]);
    }
    
    // Convert to FFT bins
    int numBins = nFft / 2 + 1;
    std::vector<float> binPoints(numMels + 2);
    for (int i = 0; i <= numMels + 1; ++i)
    {
        // Use continuous (fractional) bin numbers like librosa
        binPoints[i] = (nFft + 1) * hzPoints[i] / sampleRate;
    }
    
    // Create filterbank with Slaney normalization (area normalization)
    // Store in sparse representation: only non-zero bins per mel band
    melFilterbank.resize(numMels);
    for (int m = 0; m < numMels; ++m)
    {
        float fLow = hzPoints[m];
        float fCenter = hzPoints[m + 1];
        float fHigh = hzPoints[m + 2];
        
        // Slaney normalization: divide by the width of the mel band
        float enorm = 2.0f / (fHigh - fLow);
        
        // Find the range of bins that fall within [fLow, fHigh]
        int firstBin = numBins;  // will be narrowed
        int lastBin = -1;        // will be narrowed
        
        for (int k = 0; k < numBins; ++k)
        {
            float freq = static_cast<float>(k) * sampleRate / nFft;
            if (freq >= fLow && freq <= fHigh)
            {
                if (k < firstBin) firstBin = k;
                if (k > lastBin)  lastBin = k;
            }
        }
        
        MelBand& band = melFilterbank[m];
        if (lastBin < firstBin)
        {
            // Empty band (shouldn't happen in practice)
            band.startBin = 0;
            band.endBin = 0;
            continue;
        }
        
        band.startBin = firstBin;
        band.endBin = lastBin + 1;  // exclusive
        band.weights.resize(band.endBin - band.startBin, 0.0f);
        
        for (int k = firstBin; k <= lastBin; ++k)
        {
            float freq = static_cast<float>(k) * sampleRate / nFft;
            
            if (freq >= fLow && freq < fCenter)
            {
                // Rising edge
                band.weights[k - firstBin] = enorm * (freq - fLow) / (fCenter - fLow);
            }
            else if (freq >= fCenter && freq <= fHigh)
            {
                // Falling edge
                band.weights[k - firstBin] = enorm * (fHigh - freq) / (fHigh - fCenter);
            }
        }
    }
}

std::vector<std::vector<float>> MelSpectrogram::compute(const float* audio, int numSamples)
{
    // Add center padding for better frame alignment (matches librosa default)
    // This ensures the first frame is centered at hopSize/2
    int padLeft = nFft / 2;
    int padRight = nFft / 2;
    
    // Calculate number of frames with proper padding
    int paddedLength = numSamples + padLeft + padRight;
    int numFrames = (paddedLength - nFft) / hopSize + 1;
    if (numFrames < 1)
    {
        numFrames = 1;
    }
    
    std::vector<std::vector<float>> mel(numFrames);
    int numBins = nFft / 2 + 1;
    
    std::vector<float> frame(nFft * 2, 0.0f);  // Complex FFT buffer
    std::vector<float> mag(numBins);            // Reused across frames
    
    for (int i = 0; i < numFrames; ++i)
    {
        // Calculate sample position in original audio (accounting for padding)
        int centerSample = i * hopSize;
        int startSample = centerSample - padLeft;
        
        // Copy and window with proper boundary handling
        std::fill(frame.begin(), frame.end(), 0.0f);
        for (int j = 0; j < nFft; ++j)
        {
            int srcIdx = startSample + j;
            
            if (srcIdx < 0)
            {
                // Left padding: reflect
                frame[j] = audio[std::min(-srcIdx - 1, numSamples - 1)] * window[j];
            }
            else if (srcIdx >= numSamples)
            {
                // Right padding: reflect
                int reflectIdx = numSamples - 1 - (srcIdx - numSamples);
                frame[j] = audio[std::max(0, reflectIdx)] * window[j];
            }
            else
            {
                // Normal case
                frame[j] = audio[srcIdx] * window[j];
            }
        }
        
        // Perform FFT
        fft.performRealOnlyForwardTransform(frame.data());
        
        // Compute magnitude spectrum with small epsilon to avoid log(0)
        for (int k = 0; k < numBins; ++k)
        {
            float real = frame[k * 2];
            float imag = frame[k * 2 + 1];
            mag[k] = std::sqrt(real * real + imag * imag + 1e-9f);
        }
        
        // Apply sparse mel filterbank
        mel[i].resize(numMels);
        for (int m = 0; m < numMels; ++m)
        {
            const auto& band = melFilterbank[m];
            float sum = 0.0f;
            for (int k = band.startBin; k < band.endBin; ++k)
            {
                sum += mag[k] * band.weights[k - band.startBin];
            }
            
            // Log scale (natural log for vocoder compatibility)
            // Use slightly larger epsilon to match common vocoder implementations
            mel[i][m] = std::log(std::max(sum, 1e-10f));
        }
    }
    
    return mel;
}
