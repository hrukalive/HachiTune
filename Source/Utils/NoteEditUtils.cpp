#include "NoteEditUtils.h"
#include "../Models/Project.h"
#include "../Models/Note.h"
#include "PitchCurveProcessor.h"
#include "CurveResampler.h"

namespace NoteEditUtils {

void resetNoteToOriginal(Project& project, Note& note)
{
  const int startFrame = note.getStartFrame();
  const int endFrame = note.getEndFrame();
  const int len = endFrame - startFrame;
  if (len <= 0)
    return;

  // 1. Restore originalDeltaPitch and originalPitch from global analysisData.
  //    When the note is stretched (src range differs from output range),
  //    slice from the source range and resample to output duration.
  const auto& analysisData = project.getAnalysisData();
  const int analysisFrames = analysisData.getNumFrames();

  if (analysisFrames > 0)
  {
    const int srcStart = note.getSrcStartFrame();
    const int srcEnd = note.getSrcEndFrame();
    const int srcLen = srcEnd - srcStart;

    auto sliceAnalysis = [&](const std::vector<float>& global) {
      const int sliceLen = (srcLen > 0) ? srcLen : len;
      const int sliceStart = (srcLen > 0) ? srcStart : startFrame;
      std::vector<float> slice(static_cast<size_t>(sliceLen));
      for (int i = 0; i < sliceLen; ++i)
      {
        int gi = sliceStart + i;
        if (gi >= 0 && gi < analysisFrames)
          slice[static_cast<size_t>(i)] = global[static_cast<size_t>(gi)];
      }
      return slice;
    };

    if (!analysisData.originalDeltaPitch.empty())
    {
      auto srcSlice = sliceAnalysis(analysisData.originalDeltaPitch);
      if (srcLen > 0 && srcLen != len)
        note.setOriginalDeltaPitch(
            CurveResampler::resampleLinear(srcSlice, len));
      else
        note.setOriginalDeltaPitch(std::move(srcSlice));
    }

    if (!analysisData.originalPitch.empty())
    {
      auto srcSlice = sliceAnalysis(analysisData.originalPitch);
      if (srcLen > 0 && srcLen != len)
        note.setOriginalPitch(
            CurveResampler::resampleLinear(srcSlice, len));
      else
        note.setOriginalPitch(std::move(srcSlice));
    }
  }

  // 2. Reset tool parameters to defaults
  note.resetToolParams();

  // 3. Reset pitch offset
  note.setPitchOffset(0.0f);

  // 4. Clear working deltaPitch so rebuild picks up from restored originalDeltaPitch
  note.setDeltaPitch({});

  // 5. Clear f0EditedMask for this note's frame range
  auto& audioData = project.getAudioData();
  if (!audioData.f0EditedMask.empty())
  {
    for (int i = startFrame;
         i < endFrame &&
         i < static_cast<int>(audioData.f0EditedMask.size());
         ++i)
    {
      if (i >= 0)
        audioData.f0EditedMask[static_cast<size_t>(i)] = false;
    }
  }

  // 6. Mark dirty for resynthesis
  note.markDirty();
  note.markSynthDirty();

  // 7. Rebuild global pitch curves from notes
  PitchCurveProcessor::rebuildBaseFromNotes(project);
}

}
