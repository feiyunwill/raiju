#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_REPORT_SECONDS 30
#define RAIJU_LISTEN_PORT 6801

#define SCE_SYSTEM_SERVICE_EVENT_ON_RESUME 0x10000000u
#define SCE_SYSTEM_SERVICE_EVENT_BEFORE_SLEEP 0x10000001u

/**
 * What the system service reports: a type plus an opaque body, as the payload
 * SDK's own clients declare it.
 */
typedef struct {
	uint32_t eventType;
	uint8_t data[60];
} sce_system_service_event;

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

#ifndef RAIJU_HOLD_SECONDS
#define RAIJU_HOLD_SECONDS 7200
#endif

#ifndef RAIJU_TICK_SECONDS
#define RAIJU_TICK_SECONDS 10
#endif

#ifndef RAIJU_RELEASE_SECONDS
#define RAIJU_RELEASE_SECONDS 3600
#endif

#ifndef RAIJU_RELEASE_ONLY
#define RAIJU_RELEASE_ONLY 0
#endif

#ifndef RAIJU_PEER_HOST
#define RAIJU_PEER_HOST "192.168.100.74"
#endif

#ifndef RAIJU_PEER_PORT
#define RAIJU_PEER_PORT 8000
#endif

int sceNotificationSend(int userId, bool isLogged, const char *payload);
int sceSystemServicePowerTick(void) __attribute__((weak));
int sceSystemServiceReceiveEvent(sce_system_service_event *event) __attribute__((weak));
int sceShellCoreUtilResetAutoPowerDownTimer(void) __attribute__((weak));
int sceShellCoreUtilSignalUserInput(void) __attribute__((weak));

static const char toast_tmpl[] =
	"{"
	"  \"rawData\": {"
	"    \"viewTemplateType\": \"InteractiveToastTemplateB\","
	"    \"channelType\": \"Downloads\","
	"    \"useCaseId\": \"IDC\","
	"    \"toastOverwriteType\": \"No\","
	"    \"isImmediate\": true,"
	"    \"priority\": 100,"
	"    \"viewData\": {"
	"      \"icon\": {\"type\": \"Predefined\", \"parameters\": {\"icon\": \"download\"}},"
	"      \"message\": {\"body\": \"%s\"},"
	"      \"subMessage\": {\"body\": \"%s\"}"
	"    },"
	"    \"platformViews\": {"
	"      \"previewDisabled\": {"
	"        \"viewData\": {"
	"          \"icon\": {\"type\": \"Predefined\", \"parameters\": {\"icon\": \"download\"}},"
	"          \"message\": {\"body\": \"%s\"}"
	"        }"
	"      }"
	"    }"
	"  },"
	"  \"localNotificationId\": \"1\""
	"}";

static int tick_count;
static int tick_failures;
static int reset_count;
static int reset_failures;
static int signal_count;
static int signal_failures;
static int sleep_events;
static int resume_events;
static int peer_fd = -1;
static int listen_fd = -1;

/** Raises a toast and reports what the notification service answered. */
static void notify(const char *title, const char *detail) {
	char payload[2048];
	int result;

	if (snprintf(payload, sizeof(payload), toast_tmpl, title, detail, title) < 0) {
		return;
	}
	result = sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true,
				     payload);
	printf("notify: %d\n", result);
	fflush(stdout);
}

/** Seconds of wall clock since started, from a monotonic source. */
static double monotonic_seconds(void) {
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0.0;
	}

	return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

/** Seconds since the epoch, so console output can be lined up with the system's own clock. */
static double realtime_seconds(void) {
	struct timespec now;

	if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
		return 0.0;
	}

	return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

/** Opens a TCP connection to a host and port, or -1 when it cannot. */
static int connect_to(const char *host, int port) {
	struct sockaddr_in address;
	int fd;

	memset(&address, 0, sizeof address);
	address.sin_family = AF_INET;
	address.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
		return -1;
	}

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	if (connect(fd, (struct sockaddr *)&address, sizeof address) != 0) {
		close(fd);
		return -1;
	}

	return fd;
}

/** Binds a loopback listener the way the engine's RPC socket will be bound. */
static int listen_on_loopback(int port) {
	struct sockaddr_in address;
	int fd;
	int reuse = 1;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

	memset(&address, 0, sizeof address);
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons((uint16_t)port);
	if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
	    listen(fd, 4) != 0) {
		close(fd);
		return -1;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

	return fd;
}

/**
 * Reports whether a socket the payload opened before sleeping still works: the
 * listener by connecting to it, the established connection by writing to it.
 */
static void report_sockets(void) {
	if (listen_fd >= 0) {
		struct sockaddr_in address;
		int probe = connect_to("127.0.0.1", RAIJU_LISTEN_PORT);

		if (probe >= 0) {
			socklen_t length = sizeof address;
			int accepted = accept(listen_fd, (struct sockaddr *)&address,
					      &length);

			printf("socket: listener accepted=%s\n",
			       accepted >= 0 ? "yes" : "no");
			if (accepted >= 0) {
				close(accepted);
			}
			close(probe);
		} else {
			printf("socket: listener refused a fresh connection\n");
		}
	} else {
		printf("socket: listener was never opened\n");
	}

	if (peer_fd >= 0) {
		static const char request[] = "HEAD / HTTP/1.0\r\n\r\n";
		char buffer[64];
		ssize_t put = write(peer_fd, request, sizeof request - 1);
		ssize_t got = put > 0 ? read(peer_fd, buffer, sizeof buffer) : -1;

		printf("socket: established connection write=%s read=%s\n",
		       put > 0 ? "ok" : "broken", got > 0 ? "ok" : "broken");
		if (got <= 0) {
			close(peer_fd);
			peer_fd = -1;
		}
	} else {
		printf("socket: no established connection to test\n");
	}
	fflush(stdout);
}

/** Names the events the system service sends around sleep and resume. */
static const char *event_name(uint32_t type) {
	switch (type) {
	case SCE_SYSTEM_SERVICE_EVENT_ON_RESUME:
		return "on_resume";
	case SCE_SYSTEM_SERVICE_EVENT_BEFORE_SLEEP:
		return "before_sleep";
	default:
		return "other";
	}
}

/**
 * Waits for system service events for the life of the payload, so a sleep and
 * the resume after it are observed rather than inferred from the clock.
 */
static void *event_thread(void *argument) {
	(void)argument;

	if (sceSystemServiceReceiveEvent == NULL) {
		printf("event: sceSystemServiceReceiveEvent is not in this firmware's library\n");
		fflush(stdout);
		return NULL;
	}

	for (;;) {
		sce_system_service_event event;
		int result;

		memset(&event, 0, sizeof event);
		result = sceSystemServiceReceiveEvent(&event);
		if (result < 0) {
			printf("event: receive rc=0x%08x\n", (unsigned)result);
			fflush(stdout);
			usleep(500000);
			continue;
		}

		if (event.eventType == SCE_SYSTEM_SERVICE_EVENT_ON_RESUME) {
			resume_events++;
		} else if (event.eventType == SCE_SYSTEM_SERVICE_EVENT_BEFORE_SLEEP) {
			sleep_events++;
		}

		printf("event: %s (0x%08x) at realtime %.0f\n",
		       event_name(event.eventType), event.eventType,
		       realtime_seconds());
		fflush(stdout);

		if (event.eventType == SCE_SYSTEM_SERVICE_EVENT_ON_RESUME) {
			notify("Raiju power", "resumed, checking sockets");
			report_sockets();
		}
	}

	return NULL;
}

/** Asks the system to leave the idle timer alone, and says what each call answered. */
static void hold_awake(void) {
	int result;

	if (sceSystemServicePowerTick != NULL) {
		result = sceSystemServicePowerTick();
		tick_count++;
		if (result < 0) {
			tick_failures++;
			printf("tick: sceSystemServicePowerTick rc=0x%08x\n",
			       (unsigned)result);
		}
	}

	if (sceShellCoreUtilResetAutoPowerDownTimer != NULL) {
		result = sceShellCoreUtilResetAutoPowerDownTimer();
		reset_count++;
		if (result < 0) {
			reset_failures++;
			printf("tick: sceShellCoreUtilResetAutoPowerDownTimer rc=0x%08x\n",
			       (unsigned)result);
		}
	}

	if (sceShellCoreUtilSignalUserInput != NULL) {
		result = sceShellCoreUtilSignalUserInput();
		signal_count++;
		if (result < 0) {
			signal_failures++;
			printf("tick: sceShellCoreUtilSignalUserInput rc=0x%08x\n",
			       (unsigned)result);
		}
	}

	printf("tick: %d power ticks, %d timer resets, %d user-input signals\n",
	       tick_count, reset_count, signal_count);
	fflush(stdout);
}

/** Builds the sockets whose survival after a rest mode is the question. */
static void open_sockets(void) {
	listen_fd = listen_on_loopback(RAIJU_LISTEN_PORT);
	printf("socket: listener on 127.0.0.1:%d is %s\n", RAIJU_LISTEN_PORT,
	       listen_fd >= 0 ? "up" : "unavailable");

	peer_fd = connect_to(RAIJU_PEER_HOST, RAIJU_PEER_PORT);
	printf("socket: connection to %s:%d is %s\n", RAIJU_PEER_HOST,
	       RAIJU_PEER_PORT, peer_fd >= 0 ? "up" : "unavailable");
	fflush(stdout);
}

/**
 * Holds the idle timer off for RAIJU_HOLD_SECONDS, releases it, and keeps
 * reporting for RAIJU_RELEASE_SECONDS so the console can sleep and be woken.
 */
int main(void) {
	pthread_t events;
	double started;
	double next_tick = 0.0;
	double next_report = 0.0;
	char title[64];
	char detail[128];

	printf("raiju power spike %s, firmware %08x\n", RAIJU_VERSION,
	       kernel_get_fw_version());
	printf("libs: powertick=%s receivEvent=%s resetTimer=%s signalInput=%s\n",
	       sceSystemServicePowerTick ? "yes" : "no",
	       sceSystemServiceReceiveEvent ? "yes" : "no",
	       sceShellCoreUtilResetAutoPowerDownTimer ? "yes" : "no",
	       sceShellCoreUtilSignalUserInput ? "yes" : "no");
	printf("plan: %s for %d s, then release for %d s, ticking every %d s\n",
	       RAIJU_RELEASE_ONLY ? "no keep-awake at all" : "keep awake",
	       RAIJU_HOLD_SECONDS, RAIJU_RELEASE_SECONDS, RAIJU_TICK_SECONDS);
	fflush(stdout);

	open_sockets();
	pthread_create(&events, NULL, event_thread, NULL);

	started = monotonic_seconds();
	notify("Raiju power", RAIJU_RELEASE_ONLY ? "control run, no keep-awake" : "holding keep-awake");

	for (;;) {
		double elapsed = monotonic_seconds() - started;
		bool holding = !RAIJU_RELEASE_ONLY && elapsed < (double)RAIJU_HOLD_SECONDS;

		if (holding && elapsed >= next_tick) {
			hold_awake();
			next_tick = elapsed + (double)RAIJU_TICK_SECONDS;
		}

		if (elapsed >= next_report) {
			printf("report: %.0f s elapsed, %s, sleeps=%d resumes=%d\n",
			       elapsed, holding ? "holding" : "released",
			       sleep_events, resume_events);
			fflush(stdout);
			next_report = elapsed + (double)RAIJU_REPORT_SECONDS;
		}

		if (elapsed >= (double)(RAIJU_HOLD_SECONDS + RAIJU_RELEASE_SECONDS)) {
			break;
		}
		usleep(200000);
	}

	snprintf(title, sizeof(title), "Raiju power %s", RAIJU_VERSION);
	snprintf(detail, sizeof(detail), "ticks %d, sleeps %d, resumes %d",
		 tick_count, sleep_events, resume_events);
	notify(title, detail);

	printf("done: %d ticks (%d failed), %d resets (%d failed), %d signals (%d failed)\n",
	       tick_count, tick_failures, reset_count, reset_failures, signal_count,
	       signal_failures);
	printf("done: sleeps=%d resumes=%d\n", sleep_events, resume_events);
	fflush(stdout);

	payload_exit(0);

	return 0;
}
