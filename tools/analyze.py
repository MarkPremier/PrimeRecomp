"""Instantiate the Prime Target PEF, find functions, and write the image description.

Outputs (in <out_dir>):
    code.bin, data.bin   relocated section contents
    image.json           layout, imports, entry points, functions

Function boundaries come from the AIX-style traceback tables CodeWarrior emits
after every function: a zero word, then the table, whose optional tb_offset
field gives the distance back to the function's first instruction.

Usage: python tools/analyze.py <PEF data fork> <out_dir>
"""
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
            tb = parse_traceback(code, pos)
            if tb and 'tb_offset' in tb and tb['end'] <= len(code):
                start = pos - tb['tb_offset']
                if prev_end <= start < pos and start % 4 == 0:
                    prev_end = tb['end']
                    funcs.append({'start': start, 'body_end': pos, 'tb_end': tb['end'], **tb})
                    pos = tb['end']
                    continue
        pos += 4
    return funcs


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    p = peflib.PEF(open(src, 'rb').read())
    p.instantiate()
    code_s, data_s = p.sections[0], p.sections[1]
    code, data = bytes(code_s.data), bytes(data_s.data)

    funcs = find_functions(code)
    # sanity: functions must not overlap and should tile the code section
    gaps = []
    prev_end = 0
    for f in funcs:
        if f['start'] < prev_end:
            raise SystemExit(f"overlapping functions at {f['start']:#x}")
        if f['start'] > prev_end:
            gaps.append((prev_end, f['start']))
        prev_end = f['tb_end']
    if prev_end < len(code):
        gaps.append((prev_end, len(code)))

    main_tv = p.main_tvector
    mo = main_tv - data_s.base
    main_code, main_toc = struct.unpack('>II', data[mo:mo + 8])

    # relocation sanity: count words that look like guest pointers
    def classify(v):
        if code_s.base <= v < code_s.base + code_s.total:
            return 'code'
        if data_s.base <= v < data_s.base + data_s.total:
            return 'data'
        if peflib.IMPORT_TVEC_BASE <= v < peflib.IMPORT_TVEC_BASE + 8 * len(p.imports):
            return 'import'
        if 0x02000000 <= v < 0x04000000:
            return 'suspicious'  # base added twice?
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
        'code_base': code_s.base, 'code_size': code_s.total,
        'data_base': data_s.base, 'data_size': data_s.total, 'data_init_size': data_s.unpacked,
        'main_tvector': main_tv, 'main_code': main_code, 'toc': main_toc,
        'init_tvector': p.symbol_address(p.init_section, p.init_offset),
        'term_tvector': p.symbol_address(p.term_section, p.term_offset),
        'import_tvec_base': peflib.IMPORT_TVEC_BASE,
        'import_code_base': peflib.IMPORT_CODE_BASE,
        'import_data_base': peflib.IMPORT_DATA_BASE,
        'imports': [{'index': i.index, 'library': i.library, 'name': i.name,
                     'class': i.class_name, 'weak': i.weak, 'address': i.address,
                     'code_address': i.code_address} for i in p.imports],
        'functions': [{'start': code_s.base + f['start'], 'end': code_s.base + f['body_end'],
                       'name': f.get('name'), 'saves_lr': f['saves_lr'],
                       'fixedparms': f['fixedparms'], 'floatparms': f['floatparms']}
                      for f in funcs],
        'gaps': [[code_s.base + a, code_s.base + b] for a, b in gaps],
        # words relocated by the code section base: [guest address of word, value]
        'code_pointers': [[p.sections[sec].base + off,
                           struct.unpack('>I', p.sections[sec].data[off:off + 4])[0]]
                          for sec, off, delta in p.reloc_sites if delta == code_s.base],
    }
    json.dump(image, open(os.path.join(out, 'image.json'), 'w'), indent=1)

    print(f'code {code_s.base:#x}+{code_s.total:#x}  data {data_s.base:#x}+{data_s.total:#x} '
          f'(initialized {data_s.unpacked:#x})')
    print(f'main tvector {main_tv:#x} -> code {main_code:#x}, toc {main_toc:#x}')
    print(f'init {image["init_tvector"]}, term {image["term_tvector"]}')
    print(f'imports {len(p.imports)}; classes', {c: sum(1 for i in p.imports if i.class_name == c)
                                                 for c in set(i.class_name for i in p.imports)})
    print(f'functions {len(funcs)} (named {sum(1 for f in funcs if f.get("name"))}); '
          f'gaps {len(gaps)} totalling {sum(b - a for a, b in gaps)} bytes')
    print('pointer-like words in data:', kinds)
    mf = [f for f in funcs if code_s.base + f['start'] == main_code]
    print('main function:', mf[0].get('name') if mf else 'NOT AT A FUNCTION START')


if __name__ == '__main__':
    main()
