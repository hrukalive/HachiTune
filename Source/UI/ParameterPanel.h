#pragma once

#include "../JuceHeader.h"
#include "../Models/Note.h"
#include "../Models/Project.h"
#include "../Utils/Constants.h"
#include "../Utils/UI/Theme.h"

class DarkLookAndFeel;  // Forward declaration

class ParameterPanel : public juce::Component,
                       public juce::Slider::Listener,
                       public juce::Button::Listener
{
public:
    ParameterPanel();
    ~ParameterPanel() override;

    void paint(juce::Graphics& g) override;
    void resized() override;

    void sliderValueChanged(juce::Slider* slider) override;
    void sliderDragEnded(juce::Slider* slider) override;
    void buttonClicked(juce::Button* button) override;

    void setProject(Project* proj);
    void setSelectedNote(Note* note);
    void updateFromNote();
    void updateGlobalSliders();

    int getPreferredHeight() const { return 500; }

    std::function<void()> onParameterChanged;
    std::function<void()> onParameterEditFinished;  // Called when slider drag ends
    std::function<void()> onGlobalPitchChanged;
    std::function<void(float)> onVolumeChanged;  // Called with volume in dB
    
private:
    void setupSlider(juce::Slider& slider, juce::Label& label,
                    const juce::String& name, double min, double max, double def);

    Project* project = nullptr;
    Note* selectedNote = nullptr;
    bool isUpdating = false;  // Prevent feedback loops

    // Note info
    juce::Label noteInfoLabel;
    juce::Rectangle<int> noteCardBounds;
    
    // Pitch controls
    juce::Label pitchSectionLabel { {}, "Pitch" };
    juce::Slider pitchOffsetSlider;
    juce::Label pitchOffsetLabel { {}, "Offset (semitones):" };
    juce::Rectangle<int> pitchCardBounds;

    // Volume control (using rotary knob style)
    juce::Label volumeSectionLabel { {}, "Volume" };
    juce::Slider volumeKnob;
    juce::Label volumeValueLabel;  // Shows current dB value
    juce::Rectangle<int> volumeCardBounds;

    juce::Label formantSectionLabel { {}, "Formant" };
    juce::Slider formantShiftSlider;
    juce::Label formantShiftLabel { {}, "Shift (semitones):" };
    juce::Rectangle<int> formantCardBounds;
    
    // Global settings
    juce::Label globalSectionLabel { {}, "Global Settings" };
    juce::Slider globalPitchSlider;
    juce::Label globalPitchLabel { {}, "Global Pitch:" };
    juce::Rectangle<int> globalCardBounds;
    
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterPanel)
};
