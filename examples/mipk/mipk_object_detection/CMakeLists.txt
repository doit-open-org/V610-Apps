cmake_minimum_required(VERSION 3.16)

project(mipk_object_detection LANGUAGES C CXX ASM)

# APP_SDK_PATH is exported by "source <V610-App-SDK>/export.sh".
if(NOT DEFINED ENV{APP_SDK_PATH} OR "$ENV{APP_SDK_PATH}" STREQUAL "")
    message(FATAL_ERROR
        "APP_SDK_PATH is not set; run 'source <V610-App-SDK>/export.sh' first")
endif()
get_filename_component(APP_SDK_PATH "$ENV{APP_SDK_PATH}" ABSOLUTE)
set(CMAKE_SKIP_RPATH TRUE)

set(HIFW_EXTRA_COMPONENTS media_video_udp media_video_rtsp)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../../components/media_video_udp"
    "${CMAKE_CURRENT_LIST_DIR}/../../../components/media_video_rtsp")

include("${APP_SDK_PATH}/cmake/project.cmake")
hifw_project(mipk_object_detection)
mipk_write_capabilities(TARGET mipk_object_detection
    OUTPUT "${HIFW_OUTPUT_DIR}/mipk_capabilities.json")

set(MIPK_MODEL_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/model/yolo11n.mipk")
set_property(TARGET mipk_object_detection APPEND PROPERTY
    LINK_DEPENDS "${MIPK_MODEL_SOURCE}")
add_custom_command(TARGET mipk_object_detection POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MIPK_MODEL_SOURCE}"
        "${HIFW_OUTPUT_DIR}/yolo11n.mipk"
    VERBATIM)
install(FILES "${MIPK_MODEL_SOURCE}" DESTINATION bin)
