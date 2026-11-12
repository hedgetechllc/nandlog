#!/usr/bin/env python3
"""The modeled time and energy of a durable flush cycle: the white paper's italic row of the comparison table and the
energy figures after it, from results/*.txt (one RESULT line per bench run) and board C's measured primitives.

Usage: python3 -I model_time.py <results dir>   > model_time.md
Harness code, not part of nandlog.

WHAT IS MODELED. A durable flush cycle is one flush and the appends since the one before, so each stream's totals are
divided by its flush calls. Every operation is costed as the white paper's cost model costs a call, from board C's
primitives: a page latch as its command and wait, plus its transfer, a fixed cost and a cost per byte clocked out; a
program with its 4 KiB page load, through the driver; each of Dhara's garbage-collection copies as the part's internal
copy, which is one latch and one program in the counts and is costed once, as a copy; and an erase as anything from a
block already erased to a full one, both through the driver. The bytes clocked out are the stream's, which include
one byte for every status-register read the simulated part answered; status polls are already inside the measured
latch, program and erase, so this overstates a cycle by well under 0.1%. No processor time and no overlap: a lower
bound, the lowest with every erase of a block already erased. Energy is the part's share only, at the datasheet's
maximum active current.
"""
import glob
import os
import sys

RES = sys.argv[1]

# Board C, bench run 8 (tests/test_nandlog_bench.c), the medians of these BENCH lines; the white paper's macro in brackets
HEADER_READ_US = 275.237            # prim.read.header_32B.programmed: a 32-byte header read through the driver (hwHeaderRead)
XFER_32_US = 35.197                 # prim.xfer_out_32B: clocking 32 bytes out of the cache register (hwXferHeader)
XFER_4096_US = 728.631              # prim.xfer_out_4096B: clocking 4,096 bytes out (hwXferPage)
PROGRAM_US = 1553.271               # prim.program.driver: a page program through the driver, its 4 KiB load included (hwProgDriver)
COPY_US = 1225.273                  # prim.copy.internal: the part's internal copy of a page, latch and program (hwCopyInternal)
ERASE_EMPTY_US = 1016.952           # store.32B.erase_ahead: a store that carried the erase of a block already erased (1.02 ms)
ERASE_FULL_US = 3525.114            # prim.erase.driver.full_block: the erase of a fully programmed block (hwEraseFullDriver)

# Derived as the paper derives them: the cost of each byte clocked out (hwReadPerByte), a transfer's fixed cost
# (hwXferFixed), and a latch's command and wait, the header read less its transfer (hwHeaderLatch)
PER_BYTE_US = (XFER_4096_US - XFER_32_US) / (4096 - 32)
XFER_FIXED_US = XFER_32_US - (32 * PER_BYTE_US)
LATCH_US = HEADER_READ_US - XFER_32_US

# AS5F18G04SND datasheet: 30 mA maximum active current, at the 1.8 V supply of the part (hwCommitEnergy's figures)
CURRENT_MA = 30.0
SUPPLY_V = 1.8
UJ_PER_MS = CURRENT_MA * SUPPLY_V   # mA x V = mW = uJ per ms

STACK_ORDER = ["nandlog", "dhara", "lfs-dhara", "lfs-raw"]
STACK_LABEL = {"nandlog": "nandlog", "dhara": "Dhara + DLA", "lfs-dhara": "littlefs over Dhara", "lfs-raw": "littlefs alone"}
CASES = [(1024, 1, "1 KiB records, a durable flush after every record (the table's row)"),
         (32, 16, "32-byte records, a durable flush every 16 (the text's second set)")]


def load():
    runs = {}
    for path in sorted(glob.glob(os.path.join(RES, "*.txt"))):
        name = os.path.basename(path)[:-4]
        if name.startswith(("steady", "sens", "wrapwin")):
            continue
        with open(path) as f:
            line = next((l for l in f.read().splitlines() if l.startswith("RESULT ")), None)
        if line is None:
            continue
        kv = dict(tok.split("=", 1) for tok in line.split()[1:])
        runs[(kv["stack"], int(kv["record_bytes"]), int(kv["flush_every"]))] = kv
    return runs


def i(r, k):
    return int(float(r.get(k, 0)))


def model(r):
    # Per cycle: the counts, and the modeled time with every erase of an empty block and of a full one, in ms
    cycles = i(r, "flush_calls")
    copies = i(r, "dhara_stream_copies")
    reads = i(r, "stream_reads") - copies
    programs = i(r, "stream_programs") - copies
    erases = i(r, "stream_erases")
    rbytes = i(r, "stream_rbytes")
    rest_us = (reads * (LATCH_US + XFER_FIXED_US)) + (rbytes * PER_BYTE_US) + (programs * PROGRAM_US) + (copies * COPY_US)
    low = (rest_us + (erases * ERASE_EMPTY_US)) / cycles / 1000.0
    high = (rest_us + (erases * ERASE_FULL_US)) / cycles / 1000.0
    per = lambda n: n / float(cycles)
    return {"cycles": cycles, "reads": per(reads), "copies": per(copies), "programs": per(programs), "erases": per(erases),
            "rbytes": per(rbytes), "low": low, "high": high}


def ms(x):
    # As the paper prints a time: one decimal under 10 ms, whole milliseconds above
    return "%.1f" % x if x < 10 else "%.0f" % x


def span(lo, hi, f):
    a, b = f(lo), f(hi)
    return a if a == b else "%s-%s" % (a, b)


def main():
    runs = load()
    print("### Modeled time and energy of a durable flush cycle (board C's primitives; lower bounds)\n")
    print("Constants: latch %.1f us + %.1f us a transfer + %.4f us a byte clocked out; program %.1f us; internal copy %.1f us;"
          % (LATCH_US, XFER_FIXED_US, PER_BYTE_US, PROGRAM_US, COPY_US))
    print("erase %.1f us (block already erased) to %.1f us (full block); %.0f uJ per ms (%.0f mA at %.1f V).\n"
          % (ERASE_EMPTY_US, ERASE_FULL_US, UJ_PER_MS, CURRENT_MA, SUPPLY_V))
    for record_bytes, flush_every, title in CASES:
        print("#### %s\n" % title)
        print("| stack | cycles | latches | of them copies | programs (not copies) | erases | bytes read | ms, empty erase | ms, full erase | mJ, empty | mJ, full |")
        print("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
        row = []
        for stack in STACK_ORDER:
            r = runs.get((stack, record_bytes, flush_every))
            if r is None:
                print("| %s | (no results) |||||||||" % STACK_LABEL[stack])
                row.append(None)
                continue
            m = model(r)
            row.append(m)
            print("| %s | {:,} | %.2f | %.2f | %.2f | %.4f | {:,.0f} | %.3f | %.3f | %.3f | %.3f |".format(m["cycles"], m["rbytes"]) % (
                STACK_LABEL[stack], m["reads"] + m["copies"], m["copies"], m["programs"], m["erases"],
                m["low"], m["high"], m["low"] * UJ_PER_MS / 1000.0, m["high"] * UJ_PER_MS / 1000.0))
        if all(row):
            print("\nAs printed: %s ms with every erase of a block already erased (the lower bound); %s ms with full blocks; "
                  "%s ms as ranges." % (" / ".join(ms(m["low"]) for m in row), " / ".join(ms(m["high"]) for m in row),
                                        " / ".join(span(m["low"], m["high"], ms) for m in row)))
            mj = lambda x: x * UJ_PER_MS / 1000.0
            least, most = min(mj(m["low"]) for m in row[1:]), max(mj(m["high"]) for m in row[1:])
            print("Energy, the part's share at %.0f uJ per modeled ms: nandlog %.3f-%.3f mJ, the others %.3f-%.3f mJ; as printed, "
                  "about %s mJ against %.1f to %.1f mJ.\n" % (UJ_PER_MS, mj(row[0]["low"]), mj(row[0]["high"]), least, most,
                                                          span(mj(row[0]["low"]), mj(row[0]["high"]), lambda x: "%.2f" % x),
                                                          least, most))


if __name__ == "__main__":
    main()
