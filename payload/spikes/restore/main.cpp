#include <aria2/aria2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_DOWNLOADS RAIJU_DATA_ROOT "/downloads"
#define RAIJU_SESSION_FILE RAIJU_DATA_ROOT "/session/restore.session"
#define RAIJU_FIRST_SECONDS 8.0
#define RAIJU_BUDGET_SECONDS 180.0
#define RAIJU_REPORT_SECONDS 4.0
#define RAIJU_OUT "raiju-restore.bin"

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

#ifndef RAIJU_SOURCE_URL
#define RAIJU_SOURCE_URL "http://192.168.100.74:8001/raiju-test.bin"
#endif

#ifndef RAIJU_PHASE
#define RAIJU_PHASE 1
#endif

/** Monotonic seconds, for timing a download. */
static double monotonic_seconds(void) {
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return 0.0;
	}

	return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

/** Prints the bytes actually allocated to a file, which sparse lengths do not show. */
static void report_allocated(const char *label, const std::string &path) {
	struct stat info;

	if (stat(path.c_str(), &info) != 0) {
		printf("%s: %s is absent (%s)\n", label, path.c_str(), strerror(errno));
		return;
	}
	printf("%s: %lld bytes allocated, %lld bytes long\n", label,
	       (long long)info.st_blocks * 512, (long long)info.st_size);
}

/** Prints how much CPU this process has used so far. */
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

/** Pumps the event loop for a while, or until nothing is left to download. */
static double pump(aria2::Session *session, double seconds) {
	double started = monotonic_seconds();
	double next_report = started + RAIJU_REPORT_SECONDS;

	for (;;) {
		aria2::GlobalStat stat;
		double now;

		if (aria2::run(session, aria2::RUN_ONCE) != 1) {
			printf("run: the session finished the loop\n");
			break;
		}
		stat = aria2::getGlobalStat(session);
		now = monotonic_seconds();
		if (stat.numActive == 0 && stat.numWaiting == 0) {
			printf("run: nothing left to download after %.1f s\n", now - started);
			break;
		}
		if (now - started >= seconds) {
			printf("stop: %.0f s spent, %d active and %d waiting at %d B/s\n",
			       seconds, stat.numActive, stat.numWaiting, stat.downloadSpeed);
			break;
		}
		if (now >= next_report) {
			printf("running: %d B/s, %d active, %d waiting\n", stat.downloadSpeed,
			       stat.numActive, stat.numWaiting);
			next_report = now + RAIJU_REPORT_SECONDS;
		}
	}

	return monotonic_seconds() - started;
}

/** Creates the engine session the test needs, with the session file as its only durable state. */
static aria2::Session *start_engine(void) {
	aria2::KeyVals options;
	aria2::SessionConfig config;

	options.push_back(std::make_pair("dir", RAIJU_DOWNLOADS));
	options.push_back(std::make_pair("file-allocation", "none"));
	options.push_back(std::make_pair("continue", "true"));
	options.push_back(std::make_pair("auto-file-renaming", "false"));
	options.push_back(std::make_pair("max-connection-per-server", "16"));
	options.push_back(std::make_pair("split", "16"));
	options.push_back(std::make_pair("min-split-size", "4M"));
	options.push_back(std::make_pair("save-session", RAIJU_SESSION_FILE));
	options.push_back(std::make_pair("console-log-level", "notice"));
	options.push_back(std::make_pair("summary-interval", "0"));

	config.keepRunning = true;

	return aria2::sessionNew(options, config);
}

/**
 * Measures whether a download survives the payload being restarted. The first phase starts a
 * download, stops after a few seconds and closes the session properly, which is the one case where
 * the engine gets to write its session file; the second phase starts fresh and reports whether the
 * engine resumes that download by itself, and if it does not, whether adding the same URI again
 * continues from the bytes already on disk. Everything it downloads is removed at the end.
 */
int main(void) {
	std::string path = std::string(RAIJU_DOWNLOADS) + "/" + RAIJU_OUT;
	std::string control = path + ".aria2";
	aria2::Session *session;
	std::string session_url = RAIJU_SOURCE_URL;
	aria2::A2Gid gid;
	double elapsed;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("raiju restore spike, phase %d, %s\n", RAIJU_PHASE, RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());
	printf("source: %s\n", RAIJU_SOURCE_URL);

	mkdir(RAIJU_DATA_ROOT, 0755);
	mkdir(RAIJU_DOWNLOADS, 0755);
	mkdir(RAIJU_DATA_ROOT "/session", 0755);

	if (RAIJU_PHASE == 1) {
		unlink(path.c_str());
		unlink(control.c_str());
		unlink(RAIJU_SESSION_FILE);
		printf("start: cleared the partial file, its control file and the session file\n");
	} else {
		report_allocated("before", path);
		report_allocated("session file", RAIJU_SESSION_FILE);
	}

	if (aria2::libraryInit() != 0) {
		printf("library: init failed\n");
		payload_exit(1);
		return 1;
	}
	session = start_engine();
	if (session == NULL) {
		printf("session: creation failed\n");
		aria2::libraryDeinit();
		payload_exit(1);
		return 1;
	}
	printf("library: initialised, session created\n");

	if (RAIJU_PHASE == 2) {
		aria2::GlobalStat stat = aria2::getGlobalStat(session);

		printf("resumed: %d active, %d waiting, %d stopped\n", stat.numActive,
		       stat.numWaiting, stat.numStopped);
		if (stat.numActive == 0 && stat.numWaiting == 0) {
			printf("resumed: the session did not bring the download back by itself\n");
			result = aria2::addUri(session, &gid,
					       std::vector<std::string>{session_url},
					       aria2::KeyVals{{"out", RAIJU_OUT}});
			printf("re-add: %s\n", result == 0 ? "accepted" : "refused");
		}
		pump(session, RAIJU_BUDGET_SECONDS);
		report_allocated("after", path);
		report_cpu("cpu");

		aria2::shutdown(session);
		aria2::sessionFinal(session);
		aria2::libraryDeinit();
		printf("cleanup: partial %s, control %s, session %s\n",
		       unlink(path.c_str()) == 0 ? "removed" : "kept",
		       unlink(control.c_str()) == 0 ? "removed" : "kept",
		       unlink(RAIJU_SESSION_FILE) == 0 ? "removed" : "kept");
	} else {
		result = aria2::addUri(session, &gid, std::vector<std::string>{session_url},
				       aria2::KeyVals{{"out", RAIJU_OUT}});
		printf("add: %s\n", result == 0 ? "accepted" : "refused");
		elapsed = pump(session, RAIJU_FIRST_SECONDS);
		report_allocated("stopped at", path);
		printf("phase: ran %.1f s before closing the session\n", elapsed);

		aria2::shutdown(session);
		aria2::sessionFinal(session);
		aria2::libraryDeinit();
		report_allocated("session file after closing", RAIJU_SESSION_FILE);
		printf("left: partial and control file in place for phase 2\n");
	}

	printf("exit\n");
	payload_exit(0);

	return 0;
}
