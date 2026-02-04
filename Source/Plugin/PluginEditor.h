#pragma once

#include "../JuceHeader.h"
#include "../UI/IMainView.h"
#include "../UI/MainViewFactory.h"
#include <memory>
#include "PluginProcessor.h"

class HachiTuneAudioProcessorEditor : public juce::AudioProcessorEditor
#if JucePlugin_Enable_ARA
    , public juce::AudioProcessorEditorARAExtension
#endif
{
public:
    explicit HachiTuneAudioProcessorEditor(HachiTuneAudioProcessor&);
    ~HachiTuneAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Grab keyboard focus when editor becomes visible
    void visibilityChanged() override;

    // Handle mouse clicks to grab focus
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void setupARAMode();
    void setupNonARAMode();
    void setupCallbacks();

    HachiTuneAudioProcessor& audioProcessor;
    std::unique_ptr<IMainView> mainView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(HachiTuneAudioProcessorEditor)
};
