/*
 * The touch screen of the Xiaomi 15 (dada, SM8750): a Synaptics TouchComm ("TCM") controller on
 * the SPI bus of serial engine 2 of the second QUP wrapper, as a source of touches for
 * /dev/redwood-touch (sys/redwood_input.h). Only with boot-arg dadatouch=1: every address below
 * belongs to this phone (its own device tree, Android's).
 *
 * As redwood_goodix.c does for the POCO X5 Pro, the driver does everything itself:
 *   power    iovdd 1.8 V from LDO12 of PMIC B ("ldob12"), avdd 3.0 V from LDO16 ("ldob16"),
 *            through RPMh (the application processor's DRV 2, RSC version 3 register layout)
 *   clocks   the wrapper's AHB and core clocks and the engine's clock, by vote in GCC; the
 *            engine's rate comes from the wrapper's "QSPI reference" clock generator
 *   pins     gpio40..43 to the engine (MISO, MOSI, CLK, CS), gpio161 reset, gpio162 interrupt
 *            (low while the controller has a message)
 *   SPI      the engine ("GENI") in FIFO mode, polled
 *   chip     TouchComm: every read is a message -- 0xa5, a status or report code, a 16-bit length,
 *            the payload, 0x5a -- read as the 4-byte header and then the rest (which the chip sends
 *            again behind 0xa5 0x03, "continued read"); a command is the code, a 16-bit length and
 *            the payload. After reset the chip announces itself with an identify report; we ask
 *            for the application information (the coordinate range, the number of objects) and the
 *            touch report configuration, a list of fields and their widths in bits, by which every
 *            touch report (0x11) is then taken apart.
 * No interrupt is used: a thread looks at the interrupt line every 4 ms (touch_poll_us=N).
 * Lines go to the loader's log (redwood_tl): what was found, the first reports, a count a minute.
 *
 * Layouts: Linux drivers/spi/spi-geni-qcom.c, drivers/soc/qcom/{qcom-geni-se,rpmh-rsc,cmd-db}.c,
 * drivers/clk/qcom/gcc-sm8750.c, drivers/pinctrl/qcom/pinctrl-sm8750.c, and Synaptics' TCM driver
 * (synaptics_tcm_{core,spi,touch}.c of the vendor kernels).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern void redwood_tl(const char *format, ...) __printflike(1, 2);

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);

#define say(...) redwood_tl("dtouch: " __VA_ARGS__)

static vm_offset_t tlmm, gcc, se, rsc, cmd_db;
static uint32_t ultra_touch;
static unsigned pin_miso = 40, pin_mosi = 41, pin_clk = 42, pin_cs = 43;

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
    /* Keep even zero in a general register for the trapped-MMIO comparison.
     * The vendor documents this requirement for SMMU; applicability to GENI
     * and GPI is experimental and must be established on the device. */
    __asm__ volatile("str %w0, [%1]" : : "r"(value), "r"(address) : "memory");
}

/* A trapped Qualcomm SMMU store must carry a real register, including zero. */
static inline void
smmu_write32(vm_offset_t address, uint32_t value)
{
	__asm__ volatile("str %w0, [%1]" : : "r"(value), "r"(address) : "memory");
}

/* Explicit PoC maintenance for an opt-in reserved-DMA visibility comparison. */
static void
xuanyuan_dma_publish(vm_offset_t address, unsigned length)
{
 uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
 unsigned line=4u<<((ctr>>16)&15);
 __builtin_arm_dsb(0xf);
 for(vm_offset_t at=address&~(vm_offset_t)(line-1);at<address+length;at+=line)
  __asm__ volatile("dc civac, %0"::"r"(at):"memory");
 __builtin_arm_dsb(0xf);
}

static inline void
smmu_write64(vm_offset_t address, uint64_t value)
{
	__asm__ volatile("str %0, [%1]" : : "r"(value), "r"(address) : "memory");
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- pins (TLMM) ------------------------------------------------------------------------------- */

#define TLMM_PHYS               0x0f100000UL    /* GPIO 0 (Android's "sun-tlmm" node starts 1 MB lower) */
#define TLMM_SIZE               0x102000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define PULL_NONE               0
#define PULL_DOWN               1
#define PULL_UP                 3

#define PIN_MISO                pin_miso
#define PIN_MOSI                pin_mosi
#define PIN_CLK                pin_clk
#define PIN_CS                pin_cs
#define PIN_RESET               161     /* low = in reset */
#define PIN_INT                 162     /* low = the chip has a message */
#define PIN_FUNCTION_SPI        1       /* "qup1_se2" */

static void
gpio_configure(unsigned pin, unsigned function, unsigned pull, unsigned ma, bool output)
{
    uint32_t fields=pull | function << 2 | (ma / 2 - 1) << 6 | (output ? 1u << 9 : 0);
    if(ultra_touch){
     /* MiCode pinctrl-msm: an eGPIO-capable pad must be owned by APPS
      * before selecting GPIO or QUP. Preserve unrelated upper fields. */
     uint32_t previous=rd(GPIO_CFG(pin));
     fields|=previous&~0x3ffu;
     if(previous&(1u<<11))fields|=1u<<12;
    }
    wr(GPIO_CFG(pin),fields);
}

static void
gpio_set(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), high ? 2 : 0);
}

static bool
gpio_get(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
gpio_output(unsigned pin, bool high, unsigned pull, unsigned ma)
{
	gpio_set(pin, high);
	gpio_configure(pin, 0, pull, ma, true);
}

/* ---- the supplies (RPMh) ------------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x81c60000UL
#define CMD_DB_SIZE             0x20000UL
#define RSC_PHYS                0x16520000UL    /* "drv-2", the application processor's */
#define RSC_SIZE                0x10000UL
#define TCS_BASE                0xd00           /* command slot set 0: the first "active" one (tcs-config) */
#define MSGID_WRITE_4_BYTES     0x10008
#define MSGID_RESPONSE          0x100
#define VRM_VOLTAGE             0
#define VRM_ENABLE              4
#define VRM_MODE                8
#define VRM_MODE_LDO_HPM        7

/* register offsets inside a slot set: RSC version 2 and version 3 (SM8550 on) differ */
static struct {
	uint32_t wait, control, status, cmd_enable, msgid, addr, data, cmd_size;
} tcs_reg = { 0x10, 0x14, 0x18, 0x1c, 0x30, 0x34, 0x38, 20 };
#define TCS_IRQ_ENABLE          0x00
#define TCS_IRQ_STATUS          0x04
#define TCS_IRQ_CLEAR           0x08
#define TCS_CONTROL_ENABLE      (1u << 16)
#define TCS_CONTROL_TRIGGER     (1u << 24)
#define TCS(reg)                (rsc + TCS_BASE + (reg))

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static uint32_t
rpmh_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db;
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		say("no command database (magic %02x %02x %02x %02x)\n", db->magic[0], db->magic[1], db->magic[2],
		    db->magic[3]);
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
rpmh_wait(uint32_t reg, uint32_t mask, uint32_t value, const char *what)
{
	for (int i = 0; i < 2000; i++) {
		if ((rd(TCS(reg)) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	say("RPMh: timeout waiting for %s (register %#x = %#x)\n", what, reg, rd(TCS(reg)));
	return false;
}

static bool
rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = rd(TCS(TCS_IRQ_ENABLE));
	bool done;

	for (int i = 0; rd(TCS(tcs_reg.status)) == 0; i++) {
		if (i > 2000) {
			say("RPMh: the command slots stay busy\n");
			return false;
		}
		sleep_us(100);
	}
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	for (unsigned i = 0; i < count; i++) {
		wr(TCS(tcs_reg.msgid + tcs_reg.cmd_size * i), MSGID_WRITE_4_BYTES | MSGID_RESPONSE);
		wr(TCS(tcs_reg.addr + tcs_reg.cmd_size * i), address[i]);
		wr(TCS(tcs_reg.data + tcs_reg.cmd_size * i), data[i]);
	}
	wr(TCS(tcs_reg.wait), (1u << count) - 1);
	wr(TCS(tcs_reg.cmd_enable), (1u << count) - 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable | 1);
	wr(TCS(tcs_reg.control), TCS_CONTROL_ENABLE);
	wr(TCS(tcs_reg.control), TCS_CONTROL_ENABLE | TCS_CONTROL_TRIGGER);

	done = rpmh_wait(TCS_IRQ_STATUS, 1, 1, "the request");
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_TRIGGER);
	wr(TCS(tcs_reg.control), rd(TCS(tcs_reg.control)) & ~TCS_CONTROL_ENABLE);
	wr(TCS(tcs_reg.cmd_enable), 0);
	wr(TCS(tcs_reg.wait), 0);
	wr(TCS(TCS_IRQ_CLEAR), 1);
	wr(TCS(TCS_IRQ_ENABLE), irq_enable);
	return done;
}

static bool
ldo_on(const char *name, uint32_t millivolts)
{
	uint32_t base = rpmh_address(name);
	uint32_t address[3] = { base + VRM_VOLTAGE, base + VRM_ENABLE, base + VRM_MODE };
	uint32_t data[3] = { millivolts, 1, VRM_MODE_LDO_HPM };

	if (base == 0 || (base >> 16 & 0xf) != 4) {
		say("RPMh: no regulator \"%s\" (address %#x)\n", name, base);
		return false;
	}
	if (!rpmh_write(address, data, 3)) {
		return false;
	}
	say("%s on at %u mV (RPMh address %#x)\n", name, millivolts, base);
	return true;
}

static void
supplies_on(void)
{
	uint32_t id = rd(rsc);
	uint32_t major = id >> 16 & 0xff;

	if (major >= 3) {
		tcs_reg.wait = 0x20;
		tcs_reg.control = 0x24;
		tcs_reg.status = 0x28;
		tcs_reg.cmd_enable = 0x2c;
		tcs_reg.msgid = 0x34;
		tcs_reg.addr = 0x38;
		tcs_reg.data = 0x3c;
		tcs_reg.cmd_size = 24;
	}
	say("RPMh: RSC id %#x (version %u); slot status %#x control %#x\n", id, major, rd(TCS(tcs_reg.status)),
	    rd(TCS(tcs_reg.control)));
	if (ultra_touch) {
		ldo_on("ldob14", 3200); /* xuanyuan captured DT avdd */
		sleep_us(3000); /* vendor syna_spi_enable_regulator order */
		ldo_on("ldob12", 1800);
	} else {
		ldo_on("ldob12", 1800);
		ldo_on("ldob16", 3000);
	}
}

/* ---- clocks (GCC) -------------------------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x60000UL
#define GCC_VOTE                (gcc + 0x52008)
#define VOTE_QUP1_CORE_2X       (1u << 18)
#define VOTE_QUP1_CORE          (1u << 19)
#define VOTE_QUP1_M_AHB         (1u << 20)
#define VOTE_QUP1_S_AHB         (1u << 21)
#define VOTE_QUP1_S2            (1u << 24)
#define GCC_QUP1_M_AHB_CBCR     (gcc + 0x23140)
#define GCC_QUP1_S_AHB_CBCR     (gcc + 0x23144)
#define GCC_QUP1_S2_CBCR        (gcc + 0x1827c)
#define GCC_QUP1_S2_DIV         (gcc + 0x1828c)         /* [3:0] divider - 1, behind the generator */
#define GCC_QSPI_REF_CMD        (gcc + 0x188c0)
#define GCC_QSPI_REF_CFG        (gcc + 0x188c4)
#define GCC_QSPI_REF_DFSR       (gcc + 0x188d4)
#define GCC_QSPI_REF_PERF_CFG(l) (gcc + 0x188dc + 4 * (l))
#define GCC_QSPI_REF_PERF_M(l)  (gcc + 0x1891c + 4 * (l))
#define GCC_QSPI_REF_PERF_N(l)  (gcc + 0x1895c + 4 * (l))
#define CLOCK_OFF               (1u << 31)
#define XO_HZ                   19200000

static uint32_t se_source_hz, se_clock_select;

static uint32_t
rcg_rate(uint32_t cfg, uint32_t m, uint32_t not_n_minus_m)
{
	/* parent map 5: 0 crystal, 1 GPLL0 (600 MHz), 2 GPLL7 (not known here), 6 GPLL0 even (300 MHz) */
	static const uint32_t parent[8] = { XO_HZ, 600000000, 0, 0, 0, 0, 300000000, 0 };
	uint64_t hz = parent[cfg >> 8 & 7];
	uint32_t divider = cfg & 0x1f, mode = cfg >> 12 & 3;

	if (divider) {
		hz = hz * 2 / (divider + 1);
	}
	if (mode) {
		uint32_t n = (~not_n_minus_m & 0xffff) + (m & 0xffff);

		if (n) {
			hz = hz * (m & 0xffff) / n;
		}
	}
	return (uint32_t)hz;
}

static bool
clocks_on(void)
{
	uint32_t post = (rd(GCC_QUP1_S2_DIV) & 0xf) + 1;

	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_CORE_2X | VOTE_QUP1_CORE | VOTE_QUP1_M_AHB | VOTE_QUP1_S_AHB);
	if (rd(GCC_QSPI_REF_DFSR) & 1) {
		for (uint32_t level = 0; level < 8; level++) {
			uint32_t hz = rcg_rate(rd(GCC_QSPI_REF_PERF_CFG(level)), rd(GCC_QSPI_REF_PERF_M(level)),
			    rd(GCC_QSPI_REF_PERF_N(level))) / post;

			if (hz >= 4000000 && (se_source_hz == 0 || hz < se_source_hz)) {
				se_source_hz = hz;
				se_clock_select = level;
			}
		}
		if (se_source_hz == 0) {
			say("no usable preset clock rate\n");
			return false;
		}
	} else {
		wr(GCC_QSPI_REF_CFG, (rd(GCC_QSPI_REF_CFG) & ~0x371fu) | 1);
		wr(GCC_QSPI_REF_CMD, rd(GCC_QSPI_REF_CMD) | 1);
		for (int i = 0; i < 500 && (rd(GCC_QSPI_REF_CMD) & 1); i++) {
			sleep_us(10);
		}
		if (rd(GCC_QSPI_REF_CMD) & 1) {
			say("the engine's clock does not take its configuration (%#x)\n", rd(GCC_QSPI_REF_CMD));
			return false;
		}
		se_source_hz = XO_HZ / post;
		se_clock_select = 0;
	}
	wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_S2);
	for (int i = 0; i < 1000; i++) {
		if (!((rd(GCC_QUP1_M_AHB_CBCR) | rd(GCC_QUP1_S_AHB_CBCR) | rd(GCC_QUP1_S2_CBCR)) & CLOCK_OFF)) {
			return true;
		}
		sleep_us(10);
	}
	say("clocks stay off: %#x %#x %#x\n", rd(GCC_QUP1_M_AHB_CBCR), rd(GCC_QUP1_S_AHB_CBCR), rd(GCC_QUP1_S2_CBCR));
	return false;
}

/* ---- SPI through the serial engine (GENI) ---------------------------------------------------------- */

#define SE_PHYS                 0x00a88000UL
#define SE_SIZE                 0x4000UL
#define GENI_FORCE_DEFAULT      (se + 0x020)
#define GENI_OUTPUT_CTRL        (se + 0x024)
#define GENI_CGC_CTRL           (se + 0x028)
#define GENI_STATUS             (se + 0x040)
#define GENI_SER_M_CLK_CFG      (se + 0x048)
#define GENI_IF_DISABLE_RO      (se + 0x064)
#define GENI_FW_REVISION_RO     (se + 0x068)
#define GENI_CLK_SEL            (se + 0x07c)
#define SPI_CPHA                (se + 0x224)
#define SPI_LOOPBACK            (se + 0x22c)
#define SPI_CPOL                (se + 0x230)
#define SPI_DEMUX_OUTPUT_INV    (se + 0x24c)
#define SPI_DEMUX_SEL           (se + 0x250)
#define GENI_BYTE_GRAN          (se + 0x254)
#define GENI_DMA_MODE_EN        (se + 0x258)
#define SPI_TRANS_CFG           (se + 0x25c)
#define GENI_TX_PACKING_CFG0    (se + 0x260)
#define GENI_TX_PACKING_CFG1    (se + 0x264)
#define SPI_WORD_LEN            (se + 0x268)
#define SPI_TX_TRANS_LEN        (se + 0x26c)
#define SPI_RX_TRANS_LEN        (se + 0x270)
#define SPI_DELAY_COUNTERS      (se + 0x278)
#define GENI_RX_PACKING_CFG0    (se + 0x284)
#define GENI_RX_PACKING_CFG1    (se + 0x288)
#define GENI_M_CMD0             (se + 0x600)
#define GENI_M_CMD_CTRL         (se + 0x604)
#define GENI_M_IRQ_STATUS       (se + 0x610)
#define GENI_M_IRQ_EN           (se + 0x614)
#define GENI_M_IRQ_CLEAR        (se + 0x618)
#define GENI_S_IRQ_CLEAR        (se + 0x648)
#define GENI_TX_FIFO            (se + 0x700)
#define GENI_RX_FIFO            (se + 0x780)
#define GENI_TX_FIFO_STATUS     (se + 0x800)
#define GENI_RX_FIFO_STATUS     (se + 0x804)
#define GENI_TX_WATERMARK       (se + 0x80c)
#define GENI_RX_WATERMARK       (se + 0x810)
#define GENI_RX_RFR_WATERMARK   (se + 0x814)
#define SE_DMA_TX_IRQ_CLR       (se + 0xc44)
#define SE_DMA_RX_IRQ_CLR       (se + 0xd44)
#define SE_GSI_EVENT_EN         (se + 0xe18)
#define SE_IRQ_EN               (se + 0xe1c)
#define SE_HW_PARAM_0           (se + 0xe24)
#define SE_DMA_GENERAL_CFG      (se + 0xe30)

#define M_CMD_DONE              (1u << 0)
#define M_CMD_CANCEL            (1u << 4)
#define M_CMD_ABORT             (1u << 5)
#define M_ERRORS                ((1u << 1) | (1u << 2) | (1u << 3) | (1u << 24) | (1u << 25) | (1u << 28) | (1u << 29))
#define SPI_FULL_DUPLEX         (3u << 27)

static uint32_t fifo_depth;

static const char *
geni_spi_init(uint32_t hz)
{
	uint32_t divider;

	if ((rd(GENI_FW_REVISION_RO) >> 8 & 0xff) != 1) {
		return "the serial engine has no SPI firmware";
	}
	if (rd(GENI_IF_DISABLE_RO) & 1) {
		return "the serial engine's FIFO interface is disabled";
	}
	fifo_depth = rd(SE_HW_PARAM_0) >> 16 & 0x3f;
	if (fifo_depth < 4) {
		return "the serial engine reports no FIFO";
	}
	wr(SE_GSI_EVENT_EN, 0);
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(GENI_S_IRQ_CLEAR, 0xffffffff);
	wr(SE_DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE_DMA_RX_IRQ_CLR, 0xffffffff);
	wr(GENI_CGC_CTRL, rd(GENI_CGC_CTRL) | 0x7f);
	wr(SE_DMA_GENERAL_CFG, rd(SE_DMA_GENERAL_CFG) | 0xf);
	wr(GENI_OUTPUT_CTRL, 0x7f);
	wr(GENI_FORCE_DEFAULT, 1);
	wr(GENI_DMA_MODE_EN, rd(GENI_DMA_MODE_EN) & ~1u);
	wr(GENI_RX_WATERMARK, fifo_depth - 3);
	wr(GENI_RX_RFR_WATERMARK, fifo_depth - 2);
	wr(GENI_TX_WATERMARK, 0);
	wr(SE_IRQ_EN, rd(SE_IRQ_EN) | 0xf);
	wr(GENI_M_IRQ_EN, rd(GENI_M_IRQ_EN) | 0x7fc0007f);

	/* mode 0 (synaptics,spi-mode 0), chip select 0, 8 bits, most significant first */
	wr(SPI_LOOPBACK, 0);
	wr(SPI_DEMUX_SEL, 0);
	wr(SPI_CPHA, 0);
	wr(SPI_CPOL, 0);
	wr(SPI_DEMUX_OUTPUT_INV, 0);
	wr(SPI_TRANS_CFG, rd(SPI_TRANS_CFG) & ~2u);
	wr(SPI_DELAY_COUNTERS, 0);
	wr(GENI_TX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_TX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_RX_PACKING_CFG0, 0x0007f8fe);
	wr(GENI_RX_PACKING_CFG1, 0x000ffefe);
	wr(GENI_BYTE_GRAN, 0);
	wr(SPI_WORD_LEN, 8 - 4);

	divider = (se_source_hz + hz - 1) / hz;
	wr(GENI_CLK_SEL, se_clock_select & 7);
	wr(GENI_SER_M_CLK_CFG, (divider ? divider : 1) << 4 | 1);
	return NULL;
}

static void
geni_drain(uint8_t *rx, uint32_t length, uint32_t *received)
{
	uint32_t status = rd(GENI_RX_FIFO_STATUS), words = status & 0x01ffffff, bytes = words * 4;

	if (status & 0x80000000u) {
		uint32_t valid = status >> 28 & 7;

		if (valid && valid < 4) {
			bytes -= 4 - valid;
		}
	}
	for (uint32_t i = 0; i < words; i++) {
		uint32_t word = rd(GENI_RX_FIFO);

		for (int j = 0; j < 4 && bytes; j++, bytes--) {
			if (*received < length) {
				if (rx) {
					rx[*received] = (uint8_t)(word >> (8 * j));
				}
				(*received)++;
			}
		}
	}
}

static void
geni_recover(void)
{
	wr(GENI_M_CMD_CTRL, 1u << 2);
	for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL); i++) {
		sleep_us(100);
	}
	if (!(rd(GENI_M_IRQ_STATUS) & M_CMD_CANCEL)) {
		wr(GENI_M_CMD_CTRL, 1u << 1);
		for (int i = 0; i < 2000 && !(rd(GENI_M_IRQ_STATUS) & M_CMD_ABORT); i++) {
			sleep_us(100);
		}
	}
	for (int i = 0; i < 64 && (rd(GENI_RX_FIFO_STATUS) & 0x01ffffff); i++) {
		(void)rd(GENI_RX_FIFO);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
}

/* One transfer with the chip selected throughout: `length` bytes out (0xff without tx) and as many in. */
static bool
geni_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	static unsigned complaints;
	uint32_t sent = 0, received = 0, status = 0;
	uint64_t deadline;

	for (int i = 0; rd(GENI_STATUS) & 1; i++) {
		if (i > 2000) {
			geni_recover();
			break;
		}
		sleep_us(100);
	}
	wr(GENI_M_IRQ_CLEAR, 0xffffffff);
	wr(SPI_TX_TRANS_LEN, length);
	wr(SPI_RX_TRANS_LEN, length);
	wr(GENI_M_CMD0, SPI_FULL_DUPLEX);
	clock_interval_to_deadline(200, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		while (sent < length && (rd(GENI_TX_FIFO_STATUS) & 0x0fffffff) + 1 < fifo_depth) {
			uint32_t word = 0;

			for (int j = 0; j < 4 && sent < length; j++, sent++) {
				word |= (uint32_t)(tx ? tx[sent] : 0xff) << (8 * j);
			}
			wr(GENI_TX_FIFO, word);
		}
		status = rd(GENI_M_IRQ_STATUS);
		geni_drain(rx, length, &received);
		if (status & M_ERRORS) {
			break;
		}
		if (status & M_CMD_DONE) {
			geni_drain(rx, length, &received);
			wr(GENI_M_IRQ_CLEAR, status);
			if (received == length) {
				return true;
			}
			break;
		}
	}
	if (complaints++ < 8) {
		say("SPI transfer of %u bytes failed: status %#x, sent %u, received %u\n", length, status, sent, received);
	}
	geni_recover();
	return false;
}

/*
 * ---- SPI by hand, on the same pins ----
 * dada's boot firmware hands serial engine 2 over in GSI (DMA) mode only: its FIFO interface is
 * disabled ("qcom,la-vm", a GPI channel in Android's device tree). Until the GPI DMA engine is
 * driven, the four pins are driven as GPIOs: mode 0, clock idle low, data read on the rising edge
 * and changed on the falling one, most significant bit first. Each pin write is a posted store to
 * the TLMM; reading the input pin orders them, which also paces the clock at a few MHz at most.
 */
static void
bitbang_init(void)
{
	gpio_output(PIN_CS, true, PULL_NONE, 6);
	gpio_output(PIN_CLK, false, PULL_NONE, 6);
	gpio_output(PIN_MOSI, true, PULL_NONE, 6);
	gpio_configure(PIN_MISO, 0, PULL_DOWN, 2, false);
}

static uint32_t bitbang_half_ns = 150;

static inline void
bitbang_wait(void)
{
	uint64_t until = mach_absolute_time() + bitbang_half_ns / 52;        /* 19.2 MHz ticks, about 52 ns each */

	while (mach_absolute_time() < until) {
	}
}

static void
bitbang_select(bool on)
{
	if (on) {
		gpio_set(PIN_CS, false);
		(void)gpio_get(PIN_CS);
		bitbang_wait();
	} else {
		(void)gpio_get(PIN_MISO);
		bitbang_wait();
		gpio_set(PIN_CS, true);
		(void)gpio_get(PIN_CS);
	}
}

/* bytes with the chip already selected (bitbang_select) */
static void
bitbang_bytes(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	for (uint32_t i = 0; i < length; i++) {
		uint8_t out = tx ? tx[i] : 0xff, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			gpio_set(PIN_MOSI, out >> bit & 1);
			(void)gpio_get(PIN_MISO);
			bitbang_wait();
			gpio_set(PIN_CLK, true);
			(void)gpio_get(PIN_CLK);
			bitbang_wait();
			in = (uint8_t)(in << 1 | gpio_get(PIN_MISO));
			gpio_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
}

static bool
bitbang_transfer(const uint8_t *tx, uint8_t *rx, uint32_t length)
{
	bitbang_select(true);
	bitbang_bytes(tx, rx, length);
	bitbang_select(false);
	return true;
}

static bool (*spi_transfer)(const uint8_t *tx, uint8_t *rx, uint32_t length) = geni_transfer;

/* ---- TouchComm -------------------------------------------------------------------------------------- */

#define TCM_MARKER              0xa5
#define TCM_PADDING             0x5a
#define CMD_IDENTIFY            0x02
#define CMD_RESET               0x04
#define CMD_ENABLE_REPORT       0x05
#define CMD_DISABLE_REPORT      0x06
#define CMD_GET_DYNAMIC_CONFIG  0x23
#define CMD_SET_DYNAMIC_CONFIG  0x24
#define CMD_EXIT_DEEP_SLEEP     0x2d
#define DC_ENABLE_WAKEUP_GESTURE_MODE 0x09
#define DC_TOUCH_SCAN_MODE      0xf2
#define REPORT_XIAOMI_FRAME     0xc0    /* 1708 bytes, unasked, at every frame: Xiaomi's ("frame data") */
#define CMD_RUN_APPLICATION     0x14
#define CMD_GET_APPLICATION_INFO 0x20
#define CMD_GET_TOUCH_REPORT_CONFIG 0x25
#define STATUS_IDLE             0x00
#define STATUS_OK               0x01
#define STATUS_BUSY             0x02
#define STATUS_CONTINUED_READ   0x03
#define STATUS_INVALID          0xff
#define REPORT_IDENTIFY         0x10
#define REPORT_TOUCH            0x11
#define MODE_APPLICATION        0x01
#define TCM_MAX_PAYLOAD         4096
#define TCM_ONE_READ            (TCM_MAX_PAYLOAD + 1) /* SPI read limit is independent of identify max_write_size */
#define MAX_OBJECTS             10

static uint8_t message[TCM_MAX_PAYLOAD + 8];
static uint8_t touch_config[256];
static uint32_t touch_config_size, max_x = 1200, max_y = 2670, max_objects = MAX_OBJECTS;
static struct {
	unsigned polls, messages, touch_reports, bad, other, touches, resyncs;
	uint32_t last_x, last_y;
} seen;

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static void
dump(const char *what, const uint8_t *p, uint32_t length)
{
	char line[3 * 40 + 1];
	uint32_t n = length < 40 ? length : 40;

	for (uint32_t i = 0; i < n; i++) {
		snprintf(line + 3 * i, sizeof(line) - 3 * i, "%02x ", p[i]);
	}
	line[3 * n] = 0;
	say("%s (%u bytes): %s\n", what, length, line);
}

/*
 * Read one message: its code into *code, its payload into message[], its length into *length.
 * The chip sends a message from its start in every transfer (0xa5, code, 16-bit length, payload,
 * 0x5a) until it has all been read; what a transfer leaves, the next one gets behind 0xa5 0x03
 * ("continued read", no length). So the first transfer is a generous one (READ_FIRST bytes,
 * Synaptics' "predictive reading"), and only a longer message takes a second for the rest.
 * false: no message (no marker: the chip is not there, or it is not talking yet).
 */
#define READ_FIRST              64
static unsigned raw_dumps;
static uint32_t pending_payload;        /* a big message whose header was read and whose rest was not yet */
static uint8_t pending_code;

static bool
tcm_read(uint8_t *code, uint32_t *length)
{
	static uint8_t first[READ_FIRST];
	uint32_t payload, total;

	if (spi_transfer == bitbang_transfer) {
		/*
		 * By hand the chip stays selected from the header to the padding byte: the whole message in
		 * one go, no "continued read" (which this chip answers with nothing but 0xff by hand).
		 */
		bitbang_select(true);
		bitbang_bytes(NULL, first, 4);
		if (first[0] != TCM_MARKER) {
			bitbang_select(false);
			seen.bad++;
			if (seen.bad <= 4 && first[0] != 0xff) {
				dump("no marker", first, 4);
			}
			return false;
		}
		*code = first[1];
		payload = le16(first + 2);
		*length = 0;
        uint32_t chunked=0;PE_parse_boot_argn("touch_chunked",&chunked,sizeof(chunked));
        if(ultra_touch && chunked && *code!=STATUS_CONTINUED_READ && payload<=TCM_MAX_PAYLOAD){
         bitbang_select(false);
         if(*code==STATUS_IDLE || *code==STATUS_BUSY || *code==STATUS_INVALID || payload==0)return true;
         unsigned remaining=payload+1,offset=0;
         while(remaining){
          unsigned count=remaining; /* vendor SPI RD_CHUNK_SIZE=0: no read limit */
          if(count==1){message[offset]=TCM_PADDING;break;}
          uint8_t lead[2];sleep_us(100);
          bitbang_select(true);bitbang_bytes(NULL,lead,2);
          if(lead[0]!=TCM_MARKER || lead[1]!=STATUS_CONTINUED_READ){
           bitbang_select(false);seen.bad++;
           if(seen.bad<8)dump("chunked read invalid header",lead,2);
           return false;
          }
          bitbang_bytes(NULL,message+offset,count);bitbang_select(false);
          offset+=count;remaining-=count;
         }
         if(message[payload]!=TCM_PADDING){seen.bad++;return false;}
         *length=payload;return true;
        }
		if (*code == STATUS_CONTINUED_READ && pending_payload != 0 && pending_payload <= TCM_MAX_PAYLOAD) {
			/*
			 * The rest of the big message whose header the last look had: 0xa5 0x03 and then its
			 * payload from the start (the two bytes read as a length are its first two).
			 */
			message[0] = first[2];
			message[1] = first[3];
			bitbang_bytes(NULL, message + 2, pending_payload - 2 + 1);
			bitbang_select(false);
			*code = pending_code;
			*length = pending_payload;
			pending_payload = 0;
			seen.resyncs++;
			return true;
		}
		if (*code == STATUS_CONTINUED_READ || payload > TCM_MAX_PAYLOAD) {
			/*
			 * Out of step: the chip is still sending the rest of an earlier message (behind 0xa5 0x03,
			 * no length of its own). Take all of it now, in this same selection, and drop it: the
			 * longest message there is (Xiaomi's frame) fits in the buffer, and what follows its end
			 * is 0xff.
			 */
			bitbang_bytes(NULL, message, 2048);
			bitbang_select(false);
			if (seen.resyncs++ < 6) {
				say("out of step (%02x %02x %02x %02x): the rest of a message dropped\n", first[0], first[1],
				    first[2], first[3]);
			}
			return false;
		}
		if (payload + 1 > TCM_ONE_READ) {
			pending_code = *code;
			/*
			 * More than the chip sends in one selection (Xiaomi's 1708-byte frame): the header was
			 * all of this one; the payload comes in the next, behind 0xa5 0x03 -- once the chip has
			 * it ready, so a selection that finds 0xff is let go and tried again.
			 */
			bitbang_select(false);
			for (int tries = 0;; tries++) {
				uint8_t lead[2];

				if (tries == 200) {
					/* (what the chip still has of it, the next look takes as the rest: pending_payload) */
					pending_payload = payload;
					seen.bad++;
					return false;
				}
				delay_for_interval(100, NSEC_PER_USEC);
				bitbang_select(true);
				bitbang_bytes(NULL, lead, 2);
				if (lead[0] == TCM_MARKER && lead[1] == STATUS_CONTINUED_READ) {
					break;
				}
				bitbang_select(false);
			}
		}
		bitbang_bytes(NULL, message, payload + 1);      /* and the padding byte */
		bitbang_select(false);
		if (raw_dumps < 8) {
			raw_dumps++;
			dump("message", first, 4);
		}
		if (*code == STATUS_CONTINUED_READ) {
			seen.bad++;
			return false;
		}
		*length = payload;
		return true;
	}
	if (!spi_transfer(NULL, first, sizeof(first))) {
		return false;
	}
	if (raw_dumps < 6 && first[0] != 0xff) {
		raw_dumps++;
		dump("raw", first, 16);
	}
	if (first[0] != TCM_MARKER) {
		seen.bad++;
		if (seen.bad <= 4) {
			dump("no marker", first, 8);
		}
		return false;
	}
	*code = first[1];
	*length = 0;
	if (*code == STATUS_CONTINUED_READ) {
		/* the rest of a message we lost track of: what is left of it was just read (and dropped) */
		seen.bad++;
		return false;
	}
	payload = le16(first + 2);
	if (*code == STATUS_IDLE || *code == STATUS_BUSY || *code == STATUS_INVALID || payload == 0) {
		return true;
	}
	if (payload > TCM_MAX_PAYLOAD) {
		say("message %#x of %u bytes: too long\n", *code, payload);
		return false;
	}
	total = 4 + payload + 1;
	if (total <= sizeof(first)) {
		memcpy(message, first + 4, payload);
	} else {
		uint32_t have = sizeof(first) - 4, rest = total - sizeof(first);

		memcpy(message, first + 4, have);
		/* the rest, behind 0xa5 0x03, padding byte included */
		if (!spi_transfer(NULL, message + have - 2, rest + 2)) {
			return false;
		}
		if (message[have - 2] != TCM_MARKER || message[have - 1] != STATUS_CONTINUED_READ) {
			seen.bad++;
			if (seen.bad <= 4) {
				dump("bad continued read", message + have - 2, 16);
			}
			return false;
		}
		/* the two header bytes sat where the payload goes on: put back what they overwrote */
		memcpy(message + have - 2, first + 4 + have - 2, 2);
	}
	*length = payload;
	return true;
}

static bool
tcm_write(uint8_t command, const uint8_t *payload, uint32_t length)
{
	uint8_t out[64];

	if (length + 3 > sizeof(out)) {
		return false;
	}
	out[0] = command;
	out[1] = (uint8_t)length;
	out[2] = (uint8_t)(length >> 8);
	if (length) {
		memcpy(out + 3, payload, length);
	}
	return spi_transfer(out, NULL, length + 3);
}

/* Wait (up to ms) for the interrupt line and read what comes, until a message with code `want`. */
static bool
tcm_wait_for(uint8_t want, uint32_t *length, uint32_t ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		uint8_t code;

		if (!ultra_touch && gpio_get(PIN_INT)) {
			sleep_us(500);
			continue;
		}
		if (!tcm_read(&code, length)) {
			sleep_ms(1);
			continue;
		}
		if (code == want) {
			return true;
		}
		if (code != STATUS_IDLE && seen.other++ < 12) {
			say("while waiting for %#x: message %#x (%u bytes)\n", want, code, *length);
			if (*length) {
				dump("  payload", message, *length);
			}
		}
	}
	return false;
}

static bool
tcm_command(uint8_t command, const uint8_t *payload, uint32_t payload_length, uint32_t *length)
{
	if (!tcm_write(command, payload, payload_length)) {
		return false;
	}
	if (!tcm_wait_for(STATUS_OK, length, 500)) {
		say("command %#x: no answer\n", command);
		return false;
	}
	return true;
}

static void
ultra_pin_state(const char *phase)
{
	if (!ultra_touch) return;
	say("xuanyuan %s: cfg MISO=%#x MOSI=%#x CLK=%#x CS=%#x RESET=%#x IRQ=%#x\n", phase,
	    rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_MOSI)), rd(GPIO_CFG(PIN_CLK)),
	    rd(GPIO_CFG(PIN_CS)), rd(GPIO_CFG(PIN_RESET)), rd(GPIO_CFG(PIN_INT)));
	say("xuanyuan %s: io MISO=%#x MOSI=%#x CLK=%#x CS=%#x RESET=%#x IRQ=%#x\n", phase,
	    rd(GPIO_IO(PIN_MISO)), rd(GPIO_IO(PIN_MOSI)), rd(GPIO_IO(PIN_CLK)),
	    rd(GPIO_IO(PIN_CS)), rd(GPIO_IO(PIN_RESET)), rd(GPIO_IO(PIN_INT)));
}

static void
touch_reset(void)
{
	gpio_set(PIN_RESET, false);
	ultra_pin_state("reset low");
	sleep_ms(10);                   /* synaptics,reset-active-ms */
	gpio_set(PIN_RESET, true);
	sleep_ms(200);                  /* synaptics,reset-delay-ms */
	ultra_pin_state("reset released");
}

static bool
tcm_probe(void)
{
	uint32_t length = 0;
	bool identified;

	touch_reset();
	identified = tcm_wait_for(REPORT_IDENTIFY, &length, 1000);
	if (!identified) {
		say("no identify report after reset (interrupt line %u); asking\n", gpio_get(PIN_INT));
		if (!tcm_write(CMD_IDENTIFY, NULL, 0)) {
			return false;
		}
		identified = tcm_wait_for(STATUS_OK, &length, 500);
	}
	if (!identified || length < 2) {
		say("the chip does not identify itself\n");
		return false;
	}
	dump("identify", message, length);
	if (length >= 22) {
		char part[17];

		memcpy(part, message + 2, 16);
		part[16] = 0;
		say("identify: version %u, mode %#x, part \"%s\", build %u\n", message[0], message[1], part,
		    message[18] | message[19] << 8 | message[20] << 16 | (uint32_t)message[21] << 24);
	}
	if (message[1] != MODE_APPLICATION) {
		say("not in application mode (%#x): running the application firmware\n", message[1]);
		if (!tcm_write(CMD_RUN_APPLICATION, NULL, 0) || !tcm_wait_for(REPORT_IDENTIFY, &length, 1000) ||
		    length < 2 || message[1] != MODE_APPLICATION) {
			say("still not in application mode\n");
			return false;
		}
	}
	{
		/* the chip sends a 1708-byte frame of its own unasked, every frame: too much for pins driven by hand */
		uint8_t report = REPORT_XIAOMI_FRAME;

		if (!tcm_command(CMD_DISABLE_REPORT, &report, 1, &length)) {
			say("report %#x could not be turned off\n", report);
		}
	}
	if (tcm_command(CMD_GET_APPLICATION_INFO, NULL, 0, &length) && length >= 40) {
		dump("application info", message, length);
		max_x = le16(message + 32);
		max_y = le16(message + 34);
		max_objects = le16(message + 36);
		say("application info: status %u, max x %u, max y %u, %u objects\n", le16(message + 2), max_x, max_y,
		    max_objects);
		if (max_objects == 0 || max_objects > MAX_OBJECTS) {
			max_objects = MAX_OBJECTS;
		}
		if (max_x == 0 || max_y == 0) {
			max_x = 1200;
			max_y = 2670;
		}
	}
	if (!tcm_command(CMD_GET_TOUCH_REPORT_CONFIG, NULL, 0, &length) || length == 0 || length > sizeof(touch_config)) {
		say("no touch report configuration\n");
		return false;
	}
	memcpy(touch_config, message, length);
	touch_config_size = length;
	dump("touch report configuration", touch_config, length);
	{
		uint8_t report = REPORT_TOUCH;

		tcm_command(CMD_ENABLE_REPORT, &report, 1, &length);
	}
	/*
	 * Out of any low-power state the boot left it in: out of deep sleep, the wake-up gesture mode
	 * off (Xiaomi's firmware may sit in it, scanning but not reporting fingers, until the display
	 * is said to be on), and what scan mode it says it is in: 0/1 normal idle/active, 2/3 the
	 * low-power wake-up gesture mode, 4 asleep.
	 */
	{
		uint8_t off_gesture[3] = { DC_ENABLE_WAKEUP_GESTURE_MODE, 0, 0 }, mode_id = DC_TOUCH_SCAN_MODE;

		say("exit deep sleep: %s\n", tcm_command(CMD_EXIT_DEEP_SLEEP, NULL, 0, &length) ? "ok" : "no answer");
		say("wake-up gesture mode off: %s\n",
		    tcm_command(CMD_SET_DYNAMIC_CONFIG, off_gesture, sizeof(off_gesture), &length) ? "ok" : "no answer");
		if (tcm_command(CMD_GET_DYNAMIC_CONFIG, &mode_id, 1, &length) && length >= 2) {
			say("scan mode %u\n", le16(message));
		} else {
			say("scan mode: no answer\n");
		}
	}
	return true;
}

/* ---- touch reports ---------------------------------------------------------------------------------- */

enum {
	TOUCH_END = 0, TOUCH_FOREACH_ACTIVE_OBJECT, TOUCH_FOREACH_OBJECT, TOUCH_FOREACH_END, TOUCH_PAD_TO_NEXT_BYTE,
	TOUCH_TIMESTAMP, TOUCH_OBJECT_N_INDEX, TOUCH_OBJECT_N_CLASSIFICATION, TOUCH_OBJECT_N_X_POSITION,
	TOUCH_OBJECT_N_Y_POSITION, TOUCH_NUM_OF_ACTIVE_OBJECTS = 24,
};
#define CLASS_LIFT              0
#define CLASS_FINGER            1
#define CLASS_GLOVED_FINGER     2

static struct {
	uint32_t status, x, y;
} object[MAX_OBJECTS];
static bool down[MAX_OBJECTS];
static uint32_t last_x[MAX_OBJECTS], last_y[MAX_OBJECTS];  /* so an UP carries where the finger was, not 0,0 */

static uint32_t
bits_at(const uint8_t *report, uint32_t size, uint32_t offset, uint32_t bits)
{
	uint32_t value = 0;

	if (bits == 0 || bits > 32 || offset + bits > size * 8) {
		return 0;
	}
	for (uint32_t i = 0; i < bits; i++) {
		uint32_t at = offset + i;

		value |= (uint32_t)(report[at / 8] >> (at % 8) & 1) << i;
	}
	return value;
}

static void
touch_report(const uint8_t *report, uint32_t size)
{
	uint32_t idx = 0, offset = 0, obj = 0, next = 0, objects = 0, active = 0, end_of_foreach = 0;
	bool active_only = false, have_active = false;

	memset(object, 0, sizeof(object));
	while (idx < touch_config_size) {
		uint8_t code = touch_config[idx++];
		uint32_t bits, value;

		switch (code) {
		case TOUCH_END:
			goto done;
		case TOUCH_FOREACH_ACTIVE_OBJECT:
		case TOUCH_FOREACH_OBJECT:
			obj = 0;
			next = idx;
			active_only = code == TOUCH_FOREACH_ACTIVE_OBJECT;
			break;
		case TOUCH_FOREACH_END:
			end_of_foreach = idx;
			if (active_only) {
				if (have_active) {
					objects++;
					if (objects < active) {
						idx = next;
					}
				} else if (offset < size * 8) {
					idx = next;
				}
			} else {
				obj++;
				if (obj < max_objects) {
					idx = next;
				}
			}
			break;
		case TOUCH_PAD_TO_NEXT_BYTE:
			offset = (offset + 7) / 8 * 8;
			break;
		default:
			if (idx >= touch_config_size) {
				goto done;
			}
			bits = touch_config[idx++];
			value = bits <= 32 ? bits_at(report, size, offset, bits) : 0;
			offset += bits;
			if (code == TOUCH_OBJECT_N_INDEX) {
				obj = value;
			} else if (obj < MAX_OBJECTS && code == TOUCH_OBJECT_N_CLASSIFICATION) {
				object[obj].status = value;
			} else if (obj < MAX_OBJECTS && code == TOUCH_OBJECT_N_X_POSITION) {
				object[obj].x = value;
			} else if (obj < MAX_OBJECTS && code == TOUCH_OBJECT_N_Y_POSITION) {
				object[obj].y = value;
			} else if (code == TOUCH_NUM_OF_ACTIVE_OBJECTS) {
				active = value;
				have_active = true;
				if (active == 0 && end_of_foreach != 0) {
					idx = end_of_foreach;
				}
			}
			break;
		}
	}
done:
	for (uint32_t i = 0; i < max_objects && i < MAX_OBJECTS; i++) {
		bool touching = object[i].status == CLASS_FINGER || object[i].status == CLASS_GLOVED_FINGER;
		uint32_t x = (uint32_t)((uint64_t)object[i].x * 65535 / max_x);
		uint32_t y = (uint32_t)((uint64_t)object[i].y * 65535 / max_y);

		if (touching) {
			if (seen.touches++ < 6) {
				say("finger %u at %u, %u (raw %u, %u)\n", i, x, y, object[i].x, object[i].y);
			}
			seen.last_x = object[i].x;
			seen.last_y = object[i].y;
			redwood_touch_post(down[i] ? REDWOOD_TOUCH_MOVE : REDWOOD_TOUCH_DOWN, i, x, y);
			last_x[i] = x;
			last_y[i] = y;
			down[i] = true;
		} else if (down[i]) {
			/* the finger has lifted where it last was -- not 0,0, which UIKit reads as a drag off the
			 * control and cancels the tap (a Settings row highlights but does not open) */
			redwood_touch_post(REDWOOD_TOUCH_UP, i, last_x[i], last_y[i]);
			down[i] = false;
		}
	}
}

/*
 * Xiaomi's 0xc0 frame (1708 bytes): what is in it is not known yet. The first one is written out
 * whole; after it, for each frame, how many bytes differ from that first one and where the first
 * differences are -- with a finger on the screen and without, that tells a capacitance image from
 * a list of touches.
 */
static uint8_t frame0[2048];
static uint32_t frame0_length;

static void
xiaomi_frame(const uint8_t *p, uint32_t length)
{
	static unsigned frames;
	char line[200];
	uint32_t differ = 0, n = 0;

	frames++;
	if (frame0_length == 0 && length <= sizeof(frame0)) {
		memcpy(frame0, p, length);
        frame0_length = length;
        uint32_t verbose=0;PE_parse_boot_argn("touch_frame_dump",&verbose,sizeof(verbose));
        if(!verbose){say("raw sensor frame: %u bytes; hex dump disabled (touch_frame_dump=1 enables)\n",length);return;}
		for (uint32_t at = 0; at < length; at += 48) {
			uint32_t k = 0;

			for (uint32_t i = at; i < length && i < at + 48; i++) {
				k += snprintf(line + k, sizeof(line) - k, "%02x", p[i]);
			}
			say("frame0 %4u: %s\n", at, line);
		}
		return;
	}
	if (length != frame0_length || frames > 40) {
		return;
	}
	for (uint32_t i = 12; i < length; i++) {        /* (the first 12: a counter and time stamps) */
		differ += p[i] != frame0[i];
	}
	if (differ == 0 && frames % 20 != 0) {
		return;
	}
	n = (uint32_t)snprintf(line, sizeof(line), "frame %u: %u bytes differ:", frames, differ);
	for (uint32_t i = 12, shown = 0; i < length && shown < 14; i++) {
		if (p[i] != frame0[i]) {
			n += (uint32_t)snprintf(line + n, sizeof(line) - n, " %u=%02x", i, p[i]);
			shown++;
		}
	}
	say("%s\n", line);
}

/* one message; false when there was none (idle, or nothing readable) */
static bool
poll_once(void)
{
	uint8_t code;
	uint32_t length;

	seen.polls++;
	if (!tcm_read(&code, &length)) {
		return false;
	}
	if (code == STATUS_IDLE) {
		return false;
	}
	seen.messages++;
	if (code == REPORT_TOUCH) {
		if (seen.touch_reports++ < 3) {
			dump("touch report", message, length);
		}
		touch_report(message, length);
	} else if (code == REPORT_XIAOMI_FRAME) {
		static unsigned asked;

		xiaomi_frame(message, length);
		/*
		 * In step with the chip again: what Android's driver does for enable_touch_raw=0 -- the
		 * frames for Xiaomi's host-side touch processing off, the chip's own touch reports on.
		 * (Seen to work there: 319 touch reports in 15 s, and the frames stop.)
		 */
		if (asked < 5) {
			uint8_t frame = REPORT_XIAOMI_FRAME, touch = REPORT_TOUCH;

			asked++;
            /* Drain each command response before sending the next command.
             * Ultra can leave IRQ high with readable TCM responses queued. */
            uint32_t reply_length=0;
            bool frame_off=tcm_command(CMD_DISABLE_REPORT,&frame,1,&reply_length);
            bool touch_on=tcm_command(CMD_ENABLE_REPORT,&touch,1,&reply_length);
            say("report switch: raw_off=%u touch_on=%u\n",frame_off,touch_on);
			say("frame %u read whole: asked for the chip's own touch reports instead (time %u)\n", seen.messages,
			    asked);
		}
	} else if (code != STATUS_IDLE) {
		if (seen.other++ < 8) {
			say("message %#x (%u bytes)\n", code, length);
			if (length) {
				dump("  payload", message, length);
			}
		}
		if (code == REPORT_IDENTIFY) {
			/* the chip has reset itself: its reports are to be asked for again */
			uint8_t report = REPORT_TOUCH;

			tcm_write(CMD_ENABLE_REPORT, &report, 1);
		}
	}
	return true;
}

/* ---- all of it together ---------------------------------------------------------------------------- */

static void
show_state(const char *when)
{
	say("%s: votes %#x, clocks %#x %#x %#x, s2 div %#x, ref cfg %#x dfs %#x; pins %#x %#x %#x %#x, reset %#x/%u, "
	    "int %#x/%u\n", when, rd(GCC_VOTE), rd(GCC_QUP1_M_AHB_CBCR), rd(GCC_QUP1_S_AHB_CBCR), rd(GCC_QUP1_S2_CBCR),
	    rd(GCC_QUP1_S2_DIV), rd(GCC_QSPI_REF_CFG), rd(GCC_QSPI_REF_DFSR), rd(GPIO_CFG(PIN_MISO)), rd(GPIO_CFG(PIN_MOSI)),
	    rd(GPIO_CFG(PIN_CLK)), rd(GPIO_CFG(PIN_CS)), rd(GPIO_CFG(PIN_RESET)), rd(GPIO_IO(PIN_RESET)),
	    rd(GPIO_CFG(PIN_INT)), rd(GPIO_IO(PIN_INT)));
}

static bool
bring_up(void)
{
	const char *problem;
	uint32_t spi_hz = 5000000;

	if (ultra_touch) {
		uint32_t native = 0;
		PE_parse_boot_argn("xuanyuantouch_native", &native, sizeof(native));
		if (native) {
			say("xuanyuan native: IF=%#x S0 cbcr=%#x RCG cmd=%#x cfg=%#x\n",
			    rd(GENI_IF_DISABLE_RO), rd(gcc + 0x18004), rd(gcc + 0x18014), rd(gcc + 0x18018));
			if (rd(GENI_IF_DISABLE_RO) & 1) {
				say("xuanyuan native: FIFO unavailable; GPI DMA transport required\n");
				return false;
			}
			wr(GCC_VOTE, rd(GCC_VOTE) | VOTE_QUP1_CORE_2X | VOTE_QUP1_CORE |
			    VOTE_QUP1_M_AHB | VOTE_QUP1_S_AHB | (1u << 22));
			se_source_hz = rcg_rate(rd(gcc + 0x18018), rd(gcc + 0x1801c), rd(gcc + 0x18020));
			se_clock_select = 0;
			if (!se_source_hz) { say("xuanyuan native: unknown S0 source rate\n"); return false; }
			const char *failure = geni_spi_init(1000000);
			if (failure) { say("xuanyuan native: %s\n", failure); return false; }
			gpio_output(PIN_RESET, false, PULL_NONE, 2);
			gpio_configure(PIN_INT, 0, PULL_NONE, 2, false);
			supplies_on(); sleep_ms(50);
			gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_DOWN, 6, false);
			gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_DOWN, 6, false);
			gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_DOWN, 6, false);
			gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_DOWN, 6, false);
			say("xuanyuan native: S0 source %u Hz, requesting 1 MHz\n", se_source_hz);
			return tcm_probe();
		}
		/* Do not program the dada S2 clock tree on xuanyuan's S0 engine. */
		say("xuanyuan: GPIO SPI probe, no GCC clock-tree writes\n");
		gpio_output(PIN_RESET, false, PULL_NONE, 2);
		gpio_configure(PIN_INT, 0, PULL_NONE, 2, false);
		supplies_on();
		sleep_ms(50);
		spi_transfer = bitbang_transfer;
		PE_parse_boot_argn("touch_half_ns", &bitbang_half_ns, sizeof(bitbang_half_ns));
		bitbang_init();
		return tcm_probe();
	}
	show_state("as found");
	gpio_output(PIN_RESET, false, PULL_NONE, 2);
	gpio_configure(PIN_INT, 0, PULL_NONE, 2, false);
	supplies_on();
	sleep_ms(50);                   /* synaptics,power-delay-ms */

	if (!clocks_on()) {
		show_state("no clocks");
		say("driving the pins by hand\n");
		spi_transfer = bitbang_transfer;
		bitbang_init();
		return tcm_probe();
	}
	say("serial engine: firmware %#x, interface %#x, parameters %#x, source clock %u Hz (select %u)\n",
	    rd(GENI_FW_REVISION_RO), rd(GENI_IF_DISABLE_RO), rd(SE_HW_PARAM_0), se_source_hz, se_clock_select);
	/* boot-arg touch_spi_hz=N (the device tree allows the chip 15 MHz) */
	PE_parse_boot_argn("touch_spi_hz", &spi_hz, sizeof(spi_hz));
	if (spi_hz < 100000 || spi_hz > 15000000) {
		spi_hz = 5000000;
	}
	problem = geni_spi_init(spi_hz);
	if (problem == NULL) {
		gpio_configure(PIN_MISO, PIN_FUNCTION_SPI, PULL_DOWN, 6, false);
		gpio_configure(PIN_MOSI, PIN_FUNCTION_SPI, PULL_DOWN, 6, false);
		gpio_configure(PIN_CLK, PIN_FUNCTION_SPI, PULL_DOWN, 6, false);
		gpio_configure(PIN_CS, PIN_FUNCTION_SPI, PULL_DOWN, 6, false);
		show_state("powered, on the serial engine");
		say("the bus at %u Hz\n", spi_hz);
		return tcm_probe();
	}
	/* boot-arg touch_half_ns=N: half a clock period of the hand-driven bus (default 200 ns) */
	PE_parse_boot_argn("touch_half_ns", &bitbang_half_ns, sizeof(bitbang_half_ns));
	say("%s: driving the pins by hand (half period %u ns)\n", problem, bitbang_half_ns);
	spi_transfer = bitbang_transfer;
	bitbang_init();
	show_state("powered, pins by hand");
	return tcm_probe();
}

/*
 * Under Gunyah a register this VM may not touch is no fault of ours to catch: the whole phone
 * stops. So the first time, every block is looked at alone, with a line before it and a pause
 * after (the loader's log is fetched every few seconds), and boot-arg dadatouch_step=N stops
 * before step N: the last line seen says which block it was.
 */
static bool
careful_look(void)
{
	const struct {
		const char *what;
		vm_offset_t *base;
		uint32_t offset;
	} steps[] = {
		{ "TLMM gpio40 cfg", &tlmm, 0x1000 * PIN_MISO },
		{ "TLMM gpio161 cfg", &tlmm, 0x1000 * PIN_RESET },
		{ "TLMM gpio162 io", &tlmm, 0x1000 * PIN_INT + 4 },
		{ "GCC vote 0x52008", &gcc, 0x52008 },
		{ "GCC m_ahb 0x23140", &gcc, 0x23140 },
		{ "GCC s2 cbcr 0x1827c", &gcc, 0x1827c },
		{ "GCC s2 div 0x1828c", &gcc, 0x1828c },
		{ "GCC qspi ref cmd 0x188c0", &gcc, 0x188c0 },
		{ "GCC qspi ref dfsr 0x188d4", &gcc, 0x188d4 },
		{ "RSC drv2 id", &rsc, 0 },
		{ "RSC tcs0 status", &rsc, TCS_BASE + 0x28 },
		{ "cmd-db magic", &cmd_db, 4 },
		{ "SE fw revision", &se, 0x68 },
		{ "SE hw param", &se, 0xe24 },
	};
	uint32_t stop = 99, pause_ms = 2500;

	PE_parse_boot_argn("dadatouch_step", &stop, sizeof(stop));
	PE_parse_boot_argn("dadatouch_pause", &pause_ms, sizeof(pause_ms));
	for (uint32_t i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
		if (i >= stop) {
			say("stopping before step %u (dadatouch_step)\n", i);
			return false;
		}
		say("step %u: reading %s ...\n", i, steps[i].what);
		sleep_ms(pause_ms);
		say("step %u: %s = %#x\n", i, steps[i].what, rd(*steps[i].base + steps[i].offset));
	}
	return true;
}

static void
dada_touch_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 4000;
	unsigned idle = 0;

	PE_parse_boot_argn("touch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 250 || poll_us > 100000) {
		poll_us = 4000;
	}
	{
		uint32_t start_s = 15;

		PE_parse_boot_argn("dadatouch_delay", &start_s, sizeof(start_s));
		say("thread up; looking at the hardware in %u s\n", start_s);
		sleep_ms(start_s * 1000);
	}
	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	se = ml_io_map(ultra_touch ? 0x00a80000UL : SE_PHYS, SE_SIZE);
	rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	cmd_db = ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);

	uint32_t careful = 0;

	PE_parse_boot_argn("dadatouch_careful", &careful, sizeof(careful));
	if (careful && !careful_look()) {
		thread_terminate(current_thread());
		return;
	}
	if (!bring_up()) {
		say("no touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	say("touch transport initialized (%u x %u, %u objects); awaiting verified finger reports, poll %u us\n", max_x, max_y,
	    max_objects, poll_us);
	uint64_t next_report;

	clock_interval_to_deadline(30, NSEC_PER_SEC, &next_report);
	for (unsigned tick = 1;; tick++) {
		/*
		 * Read until the chip says it has nothing more: it queues a report per frame (Xiaomi's 0xc0
		 * frames among them, tens a second), and one read per look let the queue overflow and drop
		 * the touch reports. (The interrupt line, gpio162, has not been seen low here: not relied on.)
		 */
		(void)idle;
		for (int n = 0; n < 32 && poll_once(); n++) {
		}
		if (mach_absolute_time() >= next_report) {
			clock_interval_to_deadline(30, NSEC_PER_SEC, &next_report);
			say("so far %u messages in %u looks (%u touch reports, %u other, %u bad), %u finger positions, "
			    "the last at %u, %u; interrupt line %s\n", seen.messages, seen.polls, seen.touch_reports, seen.other,
			    seen.bad, seen.touches, seen.last_x, seen.last_y, gpio_get(PIN_INT) ? "high" : "low");
		}
		sleep_us(poll_us);
	}
}

/*
 * Boot-arg autoswipe=S: S seconds into the run, one swipe up from the bottom edge, as a finger the
 * panel does not have (index 9) -- the lock screen opened without anybody at the phone, so that a
 * run can be checked past it from its screenshot alone. autoswipe_again=T repeats it T seconds
 * later (the first may land before SpringBoard takes gestures).
 */
static void
dada_autoswipe_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint32_t first = 0, again = 0;

	PE_parse_boot_argn("autoswipe", &first, sizeof(first));
	PE_parse_boot_argn("autoswipe_again", &again, sizeof(again));
	delay_for_interval(first, NSEC_PER_SEC);
	for (int round = 0; round < (again ? 2 : 1); round++) {
		const uint32_t x = 32768, from = 64500, to = 26000, steps = 24;

		say("autoswipe: a swipe up (%d)\n", round + 1);
		redwood_touch_post(REDWOOD_TOUCH_DOWN, 9, x, from);
		for (uint32_t i = 1; i <= steps; i++) {
			delay_for_interval(12, NSEC_PER_MSEC);
			redwood_touch_post(REDWOOD_TOUCH_MOVE, 9, x, from - (from - to) * i / steps);
		}
		delay_for_interval(12, NSEC_PER_MSEC);
		redwood_touch_post(REDWOOD_TOUCH_UP, 9, x, to);
		if (again) {
			delay_for_interval(again, NSEC_PER_SEC);
		}
	}
}

/* Vendor qcom,gpi-ee-offset is subtracted before GPII context offsets. */
/* Empty-channel lifecycle test: IOVA rings only, never publish a transfer. */
static void
xuanyuan_gpi_observe(const char *stage)
{
 uint32_t pause=0;PE_parse_boot_argn("gpipause",&pause,sizeof(pause));
 if(pause){say("GPI OBSERVE: %s; 1500ms host capture window\n",stage);sleep_ms(1500);}
}

static vm_offset_t
xuanyuan_gpi_prepare_memory(uint64_t low)
{
 vm_offset_t memory=ml_io_map_wcomb(low+0x10000,0x10000);
 if(!memory)return 0;
 say("GPI DMA prepare BEFORE SMMU context, route, clocks or channel writes\n");
 for(unsigned block=0;block<4;block++){
  say("GPI DMA clear BEFORE block=%u PA=%#llx\n",block,low+0x10000+block*0x4000);
  for(unsigned i=block*0x4000/4;i<(block+1)*0x4000/4;i++)smmu_write32(memory+i*4,0);
  __builtin_arm_dsb(0xf);say("GPI DMA clear block=%u completed\n",block);
 }
 return memory;
}

static void
xuanyuan_gpi_empty_lifecycle(vm_offset_t base, uint64_t low, vm_offset_t memory)
{
 vm_offset_t clock=ml_io_map(GCC_PHYS,GCC_SIZE);
 /* Buffer mapping and clearing completed before SMMU route activation. */
 if(!clock||!memory)return;
 uint32_t test_core=0;
 if(!PE_parse_boot_argn("bootcpu",&test_core,sizeof(test_core)) || test_core!=2){say("GPI START diagnostic requires verified bootcpu=2 live logging; skipped\n");return;}
 vm_offset_t serial=ml_io_map(0xa80000,0x4000);
 if(!serial)return;
 const unsigned serial_offsets[]={0xe1c,0x614,0x258,0xe18,0x28,0xe30,0x24,0x20};
 uint32_t serial_saved[8];for(unsigned i=0;i<8;i++)serial_saved[i]=rd(serial+serial_offsets[i]);
 vm_offset_t distributor=ml_io_map(0x16000000,0x10000);
 if(!distributor)return;
 /* Live Android: GPII1 SPI 280 -> architectural INTID 312. */
 uint32_t irq_bit=1u<<(312%32),irq_word=4*(312/32);
 uint32_t irq_enabled=rd(distributor+0x100+irq_word)&irq_bit;
 wr(distributor+0x180+irq_word,irq_bit);__builtin_arm_dsb(0xf);
 say("GPI empty IRQ312 masked=%u previous_enabled=%u\n",!(rd(distributor+0x100+irq_word)&irq_bit),!!irq_enabled);
 uint32_t vote=rd(clock+0x52008);
 const uint32_t offset[]={0x17088,0x170b8,0x17098,0x1709c,0x17108,0x17120,0x17188,0x1718c,0x17400,0x17404,0x17180};
 const uint32_t value[]={0x4f,1,3,1,1,15,0,0,0,0,1};
 const uint32_t mask[]={0x7f,1,3,1,1,15,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu,1};
 uint32_t saved[11];unsigned allocated[2]={0,0},started[2]={0,0};
 for(unsigned i=0;i<11;i++)saved[i]=rd(base+offset[i]);
 xuanyuan_gpi_observe("before clock vote");
 wr(clock+0x52008,vote|(0x1fu<<18));__builtin_arm_dsb(0xf);
 xuanyuan_gpi_observe("before GENI IRQ clear");
 /* Match vendor geni_se_irq_clear before selecting GPI DMA mode. */
 wr(serial+0xe18,0);
 wr(serial+0x618,0xffffffffu);wr(serial+0x648,0xffffffffu);
 wr(serial+0xc44,0xffffffffu);wr(serial+0xd44,0xffffffffu);
 wr(serial+0xe1c,0xffffffffu);__builtin_arm_dsb(0xf);
 xuanyuan_gpi_observe("before GENI IRQ mode");
 wr(serial+0xe1c,0);wr(serial+0x614,rd(serial+0x614)&~0x4c000001u);
 xuanyuan_gpi_observe("before GENI DMA mode");
 wr(serial+0x258,1);wr(serial+0xe18,rd(serial+0xe18)|15u);__builtin_arm_dsb(0xf);
 say("GPI empty serial mode DMA=%#x GSI=%#x S0 clock=%#x\n",rd(serial+0x258),rd(serial+0xe18),rd(clock+0x18004));


 __builtin_arm_dsb(0xf);
 for(unsigned i=0;i<11;i++)wr(base+offset[i],(saved[i]&~mask[i])|(value[i]&mask[i]));
 __builtin_arm_dsb(0xf);
 vm_offset_t event=base+0x15000;
 say("GPI empty lifecycle BEFORE event allocate; mapped IOVA=100000..10ffff\n");
 wr(base+0x17010,0);__builtin_arm_dsb(0xf);
 unsigned n;for(n=0;n<2500;n++){if((rd(event)>>20&15)==1)break;sleep_us(100);}
 if((rd(event)>>20&15)==1){
  wr(event,(16u<<24)|(1u<<16)|2);wr(event+4,1024);wr(event+8,0x108000);wr(event+12,0);
  wr(base+0x16104,0);wr(event+0x20,0);wr(event+0x28,0);wr(event+0x2c,0);wr(event+0x30,0);wr(event+0x34,0);
  __builtin_arm_dsb(0xf);wr(base+0x16100,0x108000+1024-16);__builtin_arm_dsb(0xf);
  wr(base+0x170a4,1);
  say("GPI empty event configured state=%u WP=%#x\n",rd(event)>>20&15,rd(event+0x18));
  for(unsigned ch=0;ch<2;ch++){
   vm_offset_t ctx=base+0x14000+ch*0x80;
   wr(base+0x17008,ch);__builtin_arm_dsb(0xf);
   for(n=0;n<2500;n++){if((rd(ctx)>>20&15)==1)break;sleep_us(100);}
   if((rd(ctx)>>20&15)!=1){say("GPI empty ch%u allocation failed config=%#x\n",ch,rd(ctx));break;}
   allocated[ch]=1;wr(base+0x170a0,1u<<ch);
   wr(ctx,(16u<<24)|2u|(ch==0?8u:0));wr(ctx+4,1024);wr(ctx+8,0x100000+ch*0x4000);wr(ctx+12,0);
   wr(base+0x16004+ch*8,0);wr(ctx+0x60,((ch==0?1u:0u)<<16)|(1u<<4));
   wr(ctx+0x64,0);wr(ctx+0x68,0);wr(ctx+0x6c,0);wr(ctx+0x5c,1);__builtin_arm_dsb(0xf);
   say("GPI empty ch%u configured; ring IOVA=%#x scratch=%#x\n",ch,rd(ctx+8),rd(ctx+0x60));
  }
  for(unsigned ch=0;allocated[0]&&allocated[1]&&ch<2;ch++){
   vm_offset_t ctx=base+0x14000+ch*0x80;
   say("GPI empty ch%u BEFORE start, both channels configured\n",ch);
   wr(base+0x17008,(1u<<24)|ch);__builtin_arm_dsb(0xf);
   for(n=0;n<2500;n++){if((rd(ctx)>>20&15)==2)break;sleep_us(100);}
   started[ch]=(rd(ctx)>>20&15)==2;
   say("GPI empty ch%u started=%u state=%u RP=%#x WP=%#x; no transfer doorbell\n",ch,started[ch],rd(ctx)>>20&15,rd(ctx+0x10),rd(ctx+0x18));
   wr(base+0x170a0,1u<<ch);
   if(!started[ch])break;
  }
  uint32_t transfer_test=0;PE_parse_boot_argn("xuanyuangpi_transfer",&transfer_test,sizeof(transfer_test));
  if(transfer_test==3){say("GPI chained transfer disabled after automatic reboot; no descriptor submitted\n");transfer_test=0;}
  if((transfer_test==1 || transfer_test==2 || transfer_test==3 || transfer_test==4 || transfer_test==5 || transfer_test==6) && started[0] && started[1]) {
   volatile uint32_t *ring=(volatile uint32_t*)memory;
   /* One SPI configuration element: no GO command, no bus transaction. */
   ring[0]=0x03000004;ring[1]=0x1e00;ring[2]=0x70008;ring[3]=0x220100;
   uint32_t prefill=0;PE_parse_boot_argn("xuanyuangpi_prefill",&prefill,sizeof(prefill));
   if(prefill){ring[4]=0x03000004;ring[5]=0x1e00;ring[6]=0x70008;ring[7]=0x220100;}
   if(transfer_test==5){ring[3]=0x220001;ring[4]=0x03000004;ring[5]=0x1e00;ring[6]=0x70008;ring[7]=0x220100;}
   uint32_t loopback=0;PE_parse_boot_argn("gpiloopback",&loopback,sizeof(loopback));
   if(loopback){ring[0]|=0x100;if(prefill||transfer_test==5)ring[4]|=0x100;say("GPI CONFIG internal SPI loopback enabled\n");}
   uint32_t clock_zero=0;PE_parse_boot_argn("gpiclkzero",&clock_zero,sizeof(clock_zero));
   if(clock_zero){ring[2]=8;if(prefill||transfer_test==5)ring[6]=8;say("GPI CONFIG using clock index 0 divider 8; actual bus rate unmeasured\n");}


   __builtin_arm_dsb(0xf);
   say("GPI CONFIG TRE BEFORE doorbell: IOVA=100000 WP=100010\n");
   if(transfer_test!=3 && transfer_test!=6){wr(base+0x16000,transfer_test==5?0x100020:0x100010);__builtin_arm_dsb(0xf);}
   unsigned waited;for(waited=0;transfer_test!=3 && transfer_test!=6 && waited<1000;waited++){if(rd(base+0x14010)==(transfer_test==5?0x100020u:0x100010u))break;sleep_us(100);}
   say("GPI CONFIG TRE completed=%u RP=%#x WP=%#x eventRP=%#x IRQ=%#x global=%#x error=%#x after=%u us\n",rd(base+0x14010)==(transfer_test==5?0x100020u:0x100010u),rd(base+0x14010),rd(base+0x14018),rd(event+0x10),rd(base+0x17080),rd(base+0x17100),rd(base+0x17200),waited*100);
   say("GPI CONFIG SMMU FSR=%#x FAR=%#x:%#x FSYNR=%#x\n",rd(ml_io_map(0x15085000,0x1000)+0x58),rd(ml_io_map(0x15085000,0x1000)+0x64),rd(ml_io_map(0x15085000,0x1000)+0x60),rd(ml_io_map(0x15085000,0x1000)+0x68));
   volatile uint32_t *ev=(volatile uint32_t*)(memory+0x8000);
   say("GPI CONFIG first event=%#x %#x %#x %#x\n",ev[0],ev[1],ev[2],ev[3]);
   if(transfer_test==5)say("GPI CONFIG pair end RP=%#x WP=%#x EVT=%#x/%#x\n",rd(base+0x14010),rd(base+0x14018),ev[2],ev[3]);
   if(transfer_test==4 && rd(base+0x14010)==0x100010) {
    wr(base+0x170c0,1);
    wr(base+0x16100,0x108000);__builtin_arm_dsb(0xf);
    if(!prefill){ring[4]=0x03000004;ring[5]=0x1e00;ring[6]=0x70008;ring[7]=0x220100;}__builtin_arm_dsb(0xf);
    uint32_t cache_test=0;PE_parse_boot_argn("xuanyuangpi_cache",&cache_test,sizeof(cache_test));
    if(cache_test){xuanyuan_dma_publish(memory,64);say("GPI second CONFIG descriptor PoC maintenance done\n");}
    say("GPI second CONFIG BEFORE doorbell WP=100020\n");
    wr(base+0x16000,0x100020);__builtin_arm_dsb(0xf);
    if(cache_test==2){
     say("GPI second CONFIG restart BEFORE state=%u\n",rd(base+0x14000)>>20&15);
     wr(base+0x17008,1u<<24);__builtin_arm_dsb(0xf);
     say("GPI second CONFIG restart command returned state=%u\n",rd(base+0x14000)>>20&15);
    }

    unsigned w;for(w=0;w<1000;w++){if(rd(base+0x14010)==0x100020)break;sleep_us(100);}
    say("GPI second CONFIG completed=%u TXRP=%#x TXWP=%#x EVRP=%#x IRQ=%#x after=%u us\n",rd(base+0x14010)==0x100020,rd(base+0x14010),rd(base+0x14018),rd(event+0x10),rd(base+0x17080),w*100);
    say("GPI second CONFIG event=%#x %#x %#x %#x\n",ev[4],ev[5],ev[6],ev[7]);
   }
   if((transfer_test==2 && rd(base+0x14010)==0x100010) || transfer_test==3 || transfer_test==6) {
    xuanyuan_gpi_observe("before SPI branch");
    ultra_touch=1;pin_miso=32;pin_mosi=33;pin_clk=34;pin_cs=35;
    uint32_t preserve_all=0;PE_parse_boot_argn("xuanyuangpi_preserve",&preserve_all,sizeof(preserve_all));
    tlmm=ml_io_map(TLMM_PHYS,TLMM_SIZE);gcc=clock;se=serial;
    if(!preserve_all){rsc=ml_io_map(RSC_PHYS,RSC_SIZE);cmd_db=ml_io_map_wcomb(CMD_DB_PHYS,CMD_DB_SIZE);}

    if(tlmm && (preserve_all || (rsc&&cmd_db))) {
     if(!preserve_all) {
     uint32_t int_cfg=rd(tlmm+PIN_INT*0x1000+8);
     say("GPI SPI touch IRQ cfg BEFORE=%#x status=%#x; masking delivery for polling\n",int_cfg,rd(tlmm+PIN_INT*0x1000+12));
     wr(tlmm+PIN_INT*0x1000+8,int_cfg&~1u);__builtin_arm_dsb(0xf);
     uint32_t reset_test=0;PE_parse_boot_argn("xuanyuangpi_reset",&reset_test,sizeof(reset_test));
     if(reset_test)gpio_output(PIN_RESET,false,PULL_NONE,2);
     gpio_configure(PIN_INT,0,PULL_NONE,2,false);

     uint32_t power_test=0;PE_parse_boot_argn("xuanyuangpi_power",&power_test,sizeof(power_test));
     if(power_test){supplies_on();sleep_ms(50);}else say("GPI SPI preserving firmware power rails; no RPMh writes\n");
     for(unsigned pin=32;pin<=35;pin++)gpio_configure(pin,1,PULL_DOWN,6,false);
     wr(GENI_CGC_CTRL,rd(GENI_CGC_CTRL)|0x7f);wr(SE_DMA_GENERAL_CFG,rd(SE_DMA_GENERAL_CFG)|0xf);wr(GENI_OUTPUT_CTRL,0x7f);wr(GENI_FORCE_DEFAULT,1);
     if(reset_test)touch_reset();else say("GPI SPI: preserving touch reset level=%u; no hardware reset\n",gpio_get(PIN_RESET));
     } else say("GPI SPI: preserving all firmware pins, power and reset state\n");
     uint32_t io_init=0;PE_parse_boot_argn("gpiioinit",&io_init,sizeof(io_init));
     if(io_init){
      xuanyuan_gpi_observe("before GENI internal IO initialization");
      wr(serial+0x28,rd(serial+0x28)|0x7f);
      wr(serial+0xe30,rd(serial+0xe30)|0xf);
      wr(serial+0x24,0x7f);wr(serial+0x20,1);__builtin_arm_dsb(0xf);
      say("GPI SPI internal IO configured CGC=%#x DMA=%#x OUTPUT=%#x; pins and power untouched\n",rd(serial+0x28),rd(serial+0xe30),rd(serial+0x24));
     }

     /* Drop CONFIG completion, then RX data before issuing TX GO. */
     /* Only transfer 2 consumed a standalone CONFIG event. Return that
      * slot with wraparound; a fresh batch keeps the initial last-slot WP.
      * Writing base+length is outside the event ring. */
     if(transfer_test==2){
      wr(base+0x170c0,1);wr(base+0x16100,0x108000);__builtin_arm_dsb(0xf);
     }
     volatile uint32_t *rxring=(volatile uint32_t*)(memory+0x4000);
     volatile uint32_t *txdata=(volatile uint32_t*)(memory+0xc000);
     volatile uint32_t *rxdata=(volatile uint32_t*)(memory+0xd000);
     txdata[0]=loopback?0x12345678u:0xffffffffu;rxdata[0]=0xccccccccu;
     rxring[0]=0x10d000;rxring[1]=0;rxring[2]=4;rxring[3]=0x100000;
     if(transfer_test==3 || transfer_test==6)ring[3]=0x220001;
     ring[4]=3;ring[5]=0;ring[6]=4;ring[7]=0x200801;
     ring[8]=0x10c000;ring[9]=0;ring[10]=4;ring[11]=0x100200;
     __builtin_arm_dsb(0xf);
     say("GPI SPI4 BEFORE RX/TX doorbells IRQ=%u\n",gpio_get(PIN_INT));
     uint32_t go_only=0;PE_parse_boot_argn("gpigoonly",&go_only,sizeof(go_only));
     uint32_t tx_only=0;PE_parse_boot_argn("gpitxonly",&tx_only,sizeof(tx_only));
     if(tx_only){ring[4]=1;ring[6]=0;ring[7]=0x200001;__builtin_arm_dsb(0xf);}
     if(go_only){ring[4]=2;ring[6]=4;ring[7]=0x200900;__builtin_arm_dsb(0xf);}
     say("GPI SPI identity FW=%#x IF=%#x HW=%#x; go_only=%u\n",rd(serial+0x68),rd(serial+0x64),rd(serial+0xe24),go_only);
     uint32_t tx_config_only=0;PE_parse_boot_argn("gpitxconfig",&tx_config_only,sizeof(tx_config_only));
     if(tx_config_only){ring[3]=0x220100;__builtin_arm_dsb(0xf);}
     xuanyuan_gpi_observe("before SPI RX/TX doorbells");
     if(!tx_only){wr(base+0x16008,0x104010);__builtin_arm_dsb(0xf);}
     else say("GPI SPI4 TX-only control: RX doorbell skipped; GO has no RX link\n");
     say("GPI SPI4 RX doorbell submitted RP=%#x WP=%#x; TX not submitted\n",rd(base+0x14090),rd(base+0x14098));
     xuanyuan_gpi_observe("after RX doorbell before TX");
     uint32_t rx_only=0;PE_parse_boot_argn("gpirxonly",&rx_only,sizeof(rx_only));
     if(!rx_only){
      wr(base+0x16000,tx_config_only?0x100010:(go_only?0x100020:0x100030));__builtin_arm_dsb(0xf);
      say("GPI SPI4 TX doorbell submitted\n");
     }else say("GPI SPI4 RX-only control: TX CONFIG/GO/DMA not submitted\n");
     unsigned w;for(w=0;w<1500;w++){if(rd(base+0x14010)==0x100030&&rd(base+0x14090)==0x104010)break;sleep_us(100);}
     say("GPI SPI4 result TXRP=%#x RXRP=%#x DATA=%#x eventRP=%#x irq=%#x after=%u us\n",rd(base+0x14010),rd(base+0x14090),rxdata[0],rd(event+0x10),rd(base+0x17080),w*100);
     if(loopback)say("GPI SPI4 loopback complete=%u pattern_match=%u expected=0x12345678 actual=%#x\n",rd(base+0x14010)==0x100030&&rd(base+0x14090)==0x104010,rxdata[0]==0x12345678u,rxdata[0]);
     say("GPI SPI4 regs TXcfg=%#x RXcfg=%#x TXdb=%#x RXdb=%#x GENIstatus=%#x M_CMD=%#x M_IRQ=%#x DMAcfg=%#x GSI=%#x CLK=%#x SEL=%#x\n",rd(base+0x14000),rd(base+0x14080),rd(base+0x16000),rd(base+0x16008),rd(GENI_STATUS),rd(GENI_M_CMD0),rd(serial+0x610),rd(SE_DMA_GENERAL_CFG),rd(SE_GSI_EVENT_EN),rd(serial+0x48),rd(serial+0x7c));

     say("GPI SPI4 WPs TX=%#x RX=%#x descriptor0=%#x/%#x GO=%#x/%#x\n",rd(base+0x14018),rd(base+0x14098),ring[0],ring[3],ring[4],ring[7]);
     for(unsigned e=1;e<4;e++)say("GPI SPI4 event%u=%#x %#x %#x %#x\n",e,ev[e*4],ev[e*4+1],ev[e*4+2],ev[e*4+3]);
    }
   }

  }
  /* Vendor SPI channels are a group: stop both, then reset both, then
   * release both. Never reset one while its peer is still running. */
  for(unsigned ch=0;ch<2;ch++)if(allocated[ch]&&started[ch]){
   vm_offset_t ctx=base+0x14000+ch*0x80;
   wr(base+0x17008,(2u<<24)|ch);__builtin_arm_dsb(0xf);
   for(n=0;n<2500;n++){if((rd(ctx)>>20&15)==3)break;sleep_us(100);}
   say("GPI empty ch%u stopped state=%u\n",ch,rd(ctx)>>20&15);wr(base+0x170a0,1u<<ch);
  }
  for(unsigned ch=0;ch<2;ch++)if(allocated[ch]){
   vm_offset_t ctx=base+0x14000+ch*0x80;
   wr(base+0x17008,(9u<<24)|ch);__builtin_arm_dsb(0xf);
   for(n=0;n<2500;n++){if((rd(ctx)>>20&15)==1)break;sleep_us(100);}
   say("GPI empty ch%u reset state=%u\n",ch,rd(ctx)>>20&15);wr(base+0x170a0,1u<<ch);
  }
  for(unsigned ch=0;ch<2;ch++)if(allocated[ch]){
   vm_offset_t ctx=base+0x14000+ch*0x80;
   wr(base+0x17008,(10u<<24)|ch);__builtin_arm_dsb(0xf);sleep_ms(1);
   say("GPI empty ch%u deallocated state=%u\n",ch,rd(ctx)>>20&15);wr(base+0x170a0,1u<<ch);
  }
  wr(base+0x17010,10u<<24);__builtin_arm_dsb(0xf);sleep_ms(1);wr(base+0x170a4,1);
 }
 for(unsigned i=11;i>0;i--)wr(base+offset[i-1],saved[i-1]);
 for(unsigned i=8;i>0;i--)wr(serial+serial_offsets[i-1],serial_saved[i-1]);
 wr(clock+0x52008,vote);__builtin_arm_dsb(0xf);
 wr(distributor+0x280+irq_word,irq_bit);__builtin_arm_dsb(0xf);
 if(irq_enabled)wr(distributor+0x100+irq_word,irq_bit);
 __builtin_arm_dsb(0xf);
 say("GPI empty IRQ312 enable restored=%u pending=%u\n",(rd(distributor+0x100+irq_word)&irq_bit)==irq_enabled,!!(rd(distributor+0x200+irq_word)&irq_bit));
 say("GPI empty lifecycle finished TX=%u RX=%u, IRQ config and clock vote restored\n",started[0],started[1]);
}

static void
xuanyuan_gpi_probe_thread(__unused void *arg, __unused wait_result_t result)
{
    uint32_t memory_only=0;
    PE_parse_boot_argn("xuanyuangpi_memory",&memory_only,sizeof(memory_only));
    if(memory_only){
     uint64_t low=0;uint32_t mb=0;
     PE_parse_boot_argn("lowdma",&low,sizeof(low));PE_parse_boot_argn("lowdma_mb",&mb,sizeof(mb));
     if(low && !(low&0x3fff) && low<0xfffc0000ULL && mb>=1){
      vm_offset_t memory=ml_io_map_wcomb(low+0x10000,0x10000);
      say("GPI memory-only BEGIN PA=%#llx VA=%#llx; no peripheral mappings or writes\n",low+0x10000,(uint64_t)memory);
      if(memory){
       for(unsigned block=0;block<4;block++){
        say("GPI memory-only BEFORE block=%u\n",block);
        for(unsigned i=block*0x4000/4;i<(block+1)*0x4000/4;i++)smmu_write32(memory+i*4,0);
        __builtin_arm_dsb(0xf);
        say("GPI memory-only block=%u cleared first=%#x last=%#x\n",block,rd(memory+block*0x4000),rd(memory+(block+1)*0x4000-4));
       }
       say("GPI memory-only PASS; no SMMU route or GPI command issued\n");
      }
     }else say("GPI memory-only skipped: invalid reserved lowdma window\n");
     thread_terminate(current_thread());return;
    }
	vm_offset_t base = ml_io_map(0x00a00000UL, 0x60000UL);
	say("xuanyuan GPI probe: base a00000, EE offset 10000, GPII 1; read only\n");
	if (!base) { say("GPI mapping failed\n"); thread_terminate(current_thread()); return; }
	for (unsigned ch = 0; ch < 2; ch++) {
		vm_offset_t ctx = base + 0x14000 + 0x80 * ch;
		say("GPI ch%u before context read\n", ch);
		say("GPI ch%u config=%#x length=%#x base=%#x:%#x rp=%#x:%#x wp=%#x:%#x\n", ch,
		    rd(ctx), rd(ctx+4), rd(ctx+12), rd(ctx+8), rd(ctx+20), rd(ctx+16), rd(ctx+28), rd(ctx+24));
	}
	say("GPI event before context read\n");
	vm_offset_t event = base + 0x15000;
	say("GPI event config=%#x length=%#x base=%#x:%#x rp=%#x:%#x wp=%#x:%#x\n",
	    rd(event), rd(event+4), rd(event+12), rd(event+8), rd(event+20), rd(event+16), rd(event+28), rd(event+24));
	say("GPI read-only probe complete\n");
	uint32_t inspect_smmu = 0;
	PE_parse_boot_argn("xuanyuangpi_smmu", &inspect_smmu, sizeof(inspect_smmu));
	if (inspect_smmu) {
		vm_offset_t smmu = ml_io_map(0x15000000UL, 0x100000UL);
		say("GPI SMMU probe: before global ID reads, stream 0xb6\n");
		if (smmu) {
			uint32_t idr0 = rd(smmu+0x20), idr1 = rd(smmu+0x24);
			unsigned count = idr0 & 0xff, found = 0;
			say("GPI SMMU IDR2=%#x\n", rd(smmu+0x28));
			say("GPI SMMU SCR0=%#x IDR0=%#x IDR1=%#x, %u entries\n", rd(smmu), idr0, idr1, count);
			for (unsigned i=0; i<count && i<6; i++) {
				uint32_t smr = rd(smmu+0x800+4*i), s2cr = rd(smmu+0xc00+4*i);
				uint32_t mask = smr>>16 & 0x7fff;
				if (smr & 0x80000000u)
					say("GPI SMMU active route %u SMR=%#x S2CR=%#x bank=%u type=%u\n", i, smr, s2cr, s2cr&0xff, s2cr>>16&3);
				if ((smr & 0x80000000u) && (((smr & 0x7fff)^0xb6u) & ~mask) == 0) {
					say("GPI SMMU matching entry %u SMR=%#x S2CR=%#x bank=%u type=%u\n", i, smr, s2cr, s2cr&0xff, s2cr>>16&3); found++;
				}
			}
			say("GPI SMMU read-only complete: %u matches for stream 0xb6; no mapping changed\n", found);
			unsigned banks=idr1&255, pages=1u<<(((idr1>>28)&7)+1);
			if (!(idr1>>31) && banks>1 && banks<=128 && pages<=128) {
				for (unsigned b=0; b<banks && b<5; b++) {
					unsigned cb=(pages+b)*0x1000;
					say("GPI SMMU bank %u SCTLR=%#x CBAR=%#x CBA2R=%#x TCR=%#x TCR2=%#x MAIR0=%#x\n", b,
					    rd(smmu+cb),rd(smmu+0x1000+4*b),rd(smmu+0x1800+4*b),rd(smmu+cb+0x30),rd(smmu+cb+0x10),rd(smmu+cb+0x38));
				}
				unsigned bank=5, ctx=(pages+bank)*0x1000;
				say("GPI SMMU inspected bank=%u offset=%#x SCTLR=%#x CBAR=%#x CBA2R=%#x\n",bank,ctx,
				    rd(smmu+ctx),rd(smmu+0x1000+4*bank),rd(smmu+0x1800+4*bank));
				unsigned references=0;
				for(unsigned i=0;i<count&&i<6;i++) {
					uint32_t smr=rd(smmu+0x800+4*i), route=rd(smmu+0xc00+4*i);
					if((smr&0x80000000u)&&((route>>16)&3)==0&&(route&255)==bank)references++;
				}
				say("GPI SMMU inspected bank has %u active stream references\n",references);
				/* Read-only diagnostics: no context ownership is inferred here. */

				uint32_t context_test=0;
				PE_parse_boot_argn("xuanyuangpi_smmu_context", &context_test, sizeof(context_test));
				if(context_test==3 && rd(smmu+0x1014)==0x1f300 && rd(smmu+0x1814)==1 && rd(smmu+0xc14)==5 && (rd(smmu+0x814)==0 || rd(smmu+0x814)==0x800000b6u)) {
					say("GPI SMMU cleanup BEFORE FAULT route: slot5 S2CR=%#x SMR=%#x\n",rd(smmu+0xc14),rd(smmu+0x814));
					smmu_write32(smmu+0xc14,0x20000);__builtin_arm_dsb(0xf);
					say("GPI SMMU cleanup FAULT readback=%#x\n",rd(smmu+0xc14));
					smmu_write32(smmu+0x814,0);__builtin_arm_dsb(0xf);
					say("GPI SMMU cleanup invalid SMR readback=%#x\n",rd(smmu+0x814));
					smmu_write32(smmu+(pages+5)*0x1000,0);__builtin_arm_dsb(0xf);
					say("GPI SMMU cleanup bank disabled=%u\n",rd(smmu+(pages+5)*0x1000)==0);
				}
				if((context_test==1 || context_test==2 || context_test==4) && idr0==0x4c017e7f && idr1==0x60000053 && rd(smmu+0x28)==0x5111) {
					unsigned test_bank=5, refs=0, cb=(pages+5)*0x1000;
					for(unsigned i=0;i<count&&i<6;i++) {
						uint32_t match=rd(smmu+0x800+4*i),route=rd(smmu+0xc00+4*i);
						if((match&0x80000000u)&&((route>>16)&3)==0&&(route&255)==test_bank)refs++;
					}
					uint64_t low=0;uint32_t mb=0;
					PE_parse_boot_argn("lowdma",&low,sizeof(low));PE_parse_boot_argn("lowdma_mb",&mb,sizeof(mb));
					if(!refs && ((!rd(smmu+0x1014) && !rd(smmu+0x1814)) || (rd(smmu+0x1014)==0x1f300 && rd(smmu+0x1814)==1 && rd(smmu+cb+0x38)==0x44)) && !(rd(smmu+cb)&1) && low && !(low&0x3fff) && low<((1ULL<<36)-0x30000) && mb>=1) {
						uint64_t table_pa=low+0x20000, buffer_pa=low+0x10000;
						vm_offset_t prepared_memory=context_test==4?xuanyuan_gpi_prepare_memory(low):0;
						vm_offset_t mapped=ml_io_map_wcomb(table_pa,0x4000);
						if(mapped && (context_test!=4 || prepared_memory)) {
							volatile uint32_t *words=(volatile uint32_t*)mapped;
							for(unsigned i=0;i<0x4000/4;i++)smmu_write32(mapped+i*4,0);
							uint64_t entry=(table_pa+0x1000)|3;words[0]=(uint32_t)entry;words[1]=(uint32_t)(entry>>32);
							entry=(table_pa+0x2000)|3;words[0x1000/4]=(uint32_t)entry;words[0x1000/4+1]=(uint32_t)(entry>>32);
							for(unsigned i=0;i<16;i++) {
								entry=(buffer_pa+i*0x1000)|0x60000000000743ULL;
								unsigned at=0x2000/4+(256+i)*2;words[at]=(uint32_t)entry;words[at+1]=(uint32_t)(entry>>32);
							}
							__builtin_arm_dsb(0xf);
							say("GPI SMMU context BEFORE: bank=5 tables=%#llx buffer=%#llx IAS/OAS=36; no stream writes\n",table_pa,buffer_pa);
							smmu_write32(smmu+0x1814,1);smmu_write32(smmu+0x1014,0x1f300);
							smmu_write32(smmu+cb+0x10,0x38011);smmu_write32(smmu+cb+0x30,0x80301c);
							smmu_write64(smmu+cb+0x20,table_pa|(5ULL<<48));smmu_write64(smmu+cb+0x28,5ULL<<48);
							smmu_write32(smmu+cb+0x38,0x44);smmu_write32(smmu+cb+0x3c,0);
							smmu_write32(smmu+cb,0);__builtin_arm_dsb(0xf);
							say("GPI SMMU context readback: CBAR=%#x CBA2R=%#x TCR=%#x TCR2=%#x TTBR=%#x:%#x MAIR=%#x SCTLR=%#x\n",rd(smmu+0x1014),rd(smmu+0x1814),rd(smmu+cb+0x30),rd(smmu+cb+0x10),rd(smmu+cb+0x24),rd(smmu+cb+0x20),rd(smmu+cb+0x38),rd(smmu+cb));
							unsigned ok=rd(smmu+0x1814)==1 && rd(smmu+cb+0x30)==0x80301c && (rd(smmu+cb+0x10)&~0x60u)==0x38011 && rd(smmu+cb+0x20)==(uint32_t)table_pa && rd(smmu+cb+0x24)==(uint32_t)((table_pa|(5ULL<<48))>>32) && rd(smmu+cb+0x38)==0x44 && rd(smmu+cb)==0;
							say("GPI SMMU context configured=%u; bank left disabled, no route or DMA started\n",ok);
							if(ok && (context_test==2 || context_test==4)) {
								say("GPI SMMU sync BEFORE: bank=5, streams untouched\n");
								smmu_write32(smmu+cb+0x610,5);__builtin_arm_dsb(0xf);
								smmu_write32(smmu+cb+0x7f0,0xffffffffu);__builtin_arm_dsb(0xf);
								unsigned polls=0;
								while((rd(smmu+cb+0x7f4)&1) && polls<1000){sleep_us(1);polls++;}
								unsigned synced=!(rd(smmu+cb+0x7f4)&1);
								say("GPI SMMU sync completed=%u polls=%u status=%#x\n",synced,polls,rd(smmu+cb+0x7f4));
								if(synced) {
									unsigned slot=128;
									for(unsigned i=0;i<count&&i<6;i++)if(!(rd(smmu+0x800+4*i)&0x80000000u)){slot=i;break;}
									if(slot<128) {
										uint32_t old_match=rd(smmu+0x800+4*slot);
										say("GPI SMMU route BEFORE: slot=%u bank=5; DMA remains stopped\n",slot);
										smmu_write32(smmu+cb,0x1007);__builtin_arm_dsb(0xf);
										smmu_write32(smmu+0xc00+4*slot,5);smmu_write32(smmu+0x800+4*slot,0x800000b6u);__builtin_arm_dsb(0xf);
										say("GPI SMMU route active SMR=%#x S2CR=%#x SCTLR=%#x\n",rd(smmu+0x800+4*slot),rd(smmu+0xc00+4*slot),rd(smmu+cb));
										if(context_test==4)xuanyuan_gpi_empty_lifecycle(base,low,prepared_memory);
										smmu_write32(smmu+0xc00+4*slot,0x20000);__builtin_arm_dsb(0xf);smmu_write32(smmu+0x800+4*slot,old_match);__builtin_arm_dsb(0xf);smmu_write32(smmu+cb,0);__builtin_arm_dsb(0xf);
										say("GPI SMMU route removed=%u bank disabled=%u; test path finished\n",rd(smmu+0x800+4*slot)==old_match&&rd(smmu+0xc00+4*slot)==0x20000,rd(smmu+cb)==0);
									}
								}
							}

						}
					} else say("GPI SMMU context skipped: bank references=%u low=%#llx mb=%u\n",refs,low,mb);
				}
				uint32_t attribute_test=0;
				PE_parse_boot_argn("xuanyuangpi_smmu_attr", &attribute_test, sizeof(attribute_test));
				if(attribute_test==1 && idr0==0x4c017e7f && idr1==0x60000053 && rd(smmu+0x28)==0x5111) {
					/* Only the first unreferenced bank after the five observed firmware routes. */
					unsigned test_bank=5, refs=0;
					for(unsigned i=0;i<count&&i<6;i++) {
						uint32_t match=rd(smmu+0x800+4*i),route=rd(smmu+0xc00+4*i);
						if((match&0x80000000u)&&((route>>16)&3)==0&&(route&255)==test_bank) refs++;
					}
					uint32_t attr=rd(smmu+0x1000+4*test_bank),ext=rd(smmu+0x1800+4*test_bank);
					if(!refs && !attr && !ext && !(rd(smmu+(pages+test_bank)*0x1000)&1)) {
						say("GPI SMMU attribute test BEFORE: bank=5 CBA2R=0; no stream/context enable writes\n");
						smmu_write32(smmu+0x1800+4*test_bank,1);__builtin_arm_dsb(0xf);
						uint32_t set=rd(smmu+0x1800+4*test_bank);
						say("GPI SMMU attribute test set readback=%#x\n",set);
						smmu_write32(smmu+0x1800+4*test_bank,ext);__builtin_arm_dsb(0xf);
						say("GPI SMMU attribute test restored=%u readback=%#x\n",rd(smmu+0x1800+4*test_bank)==ext,rd(smmu+0x1800+4*test_bank));
					} else say("GPI SMMU attribute test skipped: refs=%u CBAR=%#x CBA2R=%#x\n",refs,attr,ext);
				}

			}

		}
	}

	uint32_t allocate = 0;
	PE_parse_boot_argn("xuanyuangpi_allocate", &allocate, sizeof(allocate));
	if (allocate) {
		uint32_t step = 0;
		PE_parse_boot_argn("xuanyuangpi_step", &step, sizeof(step));
		if (!step) {
			say("GPI write test requires an explicit stage, stopping before writes\n");
			thread_terminate(current_thread()); return;
		}
		say("GPI write test stage %u: before clock map\n", step);
		vm_offset_t clock = ml_io_map(GCC_PHYS, GCC_SIZE);
		uint32_t previous = rd(clock + 0x52008);
		say("GPI stage 1: before clock vote write, previous=%#x\n", previous);
		wr(clock + 0x52008, previous | (0x1fu << 18));
		__builtin_arm_dsb(0xf);
		say("GPI allocate probe: clock votes=%#x; no DMA rings or doorbells\n", rd(clock + 0x52008));
		if (step == 2) {
			/* Validate writable instance access before any channel command. */
			vm_offset_t scratch = base + 0x17400;
			uint32_t saved = rd(scratch);
			say("GPI stage 2: before scratch write at a17400, original=%#x\n", saved);
			wr(scratch, 0x5855414eu); __builtin_arm_dsb(0xf);
			uint32_t observed = rd(scratch);
			wr(scratch, saved); __builtin_arm_dsb(0xf);
			say("GPI stage 2 scratch observed=%#x restored=%#x, access %s\n",
			    observed, rd(scratch), observed == 0x5855414eu ? "PASS" : "FAIL");
		}
		if (step == 5 || step == 6 || step == 7 || step == 8) {
			/* Actual-phone gpi_config_interrupts: retain polling-only delivery here. */
			const uint32_t offset[] = {0x17088,0x170b8,0x17098,0x1709c,0x17108,0x17120,0x17188,0x1718c,0x17400,0x17404,0x17180};
			const uint32_t value[] = {0x4f,1,3,1,1,15,0,0,0,0,1};
			const uint32_t mask[] = {0x7f,1,3,1,1,15,0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu,1};
			uint32_t saved[11];
			for (unsigned i=0;i<11;i++) saved[i]=rd(base+offset[i]);
			for (unsigned i=0;i<11;i++) {
				say("GPI IRQ setup: before write +%#x = %#x\n",offset[i],value[i]);
				wr(base+offset[i], (saved[i] & ~mask[i]) | (value[i] & mask[i])); __builtin_arm_dsb(0xf);
				say("GPI IRQ setup: read +%#x = %#x\n",offset[i],rd(base+offset[i]));
			}
			say("GPI IRQ setup: status=%#x CH=%#x EV=%#x; no allocate or doorbell\n",rd(base+0x17080),rd(base+0x17090),rd(base+0x17094));
			if (step == 6 || step == 7 || step == 8) {
				say("GPI stage 6: IRQ configured; BEFORE event allocate a17010\n");
				wr(base+0x17010,0); __builtin_arm_dsb(0xf);
				say("GPI stage 6: event command write RETURNED\n");
				unsigned wait;
				for(wait=0;wait<2500;wait++) { if ((rd(event)>>20&0xf)!=0) break; sleep_us(100); }
				uint32_t state=rd(event)>>20&0xf;
				say("GPI stage 6: event state=%u config=%#x irq=%#x after %u us\n",state,rd(event),rd(base+0x17094),wait*100);
				if (state==1 && (step==7 || step==8)) {
					for (unsigned ch=0;ch<2;ch++) {
						vm_offset_t ctx=base+0x14000+0x80*ch;
						uint32_t initial=rd(ctx)>>20&15;
						if (initial==1 && step==8) {
							/* Previous aborted allocation had never been started. */
							say("GPI ring ch%u reusing allocated state without a START command\n",ch);
						} else if(initial) {say("GPI stage7 ch%u already state=%u; skipped\n",ch,initial);continue;}
						say("GPI stage7 BEFORE ch%u allocate\n",ch);
						if(initial==0) {wr(base+0x17008,ch); __builtin_arm_dsb(0xf);}
						unsigned waited;
						for(waited=0;waited<2500;waited++) {if((rd(ctx)>>20&15)!=0)break;sleep_us(100);}
						uint32_t allocated=rd(ctx)>>20&15;
						say("GPI stage7 ch%u state=%u config=%#x IRQ=%#x\n",ch,allocated,rd(ctx),rd(base+0x17090));
						if (allocated==1 && step==8) {
							uint64_t low=0;
							if (PE_parse_boot_argn("lowdma",&low,sizeof(low)) && low && !(low&0x3fff) && low<0xfffc0000ULL) {
								uint32_t ring=(uint32_t)low+0x10000+ch*0x4000;
								vm_offset_t memory=ml_io_map_wcomb(ring,0x4000);
								if(memory) {
									/* Device/WC mappings must not use bzero's DC ZVA. */
									for (unsigned word=0;word<0x4000/4;word++)
										((volatile uint32_t *)memory)[word]=0;
									__builtin_arm_dsb(0xf);
									uint32_t nonzero=0;
									for(unsigned word=0;word<0x4000/4;word++) nonzero |= ((volatile uint32_t *)memory)[word];
									say("GPI ring ch%u explicit-store zero readback=%#x\n",ch,nonzero);
									uint32_t original[4]={rd(ctx),rd(ctx+4),rd(ctx+8),rd(ctx+12)};
									wr(ctx,(original[0]&~0xff07c00fu)|(16u<<24)|2u|(ch==0 ? 8u:0u));
									wr(ctx+4,64u*16u);wr(ctx+8,ring);wr(ctx+12,0);__builtin_arm_dsb(0xf);
									say("GPI ring ch%u config=%#x bytes=%u DMA=%#x:%#x; no start/doorbell\n",ch,rd(ctx),rd(ctx+4),rd(ctx+12),rd(ctx+8));
									for(unsigned n=0;n<4;n++)wr(ctx+4*n,original[n]);__builtin_arm_dsb(0xf);
									say("GPI ring ch%u context restored\n",ch);
								}
							}else{say("GPI ring: no usable reserved lowdma window\n");}
						}
						if(allocated==1) {wr(base+0x17008,(10u<<24)|ch);__builtin_arm_dsb(0xf);sleep_ms(1);say("GPI stage7 ch%u deallocation returned\n",ch);}
						wr(base+0x170a0,1u<<ch);__builtin_arm_dsb(0xf);
					}
				}
				if (state==1) {
					wr(base+0x17010,10u<<24); __builtin_arm_dsb(0xf); sleep_ms(1);
					say("GPI stage 6: deallocation write returned\n");
				}
				wr(base+0x170a4,1); __builtin_arm_dsb(0xf);
			}
			for(unsigned i=11;i>0;i--) { wr(base+offset[i-1],saved[i-1]); __builtin_arm_dsb(0xf); }
			say("GPI IRQ setup registers restored\n");
		}
		if (step == 3) {
			/* Vendor initialization allocates the event channel before TX/RX. */
			uint32_t initial = rd(event);
			say("GPI stage 3: event initial=%#x, before allocate command a17010\n", initial);
			if ((initial >> 20 & 0xf) == 0) {
				wr(base + 0x17010, 0); __builtin_arm_dsb(0xf);
				say("GPI stage 3: event allocate MMIO completed\n");
				unsigned waited;
				for (waited=0; waited<2500; waited++) {
					if ((rd(event)>>20 & 0xf) != 0) break;
					sleep_us(100);
				}
				uint32_t observed = rd(event);
				say("GPI stage 3: event config=%#x state=%u after %u us, IRQ=%#x\n",
				    observed, observed>>20&0xf, waited*100, rd(base+0x17094));
				if ((observed>>20&0xf) == 1) {
					wr(base+0x17010, 10u<<24); __builtin_arm_dsb(0xf);
					say("GPI event deallocation submitted; no ring, doorbell or start\n");
				}
			}
		}
		for (unsigned ch = 0; step == 4 && ch < 2; ch++) {
			vm_offset_t ctx = base + 0x14000 + 0x80 * ch;
			uint32_t initial = rd(ctx);
			if ((initial >> 20 & 0xf) != 0) { say("GPI ch%u already active: %#x, skip\n", ch, initial); continue; }
			say("GPI stage 3: before channel %u allocate command\n", ch);
			wr(base + 0x17008, ch); /* ALLOCATE opcode 0 */
			__builtin_arm_dsb(0xf);
			unsigned wait;
			for (wait=0; wait<200; wait++) { if ((rd(ctx)>>20 & 0xf) != 0) break; sleep_us(100); }
			uint32_t after = rd(ctx);
			say("GPI ch%u allocation state=%u config=%#x after %u us, ctrl IRQ=%#x\n", ch,
			    after>>20 & 0xf, after, wait*100, rd(base + 0x17090));
			if ((after>>20 & 0xf) == 1) {
				wr(base + 0x17008, (10u<<24)|ch); /* DEALLOCATE without ever starting */
				__builtin_arm_dsb(0xf); sleep_ms(1);
				say("GPI ch%u deallocation submitted, no transfer started\n", ch);
			}
		}
		wr(clock + 0x52008, previous); __builtin_arm_dsb(0xf);
		say("GPI allocation probe finished, original clock votes restored\n");
	}

	thread_terminate(current_thread());
}

/* Ultra-only read-only probe, verified against xuanyuan's captured Android DT.
 * It deliberately does not apply the Xiaomi 15 clock, pinmux or regulator recipe.
 */
static void
xuanyuan_touch_probe_thread(__unused void *arg, __unused wait_result_t result)
{
	vm_offset_t pins = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	vm_offset_t spi = ml_io_map(0x00a80000UL, 0x4000UL);
	say("xuanyuan read-only probe: SPI 0xa80000, pins 32..35, reset 161, irq 162\n");
	if (!pins || !spi) {
		say("xuanyuan probe: mapping failed\n");
	} else {
		for (unsigned pin = 32; pin <= 35; pin++) {
			say("xuanyuan gpio%u cfg=%#x io=%#x\n", pin,
			    rd(pins + 0x1000UL * pin), rd(pins + 0x1000UL * pin + 4));
		}
		say("xuanyuan reset=%#x irq=%#x; before SPI identification read\n",
		    rd(pins + 0x1000UL * 161 + 4), rd(pins + 0x1000UL * 162 + 4));
		say("xuanyuan SPI fw=%#x hw=%#x; probe complete, no register writes\n",
		    rd(spi + 0x68), rd(spi + 0xe24));
	}
	thread_terminate(current_thread());
}

void
hakios_dada_touch_init(void)
{
	uint32_t enabled = 0, autoswipe = 0;
	thread_t thread;
	uint32_t ultra_probe = 0;
	uint32_t gpi_probe = 0;
	if (PE_parse_boot_argn("xuanyuangpi_probe", &gpi_probe, sizeof(gpi_probe)) && gpi_probe) {
		if (kernel_thread_start(xuanyuan_gpi_probe_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
		return;
	}

	if (PE_parse_boot_argn("xuanyuantouch_probe", &ultra_probe, sizeof(ultra_probe)) && ultra_probe) {
		if (kernel_thread_start(xuanyuan_touch_probe_thread, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
		return;
	}


	if (PE_parse_boot_argn("autoswipe", &autoswipe, sizeof(autoswipe)) && autoswipe &&
	    kernel_thread_start(dada_autoswipe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}

	PE_parse_boot_argn("xuanyuantouch", &ultra_touch, sizeof(ultra_touch));
	if (ultra_touch) {
		pin_miso = 32; pin_mosi = 33; pin_clk = 34; pin_cs = 35;
		max_x = 1440; max_y = 3200;
		enabled = 1;
	}
	if (!enabled && (!PE_parse_boot_argn("dadatouch", &enabled, sizeof(enabled)) || !enabled)) {
		return;
	}
	if (kernel_thread_start(dada_touch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
