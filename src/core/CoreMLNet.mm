#include "CoreMLNet.h"

#import <CoreML/CoreML.h>

#include <cstring>
#include <stdexcept>

namespace juststem
{
namespace
{
[[noreturn]] void fail (const char* what, NSError* error)
{
    std::string message = what;
    if (error != nil) message += std::string (": ") + error.localizedDescription.UTF8String;
    throw std::runtime_error (message);
}

NSString* ns (const std::string& s) { return [NSString stringWithUTF8String:s.c_str()]; }

/** The model compiled in `cacheFolder`, compiling `package` the first time. */
NSURL* compiledModel (const std::string& package, const std::string& cacheFolder)
{
    auto* files = NSFileManager.defaultManager;
    NSURL* compiled = [NSURL fileURLWithPath:[ns (cacheFolder) stringByAppendingPathComponent:@"model.mlmodelc"] isDirectory:YES];
    if ([files fileExistsAtPath:compiled.path]) return compiled;

    NSError* error = nil;
    [files createDirectoryAtPath:ns (cacheFolder) withIntermediateDirectories:YES attributes:nil error:nil];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" // the async replacement is macOS 14+
    NSURL* built = [MLModel compileModelAtURL:[NSURL fileURLWithPath:ns (package) isDirectory:YES] error:&error];
#pragma clang diagnostic pop
    if (built == nil) fail ("Core ML couldn't compile the model", error);
    // Another plug-in (AU, VST3, standalone) may have compiled the same model meanwhile.
    if (! [files moveItemAtURL:built toURL:compiled error:&error] && ! [files fileExistsAtPath:compiled.path])
        fail ("Core ML couldn't save the compiled model", error);
    [files removeItemAtURL:built error:nil];
    return compiled;
}
} // namespace

struct CoreMLNet::Impl
{
    MLModel* model = nil;
};

CoreMLNet::CoreMLNet (const std::string& package, const std::string& cacheFolder) : impl (std::make_unique<Impl>())
{
    if (@available (macOS 13.0, *)) {} // the models are built for macOS 13 (ML programs, getBytesWithHandler)
    else throw std::runtime_error ("Core ML models need macOS 13 or later");
    @autoreleasepool
    {
        auto* config = [[MLModelConfiguration alloc] init];
        config.computeUnits = MLComputeUnitsAll;
        NSError* error = nil;
        impl->model = [MLModel modelWithContentsOfURL:compiledModel (package, cacheFolder) configuration:config error:&error];
        if (impl->model == nil) fail ("Core ML couldn't load the model", error);
    }
}

CoreMLNet::~CoreMLNet() = default;

void CoreMLNet::run (float* input, const std::vector<int64_t>& shape, const std::vector<std::string>& outputNames,
                     std::vector<std::vector<float>>& outputs)
{
    @autoreleasepool
    {
        NSMutableArray<NSNumber*>* dims = [NSMutableArray array];
        NSMutableArray<NSNumber*>* strides = [NSMutableArray array];
        int64_t stride = 1;
        for (auto it = shape.rbegin(); it != shape.rend(); ++it)
        {
            [dims insertObject:@(*it) atIndex:0];
            [strides insertObject:@(stride) atIndex:0];
            stride *= *it;
        }
        NSError* error = nil;
        auto* array = [[MLMultiArray alloc] initWithDataPointer:input shape:dims dataType:MLMultiArrayDataTypeFloat32
                                                        strides:strides deallocator:nil error:&error];
        if (array == nil) fail ("Core ML couldn't take the input", error);
        NSString* inputName = impl->model.modelDescription.inputDescriptionsByName.allKeys.firstObject;
        auto* features = [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{ inputName : array } error:&error];
        if (features == nil) fail ("Core ML couldn't take the input", error);
        id<MLFeatureProvider> result = [impl->model predictionFromFeatures:features error:&error];
        if (result == nil) fail ("Core ML couldn't run the model", error);

        if (@available (macOS 13.0, *)) // always: the constructor refuses older systems
        {
            outputs.resize (outputNames.size());
            for (size_t i = 0; i < outputNames.size(); ++i)
            {
                MLMultiArray* out = [result featureValueForName:ns (outputNames[i])].multiArrayValue;
                if (out == nil || out.dataType != MLMultiArrayDataTypeFloat32 || out.shape.count == 0)
                    throw std::runtime_error ("Core ML gave no float32 output " + outputNames[i]);
                const auto rank = (size_t) out.shape.count;
                std::vector<int64_t> n (rank), st (rank);
                size_t count = 1;
                for (size_t d = 0; d < rank; ++d)
                {
                    n[d] = out.shape[d].longLongValue;
                    st[d] = out.strides[d].longLongValue;
                    count *= (size_t) n[d];
                }
                outputs[i].resize (count);
                float* dst = outputs[i].data(); // a block copies what it captures: a pointer, not the vector
                // The GPU and Neural Engine can pad rows, so the output is copied row by row along its strides.
                [out getBytesWithHandler:^(const void* bytes, NSInteger) {
                    const auto* src = static_cast<const float*> (bytes);
                    const auto row = (size_t) n[rank - 1];
                    const bool packed = st[rank - 1] == 1;
                    std::vector<int64_t> at (rank, 0);
                    for (size_t done = 0; done < count; done += row)
                    {
                        int64_t offset = 0;
                        for (size_t d = 0; d + 1 < rank; ++d) offset += at[d] * st[d];
                        if (packed) std::memcpy (dst + done, src + offset, row * sizeof (float));
                        else
                            for (size_t j = 0; j < row; ++j) dst[done + j] = src[offset + (int64_t) j * st[rank - 1]];
                        for (size_t d = rank - 1; d-- > 0;) // next row: carry through the outer dimensions
                        {
                            if (++at[d] < n[d]) break;
                            at[d] = 0;
                        }
                    }
                }];
            }
        }
    }
}
} // namespace juststem
