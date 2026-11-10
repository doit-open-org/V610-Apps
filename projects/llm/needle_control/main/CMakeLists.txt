hifw_component_register(
    SRCS src/main.c src/agent.c src/board.c
    REQUIRES needle_engine media_engine
    LINK_LIBRARIES pthread m
    COMPILE_OPTIONS -Wall -Wextra -Werror)
