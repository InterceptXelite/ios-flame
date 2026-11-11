#!/bin/bash
# Build the shim for flame and wrap it in a boot.img header v4 (flame is a GKI phone).
# Needs shim/board_gen.h generated from flame's live DTB (tools/flame-extract.py).
set -euo pipefail
cd "$(dirname "$0")"
CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra ${SHIM_DEFS:-}"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever ABL puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# ABL copies the file; pad to image_size so .bss/stacks exist as zeroed bytes.
size=$((16#$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print $1}')))   # no gawk needed
truncate -s "$size" out/Image
# Ramdisk: an xnu payload (tools/mkpayload.py) if given as $1, else an empty
# newc cpio (ABL dislikes a zero-size ramdisk on some builds) -> diagnostics.
if [ -n "${1:-}" ]; then
	cp "$1" out/ramdisk.bin
else
	python3 -c "import gzip,sys; n=b'TRAILER!!!\\0'; h=b'070701'+b'00000000'*4+b'00000001'+b'00000000'*6+b'%08X'%len(n)+b'00000000'; e=h+n; e+=b'\\0'*((-len(e))%4); e+=b'\\0'*((-len(e))%512); sys.stdout.buffer.write(gzip.compress(e,9))" > out/ramdisk.bin   # empty newc cpio, no cpio tool needed
fi
VB=../dtb/vendor_boot.img
# vendor_boot.img is optional: with it the payload is padded to land 4K-aligned;
# without it the shim finds the payload byte by byte and slides it into alignment.
python3 ../tools/flame-mkboot.py --kernel out/Image --ramdisk out/ramdisk.bin $([ -f "$VB" ] && echo --vendor-boot "$VB") -o out/shim-boot.img
# Host render of the console layout.
gcc -O1 -Wall -o out/host_test console.c host_test.c && ./out/host_test
ls -l out/Image out/shim-boot.img
