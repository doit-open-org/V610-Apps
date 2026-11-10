#define _POSIX_C_SOURCE 200809L

#include "media_video_udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define VIDEO_UDP_MAGIC 0x4d445331U
#define VIDEO_UDP_DEFAULT_MTU 1472U
#define VIDEO_UDP_MIN_MTU 256U
#define VIDEO_UDP_KEY_FRAME 1U
#define VIDEO_UDP_H264_FORMAT 5U

typedef struct {
    uint32_t magic;
    uint32_t channel_id;
    uint32_t frame_id;
    uint32_t sequence;
    uint32_t packet_count;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t pts;
    uint32_t flags;
    uint32_t payload_size;
} video_packet_header;

struct media_video_udp {
    int socket_fd;
    struct sockaddr_in destination;
    media_video_udp_config config;
    media_video_udp_stats stats;
    uint32_t next_frame_id;
    uint8_t *packet;
};

void media_video_udp_config_init(media_video_udp_config *config)
{
    if (config == NULL) {
        return;
    }
    *config = (media_video_udp_config) {
        .port = 9000,
        .mtu = VIDEO_UDP_DEFAULT_MTU,
        .nonblocking = true,
    };
}

static bool config_valid(const media_video_udp_config *config)
{
    return config != NULL && config->address != NULL &&
        config->address[0] != '\0' && config->port != 0 &&
        config->width != 0 && config->height != 0 &&
        config->mtu >= VIDEO_UDP_MIN_MTU &&
        config->mtu <= UINT16_MAX &&
        config->mtu > sizeof(video_packet_header);
}

media_video_udp_status media_video_udp_open(
    const media_video_udp_config *config, media_video_udp **output)
{
    if (!config_valid(config) || output == NULL) {
        return MEDIA_VIDEO_UDP_ERR_ARGUMENT;
    }
    *output = NULL;
    media_video_udp *sender = calloc(1, sizeof(*sender));
    if (sender == NULL) {
        return MEDIA_VIDEO_UDP_ERR_MEMORY;
    }
    sender->socket_fd = -1;
    sender->packet = malloc(config->mtu);
    if (sender->packet == NULL) {
        media_video_udp_close(sender);
        return MEDIA_VIDEO_UDP_ERR_MEMORY;
    }
    sender->socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender->socket_fd < 0) {
        media_video_udp_close(sender);
        return MEDIA_VIDEO_UDP_ERR_SOCKET;
    }
    if (config->nonblocking) {
        const int flags = fcntl(sender->socket_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(sender->socket_fd, F_SETFL,
                flags | O_NONBLOCK) != 0) {
            media_video_udp_close(sender);
            return MEDIA_VIDEO_UDP_ERR_SOCKET;
        }
    }
    sender->destination.sin_family = AF_INET;
    sender->destination.sin_port = htons(config->port);
    if (inet_pton(AF_INET, config->address,
            &sender->destination.sin_addr) != 1) {
        media_video_udp_close(sender);
        return MEDIA_VIDEO_UDP_ERR_ARGUMENT;
    }
    sender->config = *config;
    *output = sender;
    return MEDIA_VIDEO_UDP_OK;
}

static media_video_udp_status send_packet(media_video_udp *sender,
    size_t packet_size)
{
    const ssize_t sent = sendto(sender->socket_fd, sender->packet,
        packet_size, 0, (const struct sockaddr *)&sender->destination,
        sizeof(sender->destination));
    if (sent == (ssize_t)packet_size) {
        sender->stats.packets_sent++;
        sender->stats.bytes_sent += packet_size;
        return MEDIA_VIDEO_UDP_OK;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == ENOBUFS)) {
        return MEDIA_VIDEO_UDP_AGAIN;
    }
    return MEDIA_VIDEO_UDP_ERR_SOCKET;
}

media_video_udp_status media_video_udp_send_h264(media_video_udp *sender,
    const void *data, size_t size, bool key_frame, uint64_t pts)
{
    if (sender == NULL || data == NULL || size == 0) {
        return MEDIA_VIDEO_UDP_ERR_ARGUMENT;
    }
    const size_t max_payload = sender->config.mtu -
        sizeof(video_packet_header);
    const size_t packet_count_size = (size - 1U) / max_payload + 1U;
    if (packet_count_size > UINT32_MAX) {
        return MEDIA_VIDEO_UDP_ERR_ARGUMENT;
    }
    const uint32_t packet_count = (uint32_t)packet_count_size;
    const uint32_t frame_id = ++sender->next_frame_id;
    const uint8_t *bytes = data;
    for (uint32_t sequence = 0; sequence < packet_count; ++sequence) {
        const size_t offset = (size_t)sequence * max_payload;
        const size_t remaining = size - offset;
        const size_t payload_size = remaining < max_payload ?
            remaining : max_payload;
        const video_packet_header header = {
            .magic = VIDEO_UDP_MAGIC,
            .channel_id = sender->config.stream_id,
            .frame_id = frame_id,
            .sequence = sequence,
            .packet_count = packet_count,
            .format = VIDEO_UDP_H264_FORMAT,
            .width = sender->config.width,
            .height = sender->config.height,
            .pts = pts,
            .flags = key_frame ? VIDEO_UDP_KEY_FRAME : 0U,
            .payload_size = (uint32_t)payload_size,
        };
        memcpy(sender->packet, &header, sizeof(header));
        memcpy(sender->packet + sizeof(header), bytes + offset, payload_size);
        const media_video_udp_status status = send_packet(sender,
            sizeof(header) + payload_size);
        if (status != MEDIA_VIDEO_UDP_OK) {
            sender->stats.frames_dropped++;
            return status;
        }
    }
    sender->stats.frames_sent++;
    return MEDIA_VIDEO_UDP_OK;
}

void media_video_udp_get_stats(const media_video_udp *sender,
    media_video_udp_stats *stats)
{
    if (stats == NULL) {
        return;
    }
    *stats = sender == NULL ? (media_video_udp_stats) {0} : sender->stats;
}

void media_video_udp_close(media_video_udp *sender)
{
    if (sender == NULL) {
        return;
    }
    if (sender->socket_fd >= 0) {
        (void)close(sender->socket_fd);
    }
    free(sender->packet);
    free(sender);
}

const char *media_video_udp_status_string(media_video_udp_status status)
{
    switch (status) {
        case MEDIA_VIDEO_UDP_OK: return "success";
        case MEDIA_VIDEO_UDP_AGAIN: return "packet dropped";
        case MEDIA_VIDEO_UDP_ERR_ARGUMENT: return "invalid argument";
        case MEDIA_VIDEO_UDP_ERR_MEMORY: return "out of memory";
        case MEDIA_VIDEO_UDP_ERR_SOCKET: return "socket error";
        default: return "unknown error";
    }
}
