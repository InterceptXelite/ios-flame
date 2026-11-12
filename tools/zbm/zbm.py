"""decmpfs LZBITMAP (types 13 inline, 14 resource fork) via libzbm.so."""
import ctypes, os, struct

_lib = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'libzbm.so'))
_lib.zbm_decompress.restype = ctypes.c_long
_lib.zbm_decompress.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
BLOCK = 0x10000


def _chunk(src, want):
    if src[:4] != b'ZBM\x09':
        if len(src) == want + 1 and src[0] in (0x06, 0xff):   # stored chunk marker
            return src[1:]
        raise ValueError('bad LZBITMAP chunk (len %d, want %d, head %r)' % (len(src), want, src[:4]))
    out = ctypes.create_string_buffer(want + 64)
    n = _lib.zbm_decompress(out, want + 64, src, len(src))
    if n != want:
        raise ValueError('LZBITMAP chunk decoded to %d, expected %d' % (n, want))
    return out.raw[:n]


def decmpfs_decode(decmpfs, rsrc):
    magic, typ, size = struct.unpack('<4sIQ', decmpfs[:16])
    if magic != b'fpmc':
        raise ValueError('not decmpfs')
    if typ == 13:                                   # inline
        return _chunk(decmpfs[16:], size) if size else b''
    if typ != 14:
        raise ValueError('decmpfs type %d not LZBITMAP' % typ)
    nchunks = (size + BLOCK - 1) // BLOCK
    first = struct.unpack('<I', rsrc[:4])[0]
    offs = struct.unpack('<%dI' % (first // 4), rsrc[:first])
    if len(offs) != nchunks + 1:
        raise ValueError('chunk table %d entries, expected %d' % (len(offs), nchunks + 1))
    parts = []
    for i in range(nchunks):
        want = min(BLOCK, size - i * BLOCK)
        parts.append(_chunk(rsrc[offs[i]:offs[i + 1]], want))
    return b''.join(parts)
