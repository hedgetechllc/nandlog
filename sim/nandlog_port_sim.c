// A RAM-backed implementation of nandlog_port.h, for running the log on a host.
//
// The porting seam is at the SPI command level, so this is not a stub that pretends to be a log -- it is an
// emulation of a NAND part, and the real chip driver runs unmodified on top of it. That is what makes a host
// test worth anything: the addressing, the status-register handshakes, the busy-waiting, the bad-block
// bookkeeping and the power-fail behaviour are all the shipping code, exercised at memory speed.
//
// NAND semantics are honoured rather than approximated. Erase sets bits to one; programming may only clear
// them, so a page programmed twice keeps the AND of both writes. The part has one cache register: a page read
// fills it, a program load resets it to 0xFF from the column it was given to the end of the spare area -- and
// only from there, as the Alliance part does on silicon, so what lies before that column is left as the last read
// or load left it -- a read from cache returns it, at any column and as often as asked, and a program executes all
// of it. A random-data load (84h) writes its bytes from the column it names and leaves the rest of the register as
// it was, and only inside an internal data move, as the datasheet has it: straight after a PAGE DATA READ and its
// status polls, or another 84h. Any other is refused, counted and reported, so a driver that misuses it fails. So
// an internal data move is a page read followed by a program with no load between, or with 84h loads, exactly as
// on the part, and a load that does not start at column 0 programs stale bytes, exactly as on the part. Getting any
// of this wrong would make the simulator agree with a buggy log.
//
// Faults are deliberately adversarial, because a fault model is a claim about the medium and an optimistic
// one passes code that real silicon breaks. A cut can land between operations, leave the first half of the
// operation applied, or leave every bit it would have changed changed independently with some probability --
// and in the unstable variant, the bits left undecided read back differently on every latch until the block
// is erased. With ECC turned on, a page can be made to report an uncorrectable error, a correction, or a
// correction at the limit of the part's ECC; a page a cut left torn can be made to read uncorrectable; and one
// that a cut left only lightly programmed can be made to read back as erased, as the erased-page handling in a
// part's ECC engine would show it. A read with ECC turned off always returns the cells as they are.
//
// Two hazards of time are modelled as well. A page is WEAK if a cut stopped its program before every bit it was
// to change had changed, or if it was programmed in a block whose last erase a cut interrupted -- one that may
// read erased and yet hold charge unevenly. nandlog_sim_drift() then plays a long wait: a weak page's programmed
// cells drift back, all of them or some, so a page that read torn can come to read blank in every bit. A page
// whose program completed in a block whose erase completed never drifts. A program of a page that drifted is
// counted on its own, because nothing should ever program such a page again.

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nandlog_chip.h"
#include "nandlog_port.h"
#include "nandlog_port_sim.h"


// Emulated Part -------------------------------------------------------------------------------------------------------

#define SIM_COMMAND_READ_DEVICE_ID                  0x9F
#define SIM_COMMAND_DEVICE_RESET                    0xFF
#define SIM_COMMAND_READ_STATUS_REGISTER            0x0F
#define SIM_COMMAND_WRITE_STATUS_REGISTER           0x1F
#define SIM_COMMAND_WRITE_ENABLE                    0x06
#define SIM_COMMAND_WRITE_DISABLE                   0x04
#define SIM_COMMAND_BLOCK_ERASE                     0xD8
#define SIM_COMMAND_PROGRAM_DATA_LOAD               0x02
#define SIM_COMMAND_PROGRAM_LOAD_RANDOM_DATA        0x84
#define SIM_COMMAND_PROGRAM_EXECUTE                 0x10
#define SIM_COMMAND_PAGE_DATA_READ                  0x13
#define SIM_COMMAND_READ                            0x03
#define SIM_COMMAND_WRITE_BBM_LUT                   0xA1
#define SIM_COMMAND_READ_BBM_LUT                    0xA5

#define SIM_STATUS_REGISTER_1                       0xA0
#define SIM_STATUS_REGISTER_2                       0xB0
#define SIM_STATUS_REGISTER_3                       0xC0

#define SIM_STATUS_LUT_FULL                         0b01000000
#define SIM_STATUS_ECC_MASK                         0b00110000
#define SIM_STATUS_WRITE_FAILURE                    0b00001000
#define SIM_STATUS_ERASE_FAILURE                    0b00000100
#define SIM_STATUS_BUSY                             0b00000001

#define SIM_CONFIG_OTP_ENTER                        0b01000000
#define SIM_CONFIG_ECC_ENABLE                       0b00010000

// Power-on register values: the array locked, ECC on, nothing pending
#define SIM_POWER_ON_PROTECT                        0x38
#define SIM_POWER_ON_CONFIG                         0x10

// The OTP region is a handful of pages mapped over the array when the configuration bit is set. Only the low
// pages are ever touched, so a small window is enough
#define SIM_OTP_PAGES                               8

// A remap table of the Winbond kind: logical block to spare block, one-way, a fixed number of entries
#define SIM_LUT_ENTRIES                             20

// Per-page state kept beside the cells
#define PAGE_TORN                                   0x01        // a cut left this page part-programmed or part-erased
#define PAGE_BLANK_TEAR                             0x04        // a cut stopped this page's program before any bit changed
#define PAGE_ECC_INJECTED                           0x02        // a test fixed what ECC reports for this page
#define PAGE_ECC_SHIFT                              4           // ...and the verdict, in the status register's bits
#define PAGE_WEAK                                   0x08        // its cells may drift back: see nandlog_sim_drift()
#define PAGE_DRIFTED                                0x40        // ...and the drift model has moved them

#define SIM_MAX_UNSTABLE_PAGES                      256
#define SIM_REPEATED_LOG                            64


// Simulated State -----------------------------------------------------------------------------------------------------

static uint8_t *array;                    // main array plus spare, one contiguous block of page-with-spare records
static uint8_t *otp;                      // the OTP window, which erase never reaches
static uint32_t page_size, spare_size, page_stride, page_count, pages_per_block, block_count;
static uint8_t status_register[3];        // indexed by (register number >> 4) - 0x0A
static uint8_t cache_register[NANDLOG_SIM_MAX_PAGE_WITH_SPARE];
static uint32_t read_page_address;        // the page most recently latched by PAGE_DATA_READ
static bool write_latch;                  // set by WRITE_ENABLE, cleared by the operation that consumes it
static bool data_move_open;               // a PAGE DATA READ, then nothing but status polls and 84h loads since
static bool port_open, powered, write_permitted;

static nandlog_sim_faults_t faults;
static nandlog_sim_counters_t counters;
static bool fatal_expected, fatal_seen, unwritable_reported;
static uint32_t lock_count;
static char fatal_reason[128];
static bool verbose;
static jmp_buf fatal_jump;

static uint8_t *page_programs;            // PROGRAM EXECUTEs a page has taken since its erase, saturating
static uint32_t *page_program_boot;       // the boot in which a page took its last PROGRAM EXECUTE
static uint32_t boot_generation;          // counts nandlog_port_init() calls, which is how the part sees a reboot
static uint8_t *page_flags;               // PAGE_* above
static uint32_t *page_latches;            // PAGE DATA READs of each page since the counters were last reset
static uint32_t *block_erasures;          // BLOCK ERASEs of each block, refused ones too, since the same
static uint32_t *block_programs;          // PROGRAM EXECUTEs into each block's pages, refused ones too, since the same
static uint8_t *block_weak_erase;         // a cut interrupted the block's last erase
static uint8_t *block_dead;               // the block reads 00h everywhere and refuses every program and erase
static uint32_t repeated_log[SIM_REPEATED_LOG], repeated_logged, blank_log[SIM_REPEATED_LOG], blank_logged;
static uint32_t mark_log[SIM_REPEATED_LOG], mark_logged;

static struct { uint16_t lba, pba; bool used; } lut[SIM_LUT_ENTRIES];

// The cut that is armed, and where the last one landed
static uint32_t cut_countdown, operations;
static nandlog_sim_cut_model_t cut_model;
static double cut_q;
static jmp_buf *cut_stop;
static bool power_failed, cut_valid;
static nandlog_sim_cut_t last_cut;
static uint64_t rng_state = 0x9E3779B97F4A7C15ULL;

// Cells a cut left undecided, which read back randomly on every latch until their block is erased
static struct { uint32_t page; bool used; uint8_t mask[NANDLOG_SIM_MAX_PAGE_WITH_SPARE]; } *unstable;

// Snapshot of the part, and which blocks have been touched since it was taken
static uint8_t *block_dirty, *block_touched, *snap_has;
static uint32_t *snap_slot;
static uint8_t *snap_data, *snap_otp, *snap_programs, *snap_flags, *snap_weak_erase;
static uint32_t *snap_program_boot;
static uint8_t snap_lut[sizeof(lut)];
static bool snap_valid;


// Private Helper Functions --------------------------------------------------------------------------------------------

static bool otp_selected(uint32_t page)
{
   return (status_register[1] & SIM_CONFIG_OTP_ENTER) && (page < SIM_OTP_PAGES);
}

static uint32_t physical_page(uint32_t page)
{
   // A logical block named in the remap table is answered by its spare, which is the whole point of the table
   page %= page_count;
   const uint32_t block = page / pages_per_block;
   for (uint32_t i = 0; i < SIM_LUT_ENTRIES; ++i)
      if (lut[i].used && (lut[i].lba == block))
         return (lut[i].pba * pages_per_block) + (page % pages_per_block);
   return page;
}

static uint8_t *page_at(uint32_t page)
{
   if (otp_selected(page))
      return otp + ((size_t)page * page_stride);
   return array + ((size_t)physical_page(page) * page_stride);
}

static uint32_t address_from(const uint8_t *bytes)
{
   return ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | (uint32_t)bytes[2];
}

static bool block_in_list(const uint32_t *list, uint32_t count, uint32_t page)
{
   const uint32_t block = page / faults.pages_per_block;
   for (uint32_t i = 0; i < count; ++i)
      if (list[i] == block)
         return true;
   return false;
}

static bool page_in_list(const uint32_t *list, uint32_t count, uint32_t page)
{
   for (uint32_t i = 0; i < count; ++i)
      if (list[i] == page)
         return true;
   return false;
}

static uint64_t rng_next(void)
{
   uint64_t x = rng_state;
   x ^= x >> 12;
   x ^= x << 25;
   x ^= x >> 27;
   rng_state = x;
   return x * 0x2545F4914F6CDD1DULL;
}

static bool rng_bernoulli(double p)
{
   return ((double)(rng_next() >> 11) * (1.0 / 9007199254740992.0)) < p;
}

static void touch_block(uint32_t block)
{
   if (block_dirty && (block < block_count))
      block_dirty[block] = block_touched[block] = 1;
}

static int unstable_find(uint32_t page)
{
   for (int i = 0; i < SIM_MAX_UNSTABLE_PAGES; ++i)
      if (unstable[i].used && (unstable[i].page == page))
         return i;
   return -1;
}

static uint8_t *unstable_mask_for(uint32_t page)
{
   int i = unstable_find(page);
   if (i >= 0)
      return unstable[i].mask;
   for (i = 0; i < SIM_MAX_UNSTABLE_PAGES; ++i)
      if (!unstable[i].used)
      {
         unstable[i].used = true;
         unstable[i].page = page;
         memset(unstable[i].mask, 0, sizeof(unstable[i].mask));
         return unstable[i].mask;
      }
   fprintf(stderr, "SIM: more than %u unstable pages at once\n", SIM_MAX_UNSTABLE_PAGES);
   exit(2);
}

static void unstable_clear_block(uint32_t block_start)
{
   for (int i = 0; i < SIM_MAX_UNSTABLE_PAGES; ++i)
      if (unstable[i].used && (unstable[i].page >= block_start) && (unstable[i].page < (block_start + pages_per_block)))
         unstable[i].used = false;
}

static void reset_volatile_state(void)
{
   // What the part forgets when it loses power
   data_move_open = false;
   status_register[0] = SIM_POWER_ON_PROTECT;
   status_register[1] = SIM_POWER_ON_CONFIG;
   status_register[2] = 0;
   memset(cache_register, 0xFF, sizeof(cache_register));
   read_page_address = 0;
   write_latch = false;
}

static bool cut_lands_here(void)
{
   // Counts every program and erase, and says whether this one is the one that gets cut short
   ++operations;
   if (!cut_countdown || --cut_countdown)
      return false;
   power_failed = true;
   return true;
}

static void record_cut(uint8_t command, uint32_t page, bool is_otp)
{
   last_cut.operation = operations;
   last_cut.command = command;
   last_cut.page = page;
   last_cut.otp = is_otp;
   cut_valid = true;
}

static void stop_if_power_is_gone(void)
{
   // Nothing after the cut executes: the part resets and control returns to the harness
   if (cut_stop)
   {
      jmp_buf *stop = cut_stop;
      cut_stop = NULL;
      cut_countdown = 0;
      reset_volatile_state();
      longjmp(*stop, 1);
   }
}

static uint32_t programmed_bits(const uint8_t *page)
{
   uint32_t bits = 0;
   for (uint32_t i = 0; i < page_stride; ++i)
      for (uint8_t value = (uint8_t)~page[i]; value; value &= (uint8_t)(value - 1))
         ++bits;
   return bits;
}

static void latch_page(uint32_t page)
{
   // PAGE DATA READ: decide the ECC verdict, then fill the cache register with what the part would put there
   const bool is_otp = otp_selected(page);
   const uint32_t physical = is_otp ? page : physical_page(page);
   uint8_t *cells = page_at(page);
   bool hide = false;
   status_register[2] &= (uint8_t)~SIM_STATUS_ECC_MASK;
   if (!is_otp)
   {
      // Undecided cells read however they read this time
      const int u = unstable_find(physical);
      if (u >= 0)
         for (uint32_t b = 0; b < page_stride; ++b)
            if (unstable[u].mask[b])
               cells[b] = (uint8_t)((cells[b] & (uint8_t)~unstable[u].mask[b]) | ((uint8_t)rng_next() & unstable[u].mask[b]));

      if (status_register[1] & SIM_CONFIG_ECC_ENABLE)
      {
         const uint8_t flags = page_flags[physical];
         if (flags & PAGE_ECC_INJECTED)
            status_register[2] |= (uint8_t)((flags >> PAGE_ECC_SHIFT) << 4) & SIM_STATUS_ECC_MASK;
         else if (flags & PAGE_TORN)
         {
            if (faults.ecc_hides_bits && (programmed_bits(cells) <= faults.ecc_hides_bits))
               hide = true;
            else if (faults.torn_reads_uncorrectable)
               status_register[2] |= NANDLOG_SIM_ECC_UNCORRECTABLE;
         }
         if (!hide && !(status_register[2] & SIM_STATUS_ECC_MASK) && page_in_list(faults.worn_pages, faults.num_worn_pages, physical))
            status_register[2] |= NANDLOG_SIM_ECC_AT_LIMIT;
      }
      else
         ++counters.raw_page_reads;
   }
   if (hide)
      memset(cache_register, 0xFF, page_stride);
   else
      memcpy(cache_register, cells, page_stride);
   read_page_address = page;
   ++counters.page_reads;
   if (!is_otp)
      ++page_latches[physical];
}

static void program_page(uint32_t page)
{
   // PROGRAM EXECUTE: the cache register goes into the page, as an AND, because programming only clears bits
   const bool is_otp = otp_selected(page);
   const uint32_t physical = is_otp ? page : physical_page(page);
   status_register[2] &= (uint8_t)~SIM_STATUS_WRITE_FAILURE;
   ++counters.page_writes;
   if (!is_otp)
      ++block_programs[physical / pages_per_block];
   if (!write_latch || !write_permitted)
   {
      // A program with the latch or the pin against it is refused, exactly as the part would
      status_register[2] |= SIM_STATUS_WRITE_FAILURE;
      return;
   }
   write_latch = false;

   // A page the part accepted a PROGRAM EXECUTE for since its erase has already had charge put into it, whatever
   // that program then reported; a second one is a second program on a part that permits one
   const bool refused = !is_otp && (block_dead[physical / pages_per_block] ||
                                    block_in_list(faults.unwritable_blocks, faults.num_unwritable_blocks, physical) ||
                                    page_in_list(faults.unwritable_pages, faults.num_unwritable_pages, physical));
   const bool cut = !refused && cut_lands_here();
   if (cut)
   {
      record_cut(SIM_COMMAND_PROGRAM_EXECUTE, page, is_otp);
      if (cut_model == NANDLOG_SIM_CUT_BOUNDARY)
      {
         // Power went before this program began, so the part never saw it
         stop_if_power_is_gone();
         return;
      }
   }
   // A bad-block mark: ECC off, nothing in the main area, and the one byte at the start of the spare area 00h
   bool is_mark = !is_otp && page_programs[physical] && !(status_register[1] & SIM_CONFIG_ECC_ENABLE) && (cache_register[page_size] == 0x00);
   for (uint32_t i = 0; is_mark && (i < page_stride); ++i)
      is_mark = (i == page_size) || (cache_register[i] == 0xFF);
   if (is_mark)
   {
      ++counters.mark_reprograms;
      mark_log[mark_logged++ % SIM_REPEATED_LOG] = physical;
      if (page_programs[physical] < 255)
         ++page_programs[physical];
      page_program_boot[physical] = boot_generation;
   }
   else if (!is_otp)
   {
      if (page_programs[physical] && (page_flags[physical] & PAGE_DRIFTED))
      {
         ++counters.drifted_reprograms;
         repeated_log[repeated_logged++ % SIM_REPEATED_LOG] = physical;
      }
      else if (page_programs[physical])
      {
         // Within one boot the library saw the earlier program's status, so a second is a defect. Across a reboot,
         // a page every bit of which still reads erased is all anything can know of it
         if ((page_program_boot[physical] != boot_generation) && !programmed_bits(page_at(page)) && (unstable_find(physical) < 0))
         {
            ++counters.blank_reprograms_after_boot;
            blank_log[blank_logged++ % SIM_REPEATED_LOG] = physical;
         }
         else
         {
            ++counters.repeated_programs;
            repeated_log[repeated_logged++ % SIM_REPEATED_LOG] = physical;
         }
      }
      if (page_programs[physical] < 255)
         ++page_programs[physical];
      page_program_boot[physical] = boot_generation;
   }
   if (refused)
   {
      status_register[2] |= SIM_STATUS_WRITE_FAILURE;
      return;
   }

   uint8_t *cells = page_at(page);
   if (!is_otp)
      touch_block(physical / pages_per_block);
   if (cut)
   {
      const bool was_blank = !is_otp && !programmed_bits(cells);
      bool undecided = false, short_of_target = false;
      if (cut_model == NANDLOG_SIM_CUT_PREFIX)
      {
         // The prefix that made it
         for (uint32_t i = 0; i < page_stride; ++i)
         {
            if (i < (page_stride / 2))
               cells[i] &= cache_register[i];
            else
               short_of_target = short_of_target || (cells[i] & (uint8_t)~cache_register[i]);
         }
      }
      else
      {
         uint8_t *mask = (!is_otp && (cut_model == NANDLOG_SIM_CUT_UNSTABLE)) ? unstable_mask_for(physical) : NULL;
         for (uint32_t i = 0; i < page_stride; ++i)
         {
            const uint8_t pending = (uint8_t)(cells[i] & (uint8_t)~cache_register[i]);
            uint8_t applied = 0;
            for (uint8_t bit = 1; bit; bit = (uint8_t)(bit << 1))
               if ((pending & bit) && rng_bernoulli(cut_q))
                  applied |= bit;
            cells[i] &= (uint8_t)~applied;
            short_of_target = short_of_target || (pending & (uint8_t)~applied);
            if (mask)
            {
               mask[i] |= (uint8_t)(pending & (uint8_t)~applied);
               undecided = undecided || (pending & (uint8_t)~applied);
            }
         }
      }

      // A page the cut left exactly as erased reads as erased, to ECC as to anything else; any other is torn, and
      // weak if any bit it was to change did not
      if (!is_otp)
      {
         page_flags[physical] |= (was_blank && !undecided && !programmed_bits(cells)) ? PAGE_BLANK_TEAR : PAGE_TORN;
         if (short_of_target || block_weak_erase[physical / pages_per_block])
            page_flags[physical] |= PAGE_WEAK;
      }
      stop_if_power_is_gone();
      return;
   }

   for (uint32_t i = 0; i < page_stride; ++i)
      cells[i] &= cache_register[i];
   if (!is_otp && faults.has_stuck_cells && (physical == faults.stuck_page) && (faults.stuck_offset < page_stride))
      cells[faults.stuck_offset] |= faults.stuck_mask;
   if (!is_otp)
   {
      // Every cell a completed program drives to zero is driven to its full level, whatever an earlier program a cut
      // stopped had left it with; only a block whose erase a cut interrupted leaves it unsure
      page_flags[physical] &= (uint8_t)~PAGE_WEAK;
      if (block_weak_erase[physical / pages_per_block])
         page_flags[physical] |= PAGE_WEAK;
   }
   if (!is_otp)
   {
      // Cells a completed program drives to zero are no longer undecided
      const int u = unstable_find(physical);
      if (u >= 0)
         for (uint32_t i = 0; i < page_stride; ++i)
            unstable[u].mask[i] &= cache_register[i];
   }
}

static void erase_block(uint32_t page)
{
   const uint32_t physical = physical_page(page);
   const uint32_t block_start = (physical / pages_per_block) * pages_per_block;
   status_register[2] &= (uint8_t)~SIM_STATUS_ERASE_FAILURE;
   ++counters.block_erases;
   ++block_erasures[block_start / pages_per_block];
   if (!write_latch || !write_permitted)
   {
      status_register[2] |= SIM_STATUS_ERASE_FAILURE;
      return;
   }
   write_latch = false;
   if (block_dead[block_start / pages_per_block] || block_in_list(faults.unerasable_blocks, faults.num_unerasable_blocks, physical))
   {
      status_register[2] |= SIM_STATUS_ERASE_FAILURE;
      return;
   }

   touch_block(block_start / pages_per_block);
   uint32_t pages_to_erase = pages_per_block;
   const bool cut = cut_lands_here();
   if (cut)
   {
      record_cut(SIM_COMMAND_BLOCK_ERASE, block_start, false);
      if (cut_model == NANDLOG_SIM_CUT_BOUNDARY)
      {
         stop_if_power_is_gone();
         return;
      }
      block_weak_erase[block_start / pages_per_block] = 1;
      if (cut_model == NANDLOG_SIM_CUT_PREFIX)
         pages_to_erase /= 2;
      else
      {
         // Every programmed cell of the block independently reaches the erased state with probability q
         unstable_clear_block(block_start);
         for (uint32_t i = 0; i < pages_per_block; ++i)
         {
            uint8_t *cells = array + ((size_t)(block_start + i) * page_stride);
            uint8_t *mask = NULL;
            bool left = false;
            for (uint32_t b = 0; b < page_stride; ++b)
            {
               const uint8_t pending = (uint8_t)~cells[b];
               if (!pending)
                  continue;
               uint8_t applied = 0;
               for (uint8_t bit = 1; bit; bit = (uint8_t)(bit << 1))
                  if ((pending & bit) && rng_bernoulli(cut_q))
                     applied |= bit;
               cells[b] |= applied;
               const uint8_t undecided = (uint8_t)(pending & (uint8_t)~applied);
               if (undecided)
               {
                  left = true;
                  if (cut_model == NANDLOG_SIM_CUT_UNSTABLE)
                  {
                     if (!mask)
                        mask = unstable_mask_for(block_start + i);
                     mask[b] |= undecided;
                  }
               }
            }
            page_programs[block_start + i] = 0;
            page_flags[block_start + i] = left ? PAGE_TORN : 0;
         }
         stop_if_power_is_gone();
         return;
      }
   }

   unstable_clear_block(block_start);
   for (uint32_t i = 0; i < pages_to_erase; ++i)
   {
      memset(array + ((size_t)(block_start + i) * page_stride), 0xFF, page_stride);
      page_programs[block_start + i] = 0;
      page_flags[block_start + i] = 0;
   }
   if (cut)
      stop_if_power_is_gone();
   else
      block_weak_erase[block_start / pages_per_block] = 0;
}

static void write_lut_entry(const uint8_t *in, uint32_t length)
{
   // The remap is written into the part's own table, one entry per command, and never removed
   if (!write_latch || !write_permitted || (length < 4))
      return;
   write_latch = false;
   if (cut_lands_here())
   {
      // A cut here is modelled as the entry never being written
      record_cut(SIM_COMMAND_WRITE_BBM_LUT, 0, false);
      stop_if_power_is_gone();
      return;
   }
   const uint16_t lba = (uint16_t)((((uint16_t)in[0] << 8) | in[1]) & 0x3FF);
   const uint16_t pba = (uint16_t)((((uint16_t)in[2] << 8) | in[3]) & 0x3FF);
   for (uint32_t i = 0; i < SIM_LUT_ENTRIES; ++i)
      if (!lut[i].used)
      {
         lut[i].used = true;
         lut[i].lba = lba;
         lut[i].pba = pba;
         touch_block(pba);
         if (i == (SIM_LUT_ENTRIES - 1))
            status_register[2] |= SIM_STATUS_LUT_FULL;
         return;
      }
   status_register[2] |= SIM_STATUS_LUT_FULL;
}


// Simulator Control ---------------------------------------------------------------------------------------------------

void nandlog_sim_create(const uint8_t *device_id, uint32_t device_id_length)
{
   // Geometry comes from the compiled chip driver, so the emulated part cannot be the wrong shape for it
   const nandlog_geometry_t *geometry = nandlog_chip_geometry();
   nandlog_sim_destroy();
   page_size = geometry->page_size_bytes;
   spare_size = geometry->spare_size_bytes;
   page_stride = page_size + spare_size;
   pages_per_block = geometry->pages_per_block;
   block_count = geometry->block_count;
   page_count = pages_per_block * block_count;
   array = malloc((size_t)page_count * page_stride);
   otp = malloc((size_t)SIM_OTP_PAGES * page_stride);
   page_programs = calloc(page_count, 1);
   page_program_boot = calloc(page_count, sizeof(uint32_t));
   page_flags = calloc(page_count, 1);
   page_latches = calloc(page_count, sizeof(uint32_t));
   block_erasures = calloc(block_count, sizeof(uint32_t));
   block_programs = calloc(block_count, sizeof(uint32_t));
   block_weak_erase = calloc(block_count, 1);
   block_dead = calloc(block_count, 1);
   block_dirty = calloc(block_count, 1);
   block_touched = calloc(block_count, 1);
   unstable = calloc(SIM_MAX_UNSTABLE_PAGES, sizeof(*unstable));
   if (!array || !otp || !page_programs || !page_program_boot || !page_flags || !page_latches || !block_erasures || !block_programs || !block_weak_erase || !block_dead ||
       !block_dirty || !block_touched || !unstable)
   {
      fprintf(stderr, "SIM: out of memory for a %u-page part\n", page_count);
      exit(1);
   }
   memset(array, 0xFF, (size_t)page_count * page_stride);
   memset(otp, 0xFF, (size_t)SIM_OTP_PAGES * page_stride);
   memset(&counters, 0, sizeof(counters));
   memset(&faults, 0, sizeof(faults));
   memset(lut, 0, sizeof(lut));
   faults.pages_per_block = pages_per_block;
   memcpy(faults.device_id, device_id, (device_id_length < sizeof(faults.device_id)) ? device_id_length : sizeof(faults.device_id));
   reset_volatile_state();
   powered = write_permitted = port_open = false;
   cut_countdown = operations = repeated_logged = blank_logged = mark_logged = 0;
   cut_stop = NULL;
   power_failed = cut_valid = fatal_expected = fatal_seen = unwritable_reported = false;
   lock_count = 0;
}

void nandlog_sim_destroy(void)
{
   free(array);
   free(otp);
   free(page_programs);
   free(page_program_boot);
   free(page_flags);
   free(page_latches);
   page_latches = NULL;
   free(block_erasures);
   block_erasures = NULL;
   free(block_programs);
   block_programs = NULL;
   free(block_weak_erase);
   free(block_dead);
   free(block_dirty);
   free(block_touched);
   free(unstable);
   free(snap_has);
   free(snap_slot);
   free(snap_data);
   free(snap_otp);
   free(snap_programs);
   free(snap_flags);
   free(snap_weak_erase);
   free(snap_program_boot);
   snap_program_boot = NULL;
   array = otp = page_programs = page_flags = block_weak_erase = block_dead = block_dirty = block_touched = snap_has = NULL;
   page_program_boot = NULL;
   snap_data = snap_otp = snap_programs = snap_flags = snap_weak_erase = NULL;
   snap_slot = NULL;
   unstable = NULL;
   snap_valid = false;
}

void nandlog_sim_set_faults(const nandlog_sim_faults_t *new_faults)
{
   const uint32_t ppb = faults.pages_per_block;
   faults = *new_faults;
   if (!faults.pages_per_block)
      faults.pages_per_block = ppb;
}

void nandlog_sim_set_ecc_status(uint32_t page, uint8_t status)
{
   if (page >= page_count)
      return;
   page_flags[page] &= (uint8_t)~(PAGE_ECC_INJECTED | (0x03 << PAGE_ECC_SHIFT));
   if (status != NANDLOG_SIM_ECC_CLEAN)
      page_flags[page] |= (uint8_t)(PAGE_ECC_INJECTED | (((status >> 4) & 0x03) << PAGE_ECC_SHIFT));
   touch_block(page / pages_per_block);
}

void nandlog_sim_kill_block(uint32_t block)
{
   // What the board's top blocks were found doing: 00h in every byte of every page, main and spare, a clean ECC
   // verdict, and every program and erase refused. A part property, kept for as long as the part exists; the cells
   // go into any snapshot taken after, like any other change
   if (block >= block_count)
      return;
   block_dead[block] = 1;
   unstable_clear_block(block * pages_per_block);
   for (uint32_t i = 0; i < pages_per_block; ++i)
   {
      const uint32_t page = (block * pages_per_block) + i;
      memset(array + ((size_t)page * page_stride), 0x00, page_stride);
      page_flags[page] = 0;
   }
   touch_block(block);
}

bool nandlog_sim_block_dead(uint32_t block)
{
   return (block < block_count) && block_dead[block];
}

void nandlog_sim_power_fail_after(uint32_t operations_ahead)
{
   nandlog_sim_power_cut(operations_ahead, NANDLOG_SIM_CUT_PREFIX, 0.5, NULL);
}

void nandlog_sim_power_cut(uint32_t operations_ahead, nandlog_sim_cut_model_t model, double q, jmp_buf *stop)
{
   cut_countdown = operations_ahead;
   cut_model = model;
   cut_q = q;
   cut_stop = stop;
   power_failed = cut_valid = false;
}

void nandlog_sim_power_cut_disarm(void)
{
   cut_countdown = 0;
   cut_stop = NULL;
}

bool nandlog_sim_power_failed(void)
{
   return power_failed;
}

bool nandlog_sim_last_cut(nandlog_sim_cut_t *cut)
{
   if (cut && cut_valid)
      *cut = last_cut;
   return cut_valid;
}

uint32_t nandlog_sim_operations(void)
{
   return operations;
}

void nandlog_sim_reset_operations(void)
{
   operations = 0;
}

void nandlog_sim_seed(uint64_t seed)
{
   rng_state = seed ? seed : 0x9E3779B97F4A7C15ULL;
}

uint32_t nandlog_sim_repeated_program_pages(uint32_t *pages, uint32_t capacity)
{
   const uint32_t held = (repeated_logged < SIM_REPEATED_LOG) ? repeated_logged : SIM_REPEATED_LOG;
   const uint32_t first = repeated_logged - held;
   uint32_t n = 0;
   for (uint32_t i = 0; (i < held) && (n < capacity); ++i)
      pages[n++] = repeated_log[(first + i) % SIM_REPEATED_LOG];
   return repeated_logged;
}

uint32_t nandlog_sim_blank_reprogram_pages(uint32_t *pages, uint32_t capacity)
{
   const uint32_t held = (blank_logged < SIM_REPEATED_LOG) ? blank_logged : SIM_REPEATED_LOG;
   const uint32_t first = blank_logged - held;
   uint32_t n = 0;
   for (uint32_t i = 0; (i < held) && (n < capacity); ++i)
      pages[n++] = blank_log[(first + i) % SIM_REPEATED_LOG];
   return blank_logged;
}

void nandlog_sim_clear_repeated_programs(void)
{
   repeated_logged = blank_logged = mark_logged = 0;
   counters.repeated_programs = counters.blank_reprograms_after_boot = counters.drifted_reprograms = counters.mark_reprograms = 0;
}

uint32_t nandlog_sim_mark_pages(uint32_t *pages, uint32_t capacity)
{
   const uint32_t held = (mark_logged < SIM_REPEATED_LOG) ? mark_logged : SIM_REPEATED_LOG;
   const uint32_t first = mark_logged - held;
   uint32_t n = 0;
   for (uint32_t i = 0; (i < held) && (n < capacity); ++i)
      pages[n++] = mark_log[(first + i) % SIM_REPEATED_LOG];
   return mark_logged;
}

uint32_t nandlog_sim_drift(nandlog_sim_drift_t how, double p)
{
   // Time passes: each weak page, with probability p, loses the charge its cells were left with -- all of it, so
   // the page reads blank in every bit, or each programmed bit with even odds
   uint32_t drifted = 0;
   for (uint32_t page = 0; page < page_count; ++page)
   {
      uint8_t *cells = array + ((size_t)page * page_stride);
      if (!(page_flags[page] & PAGE_WEAK) || !programmed_bits(cells) || !rng_bernoulli(p))
         continue;
      const int u = unstable_find(page);
      if (u >= 0)
         unstable[u].used = false;
      if (how == NANDLOG_SIM_DRIFT_BLANK)
      {
         memset(cells, 0xFF, page_stride);
         page_flags[page] &= (uint8_t)~(PAGE_TORN | PAGE_BLANK_TEAR);
      }
      else
         for (uint32_t i = 0; i < page_stride; ++i)
            cells[i] |= (uint8_t)(~cells[i] & (uint8_t)rng_next());
      page_flags[page] |= PAGE_DRIFTED;
      touch_block(page / pages_per_block);
      ++drifted;
   }
   return drifted;
}

uint32_t nandlog_sim_weak_pages(void)
{
   uint32_t weak = 0;
   for (uint32_t page = 0; page < page_count; ++page)
      weak += (page_flags[page] & PAGE_WEAK) ? 1 : 0;
   return weak;
}

void nandlog_sim_snapshot(void)
{
   free(snap_has);
   free(snap_slot);
   free(snap_data);
   free(snap_otp);
   free(snap_programs);
   free(snap_flags);
   free(snap_weak_erase);
   free(snap_program_boot);
   const size_t block_bytes = (size_t)pages_per_block * page_stride;
   uint32_t count = 0;
   for (uint32_t b = 0; b < block_count; ++b)
      count += block_dirty[b] ? 1 : 0;
   snap_has = calloc(block_count, 1);
   snap_slot = calloc(block_count, sizeof(uint32_t));
   snap_data = malloc((count ? count : 1) * block_bytes);
   snap_otp = malloc((size_t)SIM_OTP_PAGES * page_stride);
   snap_programs = malloc(page_count);
   snap_flags = malloc(page_count);
   snap_weak_erase = malloc(block_count);
   snap_program_boot = malloc((size_t)page_count * sizeof(uint32_t));
   if (!snap_has || !snap_slot || !snap_data || !snap_otp || !snap_programs || !snap_flags || !snap_weak_erase || !snap_program_boot)
   {
      fprintf(stderr, "SIM: out of memory for a %u-block snapshot\n", count);
      exit(1);
   }
   uint32_t slot = 0;
   for (uint32_t b = 0; b < block_count; ++b)
   {
      if (block_dirty[b])
      {
         snap_has[b] = 1;
         snap_slot[b] = slot;
         memcpy(snap_data + ((size_t)slot++ * block_bytes), array + ((size_t)b * block_bytes), block_bytes);
      }
      block_touched[b] = 0;
   }
   memcpy(snap_otp, otp, (size_t)SIM_OTP_PAGES * page_stride);
   memcpy(snap_programs, page_programs, page_count);
   memcpy(snap_flags, page_flags, page_count);
   memcpy(snap_weak_erase, block_weak_erase, block_count);
   memcpy(snap_program_boot, page_program_boot, (size_t)page_count * sizeof(uint32_t));
   memcpy(snap_lut, lut, sizeof(lut));
   snap_valid = true;
}

void nandlog_sim_restore(void)
{
   if (!snap_valid)
      return;
   const size_t block_bytes = (size_t)pages_per_block * page_stride;
   for (uint32_t b = 0; b < block_count; ++b)
   {
      if (!block_touched[b])
         continue;
      if (snap_has[b])
         memcpy(array + ((size_t)b * block_bytes), snap_data + ((size_t)snap_slot[b] * block_bytes), block_bytes);
      else
         memset(array + ((size_t)b * block_bytes), 0xFF, block_bytes);
      block_dirty[b] = snap_has[b];
      block_touched[b] = 0;
   }
   memcpy(otp, snap_otp, (size_t)SIM_OTP_PAGES * page_stride);
   memcpy(page_programs, snap_programs, page_count);
   memcpy(page_flags, snap_flags, page_count);
   memcpy(block_weak_erase, snap_weak_erase, block_count);
   memcpy(page_program_boot, snap_program_boot, (size_t)page_count * sizeof(uint32_t));
   memcpy(lut, snap_lut, sizeof(lut));
   for (int i = 0; i < SIM_MAX_UNSTABLE_PAGES; ++i)
      unstable[i].used = false;
   reset_volatile_state();
   cut_countdown = 0;
   cut_stop = NULL;
   power_failed = cut_valid = false;
}

jmp_buf *nandlog_sim_fatal_jump(void)
{
   return &fatal_jump;
}

bool nandlog_sim_dump(const char *path, uint32_t first_page, uint32_t num_pages)
{
   if (first_page >= page_count)
      return false;
   if (num_pages > (page_count - first_page))
      num_pages = page_count - first_page;
   FILE *f = fopen(path, "wb");
   if (!f)
      return false;
   const size_t bytes = (size_t)num_pages * page_stride;
   const bool ok = (fwrite(array + ((size_t)first_page * page_stride), 1, bytes, f) == bytes);
   fclose(f);
   return ok;
}

bool nandlog_sim_load(const char *path, uint32_t first_page)
{
   FILE *f = fopen(path, "rb");
   if (!f || (first_page >= page_count))
   {
      if (f)
         fclose(f);
      return false;
   }
   const size_t room = (size_t)(page_count - first_page) * page_stride;
   const size_t read = fread(array + ((size_t)first_page * page_stride), 1, room, f);
   fclose(f);
   for (uint32_t b = first_page / pages_per_block; b < block_count; ++b)
      touch_block(b);
   return (read > 0) && !(read % page_stride);
}

nandlog_sim_counters_t nandlog_sim_counters(void)
{
   return counters;
}

uint32_t nandlog_sim_page_latches(uint32_t page)
{
   return (page_latches && (page < page_count)) ? page_latches[physical_page(page)] : 0;
}

uint32_t nandlog_sim_block_erases(uint32_t block)
{
   return (block_erasures && (block < block_count)) ? block_erasures[physical_page(block * pages_per_block) / pages_per_block] : 0;
}

uint32_t nandlog_sim_block_programs(uint32_t block)
{
   return (block_programs && (block < block_count)) ? block_programs[physical_page(block * pages_per_block) / pages_per_block] : 0;
}

void nandlog_sim_reset_counters(void)
{
   memset(&counters, 0, sizeof(counters));
   if (page_latches)
      memset(page_latches, 0, (size_t)page_count * sizeof(uint32_t));
   if (block_erasures)
      memset(block_erasures, 0, (size_t)block_count * sizeof(uint32_t));
   if (block_programs)
      memset(block_programs, 0, (size_t)block_count * sizeof(uint32_t));
   unwritable_reported = false;
   lock_count = 0;
}

uint32_t nandlog_sim_lock_count(void)
{
   return lock_count;
}

bool nandlog_sim_unwritable_reported(void)
{
   return unwritable_reported;
}

void nandlog_sim_set_verbose(bool on)
{
   verbose = on;
}

void nandlog_sim_expect_fatal(bool expected)
{
   fatal_expected = expected;
   fatal_seen = false;
}

bool nandlog_sim_fatal_seen(const char **reason)
{
   if (reason)
      *reason = fatal_reason;
   return fatal_seen;
}

bool nandlog_sim_page_unstable(uint32_t page)
{
   if (page >= page_count)
      return false;
   const int u = unstable_find(physical_page(page));
   if (u < 0)
      return false;
   for (uint32_t b = 0; b < page_stride; ++b)
      if (unstable[u].mask[b])
         return true;
   return false;
}

uint8_t *nandlog_sim_raw_page(uint32_t page)
{
   if (page >= page_count)
      return NULL;
   touch_block(page / pages_per_block);      // the caller may write through this pointer
   return array + ((size_t)page * page_stride);
}

void nandlog_sim_corrupt(uint32_t page, uint32_t offset, uint8_t mask)
{
   // Clear bits in place, which is the one direction flash can rot without an erase
   uint8_t *p = nandlog_sim_raw_page(page);
   if (p && (offset < page_stride))
      p[offset] &= (uint8_t)~mask;
}


// Port Implementation -------------------------------------------------------------------------------------------------

bool nandlog_port_init(void)
{
   if (!array)
   {
      fprintf(stderr, "SIM: nandlog_port_init() before nandlog_sim_create()\n");
      exit(1);
   }
   ++boot_generation;
   port_open = powered = true;
   return true;
}

void nandlog_port_deinit(void)
{
   port_open = false;
}

void nandlog_port_write_enable(bool enable)
{
   write_permitted = enable;
}

void nandlog_port_power(bool awake)
{
   powered = awake;
}

void nandlog_port_delay_us(uint32_t microseconds)
{
   (void)microseconds;
}

void nandlog_port_delay_ms(uint32_t milliseconds)
{
   (void)milliseconds;
}

void nandlog_port_log(const char *format, ...)
{
   if (!verbose)
      return;
   va_list args;
   va_start(args, format);
   vprintf(format, args);
   va_end(args);
}

void nandlog_port_lock(void)
{
   ++lock_count;
}

void nandlog_port_unlock(void) {}

void nandlog_port_unwritable(void)
{
   unwritable_reported = true;
   if (verbose)
      fprintf(stderr, "SIM: log reported the part as unwritable\n");
}

// With NANDLOG_PORT_CRC32 every CRC the log writes or checks comes through here. A host has no engine, so this goes on
// from the library's software in deliberately awkward pieces -- odd lengths, split differently from one call to the
// next -- as a port does that hands an engine's unaligned head and short tail to the software, and as the log itself
// does, adding a page's records to its CRC one at a time. A piece that broke the running value would show as a page
// the reference parser rejects, and in NANDLOG_CHECK_CRC's comparison at every commit
#if NANDLOG_PORT_CRC32
static uint32_t crc32_calls, crc32_bytes;

uint32_t nandlog_port_crc32(uint32_t crc, const void *data, uint32_t length)
{
   static const uint32_t pieces[] = { 1, 3, 2, 7, 5, 13, 4, 61, 9, 251, 6, 1021 };
   const uint8_t *bytes = (const uint8_t*)data;
   uint32_t turn = crc32_calls++;
   crc32_bytes += length;
   while (length)
   {
      uint32_t piece = pieces[turn++ % (sizeof(pieces) / sizeof(pieces[0]))];
      if (piece > length)
         piece = length;
      crc = nandlog_crc32_software(crc, bytes, piece);
      bytes += piece;
      length -= piece;
   }
   return crc;
}

uint32_t nandlog_sim_crc32_calls(void) { return crc32_calls; }
uint32_t nandlog_sim_crc32_bytes(void) { return crc32_bytes; }
#endif

void nandlog_port_fatal(const char *reason)
{
   fatal_seen = true;
   snprintf(fatal_reason, sizeof(fatal_reason), "%s", reason ? reason : "");
   if (!fatal_expected)
   {
      fprintf(stderr, "SIM: unexpected fatal fault: %s\n", fatal_reason);
      exit(1);
   }
   // A real port resets the system here. The host equivalent is to unwind to the test, which never returns to
   // the caller -- exactly the promise the declaration makes
   longjmp(fatal_jump, 1);
}

void nandlog_port_transfer_read(uint8_t command, const void *address, uint32_t address_length, void *read_buffer, uint32_t read_length)
{
   const uint8_t *addr = (const uint8_t*)address;
   uint8_t *out = (uint8_t*)read_buffer;
   ++counters.spi_reads;
   counters.spi_read_bytes += read_length;
   data_move_open = data_move_open && (command == SIM_COMMAND_READ_STATUS_REGISTER);

   switch (command)
   {
      case SIM_COMMAND_READ_DEVICE_ID:
      {
         // Two shapes are in use: a leading dummy byte then three identity bytes, or a one-byte address
         // selecting a single identity byte. Serving both keeps one simulator usable by either driver
         for (uint32_t i = 0; i < read_length; ++i)
            out[i] = 0x00;
         if ((address_length == 1) && (read_length == 1))
            out[0] = faults.device_id[0];
         else if (read_length >= 4)
            memcpy(out + 1, faults.device_id, 3);
         else
            memcpy(out, faults.device_id, (read_length < 3) ? read_length : 3);
         break;
      }

      case SIM_COMMAND_READ_STATUS_REGISTER:
      {
         const uint8_t which = addr ? addr[0] : SIM_STATUS_REGISTER_3;
         uint8_t value = status_register[(which >> 4) - 0x0A];
         if (which == SIM_STATUS_REGISTER_3)
         {
            // The part is never busy from the host's point of view: operations complete within their call
            value &= (uint8_t)~SIM_STATUS_BUSY;
            if (faults.stay_busy)
               value |= SIM_STATUS_BUSY;
         }
         if (read_length)
            out[0] = value;
         break;
      }

      case SIM_COMMAND_READ:
      {
         // Out of the cache register. The address is a 16-bit column, most significant byte first, then a
         // dummy byte
         const uint32_t column = (addr && (address_length >= 2)) ? (((uint32_t)addr[0] << 8) | addr[1]) : 0;
         if (read_length <= 64)
            ++counters.reads_header;
         else if (read_length < page_size)
            ++counters.reads_partial;
         else if (read_length == page_size)
            ++counters.reads_page;
         else
            ++counters.reads_spare;
         memset(out, 0xFF, read_length);
         if (column < page_stride)
         {
            const uint32_t available = page_stride - column;
            memcpy(out, cache_register + column, (read_length < available) ? read_length : available);
         }
         break;
      }

      case SIM_COMMAND_READ_BBM_LUT:
      {
         // Each entry most significant byte first, with the part's "entry in use" bit set on the logical block
         memset(out, 0x00, read_length);
         for (uint32_t i = 0; (i < SIM_LUT_ENTRIES) && (((i + 1) * 4) <= read_length); ++i)
            if (lut[i].used)
            {
               const uint16_t lba = (uint16_t)(lut[i].lba | 0x8000);
               out[(i * 4) + 0] = (uint8_t)(lba >> 8);
               out[(i * 4) + 1] = (uint8_t)lba;
               out[(i * 4) + 2] = (uint8_t)(lut[i].pba >> 8);
               out[(i * 4) + 3] = (uint8_t)lut[i].pba;
            }
         break;
      }

      default:
         memset(out, 0xFF, read_length);
         break;
   }
}

void nandlog_port_transfer_write(uint8_t command, const void *address, uint32_t address_length, const void *write_buffer, uint32_t write_length)
{
   const uint8_t *addr = (const uint8_t*)address;
   const uint8_t *in = (const uint8_t*)write_buffer;
   ++counters.spi_writes;
   counters.spi_write_bytes += write_length;
   const bool in_data_move = data_move_open;
   data_move_open = (command == SIM_COMMAND_PAGE_DATA_READ) || (in_data_move && (command == SIM_COMMAND_PROGRAM_LOAD_RANDOM_DATA));

   switch (command)
   {
      case SIM_COMMAND_WRITE_STATUS_REGISTER:
         if (addr && write_length)
            status_register[(addr[0] >> 4) - 0x0A] = in[0];
         break;

      case SIM_COMMAND_WRITE_ENABLE:
         write_latch = true;
         break;

      case SIM_COMMAND_WRITE_DISABLE:
         write_latch = false;
         break;

      case SIM_COMMAND_PAGE_DATA_READ:
         latch_page(address_from(in));
         break;

      case SIM_COMMAND_PROGRAM_DATA_LOAD:
      {
         // The load resets the cache register from the column it names, most significant byte first, to the end
         // of the page and spare area, then fills it from that column. The bytes before the column keep whatever
         // the last read or load left there, as on the Alliance part: a load that does not start at column 0
         // programs them too. The Winbond datasheet promises more, but nothing should rely on it
         const uint32_t column = (addr && (address_length >= 2)) ? (((uint32_t)addr[0] << 8) | addr[1]) : 0;
         if (column < page_stride)
         {
            const uint32_t room = page_stride - column;
            memset(cache_register + column, 0xFF, room);
            memcpy(cache_register + column, in, (write_length < room) ? write_length : room);
         }
         break;
      }

      case SIM_COMMAND_PROGRAM_LOAD_RANDOM_DATA:
      {
         // Bytes from the column it names, the rest of the cache register kept: nothing is reset. Only in an
         // internal data move; anywhere else the part is not described as taking it, so it takes nothing here
         const uint32_t column = (addr && (address_length >= 2)) ? (((uint32_t)addr[0] << 8) | addr[1]) : 0;
         if (!in_data_move)
         {
            ++counters.random_loads_refused;
            fprintf(stderr, "SIM: PROGRAM LOAD RANDOM DATA (84h) outside an internal data move refused\n");
         }
         else if (column < page_stride)
            memcpy(cache_register + column, in, (write_length < (page_stride - column)) ? write_length : (page_stride - column));
         break;
      }

      case SIM_COMMAND_PROGRAM_EXECUTE:
         program_page(address_from(in));
         break;

      case SIM_COMMAND_BLOCK_ERASE:
         erase_block(address_from(in));
         break;

      case SIM_COMMAND_WRITE_BBM_LUT:
         write_lut_entry(in, write_length);
         break;

      case SIM_COMMAND_DEVICE_RESET:
         write_latch = false;
         break;

      default:
         (void)address_length;
         break;
   }
}
