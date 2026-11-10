#ifndef AGENT_H
#define AGENT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Turns one sentence into a tool call with Needle 3.
 *
 * The model file is mapped rather than read, so its pages come from the file
 * as they are used instead of occupying the process's memory. The prompt
 * prefix that carries the tool schemas is the same for every request; it is
 * prefilled once at open and every request resumes from that snapshot.
 */

typedef struct {
    /* The tool call array the model produced, "[]" when no tool fits. */
    char call[512];
    /* Calibrated confidence in [0, 1], or a negative value if unavailable. */
    float confidence;
    unsigned int tokens;
    double ms;
} agent_result;

/* Returns 0 on success; on failure prints the reason to stderr. */
int agent_open(const char *model_path, const char *tools_path);
void agent_close(void);

/*
 * think false skips the model's reasoning block, which is most of what it
 * generates, and goes straight to the call.
 */
int agent_run(const char *text, bool think, unsigned int max_tokens,
    agent_result *result);

#endif
