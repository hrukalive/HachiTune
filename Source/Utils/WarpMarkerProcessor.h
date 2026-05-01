#pragma once

#include "../Models/Project.h"

#include <vector>

namespace WarpMarkerProcessor
{
struct Boundary
{
    Note* left = nullptr;
    Note* right = nullptr;
    int sourceFrame = 0;
    int currentFrame = 0;
    bool active = false;
};

int getSourceFrameLimit(const Project& project);
std::vector<Project::WarpMarker> normalizeMarkers(
    const Project& project,
    const std::vector<Project::WarpMarker>& markers);
std::vector<Project::WarpMarker> buildWarpMapWithEndpoints(
    const Project& project,
    const std::vector<Project::WarpMarker>& markers);
void rebuildSourceDerivedOutput(Project& project,
                                const std::vector<Project::WarpMarker>& markers);
void recomputeFromMarkers(Project& project,
                          const std::vector<Project::WarpMarker>& currentMarkers,
                          const std::vector<Project::WarpMarker>& markers,
                          bool updateProjectMarkers);
std::vector<Boundary> collectBoundaries(Project& project);
bool hasMarkerAtSourceFrame(const std::vector<Project::WarpMarker>& markers,
                            int sourceFrame);
int mapSourceFrame(const Project& project,
                   int sourceFrame,
                   const std::vector<Project::WarpMarker>& markers);
int computeSegmentMinimumOutputSpan(const Project& project,
                                    int sourceStartFrame,
                                    int sourceEndFrame,
                                    int minNoteFrames = 3);
void recomputeFromMarkers(Project& project,
                          const std::vector<Project::WarpMarker>& markers,
                          bool updateProjectMarkers = true);
} // namespace WarpMarkerProcessor
