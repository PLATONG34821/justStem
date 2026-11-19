// covertools_cli: test the separation engine outside a DAW.
//   covertools_cli selftest <model.onnx> [seconds] [overlap]
//   covertools_cli separate <model.onnx> <input audio> <output folder>
//   covertools_cli import <YouTube link | audio file> <output.wav>
//   covertools_cli analyze-selftest
//   covertools_cli analyze <audio file> [model.onnx]   (with a model: analyse the separated stems)
#include "../src/core/Analysis.h"
#include "../src/core/Resampler.h"
#include "../src/core/Separator.h"
#include "../src/io/AudioFiles.h"
#include "../src/io/YouTube.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <numbers>
#include <cstdio>

using namespace covertools;

/** Developer tools keep their own CoreML cache, so they don't add copies to the plug-in's. */
static std::string devCoremlCache()
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("covertools-coreml-cache").getFullPathName().toStdString();
}

static double siSdr (const std::vector<float>& est, const std::vector<float>& ref)
{
    double dot = 0, rr = 0;
    for (size_t i = 0; i < ref.size(); ++i) { dot += (double) est[i] * ref[i]; rr += (double) ref[i] * ref[i]; }
    const double a = dot / rr;
    double s = 0, e = 0;
    for (size_t i = 0; i < ref.size(); ++i) { const double t = a * ref[i]; s += t * t; e += (est[i] - t) * (est[i] - t); }
    return 10.0 * std::log10 (s / e);
}

static void printProgress (const SeparationProgress& p)
{
    std::fprintf (stderr, "\r  [%s] segment %d/%d, eta %.0fs   ", backendName (p.backend), p.segment, p.totalSegments, p.etaSeconds);
}

static int selftest (const std::string& model, int seconds, float overlap)
{
    // Synthetic track with known parts: drums + bass + chord "keys" (fixed LCG seed, so runs are repeatable).
    const int n = sampleRate * seconds;
    std::vector<float> drums ((size_t) n), bass ((size_t) n), keys ((size_t) n);
    std::uint32_t seed = 1;
    auto rnd = [&] { seed = (seed * 1103515245u + 12345u) & 0x7fffffffu; return (double) seed / 0x7fffffff * 2 - 1; };
    const int chords[4][4] { { 45, 57, 60, 64 }, { 41, 53, 57, 60 }, { 48, 55, 60, 64 }, { 43, 55, 59, 62 } };
    for (int i = 0; i < n; ++i)
    {
        const double t = (double) i / sampleRate, b = std::fmod (t, 0.5), e8 = std::fmod (t, 0.25);
        const int bar = (int) std::floor (t / 0.5) % 4;
        double d = b < 0.25 ? std::sin (2 * std::numbers::pi * (50 + 70 * std::exp (-b * 30)) * b) * std::exp (-b * 12) * 0.9 : 0;
        if ((bar == 1 || bar == 3) && b < 0.15) d += rnd() * std::exp (-b * 25) * 0.5;
        if (e8 < 0.04) d += rnd() * std::exp (-e8 * 120) * 0.15;
        drums[(size_t) i] = (float) d;
        const int* c = chords[(int) std::floor (t / 2) % 4];
        const double f0 = 440 * std::pow (2.0, (c[0] - 69) / 12.0);
        double bs = 0;
        for (int h = 1; h < 8; ++h) bs += std::sin (2 * std::numbers::pi * f0 * h * t) / h;
        bass[(size_t) i] = (float) (bs * 0.25 * (0.6 + 0.4 * std::exp (-b * 4)));
        double k = 0;
        for (int m = 1; m < 4; ++m)
        {
            const double f = 440 * std::pow (2.0, (c[m] - 69) / 12.0);
            for (int h = 1; h < 6; ++h) k += std::sin (2 * std::numbers::pi * f * h * t) * std::exp (-h * 0.7);
        }
        keys[(size_t) i] = (float) (k * 0.07 * std::exp (-std::fmod (t, 1.0) * 3));
    }
    StereoAudio mix;
    mix.left.resize ((size_t) n);
    mix.right.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        mix.left[(size_t) i] = drums[(size_t) i] + bass[(size_t) i] + keys[(size_t) i] * 1.2f;
        mix.right[(size_t) i] = drums[(size_t) i] + bass[(size_t) i] + keys[(size_t) i] * 0.8f;
    }

    Separator sep (model, devCoremlCache());
    if (overlap > 0) sep.overlap = overlap;
    std::atomic<bool> cancel { false };
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto stems = sep.separate (mix, printProgress, cancel);
    const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    std::fprintf (stderr, "\n");

    auto mid = [] (const StereoAudio& s) { std::vector<float> m (s.size()); for (size_t i = 0; i < m.size(); ++i) m[i] = 0.5f * (s.left[i] + s.right[i]); return m; };
    std::vector<float> harm ((size_t) n, 0.0f), sum ((size_t) n, 0.0f);
    for (auto stem : { Stem::Piano, Stem::Other, Stem::Guitar })
    {
        auto m = mid (stems[stem]);
        for (int i = 0; i < n; ++i) harm[(size_t) i] += m[(size_t) i];
    }
    for (auto& [_, s] : stems)
        for (int i = 0; i < n; ++i) sum[(size_t) i] += s.left[(size_t) i];

    std::printf ("demucs on %s: %.1fs for %ds (%.2fx realtime)\n", backendName (sep.backend()), secs, seconds, seconds / secs);
    std::printf ("  SI-SDR drums %.2f dB, bass %.2f dB, keys(piano+other+guitar) %.2f dB, sum-of-stems vs mix %.2f dB\n",
                 siSdr (mid (stems[Stem::Drums]), drums), siSdr (mid (stems[Stem::Bass]), bass), siSdr (harm, keys), siSdr (sum, mix.left));
    return 0;
}

static int separateFile (const std::string& model, const juce::File& in, const juce::File& outDir)
{
    const auto mix = loadAudioFile (in);
    Separator sep (model, devCoremlCache());
    std::atomic<bool> cancel { false };
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    const auto stems = sep.separate (mix, printProgress, cancel);
    const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    const double length = (double) mix.size() / sampleRate;
    std::fprintf (stderr, "\n%.1fs for %.1fs of audio on %s (%.2fx realtime)\n", secs, length, backendName (sep.backend()), length / secs);

    outDir.createDirectory();
    for (const auto& [stem, audio] : stems)
    {
        const auto file = outDir.getChildFile (juce::String (std::string (stemName (stem))) + ".wav");
        writeWav (file, audio);
        std::printf ("wrote %s\n", file.getFullPathName().toRawUTF8());
    }
    return 0;
}

static int importSong (const juce::String& source, const juce::File& outWav)
{
    std::atomic<bool> cancel { false };
    juce::File file (source);
    juce::String title;
    if (isYouTubeLink (source))
    {
        auto dl = downloadYouTubeAudio (source, [] (const juce::String& status, double f) {
            std::fprintf (stderr, "\r  %s %s   ", status.toRawUTF8(), f >= 0 ? (juce::String (juce::roundToInt (f * 100)) + "%").toRawUTF8() : "...");
        }, cancel);
        std::fprintf (stderr, "\n");
        file = dl.file;
        title = dl.title;
    }
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto audio = loadAudioFile (file);
    std::printf ("title: %s\nsource: %s\ndecoded %.1f s of audio in %.0f ms\n", title.toRawUTF8(), file.getFullPathName().toRawUTF8(),
                 audio.size() / (double) sampleRate, juce::Time::getMillisecondCounterHiRes() - t0);

    writeWav (outWav, audio, 16);
    std::printf ("wrote %s\n", outWav.getFullPathName().toRawUTF8());
    return 0;
}

static int analyzeSelftest()
{
    int failures = 0;
    auto check = [&] (bool ok, const std::string& what) { std::printf ("  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str()); failures += ok ? 0 : 1; };

    std::printf ("Loudness\n");
    {
        // ITU-R BS.1770 published 48 kHz coefficients.
        auto c = kWeightingCoefficients (48000);
        const double ref[2][5] { { 1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, 0.73248077421585 },
                                 { 1.0, -2.0, 1.0, -1.99004745483398, 0.99007225036621 } };
        double err = 0;
        for (int s = 0; s < 2; ++s) for (int k = 0; k < 5; ++k) err = std::max (err, std::fabs (c[(size_t) s][(size_t) k] - ref[s][k]));
        check (err < 1e-6, "K-weighting coefficients at 48 kHz match BS.1770 (max error " + juce::String (err, 9).toStdString() + ")");
        for (double level : { -23.0, -33.0 })
        {
            const int n = sampleRate * 20;
            std::vector<float> l ((size_t) n), r;
            const double a = std::pow (10.0, level / 20.0);
            for (int i = 0; i < n; ++i) l[(size_t) i] = (float) (a * std::sin (2 * std::numbers::pi * 1000.0 * i / sampleRate));
            r = l;
            auto res = measureLoudness (l, r, sampleRate);
            check (std::fabs (res.integratedLufs - level) < 0.1, "EBU stereo 1 kHz sine at " + juce::String (level, 0).toStdString() + " dBFS -> "
                       + juce::String (res.integratedLufs, 2).toStdString() + " LUFS (expected " + juce::String (level, 1).toStdString() + ")");
        }
    }

    std::printf ("Resampler (48 kHz -> 44.1 kHz)\n");
    {
        auto sine = [] (double freq, int rate, int n) {
            std::vector<float> x ((size_t) n);
            for (int i = 0; i < n; ++i) x[(size_t) i] = (float) (0.5 * std::sin (2 * std::numbers::pi * freq * i / rate));
            return x;
        };
        auto rms = [] (const std::vector<float>& x, size_t from, size_t to) {
            double e = 0;
            for (size_t i = from; i < to; ++i) e += (double) x[i] * x[i];
            return std::sqrt (e / (double) (to - from));
        };
        const int n = 48000 * 4;
        for (double f : { 100.0, 1000.0, 10000.0, 18000.0 })
        {
            const auto y = resample (sine (f, 48000, n), 48000, sampleRate);
            const auto ideal = sine (f, sampleRate, (int) y.size());
            std::vector<float> err (y.size());
            for (size_t i = 0; i < y.size(); ++i) err[i] = y[i] - ideal[i];
            const size_t a = 4410, b = y.size() - 4410; // skip the edges
            const double snr = 20 * std::log10 (rms (ideal, a, b) / rms (err, a, b));
            check (snr > 70, juce::String (f, 0).toStdString() + " Hz sine matches the exact 44.1 kHz sine: " + juce::String (snr, 1).toStdString() + " dB (timing and gain exact)");
        }
        const auto alias = resample (sine (23000.0, 48000, n), 48000, sampleRate); // above 22.05 kHz: must vanish
        const double rejection = 20 * std::log10 (0.5 / std::sqrt (2.0) / rms (alias, 4410, alias.size() - 4410));
        check (rejection > 70, "23 kHz tone (can't exist at 44.1 kHz) removed by " + juce::String (rejection, 1).toStdString() + " dB, no aliasing");
    }

    std::printf ("Key (I-IV-V-I in major, i-iv-V-i in harmonic minor, all 24 keys)\n");
    {
        int exact = 0, relativeOnly = 0;
        std::string misses;
        auto tone = [] (std::vector<float>& x, int start, int len, int midi, float amp) {
            const double f = 440 * std::pow (2.0, (midi - 69) / 12.0);
            for (int i = 0; i < len; ++i)
            {
                const double t = (double) i / sampleRate;
                double v = 0;
                for (int h = 1; h <= 6; ++h) v += std::sin (2 * std::numbers::pi * f * h * t) * std::pow (0.55, h - 1);
                x[(size_t) (start + i)] += (float) (v * amp * std::exp (-t * 1.5));
            }
        };
        for (int minor = 0; minor < 2; ++minor)
            for (int tonic = 0; tonic < 12; ++tonic)
            {
                // chord roots relative to tonic, and triad intervals
                const int roots[4] { 0, 5, 7, 0 };
                const int len = sampleRate * 2;
                std::vector<float> x ((size_t) len * 4, 0.0f);
                for (int c = 0; c < 4; ++c)
                {
                    const int root = 48 + tonic + roots[c];
                    const bool dominant = roots[c] == 7;
                    const int third = (minor && ! dominant) ? 3 : 4;
                    for (int iv : { 0, third, 7 }) tone (x, c * len, len, root + iv + 12, 0.15f);
                    tone (x, c * len, len, root - 12, 0.2f); // bass
                }
                KeyResult truth;
                truth.tonic = tonic;
                truth.minor = minor;
                auto k = detectKey (x, sampleRate);
                if (k.tonic == truth.tonic && k.minor == truth.minor) ++exact;
                else
                {
                    const auto rel = truth.relative();
                    if (k.tonic == rel.tonic && k.minor == rel.minor) ++relativeOnly;
                    misses += " " + truth.shortName() + "->" + k.shortName();
                }
            }
        check (exact >= 22, "exact key " + std::to_string (exact) + "/24, relative-key confusions " + std::to_string (relativeOnly)
                                + (misses.empty() ? "" : ", misses:" + misses));
    }

    std::printf ("Tempo (drum loop: kick on 1 and 3, snare on 2 and 4, eighth-note hats)\n");
    {
        std::uint32_t seed = 7;
        auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (double) (seed >> 8) / (1 << 24) * 2 - 1; };
        for (double bpm : { 72.0, 90.0, 100.0, 115.0, 120.0, 128.0, 140.0, 160.0, 174.0 })
        {
            const int n = sampleRate * 30;
            std::vector<float> x ((size_t) n, 0.0f);
            const double beat = 60.0 / bpm;
            for (int b = 0; b * beat < 30.0; ++b)
                for (int half = 0; half < 2; ++half)
                {
                    const int s0 = (int) ((b + 0.5 * half) * beat * sampleRate);
                    for (int i = 0; i < sampleRate / 4 && s0 + i < n; ++i)
                    {
                        const double t = (double) i / sampleRate;
                        double v = 0;
                        if (half == 0 && b % 2 == 0) v += std::sin (2 * std::numbers::pi * (50 + 80 * std::exp (-t * 30)) * t) * std::exp (-t * 15) * 0.9; // kick
                        if (half == 0 && b % 2 == 1) v += rnd() * std::exp (-t * 25) * 0.5;                                                // snare
                        v += rnd() * std::exp (-t * 120) * 0.12;                                                                            // hat
                        x[(size_t) (s0 + i)] += (float) v;
                    }
                }
            auto r = detectTempo (x, sampleRate);
            // Tempo octave is ambiguous (the UI always shows half / double); the period must be precise.
            double best = r.bpm, err = 1e9;
            for (double f : { 0.5, 1.0, 2.0 }) if (std::fabs (r.bpm * f - bpm) < err) { err = std::fabs (r.bpm * f - bpm); best = r.bpm * f; }
            const bool sameOctave = std::fabs (r.bpm - bpm) < 0.5;
            check (err < 0.2, juce::String (bpm, 0).toStdString() + " BPM -> " + juce::String (r.bpm, 2).toStdString()
                                  + (sameOctave ? "" : " (octave: " + juce::String (best, 2).toStdString() + " via half/double)"));
        }
    }
    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : (std::to_string (failures) + " FAILED").c_str());
    return failures == 0 ? 0 : 1;
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
    const auto [key, tempo, loud] = analyzeSong (audio, stems);
    std::printf ("analysed in %.0f ms\n", juce::Time::getMillisecondCounterHiRes() - t0);
    std::printf ("key: %s (relative %s, Camelot %s), confidence %.2f, %d/3 profiles agree, tuning %+.0f cents\n",
                 key.name().c_str(), key.relative().name().c_str(), key.camelot().c_str(), key.confidence, key.votes, key.tuningCents);
    std::printf ("tempo: %.2f BPM (half %.2f, double %.2f)\n", tempo.bpm, tempo.bpm / 2, tempo.bpm * 2);
    std::printf ("loudness: %.1f LUFS, sample peak %.1f dBFS\n", loud.integratedLufs, loud.samplePeakDb);
    return 0;
}

int main (int argc, char** argv)
{
    if (argc < 2) { std::fprintf (stderr, "usage: see the comment at the top of tools/cli.cpp\n"); return 2; }
    const std::string cmd = argv[1];
    try
    {
        if (cmd == "analyze-selftest") return analyzeSelftest();
        if (cmd == "analyze" && argc >= 3) return analyzeFile (juce::File (juce::String::fromUTF8 (argv[2])), argc > 3 ? argv[3] : "");
        if (argc < 3) return 2;
        if (cmd == "selftest") return selftest (argv[2], argc > 3 ? std::atoi (argv[3]) : 12, argc > 4 ? (float) std::atof (argv[4]) : 0.0f);
        if (cmd == "import" && argc >= 4) return importSong (juce::String::fromUTF8 (argv[2]), juce::File (juce::String::fromUTF8 (argv[3])));
        if (cmd == "separate" && argc >= 5) return separateFile (argv[2], juce::File (argv[3]), juce::File (argv[4]));
    }
    catch (const std::exception& e)
    {
        std::fprintf (stderr, "\nerror: %s\n", e.what());
        return 1;
    }
    return 2;
}
