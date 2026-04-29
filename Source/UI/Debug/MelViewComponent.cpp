#include "MelViewComponent.h"
#include <cmath>
#include <algorithm>

MelViewComponent::MelViewComponent() {}

void MelViewComponent::setProject(Project* proj)
{
  project = proj;
  melImageDirty = true;
  repaint();
}

void MelViewComponent::rebuildMelImage()
{
  if (!project)
  {
    melImage = juce::Image();
    return;
  }

  const auto& audioData = project->getAudioData();
  const auto& mel = audioData.melSpectrogram;
  if (mel.empty())
  {
    melImage = juce::Image();
    return;
  }

  const int numFrames = static_cast<int>(mel.size());
  const int numMels = mel[0].empty() ? 0 : static_cast<int>(mel[0].size());
  if (numMels <= 0)
  {
    melImage = juce::Image();
    return;
  }

  float globalMin = std::numeric_limits<float>::max();
  float globalMax = std::numeric_limits<float>::lowest();
  for (const auto& frame : mel)
  {
    for (float val : frame)
    {
      globalMin = std::min(globalMin, val);
      globalMax = std::max(globalMax, val);
    }
  }

  const float range = (globalMax - globalMin > 1e-6f)
                          ? (globalMax - globalMin)
                          : 1.0f;

  melImage = juce::Image(juce::Image::RGB, numFrames, numMels, false);
  for (int x = 0; x < numFrames; ++x)
  {
    const auto& frame = mel[x];
    for (int m = 0; m < numMels && m < static_cast<int>(frame.size()); ++m)
    {
      const int y = numMels - 1 - m;
      float normalised = (frame[m] - globalMin) / range;
      normalised = juce::jlimit(0.0f, 1.0f, normalised);
      melImage.setPixelAt(x, y, viridisColour(normalised));
    }
  }

  melImageDirty = false;
}

void MelViewComponent::rebuildF0Path()
{
  repaint();
}

void MelViewComponent::paint(juce::Graphics& g)
{
  g.fillAll(juce::Colour(0xff1a1a2e));

  if (!project)
  {
    g.setColour(juce::Colours::grey);
    g.drawText("No project loaded", getLocalBounds(),
               juce::Justification::centred);
    return;
  }

  if (melImageDirty)
    rebuildMelImage();

  const auto& audioData = project->getAudioData();
  const auto& mel = audioData.melSpectrogram;
  if (melImage.isNull() || mel.empty() || mel[0].empty())
  {
    g.setColour(juce::Colours::grey);
    g.drawText("No mel data", getLocalBounds(),
               juce::Justification::centred);
    return;
  }

  const int numMels = static_cast<int>(mel[0].size());
  const float h = static_cast<float>(getHeight());
  const float w = static_cast<float>(getWidth());

  const int startFrame = static_cast<int>(scrollOffsetFrames);
  const int visibleFrames = static_cast<int>(w * framesPerPixel);
  const int numFrames = static_cast<int>(mel.size());
  const int endFrame = std::min(startFrame + visibleFrames, numFrames);

  if (startFrame < endFrame)
  {
    auto subImage = melImage.getClippedImage(
        juce::Rectangle<int>(startFrame, 0,
                              endFrame - startFrame, numMels));
    g.drawImage(subImage, 0.0f, 0.0f, w, h,
                0, 0, subImage.getWidth(), subImage.getHeight());
  }

  // Helper: convert frame to pixel X
  auto frameToX = [&](int frame) -> float {
    return static_cast<float>(frame - startFrame) / framesPerPixel;
  };

  // Draw warp markers
  const auto& markers = project->getWarpMarkers();
  if (!markers.empty())
  {
    for (int i = 0; i < static_cast<int>(markers.size()); ++i)
    {
      const auto& marker = markers[i];
      const float srcX = frameToX(marker.sourceFrame);
      const float outX = frameToX(marker.outputFrame);
      const juce::String label = juce::String(i);

      // Source position: dashed vertical line (cyan)
      if (srcX >= 0.0f && srcX <= w)
      {
        g.setColour(juce::Colours::cyan.withAlpha(0.6f));
        const float dashLengths[] = { 4.0f, 3.0f };
        juce::Path srcLine;
        srcLine.startNewSubPath(srcX, 0.0f);
        srcLine.lineTo(srcX, h);
        juce::Path dashedSrc;
        juce::PathStrokeType(1.0f).createDashedStroke(dashedSrc, srcLine, dashLengths, 2);
        g.fillPath(dashedSrc);
        g.setColour(juce::Colours::cyan);
        g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText("S" + label, static_cast<int>(srcX) + 2, 2, 30, 14,
                   juce::Justification::centredLeft);
      }

      // Output position: solid vertical line (yellow)
      if (outX >= 0.0f && outX <= w)
      {
        g.setColour(juce::Colours::yellow.withAlpha(0.8f));
        g.drawVerticalLine(static_cast<int>(outX), 0.0f, h);
        g.setColour(juce::Colours::yellow);
        g.setFont(juce::Font(12.0f, juce::Font::bold));
        g.drawText("O" + label, static_cast<int>(outX) + 2, static_cast<int>(h) - 16, 30, 14,
                   juce::Justification::centredLeft);
      }
    }
  }

  // Draw F0 curve from editedData
  const auto& editedData = project->getEditedData();
  const auto& f0 = editedData.f0;
  if (f0.empty())
    return;

  juce::Path f0Path;
  bool prevValid = false;

  for (int x = 0; x < static_cast<int>(w); ++x)
  {
    const int frame = startFrame + static_cast<int>(x * framesPerPixel);
    if (frame < 0 || frame >= static_cast<int>(f0.size()))
    {
      prevValid = false;
      continue;
    }

    const float freq = f0[frame];
    if (freq <= 0.0f)
    {
      prevValid = false;
      continue;
    }

    const float melBin = hzToMelBin(freq, numMels);
    const float yPos = h - (melBin / static_cast<float>(numMels)) * h;

    if (prevValid)
      f0Path.lineTo(static_cast<float>(x), yPos);
    else
      f0Path.startNewSubPath(static_cast<float>(x), yPos);

    prevValid = true;
  }

  g.setColour(juce::Colours::red);
  g.strokePath(f0Path, juce::PathStrokeType(2.0f));
}

void MelViewComponent::resized() {}

void MelViewComponent::mouseWheelMove(const juce::MouseEvent& e,
                                       const juce::MouseWheelDetails& wheel)
{
  juce::ignoreUnused(e);

  if (wheel.deltaY != 0.0f)
  {
    const float zoomFactor = (wheel.deltaY > 0) ? 0.8f : 1.25f;
    framesPerPixel = juce::jlimit(0.1f, 100.0f,
                                   framesPerPixel * zoomFactor);
    repaint();
  }

  if (wheel.deltaX != 0.0f)
  {
    scrollOffsetFrames -= wheel.deltaX * 50.0f;
    scrollOffsetFrames = std::max(0.0f, scrollOffsetFrames);
    repaint();
  }
}

void MelViewComponent::mouseDown(const juce::MouseEvent& e)
{
  dragStartScrollOffset = scrollOffsetFrames;
  dragStartPos = e.position;
}

void MelViewComponent::mouseDrag(const juce::MouseEvent& e)
{
  const float dx = e.position.x - dragStartPos.x;
  scrollOffsetFrames = std::max(0.0f,
                                 dragStartScrollOffset - dx * framesPerPixel);
  repaint();
}

juce::Colour MelViewComponent::viridisColour(float t)
{
  t = juce::jlimit(0.0f, 1.0f, t);

  float r, gr, b;
  if (t < 0.25f)
  {
    float s = t / 0.25f;
    r = 0.267f + s * (0.282f - 0.267f);
    gr = 0.004f + s * (0.140f - 0.004f);
    b = 0.329f + s * (0.458f - 0.329f);
  }
  else if (t < 0.5f)
  {
    float s = (t - 0.25f) / 0.25f;
    r = 0.282f + s * (0.127f - 0.282f);
    gr = 0.140f + s * (0.566f - 0.140f);
    b = 0.458f + s * (0.551f - 0.458f);
  }
  else if (t < 0.75f)
  {
    float s = (t - 0.5f) / 0.25f;
    r = 0.127f + s * (0.741f - 0.127f);
    gr = 0.566f + s * (0.873f - 0.566f);
    b = 0.551f + s * (0.150f - 0.551f);
  }
  else
  {
    float s = (t - 0.75f) / 0.25f;
    r = 0.741f + s * (0.993f - 0.741f);
    gr = 0.873f + s * (0.906f - 0.873f);
    b = 0.150f + s * (0.144f - 0.150f);
  }

  return juce::Colour::fromFloatRGBA(r, gr, b, 1.0f);
}

float MelViewComponent::hzToMelBin(float hz, int numMels)
{
  if (hz <= 0.0f)
    return 0.0f;

  const float mel = 2595.0f * std::log10(1.0f + hz / 700.0f);
  constexpr float maxMel = 3800.0f;
  return (mel / maxMel) * static_cast<float>(numMels);
}
