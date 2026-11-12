// Flash operations per appended record: nandlog vs Dhara (+ a minimal append layer) vs littlefs over Dhara vs
// littlefs alone, all over the same simulated Alliance AS5F18G04SND (4 KiB pages, 64 pages/block, 4096 blocks)
// and the same chip driver. Baseline harness; nothing here is part of nandlog.
//
//   bench <stack> <record_bytes> <records> <flush_every> [option=value ...]
//
//   stack        nandlog | dhara | lfs-dhara | lfs-raw
//   flush_every  0: never flush (each stack commits on its own; durability differs, see reproduce.sh)
//                N: after every N-th record call the stack's flush: nandlog_flush(true) / dla_flush() (sector write +
//                   dhara_map_sync) / lfs_file_sync()
//   options      gc=<dhara gc_ratio, default 4>  lookahead=<littlefs lookahead bytes>  verify=0|1 (default 1)
//
// Every stack receives the identical record stream: record i has type 7, timestamp 1000 + i, and a payload whose
// first 4 bytes are i and the rest a deterministic function of i. After the stream, the stack is flushed, its RAM
// state is dropped (a power-off after the final flush), it is mounted again (the boot cost), one more record is
// appended and flushed (the first-append cost), and then everything is read back through the stack and checked
// record by record (the read-back cost and a guard against harness bugs).
//
// Counts come from the simulator's command layer: a "page read" is one PAGE DATA READ latch (ECC on or off,
// whatever is clocked out), a "program" one PROGRAM EXECUTE, an "erase" one BLOCK ERASE.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nandlog.h"
#include "nandlog_chip.h"
#include "nandlog_port.h"
#include "nandlog_port_sim.h"
#include "map.h"
#include "dla.h"
#include "dhara_glue.h"
#include "lfs_bd.h"

#define PAGE_BYTES          4096u
#define LOG2_PAGE           12
#define LOG2_PPB            6
#define DHARA_BLOCKS        (4096u - 16u)       // every block below the driver's 16-block reserve, as nandlog uses
#define RECORD_TYPE         7

static const uint8_t DEVICE_ID[3] = { 0x8D, 0x00, 0x00 };

typedef enum { STACK_NANDLOG, STACK_DHARA, STACK_LFS_DHARA, STACK_LFS_RAW } bench_stack_t;
static const char *stack_names[] = { "nandlog", "dhara", "lfs-dhara", "lfs-raw" };

static bench_stack_t stack;
static uint32_t record_bytes, records, flush_every, gc_ratio = 4, lookahead = 0, warmup = 0, epochs_begun = 0;
static uint32_t epoch_first_id = 0;              // the first record of nandlog's newest epoch
static bool verify = true, first_mount = true;   // only the very first mount may format

// Static buffers: what each stack needs at run time. Sized for the 4 KiB part
static uint8_t payload[PAGE_BYTES], readback[PAGE_BYTES + 64];
static uint8_t dhara_page_buf[PAGE_BYTES], dla_sector_buf[PAGE_BYTES];
static uint8_t lfs_read_buf[PAGE_BYTES], lfs_prog_buf[PAGE_BYTES], lfs_file_buf[PAGE_BYTES], lfs_rfile_buf[PAGE_BYTES];
static uint8_t lfs_lookahead_buf[8192];
static const struct dhara_nand dhara_nand_geometry = { LOG2_PAGE, LOG2_PPB, DHARA_BLOCKS };
static dla_t dla;
static struct dhara_map lfs_map;
static struct lfs_config lfs_cfg;
static lfa_t lfa;


// Record stream -------------------------------------------------------------------------------------------------------

static void fill_record(uint8_t *buf, uint32_t length, uint32_t id)
{
   uint64_t x = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)id << 20) ^ length;
   for (uint32_t i = 0; i < length; ++i)
   {
      x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
      buf[i] = (uint8_t)((x * 0x2545F4914F6CDD1DULL) >> 56);
   }
   if (length >= 4)
      memcpy(buf, &id, 4);
}

static inline uint32_t timestamp_of(uint32_t id) { return 1000u + id; }


// Counters ------------------------------------------------------------------------------------------------------------

typedef struct { uint64_t reads, raw_reads, programs, erases, rbytes, wbytes; } ops_t;
typedef struct { uint32_t reads, raw_reads, programs, erases, rbytes, wbytes; } snap_t;

static snap_t snap_now(void)
{
   const nandlog_sim_counters_t c = nandlog_sim_counters();
   snap_t o = { c.page_reads, c.raw_page_reads, c.page_writes, c.block_erases, c.spi_read_bytes, c.spi_write_bytes };
   return o;
}

static ops_t snap_diff(snap_t a, snap_t b)
{
   // The simulator's counters are 32-bit and the byte counters wrap over a long run, so every difference is taken
   // modulo 2^32 over a span far shorter than a wrap, and long totals are sums of such differences
   ops_t d = { (uint32_t)(b.reads - a.reads), (uint32_t)(b.raw_reads - a.raw_reads), (uint32_t)(b.programs - a.programs),
               (uint32_t)(b.erases - a.erases), (uint32_t)(b.rbytes - a.rbytes), (uint32_t)(b.wbytes - a.wbytes) };
   return d;
}

static void ops_add(ops_t *t, ops_t d)
{
   t->reads += d.reads; t->raw_reads += d.raw_reads; t->programs += d.programs;
   t->erases += d.erases; t->rbytes += d.rbytes; t->wbytes += d.wbytes;
}

// The counters since the last reset, for spans short enough not to wrap (boot, first append)
static ops_t ops_now(void)
{
   const snap_t zero = { 0, 0, 0, 0, 0, 0 };
   return snap_diff(zero, snap_now());
}

// Indicative array-busy plus bus time for one call, from the AS5F18G04SND datasheet: tRD 140 us (4 KiB page),
// tPROG 600 us typical, tBERS 3 ms maximum (the parameter page's value), and every byte on a 1-bit SPI bus at
// 24 MHz (0.333 us/byte). Host CPU time is not included. Only for ranking worst cases; the counts are the result
static double est_us(ops_t o)
{
   return (o.reads * 140.0) + (o.programs * 600.0) + (o.erases * 3000.0) + ((o.rbytes + o.wbytes) / 3.0);
}

typedef struct
{
   uint64_t calls, calls_programming, calls_erasing, calls_reading;
   ops_t total, max_programs, max_reads, max_erases, max_time;   // the whole call with the most of each
} call_stats_t;

static void account(call_stats_t *s, ops_t d)
{
   s->calls++;
   s->calls_programming += d.programs ? 1 : 0;
   s->calls_erasing += d.erases ? 1 : 0;
   s->calls_reading += d.reads ? 1 : 0;
   ops_add(&s->total, d);
   if (d.programs > s->max_programs.programs) s->max_programs = d;
   if (d.reads > s->max_reads.reads) s->max_reads = d;
   if (d.erases > s->max_erases.erases) s->max_erases = d;
   if (est_us(d) > est_us(s->max_time)) s->max_time = d;
}

static void die(const char *what)
{
   fprintf(stderr, "FAIL %s %s %u %u %u: %s\n", stack_names[stack], "", record_bytes, records, flush_every, what);
   exit(1);
}


// Driver bring-up (identical for every stack) -------------------------------------------------------------------------

static void driver_up(void)
{
   // What nandlog_init() does before the log itself: open the port, identify the part, load its bad-block table
   if (!nandlog_port_init() || !nandlog_chip_probe())
      die("driver did not come up");
   nandlog_port_power(true);
   nandlog_chip_init();
}


// Stack adapters ------------------------------------------------------------------------------------------------------

static void stack_mount(void)
{
   switch (stack)
   {
      case STACK_NANDLOG:
         if (!nandlog_init())
            die("nandlog_init");
         break;
      case STACK_DHARA:
         driver_up();
         if (dla_mount(&dla, &dhara_nand_geometry, dhara_page_buf, dla_sector_buf, (uint8_t)gc_ratio, false) < 0)
            die("dla_mount");
         break;
      case STACK_LFS_DHARA:
      {
         driver_up();
         dhara_error_t err;
         dhara_map_init(&lfs_map, &dhara_nand_geometry, dhara_page_buf, (uint8_t)gc_ratio);
         (void)dhara_map_resume(&lfs_map, &err);
         lfs_bd_dhara_config(&lfs_cfg, &lfs_map, lfs_read_buf, lfs_prog_buf, lfs_lookahead_buf, lookahead);
         if (lfa_mount(&lfa, &lfs_cfg, lfs_file_buf, first_mount))
            die("lfa_mount (dhara)");
         break;
      }
      case STACK_LFS_RAW:
         driver_up();
         lfs_bd_raw_config(&lfs_cfg, DHARA_BLOCKS, lfs_read_buf, lfs_prog_buf, lfs_lookahead_buf, lookahead);
         if (lfa_mount(&lfa, &lfs_cfg, lfs_file_buf, first_mount))
            die("lfa_mount (raw)");
         break;
   }
}

static void stack_power_off(void)
{
   // Drop RAM state without any further flash traffic, as a power cut after the last flush would
   if (stack == STACK_NANDLOG)
      nandlog_deinit();
   else
      nandlog_port_deinit();
}

static bool stack_append(uint32_t id)
{
   fill_record(payload, record_bytes, id);
   switch (stack)
   {
      case STACK_NANDLOG:
      {
         nandlog_status_t st = nandlog_store_record(RECORD_TYPE, timestamp_of(id), payload, record_bytes);
         if (st == NANDLOG_ERR_FULL)
         {
            // A ring logger's policy once the region is full: begin a new epoch, which reuses the region from its
            // start, and store the record there. Counted as part of this call
            uint8_t details[16] = "baseline";
            nandlog_begin_session();
            const bool begun = nandlog_begin_epoch(details, sizeof(details));
            nandlog_end_session();
            if (!begun)
               return false;
            ++epochs_begun;
            epoch_first_id = id;
            st = nandlog_store_record(RECORD_TYPE, timestamp_of(id), payload, record_bytes);
         }
         return st == NANDLOG_OK;
      }
      case STACK_DHARA:
         return dla_append(&dla, RECORD_TYPE, timestamp_of(id), payload, (uint16_t)record_bytes) == 0;
      default:
         return lfa_append(&lfa, RECORD_TYPE, timestamp_of(id), payload, (uint16_t)record_bytes) == 0;
   }
}

static bool stack_flush(void)
{
   switch (stack)
   {
      case STACK_NANDLOG:
         return nandlog_flush(true) == NANDLOG_OK;
      case STACK_DHARA:
         return dla_flush(&dla) == 0;
      default:
         return lfa_flush(&lfa) == 0;
   }
}


// Read-back verification ----------------------------------------------------------------------------------------------

static uint32_t verified, verify_errors;
static uint8_t expected[PAGE_BYTES];

static void check_record(uint8_t type, uint32_t ts, const uint8_t *data, uint32_t length)
{
   const uint32_t id = verified;
   fill_record(expected, record_bytes, id);
   if ((type != RECORD_TYPE) || (ts != timestamp_of(id)) || (length != record_bytes) || memcmp(data, expected, length))
   {
      if (verify_errors < 5)
         fprintf(stderr, "verify: record %u mismatch (type %u ts %u len %u)\n", id, type, ts, length);
      ++verify_errors;
   }
   ++verified;
}

static void walk_frames(const uint8_t *p, uint32_t n)
{
   for (uint32_t off = 0; (off + 7) <= n; )
   {
      uint16_t length;
      uint32_t ts;
      memcpy(&length, p + off, 2);
      memcpy(&ts, p + off + 3, 4);
      if ((off + 7 + length) > n)
      {
         ++verify_errors;
         return;
      }
      check_record(p[off + 2], ts, p + off + 7, length);
      off += 7u + length;
   }
}

static void read_back(void)
{
   verified = verify_errors = 0;
   switch (stack)
   {
      case STACK_NANDLOG:
      {
         nandlog_begin_session();
         nandlog_begin_reading(0, 0);
         uint32_t pages = 0;
         nandlog_read_span(&pages, NULL);
         for (uint32_t i = 0; i < pages; ++i)
         {
            nandlog_page_header_t h;
            const uint32_t n = nandlog_retrieve_next_page(readback, &h);
            if (!n)
               ++verify_errors;
            walk_frames(readback, n);
         }
         nandlog_end_reading();
         nandlog_end_session();
         break;
      }
      case STACK_DHARA:
      {
         const dhara_sector_t n = dhara_map_size(&dla.map);
         for (dhara_sector_t s = 0; s < n; ++s)
         {
            dla_sector_header_t h;
            const int len = dla_read_sector(&dla, s, readback, &h);
            if (len < 0)
            {
               ++verify_errors;
               continue;
            }
            walk_frames(readback + sizeof(h), (uint32_t)len);
         }
         break;
      }
      default:
      {
         // A second handle, read-only, with its own buffer; the log handle stays open for appending
         lfs_file_t rf;
         struct lfs_file_config rcfg;
         memset(&rcfg, 0, sizeof(rcfg));
         rcfg.buffer = lfs_rfile_buf;
         if (lfs_file_opencfg(&lfa.lfs, &rf, "log", LFS_O_RDONLY, &rcfg))
            die("open for read-back");
         uint8_t frame[7];
         for (;;)
         {
            const lfs_ssize_t got = lfs_file_read(&lfa.lfs, &rf, frame, sizeof(frame));
            if (got == 0)
               break;
            uint16_t length;
            uint32_t ts;
            memcpy(&length, frame, 2);
            memcpy(&ts, frame + 3, 4);
            if ((got != (lfs_ssize_t)sizeof(frame)) || (length > PAGE_BYTES) ||
                (lfs_file_read(&lfa.lfs, &rf, readback, length) != (lfs_ssize_t)length))
            {
               ++verify_errors;
               break;
            }
            check_record(frame[2], ts, readback, length);
         }
         lfs_file_close(&lfa.lfs, &rf);
         break;
      }
   }
}


// Tail verification (steady-state runs) -------------------------------------------------------------------------------
//
// After a wrap the read-back no longer starts at record 0, so a steady-state run instead walks back from the newest
// page/sector and checks that the newest records are all there, contiguous and intact, ending at the last one stored

static uint32_t tail_verified, tail_expect;
static bool tail_ok = true;

static void tail_page(const uint8_t *p, uint32_t n)
{
   static uint32_t ids[PAGE_BYTES / 7];
   uint32_t count = 0;
   for (uint32_t off = 0; (off + 7) <= n; )
   {
      uint16_t length;
      uint32_t ts, id = 0;
      memcpy(&length, p + off, 2);
      memcpy(&ts, p + off + 3, 4);
      if (((off + 7 + length) > n) || (length != record_bytes))
      {
         tail_ok = false;
         return;
      }
      memcpy(&id, p + off + 7, 4);
      fill_record(expected, record_bytes, id);
      if ((p[off + 2] != RECORD_TYPE) || (ts != timestamp_of(id)) || memcmp(p + off + 7, expected, length))
         tail_ok = false;
      ids[count++] = id;
      off += 7u + length;
   }
   for (uint32_t k = count; k--; )
   {
      if (ids[k] != tail_expect)
         tail_ok = false;
      --tail_expect;
      ++tail_verified;
   }
}

static void tail_verify(uint32_t last_id, uint32_t want)
{
   tail_verified = 0;
   tail_expect = last_id;
   if (stack == STACK_NANDLOG)
   {
      // Reads go back no further than the newest epoch, which a wrap may have begun fewer records ago than asked for:
      // then every record of the epoch must be there
      if (want > last_id + 1 - epoch_first_id)
         want = last_id + 1 - epoch_first_id;
      nandlog_begin_session();
      for (uint32_t back = 0; tail_ok && (tail_verified < want); ++back)
      {
         nandlog_page_header_t h;
         bool end = false;
         const uint32_t n = nandlog_read_recent_page(back, readback, &h, &end);
         if (!n)
         {
            tail_ok = false;
            break;
         }
         tail_page(readback, n);
         if (end)
            break;
      }
      if (tail_verified < want)
         tail_ok = false;
      nandlog_end_session();
   }
   else
   {
      dhara_sector_t s = dla.used ? dla.current : ((dla.current + dla.capacity - 1) % dla.capacity);
      for (; tail_ok && (tail_verified < want); s = (s + dla.capacity - 1) % dla.capacity)
      {
         dla_sector_header_t h;
         const int len = dla_read_sector(&dla, s, readback, &h);
         if (len < 0)
         {
            tail_ok = false;
            break;
         }
         tail_page(readback + sizeof(h), (uint32_t)len);
      }
   }
   if (!tail_ok)
      die("tail verification failed");
}


// Output --------------------------------------------------------------------------------------------------------------

static void print_ops(const char *key, ops_t o)
{
   printf(" %s_reads=%llu %s_raw_reads=%llu %s_programs=%llu %s_erases=%llu %s_rbytes=%llu %s_wbytes=%llu %s_est_us=%.0f",
          key, (unsigned long long)o.reads, key, (unsigned long long)o.raw_reads, key, (unsigned long long)o.programs,
          key, (unsigned long long)o.erases, key, (unsigned long long)o.rbytes, key, (unsigned long long)o.wbytes,
          key, est_us(o));
}

static void print_calls(const char *key, const call_stats_t *s)
{
   printf(" %s_calls=%llu %s_calls_programming=%llu %s_calls_erasing=%llu %s_calls_reading=%llu", key,
          (unsigned long long)s->calls, key, (unsigned long long)s->calls_programming, key,
          (unsigned long long)s->calls_erasing, key, (unsigned long long)s->calls_reading);
   char k[64];
   snprintf(k, sizeof(k), "%s_maxprog", key);  print_ops(k, s->max_programs);
   snprintf(k, sizeof(k), "%s_maxread", key);  print_ops(k, s->max_reads);
   snprintf(k, sizeof(k), "%s_maxerase", key); print_ops(k, s->max_erases);
   snprintf(k, sizeof(k), "%s_maxtime", key);  print_ops(k, s->max_time);
}


// Main ----------------------------------------------------------------------------------------------------------------

int main(int argc, char **argv)
{
   if (argc < 5)
   {
      fprintf(stderr, "usage: %s nandlog|dhara|lfs-dhara|lfs-raw <record_bytes> <records> <flush_every> [gc=N] [lookahead=N] [verify=0|1]\n", argv[0]);
      return 2;
   }
   for (stack = STACK_NANDLOG; stack <= STACK_LFS_RAW; ++stack)
      if (!strcmp(argv[1], stack_names[stack]))
         break;
   if (stack > STACK_LFS_RAW)
      return 2;
   record_bytes = (uint32_t)strtoul(argv[2], NULL, 0);
   records = (uint32_t)strtoul(argv[3], NULL, 0);
   flush_every = (uint32_t)strtoul(argv[4], NULL, 0);
   lookahead = (stack == STACK_LFS_RAW) ? 512 : 256;
   for (int i = 5; i < argc; ++i)
   {
      if (!strncmp(argv[i], "gc=", 3)) gc_ratio = (uint32_t)strtoul(argv[i] + 3, NULL, 0);
      else if (!strncmp(argv[i], "lookahead=", 10)) lookahead = (uint32_t)strtoul(argv[i] + 10, NULL, 0);
      else if (!strncmp(argv[i], "verify=", 7)) verify = argv[i][7] != '0';
      else if (!strncmp(argv[i], "warmup=", 7)) warmup = (uint32_t)strtoul(argv[i] + 7, NULL, 0);
   }
   if ((record_bytes < 4) || (record_bytes > 4000) || (lookahead > sizeof(lfs_lookahead_buf)))
      return 2;
   if (warmup && (stack != STACK_NANDLOG) && (stack != STACK_DHARA))
   {
      fprintf(stderr, "warmup (steady-state) runs are defined for nandlog and dhara only\n");
      return 2;
   }
   if (warmup)
      verify = false;      // after the logical space wraps, the read-back no longer starts at record 0

   // A fresh part, and a first boot: the driver builds its bad-block table and each stack formats / begins
   nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
   stack_mount();
   if (stack == STACK_NANDLOG)
   {
      uint8_t details[16] = "baseline";
      nandlog_begin_session();
      if (!nandlog_begin_epoch(details, sizeof(details)))
         die("begin_epoch");
      nandlog_end_session();
   }

   first_mount = false;

   // Steady state: a warm-up stream, uncounted, long enough to wrap the logical space, with the same cadence
   for (uint32_t id = 0; id < warmup; ++id)
   {
      if (!stack_append(id))
         die("warm-up append refused");
      if (flush_every && (((id + 1) % flush_every) == 0) && !stack_flush())
         die("warm-up flush failed");
   }
   const uint32_t warm_epochs = epochs_begun;

   // The stream
   nandlog_sim_reset_counters();
   call_stats_t appends, flushes;
   memset(&appends, 0, sizeof(appends));
   memset(&flushes, 0, sizeof(flushes));
#ifdef DHARA_GLUE_COUNTERS
   const uint32_t progs0 = dhara_glue_progs, copies0 = dhara_glue_copies, writes0 = dla_sector_writes, syncs0 = lfs_bd_dhara_syncs;
#endif
   for (uint32_t id = warmup; id < warmup + records; ++id)
   {
      snap_t a = snap_now();
      if (!stack_append(id))
         die("append refused");
      account(&appends, snap_diff(a, snap_now()));
      if (flush_every && (((id + 1) % flush_every) == 0))
      {
         a = snap_now();
         if (!stack_flush())
            die("flush failed");
         account(&flushes, snap_diff(a, snap_now()));
      }
   }
   snap_t a = snap_now();
   if (!stack_flush())
      die("final flush failed");
   const ops_t final_flush = snap_diff(a, snap_now());
   ops_t stream = appends.total;
   ops_add(&stream, flushes.total);
   ops_add(&stream, final_flush);
#ifdef DHARA_GLUE_COUNTERS
   const uint32_t stream_dhara_progs = dhara_glue_progs - progs0, stream_dhara_copies = dhara_glue_copies - copies0,
                  stream_dla_writes = dla_sector_writes - writes0, stream_lfs_bd_syncs = lfs_bd_dhara_syncs - syncs0;
#endif
   nandlog_stats_t nl_stats;
   memset(&nl_stats, 0, sizeof(nl_stats));
   if (stack == STACK_NANDLOG)
      nandlog_get_stats(&nl_stats);
   uint64_t dhara_sectors = 0, lfs_blocks = 0;
   if (stack == STACK_DHARA)
      dhara_sectors = dhara_map_size(&dla.map);

   // Power off, then a boot: the driver's share alone first, then the whole mount
   stack_power_off();
   nandlog_sim_reset_counters();
   driver_up();
   const ops_t driver_boot = ops_now();
   nandlog_port_deinit();
   nandlog_sim_reset_counters();
   stack_mount();
   const ops_t boot = ops_now();

   // The first record after the boot, and its flush
   nandlog_sim_reset_counters();
   if (!stack_append(warmup + records))
      die("first append after boot refused");
   const ops_t first_append = ops_now();
   nandlog_sim_reset_counters();
   if (!stack_flush())
      die("first flush after boot failed");
   const ops_t first_flush = ops_now();
   if (stack == STACK_LFS_DHARA || stack == STACK_LFS_RAW)
   {
      const lfs_ssize_t used = lfs_fs_size(&lfa.lfs);
      lfs_blocks = (used > 0) ? (uint64_t)used : 0;
   }

   // Read everything back through the stack, and check it
   nandlog_sim_reset_counters();
   if (verify)
      read_back();
   else if (warmup)
      tail_verify(warmup + records, 20000);
   const ops_t readback_ops = ops_now();
   if (verify && ((verified != records + 1) || verify_errors))
   {
      fprintf(stderr, "verify: %u records read back, %u expected, %u errors\n", verified, records + 1, verify_errors);
      die("read-back verification failed");
   }

   printf("RESULT stack=%s record_bytes=%u records=%u flush_every=%u gc_ratio=%u lookahead=%u warmup=%u verified=%u",
          stack_names[stack], record_bytes, records, flush_every, gc_ratio,
          (stack >= STACK_LFS_DHARA) ? lookahead : 0, warmup, verify ? verified : 0);
   printf(" nl_epochs_begun_warmup=%u nl_epochs_begun_counted=%u", warm_epochs, epochs_begun - warm_epochs);
   printf(" user_bytes=%llu", (unsigned long long)record_bytes * records);
   print_ops("stream", stream);
   print_ops("finalflush", final_flush);
   print_calls("append", &appends);
   print_calls("flush", &flushes);
   print_ops("driverboot", driver_boot);
   print_ops("boot", boot);
   print_ops("firstappend", first_append);
   print_ops("firstflush", first_flush);
   print_ops("readback", readback_ops);
#ifdef DHARA_GLUE_COUNTERS
   if (stack == STACK_DHARA || stack == STACK_LFS_DHARA)
      printf(" dhara_stream_progs=%u dhara_stream_copies=%u dla_stream_sector_writes=%u lfs_stream_bd_syncs=%u",
             stream_dhara_progs, stream_dhara_copies, stream_dla_writes, stream_lfs_bd_syncs);
#endif
   printf(" tail_verified=%u", tail_verified);
   printf(" nl_pages_committed=%u dhara_sectors=%llu lfs_blocks_in_use=%llu", nl_stats.pages_committed,
          (unsigned long long)dhara_sectors, (unsigned long long)lfs_blocks);
   if (stack == STACK_DHARA)
      printf(" dhara_capacity=%u dhara_fixed_capacity=%u dhara_log2_ppc=%u", (unsigned)dhara_map_capacity(&dla.map),
             (unsigned)dhara_fixed_capacity(&dla.map, DHARA_GLUE_ASSUMED_BAD_BLOCKS), (unsigned)dla.map.journal.log2_ppc);
   if (stack == STACK_LFS_DHARA)
      printf(" dhara_capacity=%u dhara_fixed_capacity=%u dhara_log2_ppc=%u", (unsigned)dhara_map_capacity(&lfs_map),
             (unsigned)dhara_fixed_capacity(&lfs_map, DHARA_GLUE_ASSUMED_BAD_BLOCKS), (unsigned)lfs_map.journal.log2_ppc);
   printf("\n");
   nandlog_sim_destroy();
   return 0;
}
