# Packaging: put ONNX Runtime and the model where the plug-in finds them, sign (macOS), install.
#
#   covertools_package(<target> [INSTALL_DIR <dir>])
#
# Layout (the model is found by CoverToolsProcessor::bundledModelFile):
#   macOS bundles  Contents/Frameworks/<runtime>, Contents/Resources/<model>; ad-hoc signed
#   VST3 bundles   Contents/<arch>/<runtime> next to the binary, Contents/Resources/<model>
#   Standalone     <runtime> and <model> next to the executable
#
# Done here rather than with JUCE's COPY_PLUGIN_AFTER_BUILD, which installs before the extra
# files are added and (on macOS) before the bundle is re-signed.

set(COVERTOOLS_MODEL "${CMAKE_SOURCE_DIR}/models/htdemucs_6s_web2.onnx")
if(NOT EXISTS "${COVERTOOLS_MODEL}")
    message(FATAL_ERROR "Missing ${COVERTOOLS_MODEL}.\n"
        "Build it with scripts/prepare_demucs_onnx.py (see the comment at the top of that script).")
endif()

# Copy ONNX Runtime next to a developer tool so it runs from the build folder.
function(covertools_copy_runtime target)
    if(NOT APPLE)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
    if(UNIX)
        set_target_properties(${target} PROPERTIES BUILD_RPATH "${ORT_LIB_DIR}")
    endif()
endfunction()

function(covertools_package target)
    cmake_parse_arguments(ARG "" "INSTALL_DIR" "" ${ARGN})
    get_target_property(format ${target} JUCE_TARGET_KIND_STRING)

    if(APPLE)
        set(contents "$<TARGET_BUNDLE_CONTENT_DIR:${target}>")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${contents}/Frameworks" "${contents}/Resources"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ORT_RUNTIME_FILES} "${contents}/Frameworks/"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${COVERTOOLS_MODEL}" "${contents}/Resources/"
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
            COMMAND ${CMAKE_COMMAND} -E make_directory "${model_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${COVERTOOLS_MODEL}" "${model_dir}/"
            VERBATIM)
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
