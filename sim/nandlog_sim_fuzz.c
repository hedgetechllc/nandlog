// Randomized fuzzing against an independent oracle, run against the RAM-backed port. `make fuzz` builds it in four
// configurations and runs a fixed set of seeds in each. Adapted from the harness the first adversarial tester
// wrote.
//
// Each step is a random library call -- a record of random length and time, now and then stepping back; a flush,
// partial or not; a new epoch; a reboot -- or an injected fault: a page or block that refuses programs or erases,
// in the log region, the metadata ring or the bad-block table's reserve; a page read only at the ECC limit, or not
// at all; one copy of the newest metadata slot or of the newest bad-block table version made unreadable. Now and
// then a power cut is armed a few operations ahead, under a random model, so cuts land in commits, relocations,
// erases, table and ring writes, and in the boots that recover from them. And now and then time passes: the weak
// pages -- those a cut stopped short, or programmed into a block whose erase a cut interrupted -- drift, all at
// once or some of them, back to erased or half of their bits, before a reboot. Every boot is followed by an audit.
//
// THE ORACLE, checked at every audit, over every epoch the ring lists:
//
//   A. every durable record of the current epoch is delivered, intact
//   B. nothing delivered is damaged, delivered twice, or another epoch's
//   C. every valid page of an epoch that sits on the medium exactly at its own position, at or before that epoch's
//      last such page, is delivered: nothing reachable is hidden
//   D. a read's declared byte total is exactly what it delivers -- on a medium that reads the same twice. A page a
//      cut tore under the unstable model can pass its checks on one read and not the next; a read over such a page
//      that misses its total is tallied as that, rather than as a violation
//   E. no page is programmed twice outside a retired block, and no page that drifted is programmed at all
//   F. a page fetched again by its sequence number is the one the read delivered
//   G. what any read served under an epoch and sequence number is what every later read of the run serves there,
//      or a gap: never other content, and never records stored after it was served as a gap
//
// A record is durable once the call that committed its page has returned, which the library's own page counter
// says. Records in a page the harness made unreadable are excused from A.
//
// A build with SIM_DEAD_RESERVE_BLOCKS set kills that many blocks from the start of the Alliance driver's reserve, and
// SIM_DEAD_LOG_BLOCKS the top blocks of the log region below it, before the first boot, as one board was found: the
// table then lives in a pair of blocks below the reserve, or, with none to be had, the spare-area marks stand in for
// it, and the faults aimed at the table's blocks are aimed at whichever blocks hold it.
//
// Usage: nandlog_sim_fuzz <seed> <steps> [cut rate] [fault rate] [mode]
//        a cut is armed on one step in 'cut rate', a fault injected on about one in 'fault rate'; mode 1 favours
//        page-sized records, mode 2 makes new epochs rare

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nandlog.h"
#include "nandlog_chip.h"
#include "nandlog_port_sim.h"

#ifdef SIM_CHIP_WINBOND
static const uint8_t DEVICE_ID[3] = { 0xEF, 0xBA, 0x21 };
#define FACTORY_MARKER_OFFSET(page_size)            0
#else
static const uint8_t DEVICE_ID[3] = { 0x8D, 0x00, 0x00 };
#define FACTORY_MARKER_OFFSET(page_size)            (page_size)
#endif

#define METADATA_RING_BLOCKS                        8
#define TABLE_BLOCKS                                8
#define MAX_IDS                                     200000
#define MAX_POSITIONS                               300000
#define MAX_READ                                    20000

static uint8_t record[NANDLOG_MAX_PAGE_SIZE_BYTES], readback[NANDLOG_MAX_PAGE_SIZE_BYTES];
static jmp_buf cut_jump;

// G's memory: what each (epoch, sequence number) was first served as -- a checksum of its content, or a gap and how
// many records existed then -- in an open-addressed table
#define IDENTITY_SLOTS                              (1u << 20)
typedef struct { uint32_t epoch, seq, crc, bound; uint8_t used, data; } identity_t;
static identity_t identity[IDENTITY_SLOTS];
static uint32_t identity_used;


// Geometry and the Part -----------------------------------------------------------------------------------------------

static uint32_t ppb(void)               { return nandlog_chip_geometry()->pages_per_block; }
static uint32_t page_bytes(void)        { return nandlog_chip_geometry()->page_size_bytes; }
static uint32_t log_first_block(void)   { return METADATA_RING_BLOCKS; }
static uint32_t log_end_block(void)     { return nandlog_chip_geometry()->block_count - nandlog_chip_geometry()->reserved_blocks; }
static uint32_t log_blocks(void)        { return log_end_block() - log_first_block(); }
static uint32_t page_record_bytes(void) { return nandlog_data_bytes_per_page() - NANDLOG_FRAMING_LENGTH_BYTES - 5; }

static nandlog_sim_faults_t faults;

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

static uint32_t table_search_first_block(void)
{
   const uint32_t blocks = nandlog_chip_geometry()->block_count;
   return (TABLE_SEARCH_BLOCKS < blocks) ? (blocks - TABLE_SEARCH_BLOCKS) : 0;
}

static uint32_t table_block(uint32_t i)
{
   // The i-th block the table may be in: the reserve's eight, or, in a build whose reserve is dead, the blocks below
   // it as far as the driver looks
   return SIM_DEAD_RESERVE_BLOCKS ? (table_search_first_block() + (i % (log_end_block() - table_search_first_block()))) : (log_end_block() + (i % TABLE_BLOCKS));
}

static void apply_faults(void)
{
   memcpy(faults.device_id, DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_sim_set_faults(&faults);
}

static uint32_t crc32_bits(const uint8_t *bytes, uint32_t length)
{
   // Written here rather than borrowed from the library, so the oracle does not trust the code under test
   uint32_t crc = 0xFFFFFFFF;
   for (uint32_t i = 0; i < length; ++i)
   {
      crc ^= bytes[i];
      for (uint32_t bit = 0; bit < 8; ++bit)
         crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
   }
   return ~crc;
}

static bool raw_header(uint32_t page, nandlog_page_header_t *header)
{
   const uint8_t *raw = nandlog_sim_raw_page(page);
   memcpy(header, raw, sizeof(*header));
   return ((header->magic == NANDLOG_PAGE_MAGIC) || (header->magic == NANDLOG_PAGE_MAGIC_FRAMED)) &&
          (crc32_bits(raw, NANDLOG_PAGE_HEADER_CRC_BYTES) == header->header_crc) && (header->payload_length <= (page_bytes() - sizeof(*header)));
}

static bool raw_payload_valid(uint32_t page, const nandlog_page_header_t *header)
{
   return crc32_bits(nandlog_sim_raw_page(page) + sizeof(*header), header->payload_length) == header->payload_crc;
}

static uint32_t repeated_page;

static uint32_t repeated_programs_outside_retired_blocks(void)
{
   uint32_t pages[64];
   const uint32_t total = nandlog_sim_repeated_program_pages(pages, 64), held = (total < 64) ? total : 64;
   uint32_t defects = 0;
   for (uint32_t i = 0; i < held; ++i)
      if (!nandlog_chip_is_bad_block(pages[i]))
      {
         if (!defects)
            repeated_page = pages[i];
         ++defects;
      }
   return defects;
}


// Randomness and the Record Model -------------------------------------------------------------------------------------

static uint64_t random_state;

static uint32_t random_below(uint32_t n)
{
   random_state ^= random_state << 13;
   random_state ^= random_state >> 7;
   random_state ^= random_state << 17;
   return n ? ((uint32_t)(random_state >> 11) % n) : 0;
}

static uint32_t next_id, id_epoch[MAX_IDS], pending[4096], pending_count, current_epoch, now = 1000;
static uint8_t durable[MAX_IDS], excused[MAX_IDS], seen[MAX_IDS];
static uint32_t steps_done, audits, cuts_fired, violations, unrepeatable_reads, drifts;
static const char *last_call = "";
static bool last_full;

#define VIOLATION(...) do { ++violations; printf("VIOLATION at step %u (after %s): ", steps_done, last_call); printf(__VA_ARGS__); printf("\n"); } while (0)

static void fill_record(uint8_t *buffer, uint32_t length, uint32_t id)
{
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

static uint32_t pages_committed(void)
{
   nandlog_stats_t stats;
   nandlog_get_stats(&stats);
   return stats.pages_committed;
}

static uint32_t library_epoch(void)
{
   nandlog_epoch_info_t info;
   memset(&info, 0, sizeof(info));
   nandlog_begin_session();
   const bool listed = nandlog_epoch_info(0, &info);
   nandlog_end_session();
   return listed ? info.epoch : 0;
}

static void pending_committed(void)
{
   for (uint32_t i = 0; i < pending_count; ++i)
      durable[pending[i]] = 1;
   pending_count = 0;
}

static void call_store(uint32_t length, uint32_t timestamp)
{
   const uint32_t id = next_id++, before = pages_committed();
   if (id >= (MAX_IDS - 10))
   {
      printf("record ids exhausted\n");
      exit(3);
   }
   id_epoch[id] = current_epoch;
   fill_record(record, length, id);
   const nandlog_status_t status = nandlog_store_record(7, timestamp, record, length);
   if (pages_committed() > before)
      pending_committed();
   last_full = (status == NANDLOG_ERR_FULL);
   if (status == NANDLOG_OK)
      pending[pending_count++] = id;
   else if ((status == NANDLOG_ERR_UNWRITABLE) || (status == NANDLOG_ERR_FULL))
      pending_count = 0;
   if (pending_count >= 4000)
   {
      printf("too many records pending\n");
      exit(3);
   }
}

static void call_flush(bool partial)
{
   const uint32_t before = pages_committed();
   const nandlog_status_t status = nandlog_flush(partial);
   if (pages_committed() > before)
      pending_committed();
   else if ((status == NANDLOG_ERR_UNWRITABLE) || (status == NANDLOG_ERR_FULL))
      pending_count = 0;
}

static void call_begin_epoch(void)
{
   const uint32_t before = pages_committed();
   const uint8_t details[8] = { 'f', 'u', 'z', 'z' };
   nandlog_begin_session();
   const bool begun = nandlog_begin_epoch(details, sizeof(details));
   nandlog_end_session();
   if (pages_committed() > before)
      pending_committed();
   else if (!begun)
      pending_count = 0;
   if (begun)
   {
      current_epoch = library_epoch();
      pending_count = 0;
      last_full = false;
   }
}


// The Oracle ----------------------------------------------------------------------------------------------------------

static uint32_t good_blocks[4096], good_block_count;
static uint8_t injected[1 << 17];

static void list_good_blocks(uint32_t start_block)
{
   // The blocks an epoch begun at 'start_block' lays its positions over, by the driver's retired blocks
   good_block_count = 0;
   for (uint32_t steps = 0, block = start_block; steps < log_blocks(); ++steps)
   {
      if (!nandlog_chip_is_bad_block(block * ppb()))
         good_blocks[good_block_count++] = block;
      block = ((block + 1) < log_end_block()) ? (block + 1) : log_first_block();
   }
}

static uint32_t walk_records(const uint8_t *payload, uint32_t length, uint32_t epoch, bool mark, uint32_t *damaged, uint32_t *duplicates,
                             uint32_t *foreign, uint32_t *ids, uint32_t capacity)
{
   // Every record of a framed payload, checked against the content its id must have
   uint32_t offset = 0, count = 0;
   while (offset < length)
   {
      uint16_t data_length = 0;
      memcpy(&data_length, payload + offset, sizeof(data_length));
      const uint32_t total = NANDLOG_FRAMING_LENGTH_BYTES + 5 + data_length;
      const uint8_t *data = payload + offset + NANDLOG_FRAMING_LENGTH_BYTES + 5;
      uint32_t id = 0;
      if (((offset + total) > length) || (data_length < 4))
      {
         ++*damaged;
         return count;
      }
      memcpy(&id, data, sizeof(id));
      if (id >= next_id)
      {
         ++*damaged;
         return count;
      }
      fill_record(record, data_length, id);
      if (memcmp(record, data, data_length))
         ++*damaged;
      else
      {
         if (mark)
         {
            *duplicates += seen[id] ? 1 : 0;
            seen[id] = 1;
            *foreign += (id_epoch[id] != epoch) ? 1 : 0;
         }
         if (ids && (count < capacity))
            ids[count] = id;
         ++count;
      }
      offset += total;
   }
   return count;
}

static bool identity_kept(uint32_t epoch, uint32_t seq, const uint8_t *payload, uint32_t length)
{
   // G, for one page a read served
   uint32_t slot = ((epoch * 0x9E3779B1u) ^ (seq * 0x85EBCA77u)) & (IDENTITY_SLOTS - 1);
   while (identity[slot].used && ((identity[slot].epoch != epoch) || (identity[slot].seq != seq)))
      slot = (slot + 1) & (IDENTITY_SLOTS - 1);
   identity_t *entry = &identity[slot];
   const uint32_t crc = length ? (crc32_bits(payload, length) ^ length) : 0;
   if (!entry->used)
   {
      if (identity_used >= ((IDENTITY_SLOTS / 4) * 3))
         return true;
      ++identity_used;
      entry->used = 1;
      entry->epoch = epoch;
      entry->seq = seq;
      entry->data = (length > 0);
      entry->crc = crc;
      entry->bound = next_id;
      return true;
   }
   if (!length)
      return true;
   if (entry->data)
      return entry->crc == crc;
   uint32_t ids[256], unused = 0;
   const uint32_t count = walk_records(payload, length, epoch, false, &unused, &unused, &unused, ids, 256);
   for (uint32_t k = 0; (k < count) && (k < 256); ++k)
      if (ids[k] >= entry->bound)
         return false;
   entry->data = 1;
   entry->crc = crc;
   return true;
}

static bool span_holds_unstable_page(uint32_t first_page, uint32_t positions)
{
   // Whether a page of the read is one whose undecided bits read differently on every latch
   list_good_blocks(first_page / ppb());
   for (uint32_t position = 0; (position < positions) && (position < (good_block_count * ppb())); ++position)
      if (nandlog_sim_page_unstable((good_blocks[position / ppb()] * ppb()) + (position % ppb())))
         return true;
   return false;
}

static void audit_epoch(uint32_t index, const char *why)
{
   // One listed epoch, read end to end, and then held against the medium
   nandlog_epoch_info_t info;
   if (!nandlog_epoch_info(index, &info))
   {
      VIOLATION("[%s] epoch_info(%u) failed", why, index);
      return;
   }
   if (index && !nandlog_select_epoch(info.epoch))
   {
      VIOLATION("[%s] select_epoch(%u) failed", why, info.epoch);
      return;
   }
   if (!index)
      nandlog_select_current_epoch();

   static uint32_t seq_of[MAX_READ], length_of[MAX_READ];
   uint32_t positions = 0, declared = 0, delivered = 0, damaged = 0, duplicates = 0, foreign = 0, served = 0;
   nandlog_begin_reading(0, 0);
   nandlog_read_span(&positions, &declared);
   for (uint32_t i = 0; i < positions; ++i)
   {
      nandlog_page_header_t header;
      const uint32_t length = nandlog_retrieve_next_page(readback, &header);
      if (!identity_kept(info.epoch, header.seq, readback, length))
         VIOLATION("[%s] epoch %u: seq %u served other content than an earlier read served under it", why, info.epoch, header.seq);
      if (!length)
         continue;
      delivered += length;
      walk_records(readback, length, info.epoch, true, &damaged, &duplicates, &foreign, NULL, 0);
      if (served < MAX_READ)
      {
         seq_of[served] = header.seq;
         length_of[served++] = length;
      }
   }
   nandlog_end_reading();
   if ((delivered != declared) && !span_holds_unstable_page(info.first_page, positions))
      VIOLATION("[%s] epoch %u: the read declared %u bytes and delivered %u", why, info.epoch, declared, delivered);
   else if (delivered != declared)
      ++unrepeatable_reads;
   if (damaged || duplicates || foreign)
      VIOLATION("[%s] epoch %u: %u damaged, %u duplicated and %u foreign records", why, info.epoch, damaged, duplicates, foreign);

   // F: a few pages again, by sequence number
   for (uint32_t k = 0; served && (k < 3); ++k)
   {
      const uint32_t j = random_below(served);
      nandlog_page_header_t header;
      const uint32_t length = nandlog_retrieve_page_by_seq(seq_of[j], readback, &header);
      if (length != length_of[j])
         VIOLATION("[%s] epoch %u: seq %u came back by sequence number with %u bytes, from the read with %u", why, info.epoch, seq_of[j], length, length_of[j]);
   }

   // C: every valid page of the epoch at its own position, up to the last one, was delivered
   static uint8_t valid_at[MAX_POSITIONS];
   uint32_t last = 0;
   bool any = false;
   list_good_blocks(info.first_page / ppb());
   for (uint32_t position = 0; (position < (good_block_count * ppb())) && (position < MAX_POSITIONS); ++position)
   {
      const uint32_t page = (good_blocks[position / ppb()] * ppb()) + (position % ppb());
      nandlog_page_header_t header;
      valid_at[position] = !((injected[page / 8] >> (page % 8)) & 1) && raw_header(page, &header) && (header.epoch == info.epoch) &&
                           (header.seq == position) && header.payload_length && raw_payload_valid(page, &header);
      if (valid_at[position])
      {
         last = position;
         any = true;
      }
   }
   for (uint32_t position = 0; any && (position <= last); ++position)
      if (valid_at[position])
      {
         const uint32_t page = (good_blocks[position / ppb()] * ppb()) + (position % ppb());
         const uint8_t *raw = nandlog_sim_raw_page(page);
         uint32_t ids[256], unused = 0;
         const uint32_t count = walk_records(raw + sizeof(nandlog_page_header_t), ((const nandlog_page_header_t*)raw)->payload_length, info.epoch, false,
                                             &unused, &unused, &unused, ids, 256);
         bool missing = false;
         for (uint32_t k = 0; (k < count) && (k < 256); ++k)
            missing = missing || (!seen[ids[k]] && !excused[ids[k]]);
         if (missing)
         {
            VIOLATION("[%s] epoch %u (start block %u, %u positions read): the valid page at position %u (page %u) was not delivered",
                      why, info.epoch, info.first_page / ppb(), positions, position, page);
            break;
         }
      }
}

static void audit(const char *why)
{
   ++audits;
   memset(seen, 0, sizeof(seen));
   nandlog_begin_session();
   const uint32_t count = nandlog_epoch_count();
   for (uint32_t index = 0; index < count; ++index)
      audit_epoch(index, why);
   nandlog_select_current_epoch();
   nandlog_end_session();

   // A: the durable records of the current epoch, all of them
   uint32_t lost = 0, first_lost = 0;
   for (uint32_t id = 0; id < next_id; ++id)
      if (durable[id] && !excused[id] && current_epoch && (id_epoch[id] == current_epoch) && !seen[id])
      {
         if (!lost)
            first_lost = id;
         ++lost;
      }
   if (lost)
      VIOLATION("[%s] %u durable records of the current epoch %u were not delivered (the first id %u)", why, lost, current_epoch, first_lost);

   // E: no second program outside a retired block since the last audit, and none of a page that drifted
   const uint32_t repeated = repeated_programs_outside_retired_blocks();
   if (repeated)
      VIOLATION("[%s] %u pages programmed twice outside a retired block, the first page %u (block %u)", why, repeated, repeated_page, repeated_page / ppb());
   nandlog_sim_clear_repeated_programs();
}


// Faults --------------------------------------------------------------------------------------------------------------

static void make_unreadable(uint32_t page)
{
   nandlog_sim_corrupt(page, 4, 0x5A);
   nandlog_sim_set_ecc_status(page, NANDLOG_SIM_ECC_UNCORRECTABLE);
}

static void lose_newest_ring_copy(void)
{
   // The first copy of the newest metadata slot becomes unreadable, while its second still reads: one page lost
   uint32_t newest = 0, newest_page = 0;
   for (uint32_t page = 0; page < (METADATA_RING_BLOCKS * ppb()); page += 2)
   {
      const uint8_t *raw = nandlog_sim_raw_page(page + 1);
      nandlog_meta_header_t header;
      nandlog_meta_extension_t extension;
      memcpy(&header, raw, sizeof(header));
      memcpy(&extension, raw + sizeof(header), sizeof(extension));
      if ((header.magic == NANDLOG_META_MAGIC) && (crc32_bits(raw, NANDLOG_META_EXTENSION_CRC_BYTES) == extension.extension_crc) &&
          (extension.sequence > newest))
      {
         newest = extension.sequence;
         newest_page = page;
      }
   }
   if (newest)
      make_unreadable(newest_page);
}

#ifndef SIM_CHIP_WINBOND
static void lose_newest_table_copy(void)
{
   // The same for the newest version of the bad-block table, wherever the driver keeps it
   const uint32_t first = table_block(0) * ppb(), blocks = SIM_DEAD_RESERVE_BLOCKS ? (log_end_block() - table_search_first_block()) : TABLE_BLOCKS;
   uint32_t newest = 0, newest_page = 0;
   for (uint32_t page = first; page < (first + (blocks * ppb())); page += 2)
   {
      const uint8_t *raw = nandlog_sim_raw_page(page + 1);
      uint32_t crc = 0, version = 0, count = 0;
      memcpy(&crc, raw + 4, sizeof(crc));
      memcpy(&version, raw + 8, sizeof(version));
      memcpy(&count, raw + 12, sizeof(count));
      if (!memcmp(raw, "BBT3", 4) && (count <= 256) && (crc32_bits(raw + 8, 8 + (4 * count)) == crc) && (version > newest))
      {
         newest = version;
         newest_page = page;
      }
   }
   if (newest)
      make_unreadable(newest_page);
}
#endif

static void inject_fault(void)
{
   const uint32_t block = log_first_block() + random_below(log_blocks()), reserve = log_end_block();
   switch (random_below(10))
   {
      case 0:     // a log page that refuses its program
         if (faults.num_unwritable_pages < NANDLOG_SIM_MAX_FAULT_PAGES)
            faults.unwritable_pages[faults.num_unwritable_pages++] = (block * ppb()) + random_below(ppb());
         break;
      case 1:     // a log block that refuses its erase
         if (faults.num_unerasable_blocks < NANDLOG_SIM_MAX_FAULT_BLOCKS)
            faults.unerasable_blocks[faults.num_unerasable_blocks++] = block;
         break;
      case 2:     // a log block that refuses every program
         if (faults.num_unwritable_blocks < 4)
            faults.unwritable_blocks[faults.num_unwritable_blocks++] = block;
         break;
      case 3:     // a ring page that refuses its program
         if (faults.num_unwritable_pages < NANDLOG_SIM_MAX_FAULT_PAGES)
            faults.unwritable_pages[faults.num_unwritable_pages++] = (random_below(METADATA_RING_BLOCKS) * ppb()) + random_below(ppb());
         break;
      case 4:     // a ring block that refuses its erase, now and then
         if ((faults.num_unerasable_blocks < NANDLOG_SIM_MAX_FAULT_BLOCKS) && !random_below(3))
            faults.unerasable_blocks[faults.num_unerasable_blocks++] = random_below(METADATA_RING_BLOCKS);
         break;
      case 5:     // a page of the table's blocks that refuses its program
         if (faults.num_unwritable_pages < NANDLOG_SIM_MAX_FAULT_PAGES)
            faults.unwritable_pages[faults.num_unwritable_pages++] = (table_block(random_below(64)) * ppb()) + random_below(ppb());
         (void)reserve;
         break;
      case 6:     // a log page the part corrects only at the limit of its ECC
#ifndef SIM_CHIP_WINBOND
         nandlog_sim_set_ecc_status((block * ppb()) + random_below(ppb()), NANDLOG_SIM_ECC_AT_LIMIT);
#endif
         break;
      case 7:     // a valid log page that becomes unreadable; its records are excused
      {
         const uint32_t page = (block * ppb()) + random_below(ppb());
         nandlog_page_header_t header;
         if (raw_header(page, &header) && header.payload_length && raw_payload_valid(page, &header))
         {
            uint32_t ids[256], unused = 0;
            const uint32_t count = walk_records(nandlog_sim_raw_page(page) + sizeof(header), header.payload_length, 0, false, &unused, &unused, &unused, ids, 256);
            for (uint32_t k = 0; (k < count) && (k < 256); ++k)
               excused[ids[k]] = 1;
            injected[page / 8] |= (uint8_t)(1u << (page % 8));
            nandlog_sim_set_ecc_status(page, NANDLOG_SIM_ECC_UNCORRECTABLE);
         }
         break;
      }
      case 8:
         lose_newest_ring_copy();
         break;
      default:
#ifndef SIM_CHIP_WINBOND
         lose_newest_table_copy();
#endif
         break;
   }
   apply_faults();
}


// The Run -------------------------------------------------------------------------------------------------------------

static void boot(void)
{
   // A boot a cut interrupts is a boot like any other: power comes back and it runs again
   for (uint32_t attempt = 0; attempt < 10; ++attempt)
   {
      nandlog_deinit();
      if (setjmp(cut_jump) == 0)
      {
         if (!nandlog_init())
         {
            printf("nandlog_init() failed\n");
            exit(1);
         }
         nandlog_sim_power_cut_disarm();
         pending_count = 0;
         current_epoch = library_epoch();
         return;
      }
      ++cuts_fired;
      last_call = "a cut during a boot";
   }
}

int main(int argc, char **argv)
{
   const uint64_t seed = (argc > 1) ? strtoull(argv[1], NULL, 10) : 1;
   const uint32_t steps = (argc > 2) ? (uint32_t)atoi(argv[2]) : 3000;
   const uint32_t cut_rate = (argc > 3) ? (uint32_t)atoi(argv[3]) : 30;
   const uint32_t fault_rate = (argc > 4) ? (uint32_t)atoi(argv[4]) : 600;
   const uint32_t mode = (argc > 5) ? (uint32_t)atoi(argv[5]) : 0;
   setvbuf(stdout, NULL, _IOLBF, 0);
   random_state = (seed * 0x9E3779B97F4A7C15ULL) + 1;
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_sim_set_verbose(getenv("NANDLOG_VERBOSE") != NULL);
   nandlog_sim_seed(seed);
   memset(&faults, 0, sizeof(faults));
   apply_faults();
   for (uint32_t b = log_end_block() - SIM_DEAD_LOG_BLOCKS; b < (log_end_block() + SIM_DEAD_RESERVE_BLOCKS); ++b)
      nandlog_sim_kill_block(b);

   // Now and then a factory-bad block in the log region, and one in the ring
   if (random_below(2))
      nandlog_sim_raw_page((log_first_block() + random_below(log_blocks())) * ppb())[FACTORY_MARKER_OFFSET(page_bytes())] = 0x00;
   if (!random_below(3))
      nandlog_sim_raw_page(random_below(METADATA_RING_BLOCKS) * ppb())[FACTORY_MARKER_OFFSET(page_bytes())] = 0x00;
   boot();
   call_begin_epoch();

   bool armed = false;
   for (steps_done = 0; (steps_done < steps) && (violations <= 20); ++steps_done)
   {
      if (!armed && cut_rate && !random_below(cut_rate))
      {
         static const nandlog_sim_cut_model_t models[4] = { NANDLOG_SIM_CUT_BOUNDARY, NANDLOG_SIM_CUT_PREFIX, NANDLOG_SIM_CUT_BITWISE, NANDLOG_SIM_CUT_UNSTABLE };
         static const double q_values[6] = { 0.0005, 0.01, 0.2, 0.5, 0.8, 0.95 };
         nandlog_sim_power_cut(1 + random_below(6), models[random_below(4)], q_values[random_below(6)], &cut_jump);
         armed = true;
      }
      if (setjmp(cut_jump) != 0)
      {
         ++cuts_fired;
         armed = false;
         boot();
         audit("after a cut");
         continue;
      }

      const uint32_t call = random_below(1000);
      if (call < 700)
      {
         last_call = "store";
         const uint32_t kind = random_below(10);
         uint32_t length = page_record_bytes();
         if ((mode & 1) && (kind < 7))
            length -= random_below(3);
         else if (kind < 5)
            length = 4 + random_below(60);
         else if (kind < 8)
            length = 4 + random_below(page_record_bytes());
         const uint32_t timestamp = random_below(50) ? (now += random_below(30)) : (now - random_below(2000));
         call_store(length, timestamp);
      }
      else if (call < 800)
      {
         last_call = "flush(true)";
         call_flush(true);
      }
      else if (call < 900)
      {
         last_call = "flush(false)";
         call_flush(false);
      }
      else if (call < 915)
      {
         if (!(mode & 2) || !random_below(30))
         {
            last_call = "begin_epoch";
            call_begin_epoch();
         }
      }
      else if (call < 935)
      {
         last_call = "reboot";
         nandlog_sim_power_cut_disarm();
         armed = false;
         boot();
         audit("after a reboot");
      }
      else if (call < 945)
      {
         last_call = "audit";
         nandlog_sim_power_cut_disarm();
         armed = false;
         audit("idle");
      }
      else if ((call < 952) && nandlog_sim_weak_pages())
      {
         last_call = "drift";
         nandlog_sim_power_cut_disarm();
         armed = false;
         nandlog_deinit();
         drifts += nandlog_sim_drift(random_below(2) ? NANDLOG_SIM_DRIFT_PARTIAL : NANDLOG_SIM_DRIFT_BLANK, random_below(2) ? 1.0 : 0.5);
         boot();
         audit("after a drift");
      }
      else if (fault_rate && !random_below((fault_rate / 50) + 1))
      {
         last_call = "a fault";
         inject_fault();
      }

      // A full region gets a new epoch soon
      if (last_full && !random_below(4))
      {
         last_call = "begin_epoch on a full region";
         call_begin_epoch();
      }
   }
   nandlog_sim_power_cut_disarm();
   boot();
   audit("final");
   printf("FUZZ seed %llu steps %u records %u cuts %u audits %u retired %u epoch %u violations %u unrepeatable %u drifted %u\n", (unsigned long long)seed,
          steps_done, next_id, cuts_fired, audits, nandlog_bad_block_count(), current_epoch, violations, unrepeatable_reads, drifts);
   nandlog_deinit();
   nandlog_sim_destroy();
   return violations ? 1 : 0;
}
