#!/usr/bin/env python3
"""Pack the shim into an Android boot image header v4 for flame (GKI phone).

  python3 tools/flame-mkboot.py --kernel shim/out/Image --ramdisk PAYLOAD_OR_EMPTY \
      --vendor-boot dtb/vendor_boot.img -o shim/out/shim-boot.img

Why v4: flame launched with Android 14/GKI. ABL takes the DTB and the vendor
ramdisk from the FLASHED vendor_boot and loads our ramdisk right behind the
vendor ramdisk. --vendor-boot reads that size so the SWEETXNU payload is padded
to land 4K-aligned (the shim also scans for it). Nothing is ever flashed.
"""
import argparse, struct, sys

PAGE = 4096


def os_version(ver='17.0.0', patch='2035-06'):
    a, b, c = (int(x) for x in ver.split('.'))
    y, m = (int(x) for x in patch.split('-'))
    return ((a << 14 | b << 7 | c) << 11) | ((y - 2000) << 4 | m)


def vendor_ramdisk_size(path):
    h = open(path, 'rb').read(2112)
    if h[:8] != b'VNDRBOOT':
        sys.exit('%s is not a vendor_boot image' % path)
    hv, page = struct.unpack('<II', h[8:16])
    # VNDRBOOT magic, header_version, page_size, kernel_addr, ramdisk_addr, vendor_ramdisk_size
    vrs = struct.unpack('<I', h[24:28])[0]
    return hv, page, vrs


def pad(b, n=PAGE):
    return b + b'\0' * ((-len(b)) % n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--ramdisk', required=True)
    ap.add_argument('--vendor-boot', help="flame's stock vendor_boot.img (for payload alignment)")
    ap.add_argument('--cmdline', default='console=null')
    ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args()

    kernel = open(a.kernel, 'rb').read()
    rd = open(a.ramdisk, 'rb').read()
    if a.vendor_boot:
        hv, page, vrs = vendor_ramdisk_size(a.vendor_boot)
        front = (-vrs) % PAGE
        print('vendor_boot v%d: vendor ramdisk %d bytes -> %d bytes front padding' % (hv, vrs, front))
        rd = b'\0' * front + rd
    if len(a.cmdline) >= 1536:
        sys.exit('cmdline too long')

    hdr = struct.pack('<8sIIII4II1536sI', b'ANDROID!', len(kernel), len(rd), os_version(),
                      1584, 0, 0, 0, 0, 4, a.cmdline.encode(), 0)
    assert len(hdr) == 1584
    img = pad(hdr) + pad(kernel) + pad(rd)
    open(a.out, 'wb').write(img)
    print('wrote %s (%d KiB, kernel %d, ramdisk %d)' % (a.out, len(img) >> 10, len(kernel), len(rd)))


if __name__ == '__main__':
    main()
