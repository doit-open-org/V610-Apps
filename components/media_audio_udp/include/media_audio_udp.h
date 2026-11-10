#ifndef MEDIA_AUDIO_UDP_H
#define MEDIA_AUDIO_UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEDIA_AUDIO_UDP_MAX_OPUS_BYTES 1275U

typedef enum {
    MEDIA_AUDIO_UDP_OK = 0,
    MEDIA_AUDIO_UDP_AGAIN = 1,
    MEDIA_AUDIO_UDP_ERR_ARGUMENT = -1,
    MEDIA_AUDIO_UDP_ERR_MEMORY = -2,
    MEDIA_AUDIO_UDP_ERR_SOCKET = -3,
    MEDIA_AUDIO_UDP_ERR_PACKET = -4,
} media_audio_udp_status;

typedef struct {
    const char *address;
    uint16_t port;
    uint16_t receive_port;
    uint32_t stream_id;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frame_duration_ms;
    bool nonblocking;
} media_audio_udp_config;

typedef struct {
    uint64_t frames_sent;
    uint64_t frames_dropped;
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint64_t frames_received;
    uint64_t packets_received;
    uint64_t bytes_received;
    uint64_t packets_rejected;
} media_audio_udp_stats;

typedef struct {
    uint32_t sequence;
    uint32_t pts;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t frame_duration_ms;
} media_audio_udp_packet_info;

typedef struct media_audio_udp media_audio_udp;

void media_audio_udp_config_init(media_audio_udp_config *config);
media_audio_udp_status media_audio_udp_open(
    const media_audio_udp_config *config, media_audio_udp **sender);
media_audio_udp_status media_audio_udp_send_opus(media_audio_udp *sender,
    const void *data, size_t size, uint64_t pts);
media_audio_udp_status media_audio_udp_receive_opus(media_audio_udp *sender,
    void *data, size_t capacity, size_t *size,
    media_audio_udp_packet_info *info);
void media_audio_udp_get_stats(const media_audio_udp *sender,
    media_audio_udp_stats *stats);
void media_audio_udp_close(media_audio_udp *sender);
const char *media_audio_udp_status_string(media_audio_udp_status status);

#ifdef __cplusplus
}
#endif
#endif
