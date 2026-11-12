// Regression tests, one or more for each defect found while nandlog was being reviewed and attacked before its
// first release, run against the RAM-backed port. Each test failed against the code in which its defect was found,
// and passes now. D1-D12 are the defects of the first review; later groups are named for who found them.
//
// The suite is built over a part with fewer blocks than the real one (NANDLOG_CHIP_BLOCK_COUNT on the command
// line), so that filling the whole log region -- which several defects need -- takes a fraction of a second.

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nandlog.h"
#include "nandlog_chip.h"
#include "nandlog_port.h"
#include "nandlog_port_sim.h"

#ifdef SIM_CHIP_WINBOND
static const uint8_t DEVICE_ID[3] = { 0xEF, 0xBA, 0x21 };
#define FACTORY_MARKER_OFFSET(page_size)            0
#else
static const uint8_t DEVICE_ID[3] = { 0x8D, 0x00, 0x00 };
#define FACTORY_MARKER_OFFSET(page_size)            (page_size)
#endif

#define METADATA_RING_BLOCKS                        8
#define MAX_IDS                                     40000

// How far down the Alliance driver looks for a bad-block table kept below its reserve, as the build has it
#ifndef NANDLOG_CHIP_TABLE_SEARCH_BLOCKS
#define TABLE_SEARCH_BLOCKS                         (nandlog_chip_geometry()->reserved_blocks + 48)
#else
#define TABLE_SEARCH_BLOCKS                         NANDLOG_CHIP_TABLE_SEARCH_BLOCKS
#endif

static uint32_t tests_run, tests_failed, defects_failed;
static bool this_test_failed;
static uint8_t record[NANDLOG_MAX_PAGE_SIZE_BYTES], readback[NANDLOG_MAX_PAGE_SIZE_BYTES];
static uint8_t seen[MAX_IDS];
static jmp_buf cut_jump;

#define CHECK(cond, ...)  do {                                              \
      ++tests_run;                                                          \
      if (!(cond)) { ++tests_failed; this_test_failed = true; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
   } while (0)


// Geometry and the Part -----------------------------------------------------------------------------------------------

static uint32_t ppb(void)               { return nandlog_chip_geometry()->pages_per_block; }
static uint32_t page_bytes(void)        { return nandlog_chip_geometry()->page_size_bytes; }
static uint32_t log_first_block(void)   { return METADATA_RING_BLOCKS; }
static uint32_t log_end_block(void)     { return nandlog_chip_geometry()->block_count - nandlog_chip_geometry()->reserved_blocks; }
static uint32_t log_blocks(void)        { return log_end_block() - log_first_block(); }

static uint32_t crc32_bits(const uint8_t *bytes, uint32_t length)
{
   // Written here rather than borrowed from the library, so a medium check does not trust the code under test
   uint32_t crc = 0xFFFFFFFF;
   for (uint32_t i = 0; i < length; ++i)
   {
      crc ^= bytes[i];
      for (uint32_t bit = 0; bit < 8; ++bit)
         crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
   }
   return ~crc;
}

static bool raw_page_valid(uint32_t page, nandlog_page_header_t *header, bool check_payload)
{
   // A page as the medium holds it, judged independently of the library
   const uint8_t *raw = nandlog_sim_raw_page(page);
   memcpy(header, raw, sizeof(*header));
   if ((header->magic != NANDLOG_PAGE_MAGIC) && (header->magic != NANDLOG_PAGE_MAGIC_FRAMED))
      return false;
   if (header->header_crc != crc32_bits(raw, NANDLOG_PAGE_HEADER_CRC_BYTES))
      return false;
   if (header->payload_length > (page_bytes() - sizeof(*header)))
      return false;
   return !check_payload || (header->payload_crc == crc32_bits(raw + sizeof(*header), header->payload_length));
}

static bool raw_page_erased(uint32_t page)
{
   const uint8_t *raw = nandlog_sim_raw_page(page);
   for (uint32_t i = 0; i < page_bytes(); ++i)
      if (raw[i] != 0xFF)
         return false;
   return true;
}

static void set_faults(nandlog_sim_faults_t *faults)
{
   memcpy(faults->device_id, DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_sim_set_faults(faults);
}

static void clear_faults(void)
{
   nandlog_sim_faults_t faults;
   memset(&faults, 0, sizeof(faults));
   set_faults(&faults);
}

static nandlog_sim_faults_t no_faults(void)
{
   nandlog_sim_faults_t faults;
   memset(&faults, 0, sizeof(faults));
   return faults;
}

static void factory_mark(uint32_t block)
{
   uint8_t *raw = nandlog_sim_raw_page(block * ppb());
   raw[FACTORY_MARKER_OFFSET(page_bytes())] = 0x00;
}

static void reboot(void)
{
   nandlog_deinit();
   if (!nandlog_init())
   {
      printf("    FATAL: nandlog_init() failed\n");
      exit(1);
   }
}

static void new_part(void)
{
   nandlog_deinit();
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
}

static bool begin_epoch(const char *name)
{
   uint8_t details[16] = { 0 };
   strncpy((char*)details, name, sizeof(details) - 1);
   nandlog_begin_session();
   const bool begun = nandlog_begin_epoch(details, sizeof(details));
   nandlog_end_session();
   return begun;
}

static void fresh_epoch(void)
{
   new_part();
   reboot();
   begin_epoch("regress");
}

static uint32_t current_epoch(void)
{
   nandlog_epoch_info_t info;
   memset(&info, 0, sizeof(info));
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   return info.epoch;
}

static uint32_t epoch_start_block(void)
{
   nandlog_epoch_info_t info;
   memset(&info, 0, sizeof(info));
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   return info.first_page / ppb();
}


// Records -------------------------------------------------------------------------------------------------------------

static uint32_t prefix_bytes(void)       { return NANDLOG_RECORD_FRAMING ? NANDLOG_FRAMING_LENGTH_BYTES : 0; }
static uint32_t page_record_bytes(void)  { return nandlog_data_bytes_per_page() - prefix_bytes() - 5; }

static void fill_record(uint8_t *buffer, uint32_t length, uint32_t id)
{
   // Content that only one id can produce, so a record that comes back damaged or mixed up is caught
   uint64_t x = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)id << 20);
   for (uint32_t i = 0; i < length; ++i)
   {
      x ^= x >> 12;
      x ^= x << 25;
      x ^= x >> 27;
      buffer[i] = (uint8_t)((x * 0x2545F4914F6CDD1DULL) >> 56);
   }
   if (length >= 4)
      memcpy(buffer, &id, sizeof(id));
}

static int store(uint32_t id, uint32_t length)
{
   fill_record(record, length, id);
   return (int)nandlog_store_record(7, 1000 + id, record, length);
}

static int flush(bool partial)
{
   return (int)nandlog_flush(partial);
}

static void write_pages(uint32_t count, uint32_t first_id)
{
   // One record that fills a page exactly, so every store after the first commits the one before
   for (uint32_t i = 0; i < count; ++i)
      store(first_id + i, page_record_bytes());
   flush(true);
}

typedef struct
{
   uint32_t positions, bytes_declared, delivered, delivered_bytes, gaps, damaged, duplicates, seq_breaks, foreign;
} audit_t;

static void note_records(const uint8_t *payload, uint32_t length, uint32_t record_count, audit_t *audit)
{
   // Walk a delivered payload and check every record in it against its id. Without framing a record's extent is
   // the application's business; the tests here put records of one size in a page, so the count settles it
   uint32_t offset = 0;
   while (offset < length)
   {
      uint32_t data_length = (record_count ? (length / record_count) : length) - prefix_bytes() - 5;
      if (NANDLOG_RECORD_FRAMING)
      {
         uint16_t framed = 0;
         memcpy(&framed, payload + offset, sizeof(framed));
         data_length = framed;
      }
      const uint32_t total = prefix_bytes() + 5 + data_length;
      if ((offset + total) > length)
      {
         ++audit->damaged;
         return;
      }
      const uint8_t *data = payload + offset + prefix_bytes() + 5;
      uint32_t id = 0;
      if (data_length >= 4)
         memcpy(&id, data, sizeof(id));
      fill_record(record, data_length, id);
      if ((data_length < 4) || (id >= MAX_IDS) || memcmp(record, data, data_length))
         ++audit->damaged;
      else
      {
         audit->duplicates += seen[id] ? 1 : 0;
         seen[id] = 1;
      }
      offset += total;
   }
}

static audit_t audit_view(bool clear_seen)
{
   // Read whatever epoch is selected from end to end through the public API, checking everything that comes back
   audit_t audit;
   memset(&audit, 0, sizeof(audit));
   if (clear_seen)
      memset(seen, 0, sizeof(seen));
   nandlog_begin_reading(0, 0);
   nandlog_read_span(&audit.positions, &audit.bytes_declared);
   for (uint32_t i = 0; i < audit.positions; ++i)
   {
      nandlog_page_header_t header;
      const uint32_t length = nandlog_retrieve_next_page(readback, &header);
      if (header.seq != i)
         ++audit.seq_breaks;
      if (!length)
      {
         ++audit.gaps;
         continue;
      }
      ++audit.delivered;
      audit.delivered_bytes += length;
      note_records(readback, length, header.record_count, &audit);
   }
   nandlog_end_reading();
   return audit;
}

static audit_t audit_current(void)
{
   nandlog_begin_session();
   const audit_t audit = audit_view(true);
   nandlog_end_session();
   return audit;
}

static uint32_t ids_seen(uint32_t first_id, uint32_t count)
{
   uint32_t found = 0;
   for (uint32_t i = 0; i < count; ++i)
      found += seen[first_id + i] ? 1 : 0;
   return found;
}

static uint32_t repeated_programs_outside_retired_blocks(void)
{
   // A driver may deliberately write a marker into a block it has just retired; anywhere else, a second program
   // of a page is a defect
   uint32_t pages[64];
   const uint32_t total = nandlog_sim_repeated_program_pages(pages, 64), held = (total < 64) ? total : 64;
   uint32_t defects = 0;
   for (uint32_t i = 0; i < held; ++i)
      if (!nandlog_chip_is_bad_block(pages[i]))
         ++defects;
   return defects;
}

static uint32_t blank_reprograms_outside_retired_blocks(void)
{
   // The same for pages programmed again across a reboot that read erased in every bit: the driver's mark into a
   // retired block whose program the part refused is tried again at a later boot, and is not a defect
   uint32_t pages[64];
   const uint32_t total = nandlog_sim_blank_reprogram_pages(pages, 64), held = (total < 64) ? total : 64;
   uint32_t defects = (total > 64) ? (total - 64) : 0;
   for (uint32_t i = 0; i < held; ++i)
      if (!nandlog_chip_is_bad_block(pages[i]))
         ++defects;
   return defects;
}

static uint32_t retired_on_purpose_first, retired_on_purpose_end;      // blocks a test retired itself, through the driver

static void begin_test(const char *defect, const char *description)
{
   this_test_failed = false;
   retired_on_purpose_first = retired_on_purpose_end = 0;
   printf("  %-6s %s\n", defect, description);
}

static void end_test(void)
{
   defects_failed += this_test_failed ? 1 : 0;
   clear_faults();
   nandlog_sim_power_cut_disarm();
}


// D1: Write-Head Recovery ---------------------------------------------------------------------------------------------

static void test_d1_invalid_header_anywhere(bool uncorrectable)
{
   // Every position of a 130-page epoch in turn is made unreadable -- its magic rotted, or its ECC verdict made
   // uncorrectable -- and the part rebooted. The head must come back exactly where it was, so the other pages are
   // all still delivered and the next ones land after them
   begin_test("D1", uncorrectable ? "an uncorrectable page anywhere in the epoch does not move the write head"
                                  : "an invalid header anywhere in the epoch does not move the write head");
   const uint32_t pages = 130;
   fresh_epoch();
   const uint32_t start = epoch_start_block() * ppb();
   write_pages(pages, 0);
   nandlog_deinit();
   nandlog_sim_snapshot();
   uint32_t wrong = 0, first_wrong = 0;
   for (uint32_t p = 0; p < pages; ++p)
   {
      nandlog_sim_restore();
      if (uncorrectable)
         nandlog_sim_set_ecc_status(start + p, NANDLOG_SIM_ECC_UNCORRECTABLE);
      else
         nandlog_sim_corrupt(start + p, 0, 0xFF);
      reboot();
      audit_t a = audit_current();
      write_pages(5, 1000);
      const audit_t b = audit_current();
      const bool ok = (a.positions == pages) && (a.delivered == (pages - 1)) && (b.delivered == (pages + 4)) &&
                      (ids_seen(0, pages) == (pages - 1)) && (ids_seen(1000, 5) == 5) && !b.damaged;
      if (!ok && !wrong++)
         first_wrong = p;
   }
   CHECK(!wrong, "%u of %u positions misplaced the write head or lost pages (first at position %u)", wrong, pages, first_wrong);
   end_test();
}

static void test_d1_retired_block_in_epoch(void)
{
   // A factory-marked block in the middle of the epoch, and the head at every page of the four blocks after it.
   // Retired blocks are skipped on purpose; they must never look like the end of the log
   begin_test("D1", "a retired block inside the epoch never truncates recovery");
   new_part();
   factory_mark(log_first_block() + 4);
   nandlog_sim_snapshot();
   uint32_t wrong = 0, trials = 0, first_wrong = 0;
   for (uint32_t pages = 2 * ppb(); pages <= (10 * ppb()); pages += 3, ++trials)
   {
      nandlog_sim_restore();
      reboot();
      begin_epoch("d1");
      write_pages(pages, 0);
      reboot();
      const audit_t a = audit_current();
      if (((a.delivered != pages) || (ids_seen(0, pages) != pages)) && !wrong++)
         first_wrong = pages;
   }
   CHECK(!wrong, "%u of %u reboots lost committed pages (first with %u pages)", wrong, trials, first_wrong);
   end_test();
}


// D2: A Torn Page at the Head -----------------------------------------------------------------------------------------

static void test_d2_torn_head(double q, uint32_t ecc_hides_bits, const char *description)
{
   // A cut tears the program of the next page. After a reboot nothing may be programmed twice and no good block
   // retired, and every page committed before and after must be delivered
   begin_test("D2", description);
   fresh_epoch();
   write_pages(20, 0);
   const uint32_t bad_before = nandlog_bad_block_count();
   nandlog_sim_faults_t faults = no_faults();
   faults.ecc_hides_bits = ecc_hides_bits;
   faults.torn_reads_uncorrectable = true;
   set_faults(&faults);
   nandlog_sim_seed(7);
   store(100, page_record_bytes());
   nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_BITWISE, q, &cut_jump);
   if (setjmp(cut_jump) == 0)
      store(101, page_record_bytes());      // commits page 100's record, and the cut lands in that program
   nandlog_sim_power_cut_disarm();
   CHECK(nandlog_sim_power_failed(), "the cut never fired");
   nandlog_sim_clear_repeated_programs();
   reboot();
   write_pages(5, 200);
   reboot();
   const audit_t a = audit_current();
   CHECK(!repeated_programs_outside_retired_blocks(), "a page was programmed twice after the torn program");
   CHECK(nandlog_bad_block_count() == bad_before, "a good block was retired (%u -> %u)", bad_before, nandlog_bad_block_count());
   CHECK((ids_seen(0, 20) == 20) && (ids_seen(200, 5) == 5) && !a.damaged, "committed pages lost or damaged (%u/20, %u/5, %u damaged)",
         ids_seen(0, 20), ids_seen(200, 5), a.damaged);
   end_test();
}


// D3, D4 and Reviewer-3 (b): The Full Region and the Wrap -------------------------------------------------------------

static uint32_t fill_region(uint32_t first_id)
{
   // Commit page after page until the log will take no more, and say how many it took
   uint32_t pages = 0;
   const uint32_t limit = log_blocks() * ppb() + 1;
   for (uint32_t i = 0; i < limit; ++i)
   {
      if (store(first_id + i, page_record_bytes()) != NANDLOG_OK)
         break;
      ++pages;
   }
   nandlog_stats_t before, after;
   nandlog_get_stats(&before);
   (void)before;
   flush(true);
   nandlog_get_stats(&after);
   return pages ? (pages - (after.records_lost ? 1 : 0)) : 0;
}

static void test_d3_full_region_boot(void)
{
   // An epoch that fills the region, then warm boots. A boot must erase nothing of it, and a read must cover it all
   begin_test("D3", "a warm boot of a full epoch erases nothing and reads it all");
   fresh_epoch();
   const uint32_t capacity = (log_blocks() - nandlog_bad_block_count()) * ppb();
   const uint32_t committed = fill_region(0);
   CHECK(committed == capacity, "the region took %u pages, expected %u", committed, capacity);
   for (uint32_t boot = 0; boot < 3; ++boot)
   {
      nandlog_sim_reset_counters();
      reboot();
      CHECK(nandlog_sim_counters().block_erases == 0, "boot %u issued %u erases on a full epoch", boot, nandlog_sim_counters().block_erases);
   }
   const audit_t a = audit_current();
   CHECK((a.positions == capacity) && (a.delivered == capacity) && (ids_seen(0, capacity) == capacity),
         "after the boots the read declares %u and delivers %u of %u pages", a.positions, a.delivered, capacity);
   CHECK(store(90000, 8) == NANDLOG_ERR_FULL, "a record offered to a full region was not refused as full");
   end_test();
}

static void test_d4_failure_at_the_wrap(void)
{
   // The block just behind the epoch's first block refuses programs once the head is four pages into it.
   // Relocation must not take the epoch's own first block; the region is simply full
   begin_test("D4", "a block failing just before the wrap never erases the epoch's start");
   fresh_epoch();
   const uint32_t start_block = epoch_start_block();
   const uint32_t capacity = log_blocks() * ppb();
   write_pages(capacity - ppb() + 4, 0);
   const uint32_t last_block = (start_block == log_first_block()) ? (log_end_block() - 1) : (start_block - 1);
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_blocks[0] = last_block;
   faults.num_unwritable_blocks = 1;
   set_faults(&faults);
   write_pages(100, 100000);
   clear_faults();
   reboot();
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   uint32_t first_block_pages = 0;
   for (uint32_t seq = 0; seq < ppb(); ++seq)
   {
      nandlog_page_header_t header;
      first_block_pages += nandlog_retrieve_page_by_seq(seq, readback, &header) ? 1 : 0;
   }
   nandlog_end_reading();
   nandlog_end_session();
   const audit_t a = audit_current();
   CHECK(first_block_pages == ppb(), "only %u of the epoch's first %u pages survive", first_block_pages, ppb());
   CHECK(ids_seen(0, capacity - ppb() + 4) == (capacity - ppb() + 4), "%u of %u pages committed before the failure survive",
         ids_seen(0, capacity - ppb() + 4), capacity - ppb() + 4);
   CHECK(a.delivered <= capacity, "the epoch delivers %u pages, more than the %u the region can hold", a.delivered, capacity);
   CHECK(!a.damaged && !a.duplicates, "%u damaged and %u duplicated records", a.damaged, a.duplicates);
   end_test();
}

static void test_r3b_first_block_retired_then_full(void)
{
   // The epoch's first block fails at its fourth page, so it is retired and its pages move on; then the region
   // fills. Nothing may compare against the retired first block and miss, letting the window wrap into the epoch
   begin_test("R3b", "with the epoch's first block retired, filling the region erases nothing of the epoch");
   fresh_epoch();
   const uint32_t start_block = epoch_start_block();
   write_pages(3, 0);
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_blocks[0] = start_block;
   faults.num_unwritable_blocks = 1;
   set_faults(&faults);
   write_pages(1, 3);
   clear_faults();
   CHECK(nandlog_chip_is_bad_block(start_block * ppb()), "the failing first block was not retired");
   const uint32_t expected = 4 + fill_region(4);
   reboot();
   const audit_t a = audit_current();
   CHECK((ids_seen(0, 4) == 4) && (ids_seen(4, expected - 4) == (expected - 4)),
         "of %u committed pages %u were delivered (%u of the first four)", expected, a.delivered, ids_seen(0, 4));
   CHECK(!a.damaged && !a.duplicates, "%u damaged and %u duplicated records", a.damaged, a.duplicates);
   end_test();
}


// D5: Relocation ------------------------------------------------------------------------------------------------------

static void relocation_setup(uint32_t pages, nandlog_sim_faults_t *faults)
{
   // Fill 'pages' of the epoch's first block, then make that block refuse programs; the next commit relocates
   write_pages(pages, 0);
   faults->unwritable_blocks[faults->num_unwritable_blocks++] = epoch_start_block();
   set_faults(faults);
}

static void test_d5_refused_copy(void)
{
   begin_test("D5", "a relocation target that refuses one copy is not trusted with the rest");
   nandlog_sim_faults_t faults = no_faults();
   fresh_epoch();
   const uint32_t target = (epoch_start_block() + 1) * ppb();
   faults.unwritable_pages[0] = target + 3;
   faults.num_unwritable_pages = 1;
   relocation_setup(11, &faults);
   nandlog_sim_clear_repeated_programs();
   write_pages(1, 11);
   const uint32_t repeats = repeated_programs_outside_retired_blocks();
   clear_faults();
   reboot();
   const audit_t a = audit_current();
   CHECK((ids_seen(0, 12) == 12) && !a.damaged, "%u of 12 committed pages survive the relocation", ids_seen(0, 12));
   CHECK(!repeats, "the refused copy was programmed again into the same page");
   end_test();
}

static void test_d5_target_refuses_erase(void)
{
   begin_test("D5", "a relocation target that refuses its erase is retired and never written");
   nandlog_sim_faults_t faults = no_faults();
   fresh_epoch();
   faults.unerasable_blocks[0] = epoch_start_block() + 1;
   faults.num_unerasable_blocks = 1;
   relocation_setup(11, &faults);
   write_pages(1, 11);
   clear_faults();
   reboot();
   const audit_t a = audit_current();
   CHECK((ids_seen(0, 12) == 12) && !a.damaged, "%u of 12 committed pages survive", ids_seen(0, 12));
   end_test();
}

static void test_d5_unreadable_source(void)
{
   // The epoch's second block fails with an unreadable page in it. That page has to arrive in the new block as a
   // tombstone -- a valid header with its sequence number -- and never as something that might read as erased
   begin_test("D5", "an unreadable page is relocated as a tombstone, not as erased-looking cells");
   fresh_epoch();
   const uint32_t start_block = epoch_start_block();
   write_pages(ppb() + 30, 0);
   nandlog_sim_set_ecc_status((start_block + 1) * ppb() + 20, NANDLOG_SIM_ECC_UNCORRECTABLE);
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_blocks[0] = start_block + 1;
   faults.num_unwritable_blocks = 1;
   set_faults(&faults);
   write_pages(1, 5000);
   clear_faults();
   const uint32_t destination = (start_block + 2) * ppb() + 20;
   nandlog_page_header_t header;
   const bool tombstone = raw_page_valid(destination, &header, true) && (header.payload_length == 0) && (header.seq == ppb() + 20);
   CHECK(tombstone, "position %u's copy is not a tombstone (erased: %s)", ppb() + 20, raw_page_erased(destination) ? "yes" : "no");
   reboot();
   const audit_t a = audit_current();
   CHECK((a.positions == (ppb() + 31)) && (a.delivered == (ppb() + 30)), "the read declares %u and delivers %u, expected %u and %u",
         a.positions, a.delivered, ppb() + 31, ppb() + 30);
   end_test();
}


// D6: The Drivers -----------------------------------------------------------------------------------------------------

static void test_d6_no_second_program(void)
{
   begin_test("D6", "a page that reported a program failure is never programmed again");
   nandlog_sim_faults_t faults = no_faults();
   fresh_epoch();
   relocation_setup(10, &faults);
   nandlog_sim_clear_repeated_programs();
   write_pages(1, 10);
   uint32_t pages[64];
   const uint32_t total = nandlog_sim_repeated_program_pages(pages, 64);
   const uint32_t failing_page = epoch_start_block() * ppb() + 10;
   uint32_t on_failing_page = 0;
   for (uint32_t i = 0; (i < total) && (i < 64); ++i)
      on_failing_page += (pages[i] == failing_page) ? 1 : 0;
   CHECK(!on_failing_page, "the page that failed took %u further PROGRAM EXECUTEs", on_failing_page);
   end_test();
}

static void test_d6_ecc_limit_is_readable(void)
{
   // ECC status 11b is "corrected at the limit" on the Alliance part and "uncorrectable" on the Winbond part
#ifdef SIM_CHIP_WINBOND
   begin_test("D6", "ECC status 11b reads as uncorrectable on this part, as its datasheet has it");
#else
   begin_test("D6", "a page corrected at the ECC limit is delivered, not served as a gap");
#endif
   fresh_epoch();
   write_pages(10, 0);
   nandlog_sim_set_ecc_status(epoch_start_block() * ppb() + 4, NANDLOG_SIM_ECC_AT_LIMIT);
   const audit_t a = audit_current();
#ifdef SIM_CHIP_WINBOND
   CHECK((a.delivered == 9) && (a.gaps == 1), "delivered %u, %u gaps", a.delivered, a.gaps);
#else
   CHECK((a.delivered == 10) && !a.gaps, "delivered %u, %u gaps", a.delivered, a.gaps);
   nandlog_stats_t stats;
   nandlog_get_stats(&stats);
   CHECK(stats.reads_at_ecc_limit > 0, "the read at the ECC limit was not counted");
#endif
   end_test();
}


// D7: The Bad-Block Table ---------------------------------------------------------------------------------------------

static void test_d7_cut_in_table_update(void)
{
   // A factory-bad block, then a retirement at runtime, cut at every program and erase it issues under three
   // models. After the reboot, the factory-bad block must still be known; so must the new one, once it completed
   begin_test("D7", "a cut anywhere in a bad-block table update forgets nothing");
   new_part();
   factory_mark(3);
   reboot();
   begin_epoch("d7");
   write_pages(10, 0);
   const uint32_t failing = epoch_start_block();
   nandlog_deinit();
   nandlog_sim_snapshot();

   // How many operations the retirement and its relocation take, uncut
   reboot();
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_blocks[0] = failing;
   faults.num_unwritable_blocks = 1;
   set_faults(&faults);
   nandlog_sim_reset_operations();
   write_pages(1, 10);
   const uint32_t span = nandlog_sim_operations();

   uint32_t forgotten = 0, trials = 0;
   const nandlog_sim_cut_model_t models[3] = { NANDLOG_SIM_CUT_BOUNDARY, NANDLOG_SIM_CUT_PREFIX, NANDLOG_SIM_CUT_BITWISE };
   for (uint32_t m = 0; m < 3; ++m)
      for (uint32_t n = 1; n <= span; ++n, ++trials)
      {
         nandlog_deinit();
         nandlog_sim_restore();
         reboot();
         set_faults(&faults);
         nandlog_sim_seed(n);
         nandlog_sim_power_cut(n, models[m], 0.5, &cut_jump);
         if (setjmp(cut_jump) == 0)
            write_pages(1, 10);
         nandlog_sim_power_cut_disarm();
         clear_faults();
         reboot();
         if (!nandlog_chip_is_bad_block(3 * ppb()))
            ++forgotten;
      }
   CHECK(!forgotten, "%u of %u cuts left the factory-bad block forgotten", forgotten, trials);
   end_test();
}

static void test_d7_cut_in_first_boot(void)
{
   begin_test("D7", "a cut anywhere in the first boot forgets no factory-bad block");
   new_part();
   factory_mark(3);
   nandlog_sim_snapshot();
   nandlog_sim_reset_operations();
   reboot();
   const uint32_t span = nandlog_sim_operations();
   uint32_t forgotten = 0;
   for (uint32_t n = 1; n <= span; ++n)
   {
      nandlog_deinit();
      nandlog_sim_restore();
      nandlog_sim_power_cut(n, NANDLOG_SIM_CUT_BOUNDARY, 0.5, &cut_jump);
      if (setjmp(cut_jump) == 0)
         nandlog_init();
      nandlog_sim_power_cut_disarm();
      reboot();
      forgotten += nandlog_chip_is_bad_block(3 * ppb()) ? 0 : 1;
   }
   CHECK(span && !forgotten, "%u of %u cuts in the first boot left the factory-bad block forgotten", forgotten, span);
   end_test();
}

#ifndef SIM_CHIP_WINBOND
static void test_d7_large_table_kept(void)
{
   // More retired blocks than the datasheet allows to be bad, 80: unusual, but the table must be kept and reported,
   // not dropped
   begin_test("D7", "a table with more entries than the datasheet allows bad blocks is kept");
   new_part();
   reboot();
   const uint32_t many = 81;
   for (uint32_t i = 0; i < many; ++i)
      nandlog_chip_mark_bad_block((log_first_block() + 10 + i) * ppb());
   reboot();
   uint32_t kept = 0;
   for (uint32_t i = 0; i < many; ++i)
      kept += nandlog_chip_is_bad_block((log_first_block() + 10 + i) * ppb()) ? 1 : 0;
   CHECK(kept == many, "%u of %u retired blocks survived a reboot", kept, many);
   end_test();
}
#endif


// D8: Read Accounting -------------------------------------------------------------------------------------------------

static void test_d8_retired_positions(void)
{
   begin_test("D8", "a read over a retired block declares good positions only, with exact bytes");
   new_part();
   factory_mark(log_first_block() + 2);
   reboot();
   begin_epoch("d8");
   const uint32_t pages = 4 * ppb();
   write_pages(pages, 0);
   const audit_t a = audit_current();
   CHECK((a.positions == pages) && !a.gaps && !a.seq_breaks, "declares %u positions with %u gaps and %u sequence breaks for %u pages",
         a.positions, a.gaps, a.seq_breaks, pages);
   CHECK(a.bytes_declared == a.delivered_bytes, "declared %u bytes, delivered %u", a.bytes_declared, a.delivered_bytes);
   end_test();
}

static void test_d8_earlier_epoch(void)
{
   begin_test("D8", "an earlier epoch reads exactly its own pages, ending at its last");
   fresh_epoch();
   write_pages(11, 0);
   begin_epoch("second");
   write_pages(3, 100);
   nandlog_epoch_info_t older;
   nandlog_begin_session();
   nandlog_epoch_info(1, &older);
   CHECK(nandlog_select_epoch(older.epoch), "could not select the earlier epoch");
   memset(seen, 0, sizeof(seen));
   const audit_t a = audit_view(true);
   nandlog_end_session();
   CHECK((a.positions == 11) && (a.delivered == 11) && !a.gaps, "declares %u, delivers %u with %u gaps; expected 11, 11, 0",
         a.positions, a.delivered, a.gaps);
   CHECK(!ids_seen(100, 3), "a page of the next epoch was served as part of the earlier one");
   CHECK(a.bytes_declared == a.delivered_bytes, "declared %u bytes, delivered %u", a.bytes_declared, a.delivered_bytes);
   end_test();
}


// D9 and Reviewer-3 (a): Epochs ---------------------------------------------------------------------------------------

static void test_d9_no_logging_before_an_epoch(void)
{
   begin_test("D9", "logging before the first epoch programs nothing and is refused");
   new_part();
   reboot();
   nandlog_sim_reset_counters();
   nandlog_sim_clear_repeated_programs();
   write_pages(21, 0);
   reboot();
   write_pages(6, 100);
   CHECK(!nandlog_sim_counters().repeated_programs, "%u pages were programmed twice", nandlog_sim_counters().repeated_programs);
   CHECK(!nandlog_bad_block_count(), "%u good blocks were retired", nandlog_bad_block_count());
   CHECK(store(500, 8) == NANDLOG_ERR_NO_EPOCH, "a record with no epoch begun was not refused as such");
   end_test();
}

static void test_r3a_epoch_start_refuses_erase(void)
{
   begin_test("R3a", "an epoch whose first block refuses its erase never puts a page in it");
   new_part();
   reboot();

   // The blocks an epoch could begin in refuse their erase only from the moment it begins: a block that had
   // already failed at boot would simply have been retired then
   nandlog_sim_faults_t faults = no_faults();
   faults.unerasable_blocks[0] = log_first_block();
   faults.unerasable_blocks[1] = log_first_block() + 1;
   faults.num_unerasable_blocks = 2;
   set_faults(&faults);
   begin_epoch("r3a");
   write_pages(10, 0);
   clear_faults();
   reboot();
   const audit_t a = audit_current();
   CHECK((a.delivered == 10) && (ids_seen(0, 10) == 10), "delivered %u of 10 pages", a.delivered);
   end_test();
}

static void test_r3a_begin_epoch_keeps_buffer(void)
{
   begin_test("R3a", "beginning an epoch commits what was buffered for the last one");
   fresh_epoch();
   write_pages(2, 0);
   for (uint32_t i = 0; i < 3; ++i)
      store(10 + i, 40);
   begin_epoch("next");
   nandlog_epoch_info_t older;
   nandlog_begin_session();
   nandlog_epoch_info(1, &older);
   nandlog_select_epoch(older.epoch);
   audit_view(true);
   nandlog_end_session();
   CHECK(ids_seen(10, 3) == 3, "%u of 3 buffered records reached the epoch they were logged in", ids_seen(10, 3));
   end_test();
}


// D10: Erase-Ahead ----------------------------------------------------------------------------------------------------

static void test_d10_erase_once_outside_commits(void)
{
   // Small records, so most calls commit nothing. Each block the head enters should be erased exactly once, and
   // no call should both program a page and erase a block
   begin_test("D10", "each block is erased once, never in the call that commits a page");
   fresh_epoch();
   const uint32_t start_block = epoch_start_block();
   const uint32_t per_page = nandlog_data_bytes_per_page() / (prefix_bytes() + 5 + 100);
   const uint32_t records = per_page * 20 * ppb();
   uint32_t both = 0, erases = 0;
   for (uint32_t i = 0; i < records; ++i)
   {
      nandlog_sim_reset_counters();
      store(i % MAX_IDS, 100);
      const nandlog_sim_counters_t c = nandlog_sim_counters();
      both += (c.page_writes && c.block_erases) ? 1 : 0;
      erases += c.block_erases;
   }
   flush(true);
   const uint32_t blocks_entered = 20;
   CHECK(!both, "%u calls both committed a page and erased a block", both);
   CHECK(erases <= (blocks_entered + 1), "%u erases for the %u blocks the head entered after block %u", erases, blocks_entered, start_block);
   end_test();
}


// D11: Reporting What Was Dropped -------------------------------------------------------------------------------------

static void test_d11_statuses(void)
{
   begin_test("D11", "every refused record says why, and is counted");
   new_part();
   reboot();
   CHECK(store(1, 8) == NANDLOG_ERR_NO_EPOCH, "no epoch: wrong status");
   begin_epoch("d11");
   CHECK(store(2, 8) == NANDLOG_OK, "an ordinary record was refused");
   CHECK(store(3, nandlog_data_bytes_per_page()) == NANDLOG_ERR_TOO_LARGE, "an oversized record: wrong status");
   nandlog_disable(true);
   CHECK(store(4, 8) == NANDLOG_ERR_DISABLED, "a disabled log: wrong status");
   nandlog_disable(false);
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   CHECK(store(5, 8) == NANDLOG_OK, "a record buffered during a read was refused");
   CHECK(store(6, page_record_bytes()) == NANDLOG_ERR_READING, "a commit during a read: wrong status");
   nandlog_end_reading();
   nandlog_end_session();
   nandlog_stats_t stats;
   nandlog_get_stats(&stats);
   CHECK((stats.records_refused == 4) && (stats.records_stored == 2), "counted %u refused and %u stored, expected 4 and 2",
         stats.records_refused, stats.records_stored);

   // A part that will take no page at all: the records already buffered are reported lost, and so is the log
   nandlog_sim_faults_t faults = no_faults();
   for (uint32_t i = 0; i < NANDLOG_SIM_MAX_FAULT_BLOCKS; ++i)
      faults.unwritable_blocks[i] = log_first_block() + i;
   faults.num_unwritable_blocks = NANDLOG_SIM_MAX_FAULT_BLOCKS;
   set_faults(&faults);
   CHECK(flush(true) == NANDLOG_ERR_UNWRITABLE, "a commit no block would take: wrong status");
   nandlog_get_stats(&stats);
   CHECK(stats.records_lost == 1, "%u records counted lost, expected 1", stats.records_lost);
   CHECK(store(7, 8) == NANDLOG_ERR_DISABLED, "after an unwritable commit the log is not disabled");
   end_test();
}

static void test_d11_payload_verified(void)
{
   // A cell in the payload that will not program, while the program reports success. Only a read-back that checks
   // the payload notices; one that checks the header alone keeps the page and loses its records
   begin_test("D11", "the commit read-back catches a payload that did not program");
   fresh_epoch();
   write_pages(5, 0);
   nandlog_sim_faults_t faults = no_faults();
   faults.has_stuck_cells = true;
   faults.stuck_page = epoch_start_block() * ppb() + 5;
   faults.stuck_offset = sizeof(nandlog_page_header_t) + 100;
   faults.stuck_mask = 0xFF;
   set_faults(&faults);
   write_pages(1, 5);
   clear_faults();
   const audit_t a = audit_current();
   CHECK((ids_seen(0, 6) == 6) && !a.damaged, "%u of 6 committed pages are deliverable", ids_seen(0, 6));
   end_test();
}


static void test_unwritable_is_sticky(void)
{
   // A commit that runs out of blocks leaves the head on a page whose program failed, which may read erased.
   // Nothing may be written past it until a reboot, even if the caller re-enables logging, or a blank position
   // could end up below the head
   begin_test("-", "after the part stops retaining pages, nothing is written until a reboot");
   fresh_epoch();
   write_pages(10, 0);
   nandlog_sim_faults_t faults = no_faults();
   for (uint32_t i = 0; i < 6; ++i)
      faults.unwritable_blocks[faults.num_unwritable_blocks++] = epoch_start_block() + i;
   set_faults(&faults);
   store(10, page_record_bytes());
   CHECK(flush(true) == NANDLOG_ERR_UNWRITABLE, "the commit did not fail as unwritable");
   clear_faults();
   nandlog_disable(false);
   CHECK(store(11, page_record_bytes()) == NANDLOG_ERR_DISABLED, "a record was taken after the part stopped retaining pages");
   reboot();
   write_pages(5, 20);
   reboot();
   const audit_t a = audit_current();
   CHECK((ids_seen(0, 10) == 10) && (ids_seen(20, 5) == 5) && !a.damaged && !a.seq_breaks, "after the reboot %u of 10 and %u of 5 pages delivered",
         ids_seen(0, 10), ids_seen(20, 5));
   end_test();
}


static void put_page_as(uint32_t page, uint32_t magic, uint32_t epoch, uint32_t seq, uint32_t id)
{
   // A page laid down byte for byte the way the library writes one: one page-filling record, both CRCs
   uint8_t *raw = nandlog_sim_raw_page(page);
   nandlog_page_header_t header;
   memset(&header, 0, sizeof(header));
   const uint32_t length = page_record_bytes();
   uint8_t *payload = raw + sizeof(header);
   if (NANDLOG_RECORD_FRAMING)
   {
      const uint16_t framed = (uint16_t)length;
      memcpy(payload, &framed, sizeof(framed));
   }
   payload[prefix_bytes()] = 7;
   const uint32_t timestamp = 1000 + id;
   memcpy(payload + prefix_bytes() + 1, &timestamp, sizeof(timestamp));
   fill_record(payload + prefix_bytes() + 5, length, id);
   header.magic = magic;
   header.epoch = epoch;
   header.seq = seq;
   header.first_timestamp = header.last_timestamp = timestamp;
   header.payload_length = (uint16_t)(prefix_bytes() + 5 + length);
   header.record_count = 1;
   header.payload_crc = crc32_bits(payload, header.payload_length);
   header.header_crc = crc32_bits((const uint8_t*)&header, NANDLOG_PAGE_HEADER_CRC_BYTES);
   memcpy(raw, &header, sizeof(header));
}


// D12 and Boot Cost ---------------------------------------------------------------------------------------------------

static void test_d12_accessors_locked(void)
{
   begin_test("D12", "every public accessor takes the library's lock");
   fresh_epoch();
   nandlog_sim_reset_counters();
   nandlog_disable(false);
   nandlog_has_buffered_data();
   nandlog_bad_block_count();
   nandlog_retransmit_count();
   nandlog_retransmit_clear();
   CHECK(nandlog_sim_lock_count() == 5, "5 accessor calls took the lock %u times", nandlog_sim_lock_count());
   end_test();
}

static void test_boot_cost(void)
{
   // A hundred epochs in the ring: finding the newest must not mean reading every slot
   begin_test("13", "a warm boot with a populated ring reads a handful of pages, not the whole ring");
   fresh_epoch();
   for (uint32_t i = 0; i < 100; ++i)
   {
      begin_epoch("ring");
      write_pages(1, i);
   }
   nandlog_sim_reset_counters();
   reboot();
   const nandlog_sim_counters_t c = nandlog_sim_counters();
   CHECK(c.page_reads <= 90, "a warm boot latched %u pages", c.page_reads);
   nandlog_begin_session();
   CHECK(nandlog_epoch_count() == NANDLOG_MAX_LISTED_EPOCHS, "the ring lists %u epochs", nandlog_epoch_count());
   nandlog_end_session();
   end_test();
}


// Light Tears and Torn Erases -----------------------------------------------------------------------------------------

static void test_torn_erase_at_head(void)
{
   // A cut lands in the erase of the next block the head will enter, leaving it part-erased. Whatever recovery does
   // with it, no committed page is lost and nothing is programmed twice
   begin_test("-", "an erase torn just ahead of the head costs nothing committed");
   fresh_epoch();
   write_pages(ppb() + (ppb() / 2), 0);
   nandlog_deinit();
   nandlog_sim_snapshot();
   uint32_t bad = 0, trials = 0;
   for (uint32_t seed = 1; seed <= 12; ++seed, ++trials)
   {
      nandlog_sim_restore();
      reboot();
      nandlog_sim_faults_t faults = no_faults();
      faults.torn_reads_uncorrectable = true;
      faults.ecc_hides_bits = 16;
      set_faults(&faults);
      nandlog_sim_seed(seed);
      nandlog_sim_power_cut(1, (seed & 1) ? NANDLOG_SIM_CUT_BITWISE : NANDLOG_SIM_CUT_UNSTABLE, 0.02 * seed, &cut_jump);
      if (setjmp(cut_jump) == 0)
         for (uint32_t i = 0; i < 4; ++i)
         {
            store(5000 + i, 100);
            flush(false);
         }
      nandlog_sim_power_cut_disarm();
      nandlog_sim_clear_repeated_programs();
      reboot();
      write_pages(2 * ppb(), 10000);
      reboot();
      const audit_t a = audit_current();
      const uint32_t committed = ppb() + (ppb() / 2);
      if ((ids_seen(0, committed) != committed) || (ids_seen(10000, 2 * ppb()) != (2 * ppb())) || a.damaged ||
          repeated_programs_outside_retired_blocks())
         ++bad;
   }
   CHECK(!bad, "%u of %u torn erases lost or damaged pages, or caused a second program", bad, trials);
   end_test();
}


// Found by the First Adversarial Tester and Reviewers -----------------------------------------------------------------
//
// An adversarial tester and a reviewer found what follows. Each test fails against the code they read and passes
// now. Tags: T for the tester's report,
// R for the reviewer's, A for the paper author's

static int store_at(uint32_t id, uint32_t length, uint32_t timestamp)
{
   fill_record(record, length, id);
   return (int)nandlog_store_record(7, timestamp, record, length);
}

static uint32_t span_pages(uint32_t from, uint32_t to)
{
   uint32_t pages = 0;
   nandlog_begin_session();
   nandlog_begin_reading(from, to);
   nandlog_read_span(&pages, NULL);
   nandlog_end_reading();
   nandlog_end_session();
   return pages;
}

static uint32_t newest_slot_pages(uint32_t *pages, uint32_t capacity)
{
   // Every ring page holding a valid slot of the highest epoch named anywhere in the ring, judged from the medium
   uint32_t found = 0, newest = 0;
   for (uint32_t page = 0; page < (METADATA_RING_BLOCKS * ppb()); ++page)
   {
      nandlog_meta_header_t header;
      memcpy(&header, nandlog_sim_raw_page(page), sizeof(header));
      if ((header.magic != NANDLOG_META_MAGIC) ||
          (header.header_crc != crc32_bits((const uint8_t*)&header, NANDLOG_META_HEADER_CRC_BYTES)) || (header.epoch < newest))
         continue;
      if (header.epoch > newest)
         found = 0;
      newest = header.epoch;
      if (found < capacity)
         pages[found++] = page;
   }
   return found;
}

static void rot_page(uint32_t page)
{
   // Bits lost from the header, and the part's ECC unable to correct the page
   nandlog_sim_corrupt(page, 4, 0x5A);
   nandlog_sim_corrupt(page, 12, 0xA5);
   nandlog_sim_set_ecc_status(page, NANDLOG_SIM_ECC_UNCORRECTABLE);
}

#ifndef SIM_CHIP_WINBOND
static void test_t1_torn_table_update(void)
{
   // A retirement's table update cut under the model in which the bits a cut leaves undecided read differently on
   // every boot. Whatever the first boot makes of it must hold on every boot after, or positions shift under the
   // pages committed in between
   begin_test("T1", "a torn bad-block table update reads the same on every later boot (UNSTABLE cut)");
   uint32_t trials = 0, flipped = 0, lost = 0;
   for (uint64_t seed = 1; seed <= 12; ++seed)
      for (uint32_t cut_at = 1; cut_at < 32; ++cut_at)
      {
         fresh_epoch();
         const uint32_t start = epoch_start_block();
         write_pages(10, 0);
         nandlog_sim_set_ecc_status(start * ppb() + 10, NANDLOG_SIM_ECC_AT_LIMIT);
         store(10, page_record_bytes());
         nandlog_sim_seed(seed);
         nandlog_sim_reset_operations();
         nandlog_sim_power_cut(cut_at, NANDLOG_SIM_CUT_UNSTABLE, 0.97, &cut_jump);
         if (setjmp(cut_jump) == 0)
         {
            flush(true);
            nandlog_sim_power_cut_disarm();
            break;
         }
         nandlog_sim_power_cut_disarm();
         nandlog_sim_cut_t cut;
         nandlog_sim_last_cut(&cut);
         if ((cut.command != 0x10) || cut.otp || ((cut.page / ppb()) < log_end_block()))
            continue;
         ++trials;
         reboot();
         const bool retired = nandlog_chip_is_bad_block(start * ppb());
         write_pages(70, 100);
         bool flip = false, loss = false;
         for (uint32_t boot = 0; boot < 20; ++boot)
         {
            reboot();
            flip = flip || (nandlog_chip_is_bad_block(start * ppb()) != retired);
            audit_current();
            loss = loss || (ids_seen(0, 10) != 10) || (ids_seen(100, 70) != 70);
         }
         flipped += flip ? 1 : 0;
         lost += loss ? 1 : 0;
      }
   CHECK(trials, "no cut landed in a table page");
   CHECK(!flipped && !lost, "of %u torn table updates, %u read differently on a later boot and %u lost committed pages", trials, flipped, lost);
   end_test();
}
#endif

static void test_t2_torn_ring_slot(void)
{
   // The same for the slot begin_epoch() writes: one boot takes the old epoch and commits pages into the blocks the
   // torn slot names; no later boot may take the new one and erase them, or the reverse
   begin_test("T2", "a torn metadata slot reads the same on every later boot (UNSTABLE cut)");
   uint32_t trials = 0, lost = 0;
   for (uint64_t seed = 1; seed <= 16; ++seed)
      for (uint32_t cut_at = 1; cut_at < 8; ++cut_at)
      {
         fresh_epoch();
         write_pages(10, 0);
         nandlog_sim_seed(seed);
         nandlog_sim_reset_operations();
         nandlog_begin_session();
         nandlog_sim_power_cut(cut_at, NANDLOG_SIM_CUT_UNSTABLE, 0.97, &cut_jump);
         if (setjmp(cut_jump) == 0)
         {
            nandlog_begin_epoch("E2", 2);
            nandlog_sim_power_cut_disarm();
            nandlog_end_session();
            break;
         }
         nandlog_sim_power_cut_disarm();
         nandlog_sim_cut_t cut;
         nandlog_sim_last_cut(&cut);
         if ((cut.command != 0x10) || cut.otp || (cut.page >= (METADATA_RING_BLOCKS * ppb())))
            continue;
         ++trials;
         reboot();
         nandlog_epoch_info_t info;
         nandlog_begin_session();
         nandlog_epoch_info(0, &info);
         nandlog_end_session();
         const bool old_epoch = (info.epoch == 1);
         write_pages(70, 100);
         bool loss = false;
         for (uint32_t boot = 0; boot < 20; ++boot)
         {
            reboot();
            audit_current();
            loss = loss || (ids_seen(100, 70) != 70) || (old_epoch && (ids_seen(0, 10) != 10));
         }
         lost += loss ? 1 : 0;
      }
   CHECK(trials, "no cut landed in a ring slot");
   CHECK(!lost, "of %u torn slots, %u let a later boot lose committed pages", trials, lost);
   end_test();
}

static void test_t3_reads_stay_in_the_buffer(void)
{
   // The documented read loop sizes its buffer at NANDLOG_MAX_DATA_BYTES_PER_PAGE, and a page holds less than the
   // part's page size. Nothing past the payload the part can hold may be written into the caller's buffer
   begin_test("T3", "page reads write nothing past a payload's worth of the caller's buffer");
   fresh_epoch();
   write_pages(3, 0);
   static uint8_t guarded[NANDLOG_MAX_PAGE_SIZE_BYTES + 64];
   const uint32_t room = nandlog_data_bytes_per_page();
   memset(guarded, 0xA5, sizeof(guarded));
   nandlog_page_header_t header;
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   uint32_t pages = 0, delivered = 0;
   nandlog_read_span(&pages, NULL);
   nandlog_retrieve_page_by_seq(1, guarded, &header);
   nandlog_retransmit_clear();
   const uint32_t seqs[1] = { 2 };
   nandlog_retransmit_add(seqs, 1);
   nandlog_retrieve_retransmit_page(0, guarded, &header);
   for (uint32_t i = 0; i < pages; ++i)
      delivered += nandlog_retrieve_next_page(guarded, &header) ? 1 : 0;
   nandlog_end_reading();
   nandlog_end_session();
   uint32_t overrun = 0;
   for (uint32_t i = room; i < sizeof(guarded); ++i)
      overrun += (guarded[i] != 0xA5) ? 1 : 0;
   CHECK((delivered == 3) && !overrun, "%u of 3 pages delivered; %u bytes written past the %u a page can carry", delivered, overrun, room);
   end_test();
}

static void test_t4_earlier_full_epoch_with_retired_start(void)
{
   // An epoch whose first block was retired after it began fills the region. Its successor begins at the first
   // good block, which is the earlier epoch's position 0; selecting the earlier epoch must still find its pages
   begin_test("T4", "an earlier epoch that filled the region from a retired first block still reads");
   new_part();
   reboot();
   begin_epoch("E1");
   const uint32_t start = epoch_start_block();
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_pages[0] = start * ppb() + 5;
   faults.num_unwritable_pages = 1;
   set_faults(&faults);
   const uint32_t e1 = fill_region(0);
   clear_faults();
   audit_current();
   const uint32_t e1_seen = ids_seen(0, e1);
   reboot();
   begin_epoch("E2");
   write_pages(3, 30000);
   nandlog_begin_session();
   const bool selected = nandlog_select_epoch(1);
   const audit_t a = audit_view(true);
   nandlog_end_session();
   const uint32_t still = ids_seen(0, e1);
   CHECK(nandlog_bad_block_count() == 1, "the first block was not retired (%u retired)", nandlog_bad_block_count());
   CHECK(selected && (still + (4 * ppb()) >= e1_seen) && !a.damaged, "the earlier epoch delivers %u of the %u pages it held, over %u positions",
         still, e1_seen, a.positions);
   end_test();
}

static void test_t5_earlier_epoch_past_a_torn_page(void)
{
   // A cut tears a page so lightly that its header still reads erased, though the page is not blank. Recovery passes
   // over it and the epoch commits more pages after it; read later as an earlier epoch, it must still end at the
   // last of them, not at the torn page
   begin_test("T5", "an earlier epoch's read ends at its last page, past a torn one whose header reads erased");
   bool reproduced = false;
   for (uint64_t seed = 1; (seed < 200) && !reproduced; ++seed)
   {
      fresh_epoch();
      const uint32_t torn = epoch_start_block() * ppb() + 32;
      write_pages(32, 0);
      store(32, page_record_bytes());
      nandlog_sim_seed(seed);
      nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_BITWISE, 0.0005, &cut_jump);
      if (setjmp(cut_jump) == 0)
         flush(true);
      nandlog_sim_power_cut_disarm();
      const uint8_t *raw = nandlog_sim_raw_page(torn);
      bool header_erased = true, blank = true;
      for (uint32_t i = 0; i < (page_bytes() + nandlog_chip_geometry()->spare_size_bytes); ++i)
      {
         if (raw[i] != 0xFF)
            blank = false;
         if ((i < sizeof(nandlog_page_header_t)) && (raw[i] != 0xFF))
            header_erased = false;
      }
      if (!header_erased || blank)
         continue;
      reproduced = true;
      reboot();
      write_pages(8, 1000);
      begin_epoch("E2");
      write_pages(2, 5000);
      nandlog_begin_session();
      const bool selected = nandlog_select_epoch(1);
      audit_view(true);
      nandlog_end_session();
      CHECK(selected && (ids_seen(0, 32) == 32) && (ids_seen(1000, 8) == 8), "the earlier epoch delivers %u of 32 pages before the torn one and %u of 8 after",
            ids_seen(0, 32), ids_seen(1000, 8));
   }
   CHECK(reproduced, "no seed tore a page that way");
   end_test();
}

static void test_t6_sequential_read_reports_page_format(void)
{
   // A firmware change of NANDLOG_RECORD_FRAMING continues an epoch in place, so pages of both formats share it.
   // Each page must be reported with its own magic, as the by-sequence read already does
   begin_test("T6", "a sequential read reports each page's own record format");
   fresh_epoch();
   const uint32_t first = epoch_start_block() * ppb();
   write_pages(3, 0);
   const uint32_t other = (NANDLOG_PAGE_MAGIC_THIS_BUILD == NANDLOG_PAGE_MAGIC) ? NANDLOG_PAGE_MAGIC_FRAMED : NANDLOG_PAGE_MAGIC;
   nandlog_deinit();
   put_page_as(first + 3, other, 1, 3, 3);
   reboot();
   write_pages(1, 4);
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   uint32_t pages = 0, wrong = 0;
   nandlog_read_span(&pages, NULL);
   for (uint32_t i = 0; i < pages; ++i)
   {
      nandlog_page_header_t header;
      nandlog_retrieve_next_page(readback, &header);
      const uint32_t expected = (header.seq == 3) ? other : NANDLOG_PAGE_MAGIC_THIS_BUILD;
      wrong += (header.magic != expected) ? 1 : 0;
   }
   nandlog_end_reading();
   nandlog_end_session();
   CHECK((pages == 5) && !wrong, "%u of %u pages reported with another page's format", wrong, pages);
   end_test();
}

static void test_t7_length_overflow(void)
{
   begin_test("T7", "a record length near 2^32 is refused as too large, not wrapped");
   fresh_epoch();
   static uint8_t small[64];
   const nandlog_status_t status = nandlog_store_record(1, 5, small, 0xFFFFFFF9u);
   CHECK(status == NANDLOG_ERR_TOO_LARGE, "status %d", (int)status);
   CHECK(!nandlog_has_buffered_data(), "something was buffered");
   end_test();
}

static void test_t8_time_bound_edges(void)
{
   begin_test("T8", "time bounds: a window before the first record selects nothing; 0xFFFFFFFF is a time");
   fresh_epoch();
   for (uint32_t page = 0; page < 3; ++page)
   {
      for (uint32_t i = 0; i < 10; ++i)
         store_at(page * 100 + i, 8, 1000 * (page + 1) + i);
      flush(true);
   }
   CHECK(span_pages(1500, 1600) == 0, "a window between pages selects %u pages", span_pages(1500, 1600));
   CHECK(span_pages(100, 500) == 0, "a window before the first record selects %u pages", span_pages(100, 500));
   CHECK(span_pages(0, 500) == 0, "a window from the start to before the first record selects %u pages", span_pages(0, 500));
   CHECK(span_pages(5000, 0) == 0, "a window after the last record selects %u pages", span_pages(5000, 0));
   store_at(900, 8, 4000);
   store_at(901, 8, 0xFFFFFFFF);
   flush(true);
   nandlog_page_header_t header;
   memset(&header, 0, sizeof(header));
   bool end = false;
   nandlog_read_recent_page(0, readback, &header, &end);
   CHECK((header.last_timestamp > 4000) && (header.last_timestamp != NANDLOG_NO_TIMESTAMP), "the page's last time %u does not cover the record stamped 0xFFFFFFFF",
         header.last_timestamp);
   CHECK(span_pages(5000, 0) == 1, "a window from 5000 on selects %u pages, not the one holding the record stamped 0xFFFFFFFF", span_pages(5000, 0));
   end_test();
}

static void test_r1_rotted_ring_slot(void)
{
   // The newest slot becomes unreadable with no power cut at all. One copy lost must change nothing; with every
   // copy lost, the epoch's own pages still say it exists, and no boot may erase them as the older epoch's window
   begin_test("R1", "an unreadable newest metadata slot neither reverts the epoch nor erases its pages");
   for (uint32_t lose_all = 0; lose_all < 2; ++lose_all)
   {
      fresh_epoch();
      write_pages(100, 0);
      begin_epoch("E2");
      write_pages(150, 1000);
      uint32_t pages[8];
      const uint32_t copies = newest_slot_pages(pages, 8);
      for (uint32_t i = 0; i < (lose_all ? copies : 1); ++i)
         rot_page(pages[i]);
      reboot();
      write_pages(10, 2000);
      reboot();
      const audit_t a = audit_current();
      CHECK((ids_seen(1000, 150) == 150) && (ids_seen(2000, 10) == 10) && !a.damaged,
            "with %s of the slot unreadable, the current epoch delivers %u of its 150 pages and %u of 10 more",
            lose_all ? "every copy" : "one copy", ids_seen(1000, 150), ids_seen(2000, 10));
   }
   end_test();
}

#ifndef SIM_CHIP_WINBOND
static void test_r1_rotted_table_version(void)
{
   // The same for the newest version of the bad-block table: losing one page of it must not re-admit the block it
   // retired, which would shift every position after that block
   begin_test("R1", "an unreadable page of the newest bad-block table changes nothing");
   fresh_epoch();
   const uint32_t start = epoch_start_block();
   write_pages(10, 0);
   nandlog_sim_set_ecc_status(start * ppb() + 10, NANDLOG_SIM_ECC_AT_LIMIT);
   write_pages(161, 10);
   CHECK(nandlog_bad_block_count() == 1, "the relocation retired %u blocks", nandlog_bad_block_count());
   uint32_t newest = 0, newest_page = 0;
   for (uint32_t page = log_end_block() * ppb(); page < ((log_end_block() + 8) * ppb()); ++page)
   {
      const uint8_t *raw = nandlog_sim_raw_page(page);
      uint32_t version = 0, count = 0, crc = 0;
      memcpy(&crc, raw + 4, 4);
      memcpy(&version, raw + 8, 4);
      memcpy(&count, raw + 12, 4);
      if (!memcmp(raw, "BBT3", 4) && (count <= 256) && (crc32_bits(raw + 8, 8 + (4 * count)) == crc) && (version >= newest))
      {
         if (version > newest)
            newest_page = page;
         newest = version;
      }
   }
   rot_page(newest_page);
   reboot();
   const audit_t a = audit_current();
   CHECK(nandlog_chip_is_bad_block(start * ppb()) && (nandlog_bad_block_count() == 1), "the retired block came back (%u retired)", nandlog_bad_block_count());
   CHECK((ids_seen(0, 171) == 171) && (a.positions == 171) && !a.damaged, "%u of 171 pages delivered over %u positions", ids_seen(0, 171), a.positions);
   end_test();
}
#endif

static void test_r2_failure_at_the_wrap_survives_a_reboot(void)
{
   // The block before the wrap refuses a program and cannot be relocated, because every other block is the epoch's.
   // The refused page may hold a partial program no read can see: after a reboot it must not be programmed again
   begin_test("R2", "a program refused at the wrap is not retried after a reboot");
   fresh_epoch();
   const uint32_t last = log_end_block() - 1;
   for (uint32_t i = 0; i < ((log_blocks() - 1) * ppb()) + 10; ++i)
      store(i, page_record_bytes());
   flush(true);
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_blocks[0] = last;
   faults.num_unwritable_blocks = 1;
   set_faults(&faults);
   store(20000, page_record_bytes());
   CHECK(flush(true) == NANDLOG_ERR_FULL, "the commit did not fail as full");
   clear_faults();
   nandlog_sim_reset_counters();
   nandlog_sim_clear_repeated_programs();
   reboot();
   const int stored = store(20001, page_record_bytes());
   const int flushed = flush(true);
   const nandlog_sim_counters_t c = nandlog_sim_counters();
   const uint32_t repeated = repeated_programs_outside_retired_blocks();
   CHECK(!c.blank_reprograms_after_boot && !repeated, "after the reboot %u refused page(s) were programmed again (store %d, flush %d)",
         c.blank_reprograms_after_boot + repeated, stored, flushed);
   end_test();
}

static void test_r2_unwritable_head_relocated_after_reboot(void)
{
   // No block would take the head's page, so the commit gives up. The page it was refused at must not be programmed
   // again after a reboot, and its block's pages move before logging resumes
   begin_test("R2", "a head block that refused a program is relocated after a reboot, not programmed again");
   fresh_epoch();
   const uint32_t start = epoch_start_block();
   write_pages(10, 0);
   nandlog_sim_faults_t faults = no_faults();
   for (uint32_t i = 0; i < 6; ++i)
      faults.unwritable_blocks[faults.num_unwritable_blocks++] = start + i;
   set_faults(&faults);
   store(10, page_record_bytes());
   CHECK(flush(true) == NANDLOG_ERR_UNWRITABLE, "the commit did not fail as unwritable");
   clear_faults();
   nandlog_sim_reset_counters();
   nandlog_sim_clear_repeated_programs();
   reboot();
   write_pages(5, 20);
   const nandlog_sim_counters_t c = nandlog_sim_counters();
   const uint32_t repeated = repeated_programs_outside_retired_blocks();
   reboot();
   const audit_t a = audit_current();
   const uint32_t blank = blank_reprograms_outside_retired_blocks();
   (void)c;
   CHECK(!blank && !repeated, "%u refused page(s) programmed again after the reboot", blank + repeated);
   CHECK(nandlog_chip_is_bad_block(start * ppb()), "the block that refused the program is still in use");
   CHECK((ids_seen(0, 10) == 10) && (ids_seen(20, 5) == 5) && !a.damaged, "%u of 10 and %u of 5 pages delivered", ids_seen(0, 10), ids_seen(20, 5));
   end_test();
}

static void test_r3_span_exact_while_storing(void)
{
   // Records may be stored while a read is open; the read's declared byte total must still be what it delivers
   begin_test("R3", "a read's byte total stays exact when records are stored during it");
   fresh_epoch();
   write_pages(5, 0);
   store(100, 100);
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   uint32_t pages = 0, declared = 0, delivered = 0;
   nandlog_read_span(&pages, &declared);
   store(101, 100);
   for (uint32_t i = 0; i < pages; ++i)
   {
      nandlog_page_header_t header;
      delivered += nandlog_retrieve_next_page(readback, &header);
   }
   nandlog_end_reading();
   nandlog_end_session();
   CHECK((pages == 6) && (declared == delivered), "declared %u bytes over %u pages, delivered %u", declared, pages, delivered);
   end_test();
}

static void test_a1_interrupted_erase_reerased(void)
{
   // The head waits at the start of a block holding an earlier epoch's first few pages, and the boot's erase of it
   // is cut. What is left reads as programmed pages followed by an erased tail -- the same picture as pages the
   // epoch itself tore -- and the tail may read erased without being reliably so. The block must be erased again
   // before anything is programmed into it
   begin_test("A1", "a block whose erase a cut interrupted is erased again before the head programs it");
   fresh_epoch();
   const uint32_t block = epoch_start_block() + 1;
   write_pages(ppb(), 0);
   nandlog_deinit();
   for (uint32_t i = 0; i < 10; ++i)
      put_page_as(block * ppb() + i, NANDLOG_PAGE_MAGIC_THIS_BUILD, 0, i, 30000 + i);   // an epoch older than this one
   nandlog_sim_seed(3);
   nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_BITWISE, 0.9, &cut_jump);
   if (setjmp(cut_jump) == 0)
      nandlog_init();
   nandlog_sim_power_cut_disarm();
   nandlog_sim_cut_t cut;
   nandlog_sim_last_cut(&cut);
   CHECK((cut.command == 0xD8) && ((cut.page / ppb()) == block), "the cut did not land in the erase of block %u", block);
   reboot();
   write_pages(5, 100);
   uint32_t residue = 0;
   for (uint32_t i = 0; i < ppb(); ++i)
   {
      nandlog_page_header_t header;
      if (!raw_page_valid(block * ppb() + i, &header, true) && !raw_page_erased(block * ppb() + i))
         ++residue;
   }
   reboot();
   const audit_t a = audit_current();
   CHECK(!residue, "%u pages of the interrupted erase are still in the block the head programmed", residue);
   CHECK((ids_seen(0, ppb()) == ppb()) && (ids_seen(100, 5) == 5) && !a.damaged, "%u of %u and %u of 5 pages delivered", ids_seen(0, ppb()), ppb(), ids_seen(100, 5));
   end_test();
}

static void test_r2_blank_page_below_the_head(void)
{
   // A page below the head comes to read blank in every bit -- a torn page whose few programmed cells drifted back,
   // say -- with pages committed after it. Wherever the search happens to meet it, the head must not land on it
   begin_test("R2", "a page below the head that reads blank does not pull the head back to it");
   fresh_epoch();
   const uint32_t first = epoch_start_block() * ppb(), head = (2 * ppb()) + 20;
   write_pages(head, 0);
   nandlog_deinit();
   nandlog_sim_snapshot();
   uint32_t misplaced = 0, trials = 0;
   for (uint32_t position = head - ppb(); position < (head - 1); ++position, ++trials)
   {
      nandlog_sim_restore();
      memset(nandlog_sim_raw_page(first + position), 0xFF, page_bytes() + nandlog_chip_geometry()->spare_size_bytes);
      nandlog_sim_clear_repeated_programs();
      nandlog_sim_reset_counters();
      reboot();
      write_pages(1, 30000);
      const uint32_t programmed_again = nandlog_sim_counters().blank_reprograms_after_boot + nandlog_sim_counters().repeated_programs;
      reboot();
      audit_current();
      if ((ids_seen(position + 1, head - position - 1) != (head - position - 1)) || (ids_seen(30000, 1) != 1) || programmed_again)
         ++misplaced;
   }
   CHECK(!misplaced, "%u of %u blank pages below the head misplaced it", misplaced, trials);
   end_test();
}

static void test_t9_questionable_calls(void)
{
   begin_test("T9", "flush commits while disabled; by-sequence reads work after a read and for the page a read committed");
   fresh_epoch();
   write_pages(5, 0);
   store(100, 20);
   nandlog_disable(true);
   CHECK(flush(true) == NANDLOG_OK, "flush(true) of records accepted before nandlog_disable(true) was refused");
   nandlog_disable(false);

   // Read everything, so the read closes itself, then ask for retransmissions
   nandlog_page_header_t header;
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   uint32_t pages = 0;
   nandlog_read_span(&pages, NULL);
   for (uint32_t i = 0; i < pages; ++i)
      nandlog_retrieve_next_page(readback, &header);
   const uint32_t seqs[2] = { 1, 3 };
   nandlog_retransmit_clear();
   nandlog_retransmit_add(seqs, 2);
   const uint32_t total = nandlog_retransmit_total_bytes();
   const uint32_t got = nandlog_retrieve_retransmit_page(0, readback, &header) + nandlog_retrieve_retransmit_page(1, readback, &header);
   nandlog_end_session();
   CHECK(total && (got == total), "after the read ran to its end, retransmission declared %u bytes and delivered %u", total, got);

   // What was buffered is committed by the read, and served by its sequence number
   store(200, 20);
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   nandlog_read_span(&pages, NULL);
   uint32_t last_length = 0, last_seq = 0;
   for (uint32_t i = 0; i < pages; ++i)
   {
      last_length = nandlog_retrieve_next_page(readback, &header);
      last_seq = header.seq;
   }
   nandlog_begin_reading(0, 0);
   const uint32_t again = nandlog_retrieve_page_by_seq(last_seq, readback, &header);
   nandlog_end_session();
   CHECK(last_length && (again == last_length), "the page the read committed came as seq %u with %u bytes, and by its sequence number with %u", last_seq, last_length, again);
   end_test();
}


// Found by the Second Adversarial Tester -----------------------------------------------------------------------------
//
// A second adversarial tester, a fifth reviewer and the paper's author found what follows. Each test fails against
// the code they read and passes now. Tags: S for the tester's report, Q for the reviewer's

static uint32_t spare_bytes(void)       { return nandlog_chip_geometry()->spare_size_bytes; }

static bool cut_program_of(uint32_t page, uint32_t id, nandlog_sim_cut_model_t model, double q, uint64_t seed)
{
   // Store a page-filling record and flush it, with power cut at the program of 'page' and nothing after the cut
   // run, then reboot. The cut moves along the flush's operations until it lands on that program. False if it never
   // does
   nandlog_deinit();
   nandlog_sim_snapshot();
   for (uint32_t n = 1; n < 8; ++n)
   {
      reboot();
      store(id, page_record_bytes());
      nandlog_sim_seed(seed);
      nandlog_sim_power_cut(n, model, q, &cut_jump);
      if (setjmp(cut_jump) == 0)
         flush(true);
      nandlog_sim_power_cut_disarm();
      nandlog_sim_cut_t cut;
      if (nandlog_sim_last_cut(&cut) && (cut.command == 0x10) && (cut.page == page))
      {
         reboot();
         return true;
      }
      nandlog_deinit();
      nandlog_sim_restore();
   }
   reboot();
   return false;
}

static void drift_blank(uint32_t page)
{
   // Every cell the page was left with drifts back: it reads erased in every bit, main and spare
   memset(nandlog_sim_raw_page(page), 0xFF, page_bytes() + spare_bytes());
}

static uint32_t page_by_seq(uint32_t seq, uint32_t *id)
{
   // One page of the current epoch by its sequence number: its length, and the id of its first record
   nandlog_page_header_t header;
   nandlog_begin_session();
   const uint32_t length = nandlog_retrieve_page_by_seq(seq, readback, &header);
   nandlog_end_session();
   *id = 0xFFFFFFFF;
   if (length >= (prefix_bytes() + 5 + 4))
      memcpy(id, readback + prefix_bytes() + 5, sizeof(*id));
   return length;
}

static void test_s1_drift_below_the_head(void)
{
   // Two commits in a row torn by cuts -- a brown-out loop -- and pages committed after them; then the first torn
   // page drifts back to reading blank in every bit, or both do. Wherever the search meets them, the head must stay
   // where it is
   begin_test("S1", "two torn pages below the head, since drifted blank, cannot pull the head back");
   uint32_t misplaced = 0, trials = 0;
   for (uint32_t b = (2 * ppb()) - 28; b < (2 * ppb()); ++b, ++trials)
   {
      const bool both = (b & 1);
      new_part();
      reboot();
      begin_epoch("s1");
      const uint32_t first = epoch_start_block() * ppb(), after = (2 * ppb()) + 22 - (b + 2);
      write_pages(b, 0);
      if (!cut_program_of(first + b, 30000, NANDLOG_SIM_CUT_BITWISE, 0.5, 11 + b) ||
          !cut_program_of(first + b + 1, 30001, NANDLOG_SIM_CUT_BITWISE, 0.5, 99 + b))
      {
         ++misplaced;
         continue;
      }
      write_pages(after, 1000);
      nandlog_deinit();
      drift_blank(first + b);
      if (both)
         drift_blank(first + b + 1);
      nandlog_sim_clear_repeated_programs();
      reboot();
      write_pages(1, 30002);
      const nandlog_sim_counters_t c = nandlog_sim_counters();
      reboot();
      const audit_t a = audit_current();
      if ((ids_seen(1000, after) != after) || (ids_seen(30002, 1) != 1) || a.damaged || a.duplicates || c.repeated_programs ||
          c.blank_reprograms_after_boot)
         ++misplaced;
   }
   CHECK(!misplaced, "%u of %u placements lost committed pages or programmed a page again", misplaced, trials);
   end_test();
}

static void test_q1_drift_under_durable_pages(void)
{
   // The reviewer's sequences: pages 0-9 committed; the commit of 10 torn; then either the commit of 11 torn too, or
   // 11 committed and later rotted; then 12 committed. Page 10 then drifts blank. Page 12 is durable
   begin_test("Q1", "a torn page drifting blank under a durable page never costs it, nor its sequence number");
   for (uint32_t variant = 0; variant < 2; ++variant)
   {
      new_part();
      reboot();
      begin_epoch("q1");
      const uint32_t first = epoch_start_block() * ppb();
      write_pages(10, 0);
      bool placed = cut_program_of(first + 10, 30000, NANDLOG_SIM_CUT_BITWISE, 0.05, 5);
      if (!variant)
         placed = placed && cut_program_of(first + 11, 30001, NANDLOG_SIM_CUT_BITWISE, 0.05, 6);
      else
         write_pages(1, 11);
      write_pages(1, 12);
      nandlog_deinit();
      if (variant)
         rot_page(first + 11);
      drift_blank(first + 10);
      nandlog_sim_clear_repeated_programs();
      reboot();
      write_pages(4, 100);
      const nandlog_sim_counters_t c = nandlog_sim_counters();
      reboot();
      const audit_t a = audit_current();
      uint32_t id = 0;
      const uint32_t length = page_by_seq(12, &id);
      CHECK(placed && (ids_seen(12, 1) == 1) && (ids_seen(100, 4) == 4) && length && (id == 12) && !a.damaged && !a.duplicates &&
            !c.repeated_programs && !c.blank_reprograms_after_boot,
            "%s: page 12 %s, %u of 4 later pages delivered, sequence 12 now holds id %u, %u page(s) programmed again",
            variant ? "11 rotted" : "11 torn", ids_seen(12, 1) ? "delivered" : "LOST", ids_seen(100, 4), length ? id : 0,
            c.repeated_programs + c.blank_reprograms_after_boot);
   }
   end_test();
}

static void test_s2_blank_then_unreadable(void)
{
   // A page below the head reads blank and the one after it no longer checksums. Out of the fault model for pages
   // that were committed, but the search still looks past the unreadable page for one of the epoch
   begin_test("S2", "a blank page followed by an unreadable one below the head does not pull the head back");
   fresh_epoch();
   const uint32_t first = epoch_start_block() * ppb(), head = (2 * ppb()) + 20;
   write_pages(head, 0);
   nandlog_deinit();
   nandlog_sim_snapshot();
   uint32_t misplaced = 0, trials = 0;
   for (uint32_t b = head - ppb() - 10; b < (head - 2); ++b, ++trials)
   {
      nandlog_sim_restore();
      drift_blank(first + b);
      nandlog_sim_corrupt(first + b + 1, 0, 0xFF);
      nandlog_sim_clear_repeated_programs();
      nandlog_sim_reset_counters();
      reboot();
      write_pages(1, 30000);
      const uint32_t again = nandlog_sim_counters().blank_reprograms_after_boot + nandlog_sim_counters().repeated_programs;
      reboot();
      audit_current();
      if ((ids_seen(b + 2, head - b - 2) != (head - b - 2)) || (ids_seen(30000, 1) != 1) || again)
         ++misplaced;
   }
   CHECK(!misplaced, "%u of %u placements misplaced the head", misplaced, trials);
   end_test();
}

static void test_s3_adoption_after_the_wrap(void)
{
   // Every copy of the newest slot is lost after its epoch has swept the region round to the older epoch's first
   // block -- erasing it as window, or writing into it. The newer epoch is found from its own pages all the same
   begin_test("S3", "an epoch whose slot is lost is recovered from its pages after it wrapped onto the older epoch");
   const int32_t short_of_wrap[2] = { 20, -70 };
   for (uint32_t i = 0; i < 2; ++i)
   {
      fresh_epoch();
      write_pages(100, 0);
      begin_epoch("E2");
      const uint32_t pages = ((log_blocks() - 2) * ppb()) - short_of_wrap[i];
      write_pages(pages, 1000);
      for (uint32_t k = 0; k < 4; ++k)
         flush(false);
      nandlog_deinit();
      uint32_t slots[8];
      const uint32_t copies = newest_slot_pages(slots, 8);
      for (uint32_t k = 0; k < copies; ++k)
         rot_page(slots[k]);
      reboot();
      nandlog_epoch_info_t info;
      memset(&info, 0, sizeof(info));
      nandlog_begin_session();
      nandlog_epoch_info(0, &info);
      nandlog_end_session();
      const int stored = store(20000, page_record_bytes());
      write_pages(20, 20001);
      reboot();
      const audit_t a = audit_current();
      CHECK((info.epoch == 2) && !stored && (ids_seen(1000, pages) == pages) && (ids_seen(20000, 21) == 21) && !a.damaged,
            "with the newer epoch %d pages short of the wrap: epoch %u current, store returned %d, %u of %u of its pages and %u of 21 more delivered",
            short_of_wrap[i], info.epoch, stored, ids_seen(1000, pages), pages, ids_seen(20000, 21));
   }
   end_test();
}

#ifndef SIM_CHIP_WINBOND



static void test_q3_table_rollback_detected(void)
{
   // Both copies of the newest bad-block table version become unreadable, so the driver loads the version before
   // it, which lacks the newest retirement: every position after that block would shift. The ring recorded how
   // many blocks were retired, and the boot refuses to write over a table that has fewer
   begin_test("Q3", "a bad-block table that lost its newest retirement is caught before anything is written");
   fresh_epoch();
   const uint32_t start = epoch_start_block(), failing = start + 3;
   nandlog_sim_faults_t faults = no_faults();
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = failing;
   set_faults(&faults);
   write_pages(5 * ppb(), 0);
   clear_faults();
   CHECK(nandlog_chip_is_bad_block(failing * ppb()), "the block that would not erase was not retired");
   nandlog_deinit();

   // The newest table version: the last pair written in the reserve
   const uint32_t base = log_end_block() * ppb();
   uint32_t newest = 0, found = 0;
   for (uint32_t page = base; page < (base + (8 * ppb())); ++page)
   {
      uint32_t version = 0;
      if (memcmp(nandlog_sim_raw_page(page), "BBT", 3))
         continue;
      memcpy(&version, nandlog_sim_raw_page(page) + 8, sizeof(version));
      if (version >= newest)
      {
         newest = version;
         found = page;
      }
   }
   rot_page(found);
   rot_page(found - 1);
   nandlog_sim_reset_counters();
   reboot();
   const nandlog_sim_counters_t before = nandlog_sim_counters();
   const int stored = store(5000, page_record_bytes());
   const int flushed = flush(true);
   CHECK(!nandlog_chip_is_bad_block(failing * ppb()), "the table did not lose the retirement, so this test proves nothing");
   CHECK((flushed != NANDLOG_OK) && nandlog_sim_unwritable_reported() && (nandlog_sim_counters().page_writes == before.page_writes),
         "with the retirement lost, the store returned %d, the flush %d, and %u page(s) were programmed", stored, flushed,
         nandlog_sim_counters().page_writes - before.page_writes);
   end_test();
}
#endif

static void test_s6_epoch_details_protected(void)
{
   // An epoch's details are guarded by their checksum in both copies of its slot: a copy whose details do not
   // check is never the one written again, a slot whose details were torn is never adopted, and one with only one
   // good copy of its details is written again before a boot goes by it
   begin_test("S6", "epoch details survive one bad copy, a torn first copy and a torn second copy");
   static uint8_t details[200], got[200];
   for (uint32_t i = 0; i < sizeof(details); ++i)
      details[i] = (uint8_t)((i * 37) ^ 0x5A);

   // One copy's details unreadable, then a failed program at the head, which writes the slot again
   fresh_epoch();
   nandlog_begin_session();
   nandlog_begin_epoch(details, sizeof(details));
   nandlog_end_session();
   write_pages(5, 0);
   uint32_t slots[8];
   newest_slot_pages(slots, 8);
   nandlog_sim_corrupt(slots[0], 70, 0xFF);
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_pages[faults.num_unwritable_pages++] = (epoch_start_block() * ppb()) + 5;
   set_faults(&faults);
   write_pages(3, 100);
   clear_faults();
   reboot();
   nandlog_begin_session();
   nandlog_retrieve_epoch_details(got, sizeof(got));
   nandlog_end_session();
   CHECK(!memcmp(got, details, sizeof(details)), "the slot written after a failed program lost details only one copy had lost");

   // A cut in the first copy of the new epoch's slot, and in the second
   uint32_t lost_first = 0, lost_second = 0;
   for (uint32_t copy = 1; copy <= 2; ++copy)
      for (uint64_t seed = 1; seed <= 20; ++seed)
      {
         fresh_epoch();
         write_pages(5, 0);
         nandlog_sim_seed(seed);
         nandlog_sim_power_cut(copy, NANDLOG_SIM_CUT_BITWISE, 0.999, &cut_jump);
         if (setjmp(cut_jump) == 0)
         {
            nandlog_begin_session();
            nandlog_begin_epoch(details, sizeof(details));
            nandlog_end_session();
         }
         nandlog_sim_power_cut_disarm();
         reboot();
         if (copy == 2)
         {
            // One later unreadable ring page, whichever copy of the details it takes, must not lose them
            nandlog_deinit();
            uint32_t whole[8], wholes = 0;
            const uint32_t found = newest_slot_pages(slots, 8);
            for (uint32_t i = 0; i < found; ++i)
            {
               nandlog_meta_header_t header;
               const uint8_t *raw = nandlog_sim_raw_page(slots[i]);
               memcpy(&header, raw, sizeof(header));
               if (crc32_bits(raw + sizeof(nandlog_meta_header_t) + sizeof(nandlog_meta_extension_t), header.details_length) == header.details_crc)
                  whole[wholes++] = slots[i];
            }
            if (wholes)
               rot_page(whole[seed % wholes]);
            reboot();
         }
         nandlog_epoch_info_t info;
         nandlog_begin_session();
         nandlog_epoch_info(0, &info);
         memset(got, 0, sizeof(got));
         nandlog_retrieve_epoch_details(got, sizeof(got));
         nandlog_end_session();
         if ((info.epoch == 2) && memcmp(got, details, sizeof(details)))
            ++*((copy == 1) ? &lost_first : &lost_second);
      }
   CHECK(!lost_first, "%u of 20 cuts in the first copy left the new epoch current with details that do not read back", lost_first);
   CHECK(!lost_second, "%u of 20 cuts in the second copy, and one unreadable ring page later, lost the details", lost_second);
   end_test();
}

static void test_s8_one_copy_is_still_written(void)
{
   // Every pair the ring offers takes its first copy and refuses its second. The slot is on the part once, and a
   // reboot goes by it, so beginning the epoch must report success
   begin_test("S8", "an epoch whose slot only one copy of could be written is reported begun, as a reboot finds it");
   fresh_epoch();
   write_pages(3, 0);
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_pages[faults.num_unwritable_pages++] = 3;
   for (uint32_t block = 1; block < METADATA_RING_BLOCKS; ++block)
      faults.unwritable_pages[faults.num_unwritable_pages++] = (block * ppb()) + 1;
   set_faults(&faults);
   const bool begun = begin_epoch("S8");
   clear_faults();
   reboot();
   nandlog_epoch_info_t info;
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   CHECK(begun == (info.epoch == 2), "begin_epoch reported %s, and the reboot went by epoch %u", begun ? "success" : "failure", info.epoch);
   end_test();
}

static void test_q2_sequence_numbers_identify_pages(void)
{
   // What a read served under a sequence number is what that sequence number holds from then on, or a gap where the
   // part lost it: never other records. Without a fault, with a cut before the next commit, and with a cut in the
   // boot that recovers a delivered page that rotted
   begin_test("Q2", "a sequence number a read has served is never given to other records");
   nandlog_page_header_t header;
   uint32_t id = 0;
   for (uint32_t cut = 0; cut < 2; ++cut)
   {
      fresh_epoch();
      write_pages(3, 0);
      store(100, 40);
      nandlog_begin_session();
      nandlog_begin_reading(0, 0);
      uint32_t pages = 0, served = 0, served_seq = 0, served_records = 0;
      nandlog_read_span(&pages, NULL);
      for (uint32_t i = 0; i < pages; ++i)
      {
         served = nandlog_retrieve_next_page(readback, &header);
         served_seq = header.seq;
         served_records = header.record_count;
      }
      nandlog_end_reading();
      nandlog_end_session();
      store(101, 40);
      if (cut)
         reboot();
      store(102, 40);
      flush(true);
      const uint32_t length = page_by_seq(served_seq, &id);
      nandlog_begin_session();
      nandlog_retrieve_page_by_seq(served_seq, readback, &header);
      nandlog_end_session();
      CHECK((length == served) && (id == 100) && (header.record_count == served_records),
            "%s: sequence %u was served with %u bytes and %u record(s), and now holds %u bytes, %u record(s), first id %u",
            cut ? "a reboot before the next commit" : "no fault", served_seq, served, served_records, length, header.record_count, id);
   }

   // A delivered page at a block's first position rots; the recovering boot is cut at each of its operations
   fresh_epoch();
   write_pages(ppb() + 1, 0);
   reboot();
   const uint32_t page = (epoch_start_block() * ppb()) + ppb();
   nandlog_deinit();
   rot_page(page);
   nandlog_sim_snapshot();
   nandlog_sim_reset_operations();
   reboot();
   const uint32_t operations = nandlog_sim_operations();
   uint32_t reissued = 0;
   for (uint32_t k = 1; k <= operations; ++k)
   {
      nandlog_deinit();
      nandlog_sim_restore();
      nandlog_sim_power_cut(k, NANDLOG_SIM_CUT_BOUNDARY, 0.5, &cut_jump);
      if (setjmp(cut_jump) == 0)
         nandlog_init();
      nandlog_sim_power_cut_disarm();
      reboot();
      write_pages(1, 9000);
      const uint32_t length = page_by_seq(ppb(), &id);
      reissued += (length && (id != ppb())) ? 1 : 0;
   }
   CHECK(!reissued, "%u of %u cuts in the recovering boot gave the rotted page's sequence number to new records", reissued, operations);
   end_test();
}

// Found by the Third Adversarial Tester ------------------------------------------------------------------------------
//
// A third adversarial tester found what follows. Each test fails against the code that tester read and passes now.
// Tag: U

static bool cut_next_flush(uint32_t id, uint32_t length)
{
   // Store a record and flush it with power cut at the first program or erase, bitwise model; true if it fired
   store(id, length);
   for (uint32_t i = 0; i < 4; ++i)
      flush(false);
   nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_BITWISE, 0.5, &cut_jump);
   if (setjmp(cut_jump) == 0)
      flush(true);
   nandlog_sim_power_cut_disarm();
   return nandlog_sim_power_failed();
}

static void test_u1_torn_slot_keeps_the_list(void)
{
   // Positions torn and listed by two boots, pages committed after them, the next commit torn too, and the boot that
   // lists it cut at the first copy of its slot, under the unstable model. Whatever the boot after makes of that
   // copy, the positions listed before stay listed: when the torn pages drift blank, nothing committed is lost
   begin_test("U1", "a cut in the slot that lists a torn page never drops or rolls back the positions listed before");
   nandlog_sim_seed(1);
   new_part();
   factory_mark(log_end_block() - 1);
   reboot();
   begin_epoch("u1");
   const uint32_t run = ((log_blocks() - 1) * ppb()) / 2, after = 150;
   write_pages(run, 0);
   bool placed = cut_next_flush(30000, 60);
   reboot();
   placed = placed && cut_next_flush(30001, 60);
   reboot();
   write_pages(after, 20000);
   placed = placed && cut_next_flush(30002, 60);
   nandlog_deinit();
   nandlog_sim_snapshot();
   uint32_t lost = 0, again = 0, trials = 0;
   for (uint64_t seed = 21; seed <= 60; ++seed, ++trials)
   {
      nandlog_deinit();
      nandlog_sim_restore();
      nandlog_sim_seed(seed);
      nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_UNSTABLE, 0.995, &cut_jump);
      if (setjmp(cut_jump) == 0)
         nandlog_init();
      nandlog_sim_power_cut_disarm();
      reboot();
      nandlog_deinit();
      nandlog_sim_drift(NANDLOG_SIM_DRIFT_BLANK, 1.0);
      nandlog_sim_reset_counters();
      reboot();
      write_pages(1, 30100);
      again += nandlog_sim_counters().drifted_reprograms;
      reboot();
      audit_current();
      lost += (ids_seen(20000, after) != after) ? 1 : 0;
   }
   CHECK(placed && !lost && !again, "%u of %u trials lost pages committed after the listed positions; %u drifted pages programmed again", lost, trials, again);
   end_test();
}

static void test_u2_full_list_seals_the_epoch(void)
{
   // Torn commits one boot after another until the passed-over list is full. The epoch stops taking pages then, and
   // goes on refusing them after its torn pages drift blank and the device reboots: the refusal is in the ring
   begin_test("U2", "an epoch whose passed-over list is full stays refused across reboots, until a new epoch");
   new_part();
   reboot();
   begin_epoch("u2");
   uint32_t id = 0, boots = 0;
   int refused = NANDLOG_OK;
   while ((refused == NANDLOG_OK) && (boots < 1200))
   {
      store(id++, page_record_bytes());
      store(id++, page_record_bytes());
      flush(true);
      cut_next_flush(id++, 1000);
      reboot();
      ++boots;
      refused = store(id++, 8);
      if (refused == NANDLOG_OK)
         flush(true);
   }
   nandlog_deinit();
   nandlog_sim_drift(NANDLOG_SIM_DRIFT_BLANK, 1.0);
   nandlog_sim_reset_counters();
   reboot();
   const int stored = store(39000, 16), flushed = flush(true);
   const uint32_t again = nandlog_sim_counters().drifted_reprograms;
   const bool begun = begin_epoch("u2b");
   const int after = store(39001, 16);
   CHECK((refused != NANDLOG_OK) && (stored == refused) && (flushed == refused) && !again && begun && (after == NANDLOG_OK),
         "after %u boots the list filled (store %d); after a drift and a reboot store %d, flush %d, %u drifted pages programmed again; a new epoch %s",
         boots, refused, stored, flushed, again, begun ? "takes records" : "was refused");
   end_test();
}

#ifdef SIM_CHIP_WINBOND
static void test_u7_stranded_records_counted(void)
{
   // The window's next block will not erase, and the driver cannot retire it -- the part's remap table is full --
   // inside a call that committed nothing: the log stops taking pages with records still buffered. They can never
   // be committed, so they are counted lost then, not left stranded
   begin_test("U7", "records buffered when the log stops taking pages are counted lost");
   new_part();
   for (uint32_t b = 0; b < 20; ++b)
      factory_mark(log_end_block() - 1 - b);
   reboot();
   begin_epoch("u7");
   write_pages(ppb() + 31, 0);
   nandlog_sim_faults_t faults = no_faults();
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = log_first_block() + 3;
   set_faults(&faults);
   nandlog_stats_t before, stats;
   nandlog_get_stats(&before);
   for (uint32_t i = 0; (i < 2000) && !nandlog_sim_unwritable_reported(); ++i)
      store(1000 + (i % 1000), 100);
   const int flushed = flush(true);
   nandlog_get_stats(&stats);
   CHECK(nandlog_sim_unwritable_reported() && !nandlog_has_buffered_data() && (flushed != NANDLOG_OK) &&
         ((stats.records_lost - before.records_lost) + ((stats.pages_committed - before.pages_committed) ? 1 : 0) > 0),
         "the log %s; %s buffered; %u records lost", nandlog_sim_unwritable_reported() ? "stopped" : "never stopped",
         nandlog_has_buffered_data() ? "records still" : "nothing", stats.records_lost - before.records_lost);
   end_test();
}
#endif

static void test_u8_adoption_past_an_unreadable_first_page(void)
{
   // Both copies of the newer epoch's slot rot, and so does the header of its first page. Its other pages still say
   // where it began, and no boot may erase them as the older epoch's window
   begin_test("U8", "an epoch whose slot and first page are both unreadable is found from its later pages");
   new_part();
   reboot();
   begin_epoch("E1");
   write_pages(20, 0);
   begin_epoch("E2");
   const uint32_t start = epoch_start_block();
   write_pages(10, 100);
   nandlog_deinit();
   uint32_t slots[8];
   const uint32_t copies = newest_slot_pages(slots, 8);
   for (uint32_t i = 0; i < copies; ++i)
      rot_page(slots[i]);
   rot_page(start * ppb());
   reboot();
   write_pages(5, 200);
   reboot();
   nandlog_epoch_info_t info;
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   audit_current();
   CHECK((info.epoch == 2) && (ids_seen(101, 9) == 9) && (ids_seen(200, 5) == 5), "epoch %u current; %u of 9 readable pages of epoch 2 and %u of 5 more delivered",
         info.epoch, ids_seen(101, 9), ids_seen(200, 5));
   end_test();
}

#ifndef SIM_CHIP_WINBOND
static void test_u3_cut_before_a_mark(void)
{
   // A program fails, the block is relocated and retired, and power goes at each operation of that in turn --
   // between the table's new version and the block's spare-area mark among them. After a reboot, more pages, and
   // the recovery utility, the block is still retired and nothing committed is lost
   begin_test("U3", "a cut anywhere in a retirement, then the recovery utility, keeps the block retired");
   fresh_epoch();
   const uint32_t failing = epoch_start_block();
   write_pages(10, 0);
   nandlog_deinit();
   nandlog_sim_snapshot();
   uint32_t forgotten = 0, lost = 0, trials = 0;
   for (uint32_t n = 1; n < 60; ++n)
   {
      nandlog_sim_restore();
      clear_faults();
      reboot();
      nandlog_sim_faults_t faults = no_faults();
      faults.unwritable_pages[faults.num_unwritable_pages++] = (failing * ppb()) + 10;
      set_faults(&faults);
      store(10, page_record_bytes());
      nandlog_sim_power_cut(n, NANDLOG_SIM_CUT_BOUNDARY, 0.5, &cut_jump);
      if (setjmp(cut_jump) == 0)
         flush(true);
      nandlog_sim_power_cut_disarm();
      if (!nandlog_sim_power_failed())
         break;
      clear_faults();
      reboot();
      write_pages(20, 100);
      reboot();
      nandlog_begin_session();
      nandlog_reset_bad_block_table();
      nandlog_end_session();
      reboot();
      write_pages(2, 200);
      reboot();
      audit_current();
      ++trials;
      forgotten += (nandlog_bad_block_count() && !nandlog_chip_is_bad_block(failing * ppb())) ? 1 : 0;
      lost += ((ids_seen(0, 10) != 10) || (ids_seen(100, 20) != 20) || (ids_seen(200, 2) != 2)) ? 1 : 0;
   }
   CHECK(trials && !forgotten && !lost, "of %u cuts, %u left the block forgotten by the utility and %u lost committed pages", trials, forgotten, lost);
   end_test();
}

static void test_u4_rotted_first_page_is_no_mark(void)
{
   // The first page of a block in the epoch rots past what the part can correct, and the table is then rebuilt from
   // the marks. That is not a mark: the block stays in use, and so does every position after it
   begin_test("U4", "a rebuild from the marks does not retire a block whose first page only reads uncorrectable");
   fresh_epoch();
   const uint32_t block = epoch_start_block() + 1;
   write_pages(200, 0);
   rot_page(block * ppb());
   reboot();
   nandlog_begin_session();
   nandlog_reset_bad_block_table();
   nandlog_end_session();
   reboot();
   write_pages(5, 1000);
   reboot();
   const audit_t a = audit_current();
   CHECK(!nandlog_chip_is_bad_block(block * ppb()) && (ids_seen(0, 200) == 199) && (ids_seen(1000, 5) == 5) && !a.damaged,
         "block %u is %s; %u of 199 readable pages and %u of 5 more delivered", block, nandlog_chip_is_bad_block(block * ppb()) ? "retired" : "in use",
         ids_seen(0, 200), ids_seen(1000, 5));
   end_test();
}

static void test_u5_reset_keeps_factory_marks(void)
{
   // Two blocks of the reserve are factory-bad. The recovery utility must not erase them -- an erase would erase the
   // factory's mark -- so the rebuilt table still names them, and is not written into one
   begin_test("U5", "the recovery utility leaves marked blocks of the reserve, and their marks, alone");
   new_part();
   const uint32_t first = log_end_block();
   factory_mark(first);
   factory_mark(first + 1);
   reboot();
   nandlog_begin_session();
   nandlog_reset_bad_block_table();
   nandlog_end_session();
   reboot();
   reboot();
   uint32_t written = 0;
   for (uint32_t b = first; b < (first + 2); ++b)
      for (uint32_t p = 0; p < ppb(); ++p)
         written += memcmp(nandlog_sim_raw_page((b * ppb()) + p), "BBT3", 4) ? 0 : 1;
   CHECK(nandlog_chip_is_bad_block(first * ppb()) && nandlog_chip_is_bad_block((first + 1) * ppb()) && !written,
         "after the utility the reserve's factory-bad blocks are %s and %s, with %u table pages written into them",
         nandlog_chip_is_bad_block(first * ppb()) ? "retired" : "forgotten", nandlog_chip_is_bad_block((first + 1) * ppb()) ? "retired" : "forgotten", written);
   end_test();
}

static void test_u6_logging_on_after_a_reset(void)
{
   // The recovery utility, then logging in the same boot, during which a block is retired. The table that
   // retirement writes must still name every block the old one did
   begin_test("U6", "logging on after the recovery utility, through a retirement, keeps every retired block");
   new_part();
   factory_mark(log_first_block() + 20);
   factory_mark(log_first_block() + 40);
   reboot();
   begin_epoch("u6");
   write_pages(10, 0);
   nandlog_begin_session();
   nandlog_reset_bad_block_table();
   nandlog_end_session();
   const uint32_t failing = epoch_start_block();
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_pages[faults.num_unwritable_pages++] = (failing * ppb()) + 10;
   set_faults(&faults);
   write_pages(10, 100);
   clear_faults();
   reboot();
   const int stored = store(300, page_record_bytes());
   flush(true);
   reboot();
   audit_current();
   CHECK(nandlog_chip_is_bad_block((log_first_block() + 20) * ppb()) && nandlog_chip_is_bad_block((log_first_block() + 40) * ppb()) &&
         nandlog_chip_is_bad_block(failing * ppb()) && !stored && (ids_seen(0, 10) == 10) && (ids_seen(100, 10) == 10) && (ids_seen(300, 1) == 1),
         "%u blocks retired after the reboot; store returned %d; %u, %u and %u pages delivered", nandlog_bad_block_count(), stored,
         ids_seen(0, 10), ids_seen(100, 10), ids_seen(300, 1));
   end_test();
}
#endif

static void test_u9_cut_rewrite_is_done_again(void)
{
   // A commit at the first page of a block torn so that its header checks over a payload that does not; the boot
   // that lists it erases the block again to tombstone it, and a cut barely starts that erase. The boot after must
   // not take the block as it stands: pages it then commits there would drift, the block's last erase never having
   // finished. Seeds are drawn until the tear and the cut erase both leave the header checking. Found by the
   // campaign's ring trace in round 4, not by the tester
   begin_test("U9", "a cut in the erase that tombstones a listed block-start page is done again by the next boot");
   uint32_t trials = 0, lost = 0, again = 0;
   for (uint64_t seed = 1; (seed <= 400) && (trials < 8); ++seed)
   {
      new_part();
      reboot();
      begin_epoch("u9");
      write_pages(ppb(), 0);
      const uint32_t torn = (epoch_start_block() + 1) * ppb();
      store(500, page_record_bytes());
      nandlog_sim_seed(seed);
      nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_BITWISE, 0.995, &cut_jump);
      if (setjmp(cut_jump) == 0)
         flush(true);
      nandlog_sim_power_cut_disarm();
      nandlog_page_header_t header;
      if (!raw_page_valid(torn, &header, false) || raw_page_valid(torn, &header, true))
         continue;
      nandlog_deinit();
      nandlog_sim_seed(seed * 7);
      nandlog_sim_power_cut(3, NANDLOG_SIM_CUT_BITWISE, 0.0005, &cut_jump);
      if (setjmp(cut_jump) == 0)
         nandlog_init();
      nandlog_sim_power_cut_disarm();
      nandlog_sim_cut_t cut;
      if (!nandlog_sim_last_cut(&cut) || (cut.command != 0xD8) || (cut.page != torn) || !raw_page_valid(torn, &header, false))
         continue;
      ++trials;
      reboot();
      write_pages(40, 1000);
      nandlog_deinit();
      nandlog_sim_drift(NANDLOG_SIM_DRIFT_BLANK, 1.0);
      nandlog_sim_reset_counters();
      reboot();
      write_pages(1, 2000);
      again += nandlog_sim_counters().drifted_reprograms;
      reboot();
      audit_current();
      lost += ((ids_seen(0, ppb()) != ppb()) || (ids_seen(1000, 40) != 40) || (ids_seen(2000, 1) != 1)) ? 1 : 0;
   }
   CHECK((trials == 8) && !lost && !again, "%u of %u trials lost committed pages; %u drifted pages programmed again", lost, trials, again);
   end_test();
}

static void test_u10_boot_relocation_cut_before_its_record(void)
{
   // A torn page at the start of a block, listed; the block will not erase again, so the boot moves it and retires
   // it. A cut anywhere in that -- between the table's new version and the ring's record of the retirement
   // included -- must not leave a table the next boot takes for one that lays the epoch out differently. Found in
   // review of the fixes for the third tester's findings, which added the check that refused
   begin_test("U10", "a cut in a boot's relocation of the head's block never seals the epoch or loses a page");
   new_part();
   reboot();
   begin_epoch("u10");
   write_pages(ppb(), 0);
   const uint32_t block = epoch_start_block() + 1;
   const bool torn = cut_next_flush(500, 60);
   nandlog_deinit();
   nandlog_sim_snapshot();
   uint32_t cuts = 0, refused = 0, lost = 0;
   for (uint32_t n = 1; n <= 40; ++n)
   {
      nandlog_deinit();
      nandlog_sim_restore();
      nandlog_sim_faults_t faults = no_faults();
      faults.unerasable_blocks[faults.num_unerasable_blocks++] = block;
      set_faults(&faults);
      nandlog_sim_power_cut(n, NANDLOG_SIM_CUT_BOUNDARY, 0.5, &cut_jump);
      if (setjmp(cut_jump) == 0)
         nandlog_init();
      nandlog_sim_power_cut_disarm();
      if (!nandlog_sim_power_failed())
         break;
      ++cuts;
      reboot();
      const int stored = store(1000 + n, page_record_bytes());
      flush(true);
      refused += (stored != NANDLOG_OK) ? 1 : 0;
      reboot();
      audit_current();
      lost += ((ids_seen(0, ppb()) != ppb()) || (!stored && (ids_seen(1000 + n, 1) != 1))) ? 1 : 0;
   }
   clear_faults();
   CHECK(torn && (cuts >= 8) && !refused && !lost, "of %u cuts, %u left records refused (the epoch sealed) and %u lost pages", cuts, refused, lost);
   end_test();
}


// Found by the Fourth Adversarial Tester ------------------------------------------------------------------------------
//
// A fourth adversarial tester found what follows. Each test fails against the code as that tester found it and
// passes now. Tag: X

static void test_x1_torn_block_entry_erase(void)
{
   // Page-sized records, so every store commits the page before it and no call carries the erase-ahead: the head
   // enters each block by erasing it inside a commit. The region has wrapped, so the block holds an earlier epoch's
   // pages, and a cut late in that erase, under the model in which undecided bits read differently every time,
   // leaves a block that can read one way to the head search and another to the boot's next look. The boot must not
   // program into it without erasing it again: pages committed after the reboot must survive the drift
   begin_test("X1", "a cut in the erase that enters a block never lets the boot program the block unerased");
   new_part();
   reboot();
   for (uint32_t e = 0; e < (log_blocks() + 4); ++e)
   {
      begin_epoch("x1");
      store(e * 2, page_record_bytes());
      store((e * 2) + 1, 200);
      flush(true);
   }
   begin_epoch("x1 last");
   for (uint32_t i = 0; i < ((2 * ppb()) + 3); ++i)
      store(10000 + i, page_record_bytes());
   nandlog_deinit();
   nandlog_sim_snapshot();
   static const double qs[4] = { 0.995, 0.998, 0.999, 0.9995 };
   uint32_t erases = 0, trials = 0, lost = 0, again = 0;
   for (uint32_t c = 1; (c < 400) && (erases < 2); ++c)
   {
      // The erases in the stream, found by cutting before each operation in turn
      nandlog_deinit();
      nandlog_sim_restore();
      reboot();
      nandlog_sim_reset_operations();
      nandlog_sim_power_cut(c, NANDLOG_SIM_CUT_BOUNDARY, 0.5, &cut_jump);
      uint32_t id = 30000;
      if (setjmp(cut_jump) == 0)
         for (uint32_t i = 0; i < (4 * ppb()); ++i)
            store(id++, page_record_bytes());
      nandlog_sim_power_cut_disarm();
      nandlog_sim_cut_t cut;
      if (!nandlog_sim_power_failed() || !nandlog_sim_last_cut(&cut) || (cut.command != 0xD8) || ((cut.page / ppb()) < log_first_block()))
         continue;
      ++erases;
      for (uint64_t seed = 1; seed <= 24; ++seed)
         for (uint32_t qi = 0; qi < 4; ++qi)
         {
            nandlog_deinit();
            nandlog_sim_restore();
            nandlog_sim_seed((seed * 104729) + qi + c);
            reboot();
            nandlog_sim_reset_operations();
            nandlog_sim_power_cut(c, NANDLOG_SIM_CUT_UNSTABLE, qs[qi], &cut_jump);
            id = 30000;
            if (setjmp(cut_jump) == 0)
               for (uint32_t i = 0; i < (4 * ppb()); ++i)
                  store(id++, page_record_bytes());
            nandlog_sim_power_cut_disarm();
            if (!nandlog_sim_power_failed())
               continue;
            ++trials;
            reboot();
            write_pages(40, 20000);
            nandlog_deinit();
            nandlog_sim_reset_counters();
            nandlog_sim_drift(NANDLOG_SIM_DRIFT_BLANK, 1.0);
            reboot();
            audit_current();
            lost += (ids_seen(20000, 40) != 40) ? 1 : 0;
            write_pages(10, 21000);
            again += nandlog_sim_counters().drifted_reprograms ? 1 : 0;
         }
   }
   CHECK((erases == 2) && trials && !lost && !again, "of %u trials over %u erases, %u lost pages committed after the reboot and %u programmed a drifted page",
         trials, erases, lost, again);
   end_test();
}

static void test_x4_lost_newer_epoch_after_a_seal(void)
{
   // An epoch sealed, the new epoch the seal asks for begun and logged into, and then every copy of the new epoch's
   // slot lost: the boot goes by the sealed slot before it. It must still find the newer epoch from its own pages,
   // and a later epoch must never be given its number
   begin_test("X4", "a newer epoch whose slot is lost is found from its pages even when the slot before it is sealed");
   new_part();
   reboot();
   begin_epoch("x4 one");
   write_pages(20, 0);
   cut_next_flush(90, 60);
   reboot();                                   // lists the torn page: the slot now carries a list
   write_pages(2, 100);
   cut_next_flush(91, 60);
   reboot();                                   // and again, so the slot before the newest carries one too
   write_pages(2, 110);
   nandlog_deinit();
   uint32_t slots[16];
   const uint32_t copies = newest_slot_pages(slots, 16);
   for (uint32_t i = 0; i < copies; ++i)
   {
      // The list of passed-over positions unreadable in every copy of the epoch's slots. The newest pair, the last
      // written, may be one a cut tore, so the boot looks to the pair before it; that one was acted on, and with
      // its list gone the epoch is sealed
      nandlog_meta_header_t header;
      memcpy(&header, nandlog_sim_raw_page(slots[i]), sizeof(header));
      nandlog_sim_corrupt(slots[i], 64 + header.details_length, 0xFF);
      nandlog_sim_corrupt(slots[i], 64 + header.details_length + 1, 0xFF);
   }
   reboot();
   const int sealed = store(200, 16);
   begin_epoch("x4 two");
   const uint32_t second = current_epoch();
   write_pages(150, 1000);
   reboot();
   nandlog_deinit();
   const uint32_t lost = newest_slot_pages(slots, 16);
   for (uint32_t i = 0; i < lost; ++i)
      rot_page(slots[i]);
   reboot();
   const uint32_t after = current_epoch();
   audit_current();
   const uint32_t delivered = ids_seen(1000, 150);
   begin_epoch("x4 three");
   const uint32_t third = current_epoch();
   write_pages(1, 2000);
   reboot();
   audit_current();
   CHECK((sealed == NANDLOG_ERR_SEALED) && (second == 2) && (after == 2) && (delivered == 150) && (third == 3) &&
         (ids_seen(2000, 1) == 1) && !ids_seen(1000, 150),
         "first epoch store %d; second epoch %u, current after its slot was lost %u with %u of 150 pages; the next epoch "
         "numbered %u, serving %u of the second's pages as its own", sealed, second, after, delivered, third, ids_seen(1000, 150));
   end_test();
}

#ifndef SIM_CHIP_WINBOND
static uint32_t reserve_block(void) { return log_end_block(); }

static void test_x2_reset_never_rolls_the_table_back(void)
{
   // The table moves to a second candidate block (a page of its block refuses a program); later the recovery
   // utility runs and the older candidate block refuses its erase. The reboot must not load the older version: a
   // retirement would be forgotten, the epoch sealed, and its reads would lose committed records
   begin_test("X2", "the recovery utility never leaves an older table version to be loaded");
   new_part();
   reboot();
   begin_epoch("x2");
   write_pages(100, 0);
   const uint32_t bad1 = log_first_block() + 4, bad2 = log_first_block() + 8;
   nandlog_sim_faults_t faults = no_faults();
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = bad1;
   faults.unwritable_pages[faults.num_unwritable_pages++] = ((reserve_block() + 1) * ppb()) + 2;
   set_faults(&faults);
   write_pages(100, 1000);
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = bad2;
   set_faults(&faults);
   write_pages(250, 2000);
   clear_faults();
   reboot();
   const bool retired = nandlog_chip_is_bad_block(bad1 * ppb()) && nandlog_chip_is_bad_block(bad2 * ppb());
   nandlog_deinit();
   nandlog_sim_snapshot();
   reboot();
   faults = no_faults();
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = reserve_block() + 1;
   set_faults(&faults);
   nandlog_begin_session();
   nandlog_reset_bad_block_table();
   nandlog_end_session();
   clear_faults();
   reboot();
   const int stored = store(50000, 16);
   flush(true);
   audit_current();
   const uint32_t delivered = ids_seen(0, 100) + ids_seen(1000, 100) + ids_seen(2000, 250);
   CHECK(retired && nandlog_chip_is_bad_block(bad1 * ppb()) && nandlog_chip_is_bad_block(bad2 * ppb()) && (stored == NANDLOG_OK) && (delivered == 450),
         "after the utility and a reboot: blocks %u and %u retired %d and %d; store %d; %u of 450 committed pages delivered", bad1, bad2,
         nandlog_chip_is_bad_block(bad1 * ppb()), nandlog_chip_is_bad_block(bad2 * ppb()), stored, delivered);

   // A cut at every operation of the utility, on the same part as it was before it: never an older version after
   uint32_t cuts = 0, forgotten = 0;
   for (uint32_t n = 1; n <= 16; ++n)
   {
      nandlog_deinit();
      nandlog_sim_restore();
      reboot();
      nandlog_sim_reset_operations();
      nandlog_sim_power_cut(n, NANDLOG_SIM_CUT_BOUNDARY, 0.5, &cut_jump);
      if (setjmp(cut_jump) == 0)
      {
         nandlog_begin_session();
         nandlog_reset_bad_block_table();
         nandlog_end_session();
      }
      nandlog_sim_power_cut_disarm();
      if (!nandlog_sim_power_failed())
         break;
      ++cuts;
      reboot();
      forgotten += (!nandlog_chip_is_bad_block(bad1 * ppb()) || !nandlog_chip_is_bad_block(bad2 * ppb()) || (store(60000 + n, 16) != NANDLOG_OK)) ? 1 : 0;
   }
   CHECK(cuts && !forgotten, "of %u cuts in the utility, %u left a retirement forgotten or the epoch sealed", cuts, forgotten);
   end_test();
}

static void test_x3_reset_keeps_an_unmarked_retirement(void)
{
   // A block that refuses every program -- its spare-area mark included -- is relocated and retired. The recovery
   // utility must not discard a table whose rebuild from the marks would forget it
   begin_test("X3", "the recovery utility never forgets a retired block whose mark would not take");
   new_part();
   reboot();
   begin_epoch("x3");
   write_pages(70, 0);
   const uint32_t failing = epoch_start_block() + 1;
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = failing;
   set_faults(&faults);
   write_pages(10, 1000);
   write_pages(200, 2000);
   reboot();
   nandlog_begin_session();
   nandlog_reset_bad_block_table();
   nandlog_end_session();
   reboot();
   const int stored = store(50000, 16);
   flush(true);
   audit_current();
   const uint32_t delivered = ids_seen(0, 70) + ids_seen(1000, 10) + ids_seen(2000, 200);
   clear_faults();
   CHECK(nandlog_chip_is_bad_block(failing * ppb()) && (stored == NANDLOG_OK) && (delivered == 280),
         "after the utility and a reboot: block %u retired %d; store %d; %u of 280 committed pages delivered", failing,
         nandlog_chip_is_bad_block(failing * ppb()), stored, delivered);
   end_test();
}
#endif


// Found by the Fifth Adversarial Tester -------------------------------------------------------------------------------
//
// A fifth adversarial tester found what follows: decisions taken from more than one read of a page a cut may have
// left unstable. Each test fails against the code that tester read and passes now. Tag: Y

static uint64_t mix_seed(uint64_t z)
{
   // splitmix64, so consecutive seeds give the simulator's generator well-separated states
   z += 0x9E3779B97F4A7C15ULL;
   z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
   z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
   return z ^ (z >> 31);
}

#ifndef SIM_CHIP_WINBOND
static void test_y1_table_version_from_one_read(void)
{
   // The first copy of the first pair of a new table block torn under the unstable model: its version bits can read
   // differently from one latch to the next. Judged from one read, the page either checks whole or does not; judged
   // from two, it could read valid with a version its checksum never covered, outrank the real newest block, and
   // roll the table back -- forgetting a retirement the ring recorded
   begin_test("Y1", "a torn table page is judged from one read, so it never rolls the table back");
   uint32_t trials = 0, rollbacks = 0;
   const uint32_t table_a = log_end_block() + 1, table_b = table_a + 1, table_c = table_a + 2;
   for (uint64_t seed = 40; seed <= 60; ++seed)
   {
      new_part();
      nandlog_sim_faults_t faults = no_faults();
      faults.unerasable_blocks[faults.num_unerasable_blocks++] = 9;
      set_faults(&faults);
      reboot();
      begin_epoch("y1");
      if (!nandlog_chip_is_bad_block(9 * ppb()))
         continue;
      faults.unerasable_blocks[faults.num_unerasable_blocks++] = 11;
      faults.unwritable_pages[faults.num_unwritable_pages++] = (table_a * ppb()) + 4;
      set_faults(&faults);
      nandlog_deinit();
      nandlog_sim_snapshot();
      bool cut_in_b = false;
      for (uint32_t cut_at = 1; (cut_at < 20) && !cut_in_b; ++cut_at)
      {
         nandlog_sim_restore();
         nandlog_sim_seed(mix_seed(seed));
         nandlog_sim_power_cut(cut_at, NANDLOG_SIM_CUT_UNSTABLE, 0.99, &cut_jump);
         if (setjmp(cut_jump) == 0)
         {
            reboot();
            nandlog_sim_power_cut_disarm();
            continue;
         }
         nandlog_sim_power_cut_disarm();
         nandlog_sim_cut_t cut;
         nandlog_sim_last_cut(&cut);
         cut_in_b = (cut.command == 0x10) && !cut.otp && (cut.page == (table_b * ppb()));
      }
      if (!cut_in_b)
         continue;
      reboot();
      if (memcmp(nandlog_sim_raw_page(table_c * ppb()), "BBT3", 4))
         continue;
      ++trials;
      faults.unerasable_blocks[faults.num_unerasable_blocks++] = 12;
      set_faults(&faults);
      reboot();
      if (!nandlog_chip_is_bad_block(12 * ppb()))
         continue;
      for (uint32_t boot = 0; boot < 200; ++boot)
      {
         reboot();
         if (!nandlog_chip_is_bad_block(12 * ppb()))
         {
            ++rollbacks;
            break;
         }
      }
   }
   // The boot's window reaches block 11 only if it is at least two blocks long, the first, block 9, being retired
   clear_faults();
   CHECK((trials || (NANDLOG_ERASE_AHEAD_BLOCKS < 2)) && !rollbacks, "of %u trials in which a boot took the torn table page, %u rolled the table back", trials, rollbacks);
   end_test();
}

static void test_y2_torn_mark_programmed_again(void)
{
   // A cut in the spare-area mark of a block a window erase retired, under the unstable model: the mark can read as
   // a mark at one check and as no mark at the next. Only a mark that reads exactly 00h counts; anything else is
   // programmed again, which completes it, so the recovery utility's rebuild from the marks keeps the block
   begin_test("Y2", "a torn bad-block mark is programmed again, so the recovery utility never forgets its block");
   uint32_t trials = 0, forgotten = 0;
   for (uint64_t seed = 1; seed <= 150; ++seed)
   {
      new_part();
      reboot();
      nandlog_deinit();
      nandlog_sim_snapshot();
      bool cut_in_mark = false;
      for (uint32_t cut_at = 1; (cut_at < 12) && !cut_in_mark; ++cut_at)
      {
         nandlog_sim_restore();
         reboot();
         nandlog_sim_faults_t faults = no_faults();
         faults.unerasable_blocks[faults.num_unerasable_blocks++] = 9;
         set_faults(&faults);
         nandlog_sim_seed(mix_seed(seed));
         nandlog_begin_session();
         nandlog_sim_power_cut(cut_at, NANDLOG_SIM_CUT_UNSTABLE, 0.01, &cut_jump);
         if (setjmp(cut_jump) == 0)
         {
            nandlog_begin_epoch("y2", 2);
            nandlog_sim_power_cut_disarm();
            nandlog_end_session();
            continue;
         }
         nandlog_sim_power_cut_disarm();
         nandlog_sim_cut_t cut;
         nandlog_sim_last_cut(&cut);
         cut_in_mark = (cut.command == 0x10) && !cut.otp && (cut.page == (9 * ppb()));
      }
      if (!cut_in_mark)
         continue;
      reboot();
      if (!nandlog_chip_is_bad_block(9 * ppb()))
         continue;
      ++trials;
      nandlog_begin_session();
      nandlog_reset_bad_block_table();
      nandlog_end_session();
      reboot();
      forgotten += nandlog_chip_is_bad_block(9 * ppb()) ? 0 : 1;
   }
   clear_faults();
   CHECK((trials || !NANDLOG_ERASE_AHEAD_BLOCKS) && !forgotten, "of %u torn marks, %u blocks forgotten after the recovery utility", trials, forgotten);
   end_test();
}
#endif

static nandlog_status_t store_whole_page(uint32_t id)
{
   return (nandlog_status_t)store(id, page_record_bytes());
}

static void test_y3_relocating_an_unstable_page(void)
{
   // A commit torn under the unstable model, listed by the boot; then a program fails further on in the block, and
   // the block's pages move. The torn page reads differently every time: judged from one full read it is not whole,
   // so it goes through RAM -- verbatim from that read, or as a tombstone -- and no good target block is retired
   // for a difference that was the source's
   begin_test("Y3", "relocating a block with an unstable torn page retires no good block and keeps logging");
   uint32_t trials = 0, bad = 0, unwritable = 0;
   for (uint64_t seed = 1; seed <= 60; ++seed)
   {
      new_part();
      reboot();
      begin_epoch("y3");
      const uint32_t start = epoch_start_block();
      for (uint32_t id = 0; id < 11; ++id)
         store_whole_page(id);
      nandlog_sim_seed(mix_seed(seed));
      nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_UNSTABLE, 0.995, &cut_jump);
      if (setjmp(cut_jump) == 0)
      {
         store_whole_page(11);
         nandlog_sim_power_cut_disarm();
         continue;
      }
      nandlog_sim_power_cut_disarm();
      nandlog_sim_cut_t cut;
      nandlog_sim_last_cut(&cut);
      if ((cut.command != 0x10) || (cut.page != ((start * ppb()) + 10)) || !nandlog_sim_page_unstable(cut.page))
         continue;
      reboot();
      nandlog_stats_t stats;
      nandlog_get_stats(&stats);
      if (stats.positions_skipped != 1)
         continue;
      ++trials;
      nandlog_sim_faults_t faults = no_faults();
      faults.unwritable_pages[faults.num_unwritable_pages++] = (start * ppb()) + 15;
      set_faults(&faults);
      nandlog_status_t worst = NANDLOG_OK;
      for (uint32_t id = 100; id < 110; ++id)
      {
         const nandlog_status_t status = store_whole_page(id);
         worst = (status != NANDLOG_OK) ? status : worst;
      }
      const nandlog_status_t flushed = (nandlog_status_t)flush(true);
      worst = (flushed != NANDLOG_OK) ? flushed : worst;
      uint32_t good_retired = 0;
      for (uint32_t b = start + 1; b < (start + 16); ++b)
         good_retired += nandlog_chip_is_bad_block(b * ppb()) ? 1 : 0;
      bad += good_retired ? 1 : 0;
      unwritable += (worst == NANDLOG_ERR_UNWRITABLE) ? 1 : 0;
      clear_faults();
   }
   CHECK(trials && !bad && !unwritable, "of %u trials, %u retired good target blocks and %u ended unwritable", trials, bad, unwritable);
   end_test();
}

#ifndef SIM_CHIP_WINBOND
static uint32_t raw_pages_of_epoch(uint32_t epoch)
{
   uint32_t count = 0;
   for (uint32_t page = log_first_block() * ppb(); page < (log_end_block() * ppb()); ++page)
   {
      nandlog_page_header_t header;
      count += (raw_page_valid(page, &header, true) && (header.epoch == epoch)) ? 1 : 0;
   }
   return count;
}

static void test_y5_new_epoch_after_an_unverified_layout(void)
{
   // Both copies of the newest table version lost: the table forgets a retirement inside the epoch, which is sealed
   // with its layout unverified. The head found under that table says nothing about where the epoch really ends, so
   // the new epoch the seal asks for must begin past every block holding the old epoch's pages, and erase none
   begin_test("Y5", "a new epoch after a seal with an unverified layout erases none of the old epoch's pages");
   fresh_epoch();
   const uint32_t start = epoch_start_block(), failing = start + 3;
   nandlog_sim_faults_t faults = no_faults();
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = failing;
   set_faults(&faults);
   write_pages(5 * ppb(), 0);
   clear_faults();
   nandlog_deinit();
   const uint32_t base = log_end_block() * ppb();
   uint32_t newest = 0, found = 0;
   for (uint32_t page = base; page < (base + (8 * ppb())); ++page)
   {
      uint32_t version = 0;
      if (memcmp(nandlog_sim_raw_page(page), "BBT3", 4))
         continue;
      memcpy(&version, nandlog_sim_raw_page(page) + 8, sizeof(version));
      if (version >= newest)
      {
         newest = version;
         found = page;
      }
   }
   rot_page(found);
   rot_page(found - 1);
   reboot();
   const uint32_t before = raw_pages_of_epoch(1);
   const bool unverified = (store(9000, 16) == NANDLOG_ERR_SEALED) && !nandlog_chip_is_bad_block(failing * ppb());
   const bool begun = begin_epoch("y5 next");
   write_pages(3, 9100);
   reboot();
   const uint32_t after = raw_pages_of_epoch(1);
   audit_current();
   CHECK(unverified && begun && (after == before) && (ids_seen(9100, 3) == 3),
         "with the layout unverified the new epoch was %s; the old epoch's pages on the part went from %u to %u; the new one "
         "delivers %u of 3", begun ? "begun" : "refused", before, after, ids_seen(9100, 3));
   end_test();
}
#endif

#if (NANDLOG_ERASE_AHEAD_BLOCKS == 0) && !defined(SIM_CHIP_WINBOND)
static void test_y6_store_that_strands_says_so(void)
{
   // With no window, the head enters a block by erasing it inside the call that stores into it. The block refuses
   // its erase and the table can take no new version, so the log stops taking pages in that call, and the record
   // it was given goes with the buffer. The call must say so rather than return OK
   begin_test("Y6", "a record the same call has to drop is refused, not reported stored");
   fresh_epoch();
   for (uint32_t i = 0; i <= ppb(); ++i)
      store(i, page_record_bytes());
   flush(true);
   for (uint32_t i = 0; i < (ppb() - 1); ++i)
      store(100 + i, page_record_bytes());
   flush(true);
   nandlog_sim_faults_t faults = no_faults();
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = epoch_start_block() + 2;
   for (uint32_t c = 0; c < 8; ++c)
      faults.unwritable_blocks[faults.num_unwritable_blocks++] = log_end_block() + c;
   set_faults(&faults);
   nandlog_stats_t before, after;
   nandlog_get_stats(&before);
   const int status = store(999, 16);
   nandlog_get_stats(&after);
   clear_faults();
   CHECK(!nandlog_sim_unwritable_reported() || ((status != NANDLOG_OK) && (after.records_stored == before.records_stored)),
         "the call that stopped the log returned %d; records stored +%u, lost +%u", status, after.records_stored - before.records_stored,
         after.records_lost - before.records_lost);
   end_test();
}
#endif


// Found by the Seventh Review ----------------------------------------------------------------------------------------
//
// A seventh review found what follows. Each test fails against the code that review read and passes now. Tag: Z

static void test_z1_head_search_probes_move(void)
{
   // Every boot latched the same pages first -- halfway round the region from the epoch's start, then a quarter of the
   // way, and so on -- whatever else changed, and read disturb counts exactly that. The first probes now move whenever
   // the ring is written: here each boot lists a commit the cut before it tore. Every boot must still find the head
   begin_test("Z1", "the head search's first probes move from boot to boot, and every boot finds the head");
   fresh_epoch();
   write_pages(20, 0);
   const uint32_t start = epoch_start_block(), half = (log_blocks() * ppb()) / 2;
   const uint32_t middle = ((log_first_block() + (((start - log_first_block()) + (half / ppb())) % log_blocks())) * ppb()) + (half % ppb());
   uint32_t boots = 0, latched = 0;
   for (uint32_t i = 0; i < 12; ++i)
   {
      if (!cut_next_flush(100 + i, 60))
         continue;
      nandlog_sim_reset_counters();
      reboot();
      ++boots;
      latched += nandlog_sim_page_latches(middle) ? 1 : 0;
      write_pages(1, 200 + i);
   }
   const audit_t audit = audit_current();
   CHECK(boots >= 8, "only %u of 12 cuts tore a commit", boots);
   CHECK(latched <= (boots / 2), "the page halfway round the region was latched by %u of %u boots", latched, boots);
   CHECK(!audit.damaged && !audit.duplicates && !audit.seq_breaks && (ids_seen(0, 20) == 20) && (ids_seen(200, 12) == boots),
         "after the boots: %u damaged, %u duplicated, %u sequence breaks, %u of 20 and %u of %u committed pages read", audit.damaged,
         audit.duplicates, audit.seq_breaks, ids_seen(0, 20), ids_seen(200, 12), boots);
   CHECK(!repeated_programs_outside_retired_blocks(), "a page was programmed twice");
   end_test();
}

// Found on Silicon ----------------------------------------------------------------------------------------------------
//
// What the Alliance part was seen to do on the bench, and the simulator now does too. Tag: H

#ifndef SIM_CHIP_WINBOND
static bool first_page_holds(uint32_t block, const uint8_t *main_area)
{
   // The block's first page holds 'main_area', and its spare area is erased but for a 00h mark in its first byte
   const uint8_t *raw = nandlog_sim_raw_page(block * ppb());
   if (memcmp(raw, main_area, page_bytes()))
      return false;
   for (uint32_t i = 0; i < nandlog_chip_geometry()->spare_size_bytes; ++i)
      if (raw[page_bytes() + i] != (i ? 0xFF : 0x00))
         return false;
   return true;
}

static void test_h1_mark_programs_only_the_mark(void)
{
   // On the Alliance part a PROGRAM LOAD resets the cache register to FFh only from the column it starts at; what
   // lies before that column is programmed as the last read left it. A mark loaded as the one byte at the spare
   // area's column therefore wrote the page read last into the main area of the block it marked. Here the page read
   // last before each mark is all zeros or a pattern, and the blocks marked -- one erased, one with data in its first
   // page -- by their retirement and again by the recovery utility, which marks every block the table names back to
   // back, must keep their first page as it was, with one byte of its spare area changed
   begin_test("H1", "a bad-block mark programs the mark byte and nothing else of the page, whatever was read last");
   new_part();
   reboot();
   static uint8_t erased[NANDLOG_MAX_PAGE_SIZE_BYTES], pattern[NANDLOG_MAX_PAGE_SIZE_BYTES];
   const uint32_t zeros = ((log_end_block() - 1) * ppb()) + 1, patterned = ((log_end_block() - 1) * ppb()) + 2;
   const uint32_t empty = log_end_block() - 3, holding = log_end_block() - 4;
   memset(erased, 0xFF, sizeof(erased));
   fill_record(pattern, page_bytes(), 4242);
   memset(nandlog_sim_raw_page(zeros), 0x00, page_bytes());
   memcpy(nandlog_sim_raw_page(patterned), pattern, page_bytes());
   fill_record(record, page_bytes(), 777);
   memcpy(nandlog_sim_raw_page(holding * ppb()), record, page_bytes());

   nandlog_begin_session();
   nandlog_chip_read_page(readback, zeros);
   nandlog_chip_mark_bad_block(empty * ppb());
   nandlog_chip_read_page(readback, patterned);
   nandlog_chip_mark_bad_block(holding * ppb());
   const bool marked = first_page_holds(empty, erased) && first_page_holds(holding, record);
   nandlog_chip_read_page(readback, zeros);
   nandlog_reset_bad_block_table();
   nandlog_end_session();
   const bool remarked = first_page_holds(empty, erased) && first_page_holds(holding, record);
   reboot();
   CHECK(marked && remarked && nandlog_chip_is_bad_block(empty * ppb()) && nandlog_chip_is_bad_block(holding * ppb()),
         "first pages intact after the retirements %d, after the recovery utility %d; blocks retired after the rebuild %d %d",
         marked, remarked, nandlog_chip_is_bad_block(empty * ppb()), nandlog_chip_is_bad_block(holding * ppb()));
   end_test();
}

static void test_h2_dead_reserve_rebuilds_every_boot(void)
{
   // The board's state: every block of the reserve, and the top few of the log region, read all 00h in their first
   // page and refuse every program and erase. The first boot rebuilds the table from the marks -- one raw read per
   // block -- and, finding nowhere in the reserve for it, is given the highest pair of good blocks below the dead
   // ones, which it retires; every later boot finds the table there, with no scan of the marks, and the log lays its
   // positions out over the blocks left, and keeps what it commits
   begin_test("H2", "with the whole reserve dead, the table moves below it once, and every later boot finds it there");
   new_part();
   const uint32_t first_dead = log_end_block() - 3, blocks = nandlog_chip_geometry()->block_count;
   for (uint32_t b = first_dead; b < blocks; ++b)
      nandlog_sim_kill_block(b);
   const uint32_t home = ((first_dead - 2) / 2) * 2;
   uint32_t first_raw = 0, most_reads = 0, most_raw = 0, wrong = 0;
   for (uint32_t boot = 0; boot < 3; ++boot)
   {
      nandlog_sim_reset_counters();
      reboot();
      const nandlog_sim_counters_t c = nandlog_sim_counters();
      if (!boot)
         first_raw = c.raw_page_reads;
      else
      {
         most_reads = (c.page_reads > most_reads) ? c.page_reads : most_reads;
         most_raw = (c.raw_page_reads > most_raw) ? c.raw_page_reads : most_raw;
      }
      for (uint32_t b = 0; b < blocks; ++b)
         wrong += (nandlog_chip_is_bad_block(b * ppb()) != ((b >= first_dead) || (b == home) || (b == (home + 1)))) ? 1 : 0;
      if (!boot)
         begin_epoch("h2");
      write_pages(30, 100 * boot);
   }
   reboot();
   const audit_t a = audit_current();
   // After the first boot, the driver's share is the reserve's eight candidates (two reads each: both copies of a first
   // pair that read as zeros), one header a block down to the search's floor, the table's own pages and one marker
   // read a retired block; the rest is the log's own boot, a few dozen reads at most here
   const uint32_t bound = 8 * 2 + TABLE_SEARCH_BLOCKS + 32 + 128, raw_bound = (blocks - first_dead) + 32;
   CHECK(!wrong && !a.damaged && (ids_seen(0, 30) == 30) && (ids_seen(100, 30) == 30) && (ids_seen(200, 30) == 30),
         "%u wrong bad-block verdicts over three boots; %u damaged records, %u %u %u of 30 pages delivered", wrong, a.damaged,
         ids_seen(0, 30), ids_seen(100, 30), ids_seen(200, 30));
   CHECK((first_raw >= blocks) && (most_reads <= bound) && (most_raw <= raw_bound),
         "the first boot read %u marks raw (at least %u); a later boot read %u pages (bound %u), %u of them raw (bound %u)",
         first_raw, blocks, most_reads, bound, most_raw, raw_bound);
   end_test();
}


// K: a Dead Reserve (round 7) -----------------------------------------------------------------------------------------
//
// One board's reserve -- and the top of its log region -- was found dead: every byte 00h, every program and erase
// refused. Nothing could hold the table, so every boot rebuilt it from the marks, and the first block to fail in the
// log region stopped logging, its retirement having nowhere to go. These hold the driver to its two answers: the
// table moved to a pair of blocks below the reserve, and, with no pair to be had, degraded mode, in which a block's
// own mark retires it. From K11 on, a retirement neither can record: the table given a new pair in the call that
// needed it, under the boot's rules, and logging going on

static uint32_t search_first_block(void)
{
   const uint32_t blocks = nandlog_chip_geometry()->block_count;
   return (TABLE_SEARCH_BLOCKS < blocks) ? (blocks - TABLE_SEARCH_BLOCKS) : 0;
}

static void kill_blocks(uint32_t first, uint32_t end)
{
   for (uint32_t b = first; b < end; ++b)
      nandlog_sim_kill_block(b);
}

static bool raw_table_page(uint32_t page, uint32_t *version)
{
   // A bad-block table page as the medium holds it, judged here: the magic, a plausible count, the CRC
   const uint8_t *raw = nandlog_sim_raw_page(page);
   uint32_t crc = 0, count = 0;
   memcpy(&crc, raw + 4, sizeof(crc));
   memcpy(version, raw + 8, sizeof(*version));
   memcpy(&count, raw + 12, sizeof(count));
   return !memcmp(raw, "BBT3", 4) && (count <= 256) && (crc32_bits(raw + 8, 8 + (4 * count)) == crc);
}

static uint32_t newest_table_block(uint32_t first, uint32_t end)
{
   // The unmarked block in [first, end) holding the highest version that reads whole, or ~0
   uint32_t best = 0, block = ~0u;
   for (uint32_t b = first; b < end; ++b)
   {
      if (nandlog_sim_raw_page(b * ppb())[page_bytes()] != 0xFF)
         continue;
      for (uint32_t p = 0; p < ppb(); ++p)
      {
         uint32_t version = 0;
         if (raw_table_page((b * ppb()) + p, &version) && ((block == ~0u) || (version > best)))
         {
            best = version;
            block = b;
         }
      }
   }
   return block;
}

static bool ever_held_table(uint32_t block)
{
   // A block of a pair the table was, or is, kept in below the reserve: the first page of one of the two carries the
   // table's magic
   return (block >= search_first_block()) && (!memcmp(nandlog_sim_raw_page(block * ppb()), "BBT3", 4) ||
                                              !memcmp(nandlog_sim_raw_page((block ^ 1) * ppb()), "BBT3", 4));
}

static uint32_t bad_set_signature(void)
{
   // Which blocks the driver calls bad, as one number, to compare one boot's view with another's
   uint32_t sum = 0;
   for (uint32_t b = 0; b < nandlog_chip_geometry()->block_count; ++b)
      if (nandlog_chip_is_bad_block(b * ppb()))
         sum = (sum * 31) + b + 1;
   return sum;
}

static uint32_t good_blocks_retired(uint32_t failing)
{
   // Blocks retired that are not dead, factory-marked, the one a test made fail, one it retired itself, or a block the
   // table is kept in
   uint32_t count = 0;
   for (uint32_t b = 0; b < log_end_block(); ++b)
      if (nandlog_chip_is_bad_block(b * ppb()) && !nandlog_sim_block_dead(b) && (b != failing) && (b != 3) && !ever_held_table(b) &&
          ((b < retired_on_purpose_first) || (b >= retired_on_purpose_end)))
         ++count;
   return count;
}

static void test_k1_degraded_mode_logs_on(void)
{
   // The reserve dead, and every block a table could be moved to dead too: no table can be written anywhere. A
   // block fails under the head; its pages move, and its own mark retires it. Logging goes on, the mark is what the
   // next boot finds, and every page committed before and after is delivered
   begin_test("K1", "with nowhere for the table, a block failing under the head is retired by its mark, and logging goes on");
   new_part();
   kill_blocks(search_first_block(), nandlog_chip_geometry()->block_count);
   reboot();
   begin_epoch("k1");
   write_pages(10, 0);
   const uint32_t failing = epoch_start_block();
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_pages[faults.num_unwritable_pages++] = (failing * ppb()) + 10;
   set_faults(&faults);
   nandlog_sim_reset_counters();
   for (uint32_t i = 0; i < 10; ++i)
      store(10 + i, page_record_bytes());
   const int status = flush(true);
   const bool stopped = nandlog_sim_unwritable_reported();
   clear_faults();
   reboot();
   const bool retired = nandlog_chip_is_bad_block(failing * ppb());
   write_pages(5, 30);
   reboot();
   const audit_t a = audit_current();
   CHECK((status == NANDLOG_OK) && !stopped && retired, "the store after the failure returned %d, logging stopped %d, the block retired after a reboot %d",
         status, stopped, retired);
   CHECK((ids_seen(0, 20) == 20) && (ids_seen(30, 5) == 5) && !a.damaged && !a.duplicates && !a.seq_breaks,
         "%u of 20 and %u of 5 pages delivered, %u damaged, %u twice, %u sequence breaks", ids_seen(0, 20), ids_seen(30, 5), a.damaged, a.duplicates, a.seq_breaks);
   CHECK(!good_blocks_retired(failing), "%u good blocks retired", good_blocks_retired(failing));
   end_test();
}

static void test_k2_table_moves_below_a_dead_reserve(void)
{
   // The reserve dead. The first boot rebuilds the table from the marks and moves it to the highest pair of good
   // blocks below the reserve; a block that fails later is retired in a version written there; and every later boot
   // finds the table there, by one header a block down to the search's floor, not one raw read a block
   begin_test("K2", "with the reserve dead, the table moves below it, keeps a runtime retirement, and boots find it cheaply");
   new_part();
   kill_blocks(log_end_block(), nandlog_chip_geometry()->block_count);
   reboot();
   const uint32_t home = newest_table_block(search_first_block(), log_end_block());
   begin_epoch("k2");
   write_pages(10, 0);
   const uint32_t failing = epoch_start_block();
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_pages[faults.num_unwritable_pages++] = (failing * ppb()) + 10;
   set_faults(&faults);
   for (uint32_t i = 0; i < 10; ++i)
      store(10 + i, page_record_bytes());
   const int status = flush(true);
   const bool stopped = nandlog_sim_unwritable_reported();
   clear_faults();
   nandlog_sim_reset_counters();
   reboot();
   const nandlog_sim_counters_t c = nandlog_sim_counters();
   const uint32_t signature = bad_set_signature();
   write_pages(5, 30);
   reboot();
   const audit_t a = audit_current();
   CHECK(home == (log_end_block() - 2), "the table went to block %u, not the top pair below the reserve (%u)", home, log_end_block() - 2);
   CHECK((status == NANDLOG_OK) && !stopped && nandlog_chip_is_bad_block(failing * ppb()) && (signature == bad_set_signature()),
         "the store after the failure returned %d, logging stopped %d, the block retired %d, the same blocks retired over two boots %d",
         status, stopped, nandlog_chip_is_bad_block(failing * ppb()), signature == bad_set_signature());
   CHECK(newest_table_block(search_first_block(), log_end_block()) == home, "the retirement did not go to the table below the reserve");
   CHECK(c.raw_page_reads < 64, "a warm boot read %u pages raw: the marks were scanned", c.raw_page_reads);
   CHECK((ids_seen(0, 20) == 20) && (ids_seen(30, 5) == 5) && !a.damaged && !a.duplicates && !a.seq_breaks,
         "%u of 20 and %u of 5 pages delivered, %u damaged, %u twice, %u sequence breaks", ids_seen(0, 20), ids_seen(30, 5), a.damaged, a.duplicates, a.seq_breaks);
   CHECK(!good_blocks_retired(failing), "%u good blocks retired", good_blocks_retired(failing));
   end_test();
}

static void test_k3_the_boards_dead_blocks(void)
{
   // The board's own shape, scaled: the reserve and the region's top eighteen blocks dead (4062-4095 of 4096). The
   // table goes to the highest pair below them, and from the second boot on a boot reads about what a healthy part's
   // does, plus one header a block of the search -- never the scan of every block's mark the first boot needed
   begin_test("K3", "the reserve and the region's top 18 blocks dead: the table goes below them, warm boots stay cheap");
   new_part();
   const uint32_t first_dead = log_end_block() - 18, blocks = nandlog_chip_geometry()->block_count;
   kill_blocks(first_dead, blocks);
   nandlog_sim_reset_counters();
   reboot();
   const nandlog_sim_counters_t first = nandlog_sim_counters();
   begin_epoch("k3");
   write_pages(100, 0);
   uint32_t most_reads = 0, most_raw = 0;
   const uint32_t signature = bad_set_signature();
   bool same = true;
   for (uint32_t boot = 0; boot < 4; ++boot)
   {
      nandlog_sim_reset_counters();
      reboot();
      const nandlog_sim_counters_t c = nandlog_sim_counters();
      most_reads = (c.page_reads > most_reads) ? c.page_reads : most_reads;
      most_raw = (c.raw_page_reads > most_raw) ? c.raw_page_reads : most_raw;
      same = same && (signature == bad_set_signature());
   }
   const uint32_t home = newest_table_block(search_first_block(), log_end_block());
   const audit_t a = audit_current();
   CHECK((home == (((first_dead - 2) / 2) * 2)) && nandlog_chip_is_bad_block(home * ppb()) && nandlog_chip_is_bad_block((home + 1) * ppb()) && same,
         "the table is in block %u (the pair below the dead blocks starts at %u), retired %d %d, the same blocks retired every boot %d",
         home, ((first_dead - 2) / 2) * 2, nandlog_chip_is_bad_block(home * ppb()), nandlog_chip_is_bad_block((home + 1) * ppb()), same);
   CHECK((first.raw_page_reads >= blocks) && (most_reads <= (TABLE_SEARCH_BLOCKS + 160)) && (most_raw <= ((blocks - first_dead) + 32)),
         "the first boot read %u marks raw; a warm boot read %u pages, %u of them raw", first.raw_page_reads, most_reads, most_raw);
   CHECK((ids_seen(0, 100) == 100) && !a.damaged, "%u of 100 pages delivered, %u damaged", ids_seen(0, 100), a.damaged);
   end_test();
}

static void test_k4_partly_dead_reserve(void)
{
   // Six of the reserve's eight table blocks dead: the table rotates over the two left, across forty retirements and
   // so several block boundaries, and never leaves the reserve
   begin_test("K4", "six of the reserve's table blocks dead: the table rotates over the other two and stays in the reserve");
   new_part();
   kill_blocks(log_end_block(), log_end_block() + 6);
   reboot();
   begin_epoch("k4");
   write_pages(5, 0);
   nandlog_begin_session();
   bool all = true;
   for (uint32_t i = 0; i < 40; ++i)
      all = nandlog_chip_mark_bad_block((log_end_block() + 8 + (i % 8)) * ppb()) && all;
   nandlog_end_session();
   for (uint32_t i = 0; i < 6; ++i)
   {
      reboot();
      write_pages(1, 10 + i);
   }
   reboot();
   const audit_t a = audit_current();
   CHECK(all && (newest_table_block(search_first_block(), log_end_block()) == ~0u) && !good_blocks_retired(~0u),
         "retirements kept %d; a table below the reserve %d; %u good blocks retired", all,
         newest_table_block(search_first_block(), log_end_block()) != ~0u, good_blocks_retired(~0u));
   CHECK((ids_seen(0, 5) == 5) && (ids_seen(10, 6) == 6) && !a.damaged, "%u of 5 and %u of 6 pages delivered", ids_seen(0, 5), ids_seen(10, 6));
   end_test();
}

typedef struct
{
   uint32_t trials, lost, unstable_sets, good_retired, refused, damaged;
} cut_tally_t;

static void cut_trial_audit(cut_tally_t *t, uint32_t failing, uint32_t durable)
{
   // After a cut: two boots must agree on the blocks retired, every page committed before the trial's work began is
   // delivered, nothing twice or damaged, no good block is retired, and logging goes on
   clear_faults();
   reboot();
   const uint32_t signature = bad_set_signature();
   reboot();
   if (signature != bad_set_signature())
      ++t->unstable_sets;
   nandlog_sim_reset_counters();
   for (uint32_t i = 0; i < 3; ++i)
      store(900 + i, page_record_bytes());
   const int status = flush(true);
   if ((status != NANDLOG_OK) || nandlog_sim_unwritable_reported())
      ++t->refused;
   reboot();
   if (signature != bad_set_signature())
      ++t->unstable_sets;
   const audit_t a = audit_current();
   if ((ids_seen(0, durable) != durable) || (ids_seen(900, 3) != 3))
      ++t->lost;
   if (a.damaged || a.duplicates || a.seq_breaks)
      ++t->damaged;
   t->good_retired += good_blocks_retired(failing);
   ++t->trials;
}

static void report_cuts(const cut_tally_t *t, const char *what)
{
   CHECK(t->trials && !t->lost && !t->unstable_sets && !t->good_retired && !t->refused && !t->damaged,
         "%s: of %u cuts, %u lost pages, %u changed the blocks retired between boots, %u retired good blocks, %u stopped logging, %u damaged or repeated records",
         what, t->trials, t->lost, t->unstable_sets, t->good_retired, t->refused, t->damaged);
}

static const nandlog_sim_cut_model_t k_models[4] = { NANDLOG_SIM_CUT_BOUNDARY, NANDLOG_SIM_CUT_PREFIX, NANDLOG_SIM_CUT_BITWISE, NANDLOG_SIM_CUT_UNSTABLE };

static void test_k5_cut_moving_the_table(void)
{
   // A healthy part, an epoch, a factory-bad block and a runtime retirement -- then the reserve dies, its table with
   // it. The next boot rebuilds from the marks, abandons the reserve, cleans the blocks below it, takes a pair and
   // writes the table there, and power goes at every one of its operations in turn, under four models
   begin_test("K5", "a cut at every operation of the boot that moves the table below a reserve that died");
   new_part();
   factory_mark(3);
   reboot();
   begin_epoch("k5");
   write_pages(20, 0);
   nandlog_begin_session();
   nandlog_chip_mark_bad_block((log_end_block() - 40) * ppb());
   nandlog_end_session();
   retired_on_purpose_first = log_end_block() - 40;
   retired_on_purpose_end = retired_on_purpose_first + 1;
   nandlog_deinit();
   kill_blocks(log_end_block(), nandlog_chip_geometry()->block_count);
   nandlog_sim_snapshot();
   reboot();
   nandlog_sim_reset_operations();
   write_pages(2, 50);
   reboot();
   const uint32_t span = nandlog_sim_operations() + 8;
   cut_tally_t t;
   memset(&t, 0, sizeof(t));
   for (uint32_t m = 0; m < 4; ++m)
      for (uint32_t n = 1; n <= span; ++n)
      {
         nandlog_deinit();
         nandlog_sim_restore();
         nandlog_sim_seed(mix_seed((m * 1000) + n));
         nandlog_sim_reset_operations();
         nandlog_sim_power_cut(n, k_models[m], 0.5, &cut_jump);
         if (setjmp(cut_jump) == 0)
         {
            reboot();
            write_pages(2, 50);
         }
         nandlog_sim_power_cut_disarm();
         if (!nandlog_sim_power_failed())
            continue;
         cut_trial_audit(&t, log_end_block() - 40, 20);
      }
   report_cuts(&t, "moving the table");
   end_test();
}

static void cut_a_retirement(const char *what, bool below)
{
   // An epoch, and the head's block refusing a program: its pages move, and it is retired -- in a version below the
   // reserve, or by its mark alone -- with power cut at every operation of that, under four models
   new_part();
   kill_blocks(below ? log_end_block() : search_first_block(), nandlog_chip_geometry()->block_count);
   reboot();
   begin_epoch("k");
   write_pages(10, 0);
   const uint32_t failing = epoch_start_block();
   nandlog_deinit();
   nandlog_sim_snapshot();
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_pages[faults.num_unwritable_pages++] = (failing * ppb()) + 10;
   reboot();
   set_faults(&faults);
   nandlog_sim_reset_operations();
   write_pages(2, 10);
   const uint32_t span = nandlog_sim_operations() + 4;
   cut_tally_t t;
   memset(&t, 0, sizeof(t));
   for (uint32_t m = 0; m < 4; ++m)
      for (uint32_t n = 1; n <= span; ++n)
      {
         nandlog_deinit();
         nandlog_sim_restore();
         reboot();
         set_faults(&faults);
         nandlog_sim_seed(mix_seed((m * 1000) + n + 7));
         nandlog_sim_reset_operations();
         nandlog_sim_power_cut(n, k_models[m], 0.5, &cut_jump);
         if (setjmp(cut_jump) == 0)
            write_pages(2, 10);
         nandlog_sim_power_cut_disarm();
         if (!nandlog_sim_power_failed())
            continue;
         cut_trial_audit(&t, failing, 10);
      }
   report_cuts(&t, what);
}

static void test_k6_cut_in_a_retirement_below(void)
{
   begin_test("K6", "a cut at every operation of a retirement kept in the table below the reserve");
   cut_a_retirement("a retirement below the reserve", true);
   end_test();
}

static void test_k7_cut_in_a_retirement_by_mark(void)
{
   begin_test("K7", "a cut at every operation of a retirement committed by the block's mark alone");
   cut_a_retirement("a retirement by mark", false);
   end_test();
}

static void test_k8_table_block_fails(void)
{
   // The table below the reserve, and then its pair fails: the next pair of its block refuses the version and the
   // other block refuses the erase. The marks take over in the same call -- the pair marked, the sibling first -- and
   // the failing block is retired by its mark. The next boot rebuilds from the marks and moves the table to a new
   // pair; the old pair's versions, higher-numbered than the new pair's first, are never loaded again. Then the same,
   // cut at every operation
   begin_test("K8", "the table's own blocks failing: the marks take over, the next boot moves it, old versions stay dead");
   new_part();
   kill_blocks(log_end_block(), nandlog_chip_geometry()->block_count);
   reboot();
   const uint32_t home = newest_table_block(search_first_block(), log_end_block());
   if (home == ~0u)
   {
      CHECK(false, "the first boot put no table below the dead reserve");
      end_test();
      return;
   }
   begin_epoch("k8");
   write_pages(10, 0);
   nandlog_begin_session();
   for (uint32_t i = 0; i < 6; ++i)
      nandlog_chip_mark_bad_block((log_end_block() - 30 + i) * ppb());
   nandlog_end_session();
   retired_on_purpose_first = log_end_block() - 30;
   retired_on_purpose_end = retired_on_purpose_first + 6;
   const uint32_t failing = epoch_start_block();
   nandlog_deinit();
   nandlog_sim_snapshot();
   uint32_t next = home * ppb();
   while (!raw_page_erased(next))
      ++next;
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_pages[faults.num_unwritable_pages++] = (failing * ppb()) + 10;
   faults.unwritable_pages[faults.num_unwritable_pages++] = next;
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = home + 1;
   reboot();
   set_faults(&faults);
   for (uint32_t i = 0; i < 10; ++i)
      store(10 + i, page_record_bytes());
   const int status = flush(true);
   const bool stopped = nandlog_sim_unwritable_reported();
   clear_faults();
   reboot();
   const uint32_t moved = newest_table_block(search_first_block(), log_end_block()), signature = bad_set_signature();
   bool same = true;
   for (uint32_t boot = 0; boot < 3; ++boot)
   {
      reboot();
      same = same && (signature == bad_set_signature());
   }
   const audit_t a = audit_current();
   CHECK((status == NANDLOG_OK) && !stopped && nandlog_chip_is_bad_block(failing * ppb()), "the store returned %d, logging stopped %d, the block retired %d",
         status, stopped, nandlog_chip_is_bad_block(failing * ppb()));
   CHECK((moved != ~0u) && (moved != home) && (moved != (home + 1)) && nandlog_chip_is_bad_block(home * ppb()) && nandlog_chip_is_bad_block((home + 1) * ppb()) && same,
         "the table moved from %u to %u; the old pair retired %d %d; the same blocks retired every boot %d", home, moved,
         nandlog_chip_is_bad_block(home * ppb()), nandlog_chip_is_bad_block((home + 1) * ppb()), same);
   CHECK((ids_seen(0, 20) == 20) && !a.damaged && !good_blocks_retired(failing), "%u of 20 pages delivered, %u damaged, %u good blocks retired",
         ids_seen(0, 20), a.damaged, good_blocks_retired(failing));

   // The same, cut at every operation
   reboot();
   nandlog_sim_restore();
   reboot();
   set_faults(&faults);
   nandlog_sim_reset_operations();
   write_pages(2, 10);
   const uint32_t span = nandlog_sim_operations() + 4;
   cut_tally_t t;
   memset(&t, 0, sizeof(t));
   for (uint32_t m = 0; m < 4; ++m)
      for (uint32_t n = 1; n <= span; ++n)
      {
         nandlog_deinit();
         nandlog_sim_restore();
         reboot();
         set_faults(&faults);
         nandlog_sim_seed(mix_seed((m * 1000) + n + 11));
         nandlog_sim_reset_operations();
         nandlog_sim_power_cut(n, k_models[m], 0.5, &cut_jump);
         if (setjmp(cut_jump) == 0)
            write_pages(2, 10);
         nandlog_sim_power_cut_disarm();
         if (!nandlog_sim_power_failed())
            continue;
         cut_trial_audit(&t, failing, 10);
      }
   report_cuts(&t, "the table's blocks failing");
   end_test();
}

static void test_k9_reset_with_the_table_below(void)
{
   // The recovery utility with the table below the reserve: its blocks must stay retired, so they are marked, not
   // erased. After the reboot the table is rebuilt from the marks with them, a runtime retirement and the dead blocks
   // in it, and the next boot moves it to a new pair
   begin_test("K9", "the recovery utility with the table below the reserve keeps its blocks and every retirement");
   new_part();
   kill_blocks(log_end_block(), nandlog_chip_geometry()->block_count);
   reboot();
   const uint32_t home = newest_table_block(search_first_block(), log_end_block());
   begin_epoch("k9");
   write_pages(10, 0);
   const uint32_t retired = log_end_block() - 30;
   retired_on_purpose_first = retired;
   retired_on_purpose_end = retired + 1;
   nandlog_begin_session();
   nandlog_chip_mark_bad_block(retired * ppb());
   nandlog_reset_bad_block_table();
   nandlog_end_session();
   reboot();
   const uint32_t signature = bad_set_signature();
   reboot();
   write_pages(5, 20);
   reboot();
   const audit_t a = audit_current();
   const uint32_t moved = newest_table_block(search_first_block(), log_end_block());
   CHECK(nandlog_chip_is_bad_block(home * ppb()) && nandlog_chip_is_bad_block((home + 1) * ppb()) && nandlog_chip_is_bad_block(retired * ppb()) &&
         (signature == bad_set_signature()) && (moved != ~0u) && (moved != home),
         "the old pair retired %d %d, the runtime retirement kept %d, the same blocks retired since %d, the table moved from %u to %u",
         nandlog_chip_is_bad_block(home * ppb()), nandlog_chip_is_bad_block((home + 1) * ppb()), nandlog_chip_is_bad_block(retired * ppb()),
         signature == bad_set_signature(), home, moved);
   CHECK((ids_seen(0, 10) == 10) && (ids_seen(20, 5) == 5) && !a.damaged, "%u of 10 and %u of 5 pages delivered", ids_seen(0, 10), ids_seen(20, 5));
   end_test();
}

static uint32_t first_unwritten_page(uint32_t block)
{
   uint32_t page = block * ppb();
   while (((page + 1) < ((block + 1) * ppb())) && !raw_page_erased(page))
      ++page;
   return page;
}

static nandlog_sim_faults_t pair_broken_faults(uint32_t home)
{
   // The table's pair below the reserve can take no more: the next page of its block refuses the version, and the
   // pair's other block refuses the erase that would rotate into it
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_pages[faults.num_unwritable_pages++] = first_unwritten_page(home);
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = home ^ 1;
   return faults;
}

static void test_k11_mark_refused_table_placed_mid_logging(void)
{
   // The reserve dead, the table in a pair below it, and then that pair failing; the block under the head refuses every
   // program, its mark included, so neither the table nor the marks can retire it. The log gives the table a new pair
   // there and then, retires the block through it, and goes on. Before: the retirement failed and logging stopped
   begin_test("K11", "a block refusing even its mark, the table's pair failing: a new pair mid-logging, and logging goes on");
   new_part();
   kill_blocks(log_end_block(), nandlog_chip_geometry()->block_count);
   reboot();
   const uint32_t home = newest_table_block(search_first_block(), log_end_block());
   if (home == ~0u)
   {
      CHECK(false, "the first boot put no table below the dead reserve");
      end_test();
      return;
   }
   begin_epoch("k11");
   write_pages(10, 0);
   const uint32_t failing = epoch_start_block();
   nandlog_sim_faults_t faults = pair_broken_faults(home);
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = failing;
   set_faults(&faults);
   nandlog_sim_reset_counters();
   for (uint32_t i = 0; i < 10; ++i)
      store(10 + i, page_record_bytes());
   const int status = flush(true);
   const bool stopped = nandlog_sim_unwritable_reported();
   clear_faults();
   reboot();
   const uint32_t moved = newest_table_block(search_first_block(), log_end_block()), signature = bad_set_signature();
   reboot();
   write_pages(5, 30);
   reboot();
   const audit_t a = audit_current();
   CHECK((status == NANDLOG_OK) && !stopped && nandlog_chip_is_bad_block(failing * ppb()),
         "the store returned %d, logging stopped %d, the block retired after a reboot %d", status, stopped, nandlog_chip_is_bad_block(failing * ppb()));
   CHECK((moved != ~0u) && ((moved & ~1u) != (home & ~1u)) && nandlog_chip_is_bad_block(moved * ppb()) && (signature == bad_set_signature()),
         "the table moved from %u to %u mid-logging; the same blocks retired since %d", home, moved, signature == bad_set_signature());
   CHECK((ids_seen(0, 20) == 20) && (ids_seen(30, 5) == 5) && !a.damaged && !a.duplicates && !a.seq_breaks && !good_blocks_retired(failing),
         "%u of 20 and %u of 5 pages delivered, %u damaged, %u twice, %u sequence breaks, %u good blocks retired",
         ids_seen(0, 20), ids_seen(30, 5), a.damaged, a.duplicates, a.seq_breaks, good_blocks_retired(failing));
   end_test();
}

static void test_k12_no_pair_anywhere_fails_safe(void)
{
   // The same refusal with every block the table's search reaches dead: nothing can hold the retirement, so the log
   // stops taking pages and says so, exactly as before; after a reboot with the block mended, it is retired by its
   // mark and logging goes on
   begin_test("K12", "with no pair anywhere, a block refusing even its mark stops the log, reported as before");
   new_part();
   kill_blocks(search_first_block(), nandlog_chip_geometry()->block_count);
   reboot();
   begin_epoch("k12");
   write_pages(10, 0);
   const uint32_t failing = epoch_start_block();
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = failing;
   set_faults(&faults);
   nandlog_sim_reset_counters();
   for (uint32_t i = 0; i < 10; ++i)
      store(10 + i, page_record_bytes());
   const int status = flush(true);
   const bool stopped = nandlog_sim_unwritable_reported();
   const int refused = store(100, page_record_bytes());
   clear_faults();
   reboot();
   write_pages(5, 30);
   reboot();
   const audit_t a = audit_current();
   // The store whose commit failed reports UNWRITABLE, as ever; every call after it reports the log disabled itself
   CHECK((status == NANDLOG_ERR_DISABLED) && stopped && (refused == NANDLOG_ERR_DISABLED),
         "the flush after the failure returned %d, unwritable reported %d, the next store %d", status, stopped, refused);
   CHECK(nandlog_chip_is_bad_block(failing * ppb()) && (ids_seen(0, 10) == 10) && (ids_seen(30, 5) == 5) && !a.damaged,
         "after a reboot: block retired %d, %u of 10 and %u of 5 pages delivered", nandlog_chip_is_bad_block(failing * ppb()),
         ids_seen(0, 10), ids_seen(30, 5));
   end_test();
}

static void test_k13_cut_placing_the_table_mid_logging(void)
{
   // K11's failing call -- the pair failing, the marks taking over, the refused mark, the new pair, the retirement
   // through it, the ring -- cut at every operation, under four models
   begin_test("K13", "a cut at every operation of a retirement that places the table mid-logging");
   new_part();
   kill_blocks(log_end_block(), nandlog_chip_geometry()->block_count);
   reboot();
   const uint32_t home = newest_table_block(search_first_block(), log_end_block());
   if (home == ~0u)
   {
      CHECK(false, "the first boot put no table below the dead reserve");
      end_test();
      return;
   }
   begin_epoch("k13");
   write_pages(10, 0);
   const uint32_t failing = epoch_start_block();
   nandlog_deinit();
   nandlog_sim_snapshot();
   nandlog_sim_faults_t faults = pair_broken_faults(home);
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = failing;
   reboot();
   set_faults(&faults);
   nandlog_sim_reset_operations();
   write_pages(2, 10);
   const uint32_t span = nandlog_sim_operations() + 4;
   cut_tally_t t;
   memset(&t, 0, sizeof(t));
   for (uint32_t m = 0; m < 4; ++m)
      for (uint32_t n = 1; n <= span; ++n)
      {
         nandlog_deinit();
         nandlog_sim_restore();
         reboot();
         set_faults(&faults);
         nandlog_sim_seed(mix_seed((m * 1000) + n + 13));
         nandlog_sim_reset_operations();
         nandlog_sim_power_cut(n, k_models[m], 0.5, &cut_jump);
         if (setjmp(cut_jump) == 0)
            write_pages(2, 10);
         nandlog_sim_power_cut_disarm();
         if (!nandlog_sim_power_failed())
            continue;
         cut_trial_audit(&t, failing, 10);
      }
   report_cuts(&t, "placing the table mid-logging");
   end_test();
}

static void test_k14_window_block_refusing_its_mark(void)
{
   // The block the erase-ahead tops the window up with refuses its erase and every program, its mark included, while
   // the table's pair can take no more: the table is given a new pair inside the idle call that erases ahead, the block
   // is retired through it, and the window is counted again from nothing, so no block given away, nor any block past
   // it, is ever taken for erased. Every page logged through and past the window is delivered
   begin_test("K14", "a window block refusing its erase and its mark: the table placed mid-logging, the window counted again");
   new_part();
   kill_blocks(log_end_block(), nandlog_chip_geometry()->block_count);
   reboot();
   const uint32_t home = newest_table_block(search_first_block(), log_end_block());
   if (home == ~0u)
   {
      CHECK(false, "the first boot put no table below the dead reserve");
      end_test();
      return;
   }
   begin_epoch("k14");
   const uint32_t start = epoch_start_block(), refusing = start + NANDLOG_ERASE_AHEAD_BLOCKS + 1;
   nandlog_sim_faults_t faults = pair_broken_faults(home);
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = refusing;
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = refusing;
   set_faults(&faults);
   nandlog_sim_reset_counters();
   for (uint32_t i = 0; i < (5 * ppb()); ++i)
   {
      store(i, page_record_bytes());
      flush(false);
   }
   const int status = flush(true);
   const bool stopped = nandlog_sim_unwritable_reported();
   clear_faults();
   reboot();
   const uint32_t moved = newest_table_block(search_first_block(), log_end_block());
   const audit_t a = audit_current();
   CHECK((status == NANDLOG_OK) && !stopped && nandlog_chip_is_bad_block(refusing * ppb()) && (moved != ~0u) && ((moved & ~1u) != (home & ~1u)),
         "the flush returned %d, logging stopped %d, the window block retired %d, the table moved from %u to %u", status, stopped,
         nandlog_chip_is_bad_block(refusing * ppb()), home, moved);
   CHECK((ids_seen(0, 5 * ppb()) == (5 * ppb())) && !a.damaged && !a.duplicates && !a.seq_breaks && !good_blocks_retired(refusing),
         "%u of %u pages delivered, %u damaged, %u twice, %u sequence breaks, %u good blocks retired", ids_seen(0, 5 * ppb()), 5 * ppb(),
         a.damaged, a.duplicates, a.seq_breaks, good_blocks_retired(refusing));
   end_test();
}

static void plant_old_page(uint32_t block)
{
   // A valid page of an epoch older than any the test begins, at the first page of 'block': something a read could
   // serve, so the block may not be given to the table
   uint8_t *raw = nandlog_sim_raw_page(block * ppb());
   nandlog_page_header_t header;
   memset(&header, 0, sizeof(header));
   header.magic = NANDLOG_PAGE_MAGIC_FRAMED;
   header.epoch = 0;
   header.seq = 0;
   header.first_timestamp = header.last_timestamp = 1;
   header.payload_crc = crc32_bits(raw, 0);
   header.header_crc = crc32_bits((const uint8_t*)&header, NANDLOG_PAGE_HEADER_CRC_BYTES);
   memcpy(raw, &header, sizeof(header));
}

static void test_k15_relocation_target_never_given(void)
{
   // The head's block fails at its very first page, so the relocation's target holds no copies yet, and the only
   // pair of blocks that holds nothing is the one that target begins: every block above it is dead, the table's, or
   // holds an old page. The target must not be given to the table -- the head is about to move into it -- so with no
   // other pair the log stops, safely, and after a reboot every page is there and the table intact
   begin_test("K15", "a relocation's empty target is never given to the table, even as the only free pair");
   new_part();
   const uint32_t blocks = nandlog_chip_geometry()->block_count, head = 159;
   kill_blocks(168, blocks);
   for (uint32_t b = 162; b < 166; ++b)
      plant_old_page(b);
   reboot();
   const uint32_t home = newest_table_block(search_first_block(), log_end_block());
   begin_epoch("k15");
   const uint32_t pages = (head - epoch_start_block()) * ppb();
   write_pages(pages, 0);
   nandlog_sim_faults_t faults = pair_broken_faults(home);
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = head;
   set_faults(&faults);
   for (uint32_t i = 0; i < 3; ++i)
      store(pages + i, page_record_bytes());
   flush(true);
   const bool target_retired = nandlog_chip_is_bad_block((head + 1) * ppb());
   clear_faults();
   reboot();
   const uint32_t signature = bad_set_signature();
   write_pages(5, 39000);
   reboot();
   const audit_t a = audit_current();
   CHECK((home == 166) && !target_retired, "the table went to %u; the relocation's target given to it %d", home, target_retired);
   CHECK((ids_seen(0, pages) == pages) && (ids_seen(39000, 5) == 5) && !a.damaged && !a.duplicates && (signature == bad_set_signature()),
         "%u of %u and %u of 5 pages delivered, %u damaged, %u twice, the same blocks retired since %d", ids_seen(0, pages), pages,
         ids_seen(39000, 5), a.damaged, a.duplicates, signature == bad_set_signature());
   end_test();
}

#if NANDLOG_ERASE_AHEAD_BLOCKS == 3
static void test_k16_window_blocks_given_to_the_table(void)
{
   // A window of three: with the two blocks after the head's erased and counted, the third refuses its erase and its
   // mark, and the only pair of blocks that holds nothing is the two the window counts. The table is given them, the
   // third is retired through it, and the window is counted again from nothing, so the next block the head enters --
   // past the pair and the retired block, and holding an old page -- is erased before a page goes into it, not taken
   // for erased, programmed over, and lost to a failed program. (With a window of two or fewer, no block the window
   // counts can be part of a pair the table may be given)
   begin_test("K16", "the table given two blocks the window counted: the window counted again, no block taken for erased");
   new_part();
   const uint32_t blocks = nandlog_chip_geometry()->block_count, head = 159, refusing = head + NANDLOG_ERASE_AHEAD_BLOCKS;
   kill_blocks(168, blocks);
   for (uint32_t b = refusing + 1; b < 166; ++b)
      plant_old_page(b);
   reboot();
   const uint32_t home = newest_table_block(search_first_block(), log_end_block());
   begin_epoch("k16");
   const uint32_t pages = (head - epoch_start_block()) * ppb();
   write_pages(pages, 0);
   nandlog_sim_faults_t faults = pair_broken_faults(home);
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = refusing;
   faults.unerasable_blocks[faults.num_unerasable_blocks++] = refusing;
   set_faults(&faults);
   for (uint32_t i = 0; i < (3 * ppb()); ++i)
   {
      store(pages + i, page_record_bytes());
      flush(false);
   }
   const int status = flush(true);
   const bool stopped = nandlog_sim_unwritable_reported();
   clear_faults();
   reboot();
   const uint32_t moved = newest_table_block(search_first_block(), log_end_block());
   const audit_t a = audit_current();
   CHECK((home == 166) && (status == NANDLOG_OK) && !stopped && nandlog_chip_is_bad_block(refusing * ppb()) && ((moved & ~1u) == (head + 1)),
         "the table first at %u; the flush returned %d, logging stopped %d, the refusing block retired %d, the table moved to %u", home,
         status, stopped, nandlog_chip_is_bad_block(refusing * ppb()), moved);
   CHECK((ids_seen(0, pages + (3 * ppb())) == (pages + (3 * ppb()))) && !a.damaged && !a.duplicates && !a.seq_breaks && !good_blocks_retired(refusing),
         "%u of %u pages delivered, %u damaged, %u twice, %u sequence breaks, %u good blocks retired", ids_seen(0, pages + (3 * ppb())),
         pages + (3 * ppb()), a.damaged, a.duplicates, a.seq_breaks, good_blocks_retired(refusing));
   end_test();
}
#endif

static void test_k10_healthy_part_never_looks_below(void)
{
   // A healthy part pays nothing for any of it: a warm boot latches no page below the reserve that the log itself did
   // not ask for, reads no more than the log's own boot and the reserve's table, and asks the log for no blocks
   begin_test("K10", "a healthy part never looks below the reserve, and its warm boot reads what it did before");
   new_part();
   reboot();
   begin_epoch("k10");
   write_pages(100, 0);
   nandlog_sim_reset_counters();
   reboot();
   const nandlog_sim_counters_t c = nandlog_sim_counters();
   uint32_t latched = 0;
   for (uint32_t page = search_first_block() * ppb(); page < (log_end_block() * ppb()); ++page)
      latched += nandlog_sim_page_latches(page);
   uint32_t lowest = 0;
   CHECK(!latched && !nandlog_chip_table_blocks_wanted(&lowest) && (c.page_reads <= 90) && (c.raw_page_reads <= 30),
         "%u latches below the reserve; blocks wanted %u; the boot read %u pages, %u raw", latched, nandlog_chip_table_blocks_wanted(&lowest),
         c.page_reads, c.raw_page_reads);
   end_test();
}
#endif


// The Page CRC, Kept Up as Records Are Stored ------------------------------------------------------------------------
//
// A page's payload CRC is no longer computed in one go when the page is committed: the log adds each record to it as
// the record is stored, and a read reports the CRC it checked instead of computing it over the served bytes again.
// Every build here also compares each such CRC with a recompute where it is used (NANDLOG_CHECK_CRC). These tests drive
// every path that begins, extends, keeps or drops a page's CRC, and judge what reached the medium with this file's own
// CRC. Tag: C

static uint32_t medium_pages(uint32_t *records, uint32_t *bad_payloads)
{
   // Every page on the medium outside a retired block whose header checks: how many, the records they hold, and how
   // many of them carry a payload CRC their payload does not have
   uint32_t pages = 0;
   *records = *bad_payloads = 0;
   for (uint32_t page = log_first_block() * ppb(); page < (log_end_block() * ppb()); ++page)
   {
      nandlog_page_header_t header;
      if (nandlog_chip_is_bad_block(page) || !raw_page_valid(page, &header, false))
         continue;
      ++pages;
      *records += header.record_count;
      *bad_payloads += raw_page_valid(page, &header, true) ? 0 : 1;
   }
   return pages;
}

static uint32_t kept(uint32_t id, uint32_t length, uint32_t timestamp)
{
   // One record stored as store_at() stores it: 1 if it was taken
   return (store_at(id, length, timestamp) == NANDLOG_OK) ? 1 : 0;
}

static void test_c1_crc_routines(void)
{
   // zlib's CRC-32, whole and continued: from every split of a page's payload at each alignment, and in three pieces
   begin_test("C1", "the log's CRC is zlib's, and continues from any split exactly");
   static const char check[] = "123456789";
   CHECK((nandlog_crc32(0, check, 9) == 0xCBF43926u) && (nandlog_crc32_software(0, check, 9) == 0xCBF43926u),
         "\"123456789\" gives %08X and %08X, not CBF43926", nandlog_crc32(0, check, 9), nandlog_crc32_software(0, check, 9));
   CHECK((nandlog_crc32(0, NULL, 0) == 0) && (nandlog_crc32(0xDEADBEEFu, NULL, 0) == 0xDEADBEEFu) &&
         (nandlog_crc32_software(0x12345678u, check, 0) == 0x12345678u), "no bytes changed the value");
   static uint8_t pattern[4064 + 3];
   fill_record(pattern, sizeof(pattern), 4064);
   uint32_t whole_wrong = 0, split_wrong = 0, three_wrong = 0;
   for (uint32_t align = 0; align < 4; ++align)
   {
      const uint8_t *bytes = pattern + align;
      const uint32_t whole = crc32_bits(bytes, 4064);
      whole_wrong += (nandlog_crc32(0, bytes, 4064) != whole) + (nandlog_crc32_software(0, bytes, 4064) != whole);
      for (uint32_t split = 0; split <= 4064; ++split)
         split_wrong += (nandlog_crc32(nandlog_crc32(0, bytes, split), bytes + split, 4064 - split) != whole) +
                        (nandlog_crc32_software(nandlog_crc32_software(0, bytes, split), bytes + split, 4064 - split) != whole);
      for (uint32_t a = 1; a < 4064; a += 37)
         for (uint32_t b = a; b < 4064; b += 101)
            three_wrong += (nandlog_crc32(nandlog_crc32(nandlog_crc32(0, bytes, a), bytes + a, b - a), bytes + b, 4064 - b) != whole);
   }
   CHECK(!whole_wrong && !split_wrong && !three_wrong, "%u whole, %u two-piece and %u three-piece CRCs of 4,064 bytes differ from zlib's",
         whole_wrong, split_wrong, three_wrong);
   end_test();
}

static void test_c2_every_commit_path(void)
{
   // Records of every awkward size, empty ones included, committed because the next did not fit, because time stepped
   // back, by a partial flush, by a full one, by begin_reading(), by begin_epoch() and around a reboot -- with refused
   // records in between, which must leave the page's CRC as they found it
   begin_test("C2", "a page's CRC, kept up record by record, is its payload's on every path that commits it");
   static const uint32_t sizes[] = { 0, 1, 2, 3, 5, 7, 31, 64, 255, 1000, 2047 };
   fresh_epoch();
   uint32_t stored = 0, id = 1, time = 100000;
   for (uint32_t i = 0; i < 60; ++i)
      stored += kept(id++, sizes[i % (sizeof(sizes) / sizeof(sizes[0]))], time += 10);
   stored += kept(id++, 40, time += 10);
   stored += kept(id++, 40, time - 5000);                        // the time base moved back: the page is committed
   stored += kept(id++, 3, time += 10);
   CHECK(flush(true) == NANDLOG_OK, "a partial flush failed");
   stored += kept(id++, page_record_bytes(), time += 10);
   CHECK(flush(false) == NANDLOG_OK, "a full page was not committed by flush(false)");

   // Refused records: too large, while disabled, and one that would commit while a read is open
   stored += kept(id++, 100, time += 10);
   CHECK(!kept(id++, nandlog_data_bytes_per_page(), time), "an oversized record was stored");
   nandlog_disable(true);
   CHECK(!kept(id++, 10, time), "a record was stored while disabled");
   nandlog_disable(false);
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);                                       // commits the record above
   stored += kept(id++, 17, time += 10);
   CHECK(!kept(id++, page_record_bytes(), time), "a record that needed a commit was stored during a read");
   nandlog_end_reading();
   nandlog_end_session();
   stored += kept(id++, 9, time += 10);
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);                                       // commits the two records above
   nandlog_end_reading();
   nandlog_end_session();

   // Records buffered when an epoch ends go to it; the next epoch's pages start their CRCs afresh, as after a reboot
   for (uint32_t i = 0; i < 5; ++i)
      stored += kept(id++, 77, time += 10);
   CHECK(begin_epoch("c2b"), "the second epoch was not begun");
   for (uint32_t i = 0; i < 30; ++i)
      stored += kept(id++, sizes[(i * 7) % (sizeof(sizes) / sizeof(sizes[0]))], time += 10);
   CHECK(flush(true) == NANDLOG_OK, "a flush in the second epoch failed");
   for (uint32_t i = 0; i < 4; ++i)                                  // buffered when the power goes, and lost
      kept(id++, 50 + i, time += 10);
   reboot();
   for (uint32_t i = 0; i < 30; ++i)
      stored += kept(id++, sizes[(i * 5) % (sizeof(sizes) / sizeof(sizes[0]))], time += 10);
   CHECK(flush(true) == NANDLOG_OK, "the last flush failed");

   uint32_t records = 0, bad = 0;
   const uint32_t pages = medium_pages(&records, &bad);
   CHECK(!bad && (records == stored) && (pages >= 12), "%u of %u pages carry a payload CRC their payload does not have; %u records on the part, %u stored",
         bad, pages, records, stored);
   end_test();
}

static void test_c3_retries_and_relocation(void)
{
   // The page being committed is built again for each block it is tried in: once after its program fails and the head's
   // block is relocated, once after a read-back finds a cell that did not program. Its CRC is the same each time
   begin_test("C3", "a page tried again after a failed program or read-back keeps its payload's CRC");
   fresh_epoch();
   uint32_t stored = 0, id = 1;
   for (uint32_t i = 0; i < 5; ++i)
   {
      stored += kept(id, 300 + i, 1000 + id);
      ++id;
   }
   CHECK(flush(true) == NANDLOG_OK, "the first page was not committed");
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = epoch_start_block();
   set_faults(&faults);
   for (uint32_t i = 0; i < 9; ++i)
   {
      stored += kept(id, 211 + (3 * i), 1000 + id);
      ++id;
   }
   CHECK(flush(true) == NANDLOG_OK, "the page whose program failed was not placed");
   clear_faults();
   for (uint32_t i = 0; i < 6; ++i)
   {
      stored += kept(id, 150 + i, 1000 + id);
      ++id;
   }
   flush(true);
   faults = no_faults();
   faults.has_stuck_cells = true;
   faults.stuck_page = epoch_start_block() * ppb();
   for (uint32_t page = faults.stuck_page; page < (log_end_block() * ppb()); ++page)
      if (raw_page_erased(page) && !nandlog_chip_is_bad_block(page))
      {
         faults.stuck_page = page;
         break;
      }
   faults.stuck_offset = sizeof(nandlog_page_header_t) + 50;
   faults.stuck_mask = 0xFF;
   set_faults(&faults);
   for (uint32_t i = 0; i < 4; ++i)
   {
      stored += kept(id, 99 + i, 1000 + id);
      ++id;
   }
   CHECK(flush(true) == NANDLOG_OK, "the page whose read-back failed was not placed");
   clear_faults();
   uint32_t records = 0, bad = 0;
   const uint32_t pages = medium_pages(&records, &bad);
   nandlog_stats_t stats;
   nandlog_get_stats(&stats);
   CHECK(!bad && (records == stored) && (stats.pages_relocated > 0), "%u of %u pages carry a wrong payload CRC; %u records on the part, %u stored; %u pages relocated",
         bad, pages, records, stored, stats.pages_relocated);
   end_test();
}

static void test_c4_dropped_records(void)
{
   // Records lost with a commit no block would take are dropped with their CRC: the next epoch's first page starts afresh
   begin_test("C4", "records dropped with a failed commit take their CRC with them");
   fresh_epoch();
   for (uint32_t i = 0; i < 7; ++i)
      kept(100 + i, 333, 2000 + i);                                  // a page of them committed, if a page holds fewer
   nandlog_sim_faults_t faults = no_faults();
   for (uint32_t i = 0; i < NANDLOG_SIM_MAX_FAULT_BLOCKS; ++i)
      faults.unwritable_blocks[i] = log_first_block() + i;
   faults.num_unwritable_blocks = NANDLOG_SIM_MAX_FAULT_BLOCKS;
   set_faults(&faults);
   CHECK(flush(true) == NANDLOG_ERR_UNWRITABLE, "the commit no block would take did not fail");
   clear_faults();
   CHECK(!nandlog_has_buffered_data(), "the lost records are still buffered");
   CHECK(begin_epoch("c4b"), "no epoch could be begun after the failure");
   uint32_t stored = 0;
   for (uint32_t i = 0; i < 12; ++i)
      stored += kept(200 + i, 401, 3000 + i);
   CHECK(flush(true) == NANDLOG_OK, "the new epoch's page was not committed");
   nandlog_stats_t stats;
   nandlog_get_stats(&stats);
   uint32_t records = 0, bad = 0;
   const uint32_t pages = medium_pages(&records, &bad);
   CHECK(!bad && (records == (stats.records_stored - stats.records_lost)) && (stored == 12), "%u of %u pages carry a wrong payload CRC; %u records on the part, %u stored and %u lost",
         bad, pages, records, stats.records_stored, stats.records_lost);
   end_test();
}

static void test_c5_reads_report_the_crc_they_checked(void)
{
   // Every way of serving a page reports the payload CRC of exactly the bytes it served; a gap reports none
   begin_test("C5", "every read reports its payload's CRC, and a gap none");
   fresh_epoch();
   for (uint32_t i = 0; i < 300; ++i)
      kept(i, 20 + ((i * 37) % 900), 1000 + i);
   flush(true);
   const uint32_t rotted = (epoch_start_block() * ppb()) + 3;
   uint32_t offset = sizeof(nandlog_page_header_t) + 10;
   while (!nandlog_sim_raw_page(rotted)[offset])
      ++offset;
   nandlog_sim_corrupt(rotted, offset, 0xFF);
   uint32_t served = 0, wrong = 0, gaps = 0, gap_crc = 0;
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   uint32_t positions = 0;
   nandlog_read_span(&positions, NULL);
   for (uint32_t i = 0; i < positions; ++i)
   {
      nandlog_page_header_t header;
      const uint32_t length = nandlog_retrieve_next_page(readback, &header);
      if (!length)
      {
         ++gaps;
         gap_crc += header.payload_crc ? 1 : 0;
         continue;
      }
      ++served;
      wrong += (header.payload_crc != crc32_bits(readback, length));
      nandlog_page_header_t again;
      const uint32_t by_seq = nandlog_retrieve_page_by_seq(header.seq, readback, &again);
      wrong += !by_seq || (again.payload_crc != header.payload_crc) || (again.payload_crc != crc32_bits(readback, by_seq));
   }
   nandlog_end_reading();
   bool end = false;
   for (uint32_t back = 0; !end; ++back)
   {
      nandlog_page_header_t header;
      const uint32_t length = nandlog_read_recent_page(back, readback, &header, &end);
      wrong += length && (header.payload_crc != crc32_bits(readback, length));
   }
   nandlog_end_session();
   CHECK(!wrong && (gaps == 1) && !gap_crc && (served == (positions - 1)), "%u of %u pages served reported a wrong CRC; %u gaps, %u with a CRC",
         wrong, served, gaps, gap_crc);
   end_test();
}

#if NANDLOG_PORT_CRC32
static void test_c6_crc_work_where_it_belongs(void)
{
   // The port's CRC sees exactly the bytes each call has to cover: a store its record, a commit its header and nothing
   // of the payload, a page served its header and its payload once
   begin_test("C6", "a store checksums its record, a commit only its header, a page served its payload once");
   fresh_epoch();
   uint32_t before = nandlog_sim_crc32_bytes();
   kept(1, 100, 1000);
   const uint32_t stored_bytes = nandlog_sim_crc32_bytes() - before;
   kept(2, 2000, 1001);
   before = nandlog_sim_crc32_bytes();
   flush(true);
   const uint32_t committed_bytes = nandlog_sim_crc32_bytes() - before;
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   nandlog_page_header_t header;
   before = nandlog_sim_crc32_bytes();
   const uint32_t length = nandlog_retrieve_next_page(readback, &header);
   const uint32_t served_bytes = nandlog_sim_crc32_bytes() - before;
   nandlog_end_reading();
   nandlog_end_session();
   const uint32_t record_bytes = prefix_bytes() + 5 + 100;
   CHECK((stored_bytes == record_bytes) && (committed_bytes == NANDLOG_PAGE_HEADER_CRC_BYTES) &&
         (served_bytes == (NANDLOG_PAGE_HEADER_CRC_BYTES + length)) && (length == (2 * prefix_bytes()) + 10 + 2100),
         "a store checksummed %u bytes (its record is %u), a commit %u (its header is %u), a page served %u (its header and payload are %u)",
         stored_bytes, record_bytes, committed_bytes, (uint32_t)NANDLOG_PAGE_HEADER_CRC_BYTES, served_bytes,
         (uint32_t)NANDLOG_PAGE_HEADER_CRC_BYTES + length);
   end_test();
}
#endif


// L: Metadata Read at the ECC Limit -----------------------------------------------------------------------------------
//
// Found by the twelfth review. Both copies of the newest ring slot, and of the newest bad-block table version, share
// a block that every boot reads and a clean boot never writes, so read disturb wears them together; and no boot wrote
// either again while both still read, because none looked at the part's verdict that a copy was corrected only at the
// limit of its ECC. Such a copy now leaves its record unsettled, and the boot writes the record again into a block of
// its own. Tag: L

typedef struct __attribute__ ((__packed__))
{
   nandlog_meta_header_t header;
   nandlog_meta_extension_t extension;
} raw_slot_t;

static uint32_t newest_ring_pair(raw_slot_t *slot)
{
   // The even page of the pair holding the slot of the highest sequence number whose head and extension check, judged
   // from the medium; ~0 if there is none
   uint32_t best = ~0u, sequence = 0;
   for (uint32_t page = 0; page < (METADATA_RING_BLOCKS * ppb()); ++page)
   {
      raw_slot_t candidate;
      memcpy(&candidate, nandlog_sim_raw_page(page), sizeof(candidate));
      if ((candidate.header.magic != NANDLOG_META_MAGIC) ||
          (candidate.extension.extension_crc != crc32_bits((const uint8_t*)&candidate, NANDLOG_META_EXTENSION_CRC_BYTES)) ||
          ((best != ~0u) && (candidate.extension.sequence <= sequence)))
         continue;
      best = page & ~1u;
      sequence = candidate.extension.sequence;
      if (slot)
         *slot = candidate;
   }
   return best;
}

#ifndef SIM_CHIP_WINBOND
static bool same_slot_content(uint32_t a, uint32_t b)
{
   // Two slots that say the same of their epoch: all but the numbering, the checksums over it, and the span a slot
   // measures when it is written
   raw_slot_t x, y;
   memcpy(&x, nandlog_sim_raw_page(a), sizeof(x));
   memcpy(&y, nandlog_sim_raw_page(b), sizeof(y));
   const uint32_t length = x.header.details_length + (x.extension.passed_over_count * sizeof(uint32_t));
   return (x.header.epoch == y.header.epoch) && (x.header.log_start_page == y.header.log_start_page) &&
          (x.header.details_length == y.header.details_length) && (x.header.details_crc == y.header.details_crc) &&
          (x.extension.head_failed_page == y.extension.head_failed_page) && (x.extension.retired_blocks == y.extension.retired_blocks) &&
          (x.extension.sealed == y.extension.sealed) && (x.extension.passed_over_count == y.extension.passed_over_count) &&
          (x.extension.passed_over_crc == y.extension.passed_over_crc) && (length <= (page_bytes() - sizeof(x))) &&
          !memcmp(nandlog_sim_raw_page(a) + sizeof(x), nandlog_sim_raw_page(b) + sizeof(y), length);
}
#endif

typedef struct { uint32_t erases, programs; } block_writes_t;

static block_writes_t writes_in(uint32_t first_block, uint32_t blocks)
{
   // Erases of these blocks, and programs of their pages, since the counters were reset
   block_writes_t w = { 0, 0 };
   for (uint32_t b = first_block; b < (first_block + blocks); ++b)
   {
      w.erases += nandlog_sim_block_erases(b);
      w.programs += nandlog_sim_block_programs(b);
   }
   return w;
}

static block_writes_t ring_writes(void)
{
   return writes_in(0, METADATA_RING_BLOCKS);
}

static block_writes_t table_writes(void)
{
   // The Alliance driver's table blocks; nothing on the Winbond part, whose table is its own
#ifdef SIM_CHIP_WINBOND
   const block_writes_t none = { 0, 0 };
   return none;
#else
   return writes_in(log_end_block(), 8);
#endif
}

static uint32_t metadata_writes(void)
{
   const block_writes_t ring = ring_writes(), table = table_writes();
   return ring.erases + ring.programs + table.erases + table.programs;
}

static uint32_t boot_writes(void)
{
   // A reboot, and the programs and erases it issued to the ring's and the table's blocks. Every boot erases the
   // window ahead of the head again, which is not counted
   nandlog_sim_reset_counters();
   reboot();
   return metadata_writes();
}

typedef struct
{
   uint32_t epoch, first_page, positions, delivered, gaps, retired, ring_pair;
   bool whole;
   raw_slot_t slot;
} l_state_t;

static l_state_t l_state(void)
{
   // What the log says of the current epoch, read through the API, and where the newest slot is on the medium
   l_state_t s;
   memset(&s, 0, sizeof(s));
   nandlog_epoch_info_t info;
   memset(&info, 0, sizeof(info));
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   const audit_t a = audit_current();
   s.epoch = info.epoch;
   s.first_page = info.first_page;
   s.positions = a.positions;
   s.delivered = a.delivered;
   s.gaps = a.gaps;
   s.whole = !a.damaged && !a.duplicates && !a.seq_breaks && (ids_seen(0, 30) == 30) && (ids_seen(200, 20) == 20);
   s.retired = nandlog_bad_block_count();
   s.ring_pair = newest_ring_pair(&s.slot);
   return s;
}

static bool l_same_epoch(const l_state_t *a, const l_state_t *b)
{
   return (a->epoch == b->epoch) && (a->first_page == b->first_page) && (a->positions == b->positions) && (a->delivered == b->delivered) &&
          (a->gaps == b->gaps) && (a->retired == b->retired) && a->whole && b->whole;
}

static bool l_epoch_with_a_list(void)
{
   // An epoch whose slot lists a position a cut tore, with pages committed before and after it, booted until settled.
   // True if the slot lists something
   fresh_epoch();
   write_pages(30, 0);
   cut_next_flush(100, 60);
   reboot();
   write_pages(20, 200);
   reboot();
   raw_slot_t slot;
   return (newest_ring_pair(&slot) != ~0u) && slot.extension.passed_over_count;
}

#ifndef SIM_CHIP_WINBOND
static uint32_t newest_table_pair(uint32_t *version)
{
   // The even page of the pair holding the highest table version that checks, in the reserve's unmarked table blocks,
   // judged from the medium; ~0 if there is none
   uint32_t best = ~0u, newest = 0;
   for (uint32_t page = log_end_block() * ppb(); page < ((log_end_block() + 8) * ppb()); ++page)
   {
      uint32_t v = 0;
      if ((nandlog_sim_raw_page((page / ppb()) * ppb())[page_bytes()] != 0xFF) || !raw_table_page(page, &v) || ((best != ~0u) && (v <= newest)))
         continue;
      best = page & ~1u;
      newest = v;
   }
   if (version)
      *version = newest;
   return best;
}

static bool same_table_entries(uint32_t a, uint32_t b)
{
   uint32_t count_a = 0, count_b = 0;
   memcpy(&count_a, nandlog_sim_raw_page(a) + 12, sizeof(count_a));
   memcpy(&count_b, nandlog_sim_raw_page(b) + 12, sizeof(count_b));
   return (count_a == count_b) && (count_a <= 256) && !memcmp(nandlog_sim_raw_page(a) + 16, nandlog_sim_raw_page(b) + 16, count_a * sizeof(uint32_t));
}

static uint32_t unused_block(uint32_t from)
{
   // A good block of the region at least 'from' blocks on from the current epoch's first, where nothing of it lies
   uint32_t block = log_first_block() + ((epoch_start_block() - log_first_block() + from) % log_blocks());
   while (nandlog_chip_is_bad_block(block * ppb()))
      block = (block + 1 < log_end_block()) ? (block + 1) : log_first_block();
   return block;
}

static void l_retire_two_unused_blocks(void)
{
   // Two retirements of blocks the epoch never reached, so the table's newest version is its third pair; then the
   // boot that records the new count in the ring, and one that writes nothing
   nandlog_chip_mark_bad_block(unused_block(log_blocks() / 2) * ppb());
   nandlog_chip_mark_bad_block(unused_block(log_blocks() / 2) * ppb());
   reboot();
   reboot();
}
#endif

static void test_l1_ring_copy_at_the_ecc_limit(void)
{
   // Either copy of the newest slot read at the ECC limit. The boot goes by the slot as before, and writes it again,
   // numbered on, as the first pair of the next ring block: the same epoch, list and positions. The boot after writes
   // nothing, and logging goes on
#ifdef SIM_CHIP_WINBOND
   begin_test("L1", "ECC status 11b on a slot copy is uncorrectable on this part, which has no at-limit verdict: nothing is written");
#else
   begin_test("L1", "a copy of the newest slot read at the ECC limit is written again in the next ring block, nothing else changed");
#endif
   for (uint32_t copy = 0; copy < 2; ++copy)
   {
      const bool listed = l_epoch_with_a_list();
      const uint32_t clean = boot_writes();
      const l_state_t before = l_state();
      CHECK(listed && !clean, "copy %u: the slot lists %s, and a clean boot programmed or erased %u times", copy, listed ? "a run" : "nothing", clean);
      nandlog_sim_set_ecc_status(before.ring_pair + copy, NANDLOG_SIM_ECC_AT_LIMIT);
      nandlog_sim_reset_counters();
      reboot();
      const block_writes_t ring = ring_writes(), table = table_writes();
      nandlog_stats_t stats;
      nandlog_get_stats(&stats);
      const l_state_t after = l_state();
#ifdef SIM_CHIP_WINBOND
      (void)table;
      CHECK(!ring.erases && !ring.programs && (after.ring_pair == before.ring_pair), "copy %u: the boot programmed %u ring pages and erased %u ring blocks",
            copy, ring.programs, ring.erases);
#else
      const uint32_t next = (((before.ring_pair / ppb()) + 1) % METADATA_RING_BLOCKS) * ppb();
      CHECK((ring.erases == 1) && (ring.programs == 2) && !table.erases && !table.programs, "copy %u: the boot erased %u ring blocks and programmed %u ring "
            "pages, and wrote %u times to the table's blocks", copy, ring.erases, ring.programs, table.erases + table.programs);
      CHECK((after.ring_pair == next) && (after.slot.extension.sequence > before.slot.extension.sequence),
            "copy %u: the newest slot is at page %u, numbered %u; it was at page %u, numbered %u", copy, after.ring_pair, after.slot.extension.sequence,
            before.ring_pair, before.slot.extension.sequence);
      CHECK((after.ring_pair != ~0u) && same_slot_content(before.ring_pair, after.ring_pair), "copy %u: the slot written again says something else", copy);
      CHECK(stats.reads_at_ecc_limit >= 1, "copy %u: the read at the ECC limit was not counted", copy);
#endif
      CHECK(l_same_epoch(&before, &after), "copy %u: epoch %u from page %u, %u positions, %u delivered, %u gaps, %u retired became %u, %u, %u, %u, %u, %u%s",
            copy, before.epoch, before.first_page, before.positions, before.delivered, before.gaps, before.retired, after.epoch, after.first_page,
            after.positions, after.delivered, after.gaps, after.retired, after.whole ? "" : ", records lost or damaged");
      const uint32_t again = boot_writes();
      write_pages(10, 300);
      reboot();
      audit_current();
      CHECK(!again && (ids_seen(0, 30) == 30) && (ids_seen(200, 20) == 20) && (ids_seen(300, 10) == 10),
            "copy %u: the boot after programmed or erased %u times; %u of 30, %u of 20 and %u of 10 pages delivered after logging on", copy, again,
            ids_seen(0, 30), ids_seen(200, 20), ids_seen(300, 10));
   }
   end_test();
}

#ifndef SIM_CHIP_WINBOND
static void test_l2_table_copy_at_the_ecc_limit(void)
{
   // The same for either copy of the newest bad-block table version: the same entries written again as a new version,
   // in a fresh table block, by the path a torn version takes; the ring untouched; the boot after writes nothing
   begin_test("L2", "a copy of the newest table version read at the ECC limit is written again in a fresh block, the same entries");
   for (uint32_t copy = 0; copy < 2; ++copy)
   {
      l_epoch_with_a_list();
      l_retire_two_unused_blocks();
      const uint32_t clean = boot_writes();
      const l_state_t before = l_state();
      uint32_t version_before = 0, version_after = 0;
      const uint32_t table_before = newest_table_pair(&version_before);
      CHECK((table_before != ~0u) && (table_before % ppb()) && !clean, "copy %u: the newest version is at page %u, and a clean boot programmed or erased %u times",
            copy, table_before, clean);
      nandlog_sim_set_ecc_status(table_before + copy, NANDLOG_SIM_ECC_AT_LIMIT);
      nandlog_sim_reset_counters();
      reboot();
      const block_writes_t ring = ring_writes(), table = table_writes();
      const uint32_t table_after = newest_table_pair(&version_after);
      const l_state_t after = l_state();
      CHECK((table.erases == 1) && (table.programs == 2) && !ring.erases && !ring.programs, "copy %u: the boot erased %u table blocks and programmed %u "
            "table pages, and wrote %u times to the ring", copy, table.erases, table.programs, ring.erases + ring.programs);
      CHECK((table_after != ~0u) && ((table_after / ppb()) != (table_before / ppb())) && !(table_after % ppb()) && (version_after > version_before),
            "copy %u: the newest version is %u at page %u; it was %u at page %u", copy, version_after, table_after, version_before, table_before);
      CHECK((table_after != ~0u) && same_table_entries(table_before, table_after), "copy %u: the version written again holds other entries", copy);
      CHECK(l_same_epoch(&before, &after) && (after.ring_pair == before.ring_pair), "copy %u: the epoch or the ring changed", copy);
      const uint32_t again = boot_writes();
      CHECK(!again && (newest_table_pair(NULL) == table_after), "copy %u: the boot after programmed or erased %u times", copy, again);
   }
   end_test();
}
#endif

static void test_l3_worn_page_costs_one_write(void)
{
   // A page whose cells read at the ECC limit whatever they hold, through every erase. As a copy of the newest slot or
   // of the newest table version, it costs one boot one write: the record moves off it, and twenty boots after write
   // nothing. Nor does a later write leave the newest record on such a page: the ring wrapping right round past it, a
   // new epoch and a boot at a time, or a retirement whose version would land on one
#ifdef SIM_CHIP_WINBOND
   begin_test("L3", "a slot copy stuck at ECC status 11b, uncorrectable on this part, costs no write in twenty boots");
   l_epoch_with_a_list();
   nandlog_sim_faults_t faults = no_faults();
   faults.worn_pages[faults.num_worn_pages++] = newest_ring_pair(NULL);
   set_faults(&faults);
   uint32_t writes = 0;
   for (uint32_t boot = 0; boot < 20; ++boot)
      writes += boot_writes();
   CHECK(!writes, "twenty boots programmed or erased %u times", writes);
#else
   begin_test("L3", "a page stuck at the ECC limit costs one write, not one a boot, and no later write leaves a record on it");
   l_epoch_with_a_list();
   l_retire_two_unused_blocks();
   const uint32_t worn_slot = newest_ring_pair(NULL), worn_version = newest_table_pair(NULL) + 1;
   nandlog_sim_faults_t faults = no_faults();
   faults.worn_pages[faults.num_worn_pages++] = worn_slot;
   set_faults(&faults);
   uint32_t ring = 0, table = 0, writing = 0;
   for (uint32_t boot = 0; boot < 20; ++boot)
   {
      writing += boot_writes() ? 1 : 0;
      ring += ring_writes().erases;
      table += table_writes().erases;
   }
   CHECK((ring == 1) && !table && (writing == 1) && (newest_ring_pair(NULL) != worn_slot),
         "the newest slot's first copy worn: over twenty boots %u ring erases, %u table erases, %u boots that wrote", ring, table, writing);

   faults.worn_pages[faults.num_worn_pages++] = worn_version;
   set_faults(&faults);
   ring = table = writing = 0;
   for (uint32_t boot = 0; boot < 20; ++boot)
   {
      writing += boot_writes() ? 1 : 0;
      ring += ring_writes().erases;
      table += table_writes().erases;
   }
   CHECK(!ring && (table == 1) && (writing == 1) && (newest_table_pair(NULL) != (worn_version & ~1u)),
         "the newest table version's second copy worn too: over twenty boots %u ring erases, %u table erases, %u boots that wrote", ring, table, writing);

   // The ring wraps right round past the worn page. A slot whose first copy lands there fails its read-back and goes
   // to the next block, so no boot finds the newest slot there, and none writes one
   const uint32_t epochs = (METADATA_RING_BLOCKS * (ppb() / 2)) + 8;
   uint32_t boots_writing_a_slot = 0, newest_on_worn = 0;
   raw_slot_t original, last;
   memcpy(&original, nandlog_sim_raw_page(worn_slot), sizeof(original));
   ring = 0;
   for (uint32_t i = 0; i < epochs; ++i)
   {
      nandlog_sim_reset_counters();
      begin_epoch("l3");
      ring += ring_writes().erases;
      const uint32_t pair = newest_ring_pair(NULL);
      nandlog_sim_reset_counters();
      reboot();
      ring += ring_writes().erases;
      boots_writing_a_slot += (newest_ring_pair(NULL) != pair) ? 1 : 0;
      newest_on_worn += (newest_ring_pair(NULL) == worn_slot) ? 1 : 0;
   }
   memcpy(&last, nandlog_sim_raw_page(worn_slot), sizeof(last));
   const bool landed = (last.header.magic == NANDLOG_META_MAGIC) && (last.extension.sequence > original.extension.sequence);
   CHECK(landed && !boots_writing_a_slot && !newest_on_worn && (ring <= ((epochs / (ppb() / 2)) + 2)),
         "over %u new epochs, each followed by a boot: a slot %s on the worn page, %u boots wrote a slot, %u found the newest on the worn page, and "
         "the ring took %u erases", epochs, landed ? "was programmed" : "was never programmed again", boots_writing_a_slot, newest_on_worn, ring);

   // A retirement whose version would go to the next pair of the table's block, its first page worn as well: the
   // version is refused there and written to a fresh block, and after the boot that records the retirement in the
   // ring, no boot writes anything
   const uint32_t home = newest_table_pair(NULL), worn_next = home + 2, victim = unused_block(log_blocks() / 2);
   faults.worn_pages[faults.num_worn_pages++] = worn_next;
   set_faults(&faults);
   nandlog_sim_reset_counters();
   const bool retired = nandlog_chip_mark_bad_block(victim * ppb());
   const uint32_t moved = table_writes().erases, now = newest_table_pair(NULL);
   reboot();
   writing = 0;
   for (uint32_t boot = 0; boot < 20; ++boot)
      writing += boot_writes() ? 1 : 0;
   CHECK(retired && (moved == 1) && (now != ~0u) && ((now / ppb()) != (home / ppb())) && (newest_table_pair(NULL) == now) && !writing &&
         nandlog_chip_is_bad_block(victim * ppb()), "a retirement with the next pair worn: %s, %u table erases, the version at page %u (the block's "
         "first at %u); %u of twenty boots after wrote", retired ? "retired" : "not retired", moved, now, home, writing);
#endif
   end_test();
}

#ifndef SIM_CHIP_WINBOND
static void test_l4_cut_in_a_refresh(void)
{
   // A cut at every program and erase of the boot that writes a record again because a copy read at the ECC limit,
   // the ring's and the table's, under four models, the unstable one with four draws. Every boot after goes by the
   // same epoch, the same list and the same table, and the third writes nothing. (A second copy a cut left unstable
   // can read whole at one boot, which then has nothing to settle, and torn at the next, which writes it again)
   begin_test("L4", "a cut anywhere in a boot's rewrite of the ring or the table changes nothing, and the boots after settle it");
   static const nandlog_sim_cut_model_t models[4] = { NANDLOG_SIM_CUT_BOUNDARY, NANDLOG_SIM_CUT_PREFIX, NANDLOG_SIM_CUT_BITWISE, NANDLOG_SIM_CUT_UNSTABLE };
   uint32_t trials = 0, changed = 0, unsettled = 0, spans[2] = { 0, 0 }, rewrites[2] = { 0, 0 };
   for (uint32_t target = 0; target < 2; ++target)
   {
      l_epoch_with_a_list();
      l_retire_two_unused_blocks();
      const l_state_t before = l_state();
      const uint32_t page = (target ? newest_table_pair(NULL) : before.ring_pair) + target;   // the slot's first copy, the version's second
      nandlog_sim_set_ecc_status(page, NANDLOG_SIM_ECC_AT_LIMIT);
      nandlog_deinit();
      nandlog_sim_snapshot();
      nandlog_sim_reset_operations();
      nandlog_sim_reset_counters();
      reboot();
      spans[target] = nandlog_sim_operations();
      rewrites[target] = metadata_writes();
      for (uint32_t m = 0; m < 4; ++m)
         for (uint32_t n = 1; n <= spans[target]; ++n)
            for (uint32_t draw = 1; draw <= ((models[m] == NANDLOG_SIM_CUT_UNSTABLE) ? 4u : 1u); ++draw)
            {
               nandlog_deinit();
               nandlog_sim_restore();
               nandlog_sim_seed((n * 16) + draw);
               nandlog_sim_power_cut(n, models[m], (models[m] == NANDLOG_SIM_CUT_UNSTABLE) ? 0.995 : 0.5, &cut_jump);
               if (setjmp(cut_jump) == 0)
                  nandlog_init();
               nandlog_sim_power_cut_disarm();
               ++trials;
               reboot();
               const l_state_t after = l_state();
               bool same = l_same_epoch(&before, &after);
               uint32_t last = 0;
               for (uint32_t boot = 0; boot < 3; ++boot)
               {
                  last = boot_writes();
                  const l_state_t later = l_state();
                  same = same && l_same_epoch(&before, &later);
               }
               changed += same ? 0 : 1;
               unsettled += last ? 1 : 0;
            }
   }
   CHECK((rewrites[0] == 3) && (rewrites[1] == 3), "uncut, the boots wrote %u times to the ring and %u to the table, not an erase and two programs each",
         rewrites[0], rewrites[1]);
   CHECK(trials && !changed && !unsettled, "the rewriting boots took %u and %u operations; of %u cuts, %u changed the epoch, and after %u the fourth "
         "boot still wrote", spans[0], spans[1], trials, changed, unsettled);
   end_test();
}
#endif


// N: The Thirteenth Review -------------------------------------------------------------------------------------------
//
// Asked or found by the thirteenth review. Tag: N
//
// N1. While a read is open a store that would commit a page is refused, so a download held open across a slow link stops
// logging. The library allows another way: open a read only to take its span -- the first page's sequence number and
// the page count -- close it, and fetch the span page by page by sequence number, which needs no open read, while
// records go on being stored and pages committed. This holds the library to that, across a commit whose program fails
// and so relocates the head's block -- positions inside the span -- and retires it.
//
// N2. A boot that found the bad-block table laying the epoch out differently from its slot sealed the epoch, but wrote
// the seal into a slot measured under that table. The next boot found the two in agreement, no longer knew the layout
// was unverified, and began the next epoch after the head the wrong table had placed -- erasing the old epoch's pages
// there. Y5 begins the new epoch in the sealing boot; this one boots again first. It fails against that code

static void test_n1_fetch_by_seq_while_logging(void)
{
   begin_test("N1", "a span fetched by sequence number, no read open, while pages commit and the head's block is relocated");
   fresh_epoch();
   const uint32_t start = epoch_start_block(), before = ppb() + 20;
   write_pages(before, 0);

   // The span of a time-bounded read that reaches into the head's block, taken with the read open and then closed. Its
   // first page is served by the read itself, and says which sequence number the span begins at
   memset(seen, 0, sizeof(seen));
   audit_t a;
   memset(&a, 0, sizeof(a));
   nandlog_page_header_t header;
   uint32_t span = 0, wrong = 0;
   nandlog_begin_session();
   nandlog_begin_reading(1000 + ppb() - 10, 1000 + ppb() + 15);
   nandlog_read_span(&span, NULL);
   uint32_t length = nandlog_retrieve_next_page(readback, &header);
   const uint32_t first = header.seq;
   if (length)
      note_records(readback, length, header.record_count, &a);
   nandlog_end_reading();
   nandlog_end_session();
   CHECK((span == 26) && (first == (ppb() - 10)) && length, "the read's span is %u pages from seq %u (first page %u bytes), not 26 from %u",
         span, first, length, ppb() - 10);

   // The rest of the span by sequence number, a page-filling record stored before each fetch, so each store from the
   // second on commits a page. Before the fetches reach the head's block, that block refuses its next program
   uint32_t stored = 0, refused = 0, id = 5000, gaps = 0, retired_when = 0;
   const uint32_t head_block = start + 1;
   for (uint32_t seq = first + 1; seq < (first + span); ++seq)
   {
      if (seq == ppb())
      {
         nandlog_sim_faults_t faults = no_faults();
         faults.unwritable_blocks[faults.num_unwritable_blocks++] = head_block;
         set_faults(&faults);
         for (uint32_t k = 0; k < 2; ++k)
         {
            const int status = store(id++, page_record_bytes());
            stored += (status == NANDLOG_OK) ? 1 : 0;
            refused += (status == NANDLOG_OK) ? 0 : 1;
         }
         clear_faults();
         retired_when = nandlog_chip_is_bad_block(head_block * ppb()) ? seq : 0;

         // Whatever is left at the retired block's address reads as nothing, so a fetch that went there would be a gap
         for (uint32_t page = head_block * ppb(); page < ((head_block + 1) * ppb()); ++page)
            nandlog_sim_set_ecc_status(page, NANDLOG_SIM_ECC_UNCORRECTABLE);
      }
      const int status = store(id++, page_record_bytes());
      stored += (status == NANDLOG_OK) ? 1 : 0;
      refused += (status == NANDLOG_OK) ? 0 : 1;
      length = nandlog_retrieve_page_by_seq(seq, readback, &header);
      wrong += (header.seq != seq) ? 1 : 0;
      gaps += length ? 0 : 1;
      if (length)
         note_records(readback, length, header.record_count, &a);
   }
   CHECK(flush(true) == NANDLOG_OK, "the last page did not commit");
   nandlog_stats_t stats;
   nandlog_get_stats(&stats);
   CHECK(!refused && (stored == (span + 1)), "%u of %u records stored during the download were refused", refused, stored + refused);
   CHECK(retired_when && (stats.pages_relocated >= 20), "the head's block was %s and %u pages relocated", retired_when ? "retired" : "not retired",
         stats.pages_relocated);
   CHECK((ids_seen(first, span) == span) && !gaps && !wrong && !a.damaged && !a.duplicates,
         "the span delivered %u of its %u pages' records, %u gaps, %u under the wrong seq, %u damaged, %u twice", ids_seen(first, span), span,
         gaps, wrong, a.damaged, a.duplicates);

   // Everything stored before and during the download is on the part, once each
   reboot();
   const audit_t after = audit_current();
   CHECK((ids_seen(0, before) == before) && (ids_seen(5000, id - 5000) == (id - 5000)) && !after.damaged && !after.duplicates,
         "after a reboot %u of %u records from before the download and %u of %u from during it are delivered (%u damaged, %u twice)",
         ids_seen(0, before), before, ids_seen(5000, id - 5000), id - 5000, after.damaged, after.duplicates);
   end_test();
}

#ifndef SIM_CHIP_WINBOND
static void rot_newest_table_version(void)
{
   // Both copies of the newest bad-block table version, the last pair written in the reserve, made unreadable
   const uint32_t base = log_end_block() * ppb();
   uint32_t newest = 0, found = 0;
   for (uint32_t page = base; page < (base + (8 * ppb())); ++page)
   {
      uint32_t version = 0;
      if (memcmp(nandlog_sim_raw_page(page), "BBT3", 4))
         continue;
      memcpy(&version, nandlog_sim_raw_page(page) + 8, sizeof(version));
      if (version >= newest)
      {
         newest = version;
         found = page;
      }
   }
   rot_page(found);
   rot_page(found - 1);
}

static void test_n2_unverified_layout_survives_a_reboot(void)
{
   // Both copies of the newest table version lost, so the table forgets a retirement inside the epoch, which is sealed
   // with its layout unverified -- and the part boots again before a new epoch is begun. That boot must still find the
   // layout unverified, and the new epoch must begin past every block holding the old one's pages: every durable page
   // of the old epoch survives it, and reads back as an earlier epoch. Once for a block retired when it refused its
   // erase, as in Y5, and once for the head's block retired after a program failed in it, part-way into which the
   // rolled-back table puts the head
   begin_test("N2", "a layout found unverified is still unverified a boot later, and the new epoch erases none of the old one's pages");
   for (uint32_t relocated = 0; relocated < 2; ++relocated)
   {
      fresh_epoch();
      const uint32_t failing = epoch_start_block() + 3, durable = 5 * ppb(), head = (3 * ppb()) + 20;
      nandlog_sim_faults_t faults = no_faults();
      if (relocated)
      {
         write_pages(head, 0);
         faults.unwritable_blocks[faults.num_unwritable_blocks++] = failing;
         set_faults(&faults);
         write_pages(1, head);
         clear_faults();
         write_pages(durable - head - 1, head + 1);
      }
      else
      {
         faults.unerasable_blocks[faults.num_unerasable_blocks++] = failing;
         set_faults(&faults);
         write_pages(durable, 0);
         clear_faults();
      }
      const bool retired = nandlog_chip_is_bad_block(failing * ppb());
      nandlog_deinit();
      rot_newest_table_version();
      reboot();
      const bool sealed = (store(9000, 16) == NANDLOG_ERR_SEALED) && !nandlog_chip_is_bad_block(failing * ppb());

      // The boot after the seal: still unverified, and the new epoch placed past the old one
      reboot();
      nandlog_epoch_info_t info;
      memset(&info, 0, sizeof(info));
      nandlog_begin_session();
      nandlog_epoch_info(0, &info);
      nandlog_end_session();
      const uint32_t before = raw_pages_of_epoch(1);
      const bool begun = begin_epoch("n2 next");
      write_pages(3, 9100);
      reboot();
      const uint32_t after = raw_pages_of_epoch(1);
      memset(seen, 0, sizeof(seen));
      audit_t old;
      memset(&old, 0, sizeof(old));
      uint32_t by_seq = 0;
      nandlog_begin_session();
      const bool selected = nandlog_select_epoch(1);
      if (selected)
      {
         old = audit_view(false);
         for (uint32_t seq = 0; seq < durable; ++seq)
         {
            nandlog_page_header_t header;
            uint32_t id = ~0u;
            const uint32_t length = nandlog_retrieve_page_by_seq(seq, readback, &header);
            if (length >= (prefix_bytes() + 5 + 4))
               memcpy(&id, readback + prefix_bytes() + 5, sizeof(id));
            by_seq += (length && (id == seq) && (header.seq == seq)) ? 1 : 0;
         }
      }
      nandlog_end_session();
      const uint32_t old_ids = ids_seen(0, durable);
      audit_current();
      CHECK(retired && sealed && info.is_sealed && info.layout_unverified && begun,
            "%s: retired %d, sealed with the retirement lost %d; a boot later sealed %d, layout unverified %d; new epoch begun %d",
            relocated ? "relocation" : "refused erase", retired, sealed, info.is_sealed, info.layout_unverified, begun);

      // Every durable record comes back by its sequence number, and a sequential read delivers each of them undamaged.
      // Under the table that forgot the relocation's retirement, that read also passes through the failed block, whose
      // pages were copied and never erased, so it serves those pages twice -- the same records under the same sequence
      // numbers -- as it would have in the sealing boot. Reads of a layout the table no longer confirms go on uncorrected
      CHECK((after == before) && selected && (by_seq == durable) && (old_ids == durable) && !old.damaged && (relocated || !old.duplicates) &&
            (ids_seen(9100, 3) == 3),
            "%s: the old epoch's whole pages on the part went from %u to %u; as an earlier epoch (%s) %u of %u durable records come back "
            "by sequence number, and a read delivers %u of them (%u damaged, %u twice); the new epoch delivers %u of 3",
            relocated ? "relocation" : "refused erase", before, after, selected ? "selected" : "not selectable", by_seq, durable, old_ids,
            old.damaged, old.duplicates, ids_seen(9100, 3));
   }
   end_test();
}
#endif


// P: The Fourteenth Review --------------------------------------------------------------------------------------------
//
// Asked or found by the fourteenth review. Tag: P
//
// P1. In degraded mode a retirement is committed by its block's mark alone, and a cut in that mark can leave a byte that
// reads as no mark at one boot and as a mark at the next. When the block is the head's failed block, the boot between
// relocates it again, and if a cut stops that part-way, the next boot found the block retired -- the relocation it
// completes recorded, so excused -- with the copies part-made, and laid the epoch out without the pages left in the
// block. It fails against that code, which logged on over those pages' positions
//
// P2. NANDLOG_MAX_EPOCH_DETAILS_BYTES sets the room a slot leaves its list. A slot was judged by this build's limits:
// one written under a larger value, whose details are longer, did not count as a slot at all, and one written under
// a smaller value, whose list is longer than this build's room, did not count as whole -- so a firmware update that
// changed the value found no epoch, or went by an older slot. It fails against that code
//
// P3. The Alliance driver passed over any table block whose mark byte read other than FFh, and that byte is outside the
// part's ECC, in the block every boot reads. One stray programmed bit there, in the block holding the newest version,
// loaded the version before it from the block the table had left: the retirements since forgotten, and the epoch sealed
// for a table that no longer laid it out as written. It fails against that code
//
// P4. With the marks standing in for the table, a stray programmed bit in the mark of a good block retires it at the
// next rebuild. In a block the epoch entered after its newest slot, beyond the span that slot recorded, nothing
// disagreed with the ring: the pages after the block lay below their own positions, the head was found short by a
// block, and the boot's window erased committed pages past it. It fails against that code
//
// P5. The same inside the span the newest slot recorded, where the slot shows the change and the epoch is sealed

#ifndef SIM_CHIP_WINBOND
static bool cut_lands_on(uint32_t page, uint32_t n, bool boot)
{
   // Arm a cut at the n-th operation from here, boundary model, and run a boot, or a commit at the head; whether it
   // fired at a program of 'page'
   nandlog_sim_power_cut(n, NANDLOG_SIM_CUT_BOUNDARY, 0.5, &cut_jump);
   if (setjmp(cut_jump) == 0)
   {
      if (boot)
         reboot();
      else
      {
         store(500, page_record_bytes());
         flush(true);
      }
   }
   nandlog_sim_power_cut_disarm();
   nandlog_sim_cut_t cut;
   return nandlog_sim_power_failed() && nandlog_sim_last_cut(&cut) && (cut.command == 0x10) && !cut.otp && (cut.page == page);
}

static void test_p1_retirement_never_outruns_its_copies(void)
{
   // With the marks standing in for the table, the head's block refuses a program: its pages are copied one block on,
   // and the cut comes as its mark, which retires it, is programmed -- before a bit of it changed. The next boot finds
   // the block good, relocates it again, and is cut part-way through the copies. Then the mark reads as one, as a mark
   // a cut left part-way can. The boot after finds the block retired, its relocation recorded, and the write head short
   // of the failed page's position, the copies not all there: it must seal the epoch, its layout unverified, rather
   // than log on over the positions of pages left in the retired block
   begin_test("P1", "a retirement by a mark read only after an interrupted relocation never leaves committed pages behind");
   new_part();
   kill_blocks(search_first_block(), nandlog_chip_geometry()->block_count);
   reboot();
   begin_epoch("p2");
   const uint32_t failing = epoch_start_block() + 1, target = failing + 1, durable = ppb() + 20;
   write_pages(durable, 0);
   nandlog_sim_faults_t faults = no_faults();
   faults.unwritable_pages[faults.num_unwritable_pages++] = (failing * ppb()) + 20;
   nandlog_deinit();
   nandlog_sim_snapshot();
   bool marked = false, copying = false;
   for (uint32_t n = 1; !marked && (n < 200); ++n)
   {
      nandlog_sim_restore();
      reboot();
      set_faults(&faults);
      marked = cut_lands_on(failing * ppb(), n, false);
   }
   clear_faults();
   nandlog_deinit();
   nandlog_sim_snapshot();
   for (uint32_t n = 1; marked && !copying && (n < 200); ++n)
   {
      nandlog_sim_restore();
      copying = cut_lands_on((target * ppb()) + 10, n, true);
   }
   if (!marked || !copying)
   {
      CHECK(false, "the cuts never landed: in the mark %d, in the copies %d", marked, copying);
      end_test();
      return;
   }
   nandlog_deinit();
   nandlog_sim_raw_page(failing * ppb())[page_bytes()] = 0x00;
   reboot();
   nandlog_epoch_info_t info;
   memset(&info, 0, sizeof(info));
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   const int stored = store(600, page_record_bytes());
   flush(true);
   reboot();
   const audit_t a = audit_current();
   uint32_t left = 0;
   for (uint32_t p = 10; p < 20; ++p)
   {
      nandlog_page_header_t header;
      left += (raw_page_valid((failing * ppb()) + p, &header, true) && (header.seq == (ppb() + p))) ? 1 : 0;
   }
   CHECK(nandlog_chip_is_bad_block(failing * ppb()) && info.is_sealed && info.layout_unverified && (stored == NANDLOG_ERR_SEALED),
         "block %u retired %d; the epoch sealed %d, layout unverified %d; a store returned %d", failing,
         nandlog_chip_is_bad_block(failing * ppb()), info.is_sealed, info.layout_unverified, stored);
   CHECK((left == 10) && (ids_seen(0, ppb() + 10) == (ppb() + 10)) && !ids_seen(600, 1) && !a.damaged,
         "%u of the 10 pages left in the retired block still on the part; %u of the %u copied delivered; the store after %s; %u damaged",
         left, ids_seen(0, ppb() + 10), ppb() + 10, ids_seen(600, 1) ? "delivered" : "not delivered", a.damaged);
   end_test();
}

static void test_p3_stray_bit_in_the_newest_table_blocks_mark(void)
{
   // Normal mode: the table moved on to another block of the reserve by 40 retirements of blocks the epoch never
   // reached, and the epoch's slot written after them. Then one bit of the mark byte of the block holding the newest
   // version is cleared. The boot must load that version, not the newest of the block the table left: the same blocks
   // retired, the epoch neither sealed nor unverified, every record delivered and logging on. And it writes the same
   // entries again as a new version in a fresh block, as for a copy read at the limit of the ECC, so the boot after
   // goes by a block with no mark and writes nothing
   begin_test("P3", "one stray bit in the mark of the newest table version's block never loads an older version");
   new_part();
   reboot();
   begin_epoch("p3");
   write_pages(100, 0);
   nandlog_begin_session();
   for (uint32_t i = 0; i < 40; ++i)
      nandlog_chip_mark_bad_block((log_end_block() - 50 + i) * ppb());
   nandlog_end_session();
   reboot();
   write_pages(10, 100);
   reboot();
   uint32_t version_before = 0, version_after = 0;
   const uint32_t table_before = newest_table_pair(&version_before), home = table_before / ppb();
   const uint32_t retired = nandlog_bad_block_count(), signature = bad_set_signature();
   nandlog_deinit();
   nandlog_sim_corrupt(home * ppb(), page_bytes(), 0x01);
   const uint32_t older = newest_table_block(log_end_block(), log_end_block() + 8);
   CHECK((table_before != ~0u) && (older != ~0u) && (older != home) && (retired == 40),
         "before the bit: the newest version %u in block %u, an older one in block %u, %u blocks retired", version_before, home, older, retired);
   nandlog_sim_reset_counters();
   reboot();
   const block_writes_t table = table_writes();
   nandlog_epoch_info_t info;
   memset(&info, 0, sizeof(info));
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   const uint32_t table_after = newest_table_pair(&version_after);
   CHECK((nandlog_bad_block_count() == retired) && (bad_set_signature() == signature) && !info.is_sealed && !info.layout_unverified,
         "blocks retired %u -> %u (the same ones %d); the epoch sealed %d, layout unverified %d", retired, nandlog_bad_block_count(),
         bad_set_signature() == signature, info.is_sealed, info.layout_unverified);
   CHECK((table.erases == 1) && (table.programs == 2) && (table_after != ~0u) && ((table_after / ppb()) != home) && ((table_after / ppb()) != older) &&
         (version_after > version_before) && same_table_entries(table_before, table_after),
         "the boot erased %u table blocks and programmed %u table pages; the newest unmarked version is %u in block %u, the same entries %d",
         table.erases, table.programs, version_after, table_after / ppb(), (table_after != ~0u) && same_table_entries(table_before, table_after));
   const uint32_t again = boot_writes();
   const int stored = store(2000, page_record_bytes());
   flush(true);
   reboot();
   const audit_t a = audit_current();
   CHECK(!again && (stored == NANDLOG_OK) && (ids_seen(0, 110) == 110) && ids_seen(2000, 1) && !a.damaged && (nandlog_bad_block_count() == retired),
         "the boot after wrote %u times; a store returned %d; %u of 110 delivered, the store after %s; %u damaged; %u blocks retired", again, stored,
         ids_seen(0, 110), ids_seen(2000, 1) ? "delivered" : "not delivered", a.damaged, nandlog_bad_block_count());
   end_test();
}

static uint32_t raw_epoch_seqs(uint32_t epoch, uint32_t first, uint32_t end)
{
   // How many of sequence numbers [first, end) of 'epoch' have a page anywhere in the log region that reads whole,
   // retired blocks included
   static uint8_t hit[MAX_IDS];
   uint32_t n = 0;
   memset(hit, 0, sizeof(hit));
   for (uint32_t page = log_first_block() * ppb(); page < log_end_block() * ppb(); ++page)
   {
      nandlog_page_header_t header;
      if (raw_page_valid(page, &header, true) && (header.epoch == epoch) && (header.seq >= first) && (header.seq < end) &&
          (header.seq < MAX_IDS) && !hit[header.seq])
      {
         hit[header.seq] = 1;
         ++n;
      }
   }
   return n;
}

static void stray_bit_in_a_mark_under_the_epoch(bool recorded)
{
   // With the marks standing in -- from the first boot, so no slot is written after the epoch's own, or from a boot
   // after the pages, which records their span -- one bit is cleared in the mark of the epoch's second block, which
   // holds committed pages. The boot must seal the epoch, its layout unverified, and erase nothing; a new epoch must
   // begin past every page of it; and the old one, read as an earlier epoch, must give every page not in the block the
   // bit retired, by sequence number and by a read
   const uint32_t durable = (3 * ppb()) + 10, lost = ppb();
   new_part();
   if (!recorded)
      kill_blocks(search_first_block(), nandlog_chip_geometry()->block_count);
   reboot();
   begin_epoch(recorded ? "p5" : "p4");
   write_pages(durable, 0);
   if (recorded)
   {
      nandlog_deinit();
      kill_blocks(search_first_block(), nandlog_chip_geometry()->block_count);
      reboot();
   }
   const uint32_t stray = epoch_start_block() + 1;
   nandlog_deinit();
   nandlog_sim_corrupt(stray * ppb(), page_bytes(), 0x10);
   reboot();
   nandlog_epoch_info_t info;
   memset(&info, 0, sizeof(info));
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   const uint32_t whole = raw_epoch_seqs(1, 0, durable);
   CHECK(nandlog_chip_is_bad_block(stray * ppb()) && info.is_sealed && info.layout_unverified && (whole == durable),
         "block %u retired %d; the epoch sealed %d, layout unverified %d; %u of %u pages still whole on the part", stray,
         nandlog_chip_is_bad_block(stray * ppb()), info.is_sealed, info.layout_unverified, whole, durable);
   const bool begun = begin_epoch("next");
   uint32_t by_seq = 0;
   nandlog_begin_session();
   const bool selected = nandlog_select_epoch(1);
   for (uint32_t seq = 0; selected && (seq < durable); ++seq)
   {
      nandlog_page_header_t header;
      by_seq += nandlog_retrieve_page_by_seq(seq, readback, &header) ? 1 : 0;
   }
   memset(seen, 0, sizeof(seen));
   const audit_t old = selected ? audit_view(false) : audit_current();
   nandlog_end_session();
   CHECK(begun && selected && (by_seq == (durable - lost)) && (ids_seen(0, durable) == (durable - lost)) && !old.damaged &&
         (raw_epoch_seqs(1, 0, durable) == durable),
         "new epoch begun %d; the old one, selected %d, gives %u of %u by sequence number and %u by a read (%u damaged); %u whole "
         "on the part", begun, selected, by_seq, durable - lost, ids_seen(0, durable), old.damaged, raw_epoch_seqs(1, 0, durable));
}

static void test_p4_stray_mark_past_the_recorded_span(void)
{
   begin_test("P4", "a stray bit in a mark past the newest slot's span seals the epoch and erases none of its pages");
   stray_bit_in_a_mark_under_the_epoch(false);
   end_test();
}

static void test_p5_stray_mark_in_the_recorded_span(void)
{
   begin_test("P5", "a stray bit in a mark inside the newest slot's span seals the epoch and erases none of its pages");
   stray_bit_in_a_mark_under_the_epoch(true);
   end_test();
}
#endif

static void rewrite_slot(uint32_t pair, uint16_t details_length, uint16_t runs)
{
   // Both copies of a slot written again as a build with other limits would have written it: 'details_length' bytes of
   // details, and a list of 'runs' runs of one position each, every other position from 0 -- all of them committed pages
   raw_slot_t slot;
   static uint8_t image[NANDLOG_MAX_PAGE_SIZE_BYTES];
   memcpy(&slot, nandlog_sim_raw_page(pair), sizeof(slot));
   memset(image, 0xFF, sizeof(image));
   uint8_t *details = image + sizeof(slot), *list = details + details_length;
   for (uint32_t i = 0; i < details_length; ++i)
      details[i] = (uint8_t)(i * 7);
   for (uint32_t i = 0; i < runs; ++i)
   {
      const uint32_t run = 2 * i;
      memcpy(list + (i * sizeof(run)), &run, sizeof(run));
   }
   slot.header.details_length = details_length;
   slot.header.details_crc = crc32_bits(details, details_length);
   slot.header.header_crc = crc32_bits((const uint8_t*)&slot.header, NANDLOG_META_HEADER_CRC_BYTES);
   slot.extension.passed_over_count = runs;
   slot.extension.passed_over_crc = crc32_bits(list, runs * sizeof(uint32_t));
   slot.extension.extension_crc = crc32_bits((const uint8_t*)&slot, NANDLOG_META_EXTENSION_CRC_BYTES);
   memcpy(image, &slot, sizeof(slot));
   for (uint32_t copy = 0; copy < 2; ++copy)
      memcpy(nandlog_sim_raw_page(pair + copy), image, page_bytes());
}

static void test_p2_details_limit_changed_under_a_log(void)
{
   // The epoch's slot written again as a build with another NANDLOG_MAX_EPOCH_DETAILS_BYTES would have written it, and
   // the part booted by this one: with 600 bytes of details, more than this build's 512, as after a firmware update
   // that lowered the limit; and with 16 bytes of details and a list 20 runs longer than this build leaves room for, as
   // after one that raised it. Either way the boot must find the epoch -- not a part with no log on it, whose first epoch
   // would begin over the old one -- and seal it with its slot whole: every committed page read back, the details and
   // the list kept, and a new epoch begun past it, the old one then read back as an earlier epoch
   begin_test("P2", "a slot written under another epoch-details limit is still found, and its epoch sealed whole");
   const uint32_t room = (page_bytes() - 64 - NANDLOG_MAX_EPOCH_DETAILS_BYTES) / 4, longer = room + 20, pages = (2 * longer) + 20;
   for (uint32_t raised = 0; raised < 2; ++raised)
   {
      const char *what = raised ? "a list longer than the room" : "details longer than the limit";
      fresh_epoch();
      write_pages(pages, 0);
      nandlog_deinit();
      raw_slot_t slot;
      rewrite_slot(newest_ring_pair(&slot), raised ? 16 : 600, raised ? longer : 0);
      reboot();
      nandlog_epoch_info_t info;
      memset(&info, 0, sizeof(info));
      uint8_t details[16];
      memset(details, 0, sizeof(details));
      nandlog_begin_session();
      nandlog_epoch_info(0, &info);
      nandlog_retrieve_epoch_details(details, sizeof(details));
      nandlog_end_session();
      const int stored = store(30000, page_record_bytes());
      const audit_t a = audit_current();
      const bool newest_kept = (newest_ring_pair(&slot) != ~0u) && !slot.extension.sealed && (slot.header.details_length == (raised ? 16 : 600)) &&
                               (slot.extension.passed_over_count == (raised ? longer : 0));
      CHECK((info.epoch == 1) && info.is_sealed && (stored == NANDLOG_ERR_SEALED) && (details[1] == 7) && (details[15] == 105) && newest_kept,
            "%s: epoch %u found, sealed %d, a store returned %d; details kept %d; the newest slot sealed, carrying them and %u runs, %d",
            what, info.epoch, info.is_sealed, stored, (details[1] == 7) && (details[15] == 105), slot.extension.passed_over_count, newest_kept);
      CHECK((ids_seen(0, pages) == pages) && !a.damaged && !a.seq_breaks, "%s: %u of %u pages delivered, %u damaged, %u sequence breaks", what,
            ids_seen(0, pages), pages, a.damaged, a.seq_breaks);
      const bool begun = begin_epoch("p3 next");
      write_pages(3, 30001);
      reboot();
      memset(seen, 0, sizeof(seen));
      nandlog_begin_session();
      const bool selected = nandlog_select_epoch(1);
      const audit_t old = selected ? audit_view(false) : a;
      nandlog_end_session();
      const uint32_t old_ids = ids_seen(0, pages);
      audit_current();
      CHECK(begun && (current_epoch() == 2) && (ids_seen(30001, 3) == 3) && selected && (old_ids == pages) && !old.damaged,
            "%s: a new epoch begun %d, current %u, delivering %u of 3; the old one selected %d, delivering %u of %u, %u damaged", what, begun,
            current_epoch(), ids_seen(30001, 3), selected, old_ids, pages, old.damaged);
   }
   end_test();
}


// A test runs if no name was given on the command line, or if its call contains the name given
static const char *only;
#define RUN(call)                                   do { if (!only || strstr(#call, only)) call; } while (0)

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   only = (argc > 1) ? argv[1] : NULL;
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_sim_set_verbose(getenv("NANDLOG_VERBOSE") != NULL);
   printf("nandlog regression tests against %s %u.%u (record framing %s, chip copy %s, %u blocks)\n",
#ifdef SIM_CHIP_WINBOND
          "W25N01GW",
#else
          "AS5F18G04",
#endif
          NANDLOG_VERSION_MAJOR, NANDLOG_VERSION_MINOR, NANDLOG_RECORD_FRAMING ? "on" : "off", NANDLOG_CHIP_PAGE_COPY ? "on" : "off",
          nandlog_chip_geometry()->block_count);

   RUN(test_d1_invalid_header_anywhere(false));
   RUN(test_d1_invalid_header_anywhere(true));
   RUN(test_d1_retired_block_in_epoch());
   RUN(test_d2_torn_head(0.5, 0, "a torn page at the head is passed over, not programmed again"));
   RUN(test_d2_torn_head(0.0005, 64, "a lightly torn page that ECC shows as erased is still passed over"));
   RUN(test_torn_erase_at_head());
   RUN(test_d3_full_region_boot());
   RUN(test_d4_failure_at_the_wrap());
   RUN(test_r3b_first_block_retired_then_full());
   RUN(test_d5_refused_copy());
   RUN(test_d5_target_refuses_erase());
   RUN(test_d5_unreadable_source());
   RUN(test_d6_no_second_program());
   RUN(test_d6_ecc_limit_is_readable());
   RUN(test_d7_cut_in_table_update());
   RUN(test_d7_cut_in_first_boot());
#ifndef SIM_CHIP_WINBOND
   RUN(test_d7_large_table_kept());       // the Winbond part's remap table holds twenty entries, fixed by the silicon
#endif
   RUN(test_d8_retired_positions());
   RUN(test_d8_earlier_epoch());
   RUN(test_d9_no_logging_before_an_epoch());
   RUN(test_r3a_epoch_start_refuses_erase());
   RUN(test_r3a_begin_epoch_keeps_buffer());
   RUN(test_d10_erase_once_outside_commits());
   RUN(test_d11_statuses());
   RUN(test_d11_payload_verified());
   RUN(test_unwritable_is_sticky());
#ifndef SIM_CHIP_WINBOND
#endif
   RUN(test_d12_accessors_locked());
   RUN(test_boot_cost());

#ifndef SIM_CHIP_WINBOND
   RUN(test_t1_torn_table_update());      // the Winbond part's remap table is the part's own
#endif
   RUN(test_t2_torn_ring_slot());
   RUN(test_t3_reads_stay_in_the_buffer());
   RUN(test_t4_earlier_full_epoch_with_retired_start());
   RUN(test_t5_earlier_epoch_past_a_torn_page());
   RUN(test_t6_sequential_read_reports_page_format());
   RUN(test_t7_length_overflow());
   RUN(test_t8_time_bound_edges());
   RUN(test_t9_questionable_calls());
   RUN(test_r1_rotted_ring_slot());
#ifndef SIM_CHIP_WINBOND
   RUN(test_r1_rotted_table_version());
#endif
   RUN(test_r2_failure_at_the_wrap_survives_a_reboot());
   RUN(test_r2_unwritable_head_relocated_after_reboot());
   RUN(test_r2_blank_page_below_the_head());
   RUN(test_r3_span_exact_while_storing());
   RUN(test_a1_interrupted_erase_reerased());

   RUN(test_s1_drift_below_the_head());
   RUN(test_q1_drift_under_durable_pages());
   RUN(test_s2_blank_then_unreadable());
   RUN(test_s3_adoption_after_the_wrap());
#ifndef SIM_CHIP_WINBOND
   RUN(test_q3_table_rollback_detected());
#endif
   RUN(test_s6_epoch_details_protected());
   RUN(test_s8_one_copy_is_still_written());
   RUN(test_q2_sequence_numbers_identify_pages());

   RUN(test_u1_torn_slot_keeps_the_list());
   RUN(test_u2_full_list_seals_the_epoch());
#ifdef SIM_CHIP_WINBOND
   RUN(test_u7_stranded_records_counted());
#endif
   RUN(test_u8_adoption_past_an_unreadable_first_page());
   RUN(test_u9_cut_rewrite_is_done_again());
   RUN(test_u10_boot_relocation_cut_before_its_record());

   RUN(test_x1_torn_block_entry_erase());
   RUN(test_x4_lost_newer_epoch_after_a_seal());
#ifndef SIM_CHIP_WINBOND
   RUN(test_x2_reset_never_rolls_the_table_back());
   RUN(test_x3_reset_keeps_an_unmarked_retirement());
#endif

   RUN(test_y3_relocating_an_unstable_page());
   RUN(test_z1_head_search_probes_move());
#ifndef SIM_CHIP_WINBOND
   RUN(test_h1_mark_programs_only_the_mark());
   RUN(test_h2_dead_reserve_rebuilds_every_boot());
   RUN(test_k1_degraded_mode_logs_on());
   RUN(test_k2_table_moves_below_a_dead_reserve());
   RUN(test_k3_the_boards_dead_blocks());
   RUN(test_k4_partly_dead_reserve());
   RUN(test_k5_cut_moving_the_table());
   RUN(test_k6_cut_in_a_retirement_below());
   RUN(test_k7_cut_in_a_retirement_by_mark());
   RUN(test_k8_table_block_fails());
   RUN(test_k9_reset_with_the_table_below());
   RUN(test_k10_healthy_part_never_looks_below());
   RUN(test_k11_mark_refused_table_placed_mid_logging());
   RUN(test_k12_no_pair_anywhere_fails_safe());
   RUN(test_k13_cut_placing_the_table_mid_logging());
   RUN(test_k14_window_block_refusing_its_mark());
   RUN(test_k15_relocation_target_never_given());
#if NANDLOG_ERASE_AHEAD_BLOCKS == 3
   RUN(test_k16_window_blocks_given_to_the_table());
#endif
#endif
#ifndef SIM_CHIP_WINBOND
   RUN(test_y1_table_version_from_one_read());
   RUN(test_y2_torn_mark_programmed_again());
   RUN(test_y5_new_epoch_after_an_unverified_layout());
#endif
#if (NANDLOG_ERASE_AHEAD_BLOCKS == 0) && !defined(SIM_CHIP_WINBOND)
   RUN(test_y6_store_that_strands_says_so());
#endif
#ifndef SIM_CHIP_WINBOND
   RUN(test_u3_cut_before_a_mark());
   RUN(test_u4_rotted_first_page_is_no_mark());
   RUN(test_u5_reset_keeps_factory_marks());
   RUN(test_u6_logging_on_after_a_reset());
#endif

   RUN(test_c1_crc_routines());
   RUN(test_c2_every_commit_path());
   RUN(test_c3_retries_and_relocation());
   RUN(test_c4_dropped_records());
   RUN(test_c5_reads_report_the_crc_they_checked());
#if NANDLOG_PORT_CRC32
   RUN(test_c6_crc_work_where_it_belongs());
#endif

   RUN(test_l1_ring_copy_at_the_ecc_limit());
#ifndef SIM_CHIP_WINBOND
   RUN(test_l2_table_copy_at_the_ecc_limit());
#endif
   RUN(test_l3_worn_page_costs_one_write());
#ifndef SIM_CHIP_WINBOND
   RUN(test_l4_cut_in_a_refresh());
#endif

   RUN(test_n1_fetch_by_seq_while_logging());
#ifndef SIM_CHIP_WINBOND
   RUN(test_n2_unverified_layout_survives_a_reboot());     // the Winbond part's remap table is the part's own
#endif

#ifndef SIM_CHIP_WINBOND
   RUN(test_p1_retirement_never_outruns_its_copies());     // the Winbond part's remap table is the part's own
#endif
   RUN(test_p2_details_limit_changed_under_a_log());
#ifndef SIM_CHIP_WINBOND
   RUN(test_p3_stray_bit_in_the_newest_table_blocks_mark());
   RUN(test_p4_stray_mark_past_the_recorded_span());
   RUN(test_p5_stray_mark_in_the_recorded_span());
#endif

   nandlog_deinit();
   nandlog_sim_destroy();
   printf("\n%u checks, %u failed; %u tests failed\n", tests_run, tests_failed, defects_failed);
   return tests_failed ? 1 : 0;
}
