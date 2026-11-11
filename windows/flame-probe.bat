@echo off
REM ios-flame: collects everything needed to port iOS to flame. Reads only, changes NOTHING.
REM Needs: platform-tools in PATH, phone booted in Android (any ROM), USB debugging on, root (Magisk/KSU).
set D=flame_probe
mkdir %D% 2>nul
adb wait-for-device
echo [1/7] live device tree (most important)
adb exec-out "su -c 'cat /sys/firmware/fdt'" > %D%\flame.dtb
echo [2/7] boot images of the active slot
for %%P in (boot vendor_boot init_boot dtbo) do (
  adb shell "su -c 'S=$(getprop ro.boot.slot_suffix); dd if=/dev/block/by-name/%%P$S of=/sdcard/%%P.img bs=4M 2>/dev/null'"
  adb pull /sdcard/%%P.img %D%\%%P.img >nul 2>&1
  adb shell rm -f /sdcard/%%P.img
)
echo [3/7] memory map
adb exec-out "su -c 'cat /proc/iomem'" > %D%\iomem.txt
adb exec-out "cat /proc/cmdline" > %D%\cmdline.txt
adb exec-out "su -c 'cat /proc/bootconfig'" > %D%\bootconfig.txt 2>nul
echo [4/7] input devices (touch + keys)
adb exec-out "su -c 'getevent -p'" > %D%\getevent.txt
adb exec-out "su -c 'cat /proc/bus/input/devices'" > %D%\input_devices.txt
echo [5/7] loaded drivers
adb exec-out "su -c 'lsmod'" > %D%\lsmod.txt
adb exec-out "su -c 'ls -la /vendor/lib/modules /vendor_dlkm/lib/modules'" > %D%\modules_list.txt 2>nul
echo [6/7] display + partitions + props
adb exec-out "su -c 'cat /sys/class/drm/*/modes; dumpsys SurfaceFlinger --display-id; wm size; wm density'" > %D%\display.txt 2>nul
adb exec-out "su -c 'ls -la /dev/block/by-name; cat /proc/partitions'" > %D%\partitions.txt
adb shell getprop > %D%\props.txt
echo [7/7] kernel log of this boot
adb exec-out "su -c dmesg" > %D%\dmesg.txt
echo.
echo Done. Zip the "%D%" folder and send it.
echo It holds NO personal data (no photos/accounts) - only hardware info and boot images.
pause
