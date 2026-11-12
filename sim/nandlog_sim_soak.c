// The on-device power-cut soak, tests/test_nandlog_powercut.c, run unchanged against the simulator's emulated
// Alliance part, with the power cut at random -- the host build behind `make soak`. Its main() is renamed by the
// build, and what the board would print over its serial port goes to stdout, for tests/powercut_check.py.
//
//    nandlog_sim_soak <boots> [sabotage_boot]
//
// Each boot is cut either in the middle of a program or erase (the simulator's cut models: prefix, bitwise, unstable,
// boundary, at a random q), or between operations at a random point of the logging loop, where the soak waits. The
// last boot is let verify and print its POWERCUT line. With a sabotage boot, one committed page of the newest epoch
// has a payload bit cleared just before that boot -- data loss the checker must report, which shows it can.

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nandlog_hw_test.h"
#include "nandlog_port_sim.h"

int powercut_main(void);

static jmp_buf cut_jump;
static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t delay_budget;
static bool delay_cut_armed, cut_between_operations;
static uint32_t delay_cuts, operation_cuts;

static uint32_t rnd(void)
{
   rng_state ^= rng_state << 13;
   rng_state ^= rng_state >> 7;
   rng_state ^= rng_state << 17;
   return (uint32_t)(rng_state >> 32);
}


// The Board, As The Soak Sees It --------------------------------------------------------------------------------------

uint32_t hw_checks_run, hw_checks_failed;

uint32_t host_cycle_count(void)
{
   // A host clock, scaled to the 96 cycles a microsecond the soak is told it has
   struct timespec now;
   clock_gettime(CLOCK_MONOTONIC, &now);
   return (uint32_t)((((uint64_t)now.tv_sec * 1000000000ull) + (uint64_t)now.tv_nsec) * 96u / 1000u);
}

void hw_timing_init(void)                                { print("Timing calibrated: 96 cycles/us (host clock)\n"); }
uint32_t hw_cycles_per_us(void)                          { return 96; }
uint32_t hw_elapsed_us(uint32_t start, uint32_t end)     { return (end - start) / 96; }
void hw_power_up(void)                                   { nandlog_begin_session(); }
void hw_power_down(void)                                 { nandlog_end_session(); }
uint32_t hw_scratch_page(uint32_t index)                 { (void)index; return 0; }
void hw_fill_pattern(uint8_t *buffer, uint32_t length, uint32_t seed)              { (void)buffer; (void)length; (void)seed; }
bool hw_check_pattern(const uint8_t *buffer, uint32_t length, uint32_t seed)       { (void)buffer; (void)length; (void)seed; return true; }
void system_enable_interrupts(bool enabled)              { (void)enabled; }
void system_reset(bool immediate)                        { (void)immediate; abort(); }

uint32_t am_util_stdio_printf(const char *format, ...)
{
   va_list args;
   va_start(args, format);
   const int written = vprintf(format, args);
   va_end(args);
   return (uint32_t)written;
}

void setup_hardware(void)
{
   print("\n----------------------------------------\nReset Reasons: HW Power-On Reset, \n");
}

void am_hal_delay_us(uint32_t microseconds)
{
   // The soak waits here once a record and in its idle loop, so a cut between operations lands here
   (void)microseconds;
   if (delay_cut_armed && !--delay_budget)
   {
      cut_between_operations = true;
      longjmp(cut_jump, 2);
   }
}


// The Harness ---------------------------------------------------------------------------------------------------------

static void rot_a_committed_page(void)
{
   // Clear one payload bit of a page of the newest epoch, as a cell that failed after the commit would
   const nandlog_geometry_t *g = nandlog_chip_geometry();
   const uint32_t first = 8 * g->pages_per_block, end = (g->block_count - g->reserved_blocks) * g->pages_per_block;
   uint32_t newest = 0, target = 0xFFFFFFFFu;
   for (int pass = 0; pass < 2; ++pass)
      for (uint32_t page = first; page < end; ++page)
      {
         nandlog_page_header_t header;
         memcpy(&header, nandlog_sim_raw_page(page), sizeof(header));
         if (header.magic != NANDLOG_PAGE_MAGIC_FRAMED)
            continue;
         if (!pass && (header.epoch > newest))
            newest = header.epoch;
         else if (pass && (header.epoch == newest) && (header.seq >= 2) && header.payload_length && (target == 0xFFFFFFFFu))
            target = page;
      }
   if (target == 0xFFFFFFFFu)
   {
      fprintf(stderr, "harness: nothing to sabotage yet\n");
      return;
   }

   // The first payload byte that has a bit to lose, so the damage cannot be a no-op
   const uint8_t *raw = nandlog_sim_raw_page(target);
   uint32_t offset = sizeof(nandlog_page_header_t) + 12;
   while (!raw[offset])
      ++offset;
   const uint8_t before = raw[offset];
   nandlog_sim_corrupt(target, offset, (uint8_t)(before & (0u - before)));
   fprintf(stderr, "harness: rotted page %u (epoch %u) byte %u: %02X -> %02X\n", target, newest, offset, before, raw[offset]);
}

int main(int argc, char **argv)
{
   const uint32_t boots = (argc > 1) ? (uint32_t)strtoul(argv[1], NULL, 10) : 300;
   const uint32_t sabotage_boot = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 0;
   static const uint8_t device_id[3] = { 0x8D, 0x00, 0x00 };
   static const nandlog_sim_cut_model_t models[4] = { NANDLOG_SIM_CUT_PREFIX, NANDLOG_SIM_CUT_BITWISE, NANDLOG_SIM_CUT_UNSTABLE, NANDLOG_SIM_CUT_BOUNDARY };
   static const double q_values[8] = { 0.0005, 0.002, 0.01, 0.05, 0.2, 0.5, 0.8, 0.95 };

   nandlog_sim_create(device_id, sizeof(device_id));
   nandlog_sim_faults_t faults;
   memset(&faults, 0, sizeof(faults));
   memcpy(faults.device_id, device_id, sizeof(device_id));
   faults.torn_reads_uncorrectable = true;
   faults.ecc_hides_bits = 32;
   nandlog_sim_set_faults(&faults);
   nandlog_sim_set_verbose(true);

   // One factory-bad block inside the log region, so positions step over it, and the part's first boot builds a table
   const nandlog_geometry_t *g = nandlog_chip_geometry();
   nandlog_sim_raw_page(20 * g->pages_per_block)[g->page_size_bytes] = 0x00;

   for (uint32_t boot = 0; boot < boots; ++boot)
   {
      if (sabotage_boot && (boot == sabotage_boot))
         rot_a_committed_page();
      nandlog_sim_reset_operations();
      nandlog_sim_seed(((uint64_t)rnd() << 32) | rnd());
      delay_cut_armed = true;
      if ((boot + 1) == boots)
         delay_budget = 1;                                     // verify, print, then stop at the first record
      else if ((rnd() % 10) < 6)
      {
         nandlog_sim_power_cut(((rnd() % 5) == 0) ? (1 + (rnd() % 4)) : (1 + (rnd() % 120)), models[rnd() % 4], q_values[rnd() % 8], &cut_jump);
         delay_budget = 400000;                                // a backstop, in case the boot idles first
      }
      else
         delay_budget = 1 + (rnd() % 30000);
      cut_between_operations = false;
      if (setjmp(cut_jump) == 0)
         powercut_main();
      if (cut_between_operations)
         ++delay_cuts;
      else
         ++operation_cuts;
      nandlog_sim_power_cut_disarm();
      delay_cut_armed = false;
      fflush(stdout);
      nandlog_deinit();
   }
   const nandlog_sim_counters_t counters = nandlog_sim_counters();
   fprintf(stderr, "harness: %u boots, %u cuts inside a program or erase, %u between operations; %u page programs, %u erases, %u repeated programs\n",
           boots, operation_cuts, delay_cuts, counters.page_writes, counters.block_erases, counters.repeated_programs);
   nandlog_sim_destroy();
   return 0;
}
