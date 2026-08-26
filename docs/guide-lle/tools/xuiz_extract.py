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

    Each XUIB block declares its own byte length at +0x0E, and that length is
    checked against the distance to the next XUIB magic. With extents pinned
    that way, the mapping is a straight 1:1 in directory order: the Nth .xur
    entry is the Nth XUIB block.

    Do NOT try to locate payloads using the directory's size field. That field
    lags its name by one record -- it holds the size of the *next* member, and
    matches its own block for only 5 of 34 entries. Trusting it produces a
    plausible-looking but wrong off-by-one mapping in which every scene is
    misnamed as its predecessor.

    Returns (located, unmatched) where located is [(name, offset, size)].
    """
    hits = [m.start() for m in re.finditer(b'XUIB', data)]
    scenes = sorted((e for e in entries if e[0].endswith('.xur')),
                    key=lambda e: e[2])
    if len(hits) != len(scenes):
        raise ValueError('%d XUIB blocks for %d .xur entries'
                         % (len(hits), len(scenes)))

    located = []
    for i, (name, _, _) in enumerate(scenes):
        start = hits[i]
        size = struct.unpack_from('>I', data, start + 0x0E)[0]
        limit = hits[i + 1] if i + 1 < len(hits) else start + size
        if start + size != limit:
            raise ValueError('%s: declared size %#x does not reach the next '
                             'XUIB at %#x' % (name, size, limit))
        located.append((name, start, size))
    return located, []


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
        open(os.path.join(outdir, name), 'wb').write(blob)
        print('  %-30s %#-9x %#x' % (name, offset, size))


if __name__ == '__main__':
    main()
