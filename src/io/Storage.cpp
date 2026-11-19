#include "Storage.h"

#include <algorithm>

namespace juststem::storage
{
juce::int64 folderSize (const juce::File& dir)
{
    juce::int64 total = 0;
    for (const auto& entry : juce::RangedDirectoryIterator (dir, true, "*", juce::File::findFiles))
        total += entry.getFileSize();
    return total;
}

std::vector<SavedSong> savedSongs()
{
    std::vector<SavedSong> songs;
    for (const auto& entry : juce::RangedDirectoryIterator (paths::stems(), false, "*", juce::File::findDirectories))
    {
        const auto dir = entry.getFile();
        const auto info = juce::JSON::parse (dir.getChildFile ("info.json"));
        if (! info.isObject()) continue; // not made by justStem
        songs.push_back ({ info["title"].toString().isNotEmpty() ? info["title"].toString() : dir.getFileName(), dir,
                           folderSize (dir), dir.getCreationTime() });
    }
    std::sort (songs.begin(), songs.end(), [] (const SavedSong& a, const SavedSong& b) { return a.created > b.created; });
    return songs;
}

bool moveSongToTrash (const SavedSong& song)
{
    return song.folder.getChildFile ("info.json").existsAsFile() && song.folder.moveToTrash();
}

void clearFolder (const juce::File& dir)
{
    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFilesAndDirectories))
        entry.getFile().isDirectory() ? (void) entry.getFile().deleteRecursively() : (void) entry.getFile().deleteFile();
}
} // namespace juststem::storage
