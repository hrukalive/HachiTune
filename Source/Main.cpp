// Main.cpp - Entry point for Windows/Linux builds
// For macOS, use Main.mm which includes Objective-C++ code

#include "JuceHeader.h"
#include "UI/MainComponent.h"
#include "Utils/Constants.h"
#include "Utils/Localization.h"

#if JUCE_WINDOWS
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

class PitchEditorApplication : public juce::JUCEApplication
{
public:
    PitchEditorApplication() {}

    const juce::String getApplicationName() override
    {
        return "Pitch Editor";
    }

    const juce::String getApplicationVersion() override
    {
        return "1.0.0";
    }

    bool moreThanOneInstanceAllowed() override
    {
        return true;
    }

    void initialise(const juce::String& commandLine) override
    {
        juce::ignoreUnused(commandLine);
        Localization::loadFromSettings();
        mainWindow = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted(const juce::String& commandLine) override
    {
        juce::ignoreUnused(commandLine);
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow(juce::String name)
            : DocumentWindow(name,
                            juce::Colour(COLOR_BACKGROUND),
                            0)  // No JUCE buttons - we use custom title bar
        {
            setUsingNativeTitleBar(false);
            setTitleBarHeight(0);
            setContentOwned(new MainComponent(), true);

            setResizable(true, true);
            centreWithSize(getWidth(), getHeight());

            setVisible(true);

#if JUCE_WINDOWS
            // Enable rounded corners on Windows 11+
            if (auto* peer = getPeer())
            {
                if (auto hwnd = (HWND)peer->getNativeHandle())
                {
                    DWORD preference = 2;  // DWMWCP_ROUND
                    DwmSetWindowAttribute(hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/,
                                         &preference, sizeof(preference));
                }
            }
#endif
        }

        void closeButtonPressed() override
        {
            JUCEApplication::getInstance()->systemRequestedQuit();
        }

    private:
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(PitchEditorApplication)
