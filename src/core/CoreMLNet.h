#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace juststem
{
/** A Core ML model (macOS): one float32 input, float32 outputs, on the Neural Engine, GPU and CPU
    as Core ML sees fit. ONNX Runtime's CoreML provider runs some models far slower than Core ML
    itself does (see scripts/prepare_transkun_coreml.py). Throws std::runtime_error. */
class CoreMLNet
{
public:
    /** `package` is an .mlpackage folder. It is compiled once into `cacheFolder`, which is created. */
    CoreMLNet (const std::string& package, const std::string& cacheFolder);
    ~CoreMLNet();

    /** Runs `input` (with `shape`), filling `outputs[i]` (contiguous) with the output named `outputNames[i]`. */
    void run (float* input, const std::vector<int64_t>& shape, const std::vector<std::string>& outputNames,
              std::vector<std::vector<float>>& outputs);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace juststem
