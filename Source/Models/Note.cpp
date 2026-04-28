#include "Note.h"
#include "../Utils/Constants.h"

Note::Note(int startFrame, int endFrame, float midiNote)
    : srcStartFrame(startFrame), srcEndFrame(endFrame),
      startFrame(startFrame), endFrame(endFrame), midiNote(midiNote)
{
}

std::vector<float> Note::computeF0FromDelta() const
{
    int numFrames = endFrame - startFrame;
    std::vector<float> result(numFrames, 0.0f);

    for (int i = 0; i < numFrames; ++i)
    {
        // Get delta pitch for this frame (or 0 if not available)
        float delta = (i < static_cast<int>(deltaPitch.size())) ? deltaPitch[i] : 0.0f;

        // Actual MIDI = base + offset + delta
        float actualMidi = midiNote + pitchOffset + delta;

        // Convert to Hz
        result[i] = midiToFreq(actualMidi);
    }

    return result;
}

bool Note::containsFrame(int frame) const
{
    return frame >= startFrame && frame < endFrame;
}
