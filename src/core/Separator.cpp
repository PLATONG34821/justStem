#include "Separator.h"

#include <onnxruntime_cxx_api.h>
#if defined (_WIN32)
 #include <dml_provider_factory.h>
#endif

#include <chrono>
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

struct Separator::Impl
{
    std::string path, cacheDir;
    std::unique_ptr<Ort::Session> session;

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
        const int chunk = model::chunkSamples;
        auto mem = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
        const std::array<std::int64_t, 3> shape { 1, 2, chunk };
        auto input = Ort::Value::CreateTensor<float> (mem, planar.data(), planar.size(), shape.data(), shape.size());
        const char* in[] { "mix" };
        const char* out[] { "stems" };
        auto res = session->Run (Ort::RunOptions { nullptr }, in, &input, 1, out, 1);
        const float* p = res[0].GetTensorData<float>();
        return { p, p + (std::size_t) numStems * 2 * (std::size_t) chunk };
    }
};

Separator::Separator (std::string modelPath, std::string cacheDir) : impl (std::make_unique<Impl>())
{
    impl->path = std::move (modelPath);
    impl->cacheDir = std::move (cacheDir);
}

Separator::~Separator() = default;

StemMap Separator::separate (const StereoAudio& mix,
                             const std::function<void (const SeparationProgress&)>& onProgress,
                             const std::atomic<bool>& cancel)
{
    const int N = (int) mix.size();
    const int chunk = model::chunkSamples;
    const int ov = (int) std::floor ((float) chunk * overlap);
    const int step = chunk - ov;
    const int total = std::max (1, (int) std::ceil ((double) (N - ov) / step));

    // Demucs expects the mix normalised by its standard deviation (see demucs/separate.py).
    double sum = 0, sumSq = 0;
    for (int i = 0; i < N; ++i)
    {
        const double m = 0.5 * (mix.left[(size_t) i] + mix.right[(size_t) i]);
        sum += m;
        sumSq += m * m;
    }
    const double mean = sum / std::max (1, N);
    const float inputScale = (float) (1.0 / std::sqrt (std::max (sumSq / std::max (1, N) - mean * mean, 1e-12)));
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
                result[model::outputStems[(size_t) s]] = std::move (out[(size_t) s]);
            impl->session.reset();
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
