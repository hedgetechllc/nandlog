// Bounded-exhaustive check of write-head recovery, run against the RAM-backed port. `make exhaustive` builds and runs
// it.
//
// The campaign and the fuzzer reach the states a part can be in by running the library into power cuts. This goes
// the other way: it lays the states down directly and enumerates every combination of them over the positions just
// below the write head, then boots the shipping core and driver over each and holds what they do to the invariant
// and the contract. The part is a toy -- four pages a block, five log blocks -- so that a window of four positions
// spans a block boundary, and a handful of scenarios put it everywhere the layout has an edge: part-way into a
// block, at a block the epoch has not entered yet (with what an earlier epoch or an interrupted erase left there),
// across a retired block, at the epoch's first block, and across the wrap of the region.
//
// Each position of the window below the head h is one of
//
//   V   a page the epoch committed, whole                 R   one that has since rotted: unreadable
//   L   a page a cut tore, listed as passed over          H   one torn with a header that checks over a payload
//                                                             that does not, listed
//   E   a torn page, listed, since drifted blank          S   a listed position the epoch has since tombstoned
//   B   a committed page that reads blank in every bit -- a retention failure, outside the fault model
//
// and the position directly below the head may also be a page torn by the last cut before this boot, which no boot
// has yet listed: u (unreadable) or h (a header that checks, a payload that does not). Positions below the window
// hold committed pages, and those at and after the head hold what the scenario says. The epoch's metadata slot lists
// exactly the positions in L, H, E and S.
//
// For every combination the device boots, and then:
//
//   - the write head the boot found is h -- or, where the head's block holds an erase's remains, the start of a
//     later block, with every position from h to it listed as passed over;
//   - the part satisfies the invariant (I1)-(I3) of nandlog.c, judged from the medium and from the newest metadata
//     slot read by this file's own parser: every position below h is listed or not blank, no valid page of the epoch
//     sits off its own position, h is blank and unlisted, and nothing at or after it is listed or holds the epoch's
//     page for that position;
//   - it logs six pages, reboots, and every committed page and every new one is delivered, intact, once;
//   - identity: a position below h serves what it served before the boot -- its committed page, or a gap -- and
//     never new records; a committed page's bytes are untouched, and any other position below h holds what it held
//     or a tombstone with its own sequence number.
//
// A combination holding B is outside the fault model: what such a page held is gone, and is excused, but the head
// must still land in the right place and nothing new may be served below it. Those combinations are counted
// separately and do not fail the check.
//
// A build with SIM_DEAD_RESERVE_BLOCKS kills that many blocks from the start of the driver's reserve, and
// SIM_DEAD_LOG_BLOCKS the region's top blocks below it, on every part it creates, as one board was found. It runs the
// metadata part only (`metadata`), with traces of its own: the table moved below the reserve on a part's first boot,
// a retirement that rotates it within its pair there, its pair failing so the marks take over mid-run, a boot that
// moves it to a new pair with an epoch in the region, and the pair failing under a block that refuses even its mark,
// so the log moves the table to a new pair mid-logging. A build whose search for the table reaches only dead blocks runs
// the same traces with nowhere to move it: every retirement is by a mark, and the last trace's block, which takes none,
// stops the log, safely, until a reboot.
//
// Usage: nandlog_sim_exhaustive [scenario [combination after]]   one combination, e.g. "boundary LRRR 2", with the
//                                                                library's diagnostics
//        nandlog_sim_exhaustive metadata                         the metadata paths only
//        nandlog_sim_exhaustive <trace> <op> <model> <damage>    one metadata trial, with the library's diagnostics

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nandlog.h"
#include "nandlog_chip.h"
#include "nandlog_port_sim.h"

static const uint8_t DEVICE_ID[3] = { 0x8D, 0x00, 0x00 };

#define METADATA_RING_BLOCKS                        8
#define WINDOW                                      4
#define NEW_PAGES                                   6
#define MAX_POSITIONS                               64

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

static uint8_t record[NANDLOG_MAX_PAGE_SIZE_BYTES], readback[NANDLOG_MAX_PAGE_SIZE_BYTES], image[NANDLOG_MAX_PAGE_SIZE_BYTES + 512];
static uint8_t before[MAX_POSITIONS][NANDLOG_MAX_PAGE_SIZE_BYTES];


// The Part ------------------------------------------------------------------------------------------------------------

static uint32_t ppb(void)               { return nandlog_chip_geometry()->pages_per_block; }
static uint32_t page_bytes(void)        { return nandlog_chip_geometry()->page_size_bytes; }
static uint32_t stride(void)            { return page_bytes() + nandlog_chip_geometry()->spare_size_bytes; }
static uint32_t log_first_block(void)   { return METADATA_RING_BLOCKS; }
static uint32_t log_end_block(void)     { return nandlog_chip_geometry()->block_count - nandlog_chip_geometry()->reserved_blocks; }
static uint32_t prefix_bytes(void)      { return NANDLOG_RECORD_FRAMING ? NANDLOG_FRAMING_LENGTH_BYTES : 0; }
static uint32_t page_record(void)       { return nandlog_data_bytes_per_page() - prefix_bytes() - 5; }

static uint32_t crc32_bits(const uint8_t *bytes, uint32_t length)
{
   // Written here rather than borrowed from the library, so the check does not trust the code under test
   uint32_t crc = 0xFFFFFFFF;
   for (uint32_t i = 0; i < length; ++i)
   {
      crc ^= bytes[i];
      for (uint32_t bit = 0; bit < 8; ++bit)
         crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
   }
   return ~crc;
}

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

static uint32_t table_search_first_block(void)
{
   const uint32_t blocks = nandlog_chip_geometry()->block_count;
   return (TABLE_SEARCH_BLOCKS < blocks) ? (blocks - TABLE_SEARCH_BLOCKS) : 0;
}

static void create_part(void)
{
   // A new part, with the build's dead blocks
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   for (uint32_t b = log_end_block() - SIM_DEAD_LOG_BLOCKS; b < (log_end_block() + SIM_DEAD_RESERVE_BLOCKS); ++b)
      nandlog_sim_kill_block(b);
}

static bool holds_table(uint32_t block)
{
   // A block of a pair below the reserve the table was, or is, kept in: one of the two carries its magic first
   const uint32_t pair = block & ~1u;
   bool magic = false;
   for (uint32_t page = pair * ppb(); !magic && (page < ((pair + 2) * ppb())); page += ((page % ppb()) ? (ppb() - 1) : 1))
      magic = !memcmp(nandlog_sim_raw_page(page), "BBT3", 4);
   return (block >= table_search_first_block()) && (block < log_end_block()) && magic;
}

static bool raw_blank(uint32_t page)
{
   const uint8_t *raw = nandlog_sim_raw_page(page);
   for (uint32_t i = 0; i < stride(); ++i)
      if (raw[i] != 0xFF)
         return false;
   return true;
}

static bool raw_header(uint32_t page, nandlog_page_header_t *header)
{
   const uint8_t *raw = nandlog_sim_raw_page(page);
   memcpy(header, raw, sizeof(*header));
   return ((header->magic == NANDLOG_PAGE_MAGIC) || (header->magic == NANDLOG_PAGE_MAGIC_FRAMED)) &&
          (crc32_bits(raw, NANDLOG_PAGE_HEADER_CRC_BYTES) == header->header_crc);
}


// Laying Down States --------------------------------------------------------------------------------------------------

#define EPOCH                                       2
#define OLD_EPOCH                                   1

static void build_page(uint32_t epoch, uint32_t seq, uint32_t id, bool tombstone)
{
   // A page in 'image' exactly as the library writes one: one page-filling record, or a tombstone
   nandlog_page_header_t header;
   memset(image, 0xFF, stride());
   memset(&header, 0, sizeof(header));
   header.magic = NANDLOG_PAGE_MAGIC_THIS_BUILD;
   header.epoch = epoch;
   header.seq = seq;
   if (tombstone)
   {
      header.first_timestamp = header.last_timestamp = NANDLOG_NO_TIMESTAMP;
      header.payload_crc = crc32_bits(NULL, 0);
   }
   else
   {
      uint8_t *payload = image + sizeof(header);
      const uint32_t length = page_record(), timestamp = 1000 + id;
      if (NANDLOG_RECORD_FRAMING)
      {
         const uint16_t framed = (uint16_t)length;
         memcpy(payload, &framed, sizeof(framed));
      }
      payload[prefix_bytes()] = 7;
      memcpy(payload + prefix_bytes() + 1, &timestamp, sizeof(timestamp));
      fill_record(payload + prefix_bytes() + 5, length, id);
      header.first_timestamp = header.last_timestamp = timestamp;
      header.payload_length = (uint16_t)(prefix_bytes() + 5 + length);
      header.record_count = 1;
      header.payload_crc = crc32_bits(payload, header.payload_length);
   }
   header.header_crc = crc32_bits((const uint8_t*)&header, NANDLOG_PAGE_HEADER_CRC_BYTES);
   memcpy(image, &header, sizeof(header));
}

typedef enum { S_V, S_R, S_L, S_H, S_E, S_S, S_B, S_u, S_h, NUM_STATES } state_t;
static const char state_names[NUM_STATES] = { 'V', 'R', 'L', 'H', 'E', 'S', 'B', 'u', 'h' };

static bool listed_state(state_t s)      { return (s == S_L) || (s == S_H) || (s == S_E) || (s == S_S); }

static void lay_down(uint32_t page, state_t state, uint32_t seq)
{
   // One position's state on the medium. Torn pages hold a subset of the bits their program was to set; a rotted
   // page has lost bits from its header and reads uncorrectable
   uint8_t *raw = nandlog_sim_raw_page(page);
   build_page(EPOCH, seq, seq + 1, state == S_S);
   switch (state)
   {
      case S_V:
      case S_S:
         memcpy(raw, image, stride());
         break;
      case S_R:
         memcpy(raw, image, stride());
         raw[4] &= 0x5A;
         raw[12] &= 0xA5;
         nandlog_sim_set_ecc_status(page, NANDLOG_SIM_ECC_UNCORRECTABLE);
         break;
      case S_L:
      case S_u:
         for (uint32_t i = 0; i < stride(); ++i)
            raw[i] = (uint8_t)(image[i] | ((i % 3) ? 0x00 : 0xFF) | 0x11);
         break;
      case S_H:
      case S_h:
         for (uint32_t i = 0; i < stride(); ++i)
            raw[i] = (i < (sizeof(nandlog_page_header_t) + 64)) ? image[i] : (uint8_t)(image[i] | 0x24);
         break;
      default:
         memset(raw, 0xFF, stride());
         break;
   }
}

static void write_slot(uint32_t start_page, const uint32_t *listed, uint32_t count, uint32_t retired)
{
   // The epoch's metadata slot, both copies, in the first pair of the ring, laid out as nandlog.h describes it
   nandlog_meta_header_t header;
   nandlog_meta_extension_t extension;
   static const uint8_t details[8] = "toy";
   memset(image, 0xFF, stride());
   memset(&header, 0, sizeof(header));
   header.magic = NANDLOG_META_MAGIC;
   header.epoch = EPOCH;
   header.log_start_page = start_page;
   header.details_length = sizeof(details);
   header.format_version = NANDLOG_FORMAT_VERSION;
   header.details_crc = crc32_bits(details, sizeof(details));
   header.header_crc = crc32_bits((const uint8_t*)&header, NANDLOG_META_HEADER_CRC_BYTES);
   header.reserved = 0xFFFFFFFF;
   memset(&extension, 0, sizeof(extension));
   extension.sequence = 5;
   extension.head_failed_page = 0xFFFFFFFF;
   extension.retired_blocks = retired;
   extension.passed_over_count = (uint16_t)count;
   extension.passed_over_crc = crc32_bits((const uint8_t*)listed, count * sizeof(uint32_t));
   extension.sealed = 0xFF;
   extension.reserved = 0xFFFF;
   memcpy(image, &header, sizeof(header));
   memcpy(image + sizeof(header), &extension, sizeof(extension));
   extension.extension_crc = crc32_bits(image, NANDLOG_META_EXTENSION_CRC_BYTES);
   memcpy(image + sizeof(header), &extension, sizeof(extension));
   memcpy(image + sizeof(header) + sizeof(extension), details, sizeof(details));
   memcpy(image + sizeof(header) + sizeof(extension) + sizeof(details), listed, count * sizeof(uint32_t));
   for (uint32_t copy = 0; copy < 2; ++copy)
      memcpy(nandlog_sim_raw_page(copy), image, stride());
}

static uint32_t newest_list(uint32_t *listed, uint32_t capacity)
{
   // The passed-over positions of the newest slot the ring holds whole, by this file's reading of the format: runs,
   // each a first position in the low 24 bits and a length less one in the high 8, expanded here
   uint32_t best = 0, count = 0;
   for (uint32_t page = 0; page < (METADATA_RING_BLOCKS * ppb()); ++page)
   {
      const uint8_t *raw = nandlog_sim_raw_page(page);
      nandlog_meta_header_t header;
      nandlog_meta_extension_t extension;
      memcpy(&header, raw, sizeof(header));
      memcpy(&extension, raw + sizeof(header), sizeof(extension));
      if ((header.magic != NANDLOG_META_MAGIC) || (crc32_bits(raw, NANDLOG_META_HEADER_CRC_BYTES) != header.header_crc) ||
          (crc32_bits(raw, NANDLOG_META_EXTENSION_CRC_BYTES) != extension.extension_crc) || (extension.sequence <= best))
         continue;
      const uint8_t *list = raw + sizeof(header) + sizeof(extension) + header.details_length;
      if ((extension.passed_over_count > capacity) || (crc32_bits(list, extension.passed_over_count * sizeof(uint32_t)) != extension.passed_over_crc))
         continue;
      best = extension.sequence;
      count = 0;
      for (uint32_t i = 0; i < extension.passed_over_count; ++i)
      {
         uint32_t run;
         memcpy(&run, list + (i * sizeof(run)), sizeof(run));
         for (uint32_t k = 0; (k < ((run >> 24) + 1)) && (count < capacity); ++k)
            listed[count++] = (run & 0x00FFFFFF) + k;
      }
   }
   return count;
}

static bool listed_in_newest(uint32_t position)
{
   uint32_t listed[256];
   const uint32_t count = newest_list(listed, 256);
   for (uint32_t i = 0; i < count; ++i)
      if (listed[i] == position)
         return true;
   return false;
}


// Scenarios -----------------------------------------------------------------------------------------------------------

typedef enum { AFTER_BLANK, AFTER_STALE, AFTER_ERASE_REMAINS, NUM_AFTER } after_t;

typedef struct
{
   const char *name, *description;
   uint32_t start_block;                  // the epoch's first block, as an offset into the log region
   uint32_t retired_block;                // a block retired before the epoch began, as an offset, or ~0
   uint32_t head;                         // the true write head
   uint32_t window;                       // positions examined below it
   bool unentered;                        // the head is the first position of a block the epoch has not entered
} scenario_t;

static const scenario_t scenarios[] =
{
   { "mid",      "the head part-way into a block, the window across the block boundary below it", 0, ~0u, 6, WINDOW, false },
   { "boundary", "the head at a block the epoch has not entered: erased, stale, or an erase's remains", 0, ~0u, 8, WINDOW, true },
   { "retired",  "the window across a block retired before the epoch began",                        0, 1,   5, WINDOW, false },
   { "first",    "the window down to the epoch's first position",                                   0, ~0u, 2, 2,      false },
   { "wrap",     "the window across the wrap of the region",                                        3, ~0u, 9, WINDOW, false },
};
#define NUM_SCENARIOS                               (sizeof(scenarios) / sizeof(scenarios[0]))

static uint32_t position_pages[MAX_POSITIONS], log_blocks_in_order[16], passed_over_heads;
static const char *only_combination;
static uint32_t only_after;

static void lay_out_positions(const scenario_t *sc)
{
   // Good positions of the epoch, over the log region from its first block, wrapping, stepping over the retired one
   const uint32_t blocks = log_end_block() - log_first_block();
   uint32_t good = 0;
   for (uint32_t i = 0; i < blocks; ++i)
   {
      const uint32_t offset = (sc->start_block + i) % blocks;
      if (offset != sc->retired_block)
         log_blocks_in_order[good++] = log_first_block() + offset;
   }
   for (uint32_t p = 0; (p < MAX_POSITIONS) && ((p / ppb()) < good); ++p)
      position_pages[p] = (log_blocks_in_order[p / ppb()] * ppb()) + (p % ppb());
}


// One Combination -----------------------------------------------------------------------------------------------------

typedef struct
{
   uint32_t combinations, failed, outside_model, outside_model_failed;
   uint32_t head_wrong, invariant_broken, contract_broken, identity_broken;
} tally_t;

static uint32_t current_positions(void)
{
   uint32_t positions = 0;
   nandlog_begin_session();
   nandlog_select_current_epoch();
   nandlog_begin_reading(0, 0);
   nandlog_read_span(&positions, NULL);
   nandlog_end_reading();
   nandlog_end_session();
   return positions;
}

static bool audit(const scenario_t *sc, const state_t *states, uint32_t found, uint32_t first_new, uint32_t new_pages, char *why, size_t why_size)
{
   // Every committed page and every new one delivered, intact and once; below the head, a committed position
   // serves its page and any other a gap, as do the positions the boot passed over above it. A committed page that
   // reads blank is gone, and excused
   static uint8_t seen[1200];
   memset(seen, 0, sizeof(seen));
   const uint32_t window_start = sc->head - sc->window;
   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   uint32_t positions = 0;
   nandlog_read_span(&positions, NULL);
   for (uint32_t i = 0; i < positions; ++i)
   {
      nandlog_page_header_t header;
      const uint32_t length = nandlog_retrieve_next_page(readback, &header);
      if (!length)
         continue;
      uint32_t id = 0;
      uint16_t data_length = (uint16_t)page_record();
      if (NANDLOG_RECORD_FRAMING)
         memcpy(&data_length, readback, sizeof(data_length));
      memcpy(&id, readback + prefix_bytes() + 5, sizeof(id));
      fill_record(record, data_length, id);
      if ((id >= sizeof(seen)) || memcmp(record, readback + prefix_bytes() + 5, data_length))
      {
         snprintf(why, why_size, "position %u delivered a damaged record", i);
         nandlog_end_reading();
         nandlog_end_session();
         return false;
      }
      if (seen[id]++)
      {
         snprintf(why, why_size, "record %u delivered twice", id);
         nandlog_end_reading();
         nandlog_end_session();
         return false;
      }
      if ((i < found) && (id != (i + 1)))
      {
         snprintf(why, why_size, "position %u below the head served record %u", i, id);
         nandlog_end_reading();
         nandlog_end_session();
         return false;
      }
      if ((i >= window_start) && (i < sc->head) && (states[i - window_start] != S_V))
      {
         snprintf(why, why_size, "position %u (%c) served data", i, state_names[states[i - window_start]]);
         nandlog_end_reading();
         nandlog_end_session();
         return false;
      }
   }
   nandlog_end_reading();
   nandlog_end_session();
   for (uint32_t p = 0; p < sc->head; ++p)
   {
      const bool committed = (p < window_start) || (states[p - window_start] == S_V);
      if (committed && !seen[p + 1])
      {
         snprintf(why, why_size, "committed position %u not delivered", p);
         return false;
      }
   }
   for (uint32_t k = 0; k < new_pages; ++k)
      if (!seen[first_new + k])
      {
         snprintf(why, why_size, "new page %u of %u not delivered", k, new_pages);
         return false;
      }
   return true;
}

static bool invariant_holds(const scenario_t *sc, const state_t *states, uint32_t head, char *why, size_t why_size)
{
   // (I1)-(I3), from the medium and the newest slot. A committed page that reads blank breaks (I1) by itself -- it is
   // outside the model -- so it is excused here, and held only to what the contract asks
   uint32_t listed[256];
   const uint32_t count = newest_list(listed, 256);
   for (uint32_t p = 0; p < MAX_POSITIONS; ++p)
   {
      if ((p / ppb()) >= ((log_end_block() - log_first_block()) - ((sc->retired_block != ~0u) ? 1 : 0)))
         break;
      bool on_list = false;
      for (uint32_t i = 0; i < count; ++i)
         on_list = on_list || (listed[i] == p);
      const uint32_t page = position_pages[p];
      nandlog_page_header_t header;
      const bool valid = raw_header(page, &header);
      if (valid && (header.epoch == EPOCH) && (header.seq != p) && (p < head))
      {
         snprintf(why, why_size, "(I1) position %u holds the epoch's page for %u", p, header.seq);
         return false;
      }
      const bool excused = (p >= (sc->head - sc->window)) && (p < sc->head) && (states[p - (sc->head - sc->window)] == S_B);
      if ((p < head) && !on_list && !excused && raw_blank(page))
      {
         snprintf(why, why_size, "(I1) position %u below the head is blank and not listed", p);
         return false;
      }
      if ((p == head) && (on_list || !raw_blank(page)))
      {
         snprintf(why, why_size, "(I2) the head's position %u is %s", p, on_list ? "listed" : "not blank");
         return false;
      }
      if ((p >= head) && (on_list || (valid && (header.epoch == EPOCH) && (header.seq == p))))
      {
         snprintf(why, why_size, "(I3) position %u at or after the head is %s", p, on_list ? "listed" : "the epoch's own page");
         return false;
      }
   }
   return true;
}

static bool identity_holds(const scenario_t *sc, const state_t *states, char *why, size_t why_size)
{
   // Below the head, a committed page's bytes are untouched; any other position holds what it held, or a tombstone
   // with its own sequence number
   const uint32_t window_start = sc->head - sc->window;
   for (uint32_t p = 0; p < sc->head; ++p)
   {
      const uint8_t *raw = nandlog_sim_raw_page(position_pages[p]);
      if (!memcmp(raw, before[p], page_bytes()))
         continue;
      const bool committed = (p < window_start) || (states[p - window_start] == S_V);
      nandlog_page_header_t header;
      if (!committed && raw_header(position_pages[p], &header) && (header.epoch == EPOCH) && (header.seq == p) && !header.payload_length)
         continue;
      snprintf(why, why_size, "position %u below the head was programmed with new content", p);
      return false;
   }
   return true;
}

static bool run_combination(const scenario_t *sc, const state_t *states, after_t after, char *why, size_t why_size, uint32_t *failure_kind)
{
   const uint32_t window_start = sc->head - sc->window;
   uint32_t listed[MAX_POSITIONS], count = 0;
   nandlog_deinit();
   nandlog_sim_restore();
   for (uint32_t p = 0; p < sc->head; ++p)
   {
      const state_t state = (p < window_start) ? S_V : states[p - window_start];
      lay_down(position_pages[p], state, p);
      if (listed_state(state))
         listed[count++] = p;
      memcpy(before[p], nandlog_sim_raw_page(position_pages[p]), page_bytes());
   }

   // At and after the head: blank, unless the head's block is one the epoch has not entered
   if (sc->unentered)
      for (uint32_t k = 0; k < ppb(); ++k)
      {
         uint8_t *raw = nandlog_sim_raw_page(position_pages[sc->head + k]);
         if (after == AFTER_STALE)
         {
            build_page(OLD_EPOCH, 40 + k, 900 + k, false);
            memcpy(raw, image, stride());
         }
         else if (after == AFTER_ERASE_REMAINS)
         {
            build_page(OLD_EPOCH, 40 + k, 900 + k, false);
            for (uint32_t i = 0; i < stride(); ++i)
               raw[i] = (uint8_t)(image[i] | (((i * 7) % 5) ? 0xF0 : 0x0F));
         }
      }
   write_slot(position_pages[0] - (position_pages[0] % ppb()), listed, count, (sc->retired_block != ~0u) ? 1 : 0);

   nandlog_sim_clear_repeated_programs();
   if (!nandlog_init())
   {
      snprintf(why, why_size, "nandlog_init() failed");
      *failure_kind = 2;
      return false;
   }
   const uint32_t head = current_positions();
   if (only_combination)
   {
      uint32_t listed[256];
      const uint32_t count = newest_list(listed, 256);
      printf("  newest slot lists %u position(s):", count);
      for (uint32_t i = 0; i < count; ++i)
         printf(" %u", listed[i]);
      printf("\n");
   }
   bool passed_over = (head > sc->head) && sc->unentered && !(head % ppb());
   for (uint32_t p = sc->head; passed_over && (p < head); ++p)
      passed_over = listed_in_newest(p);
   if ((head != sc->head) && !passed_over)
   {
      snprintf(why, why_size, "the boot found the head at %u, not %u", head, sc->head);
      *failure_kind = 0;
      return false;
   }
   passed_over_heads += passed_over ? 1 : 0;
   if (!invariant_holds(sc, states, head, why, why_size))
   {
      *failure_kind = 1;
      return false;
   }
   for (uint32_t k = 0; k < NEW_PAGES; ++k)
   {
      fill_record(record, page_record(), 1000 + k);
      nandlog_store_record(7, 2000 + k, record, page_record());
   }
   nandlog_flush(true);
   nandlog_deinit();
   nandlog_init();
   if (!audit(sc, states, head, 1000, NEW_PAGES, why, why_size))
   {
      *failure_kind = 2;
      return false;
   }
   if (!identity_holds(sc, states, why, why_size) || nandlog_sim_counters().repeated_programs)
   {
      if (nandlog_sim_counters().repeated_programs)
         snprintf(why, why_size, "a page was programmed twice");
      *failure_kind = 3;
      return false;
   }
   return true;
}


// The Enumeration -----------------------------------------------------------------------------------------------------

static int run_scenario(const scenario_t *sc, tally_t *total)
{
   // The base: a part whose driver has written its table, with the retired block marked bad from the factory
   create_part();
   lay_out_positions(sc);
   if (sc->retired_block != ~0u)
      nandlog_sim_raw_page((log_first_block() + sc->retired_block) * ppb())[page_bytes()] = 0x00;
   nandlog_init();
   nandlog_deinit();
   nandlog_sim_snapshot();

   tally_t t;
   memset(&t, 0, sizeof(t));
   state_t states[WINDOW];
   uint32_t combination_count = 1;
   for (uint32_t i = 0; i < sc->window; ++i)
      combination_count *= (i == (sc->window - 1)) ? NUM_STATES : S_u;
   const uint32_t afters = sc->unentered ? NUM_AFTER : 1;
   for (uint32_t after = 0; after < afters; ++after)
      for (uint32_t c = 0; c < combination_count; ++c)
      {
         uint32_t rest = c;
         bool outside = false;
         for (uint32_t i = 0; i < sc->window; ++i)
         {
            const uint32_t radix = (i == (sc->window - 1)) ? NUM_STATES : S_u;
            states[i] = (state_t)(rest % radix);
            rest /= radix;
            outside = outside || (states[i] == S_B);
         }
         char why[160], name[WINDOW + 1];
         uint32_t kind = 0;
         for (uint32_t i = 0; i < sc->window; ++i)
            name[i] = state_names[states[i]];
         name[sc->window] = 0;
         if (only_combination && (strcmp(name, only_combination) || (after != only_after)))
            continue;
         nandlog_sim_set_verbose(only_combination != NULL);
         ++t.combinations;
         t.outside_model += outside ? 1 : 0;
         if (run_combination(sc, states, (after_t)after, why, sizeof(why), &kind))
            continue;
         if (only_combination)
            printf("  %s %s after=%u: %s\n", sc->name, name, after, why);
         if (outside)
         {
            ++t.outside_model_failed;
            continue;
         }
         ++t.failed;
         t.head_wrong += (kind == 0) ? 1 : 0;
         t.invariant_broken += (kind == 1) ? 1 : 0;
         t.contract_broken += (kind == 2) ? 1 : 0;
         t.identity_broken += (kind == 3) ? 1 : 0;
         if (t.failed <= 5)
            printf("  FAIL %s %s after=%u: %s\n", sc->name, name, after, why);
      }
   nandlog_deinit();
   nandlog_sim_destroy();
   printf("  %-9s %-86s %6u combinations, %5u outside the model (%u of those not held); %u failed\n", sc->name, sc->description,
          t.combinations, t.outside_model, t.outside_model_failed, t.failed);
   total->combinations += t.combinations;
   total->outside_model += t.outside_model;
   total->outside_model_failed += t.outside_model_failed;
   total->failed += t.failed;
   total->head_wrong += t.head_wrong;
   total->invariant_broken += t.invariant_broken;
   total->contract_broken += t.contract_broken;
   total->identity_broken += t.identity_broken;
   return t.failed ? 1 : 0;
}


// Metadata Paths ------------------------------------------------------------------------------------------------------
//
// The second part enumerates the metadata write paths of the same toy part, running the real code into them: a
// boot listing a torn page; a boot listing one more torn page onto a slot that already lists a run where the head
// search probes first; a boot listing a torn page at the start of a block, then erasing the block and tombstoning
// it, or, the block refusing that erase, relocating and retiring it; a new epoch; a boot settling a torn slot; a
// failed program (its record, the relocation, the table's new version, the retired block's mark, the ring's record
// of the retirement); a window block that will not erase (the same, without a relocation); and the recovery utility
// followed by logging in the same boot. Each is cut at every operation it does, under every cut model -- the
// operation never begun, half done, each bit done with probability 0.001, 0.05, 0.5 or 0.995, or left undecided so
// that it reads differently every time, eight draws of those -- and then one copy of the newest ring slot or of the
// newest table version is damaged as well, or neither: its header rotted, or its details, list or entries made
// unreadable; or, damage that is none, the copy left whole but read only at the limit of the part's ECC, the first
// copy or the second as the operation cut is even or odd. The device then boots, is audited, lets every weak page
// drift (all at once to blank, or half of each one's bits), boots, logs three pages, lets every weak page drift
// again -- the pages just logged are weak if they went into a block whose last erase a cut stopped -- boots, and is
// audited again.
//
// A cut together with a damaged copy is two faults, and the log may answer them by refusing to write -- which is
// counted; after a cut alone, refusing to write is a failure -- but never by losing a durable record, delivering
// one damaged or twice, serving other content under a sequence number a read has served, programming a page that
// drifted or one it programmed before, retiring a good block, or forgetting a factory-bad one. A copy read at the
// ECC limit is no fault: the trial is held to everything a cut alone is, and the first boot must also have written
// the record again, so that the newest slot or version is no longer the pair that copy is in.

#define META_MAX_IDS                                512

typedef struct
{
   const char *name, *description;
   void (*prepare)(void);
   void (*body)(void);
} meta_trace_t;

static uint32_t meta_next_id, meta_pending[64], meta_pending_count, meta_faulty[4], meta_faulty_count, meta_epoch;
static nandlog_status_t meta_last_status;
static uint8_t meta_durable[META_MAX_IDS], meta_seen[META_MAX_IDS], meta_epoch_of[META_MAX_IDS];
static uint32_t base_next_id;
static uint8_t base_durable[META_MAX_IDS];
static jmp_buf meta_cut;

static uint32_t meta_pages_committed(void)
{
   nandlog_stats_t stats;
   nandlog_get_stats(&stats);
   return stats.pages_committed;
}

static void meta_store(void)
{
   // One page-filling record, which commits the page before it; the model learns what became durable
   const uint32_t id = meta_next_id++, before = meta_pages_committed();
   meta_epoch_of[id] = (uint8_t)meta_epoch;
   fill_record(record, page_record(), id);
   const nandlog_status_t status = nandlog_store_record(7, 2000 + id, record, page_record());
   meta_last_status = status;
   if (meta_pages_committed() != before)
   {
      for (uint32_t i = 0; i < meta_pending_count; ++i)
         meta_durable[meta_pending[i]] = 1;
      meta_pending_count = 0;
   }
   if ((status == NANDLOG_OK) && (meta_pending_count < 64))
      meta_pending[meta_pending_count++] = id;
}

static void meta_flush(void)
{
   const uint32_t before = meta_pages_committed();
   nandlog_flush(true);
   if (meta_pages_committed() != before)
      for (uint32_t i = 0; i < meta_pending_count; ++i)
         meta_durable[meta_pending[i]] = 1;
   meta_pending_count = 0;
}

static void meta_pages(uint32_t count)
{
   for (uint32_t i = 0; i < count; ++i)
      meta_store();
   meta_flush();
}

static void meta_begin_epoch(const char *name)
{
   uint8_t details[16] = { 0 };
   strncpy((char*)details, name, sizeof(details) - 1);
   nandlog_begin_session();
   if (nandlog_begin_epoch(details, sizeof(details)))
      ++meta_epoch;
   nandlog_end_session();
}

static uint32_t meta_current_epoch(void)
{
   nandlog_epoch_info_t info;
   memset(&info, 0, sizeof(info));
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_end_session();
   return info.epoch;
}

static void meta_faults(uint32_t unwritable_page, uint32_t unerasable_block)
{
   nandlog_sim_faults_t faults;
   memset(&faults, 0, sizeof(faults));
   memcpy(faults.device_id, DEVICE_ID, sizeof(DEVICE_ID));
   if (unwritable_page != 0xFFFFFFFF)
   {
      faults.unwritable_pages[faults.num_unwritable_pages++] = unwritable_page;
      meta_faulty[meta_faulty_count++] = unwritable_page / ppb();
   }
   if (unerasable_block != 0xFFFFFFFF)
   {
      faults.unerasable_blocks[faults.num_unerasable_blocks++] = unerasable_block;
      meta_faulty[meta_faulty_count++] = unerasable_block;
   }
   nandlog_sim_set_faults(&faults);
}

static void meta_torn_commit_q(double q)
{
   // A commit whose program a cut tears, each bit done with probability q, and power off
   fill_record(record, 60, meta_next_id);
   nandlog_store_record(7, 2000 + meta_next_id, record, 60);
   ++meta_next_id;
   for (uint32_t i = 0; i < 4; ++i)
      nandlog_flush(false);
   nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_BITWISE, q, &meta_cut);
   if (setjmp(meta_cut) == 0)
      nandlog_flush(true);
   nandlog_sim_power_cut_disarm();
   meta_pending_count = 0;
   nandlog_deinit();
}

static void meta_torn_commit(void) { meta_torn_commit_q(0.5); }

static uint32_t first_log_page(void) { return log_first_block() * ppb(); }

static void prepare_list(void)   { nandlog_init(); meta_begin_epoch("list"); meta_pages(6); meta_torn_commit(); }
static void prepare_rewrite(void)
{
   // A commit torn at the first page of the epoch's second block, its header checking over a payload that does not.
   // The boot that lists it erases the block again and tombstones the position; an erase a cut barely began leaves
   // that header checking still, and the block must not be taken as it stands
   nandlog_init();
   meta_begin_epoch("rewrite");
   meta_pages(4);
   nandlog_deinit();
   nandlog_sim_snapshot();
   const uint32_t next_id = meta_next_id, page = first_log_page() + 4;
   for (uint64_t seed = 1; seed < 64; ++seed)
   {
      nandlog_sim_restore();
      meta_next_id = next_id;
      nandlog_init();
      nandlog_sim_seed(seed);
      meta_torn_commit_q(0.995);
      nandlog_page_header_t header;
      if (raw_header(page, &header) && (header.payload_length <= (page_bytes() - sizeof(header))) &&
          (crc32_bits(nandlog_sim_raw_page(page) + sizeof(header), header.payload_length) != header.payload_crc))
         return;
   }
}
static void body_list(void)      { nandlog_init(); meta_pages(2); }
static void prepare_relist(void)
{
   // Two commits in a row torn and listed -- a run of two passed-over positions where the write-head search probes
   // first, the middle of the region -- then pages, then a third commit torn
   nandlog_init();
   meta_begin_epoch("relist");
   meta_pages(10);
   meta_torn_commit();
   nandlog_init();
   meta_torn_commit();
   nandlog_init();
   meta_pages(3);
   meta_torn_commit();
}
static void prepare_moved(void)  { nandlog_init(); meta_begin_epoch("moved"); meta_pages(4); meta_torn_commit(); }
static void body_moved(void)     { meta_faults(0xFFFFFFFF, log_first_block() + 1); nandlog_init(); meta_pages(2); }
static void prepare_begin(void)  { nandlog_init(); meta_begin_epoch("E1"); meta_pages(5); meta_torn_commit(); nandlog_init(); meta_pages(2); }
static void body_begin(void)     { nandlog_init(); meta_begin_epoch("E2"); meta_pages(2); }
static void prepare_settle(void)
{
   nandlog_init();
   meta_begin_epoch("E1");
   meta_pages(4);
   nandlog_sim_seed(3);
   nandlog_sim_power_cut(2, NANDLOG_SIM_CUT_BITWISE, 0.5, &meta_cut);
   if (setjmp(meta_cut) == 0)
      meta_begin_epoch("E2");
   nandlog_sim_power_cut_disarm();
   nandlog_deinit();
}
static void body_settle(void)    { nandlog_init(); meta_pages(2); }
static void prepare_failure(void){ nandlog_init(); meta_begin_epoch("fail"); meta_pages(6); }
static void body_failure(void)   { meta_faults(first_log_page() + 6, 0xFFFFFFFF); nandlog_init(); meta_pages(3); }
static void prepare_window(void) { nandlog_init(); meta_begin_epoch("win"); meta_pages(5); }
static void body_window(void)
{
   meta_faults(0xFFFFFFFF, log_first_block() + 3);
   nandlog_init();
   for (uint32_t i = 0; i < 4; ++i)
      nandlog_flush(false);
   meta_pages(6);
}
static void prepare_reset(void)
{
   nandlog_sim_raw_page((log_first_block() + 4) * ppb())[page_bytes()] = 0x00;
   nandlog_sim_raw_page((log_end_block() + 1) * ppb())[page_bytes()] = 0x00;
   nandlog_init();
   meta_begin_epoch("reset");
   meta_pages(3);
}
static void body_reset(void)
{
   nandlog_init();
   nandlog_begin_session();
   nandlog_reset_bad_block_table();
   nandlog_end_session();
   meta_faults(first_log_page() + 3, 0xFFFFFFFF);
   meta_pages(2);
   nandlog_deinit();
   nandlog_init();
   meta_pages(1);
}

#if SIM_DEAD_RESERVE_BLOCKS
static uint32_t newest_table_pair(void)
{
   // The pair below the reserve holding the newest table version that reads whole, or ~0
   uint32_t best = 0, pair = ~0u;
   for (uint32_t page = table_search_first_block() * ppb(); page < (log_end_block() * ppb()); ++page)
   {
      const uint8_t *raw = nandlog_sim_raw_page(page);
      uint32_t crc = 0, version = 0, count = 0;
      memcpy(&crc, raw + 4, sizeof(crc));
      memcpy(&version, raw + 8, sizeof(version));
      memcpy(&count, raw + 12, sizeof(count));
      if ((nandlog_sim_raw_page((page / ppb()) * ppb())[page_bytes()] == 0xFF) && !memcmp(raw, "BBT3", 4) && (count <= 256) &&
          (crc32_bits(raw + 8, 8 + (4 * count)) == crc) && ((pair == ~0u) || (version > best)))
      {
         best = version;
         pair = (page / ppb()) & ~1u;
      }
   }
   return pair;
}

static void prepare_place(void)
{
   // A dead reserve's first boot with every pair the table could go to refusing its erase: the marks stand in for the
   // table, and an epoch is begun and written. The body's boot then moves the table below the reserve, under it
   nandlog_sim_faults_t faults;
   memset(&faults, 0, sizeof(faults));
   memcpy(faults.device_id, DEVICE_ID, sizeof(DEVICE_ID));
   for (uint32_t b = table_search_first_block() | 1u; b < log_end_block(); b += 2)
      faults.unerasable_blocks[faults.num_unerasable_blocks++] = b;
   nandlog_sim_set_faults(&faults);
   nandlog_init();
   memset(&faults, 0, sizeof(faults));
   memcpy(faults.device_id, DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_sim_set_faults(&faults);
   meta_begin_epoch("place");
   meta_pages(3);
}
static void body_place(void)     { nandlog_init(); meta_pages(3); }
static void prepare_rotate(void)
{
   // The table below the reserve, its block one version short of full: the next retirement rotates it to the pair's
   // other block
   nandlog_init();
   meta_begin_epoch("rotate");
   meta_pages(2);
   const uint32_t pair = newest_table_pair();
   if (pair != ~0u)
   {
      nandlog_begin_session();
      nandlog_chip_mark_bad_block((pair - 2) * ppb());
      nandlog_end_session();
      meta_faulty[meta_faulty_count++] = pair - 2;
   }
}
static void body_rotate(void)    { meta_faults(first_log_page() + 2, 0xFFFFFFFF); nandlog_init(); meta_pages(3); }
static void prepare_tablefail(void) { nandlog_init(); meta_begin_epoch("tablefail"); meta_pages(4); }
static void body_tablefail(void)
{
   // The head's block refuses a program, and the table's pair refuses the version that would retire it -- its next
   // page will not program, its other block will not erase -- so the marks take over, and the block's own mark retires it.
   // The program refused is the block's second page: its first, where the mark goes, takes the mark ("refused" below
   // is the block that refuses that too)
   const uint32_t pair = newest_table_pair();
   nandlog_sim_faults_t faults;
   memset(&faults, 0, sizeof(faults));
   memcpy(faults.device_id, DEVICE_ID, sizeof(DEVICE_ID));
   faults.unwritable_pages[faults.num_unwritable_pages++] = first_log_page() + ppb() + 1;
   meta_faulty[meta_faulty_count++] = log_first_block() + 1;
   if (pair != ~0u)
   {
      faults.unwritable_pages[faults.num_unwritable_pages++] = (pair * ppb()) + 2;
      faults.unerasable_blocks[faults.num_unerasable_blocks++] = pair + 1;
   }
   nandlog_sim_set_faults(&faults);
   nandlog_init();
   meta_pages(3);
}
static void body_refused(void)
{
   // The same, but the head's block refuses every program, its mark included, so neither the table nor the marks can
   // retire it: the log gives the table a new pair mid-logging and retires the block through it. Where there is
   // nowhere to move the table, there is no pair to give, and the log stops, saying so, until a reboot
   const uint32_t pair = newest_table_pair();
   nandlog_sim_faults_t faults;
   memset(&faults, 0, sizeof(faults));
   memcpy(faults.device_id, DEVICE_ID, sizeof(DEVICE_ID));
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = log_first_block() + 1;
   meta_faulty[meta_faulty_count++] = log_first_block() + 1;
   if (pair != ~0u)
   {
      faults.unwritable_pages[faults.num_unwritable_pages++] = (pair * ppb()) + 2;
      faults.unerasable_blocks[faults.num_unerasable_blocks++] = pair + 1;
   }
   nandlog_sim_set_faults(&faults);
   nandlog_init();
   meta_pages(3);
}
static void prepare_moveagain(void) { prepare_tablefail(); body_tablefail(); nandlog_deinit(); }
static void body_moveagain(void) { nandlog_init(); meta_pages(2); }
#endif

static const meta_trace_t meta_traces[] =
{
#if SIM_DEAD_RESERVE_BLOCKS
   { "place",   "a dead reserve's table, in the marks, moved below it by a boot under an epoch",   prepare_place,   body_place },
   { "rotate",  "a retirement that rotates the table to the other block of its pair",             prepare_rotate,  body_rotate },
   { "tablefail","the table's pair refusing a retirement: the marks take over, the block's mark",  prepare_tablefail, body_tablefail },
   { "moveagain","the boot after that: the marks, the table moved to a new pair under an epoch",   prepare_moveagain, body_moveagain },
   { "refused", "the block refusing even its mark too: a pair taken mid-logging, else a safe stop", prepare_tablefail, body_refused },
#endif
   { "list",    "a boot listing a torn page, then logging",                                      prepare_list,    body_list },
   { "relist",  "a boot listing a torn page after a run of two listed before, then logging",     prepare_relist,  body_list },
   { "rewrite", "a boot listing a torn block-start page, erasing the block, tombstoning it",       prepare_rewrite, body_list },
   { "moved",   "the same block refusing that erase: the boot relocates it, the ring, the table",  prepare_moved,   body_moved },
   { "begin",   "a new epoch begun after a listed torn page",                                    prepare_begin,   body_begin },
   { "settle",  "a boot settling a slot whose second copy a cut tore",                          prepare_settle,  body_settle },
   { "failure", "a failed program: its record, the relocation, the table, the mark, the ring",  prepare_failure, body_failure },
   { "window",  "a window block that will not erase: the table, the mark, the ring",            prepare_window,  body_window },
   { "reset",   "the recovery utility, logging through a retirement, and a reboot",             prepare_reset,   body_reset },
};
#define NUM_META_TRACES                             (sizeof(meta_traces) / sizeof(meta_traces[0]))

static const nandlog_sim_cut_model_t meta_models[8] = { NANDLOG_SIM_CUT_BOUNDARY, NANDLOG_SIM_CUT_PREFIX, NANDLOG_SIM_CUT_BITWISE, NANDLOG_SIM_CUT_BITWISE,
                                                        NANDLOG_SIM_CUT_BITWISE, NANDLOG_SIM_CUT_BITWISE, NANDLOG_SIM_CUT_UNSTABLE, NANDLOG_SIM_CUT_UNSTABLE };
static const double meta_q[8] = { 0.5, 0.5, 0.001, 0.05, 0.5, 0.995, 0.5, 0.995 };
#define META_DAMAGE_KINDS                           7
static const char *meta_damage_names[META_DAMAGE_KINDS] = { "none", "ring copy rotted", "ring copy's details and list unreadable", "table copy rotted",
                                                            "table copy uncorrectable", "ring copy at the ECC limit", "table copy at the ECC limit" };

static bool damage_is_fault(uint32_t which)
{
   // A copy read at the ECC limit is whole: no second fault beside the cut
   return (which >= 1) && (which <= 4);
}

static uint32_t newest_ring_page(void)
{
   // The ring page holding the slot of the highest sequence number whose head and extension check, judged from the
   // medium -- the second copy of its pair where both do -- or ~0
   uint32_t best = 0, best_page = 0xFFFFFFFF;
   for (uint32_t page = 0; page < (METADATA_RING_BLOCKS * ppb()); ++page)
   {
      const uint8_t *raw = nandlog_sim_raw_page(page);
      nandlog_meta_header_t header;
      nandlog_meta_extension_t extension;
      memcpy(&header, raw, sizeof(header));
      memcpy(&extension, raw + sizeof(header), sizeof(extension));
      if ((header.magic == NANDLOG_META_MAGIC) && (crc32_bits(raw, NANDLOG_META_EXTENSION_CRC_BYTES) == extension.extension_crc) &&
          (extension.sequence >= best))
      {
         best = extension.sequence;
         best_page = page;
      }
   }
   return best_page;
}

static uint32_t newest_table_page(void)
{
   // The same for the bad-block table: the page holding its highest version that checks, in an unmarked block it may
   // be kept in, or ~0
   uint32_t best = 0, best_page = 0xFFFFFFFF;
   const uint32_t table_first = SIM_DEAD_RESERVE_BLOCKS ? table_search_first_block() : log_end_block();
   for (uint32_t page = table_first * ppb(); page < ((log_end_block() + 8) * ppb()); ++page)
   {
      const uint8_t *raw = nandlog_sim_raw_page(page);
      if (nandlog_sim_raw_page((page / ppb()) * ppb())[page_bytes()] != 0xFF)
         continue;
      uint32_t crc = 0, version = 0, count = 0;
      memcpy(&crc, raw + 4, sizeof(crc));
      memcpy(&version, raw + 8, sizeof(version));
      memcpy(&count, raw + 12, sizeof(count));
      if (!memcmp(raw, "BBT3", 4) && (count <= 256) && (crc32_bits(raw + 8, 8 + (4 * count)) == crc) && (version >= best))
      {
         best = version;
         best_page = page;
      }
   }
   return best_page;
}

static uint32_t damage_metadata(uint32_t which, uint32_t operation)
{
   // One copy of the newest ring slot, or of the newest table version, damaged: the copy written first, or second; or
   // read at the ECC limit, the first copy or the second as the operation cut is even or odd. The page, or ~0
   if (!which)
      return 0xFFFFFFFF;
   const bool ring = (which <= 2) || (which == 5);
   const uint32_t best_page = ring ? newest_ring_page() : newest_table_page();
   if (best_page == 0xFFFFFFFF)
      return best_page;
   if (which >= 5)
   {
      const uint32_t page = (best_page & ~1u) | (operation & 1);
      nandlog_sim_set_ecc_status(page, NANDLOG_SIM_ECC_AT_LIMIT);
      return page;
   }
   if (ring)
   {
      const uint32_t page = (which == 1) ? (best_page & ~1u) : (best_page | 1u);
      if (which == 1)
      {
         nandlog_sim_corrupt(page, 4, 0x5A);
         nandlog_sim_corrupt(page, 12, 0xA5);
      }
      else
         nandlog_sim_corrupt(page, sizeof(nandlog_meta_header_t) + sizeof(nandlog_meta_extension_t), 0x81);
      nandlog_sim_set_ecc_status(page, NANDLOG_SIM_ECC_UNCORRECTABLE);
      return page;
   }
   const uint32_t page = (which == 3) ? (best_page & ~1u) : (best_page | 1u);
   if (which == 3)
      nandlog_sim_corrupt(page, 5, 0x3C);
   nandlog_sim_set_ecc_status(page, NANDLOG_SIM_ECC_UNCORRECTABLE);
   return page;
}

typedef struct { uint32_t trial, epoch, crc, bound; bool data; } meta_identity_t;
static meta_identity_t meta_identity[8][256];
static uint32_t meta_trial;

static bool meta_audit(char *why, size_t why_size)
{
   // Every listed epoch read end to end: nothing damaged or twice, and every sequence number serving what it served
   // before in this trial, or a gap
   memset(meta_seen, 0, sizeof(meta_seen));
   nandlog_begin_session();
   const uint32_t epochs = nandlog_epoch_count();
   for (uint32_t e = 0; e < epochs; ++e)
   {
      nandlog_epoch_info_t info;
      if (!nandlog_epoch_info(e, &info) || (e && !nandlog_select_epoch(info.epoch)))
         continue;
      if (!e)
         nandlog_select_current_epoch();
      uint32_t positions = 0;
      nandlog_begin_reading(0, 0);
      nandlog_read_span(&positions, NULL);
      for (uint32_t i = 0; i < positions; ++i)
      {
         nandlog_page_header_t header;
         const uint32_t length = nandlog_retrieve_next_page(readback, &header);
         uint32_t id = 0xFFFFFFFF;
         if (length)
         {
            uint16_t data_length = 0;
            memcpy(&data_length, readback, sizeof(data_length));
            memcpy(&id, readback + prefix_bytes() + 5, sizeof(id));
            fill_record(record, data_length, id);
            if ((id >= META_MAX_IDS) || memcmp(record, readback + prefix_bytes() + 5, data_length) || meta_seen[id]++)
            {
               snprintf(why, why_size, "epoch %u seq %u delivered a damaged or duplicated record", info.epoch, header.seq);
               nandlog_end_reading();
               nandlog_end_session();
               return false;
            }
         }
         if (header.seq < 256)
         {
            meta_identity_t *entry = &meta_identity[info.epoch % 8][header.seq];
            const uint32_t crc = length ? crc32_bits(readback, length) : 0;
            if ((entry->trial != meta_trial) || (entry->epoch != info.epoch))
            {
               entry->trial = meta_trial;
               entry->epoch = info.epoch;
               entry->crc = crc;
               entry->data = (length > 0);
               entry->bound = meta_next_id;
            }
            else if (length && ((entry->data && (entry->crc != crc)) || (!entry->data && (id >= entry->bound))))
            {
               snprintf(why, why_size, "epoch %u seq %u served other content than before", info.epoch, header.seq);
               nandlog_end_reading();
               nandlog_end_session();
               return false;
            }
         }
      }
      nandlog_end_reading();
   }
   nandlog_select_current_epoch();
   nandlog_end_session();
   return true;
}

static bool meta_trial_holds(uint32_t damage, uint32_t operation, bool partial_drift, char *why, size_t why_size, bool *refused)
{
   const uint32_t damaged = damage_metadata(damage, operation);
   nandlog_sim_clear_repeated_programs();
   nandlog_sim_reset_counters();
   nandlog_sim_faults_t none;
   memset(&none, 0, sizeof(none));
   memcpy(none.device_id, DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_sim_set_faults(&none);
   nandlog_init();
   meta_epoch = meta_current_epoch();
   if ((damage >= 5) && (damaged != 0xFFFFFFFF))
   {
      // The copy that read at the limit made its record unsettled, and the boot wrote it again elsewhere
      const uint32_t newest = (damage == 5) ? newest_ring_page() : newest_table_page();
      if ((newest == 0xFFFFFFFF) || ((newest & ~1u) == (damaged & ~1u)))
      {
         snprintf(why, why_size, "the newest %s is still the pair whose copy at page %u read at the ECC limit", (damage == 5) ? "slot" : "table version", damaged);
         return false;
      }
   }
   if (!meta_audit(why, why_size))
      return false;
   nandlog_deinit();
   nandlog_sim_drift(partial_drift ? NANDLOG_SIM_DRIFT_PARTIAL : NANDLOG_SIM_DRIFT_BLANK, 1.0);
   nandlog_init();
   if (!meta_audit(why, why_size))
      return false;
   meta_pending_count = 0;
   const uint32_t before = meta_pages_committed();
   meta_pages(3);
   *refused = (meta_pages_committed() == before) && (meta_last_status != NANDLOG_ERR_FULL);   // a full toy region is no refusal
   nandlog_deinit();
   nandlog_sim_drift(partial_drift ? NANDLOG_SIM_DRIFT_PARTIAL : NANDLOG_SIM_DRIFT_BLANK, 1.0);
   nandlog_init();
   if (!meta_audit(why, why_size))
      return false;
   // A record of an epoch older than the current one may have been swept by its write head, as the toy part's
   // five log blocks soon are
   for (uint32_t id = 0; id < meta_next_id; ++id)
      if (meta_durable[id] && !meta_seen[id] && (meta_epoch_of[id] >= meta_epoch))
      {
         snprintf(why, why_size, "durable record %u lost", id);
         return false;
      }
   // The log of second programs holds the drifted ones too. The driver's mark into a block it has retired is the
   // one second program that is deliberate, wherever that block's first page has drifted to
   uint32_t pages[64], repeated = 0, last = 0;
   const uint32_t total = nandlog_sim_repeated_program_pages(pages, 64);
   for (uint32_t i = 0; (i < total) && (i < 64); ++i)
      if (!nandlog_chip_is_bad_block(pages[i]))
      {
         ++repeated;
         last = pages[i];
      }
   if (repeated)
   {
      snprintf(why, why_size, "%u pages programmed again, drifted or not (the last page %u)", repeated, last);
      return false;
   }
   for (uint32_t block = 0; block < nandlog_chip_geometry()->block_count; ++block)
   {
      const bool factory = (block == (log_first_block() + 4)) || (block == (log_end_block() + 1));
      bool faulty = false;
      for (uint32_t i = 0; i < meta_faulty_count; ++i)
         faulty = faulty || (meta_faulty[i] == block);
      const bool bad = nandlog_chip_is_bad_block(block * ppb());
      if (factory && !bad && (nandlog_sim_raw_page(block * ppb())[page_bytes()] != 0xFF))
      {
         snprintf(why, why_size, "factory-bad block %u forgotten", block);
         return false;
      }
      if (bad && !factory && !faulty && (block < log_end_block()) && !nandlog_sim_block_dead(block) && !holds_table(block))
      {
         snprintf(why, why_size, "good block %u retired", block);
         return false;
      }
   }
   return true;
}

typedef struct { uint32_t trials, failed, refused; } meta_tally_t;
static int only_op = -1, only_model = -1, only_damage = -1;

static int run_meta_trace(const meta_trace_t *trace, meta_tally_t *total)
{
   nandlog_deinit();
   create_part();
   meta_next_id = meta_pending_count = meta_faulty_count = meta_epoch = 0;
   memset(meta_durable, 0, sizeof(meta_durable));
   if (trace->prepare)
      trace->prepare();
   nandlog_deinit();
   const uint32_t base_epoch = meta_epoch;
   base_next_id = meta_next_id;
   memcpy(base_durable, meta_durable, sizeof(meta_durable));
   const uint32_t base_faulty = meta_faulty_count;
   nandlog_sim_snapshot();

   // The uncut body, to count its operations
   nandlog_sim_reset_operations();
   trace->body();
   const uint32_t operations = nandlog_sim_operations();
   meta_tally_t t;
   memset(&t, 0, sizeof(t));
   for (uint32_t n = 1; n <= operations; ++n)
      for (uint32_t m = 0; m < (sizeof(meta_models) / sizeof(meta_models[0])); ++m)
         for (uint32_t damage = 0; damage < META_DAMAGE_KINDS; ++damage)
            for (uint32_t draw = 0; draw < ((meta_models[m] == NANDLOG_SIM_CUT_UNSTABLE) ? 8 : 1); ++draw)
         {
            if (((only_op >= 0) && (n != (uint32_t)only_op)) || ((only_model >= 0) && (m != (uint32_t)only_model)) ||
                ((only_damage >= 0) && (damage != (uint32_t)only_damage)))
               continue;
            nandlog_sim_set_verbose(only_op >= 0);
            nandlog_deinit();
            nandlog_sim_restore();
            meta_next_id = base_next_id;
            meta_epoch = base_epoch;
            meta_pending_count = 0;
            meta_faulty_count = base_faulty;
            memcpy(meta_durable, base_durable, sizeof(meta_durable));
            ++meta_trial;
            nandlog_sim_seed(0xBEEF + (n * 7) + m + (damage * 1000) + (draw * 100000));
            nandlog_sim_reset_operations();
            nandlog_sim_power_cut(n, meta_models[m], meta_q[m], &meta_cut);
            if (setjmp(meta_cut) == 0)
               trace->body();
            nandlog_sim_power_cut_disarm();
            if (!nandlog_sim_power_failed())
               continue;
            nandlog_deinit();
            char why[160];
            bool refused = false;
            ++t.trials;
            if (meta_trial_holds(damage, n, (n + m) & 1, why, sizeof(why), &refused) && (damage_is_fault(damage) || !refused))
            {
               t.refused += refused ? 1 : 0;
               continue;
            }
            if (!damage_is_fault(damage) && refused)
               snprintf(why, sizeof(why), "logging refused after one cut and no other fault");
            if (++t.failed <= 5)
               printf("  FAIL %s op %u model %u (q %.3f) damage '%s': %s\n", trace->name, n, m, meta_q[m], meta_damage_names[damage], why);
         }
   nandlog_deinit();
   nandlog_sim_destroy();
   printf("  %-8s %-78s %3u ops %5u trials, %4u refused to log after, %u failed\n", trace->name, trace->description, operations, t.trials, t.refused, t.failed);
   total->trials += t.trials;
   total->failed += t.failed;
   total->refused += t.refused;
   return t.failed ? 1 : 0;
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   only_combination = (argc > 3) ? argv[2] : NULL;
   only_after = (argc > 3) ? (uint32_t)atoi(argv[3]) : 0;
   printf("bounded-exhaustive write-head recovery: %u pages a block, log blocks %u..%u, window of %u positions below the head\n",
          nandlog_chip_geometry()->pages_per_block, METADATA_RING_BLOCKS, nandlog_chip_geometry()->block_count - nandlog_chip_geometry()->reserved_blocks - 1, WINDOW);
   printf("states: V committed, R rotted, L torn and listed, H torn with a good header and listed, E listed and drifted blank,\n"
          "        S listed and tombstoned, B committed and blank (outside the model); directly below the head also u and h,\n"
          "        a torn page no boot has listed yet\n");
   tally_t total;
   memset(&total, 0, sizeof(total));
   int status = 0;
   const bool metadata_only = (argc > 1) && !strcmp(argv[1], "metadata");
   for (uint32_t i = 0; (i < NUM_SCENARIOS) && !metadata_only; ++i)
      if ((argc < 2) || !strcmp(argv[1], scenarios[i].name))
         status |= run_scenario(&scenarios[i], &total);
   printf("\n%u combinations booted (%u within the fault model, %u outside it); %u within the model failed: head %u, invariant %u, "
          "contract %u, identity %u; %u outside the model not held\n", total.combinations, total.combinations - total.outside_model,
          total.outside_model, total.failed, total.head_wrong, total.invariant_broken, total.contract_broken, total.identity_broken,
          total.outside_model_failed);
   printf("in %u combinations the boot passed over a block an interrupted erase had left, rather than program it\n", passed_over_heads);

   if (argc > 4)
   {
      only_op = atoi(argv[2]);
      only_model = atoi(argv[3]);
      only_damage = atoi(argv[4]);
      only_combination = NULL;
   }
   printf("\nmetadata write paths: every operation cut under 8 models (boundary, prefix, bitwise 0.001/0.05/0.5/0.995,\n"
          "unstable 0.5/0.995), with no further damage, or one copy of the newest ring slot or table version damaged or read\n"
          "at the ECC limit, then drift\n");
   meta_tally_t meta;
   memset(&meta, 0, sizeof(meta));
   for (uint32_t i = 0; i < NUM_META_TRACES; ++i)
      if ((argc < 2) || metadata_only || !strcmp(argv[1], meta_traces[i].name))
         status |= run_meta_trace(&meta_traces[i], &meta);
   printf("%u metadata trials, %u failed; %u refused to log afterwards (a cut and a damaged copy are two faults)\n", meta.trials, meta.failed, meta.refused);
   printf("%s\n", status ? "EXHAUSTIVE CHECK FAILED" : "exhaustive check: every combination within the fault model kept the invariant and the contract");
   return status;
}
