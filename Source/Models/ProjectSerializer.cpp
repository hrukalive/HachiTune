#include "ProjectSerializer.h"
#include "../Utils/CurveResampler.h"
#include "../Utils/HNSepCurveProcessor.h"
#include "../Utils/PitchCurveProcessor.h"
#include "../Utils/WarpMarkerProcessor.h"

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
    const auto markersToSave =
        WarpMarkerProcessor::buildWarpMapWithEndpoints(
            project, project.getWarpMarkers());
    for (const auto& marker : markersToSave) {
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
    project.getAnalysisData().clear();
    project.getEditedData().clear();

    if (analysisDataVar.isObject() && editedDataVar.isObject())
    {
        // New format
        analysisDataFromJson(project.getAnalysisData(), analysisDataVar);
        editedDataFromJson(project.getEditedData(), editedDataVar);
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

    int segIndex = 0;
    for (auto& note : project.getNotes())
    {
        if (note.isRest())
            continue;
        if (segIndex < static_cast<int>(project.getAnalysisData().noteSegments.size()))
        {
            const auto& seg =
                project.getAnalysisData().noteSegments[static_cast<size_t>(segIndex)];
            note.setSrcStartFrame(seg.srcStartFrame);
            note.setSrcEndFrame(seg.srcEndFrame);
        }
        ++segIndex;
    }

    // Rebuild curves if needed
    auto& editedData2 = project.getEditedData();
    if (!editedData2.f0.empty() && (editedData2.basePitch.empty() || editedData2.deltaPitch.empty())) {
        PitchCurveProcessor::rebuildCurvesFromSource(project, editedData2.f0);
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
        const int totalFrames = static_cast<int>(editedData2.deltaPitch.size());
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
                    outDelta[static_cast<size_t>(i)] = editedData2.deltaPitch[static_cast<size_t>(globalIdx)];
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
        !editedData2.voicingCurve.empty() ||
        !editedData2.breathCurve.empty() ||
        !editedData2.tensionCurve.empty();
    const bool hasNoteHNSep = std::any_of(project.getNotes().begin(),
                                          project.getNotes().end(),
                                          [](const Note& note)
                                          {
                                              return note.hasVoicingCurve() ||
                                                     note.hasBreathCurve() ||
                                                     note.hasTensionCurve();
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
    vibrato->setProperty("startFrame", note.getVibratoStartFrame());
    vibrato->setProperty("lengthFrames", note.getVibratoLengthFrames());
    vibrato->setProperty("fadeInFrames", note.getVibratoFadeInFrames());
    vibrato->setProperty("fadeOutFrames", note.getVibratoFadeOutFrames());
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

    if (std::abs(note.getHighPassFilterStrength()) > 0.0001f)
        obj->setProperty("highPassFilterStrength",
                         note.getHighPassFilterStrength());
    if (std::abs(note.getLowPassFilterStrength()) > 0.0001f)
        obj->setProperty("lowPassFilterStrength",
                         note.getLowPassFilterStrength());

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
        note.setVibratoStartFrame(static_cast<int>(vibratoVar.getProperty("startFrame", 0)));
        note.setVibratoLengthFrames(static_cast<int>(vibratoVar.getProperty("lengthFrames", 0)));
        note.setVibratoFadeInFrames(static_cast<int>(vibratoVar.getProperty("fadeInFrames", 0)));
        note.setVibratoFadeOutFrames(static_cast<int>(vibratoVar.getProperty("fadeOutFrames", 0)));
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

    // Per-note original F0 values: read and discard (backward compatibility only)
    // f0Values are no longer stored per-note; use analysisData.originalF0 instead.
    // (no action needed)

    // Per-note original delta pitch (pristine curve from analysis)
    auto origDeltaStr = json.getProperty("originalDeltaPitch", juce::var());
    if (!origDeltaStr.isVoid() && origDeltaStr.toString().isNotEmpty())
        note.setOriginalDeltaPitch(stringToFloatArray(origDeltaStr.toString()));

    // Per-note delta scale/offset (read for backward compat, discarded)
    // note.setDeltaScale(static_cast<float>(json.getProperty("deltaScale", 1.0)));
    // note.setDeltaOffset(static_cast<float>(json.getProperty("deltaOffset", 0.0)));
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
    // Read for backward compatibility, discard (source curves removed).
    (void)sourceVoicingStr;

    auto sourceBreathStr = json.getProperty("sourceBreathCurve", juce::var());
    // Read for backward compatibility, discard.
    (void)sourceBreathStr;

    auto sourceTensionStr = json.getProperty("sourceTensionCurve", juce::var());
    // Read for backward compatibility, discard.
    (void)sourceTensionStr;

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
    juce::var segments;
    for (const auto& seg : data.noteSegments)
    {
        auto* segObj = new juce::DynamicObject();
        segObj->setProperty("srcStartFrame", seg.srcStartFrame);
        segObj->setProperty("srcEndFrame", seg.srcEndFrame);
        segments.append(juce::var(segObj));
    }
    obj->setProperty("noteSegments", segments);
    return juce::var(obj);
}

bool ProjectSerializer::analysisDataFromJson(AnalysisData& data, const juce::var& json)
{
    if (!json.isObject())
        return false;
    data.clear();
    data.originalF0 = stringToFloatArray(json.getProperty("originalF0", "").toString());
    data.originalPitch = stringToFloatArray(json.getProperty("originalPitch", "").toString());
    data.originalDeltaPitch = stringToFloatArray(json.getProperty("originalDeltaPitch", "").toString());
    data.originalVoicedMask = stringToBoolArray(json.getProperty("originalVoicedMask", "").toString());
    data.originalVADMask = stringToBoolArray(json.getProperty("originalVADMask", "").toString());
    if (auto* segsArray = json["noteSegments"].getArray())
    {
        for (const auto& item : *segsArray)
        {
            AnalysisData::NoteSegment seg;
            seg.srcStartFrame = item.getProperty("srcStartFrame", 0);
            seg.srcEndFrame = item.getProperty("srcEndFrame", 0);
            data.noteSegments.push_back(seg);
        }
    }
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
    analysisData.clear();

    // Old format stored everything flat in pitchData
    editedData.f0 = stringToFloatArray(json.getProperty("f0", "").toString());
    editedData.basePitch = stringToFloatArray(json.getProperty("basePitch", "").toString());
    editedData.deltaPitch = stringToFloatArray(json.getProperty("deltaPitch", "").toString());
    editedData.voicingCurve = stringToFloatArray(json.getProperty("voicingCurve", "").toString());
    editedData.breathCurve = stringToFloatArray(json.getProperty("breathCurve", "").toString());
    editedData.tensionCurve = stringToFloatArray(json.getProperty("tensionCurve", "").toString());
    editedData.voicedMask = stringToBoolArray(json.getProperty("voicedMask", "").toString());
    editedData.vadMask = stringToBoolArray(json.getProperty("vadMask", "").toString());

    // Legacy audioData fields removed — data now lives only in editedData

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
