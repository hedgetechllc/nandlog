// Crash-point campaign and recovery sweep, run against the RAM-backed port. `make campaign` builds and runs it.
//
// THE CAMPAIGN. A trace is a fixed sequence of library calls run from a fixed starting state of the part. It is
// run once uncut, to count its program and erase operations; then, for every operation n of it and under every
// cut model, the part is restored, the trace is run with power cut at operation n (nothing after the cut
// executes), and the device reboots and is audited. A second pass cuts power a second time, at every operation of
// that recovering boot, before the clean reboot; a third lets that boot finish and cuts the logging after it, at
// its first few operations -- the first commits past whatever the boot passed over. After the first audit the
// device carries on -- begins an epoch if it has none, logs forty more pages, flushes -- reboots and is audited
// again, which is what exposes a write head recovery put in the wrong place. After a cut under the unstable model,
// where the bits a cut left undecided read differently on every boot, it reboots and is audited three times more,
// which is what exposes a boot that went by a torn page one way and a later boot that reads it the other. And then
// time passes: every WEAK page -- one a cut stopped short of its target, or one programmed into a block whose last
// erase a cut interrupted -- drifts, back to erased in every bit or with half its programmed bits lost, and the
// device reboots, is audited, logs ten pages more, reboots and is audited again.
//
// The audit holds the library to its durability contract:
//
//   - every record whose commit RETURNED before the cut is delivered, intact, from the epoch it was logged in --
//     unless a later epoch's write head has since swept over the block that held it, which is how an earlier
//     epoch is documented to end;
//   - no record is ever delivered damaged, or twice;
//   - in the current epoch, every page and every gap carries the sequence number of its position;
//   - a read's declared byte total is exactly what it delivers;
//   - nothing recovery or later logging does programs a page a second time, outside a block being retired, nor
//     programs a page that drifted;
//   - no block is retired that the trace did not make fail, and no factory-bad block is forgotten;
//   - what a read served under an epoch and sequence number is what that sequence number serves at every later
//     audit of the trial, or a gap: never other content, and never records stored after it was served as a gap.
//
// A record is durable once the call that committed its page has returned: the store that pushed the page out, a
// flush, or a begin_epoch that committed what was buffered. The harness learns which calls committed from the
// library's own page counter, read after every call.
//
// THE SWEEP. Separately, recovery is enumerated without cuts: an epoch of each of a range of lengths, with one
// fault at each of a range of positions -- a rotted header, an uncorrectable page, a page corrected at the ECC
// limit, a retired block -- including the epoch's first block and positions across the wrap of the region; the
// device reboots, is audited, logs on, reboots, and is audited again. And the metadata is swept the same way: a
// part with forty epochs and forty-odd bad-block table versions behind it, and every page of the ring and of the
// table, one at a time, rotted, made uncorrectable, or made to read whole but only at the limit of the part's ECC
// before a reboot. One such page of either must change nothing: not the epoch, not its write head, not the epochs
// listed, not the blocks retired. A boot may write the ring's newest slot or the table's newest version again, which
// is counted; the boot after must write neither; and a page read at the ECC limit must have its record written again
// exactly when it is a copy of the newest slot or version.
//
// The part is built smaller than the real one (NANDLOG_CHIP_BLOCK_COUNT in the Makefile), so that a trace that
// fills the log region runs in milliseconds. Everything else is the shipping driver and core.
//
// A build with SIM_DEAD_RESERVE_BLOCKS kills that many blocks from the start of the driver's reserve -- its table
// blocks -- and SIM_DEAD_LOG_BLOCKS the region's top blocks below it, on every part it creates, as one board was found:
// the table moves to a pair of blocks below them on the first boot, and every trace runs with it there. Those blocks,
// and the pair, are no good blocks retired; the traces that place the head against the end of the region place it
// against the last good block; and the metadata sweep rots the pages of the pair instead of the reserve's.
//
// Usage: nandlog_sim_campaign list                          every job `make campaign` runs
//        nandlog_sim_campaign campaign <trace> [model]      one trace, every cut, every model or the one given (0-3)
//        nandlog_sim_campaign sweep <part>                  one part of the recovery or metadata sweep
//        nandlog_sim_campaign one <trace> <model> <n> [k] [c] one trial, with the library's diagnostics: the second
//                                                            cut at the recovering boot's k-th operation, or with k
//                                                            zero, at the c-th (1-4) of the cuts in the logging after

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nandlog.h"
#include "nandlog_chip.h"
#include "nandlog_port_sim.h"

static const uint8_t DEVICE_ID[3] = { 0x8D, 0x00, 0x00 };

#ifndef SIM_DEAD_RESERVE_BLOCKS
#define SIM_DEAD_RESERVE_BLOCKS                     0
#endif
#ifndef SIM_DEAD_LOG_BLOCKS
#define SIM_DEAD_LOG_BLOCKS                         0
#endif
#ifndef NANDLOG_CHIP_TABLE_SEARCH_BLOCKS
#define TABLE_SEARCH_BLOCKS                         (nandlog_chip_geometry()->reserved_blocks + 48)
#else
#define TABLE_SEARCH_BLOCKS                         NANDLOG_CHIP_TABLE_SEARCH_BLOCKS
#endif

#define METADATA_RING_BLOCKS                        8
#define MAX_IDS                                     65536
#define MAX_TRACE_EPOCHS                            128
#define CONTINUE_PAGES                              40

static uint8_t record[NANDLOG_MAX_PAGE_SIZE_BYTES], readback[NANDLOG_MAX_PAGE_SIZE_BYTES];
static jmp_buf cut_jump;

// What each sequence number of each epoch was first served as in a trial: a checksum of its content, or a gap and
// how many records had been stored by then. Entries carry the trial they belong to, so nothing is cleared between
#define IDENTITY_EPOCHS                             64
#define IDENTITY_SEQS                               4096
typedef struct { uint32_t trial, epoch, crc, bound; bool data; } identity_t;
static identity_t identity[IDENTITY_EPOCHS][IDENTITY_SEQS];
static uint32_t identity_trial;


// Geometry ------------------------------------------------------------------------------------------------------------

static uint32_t ppb(void)              { return nandlog_chip_geometry()->pages_per_block; }
static uint32_t log_first_block(void)  { return METADATA_RING_BLOCKS; }
static uint32_t log_end_block(void)    { return nandlog_chip_geometry()->block_count - nandlog_chip_geometry()->reserved_blocks; }
static uint32_t log_blocks(void)       { return log_end_block() - log_first_block(); }
static uint32_t page_bytes(void)       { return nandlog_chip_geometry()->page_size_bytes; }
static uint32_t prefix_bytes(void)     { return NANDLOG_RECORD_FRAMING ? NANDLOG_FRAMING_LENGTH_BYTES : 0; }
static uint32_t page_record(void)      { return nandlog_data_bytes_per_page() - prefix_bytes() - 5; }

static uint32_t log_block_after(uint32_t block)
{
   return ((block + 1) < log_end_block()) ? (block + 1) : log_first_block();
}

static bool block_bad(uint32_t block)
{
   return nandlog_chip_is_bad_block(block * ppb());
}

static uint32_t table_search_first_block(void)
{
   const uint32_t blocks = nandlog_chip_geometry()->block_count;
   return (TABLE_SEARCH_BLOCKS < blocks) ? (blocks - TABLE_SEARCH_BLOCKS) : 0;
}

static void create_part(void)
{
   // A factory-fresh part, with the build's dead blocks
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   for (uint32_t b = log_end_block() - SIM_DEAD_LOG_BLOCKS; b < (log_end_block() + SIM_DEAD_RESERVE_BLOCKS); ++b)
      nandlog_sim_kill_block(b);
}

static bool holds_table(uint32_t block)
{
   // A block of a pair below the reserve the table was, or is, kept in: a copy of its first pair carries the magic
   const uint32_t pair = block & ~1u;
   bool magic = false;
   for (uint32_t i = 0; !magic && (i < 4); ++i)
      magic = !memcmp(nandlog_sim_raw_page(((pair + (i / 2)) * ppb()) + (i % 2)), "BBT3", 4);
   return SIM_DEAD_RESERVE_BLOCKS && (block >= table_search_first_block()) && (block < log_end_block()) && magic;
}

static uint32_t good_log_blocks(void)
{
   uint32_t good = 0;
   for (uint32_t b = log_first_block(); b < log_end_block(); ++b)
      good += block_bad(b) ? 0 : 1;
   return good;
}

static uint32_t last_good_log_block(void)
{
   uint32_t b = log_end_block() - 1;
   while ((b > log_first_block()) && block_bad(b))
      --b;
   return b;
}


// The Record Model ----------------------------------------------------------------------------------------------------

static uint32_t next_id, pending[MAX_IDS], pending_count, trace_epoch, epoch_numbers[MAX_TRACE_EPOCHS];
static uint8_t durable[MAX_IDS], seen[MAX_IDS];
static uint16_t record_epoch[MAX_IDS];
static uint32_t id_page[MAX_IDS];         // where each record's page sits on the part, from the uncut run

// The model as it stood when a trace's base state was snapshotted, restored with the part before every run
static uint32_t base_next_id, base_trace_epoch, base_epoch_numbers[MAX_TRACE_EPOCHS];
static uint8_t base_durable[MAX_IDS];
static uint16_t base_record_epoch[MAX_IDS];

static void fill_record(uint8_t *buffer, uint32_t length, uint32_t id)
{
   // Content that only one id produces, a word at a time
   uint64_t x = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)id << 20) ^ length;
   for (uint32_t i = 0; i < length; i += 8)
   {
      x ^= x >> 12;
      x ^= x << 25;
      x ^= x >> 27;
      const uint64_t word = x * 0x2545F4914F6CDD1DULL;
      memcpy(buffer + i, &word, ((length - i) < 8) ? (length - i) : 8);
   }
   if (length >= 4)
      memcpy(buffer, &id, sizeof(id));
}

static uint32_t pages_committed(void)
{
   nandlog_stats_t stats;
   nandlog_get_stats(&stats);
   return stats.pages_committed;
}

static void model_reset(void)
{
   next_id = pending_count = trace_epoch = 0;
   memset(durable, 0, sizeof(durable));
   memset(record_epoch, 0, sizeof(record_epoch));
   memset(epoch_numbers, 0, sizeof(epoch_numbers));
}

static void model_save_base(void)
{
   base_next_id = next_id;
   base_trace_epoch = trace_epoch;
   memcpy(base_epoch_numbers, epoch_numbers, sizeof(epoch_numbers));
   memcpy(base_durable, durable, sizeof(durable));
   memcpy(base_record_epoch, record_epoch, sizeof(record_epoch));
}

static void model_restore_base(void)
{
   next_id = base_next_id;
   trace_epoch = base_trace_epoch;
   pending_count = 0;
   memcpy(epoch_numbers, base_epoch_numbers, sizeof(epoch_numbers));
   memcpy(durable, base_durable, sizeof(durable));
   memcpy(record_epoch, base_record_epoch, sizeof(record_epoch));
}

static void model_committed(void)
{
   // The call that just returned committed a page: everything buffered before it is durable now
   for (uint32_t i = 0; i < pending_count; ++i)
      durable[pending[i]] = 1;
   pending_count = 0;
}

static void after_call(uint32_t committed_before, nandlog_status_t status)
{
   if (pages_committed() != committed_before)
      model_committed();
   else if ((status == NANDLOG_ERR_UNWRITABLE) || (status == NANDLOG_ERR_FULL))
      pending_count = 0;                   // a commit that failed lost what was buffered
}

static nandlog_status_t t_store(uint32_t length)
{
   const uint32_t id = next_id++, before = pages_committed();
   fill_record(record, length, id);
   const nandlog_status_t status = nandlog_store_record(7, 1000 + id, record, length);
   after_call(before, status);
   if (status == NANDLOG_OK)
   {
      pending[pending_count++] = id;
      record_epoch[id] = (uint16_t)trace_epoch;
   }
   return status;
}

static void t_flush(bool partial)
{
   const uint32_t before = pages_committed();
   after_call(before, nandlog_flush(partial));
}

static uint32_t current_epoch_number(void)
{
   nandlog_epoch_info_t info;
   memset(&info, 0, sizeof(info));
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   return info.epoch;
}

static void t_begin_epoch(void)
{
   const uint8_t details[8] = "trace";
   const uint32_t before = pages_committed();
   nandlog_begin_session();
   const bool begun = nandlog_begin_epoch(details, sizeof(details));
   nandlog_end_session();
   if (pages_committed() != before)
      model_committed();
   if (begun && (trace_epoch + 1 < MAX_TRACE_EPOCHS))
      epoch_numbers[++trace_epoch] = current_epoch_number();
}

static void t_boot(void)
{
   // A reboot inside a trace: RAM is lost, so whatever was buffered was never durable
   nandlog_deinit();
   nandlog_init();
   pending_count = 0;
}

static void t_pages(uint32_t count)
{
   for (uint32_t i = 0; i < count; ++i)
      t_store(page_record());
}

static void follow_current_epoch(void)
{
   // Records logged from here on belong to whatever epoch the boot found current, which after a cut in the write
   // of a slot may be either the old epoch or the new one
   const uint32_t number = current_epoch_number();
   uint32_t t = 0;
   for (uint32_t i = 0; i <= trace_epoch; ++i)
      if (epoch_numbers[i] == number)
         t = i;
   if (epoch_numbers[t] != number)
   {
      t = (trace_epoch + 1 < MAX_TRACE_EPOCHS) ? (trace_epoch + 1) : trace_epoch;
      epoch_numbers[t] = number;
   }
   trace_epoch = t;
}


// Fault Injection -----------------------------------------------------------------------------------------------------

static nandlog_sim_faults_t faults;
static uint32_t faulty_blocks[2 * NANDLOG_SIM_MAX_FAULT_BLOCKS], num_faulty_blocks;

static void faults_reset(void)
{
   memset(&faults, 0, sizeof(faults));
   memcpy(faults.device_id, DEVICE_ID, sizeof(DEVICE_ID));
   faults.torn_reads_uncorrectable = true;   // a torn page's ECC does not match it
   faults.ecc_hides_bits = 32;              // and a barely torn one reads back as erased with ECC on
   nandlog_sim_set_faults(&faults);
   num_faulty_blocks = 0;
}

static void fail_programs(uint32_t block)
{
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = block;
   faulty_blocks[num_faulty_blocks++] = block;
   nandlog_sim_set_faults(&faults);
}

static void fail_erases(uint32_t block)
{
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = block;
   faulty_blocks[num_faulty_blocks++] = block;
   nandlog_sim_set_faults(&faults);
}

static void clear_program_faults(void)
{
   // The part recovers: no block refuses programs any longer. Those that did still count as made faulty
   faults.num_unwritable_blocks = 0;
   nandlog_sim_set_faults(&faults);
}

static void expect_retirement(uint32_t block)
{
   // A block the base state already made fail, whose retirement a trial may complete
   faulty_blocks[num_faulty_blocks++] = block;
}

static bool block_was_made_faulty(uint32_t block)
{
   for (uint32_t i = 0; i < num_faulty_blocks; ++i)
      if (faulty_blocks[i] == block)
         return true;
   return false;
}

static void factory_mark(uint32_t block)
{
   uint8_t *raw = nandlog_sim_raw_page(block * ppb());
   raw[page_bytes()] = 0x00;
}


// Traces --------------------------------------------------------------------------------------------------------------
//
// Each has a base state, prepared once and snapshotted, and a body, run from it. Factory-bad blocks are block 5,
// in the metadata ring, and block 11, inside the first epoch

#define FACTORY_BAD_RING                            5
#define FACTORY_BAD_LOG                             11

typedef struct
{
   const char *name, *description;
   void (*prepare)(void);                  // from a factory-fresh part; leaves the part to be snapshotted
   void (*body)(void);
   bool fresh_boot;                        // the body starts with the part's first boot
} trace_t;

static void prepare_fresh(void)
{
   factory_mark(FACTORY_BAD_RING);
   factory_mark(FACTORY_BAD_LOG);
}

static void prepare_one_epoch(void)
{
   prepare_fresh();
   nandlog_init();
   t_begin_epoch();
   t_pages(30);
   t_flush(true);
   nandlog_deinit();
}

static void body_basic(void)
{
   // First boot, then two epochs: pages with partial flushes, flush(false) calls that carry the erase-ahead, and an
   // epoch boundary with records still buffered
   nandlog_init();
   t_begin_epoch();
   for (uint32_t i = 0; i < 200; ++i)
   {
      t_store(((i % 9) == 8) ? 300 : page_record());
      t_flush(false);
      if ((i % 37) == 36)
         t_flush(true);
   }
   t_store(40);
   t_begin_epoch();
   t_pages(60);
   t_flush(true);
}

static void body_relocation(void)
{
   // As the basic trace, but the block under the head refuses programs part-way through it, and the block the
   // window would erase next refuses its erase: a relocation, two retirements and two table updates in the trace
   nandlog_init();
   t_begin_epoch();
   for (uint32_t i = 0; i < 220; ++i)
   {
      if (i == 150)
         fail_programs(log_first_block() + 2);
      if (i == 175)
         fail_erases(log_first_block() + 6);
      t_store(page_record());
      t_flush(false);
      if ((i % 37) == 36)
         t_flush(true);
   }
   t_begin_epoch();
   t_pages(40);
   t_flush(true);
}

static void body_first_block_retired(void)
{
   // The epoch's own first block fails three pages in, so seq 0 moves on and the epoch's first block is retired
   nandlog_init();
   t_begin_epoch();
   t_pages(3);
   t_flush(true);
   fail_programs(log_first_block());
   for (uint32_t i = 0; i < 150; ++i)
   {
      t_store(page_record());
      t_flush(false);
   }
   t_flush(true);
}

static void body_ring(void)
{
   // Seventy epochs, each with a page, so the metadata ring is written slot after slot and moves into a new block
   nandlog_init();
   for (uint32_t i = 0; i < 70; ++i)
   {
      t_begin_epoch();
      t_store(page_record());
      t_store(200);
      t_flush(true);
   }
}

static void prepare_table_nearly_full(void)
{
   // Sixty retirements of reserve blocks the log never uses, so the driver's table block is nearly used up
   prepare_fresh();
   nandlog_init();
   for (uint32_t i = 0; i < 60; ++i)
      nandlog_chip_mark_bad_block((log_end_block() + 20 + i) * ppb());
   t_begin_epoch();
   t_pages(10);
   t_flush(true);
   nandlog_deinit();
}

static void body_table(void)
{
   // Four blocks fail as the head reaches them, so the table is rewritten four times, across its block boundary
   nandlog_init();
   static const uint32_t failing[4] = { 1, 5, 7, 9 };      // blocks after the first; 3 is factory-bad
   for (uint32_t i = 0; i < 4; ++i)
      fail_programs(log_first_block() + failing[i]);
   for (uint32_t i = 0; i < 300; ++i)
   {
      t_store(page_record());
      t_flush(false);
   }
   t_flush(true);
}

static uint32_t full_epoch_pages(void)
{
   // The log region less the factory-bad block -- and, on a part with dead blocks, less those and the table's pair
   return SIM_DEAD_RESERVE_BLOCKS ? (good_log_blocks() * ppb()) : ((log_blocks() - 1) * ppb());
}

static void prepare_nearly_full(void)
{
   prepare_fresh();
   nandlog_init();
   t_begin_epoch();
   t_pages(full_epoch_pages() - 100);
   t_flush(true);
   nandlog_deinit();
}

static void body_full(void)
{
   // The region fills, records are refused, a warm boot of the full epoch, a new epoch begun on it -- which
   // starts in the full epoch's own first block -- and logging in that
   nandlog_init();
   for (uint32_t i = 0; i < 140; ++i)
   {
      t_store(page_record());
      t_flush(false);
   }
   t_flush(true);
   t_boot();
   t_begin_epoch();
   t_pages(40);
   t_flush(true);
}

static void prepare_head_in_last_block(void)
{
   prepare_fresh();
   nandlog_init();
   t_begin_epoch();
   t_pages(full_epoch_pages() - 44);
   t_flush(true);
   nandlog_deinit();
}

static void body_wrap_failure(void)
{
   // The last block before the wrap refuses programs twenty pages in: there is nowhere to relocate to but the
   // epoch's own first block, so the region is full
   nandlog_init();
   fail_programs(SIM_DEAD_RESERVE_BLOCKS ? last_good_log_block() : (log_end_block() - 1));
   for (uint32_t i = 0; i < 60; ++i)
   {
      t_store(page_record());
      t_flush(false);
   }
   t_flush(true);
   t_boot();
   t_begin_epoch();
   t_pages(30);
   t_flush(true);
}

static void prepare_epoch_near_the_end(void)
{
   // The head three blocks short of the end of the region, so the next epoch wraps. A one-page epoch per block
   // gets it there with little data for every audit to read back
   prepare_fresh();
   nandlog_init();
   for (uint32_t block = 0; block < ((SIM_DEAD_RESERVE_BLOCKS ? (good_log_blocks() + 1) : log_blocks()) - 4); ++block)
   {
      t_begin_epoch();
      t_pages(1);
      t_flush(true);
   }
   nandlog_deinit();
}

static void body_wrap_with_retired_block(void)
{
   // An epoch that wraps from the end of the region to its start, where a block refuses its erase as the window
   // reaches it, and another fails part-way through being written
   nandlog_init();
   t_begin_epoch();
   fail_erases(log_first_block() + 1);
   for (uint32_t i = 0; i < 300; ++i)
   {
      if (i == 260)
         fail_programs(log_first_block() + 4);
      t_store(page_record());
      t_flush(false);
   }
   t_flush(true);
}

static void prepare_torn_slot(void)
{
   // An epoch with pages, then a cut under the unstable model in the first copy of the next epoch's slot, so it may
   // read valid on one boot and not on the next. The body's first boot has to settle it before logging anything
   prepare_one_epoch();
   nandlog_init();
   nandlog_sim_seed(77);
   nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_UNSTABLE, 0.97, &cut_jump);
   if (setjmp(cut_jump) == 0)
      t_begin_epoch();
   nandlog_sim_power_cut_disarm();
   nandlog_deinit();
}

static void prepare_torn_table(void)
{
   // An epoch's page reads back only at the ECC limit, so its block is relocated and retired, and the cut, under the
   // unstable model, lands in the first copy of the bad-block table version that retires it. The page after it reads
   // at the limit too, because a build that passes over the head's page at every boot commits there instead
   prepare_fresh();
   nandlog_init();
   t_begin_epoch();
   t_pages(10);
   t_flush(true);
   nandlog_deinit();
   nandlog_sim_snapshot();
   for (uint32_t n = 1; n < 40; ++n)
   {
      nandlog_deinit();
      nandlog_sim_restore();
      nandlog_init();
      nandlog_sim_set_ecc_status((log_first_block() * ppb()) + 10, NANDLOG_SIM_ECC_AT_LIMIT);
      nandlog_sim_set_ecc_status((log_first_block() * ppb()) + 11, NANDLOG_SIM_ECC_AT_LIMIT);
      t_store(page_record());
      nandlog_sim_seed(78);
      nandlog_sim_reset_operations();
      nandlog_sim_power_cut(n, NANDLOG_SIM_CUT_UNSTABLE, 0.97, &cut_jump);
      if (setjmp(cut_jump) == 0)
         t_flush(true);
      nandlog_sim_power_cut_disarm();
      pending_count = 0;
      nandlog_sim_cut_t cut;
      if (nandlog_sim_last_cut(&cut) && (cut.command == 0x10) && !cut.otp && (((cut.page / ppb()) >= log_end_block()) || holds_table(cut.page / ppb())))
         break;
   }
   nandlog_deinit();
}

static void prepare_torn_block_start(void)
{
   // The head has just entered a block when a cut tears the block's first page, so the boot finds a block holding
   // nothing of the epoch but a position to pass over: it erases the block again and leaves a tombstone there
   prepare_fresh();
   nandlog_init();
   t_begin_epoch();
   t_pages(ppb());
   t_flush(true);
   t_store(page_record());
   nandlog_sim_seed(79);
   nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_BITWISE, 0.5, &cut_jump);
   if (setjmp(cut_jump) == 0)
      t_flush(true);
   nandlog_sim_power_cut_disarm();
   pending_count = 0;
   nandlog_deinit();
}

static void body_after_torn_write(void)
{
   // The boot settles what the cut left first; then logging, and an epoch begun on top
   nandlog_init();
   follow_current_epoch();
   t_pages(30);
   t_flush(true);
   t_begin_epoch();
   t_pages(10);
   t_flush(true);
}

static void tear_commit(uint64_t seed)
{
   // One more page committed, its program torn by a cut at even odds, and power off
   t_store(60);
   for (uint32_t i = 0; i < 4; ++i)
      t_flush(false);
   nandlog_sim_seed(seed);
   nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_BITWISE, 0.5, &cut_jump);
   if (setjmp(cut_jump) == 0)
      t_flush(true);
   nandlog_sim_power_cut_disarm();
   pending_count = 0;
   nandlog_deinit();
}

static void prepare_torn_loop(void)
{
   // An epoch with pages, then commits torn by cuts one boot after another -- a brown-out loop -- each listed by the
   // boot after it: a run of passed-over positions, then pages, then one more torn commit for the body's first boot
   // to list
   prepare_one_epoch();
   for (uint64_t cut = 0; cut < 3; ++cut)
   {
      nandlog_init();
      tear_commit(91 + cut);
   }
   nandlog_init();
   t_pages(5);
   t_flush(true);
   tear_commit(95);
}

static void body_torn_loop(void)
{
   // The boot that lists the last torn page, then torn commits and boots in turn: every cut of the campaign lands in
   // a commit, in the slot a boot writes to list what the last cut tore, or in the boots between
   nandlog_init();
   follow_current_epoch();
   t_pages(3);
   t_flush(true);
   t_store(60);
   t_flush(true);
   t_boot();
   t_pages(2);
   t_flush(true);
   t_boot();
   t_pages(3);
   t_flush(true);
}

static void body_after_torn_table(void)
{
   expect_retirement(log_first_block());
   body_after_torn_write();
}

static void body_unwritable(void)
{
   // Every block the commit can reach refuses programs, so it gives up and records the failed head in the ring. The
   // part recovers before the reboot, which moves the failed head's block out of the way before logging on
   nandlog_init();
   for (uint32_t i = 0; i < 3; ++i)
      fail_programs(log_first_block() + i);
   t_pages(2);
   t_flush(true);
   clear_program_faults();
   t_boot();
   t_pages(30);
   t_flush(true);
}

static bool raw_header_erased(uint32_t page)
{
   const uint8_t *raw = nandlog_sim_raw_page(page);
   for (uint32_t i = 0; i < 16; ++i)
      if (raw[i] != 0xFF)
         return false;
   return true;
}

static void body_refused(void)
{
   // The block under the head refuses every program, its mark included. On a part whose reserve is dead the table's
   // pair below it can take no more either -- its next page refuses the version, its other block the erase -- so the
   // marks take over, refuse the block too, and the log gives the table a new pair mid-logging and retires the block
   // through it. On a healthy part, a relocation whose block will not take its mark
   nandlog_init();
   uint32_t pair = ~0u;
   for (uint32_t b = table_search_first_block(); SIM_DEAD_RESERVE_BLOCKS && (b < log_end_block()); b += 2)
      if (holds_table(b) && block_bad(b))
         pair = b & ~1u;
   for (uint32_t i = 0; i < 60; ++i)
   {
      if (i == 20)
      {
         if (pair != ~0u)
         {
            // The block holding the newest version is the one written furthest, by page, of the pair; its next page
            // refuses, and the other block refuses the erase that would rotate into it
            uint32_t ends[2], next = 0;
            for (uint32_t k = 0; k < 2; ++k)
               for (ends[k] = (pair + k) * ppb(); (ends[k] < (((pair + k + 1) * ppb()) - 1)) && !raw_header_erased(ends[k]); ++ends[k])
                  ;
            const uint32_t home = ((ends[1] % ppb()) > (ends[0] % ppb())) ? (pair + 1) : pair;
            next = ends[home - pair];
            faults.unwritable_pages[faults.num_unwritable_pages++] = next;
            faults.unerasable_blocks[faults.num_unerasable_blocks++] = home ^ 1u;
         }
         fail_programs(log_first_block());
      }
      t_store(page_record());
      t_flush(false);
   }
   t_flush(true);
}

static const trace_t traces[] =
{
   { "basic",   "first boot, two epochs, partial flushes, a factory-bad block inside the epoch", prepare_fresh, body_basic, true },
   { "reloc",   "a block failing under the head, and a window block refusing its erase", prepare_fresh, body_relocation, true },
   { "first",   "the epoch's first block failing three pages in", prepare_fresh, body_first_block_retired, true },
   { "ring",    "seventy epochs, crossing a metadata ring block", prepare_one_epoch, body_ring, false },
   { "table",   "four retirements across the bad-block table's block boundary", prepare_table_nearly_full, body_table, false },
   { "full",    "the region filling, a warm boot of it full, a new epoch on it", prepare_nearly_full, body_full, false },
   { "wrapfail","the last block before the wrap failing", prepare_head_in_last_block, body_wrap_failure, false },
   { "wrapbad", "an epoch wrapping onto a block that refuses its erase, and one that fails", prepare_epoch_near_the_end, body_wrap_with_retired_block, false },
   { "slottorn","a boot settling a new epoch's slot torn under the unstable model", prepare_torn_slot, body_after_torn_write, false },
   { "tabletorn","a boot settling a bad-block table version torn under the unstable model", prepare_torn_table, body_after_torn_table, false },
   { "blocktorn","a boot erasing again a block whose only page of the epoch is torn", prepare_torn_block_start, body_after_torn_write, false },
   { "unwritable","a commit no block will take, recorded in the ring, then relocated after a reboot", prepare_one_epoch, body_unwritable, false },
   { "tornloop","torn commits listed by the boots after them, with cuts in those boots' slot writes", prepare_torn_loop, body_torn_loop, false },
   { "refused", "a block under the head refusing even its mark; on a dead reserve the table's pair failing too", prepare_one_epoch, body_refused, false },
};
#define NUM_TRACES                                  (sizeof(traces) / sizeof(traces[0]))


// The Audit -----------------------------------------------------------------------------------------------------------

typedef struct
{
   uint32_t lost, damaged, duplicates, misplaced, seq_breaks, span_errors, repeated, false_retired, table_lost, reissued;
   uint32_t blank_reprograms;              // not a violation: a page no read can tell from erased, programmed again
} verdict_t;

static bool verdict_clean(const verdict_t *v)
{
   return !(v->lost | v->damaged | v->duplicates | v->misplaced | v->seq_breaks | v->span_errors | v->repeated | v->false_retired |
            v->table_lost | v->reissued);
}

static void verdict_add(verdict_t *sum, const verdict_t *v)
{
   sum->lost += v->lost;
   sum->damaged += v->damaged;
   sum->duplicates += v->duplicates;
   sum->misplaced += v->misplaced;
   sum->seq_breaks += v->seq_breaks;
   sum->span_errors += v->span_errors;
   sum->repeated += v->repeated;
   sum->false_retired += v->false_retired;
   sum->table_lost += v->table_lost;
   sum->reissued += v->reissued;
   sum->blank_reprograms += v->blank_reprograms;
}

static uint32_t walk_records(const uint8_t *payload, uint32_t length, uint32_t epoch_number, uint32_t *ids, uint32_t capacity, verdict_t *v)
{
   // Check every record in a delivered payload against its id; returns the ids found
   uint32_t offset = 0, found = 0;
   while (offset < length)
   {
      uint32_t data_length = length - offset - prefix_bytes() - 5;
      if (NANDLOG_RECORD_FRAMING)
      {
         uint16_t framed = 0;
         memcpy(&framed, payload + offset, sizeof(framed));
         data_length = framed;
      }
      const uint32_t total = prefix_bytes() + 5 + data_length;
      if (((offset + total) > length) || (data_length < 4))
      {
         if (v)
            ++v->damaged;
         return found;
      }
      const uint8_t *data = payload + offset + prefix_bytes() + 5;
      uint32_t id = 0;
      memcpy(&id, data, sizeof(id));
      fill_record(record, data_length, id);
      if ((id >= MAX_IDS) || memcmp(record, data, data_length))
      {
         if (v)
            ++v->damaged;
      }
      else
      {
         if (v)
         {
            if (seen[id])
               ++v->duplicates;
            if (epoch_number && record_epoch[id] && (epoch_numbers[record_epoch[id]] != epoch_number))
               ++v->misplaced;
         }
         seen[id] = 1;
         if (ids && (found < capacity))
            ids[found] = id;
         ++found;
      }
      offset += total;
   }
   return found;
}

static void map_record_pages(void)
{
   // Where every record sits on the part, so an earlier epoch's record can later be excused if a newer epoch's
   // write head has swept its block. Called on the base state and again after the uncut run; a record already
   // placed keeps its first placement, which is where it was before anything swept it
   const uint32_t pages = log_end_block() * ppb();
   for (uint32_t page = log_first_block() * ppb(); page < pages; ++page)
   {
      if (block_bad(page / ppb()))
         continue;
      const uint8_t *raw = nandlog_sim_raw_page(page);
      nandlog_page_header_t header;
      memcpy(&header, raw, sizeof(header));
      if (((header.magic != NANDLOG_PAGE_MAGIC) && (header.magic != NANDLOG_PAGE_MAGIC_FRAMED)) || !header.payload_length ||
          (header.payload_length > nandlog_data_bytes_per_page()))
         continue;
      uint32_t ids[64];
      const uint32_t n = walk_records(raw + sizeof(header), header.payload_length, 0, ids, 64, NULL);
      for (uint32_t i = 0; (i < n) && (i < 64); ++i)
         if (id_page[ids[i]] == 0xFFFFFFFF)
            id_page[ids[i]] = page;
   }
   memset(seen, 0, sizeof(seen));
}

static bool swept_by_current_epoch(uint32_t page, uint32_t start_block, uint32_t positions)
{
   // Whether the current epoch's write head has legitimately erased the block of 'page': its span, the head's
   // block, and the erase-ahead window beyond it
   const uint32_t reach = (positions / ppb()) + 1 + NANDLOG_ERASE_AHEAD_BLOCKS;
   uint32_t block = start_block;
   for (uint32_t good = 0, steps = 0; (good < reach) && (steps < log_blocks()); ++steps, block = log_block_after(block))
   {
      // A block the sweep retired, because it would not erase or failed once erased, was swept just the same
      if (block == (page / ppb()))
         return true;
      if (!block_bad(block))
         ++good;
   }
   return false;
}

static uint32_t payload_crc(const uint8_t *payload, uint32_t length)
{
   uint32_t crc = 0xFFFFFFFF;
   for (uint32_t i = 0; i < length; ++i)
   {
      crc ^= payload[i];
      for (uint32_t bit = 0; bit < 8; ++bit)
         crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
   }
   return ~crc ^ length;
}

static bool identity_kept(uint32_t epoch, uint32_t seq, const uint8_t *payload, uint32_t length)
{
   // Whether what a read just served under (epoch, seq) agrees with what it was first served as in this trial
   if (seq >= IDENTITY_SEQS)
      return true;
   identity_t *entry = &identity[epoch % IDENTITY_EPOCHS][seq];
   if ((entry->trial != identity_trial) || (entry->epoch != epoch))
   {
      entry->trial = identity_trial;
      entry->epoch = epoch;
      entry->data = (length > 0);
      entry->crc = length ? payload_crc(payload, length) : 0;
      entry->bound = next_id;
      return true;
   }
   if (!length)
      return true;
   if (entry->data)
      return entry->crc == payload_crc(payload, length);

   // First served as a gap: what it serves now may be what a torn program left, but no record stored since
   uint32_t ids[64];
   const uint32_t found = walk_records(payload, length, 0, ids, 64, NULL);
   for (uint32_t i = 0; (i < found) && (i < 64); ++i)
      if (ids[i] >= entry->bound)
         return false;
   entry->data = true;
   entry->crc = payload_crc(payload, length);
   return true;
}

static verdict_t audit(void)
{
   // Read every epoch the ring lists, check every record that comes back, then account for every durable one
   verdict_t v;
   memset(&v, 0, sizeof(v));
   memset(seen, 0, sizeof(seen));
   nandlog_begin_session();
   const uint32_t count = nandlog_epoch_count();
   uint32_t current_start = 0, current_positions = 0;
   for (uint32_t e = 0; e < count; ++e)
   {
      nandlog_epoch_info_t info;
      if (!nandlog_epoch_info(e, &info))
         continue;
      if (e)
      {
         if (!nandlog_select_epoch(info.epoch))
            continue;
      }
      else
      {
         nandlog_select_current_epoch();
         current_start = info.first_page / ppb();
      }
      uint32_t positions = 0, bytes = 0, delivered = 0;
      nandlog_begin_reading(0, 0);
      nandlog_read_span(&positions, &bytes);
      if (!e)
         current_positions = positions;
      for (uint32_t i = 0; i < positions; ++i)
      {
         nandlog_page_header_t header;
         const uint32_t length = nandlog_retrieve_next_page(readback, &header);
         if (!e && (header.seq != i) && (i < current_positions))
            ++v.seq_breaks;
         if (length)
         {
            delivered += length;
            walk_records(readback, length, info.epoch, NULL, 0, &v);
         }
         if (!identity_kept(info.epoch, header.seq, readback, length))
            ++v.reissued;
      }
      nandlog_end_reading();
      if (delivered != bytes)
         ++v.span_errors;
   }
   nandlog_select_current_epoch();
   nandlog_end_session();

   // Only the newest NANDLOG_MAX_LISTED_EPOCHS epochs can be selected for reading at all, by design, so a record
   // of an older one cannot be audited through the library
   const uint32_t current_number = current_epoch_number();
   const uint32_t oldest_listed = (current_number >= NANDLOG_MAX_LISTED_EPOCHS) ? (current_number - NANDLOG_MAX_LISTED_EPOCHS + 1) : 0;
   for (uint32_t id = 0; id < next_id; ++id)
   {
      if (!durable[id] || seen[id] || (epoch_numbers[record_epoch[id]] < oldest_listed))
         continue;
      // A record of an earlier epoch whose block the current epoch has swept is gone by design
      const bool earlier = epoch_numbers[record_epoch[id]] != current_number;
      if (earlier && (id_page[id] != 0xFFFFFFFF) && swept_by_current_epoch(id_page[id], current_start, current_positions))
         continue;
      ++v.lost;
   }
   if (!block_bad(FACTORY_BAD_RING) || !block_bad(FACTORY_BAD_LOG))
      ++v.table_lost;
   return v;
}

static uint32_t blank_reprograms_outside_retired_blocks(void)
{
   // Pages that read erased in every bit, programmed again after a reboot -- leaving out the marker the driver
   // writes into a block it retires, which can land on such a page when a boot finishes a retirement
   uint32_t pages[64];
   const uint32_t total = nandlog_sim_blank_reprogram_pages(pages, 64), held = (total < 64) ? total : 64;
   uint32_t count = (total > 64) ? (total - 64) : 0;
   for (uint32_t i = 0; i < held; ++i)
      if (!block_bad(pages[i] / ppb()))
         ++count;
   return count;
}

static uint32_t repeated_programs_outside_retired_blocks(void)
{
   uint32_t pages[64];
   const uint32_t total = nandlog_sim_repeated_program_pages(pages, 64), held = (total < 64) ? total : 64;
   uint32_t defects = (total > 64) ? (total - 64) : 0;
   for (uint32_t i = 0; i < held; ++i)
      if (!block_bad(pages[i] / ppb()))
         ++defects;
   return defects;
}


// The Campaign --------------------------------------------------------------------------------------------------------

static const char *model_names[4] = { "boundary", "prefix", "bitwise", "unstable" };
static const nandlog_sim_cut_model_t models[4] = { NANDLOG_SIM_CUT_BOUNDARY, NANDLOG_SIM_CUT_PREFIX, NANDLOG_SIM_CUT_BITWISE, NANDLOG_SIM_CUT_UNSTABLE };

// Per-bit completion probabilities the bit-level models cycle through, from a cut in the first instant of a
// program, which flips a handful of bits, to one in its last
static const double q_values[8] = { 0.0005, 0.002, 0.01, 0.05, 0.2, 0.5, 0.95, 0.995 };

static uint8_t retired_before[4096];

static void note_retired(void)
{
   for (uint32_t b = 0; b < nandlog_chip_geometry()->block_count; ++b)
      retired_before[b] = block_bad(b) ? 1 : 0;
}

static uint32_t newly_retired_good_blocks(void)
{
   // Blocks retired since the cut that the trace never made fail. The factory-bad blocks count as known bad
   // even when the cut came before the first table held them
   uint32_t count = 0;
   for (uint32_t b = 0; b < log_end_block(); ++b)
      if (block_bad(b) && !retired_before[b] && !block_was_made_faulty(b) && (b != FACTORY_BAD_RING) && (b != FACTORY_BAD_LOG) &&
          !nandlog_sim_block_dead(b) && !holds_table(b))
         ++count;
   return count;
}

static bool trial_unstable, trial_partial_drift;

static bool run_to_cut(const trace_t *trace, uint32_t cut, nandlog_sim_cut_model_t model, double q, uint64_t seed)
{
   // Restore the base state, and the model of it, and run the trace with power cut at operation 'cut' (zero: no
   // cut). True if it fired
   nandlog_deinit();
   nandlog_sim_restore();
   faults_reset();
   model_restore_base();
   nandlog_sim_seed(seed);
   nandlog_sim_reset_operations();
   trial_unstable = (model == NANDLOG_SIM_CUT_UNSTABLE);
   ++identity_trial;
   if (cut)
      nandlog_sim_power_cut(cut, model, q, &cut_jump);
   if (setjmp(cut_jump) == 0)
      trace->body();
   nandlog_sim_power_cut_disarm();
   return cut && nandlog_sim_power_failed();
}

static void carry_on(uint32_t pages)
{
   // Log on as a device would. A trace cut before its first epoch has none, so one is begun
   if (!current_epoch_number())
      t_begin_epoch();
   else
      follow_current_epoch();
   pending_count = 0;
   t_pages(pages);
   t_flush(true);
}

static void boot_after_cut(void)
{
   nandlog_init();
   if (trace_epoch < MAX_TRACE_EPOCHS)
      epoch_numbers[trace_epoch] = epoch_numbers[trace_epoch] ? epoch_numbers[trace_epoch] : current_epoch_number();
}

static verdict_t recover_and_audit_from(bool noted)
{
   // The boot after the cut, the audit, more logging, another boot, and the audit again. 'noted' says the blocks
   // retired before recovery began are already noted
   verdict_t v1, v2;
   nandlog_deinit();
   if (!noted)
   {
      note_retired();
      nandlog_sim_clear_repeated_programs();
   }
   boot_after_cut();
   v1 = audit();
   carry_on(CONTINUE_PAGES);
   nandlog_deinit();
   nandlog_init();
   v2 = audit();
   verdict_add(&v1, &v2);

   // Undecided bits read differently on every boot, so after an unstable cut every later boot must agree too
   for (uint32_t boot = 0; trial_unstable && (boot < 3); ++boot)
   {
      nandlog_deinit();
      nandlog_init();
      v2 = audit();
      verdict_add(&v1, &v2);
   }

   // Time passes, and every weak page drifts. Nothing durable may go with them, and none may be programmed again
   if (nandlog_sim_weak_pages())
   {
      nandlog_deinit();
      nandlog_sim_drift(trial_partial_drift ? NANDLOG_SIM_DRIFT_PARTIAL : NANDLOG_SIM_DRIFT_BLANK, 1.0);
      nandlog_init();
      v2 = audit();
      verdict_add(&v1, &v2);
      carry_on(CONTINUE_PAGES / 4);
      nandlog_deinit();
      nandlog_init();
      v2 = audit();
      verdict_add(&v1, &v2);
   }
   v1.repeated = repeated_programs_outside_retired_blocks();
   v1.false_retired = newly_retired_good_blocks();
   v1.blank_reprograms = blank_reprograms_outside_retired_blocks();
   return v1;
}

static verdict_t recover_and_audit(void)
{
   return recover_and_audit_from(false);
}

static bool recover_then_cut(uint32_t later, nandlog_sim_cut_model_t model, double q, verdict_t *v)
{
   // The boot after the cut runs to its end, and the logging after it is cut at its 'later'-th operation; then
   // recovery as ever. False if the logging never reached that operation
   nandlog_deinit();
   note_retired();
   nandlog_sim_clear_repeated_programs();
   boot_after_cut();
   nandlog_sim_reset_operations();
   nandlog_sim_power_cut(later, model, q, &cut_jump);
   if (setjmp(cut_jump) == 0)
      carry_on(CONTINUE_PAGES);
   nandlog_sim_power_cut_disarm();
   if (!nandlog_sim_power_failed())
      return false;
   *v = recover_and_audit_from(true);
   return true;
}

static int run_campaign(const char *name, int only_model)
{
   const trace_t *trace = NULL;
   for (uint32_t i = 0; i < NUM_TRACES; ++i)
      if (!strcmp(traces[i].name, name))
         trace = &traces[i];
   if (!trace)
   {
      fprintf(stderr, "no trace named %s\n", name);
      return 2;
   }

   // The base state, and one uncut run to count operations and to map where every record lands
   create_part();
   faults_reset();
   model_reset();
   for (uint32_t i = 0; i < MAX_IDS; ++i)
      id_page[i] = 0xFFFFFFFF;
   trace->prepare();
   nandlog_deinit();
   model_save_base();
   if (!trace->fresh_boot)
      map_record_pages();
   nandlog_sim_snapshot();
   run_to_cut(trace, 0, NANDLOG_SIM_CUT_BOUNDARY, 0.5, 1);
   const uint32_t operations = nandlog_sim_operations();
   map_record_pages();
   const verdict_t uncut = recover_and_audit();
   if (only_model <= 0)
      printf("# trace %s: %s; %u program/erase operations; uncut run %s\n", trace->name, trace->description, operations,
             verdict_clean(&uncut) ? "clean" : "VIOLATES THE CONTRACT");

   int status = verdict_clean(&uncut) ? 0 : 1;
   for (uint32_t m = 0; m < 4; ++m)
   {
      if ((only_model >= 0) && (m != (uint32_t)only_model))
         continue;
      uint32_t trials = 0, clean = 0, second_trials = 0, second_clean = 0, blank_reprogram_trials = 0;
      verdict_t sum;
      memset(&sum, 0, sizeof(sum));
      for (uint32_t n = 1; n <= operations; ++n)
      {
         const double q = q_values[(n + m) % 8];
         const uint64_t seed = 0xC0FFEE00ULL + (n * 131u) + m;
         trial_partial_drift = (n & 1);
         if (!run_to_cut(trace, n, models[m], q, seed))
            continue;

         // How much work the recovering boot does, uncut
         nandlog_deinit();
         nandlog_sim_reset_operations();
         nandlog_init();
         const uint32_t boot_operations = nandlog_sim_operations();

         // The single cut
         run_to_cut(trace, n, models[m], q, seed);
         const verdict_t v = recover_and_audit();
         ++trials;
         blank_reprogram_trials += v.blank_reprograms ? 1 : 0;
         if (verdict_clean(&v))
            ++clean;
         else
         {
            verdict_add(&sum, &v);
            printf("  VIOLATION %s %s n=%u q=%.4f: lost %u damaged %u dup %u misplaced %u seq %u span %u repeated %u retired %u table %u reissued %u\n",
                   trace->name, model_names[m], n, q, v.lost, v.damaged, v.duplicates, v.misplaced, v.seq_breaks, v.span_errors,
                   v.repeated, v.false_retired, v.table_lost, v.reissued);
         }

         // A second cut at every operation of the recovering boot
         for (uint32_t k = 1; k <= boot_operations; ++k)
         {
            run_to_cut(trace, n, models[m], q, seed);
            nandlog_deinit();
            nandlog_sim_seed(seed ^ (0x5151ULL * k));
            nandlog_sim_power_cut(k, models[m], q_values[(n + m + k) % 8], &cut_jump);
            if (setjmp(cut_jump) == 0)
               nandlog_init();
            nandlog_sim_power_cut_disarm();
            const verdict_t v2 = recover_and_audit();
            ++second_trials;
            blank_reprogram_trials += v2.blank_reprograms ? 1 : 0;
            if (verdict_clean(&v2))
               ++second_clean;
            else
            {
               verdict_add(&sum, &v2);
               printf("  VIOLATION %s %s n=%u k=%u: lost %u damaged %u dup %u misplaced %u seq %u span %u repeated %u retired %u table %u reissued %u\n",
                      trace->name, model_names[m], n, k, v2.lost, v2.damaged, v2.duplicates, v2.misplaced, v2.seq_breaks,
                      v2.span_errors, v2.repeated, v2.false_retired, v2.table_lost, v2.reissued);
            }
         }

         // And a second cut in the logging after a recovering boot that finished: its first commits and erases
         static const uint32_t later_cuts[4] = { 1, 2, 3, 6 };
         for (uint32_t c = 0; c < 4; ++c)
         {
            verdict_t v3;
            run_to_cut(trace, n, models[m], q, seed);
            nandlog_sim_seed(seed ^ (0xA5A5ULL * later_cuts[c]));
            if (!recover_then_cut(later_cuts[c], models[m], q_values[(n + m + c + 3) % 8], &v3))
               continue;
            ++second_trials;
            blank_reprogram_trials += v3.blank_reprograms ? 1 : 0;
            if (verdict_clean(&v3))
               ++second_clean;
            else
            {
               verdict_add(&sum, &v3);
               printf("  VIOLATION %s %s n=%u later=%u: lost %u damaged %u dup %u misplaced %u seq %u span %u repeated %u retired %u table %u reissued %u\n",
                      trace->name, model_names[m], n, later_cuts[c], v3.lost, v3.damaged, v3.duplicates, v3.misplaced, v3.seq_breaks,
                      v3.span_errors, v3.repeated, v3.false_retired, v3.table_lost, v3.reissued);
            }
         }
      }
      // One line per trace and model, fields in a fixed order for the Makefile's table: trace, model, operations,
      // single-cut trials and clean ones, double-cut trials and clean ones, then the violations summed over
      // trials -- records lost, records damaged, duplicates, misplaced, sequence breaks, span errors, repeated
      // programs, good blocks retired, factory-bad blocks forgotten, sequence numbers given other content -- and
      // the trials in which a page that read erased in every bit, after a reboot, was programmed again
      printf("RESULT %s %s %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n", trace->name, model_names[m], operations, trials, clean,
             second_trials, second_clean, sum.lost, sum.damaged, sum.duplicates, sum.misplaced, sum.seq_breaks, sum.span_errors,
             sum.repeated, sum.false_retired, sum.table_lost, sum.reissued, blank_reprogram_trials);
      if ((clean != trials) || (second_clean != second_trials))
         status = 1;
   }
   nandlog_deinit();
   nandlog_sim_destroy();
   return status;
}


// The Recovery Sweep --------------------------------------------------------------------------------------------------

typedef enum { FAULT_ROTTED_HEADER, FAULT_UNCORRECTABLE, FAULT_AT_ECC_LIMIT, FAULT_TYPES } sweep_fault_t;

typedef struct { uint32_t trials, wrong_head, lost, damaged, repeated; } sweep_tally_t;

static void sweep_audit(uint32_t expected_positions, uint32_t first_id, uint32_t count, uint32_t unreadable_id, sweep_tally_t *t)
{
   // After a clean reboot: the head where it was, every readable page delivered, then five more pages land after
   memset(seen, 0, sizeof(seen));
   nandlog_begin_session();
   uint32_t positions = 0, bytes = 0, delivered = 0;
   nandlog_begin_reading(0, 0);
   nandlog_read_span(&positions, &bytes);
   verdict_t v;
   memset(&v, 0, sizeof(v));
   for (uint32_t i = 0; i < positions; ++i)
   {
      nandlog_page_header_t header;
      const uint32_t length = nandlog_retrieve_next_page(readback, &header);
      v.seq_breaks += (header.seq != i) ? 1 : 0;
      if (length)
         walk_records(readback, length, 0, NULL, 0, &v);
      delivered += length;
   }
   nandlog_end_reading();
   nandlog_end_session();
   bool head_right = (positions == expected_positions) && !v.seq_breaks && (delivered == bytes);
   uint32_t lost = 0;
   for (uint32_t i = 0; i < count; ++i)
      lost += (!seen[first_id + i] && ((first_id + i) != unreadable_id)) ? 1 : 0;

   // Five more pages must land at the next five positions, and survive another reboot
   nandlog_sim_clear_repeated_programs();
   const uint32_t more = next_id;
   t_pages(5);
   t_flush(true);
   nandlog_deinit();
   nandlog_init();
   memset(seen, 0, sizeof(seen));
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   nandlog_read_span(&positions, NULL);
   for (uint32_t i = 0; i < 5; ++i)
   {
      nandlog_page_header_t header;
      const uint32_t length = nandlog_retrieve_page_by_seq(expected_positions + i, readback, &header);
      if (length)
         walk_records(readback, length, 0, NULL, 0, &v);
   }
   nandlog_end_reading();
   nandlog_end_session();
   for (uint32_t i = 0; i < 5; ++i)
      lost += seen[more + i] ? 0 : 1;
   head_right = head_right && (positions == (expected_positions + 5));

   ++t->trials;
   t->wrong_head += head_right ? 0 : 1;
   t->lost += lost ? 1 : 0;
   t->damaged += (v.damaged || v.duplicates) ? 1 : 0;
   t->repeated += repeated_programs_outside_retired_blocks() ? 1 : 0;
}

static uint32_t sweep_page_of(uint32_t start_block, uint32_t position)
{
   // Good position to page, independently of the library
   uint32_t block = start_block, k = position / ppb();
   for (uint32_t steps = 0; steps < log_blocks(); ++steps, block = log_block_after(block))
      if (!block_bad(block) && !k--)
         return (block * ppb()) + (position % ppb());
   return 0;
}

// Epoch lengths, and the fault positions tried in each: every position for the short ones, and for the long ones
// the first two blocks, the last two before the head, and every thirteenth position between
static const uint32_t sweep_lengths[] = { 1, 2, 63, 64, 65, 100, 127, 128, 129, 200, 640, 1000, 1500, 2000 };
#define SWEEP_LENGTHS                               (sizeof(sweep_lengths) / sizeof(sweep_lengths[0]))

static int run_sweep_faults(uint32_t placement, uint32_t fault, uint32_t length_index)
{
   int status = 0;
   {
      sweep_tally_t tally[FAULT_TYPES];
      memset(tally, 0, sizeof(tally));
      for (uint32_t li = length_index; li == length_index; ++li)
      {
         const uint32_t length = sweep_lengths[li];
         create_part();
         faults_reset();
         model_reset();
         nandlog_init();
         if (placement)
         {
            // A first epoch filling all but the last three blocks, so the epoch under test begins there and wraps
            t_begin_epoch();
            t_pages((log_blocks() - 3) * ppb());
            t_flush(true);
         }
         t_begin_epoch();
         const uint32_t first_id = next_id;
         t_pages(length);
         t_flush(true);
         nandlog_epoch_info_t info;
         nandlog_begin_session();
         nandlog_epoch_info(0, &info);
         nandlog_end_session();
         const uint32_t start_block = info.first_page / ppb();
         nandlog_deinit();
         nandlog_sim_snapshot();
         const uint32_t saved_next = next_id;

         for (uint32_t p = 0; p < length; ++p)
         {
            const bool tried = (length <= 200) || (p < (2 * ppb())) || (p >= (length - (2 * ppb()))) || !(p % 13);
            if (!tried)
               continue;
            for (uint32_t f = fault; f == fault; ++f)
            {
               nandlog_sim_restore();
               next_id = saved_next;
               const uint32_t page = sweep_page_of(start_block, p);
               if (f == FAULT_ROTTED_HEADER)
                  nandlog_sim_corrupt(page, 0, 0xFF);
               else
                  nandlog_sim_set_ecc_status(page, (f == FAULT_UNCORRECTABLE) ? NANDLOG_SIM_ECC_UNCORRECTABLE : NANDLOG_SIM_ECC_AT_LIMIT);
               nandlog_init();
               sweep_audit(length, first_id, length, (f == FAULT_AT_ECC_LIMIT) ? 0xFFFFFFFF : (first_id + p), &tally[f]);
               nandlog_deinit();
            }
         }
         nandlog_sim_destroy();
      }
      // Fields: placement, fault, epoch length, reboots, then the reboots that misplaced the head, lost a
      // committed page, delivered a damaged or duplicated record, or programmed a page twice
      const uint32_t f = fault;
      printf("SWEEP %u %u %u %u %u %u %u %u\n", placement, f, sweep_lengths[length_index], tally[f].trials, tally[f].wrong_head,
             tally[f].lost, tally[f].damaged, tally[f].repeated);
      if (tally[f].wrong_head || tally[f].lost || tally[f].damaged || tally[f].repeated)
         status = 1;
   }
   return status;
}

static int run_sweep_retired(uint32_t placement)
{
   // A retired block at each block index of the epoch, its first included, with the head at every page of the
   // three blocks that follow it; and the same with the retired block just past the wrap
   int status = 0;
   {
      sweep_tally_t tally;
      memset(&tally, 0, sizeof(tally));
      for (uint32_t index = 0; index < 5; ++index)
      {
         create_part();
         faults_reset();
         model_reset();
         const uint32_t base_block = placement ? (log_end_block() - 3) : log_first_block();
         uint32_t bad_block = base_block + index;
         if (bad_block >= log_end_block())
            bad_block = log_first_block() + (bad_block - log_end_block());
         nandlog_init();
         if (placement)
         {
            t_begin_epoch();
            t_pages((log_blocks() - 3) * ppb());
            t_flush(true);
         }

         // Retired the way the log retires a block, which is what any later boot trusts
         nandlog_chip_mark_bad_block(bad_block * ppb());
         nandlog_deinit();
         nandlog_sim_snapshot();
         const uint32_t saved_next = next_id;
         for (uint32_t extra = 1; extra <= (3 * ppb()); extra += (placement ? 3 : 1))
         {
            nandlog_sim_restore();
            next_id = saved_next;
            nandlog_init();
            t_begin_epoch();
            const uint32_t first_id = next_id, pages = (index * ppb()) + extra;
            t_pages(pages);
            t_flush(true);
            nandlog_deinit();
            nandlog_init();
            sweep_audit(pages, first_id, pages, 0xFFFFFFFF, &tally);
            nandlog_deinit();
         }
         nandlog_sim_destroy();
      }
      printf("SWEEP %u %u %u %u %u %u %u %u\n", placement, FAULT_TYPES, 0, tally.trials, tally.wrong_head, tally.lost, tally.damaged, tally.repeated);
      if (tally.wrong_head || tally.lost || tally.damaged || tally.repeated)
         status = 1;
   }
   return status;
}

static uint32_t crc32_bits(const uint8_t *bytes, uint32_t length)
{
   // Written here rather than borrowed from the library, so a check of the medium does not trust the code under test
   uint32_t crc = 0xFFFFFFFF;
   for (uint32_t i = 0; i < length; ++i)
   {
      crc ^= bytes[i];
      for (uint32_t bit = 0; bit < 8; ++bit)
         crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
   }
   return ~crc;
}

static uint32_t newest_ring_pair(void)
{
   // The even page of the pair holding the slot of the highest sequence number whose head and extension check, judged
   // from the medium, or ~0
   uint32_t best = 0xFFFFFFFF, sequence = 0;
   for (uint32_t page = 0; page < (METADATA_RING_BLOCKS * ppb()); ++page)
   {
      const uint8_t *raw = nandlog_sim_raw_page(page);
      nandlog_meta_header_t header;
      nandlog_meta_extension_t extension;
      memcpy(&header, raw, sizeof(header));
      memcpy(&extension, raw + sizeof(header), sizeof(extension));
      if ((header.magic == NANDLOG_META_MAGIC) && (crc32_bits(raw, NANDLOG_META_EXTENSION_CRC_BYTES) == extension.extension_crc) &&
          ((best == 0xFFFFFFFF) || (extension.sequence > sequence)))
      {
         best = page & ~1u;
         sequence = extension.sequence;
      }
   }
   return best;
}

static uint32_t newest_table_pair(uint32_t first_block, uint32_t blocks)
{
   // The same for the bad-block table's highest version that checks, in the unmarked blocks of those given
   uint32_t best = 0xFFFFFFFF, newest = 0;
   for (uint32_t page = first_block * ppb(); page < ((first_block + blocks) * ppb()); ++page)
   {
      const uint8_t *raw = nandlog_sim_raw_page(page);
      uint32_t crc = 0, version = 0, count = 0;
      memcpy(&crc, raw + 4, sizeof(crc));
      memcpy(&version, raw + 8, sizeof(version));
      memcpy(&count, raw + 12, sizeof(count));
      if ((nandlog_sim_raw_page((page / ppb()) * ppb())[page_bytes()] == 0xFF) && !memcmp(raw, "BBT3", 4) && (count <= 256) &&
          (crc32_bits(raw + 8, 8 + (4 * count)) == crc) && ((best == 0xFFFFFFFF) || (version > newest)))
      {
         best = page & ~1u;
         newest = version;
      }
   }
   return best;
}

static uint32_t metadata_writes(uint32_t table_first, uint32_t table_blocks)
{
   // Programs and erases of the ring's blocks and the table's since the counters were reset. Every boot erases the
   // window ahead of the head again, which is not counted
   uint32_t writes = 0;
   for (uint32_t b = 0; b < METADATA_RING_BLOCKS; ++b)
      writes += nandlog_sim_block_erases(b) + nandlog_sim_block_programs(b);
   for (uint32_t b = table_first; b < (table_first + table_blocks); ++b)
      writes += nandlog_sim_block_erases(b) + nandlog_sim_block_programs(b);
   return writes;
}

static int run_sweep_metadata(uint32_t target, uint32_t fault)
{
   // Every written page of the metadata ring (target 0) or of the bad-block table's blocks (target 1), one at a time,
   // rotted (fault 0), made uncorrectable (fault 1) or made to read at the ECC limit (fault 2) before a reboot. The
   // part carries forty earlier epochs, so the ring has crossed a block, and forty-odd table versions, so the table has
   // too, the newest of them retiring the block after the current epoch's first, so that its four blocks of pages lie
   // over a retired one
   create_part();
   faults_reset();
   model_reset();
   prepare_fresh();
   nandlog_init();
   for (uint32_t i = 0; i < 40; ++i)
   {
      t_begin_epoch();
      t_pages(1);
      t_flush(true);
   }
   for (uint32_t i = 0; i < 40; ++i)
      nandlog_chip_mark_bad_block((log_end_block() + 20 + i) * ppb());
   t_begin_epoch();
   nandlog_epoch_info_t info;
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   uint32_t retired = log_block_after(info.first_page / ppb());
   while (block_bad(retired))
      retired = log_block_after(retired);
   nandlog_chip_mark_bad_block(retired * ppb());
   nandlog_deinit();
   nandlog_init();
   const uint32_t first_id = next_id, pages = 4 * ppb();
   t_pages(pages);
   t_flush(true);
   nandlog_begin_session();
   const uint32_t epochs = nandlog_epoch_count();
   nandlog_end_session();
   const uint32_t bad_blocks = nandlog_bad_block_count();
   nandlog_deinit();
   nandlog_sim_snapshot();
   const uint32_t saved_next = next_id;

   uint32_t trials = 0, wrong_epoch = 0, wrong_listing = 0, wrong_table = 0, rewrites = 0, wrong_rewrites = 0;
   sweep_tally_t tally;
   memset(&tally, 0, sizeof(tally));
   // The ring, or the table's eight reserve blocks -- or, with those dead, the pair below the reserve it was moved to
   uint32_t table_first = log_end_block(), table_blocks = 8;
   for (uint32_t b = table_search_first_block(); SIM_DEAD_RESERVE_BLOCKS && (b < log_end_block()); b += 2)
      if (holds_table(b) && block_bad(b))
      {
         table_first = b & ~1u;
         table_blocks = 2;
      }
   const uint32_t first_page = target ? (table_first * ppb()) : 0, end_page = first_page + ((target ? table_blocks : 8) * ppb());
   const uint32_t newest_pair = target ? newest_table_pair(table_first, table_blocks) : newest_ring_pair();
   for (uint32_t page = first_page; page < end_page; ++page)
   {
      nandlog_sim_restore();
      next_id = saved_next;
      const uint8_t *raw = nandlog_sim_raw_page(page);
      bool written = false;
      for (uint32_t i = 0; (i < page_bytes()) && !written; ++i)
         written = (raw[i] != 0xFF);
      if (!written)
         continue;
      if (fault)
         nandlog_sim_set_ecc_status(page, (fault == 1) ? NANDLOG_SIM_ECC_UNCORRECTABLE : NANDLOG_SIM_ECC_AT_LIMIT);
      else
      {
         nandlog_sim_corrupt(page, 4, 0xFF);
         nandlog_sim_corrupt(page, 13, 0x3C);
      }
      nandlog_sim_clear_repeated_programs();
      nandlog_sim_reset_counters();
      nandlog_init();
      const bool rewrote = (metadata_writes(table_first, table_blocks) > 0);
      ++trials;
      wrong_epoch += (current_epoch_number() != info.epoch) ? 1 : 0;
      nandlog_begin_session();
      wrong_listing += (nandlog_epoch_count() != epochs) ? 1 : 0;
      nandlog_end_session();
      wrong_table += (nandlog_bad_block_count() != bad_blocks) ? 1 : 0;

      // The boot after writes nothing to the ring or the table, whatever the first wrote
      nandlog_deinit();
      nandlog_sim_reset_counters();
      nandlog_init();
      const bool again = (metadata_writes(table_first, table_blocks) > 0);
      rewrites += rewrote ? 1 : 0;
      wrong_rewrites += (again || ((fault == 2) && (rewrote != ((page & ~1u) == newest_pair)))) ? 1 : 0;
      // A page programmed twice by either boot counts as by the logging the audit does after them, once a trial
      const bool repeated = (repeated_programs_outside_retired_blocks() > 0);
      const uint32_t repeated_before = tally.repeated;
      sweep_audit(pages, first_id, pages, 0xFFFFFFFF, &tally);
      tally.repeated += (repeated && (tally.repeated == repeated_before)) ? 1 : 0;
      nandlog_deinit();
   }
   nandlog_sim_destroy();

   // Fields: target, fault, pages tried, then the reboots that changed the current epoch, the epochs listed or the
   // blocks retired, misplaced the head, lost a committed page, delivered a damaged or duplicated record, or
   // programmed a page twice; then those whose boot wrote the newest slot or table version again, and those after which
   // the next boot wrote again, or -- a page read at the ECC limit -- that wrote again other than for a copy of the newest
   printf("METADATA %u %u %u %u %u %u %u %u %u %u %u %u\n", target, fault, trials, wrong_epoch, wrong_listing, wrong_table, tally.wrong_head,
          tally.lost, tally.damaged, tally.repeated, rewrites, wrong_rewrites);
   return (trials && !wrong_epoch && !wrong_listing && !wrong_table && !tally.wrong_head && !tally.lost && !tally.damaged && !tally.repeated &&
           !wrong_rewrites) ? 0 : 1;
}

static int run_sweep(const char *part)
{
   // Parts that can run side by side: 'faults-<placement>-<fault>-<length>', 'retired-<placement>' and
   // 'metadata-<target>-<fault>'
   uint32_t placement = 0, fault = 0, length_index = 0;
   if ((sscanf(part, "metadata-%u-%u", &placement, &fault) == 2) && (placement < 2) && (fault < 3))
      return run_sweep_metadata(placement, fault);
   if ((sscanf(part, "faults-%u-%u-%u", &placement, &fault, &length_index) == 3) && (placement < 2) && (fault < FAULT_TYPES) && (length_index < SWEEP_LENGTHS))
      return run_sweep_faults(placement, fault, length_index);
   if ((sscanf(part, "retired-%u", &placement) == 1) && (placement < 2))
      return run_sweep_retired(placement);
   fprintf(stderr, "no sweep part named %s\n", part);
   return 2;
}


static int run_one(const char *name, uint32_t m, uint32_t n, uint32_t k, uint32_t later)
{
   // One trial, with the library's diagnostics on: for looking into a violation the campaign reported
   const trace_t *trace = NULL;
   for (uint32_t i = 0; i < NUM_TRACES; ++i)
      if (!strcmp(traces[i].name, name))
         trace = &traces[i];
   if (!trace || (m > 3))
      return 2;
   create_part();
   faults_reset();
   model_reset();
   for (uint32_t i = 0; i < MAX_IDS; ++i)
      id_page[i] = 0xFFFFFFFF;
   trace->prepare();
   nandlog_deinit();
   model_save_base();
   if (!trace->fresh_boot)
      map_record_pages();
   nandlog_sim_snapshot();
   run_to_cut(trace, 0, NANDLOG_SIM_CUT_BOUNDARY, 0.5, 1);
   map_record_pages();
   const double q = q_values[(n + m) % 8];
   const uint64_t seed = 0xC0FFEE00ULL + (n * 131u) + m;
   trial_partial_drift = (n & 1);
   nandlog_sim_set_verbose(true);
   run_to_cut(trace, n, models[m], q, seed);
   nandlog_sim_cut_t cut;
   if (nandlog_sim_last_cut(&cut))
   {
      uint32_t zeros = 0;
      const uint8_t *raw = nandlog_sim_raw_page(cut.page);
      for (uint32_t i = 0; i < (page_bytes() + nandlog_chip_geometry()->spare_size_bytes); ++i)
         for (uint8_t value = (uint8_t)~raw[i]; value; value &= (uint8_t)(value - 1))
            ++zeros;
      printf("cut at operation %u: command 0x%02X page %u (block %u)%s; %u programmed bits there now\n", cut.operation, cut.command,
             cut.page, cut.page / ppb(), cut.otp ? " OTP" : "", zeros);
   }
   if (k)
   {
      nandlog_deinit();
      nandlog_sim_seed(seed ^ (0x5151ULL * k));
      nandlog_sim_power_cut(k, models[m], q_values[(n + m + k) % 8], &cut_jump);
      if (setjmp(cut_jump) == 0)
         nandlog_init();
      nandlog_sim_power_cut_disarm();
      if (nandlog_sim_last_cut(&cut))
         printf("second cut at operation %u: command 0x%02X page %u (block %u)\n", cut.operation, cut.command, cut.page, cut.page / ppb());
   }
   verdict_t v;
   memset(&v, 0, sizeof(v));
   if (later)
   {
      // The 'later'-th entry of the campaign's cuts in the logging after a finished recovering boot
      static const uint32_t later_cuts[4] = { 1, 2, 3, 6 };
      const uint32_t c = (later - 1) % 4;
      nandlog_sim_seed(seed ^ (0xA5A5ULL * later_cuts[c]));
      if (!recover_then_cut(later_cuts[c], models[m], q_values[(n + m + c + 3) % 8], &v))
         printf("the logging never reached operation %u\n", later_cuts[c]);
   }
   else
      v = recover_and_audit();
   uint32_t pages[64];
   const uint32_t total = nandlog_sim_repeated_program_pages(pages, 64);
   for (uint32_t i = 0; (i < total) && (i < 64); ++i)
      printf("repeated program: page %u (block %u, offset %u)%s\n", pages[i], pages[i] / ppb(), pages[i] % ppb(), block_bad(pages[i] / ppb()) ? " retired" : "");
   printf("lost %u damaged %u dup %u misplaced %u seq %u span %u repeated %u retired %u table %u reissued %u; erased-reading pages programmed again %u\n",
          v.lost, v.damaged, v.duplicates, v.misplaced, v.seq_breaks, v.span_errors, v.repeated, v.false_retired, v.table_lost, v.reissued,
          v.blank_reprograms);
   return verdict_clean(&v) ? 0 : 1;
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   if ((argc >= 5) && !strcmp(argv[1], "one"))
      return run_one(argv[2], (uint32_t)atoi(argv[3]), (uint32_t)atoi(argv[4]), (argc >= 6) ? (uint32_t)atoi(argv[5]) : 0,
                     (argc >= 7) ? (uint32_t)atoi(argv[6]) : 0);
   if ((argc >= 3) && !strcmp(argv[1], "campaign"))
      return run_campaign(argv[2], (argc >= 4) ? atoi(argv[3]) : -1);
   if ((argc >= 3) && !strcmp(argv[1], "sweep"))
      return run_sweep(argv[2]);
   if ((argc >= 2) && !strcmp(argv[1], "list"))
   {
      // Every job `make campaign` runs, one per line
      for (uint32_t i = 0; i < NUM_TRACES; ++i)
         for (uint32_t m = 0; m < 4; ++m)
            printf("campaign %s %u\n", traces[i].name, m);
      for (uint32_t placement = 0; placement < 2; ++placement)
      {
         for (uint32_t fault = 0; fault < FAULT_TYPES; ++fault)
            for (uint32_t length = 0; length < SWEEP_LENGTHS; ++length)
               printf("sweep faults-%u-%u-%u\n", placement, fault, length);
         printf("sweep retired-%u\n", placement);
      }
      for (uint32_t target = 0; target < 2; ++target)
         for (uint32_t fault = 0; fault < 3; ++fault)
            printf("sweep metadata-%u-%u\n", target, fault);
      return 0;
   }
   fprintf(stderr, "usage: %s campaign <trace> | sweep <part> | one <trace> <model> <cut> [second cut] | list\n", argv[0]);
   return 2;
}
