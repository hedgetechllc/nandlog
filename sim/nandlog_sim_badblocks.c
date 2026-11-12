// The Alliance driver's nandlog_chip_is_bad_block() against a full search of its table, run against the RAM-backed
// port. `make` builds and runs it, over the full-size part and over the campaign's small one.
//
// Every boot asks the driver about each block of the log region in order, so the driver keeps the run of pages around
// the last block it was asked about that holds no entry of the table on the part, and answers from that run without a
// search. An answer from a stale run would put a log page in a retired block, so this holds every answer to what a
// search of the table would say, after every kind of change the table goes through: factory marks at the first boot,
// retirements, reboots, versions moved below the reserve when its blocks die, blocks given to the table, and degraded
// mode, where nowhere takes a version and the marks stand in. The table and its count are the driver's own statics,
// so the driver's source is compiled into this file rather than linked beside it.
//
// Usage: nandlog_sim_badblocks [rounds]

#include <stdio.h>
#include <stdlib.h>
#include "../chips/nandlog_chip_AS5F18G04SND.c"
#include "nandlog_port_sim.h"

static const uint8_t DEVICE_ID[3] = { 0x8D, 0x00, 0x00 };
static uint64_t rng = 88172645463325252ull, checks, degraded_steps, relocated_steps, pairs_taken;

static uint32_t random_below(uint32_t n)
{
   rng ^= rng << 13;
   rng ^= rng >> 7;
   rng ^= rng << 17;
   return (uint32_t)(rng % n);
}

static void check(uint32_t page, const char *when)
{
   ++checks;
   const bool expected = table_contains(page, bbm_index), answered = nandlog_chip_is_bad_block(page);
   if (expected != answered)
   {
      printf("FAILED %s: page %u (block %u) answered %d, the table says %d; %u entries, clear run [%u, %u)\n", when,
             page, page / NANDLOG_CHIP_PAGES_PER_BLOCK, answered, expected, bbm_index, clear_first_page, clear_end_page);
      exit(1);
   }
}

static void ask_everything(const char *when)
{
   // In order, as a boot asks, at a random page of each block; in reverse; then anywhere
   for (uint32_t block = 0; block < NANDLOG_CHIP_BLOCK_COUNT; ++block)
      check((block * NANDLOG_CHIP_PAGES_PER_BLOCK) + random_below(NANDLOG_CHIP_PAGES_PER_BLOCK), when);
   for (uint32_t block = NANDLOG_CHIP_BLOCK_COUNT; block-- > 0; )
      check(block * NANDLOG_CHIP_PAGES_PER_BLOCK, when);
   for (uint32_t i = 0; i < NANDLOG_CHIP_BLOCK_COUNT; ++i)
      check(random_below(NANDLOG_CHIP_PAGE_COUNT), when);
}

static uint32_t log_region_page(void)
{
   return random_below(NANDLOG_CHIP_RESERVED_BASE_PAGE);
}

int main(int argc, char **argv)
{
   const uint32_t rounds = (argc > 1) ? (uint32_t)atoi(argv[1]) : 20;
   printf("bad-block answers against a search of the table, %u rounds\n", rounds);
   for (uint32_t round = 0; round < rounds; ++round)
   {
      rng += round * 7919u;
      nandlog_sim_create(DEVICE_ID, sizeof(DEVICE_ID));
      nandlog_sim_seed(round + 1);
      for (uint32_t marks = random_below(12); marks; --marks)
         nandlog_sim_raw_page(random_below(NANDLOG_CHIP_BLOCK_COUNT) * NANDLOG_CHIP_PAGES_PER_BLOCK)[NANDLOG_CHIP_PAGE_SIZE_BYTES] = 0;
      nandlog_port_init();
      nandlog_chip_init();
      ask_everything("after the first boot");

      // Every third round, every block of the reserve dies and the table is never given blocks: once the version in
      // hand cannot be added to, the marks stand in for the table
      const bool starved = (round % 3) == 2;
      if (starved)
         for (uint32_t block = NANDLOG_CHIP_BLOCK_COUNT - NANDLOG_CHIP_RESERVED_BLOCKS; block < NANDLOG_CHIP_BLOCK_COUNT; ++block)
            nandlog_sim_kill_block(block);

      for (uint32_t step = 0; step < 60; ++step)
      {
         degraded_steps += table_abandoned ? 1 : 0;
         relocated_steps += table_relocated ? 1 : 0;
         switch (starved ? (random_below(2) * 2) : random_below(6))
         {
            case 0:
            case 1:
               nandlog_chip_mark_bad_block(log_region_page());
               ask_everything("after a retirement");
               break;
            case 2:
               nandlog_chip_init();
               ask_everything("after a reboot");
               break;
            case 3:
               // A block of the reserve dies, so a later version may have to go somewhere else
               nandlog_sim_kill_block(NANDLOG_CHIP_BLOCK_COUNT - 1 - random_below(NANDLOG_CHIP_RESERVED_BLOCKS));
               nandlog_chip_mark_bad_block(log_region_page());
               ask_everything("after a retirement with a reserve block dead");
               break;
            case 4:
            {
               // A pair from as low as the driver's boot search reaches, which is all it will take
               const uint32_t lowest = TABLE_SEARCH_FIRST_PAGE / NANDLOG_CHIP_PAGES_PER_BLOCK,
                              pair = (lowest + random_below((NANDLOG_CHIP_RESERVED_BASE_PAGE / NANDLOG_CHIP_PAGES_PER_BLOCK) - lowest)) & ~1u;
               pairs_taken += nandlog_chip_table_take_blocks(pair * NANDLOG_CHIP_PAGES_PER_BLOCK) ? 1 : 0;
               ask_everything("after giving the table blocks");
               break;
            }
            default:
               for (uint32_t i = 0; i < 2000; ++i)
                  check(random_below(NANDLOG_CHIP_PAGE_COUNT), "at random");
               break;
         }
      }
      nandlog_sim_destroy();
   }

   // Each kind of change must have happened, or the run proved less than it says
   printf("%llu answers checked; steps with the table below the reserve %llu, in degraded mode %llu; pairs given %llu\n",
          (unsigned long long)checks, (unsigned long long)relocated_steps, (unsigned long long)degraded_steps,
          (unsigned long long)pairs_taken);
   const bool covered = relocated_steps && degraded_steps && pairs_taken;
   printf("\n%llu checks, %u failed\n", (unsigned long long)checks, covered ? 0u : 1u);
   return covered ? 0 : 1;
}
