#pragma once

#include "UndoableAction.h"
#include "../Models/Project.h"
#include <functional>
#include <vector>

class WarpMarkerStateAction : public UndoableAction
{
public:
    WarpMarkerStateAction(Project* project,
                          std::vector<Project::WarpMarker> oldMarkers,
                          std::vector<Project::WarpMarker> newMarkers,
                          std::function<void(const std::vector<Project::WarpMarker>&)> onApply = nullptr)
        : project(project),
          oldMarkers(std::move(oldMarkers)),
          newMarkers(std::move(newMarkers)),
          onApply(std::move(onApply)) {}

    void undo() override { apply(oldMarkers); }
    void redo() override { apply(newMarkers); }
    juce::String getName() const override { return "Edit Warp Markers"; }

private:
    void apply(const std::vector<Project::WarpMarker>& markers)
    {
        if (!project)
            return;
        if (onApply)
        {
            onApply(markers);
            return;
        }

        project->setWarpMarkers(markers);
    }

    Project* project = nullptr;
    std::vector<Project::WarpMarker> oldMarkers;
    std::vector<Project::WarpMarker> newMarkers;
    std::function<void(const std::vector<Project::WarpMarker>&)> onApply;
};
