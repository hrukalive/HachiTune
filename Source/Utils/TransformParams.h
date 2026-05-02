#pragma once

#include "../Models/Note.h"

/**
 * Stores pitch tool transformation parameters for a single note.
 * Used by UndoManager to capture and restore transformation state non-destructively.
 */
struct TransformParams
{
    float tiltLeft = 0.0f;
    float tiltRight = 0.0f;
    float varianceScale = 1.0f;
    int smoothLeftFrames = 0;
    int smoothRightFrames = 0;
    float pitchOffset = 0.0f;
    float highPassFilterStrength = 0.0f;
    float lowPassFilterStrength = 0.0f;

    TransformParams() = default;

    /** Capture all transformation params from a note. */
    static TransformParams fromNote(const Note& note)
    {
        TransformParams p;
        p.tiltLeft = note.getTiltLeft();
        p.tiltRight = note.getTiltRight();
        p.varianceScale = note.getVarianceScale();
        p.smoothLeftFrames = note.getSmoothLeftFrames();
        p.smoothRightFrames = note.getSmoothRightFrames();
        p.pitchOffset = note.getPitchOffset();
        p.highPassFilterStrength = note.getHighPassFilterStrength();
        p.lowPassFilterStrength = note.getLowPassFilterStrength();
        return p;
    }

    /** Apply all transformation params back to a note. */
    void applyToNote(Note& note) const
    {
        note.setPitchOffset(pitchOffset);
        note.setTiltLeft(tiltLeft);
        note.setTiltRight(tiltRight);
        note.setVarianceScale(varianceScale);
        note.setSmoothLeftFrames(smoothLeftFrames);
        note.setSmoothRightFrames(smoothRightFrames);
        note.setHighPassFilterStrength(highPassFilterStrength);
        note.setLowPassFilterStrength(lowPassFilterStrength);
    }

    bool operator==(const TransformParams& other) const
    {
        return tiltLeft == other.tiltLeft &&
               tiltRight == other.tiltRight &&
               varianceScale == other.varianceScale &&
               smoothLeftFrames == other.smoothLeftFrames &&
               smoothRightFrames == other.smoothRightFrames &&
               pitchOffset == other.pitchOffset &&
               highPassFilterStrength == other.highPassFilterStrength &&
               lowPassFilterStrength == other.lowPassFilterStrength;
    }

    bool operator!=(const TransformParams& other) const
    {
        return !(*this == other);
    }

    bool isIdentity() const
    {
        return tiltLeft == 0.0f &&
               tiltRight == 0.0f &&
               varianceScale == 1.0f &&
               smoothLeftFrames == 0 &&
               smoothRightFrames == 0 &&
               highPassFilterStrength == 0.0f &&
               lowPassFilterStrength == 0.0f;
    }
};
