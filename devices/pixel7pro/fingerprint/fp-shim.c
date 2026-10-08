/* SPDX-License-Identifier: MIT
 * Compatibility layer for the stock fingerprint stack (servicemanager and
 * Goodix's HAL) in the fingerprint lab: the modem runtime's file-backed
 * properties and filtered logging. Unlike the GNSS shim it leaves Binder
 * alone: the lab mounts binderfs, and the HAL is a real AIDL service.
 *
 * Logging: libbase's LOG() goes through __android_log_write_log_message()
 * and fatal ones through __android_log_assert(); without logd both were
 * lost, so an abort said nothing. They go to stderr here, tagged, through the
 * same identity filter.
 *
 * SELinux: this system has none, so libselinux cannot open its status page
 * and servicemanager aborts (CHECK(selinux_status_open(true) >= 0)). Its
 * policy checks are answered here: fixed contexts, every access allowed.
 * The lab's real limits are the chroot and running without capabilities.
 *
 * Build like the modem shim: AArch64, -std=gnu11 -shared -fPIC -nostdlib,
 * plus -mno-outline-atomics (its atomics would call libgcc helpers).
 */
#include "../modem/android-shim.c"

/* Binder: servicemanager asks the driver for every caller's security context
 * (BINDER_SET_CONTEXT_MGR_EXT, FLAT_BINDER_FLAG_TXN_SECURITY_CTX). No LSM in
 * this kernel provides one, so every transaction to it would fail with
 * -EOPNOTSUPP. Refusing the request makes libbinder fall back to the plain
 * BINDER_SET_CONTEXT_MGR, and servicemanager asks getpidcon() (below)
 * instead. Every other ioctl goes straight to the kernel. */
#define BINDER_SET_CONTEXT_MGR_EXT	0x4018620du	/* _IOW('b', 13, 24-byte flat_binder_object) */
#define NR_IOCTL			29		/* arm64 */
#define EINVAL_				22
extern int *__errno(void);			/* Bionic's errno */
extern long syscall(long number, ...);

int ioctl(int fd, int request, ...)
{
	va_list args;
	void *arg;

	va_start(args, request);
	arg = va_arg(args, void *);
	va_end(args);
	if ((unsigned int)request == BINDER_SET_CONTEXT_MGR_EXT) {
		*__errno() = EINVAL_;
		return -1;
	}
	return syscall(NR_IOCTL, fd, request, arg);
}

/* Service lookups of Android services that do not exist here fail at once:
 * - the power HAL: Goodix's HAL boosts the CPUs around every operation and
 *   looks it up each time with AServiceManager_getService(), which waits up
 *   to 5 s, four times per operation;
 * - the sensor service: at every touch the HAL reads ambient light through
 *   libsensorndkbridge, whose AServiceManager_waitForService() never gives
 *   up, so no capture ever finished.
 * Both callers carry on without them (no boost; no light reading, as
 * libsensorndkbridge does without a sensor manager). Every other lookup goes
 * to libbinder_ndk. */
#define RTLD_NEXT	((void *)-1L)	/* Bionic, LP64 */
extern void *dlsym(void *handle, const char *symbol);

static bool absent_service(const char *instance)
{
	static const char *const absent[] = {
		"android.hardware.power.IPower/",
		"android.frameworks.sensorservice.ISensorManager/",
	};

	for (size_t i = 0; instance && i < sizeof(absent) / sizeof(*absent); i++)
		if (!strncmp(instance, absent[i], strlen(absent[i])))
			return true;
	return false;
}

static void *lookup(const char *function, const char *instance)
{
	void *(*real)(const char *) = (void *(*)(const char *))dlsym(RTLD_NEXT, function);

	return real && !absent_service(instance) ? real(instance) : NULL;
}

void *AServiceManager_getService(const char *instance)
{
	return lookup("AServiceManager_getService", instance);
}

void *AServiceManager_waitForService(const char *instance)
{
	return lookup("AServiceManager_waitForService", instance);
}

void *AServiceManager_checkService(const char *instance)
{
	return lookup("AServiceManager_checkService", instance);
}

bool AServiceManager_isDeclared(const char *instance)
{
	bool (*real)(const char *) = (bool (*)(const char *))dlsym(RTLD_NEXT,
								    "AServiceManager_isDeclared");

	return real && !absent_service(instance) && real(instance);
}

/* System properties through libc (libbase's GetProperty() and
 * WaitForPropertyCreation(): KeyMint's HAL configures its trusted app with
 * the OS version and patch levels). There is no property area here, so
 * names with a file in /properties, the modem shim's store, get a
 * prop_info of their own; everything else goes to Bionic, which finds
 * nothing. */
typedef void (*property_callback)(void *cookie, const char *name, const char *value,
				  uint32_t serial);

static struct file_property {
	char name[96];
} file_properties[64];
static int file_property_count;

static bool is_file_property(const void *pi)
{
	return (const char *)pi >= (const char *)file_properties &&
	       (const char *)pi < (const char *)(file_properties + 64);
}

static void *real_function(void **slot, const char *name)
{
	if (!*slot)
		*slot = dlsym(RTLD_NEXT, name);
	return *slot;
}

const void *__system_property_find(const char *name)
{
	static void *real;
	const void *(*find)(const char *);
	char value[92];
	int i, count;

	if (name && property_get(name, value, NULL) > 0) {
		count = __atomic_load_n(&file_property_count, __ATOMIC_ACQUIRE);
		for (i = 0; i < count; i++)
			if (!strcmp(file_properties[i].name, name))
				return &file_properties[i];
		i = __atomic_fetch_add(&file_property_count, 1, __ATOMIC_ACQ_REL);
		if (i < 64 && strlen(name) < sizeof(file_properties[i].name)) {
			strcpy(file_properties[i].name, name);
			return &file_properties[i];
		}
	}
	find = (const void *(*)(const char *))real_function(&real, "__system_property_find");
	return find ? find(name) : NULL;
}

void __system_property_read_callback(const void *pi, property_callback callback, void *cookie)
{
	static void *real;
	void (*read)(const void *, property_callback, void *);
	char value[92];

	if (is_file_property(pi)) {
		const char *name = ((const struct file_property *)pi)->name;

		property_get(name, value, "");
		callback(cookie, name, value, 0);
		return;
	}
	read = (void (*)(const void *, property_callback, void *))
		real_function(&real, "__system_property_read_callback");
	if (read)
		read(pi, callback, cookie);
}

/* Wake locks (libhardware_legacy): storageproxyd takes one around every RPMB
 * request, through Android's SystemSuspend service, which does not exist
 * here. A wake lock is only a request; these succeed as no-ops, as in the
 * GNSS shim. */
int acquire_wake_lock(int lock, const char *id)
{
	return 0;
}

int release_wake_lock(const char *id)
{
	return 0;
}

int selinux_status_open(int fallback)
{
	return 0;
}

int selinux_status_updated(void)
{
	return 0;
}

static int context(char **con, const char *value)
{
	*con = strdup(value);
	return *con ? 0 : -1;
}

int getcon(char **con)
{
	return context(con, "u:r:servicemanager:s0");
}

int getpidcon(int pid, char **con)
{
	return context(con, "u:r:fingerprint_lab:s0");
}

void freecon(char *con)
{
	free(con);
}

void *selinux_android_service_context_handle(void)
{
	static int handle;

	return &handle;
}

void *selinux_android_vendor_service_context_handle(void)	/* vndservicemanager */
{
	return selinux_android_service_context_handle();
}

int selabel_lookup(void *handle, char **con, const char *key, int type)
{
	return context(con, "u:object_r:default_android_service:s0");
}

void selabel_close(void *handle)
{
}

int selinux_check_access(const char *scon, const char *tcon, const char *tclass,
			 const char *perm, void *auditdata)
{
	return 0;
}

union selinux_callback {
	void *ptr;
};

void selinux_set_callback(int type, union selinux_callback cb)
{
}

int selinux_log_callback(int type, const char *format, ...)
{
	return 0;
}

int selinux_vendor_log_callback(int type, const char *format, ...)
{
	return 0;
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

int __android_log_vprint(int priority, const char *tag, const char *format, va_list args)
{
	return log_message(format, args);
}

int __android_log_buf_write(int buffer, int priority, const char *tag, const char *text)
{
	return log_text("%s: %s", tag ? tag : "?", text ? text : "");
}

int __android_log_write(int priority, const char *tag, const char *text)
{
	return log_text("%s: %s", tag ? tag : "?", text ? text : "");
}

/* liblog's struct __android_log_message (API 30) */
struct android_log_message {
	size_t struct_size;
	int32_t buffer_id;
	int32_t priority;
	const char *tag;
	const char *file;
	uint32_t line;
	const char *message;
};

void __android_log_write_log_message(struct android_log_message *msg)
{
	if (msg)
		log_text("%s: %s", msg->tag ? msg->tag : "?", msg->message ? msg->message : "");
}

void __android_log_assert(const char *cond, const char *tag, const char *format, ...)
{
	va_list args;

	log_text("%s: assertion %s", tag ? tag : "?", cond ? cond : "");
	if (format) {
		va_start(args, format);
		log_message(format, args);
		va_end(args);
	}
	abort();
}
