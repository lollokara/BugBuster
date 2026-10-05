#pragma once

// hub_task.h - the single cooperative task behind device -> hub streaming (spec 2026-10-03 section 6).
#ifdef __cplusplus
extern "C" {
#endif

/** Early in app_main: installs the log hook so boot logs are kept. Idempotent. */
void hub_early_init(void);
/** After the web server and HAT are up: allocates buffers and starts the task (6 KB internal stack). */
void hub_start(void);

#ifdef __cplusplus
}
#endif
