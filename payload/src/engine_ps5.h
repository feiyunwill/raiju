#ifndef RAIJU_ENGINE_PS5_H
#define RAIJU_ENGINE_PS5_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates the embedded aria2 session on a thread of its own, reading the
 * options the supervisor wrote to options_file (ADR 0001: the engine runs
 * in-process, because a payload cannot start a program it built). Returns 0
 * once the session is up, -1 when it could not be created.
 */
int raiju_engine_start(const char *options_file);

/**
 * Shuts the session down, joins the engine thread and releases the library.
 * Safe to call when the engine never started.
 */
void raiju_engine_stop(void);

#ifdef __cplusplus
}
#endif

#endif
