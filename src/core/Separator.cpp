#include "Separator.h"

#include <onnxruntime_cxx_api.h>
#if defined (_WIN32)
 #include <dml_provider_factory.h>
#endif

#include <signalsmith-linear/fft.h>

#include <chrono>
#include <fstream>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <complex>
#include <numbers>
#include <filesystem>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace juststem
{
const char* backendName (Backend b)
{
    switch (b)
    {
        case Backend::CoreML: return "CoreML";
        case Backend::DirectML: return "DirectML";
        case Backend::CPU: return "CPU";
    }
    return "?";
}

namespace
{
Ort::Env& ortEnv()
{
    static Ort::Env env (ORT_LOGGING_LEVEL_ERROR, "justStem");
    return env;
}

struct BadOutput : std::runtime_error
{
    BadOutput() : std::runtime_error ("model produced invalid output on this backend") {}
};

bool looksValid (const float* data, std::size_t n)
{
    double energy = 0.0;
    for (std::size_t i = 0; i < n; i += 7)
    {
        if (! std::isfinite (data[i])) return false;
        energy += (double) data[i] * data[i];
    }
    return energy > 1e-12;
}

bool hasSignal (const std::vector<float>& planar)
{
    for (std::size_t i = 0; i < planar.size(); i += 13)
        if (std::fabs (planar[i]) > 1e-4f) return true;
    return false;
}

// GPU first, CPU as the fallback. Measured on an M4 (per 7.8 s chunk): CoreML 459 ms, CPU 925 ms.
#if defined (__APPLE__)
constexpr Backend backendOrder[] { Backend::CoreML, Backend::CPU };
#elif defined (_WIN32)
constexpr Backend backendOrder[] { Backend::DirectML, Backend::CPU };
#else
constexpr Backend backendOrder[] { Backend::CPU };
#endif
} // namespace

/** The STFT the spectrogram models were trained with (torch.stft defaults): 2048-point, hop 512,
    periodic Hann window, centred with reflect padding, unnormalised. Bins are [channel][bin][frame]. */
class Stft
{
public:
    static constexpr int nfft = 2048, hop = 512, bins = nfft / 2 + 1, pad = nfft / 2;

    explicit Stft (int chunk) : chunk (chunk), frames (1 + chunk / hop), fft (nfft), window ((size_t) nfft),
                                frame ((size_t) nfft), spectrum ((size_t) nfft / 2), windowSum ((size_t) (chunk + nfft), 0.0f)
    {
        // Not dsp::hann: that rounds once in double, and its window (at most 1 float ulp away from this
        // one) moved the HQ selftest's SI-SDR by up to 0.8 dB. The fp16 model is that sensitive.
        for (int i = 0; i < nfft; ++i)
            window[(size_t) i] = 0.5f - 0.5f * (float) std::cos (2.0 * std::numbers::pi * i / nfft);
        for (int f = 0; f < frames; ++f)
            for (int i = 0; i < nfft; ++i)
                windowSum[(size_t) (f * hop + i)] += window[(size_t) i] * window[(size_t) i];
    }

    int numFrames() const { return frames; }
    std::size_t size() const { return (size_t) bins * (size_t) frames; } // per channel

    /** x[chunk] -> re/im[bins][frames] */
    void forward (const float* x, float* re, float* im)
    {
        for (int f = 0; f < frames; ++f)
        {
            for (int i = 0; i < nfft; ++i)
                frame[(size_t) i] = sample (x, f * hop + i - pad) * window[(size_t) i];
            fft.fft (frame.data(), spectrum.data());
            re[f] = spectrum[0].real(); // DC
            im[f] = 0.0f;
            re[(size_t) (bins - 1) * (size_t) frames + (size_t) f] = spectrum[0].imag(); // Nyquist, packed into bin 0
            im[(size_t) (bins - 1) * (size_t) frames + (size_t) f] = 0.0f;
            for (int k = 1; k < bins - 1; ++k)
            {
                re[(size_t) k * (size_t) frames + (size_t) f] = spectrum[(size_t) k].real();
                im[(size_t) k * (size_t) frames + (size_t) f] = spectrum[(size_t) k].imag();
            }
        }
    }

    /** re/im[bins][frames] -> y[chunk] (overwritten) */
    void inverse (const float* re, const float* im, float* y)
    {
        acc.assign ((size_t) (chunk + nfft), 0.0f);
        for (int f = 0; f < frames; ++f)
        {
            spectrum[0] = { re[f], re[(size_t) (bins - 1) * (size_t) frames + (size_t) f] };
            for (int k = 1; k < bins - 1; ++k)
                spectrum[(size_t) k] = { re[(size_t) k * (size_t) frames + (size_t) f], im[(size_t) k * (size_t) frames + (size_t) f] };
            fft.ifft (spectrum.data(), frame.data());
            for (int i = 0; i < nfft; ++i)
                acc[(size_t) (f * hop + i)] += frame[(size_t) i] * window[(size_t) i];
        }
        for (int i = 0; i < chunk; ++i)
        {
            const auto j = (size_t) (i + pad);
            y[i] = acc[j] / ((float) nfft * std::max (windowSum[j], 1e-8f));
        }
    }

private:
    /** x with reflect padding at both ends (torch.stft center=True). */
    float sample (const float* x, int i) const
    {
        if (i < 0) i = -i;
        if (i >= chunk) i = 2 * (chunk - 1) - i;
        return x[i];
    }

    int chunk, frames;
    signalsmith::linear::RealFFT<float> fft;
    std::vector<float> window, frame;
    std::vector<std::complex<float>> spectrum;
    std::vector<float> windowSum, acc;
};

struct Separator::Impl
{
    std::string path, cacheDir;
    const ModelSpec* spec = nullptr;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Stft> stft;

    void createSession (Backend b)
    {
        auto& env = ortEnv(); // must exist before an execution provider is appended
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads (0); // ORT default: one per performance core
        so.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (b == Backend::CoreML)
        {
            // CPUAndGPU, not ALL: leaving out the Neural Engine loads a cached model in ~1 s instead
            // of ~50 s and runs ~3x faster (M4, HT-Demucs 2.0 s vs 5.7 s for 12 s of audio).
            std::unordered_map<std::string, std::string> opts { { "ModelFormat", "MLProgram" },
                                                                { "MLComputeUnits", "CPUAndGPU" } };
            if (! cacheDir.empty()) opts["ModelCacheDirectory"] = cacheDir;
            so.AppendExecutionProvider ("CoreML", opts);
        }
       #if defined (_WIN32)
        if (b == Backend::DirectML)
        {
            // Required by the DirectML execution provider.
            so.DisableMemPattern();
            so.SetExecutionMode (ExecutionMode::ORT_SEQUENTIAL);
            Ort::ThrowOnError (OrtSessionOptionsAppendExecutionProvider_DML (so, 0)); // default GPU
        }
       #endif
        // ONNX Runtime takes native paths (wide strings on Windows); the model path is UTF-8.
        const std::filesystem::path modelPath (std::u8string (path.begin(), path.end()));
        session = std::make_unique<Ort::Session> (env, modelPath.c_str(), so);
    }

    /** planar [2][chunk] -> audio [stem][channel][chunk] */
    std::vector<float> run (std::vector<float>& planar)
    {
        return spec->input == ModelInput::Spectrogram ? runSpectrogram (planar) : runWaveform (planar);
    }

    std::vector<float> runWaveform (std::vector<float>& planar)
    {
        const int chunk = spec->chunkSamples;
        auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<std::int64_t, 3> shape { 1, 2, chunk };
        auto input = Ort::Value::CreateTensor<float> (mem, planar.data(), planar.size(), shape.data(), shape.size());
        const char* in[] { "mix" };
        const char* out[] { "stems" };
        auto res = session->Run (Ort::RunOptions { nullptr }, in, &input, 1, out, 1);
        const float* p = res[0].GetTensorData<float>();
        return { p, p + (std::size_t) numStems * 2 * (std::size_t) chunk };
    }

    std::vector<float> runSpectrogram (std::vector<float>& planar)
    {
        const int chunk = spec->chunkSamples;
        if (stft == nullptr) stft = std::make_unique<Stft> (chunk);
        const auto per = stft->size(); // one channel's bins x frames
        std::vector<float> re (2 * per), im (2 * per);
        for (int c = 0; c < 2; ++c)
            stft->forward (planar.data() + (size_t) c * (size_t) chunk, re.data() + (size_t) c * per, im.data() + (size_t) c * per);

        auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<std::int64_t, 4> shape { 1, 2, Stft::bins, stft->numFrames() };
        std::array<Ort::Value, 2> inputs { Ort::Value::CreateTensor<float> (mem, re.data(), re.size(), shape.data(), shape.size()),
                                           Ort::Value::CreateTensor<float> (mem, im.data(), im.size(), shape.data(), shape.size()) };
        const char* in[] { "spec_real", "spec_imag" };
        const char* out[] { "out_spec_real", "out_spec_imag" };
        auto res = session->Run (Ort::RunOptions { nullptr }, in, inputs.data(), 2, out, 2);
        const float* outRe = res[0].GetTensorData<float>();
        const float* outIm = res[1].GetTensorData<float>();

        std::vector<float> audio ((size_t) numStems * 2 * (size_t) chunk);
        for (std::size_t sc = 0; sc < (size_t) numStems * 2; ++sc) // stem x channel, in tensor order
            stft->inverse (outRe + sc * per, outIm + sc * per, audio.data() + sc * (size_t) chunk);
        return audio;
    }
};

namespace model
{
const ModelSpec& forFile (std::string_view path)
{
    for (const auto* s : { &hq, &fast })
        if (path.size() >= s->fileName.size() && path.substr (path.size() - s->fileName.size()) == s->fileName)
            return *s;
    return fast;
}
} // namespace model

std::string Separator::coremlCacheFolder (const std::string& modelPath, const std::string& cacheDir)
{
    namespace fs = std::filesystem;
    const fs::path model (std::u8string (modelPath.begin(), modelPath.end()));
    const fs::path base (std::u8string (cacheDir.begin(), cacheDir.end()));
    std::error_code ec;
    const auto size = fs::file_size (model, ec);
    // A fingerprint of the contents, not the modification time: the AU, VST3 and standalone each bundle
    // their own copy of the model, and those copies must share one compiled model. Hashing 32 spread-out
    // 64 KB blocks reads 2 MB, which takes milliseconds.
    std::uint64_t hash = 1469598103934665603ull; // FNV-1a
    if (std::ifstream in { model, std::ios::binary }; in && size > 0)
    {
        std::vector<char> block (1 << 16);
        constexpr int blocks = 32;
        for (int i = 0; i < blocks; ++i)
        {
            const auto at = (std::uintmax_t) ((double) (size > block.size() ? size - block.size() : 0) * i / (blocks - 1));
            in.clear();
            in.seekg ((std::streamoff) at);
            in.read (block.data(), (std::streamsize) block.size());
            for (std::streamsize k = 0; k < in.gcount(); ++k)
                hash = (hash ^ (unsigned char) block[(size_t) k]) * 1099511628211ull;
        }
    }
    const auto stem = model.stem().u8string();
    char hex[17];
    std::snprintf (hex, sizeof hex, "%016llx", (unsigned long long) hash);
    const auto tag = "-" + std::to_string ((unsigned long long) size) + "-" + hex;
    const auto name = stem + std::u8string (tag.begin(), tag.end());

    // Older compiles of the same model (and the pre-versioned folder named just after it).
    for (const auto& entry : fs::directory_iterator (base, ec))
    {
        const auto other = entry.path().filename().u8string();
        if (other != name && (other == stem || other.starts_with (stem + u8"-")))
            fs::remove_all (entry.path(), ec);
    }
    const auto folder = base / name;
    fs::create_directories (folder, ec);
    const auto u8 = folder.u8string();
    return { u8.begin(), u8.end() };
}

Separator::Separator (std::string modelPath, std::string cacheDir)
    : impl (std::make_unique<Impl>()), modelSpec (model::forFile (modelPath))
{
    impl->path = std::move (modelPath);
#if defined (__APPLE__)
    impl->cacheDir = cacheDir.empty() ? std::string() : coremlCacheFolder (impl->path, cacheDir);
#else
    (void) cacheDir; // only CoreML compiles and caches models
#endif
    impl->spec = &modelSpec;
}

Separator::~Separator() = default;

const std::string& Separator::cacheFolder() const { return impl->cacheDir; }

StemMap Separator::separate (const StereoAudio& mix,
                             const std::function<void (const SeparationProgress&)>& onProgress,
                             const std::atomic<bool>& cancel)
{
    const int N = (int) mix.size();
    const int chunk = modelSpec.chunkSamples;
    const int ov = (int) std::floor ((float) chunk * overlap);
    const int step = chunk - ov;
    const int total = std::max (1, (int) std::ceil ((double) (N - ov) / step));

    // HT-Demucs expects the mix normalised by its standard deviation (see demucs/separate.py).
    double sum = 0, sumSq = 0;
    for (int i = 0; i < N; ++i)
    {
        const double m = 0.5 * (mix.left[(size_t) i] + mix.right[(size_t) i]);
        sum += m;
        sumSq += m * m;
    }
    const double mean = sum / std::max (1, N);
    const float inputScale = modelSpec.normaliseInput
                                 ? (float) (1.0 / std::sqrt (std::max (sumSq / std::max (1, N) - mean * mean, 1e-12)))
                                 : 1.0f;
    const float outScale = 1.0f / inputScale;

    std::vector<float> fadeIn ((size_t) ov);
    for (int i = 0; i < ov; ++i) fadeIn[(size_t) i] = (float) i / (float) ov;

    for (auto backend : backendOrder)
    {
        const bool last = backend == backendOrder[std::size (backendOrder) - 1];
        try
        {
            currentBackend = backend;
            impl->createSession (backend);

            std::vector<StereoAudio> out ((size_t) numStems);
            for (auto& s : out) { s.left.assign ((size_t) N, 0.0f); s.right.assign ((size_t) N, 0.0f); }

            bool checked = false;
            const auto t0 = std::chrono::steady_clock::now();
            for (int seg = 0; seg < total; ++seg)
            {
                if (cancel.load()) throw std::runtime_error ("cancelled");
                const int start = seg * step;
                const int len = std::min (chunk, N - start);
                std::vector<float> planar ((size_t) chunk * 2, 0.0f);
                for (int i = 0; i < len; ++i)
                {
                    planar[(size_t) i] = mix.left[(size_t) (start + i)] * inputScale;
                    planar[(size_t) (chunk + i)] = mix.right[(size_t) (start + i)] * inputScale;
                }
                const bool validate = ! checked && hasSignal (planar);
                auto audio = impl->run (planar);
                if (validate)
                {
                    if (! looksValid (audio.data(), audio.size())) throw BadOutput();
                    checked = true;
                }

                for (int s = 0; s < numStems; ++s)
                {
                    auto& dst = out[(size_t) s];
                    const float* l = audio.data() + (size_t) s * 2 * (size_t) chunk;
                    const float* r = l + chunk;
                    for (int i = 0; i < len; ++i)
                    {
                        float w = outScale;
                        if (seg > 0 && i < ov) w *= fadeIn[(size_t) i];
                        else if (seg < total - 1 && i >= step) w *= 1.0f - fadeIn[(size_t) (i - step)];
                        dst.left[(size_t) (start + i)] += l[i] * w;
                        dst.right[(size_t) (start + i)] += r[i] * w;
                    }
                }

                const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
                if (onProgress)
                    onProgress ({ seg + 1, total, elapsed / (seg + 1) * (total - seg - 1), backend });
            }

            StemMap result;
            for (int s = 0; s < numStems; ++s)
                result[modelSpec.outputStems[(size_t) s]] = std::move (out[(size_t) s]);
            impl->session.reset();
            impl->stft.reset();
            return result;
        }
        catch (const Ort::Exception& e)
        {
            impl->session.reset();
            if (last) throw std::runtime_error (std::string ("ONNX Runtime: ") + e.what());
        }
        catch (const BadOutput&)
        {
            impl->session.reset();
            if (last) throw;
        }
    }
    throw std::runtime_error ("no backend available");
}
} // namespace juststem
