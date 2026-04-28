# HachiTune Agent Guidelines

This document provides essential context for AI coding agents working on the HachiTune codebase.

## Project Overview

HachiTune is a JUCE-based audio plugin (VST3/AU/AAX) and standalone app for vocal pitch editing with:
- Neural pitch detection (RMVPE, FCPE)
- Neural vocoder resynthesis (pc_nsf_hifigan)
- ARA integration for DAW hosts
- Real-time preview and piano roll editing

**Tech Stack**: C++17, JUCE 8.x, ONNX Runtime, ARA SDK, CMake 3.22+

## Build Commands

### Initial Setup
```powershell
# Clone with submodules
git submodule update --init --recursive

# Download required models to Resources/models/ (see README.md)
# Required: rmvpe.onnx, fcpe.onnx, some.onnx, pc_nsf_hifigan.onnx, cent_table.bin, mel_filterbank.bin
```

### Configure Build (Windows PowerShell)
```powershell
# CPU-only build
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Enable CUDA acceleration (mutually exclusive with DirectML)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_CUDA=ON

# Enable DirectML acceleration (recommended for Windows, cannot be used with CUDA)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON

# Enable ASIO support (Windows)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_ASIO=ON

# Combined: DirectML + ASIO (common for Windows development)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON -DUSE_ASIO=ON
```

### Configure Build (macOS/Linux)
```bash
# macOS: Specify architecture and deployment target
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DCMAKE_OSX_ARCHITECTURES=arm64

# Linux: With Ninja generator
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

### Build
```powershell
# Build all targets (parallel)
cmake --build build --config Release --parallel

# Build specific target
cmake --build build --config Release --target HachiTune
cmake --build build --config Release --target HachiTunePlugin
```

### Debug Build (Windows)
```powershell
# Debug build with DirectML (default recommendation)
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DUSE_DIRECTML=ON
cmake --build build --config Debug
```

### Clean Build
```powershell
# Remove build directory and rebuild
Remove-Item -Recurse -Force build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

## Testing

**⚠️ Note**: This project currently has NO test infrastructure. When implementing tests:
- Consider using JUCE's built-in UnitTest framework
- Add CMake test targets to CMakeLists.txt
- Follow JUCE testing patterns for audio processing

## Code Style Guidelines

### File Organization

**Directory Structure:**
```
Source/
  Audio/          # Audio engine, pitch detection, vocoder, synthesis
    Analysis/     # AudioAnalyzer and analysis utilities
    Engine/       # Playback, transport, host sync
    IO/           # File I/O (audio, MIDI)
    Synthesis/    # IncrementalSynthesizer
      StretchProcessor.h/.cpp  # Pure warp/stretch algorithms
  Models/         # Data models (Project, Note, serialization)
    AnalysisData.h    # Immutable analysis results (F0, pitch, masks)
    EditedData.h      # Global editable state (pitch, curves, masks)
    ProjectListener.h # Change notification interface
  UI/             # All UI components
    Main/         # MenuHandler, SettingsManager
    PianoRoll/    # Piano roll sub-components
    Workspace/    # Workspace layout components
    Debug/        # Debug/development tools
      TreeValueMonitor.h/.cpp  # Debug project state monitor
  Undo/           # Undo/redo infrastructure
    SnapshotHelper.h/.cpp  # Snapshot capture/restore for undo
  Utils/          # Utilities, localization, curves, constants
  Plugin/         # Plugin-specific code (ARA, processors, editors)
  JuceHeader.h    # Centralized JUCE includes
  Main.cpp        # Standalone app entry point
```

**Header/Source Pairing:**
- Each `.h` file has corresponding `.cpp` (unless template-only or trivial)
- Headers declare interface, `.cpp` implements logic

### Naming Conventions

**Classes**: `PascalCase`
```cpp
class AudioEngine { };
class PianoRollComponent { };
class Note { };
```

**Functions/Methods**: `camelCase`
```cpp
void analyzeAudio();
float getMidiNote() const;
void setPlaying(bool playing);
```

**Variables**: `camelCase`
```cpp
int startFrame = 0;
float midiNote = 60.0f;
std::atomic<bool> isPlaying{false};
```

**Constants**: `SCREAMING_SNAKE_CASE` or `PascalCase` with `constexpr`
```cpp
constexpr int SAMPLE_RATE = 44100;
constexpr float DEFAULT_PIXELS_PER_SECOND = 100.0f;
```

**Private Members**: No prefix (clean modern C++)
```cpp
private:
    int currentPosition = 0;  // NOT m_currentPosition or _currentPosition
    bool playing = false;
```

**Booleans**: Use `is`, `has`, `should` prefixes
```cpp
bool isPlaying() const;
bool hasOriginalWaveform = false;
bool shouldStop = false;
```

### Include Patterns

**Include Order** (consistent across codebase):
1. Corresponding header (for `.cpp` files)
2. Project headers (relative paths from Source/)
3. JUCE headers (via `JuceHeader.h` or direct)
4. Standard library headers

**Example from `Note.cpp`:**
```cpp
#include "Note.h"                    // 1. Corresponding header
#include "../Utils/Constants.h"      // 2. Project headers
                                      // 3. JUCE (via JuceHeader.h in Note.h)
```

**Example from `MainComponent.h`:**
```cpp
#pragma once

#include "../Audio/Analysis/AudioAnalyzer.h"
#include "../Audio/AudioEngine.h"
#include "../Audio/FCPEPitchDetector.h"
// ... more project headers ...
#include "../JuceHeader.h"           // JUCE umbrella header
#include <atomic>                    // Standard library
#include <thread>
```

**Include Guards**: Use `#pragma once` (modern, simpler)
```cpp
#pragma once  // Preferred over #ifndef guards

#include "..."
```

**Forward Declarations**: Prefer forward declarations in headers when possible to reduce compile times
```cpp
// In header: forward declare when you only need pointers/references
class Project;
class Vocoder;

class MainComponent {
    std::unique_ptr<Project> project;  // Only pointer, no full definition needed
};
```

### Formatting

**Indentation**: 2 spaces (NOT tabs)
```cpp
class Example {
public:
  void method() {
    if (condition) {
      doSomething();
    }
  }
};
```

**Braces**: Opening brace on same line (JUCE/Google style)
```cpp
void function() {       // NOT: function()\n{
  if (x) {
    // ...
  } else {
    // ...
  }
}
```

**Line Length**: Keep under 80-100 characters when reasonable, but readability > strict limits

**Spacing**:
```cpp
// Good
void function(int x, float y) {
  int result = x + y;
}

// Avoid
void function(int x,float y){
  int result=x+y;
}
```

### Comments and Documentation

**Class Documentation**: Brief Javadoc-style comments
```cpp
/**
 * Represents a single note/pitch segment.
 *
 * Pitch model:
 * - midiNote: The base pitch of the note (can be changed by dragging)
 * - deltaPitch: Per-frame deviation from base pitch (preserved during drag)
 * - pitchOffset: Non-destructive pitch offset (semitones)
 *
 * Vibrato parameters:
 * - mix: Vibrato wet/dry (0..1)
 * - fadeInMs / fadeOutMs: Vibrato fade-in/fade-out in milliseconds
 */
class Note {
  // ...
};
```

**Inline Comments**: Explain "why" not "what"
```cpp
// Good: Explains intent
// Convert semitone offset to frequency ratio
float ratio = std::pow(2.0f, pitchOffset / 12.0f);

// Bad: States the obvious
// Set ratio to 2 to the power of pitchOffset divided by 12
float ratio = std::pow(2.0f, pitchOffset / 12.0f);
```

**TODO Comments**: Use for future work
```cpp
// TODO: Implement GPU acceleration for CUDA path
// FIXME: Race condition in audio callback
```

### Types and Memory Management

**Smart Pointers**: Prefer RAII and smart pointers
```cpp
std::unique_ptr<Project> project;           // Exclusive ownership
std::shared_ptr<PositionCallback> callback; // Shared ownership
```

**Atomics for Lock-Free**: Use `std::atomic` for thread-safe primitives
```cpp
std::atomic<bool> playing{false};
std::atomic<int64_t> currentPosition{0};
```

**JUCE Types**: Use JUCE types for audio and UI
```cpp
juce::AudioBuffer<float> buffer;
juce::String filename;
juce::File modelPath;
juce::CriticalSection lock;  // For thread safety
```

**Const Correctness**: Mark methods `const` when they don't modify state
```cpp
int getStartFrame() const { return startFrame; }  // Getter should be const
float getMidiNote() const { return midiNote; }
```

### Error Handling

**JUCE Result Pattern**: Use `juce::Result` for operations that can fail
```cpp
juce::Result loadFile(const juce::File& file) {
    if (!file.exists())
        return juce::Result::fail("File does not exist");
    
    // ... load logic ...
    
    return juce::Result::ok();
}
```

**Assertions**: Use `jassert` (JUCE) for debug-time invariants
```cpp
jassert(startFrame < endFrame);  // Only in Debug builds
jassert(sampleRate > 0);
```

**Error Checking**: Check for invalid states
```cpp
if (f0 <= 0.0f)
    return 0.0f;

if (buffer.getNumSamples() == 0)
    return;
```

**No Exceptions**: JUCE codebase generally avoids exceptions in favor of Result/assertion patterns

## JUCE-Specific Patterns

### Component Lifecycle
```cpp
class MyComponent : public juce::Component {
public:
    MyComponent() {
        // Initialize UI, set sizes
        setSize(800, 600);
        addAndMakeVisible(childComponent);
    }
    
    void paint(juce::Graphics& g) override {
        // Drawing code (called on message thread)
    }
    
    void resized() override {
        // Layout child components
        childComponent.setBounds(getLocalBounds());
    }
    
private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MyComponent)
};
```

### Message Thread Safety
```cpp
// UI updates MUST happen on message thread
juce::MessageManager::callAsync([this]() {
    // Safe to update UI here
    updateDisplay();
});
```

### Audio Thread Safety
```cpp
// Audio callbacks must be real-time safe:
// - No allocations
// - No locks (prefer lock-free atomics)
// - No blocking operations
void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override {
    // Real-time safe code only!
    auto position = currentPosition.load();  // Atomic read is OK
    // malloc(), new, mutex.lock() are NOT OK
}
```

### JUCE Macros
```cpp
JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MyClass)  // End of class definition
```

## Architecture Notes

### Data Model

Project contains three key data layers:
- **AudioData**: Raw audio samples and metadata
- **AnalysisData**: Immutable analysis results (F0 contours, pitch, masks) — produced by analyzers, never edited directly
- **EditedData**: Global editable state (edited pitch curves, masks) — what the user modifies

Changes are broadcast via **ProjectListener** interface.

### Stretch / Warp

Stretch/warp functionality is always enabled (no conditional compilation flags). There is no `ENABLE_NOTE_STRETCH` or similar preprocessor guard.

- **StretchProcessor** (`Audio/Synthesis/`): Pure algorithms for time-stretching and warping
- **StretchHandler** (`UI/`): UI-only interaction handler for stretch operations in the piano roll

### Undo System

Undo uses snapshot-based approach via `SnapshotHelper` — captures and restores project state snapshots.

## ARA Integration Notes

- ARA support is conditional: enabled when `third_party/ARA_SDK` submodule exists
- Compile definition: `JucePlugin_Enable_ARA=1`
- See `Plugin/ARADocumentController.cpp` for ARA-specific implementations
- Non-ARA fallback: `Plugin/NonAraCaptureController.cpp`

## Platform-Specific Notes

### Windows
- Use `NOMINMAX` to prevent min/max macro conflicts
- ASIO support optional (`-DUSE_ASIO=ON`)
- GPU options: CUDA or DirectML (mutually exclusive)

### macOS
- Code signing required (ad-hoc signing in CI: `codesign -s - ...`)
- Bundle structure: `.app/Contents/Resources/` for models/fonts/lang
- Use `@executable_path/../Frameworks` for dylib paths

### Linux
- Dependencies: ALSA, JACK, X11, FreeType, etc. (see `.github/workflows/build.yml`)
- Runtime: Set `LD_LIBRARY_PATH` for ONNX Runtime dylibs

## Common Patterns

### Callback Pattern
```cpp
using ProgressCallback = std::function<void(double progress, const juce::String& message)>;

void analyzeAudio(ProgressCallback onProgress) {
    onProgress(0.5, "Detecting pitch...");
}
```

### Async Operations
```cpp
std::thread analysisThread;
std::atomic<bool> isAnalyzing{false};

void startAnalysis() {
    isAnalyzing = true;
    analysisThread = std::thread([this]() {
        // Heavy work here
        isAnalyzing = false;
    });
}
```

### JUCE ValueTree for Settings
```cpp
// Use juce::ValueTree for hierarchical settings/state
juce::ValueTree settings("Settings");
settings.setProperty("sampleRate", 44100, nullptr);
```

## Git Workflow

- Branch naming: Use descriptive names (e.g., `feature/add-vibrato`, `fix/crash-on-load`)
- Commit messages: Clear, concise, imperative mood
  - Good: "Add CUDA support for vocoder"
  - Bad: "fixed stuff", "WIP"

## Resources

- [JUCE Documentation](https://docs.juce.com/)
- [JUCE Tutorials](https://juce.com/learn/tutorials)
- [ARA SDK Documentation](third_party/ARA_SDK/ARA_API/README.md)
- Project README: [README.md](README.md)

## Important Reminders

1. **No Breaking Changes**: Ensure model file compatibility (ONNX format changes require coordination)
2. **Thread Safety**: Audio thread code must be real-time safe (no allocations, locks, blocking)
3. **Cross-Platform**: Test on Windows, macOS, Linux if possible; use platform-agnostic JUCE APIs
4. **Resource Paths**: Models/fonts/lang must be found at runtime (see CMakeLists.txt copy commands)
5. **ARA Hosts**: When modifying plugin code, test in ARA and non-ARA modes
6. **GPU Acceleration**: Changes to inference code should work with CPU/CUDA/DirectML/CoreML

## Quick Reference (Windows PowerShell)

| Task | Command |
|------|---------|
| Configure (CPU) | `cmake -B build -DCMAKE_BUILD_TYPE=Release` |
| Configure (DirectML) | `cmake -B build -DCMAKE_BUILD_TYPE=Release -DUSE_DIRECTML=ON -DUSE_ASIO=ON` |
| Build all | `cmake --build build --config Release --parallel` |
| Clean build | `Remove-Item -Recurse -Force build; cmake -B build; cmake --build build --config Release --parallel` |
| Run standalone | `.\build\HachiTune_artefacts\Release\HachiTune.exe` |
| VST3 location | `build\HachiTunePlugin_artefacts\Release\VST3\HachiTune.vst3\Contents\x86_64-win\` |

## Quick Reference (macOS/Linux)

| Task | Command |
|------|---------|
| Configure | `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` |
| Build all | `cmake --build build --config Release --parallel` |
| Clean build | `rm -rf build && cmake -B build && cmake --build build` |
| Run standalone | `./build/HachiTune_artefacts/Release/HachiTune.app/Contents/MacOS/HachiTune` (macOS)<br>`./build/HachiTune_artefacts/Release/HachiTune` (Linux) |

---

**Last Updated**: 2026-04-28  
**For Questions**: See project maintainers or open an issue on GitHub
