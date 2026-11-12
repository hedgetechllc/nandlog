// Winbond W25N01GWZEIG SPI NAND driver

// Chip Declaration ----------------------------------------------------------------------------------------------------

#define NANDLOG_CHIP_NAME                           "W25N01GWZEIG"
#define NANDLOG_CHIP_PAGE_SIZE_BYTES                2048
#define NANDLOG_CHIP_SPARE_SIZE_BYTES               64
#define NANDLOG_CHIP_PAGES_PER_BLOCK                64
#define NANDLOG_CHIP_RESERVED_BLOCKS                40

// Overridable only so a host test can fill the log region quickly with fewer blocks
#ifndef NANDLOG_CHIP_BLOCK_COUNT
#define NANDLOG_CHIP_BLOCK_COUNT                    1024
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
#define COMMAND_WRITE_BBM_LUT                       0xA1
#define COMMAND_READ_BBM_LUT                        0xA5


// Status Registers ----------------------------------------------------------------------------------------------------

#define STATUS_REGISTER_1                           0xA0
#define STATUS_REGISTER_2                           0xB0
#define STATUS_REGISTER_3                           0xC0

// The ECC status is bits 5:4, where both 10b and 11b mean the page could not be corrected. Bit 5 alone decides, and
// this part has no "corrected at the limit" status
#define STATUS_LUT_FULL                             0b01000000
#define STATUS_PAGE_FATAL_ERROR                     0b00100000
#define STATUS_WRITE_FAILURE                        0b00001000
#define STATUS_ERASE_FAILURE                        0b00000100
#define STATUS_BUSY                                 0b00000001

// What a timed-out wait returns if nandlog_port_fatal() does: every failure bit, so no caller reads it as success
#define STATUS_TIMED_OUT                            (STATUS_PAGE_FATAL_ERROR | STATUS_WRITE_FAILURE | STATUS_ERASE_FAILURE)

// Block-protect patterns for status register 1: everything locked, or the whole array writable
#define PROTECT_ALL                                 0b01111110
#define PROTECT_NONE                                0b00000010

// Status register 2 in normal operation, with ECC off for raw reads, and with the OTP pages mapped over the main array
#define CONFIG_NORMAL                               0b00011001
#define CONFIG_RAW                                  0b00001001
#define CONFIG_OTP                                  0b01011001

// Page of the OTP region carrying the marker that says this part has been initialized before
#define FIRST_BOOT_ADDRESS                          0x02

// Entries in the chip's hardware LUT. Fixed by the part, not a policy choice. The first boot remaps every
// factory-bad block, so those entries are not available to blocks that fail later
#define BBM_LUT_NUM_ENTRIES                         20

// A block address is carried in bits 6..15 of a page address, and the LUT holds it in that shifted form
#define PAGE_TO_LUT_BLOCK(page)                     (((page) & 0x0000FFC0) >> 6)
#define LUT_ADDRESS_MASK                            0x3FF


// Helper Structures ---------------------------------------------------------------------------------------------------

typedef struct __attribute__ ((__packed__)) { uint16_t lba, pba; } bbm_lut_t;


// Static Global Variables ---------------------------------------------------------------------------------------------

static uint32_t scratch_words[(NANDLOG_CHIP_PAGE_WITH_SPARE_SIZE_BYTES + 3) / 4];   // word-aligned, for the blank check
static uint8_t *const scratch = (uint8_t*)scratch_words;
static bbm_lut_t bad_block_lookup_table[BBM_LUT_NUM_ENTRIES];

// The identity this part reports, which doubles as the marker written to its OTP page on a first boot
static const uint8_t device_id[3] = { 0xEF, 0xBA, 0x21 };


// Private Helper Functions --------------------------------------------------------------------------------------------

static inline uint16_t byte_swap16(uint16_t value)
{
   // The LUT is transferred most significant byte first, unlike everything else on this bus
   return (uint16_t)(((value << 8) & 0xFF00) | ((value >> 8) & 0x00FF));
}

static void split_page_address(uint32_t page, uint8_t *out)
{
   // Page addresses go out most significant byte first
   out[0] = (uint8_t)((page & 0x00FF0000) >> 16);
   out[1] = (uint8_t)((page & 0x0000FF00) >> 8);
   out[2] = (uint8_t)(page & 0x000000FF);
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
   return ((status & STATUS_PAGE_FATAL_ERROR) == STATUS_PAGE_FATAL_ERROR) ? NANDLOG_READ_UNCORRECTABLE : NANDLOG_READ_OK;
}

static nandlog_read_result_t read_page_raw(uint8_t *buffer, uint32_t page, uint32_t length)
{
   return read_page_region(buffer, page, 0, length);
}

static bool write_page_raw(const uint8_t *data, uint32_t page)
{
   // Assumes write protection has already been opened by the caller. The load starts at column 0 because PROGRAM
   // EXECUTE programs the whole buffer, and a load need not clear what a read left before its column. A failed
   // program is not retried, since the page may already hold part of it
   const uint8_t column_address[2] = { 0x00, 0x00 };
   uint8_t page_number_reordered[3];
   split_page_address(page, page_number_reordered);
   wait_until_not_busy();
   nandlog_port_transfer_write(COMMAND_WRITE_ENABLE, NULL, 0, NULL, 0);
   nandlog_port_transfer_write(COMMAND_PROGRAM_DATA_LOAD, column_address, sizeof(column_address), data, NANDLOG_CHIP_PAGE_SIZE_BYTES);
   wait_until_not_busy();
   nandlog_port_transfer_write(COMMAND_PROGRAM_EXECUTE, NULL, 0, page_number_reordered, sizeof(page_number_reordered));
   return (wait_until_not_busy() & STATUS_WRITE_FAILURE) != STATUS_WRITE_FAILURE;
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

static bool verify_device_id(void)
{
   uint8_t device_id_read[4];
   nandlog_port_transfer_read(COMMAND_READ_DEVICE_ID, NULL, 0, device_id_read, sizeof(device_id_read));
   return (memcmp(device_id_read + 1, device_id, sizeof(device_id)) == 0);
}

static void load_bad_block_table(void)
{
   // Read the chip's own LUT and byte-swap it into host order once, so every later comparison is plain. An entry
   // the chip has not used reads back as zero in both halves
   uint8_t dummy_value = 0;
   nandlog_port_transfer_read(COMMAND_READ_BBM_LUT, &dummy_value, 1, &bad_block_lookup_table, sizeof(bad_block_lookup_table));
   for (uint32_t i = 0; i < BBM_LUT_NUM_ENTRIES; ++i)
   {
      bad_block_lookup_table[i].lba = byte_swap16(bad_block_lookup_table[i].lba) & LUT_ADDRESS_MASK;
      bad_block_lookup_table[i].pba = byte_swap16(bad_block_lookup_table[i].pba) & LUT_ADDRESS_MASK;
   }
}

static inline bool lut_entry_used(uint32_t i)
{
   // Every spare block is in the reserve at the top of the array, so a used entry never names block 0 as its spare
   return bad_block_lookup_table[i].pba != 0;
}

static bool first_boot_done(void)
{
   write_register(STATUS_REGISTER_2, CONFIG_OTP);
   read_page_raw(scratch, FIRST_BOOT_ADDRESS, sizeof(device_id));
   write_register(STATUS_REGISTER_2, CONFIG_NORMAL);
   return memcmp(scratch, device_id, sizeof(device_id)) == 0;
}

static void mark_first_boot_done(void)
{
   // Assumes write protection has already been opened by the caller
   memset(scratch, 0, NANDLOG_CHIP_PAGE_SIZE_BYTES);
   memcpy(scratch, device_id, sizeof(device_id));
   write_register(STATUS_REGISTER_2, CONFIG_OTP);
   write_page_raw(scratch, FIRST_BOOT_ADDRESS);
   nandlog_port_transfer_write(COMMAND_WRITE_DISABLE, NULL, 0, NULL, 0);
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

   // Mirror the chip's existing remappings into RAM
   load_bad_block_table();

   // Scan for factory-marked bad blocks until a first boot has finished, looking for the mark at the start of each
   // block's first page. The OTP marker is written last, so a power cut during the scan only repeats it
   if (!first_boot_done())
   {
      for (uint32_t page = 0; page < NANDLOG_CHIP_RESERVED_BASE_PAGE; page += NANDLOG_CHIP_PAGES_PER_BLOCK)
         if ((read_page_raw(scratch, page, 1) == NANDLOG_READ_UNCORRECTABLE) || (scratch[0] != 0xFF))
            nandlog_chip_mark_bad_block(page);
      write_protect(false);
      mark_first_boot_done();
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
   return read_page_raw(buffer, page, NANDLOG_CHIP_PAGE_SIZE_BYTES);
}

nandlog_read_result_t nandlog_chip_read_page_region(uint8_t *buffer, uint32_t page, uint32_t offset, uint32_t length)
{
   return read_page_region(buffer, page, offset, length);
}

bool nandlog_chip_page_is_blank(uint32_t page)
{
   // Read the main area and spare with ECC off, so the check sees the raw cells
   write_register(STATUS_REGISTER_2, CONFIG_RAW);
   read_page_region(scratch, page, 0, NANDLOG_CHIP_PAGE_WITH_SPARE_SIZE_BYTES);
   write_register(STATUS_REGISTER_2, CONFIG_NORMAL);
   return nandlog_bytes_erased(scratch, NANDLOG_CHIP_PAGE_WITH_SPARE_SIZE_BYTES);
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
   // Search for the block address among the LUT entries the chip has used
   const uint16_t block = (uint16_t)PAGE_TO_LUT_BLOCK(page);
   for (uint32_t i = 0; i < BBM_LUT_NUM_ENTRIES; ++i)
      if (lut_entry_used(i) && (bad_block_lookup_table[i].lba == block))
         return true;
   return false;
}

bool nandlog_chip_mark_bad_block(uint32_t page)
{
   // The chip's LUT remaps one-way and no command removes an entry, so a block that is already retired must
   // not be given a second spare block
   if (nandlog_chip_is_bad_block(page))
      return true;
   if (read_register(STATUS_REGISTER_3) & STATUS_LUT_FULL)
   {
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: The part's remap table is full; block %u not retired\n", (uint32_t)PAGE_TO_LUT_BLOCK(page));
      return false;
   }

   // Find first available workaround block
   uint16_t workaround_block = 0;
   for (uint32_t candidate = NANDLOG_CHIP_RESERVED_BASE_PAGE; !workaround_block && (candidate < NANDLOG_CHIP_PAGE_COUNT); candidate += NANDLOG_CHIP_PAGES_PER_BLOCK)
      if ((read_page_raw(scratch, candidate, 1) != NANDLOG_READ_UNCORRECTABLE) && (scratch[0] == 0xFF))
      {
         // Ensure that the candidate block is not already in use
         workaround_block = (uint16_t)PAGE_TO_LUT_BLOCK(candidate);
         for (uint32_t i = 0; i < BBM_LUT_NUM_ENTRIES; ++i)
            if (lut_entry_used(i) && (bad_block_lookup_table[i].pba == workaround_block))
            {
               workaround_block = 0;
               break;
            }
      }

   // Every spare is already spoken for, so there is nothing further this part can do
   if (!workaround_block)
   {
      NANDLOG_LOG(NANDLOG_LOG_ERRORS, "ERROR: No spare block available to remap block %u\n", (uint32_t)PAGE_TO_LUT_BLOCK(page));
      return false;
   }

   // Hand the remapping to the chip, which is what makes it survive a power cycle
   const uint16_t block = (uint16_t)PAGE_TO_LUT_BLOCK(page);
   bbm_lut_t destination_address = { .lba = byte_swap16(block), .pba = byte_swap16(workaround_block) };
   write_protect(false);
   nandlog_port_transfer_write(COMMAND_WRITE_ENABLE, NULL, 0, NULL, 0);
   nandlog_port_transfer_write(COMMAND_WRITE_BBM_LUT, NULL, 0, &destination_address, sizeof(destination_address));
   wait_until_not_busy();
   write_protect(true);

   // Re-read the LUT from the chip, so the block counts as retired only if the chip really holds the entry
   load_bad_block_table();
   return nandlog_chip_is_bad_block(page);
}

uint32_t nandlog_chip_table_blocks_wanted(uint32_t *lowest_page)
{
   // The part keeps its own remap table and needs no blocks from the log
   *lowest_page = NANDLOG_CHIP_RESERVED_BASE_PAGE;
   return 0;
}

bool nandlog_chip_table_take_blocks(uint32_t first_page)
{
   (void)first_page;
   return false;
}

bool nandlog_chip_reset_bad_blocks(void)
{
   // Entries in the chip's hardware LUT are one-way and no command removes them, so nothing is erased and the
   // caller is told the table stands
   NANDLOG_LOG(NANDLOG_LOG_WARNINGS, "WARNING: This part manages bad blocks in hardware; its LUT cannot be cleared\n");
   return false;
}
