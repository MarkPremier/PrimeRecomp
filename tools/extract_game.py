"""Extract Prime Target from its CD (BIN/CUE or ISO) or an installed HFS disk image.

Each Mac file becomes:
    <name>        data fork (always written, may be empty)
    <name>.rsrc   resource fork (only when non-empty)
and every folder gets a finder.json mapping file name -> {type, creator, flags}.

Usage: python tools/extract_game.py <PrimeTarget.cue | .iso | .hfv> <out_dir>

From the CD only the 'Prime Target' and 'Movies' folders are copied (the rest
is demos and the installer).
"""
import json
import os
import struct
import sys

import machfs


def read_data_track(path):
    """Raw bytes of the image, or of the CUE's first (data) track as 2048-byte sectors."""
    if not path.lower().endswith('.cue'):
        return open(path, 'rb').read()
    sys.path.insert(0, os.path.dirname(__file__))
    from rip_cd_audio import parse_cue
    binfile, tracks = parse_cue(path)
    mode = next((l.split()[2] for l in open(path, encoding='latin-1') if l.strip().startswith('TRACK 01')), '')
    raw = open(binfile, 'rb').read()
    end = tracks[1]['start'] * 2352 if len(tracks) > 1 else len(raw)
    if mode == 'MODE1/2048':
        return raw[:end]
    if mode != 'MODE1/2352':
        raise SystemExit(f'unsupported data track mode {mode!r}')
    return b''.join(raw[o + 16:o + 16 + 2048] for o in range(0, end, 2352))


def load_volume(path):
    data = read_data_track(path)
    if data[:2] == b'ER':  # Apple partition map (CD images): pick the HFS partition
        blk = struct.unpack('>H', data[2:4])[0]
        i = 1
        while data[i * blk:i * blk + 2] == b'PM':
            off = i * blk
            start, size = struct.unpack('>II', data[off + 8:off + 16])
            ptype = data[off + 48:off + 80].split(b'\0')[0]
            if ptype == b'Apple_HFS':
                data = data[start * 512:(start + size) * 512]
                break
            i += 1
    vol = machfs.Volume()
    vol.read(data)
    return vol


def host_name(mac_name):
    # HFS names may contain characters Windows rejects.
    return ''.join('_' if c in '<>:"/\\|?*' or ord(c) < 32 else c for c in mac_name).rstrip(' .')


def extract(folder, out):
    os.makedirs(out, exist_ok=True)
    finder = {}
    for name, item in folder.items():
        hname = host_name(name)
        if isinstance(item, machfs.Folder):
            extract(item, os.path.join(out, hname))
            continue
        with open(os.path.join(out, hname), 'wb') as fh:
            fh.write(item.data)
        if item.rsrc:
            with open(os.path.join(out, hname + '.rsrc'), 'wb') as fh:
                fh.write(item.rsrc)
        finder[hname] = {
            'mac_name': name,
            'type': item.type.decode('mac_roman'),
            'creator': item.creator.decode('mac_roman'),
            'flags': item.flags,
        }
    with open(os.path.join(out, 'finder.json'), 'w', encoding='utf-8') as fh:
        json.dump(finder, fh, indent=1, ensure_ascii=False)
    # machine-readable twin for the runtime: host name, then raw Mac bytes as hex
    with open(os.path.join(out, 'finder.tsv'), 'w', encoding='utf-8', newline='\n') as fh:
        for hname, f in finder.items():
            fh.write('\t'.join([hname, f['mac_name'].encode('mac_roman').hex(),
                                f['type'].encode('mac_roman').hex(), f['creator'].encode('mac_roman').hex(),
                                f'{f["flags"]:04x}']) + '\n')
    # folders: record Mac names too, since host names may have been sanitised
    with open(os.path.join(out, 'folders.tsv'), 'w', encoding='utf-8', newline='\n') as fh:
        for name, item in folder.items():
            if isinstance(item, machfs.Folder):
                fh.write(host_name(name) + '\t' + name.encode('mac_roman').hex() + '\n')


def extract_volume(src, dst):
    vol = load_volume(src)
    if 'Prime Target Installer' in vol:   # the CD: keep just the game
        keep = {k: vol[k] for k in ('Prime Target', 'Movies')}
        vol = machfs.Folder()
        for k, v in keep.items():
            vol[k] = v
    extract(vol, dst)


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    extract_volume(src, dst)
    print('extracted', src, '->', dst)
