idf_component_register(SRCS "face.c" "face_anim.c" "face_action.c" "face_follow.c" "face_render.c"
                            "face_geom.c" "face_raster.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "."
                       PRIV_REQUIRES display servo esp_timer esp_hw_support)

# The face is rendered every frame: build it optimized even when the project
# uses the default debug level (-Og). Only this component is affected.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)

# Tables are generated at build time: edit tong.avatar.json (expressions,
# states) or actions.json (head motions), run `idf.py build`, done. The output
# lives in the build directory and is not committed.
idf_build_get_property(python PYTHON)

set(avatar_json  "${COMPONENT_DIR}/tong.avatar.json")
set(actions_json "${COMPONENT_DIR}/actions.json")
set(avatar2c     "${COMPONENT_DIR}/scripts/avatar2c.py")
set(actions2c    "${COMPONENT_DIR}/scripts/actions2c.py")
set(gen_c        "${CMAKE_CURRENT_BINARY_DIR}/face_data.c")
set(gen_h        "${CMAKE_CURRENT_BINARY_DIR}/face_data.h")
set(gen_act_c    "${CMAKE_CURRENT_BINARY_DIR}/face_action_data.c")
set(gen_act_h    "${CMAKE_CURRENT_BINARY_DIR}/face_action_data.h")

if(NOT EXISTS "${avatar_json}")
    message(FATAL_ERROR
        "face: ${avatar_json} is missing.\n"
        "Export it from the avatar lab (Export > Download .avatar.json definition) "
        "and save it there under that name.")
endif()
if(NOT EXISTS "${actions_json}")
    message(FATAL_ERROR "face: ${actions_json} is missing (it ships with the face component).")
endif()

add_custom_command(
    OUTPUT ${gen_c} ${gen_h}
    COMMAND ${python} ${avatar2c} ${avatar_json} ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${avatar_json} ${avatar2c}
    COMMENT "Generating face tables from tong.avatar.json"
    VERBATIM)

# actions may borrow expressions, so they are checked against the avatar too
add_custom_command(
    OUTPUT ${gen_act_c} ${gen_act_h}
    COMMAND ${python} ${actions2c} ${actions_json} ${avatar_json} ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS ${actions_json} ${avatar_json} ${actions2c}
    COMMENT "Generating face actions from actions.json"
    VERBATIM)

add_custom_target(face_data DEPENDS ${gen_c} ${gen_h} ${gen_act_c} ${gen_act_h})
add_dependencies(${COMPONENT_LIB} face_data)

target_sources(${COMPONENT_LIB} PRIVATE ${gen_c} ${gen_act_c})
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

set_property(DIRECTORY "${COMPONENT_DIR}" APPEND PROPERTY
             ADDITIONAL_CLEAN_FILES ${gen_c} ${gen_h} ${gen_act_c} ${gen_act_h})
