// Footprint probe: littlefs over Dhara over the Alliance driver, one file open for appending plus a read handle for
// the offload. Every buffer is declared here, statically, at the configuration reproduce.sh states.
#include <string.h>
#include "lfs_bd.h"
#include "nandlog_chip.h"
#include "nandlog_port.h"

#define LOOKAHEAD_BYTES 256

static const struct dhara_nand nand = { 12, 6, 4096 - 16 };
static uint8_t map_page_buf[4096];                             // Dhara's journal/metadata page buffer
static struct dhara_map map;
static uint8_t read_buf[4096], prog_buf[4096];                 // littlefs read and program caches (cache_size)
static uint8_t lookahead_buf[LOOKAHEAD_BYTES];
static uint8_t file_buf[4096];                                 // the open log file's cache (cache_size)
static uint8_t rfile_buf[4096];                                // the offload's read handle's cache
static struct lfs_config cfg;
static lfa_t app;                                              // lfs_t + lfs_file_t + lfs_file_config
static lfs_file_t rf;
static struct lfs_file_config rcfg;
static uint8_t record[32];
static uint8_t page[4096];
volatile uint32_t sink;

int main(void)
{
   if (!nandlog_port_init() || !nandlog_chip_probe())
      return 1;
   nandlog_chip_init();
   dhara_error_t err;
   dhara_map_init(&map, &nand, map_page_buf, 4);
   (void)dhara_map_resume(&map, &err);
   lfs_bd_dhara_config(&cfg, &map, read_buf, prog_buf, lookahead_buf, LOOKAHEAD_BYTES);
   if (lfa_mount(&app, &cfg, file_buf, true))
      return 1;
   for (uint32_t i = 0; i < 1000; ++i)
      (void)lfa_append(&app, 7, i, record, sizeof(record));
   (void)lfa_flush(&app);

   rcfg.buffer = rfile_buf;
   if (lfs_file_opencfg(&app.lfs, &rf, "log", LFS_O_RDONLY, &rcfg))
      return 1;
   while (lfs_file_read(&app.lfs, &rf, page, sizeof(page)) > 0)
      sink++;
   lfs_file_close(&app.lfs, &rf);
   return 0;
}
