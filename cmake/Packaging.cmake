# Packaging: put ONNX Runtime (and optionally the models) where the plug-in finds them, sign (macOS), install.
#
#   juststem_package(<target> [INSTALL_DIR <dir>])
#
# Layout (the model is found by justStemProcessor::defaultModelFile):
#   macOS bundles  Contents/Frameworks/<runtime>, Contents/Resources/<models>; ad-hoc signed
#   VST3 bundles   Contents/<arch>/<runtime> next to the binary, Contents/Resources/<models>
#   Standalone     <runtime> and <models> next to the executable
#
# Done here rather than with JUCE's COPY_PLUGIN_AFTER_BUILD, which installs before the extra
# files are added and (on macOS) before the bundle is re-signed.

set(JUSTSTEM_BUNDLE_MODELS "FAST" CACHE STRING "Models to bundle inside plug-in packages: FAST, ALL, or OFF")
set_property(CACHE JUSTSTEM_BUNDLE_MODELS PROPERTY STRINGS FAST ALL OFF)
set(JUSTSTEM_MODELS "")
if(JUSTSTEM_BUNDLE_MODELS AND NOT JUSTSTEM_BUNDLE_MODELS STREQUAL "OFF")
    # FAST: the fast model.
    set(JUSTSTEM_MODEL "${CMAKE_SOURCE_DIR}/models/htdemucs_6s_opt.onnx")
    if(NOT EXISTS "${JUSTSTEM_MODEL}")
        message(FATAL_ERROR "Missing ${JUSTSTEM_MODEL}.\n"
            "Build it with scripts/prepare_demucs_onnx.py (see the comment at the top of that script).")
    endif()
    set(JUSTSTEM_MODELS "${JUSTSTEM_MODEL}")
    if(JUSTSTEM_BUNDLE_MODELS STREQUAL "ALL")
        # ALL: also the HQ model and the MIDI model, when they've been built / fetched.
        set(JUSTSTEM_HQ_MODEL "${CMAKE_SOURCE_DIR}/models/bs_roformer_sw_6stem_opt.onnx")
        if(EXISTS "${JUSTSTEM_HQ_MODEL}")
            list(APPEND JUSTSTEM_MODELS "${JUSTSTEM_HQ_MODEL}")
            message(STATUS "Bundling the HQ model: ${JUSTSTEM_HQ_MODEL}")
        else()
            message(STATUS "No HQ model (${JUSTSTEM_HQ_MODEL}); building without bundled HQ model. See scripts/prepare_roformer_onnx.py.")
        endif()
        set(JUSTSTEM_MIDI_MODELS "${CMAKE_SOURCE_DIR}/models/transkun_piano.onnx"
            "${CMAKE_SOURCE_DIR}/models/transkun_piano_heads.onnx" "${CMAKE_SOURCE_DIR}/models/transkun_piano_frontend.bin")
        if(EXISTS "${CMAKE_SOURCE_DIR}/models/transkun_piano.onnx")
            list(APPEND JUSTSTEM_MODELS ${JUSTSTEM_MIDI_MODELS})
            message(STATUS "Bundling the MIDI model")
        else()
            message(STATUS "No MIDI model; building without it bundled. See scripts/fetch_model.sh midi.")
        endif()
    endif()
endif()

# Copy ONNX Runtime next to a developer tool so it runs from the build folder.
function(juststem_copy_runtime target)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    if(UNIX)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "${ORT_LIB_DIR}")
    endif()
endfunction()

function(juststem_package target)
    cmake_parse_arguments(ARG "" "INSTALL_DIR" "" ${ARGN})
    get_target_property(format ${target} JUCE_TARGET_KIND_STRING)

    if(APPLE)
        set(contents "$<TARGET_BUNDLE_CONTENT_DIR:${target}>")
        if(JUSTSTEM_MODELS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${contents}/Resources"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${JUSTSTEM_MODELS} "${contents}/Resources/"
                VERBATIM)
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${contents}/Frameworks"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_RUNTIME_FILES} "${contents}/Frameworks/"
            # On CI, re-signing Microsoft's signed dylib prints "timestamps differ by 181 seconds" and the
            # chain stops there (touch did not help). Drop the existing signature first (unconfirmed; see CI).
            COMMAND codesign --remove-signature "${contents}/Frameworks/libonnxruntime.1.dylib"
            COMMAND codesign --force --sign - "${contents}/Frameworks/libonnxruntime.1.dylib"
            COMMAND codesign --force --sign - "$<TARGET_BUNDLE_DIR:${target}>"
            VERBATIM)
        set_target_properties(${target} PROPERTIES
            BUILD_RPATH "@loader_path/../Frameworks"
            INSTALL_RPATH "@loader_path/../Frameworks")
        set(bundle "$<TARGET_BUNDLE_DIR:${target}>")
        set(bundle_name "$<TARGET_BUNDLE_DIR_NAME:${target}>")
    else()
        set(binary_dir "$<TARGET_FILE_DIR:${target}>")
        if(format STREQUAL "VST3")
            set(model_dir "${binary_dir}/../Resources") # <name>.vst3/Contents/Resources
        else()
            set(model_dir "${binary_dir}")
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_RUNTIME_FILES} "${binary_dir}"
            VERBATIM)
        if(JUSTSTEM_MODELS)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${model_dir}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different ${JUSTSTEM_MODELS} "${model_dir}/"
                VERBATIM)
        endif()
        if(UNIX) # Linux: find the runtime next to the binary
            set_target_properties(${target} PROPERTIES BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
        endif()
        set(bundle "${binary_dir}/../..")                 # <name>.vst3
        set(bundle_name "${PROJECT_NAME}.vst3")
    endif()

    if(ARG_INSTALL_DIR)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -rf "${ARG_INSTALL_DIR}/${bundle_name}"
            COMMAND ${CMAKE_COMMAND} -E copy_directory "${bundle}" "${ARG_INSTALL_DIR}/${bundle_name}"
            VERBATIM)
    endif()
endfunction()
