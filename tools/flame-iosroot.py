#!/usr/bin/env python3
"""Build the flame iOS root (stage 4) WITHOUT mounting anything.

Same content as tools/mkiosroot.sh followed by tools/sweet-root-tweaks.py, but:
  * the IPSW volumes are read with 7-Zip (sealed APFS, UDIF/LZFSE), and the
    LZBITMAP-compressed cryptex files are decoded by tools/zbm (checked against
    each Mach-O's code-signature page hashes);
  * the HFSX volume is written by tools/hfsbuild.py straight into an Android
    sparse image (what fastboot flashes), cut into download-sized parts.

  flame-iosroot.py OS.dmg SYSTEMOS.dmg APPOS.dmg OUTPREFIX [--meta-only] [--part-mib N] [--volume-gib N]

Differences to mkiosroot.sh, on purpose:
  * hard links become copies (this HFS+ writer has no indirect-node support);
    the dyld cache "links" are therefore real copies (+~2.5 GB).
  * not journaled (as Bergamota's renegade does); xattrs are not copied
    (mkiosroot's rsync -aH did not copy them either).
"""
import argparse, fnmatch, io, os, plistlib, posixpath, shutil, stat, subprocess, sys, time
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'zbm'))
import hfsbuild as H          # noqa: E402
import zbm, cscheck           # noqa: E402

BUILD, VERSION = '20H392', '16.7.16'
MOBILE = 501


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


# ---------------------------------------------------------------- 7-Zip listing
def parse_mode(s):
    kind = {'d': stat.S_IFDIR, 'l': stat.S_IFLNK, '-': stat.S_IFREG}.get(s[0])
    m = 0
    for i, (r, w, x) in enumerate(((1, 2, 3), (4, 5, 6), (7, 8, 9))):
        sh = 6 - 3 * i
        if s[r] == 'r': m |= 4 << sh
        if s[w] == 'w': m |= 2 << sh
        c = s[x]
        if c in 'xst': m |= 1 << sh
        if i < 2 and c in 'sS': m |= (0o4000 if i == 0 else 0o2000)
        if i == 2 and c in 'tT': m |= 0o1000
    return kind, m


def parse_time(s):
    if not s:
        return 0.0
    base, _, frac = s.partition('.')
    t = datetime.strptime(base, '%Y-%m-%d %H:%M:%S').replace(tzinfo=timezone.utc).timestamp()
    return t + (float('0.' + frac) if frac else 0.0)


def listing(archive):
    out = subprocess.run(['7z', 'l', '-slt', archive], capture_output=True, text=True, errors='surrogateescape').stdout
    body = out.split('\n----------\n', 1)[1]
    items, streams = [], {}
    for blk in body.split('\n\n'):
        d = {}
        for line in blk.splitlines():
            k, sep, v = line.partition(' = ')
            if sep:
                d[k] = v
        if 'Path' not in d or 'Mode' not in d or not d['Mode']:
            continue
        if d.get('Alternate Stream') == '+':
            base, _, sname = d['Path'].rpartition(':')
            streams.setdefault(base, {})[sname] = int(d.get('Size') or 0)
            continue
        items.append(d)
    return items, streams


# ---------------------------------------------------------------- tree helpers
def mkdirs(root, path, uid=0, gid=0, mode=0o755):
    """mkdir -p as root (umask 022): only NEW directories get these attributes."""
    d = root
    for part in [p for p in path.split('/') if p]:
        n = d.children.get(part)
        if n is None:
            n = d.add(part, H.Dir(uid=uid, gid=gid, mode=mode))
        if not isinstance(n, H.Dir):
            raise SystemExit('not a directory: %s' % path)
        d = n
    return d


def lookup(root, path, follow_last=True, depth=0):
    """Resolve path inside the image like the kernel would (symlinks too)."""
    if depth > 40:
        return None
    parts = [p for p in path.split('/') if p and p != '.']
    cur, stack = root, []
    for i, part in enumerate(parts):
        if part == '..':
            cur = stack.pop() if stack else root
            continue
        if not isinstance(cur, H.Dir):
            return None
        n = cur.children.get(part)
        if n is None:
            return None
        last = i == len(parts) - 1
        if isinstance(n, H.Symlink) and (follow_last or not last):
            t = n.target.decode('utf-8', 'surrogateescape')
            base = '/' if t.startswith('/') else '/'.join(parts[:i])
            rest = '/'.join(parts[i + 1:])
            return lookup(root, posixpath.join(base, t, rest) if rest else posixpath.join(base, t),
                          follow_last, depth + 1)
        stack.append(cur)
        cur = n
    return cur


def clone(n):
    """A copy standing in for a hard link: same metadata, same data source."""
    if isinstance(n, H.Symlink):
        c = H.Symlink(n.target, uid=n.uid, gid=n.gid, mode=n.mode, mtime=n.mtime, ctime=n.ctime)
    else:
        c = H.File(n.size, n.opener, uid=n.uid, gid=n.gid, mode=n.mode, mtime=n.mtime, ctime=n.ctime)
        for a in ('order', 'label', 'src'):
            if hasattr(n, a):
                setattr(c, a, getattr(n, a))
    return c


def walk(d, prefix=''):
    for nm, n in d.children.items():
        p = prefix + nm
        yield p, n
        if isinstance(n, H.Dir):
            yield from walk(n, p + '/')


# ---------------------------------------------------------------- data source (batched 7-Zip extraction)
class Source:
    BATCH = 1536 << 20

    def __init__(self, stage):
        self.stage = stage
        self.cur = None
        self.batches = []        # (archive, [paths], zbm_paths)
        self.zbm_checked = [0, 0]

    def plan(self, files):
        """files: list of (archive, path, size, is_zbm) unique sources, in order."""
        idx = {}
        cur, cur_size, cur_arch = None, 0, None
        for arch, path, size, is_zbm in files:
            if cur is None or arch != cur_arch or cur_size + size > self.BATCH:
                cur = (arch, [], set())
                self.batches.append(cur)
                cur_size, cur_arch = 0, arch
            cur[1].append(path)
            if is_zbm:
                cur[2].add(path)
            cur_size += size
            idx[(arch, path)] = len(self.batches) - 1
        return idx

    def _extract(self, b):
        if self.cur == b:
            return
        if self.cur is not None:
            shutil.rmtree(os.path.join(self.stage, 'b%04d' % self.cur), ignore_errors=True)
        arch, paths, zb = self.batches[b]
        d = os.path.join(self.stage, 'b%04d' % b)
        os.makedirs(d, exist_ok=True)
        lst = os.path.join(self.stage, 'list%04d.txt' % b)
        with open(lst, 'w', errors='surrogateescape') as f:
            for p in paths:
                f.write(p + '\n')
        cmd = ['7z', 'x', '-y', '-o' + d, '-scsUTF-8', arch, '@' + lst]
        if zb:
            cmd.insert(3, '-sns')
        r = subprocess.run(cmd, capture_output=True, text=True, errors='replace')
        # ZBM files make 7-Zip report "Unsupported method" for their main stream: expected
        if r.returncode not in (0, 1, 2):
            raise SystemExit('7z failed (%d) on batch %d:\n%s' % (r.returncode, b, r.stdout[-2000:]))
        self.cur = b
        log('batch %d/%d extracted (%s, %d files)' % (b + 1, len(self.batches), os.path.basename(arch), len(paths)))

    def opener(self, b, path, size, is_zbm):
        def op():
            self._extract(b)
            fp = os.path.join(self.stage, 'b%04d' % b, path)
            if is_zbm:
                dec = open(fp + ':com.apple.decmpfs', 'rb').read()
                rsrc_p = fp + ':com.apple.ResourceFork'
                rsrc = open(rsrc_p, 'rb').read() if os.path.exists(rsrc_p) else b''
                data = zbm.decmpfs_decode(dec, rsrc)
                if len(data) != size:
                    raise SystemExit('ZBM size mismatch %s: %d != %d' % (path, len(data), size))
                ok = cscheck.verify(data)
                if ok is False:
                    raise SystemExit('ZBM decode failed the code-signature check: ' + path)
                if ok:
                    self.zbm_checked[0] += 1
                else:
                    self.zbm_checked[1] += 1
                return io.BytesIO(data)
            if os.path.getsize(fp) != size:
                raise SystemExit('size mismatch %s: %d != %d' % (path, os.path.getsize(fp), size))
            return open(fp, 'rb')
        return op


# ---------------------------------------------------------------- build the tree
def add_volume(root, prefix, arch, sources):
    items, streams = listing(arch)
    base = mkdirs(root, prefix) if prefix else root
    nfile = 0
    items.sort(key=lambda d: d['Path'].count('/'))
    for d in items:
        path = d['Path']
        kind, perm = parse_mode(d['Mode'])
        uid, gid = int(d.get('User ID') or 0), int(d.get('Group ID') or 0)
        mt = parse_time(d.get('Modified'))
        ct = parse_time(d.get('Created')) or mt
        parent_path, _, name = path.rpartition('/')
        parent = base.get(parent_path, create=lambda: H.Dir(uid=0, gid=0, mode=0o755)) if parent_path else base
        if kind == stat.S_IFDIR:
            ex = parent.children.get(name)
            if isinstance(ex, H.Dir):
                ex.uid, ex.gid, ex.mode, ex.mtime, ex.ctime = uid, gid, perm, mt, ct
            else:
                parent.add(name, H.Dir(uid=uid, gid=gid, mode=perm, mtime=mt, ctime=ct))
        elif kind == stat.S_IFLNK:
            parent.add(name, H.Symlink(d.get('Symbolic Link', '').encode('utf-8', 'surrogateescape'),
                                       uid=uid, gid=gid, mode=perm, mtime=mt, ctime=ct))
        elif kind == stat.S_IFREG:
            size = int(d.get('Size') or 0)
            is_zbm = d.get('Method') == 'ZBM-rsrc' or d.get('Method', '').startswith('ZBM')
            f = H.File(size, None, uid=uid, gid=gid, mode=perm, mtime=mt, ctime=ct)
            f.src = (arch, path, size, is_zbm)
            f.label = prefix + '/' + path if prefix else path
            if size:
                sources.append(f)
            else:
                f.opener = lambda: io.BytesIO(b'')
            parent.add(name, f)
            nfile += 1
        else:
            raise SystemExit('unsupported entry type %r: %s' % (d['Mode'], path))
    log('%s: %d entries (%d files) -> /%s' % (os.path.basename(arch), len(items), nfile, prefix))


def put(root, path, node):
    parent_path, _, name = path.rpartition('/')
    mkdirs(root, parent_path).add(name, node)


def extract_small(arch, paths, stage):
    d = os.path.join(stage, 'pre')
    os.makedirs(d, exist_ok=True)
    subprocess.run(['7z', 'x', '-y', '-o' + d, arch] + paths, capture_output=True)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('os'); ap.add_argument('sysos'); ap.add_argument('appos'); ap.add_argument('out')
    ap.add_argument('--meta-only', action='store_true')
    ap.add_argument('--part-mib', type=int, default=1024)
    ap.add_argument('--volume-gib', type=int, default=100)
    ap.add_argument('--stage', default=None)
    a = ap.parse_args()
    stage = a.stage or (a.out + '.stage')
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)

    root = H.Dir(uid=0, gid=0, mode=0o755)
    sources = []
    # mkiosroot.sh: copy OS root, then the cryptexes into /private/preboot/Cryptexes/{OS,App}
    add_volume(root, '', a.os, sources)
    add_volume(root, 'private/preboot/Cryptexes/OS', a.sysos, sources)
    add_volume(root, 'private/preboot/Cryptexes/App', a.appos, sources)

    # shared cache where dyld looks at ignition level 1 (hard links -> copies)
    dsc = 'private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld'
    cache_dir = lookup(root, dsc)
    dst = mkdirs(root, 'System/Library/Caches/com.apple.dyld')
    n = 0
    for nm, f in sorted(cache_dir.children.items()):
        if isinstance(f, H.Dir):
            continue
        dst.add(nm, clone(f)); n += 1
    log('dyld shared cache: %d files copied to /System/Library/Caches/com.apple.dyld' % n)

    # cryptex files with no counterpart in the OS root -> same path in the root
    linked = 0
    for c in ('OS', 'App'):
        croot = lookup(root, 'private/preboot/Cryptexes/' + c)
        for p, node in list(walk(croot)):
            if isinstance(node, H.Dir) or p.startswith('System/Library/Caches/com.apple.dyld/'):
                continue
            if lookup(root, p) is None and lookup(root, p, follow_last=False) is None:
                parent_path = posixpath.dirname(p)
                mkdirs(root, parent_path)
                if isinstance(node, H.Symlink):      # cp -P as root
                    put(root, p, H.Symlink(node.target, uid=0, gid=0, mode=0o755))
                else:
                    put(root, p, clone(node))
                linked += 1
    log('cryptex files linked into the root: %d' % linked)

    # logd stores (owner _logd 272, .migrated markers)
    for sub, mode in (('diagnostics', 0o751), ('uuidtext', 0o755), ('diagnostics/timesync', 0o755)):
        d = mkdirs(root, 'private/var/db/' + sub)
        d.uid = d.gid = 272
        d.mode = mode
    for sub in ('diagnostics', 'uuidtext', 'diagnostics/timesync'):
        put(root, 'private/var/db/%s/.migrated' % sub, H.bytes_file(b'', uid=272, gid=272, mode=0o644))
    # system keybag for the built-in AppleKeyStore (no passcode)
    kb = mkdirs(root, 'private/var/keybags')
    kb.uid = kb.gid = 0
    kb.mode = 0o700
    put(root, 'private/var/keybags/systembag.kb', H.bytes_file(
        b'<?xml version="1.0" encoding="UTF-8"?>\n'
        b'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">\n'
        b'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>\n',
        uid=0, gid=0, mode=0o600))

    # ---- sweet-root-tweaks.py, on the tree
    mt_confs = [p for p, _ in walk(lookup(root, 'System/Library/Frameworks/MediaToolbox.framework'),
                                   'System/Library/Frameworks/MediaToolbox.framework/')
                if fnmatch.fnmatch(p, 'System/Library/Frameworks/MediaToolbox.framework/*/SubServerConfiguration.plist')
                and p.count('/') == 5]
    pre = extract_small(a.os, ['System/Library/xpc/launchd.plist'] + mt_confs, stage)

    def replace(path, data, like):
        put(root, path, H.bytes_file(data, uid=like.uid, gid=like.gid, mode=like.mode, mtime=time.time()))

    lp = 'System/Library/xpc/launchd.plist'
    plist = plistlib.load(open(os.path.join(pre, lp), 'rb'))
    jobs = plist['LaunchDaemons']
    renamed = 0
    for job in jobs.values():
        if '_PanicOnCrash' in job:
            job['_PanicOnCrasX'] = job.pop('_PanicOnCrash'); renamed += 1
    removed = [k for k, j in jobs.items() if j.get('Label') == 'com.apple.CommCenter']
    for k in removed:
        del jobs[k]
    replace(lp, plistlib.dumps(plist, fmt=plistlib.FMT_BINARY), lookup(root, lp))
    log('launchd.plist: %d _PanicOnCrash renamed, %d CommCenter jobs removed' % (renamed, len(removed)))

    for sub, uid in (('Bundle', 33), ('Data/System', 0), ('Shared/SystemGroup', 0), ('Temp', 0)):
        p = 'private/var/containers/' + sub
        if not isinstance(lookup(root, p), H.Dir):
            d = mkdirs(root, p)
            d.uid = d.gid = uid
            d.mode = 0o755
            log('mkdir /' + p)
    log('warning: no mobileactivationd system container in the image; skipped data ark')

    vap = 'Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin'
    hal = lookup(root, 'Library/Audio/Plug-Ins/HAL')
    if isinstance(hal, H.Dir) and 'VirtualAudio.plugin' in hal.children:
        node = hal.children.pop('VirtualAudio.plugin')
        mkdirs(root, 'Library/Audio/Plug-Ins/HAL.disabled').add('VirtualAudio.plugin', node)
        log('VirtualAudio.plugin disabled')
    for p in mt_confs:
        data = plistlib.load(open(os.path.join(pre, p), 'rb'))
        data['StartRecorderServer'] = False
        replace(p, plistlib.dumps(data, fmt=plistlib.FMT_XML), lookup(root, p))
        log('StartRecorderServer off', p)

    prefs = 'private/var/mobile/Library/Preferences'

    def merge_pref(name, values):
        p = '%s/%s.plist' % (prefs, name)
        if lookup(root, p) is not None:
            raise SystemExit('pref exists in the image, merge not implemented: ' + p)
        put(root, p, H.bytes_file(plistlib.dumps(values, fmt=plistlib.FMT_BINARY), uid=MOBILE, gid=MOBILE, mode=0o600))
        log('pref', name, sorted(values))
    merge_pref('com.apple.migration', {
        'LastSystemVersion': BUILD,
        'DMUserDataDisposition': {'SyntheticDidUpgrade': False, 'buildVersion': BUILD}})
    merge_pref('com.apple.springboard', {
        'BKNoWatchdogs': True,
        'FBLaunchWatchdogExceptions': {'com.apple.purplebuddy': 100.0},
        'MCForceShowCloudConfigurationUI': False})
    merge_pref('com.apple.purplebuddy', {
        'SetupDone': True, 'SetupFinishedAllSteps': True, 'SetupVersion': 11,
        'daemonizeNeedsToRun': False})
    merge_pref('com.apple.purplebuddy.notbackedup', {
        k: False for k in ('ShouldRunDiagnosticsMiniBuddy', 'ShouldRunHSA2MiniBuddy',
                           'ShouldRunKeychainSyncMiniBuddy', 'ShouldRunMesaMiniBuddy',
                           'ShouldRunPaymentMiniBuddy', 'ShouldRunStandardMiniBuddy',
                           'SilentICDPUpgrade')})
    merge_pref('com.apple.managedconfiguration.notbackedup', {
        'LockdownActivationIndicatesCloudConfigurationAvailable': False})

    # ---- data sources: batch plan in (archive, path) order
    src = Source(stage)
    uniq, seen = [], set()
    allfiles = [n for _, n in walk(root) if isinstance(n, H.File) and getattr(n, 'src', None) and n.size]
    for n in allfiles:
        if n.src[:2] not in seen:
            seen.add(n.src[:2]); uniq.append(n.src)
    order = {a: i for i, a in enumerate((a.os, a.sysos, a.appos))}
    uniq.sort(key=lambda s: (order[s[0]], s[1]))
    batch_of = src.plan(uniq)
    for n in allfiles:
        b = batch_of[n.src[:2]]
        n.order = b
        n.opener = src.opener(b, n.src[1], n.size, n.src[3])
    total = sum(n.size for _, n in walk(root) if isinstance(n, (H.File, H.Symlink)))
    log('data: %d MiB in %d batches, %d unique sources' % (total >> 20, len(src.batches), len(uniq)))

    t = time.time()
    if a.meta_only:
        H.build(root, a.out, a.volume_gib << 30, name='iOSRoot', progress=log, metadata_only=True)
    else:
        H.build(root, a.out, a.volume_gib << 30, name='iOSRoot', progress=log, part_bytes=a.part_mib << 20)
        log('LZBITMAP files: %d verified by code signature, %d without signature' % tuple(src.zbm_checked))
    shutil.rmtree(stage, ignore_errors=True)
    log('finished in %.0f s' % (time.time() - t))


if __name__ == '__main__':
    main()
