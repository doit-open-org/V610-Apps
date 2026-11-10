#define _DEFAULT_SOURCE

#include "board.h"

#include "media_engine/media.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PHOTO_SETTLE_SECONDS 1.0
#define BEEP_RATE 16000
#define BEEP_FREQUENCY 1000.0
#define BEEP_ON_MS 120
#define BEEP_OFF_MS 150

/*
 * A timer can go off while a request is using the camera or the speaker, so
 * the actions that open the media engine take turns.
 */
static pthread_mutex_t g_media_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int g_photo_count;

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int board_take_photo(char *result, size_t size)
{
    media_config config;
    media_context *media = NULL;
    char path[64];
    int status = -1;

    snprintf(path, sizeof(path), "/tmp/photo_%u.jpg", ++g_photo_count);
    media_config_init(&config);
    media_video_config_init(&config.video);
    config.video.streams[0].output = MEDIA_VIDEO_OUTPUT_H264;
    config.video.streams[0].width = 1280;
    config.video.streams[0].height = 720;

    pthread_mutex_lock(&g_media_lock);
    media_status media_result = media_engine_open(&config, &media);
    if (media_result == MEDIA_OK) {
        media_result = media_video_open(media);
    }
    if (media_result == MEDIA_OK) {
        /* Let auto exposure settle; the stream is read only to keep it moving. */
        const double start = now_seconds();
        while (now_seconds() - start < PHOTO_SETTLE_SECONDS) {
            media_video_packet packet;
            if (media_video_read_packet(media, 0, 200, &packet) == MEDIA_OK) {
                media_video_packet_release(&packet);
            }
        }
        media_result = media_video_save_jpeg(media, 0, path, 2000);
        media_video_close(media);
    }
    media_engine_close(media);
    pthread_mutex_unlock(&g_media_lock);

    if (media_result == MEDIA_OK) {
        snprintf(result, size, "photo saved to %s", path);
        status = 0;
    } else {
        snprintf(result, size, "camera failed: %s", media_status_string(media_result));
    }
    return status;
}

/* Plays the beeps and waits until the speaker has finished them. */
static media_status play_beeps(int times)
{
    static int16_t tone[BEEP_RATE * BEEP_ON_MS / 1000];
    static int16_t silence[BEEP_RATE * BEEP_OFF_MS / 1000];
    for (size_t index = 0; index < sizeof(tone) / sizeof(tone[0]); ++index) {
        tone[index] = (int16_t)(12000.0 * sin(2.0 * M_PI * BEEP_FREQUENCY * (double)index / BEEP_RATE));
    }

    media_config config;
    media_context *media = NULL;
    media_config_init(&config);
    media_audio_output_config_init(&config.audio_output);
    media_status status = media_engine_open(&config, &media);
    if (status == MEDIA_OK) {
        status = media_audio_output_open(media);
    }
    for (int beep = 0; status == MEDIA_OK && beep < times; ++beep) {
        status = media_audio_output_write(media, tone, sizeof(tone) / sizeof(tone[0]), 1000);
        if (status == MEDIA_OK) {
            status = media_audio_output_write(media, silence, sizeof(silence) / sizeof(silence[0]), 1000);
        }
    }
    if (status == MEDIA_OK) {
        status = media_audio_output_finish(media);
        const double deadline = now_seconds() + 1.0 + times * (BEEP_ON_MS + BEEP_OFF_MS) / 1000.0;
        while (media_audio_play_get_state(media) == MEDIA_AUDIO_PLAY_PLAYING &&
            now_seconds() < deadline) {
            usleep(20000);
        }
        media_audio_output_close(media);
    }
    media_engine_close(media);
    return status;
}

int board_beep(int times, char *result, size_t size)
{
    if (times < 1 || times > 10) {
        snprintf(result, size, "beep count %d is out of range 1..10", times);
        return -1;
    }
    pthread_mutex_lock(&g_media_lock);
    const media_status status = play_beeps(times);
    pthread_mutex_unlock(&g_media_lock);
    if (status != MEDIA_OK) {
        snprintf(result, size, "speaker failed: %s", media_status_string(status));
        return -1;
    }
    snprintf(result, size, "beeped %d time%s", times, times == 1 ? "" : "s");
    return 0;
}

static void *timer_thread(void *argument)
{
    const int seconds = (int)(intptr_t)argument;
    sleep((unsigned int)seconds);
    char result[128];
    (void)board_beep(3, result, sizeof(result));
    printf("timer: %d s countdown finished\n", seconds);
    fflush(stdout);
    return NULL;
}

int board_set_timer(int seconds, char *result, size_t size)
{
    pthread_t thread;
    if (seconds < 1 || seconds > 3600) {
        snprintf(result, size, "timer length %d s is out of range 1..3600", seconds);
        return -1;
    }
    if (pthread_create(&thread, NULL, timer_thread, (void *)(intptr_t)seconds) != 0) {
        snprintf(result, size, "cannot start the timer");
        return -1;
    }
    pthread_detach(thread);
    snprintf(result, size, "timer set for %d s", seconds);
    return 0;
}

int board_get_memory(char *result, size_t size)
{
    FILE *file = fopen("/proc/meminfo", "r");
    unsigned long total = 0;
    unsigned long available = 0;
    char line[128];
    while (file != NULL && fgets(line, sizeof(line), file) != NULL) {
        (void)sscanf(line, "MemTotal: %lu kB", &total);
        (void)sscanf(line, "MemAvailable: %lu kB", &available);
    }
    if (file != NULL) fclose(file);
    if (total == 0) {
        snprintf(result, size, "cannot read /proc/meminfo");
        return -1;
    }
    snprintf(result, size, "%lu MB of %lu MB memory available", available / 1024, total / 1024);
    return 0;
}

int board_get_ip_address(char *result, size_t size)
{
    struct ifaddrs *list = NULL;
    size_t used = 0;
    result[0] = '\0';
    if (getifaddrs(&list) != 0) {
        snprintf(result, size, "cannot list the network interfaces");
        return -1;
    }
    for (struct ifaddrs *entry = list; entry != NULL; entry = entry->ifa_next) {
        if (entry->ifa_addr == NULL || entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        char address[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &((struct sockaddr_in *)entry->ifa_addr)->sin_addr, address,
            sizeof(address));
        const int written = snprintf(result + used, size - used, "%s%s %s",
            used == 0 ? "" : ", ", entry->ifa_name, address);
        if (written < 0 || (size_t)written >= size - used) break;
        used += (size_t)written;
    }
    freeifaddrs(list);
    if (used == 0) {
        snprintf(result, size, "no IPv4 address");
    }
    return 0;
}
