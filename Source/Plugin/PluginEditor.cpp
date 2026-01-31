#include "PluginEditor.h"
#include "../UI/StyledComponents.h"
#include "../Utils/AppLogger.h"
#include "../Utils/WindowSizing.h"
#include "HostCompatibility.h"

#if JucePlugin_Enable_ARA
#include "ARADocumentController.h"
#endif

HachiTuneAudioProcessorEditor::HachiTuneAudioProcessorEditor(
    HachiTuneAudioProcessor &p)
    : AudioProcessorEditor(&p), audioProcessor(p)
#if JucePlugin_Enable_ARA
      ,
      AudioProcessorEditorARAExtension(&p)
#endif
{
  // Initialize app font
  AppLogger::init();
  AppFont::initialize();

  // Enable keyboard focus for the editor
  setWantsKeyboardFocus(true);

  addAndMakeVisible(mainComponent);
  audioProcessor.setMainComponent(&mainComponent);

#if JucePlugin_Enable_ARA
  setupARAMode();
#else
  setupNonARAMode();
#endif

  setupCallbacks();

  auto *display = WindowSizing::getDisplayForComponent(this);
  auto constraints = WindowSizing::Constraints();
  if (display != nullptr) {
    auto size = WindowSizing::getClampedSize(WindowSizing::kDefaultWidth,
                                             WindowSizing::kDefaultHeight,
                                             *display, constraints);
    setSize(size.x, size.y);
  } else {
    setSize(WindowSizing::kDefaultWidth, WindowSizing::kDefaultHeight);
  }
  setResizable(true, true);

  // Grab keyboard focus on mainComponent when editor is shown
  mainComponent.grabKeyboardFocus();
}

HachiTuneAudioProcessorEditor::~HachiTuneAudioProcessorEditor() {
  audioProcessor.setMainComponent(nullptr);
  AppFont::shutdown(); // Release font resources (reference counted)
}

void HachiTuneAudioProcessorEditor::setupARAMode() {
#if JucePlugin_Enable_ARA
  mainComponent.getToolbar().setARAMode(true);

  auto *editorView = getARAEditorView();
  if (!editorView) {
    setupNonARAMode();
    return;
  }

  auto *docController = editorView->getDocumentController();
  if (!docController) {
    setupNonARAMode();
    return;
  }

  auto *pitchDocController = juce::ARADocumentControllerSpecialisation::
      getSpecialisedDocumentController<HachiTuneDocumentController>(
          docController);

  if (!pitchDocController) {
    setupNonARAMode();
    return;
  }

  // Connect ARA controller to UI
  pitchDocController->setMainComponent(&mainComponent);
  pitchDocController->setRealtimeProcessor(
      &audioProcessor.getRealtimeProcessor());

  // Setup re-analyze callback
  mainComponent.onReanalyzeRequested = [pitchDocController]() {
    pitchDocController->reanalyze();
  };

  mainComponent.onRequestHostPlayState = [this](bool shouldPlay) {
    audioProcessor.requestHostPlayState(shouldPlay);
  };

  mainComponent.onRequestHostStop = [this]() {
    audioProcessor.requestHostStop();
  };

  mainComponent.onRequestHostSeek = [this](double timeInSeconds) {
    audioProcessor.requestHostSeek(timeInSeconds);
  };

  // Check for existing audio sources
  auto *juceDocument = docController->getDocument();
  if (juceDocument) {
    auto &audioSources = juceDocument->getAudioSources<juce::ARAAudioSource>();

    if (!audioSources.empty()) {
      // Process first audio source
      auto *source = audioSources.front();
      if (source && source->getSampleCount() > 0) {
        juce::ARAAudioSourceReader reader(source);
        int numSamples = static_cast<int>(source->getSampleCount());
        int numChannels = source->getChannelCount();
        double sampleRate = source->getSampleRate();

        juce::AudioBuffer<float> buffer(numChannels, numSamples);
        if (reader.read(&buffer, 0, numSamples, 0, true, true)) {
          mainComponent.setHostAudio(buffer, sampleRate);
          return;
        }
      }
    }
  }
#endif
}

void HachiTuneAudioProcessorEditor::setupNonARAMode() {
  mainComponent.getToolbar().setARAMode(false);

  // Setup host transport control callbacks for non-ARA mode
  mainComponent.onRequestHostPlayState = [this](bool shouldPlay) {
    audioProcessor.requestHostPlayState(shouldPlay);
  };

  mainComponent.onRequestHostStop = [this]() {
    audioProcessor.requestHostStop();
  };

  mainComponent.onRequestHostSeek = [this](double timeInSeconds) {
    audioProcessor.requestHostSeek(timeInSeconds);
  };
}

void HachiTuneAudioProcessorEditor::setupCallbacks() {
  // When project data changes (analysis complete or synthesis complete)
  mainComponent.onProjectDataChanged = [this]() {
    // IMPORTANT: Set project FIRST, then vocoder
    // setProject() calls invalidate() which needs a valid project
    if (mainComponent.getProject())
      audioProcessor.getRealtimeProcessor().setProject(
          mainComponent.getProject());

    // setVocoder() does NOT call invalidate() anymore (safe to call anytime)
    if (mainComponent.getVocoder())
      audioProcessor.getRealtimeProcessor().setVocoder(
          mainComponent.getVocoder());

    audioProcessor.getRealtimeProcessor().invalidate();
  };

  // onPitchEditFinished is handled by onProjectDataChanged (called after async
  // synthesis completes) No need for separate callback here
}

void HachiTuneAudioProcessorEditor::paint(juce::Graphics &) {
  // MainComponent handles all painting
}

void HachiTuneAudioProcessorEditor::resized() {
  mainComponent.setBounds(getLocalBounds());
}

void HachiTuneAudioProcessorEditor::visibilityChanged() {
  if (isVisible())
    mainComponent.grabKeyboardFocus();
}

void HachiTuneAudioProcessorEditor::mouseDown(const juce::MouseEvent& e) {
  juce::ignoreUnused(e);
  mainComponent.grabKeyboardFocus();
}
