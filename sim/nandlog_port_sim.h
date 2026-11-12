#ifndef __NANDLOG_PORT_SIM_HEADER_H__
#define __NANDLOG_PORT_SIM_HEADER_H__

// Control surface for the RAM-backed port. Only host tests include this; the log and the chip driver see
// nothing but nandlog_port.h, and neither is aware it is not talking to real hardware.

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include "nandlog_conf.h"

#define NANDLOG_SIM_MAX_PAGE_WITH_SPARE             (8192 + 512)
#define NANDLOG_SIM_MAX_FAULT_BLOCKS                32
#define NANDLOG_SIM_MAX_FAULT_PAGES                 64

// ECC verdicts a page can be made to report, in the two status bits both supplied parts use. What 11b means
// is the part's business: the Alliance part calls it "corrected at the ECC limit", the Winbond part an
// uncorrectable error in a continuous read
#define NANDLOG_SIM_ECC_CLEAN                       0x00
#define NANDLOG_SIM_ECC_CORRECTED                   0x10
#define NANDLOG_SIM_ECC_UNCORRECTABLE               0x20
#define NANDLOG_SIM_ECC_AT_LIMIT                    0x30


// What the emulated part should refuse to do. Blocks are block numbers, not page addresses
typedef struct
{
   uint8_t device_id[3];                                       // what READ_DEVICE_ID reports
   uint32_t pages_per_block;                                   // set from the chip's geometry by create()
   uint32_t unwritable_blocks[NANDLOG_SIM_MAX_FAULT_BLOCKS];   // program always reports failure
   uint32_t num_unwritable_blocks;
   uint32_t unerasable_blocks[NANDLOG_SIM_MAX_FAULT_BLOCKS];   // erase always reports failure
   uint32_t num_unerasable_blocks;
   bool stay_busy;                                             // never clears BUSY, to exercise the timeout
   uint32_t unwritable_pages[NANDLOG_SIM_MAX_FAULT_PAGES];     // program of just these pages reports failure
   uint32_t num_unwritable_pages;
   bool torn_reads_uncorrectable;                              // with ECC on, a page a cut left torn reads uncorrectable
   uint32_t ecc_hides_bits;                                    // with ECC on, a page holding at most this many programmed
                                                               // bits reads back erased, as an erased-page check would
                                                               // make it; zero turns that off
   bool has_stuck_cells;                                       // a program of 'stuck_page' reports success but leaves
   uint32_t stuck_page, stuck_offset;                          // the bits of 'stuck_mask' at byte 'stuck_offset' erased
   uint8_t stuck_mask;
   uint32_t worn_pages[NANDLOG_SIM_MAX_FAULT_PAGES];           // with ECC on, these pages report NANDLOG_SIM_ECC_AT_LIMIT
   uint32_t num_worn_pages;                                    // whatever they hold, through every erase: worn cells
} nandlog_sim_faults_t;

typedef struct
{
   uint32_t spi_reads, spi_writes;                             // transfers issued
   uint32_t spi_read_bytes, spi_write_bytes;                   // payload bytes those transfers carried
   uint32_t page_reads, page_writes, block_erases;
   uint32_t raw_page_reads;                                    // page reads latched with ECC turned off
   uint32_t reads_header, reads_partial, reads_page, reads_spare;  // data clocked out of the cache register, by length:
                                                               // at most 64 bytes (a header, a mark), more but less than
                                                               // a page, a whole page, or a page and its spare area
   uint32_t repeated_programs;                                 // PROGRAM EXECUTEs to a page already programmed since its erase
   uint32_t blank_reprograms_after_boot;                       // ...not counted above: to a page whose earlier program was
                                                               // in an earlier boot and left every bit erased -- cut before
                                                               // it changed one, or refused -- which no read can tell from
                                                               // an erased page, and so nothing after a reboot can know of
   uint32_t drifted_reprograms;                                // ...nor above: to a page nandlog_sim_drift() moved, which
                                                               // the log had long since seen in its weak state
   uint32_t mark_reprograms;                                   // ...nor above: a bad-block MARK into a programmed page -- ECC
                                                               // off, the main area FFh, the spare FFh but 00h at its first
                                                               // byte -- which changes no bit a page's data lives in
   uint32_t random_loads_refused;                              // 84h outside an internal data move: not straight after a
                                                               // PAGE DATA READ and its status polls, or another 84h
} nandlog_sim_counters_t;

// How a cut leaves the operation it lands in
typedef enum
{
   NANDLOG_SIM_CUT_PREFIX,                                     // the first half of the page or block takes effect
   NANDLOG_SIM_CUT_BITWISE,                                    // each bit the operation would change does, with probability q
   NANDLOG_SIM_CUT_UNSTABLE,                                   // bitwise, and the bits left undecided read randomly until erased
   NANDLOG_SIM_CUT_BOUNDARY                                    // the operation never starts: power went between operations
} nandlog_sim_cut_model_t;

// Where the last cut landed
typedef struct
{
   uint32_t operation;                                         // 1-based count of programs and erases since the arm
   uint8_t command;                                            // 0x10 program, 0xD8 erase, 0xA1 a remap-table write
   uint32_t page;                                              // target page, or the first page of the erased block
   bool otp;                                                   // the target was the one-time-programmable window
} nandlog_sim_cut_t;


// Build a part matching the geometry the compiled chip driver reports, so the two can never disagree. Only
// the identity has to be supplied, because it is the one thing geometry does not carry
void nandlog_sim_create(const uint8_t *device_id, uint32_t device_id_length);
void nandlog_sim_destroy(void);

// Fault injection. Passing a zeroed structure clears everything
void nandlog_sim_set_faults(const nandlog_sim_faults_t *faults);

// Make every ECC-enabled read of a page report this verdict, until its block is next erased. Reads with ECC
// turned off report nothing, exactly as the parts do. NANDLOG_SIM_ECC_CLEAN removes an injection
void nandlog_sim_set_ecc_status(uint32_t page, uint8_t status);

// Make a block DEAD, as the top blocks of one board were found: every page reads 00h in every byte, main and spare,
// with a clean ECC verdict, and the part refuses every program and erase of it. It stays so for as long as the part
// exists, through snapshots, restores and fault changes. A test kills blocks once, after nandlog_sim_create()
void nandlog_sim_kill_block(uint32_t block);
bool nandlog_sim_block_dead(uint32_t block);

// Cut the n-th subsequent program or erase short, leaving half of it applied, as losing power would. Zero
// disables. nandlog_sim_power_failed() reports whether it has fired yet. Execution carries on afterwards
void nandlog_sim_power_fail_after(uint32_t operations);
bool nandlog_sim_power_failed(void);

// The general form: cut the n-th subsequent program or erase under 'model', where q is the per-bit
// probability of the bit-level models. With 'stop' set, the port then loses power for real: the part's
// volatile registers reset and execution longjmps to 'stop', so nothing after the cut runs. The harness
// brings the log back with nandlog_deinit() and nandlog_init(), as a reboot would
void nandlog_sim_power_cut(uint32_t operations, nandlog_sim_cut_model_t model, double q, jmp_buf *stop);
void nandlog_sim_power_cut_disarm(void);
bool nandlog_sim_last_cut(nandlog_sim_cut_t *cut);

// Programs, erases and remap-table writes issued since the part was created or this was last reset, which is
// what a harness enumerates cut points over
uint32_t nandlog_sim_operations(void);
void nandlog_sim_reset_operations(void);

// Seed the generator behind the bit-level models and unstable reads
void nandlog_sim_seed(uint64_t seed);

// Whether a page holds bits a cut left undecided under NANDLOG_SIM_CUT_UNSTABLE, which read differently on every
// latch until its block is erased
bool nandlog_sim_page_unstable(uint32_t page);

// The passing of time. A page is WEAK if a cut stopped its program short of a bit it was to change, or if it was
// programmed in a block whose last erase a cut interrupted. Each weak page, with probability p, drifts: its cells
// lose their charge, all of it or each programmed bit with even odds. Returns the pages that drifted
typedef enum
{
   NANDLOG_SIM_DRIFT_BLANK,                                    // the page reads erased in every bit
   NANDLOG_SIM_DRIFT_PARTIAL                                   // each programmed bit reads erased with probability 1/2
} nandlog_sim_drift_t;

uint32_t nandlog_sim_drift(nandlog_sim_drift_t how, double p);
uint32_t nandlog_sim_weak_pages(void);

// Rot bits in place. Only clearing is possible without an erase, so 'mask' names bits to drive to zero
void nandlog_sim_corrupt(uint32_t page, uint32_t offset, uint8_t mask);
uint8_t *nandlog_sim_raw_page(uint32_t page);

// The pages most recently programmed a second time since their erase, newest last, so a harness can tell a
// deliberate one -- the marker a driver writes into a block it retires -- from one that is a defect
uint32_t nandlog_sim_repeated_program_pages(uint32_t *pages, uint32_t capacity);

// The same for programs counted in 'blank_reprograms_after_boot', so a harness can tell the marker a driver writes
// into a block it retires after a reboot from a page programmed twice in a block still in use. Both logs, and both
// counts, are cleared together
uint32_t nandlog_sim_blank_reprogram_pages(uint32_t *pages, uint32_t capacity);
void nandlog_sim_clear_repeated_programs(void);

// The pages that took a bad-block mark after an earlier program, newest last, kept apart from the logs above: the
// driver marks blocks it retires, blocks whose table versions it makes unloadable, and -- in degraded mode, where the
// mark is what retires a block -- a block whose mark the part then refuses, which stays unretired. Cleared with them
uint32_t nandlog_sim_mark_pages(uint32_t *pages, uint32_t capacity);

// Remember the part as it stands, and put it back later. A restore copies only blocks touched since, so a
// harness can afford one per trial even on a nearly full part
void nandlog_sim_snapshot(void);
void nandlog_sim_restore(void);

// nandlog_port_fatal() aborts the process unless a fatal is expected, in which case it longjmps to this
// buffer. A test arms it with  if (setjmp(*nandlog_sim_fatal_jump()) == 0) { ...expect a fault... }
void nandlog_sim_expect_fatal(bool expected);
bool nandlog_sim_fatal_seen(const char **reason);
jmp_buf *nandlog_sim_fatal_jump(void);

// Write a range of the emulated part out byte for byte, so the reference parser can be run against something
// a device would actually have produced. A whole part is gigabytes, so tests dump the region they used
bool nandlog_sim_dump(const char *path, uint32_t first_page, uint32_t num_pages);

// The reverse: put a dump back on the part from 'first_page' on, so a log written by one build -- an earlier
// release, say -- can be booted by another
bool nandlog_sim_load(const char *path, uint32_t first_page);

nandlog_sim_counters_t nandlog_sim_counters(void);

// How many times a page has been latched for reading since the counters were last reset -- read disturb's measure
uint32_t nandlog_sim_page_latches(uint32_t page);

// How many erases a block has been given since the counters were last reset, and how many programs its pages, those
// the part refused included -- so a test can tell the metadata's writes from the log region's
uint32_t nandlog_sim_block_erases(uint32_t block);
uint32_t nandlog_sim_block_programs(uint32_t block);

// Zero the counters without disturbing the part, so a test can measure one span of work rather than a run
void nandlog_sim_reset_counters(void);

// Whether nandlog_port_unwritable() has been called since the part was created or the counters were reset
bool nandlog_sim_unwritable_reported(void);

// How many times the library has taken its lock since the part was created or the counters were reset
uint32_t nandlog_sim_lock_count(void);

void nandlog_sim_set_verbose(bool on);

#if NANDLOG_PORT_CRC32
// The port's CRC-32 (nandlog_port_crc32()): calls, and the bytes they covered, since the program started
uint32_t nandlog_sim_crc32_calls(void);
uint32_t nandlog_sim_crc32_bytes(void);
#endif

#endif  // #ifndef __NANDLOG_PORT_SIM_HEADER_H__
