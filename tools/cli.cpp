// juststem_cli: test the separation engine outside a DAW.
//   juststem_cli selftest <model.onnx> [seconds] [overlap]
//   juststem_cli separate <model.onnx> <input audio> <output folder>
//   juststem_cli import <YouTube link | audio file> <output.wav>
//   juststem_cli analyze-selftest
//   juststem_cli analyze <audio file> [model.onnx]   (with a model: analyse the separated stems)
//   juststem_cli chords <song folder>                 (stems saved by the plug-in)
//   juststem_cli midi-selftest <models folder>
//   juststem_cli midi <song folder> <models folder> <output folder> [bpm]
//       (MIDI of the vocals, bass, guitar and piano stems; the folder has basic_pitch.onnx and the
//        transkun_piano files)
// The self-tests are in tools/selftest.cpp.
#include "cli.h"
#include "../src/core/Analysis.h"
#include "../src/core/Transcriber.h"
#include "../src/io/AudioFiles.h"
#include "../src/io/YouTube.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cstdio>

using namespace juststem;

/** A path from the command line (UTF-8; relative to the current folder unless absolute). */
static juce::File fileArg (const char* arg)
{
    return juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (arg));
}

/** Developer tools keep their own CoreML cache, so they don't add copies to the plug-in's. */
std::string devCoremlCache()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("juststem-coreml-cache").getFullPathName().toStdString();
}

void printProgress (const SeparationProgress& p)
{
    std::fprintf (stderr, "\r  [%s] segment %d/%d, eta %.0fs   ", backendName (p.backend), p.segment, p.totalSegments, p.etaSeconds);
}

static int separateFile (const std::string& model, const juce::File& in, const juce::File& outDir)
{
    const auto mix = loadAudioFile (in);
    Separator sep (model, devCoremlCache());
    std::atomic<bool> cancel { false };
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto stems = sep.separate (mix, printProgress, cancel);
    const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    const double length = (double) mix.size() / sampleRate;
    std::fprintf (stderr, "\n%.1fs for %.1fs of audio on %s (%.2fx realtime)\n", secs, length, backendName (sep.backend()), length / secs);
    if (const float db = fitUnderFullScale (stems); db < 0)
        std::fprintf (stderr, "lowered all stems by %.2f dB so they don't clip\n", -db);

    outDir.createDirectory();
    for (const auto& [stem, audio] : stems)
    {
        const auto file = outDir.getChildFile (juce::String (stemFileName (stem)));
        writeFlac (file, audio);
        std::printf ("wrote %s\n", file.getFullPathName().toRawUTF8());
    }
    return 0;
}

static int importSong (const juce::String& source, const juce::File& outWav)
{
    std::atomic<bool> cancel { false };
    juce::File file = juce::File::getCurrentWorkingDirectory().getChildFile (source);
    DownloadedAudio dl; // deleted (with its folder) when this returns
    if (isYouTubeLink (source))
    {
        dl = downloadYouTubeAudio (source, [] (const juce::String& status, double f) {
            std::fprintf (stderr, "\r  %s %s   ", status.toRawUTF8(), f >= 0 ? (juce::String (juce::roundToInt (f * 100)) + "%").toRawUTF8() : "...");
        }, cancel);
        std::fprintf (stderr, "\n");
        file = dl.file;
    }
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto audio = loadAudioFile (file);
    std::printf ("title: %s\nsource: %s\ndecoded %.1f s of audio in %.0f ms\n", dl.title.toRawUTF8(), file.getFullPathName().toRawUTF8(),
                 audio.size() / (double) sampleRate, juce::Time::getMillisecondCounterHiRes() - t0);

    writeWav (outWav, audio, 16);
    std::printf ("wrote %s\n", outWav.getFullPathName().toRawUTF8());
    return 0;
}

static int analyzeFile (const juce::File& in, const std::string& model)
{
    const auto audio = loadAudioFile (in);
    StemMap stems; // empty: analyse the mix alone
    if (! model.empty())
    {
        Separator sep (model, devCoremlCache());
        std::atomic<bool> cancel { false };
        stems = sep.separate (audio, printProgress, cancel);
        std::fprintf (stderr, "\n");
    }
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto analysis = analyzeSong (audio, stems);
    const auto& [key, tempo, loud, chords] = analysis;
    std::printf ("analysed in %.0f ms\n", juce::Time::getMillisecondCounterHiRes() - t0);
    std::printf ("key: %s (relative %s, Camelot %s), confidence %.2f, %d/3 profiles agree, tuning %+.0f cents\n",
                 key.name().c_str(), key.relative().name().c_str(), key.camelot().c_str(), key.confidence, key.votes, key.tuningCents);
    std::printf ("tempo: %.2f BPM (half %.2f, double %.2f)\n", tempo.bpm, tempo.bpm / 2, tempo.bpm * 2);
    std::printf ("loudness: %.1f LUFS, sample peak %.1f dBFS\n", loud.integratedLufs, loud.samplePeakDb);
    std::printf ("chords:\n%s", chordChart (chords, (double) audio.size() / sampleRate, key).c_str());
    return 0;
}

/** Analyse a song folder written by the plug-in (its stem files), without separating again. */
static int analyzeFolder (const juce::File& folder)
{
    StemMap stems;
    StereoAudio mix;
    for (auto stem : stemOrder)
    {
        auto file = folder.getChildFile (juce::String (stemFileName (stem)));
        if (! file.existsAsFile()) file = folder.getChildFile (juce::String (stemWavName (stem))); // saved before 0.0.3
        auto audio = loadAudioFile (file);
        mix.left.resize (std::max (mix.size(), audio.size()), 0.0f);
        mix.right.resize (mix.left.size(), 0.0f);
        for (std::size_t i = 0; i < audio.size(); ++i) { mix.left[i] += audio.left[i]; mix.right[i] += audio.right[i]; }
        stems[stem] = std::move (audio);
    }
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto a = analyzeSong (mix, stems);
    std::printf ("analysed in %.0f ms\nkey %s, %.2f BPM, %zu beats, first downbeat = beat %d, %zu chord segments\n%s",
                 juce::Time::getMillisecondCounterHiRes() - t0, a.key.name().c_str(), a.tempo.bpm, a.chords.beats.size(),
                 a.chords.firstDownbeat, a.chords.segments.size(), chordChart (a.chords, (double) mix.size() / sampleRate, a.key).c_str());
    return 0;
}

/** MIDI from a song folder's stems, timing each model. */
static int midiFolder (const juce::File& folder, const juce::File& models, const juce::File& outDir, double bpm)
{
    outDir.createDirectory();
    std::atomic<bool> cancel { false };
    for (const auto& m : midiStems)
    {
        const auto audio = loadAudioFile (folder.getChildFile (juce::String (stemFileName (m.stem))));
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const auto t = m.stem == Stem::Piano
                           ? transcribePiano (models.getChildFile (juce::String (std::string (model::pianoFile))).getFullPathName().toStdString(), audio,
                                              [] (double f) { std::fprintf (stderr, "\r  piano %3d%%", juce::roundToInt (f * 100)); }, cancel)
                           : transcribeNotes (models.getChildFile (juce::String (std::string (model::notesFile))).getFullPathName().toStdString(), audio,
                                              m.minHz, m.maxHz, cancel);
        const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        const auto file = outDir.getChildFile (juce::String (stemMidiName (m.stem)));
        writeMidi (file, t, bpm, juce::String (std::string (stemName (m.stem))), m.program);
        std::printf ("\r%-7s %5zu notes, %3zu pedal presses in %5.1f s (%.0fx realtime) -> %s\n", std::string (stemName (m.stem)).c_str(),
                     t.notes.size(), t.pedals.size(), secs, (double) audio.size() / sampleRate / secs, file.getFullPathName().toRawUTF8());
    }
    return 0;
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "usage: see the comment at the top of tools/cli.cpp\n"); return 2; }
    const std::string cmd = argv[1];
    try
    {
        if (cmd == "analyze-selftest") return analysisSelftest();
        if (cmd == "analyze" && argc >= 3) return analyzeFile (fileArg (argv[2]), argc > 3 ? argv[3] : "");
        if (cmd == "chords" && argc >= 3) return analyzeFolder (fileArg (argv[2]));
        if (argc < 3) return 2;
        if (cmd == "selftest") return separationSelftest (argv[2], argc > 3 ? std::atoi (argv[3]) : 12, argc > 4 ? (float) std::atof (argv[4]) : 0.0f);
        if (cmd == "import" && argc >= 4) return importSong (juce::String::fromUTF8 (argv[2]), fileArg (argv[3]));
        if (cmd == "midi-selftest") return midiSelftest (fileArg (argv[2]).getFullPathName().toStdString());
        if (cmd == "midi" && argc >= 5) return midiFolder (fileArg (argv[2]), fileArg (argv[3]), fileArg (argv[4]), argc > 5 ? std::atof (argv[5]) : 120.0);
        if (cmd == "separate" && argc >= 5) return separateFile (argv[2], fileArg (argv[3]), fileArg (argv[4]));
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "\nerror: %s\n", e.what());
        return 1;
    }
    return 2;
}
