#define _POSIX_C_SOURCE 200809L

#include "servo_pinmux.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

int servo_pinmux_apply(uint32_t address, uint32_t value)
{
    if (address == 0 || (address & 3U) != 0) return -EINVAL;
    const int fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) return -errno;

    const long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        (void)close(fd);
        return -EINVAL;
    }
    const uint32_t page_mask = (uint32_t)page_size - 1U;
    const off_t page = (off_t)(address & ~page_mask);
    volatile unsigned char *mapping = mmap(NULL, (size_t)page_size,
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
    const int mapped = mapping == MAP_FAILED ? -errno : 0;
    (void)close(fd);
    if (mapped != 0) return mapped;

    volatile uint32_t *reg = (volatile uint32_t *)(mapping +
        (address & page_mask));
    *reg = value;
    /* Read back, both to flush the write and to prove the register took it. */
    const uint32_t seen = *reg;
    (void)munmap((void *)mapping, (size_t)page_size);
    return seen == value ? 0 : -EIO;
}
