#pragma once

#include "Audio.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace juststem
{
/** The bundled model: HT-Demucs 6-stem, built by scripts/prepare_demucs_onnx.py. */
namespace model
{
    constexpr std::string_view fileName = "htdemucs_6s_opt.onnx";
    /** Order of the stems in the model's output tensor. */
    constexpr std::array<Stem, numStems> outputStems { Stem::Drums, Stem::Bass, Stem::Other,
                                                       Stem::Vocals, Stem::Guitar, Stem::Piano };
    /** Chunk length fixed by the ONNX trace (7.8 s). */
    constexpr int chunkSamples = 343'980;
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

/** Runs HT-Demucs on a whole song, in crossfaded chunks.
    Uses the OS's GPU backend (CoreML on macOS, DirectML on Windows) and falls back to the CPU if
    that fails or returns bad output. Linux uses the CPU. */
class Separator
{
public:
    /** @param modelPath  UTF-8 path of the .onnx file.
        @param cacheDir   where CoreML may keep its compiled model (macOS; speeds up later loads). */
    Separator (std::string modelPath, std::string cacheDir);
    ~Separator();

    /** Blocking. Call from a background thread. Throws std::runtime_error on failure.
        Set `cancel` to stop early (throws std::runtime_error("cancelled")). */
    StemMap separate (const StereoAudio& mix,
                      const std::function<void (const SeparationProgress&)>& onProgress,
                      const std::atomic<bool>& cancel);

    Backend backend() const { return currentBackend; }

    /** Chunk overlap (0..0.5). */
    float overlap = 0.25f;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    Backend currentBackend = Backend::CPU;
};
} // namespace juststem
