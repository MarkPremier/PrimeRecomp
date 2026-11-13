"""Quick disassembly of the instantiated image using capstone (for inspection only).

Usage: python tools/dis.py <image_dir> <start_hex> <end_hex>
"""
import json
import os
import struct
import sys

import capstone

img = sys.argv[1]
start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
im = json.load(open(os.path.join(img, 'image.json')))
code = open(os.path.join(img, 'code.bin'), 'rb').read()
data = open(os.path.join(img, 'data.bin'), 'rb').read()
names = {f['start']: f['name'] for f in im['functions']}
imports = {i['address']: i['name'] for i in im['imports']}
toc = im['toc']

md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
md.skipdata = True   # keep going past words capstone cannot decode
base = im['code_base']
for ins in md.disasm(code[start - base:end - base], start):
    note = ''
    if ins.address in names:
        print(f'\n{names[ins.address]}:')
    # annotate TOC loads: lwz rX, off(r2)
    if ins.mnemonic == 'lwz' and ', r2)' in ins.op_str.replace('(r2)', ', r2)'):
        try:
            off = int(ins.op_str.split(',')[1].split('(')[0], 0)
            a = toc + off
            v = struct.unpack('>I', data[a - im['data_base']:a - im['data_base'] + 4])[0]
            note = f'  ; [toc{off:+#x}] = {v:#x} {imports.get(v, names.get(v, ""))}'
        except Exception:
            pass
    if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('0x'):
        t = int(ins.op_str, 16)
        if t in names:
            note = f'  ; {names[t]}'
    print(f'{ins.address:08x}: {ins.bytes.hex()}  {ins.mnemonic:8} {ins.op_str}{note}')
