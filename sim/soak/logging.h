#ifndef __LOGGING_HEADER_H__
#define __LOGGING_HEADER_H__

// HOST BUILD ONLY (make soak). Stands in for the firmware's logging.h, which maps print() onto the board's
// am_util_stdio_printf(); the soak harness, nandlog_sim_soak.c, prints it to stdout

#include "system.h"

uint32_t am_util_stdio_printf(const char *format, ...);
#define print(...) am_util_stdio_printf(__VA_ARGS__)

#endif  // #ifndef __LOGGING_HEADER_H__
