#include "SaveStems.h"
#include "../../io/AppPaths.h"

#include <utility>

namespace juststem::ui
{
namespace
{
/** Saves the stems, the instrumental and the full mix into a folder (copies, or pitch-shifted when transposed) on a
    background thread, with a progress window and Cancel. Owned by the processor via StemExport. */
class SaveStems final : public StemExport, private juce::ThreadWithProgressWindow
{
public:
    SaveStems (SongResultPtr s, int semis, juce::File folder, juce::Component* centreAround, std::function<void()> finished)
        : juce::ThreadWithProgressWindow (semis == 0 ? juce::String ("Saving stems")
                                                     : "Saving stems transposed " + juce::String (semis > 0 ? "+" : "") + juce::String (semis),
                                          true, true, 10000, {}, centreAround),
          song (std::move (s)), semitones (semis), dir (std::move (folder)), onFinished (std::move (finished))
    {
        setStatusMessage (semis == 0 ? "Copying 6 stems, the instrumental and the full mix..." : "Pitch-shifting 6 stems, the instrumental and the full mix...");
    }

    ~SaveStems() override
    {
        signalThreadShouldExit();
        stopThread (10000);
        if (! done) dir.deleteRecursively();
    }

    void launch() { launchThread(); }

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

    void threadComplete (bool userPressedCancel) override
    {
        if (done)
        {
            if (paths::dataOverride() == juce::File()) dir.revealToUser();
        }
        else
        {
            dir.deleteRecursively(); // cancelled or failed: don't leave half a set behind
            if (error.isNotEmpty() && ! userPressedCancel)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't save the stems", error);
        }
        if (auto cb = std::move (onFinished))
            juce::MessageManager::callAsync (std::move (cb));
    }

private:
    SongResultPtr song;
    int semitones;
    juce::File dir;
    std::function<void()> onFinished;
    bool done = false;
    juce::String error;
};
} // namespace

std::unique_ptr<StemExport> startSaveStems (SongResultPtr song, int semitones, const juce::File& dest,
                                            juce::Component* centreAround, std::function<void()> onFinished)
{
    // A new folder each time ("... stems (2)"), so an earlier export is never overwritten.
    auto name = song->title + " stems";
    if (semitones != 0)
        name << " (" << (semitones > 0 ? "+" : "") << semitones << ", " << juce::String (song->analysis.key.transposed (semitones).name()) << ")";
    const auto dir = dest.getChildFile (juce::File::createLegalFileName (name)).getNonexistentSibling();
    auto exp = std::make_unique<SaveStems> (std::move (song), semitones, dir, centreAround, std::move (onFinished));
    exp->launch();
    return exp;
}
} // namespace juststem::ui
