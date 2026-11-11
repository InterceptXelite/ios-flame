#pragma once
/*
 * flame board values from the stock vendor device tree (ravelin, board-id 0x600),
 * as documented by github.com/i7m7r8/flame-mainline (docs/hardware-flame.md,
 * dts/sm4450-xiaomi-flame.dts). Re-check against YOUR phone's live DTB with
 * tools/flame-extract.py dtb/flame.dtb (it overwrites this file).
 */
#define BOARD_GEN_OK		1
#define FB_BASE			0xb8000000UL	/* splash_region@b8000000, ABL keeps scanning it */
#define FB_SIZE			0x2b00000UL
#define FB_W			720
#define FB_H			1640
#define FB_STRIDE		(FB_W * 4)	/* PROVISIONAL: confirm on the first diagnostic boot */
#define UFS_BASE		0x1d84000UL	/* ufshc@1d84000 ufs_mem */
#define PSHOLD			0x0UL		/* not in the vendor DT notes: reset goes through PSCI only */
#define IMEM_RESTART_REASON	0x0UL		/* unknown: after reset the phone boots normally, not to fastboot */
