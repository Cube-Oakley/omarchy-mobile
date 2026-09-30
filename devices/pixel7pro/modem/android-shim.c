/* SPDX-License-Identifier: MIT
 * Small compatibility layer for this phone's unmodified cbd/rfsd binaries.
 * Properties are ordinary files within the RAM-only chroot. Log values that
 * might contain subscriber/device identity are removed before reaching stderr.
 * Build with the AArch64 compiler, -shared -fPIC -nostdlib: Bionic resolves
 * these libc calls at runtime; do not link against the host's glibc.
 */
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int property_path(char *path, size_t size, const char *key)
{
	if (!key || !*key || strlen(key) > 96)
		return -1;
	for (const char *p = key; *p; ++p)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '_' || *p == '.' || *p == '-'))
			return -1;
	return snprintf(path, size, "/properties/%s", key) >= (int)size ? -1 : 0;
}

int property_get(const char *key, char *value, const char *fallback)
{
	char path[128];
	ssize_t n = -1;
	int fd;

	if (!property_path(path, sizeof(path), key)) {
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			n = read(fd, value, 91);
			close(fd);
		}
	}
	if (n < 0)
		n = snprintf(value, 92, "%s", fallback ? fallback : "");
	if (n > 91)
		n = 91;
	value[n] = 0;
	return n;
}

int property_set(const char *key, const char *value)
{
	char path[128], temporary[160];
	int fd, result;
	size_t n;

	if (property_path(path, sizeof(path), key) || !value || (n = strlen(value)) > 91)
		return -1;
	snprintf(temporary, sizeof(temporary), "%s.%d.tmp", path, getpid());
	fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	result = write(fd, value, n) == (ssize_t)n ? 0 : -1;
	close(fd);
	if (!result)
		result = rename(temporary, path);
	if (result)
		unlink(temporary);
	return result;
}

int32_t property_get_int32(const char *key, int32_t fallback)
{
	char value[92], *end;
	long number;
	if (!property_get(key, value, ""))
		return fallback;
	number = strtol(value, &end, 0);
	return *end || number < INT32_MIN || number > INT32_MAX ? fallback : number;
}

bool property_get_bool(const char *key, bool fallback)
{
	char value[92];
	if (!property_get(key, value, ""))
		return fallback;
	if (!strcmp(value, "1") || !strcmp(value, "true") || !strcmp(value, "y") ||
	    !strcmp(value, "yes") || !strcmp(value, "on"))
		return true;
	if (!strcmp(value, "0") || !strcmp(value, "false") || !strcmp(value, "n") ||
	    !strcmp(value, "no") || !strcmp(value, "off"))
		return false;
	return fallback;
}

static int log_message(const char *format, va_list args)
{
	char line[2048], lower[2048];
	size_t len;
	vsnprintf(line, sizeof(line), format, args);
	len = strlen(line);
	for (size_t i = 0; i <= len; i++)
		lower[i] = line[i] >= 'A' && line[i] <= 'Z' ? line[i] + 32 : line[i];
	if (strstr(lower, "imei") || strstr(lower, "imsi") || strstr(lower, "iccid") ||
	    strstr(lower, "cpid") || strstr(lower, "cpsha") || strstr(lower, "cpsig") ||
	    strstr(lower, "serial") || strstr(lower, "chosen/config"))
		return 0;
	/* Also suppress unlabeled long decimal identifiers. */
	for (size_t i = 0, digits = 0; i < len; i++) {
		digits = line[i] >= '0' && line[i] <= '9' ? digits + 1 : 0;
		if (digits >= 14)
			return 0;
	}
	write(STDERR_FILENO, line, len);
	write(STDERR_FILENO, "\n", 1);
	return 0;
}

int __android_log_print(int priority, const char *tag, const char *format, ...)
{
	va_list args;
	int result;
	va_start(args, format);
	result = log_message(format, args);
	va_end(args);
	return result;
}

int __android_log_buf_print(int buffer, int priority, const char *tag, const char *format, ...)
{
	va_list args;
	int result;
	va_start(args, format);
	result = log_message(format, args);
	va_end(args);
	return result;
}
