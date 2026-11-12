// Footprint probe: the calls an append-only logger with a sequential offload makes, so a --gc-sections link keeps
// exactly the code that use needs. Never run; only linked and measured.
#include "nandlog.h"

static uint8_t record[32];
static uint8_t page[NANDLOG_MAX_DATA_BYTES_PER_PAGE];
volatile uint32_t sink;

int main(void)
{
   if (!nandlog_init())
      return 1;
   nandlog_begin_session();
   (void)nandlog_begin_epoch("run", 3);
   nandlog_end_session();
   for (uint32_t i = 0; i < 1000; ++i)
      (void)nandlog_store_record(7, i, record, sizeof(record));
   (void)nandlog_flush(true);

   nandlog_begin_session();
   nandlog_begin_reading(0, 0);
   uint32_t pages = 0;
   nandlog_read_span(&pages, (uint32_t*)0);
   for (uint32_t i = 0; i < pages; ++i)
      sink += nandlog_retrieve_next_page(page, (nandlog_page_header_t*)0);
   nandlog_end_reading();
   nandlog_end_session();
   return 0;
}
