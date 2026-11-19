#pragma once

#include "AppPaths.h"

#include <vector>

namespace juststem
{
/** What justStem keeps on disk, and ways to free it. */
namespace storage
{
    struct SavedSong
    {
        juce::String title;
        juce::File folder;
        juce::int64 bytes = 0;
        juce::Time created;
    };

    /** Song folders made by justStem (they contain info.json), newest first. Other files in
        the stems folder are never listed, so they can't be deleted from here. */
    std::vector<SavedSong> savedSongs();

    /** Total size of the files in a folder (recursively). */
    juce::int64 folderSize (const juce::File&);

    /** Move a song's folder to the Trash / Recycle Bin (so it can be restored). */
    bool moveSongToTrash (const SavedSong&);

    /** Delete a cache folder's contents (the folder itself is recreated on demand). */
    void clearFolder (const juce::File&);
} // namespace storage
} // namespace juststem
