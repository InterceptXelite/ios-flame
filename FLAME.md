# ios-flame — iOS 16 on Redmi 14C 5G / POCO M7 5G (`flame`, SM4450)

Fork of [ios-sweet](https://github.com/dipa-studio/ios-sweet) (Redmi Note 10 Pro, SM7150).
Same idea: Apple's open-source XNU kernel + the unmodified iPhone X (D22AP)
iOS 16.7.x userland, started with `fastboot boot` (nothing flashed to boot).

Personal use only. Apple's IPSW files, partition backups and device dumps never go into git or get shared.

## Status

| Stage | What | State |
|---|---|---|
| 0 | Board constants in `shim/board.h` / `board_gen.h`, extractor, v4 boot packer, probe script | done |
| 1 | Flame values filled from the stock vendor DT (via [flame-mainline](https://github.com/i7m7r8/flame-mainline)): splash FB `0xb8000000`, UFS `0x1d84000`, GIC, timer, watchdog | **done** |
| 1 | Shim stops ABL's watchdog (`0x17410000`, ~11 s bark) first thing; clamps XNU RAM to the DRAM bank ABL reports | **done** |
| 1 | `flame-stage1-diag.img` built (diagnostics only, no XNU) | **ready to boot — never tested on a phone** |
| 2 | XNU patches: virtual timer 27, early framebuffer at `0xb8000000` 720x1640 (0004, 0010, 0012) | done |
| 2 | New `0090-flame-scaled-display-and-touch-map.patch`: iPhone X 1125x2436 shrunk 16/25 onto 720x1640 with 40 px bars; touch mapping to match | done, scaler unit-tested on the host |
| 2 | Sweet-only drivers (touch, keys, RPMh, USB) forced off by `tools/flame-build.sh` boot-args | done |
| 2 | Kernel CI build | **needs your GitHub fork** (see below) |
| 3 | XNU reaches the Apple logo / console on flame | todo — first real XNU boot |
| 4 | iOS root image into `userdata` (**erases Android data**) | todo — you download the IPSW |
| 5 | SpringBoard / lock screen | todo |
| 6 | flame drivers: FocalTech FT8726 touch (I2C QUP0 SE1 `0x984000`, addr 0x38, irq gpio13, reset gpio12), volume up (TLMM gpio53), power/vol- (PMK8350 PON over SPMI arb v7 at `0xc400000`) | todo |

Not possible on any iOS-on-Android port today: Wi-Fi, Bluetooth, calls/data, audio, camera, GPU.

## The full run, in order

**Stage 1 — diagnostic boot (safe):**
```
adb reboot bootloader
fastboot boot flame-stage1-diag.img
```
Expect a RED screen with white text (`SHIM ios-flame stage 1`, `LOADER no SWEETXNU payload`, then UFS lines). Photo it.
- Text slanted/garbled → framebuffer stride is wrong (tell me).
- Red shows as blue → pixel format is BGR (tell me).
- Phone reboots after ~11 s with nothing → watchdog not stopped / shim not reached.
- Stuck: hold **Power + Vol-** → fastboot. Reboot = Android, untouched.

**Stage 2 — kernel build (GitHub, free):**
1. Push this folder to a **public** GitHub repo of yours (public = free macOS runner minutes).
2. Actions → `xnu` workflow → Run. Or: `gh workflow run xnu.yml -R YOU/ios-flame`
3. Download artifact `xnu-kernel` into `xnu/out/`.

**Stage 3 — first XNU boot (safe, still nothing flashed):**
```
python3 tools/remotezip.py <iPhone X 16.7.x IPSW URL> DeviceTree.d22ap.im4p research/ipsw-x/dt
ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
cp <backup>/vendor_boot.img dtb/
tools/flame-build.sh xnu/out/kernel.development.sweet 1
fastboot boot flame-ios-boot.img
```
Expect the kernel console on screen, ending at "no root device" (stage 4 not done). Photo it.

**Stage 4+** follows the ios-sweet README (`mkiosroot.sh`, `fastboot flash userdata`) — **wipes Android data, ask first.**

## Differences flame vs sweet (already known)

| Item | sweet (SM7150) | flame (SM4450) | Source |
|---|---|---|---|
| GICD / GICR | 0x17a00000 / 0x17a60000 | **0x17200000 / 0x17260000** | mainline sm4450.dtsi |
| 0x17a00000 on flame | — | **apps_rsc (RPMh)** — writing GIC regs there would break power control | sm4450.dtsi |
| Virtual timer | PPI 3 → INTID 19 | **PPI 11 → INTID 27** | sm4450.dtsi |
| CPUs | 6x A55 + 2x A76 | 6x A55 (cpu0–5) + 2x A78 (cpu6–7), MPIDR Aff1 = core | sm4450.dtsi |
| Boot image | header v2 with DTB | **header v4**, DTB + vendor ramdisk from vendor_boot | GKI |
| Panel | 1080x2400 | **720x1640** LCD | spec |
| Framebuffer, UFS, PS_HOLD, IMEM | fixed | **from live DTB** (flame-extract.py) | phone |
| Touch | Goodix GT9896 on QUP SE7 | **unknown yet** (from DTB / getevent / lsmod) | phone |
| Keys | PM6150 PON + PM6150L GPIO | **unknown yet** (PMIC from DTB) | phone |

## XNU patches that must change for flame (stage 2+)

| Patch | Hard-coded sweet value | flame change |
|---|---|---|
| 0004 machine-config | `SWEET_VTIMER_INTID 19` | 27 |
| 0010 early-framebuffer | FB 0x9c000000, 1080x2400 | flame FB address, 720x1640 |
| 0012 keep-debug-console | FB 0x9c000000 block map, 1080x2400 | same |
| 0046 iosurface-iomfb | dot pitch 395 ppi, 1080x2400 | ~260 ppi, 720x1640 |
| 0061 iphone-x-display-size | crops centre 1080x2400 of 1125x2436 | **must downscale** 1125x2436 → 720x1640 (≈0.64) in the present copy |
| 0060 keys-touch | SPMI 0x0c440000/0x0c40a000/0x0e600000, touch SE 0xa84000, TLMM 0x03900000 | flame SPMI + touch bus from DTB (TLMM is 0x0f100000 on SM4450) |
| 0062 touch-power-battery | RPMh 0x18220000 / cmd-db, PM6150L LDO11 | flame RSC 0x17a00000, flame touch rail |
| 0063 goodix | GT9896 protocol | replace with flame's touch IC protocol |
| 0065 / 0078 smp | A55/A76 layout | same layout works (A78 instead of A76) — verify |
| 0072–0081 usb | sweet USB controller | flame USB (later) |

## How to run stage 1 (safe: nothing is flashed)

1. Windows, phone booted with root: run `windows\flame-probe.bat` from **cmd** (not PowerShell — it corrupts binary output). Send the `flame_probe` folder zipped.
2. In WSL/Linux:
   ```
   cp flame_probe/flame.dtb flame_probe/vendor_boot.img dtb/
   python3 tools/flame-extract.py dtb/flame.dtb      # read the PROBLEMS list!
   sudo apt install gcc-aarch64-linux-gnu python3
   cd shim && ./build.sh                             # -> shim/out/shim-boot.img (diagnostics, no XNU)
   ```
3. Phone:
   ```
   adb reboot bootloader
   fastboot boot shim/out/shim-boot.img
   ```
   Expected: text on screen with CPU features, PSCI version, which cores came up, GIC timer interrupt, UFS registers. **Take a photo** and send it. After ~20–40 s it reboots to fastboot by itself; if it hangs, hold **Power + Vol-** to get back to fastboot. Reboot = Android, untouched.
   If `fastboot boot` answers *unknown command / not allowed*, stop and report — the plan then changes to the inactive slot.

## Boot log (real phone)

| Image | Result | Learned / fixed |
|---|---|---|
| stage1 / 1b | PANIC SYNC, recursive (stage NULL) | APSS watchdog `0x17410000` is **locked from EL1** (sync external abort). Now probed; it is not armed under `fastboot boot` |
| 1c | PANIC SYNC `gic config`, ESR 96000010, FAR 0x17270400 | **GIC is emulated by Gunyah**: only plain `ldr/str [Xn]` (valid ISS). Compiler emitted `str w0,[x1],#4` → abort. Shim MMIO accessors are inline asm now; XNU patch 0004 loop unrolled to fixed offsets |
| 1e | PANIC SYNC `gic vtimer`, ESR 02000000 | **CNTP_* (EL1 physical timer) is UNDEFINED** under Gunyah. Shim no longer touches it; XNU uses the virtual timer |
| **1f** | **PASS** | MMU ok, FB `0xb8000000` ok, PSCI 1.1, GICR `0x17260000`, VTIMER 30 IRQs / 300 ms, SGI 10/10 |
| 1g/1h UFS | commands complete (OCS 0) but every LU: CHECK CONDITION, sense **02/04/02** (all 8 LUNs, even INQUIRY) | ABL leaves the **UFS device in PowerDown** |
| **1i UFS** | **PASS** | START STOP UNIT (ACTIVE) to **W-LUN 0xD0** wakes it. LUN0 = 117 GB, MBR/GPT read; `super` 7.2 GB, `userdata` ~105 GB @ LBA 0x2F6028. Kernel fix: `0091-flame-ufs-wake-device.patch` |

Rules for ALL flame code (shim and XNU drivers): plain MMIO accessors only (GIC is emulated by Gunyah); never CNTP_*; don't rely on the APSS watchdog; wake UFS via W-LUN 0xD0 before any LU command.

**Stage 1 complete (2026-11-11).** Next: stage 2, kernel CI build.
