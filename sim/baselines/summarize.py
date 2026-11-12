#!/usr/bin/env python3
"""Turn results/*.txt (one RESULT line per bench run) into the comparison's markdown tables.

Usage: python3 -I summarize.py <results dir>   > summary.md
Harness code, not part of nandlog.
"""
import glob
import os
import sys

RES = sys.argv[1]
STACK_ORDER = ["nandlog", "dhara", "lfs-dhara", "lfs-raw"]
STACK_LABEL = {"nandlog": "nandlog", "dhara": "Dhara + DLA", "lfs-dhara": "littlefs / Dhara", "lfs-raw": "littlefs alone"}


def load():
    runs = {}
    for path in sorted(glob.glob(os.path.join(RES, "*.txt"))):
        name = os.path.basename(path)[:-4]
        with open(path) as f:
            text = f.read()
        line = next((l for l in text.splitlines() if l.startswith("RESULT ")), None)
        if line is None:
            print("<!-- %s: no RESULT line -->" % name)
            continue
        kv = {}
        for tok in line.split()[1:]:
            k, v = tok.split("=", 1)
            kv[k] = v
        for tok in text.split():
            if tok.startswith("elapsed_s="):
                kv["elapsed_s"] = tok.split("=")[1]
        runs[name] = kv
    return runs


def i(r, k):
    return int(float(r.get(k, 0)))


def cadence(r):
    f = i(r, "flush_every")
    if f == 0:
        return "none"
    per_page = {32: 104, 1024: 3}.get(i(r, "record_bytes"))
    return "every %d%s" % (f, " (= a page)" if f == per_page else "")


def durability(r):
    s, f = r["stack"], i(r, "flush_every")
    if s == "nandlog":
        return "page commit" if f == 0 else "each flush (+ page commit)"
    if s == "dhara":
        return "Dhara checkpoint (15 sectors)" if f == 0 else "each flush (+ checkpoint)"
    return "never before final sync" if f == 0 else "each lfs_file_sync"


def per_k(r, key):
    n = i(r, "records")
    return 1000.0 * i(r, key) / n if n else 0.0


def fmt(x):
    if x >= 100:
        return "%.0f" % x
    if x >= 10:
        return "%.1f" % x
    return "%.2f" % x


def main():
    runs = load()
    main_runs = [r for n, r in runs.items() if not n.startswith(("steady", "sens", "wrapwin"))]
    key = lambda r: (i(r, "record_bytes"), i(r, "flush_every") == 0 and -1 or (1000 // max(1, i(r, "flush_every"))),
                     STACK_ORDER.index(r["stack"]))
    main_runs.sort(key=lambda r: (i(r, "record_bytes"), [0, 104, 16, 3, 1].index(i(r, "flush_every")) if i(r, "flush_every") in (0, 104, 16, 3, 1) else 9, STACK_ORDER.index(r["stack"])))

    print("### Flash operations over the stream (100,000 records, final flush included), per 1,000 records\n")
    print("| record | flush cadence | stack | durable at | programs | page reads | erases | write amp. | SPI bytes/record | records verified |")
    print("|---|---|---|---|---:|---:|---:|---:|---:|---:|")
    for r in main_runs:
        wa = i(r, "stream_programs") * 4096.0 / i(r, "user_bytes")
        bus = (i(r, "stream_rbytes") + i(r, "stream_wbytes")) / float(i(r, "records"))
        print("| %d B | %s | %s | %s | %s | %s | %s | %.2f | %.0f | %s |" % (
            i(r, "record_bytes"), cadence(r), STACK_LABEL[r["stack"]], durability(r),
            fmt(per_k(r, "stream_programs")), fmt(per_k(r, "stream_reads")), fmt(per_k(r, "stream_erases")), wa, bus,
            "{:,}".format(i(r, "verified"))))

    print("\n### Worst case inside one call (each column is the call with the most of that operation)\n")
    print("| record | flush cadence | stack | append calls that program | append: max programs / reads / erases | worst append call (indicative) | flush: max programs / reads / erases | worst flush call (indicative) |")
    print("|---|---|---|---:|---|---:|---|---:|")
    for r in main_runs:
        prog_calls = 100.0 * i(r, "append_calls_programming") / max(1, i(r, "append_calls"))
        fl = "%d / %d / %d" % (i(r, "flush_maxprog_programs"), i(r, "flush_maxread_reads"), i(r, "flush_maxerase_erases")) if i(r, "flush_calls") else "--"
        flt = "%.1f ms" % (i(r, "flush_maxtime_est_us") / 1000.0) if i(r, "flush_calls") else "--"
        print("| %d B | %s | %s | %.2f %% | %d / %d / %d | %.1f ms | %s | %s |" % (
            i(r, "record_bytes"), cadence(r), STACK_LABEL[r["stack"]], prog_calls,
            i(r, "append_maxprog_programs"), i(r, "append_maxread_reads"), i(r, "append_maxerase_erases"),
            i(r, "append_maxtime_est_us") / 1000.0, fl, flt))

    print("\n### Boot (mount) after the stream, the first record after it, and reading everything back\n")
    print("| record | flush cadence | stack | boot page reads (driver's share) | boot bytes read | boot erases / programs | first append + flush: reads / programs / erases | read-back page reads per record |")
    print("|---|---|---|---:|---:|---:|---|---:|")
    for r in main_runs:
        print("| %d B | %s | %s | %d (%d) | %s | %d / %d | %d / %d / %d | %.3f |" % (
            i(r, "record_bytes"), cadence(r), STACK_LABEL[r["stack"]], i(r, "boot_reads"), i(r, "driverboot_reads"),
            "{:,}".format(i(r, "boot_rbytes")), i(r, "boot_erases"), i(r, "boot_programs"),
            i(r, "firstappend_reads") + i(r, "firstflush_reads"), i(r, "firstappend_programs") + i(r, "firstflush_programs"),
            i(r, "firstappend_erases") + i(r, "firstflush_erases"), i(r, "readback_reads") / float(max(1, i(r, "verified")))))

    other = [(n, r) for n, r in runs.items() if n.startswith(("steady", "sens", "wrapwin"))]
    if other:
        print("\n### Steady state and sensitivity runs, per 1,000 records\n")
        print("| run | stack | record | records counted (after warm-up) | flush | options | programs | page reads | erases | append: max programs / reads / erases | flush: max programs / reads / erases | boot page reads | boot erases |")
        print("|---|---|---|---|---|---|---:|---:|---:|---|---|---:|---:|")
        for n, r in sorted(other):
            opts = []
            if r["stack"] in ("dhara", "lfs-dhara"):
                opts.append("gc_ratio=%s" % r["gc_ratio"])
            if r["stack"].startswith("lfs"):
                opts.append("lookahead=%s B" % r["lookahead"])
            if i(r, "nl_epochs_begun_warmup") or i(r, "nl_epochs_begun_counted"):
                opts.append("epochs begun on FULL: %d in warm-up, %d counted" % (i(r, "nl_epochs_begun_warmup"), i(r, "nl_epochs_begun_counted")))
            fl = "%d / %d / %d" % (i(r, "flush_maxprog_programs"), i(r, "flush_maxread_reads"), i(r, "flush_maxerase_erases")) if i(r, "flush_calls") else "--"
            print("| %s | %s | %d B | %s (+%s) | %s | %s | %s | %s | %s | %d / %d / %d | %s | %d | %d |" % (
                n, STACK_LABEL[r["stack"]], i(r, "record_bytes"), "{:,}".format(i(r, "records")), "{:,}".format(i(r, "warmup")),
                cadence(r), "; ".join(opts) or "--",
                fmt(per_k(r, "stream_programs")), fmt(per_k(r, "stream_reads")), fmt(per_k(r, "stream_erases")),
                i(r, "append_maxprog_programs"), i(r, "append_maxread_reads"), i(r, "append_maxerase_erases"), fl,
                i(r, "boot_reads"), i(r, "boot_erases")))

    print("\n### Run details\n")
    print("| run | elapsed (s) | nandlog pages committed | Dhara sectors mapped | dhara_map_capacity() at the end (it moves) | the fixed capacity DLA and littlefs lay out against (80 bad blocks assumed) | littlefs blocks in use |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for n in sorted(runs):
        r = runs[n]
        print("| %s | %s | %s | %s | %s | %s | %s |" % (n, r.get("elapsed_s", "?"), r.get("nl_pages_committed", "0"),
                                                   r.get("dhara_sectors", "0"), r.get("dhara_capacity", "--"),
                                                   r.get("dhara_fixed_capacity", "--"), r.get("lfs_blocks_in_use", "0")))


if __name__ == "__main__":
    main()
