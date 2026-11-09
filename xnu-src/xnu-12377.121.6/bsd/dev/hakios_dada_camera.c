/*
 * The Xiaomi 15's (dada) cameras, first steps. Boot-arg dadacam=1: the wide camera's sensor (OmniVision OVX9100,
 * Android's camera 0, DT qcom,cam-sensor0) powered up as its module file says, its chip ID read, powered down again.
 * Numbers from ~/hakios-gpu2/patches/camera-dada/bringup-spec.md (DT, mainline sm8750, the vendor module files):
 *
 *   I2C         CCI 0 master 1 pins, gpio115 data, gpio116 clock -- driven here by hand as plain GPIOs (as
 *               redwood_camera.c does), so that neither the camera's power domain nor the CCI's clocks are needed
 *   address     0x10; chip ID: 16-bit register 0x300a = 0x5650
 *   MCLK        MCLK1 of the camera's MCLK clock controller (cambistmclkcc, 0x01760000): RCG +0x401c, branch +0x4034,
 *               19.2 MHz straight from the crystal; pin gpio90, function 1
 *   reset       gpio6, high = running
 *   supplies    BOB2 (bobb2), VANA 2.9 V a fixed regulator gpio127 switches, VIO 1.8 V (ldob10), VDIG 1.2 V (ldoi1)
 *   power-up    reset low; BOB2, 2 ms; VANA; VIO, 1 ms; VDIG, 1 ms; MCLK, 1 ms; reset high, 5 ms
 *   power-down  MCLK off, 1 ms; reset low, 1 ms; VDIG, VIO, VANA off
 *
 * Boot-arg dadacam_keep=1 leaves the sensor powered (for the next steps).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern bool hakios_rpmh_regulator(const char *name, uint32_t mv, bool enable, uint32_t mode);
extern bool hakios_rpmh_level(const char *name, uint32_t level);

#define say(...) redwood_tl("dcam: " __VA_ARGS__)

#define TLMM_PHYS               0x0f100000UL
#define MCLKCC_PHYS             0x01760000UL
#define GCC_PHYS                0x00100000UL
#define MCLK1_CMD               0x401c
#define MCLK1_CFG               0x4020
#define MCLK1_CBCR              0x4034

#define PIN_SDA                 115
#define PIN_SCL                 116
#define PIN_MCLK                90
#define PIN_RESET               6
#define PIN_VANA                127
#define SENSOR_ADDR             0x10
#define CHIP_ID_REG             0x300a
#define CHIP_ID                 0x5650

#define CFG_PULL_UP             3u
#define CFG_FUNC(f)             ((uint32_t)(f) << 2)
#define CFG_DRIVE(ma)           ((uint32_t)((ma) / 2 - 1) << 6)
#define CFG_OUTPUT              (1u << 9)

static vm_offset_t pin_page[180];
static uint32_t egpio_take = 1, skip, hwctl_off;     /* boot-args dadacam_egpio (1), dadacam_skip (1: no BOB2) */
static vm_offset_t mclkcc;

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

static vm_offset_t
pin(unsigned n)
{
	if (pin_page[n] == 0) {
		pin_page[n] = ml_io_map(TLMM_PHYS + 0x1000 * n, 0x1000);
	}
	return pin_page[n];
}

/* a pin's configuration; an eGPIO pad (bit 11) also needs bit 12 for the APPS to own it */
static void
pin_cfg(unsigned n, uint32_t cfg)
{
	uint32_t now = rd(pin(n));

	if (now & (1u << 11)) {
		cfg |= 1u << 11 | (egpio_take ? 1u << 12 : (now & (1u << 12)));
	}
	wr(pin(n), cfg);
}

static void
pin_out(unsigned n, bool high)
{
	wr(pin(n) + 4, high ? 2 : 0);
	pin_cfg(n, CFG_DRIVE(2) | CFG_OUTPUT);
}

/* ---- I2C by hand: an output low or let go (pulled up) ---- */

static void
half_bit(void)
{
	us(5);
}

static void
line(unsigned n, bool high)
{
	wr(pin(n) + 4, 0);
	pin_cfg(n, CFG_PULL_UP | CFG_DRIVE(2) | (high ? 0 : CFG_OUTPUT));
}

static bool
level(unsigned n)
{
	return rd(pin(n) + 4) & 1;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !level(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

/* a 16-bit register at a 16-bit address: false if nobody acknowledged (which byte is said) */
static bool
sensor_read16(uint8_t addr, uint16_t reg, uint16_t *value)
{
	const char *failed = NULL;
	uint8_t hi, lo;

	i2c_start();
	if (!i2c_write((uint8_t)(addr << 1))) {
		failed = "its address";
	} else if (!i2c_write((uint8_t)(reg >> 8)) || !i2c_write((uint8_t)reg)) {
		failed = "the register's address";
	}
	if (failed == NULL) {
		i2c_start();
		if (!i2c_write((uint8_t)(addr << 1 | 1))) {
			failed = "its address (read)";
		}
	}
	if (failed != NULL) {
		i2c_stop();
		say("0x%02x: no acknowledgement of %s\n", addr, failed);
		return false;
	}
	hi = i2c_read(true);
	lo = i2c_read(false);
	i2c_stop();
	*value = (uint16_t)(hi << 8 | lo);
	return true;
}

/* ---- the sensor's power ---- */

/* GCC's always-on branches for the camera (gcc-sm8750.c keeps them on): the MCLK controller's AHB, camera AHB and XO */
static void
gcc_camera_branches(void)
{
	static const uint32_t branches[] = { 0xa0004, 0x26004, 0x26034 };
	vm_offset_t gcc = ml_io_map(GCC_PHYS, 0x200000);

	for (unsigned i = 0; i < 3; i++) {
		uint32_t before = rd(gcc + branches[i]);

		wr(gcc + branches[i], (before | 1) & ~(hwctl_off ? 2u : 0u));     /* dadacam_hwctl=0: hardware gating off too */
		for (int t = 0; t < 100 && (rd(gcc + branches[i]) & (1u << 31)); t++) {
			us(10);
		}
		say("GCC branch %#x: %#x -> %#x\n", branches[i], before, rd(gcc + branches[i]));
	}
}

static bool
mclk(bool on)
{
	if (mclkcc == 0) {
		mclkcc = ml_io_map(MCLKCC_PHYS, 0x10000);
	}
	if (!on) {
		wr(mclkcc + MCLK1_CBCR, rd(mclkcc + MCLK1_CBCR) & ~1u);
		wr(mclkcc + MCLK1_CMD, rd(mclkcc + MCLK1_CMD) & ~2u);
		pin_out(PIN_MCLK, false);
		return true;
	}
	wr(mclkcc + 0x40f8, rd(mclkcc + 0x40f8) | 1);   /* the controller's always-on sleep clock (critical in mainline) */
	/*
	 * Two registers of the controller's that neither mainline nor our earlier dumps (0x4000..0x40ff) covered: Android has
	 * 0x4100 = 0xf and 0x410c = 0x3 while its camera runs (regmap dump, 31 Oct) -- by the look of them the MCLK outputs'
	 * enables. Here: as Android's.
	 */
	say("MCLK controller before: 0x4100 %#x 0x410c %#x\n", rd(mclkcc + 0x4100), rd(mclkcc + 0x410c));
	wr(mclkcc + 0x4100, 0xf);
	wr(mclkcc + 0x410c, 0x3);
	wr(mclkcc + MCLK1_CMD, rd(mclkcc + MCLK1_CMD) | 2);     /* the root forced on */
	wr(mclkcc + MCLK1_CFG, 0x100001);               /* the crystal, divided by 1 (bit 20 kept, as Android has it: 0x00100001) */
	wr(mclkcc + MCLK1_CMD, rd(mclkcc + MCLK1_CMD) | 1);
	for (int i = 0; i < 100 && (rd(mclkcc + MCLK1_CMD) & 1); i++) {
		us(10);
	}
	wr(mclkcc + MCLK1_CBCR, rd(mclkcc + MCLK1_CBCR) | 1);
	for (int i = 0; i < 100 && (rd(mclkcc + MCLK1_CBCR) & (1u << 31)); i++) {
		us(10);
	}
	pin_cfg(PIN_MCLK, CFG_FUNC(1) | CFG_DRIVE(2));
	{
		unsigned ones = 0;

		for (unsigned i = 0; i < 1000; i++) {
			ones += rd(pin(PIN_MCLK) + 4) & 1;
		}
		say("MCLK1 at gpio90: %u of 1000 samples high (%s)\n", ones, ones > 0 && ones < 1000 ? "it toggles" : "STILL");
	}
	{
		char line[400];
		int n = 0;

		for (uint32_t off = 0x4000; off < 0x4100 && n < (int)sizeof(line) - 20; off += 4) {
			uint32_t v = rd(mclkcc + off);

			if (v != 0) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %x=%x", off, v);
			}
		}
		say("MCLK controller 0x4000..:%s\n", line);
		n = 0;
		for (uint32_t off = 0x0; off < 0x60 && n < (int)sizeof(line) - 20; off += 4) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %x", rd(mclkcc + off));
		}
		say("MCLK controller PLL0 0x0..:%s; 0x5000.. %x %x %x %x\n", line, rd(mclkcc + 0x5000), rd(mclkcc + 0x5004),
		    rd(mclkcc + 0x5008), rd(mclkcc + 0x500c));
	}
	{
		/* (a test: MCLK0's branch too, from its own RCG at the crystal) */
		wr(mclkcc + 0x4000, rd(mclkcc + 0x4000) | 2);
		wr(mclkcc + 0x4004, 0x100001);
		wr(mclkcc + 0x4000, rd(mclkcc + 0x4000) | 1);
		us(100);
		wr(mclkcc + 0x4018, rd(mclkcc + 0x4018) | 1);
		us(100);
		say("MCLK0 (a test): cmd %#x cfg %#x branch %#x\n", rd(mclkcc + 0x4000), rd(mclkcc + 0x4004), rd(mclkcc + 0x4018));
	}
	say("MCLK1: cmd %#x cfg %#x branch %#x (%s); pin %#x\n", rd(mclkcc + MCLK1_CMD), rd(mclkcc + MCLK1_CFG),
	    rd(mclkcc + MCLK1_CBCR), (rd(mclkcc + MCLK1_CBCR) & (1u << 31)) ? "STILL OFF" : "running", rd(pin(PIN_MCLK)));
	return !(rd(mclkcc + MCLK1_CBCR) & (1u << 31));
}

static void
power_down(void)
{
	(void)mclk(false);
	us(1000);
	pin_out(PIN_RESET, false);
	us(1000);
	(void)hakios_rpmh_regulator("ldoi1", 0, false, 0);
	(void)hakios_rpmh_regulator("ldob10", 0, false, 0);
	pin_out(PIN_VANA, false);
	say("the wide sensor powered down\n");
}

static void
camera_thread(void *arg __unused, wait_result_t w __unused)
{
	uint32_t keep = 0;
	uint16_t id = 0;
	bool ok;

	(void)PE_parse_boot_argn("dadacam_keep", &keep, sizeof(keep));
	(void)PE_parse_boot_argn("dadacam_egpio", &egpio_take, sizeof(egpio_take));
	(void)PE_parse_boot_argn("dadacam_skip", &skip, sizeof(skip));
	(void)PE_parse_boot_argn("dadacam_hwctl_off", &hwctl_off, sizeof(hwctl_off));
	{
		uint32_t wait_s = 40;           /* after the other drivers' start (an RPMh vote at ~23 s, with the GPU's start, ended the system) */

		(void)PE_parse_boot_argn("dadacam_delay", &wait_s, sizeof(wait_s));
		delay_for_interval(wait_s * 1000, NSEC_PER_MSEC);
	}
	say("the wide sensor (OVX9100): pins as found: sda %#x/%#x scl %#x/%#x mclk %#x reset %#x/%#x vana %#x/%#x\n",
	    rd(pin(PIN_SDA)), rd(pin(PIN_SDA) + 4), rd(pin(PIN_SCL)), rd(pin(PIN_SCL) + 4), rd(pin(PIN_MCLK)), rd(pin(PIN_RESET)),
	    rd(pin(PIN_RESET) + 4), rd(pin(PIN_VANA)), rd(pin(PIN_VANA) + 4));
	pin_out(PIN_RESET, false);
	say("step: reset low (%#x/%#x)\n", rd(pin(PIN_RESET)), rd(pin(PIN_RESET) + 4));
	us(50000);
	say("step: 50 ms later, still here\n");
	ok = (skip & 1) ? true : hakios_rpmh_regulator("bobb2", 0, true, 0);
	us(2000);
	say("step: BOB2 %s\n", ok ? "on" : "NOT VOTED");
	pin_out(PIN_VANA, true);
	say("step: VANA (gpio127 %#x/%#x)\n", rd(pin(PIN_VANA)), rd(pin(PIN_VANA) + 4));
	ok = hakios_rpmh_regulator("ldob10", 1800, true, 7) && ok;
	us(1000);
	ok = hakios_rpmh_regulator("ldoi1", 1200, true, 7) && ok;
	us(1000);
	say("supplies: %s\n", ok ? "BOB2, VANA, VIO, VDIG on" : "NOT ALL VOTED");
	{
		uint32_t mmcx = 128;

		(void)PE_parse_boot_argn("dadacam_mmcx", &mmcx, sizeof(mmcx));
		say("MMCX at %u: %s\n", mmcx, hakios_rpmh_level("mmcx.lvl", mmcx) ? "voted" : "NOT VOTED");
		if (PE_parse_boot_argn("dadacam_mx", &mmcx, sizeof(mmcx)) && mmcx != 0) {
			say("MX at %u: %s\n", mmcx, hakios_rpmh_level("mx.lvl", mmcx) ? "voted" : "NOT VOTED");
		}
	}
	{
		/*
		 * The MCLK controller's crystal: Android's clock tree has it as xo_board -> xo_pad -> bi_tcxo, and bi_tcxo is an RPMh
		 * clock (clk-rpmh ARC "xo.lvl", level 3 = on) that its users vote for. Without the vote no MCLK branch ever ran.
		 */
		extern bool hakios_rpmh_write(const char *name, uint32_t value);

		say("XO for the camera (xo.lvl 3): %s\n", hakios_rpmh_write("xo.lvl", 3) ? "voted" : "NOT VOTED");
	}
	gcc_camera_branches();
	{
		/*
		 * The camera's top power domain (camcc titan_top_gdsc, GDSCR 0x1134c; spec: clear bit 0, wait for CFG_GDSCR 0x11350
		 * bit 16 POWER_ON): with Android's camera running the whole camcc is up; our MCLK branches never ran without it.
		 * Boot-arg dadacam_titan=0: not.
		 */
		uint32_t titan = 1;

		(void)PE_parse_boot_argn("dadacam_titan", &titan, sizeof(titan));
		if (titan) {
			vm_offset_t camcc = ml_io_map(0x0ade0000UL, 0x20000);
			uint32_t before = rd(camcc + 0x1134c);

			wr(camcc + 0x1134c, before & ~1u);
			for (int i = 0; i < 200 && !(rd(camcc + 0x11350) & (1u << 16)); i++) {
				us(10);
			}
			say("titan_top_gdsc: %#x -> %#x, cfg %#x (%s)\n", before, rd(camcc + 0x1134c), rd(camcc + 0x11350),
			    (rd(camcc + 0x11350) & (1u << 16)) ? "ON" : "NOT ON");
		}
	}
	ok = mclk(true) && ok;
	us(1000);
	pin_out(PIN_RESET, true);
	us(5000);
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	say("the bus at rest: sda %u scl %u\n", level(PIN_SDA), level(PIN_SCL));
	if (sensor_read16(SENSOR_ADDR, CHIP_ID_REG, &id)) {
		say("the wide sensor's chip ID: %#06x (%s)\n", id, id == CHIP_ID ? "an OVX9100, AS EXPECTED" : "not what was expected");
	}
	/* the bus's pins back to CCI's function */
	pin_cfg(PIN_SDA, CFG_FUNC(1) | CFG_DRIVE(2));
	pin_cfg(PIN_SCL, CFG_FUNC(1) | CFG_DRIVE(2));
	if (!keep) {
		power_down();
	}
}

void hakios_dada_camera_init(void);

void
hakios_dada_camera_init(void)
{
	uint32_t level = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("dadacam", &level, sizeof(level)) || level == 0) {
		return;
	}
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
