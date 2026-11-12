// DLA: a minimal append-only record log over Dhara's map layer. See dla.h. Baseline harness code.
#include <string.h>
#include "dla.h"
#include "dhara_glue.h"

#ifdef DHARA_GLUE_COUNTERS
uint32_t dla_sector_writes;      // harness-only instrumentation: dhara_map_write() calls DLA made
#endif

static uint32_t dla_crc32(const uint8_t *bytes, uint32_t length)
{
   // Bitwise IEEE CRC-32: small, and the cost is irrelevant to the flash-operation counts
   uint32_t crc = 0xFFFFFFFFu;
   for (uint32_t i = 0; i < length; ++i)
   {
      crc ^= bytes[i];
      for (uint32_t bit = 0; bit < 8; ++bit)
         crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
   }
   return ~crc;
}

static bool header_valid(const dla_sector_header_t *h, const uint8_t *payload, uint32_t payload_max, bool check_crc)
{
   if ((h->magic != DLA_MAGIC) || (h->payload_length > payload_max))
      return false;
   return !check_crc || (dla_crc32(payload, h->payload_length) == h->payload_crc);
}

static int read_header(dla_t *l, dhara_sector_t s, dla_sector_header_t *h)
{
   // Locate the sector and clock out only its header: one map lookup plus one page latch
   dhara_page_t p;
   dhara_error_t err;
   if (dhara_map_find(&l->map, s, &p, &err) < 0)
      return -1;
   if (dhara_nand_read(l->map.journal.nand, p, 0, sizeof(*h), (uint8_t*)h, &err) < 0)
      return -1;
   return 0;
}

static int write_current(dla_t *l)
{
   dla_sector_header_t h;
   h.magic = DLA_MAGIC;
   h.seq = l->seq;
   h.payload_length = (uint16_t)l->used;
   h.record_count = l->count;
   h.payload_crc = dla_crc32(l->sector + sizeof(h), l->used);
   memcpy(l->sector, &h, sizeof(h));
   memset(l->sector + sizeof(h) + l->used, 0xFF, l->payload_max - l->used);
   dhara_error_t err;
#ifdef DHARA_GLUE_COUNTERS
   ++dla_sector_writes;
#endif
   if (dhara_map_write(&l->map, l->current, l->sector, &err) < 0)
      return -1;
   l->dirty = false;
   return 0;
}

int dla_mount(dla_t *l, const struct dhara_nand *nand, uint8_t *map_page_buf, uint8_t *sector_buf, uint8_t gc_ratio, bool sync_each_sector)
{
   memset(l, 0, sizeof(*l));
   l->sector = sector_buf;
   l->sector_bytes = 1u << nand->log2_page_size;
   l->payload_max = l->sector_bytes - (uint32_t)sizeof(dla_sector_header_t);
   l->sync_each_sector = sync_each_sector;
   dhara_map_init(&l->map, nand, map_page_buf, gc_ratio);

   dhara_error_t err;
   (void)dhara_map_resume(&l->map, &err);          // -1 on a blank part: an empty map, which is what we want
   // The sector ring's size must never move, and dhara_map_capacity() does (see dhara_glue.h)
   l->capacity = dhara_fixed_capacity(&l->map, DHARA_GLUE_ASSUMED_BAD_BLOCKS);
   const dhara_sector_t n = dhara_map_size(&l->map);
   if (!n)
      return 0;

   // The newest sector. Until the log first wraps, sectors 0 .. n-1 are all written, in order. After that every
   // sector is mapped and the newest is found by binary search on the sequence numbers, which rise around the circle
   dhara_sector_t newest = n - 1;
   if (n >= l->capacity)
   {
      dla_sector_header_t first, h;
      if (read_header(l, 0, &first) < 0)
         return -1;
      dhara_sector_t lo = 1, hi = l->capacity;     // first index whose seq is below sector 0's, or capacity
      while (lo < hi)
      {
         const dhara_sector_t mid = lo + ((hi - lo) / 2);
         if (read_header(l, mid, &h) < 0)
            return -1;
         if ((int32_t)(h.seq - first.seq) < 0)
            hi = mid;
         else
            lo = mid + 1;
      }
      newest = lo - 1;
   }

   // Continue the newest sector if it has room for another record, else start the one after it
   if (dhara_map_read(&l->map, newest, l->sector, &err) < 0)
      return -1;
   dla_sector_header_t h;
   memcpy(&h, l->sector, sizeof(h));
   if (!header_valid(&h, l->sector + sizeof(h), l->payload_max, true))
      return -1;
   l->seq = h.seq;
   l->current = newest;
   l->used = h.payload_length;
   l->count = h.record_count;
   if ((l->used + DLA_FRAME_BYTES + 1u) > l->payload_max)
   {
      l->current = (newest + 1) % l->capacity;
      l->seq++;
      l->used = l->count = 0;
   }
   return 0;
}

int dla_append(dla_t *l, uint8_t type, uint32_t timestamp, const void *data, uint16_t length)
{
   const uint32_t record = DLA_FRAME_BYTES + length;
   if (record > l->payload_max)
      return -1;
   if ((l->used + record) > l->payload_max)
   {
      // The assembled sector is complete: write it (unless a flush already has), then move to the next one
      if (l->dirty)
      {
         if (write_current(l) < 0)
            return -1;
         dhara_error_t err;
         if (l->sync_each_sector && (dhara_map_sync(&l->map, &err) < 0))
            return -1;
      }
      l->current = (l->current + 1) % l->capacity;
      l->seq++;
      l->used = l->count = 0;
   }
   uint8_t *at = l->sector + sizeof(dla_sector_header_t) + l->used;
   memcpy(at, &length, 2);
   at[2] = type;
   memcpy(at + 3, &timestamp, 4);
   memcpy(at + DLA_FRAME_BYTES, data, length);
   l->used += record;
   l->count++;
   l->dirty = true;
   return 0;
}

int dla_flush(dla_t *l)
{
   if (l->dirty && (write_current(l) < 0))
      return -1;
   dhara_error_t err;
   return dhara_map_sync(&l->map, &err);
}

int dla_read_sector(dla_t *l, dhara_sector_t s, uint8_t *buf, dla_sector_header_t *header)
{
   dhara_error_t err;
   if (dhara_map_read(&l->map, s, buf, &err) < 0)
      return -1;
   memcpy(header, buf, sizeof(*header));
   if (!header_valid(header, buf + sizeof(*header), l->payload_max, true))
      return -1;
   return header->payload_length;
}
