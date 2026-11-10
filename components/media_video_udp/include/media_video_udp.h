#ifndef MEDIA_VIDEO_UDP_H
#define MEDIA_VIDEO_UDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_VIDEO_UDP_OK = 0,
    MEDIA_VIDEO_UDP_AGAIN = 1,
    MEDIA_VIDEO_UDP_ERR_ARGUMENT = -1,
    MEDIA_VIDEO_UDP_ERR_MEMORY = -2,
    MEDIA_VIDEO_UDP_ERR_SOCKET = -3,
} media_video_udp_status;

typedef struct {
    const char *address;
    uint16_t port;
    uint32_t stream_id;
    uint32_t width;
    uint32_t height;
    size_t mtu;
    bool nonblocking;
} media_video_udp_config;

typedef struct {
    uint64_t frames_sent;
    uint64_t frames_dropped;
    uint64_t packets_sent;
    uint64_t bytes_sent;
} media_video_udp_stats;

typedef struct media_video_udp media_video_udp;

void media_video_udp_config_init(media_video_udp_config *config);
media_video_udp_status media_video_udp_open(
    const media_video_udp_config *config, media_video_udp **sender);
media_video_udp_status media_video_udp_send_h264(media_video_udp *sender,
    const void *data, size_t size, bool key_frame, uint64_t pts);
void media_video_udp_get_stats(const media_video_udp *sender,
    media_video_udp_stats *stats);
void media_video_udp_close(media_video_udp *sender);
const char *media_video_udp_status_string(media_video_udp_status status);

#ifdef __cplusplus
}
#endif
#endif
