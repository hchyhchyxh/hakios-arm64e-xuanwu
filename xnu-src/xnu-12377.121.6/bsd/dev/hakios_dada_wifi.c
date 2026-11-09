/*
 * The Xiaomi 15's (dada) Wi-Fi: Qualcomm's "peach" (FastConnect 7900, PCI 17cb:110e) on the SoC's PCIe 0, step by
 * step, from what Android's device tree for dada says (pcie@1c00000 "qcom,pci-msm", cnss "qcom,cnss-peach") and
 * the order Linux brings such a link up in (pcie-qcom.c, phy-qcom-qmp-pcie.c; UniXNU did the same for SM8650's
 * WCN7850: UniXNU-sheng WifiProbe.cpp). Registers of SM8750 from Linux's gcc-sm8750.c and tcsrcc-sm8750.c.
 *
 *   dadawifi=1  a look only: the PCIe 0 power domains and clocks, the pins
 *   dadawifi=2  the link: the chip's supplies through RPMh (the cnss node's vdd-wlan-*), WLAN_EN (gpio16) up, the
 *               controller's and PHY's power domains and clocks, their resets, the PHY's own table (the device
 *               tree's qcom,phy-sequence), the controller's PARF, PERST# (gpio102) released, the link trained
 *               (LTSSM), and the endpoint's configuration space read through the iATU: its vendor and device ID.
 *   dadawifi=3  ... and the firmware: the chip's own power-up (ath12k_pci_power_up), its MSI pointed into our memory,
 *               then the MHI boot (UniXNU's MhiBoot, hakios_mhi_boot.c): the secondary boot loader (amss.bin's first
 *               512 KiB) through BHI, the MHI contexts at READY, M0, the whole image through BHIe, mission mode. The chip
 *               takes 32-bit addresses only: everything it reads or writes is in the loader's memory below 4 GB
 *               (lowdma=, lowdma_mb=), past the sound's first 4 MB, and its SMMU stream (0x1401) passes through.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <kern/kalloc.h>
#include "hakios_mhi_boot.h"

#define say(...) redwood_tl("dwifi: " __VA_ARGS__)
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t hakios_normal_nc_lo, hakios_normal_nc_hi;   /* osfmk/arm/pmap/pmap.c */
extern bool hakios_rpmh_regulator(const char *name, uint32_t mv, bool enable, uint32_t mode);
extern bool hakios_rpmh_level(const char *name, uint32_t level);
extern bool hakios_aop_send(const char *text);

#define GCC_PHYS                0x100000UL
#define GCC_SIZE                0x200000UL
#define TLMM_PHYS               0x0f100000UL
#define TCSR_CLKREF_PHYS        0x0f204000UL    /* tcsrcc "qcom,sun-tcsrcc" at 0xf204008: pcie_0_clkref_en at +0 */
#define PARF_PHYS               0x01c00000UL
#define PHY_PHYS                0x01c06000UL
#define DBI_PHYS                0x40000000UL
#define ATU_PHYS                0x40001000UL
#define CONF_PHYS               0x40100000UL

/* gcc-sm8750.c */
#define GCC_VOTE0               0x52000         /* aggre_noc_pcie_axi 12, qmip_pcie_ahb 11, ddrss_pcie_sf_qtb 19, phy_rchng 22 */
#define GCC_VOTE8               0x52008         /* slv_axi 0, mstr_axi 1, cfg_ahb 2, aux 3, pipe 4, slv_q2a 5, cnoc_pcie_sf 6 */
#define GCC_COLLAPSE            0x5214c         /* PCIe 0 GDSC bit 0, its PHY's bit 2 */
#define GCC_PCIE0_GDSC          0x6b004
#define GCC_PCIE0_PHY_GDSC      0x6c000
#define GCC_PCIE0_BCR           0x6b000
#define GCC_PCIE0_PHY_BCR       0x6c01c
#define GCC_PIPE_MUX            0x6b080         /* 0 the PHY's pipe clock, 2 TCXO */
#define GCC_AUX_RCG             0x6b084
#define GCC_RCHNG_RCG           0x6b068
#define VOTE8_BITS              0x7fu
#define VOTE0_BITS              (1u << 11 | 1u << 12 | 1u << 19 | 1u << 22)

static const struct {
	const char *name;
	uint32_t halt;
} branches[] = {
	{ "slv_axi", 0x6b020 }, { "mstr_axi", 0x6b030 }, { "cfg_ahb", 0x6b040 }, { "aux", 0x6b044 }, { "pipe", 0x6b054 },
	{ "slv_q2a", 0x6b01c }, { "cnoc_sf", 0x10058 }, { "aggre_noc", 0x10068 }, { "ddrss_qtb", 0x1007c }, { "rchng", 0x6b064 },
	{ "qmip_ahb", 0x6b018 },
};
#define NEEDED                  0xfu            /* slv_axi, mstr_axi, cfg_ahb, aux: the controller's registers answer */

/* pcie-qcom.c */
#define PARF_SYS_CTRL           0x00
#define PARF_PM_CTRL            0x20
#define PARF_PHY_CTRL           0x40
#define PARF_MHI_CLOCK_RESET    0x174
#define PARF_AXI_HALT           0x1a8
#define PARF_LTSSM              0x1b0
#define PARF_DBI_BASE           0x350
#define PARF_SLV_SPACE          0x358
#define PARF_ATU_BASE           0x634
#define PARF_DEVICE_TYPE        0x1000
/* pcie-designware.h */
#define DBI_COMMAND             0x04
#define DBI_BUS_NUMBERS         0x18
#define DBI_LINK_CAP            0x7c            /* (the root port's PCIe capability is at 0x70) */
#define DBI_LINK_CONTROL        0x80
#define DBI_LINK_CONTROL2       0xa0
#define DBI_MISC_CONTROL_1      0x8bc           /* bit 0: read-only registers writable */
#define DBI_PORT_DEBUG0         0x728
#define DBI_PORT_DEBUG1         0x72c
/* the PHY (the device tree's qcom,phy-status-offset / -bit) */
#define PHY_STATUS              0x414
#define PHY_STATUS_BIT          (1u << 6)

#define PIN_WLAN_EN             16
#define PIN_SW_CTRL             18
#define PIN_WL_CX               19
#define PIN_PERST               102
#define PIN_CLKREQ              103
#define PIN_WAKE                104

/* pcie0 qcom,phy-sequence of dada's device tree: offset in the PHY (0x1c06000), value, delay (us) */
static const uint32_t phy_sequence[][3] = {
	{ 0x440, 0x3, 0 },
	{ 0xc0, 0x1, 0 },
	{ 0xcc, 0x62, 0 },
	{ 0xd0, 0x2, 0 },
	{ 0x60, 0xf8, 0 },
	{ 0x64, 0x1, 0 },
	{ 0x0, 0x93, 0 },
	{ 0x4, 0x1, 0 },
	{ 0xe0, 0x90, 0 },
	{ 0xe4, 0x82, 0 },
	{ 0xf4, 0x7, 0 },
	{ 0x70, 0x2, 0 },
	{ 0x10, 0x2, 0 },
	{ 0x74, 0x16, 0 },
	{ 0x14, 0x16, 0 },
	{ 0x78, 0x36, 0 },
	{ 0x18, 0x36, 0 },
	{ 0x110, 0x8, 0 },
	{ 0xbc, 0xa, 0 },
	{ 0x120, 0x42, 0 },
	{ 0x80, 0x4, 0 },
	{ 0x84, 0xd, 0 },
	{ 0x20, 0xa, 0 },
	{ 0x24, 0x1a, 0 },
	{ 0x88, 0x41, 0 },
	{ 0x28, 0x34, 0 },
	{ 0x90, 0xab, 0 },
	{ 0x94, 0xaa, 0 },
	{ 0x98, 0x1, 0 },
	{ 0x30, 0x55, 0 },
	{ 0x34, 0x55, 0 },
	{ 0x38, 0x1, 0 },
	{ 0x140, 0x14, 0 },
	{ 0x164, 0x34, 0 },
	{ 0x3c, 0x1, 0 },
	{ 0x1c, 0x4, 0 },
	{ 0x174, 0x16, 0 },
	{ 0x1bc, 0xf, 0 },
	{ 0x170, 0xa0, 0 },
	{ 0x13a4, 0x38, 0 },
	{ 0x12dc, 0x11, 0 },
	{ 0x1360, 0xbf, 0 },
	{ 0x1364, 0xbf, 0 },
	{ 0x1368, 0xb7, 0 },
	{ 0x136c, 0xea, 0 },
	{ 0x135c, 0x3f, 0 },
	{ 0x1374, 0x9, 0 },
	{ 0x1378, 0x49, 0 },
	{ 0x137c, 0x1b, 0 },
	{ 0x1380, 0x8f, 0 },
	{ 0x1370, 0xd1, 0 },
	{ 0x1388, 0x9, 0 },
	{ 0x138c, 0x49, 0 },
	{ 0x1390, 0x1b, 0 },
	{ 0x1394, 0x8f, 0 },
	{ 0x1384, 0xd1, 0 },
	{ 0x12c4, 0x3e, 0 },
	{ 0x12c8, 0x1e, 0 },
	{ 0x12cc, 0xd2, 0 },
	{ 0x1208, 0x9, 0 },
	{ 0x1214, 0x5, 0 },
	{ 0x124c, 0x8, 0 },
	{ 0x1250, 0x8, 0 },
	{ 0x12d8, 0x9, 0 },
	{ 0x1318, 0x1c, 0 },
	{ 0x131c, 0x60, 0 },
	{ 0x12f8, 0x7, 0 },
	{ 0x13f8, 0x8, 0 },
	{ 0x1800, 0x0, 0 },
	{ 0x1084, 0x35, 0 },
	{ 0x108c, 0x10, 0 },
	{ 0x1090, 0x31, 0 },
	{ 0x1094, 0x7f, 0 },
	{ 0x10e4, 0x2, 0 },
	{ 0x1040, 0x8, 0 },
	{ 0x103c, 0x14, 0 },
	{ 0x1ba4, 0x38, 0 },
	{ 0x1adc, 0x11, 0 },
	{ 0x1b60, 0xbf, 0 },
	{ 0x1b64, 0xbf, 0 },
	{ 0x1b68, 0xb7, 0 },
	{ 0x1b6c, 0xea, 0 },
	{ 0x1b5c, 0x3f, 0 },
	{ 0x1b74, 0x9, 0 },
	{ 0x1b78, 0x49, 0 },
	{ 0x1b7c, 0x1b, 0 },
	{ 0x1b80, 0x8f, 0 },
	{ 0x1b70, 0xd1, 0 },
	{ 0x1b88, 0x9, 0 },
	{ 0x1b8c, 0x49, 0 },
	{ 0x1b90, 0x1b, 0 },
	{ 0x1b94, 0x8f, 0 },
	{ 0x1b84, 0xd1, 0 },
	{ 0x1ac4, 0x3e, 0 },
	{ 0x1ac8, 0x1e, 0 },
	{ 0x1acc, 0xd2, 0 },
	{ 0x1a08, 0x9, 0 },
	{ 0x1a14, 0x5, 0 },
	{ 0x1a4c, 0x8, 0 },
	{ 0x1a50, 0x8, 0 },
	{ 0x1ad8, 0x9, 0 },
	{ 0x1b18, 0x1c, 0 },
	{ 0x1b1c, 0x60, 0 },
	{ 0x1af8, 0x7, 0 },
	{ 0x1bf8, 0x8, 0 },
	{ 0x1884, 0x35, 0 },
	{ 0x188c, 0x10, 0 },
	{ 0x1890, 0x31, 0 },
	{ 0x1894, 0x7f, 0 },
	{ 0x18e4, 0x2, 0 },
	{ 0x1840, 0x8, 0 },
	{ 0x183c, 0x14, 0 },
	{ 0x4dc, 0x5, 0 },
	{ 0x588, 0x77, 0 },
	{ 0x598, 0xb, 0 },
	{ 0x8a4, 0x1e, 0 },
	{ 0x8f4, 0x27, 0 },
	{ 0x5e4, 0xf, 0 },
	{ 0x80c, 0x1d, 0 },
	{ 0x814, 0x7, 0 },
	{ 0x820, 0xc1, 0 },
	{ 0x894, 0x0, 0 },
	{ 0x8f8, 0x1f, 0 },
	{ 0x5d0, 0x8c, 0 },
	{ 0x568, 0x17, 0 },
	{ 0x570, 0x2e, 0 },
	{ 0x400, 0x0, 0 },
	{ 0x444, 0x3, 0 },
};

static vm_offset_t gcc, tlmm;

static inline uint32_t
rd(vm_offset_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void
wr(vm_offset_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__builtin_arm_dsb(0xf);
}

static void
us(uint32_t n)
{
	delay_for_interval(n, NSEC_PER_USEC);
}

static void
ms(uint32_t n)
{
	delay_for_interval(n, NSEC_PER_MSEC);
}

/* boot-arg dadawifi_slow=MS: a pause of `times` MS, so that a run whose phone stops answering (the whole SoC, the
 * loader's core too: runs wifi3/wifi7) still has the steps before it in the last live log */
static void
wifi_slow(uint32_t times)
{
	static int32_t slow = -1;

	if (slow < 0 && !PE_parse_boot_argn("dadawifi_slow", &slow, sizeof(slow))) {
		slow = 0;
	}
	if (slow > 0) {
		ms((uint32_t)slow * times);
	}
}

/* TLMM pin: pull [1:0], function [5:2], drive [8:6], output enable 9; eGPIO pads (bit 11) given to us (bit 12) */
static void
pin(unsigned n, uint32_t config, int level)
{
	vm_offset_t cfg = tlmm + 0x1000UL * n;
	uint32_t old = rd(cfg);

	if (old & (1u << 11)) {
		config |= 1u << 12;
	}
	if (level >= 0) {
		wr(cfg + 4, level ? 2 : 0);
	}
	wr(cfg, config);
	if (level >= 0) {
		wr(cfg + 4, level ? 2 : 0);
	}
}

static unsigned
pin_state(unsigned n)
{
	return (rd(tlmm + 0x1000UL * n) & 0x3fff) << 4 | (rd(tlmm + 0x1000UL * n + 4) & 3);
}

static uint32_t
branches_off(void)
{
	uint32_t off = 0;

	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		if (rd(gcc + branches[i].halt) & (1u << 31)) {
			off |= 1u << i;
		}
	}
	return off;
}

static void
look(const char *when)
{
	uint32_t off = branches_off();
	char line[200];
	int at = 0;

	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		if (off & (1u << i)) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %s", branches[i].name);
		}
	}
	line[at] = 0;
	say("%s: GDSCs %#x / %#x, votes %#x %#x, collapse %#x; clocks off:%s; pins WLAN_EN %#x SW_CTRL %#x PERST %#x CLKREQ %#x "
	    "WAKE %#x (config << 4 | io)\n", when, rd(gcc + GCC_PCIE0_GDSC), rd(gcc + GCC_PCIE0_PHY_GDSC), rd(gcc + GCC_VOTE8),
	    rd(gcc + GCC_VOTE0), rd(gcc + GCC_COLLAPSE), at ? line : " none", pin_state(PIN_WLAN_EN), pin_state(PIN_SW_CTRL),
	    pin_state(PIN_PERST), pin_state(PIN_CLKREQ), pin_state(PIN_WAKE));
}

static bool
wait_clear(vm_offset_t reg, uint32_t mask, unsigned tries)
{
	for (unsigned i = 0; i < tries; i++) {
		if (!(rd(reg) & mask)) {
			return true;
		}
		us(100);
	}
	return !(rd(reg) & mask);
}

static void
pulse(uint32_t bcr)
{
	wr(gcc + bcr, rd(gcc + bcr) | 1);
	us(1000);
	wr(gcc + bcr, rd(gcc + bcr) & ~1u);
	us(1000);
}

/*
 * The PMICs through the SPMI arbiter (version 7, as the button driver reads it; writes through the "chnls" channels,
 * Linux's spmi-pmic-arb.c, UniXNU's BootResetCore.h): a peripheral's channel (APID) by its PPID, a register read or
 * written.
 */
#define SPMI_CORE               0x0c400000UL
#define SPMI_CNFG               0x0c42d000UL
#define SPMI_OBS                0x0c440000UL
#define SPMI_CHNLS              0x0c500000UL
static vm_offset_t spmi_core, spmi_cnfg, spmi_obs, spmi_chnls;
static uint32_t spmi_apids;

static int
spmi_apid(uint32_t ppid)
{
	int found = -1;

	if (spmi_core == 0) {
		spmi_core = ml_io_map(SPMI_CORE, 0x3000);
		spmi_cnfg = ml_io_map(SPMI_CNFG, 0x4000);
		spmi_apids = rd(spmi_core + 4) & 0x7ff;
		if (spmi_apids == 0 || spmi_apids > 1024) {
			return -1;
		}
		spmi_obs = ml_io_map(SPMI_OBS, 0x8000);
		spmi_chnls = ml_io_map(SPMI_CHNLS, 0x1000UL * spmi_apids);
	}
	for (uint32_t i = 0; i < spmi_apids; i++) {
		uint32_t map = rd(spmi_core + 0x2000 + 4 * i);

		if (map != 0 && ((map >> 8) & 0xfff) == ppid && (found < 0 || (rd(spmi_cnfg + 4 * i) & 7) == 0)) {
			found = (int)i;
		}
	}
	return found;
}

static int
spmi_wait(vm_offset_t ch)
{
	for (unsigned n = 0; n < 100000; n++) {
		uint32_t st = rd(ch + 8);

		if (st & 1) {
			return (st & 0xe) ? (int)(st & 0xe) : 0;
		}
	}
	return 1;
}

static int
spmi_read8(int apid, uint8_t reg, uint8_t *value)
{
	vm_offset_t ch = spmi_obs + 0x20UL * (uint32_t)apid;
	int rc;

	wr(ch, 1u << 27 | (uint32_t)reg << 4);
	if ((rc = spmi_wait(ch)) == 0) {
		*value = (uint8_t)rd(ch + 0x18);
	}
	return rc;
}

static int
spmi_write8(int apid, uint8_t reg, uint8_t value)
{
	vm_offset_t ch = spmi_chnls + 0x1000UL * (uint32_t)apid;

	wr(ch + 0x10, value);
	wr(ch, (uint32_t)reg << 4);
	return spmi_wait(ch);
}

/* A PMIC GPIO's registers: type/subtype, mode, power source, pull, source select, enable */
static void
pmic_gpio_look(const char *what, int apid)
{
	static const uint8_t regs[] = { 0x04, 0x05, 0x40, 0x41, 0x42, 0x44, 0x46, 0x08 };
	char line[120];
	int at = 0;

	for (unsigned i = 0; i < sizeof(regs); i++) {
		uint8_t v = 0xee;

		(void)spmi_read8(apid, regs[i], &v);
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x=%02x", regs[i], v);
	}
	say("%s (APID %d, owner %u):%s\n", what, apid, rd(spmi_cnfg + 4 * (uint32_t)apid) & 7, line);
}

/*
 * The chip's 32 kHz sleep clock: PMK8550 (SID 0) GPIO3 in function 1 (the device tree's alt_sleep_clk_default:
 * output, no pull, power source 0), set as Linux's pinctrl driver sets it.
 */
static void
sleep_clock(void)
{
	int apid = spmi_apid(0x0ba), rc;

	if (apid < 0) {
		say("no SPMI channel for PMK8550 GPIO3\n");
		return;
	}
	pmic_gpio_look("the sleep clock's pin before", apid);
	rc = spmi_write8(apid, 0x42, 5);
	rc = rc ?: spmi_write8(apid, 0x41, 0);
	rc = rc ?: spmi_write8(apid, 0x44, 2);
	rc = rc ?: spmi_write8(apid, 0x40, 1);
	rc = rc ?: spmi_write8(apid, 0x46, 0x80);
	say("the sleep clock's pin set up: %d\n", rc);
	pmic_gpio_look("the sleep clock's pin after", apid);
}

static void
rails(void)
{
	/*
	 * the cnss node's supplies, lowest voltage of each range (vdd-wlan-io, -io12, -aon, -dig, -rfa1, -rfa2), the SMPS in
	 * whatever mode they are in (AOP's WLAN PDC moves them for Android; DT qcom,pdc_init_table). Not asked: their PWM
	 * mode and vdd-wlan-ant-share (LDO K6) -- with those the firmware died at its first radio work in nearly every run
	 * (drv1..drv13, wifi30, 28 October; boot-arg dadawifi_rails=1 puts them back for trying), without them it scans.
	 */
	static const struct {
		const char *name;
		uint32_t mv, mode, top;         /* top: the range's highest (boot-arg dadawifi_railtop=1) */
	} r[] = {
		{ "ldof3", 1800, 7, 1800 }, { "ldof2", 1200, 7, 1200 }, { "smpd4", 876, 0, 1036 }, { "smpf5", 876, 0, 1000 },
		{ "smpg3", 1860, 0, 2000 }, { "smpi7", 1312, 0, 1340 }, { "ldok6", 1800, 7, 1860 },
	};
	uint32_t top = 0, more = 0, antshare = 0;

	(void)PE_parse_boot_argn("dadawifi_railtop", &top, sizeof(top));
	(void)PE_parse_boot_argn("dadawifi_rails", &more, sizeof(more));
	/*
	 * dadawifi_antshare=1: vdd-wlan-ant-share (LDO K6, 1.8 V, HPM) alone, without the SMPS' PWM mode that came with it in
	 * dadawifi_rails=1. cnss2 turns it on with the chip (DT vdd-wlan-ant-share-supply; the BT node's bt-ant-ldo is the
	 * same LDO): the shared antenna's switch/front end. Off, the firmware's first radio work talks to an unpowered front
	 * end -- one more candidate for its death there (30 October).
	 */
	(void)PE_parse_boot_argn("dadawifi_antshare", &antshare, sizeof(antshare));
	for (unsigned i = 0; i < sizeof(r) / sizeof(r[0]); i++) {
		uint32_t mode = r[i].mode != 0 || !more ? r[i].mode : 7;

		if (strcmp(r[i].name, "ldok6") == 0 && !more && !antshare) {
			continue;
		}
		if (strcmp(r[i].name, "ldok6") == 0) {
			say("rails: the antenna share LDO K6 on (%s)\n", antshare ? "dadawifi_antshare" : "dadawifi_rails");
		}
		if (!hakios_rpmh_regulator(r[i].name, top ? r[i].top : r[i].mv, true, mode)) {
			say("rails: %s not voted\n", r[i].name);
		}
	}
	/*
	 * AOP's WLAN PDC, as cnss2 sets it up at power on (cnss_aop_pdc_reconfig; DT qcom,pdc_init_table): the chip asks
	 * for its radio's supplies through SW_CTRL, and the PDC moves S5F's mode and voltage. Boot-arg dadawifi_pdc=1
	 * sends it (AOP takes it; it did not change the firmware's death at its first radio work: run wifi42 -- but then
	 * the chip's request lines never reached the PDC, see sw_ctrl(); dadawifi_swctrl=1 sends it too).
	 */
	{
		static const char *const pdc[] = {
			"{class: wlan_pdc, ss: rf, res: s5f.m, enable: 1}", "{class: wlan_pdc, ss: rf, res: s5f.v, enable: 1}",
			"{class: wlan_pdc, ss: rf, res: s5f.v, upval: 876}", "{class: wlan_pdc, ss: rf, res: s5f.v, dwnval: 876}",
		};
		static bool sent;
		uint32_t want = 0, swctrl = 0;

		(void)PE_parse_boot_argn("dadawifi_pdc", &want, sizeof(want));
		(void)PE_parse_boot_argn("dadawifi_swctrl", &swctrl, sizeof(swctrl));
		want |= swctrl;
		for (unsigned i = 0; want && !sent && i < sizeof(pdc) / sizeof(pdc[0]); i++) {
			say("WLAN PDC: %s: %s\n", pdc[i], hakios_aop_send(pdc[i]) ? "taken" : "NOT TAKEN");
		}
		/*
		 * dadawifi_pdc=2: and the radio rails' voltages Android's cnss sets after target capability, from the firmware's
		 * OL CPR table (cnss_aop_ol_cpr_cfg_setup; its IPC log at 5.95 s): what the PDC moves s5f/s4d/s7i/s3g to when the
		 * chip asks for its radio through SW_CTRL -- without them, to nothing of the chip's choosing.
		 */
		if (want >= 2 && !sent) {
			static const char *const up[] = {
				"{class: wlan_pdc, ss: rf, res: s5f.v, upval: 814}", "{class: wlan_pdc, ss: bb, res: s4d.v, upval: 822}",
				"{class: wlan_pdc, ss: rf, res: s7i.v, upval: 1314}", "{class: wlan_pdc, ss: rf, res: s3g.v, upval: 1866}",
			};

			for (unsigned i = 0; i < sizeof(up) / sizeof(up[0]); i++) {
				say("WLAN PDC: %s: %s\n", up[i], hakios_aop_send(up[i]) ? "taken" : "NOT TAKEN");
			}
		}
		sent = true;
	}
	/* the chip's crystal clock: RPMh's RF_CLK1 (clk-rpmh "clka1", what the BT node names bt_rf_clk1); and the
	 * link's own reference, ln_bb_clk (clka3), in case */
	(void)hakios_rpmh_regulator("clka1", 0, true, 0);
	(void)hakios_rpmh_regulator("clka3", 0, true, 0);
}

/*
 * The chip's two power-request lines, as cnss2 leaves them: SW_CTRL (gpio18) on its function wcn_sw_ctrl and WL_CX
 * (gpio19) on wcn_sw (DT pinctrl "sw_ctrl" / "sw_ctrl_wl_cx"; pinctrl-sm8750.c function 1 of each), and both given to
 * the PDC (DT mpm_wake_set_gpios = 18 19: TLMM's wakeup enable, intr cfg bit 7 where bit 6 says the pin has a PDC
 * line, pins 110 and 79). Through them the chip asks AOP's WLAN PDC (above) for its radio's supplies when the radio
 * starts. Our boots left both plain GPIO inputs (SW_CTRL 0x11 in every run, passing or not), so the request went
 * nowhere -- and the firmware dies, at random, at exactly its first radio work. Boot-arg dadawifi_swctrl=1.
 */
/*
 * The WLAN-LTE coex UARTs (WCI-2): gpio148/149 coex_uart1 rx/tx, gpio150/151 coex_uart2 rx/tx (pinctrl-sm8750.c). The modem
 * owns them on Android and never runs here, so the SoC's TX pad sits as a pulled-down input: the chip's coex receiver sees a
 * line held low -- a UART break, forever. Boot-arg dadawifi_coexidle=1: both TX pads driven high (the line's idle level), the
 * RX pads pulled up, before WLAN_EN.
 */
static void
coex_idle(void)
{
	uint32_t on = 0;

	if (!PE_parse_boot_argn("dadawifi_coexidle", &on, sizeof(on)) || on == 0) {
		return;
	}
	pin(149, 1u << 9, 1);
	pin(151, 1u << 9, 1);
	pin(148, 3u, -1);
	pin(150, 3u, -1);
	say("coex UARTs idle: tx gpio149/151 high, rx gpio148/150 pulled up\n");
}

static void
sw_ctrl(void)
{
	static const unsigned pins[] = { PIN_SW_CTRL, PIN_WL_CX };
	uint32_t want = 0;

	(void)PE_parse_boot_argn("dadawifi_swctrl", &want, sizeof(want));
	if (!want) {
		return;
	}
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		vm_offset_t cfg = tlmm + 0x1000UL * pins[i];
		uint32_t intr;

		pin(pins[i], (rd(cfg) & ~(0xfu << 2 | 1u << 9 | 1u << 12)) | 1u << 2, -1);   /* pull kept, input, function 1 */
		intr = rd(cfg + 8);
		if (intr & (1u << 6)) {
			wr(cfg + 8, intr | 1u << 7);
		}
		say("gpio%u: config %#x, intr cfg %#x (was %#x)%s\n", pins[i], rd(cfg), rd(cfg + 8), intr,
		    intr & (1u << 6) ? "" : ": no PDC line");
	}
}

static vm_offset_t ep_conf, ep_mhi;     /* the endpoint's configuration space and BAR0 (its MHI registers), once up */
static uint64_t ep_bar_size;

static void
link_up(void)
{
	vm_offset_t parf, phy, dbi, atu, conf, clkref;
	uint32_t off, d0 = 0, d1 = 0;
	unsigned t;

	rails();
	sleep_clock();
	/*
	 * Not asked unless for trying (boot-args): the SoC's CX and MX at a level (dadawifi_cx=256: what msm_pcie asks
	 * for Gen3, DT qcom,bw-scale) and the root port PHY's supplies L1F 0.88 V / L3G 1.2 V in HPM (dadawifi_phyvreg=1,
	 * as Linux's sm8750-mtp.dts). Our vote replaces whatever the boot loader's was on the same RPMh client, and may
	 * be the lower one: with these the firmware died at its first radio work in runs where it had scanned before
	 * (wifi30, wifi31, 28 October).
	 */
	{
		uint32_t level = 0, phyvreg = 0;

		(void)PE_parse_boot_argn("dadawifi_cx", &level, sizeof(level));
		(void)PE_parse_boot_argn("dadawifi_phyvreg", &phyvreg, sizeof(phyvreg));
		if (level != 0) {
			say("CX at %u: %s, MX at %u: %s\n", level, hakios_rpmh_level("cx.lvl", level) ? "voted" : "NOT VOTED",
			    level, hakios_rpmh_level("mx.lvl", level) ? "voted" : "NOT VOTED");
		}
		if (phyvreg != 0) {
			say("PHY supplies: L1F %s, L3G %s\n", hakios_rpmh_regulator("ldof1", 880, true, 7) ? "voted" : "NOT VOTED",
			    hakios_rpmh_regulator("ldog3", 1200, true, 7) ? "voted" : "NOT VOTED");
		}
	}
	/*
	 * The chip's path to memory (Linux interconnect/qcom/sm8750.c): xm_pcie3 -> qns_pcie_mem_noc (BCM SN4) -> gem_noc's
	 * qnm_pcie (SH1, on/off) -> qns_llcc (SH0) -> DDR (MC0, ACV on/off). Android votes it all the time (the PCIe root
	 * port's icc_path, cnss's pcie_to_memnoc and memnoc_to_ddr); here only MC0 and SH0 had a vote (dadaddr, the CPU
	 * driver), so SN4, SH1 and ACV were whatever someone else left: what the firmware pages in from host memory and
	 * the receive buffers it fills cross a link that may be at its floor or off. On by default; boot-arg
	 * dadawifi_icc=0 leaves them alone, dadawifi_icc=N is SN4's vote (AB = IB = N in the BCM's units; 0x1000 as
	 * dadaddr's). Encoding as Linux BCM_TCS_CMD: commit 30, valid 29, AB [27:14], IB [13:0]; an on/off BCM gets its
	 * enable mask (bit 0) in both.
	 */
	{
		extern bool hakios_rpmh_write(const char *name, uint32_t value);
		uint32_t icc = 0x1000, bw, on = 1u << 30 | 1u << 29 | 1u << 14 | 1u;

		(void)PE_parse_boot_argn("dadawifi_icc", &icc, sizeof(icc));
		if (icc != 0) {
			bw = 1u << 30 | 1u << 29 | (icc & 0x3fff) << 14 | (icc & 0x3fff);
			say("the chip's path to memory: SN4 %s (%#x), SH1 %s, ACV %s, SH0 %s, MC0 %s\n",
			    hakios_rpmh_write("SN4", bw) ? "voted" : "NOT VOTED", bw, hakios_rpmh_write("SH1", on) ? "on" : "NOT ON",
			    hakios_rpmh_write("ACV", on) ? "on" : "NOT ON", hakios_rpmh_write("SH0", bw) ? "voted" : "NOT VOTED",
			    hakios_rpmh_write("MC0", bw) ? "voted" : "NOT VOTED");
		}
	}
	ms(5);
	/* PERST# held low, CLKREQ# to its function, the chip restarted: WLAN_EN low, then high (cnss: 50 ms and more) */
	pin(PIN_PERST, 1u | 1u << 9, 0);
	pin(PIN_CLKREQ, 3u | 1u << 2, -1);
	coex_idle();
	sw_ctrl();
	pin(PIN_WLAN_EN, 1u | 7u << 6 | 1u << 9, 0);
	ms(150);
	pin(PIN_WLAN_EN, 1u | 7u << 6 | 1u << 9, 1);
	ms(100);
	look("WLAN_EN up");

	/* the two power domains (votable: a set collapse bit keeps them down) */
	wr(gcc + GCC_COLLAPSE, rd(gcc + GCC_COLLAPSE) & ~(1u << 0 | 1u << 2));
	for (t = 0; t < 100 && (!(rd(gcc + GCC_PCIE0_GDSC) & (1u << 31)) || !(rd(gcc + GCC_PCIE0_PHY_GDSC) & (1u << 31))); t++) {
		us(10);
	}
	if (!(rd(gcc + GCC_PCIE0_GDSC) & (1u << 31)) || !(rd(gcc + GCC_PCIE0_PHY_GDSC) & (1u << 31))) {
		look("the power domains did not come on");
		return;
	}
	/* sources: aux 19.2 MHz TCXO, rchng 100 MHz (GPLL0 even / 3); the pipe mux on TCXO until the PHY runs; then the votes */
	wr(gcc + GCC_AUX_RCG + 4, 0);
	wr(gcc + GCC_AUX_RCG, rd(gcc + GCC_AUX_RCG) | 1);
	wr(gcc + GCC_RCHNG_RCG + 4, 6u << 8 | 5u);
	wr(gcc + GCC_RCHNG_RCG, rd(gcc + GCC_RCHNG_RCG) | 1);
	(void)wait_clear(gcc + GCC_AUX_RCG, 1, 100);
	(void)wait_clear(gcc + GCC_RCHNG_RCG, 1, 100);
	wr(gcc + GCC_PIPE_MUX, (rd(gcc + GCC_PIPE_MUX) & ~3u) | 2);
	wr(gcc + GCC_VOTE8, rd(gcc + GCC_VOTE8) | VOTE8_BITS);
	wr(gcc + GCC_VOTE0, rd(gcc + GCC_VOTE0) | VOTE0_BITS);
	/*
	 * boot-arg dadawifi_noclkgate=1: the chip's way into memory kept clocked -- the hardware clock gating (bit 1 of the
	 * branch register) of aggre_noc_pcie_axi (0x10068, Linux's "noc_aggr") and qmip_pcie_ahb (0x6b018) off. With it on,
	 * both read "off" in every run (look()); the firmware pages its code from host memory through that NOC, and dies at
	 * random at its first radio work (30 October)
	 */
	{
		uint32_t noclkgate = 0;

		(void)PE_parse_boot_argn("dadawifi_noclkgate", &noclkgate, sizeof(noclkgate));
		if (noclkgate) {
			wr(gcc + 0x10068, rd(gcc + 0x10068) & ~2u);
			wr(gcc + 0x6b018, rd(gcc + 0x6b018) & ~2u);
			us(50);
			say("clock gating of aggre_noc_pcie_axi / qmip_pcie_ahb off: %#x %#x\n", rd(gcc + 0x10068), rd(gcc + 0x6b018));
		}
	}
	clkref = ml_io_map(TCSR_CLKREF_PHYS, 0x1000);
	wr(clkref + 8, rd(clkref + 8) | 1);
	us(200);
	say("the PCIe 0 reference clock's switch (TCSR): %#x\n", rd(clkref + 8));
	off = branches_off();
	look("clocks voted");
	if (off & NEEDED) {
		return;         /* the controller's registers would stall the bus */
	}
	pulse(GCC_PCIE0_BCR);
	pulse(GCC_PCIE0_PHY_BCR);

	parf = ml_io_map(PARF_PHYS, 0x3000);
	dbi = ml_io_map(DBI_PHYS, 0x1000);
	wr(parf + PARF_DEVICE_TYPE, 4);                         /* root port */
	wr(parf + PARF_PHY_CTRL, rd(parf + PARF_PHY_CTRL) & ~1u);
	wr(parf + PARF_DBI_BASE, (uint32_t)DBI_PHYS);
	wr(parf + PARF_DBI_BASE + 4, 0);
	wr(parf + PARF_ATU_BASE, (uint32_t)ATU_PHYS);
	wr(parf + PARF_ATU_BASE + 4, 0);
	wr(parf + PARF_SLV_SPACE, 0);
	wr(parf + PARF_SLV_SPACE + 4, 0x80000000u);
	wr(parf + PARF_SYS_CTRL, rd(parf + PARF_SYS_CTRL) & ~(1u << 29));
	wr(parf + PARF_MHI_CLOCK_RESET, rd(parf + PARF_MHI_CLOCK_RESET) | 1u << 4);
	wr(parf + PARF_PM_CTRL, rd(parf + PARF_PM_CTRL) & ~(1u << 5));
	wr(parf + PARF_AXI_HALT, rd(parf + PARF_AXI_HALT) | 1u << 31);

	/* the PHY: its table (power-down control first, start last), the pipe clock from it, ready when PHYSTATUS drops */
	phy = ml_io_map(PHY_PHYS, 0x2000);
	for (unsigned i = 0; i < sizeof(phy_sequence) / sizeof(phy_sequence[0]); i++) {
		if (phy_sequence[i][0] == 0x400 || phy_sequence[i][0] == 0x444) {
			continue;       /* reset and start: after the pipe mux */
		}
		wr(phy + phy_sequence[i][0], phy_sequence[i][1]);
		if (phy_sequence[i][2]) {
			us(phy_sequence[i][2]);
		}
	}
	wr(gcc + GCC_PIPE_MUX, rd(gcc + GCC_PIPE_MUX) & ~3u);
	wr(phy + 0x400, 0);
	wr(phy + 0x444, 3);
	us(1000);
	t = wait_clear(phy + PHY_STATUS, PHY_STATUS_BIT, 200);
	say("the PHY %s: status %#x, pipe clock %s\n", t ? "is ready" : "IS NOT READY", rd(phy + PHY_STATUS),
	    (rd(gcc + 0x6b054) & (1u << 31)) ? "off" : "running");
	if (!t) {
		return;
	}
	say("the root port: %#x\n", rd(dbi));
	wr(dbi + DBI_BUS_NUMBERS, 0x00010100);
	wr(dbi + DBI_COMMAND, rd(dbi + DBI_COMMAND) | 6);
	/*
	 * The link's top speed, boot-arg dadawifi_gen=N (default: as the hardware offers, Gen3). Whenever the chip's
	 * firmware died, the link had lost a lane (x2 -> x1) -- at Gen2 too (run drv9, 28 October), so the speed is not
	 * what does it; the knob stays for trying.
	 */
	{
		uint32_t gen = 0;

		(void)PE_parse_boot_argn("dadawifi_gen", &gen, sizeof(gen));
		if (gen >= 1 && gen <= 3) {
			wr(dbi + DBI_MISC_CONTROL_1, rd(dbi + DBI_MISC_CONTROL_1) | 1);
			wr(dbi + DBI_LINK_CAP, (rd(dbi + DBI_LINK_CAP) & ~0xfu) | gen);
			wr(dbi + DBI_LINK_CONTROL2, (rd(dbi + DBI_LINK_CONTROL2) & ~0xfu) | gen);
			wr(dbi + DBI_MISC_CONTROL_1, rd(dbi + DBI_MISC_CONTROL_1) & ~1u);
		}
		say("the link's top speed: %u (0: the hardware's; link capabilities %#x, control 2 %#x)\n", gen, rd(dbi + DBI_LINK_CAP),
		    rd(dbi + DBI_LINK_CONTROL2));
	}

	/* PERST# low for 100 ms, then high; the LTSSM */
	ms(100);
	pin(PIN_PERST, 1u | 1u << 9, 1);
	us(1000);
	wr(parf + PARF_LTSSM, rd(parf + PARF_LTSSM) | 1u << 8);
	for (t = 0; t < 1000; t++) {
		d0 = rd(dbi + DBI_PORT_DEBUG0);
		d1 = rd(dbi + DBI_PORT_DEBUG1);
		if ((d1 & (1u << 4)) && (d0 & 0x3f) == 0x11) {
			break;
		}
		ms(1);
	}
	say("the link %s after %u ms: LTSSM %#x, debug1 %#x, link control/status %#x\n", t < 1000 ? "IS UP" : "did not come up", t,
	    d0 & 0x3f, d1, rd(dbi + DBI_LINK_CONTROL));
	if (t >= 1000) {
		return;
	}
	/*
	 * The endpoint's stream ID, as Linux's pcie-qcom sets it (qcom_pcie_config_sid_1_9_0, from the DT's iommu-map
	 * <0x0 smmu 0x1400 1>, <0x100 smmu 0x1401 1>): the BDF-to-SID table on, and bus 1 device 0 (BDF 0x100) at the
	 * slot its CRC-8 (polynomial 7, big-endian BDF) names, 0x15, as BDF << 16 | (SID - 0x1400) << 8. Left in its
	 * default bypass the controller tags the chip's DMA with a stream nobody let through, and the chip's first write
	 * after its boot loader started (its first MSI) took the whole SoC down a second later (runs wifi3..wifi11).
	 */
	say("BDF-to-SID: config %#x before\n", rd(parf + 0x2c00));
	for (uint32_t i = 0; i < 256; i++) {
		wr(parf + 0x2000 + 4 * i, 0);
	}
	wr(parf + 0x2c00, rd(parf + 0x2c00) & ~1u);
	wr(parf + 0x2000 + 4 * 0x15, 0x100u << 16 | 1u << 8);
	say("BDF-to-SID: config %#x, slot 0x15 %#x\n", rd(parf + 0x2c00), rd(parf + 0x2000 + 4 * 0x15));
	/* iATU region 0 (unrolled at the ATU base): the config window to type 0 accesses of bus 1 */
	atu = ml_io_map(ATU_PHYS, 0x1000);
	conf = ml_io_map(CONF_PHYS, 0x1000);
	wr(atu + 0x08, (uint32_t)CONF_PHYS);
	wr(atu + 0x0c, 0);
	wr(atu + 0x10, (uint32_t)(CONF_PHYS + 0xfffff));
	wr(atu + 0x20, 0);
	wr(atu + 0x14, 1u << 24);
	wr(atu + 0x18, 0);
	wr(atu + 0x00, 0x4);
	wr(atu + 0x04, 1u << 31);
	us(100);
	say("the endpoint: ID %#x, class %#x, BAR0 %#x\n", rd(conf), rd(conf + 8), rd(conf + 0x10));
	if ((rd(conf) & 0xffff) != 0x17cb) {
		return;
	}
	/*
	 * LTR and L1 substates as Android has them while its Wi-Fi works (config space read from /sys/bus/pci, 30 Oct): both
	 * ends with LTR enabled (device control 2 bit 10), L1SS control 1 with the LTR L1.2 threshold 0x40df.... (scale 2,
	 * value 0xdf) -- the firmware checks it ("Invalid LTR threshold programmed by PCIe RC - PANIC"); ours were left zero.
	 * Boot-arg dadawifi_ltr=1: LTR and the threshold; =2: also ASPM L0s/L1 and the four L1SS enables, as Android's
	 * (endpoint link control 0x43, L1SS control 1 0x40df000f; root port 0x4b / 0x40df460f).
	 */
	{
		uint32_t ltr = 0;

		(void)PE_parse_boot_argn("dadawifi_ltr", &ltr, sizeof(ltr));
		if (ltr != 0) {
			wr(dbi + 0x98, rd(dbi + 0x98) | 1u << 10);                       /* root port: LTR */
			wr(conf + 0x98, rd(conf + 0x98) | 1u << 10);                     /* endpoint: LTR */
			wr(dbi + 0x184, (rd(dbi + 0x184) & 0x0000fff0u) | 0x40df0000u | (ltr >= 2 ? 0xfu : 0));
			wr(conf + 0x204, (rd(conf + 0x204) & 0x0000fff0u) | 0x40df0000u | (ltr >= 2 ? 0xfu : 0));
			if (ltr >= 2) {
				wr(dbi + DBI_LINK_CONTROL, rd(dbi + DBI_LINK_CONTROL) | 3u);
				wr(conf + 0x80, rd(conf + 0x80) | 3u);
			}
			say("LTR/L1SS as Android (dadawifi_ltr=%u): root port devctl2 %#x l1ss %#x lnkctl %#x; endpoint devctl2 %#x "
			    "l1ss %#x lnkctl %#x\n", ltr, rd(dbi + 0x98), rd(dbi + 0x184), rd(dbi + DBI_LINK_CONTROL), rd(conf + 0x98),
			    rd(conf + 0x204), rd(conf + 0x80));
		}
	}
	/*
	 * BAR0 (64-bit memory): sized, placed in the controller's memory window (the device tree's ranges: 0x40300000,
	 * 61 MB) on its own alignment; the root port forwards that range; iATU region 1 passes it through as memory;
	 * the endpoint's memory decoding and bus mastering on. Then what the chip's MHI says of itself.
	 */
	{
		uint64_t mask, size, bar;
		vm_offset_t mhi;
		uint32_t bhi;

		wr(conf + 0x10, 0xffffffffu);
		wr(conf + 0x14, 0xffffffffu);
		mask = (uint64_t)rd(conf + 0x14) << 32 | (rd(conf + 0x10) & ~0xfu);
		size = ~mask + 1;
		bar = (0x40300000ull + size - 1) & ~(size - 1);
		say("BAR0 wants %#llx bytes: at %#llx\n", size, bar);
		if (size == 0 || size > 0x2000000 || bar + size > 0x40300000ull + 0x3d00000ull) {
			return;
		}
		wr(conf + 0x10, (uint32_t)bar | 4);
		wr(conf + 0x14, (uint32_t)(bar >> 32));
		wr(dbi + 0x20, (uint32_t)((bar >> 16) & 0xfff0) | (uint32_t)((bar + size - 1) & 0xfff00000u));
		wr(atu + 0x200 + 0x08, (uint32_t)bar);
		wr(atu + 0x200 + 0x0c, 0);
		wr(atu + 0x200 + 0x10, (uint32_t)(bar + size - 1));
		wr(atu + 0x200 + 0x20, 0);
		wr(atu + 0x200 + 0x14, (uint32_t)bar);
		wr(atu + 0x200 + 0x18, 0);
		wr(atu + 0x200 + 0x00, 0);
		wr(atu + 0x200 + 0x04, 1u << 31);
		us(100);
		wr(conf + 0x04, rd(conf + 0x04) | 6);
		us(100);
		mhi = ml_io_map(bar, size > 0x100000 ? 0x100000 : size);
		ep_conf = conf;
		ep_mhi = mhi;
		ep_bar_size = size > 0x100000 ? 0x100000 : size;
		bhi = rd(mhi + 0x28);
		say("MHI: REGLEN %#x, version %#x, config %#x, BHI at %#x, BHIe at %#x, control %#x, status %#x\n", rd(mhi + 0),
		    rd(mhi + 8), rd(mhi + 0x10), bhi, rd(mhi + 0x2c), rd(mhi + 0x38), rd(mhi + 0x48));
		if (bhi != 0 && bhi < 0x100000 - 0x40) {
			say("BHI: version %#x / %#x, execution environment %u, status %#x, error %#x\n", rd(mhi + bhi), rd(mhi + bhi + 4),
			    rd(mhi + bhi + 0x28), rd(mhi + bhi + 0x2c), rd(mhi + bhi + 0x30));
		}
	}
}

/* ---- dadawifi=3: the firmware ---------------------------------------------------------------------------------- */

/* the apps SMMU: the endpoint's stream (DT pcie0 iommu-map: bus 1 = 0x1401) to the last context bank, untranslating,
 * which is where the sound's streams go too (hakios_dada_audio.c smmu_bypass: the hypervisor lets nobody translate) */
#define APPS_SMMU_PHYS          0x15000000UL
#define WIFI_STREAM             0x1401u

static bool
smmu_pass(uint16_t stream)
{
	vm_offset_t smmu = ml_io_map(APPS_SMMU_PHYS, 0x100000);
	uint32_t idr0, idr1, count, banks, pages, bank, slot = ~0u, cb;

	if (smmu == 0) {
		return false;
	}
	idr0 = rd(smmu + 0x20);
	idr1 = rd(smmu + 0x24);
	count = MIN(idr0 & 0xff, 128u);
	banks = idr1 & 0xff;
	pages = 1u << (((idr1 >> 28) & 7) + 1);
	bank = banks - 1;
	if ((idr1 >> 31) || banks < 2 || pages > 128) {
		return false;
	}
	cb = (pages + bank) * 0x1000;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t smr = rd(smmu + 0x800 + 4 * i), mask = (smr >> 16) & 0x7fff;

		if (!(smr & (1u << 31))) {
			if (slot == ~0u) {
				slot = i;
			}
			continue;
		}
		if (((smr ^ stream) & 0x7fff & ~mask) == 0) {
			say("fw: stream %#x is matched already (%u: %#x -> %#x)\n", stream, i, smr, rd(smmu + 0xc00 + 4 * i));
			return ((rd(smmu + 0xc00 + 4 * i) >> 16) & 3) == 1 ||
			       ((rd(smmu + 0xc00 + 4 * i) & 0xff) == bank && !(rd(smmu + cb) & 1));
		}
	}
	if (slot == ~0u || (rd(smmu + cb) & 1)) {
		return false;
	}
	if (rd(smmu + 0x1000 + 4 * bank) != 0x10000 || rd(smmu + 0x1800 + 4 * bank) != 1) {
		wr(smmu + cb, 0);
		wr(smmu + 0x1800 + 4 * bank, 1);
		wr(smmu + 0x1000 + 4 * bank, 0x10000);
	}
	wr(smmu + 0xc00 + 4 * slot, bank);
	wr(smmu + 0x800 + 4 * slot, (1u << 31) | stream);
	say("fw: stream %#x passes through: match %u, bank %u\n", stream, slot, bank);
	return rd(smmu + 0x800 + 4 * slot) == ((1u << 31) | stream);
}

extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

/* a file of /redwood/fw (build.sh copies ../fw-wifi-dada there), read whole, into memory the caller frees */
static uint8_t *
fw_file(const char *name, uint32_t *size)
{
	char path[96];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;
	const uint8_t *in_image;
	uint64_t image_size = 0;

	/* First from the small image as it lies in memory (redwood_image.c), as the GPU's and the audio DSP's firmware
	 * is: no root and no path needed. Through the file system, a boot without iOS answered ENOENT for every file of
	 * /redwood/fw that init itself could list (runs wifi5/wifi6, 28 October). */
	in_image = redwood_image_file("fw", name, &image_size, NULL);
	if (in_image != NULL && image_size != 0 && image_size <= (32u << 20)) {
		data = kalloc_data((vm_size_t)image_size, Z_WAITOK);
		if (data != NULL) {
			memcpy(data, in_image, (size_t)image_size);
			*size = (uint32_t)image_size;
			return data;
		}
	}

	snprintf(path, sizeof(path), "/redwood/fw/%s", name);
	if (rootvnode == NULLVP || (error = vnode_lookup(path, 0, &vnode, context)) != 0) {
		int first = error;

		snprintf(path, sizeof(path), "/fw/%s", name);
		if (rootvnode == NULLVP || (error = vnode_lookup(path, 0, &vnode, context)) != 0) {
			say("fw: %s: lookup %d, under /fw %d (root %s)\n", name, first, error, rootvnode ? "set" : "NONE");
			return NULL;
		}
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if ((error = vnode_getattr(vnode, &attributes, context)) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > (32u << 20)) {
		say("fw: %s: getattr %d, size %llu\n", path, error, (unsigned long long)attributes.va_data_size);
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		say("fw: %s: no memory for %llu bytes\n", path, (unsigned long long)attributes.va_data_size);
	} else {
		/* in pieces of 1 MB: one read of a whole 7 MB amss20.bin came back short, and a short read used to look
		 * exactly like a missing file */
		for (uint64_t at = 0; at < attributes.va_data_size;) {
			uint64_t piece = MIN(attributes.va_data_size - at, 1u << 20);

			left = 0;
			error = vn_rdwr(UIO_READ, vnode, (caddr_t)data + at, (int)piece, (off_t)at, UIO_SYSSPACE, 0,
			    vfs_context_ucred(context), &left, vfs_context_proc(context));
			if (error != 0 || (uint64_t)left >= piece) {
				say("fw: %s: read at %llu: error %d, %d of %llu left\n", path, (unsigned long long)at, error, left,
				    (unsigned long long)piece);
				kfree_data(data, attributes.va_data_size);
				data = NULL;
				break;
			}
			at += piece - (uint64_t)left;
		}
	}
	vnode_put(vnode);
	*size = (uint32_t)attributes.va_data_size;
	return data;
}

/* DMA memory: the loader's below 4 GB, from 4 MB on, write-combined (nothing of it in a cache); aligned to its own
 * size rounded up to a power of two, as the boot loader wants its image and the firmware its regions (UniXNU arenaAlloc) */
static struct {
	vm_offset_t va;
	uint64_t pa, size, used;
} dma;

static void *
dma_alloc(uint64_t bytes, uint64_t *pa)
{
	uint64_t align = 64, at;

	bytes = (bytes + 4095) & ~4095ull;
	if (bytes >= 4096) {
		align = 4096;
		while (align < bytes) {
			align <<= 1;
		}
	}
	at = ((dma.pa + dma.used + align - 1) & ~(align - 1)) - dma.pa;
	if (at + bytes > dma.size) {
		return NULL;
	}
	dma.used = at + bytes;
	*pa = dma.pa + at;
	bzero((void *)(dma.va + at), (size_t)bytes);
	return (void *)(dma.va + at);
}

/* chip registers above 0x80000 through the window at BAR + 0x310c (ath12k_pci_select_window; UniXNU chipRead) */
static uint32_t window = 0xffffffffu;

static uint32_t
chip_rd(uint32_t off)
{
	if (off < 0x80000) {
		return rd(ep_mhi + off);
	}
	if (((off >> 19) & 0x3f) != window) {
		window = (off >> 19) & 0x3f;
		wr(ep_mhi + 0x310c, 0x40000000u | window);
		(void)rd(ep_mhi + 0x310c);
	}
	return rd(ep_mhi + 0x80000 + (off & 0x7ffff));
}

static void
chip_wr(uint32_t off, uint32_t value)
{
	if (off < 0x80000) {
		wr(ep_mhi + off, value);
		return;
	}
	if (((off >> 19) & 0x3f) != window) {
		window = (off >> 19) & 0x3f;
		wr(ep_mhi + 0x310c, 0x40000000u | window);
		(void)rd(ep_mhi + 0x310c);
	}
	wr(ep_mhi + 0x80000 + (off & 0x7ffff), value);
}

/* ath12k_pci_power_up's register work (UniXNU chipPowerUp): LTSSM, hot reset, interrupts left over, BHIe doorbells cleared, the
 * warm boot marker cleared, the SoC's global reset pulsed, MHI reset */
static void
chip_power_up(void)
{
	uint32_t v = chip_rd(0x1e081b0);

	window = 0xffffffffu;
	for (unsigned i = 0; v != 0x111 && i < 5; i++) {
		if (v == 0xffffffffu) {
			ms(5);
		}
		chip_wr(0x1e081b0, 0x111);
		v = chip_rd(0x1e081b0);
	}
	chip_wr(0x1e40304, chip_rd(0x1e40304) | 0x10);
	ms(5);
	chip_wr(0x1e08228, 0xffffffffu);
	chip_wr(0x1f8031c, chip_rd(0x1f8031c) & ~4u);
	chip_wr(0x360, 0);
	chip_wr(0x368, 0);
	chip_wr(0x394, 0);
	chip_wr(0x39c, 0);
	say("fw: chip: LTSSM %#x, Q6 cookie %#x, warm entry %#x\n", v, chip_rd(0x1f80500), chip_rd(0x1f80504));
	ms(10);
	chip_wr(0x1f80504, 0);
	ms(10);
	(void)chip_rd(0x1f8060c);
	v = chip_rd(0x3008);
	chip_wr(0x3008, v | 1);
	(void)chip_rd(0x3008);
	ms(10);
	chip_wr(0x3008, v & ~1u);
	(void)chip_rd(0x3008);
	ms(10);
	wr(ep_mhi + 0x310c, 0x40000000u | (window == 0xffffffffu ? 0 : window));
	(void)rd(ep_mhi + 0x310c);
	say("fw: chip: global reset pulsed (%#x); MHI status %#x\n", v, rd(ep_mhi + 0x48));
	wr(ep_mhi + 0x38, 2);                           /* MHI reset: clears a SYSERR the global reset leaves */
	ms(10);
	say("fw: chip: after the MHI reset: status %#x, execution environment %u\n", rd(ep_mhi + 0x48),
	    rd(ep_mhi + rd(ep_mhi + 0x28) + 0x28));
}

/* no ASPM while it loads; MSI on, its writes landing in SINK (UniXNU endpointLinkSetup) */
static void
endpoint_setup(uint64_t sink)
{
	uint8_t cap = (uint8_t)(rd(ep_conf + 0x34) & 0xff);

	for (unsigned guard = 0; cap >= 0x40 && guard < 48; guard++) {
		uint32_t w = rd(ep_conf + cap);
		uint8_t id = (uint8_t)w, next = (uint8_t)(w >> 8);

		if (id == 0x10) {
			wr(ep_conf + cap + 0x10, rd(ep_conf + cap + 0x10) & ~3u);
		} else if (id == 0x05) {
			bool is64 = (w >> 16) & 0x80;

			wr(ep_conf + cap + 4, (uint32_t)sink);
			if (is64) {
				wr(ep_conf + cap + 8, (uint32_t)(sink >> 32));
				wr(ep_conf + cap + 0xc, 0);
			} else {
				wr(ep_conf + cap + 8, 0);
			}
			wr(ep_conf + cap, (w & 0xffff) | ((((w >> 16) & ~0x70u) | 0x20u | 1u) << 16));
		}
		cap = next;
	}
}

/* the device's context arrays (mhi_init_dev_ctxt; UniXNU programContexts): 128 channels off, two event rings, the command
 * ring, and the registers that point at them */
#define CTX                     44u
#define CHANNELS                128u
#define EVENT_RINGS             2u
#define ELEMENT                 16u
static uint8_t *ctx_va, *ev_va[EVENT_RINGS];

static void
contexts(uint64_t pa)
{
	static const uint32_t ev_len[EVENT_RINGS] = { 32 * ELEMENT, 256 * ELEMENT };
	uint32_t ev_off = CHANNELS * CTX, cmd_off = ev_off + EVENT_RINGS * CTX, cur = (cmd_off + CTX + 4095) & ~4095u;
	uint64_t ev_base[EVENT_RINGS], cmd_base, v64;
	uint32_t erdb, v32;

#define W32(at, v) do { v32 = (v); memcpy(ctx_va + (at), &v32, 4); } while (0)
#define W64(at, v) do { v64 = (v); memcpy(ctx_va + (at), &v64, 8); } while (0)
	for (uint32_t i = 0; i < EVENT_RINGS; i++) {
		uint32_t at = ev_off + i * CTX;

		ev_base[i] = pa + cur;
		ev_va[i] = ctx_va + cur;
		W32(at, i == 0 ? 0 : 1u << 16);
		W32(at + 4, 1);
		W32(at + 8, i + 1);
		W64(at + 12, ev_base[i]);
		W64(at + 20, ev_len[i]);
		W64(at + 28, ev_base[i]);
		W64(at + 36, ev_base[i] + ev_len[i] - ELEMENT);
		cur += ev_len[i];
	}
	cmd_base = pa + cur;
	W64(cmd_off + 12, cmd_base);
	W64(cmd_off + 20, 8 * ELEMENT);
	W64(cmd_off + 28, cmd_base);
	W64(cmd_off + 36, cmd_base);
#undef W32
#undef W64
	__builtin_arm_dsb(0xf);
	wr(ep_mhi + 0x58, (uint32_t)pa);
	wr(ep_mhi + 0x5c, (uint32_t)(pa >> 32));
	wr(ep_mhi + 0x60, (uint32_t)(pa + ev_off));
	wr(ep_mhi + 0x64, (uint32_t)((pa + ev_off) >> 32));
	wr(ep_mhi + 0x68, (uint32_t)(pa + cmd_off));
	wr(ep_mhi + 0x6c, (uint32_t)((pa + cmd_off) >> 32));
	wr(ep_mhi + 0x80, 0);
	wr(ep_mhi + 0x84, 0);
	wr(ep_mhi + 0x88, 0xffffffffu);
	wr(ep_mhi + 0x8c, 0);
	wr(ep_mhi + 0x98, 0);
	wr(ep_mhi + 0x9c, 0);
	wr(ep_mhi + 0xa0, 0xffffffffu);
	wr(ep_mhi + 0xa4, 0);
	wr(ep_mhi + 0x10, (rd(ep_mhi + 0x10) & ~0xffff0000u) | EVENT_RINGS << 16);
	erdb = rd(ep_mhi + 0x20);
	for (uint32_t i = 0; erdb + 8 * EVENT_RINGS <= ep_bar_size && i < EVENT_RINGS; i++) {
		wr(ep_mhi + erdb + 8 * i, (uint32_t)(ev_base[i] + ev_len[i] - ELEMENT));
		wr(ep_mhi + erdb + 8 * i + 4, (uint32_t)((ev_base[i] + ev_len[i] - ELEMENT) >> 32));
	}
}

/* the host's half of the event rings: what the device posted is taken, the ring given back (UniXNU eventAck) */
static unsigned
events_ack(void)
{
	static uint64_t acked[EVENT_RINGS];
	uint32_t erdb = rd(ep_mhi + 0x20);
	unsigned moved = 0;

	for (unsigned r = 0; r < EVENT_RINGS; r++) {
		uint8_t *c = ctx_va + CHANNELS * CTX + r * CTX;
		uint64_t base, len, rp, wp;

		memcpy(&base, c + 12, 8);
		memcpy(&len, c + 20, 8);
		memcpy(&rp, c + 28, 8);
		if (rp == acked[r] || rp < base || rp >= base + len) {
			continue;
		}
		acked[r] = rp;
		wp = rp == base ? base + len - ELEMENT : rp - ELEMENT;
		memcpy(c + 36, &wp, 8);
		__builtin_arm_dsb(0xf);
		wr(ep_mhi + erdb + 8 * r, (uint32_t)wp);
		wr(ep_mhi + erdb + 8 * r + 4, (uint32_t)(wp >> 32));
		moved++;
	}
	return moved;
}

static uint32_t
mhi_read(void *c __unused, uint32_t reg)
{
	return rd(ep_mhi + reg);
}

static void
mhi_write(void *c __unused, uint32_t reg, uint32_t value)
{
	wr(ep_mhi + reg, value);
}

#define SEGMENT                 (512u << 10)
#define SBL_BYTES               (512u << 10)
#define MAX_SEGMENTS            32u

static void
firmware(void)
{
	static const char *const phases[] = { "idle", "resetting", "loading the boot loader", "waiting for READY",
		                              "contexts", "waiting for the boot loader", "loading the image",
		                              "waiting for mission mode", "done", "failed" };
	uint64_t low = 0, mb = 0, seg_pa[MAX_SEGMENTS], table_pa, ctx_pa, sink_pa;
	uint8_t *image, *seg[MAX_SEGMENTS];
	uint32_t size = 0, segments, entries, which = 1, phase, last, since = 0, acks = 0;
	MhiBhieEntry *table;
	MhiBoot boot;
	MhiIo io = { mhi_read, mhi_write, NULL, 0 };

	if (ep_mhi == 0 || ep_conf == 0) {
		return;
	}
	if (!PE_parse_boot_argn("lowdma", &low, sizeof(low)) || !PE_parse_boot_argn("lowdma_mb", &mb, sizeof(mb)) || mb < 16 ||
	    low == 0 || low + (mb << 20) > 0x100000000ULL) {
		say("fw: no 32 MB below 4 GB from the loader (lowdma=, lowdma_mb=): a loader from before 27 October\n");
		return;
	}
	dma.pa = low + (4u << 20);
	dma.size = (mb - 4) << 20;
	dma.va = ml_io_map_wcomb((vm_offset_t)dma.pa, (vm_size_t)dma.size);
	if (dma.va == 0) {
		return;
	}
	(void)PE_parse_boot_argn("dadawifi_fw", &which, sizeof(which));
	image = fw_file(which == 2 ? "amss20.bin" : "amss.bin", &size);
	if (image == NULL) {
		say("fw: no /redwood/fw/%s\n", which == 2 ? "amss20.bin" : "amss.bin");
		return;
	}
	segments = (size + SEGMENT - 1) / SEGMENT;
	if (segments > MAX_SEGMENTS) {
		kfree_data(image, size);
		return;
	}
	for (uint32_t i = 0; i < segments; i++) {
		seg[i] = dma_alloc(SEGMENT, &seg_pa[i]);
		if (seg[i] == NULL) {
			say("fw: no DMA memory for segment %u\n", i);
			kfree_data(image, size);
			return;
		}
		memcpy(seg[i], image + i * SEGMENT, MIN(SEGMENT, size - i * SEGMENT));
	}
	kfree_data(image, size);
	table = dma_alloc(4096, &table_pa);
	ctx_va = dma_alloc(32 << 10, &ctx_pa);
	if (table == NULL || ctx_va == NULL || dma_alloc(4096, &sink_pa) == NULL) {
		return;
	}
	entries = mhi_bhie_table(table, 4096 / sizeof(MhiBhieEntry), seg_pa, size, SEGMENT);
	__builtin_arm_dsb(0xf);
	say("fw: %s, %u bytes in %u segments at %#llx..; table %#llx, contexts %#llx\n", which == 2 ? "amss20.bin" : "amss.bin",
	    size, segments, seg_pa[0], table_pa, ctx_pa);
	if (!smmu_pass(WIFI_STREAM)) {
		say("fw: the endpoint's SMMU stream could not be passed through\n");
		return;
	}
	chip_power_up();
	endpoint_setup(sink_pa);
	if (mhi_boot_open(&boot, &io) != 0) {
		say("fw: MHI registers out of shape (BHI %#x, BHIe %#x)\n", rd(ep_mhi + 0x28), rd(ep_mhi + 0x2c));
		return;
	}
	contexts(ctx_pa);
	phase = mhi_boot_begin(&boot, 0, seg_pa[0], MIN(SBL_BYTES, size), table_pa, entries * (uint32_t)sizeof(MhiBhieEntry),
	    0x5eed);
	say("fw: the boot loader asked for (execution environment %u, state %u): %s\n", boot.ee, boot.state, phases[phase]);
	last = phase;
	for (unsigned i = 0; i < 8000 && phase != MhiDone && phase != MhiFailed; i++) {
		if (phase == MhiNeedContexts) {
			contexts(ctx_pa);       /* (the boot loader clears the MHI registers when it starts: they go in again) */
			phase = mhi_boot_contexts_done(&boot);
		} else {
			phase = mhi_boot_step(&boot);
		}
		if (phase != MhiNeedContexts && phase != MhiDone && phase != MhiFailed) {
			acks += events_ack();
		}
		if (phase != last) {
			say("fw: %s (execution environment %u, state %u, status %#x)\n", phases[phase], boot.ee, boot.state,
			    boot.last_status);
			last = phase;
			since = 0;
		} else if (++since > 4000) {
			phase = mhi_boot_timeout(&boot);
		}
		ms(5);
	}
	say("fw: the end: %s (error %u in %s; execution environment %u, state %u, BHI status %#x error %#x; %u event rings given "
	    "back)\n", phases[MIN(phase, 9u)], boot.error, phases[MIN(boot.failed_phase, 9u)], mhi_boot_ee(&boot),
	    mhi_boot_state(&boot), rd(ep_mhi + boot.bhi + 0x2c), rd(ep_mhi + boot.bhi + 0x30), acks);
}

/*
 * Bluetooth (boot-arg dadabt=1): the same chip's BT part speaks HCI over a 4-wire UART, QUP wrapper 2's SE6
 * ("qupv3_se14_4uart" at 0x898000; Android's /dev/ttyHS0): gpio24 CTS, gpio25 RFR, gpio26 TX, gpio27 RX in
 * function 1. Step 1: the wrapper's clocks on, the SE's protocol firmware and interfaces looked at; BT_EN
 * (PM8550VE, SID 5, GPIO3) up.
 */
#define QUP2_SE6_PHYS           0x00898000UL
#define GCC_QUP_VOTE            0x52010         /* wrap2: core 0, s_ahb 1, m_ahb 2, core_2x 3, s6 10 */
#define GCC_QUP2_S6_RCG         0x1e77c

static void
bt_probe(void)
{
	vm_offset_t se;
	int apid;

	wr(gcc + GCC_QUP_VOTE, rd(gcc + GCC_QUP_VOTE) | 1u << 0 | 1u << 1 | 1u << 2 | 1u << 3 | 1u << 10);
	us(100);
	say("bt: QUP wrapper 2 clocks: core %s, s_ahb %s, m_ahb %s, s6 %s; s6 source CFG %#x\n",
	    (rd(gcc + 0x232a0) & (1u << 31)) ? "off" : "on", (rd(gcc + 0x2329c) & (1u << 31)) ? "off" : "on",
	    (rd(gcc + 0x23298) & (1u << 31)) ? "off" : "on", (rd(gcc + 0x1e76c) & (1u << 31)) ? "off" : "on",
	    rd(gcc + GCC_QUP2_S6_RCG + 4));
	if (rd(gcc + 0x2329c) & (1u << 31)) {
		return;
	}
	se = ml_io_map(QUP2_SE6_PHYS, 0x4000);
	say("bt: SE14: firmware %#x / %#x (protocol %u), interfaces disabled %#x, status %#x, HW params %#x %#x, "
	    "DMA %#x, output control %#x, clock config %#x\n", rd(se + 0x68), rd(se + 0x6c), (rd(se + 0x68) >> 8) & 0xff,
	    rd(se + 0x64), rd(se + 0x40), rd(se + 0xe24), rd(se + 0xe28), rd(se + 0xc04), rd(se + 0x24), rd(se + 0x48));
	apid = spmi_apid(0x58a);
	if (apid >= 0) {
		int rc;

		pmic_gpio_look("bt: BT_EN before", apid);
		rc = spmi_write8(apid, 0x40, 1);                /* digital output */
		rc = rc ?: spmi_write8(apid, 0x44, 0x80);       /* source: constant, inverted = high */
		rc = rc ?: spmi_write8(apid, 0x46, 0x80);
		say("bt: BT_EN up: %d\n", rc);
		pmic_gpio_look("bt: BT_EN after", apid);
	} else {
		say("bt: no SPMI channel for PM8550VE GPIO3\n");
	}
}

/*
 * Bluetooth, step 2 (boot-arg dadabt=2): the UART itself and the chip's first answers. The serial engine runs the UART
 * protocol already (its firmware: protocol 2); its clock is moved to 7.3728 MHz (GCC's table for wrap2_s6: GPLL0's even
 * output, 300 MHz, through M/N = 384/15625), which divides to 115200 baud exactly; the engine is set up the way Linux's
 * qcom_geni_serial does (packing, FIFO mode, 8N1, hardware flow control, the divider); then an HCI Reset and Qualcomm's
 * version request, and whatever comes back said byte by byte.
 */
#define SE_GENI_FORCE_DEFAULT   0x20
#define SE_GENI_OUTPUT_CTRL     0x24
#define SE_GENI_CGC_CTRL        0x28
#define SE_GENI_M_CLK_CFG       0x48
#define SE_GENI_S_CLK_CFG       0x4c
#define SE_GENI_CLK_SEL         0x7c
#define SE_GENI_BYTE_GRAN       0x254
#define SE_GENI_DMA_MODE        0x258
#define SE_GENI_TX_PACK0        0x260
#define SE_GENI_TX_PACK1        0x264
#define SE_GENI_RX_PACK0        0x284
#define SE_GENI_RX_PACK1        0x288
#define SE_UART_LOOPBACK        0x22c
#define SE_UART_TX_TRANS_CFG    0x25c
#define SE_UART_TX_WORD_LEN     0x268
#define SE_UART_TX_STOP_BITS    0x26c
#define SE_UART_TX_TRANS_LEN    0x270
#define SE_UART_RX_TRANS_CFG    0x280
#define SE_UART_RX_WORD_LEN     0x28c
#define SE_UART_RX_STALE        0x294
#define SE_UART_TX_PARITY       0x2a4
#define SE_UART_RX_PARITY       0x2a8
#define SE_GENI_M_CMD0          0x600
#define SE_GENI_M_IRQ_STATUS    0x610
#define SE_GENI_M_IRQ_CLEAR     0x618
#define SE_GENI_S_CMD0          0x630
#define SE_GENI_S_IRQ_STATUS    0x640
#define SE_GENI_S_IRQ_CLEAR     0x648
#define SE_GENI_TX_FIFO         0x700
#define SE_GENI_RX_FIFO         0x780
#define SE_GENI_RX_FIFO_STATUS  0x804
#define SE_GENI_RX_WATERMARK    0x810
#define SE_GENI_RX_RFR_WM       0x814
#define SE_GSI_EVENT_EN         0xe18
#define SE_IRQ_EN               0xe1c
#define SE_DMA_GENERAL_CFG      0xe30

static vm_offset_t bt_se;

/* wrap2_s6's clock source at 7.3728 MHz (clk-rcg2.c __clk_rcg2_configure_mnd, F(7372800, GPLL0_OUT_EVEN, 1, 384, 15625)) */
static bool
bt_clock(void)
{
	vm_offset_t r = gcc + GCC_QUP2_S6_RCG;
	uint32_t m = 384, n = 15625, d = n, mask = 0xffff;

	wr(r + 8, m & mask);
	wr(r + 0xc, ~(n - m) & mask);
	d = d < m ? m : d > 2 * (n - m) ? 2 * (n - m) : d;
	wr(r + 0x10, ~d & mask);
	wr(r + 4, (rd(r + 4) & (1u << 20)) | 1u | 6u << 8 | 2u << 12);
	wr(r, rd(r) | 1);
	for (int i = 0; i < 1000 && (rd(r) & 1); i++) {
		us(1);
	}
	say("bt: s6 clock source: CFG %#x M %#x N %#x D %#x (update %s)\n", rd(r + 4), rd(r + 8), rd(r + 0xc), rd(r + 0x10),
	    (rd(r) & 1) ? "pending" : "taken");
	return !(rd(r) & 1);
}

/* geni_se_config_packing(8 bits, 4 a word, LSB first): FIFO bytes as they come */
static void
bt_packing(void)
{
	uint32_t cfg[4];

	for (unsigned i = 0; i < 4; i++) {
		cfg[i] = (8 * i) << 5 | 7u << 1;
	}
	cfg[3] |= 1;
	wr(bt_se + SE_GENI_TX_PACK0, cfg[0] | cfg[1] << 10);
	wr(bt_se + SE_GENI_TX_PACK1, cfg[2] | cfg[3] << 10);
	wr(bt_se + SE_GENI_RX_PACK0, cfg[0] | cfg[1] << 10);
	wr(bt_se + SE_GENI_RX_PACK1, cfg[2] | cfg[3] << 10);
	wr(bt_se + SE_GENI_BYTE_GRAN, 0);
}

static void
bt_uart(uint32_t divider)
{
	/* geni_se_init: interrupts cleared, clock gating and IO defaults, FIFO (not DMA) mode, watermarks */
	wr(bt_se + SE_GSI_EVENT_EN, 0);
	wr(bt_se + SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(bt_se + SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
	wr(bt_se + SE_GENI_CGC_CTRL, rd(bt_se + SE_GENI_CGC_CTRL) | 0x7f);
	wr(bt_se + SE_DMA_GENERAL_CFG, rd(bt_se + SE_DMA_GENERAL_CFG) | 0xf);
	wr(bt_se + SE_GENI_OUTPUT_CTRL, 0x7f);
	wr(bt_se + SE_GENI_FORCE_DEFAULT, 1);
	wr(bt_se + SE_IRQ_EN, rd(bt_se + SE_IRQ_EN) | 0xf);
	wr(bt_se + SE_GENI_DMA_MODE, rd(bt_se + SE_GENI_DMA_MODE) & ~1u);
	bt_packing();
	wr(bt_se + SE_GENI_RX_WATERMARK, 2);
	wr(bt_se + SE_GENI_RX_RFR_WM, 16 - 2);
	/* set_termios: 8N1, the chip's CTS honoured (flow control), a stale count, the clock divider */
	wr(bt_se + SE_UART_LOOPBACK, 0);
	wr(bt_se + SE_UART_TX_TRANS_CFG, 0);
	wr(bt_se + SE_UART_TX_PARITY, 0);
	wr(bt_se + SE_UART_RX_TRANS_CFG, 0);
	wr(bt_se + SE_UART_RX_PARITY, 0);
	wr(bt_se + SE_UART_TX_WORD_LEN, 8);
	wr(bt_se + SE_UART_RX_WORD_LEN, 8);
	wr(bt_se + SE_UART_TX_STOP_BITS, 0);
	wr(bt_se + SE_UART_RX_STALE, 10 * 16);
	wr(bt_se + SE_GENI_CLK_SEL, 0);
	wr(bt_se + SE_GENI_M_CLK_CFG, divider << 4 | 1);
	wr(bt_se + SE_GENI_S_CLK_CFG, divider << 4 | 1);
	/* the receiver, for good (UART_START_READ) */
	wr(bt_se + SE_GENI_S_CMD0, 1u << 27);
}

/* the chip's bytes, kept between reads: an event may come in pieces, or several in one read */
static uint8_t bt_rx[2048];
static uint32_t bt_rx_len, bt_rx_lost;

/* whatever the RX FIFO holds, into bt_rx; how many bytes came */
static uint32_t
bt_pull(void)
{
	uint32_t st = rd(bt_se + SE_GENI_RX_FIFO_STATUS), words = st & 0x1ffffff, got = 0;

	for (uint32_t i = 0; i < words; i++) {
		uint32_t w = rd(bt_se + SE_GENI_RX_FIFO), bytes = 4;

		if (i + 1 == words && (st & (1u << 31))) {
			bytes = (st >> 28) & 7;
			bytes = bytes ? bytes : 4;
		}
		for (uint32_t k = 0; k < bytes; k++) {
			if (bt_rx_len < sizeof(bt_rx)) {
				bt_rx[bt_rx_len++] = (uint8_t)(w >> (8 * k));
			} else {
				bt_rx_lost++;
			}
			got++;
		}
	}
	if (rd(bt_se + SE_GENI_S_IRQ_STATUS) & 1) {     /* the read command ended (stale): again */
		wr(bt_se + SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
		wr(bt_se + SE_GENI_S_CMD0, 1u << 27);
	}
	return got;
}

static bool
bt_send(const uint8_t *p, uint32_t n)
{
	uint32_t depth = (rd(bt_se + 0xe24) >> 16) & 0x3f;

	depth = depth ? depth : 16;
	wr(bt_se + SE_GENI_M_IRQ_CLEAR, 0xffffffffu);
	wr(bt_se + SE_UART_TX_TRANS_LEN, n);
	wr(bt_se + SE_GENI_M_CMD0, 1u << 27);
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t w = 0;

		/* room in the FIFO (the chip's CTS may hold it); meanwhile what the chip says is kept */
		for (int t = 0; (rd(bt_se + 0x800) & 0x0fffffffu) + 1 >= depth; t++) {
			if (t > 20000) {
				say("bt: TX FIFO stuck at %u words (CTS?)\n", rd(bt_se + 0x800) & 0x0fffffffu);
				return false;
			}
			(void)bt_pull();
			us(50);
		}
		for (uint32_t k = 0; k < 4 && i + k < n; k++) {
			w |= (uint32_t)p[i + k] << (8 * k);
		}
		wr(bt_se + SE_GENI_TX_FIFO, w);
	}
	for (int t = 0; t < 2000; t++) {
		if (rd(bt_se + SE_GENI_M_IRQ_STATUS) & 1) {
			wr(bt_se + SE_GENI_M_IRQ_CLEAR, 1);
			return true;
		}
		(void)bt_pull();
		us(50);
	}
	return false;
}

static void
bt_say(const char *what, const uint8_t *p, uint32_t n)
{
	char line[200];
	int at = 0;

	for (uint32_t i = 0; i < n && at < (int)sizeof(line) - 4; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", p[i]);
	}
	line[at] = 0;
	say("bt: %s: %u bytes:%s\n", what, n, line);
}

/* the next HCI event (H4: 04, code, length, parameters) into ev: its whole length, 0 when none came in wait_ms. Bytes
 * that are no event (the chip's in-band sleep bytes FC/FD/FE, noise) are dropped and said. */
static uint32_t
bt_event(uint8_t *ev, uint32_t max, uint32_t wait_ms)
{
	uint64_t end = mach_absolute_time(), now;
	uint32_t dropped = 0;

	nanoseconds_to_absolutetime((uint64_t)wait_ms * NSEC_PER_MSEC, &now);
	end += now;
	for (;;) {
		uint32_t skip = 0;

		while (skip < bt_rx_len && bt_rx[skip] != 0x04) {
			skip++;
		}
		if (skip) {
			if (dropped < 8) {
				bt_say("not an event, dropped", bt_rx, skip < 16 ? skip : 16);
			}
			dropped++;
			memmove(bt_rx, bt_rx + skip, bt_rx_len - skip);
			bt_rx_len -= skip;
		}
		if (bt_rx_len >= 3 && bt_rx_len >= 3u + bt_rx[2]) {
			uint32_t n = 3u + bt_rx[2];

			memcpy(ev, bt_rx, n < max ? n : max);
			memmove(bt_rx, bt_rx + n, bt_rx_len - n);
			bt_rx_len -= n;
			return n < max ? n : max;
		}
		if (mach_absolute_time() > end) {
			return 0;
		}
		if (bt_pull() == 0) {
			us(100);
		}
	}
}

/*
 * An HCI command (H4 01, opcode, length, parameters). With an answer wanted: the events until its Command Complete
 * (or Command Status) come, others said; the complete event in ev, its length (0: none in time). Qualcomm's chips
 * (WCN3991 on) answer their vendor commands (0xfc00) with Command Complete: ev[6] on are its return parameters.
 */
static uint32_t
bt_cmd(uint16_t op, const uint8_t *param, uint32_t n, uint8_t *ev, uint32_t max, uint32_t wait_ms)
{
	uint8_t packet[4 + 255];
	uint32_t got;

	packet[0] = 0x01;
	packet[1] = (uint8_t)op;
	packet[2] = (uint8_t)(op >> 8);
	packet[3] = (uint8_t)n;
	memcpy(packet + 4, param, n);
	if (!bt_send(packet, 4 + n)) {
		say("bt: command %#x not taken by the UART\n", op);
		return 0;
	}
	if (wait_ms == 0) {
		return 0;
	}
	for (int others = 0; others < 16; others++) {
		got = bt_event(ev, max, wait_ms);
		if (got == 0) {
			say("bt: command %#x: no answer in %u ms\n", op, wait_ms);
			return 0;
		}
		if ((ev[1] == 0x0e && got >= 6 && (ev[4] | ev[5] << 8) == op) ||
		    (ev[1] == 0x0f && got >= 7 && (ev[5] | ev[6] << 8) == op)) {
			return got;
		}
		bt_say("an event on the way", ev, got < 24 ? got : 24);
	}
	return 0;
}

static struct {
	uint32_t product, soc;
	uint16_t patch, rom;
	uint8_t rom_ver;
	uint16_t board;
} bt_chip;

/* Qualcomm's version request (EDL_PATCH_VER_REQ_CMD): product, patch, ROM and SoC versions; the file names come from
 * the ROM version (btqca.c qca_uart_setup: ((soc_ver & 0xf00) >> 4) | (soc_ver & 0xf)) */
static bool
bt_version(void)
{
	uint8_t cmd = 0x19, ev[64];
	uint32_t n = bt_cmd(0xfc00, &cmd, 1, ev, sizeof(ev), 500), soc_ver;
	const uint8_t *v = ev + 9;

	if (n < 9 + 12 || ev[6] != 0x00 || ev[7] != 0x19) {
		if (n) {
			bt_say("an unexpected version answer", ev, n);
		}
		return false;
	}
	bt_chip.product = v[0] | v[1] << 8 | v[2] << 16 | (uint32_t)v[3] << 24;
	bt_chip.patch = (uint16_t)(v[4] | v[5] << 8);
	bt_chip.rom = (uint16_t)(v[6] | v[7] << 8);
	bt_chip.soc = v[8] | v[9] << 8 | v[10] << 16 | (uint32_t)v[11] << 24;
	soc_ver = bt_chip.soc << 16 | bt_chip.rom;
	bt_chip.rom_ver = (uint8_t)(((soc_ver & 0xf00) >> 4) | (soc_ver & 0xf));
	say("bt: the chip: product %#x, SoC %#x, ROM %#x, patch %#x: version %#x, files of ROM %02x\n", bt_chip.product,
	    bt_chip.soc, bt_chip.rom, bt_chip.patch, soc_ver, bt_chip.rom_ver);
	return bt_chip.soc != 0 && bt_chip.rom != 0;
}

/* the board ID (EDL_GET_BID_REQ_CMD), which names the NVM file */
static void
bt_board(void)
{
	uint8_t cmd = 0x23, ev[64];
	uint32_t n = bt_cmd(0xfc00, &cmd, 1, ev, sizeof(ev), 500);

	if (n >= 6 + 5 && ev[6] == 0 && ev[7] == 0x23) {
		bt_chip.board = (uint16_t)(ev[9] << 8 | ev[10]);
		say("bt: board ID %#x\n", bt_chip.board);
	} else if (n) {
		bt_say("an unexpected board ID answer", ev, n);
	}
}

/*
 * One firmware file to the chip in 243-byte pieces (EDL_PATCH_TLV_REQ_CMD), as btqca.c's qca_download_firmware: the
 * patch says itself whether the chip answers each piece (download mode 0) or only the last (3: no vendor event, no
 * command complete); an ELF patch (.mbn) is always mode 3; the NVM answers every piece. In the NVM's tags: the HCI
 * transport's (17) baud rate stays 115200 and its in-band sleep off, the deep sleep (27) off, as we do no sleep
 * protocol (IBS) on the UART.
 */
static bool
bt_download(const char *name, int type)
{
	uint32_t size, mode = 0, sent = 0, pieces = 0, next = 25;
	uint8_t *data = fw_file(name, &size), cmd[2 + 243], ev[64];
	bool ok = true;

	if (data == NULL) {
		say("bt: no %s\n", name);
		return false;
	}
	if (type == 1 && size > 14) {
		mode = data[14];
	} else if (type == 3) {
		mode = 3;
	} else if (type == 2 && size > 8) {
		uint32_t at = 0, tl = data[0] | data[1] << 8 | data[2] << 16 | (uint32_t)data[3] << 24;

		if ((tl & 0xff) == 4 && size > 8) {
			at = 4;
			tl = data[4] | data[5] << 8 | data[6] << 16 | (uint32_t)data[7] << 24;
		}
		if ((tl & 0xff) == 2) {
			uint32_t length = tl >> 8, i = 0;
			uint8_t *tags = data + at + 4;

			while (i + 12 <= length && at + 4 + length <= size) {
				uint16_t id = (uint16_t)(tags[i] | tags[i + 1] << 8), len = (uint16_t)(tags[i + 2] | tags[i + 3] << 8);
				uint8_t *d = tags + i + 12;

				if (i + 12 + len > length) {
					break;
				}
				if (id == 17 && len >= 3) {
					say("bt: NVM HCI tag: %02x %02x %02x -> in-band sleep off, 115200\n", d[0], d[1], d[2]);
					d[0] &= (uint8_t)~0x80;
					d[1] = 0;
				} else if (id == 27 && len >= 1) {
					say("bt: NVM deep sleep tag: %02x -> off\n", d[0]);
					d[0] &= (uint8_t)~0x01;
				} else if (id == 2 && len == 6) {
					say("bt: NVM address: %02x:%02x:%02x:%02x:%02x:%02x\n", d[5], d[4], d[3], d[2], d[1], d[0]);
				}
				i += 12u + len;
			}
		}
	}
	say("bt: sending %s: %u bytes, download mode %u\n", name, size, mode);
	cmd[0] = 0x1e;
	while (sent < size) {
		uint32_t piece = size - sent < 243 ? size - sent : 243;
		bool last = sent + piece == size || piece < 243, answer = mode == 0 || last;
		uint32_t n;

		cmd[1] = (uint8_t)piece;
		memcpy(cmd + 2, data + sent, piece);
		n = bt_cmd(0xfc00, cmd, 2 + piece, ev, sizeof(ev), answer ? 2000 : 0);
		if (answer && (n < 8 || ev[6] != 0x00 || ev[7] != 0x1e)) {
			say("bt: piece %u (at %u) of %s: %s\n", pieces, sent, name, n ? "refused" : "no answer");
			if (n) {
				bt_say("its answer", ev, n);
			}
			ok = false;
			break;
		}
		sent += piece;
		pieces++;
		if (sent * 100 / size >= next) {
			say("bt: %s: %u%%\n", name, sent * 100 / size);
			next += 25;
		}
	}
	kfree_data(data, size);
	if (ok) {
		say("bt: %s taken (%u pieces)\n", name, pieces);
	}
	return ok;
}

/* the rampatch and the NVM of the chip's ROM version: ganges ("gng", Android's persist.vendor.qcom.bluetooth.soc on
 * dada) unless dadabt_fw=1 picks the brahma ("brh") files; TLV first, then the ELF (.mbn) */
static bool
bt_firmware(void)
{
	const char *stem = "gng";
	uint32_t which = 0;
	char name[48];

	if (PE_parse_boot_argn("dadabt_fw", &which, sizeof(which)) && which == 1) {
		stem = "brh";
	}
	snprintf(name, sizeof(name), "%sbtfw%02x.tlv", stem, bt_chip.rom_ver);
	if (!bt_download(name, 1)) {
		snprintf(name, sizeof(name), "%sbtfw%02x.mbn", stem, bt_chip.rom_ver);
		if (!bt_download(name, 3)) {
			return false;
		}
	}
	ms(10);
	bt_board();
	if (bt_chip.board != 0 && bt_chip.board != 0xffff) {
		snprintf(name, sizeof(name), "%sbtnv%02x.b%02x", stem, bt_chip.rom_ver, bt_chip.board);
		if (bt_download(name, 2)) {
			return true;
		}
	}
	snprintf(name, sizeof(name), "%sbtnv%02x.bin", stem, bt_chip.rom_ver);
	return bt_download(name, 2);
}

/* LE scanning for a few seconds: who advertises around, by address, signal and name */
static void
bt_scan(uint32_t seconds)
{
	static const uint8_t mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x3f };     /* LE meta events too */
	static const uint8_t params[7] = { 0x01, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00 };   /* active, 30 ms of 30 ms */
	uint8_t on[2] = { 1, 0 }, ev[260], seen[24][6];
	uint32_t n, unique = 0, reports = 0;
	uint64_t end, span;

	(void)bt_cmd(0x0c01, mask, sizeof(mask), ev, sizeof(ev), 500);
	n = bt_cmd(0x200b, params, sizeof(params), ev, sizeof(ev), 500);
	if (n < 7 || ev[6] != 0) {
		say("bt: LE scan parameters refused (%u bytes, status %#x)\n", n, n >= 7 ? ev[6] : 0xff);
	}
	n = bt_cmd(0x200c, on, sizeof(on), ev, sizeof(ev), 500);
	if (n < 7 || ev[6] != 0) {
		say("bt: LE scan refused (%u bytes, status %#x)\n", n, n >= 7 ? ev[6] : 0xff);
		return;
	}
	say("bt: LE scan on for %u s\n", seconds);
	nanoseconds_to_absolutetime((uint64_t)seconds * NSEC_PER_SEC, &span);
	end = mach_absolute_time() + span;
	while (mach_absolute_time() < end) {
		uint32_t got = bt_event(ev, sizeof(ev), 200), at, count;

		if (got < 5 || ev[1] != 0x3e || ev[3] != 0x02) {
			continue;
		}
		count = ev[4];
		at = 5;
		for (uint32_t r = 0; r < count && at + 9 <= got; r++) {
			const uint8_t *a = ev + at + 2;
			uint32_t len = ev[at + 8], known = 0;
			int8_t rssi = at + 9 + len < got ? (int8_t)ev[at + 9 + len] : 0;
			char nm[32] = "";

			reports++;
			for (uint32_t i = 0; i + 1 < len;) {    /* the advertised name, if any (AD types 8, 9) */
				uint32_t l = ev[at + 9 + i], t = ev[at + 10 + i];

				if (l == 0 || i + 1 + l > len) {
					break;
				}
				if ((t == 8 || t == 9) && l > 1) {
					uint32_t k = l - 1 < sizeof(nm) - 1 ? l - 1 : sizeof(nm) - 1;

					memcpy(nm, ev + at + 11 + i, k);
					nm[k] = 0;
				}
				i += 1 + l;
			}
			for (uint32_t i = 0; i < unique; i++) {
				known |= memcmp(seen[i], a, 6) == 0;
			}
			if (!known && unique < 24) {
				memcpy(seen[unique++], a, 6);
				say("bt: seen %02x:%02x:%02x:%02x:%02x:%02x %d dBm %s\n", a[5], a[4], a[3], a[2], a[1], a[0], rssi, nm);
			}
			at += 10 + len;
		}
	}
	on[0] = 0;
	(void)bt_cmd(0x200c, on, sizeof(on), ev, sizeof(ev), 500);
	say("bt: LE scan done: %u reports, %u devices%s\n", reports, unique, unique ? " -- Bluetooth works" : "");
}

/*
 * Bluetooth for iOS (dadabt=4): /dev/uart.bluetooth, the serial port iOS's bluetoothd looks for (it opens
 * /dev/cu.bluetooth or /dev/uart.bluetooth, else it exits at once: ios26pro bluetoothd FindLocalMacAddress). The chip
 * is already up (its firmware downloaded, HCI reset done, bt_hci step 3); from here the host does nothing of its own
 * on the UART. bluetoothd drives it: the H4 bytes it writes (01 command, 02 ACL) go straight to the chip, and what the
 * chip sends back (04 events, 02 ACL) is read back. A pump thread moves the chip's bytes into a ring the device reads
 * from (the GENI UART has no interrupt here; it is polled, as everywhere in this file). termios and modem-control
 * ioctls answer success and do nothing: the baud is already the chip's, flow control is the wires'.
 */
#include <sys/conf.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/select.h>
#include <miscfs/devfs/devfs.h>
#include <sys/ttycom.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>

#define BT_RING                 (16u << 10)
static uint8_t *bt_ring;
static uint32_t bt_ring_in, bt_ring_out;
static lck_mtx_t *bt_ring_lock;
static struct selinfo bt_sel;
static bool bt_serving;
static uint32_t bt_writes;
static uint32_t bt_ioctls;

static uint32_t
bt_ring_used(void)
{
	return bt_ring_in - bt_ring_out;
}

/* the chip's bytes, raw, into the ring (called by the pump thread, under the lock) */
static void
bt_ring_fill(void)
{
	uint32_t st = rd(bt_se + SE_GENI_RX_FIFO_STATUS), words = st & 0x1ffffff;

	for (uint32_t i = 0; i < words; i++) {
		uint32_t w = rd(bt_se + SE_GENI_RX_FIFO), bytes = 4;

		if (i + 1 == words && (st & (1u << 31))) {
			bytes = (st >> 28) & 7;
			bytes = bytes ? bytes : 4;
		}
		for (uint32_t k = 0; k < bytes; k++) {
			if (bt_ring_used() < BT_RING) {
				bt_ring[bt_ring_in++ % BT_RING] = (uint8_t)(w >> (8 * k));
			} else {
				bt_rx_lost++;
			}
		}
	}
	if (rd(bt_se + SE_GENI_S_IRQ_STATUS) & 1) {
		wr(bt_se + SE_GENI_S_IRQ_CLEAR, 0xffffffffu);
		wr(bt_se + SE_GENI_S_CMD0, 1u << 27);
	}
}

/*
 * iOS 26's bluetoothd does not open /dev/uart.bluetooth: its serial driver ("POSIX", transport H4) connects a kernel
 * control socket named com.apple.uart.sk.bluetooth (bluetoothd 0x100192fd0: socket(PF_SYSTEM, SOCK_STREAM,
 * SYSPROTO_CONTROL), CTLIOCGINFO, connect), takes the line's termios through socket options (get 0x7413 / 0x5412,
 * set 0x5414 ...) and then sends and receives the H4 stream on it. That control, here: what bluetoothd sends goes to the
 * chip (its Broadcom vendor commands answered on the way, bt_answer_vendor), what the chip says goes to it (the pump).
 */
static kern_ctl_ref bt_kctl;
static uint32_t bt_kctl_unit;
static volatile int bt_kctl_up;
static bool bt_answer_vendor(const uint8_t *packet, uint32_t n);

static errno_t
bt_kctl_connect(kern_ctl_ref ref __unused, struct sockaddr_ctl *sac, void **unitinfo)
{
	bt_kctl_unit = sac->sc_unit;
	*unitinfo = NULL;
	bt_kctl_up = 1;
	say("bt: iOS's bluetoothd connected to com.apple.uart.sk.bluetooth (unit %u)\n", sac->sc_unit);
	return 0;
}

static errno_t
bt_kctl_disconnect(kern_ctl_ref ref __unused, u_int32_t unit __unused, void *unitinfo __unused)
{
	bt_kctl_up = 0;
	say("bt: bluetoothd let go of com.apple.uart.sk.bluetooth\n");
	return 0;
}

static errno_t
bt_kctl_send(kern_ctl_ref ref __unused, u_int32_t unit __unused, void *unitinfo __unused, mbuf_t m, int flags __unused)
{
	static uint32_t said;
	uint8_t buf[1100];
	size_t n = mbuf_pkthdr_len(m);

	if (n > sizeof(buf)) {
		n = sizeof(buf);
	}
	if (mbuf_copydata(m, 0, n, buf) == 0 && n != 0) {
		if (said++ < 24) {
			bt_say("iOS sends (socket)", buf, (uint32_t)(n < 16 ? n : 16));
		}
		if (!bt_answer_vendor(buf, (uint32_t)n)) {
			(void)bt_send(buf, (uint32_t)n);
		}
	}
	mbuf_freem(m);
	return 0;
}

static errno_t
bt_kctl_setopt(kern_ctl_ref ref __unused, u_int32_t unit __unused, void *unitinfo __unused, int opt, void *data __unused,
    size_t len)
{
	static uint32_t said;

	if (said++ < 16) {
		say("bt: bluetoothd sets socket option %#x (%zu bytes): taken\n", opt, len);
	}
	return 0;
}

static errno_t
bt_kctl_getopt(kern_ctl_ref ref __unused, u_int32_t unit __unused, void *unitinfo __unused, int opt, void *data,
    size_t *len)
{
	static uint32_t said;

	extern bool hakios_btsk_uuid(uint8_t uuid[16]);

	if (data != NULL && len != NULL) {
		bzero(data, *len);
		/* the Skywalk nexus the H4 stream goes through (hakios_dada_btsk.c): bluetoothd opens a channel on it */
		if ((opt & 0xffff) == 0x5412 && *len >= 16) {
			(void)hakios_btsk_uuid(data);
		}
	}
	if (said++ < 16) {
		say("bt: bluetoothd asks socket option %#x (%zu bytes)%s\n", opt, len ? *len : 0,
		    (opt & 0xffff) == 0x5412 ? ": the nexus UUID" : ": zeros");
	}
	return 0;
}

static void
bt_kctl_register(void)
{
	struct kern_ctl_reg reg;
	errno_t error;

	bzero(&reg, sizeof(reg));
	strlcpy(reg.ctl_name, "com.apple.uart.sk.bluetooth", sizeof(reg.ctl_name));
	reg.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	reg.ctl_sendsize = 64 * 1024;
	reg.ctl_recvsize = 64 * 1024;
	reg.ctl_connect = bt_kctl_connect;
	reg.ctl_disconnect = bt_kctl_disconnect;
	reg.ctl_send = bt_kctl_send;
	reg.ctl_setopt = bt_kctl_setopt;
	reg.ctl_getopt = bt_kctl_getopt;
	error = ctl_register(&reg, &bt_kctl);
	say("bt: kernel control com.apple.uart.sk.bluetooth: %s (%d)\n", error == 0 ? "registered" : "NOT registered", error);
}

/* the chip's bytes waiting in the ring, to bluetoothd's socket (with the ring lock held) */
static void __unused
bt_kctl_feed(void)
{
	uint8_t chunk[512];

	while (bt_kctl_up && bt_ring_used() != 0) {
		uint32_t n = 0;

		while (n < sizeof(chunk) && bt_ring_used() != 0) {
			chunk[n++] = bt_ring[bt_ring_out++ % BT_RING];
		}
		if (ctl_enqueuedata(bt_kctl, bt_kctl_unit, chunk, n, 0) != 0) {
			bt_ring_out -= n;       /* (its buffer is full: again next time) */
			break;
		}
	}
}

/* for the Skywalk channel (hakios_dada_btsk.c): bluetoothd's bytes to the chip, the chip's bytes waiting for it */
void hakios_bt_put(const uint8_t *p, uint32_t n);
uint32_t hakios_bt_take(uint8_t *p, uint32_t most);

void
hakios_bt_put(const uint8_t *p, uint32_t n)
{
	static uint32_t said;

	if (said++ < 120) {
		bt_say("iOS sends (channel)", p, n < 16 ? n : 16);
	}
	if (!bt_answer_vendor(p, n)) {
		(void)bt_send(p, n);
	}
}

uint32_t
hakios_bt_take(uint8_t *p, uint32_t most)
{
	uint32_t n = 0;

	if (bt_ring_lock == NULL) {
		return 0;
	}
	lck_mtx_lock(bt_ring_lock);
	while (n < most && bt_ring_used() != 0) {
		p[n++] = bt_ring[bt_ring_out++ % BT_RING];
	}
	lck_mtx_unlock(bt_ring_lock);
	/* the chip's refusals, said: Command Complete (04 0e len n op op status) / Command Status (04 0f 04 status n op op) */
	{
		static uint32_t said_refused;

		for (uint32_t i = 0; i + 7 <= n; i++) {
			if (p[i] == 0x04 && p[i + 1] == 0x0e && p[i + 6] != 0 && said_refused < 40) {
				said_refused++;
				say("bt: the chip refuses command %#06x: status %#x\n", p[i + 4] | p[i + 5] << 8, p[i + 6]);
			} else if (p[i] == 0x04 && p[i + 1] == 0x0f && p[i + 3] != 0 && said_refused < 40) {
				said_refused++;
				say("bt: the chip refuses command %#06x (status event): %#x\n", p[i + 5] | p[i + 6] << 8, p[i + 3]);
			}
		}
	}
	/*
	 * the chip's Read Local Supported Commands answer (Command Complete 04 0e 44 01 02 10 00, then 64 octets): octet 18
	 * bits 2 and 3, Read/Write Default Erroneous Data Reporting, set -- bluetoothd will not start without them ("status
	 * 628", run w88t1) and bt_answer_vendor answers them itself. Matched on the stream: an answer comes in pieces.
	 */
	{
		static const uint8_t head[7] = { 0x04, 0x0e, 0x44, 0x01, 0x02, 0x10, 0x00 };
		static uint32_t matched, after;         /* bytes of head matched; octets of the mask passed (0: none yet) */

		for (uint32_t i = 0; i < n; i++) {
			if (after != 0) {
				if (after == 19) {
					p[i] |= 0x0c;
					say("bt: the chip's supported commands: octet 18 now %#x (erroneous data reporting answered here)\n", p[i]);
					after = 0;
				} else {
					after++;
				}
				continue;
			}
			matched = p[i] == head[matched] ? matched + 1 : (p[i] == head[0] ? 1 : 0);
			if (matched == sizeof(head)) {
				matched = 0;
				after = 1;              /* the next byte is octet 0 */
			}
		}
	}
	return n;
}

static void
bt_pump_thread(void *arg __unused, wait_result_t w __unused)
{
	for (;;) {
		bool woke = false;

		lck_mtx_lock(bt_ring_lock);
		bt_ring_fill();
		if (bt_ring_used() != 0 && bt_kctl_up) {
			extern void hakios_btsk_kick(void);

			lck_mtx_unlock(bt_ring_lock);
			hakios_btsk_kick();    /* (bluetoothd reads through the Skywalk channel, which takes from the ring) */
			lck_mtx_lock(bt_ring_lock);
		}
		if (bt_ring_used() != 0) {
			woke = true;
			wakeup(&bt_ring_in);
		}
		lck_mtx_unlock(bt_ring_lock);
		if (woke) {
			selwakeup(&bt_sel);
		}
		ms(2);
	}
}

static uint32_t bt_opens;

static int
bt_dev_open(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	int waited;

	if (bt_opens < 8) {
		bt_opens++;
		say("bt: /dev/uart.bluetooth opened (flags %#x; chip %s)\n", flags, bt_serving ? "up" : "not up yet");
	}
	/* the node exists before the chip is up: let the opener wait here rather than fail (bluetoothd gives up on a
	 * failed open), up to ~40 s for the firmware download to finish */
	for (waited = 0; !bt_serving && waited < 40000; waited += 50) {
		ms(50);
	}
	return bt_serving ? 0 : ENXIO;
}

static int
bt_dev_close(dev_t dev __unused, int flags __unused, int devtype __unused, struct proc *p __unused)
{
	return 0;
}

static int
bt_dev_read(dev_t dev __unused, struct uio *uio, int ioflag)
{
	int error = 0;

	lck_mtx_lock(bt_ring_lock);
	while (bt_ring_used() == 0) {
		if (ioflag & IO_NDELAY) {
			lck_mtx_unlock(bt_ring_lock);
			return EWOULDBLOCK;
		}
		error = msleep(&bt_ring_in, bt_ring_lock, PCATCH, "btread", NULL);
		if (error != 0) {
			lck_mtx_unlock(bt_ring_lock);
			return error == ERESTART ? 0 : error;
		}
	}
	while (bt_ring_used() != 0 && uio_resid(uio) > 0 && error == 0) {
		uint32_t at = bt_ring_out % BT_RING, run = BT_RING - at, have = bt_ring_used();
		uint32_t n = have < run ? have : run;

		if ((user_ssize_t)n > uio_resid(uio)) {
			n = (uint32_t)uio_resid(uio);
		}
		error = uiomove((caddr_t)(bt_ring + at), (int)n, uio);
		if (error == 0) {
			bt_ring_out += n;
		}
	}
	lck_mtx_unlock(bt_ring_lock);
	return error;
}

/*
 * bluetoothd speaks Broadcom: Apple's chips descend from Broadcom's, and it sends their vendor commands (OGF 0x3f,
 * opcodes 0xfcxx: sleep mode, BD_ADDR, verbose config...) which the Qualcomm chip does not know -- or, worse, takes as
 * its own (Qualcomm's vendor commands share the range). Each is answered here, never sent: a Command Complete with
 * status 0 and no return parameters, put in the stream to bluetoothd as the chip's. Boot-arg dadabt_vsc=0 sends them.
 */
static uint32_t bt_vsc_answered;

static bool
bt_answer_vendor(const uint8_t *packet, uint32_t n)
{
	static uint32_t pass = 0xffffffffu;
	const uint8_t event[7] = { 0x04, 0x0e, 0x04, 0x01, packet[1], packet[2], 0x00 };

	if (pass == 0xffffffffu) {
		uint32_t answer = 1;

		(void)PE_parse_boot_argn("dadabt_vsc", &answer, sizeof(answer));
		pass = answer == 0;
	}
	if (pass || n < 4 || packet[0] != 0x01) {
		return false;
	}
	if ((packet[2] & 0xfc) != 0xfc) {
		/*
		 * standard commands the Qualcomm chip refuses and bluetoothd will not start without, answered here too:
		 * Write Default Erroneous Data Reporting (0x0c5b: "HCI initialization failed", run w87s1), and the ones the
		 * boot-arg dadabt_fake=0xOPCODE[,0xOPCODE...] names (up to 8)
		 */
		static uint32_t fake[9] = { 0x0c5b, 0x0c5a };
		static int fake_read;
		uint32_t op = packet[1] | (uint32_t)packet[2] << 8;
		bool hit = false;

		if (!fake_read) {
			char list[96] = { 0 };
			const char *q = list;
			unsigned k = 1;

			fake_read = 1;
			if (PE_parse_boot_arg_str("dadabt_fake", list, sizeof(list))) {
				while (*q != 0 && k < 9) {
					fake[k++] = (uint32_t)strtoul(q, (char **)&q, 0);
					while (*q == ',') {
						q++;
					}
				}
			}
		}
		for (unsigned k = 0; k < 9; k++) {
			hit |= fake[k] != 0 && fake[k] == op;
		}
		if (!hit) {
			return false;
		}
	}
	lck_mtx_lock(bt_ring_lock);
	/*
	 * the answer's return parameters: status 0, and for a vendor command the first parameter byte echoed (often its
	 * sub-opcode) and 8 zero bytes -- bluetoothd's ACI code asserts on an answer with nothing after the status
	 * (aci_vsc.c:135 "ByteStream_NumReadBytesAvail(*pBs) >= 1", BD_VSC_LE_META_ENA_FEATURES, run w90v1);
	 * Read Default Erroneous Data Reporting gets its one setting byte (0 = off)
	 */
	{
		bool vendor = (packet[2] & 0xfc) == 0xfc;
		bool one_more = packet[1] == 0x5a && packet[2] == 0x0c;
		uint8_t ev[16];
		unsigned len;

		/* boot-arg dadabt_short=0xOPCODE[,...]: those answered with the status alone (up to 8) */
		static uint32_t shorts[8];
		static int shorts_read;
		uint32_t op = packet[1] | (uint32_t)packet[2] << 8;

		if (!shorts_read) {
			char list[96] = { 0 };
			const char *q = list;
			unsigned k = 0;

			shorts_read = 1;
			if (PE_parse_boot_arg_str("dadabt_short", list, sizeof(list))) {
				while (*q != 0 && k < 8) {
					shorts[k++] = (uint32_t)strtoul(q, (char **)&q, 0);
					while (*q == ',') {
						q++;
					}
				}
			}
		}
		for (unsigned k = 0; k < 8; k++) {
			if (shorts[k] != 0 && shorts[k] == op) {
				vendor = false;
			}
		}
		bzero(ev, sizeof(ev));
		memcpy(ev, event, sizeof(event));
		if (vendor) {
			ev[7] = n > 4 ? packet[4] : 0;
			len = 7 + 1 + 8;
		} else {
			len = 7 + (one_more ? 1 : 0);
		}
		ev[2] = (uint8_t)(len - 3);
		for (unsigned i = 0; i < len && bt_ring_used() < BT_RING; i++) {
			bt_ring[bt_ring_in++ % BT_RING] = ev[i];
		}
	}
	wakeup(&bt_ring_in);
	lck_mtx_unlock(bt_ring_lock);
	selwakeup(&bt_sel);
	if (bt_vsc_answered++ < 120) {
		say("bt: iOS's command %#06x (%u bytes of parameters) answered here, not sent\n",
		    packet[1] | packet[2] << 8, packet[3]);
	}
	return true;
}

static int
bt_dev_write(dev_t dev __unused, struct uio *uio, int ioflag __unused)
{
	uint8_t buf[512];
	int error = 0;

	while (uio_resid(uio) > 0 && error == 0) {
		user_ssize_t want = uio_resid(uio);
		uint32_t n = want > (user_ssize_t)sizeof(buf) ? (uint32_t)sizeof(buf) : (uint32_t)want;

		error = uiomove((caddr_t)buf, (int)n, uio);
		if (error != 0) {
			break;
		}
		/* the first writes said: which HCI packets bluetoothd sends first tells whether it takes the chip as it is
		 * (01 03 0c: HCI Reset, then Read Local Version) or runs an Apple chip-boot our Qualcomm chip would refuse */
		if (bt_writes < 12) {
			bt_writes++;
			bt_say(buf[0] == 1 ? "iOS writes a command" : buf[0] == 2 ? "iOS writes ACL" : "iOS writes", buf,
			    n < 16 ? n : 16);
		}
		if (bt_answer_vendor(buf, n)) {
			continue;
		}
		if (!bt_send(buf, n)) {         /* raw to the chip: bluetoothd already put the H4 type byte first */
			error = EIO;
		}
	}
	return error;
}

static int
bt_dev_select(dev_t dev __unused, int which, void *wql, struct proc *p)
{
	int ready = 0;

	if (which == FREAD) {
		lck_mtx_lock(bt_ring_lock);
		if (bt_ring_used() != 0) {
			ready = 1;
		} else {
			selrecord(p, &bt_sel, wql);
		}
		lck_mtx_unlock(bt_ring_lock);
	} else if (which == FWRITE) {
		ready = 1;
	}
	return ready;
}

static int
bt_dev_ioctl(dev_t dev __unused, u_long command, caddr_t data __unused, int flags __unused, struct proc *p __unused)
{
	/* termios and modem lines: whatever bluetoothd sets, say yes and do nothing (the baud is the chip's already) */
	if (bt_ioctls < 16) {
		bt_ioctls++;
		say("bt: ioctl %#lx\n", command);
	}
	switch (command) {
	case TIOCGETA:
	case TIOCSETA:
	case TIOCSETAW:
	case TIOCSETAF:
	case TIOCMGET:
	case TIOCMSET:
	case TIOCMBIS:
	case TIOCMBIC:
	case TIOCFLUSH:
	case TIOCEXCL:
	case TIOCNXCL:
		return 0;
	default:
		return 0;               /* unknown ioctls succeed too: bluetoothd must not take one as a reason to give up */
	}
}

static const struct cdevsw bt_cdevsw = {
	.d_open = bt_dev_open, .d_close = bt_dev_close, .d_read = bt_dev_read, .d_write = bt_dev_write,
	.d_ioctl = bt_dev_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = bt_dev_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * /dev/uart.bluetooth is made EARLY, at kernel start (bt_make_node, before the chip is up), because iOS's bluetoothd
 * looks for the serial port as soon as it runs and gives up ("No bluetooth on this device", errx) if it is not there;
 * a node that turns up 25 s later is one it has already failed to find. The chip is not ready yet, so bt_dev_open
 * waits for it (bt_serving), and the pump starts when bt_serve() is called at the end of the chip's bring-up.
 */
void hakios_dada_bt_make_node(void);

void
hakios_dada_bt_make_node(void)
{
	static lck_grp_t *grp;
	int major;

	if (bt_ring != NULL) {
		return;
	}
	bt_ring = kalloc_data(BT_RING, Z_WAITOK | Z_ZERO);
	if (bt_ring == NULL) {
		return;
	}
	grp = lck_grp_alloc_init("bt-uart", LCK_GRP_ATTR_NULL);
	bt_ring_lock = lck_mtx_alloc_init(grp, LCK_ATTR_NULL);
	major = cdevsw_add(-1, &bt_cdevsw);
	if (major < 0) {
		say("bt: cannot register the bluetooth cdev\n");
		return;
	}
	/* both names bluetoothd looks for, same device: it tries /dev/cu.bluetooth first, then /dev/uart.bluetooth */
	(void)devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "cu.bluetooth");
	(void)devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "uart.bluetooth");
	say("bt: /dev/cu.bluetooth and /dev/uart.bluetooth made (chip not up yet; opens wait)\n");
	/* ... and the IOKit service bluetoothd actually finds the port through (iokit/Kernel/RedwoodBT.cpp): a bare
	 * /dev node is invisible to its IOServiceNameMatching("bluetooth") lookup */
	{
		extern void RedwoodBTStart(const char *bsdname, const uint8_t *address);

		RedwoodBTStart("uart.bluetooth", NULL);
	}
}

/* the chip is up: start the pump and let opens through */
static void
bt_serve(void)
{
	thread_t thread;

	hakios_dada_bt_make_node();
	if (bt_ring == NULL) {
		return;
	}
	bt_serving = true;
	{
		extern void hakios_btsk_start(void);

		hakios_btsk_start();
	}
	bt_kctl_register();
	if (kernel_thread_start(bt_pump_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_set_thread_name(thread, "bt-uart-pump");
		thread_deallocate(thread);
	}
	say("bt: /dev/uart.bluetooth serving; iOS's bluetoothd drives the chip now\n");
}

/*
 * Bluetooth, steps 2 and 3. dadabt=2: the UART and the chip's ROM answering (HCI Reset, Qualcomm's version request);
 * if nothing sensible comes back, the other oversampling (32 / 16) is tried. dadabt=3 (on top): btqca.c's
 * qca_uart_setup for WCN7850-like chips: the rampatch and the NVM by ROM version and board ID, SoC logging off, HCI
 * Reset, the firmware's build string and the device address, and an LE scan to show the radio hears the world.
 * dadabt=4 (on top): instead of our scan, /dev/uart.bluetooth for iOS's bluetoothd (bt_serve).
 */
static void
bt_hci(uint32_t step)
{
	uint32_t sampling = 16, n;
	uint8_t answer[260];
	bool alive = false;

	(void)PE_parse_boot_argn("dadabt_os", &sampling, sizeof(sampling));
	if (!bt_clock()) {
		return;
	}
	pin(24, 1u << 2, -1);                   /* CTS */
	pin(25, 1u << 2, -1);                   /* RFR */
	pin(26, 1u << 2 | 3u, -1);              /* TX, pulled up */
	pin(27, 1u << 2 | 3u, -1);              /* RX, pulled up */
	bt_se = ml_io_map(QUP2_SE6_PHYS, 0x4000);
	for (int attempt = 0; attempt < 2 && !alive; attempt++) {
		if (attempt) {
			sampling = sampling == 16 ? 32 : 16;
			say("bt: again with oversampling %u\n", sampling);
		}
		bt_uart(7372800 / (115200 * sampling));
		ms(200);                        /* (the chip's ROM, after BT_EN, before it listens) */
		bt_rx_len = 0;
		(void)bt_pull();
		if (bt_rx_len) {
			bt_say("before anything was sent", bt_rx, bt_rx_len < 32 ? bt_rx_len : 32);
			bt_rx_len = 0;
		}
		n = bt_cmd(0x0c03, NULL, 0, answer, sizeof(answer), 500);
		if (n) {
			bt_say("HCI Reset's answer", answer, n);
		} else if (bt_rx_len) {
			bt_say("bytes, not an event", bt_rx, bt_rx_len < 32 ? bt_rx_len : 32);
		}
		alive = bt_version();
	}
	say("bt: UART status %#x, RX FIFO %#x, M IRQ %#x, S IRQ %#x\n", rd(bt_se + 0x40), rd(bt_se + SE_GENI_RX_FIFO_STATUS),
	    rd(bt_se + SE_GENI_M_IRQ_STATUS), rd(bt_se + SE_GENI_S_IRQ_STATUS));
	if (!alive || step < 3) {
		return;
	}
	if (!bt_firmware()) {
		say("bt: the firmware was not taken: the ROM's own code only\n");
	} else {
		static const uint8_t logging[2] = { 0x14, 0x00 };

		ms(10);
		(void)bt_cmd(0xfc17, logging, sizeof(logging), answer, sizeof(answer), 500);
	}
	n = bt_cmd(0x0c03, NULL, 0, answer, sizeof(answer), 2000);
	say("bt: HCI Reset after the firmware: %s\n", n >= 7 && answer[6] == 0 ? "done" : "FAILED");
	{
		uint8_t info = 0x20;

		n = bt_cmd(0xfc00, &info, 1, answer, sizeof(answer), 500);
		if (n > 9 && answer[7] == 0x20) {
			uint32_t len = answer[8] < n - 9 ? answer[8] : n - 9;
			char build[200];

			memcpy(build, answer + 9, len < sizeof(build) - 1 ? len : sizeof(build) - 1);
			build[len < sizeof(build) - 1 ? len : sizeof(build) - 1] = 0;
			say("bt: firmware build: %s\n", build);
		}
	}
	n = bt_cmd(0x1009, NULL, 0, answer, sizeof(answer), 500);
	if (n >= 13 && answer[6] == 0) {
		extern void RedwoodBTSetAddress(const uint8_t *address);
		const uint8_t shown[6] = { answer[12], answer[11], answer[10], answer[9], answer[8], answer[7] };

		say("bt: device address %02x:%02x:%02x:%02x:%02x:%02x\n", shown[0], shown[1], shown[2], shown[3], shown[4],
		    shown[5]);
		RedwoodBTSetAddress(shown);     /* bluetoothd quits on an all-00s address before it opens the port */
	}
	n = bt_cmd(0x1001, NULL, 0, answer, sizeof(answer), 500);
	if (n >= 15 && answer[6] == 0) {
		say("bt: HCI version %u rev %#x, LMP version %u, manufacturer %#x\n", answer[7], answer[8] | answer[9] << 8,
		    answer[10], answer[11] | answer[12] << 8);
	}
	if (step >= 4) {
		bt_serve();     /* hand the chip to iOS's bluetoothd over /dev/uart.bluetooth, not our own scan */
		return;
	}
	bt_scan(10);
	if (bt_rx_lost) {
		say("bt: %u bytes lost (bt_rx full)\n", bt_rx_lost);
	}
}

/*
 * ---- dadawifi=4: the driver -----------------------------------------------------------------------------------
 *
 * Instead of our own small MHI boot (dadawifi=3), the chip is handed to Linux's ath12k (tools/mk-ath12k.sh links its
 * object into the kernel: it shows nw_wifi_start/step/stop and asks only for C library names). The driver does the
 * chip's own power-up, the MHI boot, QMI, the data path and a WPA2 station itself, over two things this side gives it:
 * a platform (memory, time, DMA, firmware files) and a board (the endpoint's registers, its configuration space, where
 * its interrupts would land). It takes 32-bit addresses only, so all of its DMA memory is the loader's below 4 GB
 * (lowdma=), and its interrupts are not delivered: the driver's runtime offers every handler a look about once a
 * millisecond and each reads its own registers (nw_pci.c, lx_rt.c). link_up() has run (stage >= 2): the PCIe link is
 * up, the endpoint's configuration space is at ep_conf and its BAR0 (MHI registers, and the chip-register window at
 * +0x80000) at ep_mhi.
 */

/* the plain-C boundary to the driver's object (its include/nw_abi.h; kept in step by hand, not a shared header) */
struct lx_platform {
	uint64_t (*now_ms)(void);
	void (*sleep_ms)(unsigned int ms);
	void (*poll)(void);
	void (*log)(int level, const char *line);
	void (*halt)(void);
	void *(*alloc)(size_t n);
	void (*free)(void *p);
	void *(*dma_alloc)(size_t n, uint64_t *bus);
	void (*dma_free)(size_t n, void *p, uint64_t bus);
	uint64_t (*dma_map)(void *p, size_t n, int dir);
	void (*dma_sync)(uint64_t bus, size_t n, int dir);
	int (*fw_get)(const char *name, const uint8_t **data, size_t *size);
};
struct nw_board {
	void *bar;
	size_t bar_len;
	uint16_t device_id, msi_cap;
	int irq_base;
	int (*cfg_read)(int off, int size, uint32_t *v);
	int (*cfg_write)(int off, int size, uint32_t v);
	int (*msi_target)(uint32_t *lo, uint32_t *hi, uint32_t *data);
	void *(*map_phys)(uint64_t pa, size_t n);
};
struct nw_wifi_stats { uint32_t stage, scans, frames, beacons, bss_seen; int last_err; };
/* weak: a kernel built without the driver's object (tools/mk-ath12k.sh; e.g. a redwood build) still links, and
 * driver() finds nw_wifi_start NULL and says so */
extern struct nw_wifi_stats nw_wifi_stats __attribute__((weak));
extern volatile uint64_t nw_wifi_where[4] __attribute__((weak));
extern int nw_wifi_start(const struct lx_platform *p, const struct nw_board *b) __attribute__((weak));
extern int nw_wifi_step(void) __attribute__((weak));
extern void nw_wifi_stop(void) __attribute__((weak));
extern int nw_wifi_scan(const uint8_t *ssid, size_t len) __attribute__((weak));

/*
 * The DMA heap: the driver asks for memory many times over and gives it back (the receive buffers above all), so the
 * bump allocator above (dadawifi=3, which never frees) will not do. This is the loader's memory below 4 GB from 4 MB
 * on, in blocks rounded up to a power of two (at least 64 bytes), 64-byte aligned, with an 8-byte length kept in the
 * eight bytes before each block; a freed block goes on the free list of its size and is handed back whole next time.
 */
static struct {
	vm_offset_t base_va;
	uint64_t base_pa, size, used, peak;
	uint32_t free_off[32];          /* per size class: the offset of the first free block, plus one (0 = none) */
	uint32_t live, fails;
	uint8_t *big;                   /* per 4 KB page: the size class of a block of 4 KB or more that starts there */
} hp;

static unsigned
hp_class(uint64_t n)
{
	unsigned c = 6;
	uint64_t s = 64;

	while (s < n) {
		s <<= 1;
		c++;
	}
	return c < 31 ? c : 31;
}

static void *
hp_alloc(uint64_t bytes, uint64_t *pa)
{
	unsigned c = hp_class(bytes);
	uint64_t block = 1ull << c, off;

	if (hp.free_off[c] != 0) {
		off = hp.free_off[c] - 1;
		memcpy(&hp.free_off[c], (void *)(hp.base_va + off), 4);          /* next free, +1, kept in the block */
	} else {
		/* aligned as Linux's dma_alloc_coherent is (pages of the block's own order): a block of 4 KB or more to its
		 * own size, a smaller one to 64 bytes. MHI hands the chip these addresses as they are -- the firmware's
		 * segments, the BHIe vector table, the rings -- and with 8-byte alignment (the length used to sit right
		 * before a 64-byte boundary) the chip's first DMA after BHI took the whole phone down (runs wifi7..9). */
		/* A block of 4 KB or more keeps its size in hp.big, by page, and nothing before it: with the length in the
		 * eight bytes before, every such block was pushed to the next boundary of its own size and the heap ran
		 * out (a 512 KB block took 1 MB; run wifi21: "failed to set up tcl_comp ring: -12"). */
		uint64_t align = block >= 4096 ? block : 64;
		uint64_t head = block >= 4096 ? 0 : 8;
		uint64_t pa_off = (hp.base_pa + hp.used + head + align - 1) & ~(align - 1);   /* aligned in BUS addresses */

		off = pa_off - hp.base_pa;
		if (off + block > hp.size || (block >= 4096 && hp.big == NULL)) {
			hp.fails++;
			return NULL;
		}
		if (block >= 4096) {
			hp.big[off >> 12] = (uint8_t)c;
		} else {
			memcpy((void *)(hp.base_va + off - 8), &block, 8);      /* the eight bytes before hold the length */
		}
		hp.used = off + block;
		if (hp.used > hp.peak) {
			hp.peak = hp.used;
		}
	}
	hp.live++;
	bzero((void *)(hp.base_va + off), (size_t)block);
	if (pa != NULL) {
		*pa = hp.base_pa + off;
	}
	return (void *)(hp.base_va + off);
}

static void
hp_free(void *p)
{
	uint64_t off, block;
	unsigned c;

	if (p == NULL || (vm_offset_t)p < hp.base_va || (vm_offset_t)p >= hp.base_va + hp.size) {
		return;
	}
	off = (uint64_t)((vm_offset_t)p - hp.base_va);
	if ((off & 0xfff) == 0 && hp.big != NULL && hp.big[off >> 12] != 0) {
		c = hp.big[off >> 12];                                  /* a block of 4 KB or more (hp_alloc) */
	} else {
		memcpy(&block, (void *)(hp.base_va + off - 8), 8);
		c = hp_class(block);
	}
	memcpy((void *)(hp.base_va + off), &hp.free_off[c], 4);
	hp.free_off[c] = (uint32_t)off + 1;
	hp.live--;
}

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000ull;
}

static void plat_sleep(unsigned int n) { if (n) ms(n); else us(200); }
static void plat_poll(void) { }
/* the link as the root port sees it, from its own registers only (DBI and PARF answer whatever the link does; the
 * endpoint's BAR does not) */
static void
link_state(const char *after)
{
	static vm_offset_t dbi_va, parf_va;

	if (dbi_va == 0) {
		dbi_va = ml_io_map(DBI_PHYS, 0x1000);
		parf_va = ml_io_map(PARF_PHYS, 0x3000);
	}
	if (dbi_va == 0 || parf_va == 0) {
		return;
	}
	say("link: LTSSM %#x debug1 %#x link control/status %#x parf ltssm %#x pm %#x (after: %.40s)\n",
	    rd(dbi_va + DBI_PORT_DEBUG0) & 0x3f, rd(dbi_va + DBI_PORT_DEBUG1), rd(dbi_va + DBI_LINK_CONTROL),
	    rd(parf_va + PARF_LTSSM), rd(parf_va + PARF_PM_CTRL), after);
}

/* The firmware's own account of a crash. MHI programs the RDDM vector table at power-up ("BHIe programming for
 * RDDM"), so a chip that goes to RDDM writes its memory into those segments of the DMA heap by itself; the reason
 * is a string in it ("... .c:LINE ... assert"). Printed a few seconds after the RDDM event, from the driver's loop. */
static uint64_t rddm_at;
static bool rddm_scanned;

/* whether `needle` is in `hay` (the kernel has no strstr) */
static bool
has(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; *hay; hay++) {
		if (strncmp(hay, needle, n) == 0) {
			return true;
		}
	}
	return false;
}

static void
rddm_scan(void)
{
	static const char *const keys[] = { "ssert", ".c:", "FATAL", "atal ", "rror", "xception", "PANIC", "panic" };
	const char *mem = (const char *)hp.base_va;
	uint64_t at = 0, start = 0;
	unsigned shown = 0, run = 0;

	say("rddm: looking through %llu MB of DMA memory for the firmware's reason\n", hp.used >> 20);
	for (at = 0; at < hp.used && shown < 40; at++) {
		char c = mem[at];

		if (c >= 0x20 && c < 0x7f) {
			if (run++ == 0) {
				start = at;
			}
			continue;
		}
		if (run >= 12) {
			char line[160];
			size_t n = MIN((size_t)run, sizeof(line) - 1);

			memcpy(line, mem + start, n);
			line[n] = 0;
			for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
				if (has(line, keys[k])) {
					say("rddm: +%#llx: %s\n", start, line);
					shown++;
					break;
				}
			}
		}
		run = 0;
	}
	say("rddm: %u strings\n", shown);
}

/*
 * The driver's last lines, said again when it is stuck: with iOS up, the loader's 1 MB ring of log turns over within
 * seconds, and the lines before a hang are gone by the time anyone looks (run final2, 28 October).
 */
#define TRAIL_LINES     64
static char trail[TRAIL_LINES][160];
static uint64_t trail_ms[TRAIL_LINES];
static uint32_t trail_next, trail_said;

static void
trail_keep(const char *line)
{
	uint32_t i = trail_next % TRAIL_LINES;

	strlcpy(trail[i], line, sizeof(trail[i]));
	trail_ms[i] = now_ms();
	trail_next++;
	if (!trail_said && has(line, "stuck:")) {
		trail_said = 1;
		for (uint32_t k = trail_next > TRAIL_LINES ? trail_next - TRAIL_LINES : 0; k < trail_next; k++) {
			say("trail %llu ms: %s\n", trail_ms[k % TRAIL_LINES], trail[k % TRAIL_LINES]);
		}
		if (tlmm) {	/* the chip's control pins at the hang: WLAN_EN 16, SW_CTRL 18/19, SOL 202/203 (cfg, in_out) */
			static const uint32_t pins[] = { 16, 18, 19, 202, 203, 148, 149, 150, 151 };	/* + 148..151: the WLAN-LTE coex UART (WCI-2) */

			for (uint32_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
				say("stuck pins: gpio%u cfg 0x%x in_out 0x%x\n", pins[i], rd(tlmm + 0x1000UL * pins[i]),
				    rd(tlmm + 0x1000UL * pins[i] + 4));
			}
		}
	}
}

/*
 * The firmware's OL CPR table (tools/ath12k-olcpr.py: "ol cpr: PIN wake MV sleep MV"): the voltage each PMU pin wants
 * while the chip is awake. cnss2 hands it to AOP for the rail the DT's qcom,pmu_vreg_map puts the pin on; here our own
 * RPMh vote for that rail is raised to it (never lowered, never past the rail's DT maximum).
 */
static void
ol_cpr(const char *line)
{
	static const struct {
		const char *pin, *rail;
		uint32_t min, max;      /* what rails() votes; the rail's DT maximum */
	} map[] = {
		{ "VDDD_AON_0P9", "smpf5", 876, 1000 }, { "VDDA_RFA_1P9", "smpg3", 1860, 2000 },
		{ "VDDA_RFA_1P3", "smpi7", 1312, 1340 }, { "VDDA_RFA_0P9", "smpf5", 876, 1000 },
		{ "VDDD_WLMX_0P9", "smpd4", 876, 1036 }, { "VDDD_WLCX_0P9", "smpf5", 876, 1000 },
		{ "VDDD_BTCX_0P9", "smpf5", 876, 1000 }, { "VDDD_BTCMX_0P9", "smpf5", 876, 1000 },
		{ "VDDA_PCIE_1P2", "smpi7", 1312, 1340 }, { "VDDA_PCIE_0P9", "smpi7", 1312, 1340 },
	};
	static uint32_t voted[sizeof(map) / sizeof(map[0])];
	const char *at = line, *w;
	char pin[32];
	uint32_t mv = 0, n = 0;

	for (; *at != 0 && strncmp(at, "ol cpr: ", 8) != 0; at++) {
	}
	if (*at == 0) {
		return;
	}
	at += 8;
	while (at[n] != 0 && at[n] != ' ' && n < sizeof(pin) - 1) {
		pin[n] = at[n];
		n++;
	}
	pin[n] = 0;
	for (w = at; *w != 0 && strncmp(w, " wake ", 6) != 0; w++) {
	}
	if (*w == 0) {
		return;
	}
	for (w += 6; *w >= '0' && *w <= '9'; w++) {
		mv = mv * 10 + (uint32_t)(*w - '0');
	}
	if (mv > 5000) {
		mv /= 1000;             /* (in microvolts) */
	}
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
		uint32_t want = mv > map[i].max ? map[i].max : mv;
		bool raise = want > map[i].min;

		if (strcmp(pin, map[i].pin) != 0 || mv == 0) {
			continue;
		}
		/* the highest any pin on the same rail asked for so far */
		for (unsigned k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
			if (strcmp(map[k].rail, map[i].rail) == 0 && voted[k] >= want) {
				raise = false;
			}
		}
		voted[i] = want;
		if (raise) {
			say("ol cpr: %s on %s wants %u mV: %s\n", pin, map[i].rail, want,
			    hakios_rpmh_regulator(map[i].rail, want, true, 7) ? "voted" : "NOT VOTED");
		}
	}
}

static void
plat_log(int level __unused, const char *line)
{
	static int32_t trace = -1;

	/* the lines that come by the dozen and say nothing a run needs: past ~1.4 MB the loader's live log no longer
	 * comes back at all (runs wifi15, wifi18: only the loader's own 32 KB) */
	static const char *const chatter[] = { "target_reg ", "bdf download req fixed addr", "bdf download request remaining",
		"qrtr: data from", "ext irq:", "rx ce pipe ", "WMI chan scan list chan[", "mac channel [" };

	for (size_t k = 0; k < sizeof(chatter) / sizeof(chatter[0]); k++) {
		if (has(line, chatter[k])) {
			return;
		}
	}
	redwood_tl("dwifi/ath12k: %s\n", line);
	trail_keep(line);
	ol_cpr(line);
	if (rddm_at == 0 && has(line, "MHI_CB_EE_RDDM")) {
		rddm_at = now_ms();
	}
	if (trace < 0 && !PE_parse_boot_argn("dadawifi_link", &trace, sizeof(trace))) {
		trace = 0;
	}
	if (trace > 0) {
		link_state(line);
	}
	wifi_slow(1);
}
static void plat_halt(void) { say("the driver halted\n"); }

/* kernel memory the driver frees without saying how much: the size goes in the eight bytes before what is returned */
static void *
plat_alloc(size_t n)
{
	uint64_t total = n + 16;
	uint8_t *p = kalloc_data((vm_size_t)total, Z_WAITOK | Z_ZERO);

	if (p == NULL) {
		return NULL;
	}
	memcpy(p, &total, 8);
	return p + 16;
}

static void
plat_free(void *p)
{
	uint64_t total;
	uint8_t *base;

	if (p == NULL) {
		return;
	}
	base = (uint8_t *)p - 16;
	memcpy(&total, base, 8);
	kfree_data(base, (vm_size_t)total);
}

static void *
plat_dma_alloc(size_t n, uint64_t *bus)
{
	void *p = hp_alloc(n, bus);

	if (n >= (256u << 10)) {
		say("dma: %zu bytes at %#llx (%p)\n", n, bus ? *bus : 0ULL, p);
		wifi_slow(1);
	}
	return p;
}
static void plat_dma_free(size_t n __unused, void *p, uint64_t bus __unused) { hp_free(p); }

static uint64_t
plat_dma_map(void *p, size_t n __unused, int dir __unused)
{
	if ((vm_offset_t)p < hp.base_va || (vm_offset_t)p >= hp.base_va + hp.size) {
		return 0;               /* only memory already in the window is reachable; nothing is bounced */
	}
	return hp.base_pa + (uint64_t)((vm_offset_t)p - hp.base_va);
}

static void plat_dma_sync(uint64_t bus __unused, size_t n __unused, int dir __unused) { __builtin_arm_dsb(0xf); }

/* the chip's firmware revision: 2 takes the ..20 files. driver() reads it from the chip (as cnss does, from the SoC
 * hardware version at BAR + 0x224); boot-arg dadawifi_fw overrides it */
static uint32_t fw_rev = 1;

/* a firmware file by the name Linux has for it, mapped to what dada's vendor image calls it (fw-wifi-dada/image-peach) */
static int
plat_fw_get(const char *name, const uint8_t **data, size_t *size)
{
	uint32_t which = fw_rev;
	const char *base = name, *p, *file = NULL;
	uint32_t n = 0;
	uint8_t *buf;

	(void)PE_parse_boot_argn("dadawifi_fw", &which, sizeof(which));
	for (p = name; *p; p++) {
		if (*p == '/') {
			base = p + 1;
		}
	}
	if (strcmp(base, "amss.bin") == 0) {
		file = which == 2 ? "amss20.bin" : "amss.bin";
	} else if (strcmp(base, "m3.bin") == 0) {
		file = which == 2 ? "phy_ucode20.elf" : "phy_ucode.elf";
	} else if (strcmp(base, "aux_ucode.bin") == 0) {
		file = which == 2 ? "aux_ucode20.elf" : "aux_ucode.elf";
	} else if (strcmp(base, "board.bin") == 0 || strcmp(base, "board-2.bin") == 0 || strcmp(base, "caldata.bin") == 0) {
		return 2;               /* not on this phone: the driver falls back (bdwlan.e<id> for the board; no calibration file) */
	} else if (strncmp(base, "bdwlan.", 7) == 0) {
		/* the board data Android's cnss2 sends on this phone: Xiaomi's own, "Downloading BDF: peach/bd_o3.elf, size:
		 * 125280" (the chip's OTP says board 0xff, which made ath12k take the generic bdwlan.elf, and the firmware
		 * stopped in a system error right after "firmware ready": run wifi13) */
		file = "bd_o3.elf";
		{	/* boot-arg dadawifi_bd=NAME: bd_NAME.elf instead (p81: the Xiaomi Pad 8 Pro's, a board without a modem) */
			static char other[32];
			char name[16] = { 0 };

			if (PE_parse_boot_arg_str("dadawifi_bd", name, sizeof(name)) && name[0] != 0) {
				snprintf(other, sizeof(other), "bd_%s.elf", name);
				file = other;
			}
		}
		say("fw: board data %s\n", file);
	} else if (strcmp(base, "qdss_trace_config.cfg") == 0) {
		file = "qdss_trace_config_v2.cfg";      /* cnss2: "peach/qdss_trace_config_v2.cfg" */
	} else if (strcmp(base, "regdb.bin") == 0) {
		file = "regdb_xiaomi.bin";      /* "Downloading REGDB: peach/regdb_xiaomi.bin" */
	} else if (strcmp(base, "wlan_mac.bin") == 0) {
		/* the address en0 was made with (hakios_dada_wlan.c): the radio takes the same (mk-ath12k.sh 13) */
		extern bool hakios_dada_wlan_address(uint8_t *address);
		static char text[40];
		uint8_t a[6];

		(void)hakios_dada_wlan_address(a);
		snprintf(text, sizeof(text), "Intf0MacAddress=%02X%02X%02X%02X%02X%02X\n", a[0], a[1], a[2], a[3], a[4], a[5]);
		*data = (const uint8_t *)text;
		*size = strlen(text);
		return 0;
	} else {
		file = base;            /* the rest: as they are */
	}
	buf = fw_file(file, &n);
	if (buf == NULL) {
		say("fw: no /redwood/fw/%s (for %s)\n", file, base);
		return 2;
	}
	say("fw: %s (for %s): %u bytes at %p\n", file, base, n, buf);
	link_state("the firmware file");
	wifi_slow(5);
	link_state("a pause after it");
	*data = buf;
	*size = n;
	return 0;
}

/* the endpoint's configuration space, through the iATU window at ep_conf; an offset with bit 31 set is inside the PCI
 * Express capability (nw_board: the MHI host reads the device and link control registers that way) */
static uint16_t cfg_pcie;

static int
cfg_read(int off, int size, uint32_t *v)
{
	uint32_t real = (off & 0x80000000u) ? (uint32_t)cfg_pcie + (off & 0xffff) : (uint32_t)off;
	uint32_t word;

	if (ep_conf == 0 || real + 4 > 0x1000) {
		return 1;
	}
	word = rd(ep_conf + (real & ~3u));
	*v = size >= 4 ? word : (word >> ((real & 3) * 8)) & ((1u << (size * 8)) - 1);
	return 0;
}

static int
cfg_write(int off, int size, uint32_t v)
{
	uint32_t real = (off & 0x80000000u) ? (uint32_t)cfg_pcie + (off & 0xffff) : (uint32_t)off;

	if (ep_conf == 0 || real + 4 > 0x1000) {
		return 1;
	}
	if (size >= 4) {
		wr(ep_conf + (real & ~3u), v);
	} else {
		uint32_t shift = (real & 3) * 8, mask = ((1u << (size * 8)) - 1) << shift;

		wr(ep_conf + (real & ~3u), (rd(ep_conf + (real & ~3u)) & ~mask) | ((v << shift) & mask));
	}
	return 0;
}

static uint64_t msi_sink_pa;
static int
msi_target(uint32_t *lo, uint32_t *hi, uint32_t *data)
{
	if (msi_sink_pa == 0) {
		return 1;
	}
	*lo = (uint32_t)msi_sink_pa;            /* the chip's interrupt writes land here and are never read: the driver polls */
	*hi = (uint32_t)(msi_sink_pa >> 32);
	*data = 0;
	return 0;
}

static void *map_phys(uint64_t pa, size_t n) { return (void *)ml_io_map((vm_offset_t)pa, (vm_size_t)n); }

/*
 * The chip from cold again, for the driver's recovery (tools/mk-ath12k.sh 12 and 14): a firmware that hung leaves the
 * chip's boot ROM in SYS_ERR, which neither the driver's soft reset nor MHI's reset clears. As Android's cnss recovers
 * it: WLAN_EN down and up and the PCIe link trained again -- link_up(), which does both and puts BAR0 where it was (the
 * driver's mapping of it stays good) -- and what nw_pci.c set at the probe, the device control's read size at its
 * smallest. The chip's interrupts need nothing: they are not delivered here anyway (the driver polls).
 */
/*
 * A settle after the driver creates the scan vdev, before its first WMI set_param (tools/mk-ath12k.sh 15): boot-arg
 * dadawifi_settle=MS, off (0) by default. The firmware stops answering WMI right there in most boots; this is the
 * "it is not done with vdev_create yet" experiment.
 */
/* skip the NSS set_param after vdev create (tools/mk-ath12k.sh 16): boot-arg dadawifi_nonss=1, cld3 does not send it */
int hakios_dada_wifi_skip_nss(void);

int
hakios_dada_wifi_skip_nss(void)
{
	static uint32_t skip = 0xffffffffu;

	if (skip == 0xffffffffu) {
		skip = 0;
		(void)PE_parse_boot_argn("dadawifi_nonss", &skip, sizeof(skip));
		say("ath12k: the post-create NSS set_param is %s (dadawifi_nonss)\n", skip ? "SKIPPED" : "sent");
	}
	return skip != 0;
}

/* idle power save before the first vdev (tools/mk-ath12k.sh 17): boot-arg dadawifi_noidleps=1 leaves it off */
int hakios_dada_wifi_idle_ps(void);

int
hakios_dada_wifi_idle_ps(void)
{
	uint32_t no = 0;

	(void)PE_parse_boot_argn("dadawifi_noidleps", &no, sizeof(no));
	say("ath12k: idle power save is %s (dadawifi_noidleps)\n", no ? "NOT enabled" : "enabled");
	return no == 0;
}

/* the hw mode sent at start (tools/mk-ath12k.sh 18): boot-arg dadawifi_hwmode=N, 255 = none (as cld3), default 1 = DBS */
int hakios_dada_wifi_hw_mode(void);

int
hakios_dada_wifi_hw_mode(void)
{
	uint32_t m = 1;

	(void)PE_parse_boot_argn("dadawifi_hwmode", &m, sizeof(m));
	say("ath12k: hw mode %u sent (dadawifi_hwmode; 255 = none)\n", m);
	return m == 255 ? -1 : (int)m;
}

/* qcacld's SAR TX power limit at start (tools/mk-ath12k.sh 19): boot-arg dadawifi_sar=N, 0 = not sent */
int hakios_dada_wifi_sar(void);

int
hakios_dada_wifi_sar(void)
{
	uint32_t v = 0;

	(void)PE_parse_boot_argn("dadawifi_sar", &v, sizeof(v));
	say("ath12k: SAR TX power limit %u sent (dadawifi_sar; 0 = none)\n", v);
	return (int)v;
}

/* passive scans only (tools/mk-ath12k.sh 20): boot-arg dadawifi_passive=1 */
int hakios_dada_wifi_passive(void);

int
hakios_dada_wifi_passive(void)
{
	static uint32_t v = 0xffffffffu;

	if (v == 0xffffffffu) {
		v = 0;
		(void)PE_parse_boot_argn("dadawifi_passive", &v, sizeof(v));
		say("ath12k: scans are %s (dadawifi_passive)\n", v ? "PASSIVE only" : "active when asked");
	}
	return v != 0;
}

/* the nm_modem host capability (tools/mk-ath12k.sh 21): boot-arg dadawifi_nmmodem=1, "not a Qualcomm platform" + internal sleep clock */
int hakios_dada_wifi_nm_modem(void);

int
hakios_dada_wifi_nm_modem(void)
{
	uint32_t v = 0;

	(void)PE_parse_boot_argn("dadawifi_nmmodem", &v, sizeof(v));
	say("ath12k: host capability nm_modem (HOST_CSTATE + internal sleep clock) %s (dadawifi_nmmodem)\n", v ? "SENT" : "not sent");
	return v != 0;
}

/* the host capability's feature list (tools/mk-ath12k.sh 23): boot-arg dadawifi_features=MASK, else the driver's own */
/* HTC's transmit credits as the firmware offers them, or at most dadawifi_credits=N (Android: gWmiCreditCount=1) */
uint32_t hakios_dada_wifi_credits(uint32_t offered);

uint32_t
hakios_dada_wifi_credits(uint32_t offered)
{
	uint32_t most = 0;

	if (PE_parse_boot_argn("dadawifi_credits", &most, sizeof(most)) && most != 0 && most < offered) {
		say("HTC credits: the firmware offers %u, %u taken (one WMI command at a time, as Android)\n", offered, most);
		return most;
	}
	say("HTC credits: the firmware offers %u\n", offered);
	return offered;
}

/* the firmware's voltage-corner mode (pdev param 0x9a), boot-arg dadawifi_vcmode=N; 0: none sent */
uint32_t hakios_dada_wifi_vcmode(void);

uint32_t
hakios_dada_wifi_vcmode(void)
{
	uint32_t vc = 0;

	(void)PE_parse_boot_argn("dadawifi_vcmode", &vc, sizeof(vc));
	if (vc != 0) {
		say("voltage-corner mode %#x sent (pdev param 0x9a, as cld3's gConfigVCmode)\n", vc);
	}
	return vc;
}

/* cld3's coex configuration right after a vdev is created (tools/mk-ath12k.sh 28), boot-arg dadawifi_coex=1 */
int hakios_dada_wifi_coex(void);

int
hakios_dada_wifi_coex(void)
{
	uint32_t on = 0;

	(void)PE_parse_boot_argn("dadawifi_coex", &on, sizeof(on));
	if (on) {
		say("coex configuration sent after the vdev's creation (as cld3's ini defaults)\n");
	}
	return on != 0;
}

/*
 * At the hang (tools/mk-ath12k.sh 29), boot-arg dadawifi_solrddm=1: host SOL (gpio202, the cnss node's wlan-host-sol-gpio)
 * driven high, which makes the firmware crash and dump (cnss_pci_assert_host_sol); dev SOL (gpio203) watched for its answer.
 * The dump is read by rddm_scan() once MHI says RDDM. True: done (no restart).
 */
int hakios_dada_wifi_sol_crash(void);

int
hakios_dada_wifi_sol_crash(void)
{
	uint32_t on = 0;

	if (!PE_parse_boot_argn("dadawifi_solrddm", &on, sizeof(on)) || on == 0 || tlmm == 0) {
		return 0;
	}
	say("stuck: host SOL (gpio202) high to make the firmware dump; dev SOL (gpio203) reads %u\n", rd(tlmm + 0x1000UL * 203 + 4) & 1);
	pin(202, 1u | 1u << 6 | 1u << 9, 1);
	for (unsigned i = 0; i < 30; i++) {
		ms(100);
		if (rd(tlmm + 0x1000UL * 203 + 4) & 1) {
			say("stuck: dev SOL went high after %u ms: the firmware answers\n", (i + 1) * 100);
			break;
		}
	}
	say("stuck: host SOL cfg %#x io %#x; dev SOL %u\n", rd(tlmm + 0x1000UL * 202), rd(tlmm + 0x1000UL * 202 + 4),
	    rd(tlmm + 0x1000UL * 203 + 4) & 1);
	/*
	 * The link goes down behind the dump (cnss: "expecting link down"), and the driver's next register read over it ended the
	 * whole system (w151a). So nothing of the chip is touched from here: the dump is read where the chip wrote it, in our
	 * memory (the RDDM vectors MHI programmed at power-up), and this thread -- the driver's -- never returns to it.
	 */
	ms(5000);
	say("stuck: 5 s after host SOL: reading the dump from our memory\n");
	rddm_scan();
	for (;;) {
		ms(60000);
	}
	return 1;
}

/* the calibration's power cycle cold, as Android's (WLAN_EN down, the link trained again): boot-arg dadawifi_cbccold=1 */
int hakios_dada_wifi_cbc_cold(void);

int
hakios_dada_wifi_cbc_cold(void)
{
	uint32_t on = 0;

	(void)PE_parse_boot_argn("dadawifi_cbccold", &on, sizeof(on));
	if (on) {
		say("cbc: the chip power-cycled cold between calibration and mission mode, as Android (%s the power-down)\n",
		    on == 2 ? "after" : on == 3 ? "3 s after OFF, before" : "before");
	}
	return (int)on;
}

/* whether MHI has said RDDM since `since' (ms, now_ms()): the calibration firmware's own dump begun (dadawifi_cbccold=4) */
int hakios_dada_wifi_rddm_seen(uint64_t since);

int
hakios_dada_wifi_rddm_seen(uint64_t since)
{
	return rddm_at != 0 && rddm_at >= since;
}

uint64_t hakios_dada_wifi_features(uint64_t own);

uint64_t
hakios_dada_wifi_features(uint64_t own)
{
	uint64_t want = own;

	(void)PE_parse_boot_argn("dadawifi_features", &want, sizeof(want));
	say("ath12k: host capability features %#llx (dadawifi_features; the driver's %#llx)\n", want, own);
	return want;
}

/* no cold-boot calibration (tools/mk-ath12k.sh 24): boot-arg dadawifi_nocbc=1 */
int hakios_dada_wifi_no_cbc(void);

int
hakios_dada_wifi_no_cbc(void)
{
	uint32_t no = 0;

	(void)PE_parse_boot_argn("dadawifi_nocbc", &no, sizeof(no));
	if (no) {
		say("ath12k: no cold-boot calibration (dadawifi_nocbc)\n");
	}
	return no != 0;
}

void hakios_dada_wifi_settle(void);

void
hakios_dada_wifi_settle(void)
{
	static uint32_t settle = 0xffffffffu;

	if (settle == 0xffffffffu) {
		settle = 0;
		(void)PE_parse_boot_argn("dadawifi_settle", &settle, sizeof(settle));
		if (settle != 0) {
			say("ath12k: a %u ms settle after each vdev create (dadawifi_settle)\n", settle);
		}
	}
	if (settle != 0) {
		ms(settle);
	}
}

/* a cold power cycle just happened: the driver's next MHI reset of the chip is skipped (tools/mk-ath12k.sh 22) */
static volatile int wifi_just_cycled;
int hakios_dada_wifi_just_cycled(void);

int
hakios_dada_wifi_just_cycled(void)
{
	int was = wifi_just_cycled;
	uint32_t want = 0;

	/* opt-in (boot-arg dadawifi_skipreset=1): with it, run w95a1 stalled the whole phone right after the hang */
	(void)PE_parse_boot_argn("dadawifi_skipreset", &want, sizeof(want));
	if (!want) {
		was = 0;
	}
	wifi_just_cycled = 0;
	if (was) {
		say("ath12k: MHI reset skipped: the chip was just power-cycled\n");
	}
	return was;
}

void hakios_dada_wifi_cycle(void);

void
hakios_dada_wifi_cycle(void)
{
	uint32_t ctl = 0;

	wifi_just_cycled = 1;

	/*
	 * A live chip (the calibration firmware just told OFF) may still have its own transactions under way -- MSI writes, DMA:
	 * cut by WLAN_EN they killed the whole SoC about half the time (w163a, w168a, w170a, w173a), where a hung chip's never
	 * did. Bus mastering off first, then a moment, then the cut.
	 */
	{
		uint32_t command = 0;

		if (ep_conf != 0 && cfg_read(0x04, 2, &command) == 0) {
			(void)cfg_write(0x04, 2, command & ~4u);
			ms(10);
			say("ath12k: the chip's bus mastering off before the cut (command %#x -> %#x)\n", command, command & ~4u);
		}
	}
	say("ath12k: the chip is power-cycled (WLAN_EN) and its link trained again\n");
	link_up();
	if (ep_mhi == 0 || ep_conf == 0) {
		say("ath12k: the link did not come back after the power cycle\n");
		return;
	}
	if (cfg_read(0x80000008, 2, &ctl) == 0) {
		(void)cfg_write(0x80000008, 2, ctl & ~0x7000u);
	}
	say("ath12k: the chip is back: MHI status %#x, device control %#x\n", rd(ep_mhi + 0x48), ctl & ~0x7000u);
	say("ath12k: the cycle returns\n");
}

/* a mark from the driver's own code (tools/ath12k-cbc.py), said through this file's log rather than the shim's */
void hakios_dada_wifi_note(const char *what);

void
hakios_dada_wifi_note(const char *what)
{
	say("note: %s\n", what);
}

static void
driver(void)
{
	static const struct lx_platform plat = {
		now_ms, plat_sleep, plat_poll, plat_log, plat_halt, plat_alloc, plat_free,
		plat_dma_alloc, plat_dma_free, plat_dma_map, plat_dma_sync, plat_fw_get,
	};
	static struct nw_board board;           /* the driver keeps a pointer to it: it outlives this call */
	uint64_t low = 0, mb = 0, sink;
	uint32_t cap, guard, stage_was = 0, scans_was = 0;
	int r;

	if (nw_wifi_start == NULL) {
		say("ath12k: the driver's object is not in this kernel (tools/mk-ath12k.sh)\n");
		return;
	}
	if (ep_mhi == 0 || ep_conf == 0) {
		say("ath12k: the link is not up\n");
		return;
	}
	if (!PE_parse_boot_argn("lowdma", &low, sizeof(low)) || !PE_parse_boot_argn("lowdma_mb", &mb, sizeof(mb)) || mb < 48 ||
	    low == 0 || low + (mb << 20) > 0x100000000ULL) {
		say("ath12k: no 48 MB below 4 GB from the loader (lowdma=, lowdma_mb=): a loader from before 28 October\n");
		return;
	}
	/* from 4 MB on, rounded up to the kernel's 16 KB page: lowdma= is only 4 KB aligned (0x...c3000), and io_map
	 * maps whole pages, so an unaligned start would share its first page with the sound's memory below and take
	 * that page outside the Normal non-cacheable range set below */
	hp.base_pa = (low + (4u << 20) + 0x3fffULL) & ~0x3fffULL;
	hp.size = ((low + (mb << 20)) - hp.base_pa) & ~0x3fffULL;
	/* Normal non-cacheable, not Device: the driver bzero()s and memcpy()s this memory, and the loader's
	 * lowdma= is outside the kernel's RAM window, where VM_WIMG_WCOMB would otherwise be Device-GRE
	 * (osfmk/arm/pmap/pmap.c wimg_to_pte): bzero's DC ZVA there was "Unaligned kernel data abort" */
	hakios_normal_nc_lo = hp.base_pa;
	hakios_normal_nc_hi = hp.base_pa + hp.size;
	hp.base_va = ml_io_map_wcomb((vm_offset_t)hp.base_pa, (vm_size_t)hp.size);
	hp.big = kalloc_data((vm_size_t)(hp.size >> 12), Z_WAITOK | Z_ZERO);
	if (hp.base_va == 0) {
		return;
	}
	/* the root port's own stream as well (iommu-map's first entry): what the controller tags DMA with when its
	 * BDF-to-SID table is bypassed, so a missed mapping is a stream passed through and not a fault */
	if (!smmu_pass(0x1400)) {
		say("ath12k: stream 0x1400 could not be passed through\n");
	}
	if (!smmu_pass(WIFI_STREAM)) {
		say("ath12k: the endpoint's SMMU stream could not be passed through\n");
		return;
	}
	/* the configuration-space capability list: the MSI capability (0x05) and the PCI Express capability (0x10) */
	cap = rd(ep_conf + 0x34) & 0xff;
	for (guard = 0; cap >= 0x40 && cap + 4 <= 0x1000 && guard < 48; guard++) {
		uint32_t w = rd(ep_conf + cap);

		if ((w & 0xff) == 0x05) {
			board.msi_cap = (uint16_t)cap;
		} else if ((w & 0xff) == 0x10) {
			cfg_pcie = (uint16_t)cap;
		}
		cap = (w >> 8) & 0xff;
	}
	sink = 0;
	if (hp_alloc(4096, &sink) == NULL) {
		return;
	}
	msi_sink_pa = sink;
	/* the chip's revision from its SoC hardware version (cnss cnss_get_soc_info_pre_mhi: BAR + 0x224, major in bits
	 * 8..11), so the ..20 firmware is taken for a revision-2 chip without being told which (dadawifi_fw overrides) */
	{
		uint32_t soc = rd(ep_mhi + 0x224), major = (soc >> 8) & 0xf;

		fw_rev = major >= 2 ? 2 : 1;
		say("ath12k: SoC hardware version %#x: revision %u, firmware %s\n", soc, major, fw_rev == 2 ? "..20" : "default");
	}
	board.bar = (void *)ep_mhi;
	board.bar_len = (size_t)ep_bar_size;
	board.device_id = 0x110e;
	board.irq_base = 1;
	board.cfg_read = cfg_read;
	board.cfg_write = cfg_write;
	board.msi_target = msi_target;
	board.map_phys = map_phys;
	say("ath12k: starting the driver: BAR %u KiB, MSI capability %#x, PCIe capability %#x, DMA %llu MB at %#llx, sink %#llx\n",
	    (uint32_t)(ep_bar_size >> 10), board.msi_cap, cfg_pcie, hp.size >> 20, hp.base_pa, sink);
	{
		extern void hakios_dada_wlan_events(void);

		hakios_dada_wlan_events();     /* scan, join, link and frames to en0 and iOS (hakios_dada_wlan.c) */
	}
	r = nw_wifi_start(&plat, &board);
	say("ath12k: the driver's start returns %d (stage %u, last error %d)\n", r, nw_wifi_stats.stage, nw_wifi_stats.last_err);
	if (r != 0) {
		return;
	}
	/* the driver runs on this thread: it is pumped, and where it stands is said as it moves and every few seconds */
	for (uint32_t i = 0, quiet = 0; ; i++) {
		extern void hakios_dada_wlan_poll(void);
		int moved = nw_wifi_step();

		hakios_dada_wlan_poll();       /* the station attached once the driver's interface is up */

		if (rddm_at != 0 && !rddm_scanned && now_ms() - rddm_at > 3000) {
			rddm_scanned = true;
			rddm_scan();
		}

		if (!moved) {
			/* boot-arg dadawifi_pollus=N: the idle pause in microseconds (default 2 ms): how late the chip's
			 * interrupts are looked at -- a test of whether its death at the first radio work is a latency one */
			static uint32_t poll_us = 0xffffffffu;

			if (poll_us == 0xffffffffu) {
				poll_us = 2000;
				(void)PE_parse_boot_argn("dadawifi_pollus", &poll_us, sizeof(poll_us));
				say("ath12k: the driver's idle pause: %u us (dadawifi_pollus)\n", poll_us);
			}
			if (poll_us >= 1000) {
				ms(poll_us / 1000);
			} else {
				us(poll_us);
			}
			quiet += poll_us >= 2000 ? 1 : (i % (2000 / (poll_us ? poll_us : 1)) == 0);
		} else {
			quiet = 0;
		}
		if (nw_wifi_stats.stage != stage_was || nw_wifi_stats.scans != scans_was || (quiet % 1500) == 1499) {
			say("ath12k: stage %u, scans %u, networks seen %u, error %d; where %llu/%#llx depth %llu\n",
			    nw_wifi_stats.stage, nw_wifi_stats.scans, nw_wifi_stats.bss_seen, nw_wifi_stats.last_err,
			    nw_wifi_where[0], nw_wifi_where[1], nw_wifi_where[2]);
			stage_was = nw_wifi_stats.stage;
			scans_was = nw_wifi_stats.scans;
		}
		if (nw_wifi_stats.scans > 0 && (i % 20000) == 19999) {
			say("ath12k: still up (DMA %u KiB of %llu MB used, %u live, %u out-of-memory)\n",
			    (uint32_t)(hp.peak >> 10), hp.size >> 20, hp.live, hp.fails);
		}
	}
}

void hakios_dada_wifi_init(void);

static void
wifi_thread(void *arg, wait_result_t w __unused)
{
	uint32_t stage = (uint32_t)(uintptr_t)arg;

	/* (after the audio DSP: RPMh's command database and sequencer are set up by then) */
	ms(20000);
	{
		/* the modem first, when it is brought up (dadampss=1): with the Wi-Fi's start beside it, it never came up (w131, w133a) */
		extern volatile int hakios_mpss_state;
		uint32_t mpss = 0;

		if (PE_parse_boot_argn("dadampss", &mpss, sizeof(mpss)) && mpss != 0) {
			for (unsigned waited = 0; hakios_mpss_state == 0 && waited < 150; waited++) {
				ms(1000);
			}
			say("the modem is %s: the Wi-Fi goes on\n", hakios_mpss_state > 0 ? "up" : "NOT up");
		}
	}
	look("as the boot left it");
	/*
	 * The chip from cold (boot-arg dadawifi_coldms=MS; default 0: not): Android reboots into the loader with Wi-Fi and
	 * Bluetooth up, and a warm reset leaves the TLMM and PMIC pins as they were. (2 s of both enables low did not
	 * change the firmware's death at its first radio work: run wifi40, 28 October.)
	 */
	{
		uint32_t cold = 0;
		int apid = spmi_apid(0x58a);

		(void)PE_parse_boot_argn("dadawifi_coldms", &cold, sizeof(cold));
		if (cold != 0 && stage >= 2) {
			pin(PIN_WLAN_EN, 1u | 7u << 6 | 1u << 9, 0);
			if (apid >= 0) {
				(void)spmi_write8(apid, 0x40, 1);               /* BT_EN: digital output */
				(void)spmi_write8(apid, 0x44, 0x00);            /* source: constant low */
				(void)spmi_write8(apid, 0x46, 0x80);
			}
			say("the chip held off (WLAN_EN and BT_EN low) for %u ms\n", cold);
			ms(cold);
		}
	}
	{
		uint32_t bt = 0;

		if (PE_parse_boot_argn("dadabt", &bt, sizeof(bt)) && bt != 0) {
			rails();
			sleep_clock();
			bt_probe();
			if (bt >= 2) {
				bt_hci(bt);
			}
		}
	}
	if (stage >= 2) {
		link_up();
	}
	if (stage == 3) {
		firmware();
	} else if (stage >= 4) {
		driver();
	}
}

void
hakios_dada_wifi_init(void)
{
	uint32_t stage = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("dadawifi", &stage, sizeof(stage)) || stage == 0) {
		return;
	}
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	tlmm = ml_io_map(TLMM_PHYS, 0x200000);
	{
		uint32_t bt = 0;

		/* /dev/uart.bluetooth exists from the start, before the chip is up: iOS's bluetoothd looks for it at once
		 * and gives up for the boot if it is not there yet (opens wait for the chip) */
		if (PE_parse_boot_argn("dadabt", &bt, sizeof(bt)) && bt >= 4) {
			hakios_dada_bt_make_node();
		}
	}
	if (stage >= 4) {
		extern void hakios_dada_wlan_early(void);

		hakios_dada_wlan_early();      /* en0 now: iOS's Wi-Fi side looks for it once, at its start (hakios_dada_wlan.c) */
	}
	if (kernel_thread_start(wifi_thread, (void *)(uintptr_t)stage, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
