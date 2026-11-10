hifw_component_register(
    SRCS src/main.c
    REQUIRES mipk_video media_video_udp media_video_rtsp
    LINK_LIBRARIES pthread
    COMPILE_OPTIONS -Wall -Wextra -Werror)
