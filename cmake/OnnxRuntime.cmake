# ONNX Runtime (C/C++ API) from Microsoft's official NuGet packages, pinned by version + SHA-256.
#
#   macOS    Microsoft.ML.OnnxRuntime 1.32.0 (arm64), which includes the CoreML execution provider
#   Windows  Microsoft.ML.OnnxRuntime.DirectML 1.24.4 (x64 / arm64) + DirectML.dll from
#            Microsoft.AI.DirectML 1.15.4: GPU acceleration on any DirectX 12 GPU
#   Linux    Microsoft.ML.OnnxRuntime 1.32.0 (x64 / arm64): CPU only
#
# Provides:
#   onnxruntime::onnxruntime   imported shared library target
#   ORT_RUNTIME_FILES          files to ship next to the binary that uses ONNX Runtime
#   ORT_LIB_DIR                the folder holding them (developer tools run from the build folder)

set(ORT_LIB_DIR ${CMAKE_BINARY_DIR}/onnxruntime)
file(MAKE_DIRECTORY ${ORT_LIB_DIR})

# Download and unpack a NuGet package; sets <out_var> to where it was unpacked.
function(juststem_nuget out_var name version sha256)
    string(TOLOWER ${name} lower)
    string(REPLACE "." "_" id ${lower})
    FetchContent_Declare(${id}
        URL https://api.nuget.org/v3-flatcontainer/${lower}/${version}/${lower}.${version}.nupkg
        URL_HASH SHA256=${sha256}
        DOWNLOAD_NAME ${id}-${version}.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(${id})
    set(${out_var} ${${id}_SOURCE_DIR} PARENT_SCOPE)
endfunction()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    set(ORT_ARCH arm64)
else()
    set(ORT_ARCH x64)
endif()

add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)

if(APPLE)
    juststem_nuget(pkg Microsoft.ML.OnnxRuntime 1.32.0 a9150bc2992cbbb6722070da9910e1ffa989a8bfc67bdd5bb8e745ca755641db)
    # The package ships libonnxruntime.dylib, but its install name is @rpath/libonnxruntime.1.dylib.
    set(lib ${ORT_LIB_DIR}/libonnxruntime.1.dylib)
    file(COPY_FILE ${pkg}/runtimes/osx-arm64/native/libonnxruntime.dylib ${lib} ONLY_IF_DIFFERENT)
    set_target_properties(onnxruntime::onnxruntime PROPERTIES
        IMPORTED_LOCATION ${lib}
        IMPORTED_SONAME @rpath/libonnxruntime.1.dylib)
    set(ORT_RUNTIME_FILES ${lib})

elseif(WIN32)
    juststem_nuget(pkg Microsoft.ML.OnnxRuntime.DirectML 1.24.4 57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681)
    juststem_nuget(dml Microsoft.AI.DirectML 1.15.4 4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9)
    set(native ${pkg}/runtimes/win-${ORT_ARCH}/native)
    file(COPY_FILE ${native}/onnxruntime.dll ${ORT_LIB_DIR}/onnxruntime.dll ONLY_IF_DIFFERENT)
    file(COPY_FILE ${dml}/bin/${ORT_ARCH}-win/DirectML.dll ${ORT_LIB_DIR}/DirectML.dll ONLY_IF_DIFFERENT)
    set_target_properties(onnxruntime::onnxruntime PROPERTIES
        IMPORTED_LOCATION ${ORT_LIB_DIR}/onnxruntime.dll
        IMPORTED_IMPLIB ${native}/onnxruntime.lib)
    set(ORT_RUNTIME_FILES ${ORT_LIB_DIR}/onnxruntime.dll ${ORT_LIB_DIR}/DirectML.dll)
    set(extra_includes ${dml}/include) # DirectML.h, needed by dml_provider_factory.h

else() # Linux
    juststem_nuget(pkg Microsoft.ML.OnnxRuntime 1.32.0 a9150bc2992cbbb6722070da9910e1ffa989a8bfc67bdd5bb8e745ca755641db)
    # The package ships libonnxruntime.so, but its soname is libonnxruntime.so.1.
    set(lib ${ORT_LIB_DIR}/libonnxruntime.so.1)
    file(COPY_FILE ${pkg}/runtimes/linux-${ORT_ARCH}/native/libonnxruntime.so ${lib} ONLY_IF_DIFFERENT)
    set_target_properties(onnxruntime::onnxruntime PROPERTIES
        IMPORTED_LOCATION ${lib}
        IMPORTED_SONAME libonnxruntime.so.1)
    set(ORT_RUNTIME_FILES ${lib})
endif()

set_target_properties(onnxruntime::onnxruntime PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${pkg}/build/native/include;${extra_includes}")
