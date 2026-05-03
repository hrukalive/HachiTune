#include "Project.h"
#include "../Audio/Synthesis/StretchProcessor.h"
#include "../Utils/CenteredMelSpectrogram.h"
#include "../Utils/Constants.h"
#include "../Utils/CurveResampler.h"
#include "../Utils/HNSepCurveProcessor.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/WarpMarkerProcessor.h"
#include <algorithm>
#include <cmath>

namespace
{
    constexpr float twoPi = 6.2831853071795864769f;

    int normalizeBeatNumerator(int numerator)
    {
        return juce::jlimit(1, 32, numerator);
    }

    int normalizeBeatDenominator(int denominator)
    {
        denominator = juce::jlimit(1, 32, denominator);
        int normalized = 1;
        while (normalized < denominator)
            normalized <<= 1;
        const int lower = normalized >> 1;
        if (lower >= 1 && (denominator - lower) < (normalized - denominator))
            normalized = lower;
        return juce::jlimit(1, 32, normalized);
    }

    TimelineGridDivision normalizeGridDivision(TimelineGridDivision division)
    {
        switch (division)
        {
        case TimelineGridDivision::Whole:
        case TimelineGridDivision::Half:
        case TimelineGridDivision::Quarter:
        case TimelineGridDivision::Eighth:
        case TimelineGridDivision::Sixteenth:
        case TimelineGridDivision::ThirtySecond:
            return division;
        default:
            return TimelineGridDivision::Quarter;
        }
    }
}

Project::Project()
{
}

int AudioData::getNumFrames() const
{
    if (waveform.getNumSamples() == 0)
        return 0;
    return waveform.getNumSamples() / HOP_SIZE + 1;
}

int Project::getFrameCount() const
{
    if (!editedData.f0.empty())
        return static_cast<int>(editedData.f0.size());
    if (!editedData.mel.empty())
        return static_cast<int>(editedData.mel.size());
    if (!editedData.tunedF0.empty())
        return static_cast<int>(editedData.tunedF0.size());
    if (!analysisData.originalMel.empty())
        return static_cast<int>(analysisData.originalMel.size());
    return audioData.getNumFrames();
}

float Project::getBaseF0ForFrame(int frame) const
{
    if (frame < 0 || frame >= static_cast<int>(editedData.basePitch.size()))
        return 0.0f;
    return midiToFreq(editedData.basePitch[static_cast<size_t>(frame)]);
}

Project::FrameDataValidation Project::validateFrameData() const
{
    FrameDataValidation result;
    int outputFrames = 0;
    auto useOutputFrameCount = [&](size_t size) {
        if (outputFrames == 0 && size > 0)
            outputFrames = static_cast<int>(size);
    };

    useOutputFrameCount(editedData.f0.size());
    useOutputFrameCount(editedData.mel.size());
    useOutputFrameCount(editedData.basePitch.size());
    useOutputFrameCount(editedData.deltaPitch.size());
    useOutputFrameCount(editedData.voicedMask.size());
    useOutputFrameCount(editedData.vadMask.size());
    useOutputFrameCount(editedData.voicingCurve.size());
    useOutputFrameCount(editedData.breathCurve.size());
    useOutputFrameCount(editedData.tensionCurve.size());

    auto checkFloat = [&](const std::vector<float>& values,
                          const char* name,
                          bool required,
                          int expectedFrames) {
        if (required && values.empty())
            result.messages.push_back(juce::String(name) + " is empty");
        if (!values.empty() && expectedFrames > 0 &&
            static_cast<int>(values.size()) != expectedFrames)
            result.messages.push_back(juce::String(name) + " size mismatch");
    };

    auto checkBool = [&](const std::vector<bool>& values,
                         const char* name,
                         bool required,
                         int expectedFrames) {
        if (required && values.empty())
            result.messages.push_back(juce::String(name) + " is empty");
        if (!values.empty() && expectedFrames > 0 &&
            static_cast<int>(values.size()) != expectedFrames)
            result.messages.push_back(juce::String(name) + " size mismatch");
    };

    if (outputFrames > 0)
    {
        checkFloat(editedData.basePitch, "editedData.basePitch", true, outputFrames);
        checkFloat(editedData.deltaPitch, "editedData.deltaPitch", true, outputFrames);
        checkFloat(editedData.f0, "editedData.f0", true, outputFrames);
        checkBool(editedData.voicedMask, "editedData.voicedMask", true, outputFrames);
        checkBool(editedData.vadMask, "editedData.vadMask", true, outputFrames);
        checkFloat(editedData.voicingCurve, "editedData.voicingCurve", true, outputFrames);
        checkFloat(editedData.breathCurve, "editedData.breathCurve", true, outputFrames);
        checkFloat(editedData.tensionCurve, "editedData.tensionCurve", true, outputFrames);
    }

    const int analysisFrames = analysisData.getNumFrames();
    int sourceFrames = analysisFrames;
    auto useSourceFrameCount = [&](size_t size) {
        if (sourceFrames == 0 && size > 0)
            sourceFrames = static_cast<int>(size);
    };
    useSourceFrameCount(analysisData.originalMel.size());
    useSourceFrameCount(editedData.tunedF0.size());
    useSourceFrameCount(editedData.baseVoicing.size());
    useSourceFrameCount(editedData.baseBreath.size());
    useSourceFrameCount(editedData.baseTension.size());
    useSourceFrameCount(editedData.adjustedMel.size());

    auto checkAnalysisFloat = [&](const std::vector<float>& values,
                                  const char* name,
                                  bool required) {
        if (required && values.empty())
            result.messages.push_back(juce::String(name) + " is empty");
        if (!values.empty() && static_cast<int>(values.size()) != analysisFrames)
            result.messages.push_back(juce::String(name) + " size mismatch");
    };
    auto checkAnalysisBool = [&](const std::vector<bool>& values,
                                 const char* name,
                                 bool required) {
        if (required && values.empty())
            result.messages.push_back(juce::String(name) + " is empty");
        if (!values.empty() && static_cast<int>(values.size()) != analysisFrames)
            result.messages.push_back(juce::String(name) + " size mismatch");
    };

    checkAnalysisFloat(analysisData.originalF0, "analysisData.originalF0", false);
    const bool requiresAnalysisArrays = analysisFrames > 0;
    checkAnalysisFloat(analysisData.originalPitch,
                       "analysisData.originalPitch",
                       requiresAnalysisArrays);
    checkAnalysisFloat(analysisData.originalDeltaPitch,
                       "analysisData.originalDeltaPitch",
                       requiresAnalysisArrays);
    checkAnalysisBool(analysisData.originalVoicedMask,
                      "analysisData.originalVoicedMask",
                      requiresAnalysisArrays);
    checkAnalysisBool(analysisData.originalVADMask,
                       "analysisData.originalVADMask",
                       requiresAnalysisArrays);

    if (sourceFrames > 0)
    {
        checkFloat(editedData.tunedF0, "editedData.tunedF0", false, sourceFrames);
        checkFloat(editedData.baseVoicing, "editedData.baseVoicing", false, sourceFrames);
        checkFloat(editedData.baseBreath, "editedData.baseBreath", false, sourceFrames);
        checkFloat(editedData.baseTension, "editedData.baseTension", false, sourceFrames);
    }

    const int projectFrames = getFrameCount();
    int nonRestNotes = 0;
    for (const auto& note : notes)
    {
        if (!note.isRest())
            ++nonRestNotes;

        if (note.getStartFrame() < 0 ||
            note.getEndFrame() <= note.getStartFrame() ||
            (projectFrames > 0 && note.getEndFrame() > projectFrames))
            result.messages.push_back("invalid note output range");

        if (!note.isRest())
        {
            const bool invalidSourceRange =
                note.getSrcStartFrame() < 0 ||
                note.getSrcEndFrame() <= note.getSrcStartFrame();
            const bool exceedsAnalysisRange =
                analysisFrames > 0 && note.getSrcEndFrame() > analysisFrames;
            if (invalidSourceRange || exceedsAnalysisRange)
                result.messages.push_back("invalid note source range");
        }
    }

    if (((analysisFrames > 0 && nonRestNotes > 0) ||
         !analysisData.noteSegments.empty()) &&
        static_cast<int>(analysisData.noteSegments.size()) != nonRestNotes)
        result.messages.push_back("analysisData.noteSegments count mismatch");

    auto checkMelMatrix = [&](const std::vector<std::vector<float>>& matrix,
                              const char* name,
                              int expectedFrames) {
        if (matrix.empty())
            return;
        if (expectedFrames > 0 &&
            static_cast<int>(matrix.size()) != expectedFrames)
            result.messages.push_back(juce::String(name) + " size mismatch");
        const auto bins = matrix.front().size();
        for (const auto& row : matrix)
        {
            if (row.size() != bins)
            {
                result.messages.push_back(juce::String(name) + " ragged rows");
                break;
            }
        }
    };

    checkMelMatrix(analysisData.originalMel, "analysisData.originalMel",
                   analysisFrames);
    checkMelMatrix(editedData.adjustedMel, "editedData.adjustedMel",
                   sourceFrames);
    checkMelMatrix(editedData.mel, "editedData.mel", outputFrames);

    int prevSource = -1;
    int prevOutput = -1;
    for (const auto& marker : warpMarkers)
    {
        if (marker.sourceFrame <= prevSource ||
            marker.outputFrame <= prevOutput)
            result.messages.push_back("warpMarkers are not strictly increasing");
        prevSource = marker.sourceFrame;
        prevOutput = marker.outputFrame;
    }

    return result;
}

void Project::addNote(Note note)
{
  auto it = std::lower_bound(notes.begin(), notes.end(), note,
      [](const Note& a, const Note& b)
      {
        if (a.getStartFrame() != b.getStartFrame())
          return a.getStartFrame() < b.getStartFrame();
        return a.getEndFrame() < b.getEndFrame();
      });
  notes.insert(it, std::move(note));
}

void Project::sortNotes()
{
  std::sort(notes.begin(), notes.end(),
      [](const Note& a, const Note& b)
      {
        if (a.getStartFrame() != b.getStartFrame())
          return a.getStartFrame() < b.getStartFrame();
        return a.getEndFrame() < b.getEndFrame();
      });
}

Note *Project::getNoteAtFrame(int frame)
{
  // Binary search for the first note whose startFrame > frame
  auto it = std::upper_bound(notes.begin(), notes.end(), frame,
      [](int f, const Note& note) { return f < note.getStartFrame(); });

  // Walk backward to check notes that could contain this frame
  while (it != notes.begin())
  {
    --it;
    if (it->containsFrame(frame))
      return &(*it);
    // Once startFrame is too far left, no earlier note can contain frame
    if (it->getEndFrame() <= frame)
      break;
  }
  return nullptr;
}

std::vector<Note *> Project::getNotesInRange(int startFrame, int endFrame)
{
  std::vector<Note *> result;

  // Find first note that could overlap: need note.endFrame > startFrame.
  // Since notes are sorted by startFrame, a note can overlap even if its
  // startFrame < startFrame (it extends past). Walk back from the first
  // note with startFrame >= startFrame.
  auto it = std::lower_bound(notes.begin(), notes.end(), startFrame,
      [](const Note& note, int f) { return note.getStartFrame() < f; });

  // Walk backward to catch notes that start before startFrame but extend into the range
  while (it != notes.begin())
  {
    --it;
    if (it->getEndFrame() <= startFrame)
    {
      ++it;
      break;
    }
  }

  // Scan forward collecting overlapping notes
  for (; it != notes.end(); ++it)
  {
    if (it->getStartFrame() >= endFrame)
      break;
    if (it->getEndFrame() > startFrame)
      result.push_back(&(*it));
  }

  return result;
}

std::vector<Note *> Project::getSelectedNotes()
{
    std::vector<Note *> result;
    for (auto &note : notes)
    {
        if (note.isSelected())
            result.push_back(&note);
    }
    return result;
}

bool Project::removeNoteByStartFrame(int startFrame)
{
    for (auto it = notes.begin(); it != notes.end(); ++it)
    {
        if (it->getStartFrame() == startFrame)
        {
            notes.erase(it);
            return true;
        }
    }
    return false;
}

void Project::deselectAllNotes()
{
    for (auto &note : notes)
        note.setSelected(false);
    notifyListeners(ProjectChangeType::NoteSelectionChanged);
}

void Project::selectAllNotes(bool includeRests)
{
    for (auto &note : notes)
    {
        if (!includeRests && note.isRest())
            continue;
        note.setSelected(true);
    }
    notifyListeners(ProjectChangeType::NoteSelectionChanged);
}

std::vector<Note *> Project::getDirtyNotes()
{
    std::vector<Note *> result;
    for (auto &note : notes)
    {
        if (note.isDirty())
            result.push_back(&note);
    }
    return result;
}

int Project::getNoteIndex(const Note* note) const
{
  if (!note)
    return -1;
  for (int i = 0; i < static_cast<int>(notes.size()); ++i)
  {
    if (&notes[i] == note)
      return i;
  }
  return -1;
}

void Project::clearAllDirty()
{
    for (auto &note : notes)
        note.clearDirty();
    // Also clear F0 dirty range
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
    ++f0DirtyGeneration;
    // Also clear parameter dirty range
    paramDirtyStart = -1;
    paramDirtyEnd = -1;
    ++paramDirtyGeneration;
}

void Project::clearSynthesisDirtyForRange(int startFrame, int endFrame)
{
    if (endFrame <= startFrame)
        return;

    for (auto &note : notes)
    {
        if (note.getEndFrame() <= startFrame ||
            note.getStartFrame() >= endFrame)
            continue;
        note.setSynthDirty(false);
    }
}

Project::DirtyStateSnapshot
Project::captureDirtyStateSnapshotForRange(int startFrame, int endFrame) const
{
    DirtyStateSnapshot snapshot;
    snapshot.f0DirtyStart = f0DirtyStart;
    snapshot.f0DirtyEnd = f0DirtyEnd;
    snapshot.f0DirtyGeneration = f0DirtyGeneration;
    snapshot.hadF0DirtyRange = hasF0DirtyRange() &&
                               f0DirtyEnd > startFrame &&
                               f0DirtyStart < endFrame;
    snapshot.paramDirtyStart = paramDirtyStart;
    snapshot.paramDirtyEnd = paramDirtyEnd;
    snapshot.paramDirtyGeneration = paramDirtyGeneration;
    snapshot.hadParamDirtyRange = hasParamDirtyRange() &&
                                  paramDirtyEnd > startFrame &&
                                  paramDirtyStart < endFrame;

    if (endFrame <= startFrame)
        return snapshot;

    for (int i = 0; i < static_cast<int>(notes.size()); ++i)
    {
        const auto& note = notes[static_cast<size_t>(i)];
        if (note.getEndFrame() <= startFrame ||
            note.getStartFrame() >= endFrame)
            continue;
        if (!note.isDirty() && !note.isSynthDirty())
            continue;

        DirtyStateSnapshot::NoteState noteState;
        noteState.noteIndex = i;
        noteState.startFrame = note.getStartFrame();
        noteState.endFrame = note.getEndFrame();
        noteState.srcStartFrame = note.getSrcStartFrame();
        noteState.srcEndFrame = note.getSrcEndFrame();
        noteState.dirtyGeneration = note.getDirtyGeneration();
        noteState.wasDirty = note.isDirty();
        noteState.wasSynthDirty = note.isSynthDirty();
        snapshot.notes.push_back(noteState);
    }

    return snapshot;
}

void Project::clearDirtyStateForCompletedSynthesis(
    const DirtyStateSnapshot& snapshot)
{
    for (const auto& noteState : snapshot.notes)
    {
        if (noteState.noteIndex < 0 ||
            noteState.noteIndex >= static_cast<int>(notes.size()))
            continue;

        auto& note = notes[static_cast<size_t>(noteState.noteIndex)];
        if (note.getStartFrame() != noteState.startFrame ||
            note.getEndFrame() != noteState.endFrame ||
            note.getSrcStartFrame() != noteState.srcStartFrame ||
            note.getSrcEndFrame() != noteState.srcEndFrame)
        {
            continue;
        }
        if (note.getDirtyGeneration() != noteState.dirtyGeneration)
            continue;

        if (noteState.wasSynthDirty && note.isSynthDirty())
            note.setSynthDirty(false);
        if (noteState.wasDirty && note.isDirty())
            note.clearDirty();
    }

    if (snapshot.hadF0DirtyRange &&
        f0DirtyGeneration == snapshot.f0DirtyGeneration &&
        f0DirtyStart == snapshot.f0DirtyStart &&
        f0DirtyEnd == snapshot.f0DirtyEnd)
    {
        clearF0DirtyRange();
    }

    if (snapshot.hadParamDirtyRange &&
        paramDirtyGeneration == snapshot.paramDirtyGeneration &&
        paramDirtyStart == snapshot.paramDirtyStart &&
        paramDirtyEnd == snapshot.paramDirtyEnd)
    {
        clearParamDirtyRange();
    }
}

bool Project::hasDirtyNotes() const
{
    for (const auto &note : notes)
    {
        if (note.isDirty())
            return true;
    }
    return false;
}

void Project::setF0DirtyRange(int startFrame, int endFrame)
{
    if (f0DirtyStart < 0 || startFrame < f0DirtyStart)
        f0DirtyStart = startFrame;
    if (f0DirtyEnd < 0 || endFrame > f0DirtyEnd)
        f0DirtyEnd = endFrame;
    ++f0DirtyGeneration;
}

void Project::clearF0DirtyRange()
{
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
    ++f0DirtyGeneration;
}

bool Project::hasF0DirtyRange() const
{
    return f0DirtyStart >= 0 && f0DirtyEnd >= 0;
}

std::pair<int, int> Project::getF0DirtyRange() const
{
    return {f0DirtyStart, f0DirtyEnd};
}

void Project::setParamDirtyRange(int startFrame, int endFrame)
{
    if (paramDirtyStart < 0 || startFrame < paramDirtyStart)
        paramDirtyStart = startFrame;
    if (paramDirtyEnd < 0 || endFrame > paramDirtyEnd)
        paramDirtyEnd = endFrame;
    ++paramDirtyGeneration;
}

void Project::clearParamDirtyRange()
{
    paramDirtyStart = -1;
    paramDirtyEnd = -1;
    ++paramDirtyGeneration;
}

bool Project::hasParamDirtyRange() const
{
    return paramDirtyStart >= 0 && paramDirtyEnd >= 0;
}

std::pair<int, int> Project::getParamDirtyRange() const
{
    return {paramDirtyStart, paramDirtyEnd};
}

std::pair<int, int> Project::getDirtyFrameRange() const
{
    int minStart = -1;
    int maxEnd = -1;

    // Check dirty notes
    for (const auto &note : notes)
    {
        if (note.isDirty())
        {
            if (minStart < 0 || note.getStartFrame() < minStart)
                minStart = note.getStartFrame();
            if (maxEnd < 0 || note.getEndFrame() > maxEnd)
                maxEnd = note.getEndFrame();
        }
    }

    // Also include F0 dirty range from Draw mode edits
    if (f0DirtyStart >= 0)
    {
        if (minStart < 0 || f0DirtyStart < minStart)
            minStart = f0DirtyStart;
    }
    if (f0DirtyEnd >= 0)
    {
        if (maxEnd < 0 || f0DirtyEnd > maxEnd)
            maxEnd = f0DirtyEnd;
    }

    // Also include parameter curve dirty range
    if (paramDirtyStart >= 0)
    {
        if (minStart < 0 || paramDirtyStart < minStart)
            minStart = paramDirtyStart;
    }
    if (paramDirtyEnd >= 0)
    {
        if (maxEnd < 0 || paramDirtyEnd > maxEnd)
            maxEnd = paramDirtyEnd;
    }

    return {minStart, maxEnd};
}

std::vector<float> Project::getAdjustedF0() const
{
    if (editedData.basePitch.empty() || editedData.deltaPitch.empty())
        return {};

    // Compose base + delta as dense curve; UV blending is handled downstream
    // by synthesis masks, so we do not zero F0 here.
    std::vector<float> adjustedF0 = PitchCurveProcessor::composeF0(*this,
                                                                   /*applyUvMask=*/false,
                                                                   globalPitchOffset);

    // Apply vibrato per note on top of composed curve
    for (const auto &note : notes)
    {
        const bool hasVibrato = note.isVibratoEnabled() &&
                                note.getVibratoLengthFrames() > 0 &&
                                note.getVibratoDepthSemitones() > 0.0001f &&
                                note.getVibratoRateHz() > 0.0001f;
        if (!hasVibrato)
            continue;

        const int noteStart = std::max(0, note.getStartFrame());
        const int noteEnd = std::min(note.getEndFrame(), static_cast<int>(adjustedF0.size()));
        const int vibAbsStart = noteStart + note.getVibratoStartFrame();
        const int vibAbsEnd = vibAbsStart + note.getVibratoLengthFrames();
        const int fadeIn = note.getVibratoFadeInFrames();
        const int fadeOut = note.getVibratoFadeOutFrames();
        const int vibLength = note.getVibratoLengthFrames();
        const float mix = note.getVibratoMix();
        const float depth = note.getVibratoDepthSemitones();
        const float rate = note.getVibratoRateHz();
        const float phase = note.getVibratoPhaseRadians();

        for (int i = noteStart; i < noteEnd; ++i)
        {
            if (i < vibAbsStart || i >= vibAbsEnd)
                continue;

            if (i < static_cast<int>(editedData.voicedMask.size()) && !editedData.voicedMask[i])
                continue;

            const int localT = i - vibAbsStart;

            // Compute fade envelope
            float envelope = 1.0f;
            if (localT < fadeIn && fadeIn > 0)
                envelope = static_cast<float>(localT) / static_cast<float>(fadeIn);
            if (localT >= (vibLength - fadeOut) && fadeOut > 0)
                envelope = static_cast<float>(vibLength - 1 - localT) / static_cast<float>(fadeOut);
            envelope = juce::jlimit(0.0f, 1.0f, envelope);

            // Compute vibrato signal
            float vibratoSemitones = depth *
                std::sin(twoPi * rate * framesToSeconds(localT) + phase) * envelope;

            // Apply mix
            float finalVibrato = vibratoSemitones * mix;
            adjustedF0[static_cast<size_t>(i)] *= std::pow(2.0f, finalVibrato / 12.0f);
        }
    }

    return adjustedF0;
}

std::vector<float> Project::getAdjustedF0ForRange(int startFrame, int endFrame) const
{
    if (editedData.basePitch.empty() || editedData.deltaPitch.empty())
        return {};

    // Clamp range
    startFrame = std::max(0, startFrame);
    endFrame = std::min(endFrame, static_cast<int>(editedData.basePitch.size()));

    if (startFrame >= endFrame)
        return {};

    const int rangeSize = endFrame - startFrame;
    std::vector<float> adjustedF0(static_cast<size_t>(rangeSize), 0.0f);

    for (int i = 0; i < rangeSize; ++i)
    {
        const int globalIdx = startFrame + i;
        const float base = editedData.basePitch[static_cast<size_t>(globalIdx)];
        const float delta = (globalIdx < static_cast<int>(editedData.deltaPitch.size()))
                                ? editedData.deltaPitch[static_cast<size_t>(globalIdx)]
                                : 0.0f;
        float midi = base + delta + globalPitchOffset;
        adjustedF0[static_cast<size_t>(i)] = midiToFreq(midi);
    }

    // Apply vibrato for overlapping notes
    for (const auto &note : notes)
    {
        const bool hasVibrato = note.isVibratoEnabled() &&
                                note.getVibratoLengthFrames() > 0 &&
                                note.getVibratoDepthSemitones() > 0.0001f &&
                                note.getVibratoRateHz() > 0.0001f;
        if (!hasVibrato)
            continue;

        const int noteStart = note.getStartFrame();
        const int vibAbsStart = noteStart + note.getVibratoStartFrame();
        const int vibAbsEnd = vibAbsStart + note.getVibratoLengthFrames();
        const int fadeIn = note.getVibratoFadeInFrames();
        const int fadeOut = note.getVibratoFadeOutFrames();
        const int vibLength = note.getVibratoLengthFrames();
        const float mix = note.getVibratoMix();
        const float depth = note.getVibratoDepthSemitones();
        const float rate = note.getVibratoRateHz();
        const float phase = note.getVibratoPhaseRadians();

        const int overlapStart = std::max({vibAbsStart, startFrame});
        const int overlapEnd = std::min({vibAbsEnd, endFrame, note.getEndFrame()});
        for (int frame = overlapStart; frame < overlapEnd; ++frame)
        {
            const int localIdx = frame - startFrame;
            if (frame < static_cast<int>(editedData.voicedMask.size()) && !editedData.voicedMask[frame])
                continue;

            const int localT = frame - vibAbsStart;

            // Compute fade envelope
            float envelope = 1.0f;
            if (localT < fadeIn && fadeIn > 0)
                envelope = static_cast<float>(localT) / static_cast<float>(fadeIn);
            if (localT >= (vibLength - fadeOut) && fadeOut > 0)
                envelope = static_cast<float>(vibLength - 1 - localT) / static_cast<float>(fadeOut);
            envelope = juce::jlimit(0.0f, 1.0f, envelope);

            // Compute vibrato signal
            float vibratoSemitones = depth *
                std::sin(twoPi * rate * framesToSeconds(localT) + phase) * envelope;

            // Apply mix
            float finalVibrato = vibratoSemitones * mix;
            adjustedF0[static_cast<size_t>(localIdx)] *= std::pow(2.0f, finalVibrato / 12.0f);
        }
    }

    return adjustedF0;
}

void Project::setLoopRange(double startSeconds, double endSeconds)
{
    if (startSeconds > endSeconds)
        std::swap(startSeconds, endSeconds);

    const double duration = audioData.getDuration();
    if (duration > 0.0)
    {
        startSeconds = juce::jlimit(0.0, duration, startSeconds);
        endSeconds = juce::jlimit(0.0, duration, endSeconds);
    }

    loopRange.startSeconds = startSeconds;
    loopRange.endSeconds = endSeconds;
    loopRange.enabled = loopRange.endSeconds > loopRange.startSeconds;
    notifyListeners(ProjectChangeType::SettingsChanged);
}

void Project::setLoopEnabled(bool enabled)
{
    if (enabled && loopRange.endSeconds <= loopRange.startSeconds)
        loopRange.enabled = false;
    else
        loopRange.enabled = enabled;
    notifyListeners(ProjectChangeType::SettingsChanged);
}

void Project::clearLoopRange()
{
    loopRange = {};
}

void Project::setScaleMode(ScaleMode mode)
{
    if (scaleMode == mode)
        return;

    scaleMode = mode;
    modified = true;
    notifyListeners(ProjectChangeType::SettingsChanged);
}

void Project::setScaleRootNote(int noteInOctave)
{
    const int normalized = juce::jlimit(-1, 11, noteInOctave);
    if (scaleRootNote == normalized)
        return;

    scaleRootNote = normalized;
    modified = true;
    notifyListeners(ProjectChangeType::SettingsChanged);
}

void Project::setPitchReferenceHz(int hz)
{
    const int normalized = juce::jlimit(380, 480, hz);
    if (pitchReferenceHz == normalized)
        return;

    pitchReferenceHz = normalized;
    modified = true;
}

void Project::setShowScaleColors(bool enabled)
{
    if (showScaleColors == enabled)
        return;

    showScaleColors = enabled;
    modified = true;
}

void Project::setSnapToSemitones(bool enabled)
{
    if (snapToSemitones == enabled)
        return;

    snapToSemitones = enabled;
    modified = true;
}

void Project::setDoubleClickSnapMode(DoubleClickSnapMode mode)
{
    if (doubleClickSnapMode == mode)
        return;

    doubleClickSnapMode = mode;
    modified = true;
}

void Project::setTimelineDisplayMode(TimelineDisplayMode mode)
{
    if (timelineDisplayMode == mode)
        return;

    timelineDisplayMode = mode;
    modified = true;
    notifyListeners(ProjectChangeType::SettingsChanged);
}

void Project::setTimelineBeatSignature(int numerator, int denominator)
{
    const int normalizedNumerator = normalizeBeatNumerator(numerator);
    const int normalizedDenominator = normalizeBeatDenominator(denominator);

    if (timelineBeatNumerator == normalizedNumerator &&
        timelineBeatDenominator == normalizedDenominator)
        return;

    timelineBeatNumerator = normalizedNumerator;
    timelineBeatDenominator = normalizedDenominator;
    modified = true;
    notifyListeners(ProjectChangeType::SettingsChanged);
}

void Project::setTimelineTempoBpm(double bpm)
{
    const double normalized = juce::jlimit(20.0, 300.0, bpm);
    if (std::abs(timelineTempoBpm - normalized) < 1.0e-6)
        return;

    timelineTempoBpm = normalized;
    modified = true;
    notifyListeners(ProjectChangeType::SettingsChanged);
}

void Project::setTimelineGridDivision(TimelineGridDivision division)
{
    const auto normalized = normalizeGridDivision(division);
    if (timelineGridDivision == normalized)
        return;

    timelineGridDivision = normalized;
    modified = true;
    notifyListeners(ProjectChangeType::SettingsChanged);
}

void Project::setTimelineSnapCycle(bool enabled)
{
    if (timelineSnapCycle == enabled)
        return;

    timelineSnapCycle = enabled;
    modified = true;
    notifyListeners(ProjectChangeType::SettingsChanged);
}

// ---------------------------------------------------------------------------
// composeGlobalWaveform: rebuild audioData.waveform from originalWaveform +
// per-note synthWaveforms, mapping each segment (gap/note) from its source
// position in originalWaveform to its output position in the timeline.
//
// This ensures that non-note regions (breaths, consonants, silence) shift
// along with notes during ripple stretch, and the buffer grows as needed.
// ---------------------------------------------------------------------------
std::vector<float> Project::renderMappedBaseWaveformSegment(int startSample,
                                                            int numSamples) const
{
    if (numSamples <= 0)
        return {};

    std::vector<float> segment(static_cast<size_t>(numSamples), 0.0f);
    const auto &origWaveform =
        audioData.originalWaveform.getNumSamples() > 0
            ? audioData.originalWaveform
            : audioData.waveform;
    const int origSamples = origWaveform.getNumSamples();
    if (origWaveform.getNumChannels() == 0 || origSamples <= 0)
        return segment;

    const float *src = origWaveform.getReadPointer(0);
    const int segmentEnd = startSample + numSamples;

    std::vector<const Note *> sortedNotes;
    sortedNotes.reserve(notes.size());
    for (const auto &note : notes)
    {
        if (!note.isRest())
            sortedNotes.push_back(&note);
    }

    auto writeFromOrig = [&](int srcOff, int dstOff, int len)
    {
        if (len <= 0)
            return;

        int dstStart = std::max(dstOff, startSample);
        int dstEnd = std::min(dstOff + len, segmentEnd);
        if (dstEnd <= dstStart)
            return;

        int srcStart = srcOff + (dstStart - dstOff);
        if (srcStart < 0)
        {
            dstStart -= srcStart;
            srcStart = 0;
        }

        int copyLen = dstEnd - dstStart;
        copyLen = std::min(copyLen, origSamples - srcStart);
        if (copyLen <= 0)
            return;

        std::copy(src + srcStart, src + srcStart + copyLen,
                  segment.begin() + (dstStart - startSample));
    };

    auto stretchFromOrig = [&](int srcOff, int dstOff, int srcLen, int dstLen)
    {
        if (srcLen <= 0 || dstLen <= 0)
            return;

        if (srcOff < 0)
        {
            srcLen += srcOff;
            srcOff = 0;
        }
        srcLen = std::min(srcLen, origSamples - srcOff);
        if (srcLen <= 0)
            return;

        const int dstStart = std::max(dstOff, startSample);
        const int dstEnd = std::min(dstOff + dstLen, segmentEnd);
        if (dstEnd <= dstStart)
            return;

        if (srcLen == dstLen)
        {
            const int srcStart = srcOff + (dstStart - dstOff);
            const int copyLen =
                std::min(dstEnd - dstStart, origSamples - srcStart);
            if (copyLen > 0)
            {
                std::copy(src + srcStart, src + srcStart + copyLen,
                          segment.begin() + (dstStart - startSample));
            }
            return;
        }

        const double ratio =
            (srcLen <= 1 || dstLen <= 1)
                ? 0.0
                : static_cast<double>(srcLen - 1) /
                      static_cast<double>(dstLen - 1);
        for (int globalDst = dstStart; globalDst < dstEnd; ++globalDst)
        {
            const int localDst = globalDst - dstOff;
            const double srcPos = static_cast<double>(localDst) * ratio;
            const int idx = static_cast<int>(srcPos);
            const float frac = static_cast<float>(srcPos - idx);
            const int s0 = srcOff + std::min(idx, srcLen - 1);
            const int s1 = srcOff + std::min(idx + 1, srcLen - 1);
            segment[static_cast<size_t>(globalDst - startSample)] =
                src[s0] + frac * (src[s1] - src[s0]);
        }
    };

    if (sortedNotes.empty())
    {
        writeFromOrig(0, 0, origSamples);
        return segment;
    }

    {
        const int srcLen = sortedNotes.front()->getSrcStartFrame() * HOP_SIZE;
        const int dstLen = sortedNotes.front()->getStartFrame() * HOP_SIZE;
        stretchFromOrig(0, 0, srcLen, dstLen);
    }

    for (size_t i = 0; i < sortedNotes.size(); ++i)
    {
        const auto *note = sortedNotes[i];
        const int srcStart = note->getSrcStartFrame() * HOP_SIZE;
        const int srcLen =
            (note->getSrcEndFrame() - note->getSrcStartFrame()) * HOP_SIZE;
        const int dstStart = note->getStartFrame() * HOP_SIZE;
        const int dstLen =
            (note->getEndFrame() - note->getStartFrame()) * HOP_SIZE;
        stretchFromOrig(srcStart, dstStart, srcLen, dstLen);

        const int gapSrcStart = note->getSrcEndFrame() * HOP_SIZE;
        const int gapDstStart = note->getEndFrame() * HOP_SIZE;
        int gapSrcEnd = origSamples;
        int gapDstEnd = gapDstStart;
        if (i + 1 < sortedNotes.size())
        {
            gapSrcEnd = sortedNotes[i + 1]->getSrcStartFrame() * HOP_SIZE;
            gapDstEnd = sortedNotes[i + 1]->getStartFrame() * HOP_SIZE;
        }
        else
        {
            gapDstEnd = gapDstStart + std::max(0, gapSrcEnd - gapSrcStart);
        }

        const int gapSrcLen = gapSrcEnd - gapSrcStart;
        const int gapDstLen = gapDstEnd - gapDstStart;
        if (gapSrcLen > 0 && gapDstLen > 0)
            stretchFromOrig(gapSrcStart, gapDstStart, gapSrcLen, gapDstLen);
    }

    return segment;
}

// ---------------------------------------------------------------------------
// renderMappedSourceSegment – identical to renderMappedBaseWaveformSegment but
// operates on an arbitrary source buffer instead of audioData.originalWaveform.
// Used to time-stretch HNSep harmonic/noise buffers into the output timeline.
// ---------------------------------------------------------------------------
std::vector<float> Project::renderMappedSourceSegment(const float *sourceBuffer,
                                                       int sourceNumSamples,
                                                       int startSample,
                                                       int numSamples) const
{
    if (numSamples <= 0)
        return {};

    std::vector<float> segment(static_cast<size_t>(numSamples), 0.0f);
    if (sourceBuffer == nullptr || sourceNumSamples <= 0)
        return segment;

    const float *src = sourceBuffer;
    const int origSamples = sourceNumSamples;
    const int segmentEnd = startSample + numSamples;

    std::vector<const Note *> sortedNotes;
    sortedNotes.reserve(notes.size());
    for (const auto &note : notes)
    {
        if (!note.isRest())
            sortedNotes.push_back(&note);
    }

    auto writeFromSrc = [&](int srcOff, int dstOff, int len)
    {
        if (len <= 0)
            return;

        int dstStart = std::max(dstOff, startSample);
        int dstEnd = std::min(dstOff + len, segmentEnd);
        if (dstEnd <= dstStart)
            return;

        int srcStart = srcOff + (dstStart - dstOff);
        if (srcStart < 0)
        {
            dstStart -= srcStart;
            srcStart = 0;
        }

        int copyLen = dstEnd - dstStart;
        copyLen = std::min(copyLen, origSamples - srcStart);
        if (copyLen <= 0)
            return;

        std::copy(src + srcStart, src + srcStart + copyLen,
                  segment.begin() + (dstStart - startSample));
    };

    auto stretchFromSrc = [&](int srcOff, int dstOff, int srcLen, int dstLen)
    {
        if (srcLen <= 0 || dstLen <= 0)
            return;

        if (srcOff < 0)
        {
            srcLen += srcOff;
            srcOff = 0;
        }
        srcLen = std::min(srcLen, origSamples - srcOff);
        if (srcLen <= 0)
            return;

        const int dstStart = std::max(dstOff, startSample);
        const int dstEnd = std::min(dstOff + dstLen, segmentEnd);
        if (dstEnd <= dstStart)
            return;

        if (srcLen == dstLen)
        {
            const int srcStart = srcOff + (dstStart - dstOff);
            const int copyLen =
                std::min(dstEnd - dstStart, origSamples - srcStart);
            if (copyLen > 0)
            {
                std::copy(src + srcStart, src + srcStart + copyLen,
                          segment.begin() + (dstStart - startSample));
            }
            return;
        }

        const double ratio =
            (srcLen <= 1 || dstLen <= 1)
                ? 0.0
                : static_cast<double>(srcLen - 1) /
                      static_cast<double>(dstLen - 1);
        for (int globalDst = dstStart; globalDst < dstEnd; ++globalDst)
        {
            const int localDst = globalDst - dstOff;
            const double srcPos = static_cast<double>(localDst) * ratio;
            const int idx = static_cast<int>(srcPos);
            const float frac = static_cast<float>(srcPos - idx);
            const int s0 = srcOff + std::min(idx, srcLen - 1);
            const int s1 = srcOff + std::min(idx + 1, srcLen - 1);
            segment[static_cast<size_t>(globalDst - startSample)] =
                src[s0] + frac * (src[s1] - src[s0]);
        }
    };

    if (sortedNotes.empty())
    {
        writeFromSrc(0, 0, origSamples);
        return segment;
    }

    {
        const int srcLen = sortedNotes.front()->getSrcStartFrame() * HOP_SIZE;
        const int dstLen = sortedNotes.front()->getStartFrame() * HOP_SIZE;
        stretchFromSrc(0, 0, srcLen, dstLen);
    }

    for (size_t i = 0; i < sortedNotes.size(); ++i)
    {
        const auto *note = sortedNotes[i];
        const int srcStart = note->getSrcStartFrame() * HOP_SIZE;
        const int srcLen =
            (note->getSrcEndFrame() - note->getSrcStartFrame()) * HOP_SIZE;
        const int dstStart = note->getStartFrame() * HOP_SIZE;
        const int dstLen =
            (note->getEndFrame() - note->getStartFrame()) * HOP_SIZE;
        stretchFromSrc(srcStart, dstStart, srcLen, dstLen);

        const int gapSrcStart = note->getSrcEndFrame() * HOP_SIZE;
        const int gapDstStart = note->getEndFrame() * HOP_SIZE;
        int gapSrcEnd = origSamples;
        int gapDstEnd = gapDstStart;
        if (i + 1 < sortedNotes.size())
        {
            gapSrcEnd = sortedNotes[i + 1]->getSrcStartFrame() * HOP_SIZE;
            gapDstEnd = sortedNotes[i + 1]->getStartFrame() * HOP_SIZE;
        }
        else
        {
            gapDstEnd = gapDstStart + std::max(0, gapSrcEnd - gapSrcStart);
        }

        const int gapSrcLen = gapSrcEnd - gapSrcStart;
        const int gapDstLen = gapDstEnd - gapDstStart;
        if (gapSrcLen > 0 && gapDstLen > 0)
            stretchFromSrc(gapSrcStart, gapDstStart, gapSrcLen, gapDstLen);
    }

    return segment;
}

void Project::composeGlobalWaveform()
{
    auto &waveform = audioData.waveform;
    const auto &origWaveform = audioData.originalWaveform;

    const int numChannels = waveform.getNumChannels();
    const int origSamples = origWaveform.getNumSamples();
    if (numChannels == 0 || origSamples == 0)
        return;

    // --- Step 1: Collect non-rest notes sorted by output position ----------
    std::vector<const Note *> sortedNotes;
    sortedNotes.reserve(notes.size());
    for (const auto &note : notes)
    {
        if (!note.isRest())
            sortedNotes.push_back(&note);
    }

    // --- Step 2: Compute required output buffer length --------------------
    // Output follows the edited/warped timeline and can shrink as well as grow.
    const int outputFrames = getFrameCount();
    const int timelineSamples =
        outputFrames > 0 ? outputFrames * HOP_SIZE : origSamples;
    int requiredSamples = timelineSamples;
    if (!sortedNotes.empty())
    {
        const auto *last = sortedNotes.back();
        // Last note end (account for synthWaveform that may differ from frame range)
        int lastEnd = last->getEndFrame() * HOP_SIZE;
        if (last->hasSynthWaveform())
        {
            int synthEnd = last->getStartFrame() * HOP_SIZE + static_cast<int>(last->getSynthWaveform().size());
            lastEnd = std::max(lastEnd, synthEnd);
        }
        requiredSamples = std::max(requiredSamples, lastEnd);
    }

    if (requiredSamples <= 0)
        return;

    if (waveform.getNumSamples() != requiredSamples)
        waveform.setSize(numChannels, requiredSamples, false, true, false);
    const int totalSamples = waveform.getNumSamples();

    // --- Step 3: Zero the output ------------------------------------------
    waveform.clear();

    // Helper: copy from origWaveform[srcOff .. srcOff+len) to
    //         waveform[dstOff .. dstOff+len) with bounds clamping.
    auto copyFromOrig = [&](int srcOff, int dstOff, int len)
    {
        if (len <= 0)
            return;
        if (srcOff < 0)
        {
            len += srcOff;
            dstOff -= srcOff;
            srcOff = 0;
        }
        if (dstOff < 0)
        {
            len += dstOff;
            srcOff -= dstOff;
            dstOff = 0;
        }
        if (len <= 0)
            return;
        len = std::min(len, origSamples - srcOff);
        len = std::min(len, totalSamples - dstOff);
        if (len <= 0)
            return;
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float *src = origWaveform.getReadPointer(
                std::min(ch, std::max(0, origWaveform.getNumChannels() - 1)));
            float *dst = waveform.getWritePointer(ch);
            std::copy(src + srcOff, src + srcOff + len, dst + dstOff);
        }
    };

    // Helper: time-stretch (linear interpolation) from origWaveform[srcOff..srcOff+srcLen)
    //         into waveform[dstOff..dstOff+dstLen).  Handles srcLen != dstLen gracefully
    //         so that gaps/segments are never truncated with trailing zeros.
    auto stretchFromOrig = [&](int srcOff, int dstOff, int srcLen, int dstLen)
    {
        if (srcLen <= 0 || dstLen <= 0)
            return;
        // Clamp source range
        if (srcOff < 0)
        {
            srcLen += srcOff;
            srcOff = 0;
        }
        srcLen = std::min(srcLen, origSamples - srcOff);
        if (srcLen <= 0)
            return;
        // Clamp destination range
        if (dstOff < 0)
        {
            dstLen += dstOff;
            dstOff = 0;
        }
        dstLen = std::min(dstLen, totalSamples - dstOff);
        if (dstLen <= 0)
            return;

        // If lengths match, straight copy is fine (no interpolation overhead)
        if (srcLen == dstLen)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float *src = origWaveform.getReadPointer(
                    std::min(ch, std::max(0, origWaveform.getNumChannels() - 1)));
                float *dst = waveform.getWritePointer(ch);
                std::copy(src + srcOff, src + srcOff + srcLen, dst + dstOff);
            }
            return;
        }

        // Linear interpolation resample: map each dst sample to a fractional src position
        const double ratio = (srcLen <= 1) ? 0.0
                                           : static_cast<double>(srcLen - 1) / static_cast<double>(dstLen - 1);
        for (int ch = 0; ch < numChannels; ++ch)
        {
            const float *src = origWaveform.getReadPointer(
                std::min(ch, std::max(0, origWaveform.getNumChannels() - 1)));
            float *dst = waveform.getWritePointer(ch);
            for (int i = 0; i < dstLen; ++i)
            {
                const double srcPos = static_cast<double>(i) * ratio;
                const int idx = static_cast<int>(srcPos);
                const float frac = static_cast<float>(srcPos - idx);
                const int s0 = srcOff + std::min(idx, srcLen - 1);
                const int s1 = srcOff + std::min(idx + 1, srcLen - 1);
                dst[dstOff + i] = src[s0] + frac * (src[s1] - src[s0]);
            }
        }
    };

    // --- Step 4: Place segments via src→dst coordinate mapping -------------
    // Timeline = [leading gap][note0][gap01][note1]...[trailing gap]
    // Each segment is mapped from its source position (originalWaveform) to
    // its output position, so gaps shift together with notes.

    if (sortedNotes.empty())
    {
        if (timelineSamples > 0 && timelineSamples != origSamples)
            stretchFromOrig(0, 0, origSamples, timelineSamples);
        else
            copyFromOrig(0, 0, origSamples);
    }
    else
    {
        // Leading gap: orig[0..firstNote.srcStart) → out[0..firstNote.start)
        {
            int srcLen = sortedNotes[0]->getSrcStartFrame() * HOP_SIZE;
            int dstLen = sortedNotes[0]->getStartFrame() * HOP_SIZE;
            stretchFromOrig(0, 0, srcLen, dstLen);
        }

        for (size_t i = 0; i < sortedNotes.size(); ++i)
        {
            const auto *note = sortedNotes[i];

            // Always place original audio as base layer for every note region.
            // For notes with synthWaveform, this provides a smooth base that
            // the crossfade in Step 5 blends against at note boundaries,
            // avoiding the synth→silence→synth discontinuity.
            {
                int srcStart = note->getSrcStartFrame() * HOP_SIZE;
                int srcLen = (note->getSrcEndFrame() - note->getSrcStartFrame()) * HOP_SIZE;
                int dstStart = note->getStartFrame() * HOP_SIZE;
                int dstLen = (note->getEndFrame() - note->getStartFrame()) * HOP_SIZE;
                stretchFromOrig(srcStart, dstStart, srcLen, dstLen);
            }

            // Gap after this note → before next note (or trailing gap)
            int gapSrcStart = note->getSrcEndFrame() * HOP_SIZE;
            int gapDstStart = note->getEndFrame() * HOP_SIZE;
            int gapSrcEnd, gapDstEnd;
            if (i + 1 < sortedNotes.size())
            {
                gapSrcEnd = sortedNotes[i + 1]->getSrcStartFrame() * HOP_SIZE;
                gapDstEnd = sortedNotes[i + 1]->getStartFrame() * HOP_SIZE;
            }
            else
            {
                // Trailing gap follows the warped endpoint so waveform, mel,
                // and edited curves share the same output duration.
                gapSrcEnd = origSamples;
                gapDstEnd = timelineSamples;
            }
            int gapSrcLen = gapSrcEnd - gapSrcStart;
            int gapDstLen = gapDstEnd - gapDstStart;
            if (gapSrcLen > 0 && gapDstLen > 0)
                stretchFromOrig(gapSrcStart, gapDstStart,
                                gapSrcLen, gapDstLen);
        }
    }

    // --- Step 5: Overlay synthWaveforms with adaptive edge crossfade ------
    // Pre-compute adjacency: collect synth notes and check which edges face
    // another synth note vs. a gap.  Adjacent-synth edges use a much shorter
    // crossfade so we don't linger in the base (wrong-pitch) audio.
    //
    // Each synthWaveform may contain margin samples (preroll/postroll) beyond
    // the note body, allowing real-audio crossfade at boundaries.
    struct SynthNoteInfo
    {
        const Note *note;
        int bodyStartSample;             // noteStart * HOP_SIZE (where the body begins in global coords)
        int bodySamples;                 // note body length in samples
        int preroll;                     // margin samples before body in synthWaveform
        int synthTotalLen;               // total synthWaveform length
        bool leftAdjacentSynth = false;  // previous synth note is adjacent
        bool rightAdjacentSynth = false; // next synth note is adjacent
    };

    std::vector<SynthNoteInfo> synthInfos;
    synthInfos.reserve(sortedNotes.size());
    for (const auto *n : sortedNotes)
    {
        if (!n->hasSynthWaveform())
            continue;
        SynthNoteInfo si;
        si.note = n;
        si.bodyStartSample = n->getStartFrame() * HOP_SIZE;
        si.bodySamples = (n->getEndFrame() - n->getStartFrame()) * HOP_SIZE;
        si.preroll = n->getSynthPreroll();
        si.synthTotalLen = static_cast<int>(n->getSynthWaveform().size());
        if (si.synthTotalLen <= 0)
            continue;
        synthInfos.push_back(si);
    }

    // Detect adjacency: two synth notes are "adjacent" when the gap between
    // them is at most kAdjacencyThreshold samples.
    constexpr int kAdjacencyThreshold = HOP_SIZE; // 1 frame tolerance
    for (size_t si = 0; si + 1 < synthInfos.size(); ++si)
    {
        auto &curr = synthInfos[si];
        auto &next = synthInfos[si + 1];
        int currBodyEnd = curr.bodyStartSample + curr.bodySamples;
        int nextBodyStart = next.bodyStartSample;
        if (nextBodyStart - currBodyEnd <= kAdjacencyThreshold)
        {
            curr.rightAdjacentSynth = true;
            next.leftAdjacentSynth = true;
        }
    }

    constexpr int kGapFadeSamples = 512;   // ~11.6ms — crossfade to base at gaps
    constexpr int kSpliceFadeSamples = 64; // ~1.5ms  — minimal fade at synth-synth edges

    for (const auto &si : synthInfos)
    {
        const auto &synthWave = si.note->getSynthWaveform();
        const int preroll = si.preroll;
        // The synthWaveform global start (including preroll margin)
        const int synthGlobalStart = si.bodyStartSample - preroll;
        const int synthTotalLen = si.synthTotalLen;

        // Choose fade lengths per edge
        const int leftFadeReq = si.leftAdjacentSynth ? kSpliceFadeSamples : kGapFadeSamples;
        const int rightFadeReq = si.rightAdjacentSynth ? kSpliceFadeSamples : kGapFadeSamples;
        const int maxFade = std::max(1, si.bodySamples / 4);
        const int leftFade = std::min(leftFadeReq, maxFade);
        const int rightFade = std::min(rightFadeReq, maxFade);

        // We overlay only the body portion [preroll .. preroll+bodySamples)
        // The margin is reserved for Step 6 crossfade.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            float *dst = waveform.getWritePointer(ch);

            for (int i = 0; i < si.bodySamples; ++i)
            {
                const int globalIdx = si.bodyStartSample + i;
                if (globalIdx < 0 || globalIdx >= totalSamples)
                    continue;

                // Asymmetric edge envelope
                float env = 1.0f;
                if (i < leftFade && leftFade > 1)
                {
                    const float t = static_cast<float>(i) /
                                    static_cast<float>(leftFade);
                    env = t * t * (3.0f - 2.0f * t); // smoothstep
                }
                else if (i >= si.bodySamples - rightFade && rightFade > 1)
                {
                    const int fromEnd = si.bodySamples - 1 - i;
                    const float t = static_cast<float>(fromEnd) /
                                    static_cast<float>(rightFade);
                    env = t * t * (3.0f - 2.0f * t);
                }

                const int synthIdx = preroll + i;
                if (synthIdx < 0 || synthIdx >= si.synthTotalLen)
                    continue;

                const float base = dst[globalIdx];
                const float synth = synthWave[static_cast<size_t>(synthIdx)];
                dst[globalIdx] = base + env * (synth - base);
            }
        }
    }

    // --- Step 6: Direct synth-to-synth crossfade at adjacent boundaries ---
    // At adjacent synth note boundaries, both synthWaveforms now contain real
    // margin audio (preroll of next note, postroll of current note) from the
    // same continuous synthesis pass.  We crossfade these real signals instead
    // of using held-value extrapolation, eliminating the remaining pop.
    constexpr int kSpliceHalf = 64; // 64 samples each side = 128 total ≈ 2.9ms

    for (size_t si = 0; si + 1 < synthInfos.size(); ++si)
    {
        const auto &curr = synthInfos[si];
        const auto &next = synthInfos[si + 1];
        if (!curr.rightAdjacentSynth)
            continue; // not adjacent
        if (curr.note->getSynthPassId() == 0 ||
            curr.note->getSynthPassId() != next.note->getSynthPassId())
        {
            continue; // only splice margins rendered together in the same pass
        }

        const auto &sw1 = curr.note->getSynthWaveform();
        const auto &sw2 = next.note->getSynthWaveform();
        const int n1BodyStart = curr.bodyStartSample;
        const int n1BodyLen = curr.bodySamples;
        const int n1Preroll = curr.preroll;
        const int n1TotalLen = curr.synthTotalLen;
        const int n2BodyStart = next.bodyStartSample;
        const int n2Preroll = next.preroll;
        const int n2TotalLen = next.synthTotalLen;

        // Global start of each synth (including margins)
        const int n1GlobalStart = n1BodyStart - n1Preroll;
        const int n2GlobalStart = n2BodyStart - n2Preroll;

        // Junction: where note1 body ends
        const int junction = n1BodyStart + n1BodyLen;

        for (int ch = 0; ch < numChannels; ++ch)
        {
            float *dst = waveform.getWritePointer(ch);

            for (int k = -kSpliceHalf; k < kSpliceHalf; ++k)
            {
                const int pos = junction + k;
                if (pos < 0 || pos >= totalSamples)
                    continue;

                // Crossfade parameter: 0.0 at left edge → 1.0 at right edge
                const float t_raw = static_cast<float>(k + kSpliceHalf) /
                                    static_cast<float>(2 * kSpliceHalf);
                const float t = t_raw * t_raw * (3.0f - 2.0f * t_raw);

                // synth1 value — use real margin data if available
                const int s1idx = pos - n1GlobalStart;
                float val1;
                if (s1idx >= 0 && s1idx < n1TotalLen)
                {
                    val1 = sw1[static_cast<size_t>(s1idx)];
                }
                else
                {
                    // Fallback: clamp to nearest valid sample
                    val1 = sw1[static_cast<size_t>(std::clamp(s1idx, 0, n1TotalLen - 1))];
                }

                // synth2 value — use real margin data if available
                const int s2idx = pos - n2GlobalStart;
                float val2;
                if (s2idx >= 0 && s2idx < n2TotalLen)
                {
                    val2 = sw2[static_cast<size_t>(s2idx)];
                }
                else
                {
                    // Fallback: clamp to nearest valid sample
                    val2 = sw2[static_cast<size_t>(std::clamp(s2idx, 0, n2TotalLen - 1))];
                }

                dst[pos] = val1 * (1.0f - t) + val2 * t;
            }
        }
    }
}

namespace
{
    std::vector<float> fitFloatCurve(const std::vector<float>& source,
                                     int targetLength,
                                     float defaultValue)
    {
        if (targetLength <= 0)
            return {};
        if (source.empty())
        {
            return std::vector<float>(static_cast<size_t>(targetLength),
                                      defaultValue);
        }
        if (static_cast<int>(source.size()) == targetLength)
            return source;
        return CurveResampler::resampleLinear(source, targetLength);
    }

    std::vector<bool> fitBoolCurve(const std::vector<bool>& source,
                                   int targetLength)
    {
        if (targetLength <= 0)
            return {};
        if (source.empty())
            return std::vector<bool>(static_cast<size_t>(targetLength), false);
        if (static_cast<int>(source.size()) == targetLength)
            return source;
        return CurveResampler::resampleNearest(source, targetLength);
    }

    std::vector<bool> buildSourceVoicedMask(const std::vector<float>& globalOriginalF0,
                                            int srcStart,
                                            int srcEnd)
    {
        const int len = srcEnd - srcStart;
        if (len <= 0 || globalOriginalF0.empty())
            return {};

        const int globalSize = static_cast<int>(globalOriginalF0.size());
        std::vector<bool> voiced(static_cast<size_t>(len), false);
        for (int i = 0; i < len; ++i)
        {
            const int gi = srcStart + i;
            if (gi >= 0 && gi < globalSize)
                voiced[static_cast<size_t>(i)] = globalOriginalF0[static_cast<size_t>(gi)] > 1.0f;
        }
        return voiced;
    }

    std::vector<std::vector<float>> computeSourceMelClip(const AudioData& audioData,
                                                         const AnalysisData& analysisData,
                                                         const Note& note,
                                                         int numMels)
    {
        const int sourceLength = std::max(0, note.getSrcDurationFrames());
        if (sourceLength <= 0)
            return {};

        const auto& sourceWaveform =
            audioData.originalWaveform.getNumSamples() > 0
                ? audioData.originalWaveform
                : audioData.waveform;
        if (sourceWaveform.getNumChannels() > 0 && sourceWaveform.getNumSamples() > 0)
        {
            CenteredMelSpectrogram melComputer(audioData.sampleRate, N_FFT, WIN_SIZE,
                                               numMels, FMIN, FMAX);
            std::vector<std::vector<float>> sourceMel;
            melComputer.computeTimeStretched(sourceWaveform.getReadPointer(0),
                                             sourceWaveform.getNumSamples(),
                                             note.getSrcStartFrame(),
                                             note.getSrcEndFrame(),
                                             sourceLength,
                                             sourceMel);
            if (!sourceMel.empty())
                return sourceMel;
        }

        if (!analysisData.originalMel.empty())
        {
            const int melStart = std::clamp(note.getSrcStartFrame(), 0,
                                            static_cast<int>(analysisData.originalMel.size()));
            const int melEnd = std::clamp(note.getSrcEndFrame(), melStart,
                                          static_cast<int>(analysisData.originalMel.size()));
            if (melEnd > melStart)
            {
                return std::vector<std::vector<float>>(
                    analysisData.originalMel.begin() + melStart,
                    analysisData.originalMel.begin() + melEnd);
            }
        }

        return std::vector<std::vector<float>>(
            static_cast<size_t>(sourceLength),
            std::vector<float>(static_cast<size_t>(numMels), 0.0f));
    }

    const std::vector<std::vector<float>>& ensureSourceMelClip(Note& note,
                                                               const AudioData& audioData,
                                                               const AnalysisData& analysisData,
                                                               int numMels)
    {
        if (!note.hasClipMel())
            note.setClipMel(computeSourceMelClip(audioData, analysisData,
                                                 note, numMels));
        return note.getClipMel();
    }

    std::vector<std::vector<float>> resampleMelHybrid(
        Note& note,
        const AudioData& audioData,
        const AnalysisData& analysisData,
        const EditedData& editedData,
        int targetLength,
        int numMels)
    {
        if (targetLength <= 0)
            return {};

        const auto& sourceMel = ensureSourceMelClip(note, audioData,
                                                    analysisData, numMels);
        if (sourceMel.empty())
        {
            return std::vector<std::vector<float>>(
                static_cast<size_t>(targetLength),
                std::vector<float>(static_cast<size_t>(numMels), 0.0f));
        }

        auto harmonic = CurveResampler::resampleLinear2D(sourceMel, targetLength);
        auto noise = CurveResampler::resampleNearest2D(sourceMel, targetLength);
        auto voiced = fitBoolCurve(
            buildSourceVoicedMask(editedData.f0,
                                  note.getSrcStartFrame(),
                                  note.getSrcEndFrame()),
            targetLength);

        if (harmonic.size() != static_cast<size_t>(targetLength))
        {
            harmonic.resize(static_cast<size_t>(targetLength),
                            std::vector<float>(static_cast<size_t>(numMels),
                                               0.0f));
        }
        if (noise.size() != static_cast<size_t>(targetLength))
        {
            noise.resize(static_cast<size_t>(targetLength),
                         std::vector<float>(static_cast<size_t>(numMels),
                                            0.0f));
        }

        for (auto& frame : harmonic)
        {
            if (frame.size() != static_cast<size_t>(numMels))
                frame.resize(static_cast<size_t>(numMels), 0.0f);
        }
        for (auto& frame : noise)
        {
            if (frame.size() != static_cast<size_t>(numMels))
                frame.resize(static_cast<size_t>(numMels), 0.0f);
        }

        if (voiced.empty())
            return harmonic;

        std::vector<std::vector<float>> blended = noise;
        for (int i = 0; i < targetLength; ++i)
        {
            if (!voiced[static_cast<size_t>(i)])
                continue;
            blended[static_cast<size_t>(i)] = harmonic[static_cast<size_t>(i)];
        }
        return blended;
    }

} // namespace

void Project::addListener(ProjectListener* listener)
{
  if (listener && std::find(listeners.begin(), listeners.end(), listener) == listeners.end())
    listeners.push_back(listener);
}

void Project::removeListener(ProjectListener* listener)
{
  listeners.erase(std::remove(listeners.begin(), listeners.end(), listener), listeners.end());
}

void Project::notifyListeners(ProjectChangeType type,
                              int affectedNoteIndex,
                              int rangeStart,
                              int rangeEnd)
{
  for (auto* l : listeners)
    l->onProjectChanged(type, affectedNoteIndex, rangeStart, rangeEnd);
}

void Project::initAuditionBufferFromOriginal()
{
  const auto& orig = audioData.originalWaveform;
  if (orig.getNumSamples() > 0)
    auditionBuffer.makeCopyOf(orig);
}

void Project::applyNoteVolumeToSynthesizedRange(std::vector<float>& synthesized,
                                                int startFrame,
                                                int endFrame,
                                                int hopSize) const
{
  if (synthesized.empty() || hopSize <= 0 || endFrame <= startFrame)
    return;

  const int expectedSamples = (endFrame - startFrame) * hopSize;
  const int samplesToProcess =
      std::min(expectedSamples, static_cast<int>(synthesized.size()));
  if (samplesToProcess <= 0)
    return;

  for (const auto& note : notes)
  {
    if (note.isRest())
      continue;
    if (std::abs(note.getVolumeDb()) < 0.001f)
      continue;

    const int overlapStart = std::max(startFrame, note.getStartFrame());
    const int overlapEnd = std::min(endFrame, note.getEndFrame());
    if (overlapEnd <= overlapStart)
      continue;

    const int localStart = (overlapStart - startFrame) * hopSize;
    const int localEnd = (overlapEnd - startFrame) * hopSize;
    if (localStart >= samplesToProcess)
      continue;

    const float gain =
        juce::Decibels::decibelsToGain(note.getVolumeDb(), -60.0f);
    const int clampedStart = std::max(0, localStart);
    const int clampedEnd = std::min(samplesToProcess, localEnd);
    for (int i = clampedStart; i < clampedEnd; ++i)
      synthesized[static_cast<size_t>(i)] *= gain;
  }
}

void Project::blendSynthesizedRangeIntoAuditionBuffer(
    const std::vector<float>& synthesized,
    int startFrame,
    int endFrame,
    int hopSize)
{
  if (synthesized.empty() || hopSize <= 0)
    return;

  const int outputFrames = getFrameCount();
  const int requiredFrames = outputFrames > 0
                                 ? std::max(outputFrames, endFrame)
                                 : endFrame;
  const int requiredSamples = std::max(0, requiredFrames * hopSize);
  if (requiredSamples <= 0)
    return;

  const auto& waveform = audioData.waveform;
  if (waveform.getNumChannels() > 0 && waveform.getNumSamples() > 0)
  {
    auditionBuffer.makeCopyOf(waveform);
  }
  else if (auditionBuffer.getNumSamples() == 0)
  {
    initAuditionBufferFromOriginal();
  }

  if (auditionBuffer.getNumChannels() == 0)
  {
    if (waveform.getNumChannels() > 0)
      auditionBuffer.makeCopyOf(waveform);
  }

  const int numChannels = auditionBuffer.getNumChannels();
  if (numChannels == 0)
    return;

  if (auditionBuffer.getNumSamples() != requiredSamples)
    auditionBuffer.setSize(numChannels, requiredSamples, true, true, false);

  const int rawStartSample = startFrame * hopSize;
  const int rawEndSample = endFrame * hopSize;
  const int rangeSamples = rawEndSample - rawStartSample;
  if (rangeSamples <= 0)
    return;

  const int sourceSamples =
      std::min(rangeSamples, static_cast<int>(synthesized.size()));
  const int sourceOffset = std::max(0, -rawStartSample);
  if (sourceOffset >= sourceSamples)
    return;

  const int startSample = std::max(0, rawStartSample);
  const int endSample = std::min(auditionBuffer.getNumSamples(),
                                 rawEndSample);
  if (endSample <= startSample)
    return;

  const int numSamples = std::min(
      endSample - startSample, sourceSamples - sourceOffset);
  const int fade = std::min(hopSize, sourceSamples / 2);

  for (int ch = 0; ch < numChannels; ++ch)
  {
    float* dst = auditionBuffer.getWritePointer(ch, startSample);
    for (int i = 0; i < numSamples; ++i)
    {
      const int sourceIndex = sourceOffset + i;
      float mix = 1.0f;
      if (fade > 0 && sourceIndex < fade)
        mix = static_cast<float>(sourceIndex) / static_cast<float>(fade);
      if (fade > 0 && sourceSamples - 1 - sourceIndex < fade)
        mix = std::min(mix, static_cast<float>(sourceSamples - 1 - sourceIndex) /
                            static_cast<float>(fade));
      const float synth = synthesized[static_cast<size_t>(sourceIndex)];
      dst[i] = dst[i] + mix * (synth - dst[i]);
    }
  }

  audioData.waveform.makeCopyOf(auditionBuffer);
}

void Project::refreshNoteCaches()
{
  const int totalFrames = editedData.getNumFrames();
  if (totalFrames == 0)
    return;

  int segmentIndex = 0;
  for (int noteIdx = 0; noteIdx < static_cast<int>(notes.size()); ++noteIdx)
  {
    auto& note = notes[static_cast<size_t>(noteIdx)];
    if (note.isRest())
      continue;

    const int start = note.getStartFrame();
    const int end = note.getEndFrame();
    const int len = end - start;
    if (len <= 0)
      continue;

    auto sliceFloat = [&](const std::vector<float>& global) {
      std::vector<float> slice(static_cast<size_t>(len));
      for (int i = 0; i < len; ++i)
      {
        int gi = start + i;
        if (gi >= 0 && gi < totalFrames)
          slice[static_cast<size_t>(i)] = global[static_cast<size_t>(gi)];
      }
      return slice;
    };

    note.setBasePitch(sliceFloat(editedData.basePitch));
    note.setDeltaPitch(sliceFloat(editedData.deltaPitch));

    if (!editedData.voicingCurve.empty())
      note.setVoicingCurve(sliceFloat(editedData.voicingCurve));
    if (!editedData.breathCurve.empty())
      note.setBreathCurve(sliceFloat(editedData.breathCurve));
    if (!editedData.tensionCurve.empty())
      note.setTensionCurve(sliceFloat(editedData.tensionCurve));

    if (segmentIndex < static_cast<int>(analysisData.noteSegments.size()))
    {
      note.setSrcStartFrame(analysisData.noteSegments[static_cast<size_t>(segmentIndex)].srcStartFrame);
      note.setSrcEndFrame(analysisData.noteSegments[static_cast<size_t>(segmentIndex)].srcEndFrame);
    }
    ++segmentIndex;

    const int analysisFrames = analysisData.getNumFrames();
    if (analysisFrames > 0)
    {
      const int sourceStart = note.getSrcStartFrame();
      const int sourceEnd = note.getSrcEndFrame();
      const int sourceLen = sourceEnd - sourceStart;
      auto sliceAnalysis = [&](const std::vector<float>& global) {
        std::vector<float> slice(static_cast<size_t>(sourceLen));
        for (int i = 0; i < sourceLen; ++i)
        {
          int gi = sourceStart + i;
          if (gi >= 0 && gi < analysisFrames)
            slice[static_cast<size_t>(i)] = global[static_cast<size_t>(gi)];
        }
        return slice;
      };

      if (sourceLen > 0 && !analysisData.originalDeltaPitch.empty())
        note.setOriginalDeltaPitch(sliceAnalysis(analysisData.originalDeltaPitch));
      if (sourceLen > 0 && !analysisData.originalPitch.empty())
        note.setOriginalPitch(sliceAnalysis(analysisData.originalPitch));
    }
  }
}

void Project::refreshNoteCachesForRange(int startFrame, int endFrame)
{
  for (auto& note : notes)
  {
    if (note.isRest())
      continue;
    if (note.getEndFrame() <= startFrame || note.getStartFrame() >= endFrame)
      continue;

    const int noteStart = note.getStartFrame();
    const int noteEnd = note.getEndFrame();
    const int len = noteEnd - noteStart;
    if (len <= 0)
      continue;

    const int totalFrames = editedData.getNumFrames();
    auto sliceFloat = [&](const std::vector<float>& global) {
      std::vector<float> slice(static_cast<size_t>(len));
      for (int i = 0; i < len; ++i)
      {
        int gi = noteStart + i;
        if (gi >= 0 && gi < totalFrames)
          slice[static_cast<size_t>(i)] = global[static_cast<size_t>(gi)];
      }
      return slice;
    };

    note.setBasePitch(sliceFloat(editedData.basePitch));
    note.setDeltaPitch(sliceFloat(editedData.deltaPitch));
    if (!editedData.voicingCurve.empty())
      note.setVoicingCurve(sliceFloat(editedData.voicingCurve));
    if (!editedData.breathCurve.empty())
      note.setBreathCurve(sliceFloat(editedData.breathCurve));
    if (!editedData.tensionCurve.empty())
      note.setTensionCurve(sliceFloat(editedData.tensionCurve));
  }
}
