// juststem_cli's self-tests: `selftest` (separation), `midi-selftest` (MIDI) and `analyze-selftest`
// (everything else in core/), on synthetic signals with known answers. See tools/cli.cpp for the commands.
#include "cli.h"
#include "../src/core/Analysis.h"
#include "../src/core/Dsp.h"
#include "../src/core/PitchShift.h"
#include "../src/core/Resampler.h"
#include "../src/core/Separator.h"
#include "../src/core/Transcriber.h"
#include "../src/io/AudioFiles.h"
#include "../src/io/M4aDecoder.h"
#include "../src/io/ToolInstaller.h"
#include "../src/io/UpdateCheck.h"
#include "../src/io/YouTube.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <numbers>
#include <string>

using namespace juststem;

static double siSdr (const std::vector<float>& est, const std::vector<float>& ref)
{
    double dot = 0, rr = 0;
    for (size_t i = 0; i < ref.size(); ++i) { dot += (double) est[i] * ref[i]; rr += (double) ref[i] * ref[i]; }
    const double a = dot / rr;
    double s = 0, e = 0;
    for (size_t i = 0; i < ref.size(); ++i) { const double t = a * ref[i]; s += t * t; e += (est[i] - t) * (est[i] - t); }
    return 10.0 * std::log10 (s / e);
}

/** RMS of x[from, to). */
static double rms (const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double e = 0;
    for (std::size_t i = from; i < to; ++i) e += (double) x[i] * x[i];
    return std::sqrt (e / (double) (to - from));
}

/** Repeatable noise in -1..1 for the synthetic test signals. */
struct Noise
{
    std::uint32_t seed;
    double operator()() { seed = seed * 1664525u + 1013904223u; return (double) (seed >> 8) / (1 << 24) * 2 - 1; }
};

int separationSelftest (const std::string& model, int seconds, float overlap)
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

    std::printf ("separation on %s: %.1fs for %ds (%.2fx realtime)\n", backendName (sep.backend()), secs, seconds, seconds / secs);
    std::printf ("  SI-SDR drums %.2f dB, bass %.2f dB, keys(piano+other+guitar) %.2f dB, sum-of-stems vs mix %.2f dB\n",
                 siSdr (mid (stems[Stem::Drums]), drums), siSdr (mid (stems[Stem::Bass]), bass), siSdr (harm, keys), siSdr (sum, mix.left));
    return 0;
}

int analysisSelftest()
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

    std::printf ("Tuning on a click track (flat spectra)\n");
    {
        std::vector<float> clicks ((size_t) sampleRate * 2, 0.0f);
        for (std::size_t i = 0; i < clicks.size(); i += (std::size_t) sampleRate / 2) clicks[i] = 0.9f;
        const auto key = detectKey (clicks, sampleRate);
        check (std::isfinite (key.tuningCents) && std::fabs (key.tuningCents) <= 50.0f,
               "tuning is a number: " + juce::String (key.tuningCents, 1).toStdString() + " cents");
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
        Noise rnd { 7 };
        const std::vector<double> tempos { 72.0, 90.0, 100.0, 115.0, 120.0, 128.0, 140.0, 160.0, 174.0 };
        int rightOctave = 0;
        for (double bpm : tempos)
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
            rightOctave += sameOctave ? 1 : 0;
            check (err < 0.2, juce::String (bpm, 0).toStdString() + " BPM -> " + juce::String (r.bpm, 2).toStdString()
                                  + (sameOctave ? "" : " (octave: " + juce::String (best, 2).toStdString() + " via half/double)"));
        }
        // The octave guess can't be right for every tempo (72 reads as 144, 174 as 87: both are
        // legitimate feels), so guard against regressions instead: at least as many right as today.
        constexpr int minRightOctave = 7;
        check (rightOctave >= minRightOctave, "octave right for " + std::to_string (rightOctave) + " of "
                                                  + std::to_string (tempos.size()) + " tempos (need " + std::to_string (minRightOctave) + ")");
    }
    std::printf ("Beats and chords (synthetic band: drums, bass, re-struck chords; 7ths, sus, dim, slash chords)\n");
    {
        // A 16-bar progression in C major, two chords per bar where marked. {root, quality, bass, beats}
        struct Truth { int root; ChordQuality q; int bass; int beats; };
        using Q = ChordQuality;
        const std::vector<Truth> song {
            { 0, Q::Major, -1, 4 }, { 9, Q::Minor7, -1, 4 }, { 5, Q::Major7, -1, 4 }, { 7, Q::Dominant7, -1, 4 },
            { 0, Q::Major, 4, 4 }, { 2, Q::Minor, -1, 4 }, { 7, Q::Sus4, -1, 2 }, { 7, Q::Major, -1, 2 }, { 0, Q::Major, -1, 4 },
            { 5, Q::Major, -1, 4 }, { 11, Q::Diminished, -1, 4 }, { 4, Q::Minor, -1, 4 }, { 9, Q::Minor, -1, 4 },
            { 2, Q::Sus2, -1, 4 }, { 2, Q::Minor7, -1, 2 }, { 7, Q::Dominant7, -1, 2 }, { 0, Q::Major, -1, 4 }, { 0, Q::Major, -1, 4 },
            { 9, Q::Minor, -1, 4 }, { 5, Q::Major, 9, 4 },
        };
        static constexpr int iv[8][4] { { 0, 4, 7, -1 }, { 0, 3, 7, -1 }, { 0, 4, 7, 10 }, { 0, 4, 7, 11 },
                                        { 0, 3, 7, 10 }, { 0, 2, 7, -1 }, { 0, 5, 7, -1 }, { 0, 3, 6, -1 } };
        auto note = [] (std::vector<float>& x, double t0, double len, int midi, float amp, int partials, double decay) {
            const double f = 440 * std::pow (2.0, (midi - 69) / 12.0);
            const auto s0 = (std::size_t) (t0 * sampleRate);
            for (std::size_t i = 0; i < (std::size_t) (len * sampleRate) && s0 + i < x.size(); ++i)
            {
                const double t = (double) i / sampleRate;
                double v = 0;
                for (int h = 1; h <= partials; ++h) v += std::sin (2 * std::numbers::pi * f * h * t) * std::pow (0.5, h - 1);
                x[s0 + i] += (float) (v * amp * std::exp (-t * decay) * std::min (1.0, t * 200));
            }
        };
        int totalBeats = 0;
        for (auto& c : song) totalBeats += c.beats;
        for (int transpose : { 0, 3, 6, 9 })
            for (double bpm : { 96.0, 124.0 })
            {
                const double beat = 60.0 / bpm, offset = 0.61; // music starts after a short silence, a pickup beat first
                const double length = offset + (totalBeats + 1) * beat + 1.0;
                std::vector<float> harmonic ((std::size_t) (length * sampleRate), 0.0f), bass = harmonic, drums = harmonic;
                Noise rnd { 11 };
                for (int b = 0; b <= totalBeats; ++b) // drums: kick on 1 and 3, snare on 2 and 4 (beat 0 is the pickup)
                {
                    const auto s0 = (std::size_t) ((offset + b * beat) * sampleRate);
                    for (std::size_t i = 0; i < (std::size_t) sampleRate / 5 && s0 + i < drums.size(); ++i)
                    {
                        const double t = (double) i / sampleRate;
                        drums[s0 + i] += (float) ((b % 2 == 1) ? std::sin (2 * std::numbers::pi * (50 + 80 * std::exp (-t * 30)) * t) * std::exp (-t * 15) * 0.9
                                                               : rnd() * std::exp (-t * 25) * 0.5);
                    }
                }
                double t = offset + beat; // bar 1 starts after the pickup beat
                ChordResult truth;
                KeyResult key;
                key.tonic = transpose;
                for (auto& c : song)
                {
                    const int root = c.root + transpose;
                    for (int k = 0; k < c.beats; ++k)
                    {
                        for (int i : iv[(int) c.q]) if (i >= 0) note (harmonic, t + k * beat, beat, 60 + (root + i) % 12, 0.12f, 5, 2.5);
                        if (k % 2 == 0) note (bass, t + k * beat, 2 * beat, 36 + ((c.bass >= 0 ? c.bass : c.root) + transpose) % 12, 0.35f, 3, 1.0);
                    }
                    truth.segments.push_back ({ t, t + c.beats * beat, { root % 12, c.q, c.bass >= 0 ? (c.bass + transpose) % 12 : -1 } });
                    t += c.beats * beat;
                }
                auto tempo = detectTempo (drums, sampleRate).bpm;
                if (std::fabs (tempo / 2 - bpm) < 2) tempo /= 2;
                if (std::fabs (tempo * 2 - bpm) < 2) tempo *= 2;
                const auto r = detectChords (harmonic, bass, drums, sampleRate, tempo, key);

                // Beats: every true beat has a detected beat within 35 ms.
                double worst = 0, bias = 0;
                for (int b = 0; b <= totalBeats; ++b)
                {
                    double best = 1e9;
                    for (double d : r.beats) if (std::fabs (d - (offset + b * beat)) < std::fabs (best)) best = d - (offset + b * beat);
                    worst = std::max (worst, std::fabs (best));
                    bias += best / (totalBeats + 1);
                }
                // Chords, sampled every 10 ms where the truth has a chord.
                int same = 0, sameTriad = 0, samples = 0;
                for (const auto& seg : truth.segments)
                    for (double x = seg.start + 0.005; x < seg.end; x += 0.01, ++samples)
                    {
                        const int i = r.segmentAt (x);
                        if (i < 0) continue;
                        const auto& c = r.segments[(size_t) i].chord;
                        same += c == seg.chord;
                        auto minorish = [] (ChordQuality q) { return q == ChordQuality::Minor || q == ChordQuality::Minor7 || q == ChordQuality::Diminished; };
                        sameTriad += c.root == seg.chord.root && minorish (c.quality) == minorish (seg.chord.quality);
                    }
                // The bar line: the first downbeat is beat 1 of bar 1 (after the pickup).
                const double firstBar = offset + beat;
                bool barOk = (size_t) r.firstDownbeat < r.beats.size();
                if (barOk)
                {
                    const double bars = (r.beats[(size_t) r.firstDownbeat] - firstBar) / (4 * beat);
                    barOk = std::fabs (bars - std::round (bars)) * 4 * beat < 0.05;
                }
                const auto label = key.name() + " at " + std::to_string ((int) bpm) + " BPM: ";
                check (worst < 0.035, label + "beats within " + std::to_string ((int) std::round (worst * 1000)) + " ms (average "
                                            + std::to_string ((int) std::round (bias * 1000)) + " ms)");
                check (barOk, label + "bar lines found");
                check (same >= 0.85 * samples, label + "exact chords " + std::to_string (100 * same / samples) + "%, root+major/minor "
                                                   + std::to_string (100 * sameTriad / samples) + "%");
                if (transpose == 0 && bpm == 96.0) std::printf ("%s", chordChart (r, length, key).c_str());

                if (transpose == 0)
                {
                    // The same band without drums, through analyzeSong as the plug-in runs it: the tempo
                    // and the beats come from the chords and the bass instead of a silent drums stem.
                    StemMap stems;
                    stems[Stem::Piano] = { harmonic, harmonic };
                    stems[Stem::Bass] = { bass, bass };
                    stems[Stem::Drums] = { std::vector<float> (harmonic.size(), 0.0f), std::vector<float> (harmonic.size(), 0.0f) };
                    StereoAudio mixNoDrums { harmonic, harmonic };
                    for (std::size_t i = 0; i < bass.size(); ++i) { mixNoDrums.left[i] += bass[i]; mixNoDrums.right[i] += bass[i]; }
                    const auto a = analyzeSong (mixNoDrums, stems);
                    double err = 1e9;
                    for (double f : { 0.5, 1.0, 2.0 }) err = std::min (err, std::fabs (a.tempo.bpm * f - bpm));
                    check (err < 0.5 && a.chords.beats.size() > (size_t) totalBeats / 2,
                           label + "no drums: tempo " + juce::String (a.tempo.bpm, 2).toStdString() + " BPM from the other instruments");
                }
            }
        KeyResult eb;
        eb.tonic = 3;
        const Chord bbOverD { 10, ChordQuality::Major, 2 };
        check (chordName (bbOverD, eb) == "Bb/D" && chordName (bbOverD, eb, 1) == "B/D#" && chordName ({ 6, ChordQuality::Minor7, -1 }, eb) == "Gbm7"
                   && chordName ({}, eb) == "N.C.",
               "chord spelling: Bb/D in Eb, B/D# up a semitone, N.C.");
    }

    std::printf ("Pitch shift (Transpose)\n");
    {
        // A 440 Hz tone that starts at 1.0 s, in 60 s of audio.
        const int n = sampleRate * 60;
        StereoAudio x;
        x.left.assign ((size_t) n, 0.0f);
        for (int i = sampleRate; i < n; ++i) x.left[(size_t) i] = 0.3f * (float) std::sin (2 * std::numbers::pi * 440.0 * i / sampleRate);
        x.right = x.left;
        auto freqAt = [] (const std::vector<float>& v, int from) { // FFT peak with parabolic interpolation
            constexpr int fftN = 32768;
            std::vector<float> w ((size_t) fftN);
            for (int i = 0; i < fftN; ++i) w[(size_t) i] = v[(size_t) (from + i)] * (float) (0.5 - 0.5 * std::cos (2 * std::numbers::pi * i / fftN));
            const auto mag = dsp::spectrumMagnitudes (w);
            const int k = (int) (std::max_element (mag.begin() + 1, mag.end() - 1) - mag.begin());
            const double a = std::log (mag[(size_t) k - 1]), b = std::log (mag[(size_t) k]), c = std::log (mag[(size_t) k + 1]);
            return (k + 0.5 * (a - c) / (a - 2 * b + c)) * sampleRate / fftN;
        };
        for (int semis : { 2, -3, 5 })
        {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto y = pitchShift (x, (float) semis, sampleRate);
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            const double want = 440.0 * std::pow (2.0, semis / 12.0), got = freqAt (y.left, sampleRate * 20);
            const double cents = 1200 * std::log2 (got / want);
            // Timing: where the output first reaches half its level, versus 1.0 s.
            std::size_t onset = 0;
            while (onset < y.size() && std::fabs (y.left[onset]) < 0.15f) ++onset;
            const double offsetMs = ((double) onset / sampleRate - 1.0) * 1000;
            const double gainDb = 20 * std::log10 (rms (y.left, (size_t) sampleRate * 10, (size_t) sampleRate * 50) / rms (x.left, (size_t) sampleRate * 10, (size_t) sampleRate * 50));
            check (std::fabs (cents) < 5 && std::fabs (offsetMs) < 15 && std::fabs (gainDb) < 1.5,
                   (semis > 0 ? "+" : "") + std::to_string (semis) + " semitones: " + juce::String (got, 1).toStdString() + " Hz (want "
                       + juce::String (want, 1).toStdString() + ", " + juce::String (cents, 1).toStdString() + " cents), starts "
                       + juce::String (offsetMs, 1).toStdString() + " ms off, level " + juce::String (gainDb, 2).toStdString() + " dB, "
                       + std::to_string ((int) (60000.0 / ms)) + "x realtime");
        }
    }

    std::printf ("Headroom before saving\n");
    {
        // A sine per stem; vocals and drums in phase make the full mix peak at 1.2.
        auto sine = [] (float amp) {
            StereoAudio a;
            for (int i = 0; i < sampleRate / 10; ++i) a.left.push_back (amp * (float) std::sin (2 * std::numbers::pi * 441.0 * i / sampleRate));
            a.right = a.left;
            return a;
        };
        auto peakOf = [] (const std::vector<float>& x) { float p = 0; for (float v : x) p = std::max (p, std::abs (v)); return p; };
        const float ceiling = std::pow (10.0f, -0.1f / 20.0f);

        StemMap loud { { Stem::Vocals, sine (0.5f) }, { Stem::Drums, sine (0.7f) }, { Stem::Bass, sine (0.0f) } };
        const float db = fitUnderFullScale (loud);
        std::vector<float> mix (loud.at (Stem::Vocals).left.size());
        for (const auto& [stem, a] : loud) for (std::size_t i = 0; i < mix.size(); ++i) mix[i] += a.left[i];
        const float ratio = peakOf (loud.at (Stem::Drums).left) / peakOf (loud.at (Stem::Vocals).left);
        check (peakOf (mix) <= ceiling * 1.0001f && peakOf (mix) > ceiling * 0.999f && std::fabs (ratio - 1.4f) < 1e-4f
                   && std::fabs (db - 20.0f * std::log10 (ceiling / 1.2f)) < 0.01f,
               "a full mix over full scale: every stem lowered by " + juce::String (-db, 2).toStdString() + " dB, balance kept");

        // Vocals against the drums: the full mix is quiet, but the instrumental (the drums) peaks at 1.1.
        auto against = sine (-1.1f);
        StemMap cancel { { Stem::Vocals, against }, { Stem::Drums, sine (1.1f) } };
        fitUnderFullScale (cancel);
        check (peakOf (cancel.at (Stem::Drums).left) <= ceiling * 1.0001f, "an instrumental over full scale is caught too");

        StemMap quiet { { Stem::Vocals, sine (0.4f) }, { Stem::Drums, sine (0.4f) } };
        const auto before = quiet.at (Stem::Vocals).left;
        check (fitUnderFullScale (quiet) == 0.0f && quiet.at (Stem::Vocals).left == before, "stems that fit are left alone");
    }

    std::printf ("Gapless MP3 with cover art over 64 KB\n");
    {
        // Minimal 48 kHz 128 kbps stereo MPEG-1 Layer III frames (384 bytes each).
        auto makeFrame = [] {
            std::string f (384, '\0');
            f[0] = '\xff'; f[1] = '\xfb'; f[2] = '\x94'; f[3] = '\x00';
            return f;
        };
        std::string frames;
        for (int i = 0; i < 11; ++i) frames += makeFrame();

        auto infoFrame = makeFrame();
        // After 4-byte header + 32-byte stereo side info: "Info" + flags (frame count) + 11 frames.
        infoFrame.replace (36, 4, "Info");
        infoFrame[43] = 0x01;
        infoFrame[47] = 11;
        // LAME tag starts at byte 48: 9-byte encoder + 12 bytes + 3-byte (delay=576, padding=1728).
        infoFrame.replace (48, 9, "LAME3.100");
        infoFrame[69] = 0x24; infoFrame[70] = 0x06; infoFrame[71] = '\xc0';

        // 200,000-byte ID3v2.3 tag (synchsafe size = 0x00030D40 -> 0x00, 0x0c, 0x1a, 0x40).
        constexpr std::uint32_t id3Payload = 200000;
        std::string id3 ("ID3\x03\x00\x00", 6);
        for (int shift = 21; shift >= 0; shift -= 7) id3.push_back ((char) ((id3Payload >> shift) & 0x7f));
        id3.append (id3Payload, '\0');

        const auto plainFile = juce::File::createTempFile (".mp3");
        const auto taggedFile = juce::File::createTempFile (".mp3");
        const auto artFile = juce::File::createTempFile (".mp3");
        plainFile.replaceWithData (frames.data(), frames.size());
        const auto taggedBytes = infoFrame + frames;
        taggedFile.replaceWithData (taggedBytes.data(), taggedBytes.size());
        const auto artBytes = id3 + taggedBytes;
        artFile.replaceWithData (artBytes.data(), artBytes.size());

        const auto plainLen = loadAudioFile (plainFile).size();
        const auto taggedLen = loadAudioFile (taggedFile).size();
        const auto artLen = loadAudioFile (artFile).size();
        plainFile.deleteFile();
        taggedFile.deleteFile();
        artFile.deleteFile();
        check (taggedLen < plainLen && artLen == taggedLen,
               "encoder delay and padding are read past a 200 KB ID3v2 tag (" + std::to_string (artLen)
                   + " samples == " + std::to_string (taggedLen) + ", plain " + std::to_string (plainLen) + ")");
    }

    std::printf ("Damaged M4A files\n");
    {
        // Minimal MP4 boxes: big-endian sizes, then the type and payload.
        const auto u32 = [] (std::uint32_t v) { return std::string { (char) (v >> 24), (char) (v >> 16), (char) (v >> 8), (char) v }; };
        const auto box = [&] (const char* type, const std::string& payload) { return u32 ((std::uint32_t) payload.size() + 8) + type + payload; };
        const auto zeros = [] (std::size_t n) { return std::string (n, '\0'); };
        const auto esds = box ("esds", zeros (4) + "\x03\x19" + zeros (3) + "\x04\x11\x40" + zeros (12) + "\x05\x02\x12\x10");
        const auto stsd = box ("stsd", zeros (4) + u32 (1) + box ("mp4a", zeros (28) + esds));
        const auto trak = [&] (const std::string& sampleTable) {
            return box ("trak", box ("tkhd", zeros (12) + u32 (1) + zeros (8))
                                    + box ("mdia", box ("mdhd", zeros (12) + u32 (44100) + zeros (4))
                                                       + box ("hdlr", zeros (8) + "soun" + zeros (13))
                                                       + box ("minf", box ("stbl", stsd + sampleTable))));
        };
        const auto ftyp = box ("ftyp", "M4A " + zeros (4) + "M4A ");
        const auto many = 0xffffffffu; // frames claimed: about 4 billion, of 1 byte each
        const std::pair<const char*, std::string> files[] {
            { "classic (stsz)", ftyp + box ("moov", trak (box ("stsz", zeros (4) + u32 (1) + u32 (many)) + box ("stco", zeros (4) + u32 (1) + u32 (0))
                                                          + box ("stsc", zeros (4) + u32 (1) + u32 (1) + u32 (many) + u32 (1)))) },
            { "fragmented (trun)", ftyp + box ("moov", trak ({}) + box ("mvex", box ("trex", zeros (4) + u32 (1) + zeros (8) + u32 (1) + zeros (4))))
                                       + box ("moof", box ("traf", box ("tfhd", zeros (4) + u32 (1)) + box ("trun", zeros (4) + u32 (many)))) },
            { "after 2 MB free box", ftyp + box ("free", zeros (2 << 20))
                                          + box ("moov", trak (box ("stsz", zeros (4) + u32 (1) + u32 (many)) + box ("stco", zeros (4) + u32 (1) + u32 (0))
                                                               + box ("stsc", zeros (4) + u32 (1) + u32 (1) + u32 (many) + u32 (1)))) },
        };
        const auto file = juce::File::createTempFile (".m4a");
        for (const auto& [name, bytes] : files)
        {
            file.replaceWithData (bytes.data(), bytes.size());
            std::string error;
            try { decodeM4a (file, maxSongSeconds); }
            catch (const std::exception& e) { error = e.what(); }
            check (error == songTooLong().what(), std::string (name) + ", claiming 4 billion frames, is refused before they're read: \"" + error + "\"");
        }
        file.deleteFile();
    }

    std::printf ("Tool checksum parsing\n");
    {
        const juce::String hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        check (findSha256InSums ("deadbeef  other.exe\n" + hash + "  yt-dlp_macos\n", "yt-dlp_macos") == hash,
               "multi-line SHA2-256SUMS extracts matching 64-hex digest");
        check (findSha256InSums (hash + "  deno.zip\n", "deno.zip") == hash
                   && findSha256InSums (hash + "\tdeno.zip\n", "deno.zip") == hash
                   && findSha256InSums (hash + "\n", "deno.zip") == hash,
               "single-line sha256sum (spaces, tab, or bare digest) extracts 64-hex digest");
        check (findSha256InSums (hash + "  wrong.zip\n", "deno.zip").isEmpty(),
               "single-line sha256sum for another filename is rejected");
        const juce::String pwsh = "\r\nAlgorithm : SHA256\r\nHash      : " + hash.toUpperCase()
                                  + "\r\nPath      : C:\\a\\deno\\target\\release\\deno.zip\r\n\r\n";
        check (findSha256InSums (pwsh, "deno.zip") == hash.toUpperCase() && findSha256InSums (pwsh, "other.zip").isEmpty(),
               "PowerShell Get-FileHash list (Deno on Windows) extracts the digest for its own file only");
        check (findSha256InSums ("503 Service Unavailable\n", "deno.zip").isEmpty(),
               "single-line HTTP error page is rejected");
    }

    std::printf ("Update check and YouTube URL helpers\n");
    {
        check (UpdateCheck::isNewer ("v0.0.6", "0.0.5") && UpdateCheck::isNewer ("0.1.0", "0.0.5")
                   && ! UpdateCheck::isNewer ("v0.0.5", "0.0.5") && ! UpdateCheck::isNewer ("0.0.4", "0.0.5")
                   && ! UpdateCheck::isNewer ("v0.0.5-test", "0.0.5") && ! UpdateCheck::isNewer ("invalid", "0.0.5"),
               "UpdateCheck::isNewer compares semantic versions");
        check (isYouTubeLink ("https://www.youtube.com/watch?v=abc") && isYouTubeLink ("youtu.be/abc")
                   && isYouTubeLink ("music.youtube.com/watch?v=abc") && isYouTubeLink ("https://youtube.com/@artist")
                   && ! isYouTubeLink ("https://notyoutube.com/watch?v=abc") && ! isYouTubeLink ("/tmp/song.mp3")
                   && ! isYouTubeLink ("https://evilwww.youtu.be/abc")
                   && ! isYouTubeLink ("https://www.youtube.com:x@evil.com/watch?v=abc")
                   && ! isYouTubeLink ("ftp://www.youtube.com/watch?v=abc")
                   && ! isYouTubeLink ("httpfoo://youtube.com/watch?v=abc")
                   && ! isYouTubeLink ("https://evil.com\\.youtube.com/watch?v=abc")
                   && ! isYouTubeLink ("https://evil.com%2f.youtube.com/watch?v=abc"),
               "isYouTubeLink matches YouTube hosts only and rejects URL spoofing");
        // Multi-byte UTF-8 ("เพลงไทย") split at every 2-byte boundary, plus a trailing line with no newline.
        const std::string raw = "juststem-title \xe0\xb9\x80\xe0\xb8\x9e\xe0\xb8\xa5\xe0\xb8\x87\xe0\xb9\x84\xe0\xb8\x97\xe0\xb8\xa2\nERROR: trailing";
        std::string pending;
        juce::StringArray lines;
        for (std::size_t i = 0; i < raw.size(); i += 2)
            readUtf8Lines (pending, raw.data() + i, (int) std::min<std::size_t> (2, raw.size() - i), false,
                           [&] (const juce::String& l) { lines.add (l); });
        readUtf8Lines (pending, nullptr, 0, true, [&] (const juce::String& l) { lines.add (l); });
        check (lines.size() == 2
                   && lines[0] == juce::String::fromUTF8 ("juststem-title \xe0\xb9\x80\xe0\xb8\x9e\xe0\xb8\xa5\xe0\xb8\x87\xe0\xb9\x84\xe0\xb8\x97\xe0\xb8\xa2")
                   && lines[1] == "ERROR: trailing",
               "split UTF-8 pipe chunks and trailing line without newline decode intact");
    }

    std::printf ("Spectrogram model path (Stft + ONNX)\n");
    {
        // Minimal opset-17 ONNX model matching model::hq's spectrogram I/O: splits both
        // spec_real and spec_imag equally across 6 stems (sum of stems == mix).
        auto varint = [] (std::string& out, std::uint64_t v) {
            for (; v >= 0x80; v >>= 7) out.push_back ((char) ((v & 0x7f) | 0x80));
            out.push_back ((char) v);
        };
        auto fieldBytes = [&] (std::string& out, int id, const std::string& data) {
            varint (out, (std::uint64_t) ((id << 3) | 2));
            varint (out, data.size());
            out += data;
        };
        auto fieldInt = [&] (std::string& out, int id, std::int64_t v) {
            varint (out, (std::uint64_t) ((id << 3) | 0));
            varint (out, (std::uint64_t) v);
        };
        auto valueInfo = [&] (const std::string& name, std::initializer_list<std::int64_t> dims) {
            std::string shape;
            for (auto d : dims)
            {
                std::string dim;
                fieldInt (dim, 1, d);
                fieldBytes (shape, 1, dim);
            }
            std::string tensor, type, vi;
            fieldInt (tensor, 1, 1); // FLOAT
            fieldBytes (tensor, 2, shape);
            fieldBytes (type, 1, tensor);
            fieldBytes (vi, 1, name);
            fieldBytes (vi, 2, type);
            return vi;
        };
        auto tensorInit = [&] (const std::string& name, int dtype, std::initializer_list<std::int64_t> dims, const std::string& raw) {
            std::string t;
            for (auto d : dims) fieldInt (t, 1, d);
            fieldInt (t, 2, dtype);
            fieldBytes (t, 8, name);
            fieldBytes (t, 9, raw);
            return t;
        };
        auto node = [&] (std::initializer_list<std::string> ins, std::initializer_list<std::string> outs,
                         const std::string& name, const std::string& op, int axisAttr = -1) {
            std::string n;
            for (const auto& s : ins) fieldBytes (n, 1, s);
            for (const auto& s : outs) fieldBytes (n, 2, s);
            fieldBytes (n, 3, name);
            fieldBytes (n, 4, op);
            if (axisAttr >= 0)
            {
                std::string attr;
                fieldBytes (attr, 1, "axis");
                fieldInt (attr, 3, axisAttr);
                fieldInt (attr, 20, 2); // AttributeType::INT
                fieldBytes (n, 5, attr);
            }
            return n;
        };
        const float oneSixth = 1.0f / 6.0f;
        std::string scaleRaw (sizeof (float), '\0');
        std::memcpy (scaleRaw.data(), &oneSixth, sizeof (float));
        std::string shapeRaw;
        for (std::int64_t d : { 1, 1, 2, 1025, 345 })
            for (int b = 0; b < 8; ++b) shapeRaw.push_back ((char) ((d >> (8 * b)) & 0xff));

        std::string graph;
        fieldBytes (graph, 1, node ({ "spec_real", "scale" }, { "r_scaled" }, "mul_r", "Mul"));
        fieldBytes (graph, 1, node ({ "r_scaled", "shape5d" }, { "r_5d" }, "res_r", "Reshape"));
        fieldBytes (graph, 1, node ({ "r_5d", "r_5d", "r_5d", "r_5d", "r_5d", "r_5d" }, { "out_spec_real" }, "cat_r", "Concat", 1));
        fieldBytes (graph, 1, node ({ "spec_imag", "scale" }, { "i_scaled" }, "mul_i", "Mul"));
        fieldBytes (graph, 1, node ({ "i_scaled", "shape5d" }, { "i_5d" }, "res_i", "Reshape"));
        fieldBytes (graph, 1, node ({ "i_5d", "i_5d", "i_5d", "i_5d", "i_5d", "i_5d" }, { "out_spec_imag" }, "cat_i", "Concat", 1));
        fieldBytes (graph, 2, "hq_test");
        fieldBytes (graph, 5, tensorInit ("scale", 1, {}, scaleRaw));
        fieldBytes (graph, 5, tensorInit ("shape5d", 7, { 5 }, shapeRaw));
        fieldBytes (graph, 11, valueInfo ("spec_real", { 1, 2, 1025, 345 }));
        fieldBytes (graph, 11, valueInfo ("spec_imag", { 1, 2, 1025, 345 }));
        fieldBytes (graph, 12, valueInfo ("out_spec_real", { 1, 6, 2, 1025, 345 }));
        fieldBytes (graph, 12, valueInfo ("out_spec_imag", { 1, 6, 2, 1025, 345 }));

        std::string opset, onnxModel;
        fieldInt (opset, 2, 17);
        fieldInt (onnxModel, 1, 8);
        fieldBytes (onnxModel, 7, graph);
        fieldBytes (onnxModel, 8, opset);

        const auto tmpDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("juststem-hq-selftest-" + juce::Uuid().toString());
        tmpDir.createDirectory();
        const auto hqFile = tmpDir.getChildFile (juce::String (std::string (model::hq.fileName)));
        hqFile.replaceWithData (onnxModel.data(), onnxModel.size());

        const int n = sampleRate * 6; // > 176,400 samples so crossfaded chunks run
        StereoAudio mix;
        mix.left.resize ((size_t) n);
        mix.right.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / sampleRate;
            mix.left[(size_t) i] = (float) (0.3 * std::sin (2 * std::numbers::pi * 220.0 * t));
            mix.right[(size_t) i] = (float) (0.3 * std::sin (2 * std::numbers::pi * 330.0 * t));
        }

        Separator sep (hqFile.getFullPathName().toStdString(), {});
        std::atomic<bool> cancel { false };
        const auto stems = sep.separate (mix, {}, cancel);
        tmpDir.deleteRecursively();

        std::vector<float> sumL ((size_t) n, 0.0f), sumR ((size_t) n, 0.0f);
        for (const auto& [_, s] : stems)
            for (int i = 0; i < n; ++i) { sumL[(size_t) i] += s.left[(size_t) i]; sumR[(size_t) i] += s.right[(size_t) i]; }
        const double sdrL = siSdr (sumL, mix.left), sdrR = siSdr (sumR, mix.right);
        check (stems.size() == numStems && sdrL > 50.0 && sdrR > 50.0,
               "STFT forward/inverse roundtrip across chunks (SI-SDR L " + juce::String (sdrL, 1).toStdString()
                   + " dB, R " + juce::String (sdrR, 1).toStdString() + " dB)");
    }

    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : (std::to_string (failures) + " FAILED").c_str());
    return failures == 0 ? 0 : 1;
}

int midiSelftest (const std::string& modelsDir)
{
    int failures = 0;
    auto check = [&] (bool ok, const std::string& what) { std::printf ("  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str()); failures += ok ? 0 : 1; };
    const juce::File models (juce::String::fromUTF8 (modelsDir.c_str()));
    std::atomic<bool> cancel { false };

    std::printf ("Piano decoder (semi-CRF)\n");
    {
        // 6 frames, 3 tracks: a note from frame 1 to 4 on track 0, a one-frame note at 2 on track 1, nothing on 2.
        constexpr int T = 6, K = 3;
        std::vector<float> S ((size_t) T * T * K, -1.0f);
        auto at = [&] (int e, int b, int k) -> float& { return S[((size_t) e * T + (size_t) b) * K + (size_t) k]; };
        at (4, 1, 0) = 5.0f;
        at (2, 2, 1) = 1.0f;
        const auto path = transcription::semiCrfDecode (S.data(), T, K, { 0, 0, 0 });
        using Iv = std::vector<std::pair<int, int>>;
        check (path[0] == Iv { { 1, 4 } } && path[1] == Iv { { 2, 2 } } && path[2].empty(), "the best intervals on each track");
        const auto forced = transcription::semiCrfDecode (S.data(), T, K, { 2, 3, 0 });
        check (forced[0].empty() && forced[1].empty(), "a forced start after a note leaves it out");
    }

    std::printf ("Note decoder (Basic Pitch)\n");
    {
        // Key 39 (C4) from frame 10 to 40 with an onset; key 43 from 60 to 100 with none (found by its rise).
        constexpr int n = 150, K = 88;
        std::vector<float> frames ((size_t) n * K, 0.0f), onsets = frames;
        for (int t = 10; t < 40; ++t) frames[(size_t) t * K + 39] = 0.8f;
        onsets[10 * K + 39] = 0.9f;
        for (int t = 60; t < 100; ++t) frames[(size_t) t * K + 43] = 0.7f;
        auto notes = transcription::basicPitchNotes (frames, onsets, n, 0, K);
        std::sort (notes.begin(), notes.end(), [] (const auto& a, const auto& b) { return a.start < b.start; });
        check (notes.size() == 2 && notes[0].start == 10 && notes[0].end == 40 && notes[0].key == 39
                   && notes[1].start == 60 && notes[1].end == 100 && notes[1].key == 43,
               "two notes at the right frames and keys (" + std::to_string (notes.size()) + " found)");
        frames.assign (frames.size(), 0.0f);
        onsets.assign (onsets.size(), 0.0f);
        for (int t = 10; t < 40; ++t) frames[(size_t) t * K + 39] = 0.8f;
        onsets[10 * K + 39] = 0.9f;
        check (transcription::basicPitchNotes (frames, onsets, n, 40, K).empty(), "a note below the stem's range is dropped");
    }

    // Synthetic notes: harmonics, a sharp attack, decaying.
    struct Expected { int pitch; double start, length; };
    auto render = [] (const std::vector<Expected>& notes, double seconds) {
        StereoAudio audio;
        audio.left.assign ((size_t) (seconds * sampleRate), 0.0f);
        for (const auto& e : notes)
        {
            const double f0 = 440.0 * std::pow (2.0, (e.pitch - 69) / 12.0);
            const int from = (int) (e.start * sampleRate), len = (int) (e.length * sampleRate);
            for (int i = 0; i < len; ++i)
            {
                const double t = (double) i / sampleRate;
                const double env = std::min (1.0, t / 0.003) * std::exp (-t * 3.0) * std::min (1.0, (len - i) / (0.02 * sampleRate));
                double v = 0;
                for (int h = 1; h <= 8; ++h) v += std::sin (2 * std::numbers::pi * f0 * h * t) * std::exp (-0.5 * h);
                audio.left[(size_t) (from + i)] += (float) (0.12 * env * v);
            }
        }
        audio.right = audio.left;
        return audio;
    };
    auto score = [&] (const Transcription& found, const std::vector<Expected>& expected, int minMatched, const char* name) {
        int matched = 0;
        for (const auto& e : expected)
            matched += std::any_of (found.notes.begin(), found.notes.end(), [&] (const MidiNote& n) {
                return n.pitch == e.pitch && std::fabs (n.start - e.start) < 0.06;
            }) ? 1 : 0;
        const int extra = (int) found.notes.size() - matched;
        const bool ok = matched >= minMatched && extra <= 2;
        if (! ok)
            for (const auto& n : found.notes) std::printf ("    found %d at %.3f-%.3f s, velocity %d\n", n.pitch, n.start, n.end, n.velocity);
        check (ok, std::string (name) + ": " + std::to_string (matched) + "/" + std::to_string (expected.size())
                       + " notes at the right pitch and time (within 60 ms), " + std::to_string (extra) + " extra");
    };

    std::printf ("Basic Pitch on synthetic notes\n");
    if (const auto f = models.getChildFile (juce::String (std::string (model::notesFile))); f.existsAsFile())
    {
        // C4 E4 G4 C5, then an A minor chord.
        const std::vector<Expected> melody { { 60, 0.5, 0.8 }, { 64, 1.5, 0.8 }, { 67, 2.5, 0.8 }, { 72, 3.5, 0.8 },
                                             { 57, 5.0, 1.5 }, { 60, 5.0, 1.5 }, { 64, 5.0, 1.5 } };
        score (transcribeNotes (f.getFullPathName().toStdString(), render (melody, 8), 80.0f, 1100.0f, cancel), melody,
               (int) melody.size(), "Basic Pitch");
    }
    else
        check (false, "missing " + f.getFullPathName().toStdString() + " (scripts/fetch_model.sh midi)");

    std::printf ("Transkun on synthetic notes\n");
    if (const auto f = models.getChildFile (juce::String (std::string (model::pianoFile))); f.existsAsFile())
    {
        // A rising run C3..A#4. Trained on real pianos, Transkun ignores a lone synthetic note in silence,
        // and synthetic notes from about C5 up, so the test stays in a run below that.
        std::vector<Expected> run;
        for (int p = 48; p <= 70; p += 2) run.push_back ({ p, 0.5 + (p - 48) * 0.25, 0.45 });
        score (transcribePiano (f.getFullPathName().toStdString(), render (run, 11), {}, cancel), run, (int) run.size() - 1, "Transkun");
    }
    else
        check (false, "missing " + f.getFullPathName().toStdString() + " (scripts/fetch_model.sh piano)");

    std::printf ("MIDI files\n");
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("juststem-midi-selftest-" + juce::Uuid().toString());
        dir.createDirectory();
        Transcription t;
        t.notes = { { 60, 0.5, 1.0, 100 }, { 60, 1.0, 1.25, 80 }, { 64, 2.0, 3.0, 64 } };
        t.pedals = { { 64, 0.4, 2.5, 127 } };
        const auto file = dir.getChildFile ("piano.mid"), shifted = dir.getChildFile ("up.mid");
        writeMidi (file, t, 97.0, "Piano", 0);
        writeTransposedMidi (file, shifted, 2);
        auto read = [] (const juce::File& f) {
            juce::FileInputStream in (f);
            juce::MidiFile m;
            m.readFrom (in);
            m.convertTimestampTicksToSeconds();
            std::vector<MidiNote> notes;
            int pedals = 0;
            for (const auto* e : *m.getTrack (0))
            {
                if (e->message.isNoteOn())
                    notes.push_back ({ e->message.getNoteNumber(), e->message.getTimeStamp(),
                                       e->noteOffObject != nullptr ? e->noteOffObject->message.getTimeStamp() : -1.0, e->message.getVelocity() });
                if (e->message.isController() && e->message.getControllerNumber() == 64) ++pedals;
            }
            return std::pair (notes, pedals);
        };
        const auto [back, pedals] = read (file);
        bool same = back.size() == t.notes.size() && pedals == 2;
        for (std::size_t i = 0; same && i < back.size(); ++i)
            same = back[i].pitch == t.notes[i].pitch && back[i].velocity == t.notes[i].velocity
                && std::fabs (back[i].start - t.notes[i].start) < 0.001 && std::fabs (back[i].end - t.notes[i].end) < 0.001;
        check (same, "notes, velocities and the sustain pedal read back at the same times (97 BPM file)");
        const auto [up, upPedals] = read (shifted);
        check (up.size() == back.size() && up[0].pitch == 62 && up[2].pitch == 66 && upPedals == 2
                   && std::fabs (up[2].start - 2.0) < 0.001,
               "transposed +2: every note moved, times and pedal kept");
        dir.deleteRecursively();
    }

    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : (std::to_string (failures) + " FAILED").c_str());
    return failures == 0 ? 0 : 1;
}
