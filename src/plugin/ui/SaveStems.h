#pragma once

#include "../SongResult.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace juststem::ui
{
/** Save all: the song's 6 stems and instrumental into a new folder in `dest`, "<title> stems" (copies),
    or pitch-shifted "<title> stems (+2, D major)" when `semitones` isn't 0; "... (2)" when that exists, so
    an earlier export is never overwritten. Runs on a background thread with a progress window and Cancel,
    then shows the folder; a failed or cancelled save is removed. Message thread. */
void saveStems (SongResultPtr song, int semitones, const juce::File& dest);
} // namespace juststem::ui
