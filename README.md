# nandlog

This library enables append-only, CRC-validated logging on raw SPI NAND, built to keep every committed record
across a power cut. No flash translation layer, no filesystem, no heap.

nandlog is for devices that collect data for weeks and hand it over in one go, such as a battery-powered sensor, a
tag, or a logger. Writes are small and frequent, reads are rare and bulk, and power can fail at any instant.

- **Static allocation only.** About 16 KB of RAM on a 4 KB-page part, fixed at compile time.
- **Durable once committed.** Once the call that commits a page returns, every record in that page survives a
  power cut at any later instant ([details](#what-survives-a-power-cut)).
- **Bad-block tolerant.** A block that refuses a program or an erase is retired, and its pages are moved and
  verified. The bad-block table survives a power cut at any instant, and if its reserve fails, it moves into the log
  region or falls back to using the blocks' own marks.
- **Fast recovery.** On the bench, a warm boot of an 8 Gbit Alliance part takes 82–89 ms, and the search for the
  write head stays correct even when the epoch contains retired blocks, unreadable pages, or torn writes.
- **Bulk offload.** Time-bounded reads, a framed wire format with a CRC on every page, and per-page retransmission.
- **Portable.** Add three files, write thirteen port functions, and use one of two supplied chip drivers.

📖 **[Full documentation](https://hedgetechllc.github.io/nandlog/)** · 📄 Whitepaper *(arXiv link to follow)*

---

## Install

```sh
git submodule add https://github.com/hedgetechllc/nandlog.git external/nandlog
```

There is no build system. Add three files to your project:

| File | Notes |
|---|---|
| `nandlog.c` | always |
| `chips/nandlog_chip_<PART>.c` | exactly one, for your specific chip part |
| your `nandlog_port.c` | copy `nandlog_port.c` from the repository root and fill in the stubs |

Put the repository root and `chips/` on the include path, and build with **`-fno-strict-aliasing`**, which the
library requires.

**Requirements:** a C99 compiler, a little-endian target, a SPI NAND part with a power-of-two number of pages per
block, and about 16 KB of RAM. No RTOS, heap, or filesystem required.

## Supported parts

| Driver | Part | Page | Block | Array |
|---|---|---|---|---|
| `chips/nandlog_chip_AS5F18G04SND.c` | Alliance Memory, 8 Gbit | 4096 + 256 B | 64 pages | 4096 blocks |
| `chips/nandlog_chip_W25N01GWZEIG.c` | Winbond, 1 Gbit | 2048 + 64 B | 64 pages | 1024 blocks |

Any other part needs a new driver, which amounts to adding one `.c` file. See [Adding a chip](#adding-a-chip).

## Quick start

```c
#include "nandlog.h"

// Your own types. The log stores both as opaque bytes and never interprets them
typedef struct { uint32_t device_id, start_time; } my_details_t;  // describes an epoch, up to 512 bytes by default
typedef struct { int16_t x, y, z; } my_sample_t;                  // the payload of one record
#define RECORD_SAMPLE  1                                          // a record type of your choosing, 0 to 255

my_details_t my_details = { .device_id = 42, .start_time = 1767225600 };
my_sample_t sample = { .x = 12, .y = -3, .z = 980 };
uint32_t milliseconds = 0;                                        // any clock you like; the log only compares it

// Check whether the part is present and answers
if (!nandlog_init())
   return;

// Begin an epoch (one generation of the log). Until a part has one, records are refused (NANDLOG_ERR_NO_EPOCH)
nandlog_begin_session();
nandlog_begin_epoch(&my_details, sizeof(my_details));
nandlog_end_session();

// Log. A page is committed when the next record will not fit. Anything but NANDLOG_OK says why it was not stored
if (nandlog_store_record(RECORD_SAMPLE, milliseconds, &sample, sizeof(sample)) != NANDLOG_OK)
   ; // too large, log full, no epoch yet, a read open, ...

// Commit a partial page, on a timer or at shutdown. An idle nandlog_flush(false) erases ahead of the write head
nandlog_flush(true);
```

Reading it back:

```c
uint8_t page[NANDLOG_MAX_DATA_BYTES_PER_PAGE];
uint32_t num_pages = 0;

nandlog_begin_session();
nandlog_begin_reading(0, 0);                 // a time window; 0 for either bound means "no bound"
nandlog_read_span(&num_pages, NULL);         // pass NULL unless you need an exact byte total

for (uint32_t i = 0; i < num_pages; ++i)
{
   nandlog_page_header_t header;
   uint32_t length = nandlog_retrieve_next_page(page, &header);
   if (length == 0)
      ; // an unreadable page or a tombstone: a gap, not the end. header.seq identifies it
}

nandlog_end_reading();
nandlog_end_session();
```

A **session** keeps the part powered across a run of operations. Reading and beginning an epoch require one, but
logging does not. An **epoch** is one generation of the log, and a read covers one epoch at a time.

Every record carries a one-byte type and a 32-bit **timestamp** that you supply. The log reads no clock and never
interprets the timestamp, so its units and starting point are yours (for example, milliseconds since boot or Unix
seconds). Each page header records the smallest and largest timestamps in its page, and `nandlog_begin_reading()`
uses them to select a time window. A record whose timestamp steps back by more than
`NANDLOG_TIMESTAMP_TOLERANCE_MS` commits the current page first, so a clock that resets or resynchronizes never
mixes two time bases in one page. A record with no meaningful time can carry `NANDLOG_NO_TIMESTAMP`.

While a read is open, a record that would commit a page is refused with `NANDLOG_ERR_READING`. To keep logging
through a long download, take the read's first `header.seq` and page count, close it, and fetch each page with
`nandlog_retrieve_page_by_seq()`, which needs no open read
([details](https://hedgetechllc.github.io/nandlog/api.html#long-download)).

## What survives a power cut

A record is **durable** once the call that commits its page returns: a `nandlog_store_record()` whose record
pushed the previous page out, or a `nandlog_flush(true)`, `nandlog_begin_epoch()`, or `nandlog_begin_reading()`.
After a power cut at any instant, even a second one during the reboot, every durable record is delivered intact,
and a page the power cut tore reads as a gap. What a read has served under a sequence number is what that number
holds from then on, or a gap. Records still in RAM are lost, as at any reset, so flush on a timer.

The main exceptions:

- A page that a power cut leaves erased in every bit looks unwritten, so the boot programs it again. In the log
  region this happens only at the write head, where `NANDLOG_PASS_OVER_HEAD_AT_BOOT` rules it out.
- A torn page whose header, payload, and ECC all check is taken for a committed one.
- An epoch whose metadata can no longer be verified, or that has passed over too many torn pages, is
  **sealed**: it stays readable but refuses records until you begin a new epoch.
- On the Winbond part, logging stops when a block fails after the part's 20-entry remap table has filled.

[Durability](https://hedgetechllc.github.io/nandlog/durability.html) has the full contract, its limits, and the
evidence.

## Configuration

Edit `nandlog_conf.h`, which holds every setting with its default. Leaving out a required setting is a compile
error that names it. The [configuration reference](https://hedgetechllc.github.io/nandlog/getting-started.html#configure)
explains each setting in full.

| Setting | Default | Meaning |
|---|---|---|
| `NANDLOG_HAS_HARDWARE` | 1 | 0 compiles the library to no-op stand-ins |
| `NANDLOG_MAX_PAGE_SIZE_BYTES` | 4096 | largest page you will spend RAM on |
| `NANDLOG_MAX_SPARE_SIZE_BYTES` | 256 | largest spare area you will spend RAM on |
| `NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL` | 3 | erase attempts before a block is retired |
| `NANDLOG_ERASE_AHEAD_BLOCKS` | 2 | blocks kept erased ahead of the write head |
| `NANDLOG_PAGE_PLACEMENT_ATTEMPTS` | 3 | blocks one page write may try before it gives up |
| `NANDLOG_MAX_EPOCH_DETAILS_BYTES` | 512 | largest caller-defined blob per epoch |
| `NANDLOG_TIMESTAMP_TOLERANCE_MS` | 250 | backward time step a page absorbs without being committed |
| `NANDLOG_BOOT_SALT` | 0 | a per-boot value that varies the head search's first probes |
| `NANDLOG_RECORD_FRAMING` | 1 | each record carries its own length |
| `NANDLOG_BUSY_POLL_INTERVAL_US` | 10 | wait between polls of the part's BUSY bit |
| `NANDLOG_BUSY_TIMEOUT_POLLS` | 50000 | polls before the part is declared dead |
| `NANDLOG_CHIP_PAGE_COPY` | 1 | relocate pages inside the chip, not across the bus |
| `NANDLOG_PORT_CRC32` | 0 | the port computes the CRC-32, e.g., on a hardware engine |
| `NANDLOG_CRC_TABLE_IN_RAM` | 1 | keep the software CRC's 1 KB table in RAM |
| `NANDLOG_CRC_SLICE_BY_4` | 0 | a CRC about twice as fast, for 3 KB more table |
| `NANDLOG_LOG_LEVEL` | 3 | messages that reach `nandlog_port_log()`, from 0 (none) to 3 (all) |
| `NANDLOG_PASS_OVER_HEAD_AT_BOOT` | 0 | a boot passes over the head's page instead of programming it |

Buffers are sized from the two `MAX_` values, so setting them larger than your part needs costs only unused RAM. A
part whose page exceeds the budget is a compile error.

## Porting

Copy `nandlog_port.c` from the repository root, delete its `#error` line, and implement its thirteen functions:

| | |
|---|---|
| `nandlog_port_init` / `_deinit` | bring the SPI peripheral and pins up and down |
| `nandlog_port_transfer_read` / `_write` | one command byte, *n* address bytes, then data, as one exchange |
| `nandlog_port_lock` / `_unlock` | a mutex, or empty on a single-threaded system |
| `nandlog_port_write_enable` | gate the write-protect pin |
| `nandlog_port_power` | wake or sleep the peripheral |
| `nandlog_port_log` | `printf`-style diagnostic sink, filtered by `NANDLOG_LOG_LEVEL` |
| `nandlog_port_delay_us` / `_delay_ms` | busy-wait delays |
| `nandlog_port_fatal` | the part stopped answering: reset or halt, and **never return** |
| `nandlog_port_unwritable` | the log stopped taking records: not urgent, so do not reset |

Each stub documents what the library assumes of it. The
[porting guide](https://hedgetechllc.github.io/nandlog/porting.html) has a complete port for an Ambiq Apollo4 under
FreeRTOS.

A host with a CRC engine can take over the log's CRC-32: set `NANDLOG_PORT_CRC32` and implement a fourteenth
function, `nandlog_port_crc32()`, which must return exactly what Python's `zlib.crc32(data, crc)` does. A wrong CRC
makes its page read as damaged, so check the engine against `nandlog_crc32_software()` before trusting it, as the
[porting guide](https://hedgetechllc.github.io/nandlog/porting.html#crc) shows.

## Adding a chip

A driver is one `.c` file under `chips/`. Declare the geometry, include the common header, and implement fifteen
functions:

```c
#define NANDLOG_CHIP_NAME                    "AS5F18G04SND"
#define NANDLOG_CHIP_PAGE_SIZE_BYTES         4096
#define NANDLOG_CHIP_SPARE_SIZE_BYTES        256
#define NANDLOG_CHIP_PAGES_PER_BLOCK         64     // must be a power of two
#define NANDLOG_CHIP_BLOCK_COUNT             4096
#define NANDLOG_CHIP_RESERVED_BLOCKS         16     // the bad-block table's blocks and on-device test scratch
#include "nandlog_chip_common.h"
```

| Function | Does |
|---|---|
| `nandlog_chip_geometry` | return the geometry declared above |
| `nandlog_chip_probe` | confirm the part is present and answering |
| `nandlog_chip_init` | configure it, and load or build the bad-block table |
| `nandlog_chip_low_power` | enter or leave the lowest-power state |
| `nandlog_chip_read_page` | read a whole page, reporting the part's ECC verdict |
| `nandlog_chip_read_page_region` | read part of a page, at a column address |
| `nandlog_chip_page_is_blank` | is every bit erased, read with ECC bypassed? |
| `nandlog_chip_write_page` | program a whole page once, never retried |
| `nandlog_chip_erase_block` | erase the block containing a page |
| `nandlog_chip_copy_page` | relocate a page inside the chip |
| `nandlog_chip_is_bad_block` | is this block retired? |
| `nandlog_chip_mark_bad_block` | retire a block, persistently and atomically, and say if it took |
| `nandlog_chip_table_blocks_wanted` | log-region blocks the table needs, if it has nowhere else |
| `nandlog_chip_table_take_blocks` | take those blocks for the table and retire them |
| `nandlog_chip_reset_bad_blocks` | recovery utility: discard the persisted table |

The common header supplies `NANDLOG_CHIP_GEOMETRY_INITIALIZER` for the geometry, and a part with no internal
data-move command expands `NANDLOG_CHIP_NO_INTERNAL_PAGE_COPY` instead of writing `copy_page`. Give
`read_page_region` a real column address, because reading the whole page and copying a slice gives up most of the
library's read performance. The [chip driver guide](https://hedgetechllc.github.io/nandlog/chips.html) walks
through a driver.

## API summary

| | |
|---|---|
| **Lifecycle** | `nandlog_probe` `nandlog_init` `nandlog_deinit` `nandlog_disable` `nandlog_data_bytes_per_page` |
| **Epochs** | `nandlog_begin_epoch` `nandlog_retrieve_epoch_details` `nandlog_epoch_count` `nandlog_epoch_info` `nandlog_select_epoch` `nandlog_select_current_epoch` |
| **Writing** | `nandlog_store_record` `nandlog_flush` `nandlog_has_buffered_data` |
| **Sessions** | `nandlog_begin_session` `nandlog_end_session` |
| **Reading** | `nandlog_begin_reading` `nandlog_end_reading` `nandlog_read_span` `nandlog_retrieve_next_page` `nandlog_retrieve_page_by_seq` `nandlog_read_recent_page` `nandlog_framed_next_record` `nandlog_crc32` |
| **Retransmission** | `nandlog_retransmit_clear` `nandlog_retransmit_add` `nandlog_retransmit_count` `nandlog_retransmit_total_bytes` `nandlog_retrieve_retransmit_page` |
| **Diagnostics** | `nandlog_bad_block_count` `nandlog_get_stats` |
| **Recovery** | `nandlog_reset_bad_block_table` |

Every function is documented in [nandlog.h](nandlog.h) and in the
[API reference](https://hedgetechllc.github.io/nandlog/api.html).

## Tools

`tools/nandlog_parse.py` reads a raw image of the part or an offload stream and verifies every CRC. In an image, it
also recognizes tombstones, the stale copies a relocation leaves in a retired block, and the positions each
epoch's metadata slot lists as passed over:

```sh
./tools/nandlog_parse.py image  dump.bin --page-size 4096 --spare-size 256
./tools/nandlog_parse.py stream offload.bin --json
```

## Testing

```sh
cd sim && make            # suite and regression tests, five configurations, ASan and UBSan
cd sim && make campaign   # crash-point campaign and recovery sweeps, four builds
cd sim && make fuzz       # random calls, faults, and power cuts against an oracle, nine builds
cd sim && make exhaustive # every page state below the head of a toy part, every metadata write cut
cd sim && make opcounts   # flash operations and bus bytes of each call
cd sim && make windows    # regression tests at erase-ahead windows of 0 and 1, table tests at 3
cd sim && make headpass   # exhaustive metadata paths and campaign, head pass-over on
cd sim && make soak       # the on-device power-cut soak, 1,000 boots over the simulator
cd sim && make ablation   # which checks notice when one mechanism is removed
cd sim && make baselines  # Dhara and littlefs over the same simulated part
```

The simulator runs the real log and chip drivers over an emulated part, because the port interface sits at the SPI
command level. The emulated part injects failed programs and erases, ECC verdicts, power cuts under four models of
a torn operation, and the drift of weakly programmed pages back toward erased. `tests/` holds the applications
that run on real hardware ([tests/README.md](tests/README.md)). The
[testing guide](https://hedgetechllc.github.io/nandlog/testing.html) covers both.

## Versioning

```c
#if (NANDLOG_VERSION_MAJOR != 1)
#error "This application was written for major version 1 of nandlog"
#endif
```

This is version 1.0. The major number changes when something you built against must change, and the minor number
changes for additions, which `NANDLOG_VERSION_AT_LEAST(major, minor)` tests for. A change to the documentation
alone, including this README, does not change the version. `NANDLOG_FORMAT_VERSION` versions what the library writes
on the part, separately from the library itself. It is 1, and every metadata slot records it.

## License

MIT. See [LICENSE](LICENSE).
