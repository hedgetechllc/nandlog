#ifndef __NANDLOG_PORT_HEADER_H__
#define __NANDLOG_PORT_HEADER_H__

#include <stdbool.h>
#include <stdint.h>
#include "nandlog_conf.h"

// Marks a function that never returns: the GCC and Clang attribute, C11's keyword, or nothing on other compilers
#if defined(__GNUC__) || defined(__clang__)
#define NANDLOG_NORETURN                            __attribute__ ((__noreturn__))
#elif defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#define NANDLOG_NORETURN                            _Noreturn
#else
#define NANDLOG_NORETURN
#endif

// Initialize the NAND peripheral and communications protocols
bool nandlog_port_init(void);
void nandlog_port_deinit(void);

// Serialize the library against itself; a port with no threads can leave these empty
void nandlog_port_lock(void);
void nandlog_port_unlock(void);

// Assumes a transfer either completes or does not return, so a failure that cannot be retried must end in nandlog_port_fatal()
void nandlog_port_transfer_read(uint8_t command, const void *address, uint32_t address_length, void *read_buffer, uint32_t read_length);
void nandlog_port_transfer_write(uint8_t command, const void *address, uint32_t address_length, const void *write_buffer, uint32_t write_length);

// Gate the chip's write-protect pin: true permits programming and erasing
void nandlog_port_write_enable(bool enable);

// Wake the NAND peripheral or return it to its lowest-power state
void nandlog_port_power(bool awake);

// Route any diagnostic messages as needed
void nandlog_port_log(const char *format, ...);

// How the library calls nandlog_port_log(). A message above NANDLOG_LOG_LEVEL is compiled out, text and all
#define NANDLOG_LOG_ERRORS                          1
#define NANDLOG_LOG_WARNINGS                        2
#define NANDLOG_LOG_INFO                            3
#define NANDLOG_LOG(level, ...)                     do { if (NANDLOG_LOG_LEVEL >= (level)) nandlog_port_log(__VA_ARGS__); } while (0)

// A message's text passed to a function that logs it, compiled out the same way
#define NANDLOG_LOG_TEXT(level, text)               ((NANDLOG_LOG_LEVEL >= (level)) ? (text) : "")

// Platform-specific busy wait functions
void nandlog_port_delay_us(uint32_t microseconds);
void nandlog_port_delay_ms(uint32_t milliseconds);

// Unrecoverable hardware fault. It must not return, because it is declared noreturn and the compiler may drop the
// code after a call to it
NANDLOG_NORETURN void nandlog_port_fatal(const char *reason);

// The part still answers, but the log has stopped taking records: no block will retain a page, or the epoch is sealed
void nandlog_port_unwritable(void);

// Only with NANDLOG_PORT_CRC32 set: the CRC-32 of 'data' continued from 'crc', for every CRC the log writes or checks.
// It must return exactly what Python's zlib.crc32(data, crc) does, so 0 begins a CRC and "123456789" from 0 gives
// 0xCBF43926. The log relies on continuing a CRC this way to add each record to its page's CRC. 'data' may have any
// alignment, and may be NULL when 'length' is 0. The log calls this with its lock held, but nandlog_crc32() calls it
// from its own caller's context. A wrong value makes its page read as damaged, so check an engine against
// nandlog_crc32_software() before trusting it
uint32_t nandlog_port_crc32(uint32_t crc, const void *data, uint32_t length);

// Optional, and never called by the library: whether nandlog_port_crc32() is using a hardware engine, for diagnostics
bool nandlog_port_crc32_hardware(void);

// The library's software CRC-32, with the same contract, for the bytes a port's engine cannot take
uint32_t nandlog_crc32_software(uint32_t crc, const void *data, uint32_t length);

#endif  // #ifndef __NANDLOG_PORT_HEADER_H__
