#ifndef SERVO_PINMUX_H
#define SERVO_PINMUX_H

#include <stdint.h>

/*
 * Points a pin at its PWM function without rebuilding the image.
 *
 * On this SoC the pin multiplexing is not the kernel's pinctrl framework but a
 * board driver that walks a device tree node once at boot and writes one IO
 * config register per pin.  Nothing reclaims those registers afterwards, so
 * unbinding whatever driver owns a pin does not hand the pin back -- the mux
 * stays where boot left it, and the only way to move it is to write the
 * register again.
 *
 * Which is what this does, through /dev/mem.  The alternative is to add the
 * pin to the board's device tree and reflash, and for a pin the running image
 * does not configure at all that is a lot of ceremony for one register.
 *
 * The register is the same one the board driver writes, so the value follows
 * its layout: bits 3:0 select the function, 7:4 the drive strength, 8 and 9
 * the pulls, 10 the slew rate and 12 the input enable.  Both the address and
 * the value belong to the board rather than to this code, so they are
 * configured rather than derived.
 */

/*
 * Writes `value` to the IO config register at `address`.  Returns a negated
 * errno; -EACCES means /dev/mem was not readable, which is what a kernel built
 * without it looks like.
 */
int servo_pinmux_apply(uint32_t address, uint32_t value);

#endif
