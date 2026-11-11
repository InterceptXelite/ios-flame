#pragma once
/*
 * flame (Redmi 14C 5G / POCO M7 5G / Redmi 14R 5G) — Qualcomm SM4450, Snapdragon 4 Gen 2
 *
 * Every board-specific constant of the shim lives here. Values known from the
 * mainline Linux sm4450.dtsi are fixed below; the rest (framebuffer, UFS,
 * PS_HOLD, IMEM restart reason) come from the phone's LIVE device tree and are
 * written to board_gen.h by tools/flame-extract.py.
 */
#include "board_gen.h"

/* GICv3 — sm4450.dtsi intc@17200000 (NOT 0x17a00000: on SM4450 that is apps_rsc!) */
#define GICD_BASE		0x17200000UL
#define GICR_BASE		0x17260000UL
#define GICR_SIZE		0x100000UL	/* 8 x 0x20000 */

/* arm,armv8-timer: <PPI 13>, <PPI 14>, <PPI 11>, <PPI 10>  ->  sec-phys, phys, virt, hyp */
#define VTIMER_INTID		27	/* PPI 11 (+16) — sweet used 19 */
#define PTIMER_INTID		30	/* PPI 14 (+16) */
#define TIMEBASE_HZ		19200000

/* APSS watchdog (vendor qcom,msm-watchdog): pet +0x04, enable +0x08 — ABL arms it */
#define WDT_BASE		0x17410000UL

/* CPUs: cpu@0..cpu@500 = 6x Cortex-A55, cpu@600/700 = 2x Cortex-A78, MPIDR Aff1 = core */
#define NR_CPUS			8
#define NR_LITTLE		6
