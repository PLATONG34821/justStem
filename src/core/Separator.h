#pragma once

#include "Audio.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace juststem
{
/** What a separation model takes as input. */
enum class ModelInput
{
    Waveform,   ///< "mix" [1, 2, chunk] -> "stems" [1, 6, 2, chunk]
    Spectrogram ///< "spec_real"/"spec_imag" [1, 2, 1025, frames] -> "out_spec_real"/"out_spec_imag" [1, 6, 2, 1025, frames]
};

struct ModelSpec
{
    std::string_view fileName, name;
    ModelInput input;
    /** Chunk length fixed by the ONNX export. */
    int chunkSamples;
    /** Order of the stems in the model's output tensor. */
    std::array<Stem, numStems> outputStems;
    /** Divide the mix by its standard deviation before the model (and multiply the stems back). */
    bool normaliseInput;
};

namespace model
{
    /** Fast: HT-Demucs 6-stem, built by scripts/prepare_demucs_onnx.py. Always bundled. */
    inline constexpr ModelSpec fast { "htdemucs_6s_opt.onnx", "HT-Demucs", ModelInput::Waveform, 343'980,
                                      { Stem::Drums, Stem::Bass, Stem::Other, Stem::Vocals, Stem::Guitar, Stem::Piano },
                                      true };
    /** HQ: BS-RoFormer SW 6-stem, built by scripts/prepare_roformer_onnx.py. Bundled when present at
        build time. fp16 maths (fast on GPUs, ~1.7x slower on the CPU fallback).
        STFT: 2048-point, hop 512, periodic Hann window, centred (reflect padding). */
    inline constexpr ModelSpec hq { "bs_roformer_sw_6stem_opt.onnx", "BS-RoFormer", ModelInput::Spectrogram, 176'400,
                                    { Stem::Bass, Stem::Drums, Stem::Other, Stem::Vocals, Stem::Guitar, Stem::Piano },
                                    false };

    /** The spec whose file name matches the end of `path`; HT-Demucs if none does. */
    const ModelSpec& forFile (std::string_view path);
} // namespace model

/** Where the model runs. The GPU backend depends on the OS (see Separator.cpp). */
enum class Backend { CoreML, DirectML, CPU };
const char* backendName (Backend);

struct SeparationProgress
{
    int segment = 0, totalSegments = 0;
    double etaSeconds = 0.0;
    Backend backend = Backend::CPU;
};

/** Runs a separation model (see model::forFile) on a whole song, in crossfaded chunks.
    Uses the OS's GPU backend (CoreML on macOS, DirectML on Windows) and falls back to the CPU if
    that fails or returns bad output. Linux uses the CPU. */
class Separator
{
public:
    /** @param modelPath  UTF-8 path of the .onnx file.
        @param cacheDir   where CoreML may keep its compiled models (speeds up later loads). Each model
                          gets its own subfolder, see coremlCacheFolder(). macOS only: ignored elsewhere,
                          and empty for no cache. */
    Separator (std::string modelPath, std::string cacheDir);
    ~Separator();

    /** Blocking. Call from a background thread. Throws std::runtime_error on failure.
        Set `cancel` to stop early (throws std::runtime_error("cancelled")). */
    StemMap separate (const StereoAudio& mix,
                      const std::function<void (const SeparationProgress&)>& onProgress,
                      const std::atomic<bool>& cancel);

    /** The subfolder of `cacheDir` for this model file: named after the file, its size and a
        fingerprint of its contents, so the AU, VST3 and standalone copies of one model share it.
        ONNX Runtime keys its CoreML cache by model name, not contents, so a model replaced under the
        same name would otherwise load the old compiled model, fail, and run on the CPU. Creates the
        folder and deletes stale ones for the same file name. */
    static std::string coremlCacheFolder (const std::string& modelPath, const std::string& cacheDir);

    /** This model's CoreML cache folder (see coremlCacheFolder), or empty when there is none. */
    const std::string& cacheFolder() const;

    Backend backend() const { return currentBackend; }
    const ModelSpec& spec() const { return modelSpec; }

    /** Chunk overlap (0..0.5). */
    float overlap = 0.25f;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    Backend currentBackend = Backend::CPU;
    const ModelSpec& modelSpec;
};
} // namespace juststem
