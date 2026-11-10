#define _POSIX_C_SOURCE 200809L

#include "wakeup_chime_pcm.h"
#include "hifw_config.h"
#include "media_audio_udp.h"
#include "mipk_audio.h"

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

#define AUDIO_SAMPLE_RATE 16000U
#define AUDIO_CAPTURE_SAMPLES 320U
#define AUDIO_FRAME_DURATION_MS 20U
#define AUDIO_CHANNELS 1U
#define RECEIVE_IDLE_NS 1000000L
#define OVERRIDES_POLL_MS 500U

typedef struct {
    const char *model_path;
    const char *overrides_path;
    const char *host;
    uint16_t tx_port;
    uint16_t rx_port;
    uint32_t mic_channel;
    bool enable_aec;
    bool play_local;
} app_options;

typedef struct {
    media_context *media;
    mipk_audio_prompt *prompt;
    bool play_local;
    uint64_t wake_count;
    uint64_t playback_drops;
} event_context;

typedef struct {
    media_audio_udp *udp;
    media_audio_decoder *decoder;
    uint64_t received_packets;
    uint64_t rejected_packets;
    bool failed;
} receive_context;

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

typedef enum {
    RECEIVE_OPUS_PROCESSED,
    RECEIVE_OPUS_IDLE,
    RECEIVE_OPUS_FAILED,
} receive_opus_result;

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

static void check_overrides(overrides_watcher *watcher, mipk_audio *audio,
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
    const mipk_status status = mipk_audio_load_overrides(audio, watcher->path);
    if (status == MIPK_OK) {
        printf("MIPK overrides loaded: %s\n", watcher->path);
    } else {
        fprintf(stderr, "MIPK overrides rejected: %s: %s\n",
            watcher->path, mipk_status_string(status));
    }
}

static bool parse_port(const char *text, uint16_t *port)
{
    if (text == NULL || port == NULL || text[0] == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || end[0] != '\0' ||
        value == 0 || value > UINT16_MAX) {
        return false;
    }
    *port = (uint16_t)value;
    return true;
}

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
        "usage: %s [--model MODEL.mipk] [--overrides OPTIONS.json] "
        "[--host IPv4] [--port N] [--rx-port N] [--mic-channel 0|1] "
        "[--aec|--no-aec] [--play-local]\n", program);
}

static int parse_options(int argc, char **argv, app_options *options)
{
    *options = (app_options) {
        .model_path = CONFIG_MIPK_WAKE_WORD_MODEL_PATH,
        .host = CONFIG_MIPK_WAKE_WORD_HOST,
        .tx_port = CONFIG_MIPK_WAKE_WORD_PORT,
        .rx_port = CONFIG_MIPK_WAKE_WORD_RX_PORT,
        .mic_channel = CONFIG_MIPK_WAKE_WORD_MIC_CHANNEL,
#ifdef CONFIG_MIPK_WAKE_WORD_ENABLE_AEC
        .enable_aec = true,
#endif
    };
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "-h") == 0 || strcmp(argument, "--help") == 0) {
            print_usage(stdout, argv[0]);
            return 1;
        }
        if (strcmp(argument, "--aec") == 0) {
            options->enable_aec = true;
            continue;
        }
        if (strcmp(argument, "--no-aec") == 0) {
            options->enable_aec = false;
            continue;
        }
        if (strcmp(argument, "--play-local") == 0) {
            options->play_local = true;
            continue;
        }
        if (++index >= argc) {
            fprintf(stderr, "missing value for %s\n", argument);
            return -1;
        }
        if (strcmp(argument, "--model") == 0) {
            options->model_path = argv[index];
        } else if (strcmp(argument, "--overrides") == 0) {
            options->overrides_path = argv[index];
        } else if (strcmp(argument, "--host") == 0) {
            options->host = argv[index];
        } else if (strcmp(argument, "--port") == 0) {
            if (!parse_port(argv[index], &options->tx_port)) {
                return -1;
            }
        } else if (strcmp(argument, "--rx-port") == 0) {
            if (!parse_port(argv[index], &options->rx_port)) {
                return -1;
            }
        } else if (strcmp(argument, "--mic-channel") == 0 &&
            (strcmp(argv[index], "0") == 0 || strcmp(argv[index], "1") == 0)) {
            options->mic_channel = (uint32_t)(argv[index][0] - '0');
        } else {
            fprintf(stderr, "invalid option: %s\n", argument);
            return -1;
        }
    }
    return 0;
}

static receive_opus_result receive_opus_packet(media_audio_udp *udp,
    media_audio_decoder *decoder, uint64_t *received_packets,
    uint64_t *rejected_packets)
{
    uint8_t packet[MEDIA_AUDIO_UDP_MAX_OPUS_BYTES];
    size_t packet_size = 0;
    media_audio_udp_packet_info info = {0};
    const media_audio_udp_status udp_result = media_audio_udp_receive_opus(udp,
        packet, sizeof(packet), &packet_size, &info);
    if (udp_result == MEDIA_AUDIO_UDP_AGAIN) {
        return RECEIVE_OPUS_IDLE;
    }
    if (udp_result == MEDIA_AUDIO_UDP_ERR_PACKET) {
        ++(*rejected_packets);
        if (*rejected_packets == 1 || *rejected_packets % 100U == 0) {
            fprintf(stderr, "RX UDP invalid XPAU packet: rejected=%llu\n",
                (unsigned long long)*rejected_packets);
        }
        return RECEIVE_OPUS_PROCESSED;
    }
    if (udp_result != MEDIA_AUDIO_UDP_OK) {
        fprintf(stderr, "receive Opus UDP failed: %s\n",
            media_audio_udp_status_string(udp_result));
        return RECEIVE_OPUS_FAILED;
    }
    media_status decode_result = MEDIA_ERR_ARGUMENT;
    if (info.sample_rate == AUDIO_SAMPLE_RATE &&
        info.channels == AUDIO_CHANNELS &&
        info.frame_duration_ms == AUDIO_FRAME_DURATION_MS) {
        decode_result = media_audio_decoder_write_packet(decoder,
            packet, packet_size, 100);
    }
    if (decode_result != MEDIA_OK) {
        ++(*rejected_packets);
        if (*rejected_packets == 1 || *rejected_packets % 100U == 0) {
            fprintf(stderr, "RX Opus rejected: count=%llu format=%uHz/"
                "%uch/%ums decode=%s\n",
                (unsigned long long)*rejected_packets, info.sample_rate,
                info.channels, info.frame_duration_ms,
                media_status_string(decode_result));
        }
        return RECEIVE_OPUS_PROCESSED;
    }
    ++(*received_packets);
    if (*received_packets == 1 || *received_packets % 100U == 0) {
        fprintf(stderr, "RX Opus: received=%llu rejected=%llu "
            "seq=%u pts=%u bytes=%zu\n",
            (unsigned long long)*received_packets,
            (unsigned long long)*rejected_packets, info.sequence,
            info.pts, packet_size);
    }
    return RECEIVE_OPUS_PROCESSED;
}

static void *receive_opus(void *argument)
{
    receive_context *context = argument;
    const struct timespec idle = {
        .tv_sec = 0,
        .tv_nsec = RECEIVE_IDLE_NS,
    };
    while (g_running) {
        const receive_opus_result result = receive_opus_packet(context->udp,
            context->decoder, &context->received_packets,
            &context->rejected_packets);
        if (result == RECEIVE_OPUS_FAILED) {
            context->failed = true;
            g_running = 0;
            break;
        }
        if (result == RECEIVE_OPUS_IDLE) {
            (void)nanosleep(&idle, NULL);
        }
    }
    return NULL;
}

static void audio_event(const mipk_audio_event *event, void *user_data)
{
    event_context *context = user_data;
    if (context->play_local) {
        const mipk_audio_status playback = mipk_audio_play_pcm(context->media,
            event->samples, event->sample_count, 0);
        if (playback != MIPK_AUDIO_OK) {
            ++context->playback_drops;
        }
    }
    const size_t result_count = mipk_output_result_count(event->output);
    for (size_t index = 0; index < result_count; ++index) {
        const mipk_result *result = mipk_output_result_at(event->output, index);
        const char *name = mipk_pipeline_label_name(event->pipeline,
            result->label_set_id, result->class_id);
        ++context->wake_count;
        printf("WAKEUP %s score=%.4f pts_us=%llu\n",
            name == NULL ? "?" : name, result->score,
            (unsigned long long)event->pts_us);
        (void)fflush(stdout);
    }
    if (result_count > 0 && context->prompt != NULL) {
        const mipk_audio_status prompt_result = mipk_audio_prompt_play(
            context->media, context->prompt, MEDIA_PROMPT_REPLACE);
        if (prompt_result != MIPK_AUDIO_OK) {
            fprintf(stderr, "wakeup prompt failed: %s\n",
                mipk_audio_status_string(prompt_result));
        }
    }
}

static media_status drain_opus(media_audio_encoder *encoder,
    media_audio_udp *udp, uint64_t *send_warnings)
{
    while (true) {
        uint8_t packet[MEDIA_AUDIO_UDP_MAX_OPUS_BYTES];
        size_t packet_size = 0;
        uint64_t pts = 0;
        uint32_t sequence = 0;
        const media_status read_result = media_audio_encoder_read_packet(
            encoder, packet, sizeof(packet), 0, &packet_size, &pts, &sequence);
        (void)sequence;
        if (read_result == MEDIA_ERR_TIMEOUT || read_result == MEDIA_AGAIN) {
            return MEDIA_OK;
        }
        if (read_result != MEDIA_OK) {
            return read_result;
        }
        const media_audio_udp_status udp_result = media_audio_udp_send_opus(
            udp, packet, packet_size, pts);
        if (udp_result != MEDIA_AUDIO_UDP_OK) {
            ++(*send_warnings);
            if (*send_warnings == 1 || *send_warnings % 100 == 0) {
                fprintf(stderr, "Opus UDP drop: %s (count=%llu)\n",
                    media_audio_udp_status_string(udp_result),
                    (unsigned long long)*send_warnings);
            }
        }
    }
}

static int run_microwakeword(const app_options *options)
{
    int exit_code = EXIT_FAILURE;
    mipk_audio *audio = NULL;
    mipk_audio_prompt *prompt = NULL;
    media_context *media = NULL;
    media_audio_encoder *encoder = NULL;
    media_audio_decoder *decoder = NULL;
    media_audio_udp *udp = NULL;
    pthread_t receiver;
    bool receiver_started = false;
    bool output_open = false;
    bool input_open = false;
    event_context events = {0};
    receive_context receive = {0};
    mipk_audio_config audio_config;
    mipk_audio_config_init(&audio_config);
    audio_config.package_path = options->model_path;
    mipk_audio_status audio_result = mipk_audio_open(&audio_config, &audio);
    if (audio_result != MIPK_AUDIO_OK) {
        fprintf(stderr, "open audio model failed: %s\n",
            mipk_audio_status_string(audio_result));
        goto cleanup;
    }
    const mipk_pipeline_info *info = mipk_audio_get_pipeline_info(audio);
    overrides_watcher watcher = {.path = options->overrides_path};
    check_overrides(&watcher, audio, true);
    if (info->input_sample_rate != AUDIO_SAMPLE_RATE ||
        AUDIO_CAPTURE_SAMPLES % info->input_frame_samples != 0) {
        fprintf(stderr, "MicroWakeWord requires a 16kHz model whose frame "
            "size divides 320 samples\n");
        goto cleanup;
    }

    media_audio_udp_config udp_config;
    media_audio_udp_config_init(&udp_config);
    udp_config.address = options->host;
    udp_config.port = options->tx_port;
    udp_config.receive_port = options->rx_port;
    udp_config.sample_rate = AUDIO_SAMPLE_RATE;
    udp_config.channels = AUDIO_CHANNELS;
    udp_config.frame_duration_ms = AUDIO_FRAME_DURATION_MS;
    media_audio_udp_status udp_result = media_audio_udp_open(&udp_config,
        &udp);
    if (udp_result != MEDIA_AUDIO_UDP_OK) {
        fprintf(stderr, "open Opus UDP failed: %s\n",
            media_audio_udp_status_string(udp_result));
        goto cleanup;
    }

    media_config media_config_value;
    media_config_init(&media_config_value);
    audio_result = mipk_audio_configure_input(audio,
        &media_config_value.audio_input, AUDIO_CAPTURE_SAMPLES,
        options->mic_channel, CONFIG_MIPK_WAKE_WORD_MIC_GAIN_DB,
        options->enable_aec);
    if (audio_result == MIPK_AUDIO_OK) {
        audio_result = mipk_audio_configure_output(audio,
            &media_config_value.audio_output, AUDIO_CAPTURE_SAMPLES, -10);
    }
    if (audio_result != MIPK_AUDIO_OK) {
        fprintf(stderr, "configure audio failed: %s\n",
            mipk_audio_status_string(audio_result));
        goto cleanup;
    }
    media_status media_result = media_engine_open(&media_config_value, &media);
    if (media_result == MEDIA_OK) {
        media_result = media_audio_output_open(media);
        output_open = media_result == MEDIA_OK;
    }
    if (media_result == MEDIA_OK) {
        audio_result = mipk_audio_prompt_load(media,
            MEDIA_AUDIO_FORMAT_PCM_S16LE,
            g_wakeup_chime_pcm, g_wakeup_chime_pcm_size, &prompt);
        if (audio_result != MIPK_AUDIO_OK) {
            media_result = MEDIA_ERR_MEMORY;
        }
    }
    if (media_result == MEDIA_OK) {
        media_result = media_audio_input_open(media);
        input_open = media_result == MEDIA_OK;
    }
    media_audio_encoder_config encoder_config;
    media_audio_encoder_config_init(&encoder_config);
    encoder_config.codec = MEDIA_AUDIO_CODEC_OPUS;
    encoder_config.sample_rate = AUDIO_SAMPLE_RATE;
    encoder_config.channels = AUDIO_CHANNELS;
    encoder_config.bit_rate = CONFIG_MIPK_WAKE_WORD_OPUS_BIT_RATE;
    encoder_config.frame_samples = AUDIO_CAPTURE_SAMPLES;
    if (media_result == MEDIA_OK) {
        media_result = media_audio_encoder_open(media, &encoder_config,
            &encoder);
    }
    media_audio_decoder_config decoder_config;
    media_audio_decoder_config_init(&decoder_config);
    decoder_config.codec = MEDIA_AUDIO_CODEC_OPUS;
    decoder_config.sample_rate = AUDIO_SAMPLE_RATE;
    decoder_config.channels = AUDIO_CHANNELS;
    if (media_result == MEDIA_OK) {
        media_result = media_audio_decoder_open(media, &decoder_config,
            &decoder);
    }
    if (media_result != MEDIA_OK) {
        fprintf(stderr, "start MicroWakeWord audio failed: %s\n",
            media_status_string(media_result));
        goto cleanup;
    }

    receive.udp = udp;
    receive.decoder = decoder;
    if (pthread_create(&receiver, NULL, receive_opus, &receive) != 0) {
        fprintf(stderr, "cannot create UDP receive thread\n");
        goto cleanup;
    }
    receiver_started = true;

    printf("MIPK wake word: model=%s/%s input=%uHz/%usamples "
        "capture=320samples/20ms Opus=%ubps TX=%s:%u RX=0.0.0.0:%u "
        "local=%s AEC=%s\n",
        info->name, info->version, info->input_sample_rate,
        info->input_frame_samples, encoder_config.bit_rate, options->host,
        options->tx_port, options->rx_port,
        options->play_local ? "on" : "off",
        options->enable_aec ? "TalkV2" : "off");
    events.media = media;
    events.prompt = prompt;
    events.play_local = options->play_local;
    uint64_t send_warnings = 0;
    exit_code = EXIT_SUCCESS;
    while (g_running) {
        check_overrides(&watcher, audio, false);
        audio_result = mipk_audio_poll(audio, media, 1000, audio_event,
            &events);
        if (audio_result != MIPK_AUDIO_OK &&
            audio_result != MIPK_AUDIO_AGAIN) {
            fprintf(stderr, "audio inference failed: %s\n",
                mipk_audio_status_string(audio_result));
            exit_code = EXIT_FAILURE;
            break;
        }
        media_result = drain_opus(encoder, udp, &send_warnings);
        if (media_result != MEDIA_OK) {
            fprintf(stderr, "Opus capture failed: %s\n",
                media_status_string(media_result));
            exit_code = EXIT_FAILURE;
            break;
        }
    }
cleanup:
    g_running = 0;
    if (receiver_started) {
        (void)pthread_join(receiver, NULL);
    }
    if (receive.failed) {
        exit_code = EXIT_FAILURE;
    }
    media_audio_udp_stats stats;
    media_audio_udp_get_stats(udp, &stats);
    if (udp != NULL) {
        printf("summary: wakes=%llu Opus_sent=%llu Opus_dropped=%llu "
            "Opus_received=%llu Opus_rejected=%llu local_dropped=%llu\n",
            (unsigned long long)events.wake_count,
            (unsigned long long)stats.frames_sent,
            (unsigned long long)stats.frames_dropped,
            (unsigned long long)receive.received_packets,
            (unsigned long long)receive.rejected_packets,
            (unsigned long long)events.playback_drops);
    }
    media_audio_decoder_close(decoder);
    media_audio_encoder_close(encoder);
    mipk_audio_prompt_close(prompt);
    if (input_open) {
        (void)media_audio_input_close(media);
    }
    if (output_open) {
        (void)media_audio_output_close(media);
    }
    if (media != NULL) {
        media_engine_close(media);
    }
    media_audio_udp_close(udp);
    mipk_audio_close(audio);
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
    return run_microwakeword(&options);
}
