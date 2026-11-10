#define _POSIX_C_SOURCE 200809L

#include "hifw_config.h"
#include "media_video_rtsp.h"
#include "media_video_udp.h"
#include "mipk_video.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define RECOGNITION_STREAM_ID 0U
#define H264_STREAM_ID 1U
#define VIDEO_FRAME_RATE 30U
#define H264_WIDTH 1920U
#define H264_HEIGHT 1080U
#define H264_BIT_RATE_KBPS 2500U
#define H264_GOP (VIDEO_FRAME_RATE * 2U)
#define OVERRIDES_POLL_MS 500U

typedef struct {
    const char *model_path;
    const char *overrides_path;
    const char *host;
    uint32_t camera_id;
    uint16_t port;
    uint16_t rtsp_port;
    uint64_t frame_limit;
    uint64_t report_interval;
} app_options;

typedef struct {
    media_context *media;
    media_video_udp *udp;
    media_video_rtsp *rtsp;
    bool read_failed;
    uint64_t send_warnings;
} stream_context;

typedef struct {
    const char *path;
    dev_t device;
    ino_t inode;
    off_t size;
    struct timespec modified;
    uint64_t last_check_ms;
    bool have_file;
    bool missing_reported;
} overrides_watcher;

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    g_running = 0;
}

static uint64_t monotonic_ms(void)
{
    struct timespec now = {0};
    return clock_gettime(CLOCK_MONOTONIC, &now) == 0 ?
        (uint64_t)now.tv_sec * UINT64_C(1000) +
            (uint64_t)now.tv_nsec / UINT64_C(1000000) : 0;
}

static void check_overrides(overrides_watcher *watcher, mipk_video *video,
    bool force)
{
    if (watcher->path == NULL) return;
    const uint64_t now_ms = monotonic_ms();
    if (!force && watcher->last_check_ms != 0 &&
        now_ms - watcher->last_check_ms < OVERRIDES_POLL_MS) return;
    watcher->last_check_ms = now_ms;
    struct stat attributes;
    if (stat(watcher->path, &attributes) != 0 ||
        !S_ISREG(attributes.st_mode)) {
        if (!watcher->missing_reported) {
            fprintf(stderr, "MIPK overrides unavailable: %s\n",
                watcher->path);
            watcher->missing_reported = true;
        }
        watcher->have_file = false;
        return;
    }
    watcher->missing_reported = false;
    const bool changed = !watcher->have_file ||
        watcher->device != attributes.st_dev ||
        watcher->inode != attributes.st_ino ||
        watcher->size != attributes.st_size ||
        watcher->modified.tv_sec != attributes.st_mtim.tv_sec ||
        watcher->modified.tv_nsec != attributes.st_mtim.tv_nsec;
    if (!changed) return;
    watcher->device = attributes.st_dev;
    watcher->inode = attributes.st_ino;
    watcher->size = attributes.st_size;
    watcher->modified = attributes.st_mtim;
    watcher->have_file = true;
    const mipk_status status = mipk_video_load_overrides(video, watcher->path);
    if (status == MIPK_OK) {
        printf("MIPK overrides loaded: %s\n", watcher->path);
    } else {
        fprintf(stderr, "MIPK overrides rejected: %s: %s\n",
            watcher->path, mipk_status_string(status));
    }
}

static bool parse_positive_u64(const char *text, uint64_t *value)
{
    if (text == NULL || value == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || end[0] != '\0' || parsed == 0) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
        "usage: %s [--model MODEL.mipk] [--overrides OPTIONS.json] "
        "[--camera 0|1] [--host IPv4] [--port N] [--rtsp PORT] [--frames N] "
        "[--report-interval N]\n",
        program);
}

static int parse_options(int argc, char **argv, app_options *options)
{
    *options = (app_options) {
        .model_path = CONFIG_MIPK_OBJECT_DETECTION_MODEL_PATH,
        .host = CONFIG_MIPK_OBJECT_DETECTION_HOST,
        .camera_id = 0,
        .port = CONFIG_MIPK_OBJECT_DETECTION_PORT,
        .report_interval = CONFIG_MIPK_OBJECT_DETECTION_REPORT_INTERVAL,
    };
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 1;
        }
        if (++index >= argc) {
            fprintf(stderr, "missing value for %s\n", argument);
            return -1;
        }
        if (strcmp(argument, "--model") == 0) {
            options->model_path = argv[index];
        } else if (strcmp(argument, "--overrides") == 0) {
            options->overrides_path = argv[index];
        } else if (strcmp(argument, "--camera") == 0) {
            if (strcmp(argv[index], "0") != 0 && strcmp(argv[index], "1") != 0) {
                fprintf(stderr, "invalid camera: %s (expected 0 or 1)\n", argv[index]);
                return -1;
            }
            options->camera_id = (uint32_t)(argv[index][0] - '0');
        } else if (strcmp(argument, "--host") == 0) {
            options->host = argv[index];
        } else if (strcmp(argument, "--port") == 0) {
            uint64_t port = 0;
            if (!parse_positive_u64(argv[index], &port) || port > UINT16_MAX) {
                return -1;
            }
            options->port = (uint16_t)port;
        } else if (strcmp(argument, "--rtsp") == 0) {
            uint64_t port = 0;
            if (!parse_positive_u64(argv[index], &port) || port > UINT16_MAX) {
                return -1;
            }
            options->rtsp_port = (uint16_t)port;
        } else if (strcmp(argument, "--frames") == 0) {
            if (!parse_positive_u64(argv[index], &options->frame_limit)) {
                return -1;
            }
        } else if (strcmp(argument, "--report-interval") == 0) {
            if (!parse_positive_u64(argv[index], &options->report_interval)) {
                return -1;
            }
        } else {
            fprintf(stderr, "unknown option: %s\n", argument);
            return -1;
        }
    }
    return 0;
}

/* Called by the RTSP server when a client starts playing. */
static void request_key_frame(void *user)
{
    stream_context *context = user;
    if (context->media != NULL) {
        (void)media_video_request_key_frame(context->media, H264_STREAM_ID);
    }
}

static void *h264_worker(void *argument)
{
    stream_context *context = argument;
    while (g_running) {
        media_video_packet packet = {0};
        const media_status media_result = media_video_read_packet(
            context->media, H264_STREAM_ID, 100, &packet);
        if (media_result == MEDIA_ERR_TIMEOUT || media_result == MEDIA_AGAIN) {
            continue;
        }
        if (media_result != MEDIA_OK) {
            fprintf(stderr, "H264 capture failed: %s\n",
                media_status_string(media_result));
            context->read_failed = true;
            g_running = 0;
            break;
        }
        if (context->rtsp != NULL) {
            (void)media_video_rtsp_send_h264(context->rtsp, packet.data,
                packet.size, packet.key_frame, packet.pts);
            media_video_packet_release(&packet);
            continue;
        }
        const media_video_udp_status udp_result = media_video_udp_send_h264(
            context->udp, packet.data, packet.size, packet.key_frame,
            packet.pts);
        media_video_packet_release(&packet);
        if (udp_result != MEDIA_VIDEO_UDP_OK) {
            ++context->send_warnings;
            if (context->send_warnings == 1 ||
                context->send_warnings % 100 == 0) {
                fprintf(stderr, "H264 UDP drop: %s (count=%llu)\n",
                    media_video_udp_status_string(udp_result),
                    (unsigned long long)context->send_warnings);
            }
        }
    }
    return NULL;
}

static void print_detections(const mipk_video_event *event)
{
    const size_t count = mipk_output_result_count(event->output);
    for (size_t index = 0; index < count; ++index) {
        const mipk_result *result = mipk_output_result_at(event->output, index);
        if (result->type != MIPK_RESULT_DETECTION &&
            result->type != MIPK_RESULT_TRACK) {
            continue;
        }
        const char *name = mipk_pipeline_label_name(event->pipeline,
            result->label_set_id, result->class_id);
        const mipk_bbox *bbox = result->type == MIPK_RESULT_TRACK ?
            &result->value.track.bbox : &result->value.bbox;
        printf("  class=%d(%s) score=%.4f box=[%.1f,%.1f,%.1f,%.1f]",
            result->class_id, name == NULL ? "?" : name, result->score,
            bbox->x1, bbox->y1, bbox->x2, bbox->y2);
        /* Only a package with a tracking stage produces track IDs. */
        if (result->type == MIPK_RESULT_TRACK) {
            printf(" track=%lld", (long long)result->track_id);
        }
        printf("\n");
    }
}

static int run_detection(const app_options *options)
{
    int exit_code = EXIT_FAILURE;
    mipk_video *video = NULL;
    media_context *media = NULL;
    media_video_udp *udp = NULL;
    media_video_rtsp *rtsp = NULL;
    pthread_t thread;
    bool thread_started = false;
    stream_context stream = {0};
    mipk_video_config video_config;
    mipk_video_config_init(&video_config);
    video_config.package_path = options->model_path;
    mipk_video_status video_result = mipk_video_open(&video_config, &video);
    if (video_result != MIPK_VIDEO_OK) {
        fprintf(stderr, "open video model failed: %s\n",
            mipk_video_status_string(video_result));
        goto cleanup;
    }
    const mipk_pipeline_info *info = mipk_video_get_pipeline_info(video);
    overrides_watcher watcher = {.path = options->overrides_path};
    check_overrides(&watcher, video, true);

    if (options->rtsp_port != 0) {
        media_video_rtsp_config rtsp_config;
        media_video_rtsp_config_init(&rtsp_config);
        rtsp_config.port = options->rtsp_port;
        rtsp_config.request_key_frame = request_key_frame;
        rtsp_config.user = &stream;
        const media_video_rtsp_status rtsp_result =
            media_video_rtsp_open(&rtsp_config, &rtsp);
        if (rtsp_result != MEDIA_VIDEO_RTSP_OK) {
            fprintf(stderr, "open RTSP server failed: %s\n",
                media_video_rtsp_status_string(rtsp_result));
            goto cleanup;
        }
    }
    media_video_udp_config udp_config;
    media_video_udp_config_init(&udp_config);
    udp_config.address = options->host;
    udp_config.port = options->port;
    udp_config.width = H264_WIDTH;
    udp_config.height = H264_HEIGHT;
    media_video_udp_status udp_result = options->rtsp_port != 0 ?
        MEDIA_VIDEO_UDP_OK : media_video_udp_open(&udp_config, &udp);
    if (udp_result != MEDIA_VIDEO_UDP_OK) {
        fprintf(stderr, "open H264 UDP failed: %s\n",
            media_video_udp_status_string(udp_result));
        goto cleanup;
    }

    media_config media_config_value;
    media_config_init(&media_config_value);
    media_video_config_init(&media_config_value.video);
    media_config_value.video.cameras[0] =
        media_config_value.video.cameras[options->camera_id];
    /* The board wires camera N's sensor to I2C bus N. */
    media_config_value.video.cameras[0].i2c.bus_id = options->camera_id;
    media_config_value.video.camera_count = 1;
    media_config_value.video.cameras[0].frame_rate = VIDEO_FRAME_RATE;
    media_config_value.video.stream_count = 2;
    video_result = mipk_video_configure_stream(video,
        &media_config_value.video.streams[RECOGNITION_STREAM_ID], 0,
        VIDEO_FRAME_RATE, MEDIA_VIDEO_OUTPUT_FRAME);
    if (video_result != MIPK_VIDEO_OK) {
        goto cleanup;
    }
    media_config_value.video.streams[H264_STREAM_ID] =
        media_config_value.video.streams[RECOGNITION_STREAM_ID];
    media_config_value.video.streams[H264_STREAM_ID].output =
        MEDIA_VIDEO_OUTPUT_H264;
    media_config_value.video.streams[H264_STREAM_ID].width = H264_WIDTH;
    media_config_value.video.streams[H264_STREAM_ID].height = H264_HEIGHT;
    media_config_value.video.streams[H264_STREAM_ID].bit_rate_kbps =
        H264_BIT_RATE_KBPS;
    media_config_value.video.streams[H264_STREAM_ID].gop = H264_GOP;
    media_status media_result = media_engine_open(&media_config_value, &media);
    if (media_result == MEDIA_OK) {
        media_result = media_video_open(media);
    }
    if (media_result != MEDIA_OK) {
        fprintf(stderr, "start camera failed: %s\n",
            media_status_string(media_result));
        goto cleanup;
    }

    stream.media = media;
    stream.udp = udp;
    stream.rtsp = rtsp;
    if (pthread_create(&thread, NULL, h264_worker, &stream) != 0) {
        fprintf(stderr, "start H264 streaming thread failed\n");
        goto cleanup;
    }
    thread_started = true;
    char output[80];
    if (rtsp != NULL) {
        snprintf(output, sizeof(output), "RTSP=rtsp://<board>:%u/live",
            (unsigned)options->rtsp_port);
    } else {
        snprintf(output, sizeof(output), "UDP=%s:%u", options->host,
            (unsigned)options->port);
    }
    printf("MIPK object detection: camera=%u model=%s/%s input=NV12/%ux%u "
        "H264=%ux%u %s\n",
        options->camera_id,
        info->name, info->version, info->input_width, info->input_height,
        H264_WIDTH, H264_HEIGHT, output);

    uint64_t completed = 0;
    exit_code = EXIT_SUCCESS;
    while (g_running &&
        (options->frame_limit == 0 || completed < options->frame_limit)) {
        check_overrides(&watcher, video, false);
        mipk_video_event event;
        video_result = mipk_video_process(video, media,
            RECOGNITION_STREAM_ID, 100, &event);
        if (video_result == MIPK_VIDEO_AGAIN) {
            continue;
        }
        if (video_result != MIPK_VIDEO_OK) {
            fprintf(stderr, "video inference failed: %s\n",
                mipk_video_status_string(video_result));
            exit_code = EXIT_FAILURE;
            break;
        }
        media_video_box boxes[MEDIA_VIDEO_MAX_BOXES];
        const size_t box_count = mipk_video_results_to_boxes(&event, boxes,
            MEDIA_VIDEO_MAX_BOXES);
        media_result = media_video_set_boxes(media, H264_STREAM_ID,
            boxes, box_count);
        if (media_result != MEDIA_OK) {
            fprintf(stderr, "set video overlay failed: %s\n",
                media_status_string(media_result));
            exit_code = EXIT_FAILURE;
            break;
        }
        ++completed;
        if (completed == 1 || completed % options->report_interval == 0 ||
            (options->frame_limit != 0 && completed == options->frame_limit)) {
            printf("frame=%llu results=%zu inference_ms=%.3f\n",
                (unsigned long long)completed,
                mipk_output_result_count(event.output),
                event.inference_ms);
            print_detections(&event);
        }
    }

cleanup:
    g_running = 0;
    if (thread_started) {
        (void)pthread_join(thread, NULL);
    }
    if (stream.read_failed) {
        exit_code = EXIT_FAILURE;
    }
    media_video_udp_stats stats;
    media_video_udp_get_stats(udp, &stats);
    if (udp != NULL) {
        printf("UDP summary: sent=%llu dropped=%llu packets=%llu\n",
            (unsigned long long)stats.frames_sent,
            (unsigned long long)stats.frames_dropped,
            (unsigned long long)stats.packets_sent);
    }
    if (rtsp != NULL) {
        media_video_rtsp_stats stats;
        media_video_rtsp_get_stats(rtsp, &stats);
        printf("RTSP summary: sent=%llu packets=%llu\n",
            (unsigned long long)stats.frames_sent,
            (unsigned long long)stats.packets_sent);
    }
    /* The RTSP server calls back into media; stop it first. */
    media_video_rtsp_close(rtsp);
    if (media != NULL) {
        (void)media_video_close(media);
        media_engine_close(media);
    }
    media_video_udp_close(udp);
    mipk_video_close(video);
    return exit_code;
}

int main(int argc, char **argv)
{
    app_options options;
    const int parsed = parse_options(argc, argv, &options);
    if (parsed != 0) {
        return parsed > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    if (signal(SIGINT, handle_signal) == SIG_ERR ||
        signal(SIGTERM, handle_signal) == SIG_ERR) {
        fprintf(stderr, "cannot install signal handlers\n");
        return EXIT_FAILURE;
    }
    return run_detection(&options);
}
