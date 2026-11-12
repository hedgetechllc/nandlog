#ifndef __SYSTEM_HEADER_H__
#define __SYSTEM_HEADER_H__

// HOST BUILD ONLY (make soak). The few board functions the on-device soak, tests/test_nandlog_powercut.c, calls,
// with the signatures they have in the firmware. nandlog_sim_soak.c defines them: a delay is where it cuts the power
// between operations, and a reset never returns

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nandlog_conf.h"

void am_hal_delay_us(uint32_t microseconds);
void setup_hardware(void);
void system_reset(bool immediate);
void system_enable_interrupts(bool enabled);

#endif  // #ifndef __SYSTEM_HEADER_H__
