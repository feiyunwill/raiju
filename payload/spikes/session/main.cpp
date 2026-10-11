#include <aria2/aria2.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include <ps5/kernel.h>
#include <ps5/payload.h>

#define RAIJU_DATA_ROOT "/data/raiju"
#define RAIJU_DOWNLOADS RAIJU_DATA_ROOT "/downloads"
#define RAIJU_SESSION_FILE RAIJU_DATA_ROOT "/session/engine.session"
#define RAIJU_LOG_FILE RAIJU_DATA_ROOT "/session/aria2.log"
#define RAIJU_LOG_BYTES 2048

#ifndef RAIJU_VERSION
#define RAIJU_VERSION "unknown"
#endif

/** Says whether a path exists, and how big it is, before anything touches it. */
static void report_existence(const char *label, const char *path) {
	struct stat info;

	if (stat(path, &info) == 0) {
		printf("%s: %s exists, %lld bytes\n", label, path, (long long)info.st_size);
		return;
	}
	printf("%s: %s is absent (%s)\n", label, path, strerror(errno));
}

/** Prints the printable tail of a file, so a log aria2 keeps hidden becomes visible. */
static void report_text_tail(const char *label, const char *path) {
	char buffer[RAIJU_LOG_BYTES];
	size_t got;
	FILE *file = fopen(path, "rb");

	if (file == NULL) {
		printf("%s: %s is absent (%s)\n", label, path, strerror(errno));
		return;
	}
	if (fseek(file, -(long)sizeof(buffer), SEEK_END) != 0 && fseek(file, 0, SEEK_SET) != 0) {
		printf("%s: seek: %s\n", label, strerror(errno));
		fclose(file);
		return;
	}
	got = fread(buffer, 1, sizeof(buffer), file);
	fclose(file);
	printf("%s: %s\n", label, path);
	for (size_t i = 0; i < got; i++) {
		char character = buffer[i];

		if (character == '\n' || character == '\t' || (character >= 32 && character < 127)) {
			putchar(character);
		}
	}
	printf("\n");
}

/** Tries to create a session from one option set and tears it down again. */
static bool try_session(const char *label, const aria2::KeyVals &options,
			const aria2::SessionConfig &config) {
	aria2::Session *session = aria2::sessionNew(options, config);

	printf("%s: %s\n", label, session != NULL ? "created" : "failed");
	if (session == NULL) {
		return false;
	}
	aria2::shutdown(session);
	aria2::sessionFinal(session);

	return true;
}

/**
 * Works out why `sessionNew` fails on the console after a run was interrupted. It clears the
 * save-session file the interrupted run left behind, records whether that file was there at all,
 * then creates sessions from three option sets — the full one, the same one without `save-session`,
 * and a minimal one — and prints the log libaria2 keeps to itself. It downloads nothing and leaves no
 * session running.
 */
int main(void) {
	aria2::KeyVals full;
	aria2::KeyVals without_session;
	aria2::KeyVals minimal;
	aria2::SessionConfig config;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("raiju session probe %s\n", RAIJU_VERSION);
	printf("firmware %08x\n", kernel_get_fw_version());

	mkdir(RAIJU_DATA_ROOT, 0755);
	mkdir(RAIJU_DOWNLOADS, 0755);
	report_existence("before", RAIJU_SESSION_FILE);
	unlink(RAIJU_SESSION_FILE);
	unlink(RAIJU_LOG_FILE);

	if (aria2::libraryInit() != 0) {
		printf("library: init failed\n");
		payload_exit(1);
		return 1;
	}
	printf("library: initialised\n");

	full.push_back(std::make_pair(std::string("enable-rpc"), std::string("true")));
	full.push_back(std::make_pair(std::string("rpc-listen-all"), std::string("true")));
	full.push_back(std::make_pair(std::string("rpc-listen-port"), std::string("6800")));
	full.push_back(std::make_pair(std::string("dir"), std::string(RAIJU_DOWNLOADS)));
	full.push_back(std::make_pair(std::string("file-allocation"), std::string("none")));
	full.push_back(std::make_pair(std::string("continue"), std::string("true")));
	full.push_back(std::make_pair(std::string("max-connection-per-server"), std::string("16")));
	full.push_back(std::make_pair(std::string("split"), std::string("16")));
	full.push_back(std::make_pair(std::string("auto-file-renaming"), std::string("false")));
	full.push_back(std::make_pair(std::string("check-certificate"), std::string("false")));
	full.push_back(std::make_pair(std::string("save-session"), std::string(RAIJU_SESSION_FILE)));
	full.push_back(std::make_pair(std::string("console-log-level"), std::string("notice")));
	full.push_back(std::make_pair(std::string("summary-interval"), std::string("0")));

	for (const auto &entry : full) {
		if (entry.first != "save-session") {
			without_session.push_back(entry);
		}
	}
	minimal.push_back(std::make_pair(std::string("dir"), std::string(RAIJU_DOWNLOADS)));

	config.keepRunning = false;
	try_session("session, full set", full, config);
	try_session("session, without save-session", without_session, config);
	try_session("session, minimal set", minimal, config);

	report_text_tail("log", RAIJU_LOG_FILE);
	report_existence("after", RAIJU_SESSION_FILE);

	aria2::libraryDeinit();
	printf("exit\n");
	payload_exit(0);

	return 0;
}
