#include "Transcriber.h"
#include "Resampler.h"

#include <onnxruntime_cxx_api.h>

#include <signalsmith-linear/fft.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
#include <stdexcept>

namespace juststem
{
Ort::Env& ortEnv(); // Separator.cpp: one ONNX Runtime environment for every model
void configureOrtSession (Ort::SessionOptions& so, bool cpuSession);

namespace
{
void checkCancel (const std::atomic<bool>& cancel)
{
    if (cancel.load()) throw std::runtime_error ("cancelled");
}

/** A CPU session. ONNX Runtime takes native paths (wide strings on Windows); `path` is UTF-8. */
std::unique_ptr<Ort::Session> openSession (const std::string& path)
{
    auto& env = ortEnv(); // initialises the ONNX Runtime C++ API table before SessionOptions is made
    Ort::SessionOptions so;
    configureOrtSession (so, true);
    const std::filesystem::path p (std::u8string (path.begin(), path.end()));
    return std::make_unique<Ort::Session> (env, p.c_str(), so);
}

Ort::Value tensor (std::vector<float>& data, const std::vector<int64_t>& shape)
{
    ortEnv();
    static const auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
    return Ort::Value::CreateTensor<float> (mem, data.data(), data.size(), shape.data(), shape.size());
}

int clampVelocity (long v) { return (int) std::clamp (v, 1L, 127L); }

// ============================================================================ Basic Pitch
// Constants from basic_pitch/constants.py and inference.py (ICASSP 2022 model).
namespace bp
{
    constexpr int rate = 22050, fftHop = 256, keys = 88, midiOffset = 21;
    constexpr int windowSamples = rate * 2 - fftHop;            // AUDIO_N_SAMPLES 43844
    constexpr int windowFrames = rate / fftHop * 2;             // ANNOT_N_FRAMES 172
    constexpr int overlapFrames = 30, overlapSamples = overlapFrames * fftHop;
    constexpr int hop = windowSamples - overlapSamples;          // 36164
    constexpr float onsetThreshold = 0.5f, frameThreshold = 0.3f;
    constexpr int minNoteFrames = 11;                            // round(127.7 ms * 22050 / 256 / 1000)
    constexpr int energyTolerance = 11;
    constexpr double alignment = 0.0018;                         // MAGIC_ALIGNMENT_OFFSET
    constexpr int batch = 8;                                     // windows per model run (~1.4 MB in)
} // namespace bp

/** Basic Pitch's model_frames_to_time: frame times, with its per-window alignment correction. */
double basicPitchFrameTime (int frame)
{
    const double windowOffset = (double) bp::fftHop / bp::rate * (bp::windowFrames - (double) bp::windowSamples / bp::fftHop)
                                + bp::alignment;
    return frame * (double) bp::fftHop / bp::rate - windowOffset * std::floor ((double) frame / bp::windowFrames);
}

int hzToKey (float hz) { return (int) std::lround (12.0 * std::log2 (hz / 440.0) + 69.0 - bp::midiOffset); }

// ============================================================================ Transkun
// From transkun 2.0.1's ModelTransformer.py and pretrained/2.0.conf.
namespace tk
{
    constexpr int rate = 44100, hopSize = 1024, windowSize = 4096, bins = windowSize / 2 + 1;
    constexpr int mels = 229, windows = 6, tracks = 90, ctxSize = 256;
    constexpr double eps = 1e-5;
    constexpr double segmentSeconds = 16, stepSeconds = 8;
    constexpr int padSamples = 352'800;                                  // ceil((16 - 8) * fs)
    constexpr int stepSize = 345 * hopSize;                              // ceil(8 * fs / hop) * hop
    constexpr int segmentSize = 705'600;                                 // ceil(16 * fs)
    constexpr int startFrame = 344;                                      // floor(8 * fs / hop)
    constexpr int lastFrame = 689;                                       // round(segmentSize / hop)
    constexpr int stepFrames = stepSize / hopSize;
    /** Track k is a pedal (controller 64 or 67, stored negative) or MIDI note 21..108. */
    constexpr int symbol (int k) { return k == 0 ? -64 : k == 1 ? -67 : k + 19; }
} // namespace tk

struct TkNote
{
    double start, end;
    int symbol, velocity;
    bool hasOnset, hasOffset;
};

bool earlier (const TkNote& a, const TkNote& b)
{
    return std::tie (a.start, a.end, a.symbol) < std::tie (b.start, b.end, b.symbol);
}

/** ContinuousBernoulli(logits).mean, as torch computes it (a Taylor series near p = 0.5). */
double continuousBernoulliMean (double logit)
{
    const double p = 1.0 / (1.0 + std::exp (-logit));
    if (p > 0.499 && p < 0.501)
    {
        const double x = p - 0.5;
        return 0.5 + (1.0 / 3.0 + 16.0 / 45.0 * x * x) * x;
    }
    return p / (2.0 * p - 1.0) + 1.0 / (std::log1p (-p) - std::log (p));
}

/** Transkun's resolveOverlapping: a note cut short where the next one on the same key starts;
    notes left with no length dropped. */
std::vector<TkNote> resolveOverlapping (std::vector<TkNote> notes)
{
    std::stable_sort (notes.begin(), notes.end(), earlier);
    std::map<int, std::size_t> lastOf;
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        if (const auto it = lastOf.find (notes[i].symbol); it != lastOf.end() && notes[it->second].end > notes[i].start)
            notes[it->second].end = notes[i].start;
        lastOf[notes[i].symbol] = i;
    }
    std::stable_sort (notes.begin(), notes.end(), earlier);
    std::erase_if (notes, [] (const TkNote& n) { return ! (n.start < n.end); });
    return notes;
}
} // namespace

namespace transcription
{
std::vector<FrameNote> basicPitchNotes (std::vector<float>& frames, std::vector<float>& onsets, int n, int minKey, int maxKey)
{
    constexpr int K = bp::keys;
    auto at = [] (int t, int k) { return (std::size_t) t * K + (std::size_t) k; };
    minKey = std::clamp (minKey, 0, K);
    maxKey = std::clamp (maxKey, 0, K);
    for (int t = 0; t < n; ++t)
        for (int k = 0; k < K; ++k)
            if (k < minKey || k >= maxKey) frames[at (t, k)] = onsets[at (t, k)] = 0;

    // Inferred onsets: where a frame rises over both of the last two, scaled to the onsets' peak.
    std::vector<float> diff (frames.size(), 0.0f);
    float maxDiff = 0, maxOnset = 0;
    for (std::size_t i = 0; i < onsets.size(); ++i) maxOnset = std::max (maxOnset, onsets[i]);
    for (int t = 2; t < n; ++t)
        for (int k = 0; k < K; ++k)
        {
            const float d = std::max (0.0f, std::min (frames[at (t, k)] - frames[at (t - 1, k)], frames[at (t, k)] - frames[at (t - 2, k)]));
            diff[at (t, k)] = d;
            maxDiff = std::max (maxDiff, d);
        }
    if (maxDiff > 0)
        for (std::size_t i = 0; i < onsets.size(); ++i) onsets[i] = std::max (onsets[i], maxOnset * diff[i] / maxDiff);

    std::vector<float> remaining = frames;
    std::vector<FrameNote> notes;
    auto mean = [&] (int a, int b, int k) {
        double s = 0;
        for (int t = a; t < b; ++t) s += frames[at (t, k)];
        return (float) (s / (b - a));
    };
    auto clearAround = [&] (int t, int k) {
        remaining[at (t, k)] = 0;
        if (k < K - 1) remaining[at (t, k + 1)] = 0;
        if (k > 0) remaining[at (t, k - 1)] = 0;
    };

    // Onset peaks over the threshold, latest first (and the highest key first at the same time).
    for (int t = n - 2; t >= 1; --t)
        for (int k = K - 1; k >= 0; --k)
        {
            const float o = onsets[at (t, k)];
            if (o < bp::onsetThreshold || ! (o > onsets[at (t - 1, k)] && o > onsets[at (t + 1, k)])) continue;
            int i = t + 1, quiet = 0;
            while (i < n - 1 && quiet < bp::energyTolerance)
            {
                quiet = remaining[at (i, k)] < bp::frameThreshold ? quiet + 1 : 0;
                ++i;
            }
            i -= quiet;
            if (i - t <= bp::minNoteFrames) continue;
            for (int j = t; j < i; ++j) clearAround (j, k);
            notes.push_back ({ t, i, k, mean (t, i, k) });
        }

    // Melodia trick: grow notes out from the strongest frames left. The energy only ever drops to 0,
    // so going through the frames from strongest to weakest, skipping cleared ones, visits each
    // remaining maximum in the order the original's repeated argmax does (ties: earliest first).
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < remaining.size(); ++i)
        if (remaining[i] > bp::frameThreshold) order.push_back (i);
    std::stable_sort (order.begin(), order.end(), [&] (std::size_t a, std::size_t b) { return remaining[a] > remaining[b]; });
    for (const auto idx : order)
    {
        if (remaining[idx] != frames[idx]) continue; // cleared by a note found earlier
        const int mid = (int) (idx / K), k = (int) (idx % K);
        remaining[idx] = 0;

        int i = mid + 1, quiet = 0;
        while (i < n - 1 && quiet < bp::energyTolerance)
        {
            quiet = remaining[at (i, k)] < bp::frameThreshold ? quiet + 1 : 0;
            clearAround (i, k);
            ++i;
        }
        const int end = i - 1 - quiet;

        i = mid - 1;
        quiet = 0;
        while (i > 0 && quiet < bp::energyTolerance)
        {
            quiet = remaining[at (i, k)] < bp::frameThreshold ? quiet + 1 : 0;
            clearAround (i, k);
            --i;
        }
        const int start = i + 1 + quiet;

        if (end - start <= bp::minNoteFrames) continue;
        notes.push_back ({ start, end, k, mean (start, end, k) });
    }
    return notes;
}

std::vector<std::vector<std::pair<int, int>>> semiCrfDecode (const float* S, int T, int K, const std::vector<int>& forcedStart)
{
    auto s = [&] (int e, int b, int k) { return S[((std::size_t) e * (std::size_t) T + (std::size_t) b) * (std::size_t) K + (std::size_t) k]; };
    // q[b]: the best score from frame b on; ptr: for each b, -1 (no note starts at b) or the note's end - b - 1.
    std::vector<float> q ((std::size_t) T * (std::size_t) K);
    std::vector<int> ptr ((std::size_t) std::max (0, T - 1) * (std::size_t) K);
    std::vector<float> best ((std::size_t) K);
    for (int k = 0; k < K; ++k) q[(std::size_t) (T - 1) * K + k] = std::max (0.0f, s (T - 1, T - 1, k));
    for (int b = T - 2; b >= 0; --b)
    {
        int* sel = &ptr[(std::size_t) (T - 2 - b) * K];
        for (int k = 0; k < K; ++k)
        {
            best[(std::size_t) k] = q[(std::size_t) (b + 1) * K + k]; // skip this frame (the skip score is 0)
            sel[k] = -1;
        }
        for (int e = b + 1; e < T; ++e)
        {
            const float* row = &S[((std::size_t) e * T + b) * K];
            const float* qe = &q[(std::size_t) e * K];
            for (int k = 0; k < K; ++k)
                if (const float v = qe[k] + row[k]; v > best[(std::size_t) k]) // first maximum wins, like torch.max
                {
                    best[(std::size_t) k] = v;
                    sel[k] = e - b - 1;
                }
        }
        for (int k = 0; k < K; ++k) q[(std::size_t) b * K + k] = best[(std::size_t) k] + std::max (0.0f, s (b, b, k));
    }

    std::vector<std::vector<std::pair<int, int>>> result ((std::size_t) K);
    for (int k = 0; k < K; ++k)
    {
        auto& out = result[(std::size_t) k];
        int j = k < (int) forcedStart.size() ? forcedStart[(std::size_t) k] : 0;
        while (j < T - 1)
        {
            const int sel = ptr[(std::size_t) (T - j - 2) * K + k];
            if (s (j, j, k) > 0) out.emplace_back (j, j);
            if (sel < 0) ++j;
            else
            {
                const int e = sel + j + 1;
                out.emplace_back (j, e);
                j = e;
            }
        }
        if (T > 0 && s (T - 1, T - 1, k) > 0) out.emplace_back (T - 1, T - 1);
    }
    return result;
}

std::vector<float> pianoFeatures (const std::vector<const float*>& channels, int length, const std::vector<float>& frontEnd, int& nFrames)
{
    using namespace tk;
    nFrames = (length + hopSize - 1) / hopSize + 1;
    const int leftPad = windowSize / 2;
    const auto padded = (std::size_t) ((nFrames - 1) * hopSize + windowSize);
    const float* win = frontEnd.data();
    const float* freq2mels = frontEnd.data() + windows * windowSize; // [bins][mels]

    // Each channel framed as torch's unfold over the zero-padded segment.
    std::vector<std::vector<float>> x;
    for (const float* c : channels)
    {
        auto& v = x.emplace_back (padded, 0.0f);
        std::copy (c, c + length, v.begin() + leftPad);
    }

    // Gain normalisation over every frame of every channel (overlapping samples count once per frame),
    // with torch.std's unbiased estimate.
    double sum = 0, sumSq = 0;
    for (const auto& v : x)
        for (int f = 0; f < nFrames; ++f)
            for (int i = 0; i < windowSize; ++i)
            {
                const double s = v[(std::size_t) (f * hopSize + i)];
                sum += s;
                sumSq += s * s;
            }
    const double count = (double) x.size() * nFrames * windowSize;
    const double mean = sum / count;
    const double stdDev = std::sqrt (std::max (0.0, (sumSq - sum * mean) / (count - 1)));
    const float scale = (float) (1.0 / (stdDev + 1e-8)), offset = (float) mean;

    // Only the non-zero part of each mel filter is used.
    std::vector<std::pair<int, int>> range ((std::size_t) mels, { bins, 0 });
    for (int b = 0; b < bins; ++b)
        for (int m = 0; m < mels; ++m)
            if (freq2mels[(std::size_t) b * mels + m] != 0.0f)
            {
                range[(std::size_t) m].first = std::min (range[(std::size_t) m].first, b);
                range[(std::size_t) m].second = std::max (range[(std::size_t) m].second, b + 1);
            }

    signalsmith::linear::RealFFT<float> fft (windowSize);
    std::vector<float> frame ((std::size_t) windowSize), power ((std::size_t) bins);
    std::vector<std::complex<float>> spec ((std::size_t) windowSize / 2);
    std::vector<float> out ((std::size_t) nFrames * mels * windows);
    const float norm = 1.0f / (float) windowSize / (float) x.size(); // rfft "ortho" squared, averaged over channels
    const double logEps = std::log (eps);
    for (int f = 0; f < nFrames; ++f)
        for (int w = 0; w < windows; ++w)
        {
            std::fill (power.begin(), power.end(), 0.0f);
            for (const auto& v : x)
            {
                const float* src = &v[(std::size_t) (f * hopSize)];
                const float* wv = win + (std::size_t) w * windowSize;
                for (int i = 0; i < windowSize; ++i) frame[(std::size_t) i] = (src[i] - offset) * scale * wv[i];
                fft.fft (frame.data(), spec.data());
                power[0] += spec[0].real() * spec[0].real();
                power[(std::size_t) bins - 1] += spec[0].imag() * spec[0].imag(); // Nyquist, packed with DC
                for (int b = 1; b < bins - 1; ++b) power[(std::size_t) b] += std::norm (spec[(std::size_t) b]);
            }
            for (int m = 0; m < mels; ++m)
            {
                double mel = 0;
                for (int b = range[(std::size_t) m].first; b < range[(std::size_t) m].second; ++b)
                    mel += (double) power[(std::size_t) b] * norm * freq2mels[(std::size_t) b * mels + m];
                out[((std::size_t) f * mels + m) * windows + w] = (float) ((std::log (mel + eps) - logEps) / -logEps);
            }
        }
    return out;
}
} // namespace transcription

Transcription transcribeNotes (const std::string& modelPath, const StereoAudio& audio, float minHz, float maxHz,
                               const std::atomic<bool>& cancel)
{
    std::vector<float> mono (audio.size());
    for (std::size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (audio.left[i] + audio.right[i]);
    mono = resample (mono, sampleRate, bp::rate);
    const auto length = mono.size();
    mono.insert (mono.begin(), (std::size_t) bp::overlapSamples / 2, 0.0f);

    auto session = openSession (modelPath);
    // Names from basic_pitch/inference.py (the contours, output 0, aren't used).
    const char* inNames[] { "serving_default_input_2:0" };
    const char* outNames[] { "StatefulPartitionedCall:1", "StatefulPartitionedCall:2" }; // notes, onsets

    // Windows of 2 s, 30 frames overlapping; half the overlap is dropped from each side of each window.
    const int nWindows = (int) ((mono.size() + bp::hop - 1) / bp::hop);
    constexpr int keep = bp::windowFrames - bp::overlapFrames;
    std::vector<float> frames, onsets;
    frames.reserve ((std::size_t) nWindows * keep * bp::keys);
    onsets.reserve (frames.capacity());
    for (int first = 0; first < nWindows; first += bp::batch)
    {
        checkCancel (cancel);
        const int count = std::min (bp::batch, nWindows - first);
        std::vector<float> input ((std::size_t) count * bp::windowSamples, 0.0f);
        for (int w = 0; w < count; ++w)
        {
            const auto from = (std::size_t) (first + w) * bp::hop;
            const auto n = std::min ((std::size_t) bp::windowSamples, mono.size() - from);
            std::copy_n (mono.begin() + (std::ptrdiff_t) from, n, input.begin() + (std::ptrdiff_t) w * bp::windowSamples);
        }
        auto in = tensor (input, { count, bp::windowSamples, 1 });
        auto out = session->Run (Ort::RunOptions {}, inNames, &in, 1, outNames, 2);
        for (int o = 0; o < 2; ++o)
        {
            const float* data = out[(std::size_t) o].GetTensorData<float>();
            auto& dest = o == 0 ? frames : onsets;
            for (int w = 0; w < count; ++w)
            {
                const float* from = data + ((std::size_t) w * bp::windowFrames + bp::overlapFrames / 2) * bp::keys;
                dest.insert (dest.end(), from, from + (std::size_t) keep * bp::keys);
            }
        }
    }
    const int nFrames = std::min ((int) (frames.size() / bp::keys), (int) ((double) length / bp::hop * keep));
    frames.resize ((std::size_t) nFrames * bp::keys);
    onsets.resize (frames.size());

    checkCancel (cancel);
    const auto found = transcription::basicPitchNotes (frames, onsets, nFrames, hzToKey (minHz), hzToKey (maxHz));
    Transcription result;
    for (const auto& n : found)
        result.notes.push_back ({ n.key + bp::midiOffset, basicPitchFrameTime (n.start), basicPitchFrameTime (n.end),
                                  clampVelocity (std::lround (127.0 * n.amplitude)) });
    std::stable_sort (result.notes.begin(), result.notes.end(), [] (const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
    return result;
}

Transcription transcribePiano (const std::string& modelPath, const StereoAudio& audio,
                               const std::function<void (double)>& onProgress, const std::atomic<bool>& cancel)
{
    using namespace tk;
    const std::filesystem::path model (std::u8string (modelPath.begin(), modelPath.end()));
    auto sibling = [&] (std::string_view name) { return model.parent_path() / std::filesystem::path (std::u8string (name.begin(), name.end())); };

    std::vector<float> frontEnd ((std::size_t) windows * windowSize + (std::size_t) bins * mels);
    {
        std::ifstream in (sibling (model::pianoFrontEndFile), std::ios::binary);
        in.read (reinterpret_cast<char*> (frontEnd.data()), (std::streamsize) (frontEnd.size() * sizeof (float)));
        if (! in || in.peek() != std::char_traits<char>::eof())
            throw std::runtime_error ("The piano MIDI model's front end is damaged: " + std::string (model::pianoFrontEndFile));
    }
    const auto u8 = [] (const std::filesystem::path& p) { const auto s = p.u8string(); return std::string (s.begin(), s.end()); };
    auto session = openSession (modelPath);
    auto heads = openSession (u8 (sibling (model::pianoHeadsFile)));
    const char* inNames[] { "featuresBatch" };
    const char* outNames[] { "S", "ctx" };
    const char* headInNames[] { "attr" };
    const char* headOutNames[] { "velLogits", "ofRaw" };

    // The song padded with 8 s of silence each side, in 16 s segments 8 s apart. The decode of each
    // segment starts where the notes the last one finished ended (forcedStart).
    const auto length = (std::size_t) audio.size() + 2 * (std::size_t) padSamples;
    const int nSegments = (int) ((length + stepSize - 1) / stepSize);
    std::vector<int> startPos ((std::size_t) tracks, startFrame);
    std::map<int, std::vector<TkNote>> byType;
    std::vector<float> left ((std::size_t) segmentSize), right ((std::size_t) segmentSize);
    const double frameSeconds = (double) hopSize / rate;

    for (int seg = 0; seg < nSegments; ++seg)
    {
        checkCancel (cancel);
        const auto from = (std::size_t) seg * stepSize;
        for (int i = 0; i < segmentSize; ++i)
        {
            const auto pos = from + (std::size_t) i;
            const bool inSong = pos >= (std::size_t) padSamples && pos - padSamples < audio.size();
            left[(std::size_t) i] = inSong ? audio.left[pos - padSamples] : 0.0f;
            right[(std::size_t) i] = inSong ? audio.right[pos - padSamples] : 0.0f;
        }
        int T = 0;
        auto features = transcription::pianoFeatures ({ left.data(), right.data() }, segmentSize, frontEnd, T);
        auto in = tensor (features, { 1, T, mels, windows });
        auto out = session->Run (Ort::RunOptions {}, inNames, &in, 1, outNames, 2);
        const float* S = out[0].GetTensorData<float>();
        const float* ctx = out[1].GetTensorData<float>(); // [tracks][T][ctxSize]
        const auto path = transcription::semiCrfDecode (S, T, tracks, startPos);

        std::vector<float> attr;
        for (int k = 0; k < tracks; ++k)
            for (const auto& [b, e] : path[(std::size_t) k])
            {
                const float* a = ctx + ((std::size_t) k * T + (std::size_t) b) * ctxSize;
                const float* z = ctx + ((std::size_t) k * T + (std::size_t) e) * ctxSize;
                attr.insert (attr.end(), a, a + ctxSize);
                attr.insert (attr.end(), z, z + ctxSize);
                for (int d = 0; d < ctxSize; ++d) attr.push_back (a[d] * z[d]);
            }
        const auto nIntervals = (int64_t) (attr.size() / (3 * ctxSize));

        std::vector<TkNote> notes;
        std::vector<int> lastP ((std::size_t) tracks, 0);
        if (nIntervals > 0)
        {
            auto attrIn = tensor (attr, { nIntervals, 3 * ctxSize });
            auto attrOut = heads->Run (Ort::RunOptions {}, headInNames, &attrIn, 1, headOutNames, 2);
            const float* vel = attrOut[0].GetTensorData<float>(); // [n][128]
            const float* of = attrOut[1].GetTensorData<float>();  // [n][4]: onset, offset values; onset, offset presence
            std::size_t n = 0;
            for (int k = 0; k < tracks; ++k)
            {
                double lastEnd = 0;
                for (const auto& [b, e] : path[(std::size_t) k])
                {
                    const float* v = vel + n * 128;
                    const float* o = of + n * 4;
                    auto offset = [] (float logit) { return std::clamp ((continuousBernoulliMean (logit) - 0.5) / 0.99, -0.5, 0.5); };
                    TkNote note;
                    note.symbol = symbol (k);
                    note.velocity = (int) (std::max_element (v, v + 128) - v);
                    note.start = std::max ((b + offset (o[0])) * frameSeconds, lastEnd);
                    note.end = std::max ((e + offset (o[1])) * frameSeconds, note.start + 1e-8);
                    note.hasOnset = b > 0 || o[2] > 0;
                    note.hasOffset = e < lastFrame || o[3] > 0;
                    lastEnd = note.end;
                    if (note.hasOffset) lastP[(std::size_t) k] = e;
                    notes.push_back (note);
                    ++n;
                }
            }
            std::stable_sort (notes.begin(), notes.end(), earlier);
        }
        for (int k = 0; k < tracks; ++k) startPos[(std::size_t) k] = std::max (lastP[(std::size_t) k] - stepFrames, 0);

        // Into song time; a note that carries on from the last segment extends it.
        const double beginTime = (double) from / rate - (segmentSeconds - stepSeconds);
        for (auto e : notes)
        {
            e.start = std::max (e.start + beginTime, 0.0);
            e.end = std::max (e.end + beginTime, e.start);
            auto& list = byType[e.symbol];
            if (! list.empty() && e.start < list.back().end)
            {
                if (e.hasOnset) list.back() = e;
                else
                {
                    list.back().hasOffset = e.hasOffset;
                    list.back().end = std::max (e.end, list.back().end);
                }
                continue;
            }
            if (e.hasOnset) list.push_back (e);
        }
        if (onProgress) onProgress ((double) (seg + 1) / nSegments);
    }

    std::vector<TkNote> all;
    for (auto& [symbolValue, list] : byType)
    {
        if (! list.empty()) list.back().hasOffset = true; // a note still held at the end of the song
        for (const auto& e : list)
            if (e.hasOffset) all.push_back (e);
    }
    Transcription result;
    for (const auto& e : resolveOverlapping (std::move (all)))
    {
        if (e.symbol > 0) result.notes.push_back ({ e.symbol, e.start, e.end, clampVelocity (e.velocity) });
        else result.pedals.push_back ({ -e.symbol, e.start, e.end, std::clamp (e.velocity, 0, 127) });
    }
    return result;
}
} // namespace juststem
