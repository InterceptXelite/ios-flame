"""Verify a Mach-O's code-signature page hashes (checks decompression output)."""
import hashlib, struct


def verify(d):
    if d[:4] != b'\xcf\xfa\xed\xfe':
        return None
    ncmds = struct.unpack('<I', d[16:20])[0]
    off = 32
    cs = None
    for _ in range(ncmds):
        cmd, sz = struct.unpack('<II', d[off:off + 8])
        if cmd == 0x1d:
            cs = struct.unpack('<II', d[off + 8:off + 16])
        off += sz
    if not cs:
        return None
    blob = d[cs[0]:cs[0] + cs[1]]
    count = struct.unpack('>I', blob[8:12])[0]
    best = None
    for i in range(count):
        typ, o = struct.unpack('>II', blob[12 + 8 * i:20 + 8 * i])
        if typ == 0 or 0x1000 <= typ < 0x1005:
            cd = blob[o:]
            if cd[:4] != b'\xfa\xde\x0c\x02':
                continue
            hash_off, ident_off, nspecial, ncode, code_limit, hsize, htype, plat, pshift = \
                struct.unpack('>IIIIIBBBB', cd[16:40])
            if htype == 2 and (best is None):
                best = (cd, hash_off, ncode, code_limit, hsize, pshift)
            elif best is None:
                best = (cd, hash_off, ncode, code_limit, hsize, pshift)
    if not best:
        return None
    cd, hash_off, ncode, code_limit, hsize, pshift = best
    page = 1 << pshift if pshift else code_limit
    algo = hashlib.sha256 if hsize == 32 else hashlib.sha1
    for i in range(ncode):
        chunk = d[i * page:min((i + 1) * page, code_limit)]
        if algo(chunk).digest()[:hsize] != cd[hash_off + i * hsize:hash_off + (i + 1) * hsize]:
            return False
    return True
