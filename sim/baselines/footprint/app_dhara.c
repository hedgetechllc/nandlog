// Footprint probe: Dhara + the DLA append layer + the Alliance driver, used as an append-only logger with a
// sequential offload. Every buffer the stack needs is declared here, statically, at its run-time size.
#include "dla.h"
#include "nandlog_chip.h"
#include "nandlog_port.h"

static const struct dhara_nand nand = { 12, 6, 4096 - 16 };   // 4 KiB pages, 64 pages/block, below the driver's reserve
static uint8_t map_page_buf[4096];                             // Dhara's journal/metadata page buffer
static uint8_t sector_buf[4096];                               // DLA's sector assembly buffer
static dla_t log_state;                                        // includes struct dhara_map
static uint8_t record[32];
static uint8_t page[4096];                                     // the application's read buffer (as for nandlog)
volatile uint32_t sink;

int main(void)
{
   if (!nandlog_port_init() || !nandlog_chip_probe())
      return 1;
   nandlog_chip_init();
   if (dla_mount(&log_state, &nand, map_page_buf, sector_buf, 4, false) < 0)
      return 1;
   for (uint32_t i = 0; i < 1000; ++i)
      (void)dla_append(&log_state, 7, i, record, sizeof(record));
   (void)dla_flush(&log_state);

   for (dhara_sector_t s = 0; s < dhara_map_size(&log_state.map); ++s)
   {
      dla_sector_header_t h;
      sink += (uint32_t)dla_read_sector(&log_state, s, page, &h);
   }
   return 0;
}
