#ifndef __NANDLOG_CHIP_HEADER_H__
#define __NANDLOG_CHIP_HEADER_H__

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nandlog_conf.h"


// Integrator Configuration Check --------------------------------------------------------------------------------------

#ifndef NANDLOG_MAX_PAGE_SIZE_BYTES
#error "nandlog_conf.h must define NANDLOG_MAX_PAGE_SIZE_BYTES"
#endif
#ifndef NANDLOG_MAX_SPARE_SIZE_BYTES
#error "nandlog_conf.h must define NANDLOG_MAX_SPARE_SIZE_BYTES"
#endif
#ifndef NANDLOG_HAS_HARDWARE
#error "nandlog_conf.h must define NANDLOG_HAS_HARDWARE"
#endif
#ifndef NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL
#error "nandlog_conf.h must define NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL"
#endif
#ifndef NANDLOG_PAGE_PLACEMENT_ATTEMPTS
#error "nandlog_conf.h must define NANDLOG_PAGE_PLACEMENT_ATTEMPTS"
#endif
#ifndef NANDLOG_ERASE_AHEAD_BLOCKS
#error "nandlog_conf.h must define NANDLOG_ERASE_AHEAD_BLOCKS"
#endif
#ifndef NANDLOG_MAX_EPOCH_DETAILS_BYTES
#error "nandlog_conf.h must define NANDLOG_MAX_EPOCH_DETAILS_BYTES"
#endif
#ifndef NANDLOG_TIMESTAMP_TOLERANCE_MS
#error "nandlog_conf.h must define NANDLOG_TIMESTAMP_TOLERANCE_MS"
#endif
#ifndef NANDLOG_BUSY_POLL_INTERVAL_US
#error "nandlog_conf.h must define NANDLOG_BUSY_POLL_INTERVAL_US"
#endif
#ifndef NANDLOG_BUSY_TIMEOUT_POLLS
#error "nandlog_conf.h must define NANDLOG_BUSY_TIMEOUT_POLLS"
#endif
#ifndef NANDLOG_CHIP_PAGE_COPY
#error "nandlog_conf.h must define NANDLOG_CHIP_PAGE_COPY"
#endif


// Nandlog Chip Type Definitions ---------------------------------------------------------------------------------------

// NOTE: Every function below that touches the bus assumes the port is already awake and the part is out of
// its low-power state. The log core guarantees that for itself. It wakes both around each operation and
// puts them back afterwards, or holds them awake for the length of a session, and nandlog_init()
// deliberately leaves them asleep when it returns.

// What the part is, as reported by the driver that knows. Valid at any time, including before
// nandlog_chip_init(), because these are properties of the silicon rather than of its state
typedef struct
{
   uint32_t page_size_bytes;     // bytes in a page's main array, excluding the spare area
   uint32_t spare_size_bytes;    // spare/ECC bytes that follow it
   uint32_t pages_per_block;     // erase granularity, in pages; always a power of two
   uint32_t block_count;         // blocks in the whole array
   uint32_t reserved_blocks;     // blocks at the top of the array the driver keeps for bad-block management
} nandlog_geometry_t;

// What a part made of a request to relocate a page without the host handling its contents
typedef enum
{
   NANDLOG_COPY_OK,              // the part did it; nothing but addresses crossed the bus
   NANDLOG_COPY_FAILED,          // the part tried and reported a program failure, so the block is suspect
   NANDLOG_COPY_UNSUPPORTED      // the part has no such command; the caller must read and write it itself
} nandlog_copy_result_t;

// What the part's ECC made of a page read. Only zero means the data cannot be used
typedef enum
{
   NANDLOG_READ_UNCORRECTABLE = 0,  // the part's ECC could not correct the page, so the buffer is untrustworthy
   NANDLOG_READ_OK = 1,             // no error, or errors the part corrected with margin to spare
   NANDLOG_READ_AT_ECC_LIMIT = 2    // corrected, but at the limit of the part's ECC, so one more bit error would not be
} nandlog_read_result_t;


// Chip-Specific Required Functions ------------------------------------------------------------------------------------

// Get the geometry of the NAND chip
const nandlog_geometry_t *nandlog_chip_geometry(void);

// Confirm the part is present and answering. Safe to call before nandlog_chip_init()
bool nandlog_chip_probe(void);

// Bring the chip into service: configure its status registers, load the persisted bad-block table or,
// on a first boot, build one from the factory markers and persist it. Call once, after nandlog_chip_probe()
void nandlog_chip_init(void);

// Enter or leave the chip's lowest-power state
void nandlog_chip_low_power(bool sleep);

// Read a whole page, with the part's ECC enabled
nandlog_read_result_t nandlog_chip_read_page(uint8_t *buffer, uint32_t page);

// Read 'length' bytes of a page starting at 'offset'. Same return meaning as nandlog_chip_read_page(), which
// is this function over the whole page.
nandlog_read_result_t nandlog_chip_read_page_region(uint8_t *buffer, uint32_t page, uint32_t offset, uint32_t length);

// Whether a page is erased, with no programmed bit in its main or spare area. Read it with the part's ECC bypassed
// where possible, because ECC can hide a few stray programmed bits. When unsure, answer false, which only costs a page
bool nandlog_chip_page_is_blank(uint32_t page);

// Program a whole page with a single PROGRAM EXECUTE. False means the part reported a program failure, and the
// page must not be programmed again. Whether that block is then retired is the caller's decision
bool nandlog_chip_write_page(const uint8_t *data, uint32_t page);

// Erase the block containing 'page', attempting it up to NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL times. False
// means the part reported an erase failure every time
bool nandlog_chip_erase_block(uint32_t page);

// Relocate one page to another in the chip rather than through the host. A part with no such command answers
// NANDLOG_COPY_UNSUPPORTED unconditionally and without touching the bus, and the caller falls back to a
// read and a write. Whether the block is then retired stays the caller's decision, exactly as for a write
nandlog_copy_result_t nandlog_chip_copy_page(uint32_t source_page, uint32_t destination_page);

// Whether the block containing 'page' is known bad
bool nandlog_chip_is_bad_block(uint32_t page);

// Retire the block containing 'page' and return whether it is now retired. The log lays its pages out over the good
// blocks, so a retirement must survive a power cut whole or not at all, and must not show in
// nandlog_chip_is_bad_block() until it is persistent
bool nandlog_chip_mark_bad_block(uint32_t page);

// How many blocks the bad-block table needs from the log because it has run out of room, or zero. Sets
// 'lowest_page' to the lowest page the run may start at, and the run starts on a multiple of that many blocks
uint32_t nandlog_chip_table_blocks_wanted(uint32_t *lowest_page);

// Take the run of blocks starting at 'first_page' for the bad-block table and retire them. True if the table is now
// kept there with the whole run retired. False leaves none of them retired
bool nandlog_chip_table_take_blocks(uint32_t first_page);

// RECOVERY UTILITY. Discard the persisted bad-block table so it is rebuilt on the next boot. Returns whether
// the table was verified gone afterwards, rather than assuming the erase took. A part that cannot safely forget
// its table keeps it, logs why, and returns false
bool nandlog_chip_reset_bad_blocks(void);

// Whether every byte is FFh, for the library and the drivers. It checks 16 bytes a step because a blank check scans
// a whole page, and loads through memcpy() so the compiler can use word loads without alignment faults
static inline bool nandlog_bytes_erased(const void *data, uint32_t length)
{
   const uint8_t *bytes = (const uint8_t*)data;
   for (; length >= 16; bytes += 16, length -= 16)
   {
      uint32_t words[4];
      memcpy(words, bytes, sizeof(words));
      if ((words[0] & words[1] & words[2] & words[3]) != 0xFFFFFFFFu)
         return false;
   }
   for (; length; ++bytes, --length)
      if (*bytes != 0xFF)
         return false;
   return true;
}

#endif  // #ifndef __NANDLOG_CHIP_HEADER_H__
