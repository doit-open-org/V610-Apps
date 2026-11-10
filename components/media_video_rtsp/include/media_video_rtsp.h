#ifndef MEDIA_VIDEO_RTSP_H
#define MEDIA_VIDEO_RTSP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MEDIA_VIDEO_RTSP_OK = 0,
    MEDIA_VIDEO_RTSP_ERR_ARGUMENT = -1,
    MEDIA_VIDEO_RTSP_ERR_MEMORY = -2,
    MEDIA_VIDEO_RTSP_ERR_SOCKET = -3,
} media_video_rtsp_status;

typedef struct {
    /* TCP port of the server; the stream is rtsp://<board>:<port>/<path>. */
    uint16_t port;
    const char *path;
    /*
     * Called from the server thread when a client starts playing, so the
     * encoder can send an IDR frame instead of the client waiting a GOP.
     */
    void (*request_key_frame)(void *user);
    void *user;
} media_video_rtsp_config;

typedef struct {
    uint64_t frames_sent;
    uint64_t packets_sent;
    uint64_t bytes_sent;
    uint32_t clients;
} media_video_rtsp_stats;

typedef struct media_video_rtsp media_video_rtsp;

void media_video_rtsp_config_init(media_video_rtsp_config *config);
media_video_rtsp_status media_video_rtsp_open(
    const media_video_rtsp_config *config, media_video_rtsp **server);
/* Sends one Annex B access unit to every playing client. */
media_video_rtsp_status media_video_rtsp_send_h264(media_video_rtsp *server,
    const void *data, size_t size, bool key_frame, uint64_t pts);
void media_video_rtsp_get_stats(media_video_rtsp *server,
    media_video_rtsp_stats *stats);
void media_video_rtsp_close(media_video_rtsp *server);
const char *media_video_rtsp_status_string(media_video_rtsp_status status);

#ifdef __cplusplus
}
#endif
#endif
