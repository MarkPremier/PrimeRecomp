"""Copy the Mac OS resources the game relies on out of a System 7 disk image.

Prime Target ships no fonts or cursors of its own; it uses the System file
(cursors, patterns, beeps, Chicago/Geneva) and the suitcases in
System Folder:Fonts. This writes their resource forks to <out_dir>:

    System.rsrc
    Fonts/<name>.rsrc
    files.tsv        load order: host path, Mac name as hex

Usage: python tools/extract_system.py <macos7.hfv> <out_dir>
"""
import os
import sys

import machfs

from extract_game import host_name, load_volume


def main(src, dst):
    vol = load_volume(src)
    sysfolder = vol['System Folder']
    rows = []
    os.makedirs(os.path.join(dst, 'Fonts'), exist_ok=True)

    def save(rel, mac_name, rsrc):
        with open(os.path.join(dst, rel), 'wb') as fh:
            fh.write(rsrc)
        rows.append((rel.replace(os.sep, '/'), mac_name.encode('mac_roman').hex()))

    save('System.rsrc', 'System', sysfolder['System'].rsrc)
    fonts = sysfolder.get('Fonts')
    if isinstance(fonts, machfs.Folder):
        for name, item in fonts.items():
            if isinstance(item, machfs.File) and item.rsrc:
                save(os.path.join('Fonts', host_name(name) + '.rsrc'), name, item.rsrc)
    with open(os.path.join(dst, 'files.tsv'), 'w', encoding='utf-8', newline='\n') as fh:
        for rel, hexname in rows:
            fh.write(rel + '\t' + hexname + '\n')
    print(f'copied {len(rows)} resource forks from {src} -> {dst}')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
