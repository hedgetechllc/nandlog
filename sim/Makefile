# Host build of the nandlog core over the RAM-backed port. No target toolchain, no hardware.
#   make               the suite and the regression tests, in every configuration, then the Alliance driver's bad-block
#                      answers against a search of its table, then the reference parser checked against the images the
#                      suite dumps. Every build here checks each CRC the log carries forward against a recompute
#                      (NANDLOG_CHECK_CRC in nandlog.c)
#   make campaign      the crash-point campaign and the recovery and metadata sweeps, in parallel, in four builds:
#                      as shipped, with an erase-ahead window of 0 blocks, with NANDLOG_PASS_OVER_HEAD_AT_BOOT
#                      (traces only: that option moves the head positions the sweeps pin down), and on a part whose
#                      reserve's table blocks and region's top two blocks are dead. campaign_default, campaign_w0,
#                      campaign_headpass and campaign_dead run one each
#   make fuzz          randomized calls, faults and power cuts against an independent oracle, from fixed seeds, in nine
#                      builds, the window of 0, the head pass-over, two parts whose reserve is dead -- the table moved
#                      below it, and no room for it anywhere -- and the port's CRC among them
#   make exhaustive    every combination of page states below the write head of a toy part, booted and checked, and
#                      every metadata write path cut at each of its operations: as shipped, with a window of 0, and
#                      (metadata paths only) with the head pass-over, and on parts whose reserve is dead -- the table
#                      moved below it, and no room for it anywhere
#   make opcounts      the flash operations and bus bytes of each call, and its reads split by how much each clocks out;
#                      then the boots of parts shaped like the hardware benchmark's boards
#   make windows       the regression tests again with an erase-ahead window of 0 blocks, and of 1; and the K tests,
#                      which move the bad-block table, at a window of 3
#   make headpass      the head pass-over's share of the above: its exhaustive metadata paths and its campaign
#   make soak          the on-device power-cut soak, tests/test_nandlog_powercut.c, built unchanged for the host and
#                      run for a thousand boots over the simulator, cut at random, then checked by powercut_check.py
#   make ablation      each NANDLOG_MUTATE_* build, which takes one mechanism away, through the regression tests, the
#                      exhaustive check, a short campaign subset and the fuzzer: a table of which checks notice (ablation.sh)
#   make baselines     the comparison with Dhara and littlefs: fetches both at pinned commits into baselines/third_party
#                      (never kept in the repository), measures footprint if arm-none-eabi-gcc is present, and counts
#                      every stack's flash operations over the same driver and simulated part, then models a durable
#                      flush cycle's time and energy from them and board C's primitives (baselines/reproduce.sh)
#   NANDLOG_VERBOSE=1 ./nandlog_sim_regress <name>   one regression test, with the library's diagnostics
#
# One target per table of the white paper's evaluation: the crash-point campaign (make campaign), the metadata write
# paths and bounded-exhaustive recovery (make exhaustive), operation counts with reads split by length (make opcounts),
# the comparison with Dhara and littlefs (make baselines), the ablation (make ablation) and the soak (make soak)

CC       = cc
CFLAGS   = -std=c99 -Wall -Wextra -Wno-unused-parameter -O1 -g -fsanitize=address,undefined -DNANDLOG_CHECK_CRC=1
FAST     = -std=c99 -Wall -Wextra -Wno-unused-parameter -O2 -g -DNANDLOG_CHECK_CRC=1
INCLUDES = -I. -I.. -I../chips
PORT     = nandlog_port_sim.c
CORE     = ../nandlog.c
ALLIANCE = ../chips/nandlog_chip_AS5F18G04SND.c
WINBOND  = ../chips/nandlog_chip_W25N01GWZEIG.c
DEPS     = ../nandlog.h ../nandlog_chip.h ../nandlog_conf.h ../nandlog_port.h ../chips/nandlog_chip_common.h nandlog_port_sim.h $(PORT) $(CORE)
PYTHON   = python3
JOBS     = 12

# One campaign run: the build, where its output goes, and which of its jobs run -- all, or the traces alone
CAMPAIGN_JOBS = .
CAMPAIGN_OUT  = campaign.out

# Five configurations. Both settings of NANDLOG_RECORD_FRAMING; a build that relocates pages by reading and
# writing them, with room in RAM for only one retired block so the path that asks the driver block by block runs
# too; the Winbond driver, whose bad blocks live in the part's own remap table; and a build whose CRCs all come from
# the port (NANDLOG_PORT_CRC32), which here continues the software CRC in deliberately odd pieces -- with the software
# slicing four bytes a step from a table left in flash, the two settings the others do not take. The regression tests
# build each over a small part -- a 168-block log region on the Alliance part (192 blocks: the ring, the region, the
# 16-block reserve), 208 on the Winbond (256 blocks) -- so a test can fill the whole region in a fraction of a second
PLAIN    = -DNANDLOG_RECORD_FRAMING=0
FRAMED   = -DNANDLOG_RECORD_FRAMING=1
NOCOPY   = -DNANDLOG_RECORD_FRAMING=1 -DNANDLOG_CHIP_PAGE_COPY=0 -DNANDLOG_MAX_TRACKED_RETIRED_BLOCKS=1
WINBONDC = -DNANDLOG_RECORD_FRAMING=1 -DSIM_CHIP_WINBOND
PORTCRC  = -DNANDLOG_RECORD_FRAMING=1 -DNANDLOG_PORT_CRC32=1 -DNANDLOG_CRC_SLICE_BY_4=1 -DNANDLOG_CRC_TABLE_IN_RAM=0
SMALL    = -DNANDLOG_CHIP_BLOCK_COUNT=192
SMALL_WB = -DNANDLOG_CHIP_BLOCK_COUNT=256
MEDIUM   = -DNANDLOG_CHIP_BLOCK_COUNT=64
TOY      = -DNANDLOG_CHIP_PAGES_PER_BLOCK=4 -DNANDLOG_CHIP_BLOCK_COUNT=29
# The toy part with nine log blocks and its whole reserve dead, the table's search reaching the region's top eight: the
# table moves to a pair of them. And the same with the region's top four dead too, the search reaching only dead blocks:
# the marks stand in for the table throughout
TOY_DEAD = -DNANDLOG_CHIP_PAGES_PER_BLOCK=4 -DNANDLOG_CHIP_BLOCK_COUNT=33 -DSIM_DEAD_RESERVE_BLOCKS=16 -DNANDLOG_CHIP_TABLE_SEARCH_BLOCKS=24
TOY_DEGRADED = -DNANDLOG_CHIP_PAGES_PER_BLOCK=4 -DNANDLOG_CHIP_BLOCK_COUNT=33 -DSIM_DEAD_RESERVE_BLOCKS=16 -DSIM_DEAD_LOG_BLOCKS=4 \
               -DNANDLOG_CHIP_TABLE_SEARCH_BLOCKS=20
CAMPAIGN_PART = -DNANDLOG_CHIP_BLOCK_COUNT=128 -DNANDLOG_CHIP_RESERVED_BLOCKS=80
# The campaign's part with the reserve's eight table blocks and the region's top two blocks dead, as one board was found,
# and the table's search reaching 96 blocks down: the first boot moves the table to the highest pair below them
CAMPAIGN_DEAD = -DSIM_DEAD_RESERVE_BLOCKS=8 -DSIM_DEAD_LOG_BLOCKS=2 -DNANDLOG_CHIP_TABLE_SEARCH_BLOCKS=96

SUITES   = nandlog_sim_test nandlog_sim_test_framed nandlog_sim_test_no_chip_copy nandlog_sim_test_winbond nandlog_sim_test_port_crc
BADBLOCKS = nandlog_sim_badblocks nandlog_sim_badblocks_small
REGRESS  = nandlog_sim_regress nandlog_sim_regress_framed nandlog_sim_regress_no_chip_copy nandlog_sim_regress_winbond \
           nandlog_sim_regress_port_crc
CAMPAIGN = nandlog_sim_campaign
EXHAUSTIVE = nandlog_sim_exhaustive
OPCOUNTS = nandlog_sim_opcounts
FUZZ     = nandlog_sim_fuzz nandlog_sim_fuzz_small nandlog_sim_fuzz_no_chip_copy nandlog_sim_fuzz_winbond nandlog_sim_fuzz_w0 \
           nandlog_sim_fuzz_headpass nandlog_sim_fuzz_dead nandlog_sim_fuzz_degraded nandlog_sim_fuzz_port_crc

# The fuzzer's seeds and run length. Each seed also picks a mode: mostly small records, mostly page-sized ones, or
# epochs begun rarely, so that some runs fill and wrap the region
FUZZ_SEEDS = 1 2 3 4 5 6 7 8 9 10 11 12
FUZZ_STEPS = 12000

.PHONY: all run parse campaign campaign_default campaign_w0 campaign_headpass campaign_dead campaign_run fuzz exhaustive opcounts windows \
        headpass ablation soak baselines clean
all: run

nandlog_sim_test: nandlog_sim_test.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(PLAIN) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_test_framed: nandlog_sim_test.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_test_no_chip_copy: nandlog_sim_test.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(NOCOPY) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_test_winbond: nandlog_sim_test.c $(DEPS) $(WINBOND)
	$(CC) $(CFLAGS) $(WINBONDC) $(INCLUDES) $(PORT) $< $(CORE) $(WINBOND) -o $@
nandlog_sim_test_port_crc: nandlog_sim_test.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(PORTCRC) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@

nandlog_sim_regress: nandlog_sim_regress.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(PLAIN) $(SMALL) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_regress_framed: nandlog_sim_regress.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(SMALL) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_regress_no_chip_copy: nandlog_sim_regress.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(NOCOPY) $(SMALL) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_regress_winbond: nandlog_sim_regress.c $(DEPS) $(WINBOND)
	$(CC) $(CFLAGS) $(WINBONDC) $(SMALL_WB) $(INCLUDES) $(PORT) $< $(CORE) $(WINBOND) -o $@
nandlog_sim_regress_port_crc: nandlog_sim_regress.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(PORTCRC) $(SMALL) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@

# The campaign runs unsanitized and optimized, over a part whose log region is 40 blocks, so a trace that fills it
# is cheap. Its reserve is the 80 blocks of earlier builds, so the table trace can retire sixty the log never uses
$(CAMPAIGN): nandlog_sim_campaign.c $(DEPS) $(ALLIANCE)
	$(CC) $(FAST) $(FRAMED) $(CAMPAIGN_PART) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@

# The exhaustive check runs sanitized over a toy part: four pages a block, eight ring blocks, five log blocks and
# the driver's reserve
$(EXHAUSTIVE): nandlog_sim_exhaustive.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(TOY) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@

# Operation counts over the full-size Alliance part, framed and not, with and without the part's internal copy
$(OPCOUNTS): nandlog_sim_opcounts.c $(DEPS) $(ALLIANCE)
	$(CC) $(FAST) $(FRAMED) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
$(OPCOUNTS)_no_chip_copy: nandlog_sim_opcounts.c $(DEPS) $(ALLIANCE)
	$(CC) $(FAST) $(FRAMED) -DNANDLOG_CHIP_PAGE_COPY=0 $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@

# The regression tests at the erase-ahead windows the configuration guidance names, and with the head passed over
nandlog_sim_regress_w0: nandlog_sim_regress.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(SMALL) -DNANDLOG_ERASE_AHEAD_BLOCKS=0 $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_regress_w1: nandlog_sim_regress.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(SMALL) -DNANDLOG_ERASE_AHEAD_BLOCKS=1 $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
# A window of 3, the smallest at which the table, given blocks mid-logging, can take two the window counts (K16). Only
# the K tests run there: Q3, X1 and Y5 aim a fault or a cut at an erase that a window of 3 has already done when the
# epoch began or the part booted, so their scenarios do not arise
nandlog_sim_regress_w3: nandlog_sim_regress.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(SMALL) -DNANDLOG_ERASE_AHEAD_BLOCKS=3 $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_exhaustive_headpass: nandlog_sim_exhaustive.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(TOY) -DNANDLOG_PASS_OVER_HEAD_AT_BOOT=1 $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_exhaustive_w0: nandlog_sim_exhaustive.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(TOY) -DNANDLOG_ERASE_AHEAD_BLOCKS=0 $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_exhaustive_dead: nandlog_sim_exhaustive.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(TOY_DEAD) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_exhaustive_degraded: nandlog_sim_exhaustive.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(TOY_DEGRADED) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
$(CAMPAIGN)_headpass: nandlog_sim_campaign.c $(DEPS) $(ALLIANCE)
	$(CC) $(FAST) $(FRAMED) $(CAMPAIGN_PART) -DNANDLOG_PASS_OVER_HEAD_AT_BOOT=1 $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
$(CAMPAIGN)_w0: nandlog_sim_campaign.c $(DEPS) $(ALLIANCE)
	$(CC) $(FAST) $(FRAMED) $(CAMPAIGN_PART) -DNANDLOG_ERASE_AHEAD_BLOCKS=0 $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
$(CAMPAIGN)_dead: nandlog_sim_campaign.c $(DEPS) $(ALLIANCE)
	$(CC) $(FAST) $(FRAMED) $(CAMPAIGN_PART) $(CAMPAIGN_DEAD) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@

# The fuzzer runs sanitized, over the small part and over parts whose 40-block log region fills and wraps sooner
nandlog_sim_fuzz: nandlog_sim_fuzz.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(SMALL) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_fuzz_small: nandlog_sim_fuzz.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(MEDIUM) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_fuzz_no_chip_copy: nandlog_sim_fuzz.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(NOCOPY) $(MEDIUM) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_fuzz_winbond: nandlog_sim_fuzz.c $(DEPS) $(WINBOND)
	$(CC) $(CFLAGS) $(WINBONDC) -DNANDLOG_CHIP_BLOCK_COUNT=128 $(INCLUDES) $(PORT) $< $(CORE) $(WINBOND) -o $@
nandlog_sim_fuzz_w0: nandlog_sim_fuzz.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(MEDIUM) -DNANDLOG_ERASE_AHEAD_BLOCKS=0 $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_fuzz_headpass: nandlog_sim_fuzz.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(MEDIUM) -DNANDLOG_PASS_OVER_HEAD_AT_BOOT=1 $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@
nandlog_sim_fuzz_port_crc: nandlog_sim_fuzz.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(PORTCRC) $(MEDIUM) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@

# The board with a dead reserve: the 64-block part with its whole reserve and the region's top two blocks dead, and the
# table's search reaching 32 blocks down, so the table lives in a pair of the region's top blocks, or in the marks
DEAD_FUZZ = -DSIM_DEAD_RESERVE_BLOCKS=16 -DSIM_DEAD_LOG_BLOCKS=2 -DNANDLOG_CHIP_TABLE_SEARCH_BLOCKS=32
nandlog_sim_fuzz_dead: nandlog_sim_fuzz.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(MEDIUM) $(DEAD_FUZZ) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@

# The same, with the search reaching only the dead blocks: no pair is ever found, and every retirement is by a mark
DEGRADED_FUZZ = -DSIM_DEAD_RESERVE_BLOCKS=16 -DSIM_DEAD_LOG_BLOCKS=2 -DNANDLOG_CHIP_TABLE_SEARCH_BLOCKS=18
nandlog_sim_fuzz_degraded: nandlog_sim_fuzz.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(MEDIUM) $(DEGRADED_FUZZ) $(INCLUDES) $(PORT) $< $(CORE) $(ALLIANCE) -o $@

# The driver's answers to nandlog_chip_is_bad_block() against a search of its table, over the full-size part and the
# campaign's small one. The driver's source is compiled into the test, which reads its table, rather than linked
nandlog_sim_badblocks: nandlog_sim_badblocks.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(INCLUDES) $(PORT) $< $(CORE) -o $@
nandlog_sim_badblocks_small: nandlog_sim_badblocks.c $(DEPS) $(ALLIANCE)
	$(CC) $(CFLAGS) $(FRAMED) $(CAMPAIGN_PART) $(INCLUDES) $(PORT) $< $(CORE) -o $@

run: $(SUITES) $(REGRESS) $(BADBLOCKS)
	@for t in $(SUITES) $(REGRESS); do echo; ./$$t || exit 1; done
	@echo; ./nandlog_sim_badblocks 9 && echo && ./nandlog_sim_badblocks_small 150
	@$(MAKE) --no-print-directory parse

# The reference parser, read against the library's own view of each image the suite dumped
parse:
	@echo
	@echo "reference parser against the dumped images"
	@$(PYTHON) -I ../tools/nandlog_parse.py image nandlog_image.bin --expect nandlog_image.bin.view
	@$(PYTHON) -I ../tools/nandlog_parse.py image nandlog_image_framed.bin --expect nandlog_image_framed.bin.view
	@$(PYTHON) -I ../tools/nandlog_parse.py image nandlog_image_no_chip_copy_framed.bin --expect nandlog_image_no_chip_copy_framed.bin.view
	@$(PYTHON) -I ../tools/nandlog_parse.py image nandlog_image_winbond_framed.bin --page-size 2048 --spare-size 64 --expect nandlog_image_winbond_framed.bin.view
	@$(PYTHON) -I ../tools/nandlog_parse.py image nandlog_image_port_crc_framed.bin --expect nandlog_image_port_crc_framed.bin.view

campaign: $(CAMPAIGN) $(CAMPAIGN)_w0 $(CAMPAIGN)_headpass $(CAMPAIGN)_dead
	@failed=0; \
	$(MAKE) --no-print-directory campaign_run CAMPAIGN=$(CAMPAIGN) CAMPAIGN_OUT=campaign.out || failed=1; echo; \
	$(MAKE) --no-print-directory campaign_run CAMPAIGN=$(CAMPAIGN)_w0 CAMPAIGN_OUT=campaign_w0.out || failed=1; echo; \
	$(MAKE) --no-print-directory campaign_run CAMPAIGN=$(CAMPAIGN)_headpass CAMPAIGN_OUT=campaign_headpass.out CAMPAIGN_JOBS=^campaign || failed=1; echo; \
	$(MAKE) --no-print-directory campaign_run CAMPAIGN=$(CAMPAIGN)_dead CAMPAIGN_OUT=campaign_dead.out || failed=1; \
	exit $$failed
campaign_default: $(CAMPAIGN)
	@$(MAKE) --no-print-directory campaign_run CAMPAIGN=$(CAMPAIGN) CAMPAIGN_OUT=campaign.out
campaign_w0: $(CAMPAIGN)_w0
	@$(MAKE) --no-print-directory campaign_run CAMPAIGN=$(CAMPAIGN)_w0 CAMPAIGN_OUT=campaign_w0.out
campaign_headpass: $(CAMPAIGN)_headpass
	@$(MAKE) --no-print-directory campaign_run CAMPAIGN=$(CAMPAIGN)_headpass CAMPAIGN_OUT=campaign_headpass.out CAMPAIGN_JOBS=^campaign
campaign_dead: $(CAMPAIGN)_dead
	@$(MAKE) --no-print-directory campaign_run CAMPAIGN=$(CAMPAIGN)_dead CAMPAIGN_OUT=campaign_dead.out

campaign_run:
	@echo "$(CAMPAIGN): crash-point campaign and recovery sweep: $$(./$(CAMPAIGN) list | grep -E '$(CAMPAIGN_JOBS)' | wc -l | tr -d ' ') jobs, $(JOBS) at a time"
	@rm -rf $(CAMPAIGN_OUT) && mkdir $(CAMPAIGN_OUT)
	@./$(CAMPAIGN) list | grep -E '$(CAMPAIGN_JOBS)' | tr ' ' '_' | xargs -P $(JOBS) -I{} sh -c 'job="{}"; ./$(CAMPAIGN) $$(echo $$job | tr "_" " ") > $(CAMPAIGN_OUT)/$$job.txt 2>&1; echo $$? > $(CAMPAIGN_OUT)/$$job.status'
	@echo
	@cat $(CAMPAIGN_OUT)/campaign_*.txt | grep '^#' | sed 's/^# //'
	@echo
	@echo "One cut at every program and erase of each trace, then a second at every one of the recovering boot's, and"
	@echo "at the first operations of the logging after it; every trial then lets its weak pages drift"
	@echo "Violations, summed over trials: L lost, D damaged, U duplicated, M misplaced, S sequence breaks, B byte-span"
	@echo "errors, R repeated programs (a drifted page's included), G good blocks retired, T factory-bad blocks"
	@echo "forgotten, I sequence numbers that served other content. E: trials in which a page reading erased in every"
	@echo "bit was programmed again after a reboot, outside a block being retired -- not detectable by any read"
	@printf "%-9s %-9s %5s %15s %15s   %s\n" trace model ops "one cut clean" "two cuts clean" "L D U M S B R G T I   E"
	@cat $(CAMPAIGN_OUT)/campaign_*.txt | grep '^RESULT' | awk '{ printf "%-9s %-9s %5s %7s / %-5s %7s / %-5s   %s %s %s %s %s %s %s %s %s %s   %s\n", $$2, $$3, $$4, $$6, $$5, $$8, $$7, $$9, $$10, $$11, $$12, $$13, $$14, $$15, $$16, $$17, $$18, $$19 }'
	@cat $(CAMPAIGN_OUT)/campaign_*.txt | grep '^RESULT' | awk '{ t1 += $$5; c1 += $$6; t2 += $$7; c2 += $$8 } END { printf "%-25s %7u / %-5u %7u / %-5u\n", "all", c1, t1, c2, t2 }'
	@echo
	@echo "Recovery sweep, one fault per reboot and no cut: reboots, then those that misplaced the head, lost a committed"
	@echo "page, delivered a damaged or duplicated record, or programmed a page twice"
	@cat $(CAMPAIGN_OUT)/sweep_*.txt 2>/dev/null | grep '^SWEEP' | awk 'BEGIN { split("rotted-header uncorrectable at-ECC-limit retired-block", f, " "); p[0] = "from the region start"; p[1] = "across the wrap" } { k = p[$$2] " | " f[$$3 + 1]; n[k] += $$5; h[k] += $$6; l[k] += $$7; d[k] += $$8; r[k] += $$9 } END { for (k in n) printf "  %-40s %6u reboots   %u %u %u %u\n", k, n[k], h[k], l[k], d[k], r[k] }' | sort
	@echo
	@echo "Metadata sweep, one page of the ring or of the bad-block table rotted, uncorrectable, or whole but read at the"
	@echo "ECC limit, per reboot: reboots, then those that changed the current epoch, the epochs listed or the blocks"
	@echo "retired, misplaced the head, lost a committed page, delivered a damaged or duplicated record, or programmed a"
	@echo "page twice; then those that wrote the newest slot or table version again, and those after which the next boot"
	@echo "wrote again or that -- a page at the ECC limit -- wrote again other than for a copy of the newest"
	@cat $(CAMPAIGN_OUT)/sweep_metadata*.txt 2>/dev/null | grep '^METADATA' | awk 'BEGIN { split("ring table", t, " "); split("rotted uncorrectable at-ECC-limit", f, " ") } { printf "  %-5s | %-24s %6u reboots   %u %u %u %u %u %u %u   %u %u\n", t[$$2 + 1], f[$$3 + 1], $$4, $$5, $$6, $$7, $$8, $$9, $$10, $$11, $$12, $$13 }'
	@echo
	@if grep -L '^0$$' $(CAMPAIGN_OUT)/*.status | grep -q .; then echo "CAMPAIGN FAILED ($(CAMPAIGN)): $$(grep -L '^0$$' $(CAMPAIGN_OUT)/*.status | xargs -n1 basename)"; exit 1; else echo "$(CAMPAIGN): every trial kept the contract"; fi

fuzz: $(FUZZ)
	@echo "fuzzing: $(words $(FUZZ)) configurations x $(words $(FUZZ_SEEDS)) seeds x $(FUZZ_STEPS) steps, $(JOBS) at a time"
	@rm -rf fuzz.out && mkdir fuzz.out
	@for b in $(FUZZ); do for s in $(FUZZ_SEEDS); do echo "$$b $$s"; done; done | xargs -P $(JOBS) -n 2 sh -c './$$0 $$1 $(FUZZ_STEPS) 30 600 $$(($$1 % 3)) > fuzz.out/$$0_$$1.txt 2>&1; echo $$? > fuzz.out/$$0_$$1.status'
	@echo
	@echo "Runs, steps, power cuts, audits, blocks retired, weak pages drifted and oracle violations, summed over the"
	@echo "seeds; then the reads whose byte total missed because a page a cut left unstable read differently on two passes"
	@grep -H '^FUZZ' fuzz.out/*.txt | sed 's|^fuzz.out/||; s|_[0-9]*\.txt:FUZZ||' | awk '{ r[$$1] += 1; st[$$1] += $$5; c[$$1] += $$9; a[$$1] += $$11; b[$$1] += $$13; v[$$1] += $$17; u[$$1] += $$19; d[$$1] += $$21 } END { for (k in r) printf "  %-30s %3u runs %7u steps %5u cuts %6u audits %4u retired %5u drifted %2u violations   %u\n", k, r[k], st[k], c[k], a[k], b[k], d[k], v[k], u[k] }' | sort
	@grep -h '^VIOLATION' fuzz.out/*.txt | head -20
	@echo
	@if grep -L '^0$$' fuzz.out/*.status | grep -q .; then echo "FUZZ FAILED: $$(grep -L '^0$$' fuzz.out/*.status | xargs -n1 basename)"; exit 1; else echo "fuzz: every audit held"; fi

exhaustive: $(EXHAUSTIVE) $(EXHAUSTIVE)_w0 $(EXHAUSTIVE)_headpass $(EXHAUSTIVE)_dead $(EXHAUSTIVE)_degraded
	@failed=0; \
	./$(EXHAUSTIVE) || failed=1; echo; \
	echo "--- with an erase-ahead window of 0 blocks"; ./$(EXHAUSTIVE)_w0 || failed=1; echo; \
	echo "--- with NANDLOG_PASS_OVER_HEAD_AT_BOOT, the metadata paths"; ./$(EXHAUSTIVE)_headpass metadata | sed -n '/^metadata write paths/,$$p' || failed=1; echo; \
	echo "--- a dead reserve, the table moved below it: the metadata paths"; ./$(EXHAUSTIVE)_dead metadata > exhaustive_dead.txt || failed=1; \
	sed -n '/^metadata write paths/,$$p' exhaustive_dead.txt; echo; \
	echo "--- a dead reserve and nowhere to move the table, the marks standing in: the metadata paths"; \
	./$(EXHAUSTIVE)_degraded metadata > exhaustive_degraded.txt || failed=1; sed -n '/^metadata write paths/,$$p' exhaustive_degraded.txt; \
	rm -f exhaustive_dead.txt exhaustive_degraded.txt; exit $$failed

windows: nandlog_sim_regress_w0 nandlog_sim_regress_w1 nandlog_sim_regress_w3
	@for w in 0 1; do out=$$(./nandlog_sim_regress_w$$w 2>&1); s=$$?; echo "$$out" | grep -B1 'FAIL'; \
	    echo "window of $$w: $$(echo "$$out" | tail -1)"; [ $$s -eq 0 ] || exit 1; done
	@out=$$(./nandlog_sim_regress_w3 test_k 2>&1); s=$$?; echo "$$out" | grep -B1 'FAIL'; \
	    echo "window of 3, the K tests: $$(echo "$$out" | tail -1)"; [ $$s -eq 0 ] || exit 1

headpass: $(EXHAUSTIVE)_headpass $(CAMPAIGN)_headpass
	@./$(EXHAUSTIVE)_headpass metadata | sed -n '/^metadata write paths/,$$p'
	@$(MAKE) --no-print-directory campaign_headpass

opcounts: $(OPCOUNTS) $(OPCOUNTS)_no_chip_copy
	@./$(OPCOUNTS)
	@echo
	@./$(OPCOUNTS)_no_chip_copy relocation
	@echo
	@./$(OPCOUNTS) boards

ablation:
	@sh ablation.sh

baselines:
	@sh baselines/reproduce.sh

# The soak's board functions come from nandlog_sim_soak.c and soak/; its main() is renamed so the harness can call it
# once a boot
SOAK_BOOTS = 1000
SOAK_FLAGS = $(CFLAGS) $(FRAMED) $(SMALL) -DNANDLOG_HW_TEST_HOST -Isoak -I../tests $(INCLUDES)
nandlog_sim_soak: nandlog_sim_soak.c ../tests/test_nandlog_powercut.c ../tests/nandlog_hw_test.h soak/logging.h soak/system.h $(DEPS) $(ALLIANCE)
	$(CC) $(SOAK_FLAGS) -Dmain=powercut_main -c ../tests/test_nandlog_powercut.c -o nandlog_sim_soak_powercut.o
	$(CC) $(SOAK_FLAGS) $(PORT) $< $(CORE) $(ALLIANCE) nandlog_sim_soak_powercut.o -o $@
	@rm -f nandlog_sim_soak_powercut.o

soak: nandlog_sim_soak
	@rm -rf soak.out && mkdir soak.out
	@echo "power-cut soak: $(SOAK_BOOTS) boots of tests/test_nandlog_powercut.c over the simulator, cut at random"
	@./nandlog_sim_soak $(SOAK_BOOTS) > soak.out/soak.log 2> soak.out/soak.err; grep '^harness' soak.out/soak.err
	@$(PYTHON) -I ../tests/powercut_check.py soak.out/soak.log
	@echo
	@echo "the same for 60 boots, with one committed bit cleared before boot 45, which the checker must report"
	@./nandlog_sim_soak 60 45 > soak.out/sabotage.log 2> soak.out/sabotage.err; grep '^harness: rotted' soak.out/sabotage.err
	@if $(PYTHON) -I ../tests/powercut_check.py soak.out/sabotage.log > soak.out/sabotage.txt; then \
	    echo "SOAK FAILED: the checker missed the damage"; exit 1; \
	 else grep '^RESULT' soak.out/sabotage.txt; echo "soak: the checker reported the damage it was shown"; fi

clean:
	rm -rf $(SUITES) $(REGRESS) $(BADBLOCKS) $(CAMPAIGN) $(CAMPAIGN)_w0 $(CAMPAIGN)_headpass $(CAMPAIGN)_dead $(FUZZ) $(EXHAUSTIVE) $(EXHAUSTIVE)_w0 $(EXHAUSTIVE)_headpass \
	       $(EXHAUSTIVE)_dead $(EXHAUSTIVE)_degraded \
	       $(OPCOUNTS) $(OPCOUNTS)_no_chip_copy nandlog_sim_regress_w0 nandlog_sim_regress_w1 nandlog_sim_regress_w3 *.dSYM campaign.out campaign_w0.out \
	       campaign_headpass.out campaign_dead.out fuzz.out ablation.out nandlog_sim_soak soak.out nandlog_image*.bin nandlog_image*.view
