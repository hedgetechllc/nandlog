# nandlog on-device tests

Three test applications that answer questions the host simulator cannot. The simulator covers everything
above the SPI wire; these cover the wire, the silicon, and the clock.

| test | what it gates | writes to the part? |
|---|---|---|
| `test_nandlog_partial_read.c` | column-addressed and short page reads — **the primitive under every header read the library makes** | one scratch page |
| `test_nandlog_page_copy.c` | `nandlog_chip_copy_page()` — **the primitive under a relocation, with the bus left out** | three scratch blocks |
| `test_nandlog_ecc_marker.c` | whether a bad-block mark can be written into the spare area — **gates the mark the Alliance driver writes into every block it retires** | four scratch blocks |
| `test_nandlog_bench.c` | gates nothing: **the measured figures for the whitepaper** — part, bus, CRC and every library call | **ERASES THE WHOLE LOG REGION AND RING** |
| `test_nandlog_powercut.c` | gates nothing: **the durability contract under real power cuts**, checked by `powercut_check.py` | the log region, continuously |
| `test_nandlog_interrupt.c` | gates nothing: **what an interrupted program or erase leaves on the part**, to calibrate the simulator's fault models, tabulated by `interrupt_summary.py` | six blocks mid-log-region, left erased |

**What has and has not run on silicon.** `nandlog_bench` has run on three Apollo4 Plus boards with the Alliance
part, one of them with the top 34 blocks unusable, and every hardware figure in the whitepaper comes from it. Its
last run is of a build that lacks these parts of 1.0: the refresh of a metadata copy read at the ECC limit, which reads
and writes nothing on a boot whose metadata reads clean; the rule that a slot written while an epoch's layout is
unverified keeps the epoch's layout record, which acts only after a boot finds the newest table version lost (no block
was retired in that run); the boot's check that a failed head's block found retired left none of its pages behind,
which acts only on such a retirement; and a slot judged by what any build could have written in it rather than by the
build's own limits, which reads the same bytes at the default details limit and changes nothing for a slot that build
wrote. `nandlog_powercut` has run on one of those boards through 116 power cuts made by hand, with
no violation; that board ran a build that also lacks the driver's run of entry-free pages and the word-aligned
buffers, both of which change speed alone. Its unattended builds, `nandlog_selfcut` and `nandlog_selfcut_inject`,
have not yet run. `nandlog_interrupt` has not yet run, so the simulator's models of what an interrupted program or
erase leaves remain uncalibrated. The Winbond driver has not run on silicon.

The first three work only in the **top eight blocks of the reserve**, which the log never places a page in, using
four of them and stepping over any that is factory-bad. None of them touches the log region, the metadata ring, or
the chip driver's bad-block table, which lives in the first eight blocks of the Alliance part's sixteen-block
reserve. They are still write tests: run them on a development board, not on a device holding
data you want. **The last three are not like that** — see [the measurement programs](#the-two-measurement-programs)
and [`nandlog_interrupt`](#nandlog_interrupt--what-an-interrupted-program-or-erase-leaves-on-the-part) below.

Run them in order. If test 1 fails, tests 2 and 3 tell you nothing, because both read results back through
the primitive test 1 is checking.


## Building them under SociTrack

The tests are written against the SociTrack firmware's `setup_hardware()`, `print()` and `am_hal_delay_us()`,
because that is where the hardware is. Four changes to `software/firmware/tests/Makefile`:

**1. Add this directory to `VPATH`,** next to the two nandlog lines that are already there:

```make
VPATH += ../src/external/nandlog
VPATH += ../src/external/nandlog/chips
VPATH += ../src/external/nandlog/tests          # <-- add
```

`INCLUDES` already has `-I../src/external/nandlog`, and that is the only include path these need.

**2. Add the three targets,** alongside `storage:`:

```make
nandlog_reads: TARGET = TestNandlogReads
nandlog_reads: SRC += test_nandlog_partial_read.c nandlog_hw_test.c
nandlog_reads: $(CONFIG) $(CONFIG)/test_nandlog_partial_read.o $(CONFIG)/nandlog_hw_test.o $(CONFIG)/$$(TARGET).bin program

nandlog_copy: TARGET = TestNandlogCopy
nandlog_copy: SRC += test_nandlog_page_copy.c nandlog_hw_test.c
nandlog_copy: $(CONFIG) $(CONFIG)/test_nandlog_page_copy.o $(CONFIG)/nandlog_hw_test.o $(CONFIG)/$$(TARGET).bin program

nandlog_marker: TARGET = TestNandlogMarker
nandlog_marker: SRC += test_nandlog_ecc_marker.c nandlog_hw_test.c
nandlog_marker: $(CONFIG) $(CONFIG)/test_nandlog_ecc_marker.o $(CONFIG)/nandlog_hw_test.o $(CONFIG)/$$(TARGET).bin program
```

**3. Add them to `.PHONY`** and to the `all:` error message, next to `storage`.

**4. Check that `AM_DEBUG_PRINTF` is on** — it already is in this Makefile — because the output *is* the
result. Watch it over SWO or the Segger RTT console, whichever you normally use.

Then:

```
cd software/firmware/tests
make nandlog_reads BOARD_REV=P
```

A note on timing: every figure these print comes from the DWT cycle counter, calibrated at startup against
`am_hal_delay_us(10000)` rather than against an assumed core clock. If the first line says
`WARNING: DWT is not counting`, every timing number afterwards is a fiction — DWT is gated without a debugger
on some parts, so attach one.


## Test 1 — `nandlog_reads`

Five parts. **1b and 1c are the ones that matter.**

- **1a** short reads from offset 0 agree with a whole-page read.
- **1b** reads from ten different non-zero columns land where they claim. If the 16-bit column address goes
  out in the wrong byte order, this is what catches it — and it prints what a byte-swapped address *would*
  have read, so a failure names its own cause.
- **1c** the spare-area marker byte reads the same whether you address its column directly or drag the whole
  page along in front of it. This is run over **every block on the part**.
- **1d** a page header reads correctly on its own, and the write head still recovers across a re-init — the
  boot path is now entirely header-only reads.
- **1e** boot cost, and a header read against a whole-page read.

**Why this one is not optional.** A wrong column byte order does not fail loudly. It reads `0xFF` from the
wrong place, the factory bad-block scan concludes every block is good, and the log goes on working until it
writes into a block the manufacturer had already condemned. The simulator cannot catch it, because it decodes
the column address the same way the driver encodes it.

**One caveat the test prints for itself:** if the part reports no factory-bad blocks at all, 1c compared
`0xFF` against `0xFF` and proved nothing. 1b is then the only evidence, which is why there are ten offsets
in it rather than one.

**Report back:** the PASS/FAIL line, the factory bad-block count, and the whole of 1e.


## Test 2 — `nandlog_copy`

Four parts, and it asks to be run twice with a power cycle in between.

- **2a** a copied page is the source page, and reads back without an ECC error. The test deliberately leaves
  a *different* page in the cache register first, so a part that ignores the copy sequence and programs
  whatever the register holds produces a wrong page rather than an empty one — and the test says which.
- **2b** the copy did not drag the source's spare area across. **This is the subtle one.** If it did, a
  relocated page could carry a non-`0xFF` byte into a destination block's marker position, and a later scan
  would retire a perfectly good block.
- **2c** a run of eight pages including an erased one, which catches a stale cache register.
- **2d** what it costs against a read and a write back, per page and per block.

**Report back:** the PASS/FAIL line and 2d's timing table. Then **power-cycle the board, re-flash, and run it
again.** Re-flashing between runs is fine and expected: it rewrites the MCU, not the NAND, and the check runs
before anything on the part is erased.

**What the result means.** If 2a fails, set `NANDLOG_CHIP_PAGE_COPY` to 0 in `nandlog_conf.h` before
deploying; the log falls back to reading and writing each page and everything else still works. If only 2b
fails, the copy works but the driver has to clear the destination's marker byte after each relocation — tell
me and I will add it.


## Test 3 — `nandlog_marker`

It answers whether a retired block can be marked in its own spare area. The Alliance driver does exactly
that, after every retirement: a `00h` at the first spare byte of the block's first page, programmed with ECC
off. The persisted table stays the authority — finding the marks costs a read of every block — but the marks
are what the table is rebuilt from if every copy of it is ever lost, so this test now gates shipping
behaviour. If step 3 or 4 comes out negative on your part, the table still works and nothing is lost but that
last line of defence.

It talks to the chip directly rather than through the driver, because it has to turn ECC off and program a
page that is erased but for one spare byte, and no driver function exposes either. Like the driver, it loads
that page from column 0, the whole page and spare area, `FFh` but for the mark: on the Alliance part a PROGRAM
LOAD resets the cache register only from the column it starts at, so a one-byte load at the spare's column
programs whatever the last page read left in the main area. The command codes at the top of the file are for
the Alliance AS5F18G04SND; change them for another part.

| step | question |
|---|---|
| 0 | did a marker from a previous run survive the power cycle? |
| 1 | what are the status registers as shipped, and is ECC on? |
| 2 | **can ECC be turned off and back on?** If not, stop — spare-area marking is impossible on this part |
| 3 | with ECC off, does a `0x00` byte programmed at the spare's column actually take? |
| 4 | with ECC back on, is that byte visible — or does the page now report an uncorrectable error? |
| 5 | the NOP question: can the spare be programmed *after* the main array has been? |
| 6 | how many blocks does the factory mark bad, and **how long does a full-array scan take?** |

**Step 4 has three possible outcomes and all three are informative:**

- the marker reads back as `0x00` with ECC on → the driver's marks work exactly as drawn;
- the marker is invisible but the page reports an uncorrectable error → still usable, because the scan
  already treats an unreadable page 0 as a bad block;
- the marker is invisible and the page reads clean → **negative result**, a retired block would be
  indistinguishable from a good one after a reboot, and the persisted table has to stay.

**Step 5 matters more than it looks.** A block gets retired because it has *just refused a program*, so the
marker often has to go into a page that already holds data. If the part allows only one partial program per
page, it cannot.

**Step 6 prints the deciding number.** A scheme with no persisted table would rebuild it from the spare area
on every boot, and that scan is what it costs; it is why the driver keeps the table as the authority and reads the
marks only when no table is left.

**Report back: every line from step 0 onwards.** Most of this test has no pass/fail verdict; the values are
the answer. Then **power-cycle, re-flash, and run it again** — the marker block is deliberately left
programmed at the end of the first run so the next boot's step 0 has something to find. Once step 0 has found
it, the run erases it, so the array is left as it was; a marker left behind permanently would show up as a
factory-bad block in every later survey.


## The two measurement programs

The tests above are gates. These two are not: they exist so that the whitepaper can report **measured** numbers
for the library instead of modelled ones. One is a benchmark that runs once and prints a table; the other is a soak that
runs for as long as you are willing to keep cutting the power.

> **Both write all over the log.** `nandlog_bench` erases the metadata ring and the whole log region before it
> starts. `nandlog_powercut` begins epochs of its own and sweeps whatever was there. **Development board only.**
> Neither erases the bad-block table or the OTP page (the library still records any block that fails while they
> run, as it always would), so a board goes back into service by flashing the normal firmware and beginning an epoch.

Build them from a SociTrack tree whose `src/external/nandlog` is this working tree.

### Building them

Two more targets in `software/firmware/tests/Makefile`, next to the three above, and both names added to
`.PHONY` and to the `all:` message:

```make
NANDLOG_ID_SOURCES = $(addprefix ../src/external/nandlog/,nandlog.c nandlog.h nandlog_conf.h nandlog_chip.h nandlog_port.h \
                     chips/nandlog_chip_AS5F18G04SND.c chips/nandlog_chip_W25N01GWZEIG.c) ../src/peripherals/src/storage.c
nandlog_powercut: TARGET = TestNandlogPowercut
nandlog_powercut: SRC += test_nandlog_powercut.c nandlog_hw_test.c
nandlog_powercut: CFLAGS += -DNANDLOG_SOURCE_ID=\"$(shell cat $(NANDLOG_ID_SOURCES) | shasum | cut -c1-12)\"
nandlog_powercut: $(CONFIG) $(CONFIG)/test_nandlog_powercut.o $(CONFIG)/nandlog_hw_test.o $(CONFIG)/$$(TARGET).bin program
$(CONFIG)/test_nandlog_powercut.o: $(NANDLOG_ID_SOURCES)

nandlog_bench: TARGET = TestNandlogBench
nandlog_bench: SRC += test_nandlog_bench.c nandlog_hw_test.c
nandlog_bench: CFLAGS += -DNANDLOG_SOURCE_ID=\"$(shell cat $(NANDLOG_ID_SOURCES) | shasum | cut -c1-12)\"
nandlog_bench: LFLAGS += -Wl,--wrap=nandlog_port_transfer_read,--wrap=nandlog_port_transfer_write,--wrap=nandlog_port_log,--wrap=nandlog_port_delay_us,--wrap=nandlog_port_delay_ms
nandlog_bench: $(CONFIG) $(CONFIG)/test_nandlog_bench.o $(CONFIG)/nandlog_hw_test.o $(CONFIG)/$$(TARGET).bin program
$(CONFIG)/test_nandlog_bench.o: $(NANDLOG_ID_SOURCES)
```

**The `NANDLOG_SOURCE_ID` lines name the build.** Every boot's `POWERCUT_START` line, and the bench's first
`BENCH_INFO library=` line, carries a fingerprint of the logging sources (the library, both drivers and the port),
and the last line of each target recompiles the test when any of them changes, so the fingerprint is never stale. `powercut_check.py` counts boots per build, so captures that span a
reflash are not mistaken for one library. Without these lines the soak still builds, and says `source=unrecorded`.

**The `LFLAGS` line is what makes the operation counts possible.** `nandlog_get_stats()` counts records and
pages, not what reached the part, so the bench has the linker route every call the core and the driver make to
five port functions through counting wrappers in the bench first (GNU ld `--wrap`). Nothing in the library, the
driver or `storage.c` changes. A target-specific `LFLAGS +=` reaches the link exactly as `ble_and_range`'s
`CFLAGS +=` reaches its compiles. If the line is left out the bench still links and runs; its banner then says
`op_counting=0`, there are no `BENCH_COUNT` lines, and the library's boot message is printed inside the timed
`nandlog_init()` (about 1 ms of SWO) instead of after it.

Then, as before:

```
cd software/firmware/tests
make nandlog_bench BOARD_REV=P
make nandlog_powercut BOARD_REV=P
```


## `nandlog_bench` — the whitepaper's numbers

> **DESTRUCTIVE: it erases the metadata ring and every block of the log region, then fills about 14,000 pages of
> it with benchmark data. Whatever epochs were on the part are gone.** It also puts roughly 700 erase cycles on
> the first three blocks of the log region per run, because 600 empty epochs begun back to back all start there.

Run time is **about three to six minutes**; it prints progress as it goes and ends with `BENCH_DONE`. Leave the
debugger attached (DWT has to count), and leave it alone until `BENCH_DONE`.

| phase | what it times | each as |
|---|---|---|
| A | status read; page latch + 32 B and + 4,096 B through the driver, programmed and erased pages; the latch and the two transfers separately; page-read busy time **ECC on and off**, programmed and erased, and by access pattern; page program through the driver, and split into load and busy; block erase (full block, empty block, through the driver); **internal page copy vs. read-then-write**; the **ECC-off blank check** recovery uses | `prim.*` |
| B | CRC-32 over a 4,064-byte payload and a 28-byte header: the library's software as built, `nandlog_crc32()` (the port's hook when `NANDLOG_PORT_CRC32` is set), and a byte-a-step reference over a table in flash; known vectors through all three | `crc.*`, `crc.hook.*`, `crc.flash_table.*` |
| B2 | the CPU work between flash commands, on page-sized buffers in RAM: `memcmp`, `memcpy` and `memset` as a commit uses them, and the blank check's scan through `nandlog_bytes_erased()` and as a plain byte loop | `cpu.*` |
| C | erasing the ring and the whole log region (an erase distribution over ~4,000 blocks), then `nandlog_init()` with no epoch | `region.erase.driver`, `init.no_epoch` |
| C2 | the parts of a warm boot that are not the log's own logic, each alone — the port's `nandlog_port_init()`, a power-down and power-up of the port, the driver's `nandlog_chip_init()` — then one boot with no epoch, and (in D) one with a ring of one epoch, traced for the eight widest stretches of CPU time between two port calls (`before_event` names the port call that ended each), with the boot's totals | `init.part.*`, `BENCH_GAP`, `BENCH_GAP_SUM` |
| D | `nandlog_init()` with **1, 64, 511 and 600** epochs in the ring (each epoch empty; a ring block holds 32 slots, each written to a pair of pages, so the ring has wrapped by 511), and every `begin_epoch()` on the way | `init.ring<k>.len0`, `begin_epoch.*` |
| E | `nandlog_init()` with a current epoch of **1,000 and 10,000** pages; `begin_reading()` unbounded, with an end bound (reads every header) and a start bound at the middle (reads half), with per-header cost; `read_span()` with bytes; `retrieve_next_page()` over every page; `retrieve_page_by_seq()` | `init.ring600.len*`, `begin_reading.*`, `retrieve_*` |
| F | `store_record()` for 32-, 256- and 1,024-byte records, **split by what each call did** — only buffered, committed a page, carried the erase-ahead, or both — over 1,280 pages each, outside a session as an application logs; the worst single call; `flush(true)` of a partial page; idle `flush(false)` | `store.*`, `flush.*` |
| F2 | where the time of one call goes: every port call inside one committing `store_record()` (32- and 1,024-byte records), one `flush(true)` of a partial page and one `retrieve_next_page()`, each with the CPU time before it, then the totals | `BENCH_TRACE`, `BENCH_TRACE_SUM` |

The format is fixed so the output can be pasted back and parsed:

```
BENCH <name> n=<n> min=<us> med=<us> p99=<us> max=<us> mean=<us> [extra=<k:v,...>]
BENCH_COUNT <name> n=<n> reads=.. raw_reads=.. programs=.. erases=.. polls=.. reg_writes=.. bytes_in=.. bytes_out=.. bus_us=.. delay_us=.. log_lines=..
```

A few things worth knowing when reading it:

- **Times** are microseconds to three decimals from DWT, converted with the `cycles_per_us` printed once at the
  top, which is calibrated against `am_hal_delay_us()` — so it is exactly as accurate as the core clock.
  Percentiles are nearest-rank, over every sample, or over a uniform 2,048-sample reservoir when there were more
  (`extra` then says `sampled:2048`); n, min, max and mean are always exact. Interrupts are on, as in the other
  tests, so p99 and max include whatever the board's interrupts cost.
- **`BENCH_COUNT` values are means per operation.** `reads` are page latches (PAGE DATA READ), of which
  `raw_reads` had ECC off; `programs` are PROGRAM EXECUTEs, internal copies included; `polls` are status reads;
  `bus_us` is time inside the port's transfer functions and `delay_us` time inside its delays — the BUSY-poll
  pacing, and the fixed 3 ms the driver waits in `nandlog_chip_init()`, which is in every warm boot.
- **The page-read busy time is measured two ways, which bracket it.** `prim.tR.poll.*` runs from the end of the
  PAGE DATA READ transfer to the end of the first status read that says READY: an upper bound, by at most about
  two status reads. `prim.tR.bisect.*` is the smallest delay, found to 0.25 µs, after which a single status read
  says READY: tR lies between that minus 0.25 µs and that plus one status read (`prim.status_read`). Program and
  erase busy times are polled the same way, from the end of their command. On the AS5F18G04SND the two do not
  bracket one value: with ECC on, polling back to back sees READY about 255 µs after the command and a single read
  after a quiet wait sees BUSY until about 360 µs, while a whole 32-byte read through the driver takes about 270 µs.
  So `prim.tR.pattern.*` times the latch to READY eight ways: with or without 32 bytes clocked out between latches
  (`readout`, `no_readout`), polls back to back or `NANDLOG_BUSY_POLL_INTERVAL_US` apart (`back_to_back`, `paced`),
  over consecutive pages or pages 7 apart (`consecutive`, `stride7`).
- **Driver against bare port.** `prim.read.*` go through the driver, whose BUSY wait polls every
  `NANDLOG_BUSY_POLL_INTERVAL_US` (10 µs); `prim.latch_and_poll` and `prim.tR.*` poll without pacing. The
  difference is what the pacing costs per operation.
- **CRC.** Phase B first prints `BENCH_INFO crc_hook=<0|1> crc_hw_selftest=<passed|failed|none|unknown>
  crc_table_in_ram=<0|1> crc_slice_by_4=<0|1>`: whether the log's CRC goes through the port, and whether the
  port's hardware passed the check it ran at `nandlog_port_init()` (from `nandlog_port_crc32_hardware()`, where the
  port has it; `none` without the hook). Then known vectors — `"123456789"` → `CBF43926` at every alignment, and a
  4,064-byte payload whole and continued from odd splits — through `nandlog_crc32()`, `nandlog_crc32_software()`
  and the bench's own reference, ending in `BENCH_INFO crc_vectors_ok=1 checked=<n> failed=0` (a disagreement is a
  `BENCH_ERROR`). Then three pairs of timings: `crc.payload_4064B` and `crc.header_28B` are the library's
  software as built (`extra` says where its table is and whether it slices), `crc.hook.*` are `nandlog_crc32()` —
  the CRC the log actually uses, the port's hook in a build with `NANDLOG_PORT_CRC32` — and `crc.flash_table.*`
  are the reference: a byte a step over a constant table in flash, which is what the software does with
  `NANDLOG_CRC_TABLE_IN_RAM` 0. Phase E checks the reference against the CRCs the library wrote into a page and
  prints `crc_copy_matches_library=1`. A store adds its record to the page's CRC, so the commit-path figures of
  phase F carry the header's CRC alone, and the buffer-only figures each record's.
- **Scratch blocks and marked blocks.** Phase A uses the top good, unmarked blocks of the reserve. On a part with
  fewer than four of those, it takes the rest from the top of the log region, which phase C erases anyway, and
  says so. `BENCH_INFO scratch_blocks=` names the four. `BENCH_INFO marked_blocks when=start|end` gives the
  count, lowest, highest and runs of blocks whose first spare byte reads other than `FFh` with ECC off, from one
  raw read per block (about 2 s), so a marked region that grows between runs shows. Neither ever touches a block
  the bad-block table names, so on a part whose reserve is dead the pair of log-region blocks the table was moved
  to is left alone, by phase A and by phase C's wipe alike.
- **A dead reserve.** On a part whose reserve cannot hold the bad-block table, the first boot rebuilds it from the
  marks (one raw read per block, about 1.8 s), is given an aligned pair of blocks at the top of the log region,
  and logs `WARNING: Bad-block table moved below the reserve, blocks N-N+1`; every boot after reads one header a
  block down to 48 blocks below the reserve instead of every block's mark, and logs `INFO: Bad-block table below
  the reserve, block N`. The `init.*` figures are then that part's warm boots, not a rebuild. `BENCH_INFO
  marked_blocks` does not count the pair, which carries no mark; `nandlog_bad_block_count()` does.
- **The counting wrappers cost about a cycle count and an add per bus transfer.** `prim.status_read` and
  `prim.status_read.via_counting_wrapper` measure that directly. The bench's own register-level measurements
  bypass the wrappers; everything through the driver or the library includes them.

### Energy (optional)

Build with a marker pin and every measured operation is bracketed by that GPIO: high for exactly the span DWT
times, then low for `BENCH_MARKER_GAP_US` (200 µs by default) before the next one.

```
make nandlog_bench BOARD_REV=P EXTRA_DEFINES=-DBENCH_MARKER_PIN=<gpio>
```

**There is no default pin, on purpose.** Choose a pad you can reach with a probe on your board revision that
nothing else drives (`src/boards/rev<X>/pinout.h` lists what is taken). Defining `BENCH_MARKER` without a pin,
or the pin with no number, stops the build with an `#error`.

To use it, put the analyzer (Joulescope, Otii, PPK2 or similar) in series with the board's supply and its
digital input on the marker. The bench opens with one second idle with the part awake, then one second with it
asleep, both with the marker low: those are the baselines. Each `BENCH` line follows its burst of n pulses. The
energy of one operation is then V × (mean current while the marker is high − the awake baseline) × the pulse
width, averaged over the burst. The gaps make the run slower, by a minute or so.

### Report back

The **whole serial output**, from the reset banner to `BENCH_DONE`, verbatim. Every number in it is labelled.


## `nandlog_powercut` — the durability contract under real power cuts

> **It begins epochs of its own and logs continuously, so the log region is written over and over. Development
> board only, and with the battery disconnected** — otherwise cutting the supply only switches the board to its
> battery.

On every boot it:

0. prints `POWERCUT_START nandlog=<major.minor> source=<id> port_crc=<0|1> erase_ahead=<W> head_pass=<0|1>`, which
   names its build;
1. brings the log up, timing `nandlog_init()` — which after a cut is the recovery;
2. reads the **whole current epoch** back through the public API and checks every record against the pattern
   it was written from. Each record carries the run (boot) that wrote it and its index in that run, and every
   other byte follows from those two through a small PRNG, so damage, reordering and duplication all show;
3. prints one `POWERCUT` line, protected by a CRC so a line a cut tore is never misread:

   ```
   POWERCUT boot=<run> epoch=<e> foreign=<0|1> runs=<k> records=<n> pages=<p> gaps=<g> tombstones=<t> damaged=<d> out_of_order=<o> dup=<u> holes=<h> skipped=<s> retired=<r> init_us=<us> verify_us=<us> per_run=<run:count[:prefix],...> crc=<8 hex>
   ```

4. logs records of mixed sizes (mostly small, some up to ~2 KB), with partial-page flushes and idle flushes at
   random intervals, printing `POWERCUT_ACK epoch=<e> run=<r> upto=<i>` every time `flush(true)` returns
   `NANDLOG_OK` — records 0..i of that run are then durable — until the power goes.

The run number needs no NVM of its own: each run's first record is a boot marker, made durable at once, and the
next boot takes one more than the highest run it finds. A boot cut before its marker landed left nothing, so its
number is simply used again. The epoch is replaced at boot once it reaches 2,048 pages or 48 runs, so a
read-back takes a few seconds at most. An epoch the soak did not begin — the benchmark's, or SociTrack's — is
reported `foreign=1`, not checked, and replaced.

`gaps` and `skipped` are expected to be non-zero: a cut in the middle of a program leaves a torn page, which the
next boot passes over (`skipped`) and every read after that serves as a gap. `damaged`, `out_of_order` and `dup`
must stay at zero for ever.

### Capturing the log across cuts

The capture has to outlive the board's power. SWO through the J-Link is fine as long as **the probe does not
power the target** and the viewer keeps running across the target losing and regaining power — try a few cuts
by hand first and check the capture picks up again after each. Something like
`JLinkSWOViewerCL -device AMA4B2KK-KBR -swofreq 1000000 -itmport 0 > powercut.log` (the device name is the one
`../Jtag.mk` uses; adjust for your setup), restarted in a shell loop if your version exits when the target goes
away.

**Check that the cut really is a cut.** Every boot prints its reset reason, and after a real cut it should say
`HW Power-On Reset`. The SWD lines of an attached probe, or a bulk capacitor, can hold the MCU up long enough
that the "cut" becomes a brown-out or nothing at all; the checker tallies the reset reasons it sees so this
shows. Leave the supply off for at least a couple of seconds, longer if the rail is slow to fall (put a scope on
it once).

### Automating the cuts

Anything that switches the board's only supply on and off at random will do. Two easy ways:

- **A relay or a MOSFET in the supply line, driven by a second board.** On-time drawn uniformly from about
  0.3 s to 20 s — short ones land in the boot's own recovery and read-back, which matter as much as cuts while
  logging — and an off-time of 2–3 s.
- **A USB hub with per-port power switching, driven by [`uhubctl`](https://github.com/mvp/uhubctl)**, with the
  board powered from that port alone:

  ```bash
  while true; do
     uhubctl -l <hub> -p <port> -a on  > /dev/null
     sleep "$(python3 -c 'import random; print(round(random.uniform(0.3, 20), 2))')"
     uhubctl -l <hub> -p <port> -a off > /dev/null
     sleep 3
  done
  ```

  Check with `uhubctl` alone first that the hub really removes VBUS; many hubs only pretend to.

**A thousand cuts is the goal. Any number is useful** — the checker reports exactly how many it saw, how many
landed while logging and how many during the boot's own recovery and read-back.

### Checking it

```
python3 tests/powercut_check.py powercut.log
```

It reads the capture, skips anything torn or garbled, and checks the contract across boots: no damaged,
duplicated or out-of-order record is ever delivered; between two boots of the same epoch no run's record count —
nor the stretch of it unbroken from index 0 — ever goes down (that would be recovery destroying data an earlier
boot had already read back); and every acknowledged record is still there at every later boot of that epoch. It
ends `RESULT: PASS` or `RESULT: FAIL` with every violation listed, and also prints the distribution of
`nandlog_init()` time after a cut, which is the recovery figure for the paper. `--verbose` adds a line per boot.

### Report back

The captured serial log, or at least the checker's whole output; how many cuts, and how they were made (relay
or hub, on- and off-time ranges); and whether every boot said `HW Power-On Reset`.

### Running it unattended: `nandlog_selfcut` and `nandlog_selfcut_inject`

Cuts made by hand land where the board spends its time: while logging, or in the read-only read-back. They seldom
land in a boot's own slot write or erases, and practically never in a relocation or a retirement, which happen only
when a block fails. Two build options of the same file reach those paths with nobody at a switch. Both are 0 by
default, so `nandlog_powercut` and `make soak` are exactly as above.

- **`PC_SELF_CUT=1`: the firmware stops its own operations.** Every PROGRAM EXECUTE and BLOCK ERASE is counted from
  the start of the boot, the boot's own slot, table and window writes included, and one of them is stopped
  part-way. The command goes out, then a random delay passes on the cycle counter with interrupts off: from 0 to
  110% of that operation's busy time on this part, about 620 µs for a program and 3.4 ms for a full block's erase.
  RESET (`FFh`) then goes out through the real port, the status register is polled until OIP clears, a
  `POWERCUT_SELFCUT` line is printed, and the MCU resets itself. The next boot recovers from an interrupted
  operation with everything in RAM lost, as after a power cut. **The part stays powered, though, so this is a
  RESET-interrupted operation, a proxy for a brown-out and not a power cut**, the mechanism `nandlog_interrupt`
  characterizes. About a third of the cuts land among the boot's own operations (`nandlog_init()`, and a new
  epoch's writes); the rest land in logging, mostly early in it. One boot in eight is cut late, so that epochs grow
  to thousands of pages and, over a run, the log wraps its region.
- **`PC_INJECT_FAILURES=1`: now and then a completed operation reports failure.** After a log page's program, or
  an erase of a log block, P_FAIL or E_FAIL is ORed into the status byte the driver reads once the part is ready.
  **The failure is injected.** Everything after it runs on the part, in the library and the driver: the block's
  pages moved through the part's internal copy, the block retired with a new table version and a mark, the ring
  told. A block that fails an erase fails every erase of it for the rest of that boot, so the driver's three
  attempts all fail and the block is retired. Injections come at most one per 300 operations, and stop once
  `nandlog_bad_block_count()` reaches 24. That leaves the Alliance driver's 256-entry table far from full, so the
  board is not used up. At these rates the 24 come within the first few dozen boots of a run;
  `EXTRA_DEFINES=-DPC_INJECT_ODDS=3000` on the `make` line spreads them ten times wider. Combined with self-cut,
  one injection in two moves that boot's cut into the operations answering it, so cuts land inside relocations and
  retirements.

Both options wrap the port's two transfer functions at link time, as the bench does. Nothing in the library, the
drivers or `storage.c` changes, and a build linked without the `--wrap` flags says so at its first boot and stops.
Two more targets in `software/firmware/tests/Makefile`, after `nandlog_powercut`, whose `NANDLOG_ID_SOURCES` they
reuse. Their names go in `.PHONY` and in the `all:` message. Each variant compiles the soak into an object of its
own, so building one never leaves its object where another's image links it:

```make
nandlog_selfcut: TARGET = TestNandlogSelfCut
nandlog_selfcut: SRC += nandlog_hw_test.c
nandlog_selfcut: OBJS += $(CONFIG)/test_nandlog_selfcut.o
nandlog_selfcut: CFLAGS += -DNANDLOG_SOURCE_ID=\"$(shell cat $(NANDLOG_ID_SOURCES) | shasum | cut -c1-12)\"
nandlog_selfcut: LFLAGS += -Wl,--wrap=nandlog_port_transfer_read,--wrap=nandlog_port_transfer_write
nandlog_selfcut: $(CONFIG) $(CONFIG)/test_nandlog_selfcut.o $(CONFIG)/nandlog_hw_test.o $(CONFIG)/$$(TARGET).bin program

nandlog_selfcut_inject: TARGET = TestNandlogSelfCutInject
nandlog_selfcut_inject: SRC += nandlog_hw_test.c
nandlog_selfcut_inject: OBJS += $(CONFIG)/test_nandlog_selfcut_inject.o
nandlog_selfcut_inject: CFLAGS += -DNANDLOG_SOURCE_ID=\"$(shell cat $(NANDLOG_ID_SOURCES) | shasum | cut -c1-12)\"
nandlog_selfcut_inject: LFLAGS += -Wl,--wrap=nandlog_port_transfer_read,--wrap=nandlog_port_transfer_write
nandlog_selfcut_inject: $(CONFIG) $(CONFIG)/test_nandlog_selfcut_inject.o $(CONFIG)/nandlog_hw_test.o $(CONFIG)/$$(TARGET).bin program

$(CONFIG)/test_nandlog_selfcut.o: SOAK_MODE = -DPC_SELF_CUT=1
$(CONFIG)/test_nandlog_selfcut_inject.o: SOAK_MODE = -DPC_SELF_CUT=1 -DPC_INJECT_FAILURES=1
$(CONFIG)/test_nandlog_selfcut.o $(CONFIG)/test_nandlog_selfcut_inject.o: test_nandlog_powercut.c $(NANDLOG_ID_SOURCES)
	@echo " Compiling $< as $(@F)" ;\
	$(CC) -c $(CFLAGS) $(EXTRA_CFLAGS) $(SOAK_MODE) $< -o $@

$(CONFIG)/TestNandlogSelfCut.axf: $(CONFIG)/test_nandlog_selfcut.o $(CONFIG)/nandlog_hw_test.o
$(CONFIG)/TestNandlogSelfCutInject.axf: $(CONFIG)/test_nandlog_selfcut_inject.o $(CONFIG)/nandlog_hw_test.o
```

**Randomness that survives the reset.** The run number cannot choose the cut: a boot cut before its marker landed
uses its run again, and would be cut at the same point every time. The random state lives instead in a word of TCM
in the linker script's `.noinit` section, which the startup code neither copies nor zeroes. The firmware's radio
test carries its device list across a reset the same way. A CRC tells a state a reset left alone from the garbage a
power-on leaves. The STIMER's count is mixed in at every boot. The HAL documents a software or AIRCR reset as
resetting every block except the clock generator, the RTC, the STIMER and the PMU, so once started (by the soak, if
nothing else has) the counter runs on through every reset, and no two boots read the same count. Either source alone
keeps the run from repeating itself. `retained=` on each `POWERCUT_SELFCUT` line counts the boots in a row the TCM
word survived, and the checker reports how many cuts had it, which shows on the board whether it does.

**What it adds to the output.** `POWERCUT_START` gains `self_cut=<0|1> inject=<0|1>`; the plain soak's line is
unchanged. Every other line keeps its meaning. These two are new, CRC-protected as the others are:

```
POWERCUT_SELFCUT op=<index> cmd=<program|erase> phase=<init|logging> plan=<init|early|late|recovery> target=<page|slot|table|copy|other|log|ring|reserve> block=<b> delay_us=<us> reset_us=<us> ready=<0|1> retained=<n> crc=<8 hex>
POWERCUT_INJECT op=<index> cmd=<program|erase> phase=<init|logging> block=<b> retired=<r> crc=<8 hex>
```

`target` is what the stopped operation was writing. For a program it is a log page or tombstone, a metadata slot, a
bad-block table version, the part's internal copy during a relocation, or `other` (a mark). For an erase it is
where the block lies. A boot left with nothing to do, such as one that reached the soak's page cap, resets itself
after two seconds idle. A boot that stopped on an error (`POWERCUT_ERR`) makes the next boot begin a new epoch, so
the run carries on. `powercut_check.py` checks the contract exactly as for hand cuts, and also tallies the
`SELFCUT` lines by phase, command, target and plan, with the delays before RESET and RESET's own busy time. It
counts the `INJECT` lines and reports `retired=` from first boot to last. A retirement after an injection is the
failure path doing its job, not a violation. Each boot's reset reason is a software reset, not `HW Power-On Reset`.

**Running it.** One board, left powered. The battery can stay connected, since no supply is cut. Capture SWO
exactly as for hand cuts, the viewer loop included, since the MCU resets at every cut. Then leave it. A boot takes
from a tenth of a second (cut in `nandlog_init()`) to some tens of seconds (a full read-back, then a late cut), so
two thousand cuts should take about five hours by the bench's figures.

```
make nandlog_selfcut BOARD_REV=P              # or nandlog_selfcut_inject
python3 tests/powercut_check.py selfcut.log
```

**Report back:** the checker's whole output, or the capture; which build ran, on which board, and for how long.
Neither build has run on silicon yet.


## `nandlog_interrupt` — what an interrupted program or erase leaves on the part

> **It programs and erases six blocks in the middle of the log region — the first six good, unmarked blocks from
> block 2000 up — about 35 times each, and leaves them erased. Whatever the log held there is gone, so begin an
> epoch (or run the bench, which wipes the log region) before trusting that board's log again. Development board
> only.** It never touches the metadata ring, the reserve, or the 48 blocks under the reserve that the bad-block
> table may be moved to; it steps over any block the table names or that carries a mark, and it retires nothing.

**Why.** The library's durability is tested in the simulator, which models what an interrupted PROGRAM or BLOCK
ERASE leaves on the part: a torn page that reads uncorrectable, a partly programmed page, weak pages that drift, an
interrupted erase whose block reads erased yet programs unreliably, and reads that differ from one to the next. None
of those models has been calibrated against silicon, and this test supplies the numbers. It also measures the one
residual risk the library counts: a page an interrupted program left reading erased in every bit passes the
recovery's blank check and is programmed a second time after the reboot, on a part that allows one program per page.
How often that happens, and what such a second program reads back as, is the number that matters most.

**How.** The datasheet says RESET (`FFh`) "stops all operations" (section 9, p. 36), so RESET issued a set time after
PROGRAM EXECUTE or BLOCK ERASE stops the operation at a chosen point, as often as wanted, with the board powered
throughout. That is a proxy for a brown-out, not a substitute for one: a sagging supply takes the part's charge pumps
down with it over microseconds to milliseconds, where RESET stops the state machine at once on a healthy rail. The
soak above remains what tests the library against real cuts. The test reaches the part through the port with raw
commands; it brings up the driver only so the bad-block table can be asked about a block, and calls nothing in the log.

| sweep | what is stopped | delays | per delay | recorded per trial |
|---|---|---|---|---|
| program | PROGRAM EXECUTE of a pseudo-random page, about half its bits zero, ECC on as the log programs | 21, from 0 to 1,000 µs, dense below 700 (a program takes about 620 µs) | 32 pages in order, in a freshly erased block | delay, RESET busy time, P_FAIL; three raw reads (bits programmed where the pattern has zeros and where it has ones, spare bits, erased in every bit or not, bits that differ between reads); a read with ECC on (ECC status, matches or not); **if the first raw read was erased in every bit, the same pattern programmed again to completion and read back** |
| erase | BLOCK ERASE of a block filled with 64 patterned pages | 14, from 0 to 5,000 µs (a full block's erase took 2.7–3.4 ms on the bench) | 3 blocks | delay, RESET busy time, E_FAIL; every page's raw zero-bit count and ECC status; then, **without erasing again**, the first and the last page programmed and read back with ECC on and raw; then the block erased whole |

Before the erase sweep it times twelve whole-block erases, and every program that fills a block, so the delays can be
read against what an operation nobody stops takes on those blocks that day (`INTR_REF`). At the end it erases every
block once more and reads every page raw (`INTR_FINAL`). The run takes under a minute, and restores ECC on and write
protection on every way out.

Every line meant to be parsed starts `INTR_` and holds `key=value` fields; the header of `test_nandlog_interrupt.c`
defines each one:

```
INTR_START nandlog=<major.minor> source=<id> part_id=<hex> device_id=<hex> ...
INTR_PROG trial=<n> nominal_us=<d> block=<b> page=<p> delay_us=<us> reset_us=<us> ... class=<blank|partial|complete> ecc=<status> match=<0|1> second=<0|1> [second_ecc=<status> second_match=<0|1> ...]
INTR_PROG_SUM nominal_us=<d> n=<n> ... blank=.. partial=.. complete=.. ecc_*=.. unstable=.. second_clean=.. second_mismatch=..
INTR_ERASE trial=<n> nominal_us=<d> block=<b> delay_us=<us> ... blank_pages=<n> first_ecc=<status> first_match=<0|1> last_... zeros=<64 counts> ecc_map=<64 digits>
INTR_ERASE_SUM nominal_us=<d> n=<n> ... blank_blocks=.. blank_pages=.. first_clean=.. last_clean=..
INTR_REF program_busy|erase_busy_full_block n=<n> min=<us> med=<us> max=<us> mean=<us>
INTR_FINAL block=<b> ... blank_pages=<n> pages=<n>
INTR_DONE errors=<n> aborted=<0|1> ... elapsed_s=<s>
```

A few things worth knowing when reading it:

- **`delay_us`** is from the return of the PROGRAM EXECUTE or BLOCK ERASE transfer to the call of RESET's, on the DWT
  cycle counter, with interrupts off. The port's own call overhead (a microsecond or two) is not subtracted, so a
  nominal 0 is the shortest gap the port allows.
- **`class`**: `blank` is every bit of main and spare erased, which is what the recovery's blank check passes;
  `complete` is the main array reading the pattern bit for bit; `partial` is anything else.
- **ECC status** comes from a fresh page read with ECC on, never from the RESET: the datasheet sets ECCS to 00b at a
  RESET and again at the start of every read.
- **`p_fail` and `e_fail`** after a RESET are expected to be 0, because the datasheet says RESET clears both; a 1 is
  news. **`a0` and `b0`** are the protection and configuration registers as RESET left them. Whether RESET puts back
  their power-on values (`38h`, `10h`) is not in the datasheet. The test sets them back either way.

### Building and running it

One more target in `software/firmware/tests/Makefile`, after `nandlog_powercut`, whose `NANDLOG_ID_SOURCES` it
reuses so that its captures name their build the same way; and its name in `.PHONY` and in the `all:` message:

```make
nandlog_interrupt: TARGET = TestNandlogInterrupt
nandlog_interrupt: SRC += test_nandlog_interrupt.c nandlog_hw_test.c
nandlog_interrupt: CFLAGS += -DNANDLOG_SOURCE_ID=\"$(shell cat $(NANDLOG_ID_SOURCES) | shasum | cut -c1-12)\"
nandlog_interrupt: $(CONFIG) $(CONFIG)/test_nandlog_interrupt.o $(CONFIG)/nandlog_hw_test.o $(CONFIG)/$$(TARGET).bin program
$(CONFIG)/test_nandlog_interrupt.o: $(NANDLOG_ID_SOURCES)
```

It is written for the AS5F18G04SND and checks the geometry before it writes anything. The board stays powered, with
the debugger attached (DWT has to count). Capture SWO to a file, as for the bench, and leave it until `INTR_DONE`:

```
JLinkSWOViewerCLExe -device AMA4B2KK-KBR -cpufreq 96000000 -swofreq 1000000 -itmmask 0x1 | tee interrupt.log
make nandlog_interrupt BOARD_REV=P
python3 tests/interrupt_summary.py interrupt.log
```

`interrupt_summary.py` tabulates both sweeps by delay, then prints the headline fractions:

- how often an interrupted program left the page reading erased in every bit, by delay;
- how often a second program over such a page read back clean, corrected, uncorrectable, or **readable but wrong**;
- what partly programmed pages read as, and how often raw reads of a page disagreed;
- how often an interrupted erase left a block reading erased, and what programs into its first and last pages read
  back as.

It skips any line SWO garbled, checks its own tallies against the device's per-delay summaries, and ends with a
`CHECKS` block. It exits 0 if everything agreed, 1 if anything disagreed or the device reported an error, and 2 if
there was nothing to tabulate. Repeated runs add statistics: each starts from freshly erased blocks, and the script
counts every run in the captures it is given.

### Report back

The whole capture, from the reset banner to `INTR_DONE`, or at least `interrupt_summary.py`'s whole output, and the
board it ran on.


## What to send back, all together

- [ ] **`nandlog_bench`**: the full serial output, banner to `BENCH_DONE`, verbatim. Say whether the `LFLAGS`
      line was in (the `op_counting=` field of the banner says so too).
- [ ] **`nandlog_powercut`**: the captured serial log, or `powercut_check.py`'s output for it; the number of cuts
      and how they were made.
- [ ] **`nandlog_selfcut` / `nandlog_selfcut_inject`**: `powercut_check.py`'s output, or the capture; which build,
      which board, and how long it ran.
- [ ] **`nandlog_interrupt`**: the capture, banner to `INTR_DONE`, or `interrupt_summary.py`'s output for it; the
      board it ran on.
- [ ] **The setup, once**: board revision; supply voltage, and where it was measured; ambient temperature; the SPI
      clock (48 MHz in `storage.c` unless you changed it); the compiler version and the flags the Makefile used
      (`-Ofast` at the time of writing); the commits of the nandlog and SociTrack trees.
- [ ] **If you captured energy**: the analyzer, its sample rate, and its per-pulse or per-burst export.
