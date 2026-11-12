// Helpers shared by the layers built over Dhara in this harness. See dhara_nand_nandlog.c
#ifndef DHARA_GLUE_H
#define DHARA_GLUE_H

#include <stdint.h>
#include "map.h"

// The part's datasheet guarantees at least 4016 valid blocks of 4096, so at most 80 bad blocks
#define DHARA_GLUE_ASSUMED_BAD_BLOCKS   80u

// Dhara's capacity in sectors with 'assumed_bad_blocks' bad blocks: a fixed number a layer above can lay out against
uint32_t dhara_fixed_capacity(const struct dhara_map *map, uint32_t assumed_bad_blocks);

#ifdef DHARA_GLUE_COUNTERS
extern uint32_t dhara_glue_progs, dhara_glue_copies, dla_sector_writes, lfs_bd_dhara_syncs;
#endif

#endif
