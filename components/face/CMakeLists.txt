idf_component_register(SRCS "face.c" "face_geom.c" "face_raster.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES display esp_timer)

# The face is rendered every frame: build it optimized even when the project
# uses the default debug level (-Og). Only this component is affected.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# Expression and state tables are generated from the avatar file at build time:
# edit anchor.avatar.json, run `idf.py build`, done. The output lives in the
# build directory and is not committed.
idf_build_get_property(python PYTHON)

set(avatar_json "${COMPONENT_DIR}/anchor.avatar.json")
set(avatar2c    "${COMPONENT_DIR}/scripts/avatar2c.py")
set(gen_c       "${CMAKE_CURRENT_BINARY_DIR}/face_data.c")
set(gen_h       "${CMAKE_CURRENT_BINARY_DIR}/face_data.h")

add_custom_command(
    OUTPUT ${gen_c} ${gen_h}
    COMMAND ${python} ${avatar2c} ${avatar_json} ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${avatar_json} ${avatar2c}
    COMMENT "Generating face tables from anchor.avatar.json"
    VERBATIM)

add_custom_target(face_data DEPENDS ${gen_c} ${gen_h})
add_dependencies(${COMPONENT_LIB} face_data)

target_sources(${COMPONENT_LIB} PRIVATE ${gen_c})
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

set_property(DIRECTORY "${COMPONENT_DIR}" APPEND PROPERTY
             ADDITIONAL_CLEAN_FILES ${gen_c} ${gen_h})
