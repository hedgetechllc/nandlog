// littlefs block device over Dhara. See lfs_bd.h. Baseline harness code.
#include <string.h>
#include "lfs_bd.h"
#include "nandlog_chip.h"
#include "dhara_glue.h"

#define PAGE_BYTES        4096u
#define PAGES_PER_BLOCK   64u

// littlefs over Dhara -------------------------------------------------------------------------------------------------

static int dhara_bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
   // read_size = block_size, so littlefs always asks for whole sectors
   struct dhara_map *map = (struct dhara_map*)c->context;
   dhara_error_t err;
   if ((off != 0) || (size != PAGE_BYTES))
      return LFS_ERR_INVAL;
   return (dhara_map_read(map, block, (uint8_t*)buffer, &err) < 0) ? LFS_ERR_IO : 0;
}

static int dhara_bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
   struct dhara_map *map = (struct dhara_map*)c->context;
   dhara_error_t err;
   if ((off != 0) || (size != PAGE_BYTES))
      return LFS_ERR_INVAL;
   return (dhara_map_write(map, block, (const uint8_t*)buffer, &err) < 0) ? LFS_ERR_IO : 0;
}

static int dhara_bd_erase(const struct lfs_config *c, lfs_block_t block)
{
   (void)c;
   (void)block;
   return 0;
}

#ifdef DHARA_GLUE_COUNTERS
uint32_t lfs_bd_dhara_syncs;      // harness-only instrumentation
#endif

static int dhara_bd_sync(const struct lfs_config *c)
{
   struct dhara_map *map = (struct dhara_map*)c->context;
#ifdef DHARA_GLUE_COUNTERS
   ++lfs_bd_dhara_syncs;
#endif
   dhara_error_t err;
   return (dhara_map_sync(map, &err) < 0) ? LFS_ERR_IO : 0;
}

void lfs_bd_dhara_config(struct lfs_config *cfg, struct dhara_map *map, void *read_buf, void *prog_buf,
                         void *lookahead_buf, uint32_t lookahead_size)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->context = map;
   cfg->read = dhara_bd_read;
   cfg->prog = dhara_bd_prog;
   cfg->erase = dhara_bd_erase;
   cfg->sync = dhara_bd_sync;
   cfg->read_size = PAGE_BYTES;
   cfg->prog_size = PAGE_BYTES;
   cfg->block_size = PAGE_BYTES;
   cfg->block_count = dhara_fixed_capacity(map, DHARA_GLUE_ASSUMED_BAD_BLOCKS);   // never moves; see dhara_glue.h
   cfg->block_cycles = -1;              // the FTL levels wear; littlefs's own metadata relocation is off
   cfg->cache_size = PAGE_BYTES;
   cfg->lookahead_size = lookahead_size;
   cfg->read_buffer = read_buf;
   cfg->prog_buffer = prog_buf;
   cfg->lookahead_buffer = lookahead_buf;
}
