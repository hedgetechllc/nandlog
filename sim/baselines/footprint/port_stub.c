// Stand-in nandlog_port.h for the footprint links: every stack uses the same Alliance driver, which calls these.
// Each body is the smallest that keeps the call meaningful; the port is platform code, so it is linked into every
// image and reported separately from the libraries.
#include "nandlog_port.h"

volatile uint32_t port_sink;

bool nandlog_port_init(void) { return true; }
void nandlog_port_deinit(void) {}
void nandlog_port_lock(void) {}
void nandlog_port_unlock(void) {}
void nandlog_port_transfer_read(uint8_t command, const void *address, uint32_t address_length, void *read_buffer, uint32_t read_length)
{
   port_sink = command + address_length + read_length + (uint32_t)address + (uint32_t)read_buffer;
}
void nandlog_port_transfer_write(uint8_t command, const void *address, uint32_t address_length, const void *write_buffer, uint32_t write_length)
{
   port_sink = command + address_length + write_length + (uint32_t)address + (uint32_t)write_buffer;
}
void nandlog_port_write_enable(bool enable) { port_sink = enable; }
void nandlog_port_power(bool awake) { port_sink = awake; }
void nandlog_port_log(const char *format, ...) { port_sink = (uint32_t)format; }
void nandlog_port_delay_us(uint32_t microseconds) { port_sink = microseconds; }
void nandlog_port_delay_ms(uint32_t milliseconds) { port_sink = milliseconds; }
void nandlog_port_fatal(const char *reason) { port_sink = (uint32_t)reason; for (;;) {} }
void nandlog_port_unwritable(void) { port_sink = 1; }
