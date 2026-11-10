cmake_minimum_required(VERSION 3.16)

project(needle_control LANGUAGES C CXX ASM)

# APP_SDK_PATH is exported by "source <V610-App-SDK>/export.sh".
if(NOT DEFINED ENV{APP_SDK_PATH} OR "$ENV{APP_SDK_PATH}" STREQUAL "")
    message(FATAL_ERROR
        "APP_SDK_PATH is not set; run 'source <V610-App-SDK>/export.sh' first")
endif()
get_filename_component(APP_SDK_PATH "$ENV{APP_SDK_PATH}" ABSOLUTE)
set(CMAKE_SKIP_RPATH TRUE)

set(HIFW_EXTRA_COMPONENTS needle_engine)

include("${APP_SDK_PATH}/cmake/project.cmake")
hifw_project(needle_control)

# The model is not in the repository; tools/prepare_model.sh makes it.
set(NEEDLE_TOOLS_SOURCE "${CMAKE_CURRENT_LIST_DIR}/tools.json")
set_property(TARGET needle_control APPEND PROPERTY LINK_DEPENDS "${NEEDLE_TOOLS_SOURCE}")
add_custom_command(TARGET needle_control POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${NEEDLE_TOOLS_SOURCE}" "${HIFW_OUTPUT_DIR}/tools.json"
    VERBATIM)
install(FILES "${NEEDLE_TOOLS_SOURCE}" DESTINATION bin)
