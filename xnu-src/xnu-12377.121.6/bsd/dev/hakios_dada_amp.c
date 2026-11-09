/*
 * The Xiaomi 15's (dada) two speaker amplifiers: Cirrus Logic CS35L43, on the I2C bus "hub 0"
 * (Android's device tree: qupv3_hub_i2c0 i2c@980000, SDA gpio64 and SCL gpio65; cs35l43@40 "B", the
 * bottom speaker, reset gpio107, and cs35l43@42 "T", the top one, reset gpio108; their audio comes over
 * the secondary TDM of the audio DSP's LPASS). The bus is driven by hand (hakios_dada_gauge.c).
 *
 * Step 1 of the speakers, with boot-arg dadaamp=1: the two out of reset (high, as Android's pin state
 * "default" leaves them), and each asked what it is -- its device and revision IDs, registers 0 and 4,
 * which are 32 bits wide at 32-bit addresses, most significant byte first (Linux: cs35l41 regmap).
 * Nothing else is written.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#define say(...) redwood_tl("damp: " __VA_ARGS__)
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern bool hakios_dada_i2c_xfer(unsigned sda, unsigned scl, uint8_t address, const uint8_t *w, unsigned wlen, uint8_t *r,
    unsigned rlen);
extern void hakios_dada_gpio_out(unsigned pin, bool high);

#define PIN_SDA                 64
#define PIN_SCL                 65

static const struct {
	const char *name;
	uint8_t address;
	unsigned reset;
} amps[] = { { "bottom", 0x40, 107 }, { "top", 0x42, 108 } };

/* one of the amplifier's 32-bit registers */
static bool
amp_read(uint8_t address, uint32_t reg, uint32_t *value)
{
	uint8_t w[4] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg }, r[4];

	if (!hakios_dada_i2c_xfer(PIN_SDA, PIN_SCL, address, w, 4, r, 4)) {
		return false;
	}
	*value = (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
	return true;
}

void hakios_dada_amp_init(void);

void
hakios_dada_amp_init(void)
{
	uint32_t on = 0;

	if (!PE_parse_boot_argn("dadaamp", &on, sizeof(on)) || on == 0) {
		return;
	}
	/* the resets pulsed as a driver's probe does: low, then high */
	for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
		hakios_dada_gpio_out(amps[i].reset, false);
	}
	delay_for_interval(2, NSEC_PER_MSEC);
	for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
		hakios_dada_gpio_out(amps[i].reset, true);
	}
	delay_for_interval(50, NSEC_PER_MSEC);
	{
		extern unsigned hakios_dada_gpio_state(unsigned pin);

		say("the resets: gpio107 %#x, gpio108 %#x; the interrupts: gpio84 %#x, gpio66 %#x (configuration << 4 | level)\n",
		    hakios_dada_gpio_state(107), hakios_dada_gpio_state(108), hakios_dada_gpio_state(84),
		    hakios_dada_gpio_state(66));
	}
	for (unsigned attempt = 0; attempt < 3; attempt++) {
		/* (a woken CS35L4x may NAK the transaction that wakes it) */
		for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
			(void)hakios_dada_i2c_xfer(PIN_SDA, PIN_SCL, amps[i].address, NULL, 0, NULL, 0);
		}
		delay_for_interval(2, NSEC_PER_MSEC);
	}
	{
		/* who answers on the bus at all, and how its lines rest */
		extern unsigned hakios_dada_i2c_levels(unsigned sda, unsigned scl);
		char line[200];
		int at = 0;

		for (unsigned a = 0x08; a < 0x78 && at < (int)sizeof(line) - 6; a++) {
			if (hakios_dada_i2c_xfer(PIN_SDA, PIN_SCL, (uint8_t)a, NULL, 0, NULL, 0)) {
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %#x", a);
			}
		}
		line[at] = 0;
		say("the bus (gpio64/65) rests at SDA/SCL %#x; addresses that answer:%s\n", hakios_dada_i2c_levels(PIN_SDA, PIN_SCL),
		    at ? line : " none");
	}
	for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
		uint32_t id = 0, rev = 0;
		bool ok = amp_read(amps[i].address, 0x0, &id) && amp_read(amps[i].address, 0x4, &rev);

		if (ok) {
			say("the %s speaker's amplifier (%#x): device %#x, revision %#x\n", amps[i].name, amps[i].address, id, rev);
		} else {
			say("the %s speaker's amplifier (%#x) does not answer\n", amps[i].name, amps[i].address);
		}
	}
}

/*
 * Step 2, for the sound (hakios_dada_audio.c): the amplifiers set up to play 16-bit stereo I2S at 48 kHz from
 * the secondary MI2S as clock slaves, straight from the serial port to the amplifier -- their own DSP bypassed, so
 * no firmware of theirs is needed -- the bottom one the left slot, the top one the right. The sequence is UniXNU's
 * for the same amplifier on SM8650 (ShengCs35l43.c: Cirrus's register errata, the board values Android's device
 * tree gives: 4 A boost peak, SDOUT high impedance, brownout protection).
 */
#define CS35_GLOBAL_ENABLES     0x2014
#define CS35_BLOCK_ENABLES      0x2018
#define CS35_BLOCK_ENABLES2     0x201c
#define CS35_GPIO_PAD_CONTROL   0x242c
#define CS35_REFCLK_INPUT       0x2c04
#define CS35_GLOBAL_SAMPLE_RATE 0x2c0c
#define CS35_FS_MON_0           0x2d10
#define CS35_BST_IPK_CTL        0x3808
#define CS35_BST_RSVD_1         0x3850
#define CS35_ASP_ENABLES1       0x4800
#define CS35_ASP_CONTROL2       0x4808
#define CS35_ASP_CONTROL3       0x480c
#define CS35_ASP_FRAME_CONTROL5 0x4820
#define CS35_ASP_DATA_CONTROL5  0x4840
#define CS35_DACPCM1_INPUT      0x4c00
#define CS35_AMP_CTRL           0x6000
#define CS35_VPBR_CONFIG        0x6404
#define CS35_AMP_GAIN           0x6c04
#define CS35_TST_DAC_MSM_CONFIG 0x7404
#define CS35_ALIVE_DCIN_WD      0x7424
#define CS35_IRQ1_EINT_1        0x10010
#define CS35_IRQ1_EINT_2        0x10014
#define CS35_IRQ1_MASK_1        0x10110
#define CS35_IRQ1_MASK_2        0x10114
#define CS35_IRQ1_MASK_3        0x10118
#define CS35_TEST_KEY           0x40
#define CS35_GAIN_DB            18u

static bool
amp_write(uint8_t address, uint32_t reg, uint32_t value)
{
	uint8_t w[8] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg,
		         (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };

	return hakios_dada_i2c_xfer(PIN_SDA, PIN_SCL, address, w, 8, NULL, 0);
}

static bool
amp_update(uint8_t address, uint32_t reg, uint32_t mask, uint32_t value)
{
	uint32_t old;

	if (!amp_read(address, reg, &old)) {
		return false;
	}
	return ((old & ~mask) | (value & mask)) == old || amp_write(address, reg, (old & ~mask) | (value & mask));
}

static uint32_t amps_ready;     /* bit per amplifier set up */

/* both set up (true if either is); powered down */
bool hakios_dada_amp_setup(void);

bool
hakios_dada_amp_setup(void)
{
	/* the top amplifier's speaker, not the earpiece (Android's pin state "rcv_gpio_low") */
	hakios_dada_gpio_out(67, false);
	for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
		uint8_t a = amps[i].address;
		bool ok = amp_write(a, CS35_TST_DAC_MSM_CONFIG, 0x11330000u) && amp_write(a, CS35_BST_RSVD_1, 0x50000802u) &&
		    amp_write(a, CS35_IRQ1_MASK_1, 0xffffffffu) && amp_write(a, CS35_IRQ1_MASK_2, 0xffffffffu) &&
		    amp_write(a, CS35_IRQ1_MASK_3, 0xffffffffu) &&
		    amp_update(a, CS35_ALIVE_DCIN_WD, 0x7f, 1u << 1 | 1u) &&
		    amp_update(a, CS35_BST_IPK_CTL, 0x7f, (4000u - 1600u) / 50u + 0x10u) &&
		    amp_update(a, CS35_ASP_CONTROL3, 0x3, 0x3) &&
		    amp_update(a, CS35_GPIO_PAD_CONTROL, 0x07000000u, 4u << 24) &&
		    amp_update(a, CS35_VPBR_CONFIG, 0x00ffff1fu, 5u << 12 | 3u << 8 | 8u) &&
		    amp_update(a, CS35_BLOCK_ENABLES2, 0x1000, 0x1000) &&
		    /* the serial port: I2S, clocks from the SoC, 16-bit slots and words, 48 kHz */
		    amp_update(a, CS35_ASP_CONTROL2, 0xff000000u | 0x700u | 0x40u | 0x10u | 0x4u | 0x1u, 16u << 24 | 2u << 8) &&
		    amp_update(a, CS35_ASP_DATA_CONTROL5, 0x3f, 16) &&
		    amp_update(a, CS35_GLOBAL_SAMPLE_RATE, 0x1f, 0x03) &&
		    /* the PLL locked to the 1.536 MHz bit clock */
		    amp_update(a, CS35_REFCLK_INPUT, 0x800, 0x800) && amp_update(a, CS35_REFCLK_INPUT, 0x7e0, 0x1bu << 5) &&
		    amp_update(a, CS35_REFCLK_INPUT, 0x10, 0) && amp_update(a, CS35_REFCLK_INPUT, 0x7, 0) &&
		    amp_update(a, CS35_REFCLK_INPUT, 0x800, 0) && amp_update(a, CS35_REFCLK_INPUT, 0x10, 0x10) &&
		    amp_write(a, CS35_FS_MON_0, 71u | 115u << 12) &&
		    /* the amplifier plays ASP RX1, its slot; gain; 4 ms soft ramp */
		    amp_update(a, CS35_DACPCM1_INPUT, 0x3f, 0x08) && amp_update(a, CS35_ASP_FRAME_CONTROL5, 0x3f, i) &&
		    amp_update(a, CS35_AMP_GAIN, 0x3e0, CS35_GAIN_DB << 5) && amp_update(a, CS35_AMP_CTRL, 0x7, 4);

		if (ok) {
			amps_ready |= 1u << i;
		}
		say("the %s speaker's amplifier %s\n", amps[i].name, ok ? "set up" : "could not be set up");
	}
	return amps_ready != 0;
}

void hakios_dada_amp_power(bool on);

void
hakios_dada_amp_power(bool on)
{
	for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
		uint8_t a = amps[i].address;

		if (!(amps_ready & (1u << i))) {
			continue;
		}
		if (on) {
			(void)(amp_update(a, CS35_ASP_ENABLES1, 1u << 16, 1u << 16) && amp_write(a, CS35_TEST_KEY, 0x55) &&
			    amp_write(a, CS35_TEST_KEY, 0xaa) && amp_write(a, 0x2084, 0x000f1aa0u) && amp_write(a, CS35_TEST_KEY, 0xcc) &&
			    amp_write(a, CS35_TEST_KEY, 0x33) && amp_write(a, CS35_GLOBAL_ENABLES, 1) &&
			    amp_update(a, CS35_BLOCK_ENABLES, 1, 1));
		} else {
			(void)(amp_update(a, CS35_BLOCK_ENABLES, 1, 0) && amp_write(a, CS35_TEST_KEY, 0x55) &&
			    amp_write(a, CS35_TEST_KEY, 0xaa) && amp_write(a, 0x2084, 0x000f1aa3u) && amp_write(a, CS35_TEST_KEY, 0xcc) &&
			    amp_write(a, CS35_TEST_KEY, 0x33));
			delay_for_interval(2, NSEC_PER_MSEC);
			(void)(amp_write(a, CS35_GLOBAL_ENABLES, 0) && amp_update(a, CS35_ASP_ENABLES1, 1u << 16, 0));
		}
	}
}

/* the interrupt status of each (EINT1, EINT2), acknowledged; bit 0 of EINT2: PLL locked */
void hakios_dada_amp_status(const char *when);

void
hakios_dada_amp_status(const char *when)
{
	for (unsigned i = 0; i < sizeof(amps) / sizeof(amps[0]); i++) {
		uint8_t a = amps[i].address;
		uint32_t e1 = 0, e2 = 0;

		if (!(amps_ready & (1u << i))) {
			continue;
		}
		(void)(amp_read(a, CS35_IRQ1_EINT_1, &e1) && amp_read(a, CS35_IRQ1_EINT_2, &e2));
		(void)amp_write(a, CS35_IRQ1_EINT_1, 0xffffffffu);
		(void)amp_write(a, CS35_IRQ1_EINT_2, 0xffffffffu);
		say("%s: the %s speaker's amplifier: EINT1 %#x%s, EINT2 %#x (PLL %s)\n", when, amps[i].name, e1,
		    (e1 & 0x1c400000u) ? " (errors)" : "", e2, (e2 & 1) ? "locked" : (e2 & 8) ? "UNLOCKED" : "-");
	}
}
