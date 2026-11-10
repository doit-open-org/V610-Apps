#define _POSIX_C_SOURCE 200809L

#include "media_audio_udp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define AUDIO_UDP_MAGIC "XPAU"
#define AUDIO_UDP_VERSION 1U
#define AUDIO_UDP_OPUS_FORMAT 2U
#define AUDIO_UDP_HEADER_SIZE 24U
#define AUDIO_UDP_RECEIVE_BUFFER_SIZE (512 * 1024)

struct media_audio_udp {
    int send_socket_fd;
    int receive_socket_fd;
    struct sockaddr_in destination;
    media_audio_udp_config config;
    media_audio_udp_stats stats;
    uint32_t next_sequence;
};

static void put_u16(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value >> 8);
    buffer[1] = (uint8_t)value;
}

static void put_u32(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value >> 24);
    buffer[1] = (uint8_t)(value >> 16);
    buffer[2] = (uint8_t)(value >> 8);
    buffer[3] = (uint8_t)value;
}

void media_audio_udp_config_init(media_audio_udp_config *config)
{
    if (config == NULL) {
        return;
    }
    *config = (media_audio_udp_config) {
        .port = 9001,
        .receive_port = 0,
        .stream_id = 0,
        .sample_rate = 16000,
        .channels = 1,
        .frame_duration_ms = 20,
        .nonblocking = true,
    };
}

static uint16_t get_u16(const uint8_t *buffer)
{
    return (uint16_t)(((uint16_t)buffer[0] << 8) | buffer[1]);
}

static uint32_t get_u32(const uint8_t *buffer)
{
    return ((uint32_t)buffer[0] << 24) |
        ((uint32_t)buffer[1] << 16) |
        ((uint32_t)buffer[2] << 8) | buffer[3];
}

static bool set_nonblocking(int socket_fd)
{
    const int flags = fcntl(socket_fd, F_GETFL, 0);
    return flags >= 0 &&
        fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool open_receive_socket(media_audio_udp *sender)
{
    if (sender->config.receive_port == 0) {
        return true;
    }
    sender->receive_socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender->receive_socket_fd < 0) {
        return false;
    }
    const int enable = 1;
    const int receive_buffer_size = AUDIO_UDP_RECEIVE_BUFFER_SIZE;
    if (setsockopt(sender->receive_socket_fd, SOL_SOCKET, SO_REUSEADDR,
            &enable, sizeof(enable)) != 0 ||
        setsockopt(sender->receive_socket_fd, SOL_SOCKET, SO_RCVBUF,
            &receive_buffer_size, sizeof(receive_buffer_size)) != 0 ||
        (sender->config.nonblocking &&
            !set_nonblocking(sender->receive_socket_fd))) {
        return false;
    }
    const struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(sender->config.receive_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    return bind(sender->receive_socket_fd,
        (const struct sockaddr *)&local, sizeof(local)) == 0;
}

static bool config_valid(const media_audio_udp_config *config)
{
    return config != NULL && config->address != NULL &&
        config->address[0] != '\0' && config->port != 0 &&
        config->sample_rate != 0 && config->channels > 0 &&
        config->channels <= UINT8_MAX && config->frame_duration_ms > 0 &&
        config->frame_duration_ms <= UINT8_MAX;
}

media_audio_udp_status media_audio_udp_open(
    const media_audio_udp_config *config, media_audio_udp **output)
{
    if (!config_valid(config) || output == NULL) {
        return MEDIA_AUDIO_UDP_ERR_ARGUMENT;
    }
    *output = NULL;
    media_audio_udp *sender = calloc(1, sizeof(*sender));
    if (sender == NULL) {
        return MEDIA_AUDIO_UDP_ERR_MEMORY;
    }
    sender->send_socket_fd = -1;
    sender->receive_socket_fd = -1;
    sender->send_socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sender->send_socket_fd < 0) {
        media_audio_udp_close(sender);
        return MEDIA_AUDIO_UDP_ERR_SOCKET;
    }
    if (config->nonblocking && !set_nonblocking(sender->send_socket_fd)) {
        media_audio_udp_close(sender);
        return MEDIA_AUDIO_UDP_ERR_SOCKET;
    }
    sender->destination.sin_family = AF_INET;
    sender->destination.sin_port = htons(config->port);
    if (inet_pton(AF_INET, config->address,
            &sender->destination.sin_addr) != 1) {
        media_audio_udp_close(sender);
        return MEDIA_AUDIO_UDP_ERR_ARGUMENT;
    }
    sender->config = *config;
    if (!open_receive_socket(sender)) {
        media_audio_udp_close(sender);
        return MEDIA_AUDIO_UDP_ERR_SOCKET;
    }
    *output = sender;
    return MEDIA_AUDIO_UDP_OK;
}

media_audio_udp_status media_audio_udp_send_opus(media_audio_udp *sender,
    const void *data, size_t size, uint64_t pts)
{
    if (sender == NULL || data == NULL || size == 0 ||
        size > MEDIA_AUDIO_UDP_MAX_OPUS_BYTES || size > UINT16_MAX) {
        return MEDIA_AUDIO_UDP_ERR_ARGUMENT;
    }
    uint8_t packet[AUDIO_UDP_HEADER_SIZE + MEDIA_AUDIO_UDP_MAX_OPUS_BYTES];
    memcpy(packet, AUDIO_UDP_MAGIC, 4);
    packet[4] = AUDIO_UDP_VERSION;
    packet[5] = AUDIO_UDP_OPUS_FORMAT;
    put_u16(&packet[6], AUDIO_UDP_HEADER_SIZE);
    put_u32(&packet[8], sender->next_sequence++);
    put_u32(&packet[12], (uint32_t)pts);
    put_u32(&packet[16], sender->config.sample_rate);
    packet[20] = (uint8_t)sender->config.channels;
    packet[21] = (uint8_t)sender->config.frame_duration_ms;
    put_u16(&packet[22], (uint16_t)size);
    memcpy(packet + AUDIO_UDP_HEADER_SIZE, data, size);
    const size_t packet_size = AUDIO_UDP_HEADER_SIZE + size;
    const ssize_t sent = sendto(sender->send_socket_fd, packet, packet_size, 0,
        (const struct sockaddr *)&sender->destination,
        sizeof(sender->destination));
    if (sent == (ssize_t)packet_size) {
        sender->stats.frames_sent++;
        sender->stats.packets_sent++;
        sender->stats.bytes_sent += packet_size;
        return MEDIA_AUDIO_UDP_OK;
    }
    sender->stats.frames_dropped++;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
            errno == ENOBUFS)) {
        return MEDIA_AUDIO_UDP_AGAIN;
    }
    return MEDIA_AUDIO_UDP_ERR_SOCKET;
}

static media_audio_udp_status parse_opus_packet(media_audio_udp *sender,
    const uint8_t *packet, size_t packet_size, void *data, size_t capacity,
    size_t *size, media_audio_udp_packet_info *info)
{
    if (packet_size < AUDIO_UDP_HEADER_SIZE ||
        memcmp(packet, AUDIO_UDP_MAGIC, 4) != 0 ||
        packet[4] != AUDIO_UDP_VERSION ||
        packet[5] != AUDIO_UDP_OPUS_FORMAT ||
        get_u16(&packet[6]) != AUDIO_UDP_HEADER_SIZE) {
        sender->stats.packets_rejected++;
        return MEDIA_AUDIO_UDP_ERR_PACKET;
    }
    const size_t encoded_size = get_u16(&packet[22]);
    if (encoded_size == 0 || encoded_size > capacity ||
        encoded_size > MEDIA_AUDIO_UDP_MAX_OPUS_BYTES ||
        packet_size != AUDIO_UDP_HEADER_SIZE + encoded_size) {
        sender->stats.packets_rejected++;
        return MEDIA_AUDIO_UDP_ERR_PACKET;
    }
    memcpy(data, &packet[AUDIO_UDP_HEADER_SIZE], encoded_size);
    *size = encoded_size;
    *info = (media_audio_udp_packet_info) {
        .sequence = get_u32(&packet[8]),
        .pts = get_u32(&packet[12]),
        .sample_rate = get_u32(&packet[16]),
        .channels = packet[20],
        .frame_duration_ms = packet[21],
    };
    sender->stats.frames_received++;
    sender->stats.packets_received++;
    sender->stats.bytes_received += packet_size;
    return MEDIA_AUDIO_UDP_OK;
}

media_audio_udp_status media_audio_udp_receive_opus(media_audio_udp *sender,
    void *data, size_t capacity, size_t *size,
    media_audio_udp_packet_info *info)
{
    if (sender == NULL || sender->receive_socket_fd < 0 || data == NULL ||
        capacity == 0 || size == NULL || info == NULL) {
        return MEDIA_AUDIO_UDP_ERR_ARGUMENT;
    }
    *size = 0;
    uint8_t packet[AUDIO_UDP_HEADER_SIZE + MEDIA_AUDIO_UDP_MAX_OPUS_BYTES];
    const ssize_t received = recvfrom(sender->receive_socket_fd, packet,
        sizeof(packet), 0, NULL, NULL);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return MEDIA_AUDIO_UDP_AGAIN;
        }
        return MEDIA_AUDIO_UDP_ERR_SOCKET;
    }
    return parse_opus_packet(sender, packet, (size_t)received, data,
        capacity, size, info);
}

void media_audio_udp_get_stats(const media_audio_udp *sender,
    media_audio_udp_stats *stats)
{
    if (stats == NULL) {
        return;
    }
    *stats = sender == NULL ? (media_audio_udp_stats) {0} : sender->stats;
}

void media_audio_udp_close(media_audio_udp *sender)
{
    if (sender == NULL) {
        return;
    }
    if (sender->send_socket_fd >= 0) {
        (void)close(sender->send_socket_fd);
    }
    if (sender->receive_socket_fd >= 0) {
        (void)close(sender->receive_socket_fd);
    }
    free(sender);
}

const char *media_audio_udp_status_string(media_audio_udp_status status)
{
    switch (status) {
        case MEDIA_AUDIO_UDP_OK: return "success";
        case MEDIA_AUDIO_UDP_AGAIN: return "try again";
        case MEDIA_AUDIO_UDP_ERR_ARGUMENT: return "invalid argument";
        case MEDIA_AUDIO_UDP_ERR_MEMORY: return "out of memory";
        case MEDIA_AUDIO_UDP_ERR_SOCKET: return "socket error";
        case MEDIA_AUDIO_UDP_ERR_PACKET: return "invalid packet";
        default: return "unknown error";
    }
}
