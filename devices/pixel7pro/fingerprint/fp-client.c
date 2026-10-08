/* SPDX-License-Identifier: MIT
 * A small client for stock's fingerprint HAL (android.hardware.biometrics.
 * fingerprint V3, AIDL) in the fingerprint lab, in the role Android's
 * FingerprintService and SystemUI play:
 *
 *   fp-client props                 the sensor's properties, as raw words
 *   fp-client secret                agree the auth-token key (KeyMint)
 *   fp-client list [USER]           enrolled fingers
 *   fp-client auth [SECONDS]        one authentication
 *   fp-client enroll [SECONDS]      one enrollment, with a token from
 *                                   gatekeeper for the lab's password
 *   fp-client remove ID [USER]      remove an enrolled finger
 *   fp-client serve SOCKET          the service for the shell (below)
 *
 * Touches come from the touchscreen (TOUCH, /dev/input/event1 by default),
 * in panel pixels like the HAL's sensor location. A touch on the sensor is
 * passed on (onPointerDownWithContext), the circle lit (LHBM, the
 * pixel_scanout.lhbm parameter at LHBM) and the HAL told it is lit
 * (onUiReady) once the panel has had a frame to show it (LHBM_SETTLE_MS),
 * as SystemUI does; lifting the finger reverses that.
 *
 * Plain C on libbinder_ndk: the parcels are written as the NDK backend
 * writes them (a parcelable is a non-null marker, its size, then its fields).
 * Transaction numbers are the methods' order in the frozen V3 API
 * (hash 637371b53fb7faf9bd43aa51b72c23852d6e6d96, as in the phone's
 * android.hardware.biometrics.fingerprint-V3-ndk.so).
 *
 * A Bionic executable without Android's build system: _start hands the
 * arguments to __libc_init() as Bionic's crtbegin does. Build:
 *   aarch64-linux-gnu-gcc -std=gnu11 -O2 -Wall -Wextra -fPIE -pie -nostdlib \
 *     -fno-stack-protector -U_FORTIFY_SOURCE -mno-outline-atomics \
 *     -Wl,--dynamic-linker=/system/bin/linker64 \
 *     -Wl,--hash-style=gnu -Wl,-z,now -Wl,--allow-shlib-undefined \
 *     -o fp-client fp-client.c -L LIBS -lbinder_ndk -lc
 * where LIBS holds the runtime's system/lib64 libc.so and libbinder_ndk.so.
 */
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* --- Bionic start-up --- */

typedef struct {
	void (**preinit_array)(void);
	void (**init_array)(void);
	void (**fini_array)(void);
	size_t preinit_array_count;
	size_t init_array_count;
	size_t fini_array_count;
} structors_array_t;

extern void __libc_init(void *raw_args, void (*onexit)(void),
			int (*slingshot)(int, char **, char **),
			structors_array_t const *const structors) __attribute__((noreturn));
int main(int argc, char **argv, char **envp);

/* The linker runs DT_INIT_ARRAY itself for a dynamic executable. */
static const structors_array_t structors;

__attribute__((used, noreturn)) static void start_main(void *raw_args)
{
	__libc_init(raw_args, NULL, main, &structors);
}

__asm__(".text\n"
	".global _start\n"
	".type _start, %function\n"
	"_start:\n"
	"	mov x0, sp\n"
	"	b start_main\n");

/* --- libbinder_ndk --- */

typedef struct AIBinder AIBinder;
typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel;
typedef struct AStatus AStatus;
typedef int32_t binder_status_t;
typedef uint32_t transaction_code_t;

AIBinder_Class *AIBinder_Class_define(const char *descriptor, void *(*on_create)(void *),
				      void (*on_destroy)(void *),
				      binder_status_t (*on_transact)(AIBinder *, transaction_code_t,
								     const AParcel *, AParcel *));
AIBinder *AIBinder_new(const AIBinder_Class *clazz, void *args);
bool AIBinder_associateClass(AIBinder *binder, const AIBinder_Class *clazz);
void AIBinder_markVintfStability(AIBinder *binder);
binder_status_t AIBinder_prepareTransaction(AIBinder *binder, AParcel **in);
binder_status_t AIBinder_transact(AIBinder *binder, transaction_code_t code, AParcel **in,
				  AParcel **out, uint32_t flags);
AIBinder *AServiceManager_waitForService(const char *instance);
void ABinderProcess_startThreadPool(void);
bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t threads);
void AParcel_delete(AParcel *parcel);
binder_status_t AParcel_readStatusHeader(const AParcel *parcel, AStatus **status);
binder_status_t AParcel_writeStatusHeader(AParcel *parcel, const AStatus *status);
AStatus *AStatus_newOk(void);
bool AStatus_isOk(const AStatus *status);
const char *AStatus_getDescription(const AStatus *status);
void AStatus_deleteDescription(const char *description);
void AStatus_delete(AStatus *status);
int32_t AStatus_getServiceSpecificError(const AStatus *status);
int32_t AParcel_getDataPosition(const AParcel *parcel);
binder_status_t AParcel_setDataPosition(const AParcel *parcel, int32_t position);
int32_t AParcel_getDataSize(const AParcel *parcel);
binder_status_t AParcel_writeInt32(AParcel *parcel, int32_t value);
binder_status_t AParcel_writeInt64(AParcel *parcel, int64_t value);
binder_status_t AParcel_writeFloat(AParcel *parcel, float value);
binder_status_t AParcel_writeBool(AParcel *parcel, bool value);
binder_status_t AParcel_writeString(AParcel *parcel, const char *string, int32_t length);
binder_status_t AParcel_writeByteArray(AParcel *parcel, const int8_t *array, int32_t length);
binder_status_t AParcel_writeInt32Array(AParcel *parcel, const int32_t *array, int32_t length);
binder_status_t AParcel_writeStrongBinder(AParcel *parcel, AIBinder *binder);
binder_status_t AParcel_readInt32(const AParcel *parcel, int32_t *value);
binder_status_t AParcel_readInt64(const AParcel *parcel, int64_t *value);
binder_status_t AParcel_readStrongBinder(const AParcel *parcel, AIBinder **binder);
binder_status_t AParcel_readByteArray(const AParcel *parcel, void *data,
				      bool (*allocator)(void *data, int32_t length, int8_t **buffer));

#define STATUS_OK			0
#define STATUS_UNKNOWN_TRANSACTION	(-74)	/* -EBADMSG */
#define STATUS_DEAD_OBJECT		(-32)	/* -EPIPE */
#define GET_INTERFACE_VERSION		0x00ffffffu
#define GET_INTERFACE_HASH		0x00fffffeu

#define AIDL_VERSION	3
#define AIDL_HASH	"637371b53fb7faf9bd43aa51b72c23852d6e6d96"
#define INSTANCE	"android.hardware.biometrics.fingerprint.IFingerprint/default"
#define SHARED_SECRET	"android.hardware.security.sharedsecret.ISharedSecret"
#define GATEKEEPER	"android.hardware.gatekeeper.IGatekeeper"

enum {	/* IFingerprint */
	FP_GET_SENSOR_PROPS = 1, FP_CREATE_SESSION,
};
enum {	/* ISession */
	S_GENERATE_CHALLENGE = 1, S_REVOKE_CHALLENGE, S_ENROLL, S_AUTHENTICATE,
	S_DETECT_INTERACTION, S_ENUMERATE_ENROLLMENTS, S_REMOVE_ENROLLMENTS,
	S_GET_AUTHENTICATOR_ID, S_INVALIDATE_AUTHENTICATOR_ID, S_RESET_LOCKOUT, S_CLOSE,
	S_ON_POINTER_DOWN, S_ON_POINTER_UP, S_ON_UI_READY, S_AUTHENTICATE_WITH_CONTEXT,
	S_ENROLL_WITH_CONTEXT, S_DETECT_INTERACTION_WITH_CONTEXT,
	S_ON_POINTER_DOWN_WITH_CONTEXT, S_ON_POINTER_UP_WITH_CONTEXT,
};
enum {	/* ISessionCallback */
	CB_CHALLENGE_GENERATED = 1, CB_CHALLENGE_REVOKED, CB_ACQUIRED, CB_ERROR,
	CB_ENROLLMENT_PROGRESS, CB_AUTHENTICATION_SUCCEEDED, CB_AUTHENTICATION_FAILED,
	CB_LOCKOUT_TIMED, CB_LOCKOUT_PERMANENT, CB_LOCKOUT_CLEARED, CB_INTERACTION_DETECTED,
	CB_ENROLLMENTS_ENUMERATED, CB_ENROLLMENTS_REMOVED, CB_AUTHENTICATOR_ID_RETRIEVED,
	CB_AUTHENTICATOR_ID_INVALIDATED, CB_SESSION_CLOSED,
};
#define CANCEL	1	/* ICancellationSignal.cancel, oneway */
#define FLAG_ONEWAY	0x01
enum {	/* ISharedSecret */
	SS_GET_PARAMETERS = 1, SS_COMPUTE,
};
enum {	/* IGatekeeper */
	GK_DELETE_ALL_USERS = 1, GK_DELETE_USER, GK_ENROLL, GK_VERIFY,
};

static const char *const acquired_names[] = {
	"UNKNOWN", "GOOD", "PARTIAL", "INSUFFICIENT", "SENSOR_DIRTY", "TOO_SLOW",
	"TOO_FAST", "VENDOR", "START", "TOO_DARK", "TOO_BRIGHT", "IMMOBILE",
	"RETRYING_CAPTURE", "LIFT_TOO_SOON", "POWER_PRESS",
};
static const char *const error_names[] = {
	"UNKNOWN", "HW_UNAVAILABLE", "UNABLE_TO_PROCESS", "TIMEOUT", "NO_SPACE",
	"CANCELED", "UNABLE_TO_REMOVE", "VENDOR", "BAD_CALIBRATION", "POWER_PRESS",
};

static const char *name(const char *const *names, size_t count, int32_t value)
{
	return value >= 0 && (size_t)value < count ? names[value] : "?";
}

#define NAME(names, value) name(names, sizeof(names) / sizeof(*names), value)

static void say(const char *format, ...)
{
	char line[512];
	struct timespec now;
	va_list args;
	int n;

	clock_gettime(CLOCK_MONOTONIC, &now);
	n = snprintf(line, sizeof(line), "%5ld.%03ld ", (long)now.tv_sec % 100000,
		     now.tv_nsec / 1000000);
	va_start(args, format);
	n += vsnprintf(line + n, sizeof(line) - n, format, args);
	va_end(args);
	if (n >= (int)sizeof(line) - 1)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	write(1, line, n);
}

/* --- Events from the HAL's callback thread to the main loop --- */

struct event {
	int32_t code;		/* ISessionCallback transaction */
	int32_t a, b;
	int64_t c;
};

static int events[2];

static void post(int32_t code, int32_t a, int32_t b, int64_t c)
{
	struct event e = { code, a, b, c };

	write(events[1], &e, sizeof(e));
}

struct bytes {
	int32_t length;		/* -1: null */
	int8_t data[256];
};

static bool bytes_allocator(void *data, int32_t length, int8_t **buffer)
{
	struct bytes *b = data;

	if (length > (int32_t)sizeof(b->data))
		return false;
	b->length = length;
	*buffer = length > 0 ? b->data : NULL;
	return true;
}

static bool read_bytes(const AParcel *in, struct bytes *b)
{
	b->length = -1;
	return AParcel_readByteArray(in, b, bytes_allocator) == 0;
}

/* android.hardware.keymaster/keymint HardwareAuthToken (the same parcel) */
struct hat {
	int64_t challenge, user, authenticator, timestamp;
	int32_t type;
	struct bytes mac;
};

/* A parcelable: its non-null marker and size; returns where it ends. */
static int32_t parcelable_begin(const AParcel *in)
{
	int32_t marker = 0, size = 0, start;

	AParcel_readInt32(in, &marker);
	if (!marker)
		return -1;
	start = AParcel_getDataPosition(in);
	AParcel_readInt32(in, &size);
	return start + size;
}

static bool read_hat(const AParcel *in, struct hat *hat, const char *what)
{
	int32_t end, ts_end;

	memset(hat, 0, sizeof(*hat));
	end = parcelable_begin(in);
	if (end < 0) {
		say("%s: no auth token", what);
		return false;
	}
	AParcel_readInt64(in, &hat->challenge);
	AParcel_readInt64(in, &hat->user);
	AParcel_readInt64(in, &hat->authenticator);
	AParcel_readInt32(in, &hat->type);
	ts_end = parcelable_begin(in);
	AParcel_readInt64(in, &hat->timestamp);
	if (ts_end >= 0)
		AParcel_setDataPosition(in, ts_end);
	read_bytes(in, &hat->mac);
	say("%s: auth token: challenge %lld user %#llx authenticator %#llx type %d time %lld ms, %d-byte MAC",
	    what, (long long)hat->challenge, (unsigned long long)hat->user,
	    (unsigned long long)hat->authenticator, hat->type, (long long)hat->timestamp,
	    hat->mac.length);
	AParcel_setDataPosition(in, end);
	return true;
}

/* The fingers last enumerated (the callback thread writes them before
 * posting the event the main loop reads). */
static int32_t fingers[32], finger_count;

static void read_ids(const AParcel *in, int32_t code, const char *what)
{
	char list[256] = "";
	int32_t count = -1, id;
	size_t n = 0;

	AParcel_readInt32(in, &count);
	for (int32_t i = 0; i < count && i < 32; i++) {
		AParcel_readInt32(in, &id);
		if (code == CB_ENROLLMENTS_ENUMERATED)
			fingers[i] = id;
		if (n < sizeof(list))
			n += snprintf(list + n, sizeof(list) - n, " %d", id);
	}
	if (code == CB_ENROLLMENTS_ENUMERATED)
		finger_count = count < 0 ? 0 : count > 32 ? 32 : count;
	say("%s: %d%s%s", what, count, count > 0 ? ":" : "", list);
	post(code, count, 0, 0);
}

static void *no_data(void *args)
{
	return args;
}

static void no_destroy(void *data)
{
	(void)data;
}

static binder_status_t not_local(AIBinder *binder, transaction_code_t code, const AParcel *in,
				 AParcel *out)
{
	(void)binder, (void)code, (void)in, (void)out;
	return STATUS_UNKNOWN_TRANSACTION;
}

static binder_status_t callback(AIBinder *binder, transaction_code_t code, const AParcel *in,
				AParcel *out)
{
	int32_t a = 0, b = 0;
	int64_t c = 0;
	AStatus *ok;

	(void)binder;
	ok = AStatus_newOk();
	AParcel_writeStatusHeader(out, ok);
	AStatus_delete(ok);
	switch (code) {
	case GET_INTERFACE_VERSION:
		AParcel_writeInt32(out, AIDL_VERSION);
		return STATUS_OK;
	case GET_INTERFACE_HASH:
		AParcel_writeString(out, AIDL_HASH, sizeof(AIDL_HASH) - 1);
		return STATUS_OK;
	case CB_CHALLENGE_GENERATED:
	case CB_CHALLENGE_REVOKED:
		AParcel_readInt64(in, &c);
		say("challenge %s: %lld", code == CB_CHALLENGE_GENERATED ? "generated" : "revoked",
		    (long long)c);
		break;
	case CB_ACQUIRED:
		AParcel_readInt32(in, &a);
		AParcel_readInt32(in, &b);
		say("acquired: %s (%d), vendor %d", NAME(acquired_names, a), a, b);
		break;
	case CB_ERROR:
		AParcel_readInt32(in, &a);
		AParcel_readInt32(in, &b);
		say("error: %s (%d), vendor %d", NAME(error_names, a), a, b);
		break;
	case CB_ENROLLMENT_PROGRESS:
		AParcel_readInt32(in, &a);
		AParcel_readInt32(in, &b);
		say("enrollment %d: %d to go", a, b);
		break;
	case CB_AUTHENTICATION_SUCCEEDED:
	{
		struct hat hat;

		AParcel_readInt32(in, &a);
		say("authenticated: finger %d", a);
		read_hat(in, &hat, "authenticated");
		break;
	}
	case CB_AUTHENTICATION_FAILED:
		say("authentication failed (no match)");
		break;
	case CB_LOCKOUT_TIMED:
		AParcel_readInt64(in, &c);
		say("locked out for %lld ms", (long long)c);
		break;
	case CB_LOCKOUT_PERMANENT:
		say("locked out until a strong authentication");
		break;
	case CB_LOCKOUT_CLEARED:
		say("lockout cleared");
		break;
	case CB_INTERACTION_DETECTED:
		say("interaction detected");
		break;
	case CB_ENROLLMENTS_ENUMERATED:
		read_ids(in, code, "enrolled");
		return STATUS_OK;
	case CB_ENROLLMENTS_REMOVED:
		read_ids(in, code, "removed");
		return STATUS_OK;
	case CB_AUTHENTICATOR_ID_RETRIEVED:
	case CB_AUTHENTICATOR_ID_INVALIDATED:
		AParcel_readInt64(in, &c);
		say("authenticator ID: %#llx", (unsigned long long)c);
		break;
	case CB_SESSION_CLOSED:
		say("session closed");
		break;
	default:
		say("unknown callback %u", code);
		return STATUS_OK;
	}
	post(code, a, b, c);
	return STATUS_OK;
}

/* --- Calls to the HAL --- */

static AIBinder_Class *fingerprint_class, *session_class, *cancel_class, *callback_class;
static AIBinder_Class *shared_secret_class, *gatekeeper_class;
static AIBinder *session, *cancel_signal;

static AParcel *begin(AIBinder *binder)
{
	AParcel *in = NULL;

	if (AIBinder_prepareTransaction(binder, &in) != STATUS_OK) {
		say("prepareTransaction failed");
		exit(1);
	}
	return in;
}

/* The service-specific error of the last failed call (0: none). */
static int32_t last_service_error;

/* Sends the call; returns the reply after its status, or NULL on failure. A
 * dead service ends the program: the service script starts the stack again. */
static AParcel *call(AIBinder *binder, transaction_code_t code, AParcel *in, const char *what)
{
	AParcel *out = NULL;
	AStatus *status = NULL;
	binder_status_t result;

	last_service_error = 0;
	result = AIBinder_transact(binder, code, &in, &out, 0);
	if (result == STATUS_DEAD_OBJECT) {
		say("%s: the service died", what);
		exit(3);
	}
	if (result != STATUS_OK) {
		say("%s: transaction failed (%d)", what, result);
		return NULL;
	}
	result = AParcel_readStatusHeader(out, &status);
	if (result != STATUS_OK || !AStatus_isOk(status)) {
		const char *text = status ? AStatus_getDescription(status) : NULL;

		say("%s: %s (%d)", what, text ? text : "bad reply", result);
		if (text)
			AStatus_deleteDescription(text);
		if (status) {
			last_service_error = AStatus_getServiceSpecificError(status);
			AStatus_delete(status);
		}
		AParcel_delete(out);
		return NULL;
	}
	AStatus_delete(status);
	return out;
}

static void call_oneway(AIBinder *binder, transaction_code_t code, AParcel *in, const char *what)
{
	AParcel *out = NULL;
	binder_status_t result = AIBinder_transact(binder, code, &in, &out, FLAG_ONEWAY);

	if (result != STATUS_OK)
		say("%s: transaction failed (%d)", what, result);
	if (out)
		AParcel_delete(out);
}

static bool call_void(AIBinder *binder, transaction_code_t code, AParcel *in, const char *what)
{
	AParcel *out = call(binder, code, in, what);

	if (out)
		AParcel_delete(out);
	return out != NULL;
}

/* A parcelable's non-null marker and a placeholder for its size. */
static int32_t parcelable_open(AParcel *in)
{
	int32_t start;

	AParcel_writeInt32(in, 1);
	start = AParcel_getDataPosition(in);
	AParcel_writeInt32(in, 0);
	return start;
}

static void parcelable_close(AParcel *in, int32_t start)
{
	int32_t end = AParcel_getDataPosition(in);

	AParcel_setDataPosition(in, start);
	AParcel_writeInt32(in, end - start);
	AParcel_setDataPosition(in, end);
}

static void write_pointer(AParcel *in, float x, float y, int64_t time)
{
	int32_t start = parcelable_open(in);

	AParcel_writeInt32(in, 0);		/* pointerId */
	AParcel_writeFloat(in, x);
	AParcel_writeFloat(in, y);
	AParcel_writeFloat(in, 60);		/* minor: the panel does not report it */
	AParcel_writeFloat(in, 60);		/* major */
	AParcel_writeFloat(in, 0);		/* orientation */
	AParcel_writeBool(in, false);		/* isAod */
	AParcel_writeInt64(in, time);
	AParcel_writeInt64(in, time);		/* gestureStart */
	parcelable_close(in, start);
}

static void write_hat(AParcel *in, const struct hat *hat)
{
	int32_t start = parcelable_open(in), timestamp;

	AParcel_writeInt64(in, hat->challenge);
	AParcel_writeInt64(in, hat->user);
	AParcel_writeInt64(in, hat->authenticator);
	AParcel_writeInt32(in, hat->type);
	timestamp = parcelable_open(in);
	AParcel_writeInt64(in, hat->timestamp);
	parcelable_close(in, timestamp);
	AParcel_writeByteArray(in, hat->mac.data, hat->mac.length);
	parcelable_close(in, start);
}

static AIBinder *find(const char *instance, const AIBinder_Class *clazz)
{
	AIBinder *binder = AServiceManager_waitForService(instance);

	if (!binder || !AIBinder_associateClass(binder, clazz)) {
		say("no %s", instance);
		exit(1);
	}
	return binder;
}

static AIBinder *service(void)
{
	return find(INSTANCE, fingerprint_class);
}

static void open_session(int32_t sensor, int32_t user)
{
	AIBinder *fingerprint = service(), *cb;
	AParcel *in, *out;

	cb = AIBinder_new(callback_class, NULL);
	AIBinder_markVintfStability(cb);
	in = begin(fingerprint);
	AParcel_writeInt32(in, sensor);
	AParcel_writeInt32(in, user);
	AParcel_writeStrongBinder(in, cb);
	out = call(fingerprint, FP_CREATE_SESSION, in, "createSession");
	if (!out || AParcel_readStrongBinder(out, &session) != STATUS_OK || !session ||
	    !AIBinder_associateClass(session, session_class)) {
		say("no session");
		exit(1);
	}
	AParcel_delete(out);
	say("session open: sensor %d, user %d", sensor, user);
}

static void take_cancel_signal(AParcel *out)
{
	if (AParcel_readStrongBinder(out, &cancel_signal) == STATUS_OK && cancel_signal)
		AIBinder_associateClass(cancel_signal, cancel_class);
	AParcel_delete(out);
}

/* --- Touches and the circle --- */

static const char *lhbm_path = "/sys/module/pixel_scanout/parameters/lhbm";
static int lhbm_settle_ms = 120;	/* a frame at the panel's 10 Hz idle */
static int touch_fd = -1;
static float sensor_x = 717, sensor_y = 2360, sensor_r = 121;

static bool lhbm(bool on)
{
	int fd = open(lhbm_path, O_WRONLY | O_CLOEXEC);
	bool ok = fd >= 0 && write(fd, on ? "1" : "0", 1) == 1;

	if (!ok)
		say("LHBM %s: cannot write %s (screen dark or in AOD?)", on ? "on" : "off", lhbm_path);
	if (fd >= 0)
		close(fd);
	return ok;
}

struct input_event_ {
	int64_t sec, usec;
	uint16_t type, code;
	int32_t value;
};

static int32_t touch_x = -1, touch_y = -1, touch_slot, touch_down;
static bool finger;	/* the HAL has been told a finger is down */

static int64_t now_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void pointer(bool down)
{
	AParcel *in = begin(session);
	int64_t time = now_ms();

	write_pointer(in, touch_x, touch_y, time);
	if (down) {
		say("finger down at (%d, %d)", touch_x, touch_y);
		call_void(session, S_ON_POINTER_DOWN_WITH_CONTEXT, in, "onPointerDown");
		/* The panel takes the circle at its next frame. */
		if (lhbm(true))
			usleep(lhbm_settle_ms * 1000);
		call_void(session, S_ON_UI_READY, begin(session), "onUiReady");
	} else {
		say("finger up");
		call_void(session, S_ON_POINTER_UP_WITH_CONTEXT, in, "onPointerUp");
		lhbm(false);
	}
	finger = down;
}

/* Tracks slot 0 only: one finger. */
static void touch_events(void)
{
	struct input_event_ ev[32];
	ssize_t n = read(touch_fd, ev, sizeof(ev));

	for (ssize_t i = 0; i < n / (ssize_t)sizeof(*ev); i++) {
		if (ev[i].type == 3) {			/* EV_ABS */
			if (ev[i].code == 0x2f)
				touch_slot = ev[i].value;
			else if (touch_slot)
				continue;
			else if (ev[i].code == 0x35)
				touch_x = ev[i].value;
			else if (ev[i].code == 0x36)
				touch_y = ev[i].value;
			else if (ev[i].code == 0x39)
				touch_down = ev[i].value >= 0;
		} else if (ev[i].type == 0 && ev[i].code == 0) {	/* SYN_REPORT */
			float dx = touch_x - sensor_x, dy = touch_y - sensor_y;
			bool on_sensor = touch_down && dx * dx + dy * dy <= sensor_r * sensor_r * 1.5f;

			if (on_sensor != finger)
				pointer(on_sensor);
		}
	}
}

/* Waits for one of the listed callbacks, handling touches meanwhile. */
static int32_t wait_for(int timeout_ms, const int32_t *codes, size_t count, struct event *got)
{
	int64_t deadline = now_ms() + timeout_ms;

	for (;;) {
		struct pollfd fds[2] = { { events[0], POLLIN, 0 }, { touch_fd, POLLIN, 0 } };
		int64_t left = deadline - now_ms();
		struct event e;

		if (left <= 0)
			return 0;
		if (poll(fds, touch_fd >= 0 ? 2 : 1, left) <= 0)
			continue;
		if (fds[1].revents & POLLIN)
			touch_events();
		if (!(fds[0].revents & POLLIN) || read(events[0], &e, sizeof(e)) != sizeof(e))
			continue;
		for (size_t i = 0; i < count; i++)
			if (e.code == codes[i]) {
				if (got)
					*got = e;
				return e.code;
			}
	}
}

static void finish(void)
{
	static const int32_t closed[] = { CB_SESSION_CLOSED };

	if (finger)
		pointer(false);
	if (cancel_signal) {
		call_oneway(cancel_signal, CANCEL, begin(cancel_signal), "cancel");
		cancel_signal = NULL;
		usleep(200000);
	}
	if (session && call_void(session, S_CLOSE, begin(session), "close"))
		wait_for(2000, closed, 1, NULL);
}

/* --- The auth token for enrollment --- */

/* Keystore's job at boot: the trusted apps agree on the key that signs auth
 * tokens. Only Trusty's KeyMint takes part here (stock adds the Titan M2's
 * StrongBox); gatekeeper and the fingerprint app get the key from it, and
 * until then the fingerprint app refuses every token. */
static bool shared_secret(void)
{
	AIBinder *ss = find(SHARED_SECRET "/default", shared_secret_class);
	struct bytes seed, nonce, check;
	AParcel *in, *out;
	int32_t start;

	out = call(ss, SS_GET_PARAMETERS, begin(ss), "getSharedSecretParameters");
	if (!out)
		return false;
	if (parcelable_begin(out) < 0 || !read_bytes(out, &seed) || !read_bytes(out, &nonce)) {
		say("getSharedSecretParameters: bad reply");
		AParcel_delete(out);
		return false;
	}
	AParcel_delete(out);
	in = begin(ss);
	AParcel_writeInt32(in, 1);		/* one participant */
	start = parcelable_open(in);
	AParcel_writeByteArray(in, seed.data, seed.length);
	AParcel_writeByteArray(in, nonce.data, nonce.length);
	parcelable_close(in, start);
	out = call(ss, SS_COMPUTE, in, "computeSharedSecret");
	if (!out)
		return false;
	read_bytes(out, &check);
	AParcel_delete(out);
	say("shared secret computed (%d-byte seed, %d-byte nonce; %d-byte check)",
	    seed.length, nonce.length, check.length);
	return check.length > 0;
}

/* --- The lock credential: gatekeeper --- */

/* LockSettingsService's job. User 0's PIN or password is enrolled with
 * Trusty's gatekeeper, which keeps its failure count in secure storage and
 * throttles guesses; verifying it yields a signed auth token. Until the user
 * sets one, a fixed stand-in is enrolled: enrolling a finger needs a
 * gatekeeper token, and the stand-in protects nothing.
 *
 * CREDENTIAL holds "KIND LENGTH HANDLE" (kind none, pin or password; a PIN's
 * length for the keypad; the gatekeeper handle in hex), replaced atomically:
 * losing the handle would mean enrolling afresh, with a new secure user ID
 * that the enrolled fingers are not bound to. */
#define STATE_DIR	"/data/vendor/omarchy"
#define CREDENTIAL	STATE_DIR "/credential"
#define LAB_HANDLE	"/data/vendor/fplab/gatekeeper-handle"	/* before CREDENTIAL */
#define STAND_IN	"omarchy-fingerprint-lab"
#define GK_ERROR_RETRY_TIMEOUT	1

struct secret {
	int32_t length;
	char data[128];
};

static char credential_kind[16] = "none";
static int credential_length;
static struct bytes credential_handle = { -1, { 0 } };

static int hex_value(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* HEX into DATA (at most SIZE bytes); returns the length, or -1. */
static int from_hex(const char *hex, void *data, size_t size)
{
	size_t n = strlen(hex);
	uint8_t *out = data;

	if (n % 2 || n / 2 > size)
		return -1;
	for (size_t i = 0; i < n; i += 2) {
		int high = hex_value(hex[i]), low = hex_value(hex[i + 1]);

		if (high < 0 || low < 0)
			return -1;
		out[i / 2] = high << 4 | low;
	}
	return n / 2;
}

static void load_credential(void)
{
	char text[640] = "", *kind, *length, *hex;
	int fd = open(CREDENTIAL, O_RDONLY | O_CLOEXEC), n;

	n = fd >= 0 ? read(fd, text, sizeof(text) - 1) : -1;
	if (fd >= 0)
		close(fd);
	strcpy(credential_kind, "none");
	credential_length = 0;
	credential_handle.length = -1;
	kind = n > 0 ? strtok(text, " \n") : NULL;
	length = kind ? strtok(NULL, " \n") : NULL;
	hex = length ? strtok(NULL, " \n") : NULL;
	if (hex && (!strcmp(kind, "none") || !strcmp(kind, "pin") || !strcmp(kind, "password"))) {
		credential_handle.length = from_hex(hex, credential_handle.data,
						    sizeof(credential_handle.data));
		strcpy(credential_kind, kind);
		credential_length = atoi(length);
		return;
	}
	/* The lab's handle, for the stand-in, from before CREDENTIAL. */
	fd = open(LAB_HANDLE, O_RDONLY | O_CLOEXEC);
	n = fd >= 0 ? read(fd, credential_handle.data, sizeof(credential_handle.data)) : -1;
	if (fd >= 0)
		close(fd);
	credential_handle.length = n > 0 ? n : -1;
}

static bool save_credential(const char *kind, int length, const struct bytes *handle)
{
	char text[640];
	int n = snprintf(text, sizeof(text), "%s %d ", kind, length), fd, dir;
	bool ok;

	for (int i = 0; i < handle->length && n < (int)sizeof(text) - 3; i++)
		n += snprintf(text + n, sizeof(text) - n, "%02x", (uint8_t)handle->data[i]);
	text[n++] = '\n';
	fd = open(CREDENTIAL ".new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	ok = fd >= 0 && write(fd, text, n) == n && !fsync(fd);
	if (fd >= 0)
		ok = !close(fd) && ok;
	ok = ok && !rename(CREDENTIAL ".new", CREDENTIAL);
	dir = open(STATE_DIR, O_RDONLY | O_CLOEXEC);
	if (dir >= 0) {
		fsync(dir);
		close(dir);
	}
	if (ok)
		load_credential();
	return ok;
}

/* The secret gatekeeper holds: GIVEN, or the stand-in while none is set. */
static struct secret held_secret(const struct secret *given)
{
	struct secret secret = { 0, { 0 } };

	if (!strcmp(credential_kind, "none")) {
		secret.length = strlen(STAND_IN);
		memcpy(secret.data, STAND_IN, secret.length);
	} else if (given) {
		secret = *given;
	}
	return secret;
}

/* Enrolls DESIRED for user 0 in place of CURRENT, which gatekeeper checks
 * (it keeps the secure user ID, so enrolled fingers stay valid); with no
 * handle yet, a first enrollment. Saves KIND and LENGTH with the new handle.
 * Returns 0, -1 (wrong CURRENT or a failure) or -2 (gatekeeper throttling). */
static int gk_enroll(const struct secret *current, const struct secret *desired,
		     const char *kind, int length)
{
	AIBinder *gk = find(GATEKEEPER "/default", gatekeeper_class);
	bool first = credential_handle.length <= 0;
	struct bytes handle = { -1, { 0 } };
	int32_t status = -1, timeout = 0;
	int64_t sid = 0;
	AParcel *in = begin(gk), *out;

	AParcel_writeInt32(in, 0);					/* uid */
	AParcel_writeByteArray(in, credential_handle.data, first ? 0 : credential_handle.length);
	AParcel_writeByteArray(in, (const int8_t *)current->data, first ? 0 : current->length);
	AParcel_writeByteArray(in, (const int8_t *)desired->data, desired->length);
	out = call(gk, GK_ENROLL, in, "gatekeeper enroll");
	if (!out)
		return last_service_error == GK_ERROR_RETRY_TIMEOUT ? -2 : -1;
	if (parcelable_begin(out) >= 0) {
		AParcel_readInt32(out, &status);
		AParcel_readInt32(out, &timeout);
		AParcel_readInt64(out, &sid);
		read_bytes(out, &handle);
	}
	AParcel_delete(out);
	if (status != 0 || handle.length <= 0) {
		say("gatekeeper enroll: status %d, %d-byte handle", status, handle.length);
		return status == GK_ERROR_RETRY_TIMEOUT ? -2 : -1;
	}
	if (!save_credential(kind, length, &handle)) {
		say("gatekeeper enroll: cannot save %s", CREDENTIAL);
		return -1;
	}
	say("gatekeeper: %s enrolled for user 0, secure user ID %#llx", kind, (unsigned long long)sid);
	return 0;
}

/* Verifies SECRET (the held one) for CHALLENGE, filling HAT. Returns 0, -1
 * (wrong or a failure) or -2 (gatekeeper throttling). */
static int gk_verify(int64_t challenge, const struct secret *secret, struct hat *hat)
{
	AIBinder *gk = find(GATEKEEPER "/default", gatekeeper_class);
	int32_t status = -1, timeout = 0;
	AParcel *in, *out;
	bool ok;

	if (credential_handle.length <= 0) {
		struct secret stand_in = held_secret(NULL);

		if (strcmp(credential_kind, "none") ||
		    gk_enroll(&stand_in, &stand_in, "none", 0))
			return -1;
	}
	in = begin(gk);
	AParcel_writeInt32(in, 0);
	AParcel_writeInt64(in, challenge);
	AParcel_writeByteArray(in, credential_handle.data, credential_handle.length);
	AParcel_writeByteArray(in, (const int8_t *)secret->data, secret->length);
	out = call(gk, GK_VERIFY, in, "gatekeeper verify");
	if (!out)
		return last_service_error == GK_ERROR_RETRY_TIMEOUT ? -2 : -1;
	ok = parcelable_begin(out) >= 0;
	AParcel_readInt32(out, &status);
	AParcel_readInt32(out, &timeout);
	ok = ok && read_hat(out, hat, "gatekeeper") && hat->mac.length > 0;
	AParcel_delete(out);
	if (status != 0)
		say("gatekeeper verify: status %d (1: enroll again), timeout %d ms", status, timeout);
	return ok && (status == 0 || status == 1) ? 0 : -1;
}

/* --- Commands --- */

static int props(void)
{
	AIBinder *fingerprint = service();
	AParcel *out = call(fingerprint, FP_GET_SENSOR_PROPS, begin(fingerprint), "getSensorProps");
	char line[128];
	int32_t word, i = 0, n = 0;

	if (!out)
		return 1;
	say("getSensorProps reply, %d bytes after the status:",
	    AParcel_getDataSize(out) - AParcel_getDataPosition(out));
	while (AParcel_getDataPosition(out) < AParcel_getDataSize(out) &&
	       AParcel_readInt32(out, &word) == STATUS_OK) {
		n += snprintf(line + n, sizeof(line) - n, " %08x", (uint32_t)word);
		if (++i % 8 == 0) {
			say("%4d:%s", (i - 8) * 4, line);
			n = 0;
		}
	}
	if (n)
		say("%4d:%s", (i - i % 8) * 4, line);
	AParcel_delete(out);
	return 0;
}

static int list(int32_t user)
{
	static const int32_t done[] = { CB_ENROLLMENTS_ENUMERATED, CB_ERROR };

	open_session(0, user);
	call_void(session, S_ENUMERATE_ENROLLMENTS, begin(session), "enumerateEnrollments");
	if (!wait_for(15000, done, 2, NULL))
		say("no answer");
	call_void(session, S_GET_AUTHENTICATOR_ID, begin(session), "getAuthenticatorId");
	usleep(300000);
	finish();
	return 0;
}

static int authenticate(int seconds)
{
	static const int32_t done[] = {
		CB_AUTHENTICATION_SUCCEEDED, CB_ERROR, CB_LOCKOUT_TIMED, CB_LOCKOUT_PERMANENT,
	};
	AParcel *in, *out;
	struct event e = { 0 };

	open_session(0, 0);
	in = begin(session);
	AParcel_writeInt64(in, 0);		/* operationId: a keyguard unlock */
	out = call(session, S_AUTHENTICATE, in, "authenticate");
	if (!out)
		return 1;
	take_cancel_signal(out);
	say("authenticating: touch the sensor (%d s)", seconds);
	if (wait_for(seconds * 1000, done, 4, &e) == CB_AUTHENTICATION_SUCCEEDED)
		cancel_signal = NULL;	/* the operation has ended */
	else if (e.code == CB_ERROR)
		cancel_signal = NULL;
	finish();
	return e.code == CB_AUTHENTICATION_SUCCEEDED ? 0 : 1;
}

static int enroll(int seconds)
{
	static const int32_t challenge[] = { CB_CHALLENGE_GENERATED, CB_ERROR };
	static const int32_t progress[] = { CB_ENROLLMENT_PROGRESS, CB_ERROR };
	AParcel *in, *out;
	struct event e = { 0 };
	struct secret given;
	struct hat hat;
	int64_t deadline;

	open_session(0, 0);
	call_void(session, S_GENERATE_CHALLENGE, begin(session), "generateChallenge");
	if (wait_for(3000, challenge, 2, &e) != CB_CHALLENGE_GENERATED) {
		finish();
		return 1;
	}
	load_credential();
	given.length = snprintf(given.data, sizeof(given.data), "%s",
				getenv("GK_PASSWORD") ? getenv("GK_PASSWORD") : "");
	given = held_secret(&given);
	if (!shared_secret() || gk_verify(e.c, &given, &hat)) {
		finish();
		return 1;
	}
	in = begin(session);
	write_hat(in, &hat);
	out = call(session, S_ENROLL, in, "enroll");
	if (!out) {
		finish();
		return 1;
	}
	take_cancel_signal(out);
	say("enrolling: touch the sensor again and again (%d s)", seconds);
	deadline = now_ms() + seconds * 1000;
	while (now_ms() < deadline) {
		if (!wait_for(deadline - now_ms(), progress, 2, &e))
			break;
		if (e.code == CB_ERROR || e.b == 0) {
			cancel_signal = NULL;
			break;
		}
	}
	finish();
	return e.code == CB_ENROLLMENT_PROGRESS && e.b == 0 ? 0 : 1;
}

static int remove_finger(int32_t id, int32_t user)
{
	static const int32_t done[] = { CB_ENROLLMENTS_REMOVED, CB_ERROR };
	AParcel *in;

	open_session(0, user);
	in = begin(session);
	AParcel_writeInt32Array(in, &id, 1);
	call_void(session, S_REMOVE_ENROLLMENTS, in, "removeEnrollments");
	wait_for(5000, done, 2, NULL);
	finish();
	return 0;
}

/* --- The service --- */

/* fp-client serve SOCKET: fingerprint and credential service for the shell's
 * lock screen and Settings, on a Unix socket. One command per line in, JSON
 * events out (SECRET: the PIN's or password's bytes in hex):
 *   status                    the credential, the fingers, the sensor
 *   auth                      arm authentication
 *   enroll SECRET             enroll a finger
 *   cancel                    end the operation
 *   down X Y | up             a finger on the circle (panel pixels), or lifted
 *   remove ID                 remove a finger
 *   verify SECRET             check the PIN or password
 *   setcred KIND OLD NEW      set the credential (KIND pin or password; OLD
 *                             "-" while none is set)
 * One operation runs at a time; a new one takes over once the HAL has ended
 * the running one. An operation's events go to the client that started it,
 * status to every client. The shell owns the touches: it sends down and up
 * for touches on the circle, and the service lights it.
 *
 * "@hold" and "@release" on stdout ask the service script to keep CPUs 4 and
 * 6 online while the trusted app works: it pins itself to them, and they are
 * offline while the screen is dark. */
#define MAX_CLIENTS	8
#define PANEL_WIDTH	1440
#define PANEL_HEIGHT	3120

struct client {
	int fd;
	size_t used;
	char buffer[512];
};

enum operation { OP_NONE, OP_AUTH, OP_ENROLL };
static const char *const operation_names[] = { "none", "auth", "enroll" };

static struct client clients[MAX_CLIENTS];
static enum operation operation;
static struct client *owner;
static struct {
	enum operation operation;
	struct client *client;
	struct secret secret;
} pending;
static int enroll_step, enroll_done, enroll_last;
static bool secret_agreed;

/* As on Android, a finger unlocks only after the PIN or password has been
 * checked once since the service started (a restart) and within the last 72
 * hours; after the HAL's lockout (too many unrecognized fingers) it takes
 * the PIN or password too, which then clears the lockout. */
#define STRONG_AUTH_MS	(72LL * 3600 * 1000)
static int64_t strong_auth_at = -1;	/* CLOCK_BOOTTIME ms of the last verified PIN or password */
static int64_t lockout_until;		/* a timed lockout's end, CLOCK_BOOTTIME ms */
static bool lockout_permanent;

static int64_t boottime_ms(void)
{
	struct timespec now;

	clock_gettime(CLOCK_BOOTTIME, &now);
	return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static bool pin_required(void)
{
	return strcmp(credential_kind, "none") &&
	       (strong_auth_at < 0 || boottime_ms() - strong_auth_at > STRONG_AUTH_MS);
}

static const char *lockout_state(void)
{
	return lockout_permanent ? "permanent" : boottime_ms() < lockout_until ? "timed" : "none";
}

static void hold(bool on)
{
	static bool held;

	if (on != held) {
		held = on;
		write(1, on ? "@hold\n" : "@release\n", on ? 6 : 9);
	}
}

/* A JSON line to CLIENT, or to every client when NULL. */
static void reply(struct client *client, const char *format, ...)
{
	char line[512];
	va_list args;
	int n;

	va_start(args, format);
	n = vsnprintf(line, sizeof(line) - 1, format, args);
	va_end(args);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	for (int i = 0; i < MAX_CLIENTS; i++)
		if (clients[i].fd >= 0 && (!client || client == &clients[i]))
			send(clients[i].fd, line, n, MSG_NOSIGNAL | MSG_DONTWAIT);
}

static void send_status(struct client *client)
{
	char list[256] = "";
	size_t n = 0;

	for (int i = 0; i < finger_count && n < sizeof(list); i++)
		n += snprintf(list + n, sizeof(list) - n, "%s%d", i ? "," : "", fingers[i]);
	reply(client, "{\"event\":\"status\",\"credential\":\"%s\",\"length\":%d,\"fingers\":[%s],"
	      "\"sensor\":{\"x\":%d,\"y\":%d,\"r\":%d},\"panel\":[%d,%d],\"operation\":\"%s\","
	      "\"pinRequired\":%s,\"pinReason\":\"%s\",\"lockout\":\"%s\"}",
	      credential_kind, credential_length, list, (int)sensor_x, (int)sensor_y,
	      (int)sensor_r, PANEL_WIDTH, PANEL_HEIGHT, operation_names[operation],
	      pin_required() ? "true" : "false",
	      !pin_required() ? "none" : strong_auth_at < 0 ? "restart" : "timeout", lockout_state());
}

static void refresh_fingers(void)
{
	static const int32_t done[] = { CB_ENROLLMENTS_ENUMERATED, CB_ERROR };

	hold(true);
	call_void(session, S_ENUMERATE_ENROLLMENTS, begin(session), "enumerateEnrollments");
	wait_for(10000, done, 2, NULL);
	if (operation == OP_NONE)
		hold(false);
}

static void start(enum operation op, struct client *client, const struct secret *secret);

static void end_operation(void)
{
	if (operation == OP_NONE)
		return;
	if (finger)
		pointer(false);
	operation = OP_NONE;
	owner = NULL;
	cancel_signal = NULL;
	if (pending.operation != OP_NONE) {
		enum operation next = pending.operation;

		pending.operation = OP_NONE;
		start(next, pending.client, &pending.secret);
	} else {
		hold(false);
	}
}

static void cancel_operation(void)
{
	if (cancel_signal)
		call_oneway(cancel_signal, CANCEL, begin(cancel_signal), "cancel");
}

static void start(enum operation op, struct client *client, const struct secret *secret)
{
	static const int32_t challenge[] = { CB_CHALLENGE_GENERATED, CB_ERROR };
	AParcel *in, *out;
	struct event e = { 0 };
	struct secret held;
	struct hat hat;
	int result;

	if (operation != OP_NONE) {
		pending.operation = op;
		pending.client = client;
		if (secret)
			pending.secret = *secret;
		cancel_operation();
		return;
	}
	if (op == OP_AUTH && !finger_count) {
		reply(client, "{\"event\":\"error\",\"error\":\"NO_FINGERS\"}");
		return;
	}
	if (op == OP_AUTH && (pin_required() || lockout_permanent)) {
		reply(client, "{\"event\":\"error\",\"error\":\"%s\"}",
		      lockout_permanent ? "LOCKOUT" : "PIN_REQUIRED");
		return;
	}
	hold(true);
	if (op == OP_AUTH) {
		in = begin(session);
		AParcel_writeInt64(in, 0);		/* operationId: a keyguard unlock */
		out = call(session, S_AUTHENTICATE, in, "authenticate");
	} else {
		call_void(session, S_GENERATE_CHALLENGE, begin(session), "generateChallenge");
		if (wait_for(3000, challenge, 2, &e) != CB_CHALLENGE_GENERATED) {
			reply(client, "{\"event\":\"error\",\"error\":\"HW_UNAVAILABLE\"}");
			hold(false);
			return;
		}
		if (!secret_agreed)
			secret_agreed = shared_secret();
		held = held_secret(secret);
		result = gk_verify(e.c, &held, &hat);
		if (result) {
			reply(client, "{\"event\":\"error\",\"error\":\"%s\"}",
			      result == -2 ? "THROTTLED" : "WRONG_CREDENTIAL");
			hold(false);
			return;
		}
		in = begin(session);
		write_hat(in, &hat);
		out = call(session, S_ENROLL, in, "enroll");
		enroll_step = 2;	/* until two reports show the step */
		enroll_done = 0;
		enroll_last = -1;
	}
	if (!out) {
		reply(client, "{\"event\":\"error\",\"error\":\"HW_UNAVAILABLE\"}");
		hold(false);
		return;
	}
	take_cancel_signal(out);
	operation = op;
	owner = client;
	reply(client, "{\"event\":\"started\",\"operation\":\"%s\"}", operation_names[op]);
}

static void hal_event(const struct event *e)
{
	switch (e->code) {
	case CB_ACQUIRED:
		reply(owner, "{\"event\":\"acquired\",\"info\":\"%s\",\"vendor\":%d}",
		      NAME(acquired_names, e->a), e->b);
		break;
	case CB_ENROLLMENT_PROGRESS:
		if (enroll_last > e->b)
			enroll_step = enroll_last - e->b;
		enroll_last = e->b;
		enroll_done++;
		reply(owner, "{\"event\":\"progress\",\"finger\":%d,\"remaining\":%d,\"done\":%d,"
		      "\"left\":%d}", e->a, e->b, enroll_done, (e->b + enroll_step - 1) / enroll_step);
		if (e->b == 0) {
			reply(owner, "{\"event\":\"enrolled\",\"finger\":%d}", e->a);
			end_operation();
			if (operation == OP_NONE)
				refresh_fingers();
			send_status(NULL);
		}
		break;
	case CB_AUTHENTICATION_SUCCEEDED:
		reply(owner, "{\"event\":\"match\",\"finger\":%d}", e->a);
		end_operation();
		break;
	case CB_AUTHENTICATION_FAILED:
		reply(owner, "{\"event\":\"nomatch\"}");
		break;
	case CB_LOCKOUT_TIMED:		/* the HAL has ended the operation */
		lockout_until = boottime_ms() + e->c;
		reply(owner, "{\"event\":\"lockout\",\"ms\":%lld}", (long long)e->c);
		end_operation();
		send_status(NULL);
		break;
	case CB_LOCKOUT_PERMANENT:
		lockout_permanent = true;
		reply(owner, "{\"event\":\"lockout\",\"permanent\":true}");
		end_operation();
		send_status(NULL);
		break;
	case CB_LOCKOUT_CLEARED:
		lockout_until = 0;
		lockout_permanent = false;
		send_status(NULL);
		break;
	case CB_ERROR:
		reply(owner, "{\"event\":\"error\",\"error\":\"%s\",\"vendor\":%d}",
		      NAME(error_names, e->a), e->b);
		end_operation();
		break;
	case CB_ENROLLMENTS_REMOVED:
		if (operation == OP_NONE)
			refresh_fingers();
		send_status(NULL);
		break;
	}
}

static bool parse_secret(const char *hex, struct secret *secret)
{
	memset(secret, 0, sizeof(*secret));
	if (!strcmp(hex, "-"))
		return true;
	secret->length = from_hex(hex, secret->data, 64);
	return secret->length >= 0;
}

static void set_credential(struct client *client, const char *kind, const char *old_hex,
			   const char *new_hex)
{
	struct secret old, desired, current;
	bool valid;
	int result;

	valid = (!strcmp(kind, "pin") || !strcmp(kind, "password")) &&
		parse_secret(old_hex, &old) && parse_secret(new_hex, &desired) &&
		desired.length >= 4;
	for (int i = 0; valid && !strcmp(kind, "pin") && i < desired.length; i++)
		valid = desired.data[i] >= '0' && desired.data[i] <= '9';
	if (!valid) {
		reply(client, "{\"event\":\"credential\",\"ok\":false,\"error\":\"INVALID\"}");
		return;
	}
	current = held_secret(&old);
	result = gk_enroll(&current, &desired, kind, !strcmp(kind, "pin") ? desired.length : 0);
	if (result)
		reply(client, "{\"event\":\"credential\",\"ok\":false,\"error\":\"%s\"}",
		      result == -2 ? "THROTTLED" : "WRONG_CREDENTIAL");
	else
		reply(client, "{\"event\":\"credential\",\"ok\":true,\"kind\":\"%s\"}", kind);
	send_status(NULL);
}

/* The lockout ends with a token for the PIN or password, as Android's
 * LockSettingsService gets one: from gatekeeper, for a challenge of the
 * fingerprint session's own. */
static void reset_lockout(const struct secret *held)
{
	static const int32_t challenge[] = { CB_CHALLENGE_GENERATED, CB_ERROR };
	static const int32_t cleared[] = { CB_LOCKOUT_CLEARED, CB_ERROR };
	struct event e = { 0 };
	struct hat hat;
	int64_t value;
	AParcel *in;

	hold(true);
	call_void(session, S_GENERATE_CHALLENGE, begin(session), "generateChallenge");
	if (wait_for(3000, challenge, 2, &e) == CB_CHALLENGE_GENERATED) {
		value = e.c;
		if (!gk_verify(value, held, &hat)) {
			in = begin(session);
			write_hat(in, &hat);
			call_void(session, S_RESET_LOCKOUT, in, "resetLockout");
			if (wait_for(3000, cleared, 2, &e) == CB_LOCKOUT_CLEARED) {
				lockout_until = 0;
				lockout_permanent = false;
				say("lockout cleared by the PIN or password");
			}
		}
		in = begin(session);
		AParcel_writeInt64(in, value);
		call_void(session, S_REVOKE_CHALLENGE, in, "revokeChallenge");
	}
	if (operation == OP_NONE)
		hold(false);
}

static void verify(struct client *client, const char *hex)
{
	struct secret given, held;
	struct hat hat;
	int result;

	if (!strcmp(credential_kind, "none")) {
		reply(client, "{\"event\":\"verified\",\"ok\":true}");
		return;
	}
	if (!parse_secret(hex, &given)) {
		reply(client, "{\"event\":\"verified\",\"ok\":false,\"throttled\":false}");
		return;
	}
	held = held_secret(&given);
	result = gk_verify(0, &held, &hat);
	reply(client, "{\"event\":\"verified\",\"ok\":%s,\"throttled\":%s}",
	      result ? "false" : "true", result == -2 ? "true" : "false");
	if (result)
		return;
	strong_auth_at = boottime_ms();
	if ((lockout_permanent || boottime_ms() < lockout_until) && operation == OP_NONE)
		reset_lockout(&held);
	send_status(NULL);
}

static void remove_one(struct client *client, int32_t id)
{
	AParcel *in;

	if (operation != OP_NONE) {
		reply(client, "{\"event\":\"error\",\"error\":\"BUSY\"}");
		return;
	}
	hold(true);
	in = begin(session);
	AParcel_writeInt32Array(in, &id, 1);
	call_void(session, S_REMOVE_ENROLLMENTS, in, "removeEnrollments");
	/* CB_ENROLLMENTS_REMOVED refreshes the list and releases the CPUs. */
}

static void command(struct client *client, char *line)
{
	char *word[5] = { 0 };
	int count = 0, x, y;

	for (char *token = strtok(line, " \t\r"); token && count < 5; token = strtok(NULL, " \t\r"))
		word[count++] = token;
	if (!count)
		return;
	if (!strcmp(word[0], "status")) {
		send_status(client);
	} else if (!strcmp(word[0], "auth")) {
		start(OP_AUTH, client, NULL);
	} else if (!strcmp(word[0], "enroll") && count == 2) {
		struct secret secret;

		if (parse_secret(word[1], &secret))
			start(OP_ENROLL, client, &secret);
	} else if (!strcmp(word[0], "cancel")) {
		if (pending.client == client)
			pending.operation = OP_NONE;
		if (owner == client)
			cancel_operation();
	} else if (!strcmp(word[0], "down") && count == 3) {
		x = atoi(word[1]);
		y = atoi(word[2]);
		if (operation != OP_NONE && owner == client && !finger &&
		    x >= 0 && x < PANEL_WIDTH && y >= 0 && y < PANEL_HEIGHT) {
			touch_x = x;
			touch_y = y;
			pointer(true);
			reply(client, "{\"event\":\"lit\"}");
		}
	} else if (!strcmp(word[0], "up")) {
		if (finger && owner == client)
			pointer(false);
	} else if (!strcmp(word[0], "remove") && count == 2) {
		remove_one(client, atoi(word[1]));
	} else if (!strcmp(word[0], "verify") && count == 2) {
		verify(client, word[1]);
	} else if (!strcmp(word[0], "setcred") && count == 4) {
		set_credential(client, word[1], word[2], word[3]);
	} else {
		reply(client, "{\"event\":\"error\",\"error\":\"UNKNOWN_COMMAND\"}");
	}
}

static void drop(struct client *client)
{
	close(client->fd);
	client->fd = -1;
	if (pending.client == client)
		pending.operation = OP_NONE;
	if (owner == client) {
		owner = NULL;
		cancel_operation();
	}
}

static void read_client(struct client *client)
{
	ssize_t got = read(client->fd, client->buffer + client->used,
			   sizeof(client->buffer) - 1 - client->used);
	char *newline;

	if (got <= 0) {
		drop(client);
		return;
	}
	client->used += got;
	client->buffer[client->used] = 0;
	while (client->fd >= 0 && (newline = strchr(client->buffer, '\n'))) {
		*newline = 0;
		command(client, client->buffer);
		client->used -= newline + 1 - client->buffer;
		memmove(client->buffer, newline + 1, client->used + 1);
	}
	if (client->fd >= 0 && client->used >= sizeof(client->buffer) - 1)
		drop(client);	/* a line too long */
}

static int serve(const char *path)
{
	struct sockaddr_un address = { .sun_family = AF_UNIX };
	int listener;

	for (int i = 0; i < MAX_CLIENTS; i++)
		clients[i].fd = -1;
	if (strlen(path) >= sizeof(address.sun_path))
		return 2;
	strcpy(address.sun_path, path);
	listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	unlink(path);
	umask(077);
	if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) ||
	    listen(listener, 4)) {
		say("cannot listen on %s", path);
		return 1;
	}
	load_credential();
	hold(true);
	open_session(0, 0);
	secret_agreed = shared_secret();
	refresh_fingers();
	hold(false);
	say("serving on %s: credential %s, %d fingers", path, credential_kind, finger_count);
	for (;;) {
		struct pollfd fds[2 + MAX_CLIENTS];
		struct client *polled[2 + MAX_CLIENTS];
		struct event e;
		int n = 2, fd, i;

		fds[0] = (struct pollfd){ events[0], POLLIN, 0 };
		fds[1] = (struct pollfd){ listener, POLLIN, 0 };
		for (i = 0; i < MAX_CLIENTS; i++)
			if (clients[i].fd >= 0) {
				polled[n] = &clients[i];
				fds[n++] = (struct pollfd){ clients[i].fd, POLLIN, 0 };
			}
		if (poll(fds, n, -1) <= 0)
			continue;
		if ((fds[0].revents & POLLIN) && read(events[0], &e, sizeof(e)) == sizeof(e))
			hal_event(&e);
		if (fds[1].revents & POLLIN) {
			fd = accept(listener, NULL, NULL);
			if (fd >= 0)
				fcntl(fd, F_SETFD, FD_CLOEXEC);
			for (i = 0; fd >= 0 && i < MAX_CLIENTS && clients[i].fd >= 0; i++)
				;
			if (fd >= 0 && i == MAX_CLIENTS) {
				close(fd);
			} else if (fd >= 0) {
				clients[i].fd = fd;
				clients[i].used = 0;
				send_status(&clients[i]);
			}
		}
		for (int k = 2; k < n; k++)
			if (polled[k]->fd == fds[k].fd &&
			    (fds[k].revents & (POLLIN | POLLHUP | POLLERR)))
				read_client(polled[k]);
	}
}

int main(int argc, char **argv, char **envp)
{
	const char *command = argc > 1 ? argv[1] : "props";
	const char *touch = getenv("TOUCH"), *path = getenv("LHBM"), *settle = getenv("LHBM_SETTLE_MS");
	int lock;

	(void)envp;
	/* The HAL allows one session and aborts on a second (a CHECK in
	 * createSession), so one client at a time. */
	lock = open("/data/vendor/fingerprint/fp-client.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
		say("another fp-client is running");
		return 1;
	}
	if (pipe(events)) {
		say("pipe failed");
		return 1;
	}
	if (path)
		lhbm_path = path;
	if (settle)
		lhbm_settle_ms = atoi(settle);
	touch_fd = open(touch ? touch : "/dev/input/event1", O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	fingerprint_class = AIBinder_Class_define("android.hardware.biometrics.fingerprint.IFingerprint",
						  no_data, no_destroy, not_local);
	session_class = AIBinder_Class_define("android.hardware.biometrics.fingerprint.ISession",
					      no_data, no_destroy, not_local);
	cancel_class = AIBinder_Class_define("android.hardware.biometrics.common.ICancellationSignal",
					     no_data, no_destroy, not_local);
	callback_class = AIBinder_Class_define("android.hardware.biometrics.fingerprint.ISessionCallback",
					       no_data, no_destroy, callback);
	shared_secret_class = AIBinder_Class_define(SHARED_SECRET, no_data, no_destroy, not_local);
	gatekeeper_class = AIBinder_Class_define(GATEKEEPER, no_data, no_destroy, not_local);
	ABinderProcess_setThreadPoolMaxThreadCount(1);
	ABinderProcess_startThreadPool();

	if (!strcmp(command, "serve") && argc > 2)
		return serve(argv[2]);
	if (!strcmp(command, "props"))
		return props();
	if (!strcmp(command, "secret"))
		return shared_secret() ? 0 : 1;
	if (!strcmp(command, "list"))
		return list(argc > 2 ? atoi(argv[2]) : 0);
	if (touch_fd < 0 && (!strcmp(command, "auth") || !strcmp(command, "enroll"))) {
		say("no touchscreen at %s", touch ? touch : "/dev/input/event1");
		return 1;
	}
	if (!strcmp(command, "auth"))
		return authenticate(argc > 2 ? atoi(argv[2]) : 30);
	if (!strcmp(command, "enroll"))
		return enroll(argc > 2 ? atoi(argv[2]) : 120);
	if (!strcmp(command, "remove") && argc > 2)
		return remove_finger(atoi(argv[2]), argc > 3 ? atoi(argv[3]) : 0);
	say("usage: fp-client props | secret | list [USER] | auth [SECONDS] | enroll [SECONDS] | remove ID [USER] | serve SOCKET");
	return 2;
}
