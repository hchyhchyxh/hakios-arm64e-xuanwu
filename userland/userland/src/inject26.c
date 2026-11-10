/*
 * libredwood26.dylib: the touch screen and the buttons for iOS 26's backboardd.
 *
 * backboardd is the HID event server, and without Apple's multitouch driver it has no touch
 * screen. This library (inserted by the kernel, see kern.exec_inject and build.sh's INJECT) gives
 * it one: a "virtual" HID service that turns the samples of /dev/redwood-touch -- from the phone's
 * own touch controller, bsd/dev/hakios_dada_touch.c, or from rwtouch -- into the events Apple's
 * driver would have sent, and a consumer-control service for the buttons.
 *
 * Only public-ish IOKit HID calls are used (IOHIDVirtualServiceClient*, IOHIDEvent*), no address
 * inside the shared cache: nothing here depends on a build of iOS. (src/inject.c is the iOS 16.5
 * library, whose window server patches are 16.5's addresses; the input code is the same.)
 * Lines go to stderr and to the console, "redwood26: ...".
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach_time.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <notify.h>
#include <os/lock.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "redwood_input.h"

static void
say(const char *format, ...)
{
	char line[768];
	va_list ap;
	int n, console;

	/* (two bytes kept free for the "redwood26: " below: a line that filled the buffer was moved two bytes past
	 * its end, and the fortified memmove trapped -- PosterBoard died of it on every launch, run glass6) */
	va_start(ap, format);
	n = vsnprintf(line, sizeof(line) - 2, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 3) {
		n = sizeof(line) - 3;
	}
	if (n > 9 && strncmp(line, "redwood: ", 9) == 0) {
		memmove(line + 11, line + 9, (size_t)n - 9 + 1);
		memcpy(line, "redwood26: ", 11);
		n += 2;
		if (n > (int)sizeof(line) - 1) {
			n = sizeof(line) - 1;
		}
	}
	write(2, line, (size_t)n);
	if ((console = open("/dev/console", O_WRONLY | O_NOCTTY)) >= 0) {
		write(console, line, (size_t)n);
		close(console);
	}
}

static dispatch_semaphore_t input_services_made;

/* ---- backboardd: the touch screen and the buttons ---------------------------------------------- */

/* IOKit's HID event system, the part that has no public header on iOS (prototypes checked against 20F66) */
typedef struct __IOHIDEvent *IOHIDEventRef;
typedef struct __IOHIDEventSystemClient *IOHIDEventSystemClientRef;
typedef struct __IOHIDServiceClient *IOHIDServiceClientRef;

/* IOHIDVirtualServiceClientCallbacksV2 */
struct virtual_service_callbacks {
	uint64_t version;               /* 2 */
	void (*notification)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, CFDictionaryRef property);
	Boolean (*setProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key, CFTypeRef value);
	CFTypeRef (*copyProperty)(void *target, void *refcon, IOHIDServiceClientRef service, CFStringRef key);
	IOHIDEventRef (*copyEvent)(void *target, void *refcon, IOHIDServiceClientRef service, uint32_t type, IOHIDEventRef matching, uint32_t options);
	int (*setOutputEvent)(void *target, void *refcon, IOHIDServiceClientRef service, IOHIDEventRef event);
	CFTypeRef (*copyMatchingEvent)(void *target, void *refcon, IOHIDServiceClientRef service, CFDictionaryRef matching);
};

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreateWithType(CFAllocatorRef allocator, int type, CFDictionaryRef attributes);
extern void IOHIDEventSystemClientScheduleWithDispatchQueue(IOHIDEventSystemClientRef client, dispatch_queue_t queue);
extern IOHIDServiceClientRef IOHIDVirtualServiceClientCreateWithCallbacks(IOHIDEventSystemClientRef client, CFDictionaryRef attributes,
    const struct virtual_service_callbacks *callbacks, void *target, void *refcon);
extern Boolean IOHIDVirtualServiceClientRemove(IOHIDServiceClientRef service);
extern Boolean IOHIDVirtualServiceClientDispatchEvent(IOHIDServiceClientRef service, IOHIDEventRef event);
extern IOHIDEventRef IOHIDEventCreateDigitizerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t transducer, uint32_t index,
    uint32_t identity, uint32_t event_mask, uint32_t button_mask, double x, double y, double z, double tip_pressure,
    double barrel_pressure, double twist, Boolean range, Boolean touch, uint32_t options);
extern IOHIDEventRef IOHIDEventCreateDigitizerFingerEvent(CFAllocatorRef allocator, uint64_t time, uint32_t index, uint32_t identity,
    uint32_t event_mask, double x, double y, double z, double tip_pressure, double twist, Boolean range, Boolean touch,
    uint32_t options);
extern IOHIDEventRef IOHIDEventCreateKeyboardEvent(CFAllocatorRef allocator, uint64_t time, uint32_t usage_page, uint32_t usage,
    Boolean down, uint32_t options);
extern void IOHIDEventAppendEvent(IOHIDEventRef parent, IOHIDEventRef child, uint32_t options);
extern void IOHIDEventSetIntegerValue(IOHIDEventRef event, uint32_t field, CFIndex value);

#define HID_CLIENT_SIMPLE               4       /* a client that only sends */
#define HID_CLIENT_RATE_CONTROLLED      3       /* what IOHIDEventSystemClientCreate() makes */
#define HID_PAGE_DIGITIZER              0x0d
#define HID_USAGE_TOUCH_SCREEN          0x04
#define HID_PAGE_CONSUMER               0x0c
#define HID_USAGE_CONSUMER_CONTROL      0x01
#define HID_USAGE_POWER                 0x30    /* the lock button */
#define HID_USAGE_MENU                  0x40    /* the home button */
#define HID_USAGE_VOLUME_UP             0xe9
#define HID_USAGE_VOLUME_DOWN           0xea
#define DIGITIZER_HAND                  3       /* kIOHIDDigitizerTransducerTypeHand */
#define DIGITIZER_RANGE                 0x1     /* event mask: what has changed */
#define DIGITIZER_TOUCH                 0x2
#define DIGITIZER_POSITION              0x4
#define DIGITIZER_FROM_EDGE_TIP         0x800   /* not a change: this finger came in over an edge of the screen */
#define FIELD_IS_BUILT_IN               0x4
#define FIELD_DIGITIZER_DISPLAY_INTEGRATED 0xb0019

#define MAX_FINGERS     10

/*
 * A finger that lands this close to an edge of the screen (a fraction of the width or the height)
 * is said to have come in over that edge. Apple's touch controller tells that from the shape of
 * the contact and marks the finger's events; UIKit's screen edge recognizers (the swipe up that
 * leaves an application, the swipe back in a navigation stack, the pull down from the top) take
 * no touch that is not so marked, whatever its path. All we have is where the finger landed.
 */
#define EDGE_SIDE       (16.0 / 402)
#define EDGE_TOP        (16.0 / 874)
#define EDGE_BOTTOM     (34.0 / 874)    /* the strip below the applications' content, where the home bar is */

static struct {
	bool touching, edge;
	double x, y;
} fingers[MAX_FINGERS];

static CFNumberRef
number(int value)
{
	return CFNumberCreate(NULL, kCFNumberIntType, &value);
}

/* A service of ours (the callbacks' refcon): what it says of itself, and whether the event server has taken it. */
struct service {
	CFMutableDictionaryRef properties;
	volatile bool opened;
};

/* The properties of a service of ours; the event server asks for them one at a time. */
static CFTypeRef
service_copy_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key)
{
	static CFMutableSetRef asked;
	CFTypeRef value = CFDictionaryGetValue(((struct service *)refcon)->properties, key);

	if (asked == NULL) {
		asked = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (value == NULL && !CFSetContainsValue(asked, key) && CFSetGetCount(asked) < 40) {
		char name[96] = "?";

		CFSetAddValue(asked, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		say("redwood: input: the event server asks for property \"%s\", which we do not have\n", name);
	}
	return value ? CFRetain(value) : NULL;
}

/*
 * What the window server last asked of the touch screen ("TouchDetectionMode"), in the low
 * byte: 1 = report touches; TOUCH_MODE_WAKE while the display is off and a tap is to wake it
 * -- which Apple's multitouch controller then does on its own; TOUCH_MODE_OFF = nothing at
 * all. (Seen: 0xff and 0xfe during the boot, 0x1 with the display on, 0x1fe after the lock
 * button.)
 */
#define TOUCH_MODE_WAKE         0xfe
#define TOUCH_MODE_OFF          0xff
static volatile int touch_mode;

/* What the event server tells a service of ours: said once per name, to learn what there is. */
static Boolean
service_set_property(__unused void *target, void *refcon, __unused IOHIDServiceClientRef service, CFStringRef key,
    CFTypeRef value)
{
	static CFMutableSetRef told;
	char name[96] = "?", text[160] = "?";
	CFStringRef description;

	/* the last thing the event server does when it opens a service; see make_service() */
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("DeviceOpenedByEventSystem")) &&
	    CFGetTypeID(value) == CFBooleanGetTypeID() && CFBooleanGetValue(value)) {
		((struct service *)refcon)->opened = true;
	}
	if (key != NULL && value != NULL && CFEqual(key, CFSTR("TouchDetectionMode")) && CFGetTypeID(value) == CFNumberGetTypeID()) {
		int mode = 0;

		CFNumberGetValue(value, kCFNumberIntType, &mode);
		if (mode != touch_mode) {
			say("redwood: input: touch detection mode %#x\n", mode);
		}
		touch_mode = mode;
	}
	if (told == NULL) {
		told = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (key != NULL && !CFSetContainsValue(told, key) && CFSetGetCount(told) < 24) {
		CFSetAddValue(told, key);
		CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
		description = value ? CFCopyDescription(value) : NULL;
		if (description != NULL) {
			CFStringGetCString(description, text, sizeof(text), kCFStringEncodingUTF8);
			CFRelease(description);
		}
		say("redwood: input: the event server sets \"%s\" = %s\n", name, text);
	}
	return true;
}

static void
service_notification(__unused void *target, __unused void *refcon, __unused IOHIDServiceClientRef service,
    __unused uint32_t type, __unused CFDictionaryRef property)
{
}

static const struct virtual_service_callbacks service_callbacks = {
	.version = 2, .notification = service_notification, .setProperty = service_set_property,
	.copyProperty = service_copy_property,
};

/*
 * A HID service of ours inside the event server: built in, with one usage.
 *
 * The event server is this very program, and it is still starting. A client that reaches it at
 * the wrong moment of that (backboardd's "HID system open ... start": the connection is made
 * and then replaced, "IOHIDEventSystemServer died. Reestablishing connection.") is left with a
 * service the server cannot talk to and never opens -- and a touch screen that backboardd waits
 * for in vain ("still waiting for a main screen digitizer"). When that moment comes depends on
 * how fast the machine is and on how many processors it has, so no fixed delay avoids it:
 * the service is made again until the server has opened it.
 */
static IOHIDServiceClientRef
make_service(IOHIDEventSystemClientRef client, const char *product, int page, int usage, bool display)
{
	struct service *made = calloc(1, sizeof(*made));
	IOHIDServiceClientRef service = NULL;
	CFMutableDictionaryRef properties = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFMutableDictionaryRef pair = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFStringRef name = CFStringCreateWithCString(NULL, product, kCFStringEncodingUTF8);
	CFNumberRef page_number = number(page), usage_number = number(usage), zero = number(0);
	CFArrayRef pairs;

	CFDictionarySetValue(pair, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(pair, CFSTR("DeviceUsage"), usage_number);
	pairs = CFArrayCreate(NULL, (const void **)&pair, 1, &kCFTypeArrayCallBacks);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("PrimaryUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePage"), page_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsage"), usage_number);
	CFDictionarySetValue(properties, CFSTR("DeviceUsagePairs"), pairs);
	CFDictionarySetValue(properties, CFSTR("Built-In"), kCFBooleanTrue);
	CFDictionarySetValue(properties, CFSTR("Transport"), CFSTR("SPI"));
	CFDictionarySetValue(properties, CFSTR("Manufacturer"), CFSTR("redwood"));
	CFDictionarySetValue(properties, CFSTR("Product"), name);
	CFDictionarySetValue(properties, CFSTR("VendorID"), zero);
	CFDictionarySetValue(properties, CFSTR("ProductID"), zero);
	if (display) {
		CFDictionarySetValue(properties, CFSTR("DisplayIntegrated"), kCFBooleanTrue);
	}
	/* the dictionary stays: it is what service_copy_property() answers from */
	made->properties = properties;
	for (int attempt = 1; attempt <= 20; attempt++) {
		made->opened = false;
		service = IOHIDVirtualServiceClientCreateWithCallbacks(client, properties, &service_callbacks, NULL, made);
		if (service == NULL) {
			break;
		}
		for (int tenths = 0; tenths < 20 && !made->opened; tenths++) {
			usleep(100000);
		}
		if (made->opened) {
			break;
		}
		say("redwood: input: the event server has not opened \"%s\" (attempt %d): making it again\n", product, attempt);
		IOHIDVirtualServiceClientRemove(service);
		CFRelease(service);
		service = NULL;
	}
	return service;
}

static pthread_mutex_t touch_lock = PTHREAD_MUTEX_INITIALIZER;   /* the screen's fingers: the panel's and the script's */

/* One frame of the touch screen: the hand, and under it every finger that is down or has just lifted. */
static void
send_touch_locked(IOHIDServiceClientRef screen, const struct redwood_touch *t);

static void
send_touch(IOHIDServiceClientRef screen, const struct redwood_touch *t)
{
	pthread_mutex_lock(&touch_lock);
	send_touch_locked(screen, t);
	pthread_mutex_unlock(&touch_lock);
}

static void
send_touch_locked(IOHIDServiceClientRef screen, const struct redwood_touch *t)
{
	uint32_t change = t->type == REDWOOD_TOUCH_MOVE ? DIGITIZER_POSITION : DIGITIZER_RANGE | DIGITIZER_TOUCH;
	IOHIDEventRef hand, finger;
	bool any = false;
	unsigned f = t->finger;

	if (f >= MAX_FINGERS || (t->type != REDWOOD_TOUCH_DOWN && !fingers[f].touching)) {
		return;
	}
	fingers[f].touching = t->type != REDWOOD_TOUCH_UP;
	fingers[f].x = t->x / 65535.0;
	fingers[f].y = t->y / 65535.0;
	if (t->type == REDWOOD_TOUCH_DOWN) {
		fingers[f].edge = fingers[f].x < EDGE_SIDE || fingers[f].x > 1 - EDGE_SIDE || fingers[f].y < EDGE_TOP ||
		    fingers[f].y > 1 - EDGE_BOTTOM;
	}
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		any |= fingers[i].touching;
	}

	hand = IOHIDEventCreateDigitizerEvent(NULL, t->time, DIGITIZER_HAND, 0, 0, change & ~DIGITIZER_RANGE, 0, fingers[f].x,
	    fingers[f].y, 0, 0, 0, 0, any, any, 0);
	IOHIDEventSetIntegerValue(hand, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
	IOHIDEventSetIntegerValue(hand, FIELD_IS_BUILT_IN, 1);
	for (unsigned i = 0; i < MAX_FINGERS; i++) {
		if (!fingers[i].touching && i != f) {
			continue;
		}
		finger = IOHIDEventCreateDigitizerFingerEvent(NULL, t->time, i + 1, i + 2,
		    (i == f ? change : 0) | (fingers[i].edge ? DIGITIZER_FROM_EDGE_TIP : 0), fingers[i].x, fingers[i].y, 0,
		    fingers[i].touching ? 1.0 : 0.0, 0, fingers[i].touching, fingers[i].touching, 0);
		IOHIDEventSetIntegerValue(finger, FIELD_DIGITIZER_DISPLAY_INTEGRATED, 1);
		IOHIDEventAppendEvent(hand, finger, 0);
		CFRelease(finger);
	}
	IOHIDVirtualServiceClientDispatchEvent(screen, hand);
	CFRelease(hand);
}

/*
 * Whether the side button is held right now, for our code in SpringBoard (inject_sb.c,
 * lock_long_press()): kept as the state of a Darwin notification, 2 = held, 1 = released
 * (0, what a name nobody has set says, = not known), and set before the event is sent.
 */
static void
publish_lock_button(bool down)
{
	static int token = NOTIFY_TOKEN_INVALID;

	if (token == NOTIFY_TOKEN_INVALID && notify_register_check("com.redwood.button.lock", &token) != NOTIFY_STATUS_OK) {
		token = NOTIFY_TOKEN_INVALID;
		return;
	}
	notify_set_state(token, down ? 2 : 1);
}


static int
boot_arg_on(const char *word)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *at;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0)
		return 0;
	at = strstr(args, word);
	return at != NULL && (at == args || at[-1] == ' ');
}

static void
send_button(IOHIDServiceClientRef buttons, const struct redwood_touch *t)
{
	if (t->finger == REDWOOD_BUTTON_LOCK && boot_arg_on("displaykeep=1")) {
		static int said;

		if (!said) {
			said = 1;
			say("redwood26: displaykeep=1: the lock button is dropped (no backlight/wake path yet)\n");
		}
		return;
	}
	uint32_t usage = t->finger == REDWOOD_BUTTON_LOCK ? HID_USAGE_POWER : t->finger == REDWOOD_BUTTON_HOME ? HID_USAGE_MENU :
	    t->finger == REDWOOD_BUTTON_VOLUME_UP ? HID_USAGE_VOLUME_UP : t->finger == REDWOOD_BUTTON_VOLUME_DOWN ?
	    HID_USAGE_VOLUME_DOWN : 0;
	IOHIDEventRef event;

	if (usage == 0) {
		return;
	}
	if (t->finger == REDWOOD_BUTTON_LOCK) {
		publish_lock_button(t->type == REDWOOD_BUTTON_DOWN);
	}
	event = IOHIDEventCreateKeyboardEvent(NULL, t->time, HID_PAGE_CONSUMER, usage, t->type == REDWOOD_BUTTON_DOWN, 0);
	IOHIDEventSetIntegerValue(event, FIELD_IS_BUILT_IN, 1);
	IOHIDVirtualServiceClientDispatchEvent(buttons, event);
	CFRelease(event);
}

/*
 * Boot-arg touch_lat=1: a line for every touch sample, "tl: i TYPE posted US read US sent US" -- when
 * the kernel stamped it, when this thread had it, when the event system had the event -- in
 * microseconds of mach_absolute_time, the kernel's own time (tools/latency.py).
 */
static bool touch_latency;
static mach_timebase_info_data_t timebase;

static uint64_t
micros(uint64_t absolute)
{
	return absolute * timebase.numer / timebase.denom / 1000;
}

static void script_start(IOHIDServiceClientRef screen, IOHIDServiceClientRef buttons);

static void *
input_thread(__unused void *arg)
{
	static const int types[] = { HID_CLIENT_SIMPLE, HID_CLIENT_RATE_CONTROLLED };
	IOHIDEventSystemClientRef client = NULL;
	IOHIDServiceClientRef screen = NULL, buttons = NULL;
	struct redwood_touch t;
	unsigned count = 0, waking = 0;         /* waking: the fingers whose touch went into waking the display */
	int device;

	pthread_setname_np("redwood input");
	{
		char arguments[1024] = "";
		size_t size = sizeof(arguments) - 1;

		if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
			touch_latency = strstr(arguments, "touch_lat=1") != NULL;
		}
		mach_timebase_info(&timebase);
	}
	device = open(REDWOOD_TOUCH_DEVICE, O_RDONLY);
	if (device < 0) {
		say("redwood: input: %s: %s\n", REDWOOD_TOUCH_DEVICE, strerror(errno));
		return NULL;
	}
	sleep(3);                       /* let backboardd get as far as starting its event server */
	for (unsigned i = 0; screen == NULL && i < sizeof(types) / sizeof(types[0]); i++) {
		/* this waits until the server (this very program) answers */
		client = IOHIDEventSystemClientCreateWithType(NULL, types[i], NULL);
		if (client == NULL) {
			say("redwood: input: no HID client of type %d\n", types[i]);
			continue;
		}
		IOHIDEventSystemClientScheduleWithDispatchQueue(client, dispatch_queue_create("redwood.input", NULL));
		screen = make_service(client, "Redwood touch screen", HID_PAGE_DIGITIZER, HID_USAGE_TOUCH_SCREEN, true);
		if (screen == NULL) {
			say("redwood: input: a HID client of type %d may not create services\n", types[i]);
		}
	}
	if (screen != NULL) {
		buttons = make_service(client, "Redwood buttons", HID_PAGE_CONSUMER, HID_USAGE_CONSUMER_CONTROL, false);
	}
	say("redwood: input: touch screen service %p, buttons %p\n", (void *)screen, (void *)buttons);
	dispatch_semaphore_signal(input_services_made); /* see wait_for_als_arrival() */
	if (screen == NULL) {
		return NULL;
	}
	script_start(screen, buttons);

	while (read(device, &t, sizeof(t)) == (ssize_t)sizeof(t)) {
		uint64_t read_at = mach_absolute_time();

		if (t.type == REDWOOD_TOUCH_DOWN || t.type == REDWOOD_TOUCH_UP || count++ < 4) {
			say("redwood: input: %s finger %u at %u %u (%.0f%% %.0f%%)\n",
			    t.type == REDWOOD_TOUCH_DOWN ? "DOWN" : t.type == REDWOOD_TOUCH_UP ? "UP  " : "move",
			    t.finger, t.x, t.y, t.x * 100.0 / 65535, t.y * 100.0 / 65535);
		}
		if (t.type > REDWOOD_TOUCH_UP) {
			if (buttons != NULL) {
				send_button(buttons, &t);
			}
		} else if (t.type == REDWOOD_TOUCH_DOWN && (touch_mode & 0xff) == TOUCH_MODE_WAKE && buttons != NULL) {
			/*
			 * The display is off and a tap is to wake it. The lock button is what wakes it here:
			 * press it for the finger, and keep the rest of this touch to ourselves.
			 */
			struct redwood_touch press = { .time = t.time, .type = REDWOOD_BUTTON_DOWN, .finger = REDWOOD_BUTTON_LOCK };

			send_button(buttons, &press);
			press.type = REDWOOD_BUTTON_UP;
			send_button(buttons, &press);
			waking |= 1u << (t.finger & 31);
		} else if (waking & (1u << (t.finger & 31))) {
			if (t.type == REDWOOD_TOUCH_UP) {
				waking &= ~(1u << (t.finger & 31));
			}
		} else {
			send_touch(screen, &t);
			if (touch_latency) {
				say("tl: i %u posted %llu read %llu sent %llu\n", t.type, micros(t.time), micros(read_at),
				    micros(mach_absolute_time()));
			}
		}
	}
	say("redwood: input: %s: %s\n", REDWOOD_TOUCH_DEVICE, strerror(errno));
	return NULL;
}

/*
 * Boot-arg taps=1: a script of touches, /redwood/taps.txt (userland/taps.txt), played on the touch screen as if a
 * finger did them -- to walk through the system's screens in a run nobody is holding the phone for, and record them.
 * One command a line, positions in percent of the screen:
 *   wait SECONDS          tap X Y          swipe X1 Y1 X2 Y2 MS          home          say TEXT
 * The script's finger is finger 9, out of the panel's way.
 */
#define SCRIPT_FINGER   9

static IOHIDServiceClientRef script_screen, script_buttons;

static void
script_touch(uint32_t type, double x, double y)
{
	struct redwood_touch t = { .time = mach_absolute_time(), .type = type, .finger = SCRIPT_FINGER,
		                   .x = (uint32_t)(x * 65535 / 100), .y = (uint32_t)(y * 65535 / 100) };

	send_touch(script_screen, &t);
}

static void *
script_thread(__unused void *arg)
{
	FILE *f = fopen("/redwood/taps.txt", "r");
	char line[256];
	unsigned lineno = 0;

	pthread_setname_np("redwood taps");
	if (f == NULL) {
		say("redwood: taps: no /redwood/taps.txt\n");
		return NULL;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		double a, b, c, d, ms;

		lineno++;
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '#' || line[0] == 0) {
			continue;
		}
		if (sscanf(line, "wait %lf", &a) == 1) {
			usleep((useconds_t)(a * 1000000));
		} else if (sscanf(line, "tap %lf %lf", &a, &b) == 2) {
			say("redwood: taps: %u: tap %.0f%% %.0f%%\n", lineno, a, b);
			script_touch(REDWOOD_TOUCH_DOWN, a, b);
			usleep(80000);
			script_touch(REDWOOD_TOUCH_UP, a, b);
		} else if (sscanf(line, "swipe %lf %lf %lf %lf %lf", &a, &b, &c, &d, &ms) == 5) {
			unsigned steps = (unsigned)(ms / 8) + 1;

			say("redwood: taps: %u: swipe %.0f,%.0f -> %.0f,%.0f in %.0f ms\n", lineno, a, b, c, d, ms);
			script_touch(REDWOOD_TOUCH_DOWN, a, b);
			for (unsigned i = 1; i <= steps; i++) {
				usleep(8000);
				script_touch(REDWOOD_TOUCH_MOVE, a + (c - a) * i / steps, b + (d - b) * i / steps);
			}
			script_touch(REDWOOD_TOUCH_UP, c, d);
		} else if (strcmp(line, "home") == 0 && script_buttons != NULL) {
			struct redwood_touch press = { .time = mach_absolute_time(), .type = REDWOOD_BUTTON_DOWN,
				                       .finger = REDWOOD_BUTTON_HOME };

			say("redwood: taps: %u: home\n", lineno);
			send_button(script_buttons, &press);
			usleep(60000);
			press.type = REDWOOD_BUTTON_UP;
			press.time = mach_absolute_time();
			send_button(script_buttons, &press);
		} else if (strncmp(line, "say ", 4) == 0) {
			say("redwood: taps: %u: %s\n", lineno, line + 4);
		} else {
			say("redwood: taps: %u: not understood: %s\n", lineno, line);
		}
	}
	fclose(f);
	say("redwood: taps: the script is done\n");
	return NULL;
}

static void
script_start(IOHIDServiceClientRef screen, IOHIDServiceClientRef buttons)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	pthread_t thread;

	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) != 0 || strstr(arguments, "taps=1") == NULL) {
		return;
	}
	script_screen = screen;
	script_buttons = buttons;
	if (pthread_create(&thread, NULL, script_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}

static void
input_start(void)
{
	pthread_t thread;

	input_services_made = dispatch_semaphore_create(0);
	if (pthread_create(&thread, NULL, input_thread, NULL) == 0) {
		pthread_detach(thread);
	}
}


/* ---- big thread stacks, and CoreAnimation switches from the boot-args ------------------------------- */

/*
 * No GPU: CoreAnimation draws with its software renderer, which walks the layer tree recursively
 * (CA::OGL render_layers -> LayerNode::apply -> ImagingNode::render -> ...). A deep but finite tree --
 * the app switcher's blurred snapshots of every app, a Settings push transition -- overflows the small
 * stack of the render thread and the process aborts (what looks like a "log out": SpringBoard and
 * backboardd respawn). Every thread this process makes with a stack under BIG_STACK_MIN gets BIG_STACK
 * instead, by interposing pthread_create, so the recursion has room.
 */
#define BIG_STACK_MIN   (4u << 20)
#define BIG_STACK       (24u << 20)

static int
big_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
    void *(*start)(void *), void *arg)
{
	pthread_attr_t own;
	size_t stack = 0;

	if (attr != NULL) {
		pthread_attr_getstacksize(attr, &stack);
	}
	if (stack < BIG_STACK_MIN) {
		if (pthread_attr_init(&own) == 0) {
			if (attr != NULL) {
				own = *attr;
			}
			pthread_attr_setstacksize(&own, BIG_STACK);
			int r = pthread_create(thread, &own, start, arg);
			pthread_attr_destroy(&own);
			return r;
		}
	}
	return pthread_create(thread, attr, start, arg);
}

/*
 * What the window server hands the display, swap by swap (boot-arg swaplog=1). With the GPU's
 * renderer the panel stayed on the loader's picture although the kernel's display driver counted
 * 41 swaps in five seconds, none of them with a surface in it: which layer, which surface, and
 * which events the swap was told to wait for, from the window server's side of the call.
 */
typedef void *IOMobileFramebufferRef;
typedef struct { double x, y, w, h; } rw_rect;
extern int IOMobileFramebufferSwapBegin(IOMobileFramebufferRef fb, int *token);
extern int IOMobileFramebufferSwapEnd(IOMobileFramebufferRef fb);
extern int IOMobileFramebufferSwapSetLayer(IOMobileFramebufferRef fb, int layer, void *surface, rw_rect bounds,
    rw_rect frame, int flags);
/* (the two event calls' arguments are not known for certain: all six registers are passed on as they came) */
extern int IOMobileFramebufferSwapSetEventWait(IOMobileFramebufferRef fb, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5);
extern int IOMobileFramebufferSwapSetEventSignal(IOMobileFramebufferRef fb, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5);
extern uint32_t IOSurfaceGetID(void *surface);

static unsigned swaplog_count;

static bool
swaplog_on(void)
{
	static int on = -1;

	if (on < 0) {
		char args[1024] = "";
		size_t size = sizeof(args) - 1;

		on = sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "swaplog=1") != NULL;
	}
	/* the first 40 swaps, then one in 200 */
	return on && (swaplog_count < 40 || swaplog_count % 200 == 0);
}

static void
swaplog(const char *fmt, ...)
{
	char line[300];
	va_list ap;
	int fd;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		(void)write(fd, line, strlen(line));
		close(fd);
	}
}

static int
rw_swap_begin(IOMobileFramebufferRef fb, int *token)
{
	int r = IOMobileFramebufferSwapBegin(fb, token);

	if (swaplog_on()) {
		swaplog("redwood26: swaplog %u: begin -> %#x token %d\n", swaplog_count, r, token ? *token : -1);
	}
	return r;
}

static int
rw_swap_end(IOMobileFramebufferRef fb)
{
	int r = IOMobileFramebufferSwapEnd(fb);

	if (swaplog_on()) {
		swaplog("redwood26: swaplog %u: end -> %#x\n", swaplog_count, r);
	}
	swaplog_count++;
	return r;
}

static int
rw_swap_set_layer(IOMobileFramebufferRef fb, int layer, void *surface, rw_rect bounds, rw_rect frame, int flags)
{
	int r = IOMobileFramebufferSwapSetLayer(fb, layer, surface, bounds, frame, flags);

	if (swaplog_on()) {
		swaplog("redwood26: swaplog %u: layer %d surface %u (%p) bounds %.0f,%.0f %.0fx%.0f frame %.0f,%.0f %.0fx%.0f "
		    "flags %#x -> %#x\n", swaplog_count, layer, surface ? IOSurfaceGetID(surface) : 0, surface, bounds.x,
		    bounds.y, bounds.w, bounds.h, frame.x, frame.y, frame.w, frame.h, flags, r);
	}
	return r;
}

static int
rw_swap_event_wait(IOMobileFramebufferRef fb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	int r = IOMobileFramebufferSwapSetEventWait(fb, a1, a2, a3, a4, a5);

	if (swaplog_on()) {
		swaplog("redwood26: swaplog %u: event wait %#llx %#llx %#llx %#llx -> %#x\n", swaplog_count,
		    (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)a4, r);
	}
	return r;
}

static int
rw_swap_event_signal(IOMobileFramebufferRef fb, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
	int r = IOMobileFramebufferSwapSetEventSignal(fb, a1, a2, a3, a4, a5);

	if (swaplog_on()) {
		swaplog("redwood26: swaplog %u: event signal %#llx %#llx %#llx %#llx -> %#x\n", swaplog_count,
		    (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, (unsigned long long)a4, r);
	}
	return r;
}

__attribute__((used)) static struct {
	const void *replacement;
	const void *original;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)(uintptr_t)&big_pthread_create, (const void *)(uintptr_t)&pthread_create },
	{ (const void *)(uintptr_t)&rw_swap_begin, (const void *)(uintptr_t)&IOMobileFramebufferSwapBegin },
	{ (const void *)(uintptr_t)&rw_swap_end, (const void *)(uintptr_t)&IOMobileFramebufferSwapEnd },
	{ (const void *)(uintptr_t)&rw_swap_set_layer, (const void *)(uintptr_t)&IOMobileFramebufferSwapSetLayer },
	{ (const void *)(uintptr_t)&rw_swap_event_wait, (const void *)(uintptr_t)&IOMobileFramebufferSwapSetEventWait },
	{ (const void *)(uintptr_t)&rw_swap_event_signal, (const void *)(uintptr_t)&IOMobileFramebufferSwapSetEventSignal },
};

/* Boot-arg caenv=NAME=VALUE (as many as wanted): into this process's environment, for CoreAnimation's
 * own switches (CA_DISABLE_BACKDROP_CACHE=1, CA_DISABLE_FILTER_MERGING=1, ...). Set before CA starts. */
static int caenv_glass_allowed;   /* SDF/backdrop switches: window server and SpringBoard only */

static void
caenv_from_bootargs(void)
{
	char args[2048] = "";
	size_t size = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return;
	}
	for (const char *a = args; (a = strstr(a, "caenv=")) != NULL; a += 6) {
		char setting[160];
		size_t len = strcspn(a + 6, " ");
		char *eq;

		if ((a != args && a[-1] != ' ') || len == 0 || len >= sizeof(setting)) {
			continue;
		}
		memcpy(setting, a + 6, len);
		setting[len] = 0;
		if ((eq = strchr(setting, '=')) != NULL) {
			*eq = 0;
			if (!caenv_glass_allowed &&
			    (strncmp(setting, "CA_DISABLE_SDF", 14) == 0 ||
			     strncmp(setting, "CA_DISABLE_INLINE_SDFS", 22) == 0 ||
			     strncmp(setting, "CA_DISABLE_BACKDROP_CACHE", 25) == 0)) {
				continue;
			}
			setenv(setting, eq + 1, 1);
			say("redwood: caenv: %s=%s\n", setting, eq + 1);
		}
	}
}

/* ---- SpringBoard: the device is "activated", and the setup assistant is not needed ------------------ */

/*
 * SpringBoard decides whether to show the lock/home screen or the setup assistant from two things,
 * both of which depend on daemons this port does not run:
 *   - whether the device is "activated" (-[SBLockdownManager isActivated], normally the answer of
 *     mobileactivationd): forced to YES;
 *   - whether the setup assistant is required (-[SBSetupManager _setSetupRequiredReason:], normally
 *     driven by the purplebuddy state): forced to 0, "no reason".
 * Without these, unlocking launches Setup.app, whose first screen the software renderer cannot draw --
 * backboardd runs out of stack in CoreAnimation's render tree and SpringBoard respawns with it (the
 * "log out"). Swizzled by selector name only, so nothing here depends on a build of iOS. (src/inject_sb.c
 * does the same for iOS 16.5, by hard-coded address.)
 */
static BOOL
sb_is_activated(__unused id self, __unused SEL sel)
{
	return YES;
}

static void (*orig_set_setup_required_reason)(id, SEL, long);

static void
sb_set_setup_required_reason(id self, SEL sel, long reason)
{
	if (reason != 0) {
		say("redwood: SpringBoard wants the setup assistant (reason %ld); refused\n", reason);
	}
	if (orig_set_setup_required_reason != NULL) {
		orig_set_setup_required_reason(self, sel, 0);
	}
}

static int
swizzle(const char *class_name, const char *selector, IMP replacement, IMP *original)
{
	Class cls = objc_getClass(class_name);
	Method m = cls != NULL ? class_getInstanceMethod(cls, sel_registerName(selector)) : NULL;

	if (m == NULL) {
		say("redwood: -[%s %s]: not found\n", class_name, selector);
		return 0;
	}
	IMP prev = method_setImplementation(m, replacement);
	if (original != NULL) {
		*original = prev;
	}
	return 1;
}

/*
 * The wallpaper. iOS 26's wallpaper is a "poster" of PosterBoard's, and the first ones are made when the setup
 * assistant finishes -- which never runs here (refused above). PosterBoard itself will not make a default one on an
 * iPhone ("Hello NOT iPad, trying to restoreDefaultWallpaperForAllVariantsAndNotify? not today."), so the lock and
 * home screens stayed black. What Settings' "restore the default wallpaper" asks for is asked here, once SpringBoard
 * is up: +[SBSUIWallpaperService restoreDefaultWallpaper]. Boot-arg sb_wallpaper=0: not.
 */
static void
sb_restore_wallpaper(void *context)
{
	Class service = objc_getClass("SBSUIWallpaperService");
	SEL restore = sel_registerName("restoreDefaultWallpaper");

	(void)context;
	say("redwood: SpringBoard: the default wallpaper: asking (%s)\n", service != Nil ? "SBSUIWallpaperService" : "no service class");
	if (service != Nil && class_getClassMethod(service, restore) != NULL) {
		((void (*)(Class, SEL))objc_msgSend)(service, restore);
		say("redwood: SpringBoard: the default wallpaper asked for (SBSUIWallpaperService)\n");
	} else {
		say("redwood: SpringBoard: no +[SBSUIWallpaperService restoreDefaultWallpaper]\n");
	}
}

/*
 * PosterBoard is started by its clients' requests (its Mach service does not start it), and SpringBoard asks only
 * once it has a poster: so nothing starts it, and no poster is ever made (runs flip2, wp1). Asked here as a client
 * asks, through PosterBoardServices' PRSService: prewarm, which starts it and sets up its data store, the role
 * coordinators and their first posters. The reply's arguments are not looked at: what they are is not known, and
 * describing one took SpringBoard down (run wp4: a trap in snprintf inside the reply block).
 */
static void
sb_prewarm_done(void *context)
{
	(void)context;
	say("redwood: SpringBoard: PosterBoard: prewarm answered\n");
}

static void
sb_wake_posterboard(void *context)
{
	Class service_class = objc_getClass("PRSService");
	id service;

	(void)context;
	if (service_class == Nil) {
		say("redwood: SpringBoard: no PRSService\n");
		return;
	}
	service = ((id (*)(id, SEL))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(service_class, sel_registerName("alloc")),
	    sel_registerName("init"));
	say("redwood: SpringBoard: PosterBoard: prewarm asked\n");
	((void (*)(id, SEL, void (^)(void)))objc_msgSend)(service, sel_registerName("prewarmWithCompletion:"), ^{
		dispatch_async_f(dispatch_get_main_queue(), NULL, sb_prewarm_done);
	});
}

static void
sb_inspect_clock_layer(id layer, unsigned depth)
{
    if (layer == nil || depth > 7)
        return;
    id description = ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("description"));
    id contents = ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("contents"));
    id filters = ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("filters"));
    const char *text = ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String"));
    float opacity = ((float (*)(id, SEL))objc_msgSend)(layer, sel_registerName("opacity"));
    BOOL hidden = ((BOOL (*)(id, SEL))objc_msgSend)(layer, sel_registerName("isHidden"));
    say("redwood: clock render layer %u hidden %d opacity %.3f contents %p class %s filters %p %s\n",
        depth, hidden, opacity, (void *)contents, contents != nil ? class_getName(object_getClass(contents)) : "nil",
        (void *)filters, text != NULL ? text : "?");
    id children = ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("sublayers"));
    unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(children, sel_registerName("count"));
    for (unsigned long i = 0; i < count && i < 40; i++)
        sb_inspect_clock_layer(((id (*)(id, SEL, unsigned long))objc_msgSend)(children, sel_registerName("objectAtIndex:"), i), depth + 1);
}

static unsigned sb_clock_nodes;
static void
sb_inspect_clock_view(id view, unsigned depth, unsigned clockDepth)
{
    if (view == nil || depth > 40 || sb_clock_nodes++ >= 1800)
        return;
    const char *name = class_getName(object_getClass(view));
    if (name != NULL && strcmp(name, "CSProminentTimeView") == 0)
        clockDepth = 7;
    if (clockDepth != 0 || (name != NULL && (strstr(name, "Time") != NULL || strstr(name, "Clock") != NULL || strstr(name, "Date") != NULL))) {
        id description = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("description"));
        const char *text = ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String"));
        BOOL hidden = ((BOOL (*)(id, SEL))objc_msgSend)(view, sel_registerName("isHidden"));
        double alpha = ((double (*)(id, SEL))objc_msgSend)(view, sel_registerName("alpha"));
        say("redwood: clock view depth %u hidden %d alpha %.3f %s\n", depth, hidden, alpha, text != NULL ? text : "?");
        if (clockDepth != 0) {
            id layer = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("layer"));
            id layerText = ((id (*)(id, SEL))objc_msgSend)(layer, sel_registerName("description"));
            const char *details = ((const char *(*)(id, SEL))objc_msgSend)(layerText, sel_registerName("UTF8String"));
            say("redwood: clock layer %s\n", details != NULL ? details : "?");
            if (name != NULL && strstr(name, "TitleLabelView") != NULL)
                sb_inspect_clock_layer(layer, 0);

            SEL getText = sel_registerName("text");
            if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(view, sel_registerName("respondsToSelector:"), getText)) {
                id value = ((id (*)(id, SEL))objc_msgSend)(view, getText);
                id description = ((id (*)(id, SEL))objc_msgSend)(value, sel_registerName("description"));
                const char *string = ((const char *(*)(id, SEL))objc_msgSend)(description, sel_registerName("UTF8String"));
                say("redwood: clock text %s\n", string != NULL ? string : "nil");
            }
        }

    }
    id children = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
    unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(children, sel_registerName("count"));
    for (unsigned long i = 0; i < count; i++)
        sb_inspect_clock_view(((id (*)(id, SEL, unsigned long))objc_msgSend)(children, sel_registerName("objectAtIndex:"), i), depth + 1, clockDepth != 0 ? clockDepth - 1 : 0);
}

static void
sb_inspect_clock(void *context)
{
    (void)context;
    id application = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("UIApplication"), sel_registerName("sharedApplication"));
    id windows = ((id (*)(id, SEL))objc_msgSend)(application, sel_registerName("windows"));
    unsigned long count = ((unsigned long (*)(id, SEL))objc_msgSend)(windows, sel_registerName("count"));
    sb_clock_nodes = 0;
    say("redwood: clock hierarchy inspection %lu windows\n", count);
    for (unsigned long i = 0; i < count; i++)
        sb_inspect_clock_view(((id (*)(id, SEL, unsigned long))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i), 0, 0);
}


/* ---- the lock-screen clock's glass: make it plain when the renderer cannot draw CASDFLayer ------------- */

/*
 * The lock-screen clock of iOS 26 is not text: the poster's vibrant title material makes CoverSheetKit build
 * a SwiftUI.SDFLayer over a CABackdropLayer and a CASDFLayer.  Neither CoreAnimation's software renderer nor
 * the OpenGL ES renderer (eagl.c) draws those layers - the view hierarchy shows the whole subtree visible,
 * full opacity, with the SDF element's contents set, and nothing on the screen.  An ordinary title label does
 * draw, so boot-arg clockplain=1 replaces the title content style at the one place SpringBoard/CoverSheet
 * consumes it with an opaque colour style.
 */

/* ---- redwood_clock_overlay: a plain time label over the cover sheet (until Metal draws the real one) ---- */

static id clock_overlay_label;
static volatile int clock_overlay_running;

static void
clock_overlay_set_text(void)
{
	time_t now = time(NULL);
	struct tm tm;
	char text[16];
	id string;

	if (clock_overlay_label == nil || localtime_r(&now, &tm) == NULL)
		return;
	strftime(text, sizeof(text), "%H:%M", &tm);
	string = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
	    sel_registerName("stringWithUTF8String:"), text);
	if (string == nil)
		return;
	/* UIKit may only be touched on the main thread; the label is safe to message there */
	((void (*)(id, SEL, SEL, id, unsigned char))objc_msgSend)(clock_overlay_label,
	    sel_registerName("performSelectorOnMainThread:withObject:waitUntilDone:"),
	    sel_registerName("setText:"), string, 0);
}

static void *
clock_overlay_thread(void *arg)
{
	(void)arg;
	pthread_setname_np("redwood clock");
	while (clock_overlay_running) {
		clock_overlay_set_text();
		sleep(1);
	}
	return NULL;
}

static void
redwood_clock_overlay(id controller)
{
	Class uilabel, uifont, uicolor;
	id view, window, label;
	struct CGRect frame;
	double width, height = 170.0;
	pthread_t thread;

	if (clock_overlay_label != nil)
		return;
	uilabel = objc_getClass("UILabel");
	uifont = objc_getClass("UIFont");
	uicolor = objc_getClass("UIColor");
	view = ((id (*)(id, SEL))objc_msgSend)(controller, sel_registerName("view"));
	if (view == nil || uilabel == Nil || uifont == Nil || uicolor == Nil)
		return;
	window = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("window"));
	if (window == nil)
		window = view;
	width = ((struct CGRect (*)(id, SEL))objc_msgSend)(window, sel_registerName("bounds")).size.width;
	frame = (struct CGRect){ { 0.0, 90.0 }, { width, height } };
	label = ((id (*)(Class, SEL))objc_msgSend)(uilabel, sel_registerName("alloc"));
	label = ((id (*)(id, SEL, struct CGRect))objc_msgSend)(label, sel_registerName("initWithFrame:"), frame);
	{
		id font = ((id (*)(Class, SEL, double, double))objc_msgSend)(uifont,
		    sel_registerName("systemFontOfSize:weight:"), 108.0, 0.23 /* medium */);
		id white = ((id (*)(Class, SEL))objc_msgSend)(uicolor, sel_registerName("whiteColor"));
		id clear = ((id (*)(Class, SEL))objc_msgSend)(uicolor, sel_registerName("clearColor"));

		((void (*)(id, SEL, id))objc_msgSend)(label, sel_registerName("setFont:"), font);
		((void (*)(id, SEL, id))objc_msgSend)(label, sel_registerName("setTextColor:"), white);
		((void (*)(id, SEL, id))objc_msgSend)(label, sel_registerName("setBackgroundColor:"), clear);
	}
	((void (*)(id, SEL, long))objc_msgSend)(label, sel_registerName("setTextAlignment:"), (long)1 /* center */);
	((void (*)(id, SEL, unsigned long))objc_msgSend)(label, sel_registerName("setAutoresizingMask:"), 2 /* flexible width */);
	((void (*)(id, SEL, long))objc_msgSend)(label, sel_registerName("setTag:"), (long)0x63a2d2);
	((void (*)(id, SEL, id))objc_msgSend)(window, sel_registerName("addSubview:"), label);
	clock_overlay_label = label;
	say("redwood26: clock overlay: label added (%ux%u), thread starts\n", (unsigned)width, (unsigned)height);
	clock_overlay_set_text();
	clock_overlay_running = 1;
	if (pthread_create(&thread, NULL, clock_overlay_thread, NULL) == 0)
		pthread_detach(thread);
}

typedef void (*cs_update_font_fn)(id, SEL, id, double, double, id, id, id);
static cs_update_font_fn orig_cs_update_font;
static int plainSentVibrancyLog;

static void
cs_update_font(id self, SEL sel, id font, double maxYPortrait, double maxYLandscape, id vibrancy,
    id numberingSystem, id contentStyle)
{
	static int replaced;
	id plain = contentStyle;

	if (!replaced) {
		Class discrete = objc_getClass("PRPosterContentDiscreteColorsStyle");
		Class uicolor = objc_getClass("UIColor");
		SEL init = sel_registerName("initWithOpaqueColors:");

		if (discrete != Nil && uicolor != Nil && class_getInstanceMethod(discrete, init) != NULL) {
			id white = ((id (*)(Class, SEL))objc_msgSend)(uicolor, sel_registerName("whiteColor"));
			id list = ((id (*)(Class, SEL, id))objc_msgSend)(objc_getClass("NSArray"),
			    sel_registerName("arrayWithObject:"), white);
			id alloc = ((id (*)(Class, SEL))objc_msgSend)(discrete, sel_registerName("alloc"));

			plain = ((id (*)(id, SEL, id))objc_msgSend)(alloc, init, list);
			say("redwood26: clockplain: title content style %s -> %s\n",
			    contentStyle != nil ? class_getName(object_getClass(contentStyle)) : "nil",
			    plain != nil ? class_getName(object_getClass(plain)) : "nil");
			replaced = 1;
		}
	}
	if (!plainSentVibrancyLog) {
		plainSentVibrancyLog = 1;
		say("redwood26: clockplain: vibrancy nil, style %s\n", plain != nil ? class_getName(object_getClass(plain)) : "nil");
	}
	orig_cs_update_font(self, sel, font, maxYPortrait, maxYLandscape, nil, numberingSystem, plain);
	redwood_clock_overlay(self);
}


/* ---- clockgrad=1: a plain gradient under the cover sheet -------------------------------------------------- */

static void (*orig_cs_view_did_load)(id, SEL);

static void
cs_view_did_load(id self, SEL sel)
{
	orig_cs_view_did_load(self, sel);
	Class view_class = objc_getClass("UIView");
	Class layer_class = objc_getClass("CAGradientLayer");
	id view = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("view"));

	if (view == nil || view_class == Nil || layer_class == Nil)
		return;
	if (((id (*)(id, SEL, long))objc_msgSend)(view, sel_registerName("viewWithTag:"), (long)0x63a2d1) != nil)
		return;
	{
		id background = ((id (*)(id, SEL, struct CGRect))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(view_class,
		    sel_registerName("alloc")), sel_registerName("initWithFrame:"),
		    ((struct CGRect (*)(id, SEL))objc_msgSend)(view, sel_registerName("bounds")));
		id gradient = ((id (*)(Class, SEL))objc_msgSend)(layer_class, sel_registerName("layer"));
		id cols[2];
		Class uicolor = objc_getClass("UIColor");
		cols[0] = ((id (*)(Class, SEL, double, double, double, double))objc_msgSend)(uicolor,
		    sel_registerName("colorWithRed:green:blue:alpha:"), 0.37, 0.90, 0.86, 1.0);
		cols[1] = ((id (*)(Class, SEL, double, double, double, double))objc_msgSend)(uicolor,
		    sel_registerName("colorWithRed:green:blue:alpha:"), 0.23, 0.55, 0.94, 1.0);
		id colors = ((id (*)(Class, SEL, id *, unsigned long))objc_msgSend)(objc_getClass("NSArray"),
		    sel_registerName("arrayWithObjects:count:"), cols, 2);

		((void (*)(id, SEL, long))objc_msgSend)(background, sel_registerName("setTag:"), (long)0x63a2d1);
		((void (*)(id, SEL, struct CGRect))objc_msgSend)(gradient, sel_registerName("setFrame:"),
		    ((struct CGRect (*)(id, SEL))objc_msgSend)(view, sel_registerName("bounds")));
		((void (*)(id, SEL, id))objc_msgSend)(gradient, sel_registerName("setColors:"), colors);
		((void (*)(id, SEL, struct CGPoint))objc_msgSend)(gradient, sel_registerName("setStartPoint:"),
		    (struct CGPoint){ 0.5, 0.0 });
		((void (*)(id, SEL, struct CGPoint))objc_msgSend)(gradient, sel_registerName("setEndPoint:"),
		    (struct CGPoint){ 0.5, 1.0 });
		((void (*)(id, SEL, id))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(background, sel_registerName("layer")),
		    sel_registerName("addSublayer:"), gradient);
		((void (*)(id, SEL, unsigned long))objc_msgSend)(background, sel_registerName("setAutoresizingMask:"), 18);
		((void (*)(id, SEL, id, unsigned long))objc_msgSend)(view, sel_registerName("insertSubview:atIndex:"),
		    background, 0);
		say("redwood26: clockgrad: gradient background inserted under the cover sheet\n");
	}
}


/* ---- clockgrad=1: a plain gradient in the wallpaper window -------------------------------------------- */

static void
clockgrad_install(void *context)
{
	Class uiview = objc_getClass("UIView");
	Class gradient_class = objc_getClass("CAGradientLayer");
	Class uicolor = objc_getClass("UIColor");
	id app, windows;
	unsigned long count, i;

	(void)context;
	if (uiview == Nil || gradient_class == Nil || uicolor == Nil)
		return;
	app = ((id (*)(id, SEL))objc_msgSend)(objc_getClass("UIApplication"), sel_registerName("sharedApplication"));
	windows = ((id (*)(id, SEL))objc_msgSend)(app, sel_registerName("windows"));
	count = ((unsigned long (*)(id, SEL))objc_msgSend)(windows, sel_registerName("count"));
	for (i = 0; i < count; i++) {
		id window = ((id (*)(id, SEL, unsigned long))objc_msgSend)(windows, sel_registerName("objectAtIndex:"), i);
		const char *kind = class_getName(object_getClass(window));
		id background, gradient, colors;
		id cols[2];

		if (kind == NULL || (strstr(kind, "Wallpaper") == NULL && strstr(kind, "CoverSheet") == NULL))
			continue;
		if (((id (*)(id, SEL, long))objc_msgSend)(window, sel_registerName("viewWithTag:"), (long)0x63a2d1) != nil)
			return;
		background = ((id (*)(id, SEL, struct CGRect))objc_msgSend)(((id (*)(Class, SEL))objc_msgSend)(uiview,
		    sel_registerName("alloc")), sel_registerName("initWithFrame:"),
		    ((struct CGRect (*)(id, SEL))objc_msgSend)(window, sel_registerName("bounds")));
		gradient = ((id (*)(Class, SEL))objc_msgSend)(gradient_class, sel_registerName("layer"));
		cols[0] = ((id (*)(Class, SEL, double, double, double, double))objc_msgSend)(uicolor,
		    sel_registerName("colorWithRed:green:blue:alpha:"), 0.37, 0.90, 0.86, 1.0);
		cols[1] = ((id (*)(Class, SEL, double, double, double, double))objc_msgSend)(uicolor,
		    sel_registerName("colorWithRed:green:blue:alpha:"), 0.23, 0.55, 0.94, 1.0);
		colors = ((id (*)(Class, SEL, id *, unsigned long))objc_msgSend)(objc_getClass("NSArray"),
		    sel_registerName("arrayWithObjects:count:"), cols, 2);
		((void (*)(id, SEL, long))objc_msgSend)(background, sel_registerName("setTag:"), (long)0x63a2d1);
		((void (*)(id, SEL, struct CGRect))objc_msgSend)(gradient, sel_registerName("setFrame:"),
		    ((struct CGRect (*)(id, SEL))objc_msgSend)(window, sel_registerName("bounds")));
		((void (*)(id, SEL, id))objc_msgSend)(gradient, sel_registerName("setColors:"), colors);
		((void (*)(id, SEL, struct CGPoint))objc_msgSend)(gradient, sel_registerName("setStartPoint:"),
		    (struct CGPoint){ 0.5, 0.0 });
		((void (*)(id, SEL, struct CGPoint))objc_msgSend)(gradient, sel_registerName("setEndPoint:"),
		    (struct CGPoint){ 0.5, 1.0 });
		((void (*)(id, SEL, id))objc_msgSend)(((id (*)(id, SEL))objc_msgSend)(background, sel_registerName("layer")),
		    sel_registerName("addSublayer:"), gradient);
		((void (*)(id, SEL, unsigned long))objc_msgSend)(background, sel_registerName("setAutoresizingMask:"), 18);
		((void (*)(id, SEL, id, unsigned long))objc_msgSend)(window, sel_registerName("insertSubview:atIndex:"),
		    background, 0);
		say("redwood26: clockgrad: gradient inserted into %s\n", kind);
		return;
	}
	say("redwood26: clockgrad: no wallpaper/cover sheet window among %lu\n", count);
}

static void
springboard_fixes(void)
{
	{
		char args[1024] = { 0 };
		size_t size = sizeof(args) - 1;

		if (!(sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "sb_wallpaper=0") != NULL)) {
			/* not on the main queue: the service answers from SpringBoard's main thread, so asking there waits on itself */
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL,
			    sb_wake_posterboard);
			dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 40 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL,
			    sb_restore_wallpaper);
		}
	}
	int activation = swizzle("SBLockdownManager", "isActivated", (IMP)sb_is_activated, NULL);
	int setup = swizzle("SBSetupManager", "_setSetupRequiredReason:", (IMP)sb_set_setup_required_reason,
	    (IMP *)&orig_set_setup_required_reason);

	{
		char args[1024] = { 0 };
		size_t size = sizeof(args) - 1;

		if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "clockplain=1") != NULL) {
			int ok = swizzle("CSCoverSheetViewController",
			    "updateFont:preferredTimeMaxYPortrait:preferredTimeMaxYLandscape:vibrancyConfiguration:numberingSystem:contentStyle:",
			    (IMP)cs_update_font, (IMP *)&orig_cs_update_font);
			say("redwood26: clockplain: CoverSheet title style hook %s\n", ok ? "installed" : "NOT installed");
		}
	}


	/* clockgrad=1: our own plain gradient behind the cover sheet.  The poster path is skipped with
	 * sb_wallpaper=0, so this is the lock screen's background; a CAGradientLayer is an ordinary layer
	 * that every renderer draws. */
	if (boot_arg_on("clockgrad=1")) {
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 70 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL,
		    clockgrad_install);
		dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL,
		    clockgrad_install);
		say("redwood26: clockgrad: gradient insertion scheduled\n");
	}
	/* displaykeep=1: no auto-lock.  The display path has no backlight and no wake sequence, so a
	 * sleep here is unrecoverable until Android; keep the screen awake instead. */
	if (boot_arg_on("displaykeep=1")) {
		CFNumberRef huge = CFNumberCreate(NULL, kCFNumberDoubleType, &(double){ 2147483647.0 });
		CFPreferencesSetAppValue(CFSTR("SBAutoLockTime"), huge, CFSTR("com.apple.springboard"));
		CFPreferencesAppSynchronize(CFSTR("com.apple.springboard"));
		say("redwood26: displaykeep=1: auto-lock pushed out\n");
	}

	say("redwood: SpringBoard: activation %s, setup assistant %s\n",
	    activation ? "forced on" : "NOT replaced", setup ? "refused" : "NOT replaced");
}

/*
 * PosterBoard: why no poster is made. The first lock and home screen posters are made by PosterBoard's role
 * coordinators from +[PBFInitialPosterHelper initialPosterConfigurationsWithModelCoordinatorProvider:role:], out of
 * the poster extensions it knows. Each step is said here, and 30 s on what the data store has: its extensions and
 * each role's posters.
 */
static const char *
pb_desc(id object, char *buffer, size_t size)
{
	id text = object != nil ? ((id (*)(id, SEL))objc_msgSend)(object, sel_registerName("description")) : nil;
	const char *c = text != nil ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "nil";

	strlcpy(buffer, c != NULL ? c : "?", size);
	for (char *p = buffer; *p; p++) {
		if (*p == '\n') {
			*p = ' ';
		}
	}
	return buffer;
}

static unsigned long
pb_count(id object)
{
	SEL count = sel_registerName("count");

	return object != nil && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(object, sel_registerName("respondsToSelector:"), count)
	    ? ((unsigned long (*)(id, SEL))objc_msgSend)(object, count) : 0;
}

static id (*orig_initial_posters)(id, SEL, id, id);
static void (*orig_setup_role)(id, SEL, id);
static void (*orig_setup_coordinators)(id, SEL);
static id pb_store;

static id
pb_initial_posters(id self, SEL sel, id provider, id role)
{
	char a[160], b[400];
	id result = orig_initial_posters(self, sel, provider, role);

	say("redwood: PosterBoard: initial posters for %s: %lu (%s)\n", pb_desc(role, a, sizeof(a)), pb_count(result),
	    pb_desc(result, b, sizeof(b)));
	return result;
}

static void
pb_setup_role(id self, SEL sel, id storage)
{
	char a[160];
	id role = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("role"));

	say("redwood: PosterBoard: setting up role %s\n", pb_desc(role, a, sizeof(a)));
	orig_setup_role(self, sel, storage);
}

static void
pb_setup_coordinators(id self, SEL sel)
{
	pb_store = self;
	say("redwood: PosterBoard: the data store sets up its role coordinators\n");
	orig_setup_coordinators(self, sel);
}

static void pb_report(void *context);

/*
 * ... and the first lock screen poster, asked for again once the poster extensions are known. The role coordinators
 * are set up as the data store is built, which here is before PosterBoard has found a single poster extension (/var
 * is made at every boot and the extension registry is still being filled), so +[PBFInitialPosterHelper
 * initialPosterConfigurationsWithModelCoordinatorProvider:role:] finds neither the hero poster's extension nor
 * com.apple.PaperBoard.LegacyPoster (-pbf_posterExtensionStoreCoordinatorForProviderIdentifier:error: answers nil)
 * and the role starts with none (runs glass8, glass10: "initial posters for PRPosterRoleLockScreen: 0 (nil)").
 * PosterBoard asks again only when a change to the role is finalised with no poster left
 * (-[PBFPosterRoleCoordinator finalizeChangesWithChangeHandler:outEvents:error:]). Asked here the same way, and what
 * it answers -- PRMutablePosterConfigurations -- handed in as incoming posters, which adds them to the role
 * (-processIncomingPosterConfiguration:completion: -> addPosterToRole:incomingPoster:). Once per boot.
 */
static int pb_initial_tried;

/*
 * What the helper finds, step by step, while it is asked again (pb_tracing) -- and every failure to find an extension's
 * store at any time. PosterBoard's own log of these steps (PBFLogPosterContents) reaches the console only in the live
 * log of a run, not in what is kept of it (run glass11).
 */
static volatile int pb_tracing;
static id (*orig_store_for_provider)(id, SEL, id, id *);
static id (*orig_static_descriptors)(id, SEL, id);
static id (*orig_create_dynamic)(id, SEL, id, id, id *);
static BOOL (*orig_hero)(id, SEL);
static id (*orig_incoming)(id, SEL, id, id, id);

static id
pb_store_for_provider(id self, SEL sel, id identifier, id *error)
{
	static int failures;
	char a[200], b[600];
	id local = nil;
	id result = orig_store_for_provider(self, sel, identifier, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	if (pb_tracing || (result == nil && failures++ < 20)) {
		say("redwood: PosterBoard: the store of extension %s: %s%s%s\n", pb_desc(identifier, a, sizeof(a)),
		    result != nil ? "found" : "NONE", e != nil ? ", error " : "", e != nil ? pb_desc(e, b, sizeof(b)) : "");
	}
	return result;
}

static id
pb_static_descriptors(id self, SEL sel, id identifier)
{
	char a[200];
	id result = orig_static_descriptors(self, sel, identifier);

	if (pb_tracing) {
		BOOL known = result != nil && ((BOOL (*)(id, SEL))objc_msgSend)(result, sel_registerName("hasKnownVersions"));
		say("redwood: PosterBoard: static descriptor %s: %s\n", pb_desc(identifier, a, sizeof(a)),
		    result == nil ? "none" : known ? "with versions" : "no versions");
	}
	return result;
}

static id
pb_create_dynamic(id self, SEL sel, id identifier, id role, id *error)
{
	char a[200], b[600];
	id local = nil;
	id result = orig_create_dynamic(self, sel, identifier, role, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	say("redwood: PosterBoard: dynamic descriptor %s made: %s%s%s\n", pb_desc(identifier, a, sizeof(a)),
	    result != nil ? "yes" : "NO", e != nil ? ", error " : "", e != nil ? pb_desc(e, b, sizeof(b)) : "");
	return result;
}

static BOOL
pb_hero(id self, SEL sel)
{
	BOOL hero = orig_hero(self, sel);

	say("redwood: PosterBoard: the hero poster as the default lock screen: %s\n", hero ? "yes" : "no");
	return hero;
}

static id
pb_incoming(id self, SEL sel, id role, id path, id info)
{
	char a[300], b[300];
	id result = orig_incoming(self, sel, role, path, info);

	say("redwood: PosterBoard: incoming configuration from %s: %s\n", pb_desc(path, a, sizeof(a)),
	    pb_desc(result, b, sizeof(b)));
	return result;
}

/* The data store's state lock, which the role coordinators hold when they ask the helper: the os_unfair_lock ivar at
 * 0xc4 in this build (-processIncomingPosterConfiguration:completion:'s block takes it there; 0xc0 beside it is the
 * extension store coordinators' own). Looked up by offset and type, and NULL if this PosterBoard is laid out otherwise. */
static os_unfair_lock *
pb_state_lock(id store)
{
	unsigned count = 0;
	Ivar *ivars = class_copyIvarList(object_getClass(store), &count);
	os_unfair_lock *lock = NULL;

	for (unsigned i = 0; i < count; i++) {
		const char *type = ivar_getTypeEncoding(ivars[i]);

		if (ivar_getOffset(ivars[i]) == 0xc4 && type != NULL && strncmp(type, "{os_unfair_lock", 15) == 0) {
			say("redwood: PosterBoard: the data store's state lock is %s\n", ivar_getName(ivars[i]));
			lock = (os_unfair_lock *)((char *)store + 0xc4);
		}
	}
	free(ivars);
	return lock;
}

static void
pb_make_initial_posters(id role)
{
	char a[160], b[600];
	Class helper = objc_getClass("PBFInitialPosterHelper");
	SEL make = sel_registerName("initialPosterConfigurationsWithModelCoordinatorProvider:role:");
	SEL ingest = sel_registerName("processIncomingPosterConfiguration:completion:");
	id configurations;

	/* An early report can run before ExtensionKit registration completes. Do not
	 * consume the initialization attempt while no wallpaper provider exists. */
	id ready = ((id (*)(id, SEL))objc_msgSend)(pb_store, sel_registerName("knownExtensions"));
	if (pb_count(ready) == 0) {
		say("redwood: PosterBoard: deferring initial posters until an extension is known\n");
		return;
	}
	if (pb_initial_tried++ || helper == Nil || class_getClassMethod(helper, make) == NULL ||
	    !((BOOL (*)(id, SEL, SEL))objc_msgSend)(pb_store, sel_registerName("respondsToSelector:"), ingest)) {
		return;
	}
	os_unfair_lock *lock = pb_state_lock(pb_store);
	if (lock != NULL) {
		os_unfair_lock_lock(lock);
	}
	{
		id known = ((id (*)(id, SEL))objc_msgSend)(pb_store, sel_registerName("knownExtensions"));
		for (unsigned long i = 0; i < pb_count(known); i++) {
			id extension = ((id (*)(id, SEL, unsigned long))objc_msgSend)(known, sel_registerName("objectAtIndex:"), i);
			id identifier = ((id (*)(id, SEL))objc_msgSend)(extension, sel_registerName("posterExtensionBundleIdentifier"));
			say("redwood: PosterBoard: known extension %lu: %s\n", i, pb_desc(identifier, a, sizeof(a)));
		}
	}
	pb_tracing = 1;
	configurations = ((id (*)(Class, SEL, id, id))objc_msgSend)(helper, make, pb_store, role);
	pb_tracing = 0;
	if (lock != NULL) {
		os_unfair_lock_unlock(lock);
	}
	say("redwood: PosterBoard: initial posters for %s, asked again: %lu\n", pb_desc(role, a, sizeof(a)),
	    pb_count(configurations));
	for (unsigned long i = 0; i < pb_count(configurations); i++) {
		id configuration = ((id (*)(id, SEL, unsigned long))objc_msgSend)(configurations, sel_registerName("objectAtIndex:"), i);

		say("redwood: PosterBoard: handing in %s\n", pb_desc(configuration, b, sizeof(b)));
		/* answered (nil, NSError) on failure: only the second argument is looked at */
		((void (*)(id, SEL, id, void (^)(id, id)))objc_msgSend)(pb_store, ingest, configuration, ^(id result, id error) {
			char e[600];

			(void)result;
			say("redwood: PosterBoard: the incoming poster: %s%s\n", error != nil ? "error " : "added",
			    error != nil ? pb_desc(error, e, sizeof(e)) : "");
		});
	}
    if (pb_count(configurations) == 0) {
        id provider = (id)CFSTR("com.apple.GradientPoster.GradientPosterExtension");
        id lookupError = nil;
        id descriptors = ((id (*)(id, SEL, id, id *))objc_msgSend)(pb_store,
            sel_registerName("posterDescriptorsForExtensionBundleIdentifier:error:"), provider, &lookupError);
        char detail[600];
        say("redwood: PosterBoard: available gradient descriptors %s error %s\n",
            pb_desc(descriptors, a, sizeof(a)), pb_desc(lookupError, detail, sizeof(detail)));
        if (pb_count(descriptors) != 0 && ((BOOL (*)(id, SEL, SEL))objc_msgSend)(descriptors, sel_registerName("respondsToSelector:"), sel_registerName("objectAtIndex:"))) {
            id descriptor = ((id (*)(id, SEL, unsigned long))objc_msgSend)(descriptors, sel_registerName("objectAtIndex:"), 0);
            SEL identifierSelector = sel_registerName("descriptorIdentifier");
            if (!((BOOL (*)(id, SEL, SEL))objc_msgSend)(descriptor, sel_registerName("respondsToSelector:"), identifierSelector))
                identifierSelector = sel_registerName("identifier");
            if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(descriptor, sel_registerName("respondsToSelector:"), identifierSelector)) {
                id identifier = ((id (*)(id, SEL))objc_msgSend)(descriptor, identifierSelector);
                if (identifier != nil) {
                    char actual[300];
                    say("redwood: PosterBoard: requesting native gradient descriptor %s\n", pb_desc(identifier, actual, sizeof(actual)));
                    ((void (*)(id, SEL, id, id, id, void (^)(id, id)))objc_msgSend)(pb_store,
                        sel_registerName("createPosterConfigurationForProviderIdentifier:posterDescriptorIdentifier:role:completion:"),
                        provider, identifier, role, ^(id configuration, id error) {
                            char message[600];
                            say("redwood: PosterBoard: native gradient configuration %s error %s\n",
                                configuration != nil ? "created" : "failed", pb_desc(error, message, sizeof(message)));
                            /* The native API already adds the poster to its role and
                             * returns a PFPosterPath; do not ingest that path as a configuration. */
                            dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC),
                                dispatch_get_global_queue(0, 0), NULL, pb_report);
                        });
                }
            }
        }
    }
	/* and what the roles hold afterwards */
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL, pb_report);
}

/* The extension registry can still be empty when the first delayed report runs.
 * Ask the store to reload through its own locking/notification path, then retry
 * after completion. Never wait on the callback or manufacture a poster result. */
static void
pb_refresh_extensions(void)
{
    static unsigned attempts;
    unsigned attempt = __sync_fetch_and_add(&attempts, 1);
    SEL update = sel_registerName("updateExtensionsAndNotifyWhenComplete:");
    if (attempt >= 6 || pb_store == nil ||
        !((BOOL (*)(id, SEL, SEL))objc_msgSend)(pb_store, sel_registerName("respondsToSelector:"), update)) {
        return;
    }
    id provider = ((id (*)(id, SEL))objc_msgSend)(pb_store, sel_registerName("extensionProvider"));
    say("redwood: PosterBoard: extension refresh %u, provider %p\n", attempt + 1, (void *)provider);
    if (provider == nil) {
        SEL setup = sel_registerName("_setupExtensionProvider");
        if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(pb_store, sel_registerName("respondsToSelector:"), setup)) {
            ((void (*)(id, SEL))objc_msgSend)(pb_store, setup);
        }
    }
    ((void (*)(id, SEL, void (^)(void)))objc_msgSend)(pb_store, update, ^{
        say("redwood: PosterBoard: extension refresh completed\n");
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC),
            dispatch_get_global_queue(0, 0), NULL, pb_report);
    });
}

static void
pb_report(void *context)
{
	char a[160], b[600];
	id roles, extensions;

	(void)context;
	if (pb_store == nil) {
		/* nobody has asked PosterBoard anything yet (SpringBoard asks only once there is a poster): its data
		 * store, and with it the role coordinators and their first posters, made the way a client's request makes it.
		 * -dataStore only answers the one already made (nil: runs wp3, wp4); -_dataStoreWithError: makes it
		 * (-_lock_dataStoreWithError:: base URL, migrations, buildDataStoreForURL:...) and says why it could not. */
		Class glue = objc_getClass("PBFPosterExtensionDataStoreXPCServiceGlue");
		id shared = glue != Nil ? ((id (*)(Class, SEL))objc_msgSend)(glue, sel_registerName("sharedInstance")) : nil;
		id error = nil;
		id store = shared != nil ? ((id (*)(id, SEL, id *))objc_msgSend)(shared, sel_registerName("_dataStoreWithError:"),
		    &error) : nil;

		say("redwood: PosterBoard: no data store after 30 s: made one: %s%s%s\n", pb_desc(store, a, sizeof(a)),
		    error != nil ? ", error " : "", error != nil ? pb_desc(error, b, sizeof(b)) : "");
		if (pb_store == nil) {
			pb_store = store;
		}
		if (pb_store == nil) {
			return;
		}
	}
	extensions = ((id (*)(id, SEL))objc_msgSend)(pb_store, sel_registerName("knownExtensions"));
	say("redwood: PosterBoard: %lu poster extensions known: %s\n", pb_count(extensions), pb_desc(extensions, b, sizeof(b)));
	roles = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("PBFPosterExtensionDataStore"), sel_registerName("supportedRoles"));
	roles = ((id (*)(id, SEL))objc_msgSend)(roles, sel_registerName("allObjects"));
	for (unsigned long i = 0; i < pb_count(roles); i++) {
		id role = ((id (*)(id, SEL, unsigned long))objc_msgSend)(roles, sel_registerName("objectAtIndex:"), i);
		id error = nil;
		id posters = ((id (*)(id, SEL, id, id *))objc_msgSend)(pb_store, sel_registerName("posterConfigurationsForRole:error:"),
		    role, &error);

		say("redwood: PosterBoard: role %s: %lu posters%s%s\n", pb_desc(role, a, sizeof(a)), pb_count(posters),
		    error != nil ? ", error " : "", error != nil ? pb_desc(error, b, sizeof(b)) : "");
		if (error == nil && pb_count(posters) == 0 && pb_count(extensions) != 0 && strcmp(a, "PRPosterRoleLockScreen") == 0) {
			pb_make_initial_posters(role);
		}
	}
    if (pb_count(extensions) == 0) {
        pb_refresh_extensions();
    }
}

/*
 * Why the data store is not made: PosterBoard's own log (run glass1, 28 October) says "Beginning migration to
 * Version 59", "Failed to migrate to Version 61 from 59", then "could not initialize data store: Data store
 * could not be created for an un[known reason]" -- on a /var made fresh at every boot. The migration and the
 * build, with their arguments and the NSError they give back.
 */
static BOOL (*orig_migrate)(Class, SEL, id, unsigned long, unsigned long, BOOL, id *);

static BOOL
pb_migrate(Class self, SEL sel, id url, unsigned long from, unsigned long to, BOOL cleanup, id *error)
{
	char a[300], b[1200];
	id local = nil;
	BOOL ok = orig_migrate(self, sel, url, from, to, cleanup, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	say("redwood: PosterBoard: migrate %s from %lu to %lu: %s%s%s\n", pb_desc(url, a, sizeof(a)), from, to,
	    ok ? "done" : "FAILED", e != nil ? ", error " : "", e != nil ? pb_desc(e, b, sizeof(b)) : "");
	return ok;
}

static id (*orig_build)(id, SEL, id, id, id, id, BOOL, id *);

static id
pb_build(id self, SEL sel, id url, id assertions, id monitor, id observer, BOOL migrated, id *error)
{
	char a[300], b[1200];
	id local = nil;
	id store = orig_build(self, sel, url, assertions, monitor, observer, migrated, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	say("redwood: PosterBoard: build data store at %s (migrated %d): %s%s%s\n", pb_desc(url, a, sizeof(a)),
	    (int)migrated, store != nil ? "made" : "NONE", e != nil ? ", error " : "", e != nil ? pb_desc(e, b, sizeof(b)) : "");
	return store;
}

/* ... and the file operations the migrator makes, each failure said with its NSError: the migration
 * fails with none (run glass3: "migrate ... from 59 to 61: FAILED", no error), after copying a version's
 * store forward and making its directories with NSFileProtectionNone (PFFileProtectionNoneAttributes). */
static BOOL (*orig_fm_create)(id, SEL, id, BOOL, id, id *);
static BOOL (*orig_fm_copy)(id, SEL, id, id, id *);
static BOOL (*orig_url_update)(id, SEL, id, id *);

/* `dict` without `key`, or nil when it has no such key (PosterBoard's attributes and resource values) */
static id
pb_without(id dict, const char *key)
{
	id name = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"),
	    sel_registerName("stringWithUTF8String:"), key);

	if (dict == nil || ((id (*)(id, SEL, id))objc_msgSend)(dict, sel_registerName("objectForKey:"), name) == nil) {
		return nil;
	}
	id copy = ((id (*)(id, SEL))objc_msgSend)(dict, sel_registerName("mutableCopy"));
	((void (*)(id, SEL, id))objc_msgSend)(copy, sel_registerName("removeObjectForKey:"), name);
	return copy;
}

/*
 * The check that fails it. The migrator makes a version's directories (store, extension container, gallery
 * cache) and then asks __PBFDetermineDataStoreViabilityForVersionWithinBaseURL whether that version is viable:
 * each must exist AND -[NSURL pbf_URLIsReachableAndConformToResourceValues:error:] against
 * PFPosterPathURLResourceValues(), whose file protection class this port's /var (no keybag, no data protection)
 * never reports -- so a store made a moment ago was "not viable" and the migration gave up with no error (run
 * glass4: 59 -> 61 FAILED, every directory made). Checked again without the class when the first answer is no.
 */
static BOOL (*orig_url_conform)(id, SEL, id, id *);

static BOOL
pb_url_conform(id self, SEL sel, id values, id *error)
{
	char a[300], b[600];
	id local = nil;
	BOOL ok = orig_url_conform(self, sel, values, error != NULL ? error : &local);

	if (!ok) {
		id plain = pb_without(values, "NSURLFileProtectionKey");
		if (plain != nil) {
			local = nil;
			ok = orig_url_conform(self, sel, plain, &local);
		}
		static unsigned said;
		if (said++ < 12) {
			say("redwood: PosterBoard: %s does not conform to %s; without the protection class: %s\n",
			    pb_desc(self, a, sizeof(a)), pb_desc(values, b, sizeof(b)), ok ? "conforms" : "STILL NOT");
		}
		if (ok && error != NULL) {
			*error = nil;
		}
	}
	return ok;
}

static BOOL
pb_fm_create(id self, SEL sel, id url, BOOL intermediate, id attributes, id *error)
{
	char a[300], b[300], c[900];
	id local = nil;
	BOOL ok = orig_fm_create(self, sel, url, intermediate, attributes, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	if (!ok) {
		say("redwood: PosterBoard: mkdir %s (attributes %s) FAILED: %s\n", pb_desc(url, a, sizeof(a)),
		    pb_desc(attributes, b, sizeof(b)), pb_desc(e, c, sizeof(c)));
		/* this port's /var has no data protection classes (no keybag): once more without the class */
		id plain = pb_without(attributes, "NSFileProtectionKey");
		if (plain != nil) {
			local = nil;
			ok = orig_fm_create(self, sel, url, intermediate, plain, error != NULL ? error : &local);
			say("redwood: PosterBoard: mkdir %s without NSFileProtectionKey: %s\n", pb_desc(url, a, sizeof(a)),
			    ok ? "made" : "FAILED again");
		}
	}
	return ok;
}

/*
 * ... and the files it writes with -[NSData writeToURL:options:error:]: a descriptor's identity
 * (PRBPosterExtensionDataStore/61/Extensions/<extension>/descriptors/<UUID>/com.apple.posterkit.provider.descriptor.identifier)
 * is written NSDataWritingAtomic | NSDataWritingFileProtectionNone by +[PBFPosterModelStoreCoordinator
 * createNewStoreCoordinatorWithPath:error:], and that write failed for every default poster (run glass12: "dynamic
 * descriptor space.dynamic made: NO ... errno=-1"), so no lock screen poster was ever made. Said with its error, then
 * once more without the protection class, then without the atomic rename as well.
 */
static BOOL (*orig_data_write)(id, SEL, id, unsigned long, id *);

static BOOL
pb_data_write(id self, SEL sel, id url, unsigned long options, id *error)
{
	char a[300], c[900];
	id local = nil;
	BOOL ok = orig_data_write(self, sel, url, options, error != NULL ? error : &local);
	int saved = errno;
	id e = error != NULL ? *error : local;

	if (!ok) {
		say("redwood: PosterBoard: write %s (options %#lx) FAILED, errno %d: %s\n", pb_desc(url, a, sizeof(a)), options,
		    saved, pb_desc(e, c, sizeof(c)));
		if (options & 0xf0000000ul) {
			local = nil;
			ok = orig_data_write(self, sel, url, options & ~0xf0000000ul, error != NULL ? error : &local);
			say("redwood: PosterBoard: ... without the protection class: %s\n", ok ? "written" : "FAILED again");
		}
		if (!ok && (options & 1ul)) {
			local = nil;
			ok = orig_data_write(self, sel, url, options & ~0xf0000001ul, error != NULL ? error : &local);
			say("redwood: PosterBoard: ... and not atomically: %s\n", ok ? "written" : "FAILED again");
		}
	}
	return ok;
}

/*
 * Who makes PosterBoard's "errno=-1" errors: the dynamic descriptor of every default poster failed with one whose
 * URL is the descriptor's identity file, after that file was written (run glass13: no write failed). Each such error
 * is said with the calls that made it, as image + offset from the image's header (dladdr names only exported symbols).
 */
static id (*orig_general_error)(id, SEL, long, id);

static id
pb_general_error(id self, SEL sel, long code, id info)
{
	static int said;
	char b[600];

	if (said++ < 6) {
		uintptr_t fp = (uintptr_t)__builtin_frame_address(0);

		say("redwood: PosterBoard: general error %ld %s, made by:\n", code, pb_desc(info, b, sizeof(b)));
		for (int i = 0; i < 10 && fp != 0 && (fp & 7) == 0; i++) {
			uintptr_t *frame = (uintptr_t *)fp;
			uintptr_t ret = frame[1] & 0x0000007fffffffffUL;
			Dl_info dl;

			if (ret == 0) {
				break;
			}
			if (dladdr((void *)ret, &dl) && dl.dli_fname != NULL) {
				say("redwood: PosterBoard:   %s+%#lx (%s)\n", strrchr(dl.dli_fname, '/') ? strrchr(dl.dli_fname, '/') + 1 :
				    dl.dli_fname, (unsigned long)(ret - (uintptr_t)dl.dli_fbase), dl.dli_sname != NULL ? dl.dli_sname : "?");
			}
			fp = frame[0];
		}
	}
	return orig_general_error(self, sel, code, info);
}

/* ... and the same for one made straight from the domain (PosterFoundation does not have pbf_generalErrorWithCode:) */
static id (*orig_error_with_domain)(id, SEL, id, long, id);

static id
pb_error_with_domain(id self, SEL sel, id domain, long code, id info)
{
	static int said;
	char d[120];

	if (said < 6 && domain != nil && strcmp(pb_desc(domain, d, sizeof(d)), "com.apple.PosterBoard.generalErrorDomain") == 0) {
		uintptr_t fp = (uintptr_t)__builtin_frame_address(0);
		char b[600];

		said++;
		say("redwood: PosterBoard: error %s %ld %s, made by:\n", d, code, pb_desc(info, b, sizeof(b)));
		for (int i = 0; i < 10 && fp != 0 && (fp & 7) == 0; i++) {
			uintptr_t *frame = (uintptr_t *)fp;
			uintptr_t ret = frame[1] & 0x0000007fffffffffUL;
			Dl_info dl;

			if (ret == 0) {
				break;
			}
			if (dladdr((void *)ret, &dl) && dl.dli_fname != NULL) {
				say("redwood: PosterBoard:   %s+%#lx (%s)\n", strrchr(dl.dli_fname, '/') ? strrchr(dl.dli_fname, '/') + 1 :
				    dl.dli_fname, (unsigned long)(ret - (uintptr_t)dl.dli_fbase), dl.dli_sname != NULL ? dl.dli_sname : "?");
			}
			fp = frame[0];
		}
	}
	return orig_error_with_domain(self, sel, domain, code, info);
}

/*
 * THE WALLPAPER'S LAST STEP. Every store PosterBoard makes for a poster or a descriptor has its internal directories
 * marked purgeable first (-[PBFPosterModelStoreCoordinator _accessQueue_correctPermissionsForInternalDirectories:],
 * -[PBFPosterExtensionStoreCoordinator _correctPermissionsForInternalDirectories]) by -[NSURL pbf_setPurgable:error:]:
 * open() and ffsctl(fd, APFSIOC_MARK_PURGEABLE). That is APFS's, and /var here is the kernel's ramfs, whose ioctl
 * answers ENOTTY to everything; the failure comes back as an error with "errno" -1 (the return value), the store is
 * not made, and so neither the hero poster (com.apple.MercuryPoster) nor the legacy one could be set up: no lock screen
 * poster, a black wallpaper (runs glass12-14: "failed to correct file purgable", "errno=-1"). Purgeable is a hint
 * for reclaiming space, which a memory filesystem has no use for: a failure is taken as done.
 */
static BOOL (*orig_set_purgable)(id, SEL, BOOL, id *);

static BOOL
pb_set_purgable(id self, SEL sel, BOOL purgable, id *error)
{
	static int said;
	char a[300];
	id local = nil;
	BOOL ok = orig_set_purgable(self, sel, purgable, error != NULL ? error : &local);

	if (!ok) {
		if (said++ < 3) {
			say("redwood: PosterBoard: %s cannot be marked purgeable here (no APFS): taken as done\n", pb_desc(self, a, sizeof(a)));
		}
		if (error != NULL) {
			*error = nil;
		}
		ok = YES;
	}
	return ok;
}

static BOOL
pb_fm_copy(id self, SEL sel, id from, id to, id *error)
{
	char a[300], b[300], c[900];
	id local = nil;
	BOOL ok = orig_fm_copy(self, sel, from, to, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	if (!ok) {
		say("redwood: PosterBoard: copy %s -> %s FAILED: %s\n", pb_desc(from, a, sizeof(a)), pb_desc(to, b, sizeof(b)),
		    pb_desc(e, c, sizeof(c)));
	}
	return ok;
}

static BOOL
pb_url_update(id self, SEL sel, id values, id *error)
{
	char a[300], b[400], c[900];
	id local = nil;
	BOOL ok = orig_url_update(self, sel, values, error != NULL ? error : &local);
	id e = error != NULL ? *error : local;

	if (!ok) {
		say("redwood: PosterBoard: resource values %s on %s FAILED: %s\n", pb_desc(values, b, sizeof(b)),
		    pb_desc(self, a, sizeof(a)), pb_desc(e, c, sizeof(c)));
		id plain = pb_without(values, "NSURLFileProtectionKey");
		if (plain != nil) {
			local = nil;
			ok = orig_url_update(self, sel, plain, error != NULL ? error : &local);
			say("redwood: PosterBoard: resource values on %s without NSURLFileProtectionKey: %s\n",
			    pb_desc(self, a, sizeof(a)), ok ? "set" : "FAILED again");
		}
	}
	return ok;
}

/*
 * Where PosterBoard traps. With its data store made at last (pb_url_conform) it dies of SIGTRAP ~100 ms after
 * every launch, right after "Data store migration not needed" (run glass5), and no crash report reaches the
 * console. The trap's pc and the frame chain, each named by dladdr, then the default action.
 */
static void
pb_say_frame(const char *what, uintptr_t pc)
{
	Dl_info info;

	if (pc != 0 && dladdr((void *)pc, &info) && info.dli_fname != NULL) {
		const char *image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
		say("redwood: PosterBoard: trap %s %#lx %s`%s+%#lx\n", what, (unsigned long)pc, image,
		    info.dli_sname != NULL ? info.dli_sname : "?",
		    (unsigned long)(pc - (uintptr_t)(info.dli_saddr != NULL ? info.dli_saddr : info.dli_fbase)));
	} else {
		say("redwood: PosterBoard: trap %s %#lx\n", what, (unsigned long)pc);
	}
}

static void
pb_trap(int sig, siginfo_t *si, void *context)
{
	ucontext_t *uc = context;
	uintptr_t pc = (uintptr_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
	uintptr_t lr = (uintptr_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss);
	uintptr_t fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);

	(void)si;
	say("redwood: PosterBoard: signal %d\n", sig);
	pb_say_frame("pc", pc & 0x0000007fffffffffUL);
	pb_say_frame("lr", lr & 0x0000007fffffffffUL);
	for (int i = 0; i < 24 && fp != 0 && (fp & 7) == 0; i++) {
		uintptr_t *frame = (uintptr_t *)fp;
		uintptr_t ret = frame[1] & 0x0000007fffffffffUL;
		if (ret == 0) {
			break;
		}
		pb_say_frame("fr", ret);
		fp = frame[0];
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

/* The port has no StandBy/Ambient poster provider. Enabling that role makes
 * native finalization reject otherwise valid lock-screen wallpaper transactions. */
static id (*orig_poster_supported_roles)(Class, SEL);
static id
pb_supported_roles(Class self, SEL sel)
{
    id roles = orig_poster_supported_roles(self, sel);
    if (roles == nil)
        return roles;
    CFMutableSetRef supported = CFSetCreateMutableCopy(NULL, 0, (CFSetRef)roles);
    if (CFSetContainsValue(supported, CFSTR("PRPosterRoleAmbient"))) {
        CFSetRemoveValue(supported, CFSTR("PRPosterRoleAmbient"));
        static int logged;
        if (__sync_bool_compare_and_swap(&logged, 0, 1))
            say("redwood: PosterBoard: StandBy wallpaper role disabled until its provider is ported\n");
    }
    return ((id (*)(id, SEL))objc_msgSend)((id)supported, sel_registerName("autorelease"));
}

static BOOL (*orig_poster_coordinator_change)(id, SEL, id, id, uintptr_t, id *);
static BOOL
pb_coordinator_change(id self, SEL sel, id change, id handler, uintptr_t emit, id *error)
{
    id local = nil;
    BOOL result = orig_poster_coordinator_change(self, sel, change, handler, emit, error != NULL ? error : &local);
    static unsigned count;
    if (__sync_fetch_and_add(&count, 1) < 12) {
        char operation[350], detail[350];
        say("redwood: PosterBoard: coordinator change %s %s error %s\n", result ? "accepted" : "failed",
            pb_desc(change, operation, sizeof(operation)), pb_desc(error != NULL ? *error : local, detail, sizeof(detail)));
    }
    return result;
}

static BOOL (*orig_poster_finalize)(id, SEL, id, id *, id *);
static BOOL
pb_finalize(id self, SEL sel, id handler, id *events, id *error)
{
    id local = nil;
    BOOL result = orig_poster_finalize(self, sel, handler, events, error != NULL ? error : &local);
    static unsigned count;
    if (__sync_fetch_and_add(&count, 1) < 12) {
        char role[100], detail[450];
        id identifier = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("role"));
        say("redwood: PosterBoard: coordinator finalize %s role %s error %s\n", result ? "accepted" : "failed",
            pb_desc(identifier, role, sizeof(role)), pb_desc(error != NULL ? *error : local, detail, sizeof(detail)));
    }
    return result;
}

static BOOL (*orig_poster_sql_changes)(id, SEL, BOOL (^)(id), id *);
static BOOL
pb_sql_changes(id self, SEL sel, BOOL (^changes)(id), id *error)
{
    id local = nil;
    id connection = ((id (*)(id, SEL))objc_msgSend)(self, sel_registerName("connection"));
    say("redwood: PosterBoard: SQLite transaction connection %p\n", (void *)connection);
    BOOL (^observed)(id) = ^BOOL(id storage) {
        BOOL accepted = changes(storage);
        say("redwood: PosterBoard: SQLite changes block %s\n", accepted ? "accepted" : "failed");
        return accepted;
    };
    BOOL result = orig_poster_sql_changes(self, sel, changes != nil ? observed : changes, error != NULL ? error : &local);
    char detail[500];
    say("redwood: PosterBoard: SQLite transaction %s error %s\n", result ? "accepted" : "failed",
        pb_desc(error != NULL ? *error : local, detail, sizeof(detail)));
    return result;
}

static BOOL (*orig_poster_sql_query)(id, SEL, id, id *);
static BOOL
pb_sql_query(id self, SEL sel, id query, id *error)
{
    id local = nil;
    BOOL result = orig_poster_sql_query(self, sel, query, error != NULL ? error : &local);
    static unsigned count;
    if (!result && __sync_fetch_and_add(&count, 1) < 12) {
        char sql[300], detail[400];
        say("redwood: PosterBoard: SQLite query failed %s error %s\n", pb_desc(query, sql, sizeof(sql)),
            pb_desc(error != NULL ? *error : local, detail, sizeof(detail)));
    }
    return result;
}

static id (*orig_poster_incoming_native)(id, SEL, id, id, id, id *);
static id
pb_incoming_native(id self, SEL sel, id provider, id descriptor, id role, id *error)
{
    id local = nil;
    id result = orig_poster_incoming_native(self, sel, provider, descriptor, role, error != NULL ? error : &local);
    static unsigned count;
    if (__sync_fetch_and_add(&count, 1) < 8) {
        char configuration[350], detail[350];
        say("redwood: PosterBoard: native incoming result %s error %s\n", pb_desc(result, configuration, sizeof(configuration)),
            pb_desc(error != NULL ? *error : local, detail, sizeof(detail)));
    }
    return result;
}

static BOOL (*orig_poster_update_native)(id, SEL, id, id *, unsigned long, id, id, id *);
static BOOL
pb_update_native(id self, SEL sel, id changes, id *diffs, unsigned long options, id reason, id context, id *error)
{
    id local = nil;
    BOOL result = orig_poster_update_native(self, sel, changes, diffs, options, reason, context, error != NULL ? error : &local);
    static unsigned count;
    if (__sync_fetch_and_add(&count, 1) < 8) {
        char delta[350], detail[350];
        say("redwood: PosterBoard: native role update %s diffs %s error %s\n", result ? "accepted" : "failed",
            pb_desc(diffs != NULL ? *diffs : nil, delta, sizeof(delta)), pb_desc(error != NULL ? *error : local, detail, sizeof(detail)));
    }
    return result;
}

static BOOL (*orig_poster_copy_contents)(id, SEL, id, id *);
static BOOL
pb_copy_contents(id self, SEL sel, id path, id *error)
{
    id local = nil;
    BOOL result = orig_poster_copy_contents(self, sel, path, error != NULL ? error : &local);
    static unsigned count;
    if (__sync_fetch_and_add(&count, 1) < 8) {
        char source[350], detail[350];
        say("redwood: PosterBoard: configuration contents copy %s source %s error %s\n",
            result ? "accepted" : "failed", pb_desc(path, source, sizeof(source)),
            pb_desc(error != NULL ? *error : local, detail, sizeof(detail)));
    }
    return result;
}

/* Preserve the native clock configuration while observing whether the new
 * wallpaper receives a usable title font and style. Bounded to startup. */
static id (*orig_poster_default_properties)(Class, SEL, id, id);
static id
pb_default_properties(Class self, SEL sel, id role, id rendering)
{
    id result = orig_poster_default_properties(self, sel, role, rendering);
    char args[1024] = {0};
    size_t argsSize = sizeof(args) - 1;
    if (result != nil && sysctlbyname("kern.bootargs", args, &argsSize, NULL, 0) == 0 && strstr(args, "clocksolid=1") != NULL) {
        Class styleClass = objc_getClass("PRPosterContentDiscreteColorsStyle");
        SEL init = sel_registerName("initWithOpaqueColors:");
        if (styleClass != Nil && class_getInstanceMethod(styleClass, init) != NULL) {
            id title = ((id (*)(id, SEL))objc_msgSend)(result, sel_registerName("titleStyleConfiguration"));
            id mutableTitle = ((id (*)(id, SEL))objc_msgSend)(title, sel_registerName("mutableCopy"));
            id white = ((id (*)(Class, SEL))objc_msgSend)(objc_getClass("UIColor"), sel_registerName("whiteColor"));
            id colors = ((id (*)(Class, SEL, id))objc_msgSend)(objc_getClass("NSArray"), sel_registerName("arrayWithObject:"), white);
            id allocated = ((id (*)(Class, SEL))objc_msgSend)(styleClass, sel_registerName("alloc"));
            id style = ((id (*)(id, SEL, id))objc_msgSend)(allocated, init, colors);
            if (mutableTitle != nil && style != nil) {
                ((void (*)(id, SEL, id))objc_msgSend)(mutableTitle, sel_registerName("setTitleContentStyle:"), style);
                ((void (*)(id, SEL, id))objc_msgSend)(result, sel_registerName("setTitleStyleConfiguration:"), mutableTitle);
                say("redwood: PosterBoard: opaque clock style comparison enabled\n");
            }
            ((void (*)(id, SEL))objc_msgSend)(style, sel_registerName("release"));
            ((void (*)(id, SEL))objc_msgSend)(mutableTitle, sel_registerName("release"));
        }
    }

    static unsigned count;
    if (__sync_fetch_and_add(&count, 1) < 4) {
        id title = ((id (*)(id, SEL))objc_msgSend)(result, sel_registerName("titleStyleConfiguration"));
        id font = ((id (*)(id, SEL))objc_msgSend)(title, sel_registerName("timeFontConfiguration"));
        char r[100], t[350], f[200];
        say("redwood: PosterBoard: default title role %s style %s font %s\n",
            pb_desc(role, r, sizeof(r)), pb_desc(title, t, sizeof(t)), pb_desc(font, f, sizeof(f)));
    }
    return result;
}

static __thread int pb_registering_extension;
static id (*orig_registration_error_init)(id, SEL, id, long, id);
static id
pb_registration_error_init(id self, SEL sel, id domain, long code, id info)
{
    if (pb_registering_extension) {
        char d[160], detail[1000];
        say("redwood: PosterBoard: plugin registration error %s %ld %s\n",
            pb_desc(domain, d, sizeof(d)), code, pb_desc(info, detail, sizeof(detail)));
    }
    return orig_registration_error_init(self, sel, domain, code, info);
}

static id (*orig_poster_bookmark)(id, SEL, id, id *);
static id
pb_registration_bookmark(id self, SEL sel, id relative, id *error)
{
    id local = nil;
    id result = orig_poster_bookmark(self, sel, relative, error != NULL ? error : &local);
    static unsigned failures;
    if (result == nil && __sync_fetch_and_add(&failures, 1) < 8) {
        char node[500], detail[1000];
        say("redwood: PosterBoard: registration bookmark failed node %s error %s\n",
            pb_desc(self, node, sizeof(node)), pb_desc(error != NULL ? *error : local, detail, sizeof(detail)));
    }
    return result;
}

static id (*orig_poster_resolve_bookmark)(id, SEL, id, id, void *, id *);
static id
pb_resolve_bookmark(id self, SEL sel, id data, id relative, void *stale, id *error)
{
    id local = nil;
    id result = orig_poster_resolve_bookmark(self, sel, data, relative, stale, error != NULL ? error : &local);
    if (pb_registering_extension) {
        char node[500], detail[1000];
        say("redwood: PosterBoard: registration bookmark resolution %s node %s error %s\n",
            result != nil ? "accepted" : "failed", pb_desc(result, node, sizeof(node)),
            pb_desc(error != NULL ? *error : local, detail, sizeof(detail)));
    }
    return result;
}

static id (*orig_poster_bundle_info)(id, SEL);
static id
pb_bundle_info(id self, SEL sel)
{
    id result = orig_poster_bundle_info(self, sel);
    if (pb_registering_extension) {
        char info[1600];
        say("redwood: PosterBoard: registration bundle info %s\n", pb_desc(result, info, sizeof(info)));
    }
    return result;
}

static void
pb_register_gradient_with_metadata(id workspace)
{
    SEL selector = sel_registerName("registerApplicationDictionary:");
    if (!((BOOL (*)(id, SEL, SEL))objc_msgSend)(workspace, sel_registerName("respondsToSelector:"), selector))
        return;
    Class dictionaryClass = objc_getClass("NSDictionary");
    SEL read = sel_registerName("dictionaryWithContentsOfFile:");
    id appInfo = ((id (*)(Class, SEL, id))objc_msgSend)(dictionaryClass, read,
        (id)CFSTR("/Applications/GradientPoster.app/Info.plist"));
    id extensionInfo = ((id (*)(Class, SEL, id))objc_msgSend)(dictionaryClass, read,
        (id)CFSTR("/Applications/GradientPoster.app/Extensions/GradientPosterExtension.appex/Info.plist"));
    if (appInfo == nil || extensionInfo == nil) {
        say("redwood: PosterBoard: gradient installation metadata unavailable\n");
        return;
    }
    CFMutableDictionaryRef app = CFDictionaryCreateMutableCopy(NULL, 0, (CFDictionaryRef)appInfo);
    CFMutableDictionaryRef extension = CFDictionaryCreateMutableCopy(NULL, 0, (CFDictionaryRef)extensionInfo);
    CFMutableDictionaryRef plugins = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef appID = CFDictionaryGetValue(app, CFSTR("CFBundleIdentifier"));
    CFStringRef extensionID = CFDictionaryGetValue(extension, CFSTR("CFBundleIdentifier"));
    if (appID != NULL && extensionID != NULL) {
        CFDictionarySetValue(app, CFSTR("Path"), CFSTR("/Applications/GradientPoster.app"));
        CFDictionarySetValue(app, CFSTR("ApplicationType"), CFSTR("System"));
        CFDictionarySetValue(extension, CFSTR("Path"), CFSTR("/Applications/GradientPoster.app/Extensions/GradientPosterExtension.appex"));
        CFDictionarySetValue(extension, CFSTR("ApplicationType"), CFSTR("PluginKitPlugin"));
        CFDictionarySetValue(extension, CFSTR("PluginOwnerBundleID"), appID);
        CFDictionarySetValue(plugins, extensionID, extension);
        CFDictionarySetValue(app, CFSTR("_LSBundlePlugins"), plugins);
        pb_registering_extension = 1;
        BOOL accepted = ((BOOL (*)(id, SEL, id))objc_msgSend)(workspace, selector, (id)app);
        pb_registering_extension = 0;
        say("redwood: PosterBoard: gradient installation metadata registration %s\n", accepted ? "accepted" : "failed");
    }
    CFRelease(plugins);
    CFRelease(extension);
    CFRelease(app);
}

static id (*orig_poster_execute_queries)(Class, SEL, id);
static id
pb_execute_queries(Class self, SEL sel, id queries)
{
    static unsigned said;
    id result = orig_poster_execute_queries(self, sel, queries);
    if (__sync_fetch_and_add(&said, 1) < 12) {
        char q[800], answer[1200];
        for (unsigned long i = 0; i < pb_count(queries); i++) {
            id query = ((id (*)(id, SEL, unsigned long))objc_msgSend)(queries, sel_registerName("objectAtIndex:"), i);
            id point = ((id (*)(id, SEL))objc_msgSend)(query, sel_registerName("extensionPointIdentifier"));
            id predicate = ((id (*)(id, SEL))objc_msgSend)(query, sel_registerName("predicate"));
            say("redwood: PosterBoard: query point %s predicate %s\n", pb_desc(point, q, sizeof(q)), pb_desc(predicate, answer, sizeof(answer)));
        }
        static int inspected_all;
        if (__sync_bool_compare_and_swap(&inspected_all, 0, 1)) {
            Class workspaceClass = objc_getClass("LSApplicationWorkspace");
            SEL defaultWorkspace = sel_registerName("defaultWorkspace");
            if (workspaceClass != Nil && class_getClassMethod(workspaceClass, defaultWorkspace) != NULL) {
                id workspace = ((id (*)(Class, SEL))objc_msgSend)(workspaceClass, defaultWorkspace);
                SEL registerPlugin = sel_registerName("registerPlugin:");
                if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(workspace, sel_registerName("respondsToSelector:"), registerPlugin)) {
                    const char *path = "/Applications/GradientPoster.app/Extensions/GradientPosterExtension.appex";
                    id string = ((id (*)(Class, SEL, const char *))objc_msgSend)(objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), path);
                    id url = ((id (*)(Class, SEL, id))objc_msgSend)(objc_getClass("NSURL"), sel_registerName("fileURLWithPath:"), string);
                    pb_registering_extension = 1;
                    BOOL ok = ((BOOL (*)(id, SEL, id))objc_msgSend)(workspace, registerPlugin, url);
                    pb_registering_extension = 0;
                    say("redwood: PosterBoard: native gradient extension registration %s\n", ok ? "accepted" : "failed");
                    if (!ok) {
                        int (*registerWithInfo)(id, id) = (int (*)(id, id))dlsym(RTLD_DEFAULT, "_LSRegisterPluginWithInfo");
                        if (registerWithInfo != NULL) {
                            int status = registerWithInfo(url, nil);
                            say("redwood: PosterBoard: gradient plugin OSStatus %d\n", status);
                        } else {
                            say("redwood: PosterBoard: plugin status function unavailable\n");
                        }
                        int readable = access("/Applications/GradientPoster.app/Extensions/GradientPosterExtension.appex/Info.plist", R_OK);
                        say("redwood: PosterBoard: gradient plist readable %d errno %d\n", readable == 0, readable == 0 ? 0 : errno);
                    }
                }
                pb_register_gradient_with_metadata(workspace);
                const char *lists[] = { "allApplications", "installedPlugins" };
                for (unsigned list = 0; list < 2; list++) {
                    SEL selector = sel_registerName(lists[list]);
                    if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(workspace, sel_registerName("respondsToSelector:"), selector)) {
                        id entries = ((id (*)(id, SEL))objc_msgSend)(workspace, selector);
                        say("redwood: PosterBoard: LaunchServices %s count %lu\n", lists[list], pb_count(entries));
                        for (unsigned long i = 0; i < pb_count(entries) && i < 1000; i++) {
                            id entry = ((id (*)(id, SEL, unsigned long))objc_msgSend)(entries, sel_registerName("objectAtIndex:"), i);
                            SEL identifier = sel_registerName("bundleIdentifier");
                            if (((BOOL (*)(id, SEL, SEL))objc_msgSend)(entry, sel_registerName("respondsToSelector:"), identifier)) {
                                id bundle = ((id (*)(id, SEL))objc_msgSend)(entry, identifier);
                                const char *text = pb_desc(bundle, q, sizeof(q));
                                if (strstr(text, "Poster") != NULL || strstr(text, "poster") != NULL || strstr(text, "PaperBoard") != NULL) {
                                    say("redwood: PosterBoard: LaunchServices %s %s\n", lists[list], text);
                                }
                            }
                        }
                    }
                }
            }
            Class queryClass = objc_getClass("_EXQuery");
            SEL all = sel_registerName("allExtensionsQuery");
            if (queryClass != Nil && class_getClassMethod(queryClass, all) != NULL) {
                id allQuery = ((id (*)(Class, SEL))objc_msgSend)(queryClass, all);
                id array = ((id (*)(Class, SEL, id))objc_msgSend)(objc_getClass("NSArray"), sel_registerName("arrayWithObject:"), allQuery);
                id allResults = orig_poster_execute_queries(self, sel, array);
                say("redwood: PosterBoard: unfiltered ExtensionKit inventory %lu: %s\n", pb_count(allResults), pb_desc(allResults, answer, sizeof(answer)));
            }
        }
        say("redwood: PosterBoard: ExtensionKit queries %s -> %lu results: %s\n",
            pb_desc(queries, q, sizeof(q)), pb_count(result), pb_desc(result, answer, sizeof(answer)));
    }
    return result;
}

static void
posterboard_fixes(void)
{
    Method titleDefaults = class_getClassMethod(objc_getClass("PRPosterConfiguredProperties"), sel_registerName("defaultConfiguredPropertiesForRole:renderingConfiguration:"));
    if (titleDefaults != NULL)
        orig_poster_default_properties = (id (*)(Class, SEL, id, id))method_setImplementation(titleDefaults, (IMP)pb_default_properties);
    Method supportedRolesMethod = class_getClassMethod(objc_getClass("PBFPosterExtensionDataStore"), sel_registerName("supportedRoles"));
    if (supportedRolesMethod != NULL)
        orig_poster_supported_roles = (id (*)(Class, SEL))method_setImplementation(supportedRolesMethod, (IMP)pb_supported_roles);
    swizzle("PBFPosterRoleCoordinator", "updateCoordinatorWithChange:changeHandler:emitEvents:error:", (IMP)pb_coordinator_change, (IMP *)&orig_poster_coordinator_change);
    swizzle("PBFPosterRoleCoordinator", "finalizeChangesWithChangeHandler:outEvents:error:", (IMP)pb_finalize, (IMP *)&orig_poster_finalize);
    swizzle("_PBFPosterExtensionDataStoreSQLiteDatabaseImpl", "performChanges:error:", (IMP)pb_sql_changes, (IMP *)&orig_poster_sql_changes);
    swizzle("PFSQLiteDatabaseConnection", "executeQuery:error:", (IMP)pb_sql_query, (IMP *)&orig_poster_sql_query);
    swizzle("PBFPosterExtensionDataStore", "_stateLock_incomingPosterConfigurationForProviderIdentifier:posterDescriptorIdentifier:role:error:", (IMP)pb_incoming_native, (IMP *)&orig_poster_incoming_native);
    swizzle("PBFPosterExtensionDataStore", "_stateLock_executeDataStoreUpdateWithChanges:diffs:options:reason:context:error:", (IMP)pb_update_native, (IMP *)&orig_poster_update_native);
    swizzle("PRMutablePosterConfiguration", "copyContentsOfPath:error:", (IMP)pb_copy_contents, (IMP *)&orig_poster_copy_contents);
    swizzle("FSNode", "bookmarkDataRelativeToNode:error:", (IMP)pb_registration_bookmark, (IMP *)&orig_poster_bookmark);
    swizzle("FSNode", "initByResolvingBookmarkData:relativeToNode:bookmarkDataIsStale:error:", (IMP)pb_resolve_bookmark, (IMP *)&orig_poster_resolve_bookmark);
    swizzle("_LSBundleProvider", "infoDictionary", (IMP)pb_bundle_info, (IMP *)&orig_poster_bundle_info);
    swizzle("NSError", "initWithDomain:code:userInfo:", (IMP)pb_registration_error_init, (IMP *)&orig_registration_error_init);

    Class query = objc_getClass("_EXQueryController");
    Method execute = query != Nil ? class_getClassMethod(query, sel_registerName("executeQueries:")) : NULL;
    if (execute != NULL) {
        orig_poster_execute_queries = (id (*)(Class, SEL, id))method_setImplementation(execute, (IMP)pb_execute_queries);
    }
    say("redwood: PosterBoard: ExtensionKit query tracing %s\n", execute != NULL ? "installed" : "unavailable");

	Class helper = objc_getClass("PBFInitialPosterHelper");
	Method m = helper != Nil ? class_getClassMethod(helper,
	    sel_registerName("initialPosterConfigurationsWithModelCoordinatorProvider:role:")) : NULL;

	if (m != NULL) {
		orig_initial_posters = (id (*)(id, SEL, id, id))method_setImplementation(m, (IMP)pb_initial_posters);
	}
	swizzle("PBFPosterRoleCoordinator", "setupRoleIfNecessaryWithStorage:", (IMP)pb_setup_role, (IMP *)&orig_setup_role);
	{
		Class migrator = objc_getClass("PBFPosterExtensionDataStoreMigrator");
		Method mm = migrator != Nil ? class_getClassMethod(migrator,
		    sel_registerName("migrateDataStoreAtBaseURL:fromVersion:toVersion:cleanupAfterMigrationSucceeds:error:")) : NULL;
		if (mm != NULL) {
			orig_migrate = (BOOL (*)(Class, SEL, id, unsigned long, unsigned long, BOOL, id *))
			    method_setImplementation(mm, (IMP)pb_migrate);
		}
	}
	swizzle("PBFPosterExtensionDataStoreXPCServiceGlue",
	    "buildDataStoreForURL:runtimeAssertionProvider:applicationStateMonitor:observer:wasMigrationJustPerformed:error:",
	    (IMP)pb_build, (IMP *)&orig_build);
	swizzle("NSFileManager", "createDirectoryAtURL:withIntermediateDirectories:attributes:error:", (IMP)pb_fm_create,
	    (IMP *)&orig_fm_create);
	swizzle("NSFileManager", "copyItemAtURL:toURL:error:", (IMP)pb_fm_copy, (IMP *)&orig_fm_copy);
	swizzle("NSData", "writeToURL:options:error:", (IMP)pb_data_write, (IMP *)&orig_data_write);
	swizzle("NSURL", "pbf_setPurgable:error:", (IMP)pb_set_purgable, (IMP *)&orig_set_purgable);
	{
		Method me = class_getClassMethod(objc_getClass("NSError"), sel_registerName("pbf_generalErrorWithCode:userInfo:"));
		if (me != NULL) {
			orig_general_error = (id (*)(id, SEL, long, id))method_setImplementation(me, (IMP)pb_general_error);
		} else {
			say("redwood: +[NSError pbf_generalErrorWithCode:userInfo:]: not found\n");
		}
		me = class_getClassMethod(objc_getClass("NSError"), sel_registerName("errorWithDomain:code:userInfo:"));
		if (me != NULL) {
			orig_error_with_domain = (id (*)(id, SEL, id, long, id))method_setImplementation(me, (IMP)pb_error_with_domain);
		}
	}
	swizzle("NSURL", "pbf_recursivelyUpdateResourceValues:error:", (IMP)pb_url_update, (IMP *)&orig_url_update);
	swizzle("NSURL", "pbf_URLIsReachableAndConformToResourceValues:error:", (IMP)pb_url_conform,
	    (IMP *)&orig_url_conform);
	swizzle("PBFPosterExtensionDataStore", "_stateLock_setupRoleCoordinatorsIfNeeded", (IMP)pb_setup_coordinators,
	    (IMP *)&orig_setup_coordinators);
	swizzle("PBFPosterExtensionDataStore", "pbf_posterExtensionStoreCoordinatorForProviderIdentifier:error:",
	    (IMP)pb_store_for_provider, (IMP *)&orig_store_for_provider);
	swizzle("PBFPosterExtensionStoreCoordinator", "staticDescriptorStoreCoordinatorForIdentifier:",
	    (IMP)pb_static_descriptors, (IMP *)&orig_static_descriptors);
	swizzle("PBFPosterExtensionStoreCoordinator", "createDynamicDescriptorStoreCoordinatorForIdentifier:role:error:",
	    (IMP)pb_create_dynamic, (IMP *)&orig_create_dynamic);
	swizzle("PBFWallpaperKitBridge", "shouldInstallHeroPosterAsDefaultLockScreenWallpaper", (IMP)pb_hero,
	    (IMP *)&orig_hero);
	if (helper != Nil) {
		Method mi = class_getClassMethod(helper, sel_registerName("incomingPosterConfigurationForRole:templatePath:userInfo:"));
		if (mi != NULL) {
			orig_incoming = (id (*)(id, SEL, id, id, id))method_setImplementation(mi, (IMP)pb_incoming);
		}
	}
	say("redwood: PosterBoard: watching its poster setup (%s)\n", m != NULL ? "all three steps" : "no initial poster helper");
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_sigaction = pb_trap;
		sa.sa_flags = SA_SIGINFO;
		sigaction(SIGTRAP, &sa, NULL);
		sigaction(SIGILL, &sa, NULL);
		sigaction(SIGABRT, &sa, NULL);
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
	}
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL, pb_report);
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL, pb_report);
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 90 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), NULL, pb_report);
}

/*
 * A SAMPLE of this process's threads, for the screen that stops changing (runs glass15-18: once the
 * lock screen poster exists the GPU goes idle and no frame is presented again, with nothing in the
 * log to say who waits for what). At 90 and 150 s after the process starts every thread is
 * suspended in turn and its pc, lr and frame chain said as image+offset (dladdr's names are
 * mostly "<redacted>" in the shared cache; the offsets are symbolized off the phone). Boot-arg
 * sample=0: not. The frame walk stays inside the thread's own stack.
 */
#include <mach/mach.h>

static void
rw_say_pc(const char *tag, unsigned t, const char *what, uintptr_t pc)
{
	Dl_info dl;

	pc &= 0x0000007fffffffffUL;
	if (pc != 0 && dladdr((void *)pc, &dl) && dl.dli_fname != NULL) {
		const char *image = strrchr(dl.dli_fname, '/') ? strrchr(dl.dli_fname, '/') + 1 : dl.dli_fname;
		say("redwood: sample %s t%u %s %s+%#lx %s\n", tag, t, what, image, (unsigned long)(pc - (uintptr_t)dl.dli_fbase),
		    dl.dli_sname != NULL ? dl.dli_sname : "?");
	} else {
		say("redwood: sample %s t%u %s %#lx\n", tag, t, what, (unsigned long)pc);
	}
}

static void
rw_sample(void *context)
{
	const char *tag = context;
	thread_act_array_t threads;
	mach_msg_type_number_t count = 0;
	thread_t self = mach_thread_self();
	/* copied while a thread is suspended, said after it runs again: one that is stopped while it
	 * holds malloc's or dyld's lock would otherwise hang this one in say() or dladdr() */
	static uintptr_t pcs[18];

	if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
		return;
	}
	say("redwood: sample %s: %u threads\n", tag, count);
	for (unsigned t = 0; t < count; t++) {
		arm_thread_state64_t st;
		mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
		char tname[64] = "";
		pthread_t pt = pthread_from_mach_thread_np(threads[t]);
		uintptr_t lo = 0, hi = 0;
		unsigned got = 0;

		if (threads[t] == self) {
			continue;
		}
		if (pt != NULL) {
			pthread_getname_np(pt, tname, sizeof(tname));
			hi = (uintptr_t)pthread_get_stackaddr_np(pt);
			lo = hi - pthread_get_stacksize_np(pt);
		}
		if (thread_suspend(threads[t]) != KERN_SUCCESS) {
			continue;
		}
		if (thread_get_state(threads[t], ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
			uintptr_t fp = (uintptr_t)__darwin_arm_thread_state64_get_fp(st);

			pcs[got++] = (uintptr_t)__darwin_arm_thread_state64_get_pc(st);
			pcs[got++] = (uintptr_t)__darwin_arm_thread_state64_get_lr(st);
			while (got < 18 && fp >= lo && fp + 16 <= hi && (fp & 7) == 0) {
				uintptr_t *frame = (uintptr_t *)fp;
				pcs[got++] = frame[1];
				if (frame[0] <= fp) {
					break;
				}
				fp = frame[0];
			}
		}
		thread_resume(threads[t]);
		say("redwood: sample %s t%u '%s'\n", tag, t, tname);
		for (unsigned i = 0; i < got; i++) {
			rw_say_pc(tag, t, i == 0 ? "pc" : i == 1 ? "lr" : "fr", pcs[i]);
		}
	}
	for (unsigned t = 0; t < count; t++) {
		mach_port_deallocate(mach_task_self(), threads[t]);
	}
	vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof(threads[0]));
	mach_port_deallocate(mach_task_self(), self);
}

static void
rw_schedule_samples(const char *tag)
{
	char args[1024] = { 0 };
	size_t size = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "sample=0") != NULL) {
		return;
	}
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 90 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), (void *)tag,
	    rw_sample);
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), (void *)tag,
	    rw_sample);
}


/* ---- the OpenGL ES renderer -------------------------------------------------------------------- */

#define REDWOOD_GL "/redwood/lib/libredwood-gl.dylib"

/*
 * The same switch inject.c has: with /redwood/lib/libredwood-gl.dylib in the image, dlopen it so that
 * eagl.c puts itself behind EAGLContext and the window server gets the OpenGL ES renderer.  Boot-arg
 * gl=1 asks for it even without a GPU (Mesa softpipe: slow, but it draws the vibrancy/glass filters
 * that CoreAnimation's own software renderer skips); gl=0 refuses it; no argument means only with the
 * GPU service present.
 */
static void
opengl_es26(void)
{
	char arguments[1024] = "";
	size_t size = sizeof(arguments) - 1;
	const char *argument;
	io_service_t gpu;
	void *library;
	int wanted = -1, told = 0;

	if (access(REDWOOD_GL, R_OK) != 0)
		return;
	if (sysctlbyname("kern.bootargs", arguments, &size, NULL, 0) == 0) {
		for (argument = arguments; (argument = strstr(argument, "gl=")) != NULL; argument += 3) {
			if (argument != arguments && argument[-1] != ' ')
				continue;
			wanted = argument[3] != '0';
			told = 1;
		}
	}
	if (wanted < 0) {
		gpu = IOServiceGetMatchingService(0, IOServiceMatching("RedwoodGPU"));
		wanted = gpu != 0;
		if (gpu != 0)
			IOObjectRelease(gpu);
	}
	if (!wanted) {
		say("redwood26: %s: CoreAnimation keeps its software renderer\n", told ? "gl=0" : "no GPU to draw with");
		return;
	}
	/* the 26-line OSMesa is built with the AIR frontend and references rwa_*; load the AIR library
	 * first, globally, so the OpenGL library binds without a flat-namespace failure. */
	dlopen("/redwood/lib/libredwoodair.dylib", RTLD_NOW | RTLD_GLOBAL);
	if ((library = dlopen(REDWOOD_GL, RTLD_NOW)) == NULL) {
		say("redwood26: %s: %s\n", REDWOOD_GL, dlerror());
		return;
	}
	say("redwood26: gl=%d: CoreAnimation gets the OpenGL ES renderer (%s)\n", wanted, REDWOOD_GL);
}


/* ---- Liquid Glass off: the accessibility switches, set before the UI reads them ------------------------- */

/*
 * The lock-screen clock of iOS 26 is drawn through the glass pipeline; with no Metal renderer the digits
 * never reach the screen.  iOS itself has a switch for that: with Reduce Transparency on, Liquid Glass
 * effects fall back to ordinary content.  Set it (and BaseBoardUI's vibrancy switch) from the process start,
 * before SpringBoard and backboardd read the accessibility cache.  Boot-arg clocka11y=1.
 */
static void
clock_a11y(void)
{
	char args[1024] = { 0 };
	size_t size = sizeof(args) - 1;

	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || strstr(args, "clocka11y=1") == NULL)
		return;
	CFPreferencesSetAppValue(CFSTR("ReduceTransparencyEnabled"), kCFBooleanTrue, CFSTR("com.apple.Accessibility"));
	CFPreferencesSetAppValue(CFSTR("BSUIVibrancyEffectViewDisabled"), kCFBooleanTrue, kCFPreferencesCurrentApplication);
	CFPreferencesSetAppValue(CFSTR("SBFEnableLiquidGlass"), kCFBooleanFalse, kCFPreferencesCurrentApplication);
	CFPreferencesAppSynchronize(CFSTR("com.apple.Accessibility"));
	notify_post("com.apple.accessibility.cache.reduce.transparency");
	notify_post("com.apple.accessibility.reduce.transparency.changed");
	say("redwood26: clocka11y: reduce transparency + vibrancy off, liquid glass off\n");
}


/* ---- the Metal switch: name the plug-in on the accelerator service (kernel: metal=1) ---------------- */

extern kern_return_t rw_IORegistryEntrySetCFProperties(io_registry_entry_t entry, CFTypeRef properties)
    __asm("_IORegistryEntrySetCFProperties");   /* iOS marks the IOKit name unavailable; the symbol is there */

static void
metal_switch_26(const char *name)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *list = NULL;
	bool wanted = false;

	if (name == NULL || sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) {
		return;
	}
	for (const char *a = args; (a = strstr(a, "metal_procs=")) != NULL; a += 12) {
		if (a != args && a[-1] != ' ') {
			continue;
		}
		list = a + 12;
		break;
	}
	if (list == NULL) {
		return;
	}
	for (const char *p = list; *p != '\0' && *p != ' ' && !wanted; ) {
		size_t len = strcspn(p, " ");

		if (len == strlen(name) && strncmp(p, name, len) == 0) {
			wanted = true;
		}
		p += len;
		while (*p == ' ') {
			p++;
		}
	}
	if (!wanted) {
		return;
	}
	{
		CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 2, &kCFTypeDictionaryKeyCallBacks,
		    &kCFTypeDictionaryValueCallBacks);
		io_iterator_t iter = 0;
		io_service_t service;
		int set = 0;

		if (props == NULL || IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAcceleratorES"),
		    &iter) != KERN_SUCCESS) {
			return;
		}
		CFDictionarySetValue(props, CFSTR("MetalPluginName"), CFSTR("RedwoodMetal"));
		CFDictionarySetValue(props, CFSTR("MetalPluginClassName"), CFSTR("NativeAdrenoMetalDevice"));
		while ((service = IOIteratorNext(iter)) != 0) {
			kern_return_t kr = rw_IORegistryEntrySetCFProperties(service, props);

			set += (kr == KERN_SUCCESS) ? 1 : 0;
			IOObjectRelease(service);
		}
		IOObjectRelease(iter);
		CFRelease(props);
		say("redwood26: metal_procs: Metal plugin named on %d accelerator service(s)\n", set);
	}
}

__attribute__((constructor))
static void
redwood26_init(void)
{
	const char *name = getprogname();

	say("redwood: loaded into %s (pid %d)\n", name ? name : "?", getpid());
	metal_switch_26(name);
	caenv_glass_allowed = name != NULL && (strcmp(name, "backboardd") == 0 || strcmp(name, "SpringBoard") == 0);
	caenv_from_bootargs();
	if (name != NULL && strcmp(name, "backboardd") == 0) {
		opengl_es26();
		clock_a11y();
		input_start();
		rw_schedule_samples("backboardd");
	} else if (name != NULL && strcmp(name, "SpringBoard") == 0) {
		clock_a11y();
		springboard_fixes();
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, sb_inspect_clock);
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_SEC), dispatch_get_main_queue(), NULL, sb_inspect_clock);

		rw_schedule_samples("SpringBoard");
	} else if (name != NULL && strcmp(name, "PosterBoard") == 0) {
		posterboard_fixes();
 } else if (name != NULL && strcmp(name, "GradientPosterExtension") == 0) {
        swizzle("NSFileManager", "createDirectoryAtURL:withIntermediateDirectories:attributes:error:", (IMP)pb_fm_create, (IMP *)&orig_fm_create);
        swizzle("NSFileManager", "copyItemAtURL:toURL:error:", (IMP)pb_fm_copy, (IMP *)&orig_fm_copy);
        swizzle("NSData", "writeToURL:options:error:", (IMP)pb_data_write, (IMP *)&orig_data_write);
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_global_queue(0, 0),
            (void *)"GradientPosterExtension-1s", rw_sample);
        dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0),
            (void *)"GradientPosterExtension-2s", rw_sample);
    }
}
