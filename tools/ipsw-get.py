#!/usr/bin/env python3
"""Fetch single members of a remote IPSW (zip) with HTTP range requests.

  ipsw-get.py URL list                 # list members with sizes
  ipsw-get.py URL get MEMBER OUTFILE   # download + inflate one member

Python stdlib only. Handles Zip64 (IPSWs are > 4 GiB). Resumes a partial
download of a STORED member (the big DMGs are stored, not deflated).
"""
import os, struct, sys, urllib.request, zlib

CHUNK = 8 << 20


def rng(url, start, end):
    req = urllib.request.Request(url, headers={'Range': 'bytes=%d-%d' % (start, end - 1), 'User-Agent': 'ipsw-get'})
    with urllib.request.urlopen(req, timeout=120) as r:
        data = r.read()
    if len(data) != end - start:
        raise IOError('short read %d of %d' % (len(data), end - start))
    return data


def size_of(url):
    req = urllib.request.Request(url, method='HEAD', headers={'User-Agent': 'ipsw-get'})
    with urllib.request.urlopen(req, timeout=60) as r:
        return int(r.headers['Content-Length'])


def central_directory(url):
    total = size_of(url)
    tail = rng(url, max(0, total - 65536 - 22), total)
    i = tail.rfind(b'PK\x05\x06')
    if i < 0:
        sys.exit('no end of central directory')
    n, cd_size, cd_off = struct.unpack('<HII', tail[i + 10:i + 20])
    j = tail.rfind(b'PK\x06\x06')   # zip64 EOCD
    if j >= 0:
        n, cd_size, cd_off = struct.unpack('<QQQ', tail[j + 32:j + 56])
    cd = rng(url, cd_off, cd_off + cd_size)
    out, p = {}, 0
    while p < len(cd) and cd[p:p + 4] == b'PK\x01\x02':
        (method, crc, csize, usize, nlen, xlen, clen) = struct.unpack('<H4xIIIHHH', cd[p + 10:p + 34])
        lho = struct.unpack('<I', cd[p + 42:p + 46])[0]
        name = cd[p + 46:p + 46 + nlen].decode()
        extra = cd[p + 46 + nlen:p + 46 + nlen + xlen]
        q = 0
        while q + 4 <= len(extra):
            hid, hl = struct.unpack('<HH', extra[q:q + 4])
            if hid == 1:
                vals, k = extra[q + 4:q + 4 + hl], 0
                if usize == 0xffffffff:
                    usize = struct.unpack('<Q', vals[k:k + 8])[0]; k += 8
                if csize == 0xffffffff:
                    csize = struct.unpack('<Q', vals[k:k + 8])[0]; k += 8
                if lho == 0xffffffff:
                    lho = struct.unpack('<Q', vals[k:k + 8])[0]; k += 8
            q += 4 + hl
        out[name] = (method, crc, csize, usize, lho)
        p += 46 + nlen + xlen + clen
    return out


def get(url, members, name, path):
    method, crc, csize, usize, lho = members[name]
    h = rng(url, lho, lho + 30)
    nlen, xlen = struct.unpack('<HH', h[26:30])
    data_off = lho + 30 + nlen + xlen
    have = os.path.getsize(path) if (method == 0 and os.path.exists(path)) else 0
    if have == usize:
        print('already complete:', path)
        return
    mode = 'ab' if have else 'wb'
    dec = zlib.decompressobj(-15) if method == 8 else None
    if method not in (0, 8):
        sys.exit('unsupported compression method %d' % method)
    with open(path, mode) as f:
        pos = have
        while pos < csize:
            end = min(csize, pos + CHUNK)
            for attempt in range(5):
                try:
                    blk = rng(url, data_off + pos, data_off + end)
                    break
                except Exception as e:
                    print('retry', attempt, e, file=sys.stderr)
            else:
                sys.exit('download failed at %d' % pos)
            f.write(dec.decompress(blk) if dec else blk)
            pos = end
            print('\r%s %5.1f%%' % (os.path.basename(path), 100.0 * pos / csize), end='', flush=True)
        if dec:
            f.write(dec.flush())
    print()
    if os.path.getsize(path) != usize:
        sys.exit('size mismatch: %d != %d' % (os.path.getsize(path), usize))
    c = 0
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(CHUNK), b''):
            c = zlib.crc32(b, c)
    if c != crc:
        sys.exit('CRC mismatch for %s' % name)
    print('ok', name, usize, 'bytes, crc verified')


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    url, cmd = sys.argv[1], sys.argv[2]
    m = central_directory(url)
    if cmd == 'list':
        for k, (meth, crc, cs, us, lho) in sorted(m.items(), key=lambda x: -x[1][3]):
            print('%12d  %s%s' % (us, k, '' if meth == 0 else '  (deflate)'))
    elif cmd == 'get':
        get(url, m, sys.argv[3], sys.argv[4])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
