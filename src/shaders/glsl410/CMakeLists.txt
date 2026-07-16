find_program(PARTICLESATURN_GLSLANG_VALIDATOR glslangValidator REQUIRED)
file(GLOB PARTICLESATURN_GLSL410_SOURCES "${CMAKE_CURRENT_LIST_DIR}/*.vert" "${CMAKE_CURRENT_LIST_DIR}/*.frag")

set(PARTICLESATURN_GLSL410_STAMPS)
foreach(SOURCE IN LISTS PARTICLESATURN_GLSL410_SOURCES)
    get_filename_component(NAME "${SOURCE}" NAME)
    set(STAMP "${CMAKE_BINARY_DIR}/generated/glsl410/${NAME}.stamp")
    add_custom_command(
        OUTPUT "${STAMP}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/generated/glsl410"
        COMMAND "${PARTICLESATURN_GLSLANG_VALIDATOR}" "${SOURCE}"
        COMMAND "${CMAKE_COMMAND}" -E touch "${STAMP}"
        DEPENDS "${SOURCE}"
        VERBATIM
    )
    list(APPEND PARTICLESATURN_GLSL410_STAMPS "${STAMP}")
endforeach()
add_custom_target(ParticleSaturnOpenGL41Shaders ALL DEPENDS ${PARTICLESATURN_GLSL410_STAMPS})
