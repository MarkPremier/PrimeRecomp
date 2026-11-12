"""Carry function boundaries and names from a build that has traceback tables
(Prime Target v1.1) to one that does not (v1.2, the version on the CD).

v1.2 was built from nearly the same source with traceback tables turned off,
so most functions are byte-identical once two things are masked out: the
displacement of `b`/`bl` (functions moved) and the offset of TOC-relative
accesses (the TOC gained an entry). Whole files can also move in link order
(v1.2 links the network code last). The procedure:

1. every reference function is searched for in the target; unique hits are
   placed (longest first, never overlapping);
2. ambiguous ones (small, repeated bodies) are chained off a placed
   neighbour: the hit that directly follows or precedes it;
3. the runtime library is found by searching for the reference's;
4. each stretch of target code still unclaimed is split at the target's own
   call targets and TVectors, and aligned in order against the unplaced
   reference functions that neighbour it, by instruction similarity. What
   pairs up has changed; target chunks left over are new; reference
   functions left over were removed.

Output: a TSV symbol file for the target build (read by analyze.py --symbols)
and a report of the differences.

Usage: python tools/make_symbols.py <target PEF> <reference PEF> <out.tsv>
"""
import difflib
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
import analyze
import pef as peflib

DFORM = {14, 15} | set(range(32, 56))


def load(path):
    blob = open(path, 'rb').read()
    p = peflib.PEF(blob)
    p.instantiate()
    return blob, p


def words(code):
    return [struct.unpack_from('>I', code, i)[0] for i in range(0, len(code) - 3, 4)]


def mask(w, cond_branches):
    op = w >> 26
    if op == 18:
        return w & 0xFC000003
    if op == 16 and cond_branches:
        return w & 0xFFFF0003
    if op in DFORM and ((w >> 16) & 31) == 2:
        return w & 0xFFFF0000
    return w


def normalize(code):
    return b''.join(struct.pack('>I', mask(w, False)) for w in words(code))


def loose(code):
    """Instruction words with every branch displacement and TOC offset masked (for similarity)."""
    return [mask(w, True) for w in words(code)]


def find_all(hay, needle, lo=0, hi=None, limit=8):
    hi = len(hay) if hi is None else hi
    hits = []
    p = hay.find(needle, lo, hi)
    while p != -1 and len(hits) < limit:
        if p % 4 == 0:
            hits.append(p)
        p = hay.find(needle, p + 1, hi)
    return hits


def call_entries(code, base, data, dbase, toc, code_ptrs):
    """Offsets in `code` that the target's own code calls or exposes as TVectors."""
    ents = set()
    for pc, w in enumerate(words(code)):
        pc *= 4
        if w >> 26 == 18 and w & 1 and not w & 2:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            t = pc + li
            if 0 <= t < len(code) and t != pc + 4:
                ents.add(t)
    for addr, val in code_ptrs:
        o = addr - dbase
        if 0 <= o and o + 8 <= len(data) and struct.unpack_from('>I', data, o + 4)[0] == toc:
            ents.add(val - base)
    return ents


def align(refs, tgts, threshold=0.4):
    """Order-preserving best pairing of reference and target bodies; returns (ri, ti, score)."""
    n, m = len(refs), len(tgts)
    sim = [[0.0] * m for _ in range(n)]
    for a in range(n):
        for b in range(m):
            la, lb = len(refs[a]), len(tgts[b])
            if min(la, lb) * 3 < max(la, lb):
                continue
            sim[a][b] = difflib.SequenceMatcher(None, refs[a], tgts[b], autojunk=False).ratio()
    best = [[0.0] * (m + 1) for _ in range(n + 1)]
    for a in range(n - 1, -1, -1):
        for b in range(m - 1, -1, -1):
            take = sim[a][b] + best[a + 1][b + 1] if sim[a][b] >= threshold else -1
            best[a][b] = max(take, best[a + 1][b], best[a][b + 1])
    pairs, a, b = [], 0, 0
    while a < n and b < m:
        if sim[a][b] >= threshold and best[a][b] == sim[a][b] + best[a + 1][b + 1]:
            pairs.append((a, b, sim[a][b]))
            a, b = a + 1, b + 1
        elif best[a][b] == best[a + 1][b]:
            a += 1
        else:
            b += 1
    return pairs


def main():
    tgt_path, ref_path, out_path = sys.argv[1:4]
    tblob, tp = load(tgt_path)
    _, rp = load(ref_path)
    rc = bytes(rp.sections[0].data)
    tc, td = bytes(tp.sections[0].data), bytes(tp.sections[1].data)
    cbase, dbase = tp.sections[0].base, tp.sections[1].base
    toc = struct.unpack_from('>I', td, tp.main_tvector - dbase + 4)[0]
    code_ptrs = [(tp.sections[s].base + o, struct.unpack_from('>I', tp.sections[s].data, o)[0])
                 for s, o, d in tp.reloc_sites if d == cbase]

    rfuncs = analyze.find_functions(rc)
    if not rfuncs:
        raise SystemExit('reference has no traceback tables')
    names = [f['name'].lstrip('.') for f in rfuncs]
    nr, nt = normalize(rc), normalize(tc)
    bodies = [nr[f['start']:f['body_end']] for f in rfuncs]
    sizes = [len(b) for b in bodies]
    N = len(rfuncs)
    place = [None] * N
    claimed = bytearray(len(tc) // 4 + 1)

    def free(t, size):
        return not any(claimed[t // 4:(t + size) // 4])

    def put(i, t):
        place[i] = t
        claimed[t // 4:(t + sizes[i]) // 4] = b'\1' * (sizes[i] // 4)

    # 1: unique hits, longest first
    hits = [find_all(nt, b) for b in bodies]
    for i in sorted((i for i in range(N) if len(hits[i]) == 1), key=lambda i: -sizes[i]):
        if free(hits[i][0], sizes[i]):
            put(i, hits[i][0])

    # 2: ambiguous ones chained off placed neighbours
    progress = True
    while progress:
        progress = False
        for i in range(N):
            if place[i] is not None or not hits[i]:
                continue
            want = set()
            if i > 0 and place[i - 1] is not None:
                want.add(place[i - 1] + sizes[i - 1])
            if i + 1 < N and place[i + 1] is not None:
                want.add(place[i + 1] - sizes[i])
            ok = [t for t in hits[i] if t in want and free(t, sizes[i])]
            if len(ok) == 1:
                put(i, ok[0])
                progress = True

    # 3: the runtime library (identical code, just moved)
    last_tb = rfuncs[-1]['tb_end']
    rt_hits = find_all(nt, nr[last_tb:last_tb + 256])
    if len(rt_hits) != 1:
        raise SystemExit('cannot locate the runtime library in the target')
    runtime_start = rt_hits[0]

    # 4: unclaimed stretches of game code
    ents = sorted(e for e in call_entries(tc, cbase, td, dbase, toc, code_ptrs) if e < runtime_start)
    first_game = min(t for t in place if t is not None)
    tl = loose(tc)
    rows = {place[i]: (names[i], '') for i in range(N) if place[i] is not None}
    by_target = sorted((place[i], i) for i in range(N) if place[i] is not None)
    stretches = []
    for (ta, ia), (tb, ib) in zip(by_target, by_target[1:] + [(runtime_start, None)]):
        lo = ta + sizes[ia]
        if lo < tb:
            stretches.append((lo, tb, ia, ib))
    used = set()
    for lo, hi, ia, ib in stretches:
        cand = []
        k = ia + 1
        while k < N and place[k] is None:
            cand.append(k)
            k += 1
        if ib is not None:
            k = ib - 1
            back = []
            while k >= 0 and place[k] is None:
                back.append(k)
                k -= 1
            cand += [k for k in reversed(back) if k not in cand]
        cand = [k for k in sorted(cand) if k not in used]
        cuts = sorted({lo} | {e for e in ents if lo < e < hi})
        chunks = list(zip(cuts, cuts[1:] + [hi]))
        pairs = align([loose(rc[rfuncs[k]['start']:rfuncs[k]['body_end']]) for k in cand],
                      [tl[a // 4:b // 4] for a, b in chunks])
        for ci, ti, score in pairs:
            used.add(cand[ci])
            rows[chunks[ti][0]] = (names[cand[ci]], f'changed {score:.2f}')
        # a single leftover function in a stretch with leftover code was rewritten in place:
        # give it the most similar leftover chunk
        left_c = [k for k in cand if k not in used]
        left_t = [ti for ti in range(len(chunks)) if chunks[ti][0] not in rows]
        if len(left_c) == 1 and left_t:
            k = left_c[0]
            ref = loose(rc[rfuncs[k]['start']:rfuncs[k]['body_end']])
            scored = [(difflib.SequenceMatcher(None, ref, tl[chunks[ti][0] // 4:chunks[ti][1] // 4],
                                               autojunk=False).ratio(), ti) for ti in left_t]
            score, ti = max(scored)
            used.add(k)
            rows[chunks[ti][0]] = (names[k], f'rewritten {score:.2f}')
        for a, _ in chunks:
            rows.setdefault(a, (None, 'new'))
    removed = [names[k] for k in range(N) if place[k] is None and k not in used]

    # any other call target inside game code starts a function of its own
    for e in ents:
        if e >= first_game and e not in rows:
            rows[e] = (None, 'new')
    starts = sorted(rows)
    out = []
    for n, s in enumerate(starts):
        e = starts[n + 1] if n + 1 < len(starts) else runtime_start
        name, status = rows[s]
        out.append((cbase + s, cbase + e, name or f'sub_{cbase + s:08X}', status))

    with open(out_path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(f'# Prime Target function symbols for {os.path.basename(tgt_path)}\n')
        fh.write(f'# made by tools/make_symbols.py from {os.path.basename(ref_path)}\n')
        fh.write(f'# sha1 {hashlib.sha1(tblob).hexdigest()}\n')
        fh.write(f'# runtime {cbase + runtime_start:08x}\n')
        fh.write('# start\tend\tname\tdifference from reference\n')
        for s, e, n, st in out:
            fh.write(f'{s:08x}\t{e:08x}\t{n}\t{st}\n')
        for n in removed:
            fh.write(f'# removed\t{n}\n')
    print(f'{len(out)} functions: {sum(1 for r in out if not r[3])} identical, '
          f'{sum(1 for r in out if r[3].startswith("changed"))} changed, '
          f'{sum(1 for r in out if r[3] == "new")} new; {len(removed)} removed; '
          f'game code {cbase + first_game:#x}-{cbase + runtime_start:#x}')
    for s, e, n, st in out:
        if st:
            print(f'  {st:13s} {n:44s} {s:#x} ({e - s} bytes)')
    if removed:
        print('  removed:', ', '.join(removed))


if __name__ == '__main__':
    main()
