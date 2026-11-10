#define _POSIX_C_SOURCE 200809L

#include "keypoints.h"
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
#define OVERRIDES_POLL_MS 500U
#define KEYPOINT_THRESHOLD \
    ((float)CONFIG_MIPK_KEYPOINTS_KEYPOINT_PERCENT / 100.0F)

typedef struct {
    const char *model_path;
    const char *overrides_path;
    /* Where to write one raw recognition frame, for checking geometry. */
    const char *dump_frame_path;
    /* Skeleton file describing which joints to join; must suit the model. */
    const char *layout_path;
    const char *host;
    uint32_t camera_id;
    uint16_t port;
    uint16_t rtsp_port;
    uint64_t frame_limit;
    uint64_t report_interval;
    uint32_t h264_width;
    uint32_t h264_height;
    uint32_t gop;
    uint32_t bitrate_kbps;
    bool trace;
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
            fprintf(stderr, "MIPK overrides unavailable: %s\n", watcher->path);
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
        fprintf(stderr, "MIPK overrides rejected: %s: %s\n", watcher->path,
            mipk_status_string(status));
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

/* Ports accept 0 to mean "disabled", which parse_positive_u64 rejects. */
static bool parse_port(const char *text, uint16_t *port)
{
    if (text == NULL || text[0] == '\0' || text[0] == '-') return false;
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || end[0] != '\0' || parsed > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)parsed;
    return true;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
        "usage: %s [--model MODEL.mipk] [--keypoints FILE.keypoints] "
        "[--overrides OPTIONS.json] "
        "[--camera 0|1] [--host IPv4] [--port N] [--rtsp PORT] [--frames N] "
        "[--report-interval N] [--h264 WxH] [--gop N] [--bitrate KBPS] "
        "[--dump-frame FILE] [--trace]\n",
        program);
    fprintf(stream,
        "\nWithout --keypoints every point is marked where it lies in one\n"
        "colour, which suits any keypoint model. A keypoints file says which\n"
        "points to draw, in what colour and size, and which of them to join\n"
        "into a pose; see the ones shipped beside this program.\n");
}

static int parse_options(int argc, char **argv, app_options *options)
{
    *options = (app_options) {
        .model_path = CONFIG_MIPK_KEYPOINTS_MODEL_PATH,
        .layout_path = CONFIG_MIPK_KEYPOINTS_LAYOUT,
        .host = CONFIG_MIPK_KEYPOINTS_HOST,
        .camera_id = 0,
        .port = CONFIG_MIPK_KEYPOINTS_PORT,
        .report_interval = CONFIG_MIPK_KEYPOINTS_REPORT_INTERVAL,
        .h264_width = H264_WIDTH,
        .h264_height = H264_HEIGHT,
        .gop = CONFIG_MIPK_KEYPOINTS_GOP,
        .bitrate_kbps = H264_BIT_RATE_KBPS,
    };
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 1;
        }
        if (strcmp(argument, "--trace") == 0) {
            options->trace = true;
            continue;
        }
        if (++index >= argc) {
            fprintf(stderr, "missing value for %s\n", argument);
            return -1;
        }
        const char *value = argv[index];
        if (strcmp(argument, "--model") == 0) {
            options->model_path = value;
        } else if (strcmp(argument, "--overrides") == 0) {
            options->overrides_path = value;
        } else if (strcmp(argument, "--dump-frame") == 0) {
            options->dump_frame_path = value;
        } else if (strcmp(argument, "--keypoints") == 0) {
            options->layout_path = value;
        } else if (strcmp(argument, "--camera") == 0) {
            if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
                fprintf(stderr, "invalid camera: %s (expected 0 or 1)\n",
                    value);
                return -1;
            }
            options->camera_id = (uint32_t)(value[0] - '0');
        } else if (strcmp(argument, "--host") == 0) {
            options->host = value;
        } else if (strcmp(argument, "--port") == 0) {
            if (!parse_port(value, &options->port)) {
                fprintf(stderr, "invalid --port: %s\n", value);
                return -1;
            }
        } else if (strcmp(argument, "--rtsp") == 0) {
            if (!parse_port(value, &options->rtsp_port)) {
                fprintf(stderr, "invalid --rtsp: %s\n", value);
                return -1;
            }
        } else if (strcmp(argument, "--frames") == 0) {
            if (!parse_positive_u64(value, &options->frame_limit)) {
                fprintf(stderr, "invalid --frames: %s\n", value);
                return -1;
            }
        } else if (strcmp(argument, "--h264") == 0) {
            unsigned int width = 0;
            unsigned int height = 0;
            if (sscanf(value, "%ux%u", &width, &height) != 2 ||
                width < 160U || height < 120U || width > 1920U ||
                height > 1080U || (width & 1U) != 0U || (height & 1U) != 0U) {
                fprintf(stderr, "invalid --h264 size: %s\n", value);
                return -1;
            }
            options->h264_width = width;
            options->h264_height = height;
        } else if (strcmp(argument, "--gop") == 0) {
            uint64_t gop = 0;
            if (!parse_positive_u64(value, &gop) || gop > 600) {
                fprintf(stderr, "invalid --gop: %s\n", value);
                return -1;
            }
            options->gop = (uint32_t)gop;
        } else if (strcmp(argument, "--bitrate") == 0) {
            uint64_t rate = 0;
            if (!parse_positive_u64(value, &rate) || rate > 20000) {
                fprintf(stderr, "invalid --bitrate: %s\n", value);
                return -1;
            }
            options->bitrate_kbps = (uint32_t)rate;
        } else if (strcmp(argument, "--report-interval") == 0) {
            if (!parse_positive_u64(value, &options->report_interval)) {
                fprintf(stderr, "invalid --report-interval: %s\n", value);
                return -1;
            }
        } else {
            fprintf(stderr, "unknown option: %s\n", argument);
            return -1;
        }
    }
    return 0;
}

/*
 * Writes one frame of the recognition stream out as raw NV12 and stops.
 *
 * The model's coordinates are in this frame, while the overlay is drawn into
 * the encoder's; if the two streams do not frame the scene identically, an
 * overlay that is right in the first lands wrong in the second.  Nothing in
 * the running system shows that, so this exists to put the two side by side.
 */
static int dump_recognition_frame(media_context *media, const char *path)
{
    media_video_frame *frame = NULL;
    media_status status = MEDIA_ERR_TIMEOUT;
    /* The first frames after start-up can time out while the ISP settles. */
    for (int attempt = 0; attempt < 30 && status != MEDIA_OK; ++attempt) {
        status = media_video_acquire_frame(media, RECOGNITION_STREAM_ID, 200,
            &frame);
    }
    if (status != MEDIA_OK || frame == NULL) {
        fprintf(stderr, "acquire frame failed: %s\n",
            media_status_string(status));
        return EXIT_FAILURE;
    }

    int exit_code = EXIT_FAILURE;
    media_video_frame_info info;
    unsigned char *buffer = NULL;
    const size_t size = media_video_frame_packed_size(frame);
    if (media_video_frame_get_info(frame, &info) != MEDIA_OK || size == 0) {
        fprintf(stderr, "frame has no usable geometry\n");
        goto release;
    }
    buffer = malloc(size);
    if (buffer == NULL) {
        fprintf(stderr, "out of memory for %zu frame bytes\n", size);
        goto release;
    }
    if (media_video_frame_copy(frame, buffer, size) != MEDIA_OK) {
        fprintf(stderr, "copy frame failed\n");
        goto release;
    }
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        goto release;
    }
    const bool written = fwrite(buffer, 1, size, file) == size;
    (void)fclose(file);
    if (!written) {
        fprintf(stderr, "short write to %s\n", path);
        goto release;
    }
    printf("recognition frame: %ux%u stride=%u/%u %zu bytes -> %s\n",
        info.width, info.height, info.stride[0], info.stride[1], size, path);
    exit_code = EXIT_SUCCESS;

release:
    free(buffer);
    media_video_release_frame(media, frame);
    return exit_code;
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

static float normalize(float value, uint32_t extent)
{
    if (extent == 0) return 0.0F;
    const float scaled = value / (float)extent;
    return scaled < 0.0F ? 0.0F : scaled > 1.0F ? 1.0F : scaled;
}

static bool push_line(media_video_line *lines, size_t capacity, size_t *count,
    float x1, float y1, float x2, float y2, const uint8_t rgb[3],
    uint8_t thickness)
{
    if (*count >= capacity) return false;
    lines[(*count)++] = (media_video_line) {
        .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2,
        .red = rgb[0], .green = rgb[1], .blue = rgb[2],
        .thickness = thickness,
    };
    return true;
}

/*
 * Marks every keypoint the layout asks for, and joins them into a pose with
 * its bones.
 *
 * A bone needs both its ends shown: a limb hidden behind the body is left out
 * rather than drawn to a guessed position.  A point is a line of no length,
 * which the overlay draws as a filled circle, so it costs one line rather
 * than the several a drawn circle would.
 *
 * The line budget is shared, so the highest scoring subjects are drawn first
 * and a crowded frame loses its weakest subjects, not half of each.  Bones go
 * down before points, so a joint sits on top of the limbs meeting there.
 */

/*
 * The file gives sizes in pixels of the frame the model sees; the overlay
 * wants them in pixels of the encoded frame, which is usually much larger.
 * Scaling here is what keeps a skeleton looking the same at 640x480 and at
 * 1920x1080, since a line's thickness is the one part of it that is not
 * already a fraction of the frame.
 */
static uint8_t scaled_size(uint8_t size, float scale)
{
    const float value = (float)size * scale;
    if (value < 1.0F) return 1U;
    return value > 255.0F ? 255U : (uint8_t)value;
}

static size_t build_overlay(const keypoint_layout *layout,
    const keypoint_subject *subjects, size_t subject_count, uint32_t width,
    uint32_t height, uint32_t frame_height, media_video_line *lines,
    size_t capacity)
{
    const float scale = height > 0 ?
        (float)frame_height / (float)height : 1.0F;
    size_t count = 0;
    for (size_t index = 0; index < subject_count; ++index) {
        const keypoint_subject *subject = &subjects[index];
#if defined(CONFIG_MIPK_KEYPOINTS_DRAW_BOX)
        if (subject->has_box) {
            static const uint8_t box_rgb[3] = {200U, 200U, 200U};
            const uint8_t box = scaled_size(1U, scale);
            const float x1 = normalize(subject->box.x1, width);
            const float y1 = normalize(subject->box.y1, height);
            const float x2 = normalize(subject->box.x2, width);
            const float y2 = normalize(subject->box.y2, height);
            push_line(lines, capacity, &count, x1, y1, x2, y1, box_rgb, box);
            push_line(lines, capacity, &count, x2, y1, x2, y2, box_rgb, box);
            push_line(lines, capacity, &count, x2, y2, x1, y2, box_rgb, box);
            push_line(lines, capacity, &count, x1, y2, x1, y1, box_rgb, box);
        }
#endif
        for (size_t index_bone = 0; index_bone < layout->bone_count;
            ++index_bone) {
            const layout_bone *bone = &layout->bones[index_bone];
            if (!keypoints_point_shown(layout, subject, bone->from) ||
                !keypoints_point_shown(layout, subject, bone->to)) {
                continue;
            }
            const uint8_t rgb[3] = {bone->red, bone->green, bone->blue};
            push_line(lines, capacity, &count,
                normalize(subject->x[bone->from], width),
                normalize(subject->y[bone->from], height),
                normalize(subject->x[bone->to], width),
                normalize(subject->y[bone->to], height), rgb,
                scaled_size(bone->width, scale));
        }
        for (size_t index_point = 0; index_point < KEYPOINT_MAX_POINTS;
            ++index_point) {
            const layout_point *point = &layout->points[index_point];
            if (point->size == 0 ||
                !keypoints_point_shown(layout, subject, index_point)) {
                continue;
            }
            const uint8_t rgb[3] = {point->red, point->green, point->blue};
            const float x = normalize(subject->x[index_point], width);
            const float y = normalize(subject->y[index_point], height);
            push_line(lines, capacity, &count, x, y, x, y, rgb,
                scaled_size(point->size, scale));
        }
    }
    return count;
}

/* Orders subjects strongest first, so the overlay budget goes to the best ones. */
static void sort_subjects(keypoint_subject *subjects, size_t count)
{
    for (size_t index = 1; index < count; ++index) {
        const keypoint_subject current = subjects[index];
        size_t position = index;
        while (position > 0 && subjects[position - 1].score < current.score) {
            subjects[position] = subjects[position - 1];
            --position;
        }
        subjects[position] = current;
    }
}

static void report_state(const keypoint_layout *layout,
    const keypoint_subject *subjects, size_t subject_count, uint64_t frame,
    double inference_ms, bool trace)
{
    const char *what = layout->name;
    printf("frame=%llu %s=%zu inference_ms=%.3f\n",
        (unsigned long long)frame, what, subject_count, inference_ms);
    for (size_t index = 0; index < subject_count; ++index) {
        const keypoint_subject *subject = &subjects[index];
        const size_t total = keypoints_wanted(layout);
        printf("  %s=%zu score=%.3f box=(%.0f,%.0f)-(%.0f,%.0f) "
            "visible=%zu/%zu\n",
            what, index, (double)subject->score,
            (double)subject->box.x1, (double)subject->box.y1,
            (double)subject->box.x2, (double)subject->box.y2,
            keypoints_visible(layout, subject), total);
        if (!trace) {
            continue;
        }
        /* Every point, so an overlay that looks wrong can be read back. */
        for (size_t point = 0; point < KEYPOINT_MAX_POINTS; ++point) {
            if (!subject->has_point[point]) {
                continue;
            }
            char name[KEYPOINT_NAME_MAX];
            printf("    %2zu %-18s %6.1f %6.1f  vis=%.2f\n", point,
                keypoints_name(layout, point, name, sizeof(name)),
                (double)subject->x[point], (double)subject->y[point],
                (double)subject->visibility[point]);
        }
    }
}

static int run(const app_options *options, keypoint_layout *layout)
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
    udp_config.width = options->h264_width;
    udp_config.height = options->h264_height;
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
        fprintf(stderr, "configure recognition stream failed: %s\n",
            mipk_video_status_string(video_result));
        goto cleanup;
    }
    media_config_value.video.streams[H264_STREAM_ID] =
        media_config_value.video.streams[RECOGNITION_STREAM_ID];
    media_config_value.video.streams[H264_STREAM_ID].output =
        MEDIA_VIDEO_OUTPUT_H264;
    media_config_value.video.streams[H264_STREAM_ID].width =
        options->h264_width;
    media_config_value.video.streams[H264_STREAM_ID].height =
        options->h264_height;
    media_config_value.video.streams[H264_STREAM_ID].bit_rate_kbps =
        options->bitrate_kbps;
    media_config_value.video.streams[H264_STREAM_ID].gop = options->gop;
    media_status media_result = media_engine_open(&media_config_value, &media);
    if (media_result == MEDIA_OK) {
        media_result = media_video_open(media);
    }
    if (media_result != MEDIA_OK) {
        fprintf(stderr, "start camera failed: %s\n",
            media_status_string(media_result));
        goto cleanup;
    }

    if (options->dump_frame_path != NULL) {
        exit_code = dump_recognition_frame(media, options->dump_frame_path);
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
    printf("MIPK keypoints: camera=%u model=%s/%s keypoints=%s "
        "input=NV12/%ux%u H264=%ux%u %s\n",
        options->camera_id, info->name, info->version,
        /* Only a file declares a count, so its absence is the default one. */
        layout->keypoint_count == 0 ? "none (all points)" : layout->name,
        info->input_width, info->input_height, options->h264_width,
        options->h264_height, output);

    uint64_t completed = 0;
    bool keypoints_checked = false;
    exit_code = EXIT_SUCCESS;
    while (g_running &&
        (options->frame_limit == 0 || completed < options->frame_limit)) {
        check_overrides(&watcher, video, false);
        mipk_video_event event;
        video_result = mipk_video_process(video, media, RECOGNITION_STREAM_ID,
            100, &event);
        if (video_result == MIPK_VIDEO_AGAIN) continue;
        if (video_result != MIPK_VIDEO_OK) {
            fprintf(stderr, "video inference failed: %s\n",
                mipk_video_status_string(video_result));
            exit_code = EXIT_FAILURE;
            break;
        }
        keypoint_subject subjects[KEYPOINT_MAX_SUBJECTS];
        const size_t subject_count = keypoints_collect(event.output, subjects);
        /*
         * A layout paired with the wrong model would join up joints that
         * are not there.  The model does not announce its keypoint count, so
         * the first subject seen settles it.
         */
        if (!keypoints_checked && subject_count > 0) {
            keypoints_checked = true;
            const size_t highest = keypoints_highest_index(event.output);
            if (layout->keypoint_count == 0) {
                /* No file said how many there are, so the model just did. */
                keypoint_layout_limit(layout, highest + 1U);
            } else if (highest + 1U != layout->keypoint_count) {
                fprintf(stderr, "model reports %zu keypoints but layout %s "
                    "expects %u; pick the matching --keypoints\n", highest + 1U,
                    layout->name, layout->keypoint_count);
                exit_code = EXIT_FAILURE;
                break;
            }
        }
        sort_subjects(subjects, subject_count);
        media_video_line lines[MEDIA_VIDEO_MAX_LINES];
        const size_t line_count = build_overlay(layout, subjects,
            subject_count, event.width, event.height, options->h264_height,
            lines, MEDIA_VIDEO_MAX_LINES);
        media_result = media_video_set_lines(media, H264_STREAM_ID, lines,
            line_count);
        if (media_result != MEDIA_OK) {
            fprintf(stderr, "set video overlay failed: %s\n",
                media_status_string(media_result));
            exit_code = EXIT_FAILURE;
            break;
        }
        ++completed;
        if (options->trace || completed == 1 ||
            completed % options->report_interval == 0) {
            report_state(layout, subjects, subject_count,
                completed, event.inference_ms, options->trace);
            fflush(stdout);
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
    if (udp != NULL) {
        media_video_udp_stats stats;
        media_video_udp_get_stats(udp, &stats);
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
    /*
     * No keypoints file is a working configuration, not a missing one: every
     * point is then drawn where it lies in one colour, which suits any
     * keypoint model.  That is a layout too, so there is no second path.
     */
    keypoint_layout drawn;
    keypoint_layout_default(&drawn, KEYPOINT_THRESHOLD);
    if (options.layout_path != NULL && options.layout_path[0] != '\0') {
        char reason[160];
        if (!keypoint_layout_load(options.layout_path, &drawn,
                KEYPOINT_THRESHOLD, reason, sizeof(reason))) {
            fprintf(stderr, "keypoints %s: %s\n", options.layout_path,
                reason);
            return EXIT_FAILURE;
        }
    }
    if (signal(SIGINT, handle_signal) == SIG_ERR ||
        signal(SIGTERM, handle_signal) == SIG_ERR) {
        fprintf(stderr, "cannot install signal handlers\n");
        return EXIT_FAILURE;
    }
    return run(&options, &drawn);
}
