// juststem_cli's self-tests: `selftest` (separation) and `analyze-selftest` (everything else in core/),
// on synthetic signals with known answers. See tools/cli.cpp for the commands.
#include "cli.h"
#include "../src/core/Analysis.h"
#include "../src/core/Fft.h"
#include "../src/core/PitchShift.h"
#include "../src/core/Resampler.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numbers>

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
            std::vector<float> w ((size_t) fftN), mag ((size_t) fftN / 2 + 1);
            for (int i = 0; i < fftN; ++i) w[(size_t) i] = v[(size_t) (from + i)] * (float) (0.5 - 0.5 * std::cos (2 * std::numbers::pi * i / fftN));
            RealFft (fftN).magnitudes (w.data(), mag.data());
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

    std::printf ("%s\n", failures == 0 ? "ALL PASSED" : (std::to_string (failures) + " FAILED").c_str());
    return failures == 0 ? 0 : 1;
}
