# nandlog

Append-only, power-fail-safe, CRC-validated logging directly onto raw SPI NAND, with no flash translation
layer.

It is built for devices that collect data for weeks and hand it over in one go: a battery-powered sensor, a
tag, a logger. It assumes writes are small and frequent, reads are rare and bulk, and power can be lost at
any instant without warning. It does not assume a filesystem, a heap, an RTOS, or that the part is healthy.

Everything it allocates is static and sized at compile time. On a 4 KB-page part the whole library costs
about 18 kB of RAM and no dynamic allocation at all.

**Contents** — [What it does](#what-it-does-and-does-not-do) · [Getting it](#getting-it) ·
[Layering](#layering) · [Setting it up](#setting-it-up) · [A worked port](#a-worked-port) ·
[Using it](#using-it) · [On-flash format](#on-flash-format) · [Offload](#offload) ·
[Cost and complexity](#cost-and-complexity) · [Failure](#failure-and-what-is-done-about-it) ·
[Versioning](#versioning) · [Testing](#testing) · [Adding a part](#adding-a-part)


## What it does and does not do

**Does.** Stores opaque records. Survives power loss at any point, including part-way through a program or
an erase. Detects corruption rather than serving it. Retires blocks the part refuses to write or erase, and
carries on. Recovers the write head after a reboot without a scan of the whole array. Serves bulk reads
bounded by timestamp, and re-serves individual pages a host reports it lost. Relocates a page inside the
chip, without putting it on the bus, where the part can do that.

**Does not.** Interpret what is in a record — a record's length is a property of whoever wrote it, and the
log never parses one. Read a clock; timestamps are supplied, compared, and never interpreted. Allocate.
Wear-level beyond sweeping the head forward. Provide random-access update: a page is written once and never
revised.


## Getting it

```
git clone https://github.com/hedgetechllc/nandlog.git
```

or, more usually, as a submodule of the firmware that will use it:

```
git submodule add https://github.com/hedgetechllc/nandlog.git external/nandlog
```

There is no build system, and nothing to install. Add three source files to your build, as you would FatFS:

| file | what it is |
|---|---|
| `nandlog.c` | the log core. Always compiled |
| `chips/nandlog_chip_<PART>.c` | exactly one, for the part you have fitted |
| your `nandlog_port.c` | the only file you write. `nandlog_port.c` in the repo root is the template |

Put the repository root and `chips/` on the include path. The public header is `nandlog.h`; `nandlog_chip.h`
and `nandlog_port.h` are the two interfaces you implement against, and `nandlog_conf.h` is the one file you
edit rather than write.

**Build with `-fno-strict-aliasing`.** This is not optional. The log reads page and metadata headers out of
a byte buffer by pointer cast, which is a strict-aliasing violation an optimiser is entitled to reason
around. It costs nothing measurable and removes a class of miscompilation that only appears at higher
optimisation levels. `-O2` is otherwise fine and worth having: on the project this came from, moving from
`-O0` to `-O2` took `nandlog.o` from 3362 to 1990 instructions.

Licensed MIT. See [LICENSE](LICENSE).


## Layering

Four seams, each of which exists because something concrete had to cross it.

```
        application
    ─────────────────────────  nandlog.h        the log's API: records in, pages out
        nandlog.c              the log core: framing, CRCs, epochs, the write head, bad-block policy
    ─────────────────────────  nandlog_chip.h   eleven functions: what the log needs from a part
        chips/nandlog_chip_<PART>.c             one self-contained driver per part
    ─────────────────────────  nandlog_port.h   thirteen functions: transfers, init, lock, power, delays, faults
        your nandlog_port.c    the only file you write
```

`nandlog_conf.h` sits beside all of it.

**The chip seam is at the SPI command level, not the page level.** That is deliberate, and it is what makes
the host simulator worth having: `sim/nandlog_port_sim.c` emulates a NAND part, and the real chip driver
runs on top of it unmodified. Addressing, status-register handshakes, busy-waiting and bad-block bookkeeping
are all shipping code under test, not a stub that agrees with itself.


## Setting it up

### 1. Edit `nandlog_conf.h`

Nothing is derived and nothing has a silent default — anything missing is a named compile error from
`nandlog_chip.h`.

| setting | meaning |
|---|---|
| `NANDLOG_HAS_HARDWARE` | whether a part is fitted at all. When it is not, the library compiles to stand-ins that accept everything and return nothing, so the application above needs no conditionals of its own |
| `NANDLOG_MAX_PAGE_SIZE_BYTES` | the largest page you will spend RAM on. Every buffer in the library is sized from this |
| `NANDLOG_MAX_SPARE_SIZE_BYTES` | the same for the spare area |
| `NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL` | program or erase attempts on one block before it is retired |
| `NANDLOG_ERASE_AHEAD_BLOCKS` | blocks kept erased ahead of the write head, so a page write never waits on an erase |
| `NANDLOG_PAGE_PLACEMENT_ATTEMPTS` | blocks one page write may relocate through before the log concludes the part will not hold it |
| `NANDLOG_MAX_METADATA_BYTES` | largest caller-defined blob stored alongside the log |
| `NANDLOG_TIMESTAMP_TOLERANCE_MS` | how far a timestamp may step backwards before it is treated as the time base having moved rather than as writer disagreement |
| `NANDLOG_RECORD_FRAMING` | whether each record carries its own length. **On by default** |
| `NANDLOG_BUSY_POLL_INTERVAL_US` | how often to poll the part's BUSY bit |
| `NANDLOG_BUSY_TIMEOUT_MS` | how long to poll it before declaring the part dead |
| `NANDLOG_CHIP_PAGE_COPY` | whether a relocated page may be moved inside the chip instead of across the bus. On by default |

A part whose page exceeds the RAM budget is a compile error inside that part's driver, so the only cost of
setting the two size values larger than the fitted part needs is unused RAM.

### 2. Write `nandlog_port.c`

Copy the template from the repository root into your own tree, delete its `#error` line, and fill in
thirteen functions: two SPI transfers, init and deinit, a lock pair, a write-protect pin, power, two delays,
a log sink, and two ways of saying the part has failed. Each stub states what the layer above assumes of it.
Three of them are worth reading twice:

- **`nandlog_port_lock()` / `nandlog_port_unlock()`** serialise the library against itself. Every public
  function takes the lock for its whole duration and does its work in a `_locked` internal, so a caller
  never holds it and these never nest. A single-threaded host can leave both empty; anything else must
  supply a real mutex. The log has one page cache and one transfer buffer, and — more sharply — reading a
  page is a *sequence* of commands against the one cache register inside the part: latch the page, then
  clock it out. Two callers interleaving those sequences read each other's pages, and the bus shows nothing
  wrong.
- **`nandlog_port_fatal()`** is called when the part stops answering at all. A library has no business
  resetting the system it is embedded in, so what happens next is yours.
- **`nandlog_port_unwritable()`** is the softer failure: the part still answers, but no block will retain a
  page. The log has already disabled itself and stopped trying. Nothing here is urgent and nothing here
  should reset the system — it is still running, it just has nowhere to put data.

### 3. Pick a chip driver

Two are supplied, and each is complete on its own:

| driver | part | page | block | array | reserve |
|---|---|---|---|---|---|
| `chips/nandlog_chip_AS5F18G04SND.c` | Alliance Memory, 8 Gbit | 4096 + 256 B | 64 pages | 4096 blocks | 80 blocks, bad-block table in a marker page |
| `chips/nandlog_chip_W25N01GWZEIG.c` | Winbond, 1 Gbit | 2048 + 64 B | 64 pages | 1024 blocks | 40 blocks, remapped by the chip's own LUT |

If yours is not there, see [Adding a part](#adding-a-part). It is one new `.c` file.


## A worked port

Here is most of a real `nandlog_port.c`, for an Ambiq Apollo4 running FreeRTOS, using the Ambiq HAL's IOM
peripheral. It is shown to make the shape of the contract concrete; the pin names, the `print()` sink and
the `system_*` calls are the surrounding firmware's, not the library's.

```c
#include "nandlog_port.h"
#include "logging.h"      // the firmware's print() sink
#include "system.h"       // PIN_STORAGE_*, system_reset(), system_record_diagnostic()

static void *spi_handle;
static SemaphoreHandle_t log_mutex;
static StaticSemaphore_t log_mutex_buffer;

bool nandlog_port_init(void)
{
   const am_hal_iom_config_t spi_config = {
      .eInterfaceMode = AM_HAL_IOM_SPI_MODE,
      .ui32ClockFreq  = AM_HAL_IOM_48MHZ,
      .eSpiMode       = AM_HAL_IOM_SPI_MODE_0
   };

   // Configure and assert the write-protect and hold pins to disable them
   am_hal_gpio_pinconfig(PIN_STORAGE_WRITE_PROTECT, am_hal_gpio_pincfg_output);
   am_hal_gpio_output_set(PIN_STORAGE_WRITE_PROTECT);
   am_hal_gpio_pinconfig(PIN_STORAGE_HOLD, am_hal_gpio_pincfg_output);
   am_hal_gpio_output_set(PIN_STORAGE_HOLD);

   // Bring up the SPI module and its four pins
   am_hal_iom_initialize(STORAGE_SPI_NUMBER, &spi_handle);
   am_hal_gpio_pinconfig(PIN_STORAGE_SPI_SCK, sck_config);
   am_hal_gpio_pinconfig(PIN_STORAGE_SPI_MISO, miso_config);
   am_hal_gpio_pinconfig(PIN_STORAGE_SPI_MOSI, mosi_config);
   am_hal_gpio_pinconfig(PIN_STORAGE_SPI_CS, cs_config);
   am_hal_iom_power_ctrl(spi_handle, AM_HAL_SYSCTRL_WAKE, false);
   am_hal_iom_configure(spi_handle, &spi_config);
   am_hal_iom_enable(spi_handle);
   return true;
}

void nandlog_port_deinit(void)
{
   am_hal_iom_disable(spi_handle);
   am_hal_iom_uninitialize(spi_handle);
   spi_handle = NULL;
}
```

The two transfers are the substance of the port. The command and address go out as one transaction with
`bContinue` set, so chip-select stays asserted; the data then follows, split across as many transactions as
the peripheral's maximum allows. Note that the transaction count is fixed **before** the loop rather than
tested against the remaining length, so a zero-length read still issues the single empty transaction that
the smaller-page parts rely on to complete a command:

```c
void nandlog_port_spi_read(uint8_t command, const void *address, uint32_t address_length,
                           void *read_buffer, uint32_t read_length)
{
   uint32_t instruction = command, retries_remaining = 4;
   uint32_t num_reads = 1 + (read_length / (1 + AM_HAL_IOM_MAX_TXNSIZE_SPI));
   memcpy(((uint8_t*)&instruction) + 1, address, address_length);
   am_hal_iom_transfer_t spi_transaction = {
      .eDirection    = AM_HAL_IOM_TX,
      .ui32NumBytes  = 1 + address_length,
      .pui32TxBuffer = &instruction,
      .bContinue     = true,
      .ui8Priority   = 1
   };

   // A transfer either completes or does not return
   while (--retries_remaining && (am_hal_iom_blocking_transfer(spi_handle, &spi_transaction) != AM_HAL_STATUS_SUCCESS))
      am_hal_delay_us(10);
   if (!retries_remaining)
      system_reset(true);

   uint32_t read_offset = 0;
   while (num_reads--)
   {
      uint32_t read_bytes = (read_length > AM_HAL_IOM_MAX_TXNSIZE_SPI) ? AM_HAL_IOM_MAX_TXNSIZE_SPI : read_length;
      read_length -= read_bytes;

      retries_remaining = 4;
      spi_transaction.eDirection    = AM_HAL_IOM_RX;
      spi_transaction.ui32NumBytes  = read_bytes;
      spi_transaction.pui32TxBuffer = NULL;
      spi_transaction.pui32RxBuffer = (uint32_t*)((uint8_t*)read_buffer + read_offset);
      spi_transaction.bContinue     = read_length > 0;
      read_offset += read_bytes;

      while (--retries_remaining && (am_hal_iom_blocking_transfer(spi_handle, &spi_transaction) != AM_HAL_STATUS_SUCCESS))
         am_hal_delay_us(10);
      if (!retries_remaining)
         system_reset(true);
   }
}

// nandlog_port_spi_write() is this function with the directions reversed, and splits the payload the same
// way for the same reason
```

The lock is the part most likely to be got wrong. Before the scheduler runs there is exactly one context, so
there is nothing to serialise against — and taking a FreeRTOS mutex there, or from an interrupt, is an error
rather than a wait. The mutex is created on first use, which is guaranteed to be after the scheduler has
started, and the double check under a critical section makes the creation itself safe against two tasks
arriving together:

```c
static inline bool locking_applies(void)
{
   return (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) && !xPortIsInsideInterrupt();
}

void nandlog_port_lock(void)
{
   if (locking_applies())
   {
      if (!log_mutex)
      {
         taskENTER_CRITICAL();
         if (!log_mutex)
            log_mutex = xSemaphoreCreateMutexStatic(&log_mutex_buffer);
         taskEXIT_CRITICAL();
      }
      if (log_mutex)
         xSemaphoreTake(log_mutex, portMAX_DELAY);
   }
}

void nandlog_port_unlock(void)
{
   if (log_mutex && locking_applies())
      xSemaphoreGive(log_mutex);
}
```

The rest is short. Note that on this board the write-protect pin is asserted **high** to permit programming;
which level that is depends on the board, not on the part, and getting it backwards makes every program
fail silently:

```c
void nandlog_port_write_enable(bool enable)
{
   if (enable)
      am_hal_gpio_output_set(PIN_STORAGE_WRITE_PROTECT);
   else
      am_hal_gpio_output_clear(PIN_STORAGE_WRITE_PROTECT);
}

void nandlog_port_power(bool awake)
{
   am_hal_iom_power_ctrl(spi_handle, awake ? AM_HAL_SYSCTRL_WAKE : AM_HAL_SYSCTRL_DEEPSLEEP, true);
}

void nandlog_port_delay_us(uint32_t microseconds) { am_hal_delay_us(microseconds); }
void nandlog_port_delay_ms(uint32_t milliseconds) { am_util_delay_ms(milliseconds); }

void nandlog_port_log(const char *format, ...)
{
   char message[160];
   va_list args;
   va_start(args, format);
   vsnprintf(message, sizeof(message), format, args);
   va_end(args);
   print("%s", message);
}

void nandlog_port_fatal(const char *reason)
{
   print("ERROR: Storage hardware fault: %s; resetting\n", reason);
   system_record_diagnostic(RESET_DIAGNOSTIC_STORAGE_FATAL);
   system_reset(true);
}

void nandlog_port_unwritable(void)
{
   // Not a reason to reset: the device can still range and still serve BLE. Leave a breadcrumb so that the
   // next boot's log says the flash has already given up
   system_record_diagnostic(RESET_DIAGNOSTIC_STORAGE_UNWRITABLE);
}
```


## Using it

### Coming up

```c
#include "nandlog.h"

if (!nandlog_init())
   return;   // no part, or the port would not open; no other call should be made
```

`nandlog_init()` opens the port, identifies the part, loads its bad-block table, recovers the current epoch
from the metadata ring and locates the write head within it. It is safe to call more than once.
`nandlog_probe()` is the cheaper question for a build that only wants to know whether a part is fitted.

### Starting an experiment

Every generation of the log is an *epoch*. Beginning one writes a fresh metadata slot describing it;
everything logged previously stays on the part but stops being reachable, because reads only ever cover the
current epoch. The blob is opaque — the log neither reads nor interprets it.

```c
nandlog_enter_maintenance_mode();
nandlog_store_metadata(&experiment_details, sizeof(experiment_details));
nandlog_exit_maintenance_mode();
```

A *maintenance session* holds the part powered across a run of operations instead of waking and sleeping it
around each one. Reading and storing metadata both require one; ordinary logging does not. Sessions do not
nest.

### Logging

```c
nandlog_store_record(RECORD_RANGE, milliseconds_since_start, &range, sizeof(range));
```

`record_type` and `data` are opaque. The timestamp is only ever compared, never interpreted, so its epoch
and its units are yours; `NANDLOG_TIMESTAMP_TOLERANCE_MS` decides how far it may step backwards before the
log treats that as the time base having moved rather than as two writers disagreeing.

This call is cheap, but it is not free every time: a page is committed here as soon as the next record will
not fit, so it occasionally writes to flash. Records are never split across pages, and one larger than a
page is dropped rather than corrupting the stream. `nandlog_data_bytes_per_page()` is how much a page will
hold on the fitted part.

A partial page only reaches flash when you ask for it, which is what a timed flush and a shutdown path are
for:

```c
nandlog_flush(true);                       // commit whatever is buffered
if (nandlog_has_buffered_data()) { ... }   // whether anything is sitting unwritten in RAM
```

`nandlog_disable(true)` stops the log accepting records — it discards what it is given rather than buffering
it, and still serves reads.

### Reading it back

```c
nandlog_enter_maintenance_mode();
nandlog_begin_reading(start_timestamp, end_timestamp);   // zero for either bound means "no bound"

uint32_t num_pages = 0, num_bytes = 0;
nandlog_read_span(&num_pages, &num_bytes);               // pass NULL for num_bytes unless you need it

for (uint32_t i = 0; i < num_pages; ++i)
{
   nandlog_page_header_t header;
   const uint32_t length = nandlog_retrieve_next_page(page_buffer, &header);
   // length == 0 means that page was unreadable. That is a gap, not the end of the read: header.seq
   // still identifies it, so a host can tell "still missing" from "never answered"
}

nandlog_end_reading();
nandlog_exit_maintenance_mode();   // implies nandlog_end_reading()
```

While a read is open, writing is refused. Both time bounds are resolved inside `nandlog_begin_reading()`,
deliberately: an earlier version resolved one and silently discarded the other, and a date-bounded download
returned four days of data instead of two while reporting no loss.

`nandlog_read_span()` is exact in both directions — it verifies payload CRCs as well as headers, so a page
with a sound header and a rotted payload is not counted and then not delivered. A host sizing a receive
buffer from the figure depends on that. **It costs a read of every page in the span**, which doubles the
flash traffic of a download, so pass `NULL` unless the exact total is needed. The page count alone is
arithmetic and free.

To walk the records inside a payload, when framing is on:

```c
uint32_t offset = 0, record_bytes = 0;
const uint8_t *record = NULL;
while (nandlog_framed_next_record(payload, length, &offset, &record, &record_bytes))
{
   const uint8_t  type = record[0];
   uint32_t timestamp;  memcpy(&timestamp, record + 1, sizeof(timestamp));
   const uint8_t *data = record + 5;
   const uint32_t data_length = record_bytes - 5;
}
```

The prefixes are authoritative for stepping. A record that decodes is still advanced past by its declared
length rather than by whatever the decoder consumed, so the two cannot drift apart; and a record the reader
does not recognise is stepped over exactly, instead of costing it the rest of the page.

### Peeking at what was just written

Independently of any open read, and without a session:

```c
bool end_of_epoch = false;
for (uint32_t back = 0; !end_of_epoch; ++back)
{
   nandlog_page_header_t header;
   const uint32_t length = nandlog_read_recent_page(back, buffer, &header, &end_of_epoch);
}
```

Zero means that page could not be read, which is not the end of the log; `end_of_epoch` is the signal to
stop.

### Answering a host that lost pages

Sequence numbers are contiguous within an epoch, so a host knows exactly which pages it is missing.

```c
nandlog_retransmit_clear();
nandlog_retransmit_add(seqs, count);       // accumulates across as many calls as it takes

const uint32_t pending = nandlog_retransmit_count();
const uint32_t bytes   = nandlog_retransmit_total_bytes();   // exact; reads each requested page

for (uint32_t i = 0; i < pending; ++i)
{
   nandlog_page_header_t header;
   const uint32_t length = nandlog_retrieve_retransmit_page(i, page_buffer, &header);
   // An index past the end yields a zero-length page rather than nothing
}
```

Anything beyond `NANDLOG_MAX_RETRANSMIT_PAGES` is dropped. `nandlog_retrieve_page_by_seq()` is the same
lookup for a caller that does not need the request list.


## On-flash format

The array is divided into three regions.

| region | contents |
|---|---|
| metadata ring | the first 8 blocks: a ring of slots, each describing one epoch |
| log region | everything between the ring and the reserve |
| reserve | the top *N* blocks, held by the chip driver for bad-block management |

**Epochs.** Every generation of the log has a number. Starting one writes a metadata slot naming the epoch
and the page its sequence 0 lives at. A page carrying a stale epoch is therefore unmistakable rather than
merely unexpected, which is what makes recovery after a reboot decidable.

**Pages.** Each is self-describing and self-validating: a header with the magic, epoch, sequence number, the
timestamps of its first and last records, its payload length and record count, then a CRC over the payload
and a CRC over the header. The two checksums are separate on purpose — the header CRC lets the log trust the
length and sequence fields before it has read the payload.

**Record framing.** On by default. Each record is prefixed with its own data length as a little-endian
`uint16`, so a reader can walk a page without knowing the application's record types:

    [data length: 2][record type: 1][timestamp: 4][data]

Two bytes rather than a variable-width length because these records are usually larger than 255 bytes, which
is where a one-byte-plus-escape encoding starts costing more than it saves.

A page states which format it is in through its magic (`TTP1` opaque, `TTP2` framed), which costs nothing:
the four bytes a reader looks at first to find a page at all are the same four that tell it how to read one.
That also means a log spanning a firmware change parses correctly page by page, rather than depending on
what some other structure claims.

**Recovery.** On boot the log reads the ring, takes the highest valid epoch, and binary-searches the log
region for the first page that does not belong to it. That is the write head, found in log(n) reads rather
than a scan. The last page of the epoch supplies the sequence number to continue from.

**Erase-ahead.** A rolling window of erased blocks is kept ahead of the head, topped up from the *middle* of
each block rather than at its boundary. Triggering at the boundary put the erase in the same activation as
the page write that had just crossed it, concatenating both stalls; moving the trigger gives the erase its
own activation and restores worst-case contiguous blocking to a single page write.


## Offload

A stream is a header, the caller's metadata blob, then each page framed with its sequence number,
timestamps, length, record count and payload CRC:

```
Header:
  u32   magic              'TTS1'
  u16   format_version     1 opaque, 2 framed
  u16   details_length
  u32   total_pages
  u32   total_payload_bytes
  u8[]  caller metadata

Then, per page:
  u32   seq
  u32   first_timestamp
  u32   last_timestamp
  u16   payload_length     0 means the device could not read this page
  u16   record_count
  u32   payload_crc
  u8[]  payload
```

A page that fails its CRC on the device is still emitted, with `payload_length = 0`, so the host sees an
explicit gap at a known `seq` instead of silently missing data. The host re-verifies `payload_crc`
independently, which also catches corruption introduced in transit. On the wire a page frame carries no
magic, so the stream header's `format_version` says which record grammar is inside.

`tools/nandlog_parse.py` is the reference reader for both this and a raw image dump:

```
./nandlog_parse.py image  dump.bin --page-size 4096 --spare-size 256
./nandlog_parse.py stream offload.bin --json
```

It is a second implementation, not a convenience. The device computes CRC-32 with the IEEE 802.3 polynomial
precisely so the host can verify it with `zlib.crc32` and nothing bespoke — if the two ever disagree, one of
them is wrong, and having a reader written independently of the writer is what makes that visible.

**Resolving a time bound is an exact scan, not a binary search.** A binary search over page headers needs
the time bounds to rise with sequence number, and they do not: adopting a new time base steps the clock
backwards, the log commits a page at that point, and the search predicate stops being monotone across the
boundary. The failure was not a small one — a date-limited read starting across such a step returned *zero*
pages, because a failed search reports the write head and reading begins at the end of the log. Both bounds
err **outward**: shipping a little extra for the host to filter beats dropping data that was asked for.


## Cost and complexity

Everything below is in units of flash operations, because those dominate everything else by orders of
magnitude. Symbols:

| | |
|---|---|
| **E** | pages in the current epoch |
| **S** | pages an open read will deliver |
| **N** | pages in the log region |
| **B** | blocks in the log region |
| **M** | pages in the metadata ring (8 × pages-per-block) |
| **C** | blocks on the part |
| **r** | pending retransmission requests |

| call | flash work |
|---|---|
| `nandlog_probe()` | O(1) — one identity read, no page reads |
| `nandlog_init()`, first ever boot | **O(C) page reads** — one per block, to find the factory bad-block markers |
| `nandlog_init()`, every boot after | **O(M + log N) page reads**. The ring scan dominates and is a fixed cost |
| `nandlog_deinit()` | O(1) |
| `nandlog_data_bytes_per_page()` | none |
| `nandlog_store_record()` | none directly; amortized one page program per page's worth of records |
| — one page commit, good case | 1 program + 1 read-back |
| — one page commit, block fails | ≤ `NANDLOG_PAGE_PLACEMENT_ATTEMPTS` × (1 erase + ≤ pages-per-block relocations + an O(B) walk to the next good block) |
| — once per block written | `NANDLOG_ERASE_AHEAD_BLOCKS` erases, each after an O(B) walk |
| `nandlog_flush(true)` | one page commit |
| `nandlog_store_metadata()` | ≤ M programs worst case, 1 typically, plus 1 erase for the new log start, `NANDLOG_ERASE_AHEAD_BLOCKS` for the window, and 1 more when the ring slot lands on a block boundary |
| `nandlog_retrieve_metadata()` | 1 page read |
| `nandlog_begin_reading(0, 0)` | none |
| `nandlog_begin_reading(t, 0)` | **O(E) page reads**, stopping at the first page that reaches the bound |
| `nandlog_begin_reading(0, t)` | **O(E) page reads** — a full pass over the epoch, always |
| `nandlog_begin_reading(t0, t1)` | **O(E) page reads**, up to 2E |
| `nandlog_read_span(&pages, NULL)` | none — the page count is arithmetic |
| `nandlog_read_span(&pages, &bytes)` | **O(S) page reads** |
| `nandlog_retrieve_next_page()` | 1 page read; an O(B) walk when it steps over a retired block |
| `nandlog_retrieve_page_by_seq()` | O(log E) page reads typically; O(E log E) worst case, when long runs of pages fail validation and each probe has to scan forward for a valid one |
| `nandlog_retransmit_clear()` / `_add()` / `_count()` | none; O(count) in RAM |
| `nandlog_retransmit_total_bytes()` | **O(r log E) page reads** |
| `nandlog_retrieve_retransmit_page()` | O(log E) page reads |
| `nandlog_read_recent_page(k)` | 1 page read; O(k) arithmetic, no reads, to walk back |
| `nandlog_framed_next_record()` | none |
| `nandlog_chip_is_bad_block()` | none; O(table entries) in RAM — ≤ 256 for the Alliance part, 20 for the Winbond |

### Measured, on the simulator, with the Alliance part

A 100-page epoch on a 4096-block part, 4 KB pages:

| | page reads | bus bytes |
|---|---|---|
| first ever boot (factory bad-block scan) | 4689 | 19.3 MB |
| every boot after | 612 | 2.5 MB |
| `begin_reading(0, 0)` | 0 | — |
| `begin_reading(t, 0)`, bound 14 pages in | 14 | — |
| `begin_reading(0, t)` | 100 | — |
| `begin_reading(t0, t1)` | 114 | — |
| `read_span()` with an exact byte total, 68-page span | 67 | — |
| serving those 68 pages | 68 | — |
| `retrieve_page_by_seq()`, 100-page epoch | 2 | — |

So **a fully time-bounded download with an exact byte total reads the epoch about two and a half times**:
once for the end bound, once for the byte total over the span, once more to serve it. Dropping the end bound
and filtering on the host removes the first pass; passing `NULL` for the byte total removes the second.

The two costs worth designing around are the **metadata ring scan at every boot** — 512 full page reads on a
64-page-per-block part, regardless of how much is in the ring — and the **end-bound seek**, which is a full
pass over the epoch every time a bounded download opens.

### Relocation

Relocating a page is where a failing part spends its time, so the log asks the chip to do it. Most SPI NAND
can latch a page into its own cache register and program it straight back out to another address, so the
page never crosses the bus in either direction. Retiring one block of a 4 KB-page part and relocating the
twenty pages already written in it:

| | bus traffic |
|---|---|
| `NANDLOG_CHIP_PAGE_COPY = 1` (internal copy) | 24,878 bytes |
| `NANDLOG_CHIP_PAGE_COPY = 0` (read and write) | 188,798 bytes |

7.6× less, and what remains is not the relocation at all — it is the three full-page program attempts the
block refused before it was retired, the bad-block marker page, and the one page that actually had to be
written and read back. The relocation itself moves six bytes of address per page.

Parts differ, so the decision is the driver's: a driver for a part with no internal data move expands
`NANDLOG_CHIP_NO_INTERNAL_PAGE_COPY` and the log falls back to a read and a write, per page, with no
configuration. `NANDLOG_CHIP_PAGE_COPY = 0` takes the path out of a build entirely, which is the escape
hatch if a part turns out to mishandle it — an internal copy re-encodes ECC over whatever the source read
produced, and a part whose on-die ECC covers the spare area unexpectedly is worth proving on hardware first.

Note what an internal copy does with a source page the part cannot correct: the uncorrected bytes are what
gets programmed, and the destination is then a page with sound ECC and a payload CRC that will not match.
That is the same outcome the read-and-write path reaches by writing `0xFF` over an unreadable source. An
unreadable page stays unreadable, and the log serves it as a gap either way.

### Blocking time

Measured on an Ambiq Apollo4 Plus (Cortex-M4F) with the Alliance part on a 48 MHz SPI bus, over ≈488 page
writes and ≈12 erase-ahead activations, using the DWT cycle counter:

| operation | max observed | frequency | amortized per page |
|---|---|---|---|
| one page commit | 2855 – 2893 µs | every page | 2893 µs |
| erase-ahead (2 blocks) | 1946 – 1989 µs | every 64 pages | **30 µs** |

Erase-ahead adds ≈1 % amortized overhead, and its worst single stall is *below* the per-page cost already
paid 64× more often. These loops run in whichever task calls the log, so every one of them is bounded:

| loop | bound |
|---|---|
| walking to the next good block | one pass over the log region's blocks |
| relocating a page that will not stay written | `NANDLOG_PAGE_PLACEMENT_ATTEMPTS` blocks, then `nandlog_port_unwritable()` |
| waiting for the chip to clear BUSY | `NANDLOG_BUSY_TIMEOUT_MS`, then `nandlog_port_fatal()` |
| programming one page | `NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL` attempts before the block is retired |

This is not defensive habit. A part that has stopped accepting programs turns an unbounded search into a
spin that only a watchdog can end, and the log is then the thing that killed the device.
`NANDLOG_PAGE_PLACEMENT_ATTEMPTS` is a bound on real time as much as on blocks: each attempt costs an erase
plus a block's worth of relocations, so it has to be large enough to ride out a genuine cluster of worn
blocks and small enough that a part which has stopped programming altogether is diagnosed in seconds.

### RAM

All static, all sized at compile time, nothing allocated. Measured with `NANDLOG_MAX_PAGE_SIZE_BYTES` at
4096:

| | bytes | what it is |
|---|---|---|
| `nandlog.c` | 13,408 | `3 × NANDLOG_MAX_PAGE_SIZE_BYTES` + `4 × NANDLOG_MAX_RETRANSMIT_PAGES` + ~90 bytes of state |
| `chips/nandlog_chip_AS5F18G04SND.c` | 5,385 | one page-with-spare scratch buffer, a 256-entry bad-block table |
| `chips/nandlog_chip_W25N01GWZEIG.c` | 2,193 | one page-with-spare scratch buffer, a 20-entry LUT mirror |

≈ **18.4 kB** for the Alliance part. The dominant term is the page budget, so a build that lowers
`NANDLOG_MAX_PAGE_SIZE_BYTES` to match a 2 KB-page part saves about 6 kB.


## Failure, and what is done about it

| failure | response |
|---|---|
| program reports failure | retried; then the block is retired and any pages already written in it are relocated |
| written page does not read back with a valid header | same — the header CRC makes this exact rather than ECC-dependent |
| erase reports failure | the block is retired |
| power lost mid-program | the torn page fails its CRC on the next boot and is treated as a gap |
| power lost mid-erase | the partially erased block reads as a gap and is rewritten or retired |
| payload CRC mismatch on read | that page is reported as zero-length: a gap, not the end of the log |
| a page will not stay written anywhere | relocation is retried across `NANDLOG_PAGE_PLACEMENT_ATTEMPTS` blocks, then logging is disabled and `nandlog_port_unwritable()` is called |
| part stops responding | bounded busy-wait, then `nandlog_port_fatal()` |
| bad-block table implausible | ignored rather than acted on, and rebuilt |

The consistent rule: **a page that cannot be trusted is a gap, and a gap is not the end of the log.** Reads
step over it. Offload reports it as a zero-length page carrying its sequence number, so a host can tell
"still missing" from "never answered" and ask for it again.


## Versioning

```c
#define NANDLOG_VERSION_MAJOR    1
#define NANDLOG_VERSION_MINOR    1
```

The **major** number changes when an on-flash or wire structure changes shape. A consumer that has to care
can refuse to build rather than discover it at runtime:

```c
#if (NANDLOG_VERSION_MAJOR != 1)
#error "This application understands nandlog 1.x page and stream layouts only"
#endif
```

The **minor** number changes for anything else — added functions, policy knobs, chip drivers, behaviour that
does not move a byte on the part. `NANDLOG_VERSION_AT_LEAST(major, minor)` is the test for a feature added
in a minor release:

```c
#if NANDLOG_VERSION_AT_LEAST(1, 1)
   // nandlog_chip_copy_page() exists
#endif
```

This versions the *library*. `NANDLOG_FORMAT_VERSION` versions the *record grammar* a particular build
writes — 1 opaque, 2 framed — and the two move independently: flipping `NANDLOG_RECORD_FRAMING` changes the
format version without changing the library.


## Testing

```
cd sim && make
```

Three host builds, all of the real log and the real chip driver over a RAM-backed part: framing off, framing
on, and framing on with internal page copy disabled so the read-and-write relocation path is exercised too.
Clean under ASan and UBSan, in milliseconds.

The simulator honours NAND semantics rather than approximating them: erase sets bits, programming may only
clear them, a program cut short leaves exactly the prefix that made it, and a page read fills the chip's one
cache register — which is what makes an internal data move work there at all. It injects unwritable and
unerasable blocks, mid-operation power loss, bit rot, and a part that never clears BUSY. Getting those wrong
would make the simulator agree with a buggy log.

The suite includes a regression for the time-range seek, driven by a backward clock step taken from field
logs. It is worth knowing why: the first attempt at that fix was a bounded backward scan, and the test
written for it **passed with the fix disabled** — it exercised the end bound, which extends past a
discontinuity naturally and cannot lose data. A test that passes without the code it is testing is not a
test, and finding that out is what showed the bounded scan could never be made sound.

Host tests do not replace on-device testing. They cannot say anything about the real part's timing, its ECC,
or the board. What they cover is everything above the SPI wire, with faults that are impractical to stage on
hardware.


## Adding a part

One `.c` file under `chips/`. State the geometry, include `nandlog_chip_common.h`, and implement eleven
functions:

```c
#define NANDLOG_CHIP_NAME                    "AS5F18G04SND"
#define NANDLOG_CHIP_PAGE_SIZE_BYTES         4096
#define NANDLOG_CHIP_SPARE_SIZE_BYTES        256
#define NANDLOG_CHIP_PAGES_PER_BLOCK         64
#define NANDLOG_CHIP_BLOCK_COUNT             4096
#define NANDLOG_CHIP_RESERVED_BLOCKS         80
#include "nandlog_chip_common.h"
```

The eleven are `geometry`, `probe`, `init`, `low_power`, `read_page`, `write_page`, `erase_block`,
`copy_page`, `is_bad_block`, `mark_bad_block` and `reset_bad_blocks`. A part with no internal data move
expands `NANDLOG_CHIP_NO_INTERNAL_PAGE_COPY` for the eighth. Read either supplied driver; both are complete
on their own.

Geometry lives in the driver and nowhere else — not in a board header, not in the configuration file — so
there is no second place for it to be wrong. `nandlog_chip_common.h` holds no command codes, no register
numbers and no bit patterns, and there is no shared implementation beneath it. **Each driver carries its own
copy of the mechanics it needs, even where two parts answer the same commands**, because that agreement is a
coincidence of history rather than a contract. Duplication is the price of a file that is complete on its
own and never has to be edited in sympathy with another.


## Deliberate omissions

**Wear levelling.** The head sweeps forward and wraps. For a log that fills over weeks that is enough, and
anything cleverer would need a mapping table, which is the FTL this exists to avoid.

**Random-access update.** A page is written once and never revised. There is no way to modify a record after
it has been committed, and adding one would mean either a mapping table or read-modify-write of a whole
block.

**Reading a clock.** Timestamps are supplied by the caller and only ever compared. The log has no opinion
about what they mean.
