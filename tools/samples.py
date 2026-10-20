import struct, bisect, sys, collections, re
d = sys.argv[1] if len(sys.argv) > 1 else '.'
syms = []
for line in open(d + '/ghostrecon2.map', errors='ignore'):
    m = re.match(r'\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(\S+)\s+([0-9a-f]{16})\s', line)
    if m:
        syms.append((int(m.group(2), 16) - 0x140000000, m.group(1)))
syms.sort()
addrs = [a for a, _ in syms]
def name(rva):
    if rva == 0xFFFFFFFF: return '<outside exe>'
    i = bisect.bisect_right(addrs, rva) - 1
    return syms[i][1] if i >= 0 else '?'
data = open(d + '/samples.bin', 'rb').read()
n = len(data) // 32
selfc = collections.Counter(); incl = collections.Counter()
for k in range(n):
    rec = struct.unpack_from('<8I', data, k * 32)
    frames = [name(r) for r in rec if r]
    if not frames: continue
    selfc[frames[0]] += 1
    for f in set(frames): incl[f] += 1
print(n, 'samples')
print('-- self (innermost)')
for f, c in selfc.most_common(25): print('%6.1f%%  %s' % (100.0 * c / n, f))
print('-- inclusive (on stack, top 8)')
for f, c in incl.most_common(30): print('%6.1f%%  %s' % (100.0 * c / n, f))
