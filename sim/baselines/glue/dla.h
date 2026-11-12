// DLA: a minimal append-only record log over Dhara's map layer. Baseline harness code, not part of nandlog.
//
// What a careful integrator would write in an afternoon to log records through Dhara:
//
//   - records are framed exactly as nandlog frames them ([length:2][type:1][timestamp:4][data]) and are never
//     split across sectors, so a sector holds the same records a nandlog page does;
//   - a sector is one Dhara sector (= one NAND page) with a 16-byte header (magic, sequence, length, count, CRC-32);
//   - logical sectors are written in order 0, 1, 2, ... and wrap at a FIXED capacity (Dhara's, with the part's
//     worst-case 80 bad blocks assumed; dhara_map_capacity() itself moves), overwriting the oldest;
//   - a record that does not fit writes the assembled sector and starts the next one (same lazy rule as nandlog);
//   - dla_flush() writes the partial sector and calls dhara_map_sync(). The partial sector keeps filling in RAM and
//     is REWRITTEN in place by the next write of that logical sector -- the FTL's rewrite is used, so a flush
//     never wastes logical capacity;
//   - optionally (sync_each_sector) every sector write is followed by dhara_map_sync(), which gives the same
//     durability point as a nandlog page commit.
//
// Durability: a record is durable once the sector holding it has been written AND Dhara has reached a checkpoint
// after that write -- on dhara_map_sync(), or automatically once Dhara fills its checkpoint group (every
// 2^log2_ppc - 1 user pages; 15 for a 4 KiB page).
#ifndef DLA_H
#define DLA_H

#include <stdbool.h>
#include <stdint.h>
#include "map.h"

#define DLA_MAGIC           0x31414C44u      // 'DLA1', little-endian
#define DLA_FRAME_BYTES     7u               // [length:2][type:1][timestamp:4], as NANDLOG_RECORD_FRAMING=1

typedef struct __attribute__ ((__packed__))
{
   uint32_t magic;
   uint32_t seq;                 // sector sequence number, monotonic across logical wrap
   uint16_t payload_length;
   uint16_t record_count;
   uint32_t payload_crc;         // CRC-32 (IEEE) over the payload
} dla_sector_header_t;

typedef struct
{
   struct dhara_map map;
   uint8_t *sector;              // one page: header + payload being assembled
   uint32_t sector_bytes, payload_max, capacity;
   uint32_t used;                // payload bytes assembled
   uint32_t seq;                 // sequence number of the sector being assembled
   dhara_sector_t current;       // logical sector being assembled
   uint16_t count;
   bool dirty;                   // the assembled sector holds records not yet written to Dhara
   bool sync_each_sector;
} dla_t;

// Resume the map (or start an empty one) and find where to append. 'map_page_buf' and 'sector_buf' are each one page
int dla_mount(dla_t *l, const struct dhara_nand *nand, uint8_t *map_page_buf, uint8_t *sector_buf, uint8_t gc_ratio, bool sync_each_sector);

// Append one record; 0 on success, -1 on error (record too large, or a Dhara error)
int dla_append(dla_t *l, uint8_t type, uint32_t timestamp, const void *data, uint16_t length);

// Write the partial sector, if anything is unwritten, and checkpoint the map
int dla_flush(dla_t *l);

// Read a logical sector back and validate it; returns its payload length, or -1. For the harness's verification pass
int dla_read_sector(dla_t *l, dhara_sector_t s, uint8_t *buf, dla_sector_header_t *header);

#endif
