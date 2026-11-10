hifw_component_register(
    SRCS src/servo_tracker.c src/servo_pwm.c src/servo_pinmux.c
    INCLUDE_DIRS include
    LINK_LIBRARIES m
    COMPILE_OPTIONS -Wall -Wextra -Werror)
