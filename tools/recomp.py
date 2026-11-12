"""Static recompiler: Prime Target PowerPC code -> C.

Reads build/image (from analyze.py) and writes C sources that operate on a
PPCContext (runtime/include/pt/ppc.h).

Model:
  * one C function per guest function; guest branches inside it become goto
  * bl -> direct C call (LR is set to the return address first, for fidelity)
  * b to another function's start -> tail call
  * bctr -> switch over the function's jump-table targets, else indirect tail call
  * blr -> return
Anything the lifter does not understand stops generation with a clear error.

Usage: python tools/recomp.py <image_dir> <out_dir>
"""
import bisect
import json
import os
import struct
import sys
from collections import defaultdict

FUNCS_PER_FILE = 150


def s16(v):
    return v - 0x10000 if v & 0x8000 else v


def mask32(mb, me):
    if mb <= me:
        m = ((0xFFFFFFFF >> mb) & (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    else:
        m = ((0xFFFFFFFF >> mb) | (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    return m


class LiftError(Exception):
    pass


# --------------------------------------------------------------------------
# function discovery
# --------------------------------------------------------------------------
class Image:
    def __init__(self, d):
        self.meta = json.load(open(os.path.join(d, 'image.json')))
        self.code = open(os.path.join(d, 'code.bin'), 'rb').read()
        self.data = open(os.path.join(d, 'data.bin'), 'rb').read()
        self.cbase = self.meta['code_base']
        self.dbase = self.meta['data_base']
        self.toc = self.meta['toc']

    def word(self, addr):
        o = addr - self.cbase
        return struct.unpack('>I', self.code[o:o + 4])[0]

    def dword(self, addr):
        o = addr - self.dbase
        return struct.unpack('>I', self.data[o:o + 4])[0]


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


def discover(img, gap_start, gap_end):
    """Return sorted list of (start, end, name) for every function."""
    funcs = [(f['start'], f['end'], f['name'].lstrip('.')) for f in img.meta['functions']]
    named_ranges = sorted(funcs)
    entries = set()
    # bl targets from named functions
    for s, e, _ in named_ranges:
        for pc in range(s, e, 4):
            w = img.word(pc)
            if w >> 26 in (16, 18) and w & 1:
                entries.add(branch_target(w, pc))
    entries.add(img.meta['main_code'])
    # TVectors: (code pointer, TOC) pairs in data
    for addr, val in img.meta['code_pointers']:
        try:
            if img.dword(addr + 4) == img.toc:
                entries.add(val)
        except struct.error:
            pass
    # sweep the gap: bl targets inside it, iterating until stable
    changed = True
    while changed:
        changed = False
        gap_entries = sorted(t for t in entries if gap_start <= t < gap_end)
        for i, s in enumerate(gap_entries):
            e = gap_entries[i + 1] if i + 1 < len(gap_entries) else gap_end
            for pc in range(s, e, 4):
                w = img.word(pc)
                if w >> 26 in (16, 18) and w & 1:
                    t = branch_target(w, pc)
                    if t not in entries and gap_start <= t < gap_end and t != pc + 4:
                        entries.add(t)
                        changed = True
    gap_entries = sorted(t for t in entries if gap_start <= t < gap_end)
    out = list(named_ranges)
    for i, s in enumerate(gap_entries):
        e = gap_entries[i + 1] if i + 1 < len(gap_entries) else gap_end
        out.append((s, e, None))
    return sorted(out)


# --------------------------------------------------------------------------
# instruction lifting
# --------------------------------------------------------------------------
R = lambda n: f'c->r[{n}]'
F = lambda n: f'c->f[{n}]'


def ra0(a):
    return '0' if a == 0 else R(a)


class FuncLifter:
    def __init__(self, img, start, end, name, func_starts, jt_targets):
        self.img, self.start, self.end, self.name = img, start, end, name
        self.func_starts = func_starts
        self.jt = sorted(t for t in jt_targets if start < t < end)
        self.labels = set(self.jt)
        self.lines = []
        self.errors = []
        self.calls = set()

    # -- condition helpers --
    def crbit(self, bit):
        f, b = bit >> 2, bit & 3
        return f'c->cr[{f}].{("lt", "gt", "eq", "so")[b]}'

    def bc_cond(self, bo, bi, decrement=True):
        parts = []
        pre = ''
        if not bo & 0x04:
            if not decrement:
                raise LiftError('bcctr with CTR decrement')
            pre = '--c->ctr; '
            parts.append('c->ctr == 0' if bo & 0x02 else 'c->ctr != 0')
        if not bo & 0x10:
            parts.append(self.crbit(bi) if bo & 0x08 else f'!{self.crbit(bi)}')
        return pre, ' && '.join(parts) if parts else None

    def emit(self, s):
        self.lines.append('    ' + s)

    def branch_to(self, t, pc, call):
        if call:
            if t == pc + 4:  # bcl 20,31,$+4 idiom: just materialise LR
                return f'c->lr = 0x{pc + 4:08X}u;'
            if t not in self.func_starts:
                raise LiftError(f'call to non-function 0x{t:08X}')
            self.calls.add(t)
            return f'c->lr = 0x{pc + 4:08X}u; fn_{t:08X}(c);'
        if self.start <= t < self.end:
            self.labels.add(t)
            return f'goto L_{t:08X};'
        if t in self.func_starts:
            self.calls.add(t)
            return f'{{ fn_{t:08X}(c); return; }}'
        raise LiftError(f'branch to middle of another function 0x{t:08X}')

    def lift(self):
        # pass 1: collect labels so pass 2 can emit them
        for pc in range(self.start, self.end, 4):
            w = self.img.word(pc)
            op = w >> 26
            if op in (16, 18) and not (w & 1):
                t = branch_target(w, pc)
                if self.start <= t < self.end:
                    self.labels.add(t)
        body = []
        for pc in range(self.start, self.end, 4):
            w = self.img.word(pc)
            self.lines = []
            try:
                self.lift_one(w, pc)
            except LiftError as e:
                self.errors.append(f'0x{pc:08X} {w:08X}: {e}')
                self.lines = [f'    ppc_fatal("unlifted instruction %08X at %08X", 0x{w:08X}u, 0x{pc:08X}u);']
            if pc in self.labels:
                body.append(f'L_{pc:08X}:')
            body.extend(self.lines)
        return body

    def lift_one(self, w, pc):
        op = w >> 26
        d, a, b = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        simm, uimm = s16(w & 0xFFFF), w & 0xFFFF
        rc = w & 1
        E = self.emit

        if w == 0x60000000:
            return  # nop
        if op == 7:
            E(f'{R(d)} = (uint32_t)((int32_t){R(a)} * {simm});')
        elif op == 8:
            E(f'{{ uint32_t t = {R(a)}; {R(d)} = (uint32_t){simm} - t; c->xer_ca = t <= (uint32_t){simm}; }}')
        elif op == 10:
            E(f'cr_set_unsigned(c, {d >> 2}, {R(a)}, 0x{uimm:X}u);')
        elif op == 11:
            E(f'cr_set_signed(c, {d >> 2}, (int32_t){R(a)}, {simm});')
        elif op in (12, 13):
            E(f'{{ uint32_t t = {R(a)}; {R(d)} = t + (uint32_t){simm}; c->xer_ca = {R(d)} < t; }}')
            if op == 13:
                E(f'cr0_rec(c, {R(d)});')
        elif op == 14:
            E(f'{R(d)} = {ra0(a)} + (uint32_t){simm};')
        elif op == 15:
            E(f'{R(d)} = {ra0(a)} + 0x{(uimm << 16) & 0xFFFFFFFF:08X}u;')
        elif op == 16:
            bo, bi, lk = d, a, w & 1
            t = branch_target(w, pc)
            pre, cond = self.bc_cond(bo, bi)
            act = self.branch_to(t, pc, lk)
            if pre:
                E(pre.strip())
            E(f'if ({cond}) {act}' if cond else act)
        elif op == 18:
            E(self.branch_to(branch_target(w, pc), pc, w & 1))
        elif op == 19:
            self.lift_19(w, pc, d, a, b)
        elif op in (20, 21, 23):
            sh, mb, me = b, (w >> 6) & 31, (w >> 1) & 31
            m = mask32(mb, me)
            if op == 21:
                E(f'{R(a)} = rotl32({R(d)}, {sh}) & 0x{m:08X}u;')
            elif op == 23:
                E(f'{R(a)} = rotl32({R(d)}, {R(b)} & 31) & 0x{m:08X}u;')
            else:
                E(f'{R(a)} = (rotl32({R(d)}, {sh}) & 0x{m:08X}u) | ({R(a)} & 0x{(~m) & 0xFFFFFFFF:08X}u);')
            if rc:
                E(f'cr0_rec(c, {R(a)});')
        elif op == 24:
            E(f'{R(a)} = {R(d)} | 0x{uimm:X}u;')
        elif op == 25:
            E(f'{R(a)} = {R(d)} | 0x{uimm << 16:08X}u;')
        elif op == 26:
            E(f'{R(a)} = {R(d)} ^ 0x{uimm:X}u;')
        elif op == 27:
            E(f'{R(a)} = {R(d)} ^ 0x{uimm << 16:08X}u;')
        elif op == 28:
            E(f'{R(a)} = {R(d)} & 0x{uimm:X}u; cr0_rec(c, {R(a)});')
        elif op == 29:
            E(f'{R(a)} = {R(d)} & 0x{uimm << 16:08X}u; cr0_rec(c, {R(a)});')
        elif op == 31:
            self.lift_31(w, pc, d, a, b, rc)
        elif 32 <= op <= 55:
            self.lift_mem(op, d, a, simm)
        elif op == 59:
            self.lift_fp_single(w, d, a, b, rc)
        elif op == 63:
            self.lift_fp(w, d, a, b, rc)
        elif op == 3:  # twi
            if d == 31:
                E(f'ppc_trap(c, 0x{pc:08X}u);')
            else:
                raise LiftError('conditional twi')
        else:
            raise LiftError(f'primary opcode {op}')

    def lift_19(self, w, pc, d, a, b):
        xo = (w >> 1) & 0x3FF
        E = self.emit
        lk = w & 1
        if xo == 16:  # bclr
            pre, cond = self.bc_cond(d, a)
            if lk:
                act = f'{{ uint32_t t = c->lr; c->lr = 0x{pc + 4:08X}u; ppc_call_indirect(c, t); }}'
            else:
                act = 'return;'
            if pre:
                E(pre.strip())
            E(f'if ({cond}) {act}' if cond else act)
        elif xo == 528:  # bcctr
            pre, cond = self.bc_cond(d, a, decrement=False)
            if lk:
                act = f'{{ c->lr = 0x{pc + 4:08X}u; ppc_call_indirect(c, c->ctr); }}'
            elif self.jt:
                cases = ' '.join(f'case 0x{t:08X}u: goto L_{t:08X};' for t in self.jt)
                act = f'{{ switch (c->ctr) {{ {cases} default: ppc_call_indirect(c, c->ctr); return; }} }}'
            else:
                act = '{ ppc_call_indirect(c, c->ctr); return; }'
            E(f'if ({cond}) {act}' if cond else act)
        elif xo == 0:  # mcrf
            E(f'c->cr[{d >> 2}] = c->cr[{a >> 2}];')
        elif xo in (33, 129, 193, 225, 257, 289, 417, 449):
            A, B = self.crbit(a), self.crbit(b)
            expr = {33: f'!({A} | {B})', 129: f'{A} & !{B}', 193: f'{A} ^ {B}', 225: f'!({A} & {B})',
                    257: f'{A} & {B}', 289: f'!({A} ^ {B})', 417: f'{A} | !{B}', 449: f'{A} | {B}'}[xo]
            E(f'{self.crbit(d)} = (uint8_t)({expr});')
        elif xo == 150:
            pass  # isync
        else:
            raise LiftError(f'op19 xo {xo}')

    def lift_31(self, w, pc, d, a, b, rc):
        xo = (w >> 1) & 0x3FF
        xo9 = xo & 0x1FF
        oe = (w >> 10) & 1
        E = self.emit
        RD, RA, RB = R(d), R(a), R(b)
        arith = {266: f'{RA} + {RB}', 40: f'{RB} - {RA}', 104: f'(uint32_t)(-(int32_t){RA})',
                 235: f'(uint32_t)((int32_t){RA} * (int32_t){RB})',
                 75: f'(uint32_t)(((int64_t)(int32_t){RA} * (int32_t){RB}) >> 32)',
                 11: f'(uint32_t)(((uint64_t){RA} * {RB}) >> 32)',
                 491: f'divw({RA}, {RB})', 459: f'divwu({RA}, {RB})'}
        carry = {10: (RA, RB, '0', False), 8: (f'~{RA}', RB, '1', False), 138: (RA, RB, 'c->xer_ca', False),
                 136: (f'~{RA}', RB, 'c->xer_ca', False), 202: (RA, '0', 'c->xer_ca', False),
                 200: (f'~{RA}', '0', 'c->xer_ca', False), 234: (RA, '0xFFFFFFFFu', 'c->xer_ca', False),
                 232: (f'~{RA}', '0xFFFFFFFFu', 'c->xer_ca', False)}
        if xo9 in arith or xo9 in carry:
            if oe:
                raise LiftError('OE (overflow-enable) arithmetic')
            if xo9 in arith:
                E(f'{RD} = {arith[xo9]};')
            else:
                x, y, ci, _ = carry[xo9]
                E(f'{{ uint64_t t = (uint64_t)(uint32_t)({x}) + (uint32_t)({y}) + {ci}; {RD} = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}')
            if rc:
                E(f'cr0_rec(c, {RD});')
            return
        # logical / shift: rS = d, result in rA
        RS = R(d)
        logical = {28: f'{RS} & {RB}', 60: f'{RS} & ~{RB}', 444: f'{RS} | {RB}', 412: f'{RS} | ~{RB}',
                   316: f'{RS} ^ {RB}', 124: f'~({RS} | {RB})', 476: f'~({RS} & {RB})', 284: f'~({RS} ^ {RB})',
                   24: f'(({RB} & 63) >= 32 ? 0u : {RS} << ({RB} & 63))',
                   536: f'(({RB} & 63) >= 32 ? 0u : {RS} >> ({RB} & 63))',
                   954: f'(uint32_t)(int32_t)(int8_t){RS}', 922: f'(uint32_t)(int32_t)(int16_t){RS}',
                   26: f'cntlzw({RS})', 792: f'sraw(c, {RS}, {RB})', 824: f'sraw(c, {RS}, {b})'}
        if xo in logical:
            E(f'{RA} = {logical[xo]};')
            if rc:
                E(f'cr0_rec(c, {RA});')
            return
        if rc and xo not in (150,):
            raise LiftError(f'record form of op31 xo {xo}')
        if xo == 0:
            E(f'cr_set_signed(c, {d >> 2}, (int32_t){RA}, (int32_t){RB});')
        elif xo == 32:
            E(f'cr_set_unsigned(c, {d >> 2}, {RA}, {RB});')
        elif xo in (23, 55, 87, 119, 279, 311, 343, 375, 151, 183, 215, 247, 407, 439, 534, 662, 790, 918,
                    535, 567, 599, 631, 663, 695, 727, 759):
            upd = xo in (55, 119, 311, 375, 183, 247, 439, 567, 631, 695, 759)
            ea = f'{RA} + {RB}' if upd else f'{ra0(a)} + {RB}'
            acc = {23: f'{RD} = ld32(ea);', 55: f'{RD} = ld32(ea);', 87: f'{RD} = ld8(ea);', 119: f'{RD} = ld8(ea);',
                   279: f'{RD} = ld16(ea);', 311: f'{RD} = ld16(ea);',
                   343: f'{RD} = (uint32_t)(int32_t)(int16_t)ld16(ea);', 375: f'{RD} = (uint32_t)(int32_t)(int16_t)ld16(ea);',
                   151: f'st32(ea, {RD});', 183: f'st32(ea, {RD});', 215: f'st8(ea, {RD});', 247: f'st8(ea, {RD});',
                   407: f'st16(ea, {RD});', 439: f'st16(ea, {RD});',
                   534: f'{RD} = ld32br(ea);', 662: f'st32br(ea, {RD});', 790: f'{RD} = ld16br(ea);', 918: f'st16br(ea, {RD});',
                   535: f'{F(d)} = ldf32(ea);', 567: f'{F(d)} = ldf32(ea);', 599: f'{F(d)} = ldf64(ea);', 631: f'{F(d)} = ldf64(ea);',
                   663: f'stf32(ea, {F(d)});', 695: f'stf32(ea, {F(d)});', 727: f'stf64(ea, {F(d)});', 759: f'stf64(ea, {F(d)});'}[xo]
            E(f'{{ uint32_t ea = {ea}; {acc}{f" {RA} = ea;" if upd else ""} }}')
        elif xo == 19:
            E(f'{RD} = mfcr(c);')
        elif xo == 144:
            E(f'mtcrf(c, 0x{(w >> 12) & 0xFF:02X}u, {RD});')
        elif xo in (339, 467, 371):
            spr = ((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5)
            if xo == 371 or spr in (268, 269):
                E(f'{RD} = {"ppc_timebase_lo()" if spr == 268 else "ppc_timebase_hi()"};')
                return
            field = {1: None, 8: 'c->lr', 9: 'c->ctr'}.get(spr, 'bad')
            if field == 'bad':
                raise LiftError(f'spr {spr}')
            if xo == 339:
                E(f'{RD} = {field};' if field else
                  f'{RD} = (c->xer_so << 31) | (c->xer_ov << 30) | (c->xer_ca << 29);')
            else:
                E(f'{field} = {RD};' if field else
                  f'c->xer_so = ({RD} >> 31) & 1; c->xer_ov = ({RD} >> 30) & 1; c->xer_ca = ({RD} >> 29) & 1;')
        elif xo == 1014:
            E(f'memset(g_mem + (({ra0(a)} + {RB}) & ~31u), 0, 32);')
        elif xo in (54, 86, 246, 278, 470, 982, 598, 854):
            pass  # cache / sync hints
        elif xo == 4:
            if d == 31:
                E(f'ppc_trap(c, 0x{pc:08X}u);')
            else:
                raise LiftError('conditional tw')
        else:
            raise LiftError(f'op31 xo {xo}')

    def lift_mem(self, op, d, a, simm):
        E = self.emit
        upd = op in (33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55)
        ea = f'{R(a)} + (uint32_t){simm}' if upd else f'{ra0(a)} + (uint32_t){simm}'
        RD = R(d)
        if op == 46:
            E(f'{{ uint32_t ea = {ea}; for (int i = {d}; i < 32; i++, ea += 4) c->r[i] = ld32(ea); }}')
            return
        if op == 47:
            E(f'{{ uint32_t ea = {ea}; for (int i = {d}; i < 32; i++, ea += 4) st32(ea, c->r[i]); }}')
            return
        acc = {32: f'{RD} = ld32(ea);', 34: f'{RD} = ld8(ea);', 40: f'{RD} = ld16(ea);',
               42: f'{RD} = (uint32_t)(int32_t)(int16_t)ld16(ea);', 36: f'st32(ea, {RD});', 38: f'st8(ea, {RD});',
               44: f'st16(ea, {RD});', 48: f'{F(d)} = ldf32(ea);', 50: f'{F(d)} = ldf64(ea);',
               52: f'stf32(ea, {F(d)});', 54: f'stf64(ea, {F(d)});'}[op & ~1 if upd else op]
        if upd and a == 0:
            raise LiftError('update form with rA=0')
        E(f'{{ uint32_t ea = {ea}; {acc}{f" {R(a)} = ea;" if upd else ""} }}')

    def lift_fp_single(self, w, d, a, b, rc):
        xo = (w >> 1) & 31
        cr = (w >> 6) & 31
        A, B, C, D = F(a), F(b), F(cr), F(d)
        ops = {18: f'{A} / {B}', 20: f'{A} - {B}', 21: f'{A} + {B}', 22: f'sqrt({B})', 24: f'1.0 / {B}',
               25: f'{A} * {C}', 28: f'fma({A}, {C}, -{B})', 29: f'fma({A}, {C}, {B})',
               30: f'-fma({A}, {C}, -{B})', 31: f'-fma({A}, {C}, {B})'}
        if xo not in ops:
            raise LiftError(f'op59 xo {xo}')
        if rc:
            raise LiftError('FP record form')
        self.emit(f'{D} = (double)(float)({ops[xo]});')

    def lift_fp(self, w, d, a, b, rc):
        xo5 = (w >> 1) & 31
        xo = (w >> 1) & 0x3FF
        cr = (w >> 6) & 31
        A, B, C, D = F(a), F(b), F(cr), F(d)
        E = self.emit
        if rc:
            raise LiftError('FP record form')
        aform = {18: f'{A} / {B}', 20: f'{A} - {B}', 21: f'{A} + {B}', 22: f'sqrt({B})',
                 23: f'({A} >= 0.0 ? {C} : {B})', 25: f'{A} * {C}', 26: f'1.0 / sqrt({B})',
                 28: f'fma({A}, {C}, -{B})', 29: f'fma({A}, {C}, {B})',
                 30: f'-fma({A}, {C}, -{B})', 31: f'-fma({A}, {C}, {B})'}
        if xo5 in aform and xo5 >= 18:
            E(f'{D} = {aform[xo5]};')
            return
        if xo in (0, 32):
            E(f'cr_set_fp(c, {d >> 2}, {A}, {B});')
        elif xo == 12:
            E(f'{D} = (double)(float){B};')
        elif xo in (14, 15):
            E(f'{D} = fctiw_bits({B}, {1 if xo == 15 else 0});')
        elif xo == 40:
            E(f'{D} = -{B};')
        elif xo == 72:
            E(f'{D} = {B};')
        elif xo == 136:
            E(f'{D} = -fabs({B});')
        elif xo == 264:
            E(f'{D} = fabs({B});')
        elif xo == 583:
            E(f'{D} = bits2f(0xFFF8000000000000ull | c->fpscr);')
        elif xo == 711:
            fm = (w >> 17) & 0xFF
            m = 0
            for i in range(8):
                if fm & (0x80 >> i):
                    m |= 0xF << (28 - 4 * i)
            E(f'c->fpscr = (c->fpscr & ~0x{m:08X}u) | ((uint32_t)f2bits({B}) & 0x{m:08X}u);')
        elif xo in (38, 70):
            bit = d
            E(f'c->fpscr {"|=" if xo == 38 else "&= ~"}(0x80000000u >> {bit});' if xo == 38 else
              f'c->fpscr &= ~(0x80000000u >> {bit});')
        elif xo == 134:
            crf, imm = d >> 2, (w >> 12) & 0xF
            E(f'c->fpscr = (c->fpscr & ~(0xF0000000u >> {4 * crf})) | (0x{imm:X}u << {28 - 4 * crf});')
        else:
            raise LiftError(f'op63 xo {xo}')


# --------------------------------------------------------------------------
def main():
    img_dir, out = sys.argv[1], sys.argv[2]
    img = Image(img_dir)
    gap = img.meta['gaps'][-1]
    exc_table_start = 0x0106D400  # CodeWarrior exception index lives at the end of the code section
    funcs = discover(img, gap[0], exc_table_start)
    first_gap = img.meta['gaps'][0]
    funcs.append((first_gap[0], first_gap[1], None))
    funcs.sort()
    starts = {s for s, _, _ in funcs}

    # jump-table targets: code pointers that land inside a function body
    fstarts = [s for s, _, _ in funcs]
    jt = defaultdict(set)
    for _, val in img.meta['code_pointers']:
        i = bisect.bisect_right(fstarts, val) - 1
        if i >= 0 and funcs[i][0] < val < funcs[i][1]:
            jt[funcs[i][0]].add(val)

    os.makedirs(out, exist_ok=True)
    errors = []
    bodies = []
    for s, e, name in funcs:
        fl = FuncLifter(img, s, e, name, starts, jt.get(s, ()))
        body = fl.lift()
        errors += [f'{name or "?"} {x}' for x in fl.errors]
        bodies.append((s, e, name, body))

    with open(os.path.join(out, 'ppc_funcs.h'), 'w') as fh:
        fh.write('/* generated by tools/recomp.py - do not edit */\n#pragma once\n#include "pt/ppc.h"\n')
        fh.write('#ifdef __cplusplus\nextern "C" {\n#endif\n')
        fh.write('uint32_t ppc_timebase_lo(void);\nuint32_t ppc_timebase_hi(void);\n')
        for s, e, name, _ in bodies:
            fh.write(f'void fn_{s:08X}(PPCContext *c);{f" /* {name} */" if name else ""}\n')
        fh.write('typedef struct { uint32_t addr; PPCFunc fn; const char *name; } PPCFuncEntry;\n')
        fh.write('extern const PPCFuncEntry ppc_func_table[];\nextern const unsigned ppc_func_count;\n')
        fh.write('#ifdef __cplusplus\n}\n#endif\n')

    files = []
    for i in range(0, len(bodies), FUNCS_PER_FILE):
        fn = f'ppc_code_{i // FUNCS_PER_FILE:02d}.c'
        files.append(fn)
        with open(os.path.join(out, fn), 'w') as fh:
            fh.write('/* generated by tools/recomp.py - do not edit */\n#include "ppc_funcs.h"\n')
            fh.write('#ifndef PT_FN_ENTER\n#define PT_FN_ENTER(addr)\n#endif\n')
            fh.write('#pragma warning(disable: 4102 4146)\n\n')
            for s, e, name, body in bodies[i:i + FUNCS_PER_FILE]:
                fh.write(f'/* {name or "(runtime)"} [{s:08X}-{e:08X}) */\nvoid fn_{s:08X}(PPCContext *c) {{\n')
                fh.write(f'    PT_FN_ENTER(0x{s:08X}u);\n')
                fh.write('\n'.join(body))
                fh.write('\n}\n\n')
    with open(os.path.join(out, 'ppc_table.c'), 'w') as fh:
        fh.write('/* generated by tools/recomp.py - do not edit */\n#include "ppc_funcs.h"\n')
        fh.write('const PPCFuncEntry ppc_func_table[] = {\n')
        for s, e, name, _ in bodies:
            fh.write(f'    {{0x{s:08X}u, fn_{s:08X}, "{name or f"sub_{s:08X}"}"}},\n')
        fh.write('};\n')
        fh.write(f'const unsigned ppc_func_count = {len(bodies)};\n')
    with open(os.path.join(out, 'ppc_imports.c'), 'w') as fh:
        fh.write('/* generated by tools/recomp.py - do not edit */\n')
        fh.write('/* imported symbols in PEF order; index i lives at TVector IMPORT_TVEC_BASE + 8*i */\n')
        fh.write('const char *const ppc_import_names[] = {\n')
        for imp in img.meta['imports']:
            fh.write(f'    "{imp["name"]}", /* {imp["library"]} */\n')
        fh.write('};\n')
        fh.write(f'const unsigned ppc_import_count = {len(img.meta["imports"])};\n')
    with open(os.path.join(out, 'sources.cmake'), 'w') as fh:
        fh.write('set(PT_GENERATED_SOURCES\n' + ''.join(f'    ${{CMAKE_CURRENT_LIST_DIR}}/{f}\n'
                                                        for f in files + ['ppc_table.c', 'ppc_imports.c']) + ')\n')
    with open(os.path.join(out, 'image_info.h'), 'w') as fh:
        m = img.meta
        fh.write('/* generated by tools/recomp.py - do not edit */\n#pragma once\n')
        for k in ('code_base', 'code_size', 'data_base', 'data_size', 'main_code', 'toc', 'main_tvector'):
            fh.write(f'#define PT_IMAGE_{k.upper()} 0x{m[k]:08X}u\n')

    print(f'functions: {len(bodies)} ({sum(1 for b in bodies if b[2])} named), '
          f'jump-table functions: {len(jt)}, files: {len(files)}')
    print(f'lift errors: {len(errors)}')
    for e in errors[:60]:
        print('  ', e)
    with open(os.path.join(out, 'lift_errors.txt'), 'w') as fh:
        fh.write('\n'.join(errors))


if __name__ == '__main__':
    main()
