// SPDX-License-Identifier: GPL-2.0-only
/* Recovery serial-shell PID1; optional persistent Arch bootstrap. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int logfd = -1;
static volatile sig_atomic_t stop;

static void logmsg(const char *fmt, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (logfd >= 0) dprintf(logfd, "<5>PIXEL SHELL: %s\n", text);
}

static void request_stop(int sig)
{
    stop = sig;
}

static void mount_ram_fs(const char *type, const char *path, const char *opts)
{
    mkdir(path, 0755);
    if (mount(type, path, type, 0, opts) && errno != EBUSY)
        logmsg("mount %s: %s", path, strerror(errno));
}

static unsigned int runtime_limit(void)
{
    char cmdline[8192];
    FILE *f = fopen("/proc/cmdline", "r");
    unsigned int limit = 600;
    if (f) {
        if (fgets(cmdline, sizeof(cmdline), f)) {
            for (char *p = strtok(cmdline, " \n"); p; p = strtok(NULL, " \n")) {
                const char key[] = "pixel_test_seconds=";
                if (!strncmp(p, key, sizeof(key) - 1)) {
                    char *end;
                    unsigned long n = strtoul(p + sizeof(key) - 1, &end, 10);
                    if (!*end && n <= 86400) limit = n;
                }
            }
        }
        fclose(f);
    }
    return limit;
}

static void serial_shell(void)
{
    struct termios t;
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
    if (setsid() < 0) _exit(110);
    int fd = open("/dev/ttyGS0", O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) _exit(111);
    if (tcgetattr(fd, &t)) _exit(112);
    t.c_iflag = ICRNL | IXON;
    t.c_oflag = OPOST | ONLCR;
    t.c_cflag = CS8 | CREAD | CLOCAL | HUPCL;
    t.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;
    t.c_cc[VINTR] = 3; t.c_cc[VQUIT] = 28; t.c_cc[VERASE] = 127;
    t.c_cc[VKILL] = 21; t.c_cc[VEOF] = 4; t.c_cc[VSUSP] = 26;
    t.c_cc[VSTART] = 17; t.c_cc[VSTOP] = 19;
    t.c_cc[VMIN] = 1; t.c_cc[VTIME] = 0;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    if (tcsetattr(fd, TCSANOW, &t) || ioctl(fd, TIOCSCTTY, 0)) _exit(113);
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK)) _exit(114);
    for (int i = 0; i < 3; i++) if (dup2(fd, i) < 0) _exit(115);
    if (fd > 2) close(fd);
    chdir("/root");
    puts("Pixel 7 Pro native Linux — recovery shell over USB");
#ifdef PIXEL_PERSISTENT_ROOT
    puts("Recovery shell is in RAM; persistent Arch is mounted at /run/arch.");
#else
    puts("Files are in RAM. Run reboot to return to Android.");
#endif
    fflush(stdout);
    char *argv[] = {"sh", "-i", NULL};
    char *env[] = {"PATH=/bin:/sbin:/usr/bin:/usr/sbin", "HOME=/root",
                   "TERM=vt100", "PS1=pixel-linux# ", "USER=root", "LOGNAME=root", NULL};
    execve("/bin/sh", argv, env);
    _exit(116);
}

#ifdef PIXEL_PERSISTENT_ROOT
static int stop_modem(void)
{
    const char *service = "/usr/local/lib/omarchy-mobile/modem/manager.py";
    if (access("/run/arch/run/pixel-modem", F_OK)) return 0;
    pid_t child = fork();
    if (!child) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (chroot("/run/arch") || chdir("/")) _exit(120);
        execl("/usr/bin/python3", "python3", service, "stop", (char *)NULL);
        _exit(121);
    }
    if (child < 0) return -1;
    /* Keep the watchdog/RFS alive throughout the ordered stop. A failed
     * shutdown leaves the recovery shell available instead of killing RFS
     * while CP might still be using it. */
    for (int i = 0; i < 240; i++) {
        int status;
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child)
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        if (done < 0 && errno != EINTR) return -1;
        sleep(1);
    }
    kill(child, SIGTERM);
    return -1;
}
#endif

int main(void)
{
    struct utsname u;
    struct timespec start, now;
    pid_t shell = -1;
    mount_ram_fs("devtmpfs", "/dev", NULL);
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd >= 0) {
        for (int i = 0; i < 3; i++) dup2(nullfd, i);
        if (nullfd > 2) close(nullfd);
    }
    logfd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    mount_ram_fs("proc", "/proc", NULL);
    mount_ram_fs("sysfs", "/sys", NULL);
    mount_ram_fs("tmpfs", "/tmp", "mode=1777");
    mount_ram_fs("tmpfs", "/run", "mode=0755");
    mount_ram_fs("devpts", "/dev/pts", "mode=0620,ptmxmode=0666");
    sethostname("pixel-linux", strlen("pixel-linux"));
    umask(022);
    mkdir("/root", 0700);
    struct sigaction sa = {.sa_handler = request_stop};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    unsigned int limit = runtime_limit();
    uname(&u);
    logmsg("PID1 ready: %s %s, runtime limit=%u (0=manual reboot)", u.sysname, u.release, limit);
#ifdef PIXEL_USB_NETWORK
    pid_t network = fork();
    if (!network) {
        execl("/bin/sh", "sh", "/etc/pixel-usb-network.sh", (char *)NULL);
        _exit(117);
    }
    logmsg("USB network setup pid=%d", network);
#endif
#ifdef PIXEL_PERSISTENT_ROOT
    pid_t desktop = fork();
    if (!desktop) {
        int fd = open("/run/pixel-persistent.log", O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            dup2(fd, 1);
            dup2(fd, 2);
            if (fd > 2) close(fd);
        }
        execl("/bin/sh", "sh", "/etc/pixel-persistent-start.sh", (char *)NULL);
        _exit(118);
    }
    logmsg("persistent root startup pid=%d; recovery shell remains available", desktop);
#endif
    clock_gettime(CLOCK_MONOTONIC, &start);
    unsigned int last_status = 0;
#ifdef PIXEL_PERSISTENT_ROOT
restart_wait:
#endif
    while (!stop) {
        int status;
        pid_t child;
        while ((child = waitpid(-1, &status, WNOHANG)) > 0) {
            if (child == shell) {
                logmsg("shell pid=%d exited status=%d; restarting", child, status);
                shell = -1;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        unsigned int elapsed = now.tv_sec - start.tv_sec;
        if (limit && elapsed >= limit) break;
        if (shell < 0) {
            shell = fork();
            if (!shell) serial_shell();
            if (shell > 0) logmsg("serial shell pid=%d", shell);
            else logmsg("fork failed: %s", strerror(errno));
        }
        if (elapsed >= last_status + 30) {
            logmsg("alive %us, serial shell pid=%d", elapsed, shell);
            last_status = elapsed;
        }
        sleep(1);
    }
    logmsg("reboot requested (signal=%d)", stop);
#ifdef PIXEL_PERSISTENT_ROOT
    if (stop_modem()) {
        logmsg("modem shutdown incomplete; keeping recovery and RFS running");
        stop = 0;
        limit = 0;
        goto restart_wait;
    }
    /* Stop writers before flushing the persistent filesystem. */
    kill(-1, SIGTERM);
    sleep(2);
    kill(-1, SIGKILL);
    sync();
    if (mount(NULL, "/run/arch", NULL, MS_REMOUNT | MS_RDONLY, NULL))
        logmsg("persistent root remount read-only: %s", strerror(errno));
#endif
    sync();
    reboot(RB_AUTOBOOT);
    for (;;) pause();
}
