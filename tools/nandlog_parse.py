#!/usr/bin/env python3
"""Reference parser for the nandlog on-flash and offload formats.

This is the specification's second implementation. The device computes CRC-32 with the IEEE 802.3 polynomial
precisely so that this file can verify it with ``zlib.crc32`` and nothing bespoke -- if the two ever disagree,
one of them is wrong, and having a reader written independently of the writer is what makes that visible.

Two inputs are understood:

  * a **raw image**, a byte-for-byte dump of the part, parsed by walking every page; and
  * an **offload stream**, the framing a device sends over the wire, parsed sequentially.

Records inside a page are deliberately *not* interpreted. The log stores opaque bytes, and the length of a
record is a property of the application that wrote it, so a payload is handed back whole.

    ./nandlog_parse.py image  dump.bin --page-size 4096 --spare-size 256
    ./nandlog_parse.py stream offload.bin
    ./nandlog_parse.py image  dump.bin --json
    ./nandlog_parse.py image  dump.bin --expect library_view.txt

A raw image can hold more than one copy of a page. When a block fails, the log copies the pages it already
holds into the next good block and retires the failing one, and the originals stay where they were: still
valid, no longer live. Which blocks are retired is the chip driver's business, not the format's. When the dump
includes the Alliance driver's bad-block table it is read and the retired blocks are skipped; otherwise one
copy of each (epoch, sequence number) is kept, preferring one whose payload verifies. A page relocated after
it had become unreadable arrives as a TOMBSTONE: a valid header with its sequence number and no payload.

A metadata slot ('TTM2') is the 32-byte header, a 32-byte extension (the slot's sequence number in the order the ring
was written; a failed write-head page or 0xFFFFFFFF; how many blocks were retired when it was written, and a
fingerprint of which of the epoch's first blocks were; a zero byte; a seal, 0 once the epoch takes no more pages; how
many runs of passed-over positions follow the details, and their CRC; and a CRC over all 60 bytes before it), the
details, then the PASSED-OVER positions as runs -- one word each, the first position in bits 0-23 and the length less
one in bits 24-31 -- and every slot twice, to an even page of a ring block and the odd page after it. The newest slot
is the one with the highest sequence number. The bad-block table's versions are written in pairs the same way, under
the magic 'BBT3', in the first blocks of the reserve or -- on a part whose reserve could not hold them -- in a pair of
blocks below it, which every version written there names as retired. A block carrying a bad-block mark is passed over
whatever versions it holds -- but for a mark of one programmed bit, a stray one, on a block holding a version newer than
the newest elsewhere, which that version does not name; with no version left anywhere the marks themselves name the
retired blocks. The digits in these magics are part of the tags, not versions.
"""

import argparse
import json
import struct
import sys
import zlib

PAGE_MAGIC = 0x31505454        # 'TTP1', little-endian: records are opaque
PAGE_MAGIC_FRAMED = 0x32505454 # 'TTP2': each record carries its own length
FRAMING_LENGTH = struct.Struct("<H")   # the per-record data length that framing prefixes
META_MAGIC = 0x324D5454        # 'TTM2': a metadata slot
STREAM_MAGIC = 0x31535454      # 'TTS1'
RECORD_FORMAT_OPAQUE = 1       # a stream header's record_format: records opaque
RECORD_FORMAT_FRAMED = 2       # ... or each carrying its own length
NO_TIMESTAMP = 0xFFFFFFFF

PAGE_HEADER = struct.Struct("<IIIIIHHII")     # magic epoch seq first last length count payload_crc header_crc
META_HEADER = struct.Struct("<IIIIHHIII")     # magic epoch start created length version details_crc header_crc reserved
META_EXTENSION = struct.Struct("<IIIIHBBHHII")  # sequence head_failed_page retired_blocks span_retired span_blocks
                                                # zero sealed passed_over_count reserved passed_over_crc extension_crc
STREAM_HEADER = struct.Struct("<IHHII")       # magic record_format details_length total_pages total_payload_bytes
WIRE_PAGE = struct.Struct("<IIIHHI")          # seq first last length count payload_crc

PAGE_HEADER_CRC_BYTES = PAGE_HEADER.size - 4          # the header CRC covers everything before itself
META_HEADER_CRC_BYTES = META_HEADER.size - 8          # ... and the metadata header excludes its trailing reserved word

# The Alliance driver's bad-block table: magic, a CRC over what follows it, version, count, then the first page of
# each retired block
TABLE_MAGIC = b"BBT3"
TABLE_HEADER = struct.Struct("<4sIII")         # magic crc version count


def _crc(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def _timestamp(value):
    return None if value == NO_TIMESTAMP else value


def walk_records(payload):
    """Split a framed payload into records.

    Only pages carrying PAGE_MAGIC_FRAMED can be walked: without the length prefix a record's extent is a
    property of the application that wrote it, and guessing would invent records. Returns the records parsed
    and however many bytes were left over, which should be zero.
    """
    records, offset = [], 0
    while offset + FRAMING_LENGTH.size <= len(payload):
        length = FRAMING_LENGTH.unpack_from(payload, offset)[0]
        offset += FRAMING_LENGTH.size
        if offset + 5 + length > len(payload):
            break            # a truncated tail, not a record
        record_type = payload[offset]
        timestamp = struct.unpack_from("<I", payload, offset + 1)[0]
        offset += 5
        records.append({"type": record_type, "timestamp": timestamp,
                        "data": bytes(payload[offset:offset + length])})
        offset += length
    return records, len(payload) - offset


class Page:
    """One log page, and the verdict on whether it can be trusted."""

    __slots__ = ("index", "epoch", "seq", "first_timestamp", "last_timestamp",
                 "record_count", "payload", "header_ok", "payload_ok", "framed", "stale")

    def __init__(self, index, raw):
        (magic, self.epoch, self.seq, first, last,
         length, self.record_count, payload_crc, header_crc) = PAGE_HEADER.unpack_from(raw)
        self.index = index
        self.first_timestamp = _timestamp(first)
        self.last_timestamp = _timestamp(last)
        self.payload = b""
        self.payload_ok = False
        self.stale = False

        # A page is trustworthy only if it carries the magic and its header checksums correctly. Nothing else
        # in the header -- including the payload length -- means anything until that holds
        # The magic says which format the page is in, so a log written across a firmware change parses
        # correctly page by page rather than needing to be told
        self.framed = (magic == PAGE_MAGIC_FRAMED)
        self.header_ok = (magic in (PAGE_MAGIC, PAGE_MAGIC_FRAMED) and
                          header_crc == _crc(raw[:PAGE_HEADER_CRC_BYTES]) and
                          length <= len(raw) - PAGE_HEADER.size)
        if self.header_ok:
            self.payload = bytes(raw[PAGE_HEADER.size:PAGE_HEADER.size + length])
            self.payload_ok = (payload_crc == _crc(self.payload))

    @property
    def ok(self):
        return self.header_ok and self.payload_ok

    @property
    def tombstone(self):
        """A valid header with no payload: the log's record that this sequence number's page was lost when its
        block was relocated. Never written for any other reason, since a page is only committed with data in it"""
        return self.header_ok and not self.payload

    @property
    def delivers(self):
        return self.ok and not self.tombstone

    def records(self):
        """Records in this page, or None if it is not framed and therefore not walkable."""
        if not (self.framed and self.payload_ok):
            return None
        records, leftover = walk_records(self.payload)
        return records if not leftover else None

    def as_dict(self):
        out = {"index": self.index, "epoch": self.epoch, "seq": self.seq,
               "first_timestamp": self.first_timestamp, "last_timestamp": self.last_timestamp,
               "record_count": self.record_count, "payload_length": len(self.payload),
               "framed": self.framed, "header_ok": self.header_ok, "payload_ok": self.payload_ok,
               "tombstone": self.tombstone, "stale": self.stale}
        records = self.records()
        if records is not None:
            out["records_parsed"] = len(records)
        return out


class Metadata:
    """One page of the metadata ring holding a slot, describing an epoch. Every slot is on two pages."""

    __slots__ = ("index", "epoch", "log_start_page", "created_timestamp", "format_version", "details",
                 "header_ok", "details_ok", "sequence", "head_failed_page", "retired_blocks",
                 "passed_over", "list_ok", "sealed")

    def __init__(self, index, raw):
        (magic, self.epoch, self.log_start_page, self.created_timestamp,
         length, self.format_version, details_crc, header_crc, _reserved) = META_HEADER.unpack_from(raw)
        self.index = index
        self.sequence, self.head_failed_page, self.retired_blocks, self.sealed = 0, None, 0, False
        self.passed_over = []
        self.details = b""
        self.details_ok = False
        self.list_ok = True
        start = META_HEADER.size + META_EXTENSION.size
        self.header_ok = (magic == META_MAGIC and
                          header_crc == _crc(raw[:META_HEADER_CRC_BYTES]) and
                          length <= len(raw) - start)
        runs, list_crc = 0, _crc(b"")
        if self.header_ok:
            (self.sequence, failed, self.retired_blocks, _, _, _, sealed, runs, _, list_crc,
             extension_crc) = META_EXTENSION.unpack_from(raw, META_HEADER.size)
            self.sealed = (sealed != 0xFF)
            self.header_ok = (extension_crc == _crc(raw[:start - 4])) and start + length + 4 * runs <= len(raw)
            self.head_failed_page = None if failed == 0xFFFFFFFF else failed
        if self.header_ok:
            self.details = bytes(raw[start:start + length])
            self.details_ok = (details_crc == _crc(self.details))
            listed = bytes(raw[start + length:start + length + 4 * runs])
            self.list_ok = (list_crc == _crc(listed))
            for (word,) in struct.iter_unpack("<I", listed) if self.list_ok else ():
                self.passed_over.extend(range(word & 0xFFFFFF, (word & 0xFFFFFF) + (word >> 24) + 1))

    @property
    def ok(self):
        return self.header_ok and self.details_ok and self.list_ok

    def as_dict(self):
        return {"index": self.index, "epoch": self.epoch, "log_start_page": self.log_start_page,
                "created_timestamp": self.created_timestamp, "format_version": self.format_version,
                "details_length": len(self.details), "header_ok": self.header_ok, "details_ok": self.details_ok,
                "sequence": self.sequence, "head_failed_page": self.head_failed_page,
                "retired_blocks": self.retired_blocks, "sealed": self.sealed,
                "passed_over": self.passed_over, "list_ok": self.list_ok}


def _mark(data, block, page_size, stride, pages_per_block):
    """The byte at the start of a block's first page's spare area: a bad-block mark if anything but FFh."""
    at = block * pages_per_block * stride + page_size
    return data[at] if at < len(data) else 0xFF


def _marked(data, block, page_size, stride, pages_per_block):
    """Whether a block carries a bad-block mark: any byte but FFh at the start of its first page's spare area."""
    return _mark(data, block, page_size, stride, pages_per_block) != 0xFF


def _retired_blocks(data, page_size, stride, pages_per_block):
    """The retired blocks the Alliance driver's newest table version names, wherever it is kept -- the reserve, or a pair
    of blocks below it -- passing over versions in marked blocks, as the driver does, but for a block whose mark is one
    stray programmed bit, holding a version newer than the newest elsewhere, which that version does not name. With no
    version but marks in the dump, the marked blocks, as the driver's degraded mode has it. None if there is neither."""
    newest, stray, marks = None, None, set()
    for index in range(len(data) // stride):
        block = index // pages_per_block
        mark = _mark(data, block, page_size, stride, pages_per_block)
        if index % pages_per_block == 0 and mark != 0xFF:
            marks.add(block)
        raw = data[index * stride: index * stride + page_size]
        programmed = ~mark & 0xFF
        if raw[:4] == TABLE_MAGIC and len(raw) >= TABLE_HEADER.size and not programmed & (programmed - 1):
            _, crc, version, count = TABLE_HEADER.unpack_from(raw)
            end = TABLE_HEADER.size + 4 * count
            best = stray if programmed else newest
            if count <= 256 and end <= len(raw) and _crc(raw[8:end]) == crc and (best is None or version > best[0]):
                best = (version, struct.unpack_from("<%dI" % count, raw, TABLE_HEADER.size), block)
                stray, newest = (best, newest) if programmed else (stray, best)
    if newest is not None and stray is not None and stray[0] > newest[0] and \
            stray[2] not in {entry // pages_per_block for entry in newest[1]}:
        newest = stray
    if newest is None:
        return marks or None
    return {entry // pages_per_block for entry in newest[1]}


def parse_image(data, page_size, spare_size, pages_per_block=64):
    """Walk a raw dump. Pages are found by their magic rather than by knowing the layout, so a dump taken
    without knowing where the log region starts still parses. Copies left behind by a relocation are marked
    stale (see the module notes)."""
    stride = page_size + spare_size
    pages, metadata = [], []
    for index in range(len(data) // stride):
        raw = data[index * stride: index * stride + page_size]
        if len(raw) < PAGE_HEADER.size:
            break
        magic = struct.unpack_from("<I", raw)[0]
        if magic in (PAGE_MAGIC, PAGE_MAGIC_FRAMED):
            pages.append(Page(index, raw))
        elif magic == META_MAGIC:
            metadata.append(Metadata(index, raw))

    retired = _retired_blocks(data, page_size, stride, pages_per_block)
    if retired is not None:
        for page in pages:
            page.stale = (page.index // pages_per_block) in retired

    # Without the table, or as well as it: one live copy per (epoch, seq), preferring a page that verifies, then a
    # tombstone, then a page whose header alone can be trusted. A relocated copy and its original are identical
    # when both read, so which of two good ones is kept does not matter
    rank = lambda p: (2 if p.delivers else 1 if p.tombstone else 0)
    best = {}
    for page in pages:
        if page.stale or not page.header_ok:
            continue
        key = (page.epoch, page.seq)
        if key not in best or rank(page) > rank(best[key]):
            best[key] = page
    for page in pages:
        if page.header_ok and not page.stale and best.get((page.epoch, page.seq)) is not page:
            page.stale = True
    return pages, metadata


def compare_with_library(pages, expect_path):
    """Check this reader's verdict on every page against the library's own, as written by the simulator suite:
    one line per position, 'epoch seq payload_length record_count', zero length for a gap. Returns the number of
    disagreements, each printed."""
    live = {(p.epoch, p.seq): p for p in pages if p.header_ok and not p.stale}
    mismatches = 0
    for line in open(expect_path):
        fields = line.split()
        if len(fields) != 4:
            continue
        epoch, seq, length, count = (int(f) for f in fields)
        page = live.get((epoch, seq))
        mine = (len(page.payload), page.record_count) if (page and page.delivers) else (0, 0)
        if mine != (length, count):
            mismatches += 1
            print(f"  MISMATCH epoch {epoch} seq {seq}: library delivers {length} bytes / {count} records, "
                  f"parser {mine[0]} / {mine[1]}")
    return mismatches


def parse_stream(data):
    """Walk an offload stream: one header, optional caller metadata, then a framed page each."""
    if len(data) < STREAM_HEADER.size:
        raise ValueError("stream is shorter than its header")
    magic, record_format, details_length, total_pages, total_payload_bytes = STREAM_HEADER.unpack_from(data)
    if magic != STREAM_MAGIC:
        raise ValueError(f"stream magic is 0x{magic:08X}, expected 0x{STREAM_MAGIC:08X}")
    if record_format not in (RECORD_FORMAT_OPAQUE, RECORD_FORMAT_FRAMED):
        raise ValueError(f"unknown record format {record_format}")

    offset = STREAM_HEADER.size
    details = bytes(data[offset:offset + details_length])
    offset += details_length

    pages = []
    while offset + WIRE_PAGE.size <= len(data) and len(pages) < total_pages:
        seq, first, last, length, count, payload_crc = WIRE_PAGE.unpack_from(data, offset)
        offset += WIRE_PAGE.size
        payload = bytes(data[offset:offset + length])
        offset += length
        pages.append({
            "seq": seq,
            "first_timestamp": _timestamp(first),
            "last_timestamp": _timestamp(last),
            "record_count": count,
            "payload_length": length,
            # A zero-length page is not corruption: it is the device saying "this one is a gap"
            "payload_ok": (length == 0) or (payload_crc == _crc(payload)),
            "payload": payload,
        })
        # A stream carries no per-page magic, so its framing comes from the record format its header announced
        if (record_format == RECORD_FORMAT_FRAMED) and pages[-1]["payload_ok"] and length:
            walked, leftover = walk_records(payload)
            pages[-1]["records"] = walked if not leftover else None

    return {
        "record_format": record_format,
        "total_pages": total_pages,
        "total_payload_bytes": total_payload_bytes,
        "details": details,
        "pages": pages,
    }


def _report_image(pages, metadata, as_json):
    if as_json:
        json.dump({"metadata": [m.as_dict() for m in metadata],
                   "pages": [p.as_dict() for p in pages]}, sys.stdout, indent=2)
        print()
        return

    print(f"metadata slot pages: {len(metadata)}")
    newest = max((m for m in metadata if m.header_ok), key=lambda m: m.sequence, default=None)
    for m in sorted(metadata, key=lambda m: (m.sequence, m.epoch, m.index)):
        status = ("ok" if m.ok else "passed-over list failed CRC" if m.header_ok and m.details_ok else
                  "details failed CRC" if m.header_ok else "header failed CRC")
        layout = f"#{m.sequence}"
        failed = f"  write head failed at page {m.head_failed_page}" if m.head_failed_page is not None else ""
        listed = f"  {len(m.passed_over)} position(s) passed over" if m.passed_over else ""
        offset = "  SEALED" if m.sealed else ""
        current = "  <- newest" if (newest is not None and m.sequence == newest.sequence) else ""
        print(f"  page {m.index:6d}  {layout:>9}  epoch {m.epoch:5d}  log starts at page {m.log_start_page:6d}  "
              f"{len(m.details):4d} bytes of details  [{status}]{failed}{listed}{offset}{current}")

    stale = [p for p in pages if p.stale]
    pages = [p for p in pages if not p.stale]
    print(f"\nlog pages: {len(pages)}" + (f" ({len(stale)} stale copies left by relocation ignored)" if stale else ""))
    epochs = {}
    for p in pages:
        epochs.setdefault(p.epoch, []).append(p)
    for epoch in sorted(epochs):
        group = epochs[epoch]
        good = [p for p in group if p.delivers]
        tombstones = sorted(p.seq for p in group if p.tombstone)
        payload = sum(len(p.payload) for p in good)
        records = sum(p.record_count for p in good)
        seqs = sorted(p.seq for p in good)
        gaps = [s for s in range(seqs[0], seqs[-1] + 1) if s not in set(seqs)] if seqs else []
        span = [p for p in good if p.first_timestamp is not None]
        framed = sum(1 for p in good if p.framed)
        walkable = sum(1 for p in good if p.records() is not None)
        print(f"  epoch {epoch:5d}: {len(good)}/{len(group)} pages verified, "
              f"{payload} payload bytes, {records} records")
        if framed:
            print(f"                 {framed} page(s) length-framed, {walkable} fully walkable")
        if span:
            print(f"                 timestamps {min(p.first_timestamp for p in span)} .. "
                  f"{max(p.last_timestamp for p in span)}")
        if gaps:
            print(f"                 missing sequence numbers: {gaps[:16]}"
                  f"{' ...' if len(gaps) > 16 else ''}")
        if tombstones:
            print(f"                 declared lost when relocated (tombstones): {tombstones[:16]}"
                  f"{' ...' if len(tombstones) > 16 else ''}")

    bad = [p for p in pages if not p.ok and not p.tombstone]
    if bad:
        print(f"\n{len(bad)} page(s) failed verification:")
        for p in bad[:16]:
            print(f"  page {p.index:6d}  epoch {p.epoch:5d}  seq {p.seq:6d}  "
                  f"{'payload' if p.header_ok else 'header'} CRC mismatch")


def _report_stream(stream, as_json):
    pages = stream["pages"]
    if as_json:
        printable = dict(stream)
        printable["details"] = stream["details"].hex()
        printable["pages"] = [{k: v for k, v in p.items() if k != "payload"} for p in pages]
        json.dump(printable, sys.stdout, indent=2)
        print()
        return

    verified = [p for p in pages if p["payload_ok"] and p["payload_length"]]
    gaps = [p for p in pages if p["payload_length"] == 0]
    failed = [p for p in pages if not p["payload_ok"]]
    print(f"record format {stream['record_format']} ({'framed' if stream['record_format'] == RECORD_FORMAT_FRAMED else 'opaque'}), "
          f"{len(stream['details'])} bytes of caller metadata")
    print(f"pages: {len(pages)} received of {stream['total_pages']} advertised")
    print(f"  {len(verified)} verified, {len(gaps)} reported as gaps, {len(failed)} failed CRC")
    print(f"payload: {sum(p['payload_length'] for p in verified)} bytes verified "
          f"of {stream['total_payload_bytes']} advertised")
    if gaps:
        missing = [p["seq"] for p in gaps]
        print(f"  request retransmission of: {missing[:16]}{' ...' if len(missing) > 16 else ''}")


def main():
    parser = argparse.ArgumentParser(description="Parse and verify nandlog images and offload streams.")
    sub = parser.add_subparsers(dest="mode", required=True)

    image = sub.add_parser("image", help="a raw dump of the part")
    image.add_argument("path")
    image.add_argument("--page-size", type=int, default=4096)
    image.add_argument("--spare-size", type=int, default=256,
                       help="spare bytes per page in the dump; 0 if the dump is main-array only")
    image.add_argument("--pages-per-block", type=int, default=64)
    image.add_argument("--json", action="store_true")
    image.add_argument("--expect", metavar="VIEW",
                       help="compare with the library's own view of the image, as the simulator suite writes it")

    stream = sub.add_parser("stream", help="an offload stream as sent by a device")
    stream.add_argument("path")
    stream.add_argument("--json", action="store_true")

    args = parser.parse_args()
    data = open(args.path, "rb").read()

    if args.mode == "image":
        pages, metadata = parse_image(data, args.page_size, args.spare_size, args.pages_per_block)
        if args.expect:
            mismatches = compare_with_library(pages, args.expect)
            live = [p for p in pages if not p.stale]
            print(f"{args.path}: {sum(1 for p in live if p.delivers)} pages verified, "
                  f"{sum(1 for p in live if p.tombstone)} tombstone(s), "
                  f"{sum(1 for p in live if not p.ok and not p.tombstone)} failed, "
                  f"{sum(1 for p in pages if p.stale)} stale cop(ies) ignored; "
                  f"{'agrees with the library' if not mismatches else f'{mismatches} DISAGREEMENT(S)'}")
            return 1 if mismatches else 0
        _report_image(pages, metadata, args.json)
        return 1 if any(not p.ok and not p.tombstone and not p.stale for p in pages) else 0

    parsed = parse_stream(data)
    _report_stream(parsed, args.json)
    return 1 if any(not p["payload_ok"] for p in parsed["pages"]) else 0


if __name__ == "__main__":
    sys.exit(main())
