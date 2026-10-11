#include <aria2/aria2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_DOWNLOADS RAIJU_DATA_ROOT "/downloads"
#define RAIJU_RUN_SECONDS 240.0
#define RAIJU_REPORT_SECONDS 5.0

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

#ifndef RAIJU_SOURCE_URL
#define RAIJU_SOURCE_URL "http://192.168.100.74:8001/raiju-test.bin"
#endif

/** Monotonic seconds, for timing a download. */
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

/** Prints the bytes actually allocated to a file, which sparse lengths do not show. */
static int64_t allocated_bytes(const std::string &path) {
	struct stat info;

	if (stat(path.c_str(), &info) != 0) {
		return -1;
	}

	return (int64_t)info.st_blocks * 512;
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

/** Pumps the event loop until nothing is left to download, or the budget runs out. */
static double pump_until_done(aria2::Session *session, double budget) {
	double started = monotonic_seconds();
	double next_report = started + RAIJU_REPORT_SECONDS;
	double finished = 0.0;

	for (;;) {
		aria2::GlobalStat stat;
		double now;

		if (aria2::run(session, aria2::RUN_ONCE) != 1) {
			finished = monotonic_seconds();
			break;
		}
		stat = aria2::getGlobalStat(session);
		now = monotonic_seconds();
		if (stat.numActive == 0 && stat.numWaiting == 0) {
			finished = now;
			break;
		}
		if (now - started > budget) {
			printf("budget: %.0f s spent with %d active and %d waiting\n", budget,
			       stat.numActive, stat.numWaiting);
			finished = now;
			break;
		}
		if (now >= next_report) {
			printf("running: %d B/s, %d active\n", stat.downloadSpeed, stat.numActive);
			next_report = now + RAIJU_REPORT_SECONDS;
		}
	}

	return finished - started;
}

/**
 * Measures whether more connections per server than upstream's original cap actually help, which is
 * the only part of the motrixapp fork Raiju might have wanted. It downloads the same file twice from
 * a source that throttles each connection to one mebibyte per second — sequentially, so each run has
 * the whole link — once with the upstream limit of sixteen and once with sixty-four, and reports what
 * each achieved. Removing what it downloaded happens after the session is finished, and says whether
 * each removal worked.
 */
int main(void) {
	aria2::Session *session;
	aria2::KeyVals options;
	aria2::SessionConfig config;
	aria2::A2Gid gid;
	std::string source_name = download_path(RAIJU_SOURCE_URL);
	std::string first_path = download_path("raiju-16conn.bin");
	std::string second_path = download_path("raiju-64conn.bin");
	std::string first_control = first_path + ".aria2";
	std::string second_control = second_path + ".aria2";
	double elapsed;
	int64_t allocated;
	int result;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("raiju connection spike %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());
	printf("source: %s\n", RAIJU_SOURCE_URL);
	(void)source_name;

	mkdir(RAIJU_DATA_ROOT, 0755);
	mkdir(RAIJU_DOWNLOADS, 0755);
	unlink(first_path.c_str());
	unlink(second_path.c_str());
	unlink(first_control.c_str());
	unlink(second_control.c_str());

	if (aria2::libraryInit() != 0) {
		printf("library: init failed\n");
		payload_exit(1);
		return 1;
	}
	session = NULL;

	options.push_back(std::make_pair("dir", RAIJU_DOWNLOADS));
	options.push_back(std::make_pair("file-allocation", "none"));
	options.push_back(std::make_pair("auto-file-renaming", "false"));
	options.push_back(std::make_pair("console-log-level", "notice"));
	options.push_back(std::make_pair("summary-interval", "0"));
	options.push_back(std::make_pair("min-split-size", "4M"));

	config.keepRunning = true;
	session = aria2::sessionNew(options, config);
	if (session == NULL) {
		printf("session: creation failed\n");
		aria2::libraryDeinit();
		payload_exit(1);
		return 1;
	}
	printf("library: initialised, session created\n");

	result = aria2::addUri(session, &gid, std::vector<std::string>{RAIJU_SOURCE_URL},
			       aria2::KeyVals{{"out", "raiju-16conn.bin"},
					      {"max-connection-per-server", "16"},
					      {"split", "16"}});
	printf("sixteen connections: add %s\n", result == 0 ? "accepted" : "refused");
	elapsed = pump_until_done(session, RAIJU_RUN_SECONDS);
	allocated = allocated_bytes(first_path);
	if (allocated > 0 && elapsed > 0.0) {
		printf("sixteen connections: %.1f MB in %.1f s, %.1f MB/s\n",
		       allocated / 1e6, elapsed, allocated / elapsed / 1e6);
	} else {
		printf("sixteen connections: %lld allocated bytes in %.1f s\n",
		       (long long)allocated, elapsed);
	}

	result = aria2::addUri(session, &gid, std::vector<std::string>{RAIJU_SOURCE_URL},
			       aria2::KeyVals{{"out", "raiju-64conn.bin"},
					      {"max-connection-per-server", "64"},
					      {"split", "64"}});
	printf("sixty-four connections: add %s\n", result == 0 ? "accepted" : "refused");
	elapsed = pump_until_done(session, RAIJU_RUN_SECONDS);
	allocated = allocated_bytes(second_path);
	if (allocated > 0 && elapsed > 0.0) {
		printf("sixty-four connections: %.1f MB in %.1f s, %.1f MB/s\n",
		       allocated / 1e6, elapsed, allocated / elapsed / 1e6);
	} else {
		printf("sixty-four connections: %lld allocated bytes in %.1f s\n",
		       (long long)allocated, elapsed);
	}

	report_cpu("cpu");

	aria2::shutdown(session);
	aria2::sessionFinal(session);
	aria2::libraryDeinit();

	printf("cleanup: sixteen %s, sixty-four %s\n",
	       unlink(first_path.c_str()) == 0 ? "removed" : "kept",
	       unlink(second_path.c_str()) == 0 ? "removed" : "kept");
	unlink(first_control.c_str());
	unlink(second_control.c_str());
	printf("exit\n");
	payload_exit(0);

	return 0;
}
