// Footprint probe: littlefs alone, directly on the part through the Alliance driver's page functions. One file open
// for appending plus a read handle for the offload. Buffers at the configuration reproduce.sh states.
#include <string.h>
#include "lfs_bd.h"
#include "nandlog_chip.h"
#include "nandlog_port.h"

#define LOOKAHEAD_BYTES 512                                    // one bit per 256 KiB block: the whole part

static uint8_t read_buf[4096], prog_buf[4096];
static uint8_t lookahead_buf[LOOKAHEAD_BYTES];
static uint8_t file_buf[4096];
static uint8_t rfile_buf[4096];
static struct lfs_config cfg;
static lfa_t app;
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
   lfs_bd_raw_config(&cfg, 4096 - 16, read_buf, prog_buf, lookahead_buf, LOOKAHEAD_BYTES);
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
