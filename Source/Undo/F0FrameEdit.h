#pragma once

/**
 * Represents a single frame edit in the F0 pitch curve.
 * Captures both old and new values for undo/redo support.
 */
struct F0FrameEdit
{
    int idx = -1;
    float oldF0 = 0.0f;
    float newF0 = 0.0f;
    float oldDelta = 0.0f;
    float newDelta = 0.0f;
    bool oldVoiced = false;
    bool newVoiced = false;
    bool oldEdited = false;
    bool newEdited = true;
};
