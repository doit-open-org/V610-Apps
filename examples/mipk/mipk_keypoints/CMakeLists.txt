cmake_minimum_required(VERSION 3.16)

project(mipk_keypoints LANGUAGES C CXX ASM)

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
hifw_project(mipk_keypoints)
mipk_write_capabilities(TARGET mipk_keypoints
    OUTPUT "${HIFW_OUTPUT_DIR}/mipk_capabilities.json")

set(MIPK_MODEL_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/model/person_pose.mipk"
    "${CMAKE_CURRENT_LIST_DIR}/model/mediapipe_hand.mipk")
file(GLOB MIPK_SKELETON_SOURCES "${CMAKE_CURRENT_LIST_DIR}/keypoints/*.keypoints")
set_property(TARGET mipk_keypoints APPEND PROPERTY
    LINK_DEPENDS ${MIPK_MODEL_SOURCES})
add_custom_command(TARGET mipk_keypoints POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MIPK_MODEL_SOURCES} "${HIFW_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MIPK_SKELETON_SOURCES} "${HIFW_OUTPUT_DIR}"
    VERBATIM)
install(FILES ${MIPK_MODEL_SOURCES}
    ${MIPK_SKELETON_SOURCES} DESTINATION bin)
