// juststem_uitest <model.onnx> <song> <output folder> [--hq]: drives the real plug-in code without a DAW.
// Renders the editor to PNGs, runs a full split, checks the preview audio and state save/reload.
#include "../src/io/AudioFiles.h"
#include "../src/plugin/PluginEditor.h"
#include "../src/plugin/PluginProcessor.h"

#include <cstdio>

static int failures = 0;
static void check (bool ok, const juce::String& what)
{
    std::printf ("  [%s] %s\n", ok ? "ok" : "FAIL", what.toRawUTF8());
    failures += ok ? 0 : 1;
}

static void snapshot (juce::Component& c, const juce::File& f)
{
    auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 2.0f);
    f.deleteFile();
    juce::FileOutputStream out (f);
    juce::PNGImageFormat().writeImageToStream (img, out);
    std::printf ("  snapshot %s\n", f.getFullPathName().toRawUTF8());
}

static void pumpUntil (const std::function<bool()>& done, int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    while (! done() && juce::Time::getMillisecondCounter() < end)
        juce::MessageManager::getInstance()->runDispatchLoopUntil (50);
}

static double renderRms (justStemProcessor& p, int blocks)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    double sum = 0;
    for (int b = 0; b < blocks; ++b)
    {
        buf.clear();
        p.processBlock (buf, midi);
        for (int ch = 0; ch < 2; ++ch) sum += buf.getRMSLevel (ch, 0, 512) * buf.getRMSLevel (ch, 0, 512);
    }
    return std::sqrt (sum / (2.0 * blocks));
}

/** The first child of `parent` that is a T (and passes `pred`, if given). */
template <typename T>
static T* findChild (juce::Component& parent, const std::function<bool (T&)>& pred = {})
{
    for (auto* c : parent.getChildren())
        if (auto* t = dynamic_cast<T*> (c); t != nullptr && (! pred || pred (*t))) return t;
    return nullptr;
}

/** The child button called `name` (icon-only buttons are named for this). */
static juce::Button* findButton (juce::Component& parent, const juce::String& name)
{
    return findChild<juce::Button> (parent, [&] (juce::Button& b) { return b.getName() == name; });
}

/** Render `blocks` blocks of 512 at the prepared rate; returns the left channel. */
static std::vector<float> render (justStemProcessor& p, int blocks)
{
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    std::vector<float> out;
    for (int b = 0; b < blocks; ++b)
    {
        buf.clear();
        p.processBlock (buf, midi);
        out.insert (out.end(), buf.getReadPointer (0), buf.getReadPointer (0) + 512);
    }
    return out;
}

int main (int argc, char** argv)
{
    if (argc < 4) { std::fprintf (stderr, "usage: juststem_uitest <model.onnx> <song> <output folder> [--hq]\n"); return 2; }
    const bool hq = argc > 4 && std::string (argv[4]) == "--hq";
    juce::ScopedJuceInitialiser_GUI gui;
    // UTF-8 paths, relative to the current folder unless absolute.
    auto fileArg = [] (const char* arg) { return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (arg)); };
    const juce::File model = fileArg (argv[1]), song = fileArg (argv[2]), outDir = fileArg (argv[3]);
    outDir.createDirectory();

    auto proc = std::make_unique<justStemProcessor> (model);
    proc->prepareToPlay (48000, 512);
    if (hq)
    {
        if (! proc->hqAvailable()) { std::fprintf (stderr, "--hq: no HQ model next to %s\n", model.getFullPathName().toRawUTF8()); return 2; }
        proc->setHQ (true);
    }
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());

    std::printf ("UI\n");
    snapshot (*editor, outDir.getChildFile ("1-idle.png"));

    std::printf ("Error messages\n");
    {
        // A file that isn't audio, with a Thai name: the error must show the name, not mojibake.
        const auto bad = outDir.getChildFile (juce::String::fromUTF8 ("\xe0\xb9\x80\xe0\xb8\x9e\xe0\xb8\xa5\xe0\xb8\x87") + ".mp3");
        bad.replaceWithText ("not audio");
        proc->startFile (bad);
        pumpUntil ([&] { return ! proc->isBusy(); }, 10000);
        check (proc->lastError().contains (bad.getFileName()), "error shows the file's Thai name: " + proc->lastError());
        bad.deleteFile();
    }

    proc->startFile (song);
    // On a first run (no compiled CoreML model) the gauge counts down while the model compiles.
    pumpUntil ([&] { auto s = proc->jobStatus(); return (s.secondsLeft >= 0 && s.fraction > 0.1) || s.step.startsWith ("Splitting"); }, 600000);
    if (proc->jobStatus().secondsLeft >= 0) snapshot (*editor, outDir.getChildFile ("2a-compiling.png"));
    pumpUntil ([&] { auto s = proc->jobStatus(); return s.step.startsWith ("Splitting") && s.fraction > 0.3; }, 600000);
    snapshot (*editor, outDir.getChildFile ("2-progress.png"));
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    pumpUntil ([&] { return ! proc->isBusy(); }, 900000);
    for (const auto& line : proc->jobLog())
        if (line.contains ("Model:") || line.contains ("Running on") || line.contains ("Split 6 stems"))
            std::printf ("  log: %s\n", line.toRawUTF8());
    std::printf ("  job finished %.1f s after the progress snapshot\n", (juce::Time::getMillisecondCounterHiRes() - t0) / 1000);
    check (proc->getSong() != nullptr, "song processed" + (proc->lastError().isEmpty() ? juce::String() : ": " + proc->lastError()));
    for (const auto& line : proc->jobLog()) std::printf ("    | %s\n", line.toRawUTF8());
    if (proc->getSong() == nullptr) return 1;
    pumpUntil ([] { return false; }, 300); // let the editor refresh
    snapshot (*editor, outDir.getChildFile ("3-results.png"));

    std::printf ("Storage panel\n");
    auto* storageButton = findButton (*editor, "Storage");
    check (storageButton != nullptr && storageButton->isVisible(), "Storage button shown");
    if (storageButton != nullptr) storageButton->triggerClick();
    pumpUntil ([] { return false; }, 300);
    auto* panel = findChild<juststem::ui::StoragePanel> (*editor);
    check (panel != nullptr, "Storage panel opens");
    if (panel != nullptr)
    {
        snapshot (*editor, outDir.getChildFile ("4-storage.png"));
        const auto saved = juststem::storage::savedSongs();
        check (std::any_of (saved.begin(), saved.end(), [&] (const auto& x) { return x.folder == proc->getSong()->folder; }),
               "current song listed in storage");
        panel->onClose();
        pumpUntil ([] { return false; }, 200);
        check (findChild<juststem::ui::StoragePanel> (*editor) == nullptr,
               "Storage panel closes");
    }

    std::printf ("New song\n");
    {
        auto* newSong = findButton (*editor, "New song");
        auto* link = findChild<juce::TextEditor> (*editor);
        check (newSong != nullptr && newSong->isVisible() && link != nullptr && ! link->isVisible(), "link box hidden while a song is open");
        if (newSong != nullptr) newSong->triggerClick();
        pumpUntil ([] { return false; }, 200);
        check (link != nullptr && link->isVisible() && ! newSong->isVisible(), "New song shows the link box");
        if (auto* paste = findButton (*editor, "Paste"))
        {
            const auto saved = juce::SystemClipboard::getTextFromClipboard(); // put the user's clipboard back afterwards
            juce::SystemClipboard::copyTextToClipboard ("  https://www.youtube.com/watch?v=test \n");
            paste->triggerClick();
            pumpUntil ([] { return false; }, 100);
            check (link->getText() == "https://www.youtube.com/watch?v=test", "Paste button fills the link box");
            check (! paste->isVisible(), "Paste button hides once the box has a link");
            snapshot (*editor, outDir.getChildFile ("3b-new-song.png"));
            link->clear();
            juce::SystemClipboard::copyTextToClipboard (saved);
        }
        else check (false, "Paste button shown");
        if (auto* back = findButton (*editor, "Back to song"); back != nullptr && back->isVisible())
            back->triggerClick();
        pumpUntil ([] { return false; }, 200);
        check (link != nullptr && ! link->isVisible(), "back to the song hides it again");
    }

    auto s = proc->getSong();
    const auto& a = s->analysis;
    std::printf ("  title \"%s\", key %s, %.2f BPM, %.1f LUFS, folder %s\n", s->title.toRawUTF8(), a.key.name().c_str(), a.tempo.bpm,
                 a.loudness.integratedLufs, s->folder.getFullPathName().toRawUTF8());
    int wavs = 0;
    for (auto stem : juststem::stemOrder) wavs += s->stemFile (stem).existsAsFile() ? 1 : 0;
    check (wavs == 6 && s->instrumentalFile().existsAsFile(), juce::String (wavs) + " stem WAVs + instrumental written");

    std::printf ("Preview audio (host at 48 kHz)\n");
    auto& pl = proc->player();
    // Where playback tests start: a minute in, or earlier in a short song (with room to play on).
    const double testPosition = std::min (60.0, proc->getSong()->durationSeconds * 0.45);
    check (renderRms (*proc, 20) == 0.0, "silent while stopped");
    pl.seek (testPosition);
    pl.setPlaying (true);
    const double all = renderRms (*proc, 200);
    check (all > 0.01, "plays all stems (RMS " + juce::String (all, 4) + ")");
    for (int i = 0; i < juststem::numStems; ++i) pl.setMute (i, true);
    renderRms (*proc, 2); // let the gain ramp finish
    check (renderRms (*proc, 50) < 1e-6, "all muted -> silent");
    for (int i = 0; i < juststem::numStems; ++i) pl.setMute (i, false);
    pl.setSolo (0, true); // vocals only
    renderRms (*proc, 2);
    const double vox = renderRms (*proc, 200);
    check (vox > 0.001 && vox < all, "solo vocals is quieter than the full mix (" + juce::String (vox, 4) + " < " + juce::String (all, 4) + ")");
    check (pl.positionSeconds() > testPosition, "position advances (" + juce::String (pl.positionSeconds(), 2) + " s)");
    pl.setSolo (0, false);
    {
        // Some hosts send more samples than prepareToPlay announced (512 here).
        juce::AudioBuffer<float> big (2, 4096);
        juce::MidiBuffer midi;
        big.clear();
        const double before = pl.positionSeconds();
        proc->processBlock (big, midi);
        const double advanced = pl.positionSeconds() - before;
        check (big.getRMSLevel (0, 3584, 512) > 0.001 && std::fabs (advanced - 4096 / 48000.0) < 0.002,
               "a 4096-sample block from a host prepared for 512 plays all of it (" + juce::String (advanced * 1000.0, 1) + " ms)");
    }
    pl.setVolume (1, 0.5f);
    pl.setPlaying (false);

    std::printf ("Chords\n");
    check (a.chords.segments.size() > 4 && a.chords.beats.size() > 100,
           juce::String ((int) a.chords.beats.size()) + " beats, " + juce::String ((int) a.chords.segments.size()) + " chord changes");
    auto* chordsTab = findChild<juststem::ui::TabButton> (*editor, [] (juststem::ui::TabButton& b) { return b.getButtonText() == "Chords"; });
    check (chordsTab != nullptr && chordsTab->isVisible(), "Chords tab shown");
    if (chordsTab != nullptr) chordsTab->triggerClick();
    pumpUntil ([] { return false; }, 200);
    auto* view = findChild<juststem::ui::ChordView> (*editor);
    check (view != nullptr && view->isVisible(), "chord view shown");
    if (auto* cv = dynamic_cast<juststem::ui::ChordView*> (view))
    {
        auto wheel = [cv] (float dx, float dy, bool smooth) {
            const auto now = juce::Time::getCurrentTime();
            const juce::MouseEvent e (juce::Desktop::getInstance().getMainMouseSource(), {}, {}, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                      cv, cv, now, {}, now, 1, false);
            cv->mouseWheelMove (e, { dx, dy, false, smooth, false });
        };
        const int row = cv->firstVisibleRow();
        wheel (0.3f, 0.0f, true); // a sideways trackpad swipe
        const bool sidewaysIgnored = cv->firstVisibleRow() == row;
        for (int i = 0; i < 4; ++i) wheel (0.0f, -0.02f, true); // trackpad steps adding up to just over a row
        const int afterTrackpad = cv->firstVisibleRow();
        wheel (0.0f, -0.234f, false); // a mouse-wheel click
        check (sidewaysIgnored && afterTrackpad == row + 1 && cv->firstVisibleRow() == row + 2,
               "chord grid scrolling: sideways ignored, a row per row-height of trackpad travel, a row per wheel click");
    }
    for (int i = 0; i < 2; ++i)
        if (auto* up = findButton (*editor, "Transpose up"); up != nullptr && up->isVisible())
            up->triggerClick();
    pumpUntil ([] { return false; }, 300);
    check (proc->getTranspose() == 2, "transpose +2");
    const auto chart = juststem::ui::ChordView::chartText (*s, 2);
    check (chart.contains ("transposed +2") && chart.contains ("| "), "chord chart text for the clipboard");
    std::printf ("%s", chart.upToFirstOccurrenceOf ("\n\n", false, false).toRawUTF8());
    std::printf ("\n%s\n", chart.fromFirstOccurrenceOf ("\n\n", false, false).upToFirstOccurrenceOf ("\n", false, false).toRawUTF8());
    snapshot (*editor, outDir.getChildFile ("5-chords.png"));

    std::printf ("Transposed preview (+2) and transposed save\n");
    {
        // Same passage without and with the shift: the loudness envelopes must line up (no delay).
        auto envelope = [] (const std::vector<float>& x) {
            std::vector<double> e;
            for (std::size_t i = 0; i + 480 <= x.size(); i += 480) // 10 ms at 48 kHz
            {
                double v = 0;
                for (std::size_t k = i; k < i + 480; ++k) v += (double) x[k] * x[k];
                e.push_back (std::sqrt (v / 480));
            }
            return e;
        };
        proc->setTranspose (0);
        pl.seek (testPosition);
        pl.setPlaying (true);
        const auto plain = envelope (render (*proc, 400));
        proc->setTranspose (2);
        pl.seek (testPosition);
        const auto shiftedAudio = render (*proc, 400);
        const auto shifted = envelope (shiftedAudio);
        const double listened = pl.positionSeconds();
        int bestLag = 0;
        double best = -1;
        for (int lag = -20; lag <= 20; ++lag) // +-200 ms
        {
            double c = 0;
            for (int i = 50; i < (int) plain.size() - 50; ++i) c += plain[(size_t) i] * shifted[(size_t) (i + lag)];
            if (c > best) { best = c; bestLag = lag; }
        }
        double rmsShifted = 0;
        for (std::size_t i = shiftedAudio.size() / 4; i < shiftedAudio.size(); ++i) rmsShifted += (double) shiftedAudio[i] * shiftedAudio[i];
        rmsShifted = std::sqrt (rmsShifted / (double) (shiftedAudio.size() * 3 / 4));
        check (std::abs (bestLag) <= 1, "shifted preview in time with the original (" + juce::String (bestLag * 10) + " ms)");
        check (rmsShifted > 0.05, "shifted preview plays (RMS " + juce::String (rmsShifted, 3) + ")");
        check (std::fabs (listened - (testPosition + 400 * 512 / 48000.0)) < 0.02, "position shows what you hear (" + juce::String (listened, 2) + " s)");
        // A host may prepare the plug-in again (e.g. a new block size) while it plays transposed.
        const double beforePrepare = pl.positionSeconds();
        proc->prepareToPlay (48000, 512);
        render (*proc, 1);
        const double moved = pl.positionSeconds() - beforePrepare;
        check (std::fabs (moved - 512 / 48000.0) < 0.005,
               "preparing again while transposed keeps the position (moved " + juce::String (juce::roundToInt (moved * 1000)) + " ms in an 11 ms block)");
        pl.setPlaying (false);

        const auto dir = outDir.getChildFile ("transposed stems");
        dir.deleteRecursively();
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        double lastProgress = 0;
        const bool ok = juststem::saveTransposedStems (*s, 2, dir, [&] (double f) { lastProgress = f; return true; });
        const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        int files = 0;
        bool sameLength = true;
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.wav"))
        {
            ++files;
            sameLength = sameLength && juststem::loadAudioFile (f).size() == s->stems.at (juststem::Stem::Vocals).size();
        }
        check (ok && files == 7 && sameLength && lastProgress > 0.99,
               juce::String (files) + " transposed WAVs, same length as the originals, in " + juce::String (secs, 1) + " s");
        dir.deleteRecursively();

        // Save all without Transpose: copies of the saved WAVs.
        const auto copies = outDir.getChildFile ("copied stems");
        copies.deleteRecursively();
        const bool copied = juststem::copyStems (*s, copies, [] (double) { return true; });
        int same = copies.getChildFile (s->instrumentalFile().getFileName()).getSize() == s->instrumentalFile().getSize() ? 1 : 0;
        for (auto stem : juststem::stemOrder) same += copies.getChildFile (s->stemFile (stem).getFileName()).getSize() == s->stemFile (stem).getSize() ? 1 : 0;
        check (copied && same == 7, "Save all copies the 6 stems and the instrumental");
        copies.deleteRecursively();
    }

    std::printf ("Save and reload, like reopening a DAW project\n");
    juce::MemoryBlock state;
    proc->getStateInformation (state);
    {
        // The host restores the state with the same song open (so nothing reloads): the window must still update.
        struct Listener final : juce::ChangeListener
        {
            int calls = 0;
            void changeListenerCallback (juce::ChangeBroadcaster*) override { ++calls; }
        } listener;
        proc->addChangeListener (&listener);
        proc->setStateInformation (state.getData(), (int) state.getSize());
        pumpUntil ([&] { return listener.calls > 0; }, 1000);
        proc->removeChangeListener (&listener);
        check (listener.calls > 0, "restoring the state refreshes the open window");
    }
    auto proc2 = std::make_unique<justStemProcessor> (model);
    proc2->prepareToPlay (44100, 256);
    proc2->setStateInformation (state.getData(), (int) state.getSize());
    pumpUntil ([&] { return proc2->getSong() != nullptr && ! proc2->isBusy(); }, 60000);
    check (proc2->getSong() != nullptr && proc2->getSong()->title == s->title, "song reloaded from the saved state");
    check (proc2->player().getVolume (1) == 0.5f, "stem volume restored");
    if (proc2->getSong())
    {
        const auto& b = proc2->getSong()->analysis;
        check (b.key.name() == a.key.name() && std::fabs (b.tempo.bpm - a.tempo.bpm) < 0.01f,
               "analysis restored (" + juce::String (b.key.name()) + ", " + juce::String (b.tempo.bpm, 2) + " BPM)");
        check (b.chords.segments.size() == a.chords.segments.size() && b.chords.firstDownbeat == a.chords.firstDownbeat
                   && b.chords.segments.back().chord == a.chords.segments.back().chord,
               "chords restored (" + juce::String ((int) b.chords.segments.size()) + " changes)");
        check (proc2->getTranspose() == 2, "transpose restored");
    }

    std::printf ("Song saved before chord detection existed\n");
    {
        auto info = juce::JSON::parse (s->infoFile());
        if (auto* obj = info.getDynamicObject()) { obj->removeProperty ("chords"); obj->removeProperty ("beats"); }
        s->infoFile().replaceWithText (juce::JSON::toString (info));
        auto proc3 = std::make_unique<justStemProcessor> (model);
        proc3->setStateInformation (state.getData(), (int) state.getSize());
        pumpUntil ([&] { return proc3->getSong() != nullptr && ! proc3->isBusy(); }, 60000);
        const auto song3 = proc3->getSong();
        check (song3 != nullptr && song3->analysis.chords.segments.size() == a.chords.segments.size(),
               "chords worked out on reload (" + juce::String (song3 ? (int) song3->analysis.chords.segments.size() : 0) + " changes)");
        check (juce::JSON::parse (s->infoFile())["chords"].size() == (int) a.chords.segments.size(), "and saved to info.json");
    }

    std::printf ("Closing the plug-in while it works\n");
    {
        // Loading (or, on a first run, compiling) the model can't stop part-way: closing the plug-in
        // then mustn't wait for it, or the DAW freezes.
        auto busy = std::make_unique<justStemProcessor> (model);
        busy->startFile (song);
        pumpUntil ([&] { return busy->jobStatus().step.startsWith ("Preparing"); }, 60000);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        busy.reset();
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        check (ms < 100, "closing it while the model loads doesn't wait for the job (" + juce::String (juce::roundToInt (ms)) + " ms)");
        pumpUntil ([] { return juststem::SongJob::numAbandoned() == 0; }, 10000);
        check (juststem::SongJob::numAbandoned() == 0, "the abandoned job ends and deletes itself");
    }

    std::printf ("A stem edited outside justStem\n");
    {
        // Trimmed to 10 s, e.g. in an audio editor: on reload every stem must still have one length.
        auto drums = s->stems.at (juststem::Stem::Drums);
        drums.left.resize ((size_t) juststem::sampleRate * 10);
        drums.right.resize (drums.left.size());
        juststem::writeWav (s->stemFile (juststem::Stem::Drums), drums);
        const auto edited = juststem::loadSong (s->folder);
        bool sameLength = true;
        for (const auto& [stem, audio] : edited->stems) sameLength = sameLength && audio.size() == s->stems.at (juststem::Stem::Vocals).size();
        check (sameLength && edited->stems.at (juststem::Stem::Drums).left.back() == 0.0f, "a shortened stem is padded to the others' length on reload");
    }

    std::printf ("A damaged info.json\n");
    {
        auto info = juce::JSON::parse (s->infoFile());
        if (auto* obj = info.getDynamicObject())
        {
            obj->setProperty ("keyTonic", 99);
            obj->setProperty ("firstDownbeat", -7);
        }
        s->infoFile().replaceWithText (juce::JSON::toString (info));
        const auto damaged = juststem::loadSong (s->folder);
        const auto& k = damaged->analysis.key;
        const bool inRange = k.tonic >= 0 && k.tonic < 12 && damaged->analysis.chords.firstDownbeat >= 0;
        check (inRange, "values out of range in info.json are brought into range" + (inRange ? " (key " + juce::String (k.name()) + ")" : juce::String()));
    }

    editor.reset();
    proc2.reset();
    s->folder.deleteRecursively(); // the test's own stems
    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
