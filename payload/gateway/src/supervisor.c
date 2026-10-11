#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "raiju/supervisor.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

static bool make_directory(const char *path) {
	return raiju_mkdir_p(path);
}

static bool format_path(char *out, size_t capacity, const char *data_dir, const char *suffix) {
	return snprintf(out, capacity, "%s/%s", data_dir, suffix) < (int)capacity;
}

static bool ensure_file(const char *path) {
	int fd = open(path, O_CREAT, 0600);
	if (fd < 0) {
		return false;
	}
	close(fd);
	return true;
}

raiju_supervisor_status raiju_supervisor_prepare(raiju_supervisor *supervisor, const raiju_platform *platform,
                                                 const char *data_dir) {
	memset(supervisor, 0, sizeof *supervisor);
	supervisor->platform = platform;
	supervisor->lock_fd = -1;
	if (snprintf(supervisor->data_dir, sizeof supervisor->data_dir, "%s", data_dir) >=
	    (int)sizeof supervisor->data_dir) {
		return RAIJU_SUPERVISOR_IO_ERROR;
	}
	if (!format_path(supervisor->downloads_dir, sizeof supervisor->downloads_dir, data_dir, "downloads") ||
	    !format_path(supervisor->staging_dir, sizeof supervisor->staging_dir, data_dir, "staging") ||
	    !format_path(supervisor->trash_dir, sizeof supervisor->trash_dir, data_dir, "trash") ||
	    !format_path(supervisor->session_dir, sizeof supervisor->session_dir, data_dir, "session") ||
	    !format_path(supervisor->www_dir, sizeof supervisor->www_dir, data_dir, "www") ||
	    !format_path(supervisor->lock_path, sizeof supervisor->lock_path, data_dir, "raiju.lock") ||
	    !format_path(supervisor->secret_path, sizeof supervisor->secret_path, data_dir, "session/aria2.secret") ||
	    !format_path(supervisor->options_path, sizeof supervisor->options_path, data_dir, "session/aria2.conf")) {
		return RAIJU_SUPERVISOR_IO_ERROR;
	}
	if (!make_directory(supervisor->data_dir) || !make_directory(supervisor->downloads_dir) ||
	    !make_directory(supervisor->staging_dir) || !make_directory(supervisor->trash_dir) ||
	    !make_directory(supervisor->session_dir) || !make_directory(supervisor->www_dir)) {
		return RAIJU_SUPERVISOR_IO_ERROR;
	}
	int fd = open(supervisor->lock_path, O_RDWR | O_CREAT, 0600);
	if (fd < 0) {
		return RAIJU_SUPERVISOR_IO_ERROR;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
		close(fd);
		return RAIJU_SUPERVISOR_ALREADY_RUNNING;
	}
	supervisor->lock_fd = fd;
	supervisor->locked = true;
	if (!raiju_engine_config_init(&supervisor->engine, platform, data_dir, supervisor->secret_path)) {
		return RAIJU_SUPERVISOR_IO_ERROR;
	}
	char session_file[RAIJU_PATH_MAX];
	if (!format_path(session_file, sizeof session_file, supervisor->session_dir, RAIJU_ENGINE_SESSION_FILE) ||
	    !ensure_file(session_file)) {
		return RAIJU_SUPERVISOR_IO_ERROR;
	}
	if (!raiju_engine_options_write(&supervisor->engine, supervisor->options_path)) {
		return RAIJU_SUPERVISOR_IO_ERROR;
	}
	return RAIJU_SUPERVISOR_OK;
}

raiju_supervisor_status raiju_supervisor_launch_engine(raiju_supervisor *supervisor, const char *engine_command) {
	if (supervisor->platform->launch_engine(supervisor->platform->ctx, engine_command, supervisor->options_path) !=
	    0) {
		return RAIJU_SUPERVISOR_ENGINE_ERROR;
	}
	return RAIJU_SUPERVISOR_OK;
}

raiju_supervisor_status raiju_supervisor_announce(const raiju_supervisor *supervisor, const char *address,
                                                  uint16_t port, const char *code) {
	char message[256];
	snprintf(message, sizeof message, "Raiju ready - http://%s:%u - code %s", address, (unsigned)port, code);
	if (supervisor->platform->notify(supervisor->platform->ctx, message) != 0) {
		return RAIJU_SUPERVISOR_IO_ERROR;
	}
	return RAIJU_SUPERVISOR_OK;
}

void raiju_supervisor_close(raiju_supervisor *supervisor) {
	if (supervisor->locked && supervisor->lock_fd >= 0) {
		flock(supervisor->lock_fd, LOCK_UN);
		close(supervisor->lock_fd);
		supervisor->lock_fd = -1;
		supervisor->locked = false;
	}
}
