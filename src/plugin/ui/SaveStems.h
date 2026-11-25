#pragma once

#include "../SongResult.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace juststem::ui
{
/** Handle for an in-progress Save all export; destroying it cancels and cleans up the partial folder. */
class StemExport
{
public:
    virtual ~StemExport() = default;
};

/** Save all: the song's 6 stems, instrumental and full mix into a new folder in `dest`, "<title> stems" (copies),
    or pitch-shifted "<title> stems (+2, D major)" when `semitones` isn't 0; "... (2)" when that exists, so
    an earlier export is never overwritten. Runs on a background thread with a progress window and Cancel,
    then shows the folder; a failed or cancelled save is removed. Message thread. */
std::unique_ptr<StemExport> startSaveStems (SongResultPtr song, int semitones, const juce::File& dest,
                                            juce::Component* centreAround, std::function<void()> onFinished);
} // namespace juststem::ui
