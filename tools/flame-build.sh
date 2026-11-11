#!/bin/bash
# Build the flame boot image that starts iOS:  XNU kernel + Apple device tree + boot-args,
# packed behind the shim.  Nothing is flashed by this script.
#
#   tools/flame-build.sh xnu/out/kernel.development.sweet [cpus]
#
# Needs: tools/flame_board.json, dtb/vendor_boot.img (stock, from your backup),
#        research/ipsw-x/d22ap.adt (iPhone X device tree, see README),
#        aarch64-linux-gnu-gcc, python3.
set -euo pipefail
cd "$(dirname "$0")/.."
K="${1:?usage: tools/flame-build.sh <xnu kernel> [cpus]}"
CPUS="${2:-1}"   # start with 1 core; 6 once the kernel is stable on flame

[ -f tools/flame_board.json ] || { echo "missing tools/flame_board.json"; exit 1; }
[ -f dtb/vendor_boot.img ] || { echo "missing dtb/vendor_boot.img (stock, from your backup)"; exit 1; }

# sweet-only hardware drivers stay OFF on flame until the flame drivers exist:
#   sweettouch=0  (Goodix on sweet's I2C bus + RPMh vote at sweet addresses)
#   sweetkeys=0   (SPMI arb v5 at sweet addresses; flame has arb v7)
#   sweetusb=0
FLAME_OFF="sweettouch=0 sweetkeys=0 sweetusb=0"
B="debug=0xc serial=3 cpus=$CPUS keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30 $FLAME_OFF"

mkdir -p shim/out
python3 tools/mkadt.py shim/out/flame.adt --highmem
python3 tools/mkpayload.py "$K" shim/out/flame.adt shim/out/payload.bin --cmdline "$B"
(cd shim && ./build.sh ../shim/out/payload.bin)
cp shim/out/shim-boot.img flame-ios-boot.img
echo
echo "flame-ios-boot.img ready. Boot it (temporary, nothing flashed):"
echo "  adb reboot bootloader && fastboot boot flame-ios-boot.img"
echo "Before the iOS root is installed (stage 4) it stops at 'no root device' - that is expected."
