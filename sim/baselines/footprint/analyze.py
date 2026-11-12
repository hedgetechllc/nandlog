#!/usr/bin/env python3
"""Attribute the linked footprint probes' bytes to components, and compute worst-case stack depth.

Usage: python3 -I analyze.py <out dir>        (run by build.sh)

Reads <out>/<image>.map (GNU ld map files of the --gc-sections links), <out>/obj/*.ci (GCC -fcallgraph-info=su) and
<out>/obj/app_*.o symbol sizes (via arm-none-eabi-nm), and prints plain-text tables. Harness code, not part of nandlog.
"""
import os
import re
import subprocess
import sys

OUT = sys.argv[1]
IMAGES = ["nandlog", "dhara", "lfs_dhara", "lfs_raw"]

COMPONENT = {
    "nandlog.o": "nandlog core",
    "nandlog_chip_AS5F18G04SND.o": "Alliance driver",
    "dhara_map.o": "Dhara", "dhara_journal.o": "Dhara", "dhara_error.o": "Dhara",
    "lfs.o": "littlefs", "lfs_util.o": "littlefs",
    "glue_dhara_nand.o": "glue: Dhara NAND", "glue_dla.o": "glue: DLA append layer",
    "glue_lfs_bd_dhara.o": "glue: lfs bd (Dhara)", "glue_lfs_bd_raw.o": "glue: lfs bd (raw)",
    "glue_lfa.o": "glue: lfs logger",
    "port_stub.o": "port stub",
}

# Symbols the probe applications declare that belong to the application rather than to the storage stack
APP_OWNED = {"record", "sink"}
OFFLOAD = {"page", "rfile_buf", "rf", "rcfg"}       # buffers only the sequential offload (read-back) needs


def classify(section):
    if section.startswith(".text") or section.startswith(".ARM.exidx") or section.startswith(".ARM.extab") \
            or section.startswith(".glue_7") or section.startswith(".vfp11") or section.startswith(".v4_bx") \
            or section.startswith(".iplt"):
        return "text"
    if section.startswith(".rodata"):
        return "rodata"
    if section.startswith(".data"):
        return "data"
    if section.startswith(".bss") or section == "COMMON":
        return "bss"
    return None


def parse_map(path):
    sizes = {}
    with open(path) as f:
        lines = f.read().split("\n")
    start = next(i for i, l in enumerate(lines) if l.startswith("Linker script and memory map"))
    pending, last = None, None
    residual = 0
    for line in lines[start:]:
        # Merged string sections: ld reports the whole merged size against the first input and 0 against the rest,
        # each followed by its own "(size before relaxing)". Attribute each object its own strings, and keep the
        # difference (strings shared between objects, alignment) as a separate line so totals still match the ELF
        r = re.match(r"^\s+0x([0-9a-f]+) \(size before relaxing\)\s*$", line)
        if r and last is not None:
            key, kind, recorded = last
            own = int(r.group(1), 16)
            sizes.setdefault(key, {"text": 0, "rodata": 0, "data": 0, "bss": 0})
            sizes[key][kind] += own - recorded
            residual += recorded - own
            last = None
            continue
        m = re.match(r"^ (\.\S+|COMMON)\s*$", line)
        if m:
            pending = m.group(1)
            continue
        m = re.match(r"^ (\.\S+|COMMON)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(.+)$", line)
        if m:
            section, size, obj = m.group(1), int(m.group(3), 16), m.group(4).strip()
        else:
            m = re.match(r"^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$", line)
            if not (m and pending):
                pending = None
                continue
            section, size, obj = pending, int(m.group(2), 16), m.group(3).strip()
        pending = None
        kind = classify(section)
        if kind is None:
            last = None
            continue
        base = os.path.basename(obj)
        if ".a(" in obj:
            comp = "libc/libgcc"
        else:
            comp = COMPONENT.get(base, "probe app" if base.startswith("app_") else base)
        key = (comp, base)
        sizes.setdefault(key, {"text": 0, "rodata": 0, "data": 0, "bss": 0})
        sizes[key][kind] += size
        last = (key, kind, size)
    if residual:
        sizes[("string merging", "-")] = {"text": 0, "rodata": residual, "data": 0, "bss": 0}
    return sizes


def app_symbols(image):
    obj = os.path.join(OUT, "obj", "app_%s.o" % image)
    out = subprocess.run(["arm-none-eabi-nm", "-S", "-t", "d", obj], capture_output=True, text=True, check=True).stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[2] in "bBdD":
            syms[parts[3]] = int(parts[1])
    return syms


# Stack depth ---------------------------------------------------------------------------------------------------------

LIBC_FRAME = 32       # assumed for newlib-nano leaf routines (memcpy, memset, memcmp, strlen) absent from the .ci files

BD_CALLBACKS = {
    "lfs_dhara": ["dhara_bd_read", "dhara_bd_prog", "dhara_bd_erase", "dhara_bd_sync"],
    "lfs_raw": ["raw_bd_read", "raw_bd_prog", "raw_bd_erase", "raw_bd_sync"],
}
# littlefs's function-pointer call sites (lfs.c v2.11.3 line numbers, as the .ci edge labels give them), resolved
# from the source: the block-device callbacks, and the callbacks littlefs passes to its own traversals
LFS_SITE = {95: "read", 117: "read", 182: "prog", 221: "sync", 278: "erase"}
LFS_CALLBACKS = {
    1059: ["lfs_dir_commit_commit", "lfs_dir_commit_size"], 1070: ["lfs_dir_commit_commit", "lfs_dir_commit_size"],
    1295: ["lfs_dir_find_match", "lfs_fs_parent_match"],
    3032: ["lfs_alloc_lookahead", "lfs_fs_size_count", "lfs_shrink_checkblock"],
    3053: ["lfs_alloc_lookahead", "lfs_fs_size_count", "lfs_shrink_checkblock"],
    4733: ["lfs_alloc_lookahead", "lfs_fs_size_count", "lfs_shrink_checkblock"],
    4766: ["lfs_alloc_lookahead", "lfs_fs_size_count", "lfs_shrink_checkblock"],
}

OBJECTS = {
    "nandlog": ["app_nandlog", "nandlog", "nandlog_chip_AS5F18G04SND", "port_stub"],
    "dhara": ["app_dhara", "glue_dla", "glue_dhara_nand", "dhara_map", "dhara_journal", "dhara_error",
              "nandlog_chip_AS5F18G04SND", "port_stub"],
    "lfs_dhara": ["app_lfs_dhara", "glue_lfa", "glue_lfs_bd_dhara", "lfs", "lfs_util", "glue_dhara_nand", "dhara_map",
                  "dhara_journal", "dhara_error", "nandlog_chip_AS5F18G04SND", "port_stub"],
    "lfs_raw": ["app_lfs_raw", "glue_lfa", "glue_lfs_bd_raw", "lfs", "lfs_util", "nandlog_chip_AS5F18G04SND", "port_stub"],
}


ENTRY_POINTS = {
    "nandlog": ["nandlog_init", "nandlog_store_record", "nandlog_flush", "nandlog_retrieve_next_page"],
    "dhara": ["dla_mount", "dla_append", "dla_flush", "dla_read_sector"],
    "lfs_dhara": ["lfa_mount", "lfa_append", "lfa_flush", "lfs_file_read"],
    "lfs_raw": ["lfa_mount", "lfa_append", "lfa_flush", "lfs_file_read"],
}


def short(title):
    return title.split(":")[-1]


def base_name(name):
    return re.sub(r"\.(constprop|part|isra|cold)\.\d+.*$", "", name)


def load_graph(image):
    frames, edges, dynamic = {}, {}, set()
    for o in OBJECTS[image]:
        with open(os.path.join(OUT, "obj", o + ".ci")) as f:
            text = f.read()
        for m in re.finditer(r'node: \{ title: "([^"]+)" label: "([^"]*)"', text):
            title, label = m.group(1), m.group(2)
            s = re.search(r"\\n(\d+) bytes \(([^)]*)\)", label)
            if s:
                frames[title] = int(s.group(1))
                if "dynamic" in s.group(2) and "bounded" not in s.group(2):
                    dynamic.add(title)
        for m in re.finditer(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"(?: label: "([^"]*)")?', text):
            target = m.group(2)
            if target == "__indirect_call":
                line = re.search(r":(\d+):\d+$", m.group(3) or "")
                target = "__indirect_call@%s" % (line.group(1) if line else "?")
            edges.setdefault(m.group(1), []).append(target)
    by_name = {}
    for t in frames:
        by_name.setdefault(short(t), []).append(t)
    return frames, edges, dynamic, by_name


def worst_stack(image, root="main"):
    frames, edges, dynamic, by_name = load_graph(image)
    memo, onpath, notes = {}, set(), set()

    def resolve(target, caller):
        if target.startswith("__indirect_call@"):
            line = int(target.split("@")[1]) if target.split("@")[1].isdigit() else -1
            names = LFS_CALLBACKS.get(line, [])
            if line in LFS_SITE:
                kind = LFS_SITE[line]
                names = [n for n in BD_CALLBACKS.get(image, []) if n.endswith("_" + kind)]
            out = []
            for n in names:
                out += [t for t in frames if base_name(short(t)) == n]
            if not out:
                notes.add("unresolved indirect call in %s" % short(caller))
            return out
        if target in frames:
            return [target]
        cands = by_name.get(short(target), [])
        return cands if cands else [None]

    def depth(title):
        if title is None:
            return LIBC_FRAME, ["(libc)"]
        if title in memo:
            return memo[title]
        if title in onpath:
            notes.add("recursion through %s" % short(title))
            return 0, ["(cycle)"]
        onpath.add(title)
        best, path = 0, []
        for tgt in edges.get(title, []):
            for r in resolve(tgt, title):
                d, p = depth(r)
                if d > best:
                    best, path = d, p
        onpath.discard(title)
        if title in dynamic:
            notes.add("dynamic frame in %s" % short(title))
        res = (frames.get(title, 0) + best, [short(title)] + path)
        memo[title] = res
        return res

    roots = by_name.get(root, [])
    return depth(roots[0]) + (sorted(notes),)


def main():
    print("Linked probes (--gc-sections), bytes attributed per component from the linker map")
    print("flash = .text + .rodata (+ .ARM.exidx); RAM = .data + .bss. 'probe app' holds the buffers the stack needs")
    print("the caller to supply, listed symbol by symbol below.\n")
    for image in IMAGES:
        sizes = parse_map(os.path.join(OUT, image + ".map"))
        comps = {}
        for (comp, base), s in sizes.items():
            c = comps.setdefault(comp, {"text": 0, "rodata": 0, "data": 0, "bss": 0})
            for k in s:
                c[k] += s[k]
        print("== %s" % image)
        print("  %-26s %8s %8s %8s %8s %8s" % ("component", ".text", ".rodata", ".data", ".bss", "flash"))
        tot = {"text": 0, "rodata": 0, "data": 0, "bss": 0}
        for comp in sorted(comps):
            c = comps[comp]
            if not any(c.values()):
                continue
            for k in tot:
                tot[k] += c[k]
            print("  %-26s %8d %8d %8d %8d %8d" % (comp, c["text"], c["rodata"], c["data"], c["bss"], c["text"] + c["rodata"] + c["data"]))
        print("  %-26s %8d %8d %8d %8d %8d" % ("TOTAL", tot["text"], tot["rodata"], tot["data"], tot["bss"], tot["text"] + tot["rodata"] + tot["data"]))
        syms = app_symbols(image)
        stack_bufs = {k: v for k, v in syms.items() if k not in APP_OWNED and k not in OFFLOAD}
        off = {k: v for k, v in syms.items() if k in OFFLOAD}
        own = {k: v for k, v in syms.items() if k in APP_OWNED}
        print("  caller-supplied buffers/state for logging: %s = %d" % (", ".join("%s %d" % kv for kv in sorted(stack_bufs.items())), sum(stack_bufs.values())))
        print("  offload-only buffers: %s = %d" % (", ".join("%s %d" % kv for kv in sorted(off.items())), sum(off.values())))
        print("  application-owned (same in every probe): %s" % ", ".join("%s %d" % kv for kv in sorted(own.items())))
        lib_static = sum(c["data"] + c["bss"] for comp, c in comps.items() if comp not in ("probe app", "port stub", "libc/libgcc", "string merging"))
        print("  RAM for logging = library static %d + caller-supplied %d = %d bytes (+ offload %d)" %
              (lib_static, sum(stack_bufs.values()), lib_static + sum(stack_bufs.values()), sum(off.values())))
        d, path, notes = worst_stack(image)
        print("  worst-case stack from main(): %d bytes via %s" % (d, " > ".join(path)))
        if notes:
            print("  stack notes: %s" % "; ".join(notes))
        for entry in ENTRY_POINTS[image]:
            d, path, notes = worst_stack(image, entry)
            print("  worst-case stack of %s(): %d bytes via %s" % (entry, d, " > ".join(path)))
        print()


if __name__ == "__main__":
    main()
