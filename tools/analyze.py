"""Instantiate the Prime Target PEF, find every function, and write the image description.

Outputs (in <out_dir>):
    code.bin, data.bin   relocated section contents
    image.json           layout, imports, entry points, functions

Game functions come from one of two places:
  * the AIX-style traceback tables CodeWarrior emits after every function
    (v1.1 keeps them: a zero word, then the table, whose tb_offset field gives
    the distance back to the function's first instruction), or
  * a symbol file (symbols/*.tsv, made by make_symbols.py) for builds without
    tables, such as v1.2 on the CD.
The CodeWarrior runtime library after the game code has neither; its entry
points come from call targets and TVectors. The C++ exception index at the
very end of the code section is data.

Usage: python tools/analyze.py <PEF data fork> <out_dir> [--symbols <file.tsv>]
"""
import hashlib
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import pef as peflib


def parse_traceback(code, pos):
    """Parse a traceback table whose zero word is at `pos`. Returns dict or None."""
    if pos + 12 > len(code) or code[pos:pos + 4] != b'\0\0\0\0':
        return None
    t = pos + 4
    version, lang, f1, f2, f3, f4, fixedparms, f5 = code[t:t + 8]
    if version != 0 or lang > 0x0F:
        return None
    has_tboff = bool(f1 & 0x20)
    int_hndl = bool(f2 & 0x80)
    name_present = bool(f2 & 0x40)
    uses_alloca = bool(f2 & 0x20)
    has_ctl = bool(f1 & 0x08)
    floatparms = f5 >> 1
    p = t + 8
    if p + 16 > len(code):
        return None
    info = {
        'lang': lang, 'saves_lr': bool(f2 & 0x01), 'saves_cr': bool(f2 & 0x02),
        'fpr_saved': f3 & 0x3F, 'gpr_saved': f4 & 0x3F,
        'fixedparms': fixedparms, 'floatparms': floatparms,
        'fp_present': bool(f1 & 0x02),
    }
    if fixedparms or floatparms:
        info['parminfo'] = struct.unpack('>I', code[p:p + 4])[0]
        p += 4
    if has_tboff:
        info['tb_offset'] = struct.unpack('>I', code[p:p + 4])[0]
        p += 4
    if int_hndl:
        p += 4
    if has_ctl:
        n = struct.unpack('>I', code[p:p + 4])[0]
        p += 4 + 4 * n
    if name_present:
        n = struct.unpack('>H', code[p:p + 2])[0]
        p += 2
        info['name'] = code[p:p + n].decode('mac_roman')
        p += n
    if uses_alloca:
        p += 1
    info['end'] = (p + 3) & ~3
    return info


def find_functions(code):
    """Scan for traceback tables and derive [start, end) for every function."""
    funcs = []
    pos = 0
    prev_end = 0
    while pos < len(code) - 4:
        # a zero word is a candidate table; real tables chain end-to-end, so the
        # function it describes must start at or after the previous table's end
        if code[pos:pos + 4] == b'\0\0\0\0':
            try:
                tb = parse_traceback(code, pos)
            except (struct.error, UnicodeDecodeError):
                tb = None
            if tb and 'tb_offset' in tb and tb['end'] <= len(code):
                start = pos - tb['tb_offset']
                if prev_end <= start < pos and start % 4 == 0:
                    prev_end = tb['end']
                    funcs.append({'start': start, 'body_end': pos, 'tb_end': tb['end'], **tb})
                    pos = tb['end']
                    continue
        pos += 4
    return funcs


def exception_index_start(code):
    """CodeWarrior's exception index: (function offset, 0x80000000|length, info) triples at the end."""
    pos = len(code) - 12
    while pos >= 0:
        off, ln, _ = struct.unpack_from('>III', code, pos)
        if not (off < len(code) and off % 4 == 0 and ln & 0x80000000 and not ln & 0x7FF00000):
            break
        pos -= 12
    return pos + 12


def read_symbols(path, blob):
    funcs, runtime = [], None
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if line.startswith('# sha1 '):
            want = line.split()[2]
            if hashlib.sha1(blob).hexdigest() != want:
                raise SystemExit(f'{path} is for a different build (sha1 {want})')
        elif line.startswith('# runtime '):
            runtime = int(line.split()[2], 16)
        elif line and not line.startswith('#'):
            start, end, name = line.split('\t')[:3]
            funcs.append({'start': int(start, 16), 'end': int(end, 16), 'name': name})
    if runtime is None:
        raise SystemExit(f'{path}: no "# runtime" line')
    return funcs, runtime


def branch_target(w, pc):
    op = w >> 26
    if op == 18:
        li = w & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        return (li if w & 2 else pc + li) & 0xFFFFFFFF
    if op == 16:
        bd = w & 0xFFFC
        if bd & 0x8000:
            bd -= 0x10000
        return (bd if w & 2 else pc + bd) & 0xFFFFFFFF
    return None


def discover_runtime(word, named, lo, hi, seeds):
    """Entry points in [lo, hi): calls from game code and seeds, then calls inside, until stable."""
    entries = set(seeds)
    for f in named:
        for pc in range(f['start'], f['end'], 4):
            w = word(pc)
            if w >> 26 in (16, 18) and w & 1:
                entries.add(branch_target(w, pc))
    changed = True
    while changed:
        changed = False
        inside = sorted(t for t in entries if lo <= t < hi)
        for i, s in enumerate(inside):
            e = inside[i + 1] if i + 1 < len(inside) else hi
            for pc in range(s, e, 4):
                w = word(pc)
                if w >> 26 in (16, 18) and w & 1:
                    t = branch_target(w, pc)
                    if t not in entries and lo <= t < hi and t != pc + 4:
                        entries.add(t)
                        changed = True
    inside = sorted(t for t in entries if lo <= t < hi)
    return [(s, inside[i + 1] if i + 1 < len(inside) else hi) for i, s in enumerate(inside)]


def main():
    args = sys.argv[1:]
    symbols = None
    if '--symbols' in args:
        k = args.index('--symbols')
        symbols = args[k + 1]
        del args[k:k + 2]
    src, out = args
    os.makedirs(out, exist_ok=True)
    blob = open(src, 'rb').read()
    p = peflib.PEF(blob)
    p.instantiate()
    code_s, data_s = p.sections[0], p.sections[1]
    code, data = bytes(code_s.data), bytes(data_s.data)
    cb, db = code_s.base, data_s.base

    main_tv = p.main_tvector
    mo = main_tv - db
    main_code, main_toc = struct.unpack('>II', data[mo:mo + 8])
    code_pointers = [[p.sections[sec].base + off, struct.unpack('>I', p.sections[sec].data[off:off + 4])[0]]
                     for sec, off, delta in p.reloc_sites if delta == cb]

    # game functions
    if symbols:
        named, runtime_start = read_symbols(symbols, blob)
        source = os.path.basename(symbols)
    else:
        tb = find_functions(code)
        if not tb:
            raise SystemExit('no traceback tables in this build; pass --symbols symbols/<build>.tsv')
        named = [{'start': cb + f['start'], 'end': cb + f['body_end'], 'name': f['name'].lstrip('.')}
                 for f in tb]
        runtime_start = cb + tb[-1]['tb_end']
        source = 'traceback tables'
    for a, b in zip(named, named[1:]):
        if a['end'] > b['start']:
            raise SystemExit(f"overlapping functions at {b['start']:#x}")

    # runtime library: from the end of game code up to the exception index
    exc = cb + exception_index_start(code)
    word = lambda a: struct.unpack_from('>I', code, a - cb)[0]
    seeds = {main_code}
    for addr, val in code_pointers:
        o = addr - db
        if 0 <= o and o + 8 <= len(data) and struct.unpack_from('>I', data, o + 4)[0] == main_toc:
            seeds.add(val)
    runtime = discover_runtime(word, named, runtime_start, exc, seeds)

    functions = []
    if named[0]['start'] > cb:   # glue ahead of the first game function
        functions.append({'start': cb, 'end': named[0]['start'], 'name': None})
    functions += named
    functions += [{'start': s, 'end': e, 'name': None} for s, e in runtime]
    functions.sort(key=lambda f: f['start'])

    # relocation sanity: count words that look like guest pointers
    def classify(v):
        if cb <= v < cb + code_s.total:
            return 'code'
        if db <= v < db + data_s.total:
            return 'data'
        if peflib.IMPORT_TVEC_BASE <= v < peflib.IMPORT_TVEC_BASE + 8 * len(p.imports):
            return 'import'
        return None
    kinds = {}
    for i in range(0, len(data) - 3, 4):
        k = classify(struct.unpack('>I', data[i:i + 4])[0])
        if k:
            kinds[k] = kinds.get(k, 0) + 1

    open(os.path.join(out, 'code.bin'), 'wb').write(code)
    open(os.path.join(out, 'data.bin'), 'wb').write(data)
    image = {
        'source': os.path.abspath(src),
        'app_name': os.path.basename(src),
        'sha1': hashlib.sha1(blob).hexdigest(),
        'code_base': cb, 'code_size': code_s.total,
        'data_base': db, 'data_size': data_s.total, 'data_init_size': data_s.unpacked,
        'main_tvector': main_tv, 'main_code': main_code, 'toc': main_toc,
        'init_tvector': p.symbol_address(p.init_section, p.init_offset),
        'term_tvector': p.symbol_address(p.term_section, p.term_offset),
        'import_tvec_base': peflib.IMPORT_TVEC_BASE,
        'import_code_base': peflib.IMPORT_CODE_BASE,
        'import_data_base': peflib.IMPORT_DATA_BASE,
        'imports': [{'index': i.index, 'library': i.library, 'name': i.name,
                     'class': i.class_name, 'weak': i.weak, 'address': i.address,
                     'code_address': i.code_address} for i in p.imports],
        'runtime_start': runtime_start,
        'exception_index': exc,
        'functions': functions,
        # words relocated by the code section base: [guest address of word, value]
        'code_pointers': code_pointers,
    }
    json.dump(image, open(os.path.join(out, 'image.json'), 'w'), indent=1)

    print(f'code {cb:#x}+{code_s.total:#x}  data {db:#x}+{data_s.total:#x} '
          f'(initialized {data_s.unpacked:#x})')
    print(f'main tvector {main_tv:#x} -> code {main_code:#x}, toc {main_toc:#x}')
    print(f'imports {len(p.imports)}')
    print(f'functions {len(functions)}: {len(named)} game ({source}), {len(runtime)} runtime library '
          f'[{runtime_start:#x}, {exc:#x})')
    print('pointer-like words in data:', kinds)


if __name__ == '__main__':
    main()
