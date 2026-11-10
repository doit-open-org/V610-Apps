#define _DEFAULT_SOURCE

#include "media_video_rtsp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define RTSP_MAX_CLIENTS 4
#define RTSP_REQUEST_SIZE 4096
#define RTSP_MAX_NALS 32
/* RTP payload per packet, so a packet fits a 1500 byte Ethernet frame. */
#define RTP_PAYLOAD_SIZE 1400
#define RTP_HEADER_SIZE 12
#define RTP_PAYLOAD_TYPE 96
#define TCP_SEND_BUFFER (512 * 1024)

typedef struct {
    int fd;          /* RTSP connection, -1 for a free slot */
    int rtp_fd;      /* UDP socket for RTP over UDP, -1 otherwise */
    struct sockaddr_in rtp_address;
    bool tcp;        /* RTP interleaved on the RTSP connection */
    uint8_t channel;
    bool playing;
    bool need_key;   /* skip frames until the next IDR */
    bool broken;     /* sending failed; the server thread closes it */
    uint16_t sequence;
    uint32_t ssrc;
    uint32_t session;
    size_t request_size;
    char request[RTSP_REQUEST_SIZE];
} rtsp_client;

struct media_video_rtsp {
    media_video_rtsp_config config;
    char path[64];
    int listen_fd;
    pthread_t thread;
    bool thread_started;
    atomic_bool running;
    pthread_mutex_t lock;
    rtsp_client clients[RTSP_MAX_CLIENTS];
    media_video_rtsp_stats stats;
    uint8_t packet[4 + RTP_HEADER_SIZE + 2 + RTP_PAYLOAD_SIZE];
};

void media_video_rtsp_config_init(media_video_rtsp_config *config)
{
    if (config == NULL) {
        return;
    }
    *config = (media_video_rtsp_config) {
        .port = 8554,
        .path = "live",
    };
}

static void client_close(rtsp_client *client)
{
    if (client->fd >= 0) {
        close(client->fd);
    }
    if (client->rtp_fd >= 0) {
        close(client->rtp_fd);
    }
    *client = (rtsp_client) {.fd = -1, .rtp_fd = -1};
}

/* Copies the value of a request header into value; false when absent. */
static bool header_value(const char *request, const char *name, char *value,
    size_t size)
{
    const size_t length = strlen(name);
    for (const char *line = request; line != NULL && *line != '\0';) {
        if (strncasecmp(line, name, length) == 0 && line[length] == ':') {
            const char *start = line + length + 1;
            while (*start == ' ') {
                ++start;
            }
            const size_t count = strcspn(start, "\r\n");
            snprintf(value, size, "%.*s", (int)(count < size ? count : size - 1), start);
            return true;
        }
        line = strstr(line, "\r\n");
        line = line == NULL ? NULL : line + 2;
    }
    return false;
}

static void reply(rtsp_client *client, const char *status, const char *cseq,
    const char *headers, const char *body)
{
    char response[RTSP_REQUEST_SIZE];
    const int size = snprintf(response, sizeof(response),
        "RTSP/1.0 %s\r\nCSeq: %s\r\n%s\r\n%s", status, cseq, headers, body);
    if (size > 0 && (size_t)size < sizeof(response)) {
        (void)send(client->fd, response, (size_t)size, MSG_NOSIGNAL);
    }
}

/* Prepares RTP over UDP to the client ports named in the Transport header. */
static bool setup_udp(rtsp_client *client, const char *transport,
    uint16_t *server_port)
{
    const char *ports = strstr(transport, "client_port=");
    socklen_t length = sizeof(client->rtp_address);
    struct sockaddr_in local = {0};
    if (ports == NULL || getpeername(client->fd,
            (struct sockaddr *)&client->rtp_address, &length) != 0) {
        return false;
    }
    client->rtp_address.sin_port = htons((uint16_t)atoi(ports + strlen("client_port=")));
    client->rtp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    local.sin_family = AF_INET;
    length = sizeof(local);
    if (client->rtp_fd < 0 ||
        bind(client->rtp_fd, (struct sockaddr *)&local, sizeof(local)) != 0 ||
        getsockname(client->rtp_fd, (struct sockaddr *)&local, &length) != 0) {
        return false;
    }
    *server_port = ntohs(local.sin_port);
    return true;
}

/* Handles one request; returns false to close the connection. */
static bool handle_request(media_video_rtsp *server, rtsp_client *client,
    const char *request, bool *request_key_frame)
{
    char method[16] = "";
    char url[256] = "";
    char cseq[16] = "0";
    char transport[256] = "";
    char headers[512];
    char body[512] = "";

    (void)sscanf(request, "%15s %255s", method, url);
    (void)header_value(request, "CSeq", cseq, sizeof(cseq));
    snprintf(headers, sizeof(headers), "Session: %08X;timeout=60\r\n", client->session);

    if (strcmp(method, "OPTIONS") == 0) {
        reply(client, "200 OK", cseq,
            "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER\r\n", "");
    } else if (strcmp(method, "DESCRIBE") == 0) {
        const char *path = strstr(url, "://");
        path = path == NULL ? url : strchr(path + 3, '/');
        if (path == NULL || strncmp(path + 1, server->path, strlen(server->path)) != 0) {
            reply(client, "404 Not Found", cseq, "", "");
            return true;
        }
        snprintf(body, sizeof(body),
            "v=0\r\no=- 0 0 IN IP4 0.0.0.0\r\ns=V610\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
            "m=video 0 RTP/AVP %d\r\na=rtpmap:%d H264/90000\r\n"
            "a=fmtp:%d packetization-mode=1\r\na=control:track0\r\n",
            RTP_PAYLOAD_TYPE, RTP_PAYLOAD_TYPE, RTP_PAYLOAD_TYPE);
        snprintf(headers, sizeof(headers),
            "Content-Base: %s/\r\nContent-Type: application/sdp\r\nContent-Length: %zu\r\n",
            url, strlen(body));
        reply(client, "200 OK", cseq, headers, body);
    } else if (strcmp(method, "SETUP") == 0) {
        uint16_t server_port = 0;
        (void)header_value(request, "Transport", transport, sizeof(transport));
        if (strstr(transport, "RTP/AVP/TCP") != NULL) {
            const char *interleaved = strstr(transport, "interleaved=");
            const int buffer = TCP_SEND_BUFFER;
            client->tcp = true;
            client->channel = interleaved == NULL ? 0 :
                (uint8_t)atoi(interleaved + strlen("interleaved="));
            (void)setsockopt(client->fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
            snprintf(headers + strlen(headers), sizeof(headers) - strlen(headers),
                "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\n",
                client->channel, client->channel + 1U);
        } else if (setup_udp(client, transport, &server_port)) {
            const unsigned port = ntohs(client->rtp_address.sin_port);
            snprintf(headers + strlen(headers), sizeof(headers) - strlen(headers),
                "Transport: RTP/AVP;unicast;client_port=%u-%u;server_port=%u-%u\r\n",
                port, port + 1U, server_port, server_port + 1U);
        } else {
            reply(client, "461 Unsupported Transport", cseq, "", "");
            return true;
        }
        reply(client, "200 OK", cseq, headers, "");
    } else if (strcmp(method, "PLAY") == 0) {
        client->playing = true;
        client->need_key = true;
        *request_key_frame = true;
        strncat(headers, "Range: npt=0.000-\r\n", sizeof(headers) - strlen(headers) - 1);
        reply(client, "200 OK", cseq, headers, "");
    } else if (strcmp(method, "TEARDOWN") == 0) {
        reply(client, "200 OK", cseq, headers, "");
        return false;
    } else if (strcmp(method, "GET_PARAMETER") == 0 || strcmp(method, "SET_PARAMETER") == 0) {
        reply(client, "200 OK", cseq, headers, "");
    } else {
        reply(client, "501 Not Implemented", cseq, "", "");
    }
    return true;
}

/*
 * Processes the buffered input of a client: complete requests, and RTCP
 * packets that clients interleave on the connection, which are skipped.
 */
static bool process_input(media_video_rtsp *server, rtsp_client *client,
    bool *request_key_frame)
{
    while (client->request_size > 0) {
        size_t used;
        if (client->request[0] == '$') {
            if (client->request_size < 4) {
                break;
            }
            used = 4 + (((size_t)(uint8_t)client->request[2] << 8) |
                (uint8_t)client->request[3]);
            if (used > client->request_size) {
                break;
            }
        } else {
            client->request[client->request_size] = '\0';
            const char *end = strstr(client->request, "\r\n\r\n");
            if (end == NULL) {
                return client->request_size < RTSP_REQUEST_SIZE - 1;
            }
            char length[16] = "0";
            (void)header_value(client->request, "Content-Length", length, sizeof(length));
            used = (size_t)(end + 4 - client->request) + (size_t)atoi(length);
            if (used > client->request_size) {
                break;
            }
            if (!handle_request(server, client, client->request, request_key_frame)) {
                return false;
            }
        }
        memmove(client->request, client->request + used, client->request_size - used);
        client->request_size -= used;
    }
    return true;
}

static void accept_client(media_video_rtsp *server)
{
    const int fd = accept(server->listen_fd, NULL, NULL);
    if (fd < 0) {
        return;
    }
    for (size_t index = 0; index < RTSP_MAX_CLIENTS; ++index) {
        rtsp_client *client = &server->clients[index];
        if (client->fd < 0) {
            client->fd = fd;
            client->ssrc = (uint32_t)rand();
            client->session = (uint32_t)rand();
            client->sequence = (uint16_t)rand();
            return;
        }
    }
    close(fd);
}

static void *server_thread(void *argument)
{
    media_video_rtsp *server = argument;
    while (atomic_load(&server->running)) {
        struct pollfd fds[1 + RTSP_MAX_CLIENTS] = {{.fd = server->listen_fd, .events = POLLIN}};
        bool request_key_frame = false;

        (void)pthread_mutex_lock(&server->lock);
        for (size_t index = 0; index < RTSP_MAX_CLIENTS; ++index) {
            if (server->clients[index].broken) {
                client_close(&server->clients[index]);
            }
            fds[1 + index] = (struct pollfd) {.fd = server->clients[index].fd, .events = POLLIN};
        }
        (void)pthread_mutex_unlock(&server->lock);

        if (poll(fds, 1 + RTSP_MAX_CLIENTS, 200) <= 0) {
            continue;
        }
        (void)pthread_mutex_lock(&server->lock);
        if (fds[0].revents & POLLIN) {
            accept_client(server);
        }
        for (size_t index = 0; index < RTSP_MAX_CLIENTS; ++index) {
            rtsp_client *client = &server->clients[index];
            if (fds[1 + index].fd < 0 || fds[1 + index].revents == 0 || client->fd < 0) {
                continue;
            }
            const ssize_t received = recv(client->fd, client->request + client->request_size,
                RTSP_REQUEST_SIZE - 1 - client->request_size, MSG_DONTWAIT);
            if (received <= 0) {
                if (received == 0 || (errno != EAGAIN && errno != EINTR)) {
                    client_close(client);
                }
                continue;
            }
            client->request_size += (size_t)received;
            if (!process_input(server, client, &request_key_frame)) {
                client_close(client);
            }
        }
        (void)pthread_mutex_unlock(&server->lock);

        if (request_key_frame && server->config.request_key_frame != NULL) {
            server->config.request_key_frame(server->config.user);
        }
    }
    return NULL;
}

media_video_rtsp_status media_video_rtsp_open(
    const media_video_rtsp_config *config, media_video_rtsp **output)
{
    if (config == NULL || output == NULL || config->port == 0 || config->path == NULL ||
        strlen(config->path) >= sizeof(((media_video_rtsp *)0)->path)) {
        return MEDIA_VIDEO_RTSP_ERR_ARGUMENT;
    }
    *output = NULL;
    media_video_rtsp *server = calloc(1, sizeof(*server));
    if (server == NULL) {
        return MEDIA_VIDEO_RTSP_ERR_MEMORY;
    }
    server->listen_fd = -1;
    server->config = *config;
    snprintf(server->path, sizeof(server->path), "%s", config->path);
    server->config.path = server->path;
    for (size_t index = 0; index < RTSP_MAX_CLIENTS; ++index) {
        server->clients[index] = (rtsp_client) {.fd = -1, .rtp_fd = -1};
    }
    (void)pthread_mutex_init(&server->lock, NULL);
    srand((unsigned)getpid());

    const int reuse = 1;
    const struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(config->port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listen_fd < 0 ||
        setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0 ||
        bind(server->listen_fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server->listen_fd, RTSP_MAX_CLIENTS) != 0) {
        media_video_rtsp_close(server);
        return MEDIA_VIDEO_RTSP_ERR_SOCKET;
    }
    atomic_store(&server->running, true);
    if (pthread_create(&server->thread, NULL, server_thread, server) != 0) {
        media_video_rtsp_close(server);
        return MEDIA_VIDEO_RTSP_ERR_MEMORY;
    }
    server->thread_started = true;
    *output = server;
    return MEDIA_VIDEO_RTSP_OK;
}

/* Sends one RTP packet whose header starts at packet + 4. */
static void send_rtp(media_video_rtsp *server, rtsp_client *client, size_t size)
{
    uint8_t *packet = server->packet;
    ssize_t sent;
    if (client->tcp) {
        packet[0] = '$';
        packet[1] = client->channel;
        packet[2] = (uint8_t)(size >> 8);
        packet[3] = (uint8_t)size;
        sent = send(client->fd, packet, size + 4, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent == (ssize_t)(size + 4)) {
            sent = (ssize_t)size;
        } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            client->need_key = true;   /* congested: resume at the next IDR */
            return;
        } else {
            client->broken = true;     /* a partial packet corrupts the stream */
            return;
        }
    } else {
        sent = sendto(client->rtp_fd, packet + 4, size, MSG_DONTWAIT,
            (const struct sockaddr *)&client->rtp_address, sizeof(client->rtp_address));
    }
    if (sent == (ssize_t)size) {
        server->stats.packets_sent++;
        server->stats.bytes_sent += size;
    }
}

static void send_nal(media_video_rtsp *server, rtsp_client *client, const uint8_t *nal,
    size_t size, bool last, uint32_t timestamp)
{
    uint8_t *rtp = server->packet + 4;
    const uint8_t header = nal[0];
    size_t offset = 1;
    bool first = true;

    /* Small NAL units go in one packet, larger ones are split into FU-A. */
    while (!client->need_key && !client->broken) {
        const bool single = first && size <= RTP_PAYLOAD_SIZE;
        const size_t chunk = single ? size :
            (size - offset < RTP_PAYLOAD_SIZE ? size - offset : RTP_PAYLOAD_SIZE);
        const bool end = single || offset + chunk == size;
        rtp[0] = 0x80;
        rtp[1] = (uint8_t)(((last && end) ? 0x80 : 0) | RTP_PAYLOAD_TYPE);
        rtp[2] = (uint8_t)(client->sequence >> 8);
        rtp[3] = (uint8_t)client->sequence;
        client->sequence++;
        for (int byte = 0; byte < 4; ++byte) {
            rtp[4 + byte] = (uint8_t)(timestamp >> (24 - 8 * byte));
            rtp[8 + byte] = (uint8_t)(client->ssrc >> (24 - 8 * byte));
        }
        if (single) {
            memcpy(rtp + RTP_HEADER_SIZE, nal, size);
            send_rtp(server, client, RTP_HEADER_SIZE + size);
            return;
        }
        rtp[RTP_HEADER_SIZE] = (uint8_t)((header & 0xe0) | 28);
        rtp[RTP_HEADER_SIZE + 1] = (uint8_t)((first ? 0x80 : 0) | (end ? 0x40 : 0) | (header & 0x1f));
        memcpy(rtp + RTP_HEADER_SIZE + 2, nal + offset, chunk);
        send_rtp(server, client, RTP_HEADER_SIZE + 2 + chunk);
        offset += chunk;
        first = false;
        if (end) {
            return;
        }
    }
}

media_video_rtsp_status media_video_rtsp_send_h264(media_video_rtsp *server,
    const void *data, size_t size, bool key_frame, uint64_t pts)
{
    if (server == NULL || data == NULL || size == 0) {
        return MEDIA_VIDEO_RTSP_ERR_ARGUMENT;
    }
    /* Split the Annex B access unit at its start codes. */
    const uint8_t *bytes = data;
    const uint8_t *nals[RTSP_MAX_NALS];
    size_t sizes[RTSP_MAX_NALS];
    size_t count = 0;
    for (size_t index = 0; index + 3 <= size && count < RTSP_MAX_NALS; ++index) {
        if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1) {
            if (count > 0) {
                const uint8_t *end = bytes + index;
                while (end > nals[count - 1] && end[-1] == 0) {
                    --end;
                }
                sizes[count - 1] = (size_t)(end - nals[count - 1]);
            }
            nals[count++] = bytes + index + 3;
            index += 2;
        }
    }
    if (count == 0) {
        return MEDIA_VIDEO_RTSP_ERR_ARGUMENT;
    }
    sizes[count - 1] = (size_t)(bytes + size - nals[count - 1]);

    /* RTP H.264 uses a 90 kHz clock; pts is in microseconds. */
    const uint32_t timestamp = (uint32_t)(pts * 90 / 1000);
    bool sent = false;
    (void)pthread_mutex_lock(&server->lock);
    for (size_t index = 0; index < RTSP_MAX_CLIENTS; ++index) {
        rtsp_client *client = &server->clients[index];
        if (client->fd < 0 || !client->playing || client->broken) {
            continue;
        }
        if (client->need_key && !key_frame) {
            continue;
        }
        client->need_key = false;
        for (size_t nal = 0; nal < count; ++nal) {
            if (sizes[nal] > 0) {
                send_nal(server, client, nals[nal], sizes[nal], nal + 1 == count, timestamp);
            }
        }
        sent = true;
    }
    if (sent) {
        server->stats.frames_sent++;
    }
    (void)pthread_mutex_unlock(&server->lock);
    return MEDIA_VIDEO_RTSP_OK;
}

void media_video_rtsp_get_stats(media_video_rtsp *server, media_video_rtsp_stats *stats)
{
    if (server == NULL || stats == NULL) {
        return;
    }
    (void)pthread_mutex_lock(&server->lock);
    *stats = server->stats;
    stats->clients = 0;
    for (size_t index = 0; index < RTSP_MAX_CLIENTS; ++index) {
        stats->clients += server->clients[index].fd >= 0 ? 1U : 0U;
    }
    (void)pthread_mutex_unlock(&server->lock);
}

void media_video_rtsp_close(media_video_rtsp *server)
{
    if (server == NULL) {
        return;
    }
    atomic_store(&server->running, false);
    if (server->thread_started) {
        (void)pthread_join(server->thread, NULL);
    }
    for (size_t index = 0; index < RTSP_MAX_CLIENTS; ++index) {
        client_close(&server->clients[index]);
    }
    if (server->listen_fd >= 0) {
        close(server->listen_fd);
    }
    (void)pthread_mutex_destroy(&server->lock);
    free(server);
}

const char *media_video_rtsp_status_string(media_video_rtsp_status status)
{
    switch (status) {
    case MEDIA_VIDEO_RTSP_OK:
        return "ok";
    case MEDIA_VIDEO_RTSP_ERR_ARGUMENT:
        return "invalid argument";
    case MEDIA_VIDEO_RTSP_ERR_MEMORY:
        return "out of memory";
    case MEDIA_VIDEO_RTSP_ERR_SOCKET:
        return "socket error";
    }
    return "unknown";
}
