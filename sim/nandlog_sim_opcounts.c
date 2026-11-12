// Flash operations and bus traffic per call, run against the RAM-backed port over the full-size Alliance part.
// `make opcounts` builds and runs it, with the part's internal page copy and, for the relocations, without it.
//
// Each line is one call, or one boot, measured from the simulator's counters: page latches (raw ones, with the
// part's ECC off, in brackets), programs, erases, and the bytes that crossed the SPI bus each way. The scenarios are
// the ones the white paper's Table 3 reports, so its numbers can be regenerated from the repository.
//
// Usage: nandlog_sim_opcounts               every line
//        nandlog_sim_opcounts relocation    only the lines a failing block drives
//        nandlog_sim_opcounts boards        the boots of parts shaped like the hardware benchmark's boards, each
//                                           boot the benchmark times also in the terms of its BENCH_COUNT lines

#include <stdio.h>
#include <string.h>
#include "nandlog.h"
#include "nandlog_chip.h"
#include "nandlog_port.h"
#include "nandlog_port_sim.h"

static const uint8_t DEVICE_ID[3] = { 0x8D, 0x00, 0x00 };
static uint8_t record[NANDLOG_MAX_PAGE_SIZE_BYTES], buffer[NANDLOG_MAX_PAGE_SIZE_BYTES];

static uint32_t ppb(void)            { return nandlog_chip_geometry()->pages_per_block; }
static uint32_t page_record(void)    { return nandlog_data_bytes_per_page() - (NANDLOG_RECORD_FRAMING ? NANDLOG_FRAMING_LENGTH_BYTES : 0) - 5; }

static void store_page(uint32_t id)
{
   // One record that fills a page, so each store after the first commits the one before
   memset(record, (int)id, page_record());
   nandlog_store_record(7, 1000 + id, record, page_record());
}

static void pages(uint32_t count, uint32_t first_id)
{
   for (uint32_t i = 0; i < count; ++i)
      store_page(first_id + i);
   nandlog_flush(true);
}

static nandlog_sim_counters_t last_shown;     // the counters show() last printed, before it reset them

static void show(const char *what)
{
   const nandlog_sim_counters_t c = nandlog_sim_counters();
   last_shown = c;
   printf("%-64s latches %6u (raw %4u) | programs %4u | erases %3u | bus read %8u B, write %7u B | reads %u/%u/%u/%u\n", what,
          c.page_reads, c.raw_page_reads, c.page_writes, c.block_erases, c.spi_read_bytes, c.spi_write_bytes, c.reads_header,
          c.reads_partial, c.reads_page, c.reads_spare);
   nandlog_sim_reset_counters();
}

static void begin_epoch(void)
{
   nandlog_begin_session();
   nandlog_begin_epoch("ops", 4);
   nandlog_end_session();
}

static void boot(const char *what)
{
   nandlog_deinit();
   nandlog_sim_reset_counters();
   nandlog_init();
   show(what);
}

static void fail_blocks(const uint32_t *blocks, uint32_t count, const uint32_t *pages_refused, uint32_t page_count)
{
   nandlog_sim_faults_t faults;
   memset(&faults, 0, sizeof(faults));
   memcpy(faults.device_id, DEVICE_ID, sizeof(DEVICE_ID));
   for (uint32_t i = 0; i < count; ++i)
      faults.unwritable_blocks[faults.num_unwritable_blocks++] = blocks[i];
   for (uint32_t i = 0; i < page_count; ++i)
      faults.unwritable_pages[faults.num_unwritable_pages++] = pages_refused[i];
   nandlog_sim_set_faults(&faults);
}

static uint32_t head_block(void)
{
   // The block the write head is in: the epoch's first block, stepped forward over its good blocks. Opening a read
   // commits what is buffered, so this is asked before a record is stored
   nandlog_epoch_info_t info;
   uint32_t head = 0;
   nandlog_begin_session();
   nandlog_epoch_info(0, &info);
   nandlog_begin_reading(0, 0);
   nandlog_read_span(&head, NULL);
   nandlog_end_reading();
   nandlog_end_session();
   uint32_t block = info.first_page / ppb();
   for (;;)
   {
      if (nandlog_chip_is_bad_block(block * ppb()))
         ++block;
      else if (head >= ppb())
      {
         head -= ppb();
         ++block;
      }
      else
         return block;
   }
}


// Scenarios -----------------------------------------------------------------------------------------------------------

static void steady_state(void)
{
   // Boots, commits, the erase-ahead, reads and the ring, on a part with seven factory-bad blocks
   static const uint32_t factory_bad[7] = { 3, 700, 1500, 2222, 3001, 3500, 4040 };
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   for (uint32_t i = 0; i < 7; ++i)
      nandlog_sim_raw_page(factory_bad[i] * ppb())[nandlog_chip_geometry()->page_size_bytes] = 0;
   nandlog_sim_reset_counters();
   nandlog_init();
   show("first boot, factory-fresh, 7 factory-bad blocks");
   boot("warm boot, no epoch yet");
   begin_epoch();
   show("begin_epoch (first)");
   pages(100, 0);
   boot("warm boot, 1 epoch, 100 pages");
   nandlog_deinit();
   nandlog_port_init();
   nandlog_sim_reset_counters();
   nandlog_chip_init();
   show("  of which the driver's share: its table and the OTP check");
   nandlog_init();

   store_page(500);
   nandlog_sim_reset_counters();
   store_page(501);
   show("store_record that commits a page");
   nandlog_flush(true);
   nandlog_sim_reset_counters();
   nandlog_store_record(7, 1, record, 8);
   show("store_record that commits nothing (no erase due)");
   nandlog_sim_reset_counters();
   nandlog_flush(true);
   show("flush(true) committing a partial page");

   // One commit and one idle call at a time, to see which calls carry the erase-ahead
   uint32_t committing_calls_that_erased = 0;
   bool shown = false;
   for (uint32_t i = 0; i < 300; ++i)
   {
      nandlog_sim_reset_counters();
      store_page(600 + i);
      nandlog_flush(true);
      committing_calls_that_erased += nandlog_sim_counters().block_erases ? 1 : 0;
      nandlog_sim_reset_counters();
      nandlog_flush(false);
      if (nandlog_sim_counters().block_erases && !shown)
      {
         show("flush(false) carrying the erase-ahead (one block)");
         shown = true;
      }
   }
   printf("committing calls that also erased: %u of 300\n", committing_calls_that_erased);

   nandlog_begin_session();
   nandlog_sim_reset_counters();
   nandlog_begin_reading(1200, 1250);
   show("begin_reading with both bounds, epoch of ~400 pages");
   uint32_t selected = 0, all = 0;
   nandlog_read_span(&selected, NULL);
   nandlog_end_reading();
   nandlog_begin_reading(0, 0);
   nandlog_read_span(&all, NULL);
   nandlog_end_reading();
   printf("  the bounded read selected %u pages of %u\n", selected, all);
   nandlog_sim_reset_counters();
   nandlog_begin_reading(0, 0);
   show("begin_reading unbounded");
   nandlog_page_header_t header;
   nandlog_retrieve_page_by_seq(150, buffer, &header);
   show("retrieve_page_by_seq");
   nandlog_retrieve_next_page(buffer, &header);
   show("retrieve_next_page");
   nandlog_end_reading();
   nandlog_store_record(7, 2000, record, 8);
   nandlog_sim_reset_counters();
   nandlog_begin_reading(0, 0);
   show("begin_reading with records buffered (commits them first)");
   nandlog_end_reading();
   nandlog_end_session();

   for (uint32_t e = 0; e < 99; ++e)
   {
      begin_epoch();
      pages(1, e);
   }
   boot("warm boot, 101 epochs in the ring");
   for (uint32_t e = 0; e < 420; ++e)
   {
      begin_epoch();
      pages(1, e);
   }
   boot("warm boot, 521 epochs (ring wrapped)");
   nandlog_begin_session();
   nandlog_sim_reset_counters();
   nandlog_epoch_count();
   show("epoch_count (16 listed)");
   nandlog_end_session();
   begin_epoch();
   pages(20000, 0);
   boot("warm boot, 20,000-page epoch");
   pages(200000, 0);
   boot("warm boot, 220,000-page epoch");
   nandlog_sim_reset_counters();
   begin_epoch();
   show("begin_epoch (steady state)");
   nandlog_deinit();
   nandlog_sim_destroy();
}

static void torn_page(void)
{
   // The boot after a cut tore a commit: it passes the page over, and lists it in the ring before anything else
   jmp_buf stop;
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_init();
   begin_epoch();
   pages(100, 0);
   store_page(100);
   nandlog_sim_seed(7);
   nandlog_sim_power_cut(1, NANDLOG_SIM_CUT_BITWISE, 0.5, &stop);
   if (setjmp(stop) == 0)
      nandlog_flush(true);
   nandlog_sim_power_cut_disarm();
   boot("warm boot after a cut tore a commit (one position passed over)");
   boot("the warm boot after that");
   nandlog_deinit();
   nandlog_sim_destroy();
}

static void earlier_epochs(void)
{
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_init();
   for (uint32_t e = 0; e < 20; ++e)
   {
      begin_epoch();
      pages(100, 0);
   }
   nandlog_deinit();
   nandlog_init();
   nandlog_begin_session();
   nandlog_sim_reset_counters();
   nandlog_select_epoch(19);
   show("select_epoch (the previous epoch, 100 pages)");
   nandlog_page_header_t header;
   nandlog_retrieve_page_by_seq(50, buffer, &header);
   show("retrieve_page_by_seq, an earlier epoch");
   nandlog_select_current_epoch();
   nandlog_sim_reset_counters();
   nandlog_select_epoch(10);
   show("select_epoch (ten epochs back)");
   nandlog_end_session();
   nandlog_deinit();
   nandlog_sim_destroy();
}

static void relocations(void)
{
   // A failing block: relocated with 40-odd pages in it, then at its last page, then the worst the placement bound
   // allows, and a commit no block will take
   char what[96];
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_init();
   begin_epoch();
   for (uint32_t i = 0; i < (2 * ppb()) + 40; ++i)
   {
      store_page(i);
      nandlog_flush(true);
      nandlog_flush(false);
   }
   uint32_t block = head_block();
   store_page(5000);
   fail_blocks(&block, 1, NULL, 0);
   nandlog_sim_reset_counters();
   nandlog_flush(true);
   snprintf(what, sizeof(what), "commit that relocates 40 pages (chip copy %s) and retires a block", NANDLOG_CHIP_PAGE_COPY ? "on" : "off");
   show(what);
   fail_blocks(NULL, 0, NULL, 0);
   boot("warm boot, an epoch with a block retired in it");
   nandlog_deinit();
   nandlog_sim_destroy();

   // The head's block fails at its last page
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_init();
   begin_epoch();
   for (uint32_t i = 0; i < (ppb() + ppb() - 1); ++i)
   {
      store_page(i);
      nandlog_flush(true);
      nandlog_flush(false);
   }
   block = head_block();
   store_page(999);
   fail_blocks(&block, 1, NULL, 0);
   nandlog_sim_reset_counters();
   nandlog_flush(true);
   snprintf(what, sizeof(what), "block fails at its last page: %u pages to move (chip copy %s)", ppb() - 1, NANDLOG_CHIP_PAGE_COPY ? "on" : "off");
   show(what);

   // ...and each of the two targets the bound allows takes every copy but the last
   nandlog_deinit();
   nandlog_sim_destroy();
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_init();
   begin_epoch();
   for (uint32_t i = 0; i < (ppb() + ppb() - 1); ++i)
   {
      store_page(i);
      nandlog_flush(true);
      nandlog_flush(false);
   }
   block = head_block();
   store_page(999);
   const uint32_t refused[2] = { ((block + 1) * ppb()) + ppb() - 2, ((block + 2) * ppb()) + ppb() - 2 };
   fail_blocks(&block, 1, refused, 2);
   nandlog_sim_reset_counters();
   const nandlog_status_t status = nandlog_flush(true);
   snprintf(what, sizeof(what), "worst placement: both targets refuse their last copy (status %d)", (int)status);
   show(what);

   // ...and a commit no block will take
   nandlog_deinit();
   nandlog_sim_destroy();
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   nandlog_init();
   begin_epoch();
   for (uint32_t i = 0; i < 40; ++i)
   {
      store_page(i);
      nandlog_flush(true);
      nandlog_flush(false);
   }
   block = head_block();
   store_page(999);
   const uint32_t three[3] = { block, block + 1, block + 2 };
   fail_blocks(three, 3, NULL, 0);
   nandlog_sim_reset_counters();
   const nandlog_status_t refused_status = nandlog_flush(true);
   snprintf(what, sizeof(what), "no block takes the page: %u attempts (status %d)", (uint32_t)NANDLOG_PAGE_PLACEMENT_ATTEMPTS, (int)refused_status);
   show(what);
   nandlog_deinit();
   nandlog_sim_destroy();
}

static void dead_reserve(bool table_room)
{
   // The board's state: the reserve and the region's top eighteen blocks (4062-4095) dead, read as 00h and refusing
   // every program and erase, beside the seven factory-bad blocks. With 'table_room' the first boot moves the table to
   // the highest pair below them, and every boot after finds it there; without, every block the table's search reaches
   // is dead too (4032-4095), so the marks stand in for the table and every boot rebuilds it from them
   static const uint32_t factory_bad[7] = { 3, 700, 1500, 2222, 3001, 3500, 4040 };
   const uint32_t blocks = nandlog_chip_geometry()->block_count, first_dead = table_room ? (blocks - 34) : (blocks - 64);
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   for (uint32_t i = 0; i < 7; ++i)
      if (factory_bad[i] < first_dead)
         nandlog_sim_raw_page(factory_bad[i] * ppb())[nandlog_chip_geometry()->page_size_bytes] = 0;
   for (uint32_t b = first_dead; b < blocks; ++b)
      nandlog_sim_kill_block(b);
   printf("%s\n", table_room ? "dead reserve, and the region's top 18 blocks dead: the table moved below them" :
                               "dead reserve, and every block the table's search reaches dead: the marks stand in for the table");
   nandlog_sim_reset_counters();
   nandlog_init();
   show(table_room ? "  first boot: the marks, then the table moved to a pair" : "  first boot: the marks");
   boot("  warm boot, no epoch yet");
   begin_epoch();
   pages(100, 0);
   boot("  warm boot, 1 epoch, 100 pages");
   nandlog_deinit();
   nandlog_port_init();
   nandlog_sim_reset_counters();
   nandlog_chip_init();
   show("    of which the driver's share");
   nandlog_init();

   // A block failing under the head, twelve pages into a new epoch's first block: the relocation, and a retirement by a
   // version below the reserve or by a mark
   begin_epoch();
   pages(10, 1000);
   const uint32_t failing = head_block();
   uint32_t refused[NANDLOG_SIM_MAX_FAULT_PAGES], count = 0;
   for (uint32_t offset = 12; offset < ppb(); ++offset)
      refused[count++] = (failing * ppb()) + offset;
   fail_blocks(NULL, 0, refused, count);
   for (uint32_t i = 0; i < 3; ++i)
      store_page(2000 + i);
   nandlog_sim_reset_counters();
   nandlog_flush(true);
   show(table_room ? "  a block failing at page 12: relocation, a version below the reserve" : "  a block failing at page 12: relocation, retired by its mark");
   fail_blocks(NULL, 0, NULL, 0);
   boot("  warm boot after it");

   // Then a block failing under the head that refuses every program, its mark included. With the table below the
   // reserve, its pair is made to refuse too -- its block's next page and the erase of its other block -- so the marks
   // take over, refuse the block, and the table is given a new pair in the same call; with nowhere for the table, the
   // log stops, as it always has
   begin_epoch();
   pages(10, 3000);
   const uint32_t refusing = head_block();
   uint32_t pair = 0xFFFFFFFF, home = 0, next = 0;
   for (uint32_t b = nandlog_chip_geometry()->block_count - 64; table_room && (b < (nandlog_chip_geometry()->block_count - 34)); ++b)
      if (nandlog_chip_is_bad_block(b * ppb()) && !memcmp(nandlog_sim_raw_page(b * ppb()), "BBT3", 4))
         pair = b & ~1u;
   if (pair != 0xFFFFFFFF)
   {
      uint32_t ends[2];
      for (uint32_t k = 0; k < 2; ++k)
         for (ends[k] = (pair + k) * ppb(); (ends[k] < (((pair + k + 1) * ppb()) - 1)) && (nandlog_sim_raw_page(ends[k])[0] != 0xFF); ++ends[k])
            ;
      home = ((ends[1] % ppb()) > (ends[0] % ppb())) ? (pair + 1) : pair;
      next = ends[home - pair];
   }
   nandlog_sim_faults_t faults;
   memset(&faults, 0, sizeof(faults));
   memcpy(faults.device_id, DEVICE_ID, sizeof(DEVICE_ID));
   faults.unwritable_blocks[faults.num_unwritable_blocks++] = refusing;
   if (pair != 0xFFFFFFFF)
   {
      faults.unwritable_pages[faults.num_unwritable_pages++] = next;
      faults.unerasable_blocks[faults.num_unerasable_blocks++] = home ^ 1u;
   }
   nandlog_sim_set_faults(&faults);
   store_page(4000);
   nandlog_sim_reset_counters();
   const nandlog_status_t status = nandlog_store_record(7, 4001, record, page_record());
   show(table_room ? "  a block refusing even its mark: the store, a new pair mid-call" :
                     "  a block refusing even its mark: the store, no pair, a stop");
   printf("    (the store returned %s)\n", (status == NANDLOG_OK) ? "NANDLOG_OK" :
                                          (status == NANDLOG_ERR_UNWRITABLE) ? "NANDLOG_ERR_UNWRITABLE" : "another status");
   nandlog_flush(true);
   fail_blocks(NULL, 0, NULL, 0);
   boot("  warm boot after it");
   nandlog_deinit();
   nandlog_sim_destroy();
}

static void wipe_region(void)
{
   // The benchmark's wipe: every block below the reserve erased through the driver, retired ones skipped
   nandlog_deinit();
   nandlog_port_init();
   const nandlog_geometry_t *g = nandlog_chip_geometry();
   for (uint32_t b = 0; b < (g->block_count - g->reserved_blocks); ++b)
      if (!nandlog_chip_is_bad_block(b * ppb()))
         nandlog_chip_erase_block(b * ppb());
}

static void bench_count(const char *name)
{
   // The boot just shown, in the terms of the benchmark's BENCH_COUNT line of the same name, from the counters show()
   // keeps: latches, raw ones, programs and erases as counted. bytes_in, the bytes READ (03h) clocks out of the cache
   // register, is every byte read less one for each other read transfer, each a one-byte status-register or device-ID
   // read on this driver; reg_writes, its SET FEATURE commands, is every write transfer less each latch's PAGE DATA
   // READ and each erase's WRITE ENABLE and BLOCK ERASE, which holds for a call that programs nothing. The bench's
   // polls and times depend on the part's busy times, which the simulator does not model
   const nandlog_sim_counters_t c = last_shown;
   const uint32_t cache_reads = c.reads_header + c.reads_partial + c.reads_page + c.reads_spare;
   printf("    as BENCH_COUNT %s: reads=%u raw_reads=%u programs=%u erases=%u", name, c.page_reads, c.raw_page_reads,
          c.page_writes, c.block_erases);
   if (c.page_writes)
      printf(" reg_writes=-");
   else
      printf(" reg_writes=%u", c.spi_writes - c.page_reads - (2 * c.block_erases));
   printf(" bytes_in=%u\n", c.spi_read_bytes - (c.spi_reads - cache_reads));
}

static void board_shape(const char *what, uint32_t first_dead)
{
   // A part shaped like one of the benchmark's boards, from block 'first_dead' up dead -- 00h in every byte, main and
   // spare, refusing every program and erase -- taken through tests/test_nandlog_bench.c's sequence: three boots, the
   // wipe (phase C), a boot with no epoch, epochs begun with a boot at 1, 64, 511 and 600 of them (phase D), then
   // the 600th epoch filled to 1,000 and to 10,000 pages (phase E). Phase E begins no epoch: it fills the one phase D
   // left. One begun here would number the ring's next slot one higher, and the write-head search rotates its first
   // probes by that numbering, so the boot would read other pages of the epoch than the board's did. Each boot the
   // benchmark counts is also shown in its own terms (bench_count())
   printf("\n%s\n", what);
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   const uint32_t blocks = nandlog_chip_geometry()->block_count;
   for (uint32_t b = first_dead; b < blocks; ++b)
      nandlog_sim_kill_block(b);
   nandlog_sim_reset_counters();
   nandlog_init();
   show("  first boot");
   boot("  second boot");
   boot("  third boot");
   wipe_region();
   boot("  boot with no epoch, after the wipe");
   bench_count("init.no_epoch");
   uint32_t begun = 0;
   static const uint32_t rings[4] = { 1, 64, 511, 600 };
   for (uint32_t r = 0; r < 4; ++r)
   {
      nandlog_init();
      for (; begun < rings[r]; ++begun)
         begin_epoch();
      char name[64];
      snprintf(name, sizeof(name), "  boot, %u epochs begun, the last empty", rings[r]);
      boot(name);
      snprintf(name, sizeof(name), "init.ring%u.len0", rings[r]);
      bench_count(name);
   }
   nandlog_deinit();
   nandlog_port_init();
   nandlog_sim_reset_counters();
   nandlog_chip_init();
   show("    of which the driver's initialization");
   bench_count("init.part.chip_init");
   nandlog_init();
   pages(1000, 0);
   boot("  boot, an epoch of 1,000 pages");
   bench_count("init.ring600.len1000");
   pages(9000, 2000);
   boot("  boot, an epoch of 10,000 pages");
   bench_count("init.ring600.len10000");
   nandlog_deinit();
   nandlog_sim_destroy();
}

static void board_shapes(void)
{
   // The white paper's boot table. Board B's top 34 blocks were found dead: page 0 read 00h throughout, main and spare,
   // with a clean ECC verdict, and every program and erase was refused. Boards C and A carry the same top-down run of
   // marked blocks, 4085-4095 and 4087-4095, and are shaped the same way. No dump of their pages was taken. What the
   // shape decides is how the driver's table search reads the run's table candidates (4085-4087 on C, 4087 on A): one
   // read of a candidate whose first page is erased, two of one whose first two pages read programmed and not a
   // table. Marked but otherwise erased, as before, C's boots made three latches fewer than the board's, and A's one;
   // read as 00h, both match. A warm boot never writes those blocks, so whether they refuse does not matter here.
   // None of the three boards' table histories is reproduced
   board_shape("board B: blocks 4062-4095 refusing every program and erase, nothing marked", 4062);
   board_shape("board C: blocks 4085-4095 reading 00h throughout, as board B's dead blocks do", 4085);
   board_shape("board A: blocks 4087-4095 reading 00h throughout, as board B's dead blocks do", 4087);
}

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IOLBF, 0);
   const bool only_relocation = (argc > 1) && !strcmp(argv[1], "relocation");
   const bool only_boards = (argc > 1) && !strcmp(argv[1], "boards");
   printf("operation counts: %s, %u-byte pages, %u pages a block, %u blocks, record framing %s, chip copy %s\n",
          NANDLOG_RECORD_FRAMING ? "framed" : "plain", nandlog_chip_geometry()->page_size_bytes, ppb(), nandlog_chip_geometry()->block_count,
          NANDLOG_RECORD_FRAMING ? "on" : "off", NANDLOG_CHIP_PAGE_COPY ? "on" : "off");
   printf("reads a/b/c/d: transfers out of the part's cache register of at most 64 bytes (a header, a mark), of more but\n"
          "less than a page, of a whole page, and of a page with its spare area (a raw read with ECC off)\n");
   if (only_boards)
   {
      board_shapes();
      return 0;
   }
   if (!only_relocation)
   {
      steady_state();
      torn_page();
      earlier_epochs();
   }
   relocations();
   if (!only_relocation)
   {
      dead_reserve(true);
      dead_reserve(false);
   }
   return 0;
}
