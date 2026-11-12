// littlefs block device over the raw part ("littlefs alone"). See lfs_bd.h. Baseline harness code.
#include <string.h>
#include "lfs_bd.h"
#include "nandlog_chip.h"

#define PAGE_BYTES        4096u
#define PAGES_PER_BLOCK   64u

// littlefs on the raw part --------------------------------------------------------------------------------------------

static int raw_bd_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
   (void)c;
   uint8_t *out = (uint8_t*)buffer;
   for (lfs_size_t done = 0; done < size; done += PAGE_BYTES)
      if (nandlog_chip_read_page(out + done, (block * PAGES_PER_BLOCK) + ((off + done) / PAGE_BYTES)) == NANDLOG_READ_UNCORRECTABLE)
         return LFS_ERR_CORRUPT;
   return 0;
}

static int raw_bd_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
   (void)c;
   const uint8_t *in = (const uint8_t*)buffer;
   for (lfs_size_t done = 0; done < size; done += PAGE_BYTES)
      if (!nandlog_chip_write_page(in + done, (block * PAGES_PER_BLOCK) + ((off + done) / PAGE_BYTES)))
         return LFS_ERR_CORRUPT;
   return 0;
}

static int raw_bd_erase(const struct lfs_config *c, lfs_block_t block)
{
   (void)c;
   const uint32_t page = block * PAGES_PER_BLOCK;
   if (nandlog_chip_is_bad_block(page) || !nandlog_chip_erase_block(page))
      return LFS_ERR_CORRUPT;
   return 0;
}

static int raw_bd_sync(const struct lfs_config *c)
{
   (void)c;
   return 0;
}

void lfs_bd_raw_config(struct lfs_config *cfg, uint32_t block_count, void *read_buf, void *prog_buf,
                       void *lookahead_buf, uint32_t lookahead_size)
{
   memset(cfg, 0, sizeof(*cfg));
   cfg->read = raw_bd_read;
   cfg->prog = raw_bd_prog;
   cfg->erase = raw_bd_erase;
   cfg->sync = raw_bd_sync;
   cfg->read_size = PAGE_BYTES;
   cfg->prog_size = PAGE_BYTES;
   cfg->block_size = PAGE_BYTES * PAGES_PER_BLOCK;
   cfg->block_count = block_count;
   cfg->block_cycles = 500;             // littlefs's suggested range is 100-1000; it levels wear itself here
   cfg->cache_size = PAGE_BYTES;
   cfg->lookahead_size = lookahead_size;
   cfg->read_buffer = read_buf;
   cfg->prog_buffer = prog_buf;
   cfg->lookahead_buffer = lookahead_buf;
}
