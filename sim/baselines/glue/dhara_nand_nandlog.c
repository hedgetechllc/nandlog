// Dhara's NAND interface (dhara/nand.h) implemented over nandlog's page-level chip interface (nandlog_chip.h).
//
// Baseline harness code, not part of nandlog. It lets Dhara run on exactly the driver, bad-block table and
// simulated part that nandlog runs on, so the two are compared over the same flash operations:
//
//   is_bad / mark_bad  -> the Alliance driver's persistent bad-block table (RAM lookup / versioned table update)
//   erase              -> nandlog_chip_erase_block()      one BLOCK ERASE (the driver retries a failed erase)
//   prog               -> nandlog_chip_write_page()       one PROGRAM LOAD + PROGRAM EXECUTE, no read-back
//   is_free            -> nandlog_chip_page_is_blank()    one page latch with ECC off, main + spare compared to FFh
//   read               -> nandlog_chip_read_page_region() one page latch, 'length' bytes clocked out
//   copy               -> nandlog_chip_copy_page()        one page latch + one PROGRAM EXECUTE (internal data move)
//
// Dhara's blocks 0 .. num_blocks-1 are the part's physical blocks 0 .. num_blocks-1; the caller sets num_blocks to
// stop short of the driver's reserved blocks, so Dhara gets exactly the blocks nandlog's metadata ring and log
// region occupy.
//
// Caveat (see reproduce.sh): the Alliance driver's internal copy does not report the source page's ECC
// verdict, so this copy cannot return DHARA_E_ECC for an uncorrectable source. A production glue would read the
// status after the latch; the operation count is the same.

#include "map.h"
#include "nand.h"
#include "nandlog_chip.h"
#include "dhara_glue.h"

#ifdef DHARA_GLUE_COUNTERS
uint32_t dhara_glue_progs, dhara_glue_copies;      // harness-only instrumentation
#define COUNT(x) (++(x))
#else
#define COUNT(x) ((void)0)
#endif

uint32_t dhara_fixed_capacity(const struct dhara_map *map, uint32_t assumed_bad_blocks)
{
   // dhara_map_capacity() moves with Dhara's running bad-block estimate: it starts at num_blocks/64 and is replaced by
   // the real count each time the journal wraps the chip. A layer above that lays anything out by sector number --
   // littlefs records block_count in its superblock, DLA wraps its sector ring at the capacity -- needs a number that
   // never moves, so it uses the capacity Dhara would report with 'assumed_bad_blocks' bad blocks
   const struct dhara_nand *n = map->journal.nand;
   const uint32_t good_blocks = n->num_blocks - assumed_bad_blocks - 1u;
   const uint32_t good_cps = good_blocks << (n->log2_ppb - map->journal.log2_ppc);
   const uint32_t cap = (good_cps << map->journal.log2_ppc) - good_cps;
   return cap - (cap / (map->gc_ratio + 1u)) - ((uint32_t)DHARA_MAX_RETRIES << n->log2_ppb);
}

int dhara_nand_is_bad(const struct dhara_nand *n, dhara_block_t b)
{
   return nandlog_chip_is_bad_block((uint32_t)b << n->log2_ppb) ? 1 : 0;
}

void dhara_nand_mark_bad(const struct dhara_nand *n, dhara_block_t b)
{
   (void)nandlog_chip_mark_bad_block((uint32_t)b << n->log2_ppb);
}

int dhara_nand_erase(const struct dhara_nand *n, dhara_block_t b, dhara_error_t *err)
{
   if (nandlog_chip_erase_block((uint32_t)b << n->log2_ppb))
      return 0;
   dhara_set_error(err, DHARA_E_BAD_BLOCK);
   return -1;
}

int dhara_nand_prog(const struct dhara_nand *n, dhara_page_t p, const uint8_t *data, dhara_error_t *err)
{
   (void)n;
   COUNT(dhara_glue_progs);
   if (nandlog_chip_write_page(data, p))
      return 0;
   dhara_set_error(err, DHARA_E_BAD_BLOCK);
   return -1;
}

int dhara_nand_is_free(const struct dhara_nand *n, dhara_page_t p)
{
   (void)n;
   return nandlog_chip_page_is_blank(p) ? 1 : 0;
}

int dhara_nand_read(const struct dhara_nand *n, dhara_page_t p, size_t offset, size_t length, uint8_t *data, dhara_error_t *err)
{
   (void)n;
   if (nandlog_chip_read_page_region(data, p, (uint32_t)offset, (uint32_t)length) != NANDLOG_READ_UNCORRECTABLE)
      return 0;
   dhara_set_error(err, DHARA_E_ECC);
   return -1;
}

int dhara_nand_copy(const struct dhara_nand *n, dhara_page_t src, dhara_page_t dst, dhara_error_t *err)
{
   (void)n;
   COUNT(dhara_glue_copies);
   switch (nandlog_chip_copy_page(src, dst))
   {
      case NANDLOG_COPY_OK:
         return 0;
      case NANDLOG_COPY_FAILED:
         dhara_set_error(err, DHARA_E_BAD_BLOCK);
         return -1;
      default:
         // Every part this harness targets has an internal data move; a part without one needs a page buffer here
         dhara_set_error(err, DHARA_E_BAD_BLOCK);
         return -1;
   }
}
