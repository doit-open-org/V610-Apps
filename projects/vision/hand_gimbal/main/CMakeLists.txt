hifw_component_register(
    SRCS src/main.c src/hand_pinch.c
    REQUIRES mipk_video media_video_rtsp box_tracker servo_tracker
    LINK_LIBRARIES pthread m
    COMPILE_OPTIONS -Wall -Wextra -Werror)
