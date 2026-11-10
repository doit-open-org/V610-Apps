#ifndef SERVO_TRACKER_H
#define SERVO_TRACKER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Points a pan and tilt mount at a target seen in the camera image.
 *
 * Either kind of hobby servo will drive the mount, and which one is attached
 * decides what the pulse width means.  A positional servo -- an MG90S, an SG90
 * and most things sold simply as "servo" -- reads the pulse as an angle, goes
 * there and holds it against its internal potentiometer.  A continuous
 * rotation servo has had that potentiometer taken out of the loop and reads
 * the same pulse as a speed, turning one way above the neutral width and the
 * other way below it, faster the further from neutral, reporting nothing.
 *
 * What the loop closes on is the same in both cases: the error in the image,
 * how far the target sits from the centre of the frame.  What differs is what
 * that error is allowed to command.
 *
 * On a continuous rotation servo the error commands a speed.  The mount's
 * angle is the integral of that speed, so the plant already integrates and
 * proportional control alone leaves no steady state error; an integral term
 * would only wind up while the servo sits in its dead band.  That dead band is
 * the other thing to respect: a continuous rotation servo ignores pulses
 * within a few tens of microseconds of neutral, so a correction smaller than
 * the band does nothing but make the servo buzz, and the loop steps over it.
 * Losing the target stops the servos and leaves the mount where it is, because
 * with nothing reporting position there is no centre to return to.
 *
 * On a positional servo the error commands a change of angle instead, added to
 * the angle already held.  The accumulation is what the continuous servo got
 * from its plant for free, and without it proportional control against a
 * position would settle wherever the error balanced the gain, leaving the
 * target permanently off centre.  Travel is finite and nothing stops the servo
 * driving into its end stop and stalling there, drawing its full stall current
 * for as long as it is asked to, so the commanded angle is clamped to the
 * mount's real range.  Losing the target holds the angle, or returns to the
 * middle of the travel if asked -- here there is a centre to return to.
 *
 * Both share a dead zone around the middle of the frame, so the mount does not
 * chase the jitter of the detection, and a slew limit, because the loop runs
 * at the inference rate and sees each correction a frame late.
 */

typedef enum {
    /* Pulse width is an angle: MG90S, SG90, most servos. */
    SERVO_TRACKER_POSITIONAL = 0,
    /* Pulse width is a speed: 360 degree and modified servos, FS90R. */
    SERVO_TRACKER_CONTINUOUS,
} servo_tracker_kind;

typedef enum {
    SERVO_TRACKER_OK = 0,
    SERVO_TRACKER_ERR_ARGUMENT,
    SERVO_TRACKER_ERR_UNAVAILABLE,
    SERVO_TRACKER_ERR_IO,
    SERVO_TRACKER_ERR_PINMUX,
    SERVO_TRACKER_ERR_MEMORY,
} servo_tracker_status;

typedef struct {
    bool enabled;
    unsigned int channel;
    /*
     * Which way the mount turns for a pulse above neutral depends on how the
     * servo is geared and mounted, and is easier to flip here than to rewire.
     */
    bool invert;
    /*
     * The pin's IO config register and the value that points it at this PWM
     * output, written once before the channel is opened.  Leave the address
     * zero when the board's own device tree already muxes the pin, which is
     * the case on a board built for a servo mount; it is set when borrowing a
     * pin the running image does not configure.
     */
    uint32_t iocfg_address;
    uint32_t iocfg_value;
    /*
     * Half the travel this axis is allowed, in microseconds either side of
     * neutral, for a positional servo whose mount reaches less far than the
     * servo does.  Zero takes the shared span.  Tilt is usually the one that
     * needs it.
     */
    uint32_t span_us;
    /*
     * Positional only: this axis's own per frame step, when one axis needs to
     * move more calmly than the other.  Zero takes the shared step.
     *
     * Tilt is usually the one that does.  The error is normalised against half
     * the frame in each direction, and the frame is wider than it is tall, so
     * the same movement of the hand in pixels reads as a larger error
     * vertically than horizontally -- on a 640 by 480 frame, a third larger.
     * Tilt geometry tends to be the more sensitive of the two as well.
     */
    uint32_t step_percent;
} servo_tracker_axis;

typedef struct {
    servo_tracker_kind kind;
    /* Directory holding the pwmchip entries; "/sys/class/pwm" on a board. */
    const char *sysfs_root;
    /*
     * The PWM controller both axes use, as the kernel names the device, for
     * example "11080000.pwm". Its pwmchip number is looked up at open, since
     * it depends on the order the drivers probed in.
     */
    const char *pwm_device;
    servo_tracker_axis pan;
    servo_tracker_axis tilt;

    /* Frame the servo expects, 20000 us for the usual 50 Hz servo. */
    uint32_t period_us;
    /* Middle of the travel, or the pulse at which a continuous servo stops. */
    uint32_t neutral_us;
    /* Half the travel, or the offset from neutral at full speed. */
    uint32_t span_us;
    /*
     * Continuous only.  Smallest offset from neutral that actually turns the
     * servo; below its dead band it only buzzes, so the loop steps over it.
     */
    uint32_t dead_band_us;

    /*
     * What the target commands with the target at the edge of the frame, as a
     * percentage: a share of full speed for a continuous servo, and for a
     * positional one a share of the per frame step below.
     */
    uint32_t gain_percent;
    /* Half width of the region around the frame centre where nothing moves. */
    uint32_t dead_zone_percent;
    /* Ceiling on that command, as a percentage. */
    uint32_t max_speed_percent;
    /* Most the command may change in one frame. */
    uint32_t slew_percent;

    /*
     * Positional only.  How far the angle may move in one frame at full
     * command, as a percentage of the span.  This sets how fast the mount can
     * sweep: at 30 frames a second, 5% of a 500 us span is 750 us of travel a
     * second, half the range.
     */
    uint32_t step_percent;
    /*
     * Positional only.  Whether losing the target returns the mount to the
     * middle of its travel rather than leaving it pointing where it was.
     */
    bool recentre_on_loss;
} servo_tracker_config;

typedef struct servo_tracker servo_tracker;

void servo_tracker_config_init(servo_tracker_config *config);

/*
 * Opens whichever axes are enabled.  Returns SERVO_TRACKER_ERR_UNAVAILABLE if
 * neither is, which a caller with no servos attached can treat as "no servos"
 * rather than as a failure.
 */
servo_tracker_status servo_tracker_open(const servo_tracker_config *config,
    servo_tracker **tracker);

void servo_tracker_close(servo_tracker *tracker);

/*
 * Steers towards a target at (x, y) in pixels within a width by height frame.
 * Pass have_target false when there is nothing to follow, which stops the
 * servos where they are.
 */
servo_tracker_status servo_tracker_track(servo_tracker *tracker,
    bool have_target, float x, float y, uint32_t width, uint32_t height);

/* Commanded speeds, each -1 to 1, for reporting. */
void servo_tracker_speeds(const servo_tracker *tracker, float *pan,
    float *tilt);

/* Pulse widths currently held, in microseconds, for reporting. */
void servo_tracker_pulses(const servo_tracker *tracker, uint32_t *pan,
    uint32_t *tilt);

const char *servo_tracker_status_string(servo_tracker_status status);

#endif
