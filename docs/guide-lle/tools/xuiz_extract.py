#!/usr/bin/env python3
"""Parse the XUIZ resource container embedded in xam/hud and extract its members.

XUIZ layout (big-endian):
    +0x00  'XUIZ'
    +0x04  version (3)
    +0x08  total container size
    +0x10  directory size
    +0x14  entry count
    +0x1E  directory: repeated [u8 namelen][name][u32 size][u32 offset]

The directory offsets are NOT usable as section-relative file offsets. The
member payloads sit after the directory; .xur members are located instead by
scanning for their 'XUIB' magic. The Nth XUIB block corresponds to the
(N-1)th .xur directory entry -- one unrelated XUIB block precedes the list --
and each block's byte length matches its entry's declared size.

Usage: xuiz_extract.py <hud-or-xam.pe> <section-offset> [outdir]
"""
import os
import re
import struct
import sys


def parse_directory(data, base):
    """Walk the XUIZ directory. Returns [(name, size, offset), ...]."""
    if data[base:base + 4] != b'XUIZ':
        raise ValueError('no XUIZ magic at %#x' % base)
    total = struct.unpack_from('>I', data, base + 8)[0]
    count = struct.unpack_from('>I', data, base + 0x14)[0]
    entries = []
    p = base + 0x1E
    while len(entries) < count:
        namelen = data[p]
        name = data[p + 1:p + 1 + namelen]
        if namelen == 0 or namelen > 64:
            break
        if not all(32 <= c < 127 for c in name):
            break
        size, offset = struct.unpack_from('>II', data, p + 1 + namelen)
        # The final entry overruns into the next section; its "size" field
        # reads back as the following section's magic. Stop before it.
        if size > total:
            break
        entries.append((name.decode('latin1'), size, offset))
        p += 1 + namelen + 8
    return entries


def locate_scenes(data, entries):
    """Map .xur entries onto their XUIB payloads.

    The payloads form one contiguous run of XUIB blocks. The run is offset by
    one relative to the .xur directory listing: block N+1 carries entry N, and
    block 0 is an extra XUIB (size 0x65d in hud 17489) matching no directory
    entry. The shift is verified against declared sizes rather than assumed --
    every block length must equal its entry's declared size.

    Returns (located, unmatched) where located is [(name, offset, size)].
    """
    hits = [m.start() for m in re.finditer(b'XUIB', data)]
    scenes = sorted((e for e in entries if e[0].endswith('.xur')),
                    key=lambda e: e[2])

    def score(shift):
        n = 0
        for i in range(len(scenes)):
            if i + shift + 1 >= len(hits):
                break
            if hits[i + shift + 1] - hits[i + shift] != scenes[i][1]:
                return -1
            n += 1
        return n

    shift = max(range(4), key=score)
    if score(shift) <= 0:
        raise ValueError('could not align XUIB blocks to directory entries')

    located, unmatched = [], []
    for i, (name, size, _) in enumerate(scenes):
        if i + shift < len(hits):
            located.append((name, hits[i + shift], size))
        else:
            unmatched.append(name)
    return located, unmatched


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    path, base = sys.argv[1], int(sys.argv[2], 0)
    outdir = sys.argv[3] if len(sys.argv) > 3 else 'xuiz-out'
    data = open(path, 'rb').read()

    entries = parse_directory(data, base)
    scenes, unmatched = locate_scenes(data, entries)
    print('%d entries, %d scenes located' % (len(entries), len(scenes)))
    for name in unmatched:
        print('  UNRESOLVED %s: no XUIB payload in the contiguous run' % name)

    os.makedirs(outdir, exist_ok=True)
    for name, offset, size in scenes:
        blob = data[offset:offset + size]
        declared = struct.unpack_from('>I', blob, 0x0E)[0]
        if declared != size:
            print('  WARNING %s: header size %#x != directory size %#x'
                  % (name, declared, size))
        open(os.path.join(outdir, name), 'wb').write(blob)
        print('  %-30s %#-9x %#x' % (name, offset, size))


if __name__ == '__main__':
    main()
