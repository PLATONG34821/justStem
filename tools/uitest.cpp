// juststem_uitest <model.onnx> <--synth | song> <output folder> [--hq]: drives the real plug-in code without a DAW.
// Renders the editor to PNGs, runs a full split, checks the preview audio and state save/reload.
#include "../src/io/AudioFiles.h"
#include "../src/plugin/PluginEditor.h"
#include "../src/plugin/PluginProcessor.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <numbers>

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
    const auto end = juce::Time::getMillisecondCounterHiRes() + timeoutMs;
    while (! done() && juce::Time::getMillisecondCounterHiRes() < end)
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
    if (argc < 4) { std::fprintf (stderr, "usage: juststem_uitest <model.onnx> <--synth | song> <output folder> [--hq]\n"); return 2; }
    const bool hq = argc > 4 && std::string (argv[4]) == "--hq";
    juce::ScopedJuceInitialiser_GUI gui;
    // UTF-8 paths, relative to the current folder unless absolute.
    auto fileArg = [] (const char* arg) { return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (arg)); };
    const juce::File model = fileArg (argv[1]), outDir = fileArg (argv[3]);
    outDir.createDirectory();
    const bool useSynth = std::string (argv[2]) == "--synth";
    const juce::File song = useSynth ? outDir.getChildFile ("synth-song.wav") : fileArg (argv[2]);
    if (useSynth)
    {
        // 68 s at 128 BPM (36 bars of Am - F - C - G): drums, bass and chord keys, like `juststem_cli selftest`.
        constexpr double bpm = 128.0, beatSec = 60.0 / bpm, seconds = 68.0;
        constexpr int sr = juststem::sampleRate, n = (int) (seconds * sr);
        juststem::StereoAudio synth;
        synth.left.assign ((size_t) n, 0.0f);
        synth.right.assign ((size_t) n, 0.0f);

        std::uint32_t seed = 17;
        auto rnd = [&] { seed = (seed * 1103515245u + 12345u) & 0x7fffffffu; return (double) seed / 0x7fffffff * 2.0 - 1.0; };
        const int prog[4][4] { { 57, 60, 64, 69 }, { 53, 57, 60, 65 }, { 48, 55, 60, 64 }, { 55, 59, 62, 67 } };

        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sr;
            const double beatPos = t / beatSec;
            const int beatIdx = (int) std::floor (beatPos);
            const double b = (beatPos - beatIdx) * beatSec;
            const double barT = std::fmod (t, 4.0 * beatSec);
            const double e8 = std::fmod (t, beatSec * 0.5);
            const int bar = (beatIdx / 4) % 4;
            const int inBar = beatIdx % 4;

            double drum = 0.0;
            if ((inBar == 0 || inBar == 2) && b < 0.25)
                drum += std::sin (2.0 * std::numbers::pi * (52.0 + 75.0 * std::exp (-b * 30.0)) * b) * std::exp (-b * 12.0) * 0.50;
            if ((inBar == 1 || inBar == 3) && b < 0.16)
                drum += rnd() * std::exp (-b * 26.0) * 0.32;
            if (e8 < 0.04)
                drum += rnd() * std::exp (-e8 * 110.0) * 0.07;

            const int* ch = prog[bar];
            const double fBass = 440.0 * std::pow (2.0, (ch[0] - 24 - 69) / 12.0);
            double bass = 0.0;
            for (int h = 1; h <= 5; ++h) bass += std::sin (2.0 * std::numbers::pi * fBass * h * barT) / h;
            bass *= 0.16 * (0.6 + 0.4 * std::exp (-b * 4.0));

            double keys = 0.0;
            for (int m = 1; m < 4; ++m)
            {
                const double fk = 440.0 * std::pow (2.0, (ch[m] - 69) / 12.0);
                for (int h = 1; h <= 4; ++h) keys += std::sin (2.0 * std::numbers::pi * fk * h * barT) * std::exp (-h * 0.6);
            }
            keys *= 0.06 * std::exp (-b * 2.2);

            synth.left[(size_t) i] = (float) (drum + bass + keys * 1.15);
            synth.right[(size_t) i] = (float) (drum + bass + keys * 0.85);
        }
        juststem::writeWav (song, synth, 24);
    }

    auto proc = std::make_unique<justStemProcessor> (model);
    struct HostListener final : juce::AudioProcessorListener
    {
        int nonParamChanges = 0;
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& d) override
        {
            if (d.nonParameterStateChanged) ++nonParamChanges;
        }
    } hostListener;
    proc->addListener (&hostListener);
    proc->prepareToPlay (48000, 512);
    if (hq)
    {
        if (! proc->hqAvailable()) { std::fprintf (stderr, "--hq: no HQ model next to %s\n", model.getFullPathName().toRawUTF8()); return 2; }
        proc->setHQ (true);
    }
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());

    std::printf ("UI\n");
    check (editor->getWidth() == 900 && editor->getHeight() == 620, "window is 900 x 620 (fits 720p screens)");
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
    check (hostListener.nonParamChanges > 0, "splitting a song notifies the host");
    for (const auto& line : proc->jobLog()) std::printf ("    | %s\n", line.toRawUTF8());
    if (proc->getSong() == nullptr) return 1;
    pumpUntil ([] { return false; }, 300); // let the editor refresh
    snapshot (*editor, outDir.getChildFile ("3-results.png"));
    {
        // Screen readers name each drag handle by its row.
        int handles = 0, named = 0, midiShown = 0;
        juststem::ui::DragHandle* firstHandle = nullptr;
        std::function<void (juce::Component&)> walk = [&] (juce::Component& c) {
            for (auto* child : c.getChildren())
            {
                if (auto* d = dynamic_cast<juststem::ui::DragHandle*> (child))
                {
                    if (firstHandle == nullptr) firstHandle = d;
                    ++handles;
                    named += d->getTitle().startsWith ("Drag ") ? 1 : 0;
                    midiShown += d->getTitle().contains (" MIDI ") && d->isVisible() ? 1 : 0;
                }
                walk (*child);
            }
        };
        walk (*editor);
        // 8 for the audio (6 stems + 2 mixes), and a MIDI one on each stem row, shown when the stem has MIDI.
        check (handles == 14 && named == 14, juce::String (named) + " of " + juce::String (handles) + " drag handles named for screen readers");
        int midiFiles = 0;
        for (auto stem : juststem::stemOrder) midiFiles += proc->getSong()->midiFile (stem).existsAsFile() ? 1 : 0;
        check (midiShown == midiFiles && midiFiles > 0, juce::String (midiShown) + " MIDI handles shown, one for each MIDI file ("
                                                              + juce::String (midiFiles) + ")");
        check (firstHandle != nullptr && ! firstHandle->getTooltip().contains ("Save all"),
               "drag handle tooltip is plain when not transposed");
        auto* title = findChild<juststem::ui::SongTitle> (*editor);
        auto titleHandler = title != nullptr ? title->createAccessibilityHandler() : nullptr;
        check (title != nullptr && title->isAccessible() && titleHandler != nullptr
                   && titleHandler->getRole() == juce::AccessibilityRole::staticText
                   && title->getTitle().contains (proc->getSong()->title),
               "song title is named for screen readers");
    }

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
        check (panel->about.contains (JUSTSTEM_VERSION), "Storage shows the version: " + panel->about);
        const auto saved = juststem::storage::savedSongs();
        check (std::any_of (saved.begin(), saved.end(), [&] (const auto& x) { return x.folder == proc->getSong()->folder; }),
               "current song listed in storage");
        // A shared fast model in paths::models() (macOS "Install for me only") must not show up as HQ AI model.
        const auto fastInUserModels = juststem::paths::models().getChildFile (juststem::text::from (juststem::model::fast.fileName));
        const bool hadFast = fastInUserModels.existsAsFile();
        if (! hadFast) fastInUserModels.replaceWithText ("dummy fast model");
        panel->refresh();
        auto* clearHq = findChild<juce::Button> (*panel, [] (juce::Button& b) { return b.getTitle() == "Remove downloaded AI models"; });
        bool downloaded = false;
        for (const auto name : { juststem::model::hq.fileName, juststem::model::pianoFile, juststem::model::pianoHeadsFile, juststem::model::pianoFrontEndFile })
            downloaded = downloaded || juststem::paths::models().getChildFile (juststem::text::from (name)).existsAsFile();
        check (clearHq != nullptr && (downloaded ? clearHq->isEnabled() : ! clearHq->isEnabled()),
               "Storage downloaded models row ignores the fast model in paths::models()");
        const auto leftover = juststem::paths::models().getChildFile ("bs_roformer_sw_6stem_opt-uitest.part");
        leftover.replaceWithText ("interrupted download");
        panel->refresh();
        check (clearHq != nullptr && clearHq->isEnabled(), "Storage downloaded models row counts interrupted downloads (.part)");
        leftover.deleteFile();
        if (! hadFast) fastInUserModels.deleteFile();
        panel->refresh();
        check (editor->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)), "Escape handled while Storage is open");
        pumpUntil ([] { return false; }, 200);
        check (findChild<juststem::ui::StoragePanel> (*editor) == nullptr,
               "Storage panel closes on Escape");
    }
    {
        bool dismissed = false;
        juststem::ui::UpdatePopup popup ("9.9.9");
        popup.onLater = [&] { dismissed = true; };
        check (popup.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)) && dismissed,
               "Update popup closes on Escape");
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
    int flacs = 0;
    for (auto stem : juststem::stemOrder) flacs += s->stemFile (stem).existsAsFile() && s->stemFile (stem).hasFileExtension ("flac") ? 1 : 0;
    check (flacs == 6 && s->instrumentalFile().existsAsFile() && s->fullMixFile().existsAsFile(),
           juce::String (flacs) + " stem FLACs + instrumental + full mix written");
    {
        // MIDI for the pitched stems that are in the song; the bass plays the roots of Am - F - C - G.
        juce::StringArray made;
        for (const auto& m : juststem::midiStems)
            if (s->midiFile (m.stem).existsAsFile()) made.add (juce::String (std::string (juststem::stemName (m.stem))));
        std::printf ("  MIDI made for: %s\n", made.joinIntoString (", ").toRawUTF8());
        juce::FileInputStream in (s->midiFile (juststem::Stem::Bass));
        juce::MidiFile bassMidi;
        int notes = 0, roots = 0;
        if (in.openedOk() && bassMidi.readFrom (in))
            for (const auto* e : *bassMidi.getTrack (0))
                if (e->message.isNoteOn())
                {
                    ++notes;
                    const int pc = e->message.getNoteNumber() % 12;
                    roots += pc == 9 || pc == 5 || pc == 0 || pc == 7 ? 1 : 0;
                }
        check (notes > 10 && roots >= notes * 8 / 10, "bass MIDI: " + juce::String (notes) + " notes, " + juce::String (roots)
                                                           + " on the chord roots (A, F, C, G)");
    }
    {
        auto* pluginBar = findChild<juststem::ui::PlayerBar> (*editor);
        auto* pluginFollow = pluginBar != nullptr ? findChild<juce::ToggleButton> (*pluginBar) : nullptr;
        juststem::ui::PlayerBar standaloneBar (proc->player(), false);
        standaloneBar.setSize (800, 36);
        standaloneBar.setSong (s);
        auto* standaloneFollow = findChild<juce::ToggleButton> (standaloneBar);
        check (pluginFollow != nullptr && pluginFollow->isVisible() && standaloneFollow != nullptr && ! standaloneFollow->isVisible(),
               "Follow DAW shows in the plug-in and hides in the standalone app");
    }
    {
        // The full mix is every stem added together (FLAC is lossless; 24-bit rounding aside).
        const auto mixed = juststem::loadAudioFile (s->fullMixFile());
        float worst = 0;
        for (std::size_t i = 0; i < mixed.size(); i += 97)
        {
            float sum = 0;
            for (const auto& [stem, audio] : s->stems) sum += audio.left[i];
            worst = std::max (worst, std::abs (mixed.left[i] - sum));
        }
        check (mixed.size() == s->stems.at (juststem::Stem::Vocals).size() && worst < 1e-4f,
               "the full mix is the stems added together (largest difference " + juce::String (worst, 7) + ")");
    }
    check (s->audioHash.isNotEmpty() && juststem::findSavedSong (s->audioHash, s->model) == s->folder
               && juststem::findSavedSong (s->audioHash, "another model") == juce::File(),
           "the same song with the same model is found among the saved songs");
    auto& dropTarget = dynamic_cast<juce::FileDragAndDropTarget&> (*editor);
    check (! dropTarget.isInterestedInFileDrag ({ s->stemFile (juststem::Stem::Vocals).getFullPathName() })
               && ! dropTarget.isInterestedInFileDrag ({ s->instrumentalFile().getFullPathName() })
               && ! dropTarget.isInterestedInFileDrag ({ s->fullMixFile().getFullPathName() })
               && dropTarget.isInterestedInFileDrag ({ song.getFullPathName() }),
           "a stem dragged out and dropped back on the window isn't split again; other audio files are");

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
    const int soloStem = useSynth ? 2 : 0; // bass in --synth (an instrumental band), vocals in a song
    const juce::String soloName = useSynth ? "bass" : "vocals";
    pl.setSolo (soloStem, true);
    renderRms (*proc, 2);
    const double solo = renderRms (*proc, 200);
    check (solo > 0.001 && solo < all, "solo " + soloName + " is quieter than the full mix (" + juce::String (solo, 4) + " < " + juce::String (all, 4) + ")");
    check (pl.positionSeconds() > testPosition, "position advances (" + juce::String (pl.positionSeconds(), 2) + " s)");
    pl.setSolo (soloStem, false);
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
    check (editor->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)) && pl.isPlaying()
               && editor->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)) && ! pl.isPlaying(),
           "Spacebar toggles preview play and pause");
    const int afterPlayerChanges = hostListener.nonParamChanges;
    pl.setMute (0, true);
    pl.setMute (0, false);
    check (hostListener.nonParamChanges == afterPlayerChanges + 2, "mute, solo and volume notify the host");

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
        const int afterWheel = cv->firstVisibleRow();
        wheel (0.0f, 0.234f, false); // and back up, so the screenshot shows the bar playing
        wheel (0.0f, 0.234f, false);
        check (sidewaysIgnored && afterTrackpad == row + 1 && afterWheel == row + 2 && cv->firstVisibleRow() == row,
               "chord grid scrolling: sideways ignored, a row per row-height of trackpad travel, a row per wheel click");
    }
    for (int i = 0; i < 2; ++i)
        if (auto* up = findButton (*editor, "Transpose up"); up != nullptr && up->isVisible())
            up->triggerClick();
    pumpUntil ([] { return false; }, 300);
    check (proc->getTranspose() == 2, "transpose +2");
    check (hostListener.nonParamChanges > afterPlayerChanges + 2, "transpose notifies the host");
    {
        juststem::ui::DragHandle* firstHandle = nullptr;
        std::function<void (juce::Component&)> findDrag = [&] (juce::Component& c) {
            for (auto* child : c.getChildren())
            {
                if (firstHandle == nullptr)
                    if (auto* d = dynamic_cast<juststem::ui::DragHandle*> (child)) firstHandle = d;
                findDrag (*child);
            }
        };
        findDrag (*editor);
        check (firstHandle != nullptr && firstHandle->getTooltip().contains ("original key")
                   && firstHandle->getTooltip().contains ("Save all"),
               "drag handle notes original key and Save all when transposed");
    }
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

        // Edge bursts: [0, 70 ms) and [1.93 s, 2.00 s) in a 2 s song must both play when transposed.
        {
            auto edgeSong = std::make_shared<juststem::SongResult>();
            const std::size_t n2 = (std::size_t) juststem::sampleRate * 2;
            edgeSong->durationSeconds = 2.0;
            for (auto stem : juststem::stemOrder)
            {
                edgeSong->stems[stem].left.assign (n2, 0.0f);
                edgeSong->stems[stem].right.assign (n2, 0.0f);
            }
            auto& v = edgeSong->stems[juststem::Stem::Vocals];
            for (std::size_t i = 0; i < (std::size_t) (juststem::sampleRate * 0.07); ++i)
                v.left[i] = v.right[i] = 0.4f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f * (float) i / (float) juststem::sampleRate);
            for (std::size_t i = (std::size_t) (juststem::sampleRate * 1.93); i < n2; ++i)
                v.left[i] = v.right[i] = 0.4f * std::sin (2.0f * juce::MathConstants<float>::pi * 440.0f * (float) i / (float) juststem::sampleRate);

            juststem::StemPlayer edgePlayer;
            edgePlayer.prepare (48000, 512);
            edgePlayer.setSong (edgeSong);
            edgePlayer.setTranspose (2);
            auto renderVec = [&] (int blocks) {
                juce::AudioBuffer<float> buf (2, 512);
                std::vector<float> y;
                for (int b = 0; b < blocks; ++b)
                {
                    buf.clear();
                    edgePlayer.process (buf, -1.0);
                    y.insert (y.end(), buf.getReadPointer (0), buf.getReadPointer (0) + 512);
                }
                return y;
            };
            auto rmsSlice = [] (const std::vector<float>& x, std::size_t a, std::size_t b) {
                double e = 0;
                for (std::size_t i = a; i < b; ++i) e += (double) x[i] * x[i];
                return std::sqrt (e / (double) std::max<std::size_t> (1, b - a));
            };
            edgePlayer.setPlaying (true);
            const auto atStart = renderVec (8); // ~85 ms
            edgePlayer.seek (0.0);
            const auto afterSeek = renderVec (8);
            edgePlayer.seek (1.85);
            const auto atEnd = renderVec (20); // past 2.00 s (~213 ms)
            check (rmsSlice (atStart, 0, 3360) > 0.05 && rmsSlice (afterSeek, 0, 3360) > 0.05,
                   "transposed preview plays the first 70 ms at start and after seek");
            check (rmsSlice (atEnd, 3840, 7200) > 0.05 && ! edgePlayer.isPlaying(),
                   "transposed preview flushes the final 70 ms before stopping");
        }

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
        check (ok && files == 8 && sameLength && lastProgress > 0.99,
               juce::String (files) + " transposed WAVs, same length as the originals, in " + juce::String (secs, 1) + " s");
        {
            auto firstNote = [] (const juce::File& f) {
                juce::FileInputStream in (f);
                juce::MidiFile m;
                if (in.openedOk() && m.readFrom (in))
                    for (const auto* e : *m.getTrack (0))
                        if (e->message.isNoteOn()) return e->message.getNoteNumber();
                return -1;
            };
            const int before = firstNote (s->midiFile (juststem::Stem::Bass));
            const int after = firstNote (dir.getChildFile ("bass.mid"));
            check (before >= 0 && after == before + 2, "transposed MIDI: the bass moved up 2 (" + juce::String (before) + " -> " + juce::String (after) + ")");
        }
        dir.deleteRecursively();

        const auto cancelDir = outDir.getChildFile ("cancelled stems");
        const auto c0 = juce::Time::getMillisecondCounterHiRes();
        const bool finished = juststem::saveTransposedStems (*s, 2, cancelDir, [] (double) { return false; });
        const double cancelMs = juce::Time::getMillisecondCounterHiRes() - c0;
        check (! finished && cancelMs < 1500, "Cancel stops a transposed save (" + juce::String (juce::roundToInt (cancelMs)) + " ms)");
        cancelDir.deleteRecursively();

        // Save all without Transpose: WAVs of the saved stems (lossless, so the same samples).
        const auto copies = outDir.getChildFile ("copied stems");
        copies.deleteRecursively();
        const bool copied = juststem::copyStems (*s, copies, [] (double) { return true; });
        int same = (copies.getChildFile ("instrumental.wav").existsAsFile() ? 1 : 0)
                 + (copies.getChildFile ("full mix.wav").existsAsFile() ? 1 : 0);
        for (auto stem : juststem::stemOrder)
            same += juststem::loadAudioFile (copies.getChildFile (juce::String (juststem::stemWavName (stem)))).left
                            == juststem::loadAudioFile (s->stemFile (stem)).left ? 1 : 0;
        check (copied && same == 8, "Save all writes the 6 stems, the instrumental and the full mix as WAVs");
        int midiCopied = 0, midiSaved = 0;
        for (auto stem : juststem::stemOrder)
            if (s->midiFile (stem).existsAsFile())
            {
                ++midiSaved;
                juce::MemoryBlock copy, original;
                copies.getChildFile (s->midiFile (stem).getFileName()).loadFileAsData (copy);
                s->midiFile (stem).loadFileAsData (original);
                midiCopied += copy == original && original.getSize() > 0 ? 1 : 0;
            }
        check (midiSaved > 0 && midiCopied == midiSaved, "and copies the " + juce::String (midiSaved) + " MIDI files");
        copies.deleteRecursively();

        // Loud transposed export: stems whose shifted sum exceeds 0 dBFS are lowered together.
        juststem::SongResult loudSong;
        const std::size_t n1 = (std::size_t) juststem::sampleRate;
        for (auto stem : juststem::stemOrder)
        {
            loudSong.stems[stem].left.assign (n1, 0.0f);
            loudSong.stems[stem].right.assign (n1, 0.0f);
        }
        for (std::size_t i = 0; i < n1; ++i)
        {
            const float s = std::sin (2.0f * juce::MathConstants<float>::pi * 441.0f * (float) i / (float) juststem::sampleRate);
            loudSong.stems[juststem::Stem::Vocals].left[i] = loudSong.stems[juststem::Stem::Vocals].right[i] = 0.55f * s;
            loudSong.stems[juststem::Stem::Drums].left[i] = loudSong.stems[juststem::Stem::Drums].right[i] = 0.75f * s;
        }
        const auto loudDir = outDir.getChildFile ("loud transposed");
        loudDir.deleteRecursively();
        const bool loudOk = juststem::saveTransposedStems (loudSong, 2, loudDir, [] (double) { return true; });
        float loudPeak = 0.0f;
        for (const auto& name : { juce::String ("full mix.wav"), juce::String ("instrumental.wav"), juce::String ("vocals.wav") })
        {
            const auto a = juststem::loadAudioFile (loudDir.getChildFile (name));
            for (float v : a.left) loudPeak = std::max (loudPeak, std::abs (v));
            for (float v : a.right) loudPeak = std::max (loudPeak, std::abs (v));
        }
        loudDir.deleteRecursively();
        check (loudOk && loudPeak > 0.5f && loudPeak <= 0.995f,
               "loud transposed export stays under full scale (peak " + juce::String (loudPeak, 4) + ")");
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
        const int beforeRestore = hostListener.nonParamChanges;
        proc->setStateInformation (state.getData(), (int) state.getSize());
        pumpUntil ([&] { return listener.calls > 0; }, 1000);
        proc->removeChangeListener (&listener);
        check (listener.calls > 0, "restoring the state refreshes the open window");
        check (hostListener.nonParamChanges == beforeRestore, "restoring the state does not mark the host dirty");
    }
    proc->removeListener (&hostListener);
    auto proc2 = std::make_unique<justStemProcessor> (model);
    proc2->prepareToPlay (44100, 256);
    proc2->setStateInformation (state.getData(), (int) state.getSize());
    {
        // Saving while Reload is still queued/running must preserve the restored song folder.
        juce::MemoryBlock duringReload;
        proc2->getStateInformation (duringReload);
        const auto tree = juce::ValueTree::readFromData (duringReload.getData(), duringReload.getSize());
        check (proc2->getSong() == nullptr && tree["folder"].toString() == s->folder.getFullPathName(),
               "getStateInformation during Reload preserves the song folder");
    }
    pumpUntil ([&] { return proc2->getSong() != nullptr && ! proc2->isBusy(); }, 60000);
    check (proc2->getSong() != nullptr && proc2->getSong()->title == s->title, "song reloaded from the saved state");
    check (proc2->player().getVolume (1) == 0.5f, "stem volume restored");
    {
        // Corrupt or missing stem volumes in state are clamped/defaulted instead of injecting NaN or silence.
        auto badTree = juce::ValueTree::readFromData (state.getData(), state.getSize());
        badTree.getChild (0).removeProperty ("volume", nullptr);
        badTree.getChild (1).setProperty ("volume", std::numeric_limits<double>::quiet_NaN(), nullptr);
        badTree.getChild (2).setProperty ("volume", 99.0, nullptr);
        badTree.getChild (3).setProperty ("volume", -5.0, nullptr);
        juce::MemoryBlock badBlob;
        { juce::MemoryOutputStream out (badBlob, false); badTree.writeToStream (out); }
        proc2->setStateInformation (badBlob.getData(), (int) badBlob.getSize());
        check (proc2->player().getVolume (0) == 1.0f && proc2->player().getVolume (1) == 1.0f
                   && proc2->player().getVolume (2) == 1.5f && proc2->player().getVolume (3) == 0.0f,
               "corrupt or missing stem volumes clamp/default safely");
    }
    if (proc2->getSong())
    {
        const auto& b = proc2->getSong()->analysis;
        check (b.key.name() == a.key.name() && std::fabs (b.tempo.bpm - a.tempo.bpm) < 0.01f,
               "analysis restored (" + juce::String (b.key.name()) + ", " + juce::String (b.tempo.bpm, 2) + " BPM)");
        check (b.chords.segments.size() == a.chords.segments.size() && b.chords.firstDownbeat == a.chords.firstDownbeat
                   && b.chords.segments.back().chord == a.chords.segments.back().chord,
               "chords restored (" + juce::String ((int) b.chords.segments.size()) + " changes)");
        check (proc2->getTranspose() == 2, "transpose restored");

        // Opening a saved song that fails to load keeps the open song in the saved state.
        const auto broken = s->folder.getSiblingFile ("broken-song-uitest");
        broken.createDirectory();
        broken.getChildFile ("info.json").replaceWithText ("not json");
        proc2->openSaved (broken);
        pumpUntil ([&] { return ! proc2->isBusy(); }, 10000);
        juce::MemoryBlock afterFail;
        proc2->getStateInformation (afterFail);
        const auto tree = juce::ValueTree::readFromData (afterFail.getData(), afterFail.getSize());
        check (tree["folder"].toString() == proc2->getSong()->folder.getFullPathName()
                   && ! justStemProcessor::songsInUse().contains (broken),
               "a failed Reload doesn't replace the open song in the saved state");
        broken.deleteRecursively();
    }

    std::printf ("Song saved before chord detection existed\n");
    {
        auto info = juce::JSON::parse (s->infoFile());
        if (auto* obj = info.getDynamicObject()) { obj->removeProperty ("chords"); obj->removeProperty ("beats"); }
        s->infoFile().replaceWithText (juce::JSON::toString (info));
        s->fullMixFile().deleteFile(); // and before the full mix (0.0.5)
        auto proc3 = std::make_unique<justStemProcessor> (model);
        proc3->setStateInformation (state.getData(), (int) state.getSize());
        pumpUntil ([&] { return proc3->getSong() != nullptr && ! proc3->isBusy(); }, 60000);
        const auto song3 = proc3->getSong();
        check (song3 != nullptr && song3->analysis.chords.segments.size() == a.chords.segments.size(),
               "chords worked out on reload (" + juce::String (song3 ? (int) song3->analysis.chords.segments.size() : 0) + " changes)");
        check (juce::JSON::parse (s->infoFile())["chords"].size() == (int) a.chords.segments.size(), "and saved to info.json");
        check (s->fullMixFile().existsAsFile() && juststem::loadAudioFile (s->fullMixFile()).size() == song3->stems.at (juststem::Stem::Vocals).size(),
               "the full mix made on reload");
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

    std::printf ("Closing the plug-in during Save all\n");
    {
        auto expProc = std::make_unique<justStemProcessor> (model);
        expProc->setStateInformation (state.getData(), (int) state.getSize());
        pumpUntil ([&] { return expProc->getSong() != nullptr && ! expProc->isBusy(); }, 60000);
        const auto expDest = outDir.getChildFile ("export-close-test");
        expDest.createDirectory();
        expProc->setTranspose (2);
        expProc->exportStems (expDest);
        check (expProc->isExporting(), "export started and is owned by the processor");
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        expProc.reset();
        const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
        const int left = expDest.getNumberOfChildFiles (juce::File::findFilesAndDirectories);
        expDest.deleteRecursively();
        check (ms < 1500 && left == 0,
               "closing the plug-in mid-export stops cleanly (" + juce::String (juce::roundToInt (ms)) + " ms) and cleans up");
    }

    std::printf ("A stem edited outside justStem\n");
    {
        // Trimmed to 10 s, e.g. in an audio editor: on reload every stem must still have one length.
        auto drums = s->stems.at (juststem::Stem::Drums);
        drums.left.resize ((size_t) juststem::sampleRate * 10);
        drums.right.resize (drums.left.size());
        juststem::writeFlac (s->stemFile (juststem::Stem::Drums), drums);
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
    if (useSynth) song.deleteFile();
    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
