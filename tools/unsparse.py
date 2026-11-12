#!/usr/bin/env python3
"""unsparse.py OUT.img PART... : join Android sparse parts and expand (verification helper)."""
import struct, sys
data = b''.join(open(p, 'rb').read() for p in sys.argv[2:])
magic, maj, mnr, fh, ch, bs, tot, nch, _ = struct.unpack('<IHHHHIIII', data[:28])
assert magic == 0xED26FF3A, hex(magic)
pos, blk = fh, 0
with open(sys.argv[1], 'wb') as f:
    f.truncate(tot * bs)
    for _ in range(nch):
        t, _, n, total = struct.unpack('<HHII', data[pos:pos + 12])
        if t == 0xCAC1:
            f.seek(blk * bs); f.write(data[pos + 12:pos + total])
        elif t not in (0xCAC3,):
            raise SystemExit('chunk type %x' % t)
        blk += n; pos += total
assert blk == tot and pos == len(data), (blk, tot, pos, len(data))
print('ok: %d chunks, %d blocks' % (nch, tot))
