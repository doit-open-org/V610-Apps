# Needle 3 inference engine, vendored unmodified from PruhaNLP/needle-3-esp32;
# see README.md. The engine's own unused-parameter style is tolerated here.
hifw_component_register(
    SRCS
        src/nd_cact.c
        src/nd_grammar.c
        src/nd_model.c
        src/nd_quant.c
        src/nd_sample.c
        src/nd_tokenizer.c
    INCLUDE_DIRS include
    LINK_LIBRARIES m
    COMPILE_OPTIONS -std=c99 -O3 -Wall -Wextra -Wno-unused-parameter)
