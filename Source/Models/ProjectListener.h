#pragma once

/**
 * Event types for Project state changes.
 */
enum class ProjectChangeType
{
  NoteListChanged,       // note add/remove/split
  NotePitchChanged,      // midiNote / pitchOffset / deltaPitch modification
  NoteCurveChanged,      // voicing / breath / tension curve modification
  NotePropertyChanged,   // vibrato, tool params, volumeDb, etc.
  NoteSelectionChanged,  // selection state change
  WarpChanged,           // warp marker modification
  GlobalParamChanged,    // globalPitchOffset / formantShift / volume
  EditedDataChanged,     // global editedData bulk update (e.g. after stretch)
  SettingsChanged,       // scale / timeline / loop UI settings
  AudioDataChanged,      // audio load / analysis complete
  SynthesisComplete      // incremental synthesis complete, waveform updated
};

/**
 * Interface for objects that want to be notified of Project changes.
 * Register with Project::addListener() / removeListener().
 */
class ProjectListener
{
public:
  virtual ~ProjectListener() = default;

  /**
   * Called when the project state changes.
   *
   * @param type               Category of change
   * @param affectedNoteIndex  Index for single-note ops (-1 = all/N/A)
   * @param rangeStart         Start frame of affected range (-1 = N/A)
   * @param rangeEnd           End frame of affected range (-1 = N/A)
   */
  virtual void onProjectChanged(ProjectChangeType type,
                                int affectedNoteIndex = -1,
                                int rangeStart = -1,
                                int rangeEnd = -1) = 0;
};
