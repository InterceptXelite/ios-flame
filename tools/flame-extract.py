#!/usr/bin/env python3
"""Read flame's LIVE device tree (adb shell su -c 'cat /sys/firmware/fdt' > dtb/flame.dtb)
and generate every board-specific value the port needs.

  python3 tools/flame-extract.py dtb/flame.dtb

Writes:
  shim/board_gen.h        framebuffer, UFS, PS_HOLD, IMEM restart reason, panel size
  tools/flame_board.json  same + memory banks, GIC check, SPMI, touch, keys (for mkadt / XNU patches)
and prints a report. Nothing here talks to the phone.
"""
import json, os, struct, sys

FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


class Node:
    def __init__(self, name, parent=None):
        self.name, self.parent, self.props, self.children = name, parent, {}, []

    @property
    def path(self):
        if not self.parent:
            return '/'
        pp = self.parent.path
        return (pp if pp != '/' else '') + '/' + self.name

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()

    def str(self, k):
        v = self.props.get(k)
        return v.rstrip(b'\0').decode(errors='replace') if v is not None else None

    def strs(self, k):
        v = self.props.get(k)
        return [x.decode(errors='replace') for x in v.split(b'\0') if x] if v else []

    def u32s(self, k):
        v = self.props.get(k)
        return list(struct.unpack('>%dI' % (len(v) // 4), v[:len(v) // 4 * 4])) if v else []

    def cells(self, k, default):
        v = self.u32s(k)
        return v[0] if v else default

    def reg(self):
        """[(addr, size)] using the parent's #address-cells / #size-cells."""
        p = self.parent
        ac = p.cells('#address-cells', 2) if p else 2
        sc = p.cells('#size-cells', 1) if p else 1
        v, out = self.u32s('reg'), []
        step = ac + sc
        for i in range(0, len(v) - step + 1, step):
            a = 0
            for x in v[i:i + ac]:
                a = (a << 32) | x
            s = 0
            for x in v[i + ac:i + step]:
                s = (s << 32) | x
            out.append((a, s))
        return out

    def compat(self):
        return self.strs('compatible')

    def unit(self):
        return int(self.name.split('@')[1].split(',')[0], 16) if '@' in self.name else None


def parse(blob):
    magic, total, off_struct, off_strings = struct.unpack('>IIII', blob[:16])
    if magic != 0xd00dfeed:
        sys.exit('not a DTB (bad magic) — did adb add \\r\\n? use: adb exec-out su -c "cat /sys/firmware/fdt" > flame.dtb')
    strings = blob[off_strings:]
    pos, root, cur = off_struct, None, None
    while True:
        tok = struct.unpack('>I', blob[pos:pos + 4])[0]
        pos += 4
        if tok == FDT_BEGIN_NODE:
            end = blob.index(b'\0', pos)
            name = blob[pos:end].decode()
            pos = (end + 4) & ~3
            n = Node(name, cur)
            if cur:
                cur.children.append(n)
            else:
                root = n
            cur = n
        elif tok == FDT_END_NODE:
            cur = cur.parent
        elif tok == FDT_PROP:
            ln, nameoff = struct.unpack('>II', blob[pos:pos + 8])
            pos += 8
            pname = strings[nameoff:strings.index(b'\0', nameoff)].decode()
            cur.props[pname] = blob[pos:pos + ln]
            pos = (pos + ln + 3) & ~3
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            break
        else:
            sys.exit('bad FDT token %d' % tok)
    return root


def find(root, pred):
    return [n for n in root.walk() if pred(n)]


def by_compat(root, *subs):
    return find(root, lambda n: any(any(s in c for s in subs) for c in n.compat()))


def hx(v):
    return '0x%x' % v


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = parse(open(sys.argv[1], 'rb').read())
    here = os.path.dirname(os.path.abspath(__file__))
    rep, out, problems = [], {}, []

    model = root.str('model') or '?'
    compat = root.compat()
    rep.append('model: %s' % model)
    rep.append('compatible: %s' % ', '.join(compat))
    if not any('4450' in c or 'flame' in c.lower() for c in compat + [model]):
        problems.append('this DTB does not look like flame/SM4450 — wrong phone or Pixel 4 file?')

    # memory banks
    banks = []
    for n in find(root, lambda n: n.str('device_type') == 'memory'):
        banks += [r for r in n.reg() if r[1]]
    banks.sort()
    out['memory'] = [[hx(a), hx(s)] for a, s in banks]
    rep.append('memory: ' + ', '.join('%s+%s (%d MiB)' % (hx(a), hx(s), s >> 20) for a, s in banks))

    # reserved memory (needed to choose where XNU may live)
    resv = []
    rm = find(root, lambda n: n.name == 'reserved-memory')
    for n in (rm[0].children if rm else []):
        for a, s in n.reg():
            resv.append((a, s, n.name))
    resv.sort()
    out['reserved'] = [[hx(a), hx(s), nm] for a, s, nm in resv]

    # framebuffer: continuous splash region left on by ABL
    fb = None
    for a, s, nm in resv:
        if any(k in nm.lower() for k in ('splash', 'framebuffer', 'cont_splash', 'disp_rdump')):
            if 'rdump' in nm and fb:
                continue
            fb = (a, s, nm)
            if 'splash' in nm:
                break
    for n in by_compat(root, 'simple-framebuffer'):
        r = n.reg()
        if r:
            fb = (r[0][0], r[0][1], n.path)
    if fb:
        rep.append('framebuffer: %s size %s  (%s)' % (hx(fb[0]), hx(fb[1]), fb[2]))
    else:
        problems.append('no splash/framebuffer region found')

    # panel size: from the panel node chosen by ABL (cmdline / qcom,dsi-display-active) or any panel
    w = h = None
    panels = find(root, lambda n: 'qcom,mdss-dsi-panel-width' in n.props or 'qcom,mdss-dsi-panel-height' in n.props)
    active = [n for n in panels if n.str('status') in (None, 'ok', 'okay')]
    for n in active or panels:
        if n.u32s('qcom,mdss-dsi-panel-width') and n.u32s('qcom,mdss-dsi-panel-height'):
            w, h = n.u32s('qcom,mdss-dsi-panel-width')[0], n.u32s('qcom,mdss-dsi-panel-height')[0]
            rep.append('panel: %dx%d (%s)' % (w, h, n.path))
            break
    if not w:
        w, h = 720, 1640
        rep.append('panel: not in DTB, using flame spec 720x1640')
    if fb and fb[1] and fb[1] < w * h * 4:
        problems.append('splash region (%s) smaller than %dx%dx4 — check pixel format/stride' % (hx(fb[1]), w, h))

    # UFS host
    ufs = None
    for n in by_compat(root, 'qcom,ufshc', 'ufshc'):
        if n.reg() and 'phy' not in ' '.join(n.compat()):
            ufs = n.reg()[0]
            rep.append('ufs: %s size %s (%s)' % (hx(ufs[0]), hx(ufs[1]), n.path))
            break
    if not ufs:
        problems.append('no UFS host (qcom,ufshc) found')

    # PS_HOLD (used to reboot) and IMEM restart reason (reboot-to-fastboot)
    pshold = None
    for n in by_compat(root, 'qcom,pshold'):
        if n.reg():
            pshold = n.reg()[0][0]
    rep.append('pshold: %s' % (hx(pshold) if pshold else 'not found'))
    imem_rr = None
    for n in by_compat(root, 'qcom,msm-imem'):
        base = n.reg()[0][0] if n.reg() else None
        for c in n.children:
            if 'restart_reason' in c.name or 'qcom,msm-imem-restart_reason' in c.compat():
                off = c.u32s('reg')[0] if c.u32s('reg') else c.unit()
                if base is not None and off is not None:
                    imem_rr = base + off
    rep.append('imem restart_reason: %s' % (hx(imem_rr) if imem_rr else 'not found'))

    # GIC sanity check against board.h
    gic = by_compat(root, 'arm,gic-v3')
    if gic:
        r = gic[0].reg()
        rep.append('gic: ' + ', '.join(hx(a) for a, _ in r))
        if r and r[0][0] != 0x17200000:
            problems.append('GICD is %s, board.h assumes 0x17200000 — fix board.h' % hx(r[0][0]))
        out['gic'] = [hx(a) for a, _ in r]

    # timer PPIs
    t = by_compat(root, 'arm,armv8-timer')
    if t:
        v = t[0].u32s('interrupts')
        ppis = [v[i + 1] for i in range(0, len(v), 3)]
        rep.append('timer PPIs (sec, phys, virt, hyp): %s' % ppis)
        if len(ppis) > 2 and ppis[2] + 16 != 27:
            problems.append('virtual timer INTID is %d, board.h assumes 27' % (ppis[2] + 16))

    # SPMI / PMIC / keys
    spmi = by_compat(root, 'qcom,spmi-pmic-arb')
    if spmi:
        out['spmi'] = [[hx(a), hx(s)] for a, s in spmi[0].reg()]
        rep.append('spmi: ' + ', '.join(hx(a) for a, _ in spmi[0].reg()))
    pmics = []
    for n in find(root, lambda n: any('spmi-pmic' in c or 'qcom,pm' in c for c in n.compat())):
        if n.parent and spmi and n.parent is spmi[0]:
            pmics.append('%s sid=%s %s' % (n.name, n.u32s('reg')[:1], n.compat()[:1]))
    if pmics:
        out['pmics'] = pmics
        rep.append('pmics: ' + '; '.join(pmics))
    keys = [n.path for n in by_compat(root, 'pon', 'gpio-keys')]
    out['keys'] = keys

    # touch: every I2C/SPI child with a touch-ish compatible
    touch = []
    for n in root.walk():
        c = ' '.join(n.compat()).lower()
        if any(k in c for k in ('touch', 'goodix', 'focaltech', 'novatek', 'nvt', 'ilitek', 'chipone', 'himax', 'synaptics', 'gt9', 'ft8', 'tcm', 'omnivision')):
            if n.parent and n.parent.reg():
                touch.append({'node': n.path, 'compatible': n.compat(), 'addr': n.u32s('reg')[:1],
                              'bus': n.parent.path, 'bus_compat': n.parent.compat(),
                              'bus_reg': [hx(a) for a, _ in n.parent.reg()],
                              'status': n.str('status')})
    out['touch'] = touch
    for t_ in touch:
        rep.append('touch: %s %s addr %s on %s %s status=%s' % (t_['node'], t_['compatible'][:1], t_['addr'], t_['bus_reg'], t_['bus_compat'][:1], t_['status']))
    if not touch:
        problems.append('no touchscreen node recognised — send the dtb so it can be found by hand')

    # chosen
    ch = find(root, lambda n: n.path == '/chosen')
    if ch:
        bootargs = ch[0].str('bootargs') or ''
        out['bootargs'] = bootargs
        for k in ('linux,initrd-start', 'linux,initrd-end'):
            if k in ch[0].props:
                rep.append('%s: %s' % (k, hx(int.from_bytes(ch[0].props[k], 'big'))))

    # pick kernel RAM: biggest bank region above 4G (sweet's --highmem approach), else top of low bank
    hi = [(a, s) for a, s in banks if a >= 0x100000000]
    if hi:
        a, s = max(hi, key=lambda x: x[1])
        overl = [r for r in resv if r[0] < a + s and r[0] + r[1] > a]
        out['xnu_ram'] = [hx(a), hx(s)]
        rep.append('XNU RAM (high bank): %s + %s (%d MiB)%s' % (hx(a), hx(s), s >> 20, ('  WARNING overlaps ' + str([r[2] for r in overl])) if overl else ''))
        if overl:
            problems.append('reserved regions inside the high bank — XNU RAM must be trimmed')
    else:
        problems.append('no memory bank above 4G — XNU RAM has to be chosen by hand')

    ok = fb and ufs
    gen = os.path.join(here, '..', 'shim', 'board_gen.h')
    with open(gen, 'w') as f:
        f.write('#pragma once\n/* GENERATED by tools/flame-extract.py from %s — do not edit */\n' % os.path.basename(sys.argv[1]))
        f.write('#define BOARD_GEN_OK\t\t%d\n' % (1 if ok else 0))
        f.write('#define FB_BASE\t\t\t%sUL\n#define FB_SIZE\t\t\t%sUL\n' % (hx(fb[0] if fb else 0), hx(fb[1] if fb else 0)))
        f.write('#define FB_W\t\t\t%d\n#define FB_H\t\t\t%d\n#define FB_STRIDE\t\t(FB_W * 4)\n' % (w, h))
        f.write('#define UFS_BASE\t\t%sUL\n' % hx(ufs[0] if ufs else 0))
        f.write('#define PSHOLD\t\t\t%sUL\n' % hx(pshold or 0))
        f.write('#define IMEM_RESTART_REASON\t%sUL\n' % hx(imem_rr or 0))
    out.update({'fb': [hx(fb[0]), hx(fb[1])] if fb else None, 'panel': [w, h], 'ufs': [hx(ufs[0]), hx(ufs[1])] if ufs else None,
                'pshold': hx(pshold) if pshold else None, 'imem_restart_reason': hx(imem_rr) if imem_rr else None,
                'model': model, 'compatible': compat})
    json.dump(out, open(os.path.join(here, 'flame_board.json'), 'w'), indent=1)

    print('\n'.join(rep))
    print('\nwrote shim/board_gen.h (BOARD_GEN_OK=%d) and tools/flame_board.json' % (1 if ok else 0))
    if problems:
        print('\nPROBLEMS:')
        for p in problems:
            print('  - ' + p)
    if not pshold:
        print('note: without PS_HOLD the shim cannot reboot by itself; hold Power+Vol- to get back to fastboot')


if __name__ == '__main__':
    main()
