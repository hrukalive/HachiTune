#pragma once

#include "../Models/Project.h"

namespace HNSepCurveProcessor
{
    constexpr float kDefaultVoicing = 100.0f;
    constexpr float kDefaultBreath = 100.0f;
    constexpr float kDefaultTension = 0.0f;

    /**
     * Ensure dense hnsep master curves exist in AudioData and that each note has
     * a note-local editable copy. This is the hnsep counterpart to building the
     * dense base/delta pitch curves after analysis.
     */
    void initializeCurves(Project& project);

    /**
     * Rebuild the dense master curves in AudioData from the current note-local
     * editable copies. Note curves are resampled to the note's current output
     * duration so stretch operations keep hnsep edits aligned.
     */
    void rebuildCurvesFromNotes(Project& project);

    /**
     * Partial rebuild for a specific global frame range.
     * Only the affected dense master frames are rewritten.
     */
    void rebuildCurvesForRange(Project& project, int startFrame, int endFrame);

    /**
     * Backfill note-local editable copies from existing dense AudioData curves.
     * This is primarily used for project loading / backward compatibility.
     */
    void extractNoteCurvesFromMaster(Project& project);

    /**
     * Returns true when any dense hnsep control in [startFrame, endFrame) differs
     * from the neutral defaults and therefore requires waveform/mel regeneration.
     */
    bool hasActiveEdits(const Project& project, int startFrame, int endFrame);

    /**
     * For each non-rest note that lacks per-note harmonic/noise clip waveforms,
     * slice them from the global AudioData::harmonicWaveform / noiseWaveform
     * using the note's source frame range.  This is needed after HNSep
     * separation runs (e.g. on project reload) so that the per-note mel
     * override path in IncrementalSynthesizer can apply tension/voicing/breath.
     */
    void ensureNoteHNClips(Project& project);

    /**
     * For each non-rest note overlapping [startFrame, endFrame), run the
     * TensionProcessor at source rate using per-note harmonic/noise clips and
     * source-duration HNSep curves, recompute mel from the processed audio,
     * write it into AudioData::sourceMelSpectrogram at source-frame positions,
     * and rebuild AudioData::melSpectrogram from the current warp map.
     *
     * Call this after rebuildCurvesForRange() when hasActiveEdits() is true to
     * keep the global mel in sync with current voicing/breath/tension edits.
     * Calls syncHNSepToEditedData() on completion.
     */
    void recomputeMelForRange(Project& project, int startFrame, int endFrame);
} // namespace HNSepCurveProcessor
