// ON-DEVICE CHARACTERIZATION -- what an interrupted PROGRAM or BLOCK ERASE leaves on the part
//
// *****************************************************************************************************************
// ***  DESTRUCTIVE.  THIS PROGRAM PROGRAMS AND ERASES SIX BLOCKS IN THE MIDDLE OF THE LOG REGION, FROM BLOCK    ***
// ***  2000 UP, ABOUT 35 TIMES EACH, AND LEAVES THEM ERASED.  WHATEVER THE LOG HELD IN THEM IS GONE.            ***
// ***  RUN IT ON A DEVELOPMENT BOARD ONLY -- NEVER ON A DEVICE HOLDING DATA YOU WANT.                           ***
// *****************************************************************************************************************
//
// Its blocks are the first INTR_BLOCKS good, unmarked blocks from INTR_FIRST_BLOCK up (INTR_INFO blocks= names them):
// far from the metadata ring in the bottom eight blocks of the array, the sixteen-block reserve at the top (the
// bad-block table, and the other tests' scratch blocks), and the 48 blocks under the reserve the driver may be given
// for its table. A block the bad-block table names, or whose first spare byte reads other than FFh with ECC off, is
// stepped over. Nothing else on the part is written: it never calls the log -- no nandlog_init(), no epoch, no
// record -- and never retires a block. It brings up the port and the driver (nandlog_chip_init(), so the driver's
// bad-block table can be asked about a block), and everything after that is raw commands through the port.
//
// AFTERWARDS the log on this board is not to be trusted until an epoch is begun again: the current epoch may have had
// pages in those six blocks, and they now read erased. The benchmark wipes the log region anyway, so on the bench
// board the next nandlog_bench run sets it right.
//
// WHY. The library's durability is tested in a simulator, which models what an interrupted PROGRAM or BLOCK ERASE
// leaves on the part: a torn page that reads uncorrectable, a partly programmed page, weak pages that drift, an
// interrupted erase whose block reads erased yet programs unreliably, and reads that differ from one to the next.
// Nothing has calibrated those models against silicon. The library also carries one counted residual risk: a page an
// interrupted program left reading erased in EVERY bit passes the recovery's blank check and is programmed a second
// time after the reboot, on a part that allows one program per page. How often an interrupted program leaves such a
// page, and what a second program over one reads back as, is the question this program exists to answer.
//
// HOW. The datasheet says RESET (FFh) "stops all operations" (section 9, p. 36), so RESET issued a set time after
// PROGRAM EXECUTE or BLOCK ERASE stops the operation at a chosen point, as often as wanted, with the part powered
// throughout. That makes it a PROXY for a brown-out, not a substitute for one: a sagging supply takes the charge
// pumps and the sense amplifiers down with it over microseconds to milliseconds, where RESET stops the state machine
// at once on a healthy rail. What this measures is what an operation stopped at a given point leaves in the cells;
// the power-cut soak (test_nandlog_powercut.c) remains what tests the library against real cuts.
//
// WHAT IT DOES, in order. Under a minute in all; one line per trial as it goes.
//   P  PROGRAM SWEEP. For each of 21 nominal delays from 0 to 1,000 us -- dense up to 700, as the part's program
//      takes about 620 us -- a freshly erased block, and in it 32 pages programmed in order, each one:
//        a pseudo-random pattern loaded, about half its bits zero; WRITE ENABLE; PROGRAM EXECUTE; the delay, spun on
//        the cycle counter; RESET; status polled until OIP clears;
//        three raw reads (ECC off, main and spare): the bits programmed where the pattern has zeros (progress), where
//        it has ones (wrong: none expected), in the spare (the part's parity), whether every bit of main and spare
//        reads erased, and how many bits the second and third reads differ from the first by;
//        a read with ECC on: the part's ECC status, and whether the data is the pattern;
//        and IF the first raw read found every bit erased -- what the log's blank check would have passed -- the
//        same pattern programmed again, to completion, then read back with ECC on (status, match) and raw. THIS is
//        the residual-risk event, and the line that matters most.
//   E  ERASE SWEEP. First each block filled and erased to completion INTR_REFERENCE_ROUNDS times, timed, as the
//      reference. Then for each of 14 nominal delays from 0 to 5,000 us -- a full block's erase took 2.7-3.4 ms on
//      the bench -- three blocks, each: erased; filled with 64 patterned pages; WRITE ENABLE; BLOCK ERASE; the
//      delay; RESET; poll; every page read raw (bits programmed; erased in every bit?) and with ECC on (status);
//      then, WITHOUT erasing again, a pattern programmed into the first page and into the last, each read back with
//      ECC on (status, match) and raw; then the block erased to completion again.
//   F  every block erased once more and every page of it read raw, to show it left erased; the reference busy times.
//
// TIMING. Interrupts are off from just before PROGRAM EXECUTE or BLOCK ERASE to the end of RESET's busy wait, so
// nothing the board does stretches the delay or the busy time unmeasured. delay_us runs from the return of the
// operation's transfer to the call of RESET's, on the DWT cycle counter calibrated against am_hal_delay_us(); the
// port's own call overhead -- a microsecond or two, the bench's prim.status_read -- sits at both ends and is not
// subtracted, so a nominal 0 is the shortest interval the port allows. reset_us runs from the return of RESET's
// transfer to the end of the first status read that finds OIP clear: an upper bound by about two status reads, as
// the bench's busy times are. Every other busy time is measured the same way, from the end of its command:
// second_us, first_us, last_us, restore_us, erase_us and the INTR_REF lines.
//
// WHAT THE PART IS ASKED, and what is assumed of it:
//   - sr3= is the status register as the poll that found OIP clear after RESET read it. The datasheet says RESET
//     clears P_FAIL and E_FAIL (section 12), so p_fail and e_fail there are expected to be 0 whatever happened; a 1
//     is news. A program or erase that ran to completion reports its own, from its own poll.
//   - a0= and b0= are the protection and configuration registers read right after RESET, before this program sets
//     them back to the array unlocked and ECC on. Whether RESET returns them to their power-on values (A0h 38h,
//     B0h 10h) the datasheet does not say; these say.
//   - ECC status comes from the status read that ends the PAGE DATA READ of a fresh latch with ECC on, never from
//     the RESET: the datasheet sets ECCS to 00b at a RESET and at the start of every read, then updates it when the
//     read completes. With ECC off the part clears ECCS, so a raw read says nothing about ECC.
//   - Every program loads its data afresh from column 0 -- the main array, the spare left FFh -- after the reads
//     that follow a cut have overwritten the cache register anyway, so nothing here depends on what RESET leaves in
//     the cache register. With ECC on, as the log programs, the part writes its own parity into the spare's last
//     112 bytes; the rest of the spare is loaded FFh, so spare_zeros counts the parity programmed.
//
// OUTPUT. Every line meant to be parsed starts INTR_, holds key=value fields, and is printed whole:
//
//    INTR_START nandlog=<major.minor> source=<id> part_id=<hex> device_id=<hex> cycles_per_us=<x> ...
//    INTR_INFO <key>=<value> ...
//    INTR_PROG trial=<n> nominal_us=<d> block=<b> page=<p> delay_us=<us> reset_us=<us> ready=<0|1> sr3=<hex>
//              p_fail=<0|1> a0=<hex> b0=<hex> pattern_zeros=<n> progress=<n> wrong=<n> spare_zeros=<n> blank=<0|1>
//              blank_reads=<0..3> flips=<n> unstable=<0|1> class=<blank|partial|complete> ecc=<status> match=<0|1>
//              second=<0|1> [second_us=<us> second_p_fail=<0|1> second_ecc=<status> second_match=<0|1>
//              second_missing=<n> second_wrong=<n>]
//    INTR_PROG_SUM nominal_us=<d> n=<n> ... blank=.. partial=.. complete=.. ecc_*=.. unstable=.. second_*=..
//    INTR_ERASE trial=<n> nominal_us=<d> block=<b> delay_us=<us> reset_us=<us> ready=<0|1> sr3=<hex> e_fail=<0|1>
//               a0=<hex> b0=<hex> fill_failures=<n> blank_pages=<n> max_zeros=<n> total_zeros=<n> ecc_ff=<n>
//               first_blank=<0|1> first_us=<us> first_p_fail=<0|1> first_ecc=<status> first_match=<0|1>
//               first_missing=<n> first_wrong=<n> last_... restore_us=<us> restore_fail=<0|1>
//               zeros=<64 counts, page 0 first> ecc_map=<64 digits, page 0 first>
//    INTR_ERASE_SUM nominal_us=<d> n=<n> ... blank_blocks=.. blank_pages=.. ecc_*=.. first_*=.. last_*=..
//    INTR_REF <name> n=<n> min=<us> med=<us> max=<us> mean=<us>
//    INTR_FINAL block=<b> erase_us=<us> e_fail=<0|1> blank_pages=<n> pages=<n>
//    INTR_ERROR ...       INTR_DONE errors=<n> aborted=<0|1> ... elapsed_s=<s>
//
// <status> is none, corrected, uncorrectable or limit (ECCS 00b, 01b, 10b, 11b: limit is corrected with the error
// count at the ECC's maximum). class: blank is every bit of main and spare erased; complete is the main array reading
// the pattern bit for bit; partial is anything else. flips is the most bits the second or third raw read differs from
// the first by; unstable is flips > 0. In INTR_ERASE, zeros are the bits reading 0 in each page's main and spare,
// raw, and ecc_map each page's ECCS read with ECC on; ecc_ff counts the pages whose main array read all FFh with ECC
// on. first_ and last_ are the weak-erase programs into pages 0 and 63, first_blank saying whether the page read
// erased in every bit before it. The per-delay summaries are printed as each delay finishes, and
// tests/interrupt_summary.py tabulates the whole capture.
//
// WHAT TO SEND BACK: the whole serial output, from the reset banner to INTR_DONE, or at least interrupt_summary.py's
// output for it.

#include <string.h>
#include "nandlog_hw_test.h"


// Alliance AS5F18G04SND Command Set, Below the Driver -----------------------------------------------------------------

#define CMD_READ_ID                                 0x9F
#define CMD_RESET                                   0xFF
#define CMD_GET_FEATURE                             0x0F
#define CMD_SET_FEATURE                             0x1F
#define CMD_WRITE_ENABLE                            0x06
#define CMD_BLOCK_ERASE                             0xD8
#define CMD_PROGRAM_DATA_LOAD                       0x02
#define CMD_PROGRAM_EXECUTE                         0x10
#define CMD_PAGE_DATA_READ                          0x13
#define CMD_READ                                    0x03

#define SR1                                         0xA0
#define SR2                                         0xB0
#define SR3                                         0xC0
#define SR_BUSY                                     0x01
#define SR_ERASE_FAILURE                            0x04
#define SR_PROGRAM_FAILURE                          0x08
#define SR_ECC_SHIFT                                4        // ECCS1:ECCS0 are bits 5:4 of the status register
#define SR2_ECC_ENABLE                              0x10
#define PROTECT_ALL                                 0x7E
#define PROTECT_NONE                                0x02

#define ECCS_NONE                                   0
#define ECCS_UNCORRECTABLE                          2

// The part this was written for, which the run checks before it touches a block. Fixed rather than read from the
// geometry so that the buffers and the per-page records below can be sized at compile time
#define PART_PAGE_BYTES                             4096
#define PART_SPARE_BYTES                            256
#define PART_PAGES_PER_BLOCK                        64
#define PART_RAW_BYTES                              (PART_PAGE_BYTES + PART_SPARE_BYTES)
#define PART_RAW_WORDS                              (PART_RAW_BYTES / 4)
#define PART_MAIN_WORDS                             (PART_PAGE_BYTES / 4)


// Run Parameters ------------------------------------------------------------------------------------------------------

// The blocks: the first INTR_BLOCKS good, unmarked ones from INTR_FIRST_BLOCK up, looking no further than
// INTR_CANDIDATE_BLOCKS. The run refuses a range that reaches down into the metadata ring (nandlog.c's first
// INTR_RING_BLOCKS blocks of the array), or up into the reserve or the INTR_TABLE_REACH_BLOCKS at the top of the array
// that the driver may be given for its bad-block table (its NANDLOG_CHIP_TABLE_SEARCH_BLOCKS)
#define INTR_FIRST_BLOCK                            2000
#define INTR_CANDIDATE_BLOCKS                       32
#define INTR_BLOCKS                                 6
#define INTR_RING_BLOCKS                            8
#define INTR_TABLE_REACH_BLOCKS                     64

// The program sweep: nominal delays from the end of PROGRAM EXECUTE to RESET, dense where a program begins and
// across the ~620 us it lasts, then past it; and how many pages of a fresh block each is tried on
static const uint32_t program_delays_us[] = { 0, 5, 10, 20, 35, 50, 75, 100, 150, 200, 250, 300, 350, 400, 450, 500, 550,
                                              600, 650, 700, 1000 };
#define INTR_PAGES_PER_PROGRAM_DELAY                32

// The erase sweep: nominal delays across and past the 2.7-3.4 ms a full block's erase took on the bench, and how many
// blocks each is tried on; and how many times each block is filled and erased whole first, as the reference
#define INTR_BLOCKS_PER_ERASE_DELAY                 3
static const uint32_t erase_delays_us[] = { 0, 50, 150, 300, 600, 1000, 1500, 2000, 2500, 2800, 3100, 3400, 4000, 5000 };
#define INTR_REFERENCE_ROUNDS                       2

#define INTR_REFERENCE_SAMPLES                      512      // reservoir behind a reference median
#define INTR_ERASE_ATTEMPTS                         3        // as the driver gives a block, before calling it failed

#ifndef NANDLOG_SOURCE_ID
#define NANDLOG_SOURCE_ID                           "unrecorded"   // the build's fingerprint of the logging sources
#endif

#define COUNT_OF(array)                             (sizeof(array) / sizeof((array)[0]))


// Static Buffers ------------------------------------------------------------------------------------------------------

// Word-aligned, so the bit counts below go a word at a time over buffers whose lengths are whole words
static uint32_t pattern_words[PART_MAIN_WORDS], first_words[PART_RAW_WORDS], again_words[PART_RAW_WORDS];
static uint8_t *const pattern = (uint8_t*)pattern_words;
static uint8_t *const first_read = (uint8_t*)first_words, *const again_read = (uint8_t*)again_words;

static uint32_t blocks[INTR_BLOCKS];
static uint8_t sr2_ecc_on = SR2_ECC_ENABLE, sr2_ecc_off;
static uint32_t errors, program_trials, erase_trials;
static bool part_up;


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
   // A 64-bit clock from the 32-bit counter, which wraps every ~45 s at 96 MHz. Called once a trial, and no trial
   // comes near that long, so it never misses a wrap
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

static uint32_t elapsed_s(void)
{
   // Zero, rather than a division by zero, when DWT never counted
   return cycles_per_100ms ? (uint32_t)(cycles_to_ns(now_cycles()) / 1000000000u) : 0;
}


// Line Building -------------------------------------------------------------------------------------------------------
//
// Each INTR line is assembled here and printed whole. An erase trial's line runs to about 1,100 characters, so it goes
// out in pieces small enough for any printf buffer, with the newline only after the last

typedef struct
{
   char text[1536];
   uint32_t length;
} line_t;

static line_t line;

static void line_reset(line_t *out)
{
   out->length = 0;
   out->text[0] = '\0';
}

static void line_str(line_t *out, const char *text)
{
   while (*text && ((out->length + 1) < sizeof(out->text)))
      out->text[out->length++] = *text++;
   out->text[out->length] = '\0';
}

static void line_u32(line_t *out, uint32_t value)
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
   line_str(out, text);
}

static void line_fixed(line_t *out, uint64_t value, uint32_t scale, uint32_t decimals)
{
   // value / scale, printed with exactly 'decimals' decimal places (scale is 10^decimals)
   line_u32(out, (uint32_t)(value / scale));
   line_str(out, ".");
   uint32_t remainder = (uint32_t)(value % scale), divisor = scale / 10;
   for (uint32_t i = 0; i < decimals; ++i, divisor /= 10)
   {
      const char digit[2] = { (char)('0' + ((remainder / divisor) % 10)), '\0' };
      line_str(out, digit);
   }
}

static void line_key_u32(line_t *out, const char *key, uint32_t value)
{
   line_str(out, key);
   line_u32(out, value);
}

static void line_key_str(line_t *out, const char *key, const char *value)
{
   line_str(out, key);
   line_str(out, value);
}

static void line_key_us(line_t *out, const char *key, uint64_t cycles)
{
   line_str(out, key);
   line_fixed(out, cycles_to_ns(cycles), 1000, 3);
}

static void line_key_hex(line_t *out, const char *key, uint32_t value, uint32_t digits)
{
   static const char hex[] = "0123456789ABCDEF";
   char text[9];
   digits = (digits > 8) ? 8 : digits;
   for (uint32_t i = 0; i < digits; ++i)
      text[i] = hex[(value >> (4 * (digits - 1 - i))) & 0xF];
   text[digits] = '\0';
   line_str(out, key);
   line_str(out, text);
}

static void line_print(const line_t *out)
{
   char piece[121];
   for (uint32_t offset = 0; offset < out->length; offset += (sizeof(piece) - 1))
   {
      const uint32_t count = ((out->length - offset) < (sizeof(piece) - 1)) ? (out->length - offset) : (sizeof(piece) - 1);
      memcpy(piece, out->text + offset, count);
      piece[count] = '\0';
      print("%s", piece);
   }
   print("\n");
}

static void section(const char *title)
{
   const uint64_t elapsed_ms = cycles_to_ns(now_cycles()) / 1000000u;
   print("\n--- %s   [t=%u s] ---\n", title, (uint32_t)(elapsed_ms / 1000));
}

static void intr_error(const char *what, uint32_t block)
{
   ++errors;
   print("INTR_ERROR %s block=%u\n", what, block);
}


// A Minimal Command Layer, Through the Port Alone ---------------------------------------------------------------------

static void port_read(uint8_t command, const void *address, uint32_t address_length, void *buffer, uint32_t length)
{
   nandlog_port_transfer_read(command, address, address_length, buffer, length);
}

static void port_write(uint8_t command, const void *address, uint32_t address_length, const void *buffer, uint32_t length)
{
   nandlog_port_transfer_write(command, address, address_length, buffer, length);
}

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
}

static bool wait_ready(uint8_t *status)
{
   // Poll with no pacing at all, so the poll that first sees READY ends as soon after the part does as one status
   // read allows. Gives up after 100 ms, far beyond any operation this part performs, a RESET of one included
   const uint32_t start = hw_cycle_count(), limit = us_to_cycles(100000);
   do
   {
      const uint8_t value = get_feature(SR3);
      if (!(value & SR_BUSY))
      {
         if (status)
            *status = value;
         return true;
      }
   } while ((uint32_t)(hw_cycle_count() - start) < limit);
   intr_error("the part never cleared BUSY within 100 ms", 0);
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

static void load_page(const uint8_t *data)
{
   // WRITE ENABLE, then the main array into the cache register from column 0, as the driver does it: on this part a
   // load resets the register to FFh from its starting column to the end of the spare, so the spare goes out FFh and
   // the part, with ECC on, fills its parity bytes in itself
   static const uint8_t column_zero[2] = { 0x00, 0x00 };
   port_write(CMD_WRITE_ENABLE, NULL, 0, NULL, 0);
   port_write(CMD_PROGRAM_DATA_LOAD, column_zero, sizeof(column_zero), data, PART_PAGE_BYTES);
}

static bool manual_program(uint32_t page, const uint8_t *data, uint32_t *busy_cycles, uint8_t *status)
{
   // A whole program, to completion; the busy time from the end of the EXECUTE transfer to the end of the first
   // status read that reports READY
   uint8_t address[3];
   page_address(page, address);
   *status = 0xFF;
   load_page(data);
   port_write(CMD_PROGRAM_EXECUTE, NULL, 0, address, sizeof(address));
   const uint32_t t0 = hw_cycle_count();
   const bool ready = wait_ready(status);
   *busy_cycles = hw_cycle_count() - t0;
   return ready && !(*status & SR_PROGRAM_FAILURE);
}

static bool manual_erase(uint32_t block, uint32_t *busy_cycles)
{
   uint8_t address[3], status = 0xFF;
   page_address(block * PART_PAGES_PER_BLOCK, address);
   port_write(CMD_WRITE_ENABLE, NULL, 0, NULL, 0);
   port_write(CMD_BLOCK_ERASE, NULL, 0, address, sizeof(address));
   const uint32_t t0 = hw_cycle_count();
   const bool ready = wait_ready(&status);
   *busy_cycles = hw_cycle_count() - t0;
   return ready && !(status & SR_ERASE_FAILURE);
}

static bool erase_fully(uint32_t block, uint32_t *busy_cycles)
{
   // A few chances, as the driver gives a block: an erase can be repeated harmlessly. The busy time is the last
   // attempt's
   for (uint32_t attempt = 0; attempt < INTR_ERASE_ATTEMPTS; ++attempt)
      if (manual_erase(block, busy_cycles))
         return true;
   return false;
}

static uint8_t latch(uint32_t page)
{
   // PAGE DATA READ to READY. Returns the status that ended the wait, or one saying uncorrectable if the part never
   // got there, so a read that did not happen is never taken for a clean one
   uint8_t address[3], status = (uint8_t)(ECCS_UNCORRECTABLE << SR_ECC_SHIFT);
   page_address(page, address);
   port_write(CMD_PAGE_DATA_READ, NULL, 0, address, sizeof(address));
   wait_ready(&status);
   return status;
}

static void read_raw(uint32_t page, uint8_t *buffer)
{
   // ECC off for this one read, so the cells show as they are -- main and spare, the part's parity included -- and
   // on again before anything else is done. This is the read the driver's blank check makes
   static const uint8_t column_zero[3] = { 0x00, 0x00, 0x00 };
   set_feature(SR2, sr2_ecc_off);
   latch(page);
   port_read(CMD_READ, column_zero, sizeof(column_zero), buffer, PART_RAW_BYTES);
   set_feature(SR2, sr2_ecc_on);
}

static uint32_t read_with_ecc(uint32_t page, uint8_t *buffer)
{
   // The main array as the log reads it, ECC on; returns the ECCS the read ended with
   static const uint8_t column_zero[3] = { 0x00, 0x00, 0x00 };
   const uint8_t status = latch(page);
   port_read(CMD_READ, column_zero, sizeof(column_zero), buffer, PART_PAGE_BYTES);
   return (uint32_t)(status >> SR_ECC_SHIFT) & 3u;
}


// Marked Blocks -------------------------------------------------------------------------------------------------------

static void halt(void);

static uint8_t raw_marker(uint32_t block)
{
   // The first spare byte of the block's first page, as the driver reads a mark. The caller turns ECC off
   const uint8_t column[3] = { (uint8_t)(PART_PAGE_BYTES >> 8), (uint8_t)PART_PAGE_BYTES, 0x00 };
   uint8_t address[3], marker = 0xFF;
   page_address(block * PART_PAGES_PER_BLOCK, address);
   port_write(CMD_PAGE_DATA_READ, NULL, 0, address, sizeof(address));
   wait_ready(NULL);
   port_read(CMD_READ, column, sizeof(column), &marker, sizeof(marker));
   return marker;
}

static bool scratch_usable(uint32_t block, uint8_t *marker)
{
   // Neither retired by the table nor carrying a mark: a block whose first page reads all 00h, as a dead one does,
   // carries one
   *marker = 0xFF;
   if (nandlog_chip_is_bad_block(block * PART_PAGES_PER_BLOCK))
      return false;
   set_feature(SR2, sr2_ecc_off);
   *marker = raw_marker(block);
   set_feature(SR2, sr2_ecc_on);
   return *marker == 0xFF;
}

static void choose_blocks(void)
{
   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   const uint32_t end = INTR_FIRST_BLOCK + INTR_CANDIDATE_BLOCKS;
   if ((INTR_FIRST_BLOCK < INTR_RING_BLOCKS) || (end > (geometry->block_count - geometry->reserved_blocks)) ||
       (end > (geometry->block_count - INTR_TABLE_REACH_BLOCKS)))
   {
      intr_error("the candidate blocks reach the ring, the reserve or the bad-block table's reach; nothing written",
                 INTR_FIRST_BLOCK);
      halt();
   }
   static line_t skipped;
   line_t *out = &line;
   line_reset(out);
   line_str(out, "INTR_INFO blocks=");
   line_reset(&skipped);
   uint32_t found = 0, skipped_count = 0;
   for (uint32_t block = INTR_FIRST_BLOCK; (block < end) && (found < INTR_BLOCKS); ++block)
   {
      uint8_t marker = 0xFF;
      if (scratch_usable(block, &marker))
      {
         line_str(out, found ? "," : "");
         line_u32(out, block);
         blocks[found++] = block;
         continue;
      }
      line_str(&skipped, skipped_count++ ? "," : "");
      line_u32(&skipped, block);
      line_str(&skipped, (marker == 0xFF) ? ":table" : ":mark");
   }
   line_key_str(out, " skipped=", skipped_count ? skipped.text : "-");
   line_print(out);
   if (found < INTR_BLOCKS)
   {
      intr_error("fewer good, unmarked blocks among the candidates than the run needs; nothing written", INTR_FIRST_BLOCK);
      halt();
   }
}


// Counting Bits -------------------------------------------------------------------------------------------------------
//
// A programmed bit reads 0. Every count is over whole words of buffers filled by a single read

static uint32_t ones(uint32_t word)
{
   return (uint32_t)__builtin_popcount(word);
}

typedef struct
{
   uint32_t pattern_zeros;       // zeros the pattern asked for
   uint32_t progress;            // ... of which read 0
   uint32_t wrong;               // zeros where the pattern has ones: a program pulls no bit it was not asked to, so
                                 // these are disturbance, or charge the page held before the program
   uint32_t spare_zeros;         // zeros in the spare: with ECC on during the program, the part's parity
} against_pattern_t;

static void count_against_pattern(const uint32_t *cells, against_pattern_t *out)
{
   memset(out, 0, sizeof(*out));
   for (uint32_t i = 0; i < PART_MAIN_WORDS; ++i)
   {
      out->pattern_zeros += ones(~pattern_words[i]);
      out->progress += ones(~cells[i] & ~pattern_words[i]);
      out->wrong += ones(~cells[i] & pattern_words[i]);
   }
   for (uint32_t i = PART_MAIN_WORDS; i < PART_RAW_WORDS; ++i)
      out->spare_zeros += ones(~cells[i]);
}

static uint32_t zero_bits(const uint32_t *cells, uint32_t words)
{
   uint32_t zeros = 0;
   for (uint32_t i = 0; i < words; ++i)
      zeros += ones(~cells[i]);
   return zeros;
}

static uint32_t differing_bits(const uint32_t *a, const uint32_t *b, uint32_t words)
{
   uint32_t differing = 0;
   for (uint32_t i = 0; i < words; ++i)
      differing += ones(a[i] ^ b[i]);
   return differing;
}


// Outcomes and Summaries ----------------------------------------------------------------------------------------------

static const char *ecc_name(uint32_t eccs)
{
   static const char *const names[4] = { "none", "corrected", "uncorrectable", "limit" };
   return names[eccs & 3];
}

// What the log would make of a page programmed and read back with ECC on: whole with nothing to correct, whole after
// correction, refused as unreadable -- or the one that matters most, readable by the part's account yet not what was
// written
#define OUTCOMES                                    4
enum { OUTCOME_CLEAN, OUTCOME_CORRECTED, OUTCOME_UNCORRECTABLE, OUTCOME_MISMATCH };
static const char *const outcome_names[OUTCOMES] = { "clean", "corrected", "uncorrectable", "mismatch" };

static uint32_t outcome_of(uint32_t eccs, bool match)
{
   if (eccs == ECCS_UNCORRECTABLE)
      return OUTCOME_UNCORRECTABLE;
   if (!match)
      return OUTCOME_MISMATCH;
   return (eccs == ECCS_NONE) ? OUTCOME_CLEAN : OUTCOME_CORRECTED;
}

typedef struct
{
   uint32_t delay_min, delay_max, reset_min, reset_max;     // cycles
} spans_t;

static void spans_reset(spans_t *spans)
{
   spans->delay_min = spans->reset_min = 0xFFFFFFFFu;
   spans->delay_max = spans->reset_max = 0;
}

static void spans_add(spans_t *spans, uint32_t delay_cycles, uint32_t reset_cycles)
{
   spans->delay_min = (delay_cycles < spans->delay_min) ? delay_cycles : spans->delay_min;
   spans->delay_max = (delay_cycles > spans->delay_max) ? delay_cycles : spans->delay_max;
   spans->reset_min = (reset_cycles < spans->reset_min) ? reset_cycles : spans->reset_min;
   spans->reset_max = (reset_cycles > spans->reset_max) ? reset_cycles : spans->reset_max;
}

static void line_spans(line_t *out, const spans_t *spans, uint32_t n)
{
   line_key_us(out, " delay_us_min=", n ? spans->delay_min : 0);
   line_key_us(out, " delay_us_max=", spans->delay_max);
   line_key_us(out, " reset_us_min=", n ? spans->reset_min : 0);
   line_key_us(out, " reset_us_max=", spans->reset_max);
}

static void line_outcomes(line_t *out, const char *prefix, const uint32_t *counts)
{
   for (uint32_t i = 0; i < OUTCOMES; ++i)
   {
      line_str(out, " ");
      line_str(out, prefix);
      line_str(out, outcome_names[i]);
      line_key_u32(out, "=", counts[i]);
   }
}

static void line_ecc_counts(line_t *out, const uint32_t *counts)
{
   static const char *const keys[4] = { " ecc_none=", " ecc_corrected=", " ecc_uncorrectable=", " ecc_limit=" };
   for (uint32_t i = 0; i < 4; ++i)
      line_key_u32(out, keys[i], counts[i]);
}

typedef struct
{
   uint32_t n, not_ready, p_fail, blank, partial, complete, ecc[4], match, unstable;
   uint32_t second, second_p_fail, second_outcome[OUTCOMES];
   spans_t spans;
} program_summary_t;

typedef struct
{
   uint32_t n, not_ready, e_fail, fill_failures, blank_blocks, pages, blank_pages, ecc[4], ecc_ff, max_zeros;
   uint64_t zeros;
   uint32_t first_outcome[OUTCOMES], last_outcome[OUTCOMES], weak_p_fail;
   spans_t spans;
} erase_summary_t;


// Reference Busy Times ------------------------------------------------------------------------------------------------
//
// What a program and a full block's erase take when nothing stops them, on these blocks in this run: what the delays
// are to be read against. Every program that fills a block for the erase sweep is a sample of the first

typedef struct
{
   uint32_t n, kept, min, max;
   uint64_t sum;
   uint32_t samples[INTR_REFERENCE_SAMPLES];     // cycles
} reference_t;

static reference_t program_reference, erase_reference;
static uint32_t reservoir_state = 0x2545F491u;

static void reference_add(reference_t *reference, uint32_t cycles)
{
   if (!reference->n)
      reference->min = 0xFFFFFFFFu;
   ++reference->n;
   reference->sum += cycles;
   reference->min = (cycles < reference->min) ? cycles : reference->min;
   reference->max = (cycles > reference->max) ? cycles : reference->max;
   if (reference->kept < INTR_REFERENCE_SAMPLES)
      reference->samples[reference->kept++] = cycles;
   else
   {
      // Algorithm R: every sample so far has the same chance of being in the reservoir
      reservoir_state = (1664525u * reservoir_state) + 1013904223u;
      const uint32_t slot = (uint32_t)(((uint64_t)reservoir_state * reference->n) >> 32);
      if (slot < INTR_REFERENCE_SAMPLES)
         reference->samples[slot] = cycles;
   }
}

static void reference_report(const char *name, reference_t *reference)
{
   // Nearest-rank median over the kept samples, which are sorted in place: nothing is added after this
   for (uint32_t gap = reference->kept / 2; gap; gap /= 2)
      for (uint32_t i = gap; i < reference->kept; ++i)
      {
         const uint32_t value = reference->samples[i];
         uint32_t j = i;
         for (; (j >= gap) && (reference->samples[j - gap] > value); j -= gap)
            reference->samples[j] = reference->samples[j - gap];
         reference->samples[j] = value;
      }
   line_t *out = &line;
   line_reset(out);
   line_str(out, "INTR_REF ");
   line_str(out, name);
   line_key_u32(out, " n=", reference->n);
   if (reference->n)
   {
      line_key_us(out, " min=", reference->min);
      line_key_us(out, " med=", reference->samples[(reference->kept - 1) / 2]);
      line_key_us(out, " max=", reference->max);
      line_key_us(out, " mean=", reference->sum / reference->n);
   }
   line_print(out);
}


// Stopping an Operation Part-Way --------------------------------------------------------------------------------------

typedef struct
{
   uint32_t delay_cycles, reset_cycles;
   uint8_t status, a0, b0;
   bool ready;
} cut_t;

static void cut_short(uint8_t command, uint32_t page, uint32_t nominal_us, cut_t *cut)
{
   // Assumes WRITE ENABLE has been issued and, for a program, the cache register loaded. The operation, the delay on
   // the cycle counter, RESET, and the poll until RESET is done, with interrupts off throughout: the transfers are
   // polled, not interrupt-driven, so nothing needs them, and nothing the board does can then stretch the delay or the
   // busy time unmeasured
   uint8_t address[3];
   page_address(page, address);
   const uint32_t wait = us_to_cycles(nominal_us);
   cut->status = 0xFF;
   const uint32_t interrupts = am_hal_interrupt_master_disable();
   port_write(command, NULL, 0, address, sizeof(address));
   const uint32_t issued = hw_cycle_count();
   while ((uint32_t)(hw_cycle_count() - issued) < wait)
      ;
   const uint32_t reset_called = hw_cycle_count();
   port_write(CMD_RESET, NULL, 0, NULL, 0);
   const uint32_t reset_issued = hw_cycle_count();
   cut->ready = wait_ready(&cut->status);
   const uint32_t reset_done = hw_cycle_count();
   am_hal_interrupt_master_set(interrupts);
   cut->delay_cycles = reset_called - issued;
   cut->reset_cycles = reset_done - reset_issued;
   if (!cut->ready)
      intr_error("RESET did not finish", (page / PART_PAGES_PER_BLOCK));

   // The protection and configuration registers as RESET left them -- whether it puts them back to their power-on
   // values the datasheet does not say -- then set back to what this program works with: the array unlocked, ECC on
   cut->a0 = get_feature(SR1);
   cut->b0 = get_feature(SR2);
   if (cut->a0 != PROTECT_NONE)
      set_feature(SR1, PROTECT_NONE);
   if (cut->b0 != sr2_ecc_on)
      set_feature(SR2, sr2_ecc_on);
}

static void line_cut(line_t *out, const cut_t *cut)
{
   line_key_us(out, " delay_us=", cut->delay_cycles);
   line_key_us(out, " reset_us=", cut->reset_cycles);
   line_key_u32(out, " ready=", cut->ready ? 1 : 0);
   line_key_hex(out, " sr3=", cut->status, 2);
}


// Phase P: Program Sweep ----------------------------------------------------------------------------------------------

static void program_trial(uint32_t delay_index, uint32_t block, uint32_t offset, program_summary_t *summary)
{
   const uint32_t page = (block * PART_PAGES_PER_BLOCK) + offset, nominal_us = program_delays_us[delay_index];
   hw_fill_pattern(pattern, PART_PAGE_BYTES, 0x1A700000u + (delay_index * PART_PAGES_PER_BLOCK) + offset);

   // The program, stopped
   cut_t cut;
   load_page(pattern);
   cut_short(CMD_PROGRAM_EXECUTE, page, nominal_us, &cut);

   // Three raw reads. The first is what the recovery's blank check would have seen; the other two whether the cells
   // read the same twice
   read_raw(page, first_read);
   against_pattern_t counts;
   count_against_pattern(first_words, &counts);
   const bool blank = nandlog_bytes_erased(first_read, PART_RAW_BYTES);
   uint32_t blank_reads = blank ? 1 : 0, flips = 0;
   for (uint32_t again = 0; again < 2; ++again)
   {
      read_raw(page, again_read);
      blank_reads += nandlog_bytes_erased(again_read, PART_RAW_BYTES) ? 1 : 0;
      const uint32_t differing = differing_bits(first_words, again_words, PART_RAW_WORDS);
      flips = (differing > flips) ? differing : flips;
   }
   const bool complete = (counts.progress == counts.pattern_zeros) && !counts.wrong;

   // As the log would read it, ECC on
   const uint32_t eccs = read_with_ecc(page, again_read);
   const bool match = !memcmp(again_read, pattern, PART_PAGE_BYTES);

   // The residual risk: a page the blank check passes is programmed again after the reboot, the same way, to the end
   uint32_t second_cycles = 0, second_eccs = 0;
   uint8_t second_status = 0;
   bool second_match = false;
   against_pattern_t second_counts;
   memset(&second_counts, 0, sizeof(second_counts));
   if (blank)
   {
      manual_program(page, pattern, &second_cycles, &second_status);
      second_eccs = read_with_ecc(page, again_read);
      second_match = !memcmp(again_read, pattern, PART_PAGE_BYTES);
      read_raw(page, first_read);
      count_against_pattern(first_words, &second_counts);
   }

   line_t *out = &line;
   line_reset(out);
   line_key_u32(out, "INTR_PROG trial=", program_trials++);
   line_key_u32(out, " nominal_us=", nominal_us);
   line_key_u32(out, " block=", block);
   line_key_u32(out, " page=", offset);
   line_cut(out, &cut);
   line_key_u32(out, " p_fail=", (cut.status & SR_PROGRAM_FAILURE) ? 1 : 0);
   line_key_hex(out, " a0=", cut.a0, 2);
   line_key_hex(out, " b0=", cut.b0, 2);
   line_key_u32(out, " pattern_zeros=", counts.pattern_zeros);
   line_key_u32(out, " progress=", counts.progress);
   line_key_u32(out, " wrong=", counts.wrong);
   line_key_u32(out, " spare_zeros=", counts.spare_zeros);
   line_key_u32(out, " blank=", blank ? 1 : 0);
   line_key_u32(out, " blank_reads=", blank_reads);
   line_key_u32(out, " flips=", flips);
   line_key_u32(out, " unstable=", flips ? 1 : 0);
   line_key_str(out, " class=", blank ? "blank" : (complete ? "complete" : "partial"));
   line_key_str(out, " ecc=", ecc_name(eccs));
   line_key_u32(out, " match=", match ? 1 : 0);
   line_key_u32(out, " second=", blank ? 1 : 0);
   if (blank)
   {
      line_key_us(out, " second_us=", second_cycles);
      line_key_u32(out, " second_p_fail=", (second_status & SR_PROGRAM_FAILURE) ? 1 : 0);
      line_key_str(out, " second_ecc=", ecc_name(second_eccs));
      line_key_u32(out, " second_match=", second_match ? 1 : 0);
      line_key_u32(out, " second_missing=", second_counts.pattern_zeros - second_counts.progress);
      line_key_u32(out, " second_wrong=", second_counts.wrong);
   }
   line_print(out);

   ++summary->n;
   summary->not_ready += cut.ready ? 0 : 1;
   summary->p_fail += (cut.status & SR_PROGRAM_FAILURE) ? 1 : 0;
   summary->blank += blank ? 1 : 0;
   summary->complete += (!blank && complete) ? 1 : 0;
   summary->partial += (!blank && !complete) ? 1 : 0;
   ++summary->ecc[eccs];
   summary->match += match ? 1 : 0;
   summary->unstable += flips ? 1 : 0;
   if (blank)
   {
      ++summary->second;
      summary->second_p_fail += (second_status & SR_PROGRAM_FAILURE) ? 1 : 0;
      ++summary->second_outcome[outcome_of(second_eccs, second_match)];
   }
   spans_add(&summary->spans, cut.delay_cycles, cut.reset_cycles);
   now_cycles();
}

static void program_sweep(void)
{
   section("P. Program sweep: PROGRAM EXECUTE, a delay, RESET");
   print("  %u delays x %u pages, each delay in a freshly erased block, its pages programmed in order\n",
         (uint32_t)COUNT_OF(program_delays_us), (uint32_t)INTR_PAGES_PER_PROGRAM_DELAY);
   for (uint32_t d = 0; d < COUNT_OF(program_delays_us); ++d)
   {
      program_summary_t summary;
      memset(&summary, 0, sizeof(summary));
      spans_reset(&summary.spans);
      const uint32_t block = blocks[d % INTR_BLOCKS];
      uint32_t busy = 0;
      if (!erase_fully(block, &busy))
         intr_error("block would not erase before a program delay; delay skipped", block);
      else
         for (uint32_t offset = 0; offset < INTR_PAGES_PER_PROGRAM_DELAY; ++offset)
            program_trial(d, block, offset, &summary);
      if (!erase_fully(block, &busy))
         intr_error("block would not erase after a program delay", block);

      line_t *out = &line;
      line_reset(out);
      line_key_u32(out, "INTR_PROG_SUM nominal_us=", program_delays_us[d]);
      line_key_u32(out, " n=", summary.n);
      line_spans(out, &summary.spans, summary.n);
      line_key_u32(out, " not_ready=", summary.not_ready);
      line_key_u32(out, " p_fail=", summary.p_fail);
      line_key_u32(out, " blank=", summary.blank);
      line_key_u32(out, " partial=", summary.partial);
      line_key_u32(out, " complete=", summary.complete);
      line_ecc_counts(out, summary.ecc);
      line_key_u32(out, " match=", summary.match);
      line_key_u32(out, " unstable=", summary.unstable);
      line_key_u32(out, " second=", summary.second);
      line_outcomes(out, "second_", summary.second_outcome);
      line_key_u32(out, " second_p_fail=", summary.second_p_fail);
      line_print(out);
   }
}


// Phase E: Erase Sweep ------------------------------------------------------------------------------------------------

static uint32_t fill_block(uint32_t block, uint32_t seed)
{
   // Every page, in order, ECC on, as the log fills a block; each program's busy time joins the reference. Returns
   // how many reported a failure
   uint32_t failures = 0;
   for (uint32_t p = 0; p < PART_PAGES_PER_BLOCK; ++p)
   {
      uint32_t busy = 0;
      uint8_t status = 0;
      hw_fill_pattern(pattern, PART_PAGE_BYTES, seed + p);
      if (manual_program((block * PART_PAGES_PER_BLOCK) + p, pattern, &busy, &status))
         reference_add(&program_reference, busy);
      else
         ++failures;
   }
   return failures;
}

typedef struct
{
   bool blank;
   uint8_t status;
   uint32_t busy_cycles, eccs, missing, wrong;
   bool match;
} weak_t;

static void weak_program(uint32_t page, uint32_t seed, bool blank, weak_t *weak)
{
   // A page of a block an erase was stopped in, programmed without erasing again -- as the log would, had the blank
   // check passed it -- then read back with ECC on and raw
   hw_fill_pattern(pattern, PART_PAGE_BYTES, seed);
   weak->blank = blank;
   manual_program(page, pattern, &weak->busy_cycles, &weak->status);
   weak->eccs = read_with_ecc(page, again_read);
   weak->match = !memcmp(again_read, pattern, PART_PAGE_BYTES);
   read_raw(page, first_read);
   against_pattern_t counts;
   count_against_pattern(first_words, &counts);
   weak->missing = counts.pattern_zeros - counts.progress;
   weak->wrong = counts.wrong;
}

static void line_weak(line_t *out, const char *prefix, const weak_t *weak)
{
   static const char *const fields[7] = { "blank=", "us=", "p_fail=", "ecc=", "match=", "missing=", "wrong=" };
   for (uint32_t i = 0; i < 7; ++i)
   {
      line_str(out, " ");
      line_str(out, prefix);
      line_str(out, fields[i]);
      switch (i)
      {
         case 0:  line_u32(out, weak->blank ? 1 : 0); break;
         case 1:  line_fixed(out, cycles_to_ns(weak->busy_cycles), 1000, 3); break;
         case 2:  line_u32(out, (weak->status & SR_PROGRAM_FAILURE) ? 1 : 0); break;
         case 3:  line_str(out, ecc_name(weak->eccs)); break;
         case 4:  line_u32(out, weak->match ? 1 : 0); break;
         case 5:  line_u32(out, weak->missing); break;
         default: line_u32(out, weak->wrong); break;
      }
   }
}

static void erase_trial(uint32_t delay_index, uint32_t block, erase_summary_t *summary)
{
   const uint32_t first_page = block * PART_PAGES_PER_BLOCK, nominal_us = erase_delays_us[delay_index], trial = erase_trials++;
   uint32_t busy = 0;
   if (!erase_fully(block, &busy))
   {
      intr_error("block would not erase before an erase trial; trial skipped", block);
      return;
   }
   const uint32_t fill_failures = fill_block(block, 0xE7A50000u + (trial * PART_PAGES_PER_BLOCK));

   // The erase, stopped
   cut_t cut;
   port_write(CMD_WRITE_ENABLE, NULL, 0, NULL, 0);
   cut_short(CMD_BLOCK_ERASE, first_page, nominal_us, &cut);

   // Every page as the cells hold it, and as the part's ECC makes of it
   static uint32_t zeros[PART_PAGES_PER_BLOCK];
   static char ecc_map[PART_PAGES_PER_BLOCK + 1];
   uint32_t blank_pages = 0, max_zeros = 0, total_zeros = 0, ecc_ff = 0, ecc_counts[4] = { 0, 0, 0, 0 };
   for (uint32_t p = 0; p < PART_PAGES_PER_BLOCK; ++p)
   {
      read_raw(first_page + p, first_read);
      zeros[p] = zero_bits(first_words, PART_RAW_WORDS);
      blank_pages += zeros[p] ? 0 : 1;
      max_zeros = (zeros[p] > max_zeros) ? zeros[p] : max_zeros;
      total_zeros += zeros[p];
      const uint32_t eccs = read_with_ecc(first_page + p, again_read);
      ecc_map[p] = (char)('0' + eccs);
      ++ecc_counts[eccs];
      ecc_ff += nandlog_bytes_erased(again_read, PART_PAGE_BYTES) ? 1 : 0;
   }
   ecc_map[PART_PAGES_PER_BLOCK] = '\0';

   // The weak-erase check, without erasing again: the first page, then the last, so the block is still programmed in
   // order
   weak_t weak[2];
   weak_program(first_page, 0x3EA40000u + (trial * 2), !zeros[0], &weak[0]);
   weak_program(first_page + PART_PAGES_PER_BLOCK - 1, 0x3EA40001u + (trial * 2), !zeros[PART_PAGES_PER_BLOCK - 1], &weak[1]);

   // Put back
   uint32_t restore_cycles = 0;
   const bool restored = erase_fully(block, &restore_cycles);
   if (!restored)
      intr_error("block would not erase after an erase trial", block);

   line_t *out = &line;
   line_reset(out);
   line_key_u32(out, "INTR_ERASE trial=", trial);
   line_key_u32(out, " nominal_us=", nominal_us);
   line_key_u32(out, " block=", block);
   line_cut(out, &cut);
   line_key_u32(out, " e_fail=", (cut.status & SR_ERASE_FAILURE) ? 1 : 0);
   line_key_hex(out, " a0=", cut.a0, 2);
   line_key_hex(out, " b0=", cut.b0, 2);
   line_key_u32(out, " fill_failures=", fill_failures);
   line_key_u32(out, " blank_pages=", blank_pages);
   line_key_u32(out, " max_zeros=", max_zeros);
   line_key_u32(out, " total_zeros=", total_zeros);
   line_key_u32(out, " ecc_ff=", ecc_ff);
   line_weak(out, "first_", &weak[0]);
   line_weak(out, "last_", &weak[1]);
   line_key_us(out, " restore_us=", restore_cycles);
   line_key_u32(out, " restore_fail=", restored ? 0 : 1);
   line_str(out, " zeros=");
   for (uint32_t p = 0; p < PART_PAGES_PER_BLOCK; ++p)
   {
      line_str(out, p ? "," : "");
      line_u32(out, zeros[p]);
   }
   line_key_str(out, " ecc_map=", ecc_map);
   line_print(out);

   ++summary->n;
   summary->not_ready += cut.ready ? 0 : 1;
   summary->e_fail += (cut.status & SR_ERASE_FAILURE) ? 1 : 0;
   summary->fill_failures += fill_failures;
   summary->blank_blocks += (blank_pages == PART_PAGES_PER_BLOCK) ? 1 : 0;
   summary->pages += PART_PAGES_PER_BLOCK;
   summary->blank_pages += blank_pages;
   for (uint32_t i = 0; i < 4; ++i)
      summary->ecc[i] += ecc_counts[i];
   summary->ecc_ff += ecc_ff;
   summary->zeros += total_zeros;
   summary->max_zeros = (max_zeros > summary->max_zeros) ? max_zeros : summary->max_zeros;
   ++summary->first_outcome[outcome_of(weak[0].eccs, weak[0].match)];
   ++summary->last_outcome[outcome_of(weak[1].eccs, weak[1].match)];
   summary->weak_p_fail += ((weak[0].status & SR_PROGRAM_FAILURE) ? 1 : 0) + ((weak[1].status & SR_PROGRAM_FAILURE) ? 1 : 0);
   spans_add(&summary->spans, cut.delay_cycles, cut.reset_cycles);
   now_cycles();
}

static void erase_sweep(void)
{
   section("E. Erase sweep: BLOCK ERASE of a full block, a delay, RESET");

   // The reference first: each block filled and erased whole, so the delays can be read against what an erase that
   // nobody stops takes on these blocks today
   for (uint32_t round = 0; round < INTR_REFERENCE_ROUNDS; ++round)
      for (uint32_t i = 0; i < INTR_BLOCKS; ++i)
      {
         uint32_t busy = 0;
         if (!erase_fully(blocks[i], &busy))
         {
            intr_error("block would not erase before a reference fill", blocks[i]);
            continue;
         }
         if (fill_block(blocks[i], 0x5EF00000u + (((round * INTR_BLOCKS) + i) * PART_PAGES_PER_BLOCK)))
            intr_error("a reference fill reported a program failure", blocks[i]);
         if (manual_erase(blocks[i], &busy))
            reference_add(&erase_reference, busy);
         else
            intr_error("a full block failed its reference erase", blocks[i]);
         now_cycles();
      }
   print("  reference: %u full-block erases and %u page programs timed\n", erase_reference.n, program_reference.n);
   print("  %u delays x %u blocks, each block erased, filled, its erase stopped, read, test-programmed and erased\n",
         (uint32_t)COUNT_OF(erase_delays_us), (uint32_t)INTR_BLOCKS_PER_ERASE_DELAY);

   for (uint32_t d = 0; d < COUNT_OF(erase_delays_us); ++d)
   {
      erase_summary_t summary;
      memset(&summary, 0, sizeof(summary));
      spans_reset(&summary.spans);
      for (uint32_t k = 0; k < INTR_BLOCKS_PER_ERASE_DELAY; ++k)
         erase_trial(d, blocks[((d * INTR_BLOCKS_PER_ERASE_DELAY) + k) % INTR_BLOCKS], &summary);

      line_t *out = &line;
      line_reset(out);
      line_key_u32(out, "INTR_ERASE_SUM nominal_us=", erase_delays_us[d]);
      line_key_u32(out, " n=", summary.n);
      line_spans(out, &summary.spans, summary.n);
      line_key_u32(out, " not_ready=", summary.not_ready);
      line_key_u32(out, " e_fail=", summary.e_fail);
      line_key_u32(out, " fill_failures=", summary.fill_failures);
      line_key_u32(out, " blank_blocks=", summary.blank_blocks);
      line_key_u32(out, " pages=", summary.pages);
      line_key_u32(out, " blank_pages=", summary.blank_pages);
      line_ecc_counts(out, summary.ecc);
      line_key_u32(out, " ecc_ff=", summary.ecc_ff);
      line_key_u32(out, " mean_zeros=", summary.pages ? (uint32_t)(summary.zeros / summary.pages) : 0);
      line_key_u32(out, " max_zeros=", summary.max_zeros);
      line_outcomes(out, "first_", summary.first_outcome);
      line_outcomes(out, "last_", summary.last_outcome);
      line_key_u32(out, " weak_p_fail=", summary.weak_p_fail);
      line_print(out);
   }
}


// Phase F: Leave Every Block Erased -----------------------------------------------------------------------------------

static void final_pass(void)
{
   section("F. Every block erased, and read back raw");
   for (uint32_t i = 0; i < INTR_BLOCKS; ++i)
   {
      uint32_t busy = 0, blank_pages = 0;
      const bool erased = erase_fully(blocks[i], &busy);
      for (uint32_t p = 0; p < PART_PAGES_PER_BLOCK; ++p)
      {
         read_raw((blocks[i] * PART_PAGES_PER_BLOCK) + p, first_read);
         blank_pages += nandlog_bytes_erased(first_read, PART_RAW_BYTES) ? 1 : 0;
      }
      line_t *out = &line;
      line_reset(out);
      line_key_u32(out, "INTR_FINAL block=", blocks[i]);
      line_key_us(out, " erase_us=", busy);
      line_key_u32(out, " e_fail=", erased ? 0 : 1);
      line_key_u32(out, " blank_pages=", blank_pages);
      line_key_u32(out, " pages=", PART_PAGES_PER_BLOCK);
      line_print(out);
      if (!erased || (blank_pages != PART_PAGES_PER_BLOCK))
         intr_error("block not left erased in every bit: erase it before the log uses it", blocks[i]);
   }
}


// Main ----------------------------------------------------------------------------------------------------------------

static void restore_part(void)
{
   // Every way out passes here: ECC on as the driver configured it, the array locked and WP# low, then the port put
   // to sleep. The part is waited for first, because it ignores SET FEATURE while busy (datasheet, Figure 4-2)
   if (!part_up)
      return;
   wait_ready(NULL);
   set_feature(SR2, sr2_ecc_on);
   close_write_protection();
   print("INTR_INFO restored sr1=%02X sr2=%02X\n", get_feature(SR1), get_feature(SR2));
   nandlog_chip_low_power(true);
   nandlog_port_power(false);
   part_up = false;
}

static void halt(void)
{
   restore_part();
   print("INTR_DONE errors=%u aborted=1 program_trials=%u erase_trials=%u elapsed_s=%u\n", errors, program_trials,
         erase_trials, elapsed_s());
   while (true)
      am_hal_delay_us(1000000);
}

static void print_header(void)
{
   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   static const uint8_t zero = 0x00, one = 0x01;
   uint8_t id[3] = { 0, 0, 0 }, device = 0;
   port_read(CMD_READ_ID, &zero, 1, id, sizeof(id));
   port_read(CMD_READ_ID, &one, 1, &device, 1);
   line_t *out = &line;
   line_reset(out);
   line_key_u32(out, "INTR_START nandlog=", (uint32_t)NANDLOG_VERSION_MAJOR);
   line_key_u32(out, ".", (uint32_t)NANDLOG_VERSION_MINOR);
   line_key_str(out, " source=", NANDLOG_SOURCE_ID);
   line_key_hex(out, " part_id=", ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2], 6);
   line_key_hex(out, " device_id=", device, 2);
   line_key_u32(out, " page=", geometry->page_size_bytes);
   line_key_u32(out, " spare=", geometry->spare_size_bytes);
   line_key_u32(out, " pages_per_block=", geometry->pages_per_block);
   line_key_u32(out, " blocks=", geometry->block_count);
   line_key_u32(out, " reserved=", geometry->reserved_blocks);
   line_str(out, " cycles_per_us=");
   line_fixed(out, cycles_per_100ms, 100000, 3);
   line_print(out);

   line_reset(out);
   line_str(out, "INTR_INFO program_delays_us=");
   for (uint32_t d = 0; d < COUNT_OF(program_delays_us); ++d)
   {
      line_str(out, d ? "," : "");
      line_u32(out, program_delays_us[d]);
   }
   line_key_u32(out, " pages_per_delay=", INTR_PAGES_PER_PROGRAM_DELAY);
   line_str(out, " erase_delays_us=");
   for (uint32_t d = 0; d < COUNT_OF(erase_delays_us); ++d)
   {
      line_str(out, d ? "," : "");
      line_u32(out, erase_delays_us[d]);
   }
   line_key_u32(out, " blocks_per_delay=", INTR_BLOCKS_PER_ERASE_DELAY);
   line_print(out);
}

static void check_ecc_switch(void)
{
   // Every raw read depends on ECC really going off, and every program on it coming back on: checked once, through
   // the register itself, before anything is written
   const uint8_t configured = get_feature(SR2);
   sr2_ecc_on = (uint8_t)(configured | SR2_ECC_ENABLE);
   sr2_ecc_off = (uint8_t)(configured & ~SR2_ECC_ENABLE);
   set_feature(SR2, sr2_ecc_off);
   const uint8_t off = get_feature(SR2);
   set_feature(SR2, sr2_ecc_on);
   const uint8_t on = get_feature(SR2);
   print("INTR_INFO sr1=%02X sr2=%02X sr3=%02X ecc_off_reads=%02X ecc_on_reads=%02X\n", get_feature(SR1), configured,
         get_feature(SR3), off, on);
   if ((off != sr2_ecc_off) || (on != sr2_ecc_on))
   {
      intr_error("ECC could not be switched off and on again through B0h; nothing written", 0);
      halt();
   }
}

int main(void)
{
   setup_hardware();
   hw_timing_init();
   calibrate_clock();

   print("\n============================================================\n");
   print("nandlog on-device characterization: interrupted program and erase\n");
   print("*** DESTRUCTIVE: PROGRAMS AND ERASES %u BLOCKS OF THE LOG REGION FROM BLOCK %u ***\n", (uint32_t)INTR_BLOCKS,
         (uint32_t)INTR_FIRST_BLOCK);
   print("Expected run time: under a minute. It ends with INTR_DONE.\n");
   print("============================================================\n");
   if (!cycles_per_us)
   {
      intr_error("DWT is not counting: attach the debugger and run again; no delay here would mean anything", 0);
      halt();
   }

   // The port and the driver, and nothing of the log: the driver's bring-up configures the part and loads its
   // bad-block table, which is what says whether a block may be used
   if (!nandlog_port_init() || !nandlog_chip_probe())
   {
      intr_error("the port would not open, or no part answered", 0);
      halt();
   }
   nandlog_chip_init();
   nandlog_port_power(true);
   nandlog_chip_low_power(false);
   part_up = true;
   system_enable_interrupts(true);

   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   if ((geometry->page_size_bytes != PART_PAGE_BYTES) || (geometry->spare_size_bytes != PART_SPARE_BYTES) ||
       (geometry->pages_per_block != PART_PAGES_PER_BLOCK))
   {
      intr_error("this is not the 4,096+256-byte, 64-page part the test was written for; nothing written", 0);
      halt();
   }
   print_header();
   check_ecc_switch();
   choose_blocks();

   open_write_protection();
   program_sweep();
   erase_sweep();
   final_pass();

   section("G. Done");
   reference_report("program_busy", &program_reference);
   reference_report("erase_busy_full_block", &erase_reference);
   restore_part();
   print("INTR_DONE errors=%u aborted=0 program_trials=%u erase_trials=%u elapsed_s=%u\n", errors, program_trials,
         erase_trials, elapsed_s());
   while (true)
      am_hal_delay_us(1000000);
}
