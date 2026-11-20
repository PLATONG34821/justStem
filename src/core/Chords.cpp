#include "Chords.h"
#include "Analysis.h"
#include "Dsp.h"
#include "Pitch.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <future>
#include <numbers>

namespace juststem
{
namespace
{
using Chroma = std::array<float, 12>;

constexpr int intervals[numChordQualities][4] {
    { 0, 4, 7, -1 }, { 0, 3, 7, -1 }, { 0, 4, 7, 10 }, { 0, 4, 7, 11 },
    { 0, 3, 7, 10 }, { 0, 2, 7, -1 }, { 0, 5, 7, -1 }, { 0, 3, 6, -1 },
};
constexpr const char* suffixes[numChordQualities] { "", "m", "7", "maj7", "m7", "sus2", "sus4", "dim" };

// Scores are in "cosine similarity" units (a perfect match per beat is about 1).
constexpr float qualityBias[numChordQualities] { 0, 0, -0.03f, -0.03f, -0.03f, -0.05f, -0.05f, -0.06f }; // prefer plain triads
constexpr float bassWeight = 0.25f;     // the bass note confirms the root
constexpr float slashPenalty = 0.15f;   // a slash chord needs a bass note clearly stronger than the root
constexpr float inKeyBonus = 0.05f;     // every chord tone is in the key's scale
constexpr float noChordScore = 0.4f, quietNoChordScore = 1.5f;
constexpr float changeCost[3] { 0.15f, 0.3f, 0.45f }; // a new chord on a downbeat, on beat 3, elsewhere
constexpr float uniformChangeCost = 0.3f;             // first pass, before bar lines are known

double frameRate (int rate) { return (double) rate / dsp::onsetHop; }

/** Pitch class of frequency f for a tuning reference, and its distance from the semitone centre. */
std::pair<int, float> pitchClass (float f, double ref)
{
    const double pitch = 12.0 * std::log2 (f / ref) + 69.0;
    const double q = std::round (pitch);
    return { wrapPitchClass ((int) q), (float) std::fabs (pitch - q) };
}

float norm (const Chroma& c)
{
    float s = 0;
    for (float v : c) s += v * v;
    return std::sqrt (s);
}
} // namespace

// ------------------------------------------------------------------------------------ result helpers
std::vector<ChordResult::Bar> ChordResult::bars (double length) const
{
    std::vector<double> starts;
    if (firstDownbeat > 0) starts.push_back (0.0); // pickup
    for (std::size_t i = (size_t) firstDownbeat; i < beats.size(); i += beatsPerBar) starts.push_back (beats[i]);
    if (starts.empty()) starts.push_back (0.0);
    starts.front() = 0.0;                          // the first bar also holds anything before the first beat

    std::vector<Bar> out;
    std::size_t seg = 0;
    for (std::size_t b = 0; b < starts.size() && starts[b] < length; ++b)
    {
        Bar bar { starts[b], b + 1 < starts.size() ? std::min (starts[b + 1], length) : length, {} };
        while (seg < segments.size() && segments[seg].end <= bar.start + 1e-6) ++seg;
        for (std::size_t k = seg; k < segments.size() && segments[k].start < bar.end - 1e-6; ++k) bar.segments.push_back ((int) k);
        out.push_back (std::move (bar));
    }
    return out;
}

int ChordResult::segmentAt (double t) const
{
    const auto it = std::upper_bound (segments.begin(), segments.end(), t, [] (double v, const ChordSegment& s) { return v < s.start; });
    if (it == segments.begin()) return -1;
    const auto i = (int) (it - segments.begin()) - 1;
    return t < segments[(size_t) i].end ? i : -1;
}

std::string chordName (const Chord& c, const KeyResult& key, int transpose)
{
    if (c.isNone()) return "N.C.";
    static constexpr const char* neutral[] { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
    // Spell like the key signature: the major key with the same signature decides.
    const auto k = key.transposed (transpose);
    const int major = k.minor ? (k.tonic + 3) % 12 : k.tonic;
    static constexpr int signature[12] { 0, -1, 1, -1, 1, -1, 1, 1, -1, 1, -1, 1 }; // C Db D Eb E F F# G Ab A Bb B
    const auto* names = signature[major] > 0 ? sharpNames : signature[major] < 0 ? flatNames : neutral;
    auto pc = [&] (int p) { return wrapPitchClass (p + transpose); };

    std::string s = std::string (names[pc (c.root)]) + suffixes[(int) c.quality];
    if (c.bass >= 0) s += std::string ("/") + names[pc (c.bass)];
    return s;
}

std::string chordChart (const ChordResult& r, double length, const KeyResult& key, int transpose, int barsPerLine)
{
    const auto bars = r.bars (length);
    std::vector<std::string> cells;
    std::size_t width = 0;
    for (const auto& bar : bars)
    {
        std::string cell;
        for (int s : bar.segments)
        {
            // Skip slivers (under a beat) except when they're all the bar has.
            const auto& seg = r.segments[(size_t) s];
            const double inBar = std::min (seg.end, bar.end) - std::max (seg.start, bar.start);
            if (bar.segments.size() > 1 && inBar < 0.2 * (bar.end - bar.start)) continue;
            cell += (cell.empty() ? "" : " ") + chordName (seg.chord, key, transpose);
        }
        width = std::max (width, cell.size());
        cells.push_back (std::move (cell));
    }
    std::string out;
    for (std::size_t i = 0; i < cells.size(); ++i)
    {
        out += "| " + cells[i] + std::string (width - cells[i].size() + 1, ' ');
        if ((i + 1) % (size_t) barsPerLine == 0 || i + 1 == cells.size()) out += "|\n";
    }
    return out;
}

// ------------------------------------------------------------------------------------ beats
std::vector<double> trackBeats (const std::vector<float>& env, double length, int rate, float bpm)
{
    // Dynamic programming beat tracker (Ellis 2007): beats sit on strong onsets and are spaced
    // close to the tempo's period.
    if (env.size() < 64 || bpm <= 0) return {};
    double mean = 0, var = 0;
    for (float v : env) mean += v;
    mean /= (double) env.size();
    for (float v : env) var += (v - mean) * (v - mean);
    const double sd = std::sqrt (var / (double) env.size());
    if (sd <= 1e-9) return {};

    const int n = (int) env.size();
    const double period = frameRate (rate) * 60.0 / bpm;
    const int lo = std::max (1, (int) std::round (period / 2)), hi = (int) std::round (period * 2);
    std::vector<float> penalty ((size_t) hi + 1);
    for (int d = lo; d <= hi; ++d) penalty[(size_t) d] = (float) (100.0 * std::pow (std::log (d / period), 2)); // "tightness" 100
    std::vector<float> score ((size_t) n);
    std::vector<int> back ((size_t) n, -1);
    for (int t = 0; t < n; ++t)
    {
        float best = 0;
        for (int d = lo; d <= hi && d <= t; ++d)
            if (const float v = score[(size_t) (t - d)] - penalty[(size_t) d]; v > best) { best = v; back[(size_t) t] = t - d; }
        score[(size_t) t] = (float) (env[(size_t) t] / sd) + best;
    }
    int t = n - 1;
    for (int i = std::max (0, n - (int) std::ceil (period)); i < n; ++i)
        if (score[(size_t) i] > score[(size_t) t]) t = i;
    std::vector<double> beats;
    for (; t >= 0; t = back[(size_t) t])
        beats.push_back ((t * dsp::onsetHop + dsp::onsetSize / 2 + dsp::onsetHop) / (double) rate); // + 1 hop: measured bias
    std::reverse (beats.begin(), beats.end());

    // Continue the grid to the start and the end of the song.
    const double step = 60.0 / bpm;
    while (beats.front() - step >= 0) beats.insert (beats.begin(), beats.front() - step);
    while (beats.back() + step < length) beats.push_back (beats.back() + step);
    return beats;
}

// ------------------------------------------------------------------------------------ chords
ChordPeaks chordPeaks (const std::vector<float>& harmonic, const std::vector<float>& bass, int rate)
{
    // Independent of each other: the bass (the larger FFT, the slower) on a second thread.
    auto bassPeaks = std::async (std::launch::async, [&] { return dsp::spectralPeaks (bass, rate, 16384, 2048, 30.0f, 300.0f); });
    auto harmonicPeaks = dsp::spectralPeaks (harmonic, rate, 8192, 2048, 60.0f, 2500.0f);
    return { std::move (harmonicPeaks), bassPeaks.get() };
}

ChordResult detectChords (const std::vector<float>& harmonic, const std::vector<float>& bass, const std::vector<float>& rhythm,
                          int rate, float bpm, const KeyResult& key)
{
    auto beats = trackBeats (dsp::onsetEnvelope (rhythm, rate), (double) rhythm.size() / rate, rate, bpm);
    return detectChordsOnBeats (chordPeaks (harmonic, bass, rate), (double) harmonic.size() / rate, std::move (beats), rate, bpm, key);
}

ChordResult detectChordsOnBeats (const ChordPeaks& peaks, double length, std::vector<double> beats, int rate, float bpm,
                                 const KeyResult& key)
{
    ChordResult result;
    if (length <= 0) return result;

    result.beats = std::move (beats);
    if (result.beats.empty()) // no rhythm to follow: a plain grid
        for (double t = 0, step = 60.0 / (bpm > 0 ? bpm : 120.0); t < length; t += step) result.beats.push_back (t);

    // Units: [0, beat 0), [beat 0, beat 1), ..., [last beat, end).
    // Unit u starts at beat u - 1 (unit 0 may be empty when the first beat is at 0).
    std::vector<double> bounds { 0.0 };
    bounds.insert (bounds.end(), result.beats.begin(), result.beats.end());
    bounds.push_back (std::max (length, bounds.back()));
    const int units = (int) bounds.size() - 1;
    auto unitOf = [&] (double t) {
        return std::clamp ((int) (std::upper_bound (bounds.begin(), bounds.end(), t) - bounds.begin()) - 1, 0, units - 1);
    };

    // Chroma of the chord instruments per unit: each spectral peak counts for its nearest semitone.
    const auto& hp = peaks.harmonic;
    const double ref = 440.0 * std::pow (2.0, hp.tuning / 12.0);
    std::vector<Chroma> chroma ((size_t) units, Chroma {});
    std::vector<float> activity ((size_t) units, 0.0f);
    for (std::size_t i = 0; i < hp.frames.size(); ++i)
    {
        const int u = unitOf ((double) (i * (size_t) hp.hop + (size_t) hp.size / 2) / rate);
        for (const auto& p : hp.frames[i])
        {
            const auto [pc, dist] = pitchClass (p.freq, ref);
            const float w = std::sqrt (p.mag) * (float) std::pow (std::cos (std::numbers::pi * dist), 2);
            chroma[(size_t) u][(size_t) pc] += w;
            activity[(size_t) u] += w;
        }
    }
    // Bass note per frame: the lowest strong peak (overtones of a bass note sit above it).
    const auto& bp = peaks.bass;
    std::vector<Chroma> bassChroma ((size_t) units, Chroma {});
    std::vector<float> bassActivity ((size_t) units, 0.0f);
    for (std::size_t i = 0; i < bp.frames.size(); ++i)
    {
        const auto& f = bp.frames[i];
        if (f.empty()) continue;
        float strongest = 0;
        for (const auto& p : f) strongest = std::max (strongest, p.mag);
        const auto low = std::find_if (f.begin(), f.end(), [&] (const dsp::Peak& p) { return p.mag >= 0.3f * strongest; });
        const auto [pc, dist] = pitchClass (low->freq, ref);
        const int u = unitOf ((double) (i * (size_t) bp.hop + (size_t) bp.size / 2) / rate);
        const float w = std::sqrt (low->mag) * (float) std::pow (std::cos (std::numbers::pi * dist), 2);
        bassChroma[(size_t) u][(size_t) pc] += w;
        bassActivity[(size_t) u] += w;
    }
    auto median = [] (std::vector<float> v) {
        if (v.empty()) return 0.0f;
        std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    const float typical = median (activity), typicalBass = median (bassActivity);

    // Candidate chords: no chord, then every root and quality in root position and with each
    // other chord tone in the bass.
    std::vector<Chord> states { Chord {} };
    std::vector<Chroma> templates { Chroma {} };
    std::vector<float> bias { 0.0f };
    std::array<bool, 12> scale {};
    {
        const bool minor = key.minor;
        static constexpr int majorSteps[] { 0, 2, 4, 5, 7, 9, 11 }, minorSteps[] { 0, 2, 3, 5, 7, 8, 10, 11 }; // minor: + leading tone
        if (minor) for (int s : minorSteps) scale[(size_t) ((key.tonic + s) % 12)] = true;
        else for (int s : majorSteps) scale[(size_t) ((key.tonic + s) % 12)] = true;
    }
    for (int root = 0; root < 12; ++root)
        for (int q = 0; q < numChordQualities; ++q)
        {
            Chroma t {};
            bool inKey = true;
            for (int iv : intervals[q])
                if (iv >= 0)
                {
                    t[(size_t) ((root + iv) % 12)] = 1.0f;
                    inKey = inKey && scale[(size_t) ((root + iv) % 12)];
                }
            const float n = norm (t);
            for (auto& v : t) v /= n;
            for (int iv : intervals[q])
            {
                if (iv < 0) continue;
                states.push_back ({ root, (ChordQuality) q, iv == 0 ? -1 : (root + iv) % 12 });
                templates.push_back (t);
                bias.push_back (qualityBias[q] + (inKey ? inKeyBonus : 0.0f) - (iv == 0 ? 0.0f : slashPenalty));
            }
        }
    const int numStates = (int) states.size();

    std::vector<float> emission ((size_t) units * (size_t) numStates);
    for (int u = 0; u < units; ++u)
    {
        auto* e = &emission[(size_t) u * (size_t) numStates];
        const bool quiet = activity[(size_t) u] < 0.05f * typical;
        auto c = chroma[(size_t) u];
        if (const float n = norm (c); n > 0) for (auto& v : c) v /= n;
        auto b = bassChroma[(size_t) u];
        const float bmax = *std::max_element (b.begin(), b.end());
        const bool bassPlays = bassActivity[(size_t) u] >= 0.1f * typicalBass && bmax > 0;
        for (auto& v : b) v = bassPlays ? v / bmax : 0.0f;

        e[0] = quiet ? quietNoChordScore : noChordScore;
        for (int s = 1; s < numStates; ++s)
        {
            float dot = 0;
            for (int i = 0; i < 12; ++i) dot += c[(size_t) i] * templates[(size_t) s][(size_t) i];
            const auto& st = states[(size_t) s];
            e[s] = (quiet ? 0.0f : dot) + bias[(size_t) s] + bassWeight * b[(size_t) (st.bass >= 0 ? st.bass : st.root)];
        }
    }

    // Viterbi: staying on a chord is free, changing costs `cost (unit)`.
    auto decode = [&] (auto cost) {
        std::vector<float> delta (emission.begin(), emission.begin() + numStates), next ((size_t) numStates);
        std::vector<int> bestPrev ((size_t) units, 0);
        std::vector<std::uint8_t> switched ((size_t) units * (size_t) numStates, 0);
        for (int u = 1; u < units; ++u)
        {
            const int best = (int) (std::max_element (delta.begin(), delta.end()) - delta.begin());
            bestPrev[(size_t) u] = best;
            const float change = delta[(size_t) best] - cost (u);
            const auto* e = &emission[(size_t) u * (size_t) numStates];
            for (int s = 0; s < numStates; ++s)
            {
                const bool sw = change > delta[(size_t) s];
                switched[(size_t) u * (size_t) numStates + (size_t) s] = sw;
                next[(size_t) s] = e[s] + (sw ? change : delta[(size_t) s]);
            }
            std::swap (delta, next);
        }
        std::vector<int> path ((size_t) units);
        int s = (int) (std::max_element (delta.begin(), delta.end()) - delta.begin());
        for (int u = units - 1; u >= 0; --u)
        {
            path[(size_t) u] = s;
            if (u > 0 && switched[(size_t) u * (size_t) numStates + (size_t) s]) s = bestPrev[(size_t) u];
        }
        return path;
    };
    // Unit u starts at beat (u - 1). First pass: find which beat of four the chords change on
    // most often; that's the bar line. Second pass: prefer changes on bar lines.
    const auto first = decode ([] (int) { return uniformChangeCost; });
    std::array<int, ChordResult::beatsPerBar> changes {};
    for (int u = 2; u < units; ++u)
        if (first[(size_t) u] != first[(size_t) u - 1]) ++changes[(size_t) ((u - 1) % ChordResult::beatsPerBar)];
    result.firstDownbeat = (int) (std::max_element (changes.begin(), changes.end()) - changes.begin());
    const auto path = decode ([&] (int u) {
        const int beatInBar = ((u - 1 - result.firstDownbeat) % 4 + 4) % 4;
        return changeCost[beatInBar == 0 ? 0 : beatInBar == 2 ? 1 : 2];
    });

    for (int u = 0; u < units; ++u)
    {
        const auto& chord = states[(size_t) path[(size_t) u]];
        if (! result.segments.empty() && result.segments.back().chord == chord) result.segments.back().end = bounds[(size_t) u + 1];
        else result.segments.push_back ({ bounds[(size_t) u], bounds[(size_t) u + 1], chord });
    }
    // A sliver before the first beat joins the next chord.
    if (result.segments.size() > 1 && result.segments.front().end < 0.15)
    {
        result.segments[1].start = 0;
        result.segments.erase (result.segments.begin());
    }
    return result;
}

} // namespace juststem
