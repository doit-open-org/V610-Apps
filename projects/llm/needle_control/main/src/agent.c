#define _POSIX_C_SOURCE 200809L

#include "agent.h"

#include "nd_grammar.h"
#include "nd_model.h"
#include "nd_quant.h"
#include "nd_sample.h"
#include "nd_tokenizer.h"

#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define AGENT_MAX_IDS 1024
#define AGENT_MAX_TEXT 2048

static nd_model g_model;
static nd_grammar g_grammar;
static void *g_blob = MAP_FAILED;
static size_t g_blob_size;
static bool g_open;

/*
 * Two cores: every matrix-vector product and the attention heads are split by
 * output row, half on a worker thread and half on the caller. The engine calls
 * through nd_parallel_rows for exactly this.
 */
static struct {
    pthread_t thread;
    sem_t go;
    sem_t done;
    nd_row_fn fn;
    void *ctx;
    uint32_t first;
    uint32_t end;
    bool running;
} g_worker;

static void *worker_main(void *argument)
{
    (void)argument;
    for (;;) {
        sem_wait(&g_worker.go);
        if (g_worker.fn == NULL) break;
        g_worker.fn(g_worker.ctx, g_worker.first, g_worker.end);
        sem_post(&g_worker.done);
    }
    return NULL;
}

static void rows_two_cores(nd_row_fn fn, void *ctx, uint32_t rows)
{
    const uint32_t half = rows / 2;
    /* A handful of rows costs less than waking the other core. */
    if (half < 4) {
        fn(ctx, 0, rows);
        return;
    }
    g_worker.fn = fn;
    g_worker.ctx = ctx;
    g_worker.first = half;
    g_worker.end = rows;
    sem_post(&g_worker.go);
    fn(ctx, 0, half);
    sem_wait(&g_worker.done);
}

static void worker_start(void)
{
    if (sysconf(_SC_NPROCESSORS_ONLN) < 2 || sem_init(&g_worker.go, 0, 0) != 0 ||
        sem_init(&g_worker.done, 0, 0) != 0) {
        return;
    }
    if (pthread_create(&g_worker.thread, NULL, worker_main, NULL) == 0) {
        g_worker.running = true;
        nd_parallel_rows = rows_two_cores;
    }
}

static void worker_stop(void)
{
    if (!g_worker.running) return;
    g_worker.fn = NULL;
    sem_post(&g_worker.go);
    pthread_join(g_worker.thread, NULL);
    g_worker.running = false;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static char *read_file(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        perror(path);
        return NULL;
    }
    (void)fseek(file, 0, SEEK_END);
    const long length = ftell(file);
    (void)fseek(file, 0, SEEK_SET);
    char *data = length >= 0 ? malloc((size_t)length + 1) : NULL;
    if (data == NULL || fread(data, 1, (size_t)length, file) != (size_t)length) {
        fprintf(stderr, "cannot read %s\n", path);
        free(data);
        data = NULL;
    } else {
        data[length] = '\0';
        *size = (size_t)length;
    }
    fclose(file);
    return data;
}

int agent_open(const char *model_path, const char *tools_path)
{
    size_t tools_size = 0;
    char *tools = read_file(tools_path, &tools_size);
    if (tools == NULL) {
        return -1;
    }
    /* The schema file's own formatting must not reach the model. */
    char *packed = malloc(tools_size + 1);
    const int packed_size = packed == NULL ? -1 :
        nd_json_compact(tools, tools_size, packed, tools_size + 1);
    free(tools);
    if (packed_size < 0) {
        fprintf(stderr, "%s: cannot compact the tool schemas\n", tools_path);
        free(packed);
        return -1;
    }
    const char *error = NULL;
    if (nd_grammar_compile(&g_grammar, packed, (size_t)packed_size, &error) != 0) {
        fprintf(stderr, "%s: %s\n", tools_path, error != NULL ? error : "invalid schemas");
        free(packed);
        return -1;
    }

    const int fd = open(model_path, O_RDONLY | O_CLOEXEC);
    struct stat info;
    if (fd < 0 || fstat(fd, &info) != 0) {
        perror(model_path);
        if (fd >= 0) close(fd);
        free(packed);
        return -1;
    }
    g_blob_size = (size_t)info.st_size;
    g_blob = mmap(NULL, g_blob_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (g_blob == MAP_FAILED || nd_model_open(&g_model, g_blob, g_blob_size) != 0) {
        fprintf(stderr, "%s: not a Needle 3 model\n", model_path);
        if (g_blob != MAP_FAILED) munmap(g_blob, g_blob_size);
        g_blob = MAP_FAILED;
        free(packed);
        return -1;
    }

    worker_start();
    static char prefix[AGENT_MAX_TEXT * 2];
    static uint32_t ids[AGENT_MAX_IDS];
    snprintf(prefix, sizeof(prefix), "<|im_start|>user\n<tools>%.*s</tools>",
        packed_size, packed);
    free(packed);
    ids[0] = ND_BOS_ID;
    const int count = nd_tok_encode(&g_model.tok, prefix, strlen(prefix), ids + 1,
        AGENT_MAX_IDS - 1);
    if (count < 0) {
        fprintf(stderr, "%s: the tool schemas are too long\n", tools_path);
        agent_close();
        return -1;
    }
    const double start = now_ms();
    for (int index = 0; index <= count; ++index) {
        (void)nd_model_step_hidden(&g_model, ids[index]);
    }
    nd_model_snapshot(&g_model);
    printf("needle: %u layers, %u tools, prefix %d tokens prefilled in %.0f ms\n",
        g_model.n_layers, g_grammar.n_tools, count + 1, now_ms() - start);
    g_open = true;
    return 0;
}

void agent_close(void)
{
    worker_stop();
    if (g_open) {
        nd_model_close(&g_model);
        g_open = false;
    }
    if (g_blob != MAP_FAILED) {
        munmap(g_blob, g_blob_size);
        g_blob = MAP_FAILED;
    }
}

/* Feeds a token both to the grammar and to the model, keeping its text. */
static const float *feed(nd_sampler *sampler, uint32_t id, char *text, size_t *length)
{
    char piece[256];
    nd_sample_accept(sampler, id);
    nd_tok_decode_ex(&g_model.tok, &id, 1, piece, sizeof(piece), 0);
    const size_t size = strlen(piece);
    if (*length + size < AGENT_MAX_TEXT) {
        memcpy(text + *length, piece, size + 1);
        *length += size;
    }
    return nd_model_step_hidden(&g_model, id);
}

int agent_run(const char *text, bool think, unsigned int max_tokens, agent_result *result)
{
    static char suffix[AGENT_MAX_TEXT];
    static char generated[AGENT_MAX_TEXT];
    static uint32_t ids[AGENT_MAX_IDS];
    if (!g_open || text == NULL || result == NULL) {
        return -1;
    }
    memset(result, 0, sizeof(*result));
    snprintf(suffix, sizeof(suffix), "\n%s<|im_end|>\n<|im_start|>assistant\n", text);

    const double start = now_ms();
    nd_model_rewind(&g_model);
    nd_sampler sampler;
    nd_sampler_init(&sampler, &g_model.tok, &g_grammar);
    const int count = nd_tok_encode_ex(&g_model.tok, suffix, strlen(suffix), ids,
        AGENT_MAX_IDS, 0);
    if (count <= 0) {
        return -1;
    }
    const float *hidden = NULL;
    for (int index = 0; index < count; ++index) {
        hidden = nd_model_step_hidden(&g_model, ids[index]);
    }

    size_t length = 0;
    generated[0] = '\0';
    if (!think) {
        /* An empty reasoning block, then the call opens directly. */
        uint32_t newline[4];
        const int lines = nd_tok_encode_ex(&g_model.tok, "\n", 1, newline, 4, 0);
        hidden = feed(&sampler, ND_THINK_START_ID, generated, &length);
        hidden = feed(&sampler, ND_THINK_END_ID, generated, &length);
        for (int index = 0; index < lines; ++index) {
            hidden = feed(&sampler, newline[index], generated, &length);
        }
        hidden = feed(&sampler, ND_TOOL_CALL_START_ID, generated, &length);
    }
    for (unsigned int index = 0; index < max_tokens; ++index) {
        const uint32_t id = nd_sample_hidden(&g_model, &sampler, hidden);
        if (id == (uint32_t)-1 || id == ND_EOS_ID || id == ND_IM_END_ID) {
            break;
        }
        hidden = feed(&sampler, id, generated, &length);
        ++result->tokens;
    }
    result->ms = now_ms() - start;
    result->confidence = nd_model_confidence(&g_model);

    /* The call sits between <tool_call> and </tool_call>. */
    const char *call = strstr(generated, "<tool_call>");
    const char *end = call != NULL ? strstr(call, "</tool_call>") : NULL;
    if (call == NULL || end == NULL) {
        snprintf(result->call, sizeof(result->call), "[]");
        return 0;
    }
    call += strlen("<tool_call>");
    snprintf(result->call, sizeof(result->call), "%.*s", (int)(end - call), call);
    return 0;
}
