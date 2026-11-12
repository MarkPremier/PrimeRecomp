"""PEF (Preferred Executable Format) loader for classic Mac OS PowerPC fragments.

Implements the container, section, pattern-initialized data, loader-section
(imports, relocations, entry points) formats described in Apple's
"Mac OS Runtime Architectures" (1997), chapter 8.

The loader produces a fully relocated image at fixed guest addresses so the
static recompiler and the runtime agree on every address.
"""
import struct

# ---- guest address layout (shared with runtime/include/pt/layout.h) ----
CODE_BASE = 0x01000000
DATA_BASE = 0x01100000
IMPORT_TVEC_BASE = 0x01200000   # 8-byte fake TVectors, one per imported symbol
IMPORT_CODE_BASE = 0x01300000   # 4-byte fake code addresses, one per imported symbol
IMPORT_DATA_BASE = 0x01400000   # 256-byte blocks for imported *data* symbols

SECTION_KINDS = {0: 'code', 1: 'unpacked_data', 2: 'pattern_data', 3: 'constant',
                 4: 'loader', 5: 'debug', 6: 'exec_data', 7: 'exception', 8: 'traceback'}
SYMBOL_CLASSES = {0: 'code', 1: 'data', 2: 'tvector', 3: 'toc', 4: 'glue'}


class PEFError(Exception):
    pass


def _read_arg(buf, pos):
    """Variable-length argument used by pattern-initialized data."""
    value = 0
    while True:
        b = buf[pos]
        pos += 1
        value = (value << 7) | (b & 0x7F)
        if not b & 0x80:
            return value, pos


def unpack_pidata(packed, unpacked_size):
    out = bytearray()
    pos = 0
    while pos < len(packed):
        op = packed[pos]
        pos += 1
        opcode, count = op >> 5, op & 0x1F
        if count == 0:
            count, pos = _read_arg(packed, pos)
        if opcode == 0:      # Zero
            out += bytes(count)
        elif opcode == 1:    # blockCopy
            out += packed[pos:pos + count]
            pos += count
        elif opcode == 2:    # repeatedBlock
            repeat, pos = _read_arg(packed, pos)
            block = packed[pos:pos + count]
            pos += count
            out += block * (repeat + 1)
        elif opcode == 3:    # interleaveRepeatBlockWithBlockCopy
            custom, pos = _read_arg(packed, pos)
            repeat, pos = _read_arg(packed, pos)
            common = packed[pos:pos + count]
            pos += count
            out += common
            for _ in range(repeat):
                out += packed[pos:pos + custom]
                pos += custom
                out += common
        elif opcode == 4:    # interleaveRepeatBlockWithZero
            custom, pos = _read_arg(packed, pos)
            repeat, pos = _read_arg(packed, pos)
            out += bytes(count)
            for _ in range(repeat):
                out += packed[pos:pos + custom]
                pos += custom
                out += bytes(count)
        else:
            raise PEFError(f'bad pidata opcode {opcode} at {pos - 1}')
    if len(out) != unpacked_size:
        raise PEFError(f'pidata unpacked to {len(out)} bytes, expected {unpacked_size}')
    return bytes(out)


class Section:
    def __init__(self, index, kind, default_addr, total, unpacked, packed, offset, share, align):
        self.index = index
        self.kind = kind
        self.default_addr = default_addr
        self.total = total
        self.unpacked = unpacked
        self.packed = packed
        self.offset = offset
        self.share = share
        self.align = align
        self.base = None       # guest address once placed
        self.data = None       # bytearray of `total` bytes once instantiated

    @property
    def kind_name(self):
        return SECTION_KINDS.get(self.kind, f'kind{self.kind}')


class Import:
    def __init__(self, index, library, name, sym_class, weak):
        self.index = index
        self.library = library
        self.name = name
        self.sym_class = sym_class
        self.weak = weak

    @property
    def class_name(self):
        return SYMBOL_CLASSES.get(self.sym_class, f'class{self.sym_class}')

    @property
    def address(self):
        """Guest address the fragment sees for this symbol."""
        if self.sym_class == 1:  # data
            return IMPORT_DATA_BASE + self.index * 256
        return IMPORT_TVEC_BASE + self.index * 8

    @property
    def code_address(self):
        return IMPORT_CODE_BASE + self.index * 4


class PEF:
    def __init__(self, blob):
        self.blob = blob
        if blob[:8] != b'Joy!peff':
            raise PEFError('not a PEF container')
        (self.arch, self.format_version, self.timestamp, self.old_def_version,
         self.old_imp_version, self.current_version, nsec, self.inst_section_count
         ) = struct.unpack('>4sIIIIIHH', blob[8:36])
        if self.arch != b'pwpc':
            raise PEFError(f'unsupported architecture {self.arch!r}')
        self.sections = []
        for i in range(nsec):
            o = 40 + i * 28
            (_name, default_addr, total, unpacked, packed, offset,
             kind, share, align, _r) = struct.unpack('>iIIIIIBBBB', blob[o:o + 28])
            self.sections.append(Section(i, kind, default_addr, total, unpacked, packed, offset, share, align))
        self._parse_loader()

    # ---------------- loader section ----------------
    def _parse_loader(self):
        ld = [s for s in self.sections if s.kind == 4]
        if len(ld) != 1:
            raise PEFError('expected exactly one loader section')
        L = self.blob[ld[0].offset:ld[0].offset + ld[0].packed]
        self.loader = L
        (self.main_section, self.main_offset, self.init_section, self.init_offset,
         self.term_section, self.term_offset, nlib, nsym, nreloc_sections,
         self.reloc_instr_offset, self.strtab_offset, self.hash_offset,
         self.hash_power, self.export_count) = struct.unpack('>iIiIiIIIIIIIII', L[:56])

        def cstr(o):
            s = self.strtab_offset + o
            return L[s:L.index(b'\0', s)].decode('mac_roman')

        self.libraries = []
        for i in range(nlib):
            o = 56 + i * 24
            name_off, old_imp, cur_ver, count, first, options, _a, _b = struct.unpack('>IIIIIBBH', L[o:o + 24])
            self.libraries.append({'name': cstr(name_off), 'first': first, 'count': count,
                                   'options': options, 'current_version': cur_ver})
        sym_base = 56 + nlib * 24
        self.imports = []
        lib_of = {}
        for lib in self.libraries:
            for k in range(lib['first'], lib['first'] + lib['count']):
                lib_of[k] = lib['name']
        for k in range(nsym):
            v = struct.unpack('>I', L[sym_base + k * 4:sym_base + k * 4 + 4])[0]
            flags_class = v >> 24
            self.imports.append(Import(k, lib_of.get(k, '?'), cstr(v & 0xFFFFFF),
                                       flags_class & 0x0F, bool(flags_class & 0x80)))
        rh_base = sym_base + nsym * 4
        self.reloc_headers = []
        for i in range(nreloc_sections):
            o = rh_base + i * 12
            sec, _r, count, first = struct.unpack('>HHII', L[o:o + 12])
            self.reloc_headers.append((sec, count, first))

    # ---------------- instantiation ----------------
    def instantiate(self, bases=None):
        """Place sections at guest addresses, unpack data, apply relocations."""
        bases = bases or {}
        self.reloc_sites = []   # (section index, offset in section, delta added)
        default = {0: CODE_BASE, 1: DATA_BASE}
        for s in self.sections:
            if s.kind in (4, 5, 7, 8):
                continue
            s.base = bases.get(s.index, default.get(s.index))
            if s.base is None:
                raise PEFError(f'no base address for section {s.index}')
            raw = self.blob[s.offset:s.offset + s.packed]
            if s.kind == 2:
                body = unpack_pidata(raw, s.unpacked)
            else:
                body = raw[:s.unpacked]
            s.data = bytearray(body) + bytearray(s.total - len(body))
        for sec, count, first in self.reloc_headers:
            self._relocate(self.sections[sec], count, first)

    def section_base(self, index):
        return self.sections[index].base

    def _relocate(self, target, count, first):
        L = self.loader
        instrs = [struct.unpack('>H', L[self.reloc_instr_offset + first + 2 * i:
                                       self.reloc_instr_offset + first + 2 * i + 2])[0]
                  for i in range(count)]
        data = target.data
        st = {'addr': 0, 'imp': 0,
              'C': self.sections[0].base if self.sections[0].base is not None else 0,
              'D': self.sections[1].base if len(self.sections) > 1 and self.sections[1].base is not None else 0}

        def add(delta):
            a = st['addr']
            v = struct.unpack('>I', data[a:a + 4])[0]
            struct.pack_into('>I', data, a, (v + delta) & 0xFFFFFFFF)
            self.reloc_sites.append((target.index, a, delta))
            st['addr'] = a + 4

        def sec_base(i):
            b = self.sections[i].base
            if b is None:
                raise PEFError(f'relocation references unplaced section {i}')
            return b

        def run(pc, stop):
            while pc < stop:
                ins = instrs[pc]
                pc += 1
                if ins >> 14 == 0:                     # RelocBySectDWithSkip
                    skip, cnt = (ins >> 6) & 0xFF, ins & 0x3F
                    st['addr'] += skip * 4
                    for _ in range(cnt):
                        add(st['D'])
                elif ins >> 13 == 0b010:               # RelocRun group
                    sub, n = (ins >> 9) & 0xF, (ins & 0x1FF) + 1
                    for _ in range(n):
                        if sub == 0:
                            add(st['C'])
                        elif sub == 1:
                            add(st['D'])
                        elif sub == 2:
                            add(st['C']); add(st['D']); st['addr'] += 4
                        elif sub == 3:
                            add(st['C']); add(st['D'])
                        elif sub == 4:
                            add(st['D']); st['addr'] += 4
                        elif sub == 5:
                            add(self.imports[st['imp']].address); st['imp'] += 1
                        else:
                            raise PEFError(f'bad RelocRun subopcode {sub}')
                elif ins >> 13 == 0b011:               # RelocSmIndex group
                    sub, idx = (ins >> 9) & 0xF, ins & 0x1FF
                    if sub == 0:
                        add(self.imports[idx].address); st['imp'] = idx + 1
                    elif sub == 1:
                        st['C'] = sec_base(idx)
                    elif sub == 2:
                        st['D'] = sec_base(idx)
                    elif sub == 3:
                        add(sec_base(idx))
                    else:
                        raise PEFError(f'bad RelocSmIndex subopcode {sub}')
                elif ins >> 12 == 0b1000:              # RelocIncrPosition
                    st['addr'] += (ins & 0xFFF) + 1
                elif ins >> 12 == 0b1001:              # RelocSmRepeat
                    blocks, times = ((ins >> 8) & 0xF) + 1, (ins & 0xFF) + 1
                    for _ in range(times):
                        run(pc - 1 - blocks, pc - 1)
                elif ins >> 10 == 0b101000:            # RelocSetPosition
                    st['addr'] = ((ins & 0x3FF) << 16) | instrs[pc]; pc += 1
                elif ins >> 10 == 0b101001:            # RelocLgByImport
                    idx = ((ins & 0x3FF) << 16) | instrs[pc]; pc += 1
                    add(self.imports[idx].address); st['imp'] = idx + 1
                elif ins >> 10 == 0b101100:            # RelocLgRepeat
                    blocks = ((ins >> 6) & 0xF) + 1
                    times = (((ins & 0x3F) << 16) | instrs[pc]); pc += 1
                    for _ in range(times):
                        run(pc - 2 - blocks, pc - 2)
                elif ins >> 10 == 0b101101:            # RelocLgSetOrBySection
                    sub = (ins >> 6) & 0xF
                    idx = ((ins & 0x3F) << 16) | instrs[pc]; pc += 1
                    if sub == 0:
                        add(sec_base(idx))
                    elif sub == 1:
                        st['C'] = sec_base(idx)
                    elif sub == 2:
                        st['D'] = sec_base(idx)
                    else:
                        raise PEFError(f'bad RelocLgSetOrBySection subopcode {sub}')
                else:
                    raise PEFError(f'unknown relocation opcode {ins:#06x}')
        run(0, len(instrs))

    # ---------------- entry points ----------------
    def symbol_address(self, section, offset):
        if section < 0:
            return None
        return self.sections[section].base + offset

    @property
    def main_tvector(self):
        return self.symbol_address(self.main_section, self.main_offset)
