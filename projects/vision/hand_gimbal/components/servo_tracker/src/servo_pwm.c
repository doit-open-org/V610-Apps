#define _POSIX_C_SOURCE 200809L

#include "servo_pwm.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * Exporting a channel creates its directory through udev, which is not
 * necessarily finished by the time the write returns.  A handful of short
 * waits covers it without making a failure take noticeably long.
 */
#define EXPORT_ATTEMPTS 20
#define EXPORT_WAIT_MS 10

void servo_pwm_init(servo_pwm *pwm)
{
    if (pwm == NULL) return;
    *pwm = (servo_pwm) {.duty_fd = -1};
}

int servo_pwm_find_chip(const char *root, const char *device,
    unsigned int *chip)
{
    if (root == NULL || device == NULL || chip == NULL) return -EINVAL;
    DIR *dir = opendir(root);
    if (dir == NULL) return -errno;
    int status = -ENOENT;
    for (struct dirent *entry; (entry = readdir(dir)) != NULL;) {
        if (strncmp(entry->d_name, "pwmchip", 7) != 0) continue;
        /* The device link ends in the controller's name. */
        char path[512];
        char target[256];
        (void)snprintf(path, sizeof(path), "%s/%s/device", root,
            entry->d_name);
        const ssize_t length = readlink(path, target, sizeof(target) - 1);
        if (length <= 0) continue;
        target[length] = '\0';
        const char *name = strrchr(target, '/');
        if (strcmp(name == NULL ? target : name + 1, device) == 0) {
            *chip = (unsigned int)strtoul(entry->d_name + 7, NULL, 10);
            status = 0;
            break;
        }
    }
    (void)closedir(dir);
    return status;
}

static int write_file(const char *path, const char *text)
{
    const int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    const size_t length = strlen(text);
    const ssize_t written = write(fd, text, length);
    const int error = written == (ssize_t)length ? 0 : -errno;
    (void)close(fd);
    return error;
}

static int write_number(const char *path, unsigned long long value)
{
    char text[32];
    (void)snprintf(text, sizeof(text), "%llu", value);
    return write_file(path, text);
}

static void wait_briefly(void)
{
    const struct timespec delay = {
        .tv_sec = 0, .tv_nsec = EXPORT_WAIT_MS * 1000L * 1000L,
    };
    (void)nanosleep(&delay, NULL);
}

static int export_channel(unsigned int channel, const char *chip_path,
    const char *channel_path)
{
    if (access(channel_path, F_OK) == 0) return 0;
    char path[512];
    (void)snprintf(path, sizeof(path), "%s/export", chip_path);
    const int status = write_number(path, channel);
    /*
     * EBUSY means someone else exported it between the check and the write,
     * which is exactly the state that was wanted.
     */
    if (status != 0 && status != -EBUSY) return status;
    for (int attempt = 0; attempt < EXPORT_ATTEMPTS; ++attempt) {
        if (access(channel_path, F_OK) == 0) return 0;
        wait_briefly();
    }
    return -ENOENT;
}

int servo_pwm_open(servo_pwm *pwm, const char *root, unsigned int chip,
    unsigned int channel, uint32_t period_ns, uint32_t initial_ns)
{
    if (pwm == NULL || root == NULL || period_ns == 0) return -EINVAL;
    if (initial_ns > period_ns) return -EINVAL;
    servo_pwm_init(pwm);
    pwm->chip = chip;
    pwm->channel = channel;

    char chip_path[384];
    (void)snprintf(chip_path, sizeof(chip_path), "%s/pwmchip%u", root, chip);
    const char *channel_path = pwm->channel_path;
    (void)snprintf(pwm->channel_path, sizeof(pwm->channel_path), "%s/pwm%u",
        chip_path, channel);
    int status = export_channel(channel, chip_path, channel_path);
    if (status != 0) return status;

    char path[512];
    /*
     * The kernel rejects a period below the current duty cycle, so a channel
     * left running by a previous process is silenced before its period is
     * changed.  On a channel exported for the first time the period is still
     * zero, and the kernel rejects any duty cycle at all against it -- the
     * write failing there means only that there was nothing to silence.
     */
    (void)snprintf(path, sizeof(path), "%s/duty_cycle", channel_path);
    (void)write_number(path, 0);
    (void)snprintf(path, sizeof(path), "%s/period", channel_path);
    status = write_number(path, period_ns);
    if (status != 0) goto failed;
    /* Not every controller exposes polarity; normal is the default anyway. */
    (void)snprintf(path, sizeof(path), "%s/polarity", channel_path);
    (void)write_file(path, "normal");

    (void)snprintf(path, sizeof(path), "%s/duty_cycle", channel_path);
    pwm->duty_fd = open(path, O_WRONLY | O_CLOEXEC);
    if (pwm->duty_fd < 0) {
        status = -errno;
        goto failed;
    }
    status = servo_pwm_set(pwm, initial_ns);
    if (status != 0) goto failed;

    (void)snprintf(path, sizeof(path), "%s/enable", channel_path);
    status = write_number(path, 1);
    if (status != 0) goto failed;
    pwm->enabled = true;
    return 0;

failed:
    if (pwm->duty_fd >= 0) {
        (void)close(pwm->duty_fd);
        pwm->duty_fd = -1;
    }
    /*
     * The channel is left exported.  Unexporting it would cut the signal to
     * whatever else may have opened it, and an exported channel with its
     * output disabled costs nothing.
     */
    return status;
}

int servo_pwm_set(servo_pwm *pwm, uint32_t duty_ns)
{
    if (pwm == NULL || pwm->duty_fd < 0) return -EBADF;
    char text[32];
    const int length = snprintf(text, sizeof(text), "%lu",
        (unsigned long)duty_ns);
    if (length <= 0) return -EINVAL;
    /* Each write replaces the value; sysfs attributes are not append only. */
    if (lseek(pwm->duty_fd, 0, SEEK_SET) == (off_t)-1) return -errno;
    const ssize_t written = write(pwm->duty_fd, text, (size_t)length);
    return written == (ssize_t)length ? 0 : -errno;
}

void servo_pwm_close(servo_pwm *pwm, uint32_t final_ns)
{
    if (pwm == NULL) return;
    if (pwm->duty_fd >= 0) {
        /* Leave the servo stopped rather than running at its last speed. */
        (void)servo_pwm_set(pwm, final_ns);
        (void)close(pwm->duty_fd);
        pwm->duty_fd = -1;
    }
    if (pwm->enabled) {
        char path[512];
        (void)snprintf(path, sizeof(path), "%s/enable", pwm->channel_path);
        (void)write_number(path, 0);
        pwm->enabled = false;
    }
    servo_pwm_init(pwm);
}
