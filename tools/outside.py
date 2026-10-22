"""Where the samples outside the executable come from: the first frames of
the executable above them. tools/outside.py <samples.bin> (run in build/Release)."""
import bisect
import collections
import re
import struct
import sys

syms = []
for line in open('ghostrecon2.map', errors='ignore'):
    m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s', line)
    if m:
        syms.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
syms.sort()
addrs = [a for a, _ in syms]


def name(rva):
    if rva == 0xFFFFFFFF:
        return 'OUT'
    i = bisect.bisect_right(addrs, rva) - 1
    return syms[i][1]


data = open(sys.argv[1] if len(sys.argv) > 1 else 'samples.bin', 'rb').read()
c = collections.Counter()
for k in range(len(data) // 32):
    rec = [x for x in struct.unpack_from('<8I', data, k * 32) if x]
    if rec and rec[0] == 0xFFFFFFFF:
        c[' > '.join(name(r) for r in rec[1:5])] += 1
for f, n in c.most_common(15):
    print(n, f)
