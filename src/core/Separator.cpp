#include "Separator.h"

#include <onnxruntime_cxx_api.h>
#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#else
 #include <dlfcn.h>
 #include <unistd.h>
 #if defined (__APPLE__)
  #include <sys/sysctl.h>
 #endif
#endif

#include <signalsmith-linear/fft.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <cstdio>
#include <cstdint>
#include <new>
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
#if defined (_WIN32)
std::string pathToUtf8 (const std::filesystem::path& p)
{
    const auto u8 = p.u8string();
    return { u8.begin(), u8.end() };
}
#endif

std::uint64_t totalSystemRamBytes() noexcept
{
#if defined (_WIN32)
    MEMORYSTATUSEX ms {};
    ms.dwLength = sizeof (ms);
    return GlobalMemoryStatusEx (&ms) ? (std::uint64_t) ms.ullTotalPhys : 0;
#elif defined (__APPLE__)
    std::uint64_t mem = 0;
    std::size_t len = sizeof (mem);
    return sysctlbyname ("hw.memsize", &mem, &len, nullptr, 0) == 0 ? mem : 0;
#else
    const long pages = sysconf (_SC_PHYS_PAGES), pageSize = sysconf (_SC_PAGESIZE);
    return pages > 0 && pageSize > 0 ? (std::uint64_t) pages * (std::uint64_t) pageSize : 0;
#endif
}

bool isLowMemorySystem() noexcept
{
    static const bool low = [] {
        const auto ram = totalSystemRamBytes();
        return ram > 0 && ram <= (9ull << 30); // 8 GB class machines and below
    }();
    return low;
}

bool isSystemMemoryCritical() noexcept
{
#if defined (_WIN32)
    MEMORYSTATUSEX ms {};
    ms.dwLength = sizeof (ms);
    // Under 1 GB of commit charge (RAM + pagefile) left: a multi-GB CPU fallback would starve the DAW.
    if (GlobalMemoryStatusEx (&ms))
        return ms.ullAvailPageFile < (1024ull << 20);
#endif
    return false;
}

bool isOutOfMemoryError (const std::string& msg)
{
    std::string lower = msg;
    for (char& c : lower) c = (char) std::tolower ((unsigned char) c);
    return lower.find ("allocate memory") != std::string::npos
        || lower.find ("out of memory") != std::string::npos
        || lower.find ("not enough memory") != std::string::npos
        || lower.find ("bad allocation") != std::string::npos
        || lower.find ("bad_alloc") != std::string::npos
        || lower.find ("e_outofmemory") != std::string::npos;
}

struct OrtRuntime
{
    std::unique_ptr<Ort::Env> env;
#if defined (_WIN32)
    using AppendDmlFn = OrtStatus* (ORT_API_CALL*) (OrtSessionOptions*, int);
    AppendDmlFn appendDml = nullptr;
#endif
};

/** Loads justStem's own bundled ONNX Runtime by full path and pins it, so another plug-in or DAW with
    an older onnxruntime in the same process can neither hijack our symbols nor make GetApi return null.
    Heap-allocated so process exit never tears down Ort::Env under a still-finishing abandoned job. */
const OrtRuntime& ortRuntime()
{
    static const OrtRuntime* rt = [] {
#if defined (_WIN32)
        HMODULE self = nullptr;
        if (! GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR> (&totalSystemRamBytes), &self))
            throw std::runtime_error ("Couldn't locate the justStem binary");
        std::wstring buf (MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD n = GetModuleFileNameW (self, buf.data(), (DWORD) buf.size());
            if (n == 0) throw std::runtime_error ("Couldn't read the justStem binary path");
            if (n < buf.size() - 1) { buf.resize (n); break; }
            buf.resize (buf.size() * 2);
        }
        const auto selfDir = std::filesystem::path (buf).parent_path();
        auto loadDll = [] (const std::filesystem::path& p) -> HMODULE {
            if (HMODULE h = LoadLibraryExW (p.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS))
                return h;
            return LoadLibraryExW (p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        };
        const auto dmlPath = selfDir / L"DirectML.dll";
        if (std::error_code ec; std::filesystem::exists (dmlPath, ec))
            if (loadDll (dmlPath) != nullptr)
            {
                HMODULE pinnedDml = nullptr;
                GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_PIN, dmlPath.c_str(), &pinnedDml);
            }
        const auto ortPath = selfDir / L"onnxruntime.dll";
        HMODULE mod = loadDll (ortPath);
        if (mod == nullptr)
            throw std::runtime_error ("Couldn't load ONNX Runtime (" + pathToUtf8 (ortPath) + ")");
        HMODULE pinned = nullptr;
        GetModuleHandleExW (GET_MODULE_HANDLE_EX_FLAG_PIN, ortPath.c_str(), &pinned);
        auto getBase = reinterpret_cast<const OrtApiBase* (ORT_API_CALL*) ()> (GetProcAddress (mod, "OrtGetApiBase"));
        auto appendDml = reinterpret_cast<OrtRuntime::AppendDmlFn> (GetProcAddress (mod, "OrtSessionOptionsAppendExecutionProvider_DML"));
#else
        Dl_info info {};
        if (! dladdr (reinterpret_cast<const void*> (&totalSystemRamBytes), &info) || info.dli_fname == nullptr)
            throw std::runtime_error ("Couldn't locate the justStem binary");
        const auto selfDir = std::filesystem::path (info.dli_fname).parent_path();
       #if defined (__APPLE__)
        const std::filesystem::path candidates[] {
            selfDir / "../Frameworks/libonnxruntime.1.dylib",
            selfDir / "libonnxruntime.1.dylib",
            selfDir / "../../../onnxruntime/libonnxruntime.1.dylib",
            selfDir / "../../onnxruntime/libonnxruntime.1.dylib",
        };
       #else
        const std::filesystem::path candidates[] {
            selfDir / "libonnxruntime.so.1",
            selfDir / "../../../onnxruntime/libonnxruntime.so.1",
            selfDir / "../../onnxruntime/libonnxruntime.so.1",
        };
       #endif
        void* mod = nullptr;
        std::string lastErr;
        for (const auto& c : candidates)
        {
            std::error_code ec;
            if (! std::filesystem::exists (c, ec)) continue;
            if ((mod = dlopen (c.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE)) != nullptr) break;
            if (const char* err = dlerror()) lastErr = err;
        }
        if (mod == nullptr)
            throw std::runtime_error ("Couldn't load ONNX Runtime" + (lastErr.empty() ? std::string() : ": " + lastErr));
        auto getBase = reinterpret_cast<const OrtApiBase* (*) ()> (dlsym (mod, "OrtGetApiBase"));
#endif
        if (getBase == nullptr)
            throw std::runtime_error ("ONNX Runtime is damaged (missing OrtGetApiBase)");
        const OrtApiBase* base = getBase();
        const OrtApi* api = base != nullptr ? base->GetApi (ORT_API_VERSION) : nullptr;
        if (api == nullptr)
            throw std::runtime_error (std::string ("Incompatible ONNX Runtime version")
                                      + (base != nullptr ? std::string (": ") + base->GetVersionString() : std::string()));
        Ort::InitApi (api);
        auto loaded = std::make_unique<OrtRuntime>();
        loaded->env = std::make_unique<Ort::Env> (ORT_LOGGING_LEVEL_ERROR, "justStem");
#if defined (_WIN32)
        loaded->appendDml = appendDml;
#endif
        return loaded.release();
    }();
    return *rt;
}
} // namespace

/** The one ONNX Runtime environment, shared with Transcriber.cpp. */
Ort::Env& ortEnv()
{
    return *ortRuntime().env;
}

void configureOrtSession (Ort::SessionOptions& so, bool cpuSession)
{
    so.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
    if (isLowMemorySystem())
    {
        so.SetIntraOpNumThreads (2);
        if (cpuSession)
        {
            so.DisableCpuMemArena();
            so.DisableMemPattern();
        }
    }
    else
    {
        so.SetIntraOpNumThreads (0); // ORT default: one per performance core
    }
}

namespace
{

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
    explicit Impl (const ModelSpec& s) : spec (s) {}

    std::string path, cacheDir;
    const ModelSpec& spec;
    std::unique_ptr<Ort::Session> session;
    std::unique_ptr<Stft> stft;
    std::vector<float> re, im, specAudio;
    Ort::Value waveOut { nullptr };

    void clearScratch()
    {
        waveOut = Ort::Value { nullptr };
        re = {};
        im = {};
        specAudio = {};
    }

    void createSession (Backend b)
    {
        auto& env = ortEnv(); // must exist before SessionOptions or an execution provider is created
        Ort::SessionOptions so;
        configureOrtSession (so, b == Backend::CPU);
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
            if (ortRuntime().appendDml == nullptr)
                throw std::runtime_error ("DirectML is not available in this ONNX Runtime build");
            // Required by the DirectML execution provider.
            so.DisableMemPattern();
            so.SetExecutionMode (ExecutionMode::ORT_SEQUENTIAL);
            Ort::ThrowOnError (ortRuntime().appendDml (so, 0)); // default GPU
        }
       #endif
        // ONNX Runtime takes native paths (wide strings on Windows); the model path is UTF-8.
        const std::filesystem::path modelPath (std::u8string (path.begin(), path.end()));
        session = std::make_unique<Ort::Session> (env, modelPath.c_str(), so);
    }

    /** planar [2][chunk] -> audio [stem][channel][chunk], valid until the next run() or clearScratch() */
    const float* run (std::vector<float>& planar)
    {
        return spec.input == ModelInput::Spectrogram ? runSpectrogram (planar) : runWaveform (planar);
    }

    const float* runWaveform (std::vector<float>& planar)
    {
        const int chunk = spec.chunkSamples;
        auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<std::int64_t, 3> shape { 1, 2, chunk };
        auto input = Ort::Value::CreateTensor<float> (mem, planar.data(), planar.size(), shape.data(), shape.size());
        const char* in[] { "mix" };
        const char* out[] { "stems" };
        auto res = session->Run (Ort::RunOptions { nullptr }, in, &input, 1, out, 1);
        waveOut = std::move (res[0]);
        return waveOut.GetTensorData<float>();
    }

    const float* runSpectrogram (std::vector<float>& planar)
    {
        const int chunk = spec.chunkSamples;
        if (stft == nullptr) stft = std::make_unique<Stft> (chunk);
        const auto per = stft->size(); // one channel's bins x frames
        re.resize (2 * per);
        im.resize (2 * per);
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

        specAudio.resize ((size_t) numStems * 2 * (size_t) chunk);
        for (std::size_t sc = 0; sc < (size_t) numStems * 2; ++sc) // stem x channel, in tensor order
            stft->inverse (outRe + sc * per, outIm + sc * per, specAudio.data() + sc * (size_t) chunk);
        return specAudio.data();
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
    : impl (std::make_unique<Impl> (model::forFile (modelPath)))
{
    impl->path = std::move (modelPath);
#if defined (__APPLE__)
    impl->cacheDir = cacheDir.empty() ? std::string() : coremlCacheFolder (impl->path, cacheDir);
#else
    (void) cacheDir; // only CoreML compiles and caches models
#endif
}

Separator::~Separator() = default;

const std::string& Separator::cacheFolder() const { return impl->cacheDir; }

StemMap Separator::separate (const StereoAudio& mix,
                             const std::function<void (const SeparationProgress&)>& onProgress,
                             const std::atomic<bool>& cancel)
{
    const auto& spec = impl->spec;
    const int N = (int) mix.size();
    const int chunk = spec.chunkSamples;
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
    const double var = sumSq / std::max (1, N) - mean * mean;
    const float inputScale = spec.normaliseInput && std::isfinite (var)
                                 ? (float) (1.0 / std::sqrt (var > 1e-12 ? var : 1e-12))
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
            std::vector<float> planar ((size_t) chunk * 2, 0.0f);
            const std::size_t outCount = (std::size_t) numStems * 2 * (std::size_t) chunk;
            const auto t0 = std::chrono::steady_clock::now();
            for (int seg = 0; seg < total; ++seg)
            {
                if (cancel.load()) throw std::runtime_error ("cancelled");
                const int start = seg * step;
                const int len = std::min (chunk, N - start);
                for (int i = 0; i < len; ++i)
                {
                    planar[(size_t) i] = mix.left[(size_t) (start + i)] * inputScale;
                    planar[(size_t) (chunk + i)] = mix.right[(size_t) (start + i)] * inputScale;
                }
                if (len < chunk)
                {
                    std::fill (planar.begin() + len, planar.begin() + chunk, 0.0f);
                    std::fill (planar.begin() + chunk + len, planar.end(), 0.0f);
                }
                const bool validate = ! checked && hasSignal (planar);
                const float* audio = impl->run (planar);
                if (validate)
                {
                    if (! looksValid (audio, outCount)) throw BadOutput();
                    checked = true;
                }

                for (int s = 0; s < numStems; ++s)
                {
                    auto& dst = out[(size_t) s];
                    const float* l = audio + (size_t) s * 2 * (size_t) chunk;
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
                result[spec.outputStems[(size_t) s]] = std::move (out[(size_t) s]);
            impl->clearScratch();
            impl->session.reset();
            impl->stft.reset();
            return result;
        }
        catch (const Ort::Exception& e)
        {
            impl->clearScratch();
            impl->session.reset();
            if (cancel.load()) throw std::runtime_error ("cancelled");
            if (isSystemMemoryCritical() || (last && isOutOfMemoryError (e.what())))
                throw std::runtime_error ("Not enough memory to split this song. Close other apps or try Fast mode.");
            if (last) throw std::runtime_error (std::string ("ONNX Runtime: ") + e.what());
        }
        catch (const std::bad_alloc&)
        {
            impl->clearScratch();
            impl->session.reset();
            throw std::runtime_error ("Not enough memory to split this song. Close other apps or try Fast mode.");
        }
        catch (const BadOutput&)
        {
            impl->clearScratch();
            impl->session.reset();
            if (last) throw;
        }
    }
    throw std::runtime_error ("no backend available");
}

float fitUnderFullScale (StemMap& stems)
{
    const float ceiling = std::pow (10.0f, -0.1f / 20.0f);
    std::vector<StereoAudio*> all;
    const StereoAudio* vocals = nullptr;
    std::size_t n = 0;
    for (auto& [stem, audio] : stems)
    {
        all.push_back (&audio);
        if (stem == Stem::Vocals) vocals = &audio;
        n = std::max (n, audio.size());
    }
    float peak = 0;
    for (int ch = 0; ch < 2; ++ch)
        for (std::size_t i = 0; i < n; ++i)
        {
            float sum = 0, voice = 0;
            for (const auto* a : all)
            {
                const auto& x = ch == 0 ? a->left : a->right;
                if (i >= x.size()) continue;
                sum += x[i];
                peak = std::max (peak, std::abs (x[i]));
                if (a == vocals) voice = x[i];
            }
            peak = std::max ({ peak, std::abs (sum), std::abs (sum - voice) });
        }
    if (peak <= ceiling) return 0.0f;
    const float gain = ceiling / peak;
    for (auto* a : all)
        for (auto* x : { &a->left, &a->right })
            for (auto& v : *x) v *= gain;
    return 20.0f * std::log10 (gain);
}
} // namespace juststem
