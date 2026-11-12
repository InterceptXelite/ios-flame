#!/usr/bin/env python3
"""Write an HFSX (case-sensitive HFS+) volume image from an in-memory tree,
without mounting anything (no hfsplus kernel module, no root needed).

Used by tools/flame-iosroot.py. Library API:

    root = Dir(uid=0, gid=0, mode=0o755, mtime=...)
    root.add('etc', Dir(...))
    root.add('hello', File(size, opener, uid, gid, mode, mtime))
    root.add('link', Symlink('target', ...))
    build(root, 'out.img', volume_bytes, name='iOSRoot')

Layout (4 KiB blocks): [0] volume header, allocation bitmap, extents B-tree
(header only), catalog B-tree (binary key compare, HFSX), then file data,
contiguous: every fork is one extent. Last block holds the alternate volume
header. Free space is left as holes (the output is a sparse file).
Not journaled, cleanly unmounted. No resource forks, no xattrs, no hard links
(callers duplicate hard-linked files).

Self-test: hfsbuild.py --selftest DIR OUT.img  (then fsck.hfsplus -fn OUT.img)
"""
import os, stat, struct, sys, time, unicodedata

BLOCK = 4096
CAT_NODE = 8192
EXT_NODE = 4096
HFS_EPOCH = 2082844800      # 1904-01-01 -> 1970-01-01

# CNIDs
ROOT_PARENT, ROOT_FOLDER = 1, 2
EXTENTS_ID, CATALOG_ID, BADBLOCK_ID, ALLOC_ID, STARTUP_ID, ATTR_ID = 3, 4, 5, 6, 7, 8
FIRST_USER = 16

# catalog record types
FOLDER_REC, FILE_REC, FOLDER_THREAD, FILE_THREAD = 1, 2, 3, 4
HAS_THREAD = 0x0002
HAS_FOLDER_COUNT = 0x0010


def hfs_time(t):
    return max(0, min(0xffffffff, int(t) + HFS_EPOCH))


class Node:
    def __init__(self, uid=0, gid=0, mode=0o755, mtime=None, ctime=None):
        now = time.time()
        self.uid, self.gid, self.mode = uid, gid, mode
        self.mtime = mtime if mtime is not None else now
        self.ctime = ctime if ctime is not None else self.mtime
        self.cnid = None


class Dir(Node):
    def __init__(self, **kw):
        kw.setdefault('mode', 0o755)
        super().__init__(**kw)
        self.children = {}

    def add(self, name, node):
        self.children[name] = node
        return node

    def get(self, path, create=None):
        """Walk 'a/b/c'. With create=Dir-factory, make missing directories."""
        d = self
        for part in [p for p in path.split('/') if p]:
            n = d.children.get(part)
            if n is None:
                if create is None:
                    return None
                n = d.add(part, create())
            d = n
        return d


class File(Node):
    """size bytes; opener() returns an object with read(n) (fresh each call)."""
    def __init__(self, size, opener, **kw):
        kw.setdefault('mode', 0o644)
        super().__init__(**kw)
        self.size, self.opener = size, opener


def bytes_file(data, **kw):
    import io
    return File(len(data), lambda d=data: io.BytesIO(d), **kw)


class Symlink(Node):
    def __init__(self, target, **kw):
        kw.setdefault('mode', 0o755)
        super().__init__(**kw)
        self.target = target.encode() if isinstance(target, str) else target
        self.size = len(self.target)


# ---------------------------------------------------------------- names
def hfs_name(name):
    """Name as HFS+ stores it: NFD-decomposed UTF-16BE, '/' shown as ':'."""
    if not name.isascii():
        name = unicodedata.normalize('NFD', name)
    name = name.replace(':', '/')   # on disk ':' <-> '/' are swapped
    u = name.encode('utf-16-be')
    if len(u) > 510:
        raise ValueError('name too long: %r' % name)
    return u


def uni255(u16):
    return struct.pack('>H', len(u16) // 2) + u16


def cat_key(parent, u16):
    body = struct.pack('>I', parent) + uni255(u16)
    return struct.pack('>H', len(body)) + body


def sort_key(parent, u16):
    # HFSX binary compare: parentID, then UTF-16 code units unsigned
    return (parent, struct.unpack('>%dH' % (len(u16) // 2), u16))


def bsd_info(n, ftype):
    return struct.pack('>IIBBHI', n.uid, n.gid, 0, 0, (ftype | (n.mode & 0o7777)) & 0xffff, 0)


def fork(logical, start, blocks, clump=0):
    ext = struct.pack('>II', start, blocks) if blocks else b''
    return struct.pack('>QII', logical, clump, blocks) + ext + b'\0' * (64 - len(ext))


# ---------------------------------------------------------------- B-tree
def node_desc(flink, blink, kind, height, nrec):
    return struct.pack('>IIbBHH', flink, blink, kind, height, nrec, 0)


def pack_node(size, desc, records):
    body = desc + b''.join(records)
    offs, o = [], 14
    for r in records:
        offs.append(o)
        o += len(r)
    offs.append(o)  # free space offset
    tail = b''.join(struct.pack('>H', x) for x in reversed(offs))
    if len(body) + len(tail) > size:
        raise ValueError('node overflow')
    return body + b'\0' * (size - len(body) - len(tail)) + tail


def header_node(size, depth, root, leaf_recs, first_leaf, last_leaf, total, free, max_key,
                key_cmp, attrs, clump):
    hdr = struct.pack('>HIIIIHHII', depth, root, leaf_recs, first_leaf, last_leaf, size, max_key,
                      total, free)
    hdr += struct.pack('>HIBBI', 0, clump, 0, key_cmp, attrs) + b'\0' * 64
    assert len(hdr) == 106
    user = b'\0' * 128
    map_len = size - 14 - 106 - 128 - 8
    used = total - free
    bm = bytearray(map_len)
    if used > map_len * 8:
        raise ValueError('B-tree too big for the header map record (%d nodes)' % used)
    for i in range(used):
        bm[i >> 3] |= 0x80 >> (i & 7)
    return pack_node(size, node_desc(0, 0, 1, 0, 3), [hdr, user, bytes(bm)])


def build_btree(records, node_size, key_cmp, attrs, max_key, min_nodes=1):
    """records: sorted list of (key_bytes, data_bytes). Returns (nodes, info)."""
    usable = node_size - 14
    leaves, cur, cur_len = [], [], 0
    for k, d in records:
        r = k + d
        if len(r) + 2 > usable - 2:
            raise ValueError('record too big')
        if cur and cur_len + len(r) + 2 + 2 > usable:
            leaves.append(cur)
            cur, cur_len = [], 0
        cur.append(r)
        cur_len += len(r) + 2
    if cur:
        leaves.append(cur)

    # node numbers: 0 header, then leaves, then index levels bottom-up
    nodes = {}
    level = []          # (node_no, first_key, records)
    nxt = 1
    for recs in leaves:
        level.append((nxt, recs[0][:2 + struct.unpack('>H', recs[0][:2])[0]], recs))
        nxt += 1
    for i, (no, fk, recs) in enumerate(level):
        fl = level[i + 1][0] if i + 1 < len(level) else 0
        bl = level[i - 1][0] if i > 0 else 0
        nodes[no] = pack_node(node_size, node_desc(fl, bl, -1, 1, len(recs)), recs)
    first_leaf = level[0][0] if level else 0
    last_leaf = level[-1][0] if level else 0
    height = 1
    while len(level) > 1:
        height += 1
        parents, cur, cur_len = [], [], 0
        for no, fk, _ in level:
            r = fk + struct.pack('>I', no)    # variable-length index keys
            if cur and cur_len + len(r) + 2 + 2 > usable:
                parents.append(cur)
                cur, cur_len = [], 0
            cur.append((r, fk))
            cur_len += len(r) + 2
        if cur:
            parents.append(cur)
        new_level = []
        for recs in parents:
            new_level.append((nxt, recs[0][1], [r for r, _ in recs]))
            nxt += 1
        for i, (no, fk, recs) in enumerate(new_level):
            fl = new_level[i + 1][0] if i + 1 < len(new_level) else 0
            bl = new_level[i - 1][0] if i > 0 else 0
            nodes[no] = pack_node(node_size, node_desc(fl, bl, 0, height, len(recs)), recs)
        level = new_level
    root = level[0][0] if level else 0
    used = nxt
    total = max(used, min_nodes)
    depth = height if level else 0
    nodes[0] = header_node(node_size, depth, root, len(records), first_leaf, last_leaf, total,
                           total - used, max_key, key_cmp, attrs, node_size * 16)
    return nodes, total



# ---------------------------------------------------------------- outputs
class RawOut:
    """Plain image (sparse file: unwritten blocks stay holes)."""
    def __init__(self, path, size):
        self.f = open(path, 'wb')
        self.f.truncate(size)

    def write(self, block, data):
        self.f.seek(block * BLOCK)
        self.f.write(data)

    def close(self):
        self.f.close()


class SparseOut:
    """Android sparse image (what fastboot flashes), written as a stream and
    cut into parts of part_bytes: PATH.000, PATH.001, ... (join them in order).
    Blocks must be written in increasing order; gaps become DONT_CARE."""
    RAW, DONT_CARE = 0xCAC1, 0xCAC3

    def __init__(self, path, total_blocks, part_bytes):
        self.path, self.total, self.part_bytes = path, total_blocks, part_bytes
        self.part, self.part_left, self.f = -1, 0, None
        self.parts = []
        self.chunks, self.cur = 0, 0
        self.pending, self.pending_start, self.plen = [], None, 0
        self._emit(b'\0' * 28)               # file header, patched in close()

    def _emit(self, b):
        mv = memoryview(b)
        while len(mv):
            if self.part_left == 0:
                if self.f:
                    self.f.close()
                self.part += 1
                name = '%s.%03d' % (self.path, self.part)
                self.parts.append(name)
                self.f = open(name, 'wb')
                self.part_left = self.part_bytes
            n = min(self.part_left, len(mv))
            self.f.write(mv[:n])
            self.part_left -= n
            mv = mv[n:]

    def _flush(self):
        if self.pending:
            data = b''.join(self.pending)
            nb = len(data) // BLOCK
            self._emit(struct.pack('<HHII', self.RAW, 0, nb, 12 + len(data)))
            self._emit(data)
            self.chunks += 1
            self.pending, self.pending_start, self.plen = [], None, 0

    def write(self, block, data):
        assert len(data) % BLOCK == 0, 'unaligned write'
        if block < self.cur:
            raise ValueError('sparse writes must be in increasing block order (%d < %d)' % (block, self.cur))
        if block > self.cur:
            self._flush()
            self._emit(struct.pack('<HHII', self.DONT_CARE, 0, block - self.cur, 12))
            self.chunks += 1
        self.pending.append(bytes(data))
        self.plen += len(data)
        self.cur = block + len(data) // BLOCK
        if self.plen >= (64 << 20):
            self._flush()

    def close(self):
        self._flush()
        if self.cur < self.total:
            self._emit(struct.pack('<HHII', self.DONT_CARE, 0, self.total - self.cur, 12))
            self.chunks += 1
        self.f.close()
        hdr = struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BLOCK, self.total, self.chunks, 0)
        with open(self.parts[0], 'r+b') as f:
            f.write(hdr)

# ---------------------------------------------------------------- build
def build(root, out_path, volume_bytes, name='iOSRoot', progress=print, part_bytes=0, metadata_only=False):
    total_blocks = volume_bytes // BLOCK
    t0 = time.time()

    # 1) CNIDs, catalog records (data fork start blocks filled in later)
    root.cnid = ROOT_FOLDER
    entries = []        # (parent_cnid, u16name, node)
    stack = [root]
    nxt = FIRST_USER
    nfiles = nfolders = 0
    while stack:
        d = stack.pop()
        for nm in sorted(d.children):
            n = d.children[nm]
            n.cnid = nxt
            nxt += 1
            entries.append((d.cnid, hfs_name(nm), n))
            if isinstance(n, Dir):
                nfolders += 1
                stack.append(n)
            else:
                nfiles += 1
    progress('catalog: %d folders, %d files/symlinks' % (nfolders, nfiles))

    # 2) layout: bitmap, extents tree, catalog tree, data
    bitmap_bytes = (total_blocks + 7) // 8
    bitmap_blocks = (bitmap_bytes + BLOCK - 1) // BLOCK
    ext_nodes = 16                                  # header + room, like newfs
    ext_blocks = ext_nodes * EXT_NODE // BLOCK

    # estimate catalog size: ~ (key + record + 2) per record
    est = 0
    for parent, u16, n in entries:
        k = 8 + len(u16)
        est += k + (88 if isinstance(n, Dir) else 248) + 2      # record
        est += 8 + 10 + len(u16) + 2                             # thread record
    est += 10 * CAT_NODE
    cat_nodes = int(est * 1.25) // CAT_NODE + 64
    cat_blocks = cat_nodes * CAT_NODE // BLOCK

    pos = 1                                   # block 0 = volume header
    alloc_start = pos; pos += bitmap_blocks
    ext_start = pos; pos += ext_blocks
    cat_start = pos; pos += cat_blocks
    data_start = pos
    files = [n for _, _, n in entries if isinstance(n, (File, Symlink))]
    files.sort(key=lambda n: getattr(n, 'order', 0))     # data placement order (stable)
    for n in files:
        nb = (n.size + BLOCK - 1) // BLOCK
        n.start, n.blocks = (pos, nb) if nb else (0, 0)
        pos += nb
    data_end = pos
    if data_end + 1 > total_blocks:
        raise SystemExit('volume too small: need %d blocks, have %d' % (data_end + 1, total_blocks))
    used_blocks = data_end + 1                # + last block (alternate header)
    progress('layout: data %d MiB, volume %d MiB' % ((data_end - data_start) * BLOCK >> 20, volume_bytes >> 20))

    # 3) catalog records
    recs = []
    vol_u16 = hfs_name(name)
    folder_count = {}
    valence = {}
    for parent, u16, n in entries:
        valence[parent] = valence.get(parent, 0) + 1
        if isinstance(n, Dir):
            folder_count[parent] = folder_count.get(parent, 0) + 1

    def folder_rec(n, cnid):
        return struct.pack('>hHII', FOLDER_REC, HAS_FOLDER_COUNT, valence.get(cnid, 0), cnid) + \
            struct.pack('>5I', hfs_time(n.ctime), hfs_time(n.mtime), hfs_time(n.mtime), hfs_time(n.mtime), 0) + \
            bsd_info(n, stat.S_IFDIR) + b'\0' * 16 + b'\0' * 16 + \
            struct.pack('>II', 0, folder_count.get(cnid, 0))

    rootrec = folder_rec(root, ROOT_FOLDER)
    recs.append((sort_key(ROOT_PARENT, vol_u16), cat_key(ROOT_PARENT, vol_u16), rootrec))
    recs.append((sort_key(ROOT_FOLDER, b''), cat_key(ROOT_FOLDER, b''),
                 struct.pack('>hhI', FOLDER_THREAD, 0, ROOT_PARENT) + uni255(vol_u16)))
    for parent, u16, n in entries:
        if isinstance(n, Dir):
            data = folder_rec(n, n.cnid)
            thread = struct.pack('>hhI', FOLDER_THREAD, 0, parent) + uni255(u16)
        else:
            if isinstance(n, Symlink):
                ftype, finfo = stat.S_IFLNK, b'slnkrhap' + b'\0' * 8
            else:
                ftype, finfo = stat.S_IFREG, b'\0' * 16
            data = struct.pack('>hHII', FILE_REC, HAS_THREAD, 0, n.cnid) + \
                struct.pack('>5I', hfs_time(n.ctime), hfs_time(n.mtime), hfs_time(n.mtime), hfs_time(n.mtime), 0) + \
                bsd_info(n, ftype) + finfo + b'\0' * 16 + struct.pack('>II', 0, 0) + \
                fork(n.size, n.start, n.blocks) + fork(0, 0, 0)
            assert len(data) == 248
            thread = struct.pack('>hhI', FILE_THREAD, 0, parent) + uni255(u16)
        recs.append((sort_key(parent, u16), cat_key(parent, u16), data))
        recs.append((sort_key(n.cnid, b''), cat_key(n.cnid, b''), thread))
    recs.sort(key=lambda r: r[0])
    progress('catalog records: %d (%.0fs)' % (len(recs), time.time() - t0))

    cat, cat_total = build_btree([(k, d) for _, k, d in recs], CAT_NODE, 0xBC, 0x6, 516, cat_nodes)
    if cat_total > cat_nodes:
        raise SystemExit('catalog estimate too small (%d > %d)' % (cat_total, cat_nodes))
    ext, _ = build_btree([], EXT_NODE, 0, 0x2, 10, ext_nodes)

    # 4) volume header (everything it needs is known now)
    now = hfs_time(time.time())
    fh = struct.pack('>HHI', 0x4858, 5, (1 << 8))            # 'HX', v5, unmounted
    fh += b'10.0' + struct.pack('>I', 0)                     # lastMountedVersion, journal
    fh += struct.pack('>4I', now, now, 0, now)               # create, modify, backup, checked
    fh += struct.pack('>II', nfiles, nfolders)
    fh += struct.pack('>III', BLOCK, total_blocks, total_blocks - used_blocks)
    fh += struct.pack('>IIII', data_end, 65536, 65536, nxt)  # nextAllocation, clumps, nextCatalogID
    fh += struct.pack('>IQ', 1, 1)                           # writeCount, encodingsBitmap (MacRoman)
    fh += b'\0' * 32                                         # finderInfo
    fh += fork(bitmap_blocks * BLOCK, alloc_start, bitmap_blocks, bitmap_blocks * BLOCK)
    fh += fork(ext_nodes * EXT_NODE, ext_start, ext_blocks, ext_nodes * EXT_NODE)
    fh += fork(cat_nodes * CAT_NODE, cat_start, cat_blocks, CAT_NODE * 256)
    fh += fork(0, 0, 0)                                      # attributes
    fh += fork(0, 0, 0)                                      # startup
    assert len(fh) == 512, len(fh)

    # 5) write, strictly in increasing block order (so the output can be an
    #    Android sparse stream): header block, bitmap, extents, catalog, data,
    #    gap, last block (alternate volume header at volume_bytes - 1024).
    out = (SparseOut(out_path, total_blocks, part_bytes) if part_bytes else
           RawOut(out_path, volume_bytes))
    try:
        out.write(0, b'\0' * 1024 + bytes(fh) + b'\0' * (BLOCK - 1536))
        bm = bytearray(bitmap_blocks * BLOCK)
        full, rem = divmod(data_end, 8)
        bm[:full] = b'\xff' * full
        if rem:
            bm[full] = (0xff << (8 - rem)) & 0xff
        last = total_blocks - 1
        bm[last >> 3] |= 0x80 >> (last & 7)
        out.write(alloc_start, bytes(bm))
        out.write(ext_start, b''.join(ext.get(i, b'\0' * EXT_NODE) for i in range(ext_nodes)))
        for i in range(cat_nodes):
            out.write(cat_start + i * CAT_NODE // BLOCK, cat.get(i, b'\0' * CAT_NODE))
        done, tot = 0, sum(n.size for n in files)
        last_t = 0
        for n in files:
            if not n.blocks:
                continue
            if metadata_only:
                continue
            if isinstance(n, Symlink):
                out.write(n.start, n.target + b'\0' * (n.blocks * BLOCK - n.size))
            else:
                src = n.opener()
                left, blk = n.size, n.start
                while left:
                    want = min(left, 8 << 20)
                    b = src.read(want)
                    while len(b) < want:
                        more = src.read(want - len(b))
                        if not more:
                            raise IOError('short read for %s (%d left)' % (getattr(n, 'label', n.cnid), left))
                        b += more
                    left -= len(b)
                    if not left:
                        b += b'\0' * (-len(b) % BLOCK)
                    out.write(blk, b)
                    blk += len(b) // BLOCK
                if hasattr(src, 'close'):
                    src.close()
                if hasattr(n, 'done'):
                    n.done()
            done += n.size
            if time.time() - last_t > 15:
                last_t = time.time()
                progress('data %5.1f%% (%d / %d MiB)' % (100.0 * done / max(tot, 1), done >> 20, tot >> 20))
        out.write(total_blocks - 1, b'\0' * (BLOCK - 1024) + bytes(fh) + b'\0' * 512)
    finally:
        out.close()
    progress('done in %.0fs: %s' % (time.time() - t0, out_path))


# ---------------------------------------------------------------- self test
def tree_from_dir(path):
    def walk(p):
        st = os.lstat(p)
        if stat.S_ISDIR(st.st_mode):
            d = Dir(uid=st.st_uid, gid=st.st_gid, mode=st.st_mode & 0o7777, mtime=st.st_mtime)
            for nm in os.listdir(p):
                c = walk(os.path.join(p, nm))
                if c is not None:
                    d.add(nm, c)
            return d
        if stat.S_ISLNK(st.st_mode):
            return Symlink(os.readlink(p), uid=st.st_uid, gid=st.st_gid, mode=st.st_mode & 0o7777, mtime=st.st_mtime)
        if stat.S_ISREG(st.st_mode):
            return File(st.st_size, lambda p=p: open(p, 'rb'), uid=st.st_uid, gid=st.st_gid,
                        mode=st.st_mode & 0o7777, mtime=st.st_mtime)
        return None
    return walk(path)


if __name__ == '__main__':
    if len(sys.argv) == 4 and sys.argv[1] == '--selftest':
        root = tree_from_dir(sys.argv[2])
        build(root, sys.argv[3], int(os.environ.get("VOLMB", "256")) << 20, name='SelfTest',
              part_bytes=int(os.environ.get("PARTKB", "0")) << 10,
              metadata_only=bool(os.environ.get("METAONLY")))
    else:
        sys.exit(__doc__)
