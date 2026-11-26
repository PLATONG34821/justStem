cmake_minimum_required(VERSION 3.28)
# Objective-C is only needed on Apple (JUCE's macOS sources). Enabling it elsewhere makes CMake probe
# clang's Objective-C defaults, which fails with MSVC's bundled Clang ("CMAKE_OBJC_STANDARD_COMPUTED_DEFAULT
# should be set for Clang").
set(JUSTSTEM_LANGUAGES C CXX)
if(APPLE)
    list(APPEND JUSTSTEM_LANGUAGES OBJC OBJCXX)
endif()
project(justStem VERSION 0.0.7 LANGUAGES ${JUSTSTEM_LANGUAGES})

# windows.h defines min/max macros, which break std::min/std::max calls throughout the code.
if(WIN32)
    add_compile_definitions(NOMINMAX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static libraries (juststem_core, faad) are linked into the shared plug-in on Linux; they need -fPIC.
# Likely cause of the Linux CI failure "final link failed: bad value" (not reproduced locally; see CI).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version")
    # Apple Silicon only: Microsoft ships ONNX Runtime for macOS arm64 only.
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architecture")
endif()

include(FetchContent)
# JUCE 8.0.15 (commit 91ad83ae34a81e0833b1a2b0866f54846370ae53), checked like the other downloads:
# a git tag can be moved, the hash can't.
FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.15.tar.gz
    URL_HASH SHA256=0eafd736286c6e7bb6958d79fce18d42cb654b8e231890d327b7cece379bd156
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
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
add_library(juststem_core STATIC
    src/core/Separator.cpp
    src/core/Analysis.cpp
    src/core/Chords.cpp
    src/core/Dsp.cpp
    src/core/PitchShift.cpp
    src/core/Resampler.cpp)
target_include_directories(juststem_core PUBLIC src/core)
target_link_libraries(juststem_core PUBLIC onnxruntime::onnxruntime PRIVATE signalsmith-stretch signalsmith-linear)
if(MSVC)
    target_compile_options(juststem_core PRIVATE /W4 /utf-8 /bigobj) # bigobj: Signalsmith templates
else()
    target_compile_options(juststem_core PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------- JUCE layer
set(JUSTSTEM_IO_SOURCES
    src/io/AudioFiles.cpp
    src/io/M4aDecoder.cpp
    src/io/Storage.cpp
    src/io/ToolInstaller.cpp
    src/io/UpdateCheck.cpp
    src/io/YouTube.cpp)

set(JUSTSTEM_PLUGIN_SOURCES
    src/plugin/PluginProcessor.cpp
    src/plugin/PluginEditor.cpp
    src/plugin/SongJob.cpp
    src/plugin/SongResult.cpp
    src/plugin/StemPlayer.cpp
    src/plugin/ui/ChordView.cpp
    src/plugin/ui/PlayerBar.cpp
    src/plugin/ui/SaveStems.cpp
    src/plugin/ui/SongTitle.cpp
    src/plugin/ui/StoragePanel.cpp
    src/plugin/ui/Theme.cpp
    src/plugin/ui/UpdatePopup.cpp
    src/plugin/ui/Widgets.cpp)

# Inter (SIL Open Font License), embedded so the UI looks the same on every OS.
juce_add_binary_data(juststem_fonts SOURCES
    resources/fonts/Inter-ExtraLight.ttf
    resources/fonts/Inter-Light.ttf
    resources/fonts/Inter-Regular.ttf
    resources/fonts/Inter-SemiBold.ttf)

# Settings shared by every JUCE target. The plug-in needs them PUBLIC (JUCE's format wrappers
# must see the same configuration); the developer tools use PRIVATE. After `scope`: the JUCE modules
# the target needs besides juce_cryptography (the src/io layer's), e.g. juce::juce_audio_utils for a UI.
function(juststem_juce_settings target scope)
    # JUCE_USE_CURL is set by JUCE from each target's NEEDS_CURL (Linux only).
    target_compile_definitions(${target} ${scope} JUCE_WEB_BROWSER=0 JUCE_USE_MP3AUDIOFORMAT=1
        JUSTSTEM_VERSION="${PROJECT_VERSION}") # for the update check
    target_link_libraries(${target} PRIVATE
        juststem_core
        juce::juce_cryptography
        ${ARGN}
        faad)
endfunction()

# ---------------------------------------------------------------- the plug-in
set(JUSTSTEM_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND JUSTSTEM_FORMATS AU)
endif()

juce_add_plugin(justStem
    COMPANY_NAME "justStem"
    BUNDLE_ID "com.juststem.juststem"
    PLUGIN_MANUFACTURER_CODE Cvtl
    PLUGIN_CODE Ctst
    FORMATS ${JUSTSTEM_FORMATS}
    PRODUCT_NAME "justStem"
    IS_SYNTH FALSE
    NEEDS_CURL TRUE  # Linux: HTTPS (downloading yt-dlp and Deno) needs libcurl
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD FALSE  # juststem_package installs instead
    VST3_CATEGORIES Fx Tools
    AU_MAIN_TYPE kAudioUnitType_Effect)

target_sources(justStem PRIVATE ${JUSTSTEM_IO_SOURCES} ${JUSTSTEM_PLUGIN_SOURCES})
juststem_juce_settings(justStem PUBLIC juce::juce_audio_utils)
target_compile_definitions(justStem PUBLIC JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(justStem PRIVATE juststem_fonts)
target_link_libraries(justStem PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Install for the current user where DAWs look. Windows' VST3 folder (Program Files\Common Files\VST3)
# needs admin rights, so there the bundle is left in the build folder to copy by hand.
if(APPLE)
    juststem_package(justStem_VST3 INSTALL_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
    juststem_package(justStem_AU INSTALL_DIR "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
elseif(WIN32)
    juststem_package(justStem_VST3)
else()
    juststem_package(justStem_VST3 INSTALL_DIR "$ENV{HOME}/.vst3")
endif()
juststem_package(justStem_Standalone)

# ---------------------------------------------------------------- developer tools (not shipped)
# juststem_cli: separate / analyse / import from the command line, plus self-tests.
juce_add_console_app(juststem_cli PRODUCT_NAME "juststem_cli" NEEDS_CURL TRUE) # `import` downloads too
target_sources(juststem_cli PRIVATE tools/cli.cpp tools/selftest.cpp ${JUSTSTEM_IO_SOURCES})
juststem_juce_settings(juststem_cli PRIVATE juce::juce_audio_formats) # no UI, so no GUI modules
target_link_libraries(juststem_cli PRIVATE juce::juce_recommended_config_flags)
juststem_copy_runtime(juststem_cli)

# juststem_uitest: runs the real plug-in code without a DAW (screenshots, audio, save/reload).
juce_add_console_app(juststem_uitest PRODUCT_NAME "juststem_uitest")
target_sources(juststem_uitest PRIVATE tools/uitest.cpp ${JUSTSTEM_IO_SOURCES} ${JUSTSTEM_PLUGIN_SOURCES})
juststem_juce_settings(juststem_uitest PRIVATE juce::juce_audio_utils)
target_compile_definitions(juststem_uitest PRIVATE
    JUCE_MODAL_LOOPS_PERMITTED=1 JucePlugin_Name="justStem" JucePlugin_Build_Standalone=0)
target_link_libraries(juststem_uitest PRIVATE juststem_fonts juce::juce_recommended_config_flags)
juststem_copy_runtime(juststem_uitest)
