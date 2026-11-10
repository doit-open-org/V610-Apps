#define _POSIX_C_SOURCE 200809L

#include "hand_pinch.h"
#include "hifw_config.h"

#include "media_video_rtsp.h"
#include "mipk_video.h"
#if defined(CONFIG_SERVO_TRACKER)
#include "servo_tracker.h"
#endif

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECOGNITION_STREAM_ID 0U
#define H264_STREAM_ID 1U
#define VIDEO_FRAME_RATE 30U
#define H264_WIDTH 1920U
#define H264_HEIGHT 1080U
#define H264_BIT_RATE_KBPS 2500U
#define MARKER_HALF_PIXELS 5.0F

typedef struct {
    const char *model_path;
    uint32_t camera_id;
    uint16_t rtsp_port;
    uint64_t frame_limit;
    uint64_t report_interval;
    bool trace;
} app_options;

typedef struct {
    media_context *media;
    media_video_rtsp *rtsp;
    bool read_failed;
} stream_context;

static volatile sig_atomic_t g_running = 1;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    g_running = 0;
}

static bool parse_number(const char *text, uint64_t max, uint64_t *value)
{
    char *end = NULL;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0 || parsed > max) {
        fprintf(stderr, "invalid number: %s\n", text);
        return false;
    }
    *value = parsed;
    return true;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
        "usage: %s [--model MODEL.mipk] [--camera 0|1] [--rtsp PORT] "
        "[--frames N] [--report-interval N] [--trace]\n", program);
}

static int parse_options(int argc, char **argv, app_options *options)
{
    *options = (app_options) {
        .model_path = CONFIG_HAND_GIMBAL_MODEL_PATH,
        .rtsp_port = CONFIG_HAND_GIMBAL_RTSP_PORT,
        .report_interval = CONFIG_HAND_GIMBAL_REPORT_INTERVAL,
    };
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        uint64_t number = 0;
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
        } else if (strcmp(argument, "--camera") == 0) {
            if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) {
                fprintf(stderr, "invalid camera: %s (expected 0 or 1)\n", value);
                return -1;
            }
            options->camera_id = (uint32_t)(value[0] - '0');
        } else if (strcmp(argument, "--rtsp") == 0) {
            if (!parse_number(value, UINT16_MAX, &number)) return -1;
            options->rtsp_port = (uint16_t)number;
        } else if (strcmp(argument, "--frames") == 0) {
            if (!parse_number(value, UINT64_MAX, &options->frame_limit)) return -1;
        } else if (strcmp(argument, "--report-interval") == 0) {
            if (!parse_number(value, UINT64_MAX, &options->report_interval)) return -1;
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
        const media_status result = media_video_read_packet(context->media,
            H264_STREAM_ID, 100, &packet);
        if (result == MEDIA_ERR_TIMEOUT || result == MEDIA_AGAIN) continue;
        if (result != MEDIA_OK) {
            fprintf(stderr, "H264 capture failed: %s\n", media_status_string(result));
            context->read_failed = true;
            g_running = 0;
            break;
        }
        (void)media_video_rtsp_send_h264(context->rtsp, packet.data, packet.size,
            packet.key_frame, packet.pts);
        media_video_packet_release(&packet);
    }
    return NULL;
}

static float normalize(float value, uint32_t extent)
{
    const float scaled = extent == 0 ? 0.0F : value / (float)extent;
    return scaled < 0.0F ? 0.0F : scaled > 1.0F ? 1.0F : scaled;
}

static void push_line(media_video_line *lines, size_t capacity, size_t *count,
    float x1, float y1, float x2, float y2, const uint8_t rgb[3], uint8_t thickness)
{
    if (*count >= capacity) return;
    lines[(*count)++] = (media_video_line) {
        .x1 = x1, .y1 = y1, .x2 = x2, .y2 = y2,
        .red = rgb[0], .green = rgb[1], .blue = rgb[2],
        .thickness = thickness,
    };
}

/* MediaPipe's 21-point hand connections. */
static const uint8_t g_skeleton[][2] = {
    {0, 1}, {1, 2}, {2, 3}, {3, 4},
    {0, 5}, {5, 6}, {6, 7}, {7, 8},
    {5, 9}, {9, 10}, {10, 11}, {11, 12},
    {9, 13}, {13, 14}, {14, 15}, {15, 16},
    {13, 17}, {17, 18}, {18, 19}, {19, 20}, {0, 17},
};

/*
 * Draws each tracked hand's skeleton into the encoded stream: red while it
 * pinches, with a cross on the thumb and index tips, green otherwise.
 */
static size_t build_overlay(const hand_gesture_tracker *tracker, uint32_t width,
    uint32_t height, media_video_line *lines, size_t capacity)
{
    static const uint8_t pinch_rgb[3] = {255U, 64U, 48U};
    static const uint8_t open_rgb[3] = {64U, 224U, 96U};
    size_t count = 0;
    for (size_t slot = 0; slot < HAND_GESTURE_MAX_HANDS; ++slot) {
        const hand_slot *hand = &tracker->slots[slot];
        if (!hand->present) continue;
        const uint8_t *rgb = hand->pinching ? pinch_rgb : open_rgb;
        const uint8_t thickness = hand->pinching ? 3U : 2U;
        for (size_t index = 0; index < sizeof(g_skeleton) / sizeof(g_skeleton[0]); ++index) {
            const size_t a = g_skeleton[index][0];
            const size_t b = g_skeleton[index][1];
            if (!hand->has_point[a] || !hand->has_point[b]) continue;
            push_line(lines, capacity, &count,
                normalize(hand->x[a], width), normalize(hand->y[a], height),
                normalize(hand->x[b], width), normalize(hand->y[b], height),
                rgb, thickness);
        }
        if (!hand->pinching) continue;
        static const size_t tips[2] = {HAND_LANDMARK_THUMB_TIP, HAND_LANDMARK_INDEX_TIP};
        for (size_t index = 0; index < 2; ++index) {
            const size_t p = tips[index];
            if (!hand->has_point[p]) continue;
            push_line(lines, capacity, &count,
                normalize(hand->x[p] - MARKER_HALF_PIXELS, width), normalize(hand->y[p], height),
                normalize(hand->x[p] + MARKER_HALF_PIXELS, width), normalize(hand->y[p], height),
                pinch_rgb, 3U);
            push_line(lines, capacity, &count,
                normalize(hand->x[p], width), normalize(hand->y[p] - MARKER_HALF_PIXELS, height),
                normalize(hand->x[p], width), normalize(hand->y[p] + MARKER_HALF_PIXELS, height),
                pinch_rgb, 3U);
        }
    }
    return count;
}

static void report_events(const hand_gesture_tracker *tracker, uint64_t frame)
{
    for (size_t slot = 0; slot < HAND_GESTURE_MAX_HANDS; ++slot) {
        const hand_slot *hand = &tracker->slots[slot];
        if (hand->event == HAND_EVENT_NONE) continue;
        printf("frame=%llu hand=%zu %s pinch_ratio=%.3f extension=%.3f straight=%.3f\n",
            (unsigned long long)frame, slot, hand_event_name(hand->event),
            (double)hand->pinch_ratio, (double)hand->extension,
            (double)hand->straightness);
        fflush(stdout);
    }
}

static void report_state(const hand_gesture_tracker *tracker, uint64_t frame,
    double inference_ms)
{
    size_t hands = 0;
    for (size_t slot = 0; slot < HAND_GESTURE_MAX_HANDS; ++slot) {
        hands += tracker->slots[slot].present ? 1U : 0U;
    }
    printf("frame=%llu hands=%zu inference_ms=%.3f\n", (unsigned long long)frame,
        hands, inference_ms);
    for (size_t slot = 0; slot < HAND_GESTURE_MAX_HANDS; ++slot) {
        const hand_slot *hand = &tracker->slots[slot];
        if (!hand->present) continue;
        printf("  hand=%zu score=%.3f pinch_ratio=%.3f extension=%.3f straight=%.3f %s\n",
            slot, (double)hand->score, (double)hand->pinch_ratio,
            (double)hand->extension, (double)hand->straightness,
            hand->pinching ? "PINCH" : "open");
    }
}

#if defined(CONFIG_SERVO_TRACKER)
/*
 * The mount follows the largest pinching hand, the one nearest the camera.
 * The pinch is a clutch: pinch to take hold of the mount, move, let go to
 * leave it pointing there. A hand that is merely in view never moves it.
 */
static bool servo_target(const hand_gesture_tracker *tracker, float *x, float *y)
{
    float best_area = 0.0F;
    for (size_t slot = 0; slot < HAND_GESTURE_MAX_HANDS; ++slot) {
        const hand_slot *hand = &tracker->slots[slot];
        if (!hand->present || !hand->has_bbox || !hand->pinching) continue;
        const float area = (hand->bbox.x2 - hand->bbox.x1) * (hand->bbox.y2 - hand->bbox.y1);
        if (area <= best_area) continue;
        best_area = area;
        *x = (hand->bbox.x1 + hand->bbox.x2) * 0.5F;
        *y = (hand->bbox.y1 + hand->bbox.y2) * 0.5F;
    }
    return best_area > 0.0F;
}

static servo_tracker *open_servos(void)
{
    servo_tracker_config config;
    servo_tracker_config_init(&config);
    config.pwm_device = CONFIG_SERVO_TRACKER_PWM_DEVICE;
#if defined(CONFIG_SERVO_TRACKER_PAN_ENABLE)
    config.pan = (servo_tracker_axis) {
        .enabled = true,
        .channel = CONFIG_SERVO_TRACKER_PAN_CHANNEL,
        .iocfg_address = CONFIG_SERVO_TRACKER_PAN_IOCFG,
        .iocfg_value = CONFIG_SERVO_TRACKER_PAN_IOCFG_VALUE,
        .span_us = CONFIG_SERVO_TRACKER_PAN_SPAN_US,
#if defined(CONFIG_SERVO_TRACKER_KIND_POSITIONAL)
        .step_percent = CONFIG_SERVO_TRACKER_PAN_STEP_PERCENT,
#endif
#if defined(CONFIG_SERVO_TRACKER_PAN_INVERT)
        .invert = true,
#endif
    };
#endif
#if defined(CONFIG_SERVO_TRACKER_TILT_ENABLE)
    config.tilt = (servo_tracker_axis) {
        .enabled = true,
        .channel = CONFIG_SERVO_TRACKER_TILT_CHANNEL,
        .iocfg_address = CONFIG_SERVO_TRACKER_TILT_IOCFG,
        .iocfg_value = CONFIG_SERVO_TRACKER_TILT_IOCFG_VALUE,
        .span_us = CONFIG_SERVO_TRACKER_TILT_SPAN_US,
#if defined(CONFIG_SERVO_TRACKER_KIND_POSITIONAL)
        .step_percent = CONFIG_SERVO_TRACKER_TILT_STEP_PERCENT,
#endif
#if defined(CONFIG_SERVO_TRACKER_TILT_INVERT)
        .invert = true,
#endif
    };
#endif
    config.period_us = CONFIG_SERVO_TRACKER_PERIOD_US;
    config.neutral_us = CONFIG_SERVO_TRACKER_NEUTRAL_US;
    config.span_us = CONFIG_SERVO_TRACKER_SPAN_US;
#if defined(CONFIG_SERVO_TRACKER_KIND_CONTINUOUS)
    config.kind = SERVO_TRACKER_CONTINUOUS;
    config.dead_band_us = CONFIG_SERVO_TRACKER_DEAD_BAND_US;
#else
    config.kind = SERVO_TRACKER_POSITIONAL;
    config.step_percent = CONFIG_SERVO_TRACKER_STEP_PERCENT;
#if defined(CONFIG_SERVO_TRACKER_RECENTRE_ON_LOSS)
    config.recentre_on_loss = true;
#endif
#endif
    config.gain_percent = CONFIG_SERVO_TRACKER_GAIN_PERCENT;
    config.dead_zone_percent = CONFIG_SERVO_TRACKER_DEAD_ZONE_PERCENT;
    config.max_speed_percent = CONFIG_SERVO_TRACKER_MAX_SPEED_PERCENT;
    config.slew_percent = CONFIG_SERVO_TRACKER_SLEW_PERCENT;

    servo_tracker *servos = NULL;
    const servo_tracker_status status = servo_tracker_open(&config, &servos);
    if (status != SERVO_TRACKER_OK) {
        /* The mount is optional: without it the hands are still tracked. */
        fprintf(stderr, "servo tracking off: %s\n", servo_tracker_status_string(status));
        return NULL;
    }
    printf("servo: %s, pan PWM channel %u, tilt PWM channel %u, travel %u..%u us\n",
        config.kind == SERVO_TRACKER_CONTINUOUS ? "continuous" : "positional",
        config.pan.channel, config.tilt.channel,
        config.neutral_us - config.span_us, config.neutral_us + config.span_us);
    return servos;
}
#endif

static int run(const app_options *options)
{
    int exit_code = EXIT_FAILURE;
    mipk_video *video = NULL;
    media_context *media = NULL;
    media_video_rtsp *rtsp = NULL;
    pthread_t thread;
    bool thread_started = false;
    stream_context stream = {0};
#if defined(CONFIG_SERVO_TRACKER)
    servo_tracker *servos = NULL;
    /* Consecutive frames the pinch holding the mount has been seen. */
    uint32_t servo_held = 0;
#endif

    hand_gesture_config gesture_config;
    hand_gesture_config_init(&gesture_config);
    gesture_config.close_percent = CONFIG_HAND_GIMBAL_CLOSE_PERCENT;
    gesture_config.open_percent = CONFIG_HAND_GIMBAL_OPEN_PERCENT;
    gesture_config.extension_percent = CONFIG_HAND_GIMBAL_EXTENSION_PERCENT;
    gesture_config.straight_percent = CONFIG_HAND_GIMBAL_STRAIGHT_PERCENT;
    gesture_config.confirm_frames = CONFIG_HAND_GIMBAL_CONFIRM_FRAMES;
    gesture_config.release_frames = CONFIG_HAND_GIMBAL_CONFIRM_FRAMES;
    if (gesture_config.open_percent <= gesture_config.close_percent) {
        fprintf(stderr, "pinch open threshold must exceed the close threshold\n");
        return EXIT_FAILURE;
    }
    hand_gesture_tracker tracker;
    hand_gesture_init(&tracker, &gesture_config);

    mipk_video_config video_config;
    mipk_video_config_init(&video_config);
    video_config.package_path = options->model_path;
    mipk_video_status video_result = mipk_video_open(&video_config, &video);
    if (video_result != MIPK_VIDEO_OK) {
        fprintf(stderr, "open model %s failed: %s\n", options->model_path,
            mipk_video_status_string(video_result));
        goto cleanup;
    }

    media_video_rtsp_config rtsp_config;
    media_video_rtsp_config_init(&rtsp_config);
    rtsp_config.port = options->rtsp_port;
    rtsp_config.request_key_frame = request_key_frame;
    rtsp_config.user = &stream;
    const media_video_rtsp_status rtsp_result = media_video_rtsp_open(&rtsp_config, &rtsp);
    if (rtsp_result != MEDIA_VIDEO_RTSP_OK) {
        fprintf(stderr, "open RTSP server failed: %s\n",
            media_video_rtsp_status_string(rtsp_result));
        goto cleanup;
    }

    /* The recognition stream feeds the model, the H264 stream the viewers. */
    media_config media_config_value;
    media_config_init(&media_config_value);
    media_video_config_init(&media_config_value.video);
    media_config_value.video.cameras[0] = media_config_value.video.cameras[options->camera_id];
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
    media_video_stream_config *h264 = &media_config_value.video.streams[H264_STREAM_ID];
    *h264 = media_config_value.video.streams[RECOGNITION_STREAM_ID];
    h264->output = MEDIA_VIDEO_OUTPUT_H264;
    h264->width = H264_WIDTH;
    h264->height = H264_HEIGHT;
    h264->bit_rate_kbps = H264_BIT_RATE_KBPS;
    h264->gop = CONFIG_HAND_GIMBAL_GOP;
    media_status media_result = media_engine_open(&media_config_value, &media);
    if (media_result == MEDIA_OK) {
        media_result = media_video_open(media);
    }
    if (media_result != MEDIA_OK) {
        fprintf(stderr, "start camera failed: %s\n", media_status_string(media_result));
        goto cleanup;
    }

    stream.media = media;
    stream.rtsp = rtsp;
    if (pthread_create(&thread, NULL, h264_worker, &stream) != 0) {
        fprintf(stderr, "start H264 streaming thread failed\n");
        goto cleanup;
    }
    thread_started = true;
    printf("hand gimbal: camera=%u model=%s H264=%ux%u RTSP=rtsp://<board>:%u/live\n",
        options->camera_id, options->model_path, H264_WIDTH, H264_HEIGHT,
        (unsigned)options->rtsp_port);
    printf("pinch: close<%u%% open>%u%% extension>%u%% straight>%u%% confirm=%u frames\n",
        gesture_config.close_percent, gesture_config.open_percent,
        gesture_config.extension_percent, gesture_config.straight_percent,
        gesture_config.confirm_frames);
#if defined(CONFIG_SERVO_TRACKER)
    servos = open_servos();
#endif

    uint64_t completed = 0;
    exit_code = EXIT_SUCCESS;
    while (g_running && (options->frame_limit == 0 || completed < options->frame_limit)) {
        mipk_video_event event;
        video_result = mipk_video_process(video, media, RECOGNITION_STREAM_ID, 100, &event);
        if (video_result == MIPK_VIDEO_AGAIN) continue;
        if (video_result != MIPK_VIDEO_OK) {
            fprintf(stderr, "video inference failed: %s\n",
                mipk_video_status_string(video_result));
            exit_code = EXIT_FAILURE;
            break;
        }
        hand_gesture_update(&tracker, event.output);
#if defined(CONFIG_SERVO_TRACKER)
        if (servos != NULL) {
            float target_x = 0.0F;
            float target_y = 0.0F;
            /*
             * Hand the mount over only once the pinch has been held, so a
             * gesture made in passing never moves it. Letting go releases it
             * at once; the pinch has its own hysteresis on the way out.
             */
            if (servo_target(&tracker, &target_x, &target_y)) {
                if (servo_held < CONFIG_HAND_GIMBAL_PINCH_FRAMES) ++servo_held;
            } else {
                servo_held = 0;
            }
            const bool aimed = servo_held >= CONFIG_HAND_GIMBAL_PINCH_FRAMES;
            const servo_tracker_status steered = servo_tracker_track(servos, aimed,
                target_x, target_y, event.width, event.height);
            if (steered != SERVO_TRACKER_OK) {
                fprintf(stderr, "servo steering failed: %s\n",
                    servo_tracker_status_string(steered));
                servo_tracker_close(servos);
                servos = NULL;
            }
        }
#endif
        media_video_line lines[MEDIA_VIDEO_MAX_LINES];
        const size_t line_count = build_overlay(&tracker, event.width, event.height,
            lines, MEDIA_VIDEO_MAX_LINES);
        media_result = media_video_set_lines(media, H264_STREAM_ID, lines, line_count);
        if (media_result != MEDIA_OK) {
            fprintf(stderr, "set video overlay failed: %s\n", media_status_string(media_result));
            exit_code = EXIT_FAILURE;
            break;
        }
        ++completed;
        report_events(&tracker, completed);
        if (options->trace || completed == 1 || completed % options->report_interval == 0) {
            report_state(&tracker, completed, event.inference_ms);
#if defined(CONFIG_SERVO_TRACKER)
            if (servos != NULL) {
                uint32_t pan_us = 0;
                uint32_t tilt_us = 0;
                servo_tracker_pulses(servos, &pan_us, &tilt_us);
                printf("  servo pan=%uus tilt=%uus held=%u/%u\n", pan_us, tilt_us,
                    servo_held, CONFIG_HAND_GIMBAL_PINCH_FRAMES);
            }
#endif
        }
    }

cleanup:
    g_running = 0;
#if defined(CONFIG_SERVO_TRACKER)
    /* Stop the mount before anything else goes away. */
    servo_tracker_close(servos);
#endif
    if (thread_started) {
        (void)pthread_join(thread, NULL);
    }
    if (stream.read_failed) {
        exit_code = EXIT_FAILURE;
    }
    /* The RTSP server calls back into media; stop it first. */
    media_video_rtsp_close(rtsp);
    if (media != NULL) {
        (void)media_video_close(media);
        media_engine_close(media);
    }
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
    if (signal(SIGINT, handle_signal) == SIG_ERR || signal(SIGTERM, handle_signal) == SIG_ERR) {
        fprintf(stderr, "cannot install signal handlers\n");
        return EXIT_FAILURE;
    }
    return run(&options);
}
