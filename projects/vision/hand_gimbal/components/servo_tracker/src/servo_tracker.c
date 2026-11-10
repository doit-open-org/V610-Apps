#include "servo_tracker.h"
#include "servo_pinmux.h"
#include "servo_pwm.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define US_TO_NS 1000U

typedef struct {
    servo_tracker_axis config;
    servo_pwm pwm;
    bool open;
    /* Last command, -1 to 1, kept for slew limiting. */
    float speed;
    /* Half the travel allowed on this axis, in microseconds. */
    uint32_t span_us;
    /* This axis's per frame step, as a percentage of its span. */
    uint32_t step_percent;
    /* Positional only: the angle currently held, as a pulse width. */
    float pulse_us;
} axis_state;

struct servo_tracker {
    servo_tracker_config config;
    unsigned int chip;
    axis_state pan;
    axis_state tilt;
};

void servo_tracker_config_init(servo_tracker_config *config)
{
    if (config == NULL) return;
    *config = (servo_tracker_config) {
        .kind = SERVO_TRACKER_POSITIONAL,
        .sysfs_root = "/sys/class/pwm",
        .pwm_device = "11080000.pwm",
        .pan = {.enabled = false, .channel = 5},
        .tilt = {.enabled = false, .channel = 1},
        .period_us = 20000,
        .neutral_us = 1500,
        .span_us = 500,
        .dead_band_us = 40,
        .gain_percent = 60,
        .dead_zone_percent = 8,
        .max_speed_percent = 70,
        .slew_percent = 25,
        .step_percent = 5,
        .recentre_on_loss = false,
    };
}

const char *servo_tracker_status_string(servo_tracker_status status)
{
    switch (status) {
    case SERVO_TRACKER_OK:
        return "ok";
    case SERVO_TRACKER_ERR_ARGUMENT:
        return "invalid argument";
    case SERVO_TRACKER_ERR_UNAVAILABLE:
        return "no servo configured";
    case SERVO_TRACKER_ERR_IO:
        return "pwm access failed";
    case SERVO_TRACKER_ERR_PINMUX:
        return "pin mux write failed";
    case SERVO_TRACKER_ERR_MEMORY:
        return "out of memory";
    default:
        return "unknown";
    }
}

static float clamp(float value, float low, float high)
{
    return value < low ? low : value > high ? high : value;
}

/*
 * Turns a speed of -1 to 1 into a pulse width, for a continuous servo.
 *
 * Anything inside the servo's dead band would be a pulse it ignores, so the
 * usable range starts at the far side of the band: a speed just above zero
 * asks for the slowest movement the servo will actually make, not for a pulse
 * that does nothing.
 */
static float pulse_for_speed(const servo_tracker_config *config,
    const axis_state *axis, float speed)
{
    if (!(fabsf(speed) > 0.0F)) return (float)config->neutral_us;
    const float magnitude = clamp(fabsf(speed), 0.0F, 1.0F);
    const float band = (float)config->dead_band_us;
    const float reach = (float)axis->span_us - band;
    const float offset = band + magnitude * (reach > 0.0F ? reach : 0.0F);
    return speed > 0.0F ?
        (float)config->neutral_us + offset :
        (float)config->neutral_us - offset;
}

/*
 * Moves a positional servo's angle by this frame's command.
 *
 * The command is a rate of change here rather than a position, because a
 * positional servo holds whatever angle it is given: driving the angle itself
 * proportionally to the error would settle wherever the error balanced the
 * gain and leave the target off centre for good.  Accumulating instead is what
 * the continuous servo gets from its own plant.
 *
 * The clamp is not a refinement.  A positional servo asked past its end stop
 * stalls there and keeps drawing its stall current, so the travel has to be
 * the mount's, not the servo's.
 */
static float pulse_for_step(const servo_tracker_config *config,
    const axis_state *axis, float command, bool have_target)
{
    const float centre = (float)config->neutral_us;
    const float span = (float)axis->span_us;
    float pulse = axis->pulse_us;
    const float step = span * (float)axis->step_percent / 100.0F;
    if (!have_target && config->recentre_on_loss) {
        /* Walk back to the middle at the same rate the tracking moves. */
        const float remaining = centre - pulse;
        pulse += clamp(remaining, -step, step);
    } else {
        pulse += command * step;
    }
    return clamp(pulse, centre - span, centre + span);
}

/*
 * Decides this frame's speed for one axis from the error in the image.
 *
 * `error` is the offset from the centre of the frame, scaled so that 1 is the
 * edge.  Inside the dead zone the axis stops; outside it the speed is
 * proportional, capped, and allowed to change only so fast.
 */
static float axis_speed(const servo_tracker_config *config,
    const axis_state *axis, bool have_target, float error)
{
    float wanted = 0.0F;
    if (have_target) {
        const float dead_zone = (float)config->dead_zone_percent / 100.0F;
        if (fabsf(error) > dead_zone) {
            const float gain = (float)config->gain_percent / 100.0F;
            const float ceiling = (float)config->max_speed_percent / 100.0F;
            wanted = clamp(error * gain, -ceiling, ceiling);
            if (axis->config.invert) wanted = -wanted;
        }
    }
    const float slew = (float)config->slew_percent / 100.0F;
    const float change = clamp(wanted - axis->speed, -slew, slew);
    return axis->speed + change;
}

static servo_tracker_status open_axis(axis_state *axis,
    const servo_tracker_config *config, unsigned int chip,
    const servo_tracker_axis *setup)
{
    axis->config = *setup;
    axis->speed = 0.0F;
    axis->span_us = setup->span_us != 0 ? setup->span_us : config->span_us;
    axis->step_percent = setup->step_percent != 0 ?
        setup->step_percent : config->step_percent;
    /* Both kinds start from the middle: stopped, or pointing straight on. */
    axis->pulse_us = (float)config->neutral_us;
    servo_pwm_init(&axis->pwm);
    if (!setup->enabled) return SERVO_TRACKER_OK;
    /* The pin has to reach the PWM block before the channel is worth opening. */
    if (setup->iocfg_address != 0 &&
        servo_pinmux_apply(setup->iocfg_address, setup->iocfg_value) != 0) {
        return SERVO_TRACKER_ERR_PINMUX;
    }
    const int status = servo_pwm_open(&axis->pwm, config->sysfs_root,
        chip, setup->channel, config->period_us * US_TO_NS,
        config->neutral_us * US_TO_NS);
    if (status != 0) return SERVO_TRACKER_ERR_IO;
    axis->open = true;
    return SERVO_TRACKER_OK;
}

servo_tracker_status servo_tracker_open(const servo_tracker_config *config,
    servo_tracker **out)
{
    if (config == NULL || out == NULL) return SERVO_TRACKER_ERR_ARGUMENT;
    *out = NULL;
    if (!config->pan.enabled && !config->tilt.enabled) {
        return SERVO_TRACKER_ERR_UNAVAILABLE;
    }
    if (config->sysfs_root == NULL || config->pwm_device == NULL ||
        config->period_us == 0 ||
        config->neutral_us == 0 || config->span_us == 0 ||
        config->neutral_us + config->span_us > config->period_us ||
        config->span_us >= config->neutral_us) {
        return SERVO_TRACKER_ERR_ARGUMENT;
    }
    if (config->kind == SERVO_TRACKER_CONTINUOUS &&
        config->dead_band_us >= config->span_us) {
        return SERVO_TRACKER_ERR_ARGUMENT;
    }
    if (config->kind == SERVO_TRACKER_POSITIONAL &&
        (config->step_percent == 0 || config->step_percent > 100)) {
        return SERVO_TRACKER_ERR_ARGUMENT;
    }
    if (config->pan.span_us > config->span_us ||
        config->tilt.span_us > config->span_us ||
        config->pan.step_percent > 100 || config->tilt.step_percent > 100) {
        return SERVO_TRACKER_ERR_ARGUMENT;
    }

    servo_tracker *tracker = calloc(1, sizeof(*tracker));
    if (tracker == NULL) return SERVO_TRACKER_ERR_MEMORY;
    tracker->config = *config;
    servo_tracker_status status = SERVO_TRACKER_OK;
    if (servo_pwm_find_chip(config->sysfs_root, config->pwm_device,
            &tracker->chip) != 0) {
        status = SERVO_TRACKER_ERR_IO;
    }
    if (status == SERVO_TRACKER_OK) {
        status = open_axis(&tracker->pan, config, tracker->chip, &config->pan);
    }
    if (status == SERVO_TRACKER_OK) {
        status = open_axis(&tracker->tilt, config, tracker->chip,
            &config->tilt);
    }
    if (status != SERVO_TRACKER_OK) {
        servo_tracker_close(tracker);
        return status;
    }
    *out = tracker;
    return SERVO_TRACKER_OK;
}

void servo_tracker_close(servo_tracker *tracker)
{
    if (tracker == NULL) return;
    const uint32_t neutral = tracker->config.neutral_us * US_TO_NS;
    if (tracker->pan.open) servo_pwm_close(&tracker->pan.pwm, neutral);
    if (tracker->tilt.open) servo_pwm_close(&tracker->tilt.pwm, neutral);
    free(tracker);
}

static servo_tracker_status drive_axis(const servo_tracker_config *config,
    axis_state *axis, bool have_target, float error)
{
    const float command = axis_speed(config, axis, have_target, error);
    axis->speed = command;
    axis->pulse_us = config->kind == SERVO_TRACKER_CONTINUOUS ?
        pulse_for_speed(config, axis, command) :
        pulse_for_step(config, axis, command, have_target);
    if (!axis->open) return SERVO_TRACKER_OK;
    const uint32_t pulse = (uint32_t)lrintf(axis->pulse_us);
    return servo_pwm_set(&axis->pwm, pulse * US_TO_NS) == 0 ?
        SERVO_TRACKER_OK : SERVO_TRACKER_ERR_IO;
}

servo_tracker_status servo_tracker_track(servo_tracker *tracker,
    bool have_target, float x, float y, uint32_t width, uint32_t height)
{
    if (tracker == NULL) return SERVO_TRACKER_ERR_ARGUMENT;
    if (have_target && (width == 0 || height == 0 || !isfinite(x) ||
        !isfinite(y))) return SERVO_TRACKER_ERR_ARGUMENT;

    /* Offset from the centre of the frame, where 1 is the edge. */
    const float error_x = have_target ?
        clamp((x - (float)width * 0.5F) / ((float)width * 0.5F),
            -1.0F, 1.0F) : 0.0F;
    const float error_y = have_target ?
        clamp((y - (float)height * 0.5F) / ((float)height * 0.5F),
            -1.0F, 1.0F) : 0.0F;

    const servo_tracker_status pan = drive_axis(&tracker->config,
        &tracker->pan, have_target, error_x);
    const servo_tracker_status tilt = drive_axis(&tracker->config,
        &tracker->tilt, have_target, error_y);
    return pan != SERVO_TRACKER_OK ? pan : tilt;
}

void servo_tracker_speeds(const servo_tracker *tracker, float *pan,
    float *tilt)
{
    if (pan != NULL) *pan = tracker == NULL ? 0.0F : tracker->pan.speed;
    if (tilt != NULL) *tilt = tracker == NULL ? 0.0F : tracker->tilt.speed;
}

void servo_tracker_pulses(const servo_tracker *tracker, uint32_t *pan,
    uint32_t *tilt)
{
    if (pan != NULL) {
        *pan = tracker == NULL ? 0 : (uint32_t)lrintf(tracker->pan.pulse_us);
    }
    if (tilt != NULL) {
        *tilt = tracker == NULL ? 0 : (uint32_t)lrintf(tracker->tilt.pulse_us);
    }
}
