// ON-DEVICE SOAK -- the durability contract under real power cuts
//
// *****************************************************************************************************************
// ***  WRITES TO THE LOG REGION AND BEGINS EPOCHS OF ITS OWN. WHATEVER THE CURRENT EPOCH HELD IS LEFT BEHIND AND  ***
// ***  SOON SWEPT AWAY. RUN IT ON A DEVELOPMENT BOARD ONLY, WITH THE BATTERY DISCONNECTED (see tests/README.md).  ***
// *****************************************************************************************************************
//
// WHAT IT DOES, ON EVERY BOOT:
//   1. brings the log up, timing nandlog_init() -- which after a cut is the recovery;
//   2. reads back the WHOLE current epoch through the public API and checks every record against the pattern it
//      was written from. Each record carries the run (= boot) that wrote it and its index within that run, and the
//      rest of its bytes follow from those two by a small PRNG, so damage, reordering and duplication all show;
//   3. prints one POWERCUT line summarizing what it found, protected by a CRC so that a line a cut tore is never
//      mistaken for a whole one;
//   4. logs records of varying size, with partial-page flushes and idle flushes at random intervals, printing a
//      POWERCUT_ACK each time a flush(true) returns NANDLOG_OK -- the records up to that index are then durable --
//      and carries on until the power goes.
//
// The run number needs no NVM of its own: it is derived from the log. Each run's first record is a boot marker,
// committed at once; the next boot takes one more than the highest run it finds, or the base the epoch's details
// name. A boot cut before its marker was committed left nothing behind, so its number is simply used again.
//
// The current epoch is replaced at boot once it holds PC_ROLLOVER_PAGES pages or PC_ROLLOVER_RUNS runs, which keeps
// the per-boot read-back to a few seconds and the per_run list short. An epoch this program did not begin -- left by
// the benchmark, or by SociTrack -- is reported as foreign=1, not checked, and replaced.
//
// THE CHECKER. tests/powercut_check.py reads the captured serial log and checks the contract across boots: no
// damaged, duplicated or out-of-order record is ever delivered; no earlier run's record count ever changes between
// boots; and every record acknowledged as durable is still there at every later boot of that epoch.
//
// LINES (all on their own line; the ones with crc= carry a CRC-32 of everything before "crc="):
//    POWERCUT_START nandlog=<major.minor> source=<id> port_crc=<0|1> erase_ahead=<W> head_pass=<0|1>
//       (source -- a fingerprint of the logging sources, which the SociTrack test Makefile supplies -- names the build
//        a capture ran, so captures from different builds are not counted together unknowingly)
//    POWERCUT boot=<run> epoch=<e> foreign=<0|1> runs=<k> records=<n> pages=<p> gaps=<g> tombstones=<t> damaged=<d>
//             out_of_order=<o> dup=<u> holes=<h> skipped=<s> retired=<r> init_us=<us> verify_us=<us>
//             per_run=<run:count[:prefix],...|-> crc=<8 hex>
//       (prefix -- how many records from index 0 are all present -- appears only for a run with a hole in it)
//    POWERCUT_EPOCH epoch=<e> base_run=<run> crc=<8 hex>
//    POWERCUT_ACK epoch=<e> run=<run> upto=<index> crc=<8 hex>
//    POWERCUT_ERR <what> status=<nandlog_status_t> index=<i>       POWERCUT_IDLE <why>       POWERCUT_FATAL <why>
//
// RUNNING UNATTENDED. Two build options, both 0 by default, so the plain soak and its host build (make soak) are as
// above; see "Running Unattended" below for what they do and why:
//    PC_SELF_CUT=1         the firmware stops one of its own programs or erases with the part's RESET at a random
//                          point, then resets the MCU: a RESET-interrupted operation, a proxy for a brown-out, made
//                          with no power switch and with the battery left connected (target nandlog_selfcut)
//    PC_INJECT_FAILURES=1  now and then a completed program or erase reports failure, so the relocation and
//                          retirement paths run on the part; combinable with the first (nandlog_selfcut_inject)
// Either one adds to POWERCUT_START, and prints, each line CRC-protected:
//    POWERCUT_START ... self_cut=<0|1> inject=<0|1>
//    POWERCUT_SELFCUT op=<index> cmd=<program|erase> phase=<init|logging> plan=<init|early|late|recovery>
//                     target=<page|slot|table|copy|other|log|ring|reserve> block=<b> delay_us=<us> reset_us=<us>
//                     ready=<0|1> retained=<n> crc=<8 hex>
//    POWERCUT_INJECT op=<index> cmd=<program|erase> phase=<init|logging> block=<b> retired=<r> crc=<8 hex>
//
// WHAT TO SEND BACK: the captured serial log (or at least powercut_check.py's summary of it), and how the cuts were
// made. Any number of cuts is useful; a thousand is the goal.

#include <string.h>
#include "nandlog_hw_test.h"

#if !NANDLOG_RECORD_FRAMING
#error "The power-cut soak walks every page record by record, which needs NANDLOG_RECORD_FRAMING"
#endif


// Parameters ----------------------------------------------------------------------------------------------------------

#define PC_DETAILS_MAGIC                            0x54554350u    // 'PCUT'
#define PC_ROLLOVER_PAGES                           2048           // begin a new epoch at boot past this many pages...
#define PC_ROLLOVER_RUNS                            48             // ...or this many runs
#define PC_HARD_CAP_PAGES                           4096           // stop logging for this boot if one run gets this far
#define PC_MAX_RUNS                                 128            // runs tracked per epoch
#define PC_RUN_LIMIT                                10000000u      // a run number above this is damage, not a run
#ifndef NANDLOG_SOURCE_ID
#define NANDLOG_SOURCE_ID                           "unrecorded"   // the build's fingerprint of the logging sources
#endif
#define PC_TYPE_BOOT                                'B'
#define PC_TYPE_DATA                                'D'
#define PC_RECORD_HEADER_BYTES                      10             // run (4), index (4), payload length (2)
#define PC_MAX_PAYLOAD_BYTES                        1999
#define PC_RECORD_PREFIX_BYTES                      5              // record type (1) and timestamp (4), ahead of the data
#ifndef PC_SELF_CUT
#define PC_SELF_CUT                                 0              // 1: the firmware stops its own operations (below)
#endif
#ifndef PC_INJECT_FAILURES
#define PC_INJECT_FAILURES                          0              // 1: now and then an operation reports failure (below)
#endif
#define PC_UNATTENDED                               (PC_SELF_CUT || PC_INJECT_FAILURES)

// The mode joins POWERCUT_START's build fields only in the builds that have one, so the plain soak's line is unchanged
#if PC_UNATTENDED
#define PC_START_MODE_FORMAT                        " self_cut=%u inject=%u"
#define PC_START_MODE_VALUES                        , (uint32_t)PC_SELF_CUT, (uint32_t)PC_INJECT_FAILURES
#else
#define PC_START_MODE_FORMAT                        ""
#define PC_START_MODE_VALUES
#endif

typedef struct __attribute__ ((__packed__))
{
   uint32_t magic;               // PC_DETAILS_MAGIC
   uint32_t base_run;            // the run that began the epoch; runs in it are numbered from here
   uint32_t format;              // 1
} pc_details_t;

static uint8_t page_buffer[NANDLOG_MAX_PAGE_SIZE_BYTES];
static uint8_t expected[PC_RECORD_HEADER_BYTES + PC_MAX_PAYLOAD_BYTES + 1];
static uint8_t record[PC_RECORD_HEADER_BYTES + PC_MAX_PAYLOAD_BYTES + 1];


// The Pattern ---------------------------------------------------------------------------------------------------------
//
// Everything about a record follows from (run, index): its type, its timestamp, its length and every byte of it

static uint32_t mix32(uint32_t x)
{
   x ^= x >> 16;
   x *= 0x7FEB352Du;
   x ^= x >> 15;
   x *= 0x846CA68Bu;
   x ^= x >> 16;
   return x;
}

static uint32_t record_seed(uint32_t run, uint32_t index)
{
   return mix32((run * 0x9E3779B1u) ^ mix32(index ^ 0x6A09E667u));
}

static uint32_t payload_length(uint32_t run, uint32_t index)
{
   // Mostly small records, some medium, a few large, so pages fill at every rate
   if (!index)
      return 16;
   const uint32_t r = mix32(record_seed(run, index) ^ 0xA5A5A5A5u), bucket = r % 100;
   if (bucket < 70)
      return (r >> 8) % 64;
   if (bucket < 95)
      return 64 + ((r >> 8) % 448);
   return 512 + ((r >> 8) % (PC_MAX_PAYLOAD_BYTES - 511));
}

static uint32_t build_record(uint8_t *out, uint32_t run, uint32_t index)
{
   const uint32_t length = payload_length(run, index);
   const uint16_t length16 = (uint16_t)length;
   memcpy(out, &run, sizeof(run));
   memcpy(out + 4, &index, sizeof(index));
   memcpy(out + 8, &length16, sizeof(length16));
   uint32_t state = record_seed(run, index) | 1u;
   for (uint32_t i = 0; i < length; ++i)
   {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      out[PC_RECORD_HEADER_BYTES + i] = (uint8_t)(state >> 24);
   }
   return PC_RECORD_HEADER_BYTES + length;
}

static bool record_matches(const uint8_t *framed, uint32_t framed_bytes, uint32_t *run, uint32_t *index)
{
   // 'framed' is what nandlog_framed_next_record() hands back: [type 1][timestamp 4][data]
   if (framed_bytes < (PC_RECORD_PREFIX_BYTES + PC_RECORD_HEADER_BYTES))
      return false;
   uint32_t timestamp = 0;
   memcpy(&timestamp, framed + 1, sizeof(timestamp));
   const uint8_t *data = framed + PC_RECORD_PREFIX_BYTES;
   memcpy(run, data, sizeof(*run));
   memcpy(index, data + 4, sizeof(*index));
   if (!*run || (*run > PC_RUN_LIMIT))
      return false;
   if ((framed[0] != (*index ? PC_TYPE_DATA : PC_TYPE_BOOT)) || (timestamp != *index))
      return false;
   const uint32_t length = build_record(expected, *run, *index);
   return ((framed_bytes - PC_RECORD_PREFIX_BYTES) == length) && !memcmp(data, expected, length);
}


// Output Lines, CRC-Protected -----------------------------------------------------------------------------------------

static char line[2600];
static uint32_t line_length;

static void line_begin(const char *tag)
{
   line_length = 0;
   line[0] = '\0';
   for (; *tag && (line_length < (sizeof(line) - 32)); ++tag)
      line[line_length++] = *tag;
   line[line_length] = '\0';
}

static void line_str(const char *text)
{
   for (; *text && (line_length < (sizeof(line) - 32)); ++text)
      line[line_length++] = *text;
   line[line_length] = '\0';
}

static void line_u32(uint32_t value)
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
   line_str(text);
}

static void line_field(const char *key, uint32_t value)
{
   // " key=value"
   line_str(" ");
   line_str(key);
   line_str("=");
   line_u32(value);
}

static uint32_t crc32(const void *data, uint32_t length)
{
   // IEEE CRC-32, as Python's zlib.crc32 computes it; bit by bit, because nothing here is in a hurry
   const uint8_t *bytes = (const uint8_t*)data;
   uint32_t crc = 0xFFFFFFFFu;
   for (uint32_t i = 0; i < length; ++i)
   {
      crc ^= bytes[i];
      for (uint32_t bit = 0; bit < 8; ++bit)
         crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
   }
   return ~crc;
}

static void line_emit(void)
{
   // CRC over everything up to and including the space before "crc=", then printed in pieces small enough for any
   // printf buffer; the checker sees one line, because nothing but the final piece carries a newline
   static const char hex[] = "0123456789ABCDEF";
   line_str(" ");
   const uint32_t crc = crc32(line, line_length);
   line_str("crc=");
   char crc_text[9];
   for (uint32_t i = 0; i < 8; ++i)
      crc_text[i] = hex[(crc >> (28 - (4 * i))) & 0xF];
   crc_text[8] = '\0';
   line_str(crc_text);
   char piece[129];
   for (uint32_t offset = 0; offset < line_length; offset += (sizeof(piece) - 1))
   {
      const uint32_t count = ((line_length - offset) < (sizeof(piece) - 1)) ? (line_length - offset) : (sizeof(piece) - 1);
      memcpy(piece, line + offset, count);
      piece[count] = '\0';
      print("%s", piece);
   }
   print("\n");
}


// Running Unattended --------------------------------------------------------------------------------------------------
//
// Only in a build that sets PC_SELF_CUT or PC_INJECT_FAILURES; the plain soak compiles the empty hooks at the end of
// this section instead. The port's two transfer functions are wrapped at link time, as for the benchmark --
//     -Wl,--wrap=nandlog_port_transfer_read,--wrap=nandlog_port_transfer_write
// -- so every command the library and the driver send the part passes through here first. Nothing in the library,
// the drivers or storage.c changes, and a build linked without the flags says so at its first boot and stops.
//
// SELF-CUT. Every PROGRAM EXECUTE and BLOCK ERASE is counted from the start of the boot, so the boot's own slot,
// table and window writes count with the rest, and one of them is stopped part-way: the command goes out; a random
// delay from 0 to 110% of that operation's busy time on this part passes on the cycle counter, interrupts off; RESET
// (FFh) goes out through the real port -- the datasheet says it "stops all operations"; the status register is
// polled until OIP clears; a POWERCUT_SELFCUT line is printed and drained from SWO; and the MCU resets itself. The
// next boot then recovers from an interrupted operation with everything in RAM lost, as after a power cut. The part,
// though, stays powered throughout, so this is a RESET-INTERRUPTED operation, a proxy for a brown-out and not a power
// cut, as in test_nandlog_interrupt.c. A boot that runs out of things to do resets itself after a while idle, and one
// that stopped on an error makes the next boot begin a new epoch, so the run goes on with nobody watching it.
//
// WHICH OPERATION. Each boot draws a plan before nandlog_init(). 9 in 24: one of the operations nandlog_init() and the
// boot's own writes issue (a new epoch's included), counting on as many as the last boot to reach logging issued; a
// boot that issues fewer turns the plan into the next one when logging starts, which leaves about a third of the cuts
// here (over the simulator, 2,000 boots of 8 in 24 put 28% here). 12 in 24: early in logging, 1 to 4,095 operations
// in, uniform in the logarithm, so the first commit after a boot is cut about as often as the thousandth. 3 in 24 --
// 1 in 8 -- late: between half and seven eighths of the operations the run has left before PC_HARD_CAP_PAGES, so the
// epoch grows to thousands of pages, the next boot rolls it over, and over a run the log wraps its region. No cut is
// drawn past seven eighths of those, so a boot is not left idle waiting for one.
//
// RANDOMNESS THAT SURVIVES THE RESET. The run number cannot seed it: a boot cut before its marker was committed uses
// its run again, and would be cut at the same point for ever. The random state is carried instead in a word of TCM in
// the .noinit section, which the linker script places after .bss and the startup code neither copies nor zeroes (the
// firmware's radio test carries its device list across a reset the same way); a CRC over it tells the garbage a
// power-on leaves from a state a reset left alone. Mixed into it at every boot is the STIMER's count: the HAL
// documents a software or AIRCR reset as resetting every block but the clock generator, the RTC, the STIMER and the
// PMU, so once started -- here, if nothing has -- the counter runs on through every reset and no two boots read the
// same count. Either alone keeps the run from repeating itself. retained= on each POWERCUT_SELFCUT line counts the
// boots in a row the TCM word survived, which says on the board whether it does.
//
// FAILURES INJECTED. Now and then a program or erase that completed is made to report failure: P_FAIL or E_FAIL is
// ORed into the status byte that the driver's next GET FEATURE C0h returns once the part is ready. The status is
// faked; everything after it is the driver's and the library's own handling, on the part -- the block's pages moved
// through the part's internal copy, the block retired with a new table version and a mark, the ring told. Eligible
// are a program whose load began with a log page's magic (a page or a tombstone) and an erase of a log block below
// the PC_TABLE_AREA_BLOCKS at the top of the array where the bad-block table may be kept: never a slot, a table
// version, a mark, or the ring. A block that fails an erase fails every erase of it for the rest of the boot, as a
// worn-out block does, so all NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL attempts fail and it is retired. At most one
// injection per PC_INJECT_MIN_GAP_OPS operations, counted across boots; then one eligible program in PC_INJECT_ODDS, or
// one eligible erase in a 64th of that; and none once nandlog_bad_block_count() reaches PC_INJECT_RETIRED_CAP, which
// with the datasheet's 80 factory-bad blocks makes 104 entries, against the Alliance driver's table of 256. With self-cut as well, one injection in
// PC_RECOVERY_CUT_ODDS moves the boot's cut into the operations that answer it, so cuts land in relocations and
// retirements, which nothing else in the soak reaches.

#if PC_UNATTENDED

#define PC_CMD_PROGRAM_DATA_LOAD                    0x02
#define PC_CMD_GET_FEATURE                          0x0F
#define PC_CMD_PROGRAM_EXECUTE                      0x10
#define PC_CMD_PAGE_DATA_READ                       0x13
#define PC_CMD_BLOCK_ERASE                          0xD8
#define PC_CMD_RESET                                0xFF
#define PC_STATUS_REGISTER                          0xC0
#define PC_STATUS_OIP                               0x01
#define PC_STATUS_E_FAIL                            0x04
#define PC_STATUS_P_FAIL                            0x08
#define PC_TABLE_MAGIC                              "BBT3"         // the Alliance driver's table pages begin with this

#define PC_PROGRAM_BUSY_US                          620            // a page program on the AS5F18G04SND, as measured
#define PC_ERASE_BUSY_US                            3400           // a full block's erase on it, likewise
#define PC_RESET_TIMEOUT_US                         100000         // far beyond any RESET this part performs
#define PC_SWO_DRAIN_US                             2000           // after the ITM goes idle and before the MCU resets
#define PC_IDLE_NAPS                                20             // 100 ms each: how long an idle boot waits to reset
#define PC_PLAN_SHARES                              24
#define PC_PLAN_LATE_SHARES                         3              // 1 in 8 boots cut late in logging
#define PC_PLAN_INIT_SHARES                         9              // among the boot's own operations; the rest early
#define PC_EARLY_CUT_OCTAVES                        12             // early: 1 to 4,095 operations into logging
#define PC_INIT_OPS_GUESS                           3              // until a boot has reached logging
#define PC_RING_BLOCKS                              8              // nandlog.c's METADATA_RING_BLOCKS, at the bottom
#define PC_TABLE_AREA_BLOCKS                        64             // the reserve and the 48 blocks under it (the driver's
                                                                   // NANDLOG_CHIP_TABLE_SEARCH_BLOCKS)
#ifndef PC_INJECT_RETIRED_CAP
#define PC_INJECT_RETIRED_CAP                       24
#endif
#define PC_INJECT_MIN_GAP_OPS                       300
#ifndef PC_INJECT_ODDS
#define PC_INJECT_ODDS                              300            // larger spreads the injections over more of a run
#endif
#define PC_RECOVERY_CUT_ODDS                        2
_Static_assert((PC_INJECT_RETIRED_CAP + 80) <= 128, "with 80 factory-bad blocks the cap must leave the 256-entry table half empty");
#define PC_RETAINED_MAGIC                           0x55435350u    // 'PSCU'
#define PC_UNKNOWN                                  0xFFFFFFFFu

// What an operation was writing: for a program, what the cache register was last loaded with (or that a page was
// latched into it, which makes the program the part's internal copy); for an erase, where its block lies
typedef enum { TARGET_PAGE, TARGET_SLOT, TARGET_TABLE, TARGET_COPY, TARGET_OTHER, TARGET_LOG, TARGET_RING, TARGET_RESERVE } op_target_t;
static const char *const target_names[] = { "page", "slot", "table", "copy", "other", "log", "ring", "reserve" };

typedef struct
{
   uint32_t magic;               // PC_RETAINED_MAGIC
   uint32_t state;               // the random state the next boot starts from
   uint32_t carried;             // boots in a row this survived a reset; 0 when it was made afresh
   uint32_t init_ops;            // operations the last boot to reach logging issued before it did
   uint32_t ops_since_injection; // across boots, so the gap between injections spans them
   uint32_t retired;             // nandlog_bad_block_count() as last read, or PC_UNKNOWN
   uint32_t replace_epoch;       // the last boot stopped on an error: begin a new epoch rather than meet it again
   uint32_t check;               // CRC-32 of everything above
} retained_t;

static retained_t retained __attribute__ ((section(".noinit")));

static uint32_t random_state, ops, ops_since_injection, retired_known = PC_UNKNOWN, injections_unseen, transfers_seen;
static op_target_t loaded = TARGET_OTHER;
static uint8_t inject_status;
static bool logging_phase, replace_epoch;

#if PC_SELF_CUT
typedef enum { PLAN_NONE, PLAN_INIT, PLAN_EARLY, PLAN_LATE, PLAN_RECOVERY } cut_plan_t;
static const char *const plan_names[] = { "none", "init", "early", "late", "recovery" };
static cut_plan_t plan = PLAN_NONE;
static uint32_t cut_at;                  // the operation, counted from the boot's first, that is cut; 0 until drawn
#endif
#if PC_INJECT_FAILURES
static uint32_t sticky_erase_block = PC_UNKNOWN;
#endif

void __real_nandlog_port_transfer_read(uint8_t command, const void *address, uint32_t address_length, void *read_buffer, uint32_t read_length) __attribute__((weak));
void __real_nandlog_port_transfer_write(uint8_t command, const void *address, uint32_t address_length, const void *write_buffer, uint32_t write_length) __attribute__((weak));
void __wrap_nandlog_port_transfer_read(uint8_t command, const void *address, uint32_t address_length, void *read_buffer, uint32_t read_length);
void __wrap_nandlog_port_transfer_write(uint8_t command, const void *address, uint32_t address_length, const void *write_buffer, uint32_t write_length);

static uint32_t random_next(void)
{
   // A counter through the pattern's mixer, which never sticks at a fixed point
   random_state += 0x9E3779B9u;
   return mix32(random_state);
}

static void retained_seal(void)
{
   retained.magic = PC_RETAINED_MAGIC;
   retained.check = crc32(&retained, sizeof(retained) - sizeof(retained.check));
}

static uint32_t operation_page(const void *address, uint32_t address_length, const void *data, uint32_t length)
{
   // The row address of a PROGRAM EXECUTE or BLOCK ERASE, three bytes most significant first, which both drivers send
   // as the transfer's data (the Winbond part's first byte is a dummy zero)
   const uint8_t *bytes = (length == 3) ? (const uint8_t*)data : ((address_length == 3) ? (const uint8_t*)address : NULL);
   return bytes ? (((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2]) : PC_UNKNOWN;
}

static op_target_t classify_load(const void *data, uint32_t length)
{
   uint32_t magic = 0;
   if (!data || (length < sizeof(magic)))
      return TARGET_OTHER;
   memcpy(&magic, data, sizeof(magic));
   if ((magic == NANDLOG_PAGE_MAGIC) || (magic == NANDLOG_PAGE_MAGIC_FRAMED))
      return TARGET_PAGE;
   if (magic == NANDLOG_META_MAGIC)
      return TARGET_SLOT;
   return memcmp(data, PC_TABLE_MAGIC, sizeof(magic)) ? TARGET_OTHER : TARGET_TABLE;     // or a mark, or the OTP page
}

static op_target_t erase_target(uint32_t block)
{
   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   if (block < PC_RING_BLOCKS)
      return TARGET_RING;
   return (block >= (geometry->block_count - geometry->reserved_blocks)) ? TARGET_RESERVE : TARGET_LOG;
}

static void line_operation(const char *tag, uint8_t command, uint32_t block)
{
   line_begin(tag);
   line_field("op", ops);
   line_str((command == PC_CMD_PROGRAM_EXECUTE) ? " cmd=program" : " cmd=erase");
   line_str(logging_phase ? " phase=logging" : " phase=init");
   line_field("block", block);
}

#if PC_SELF_CUT

static NANDLOG_NORETURN void reset_mcu(void)
{
   // What this boot learned goes to the next; then everything printed is let out of SWO before the reset, which would
   // otherwise cut the line short. The ITM reports idle once its FIFO is empty, and the margin covers the few bytes the
   // TPIU may still be shifting out at 1 Mbaud
   retained.ops_since_injection = ops_since_injection;
   retained_seal();
   am_hal_itm_not_busy();
   am_hal_delay_us(PC_SWO_DRAIN_US);
   NVIC_SystemReset();
}

static NANDLOG_NORETURN void cut_here(uint8_t command, const void *address, uint32_t address_length, const void *data,
                                      uint32_t length, uint32_t block, op_target_t target)
{
   // Interrupts off from the command to the end of RESET's busy wait, as in test_nandlog_interrupt.c: the transfers are
   // polled, so nothing needs them, and nothing the board does can stretch the delay. delay_us runs from the return of
   // the operation's transfer to the call of RESET's, reset_us from the return of RESET's to the poll that found OIP
   // clear. Both are measured, and the nominal delay is drawn up to 110% of the busy time, so some cuts land just after
   // the operation finished, before anything asked whether it had
   const bool program = (command == PC_CMD_PROGRAM_EXECUTE);
   const uint32_t cycles_per_us = hw_cycles_per_us(), busy_us = program ? PC_PROGRAM_BUSY_US : PC_ERASE_BUSY_US;
   const uint32_t wait = (random_next() % (((busy_us * 11) / 10) + 1)) * cycles_per_us;
   const uint8_t status_register = PC_STATUS_REGISTER;
   uint8_t status = 0xFF;
   (void)am_hal_interrupt_master_disable();
   __real_nandlog_port_transfer_write(command, address, address_length, data, length);
   const uint32_t issued = hw_cycle_count();
   while ((uint32_t)(hw_cycle_count() - issued) < wait)
      ;
   const uint32_t reset_called = hw_cycle_count();
   __real_nandlog_port_transfer_write(PC_CMD_RESET, NULL, 0, NULL, 0);
   const uint32_t reset_issued = hw_cycle_count();
   do
      __real_nandlog_port_transfer_read(PC_CMD_GET_FEATURE, &status_register, 1, &status, 1);
   while ((status & PC_STATUS_OIP) && ((uint32_t)(hw_cycle_count() - reset_issued) < (PC_RESET_TIMEOUT_US * cycles_per_us)));
   const uint32_t reset_done = hw_cycle_count();

   // The line buffer is free: every line is built between library calls, and this runs inside one
   line_operation("POWERCUT_SELFCUT", command, block);
   line_str(" plan=");
   line_str(plan_names[plan]);
   line_str(" target=");
   line_str(target_names[target]);
   line_field("delay_us", (reset_called - issued) / cycles_per_us);
   line_field("reset_us", (reset_done - reset_issued) / cycles_per_us);
   line_field("ready", (status & PC_STATUS_OIP) ? 0 : 1);
   line_field("retained", retained.carried);
   line_emit();
   reset_mcu();
}

#endif  // #if PC_SELF_CUT

#if PC_INJECT_FAILURES

static void consider_injection(uint8_t command, uint32_t page, uint32_t block, op_target_t target)
{
   // Called once the operation's command has gone out; the failure rides on the status read that ends its wait
   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   const bool program = (command == PC_CMD_PROGRAM_EXECUTE);
   if (!program && (block == sticky_erase_block))
   {
      inject_status = PC_STATUS_E_FAIL;
      return;
   }
   // An erase comes once a block, so its odds are a block's pages better, and the two kinds come about equally often
   const bool eligible = program ? (target == TARGET_PAGE) :
                         ((target == TARGET_LOG) && (block < (geometry->block_count - PC_TABLE_AREA_BLOCKS)));
   const uint32_t odds = program ? PC_INJECT_ODDS : ((PC_INJECT_ODDS / geometry->pages_per_block) + 1);
   if (!eligible || (retired_known == PC_UNKNOWN) || ((retired_known + injections_unseen) >= PC_INJECT_RETIRED_CAP) ||
       (ops_since_injection < PC_INJECT_MIN_GAP_OPS) || (random_next() % odds))
      return;
   inject_status = program ? PC_STATUS_P_FAIL : PC_STATUS_E_FAIL;
   sticky_erase_block = program ? sticky_erase_block : block;
   ops_since_injection = 0;
   ++injections_unseen;
   line_operation("POWERCUT_INJECT", command, block);
   line_field("retired", retired_known);
   line_emit();

#if PC_SELF_CUT
   // What answers the failure: after a program, the ring told of the failed page, a block erased, the pages before it
   // copied, a table version, a mark and the ring told of the retirement -- about eight operations plus the page's
   // offset in its block; after an erase, the two attempts left, a table version, a mark and the ring. The cut moves in
   // among them unless it was due there anyway
   const uint32_t span = program ? (8 + (page % geometry->pages_per_block)) : 7;
   if (((cut_at == 0) || (cut_at > (ops + span))) && !(random_next() % PC_RECOVERY_CUT_ODDS))
   {
      cut_at = ops + 1 + (random_next() % span);
      plan = PLAN_RECOVERY;
   }
#else
   (void)page;
#endif
}

#endif  // #if PC_INJECT_FAILURES

void __wrap_nandlog_port_transfer_read(uint8_t command, const void *address, uint32_t address_length, void *read_buffer, uint32_t read_length)
{
   ++transfers_seen;
   __real_nandlog_port_transfer_read(command, address, address_length, read_buffer, read_length);

   // An injected failure lands on the first status read after its operation that finds the part ready
   uint8_t *status = (uint8_t*)read_buffer;
   if (inject_status && (command == PC_CMD_GET_FEATURE) && address && (address_length >= 1) && status && read_length &&
       (((const uint8_t*)address)[0] == PC_STATUS_REGISTER) && !(status[0] & PC_STATUS_OIP))
   {
      status[0] |= inject_status;
      inject_status = 0;
   }
}

void __wrap_nandlog_port_transfer_write(uint8_t command, const void *address, uint32_t address_length, const void *write_buffer, uint32_t write_length)
{
   ++transfers_seen;
   if ((command != PC_CMD_PROGRAM_EXECUTE) && (command != PC_CMD_BLOCK_ERASE))
   {
      if (command == PC_CMD_PROGRAM_DATA_LOAD)
         loaded = classify_load(write_buffer, write_length);
      else if (command == PC_CMD_PAGE_DATA_READ)
         loaded = TARGET_COPY;
      __real_nandlog_port_transfer_write(command, address, address_length, write_buffer, write_length);
      return;
   }

   ++ops;
   ++ops_since_injection;
   const uint32_t page = operation_page(address, address_length, write_buffer, write_length);
   const uint32_t block = (page == PC_UNKNOWN) ? PC_UNKNOWN : (page / nandlog_chip_geometry()->pages_per_block);
   const op_target_t target = (command == PC_CMD_PROGRAM_EXECUTE) ? loaded : erase_target(block);
   if (command == PC_CMD_PROGRAM_EXECUTE)
      loaded = TARGET_OTHER;
#if PC_SELF_CUT
   if (ops == cut_at)
      cut_here(command, address, address_length, write_buffer, write_length, block, target);
#endif
   __real_nandlog_port_transfer_write(command, address, address_length, write_buffer, write_length);
#if PC_INJECT_FAILURES
   consider_injection(command, page, block, target);
#endif
}

static void unattended_boot(void)
{
   // Before nandlog_init(), so its operations can be cut: the random state, and this boot's plan
#if PC_SELF_CUT
   if (!hw_cycles_per_us())
   {
      print("POWERCUT_FATAL the cycle counter is not counting, so no cut could be timed -- attach the debugger\n");
      while (true)
         am_hal_delay_us(1000000);
   }
#endif
   const uint32_t stimer = STIMER->STCFG;
   if (!(stimer & STIMER_STCFG_CLKSEL_Msk) || (stimer & (STIMER_STCFG_FREEZE_Msk | STIMER_STCFG_CLEAR_Msk)))
      am_hal_stimer_config(AM_HAL_STIMER_HFRC_375KHZ);
   if ((retained.magic == PC_RETAINED_MAGIC) && (retained.check == crc32(&retained, sizeof(retained) - sizeof(retained.check))))
      ++retained.carried;
   else
   {
      memset(&retained, 0, sizeof(retained));
      retained.init_ops = PC_INIT_OPS_GUESS;
      retained.retired = PC_UNKNOWN;
   }
   random_state = retained.state ^ mix32(am_hal_stimer_counter_get()) ^ mix32(hw_cycle_count() ^ 0x632BE5ABu);
   retained.state = random_next();
   ops_since_injection = retained.ops_since_injection;
   retired_known = retained.retired;
   replace_epoch = (retained.replace_epoch != 0);
#if PC_SELF_CUT
   const uint32_t share = random_next() % PC_PLAN_SHARES;
   plan = (share < PC_PLAN_LATE_SHARES) ? PLAN_LATE : ((share < (PC_PLAN_LATE_SHARES + PC_PLAN_INIT_SHARES)) ? PLAN_INIT : PLAN_EARLY);
   if (plan == PLAN_INIT)
      cut_at = 1 + (random_next() % (retained.init_ops ? retained.init_ops : 1));
#endif
   retained_seal();
}

static void unattended_note_retired(void)
{
   // Read between library calls, never from inside one: the wrappers run with the library's lock held. Whatever an
   // injection retired has been counted by the time the call that met it returns
   retired_known = nandlog_bad_block_count();
   injections_unseen = 0;
   if (retained.retired != retired_known)
   {
      retained.retired = retired_known;
      retained_seal();
   }
}

static void unattended_init_done(void)
{
   // A link without the --wrap flags never reaches the wrappers, and would run as a plain soak nobody cuts
   if (!transfers_seen)
   {
      print("POWERCUT_FATAL the port is not wrapped at link time (the target's LFLAGS --wrap line); nothing would be cut\n");
      while (true)
         am_hal_delay_us(1000000);
   }
   unattended_note_retired();
}

static void unattended_logging_begins(uint32_t pages_at_boot)
{
   // The boot's own operations are over: their count is the next boot's guess, and the logging plans are drawn now
   logging_phase = true;
   retained.init_ops = ops ? ops : 1;
   retained.ops_since_injection = ops_since_injection;
   retained.replace_epoch = 0;
   retained_seal();
#if PC_SELF_CUT
   // A program per page and an erase per block, up to where the run stops at PC_HARD_CAP_PAGES
   const uint32_t left = (pages_at_boot < PC_HARD_CAP_PAGES) ? (PC_HARD_CAP_PAGES - pages_at_boot) : 1;
   const uint32_t available = left + (left / nandlog_chip_geometry()->pages_per_block), limit = ((available * 7) / 8) + 1;
   if (plan == PLAN_INIT)
      plan = PLAN_EARLY;
   if (plan == PLAN_EARLY)
   {
      const uint32_t octave = random_next() % PC_EARLY_CUT_OCTAVES;
      cut_at = ops + 1 + ((((1u << octave) + (random_next() % (1u << octave))) - 1) % limit);
   }
   else if (plan == PLAN_LATE)
      cut_at = ops + (available / 2) + (random_next() % (limit - (available / 2)));
#else
   (void)pages_at_boot;
#endif
}

static void unattended_error(void)
{
   // A boot that stopped on an error would meet it again at every boot after it, with nobody there to notice: the next
   // boot begins a new epoch instead, as a person restarting the soak would. The POWERCUT_ERR line stays in the capture
   retained.replace_epoch = 1;
   retained_seal();
}

static bool unattended_replace_epoch(void)
{
   return replace_epoch;
}

static void unattended_idle(void)
{
#if PC_SELF_CUT
   // Nobody will cut the power, so a boot with nothing left to do ends itself after a while idle, its RAM lost but no
   // operation stopped -- unless its planned cut lands in one of the idle flushes first
   for (uint32_t nap = 0; nap < PC_IDLE_NAPS; ++nap)
   {
      (void)nandlog_flush(false);
      am_hal_delay_us(100000);
   }
   reset_mcu();
#endif
}

#else  // The plain soak: nothing interrupts it but the power switch

static inline void unattended_boot(void) {}
static inline void unattended_init_done(void) {}
static inline void unattended_logging_begins(uint32_t pages_at_boot) { (void)pages_at_boot; }
static inline void unattended_note_retired(void) {}
static inline void unattended_error(void) {}
static inline bool unattended_replace_epoch(void) { return false; }
static inline void unattended_idle(void) {}

#endif  // #if PC_UNATTENDED


// Verification of the Current Epoch -----------------------------------------------------------------------------------

typedef struct
{
   uint32_t run, count;
   uint32_t prefix;              // records 0 .. prefix-1 all present: what an acknowledgement is checked against
   bool unbroken;                // no hole found in this run yet
} run_count_t;

typedef struct
{
   bool has_epoch, foreign;
   uint32_t epoch, base_run, pages, records, gaps, tombstones, damaged, out_of_order, dup, holes, max_run;
   uint32_t run_entries;
   bool runs_overflow;
   run_count_t runs[PC_MAX_RUNS];
} verdict_t;

static verdict_t verdict;

static void count_record(uint32_t run, uint32_t index, bool *have_last, uint32_t *last_run, uint32_t *last_index)
{
   // Records arrive in the order the log holds them. Within a run the indices must climb by one; a run must start
   // at index 0 and come after every earlier run. Anything else is a hole (missing records), a duplicate, or out of
   // order, and the last two are violations
   if (*have_last && (run < *last_run))
   {
      ++verdict.out_of_order;
      return;
   }
   if (*have_last && (run == *last_run) && (index <= *last_index))
   {
      ++verdict.dup;
      return;
   }
   const bool new_run = !*have_last || (run > *last_run);
   const uint32_t missing = new_run ? index : (index - *last_index - 1);
   verdict.holes += missing;
   if (new_run)
   {
      if (verdict.run_entries < PC_MAX_RUNS)
      {
         run_count_t *entry = &verdict.runs[verdict.run_entries++];
         entry->run = run;
         entry->count = entry->prefix = 0;
         entry->unbroken = true;
      }
      else
         verdict.runs_overflow = true;
   }
   if (!verdict.runs_overflow && verdict.run_entries)
   {
      run_count_t *entry = &verdict.runs[verdict.run_entries - 1];
      ++entry->count;
      if (missing)
         entry->unbroken = false;
      if (entry->unbroken)
         ++entry->prefix;
   }
   ++verdict.records;
   *have_last = true;
   *last_run = run;
   *last_index = index;
   if (run > verdict.max_run)
      verdict.max_run = run;
}

static bool page_is_tombstone(uint32_t seq, uint32_t positions)
{
   // A tombstone and an unreadable page both come back as zero bytes; only the tombstone has a header the library
   // trusts, and nandlog_read_recent_page() hands that header back
   if (seq >= positions)
      return false;
   nandlog_page_header_t header;
   bool end_of_epoch = false;
   memset(&header, 0, sizeof(header));
   nandlog_read_recent_page(positions - 1 - seq, NULL, &header, &end_of_epoch);
   return (header.magic == NANDLOG_PAGE_MAGIC_THIS_BUILD) && (header.seq == seq) && !header.payload_length;
}

static void verify_current_epoch(void)
{
   memset(&verdict, 0, sizeof(verdict));
   nandlog_begin_session();
   nandlog_epoch_info_t info;
   verdict.has_epoch = nandlog_epoch_info(0, &info);
   verdict.epoch = verdict.has_epoch ? info.epoch : 0;
   pc_details_t details;
   nandlog_retrieve_epoch_details(&details, sizeof(details));
   verdict.foreign = !verdict.has_epoch || (details.magic != PC_DETAILS_MAGIC);
   verdict.base_run = verdict.foreign ? 0 : details.base_run;

   bool have_last = false;
   uint32_t last_run = 0, last_index = 0;
   nandlog_begin_reading(0, 0);
   nandlog_read_span(&verdict.pages, NULL);
   for (uint32_t i = 0; i < verdict.pages; ++i)
   {
      nandlog_page_header_t header;
      const uint32_t length = nandlog_retrieve_next_page(page_buffer, &header);
      if (!length)
      {
         if (page_is_tombstone(header.seq, verdict.pages))
            ++verdict.tombstones;
         else
            ++verdict.gaps;
         continue;
      }
      if (verdict.foreign)
         continue;

      // Walk the page record by record. A page whose records do not exactly fill its payload, or whose count
      // disagrees with its header, is damaged as a whole, beyond whatever its records say
      uint32_t offset = 0, walked = 0, record_bytes = 0;
      const uint8_t *framed = NULL;
      while (nandlog_framed_next_record(page_buffer, length, &offset, &framed, &record_bytes))
      {
         uint32_t run = 0, index = 0;
         ++walked;
         if (record_matches(framed, record_bytes, &run, &index))
            count_record(run, index, &have_last, &last_run, &last_index);
         else
            ++verdict.damaged;
      }
      if ((offset != length) || (walked != header.record_count))
         ++verdict.damaged;
   }
   nandlog_end_reading();
   nandlog_end_session();
}


// Logging Until the Power Goes ----------------------------------------------------------------------------------------

static uint32_t lcg_state;

static uint32_t lcg_next(void)
{
   lcg_state = (1664525u * lcg_state) + 1013904223u;
   return lcg_state >> 8;
}

static void acknowledge(uint32_t epoch, uint32_t run, uint32_t index)
{
   line_begin("POWERCUT_ACK");
   line_field("epoch", epoch);
   line_field("run", run);
   line_field("upto", index);
   line_emit();
}

static void idle_until_power_is_cut(const char *why)
{
   print("POWERCUT_IDLE %s\n", why);
   unattended_idle();
   while (true)
   {
      (void)nandlog_flush(false);
      am_hal_delay_us(100000);
   }
}

static void report_error(const char *what, nandlog_status_t status, uint32_t index)
{
   print("POWERCUT_ERR %s status=%u index=%u\n", what, (uint32_t)status, index);
   unattended_error();
   idle_until_power_is_cut("after_error");
}

static void log_until_power_is_cut(uint32_t epoch, uint32_t run, uint32_t pages_at_boot)
{
   lcg_state = mix32(run ^ 0xC0FFEE11u);
   unattended_logging_begins(pages_at_boot);

   // The boot marker goes first, alone, and is made durable at once: it is what carries this run's number forward
   uint32_t length = build_record(record, run, 0);
   nandlog_status_t status = nandlog_store_record(PC_TYPE_BOOT, 0, record, length);
   if (status != NANDLOG_OK)
      report_error("store_record", status, 0);
   status = nandlog_flush(true);
   if (status != NANDLOG_OK)
      report_error("flush", status, 0);
   acknowledge(epoch, run, 0);

   uint32_t since_flush = 0, flush_at = 1 + (lcg_next() % 300);
   for (uint32_t index = 1; ; ++index)
   {
      length = build_record(record, run, index);
      status = nandlog_store_record(PC_TYPE_DATA, index, record, length);
      if (status != NANDLOG_OK)
         report_error("store_record", status, index);

      if (++since_flush >= flush_at)
      {
         // A partial page, committed on purpose: everything up to this index is now durable
         status = nandlog_flush(true);
         if (status != NANDLOG_OK)
            report_error("flush", status, index);
         acknowledge(epoch, run, index);
         since_flush = 0;
         flush_at = 1 + (lcg_next() % 300);

         nandlog_stats_t stats;
         nandlog_get_stats(&stats);
         if ((pages_at_boot + stats.pages_committed) >= PC_HARD_CAP_PAGES)
            idle_until_power_is_cut("epoch_page_cap");
      }
      else if (!(lcg_next() & 31))
         (void)nandlog_flush(false);         // the idle hook, where an erase-ahead lands when one is due

      unattended_note_retired();
      am_hal_delay_us(lcg_next() % 400);
   }
}


// Main ----------------------------------------------------------------------------------------------------------------

int main(void)
{
   setup_hardware();
   hw_timing_init();
   print("POWERCUT_START nandlog=%u.%u source=%s port_crc=%u erase_ahead=%u head_pass=%u" PC_START_MODE_FORMAT "\n",
         NANDLOG_VERSION_MAJOR, NANDLOG_VERSION_MINOR, NANDLOG_SOURCE_ID, NANDLOG_PORT_CRC32, NANDLOG_ERASE_AHEAD_BLOCKS,
         NANDLOG_PASS_OVER_HEAD_AT_BOOT PC_START_MODE_VALUES);
   unattended_boot();

   const uint32_t init_start = hw_cycle_count();
   if (!nandlog_init())
   {
      print("POWERCUT_FATAL nandlog_init() failed -- no part, or the port would not open\n");
      while (true)
         am_hal_delay_us(1000000);
   }
   const uint32_t init_us = hw_elapsed_us(init_start, hw_cycle_count());
   unattended_init_done();
   system_enable_interrupts(true);
   nandlog_stats_t boot_stats;
   nandlog_get_stats(&boot_stats);

   const uint32_t verify_start = hw_cycle_count();
   verify_current_epoch();
   const uint32_t verify_us = hw_elapsed_us(verify_start, hw_cycle_count());

   // This boot's run: one past the highest the epoch holds, and never below the base its details name
   uint32_t run = verdict.foreign ? 1 : (verdict.max_run + 1);
   if (!verdict.foreign && (verdict.base_run > run))
      run = verdict.base_run;
   const bool new_epoch = verdict.foreign || (verdict.pages >= PC_ROLLOVER_PAGES) ||
                          (verdict.run_entries >= PC_ROLLOVER_RUNS) || verdict.runs_overflow || unattended_replace_epoch();

   line_begin("POWERCUT");
   line_field("boot", run);
   line_field("epoch", verdict.epoch);
   line_field("foreign", verdict.foreign ? 1 : 0);
   line_field("runs", verdict.run_entries);
   line_field("records", verdict.records);
   line_field("pages", verdict.pages);
   line_field("gaps", verdict.gaps);
   line_field("tombstones", verdict.tombstones);
   line_field("damaged", verdict.damaged);
   line_field("out_of_order", verdict.out_of_order);
   line_field("dup", verdict.dup);
   line_field("holes", verdict.holes);
   line_field("skipped", boot_stats.positions_skipped);
   line_field("retired", nandlog_bad_block_count());
   line_field("init_us", init_us);
   line_field("verify_us", verify_us);
   line_str(" per_run=");
   if (!verdict.run_entries)
      line_str("-");
   for (uint32_t i = 0; i < verdict.run_entries; ++i)
   {
      // run:count, or run:count:prefix when a record is missing part-way through the run
      if (i)
         line_str(",");
      line_u32(verdict.runs[i].run);
      line_str(":");
      line_u32(verdict.runs[i].count);
      if (verdict.runs[i].prefix != verdict.runs[i].count)
      {
         line_str(":");
         line_u32(verdict.runs[i].prefix);
      }
   }
   if (verdict.runs_overflow)
      line_str(",overflow");
   line_emit();

   uint32_t epoch = verdict.epoch, pages_at_boot = verdict.pages;
   if (new_epoch)
   {
      pc_details_t details = { PC_DETAILS_MAGIC, run, 1 };
      nandlog_begin_session();
      const bool begun = nandlog_begin_epoch(&details, sizeof(details));
      nandlog_epoch_info_t info;
      if (begun && nandlog_epoch_info(0, &info))
         epoch = info.epoch;
      nandlog_end_session();
      if (!begun)
      {
         print("POWERCUT_ERR begin_epoch status=0 index=0\n");
         unattended_error();
         idle_until_power_is_cut("after_error");
      }
      pages_at_boot = 0;
      line_begin("POWERCUT_EPOCH");
      line_field("epoch", epoch);
      line_field("base_run", run);
      line_emit();
   }

   log_until_power_is_cut(epoch, run, pages_at_boot);
   return 0;
}
