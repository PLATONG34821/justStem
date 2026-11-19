// juststem_uitest <model.onnx> <song> <output folder>: drives the real plug-in code without a DAW.
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
    if (argc < 4) { std::fprintf (stderr, "usage: juststem_uitest <model.onnx> <song> <output folder>\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::File model (juce::String::fromUTF8 (argv[1])), song (juce::String::fromUTF8 (argv[2])),
        outDir (juce::String::fromUTF8 (argv[3]));
    outDir.createDirectory();

    auto proc = std::make_unique<justStemProcessor> (model);
    proc->prepareToPlay (48000, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());

    std::printf ("UI\n");
    snapshot (*editor, outDir.getChildFile ("1-idle.png"));

    proc->startFile (song);
    pumpUntil ([&] { auto s = proc->jobStatus(); return s.step.startsWith ("Splitting") && s.fraction > 0.3; }, 120000);
    snapshot (*editor, outDir.getChildFile ("2-progress.png"));
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    pumpUntil ([&] { return ! proc->isBusy(); }, 600000);
    std::printf ("  job finished %.1f s after the progress snapshot\n", (juce::Time::getMillisecondCounterHiRes() - t0) / 1000);
    check (proc->getSong() != nullptr, "song processed" + (proc->lastError().isEmpty() ? juce::String() : ": " + proc->lastError()));
    if (proc->getSong() == nullptr) return 1;
    pumpUntil ([] { return false; }, 300); // let the editor refresh
    snapshot (*editor, outDir.getChildFile ("3-results.png"));

    std::printf ("Storage panel\n");
    auto findChild = [&] (auto pred) -> juce::Component* {
        for (auto* c : editor->getChildren()) if (pred (c)) return c;
        return nullptr;
    };
    auto* storageButton = dynamic_cast<juce::Button*> (findChild ([] (juce::Component* c) {
        auto* b = dynamic_cast<juce::Button*> (c);
        return b != nullptr && b->getButtonText() == "Storage";
    }));
    check (storageButton != nullptr && storageButton->isVisible(), "Storage button shown");
    if (storageButton != nullptr) storageButton->triggerClick();
    pumpUntil ([] { return false; }, 300);
    auto* panel = dynamic_cast<juststem::ui::StoragePanel*> (findChild ([] (juce::Component* c) {
        return dynamic_cast<juststem::ui::StoragePanel*> (c) != nullptr;
    }));
    check (panel != nullptr, "Storage panel opens");
    if (panel != nullptr)
    {
        snapshot (*editor, outDir.getChildFile ("4-storage.png"));
        const auto saved = juststem::storage::savedSongs();
        check (std::any_of (saved.begin(), saved.end(), [&] (const auto& x) { return x.folder == proc->getSong()->folder; }),
               "current song listed in storage");
        panel->onClose();
        pumpUntil ([] { return false; }, 200);
        check (findChild ([] (juce::Component* c) { return dynamic_cast<juststem::ui::StoragePanel*> (c) != nullptr; }) == nullptr,
               "Storage panel closes");
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
    check (renderRms (*proc, 20) == 0.0, "silent while stopped");
    pl.seek (60.0);
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
    check (pl.positionSeconds() > 60.0, "position advances (" + juce::String (pl.positionSeconds(), 2) + " s)");
    pl.setSolo (0, false);
    pl.setVolume (1, 0.5f);
    pl.setPlaying (false);

    std::printf ("Chords\n");
    check (a.chords.segments.size() > 4 && a.chords.beats.size() > 100,
           juce::String ((int) a.chords.beats.size()) + " beats, " + juce::String ((int) a.chords.segments.size()) + " chord changes");
    auto* chordsTab = dynamic_cast<juce::Button*> (findChild ([] (juce::Component* c) {
        auto* b = dynamic_cast<juststem::ui::TabButton*> (c);
        return b != nullptr && b->getButtonText() == "Chords";
    }));
    check (chordsTab != nullptr && chordsTab->isVisible(), "Chords tab shown");
    if (chordsTab != nullptr) chordsTab->triggerClick();
    pumpUntil ([] { return false; }, 200);
    auto* view = findChild ([] (juce::Component* c) { return dynamic_cast<juststem::ui::ChordView*> (c) != nullptr; });
    check (view != nullptr && view->isVisible(), "chord view shown");
    for (int i = 0; i < 2; ++i)
        if (auto* up = dynamic_cast<juce::Button*> (findChild ([] (juce::Component* c) {
                auto* b = dynamic_cast<juce::TextButton*> (c);
                return b != nullptr && b->getButtonText() == "+" && b->isVisible();
            })))
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
        pl.seek (60.0);
        pl.setPlaying (true);
        const auto plain = envelope (render (*proc, 400));
        proc->setTranspose (2);
        pl.seek (60.0);
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
        check (std::fabs (listened - (60.0 + 400 * 512 / 48000.0)) < 0.02, "position shows what you hear (" + juce::String (listened, 2) + " s)");
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
    }

    std::printf ("Save and reload, like reopening a DAW project\n");
    juce::MemoryBlock state;
    proc->getStateInformation (state);
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

    editor.reset();
    proc2.reset();
    s->folder.deleteRecursively(); // the test's own stems
    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : (juce::String (failures) + " FAILED").toRawUTF8());
    return failures == 0 ? 0 : 1;
}
