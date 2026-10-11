#ifndef RAIJU_SUPERVISOR_H
#define RAIJU_SUPERVISOR_H

#include <stdbool.h>
#include <stdint.h>

#include "raiju/engine_options.h"
#include "raiju/fsutil.h"
#include "raiju/platform.h"

typedef enum {
	RAIJU_SUPERVISOR_OK = 0,
	RAIJU_SUPERVISOR_ALREADY_RUNNING,
	RAIJU_SUPERVISOR_IO_ERROR,
	RAIJU_SUPERVISOR_ENGINE_ERROR
} raiju_supervisor_status;

/**
 * The supervisor's view of the data directory: the directories it creates on
 * first run, the lock that keeps a single instance alive, and the engine
 * options it writes and launches.
 */
typedef struct {
	const raiju_platform *platform;
	char data_dir[RAIJU_PATH_MAX];
	char downloads_dir[RAIJU_PATH_MAX];
	char staging_dir[RAIJU_PATH_MAX];
	char trash_dir[RAIJU_PATH_MAX];
	char session_dir[RAIJU_PATH_MAX];
	char www_dir[RAIJU_PATH_MAX];
	char lock_path[RAIJU_PATH_MAX];
	char options_path[RAIJU_PATH_MAX];
	char secret_path[RAIJU_PATH_MAX];
	raiju_engine_config engine;
	int lock_fd;
	bool locked;
} raiju_supervisor;

/**
 * Creates /data/raiju/{downloads,staging,trash,session,www} under data_dir,
 * takes the single-instance lock at data_dir/raiju.lock, loads or generates
 * the engine secret and writes the aria2 options file. Returns
 * RAIJU_SUPERVISOR_ALREADY_RUNNING when another copy holds the lock (the
 * caller re-shows the notification and exits).
 */
raiju_supervisor_status raiju_supervisor_prepare(raiju_supervisor *supervisor, const raiju_platform *platform,
                                                 const char *data_dir);

/**
 * Launches the engine through the platform hook and shows the ready
 * notification carrying the address and the pairing code.
 */
raiju_supervisor_status raiju_supervisor_launch_engine(raiju_supervisor *supervisor, const char *engine_command);

/**
 * Shows the "Raiju ready" notification for the given address and code through
 * the platform hook.
 */
raiju_supervisor_status raiju_supervisor_announce(const raiju_supervisor *supervisor, const char *address,
                                                  uint16_t port, const char *code);

/**
 * Shows the "already running" notification, then releases the lock.
 */
void raiju_supervisor_close(raiju_supervisor *supervisor);

#endif
