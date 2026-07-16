cmake_minimum_required(VERSION 3.19)

if(NOT DEFINED PARTICLESATURN_PATCH OR PARTICLESATURN_PATCH STREQUAL "")
    message(FATAL_ERROR "Usage: cmake -DPARTICLESATURN_PATCH=<name> -P apply_third_party_patch.cmake")
endif()

get_filename_component(PARTICLESATURN_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
find_package(Git REQUIRED)

function(particlesaturn_apply_patch source_root patch_file)
    set(options)
    set(one_value_args)
    set(multi_value_args APPLY_ARGUMENTS)
    cmake_parse_arguments(PATCH "${options}" "${one_value_args}" "${multi_value_args}" ${ARGN})

    if(NOT IS_DIRECTORY "${source_root}" OR NOT EXISTS "${patch_file}")
        message(FATAL_ERROR "Patch source or file is missing: ${patch_file}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_root}" apply ${PATCH_APPLY_ARGUMENTS} --reverse --check "${patch_file}"
        RESULT_VARIABLE reverse_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if(reverse_result EQUAL 0)
        message(STATUS "Third-party patch already applied: ${patch_file}")
        return()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_root}" apply ${PATCH_APPLY_ARGUMENTS} --check "${patch_file}"
        RESULT_VARIABLE check_result
        OUTPUT_VARIABLE check_stdout
        ERROR_VARIABLE check_stderr)
    if(NOT check_result EQUAL 0)
        message(FATAL_ERROR "Patch does not match the current source: ${patch_file}\n${check_stdout}${check_stderr}")
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${source_root}" apply ${PATCH_APPLY_ARGUMENTS} "${patch_file}"
        RESULT_VARIABLE apply_result
        OUTPUT_VARIABLE apply_stdout
        ERROR_VARIABLE apply_stderr)
    if(NOT apply_result EQUAL 0)
        message(FATAL_ERROR "Unable to apply patch: ${patch_file}\n${apply_stdout}${apply_stderr}")
    endif()
    message(STATUS "Applied third-party patch: ${patch_file}")
endfunction()

set(PATCH_DIRECTORY "${PARTICLESATURN_REPOSITORY_ROOT}/patches")
if(PARTICLESATURN_PATCH STREQUAL "diligent-volk-loader-path")
    particlesaturn_apply_patch(
        "${PARTICLESATURN_REPOSITORY_ROOT}/libs/DiligentCore/ThirdParty/volk"
        "${PATCH_DIRECTORY}/diligent-volk-loader-path.patch")
elseif(PARTICLESATURN_PATCH STREQUAL "tensorflow-lite")
    # The resource-pruning patch predates the Apple compiler fix. Keep the
    # independent compiler hunk separate so either state applies idempotently.
    particlesaturn_apply_patch(
        "${PARTICLESATURN_REPOSITORY_ROOT}/HandTracker/libs/tensorflow"
        "${PATCH_DIRECTORY}/tflite-prune.patch"
        APPLY_ARGUMENTS "--exclude=tensorflow/lite/kernels/elementwise.cc")
    particlesaturn_apply_patch(
        "${PARTICLESATURN_REPOSITORY_ROOT}/HandTracker/libs/tensorflow"
        "${PATCH_DIRECTORY}/tflite-elementwise-compat.patch")
elseif(PARTICLESATURN_PATCH STREQUAL "imgui-md3")
    particlesaturn_apply_patch(
        "${PARTICLESATURN_REPOSITORY_ROOT}/libs/imgui"
        "${PATCH_DIRECTORY}/imgui-md3.patch")
else()
    message(FATAL_ERROR "Unknown patch '${PARTICLESATURN_PATCH}'. Supported values: diligent-volk-loader-path, tensorflow-lite, imgui-md3")
endif()
