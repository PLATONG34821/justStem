cmake_minimum_required(VERSION 3.28)
project(CoverTools VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
    # Apple Silicon only: Microsoft ships ONNX Runtime for macOS arm64 only.
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architecture")
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

include(cmake/OnnxRuntime.cmake)
include(cmake/Packaging.cmake)

# FAAD2: AAC decoder for M4A files (GPL-2.0-or-later), built from source on every OS.
set(FAAD_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
FetchContent_Declare(faad2
    URL https://github.com/knik0/faad2/archive/refs/tags/2.11.4.tar.gz
    URL_HASH SHA256=ee479ccbae4a8387ab696e6f21a481bd83fe3881471cafa81b4ae59d7d3aed43
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(faad2)

# Signalsmith Stretch (MIT): pitch shifting for Transpose. Header-only; Linear (MIT) is its FFT layer.
FetchContent_Declare(signalsmith-linear
    URL https://github.com/Signalsmith-Audio/linear/archive/refs/tags/0.6.4.tar.gz
    URL_HASH SHA256=2cb10d84b96c626255ab46c4aa4c80f64ae532be487063cff6a08903c1445a56
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
FetchContent_Declare(signalsmith-stretch
    URL https://github.com/Signalsmith-Audio/signalsmith-stretch/archive/refs/tags/1.4.0.tar.gz
    URL_HASH SHA256=077235709ecf2a358545e3ca7bdf32859f70f5c9afb4493e078bd753c9aff284
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM)
FetchContent_MakeAvailable(signalsmith-linear signalsmith-stretch)

# ---------------------------------------------------------------- core: separation + analysis (no JUCE)
add_library(covertools_core STATIC
    src/core/Separator.cpp
    src/core/Analysis.cpp
    src/core/Chords.cpp
    src/core/Dsp.cpp
    src/core/PitchShift.cpp
    src/core/Resampler.cpp)
target_include_directories(covertools_core PUBLIC src/core)
target_link_libraries(covertools_core PUBLIC onnxruntime::onnxruntime PRIVATE signalsmith-stretch)
if(MSVC)
    target_compile_options(covertools_core PRIVATE /W4 /utf-8 /bigobj) # bigobj: Signalsmith templates
else()
    target_compile_options(covertools_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------- JUCE layer
set(COVERTOOLS_IO_SOURCES
    src/io/AudioFiles.cpp
    src/io/M4aDecoder.cpp
    src/io/Storage.cpp
    src/io/ToolInstaller.cpp
    src/io/YouTube.cpp)

set(COVERTOOLS_PLUGIN_SOURCES
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/SongJob.cpp
    src/plugin/SongResult.cpp
    src/plugin/StemPlayer.cpp
    src/plugin/ui/ChordView.cpp
    src/plugin/ui/StoragePanel.cpp
    src/plugin/ui/Theme.cpp
    src/plugin/ui/Widgets.cpp)

# Inter (SIL Open Font License), embedded so the UI looks the same on every OS.
juce_add_binary_data(covertools_fonts SOURCES
    resources/fonts/Inter-ExtraLight.ttf
    resources/fonts/Inter-Light.ttf
    resources/fonts/Inter-Regular.ttf
    resources/fonts/Inter-SemiBold.ttf)

# Settings shared by every JUCE target. The plug-in needs them PUBLIC (JUCE's format wrappers
# must see the same configuration); the developer tools use PRIVATE.
function(covertools_juce_settings target scope)
    target_compile_definitions(${target} ${scope} JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_USE_MP3AUDIOFORMAT=1)
    target_link_libraries(${target} PRIVATE
        covertools_core
        juce::juce_audio_utils
        juce::juce_cryptography
        faad)
endfunction()

# ---------------------------------------------------------------- the plug-in
set(COVERTOOLS_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND COVERTOOLS_FORMATS AU)
endif()

juce_add_plugin(CoverTools
    COMPANY_NAME "CoverTools"
    BUNDLE_ID "com.covertools.covertools"
    PLUGIN_MANUFACTURER_CODE Cvtl
    PLUGIN_CODE Ctst
    FORMATS ${COVERTOOLS_FORMATS}
    PRODUCT_NAME "CoverTools"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE  # covertools_package installs instead
    VST3_CATEGORIES Fx Tools
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(CoverTools PRIVATE ${COVERTOOLS_IO_SOURCES} ${COVERTOOLS_PLUGIN_SOURCES})
covertools_juce_settings(CoverTools PUBLIC)
target_compile_definitions(CoverTools PUBLIC JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(CoverTools PRIVATE covertools_fonts)
target_link_libraries(CoverTools PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Install for the current user where DAWs look. Windows' VST3 folder (Program Files\Common Files\VST3)
# needs admin rights, so there the bundle is left in the build folder to copy by hand.
if(APPLE)
    covertools_package(CoverTools_VST3 INSTALL_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
    covertools_package(CoverTools_AU INSTALL_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
elseif(WIN32)
    covertools_package(CoverTools_VST3)
else()
    covertools_package(CoverTools_VST3 INSTALL_DIR "$ENV{HOME}/.vst3")
endif()
covertools_package(CoverTools_Standalone)

# ---------------------------------------------------------------- developer tools (not shipped)
# covertools_cli: separate / analyse / import from the command line, plus self-tests.
juce_add_console_app(covertools_cli PRODUCT_NAME "covertools_cli")
target_sources(covertools_cli PRIVATE tools/cli.cpp ${COVERTOOLS_IO_SOURCES})
covertools_juce_settings(covertools_cli PRIVATE)
target_link_libraries(covertools_cli PRIVATE juce::juce_recommended_config_flags)
covertools_copy_runtime(covertools_cli)

# covertools_uitest: runs the real plug-in code without a DAW (screenshots, audio, save/reload).
juce_add_console_app(covertools_uitest PRODUCT_NAME "covertools_uitest")
target_sources(covertools_uitest PRIVATE tools/uitest.cpp ${COVERTOOLS_IO_SOURCES} ${COVERTOOLS_PLUGIN_SOURCES})
covertools_juce_settings(covertools_uitest PRIVATE)
target_compile_definitions(covertools_uitest PRIVATE
    JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Name="CoverTools" JucePlugin_Build_Standalone=0)
target_link_libraries(covertools_uitest PRIVATE covertools_fonts juce::juce_recommended_config_flags)
covertools_copy_runtime(covertools_uitest)
