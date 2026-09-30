/* SPDX-License-Identifier: MIT
 * Compatibility layer for this phone's unmodified lhd/gpsd binaries: the
 * modem runtime's file-backed properties and filtered logging, plus the few
 * Android services the GNSS daemons reach for and that do not exist here.
 *
 * - Wake locks: libhardware_legacy asks the SystemSuspend service over
 *   Binder, and this system has no Binder driver (libbinder aborts when it
 *   cannot open one). Linux keeps the CPU awake while the daemons run; a
 *   wake lock is only a request, so these succeed as no-ops.
 * - Service manager: gpsd's main() waits for the Android sensor service
 *   (CustomerDeviceState::initialize) before it enters its event loop. There
 *   is none: the main thread gets "no service" and carries on; any other
 *   thread's wait blocks, as it would on Android, instead of opening Binder.
 * - Samsung SIT RIL client: gpsd's main() returns when it cannot connect to
 *   the RIL for cellular aiding, so its libsitril-gps.so entry points report
 *   a connected client that has nothing to say.
 * - Binder thread pool: nothing will ever call in, so joining it just parks
 *   the calling thread.
 *
 * Build like the modem shim: AArch64, -shared -fPIC -nostdlib.
 */
#include "../modem/android-shim.c"

#include <time.h>

int acquire_wake_lock(int lock, const char *id)
{
	return 0;
}

int release_wake_lock(const char *id)
{
	return 0;
}

pid_t gettid(void);	/* Bionic has it; glibc's header hides it without _GNU_SOURCE */

void *AServiceManager_waitForService(const char *instance)
{
	/* A service that never registers: Android's wait blocks forever too.
	 * Blocking gpsd's main thread, though, stops it before its event loop,
	 * so it never serves its HAL pipe or control pipe. There, answer NULL:
	 * gpsd logs "fromBinder returned nullptr manager" and starts a thread
	 * that retries (onServiceDied); that thread's wait blocks here. (The
	 * abort seen with NULL before came from main() returning after the RIL
	 * client failed, not from the missing service.)
	 */
	struct timespec day = { 86400, 0 };

	if (gettid() == getpid())
		return NULL;
	for (;;)
		nanosleep(&day, NULL);
	return NULL;
}

void *AServiceManager_checkService(const char *instance)
{
	return NULL;
}

void *AServiceManager_getService(const char *instance)
{
	return NULL;
}

bool ABinderProcess_isThreadPoolStarted(void)
{
	return true;
}

void ABinderProcess_joinThreadPool(void)
{
	struct timespec day = { 86400, 0 };

	for (;;)
		nanosleep(&day, NULL);
}

/* gpsd also logs through these two; the modem daemons never did. */
int __android_log_vprint(int priority, const char *tag, const char *format, va_list args)
{
	return log_message(format, args);
}

static int log_text(const char *format, ...)
{
	va_list args;
	int result;

	va_start(args, format);
	result = log_message(format, args);
	va_end(args);
	return result;
}

int __android_log_buf_write(int buffer, int priority, const char *tag, const char *text)
{
	return log_text("%s", text ? text : "");
}

int __android_log_write(int priority, const char *tag, const char *text)
{
	return log_text("%s", text ? text : "");
}

/* gpsd's only imports from libsitril-gps.so, which dlopen()s a Samsung RIL
 * client library that is not here; without a connection main() returns 1
 * ("Failed to start RIL client"). The aiding it would carry needs a network.
 */
int GpsOpen(void)
{
	return 0;
}

int GpsClose(void)
{
	return 0;
}

int RegisterCallback(void)
{
	return 0;
}

int RequestAGPS(void)
{
	return 0;
}
