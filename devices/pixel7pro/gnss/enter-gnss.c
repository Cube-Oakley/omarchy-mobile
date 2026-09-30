/* SPDX-License-Identifier: MIT
 * Enter a prepared GNSS chroot, then drop every capability before exec. Like
 * modem/enter-runtime.c, with the vendor libraries on the search path. Run it
 * under unshare --mount --pid --fork --mount-proc=ROOT/proc (gnss-lab.sh). The
 * root is a dedicated tmpfs; only the GNSS driver's device nodes and sysfs
 * directory are bind-mounted into it.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <grp.h>
#include <linux/capability.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

static void fail(const char *what) { perror(what); exit(1); }
int main(int argc, char **argv)
{
	struct __user_cap_header_struct header = { _LINUX_CAPABILITY_VERSION_3, 0 };
	struct __user_cap_data_struct data[2] = {0};
	char **command;
	if (argc < 3) { fprintf(stderr, "usage: enter-runtime ROOT /vendor/bin/PROGRAM [args]\n"); return 2; }
	if (chroot(argv[1]) || chdir("/")) fail("chroot");
	if (setgroups(0, NULL)) fail("setgroups");
	for (int cap = 0; cap <= CAP_LAST_CAP; cap++)
		if (prctl(PR_CAPBSET_DROP, cap, 0, 0, 0)) fail("drop capability");
	if (syscall(SYS_capset, &header, data)) fail("capset");
	if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) fail("no_new_privs");
	if (clearenv()) fail("clearenv");
	setenv("PATH", "/system/bin", 1);
	setenv("LD_LIBRARY_PATH", "/vendor/lib64:/system/lib64", 1);
	setenv("LD_PRELOAD", "/system/lib64/pixel-android-shim.so", 1);
	command = calloc(argc, sizeof(*command));
	if (!command) fail("calloc");
	command[0] = "/system/bin/linker64";
	for (int i = 2; i < argc; i++) command[i-1] = argv[i];
	execv(command[0], command);
	fail("exec");
}
