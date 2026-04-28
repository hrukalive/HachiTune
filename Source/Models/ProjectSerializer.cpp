#include "ProjectSerializer.h"
#include "../Utils/CurveResampler.h"
#include "../Utils/HNSepCurveProcessor.h"
#include "../Utils/PitchCurveProcessor.h"

#include <algorithm>

bool ProjectSerializer::saveToFile(const Project& project, const juce::File& file) {
    auto json = toJson(project);
    auto jsonString = juce::JSON::toString(json, true); // Pretty print

    return file.replaceWithText(jsonString);
}

bool ProjectSerializer::loadFromFile(Project& project, const juce::File& file) {
    auto jsonString = file.loadFileAsString();
    if (jsonString.isEmpty())
        return false;

    auto json = juce::JSON::parse(jsonString);
    if (!json.isObject())
        return false;

    return fromJson(project, json);
}

juce::var ProjectSerializer::toJson(const Project& project) {
    auto* obj = new juce::DynamicObject();

    // Metadata
    obj->setProperty("formatVersion", FORMAT_VERSION);
    obj->setProperty("name", project.getName());
    obj->setProperty("audioPath", project.getFilePath().getFullPathName());
    obj->setProperty("audioSha256", project.getAudioSha256());

    // Audio settings
    obj->setProperty("sampleRate", project.getAudioData().sampleRate);

    // Global parameters
    obj->setProperty("globalPitchOffset", project.getGlobalPitchOffset());
    obj->setProperty("formantShift", project.getFormantShift());
    obj->setProperty("volume", project.getVolume());
    obj->setProperty("scaleMode", static_cast<int>(project.getScaleMode()));
    obj->setProperty("scaleRootNote", project.getScaleRootNote());
    obj->setProperty("pitchReferenceHz", project.getPitchReferenceHz());
    obj->setProperty("showScaleColors", project.getShowScaleColors());
    obj->setProperty("snapToSemitones", project.getSnapToSemitones());
    obj->setProperty("doubleClickSnapMode",
                     static_cast<int>(project.getDoubleClickSnapMode()));
    obj->setProperty("timelineDisplayMode",
                     static_cast<int>(project.getTimelineDisplayMode()));
    obj->setProperty("timelineBeatNumerator", project.getTimelineBeatNumerator());
    obj->setProperty("timelineBeatDenominator", project.getTimelineBeatDenominator());
    obj->setProperty("timelineTempoBpm", project.getTimelineTempoBpm());
    obj->setProperty("timelineGridDivision",
                     static_cast<int>(project.getTimelineGridDivision()));
    obj->setProperty("timelineSnapCycle", project.getTimelineSnapCycle());

    // Loop range
    const auto& loopRange = project.getLoopRange();
    auto* loopObj = new juce::DynamicObject();
    loopObj->setProperty("enabled", loopRange.enabled);
    loopObj->setProperty("start", loopRange.startSeconds);
    loopObj->setProperty("end", loopRange.endSeconds);
    obj->setProperty("loop", juce::var(loopObj));

    // Notes array
    juce::Array<juce::var> notesArray;
    for (const auto& note : project.getNotes()) {
        notesArray.add(noteToJson(note));
    }
    obj->setProperty("notes", notesArray);

    juce::Array<juce::var> warpMarkersArray;
    for (const auto& marker : project.getWarpMarkers()) {
        auto* markerObj = new juce::DynamicObject();
        markerObj->setProperty("sourceFrame", marker.sourceFrame);
        markerObj->setProperty("outputFrame", marker.outputFrame);
        warpMarkersArray.add(juce::var(markerObj));
    }
    obj->setProperty("warpMarkers", warpMarkersArray);

    // Pitch data
    // Analysis data (original, immutable after detection)
    obj->setProperty("analysisData", analysisDataToJson(project.getAnalysisData()));
    // Edited data (global edit state)
    obj->setProperty("editedData", editedDataToJson(project.getEditedData()));

    return juce::var(obj);
}

bool ProjectSerializer::fromJson(Project& project, const juce::var& json) {
    if (!json.isObject())
        return false;

    // Metadata
    project.setName(json.getProperty("name", "Untitled").toString());
    project.setFilePath(juce::File(json.getProperty("audioPath", "").toString()));
    project.setAudioSha256(json.getProperty("audioSha256", "").toString());

    // Audio settings
    auto& audioData = project.getAudioData();
    audioData.sampleRate = json.getProperty("sampleRate", 44100);

    // Global parameters
    project.setGlobalPitchOffset(static_cast<float>(json.getProperty("globalPitchOffset", 0.0)));
    project.setFormantShift(static_cast<float>(json.getProperty("formantShift", 0.0)));
    project.setVolume(static_cast<float>(json.getProperty("volume", 0.0)));
    {
        const int scaleModeValue = static_cast<int>(json.getProperty(
            "scaleMode", static_cast<int>(ScaleMode::None)));
        if (scaleModeValue >= static_cast<int>(ScaleMode::None) &&
            scaleModeValue <= static_cast<int>(ScaleMode::Locrian))
            project.setScaleMode(static_cast<ScaleMode>(scaleModeValue));
        else
            project.setScaleMode(ScaleMode::None);
    }
    project.setScaleRootNote(static_cast<int>(json.getProperty("scaleRootNote", -1)));
    project.setPitchReferenceHz(static_cast<int>(json.getProperty("pitchReferenceHz", 440)));
    project.setShowScaleColors(static_cast<bool>(
        json.getProperty("showScaleColors", true)));
    project.setSnapToSemitones(static_cast<bool>(
        json.getProperty("snapToSemitones", false)));
    {
        const int snapModeValue = static_cast<int>(json.getProperty(
            "doubleClickSnapMode", static_cast<int>(DoubleClickSnapMode::PitchCenter)));
        if (snapModeValue >= static_cast<int>(DoubleClickSnapMode::PitchCenter) &&
            snapModeValue <= static_cast<int>(DoubleClickSnapMode::NearestScale))
            project.setDoubleClickSnapMode(static_cast<DoubleClickSnapMode>(snapModeValue));
        else
            project.setDoubleClickSnapMode(DoubleClickSnapMode::PitchCenter);
    }
    {
        const int modeValue = static_cast<int>(json.getProperty(
            "timelineDisplayMode", static_cast<int>(TimelineDisplayMode::Beats)));
        if (modeValue >= static_cast<int>(TimelineDisplayMode::Beats) &&
            modeValue <= static_cast<int>(TimelineDisplayMode::Time))
            project.setTimelineDisplayMode(static_cast<TimelineDisplayMode>(modeValue));
        else
            project.setTimelineDisplayMode(TimelineDisplayMode::Beats);
    }
    project.setTimelineBeatSignature(
        static_cast<int>(json.getProperty("timelineBeatNumerator", 4)),
        static_cast<int>(json.getProperty("timelineBeatDenominator", 4)));
    project.setTimelineTempoBpm(
        static_cast<double>(json.getProperty("timelineTempoBpm", 120.0)));
    {
        const int gridValue = static_cast<int>(json.getProperty(
            "timelineGridDivision", static_cast<int>(TimelineGridDivision::Quarter)));
        switch (gridValue)
        {
            case static_cast<int>(TimelineGridDivision::Whole):
            case static_cast<int>(TimelineGridDivision::Half):
            case static_cast<int>(TimelineGridDivision::Quarter):
            case static_cast<int>(TimelineGridDivision::Eighth):
            case static_cast<int>(TimelineGridDivision::Sixteenth):
            case static_cast<int>(TimelineGridDivision::ThirtySecond):
                project.setTimelineGridDivision(static_cast<TimelineGridDivision>(gridValue));
                break;
            default:
                project.setTimelineGridDivision(TimelineGridDivision::Quarter);
                break;
        }
    }
    project.setTimelineSnapCycle(static_cast<bool>(
        json.getProperty("timelineSnapCycle", false)));

    // Loop range
    auto loopVar = json.getProperty("loop", juce::var());
    if (loopVar.isObject()) {
        const double loopStart = loopVar.getProperty("start", 0.0);
        const double loopEnd = loopVar.getProperty("end", 0.0);
        project.setLoopRange(loopStart, loopEnd);
        project.setLoopEnabled(loopVar.getProperty("enabled", false));
    }

    // Notes
    project.clearNotes();
    auto notesVar = json.getProperty("notes", juce::var());
    if (notesVar.isArray()) {
        for (int i = 0; i < notesVar.size(); ++i) {
            Note note;
            if (noteFromJson(note, notesVar[i])) {
                project.addNote(std::move(note));
            }
        }
    }

    project.clearWarpMarkers();
    auto warpMarkersVar = json.getProperty("warpMarkers", juce::var());
    if (warpMarkersVar.isArray()) {
        std::vector<Project::WarpMarker> markers;
        markers.reserve(static_cast<size_t>(warpMarkersVar.size()));
        for (int i = 0; i < warpMarkersVar.size(); ++i) {
            auto markerVar = warpMarkersVar[i];
            if (!markerVar.isObject())
                continue;
            Project::WarpMarker marker;
            marker.sourceFrame =
                static_cast<int>(markerVar.getProperty("sourceFrame", 0));
            marker.outputFrame =
                static_cast<int>(markerVar.getProperty("outputFrame",
                                                       marker.sourceFrame));
            markers.push_back(marker);
        }
        project.setWarpMarkers(std::move(markers));
    }

    // Try new format first
    auto analysisDataVar = json.getProperty("analysisData", juce::var());
    auto editedDataVar = json.getProperty("editedData", juce::var());

    if (analysisDataVar.isObject() && editedDataVar.isObject())
    {
        // New format
        analysisDataFromJson(project.getAnalysisData(), analysisDataVar);
        editedDataFromJson(project.getEditedData(), editedDataVar);

        // Sync to AudioData for backward compat with existing code paths
        auto& ad = project.getAudioData();
        const auto& ed = project.getEditedData();
        ad.f0 = ed.f0;
        ad.baseF0 = ed.f0;
        ad.basePitch = ed.basePitch;
        ad.deltaPitch = ed.deltaPitch;
        ad.voicingCurve = ed.voicingCurve;
        ad.breathCurve = ed.breathCurve;
        ad.tensionCurve = ed.tensionCurve;
        ad.voicedMask = ed.voicedMask;
        ad.vadMask = ed.vadMask;
    }
    else
    {
        // Legacy format
        auto pitchDataVar = json.getProperty("pitchData", juce::var());
        if (pitchDataVar.isObject())
        {
            legacyPitchDataFromJson(audioData, project.getEditedData(),
                                    project.getAnalysisData(), pitchDataVar);
        }
    }

    // Rebuild curves if needed
    if (!audioData.f0.empty() && (audioData.basePitch.empty() || audioData.deltaPitch.empty())) {
        PitchCurveProcessor::rebuildCurvesFromSource(project, audioData.f0);
    }

    // Ensure every note has originalDeltaPitch populated.
    // The serializer may not have persisted it (older format), or
    // rebuildCurvesFromSource was skipped because basePitch/deltaPitch
    // were already loaded from the file.  Extract from global deltaPitch
    // so that rebuildBaseFromNotes() (called during stretch/undo) can
    // resample correctly instead of producing zero delta.
    //
    // originalDeltaPitch must be sized to the *source* duration so that
    // recomputeFromMarkers() can resample it to any output duration.
    // The dense deltaPitch array is indexed by output frame, so we
    // extract from the note's output region then resample back to the
    // source length when the note is stretched.
    {
        const int totalFrames = static_cast<int>(audioData.deltaPitch.size());
        for (auto& note : project.getNotes())
        {
            if (note.isRest() || note.hasOriginalDeltaPitch())
                continue;

            const int outStart = note.getStartFrame();
            const int outEnd = note.getEndFrame();
            const int outFrames = outEnd - outStart;
            if (outFrames <= 0)
                continue;

            // Extract the delta slice at the note's output position
            // (that is where the dense array stores the data).
            std::vector<float> outDelta(static_cast<size_t>(outFrames));
            for (int i = 0; i < outFrames; ++i)
            {
                const int globalIdx = outStart + i;
                if (globalIdx >= 0 && globalIdx < totalFrames)
                    outDelta[static_cast<size_t>(i)] = audioData.deltaPitch[static_cast<size_t>(globalIdx)];
            }

            // originalDeltaPitch should represent the source-length curve.
            // If the note is stretched, resample back to the source duration.
            const int srcFrames = note.getSrcDurationFrames();
            if (srcFrames > 0 && srcFrames != outFrames)
            {
                note.setOriginalDeltaPitch(
                    CurveResampler::resampleLinear(outDelta, srcFrames));
            }
            else
            {
                note.setOriginalDeltaPitch(std::move(outDelta));
            }
        }
    }

    const bool hasMasterHNSep =
        !audioData.voicingCurve.empty() ||
        !audioData.breathCurve.empty() ||
        !audioData.tensionCurve.empty();
    const bool hasNoteHNSep = std::any_of(project.getNotes().begin(),
                                          project.getNotes().end(),
                                          [](const Note& note)
                                          {
                                              return note.hasVoicingCurve() ||
                                                     note.hasBreathCurve() ||
                                                     note.hasTensionCurve() ||
                                                     note.hasSourceVoicingCurve() ||
                                                     note.hasSourceBreathCurve() ||
                                                     note.hasSourceTensionCurve();
                                          });

    if (hasNoteHNSep)
        HNSepCurveProcessor::rebuildCurvesFromNotes(project);
    else if (hasMasterHNSep)
        HNSepCurveProcessor::extractNoteCurvesFromMaster(project);
    else
        HNSepCurveProcessor::initializeCurves(project);

    project.setModified(false);
    return true;
}

juce::var ProjectSerializer::noteToJson(const Note& note) {
    auto* obj = new juce::DynamicObject();

    obj->setProperty("startFrame", note.getStartFrame());
    obj->setProperty("endFrame", note.getEndFrame());
    obj->setProperty("srcStartFrame", note.getSrcStartFrame());
    obj->setProperty("srcEndFrame", note.getSrcEndFrame());
    obj->setProperty("midiNote", note.getMidiNote());
    obj->setProperty("pitchOffset", note.getPitchOffset());
    obj->setProperty("volumeDb", note.getVolumeDb());
    obj->setProperty("rest", note.isRest());

    // Vibrato
    auto* vibrato = new juce::DynamicObject();
    vibrato->setProperty("enabled", note.isVibratoEnabled());
    vibrato->setProperty("rateHz", note.getVibratoRateHz());
    vibrato->setProperty("depthSemitones", note.getVibratoDepthSemitones());
    vibrato->setProperty("phaseRadians", note.getVibratoPhaseRadians());
    vibrato->setProperty("mix", note.getVibratoMix());
    vibrato->setProperty("fadeInMs", note.getVibratoFadeInMs());
    vibrato->setProperty("fadeOutMs", note.getVibratoFadeOutMs());
    obj->setProperty("vibrato", juce::var(vibrato));

    // Lyric/Phoneme
    if (note.hasLyric())
        obj->setProperty("lyric", note.getLyric());
    if (note.hasPhoneme())
        obj->setProperty("phoneme", note.getPhoneme());

    // Pitch tool transformation parameters (non-destructive)
    obj->setProperty("tiltLeft", note.getTiltLeft());
    obj->setProperty("tiltRight", note.getTiltRight());
    obj->setProperty("varianceScale", note.getVarianceScale());
    obj->setProperty("smoothLeftFrames", note.getSmoothLeftFrames());
    obj->setProperty("smoothRightFrames", note.getSmoothRightFrames());

    // Per-note original F0 values (source-aligned, for voiced mask reconstruction)
    if (!note.getF0Values().empty())
        obj->setProperty("f0Values", floatArrayToString(note.getF0Values(), 2));

    // Per-note original delta pitch (pristine curve from analysis)
    if (note.hasOriginalDeltaPitch())
        obj->setProperty("originalDeltaPitch", floatArrayToString(note.getOriginalDeltaPitch(), 4));

    // Per-note delta scale/offset
    if (std::abs(note.getDeltaScale() - 1.0f) > 0.0001f)
        obj->setProperty("deltaScale", note.getDeltaScale());
    if (std::abs(note.getDeltaOffset()) > 0.0001f)
        obj->setProperty("deltaOffset", note.getDeltaOffset());
    if (std::abs(note.getHighPassFilterStrength()) > 0.0001f)
        obj->setProperty("highPassFilterStrength", note.getHighPassFilterStrength());
    if (std::abs(note.getLowPassFilterStrength()) > 0.0001f)
        obj->setProperty("lowPassFilterStrength", note.getLowPassFilterStrength());

    // Harmonic-noise separation curves (voicing/breath/tension)
    if (note.hasVoicingCurve())
        obj->setProperty("voicingCurve", floatArrayToString(note.getVoicingCurve(), 2));
    if (note.hasBreathCurve())
        obj->setProperty("breathCurve", floatArrayToString(note.getBreathCurve(), 2));
    if (note.hasTensionCurve())
        obj->setProperty("tensionCurve", floatArrayToString(note.getTensionCurve(), 2));
    if (note.hasSourceVoicingCurve())
        obj->setProperty("sourceVoicingCurve",
                         floatArrayToString(note.getSourceVoicingCurve(), 2));
    if (note.hasSourceBreathCurve())
        obj->setProperty("sourceBreathCurve",
                         floatArrayToString(note.getSourceBreathCurve(), 2));
    if (note.hasSourceTensionCurve())
        obj->setProperty("sourceTensionCurve",
                         floatArrayToString(note.getSourceTensionCurve(), 2));

    return juce::var(obj);
}

bool ProjectSerializer::noteFromJson(Note& note, const juce::var& json) {
    if (!json.isObject())
        return false;

    const int startFrame = json.getProperty("startFrame", 0);
    const int endFrame = json.getProperty("endFrame", 0);
    note.setStartFrame(startFrame);
    note.setEndFrame(endFrame);
    // Backward compat: if srcStartFrame/srcEndFrame not in file, default to startFrame/endFrame
    note.setSrcStartFrame(static_cast<int>(json.getProperty("srcStartFrame", startFrame)));
    note.setSrcEndFrame(static_cast<int>(json.getProperty("srcEndFrame", endFrame)));
    note.setMidiNote(static_cast<float>(json.getProperty("midiNote", 60.0)));
    note.setPitchOffset(static_cast<float>(json.getProperty("pitchOffset", 0.0)));
    note.setVolumeDb(static_cast<float>(json.getProperty("volumeDb", 0.0)));
    note.setRest(json.getProperty("rest", false));

    // Vibrato
    auto vibratoVar = json.getProperty("vibrato", juce::var());
    if (vibratoVar.isObject()) {
        note.setVibratoEnabled(vibratoVar.getProperty("enabled", false));
        note.setVibratoRateHz(static_cast<float>(vibratoVar.getProperty("rateHz", 5.0)));
        note.setVibratoDepthSemitones(static_cast<float>(vibratoVar.getProperty("depthSemitones", 0.0)));
        note.setVibratoPhaseRadians(static_cast<float>(vibratoVar.getProperty("phaseRadians", 0.0)));
        note.setVibratoMix(static_cast<float>(vibratoVar.getProperty("mix", 0.0)));
        note.setVibratoFadeInMs(static_cast<float>(vibratoVar.getProperty("fadeInMs", 0.0)));
        note.setVibratoFadeOutMs(static_cast<float>(vibratoVar.getProperty("fadeOutMs", 0.0)));
    }

    // Lyric/Phoneme
    auto lyric = json.getProperty("lyric", juce::var());
    if (!lyric.isVoid())
        note.setLyric(lyric.toString());

    auto phoneme = json.getProperty("phoneme", juce::var());
    if (!phoneme.isVoid())
        note.setPhoneme(phoneme.toString());

    // Pitch tool transformation parameters (with defaults for backwards compatibility)
    note.setTiltLeft(static_cast<float>(json.getProperty("tiltLeft", 0.0)));
    note.setTiltRight(static_cast<float>(json.getProperty("tiltRight", 0.0)));
    note.setVarianceScale(static_cast<float>(json.getProperty("varianceScale", 1.0)));
    note.setSmoothLeftFrames(json.getProperty("smoothLeftFrames", 0));
    note.setSmoothRightFrames(json.getProperty("smoothRightFrames", 0));

    // Per-note original F0 values (source-aligned, for voiced mask reconstruction)
    auto f0ValuesStr = json.getProperty("f0Values", juce::var());
    if (!f0ValuesStr.isVoid() && f0ValuesStr.toString().isNotEmpty())
        note.setF0Values(stringToFloatArray(f0ValuesStr.toString()));

    // Per-note original delta pitch (pristine curve from analysis)
    auto origDeltaStr = json.getProperty("originalDeltaPitch", juce::var());
    if (!origDeltaStr.isVoid() && origDeltaStr.toString().isNotEmpty())
        note.setOriginalDeltaPitch(stringToFloatArray(origDeltaStr.toString()));

    // Per-note delta scale/offset
    note.setDeltaScale(static_cast<float>(json.getProperty("deltaScale", 1.0)));
    note.setDeltaOffset(static_cast<float>(json.getProperty("deltaOffset", 0.0)));
    note.setHighPassFilterStrength(
        static_cast<float>(json.getProperty("highPassFilterStrength", 0.0)));
    note.setLowPassFilterStrength(
        static_cast<float>(json.getProperty("lowPassFilterStrength", 0.0)));

    // Harmonic-noise separation curves (voicing/breath/tension)
    auto voicingStr = json.getProperty("voicingCurve", juce::var());
    if (!voicingStr.isVoid() && voicingStr.toString().isNotEmpty())
        note.setVoicingCurve(stringToFloatArray(voicingStr.toString()));

    auto breathStr = json.getProperty("breathCurve", juce::var());
    if (!breathStr.isVoid() && breathStr.toString().isNotEmpty())
        note.setBreathCurve(stringToFloatArray(breathStr.toString()));

    auto tensionStr = json.getProperty("tensionCurve", juce::var());
    if (!tensionStr.isVoid() && tensionStr.toString().isNotEmpty())
        note.setTensionCurve(stringToFloatArray(tensionStr.toString()));

    auto sourceVoicingStr = json.getProperty("sourceVoicingCurve", juce::var());
    if (!sourceVoicingStr.isVoid() && sourceVoicingStr.toString().isNotEmpty())
        note.setSourceVoicingCurve(
            stringToFloatArray(sourceVoicingStr.toString()));
    else if (note.hasVoicingCurve())
        note.setSourceVoicingCurve(note.getVoicingCurve());

    auto sourceBreathStr = json.getProperty("sourceBreathCurve", juce::var());
    if (!sourceBreathStr.isVoid() && sourceBreathStr.toString().isNotEmpty())
        note.setSourceBreathCurve(
            stringToFloatArray(sourceBreathStr.toString()));
    else if (note.hasBreathCurve())
        note.setSourceBreathCurve(note.getBreathCurve());

    auto sourceTensionStr = json.getProperty("sourceTensionCurve", juce::var());
    if (!sourceTensionStr.isVoid() && sourceTensionStr.toString().isNotEmpty())
        note.setSourceTensionCurve(
            stringToFloatArray(sourceTensionStr.toString()));
    else if (note.hasTensionCurve())
        note.setSourceTensionCurve(note.getTensionCurve());

    return true;
}

juce::var ProjectSerializer::analysisDataToJson(const AnalysisData& data)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("originalF0", floatArrayToString(data.originalF0, 2));
    obj->setProperty("originalPitch", floatArrayToString(data.originalPitch, 4));
    obj->setProperty("originalDeltaPitch", floatArrayToString(data.originalDeltaPitch, 4));
    obj->setProperty("originalVoicedMask", boolArrayToString(data.originalVoicedMask));
    obj->setProperty("originalVADMask", boolArrayToString(data.originalVADMask));
    return juce::var(obj);
}

bool ProjectSerializer::analysisDataFromJson(AnalysisData& data, const juce::var& json)
{
    if (!json.isObject())
        return false;
    data.originalF0 = stringToFloatArray(json.getProperty("originalF0", "").toString());
    data.originalPitch = stringToFloatArray(json.getProperty("originalPitch", "").toString());
    data.originalDeltaPitch = stringToFloatArray(json.getProperty("originalDeltaPitch", "").toString());
    data.originalVoicedMask = stringToBoolArray(json.getProperty("originalVoicedMask", "").toString());
    data.originalVADMask = stringToBoolArray(json.getProperty("originalVADMask", "").toString());
    return true;
}

juce::var ProjectSerializer::editedDataToJson(const EditedData& data)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty("basePitch", floatArrayToString(data.basePitch, 4));
    obj->setProperty("deltaPitch", floatArrayToString(data.deltaPitch, 4));
    obj->setProperty("f0", floatArrayToString(data.f0, 2));
    obj->setProperty("voicedMask", boolArrayToString(data.voicedMask));
    obj->setProperty("vadMask", boolArrayToString(data.vadMask));
    obj->setProperty("voicingCurve", floatArrayToString(data.voicingCurve, 2));
    obj->setProperty("breathCurve", floatArrayToString(data.breathCurve, 2));
    obj->setProperty("tensionCurve", floatArrayToString(data.tensionCurve, 2));
    return juce::var(obj);
}

bool ProjectSerializer::editedDataFromJson(EditedData& data, const juce::var& json)
{
    if (!json.isObject())
        return false;
    data.basePitch = stringToFloatArray(json.getProperty("basePitch", "").toString());
    data.deltaPitch = stringToFloatArray(json.getProperty("deltaPitch", "").toString());
    data.f0 = stringToFloatArray(json.getProperty("f0", "").toString());
    data.voicedMask = stringToBoolArray(json.getProperty("voicedMask", "").toString());
    data.vadMask = stringToBoolArray(json.getProperty("vadMask", "").toString());
    data.voicingCurve = stringToFloatArray(json.getProperty("voicingCurve", "").toString());
    data.breathCurve = stringToFloatArray(json.getProperty("breathCurve", "").toString());
    data.tensionCurve = stringToFloatArray(json.getProperty("tensionCurve", "").toString());
    return true;
}

bool ProjectSerializer::legacyPitchDataFromJson(AudioData& audioData,
                                                 EditedData& editedData,
                                                 AnalysisData& analysisData,
                                                 const juce::var& json)
{
    if (!json.isObject())
        return false;

    // Old format stored everything flat in pitchData
    editedData.f0 = stringToFloatArray(json.getProperty("f0", "").toString());
    editedData.basePitch = stringToFloatArray(json.getProperty("basePitch", "").toString());
    editedData.deltaPitch = stringToFloatArray(json.getProperty("deltaPitch", "").toString());
    editedData.voicingCurve = stringToFloatArray(json.getProperty("voicingCurve", "").toString());
    editedData.breathCurve = stringToFloatArray(json.getProperty("breathCurve", "").toString());
    editedData.tensionCurve = stringToFloatArray(json.getProperty("tensionCurve", "").toString());
    editedData.voicedMask = stringToBoolArray(json.getProperty("voicedMask", "").toString());
    editedData.vadMask = stringToBoolArray(json.getProperty("vadMask", "").toString());

    // Also populate AudioData for backward compat with existing code paths
    audioData.f0 = editedData.f0;
    audioData.baseF0 = editedData.f0;
    audioData.basePitch = editedData.basePitch;
    audioData.deltaPitch = editedData.deltaPitch;
    audioData.voicingCurve = editedData.voicingCurve;
    audioData.breathCurve = editedData.breathCurve;
    audioData.tensionCurve = editedData.tensionCurve;
    audioData.voicedMask = editedData.voicedMask;
    audioData.vadMask = editedData.vadMask;
    audioData.f0EditedMask = stringToBoolArray(json.getProperty("f0EditedMask", "").toString());

    // For legacy files, analysis data = initial edited data (best we can do)
    analysisData.originalF0 = editedData.f0;
    analysisData.originalPitch = editedData.basePitch;
    analysisData.originalDeltaPitch = editedData.deltaPitch;
    analysisData.originalVoicedMask = editedData.voicedMask;
    analysisData.originalVADMask = editedData.vadMask;

    return true;
}

juce::String ProjectSerializer::floatArrayToString(const std::vector<float>& arr, int precision) {
    if (arr.empty())
        return {};

    juce::StringArray parts;
    parts.ensureStorageAllocated(static_cast<int>(arr.size()));

    for (float v : arr) {
        parts.add(juce::String(v, precision));
    }

    return parts.joinIntoString(" ");
}

std::vector<float> ProjectSerializer::stringToFloatArray(const juce::String& str) {
    if (str.isEmpty())
        return {};

    juce::StringArray parts;
    parts.addTokens(str, " ", "");

    std::vector<float> result;
    result.reserve(static_cast<size_t>(parts.size()));

    for (const auto& p : parts) {
        if (p.isNotEmpty())
            result.push_back(p.getFloatValue());
    }

    return result;
}

juce::String ProjectSerializer::boolArrayToString(const std::vector<bool>& arr) {
    if (arr.empty())
        return {};

    juce::String result;
    result.preallocateBytes(arr.size());

    for (bool b : arr) {
        result << (b ? '1' : '0');
    }

    return result;
}

std::vector<bool> ProjectSerializer::stringToBoolArray(const juce::String& str) {
    if (str.isEmpty())
        return {};

    std::vector<bool> result;
    result.reserve(static_cast<size_t>(str.length()));

    for (int i = 0; i < str.length(); ++i) {
        result.push_back(str[i] == '1');
    }

    return result;
}
