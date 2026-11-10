cmake_minimum_required(VERSION 3.16)

project(hand_gimbal LANGUAGES C CXX ASM)

# APP_SDK_PATH is exported by "source <V610-App-SDK>/export.sh".
if(NOT DEFINED ENV{APP_SDK_PATH} OR "$ENV{APP_SDK_PATH}" STREQUAL "")
    message(FATAL_ERROR
        "APP_SDK_PATH is not set; run 'source <V610-App-SDK>/export.sh' first")
endif()
get_filename_component(APP_SDK_PATH "$ENV{APP_SDK_PATH}" ABSOLUTE)
set(CMAKE_SKIP_RPATH TRUE)

set(HIFW_EXTRA_COMPONENTS media_video_rtsp box_tracker servo_tracker)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../components/media_video_rtsp")

include("${APP_SDK_PATH}/cmake/project.cmake")
hifw_project(hand_gimbal)
mipk_write_capabilities(TARGET hand_gimbal
    OUTPUT "${HIFW_OUTPUT_DIR}/mipk_capabilities.json")

# The MediaPipe hand cascade is the same package mipk_keypoints ships.
set(MIPK_MODEL_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/../../../examples/mipk/mipk_keypoints/model/mediapipe_hand.mipk")
set_property(TARGET hand_gimbal APPEND PROPERTY LINK_DEPENDS "${MIPK_MODEL_SOURCE}")
add_custom_command(TARGET hand_gimbal POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${MIPK_MODEL_SOURCE}" "${HIFW_OUTPUT_DIR}/mediapipe_hand.mipk"
    VERBATIM)
install(FILES "${MIPK_MODEL_SOURCE}" DESTINATION bin)
