#include "SaveStems.h"

namespace juststem::ui
{
namespace
{
/** Saves the stems and the instrumental into a folder (copies, or pitch-shifted when transposed) on a
    background thread, with a progress window and Cancel. Deletes itself. */
class SaveStems final : public juce::ThreadWithProgressWindow
{
public:
    SaveStems (SongResultPtr s, int semis, juce::File folder)
        : juce::ThreadWithProgressWindow (semis == 0 ? juce::String ("Saving stems")
                                                     : "Saving stems transposed " + juce::String (semis > 0 ? "+" : "") + juce::String (semis),
                                          true, true),
          song (std::move (s)), semitones (semis), dir (std::move (folder))
    {
        setStatusMessage (semis == 0 ? "Copying 6 stems and the instrumental..." : "Pitch-shifting 6 stems and the instrumental...");
    }

    void run() override
    {
        try
        {
            const auto progress = [this] (double f) {
                setProgress (f);
                return ! threadShouldExit();
            };
            done = semitones == 0 ? copyStems (*song, dir, progress) : saveTransposedStems (*song, semitones, dir, progress);
        }
        catch (const std::exception& e)
        {
            error = juce::String::fromUTF8 (e.what());
        }
    }

    void threadComplete (bool) override
    {
        if (done) dir.revealToUser();
        else
        {
            dir.deleteRecursively(); // cancelled or failed: don't leave half a set behind
            if (error.isNotEmpty())
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't save the stems", error);
        }
        delete this;
    }

private:
    SongResultPtr song;
    int semitones;
    juce::File dir;
    bool done = false;
    juce::String error;
};
} // namespace

void saveStems (SongResultPtr song, int semitones, const juce::File& dest)
{
    // A new folder each time ("... stems (2)"), so an earlier export is never overwritten.
    auto name = song->title + " stems";
    if (semitones != 0)
        name << " (" << (semitones > 0 ? "+" : "") << semitones << ", " << juce::String (song->analysis.key.transposed (semitones).name()) << ")";
    const auto dir = dest.getChildFile (juce::File::createLegalFileName (name)).getNonexistentSibling();
    (new SaveStems (std::move (song), semitones, dir))->launchThread();
}
} // namespace juststem::ui
