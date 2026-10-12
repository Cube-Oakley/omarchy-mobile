/* SPDX-License-Identifier: MIT
 * Read-only sampling of the captured CPH2655 panel TE pin (TLMM GPIO86).
 * Does not request the GPIO, change pinmux, or write hardware registers.
 * TE cadence is not proof of rendered frame rate or panel mode programming.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

int main(void)
{
    char model[128] = {0};
    struct utsname uts;
    FILE *f = fopen("/sys/firmware/devicetree/base/model", "rb");
    if (!f || !fread(model, 1, sizeof(model) - 1, f)) return 1;
    fclose(f);
    if (uname(&uts) || !strstr(uts.release, "-omarchy-oneplus13-probe") ||
        strcmp(model, "OnePlus 13 CPH2655 Omarchy USB probe")) return 1;
    int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
    if (fd < 0) { perror("open"); return 1; }
    const volatile uint32_t *reg = mmap(NULL, 4096, PROT_READ, MAP_SHARED,
                                       fd, 0x0f156000);
    if (reg == MAP_FAILED) { perror("mmap"); close(fd); return 1; }
    uint32_t config = reg[0], last = reg[1] & 1;
    uint64_t start = now_ns(), first = 0, prev = 0, current;
    unsigned rises = 0;
    printf("GPIO86 config=0x%08x initial=%u\n", config, last);
    while ((current = now_ns()) - start < 3000000000ULL) {
        uint32_t bit = reg[1] & 1;
        if (bit && !last) {
            if (!rises) first = current;
            if (rises < 20 && prev) printf("interval_us=%.1f\n", (current-prev)/1000.0);
            prev = current;
            rises++;
        }
        last = bit;
        struct timespec delay = {.tv_nsec = 50000};
        nanosleep(&delay, NULL);
    }
    printf("rising_edges=%u observed_hz=%.3f config_unchanged=%d\n", rises,
           rises > 1 ? (rises-1)*1e9/(prev-first) : 0.0, config == reg[0]);
    munmap((void *)reg, 4096);
    close(fd);
    return 0;
}
