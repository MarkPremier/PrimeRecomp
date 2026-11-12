"""Split the Prime Target BIN/CUE into WAV files for the CD-audio soundtrack.

Writes <out_dir>/trackNN.wav for every AUDIO track and <out_dir>/toc.tsv:
    track  type(data|audio)  start_frame(absolute, incl. 2 s lead-in)  length_frames
plus a final 'leadout' line. Frames are 1/75 s CD sectors (2352 bytes).

Usage: python tools/rip_cd_audio.py <file.cue> <out_dir>
"""
import os
import re
import struct
import sys

SECTOR = 2352
LEAD_IN = 150  # 2-second pregap before track 1


def msf_to_frames(m, s, f):
    return (m * 60 + s) * 75 + f


def parse_cue(path):
    tracks = []
    binfile = None
    for line in open(path, encoding='latin-1'):
        line = line.strip()
        m = re.match(r'FILE "(.+)" BINARY', line)
        if m and binfile is None:
            binfile = m.group(1)
        m = re.match(r'TRACK (\d+) (\S+)', line)
        if m:
            tracks.append({'n': int(m.group(1)), 'type': 'audio' if m.group(2) == 'AUDIO' else 'data'})
        m = re.match(r'INDEX 01 (\d+):(\d+):(\d+)', line)
        if m and tracks and 'start' not in tracks[-1]:
            tracks[-1]['start'] = msf_to_frames(*map(int, m.groups()))
    return os.path.join(os.path.dirname(path), binfile), tracks


def wav_header(nbytes):
    return (b'RIFF' + struct.pack('<I', 36 + nbytes) + b'WAVE' +
            b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 2, 44100, 44100 * 4, 4, 16) +
            b'data' + struct.pack('<I', nbytes))


def main():
    cue, out = sys.argv[1], sys.argv[2]
    binpath, tracks = parse_cue(cue)
    total = os.path.getsize(binpath) // SECTOR
    os.makedirs(out, exist_ok=True)
    with open(binpath, 'rb') as b, open(os.path.join(out, 'toc.tsv'), 'w', newline='\n') as toc:
        for i, t in enumerate(tracks):
            end = tracks[i + 1]['start'] if i + 1 < len(tracks) else total
            length = end - t['start']
            toc.write(f"{t['n']}\t{t['type']}\t{t['start'] + LEAD_IN}\t{length}\n")
            if t['type'] != 'audio':
                continue
            b.seek(t['start'] * SECTOR)
            pcm = b.read(length * SECTOR)  # raw CD audio: 16-bit little-endian stereo
            with open(os.path.join(out, f"track{t['n']:02d}.wav"), 'wb') as w:
                w.write(wav_header(len(pcm)))
                w.write(pcm)
            print(f"track {t['n']:2d}: {length / 75:7.1f} s")
        toc.write(f"leadout\t-\t{total + LEAD_IN}\t0\n")


if __name__ == '__main__':
    main()
