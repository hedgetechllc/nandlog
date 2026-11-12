// Alliance Memory AS5F18G04SND SPI NAND driver

// Chip Declaration ----------------------------------------------------------------------------------------------------

#define NANDLOG_CHIP_NAME                           "AS5F18G04SND"
#define NANDLOG_CHIP_PAGE_SIZE_BYTES                4096
#define NANDLOG_CHIP_SPARE_SIZE_BYTES               256

// Overridable only so a host test can reach a full log region quickly with a smaller geometry
#ifndef NANDLOG_CHIP_PAGES_PER_BLOCK
#define NANDLOG_CHIP_PAGES_PER_BLOCK                64
#endif

// Blocks at the top of the array kept out of the log region. The bad-block table uses the first BBT_CANDIDATE_BLOCKS
// of them and the on-device tests use the top ones as scratch. Bad blocks are skipped where they lie, not remapped
#ifndef NANDLOG_CHIP_RESERVED_BLOCKS
#define NANDLOG_CHIP_RESERVED_BLOCKS                16
#endif

#ifndef NANDLOG_CHIP_BLOCK_COUNT
#define NANDLOG_CHIP_BLOCK_COUNT                    4096
#endif

#include "nandlog_chip_common.h"


// Command Set ---------------------------------------------------------------------------------------------------------

#define COMMAND_READ_DEVICE_ID                      0x9F
#define COMMAND_DEVICE_RESET                        0xFF
#define COMMAND_READ_STATUS_REGISTER                0x0F
#define COMMAND_WRITE_STATUS_REGISTER               0x1F
#define COMMAND_WRITE_ENABLE                        0x06
#define COMMAND_WRITE_DISABLE                       0x04
#define COMMAND_BLOCK_ERASE                         0xD8
#define COMMAND_PROGRAM_DATA_LOAD                   0x02
#define COMMAND_PROGRAM_EXECUTE                     0x10
#define COMMAND_PAGE_DATA_READ                      0x13
#define COMMAND_READ                                0x03


// Status Registers ----------------------------------------------------------------------------------------------------

#define STATUS_REGISTER_1                           0xA0
#define STATUS_REGISTER_2                           0xB0
#define STATUS_REGISTER_3                           0xC0

// ECCS, bits 5:4 of status register 3: 00b no error, 01b corrected, 10b uncorrectable, and 11b corrected at the
// ECC limit
#define STATUS_ECC_MASK                             0b00110000
#define STATUS_ECC_UNCORRECTABLE                    0b00100000
#define STATUS_ECC_AT_LIMIT                         0b00110000
#define STATUS_WRITE_FAILURE                        0b00001000
#define STATUS_ERASE_FAILURE                        0b00000100
#define STATUS_BUSY                                 0b00000001

// What a timed-out wait returns if nandlog_port_fatal() does: every failure bit, so no caller reads it as success
#define STATUS_TIMED_OUT                            (STATUS_ECC_UNCORRECTABLE | STATUS_WRITE_FAILURE | STATUS_ERASE_FAILURE)

// Block-protect patterns for status register 1: everything locked, or the whole array writable
#define PROTECT_ALL                                 0b01111110
#define PROTECT_NONE                                0b00000010

// Status register 2 in normal operation, with ECC off for raw reads, and with the OTP pages mapped over the main array
#define CONFIG_NORMAL                               0b00010000
#define CONFIG_RAW                                  0b00000000
#define CONFIG_OTP                                  0b01010000

// Page of the OTP region carrying the marker that says a bad-block table has been written to this part before
#define OTP_TABLE_EXISTS_PAGE                       0x04


// Bad-Block Table -----------------------------------------------------------------------------------------------------
//
// The table lives in the first BBT_CANDIDATE_BLOCKS blocks of the reserve and is written like a log. Each update
// programs the whole table, with a version one higher and a CRC, to both pages of the next unused pair in the current
// block. When the block is full or a page will not program, the next candidate is erased and the update goes to its
// first pair, so a new version is always verified before the old one is erased. The digit in the magic is part of a
// tag, not a version number
//
// A boot that finds the newest version torn, read only at the ECC limit, or followed by later writes, writes it again
// into a fresh block before the log uses it, so every later boot loads the same table
//
// Every retired block also gets 00h in the first byte of its first page's spare area, where the factory puts its
// marks. The table is rebuilt from these marks and the factory's if every copy is lost or the table is reset
//
// If the reserve cannot hold the table, the log gives the driver a pair of log-region blocks to keep it in. With no
// pair to be had, the driver enters degraded mode, where the marks alone record retired blocks and every boot rebuilds
// the table from them

#define BBM_TABLE_SIZE                              256
#define BBM_DATASHEET_BAD_BLOCKS                    80
#define BBT_CANDIDATE_BLOCKS                        8
#define BBT_RELOCATED_BLOCKS                        2
#define BBT_MAGIC                                   "BBT3"
#define BBT_MAGIC_LENGTH                            4
#define NO_TABLE_BLOCK                              0xFFFFFFFF
#define NO_CANDIDATE                                BBT_CANDIDATE_BLOCKS
_Static_assert(NANDLOG_CHIP_RESERVED_BLOCKS >= BBT_CANDIDATE_BLOCKS, "the bad-block table's blocks are the first of the reserve");

// How many blocks down from the top of the array a boot searches for a table kept below the reserve. The search
// costs one or two page reads per block and runs only when the reserve holds no table
#ifndef NANDLOG_CHIP_TABLE_SEARCH_BLOCKS
#define NANDLOG_CHIP_TABLE_SEARCH_BLOCKS            (NANDLOG_CHIP_RESERVED_BLOCKS + 48)
#endif
_Static_assert(NANDLOG_CHIP_TABLE_SEARCH_BLOCKS > NANDLOG_CHIP_RESERVED_BLOCKS, "the bad-block table's search reaches below the reserve");
#define TABLE_SEARCH_FIRST_PAGE                     ((NANDLOG_CHIP_TABLE_SEARCH_BLOCKS < NANDLOG_CHIP_BLOCK_COUNT) ? \
                                                     ((uint32_t)(NANDLOG_CHIP_BLOCK_COUNT - NANDLOG_CHIP_TABLE_SEARCH_BLOCKS) * NANDLOG_CHIP_PAGES_PER_BLOCK) : 0)
#define TABLE_PAIR_MASK                             (~(uint32_t)((BBT_RELOCATED_BLOCKS * NANDLOG_CHIP_PAGES_PER_BLOCK) - 1))

typedef struct __attribute__ ((__packed__))
{
   uint8_t magic[BBT_MAGIC_LENGTH];  // BBT_MAGIC
   uint32_t crc;                     // CRC-32 over everything after this field: version, count, and the entries
   uint32_t version;                 // one more than the version this replaces
   uint32_t count;                   // entries that follow, each the first page of a retired block
} table_header_t;


// Static Global Variables ---------------------------------------------------------------------------------------------

static uint32_t scratch_words[(NANDLOG_CHIP_PAGE_WITH_SPARE_SIZE_BYTES + 3) / 4];   // word-aligned, for the blank check
static uint8_t *const scratch = (uint8_t*)scratch_words;
static uint32_t bad_block_lookup_table[BBM_TABLE_SIZE], bbm_index;      // entries [0, bbm_index) are on the part

// The run of good blocks around the last good block looked up, as pages [clear_first_page, clear_end_page). A boot
// checks every block in order, so this spares most of them a pass over the table
static uint32_t clear_first_page = 1, clear_end_page;

// The blocks the table rotates over, by first page: the reserve's candidates, or a pair below the reserve.
// table_candidate indexes the block being written and table_home the one holding the newest version, or NO_CANDIDATE
// when there is none. table_abandoned means degraded mode, where the marks stand in for the table
static uint32_t table_blocks[BBT_CANDIDATE_BLOCKS], table_block_count;
static uint32_t table_version, table_candidate, table_next_page, table_home, table_loaded_pair;
static bool table_relocated, table_pair_failed, table_abandoned, table_write_failed;

// The identity this part reports, and the marker written to its OTP page once a table exists. The marker is in OTP,
// so its spelling must never change
static const uint8_t device_id[1] = { 0x8D };
static const uint8_t table_exists_marker[BBT_MAGIC_LENGTH] = { 'B', 'B', 'T', '2' };


// Private Helper Functions --------------------------------------------------------------------------------------------

static void split_page_address(uint32_t page, uint8_t *out)
{
   // Page addresses go out most significant byte first
   out[0] = (uint8_t)((page & 0x00FF0000) >> 16);
   out[1] = (uint8_t)((page & 0x0000FF00) >> 8);
   out[2] = (uint8_t)(page & 0x000000FF);
}

static uint32_t table_crc(const uint8_t *bytes, uint32_t length)
{
   // The IEEE CRC-32 the log uses, computed bit by bit because the table is checked rarely
   uint32_t crc = 0xFFFFFFFF;
   for (uint32_t i = 0; i < length; ++i)
   {
      crc ^= bytes[i];
      for (uint32_t bit = 0; bit < 8; ++bit)
         crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
   }
   return ~crc;
}

static uint8_t read_register(uint8_t register_number)
{
   static uint8_t register_value;
   nandlog_port_transfer_read(COMMAND_READ_STATUS_REGISTER, &register_number, 1, &register_value, 1);
   return register_value;
}

static void write_register(uint8_t register_number, uint8_t value)
{
   nandlog_port_transfer_write(COMMAND_WRITE_STATUS_REGISTER, &register_number, 1, &value, 1);
}

static uint8_t wait_until_not_busy(void)
{
   // Returns the status word that ended the wait
   for (uint32_t polls_remaining = NANDLOG_BUSY_TIMEOUT_POLLS; polls_remaining; --polls_remaining)
   {
      const uint8_t status = read_register(STATUS_REGISTER_3);
      if ((status & STATUS_BUSY) != STATUS_BUSY)
         return status;
      nandlog_port_delay_us(NANDLOG_BUSY_POLL_INTERVAL_US);
   }

   // Reset immediately rather than flushing first. Should the port return, the status below reads as a failure
   NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Storage flash never cleared BUSY after %u polls (>= %u ms); resetting\n", (uint32_t)NANDLOG_BUSY_TIMEOUT_POLLS,
               (uint32_t)(((uint64_t)NANDLOG_BUSY_TIMEOUT_POLLS * NANDLOG_BUSY_POLL_INTERVAL_US) / 1000));
   nandlog_port_fatal("SPI transfer failed after all retries");
   return STATUS_TIMED_OUT;
}

static void write_protect(bool protect)
{
   // Order matters in both directions
   if (protect)
   {
      write_register(STATUS_REGISTER_1, PROTECT_ALL);
      nandlog_port_write_enable(false);
   }
   else
   {
      nandlog_port_write_enable(true);
      write_register(STATUS_REGISTER_1, PROTECT_NONE);
   }
}

static nandlog_read_result_t read_page_region(uint8_t *buffer, uint32_t page, uint32_t column, uint32_t length)
{
   // Latch the page into the cache register, then clock out 'length' bytes starting at 'column'. The READ
   // command carries a 16-bit column address, most significant byte first, followed by a dummy byte
   const uint8_t column_address[3] = { (uint8_t)(column >> 8), (uint8_t)column, 0x00 };
   uint8_t page_number_reordered[3];
   split_page_address(page, page_number_reordered);
   wait_until_not_busy();
   nandlog_port_transfer_write(COMMAND_PAGE_DATA_READ, NULL, 0, page_number_reordered, sizeof(page_number_reordered));
   const uint8_t status = wait_until_not_busy();
   nandlog_port_transfer_read(COMMAND_READ, column_address, sizeof(column_address), buffer, length);
   switch (status & STATUS_ECC_MASK)
   {
      case STATUS_ECC_UNCORRECTABLE:
         return NANDLOG_READ_UNCORRECTABLE;
      case STATUS_ECC_AT_LIMIT:
         return NANDLOG_READ_AT_ECC_LIMIT;
      default:
         return NANDLOG_READ_OK;
   }
}

static bool program_from_column_zero(const uint8_t *data, uint32_t length, uint32_t page)
{
   // Assumes write protection has already been opened by the caller. Every load starts at column 0 because this part
   // resets the cache register to FFh only from the load's column on, and PROGRAM EXECUTE programs all of it. A failed
   // program is not retried, since the page may already hold part of it
   static const uint8_t column_zero[2] = { 0x00, 0x00 };
   uint8_t page_number_reordered[3];
   split_page_address(page, page_number_reordered);
   wait_until_not_busy();
   nandlog_port_transfer_write(COMMAND_WRITE_ENABLE, NULL, 0, NULL, 0);
   nandlog_port_transfer_write(COMMAND_PROGRAM_DATA_LOAD, column_zero, sizeof(column_zero), data, length);
   wait_until_not_busy();
   nandlog_port_transfer_write(COMMAND_PROGRAM_EXECUTE, NULL, 0, page_number_reordered, sizeof(page_number_reordered));
   return (wait_until_not_busy() & STATUS_WRITE_FAILURE) != STATUS_WRITE_FAILURE;
}

static bool write_page_raw(const uint8_t *data, uint32_t page)
{
   return program_from_column_zero(data, NANDLOG_CHIP_PAGE_SIZE_BYTES, page);
}

static bool erase_block_raw(uint32_t page)
{
   // Assumes write protection has already been opened by the caller. An erase, unlike a program, can be
   // repeated harmlessly, so a block gets a few chances before it is called bad
   uint8_t page_number_reordered[3];
   split_page_address(page & NANDLOG_CHIP_BLOCK_MASK, page_number_reordered);
   for (uint32_t attempt = 0; attempt < NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL; ++attempt)
   {
      wait_until_not_busy();
      nandlog_port_transfer_write(COMMAND_WRITE_ENABLE, NULL, 0, NULL, 0);
      nandlog_port_transfer_write(COMMAND_BLOCK_ERASE, NULL, 0, page_number_reordered, sizeof(page_number_reordered));
      if ((wait_until_not_busy() & STATUS_ERASE_FAILURE) != STATUS_ERASE_FAILURE)
         return true;
   }
   return false;
}

static bool page_is_blank_raw(uint32_t page)
{
   // Read with ECC off, since with it on the part can correct away the few stray programmed bits this looks for
   write_register(STATUS_REGISTER_2, CONFIG_RAW);
   read_page_region(scratch, page, 0, NANDLOG_CHIP_PAGE_WITH_SPARE_SIZE_BYTES);
   write_register(STATUS_REGISTER_2, CONFIG_NORMAL);
   return nandlog_bytes_erased(scratch, NANDLOG_CHIP_PAGE_WITH_SPARE_SIZE_BYTES);
}

static bool verify_device_id(void)
{
   static const uint8_t address = 0x01;
   uint8_t device_id_read[sizeof(device_id)];
   nandlog_port_transfer_read(COMMAND_READ_DEVICE_ID, &address, sizeof(address), device_id_read, sizeof(device_id_read));
   return (memcmp(device_id_read, device_id, sizeof(device_id)) == 0);
}

static bool table_contains(uint32_t page, uint32_t count)
{
   page &= NANDLOG_CHIP_BLOCK_MASK;
   for (uint32_t i = 0; i < count; ++i)
      if (bad_block_lookup_table[i] == page)
         return true;
   return false;
}

static void set_table_count(uint32_t count)
{
   // Entries [0, count) are now the table on the part
   bbm_index = count;
   clear_first_page = 1;
   clear_end_page = 0;
}

static bool stage_bad_block(uint32_t page, uint32_t *count)
{
   // Add a block to the table being assembled, past the entries already on the part. Nothing reads it as bad
   // until a table holding it has been persisted
   page &= NANDLOG_CHIP_BLOCK_MASK;
   if (table_contains(page, *count))
      return true;
   if (*count >= BBM_TABLE_SIZE)
   {
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Bad-block table is full at %u entries; block %u not retired\n", (uint32_t)BBM_TABLE_SIZE, (uint32_t)(page / NANDLOG_CHIP_PAGES_PER_BLOCK));
      return false;
   }
   bad_block_lookup_table[(*count)++] = page;
   return true;
}

static inline uint32_t candidate_page(uint32_t candidate)
{
   return table_blocks[candidate];
}

static void keep_table_in_reserve(void)
{
   // The table rotates over the reserve's first blocks, and the next version enters a fresh one
   for (uint32_t candidate = 0; candidate < BBT_CANDIDATE_BLOCKS; ++candidate)
      table_blocks[candidate] = NANDLOG_CHIP_RESERVED_BASE_PAGE + (candidate * NANDLOG_CHIP_PAGES_PER_BLOCK);
   table_block_count = BBT_CANDIDATE_BLOCKS;
   table_relocated = table_pair_failed = false;
   table_candidate = BBT_CANDIDATE_BLOCKS - 1;
   table_home = NO_CANDIDATE;
   table_next_page = NANDLOG_CHIP_PAGES_PER_BLOCK;
}

static void keep_table_below(uint32_t page)
{
   // The table rotates over the aligned pair of log-region blocks holding 'page'
   table_blocks[0] = page & TABLE_PAIR_MASK;
   table_blocks[1] = table_blocks[0] + NANDLOG_CHIP_PAGES_PER_BLOCK;
   table_block_count = BBT_RELOCATED_BLOCKS;
   table_relocated = true;
   table_pair_failed = false;
}

static uint32_t candidate_of(uint32_t page)
{
   for (uint32_t candidate = 0; candidate < table_block_count; ++candidate)
      if (table_blocks[candidate] == (page & NANDLOG_CHIP_BLOCK_MASK))
         return candidate;
   return NO_CANDIDATE;
}

static inline uint32_t home_page(void)
{
   return (table_home < table_block_count) ? candidate_page(table_home) : NO_TABLE_BLOCK;
}

static inline bool table_kept_in(uint32_t page)
{
   // Whether a block is one of the pair below the reserve that holds the table, the only retired blocks left unmarked
   return table_relocated && (candidate_of(page) < NO_CANDIDATE);
}

static bool table_page_valid(uint32_t page, table_header_t *header, bool *foreign, bool *at_limit)
{
   // Whether a page holds a whole table version, judged from one read because a torn page can read differently each
   // time. Leaves the CRC-covered bytes at the start of 'scratch'. 'foreign' is set for a programmed page that reads
   // cleanly without the magic, and 'at_limit' for a read corrected only at the ECC limit
   const uint32_t whole = sizeof(table_header_t) + (BBM_TABLE_SIZE * sizeof(uint32_t));
   const nandlog_read_result_t read = read_page_region(scratch, page, 0, whole);
   memcpy(header, scratch, sizeof(*header));
   if (at_limit)
      *at_limit = (read == NANDLOG_READ_AT_ECC_LIMIT);
   if (foreign)
   {
      *foreign = !nandlog_bytes_erased(scratch, sizeof(*header)) && (read != NANDLOG_READ_UNCORRECTABLE) && memcmp(header->magic, BBT_MAGIC, BBT_MAGIC_LENGTH);
   }
   if ((read == NANDLOG_READ_UNCORRECTABLE) || memcmp(header->magic, BBT_MAGIC, BBT_MAGIC_LENGTH) || (header->count > BBM_TABLE_SIZE))
      return false;
   const uint32_t covered = (2 * sizeof(uint32_t)) + (header->count * sizeof(uint32_t));
   memmove(scratch, scratch + BBT_MAGIC_LENGTH + sizeof(uint32_t), covered);
   return table_crc(scratch, covered) == header->crc;
}

static bool table_page_written(uint32_t page)
{
   // Whether anything is programmed in the page's header, a whole version or a torn one
   table_header_t header;
   read_page_region((uint8_t*)&header, page, 0, sizeof(header));
   return !nandlog_bytes_erased(&header, sizeof(header));
}

static bool header_written(const table_header_t *header)
{
   return !nandlog_bytes_erased(header, sizeof(*header));
}

static bool table_block_entry(uint32_t block_page, table_header_t *header, uint32_t *pair, bool both_copies)
{
   // Find the first version in a table block that reads, normally its first pair, which gives the version the block
   // was entered at. A first pair that reads as something else in both copies is not a table block. An erased first
   // copy ends the search unless 'both_copies', used below the reserve, where a cut while marking the block can leave
   // its first page reading erased while the second copy still holds the version
   for (uint32_t offset = 0; offset < NANDLOG_CHIP_PAGES_PER_BLOCK; offset += 2)
   {
      bool foreign = false, foreign_too = false;
      *pair = offset / 2;
      if (table_page_valid(block_page + offset, header, &foreign, NULL))
         return true;
      const bool written = header_written(header);
      if (!written && !both_copies)
         break;
      if (table_page_valid(block_page + offset + 1, header, &foreign_too, NULL))
         return true;
      if ((!offset && foreign && foreign_too) || (!written && !header_written(header)))
         break;
   }
   return false;
}

static uint32_t table_written_end(uint32_t block_page, uint32_t entry_pair)
{
   // One past the last written page of a table block, found by binary search since the block is written in order.
   // The search starts above the entry pair's first copy, which counts as written whatever it now reads
   uint32_t low = (2 * entry_pair) + 1, high = NANDLOG_CHIP_PAGES_PER_BLOCK;
   while (low < high)
   {
      const uint32_t mid = low + ((high - low) / 2);
      if (table_page_written(block_page + mid))
         low = mid + 1;
      else
         high = mid;
   }
   return low;
}

static bool may_hold_table(uint32_t block_page)
{
   // Whether a block below the reserve may hold the table: the first page's header carries the magic, or reads
   // unreadable or erased and the second page's does
   table_header_t header;
   for (uint32_t offset = 0; offset < 2; ++offset)
   {
      const nandlog_read_result_t read = read_page_region((uint8_t*)&header, block_page + offset, 0, sizeof(header));
      if (!memcmp(header.magic, BBT_MAGIC, BBT_MAGIC_LENGTH))
         return true;
      if ((read != NANDLOG_READ_UNCORRECTABLE) && header_written(&header))
         return false;
   }
   return false;
}

static uint32_t next_table_candidate(uint32_t candidate, uint32_t count)
{
   // The block the table moves to after 'candidate', skipping the newest version's block and, in the reserve, any
   // block the table retires. A pair below the reserve with a block that failed to erase has nowhere to go
   for (uint32_t step = 1; !table_pair_failed && (step < table_block_count); ++step)
   {
      const uint32_t next = (candidate + step) % table_block_count, page = candidate_page(next);
      if ((table_version && (next == table_home)) || (!table_relocated && table_contains(page, count)))
         continue;
      return next;
   }
   return NO_CANDIDATE;
}

static bool persist_table(uint32_t count);
static bool abandon_table(void);
static void mark_spare_area(uint32_t page);

static uint8_t read_marker(uint32_t page)
{
   // The mark at the start of a block's first spare area, read with ECC off so an uncorrectable page is not taken for
   // a marked one
   uint8_t marker = 0xFF;
   write_register(STATUS_REGISTER_2, CONFIG_RAW);
   read_page_region(&marker, page & NANDLOG_CHIP_BLOCK_MASK, NANDLOG_CHIP_PAGE_SIZE_BYTES, sizeof(marker));
   write_register(STATUS_REGISTER_2, CONFIG_NORMAL);
   return marker;
}

static bool block_marked(uint32_t page)
{
   // Whether a block may carry a bad-block mark: any byte but FFh, since a cut can leave a mark part-programmed. A
   // block that may be marked is never erased or taken for a table block
   return read_marker(page) != 0xFF;
}

static bool mark_holds(uint32_t page)
{
   // Assumes write protection has already been opened by the caller. Programs a block's mark unless it already reads
   // exactly 00h, and returns whether it then does. A part-programmed mark is programmed again to complete it
   if (read_marker(page) == 0x00)
      return true;
   mark_spare_area(page);
   return read_marker(page) == 0x00;
}

static bool mark_again(uint32_t page)
{
   // Assumes write protection has already been opened by the caller. Programs a block's mark again even if it reads
   // 00h, and returns whether it then does. A part-programmed mark can read 00h today and FFh tomorrow
   mark_spare_area(page);
   return read_marker(page) == 0x00;
}

static void mark_new_entries(uint32_t from)
{
   // Assumes write protection has already been opened by the caller. Marks every entry from 'from' on whose mark does
   // not read exactly 00h, so a rebuild from the marks finds it. The pair holding the table below the reserve is left
   // unmarked, since a mark there would hide its versions
   for (uint32_t i = from; i < bbm_index; ++i)
      if (!table_kept_in(bad_block_lookup_table[i]) && (read_marker(bad_block_lookup_table[i]) != 0x00))
         mark_spare_area(bad_block_lookup_table[i]);
}

static bool load_table(void)
{
   // Load the newest version that reads. Table blocks are tried newest first, by the version each was entered at, and
   // within a block the newest version is the last written pair either copy of which reads. Marked blocks are passed
   // over, and blocks below the reserve are searched only when no reserve block holds a version
   table_header_t header;
   uint32_t bound = 0, newest = NO_CANDIDATE, newest_end = 0, torn = 0, pair = 0, copies = 0, home = NO_CANDIDATE, found = 0;
   uint32_t found_pages[BBT_CANDIDATE_BLOCKS], entry_versions[BBT_CANDIDATE_BLOCKS], entry_pairs[BBT_CANDIDATE_BLOCKS];
   uint32_t stray = NO_TABLE_BLOCK, stray_version = 0, stray_pair = 0;
   bool loaded = false, at_limit = false;
   for (uint32_t candidate = 0; candidate < BBT_CANDIDATE_BLOCKS; ++candidate)
   {
      const uint32_t page = NANDLOG_CHIP_RESERVED_BASE_PAGE + (candidate * NANDLOG_CHIP_PAGES_PER_BLOCK);
      if (!table_block_entry(page, &header, &pair, false))
         continue;
      const uint8_t programmed = (uint8_t)~read_marker(page);
      if (!programmed)
      {
         found_pages[found] = page;
         entry_versions[found] = header.version;
         entry_pairs[found++] = pair;
      }
      else if (!(programmed & (programmed - 1)) && (header.version >= stray_version))
      {
         stray = page;
         stray_version = header.version;
         stray_pair = pair;
      }
   }
   const bool below = !found;
   for (uint32_t page = NANDLOG_CHIP_RESERVED_BASE_PAGE; below && (page > TABLE_SEARCH_FIRST_PAGE); )
   {
      page -= NANDLOG_CHIP_PAGES_PER_BLOCK;
      if (!may_hold_table(page) || block_marked(page) || !table_block_entry(page, &header, &pair, true))
         continue;
      uint32_t slot = found, lowest = 0;
      for (uint32_t i = 1; i < found; ++i)
         lowest = (entry_versions[i] < entry_versions[lowest]) ? i : lowest;
      if (found >= BBT_CANDIDATE_BLOCKS)
      {
         if (entry_versions[lowest] >= header.version)
            continue;
         slot = lowest;
      }
      else
         ++found;
      found_pages[slot] = page;
      entry_versions[slot] = header.version;
      entry_pairs[slot] = pair;
   }
   for (;;)
   {
      for (uint32_t round = 0; !loaded && (round < found); ++round)
      {
         uint32_t block = NO_CANDIDATE, version = 0, entry_pair = 0;
         for (uint32_t candidate = 0; candidate < found; ++candidate)
            if ((!round || (entry_versions[candidate] < bound)) && ((block == NO_CANDIDATE) || (entry_versions[candidate] > version)))
            {
               block = candidate;
               version = entry_versions[candidate];
               entry_pair = entry_pairs[candidate];
            }
         if (block == NO_CANDIDATE)
            break;
         bound = version;
         const uint32_t end = table_written_end(found_pages[block], entry_pair), pairs = (end + 1) / 2;
         if (!round)
         {
            // Pairs written after the newest that reads may read on a later boot, so number later versions past them
            newest = block;
            newest_end = end;
            table_version = version + (pairs - entry_pair);
         }
         for (pair = pairs; !loaded && pair--; )
         {
            // Read each copy once and keep the entries from the first that checks, since a torn page can read
            // differently each time
            copies = 0;
            at_limit = false;
            for (uint32_t copy = 0; copy < 2; ++copy)
            {
               table_header_t check;
               bool limit = false;
               const bool valid = table_page_valid(found_pages[block] + (2 * pair) + copy, &check, NULL, &limit);
               at_limit = at_limit || limit;
               if (!valid || (copies && (check.version != header.version)))
                  continue;
               if (!copies++)
               {
                  header = check;
                  memcpy(bad_block_lookup_table, scratch + (2 * sizeof(uint32_t)), header.count * sizeof(uint32_t));
               }
            }
            loaded = (copies > 0);
            torn += loaded ? 0 : 1;
            table_loaded_pair = pair;
         }
         home = block;
      }

      // Read disturb can program one bit of a mark. A reserve block passed over for a single-bit mark, holding a newer
      // version than the one loaded that does not name it, is taken as unmarked and the search runs again, at most once
      if (!loaded || below || (stray_version <= header.version) || table_contains(stray, header.count))
         break;
      found_pages[found] = stray;
      entry_versions[found] = stray_version;
      entry_pairs[found++] = stray_pair;
      loaded = false;
      torn = stray_version = 0;
   }
   if (!loaded)
   {
      // If no version loaded, the caller rebuilds the table from the marks. It is numbered past every version found and
      // goes to the reserve, after the newest block found there
      const uint32_t after = (newest < NO_CANDIDATE) ? found_pages[newest] : NO_TABLE_BLOCK;
      table_version += (newest < NO_CANDIDATE) ? 1 : 0;
      keep_table_in_reserve();
      table_candidate = candidate_of(after);
      return false;
   }

   // A count beyond the datasheet's allowance is reported, never discarded: forgetting a retired block is far worse
   // than remembering one too many
   if (header.count > BBM_DATASHEET_BAD_BLOCKS)
      NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: Bad-block table holds %u entries, more than the %u bad blocks the datasheet allows for\n", header.count, (uint32_t)BBM_DATASHEET_BAD_BLOCKS);
   set_table_count(header.count);

   // Carry on in the blocks the version came from: the reserve, or the pair below it
   const uint32_t home_page_found = found_pages[home], newest_page = found_pages[newest];
   if (below)
      keep_table_below(home_page_found);
   else
      keep_table_in_reserve();
   table_home = candidate_of(home_page_found);
   table_candidate = (candidate_of(newest_page) < NO_CANDIDATE) ? candidate_of(newest_page) : table_home;
   if (below)
      NANDLOG_LOG(NANDLOG_LOG_INFO, "INFO: Bad-block table below the reserve, block %u\n", (uint32_t)(home_page_found / NANDLOG_CHIP_PAGES_PER_BLOCK));

   // Settled only if both copies of the newest version read with margin and nothing was written after it, in its
   // block or the next. Otherwise the version is written again into a fresh block, so every later boot loads the same
   // table. A version from a block passed over for a stray mark bit is written again too
   const uint32_t next = next_table_candidate(table_candidate, bbm_index);
   table_header_t entry;
   bool settled = (copies == 2) && (home_page_found == newest_page) && !torn && !(newest_end & 1) &&
                  ((newest_end == NANDLOG_CHIP_PAGES_PER_BLOCK) || page_is_blank_raw(newest_page + newest_end));
   if (settled && (next < NO_CANDIDATE))
      settled = table_page_valid(candidate_page(next), &entry, NULL, NULL) || (!table_page_written(candidate_page(next)) && page_is_blank_raw(candidate_page(next)));
   const bool worn = settled && at_limit, disturbed = settled && (home_page_found == stray);
   settled = settled && !at_limit && !disturbed;
   table_next_page = settled ? newest_end : NANDLOG_CHIP_PAGES_PER_BLOCK;
   if (settled)
      table_version = header.version;
   else
   {
      // Rewrite the version, or fall back to the marks, so every later boot loads this table
      NANDLOG_LOG(NANDLOG_LOG_WARNINGS, worn ? "WARNING: The newest bad-block table version reads at the limit of the part's ECC; writing version %u again\n" :
                                        disturbed ? "WARNING: The newest bad-block table version's block reads a stray bit in its mark; writing version %u again\n" :
                                                    "WARNING: The newest bad-block table update may be torn; writing version %u again\n", header.version);
      write_protect(false);
      if (!persist_table(bbm_index))
      {
         const bool abandoned = abandon_table();
         NANDLOG_LOG(abandoned ? NANDLOG_LOG_WARNINGS : NANDLOG_LOG_ERRORS,
                     abandoned ? "WARNING: Bad-block table not written again; marks stand in\n" : "ERROR: Bad-block table not written again\n");
      }
      write_protect(true);
   }
   return true;
}

static bool enter_next_table_block(uint32_t *count)
{
   // Erase the next block the table may move to. A reserve block that will not erase is added to the table being
   // written and the next one is tried. Below the reserve a failed erase leaves the table nowhere to go
   for (uint32_t step = 1; step <= table_block_count; ++step)
   {
      const uint32_t candidate = next_table_candidate(table_candidate, *count);
      if (candidate == NO_CANDIDATE)
         return false;
      if (erase_block_raw(candidate_page(candidate)))
      {
         table_candidate = candidate;
         table_next_page = 0;
         return true;
      }
      table_pair_failed = table_relocated;
      if (table_relocated || !stage_bad_block(candidate_page(candidate), count))
         return false;
   }
   return false;
}

static void build_table_image(uint32_t count, uint32_t version)
{
   // The whole table as one page, in 'scratch'
   table_header_t header;
   memcpy(header.magic, BBT_MAGIC, BBT_MAGIC_LENGTH);
   header.version = version;
   header.count = count;
   const uint32_t covered = (2 * sizeof(uint32_t)) + (count * sizeof(uint32_t));
   memset(scratch, 0xFF, NANDLOG_CHIP_PAGE_SIZE_BYTES);
   memcpy(scratch + BBT_MAGIC_LENGTH + sizeof(uint32_t), &header.version, 2 * sizeof(uint32_t));
   memcpy(scratch + sizeof(header), bad_block_lookup_table, count * sizeof(uint32_t));
   header.crc = table_crc(scratch + BBT_MAGIC_LENGTH + sizeof(uint32_t), covered);
   memcpy(scratch, &header, sizeof(header));
}

static bool write_version(uint32_t page, uint32_t count, uint32_t version)
{
   // Assumes write protection has already been opened by the caller. Writes entries [0, count) as 'version' to both
   // pages of the pair at 'page', reading each back whole and with margin, since a boot would not settle on a copy
   // read at the ECC limit
   bool written = true;
   for (uint32_t copy = 0; written && (copy < 2); ++copy)
   {
      table_header_t check;
      bool at_limit = false;
      build_table_image(count, version);
      written = write_page_raw(scratch, page + copy) && table_page_valid(page + copy, &check, NULL, &at_limit) && !at_limit &&
                (check.version == version) && (check.count == count);
   }
   return written;
}

static bool persist_table(uint32_t count)
{
   // Assumes write protection has already been opened by the caller. Writes entries [0, count) as the next
   // version, to both pages of a pair, and makes them the table only once both have been read back from the part.
   // Every attempt is numbered afresh, so no two versions a boot might read ever share a number
   for (uint32_t tries = 0; tries <= table_block_count; ++tries)
   {
      if (((table_next_page + 2) > NANDLOG_CHIP_PAGES_PER_BLOCK) && !enter_next_table_block(&count))
         break;
      const uint32_t page = candidate_page(table_candidate) + table_next_page, version = ++table_version;
      if (table_next_page && !page_is_blank_raw(page))
      {
         // A cut left something here, so this block's run of updates ends
         table_next_page = NANDLOG_CHIP_PAGES_PER_BLOCK;
         continue;
      }
      if (write_version(page, count, version))
      {
         const uint32_t marked = bbm_index;
         table_home = table_candidate;
         table_next_page += 2;
         table_abandoned = table_write_failed = false;
         set_table_count(count);
         mark_new_entries(marked);
         return true;
      }

      // A page that will not take it ends the block's run too, so the next attempt starts a fresh block
      table_next_page = NANDLOG_CHIP_PAGES_PER_BLOCK;
   }
   table_write_failed = true;
   return false;
}

static bool yields_no_version(uint32_t page, bool may_erase)
{
   // Assumes write protection has already been opened by the caller. Makes a block yield no table version to a later
   // boot by marking it, since every search passes over a marked block, or failing that by erasing it if 'may_erase'
   if (mark_again(page))
      return true;
   return may_erase && erase_block_raw(page) && !table_page_written(page);
}

static bool reserve_yields_no_version(uint32_t home)
{
   // Assumes write protection has already been opened by the caller. Makes every written table block of the reserve
   // yield no version, except 'home', which the caller does last
   for (uint32_t candidate = 0; candidate < BBT_CANDIDATE_BLOCKS; ++candidate)
   {
      const uint32_t page = NANDLOG_CHIP_RESERVED_BASE_PAGE + (candidate * NANDLOG_CHIP_PAGES_PER_BLOCK);
      if ((page != home) && table_page_written(page) && !yields_no_version(page, true))
      {
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Block %u takes no mark\n", (uint32_t)(page / NANDLOG_CHIP_PAGES_PER_BLOCK));
         return false;
      }
   }
   return true;
}

static bool abandon_table(void)
{
   // Assumes write protection has already been opened by the caller. Enters degraded mode, where the spare-area marks
   // are the only record of retired blocks. Every entry is marked again and must read 00h. Then every block that may
   // hold a version is made to yield none, the newest version's block last, so a cut part-way still loads the newest
   // version. Returns false if a block will not take its mark, and the table then stands
   if (table_abandoned)
      return true;
   const uint32_t home = home_page();
   for (uint32_t i = 0; i < bbm_index; ++i)
      if (!table_kept_in(bad_block_lookup_table[i]) && !mark_again(bad_block_lookup_table[i]))
      {
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Block %u takes no mark\n", (uint32_t)(bad_block_lookup_table[i] / NANDLOG_CHIP_PAGES_PER_BLOCK));
         return false;
      }
   if (!reserve_yields_no_version(home))
      return false;
   for (uint32_t page = TABLE_SEARCH_FIRST_PAGE; page < NANDLOG_CHIP_RESERVED_BASE_PAGE; page += NANDLOG_CHIP_PAGES_PER_BLOCK)
      if (!table_contains(page, bbm_index) && may_hold_table(page) && !block_marked(page) && !erase_block_raw(page) && !mark_holds(page))
      {
         // A block below the reserve that carries the magic but is not in the table was left by an interrupted move.
         // It holds nothing of the log's, so it is erased, or marked if it will not erase
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Block %u takes no mark\n", (uint32_t)(page / NANDLOG_CHIP_PAGES_PER_BLOCK));
         return false;
      }
   for (uint32_t candidate = 0; table_relocated && (candidate < table_block_count); ++candidate)
      if ((candidate_page(candidate) != home) && !yields_no_version(candidate_page(candidate), false))
      {
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Block %u takes no mark\n", (uint32_t)(candidate_page(candidate) / NANDLOG_CHIP_PAGES_PER_BLOCK));
         return false;
      }
   if ((home != NO_TABLE_BLOCK) && !yields_no_version(home, !table_relocated))
   {
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Block %u takes no mark\n", (uint32_t)(home / NANDLOG_CHIP_PAGES_PER_BLOCK));
      return false;
   }
   table_abandoned = true;
   keep_table_in_reserve();
   return true;
}

static bool otp_page_holds(uint32_t otp_page, const uint8_t *marker, uint32_t length)
{
   uint8_t stored[BBT_MAGIC_LENGTH];
   write_register(STATUS_REGISTER_2, CONFIG_OTP);
   read_page_region(stored, otp_page, 0, length);
   write_register(STATUS_REGISTER_2, CONFIG_NORMAL);
   return memcmp(stored, marker, length) == 0;
}

static void otp_page_program(uint32_t otp_page, const uint8_t *marker, uint32_t length)
{
   // Assumes write protection has already been opened by the caller
   memset(scratch, 0x00, NANDLOG_CHIP_PAGE_SIZE_BYTES);
   memcpy(scratch, marker, length);
   write_register(STATUS_REGISTER_2, CONFIG_OTP);
   write_page_raw(scratch, otp_page);
   nandlog_port_transfer_write(COMMAND_WRITE_DISABLE, NULL, 0, NULL, 0);
   write_register(STATUS_REGISTER_2, CONFIG_NORMAL);
}

static void stage_marked_blocks(uint32_t *count)
{
   // Stage every block whose first spare byte is not FFh: the factory's mark or this driver's. The log leaves that byte
   // erased, so the scan works on a used part. It reads with ECC off so an uncorrectable page is not taken for a mark,
   // and any programmed bit counts, so a mark a cut left part-programmed reads the same at every boot
   write_register(STATUS_REGISTER_2, CONFIG_RAW);
   for (uint32_t page = 0; page < NANDLOG_CHIP_PAGE_COUNT; page += NANDLOG_CHIP_PAGES_PER_BLOCK)
   {
      uint8_t marker = 0xFF;
      read_page_region(&marker, page, NANDLOG_CHIP_PAGE_SIZE_BYTES, sizeof(marker));
      if (marker != 0xFF)
         stage_bad_block(page, count);
   }
   write_register(STATUS_REGISTER_2, CONFIG_NORMAL);
}

static void mark_spare_area(uint32_t page)
{
   // Assumes write protection has already been opened by the caller. Programs 00h where the factory puts its mark, with
   // ECC off so nothing else changes. This is a second program of the page, allowed because the block is retired
   memset(scratch, 0xFF, NANDLOG_CHIP_PAGE_WITH_SPARE_SIZE_BYTES);
   scratch[NANDLOG_CHIP_PAGE_SIZE_BYTES] = 0x00;
   write_register(STATUS_REGISTER_2, CONFIG_RAW);
   program_from_column_zero(scratch, NANDLOG_CHIP_PAGE_WITH_SPARE_SIZE_BYTES, page & NANDLOG_CHIP_BLOCK_MASK);
   write_register(STATUS_REGISTER_2, CONFIG_NORMAL);
}


// Chip Interface Functions --------------------------------------------------------------------------------------------

const nandlog_geometry_t *nandlog_chip_geometry(void)
{
   static const nandlog_geometry_t geometry = NANDLOG_CHIP_GEOMETRY_INITIALIZER;
   return &geometry;
}

bool nandlog_chip_probe(void)
{
   // Wait until the chip becomes accessible
   for (uint32_t retries = 0; retries < 1000; ++retries)
   {
      if (verify_device_id())
         return true;
      nandlog_port_delay_ms(1);
   }
   return false;
}

void nandlog_chip_init(void)
{
   // Programming below relies on the write-protect pin being released, as it is from reset
   nandlog_port_write_enable(true);
   nandlog_port_delay_ms(3);
   wait_until_not_busy();

   // Configure the memory chip
   write_register(STATUS_REGISTER_1, PROTECT_ALL);
   write_register(STATUS_REGISTER_2, CONFIG_NORMAL);

   // Load the newest table. Failing that, build it from the spare-area marks, as on a new part or in degraded mode
   set_table_count(0);
   table_version = 0;
   table_abandoned = table_write_failed = false;
   keep_table_in_reserve();
   const bool table_existed = otp_page_holds(OTP_TABLE_EXISTS_PAGE, table_exists_marker, sizeof(table_exists_marker));
   if (!load_table())
   {
      uint32_t count = 0;
      if (table_existed)
         NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: Bad-block table rebuilt from the marks\n");
      stage_marked_blocks(&count);
      write_protect(false);
      if (!persist_table(count))
      {
         // Nowhere in the reserve will take the table, so enter degraded mode. The table in RAM still applies for this
         // boot, and the log will be asked for a pair of blocks to keep it in
         set_table_count(count);
         const bool abandoned = abandon_table();
         NANDLOG_LOG(abandoned ? NANDLOG_LOG_WARNINGS : NANDLOG_LOG_ERRORS,
                     abandoned ? "WARNING: Bad-block table not written; marks stand in\n" : "ERROR: Bad-block table not written\n");
      }
      write_protect(true);
   }
   else
   {
      // Bring the marks up to date with the table, in case a cut fell between a version and its marks. A table loaded
      // from the first version of a pair below the reserve may come from an interrupted move, so the reserve's blocks
      // are again made to yield no version
      write_protect(false);
      if (table_relocated && !table_loaded_pair && !reserve_yields_no_version(NO_TABLE_BLOCK))
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The reserve may hold an old bad-block table\n");
      mark_new_entries(0);
      write_protect(true);
   }

   // Write the OTP marker only once a table is on the part. It is checked every boot, so a cut before it is harmless
   if (!table_existed && table_version)
   {
      write_protect(false);
      otp_page_program(OTP_TABLE_EXISTS_PAGE, table_exists_marker, sizeof(table_exists_marker));
      write_protect(true);
   }
}

void nandlog_chip_low_power(bool sleep)
{
   // This part has no deep-power-down command worth the wake-up latency
   (void)sleep;
}

nandlog_read_result_t nandlog_chip_read_page(uint8_t *buffer, uint32_t page)
{
   return read_page_region(buffer, page, 0, NANDLOG_CHIP_PAGE_SIZE_BYTES);
}

nandlog_read_result_t nandlog_chip_read_page_region(uint8_t *buffer, uint32_t page, uint32_t offset, uint32_t length)
{
   return read_page_region(buffer, page, offset, length);
}

bool nandlog_chip_page_is_blank(uint32_t page)
{
   return page_is_blank_raw(page);
}

bool nandlog_chip_write_page(const uint8_t *data, uint32_t page)
{
   write_protect(false);
   const bool success = write_page_raw(data, page);
   write_protect(true);
   return success;
}

bool nandlog_chip_erase_block(uint32_t page)
{
   write_protect(false);
   const bool success = erase_block_raw(page);
   write_protect(true);
   return success;
}

nandlog_copy_result_t nandlog_chip_copy_page(uint32_t source_page, uint32_t destination_page)
{
   // Latch the source page into the chip's cache register, then program that register straight back out to
   // the destination. There is deliberately no PROGRAM DATA LOAD between the two, which is the whole point:
   // the only thing that crosses the bus is six bytes of address. As with any program, a failure is not retried
   uint8_t source_address[3], destination_address[3];
   split_page_address(source_page, source_address);
   split_page_address(destination_page, destination_address);

   write_protect(false);
   wait_until_not_busy();
   nandlog_port_transfer_write(COMMAND_PAGE_DATA_READ, NULL, 0, source_address, sizeof(source_address));
   wait_until_not_busy();
   nandlog_port_transfer_write(COMMAND_WRITE_ENABLE, NULL, 0, NULL, 0);
   nandlog_port_transfer_write(COMMAND_PROGRAM_EXECUTE, NULL, 0, destination_address, sizeof(destination_address));
   const bool programmed = (wait_until_not_busy() & STATUS_WRITE_FAILURE) != STATUS_WRITE_FAILURE;
   write_protect(true);

   // An uncorrectable source page is not checked for here, because the log copies only pages it has read whole
   return programmed ? NANDLOG_COPY_OK : NANDLOG_COPY_FAILED;
}

bool nandlog_chip_is_bad_block(uint32_t page)
{
   // Search the entries on the part for the block address. A miss records the run of good blocks around the block,
   // so later lookups in that run need no search
   page &= NANDLOG_CHIP_BLOCK_MASK;
   if ((page >= clear_first_page) && (page < clear_end_page))
      return false;
   uint32_t first = 0, end = NANDLOG_CHIP_PAGE_COUNT;
   for (uint32_t i = 0; i < bbm_index; ++i)
   {
      const uint32_t entry = bad_block_lookup_table[i];
      if (entry == page)
         return true;
      if ((entry < page) && (entry + NANDLOG_CHIP_PAGES_PER_BLOCK > first))
         first = entry + NANDLOG_CHIP_PAGES_PER_BLOCK;
      else if ((entry > page) && (entry < end))
         end = entry;
   }
   clear_first_page = first;
   clear_end_page = end;
   return false;
}

bool nandlog_chip_mark_bad_block(uint32_t page)
{
   // Retiring a block already retired is a no-op, not a new table version
   page &= NANDLOG_CHIP_BLOCK_MASK;
   if (nandlog_chip_is_bad_block(page))
      return true;

   // Write a new version holding the block, verified before the RAM copy changes. If no block will take it, enter
   // degraded mode and retire the block by its mark, which must read back exactly 00h
   uint32_t count = bbm_index;
   if (!stage_bad_block(page, &count))
      return false;
   write_protect(false);
   bool retired = persist_table(count);
   if (!retired && abandon_table())
   {
      mark_spare_area(page);
      retired = (read_marker(page) == 0x00);
      if (retired)
      {
         set_table_count(count);
         NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: Block %u retired by its mark\n", (uint32_t)(page / NANDLOG_CHIP_PAGES_PER_BLOCK));
      }
   }
   write_protect(true);
   if (!retired)
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Bad-block table could not be written; block %u not retired\n", (uint32_t)(page / NANDLOG_CHIP_PAGES_PER_BLOCK));
   return retired;
}

uint32_t nandlog_chip_table_blocks_wanted(uint32_t *lowest_page)
{
   // Ask for an aligned pair of blocks within the boot's search when the table has no usable home: no version to
   // build on, the last write failed, or the current block is full with nowhere to move
   *lowest_page = TABLE_SEARCH_FIRST_PAGE;
   const bool homeless = (table_home >= table_block_count) || table_write_failed ||
                         (((table_next_page + 2) > NANDLOG_CHIP_PAGES_PER_BLOCK) && (next_table_candidate(table_candidate, bbm_index) == NO_CANDIDATE));
   return homeless ? BBT_RELOCATED_BLOCKS : 0;
}

bool nandlog_chip_table_take_blocks(uint32_t first_page)
{
   // Move the table to the aligned pair at 'first_page', which the log has given up. Neither block may carry a mark,
   // which would hide the table. The first version goes to the lower block, which the log reaches first should a cut
   // stop the move. Only then is the old table dropped: the reserve's blocks are made to yield no version, the newest
   // last, and an old pair below the reserve is marked with the other entries. On failure the lower block is erased
   // or marked
   const uint32_t low = first_page & TABLE_PAIR_MASK, high = low + NANDLOG_CHIP_PAGES_PER_BLOCK;
   if ((first_page != low) || (low < TABLE_SEARCH_FIRST_PAGE) || (high >= NANDLOG_CHIP_RESERVED_BASE_PAGE) ||
       table_contains(low, bbm_index) || table_contains(high, bbm_index))
      return false;
   bool taken = false;
   write_protect(false);
   if (!block_marked(low) && !block_marked(high) && erase_block_raw(high) && erase_block_raw(low))
   {
      uint32_t count = bbm_index;
      const uint32_t home = home_page();
      if (stage_bad_block(low, &count) && stage_bad_block(high, &count) && write_version(low, count, ++table_version))
      {
         taken = table_relocated || (reserve_yields_no_version(home) && ((home == NO_TABLE_BLOCK) || yields_no_version(home, true)));
         if (taken)
         {
            // A second version tells a later boot that the move finished, so it need not clear the reserve again
            keep_table_below(low);
            table_home = table_candidate = 0;
            table_next_page = write_version(low + 2, count, ++table_version) ? 4 : NANDLOG_CHIP_PAGES_PER_BLOCK;
            table_abandoned = table_write_failed = false;
            set_table_count(count);
            mark_new_entries(0);
         }
      }
      if (!taken && !erase_block_raw(low) && !mark_holds(low))
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Block %u takes no mark\n", (uint32_t)(low / NANDLOG_CHIP_PAGES_PER_BLOCK));
   }
   write_protect(true);
   if (taken)
      NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: Bad-block table moved below the reserve, blocks %u-%u\n", (uint32_t)(low / NANDLOG_CHIP_PAGES_PER_BLOCK), (uint32_t)(high / NANDLOG_CHIP_PAGES_PER_BLOCK));
   return taken;
}

bool nandlog_chip_reset_bad_blocks(void)
{
   // Discard the table so the next boot rebuilds it from the spare-area marks. Every entry is marked again first and
   // must read 00h, or nothing is discarded. Then every unmarked reserve block is erased, the newest version's last, so
   // no older version can load in its place. A table below the reserve is abandoned as in degraded mode instead, since
   // its blocks must stay retired. The table in RAM stands until the reboot
   write_protect(false);
   for (uint32_t i = 0; i < bbm_index; ++i)
      if (!table_kept_in(bad_block_lookup_table[i]))
         mark_spare_area(bad_block_lookup_table[i]);
   for (uint32_t i = 0; i < bbm_index; ++i)
      if (!table_kept_in(bad_block_lookup_table[i]) && (read_marker(bad_block_lookup_table[i]) != 0x00))
      {
         write_protect(true);
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Retired block %u carries no bad-block mark, so a rebuild would forget it; table kept\n",
                     (uint32_t)(bad_block_lookup_table[i] / NANDLOG_CHIP_PAGES_PER_BLOCK));
         return false;
      }
   if (table_relocated)
   {
      const bool abandoned = abandon_table();
      write_protect(true);
      if (!abandoned)
      {
         NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: Bad-block table kept\n");
         return false;
      }
   }
   else
   {
      const uint32_t home = home_page();
      bool older_left = false;
      for (uint32_t page = NANDLOG_CHIP_RESERVED_BASE_PAGE; page < NANDLOG_CHIP_PAGE_COUNT; page += NANDLOG_CHIP_PAGES_PER_BLOCK)
         if ((page != home) && !block_marked(page) && !erase_block_raw(page) && table_page_written(page))
            older_left = true;
      if (!older_left && (home != NO_TABLE_BLOCK) && erase_block_raw(home))
      {
         table_home = NO_CANDIDATE;
         table_next_page = NANDLOG_CHIP_PAGES_PER_BLOCK;
      }
      write_protect(true);
   }

   // Verify rather than assume: no unmarked block a boot would search may carry the magic
   uint32_t tables_remaining = 0;
   for (uint32_t page = NANDLOG_CHIP_RESERVED_BASE_PAGE; page < NANDLOG_CHIP_PAGE_COUNT; page += NANDLOG_CHIP_PAGES_PER_BLOCK)
   {
      uint8_t magic[BBT_MAGIC_LENGTH];
      read_page_region(magic, page, 0, sizeof(magic));
      if (!memcmp(magic, BBT_MAGIC, BBT_MAGIC_LENGTH) && !block_marked(page))
         ++tables_remaining;
   }
   for (uint32_t page = TABLE_SEARCH_FIRST_PAGE; page < NANDLOG_CHIP_RESERVED_BASE_PAGE; page += NANDLOG_CHIP_PAGES_PER_BLOCK)
      if (may_hold_table(page) && !block_marked(page))
         ++tables_remaining;
   if (tables_remaining)
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: %u bad-block table block(s) still present after erase\n", tables_remaining);
   return (tables_remaining == 0);
}
