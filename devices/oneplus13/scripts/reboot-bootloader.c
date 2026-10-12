/* SPDX-License-Identifier: MIT
 * Exercise the native reboot-reason path on this RAM probe only.
 */
#define _GNU_SOURCE
#include <linux/reboot.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

int main(void)
{
    struct utsname uts;
    char model[128] = {0};
    FILE *f = fopen("/sys/firmware/devicetree/base/model", "rb");
    if (!f || !fread(model, 1, sizeof(model) - 1, f)) return 1;
    fclose(f);
    if (uname(&uts) || !strstr(uts.release, "-omarchy-oneplus13-probe") ||
        strcmp(model, "OnePlus 13 CPH2655 Omarchy USB probe") || geteuid()) {
        fputs("Refusing reboot outside the OnePlus native probe\n", stderr);
        return 1;
    }
    sync();
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_RESTART2, "bootloader");
    perror("reboot bootloader");
    return 1;
}
