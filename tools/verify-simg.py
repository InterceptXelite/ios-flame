#!/usr/bin/env python3
"""Verify a split Android sparse HFS image against its sources, cheaply.

  verify-simg.py PREFIX LAYOUT.tsv OS.dmg SYSOS.dmg APPOS.dmg [--sample N]

 1. reads the parts PREFIX.000, .001, ... as one stream and indexes the chunks
 2. writes the metadata area (everything before the first data block) and the
    last block into a sparse raw file and runs fsck.hfsplus -fn on it
 3. reads a sample of files straight out of the sparse stream (all of the
    largest ones + random ones + every tweak file) and compares SHA-256 with
    a fresh 7-Zip extraction of the same files from Apple's images
"""
import glob, hashlib, io, os, plistlib, random, shutil, struct, subprocess, sys, tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'zbm'))
import zbm  # noqa: E402

BLOCK = 4096


class Stream:
    def __init__(self, prefix):
        self.parts = sorted(glob.glob(prefix + '.[0-9][0-9][0-9]'))
        self.sizes = [os.path.getsize(p) for p in self.parts]
        self.fds = [open(p, 'rb') for p in self.parts]

    def read(self, off, n):
        out = bytearray()
        for i, sz in enumerate(self.sizes):
            if off >= sz:
                off -= sz
                continue
            self.fds[i].seek(off)
            b = self.fds[i].read(min(n - len(out), sz - off))
            out += b
            off = 0
            if len(out) == n:
                break
        return bytes(out)


def index(st):
    hdr = st.read(0, 28)
    magic, maj, mnr, fh, ch, bs, total, nch, _ = struct.unpack('<IHHHHIIII', hdr)
    assert magic == 0xED26FF3A and bs == BLOCK, 'not a sparse image'
    raw, pos, blk = [], fh, 0
    for _ in range(nch):
        t, _, n, tot = struct.unpack('<HHII', st.read(pos, 12))
        if t == 0xCAC1:
            raw.append((blk, n, pos + 12))
        elif t != 0xCAC3:
            raise SystemExit('unexpected chunk type %x' % t)
        blk += n
        pos += tot
    assert blk == total, (blk, total)
    assert pos == sum(st.sizes), (pos, sum(st.sizes))
    print('sparse: %d chunks, %d blocks (%d MiB volume), stream %d MiB, consistent'
          % (nch, total, total * BLOCK >> 20, pos >> 20))
    return raw, total


def read_blocks(st, raw, start, nblocks):
    out = bytearray()
    b = start
    end = start + nblocks
    import bisect
    starts = [r[0] for r in raw]
    while b < end:
        i = bisect.bisect_right(starts, b) - 1
        cb, cn, coff = raw[i]
        if not (cb <= b < cb + cn):
            out += b'\0' * BLOCK      # DONT_CARE hole
            b += 1
            continue
        take = min(end, cb + cn) - b
        out += st.read(coff + (b - cb) * BLOCK, take * BLOCK)
        b += take
    return bytes(out)


def main():
    prefix, layout, osd, sysd, appd = sys.argv[1:6]
    nsample = int(sys.argv[sys.argv.index('--sample') + 1]) if '--sample' in sys.argv else 1500
    st = Stream(prefix)
    raw, total = index(st)
    rows = []
    for line in open(layout, errors='surrogateescape'):
        a, b, c, label = line.rstrip('\n').split('\t', 3)
        rows.append((int(a), int(b), int(c), label))
    data_start = min(r[0] for r in rows)

    # 2) structure
    tmp = tempfile.mkdtemp(dir=os.path.dirname(os.path.abspath(prefix)))
    meta = os.path.join(tmp, 'meta.raw')
    with open(meta, 'wb') as f:
        f.truncate(total * BLOCK)
        f.seek(0); f.write(read_blocks(st, raw, 0, data_start))
        f.seek((total - 1) * BLOCK); f.write(read_blocks(st, raw, total - 1, 1))
    r = subprocess.run(['fsck.hfsplus', '-fn', meta], capture_output=True, text=True)
    print('fsck (metadata read back from the parts):', r.stdout.strip().splitlines()[-1])
    ok_struct = 'appears to be OK' in r.stdout
    os.remove(meta)

    # 3) contents
    random.seed(20260112)
    labeled = [r for r in rows if r[3]]
    sample = sorted(labeled, key=lambda r: -r[2])[:12]               # the largest files
    sample += random.sample(labeled, min(nsample, len(labeled)))
    tweaks = [r for r in rows if not r[3]]
    pre = {'private/preboot/Cryptexes/OS/': sysd, 'private/preboot/Cryptexes/App/': appd}

    def source(label):
        for p, arch in pre.items():
            if label.startswith(p):
                return arch, label[len(p):]
        return osd, label
    by_arch = {}
    for row in sample:
        arch, path = source(row[3])
        by_arch.setdefault(arch, set()).add(path)
    bad = checked = 0
    for arch, paths in by_arch.items():
        d = os.path.join(tmp, 'x')
        shutil.rmtree(d, ignore_errors=True)
        lst = os.path.join(tmp, 'list.txt')
        open(lst, 'w', errors='surrogateescape').write('\n'.join(sorted(paths)) + '\n')
        subprocess.run(['7z', 'x', '-y', '-sns', '-scsUTF-8', '-o' + d, arch, '@' + lst], capture_output=True)
        for row in sample:
            a2, path = source(row[3])
            if a2 != arch:
                continue
            fp = os.path.join(d, path)
            dec = fp + ':com.apple.decmpfs'
            if os.path.exists(dec) and os.path.getsize(fp) == 0 and row[2]:
                rs = fp + ':com.apple.ResourceFork'
                want = zbm.decmpfs_decode(open(dec, 'rb').read(), open(rs, 'rb').read() if os.path.exists(rs) else b'')
            else:
                want = open(fp, 'rb').read()
            got = read_blocks(st, raw, row[0], row[1])[:row[2]]
            checked += 1
            if hashlib.sha256(got).digest() != hashlib.sha256(want).digest():
                bad += 1
                if bad <= 5:
                    print('MISMATCH', row[3])
    print('content: %d files compared byte-for-byte (incl. the 12 largest), %d mismatches' % (checked, bad))

    # tweak files: must parse, and launchd.plist must carry the edits
    found = {}
    for row in tweaks:
        b = read_blocks(st, raw, row[0], row[1])[:row[2]]
        try:
            pl = plistlib.loads(b)
        except Exception:
            continue
        if isinstance(pl, dict) and 'LaunchDaemons' in pl:
            j = pl['LaunchDaemons']
            found['launchd'] = (sum('_PanicOnCrasX' in v for v in j.values()),
                                sum('_PanicOnCrash' in v for v in j.values()),
                                sum(v.get('Label') == 'com.apple.CommCenter' for v in j.values()))
        elif isinstance(pl, dict) and 'SetupDone' in pl:
            found['purplebuddy'] = pl['SetupDone']
        elif isinstance(pl, dict) and 'KeyBagKeys' in pl:
            found['keybag'] = True
        elif isinstance(pl, dict) and pl.get('StartRecorderServer') is False:
            found['mediatoolbox'] = found.get('mediatoolbox', 0) + 1
    print('tweak files read back:', found)
    shutil.rmtree(tmp, ignore_errors=True)
    good = ok_struct and bad == 0 and found.get('launchd', (0, 1, 1))[1:] == (0, 0)
    print('RESULT:', 'PASS' if good else 'FAIL')
    sys.exit(0 if good else 1)


if __name__ == '__main__':
    main()
