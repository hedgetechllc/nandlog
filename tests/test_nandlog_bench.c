// ON-DEVICE BENCHMARK -- the measured figures behind the nandlog whitepaper
//
// *****************************************************************************************************************
// ***  DESTRUCTIVE.  THIS PROGRAM ERASES THE METADATA RING AND THE WHOLE LOG REGION, THEN FILLS PART OF IT WITH ***
// ***  BENCHMARK DATA.  EVERY EPOCH AND EVERY RECORD ON THE PART IS GONE WHEN IT FINISHES.                       ***
// ***  RUN IT ON A DEVELOPMENT BOARD ONLY -- NEVER ON A DEVICE HOLDING DATA YOU WANT.                            ***
// *****************************************************************************************************************
//
// It leaves the bad-block table and the rest of the reserve alone, and makes its part-level measurements in the
// four scratch blocks at the top of the reserve, exactly as the other on-device tests do -- or, where fewer than four
// of those are good and unmarked, in the topmost good, unmarked blocks of the log region for the rest, which it is
// about to erase anyway; BENCH_INFO scratch_blocks says which. It also puts about 700 erase cycles on the first
// three blocks of the log region per run, because 600 empty epochs begun back to back all start there. It counts
// the marked blocks at its start and its end (BENCH_INFO marked_blocks), so a marked region that grows shows.
//
// WHAT IT MEASURES, in the order it runs. About three to six minutes in all; it prints as it goes.
//   A  the part and the bus, through the driver and the port: a status read; a page latch with a 32-byte and with
//      a 4,096-byte transfer; the page-read busy time with ECC on and off, written and erased; a page program split
//      into load and busy; a block erase; the internal page copy against a read and a write; and the ECC-off blank
//      check that the recovery uses
//   B  CRC-32 over a page payload and over a page header: the library's software as built, the CRC the log uses
//      (nandlog_crc32(), the port's hardware if it has some), and a byte-a-step reference over a table in flash; which
//      of them agree on known vectors, and whether the port's hardware passed its self-test
//   C  wiping the ring and the log region -- which doubles as an erase distribution over ~4,000 blocks -- and a
//      warm boot with no epoch on the part
//   D  nandlog_init() with 1, 64, 511 and 600 epochs in the metadata ring, and begin_epoch()
//   E  nandlog_init() and every read path against a current epoch of 1,000 and of 10,000 pages
//   F  store_record() split by what each call did, for 32-, 256- and 1,024-byte records, and flush()
//
// OUTPUT. Everything meant to be parsed is on a line of its own beginning BENCH:
//
//    BENCH_INFO <key>=<value> ...
//    BENCH <name> n=<n> min=<us> med=<us> p99=<us> max=<us> mean=<us> [extra=<k:v,k:v,...>]
//    BENCH_COUNT <name> n=<n> reads=<..> raw_reads=<..> programs=<..> erases=<..> polls=<..> reg_writes=<..>
//                bytes_in=<..> bytes_out=<..> bus_us=<..> delay_us=<..> log_lines=<..>
//    BENCH_STATS ...      BENCH_ERROR ...      BENCH_DONE errors=<n> elapsed_s=<s>
//
// Times are microseconds with three decimals, from the DWT cycle counter, converted with the cycles/us figure
// printed once at the top (calibrated against am_hal_delay_us(), so as accurate as the core clock). Percentiles are
// nearest-rank, over every sample or, when there were more than 2,048, over a uniform reservoir of 2,048 of them
// (extra then says sampled:2048); n, min, max and mean are always exact. Interrupts are enabled, as in the other
// tests, so p99 and max include whatever the board's interrupts cost.
//
// BENCH_COUNT values are MEANS PER OPERATION of: reads = PAGE DATA READ commands (page latches) of which raw_reads
// were issued with the part's ECC off; programs = PROGRAM EXECUTEs (internal copies included); erases = BLOCK
// ERASEs; polls = status-register reads (nearly all BUSY polls); reg_writes = status-register writes; bytes_in and
// bytes_out = data bytes clocked out of and into the part's cache register; bus_us = time inside the port's transfer
// functions; delay_us = time inside the port's delay functions (BUSY-poll pacing, and the 3 ms the driver waits at
// init); log_lines = library diagnostics. They need the port wrapped at link time -- one LFLAGS line in the
// Makefile target, see tests/README.md. Without it the bench still runs and says op_counting=0, and no BENCH_COUNT
// lines appear.
//
// ENERGY. Build with EXTRA_DEFINES=-DBENCH_MARKER_PIN=<gpio> and every measured operation is bracketed by that GPIO
// going high, with a short low gap between samples; gate a power analyzer on it. See tests/README.md.
//
// WHAT TO SEND BACK: the whole serial output, from the reset banner to BENCH_DONE, verbatim.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "nandlog_hw_test.h"


// Alliance AS5F18G04SND Command Set, for the measurements that go below the driver ------------------------------------

#define CMD_READ_ID                                 0x9F
#define CMD_GET_FEATURE                             0x0F
#define CMD_SET_FEATURE                             0x1F
#define CMD_WRITE_ENABLE                            0x06
#define CMD_BLOCK_ERASE                             0xD8
#define CMD_PROGRAM_DATA_LOAD                       0x02
#define CMD_PROGRAM_LOAD_RANDOM_DATA                0x84
#define CMD_PROGRAM_EXECUTE                         0x10
#define CMD_PAGE_DATA_READ                          0x13
#define CMD_READ                                    0x03

#define SR1                                         0xA0
#define SR2                                         0xB0
#define SR3                                         0xC0
#define SR_BUSY                                     0x01
#define SR_ERASE_FAILURE                            0x04
#define SR_PROGRAM_FAILURE                          0x08
#define SR2_ECC_ENABLE                              0x10
#define PROTECT_ALL                                 0x7E
#define PROTECT_NONE                                0x02

#ifndef NANDLOG_SOURCE_ID
#define NANDLOG_SOURCE_ID                           "unrecorded"   // the build's fingerprint of the logging sources
#endif

#define PAGE_BYTES                                  (nandlog_chip_geometry()->page_size_bytes)
#define SPARE_BYTES                                 (nandlog_chip_geometry()->spare_size_bytes)
#define PAGES_PER_BLOCK                             (nandlog_chip_geometry()->pages_per_block)


// Run Parameters ------------------------------------------------------------------------------------------------------

#define DIST_SAMPLES                                2048     // reservoir per distribution; percentiles come from it
#define INIT_REPS                                   20       // warm boots timed per state
#define RING_BLOCK_SLOTS                            32       // slots per metadata ring block on this part (a pair of pages each)
#define STORE_PAGES_PER_SIZE                        1280     // 20 blocks, so ~20 calls carry an erase-ahead
#define ERASE_INFERRED_ABOVE_US                     400      // only used when the op counters are not linked in


// Energy Marker -------------------------------------------------------------------------------------------------------
//
// Off unless BENCH_MARKER_PIN names a GPIO. There is deliberately no default: pick a pad you can reach with a probe
// on YOUR board revision and that nothing else drives (src/boards/rev<X>/pinout.h lists what is taken), then build
//     make nandlog_bench BOARD_REV=P EXTRA_DEFINES=-DBENCH_MARKER_PIN=<gpio>
// The pin is high for exactly the span each DWT measurement covers, and low for BENCH_MARKER_GAP_US after it

#if defined(BENCH_MARKER) && !defined(BENCH_MARKER_PIN)
#error "BENCH_MARKER needs BENCH_MARKER_PIN=<gpio>: choose a free, probe-able pad on your board; this file will not guess one"
#endif

#ifdef BENCH_MARKER_PIN
#if ((1 - BENCH_MARKER_PIN - 1) == 2)
#error "BENCH_MARKER_PIN is defined but empty: give it the GPIO number, e.g. -DBENCH_MARKER_PIN=<gpio>"
#endif
#ifndef BENCH_MARKER_GAP_US
#define BENCH_MARKER_GAP_US                         200
#endif
#define MARK_INIT()      do { am_hal_gpio_pinconfig(BENCH_MARKER_PIN, am_hal_gpio_pincfg_output); am_hal_gpio_output_clear(BENCH_MARKER_PIN); } while (0)
#define MARK_HIGH()      am_hal_gpio_output_set(BENCH_MARKER_PIN)
#define MARK_LOW()       am_hal_gpio_output_clear(BENCH_MARKER_PIN)
#define MARK_GAP()       am_hal_delay_us(BENCH_MARKER_GAP_US)
#define MARKER_ENABLED   1
#else
#define MARK_INIT()      do {} while (0)
#define MARK_HIGH()      do {} while (0)
#define MARK_LOW()       do {} while (0)
#define MARK_GAP()       do {} while (0)
#define MARKER_ENABLED   0
#endif


// Static Buffers ------------------------------------------------------------------------------------------------------

// Word-aligned, as the library's own buffers are, so section B2 times what the library does: a byte array need not be,
// and both a word compare and a 16-byte blank check over an unaligned one ran at half speed or worse on the part
static uint32_t page_words[(NANDLOG_MAX_PAGE_SIZE_BYTES + NANDLOG_MAX_SPARE_SIZE_BYTES + 3) / 4];
static uint32_t data_words[(NANDLOG_MAX_PAGE_SIZE_BYTES + NANDLOG_MAX_SPARE_SIZE_BYTES + 3) / 4];
static uint8_t *const page_buffer = (uint8_t*)page_words, *const data_buffer = (uint8_t*)data_words;
static uint32_t errors;
static volatile uint32_t sink;      // keeps a computed value alive, so the compiler cannot drop the work


// Clock ---------------------------------------------------------------------------------------------------------------

static uint32_t cycles_per_100ms, cycles_per_us;
static uint64_t uptime_cycles;
static uint32_t uptime_last;

static void calibrate_clock(void)
{
   // A tenth of a second rather than the scaffolding's 10 ms, for three good decimals of cycles/us
   const uint32_t start = hw_cycle_count();
   am_hal_delay_us(100000);
   cycles_per_100ms = hw_cycle_count() - start;
   cycles_per_us = cycles_per_100ms / 100000;
   uptime_last = hw_cycle_count();
}

static uint64_t now_cycles(void)
{
   // A 64-bit clock from the 32-bit counter, which wraps every ~45 s at 96 MHz. Called often enough in every long
   // loop below that it never misses a wrap
   const uint32_t now = hw_cycle_count();
   uptime_cycles += (uint32_t)(now - uptime_last);
   uptime_last = now;
   return uptime_cycles;
}

static uint64_t cycles_to_ns(uint64_t cycles)
{
   return ((cycles / cycles_per_100ms) * 100000000ull) + (((cycles % cycles_per_100ms) * 100000000ull) / cycles_per_100ms);
}

static uint32_t us_to_cycles(uint32_t microseconds)
{
   return (uint32_t)(((uint64_t)microseconds * cycles_per_100ms) / 100000u);
}


// Line Building -------------------------------------------------------------------------------------------------------
//
// Each BENCH line is assembled here and printed by a single print() call, so nothing the library logs can land in
// the middle of one

typedef struct
{
   char text[600];
   uint32_t length;
} line_t;

static void line_reset(line_t *line)
{
   line->length = 0;
   line->text[0] = '\0';
}

static void line_str(line_t *line, const char *text)
{
   while (*text && ((line->length + 1) < sizeof(line->text)))
      line->text[line->length++] = *text++;
   line->text[line->length] = '\0';
}

static void line_u32(line_t *line, uint32_t value)
{
   char digits[11];
   uint32_t count = 0;
   do
   {
      digits[count++] = (char)('0' + (value % 10));
      value /= 10;
   } while (value);
   char text[12];
   for (uint32_t i = 0; i < count; ++i)
      text[i] = digits[count - 1 - i];
   text[count] = '\0';
   line_str(line, text);
}

static void line_fixed(line_t *line, uint64_t value, uint32_t scale, uint32_t decimals)
{
   // value / scale, printed with exactly 'decimals' decimal places (scale is 10^decimals)
   line_u32(line, (uint32_t)(value / scale));
   line_str(line, ".");
   uint32_t remainder = (uint32_t)(value % scale), divisor = scale / 10;
   for (uint32_t i = 0; i < decimals; ++i, divisor /= 10)
   {
      const char digit[2] = { (char)('0' + ((remainder / divisor) % 10)), '\0' };
      line_str(line, digit);
   }
}

static void line_us(line_t *line, uint64_t cycles)
{
   line_fixed(line, cycles_to_ns(cycles), 1000, 3);
}

static void line_ms(line_t *line, uint64_t cycles)
{
   line_fixed(line, cycles_to_ns(cycles) / 1000, 1000, 3);
}

static void line_key_u32(line_t *line, const char *key, uint32_t value)
{
   line_str(line, key);
   line_u32(line, value);
}

static const char *name_of(const char *prefix, uint32_t number, const char *suffix)
{
   // A few buffers in rotation, so two names can be built for the same call
   static line_t names[4];
   static uint32_t next;
   line_t *name = &names[next++ % 4];
   line_reset(name);
   line_str(name, prefix);
   line_u32(name, number);
   line_str(name, suffix);
   return name->text;
}


// Operation Counting, by Wrapping the Port at Link Time ---------------------------------------------------------------
//
// The library's own counters (nandlog_get_stats) count records and pages, not what reached the part, so the port
// is wrapped instead: with
//     -Wl,--wrap=nandlog_port_transfer_read,--wrap=nandlog_port_transfer_write,--wrap=nandlog_port_log,
//         --wrap=nandlog_port_delay_us,--wrap=nandlog_port_delay_ms
// on the link line, every call the core and the driver make to those functions comes here first. The __real_
// references are weak, so the same object links without the flags too; the wrappers are then simply never called,
// and detect_instrumentation() notices

typedef struct
{
   uint32_t latches, raw_latches, programs, erases, register_reads, register_writes, log_lines, delays;
   uint64_t bytes_in, bytes_out, bus_cycles, delay_cycles;
} bench_ops_t;

static bench_ops_t ops;
static bool part_ecc_on = true, ops_live, log_live, delay_live;

// Section F2's timeline of one library call: every port call inside it, what it was, and when it began and ended. The
// time between two of them is the library's and the driver's own work
#define TRACE_EVENTS                                96
typedef struct
{
   uint32_t start, end, length;
   uint8_t command;
   bool is_read, is_delay;
} trace_event_t;
static trace_event_t trace[TRACE_EVENTS];
static uint32_t trace_count, trace_dropped;
static bool trace_on;

// Section C2's view of a whole boot, too many port calls for the timeline above: the WIDE_GAPS widest stretches of CPU
// time between two port calls, widest first, and the totals
#define WIDE_GAPS                                   8
typedef struct
{
   uint32_t gap, index, length;
   uint8_t command;
   bool is_read, is_delay;
} wide_gap_t;
static wide_gap_t wide_gaps[WIDE_GAPS];
static uint32_t gaps_last_end, gaps_events, gaps_cpu, gaps_port, gaps_delay;
static bool gaps_on;

static void gap_add(uint8_t command, bool is_read, bool is_delay, uint32_t length, uint32_t start, uint32_t end)
{
   const uint32_t gap = start - gaps_last_end, index = gaps_events++;
   gaps_last_end = end;
   gaps_cpu += gap;
   if (is_delay)
      gaps_delay += end - start;
   else
      gaps_port += end - start;
   uint32_t slot = WIDE_GAPS;
   for (; slot && (gap > wide_gaps[slot - 1].gap); --slot)
      if (slot < WIDE_GAPS)
         wide_gaps[slot] = wide_gaps[slot - 1];
   if (slot < WIDE_GAPS)
      wide_gaps[slot] = (wide_gap_t){ .gap = gap, .index = index, .length = length, .command = command, .is_read = is_read, .is_delay = is_delay };
}

static inline void trace_add(uint8_t command, bool is_read, bool is_delay, uint32_t length, uint32_t start, uint32_t end)
{
   if (gaps_on)
      gap_add(command, is_read, is_delay, length, start, end);
   if (!trace_on)
      return;
   if (trace_count < TRACE_EVENTS)
   {
      trace_event_t *event = &trace[trace_count++];
      event->start = start;
      event->end = end;
      event->length = length;
      event->command = command;
      event->is_read = is_read;
      event->is_delay = is_delay;
   }
   else
      ++trace_dropped;
}

void __real_nandlog_port_transfer_read(uint8_t command, const void *address, uint32_t address_length, void *read_buffer, uint32_t read_length) __attribute__((weak));
void __real_nandlog_port_transfer_write(uint8_t command, const void *address, uint32_t address_length, const void *write_buffer, uint32_t write_length) __attribute__((weak));
void __real_nandlog_port_delay_us(uint32_t microseconds) __attribute__((weak));
void __real_nandlog_port_delay_ms(uint32_t milliseconds) __attribute__((weak));
void __wrap_nandlog_port_transfer_read(uint8_t command, const void *address, uint32_t address_length, void *read_buffer, uint32_t read_length);
void __wrap_nandlog_port_transfer_write(uint8_t command, const void *address, uint32_t address_length, const void *write_buffer, uint32_t write_length);
void __wrap_nandlog_port_delay_us(uint32_t microseconds);
void __wrap_nandlog_port_delay_ms(uint32_t milliseconds);
void __wrap_nandlog_port_log(const char *format, ...);

void __wrap_nandlog_port_transfer_read(uint8_t command, const void *address, uint32_t address_length, void *read_buffer, uint32_t read_length)
{
   if (command == CMD_READ)
      ops.bytes_in += read_length;
   else if (command == CMD_GET_FEATURE)
      ++ops.register_reads;
   const uint32_t start = hw_cycle_count();
   __real_nandlog_port_transfer_read(command, address, address_length, read_buffer, read_length);
   const uint32_t end = hw_cycle_count();
   ops.bus_cycles += (uint32_t)(end - start);
   trace_add(command, true, false, read_length, start, end);
}

void __wrap_nandlog_port_transfer_write(uint8_t command, const void *address, uint32_t address_length, const void *write_buffer, uint32_t write_length)
{
   switch (command)
   {
      case CMD_PAGE_DATA_READ:
         ++ops.latches;
         if (!part_ecc_on)
            ++ops.raw_latches;
         break;
      case CMD_PROGRAM_EXECUTE:
         ++ops.programs;
         break;
      case CMD_BLOCK_ERASE:
         ++ops.erases;
         break;
      case CMD_PROGRAM_DATA_LOAD:
      case CMD_PROGRAM_LOAD_RANDOM_DATA:
         ops.bytes_out += write_length;
         break;
      case CMD_SET_FEATURE:
         // Follow the ECC-enable bit, so a latch issued with ECC off -- the blank check -- is counted as raw
         ++ops.register_writes;
         if (address && address_length && write_buffer && write_length && (((const uint8_t*)address)[0] == SR2))
            part_ecc_on = (((const uint8_t*)write_buffer)[0] & SR2_ECC_ENABLE) != 0;
         break;
      default:
         break;
   }
   const uint32_t start = hw_cycle_count();
   __real_nandlog_port_transfer_write(command, address, address_length, write_buffer, write_length);
   const uint32_t end = hw_cycle_count();
   ops.bus_cycles += (uint32_t)(end - start);
   trace_add(command, false, false, write_length, start, end);
}

void __wrap_nandlog_port_delay_us(uint32_t microseconds)
{
   const uint32_t start = hw_cycle_count();
   __real_nandlog_port_delay_us(microseconds);
   const uint32_t end = hw_cycle_count();
   ops.delay_cycles += (uint32_t)(end - start);
   ++ops.delays;
   trace_add(0, false, true, microseconds, start, end);
}

void __wrap_nandlog_port_delay_ms(uint32_t milliseconds)
{
   const uint32_t start = hw_cycle_count();
   __real_nandlog_port_delay_ms(milliseconds);
   const uint32_t end = hw_cycle_count();
   ops.delay_cycles += (uint32_t)(end - start);
   ++ops.delays;
   trace_add(0, false, true, milliseconds * 1000u, start, end);
}

// The library's diagnostics go to SWO at 1 Mbaud, which costs ~1 ms a line -- and nandlog_init() writes one on
// every boot. While a measurement is running they are formatted into RAM instead and printed after it, so the
// figure is the library's and not the console's. This replaces the port's nandlog_port_log() outright, doing what
// it does (format, then print) without the print in the timed span
#define DEFERRED_LOG_LINES                          8
#define DEFERRED_LOG_BYTES                          160
static char deferred_log[DEFERRED_LOG_LINES][DEFERRED_LOG_BYTES];
static uint32_t deferred_log_count, deferred_log_dropped;
static bool defer_library_log;

void __wrap_nandlog_port_log(const char *format, ...)
{
   ++ops.log_lines;
   char immediate[DEFERRED_LOG_BYTES];
   char *message = immediate;
   if (defer_library_log)
   {
      if (deferred_log_count >= DEFERRED_LOG_LINES)
      {
         ++deferred_log_dropped;
         return;
      }
      message = deferred_log[deferred_log_count++];
   }
   va_list args;
   va_start(args, format);
   vsnprintf(message, DEFERRED_LOG_BYTES, format, args);
   va_end(args);
   if (!defer_library_log)
      print("%s", message);
}

static void release_library_log(bool show)
{
   for (uint32_t i = 0; show && (i < deferred_log_count); ++i)
      print("    [library] %s", deferred_log[i]);
   if (show && deferred_log_dropped)
      print("    [library] ... and %u more lines\n", deferred_log_dropped);
   deferred_log_count = deferred_log_dropped = 0;
}

// The bench's own register-level measurements bypass the wrapper when it is there, so they time the port alone
static void port_read(uint8_t command, const void *address, uint32_t address_length, void *buffer, uint32_t length)
{
   if (ops_live)
      __real_nandlog_port_transfer_read(command, address, address_length, buffer, length);
   else
      nandlog_port_transfer_read(command, address, address_length, buffer, length);
}

static void port_write(uint8_t command, const void *address, uint32_t address_length, const void *buffer, uint32_t length)
{
   if (ops_live)
      __real_nandlog_port_transfer_write(command, address, address_length, buffer, length);
   else
      nandlog_port_transfer_write(command, address, address_length, buffer, length);
}

static void detect_instrumentation(void)
{
   // A call from this file to each wrapped name lands in its wrapper only if the link wrapped it
   const bench_ops_t before = ops;
   uint8_t which = SR3, value = 0;
   nandlog_port_transfer_read(CMD_GET_FEATURE, &which, 1, &value, 1);
   ops_live = (ops.register_reads != before.register_reads);
   nandlog_port_delay_us(1);
   delay_live = (ops.delays != before.delays);
   nandlog_port_log("%s", "");
   log_live = (ops.log_lines != before.log_lines);
}

static void ops_difference(bench_ops_t *total, const bench_ops_t *before)
{
   total->latches += ops.latches - before->latches;
   total->raw_latches += ops.raw_latches - before->raw_latches;
   total->programs += ops.programs - before->programs;
   total->erases += ops.erases - before->erases;
   total->register_reads += ops.register_reads - before->register_reads;
   total->register_writes += ops.register_writes - before->register_writes;
   total->log_lines += ops.log_lines - before->log_lines;
   total->delays += ops.delays - before->delays;
   total->bytes_in += ops.bytes_in - before->bytes_in;
   total->bytes_out += ops.bytes_out - before->bytes_out;
   total->bus_cycles += ops.bus_cycles - before->bus_cycles;
   total->delay_cycles += ops.delay_cycles - before->delay_cycles;
}


// Distributions -------------------------------------------------------------------------------------------------------

typedef struct
{
   uint32_t n, kept, min, max;
   uint64_t sum;
   bool counted, sorted;
   bench_ops_t ops;                       // summed over the n operations
   uint32_t samples[DIST_SAMPLES];        // cycles
} dist_t;

static dist_t dist[5];
static uint32_t reservoir_state = 0x2545F491u;

static void dist_reset(dist_t *d)
{
   d->n = d->kept = d->max = 0;
   d->min = 0xFFFFFFFFu;
   d->sum = 0;
   d->counted = d->sorted = false;
   memset(&d->ops, 0, sizeof(d->ops));
}

static void dist_add(dist_t *d, uint32_t cycles, const bench_ops_t *before)
{
   ++d->n;
   d->sum += cycles;
   if (cycles < d->min)
      d->min = cycles;
   if (cycles > d->max)
      d->max = cycles;
   if (d->kept < DIST_SAMPLES)
      d->samples[d->kept++] = cycles;
   else
   {
      // Algorithm R: every sample so far has the same chance of being in the reservoir
      reservoir_state = (1664525u * reservoir_state) + 1013904223u;
      const uint32_t slot = (uint32_t)(((uint64_t)reservoir_state * d->n) >> 32);
      if (slot < DIST_SAMPLES)
         d->samples[slot] = cycles;
   }
   if (before)
   {
      ops_difference(&d->ops, before);
      d->counted = true;
   }
   d->sorted = false;
}

static uint32_t dist_percentile(dist_t *d, uint32_t percent)
{
   // Nearest rank over the kept samples
   if (!d->kept)
      return 0;
   if (!d->sorted)
   {
      for (uint32_t gap = d->kept / 2; gap; gap /= 2)
         for (uint32_t i = gap; i < d->kept; ++i)
         {
            const uint32_t value = d->samples[i];
            uint32_t j = i;
            for (; (j >= gap) && (d->samples[j - gap] > value); j -= gap)
               d->samples[j] = d->samples[j - gap];
            d->samples[j] = value;
         }
      d->sorted = true;
   }
   uint32_t rank = ((percent * d->kept) + 99) / 100;
   if (!rank)
      rank = 1;
   return d->samples[rank - 1];
}

#define TIMED(distribution, ...)  do {                                       \
      const bench_ops_t _before = ops;                                       \
      MARK_HIGH();                                                           \
      const uint32_t _start = hw_cycle_count();                              \
      __VA_ARGS__;                                                           \
      const uint32_t _end = hw_cycle_count();                                \
      MARK_LOW();                                                            \
      dist_add((distribution), _end - _start, &_before);                     \
      MARK_GAP();                                                            \
   } while (0)

static void line_mean(line_t *line, const char *key, uint64_t total, uint32_t n)
{
   // A per-operation mean with two decimals
   line_str(line, key);
   line_fixed(line, n ? ((total * 100) / n) : 0, 100, 2);
}

static void report(const char *name, dist_t *d, const char *extra)
{
   line_t line;
   line_reset(&line);
   line_str(&line, "BENCH ");
   line_str(&line, name);
   line_key_u32(&line, " n=", d->n);
   if (d->n)
   {
      line_str(&line, " min=");
      line_us(&line, d->min);
      line_str(&line, " med=");
      line_us(&line, dist_percentile(d, 50));
      line_str(&line, " p99=");
      line_us(&line, dist_percentile(d, 99));
      line_str(&line, " max=");
      line_us(&line, d->max);
      line_str(&line, " mean=");
      line_us(&line, d->sum / d->n);
   }
   const bool sampled = d->kept < d->n;
   if ((extra && extra[0]) || sampled)
   {
      line_str(&line, " extra=");
      if (extra && extra[0])
         line_str(&line, extra);
      if (sampled)
      {
         line_str(&line, (extra && extra[0]) ? ",sampled:" : "sampled:");
         line_u32(&line, d->kept);
      }
   }
   print("%s\n", line.text);

   if (ops_live && d->counted && d->n)
   {
      line_reset(&line);
      line_str(&line, "BENCH_COUNT ");
      line_str(&line, name);
      line_key_u32(&line, " n=", d->n);
      line_mean(&line, " reads=", d->ops.latches, d->n);
      line_mean(&line, " raw_reads=", d->ops.raw_latches, d->n);
      line_mean(&line, " programs=", d->ops.programs, d->n);
      line_mean(&line, " erases=", d->ops.erases, d->n);
      line_mean(&line, " polls=", d->ops.register_reads, d->n);
      line_mean(&line, " reg_writes=", d->ops.register_writes, d->n);
      line_mean(&line, " bytes_in=", d->ops.bytes_in, d->n);
      line_mean(&line, " bytes_out=", d->ops.bytes_out, d->n);
      line_str(&line, " bus_us=");
      line_us(&line, d->ops.bus_cycles / d->n);
      if (delay_live)
      {
         line_str(&line, " delay_us=");
         line_us(&line, d->ops.delay_cycles / d->n);
      }
      if (log_live)
         line_mean(&line, " log_lines=", d->ops.log_lines, d->n);
      print("%s\n", line.text);
   }
}

static void section(const char *title)
{
   const uint64_t elapsed_ms = cycles_to_ns(now_cycles()) / 1000000u;
   print("\n--- %s   [t=%u s] ---\n", title, (uint32_t)(elapsed_ms / 1000));
}

static void bench_error(const char *what)
{
   ++errors;
   print("BENCH_ERROR %s\n", what);
}


// A Minimal Command Layer, for What No Driver Function Exposes --------------------------------------------------------

static void page_address(uint32_t page, uint8_t *out)
{
   out[0] = (uint8_t)(page >> 16);
   out[1] = (uint8_t)(page >> 8);
   out[2] = (uint8_t)page;
}

static uint8_t get_feature(uint8_t which)
{
   uint8_t value = 0;
   port_read(CMD_GET_FEATURE, &which, 1, &value, 1);
   return value;
}

static void set_feature(uint8_t which, uint8_t value)
{
   port_write(CMD_SET_FEATURE, &which, 1, &value, 1);
   if (which == SR2)
      part_ecc_on = (value & SR2_ECC_ENABLE) != 0;
}

static bool wait_ready(uint8_t *status, uint32_t *polls)
{
   // Poll with no pacing at all, so the poll that first sees READY ends as soon after the part does as one status
   // read allows. Gives up after 100 ms, far beyond any operation this part performs
   const uint32_t start = hw_cycle_count(), limit = us_to_cycles(100000);
   do
   {
      const uint8_t value = get_feature(SR3);
      if (polls)
         ++*polls;
      if (!(value & SR_BUSY))
      {
         if (status)
            *status = value;
         return true;
      }
   } while ((uint32_t)(hw_cycle_count() - start) < limit);
   bench_error("the part never cleared BUSY within 100 ms");
   return false;
}

static void open_write_protection(void)
{
   nandlog_port_write_enable(true);
   set_feature(SR1, PROTECT_NONE);
}

static void close_write_protection(void)
{
   set_feature(SR1, PROTECT_ALL);
   nandlog_port_write_enable(false);
}

static bool manual_program(uint32_t page, const uint8_t *data, uint32_t *load_cycles, uint32_t *busy_cycles)
{
   // WRITE ENABLE + PROGRAM LOAD of the whole main array is the load; PROGRAM EXECUTE until READY is the busy time,
   // measured from the end of the EXECUTE transfer to the end of the first status read that reports READY
   static const uint8_t column[2] = { 0x00, 0x00 };
   uint8_t address[3], status = 0xFF;
   page_address(page, address);
   MARK_HIGH();
   const uint32_t t0 = hw_cycle_count();
   port_write(CMD_WRITE_ENABLE, NULL, 0, NULL, 0);
   port_write(CMD_PROGRAM_DATA_LOAD, column, sizeof(column), data, PAGE_BYTES);
   const uint32_t t1 = hw_cycle_count();
   port_write(CMD_PROGRAM_EXECUTE, NULL, 0, address, sizeof(address));
   const uint32_t t2 = hw_cycle_count();
   const bool ready = wait_ready(&status, NULL);
   const uint32_t t3 = hw_cycle_count();
   MARK_LOW();
   MARK_GAP();
   *load_cycles = t1 - t0;
   *busy_cycles = t3 - t2;
   return ready && !(status & SR_PROGRAM_FAILURE);
}

static bool manual_erase(uint32_t page, uint32_t *busy_cycles)
{
   uint8_t address[3], status = 0xFF;
   page_address(page & ~(PAGES_PER_BLOCK - 1), address);
   MARK_HIGH();
   port_write(CMD_WRITE_ENABLE, NULL, 0, NULL, 0);
   port_write(CMD_BLOCK_ERASE, NULL, 0, address, sizeof(address));
   const uint32_t t0 = hw_cycle_count();
   const bool ready = wait_ready(&status, NULL);
   *busy_cycles = hw_cycle_count() - t0;
   MARK_LOW();
   MARK_GAP();
   return ready && !(status & SR_ERASE_FAILURE);
}

static uint32_t polled_read_busy(uint32_t page, uint32_t *polls)
{
   // From the end of the PAGE DATA READ transfer to the end of the first status read that says READY. An UPPER bound
   // on the part's busy time, by at most about two status reads
   uint8_t address[3];
   page_address(page, address);
   MARK_HIGH();
   port_write(CMD_PAGE_DATA_READ, NULL, 0, address, sizeof(address));
   const uint32_t t0 = hw_cycle_count();
   wait_ready(NULL, polls);
   const uint32_t elapsed = hw_cycle_count() - t0;
   MARK_LOW();
   MARK_GAP();
   return elapsed;
}

static uint32_t bisected_read_busy(uint32_t page)
{
   // The smallest delay d after the PAGE DATA READ transfer at which ONE status read reports READY, found to 0.25 us
   // by bisection with a cycle-counted spin. The part's busy time tR then lies in (d - 0.25 us, d + one status read],
   // because the busy bit is sampled somewhere inside that status read
   uint8_t address[3];
   page_address(page, address);
   const uint32_t step = (cycles_per_us >= 4) ? (cycles_per_us / 4) : 1;
   uint32_t low = 0, high = us_to_cycles(2000);
   while ((high - low) > step)
   {
      const uint32_t mid = low + ((high - low) / 2);
      port_write(CMD_PAGE_DATA_READ, NULL, 0, address, sizeof(address));
      const uint32_t t0 = hw_cycle_count();
      while ((uint32_t)(hw_cycle_count() - t0) < mid)
         ;
      const bool busy = (get_feature(SR3) & SR_BUSY) != 0;
      wait_ready(NULL, NULL);
      if (busy)
         low = mid;
      else
         high = mid;
   }
   return high;
}


// Scratch Blocks and Marked Blocks ------------------------------------------------------------------------------------

// How far down the log region phase A may look for scratch blocks when the top of the reserve has too few good ones
#define SCRATCH_FALLBACK_BLOCKS                     64

static uint32_t scratch_pages[HW_SCRATCH_BLOCKS];
static bool scratch_in_log_region[HW_SCRATCH_BLOCKS];

static void halt(void);

static uint8_t raw_marker(uint32_t block)
{
   // The first spare byte of the block's first page, as the driver reads a mark. The caller turns ECC off
   const uint8_t column[3] = { (uint8_t)(PAGE_BYTES >> 8), (uint8_t)PAGE_BYTES, 0x00 };
   uint8_t address[3], marker = 0xFF;
   page_address(block * PAGES_PER_BLOCK, address);
   port_write(CMD_PAGE_DATA_READ, NULL, 0, address, sizeof(address));
   wait_ready(NULL, NULL);
   port_read(CMD_READ, column, sizeof(column), &marker, sizeof(marker));
   return marker;
}

static bool scratch_usable(uint32_t block)
{
   // Neither retired by the table nor carrying a mark: a block whose first page reads all 00h, as a dead one does,
   // carries one
   if (nandlog_chip_is_bad_block(block * PAGES_PER_BLOCK))
      return false;
   const uint8_t saved_sr2 = get_feature(SR2);
   set_feature(SR2, (uint8_t)(saved_sr2 & ~SR2_ECC_ENABLE));
   const uint8_t marker = raw_marker(block);
   set_feature(SR2, saved_sr2);
   return marker == 0xFF;
}

static void choose_scratch_blocks(void)
{
   // The top HW_SCRATCH_REGION_BLOCKS of the reserve first, as the other on-device tests use them. Where fewer than
   // HW_SCRATCH_BLOCKS of those are good and unmarked, the rest are the topmost good, unmarked blocks of the LOG
   // REGION. That is safe for this program alone: phase C erases the whole log region after phase A, and the log
   // never reaches the top of it here. The non-destructive tests never do this
   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   const uint32_t log_end = geometry->block_count - geometry->reserved_blocks;
   uint32_t found = 0, block = geometry->block_count;
   while ((found < HW_SCRATCH_BLOCKS) && (block-- > (geometry->block_count - HW_SCRATCH_REGION_BLOCKS)))
      if (scratch_usable(block))
      {
         scratch_in_log_region[found] = false;
         scratch_pages[found++] = block * PAGES_PER_BLOCK;
      }
   const uint32_t from_reserve = found;
   block = log_end;
   while ((found < HW_SCRATCH_BLOCKS) && (block-- > (log_end - SCRATCH_FALLBACK_BLOCKS)))
      if (scratch_usable(block))
      {
         scratch_in_log_region[found] = true;
         scratch_pages[found++] = block * PAGES_PER_BLOCK;
      }
   if (found < HW_SCRATCH_BLOCKS)
   {
      bench_error("fewer than four good, unmarked scratch blocks at the top of the reserve and of the log region");
      halt();
   }
   if (from_reserve < HW_SCRATCH_BLOCKS)
      print("  only %u good, unmarked scratch block(s) at the top of the reserve; the rest are the topmost good, unmarked\n"
            "  blocks of the log region, which phase C erases with the rest of the region\n", from_reserve);
   print("BENCH_INFO scratch_blocks=%u,%u,%u,%u from_reserve=%u from_log_region=%u\n", scratch_pages[0] / PAGES_PER_BLOCK,
         scratch_pages[1] / PAGES_PER_BLOCK, scratch_pages[2] / PAGES_PER_BLOCK, scratch_pages[3] / PAGES_PER_BLOCK,
         from_reserve, HW_SCRATCH_BLOCKS - from_reserve);
}

static void report_marked_blocks(const char *when)
{
   // Every block whose first spare byte reads other than FFh with ECC off -- a factory mark, the driver's, or a
   // block gone dead -- as a count, the lowest and highest, and the runs of consecutive blocks, so a marked region
   // that grows from one run of the bench to the next shows. One raw page latch per block, about 2 s in all
   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   const uint8_t saved_sr2 = get_feature(SR2);
   set_feature(SR2, (uint8_t)(saved_sr2 & ~SR2_ECC_ENABLE));
   const uint64_t start = now_cycles();
   line_t runs;
   line_reset(&runs);
   uint32_t count = 0, lowest = 0, highest = 0, run_count = 0, run_start = 0;
   bool in_run = false;
   for (uint32_t block = 0; block <= geometry->block_count; ++block)
   {
      const bool marked = (block < geometry->block_count) && (raw_marker(block) != 0xFF);
      if (marked)
      {
         lowest = count++ ? lowest : block;
         highest = block;
         run_start = in_run ? run_start : block;
         in_run = true;
      }
      else if (in_run)
      {
         in_run = false;
         if (run_count++ < 24)
         {
            line_str(&runs, (run_count > 1) ? "," : "");
            line_u32(&runs, run_start);
            if ((block - 1) != run_start)
            {
               line_str(&runs, "-");
               line_u32(&runs, block - 1);
            }
         }
         else if (run_count == 25)
            line_str(&runs, ",...");
      }
   }
   set_feature(SR2, saved_sr2);
   line_t line;
   line_reset(&line);
   line_str(&line, "BENCH_INFO marked_blocks when=");
   line_str(&line, when);
   line_key_u32(&line, " count=", count);
   if (count)
   {
      line_key_u32(&line, " lowest=", lowest);
      line_key_u32(&line, " highest=", highest);
      line_key_u32(&line, " run_count=", run_count);
      line_str(&line, " runs=");
      line_str(&line, runs.text);
   }
   line_key_u32(&line, " retired_in_table=", nandlog_bad_block_count());
   line_str(&line, " scan_ms=");
   line_ms(&line, now_cycles() - start);
   print("%s\n", line.text);
}


// CRC-32 --------------------------------------------------------------------------------------------------------------
//
// A REFERENCE, written here rather than taken from the library: the byte-a-step loop over a constant table in flash,
// which is what the library's software does with NANDLOG_CRC_TABLE_IN_RAM 0 and NANDLOG_CRC_SLICE_BY_4 0. Phase B
// times it beside the library's own routines and checks all of them against each other and known vectors, and phase E
// checks it against a page the library wrote. noinline so that the call measured is the whole function

__attribute__((noinline)) static uint32_t crc32_flash_reference(const void *data, uint32_t length)
{
   // Reflects IEEE 802.3 CRC-32 (poly 0xEDB88320, init and final XOR 0xFFFFFFFF). Byte-for-byte identical
   // to Python's zlib.crc32, so the host verifies device-computed values with no bespoke implementation
   static const uint32_t table[256] = {
      0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3,
      0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91,
      0x1DB71064, 0x6AB020F2, 0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
      0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5,
      0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172, 0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B,
      0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
      0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423, 0xCFBA9599, 0xB8BDA50F,
      0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D,
      0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
      0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01,
      0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457,
      0x65B0D9C6, 0x12B7E950, 0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
      0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB,
      0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0, 0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9,
      0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
      0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81, 0xB7BD5C3B, 0xC0BA6CAD,
      0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683,
      0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
      0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7,
      0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5,
      0xD6D6A3E8, 0xA1D1937E, 0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
      0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79,
      0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236, 0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F,
      0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
      0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F, 0x72076785, 0x05005713,
      0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21,
      0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
      0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45,
      0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB,
      0xAED16A4A, 0xD9D65ADC, 0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
      0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF,
      0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94, 0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
   };
   const uint8_t *bytes = (const uint8_t*)data;
   uint32_t crc = 0xFFFFFFFF;
   for (uint32_t i = 0; i < length; ++i)
      crc = (crc >> 8) ^ table[(crc ^ bytes[i]) & 0xFF];
   return ~crc;
}

// Whether the port's CRC is running on hardware, if the port can say (nandlog_port.h): optional, so referenced weakly
bool nandlog_port_crc32_hardware(void) __attribute__((weak));

static const char *crc_hardware_state(void)
{
   if (!NANDLOG_PORT_CRC32)
      return "none";
   if (!nandlog_port_crc32_hardware)
      return "unknown";
   return nandlog_port_crc32_hardware() ? "passed" : "failed";
}


// Phase A: The Part and the Bus ---------------------------------------------------------------------------------------

static void phase_a_primitives(void)
{
   section("A. Part and bus primitives, in four scratch blocks");
   hw_power_up();
   choose_scratch_blocks();
   const uint32_t s0 = scratch_pages[0], s1 = scratch_pages[1], s2 = scratch_pages[2], s3 = scratch_pages[3];
   print("  scratch blocks %u %u %u %u: s0 holds patterned pages, s1 takes program/erase rounds, s2 takes copies, s3 stays erased\n",
         s0 / PAGES_PER_BLOCK, s1 / PAGES_PER_BLOCK, s2 / PAGES_PER_BLOCK, s3 / PAGES_PER_BLOCK);
   line_t extra;

   // A1. The smallest transfer there is, through the port alone and (if linked) through the counting wrapper
   dist_reset(&dist[0]);
   for (uint32_t i = 0; i < 2000; ++i)
      TIMED(&dist[0], sink = get_feature(SR3));
   report("prim.status_read", &dist[0], NULL);
   if (ops_live)
   {
      uint8_t which = SR3, value = 0;
      dist_reset(&dist[0]);
      for (uint32_t i = 0; i < 2000; ++i)
         TIMED(&dist[0], nandlog_port_transfer_read(CMD_GET_FEATURE, &which, 1, &value, 1));
      report("prim.status_read.via_counting_wrapper", &dist[0], NULL);
   }

   for (uint32_t i = 0; i < HW_SCRATCH_BLOCKS; ++i)
      if (!nandlog_chip_erase_block(scratch_pages[i]))
         bench_error("a scratch block would not erase");

   // A2. Program through the driver, and erase a block that is fully programmed: 24 rounds over s1. The first 12
   // erase it by hand, so the busy time is isolated; the last 12 through the driver, as the log erases
   print("  24 rounds of: program all %u pages of s1, then erase it ...\n", PAGES_PER_BLOCK);
   dist_reset(&dist[0]);        // program, driver
   dist_reset(&dist[1]);        // erase busy, full block
   dist_reset(&dist[2]);        // erase, driver, full block
   uint32_t program_failures = 0;
   for (uint32_t round = 0; round < 24; ++round)
   {
      for (uint32_t p = 0; p < PAGES_PER_BLOCK; ++p)
      {
         hw_fill_pattern(data_buffer, PAGE_BYTES, 0x0BE70000u + (round * 64) + p);
         bool ok = false;
         TIMED(&dist[0], ok = nandlog_chip_write_page(data_buffer, s1 + p));
         program_failures += ok ? 0 : 1;
      }
      if (round < 12)
      {
         uint32_t busy = 0;
         open_write_protection();
         if (!manual_erase(s1, &busy))
            bench_error("manual erase of s1 failed");
         close_write_protection();
         dist_add(&dist[1], busy, NULL);
      }
      else
      {
         bool ok = false;
         TIMED(&dist[2], ok = nandlog_chip_erase_block(s1));
         if (!ok)
            bench_error("driver erase of s1 failed");
      }
      now_cycles();
   }
   line_reset(&extra);
   line_key_u32(&extra, "failures:", program_failures);
   report("prim.program.driver", &dist[0], extra.text);
   report("prim.erase.busy.full_block", &dist[1], "method:polled_from_command_end");
   report("prim.erase.driver.full_block", &dist[2], NULL);

   // A3. A program split into its two halves, by hand, on s2: load over the bus, then the part's busy time
   dist_reset(&dist[0]);
   dist_reset(&dist[1]);
   dist_reset(&dist[2]);
   for (uint32_t round = 0; round < 2; ++round)
   {
      open_write_protection();
      for (uint32_t p = 0; p < PAGES_PER_BLOCK; ++p)
      {
         uint32_t load = 0, busy = 0;
         hw_fill_pattern(data_buffer, PAGE_BYTES, 0x10AD0000u + (round * 64) + p);
         if (!manual_program(s2 + p, data_buffer, &load, &busy))
            bench_error("manual program failed");
         dist_add(&dist[0], load, NULL);
         dist_add(&dist[1], busy, NULL);
      }
      uint32_t busy = 0;
      if (!manual_erase(s2, &busy))
         bench_error("manual erase of s2 failed");
      close_write_protection();
   }
   report(name_of("prim.program.load_", PAGE_BYTES, "B"), &dist[0], "includes:write_enable");
   report("prim.program.busy", &dist[1], "method:polled_from_command_end");

   // ... and an erase of a block that is already erased, which is what the boot's window erase usually meets
   for (uint32_t round = 0; round < 32; ++round)
   {
      uint32_t busy = 0;
      open_write_protection();
      if (!manual_erase(s2, &busy))
         bench_error("manual erase of s2 failed");
      close_write_protection();
      dist_add(&dist[2], busy, NULL);
   }
   report("prim.erase.busy.empty_block", &dist[2], "method:polled_from_command_end");

   // A4. Patterned pages in s0 for everything that reads; s3 left erased
   for (uint32_t p = 0; p < PAGES_PER_BLOCK; ++p)
   {
      hw_fill_pattern(data_buffer, PAGE_BYTES, 0x5EED0000u + p);
      if (!nandlog_chip_write_page(data_buffer, s0 + p))
         bench_error("could not program s0");
   }

   // A5. Reads through the driver, programmed and erased
   dist_reset(&dist[0]);
   dist_reset(&dist[1]);
   for (uint32_t i = 0; i < 512; ++i)
      TIMED(&dist[0], sink = nandlog_chip_read_page_region(page_buffer, s0 + (i % PAGES_PER_BLOCK), 0, sizeof(nandlog_page_header_t)));
   for (uint32_t i = 0; i < 256; ++i)
      TIMED(&dist[1], sink = nandlog_chip_read_page_region(page_buffer, s3 + (i % PAGES_PER_BLOCK), 0, sizeof(nandlog_page_header_t)));
   report("prim.read.header_32B.programmed", &dist[0], "path:nandlog_chip_read_page_region");
   report("prim.read.header_32B.erased", &dist[1], "path:nandlog_chip_read_page_region");

   dist_reset(&dist[0]);
   dist_reset(&dist[1]);
   uint32_t wrong = 0;
   for (uint32_t i = 0; i < 512; ++i)
   {
      TIMED(&dist[0], sink = nandlog_chip_read_page(page_buffer, s0 + (i % PAGES_PER_BLOCK)));
      if ((i < PAGES_PER_BLOCK) && !hw_check_pattern(page_buffer, PAGE_BYTES, 0x5EED0000u + i))
         ++wrong;
   }
   for (uint32_t i = 0; i < 128; ++i)
      TIMED(&dist[1], sink = nandlog_chip_read_page(page_buffer, s3 + (i % PAGES_PER_BLOCK)));
   line_reset(&extra);
   line_key_u32(&extra, "wrong_pages:", wrong);
   report(name_of("prim.read.page_", PAGE_BYTES, "B.programmed"), &dist[0], extra.text);
   report(name_of("prim.read.page_", PAGE_BYTES, "B.erased"), &dist[1], NULL);
   if (wrong)
      bench_error("s0 did not read back as programmed");

   // A6. A page read taken apart: the latch and its BUSY poll alone, then clocking bytes out of a loaded register
   {
      static const uint8_t column_zero[3] = { 0, 0, 0 };
      uint8_t address[3];
      dist_reset(&dist[0]);
      dist_reset(&dist[1]);
      dist_reset(&dist[2]);
      uint32_t polls = 0;
      for (uint32_t i = 0; i < 512; ++i)
      {
         page_address(s0 + (i % PAGES_PER_BLOCK), address);
         TIMED(&dist[0], port_write(CMD_PAGE_DATA_READ, NULL, 0, address, sizeof(address)); wait_ready(NULL, &polls));
      }
      for (uint32_t i = 0; i < 512; ++i)
         TIMED(&dist[1], port_read(CMD_READ, column_zero, sizeof(column_zero), page_buffer, sizeof(nandlog_page_header_t)));
      for (uint32_t i = 0; i < 256; ++i)
         TIMED(&dist[2], port_read(CMD_READ, column_zero, sizeof(column_zero), page_buffer, PAGE_BYTES));
      line_reset(&extra);
      line_str(&extra, "polls_per_latch:");
      line_fixed(&extra, ((uint64_t)polls * 100) / 512, 100, 2);
      report("prim.latch_and_poll", &dist[0], extra.text);
      report("prim.xfer_out_32B", &dist[1], "path:port_READ_03h_from_loaded_register");
      line_reset(&extra);
      line_str(&extra, "path:port_READ_03h_from_loaded_register,effective_kBps:");
      const uint64_t median_ns = cycles_to_ns(dist_percentile(&dist[2], 50));
      line_u32(&extra, median_ns ? (uint32_t)(((uint64_t)PAGE_BYTES * 1000000u) / median_ns) : 0);
      report(name_of("prim.xfer_out_", PAGE_BYTES, "B"), &dist[2], extra.text);
   }

   // A7. The part's page-read busy time, ECC on and off, written and erased page, two ways
   {
      const uint8_t saved_sr2 = get_feature(SR2);
      const uint8_t ecc_off = (uint8_t)(saved_sr2 & ~SR2_ECC_ENABLE), ecc_on = (uint8_t)(saved_sr2 | SR2_ECC_ENABLE);
      print("  SR2 as configured by the driver: %02X (ECC %s)\n", saved_sr2, (saved_sr2 & SR2_ECC_ENABLE) ? "on" : "OFF");
      static const char *const names_poll[4] = { "prim.tR.poll.ecc_on.programmed", "prim.tR.poll.ecc_on.erased",
                                                 "prim.tR.poll.ecc_off.programmed", "prim.tR.poll.ecc_off.erased" };
      static const char *const names_bisect[4] = { "prim.tR.bisect.ecc_on.programmed", "prim.tR.bisect.ecc_on.erased",
                                                   "prim.tR.bisect.ecc_off.programmed", "prim.tR.bisect.ecc_off.erased" };
      for (uint32_t which = 0; which < 4; ++which)
      {
         set_feature(SR2, (which < 2) ? ecc_on : ecc_off);
         const uint32_t base = (which & 1) ? s3 : s0;
         uint32_t polls = 0;
         dist_reset(&dist[0]);
         dist_reset(&dist[1]);
         for (uint32_t i = 0; i < 256; ++i)
            dist_add(&dist[0], polled_read_busy(base + (i % PAGES_PER_BLOCK), &polls), NULL);
         for (uint32_t i = 0; i < 32; ++i)
            dist_add(&dist[1], bisected_read_busy(base + (i % PAGES_PER_BLOCK)), NULL);
         line_reset(&extra);
         line_str(&extra, "method:poll_from_command_end,bound:upper_by_<=2_status_reads,polls_per_read:");
         line_fixed(&extra, ((uint64_t)polls * 100) / 256, 100, 2);
         report(names_poll[which], &dist[0], extra.text);
         report(names_bisect[which], &dist[1], "method:bisect_0.25us,tR_in:(value-0.25us;value+1_status_read]");
      }
      set_feature(SR2, saved_sr2);
      const uint8_t restored = get_feature(SR2);
      print("  SR2 restored to %02X %s\n", restored, (restored == saved_sr2) ? "" : "(MISMATCH)");
      if (restored != saved_sr2)
         bench_error("SR2 was not restored");
   }

   // A7b. The latch to READY by access pattern, ECC as the driver sets it. A 32-byte read through the driver takes about
   // 270 us in all, yet A6's bare latch polled back to back takes about 290 us and A7's single status read after a quiet
   // wait finds the part busy for about 360 us. So: with or without 32 bytes clocked out between latches, polls back to
   // back or NANDLOG_BUSY_POLL_INTERVAL_US apart as the driver's are, over consecutive pages or pages 7 apart
   {
      static const uint8_t column_zero[3] = { 0, 0, 0 };
      static const char *const names[8] = {
         "prim.tR.pattern.no_readout.back_to_back.consecutive", "prim.tR.pattern.no_readout.back_to_back.stride7",
         "prim.tR.pattern.no_readout.paced.consecutive", "prim.tR.pattern.no_readout.paced.stride7",
         "prim.tR.pattern.readout.back_to_back.consecutive", "prim.tR.pattern.readout.back_to_back.stride7",
         "prim.tR.pattern.readout.paced.consecutive", "prim.tR.pattern.readout.paced.stride7" };
      for (uint32_t which = 0; which < 8; ++which)
      {
         const bool read_out = (which & 4) != 0, paced = (which & 2) != 0, strided = (which & 1) != 0;
         const uint32_t limit = us_to_cycles(100000);
         uint32_t polls = 0;
         uint8_t address[3];
         dist_reset(&dist[0]);
         for (uint32_t i = 0; i < 128; ++i)
         {
            page_address(s0 + ((strided ? (i * 7) : i) % PAGES_PER_BLOCK), address);
            port_write(CMD_PAGE_DATA_READ, NULL, 0, address, sizeof(address));
            const uint32_t t0 = hw_cycle_count();
            for (++polls; get_feature(SR3) & SR_BUSY; ++polls)
            {
               if ((uint32_t)(hw_cycle_count() - t0) >= limit)
               {
                  bench_error("the part never cleared BUSY within 100 ms");
                  break;
               }
               if (paced)
                  nandlog_port_delay_us(NANDLOG_BUSY_POLL_INTERVAL_US);
            }
            dist_add(&dist[0], hw_cycle_count() - t0, NULL);
            if (read_out)
               port_read(CMD_READ, column_zero, sizeof(column_zero), page_buffer, sizeof(nandlog_page_header_t));
         }
         line_reset(&extra);
         line_str(&extra, "method:poll_from_command_end,polls_per_read:");
         line_fixed(&extra, ((uint64_t)polls * 100) / 128, 100, 2);
         report(names[which], &dist[0], extra.text);
      }
   }

   // A8. The blank check the recovery uses: ECC off, the whole page and spare clocked out, every bit checked
   {
      uint32_t unexpected = 0;
      dist_reset(&dist[0]);
      dist_reset(&dist[1]);
      for (uint32_t i = 0; i < 128; ++i)
      {
         bool blank = false;
         TIMED(&dist[0], blank = nandlog_chip_page_is_blank(s3 + (i % PAGES_PER_BLOCK)));
         unexpected += blank ? 0 : 1;
      }
      line_reset(&extra);
      line_key_u32(&extra, "said_not_blank:", unexpected);
      report("prim.blank_check.erased_page", &dist[0], extra.text);
      unexpected = 0;
      for (uint32_t i = 0; i < 128; ++i)
      {
         bool blank = true;
         TIMED(&dist[1], blank = nandlog_chip_page_is_blank(s0 + (i % PAGES_PER_BLOCK)));
         unexpected += blank ? 1 : 0;
      }
      line_reset(&extra);
      line_key_u32(&extra, "said_blank:", unexpected);
      report("prim.blank_check.programmed_page", &dist[1], extra.text);
   }

   // A9. Relocating a page: inside the part, against a read into RAM and a program back out
   {
      dist_reset(&dist[0]);
      dist_reset(&dist[1]);
      uint32_t copy_failures = 0, copy_wrong = 0;
      for (uint32_t round = 0; round < 2; ++round)
      {
         if (!nandlog_chip_erase_block(s2))
            bench_error("could not erase s2");
         for (uint32_t p = 0; p < PAGES_PER_BLOCK; ++p)
         {
            nandlog_copy_result_t result = NANDLOG_COPY_FAILED;
            TIMED(&dist[0], result = nandlog_chip_copy_page(s0 + p, s2 + p));
            copy_failures += (result == NANDLOG_COPY_OK) ? 0 : 1;
         }
         for (uint32_t p = 0; p < PAGES_PER_BLOCK; p += 21)
            if (!nandlog_chip_read_page(page_buffer, s2 + p) || !hw_check_pattern(page_buffer, PAGE_BYTES, 0x5EED0000u + p))
               ++copy_wrong;
         if (!nandlog_chip_erase_block(s2))
            bench_error("could not erase s2");
         for (uint32_t p = 0; p < PAGES_PER_BLOCK; ++p)
            TIMED(&dist[1], sink = nandlog_chip_read_page(data_buffer, s0 + p); sink = nandlog_chip_write_page(data_buffer, s2 + p));
         now_cycles();
      }
      line_reset(&extra);
      line_key_u32(&extra, "not_ok:", copy_failures);
      line_key_u32(&extra, ",spot_checks_wrong:", copy_wrong);
      line_key_u32(&extra, ",NANDLOG_CHIP_PAGE_COPY:", (uint32_t)NANDLOG_CHIP_PAGE_COPY);
      report("prim.copy.internal", &dist[0], extra.text);
      report("prim.copy.read_then_write", &dist[1], NULL);
      if (copy_failures || copy_wrong)
         bench_error("the internal page copy did not reproduce its source (see test_nandlog_page_copy)");
   }
}


// Phase B: CRC-32 -----------------------------------------------------------------------------------------------------

static uint32_t crc_vector_checks, crc_vector_failures;

static void crc_vector(const char *what, uint32_t got, uint32_t expected)
{
   ++crc_vector_checks;
   if (got != expected)
   {
      ++crc_vector_failures;
      print("BENCH_ERROR crc vector %s: %08X, expected %08X\n", what, got, expected);
      ++errors;
   }
}

static void phase_b_crc(void)
{
   section("B. CRC-32: the library's software as built, nandlog_crc32() (the port's when NANDLOG_PORT_CRC32), a flash-table reference");
   const uint32_t payload_bytes = nandlog_data_bytes_per_page();
   print("BENCH_INFO crc_hook=%u crc_hw_selftest=%s crc_table_in_ram=%u crc_slice_by_4=%u\n", (uint32_t)NANDLOG_PORT_CRC32,
         crc_hardware_state(), (uint32_t)NANDLOG_CRC_TABLE_IN_RAM, (uint32_t)NANDLOG_CRC_SLICE_BY_4);

   // Known vectors: zlib's check value at every alignment, and a payload whole and continued from odd splits, through
   // the CRC the log uses, the library's software and the reference, every one of which has to give the same answer
   static const char check[] = "123456789";
   for (uint32_t align = 0; align < 4; ++align)
   {
      memcpy(data_buffer + align, check, 9);
      crc_vector("\"123456789\" nandlog_crc32", nandlog_crc32(0, data_buffer + align, 9), 0xCBF43926u);
      crc_vector("\"123456789\" software", nandlog_crc32_software(0, data_buffer + align, 9), 0xCBF43926u);
   }
   crc_vector("\"123456789\" reference", crc32_flash_reference(check, 9), 0xCBF43926u);
   hw_fill_pattern(data_buffer, payload_bytes + 3, 0xC3C3C3C3u);
   for (uint32_t align = 0; align < 4; ++align)
   {
      const uint8_t *bytes = data_buffer + align;
      const uint32_t whole = crc32_flash_reference(bytes, payload_bytes);
      crc_vector("payload nandlog_crc32", nandlog_crc32(0, bytes, payload_bytes), whole);
      crc_vector("payload software", nandlog_crc32_software(0, bytes, payload_bytes), whole);
      static const uint32_t splits[] = { 1, 3, 28, 63, 64, 65, 1001, 2047, 4063 };
      for (uint32_t i = 0; i < (sizeof(splits) / sizeof(splits[0])); ++i)
      {
         const uint32_t at = splits[i] % payload_bytes;
         crc_vector("payload continued", nandlog_crc32(nandlog_crc32(0, bytes, at), bytes + at, payload_bytes - at), whole);
      }
   }
   print("BENCH_INFO crc_vectors_ok=%u checked=%u failed=%u\n", crc_vector_failures ? 0 : 1, crc_vector_checks, crc_vector_failures);

   // Timings: the library's software as built, the CRC the log uses, and the reference, each over a page payload and a
   // page header from word-aligned buffers in RAM, as the log's own are
   hw_fill_pattern(data_buffer, payload_bytes, 0xC3C3C3C3u);
   line_t extra, payload_extra;
   for (uint32_t which = 0; which < 3; ++which)
   {
      dist_reset(&dist[0]);
      dist_reset(&dist[1]);
      for (uint32_t i = 0; i < 1000; ++i)
      {
         if (which == 0)
            TIMED(&dist[0], sink = nandlog_crc32_software(0, data_buffer, payload_bytes));
         else if (which == 1)
            TIMED(&dist[0], sink = nandlog_crc32(0, data_buffer, payload_bytes));
         else
            TIMED(&dist[0], sink = crc32_flash_reference(data_buffer, payload_bytes));
      }
      for (uint32_t i = 0; i < 10000; ++i)
      {
         if (which == 0)
            TIMED(&dist[1], sink = nandlog_crc32_software(0, data_buffer, NANDLOG_PAGE_HEADER_CRC_BYTES));
         else if (which == 1)
            TIMED(&dist[1], sink = nandlog_crc32(0, data_buffer, NANDLOG_PAGE_HEADER_CRC_BYTES));
         else
            TIMED(&dist[1], sink = crc32_flash_reference(data_buffer, NANDLOG_PAGE_HEADER_CRC_BYTES));
      }
      static const char *const payload_names[] = { "crc.payload_", "crc.hook.payload_", "crc.flash_table.payload_" };
      static const char *const header_names[] = { "crc.header_", "crc.hook.header_", "crc.flash_table.header_" };
      line_reset(&extra);
      if (which == 0)
      {
         line_key_u32(&extra, "code:software,table_in_ram:", (uint32_t)NANDLOG_CRC_TABLE_IN_RAM);
         line_key_u32(&extra, ",slice_by_4:", (uint32_t)NANDLOG_CRC_SLICE_BY_4);
      }
      else if (which == 1)
      {
         line_str(&extra, NANDLOG_PORT_CRC32 ? "code:nandlog_port_crc32,hw_selftest:" : "code:software,hw_selftest:");
         line_str(&extra, crc_hardware_state());
      }
      else
         line_str(&extra, "code:reference_flash_table");
      line_reset(&payload_extra);
      line_key_u32(&payload_extra, "bytes:", payload_bytes);
      line_str(&payload_extra, ",");
      line_str(&payload_extra, extra.text);
      report(name_of(payload_names[which], payload_bytes, "B"), &dist[0], payload_extra.text);
      report(name_of(header_names[which], (uint32_t)NANDLOG_PAGE_HEADER_CRC_BYTES, "B"), &dist[1], extra.text);
   }
}


// Shared Library-Level Helpers ----------------------------------------------------------------------------------------

static void halt(void)
{
   print("BENCH_DONE errors=%u\n", errors);
   while (true)
      am_hal_delay_us(1000000);
}

static void measure_init(const char *name, const char *extra)
{
   // Warm boots: release the port, then time nandlog_init() from cold RAM state, INIT_REPS times. The library's own
   // boot line is printed for the first repetition only
   dist_reset(&dist[0]);
   for (uint32_t rep = 0; rep < INIT_REPS; ++rep)
   {
      nandlog_deinit();
      bool ok = false;
      defer_library_log = log_live;
      TIMED(&dist[0], ok = nandlog_init());
      defer_library_log = false;
      release_library_log(rep == 0);
      if (!ok)
      {
         bench_error("nandlog_init() failed during a timed warm boot");
         halt();
      }
   }
   hw_power_up();          // the init just timed left the part asleep and no session open
   report(name, &dist[0], extra);
}

static void trace_boot(const char *name)
{
   // One more warm boot, keeping the widest stretches of CPU time between two port calls (BENCH_GAP, widest first;
   // before_event is the port call that ended each one), then the totals (BENCH_GAP_SUM; tail_us is after the last)
   nandlog_deinit();
   memset(wide_gaps, 0, sizeof(wide_gaps));
   gaps_events = gaps_cpu = gaps_port = gaps_delay = 0;
   defer_library_log = log_live;
   const uint32_t start = hw_cycle_count();
   gaps_last_end = start;
   gaps_on = true;
   const bool ok = nandlog_init();
   gaps_on = false;
   const uint32_t end = hw_cycle_count();
   defer_library_log = false;
   release_library_log(false);
   if (!ok)
   {
      bench_error("nandlog_init() failed during a traced warm boot");
      halt();
   }
   hw_power_up();
   for (uint32_t i = 0; (i < WIDE_GAPS) && wide_gaps[i].gap; ++i)
   {
      const wide_gap_t *gap = &wide_gaps[i];
      line_t line;
      line_reset(&line);
      line_str(&line, "BENCH_GAP ");
      line_str(&line, name);
      line_key_u32(&line, " rank=", i);
      line_str(&line, " gap_us=");
      line_us(&line, gap->gap);
      line_key_u32(&line, " before_event=", gap->index);
      line_str(&line, gap->is_delay ? " delay" : (gap->is_read ? " read cmd=" : " write cmd="));
      if (!gap->is_delay)
         line_u32(&line, gap->command);
      line_key_u32(&line, gap->is_delay ? " us=" : " len=", gap->length);
      print("%s\n", line.text);
   }
   line_t line;
   line_reset(&line);
   line_str(&line, "BENCH_GAP_SUM ");
   line_str(&line, name);
   line_str(&line, " total_us=");
   line_us(&line, end - start);
   line_str(&line, " port_us=");
   line_us(&line, gaps_port);
   line_str(&line, " delay_us=");
   line_us(&line, gaps_delay);
   line_str(&line, " cpu_between_us=");
   line_us(&line, gaps_cpu);
   line_str(&line, " tail_us=");
   line_us(&line, end - gaps_last_end);
   line_key_u32(&line, " events=", gaps_events);
   print("%s\n", line.text);
}

static void measure_boot_parts(void)
{
   // C2. The pieces of a warm boot that are not the log's own logic, each alone: the port's nandlog_port_init(), a
   // power-down and power-up of the port, and the driver's nandlog_chip_init(); then one boot traced for its widest
   // stretches of CPU time between port calls
   nandlog_deinit();
   dist_reset(&dist[0]);
   for (uint32_t rep = 0; rep < INIT_REPS; ++rep)
   {
      TIMED(&dist[0], sink = nandlog_port_init());
      nandlog_port_deinit();
   }
   report("init.part.port_init", &dist[0], "path:nandlog_port_init");
   nandlog_port_init();
   dist_reset(&dist[0]);
   for (uint32_t rep = 0; rep < INIT_REPS; ++rep)
      TIMED(&dist[0], nandlog_port_power(false); nandlog_port_power(true));
   report("init.part.port_power_cycle", &dist[0], "path:nandlog_port_power(false)+nandlog_port_power(true)");
   dist_reset(&dist[0]);
   for (uint32_t rep = 0; rep < INIT_REPS; ++rep)
      TIMED(&dist[0], nandlog_chip_init());
   report("init.part.chip_init", &dist[0], "path:nandlog_chip_init");
   nandlog_port_deinit();
   if (!nandlog_init())
   {
      bench_error("nandlog_init() failed after timing the boot's parts");
      halt();
   }
   hw_power_up();
}


// Phase C: Wipe, and a Boot With No Epoch -----------------------------------------------------------------------------

static void phase_c_wipe(void)
{
   section("C. Wiping the metadata ring and the whole log region (DESTRUCTIVE)");
   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   const uint32_t blocks = geometry->block_count - geometry->reserved_blocks;
   print("  erasing blocks 0 .. %u through the driver, skipping retired ones ...\n", blocks - 1);
   hw_power_up();
   dist_reset(&dist[0]);
   uint32_t skipped = 0, failed = 0;
   const uint64_t start = now_cycles();
   for (uint32_t block = 0; block < blocks; ++block)
   {
      const uint32_t page = block * geometry->pages_per_block;
      if (nandlog_chip_is_bad_block(page))
      {
         ++skipped;
         continue;
      }
      bool ok = false;
      TIMED(&dist[0], ok = nandlog_chip_erase_block(page));
      failed += ok ? 0 : 1;
      if (!(block % 512))
      {
         now_cycles();
         print("  ... block %u\n", block);
      }
   }
   line_t extra;
   line_reset(&extra);
   line_key_u32(&extra, "retired_skipped:", skipped);
   line_key_u32(&extra, ",failed:", failed);
   line_str(&extra, ",total_ms:");
   line_ms(&extra, now_cycles() - start);
   report("region.erase.driver", &dist[0], extra.text);
   if (failed)
      bench_error("some log-region blocks refused their erase; the log will retire them when it meets them");

   measure_init("init.no_epoch", "ring:empty,region:erased");
   measure_boot_parts();
   trace_boot("init.no_epoch");
}


// Phase D: The Metadata Ring ------------------------------------------------------------------------------------------

typedef struct __attribute__ ((__packed__))
{
   uint32_t magic, index;
   uint8_t padding[24];
} bench_details_t;

static uint32_t epochs_begun;

static bool begin_epoch_timed(dist_t *d)
{
   bench_details_t details;
   memset(&details, 0, sizeof(details));
   details.magic = 0x48434E42u;     // 'BNCH'
   details.index = epochs_begun;
   bool ok = false;
   TIMED(d, ok = nandlog_begin_epoch(&details, sizeof(details)));
   if (ok)
      ++epochs_begun;
   return ok;
}

static void phase_d_ring(void)
{
   section("D. Warm boot against the metadata ring (1, 64, 511, 600 epochs; each epoch empty), and begin_epoch()");
   static const uint32_t checkpoints[4] = { 1, 64, 511, 600 };
   dist_t *same_block = &dist[1], *new_block = &dist[2];
   dist_reset(same_block);
   dist_reset(new_block);
   for (uint32_t c = 0; c < 4; ++c)
   {
      while (epochs_begun < checkpoints[c])
      {
         // The ring was wiped, so the k-th epoch (from 0) lands in slot k mod 256: every 32nd enters a new ring block
         const bool enters_block = (epochs_begun % RING_BLOCK_SLOTS) == 0;
         if (!begin_epoch_timed(enters_block ? new_block : same_block))
         {
            bench_error("begin_epoch() failed");
            return;
         }
         if (!(epochs_begun % 100))
         {
            now_cycles();
            print("  ... %u epochs\n", epochs_begun);
         }
      }
      measure_init(name_of("init.ring", checkpoints[c], ".len0"), "epoch_pages:0");
      if (!c)
         trace_boot(name_of("init.ring", checkpoints[c], ".len0"));
   }
   report("begin_epoch.same_ring_block", same_block, "previous_epoch:empty");
   report("begin_epoch.enters_ring_block", new_block, "previous_epoch:empty,includes:ring_block_erase");
}


// Phase E: Epoch Length, and Every Read Path --------------------------------------------------------------------------

static uint32_t pages_filled;        // in the length-test epoch; page p carries one record stamped 10 * (p + 1)

static bool fill_pages(uint32_t target)
{
   // One record that exactly fills a page, then flush(false), which commits the full page at once and leaves the
   // next store_record() free to carry the erase-ahead, as an application's idle flush would
   const uint32_t record_bytes = nandlog_data_bytes_per_page() - (NANDLOG_RECORD_FRAMING ? NANDLOG_FRAMING_LENGTH_BYTES : 0) - 5;
   nandlog_stats_t before, after;
   nandlog_get_stats(&before);
   const uint32_t first = pages_filled;
   print("  filling the epoch from %u to %u pages ...\n", pages_filled, target);
   hw_fill_pattern(data_buffer, record_bytes, 0xF111F111u);
   while (pages_filled < target)
   {
      memcpy(data_buffer, &pages_filled, sizeof(pages_filled));
      const nandlog_status_t status = nandlog_store_record(0x46, 10 * (pages_filled + 1), data_buffer, record_bytes);
      if ((status != NANDLOG_OK) || (nandlog_flush(false) != NANDLOG_OK))
      {
         bench_error("store_record()/flush() refused while filling");
         return false;
      }
      ++pages_filled;
      if (!(pages_filled % 1000))
      {
         now_cycles();
         print("  ... %u pages\n", pages_filled);
      }
   }
   nandlog_flush(true);
   nandlog_get_stats(&after);
   if ((after.pages_committed - before.pages_committed) != (pages_filled - first))
   {
      bench_error("the fill did not commit exactly one page per record");
      return false;
   }
   return true;
}

static void verify_crc_copy(void)
{
   // The newest page the library wrote, its header and payload checked with the bench's own reference CRC
   nandlog_page_header_t header;
   bool end_of_epoch = false;
   memset(&header, 0, sizeof(header));
   const uint32_t length = nandlog_read_recent_page(0, page_buffer, &header, &end_of_epoch);
   const bool payload_ok = length && (crc32_flash_reference(page_buffer, length) == header.payload_crc);
   const bool header_ok = crc32_flash_reference(&header, NANDLOG_PAGE_HEADER_CRC_BYTES) == header.header_crc;
   print("BENCH_INFO crc_copy_matches_library=%u payload_bytes=%u\n", (payload_ok && header_ok) ? 1 : 0, length);
   if (!payload_ok || !header_ok)
      bench_error("the reference CRC does not agree with the library on a page it wrote");
}

static void measure_reads(uint32_t length)
{
   // The current epoch holds exactly 'length' pages, page p stamped 10 * (p + 1); nothing is buffered
   const uint32_t end_bound = 10 * length, mid_bound = 10 * ((length / 2) + 1);
   const uint32_t bounded_reps = (length <= 1000) ? 10 : 3;
   line_t extra;
   uint32_t pages = 0;

   dist_reset(&dist[0]);
   for (uint32_t i = 0; i < 100; ++i)
   {
      TIMED(&dist[0], nandlog_begin_reading(0, 0));
      nandlog_end_reading();
   }
   report(name_of("begin_reading.unbounded.len", length, ""), &dist[0], NULL);

   // An end bound resolves by reading every header in the epoch
   dist_reset(&dist[0]);
   for (uint32_t i = 0; i < bounded_reps; ++i)
   {
      TIMED(&dist[0], nandlog_begin_reading(0, end_bound));
      nandlog_read_span(&pages, NULL);
      nandlog_end_reading();
      now_cycles();
   }
   line_reset(&extra);
   line_key_u32(&extra, "headers_read:", length);
   line_key_u32(&extra, ",span_pages:", pages);
   line_str(&extra, ",per_header_us:");
   line_us(&extra, dist_percentile(&dist[0], 50) / length);
   report(name_of("begin_reading.end_bound.len", length, ""), &dist[0], extra.text);
   if (pages != length)
      bench_error("an end-bounded read did not span the whole epoch");

   // A start bound stops at the first page that reaches it: here the middle of the epoch
   dist_reset(&dist[0]);
   for (uint32_t i = 0; i < bounded_reps; ++i)
   {
      TIMED(&dist[0], nandlog_begin_reading(mid_bound, 0));
      nandlog_read_span(&pages, NULL);
      nandlog_end_reading();
      now_cycles();
   }
   line_reset(&extra);
   line_key_u32(&extra, "headers_read:", (length / 2) + 1);
   line_key_u32(&extra, ",span_pages:", pages);
   line_str(&extra, ",per_header_us:");
   line_us(&extra, dist_percentile(&dist[0], 50) / ((length / 2) + 1));
   report(name_of("begin_reading.start_bound_mid.len", length, ""), &dist[0], extra.text);
   if (pages != (length - (length / 2)))
      bench_error("a start-bounded read spanned the wrong number of pages");

   // read_span() with an exact byte total, which reads every page in the span
   if (length <= 1000)
   {
      uint32_t bytes = 0;
      dist_reset(&dist[0]);
      for (uint32_t i = 0; i < 3; ++i)
      {
         nandlog_begin_reading(0, 0);
         TIMED(&dist[0], nandlog_read_span(&pages, &bytes));
         nandlog_end_reading();
         now_cycles();
      }
      line_reset(&extra);
      line_key_u32(&extra, "pages:", pages);
      line_key_u32(&extra, ",bytes:", bytes);
      report(name_of("read_span.with_bytes.len", length, ""), &dist[0], extra.text);
   }

   // Every page of the epoch, in order
   {
      nandlog_page_header_t header;
      uint32_t wrong = 0;
      uint64_t bytes = 0;
      dist_reset(&dist[0]);
      nandlog_begin_reading(0, 0);
      const uint64_t start = now_cycles();
      for (uint32_t i = 0; i < length; ++i)
      {
         uint32_t got = 0;
         TIMED(&dist[0], got = nandlog_retrieve_next_page(page_buffer, &header));
         bytes += got;
         if ((got != nandlog_data_bytes_per_page()) || (header.seq != i))
            ++wrong;
         if (!(i % 1024))
            now_cycles();
      }
      const uint64_t total = now_cycles() - start;
      nandlog_end_reading();
      line_reset(&extra);
      line_key_u32(&extra, "wrong:", wrong);
      line_str(&extra, ",total_ms:");
      line_ms(&extra, total);
      line_str(&extra, ",payload_kBps_incl_loop:");
      const uint64_t total_ns = cycles_to_ns(total);
      line_u32(&extra, total_ns ? (uint32_t)((bytes * 1000000u) / total_ns) : 0);
      report(name_of("retrieve_next_page.len", length, ""), &dist[0], extra.text);
      if (wrong)
         bench_error("retrieve_next_page() returned pages that are not the ones written");
   }

   // Pages by sequence number, scattered over the epoch
   {
      nandlog_page_header_t header;
      uint32_t wrong = 0, state = 0x5EC5EC5Eu;
      dist_reset(&dist[0]);
      nandlog_begin_reading(0, 0);
      for (uint32_t i = 0; i < 500; ++i)
      {
         state = (1664525u * state) + 1013904223u;
         const uint32_t seq = (uint32_t)(((uint64_t)state * length) >> 32);
         uint32_t got = 0;
         TIMED(&dist[0], got = nandlog_retrieve_page_by_seq(seq, page_buffer, &header));
         if ((got != nandlog_data_bytes_per_page()) || (header.seq != seq))
            ++wrong;
      }
      nandlog_end_reading();
      line_reset(&extra);
      line_key_u32(&extra, "wrong:", wrong);
      report(name_of("retrieve_page_by_seq.len", length, ""), &dist[0], extra.text);
      if (wrong)
         bench_error("retrieve_page_by_seq() returned the wrong page");
   }
}

static void phase_e_lengths(void)
{
   section("E. Warm boot and every read path against the length of the current epoch (ring: 600 epochs)");
   static const uint32_t lengths[2] = { 1000, 10000 };
   for (uint32_t i = 0; i < 2; ++i)
   {
      if (!fill_pages(lengths[i]))
         return;
      if (!i)
         verify_crc_copy();
      measure_init(name_of("init.ring600.len", lengths[i], ""), name_of("epoch_pages:", lengths[i], ""));
      measure_reads(lengths[i]);
   }
}


// Phase F: store_record() and flush() ---------------------------------------------------------------------------------

typedef enum
{
   CALL_BUFFER,         // copied into RAM, nothing reached the part
   CALL_COMMIT,         // committed the page that the record did not fit in
   CALL_ERASE_AHEAD,    // committed nothing, and erased the next block ahead of the write head
   CALL_COMMIT_ERASE,   // committed AND erased: the head entered a block the window had not reached
   CALL_CLASSES
} call_class_t;

static call_class_t classify(uint32_t cycles, const bench_ops_t *before, uint32_t committed_before)
{
   if (ops_live)
   {
      const bool programmed = ops.programs != before->programs, erased = ops.erases != before->erases;
      return programmed ? (erased ? CALL_COMMIT_ERASE : CALL_COMMIT) : (erased ? CALL_ERASE_AHEAD : CALL_BUFFER);
   }

   // Without the op counters, a commit still shows in the library's own statistics; an erase can only be inferred
   nandlog_stats_t stats;
   nandlog_get_stats(&stats);
   if (stats.pages_committed != committed_before)
      return CALL_COMMIT;
   return (cycles > us_to_cycles(ERASE_INFERRED_ABOVE_US)) ? CALL_ERASE_AHEAD : CALL_BUFFER;
}

static void phase_f_store(void)
{
   section("F. store_record() and flush(), outside a session, as an application logs");
   static const uint32_t sizes[3] = { 32, 256, 1024 };
   static const char *const class_names[CALL_CLASSES] = { "B.buffer_only", "B.commit", "B.erase_ahead", "B.commit_and_erase" };
   uint32_t worst_cycles = 0, worst_size = 0;
   line_t extra;

   // A fresh epoch for this, begun after the 10,000-page one: the steady-state begin_epoch(), once
   hw_power_up();
   dist_reset(&dist[0]);
   if (!begin_epoch_timed(&dist[0]))
   {
      bench_error("begin_epoch() failed");
      return;
   }
   report("begin_epoch.after_10000_page_epoch", &dist[0], "previous_epoch:10000_pages,nothing_buffered");
   hw_power_down();
   hw_fill_pattern(data_buffer, 1024, 0x57025702u);

   for (uint32_t s = 0; s < 3; ++s)
   {
      const uint32_t size = sizes[s];
      const uint32_t per_page = nandlog_data_bytes_per_page() / ((NANDLOG_RECORD_FRAMING ? NANDLOG_FRAMING_LENGTH_BYTES : 0) + 5 + size);
      print("  %u-byte records (%u per page) until %u pages have been committed ...\n", size, per_page, (uint32_t)STORE_PAGES_PER_SIZE);
      for (uint32_t c = 0; c <= CALL_CLASSES; ++c)
         dist_reset(&dist[c]);
      nandlog_stats_t stats;
      nandlog_get_stats(&stats);
      uint32_t committed = stats.pages_committed, commits = 0, timestamp = 0;
      while (commits < STORE_PAGES_PER_SIZE)
      {
         const bench_ops_t before = ops;
         nandlog_status_t status = NANDLOG_OK;
         MARK_HIGH();
         const uint32_t start = hw_cycle_count();
         status = nandlog_store_record(0x53, ++timestamp, data_buffer, size);
         const uint32_t cycles = hw_cycle_count() - start;
         MARK_LOW();
         if (status != NANDLOG_OK)
         {
            bench_error("store_record() refused a record");
            break;
         }
         const call_class_t which = classify(cycles, &before, committed);
         dist_add(&dist[which], cycles, &before);
         dist_add(&dist[CALL_CLASSES], cycles, &before);
         if ((which == CALL_COMMIT) || (which == CALL_COMMIT_ERASE))
         {
            ++commits;
            ++committed;
         }
         if (cycles > worst_cycles)
         {
            worst_cycles = cycles;
            worst_size = size;
         }
         MARK_GAP();
         if (!(timestamp & 1023))
            now_cycles();
      }
      nandlog_flush(true);
      for (uint32_t c = 0; c < CALL_CLASSES; ++c)
         report(name_of("store.", size, class_names[c]), &dist[c], ops_live ? "classified_by:op_counts" : "classified_by:stats+time");
      line_reset(&extra);
      line_key_u32(&extra, "records_per_page:", per_page);
      line_key_u32(&extra, ",pages:", commits);
      report(name_of("store.", size, "B.all_calls"), &dist[CALL_CLASSES], extra.text);
   }
   line_reset(&extra);
   line_str(&extra, "BENCH_INFO store.worst_single_call_us=");
   line_us(&extra, worst_cycles);
   line_key_u32(&extra, " record_bytes=", worst_size);
   print("%s\n", extra.text);

   // flush(true) of a partial page, and the idle flush(false) that carries the erase-ahead
   dist_reset(&dist[0]);        // flush(true), partial page
   dist_reset(&dist[1]);        // flush(false) that erased
   dist_reset(&dist[2]);        // flush(false) that did nothing
   uint32_t timestamp = 1;
   for (uint32_t i = 0; i < 64; ++i)
   {
      for (uint32_t r = 0; r < 4; ++r)
         nandlog_store_record(0x50, ++timestamp, data_buffer, 32);
      for (uint32_t r = 0; r < 2; ++r)
      {
         nandlog_stats_t stats;
         nandlog_get_stats(&stats);
         const bench_ops_t before = ops;
         MARK_HIGH();
         const uint32_t start = hw_cycle_count();
         nandlog_flush(false);
         const uint32_t cycles = hw_cycle_count() - start;
         MARK_LOW();
         const call_class_t which = classify(cycles, &before, stats.pages_committed);
         dist_add((which == CALL_ERASE_AHEAD) ? &dist[1] : &dist[2], cycles, &before);
         MARK_GAP();
      }
      TIMED(&dist[0], nandlog_flush(true));
   }
   line_reset(&extra);
   line_key_u32(&extra, "payload_bytes:", 4 * ((NANDLOG_RECORD_FRAMING ? NANDLOG_FRAMING_LENGTH_BYTES : 0) + 5 + 32));
   report("flush.partial_page", &dist[0], extra.text);
   report("flush.idle.erase_ahead", &dist[1], NULL);
   report("flush.idle.nothing_due", &dist[2], NULL);
   hw_power_up();
}

// Phase B2: the CPU work around the flash commands --------------------------------------------------------------------

static __attribute__((noinline)) bool erased_byte_by_byte(const uint8_t *bytes, uint32_t length)
{
   // A blank check's scan as a plain byte loop, for comparison with nandlog_bytes_erased()
   for (uint32_t i = 0; i < length; ++i)
      if (bytes[i] != 0xFF)
         return false;
   return true;
}

static void phase_b2_cpu(void)
{
   section("B2. The CPU work a commit, a read and a blank check do between flash commands, on page-sized buffers in RAM");
   const uint32_t payload_bytes = nandlog_data_bytes_per_page(), whole = NANDLOG_MAX_PAGE_SIZE_BYTES + NANDLOG_MAX_SPARE_SIZE_BYTES;
   volatile uint32_t sink = 0;
   hw_fill_pattern(data_buffer, whole, 0xB2B2B2B2u);
   memcpy(page_buffer, data_buffer, whole);
   dist_reset(&dist[0]);
   for (uint32_t i = 0; i < 200; ++i)
      TIMED(&dist[0], sink += (uint32_t)memcmp(page_buffer + 32, data_buffer + 32, payload_bytes - 8));
   report(name_of("cpu.memcmp_", payload_bytes - 8, "B"), &dist[0], "buffers:equal,aligned");
   dist_reset(&dist[0]);
   for (uint32_t i = 0; i < 200; ++i)
      TIMED(&dist[0], memcpy(page_buffer + 32, data_buffer + 32, payload_bytes));
   report(name_of("cpu.memcpy_", payload_bytes, "B"), &dist[0], "aligned");
   dist_reset(&dist[0]);
   for (uint32_t i = 0; i < 200; ++i)
      TIMED(&dist[0], memset(page_buffer, 0xFF, NANDLOG_MAX_PAGE_SIZE_BYTES));
   report(name_of("cpu.memset_", NANDLOG_MAX_PAGE_SIZE_BYTES, "B"), &dist[0], NULL);
   memset(data_buffer, 0xFF, whole);
   dist_reset(&dist[0]);
   for (uint32_t i = 0; i < 200; ++i)
      TIMED(&dist[0], sink += nandlog_bytes_erased(data_buffer, whole));
   report(name_of("cpu.bytes_erased_", whole, "B"), &dist[0], "code:nandlog_bytes_erased,buffer:erased");
   dist_reset(&dist[0]);
   for (uint32_t i = 0; i < 200; ++i)
      TIMED(&dist[0], sink += erased_byte_by_byte(data_buffer, whole));
   report(name_of("cpu.byte_loop_erased_", whole, "B"), &dist[0], "code:byte_at_a_time,buffer:erased");
   (void)sink;
}


// Phase F2: where the time of one call goes ---------------------------------------------------------------------------

static void print_trace(const char *name, uint32_t call_start, uint32_t call_end)
{
   // Every port call in order, with the CPU time since the one before it -- the library's and the driver's own work
   // between two commands -- then the totals
   uint32_t at = call_start, in_port = 0, in_delay = 0, cpu = 0, widest = 0, widest_at = 0;
   for (uint32_t i = 0; i < trace_count; ++i)
   {
      const trace_event_t *event = &trace[i];
      const uint32_t gap = event->start - at;
      cpu += gap;
      if (gap > widest)
      {
         widest = gap;
         widest_at = i;
      }
      if (event->is_delay)
         in_delay += event->end - event->start;
      else
         in_port += event->end - event->start;
      line_t line;
      line_reset(&line);
      line_str(&line, "BENCH_TRACE ");
      line_str(&line, name);
      line_key_u32(&line, " i=", i);
      line_str(&line, " gap_us=");
      line_us(&line, gap);
      line_str(&line, event->is_delay ? " delay" : (event->is_read ? " read cmd=" : " write cmd="));
      if (!event->is_delay)
         line_u32(&line, event->command);
      line_key_u32(&line, event->is_delay ? " us=" : " len=", event->length);
      line_str(&line, " dur_us=");
      line_us(&line, event->end - event->start);
      print("%s\n", line.text);
      at = event->end;
   }
   const uint32_t tail = call_end - at;
   cpu += tail;
   line_t line;
   line_reset(&line);
   line_str(&line, "BENCH_TRACE_SUM ");
   line_str(&line, name);
   line_str(&line, " total_us=");
   line_us(&line, call_end - call_start);
   line_str(&line, " port_us=");
   line_us(&line, in_port);
   line_str(&line, " delay_us=");
   line_us(&line, in_delay);
   line_str(&line, " cpu_us=");
   line_us(&line, cpu);
   line_str(&line, " after_last_us=");
   line_us(&line, tail);
   line_str(&line, " widest_gap_us=");
   line_us(&line, widest);
   line_key_u32(&line, " before_event=", widest_at);
   line_key_u32(&line, " events=", trace_count);
   line_key_u32(&line, " dropped=", trace_dropped);
   print("%s\n", line.text);
}

static void phase_f2_anatomy(void)
{
   section("F2. Where the time of one call goes: each port call inside it, and the CPU time between them (cmd in decimal)");
   static const uint32_t sizes[2] = { 32, 1024 };
   uint32_t timestamp = 1000000;
   hw_fill_pattern(data_buffer, 1024, 0x57025702u);

   // A store_record() that commits a page, outside a session as in F: the first one each record size reaches
   hw_power_down();
   for (uint32_t s = 0; s < 2; ++s)
      for (uint32_t n = 0; n < 2000; ++n)
      {
         const bench_ops_t before = ops;
         trace_count = trace_dropped = 0;
         trace_on = true;
         const uint32_t start = hw_cycle_count();
         const nandlog_status_t status = nandlog_store_record(0x53, ++timestamp, data_buffer, sizes[s]);
         const uint32_t end = hw_cycle_count();
         trace_on = false;
         if (status != NANDLOG_OK)
         {
            bench_error("store_record() refused a record");
            break;
         }
         if (ops.programs != before.programs)
         {
            print_trace(name_of("store.", sizes[s], "B.commit"), start, end);
            break;
         }
      }

   // flush(true) of a partial page, as in F
   for (uint32_t r = 0; r < 4; ++r)
      nandlog_store_record(0x50, ++timestamp, data_buffer, 32);
   trace_count = trace_dropped = 0;
   trace_on = true;
   uint32_t start = hw_cycle_count();
   nandlog_flush(true);
   uint32_t end = hw_cycle_count();
   trace_on = false;
   print_trace("flush.partial_page", start, end);

   // One retrieve_next_page() from the start of the current epoch, in a session as in E
   hw_power_up();
   nandlog_page_header_t header;
   nandlog_begin_reading(0, 0);
   trace_count = trace_dropped = 0;
   trace_on = true;
   start = hw_cycle_count();
   const uint32_t got = nandlog_retrieve_next_page(page_buffer, &header);
   end = hw_cycle_count();
   trace_on = false;
   nandlog_end_reading();
   if (!got)
      bench_error("retrieve_next_page() served nothing");
   print_trace("retrieve_next_page", start, end);
}


// Main ----------------------------------------------------------------------------------------------------------------

static void print_configuration(void)
{
   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   uint8_t id[3] = { 0, 0, 0 };
   static const uint8_t zero = 0;
   port_read(CMD_READ_ID, &zero, 1, id, sizeof(id));
   print("BENCH_INFO library=%u.%u source=%s framing=%u page_copy=%u erase_ahead_blocks=%u busy_poll_us=%u placement_attempts=%u\n",
         (uint32_t)NANDLOG_VERSION_MAJOR, (uint32_t)NANDLOG_VERSION_MINOR, NANDLOG_SOURCE_ID, (uint32_t)NANDLOG_RECORD_FRAMING,
         (uint32_t)NANDLOG_CHIP_PAGE_COPY, (uint32_t)NANDLOG_ERASE_AHEAD_BLOCKS, (uint32_t)NANDLOG_BUSY_POLL_INTERVAL_US,
         (uint32_t)NANDLOG_PAGE_PLACEMENT_ATTEMPTS);
   print("BENCH_INFO geometry page=%u spare=%u pages_per_block=%u blocks=%u reserved=%u retired_blocks=%u\n",
         geometry->page_size_bytes, geometry->spare_size_bytes, geometry->pages_per_block, geometry->block_count,
         geometry->reserved_blocks, nandlog_bad_block_count());
   print("BENCH_INFO part_id=%02X%02X%02X sr1=%02X sr2=%02X sr3=%02X\n", id[0], id[1], id[2],
         get_feature(SR1), get_feature(SR2), get_feature(SR3));
   line_t line;
   line_reset(&line);
   line_str(&line, "BENCH_INFO cycles_per_us=");
   line_fixed(&line, cycles_per_100ms, 100000, 3);
   line_key_u32(&line, " cycles_per_100ms=", cycles_per_100ms);
   print("%s\n", line.text);
   uint32_t overhead = 0xFFFFFFFFu;
   for (uint32_t i = 0; i < 64; ++i)
   {
      const uint32_t a = hw_cycle_count(), b = hw_cycle_count();
      if ((b - a) < overhead)
         overhead = b - a;
   }
   print("BENCH_INFO timer_overhead_cycles=%u op_counting=%u bus_and_delay_timing=%u library_log_deferred=%u marker=%u\n",
         overhead, ops_live ? 1 : 0, delay_live ? 1 : 0, log_live ? 1 : 0, (uint32_t)MARKER_ENABLED);
#ifdef BENCH_MARKER_PIN
   print("BENCH_INFO marker_pin=%u marker_gap_us=%u\n", (uint32_t)BENCH_MARKER_PIN, (uint32_t)BENCH_MARKER_GAP_US);
#endif

   // What the compiler was told, as far as it says: the exact flags are in the Makefile, and worth sending too
   uint32_t optimize = 0, optimize_size = 0, fast_math = 0;
#ifdef __OPTIMIZE__
   optimize = 1;
#endif
#ifdef __OPTIMIZE_SIZE__
   optimize_size = 1;
#endif
#ifdef __FAST_MATH__
   fast_math = 1;
#endif
   print("BENCH_INFO compiler=\"%s\" optimize=%u optimize_size=%u fast_math=%u\n", __VERSION__, optimize, optimize_size, fast_math);
   if (!ops_live)
      print("  NOTE: the port is not wrapped at link time, so there are no BENCH_COUNT lines and store_record() calls\n"
            "        that erase are told apart by time alone. Add the LFLAGS line from tests/README.md to fix both.\n");
}

int main(void)
{
   setup_hardware();
   hw_timing_init();
   calibrate_clock();
   if (!nandlog_init())
   {
      print("BENCH_ERROR nandlog_init() failed -- no part, or the port would not open\n");
      ++errors;
      halt();
   }
   system_enable_interrupts(true);
   MARK_INIT();
   hw_power_up();
   detect_instrumentation();

   print("\n============================================================\n");
   print("nandlog on-device benchmark\n");
   print("*** DESTRUCTIVE: ERASES THE METADATA RING AND THE WHOLE LOG REGION ***\n");
   print("Expected run time: about 3 to 6 minutes. It ends with BENCH_DONE.\n");
   print("============================================================\n");
   if (!cycles_per_us)
   {
      bench_error("DWT is not counting: attach the debugger and run again; no timing here would mean anything");
      halt();
   }
   print_configuration();
   report_marked_blocks("start");

#if MARKER_ENABLED
   // A quiet second with the part awake and the marker low, then one asleep, as the analyzer's baseline
   print("BENCH_INFO baseline: 1000 ms idle with the part awake in a session, then 1000 ms with it asleep\n");
   am_hal_delay_us(1000000);
   hw_power_down();
   am_hal_delay_us(1000000);
   hw_power_up();
#endif

   phase_a_primitives();
   phase_b_crc();
   phase_b2_cpu();
   phase_c_wipe();
   phase_d_ring();
   phase_e_lengths();
   phase_f_store();
   phase_f2_anatomy();

   section("G. Done");
   hw_power_up();
   nandlog_stats_t stats;
   nandlog_get_stats(&stats);
   print("BENCH_STATS since_last_init records_stored=%u records_refused=%u records_lost=%u pages_committed=%u "
         "pages_relocated=%u tombstones=%u positions_skipped=%u blocks_retired=%u reads_at_ecc_limit=%u retired_blocks_total=%u\n",
         stats.records_stored, stats.records_refused, stats.records_lost, stats.pages_committed, stats.pages_relocated,
         stats.tombstones_written, stats.positions_skipped, stats.blocks_retired, stats.reads_at_ecc_limit,
         nandlog_bad_block_count());
   for (uint32_t i = 0; i < HW_SCRATCH_BLOCKS; ++i)
      if (!scratch_in_log_region[i])      // one in the log region was erased with it in phase C, and is the log's now
         nandlog_chip_erase_block(scratch_pages[i]);
   report_marked_blocks("end");
   hw_power_down();
   print("  scratch blocks erased; the log region now holds the benchmark's epochs %u and %u\n", epochs_begun - 1, epochs_begun);
   print("BENCH_DONE errors=%u elapsed_s=%u\n", errors, (uint32_t)(cycles_to_ns(now_cycles()) / 1000000000u));
   while (true)
      am_hal_delay_us(1000000);
}
