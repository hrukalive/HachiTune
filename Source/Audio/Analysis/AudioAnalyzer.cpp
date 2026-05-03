#include "AudioAnalyzer.h"
#include "../../Utils/Constants.h"
#include "../../Utils/HNSepCurveProcessor.h"
#include "../../Utils/PlatformPaths.h"
#include <climits>

AudioAnalyzer::AudioAnalyzer() = default;

AudioAnalyzer::~AudioAnalyzer()
{
  cancelFlag = true;
  if (analysisThread.joinable())
    analysisThread.join();
}

void AudioAnalyzer::initialize()
{
  // Try to load RMVPE model (default)
  auto rmvpeModelPath =
      PlatformPaths::getModelsDirectory().getChildFile("rmvpe.onnx");
  if (rmvpeModelPath.existsAsFile())
  {
    rmvpeDetector = std::make_unique<RMVPEPitchDetector>();
    if (rmvpeDetector->loadModel(rmvpeModelPath))
    {
    }
    else
    {
      rmvpeDetector.reset();
    }
  }
  else
  {
  }

  // Try to load FCPE model
  auto fcpeModelPath =
      PlatformPaths::getModelsDirectory().getChildFile("fcpe.onnx");
  if (fcpeModelPath.existsAsFile())
  {
    fcpeDetector = std::make_unique<FCPEPitchDetector>();
    if (fcpeDetector->loadModel(fcpeModelPath))
    {
    }
    else
    {
      fcpeDetector.reset();
    }
  }
  else
  {
  }

  // Try to load GAME models
  auto gameModelDir =
      PlatformPaths::getModelSubDir("GAME", "encoder.onnx");
  if (gameModelDir.isDirectory())
  {
    gameDetector = std::make_unique<GAMEDetector>();
    if (gameDetector->loadModels(gameModelDir))
    {
    }
    else
    {
      gameDetector.reset();
    }
  }
}

bool AudioAnalyzer::isFCPEAvailable() const
{
  auto *detector = fcpeDetector ? fcpeDetector.get() : externalFCPEDetector;
  return detector && detector->isLoaded();
}

bool AudioAnalyzer::isRMVPEAvailable() const
{
  auto *detector = rmvpeDetector ? rmvpeDetector.get() : externalRMVPEDetector;
  return detector && detector->isLoaded();
}

void AudioAnalyzer::analyze(Project &project, ProgressCallback onProgress,
                            CompleteCallback onComplete)
{
  auto &audioData = project.getAudioData();
  auto &editedData = project.getEditedData();
  if (audioData.waveform.getNumSamples() == 0)
    return;

  const float *samples = audioData.waveform.getReadPointer(0);
  int numSamples = audioData.waveform.getNumSamples();

  // Compute mel spectrogram
  if (onProgress)
    onProgress(0.35, "Computing mel spectrogram...");
  MelSpectrogram melComputer(audioData.sampleRate, N_FFT, HOP_SIZE, NUM_MELS,
                             FMIN, FMAX);
  auto& analysisData = project.getAnalysisData();
  analysisData.originalMel = melComputer.compute(samples, numSamples);
  editedData.adjustedMel = analysisData.originalMel;
  editedData.mel = editedData.adjustedMel;

  int targetFrames = static_cast<int>(editedData.mel.size());

  if (cancelFlag.load())
    return;

  // Extract F0 based on selected detector type
  if (onProgress)
    onProgress(0.55, "Extracting pitch (F0)...");

  bool extracted = false;

  // Try selected detector first
  if (detectorType == PitchDetectorType::RMVPE && isRMVPEAvailable())
  {
    extractF0WithRMVPE(project, targetFrames);
    extracted = true;
  }
  else if (detectorType == PitchDetectorType::FCPE && isFCPEAvailable())
  {
    extractF0WithFCPE(project, targetFrames);
    extracted = true;
  }

  // Fallback chain: RMVPE -> FCPE
  if (!extracted)
  {
    if (isRMVPEAvailable())
    {
    extractF0WithRMVPE(project, targetFrames);
    }
    else if (isFCPEAvailable())
    {
    extractF0WithFCPE(project, targetFrames);
    }
  }

  if (cancelFlag.load())
    return;

  // Smooth F0
  if (onProgress)
    onProgress(0.65, "Smoothing pitch curve...");
  editedData.f0 = F0Smoother::smoothF0(editedData.f0, editedData.voicedMask);
  editedData.f0 = PitchCurveProcessor::interpolateWithUvMask(
      editedData.f0, editedData.voicedMask);

  if (cancelFlag.load())
    return;

  // Compute energy-based VAD mask (captures consonants that F0 detectors miss)
  computeVadMask(project);

  // Segment into notes
  if (onProgress)
    onProgress(0.90, "Segmenting notes...");
  segmentIntoNotes(project);

  // Populate noteSegments in AnalysisData from the finalized notes
  {
    auto& analysisData = project.getAnalysisData();
    analysisData.noteSegments.clear();
    for (const auto& note : project.getNotes())
    {
      if (!note.isRest())
        analysisData.noteSegments.push_back({note.getSrcStartFrame(), note.getSrcEndFrame()});
    }
  }

  // Build dense base/delta curves
  PitchCurveProcessor::rebuildCurvesFromSource(project, editedData.f0);
  HNSepCurveProcessor::initializeCurves(project);

  if (onComplete)
    onComplete();
}

void AudioAnalyzer::analyzeAsync(std::shared_ptr<Project> project,
                                 ProgressCallback onProgress,
                                 CompleteCallback onComplete)
{
  if (isRunning.load())
    return;

  if (!project)
    return;

  cancelFlag = false;
  isRunning = true;

  if (analysisThread.joinable())
    analysisThread.join();

  analysisThread = std::thread(
      [this, project = std::move(project), onProgress, onComplete]() mutable
      {
        analyze(*project, onProgress, [this, onComplete]()
                {
          isRunning = false;
          if (onComplete)
            onComplete(); });
        isRunning = false;
      });
}

void AudioAnalyzer::extractF0WithRMVPE(Project &project, int targetFrames)
{
  auto &audioData = project.getAudioData();
  auto &editedData = project.getEditedData();
  const float *samples = audioData.waveform.getReadPointer(0);
  int numSamples = audioData.waveform.getNumSamples();

  auto *detector = rmvpeDetector ? rmvpeDetector.get() : externalRMVPEDetector;
  std::vector<float> rmvpeF0 =
      detector->extractF0(samples, numSamples, audioData.sampleRate);

  if (!rmvpeF0.empty() && targetFrames > 0)
  {
    editedData.f0.resize(targetFrames);

    // Time per frame for each system
    const double rmvpeFrameTime = 160.0 / 16000.0; // 0.01 seconds
    const double vocoderFrameTime =
        static_cast<double>(HOP_SIZE) /
        static_cast<double>(std::max(1, audioData.sampleRate));

    for (int i = 0; i < targetFrames; ++i)
    {
      double vocoderTime = i * vocoderFrameTime;
      double rmvpeFramePos = vocoderTime / rmvpeFrameTime;
      int srcIdx = static_cast<int>(rmvpeFramePos);
      double frac = rmvpeFramePos - srcIdx;

      if (srcIdx + 1 < static_cast<int>(rmvpeF0.size()))
      {
        float f0_a = rmvpeF0[srcIdx];
        float f0_b = rmvpeF0[srcIdx + 1];

        if (f0_a > 0.0f && f0_b > 0.0f)
        {
          // Log-domain interpolation for musical accuracy
          float logF0_a = std::log(f0_a);
          float logF0_b = std::log(f0_b);
          float logF0_interp = logF0_a * (1.0 - frac) + logF0_b * frac;
          editedData.f0[i] = std::exp(logF0_interp);
        }
        else if (f0_a > 0.0f)
        {
          editedData.f0[i] = f0_a;
        }
        else if (f0_b > 0.0f)
        {
          editedData.f0[i] = f0_b;
        }
        else
        {
          editedData.f0[i] = 0.0f;
        }
      }
      else if (srcIdx < static_cast<int>(rmvpeF0.size()))
      {
        editedData.f0[i] = rmvpeF0[srcIdx];
      }
      else
      {
        editedData.f0[i] = rmvpeF0.back() > 0.0f ? rmvpeF0.back() : 0.0f;
      }
    }
  }
  else
  {
    editedData.f0.clear();
  }

  // Create voiced mask
  editedData.voicedMask.resize(editedData.f0.size());
  for (size_t i = 0; i < editedData.f0.size(); ++i)
  {
    editedData.voicedMask[i] = editedData.f0[i] > 0;
  }
}

void AudioAnalyzer::extractF0WithFCPE(Project &project, int targetFrames)
{
  auto &audioData = project.getAudioData();
  auto &editedData = project.getEditedData();
  const float *samples = audioData.waveform.getReadPointer(0);
  int numSamples = audioData.waveform.getNumSamples();

  auto *detector = fcpeDetector ? fcpeDetector.get() : externalFCPEDetector;
  std::vector<float> fcpeF0 =
      detector->extractF0(samples, numSamples, audioData.sampleRate);

  if (!fcpeF0.empty() && targetFrames > 0)
  {
    editedData.f0.resize(targetFrames);

    // Time per frame for each system
    const double fcpeFrameTime = 160.0 / 16000.0; // 0.01 seconds
    const double vocoderFrameTime =
        static_cast<double>(HOP_SIZE) /
        static_cast<double>(std::max(1, audioData.sampleRate));

    for (int i = 0; i < targetFrames; ++i)
    {
      double vocoderTime = i * vocoderFrameTime;
      double fcpeFramePos = vocoderTime / fcpeFrameTime;
      int srcIdx = static_cast<int>(fcpeFramePos);
      double frac = fcpeFramePos - srcIdx;

      if (srcIdx + 1 < static_cast<int>(fcpeF0.size()))
      {
        float f0_a = fcpeF0[srcIdx];
        float f0_b = fcpeF0[srcIdx + 1];

        if (f0_a > 0.0f && f0_b > 0.0f)
        {
          // Log-domain interpolation for musical accuracy
          float logF0_a = std::log(f0_a);
          float logF0_b = std::log(f0_b);
          float logF0_interp = logF0_a * (1.0 - frac) + logF0_b * frac;
          editedData.f0[i] = std::exp(logF0_interp);
        }
        else if (f0_a > 0.0f)
        {
          editedData.f0[i] = f0_a;
        }
        else if (f0_b > 0.0f)
        {
          editedData.f0[i] = f0_b;
        }
        else
        {
          editedData.f0[i] = 0.0f;
        }
      }
      else if (srcIdx < static_cast<int>(fcpeF0.size()))
      {
        editedData.f0[i] = fcpeF0[srcIdx];
      }
      else
      {
        editedData.f0[i] = fcpeF0.back() > 0.0f ? fcpeF0.back() : 0.0f;
      }
    }
  }
  else
  {
    editedData.f0.clear();
  }

  // Create voiced mask
  editedData.voicedMask.resize(editedData.f0.size());
  for (size_t i = 0; i < editedData.f0.size(); ++i)
  {
    editedData.voicedMask[i] = editedData.f0[i] > 0;
  }
}

void AudioAnalyzer::segmentIntoNotes(Project &project)
{
  auto &audioData = project.getAudioData();
  auto &editedData = project.getEditedData();
  auto &notes = project.getNotes();
  notes.clear();

  if (editedData.f0.empty())
    return;

  // Try GAME model first
  auto *detector = gameDetector ? gameDetector.get() : externalGAMEDetector;
  if (detector && detector->isLoaded() &&
      audioData.waveform.getNumSamples() > 0)
  {
    segmentWithGAME(project);
    return;
  }

  // Fallback to F0-based segmentation
  segmentFallback(project);
}

void AudioAnalyzer::segmentWithGAME(Project &project)
{
  auto &audioData = project.getAudioData();
  auto &editedData = project.getEditedData();
  const auto& originalMel = project.getAnalysisData().originalMel;
  auto &notes = project.getNotes();

  const float *samples = audioData.waveform.getReadPointer(0);
  int numSamples = audioData.waveform.getNumSamples();
  const int f0Size = static_cast<int>(editedData.f0.size());
  const int melSize = static_cast<int>(originalMel.size());

  auto *detector = gameDetector ? gameDetector.get() : externalGAMEDetector;

  // GAME detectNotes takes the full waveform at its own sample rate
  auto gameNotes = detector->detectNotes(samples, numSamples,
                                         GAMEDetector::SAMPLE_RATE);

  for (const auto &gNote : gameNotes)
  {
    if (gNote.isRest)
      continue;

    int f0Start = std::max(0, std::min(gNote.startFrame, f0Size - 1));
    int f0End = std::max(f0Start + 1, std::min(gNote.endFrame, f0Size));

    if (f0End - f0Start < 3)
      continue;

    // Calculate average MIDI from actual F0 data
    float midiSum = 0.0f;
    int midiCount = 0;
    for (int j = f0Start; j < f0End; ++j)
    {
      if (j < static_cast<int>(editedData.voicedMask.size()) &&
          editedData.voicedMask[j] && editedData.f0[j] > 0)
      {
        midiSum += freqToMidi(editedData.f0[j]);
        midiCount++;
      }
    }

    float midi = gNote.midiNote;
    if (midiCount > 0)
    {
      midi = midiSum / midiCount;
    }

    Note note(f0Start, f0End, midi);

    // Extract mel spectrogram clip for this note
    if (!originalMel.empty() && f0Start < melSize)
    {
      int melStart = std::max(0, f0Start);
      int melEnd = std::min(f0End, melSize);
      if (melEnd > melStart)
      {
        std::vector<std::vector<float>> melClip(
            originalMel.begin() + melStart,
            originalMel.begin() + melEnd);
        note.setClipMel(std::move(melClip));
      }
    }

    notes.push_back(note);
  }

  if (!editedData.f0.empty())
    PitchCurveProcessor::rebuildCurvesFromSource(project, editedData.f0);
  HNSepCurveProcessor::initializeCurves(project);
}

void AudioAnalyzer::segmentFallback(Project &project)
{
  auto &audioData = project.getAudioData();
  auto &editedData = project.getEditedData();
  const auto& originalMel = project.getAnalysisData().originalMel;
  auto &notes = project.getNotes();
  const int melSize = static_cast<int>(originalMel.size());

  auto finalizeNote = [&](int start, int end)
  {
    if (end - start < 5)
      return;

    float midiSum = 0.0f;
    int midiCount = 0;
    for (int j = start; j < end; ++j)
    {
      if (j < static_cast<int>(editedData.voicedMask.size()) &&
          editedData.voicedMask[j] && editedData.f0[j] > 0)
      {
        midiSum += freqToMidi(editedData.f0[j]);
        midiCount++;
      }
    }
    if (midiCount == 0)
      return;

    float midi = midiSum / midiCount;
    Note note(start, end, midi);

    // Extract mel spectrogram clip
    if (!originalMel.empty() && start < melSize)
    {
      int melStart = std::max(0, start);
      int melEnd = std::min(end, melSize);
      if (melEnd > melStart)
      {
        std::vector<std::vector<float>> melClip(
            originalMel.begin() + melStart,
            originalMel.begin() + melEnd);
        note.setClipMel(std::move(melClip));
      }
    }

    notes.push_back(note);
  };

  constexpr float pitchSplitThreshold = 0.5f;
  constexpr int minFramesForSplit = 3;
  constexpr int maxUnvoicedGap = INT_MAX;

  bool inNote = false;
  int noteStart = 0;
  int currentMidiNote = 0;
  int pitchChangeCount = 0;
  int pitchChangeStart = 0;
  int unvoicedCount = 0;

  for (size_t i = 0; i < editedData.f0.size(); ++i)
  {
    bool voiced = i < editedData.voicedMask.size() && editedData.voicedMask[i];

    if (voiced && !inNote)
    {
      inNote = true;
      noteStart = static_cast<int>(i);
      currentMidiNote =
          static_cast<int>(std::round(freqToMidi(editedData.f0[i])));
      pitchChangeCount = 0;
      unvoicedCount = 0;
    }
    else if (voiced && inNote)
    {
      unvoicedCount = 0;
      float currentMidi = freqToMidi(editedData.f0[i]);
      int quantizedMidi = static_cast<int>(std::round(currentMidi));

      if (quantizedMidi != currentMidiNote &&
          std::abs(currentMidi - currentMidiNote) > pitchSplitThreshold)
      {
        if (pitchChangeCount == 0)
          pitchChangeStart = static_cast<int>(i);
        pitchChangeCount++;

        if (pitchChangeCount >= minFramesForSplit)
        {
          finalizeNote(noteStart, pitchChangeStart);
          noteStart = pitchChangeStart;
          currentMidiNote = quantizedMidi;
          pitchChangeCount = 0;
        }
      }
      else
      {
        pitchChangeCount = 0;
      }
    }
    else if (!voiced && inNote)
    {
      unvoicedCount++;
      if (unvoicedCount > maxUnvoicedGap)
      {
        finalizeNote(noteStart, static_cast<int>(i) - unvoicedCount);
        inNote = false;
        pitchChangeCount = 0;
        unvoicedCount = 0;
      }
    }
  }

  if (inNote)
  {
    finalizeNote(noteStart, static_cast<int>(editedData.f0.size()));
  }

  if (!editedData.f0.empty())
    PitchCurveProcessor::rebuildCurvesFromSource(project, editedData.f0);
  HNSepCurveProcessor::initializeCurves(project);
}

void AudioAnalyzer::computeVadMask(Project &project)
{
  auto &audioData = project.getAudioData();
  auto &editedData = project.getEditedData();
  const int numFrames = audioData.getNumFrames();
  if (numFrames <= 0 || audioData.waveform.getNumSamples() == 0)
  {
    editedData.vadMask.assign(numFrames > 0 ? numFrames : 0, false);
    return;
  }

  // RMS threshold for detecting audio energy (including consonants)
  // Lower threshold to capture soft consonants
  constexpr float kVadThreshold = 0.008f;

  const float *samples = audioData.waveform.getReadPointer(0);
  const int numSamples = audioData.waveform.getNumSamples();

  editedData.vadMask.resize(numFrames);
  for (int i = 0; i < numFrames; ++i)
  {
    int sampleStart = i * HOP_SIZE;
    int sampleEnd = std::min(sampleStart + HOP_SIZE, numSamples);
    if (sampleStart >= numSamples)
    {
      editedData.vadMask[i] = false;
      continue;
    }

    float sumSq = 0.0f;
    for (int j = sampleStart; j < sampleEnd; ++j)
      sumSq += samples[j] * samples[j];
    float rms =
        std::sqrt(sumSq / static_cast<float>(sampleEnd - sampleStart));
    editedData.vadMask[i] = rms > kVadThreshold;
  }
}

void AudioAnalyzer::extendNoteBoundariesWithVad(Project &project)
{
  auto &audioData = project.getAudioData();
  auto &editedData = project.getEditedData();
  auto &notes = project.getNotes();

  if (notes.empty() || editedData.vadMask.empty())
    return;

  const int totalFrames = static_cast<int>(editedData.vadMask.size());
  const int f0Size = static_cast<int>(editedData.f0.size());
  if (f0Size <= 0)
    return;

  // Maximum backward extension for consonant capture (~116ms at 44.1kHz/512hop)
  constexpr int kMaxConsonantFrames = 10;
  constexpr int kMaxTailFrames = 8;

  for (size_t ni = 0; ni < notes.size(); ++ni)
  {
    auto &note = notes[ni];
    if (note.isRest())
      continue;

    // Don't extend past the previous note's end
    int prevEnd = 0;
    if (ni > 0 && !notes[ni - 1].isRest())
      prevEnd = notes[ni - 1].getEndFrame();
    int nextStart = totalFrames;
    if (ni + 1 < notes.size() && !notes[ni + 1].isRest())
      nextStart = notes[ni + 1].getStartFrame();

    int searchStart =
        std::max(prevEnd, note.getStartFrame() - kMaxConsonantFrames);
    searchStart = std::max(0, searchStart);

    // Search backward from note start for energy onset
    int newStart = note.getStartFrame();
    for (int i = note.getStartFrame() - 1; i >= searchStart; --i)
    {
      if (i < totalFrames && editedData.vadMask[i])
        newStart = i;
      else
        break; // Stop at first silent frame
    }

    if (newStart < note.getStartFrame())
    {
      note.setStartFrame(newStart);
      note.setSrcStartFrame(newStart);
    }

    // Forward extension for note tails (release consonants/noise), bounded by
    // next note start.
    int newEnd = note.getEndFrame();
    int searchEnd = std::min(nextStart, note.getEndFrame() + kMaxTailFrames);
    searchEnd = std::min(searchEnd, totalFrames);
    for (int i = note.getEndFrame(); i < searchEnd; ++i)
    {
      if (editedData.vadMask[static_cast<size_t>(i)])
        newEnd = i + 1;
      else
        break;
    }
    if (newEnd > note.getEndFrame())
    {
      note.setEndFrame(newEnd);
      note.setSrcEndFrame(newEnd);
    }

    // Keep note/f0 slice consistent after boundary change.
    int clampedStart = std::max(0, std::min(note.getStartFrame(), f0Size - 1));
    int clampedEnd = std::max(clampedStart + 1, std::min(note.getEndFrame(), f0Size));
    note.setStartFrame(clampedStart);
    note.setEndFrame(clampedEnd);
    note.setSrcStartFrame(clampedStart);
    note.setSrcEndFrame(clampedEnd);
  }
}
