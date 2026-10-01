# nandlog

Append-only, power-fail-safe, CRC-validated logging on raw SPI NAND. No flash translation layer, no
filesystem, no heap.

For devices that collect data for weeks and hand it over in one go — a battery-powered sensor, a tag, a
logger — where writes are small and frequent, reads are rare and bulk, and power can be lost at any instant.

- **Static allocation only.** ~18 kB of RAM on a 4 KB-page part, fixed at compile time.
- **Power-fail safe.** A torn page fails its CRC on the next boot and is reported as a gap, never as data.
- **Bad-block tolerant.** Blocks that refuse a write or erase are retired and their contents relocated.
- **Fast recovery.** The write head is found in O(log n) reads after a reboot, not by scanning the array —
  225 ms on an 8 Gbit part.
- **Bulk offload.** Time-bounded reads, a framed wire format with per-page CRCs, and per-page retransmission.
- **Portable.** Three files to add, thirteen functions to write, two chip drivers supplied.

📖 **[Full documentation](https://hedgetechllc.github.io/nandlog/)** · 📄 Whitepaper *(arXiv link to follow)*

---

## Install

```sh
git submodule add https://github.com/hedgetechllc/nandlog.git external/nandlog
```

There is no build system. Add three files to yours:

| File | Notes |
|---|---|
| `nandlog.c` | always |
| `chips/nandlog_chip_<PART>.c` | exactly one, for your part |
| your `nandlog_port.c` | copy `nandlog_port.c` from the repo root and fill in the stubs |

Put the repository root and `chips/` on the include path, and build with **`-fno-strict-aliasing`** (required
— the log reads headers out of byte buffers by pointer cast).

**Requirements:** a C99 compiler, a SPI NAND part, and roughly 18 kB of RAM. No RTOS, heap, or filesystem.

## Supported parts

| Driver | Part | Page | Block | Array |
|---|---|---|---|---|
| `chips/nandlog_chip_AS5F18G04SND.c` | Alliance Memory, 8 Gbit | 4096 + 256 B | 64 pages | 4096 blocks |
| `chips/nandlog_chip_W25N01GWZEIG.c` | Winbond, 1 Gbit | 2048 + 64 B | 64 pages | 1024 blocks |

Anything else needs a driver — one `.c` file. See [Adding a chip](#adding-a-chip).

## Quick start

```c
#include "nandlog.h"

if (!nandlog_init())
   return;                                  // no part, or the port would not open

// Start a new run. Everything logged before this stops being reachable.
nandlog_begin_session();
nandlog_begin_epoch(&my_details, sizeof(my_details));
nandlog_end_session();

// Log. Cheap, and commits a page automatically when one fills.
nandlog_store_record(RECORD_SAMPLE, milliseconds, &sample, sizeof(sample));

// Force out a partial page — on a timer, or at shutdown.
nandlog_flush(true);
```

Reading it back:

```c
uint8_t page[NANDLOG_MAX_DATA_BYTES_PER_PAGE];
uint32_t num_pages = 0;

nandlog_begin_session();
nandlog_begin_reading(start_ms, end_ms);     // 0 for either bound means "no bound"
nandlog_read_span(&num_pages, NULL);         // pass NULL unless you need an exact byte total

for (uint32_t i = 0; i < num_pages; ++i)
{
   nandlog_page_header_t header;
   uint32_t length = nandlog_retrieve_next_page(page, &header);
   if (length == 0)
      ; // unreadable page. A gap, not the end — header.seq identifies it
}

nandlog_end_reading();
nandlog_end_session();
```

A **session** holds the part powered across a run of operations. Reading and epoch operations require one;
logging does not. An **epoch** is one generation of the log; reads cover one epoch at a time.

## Configuration

Edit `nandlog_conf.h`. Nothing has a silent default — anything missing is a named compile error.

| Setting | Default | Meaning |
|---|---|---|
| `NANDLOG_HAS_HARDWARE` | 1 | 0 compiles the library to no-op stand-ins |
| `NANDLOG_MAX_PAGE_SIZE_BYTES` | 4096 | largest page you will spend RAM on |
| `NANDLOG_MAX_SPARE_SIZE_BYTES` | 256 | the same for the spare area |
| `NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL` | 3 | attempts on a block before it is retired |
| `NANDLOG_ERASE_AHEAD_BLOCKS` | 2 | blocks kept erased ahead of the write head |
| `NANDLOG_PAGE_PLACEMENT_ATTEMPTS` | 3 | blocks one write may relocate through before giving up |
| `NANDLOG_MAX_EPOCH_DETAILS_BYTES` | 512 | largest caller-defined blob per epoch |
| `NANDLOG_TIMESTAMP_TOLERANCE_MS` | 250 | backward time step tolerated before a page is committed |
| `NANDLOG_RECORD_FRAMING` | 1 | records carry their own length |
| `NANDLOG_BUSY_POLL_INTERVAL_US` | 10 | how often to poll the part's BUSY bit |
| `NANDLOG_BUSY_TIMEOUT_MS` | 500 | how long before the part is declared dead |
| `NANDLOG_CHIP_PAGE_COPY` | 1 | relocate pages inside the chip rather than across the bus |

Buffers are sized from the two `MAX_` values, so setting them larger than your part needs costs only unused
RAM. A part whose page exceeds the budget is a compile error.

## Porting

Copy `nandlog_port.c` from the repository root, delete its `#error` line, and implement thirteen functions:

| | |
|---|---|
| `nandlog_port_init` / `_deinit` | bring the SPI peripheral and pins up and down |
| `nandlog_port_transfer_read` / `_write` | one command byte, *n* address bytes, then data, as one exchange |
| `nandlog_port_lock` / `_unlock` | a mutex, or empty on a single-threaded system |
| `nandlog_port_write_enable` | gate the write-protect pin |
| `nandlog_port_power` | wake or sleep the peripheral |
| `nandlog_port_log` | `printf`-style diagnostic sink |
| `nandlog_port_delay_us` / `_delay_ms` | busy-wait and coarse delay |
| `nandlog_port_fatal` | the part stopped answering — reset, halt, your choice |
| `nandlog_port_unwritable` | no block will retain a page; not urgent, do not reset |

Each stub documents what the layer above assumes of it. A complete worked example for an Ambiq Apollo4 under
FreeRTOS is in the [porting guide](https://hedgetechllc.github.io/nandlog/porting.html).

## Adding a chip

One `.c` file under `chips/`. Declare the geometry, include the common header, implement twelve functions:

```c
#define NANDLOG_CHIP_NAME                    "AS5F18G04SND"
#define NANDLOG_CHIP_PAGE_SIZE_BYTES         4096
#define NANDLOG_CHIP_SPARE_SIZE_BYTES        256
#define NANDLOG_CHIP_PAGES_PER_BLOCK         64     // must be a power of two
#define NANDLOG_CHIP_BLOCK_COUNT             4096
#define NANDLOG_CHIP_RESERVED_BLOCKS         80     // kept back for bad-block management
#include "nandlog_chip_common.h"
```

| Function | Does |
|---|---|
| `nandlog_chip_geometry` | return the geometry declared above |
| `nandlog_chip_probe` | confirm the part is present and answering |
| `nandlog_chip_init` | configure it, load or build the bad-block table |
| `nandlog_chip_low_power` | enter or leave the lowest-power state |
| `nandlog_chip_read_page` | read a whole page |
| `nandlog_chip_read_page_region` | read part of a page, at a column address |
| `nandlog_chip_write_page` | program a whole page, retrying |
| `nandlog_chip_erase_block` | erase the block containing a page |
| `nandlog_chip_copy_page` | relocate a page inside the chip |
| `nandlog_chip_is_bad_block` | is this block retired? |
| `nandlog_chip_mark_bad_block` | retire it |
| `nandlog_chip_reset_bad_blocks` | recovery utility: discard the persisted table |

Two of these have shortcuts. A part with no internal data-move command expands
`NANDLOG_CHIP_NO_INTERNAL_PAGE_COPY` instead of writing `copy_page`. `read_page_region` must issue a real
column address — implementing it as "read the whole page and copy a slice" works but gives up most of the
library's read performance.

Read either supplied driver; each is self-contained. Full walkthrough in the
[chip driver guide](https://hedgetechllc.github.io/nandlog/chips.html).

## API summary

| | |
|---|---|
| **Lifecycle** | `nandlog_probe` `nandlog_init` `nandlog_deinit` `nandlog_disable` `nandlog_data_bytes_per_page` |
| **Epochs** | `nandlog_begin_epoch` `nandlog_retrieve_epoch_details` `nandlog_epoch_count` `nandlog_epoch_info` `nandlog_select_epoch` `nandlog_select_current_epoch` |
| **Writing** | `nandlog_store_record` `nandlog_flush` `nandlog_has_buffered_data` |
| **Sessions** | `nandlog_begin_session` `nandlog_end_session` |
| **Reading** | `nandlog_begin_reading` `nandlog_end_reading` `nandlog_read_span` `nandlog_retrieve_next_page` `nandlog_retrieve_page_by_seq` `nandlog_read_recent_page` `nandlog_framed_next_record` |
| **Retransmission** | `nandlog_retransmit_clear` `nandlog_retransmit_add` `nandlog_retransmit_count` `nandlog_retransmit_total_bytes` `nandlog_retrieve_retransmit_page` |
| **Diagnostics** | `nandlog_bad_block_count` |
| **Recovery** | `nandlog_reset_bad_block_table` |

Every function is documented in [nandlog.h](nandlog.h) and in the
[API reference](https://hedgetechllc.github.io/nandlog/api.html).

## Tools

`tools/nandlog_parse.py` reads both a raw image dump and an offload stream, verifying every CRC:

```sh
./tools/nandlog_parse.py image  dump.bin --page-size 4096 --spare-size 256
./tools/nandlog_parse.py stream offload.bin --json
```

## Testing

```sh
cd sim && make
```

Builds and runs the real log and chip driver against a RAM-backed emulated part, in three configurations,
under ASan and UBSan. The simulator injects unwritable blocks, unerasable blocks, mid-operation power loss,
bit rot, and a part that never clears BUSY.

`tests/` holds three applications that run on real hardware, for the things a simulator cannot answer —
column-addressed reads, chip-internal page copy, and spare-area behaviour under on-die ECC. See
[tests/README.md](tests/README.md).

## Versioning

```c
#if (NANDLOG_VERSION_MAJOR != 1)
#error "This application understands nandlog 1.x only"
#endif

#if NANDLOG_VERSION_AT_LEAST(1, 2)
   // nandlog_select_epoch() and nandlog_bad_block_count() exist
#endif
```

Major changes when something you built against must change; minor for additions. `NANDLOG_FORMAT_VERSION` is
separate and versions the record grammar on flash. The on-flash format has not changed across any 1.x
release; see the [upgrade notes](https://hedgetechllc.github.io/nandlog/upgrading.html).

## License

MIT. See [LICENSE](LICENSE).
