/*
 * The Xiaomi 15's (dada) fuel gauge: a TI bq27z561 at address 0x55 on the I2C bus of QUP SE5
 * (Android's device tree: qupv3_se5_i2c i2c@a94000, bq27z561_standlone@55; SDA gpio52, SCL gpio53).
 *
 * The audio DSP's charge manager knows the battery's voltage, current and temperature and whether it
 * charges (redwood_adsp.c, Xiaomi's MCA), but not how full it is: that is the gauge's, and Android reads
 * it from the application processor over I2C (bq27z561.ko). Here the two pins are driven by hand, as the
 * touch screen's SPI is (hakios_dada_touch.c): open drain made of the output enable -- a line is pulled
 * low by enabling the pin's output at 0 and let go by disabling it, when the bus's pull-up raises it --
 * at about 100 kHz. Only reads, of the gauge's standard commands (TRM SLUUC80): nothing is written to it,
 * so it is left as Android left it.
 *
 * Boot-arg dadagauge=1. hakios_dada_gauge_read() is how the battery's report asks (redwood_adsp.c);
 * the first answers are said in the log.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define say(...) redwood_tl("dgauge: " __VA_ARGS__)
extern void redwood_tl(const char *format, ...) __printflike(1, 2);

#define TLMM_PHYS               0x0f100000UL
#define TLMM_SIZE               0x102000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_UP                 3
#define PIN_SDA                 52
#define PIN_SCL                 53
#define GAUGE_ADDRESS           0x55
#define HALF_US                 10
/* the TLMM's own pull-up for I2C (the device tree's "qcom,i2c_pull"; Linux msm_pingroup.i2c_pull_bit 13):
 * stronger than the ordinary one, and on a bus with no resistors of its own the only thing that raises
 * the lines fast enough -- the amplifiers' bus, gpio64/65, answered nothing without it */
#define I2C_PULL                (1u << 13)

/* the gauge's standard commands (two bytes each, little-endian) */
#define CMD_TEMPERATURE         0x06    /* 0.1 K */
#define CMD_VOLTAGE             0x08    /* mV */
#define CMD_CURRENT             0x0c    /* mA, signed: less than 0 while the battery gives */
#define CMD_REMAINING           0x10    /* mAh */
#define CMD_FULL                0x12    /* mAh, the full charge as learnt */
#define CMD_CYCLES              0x2a
#define CMD_SOC                 0x2c    /* %, relative state of charge */
#define CMD_HEALTH              0x2e    /* %, state of health */
#define CMD_DESIGN              0x3c    /* mAh */

static vm_offset_t tlmm;
static bool ready;

/* an I2C bus of two pins, driven by hand (the gauge's, and the speakers' amplifiers', hakios_dada_amp.c) */
struct dada_i2c {
	unsigned sda, scl;
};
static const struct dada_i2c gauge_bus = { PIN_SDA, PIN_SCL };
static const struct dada_i2c *bus = &gauge_bus;
static LCK_GRP_DECLARE(gauge_group, "dada gauge");
static LCK_MTX_DECLARE(gauge_lock, &gauge_group);

static inline void
wr(vm_offset_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
	__builtin_arm_dsb(0xf);
}

static inline uint32_t
rd(vm_offset_t a)
{
	return *(volatile uint32_t *)a;
}

static void
half(void)
{
	delay_for_interval(HALF_US, NSEC_PER_USEC);
}

/* a line let go (input, pulled up by the bus) or pulled low (output at 0) */
static void
line(unsigned pin, bool high)
{
	/* (the output's level made 0 every time, before its enable: a pin whose output a driver before us
	 * left at 1 would drive the line high when we mean to pull it low -- the amplifiers' bus, gpio64/65,
	 * answered nothing at all until this) */
	if (!high) {
		wr(GPIO_IO(pin), 0);
	}
	wr(GPIO_CFG(pin), PULL_UP | 0 << 2 | (2 / 2 - 1) << 6 | (high ? 0 : 1u << 9) | I2C_PULL);
}

static bool
level(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

/* SCL let go, and held while the gauge stretches the clock (a millisecond at most) */
static bool
scl_high(void)
{
	line(bus->scl, true);
	for (unsigned i = 0; i < 200 && !level(bus->scl); i++) {
		half();
	}
	return level(bus->scl);
}

static void
start(void)
{
	line(bus->sda, true);
	scl_high();
	half();
	line(bus->sda, false);
	half();
	line(bus->scl, false);
	half();
}

static void
stop(void)
{
	line(bus->sda, false);
	half();
	scl_high();
	half();
	line(bus->sda, true);
	half();
}

/* a byte out; true if the gauge acknowledged it */
static bool
put_byte(uint8_t b)
{
	bool ack;

	for (int i = 7; i >= 0; i--) {
		line(bus->sda, (b >> i) & 1);
		half();
		scl_high();
		half();
		line(bus->scl, false);
	}
	line(bus->sda, true);
	half();
	scl_high();
	ack = !level(bus->sda);
	half();
	line(bus->scl, false);
	return ack;
}

static uint8_t
get_byte(bool ack)
{
	uint8_t b = 0;

	line(bus->sda, true);
	for (int i = 7; i >= 0; i--) {
		half();
		scl_high();
		b |= (uint8_t)(level(bus->sda) << i);
		half();
		line(bus->scl, false);
	}
	line(bus->sda, !ack);
	half();
	scl_high();
	half();
	line(bus->scl, false);
	line(bus->sda, true);
	return b;
}

/* one of the gauge's two-byte commands read */
static bool
read_word(uint8_t command, uint16_t *value)
{
	bool ok;
	uint8_t lo, hi;

	start();
	ok = put_byte(GAUGE_ADDRESS << 1) && put_byte(command);
	if (ok) {
		line(bus->sda, true);           /* the repeated start */
		half();
		scl_high();
		half();
		line(bus->sda, false);
		half();
		line(bus->scl, false);
		half();
		ok = put_byte(GAUGE_ADDRESS << 1 | 1);
	}
	if (ok) {
		lo = get_byte(true);
		hi = get_byte(false);
		*value = (uint16_t)(lo | hi << 8);
	}
	stop();
	return ok;
}

/* what the battery's report wants of the gauge; false if it does not answer */
bool hakios_dada_gauge_read(uint32_t *soc, uint32_t *remaining_mah, uint32_t *full_mah, uint32_t *design_mah,
    uint32_t *cycles, int32_t *current_ma);

bool
hakios_dada_gauge_read(uint32_t *soc, uint32_t *remaining_mah, uint32_t *full_mah, uint32_t *design_mah,
    uint32_t *cycles, int32_t *current_ma)
{
	uint16_t s = 0, r = 0, f = 0, d = 0, c = 0, i = 0;
	bool ok;

	if (!ready) {
		return false;
	}
	lck_mtx_lock(&gauge_lock);
	ok = read_word(CMD_SOC, &s) && read_word(CMD_REMAINING, &r) && read_word(CMD_FULL, &f) &&
	    read_word(CMD_DESIGN, &d) && read_word(CMD_CYCLES, &c) && read_word(CMD_CURRENT, &i);
	lck_mtx_unlock(&gauge_lock);
	if (ok && s <= 100) {
		*soc = s;
		*remaining_mah = r;
		*full_mah = f;
		*design_mah = d;
		*cycles = c;
		*current_ma = (int16_t)i;
		return true;
	}
	return false;
}

void hakios_dada_gauge_init(void);

void
hakios_dada_gauge_init(void)
{
	uint32_t on = 0;
	uint16_t v[9] = { 0 };
	static const uint8_t commands[9] = { CMD_SOC, CMD_VOLTAGE, CMD_CURRENT, CMD_TEMPERATURE, CMD_REMAINING, CMD_FULL,
		                             CMD_DESIGN, CMD_CYCLES, CMD_HEALTH };
	bool ok = true;

	if (!PE_parse_boot_argn("dadagauge", &on, sizeof(on)) || on == 0) {
		return;
	}
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	line(bus->scl, true);
	line(bus->sda, true);
	half();
	/* a bus left in the middle of a byte: nine clocks to let a gauge that holds SDA low finish it */
	for (unsigned n = 0; n < 9 && !level(bus->sda); n++) {
		line(bus->scl, false);
		half();
		scl_high();
		half();
	}
	stop();
	for (unsigned n = 0; n < 9 && ok; n++) {
		ok = read_word(commands[n], &v[n]);
	}
	if (!ok) {
		say("the bq27z561 (0x55 on gpio52/53) does not answer: the battery's percentage stays an estimate\n");
		return;
	}
	ready = true;
	say("the bq27z561 answers: %u%%, %u mV, %d mA, %d.%u C, %u of %u mAh (designed %u), %u cycles, health %u%%\n",
	    v[0], v[1], (int16_t)v[2], ((int)v[3] - 2731) / 10, (unsigned)(((int)v[3] - 2731) % 10 + 10) % 10, v[4], v[5],
	    v[6], v[7], v[8]);
}

/*
 * Any transfer on any bus of two pins (the amplifiers' and whoever comes next): `wlen` bytes written,
 * then, after a repeated start, `rlen` read. One transfer at a time on every bus, the gauge's included.
 */
bool hakios_dada_i2c_xfer(unsigned sda, unsigned scl, uint8_t address, const uint8_t *w, unsigned wlen, uint8_t *r,
    unsigned rlen);

bool
hakios_dada_i2c_xfer(unsigned sda, unsigned scl, uint8_t address, const uint8_t *w, unsigned wlen, uint8_t *r,
    unsigned rlen)
{
	struct dada_i2c other = { sda, scl };
	bool ok = true;

	lck_mtx_lock(&gauge_lock);
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	bus = &other;
	start();
	if (wlen != 0 || rlen == 0) {
		ok = put_byte((uint8_t)(address << 1));
		for (unsigned i = 0; ok && i < wlen; i++) {
			ok = put_byte(w[i]);
		}
		if (ok && rlen != 0) {
			line(bus->sda, true);  /* the repeated start */
			half();
			scl_high();
			half();
			line(bus->sda, false);
			half();
			line(bus->scl, false);
			half();
		}
	}
	if (ok && rlen != 0) {
		ok = put_byte((uint8_t)(address << 1 | 1));
		for (unsigned i = 0; ok && i < rlen; i++) {
			r[i] = get_byte(i + 1 < rlen);
		}
	}
	stop();
	bus = &gauge_bus;
	lck_mtx_unlock(&gauge_lock);
	return ok;
}

/* a pin of the TLMM made an output at a level (a reset line) */
void hakios_dada_gpio_out(unsigned pin, bool high);

void
hakios_dada_gpio_out(unsigned pin, bool high)
{
	lck_mtx_lock(&gauge_lock);
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	wr(GPIO_IO(pin), high ? 2 : 0);
	/*
	 * Bit 12: the pin is the application processor's, not the eGPIO's. Some of SM8750's pins (bit 11 says
	 * which) can be handed to another subsystem -- the audio DSP's island owns gpio107 and gpio108 out of
	 * the boot -- and until this bit is set nothing the TLMM is told reaches the pad (Linux: msm_pingroup
	 * egpio_enable, set for every function but the eGPIO one). The amplifiers stayed in reset without it.
	 */
	wr(GPIO_CFG(pin), 0 | 0 << 2 | (2 / 2 - 1) << 6 | 1u << 9 | ((rd(GPIO_CFG(pin)) & (1u << 11)) ? 1u << 12 : 0));
	lck_mtx_unlock(&gauge_lock);
}

/* the two lines' levels, let go: bit 1 SDA, bit 0 SCL (both high on a bus with its pull-ups) */
unsigned hakios_dada_i2c_levels(unsigned sda, unsigned scl);

unsigned
hakios_dada_i2c_levels(unsigned sda, unsigned scl)
{
	unsigned v;

	lck_mtx_lock(&gauge_lock);
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	line(sda, true);
	line(scl, true);
	half();
	v = (level(sda) ? 2u : 0u) | (level(scl) ? 1u : 0u);
	/* and pulled low, each in turn: bit 3 SDA still high, bit 2 SCL still high (a pin we do not drive);
	 * bits 8.. the two pins' configuration words */
	line(sda, false);
	half();
	v |= level(sda) ? 8u : 0u;
	line(sda, true);
	line(scl, false);
	half();
	v |= level(scl) ? 4u : 0u;
	line(scl, true);
	v |= (rd(GPIO_CFG(sda)) & 0x3ff) << 8 | (rd(GPIO_IO(sda)) & 3) << 20;
	lck_mtx_unlock(&gauge_lock);
	return v;
}

/* a pin's configuration word and its level, for the log: configuration << 4 | io */
unsigned hakios_dada_gpio_state(unsigned pin);

unsigned
hakios_dada_gpio_state(unsigned pin)
{
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	return (rd(GPIO_CFG(pin)) & 0xffff) << 4 | (rd(GPIO_IO(pin)) & 3);
}
