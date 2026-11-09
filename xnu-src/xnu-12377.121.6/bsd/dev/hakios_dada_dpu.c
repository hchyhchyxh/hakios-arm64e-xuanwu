/*
 * The Xiaomi 15's (dada) display controller showing iOS's frames from buffers of its own, flipped, instead of the
 * CPU scaling every frame into the boot loader's panel memory (MiniIOMobileFramebuffer.cpp drawLayer: 1206x2622
 * filtered onto 1200x2670 in device memory, 9-10 ms a frame on three processors).
 *
 * Data A Live's way for the Xiaomi 17 Pro (pandora_dpu.c, patches 0215/0217) is the display reading iOS's surfaces
 * where they are, through page tables of its own in the display stream's SMMU bank. Here the same bank (2, apps SMMU,
 * stream 0x800) faults at the first fetch whatever the tables (runs scan6-scan11: kernel memory or the loader's
 * memory below 4 GB, 32- or 48-bit input, walks cached or not): the hypervisor lets the display reach the
 * continuous-splash region alone (DT splash_region 0xfc800000, 43 MiB; smmu_sde_iommu_region_partition), so the bank
 * stays the loader's, untranslating, and the display reads physical addresses in that region only.
 *
 * So: the boot framebuffer is the region's first 12.8 MB, and two buffers of a whole iOS frame follow it. A frame is
 * copied unscaled into the buffer not on screen -- one plain copy of its rows, no filtering -- and the source pipes are
 * pointed at it; the display does the rest: 3 columns cut off each side, 24 rows of border above and below. The
 * flush is latched at the panel's next refresh, so nothing tears.
 *
 * SM8750's MDSS (DT mdss_mdp@ae00000) has a DPU 12.0. Offsets are mainline Linux's (drivers/gpu/drm/msm/disp/dpu1:
 * catalog/dpu_12_0_sm8750.h, dpu_hw_sspp.c, dpu_hw_ctl.c), which count from the "mdp" block at MDSS + 0x1000, and
 * agree with the phone's DT (qcom,sde-sspp-off 0x5000.., qcom,sde-ctl-off 0x16000..). What the loader left (run
 * scan5): CTL0 drives INTF1 (DSI0, command mode refreshing itself), VIG0 and VIG1 each fetch 600 of the panel's 1200
 * columns of the framebuffer at 0xfc800000 (rows of 4800 bytes), source split over LM0 and LM1.
 *
 * Boot-args: dadascan=1 (dada-ios.sh): a look at the display controller (logged, "ddpu:") and the flipped buffers;
 * dadascan=2, the look alone; dadascan=3, the clocks alone.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/systm.h>

#define say(...) redwood_tl("ddpu: " __VA_ARGS__)
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern boolean_t pmap_valid_address(uint64_t pa);
extern void IOSleep(unsigned int milliseconds);

#define MDSS_PHYS               0x0ae00000UL
#define MDSS_SIZE               0x94000UL
#define MDP                     0x1000u
#define CTL(n)                  (MDP + 0x15000u + 0x1000u * (n))
#define CTL_LAYER(n)            (4u * (n))
#define CTL_TOP                 0x014
#define CTL_FLUSH               0x018
#define CTL_START               0x01c
#define CTL_INTF_ACTIVE         0x0f4
#define CTL_FETCH_PIPE_ACTIVE   0x0fc
#define CTL_FLUSH_CTL           (1u << 17)
#define LM(n)                   (MDP + 0x44000u + 0x1000u * (n))
#define INTF(n)                 (MDP + 0x34000u + 0x1000u * (n))
#define SSPP_SRC_SIZE           0x00
#define SSPP_SRC_XY             0x08
#define SSPP_OUT_SIZE           0x0c
#define SSPP_OUT_XY             0x10
#define SSPP_SRC0_ADDR          0x14
#define SSPP_YSTRIDE0           0x24
#define SSPP_SRC_FORMAT         0x30
#define SSPP_UNPACK             0x34
#define SSPP_OP_MODE            0x38
#define SSPP_FETCH_CONFIG       0x48

#define SPLASH_PHYS             0xfc800000UL    /* DT splash_region: what the display may read */
#define SPLASH_SIZE             0x2b00000UL
#define BUFFERS                 2
#define PIPES                   10u

static const struct {
	const char *name;
	uint32_t base;
	uint32_t flush;
} sspp[PIPES] = {
	{ "vig0", 0x4000, 1u << 0 }, { "vig1", 0x6000, 1u << 1 }, { "vig2", 0x8000, 1u << 2 }, { "vig3", 0xa000, 1u << 18 },
	{ "dma0", 0x24000, 1u << 11 }, { "dma1", 0x26000, 1u << 12 }, { "dma2", 0x28000, 1u << 24 },
	{ "dma3", 0x2a000, 1u << 25 }, { "dma4", 0x2c000, 1u << 13 }, { "dma5", 0x2e000, 1u << 14 },
};
static const uint32_t saved_regs[] = { SSPP_SRC_SIZE, SSPP_SRC_XY, SSPP_OUT_SIZE, SSPP_OUT_XY, SSPP_SRC0_ADDR, SSPP_YSTRIDE0 };
#define SAVED (sizeof(saved_regs) / sizeof(saved_regs[0]))

static struct {
	bool ready, on_panel, need_start;
	vm_offset_t mdss;
	uint32_t ctl, pipes, flush;
	uint32_t saved[PIPES][SAVED];
	uint32_t panel_w, panel_h;
	uint32_t buffer_pa[BUFFERS], buffer_bytes;
	uint8_t *buffer[BUFFERS];
	int shown;                      /* the buffer on screen, -1: the boot framebuffer */
	uint32_t stride, w, h;          /* ... and the picture in it */
	uint64_t shows, waits_long;
	bool pending;                   /* a flip kicked and not yet seen taken (dadascan_async) */
} sc;

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

static inline uint32_t
dpu(uint32_t off)
{
	return rd(sc.mdss + off);
}

/* ---- the display block's clocks ------------------------------------------------------------------------------------- */

/*
 * The boot loader leaves the display scanning out but its register clocks may be off: a read of the display
 * controller then never completes and the board resets (runs scan1, scan2: gone 14 s into the boot, no log). So
 * first the clocks its registers need, as Linux's drivers/clk/qcom/dispcc-sm8750.c and gcc-sm8750.c name them:
 * GCC_DISP_AHB_CLK (the clock controller's own registers), then in DISPCC the MDSS power domain must be on and the
 * AHB, MDP and vsync branches are turned on if they are not. False: leave the display controller alone.
 */
#define GCC_DISP_AHB_CBCR       0x127004UL
#define DISPCC_PHYS             0x0af00000UL
#define DISPCC_SIZE             0x10000UL
#define MDSS_GDSCR              0x9000
#define CBCR_OFF                (1u << 31)

static bool
branch_on(vm_offset_t cbcr, const char *name)
{
	uint32_t was = rd(cbcr);

	if (!(was & 1)) {
		wr(cbcr, was | 1);
	}
	for (int i = 0; i < 200 && (rd(cbcr) & CBCR_OFF); i++) {
		delay(5);
	}
	say("%s %08x -> %08x\n", name, was, rd(cbcr));
	/* (under hardware control, bit 1, a branch reads off while nothing asks for it: it is on when something does) */
	return !(rd(cbcr) & CBCR_OFF) || (rd(cbcr) & 2);
}

static bool
clocks(void)
{
	static const struct {
		uint32_t off;
		const char *name;
		bool needed;
	} branches[] = {
		{ 0x80b0, "disp_cc_mdss_ahb_clk", true }, { 0xc004, "disp_cc_mdss_non_gdsc_ahb_clk", false },
		{ 0xa028, "disp_cc_mdss_ahb1_clk", false }, { 0x8010, "disp_cc_mdss_mdp_clk", true },
		{ 0x8030, "disp_cc_mdss_vsync_clk", false },
	};
	vm_offset_t gcc = ml_io_map(GCC_DISP_AHB_CBCR & ~0xfffUL, 0x1000), dispcc;
	uint32_t gdsc;

	if (gcc == 0 || !branch_on(gcc + (GCC_DISP_AHB_CBCR & 0xfff), "gcc_disp_ahb_clk")) {
		return false;
	}
	dispcc = ml_io_map(DISPCC_PHYS, DISPCC_SIZE);
	if (dispcc == 0) {
		return false;
	}
	gdsc = rd(dispcc + MDSS_GDSCR);
	say("mdss_gdsc %08x (%s)\n", gdsc, (gdsc >> 31) ? "on" : "off");
	if (!(gdsc >> 31)) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(branches) / sizeof(branches[0]); i++) {
		if (!branch_on(dispcc + branches[i].off, branches[i].name) && branches[i].needed) {
			return false;
		}
	}
	return true;
}

/* ---- a look at what the boot loader left running ------------------------------------------------------------------ */

static void
look(uint64_t fb)
{
	say("MDSS %08x, DPU %08x; the boot framebuffer at %#llx\n", dpu(0), dpu(MDP), fb);
	for (unsigned n = 0; n < 6; n++) {
		uint32_t c = CTL(n), intf = dpu(c + CTL_INTF_ACTIVE), fetch = dpu(c + CTL_FETCH_PIPE_ACTIVE);

		if (intf == 0 && fetch == 0 && dpu(c + CTL_LAYER(0)) == 0) {
			continue;
		}
		say("ctl%u: layers %08x %08x %08x %08x top %08x flush %08x start %08x intf %08x fetch %08x\n", n,
		    dpu(c + CTL_LAYER(0)), dpu(c + CTL_LAYER(1)), dpu(c + CTL_LAYER(2)), dpu(c + CTL_LAYER(3)), dpu(c + CTL_TOP),
		    dpu(c + CTL_FLUSH), dpu(c + CTL_START), intf, fetch);
	}
	for (unsigned p = 0; p < PIPES; p++) {
		uint32_t b = MDP + sspp[p].base;

		if (dpu(b + SSPP_SRC0_ADDR) == 0 && dpu(b + SSPP_SRC_SIZE) == 0) {
			continue;
		}
		say("%s: src %08x at %08x, out %08x at %08x, addr %08x stride %08x format %08x unpack %08x op %08x fetch %08x\n",
		    sspp[p].name, dpu(b + SSPP_SRC_SIZE), dpu(b + SSPP_SRC_XY), dpu(b + SSPP_OUT_SIZE), dpu(b + SSPP_OUT_XY),
		    dpu(b + SSPP_SRC0_ADDR), dpu(b + SSPP_YSTRIDE0), dpu(b + SSPP_SRC_FORMAT), dpu(b + SSPP_UNPACK),
		    dpu(b + SSPP_OP_MODE), dpu(b + SSPP_FETCH_CONFIG));
	}
	for (unsigned n = 0; n < 4; n++) {
		uint32_t m = LM(n);

		if (dpu(m + 0x0) == 0 && dpu(m + 0x4) == 0) {
			continue;
		}
		say("lm%u: op %08x out %08x border %08x %08x\n", n, dpu(m + 0x0), dpu(m + 0x4), dpu(m + 0x8), dpu(m + 0xc));
	}
	for (unsigned n = 1; n <= 2; n++) {
		uint32_t i = INTF(n);

		uint32_t frame = dpu(i + 0xac), line = dpu(i + 0xb0);

		IOSleep(20);
		say("intf%u: timing engine %08x config %08x frame %u line %u; again in 20 ms: frame %u\n", n, dpu(i + 0x0),
		    dpu(i + 0x4), frame, line, dpu(i + 0xac));
	}
}

/* ---- frames -------------------------------------------------------------------------------------------------------- */

/*
 * Until CTL has taken the last flush (its bits clear at the frame that latches them), LIMIT_US at most. Slept for, a
 * millisecond at a time after a first short look: spinning here cost up to a refresh (8 ms) of a core every frame.
 */
static bool
flush_taken(uint32_t limit_us)
{
	for (uint32_t i = 0; i < 4; i++) {
		if ((dpu(CTL(sc.ctl) + CTL_FLUSH) & sc.flush) == 0) {
			return true;
		}
		delay(50);
	}
	for (uint32_t waited = 200; waited < limit_us; waited += 1000) {
		IOSleep(1);
		if ((dpu(CTL(sc.ctl) + CTL_FLUSH) & sc.flush) == 0) {
			return true;
		}
	}
	return (dpu(CTL(sc.ctl) + CTL_FLUSH) & sc.flush) == 0;
}

static void
kick(void)
{
	__builtin_arm_dsb(0xf);
	wr(sc.mdss + CTL(sc.ctl) + CTL_FLUSH, sc.flush);
	if (sc.need_start) {
		wr(sc.mdss + CTL(sc.ctl) + CTL_START, 1);
	}
}

static void
pipes_to_panel(void)
{
	for (unsigned p = 0; p < PIPES; p++) {
		if (sc.pipes & (1u << p)) {
			for (unsigned r = 0; r < SAVED; r++) {
				wr(sc.mdss + MDP + sspp[p].base + saved_regs[r], sc.saved[p][r]);
			}
		}
	}
	kick();
	sc.on_panel = true;
}

int hakios_dada_scanout_init(uint64_t fb, uint32_t panel_w, uint32_t panel_h, uint32_t row_bytes, uint32_t frame_bytes);
int
hakios_dada_scanout_init(uint64_t fb, uint32_t panel_w, uint32_t panel_h, uint32_t row_bytes, uint32_t frame_bytes)
{
	uint32_t mode = 0, at;

	if (!PE_parse_boot_argn("dadascan", &mode, sizeof(mode)) || mode == 0) {
		return -1;
	}
	if (!clocks()) {
		say("the display controller's clocks are not on: it is left alone, the CPU copies frames\n");
		return -1;
	}
	if (mode == 3) {                /* the clocks alone */
		return -1;
	}
	sc.mdss = ml_io_map(MDSS_PHYS, MDSS_SIZE);
	if (sc.mdss == 0) {
		say("the display controller not mapped\n");
		return -1;
	}
	look(fb);
	if (mode == 2) {
		return -1;
	}
	/* the pipes that read the boot framebuffer, and the CTL that drives the panel's interface */
	for (unsigned p = 0; p < PIPES; p++) {
		uint32_t b = MDP + sspp[p].base, a = dpu(b + SSPP_SRC0_ADDR);

		if (a != 0 && dpu(b + SSPP_SRC_SIZE) != 0 && a >= fb && a < fb + (uint64_t)row_bytes * panel_h) {
			sc.pipes |= 1u << p;
			sc.flush |= sspp[p].flush;
			for (unsigned r = 0; r < SAVED; r++) {
				sc.saved[p][r] = dpu(b + saved_regs[r]);
			}
		}
	}
	if (sc.pipes == 0) {
		say("no pipe reads the boot framebuffer at %#llx: the CPU copies frames\n", fb);
		return -1;
	}
	sc.ctl = ~0u;
	for (unsigned n = 0; n < 6 && sc.ctl == ~0u; n++) {
		if (dpu(CTL(n) + CTL_INTF_ACTIVE) & 0x6) {      /* INTF1 or INTF2: DSI */
			sc.ctl = n;
		}
	}
	if (sc.ctl == ~0u) {
		say("no CTL drives a DSI interface\n");
		return -1;
	}
	sc.flush |= CTL_FLUSH_CTL;
	sc.panel_w = panel_w;
	sc.panel_h = panel_h;

	/* the two buffers: in the splash region after the framebuffer, 1 MiB aligned, memory the kernel does not manage */
	sc.buffer_bytes = (frame_bytes + 0xfffff) & ~0xfffffu;
	at = (uint32_t)((fb + (uint64_t)row_bytes * panel_h + 0xfffff) & ~0xfffffull);
	if (fb < SPLASH_PHYS || at + BUFFERS * sc.buffer_bytes > SPLASH_PHYS + SPLASH_SIZE) {
		say("no room in the splash region for %u buffers of %u bytes after the framebuffer at %#llx\n", BUFFERS,
		    sc.buffer_bytes, fb);
		return -1;
	}
	for (unsigned i = 0; i < BUFFERS; i++) {
		sc.buffer_pa[i] = at + i * sc.buffer_bytes;
		for (uint32_t off = 0; off < sc.buffer_bytes; off += 0x100000) {
			if (pmap_valid_address(sc.buffer_pa[i] + off)) {
				say("%#x is the kernel's memory: no buffers there\n", sc.buffer_pa[i] + off);
				return -1;
			}
		}
		sc.buffer[i] = (uint8_t *)ml_io_map_wcomb(sc.buffer_pa[i], sc.buffer_bytes);
		if (sc.buffer[i] == NULL) {
			say("buffer %u not mapped\n", i);
			return -1;
		}
	}
	/* does a flush alone get taken (video mode, or command mode refreshing itself), or does each frame need a start? */
	kick();
	if (!flush_taken(50000)) {
		sc.need_start = true;
		kick();
		if (!flush_taken(50000)) {
			say("CTL does not take a flush (flush %08x): the CPU copies frames\n", dpu(CTL(sc.ctl) + CTL_FLUSH));
			return -1;
		}
	}
	say("ctl%u drives pipes %#x (flush %#x%s); buffers of %u bytes at %#x and %#x\n", sc.ctl, sc.pipes, sc.flush,
	    sc.need_start ? ", a start each" : "", sc.buffer_bytes, sc.buffer_pa[0], sc.buffer_pa[1]);
	sc.shown = -1;
	sc.on_panel = true;
	sc.ready = true;
	return 0;
}

/* The buffer to copy the next frame into (the one not on screen), BYTES at most; NULL: none. */
uint8_t *hakios_dada_scanout_buffer(uint32_t bytes);
uint8_t *
hakios_dada_scanout_buffer(uint32_t bytes)
{
	if (!sc.ready || bytes > sc.buffer_bytes) {
		return NULL;
	}
	/* the buffer given is the one the flip before last left: the last flip must be taken before it is written again */
	if (sc.pending) {
		if (!flush_taken(40000)) {
			sc.waits_long++;
		}
		sc.pending = false;
	}
	return sc.buffer[sc.shown == 0 ? 1 : 0];
}

/*
 * Show the W x H picture (rows of STRIDE bytes) just copied into the buffer hakios_dada_scanout_buffer gave: each
 * pipe keeps its place on the panel and reads the same columns of the picture, which is centred on the panel unscaled.
 * Returns once the frame is taken (the buffer before is no longer read); false if it was not within 40 ms.
 */
static TUNABLE(uint32_t, scan_async, "dadascan_async", 1);
bool hakios_dada_scanout_show(uint32_t stride, uint32_t w, uint32_t h);
bool
hakios_dada_scanout_show(uint32_t stride, uint32_t w, uint32_t h)
{
	int32_t cx = ((int32_t)w - (int32_t)sc.panel_w) / 2;
	uint32_t sy = h > sc.panel_h ? (h - sc.panel_h) / 2 : 0, rows = MIN(h, sc.panel_h);
	uint32_t dy = h < sc.panel_h ? (sc.panel_h - h) / 2 : 0;
	int next = sc.shown == 0 ? 1 : 0;

	if (!sc.ready || cx < 0 || !flush_taken(20000)) {
		return false;
	}
	__builtin_arm_dsb(0xf);         /* (the copy's writes out of the write buffers before the display reads them) */
	for (unsigned p = 0; p < PIPES; p++) {
		vm_offset_t b = sc.mdss + MDP + sspp[p].base;
		uint32_t sx, sw, ox, oy;

		if (!(sc.pipes & (1u << p))) {
			continue;
		}
		sx = (sc.saved[p][1] & 0xffff) + (uint32_t)cx;
		sw = sc.saved[p][0] & 0xffff;
		ox = sc.saved[p][3] & 0xffff;
		oy = (sc.saved[p][3] >> 16) + dy;
		wr(b + SSPP_SRC0_ADDR, sc.buffer_pa[next]);
		wr(b + SSPP_YSTRIDE0, stride & 0xffff);
		wr(b + SSPP_SRC_SIZE, rows << 16 | sw);
		wr(b + SSPP_SRC_XY, sy << 16 | sx);
		wr(b + SSPP_OUT_SIZE, rows << 16 | sw);
		wr(b + SSPP_OUT_XY, oy << 16 | ox);
	}
	kick();
	sc.on_panel = false;
	sc.shown = next;
	sc.stride = stride;
	sc.w = w;
	sc.h = h;
	sc.shows++;
	/*
	 * dadascan_async=1 (the default): back at once, and the flip is seen taken only before the buffer it leaves is written
	 * again (hakios_dada_scanout_buffer). Waiting here for the next refresh to take it cost some 4 ms of every frame, on the
	 * window server's swap (dada g2k40s: 7.5-9 ms a frame in the display's driver, 10-30 frames a second).
	 */
	if (scan_async) {
		sc.pending = true;
	} else if (!flush_taken(40000)) {
		sc.waits_long++;
	}
	if (sc.shows % 1024 == 1) {
		say("%llu frames flipped (%llu not taken within 40 ms)\n", sc.shows, sc.waits_long);
	}
	return true;
}

/* The boot framebuffer again, as the loader set the pipes up (for the CPU's own drawing). */
void hakios_dada_scanout_panel(void);
void
hakios_dada_scanout_panel(void)
{
	if (sc.ready && !sc.on_panel && flush_taken(20000)) {
		pipes_to_panel();
		sc.shown = -1;
		(void)flush_taken(40000);
	}
}

/*
 * The run is about to end (redwood_adreno_stop): the picture on screen copied into the boot framebuffer as the pipes
 * show it, and the pipes on the framebuffer again -- for the loader's screenshot, and the next kernel finds the display
 * as the loader left it. FB: the framebuffer as the kernel maps it (device memory: aligned 32-bit stores only).
 */
void hakios_dada_scanout_end(uint8_t *fb, uint32_t row_bytes);
void
hakios_dada_scanout_end(uint8_t *fb, uint32_t row_bytes)
{
	if (!sc.ready || sc.on_panel || sc.shown < 0) {
		return;
	}
	{
		int32_t cx = ((int32_t)sc.w - (int32_t)sc.panel_w) / 2;
		uint32_t rows = MIN(sc.h, sc.panel_h), dy = sc.h < sc.panel_h ? (sc.panel_h - sc.h) / 2 : 0;
		uint32_t sy = sc.h > sc.panel_h ? (sc.h - sc.panel_h) / 2 : 0;

		for (uint32_t y = 0; y < sc.panel_h; y++) {
			volatile uint32_t *to = (volatile uint32_t *)(fb + (size_t)y * row_bytes);
			const uint32_t *from = (const uint32_t *)(sc.buffer[sc.shown] + (size_t)(y - dy + sy) * sc.stride) + cx;

			for (uint32_t x = 0; x < sc.panel_w; x++) {
				to[x] = (y >= dy && y < dy + rows) ? from[x] : 0;
			}
		}
	}
	hakios_dada_scanout_panel();
	say("the run ends: the picture is in the framebuffer again, the pipes on it (%llu frames flipped)\n", sc.shows);
}
