/* SPDX-License-Identifier: MIT */
/* Bounded fbdev visual check; no panel/PMIC register access. */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(void)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) {
        perror("framebuffer");
        return 1;
    }
    if (v.xres != 1440 || v.yres != 3168 || v.bits_per_pixel != 32 || f.line_length != 5760 ||
        v.red.length != 8 || v.green.length != 8 || v.blue.length != 8 ||
        v.red.offset > 24 || v.green.offset > 24 || v.blue.offset > 24 ||
        (v.transp.length && (v.transp.length != 8 || v.transp.offset > 24))) {
        fprintf(stderr, "Unexpected framebuffer geometry/format; no writes\n");
        return 1;
    }
    size_t size = (size_t)f.line_length * v.yres;
    uint32_t *pixels = calloc(1, size);
    if (!pixels) return 1;
    for (unsigned y = 0; y < v.yres; y++) {
        for (unsigned x = 0; x < v.xres; x++) {
            unsigned r = 16, g = 20, b = 32;
            if (y > 320 && y < v.yres - 320) {
                if (x < 480) r = 160;
                else if (x < 960) g = 160;
                else b = 160;
            }
            if (((x > 50 && x < 110) || (x > 1330 && x < 1390)) &&
                ((y > 50 && y < 110) || (y > 3058 && y < 3118)))
                r = g = b = 220;
            pixels[(size_t)y * (f.line_length / 4) + x] =
                (r << v.red.offset) | (g << v.green.offset) | (b << v.blue.offset) |
                (v.transp.length ? 255U << v.transp.offset : 0);
        }
    }
    size_t done = 0;
    while (done < size) {
        ssize_t n = write(fd, (char *)pixels + done, size - done);
        if (n <= 0) { perror("framebuffer write"); return 1; }
        done += n;
    }
    printf("Wrote %zu bytes: red/green/blue vertical bands and four white corner squares\n", size);
    close(fd);
    free(pixels);
    return 0;
}
