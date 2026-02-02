#pragma once

#include "../../JuceHeader.h"
#include "../../Utils/UndoManager.h"
#include "../../Utils/Localization.h"
#include "../Commands.h"
#include <functional>

/**
 * Handles menu bar creation and menu item selection.
 */
class MenuHandler : public juce::MenuBarModel {
public:
    MenuHandler();
    ~MenuHandler() override = default;

    void setPluginMode(bool isPlugin) { pluginMode = isPlugin; }
    void setUndoManager(PitchUndoManager* mgr) { undoManager = mgr; }
    void setCommandManager(juce::ApplicationCommandManager* mgr) { commandManager = mgr; }

    // MenuBarModel interface
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int menuIndex, const juce::String& menuName) override;
    void menuItemSelected(int menuItemID, int topLevelMenuIndex) override;

private:
    bool pluginMode = false;
    PitchUndoManager* undoManager = nullptr;
    juce::ApplicationCommandManager* commandManager = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MenuHandler)
};
