# nandlog

Append-only, CRC-validated logging on raw SPI NAND, built to keep every committed record across a power cut. No
flash translation layer, no filesystem, no heap.

For devices that collect data for weeks and hand it over in one go — a battery-powered sensor, a tag, a
logger — where writes are small and frequent, reads are rare and bulk, and power can be lost at any instant.

- **Static allocation only.** About 15 kB of RAM on a 4 KB-page part, fixed at compile time.
- **Durable once committed.** A record is durable once the call that committed its page returns, and a cut at any
  instant after that must not lose it or corrupt it: the [contract below](#what-survives-a-power-cut), which the
  simulator holds the library to at every program and erase. Cutting power to a real part is still to come.
- **Bad-block tolerant.** A block that refuses a write or an erase is retired, and the pages already in it are
  moved and verified. The bad-block table survives a cut at any instant; on a part whose reserve cannot hold it,
  it moves to two blocks of the log region, or, with none to spare, the blocks' own marks stand in for it, and
  logging goes on either way.
- **Fast recovery.** The write head is found by a binary search over the log region that is sound with retired
  blocks, unreadable pages and torn writes in the epoch — about 90 page reads per boot on an 8 Gbit part.
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

**Requirements:** a C99 compiler, a little-endian target, a SPI NAND part, and roughly 15 kB of RAM. No RTOS,
heap, or filesystem.

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

// Start a new run. Everything logged before this stops being reachable. Until the first epoch is begun on a
// part, records are refused rather than written somewhere a reboot would not find them
nandlog_begin_session();
nandlog_begin_epoch(&my_details, sizeof(my_details));
nandlog_end_session();

// Log. Cheap, and commits a page automatically when one fills. Anything but NANDLOG_OK means the record was
// not stored, and says why
if (nandlog_store_record(RECORD_SAMPLE, milliseconds, &sample, sizeof(sample)) != NANDLOG_OK)
   ; // too large, log full, no epoch yet, a read open...

// Force out a partial page — on a timer, or at shutdown. An idle nandlog_flush(false) is also where the
// erase-ahead runs, so a commit never has to wait for an erase
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

While a read is open, a record that would commit a page is refused (`NANDLOG_ERR_READING`): a commit that fails
relocates the head's block and retires it, and an open read keeps its place by physical page. To keep logging
through a long download, open the read only to take its span — the first page's `header.seq` and the page count —
close it, and fetch the pages with `nandlog_retrieve_page_by_seq()`, which needs no open read (regression test N1).

## What survives a power cut

A record is **durable** once the call that committed its page has returned: the `nandlog_store_record()` whose
record did not fit and pushed the previous page out, a `nandlog_flush(true)`, or a `nandlog_begin_epoch()` that
committed what was buffered. Records still in the RAM page are lost at a cut, as they would be at any reset;
flush on a timer to bound that.

After a cut at any instant — mid-program, mid-erase, mid-relocation, mid-metadata or mid-table update, or a
second cut during the reboot that follows or the logging after it — every durable record is delivered intact, a
page torn by the cut is reported as a gap, no committed page that still reads is erased, and nothing is
programmed twice, on every boot after it, however a torn page that fails its checks reads on each boot and
however it later drifts — with the exceptions listed below. And a **sequence number is an identity**: what a
read has served under an epoch and sequence number is what that number holds from then on, or a gap if the
part loses the page — never other records. A read commits whatever is buffered before it opens, so it never
serves a page from RAM.

That is what the simulator's crash-point campaign checks at every program and erase of fourteen traces, under
four models of a torn operation, with a second cut in the recovering boot and in the logging after it, before
letting every weakly programmed page drift; what its fuzzer checks at random; and what a bounded-exhaustive
check of every combination of page states below the write head of a toy part, and of every cut in every
metadata write path, checks with the shipping code (see [Testing](#testing)). **This build has not yet run on a
real part**: `tests/test_nandlog_powercut.c` is the hardware power-cut campaign, and its results are still to
come. The model and its limits:

- a page that **reads erased in every bit** cannot be told from an erased page by any read, so a boot that finds
  one at the write head programs it again: one a cut stopped before it changed a bit; one the part refused
  without changing a bit, if the cut also beat the metadata write that records the refusal or every copy of
  that record is later lost; and a torn page that drifts back to erased before the first boot after its cut has
  finished listing it. These are the residual second programs. In the log region each lands on the page at the
  write head, which no read has served, and building with `NANDLOG_PASS_OVER_HEAD_AT_BOOT` rules them out there,
  at a price paid at every boot (see [Configuration](#configuration)); a metadata or table copy a cut stopped
  before it changed a bit is still written again, read back and verified, by the next write to its pair;
- a torn page is **listed** in the epoch's metadata slot by the first boot that finds it below the write head,
  before anything is committed after it, and counts as written from then on whatever it reads, blank included.
  A torn page that **reads whole** — header, payload and ECC all checking — cannot be told from a committed one
  and is taken as one, so a read may serve it though its commit never returned; that it then never drifts is an
  assumption of the model, as is that a committed page never comes to read erased in every bit;
- a slot lists up to 880 runs of passed-over positions on a 4 KB page, 368 on a 2 KB one. An epoch whose list
  is nearly full is **sealed**: its slot says so, and every later boot refuses records with `NANDLOG_ERR_SEALED`
  and calls `nandlog_port_unwritable()`, until a new epoch is begun. Reads go on;
- one unreadable page of the metadata ring or of the bad-block table changes nothing, because each is written
  twice, and a copy is read once and judged by every checksum it carries. Both copies share a block, so a copy of
  the newest record read only at the limit of the part's ECC has the boot write that record again, unchanged, in a
  fresh block (below). So does one stray programmed bit in the table block's spare-area mark, which is outside the
  ECC: a block of the reserve whose mark reads one programmed bit, holding a version newer than the one loaded
  without it, which that version does not name, is read as unmarked and its version loaded. If no copy of the newest slot the
  epoch acted on holds its list whole, the epoch is sealed and that list is gone; if only its details are lost,
  it goes on without them. If both copies of a slot are lost outright, the boot goes by the slot before it: an epoch begun since is
  recovered from its own pages, without its details, whatever the slot before says, sealed or not; positions
  only the lost slot listed are guarded only by the head search's look past the head it finds; and a failed
  program it recorded may be programmed again if it reads blank;
- the bad-block table has to lay the epoch out as it was written. The ring records how many blocks are retired
  and which of the epoch's own blocks are, and a table that differs — both copies of its newest version lost,
  say — seals the epoch; its reads then go ahead as best they can — they may miss pages, or serve a page a
  relocation copied twice under its one sequence number — and say so (`layout_unverified` in
  `nandlog_epoch_info()`, and an error logged), at every boot until a new epoch is begun: the seal's slot keeps the
  layout the epoch was written by, not the table's. The epoch begun after it, at whichever boot, starts past the
  last block still holding one of its pages, so none of them is erased as the new epoch's window. On the Alliance
  part every retired block also carries a spare-area mark, read with ECC off, which counts only when it reads
  exactly 00h: every boot programs again any mark that does not. The recovery utility programs every mark once
  more and keeps the table if one will not then read 00h; it never erases a block that may carry a mark, and
  erases the newest table version last, only once every older one is gone, so no cut and no block that refuses
  an erase can roll the table back;
- on the Alliance part the table lives in the first blocks of a 16-block reserve at the top of the array. If the
  reserve cannot hold it — its blocks dead, refusing every program and erase, as one board's were found — a boot
  gives the driver an aligned pair of blocks from the top of the log region, outside the current epoch and
  holding nothing a read could serve, and the table moves there; the pair is retired like any failed block, so no
  position is laid out over it, and every later boot finds the table by one header read a block down to 48
  blocks below the reserve (about 13 to 26 ms; a healthy part never looks). With no such pair to be had, the
  spare-area marks become the record — **degraded mode**: a retirement is committed by its block's mark,
  programmed and read back exactly 00h, after the block's pages have moved, and every boot rebuilds the table
  from one raw read of every block's mark, as a new part's first boot does (about 1.8 s on an 8 Gbit part), and
  looks for a pair again. The mark bytes are outside the part's ECC, and the rebuild takes any byte but FFh for a
  mark, the only rule under which a mark a cut left with a bit or two programmed reads the same at every boot. So a
  stray programmed bit in a good block's byte retires the block (in the rebuild after the recovery utility too): a
  block's capacity lost; or, if the block holds part of the current epoch, that block's pages out of reach and the
  epoch sealed, nothing erased: by the newest slot when it already counts the block in its span, and otherwise by the
  head search, which then reads pages of the epoch below their own positions. Only in the block the write head is
  in does nothing show it: its pages are out of reach and their sequence numbers are used again. A boot that finds the failed head's block retired, its relocation
  recorded, accepts that only with the write head at or past the failed page's position: a mark a cut tore can
  read as none at one boot and as a mark at the next, after the copies of a second relocation were cut short, and
  the epoch is then sealed instead. A block that will not take even its mark — or any retirement neither the
  table nor the marks can record — gets the table a pair there and then, in the call that needed it, under the
  same rules, and is retired through it; only with no such pair anywhere (the epoch filling every block the search
  reaches, say) does the log stop, as it always has;
- on the Winbond part a retirement is an entry in the part's own 20-entry remap table, which its factory-bad
  blocks share. Once it is full a failing block cannot be retired and the log stops taking pages; and that an
  entry is written whole or not at all across a cut is the part's to guarantee, assumed here and not verified;
- an earlier epoch is kept until the write head sweeps over it; a block of it the window cannot erase is retired,
  and so no longer read, a little before that;
- corruption that both the part's ECC and CRC-32 miss is outside the model;
- `nandlog_port_fatal()` must not return, and the bad-block table's atomicity rests on the part completing a
  page program or reporting its failure.

Nothing rewrites a page in place. A page of the log read back corrected only at the limit of the ECC is counted
(`reads_at_ecc_limit` in `nandlog_get_stats()`) and left to the part's ECC. The metadata is read far harder: every
boot reads about ten pages of the newest ring block and of the newest table block, some five million page reads a
year at one boot a minute, and both copies of each record sit in that one block, so wear on the block is wear on
both. A boot that reads either copy of the newest ring slot, or of the newest table version, only at the limit of
the ECC therefore writes it again, unchanged, into a fresh block, whose erase and program start its pages afresh;
and nothing that reads back at the limit counts as written, so a page stuck there costs one boot one write, not
every boot. That relies on the part reporting the limit before a page goes past it. The Alliance part reports it
when a codeword (512 + 32 B) needed all 8 of its corrections, one bit short of loss: enough for errors that build up
between one boot and the next, such as read disturb, but not for those of a long period unpowered, which are left to
the part's retention rating. The Winbond part has no such verdict, so on it, and on any part, check the read-disturb
rating against the boot rate. The head search's first
four probes move from boot to boot — with the ring's slot numbering, which a clean boot does not change, and with
`NANDLOG_BOOT_SALT`, which a device that boots often should define (a boot counter that survives a reset, or a
random number) — so they do not fall on the same pages every time.

## Configuration

Edit `nandlog_conf.h`. Nothing has a silent default — anything missing is a named compile error.

| Setting | Default | Meaning |
|---|---|---|
| `NANDLOG_HAS_HARDWARE` | 1 | 0 compiles the library to no-op stand-ins |
| `NANDLOG_MAX_PAGE_SIZE_BYTES` | 4096 | largest page you will spend RAM on |
| `NANDLOG_MAX_SPARE_SIZE_BYTES` | 256 | the same for the spare area |
| `NANDLOG_BLOCK_ERRORS_BEFORE_REMOVAL` | 3 | erase attempts on a block before it is retired; a failed program is never repeated |
| `NANDLOG_ERASE_AHEAD_BLOCKS` | 2 | blocks kept erased ahead of the write head, each erased once, outside any commit — and again at every boot, so 0 suits a device that reboots far more often than it fills a block |
| `NANDLOG_PAGE_PLACEMENT_ATTEMPTS` | 3 | blocks one write may bring into play, its own included, before giving up |
| `NANDLOG_MAX_EPOCH_DETAILS_BYTES` | 512 | largest caller-defined blob per epoch; at most the page size less 320 bytes (1,728 on 2 KB, 3,776 on 4 KB), so a slot keeps room for 64 runs of passed-over positions. It sets that room — 880 runs on a 4 KB page at 512, 64 at 3,776 — and every seal threshold that follows from it. It may change under a log already written: an epoch whose slot then carries longer details, or a longer list, than the new value leaves room for is sealed whole, until a new epoch is begun |
| `NANDLOG_TIMESTAMP_TOLERANCE_MS` | 250 | backward time step tolerated before a page is committed, in the units of your timestamps |
| `NANDLOG_BOOT_SALT` | 0 | a value that differs from boot to boot, if the host has one (a boot counter, a random number), by which the head search rotates its first probes; any value is safe |
| `NANDLOG_RECORD_FRAMING` | 1 | records carry their own length |
| `NANDLOG_BUSY_POLL_INTERVAL_US` | 10 | the wait after each poll of the part's BUSY bit |
| `NANDLOG_BUSY_TIMEOUT_POLLS` | 50000 | polls before the part is declared dead. A count, not a time: the port has no clock, so the wait is at least polls × interval (0.5 s) plus the status reads' own time — about 2.1 s on an Apollo4 port whose status read takes 32 µs, which a watchdog must allow |
| `NANDLOG_CHIP_PAGE_COPY` | 1 | relocate pages inside the chip rather than across the bus |
| `NANDLOG_PORT_CRC32` | 0 | the port computes the log's CRC-32, in `nandlog_port_crc32()` — on a hardware engine, say — rather than the library in software; see Porting |
| `NANDLOG_CRC_TABLE_IN_RAM` | 1 | keep the software CRC's 1 KiB lookup table in RAM, as initialized data, rather than in flash: on an Apollo4 the table read from MRAM made the CRC about 21 cycles a byte |
| `NANDLOG_CRC_SLICE_BY_4` | 0 | the software CRC takes four bytes a step through four tables, about twice as fast on a Cortex-M4, for 3 KiB more table |
| `NANDLOG_LOG_LEVEL` | 3 | which messages reach `nandlog_port_log()`: 0 none, 1 `ERROR:` lines, 2 those and `WARNING:` lines, 3 every one, the boot's `INFO:` lines too. The rest are compiled out, text and all, and nothing the library does depends on them; on the Cortex-M4 build, level 0 links some 4.8 KB less flash than 3 |
| `NANDLOG_PASS_OVER_HEAD_AT_BOOT` | 0 | a boot that finds the write head part-way into a block passes over the head's page rather than program it, so no page of the log region is ever programmed twice; it costs a slot pair and a position at every such boot, clean or not, and a run of the epoch's passed-over list unless nothing was committed since the boot before, so an epoch is sealed after about 876 such boots (364 on a 2 KB page). On for a device that rarely reboots, or begins an epoch every mission, and can lose power mid-program; off for one that reboots often while it logs, or rarely loses power during a program |

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
| `nandlog_port_log` | `printf`-style diagnostic sink; `NANDLOG_LOG_LEVEL` sets which messages reach it |
| `nandlog_port_delay_us` / `_delay_ms` | busy-wait and coarse delay |
| `nandlog_port_fatal` | the part stopped answering — reset or halt; **must not return** |
| `nandlog_port_unwritable` | no block will retain a page; not urgent, do not reset |

Each stub documents what the layer above assumes of it. A complete worked example for an Ambiq Apollo4 under
FreeRTOS is in the [porting guide](https://hedgetechllc.github.io/nandlog/porting.html).

A host with a CRC engine can take the log's CRC-32 over: set `NANDLOG_PORT_CRC32` and implement a fourteenth
function, `nandlog_port_crc32(crc, data, length)`, which must return exactly what Python's
`zlib.crc32(data, crc)` would — continuing a running value, for `data` at any alignment and of any length, since
the log adds each record to its page's CRC as the record is stored. The bytes an engine cannot take go to
`nandlog_crc32_software()`, which continues the same value. A wrong CRC is written to the part as it is, so an
engine whose conventions are undocumented must agree with the software on known vectors before its first use;
the template's commented sketch and the porting guide's Apollo4 port, which checks the security block's engine at
`nandlog_port_init()` and falls back to the software for good on any disagreement, show how.

## Adding a chip

One `.c` file under `chips/`. Declare the geometry, include the common header, implement fifteen functions:

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
| `nandlog_chip_init` | configure it, load or build the bad-block table |
| `nandlog_chip_low_power` | enter or leave the lowest-power state |
| `nandlog_chip_read_page` | read a whole page, reporting the part's ECC verdict |
| `nandlog_chip_read_page_region` | read part of a page, at a column address |
| `nandlog_chip_page_is_blank` | is every bit of the page erased, read with ECC bypassed? |
| `nandlog_chip_write_page` | program a whole page, once — never repeated after a failure |
| `nandlog_chip_erase_block` | erase the block containing a page |
| `nandlog_chip_copy_page` | relocate a page inside the chip |
| `nandlog_chip_is_bad_block` | is this block retired? |
| `nandlog_chip_mark_bad_block` | retire it, persistently and atomically, and say whether it took |
| `nandlog_chip_table_blocks_wanted` | how many log-region blocks the table needs because it has nowhere else; 0 for most parts |
| `nandlog_chip_table_take_blocks` | take those blocks for the table and retire them; `false` for a part that never asks |
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
| **Reading** | `nandlog_begin_reading` `nandlog_end_reading` `nandlog_read_span` `nandlog_retrieve_next_page` `nandlog_retrieve_page_by_seq` `nandlog_read_recent_page` `nandlog_framed_next_record` `nandlog_crc32` |
| **Retransmission** | `nandlog_retransmit_clear` `nandlog_retransmit_add` `nandlog_retransmit_count` `nandlog_retransmit_total_bytes` `nandlog_retrieve_retransmit_page` |
| **Diagnostics** | `nandlog_bad_block_count` `nandlog_get_stats` |
| **Recovery** | `nandlog_reset_bad_block_table` |

Every function is documented in [nandlog.h](nandlog.h) and in the
[API reference](https://hedgetechllc.github.io/nandlog/api.html).

## Tools

`tools/nandlog_parse.py` reads both a raw image dump and an offload stream, verifying every CRC. In an image it
recognises tombstones, the stale copies a relocation leaves in a retired block, and the positions each epoch's
metadata slot lists as passed over:

```sh
./tools/nandlog_parse.py image  dump.bin --page-size 4096 --spare-size 256
./tools/nandlog_parse.py stream offload.bin --json
```

## Testing

```sh
cd sim && make            # about three and a half minutes
cd sim && make campaign   # about ninety minutes on twelve cores: as shipped, window 0, head pass-over, dead reserve
cd sim && make fuzz       # about eleven minutes on twelve cores, nine builds
cd sim && make exhaustive # about three minutes, five builds
cd sim && make opcounts   # a few seconds
cd sim && make windows    # the regression tests with an erase-ahead window of 0 blocks, and of 1; the dead-reserve tests at 3
cd sim && make headpass   # the head pass-over's share of the above, on its own
cd sim && make soak       # under a minute: the on-device power-cut soak, a thousand boots over the simulator
cd sim && make ablation   # about seven minutes on twelve cores: which checks notice each mechanism taken away
cd sim && make baselines  # fetches Dhara and littlefs at pinned commits; about five minutes on seven cores
```

`make` builds and runs the real log and chip driver against a RAM-backed emulated part, under ASan and UBSan,
in five configurations: record framing off and on, relocation through RAM rather than inside the chip, the
Winbond driver with its hardware remap table emulated, and every CRC through the port's hook, which continues the
software CRC in deliberately odd pieces. Each runs the suite and a regression test for every
defect found before release, and the reference parser is checked against the images the suite dumps. Every
simulator build also checks each CRC the log keeps up record by record against a recompute wherever it is used
(`NANDLOG_CHECK_CRC`). The simulator
injects unwritable and unerasable blocks and pages, cells that will not program, power cuts that leave an
operation untouched, half-done, or with each bit independently done or undecided, torn erases, ECC verdicts of
corrected, uncorrectable, or corrected at the limit, and the passing of time: a page a cut stopped short, or one
programmed into a block whose erase a cut interrupted, drifts back toward erased.

`make campaign` cuts power at every program and erase of fourteen traces — first boot, relocation, the epoch's
first block retired, the metadata ring, the bad-block table, a full region, failures at the wrap, metadata and
pages torn so they read differently on every boot, a commit no block will take, torn commits listed by boots
that are themselves cut in their slot writes, a failing block that refuses even its mark — under four torn-write models, then again at every operation of
the reboot that recovers and at the first operations of the logging after it, lets every weak page drift, and
checks the durability contract and sequence-number identity after each, over several boots. It also sweeps
recovery over fault type, fault position and head position, and makes each page of the metadata ring and the
bad-block table unreadable in turn. It does all of that four times: as shipped; with an erase-ahead window of
0 blocks, where every block is entered by an erase inside a commit; with `NANDLOG_PASS_OVER_HEAD_AT_BOOT`,
traces only, since that option moves the head positions the sweeps check; and on a part whose reserve's table
blocks and region's top two blocks are dead, so the table lives in a pair of blocks below them. `make fuzz` drives
random calls, faults, power cuts and drift against an oracle written independently of the library, in nine
builds, those among them, two parts with a dead reserve, one where the table moves below it and one where only
the marks can hold it, and the port-CRC configuration. `make exhaustive` boots the shipping code over every combination of page states — committed,
rotted, torn, listed, drifted, tombstoned — over the positions just below the write head of a toy part, in every
place the layout has an edge, and checks the recovery invariant itself, read from the medium; then it runs every
metadata write path — a boot listing torn pages, a new epoch, a boot settling a torn slot, a retirement, a
boot relocation, a window block that will not erase, the recovery utility — into a cut at each of its
operations under every cut model, with or without one copy of the newest slot or table version damaged as well,
and lets weak pages drift; as shipped, with a window of 0, and (metadata paths) with the head pass-over and on
two parts with a dead reserve, adding the paths that move the table below it, rotate it there, and hand it to the
marks when its blocks fail. `make
opcounts` regenerates the operation counts the documentation quotes, with each call's reads split by how much they
clock out. `make soak` builds the on-device power-cut soak, `tests/test_nandlog_powercut.c`, unchanged for the host,
cuts the simulated power a thousand times at random, checks the log with `tests/powercut_check.py`, and shows the
checker a run with one committed bit cleared, which it must report. `make ablation` builds the library once for
each `NANDLOG_MUTATE_*` switch in `nandlog.c` — each takes one mechanism away: the passed-over list in the head
search, the erase guard, the single read of a metadata slot, the block-entry rule, the numbered positions (a
first-blank search instead), the tombstones — and runs the regression tests, the exhaustive check and a short
campaign subset against each, printing which noticed. `make baselines` fetches Dhara and littlefs at pinned
commits into an ignored directory, never into the repository, and counts every stack's flash operations over the
same driver and simulated part, measuring footprint too where `arm-none-eabi-gcc` is installed.

`tests/` holds applications that run on real hardware, for the things a simulator cannot answer —
column-addressed reads, chip-internal page copy, spare-area behaviour under on-die ECC, the measured figures,
and the durability contract under real power cuts. See [tests/README.md](tests/README.md).

## Versioning

```c
#if (NANDLOG_VERSION_MAJOR != 1)
#error "This application was written for major version 1 of nandlog"
#endif

#if NANDLOG_VERSION_AT_LEAST(1, 1)
   // something a later minor release adds
#endif
```

This is 1.0. Major changes when something you built against must change; minor for additions.
`NANDLOG_FORMAT_VERSION` is separate and versions what the library writes on the part: version 1, the first,
recorded in every metadata slot.

## License

MIT. See [LICENSE](LICENSE).
