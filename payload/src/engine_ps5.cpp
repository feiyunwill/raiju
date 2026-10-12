#include "engine_ps5.h"

#include <aria2/aria2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <string>
#include <utility>

#define RAIJU_ENGINE_START_SECONDS 30

namespace {

pthread_mutex_t engine_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t engine_settled = PTHREAD_COND_INITIALIZER;
pthread_t engine_thread;
aria2::Session *engine_session = NULL;
bool engine_thread_started = false;
bool engine_running = false;

/** Reads the generated key=value options file into the library's option vector. */
bool read_options(const char *options_file, aria2::KeyVals *options) {
	FILE *file = fopen(options_file, "r");
	char line[1024];

	if (file == NULL) {
		printf("engine: cannot read %s\n", options_file);
		return false;
	}
	while (fgets(line, sizeof line, file) != NULL) {
		char *terminator = strpbrk(line, "\r\n");
		char *equals = strchr(line, '=');

		if (terminator != NULL) {
			*terminator = '\0';
		}
		if (equals == NULL || line[0] == '#' || line[0] == '\0') {
			continue;
		}
		*equals = '\0';
		options->push_back(std::make_pair(std::string(line), std::string(equals + 1)));
	}
	fclose(file);
	if (options->empty()) {
		printf("engine: %s carries no options\n", options_file);
		return false;
	}
	return true;
}

/**
 * Initialises the library, creates the session and drives its event loop. A
 * session without its options would serve no RPC and download into the
 * hijacked process's working directory, so an unreadable or empty options
 * file fails the start instead of falling back to aria2's defaults.
 */
void *engine_main(void *argument) {
	char *options_file = (char *)argument;
	aria2::KeyVals options;
	aria2::SessionConfig config;
	aria2::Session *session;
	bool stop;

	if (!read_options(options_file, &options)) {
		free(options_file);
		pthread_mutex_lock(&engine_mutex);
		engine_running = false;
		pthread_cond_broadcast(&engine_settled);
		pthread_mutex_unlock(&engine_mutex);
		return NULL;
	}
	free(options_file);
	if (aria2::libraryInit() != 0) {
		printf("engine: library init failed\n");
		pthread_mutex_lock(&engine_mutex);
		engine_running = false;
		pthread_cond_broadcast(&engine_settled);
		pthread_mutex_unlock(&engine_mutex);
		return NULL;
	}
	config.keepRunning = true;
	session = aria2::sessionNew(options, config);

	pthread_mutex_lock(&engine_mutex);
	engine_session = session;
	engine_running = session != NULL;
	stop = !engine_running;
	pthread_cond_broadcast(&engine_settled);
	pthread_mutex_unlock(&engine_mutex);

	if (session == NULL) {
		printf("engine: session creation failed\n");
		aria2::libraryDeinit();
		return NULL;
	}
	printf("engine: session up\n");
	fflush(stdout);

	while (!stop) {
		int result = aria2::run(session, aria2::RUN_ONCE);

		pthread_mutex_lock(&engine_mutex);
		stop = !engine_running;
		pthread_mutex_unlock(&engine_mutex);
		if (result != 1) {
			printf("engine: loop finished with %d\n", result);
			break;
		}
	}

	aria2::shutdown(session);
	aria2::sessionFinal(session);
	aria2::libraryDeinit();
	printf("engine: stopped\n");
	fflush(stdout);
	return NULL;
}

}  // namespace

extern "C" int raiju_engine_start(const char *options_file) {
	struct timespec deadline;
	char *copy = strdup(options_file);
	int timed_out = 0;

	if (copy == NULL) {
		return -1;
	}
	pthread_mutex_lock(&engine_mutex);
	if (engine_thread_started) {
		pthread_mutex_unlock(&engine_mutex);
		free(copy);
		return 0;
	}
	engine_thread_started = true;
	if (pthread_create(&engine_thread, NULL, engine_main, copy) != 0) {
		engine_thread_started = false;
		pthread_mutex_unlock(&engine_mutex);
		free(copy);
		return -1;
	}
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += RAIJU_ENGINE_START_SECONDS;
	while (!engine_running && !timed_out) {
		timed_out = pthread_cond_timedwait(&engine_settled, &engine_mutex, &deadline) != 0;
	}
	pthread_mutex_unlock(&engine_mutex);
	return engine_running ? 0 : -1;
}

extern "C" void raiju_engine_stop(void) {
	pthread_mutex_lock(&engine_mutex);
	if (!engine_thread_started) {
		pthread_mutex_unlock(&engine_mutex);
		return;
	}
	engine_running = false;
	pthread_mutex_unlock(&engine_mutex);

	pthread_join(engine_thread, NULL);

	pthread_mutex_lock(&engine_mutex);
	engine_thread_started = false;
	engine_session = NULL;
	pthread_cond_broadcast(&engine_settled);
	pthread_mutex_unlock(&engine_mutex);
}
