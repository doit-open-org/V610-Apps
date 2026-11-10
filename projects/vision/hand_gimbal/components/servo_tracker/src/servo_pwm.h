#ifndef SERVO_PWM_H
#define SERVO_PWM_H

#include <stdbool.h>
#include <stdint.h>

/*
 * One PWM channel through the kernel's sysfs interface.
 *
 * /sys/class/pwm/pwmchipN is the generic Linux PWM interface, so nothing here
 * is specific to this SoC: a channel is exported, given a period and a duty
 * cycle in nanoseconds, and enabled.  The hardware then produces the waveform
 * on its own, which is what makes a 50 Hz servo signal practical from user
 * space -- there is no timing requirement on the process at all, only on the
 * values it writes.
 *
 * A channel already exported by something else is used as it is found rather
 * than re-exported, and is left exported on close.  Unexporting a channel that
 * another process opened would cut its signal.
 */

#define SERVO_PWM_PATH_MAX 448

typedef struct {
    int duty_fd;
    unsigned int chip;
    unsigned int channel;
    /* Kept so the channel can be disabled without rebuilding the path. */
    char channel_path[SERVO_PWM_PATH_MAX];
    bool enabled;
} servo_pwm;

void servo_pwm_init(servo_pwm *pwm);

/*
 * Finds the pwmchip under `root` whose device is named `device`, such as
 * "11080000.pwm".  Returns a negated errno, -ENOENT when there is none.
 */
int servo_pwm_find_chip(const char *root, const char *device,
    unsigned int *chip);

/*
 * Exports the channel if needed, sets the period, parks the output at
 * `initial_ns` and enables it.  Returns a negated errno on failure, leaving
 * the channel closed.
 *
 * `root` is the directory holding the pwmchip entries, normally
 * "/sys/class/pwm"; it is a parameter so the tests can point at a directory
 * they have built themselves.
 */
int servo_pwm_open(servo_pwm *pwm, const char *root, unsigned int chip,
    unsigned int channel, uint32_t period_ns, uint32_t initial_ns);

/* Writes a new duty cycle.  Cheap enough to call once per frame. */
int servo_pwm_set(servo_pwm *pwm, uint32_t duty_ns);

/* Parks the output at `final_ns`, disables the channel and closes it. */
void servo_pwm_close(servo_pwm *pwm, uint32_t final_ns);

#endif
