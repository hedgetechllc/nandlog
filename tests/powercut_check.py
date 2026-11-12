#!/usr/bin/env python3
"""Check the serial log of test_nandlog_powercut.c against nandlog's durability contract.

    python3 tests/powercut_check.py capture.log [more.log ...] [--verbose]

The firmware prints, on every boot, a POWERCUT_START line naming its build (counted per build in the summary, so
captures of different builds are not mistaken for one), then -- once it has read back the whole current epoch --
one POWERCUT line summarizing what it found, then a POWERCUT_ACK line each time a flush(true) succeeds while it
logs. The lines that matter carry a CRC-32 of their own text, so a line that a power cut tore off half-way, or that
ran into the next boot's output, is recognized and skipped rather than misread.

VIOLATIONS (any one fails the run):
  - a boot delivered a damaged record, a duplicated one, or one out of order;
  - between two boots of the same epoch, a run's record count went DOWN, or the run of records unbroken from its
    index 0 got shorter, or a run disappeared: recovery destroyed data that an earlier boot had already read back;
  - a record acknowledged as durable (POWERCUT_ACK ... upto=i: records 0..i of that run were committed and read
    back) was missing at a later boot of the same epoch -- checked against the unbroken prefix, so a hole in the
    middle of a run cannot hide behind a large count.

WARNINGS (reported, not failures): a run's count went UP between boots (a page that read as a gap became readable),
holes inside a run, an epoch's page count going down, POWERCUT_ERR/IDLE/FATAL lines, and library ERROR/retirement
messages.

UNATTENDED BUILDS. A build with PC_SELF_CUT or PC_INJECT_FAILURES says so in POWERCUT_START (self_cut=, inject=) and
prints, CRC-protected, a POWERCUT_SELFCUT line for each operation it stopped with the part's RESET before resetting
itself, and a POWERCUT_INJECT line for each program or erase it made report failure. Both are tallied -- by phase,
command, what was being written, and plan; with the delays before RESET -- and reported in the summary, as is the
range of retired= over the verified boots. They change nothing above: the cuts are counted and checked as any cut
is, a reset reason other than a power-on is what a self-cut boot ends with, and a block retired after an injection is
the failure path doing its job, not a violation.

Exit status: 0 if there were no violations, 1 if there were, 2 if the log held no verified boot at all.
"""

import argparse
import re
import statistics
import sys
import zlib

TOKEN = 'POWERCUT'
FIELD = re.compile(r'([a-z_]+)=(\S+)')
CRC_FIELD = re.compile(r' crc=([0-9A-Fa-f]{8})')
BUILD_FIELDS = ('nandlog', 'source', 'port_crc', 'erase_ahead', 'head_pass', 'self_cut', 'inject')


def segments(line):
    """Split a captured line at every POWERCUT token, so that a torn line followed by the next boot's output on the
    same line still yields the later, whole one."""
    starts = [m.start() for m in re.finditer(TOKEN, line)]
    for i, start in enumerate(starts):
        end = starts[i + 1] if (i + 1) < len(starts) else len(line)
        yield line[start:end].rstrip('\r\n')


def crc_valid(segment):
    """True if the segment carries a crc= field that matches the text before it (up to and including the space)."""
    match = CRC_FIELD.search(segment)
    if not match:
        return False
    body = segment[:match.start() + 1]
    return (zlib.crc32(body.encode('latin-1', 'replace')) & 0xFFFFFFFF) == int(match.group(1), 16)


def parse_fields(segment):
    return dict(FIELD.findall(segment))


def parse_per_run(text):
    """'3:120,4:96:40' -> {3: (120, 120), 4: (96, 40)} as (count, prefix), where prefix is how many records from
    index 0 are all present (the device prints it only when it differs from the count); '-' -> {}; a trailing
    ',overflow' is reported separately."""
    runs = {}
    overflow = False
    if text in (None, '-'):
        return runs, overflow
    for item in text.split(','):
        if item == 'overflow':
            overflow = True
            continue
        parts = item.split(':')
        if len(parts) in (2, 3) and all(p.isdigit() for p in parts):
            count = int(parts[1])
            runs[int(parts[0])] = (count, int(parts[2]) if len(parts) == 3 else count)
    return runs, overflow


def percentile(values, pct):
    if not values:
        return 0
    ordered = sorted(values)
    rank = max(1, -(-pct * len(ordered) // 100))
    return ordered[rank - 1]


class Checker:
    def __init__(self, verbose):
        self.verbose = verbose
        self.boots = 0                    # POWERCUT_START lines
        self.builds = {}                  # what each POWERCUT_START names of its build -> boots
        self.verified = 0                 # POWERCUT lines with a valid CRC
        self.foreign = 0
        self.torn = 0                     # CRC lines that failed their CRC
        self.reached_logging = []         # per boot: whether its POWERCUT line was seen
        self.began_mid_boot = False       # the capture's first lines came from a boot already running
        self.violations = []
        self.warnings = []
        self.acks = {}                    # (epoch, run) -> highest acknowledged index
        self.acks_seen = 0
        self.acks_checked = 0
        self.acks_lost = set()
        self.last = {}                    # epoch -> (boot number, per_run dict, pages) from its latest verified line
        self.epochs = []
        self.init_us_after_cut = []
        self.verify_us = []
        self.skipped = 0
        self.gaps = {}                    # epoch -> gaps / tombstones at its latest verified line
        self.records = {}                 # epoch -> records at its latest verified line
        self.epoch_events = 0
        self.errors = []
        self.reset_reasons = {}
        self.library_messages = {}
        self.self_cuts = []               # POWERCUT_SELFCUT lines with a valid CRC, as their fields
        self.injections = []              # POWERCUT_INJECT lines with a valid CRC, as their fields
        self.retired = []                 # retired= of each verified POWERCUT line, in order

    def violation(self, where, text):
        self.violations.append('%s: %s' % (where, text))

    def warning(self, where, text):
        self.warnings.append('%s: %s' % (where, text))

    def boot_line(self, where, segment):
        if not crc_valid(segment):
            self.torn += 1
            return
        f = parse_fields(segment)
        try:
            epoch = int(f['epoch'])
            boot = int(f['boot'])
            numbers = {k: int(f.get(k, '0')) for k in ('runs', 'records', 'pages', 'gaps', 'tombstones', 'damaged',
                                                        'out_of_order', 'dup', 'holes', 'skipped', 'init_us',
                                                        'verify_us')}
        except (KeyError, ValueError):
            self.torn += 1
            return
        self.verified += 1
        if f.get('retired', '').isdigit():
            self.retired.append(int(f['retired']))
        if self.reached_logging:
            self.reached_logging[-1] = True
        if len(self.reached_logging) > 1 and numbers['init_us']:
            self.init_us_after_cut.append(numbers['init_us'])
        if numbers['verify_us']:
            self.verify_us.append(numbers['verify_us'])
        self.skipped += numbers['skipped']
        if epoch not in self.epochs:
            self.epochs.append(epoch)
        if f.get('foreign') == '1':
            # Not this program's epoch: nothing in it can be checked, and nothing carries over
            self.foreign += 1
            self.last.pop(epoch, None)
            return

        where = '%s boot=%d epoch=%d' % (where, boot, epoch)
        for key in ('damaged', 'out_of_order', 'dup'):
            if numbers[key]:
                self.violation(where, '%d %s record(s) delivered' % (numbers[key], key.replace('_', '-')))
        if numbers['holes']:
            self.warning(where, '%d record(s) missing inside runs (gaps=%d tombstones=%d)'
                         % (numbers['holes'], numbers['gaps'], numbers['tombstones']))
        per_run, overflow = parse_per_run(f.get('per_run'))
        if overflow:
            self.warning(where, 'per_run list overflowed on the device; only the first runs were compared')

        previous = self.last.get(epoch)
        if previous:
            prev_boot, prev_runs, prev_pages = previous
            for run, (count, prefix) in sorted(prev_runs.items()):
                now_count, now_prefix = per_run.get(run, (0, 0))
                if now_count < count or now_prefix < prefix:
                    self.violation(where, 'run %d had %d records (%d unbroken from the start) at boot %d and has %d '
                                   '(%d) now -- committed data lost' % (run, count, prefix, prev_boot, now_count,
                                                                         now_prefix))
                elif now_count > count:
                    self.warning(where, 'run %d grew from %d to %d records since boot %d (a gap became readable)'
                                 % (run, count, now_count, prev_boot))
            if numbers['pages'] < prev_pages:
                self.warning(where, 'the epoch shrank from %d to %d pages since boot %d'
                             % (prev_pages, numbers['pages'], prev_boot))

        for (ack_epoch, run), upto in sorted(self.acks.items()):
            if ack_epoch != epoch:
                continue
            self.acks_checked += 1
            count, prefix = per_run.get(run, (0, 0))
            if (prefix < (upto + 1)) and ((ack_epoch, run) not in self.acks_lost):
                self.acks_lost.add((ack_epoch, run))      # reported once, not again at every later boot
                self.violation(where, 'run %d was acknowledged durable through record %d, but only records 0..%d of '
                               'it were delivered unbroken (%d in all)' % (run, upto, prefix - 1, count))

        self.last[epoch] = (boot, per_run, numbers['pages'])
        self.gaps[epoch] = (numbers['gaps'], numbers['tombstones'])
        self.records[epoch] = numbers['records']
        if self.verbose:
            print('  verified %s: %d runs, %d records, %d pages, gaps=%d tombstones=%d init=%d us verify=%d us'
                  % (where, numbers['runs'], numbers['records'], numbers['pages'], numbers['gaps'],
                     numbers['tombstones'], numbers['init_us'], numbers['verify_us']))

    def ack_line(self, segment):
        if not crc_valid(segment):
            self.torn += 1
            return
        f = parse_fields(segment)
        try:
            key = (int(f['epoch']), int(f['run']))
            upto = int(f['upto'])
        except (KeyError, ValueError):
            self.torn += 1
            return
        self.acks_seen += 1
        if upto > self.acks.get(key, -1):
            self.acks[key] = upto

    def unattended_line(self, segment, into):
        """A POWERCUT_SELFCUT or POWERCUT_INJECT line: kept, by its fields, if its CRC holds."""
        if not crc_valid(segment):
            self.torn += 1
            return
        into.append(parse_fields(segment))

    def feed(self, name, number, line):
        where = '%s:%d' % (name, number)
        if 'Reset Reasons:' in line:
            for reason in re.findall(r'([A-Za-z\- ]+?(?:Reset|Brownout|Initialization)),', line):
                reason = reason.strip()
                self.reset_reasons[reason] = self.reset_reasons.get(reason, 0) + 1
        for marker in ('ERROR:', 'WARNING:'):
            if marker in line and TOKEN not in line and 'Could not connect to target' not in line:  # J-Link's viewer
                text = line[line.index(marker):].strip()
                text = re.sub(r'\d+', 'N', text)
                self.library_messages[text] = self.library_messages.get(text, 0) + 1
        for segment in segments(line):
            tag = segment.split(' ', 1)[0]
            if tag != 'POWERCUT_START' and not self.reached_logging:
                # The capture began after this boot's start, so the boot that prints the first POWERCUT_START follows
                # a cut too
                self.reached_logging.append(False)
                self.began_mid_boot = True
            if tag == 'POWERCUT_START':
                self.boots += 1
                f = parse_fields(segment)
                build = ' '.join('%s=%s' % (k, f[k]) for k in BUILD_FIELDS if k in f) or 'unrecorded'
                self.builds[build] = self.builds.get(build, 0) + 1
                self.reached_logging.append(False)
            elif tag == 'POWERCUT':
                self.boot_line(where, segment)
            elif tag == 'POWERCUT_ACK':
                self.ack_line(segment)
            elif tag == 'POWERCUT_EPOCH':
                if crc_valid(segment):
                    self.epoch_events += 1
                else:
                    self.torn += 1
            elif tag == 'POWERCUT_SELFCUT':
                self.unattended_line(segment, self.self_cuts)
            elif tag == 'POWERCUT_INJECT':
                self.unattended_line(segment, self.injections)
            elif tag in ('POWERCUT_ERR', 'POWERCUT_FATAL', 'POWERCUT_IDLE'):
                self.errors.append('%s: %s' % (where, segment.strip()))

    def unattended_summary(self, cuts):
        """The self-cut and injection tallies, printed only for a capture of a build that has them."""
        def counts(lines, key, order=None):
            tally = {}
            for f in lines:
                tally[f.get(key, '?')] = tally.get(f.get(key, '?'), 0) + 1
            keys = [k for k in (order or []) if k in tally] + sorted(k for k in tally if k not in (order or []))
            return ', '.join('%s x%d' % (k, tally[k]) for k in keys) or '-'

        def numbers(lines, key):
            return [int(f[key]) for f in lines if f.get(key, '').isdigit()]

        def spread(values):
            if not values:
                return 'n=0'
            return 'n=%d min=%d med=%d max=%d' % (len(values), min(values), int(statistics.median(values)), max(values))

        built_self_cut = any('self_cut=1' in build for build in self.builds)
        built_inject = any('inject=1' in build for build in self.builds)
        if self.self_cuts or built_self_cut:
            lines = self.self_cuts
            programs = [f for f in lines if f.get('cmd') == 'program']
            erases = [f for f in lines if f.get('cmd') == 'erase']
            phase = dict((p, sum(1 for f in lines if f.get('phase') == p)) for p in ('init', 'logging'))
            print('  self-cuts (POWERCUT_SELFCUT)    : %d   (%d during init, %d while logging; %d program, %d erase)'
                  % (len(lines), phase['init'], phase['logging'], len(programs), len(erases)))
            print('    by phase and command          : init: %s; logging: %s'
                  % (counts([f for f in lines if f.get('phase') == 'init'], 'cmd', ['program', 'erase']),
                     counts([f for f in lines if f.get('phase') == 'logging'], 'cmd', ['program', 'erase'])))
            print('    what was being written        : program: %s; erase: %s'
                  % (counts(programs, 'target', ['page', 'slot', 'table', 'copy', 'other']),
                     counts(erases, 'target', ['log', 'ring', 'reserve'])))
            print('    plan                          : %s' % counts(lines, 'plan', ['init', 'early', 'late', 'recovery']))
            print('    delay before RESET, us        : program %s; erase %s'
                  % (spread(numbers(programs, 'delay_us')), spread(numbers(erases, 'delay_us'))))
            print('    RESET until OIP clear, us     : %s; OIP still set after it: %d'
                  % (spread(numbers(lines, 'reset_us')), sum(1 for f in lines if f.get('ready') == '0')))
            print('    random state kept over reset  : %d of %d cuts (retained= above 0)'
                  % (sum(1 for f in lines if f.get('retained', '0') not in ('0', '')), len(lines)))
            if cuts > len(lines):
                print('    cuts with no SELFCUT line     : %d   (a boot that idled and reset itself, a line SWO lost, or a '
                      'cut by hand)' % (cuts - len(lines)))
        if self.injections or built_inject:
            lines = self.injections
            print('  injected failures (POWERCUT_INJECT): %d   (%s; %s)'
                  % (len(lines), counts(lines, 'cmd', ['program', 'erase']), counts(lines, 'phase', ['init', 'logging'])))
        if (self.self_cuts or self.injections or built_self_cut or built_inject) and self.retired:
            print('  retired blocks (POWERCUT retired=): %d at the first verified boot, %d at the last, %d at most%s'
                  % (self.retired[0], self.retired[-1], max(self.retired),
                     ' -- retirement after an injected failure is expected' if self.injections else ''))

    def summary(self):
        cuts = max(0, len(self.reached_logging) - 1)
        logging_cuts = sum(1 for reached in self.reached_logging[:-1] if reached)
        early_cuts = cuts - logging_cuts
        print('powercut_check')
        print('  boots (POWERCUT_START)          : %d%s' % (self.boots, '   (and one already running when the capture began)'
                                                                if self.began_mid_boot else ''))
        for i, (build, n) in enumerate(sorted(self.builds.items(), key=lambda item: -item[1])):
            print('  %s: %s x%d' % ('builds (POWERCUT_START)         ' if i == 0 else ' ' * 32, build, n))
        if len(self.builds) > 1:
            print('    (more than one build: these captures are not all of one library)')
        print('  boots verified (POWERCUT line)  : %d   (%d on an epoch this program did not write)'
              % (self.verified, self.foreign))
        print('  power cuts                      : %d   (%d while logging, %d during init or read-back)'
              % (cuts, logging_cuts, early_cuts))
        print('  epochs                          : %d   (%d announced by POWERCUT_EPOCH)'
              % (len(self.epochs), self.epoch_events))
        print('  acknowledgements                : %d seen; %d checks of them at later boots'
              % (self.acks_seen, self.acks_checked))
        if self.records:
            print('  records verified                : %d, summed over the last verified boot of each of %d epochs'
                  % (sum(self.records.values()), len(self.records)))
            print('  gaps / tombstones in them       : %d / %d'
                  % (sum(g for g, _ in self.gaps.values()), sum(t for _, t in self.gaps.values())))
        print('  torn positions passed over      : %d (sum of skipped= over all boots)' % self.skipped)
        if self.init_us_after_cut:
            v = self.init_us_after_cut
            print('  nandlog_init() after a cut, us  : n=%d min=%d med=%d p99=%d max=%d'
                  % (len(v), min(v), int(statistics.median(v)), percentile(v, 99), max(v)))
        if self.verify_us:
            v = self.verify_us
            print('  full read-back of the epoch, us : n=%d min=%d med=%d max=%d'
                  % (len(v), min(v), int(statistics.median(v)), max(v)))
        if self.reset_reasons:
            print('  reset reasons                   : %s'
                  % ', '.join('%s x%d' % (k, n) for k, n in sorted(self.reset_reasons.items())))
            if self.self_cuts:
                print('    (a self-cut boot ends by resetting the MCU itself, so a software reset is expected here)')
        self.unattended_summary(cuts)
        if self.torn:
            print('  torn or garbled lines skipped   : %d' % self.torn)
            if self.verified and self.torn > max(10, self.verified // 10):
                print('    (many more than one per cut: check that the capture is not breaking long lines)')
        if self.library_messages:
            print('  library diagnostics:')
            for text, n in sorted(self.library_messages.items(), key=lambda item: -item[1])[:12]:
                print('    x%-5d %s' % (n, text))
        if self.errors:
            print('  device errors (%d):' % len(self.errors))
            for text in self.errors[:20]:
                print('    ' + text)
        if self.warnings:
            print('  warnings (%d):' % len(self.warnings))
            for text in self.warnings[:40]:
                print('    ' + text)
            if len(self.warnings) > 40:
                print('    ... and %d more' % (len(self.warnings) - 40))
        print('  violations (%d):' % len(self.violations))
        for text in self.violations[:100]:
            print('    ' + text)
        if len(self.violations) > 100:
            print('    ... and %d more' % (len(self.violations) - 100))
        if not self.verified:
            print('RESULT: NO DATA -- no verified boot found in the log')
            return 2
        if self.violations:
            print('RESULT: FAIL -- %d violation(s) over %d verified boots and %d cuts'
                  % (len(self.violations), self.verified, cuts))
            return 1
        print('RESULT: PASS -- 0 violations over %d verified boots and %d cuts' % (self.verified, cuts))
        return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('logs', nargs='+', help='captured serial log(s), in the order they were recorded')
    parser.add_argument('--verbose', action='store_true', help='print one line per verified boot')
    args = parser.parse_args()
    checker = Checker(args.verbose)
    for name in args.logs:
        with open(name, 'r', encoding='latin-1', errors='replace') as log:
            for number, line in enumerate(log, 1):
                checker.feed(name, number, line)
    return checker.summary()


if __name__ == '__main__':
    sys.exit(main())
