#pragma once

#include "../../JuceHeader.h"
#include "../../Models/Project.h"

class MelViewComponent : public juce::Component
{
public:
  MelViewComponent();

  void setProject(Project* project);
  void rebuildMelImage();
  void rebuildF0Path();

  void paint(juce::Graphics& g) override;
  void resized() override;
  void mouseWheelMove(const juce::MouseEvent& e,
                      const juce::MouseWheelDetails& wheel) override;
  void mouseDrag(const juce::MouseEvent& e) override;
  void mouseDown(const juce::MouseEvent& e) override;

private:
  static juce::Colour viridisColour(float t);
  static float hzToMelBin(float hz, int numMels = 128);

  Project* project = nullptr;
  juce::Image melImage;
  bool melImageDirty = true;
  float scrollOffsetFrames = 0.0f;
  float framesPerPixel = 1.0f;
  float dragStartScrollOffset = 0.0f;
  juce::Point<float> dragStartPos;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MelViewComponent)
};
