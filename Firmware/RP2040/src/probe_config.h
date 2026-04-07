#ifndef PROBE_CONFIG_H_
#define PROBE_CONFIG_H_

#include "FreeRTOS.h"
#include "task.h"

#if false
#define probe_info(format,args...) \
do { \
	vTaskSuspendAll(); \
	printf(format, ## args); \
	xTaskResumeAll(); \
} while (0)
#else
#define probe_info(format,...) ((void)0)
#endif

#if false
#define probe_debug(format,args...) \
do { \
	vTaskSuspendAll(); \
	printf(format, ## args); \
	xTaskResumeAll(); \
} while (0)
#else
#define probe_debug(format,...) ((void)0)
#endif

#if false
#define probe_dump(format,args...)\
do { \
	vTaskSuspendAll(); \
	printf(format, ## args); \
	xTaskResumeAll(); \
} while (0)
#else
#define probe_dump(format,...) ((void)0)
#endif

#ifdef BB_HAT_FIRMWARE
#include "board_bugbuster_hat_config.h"
#elif defined(DEBUG_ON_PICO)
#include "board_pico_config.h"
#else
#include "board_debug_probe_config.h"
#endif

void bi_decl_config();

#define PROTO_DAP_V1 1
#define PROTO_DAP_V2 2

#ifndef PROBE_DEBUG_PROTOCOL
#define PROBE_DEBUG_PROTOCOL PROTO_DAP_V2
#endif

#endif
