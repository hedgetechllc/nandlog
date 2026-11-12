// littlefs block devices for the baselines, and a minimal record logger over one littlefs file. Harness code.
//
// lfs_bd_dhara_*: littlefs over Dhara. One littlefs block = one Dhara sector = one NAND page (4 KiB), so
//                 read_size = prog_size = block_size = 4096. prog -> dhara_map_write(); erase -> nothing (an FTL
//                 sector is rewritable; trimming would cost Dhara a page program per erase); sync -> dhara_map_sync().
//                 block_count is fixed at Dhara's capacity with 80 bad blocks assumed (see lfs_bd_dhara.c). The
//                 configuration the comparison uses is filled in by lfs_bd_dhara_config().
//
// lfs_bd_raw_*:   littlefs directly on the NAND ("littlefs alone"). One littlefs block = one 256 KiB erase block,
//                 read_size = prog_size = 4096 (a page). A block the driver's table already lists as bad answers
//                 its erase with LFS_ERR_CORRUPT, so littlefs allocates another; littlefs itself keeps no
//                 persistent bad-block list.
//
// lfa_*:          the application layer both littlefs stacks use: one file opened O_APPEND, each record written
//                 with nandlog's framing ([length:2][type:1][timestamp:4][data]) as a byte stream, and
//                 lfs_file_sync() as the flush. A record is durable once lfs_file_sync() returns after it.
#ifndef LFS_BD_H
#define LFS_BD_H

#include <stdbool.h>
#include <stdint.h>
#include "lfs.h"
#include "map.h"

void lfs_bd_dhara_config(struct lfs_config *cfg, struct dhara_map *map, void *read_buf, void *prog_buf,
                         void *lookahead_buf, uint32_t lookahead_size);
void lfs_bd_raw_config(struct lfs_config *cfg, uint32_t block_count, void *read_buf, void *prog_buf,
                       void *lookahead_buf, uint32_t lookahead_size);

typedef struct
{
   lfs_t lfs;
   lfs_file_t file;
   struct lfs_file_config file_cfg;
} lfa_t;

// Mount (formatting the device if it holds no filesystem and 'allow_format') and open the log file for appending.
// 'file_buf' is cache_size bytes
int lfa_mount(lfa_t *a, const struct lfs_config *cfg, void *file_buf, bool allow_format);
int lfa_append(lfa_t *a, uint8_t type, uint32_t timestamp, const void *data, uint16_t length);
int lfa_flush(lfa_t *a);
int lfa_unmount(lfa_t *a);

#endif
