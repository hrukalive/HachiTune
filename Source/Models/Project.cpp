#include "Project.h"
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

Note *Project::getNoteAtFrame(int frame)
{
    for (auto &note : notes)
    {
        if (note.containsFrame(frame))
            return &note;
    }
    return nullptr;
}

std::vector<Note *> Project::getNotesInRange(int startFrame, int endFrame)
{
    std::vector<Note *> result;
    for (auto &note : notes)
    {
        if (note.getStartFrame() < endFrame && note.getEndFrame() > startFrame)
            result.push_back(&note);
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
}

void Project::selectAllNotes(bool includeRests)
{
    for (auto &note : notes)
    {
        if (!includeRests && note.isRest())
            continue;
        note.setSelected(true);
    }
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

void Project::clearAllDirty()
{
    for (auto &note : notes)
        note.clearDirty();
    // Also clear F0 dirty range
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
    // Also clear parameter dirty range
    paramDirtyStart = -1;
    paramDirtyEnd = -1;
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
}

void Project::clearF0DirtyRange()
{
    f0DirtyStart = -1;
    f0DirtyEnd = -1;
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
}

void Project::clearParamDirtyRange()
{
    paramDirtyStart = -1;
    paramDirtyEnd = -1;
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
    if (audioData.basePitch.empty() || audioData.deltaPitch.empty())
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
                                note.getVibratoDepthSemitones() > 0.0001f &&
                                note.getVibratoRateHz() > 0.0001f;
        if (!hasVibrato)
            continue;

        const int start = std::max(0, note.getStartFrame());
        const int end = std::min(note.getEndFrame(), static_cast<int>(adjustedF0.size()));

        for (int i = start; i < end; ++i)
        {
            if (i < static_cast<int>(audioData.voicedMask.size()) && !audioData.voicedMask[i])
                continue;

            float vib = note.getVibratoDepthSemitones() *
                        std::sin(twoPi * note.getVibratoRateHz() * framesToSeconds(i - start) +
                                 note.getVibratoPhaseRadians());
            adjustedF0[static_cast<size_t>(i)] *= std::pow(2.0f, vib / 12.0f);
        }
    }

    return adjustedF0;
}

std::vector<float> Project::getAdjustedF0ForRange(int startFrame, int endFrame) const
{
    if (audioData.basePitch.empty() || audioData.deltaPitch.empty())
        return {};

    // Clamp range
    startFrame = std::max(0, startFrame);
    endFrame = std::min(endFrame, static_cast<int>(audioData.basePitch.size()));

    if (startFrame >= endFrame)
        return {};

    const int rangeSize = endFrame - startFrame;
    std::vector<float> adjustedF0(static_cast<size_t>(rangeSize), 0.0f);

    for (int i = 0; i < rangeSize; ++i)
    {
        const int globalIdx = startFrame + i;
        const float base = audioData.basePitch[static_cast<size_t>(globalIdx)];
        const float delta = (globalIdx < static_cast<int>(audioData.deltaPitch.size()))
                                ? audioData.deltaPitch[static_cast<size_t>(globalIdx)]
                                : 0.0f;
        float midi = base + delta + globalPitchOffset;
        adjustedF0[static_cast<size_t>(i)] = midiToFreq(midi);
    }

    // Apply vibrato for overlapping notes
    for (const auto &note : notes)
    {
        const bool hasVibrato = note.isVibratoEnabled() &&
                                note.getVibratoDepthSemitones() > 0.0001f &&
                                note.getVibratoRateHz() > 0.0001f;
        if (!hasVibrato)
            continue;

        const int overlapStart = std::max(note.getStartFrame(), startFrame);
        const int overlapEnd = std::min(note.getEndFrame(), endFrame);
        for (int frame = overlapStart; frame < overlapEnd; ++frame)
        {
            const int localIdx = frame - startFrame;
            if (frame < static_cast<int>(audioData.voicedMask.size()) && !audioData.voicedMask[frame])
                continue;

            float vib = note.getVibratoDepthSemitones() *
                        std::sin(twoPi * note.getVibratoRateHz() * framesToSeconds(frame - note.getStartFrame()) +
                                 note.getVibratoPhaseRadians());
            adjustedF0[static_cast<size_t>(localIdx)] *= std::pow(2.0f, vib / 12.0f);
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
}

void Project::setLoopEnabled(bool enabled)
{
    if (enabled && loopRange.endSeconds <= loopRange.startSeconds)
        loopRange.enabled = false;
    else
        loopRange.enabled = enabled;
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
}

void Project::setScaleRootNote(int noteInOctave)
{
    const int normalized = juce::jlimit(-1, 11, noteInOctave);
    if (scaleRootNote == normalized)
        return;

    scaleRootNote = normalized;
    modified = true;
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
}

void Project::setTimelineTempoBpm(double bpm)
{
    const double normalized = juce::jlimit(20.0, 300.0, bpm);
    if (std::abs(timelineTempoBpm - normalized) < 1.0e-6)
        return;

    timelineTempoBpm = normalized;
    modified = true;
}

void Project::setTimelineGridDivision(TimelineGridDivision division)
{
    const auto normalized = normalizeGridDivision(division);
    if (timelineGridDivision == normalized)
        return;

    timelineGridDivision = normalized;
    modified = true;
}

void Project::setTimelineSnapCycle(bool enabled)
{
    if (timelineSnapCycle == enabled)
        return;

    timelineSnapCycle = enabled;
    modified = true;
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
    std::sort(sortedNotes.begin(), sortedNotes.end(),
              [](const Note *a, const Note *b)
              {
                  return a->getStartFrame() < b->getStartFrame();
              });

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
    std::sort(sortedNotes.begin(), sortedNotes.end(),
              [](const Note *a, const Note *b)
              {
                  return a->getStartFrame() < b->getStartFrame();
              });

    // --- Step 2: Compute required output buffer length --------------------
    // Output must hold all shifted notes + trailing gap after the last note.
    int requiredSamples = origSamples;
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
        // Trailing gap: original audio after last note's source end, placed
        // after last note's output end.
        int srcTrailLen = std::max(0,
                                   origSamples - last->getSrcEndFrame() * HOP_SIZE);
        requiredSamples = std::max(requiredSamples,
                                   last->getEndFrame() * HOP_SIZE + srcTrailLen);
    }

    // Resize waveform buffer if needed (grow only — shrinking left to caller)
    if (waveform.getNumSamples() < requiredSamples)
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
        // No notes — copy entire original
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
                // Trailing gap: preserve original gap length (1:1 copy),
                // don't stretch to fill the entire buffer which may be
                // larger than needed from a previous longer stretch.
                gapSrcEnd = origSamples;
                gapDstEnd = gapDstStart + std::max(0, gapSrcEnd - gapSrcStart);
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

    std::vector<bool> buildSourceVoicedMask(const Note& note)
    {
        const auto& f0 = note.getF0Values();
        if (f0.empty())
            return {};

        std::vector<bool> voiced(static_cast<size_t>(f0.size()), false);
        for (size_t i = 0; i < f0.size(); ++i)
            voiced[i] = f0[i] > 1.0f;
        return voiced;
    }

    std::vector<std::vector<float>> computeSourceMelClip(const AudioData& audioData,
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

        if (!audioData.melSpectrogram.empty())
        {
            const int melStart = std::clamp(note.getSrcStartFrame(), 0,
                                            static_cast<int>(audioData.melSpectrogram.size()));
            const int melEnd = std::clamp(note.getSrcEndFrame(), melStart,
                                          static_cast<int>(audioData.melSpectrogram.size()));
            if (melEnd > melStart)
            {
                return std::vector<std::vector<float>>(
                    audioData.melSpectrogram.begin() + melStart,
                    audioData.melSpectrogram.begin() + melEnd);
            }
        }

        return std::vector<std::vector<float>>(
            static_cast<size_t>(sourceLength),
            std::vector<float>(static_cast<size_t>(numMels), 0.0f));
    }

    const std::vector<std::vector<float>>& ensureSourceMelClip(Note& note,
                                                               const AudioData& audioData,
                                                               int numMels)
    {
        if (!note.hasClipMel())
            note.setClipMel(computeSourceMelClip(audioData, note, numMels));
        return note.getClipMel();
    }

    std::vector<std::vector<float>> resampleMelHybrid(
        Note& note,
        const AudioData& audioData,
        int targetLength,
        int numMels)
    {
        if (targetLength <= 0)
            return {};

        const auto& sourceMel = ensureSourceMelClip(note, audioData, numMels);
        if (sourceMel.empty())
        {
            return std::vector<std::vector<float>>(
                static_cast<size_t>(targetLength),
                std::vector<float>(static_cast<size_t>(numMels), 0.0f));
        }

        auto harmonic = CurveResampler::resampleLinear2D(sourceMel, targetLength);
        auto noise = CurveResampler::resampleNearest2D(sourceMel, targetLength);
        auto voiced = fitBoolCurve(buildSourceVoicedMask(note), targetLength);

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

    std::vector<Project::WarpMarker> sortedUniqueMarkers(
        const std::vector<Project::WarpMarker>& markers)
    {
        std::vector<Project::WarpMarker> result = markers;
        std::sort(result.begin(), result.end(),
                  [](const auto& a, const auto& b)
                  {
                      if (a.sourceFrame != b.sourceFrame)
                          return a.sourceFrame < b.sourceFrame;
                      return a.outputFrame < b.outputFrame;
                  });

        result.erase(std::unique(result.begin(), result.end(),
                                 [](const auto& a, const auto& b)
                                 {
                                     return a.sourceFrame == b.sourceFrame;
                                 }),
                     result.end());
        return result;
    }

    std::vector<float> selectSourceCurve(const std::vector<float>& current,
                                         const std::vector<float>& source)
    {
        if (!source.empty())
            return source;
        return current;
    }

    int getWaveformFrameCount(const juce::AudioBuffer<float>& buffer)
    {
        const int numSamples = buffer.getNumSamples();
        if (numSamples <= 0)
            return 0;
        return numSamples / HOP_SIZE + 1;
    }

    int getProjectSourceFrameLimit(const Project& project)
    {
        const auto& audioData = project.getAudioData();
        int limit = 0;
        if (audioData.originalWaveform.getNumSamples() > 0)
            limit = getWaveformFrameCount(audioData.originalWaveform);
        else if (audioData.waveform.getNumSamples() > 0)
            limit = getWaveformFrameCount(audioData.waveform);
        else
            limit = std::max(0, audioData.getNumFrames());

        for (const auto& note : project.getNotes())
            limit = std::max(limit, note.getSrcEndFrame());
        return limit;
    }

    void rebuildVadMaskFromWaveform(AudioData& audioData)
    {
        constexpr float kVadThreshold = 0.008f;

        const int numFrames = audioData.getNumFrames();
        audioData.vadMask.assign(static_cast<size_t>(numFrames), false);
        if (numFrames <= 0 || audioData.waveform.getNumSamples() <= 0)
            return;

        const float* samples = audioData.waveform.getReadPointer(0);
        const int numSamples = audioData.waveform.getNumSamples();
        for (int frame = 0; frame < numFrames; ++frame)
        {
            const int sampleStart = frame * HOP_SIZE;
            const int sampleEnd = std::min(sampleStart + HOP_SIZE, numSamples);
            if (sampleStart >= numSamples || sampleEnd <= sampleStart)
                continue;

            double sumSq = 0.0;
            for (int sample = sampleStart; sample < sampleEnd; ++sample)
            {
                const double value = samples[sample];
                sumSq += value * value;
            }

            const float rms = static_cast<float>(
                std::sqrt(sumSq /
                          static_cast<double>(sampleEnd - sampleStart)));
            audioData.vadMask[static_cast<size_t>(frame)] =
                rms > kVadThreshold;
        }
    }
} // namespace

namespace WarpMarkerProcessor
{
int getSourceFrameLimit(const Project& project)
{
    return getProjectSourceFrameLimit(project);
}

std::vector<Project::WarpMarker> normalizeMarkers(
    const Project& project,
    const std::vector<Project::WarpMarker>& markers)
{
    const int totalFrames = getSourceFrameLimit(project);
    if (totalFrames <= 1)
        return {};

    auto result = sortedUniqueMarkers(markers);
    result.erase(std::remove_if(result.begin(), result.end(),
                                [totalFrames](const auto& marker)
                                {
                                    return marker.sourceFrame <= 0 ||
                                           marker.sourceFrame >= totalFrames;
                                }),
                 result.end());

    for (auto& marker : result)
        marker.outputFrame = std::clamp(marker.outputFrame, 1, totalFrames - 1);

    int prevOut = 0;
    for (auto& marker : result)
    {
        marker.outputFrame = std::max(marker.outputFrame, prevOut + 1);
        prevOut = marker.outputFrame;
    }

    int nextOut = totalFrames;
    for (int i = static_cast<int>(result.size()) - 1; i >= 0; --i)
    {
        result[static_cast<size_t>(i)].outputFrame =
            std::min(result[static_cast<size_t>(i)].outputFrame, nextOut - 1);
        nextOut = result[static_cast<size_t>(i)].outputFrame;
    }

    return result;
}

bool hasMarkerAtSourceFrame(const std::vector<Project::WarpMarker>& markers,
                            int sourceFrame)
{
    return std::any_of(markers.begin(), markers.end(),
                       [sourceFrame](const auto& marker)
                       {
                           return marker.sourceFrame == sourceFrame;
                       });
}

std::vector<Boundary> collectBoundaries(Project& project)
{
    std::vector<Boundary> boundaries;
    std::vector<Note*> ordered;
    ordered.reserve(project.getNotes().size());
    for (auto& note : project.getNotes())
    {
        if (!note.isRest())
            ordered.push_back(&note);
    }

    if (ordered.empty())
        return boundaries;

    std::sort(ordered.begin(), ordered.end(),
              [](const Note* a, const Note* b)
              {
                  return a->getStartFrame() < b->getStartFrame();
              });

    constexpr int gapThreshold = 3;
    const auto markers = normalizeMarkers(project, project.getWarpMarkers());

    for (size_t i = 0; i < ordered.size(); ++i)
    {
        Note* current = ordered[i];
        Note* prev = (i > 0) ? ordered[i - 1] : nullptr;
        Note* next = (i + 1 < ordered.size()) ? ordered[i + 1] : nullptr;

        bool hasGapBefore = true;
        if (prev)
        {
            const int gap = current->getStartFrame() - prev->getEndFrame();
            hasGapBefore = gap > gapThreshold;
        }

        bool hasGapAfter = true;
        if (next)
        {
            const int gap = next->getStartFrame() - current->getEndFrame();
            hasGapAfter = gap > gapThreshold;
        }

        if (hasGapBefore)
        {
            const int sourceFrame = current->getSrcStartFrame();
            boundaries.push_back({nullptr, current, sourceFrame,
                                  current->getStartFrame(),
                                  hasMarkerAtSourceFrame(markers, sourceFrame)});
        }

        if (hasGapAfter)
        {
            const int sourceFrame = current->getSrcEndFrame();
            boundaries.push_back({current, nullptr, sourceFrame,
                                  current->getEndFrame(),
                                  hasMarkerAtSourceFrame(markers, sourceFrame)});
        }

        if (next && !hasGapAfter)
        {
            const int sourceFrame = current->getSrcEndFrame();
            boundaries.push_back({current, next, sourceFrame,
                                  current->getEndFrame(),
                                  hasMarkerAtSourceFrame(markers, sourceFrame)});
        }
    }

    std::sort(boundaries.begin(), boundaries.end(),
              [](const auto& a, const auto& b)
              {
                  if (a.currentFrame != b.currentFrame)
                      return a.currentFrame < b.currentFrame;
                  return a.sourceFrame < b.sourceFrame;
              });
    return boundaries;
}

int mapSourceFrame(const Project& project,
                   int sourceFrame,
                   const std::vector<Project::WarpMarker>& markers)
{
    const int totalFrames = getSourceFrameLimit(project);
    if (totalFrames <= 0)
        return 0;

    sourceFrame = std::clamp(sourceFrame, 0, totalFrames);
    const auto normalized = normalizeMarkers(project, markers);

    int prevSource = 0;
    int prevOutput = 0;
    for (const auto& marker : normalized)
    {
        if (sourceFrame <= marker.sourceFrame)
        {
            const int srcSpan = std::max(1, marker.sourceFrame - prevSource);
            const double ratio =
                static_cast<double>(sourceFrame - prevSource) /
                static_cast<double>(srcSpan);
            return prevOutput +
                   static_cast<int>(std::lround(
                       ratio *
                       static_cast<double>(marker.outputFrame - prevOutput)));
        }

        prevSource = marker.sourceFrame;
        prevOutput = marker.outputFrame;
    }

    const int srcSpan = std::max(1, totalFrames - prevSource);
    const double ratio =
        static_cast<double>(sourceFrame - prevSource) /
        static_cast<double>(srcSpan);
    return prevOutput + static_cast<int>(std::lround(
                            ratio *
                            static_cast<double>(totalFrames - prevOutput)));
}

int computeSegmentMinimumOutputSpan(const Project& project,
                                    int sourceStartFrame,
                                    int sourceEndFrame,
                                    int minNoteFrames)
{
    if (sourceEndFrame <= sourceStartFrame)
        return 0;

    const int sourceSpan = sourceEndFrame - sourceStartFrame;
    int minimumSpan = 1;

    for (const auto& note : project.getNotes())
    {
        if (note.isRest())
            continue;
        if (note.getSrcStartFrame() < sourceStartFrame ||
            note.getSrcEndFrame() > sourceEndFrame)
        {
            continue;
        }

        const int srcDuration = note.getSrcDurationFrames();
        if (srcDuration <= 0)
            continue;

        const double required =
            static_cast<double>(minNoteFrames) *
            static_cast<double>(sourceSpan) /
            static_cast<double>(srcDuration);
        minimumSpan = std::max(minimumSpan,
                               static_cast<int>(std::ceil(required)));
    }

    return minimumSpan;
}

void syncSourceCurvesFromCurrent(Project& project,
                                 int minNoteIndex,
                                 int maxNoteIndex)
{
    auto& notes = project.getNotes();
    if (notes.empty())
        return;

    minNoteIndex = std::max(0, minNoteIndex);
    maxNoteIndex = std::min(maxNoteIndex, static_cast<int>(notes.size()) - 1);
    if (minNoteIndex > maxNoteIndex)
        return;

    for (int i = minNoteIndex; i <= maxNoteIndex; ++i)
    {
        auto& note = notes[static_cast<size_t>(i)];
        const int srcDuration = std::max(0, note.getSrcDurationFrames());
        if (note.isRest() || srcDuration <= 0)
            continue;

        note.setSourceVoicingCurve(
            fitFloatCurve(note.getVoicingCurve(), srcDuration,
                          HNSepCurveProcessor::kDefaultVoicing));
        note.setSourceBreathCurve(
            fitFloatCurve(note.getBreathCurve(), srcDuration,
                          HNSepCurveProcessor::kDefaultBreath));
        note.setSourceTensionCurve(
            fitFloatCurve(note.getTensionCurve(), srcDuration,
                          HNSepCurveProcessor::kDefaultTension));
    }
}

void recomputeFromMarkers(Project& project,
                          const std::vector<Project::WarpMarker>& markers,
                          bool updateProjectMarkers)
{
    const auto normalizedMarkers = normalizeMarkers(project, markers);
    if (updateProjectMarkers)
        project.setWarpMarkers(normalizedMarkers);

    auto& audioData = project.getAudioData();
    const int totalFrames = getSourceFrameLimit(project);
    if (totalFrames <= 0)
        return;

    const int numMels =
        (!audioData.melSpectrogram.empty() &&
         !audioData.melSpectrogram.front().empty())
            ? static_cast<int>(audioData.melSpectrogram.front().size())
            : NUM_MELS;

    std::vector<std::vector<float>> newMel(
        static_cast<size_t>(totalFrames),
        std::vector<float>(static_cast<size_t>(numMels), 0.0f));
    std::vector<bool> newVoiced(static_cast<size_t>(totalFrames), false);

    for (auto& note : project.getNotes())
    {
        if (note.isRest())
            continue;

        const int oldStart = note.getStartFrame();
        const int oldEnd = note.getEndFrame();

        int newStart =
            mapSourceFrame(project, note.getSrcStartFrame(), normalizedMarkers);
        int newEnd =
            mapSourceFrame(project, note.getSrcEndFrame(), normalizedMarkers);
        newStart = std::clamp(newStart, 0, totalFrames);
        newEnd = std::clamp(newEnd, newStart + 1, totalFrames);

        note.setStartFrame(newStart);
        note.setEndFrame(newEnd);

        const int durationFrames = note.getDurationFrames();
        if (durationFrames <= 0)
            continue;

        if (oldStart != newStart || oldEnd != newEnd)
        {
            note.markDirty();
            note.markSynthDirty();
        }

        const auto& sourceDelta = note.hasOriginalDeltaPitch()
                                      ? note.getOriginalDeltaPitch()
                                      : note.getDeltaPitch();
        note.setDeltaPitch(fitFloatCurve(sourceDelta, durationFrames, 0.0f));

        const auto sourceVoicing =
            selectSourceCurve(note.getVoicingCurve(),
                              note.getSourceVoicingCurve());
        const auto sourceBreath =
            selectSourceCurve(note.getBreathCurve(),
                              note.getSourceBreathCurve());
        const auto sourceTension =
            selectSourceCurve(note.getTensionCurve(),
                              note.getSourceTensionCurve());

        note.setVoicingCurve(fitFloatCurve(sourceVoicing, durationFrames,
                                           HNSepCurveProcessor::kDefaultVoicing));
        note.setBreathCurve(fitFloatCurve(sourceBreath, durationFrames,
                                          HNSepCurveProcessor::kDefaultBreath));
        note.setTensionCurve(fitFloatCurve(sourceTension, durationFrames,
                                           HNSepCurveProcessor::kDefaultTension));

        if (!note.hasSourceVoicingCurve())
            note.setSourceVoicingCurve(sourceVoicing);
        if (!note.hasSourceBreathCurve())
            note.setSourceBreathCurve(sourceBreath);
        if (!note.hasSourceTensionCurve())
            note.setSourceTensionCurve(sourceTension);

        if (note.hasSrcClipWaveform())
        {
            note.setClipWaveform(CurveResampler::resampleLinear(
                note.getSrcClipWaveform(), durationFrames * HOP_SIZE));
        }
        else
        {
            note.setClipWaveform({});
        }

        const auto voicedFrames =
            fitBoolCurve(buildSourceVoicedMask(note), durationFrames);
        for (int i = 0; i < durationFrames && (newStart + i) < totalFrames; ++i)
        {
            newVoiced[static_cast<size_t>(newStart + i)] =
                voicedFrames.empty()
                    ? true
                    : voicedFrames[static_cast<size_t>(i)];
        }

        const auto noteMel =
            resampleMelHybrid(note, audioData, durationFrames, numMels);
        for (int i = 0; i < durationFrames && (newStart + i) < totalFrames; ++i)
            newMel[static_cast<size_t>(newStart + i)] =
                noteMel[static_cast<size_t>(i)];
    }

    audioData.voicedMask = std::move(newVoiced);
    audioData.melSpectrogram = std::move(newMel);

    PitchCurveProcessor::rebuildBaseFromNotes(project);
    HNSepCurveProcessor::rebuildCurvesFromNotes(project);
    project.composeGlobalWaveform();
    rebuildVadMaskFromWaveform(audioData);

    if (updateProjectMarkers)
        project.setModified(true);
}
} // namespace WarpMarkerProcessor
