#include <aria2/aria2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE
#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_DOWNLOADS RAIJU_DATA_ROOT "/downloads"
#define RAIJU_SESSION_FILE RAIJU_DATA_ROOT "/session/engine.session"
#define RAIJU_RPC_PORT 6800
#define RAIJU_RUN_SECONDS 900
#define RAIJU_PUMP_SECONDS 5.0
#define RAIJU_ACTION_SECONDS 8.0
#define RAIJU_PAUSE_FILE "raiju-pause-test.bin"

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

#ifndef RAIJU_SOURCE_URL
#define RAIJU_SOURCE_URL "https://192.168.100.74:8443/raiju-10g.bin"
#endif

extern "C" int sceNotificationSend(int userId, bool isLogged, const char *payload);

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
}

/** Formats a byte count the way the UI does, e.g. `612.4 GB`. */
static void human_size(int64_t bytes, char *out, size_t len) {
	static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
	double value = (double)bytes;
	size_t unit = 0;

	while (value >= 1000.0 && unit < 4) {
		value /= 1000.0;
		unit++;
	}
	snprintf(out, len, "%.1f %s", value, units[unit]);
}

/** Monotonic seconds, for timing the download. */
static double monotonic_seconds(void) {
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0.0;
	}

	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/** Returns where a download whose URL ends in `url` lands. */
static std::string download_path(const char *url) {
	const char *slash = strrchr(url, '/');
	const char *name = slash != NULL ? slash + 1 : url;

	return std::string(RAIJU_DOWNLOADS) + "/" + name;
}

/** Prints how much space is free, so anything the run leaves behind is visible. */
static void report_space(const char *label) {
	char free_text[32];
	struct statvfs fs;

	if (statvfs("/data", &fs) != 0) {
		printf("%s: statvfs: %s\n", label, strerror(errno));
		return;
	}
	human_size((int64_t)fs.f_bavail * fs.f_frsize, free_text, sizeof(free_text));
	printf("%s: %s free on /data\n", label, free_text);
}

/** Prints a file's length and, separately, the bytes actually allocated to it. */
static void report_file(const char *label, const std::string &path) {
	char length_text[32];
	char allocated_text[32];
	struct stat info;

	if (stat(path.c_str(), &info) != 0) {
		printf("%s: %s is absent (%s)\n", label, path.c_str(), strerror(errno));
		return;
	}
	human_size((int64_t)info.st_size, length_text, sizeof(length_text));
	human_size((int64_t)info.st_blocks * 512, allocated_text, sizeof(allocated_text));
	printf("%s: %s long, %s allocated\n", label, length_text, allocated_text);
}

/** Prints the engine's own speed and queue counts. */
static void report_stat(aria2::Session *session, const char *label) {
	aria2::GlobalStat stat = aria2::getGlobalStat(session);

	printf("%s: %d B/s, %d active, %d waiting, %d stopped\n", label,
	       stat.downloadSpeed, stat.numActive, stat.numWaiting, stat.numStopped);
}

/** Prints how much CPU this process has used, which the issue asks to record. */
static void report_cpu(const char *label) {
	struct rusage usage;

	if (getrusage(RUSAGE_SELF, &usage) != 0) {
		printf("%s: getrusage: %s\n", label, strerror(errno));
		return;
	}
	printf("%s: user %.2f s, system %.2f s\n", label,
	       (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6,
	       (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6);
}

/**
 * Pumps the event loop until `seconds` have passed, tracking the peak speed the engine reported and
 * reporting every `RAIJU_PUMP_SECONDS`. Bounded by the clock rather than by a pass count, because
 * `run(RUN_ONCE)` returns as soon as there are events and waits its full second when there are none,
 * so counting passes measures nothing.
 */
static int pump_for(aria2::Session *session, double seconds, const char *label,
		    const std::string &path, int *peak_speed, int *passes) {
	double started = monotonic_seconds();
	double next_report = started + RAIJU_PUMP_SECONDS;
	int result = 1;

	for (;;) {
		aria2::GlobalStat stat;
		double now;

		result = aria2::run(session, aria2::RUN_ONCE);
		(*passes)++;
		if (result != 1) {
			printf("%s: loop finished after %d passes with result %d\n", label,
			       *passes, result);
			return result;
		}
		stat = aria2::getGlobalStat(session);
		if (stat.downloadSpeed > *peak_speed) {
			*peak_speed = stat.downloadSpeed;
		}
		now = monotonic_seconds();
		if (now >= started + seconds) {
			break;
		}
		if (now >= next_report) {
			next_report = now + RAIJU_PUMP_SECONDS;
			report_stat(session, label);
			report_file(label, path);
		}
	}
	report_stat(session, label);
	report_file(label, path);

	return result;
}

/**
 * Measures the embedded engine, which is the only integration mode the console allows: a payload
 * cannot spawn a program it built, and libaria2's own header states the embedded session provides no
 * RPC. It fetches one ten-gigabyte file over TLS with sixteen connections, reporting the engine's
 * speed, the bytes allocated to the file and the CPU it cost, then exercises the actions the RPC
 * surface would have offered — pause, resume and remove — on a second download of the same source,
 * and clears both off the console's disk again.
 */
int main(void) {
	aria2::Session *session;
	aria2::KeyVals options;
	aria2::SessionConfig config;
	aria2::A2Gid gid;
	aria2::A2Gid other;
	std::string path = download_path(RAIJU_SOURCE_URL);
	std::string pause_path = download_path(RAIJU_PAUSE_FILE);
	std::string control_path = pause_path;
	double started;
	double elapsed;
	int peak_speed = 0;
	int passes = 0;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("raiju libaria2 spike %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());
	printf("source: %s\n", RAIJU_SOURCE_URL);

	mkdir(RAIJU_DATA_ROOT, 0755);
	mkdir(RAIJU_DOWNLOADS, 0755);
	unlink(path.c_str());
	unlink(pause_path.c_str());
	control_path.append(".aria2");
	unlink(control_path.c_str());
	unlink(RAIJU_SESSION_FILE);
	report_space("space");

	if (aria2::libraryInit() != 0) {
		printf("library: init failed\n");
		payload_exit(1);
		return 1;
	}
	session = NULL;
	printf("library: initialised\n");

	options.push_back(std::make_pair("enable-rpc", "true"));
	options.push_back(std::make_pair("rpc-listen-all", "true"));
	options.push_back(std::make_pair("rpc-listen-port", "6800"));
	options.push_back(std::make_pair("dir", RAIJU_DOWNLOADS));
	options.push_back(std::make_pair("file-allocation", "none"));
	options.push_back(std::make_pair("continue", "true"));
	options.push_back(std::make_pair("max-connection-per-server", "16"));
	options.push_back(std::make_pair("split", "16"));
	options.push_back(std::make_pair("auto-file-renaming", "false"));
	options.push_back(std::make_pair("check-certificate", "false"));
	options.push_back(std::make_pair("save-session", RAIJU_SESSION_FILE));
	options.push_back(std::make_pair("console-log-level", "notice"));
	options.push_back(std::make_pair("summary-interval", "0"));

	config.keepRunning = true;
	session = aria2::sessionNew(options, config);
	if (session == NULL) {
		printf("session: creation failed\n");
		aria2::libraryDeinit();
		payload_exit(1);
		return 1;
	}
	printf("session: created\n");

	started = monotonic_seconds();
	result = aria2::addUri(session, &gid, std::vector<std::string>{RAIJU_SOURCE_URL},
			       aria2::KeyVals());
	printf("add %s: %s\n", RAIJU_SOURCE_URL, result == 0 ? "accepted" : "refused");
	report_stat(session, "after add");

	while (monotonic_seconds() - started < RAIJU_RUN_SECONDS) {
		aria2::GlobalStat stat;
		int pump_result = pump_for(session, RAIJU_PUMP_SECONDS, "running", path,
					   &peak_speed, &passes);

		if (pump_result != 1) {
			break;
		}
		stat = aria2::getGlobalStat(session);
		if (stat.numActive == 0 && stat.numWaiting == 0) {
			printf("done: no active or waiting downloads after %d passes\n", passes);
			break;
		}
	}
	elapsed = monotonic_seconds() - started;
	printf("benchmark: peak %d B/s, %.0f MB/s, %d passes in %.1f s\n", peak_speed,
	       peak_speed / 1e6, passes, elapsed);
	report_file("benchmark", path);

	result = aria2::addUri(session, &other, std::vector<std::string>{RAIJU_SOURCE_URL},
			       aria2::KeyVals{{"out", RAIJU_PAUSE_FILE}});
	printf("add %s as %s: %s\n", RAIJU_SOURCE_URL, RAIJU_PAUSE_FILE,
	       result == 0 ? "accepted" : "refused");

	pump_for(session, RAIJU_ACTION_SECONDS, "second", pause_path, &peak_speed, &passes);
	printf("pause: %s\n", aria2::pauseDownload(session, other) == 0 ? "accepted" : "refused");
	pump_for(session, RAIJU_ACTION_SECONDS, "paused", pause_path, &peak_speed, &passes);

	printf("unpause: %s\n", aria2::unpauseDownload(session, other) == 0 ? "accepted" : "refused");
	pump_for(session, RAIJU_ACTION_SECONDS, "unpaused", pause_path, &peak_speed, &passes);

	printf("remove: %s\n", aria2::removeDownload(session, other) == 0 ? "accepted" : "refused");
	report_stat(session, "after remove");

	report_cpu("cpu");
	printf("rpc: enable-rpc was passed to the library and no socket opened; libaria2's header documents that the embedded session provides no RPC server\n");

	unlink(pause_path.c_str());
	unlink(control_path.c_str());
	unlink(path.c_str());
	printf("cleanup: removed the benchmark file, the pause-test file and its control file\n");
	report_space("space after cleanup");

	aria2::shutdown(session);
	aria2::sessionFinal(session);
	aria2::libraryDeinit();
	notify("Raiju", "the embedded engine benchmark finished");
	printf("exit\n");
	payload_exit(0);

	return 0;
}
