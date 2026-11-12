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
# dtb/vendor_boot.img optional (alignment padding only)

# sweet-only hardware drivers stay OFF on flame until the flame drivers exist:
#   sweettouch    now the flame NT36528A poller (patch 0094), on by default
#   sweetkeys     now the flame driver (patch 0093), on by default
#   sweetusb=0
FLAME_OFF="sweettouch=${TOUCHDRV:-1} sweetkeys=${KEYS:-1} sweetusb=0"   # KEYS=0 to disable the flame key driver (patch 0093)
# MODE=safe (default): READ-ONLY. userdata still holds Android: no ufsrw, no
#   persistent log (sweetlog writes the LAST 4 MiB of userdata). Use until stage 4.
# MODE=ios: after the iOS root is flashed to userdata (stage 4): read-write + log.
MODE="${MODE:-safe}"
if [ "$MODE" = ios ]; then
	RW="ufsrw=1 rootrw=1"
else
	RW="sweetlog=0"
fi
# NOCON=1: stop drawing kernel text over the iOS GUI (panics are still drawn)
[ "${NOCON:-0}" = 1 ] && RW="$RW sweetfb_noconsole=1"
B="debug=0xc serial=3 cpus=$CPUS keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin $RW launchd_unsecure_cache=1 wdt=-1 sweetfb_hz=30 $FLAME_OFF"
echo "mode: $MODE  ($RW)"

mkdir -p shim/out
python3 tools/mkadt.py shim/out/flame.adt --highmem
python3 tools/mkpayload.py "$K" shim/out/flame.adt shim/out/payload.bin --cmdline "$B"
# Touch (stage 6): the shim loads the panel's touch firmware before xnu starts.
# The blob is proprietary (stock flame odm/firmware) and lives only in research/.
FW="${TOUCH_FW:-research/touchfw/novatek_ts_fw_xinli.bin}"
if [ "${TOUCH:-1}" = 1 ] && [ -f "$FW" ]; then
	export TOUCH_FW="$(realpath "$FW")" SHIM_DEFS="${SHIM_DEFS:-} -DFLAME_TOUCH_BOOT"
	echo "touch: shim loads $(basename "$FW")"
else
	unset TOUCH_FW
	echo "touch: no firmware linked (TOUCH=0 or $FW missing)"
fi
(cd shim && ./build.sh ../shim/out/payload.bin)
cp shim/out/shim-boot.img flame-ios-boot.img
echo
echo "flame-ios-boot.img ready. Boot it (temporary, nothing flashed):"
echo "  adb reboot bootloader && fastboot boot flame-ios-boot.img"
echo "Before the iOS root is installed (stage 4) it stops at 'no root device' - that is expected."
