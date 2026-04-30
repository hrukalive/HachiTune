#pragma once

#include "../JuceHeader.h"
#include "AnalysisData.h"
#include "EditedData.h"
#include "Project.h"

/**
 * Handles Project serialization to/from JSON format.
 *
 * Design principles:
 * - Decoupled from Project class (Project doesn't know about serialization details)
 * - Uses JUCE's built-in JSON support (no external dependencies)
 * - Stateless utility class
 */
class ProjectSerializer {
public:
    static constexpr int FORMAT_VERSION = 2;

    /**
     * Save project to JSON file.
     */
    static bool saveToFile(const Project& project, const juce::File& file);

    /**
     * Load project from JSON file.
     */
    static bool loadFromFile(Project& project, const juce::File& file);

    /**
     * Convert project to JSON object.
     */
    static juce::var toJson(const Project& project);

    /**
     * Load project from JSON object.
     */
    static bool fromJson(Project& project, const juce::var& json);

private:
    // Note serialization
    static juce::var noteToJson(const Note& note);
    static bool noteFromJson(Note& note, const juce::var& json);

    // New: analysisData + editedData serialization
    static juce::var analysisDataToJson(const AnalysisData& data);
    static bool analysisDataFromJson(AnalysisData& data, const juce::var& json);
    static juce::var editedDataToJson(const EditedData& data);
    static bool editedDataFromJson(EditedData& data, const juce::var& json);

    // Legacy: pitchData backward compat (read only)
    static bool legacyPitchDataFromJson(AudioData& audioData,
                                        EditedData& editedData,
                                        AnalysisData& analysisData,
                                        const juce::var& json);

    // Array helpers (compact string format)
    static juce::String floatArrayToString(const std::vector<float>& arr, int precision = 4);
    static std::vector<float> stringToFloatArray(const juce::String& str);
    static juce::String boolArrayToString(const std::vector<bool>& arr);
    static std::vector<bool> stringToBoolArray(const juce::String& str);

    ProjectSerializer() = delete;
};
