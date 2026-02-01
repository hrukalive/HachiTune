#include "MenuHandler.h"

MenuHandler::MenuHandler() = default;

juce::StringArray MenuHandler::getMenuBarNames() {
    if (pluginMode)
        return {TR("menu.edit"), TR("menu.view"), TR("menu.settings")};
    return {TR("menu.file"), TR("menu.edit"), TR("menu.view"), TR("menu.settings")};
}

juce::PopupMenu MenuHandler::getMenuForIndex(int menuIndex, const juce::String& /*menuName*/) {
    juce::PopupMenu menu;

    if (pluginMode) {
        if (menuIndex == 0) {
            // Edit menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::undo);
                menu.addCommandItem(commandManager, CommandIDs::redo);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::selectAll);
            }
        } else if (menuIndex == 1) {
            // View menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::showDeltaPitch);
                menu.addCommandItem(commandManager, CommandIDs::showBasePitch);
            }
        } else if (menuIndex == 2) {
            // Settings menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::showSettings);
            }
        }
    } else {
        if (menuIndex == 0) {
            // File menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::openFile);
                menu.addCommandItem(commandManager, CommandIDs::saveProject);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::exportAudio);
                menu.addCommandItem(commandManager, CommandIDs::exportMidi);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::quit);
            }
        } else if (menuIndex == 1) {
            // Edit menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::undo);
                menu.addCommandItem(commandManager, CommandIDs::redo);
                menu.addSeparator();
                menu.addCommandItem(commandManager, CommandIDs::selectAll);
            }
        } else if (menuIndex == 2) {
            // View menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::showDeltaPitch);
                menu.addCommandItem(commandManager, CommandIDs::showBasePitch);
            }
        } else if (menuIndex == 3) {
            // Settings menu
            if (commandManager) {
                menu.addCommandItem(commandManager, CommandIDs::showSettings);
            }
        }
    }

    return menu;
}

void MenuHandler::menuItemSelected(int menuItemID, int /*topLevelMenuIndex*/) {
    // Command items are handled automatically by ApplicationCommandManager
    // All menu items are now handled by commands - this method can be empty
    (void)menuItemID;  // Unused parameter
}
