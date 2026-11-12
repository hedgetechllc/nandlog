#!/usr/bin/env python3
"""Tabulate the serial log of test_nandlog_interrupt.c: what an interrupted PROGRAM or BLOCK ERASE left on the part.

    python3 tests/interrupt_summary.py interrupt.log [more.log ...]

The firmware stops a page program or a block erase part-way with RESET (FFh) at a range of delays after the command,
and prints one INTR_PROG or INTR_ERASE line per trial and an INTR_PROG_SUM or INTR_ERASE_SUM line per delay. This
reads every capture given, tabulates both sweeps by nominal delay, and prints the headline fractions that the
simulator's fault models are to be calibrated against:

  - how often an interrupted program leaves a page reading erased in EVERY bit -- a page the recovery's blank check
    passes -- by delay;
  - how often a second program over such a page reads back clean (the library's one counted residual risk), and what
    it reads back as when it does not;
  - what the partly programmed pages read as with ECC on: unreadable (a torn page), readable and right (a weak page),
    or readable and WRONG;
  - how often three raw reads of one page disagree;
  - what an interrupted erase leaves: blocks reading erased in every page, and what a program into the first and the
    last page of a block whose erase was stopped reads back as.

A trial line is used only if every field it should carry is there, well formed and consistent with the rest of the
line; anything else -- a line SWO garbled, two lines run together -- is counted and skipped. Where the device's own
per-delay summaries are in the capture, the tallies are checked against them, which also shows any trial line lost.

Exit status: 0 if there were trials and every check agreed, 1 if a check disagreed or the device reported an error or
an aborted run, 2 if there was nothing to tabulate.
"""

import argparse
import re
import statistics
import sys

FIELD = re.compile(r'(?:^|\s)([a-z0-9_]+)=(\S+)')
HEX_BYTE = re.compile(r'[0-9A-F]{2}')
ECC_NAMES = ('none', 'corrected', 'limit', 'uncorrectable')
OUTCOMES = ('clean', 'corrected', 'uncorrectable', 'mismatch')


def segments(line):
    """Split a captured line at every INTR_ token, so two lines run together still yield both, each checked alone."""
    starts = [m.start() for m in re.finditer(r'INTR_[A-Z_]+ ', line)]
    for i, start in enumerate(starts):
        end = starts[i + 1] if (i + 1) < len(starts) else len(line)
        yield line[start:end].rstrip('\r\n ')


def outcome(ecc, match):
    """What the log would make of a page read back with ECC on: the device's own classification."""
    if ecc == 'uncorrectable':
        return 'uncorrectable'
    if not match:
        return 'mismatch'
    return 'clean' if ecc == 'none' else 'corrected'


class Malformed(Exception):
    pass


def take(fields, ints=(), floats=(), enums=None, hexes=()):
    """The named fields, converted; Malformed if any is missing or not what it should be."""
    out = {}
    try:
        for key in ints:
            out[key] = int(fields[key])
        for key in floats:
            out[key] = float(fields[key])
        for key, allowed in (enums or {}).items():
            if fields[key] not in allowed:
                raise Malformed(key)
            out[key] = fields[key]
        for key in hexes:
            if not HEX_BYTE.fullmatch(fields[key]):
                raise Malformed(key)
            out[key] = fields[key]
    except (KeyError, ValueError) as error:
        raise Malformed(str(error))
    return out


def parse_program(fields):
    t = take(fields, ints=('trial', 'nominal_us', 'block', 'page', 'ready', 'p_fail', 'pattern_zeros', 'progress',
                           'wrong', 'spare_zeros', 'blank', 'blank_reads', 'flips', 'unstable', 'match', 'second'),
             floats=('delay_us', 'reset_us'), hexes=('sr3', 'a0', 'b0'),
             enums={'class': ('blank', 'partial', 'complete'), 'ecc': ECC_NAMES})
    if t['second']:
        t.update(take(fields, ints=('second_p_fail', 'second_match', 'second_missing', 'second_wrong'),
                      floats=('second_us',), enums={'second_ecc': ECC_NAMES}))
    consistent = ((t['blank'] == 1) == (t['class'] == 'blank') and t['second'] == t['blank'] and
                  (not t['blank'] or (t['progress'] == 0 and t['wrong'] == 0 and t['spare_zeros'] == 0)) and
                  t['unstable'] == (1 if t['flips'] else 0) and 0 <= t['progress'] <= t['pattern_zeros'] and
                  0 <= t['blank_reads'] <= 3 and (not t['blank'] or t['blank_reads'] >= 1))
    if not consistent:
        raise Malformed('inconsistent')
    return t


def parse_erase(fields):
    ints = ['trial', 'nominal_us', 'block', 'ready', 'e_fail', 'fill_failures', 'blank_pages', 'max_zeros',
            'total_zeros', 'ecc_ff', 'restore_fail']
    floats = ['delay_us', 'reset_us', 'restore_us']
    enums = {}
    for prefix in ('first_', 'last_'):
        ints += [prefix + k for k in ('blank', 'p_fail', 'match', 'missing', 'wrong')]
        floats.append(prefix + 'us')
        enums[prefix + 'ecc'] = ECC_NAMES
    t = take(fields, ints=ints, floats=floats, enums=enums, hexes=('sr3', 'a0', 'b0'))
    try:
        zeros = [int(z) for z in fields['zeros'].split(',')]
    except (KeyError, ValueError):
        raise Malformed('zeros')
    ecc_map = fields.get('ecc_map', '')
    if not zeros or len(ecc_map) != len(zeros) or any(c not in '0123' for c in ecc_map):
        raise Malformed('ecc_map')
    consistent = (t['blank_pages'] == sum(1 for z in zeros if not z) and t['max_zeros'] == max(zeros) and
                  t['total_zeros'] == sum(zeros) and t['first_blank'] == (1 if not zeros[0] else 0) and
                  t['last_blank'] == (1 if not zeros[-1] else 0) and 0 <= t['ecc_ff'] <= len(zeros))
    if not consistent:
        raise Malformed('inconsistent')
    t['zeros'] = zeros
    t['ecc_pages'] = [int(c) for c in ecc_map]     # ECCS: 0 none, 1 corrected, 2 uncorrectable, 3 limit
    return t


def read_captures(paths):
    runs, rejected = [], {'INTR_PROG': 0, 'INTR_ERASE': 0}

    def current():
        if not runs:        # trials before any INTR_START: a capture begun part-way through a run
            runs.append({'start': {}, 'program': [], 'erase': [], 'program_sums': [], 'erase_sums': [], 'refs': {},
                         'errors': [], 'done': None, 'final': []})
        return runs[-1]

    for path in paths:
        with open(path, 'r', encoding='latin-1') as capture:
            for raw in capture:
                for segment in segments(raw):
                    tag = segment.split(' ', 1)[0]
                    fields = dict(FIELD.findall(segment))
                    if tag == 'INTR_START':
                        runs.append({'start': fields, 'program': [], 'erase': [], 'program_sums': [], 'erase_sums': [],
                                     'refs': {}, 'errors': [], 'done': None, 'final': []})
                    elif tag in ('INTR_PROG', 'INTR_ERASE'):
                        try:
                            trial = parse_program(fields) if tag == 'INTR_PROG' else parse_erase(fields)
                        except Malformed:
                            rejected[tag] += 1
                            continue
                        current()['program' if tag == 'INTR_PROG' else 'erase'].append(trial)
                    elif tag == 'INTR_PROG_SUM':
                        current()['program_sums'].append(fields)
                    elif tag == 'INTR_ERASE_SUM':
                        current()['erase_sums'].append(fields)
                    elif tag == 'INTR_REF':
                        name = segment.split(' ')[1] if len(segment.split(' ')) > 1 else '?'
                        current()['refs'][name] = fields
                    elif tag == 'INTR_ERROR':
                        current()['errors'].append(segment[len('INTR_ERROR '):])
                    elif tag == 'INTR_FINAL':
                        current()['final'].append(fields)
                    elif tag == 'INTR_DONE':
                        current()['done'] = fields
    return runs, rejected


def fraction(part, whole):
    return f'{part}/{whole} ({100.0 * part / whole:.0f}%)' if whole else f'{part}/0'


def median(values):
    return statistics.median(values) if values else float('nan')


def by_delay(trials):
    groups = {}
    for t in trials:
        groups.setdefault(t['nominal_us'], []).append(t)
    return sorted(groups.items())


def program_counts(trials):
    """Every tally the device's INTR_PROG_SUM carries, from the trial lines."""
    c = {'n': len(trials), 'p_fail': 0, 'blank': 0, 'partial': 0, 'complete': 0, 'match': 0, 'unstable': 0,
         'second': 0, 'second_p_fail': 0, 'not_ready': 0}
    for name in ECC_NAMES:
        c['ecc_' + name] = 0
    for name in OUTCOMES:
        c['second_' + name] = 0
    for t in trials:
        c['p_fail'] += t['p_fail']
        c[t['class']] += 1
        c['ecc_' + t['ecc']] += 1
        c['match'] += t['match']
        c['unstable'] += t['unstable']
        c['not_ready'] += 0 if t['ready'] else 1
        if t['second']:
            c['second'] += 1
            c['second_p_fail'] += t['second_p_fail']
            c['second_' + outcome(t['second_ecc'], t['second_match'])] += 1
    return c


def erase_counts(trials):
    """Every tally the device's INTR_ERASE_SUM carries, from the trial lines."""
    eccs = ('ecc_none', 'ecc_corrected', 'ecc_uncorrectable', 'ecc_limit')
    c = {'n': len(trials), 'e_fail': 0, 'fill_failures': 0, 'blank_blocks': 0, 'pages': 0, 'blank_pages': 0,
         'ecc_ff': 0, 'weak_p_fail': 0, 'not_ready': 0}
    for name in eccs:
        c[name] = 0
    for prefix in ('first_', 'last_'):
        for name in OUTCOMES:
            c[prefix + name] = 0
    for t in trials:
        pages = len(t['zeros'])
        c['e_fail'] += t['e_fail']
        c['fill_failures'] += t['fill_failures']
        c['blank_blocks'] += 1 if t['blank_pages'] == pages else 0
        c['pages'] += pages
        c['blank_pages'] += t['blank_pages']
        c['ecc_ff'] += t['ecc_ff']
        c['not_ready'] += 0 if t['ready'] else 1
        for code in t['ecc_pages']:
            c[eccs[code]] += 1
        for prefix in ('first_', 'last_'):
            c[prefix + outcome(t[prefix + 'ecc'], t[prefix + 'match'])] += 1
            c['weak_p_fail'] += t[prefix + 'p_fail']
    return c


def cross_check(runs):
    """Each device summary against the trial lines of its run and delay. Returns (checked, disagreements)."""
    checked, disagreements = 0, []
    for index, run in enumerate(runs):
        for kind, sums, trials, count in (('program', run['program_sums'], run['program'], program_counts),
                                          ('erase', run['erase_sums'], run['erase'], erase_counts)):
            for summary in sums:
                try:
                    nominal = int(summary['nominal_us'])
                except (KeyError, ValueError):
                    disagreements.append(f'run {index + 1}: a {kind} summary with no delay')
                    continue
                mine = count([t for t in trials if t['nominal_us'] == nominal])
                checked += 1
                for key, value in mine.items():
                    if key in summary and summary[key].isdigit() and int(summary[key]) != value:
                        disagreements.append(f'run {index + 1} {kind} {nominal} us: {key} device={summary[key]} '
                                             f'lines={value}')
    return checked, disagreements


def print_program_tables(trials):
    print('\nPROGRAM SWEEP -- PROGRAM EXECUTE stopped by RESET, by nominal delay (raw counts from the first raw read)')
    print(f'{"nominal":>8} {"n":>4} {"delay":>8} {"reset":>7} {"blank":>6} {"partial":>7} {"complete":>8} '
          f'{"progress":>8} {"e:none":>6} {"corr":>5} {"limit":>5} {"uncorr":>6} {"match":>5} {"unstbl":>6} '
          f'{"p_fail":>6}')
    for nominal, group in by_delay(trials):
        c = program_counts(group)
        partial = [t['progress'] / t['pattern_zeros'] for t in group if t['class'] == 'partial' and t['pattern_zeros']]
        progress = f'{100 * median(partial):.1f}%' if partial else '-'
        print(f'{nominal:>6}us {c["n"]:>4} {median([t["delay_us"] for t in group]):>8.2f} '
              f'{median([t["reset_us"] for t in group]):>7.2f} {c["blank"]:>6} {c["partial"]:>7} {c["complete"]:>8} '
              f'{progress:>8} {c["ecc_none"]:>6} {c["ecc_corrected"]:>5} {c["ecc_limit"]:>5} '
              f'{c["ecc_uncorrectable"]:>6} {c["match"]:>5} {c["unstable"]:>6} {c["p_fail"]:>6}')
    print('  delay and reset are medians in us; progress is the median share of the pattern\'s zeros programmed, over '
          'the partial pages')

    print('\nPartly programmed pages, as the log would read them (ECC on)')
    print(f'{"nominal":>8} {"partial":>7} {"torn":>6} {"weak-ok":>7} {"WRONG":>6} {"unstbl":>6} {"extra0":>6} '
          f'{"spare0":>6}')
    for nominal, group in by_delay(trials):
        partial = [t for t in group if t['class'] == 'partial']
        if not partial:
            continue
        torn = sum(1 for t in partial if t['ecc'] == 'uncorrectable')
        right = sum(1 for t in partial if t['ecc'] != 'uncorrectable' and t['match'])
        wrong = sum(1 for t in partial if t['ecc'] != 'uncorrectable' and not t['match'])
        print(f'{nominal:>6}us {len(partial):>7} {torn:>6} {right:>7} {wrong:>6} '
              f'{sum(t["unstable"] for t in partial):>6} {sum(1 for t in partial if t["wrong"]):>6} '
              f'{median([t["spare_zeros"] for t in partial]):>6.0f}')
    print('  torn: uncorrectable; weak-ok: readable and the pattern; WRONG: readable by the part\'s account and not the '
          'pattern;\n  extra0: pages with bits programmed where the pattern has ones; spare0: median parity bits '
          'programmed')

    print('\nSECOND PROGRAM over a page whose first raw read was erased in every bit (the residual risk)')
    print(f'{"nominal":>8} {"blank":>6} {"clean":>6} {"corr":>5} {"uncorr":>6} {"MISMATCH":>8} {"p_fail":>6} '
          f'{"extra0":>6} {"missing":>7} {"busy":>7}')
    for nominal, group in by_delay(trials):
        seconds = [t for t in group if t['second']]
        if not seconds:
            continue
        c = program_counts(seconds)
        print(f'{nominal:>6}us {c["second"]:>6} {c["second_clean"]:>6} {c["second_corrected"]:>5} '
              f'{c["second_uncorrectable"]:>6} {c["second_mismatch"]:>8} {c["second_p_fail"]:>6} '
              f'{max(t["second_wrong"] for t in seconds):>6} {max(t["second_missing"] for t in seconds):>7} '
              f'{median([t["second_us"] for t in seconds]):>7.1f}')
    print('  extra0 and missing are the most bits, raw, programmed where the pattern has ones and left where it has '
          'zeros; busy the median us')


def print_erase_tables(trials):
    print('\nERASE SWEEP -- BLOCK ERASE of a full block stopped by RESET, by nominal delay (pages, raw and ECC on)')
    print(f'{"nominal":>8} {"n":>3} {"delay":>8} {"reset":>7} {"e_fail":>6} {"blankblk":>8} {"blankpg":>9} '
          f'{"e:none":>6} {"corr":>5} {"limit":>5} {"uncorr":>6} {"ff":>5} {"zeros/pg":>8} {"max":>6}')
    for nominal, group in by_delay(trials):
        c = erase_counts(group)
        zeros = [z for t in group for z in t['zeros']]
        print(f'{nominal:>6}us {c["n"]:>3} {median([t["delay_us"] for t in group]):>8.1f} '
              f'{median([t["reset_us"] for t in group]):>7.2f} {c["e_fail"]:>6} {c["blank_blocks"]:>8} '
              f'{c["blank_pages"]:>4}/{c["pages"]:<4} {c["ecc_none"]:>6} {c["ecc_corrected"]:>5} {c["ecc_limit"]:>5} '
              f'{c["ecc_uncorrectable"]:>6} {c["ecc_ff"]:>5} {statistics.mean(zeros):>8.0f} {max(zeros):>6}')
    print('  blankblk: blocks erased in every bit of every page; ff: pages whose main array read all FFh with ECC on')

    print('\nWEAK-ERASE CHECK -- the first and last page programmed without erasing again, read back with ECC on')
    print(f'{"nominal":>8} {"n":>3} {"first: clean":>12} {"corr":>4} {"unc":>4} {"MISM":>4} {"last: clean":>11} '
          f'{"corr":>4} {"unc":>4} {"MISM":>4} {"p_fail":>6} {"missing":>7} {"extra0":>6}')
    for nominal, group in by_delay(trials):
        c = erase_counts(group)
        print(f'{nominal:>6}us {c["n"]:>3} {c["first_clean"]:>12} {c["first_corrected"]:>4} '
              f'{c["first_uncorrectable"]:>4} {c["first_mismatch"]:>4} {c["last_clean"]:>11} {c["last_corrected"]:>4} '
              f'{c["last_uncorrectable"]:>4} {c["last_mismatch"]:>4} {c["weak_p_fail"]:>6} '
              f'{max(max(t["first_missing"], t["last_missing"]) for t in group):>7} '
              f'{max(max(t["first_wrong"], t["last_wrong"]) for t in group):>6}')
    print('  missing and extra0: the most bits, raw, left unprogrammed where the pattern has zeros, and programmed '
          'where it has ones')


def print_headlines(program, erase, erase_floor_us):
    print('\nHEADLINES')
    if program:
        groups = by_delay(program)
        blank = [t for t in program if t['blank']]
        print(f'  An interrupted program left the page reading erased in every bit: {fraction(len(blank), len(program))} '
              f'of trials.')
        print('    by delay: ' + ', '.join(f'{n}us {sum(t["blank"] for t in g)}/{len(g)}' for n, g in groups))
        with_blank = [n for n, g in groups if any(t['blank'] for t in g)]
        without = [n for n, g in groups if not any(t['blank'] for t in g)]
        if with_blank:
            later = [n for n in without if n > max(with_blank)]
            print(f'    latest delay with any blank page: {max(with_blank)} us; delays after it with none: '
                  f'{later[0] if later else "-"} us on')
        drifting = sum(1 for t in blank if t['blank_reads'] < 3)
        surfacing = sum(1 for t in program if not t['blank'] and t['blank_reads'])
        print(f'    blank on the first raw read but not on all three: {drifting}; not blank first, blank on a later '
              f'read: {surfacing}')
        seconds = [t for t in program if t['second']]
        if seconds:
            c = program_counts(seconds)
            print(f'  A second program over such a page read back clean: {fraction(c["second_clean"], len(seconds))}; '
                  f'corrected {c["second_corrected"]}, uncorrectable {c["second_uncorrectable"]}, readable but WRONG '
                  f'{c["second_mismatch"]}, P_FAIL {c["second_p_fail"]}; with bits programmed where the pattern has '
                  f'ones {sum(1 for t in seconds if t["second_wrong"])}.')
        else:
            print('  No interrupted program left a page reading erased in every bit, so no second program was made.')
        partial = [t for t in program if t['class'] == 'partial']
        if partial:
            torn = sum(1 for t in partial if t['ecc'] == 'uncorrectable')
            right = sum(1 for t in partial if t['ecc'] != 'uncorrectable' and t['match'])
            wrong = sum(1 for t in partial if t['ecc'] != 'uncorrectable' and not t['match'])
            print(f'  Partly programmed pages: unreadable (torn) {fraction(torn, len(partial))}, readable and right '
                  f'(weak) {fraction(right, len(partial))}, readable and WRONG {fraction(wrong, len(partial))}.')
        print(f'  Raw reads that disagreed (unstable): {fraction(sum(t["unstable"] for t in program), len(program))} of '
              f'interrupted programs; {sum(t["unstable"] for t in partial)} of them partial pages.')
        print(f'  P_FAIL after RESET: {sum(t["p_fail"] for t in program)}.')
    registers = sorted({f'A0h={t["a0"]} B0h={t["b0"]}' for t in program + erase})
    if registers:
        print(f'  After RESET the registers read: {"; ".join(registers)} (power-on values are A0h=38 B0h=10; this '
              f'program had set A0h=02).')
    if erase:
        blank_blocks = [t for t in erase if t['blank_pages'] == len(t['zeros'])]
        print(f'  An interrupted erase left the block reading erased in every page: '
              f'{fraction(len(blank_blocks), len(erase))}; by delay: '
              + ', '.join(f'{n}us {sum(1 for t in g if t["blank_pages"] == len(t["zeros"]))}/{len(g)}'
                          for n, g in by_delay(erase)) + '.')
        # The weak erase is a block that reads erased though its erase was stopped: RESET before the shortest whole
        # erase this run measured. Past that, the erase may simply have finished first
        if erase_floor_us is not None:
            weak = [t for t in blank_blocks if t['delay_us'] < erase_floor_us]
            done = [t for t in blank_blocks if t['delay_us'] >= erase_floor_us]
            groups = ((f'blocks reading erased, stopped before {erase_floor_us:.0f} us (the shortest whole erase)', weak),
                      ('blocks reading erased, stopped later (the erase may have finished)', done))
        else:
            groups = (('blocks reading erased in every page', blank_blocks),)
        for label, group in groups + (('all interrupted erases', erase),):
            if not group:
                continue
            outcomes = {name: 0 for name in OUTCOMES}
            for t in group:
                for prefix in ('first_', 'last_'):
                    outcomes[outcome(t[prefix + 'ecc'], t[prefix + 'match'])] += 1
            total = 2 * len(group)
            print(f'  Programs into the first and last page of {label}: clean {fraction(outcomes["clean"], total)}, '
                  f'corrected {outcomes["corrected"]}, uncorrectable {outcomes["uncorrectable"]}, readable but WRONG '
                  f'{outcomes["mismatch"]}; with bits left unprogrammed '
                  f'{sum((t["first_missing"] > 0) + (t["last_missing"] > 0) for t in group)}.')
        print(f'  E_FAIL after RESET: {sum(t["e_fail"] for t in erase)}.')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('captures', nargs='+', help='serial captures, in the order they were made')
    args = parser.parse_args()

    runs, rejected = read_captures(args.captures)
    program = [t for run in runs for t in run['program']]
    erase = [t for run in runs for t in run['erase']]
    print(f'interrupt_summary: {", ".join(args.captures)}')
    for index, run in enumerate(runs):
        start, done = run['start'], run['done']
        state = 'no INTR_DONE (cut short, or the capture is)' if done is None else \
            f'INTR_DONE errors={done.get("errors", "?")} aborted={done.get("aborted", "?")} ' \
            f'elapsed_s={done.get("elapsed_s", "?")}'
        print(f'  run {index + 1}: source={start.get("source", "?")} part_id={start.get("part_id", "?")} '
              f'nandlog={start.get("nandlog", "?")}: {len(run["program"])} program and {len(run["erase"])} erase '
              f'trials; {state}')
    if rejected['INTR_PROG'] or rejected['INTR_ERASE']:
        print(f'  skipped as malformed: {rejected["INTR_PROG"]} INTR_PROG and {rejected["INTR_ERASE"]} INTR_ERASE lines')
    if not program and not erase:
        print('Nothing to tabulate: no well-formed INTR_PROG or INTR_ERASE line.')
        return 2

    if program:
        print_program_tables(program)
    if erase:
        print_erase_tables(erase)
    floors = []
    for run in runs:
        try:
            floors.append(float(run['refs']['erase_busy_full_block']['min']))
        except (KeyError, ValueError):
            pass
    print_headlines(program, erase, min(floors) if floors else None)

    for run in runs:
        for name, ref in sorted(run['refs'].items()):
            print(f'  Reference {name}: n={ref.get("n", "?")} median {ref.get("med", "?")} us '
                  f'(min {ref.get("min", "?")}, max {ref.get("max", "?")})')

    checked, disagreements = cross_check(runs)
    errors = [e for run in runs for e in run['errors']]
    aborted = [i + 1 for i, run in enumerate(runs) if run['done'] is None or run['done'].get('aborted') != '0']
    unfinished = [f'{f.get("block", "?")}' for run in runs for f in run['final']
                  if f.get('e_fail') != '0' or f.get('blank_pages') != f.get('pages')]
    print('\nCHECKS')
    print(f'  device summaries against the trial lines: {checked} checked, '
          + ('all agree' if not disagreements else f'{len(disagreements)} disagreements:'))
    for line in disagreements:
        print(f'    {line}')
    if errors:
        print(f'  INTR_ERROR lines: {len(errors)}')
        for error in sorted(set(errors)):
            print(f'    {error}')
    if aborted:
        print(f'  runs aborted or not finished: {", ".join(str(i) for i in aborted)}')
    if unfinished:
        print(f'  blocks NOT left erased in every bit: {", ".join(unfinished)} -- erase them before the log uses them')
    return 1 if (disagreements or errors or aborted or unfinished) else 0


if __name__ == '__main__':
    sys.exit(main())
