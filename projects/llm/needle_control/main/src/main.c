#define _GNU_SOURCE

#include "agent.h"
#include "board.h"
#include "hifw_config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUEST_SIZE 4096

typedef struct {
    const char *model_path;
    const char *tools_path;
    unsigned int port;
    bool think;
} app_options;

/* What one sentence turned into, for the console and for HTTP alike. */
typedef struct {
    agent_result agent;
    char tool[48];
    char result[256];
    int status;
} command_outcome;

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    g_running = 0;
}

/* Reads "key":"text" or "key":number from the call; the grammar keeps it flat. */
static bool call_field(const char *call, const char *key, char *out, size_t size)
{
    char quoted[64];
    snprintf(quoted, sizeof(quoted), "\"%s\":", key);
    const char *value = strstr(call, quoted);
    if (value == NULL || size == 0) return false;
    value += strlen(quoted);
    const bool string = *value == '"';
    value += string ? 1 : 0;
    size_t length = 0;
    while (value[length] != '\0' && length + 1 < size &&
        (string ? value[length] != '"' : strchr(",}]", value[length]) == NULL)) {
        out[length] = value[length];
        ++length;
    }
    out[length] = '\0';
    return length > 0;
}

static int call_int(const char *call, const char *key, int fallback)
{
    char value[16];
    return call_field(call, key, value, sizeof(value)) ? atoi(value) : fallback;
}

static void run_command(const app_options *options, const char *text, command_outcome *outcome)
{
    memset(outcome, 0, sizeof(*outcome));
    outcome->status = -1;
    if (agent_run(text, options->think, CONFIG_NEEDLE_CONTROL_MAX_TOKENS, &outcome->agent) != 0) {
        snprintf(outcome->result, sizeof(outcome->result), "inference failed");
        return;
    }
    const char *call = outcome->agent.call;
    if (!call_field(call, "name", outcome->tool, sizeof(outcome->tool))) {
        snprintf(outcome->result, sizeof(outcome->result), "no tool matches the request");
        return;
    }
    char *result = outcome->result;
    const size_t size = sizeof(outcome->result);
    if (strcmp(outcome->tool, "take_photo") == 0) {
        outcome->status = board_take_photo(result, size);
    } else if (strcmp(outcome->tool, "beep") == 0) {
        outcome->status = board_beep(call_int(call, "times", 1), result, size);
    } else if (strcmp(outcome->tool, "set_timer") == 0) {
        outcome->status = board_set_timer(call_int(call, "seconds", 0), result, size);
    } else if (strcmp(outcome->tool, "get_memory") == 0) {
        outcome->status = board_get_memory(result, size);
    } else if (strcmp(outcome->tool, "get_ip_address") == 0) {
        outcome->status = board_get_ip_address(result, size);
    } else {
        snprintf(result, size, "no handler for %s", outcome->tool);
    }
}

static void print_outcome(const command_outcome *outcome)
{
    printf("call: %s  (confidence %.2f, %u tokens, %.1f s)\n", outcome->agent.call,
        (double)outcome->agent.confidence, outcome->agent.tokens, outcome->agent.ms / 1000.0);
    printf("%s: %s\n", outcome->status == 0 ? "done" : "failed", outcome->result);
    fflush(stdout);
}

/* Copies text into a JSON string body, escaping what JSON requires. */
static size_t json_escape(const char *text, char *out, size_t size)
{
    size_t used = 0;
    for (; *text != '\0' && used + 7 < size; ++text) {
        const unsigned char c = (unsigned char)*text;
        if (c == '"' || c == '\\') {
            out[used++] = '\\';
            out[used++] = (char)c;
        } else if (c < 0x20) {
            used += (size_t)snprintf(out + used, size - used, "\\u%04x", c);
        } else {
            out[used++] = (char)c;
        }
    }
    out[used] = '\0';
    return used;
}

/* Extracts the "text" member of the request body, undoing simple escapes. */
static bool body_text(const char *body, char *out, size_t size)
{
    const char *value = strstr(body, "\"text\"");
    value = value != NULL ? strchr(value + 6, '"') : NULL;
    if (value == NULL) return false;
    size_t used = 0;
    for (++value; *value != '\0' && *value != '"' && used + 1 < size; ++value) {
        if (*value == '\\' && value[1] != '\0') ++value;
        out[used++] = *value;
    }
    out[used] = '\0';
    return used > 0;
}

static void http_reply(int client, const char *status, const char *body)
{
    char header[160];
    const int length = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Connection: close\r\n\r\n", status, strlen(body));
    (void)send(client, header, (size_t)length, MSG_NOSIGNAL);
    (void)send(client, body, strlen(body), MSG_NOSIGNAL);
}

/* POST /command with {"text": "..."}; one request per connection. */
static void serve_http(const app_options *options, int listener)
{
    static char request[REQUEST_SIZE];
    const int client = accept(listener, NULL, NULL);
    if (client < 0) return;
    size_t used = 0;
    const char *body = NULL;
    /* Read until the body announced by Content-Length has arrived. */
    while (used + 1 < sizeof(request)) {
        struct pollfd wait = {.fd = client, .events = POLLIN};
        if (poll(&wait, 1, 2000) <= 0) break;
        const ssize_t received = recv(client, request + used, sizeof(request) - 1 - used, 0);
        if (received <= 0) break;
        used += (size_t)received;
        request[used] = '\0';
        body = strstr(request, "\r\n\r\n");
        const char *length = strcasestr(request, "Content-Length:");
        if (body != NULL && (length == NULL ||
                (size_t)(request + used - (body + 4)) >= (size_t)atoi(length + 15))) {
            break;
        }
    }
    char text[512];
    if (strncmp(request, "POST /command", 13) != 0) {
        http_reply(client, "404 Not Found", "{\"error\":\"use POST /command\"}");
    } else if (body == NULL || !body_text(body + 4, text, sizeof(text))) {
        http_reply(client, "400 Bad Request", "{\"error\":\"body needs a text field\"}");
    } else {
        command_outcome outcome;
        char escaped_text[1024];
        char escaped_result[512];
        char reply[4096];
        printf("%s (HTTP)\n", text);
        run_command(options, text, &outcome);
        print_outcome(&outcome);
        json_escape(text, escaped_text, sizeof(escaped_text));
        json_escape(outcome.result, escaped_result, sizeof(escaped_result));
        snprintf(reply, sizeof(reply),
            "{\"text\":\"%s\",\"call\":%s,\"ok\":%s,\"result\":\"%s\","
            "\"confidence\":%.4f,\"ms\":%.0f}",
            escaped_text, outcome.agent.call[0] != '\0' ? outcome.agent.call : "[]",
            outcome.status == 0 ? "true" : "false", escaped_result,
            (double)outcome.agent.confidence, outcome.agent.ms);
        http_reply(client, "200 OK", reply);
    }
    close(client);
}

static int open_listener(unsigned int port)
{
    const int reuse = 1;
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        bind(fd, (const struct sockaddr *)&address, sizeof(address)) != 0 || listen(fd, 4) != 0) {
        fprintf(stderr, "cannot listen on port %u: %s\n", port, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

static int parse_options(int argc, char **argv, app_options *options)
{
    *options = (app_options) {
        .model_path = CONFIG_NEEDLE_CONTROL_MODEL_PATH,
        .tools_path = CONFIG_NEEDLE_CONTROL_TOOLS_PATH,
        .port = CONFIG_NEEDLE_CONTROL_HTTP_PORT,
    };
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "--think") == 0) {
            options->think = true;
            continue;
        }
        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0 || index + 1 >= argc) {
            fprintf(stderr, "usage: %s [--model FILE.cact] [--tools tools.json] [--port N|0] [--think]\n",
                argv[0]);
            return strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0 ? 1 : -1;
        }
        const char *value = argv[++index];
        if (strcmp(argument, "--model") == 0) {
            options->model_path = value;
        } else if (strcmp(argument, "--tools") == 0) {
            options->tools_path = value;
        } else if (strcmp(argument, "--port") == 0) {
            options->port = (unsigned int)strtoul(value, NULL, 10);
        } else {
            fprintf(stderr, "unknown option: %s\n", argument);
            return -1;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    app_options options;
    const int parsed = parse_options(argc, argv, &options);
    if (parsed != 0) {
        return parsed > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    if (agent_open(options.model_path, options.tools_path) != 0) {
        return EXIT_FAILURE;
    }
    const int listener = options.port != 0 ? open_listener(options.port) : -1;
    if (listener >= 0) {
        printf("HTTP: POST http://<board>:%u/command {\"text\":\"...\"}\n", options.port);
    }
    printf("type a request, for example \"take a picture\"; Ctrl-D or Ctrl-C to quit\n> ");
    fflush(stdout);

    char line[512];
    /* Unbuffered, so poll() sees every line that has not been read yet. */
    setvbuf(stdin, NULL, _IONBF, 0);
    /* Without a console, as when started at boot, keep serving HTTP. */
    bool console = true;
    while (g_running) {
        struct pollfd fds[2] = {
            {.fd = console ? STDIN_FILENO : -1, .events = POLLIN},
            {.fd = listener, .events = POLLIN},
        };
        if (poll(fds, 2, 500) <= 0) continue;
        if (fds[1].revents & POLLIN) {
            serve_http(&options, listener);
            printf("> ");
            fflush(stdout);
        }
        if (fds[0].revents & (POLLIN | POLLHUP)) {
            if (fgets(line, sizeof(line), stdin) == NULL) {
                console = false;
                if (listener < 0) break;
                continue;
            }
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] != '\0') {
                command_outcome outcome;
                run_command(&options, line, &outcome);
                print_outcome(&outcome);
            }
            printf("> ");
            fflush(stdout);
        }
    }
    if (listener >= 0) close(listener);
    agent_close();
    printf("\n");
    return EXIT_SUCCESS;
}
