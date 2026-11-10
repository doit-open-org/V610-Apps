# Which decoders the runtime carries is decided by Kconfig
# (CONFIG_MIPK_MODEL_YOLO_POSE, CONFIG_MIPK_EXTRA_MEDIAPIPE_HAND);
# a package naming one that was left out is refused at open with a message
# naming it.
hifw_component_register(
    SRCS src/main.c src/keypoints.c
    REQUIRES mipk_video media_video_udp media_video_rtsp
    LINK_LIBRARIES pthread m
    COMPILE_OPTIONS -Wall -Wextra -Werror)
