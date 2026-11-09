/*
 * The GPU of the Xiaomi 15 (dada, SM8750): an Adreno 830 (chip id 0x44050001, "gen8"), brought up the
 * way Linux's msm driver brings up its a8xx GPUs (drivers/gpu/drm/msm/adreno/a8xx_gpu.c, a6xx_gmu.c,
 * a6xx_hfi.c), which is the only public description of this generation. Only with boot-arg dadagpu=N,
 * which runs phases 1..N and stops at the first that fails; each says what it is about to touch
 * ("dgpu: " lines in the loader's log), so that a phase that hangs the bus is the last line there:
 *   1  power: GCC's votes and clocks, the CX power switch, the GPU's PLL (the GMU's clock comes from
 *      it), the clocks of the GMU, its bus and the SMMU; the GMU's crystal counter seen counting
 *   2  the GMU's memory: page tables of ours and a context of the GPU's SMMU for the GMU's stream (5);
 *      the GMU's firmware (fw/gen80000_gmu.bin) into its own memories and into ours
 *   3  the GMU started (a6xx_gmu_fw_start for a8xx), its message queues started ("HFI")
 *   4  the GMU's tables (speeds and supply levels, bus votes), "start", and the request for the 3D
 *      core's power ("OOB GPU"); the 3D core's first registers read
 * Everything is undone again before the system goes on.
 *
 * Register offsets are the database's (Linux's registers/adreno/*.xml), turned into
 * hakios_gen8_regs.h by tools/gen8-regs.py: 32-bit words from the GPU's base, 0x3d00000, the GMU's
 * included (Linux: GMU_BYTE_OFFSET subtracts the GMU's distance from the GPU). Addresses and levels
 * are dada's own device tree (logs/dada.dts), clocks Linux's gcc-sm8750.c and gpucc-sm8750.c.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>
#include <vm/vm_kern_xnu.h>

#include "hakios_gen8_regs.h"

extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern const uint8_t *redwood_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);

#define say(...) redwood_tl("dgpu: " __VA_ARGS__)

static uint32_t phase;                  /* the one being performed */

/* ---- registers ------------------------------------------------------------------------------------ */

/*
 * One plain load or store a register: some of what is touched (the SMMU's global registers) may be
 * the hypervisor's imitation, which copes only with the simplest instruction forms.
 */
static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
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

/* A moment for the loader, which follows our log from another core, to note what was just said. */
static void
hold(void)
{
	sleep_ms(30);
}

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x80000UL
#define GCC_APCS_CLK_VOTE       0x52000         /* bit 15 GPLL0 to the GPU, bit 16 its divided tap */
#define GCC_GPU_CFG_AHB_CBCR    0x71004
#define GCC_GPU_GEMNOC_GFX_CBCR 0x71010
#define GCC_DDRSS_GPU_AXI_CBCR  0x71150

#define GPUCC_PHYS              0x03d90000UL
#define GPUCC_SIZE              0xa000UL
#define PLL0                    0x0000          /* taycan_elu layout (clk-alpha-pll.c) */
#define PLL_MODE                0x00
#define PLL_OPMODE              0x04
#define PLL_L                   0x10
#define PLL_ALPHA               0x14
#define PLL_USER_CTL            0x18
#define PLL_USER_CTL_U          0x1c
#define PLL_CONFIG_CTL          0x20
#define PLL_CONFIG_CTL_U        0x24
#define PLL_CONFIG_CTL_U1       0x28
#define PLL_TEST_CTL            0x2c
#define PLL_TEST_CTL_U          0x30
#define PLL_OUTCTRL             (1u << 0)
#define PLL_RESET_N             (1u << 2)
#define PLL_LOCK_DET            (1u << 31)
#define CX_GDSCR                0x9080
#define GMU_RCGR                0x9318          /* gpu_cc_gmu_clk_src: +0 command, +4 source and divider */
#define HUB_RCGR                0x93ec          /* gpu_cc_hub_clk_src */
#define RCG_UPDATE              (1u << 0)
#define RCG_CFG_MASK            0x71fu          /* [4:0] 2*divider-1, [10:8] source */
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)
#define GDSC_SW_COLLAPSE        (1u << 0)
#define GDSC_RETAIN_FF_ENABLE   (1u << 11)
#define GDSC_POWER_ON           (1u << 31)

#define GPU_PHYS                0x03d00000UL
#define GPU_SIZE                0xa0000UL       /* the 3D core, the GMU, its RPMh channel, the GX clock controller, cx_misc */
#define RSCC_OFFSET             0x50000UL       /* a6xx_gmu.c: on a8xx the RSCC is the GPU's base + 0x50000 */
#define CX_MISC_OFFSET          0x9e000UL
#define GX_GDSCR_OFFSET         0x68024UL       /* gxclkctl (0x3d64000) + 0x4024 */

#define SMMU_PHYS               0x03da0000UL
#define SMMU_SIZE               0x40000UL

#define CMD_DB_PHYS             0x81c60000UL    /* dt: aop_cmd_db_region */
#define CMD_DB_SIZE             0x20000UL

static vm_offset_t gcc, gpucc, gpu, smmu;

static uint32_t
G(uint32_t reg)                         /* a register of the GPU block, by its word number */
{
	return rd(gpu + 4UL * reg);
}

static void
Gw(uint32_t reg, uint32_t value)
{
	wr(gpu + 4UL * reg, value);
}

static uint32_t
R(uint32_t reg)                         /* the GMU's RPMh channel */
{
	return rd(gpu + RSCC_OFFSET + 4UL * reg);
}

static uint32_t
X(uint32_t reg)                         /* cx_misc */
{
	return rd(gpu + CX_MISC_OFFSET + 4UL * reg);
}

static bool
wait_bits(vm_offset_t address, uint32_t mask, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if ((rd(address) & mask) == value) {
			return true;
		}
		sleep_us(100);
	}
	return (rd(address) & mask) == value;
}

static bool
clock_on(vm_offset_t cbcr, const char *name, bool must)
{
	wr(cbcr, rd(cbcr) | CBCR_ENABLE);
	if (wait_bits(cbcr, CBCR_OFF, 0, 2)) {
		return true;
	}
	say("%u: %s stays off (%08x)%s\n", phase, name, rd(cbcr), must ? "" : ", not relied on");
	return !must;
}

static bool
rcg_set(uint32_t rcgr, uint32_t cfg, const char *name)
{
	uint32_t was = rd(gpucc + rcgr + 4);

	wr(gpucc + rcgr + 4, (was & ~RCG_CFG_MASK) | cfg);
	wr(gpucc + rcgr, rd(gpucc + rcgr) | RCG_UPDATE);
	if (!wait_bits(gpucc + rcgr, RCG_UPDATE, 0, 5)) {
		say("%u: %s does not take its source (cmd %08x cfg %08x)\n", phase, name, rd(gpucc + rcgr), rd(gpucc + rcgr + 4));
		return false;
	}
	return true;
}

static bool
cx_on(void)
{
	return (rd(gpucc + CX_GDSCR) & GDSC_POWER_ON) != 0;
}

/* ---- phase 1: power and clocks ---------------------------------------------------------------------- */

static bool cx_off_done, gmu_runs, gmu_space_on, core_powered;

/*
 * The GPU's PLL as Linux configures and enables a Taycan ELU PLL (clk_lucid_evo_pll_configure,
 * alpha_pll_lucid_evo_enable) with gpucc-sm8750.c's values: 0x34 + 0x1555/65536 times 19.2 MHz,
 * about 998 MHz; its "even" output, half that, is what the GMU's clock takes for 500 MHz.
 */
static bool
pll0_on(void)
{
	vm_offset_t p = gpucc + PLL0;

	if ((rd(p + PLL_OPMODE) & 1) && (rd(p + PLL_MODE) & PLL_OUTCTRL)) {
		say("%u: PLL0 runs already (mode %08x)\n", phase, rd(p + PLL_MODE));
		return true;
	}
	wr(p + PLL_L, 0x34 | 0x44u << 16);
	wr(p + PLL_ALPHA, 0x1555);
	wr(p + PLL_CONFIG_CTL, 0x19660387);
	wr(p + PLL_CONFIG_CTL_U, 0x098060a0);
	wr(p + PLL_CONFIG_CTL_U1, 0xb416cb20);
	wr(p + PLL_USER_CTL, 0x00000400);
	wr(p + PLL_USER_CTL_U, 0x00000002);
	wr(p + PLL_TEST_CTL, 0);
	wr(p + PLL_TEST_CTL_U, 0);
	wr(p + PLL_MODE, rd(p + PLL_MODE) & ~PLL_OUTCTRL);
	wr(p + PLL_OPMODE, 0);                  /* standby */
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_RESET_N);
	wr(p + PLL_OPMODE, 1);                  /* run */
	if (!wait_bits(p + PLL_MODE, PLL_LOCK_DET, PLL_LOCK_DET, 10)) {
		say("%u: PLL0 does not lock (mode %08x)\n", phase, rd(p + PLL_MODE));
		return false;
	}
	wr(p + PLL_USER_CTL, rd(p + PLL_USER_CTL) | 7);         /* main, even, odd outputs */
	wr(p + PLL_MODE, rd(p + PLL_MODE) | PLL_OUTCTRL);
	say("%u: PLL0 locked: mode %08x L %08x user %08x\n", phase, rd(p + PLL_MODE), rd(p + PLL_L), rd(p + PLL_USER_CTL));
	return true;
}

static bool
phase_power(void)
{
	uint32_t before, after;
	uint64_t t0, nanos;

	/*
	 * boot-arg dadagpu_skip=MASK, for finding what of this phase upsets the Wi-Fi chip's PCIe link (runs drv*, 28
	 * October): 1 GCC's votes and clocks, 2 the CX power switch, 4 PLL0, 8 the GMU and hub clock sources, 16 gpucc's
	 * clocks. A skipped step reports done; later phases are not for a skipped one.
	 */
	uint32_t skip = 0;

	(void)PE_parse_boot_argn("dadagpu_skip", &skip, sizeof(skip));
	if (skip != 0) {
		say("1: skipping %#x of this phase\n", skip);
	}
	say("1: GCC: GPLL0 votes, the GPU's bus clocks\n");
	hold();
	/* (gpu_cfg_ahb reads "off", 0x88000003, under hardware control, and gpucc answers all the same: not checked) */
	if (!(skip & 1)) {
		wr(gcc + GCC_APCS_CLK_VOTE, rd(gcc + GCC_APCS_CLK_VOTE) | 1u << 15 | 1u << 16);
		clock_on(gcc + GCC_GPU_GEMNOC_GFX_CBCR, "gcc gpu_gemnoc_gfx", false);
		clock_on(gcc + GCC_DDRSS_GPU_AXI_CBCR, "gcc ddrss_gpu_axi", false);
	}

	say("1: CX power switch (%08x)\n", rd(gpucc + CX_GDSCR));
	hold();
	if (skip & 2) {
		return true;
	}
	if (!cx_on()) {
		wr(gpucc + CX_GDSCR, (rd(gpucc + CX_GDSCR) | GDSC_RETAIN_FF_ENABLE) & ~GDSC_SW_COLLAPSE);
		wait_bits(gpucc + CX_GDSCR, GDSC_POWER_ON, GDSC_POWER_ON, 10);
	}
	if (!cx_on()) {
		say("1: CX does not come on (%08x)\n", rd(gpucc + CX_GDSCR));
		return false;
	}
	cx_off_done = false;
	if (rd(gpu + GX_GDSCR_OFFSET) & GDSC_POWER_ON) {
		say("1: THE 3D CORE HAS POWER ALREADY (gx %08x): an earlier kernel left it; not starting over it\n",
		    rd(gpu + GX_GDSCR_OFFSET));
		return false;
	}

	say("1: PLL0 (for the GMU's 500 MHz)\n");
	hold();
	if (skip & 4) {
		return true;
	}
	if (!pll0_on()) {
		return false;
	}
	say("1: GMU clock from PLL0's even tap, hub clock 200 MHz from GPLL0\n");
	hold();
	if (skip & 8) {
		return true;
	}
	if (!rcg_set(GMU_RCGR, 2u << 8 | 1, "gmu_clk_src") || !rcg_set(HUB_RCGR, 5u << 8 | 5, "hub_clk_src")) {
		return false;
	}
	say("1: gpucc clocks on\n");
	hold();
	if (skip & 16) {
		return true;
	}
	if (!clock_on(gpucc + 0x90bc, "ahb", false) || !clock_on(gpucc + 0x90e4, "cxo", true) ||
	    !clock_on(gpucc + 0x93e8, "hub_aon", true) || !clock_on(gpucc + 0x90e8, "hub_cx_int", true) ||
	    !clock_on(gpucc + 0x90d4, "cx_gmu", true) || !clock_on(gpucc + 0x90f4, "memnoc_gfx", false) ||
	    !clock_on(gpucc + 0x7000, "smmu vote", false)) {
		return false;
	}
	t0 = mach_absolute_time();
	before = G(G8_GMU_ALWAYS_ON_COUNTER_L);
	sleep_ms(10);
	after = G(G8_GMU_ALWAYS_ON_COUNTER_L);
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &nanos);
	say("1: GMU crystal counter: %u Hz; sysreset %08x, fw version word %08x\n",
	    nanos ? (uint32_t)((uint64_t)(after - before) * NSEC_PER_SEC / nanos) : 0, G(G8_GMU_CM3_SYSRESET),
	    G(G8_GMU_CORE_FW_VERSION));
	return true;
}

/* ---- memory the GPU block reaches through its SMMU ---------------------------------------------------- */

/*
 * As in redwood_adreno.c (where this was found out on the Adreno 642L): three levels of 4K pages,
 * 39 bits of address, one SMMU context per address space, a stream table entry sending a stream
 * through it. dada's device tree: streams 0 (the 3D core), 1 (its LPAC), 2 (secure), 5 (the GMU),
 * all with mask 0.
 */
#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_UNPRIVILEGED        (1ull << 6)
#define PTE_READ_ONLY           (1ull << 7)
#define PTE_SHARED              (2ull << 8)
#define PTE_ACCESSED            (1ull << 10)
#define PTE_NOT_GLOBAL          (1ull << 11)
#define PTE_ADDRESS             0x0000fffffffff000ull
#define PTE_GMU                 (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)
#define PTE_CORE                (PTE_GMU | PTE_UNPRIVILEGED)

#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))               /* in the second page */
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))       /* in the second page */
#define SMMU_S2CR_FAULT         (2u << 16)
#define SMMU_SMR_VALID          (1u << 31)
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe

static const uint32_t smmu_page = 0x1000, smmu_pages = 32;      /* idr1 0x40000015: 4K pages, 32 before the contexts */

static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

/*
 * The tables of a GPU address space (4 KB granule, 39 bits, three levels). 8192 tables: the last level's map 2 MB each, so the
 * space reaches 16 GB (DADA_ADDRESSES_SIZE; it was 2048 tables and 3.5 GB, one space for every client, which the system's
 * Metal clients filled: posters and widgets were refused buffers of 4 MB). A table is found from the address an entry holds
 * by a hash (it was a walk of every table, twice for each 4 KB page mapped), and only the tables written since the last commit
 * are pushed out of the cache to the SMMU (it was every table, up to 8 MB, after each buffer mapped or unmapped).
 */
#define SPACE_TABLES            8192
#define SPACE_HASH              16384           /* a power of two, twice SPACE_TABLES */

struct gpu_space {
	const char *name;
	uint32_t context, control, table_count;
	uint64_t *root;
	struct {
		uint64_t phys;
		uint64_t *table;
	} tables[SPACE_TABLES];
	uint16_t hash[SPACE_HASH];              /* by physical page: the table's index + 1, 0 none */
	uint16_t dirty[SPACE_TABLES];           /* the indexes written since the last commit */
	uint32_t dirty_count;
	uint8_t is_dirty[SPACE_TABLES];
};

static void
space_dirty(struct gpu_space *space, uint32_t index)
{
	if (!space->is_dirty[index]) {
		space->is_dirty[index] = 1;
		space->dirty[space->dirty_count++] = (uint16_t)index;
	}
}

/* the index of the table at `phys`, or -1 */
static int
space_find(struct gpu_space *space, uint64_t phys)
{
	for (uint32_t i = (uint32_t)(phys >> 12) & (SPACE_HASH - 1), n = 0; n < SPACE_HASH; i = (i + 1) & (SPACE_HASH - 1), n++) {
		uint32_t at = space->hash[i];

		if (at == 0) {
			return -1;
		}
		if (space->tables[at - 1].phys == phys) {
			return (int)(at - 1);
		}
	}
	return -1;
}

/* a new zeroed table, -1 if none is left */
static int
space_new_table(struct gpu_space *space)
{
	static vm_offset_t spare;
	static unsigned spare_left;
	uint64_t *table;
	uint32_t index;

	if (space->table_count == SPACE_TABLES) {
		return -1;
	}
	if (spare_left == 0) {
		if (kmem_alloc(kernel_map, &spare, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
		    KERN_SUCCESS) {
			return -1;
		}
		spare_left = PAGE_SIZE / 4096;
	}
	table = (uint64_t *)(spare + PAGE_SIZE - 4096 * spare_left--);
	index = space->table_count++;
	space->tables[index].phys = kvtophys((vm_offset_t)table);
	space->tables[index].table = table;
	for (uint32_t i = (uint32_t)(space->tables[index].phys >> 12) & (SPACE_HASH - 1);; i = (i + 1) & (SPACE_HASH - 1)) {
		if (space->hash[i] == 0) {
			space->hash[i] = (uint16_t)(index + 1);
			break;
		}
	}
	space_dirty(space, index);
	return (int)index;
}

/* the table `entry` of table `parent` points to, made if there is none; -1 if none can be */
static int
space_next(struct gpu_space *space, uint32_t parent, uint64_t *entry)
{
	int index;

	if (*entry & 1) {
		return space_find(space, *entry & PTE_ADDRESS);
	}
	index = space_new_table(space);
	if (index >= 0) {
		*entry = space->tables[index].phys | PTE_TABLE;
		space_dirty(space, parent);
	}
	return index;
}

/* the last-level table for `address` (made if `make`), its index in *at; NULL if there is none */
static uint64_t *
space_leaf(struct gpu_space *space, uint64_t address, bool make, uint32_t *at)
{
	int level2, level3;

	if (address >> 39) {
		return NULL;
	}
	if (space->root == NULL) {
		if (!make || space_new_table(space) != 0) {     /* the root is table 0 (space_on reads tables[0]) */
			return NULL;
		}
		space->root = space->tables[0].table;
	}
	if (!make && !(space->root[address >> 30 & 0x1ff] & 1)) {
		return NULL;
	}
	level2 = space_next(space, 0, &space->root[address >> 30 & 0x1ff]);
	if (level2 < 0 || (!make && !(space->tables[level2].table[address >> 21 & 0x1ff] & 1))) {
		return NULL;
	}
	level3 = space_next(space, (uint32_t)level2, &space->tables[level2].table[address >> 21 & 0x1ff]);
	if (level3 < 0) {
		return NULL;
	}
	*at = (uint32_t)level3;
	return space->tables[level3].table;
}

static bool
space_map(struct gpu_space *space, uint64_t address, uint64_t phys, uint64_t attributes)
{
	uint32_t at;
	uint64_t *level3 = space_leaf(space, address, true, &at);

	if (level3 == NULL) {
		return false;
	}
	level3[address >> 12 & 0x1ff] = phys | attributes | PTE_PAGE;
	space_dirty(space, at);
	return true;
}

static void
space_commit(struct gpu_space *space)
{
	for (uint32_t i = 0; i < space->dirty_count; i++) {
		to_memory((vm_offset_t)space->tables[space->dirty[i]].table, 4096);
		space->is_dirty[space->dirty[i]] = 0;
	}
	space->dirty_count = 0;
}

struct gpu_block {
	uint64_t address;               /* the GPU's */
	uint32_t size;
	vm_offset_t memory;             /* ours */
};

static bool
block_new(struct gpu_space *space, struct gpu_block *block, uint64_t address, uint32_t size, uint64_t attributes)
{
	block->address = address;
	block->size = (size + 4095) & ~4095u;
	if (kmem_alloc(kernel_map, &block->memory, round_page(block->size), KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("%u: no memory for %#x bytes\n", phase, block->size);
		return false;
	}
	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (!space_map(space, address + offset, kvtophys(block->memory + offset), attributes)) {
			say("%u: no page table for %#llx\n", phase, address + offset);
			return false;
		}
	}
	to_memory(block->memory, block->size);
	return true;
}

/* A view without this processor's cache, for memory both sides write a word at a time: blocks in one piece. */
static vm_offset_t
block_uncached(const struct gpu_block *block)
{
	uint64_t phys = kvtophys(block->memory);

	for (uint32_t offset = 0; offset < block->size; offset += 4096) {
		if (kvtophys(block->memory + offset) != phys + offset) {
			return 0;
		}
	}
	return ml_io_map_wcomb(phys, block->size);
}

static uint32_t
smmu_context(const struct gpu_space *space)
{
	return (smmu_pages + space->context) * smmu_page;
}

static bool
space_faulted(const struct gpu_space *space)
{
	uint32_t context = smmu_context(space), status = rd(smmu + context + SMMU_CB_FSR);

	if ((status & SMMU_FSR_ALL) == 0) {
		return false;
	}
	say("%u: THE SMMU REFUSED %s: fsr %08x far %08x%08x fsynr0 %08x\n", phase, space->name, status,
	    rd(smmu + context + SMMU_CB_FAR + 4), rd(smmu + context + SMMU_CB_FAR), rd(smmu + context + SMMU_CB_FSYNR0));
	return true;
}

static bool
space_on(struct gpu_space *space, uint32_t match, uint32_t stream)
{
	uint32_t context = smmu_context(space);
	uint64_t root = space->tables[0].phys | (uint64_t)space->context << 48;
	uint32_t smr = SMMU_SMR_VALID | stream;

	space_commit(space);
	wr(smmu + context + SMMU_CB_SCTLR, 0);
	wr(smmu + smmu_page + SMMU_CBA2R(space->context), 1);           /* 64-bit */
	wr(smmu + smmu_page + SMMU_CBAR(space->context), SMMU_CBAR_STAGE_1);
	wr(smmu + context + SMMU_CB_TCR2, SMMU_TCR2);
	wr(smmu + context + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	wr(smmu + context + SMMU_CB_TTBR0, (uint32_t)root);
	wr(smmu + context + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	wr(smmu + context + SMMU_CB_TTBR1, 0);
	wr(smmu + context + SMMU_CB_TTBR1 + 4, 0);
	wr(smmu + context + SMMU_CB_MAIR0, SMMU_MAIR0);
	wr(smmu + context + SMMU_CB_MAIR1, 0);
	wr(smmu + context + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(smmu + context + SMMU_CB_TLBIASID, space->context);
	wr(smmu + context + SMMU_CB_TLBSYNC, 1);
	if (!wait_bits(smmu + context + SMMU_CB_TLBSTATUS, 1, 0, 100)) {
		say("%u: the SMMU does not finish a TLB sync\n", phase);
		return false;
	}
	wr(smmu + context + SMMU_CB_SCTLR, SMMU_SCTLR_ON | space->control);
	wr(smmu + SMMU_S2CR(match), space->context);
	wr(smmu + SMMU_SMR(match), smr);
	if (rd(smmu + SMMU_SMR(match)) != smr || (rd(smmu + SMMU_S2CR(match)) & 0x300ff) != space->context ||
	    !(rd(smmu + context + SMMU_CB_SCTLR) & 1) || rd(smmu + context + SMMU_CB_TTBR0) != (uint32_t)root) {
		say("%u: the SMMU did not take it: smr %08x s2cr %08x cbar %08x sctlr %08x ttbr0 %08x\n", phase,
		    rd(smmu + SMMU_SMR(match)), rd(smmu + SMMU_S2CR(match)), rd(smmu + smmu_page + SMMU_CBAR(space->context)),
		    rd(smmu + context + SMMU_CB_SCTLR), rd(smmu + context + SMMU_CB_TTBR0));
		return false;
	}
	say("%u: stream %u goes through context %u (%s), %u tables\n", phase, stream, space->context, space->name,
	    space->table_count);
	return true;
}

static void
space_off(struct gpu_space *space, uint32_t match)
{
	wr(smmu + SMMU_SMR(match), 0);
	wr(smmu + SMMU_S2CR(match), SMMU_S2CR_FAULT);
	wr(smmu + smmu_context(space) + SMMU_CB_SCTLR, 0);
}

/* ---- phase 2: the GMU's memory and firmware ----------------------------------------------------------- */

/*
 * Linux's a6xx_gmu_memory_probe for a8xx: "dummy" 16K at 0x60000000 (the firmware asks for its two
 * pages there), "debug" 28K at 0x60400000, the instruction cache 16M - 16K at 0x4000 (the firmware's
 * code blocks and the memory it asks for at 0x100000, 0x200000, 0x300000 lie in it), and a log and
 * the message table wherever: here after the dummy.
 */
#define GMU_STREAM              5
#define GMU_MATCH               1
#define GMU_ITCM_ADDRESS        0x00000000u
#define GMU_DTCM_ADDRESS        0x10004000u
#define GMU_TCM_SIZE            0x4000u
#define GMU_ICACHE_ADDRESS      0x00004000u
#define GMU_ICACHE_SIZE         (0x1000000u - 0x4000u)
#define GMU_DUMMY_ADDRESS       0x60000000u
#define GMU_DUMMY_SIZE          0x4000u
#define GMU_LOG_ADDRESS         0x60004000u
#define GMU_LOG_SIZE            0x4000u
#define GMU_HFI_ADDRESS         0x60008000u
#define GMU_HFI_SIZE            0x4000u
#define GMU_DEBUG_ADDRESS       0x60400000u
#define GMU_DEBUG_SIZE          0x7000u

static struct gpu_space gmu_space = { .name = "the GMU", .context = 1 };
static struct gpu_block gmu_icache, gmu_dummy, gmu_log, gmu_hfi, gmu_debug;
static const uint8_t *gmu_firmware;
static uint64_t gmu_firmware_size;

struct fw_block {
	uint32_t address, size, type, value;
};

static const uint8_t *
image_file(const char *name, uint64_t *size)
{
	const uint8_t *data = redwood_image_file("fw", name, size, NULL);

	if (data == NULL) {
		say("%u: no fw/%s in the small image (fw-gpu-dada/ and userland/build.sh)\n", phase, name);
	}
	return data;
}

static bool
in_block(const struct gpu_block *b, uint32_t address, uint32_t size)
{
	return address >= b->address && address + size <= b->address + b->size;
}

static bool
phase_gmu_firmware(void)
{
	struct gpu_block *blocks[] = { &gmu_icache, &gmu_dummy, &gmu_debug };
	uint32_t wrong = 0, words = 0, in_memory = 0;

	if (!cx_on()) {
		return false;
	}
	gmu_firmware = image_file("gen80000_gmu.bin", &gmu_firmware_size);
	if (gmu_firmware == NULL) {
		return false;
	}
	say("2: memory for the GMU, its page tables\n");
	if (!block_new(&gmu_space, &gmu_icache, GMU_ICACHE_ADDRESS, GMU_ICACHE_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_dummy, GMU_DUMMY_ADDRESS, GMU_DUMMY_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_log, GMU_LOG_ADDRESS, GMU_LOG_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_hfi, GMU_HFI_ADDRESS, GMU_HFI_SIZE, PTE_GMU) ||
	    !block_new(&gmu_space, &gmu_debug, GMU_DEBUG_ADDRESS, GMU_DEBUG_SIZE, PTE_GMU)) {
		return false;
	}

	say("2: the GMU's processor held in reset; its firmware, %llu bytes, into its memories\n", gmu_firmware_size);
	hold();
	Gw(G8_GMU_CM3_SYSRESET, 1);
	for (int pass = 0; pass < 2; pass++) {
		for (uint64_t at = 0; at + sizeof(struct fw_block) <= gmu_firmware_size;) {
			struct fw_block block;
			const uint8_t *data = gmu_firmware + at + sizeof(block);

			memcpy(&block, gmu_firmware + at, sizeof(block));
			at += sizeof(block) + block.size;
			if (at > gmu_firmware_size) {
				say("2: the file ends inside a block\n");
				return false;
			}
			if (block.size == 0) {
				continue;
			}
			if (block.address - GMU_ITCM_ADDRESS < GMU_TCM_SIZE || block.address - GMU_DTCM_ADDRESS < GMU_TCM_SIZE) {
				uint32_t reg = block.address < GMU_DTCM_ADDRESS ?
				    G8_GMU_CM3_ITCM_START + (block.address - GMU_ITCM_ADDRESS) / 4 :
				    G8_GMU_CM3_DTCM_START + (block.address - GMU_DTCM_ADDRESS) / 4;

				for (uint32_t offset = 0; offset + 4 <= block.size; offset += 4) {
					uint32_t word;

					memcpy(&word, data + offset, 4);
					if (pass == 0) {
						Gw(reg + offset / 4, word);
						words++;
					} else if (G(reg + offset / 4) != word) {
						wrong++;
					}
				}
				continue;
			}
			if (pass == 1) {
				continue;
			}
			for (unsigned i = 0; i <= sizeof(blocks) / sizeof(blocks[0]); i++) {
				if (i == sizeof(blocks) / sizeof(blocks[0])) {
					say("2: a block for %#x (%#x bytes) has no memory\n", block.address, block.size);
					return false;
				}
				if (in_block(blocks[i], block.address, block.size)) {
					memcpy((void *)(blocks[i]->memory + (block.address - blocks[i]->address)), data, block.size);
					in_memory += block.size;
					break;
				}
			}
		}
	}
	to_memory(gmu_icache.memory, gmu_icache.size);
	to_memory(gmu_dummy.memory, gmu_dummy.size);
	to_memory(gmu_debug.memory, gmu_debug.size);
	say("2: %u words through the GMU's registers (%u read back wrong), %u bytes in memory; version word %08x\n", words,
	    wrong, in_memory, G(G8_GMU_CORE_FW_VERSION));
	if (wrong) {
		return false;
	}

	say("2: SMMU context %u for the GMU's stream %u (the first SMMU writes)\n", gmu_space.context, GMU_STREAM);
	hold();
	gmu_space_on = true;
	return space_on(&gmu_space, GMU_MATCH, GMU_STREAM) && !space_faulted(&gmu_space);
}

/* ---- phase 3: the GMU started, its message queues ----------------------------------------------------- */

struct hfi_queue_header {
	uint32_t status, iova, type, size, msg_size, dropped, rx_watermark, tx_watermark, rx_request, tx_request, read_index,
	    write_index;
};
struct hfi_table_header {
	uint32_t version, size, qhdr0_offset, qhdr_size, num_queues, active_queues;
};
#define HFI_QUEUES              3
#define HFI_QUEUE_WORDS         1024

static volatile uint8_t *hfi;           /* the uncached view of the HFI block */

static volatile struct hfi_queue_header *
hfi_header(unsigned n)
{
	return (volatile struct hfi_queue_header *)(hfi + sizeof(struct hfi_table_header)) + n;
}

static volatile uint32_t *
hfi_data(unsigned n)
{
	return (volatile uint32_t *)(hfi + 4096 * (n + 1));
}

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_INTERRUPTED         1

static int32_t
scm(uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			*answer = x1;
			return (int32_t)x0;
		}
	}
	return -1;
}

static void
gmu_log_tail(void)
{
	const volatile uint32_t *log = (const volatile uint32_t *)gmu_log.memory;
	uint32_t next = G(G8_GMU_GENERAL_9) & 0xffff;

	to_memory(gmu_log.memory, gmu_log.size);
	say("%u: GMU: init %08x hfi %08x g2h %08x ao %08x pwr %08x rpmh %08x log at %u\n", phase,
	    G(G8_GMU_CM3_FW_INIT_RESULT), G(G8_GMU_HFI_CTRL_STATUS), G(G8_GMU_GMU2HOST_INTR_INFO),
	    G(G8_GMU_AO_HOST_INTERRUPT_STATUS), G(G8_GMU_PWR_CLK_STATUS), G(G8_GPU_GMU_CX_GMU_RPMH_POWER_STATE), next);
	for (uint32_t i = next > 16 ? next - 16 : 0; i < next && i + 2 <= GMU_LOG_SIZE / 16; i += 2) {
		say("%u:   log %x %x %x %x | %x %x %x %x\n", phase, log[4 * i], log[4 * i + 1], log[4 * i + 2], log[4 * i + 3],
		    log[4 * i + 4], log[4 * i + 5], log[4 * i + 6], log[4 * i + 7]);
	}
	space_faulted(&gmu_space);
}

static bool
phase_gmu_start(void)
{
	uint32_t version, mask, started, result = 0;
	uint64_t answer = ~0ull;
	int32_t r;
	volatile struct hfi_table_header *table;

	if (!gmu_space_on) {
		return false;
	}
	hfi = (volatile uint8_t *)block_uncached(&gmu_hfi);
	if (hfi == NULL) {
		say("3: the HFI block is not in one piece\n");
		return false;
	}
	table = (volatile struct hfi_table_header *)hfi;
	table->version = 0;
	table->size = sizeof(struct hfi_table_header) + HFI_QUEUES * sizeof(struct hfi_queue_header);
	table->qhdr0_offset = sizeof(struct hfi_table_header) / 4;
	table->qhdr_size = sizeof(struct hfi_queue_header) / 4;
	table->num_queues = table->active_queues = HFI_QUEUES;
	for (unsigned i = 0; i < HFI_QUEUES; i++) {
		volatile struct hfi_queue_header *h = hfi_header(i);

		h->iova = GMU_HFI_ADDRESS + 4096 * (i + 1);
		h->type = 10u << 8 | i;
		h->status = 1;
		h->size = HFI_QUEUE_WORDS;
		h->msg_size = h->dropped = 0;
		h->rx_watermark = h->tx_watermark = h->rx_request = 1;
		h->tx_request = 0;
		h->read_index = h->write_index = 0;
	}
	__asm__ volatile ("dsb sy");

	/* a6xx_gmu_secure_init: TrustZone sets the GPU's software fuses (a750 and a8xx) */
	say("3: TrustZone: gpu_init_regs (always-on, tsense)\n");
	hold();
	r = scm(SCM_64 | SCM_SIP | SCM_CALL(0x28, 1), 1, 1u << 0 | 1u << 3, 0, 0, &answer);
	say("3: TrustZone returns %d (%#llx); sw fuse value %08x\n", r, answer, X(G8_CX_MISC_CX_MISC_SW_FUSE_VALUE));

	say("3: the GMU's registers before its start (a6xx_gmu_fw_start, a8xx)\n");
	hold();
	Gw(G8_GMU_AO_HOST_INTERRUPT_CLR, 0xffffffff);
	Gw(G8_GMU_AO_HOST_INTERRUPT_MASK, 0xffffffff);
	Gw(G8_GMU_GMU2HOST_INTR_CLR, 0xffffffff);
	Gw(G8_GMU_GMU2HOST_INTR_MASK, 0xffffffff);
	Gw(G8_GPU_GMU_CX_GMU_CX_FALNEXT_INTF, 1);
	Gw(G8_GPU_GMU_CX_GMU_CX_FAL_INTF, 1);
	if (R(G8_GPU_RSCC_RSC_STATUS0_DRV0) & (1u << 16)) {     /* its RPMh channel asleep: wake it (a6xx_rpmh_start) */
		Gw(G8_GMU_RSCC_CONTROL_REQ, 2);
		wait_bits(gpu + 4UL * G8_GMU_RSCC_CONTROL_ACK, 2, 2, 10);
		wait_bits(gpu + RSCC_OFFSET + 4UL * G8_RSCC_SEQ_BUSY_DRV0, ~0u, 0, 10);
		Gw(G8_GMU_RSCC_CONTROL_REQ, 0);
	}
	say("3: rscc status0 %08x seq busy %08x\n", R(G8_GPU_RSCC_RSC_STATUS0_DRV0), R(G8_RSCC_SEQ_BUSY_DRV0));
	Gw(G8_GMU_CM3_FW_INIT_RESULT, 0);
	Gw(G8_GMU_CM3_BOOT_CONFIG, 2);
	Gw(G8_GMU_HFI_QTBL_ADDR, GMU_HFI_ADDRESS);
	Gw(G8_GMU_HFI_QTBL_INFO, 1);
	Gw(G8_GMU_AHB_FENCE_RANGE_0, 1u << 31 | 0x32u << 18 | 0x8c0);
	Gw(G8_GMU_CM3_CFG, 0x4052);
	Gw(G8_GMU_GENERAL_10, 0x8000100);       /* a830's gmu_chipid */
	Gw(G8_GMU_GENERAL_8, (GMU_LOG_ADDRESS & 0xfffff000u) | (GMU_LOG_SIZE / 4096 - 1));
	for (unsigned i = 0; i < 4; i++) {
		Gw(G8_GBIF_QSB_SIDE0 + i, 0x00071e20);
	}
	Gw(G8_GBIF_CX_CONFIG, 0x20023000);
	Gw(G8_GMU_MRC_GBIF_QOS_CTRL, 0x33);
	Gw(G8_GMU_SYS_BUS_CONFIG, 1);
	Gw(G8_GMU_ICACHE_CONFIG, 1);
	Gw(G8_GMU_DCACHE_CONFIG, 1);

	version = G(G8_GMU_CORE_FW_VERSION);
	if (version <= 0x20010004) {
		mask = 0xffffffff;
		started = 0xbabeface;
	} else {
		mask = 0x1ff;
		started = 0x100;
	}
	say("3: the GMU's processor out of reset (firmware %08x)\n", version);
	hold();
	Gw(G8_GMU_CM3_SYSRESET, 1);
	Gw(G8_GMU_GENERAL_9, 0);
	gmu_runs = true;
	Gw(G8_GMU_CM3_SYSRESET, 0);
	for (int i = 0; i < 1000 && (result & mask) != started; i++) {
		sleep_us(100);
		result = G(G8_GMU_CM3_FW_INIT_RESULT);
	}
	if ((result & mask) != started) {
		say("3: the GMU's firmware does not come up (init result %08x)\n", result);
		gmu_log_tail();
		return false;
	}
	say("3: THE GMU'S FIRMWARE RUNS (init result %08x)\n", result);

	Gw(G8_GMU_HFI_CTRL_INIT, 1);
	if (!wait_bits(gpu + 4UL * G8_GMU_HFI_CTRL_STATUS, 1, 1, 1000)) {
		say("3: the GMU does not start its message queues\n");
		gmu_log_tail();
		return false;
	}
	say("3: THE GMU'S MESSAGE QUEUES ARE UP (hfi status %08x)\n", G(G8_GMU_HFI_CTRL_STATUS));
	return !space_faulted(&gmu_space);
}

/* ---- phase 4: the GMU's tables, and the 3D core's power ----------------------------------------------- */

#define HFI_MSG_BW_TABLE        3
#define HFI_MSG_START           10
#define HFI_MSG_CORE_FW_START   14
#define HFI_MSG_TABLE           15
#define HFI_MSG_GX_BW_PERF_VOTE 30
#define HFI_MSG_PREPARE_SLUMBER 33
#define HFI_F2H_ERROR           100
#define HFI_TABLE_GPU_PERF      1

static uint32_t hfi_seqnum;

/* everything waiting in a queue of the GMU's; true if the answer to `seqnum` was among it */
static bool
hfi_receive(unsigned which, uint32_t seqnum, uint32_t *error)
{
	volatile struct hfi_queue_header *h = hfi_header(which);
	volatile uint32_t *data = hfi_data(which);
	bool answered = false;

	while (h->read_index != h->write_index) {
		uint32_t message[8] = { 0 }, at = h->read_index % HFI_QUEUE_WORDS, words = data[at] >> 8 & 0xff;

		for (uint32_t i = 0; i < words; i++) {
			if (i < 8) {
				message[i] = data[at];
			}
			at = (at + 1) % HFI_QUEUE_WORDS;
		}
		h->read_index = (at + 3) / 4 * 4 % HFI_QUEUE_WORDS;
		if ((message[0] >> 16 & 0xf) == 1 && (message[1] >> 20) == seqnum) {
			*error = message[2];
			answered = true;
		} else if ((message[0] & 0xff) == HFI_F2H_ERROR) {
			say("%u: THE GMU REPORTS AN ERROR: %08x %08x %08x %08x\n", phase, message[1], message[2], message[3],
			    message[4]);
		} else {
			say("%u: the GMU says (queue %u, %u words): %08x %08x %08x %08x\n", phase, which, words, message[0],
			    message[1], message[2], message[3]);
		}
		if (words == 0) {
			break;
		}
	}
	return answered;
}

static bool hfi_quiet;                  /* the governor's speed votes, many a second: not each said */

static bool
hfi_send(const char *what, uint32_t id, uint32_t *message, uint32_t words)
{
	volatile struct hfi_queue_header *h = hfi_header(0);
	volatile uint32_t *data = hfi_data(0);
	uint32_t at = h->write_index % HFI_QUEUE_WORDS, seqnum = ++hfi_seqnum % 0xfff, error = 0;
	bool answered = false;

	message[0] = seqnum << 20 | words << 8 | id;
	if (!hfi_quiet) {
		say("%u: to the GMU: %s (message %u, %u words)\n", phase, what, id, words);
	}
	for (uint32_t i = 0; i < (words + 3) / 4 * 4; i++) {
		data[at] = i < words ? message[i] : 0xfafafafa;
		at = (at + 1) % HFI_QUEUE_WORDS;
	}
	__asm__ volatile ("dsb sy");
	h->write_index = at;
	__asm__ volatile ("dsb sy");
	Gw(G8_GMU_HOST2GMU_INTR_SET, 1);
	for (int i = 0; i < 10000 && !answered; i++) {
		sleep_us(100);
		if (G(G8_GMU_GMU2HOST_INTR_INFO) & 1) {
			Gw(G8_GMU_GMU2HOST_INTR_CLR, 1);
			answered = hfi_receive(1, seqnum, &error);
		}
	}
	hfi_receive(2, ~0u, &error);
	if (!answered || error != 0) {
		say("%u: the GMU %s (%#x) to %s\n", phase, answered ? "says no" : "does not answer", error, what);
		gmu_log_tail();
		return false;
	}
	return true;
}

/* dt: aop_cmd_db_region: the boot loader's list of what RPMh can switch (drivers/soc/qcom/cmd-db.c) */
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

static const struct cmd_db_header *cmd_db;

/*
 * cmd_db_read_addr and cmd_db_read_aux_data in one: the resource's address (0 if cmd-db lacks it), and where its
 * auxiliary data lies and how long it is (rsc_offset: the slave's data_offset plus the entry's offset). For a rail
 * (gfx.lvl and the like) that data is its list of levels; for a bus clock manager (SH0, MC0, ACV) it is
 * struct bcm_db {le32 unit, le16 width, u8 vcd, u8 reserved}, which a6xx_gmu_rpmh_bw_votes_init turns into votes.
 */
static uint32_t
rpmh_entry(const char *name, const uint8_t **aux, unsigned *aux_length)
{
	char id[8] = { 0 };

	*aux = NULL;
	*aux_length = 0;
	if (cmd_db == NULL) {
		cmd_db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	if (cmd_db->magic[0] != 0xdb || cmd_db->magic[1] != 0x30 || cmd_db->magic[2] != 0x03 || cmd_db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &cmd_db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(cmd_db->data + slave->header_offset);

		if (slave->id == 0 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) != 0) {
				continue;
			}
			*aux = cmd_db->data + slave->data_offset + entry[i].offset;
			*aux_length = entry[i].length;
			return entry[i].address;
		}
	}
	return 0;
}

static uint32_t
rpmh_resource(const char *name, uint16_t *levels, unsigned *level_count)
{
	const uint8_t *data;
	unsigned length;
	uint32_t address = rpmh_entry(name, &data, &length);

	if (level_count != NULL) {
		*level_count = 0;
	}
	for (unsigned l = 0; address != 0 && levels != NULL && l < length / 2u && l < 16; l++) {
		levels[l] = (uint16_t)(data[2 * l] | data[2 * l + 1] << 8);
		if (l > 0 && levels[l] == 0) {
			break;
		}
		*level_count = l + 1;
	}
	return address;
}

/* a6xx_gmu_rpmh_arc_index: the first level at least `level` (or the highest) */
static unsigned
arc_index(const uint16_t *list, unsigned count, uint32_t level)
{
	for (unsigned i = 0; i < count; i++) {
		if (list[i] >= level) {
			return i;
		}
	}
	return count ? count - 1 : 0;
}

/* a6xx_gmu_rpmh_arc_votes_init: the rail's level, its index, and the index of the secondary rail's level */
static uint32_t
arc_vote(uint32_t level, const uint16_t *pri, unsigned pri_count, const uint16_t *sec, unsigned sec_count)
{
	unsigned p = arc_index(pri, pri_count, level), s = arc_index(sec, sec_count, pri[p]);

	return (uint32_t)pri[p] << 16 | s << 8 | p;
}

/*
 * dada's speeds of its 3D core: all of dt qcom,gpu-pwrlevels-0 (speed bin 0xbe, the lowest bin: every bin has
 * these, the faster ones add 967 MHz and up), 160 to 900 MHz with their RPMh levels and their qcom,bus-freq (an
 * index in qcom,bus-table-ddr, below: the memory bandwidth KGSL asks for along with that speed); and the GMU's (dt
 * qcom,gmu-freq-table: 500 MHz at 64, 650 MHz at 128), plus "off". (Until 27 October only the four lowest were
 * in the table and the GPU ran at 160 MHz: every submission took 2 ms of GPU and the home screen crawled.)
 *
 * Room for 16: a6xx_hfi_msg_perf_table's gx_votes[16], and KGSL's own limit of 16 pwrlevels (a8xx_hfi_send_perf_table
 * builds its message to fit, but the GMU firmware is KGSL's). A faster bin's levels are appended by gpu_speeds_choose,
 * so the list stays in ascending order and every index the governor (hakios_dada_cpu.c) knows keeps its speed.
 */
struct gpu_speed {
	uint32_t hz, level, bus;
};
#define GPU_SPEEDS_MAX          16
static struct gpu_speed gpu_speeds[GPU_SPEEDS_MAX] = { { 0, 0, 0 }, { 160000000, 0x32, 2 }, { 222000000, 0x34, 2 },
	                                               { 342000000, 0x38, 3 }, { 389000000, 0x3c, 4 }, { 443000000, 0x40, 4 },
	                                               { 525000000, 0x50, 4 }, { 607000000, 0x80, 6 }, { 660000000, 0x90, 6 },
	                                               { 734000000, 0xc0, 8 }, { 832000000, 0xe0, 10 }, { 900000000, 0x100, 11 } };
#define GPU_SPEEDS_BASE         12              /* bin 0xbe's eleven and "off" */
static uint32_t gpu_speed_count = GPU_SPEEDS_BASE;
#define GPU_SPEEDS              gpu_speed_count
/* the speed the 3D core runs at: boot-arg dadagpu_speed=N, an index in gpu_speeds (default 9: 734 MHz) */
static TUNABLE(uint32_t, gpu_speed_index, "dadagpu_speed", 9);
static const struct {
	uint32_t hz, level;
} gmu_speeds[] = { { 0, 0 }, { 500000000, 0x40 }, { 650000000, 0x80 } };
#define GMU_SPEEDS              (sizeof(gmu_speeds) / sizeof(gmu_speeds[0]))

/*
 * The speed bin: dt nvmem-cells "speed_bin" -> qfprom@221c8000's gpu_speed_bin@138 (reg <0x138 2>, bits <0 9>), which
 * adreno_read_speedbin reads and the catalog's ADRENO_SPEEDBINS for chip 0x44050001 knows as 190 (0xbe), 221 (0xdd),
 * 232 (0xe8), 242 and 252; this device tree has pwrlevels for the first three only. The faster bins' extra levels
 * (qcom,gpu-pwrlevels-1, bin 0xdd: to 1050 MHz; -2, bin 0xe8: to 1100 MHz) all vote the top DDR level, and in both
 * bins 900 MHz votes bus level 10 where bin 0xbe has 11. (Bin 0xe8 also lacks 160 MHz; it stays here, so the
 * governor's indexes keep their speeds.) Boot-arg dadagpu_bin=1 appends them when the fuse says so; the default 0 only
 * says what the fuse would allow. dadagpu_fuse=0 does not even read it.
 */
#define QFPROM_PHYS             0x221c8000UL    /* dt qfprom@221c8000, read-only: the fuses' corrected copy */
#define QFPROM_SIZE             0x1000UL
#define GPU_SPEED_BIN_FUSE      0x138UL
static const struct {
	uint32_t fuse, max_mhz, extra, bus_900;
	struct gpu_speed speed[3];
} gpu_bins[] = {
	{ 0xbe, 900, 0, 11, { { 0, 0, 0 } } },
	{ 0xdd, 1050, 2, 10, { { 967000000, 0x140, 11 }, { 1050000000, 0x1a0, 11 } } },
	{ 0xe8, 1100, 3, 10, { { 967000000, 0x140, 11 }, { 1050000000, 0x1a0, 11 }, { 1100000000, 0x1c0, 11 } } },
};
static TUNABLE(uint32_t, gpu_bin_on, "dadagpu_bin", 0);
/*
 * (default 0 since 29 October: reading qfprom stopped the whole SoC some 28 s into boot -- the hypervisor's, as for the
 * display and the Wi-Fi's stream -- g2k36b/g2k46. The bin is given instead: dadagpu_bin=0xe8, what Android's kgsl
 * reports for this phone -- gpu_available_frequencies to 1100 MHz.)
 */
static TUNABLE(uint32_t, gpu_fuse_on, "dadagpu_fuse", 0);

/*
 * Reads the fuse and, with dadagpu_bin=1, grows gpu_speeds by the bin's faster levels. Called from
 * hakios_dada_gpu_init and from hakios_dada_gpu_speeds (the governor may ask first: hakios_dada_cpu_init starts it
 * just before the GPU's init): it writes the same values each time, so two at once do no harm.
 */
static void
gpu_speeds_choose(void)
{
	static vm_offset_t qfprom;
	static bool told;
	uint16_t gfx[16];
	unsigned gfx_n = 0, b = 0, count = GPU_SPEEDS_BASE;
	uint32_t fuse;

	if (gpu_bin_on > 1) {
		fuse = gpu_bin_on & 0x1ff;      /* dadagpu_bin=BIN: that bin, the fuse not read */
	} else if (!gpu_fuse_on) {
		if (!told) {
			say("speed bin: the fuse not read (dadagpu_fuse=0): bin 0xbe's levels, to 900 MHz\n");
		}
		told = true;
		return;
	}
	if (gpu_bin_on <= 1) {
		if (qfprom == 0) {
			qfprom = ml_io_map(QFPROM_PHYS, QFPROM_SIZE);
		}
		/* qfprom_reg_read: byte by byte; nvmem's bits <0 9>: the low nine bits of the little-endian pair */
		fuse = (*(volatile uint8_t *)(qfprom + GPU_SPEED_BIN_FUSE) |
		    (uint32_t)*(volatile uint8_t *)(qfprom + GPU_SPEED_BIN_FUSE + 1) << 8) & 0x1ff;
	}
	while (b < sizeof(gpu_bins) / sizeof(gpu_bins[0]) && gpu_bins[b].fuse != fuse) {
		b++;
	}
	if (b == sizeof(gpu_bins) / sizeof(gpu_bins[0])) {
		if (!told) {
			say("speed bin: the fuse says %#x (%u), not a bin of the device tree's: bin 0xbe's levels, to 900 MHz\n",
			    fuse, fuse);
		}
		told = true;
		return;
	}
	if (gpu_bin_on && gpu_bins[b].extra != 0) {
		/* a6xx_gmu_rpmh_arc_votes_init gives up on a level gfx.lvl lacks; here such a level is left out */
		(void)rpmh_resource("gfx.lvl", gfx, &gfx_n);
		for (unsigned i = 0; i < gpu_bins[b].extra && count < GPU_SPEEDS_MAX; i++) {
			if (gfx_n == 0 || gpu_bins[b].speed[i].level > gfx[gfx_n - 1]) {
				if (!told) {
					say("speed bin: RPMh's gfx rail lacks level %#x: %u MHz left out\n", gpu_bins[b].speed[i].level,
					    gpu_bins[b].speed[i].hz / 1000000);
				}
				break;
			}
			gpu_speeds[count++] = gpu_bins[b].speed[i];
		}
		if (count > GPU_SPEEDS_BASE) {
			gpu_speeds[GPU_SPEEDS_BASE - 1].bus = gpu_bins[b].bus_900;
		}
		gpu_speed_count = count;
	}
	if (!told) {
		say("speed bin: the fuse says %#x: bin %#x allows %u MHz; %s, to %u MHz (%u speeds)\n", fuse, gpu_bins[b].fuse,
		    gpu_bins[b].max_mhz, count > GPU_SPEEDS_BASE ? "its levels used (dadagpu_bin=1)" :
		    gpu_bins[b].extra != 0 ? "bin 0xbe's levels used (dadagpu_bin=0)" : "its levels used",
		    gpu_speeds[count - 1].hz / 1000000, count);
	}
	told = true;
}

/*
 * The GMU's bus table (struct a6xx_hfi_msg_bw_table, the same for gen8 in KGSL's hfi_bwtable_cmd): header,
 * bw_level_num, cnoc_cmds_num, ddr_cmds_num, cnoc_wait_bitmask, ddr_wait_bitmask, cnoc_cmds_addrs[6],
 * cnoc_cmds_data[2][6], ddr_cmds_addrs[8], ddr_cmds_data[16][8]: 160 words.
 */
#define BW_LEVELS_MAX           16
#define BW_CNOC_ADDRS           6
#define BW_CNOC_DATA            12              /* [level][cmd] at 12 + 6 level + cmd */
#define BW_DDR_ADDRS            24
#define BW_DDR_DATA             32              /* [level][cmd] at 32 + 8 level + cmd */
#define BW_WORDS                (BW_DDR_DATA + 8 * BW_LEVELS_MAX)
/* include/soc/qcom/tcs.h: commit (30), valid (29), vote x (27:14), vote y (13:0) */
#define BCM_TCS_CMD(commit, valid, x, y) \
	((uint32_t)(commit) << 30 | (uint32_t)(valid) << 29 | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))

/* dt qcom,bus-table-ddr: the memory bandwidths a pwrlevel's qcom,bus-freq points at, in KB/s; 0 is "off" */
static const uint32_t ddr_kbps[] = { 0, 781250, 2136718, 5285156, 6074218, 6671875, 8171875, 10687500, 12449218,
	                             14398437, 16500000, 18597656 };
_Static_assert(sizeof(ddr_kbps) / sizeof(ddr_kbps[0]) <= BW_LEVELS_MAX, "the GMU's bus table has 16 levels");

/*
 * a8xx_catalog.c, chip 0x44050001: the bus clock managers the GMU votes on for the 3D core's memory traffic. SH0 (the
 * shared hub) and MC0 (the memory controller) by bandwidth over their bus width; ACV a fixed vote, with its perfmode
 * bit from 10687500 KB/s (bus level 7) up.
 */
static const struct {
	const char *name;
	uint32_t buswidth;
	bool fixed;
	uint32_t perfmode, perfmode_kbps;
} gpu_bcms[] = { { "SH0", 16, false, 0, 0 }, { "MC0", 4, false, 0, 0 }, { "ACV", 0, true, 1u << 2, 10687500 } };
#define GPU_BCMS                (sizeof(gpu_bcms) / sizeof(gpu_bcms[0]))

/* boot-arg dadagpu_bw=0: the single "off" level of before, so the GMU never votes memory bandwidth (A/B) */
static TUNABLE(uint32_t, gpu_bw_on, "dadagpu_bw", 1);
static uint32_t bus_levels = 1;                 /* the levels the GMU's bus table has; 1: every bw index is 0 */

/* the bw index a GX_BW_PERF_VOTE carries with gpu_speeds[speed]: its pwrlevel's qcom,bus-freq, if the table has it */
static uint32_t
gpu_bus_index(uint32_t speed)
{
	return speed < GPU_SPEEDS && gpu_speeds[speed].bus < bus_levels ? gpu_speeds[speed].bus : 0;
}

/*
 * a6xx_hfi_send_bw_table's message: a6xx_gmu_rpmh_bw_votes_init's votes for each of qcom,bus-table-ddr's levels, laid
 * out by a6xx_generate_bw_table. Each BCM's vote is the bandwidth times cmd-db's width for it over the catalog's bus
 * width, in bytes a second over cmd-db's unit; x and y alike on a7xx and a8xx; commit on the last BCM of each virtual
 * clock domain; "off" (bandwidth 0) valid nowhere. CN0, the configuration bus, as before: off, or on with 1.
 * Without the BCMs' data in cmd-db, or with dadagpu_bw=0, the single "off" level of before (a740_build_bw_table).
 */
static bool
bus_table_fill(uint32_t *bw)
{
	const uint8_t *aux[GPU_BCMS];
	unsigned aux_length[GPU_BCMS], levels = gpu_bw_on ? sizeof(ddr_kbps) / sizeof(ddr_kbps[0]) : 1;
	uint32_t unit[GPU_BCMS] = { 0 }, width[GPU_BCMS] = { 0 }, vcd[GPU_BCMS] = { 0 };

	bzero(bw, BW_WORDS * 4);
	bw[2] = 1;                              /* cnoc commands */
	bw[3] = GPU_BCMS;                       /* ddr commands */
	bw[4] = 1;                              /* cnoc wait mask: CN0 commits */
	bw[BW_CNOC_ADDRS] = rpmh_resource("CN0", NULL, NULL);
	bw[BW_CNOC_DATA] = BCM_TCS_CMD(1, 0, 0, 0);
	bw[BW_CNOC_DATA + 6] = BCM_TCS_CMD(1, 1, 0, 1);
	for (unsigned j = 0; j < GPU_BCMS; j++) {
		bw[BW_DDR_ADDRS + j] = rpmh_entry(gpu_bcms[j].name, &aux[j], &aux_length[j]);
		if (bw[BW_DDR_ADDRS + j] != 0 && aux_length[j] >= 8) {
			unit[j] = aux[j][0] | aux[j][1] << 8 | aux[j][2] << 16 | (uint32_t)aux[j][3] << 24;
			width[j] = aux[j][4] | aux[j][5] << 8;
			vcd[j] = aux[j][6];
		}
		if (aux_length[j] < 8 || (!gpu_bcms[j].fixed && unit[j] == 0)) {
			levels = 1;
		}
	}
	say("4: bus: CN0 %#x SH0 %#x MC0 %#x ACV %#x; cmd-db unit/width/vcd: SH0 %u/%u/%u MC0 %u/%u/%u ACV %u/%u/%u\n",
	    bw[BW_CNOC_ADDRS], bw[BW_DDR_ADDRS], bw[BW_DDR_ADDRS + 1], bw[BW_DDR_ADDRS + 2], unit[0], width[0], vcd[0],
	    unit[1], width[1], vcd[1], unit[2], width[2], vcd[2]);
	if (!bw[BW_CNOC_ADDRS] || !bw[BW_DDR_ADDRS] || !bw[BW_DDR_ADDRS + 1] || !bw[BW_DDR_ADDRS + 2]) {
		return false;
	}

	if (levels == 1) {
		bw[1] = 1;
		bw[5] = 7;                      /* ddr wait mask */
		for (unsigned j = 0; j < GPU_BCMS; j++) {
			bw[BW_DDR_DATA + j] = BCM_TCS_CMD(1, 0, 0, 0);
		}
		bus_levels = 1;
		say("4: bus table: the one \"off\" level (%s)\n", gpu_bw_on ? "cmd-db lacks the BCMs' data" : "dadagpu_bw=0");
		return true;
	}

	for (unsigned i = 0; i < levels; i++) {
		uint32_t *data = &bw[BW_DDR_DATA + 8 * i];

		for (unsigned j = 0; j < GPU_BCMS; j++) {
			bool commit = j == GPU_BCMS - 1 || vcd[j] != vcd[j + 1];
			uint64_t peak;
			uint32_t vote;

			if (ddr_kbps[i] == 0) {
				data[j] = BCM_TCS_CMD(commit, 0, 0, 0);
			} else if (gpu_bcms[j].fixed) {
				/* a7xx and a8xx vote perfmode only from perfmode_bw up (a6xx on every valid level) */
				data[j] = BCM_TCS_CMD(commit, 1, 0,
				    gpu_bcms[j].perfmode_kbps && ddr_kbps[i] >= gpu_bcms[j].perfmode_kbps ? gpu_bcms[j].perfmode : 0);
			} else {
				peak = (uint64_t)ddr_kbps[i] * width[j] / gpu_bcms[j].buswidth;
				peak = peak * 1000 / unit[j];   /* KB/s to bytes a second, then to the BCM's unit */
				vote = (uint32_t)(peak < 1 ? 1 : peak > 0x3fff ? 0x3fff : peak);
				data[j] = BCM_TCS_CMD(commit, 1, vote, vote);
			}
		}
		say("4: bus level %2u: %8u KB/s: SH0 %08x MC0 %08x ACV %08x\n", i, ddr_kbps[i], data[0], data[1], data[2]);
	}
	/* a6xx_generate_bw_table: wait for each BCM whose "off" command commits */
	bw[5] = 0;
	for (unsigned j = 0; j < GPU_BCMS; j++) {
		if (bw[BW_DDR_DATA + j] & BCM_TCS_CMD(1, 0, 0, 0)) {
			bw[5] |= 1u << j;
		}
	}
	bw[1] = levels;
	bus_levels = levels;
	say("4: bus table: %u levels, ddr wait mask %#x\n", levels, bw[5]);
	return true;
}

static bool
phase_core_power(void)
{
	static uint32_t perf[3 + 2 + 3 * GPU_SPEEDS_MAX + 2 + 2 * GMU_SPEEDS];
	static uint32_t bw[BW_WORDS];
	uint32_t empty[2] = { 0, 0 }, vote[4] = { 0 }, granted = 0, at = 0;
	uint16_t gfx[16], mx[16], cx[16], gmxc[16];
	unsigned gfx_n, mx_n, cx_n, gmxc_n;

	if (!gmu_runs) {
		return false;
	}
	if (!rpmh_resource("gfx.lvl", gfx, &gfx_n) || !rpmh_resource("mx.lvl", mx, &mx_n) ||
	    !rpmh_resource("cx.lvl", cx, &cx_n)) {
		say("4: RPMh's list lacks a rail\n");
		return false;
	}
	if (!rpmh_resource("gmxc.lvl", gmxc, &gmxc_n)) {
		memcpy(gmxc, mx, sizeof(mx));
		gmxc_n = mx_n;
	}
	say("4: rails: gfx %u levels (top %u), mx %u, cx %u, gmxc %u\n", gfx_n, gfx_n ? gfx[gfx_n - 1] : 0, mx_n, cx_n, gmxc_n);

	/* a8xx_hfi_send_perf_table: a "table" message of type GPU_PERF with two entries */
	perf[at++] = 0;                         /* header */
	perf[at++] = 0;                         /* version */
	perf[at++] = HFI_TABLE_GPU_PERF;
	perf[at++] = GPU_SPEEDS;
	perf[at++] = 3;
	for (unsigned i = 0; i < GPU_SPEEDS; i++) {
		perf[at++] = arc_vote(gpu_speeds[i].level, gfx, gfx_n, gmxc, gmxc_n);
		perf[at++] = i == 0 ? 0xffffffff : (0x3fffu << 14 | arc_index(mx, mx_n, gpu_speeds[i].level) << 8 | 0xff);
		perf[at++] = gpu_speeds[i].hz / 1000;
	}
	perf[at++] = GMU_SPEEDS;
	perf[at++] = 2;
	for (unsigned i = 0; i < GMU_SPEEDS; i++) {
		perf[at++] = arc_vote(gmu_speeds[i].level, cx, cx_n, mx, mx_n);
		perf[at++] = gmu_speeds[i].hz / 1000;
	}
	say("4: votes: gpu %08x %08x %08x, gmu %08x %08x\n", perf[5], perf[8], perf[11], perf[5 + 3 * GPU_SPEEDS + 2],
	    perf[5 + 3 * GPU_SPEEDS + 4]);

	/*
	 * The bus table: one level per entry of qcom,bus-table-ddr, SH0, MC0, ACV for memory, CN0 for the
	 * configuration bus (bus_table_fill). Until 29 October it had the single "off" level, as Linux sends where it
	 * has no bandwidth list (a6xx_build_bw_table), so the 3D core never asked for memory bandwidth: only the static
	 * boot vote (dadaddr, hakios_dada_cpu.c) kept DDR up.
	 */
	if (!bus_table_fill(bw)) {
		return false;
	}

	if (!hfi_send("the table of speeds", HFI_MSG_TABLE, perf, at) ||
	    !hfi_send("the table of bus votes", HFI_MSG_BW_TABLE, bw, BW_WORDS) ||
	    !hfi_send("core firmware start", HFI_MSG_CORE_FW_START, empty, 2) ||
	    !hfi_send("start", HFI_MSG_START, empty, 1)) {
		return false;
	}
	say("4: THE GMU HAS ITS TABLES\n");
	/*
	 * a6xx_gmu_set_initial_freq -> a6xx_hfi_set_freq: blocking, the speed chosen (dadagpu_speed), and the bus level of
	 * its pwrlevel (dev_pm_opp_get_bw's index in the bus table; plain, no AB vote: that is a750-family only).
	 */
	/*
	 * {header, ack_type, freq index, bw index} (a6xx_hfi_gx_bw_perf_vote_cmd; unixnu-sheng's GpuPowerProbe {1, perf, bw}). Until 29
	 * October this sent {header, index, 1, 0}: ack_type the index and freq 1, so the core ran at 160 MHz whatever was said here and
	 * every submission of the home screen took some 24 ms of GPU.
	 */
	vote[1] = 1;                            /* blocking */
	vote[2] = gpu_speed_index >= 1 && gpu_speed_index < GPU_SPEEDS ? gpu_speed_index : 1;
	vote[3] = gpu_bus_index(vote[2]);
	say("4: the 3D core at %u MHz (speed %u), bus level %u (%u KB/s)\n", gpu_speeds[vote[2]].hz / 1000000, vote[2],
	    vote[3], ddr_kbps[vote[3]]);
	if (!hfi_send("the speed wanted", HFI_MSG_GX_BW_PERF_VOTE, vote, 4)) {
		return false;
	}

	say("4: asking the GMU for the 3D core (OOB GPU set)\n");
	hold();
	core_powered = true;
	Gw(G8_GMU_HOST2GMU_INTR_SET, 1u << 30);
	for (int i = 0; i < 10000 && !(granted & 1u << 31); i++) {
		sleep_us(100);
		granted = G(G8_GMU_GMU2HOST_INTR_INFO);
	}
	Gw(G8_GMU_GMU2HOST_INTR_CLR, 1u << 31);
	say("4: g2h %08x, gx gdsc %08x, pwr clk %08x, rpmh state %08x\n", granted, rd(gpu + GX_GDSCR_OFFSET),
	    G(G8_GMU_PWR_CLK_STATUS), G(G8_GPU_GMU_CX_GMU_RPMH_POWER_STATE));
	if (!(granted & 1u << 31)) {
		say("4: the GMU does not grant the 3D core\n");
		gmu_log_tail();
		return false;
	}
	say("4: the 3D core: rbbm status %08x int %08x, cp rb base %08x\n", G(G8_RBBM_STATUS), G(G8_RBBM_INT_0_STATUS),
	    G(G8_CP_RB_BASE));
	say("4: THE 3D CORE HAS POWER AND ANSWERS\n");
	return !space_faulted(&gmu_space);
}

static void gpu_block_stop(void);

/* ---- phase 5: the 3D core's memory, microcode, registers; its command processor started ---------------- */

/*
 * a8xx_gpu.c hw_init, in its order, for one ring and without preemption, protection or clock gating
 * (those come when the system draws). The core's addresses below GPU_ADDRESSES_START are constants of
 * ours, as in redwood_adreno.c; GMEM is at 64 MB in every address space (UCHE_*_GC_GMEM_RANGE_MIN), so
 * nothing of ours goes between 0x4000000 and 0x4c00000.
 */
#define CORE_STREAM             0
#define CORE_MATCH              0
#define CORE_LPAC_STREAM        1
#define CORE_LPAC_MATCH         2
#define CORE_RING_ADDRESS       0x01000000u
#define CORE_RING_SIZE          0x8000u
#define CORE_SQE_ADDRESS        0x01100000u
#define CORE_AQE_ADDRESS        0x01200000u
#define CORE_SCRATCH_ADDRESS    0x01300000u     /* +0 rptr shadow, +0x40 the BV pipe's, +0x100 fences, +0x1000 tests */
#define CORE_SCRATCH_SIZE       0x4000u
#define CORE_PWRUP_ADDRESS      0x01400000u
#define CORE_PWRUP_SIZE         0x4000u
#define GMEM_RANGE_MIN          0x4000000ull
#define UCHE_TRAP_BASE          0x1fffffffff000ull
#define RING_WORDS              (CORE_RING_SIZE / 4)

#define PIPE_NONE               0
#define PIPE_BR                 1
#define PIPE_BV                 2
#define PIPE_LPAC               3
#define PIPE_DDE_BV             7
#define B(p)                    (1u << (p))

/* pm4 opcodes (adreno_pm4.xml) */
#define CP_NOP                  0x10
#define CP_THREAD_CONTROL       0x17
#define CP_WAIT_FOR_IDLE        0x26
#define CP_MEM_WRITE            0x3d
#define CP_ME_INIT              0x48
#define CP_SET_SECURE_MODE      0x66

static struct gpu_space core_space = { .name = "the 3D core", .context = 0, .control = 1u << 8 /* serve after fault */ };
static struct gpu_block core_ring, core_sqe, core_aqe, core_scratch, core_pwrup;
static bool core_space_on, core_runs, zap_loaded;
static uint32_t ring_at;
/*
 * Where ring() writes: ring 0 (core_ring, ring_at), which is the only ring there is unless boot-arg dadagpu_preempt
 * makes a second (core_ring1, ring1_at) for submissions that may be preempted; ring_select() points these at it for
 * the length of one submission.
 */
static struct gpu_block core_ring1;
static uint32_t ring1_at;
static struct gpu_block *ring_block = &core_ring;
static uint32_t *ring_pos = &ring_at;

/* a830_nonctxt_regs (a8xx_catalog.c): register, value, the pipes whose aperture it is written through */
static const struct {
	uint32_t reg, value, pipes;
} nonctxt[] = {
	{ G8_CP_SMMU_STREAM_ID_LPAC, 0x00000101, B(PIPE_NONE) },
	{ G8_GRAS_DBG_ECO_CNTL, 0x00000800, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_AUTO_VERTEX_STRIDE, 0x00000001, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_VIS_STREAM_CNTL, 0x10010000, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_CONTEXT_SWITCH_STABILIZE_CNTL_1, 0x00000002, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_CHICKEN_BITS_1, 0x00000003, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_CHICKEN_BITS_2, 0x00000200, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_CHICKEN_BITS_3, 0x00400000, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_CHICKEN_BITS_4, 0x00500050, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_UCHE_GBIF_GX_CONFIG, 0x010240e0, B(PIPE_NONE) },
	{ G8_RBBM_GBIF_CLIENT_QOS_CNTL, 0x22122212, B(PIPE_NONE) },
	{ G8_RB_CCU_CNTL, 0x00000068, B(PIPE_BR) },
	{ G8_RB_CCU_DBG_ECO_CNTL, 0x00002200, B(PIPE_BR) },
	{ G8_RB_RESOLVE_PREFETCH_CNTL, 0x00000007, B(PIPE_BR) },
	{ G8_RB_CMP_DBG_ECO_CNTL, 0x00004000, B(PIPE_BR) },
	{ G8_RBBM_NC_MODE_CNTL, 0x00000001, B(PIPE_NONE) },
	{ G8_RBBM_SLICE_NC_MODE_CNTL, 0x00000001, B(PIPE_NONE) },
	{ G8_RBBM_WAIT_IDLE_CLOCKS_CNTL, 0x00000030, B(PIPE_NONE) },
	{ G8_RBBM_WAIT_IDLE_CLOCKS_CNTL2, 0x00000030, B(PIPE_NONE) },
	{ G8_RBBM_CGC_P2S_CNTL, 0x00000040, B(PIPE_NONE) },
	{ G8_SP_PERFCTR_SHADER_MASK, 0x0000003f, B(PIPE_NONE) },
	{ G8_SP_CHICKEN_BITS_1, 1u << 26 | 1u << 30, B(PIPE_NONE) },
	{ G8_SP_CHICKEN_BITS_2, 1u << 22 | 1u << 23, B(PIPE_NONE) },
	{ G8_SP_CHICKEN_BITS_3, 0x00300000, B(PIPE_NONE) },
	{ G8_SP_HLSQ_TIMEOUT_THRESHOLD_DP, 0x00000080, B(PIPE_NONE) },
	{ G8_TPL1_DBG_ECO_CNTL, 0x10000000, B(PIPE_NONE) },
	{ G8_TPL1_DBG_ECO_CNTL1, 0x04000724, B(PIPE_NONE) },
	{ G8_UCHE_MODE_CNTL, 0x00080000, B(PIPE_NONE) },
	{ G8_UCHE_CCHE_MODE_CNTL, 0x00001000, B(PIPE_NONE) },
	{ G8_UCHE_CCHE_CACHE_WAYS, 0x00000800, B(PIPE_NONE) },
	{ G8_UCHE_HW_DBG_CNTL, 1u << 8, B(PIPE_NONE) },
	{ G8_UCHE_VARB_IDLE_TIMEOUT, 0x00000020, B(PIPE_NONE) },
	{ G8_VFD_DBG_ECO_CNTL, 0x00008000, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VFD_CB_BV_THRESHOLD, 0x00500050, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VFD_CB_BR_THRESHOLD, 0x00600060, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VFD_CB_BUSY_REQ_CNT, 0x00200020, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VFD_CB_LP_REQ_CNT, 0x00100020, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VPC_FLATSHADE_MODE_CNTL, 0x00000001, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VSC_KMD_DBG_ECO_CNTL, 1u << 11, B(PIPE_NONE) },
};
#define GMEM_PROTECT            0x0c000000u     /* RB_GC_GMEM_PROTECT, written once out of secure mode */

static void
aperture(uint32_t pipe)
{
	Gw(G8_CP_APERTURE_CNTL_HOST, pipe << 12);
}

static void
Gw64(uint32_t reg, uint64_t value)
{
	Gw(reg, (uint32_t)value);
	Gw(reg + 1, (uint32_t)(value >> 32));
}

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static void
ring(uint32_t word)
{
	((volatile uint32_t *)ring_block->memory)[*ring_pos] = word;
	*ring_pos = (*ring_pos + 1) % RING_WORDS;
}

static void
ring_command(uint32_t opcode, uint32_t words)
{
	ring(7u << 28 | words | odd_parity(words) << 15 | opcode << 16 | odd_parity(opcode) << 23);
}

/* what is in the ring to the core, and wait until it has read all of it and rests (a8xx_flush, a8xx_idle) */
static bool
ring_run(const char *what)
{
	uint32_t rptr = ~0u, status = ~0u;

	to_memory(core_ring.memory, core_ring.size);
	say("%u: the 3D core is to %s (ring up to %u)\n", phase, what, ring_at);
	hold();
	Gw(G8_CP_RB_WPTR, ring_at);
	for (int i = 0; i < 20000; i++) {
		sleep_us(100);
		rptr = G(G8_CP_RB_RPTR);
		status = G(G8_RBBM_STATUS);
		if (rptr == ring_at && !(status & ~1u)) {
			break;
		}
	}
	if (rptr == ring_at && !(status & ~1u) && !space_faulted(&core_space)) {
		say("%u: done (rptr %u, status %08x)\n", phase, rptr, status);
		return true;
	}
	aperture(PIPE_BR);
	say("%u: the 3D core does not get through: rptr %u of %u, status %08x, int %08x; BR: hw fault %08x int %08x "
	    "prot %08x sqe %08x\n", phase, rptr, ring_at, status, G(G8_RBBM_INT_0_STATUS), G(G8_CP_HW_FAULT_STATUS_PIPE),
	    G(G8_CP_INTERRUPT_STATUS_PIPE), G(G8_CP_PROTECT_STATUS_PIPE), G(G8_CP_SQE_STATUS_PIPE));
	aperture(PIPE_BV);
	say("%u:   BV: hw fault %08x int %08x prot %08x sqe %08x; global int %08x\n", phase, G(G8_CP_HW_FAULT_STATUS_PIPE),
	    G(G8_CP_INTERRUPT_STATUS_PIPE), G(G8_CP_PROTECT_STATUS_PIPE), G(G8_CP_SQE_STATUS_PIPE),
	    G(G8_CP_INTERRUPT_STATUS_GLOBAL));
	aperture(PIPE_NONE);
	space_faulted(&core_space);
	return false;
}

static bool
phase_core_start(void)
{
	const uint8_t *sqe, *aqe;
	uint64_t sqe_size = 0, aqe_size = 0;

	if (!core_powered) {
		return false;
	}
	sqe = image_file("gen80000_sqe.fw", &sqe_size);
	aqe = image_file("gen80000_aqe.fw", &aqe_size);
	if (sqe == NULL || aqe == NULL || sqe_size < 8 || aqe_size < 8) {
		return false;
	}
	say("5: the 3D core's memory: ring, SQE (%llu), AQE (%llu), scratch, power-up list\n", sqe_size, aqe_size);
	if (!block_new(&core_space, &core_ring, CORE_RING_ADDRESS, CORE_RING_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_sqe, CORE_SQE_ADDRESS, (uint32_t)sqe_size - 4, PTE_CORE | PTE_READ_ONLY) ||
	    !block_new(&core_space, &core_aqe, CORE_AQE_ADDRESS, (uint32_t)aqe_size - 4, PTE_CORE | PTE_READ_ONLY) ||
	    !block_new(&core_space, &core_scratch, CORE_SCRATCH_ADDRESS, CORE_SCRATCH_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_pwrup, CORE_PWRUP_ADDRESS, CORE_PWRUP_SIZE, PTE_CORE)) {
		return false;
	}
	memcpy((void *)core_sqe.memory, sqe + 4, sqe_size - 4);         /* adreno_fw_create_bo: past the first word */
	memcpy((void *)core_aqe.memory, aqe + 4, aqe_size - 4);
	to_memory(core_sqe.memory, core_sqe.size);
	to_memory(core_aqe.memory, core_aqe.size);

	say("5: SMMU context 0 for the core's streams 0 and 1\n");
	hold();
	core_space_on = true;
	if (!space_on(&core_space, CORE_MATCH, CORE_STREAM)) {
		return false;
	}
	wr(smmu + SMMU_S2CR(CORE_LPAC_MATCH), core_space.context);
	wr(smmu + SMMU_SMR(CORE_LPAC_MATCH), SMMU_SMR_VALID | CORE_LPAC_STREAM);

	say("5: the core's registers (a8xx hw_init)\n");
	hold();
	aperture(PIPE_NONE);
	Gw(G8_GBIF_HALT, 0);
	(void)G(G8_GBIF_HALT);
	Gw(G8_RBBM_GBIF_HALT, 0);
	(void)G(G8_RBBM_GBIF_HALT);
	Gw(G8_RBBM_SECVID_TSB_CNTL, 0);
	Gw64(G8_RBBM_SECVID_TSB_TRUSTED_BASE, 0);
	Gw(G8_RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
	Gw(G8_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
	Gw64(G8_UCHE_CCHE_GC_GMEM_RANGE_MIN, GMEM_RANGE_MIN);
	Gw64(G8_SP_HLSQ_GC_GMEM_RANGE_MIN, GMEM_RANGE_MIN);
	Gw64(G8_UCHE_TRAP_BASE, UCHE_TRAP_BASE);
	Gw64(G8_UCHE_WRITE_THRU_BASE, UCHE_TRAP_BASE);
	Gw64(G8_UCHE_CCHE_TRAP_BASE, UCHE_TRAP_BASE);
	Gw64(G8_UCHE_CCHE_WRITE_THRU_BASE, UCHE_TRAP_BASE);
	Gw(G8_RBBM_PERFCTR_CNTL, 1);
	Gw(G8_RBBM_SLICE_PERFCTR_CNTL, 1);
	Gw(G8_CP_PERFCTR_CP_SEL(0), 1);
	Gw(G8_UCHE_CLIENT_PF, 1u << 7 | 1);
	for (uint32_t pipe = PIPE_NONE; pipe <= PIPE_DDE_BV; pipe++) {
		if (pipe == PIPE_LPAC) {
			continue;
		}
		aperture(pipe);
		for (unsigned i = 0; i < sizeof(nonctxt) / sizeof(nonctxt[0]); i++) {
			if (nonctxt[i].pipes & B(pipe)) {
				Gw(nonctxt[i].reg, nonctxt[i].value);
			}
		}
	}
	aperture(PIPE_NONE);
	Gw(G8_RBBM_INTERFACE_HANG_INT_CNTL, 1u << 30 | 0xcfffff);
	Gw(G8_RBBM_SLICE_INTERFACE_HANG_INT_CNTL, 1u << 30);
	/* a8xx_set_ubwc_config for sm8750 (UBWC 5.0, highest bank bit 16: hbb 3, swizzling on, version tag 4) */
	aperture(PIPE_BV);
	Gw(G8_GRAS_NC_MODE_CNTL, 3u << 5);
	aperture(PIPE_BR);
	Gw(G8_GRAS_NC_MODE_CNTL, 3u << 5);
	Gw(G8_RB_CCU_NC_MODE_CNTL, 3u << 1);
	Gw(G8_RB_CMP_NC_MODE_CNTL, 4u << 15 | 1u << 2 | 1u << 1);
	aperture(PIPE_NONE);
	Gw(G8_SP_NC_MODE_CNTL, 2u << 4 | 3u << 1);
	Gw(G8_TPL1_NC_MODE_CNTL, 3u << 1);
	aperture(PIPE_BR);
	Gw(G8_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE, 1);
	for (uint32_t pipe = PIPE_BR; pipe <= PIPE_DDE_BV; pipe++) {
		if (pipe == PIPE_LPAC) {
			continue;
		}
		aperture(pipe);
		/* icache, rb fetch, rb privileged, rptr writeback; BR also the crash dumper's reads and writes */
		Gw(G8_CP_APRIV_CNTL_PIPE, pipe == PIPE_BR ? 0x6f : 0x0f);
	}
	aperture(PIPE_NONE);
	Gw64(G8_CP_SQE_INSTR_BASE, CORE_SQE_ADDRESS);
	Gw64(G8_CP_AQE_INSTR_BASE_0, CORE_AQE_ADDRESS);
	Gw64(G8_CP_RB_BASE, CORE_RING_ADDRESS);
	Gw(G8_CP_RB_CNTL, 12u | 2u << 8);       /* MSM_GPU_RB_CNTL_DEFAULT: 2^12 quad words, blocks of 32 bytes */
	Gw64(G8_CP_RB_RPTR_ADDR, CORE_SCRATCH_ADDRESS);
	Gw64(G8_CP_RB_RPTR_ADDR_BV, CORE_SCRATCH_ADDRESS + 0x40);
	say("5: sqe base %08x rb base %08x cntl %08x status %08x\n", G(G8_CP_SQE_INSTR_BASE), G(G8_CP_RB_BASE),
	    G(G8_CP_RB_CNTL), G(G8_RBBM_STATUS));

	say("5: the command processor started\n");
	hold();
	core_runs = true;
	Gw(G8_CP_SQE_CNTL, 1);
	sleep_ms(20);
	say("5: alive after it: status %08x int %08x rptr %u\n", G(G8_RBBM_STATUS), G(G8_RBBM_INT_0_STATUS), G(G8_CP_RB_RPTR));
	if (space_faulted(&core_space)) {
		return false;
	}

	/* a8xx_cp_init */
	ring_at = 0;
	ring_command(CP_THREAD_CONTROL, 1);
	ring(1u << 27);                         /* no concurrent binning */
	ring_command(CP_ME_INIT, 7);
	ring(1u << 0 | 1u << 1 | 1u << 3 | 1u << 6 | 1u << 8);
	ring(0x00000003);
	ring(0x20000000);
	ring(0x00000002);
	ring(CORE_PWRUP_ADDRESS);
	ring(0);
	ring(1u << 31);
	if (!ring_run("initialize (CP_ME_INIT)")) {
		return false;
	}
	say("5: THE 3D CORE TAKES COMMANDS\n");
	return true;
}

/* ---- phase 6: the zap shader; out of secure mode; a word written on command ----------------------------- */

/*
 * adreno_zap_shader_load -> qcom_mdt_load for the one-file image gen80000_zap.mbn (an ELF of three
 * program headers: the headers themselves, one relocatable segment of 0x3b4 bytes, the hash
 * segment): TrustZone gets the ELF header and the hashes (PAS 13, "init image"), the place (dt:
 * gpu_microcode_region 0x9b09a000, 8K) and the segment there, and is asked to authenticate it.
 * Whatever a kernel before this one left there is given up first, as in redwood_adreno.c.
 */
#define ZAP_MEMORY              0x9b09a000ull
#define ZAP_MEMORY_SIZE         0x2000u
#define PAS_GPU                 13
#define SCM_ONE_VALUE           1
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_THREE_VALUES        3

static bool
zap_load(void)
{
	const uint8_t *image;
	uint64_t size = 0, answer = ~0ull, metadata_phys;
	uint32_t phoff, phnum, header_size = 0, hash_offset = 0, hash_size = 0, seg_offset = 0, seg_size = 0;
	vm_offset_t page = 0;
	volatile uint8_t *memory;
	int32_t r;

	image = image_file("gen80000_zap.mbn", &size);
	if (image == NULL || size < 52 || memcmp(image, "\177ELF\1", 5) != 0) {
		return false;
	}
	memcpy(&phoff, image + 28, 4);
	phnum = image[44] | image[45] << 8;
	for (uint32_t i = 0; i < phnum && phoff + 32 * (i + 1) <= size; i++) {
		uint32_t ph[8];

		memcpy(ph, image + phoff + 32 * i, sizeof(ph));      /* type offset vaddr paddr filesz memsz flags align */
		if (i == 0) {
			header_size = ph[4];
		} else if ((ph[6] & (7u << 24)) == (2u << 24)) {
			hash_offset = ph[1];
			hash_size = ph[4];
		} else if (ph[0] == 1 && ph[5] != 0) {
			seg_offset = ph[1];
			seg_size = ph[4];
		}
	}
	say("6: zap: header %#x, hashes %#x at %#x, segment %#x at %#x\n", header_size, hash_size, hash_offset, seg_size,
	    seg_offset);
	if (header_size == 0 || hash_size == 0 || seg_size == 0 || header_size + hash_size > PAGE_SIZE ||
	    seg_size > ZAP_MEMORY_SIZE || hash_offset + hash_size > size || seg_offset + seg_size > size) {
		return false;
	}
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	memcpy((void *)page, image, header_size);
	memcpy((void *)(page + header_size), image + hash_offset, hash_size);
	to_memory(page, PAGE_SIZE);
	metadata_phys = kvtophys(page);

	say("6: TrustZone: give up an earlier zap shader\n");
	hold();
	r = scm(SCM_64 | SCM_SIP | SCM_CALL(2, 6), SCM_ONE_VALUE, PAS_GPU, 0, 0, &answer);
	say("6: shutdown: %d (%#llx)\n", r, answer);
	say("6: TrustZone: the zap shader's signature\n");
	hold();
	r = scm(SCM_64 | SCM_SIP | SCM_CALL(2, 1), SCM_VALUE_AND_BUFFER, PAS_GPU, metadata_phys, 0, &answer);
	say("6: init image: %d (%#llx)\n", r, answer);
	if (r != 0 || answer != 0) {
		return false;
	}
	r = scm(SCM_64 | SCM_SIP | SCM_CALL(2, 2), SCM_THREE_VALUES, PAS_GPU, ZAP_MEMORY, 0x1000, &answer);
	say("6: mem setup: %d (%#llx)\n", r, answer);
	if (r != 0 || answer != 0) {
		return false;
	}
	memory = (volatile uint8_t *)ml_io_map_wcomb(ZAP_MEMORY, ZAP_MEMORY_SIZE);
	for (uint32_t i = 0; i < 0x1000; i++) {
		memory[i] = i < seg_size ? image[seg_offset + i] : 0;
	}
	__asm__ volatile ("dsb sy");
	say("6: TrustZone: authenticate and run it\n");
	hold();
	r = scm(SCM_64 | SCM_SIP | SCM_CALL(2, 5), SCM_ONE_VALUE, PAS_GPU, 0, 0, &answer);
	say("6: auth and reset: %d (%#llx)\n", r, answer);
	if (r != 0 || answer != 0) {
		return false;
	}
	zap_loaded = true;
	say("6: TRUSTZONE RUNS THE ZAP SHADER\n");
	return true;
}

static bool
phase_core_commands(void)
{
	volatile uint32_t *test = (volatile uint32_t *)(core_scratch.memory + 0x1000);

	if (!core_runs || !zap_load()) {
		return false;
	}
	ring_command(CP_SET_SECURE_MODE, 1);
	ring(0);
	if (!ring_run("leave secure mode")) {
		return false;
	}
	aperture(PIPE_BR);
	Gw(G8_RB_GC_GMEM_PROTECT, GMEM_PROTECT);
	aperture(PIPE_NONE);

	*test = 0;
	to_memory(core_scratch.memory + 0x1000, 64);
	ring_command(CP_MEM_WRITE, 3);
	ring(CORE_SCRATCH_ADDRESS + 0x1000);
	ring(0);
	ring(0x600dc0de);
	ring_command(CP_WAIT_FOR_IDLE, 0);
	if (!ring_run("write a word to memory")) {
		return false;
	}
	to_memory(core_scratch.memory + 0x1000, 64);
	say("6: the word reads %08x: %s\n", *test, *test == 0x600dc0de ? "THE 3D CORE WRITES OUR MEMORY ON COMMAND" :
	    "not what it was told");
	return *test == 0x600dc0de;
}

/* ---- the GPU kept on, for the system to draw with ------------------------------------------------------ */

/*
 * boot-arg dadagpu_keep=1 (with dadagpu=6): the phases' work is not undone. The 3D core stays
 * powered (the OOB request of phase 4 is never taken back) and the service RedwoodGPU
 * (iokit/Kernel/RedwoodGPU.cpp) hands it to programs through the interface of Linux's msm driver,
 * as it does the redwood's Adreno: redwood_adreno.c's redwood_gpu_* functions come here when
 * hakios_dada_gpu_ready() says so. One address space for every client (context 0), addresses
 * handed out from DADA_ADDRESSES_START by a first-fit list, as redwood_adreno.c explains at length.
 */
#define DADA_ADDRESSES_START    0x20000000ull
#define DADA_ADDRESSES_SIZE     0x3e0000000ull   /* 15.5 GB: 3.5 GB ran out with every poster up (g2k14, "no addresses left", the wallpaper black); the render server's growth that froze g2k2 and g2k3 with 16 GB is gone (g2k9 on: stable) */
#define DADA_GRANULE            0x10000ull
#define FENCE_AT                0x100           /* in the scratch block: the ring's fence */
#define BV_FENCE_AT             0x108           /* ... and the BV pipe's */
#define TIMES_AT                0x400           /* ... when each of the last TIMES submissions began and ended */
#define TIMES                   64
#define CP_REG_TO_MEM           0x3e
#define CP_INDIRECT_BUFFER      0x3f
#define CP_EVENT_WRITE7         0x46
#define CP_SET_MARKER           0x65
#define CP_WAIT_TIMESTAMP       0x14
#define CCU_INVALIDATE_DEPTH    0x18
#define CCU_INVALIDATE_COLOR    0x19
#define CACHE_CLEAN             0x31
#define THREAD_BR               1
#define THREAD_BV               2
#define THREAD_BOTH             3
#define SYNC_THREADS            (1u << 31)

extern void RedwoodGPUStart(void);
extern uint32_t gpu_log;

static bool kept;
static uint32_t fence;                  /* the number of the last submission */

/*
 * Boot-arg dadagpu_preempt=1 (or 2, below): two rings and hardware preemption between them, as Linux's a8xx_preempt.c
 * (March 2026, for the A840; the A830's catalog entry carries ADRENO_QUIRK_PREEMPTION too). With one ring, the
 * wallpaper's 40-55 ms submissions and the widgets' sat in front of backboardd's frames, and the frames stuttered.
 * Ring 0 is backboardd's (RedwoodGPU.cpp says which, through dada_gpu_next_ring()), ring 1 everybody else's; when
 * ring 0 is given work while the core is on ring 1, the core is told to switch (CP_CONTEXT_SWITCH_CNTL), and once
 * ring 0 has nothing left it is switched back. Linux learns that a switch is over from the CP_SW interrupt and that a
 * ring has emptied from CACHE_FLUSH_TS; there are no interrupts here, so a thread of ours looks (preempt_poll()).
 *
 * A submission's number (a "ticket", what RedwoodGPU hands programs as the fence) stays one increasing 32-bit count
 * over both rings, and its lowest bit is its ring: ring 0's are even, ring 1's odd, so a ticket says by itself which
 * ring's fence word answers for it, and each ring's fence word -- written with the ticket -- only ever grows. Two
 * tickets of different rings say nothing about each other's order of completion: whoever waits for "everything"
 * waits for each ring's last (dada_gpu_last_on()).
 *
 * With 0 (the default) none of this is touched: one ring, the same packets as before.
 */
#define RINGS                   2
#define RPTR_AT(r)              (0x80u * (r))           /* in the scratch block: ring r's rptr shadow, +0x40 the BV's */
#define RING_FENCE_AT(r)        (FENCE_AT + 0x10u * (r)) /* ... its fence (ring 0's is FENCE_AT), +8 the BV pipe's */
#define RING_TIMES_AT(r)        (TIMES_AT + 0x400u * (r)) /* ... its TIMES start and end stamps */
/* (default 1 since 29 October: g2k44/g2k45/g2k46 ran with it, no fault, smoother by the user; dadagpu_preempt=0: one ring) */
static TUNABLE(uint32_t, gpu_preempt_arg, "dadagpu_preempt", 1);
static bool preempt_on;                 /* two rings in use: everything below is gated on it */
static bool preempt_halted;             /* the GPU is being given back: the poll thread leaves the core alone */
static uint32_t last_ticket[RINGS];     /* each ring's last submission (Linux: a6xx_gpu->last_seqno[]) */
static uint32_t submit_ring;            /* the ring of the next dada_gpu_submit(); back to 0 after each */
static struct gpu_block *const ring_blocks[RINGS] = { &core_ring, &core_ring1 };
static uint32_t *const ring_ats[RINGS] = { &ring_at, &ring1_at };
static uint32_t timed, timed_count, timed_by[7];
static uint64_t timed_ticks;
static uint64_t busy_ticks;             /* all the core has worked, never reset (the governor's) */
static struct {
	uint64_t start, size;
} va_free[1024];
static unsigned va_free_count, va_holes_most;
static uint64_t va_used, va_lost;
static uint32_t va_live, va_refused;

bool
hakios_dada_gpu_ready(void)
{
	return kept;
}

static void
ring_register(uint32_t reg, uint32_t value)
{
	ring(4u << 28 | 1 | odd_parity(1) << 7 | reg << 8 | odd_parity(reg) << 27);
	ring(value);
}

static void
ring_thread(uint32_t what)
{
	ring_command(CP_THREAD_CONTROL, 1);
	ring(what);
}

bool
dada_gpu_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01:                              /* GPU_ID: none, the chip id says it */
	case 0x06:                              /* GMEM_BASE */
	case 0x08:                              /* PP_PGTABLE */
	case 0x09:                              /* FAULTS */
	case 0x0a:                              /* SUSPENDS */
	case 0x11:                              /* RAYTRACING: not offered yet */
	case 0x15:                              /* HAS_PRR */
	case 0x17:                              /* AQE: not used */
		*value = 0;
		return true;
	case 0x02:                              /* GMEM_SIZE: 12 MB over the three slices */
		*value = 12 * 1024 * 1024;
		return true;
	case 0x03:                              /* CHIP_ID */
		*value = 0x44050001;
		return true;
	case 0x04:                              /* MAX_FREQ */
		*value = gpu_speeds[GPU_SPEEDS - 1].hz;
		return true;
	case 0x05:                              /* TIMESTAMP */
		*value = mach_absolute_time();
		return true;
	case 0x07:                              /* PRIORITIES */
		*value = 1;
		return true;
	case 0x10:                              /* HIGHEST_BANK_BIT (ubwc_config.c: sm8750, UBWC 5.0) */
		*value = 16;
		return true;
	case 0x12:                              /* UBWC_SWIZZLE: levels 2 and 3 */
		*value = 6;
		return true;
	case 0x13:                              /* MACROTILE_MODE */
		*value = 1;
		return true;
	case 0x14:                              /* UCHE_TRAP_BASE */
		*value = UCHE_TRAP_BASE;
		return true;
	default:
		return false;
	}
}

static uint64_t
va_span(uint64_t size)
{
	return (size + (DADA_GRANULE - 1)) & ~(DADA_GRANULE - 1);
}

uint64_t
dada_gpu_address(uint64_t size)
{
	size = va_span(size);
	for (unsigned i = 0; i < va_free_count; i++) {
		if (va_free[i].size >= size) {
			uint64_t address = va_free[i].start;

			va_free[i].start += size;
			va_free[i].size -= size;
			if (va_free[i].size == 0) {
				memmove(&va_free[i], &va_free[i + 1], (--va_free_count - i) * sizeof(va_free[0]));
			}
			va_used += size;
			va_live++;
			return address;
		}
	}
	va_refused++;
	return 0;
}

void
dada_gpu_address_free(uint64_t address, uint64_t size)
{
	unsigned at;

	size = va_span(size);
	va_used -= size;
	va_live--;
	for (at = 0; at < va_free_count && va_free[at].start < address; at++) {
	}
	if (at > 0 && va_free[at - 1].start + va_free[at - 1].size == address) {
		va_free[at - 1].size += size;
		if (at < va_free_count && address + size == va_free[at].start) {
			va_free[at - 1].size += va_free[at].size;
			memmove(&va_free[at], &va_free[at + 1], (--va_free_count - at) * sizeof(va_free[0]));
		}
	} else if (at < va_free_count && address + size == va_free[at].start) {
		va_free[at].start = address;
		va_free[at].size += size;
	} else if (va_free_count < sizeof(va_free) / sizeof(va_free[0])) {
		memmove(&va_free[at + 1], &va_free[at], (va_free_count++ - at) * sizeof(va_free[0]));
		va_free[at].start = address;
		va_free[at].size = size;
	} else {
		va_lost += size;
	}
	if (va_free_count > va_holes_most) {
		va_holes_most = va_free_count;
	}
}

void
dada_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes, unsigned *most, uint64_t *lost,
    uint32_t *refused)
{
	*largest = 0;
	for (unsigned i = 0; i < va_free_count; i++) {
		if (va_free[i].size > *largest) {
			*largest = va_free[i].size;
		}
	}
	*used = va_used;
	*live = va_live;
	*holes = va_free_count;
	*most = va_holes_most;
	*lost = va_lost;
	*refused = va_refused;
}

bool
dada_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return space_map(&core_space, address, phys, PTE_CORE | (read_only ? PTE_READ_ONLY : 0));
}

void
dada_gpu_mapped(void)
{
	space_commit(&core_space);
}

void
dada_gpu_unmap(uint64_t address, uint64_t size)
{
	uint32_t context = smmu_context(&core_space);

	/* the entries emptied (0: no page), without making tables where there are none */
	for (uint64_t offset = 0; offset < size; offset += 4096) {
		uint32_t at;
		uint64_t *level3 = space_leaf(&core_space, address + offset, false, &at);

		if (level3 != NULL) {
			level3[(address + offset) >> 12 & 0x1ff] = 0;
			space_dirty(&core_space, at);
		}
	}
	space_commit(&core_space);
	wr(smmu + context + SMMU_CB_TLBIASID, core_space.context);
	wr(smmu + context + SMMU_CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(smmu + context + SMMU_CB_TLBSTATUS) & 1); i++) {
	}
}

void
dada_gpu_to_memory(void *start, uint64_t size)
{
	to_memory((vm_offset_t)start, size);
}

bool
dada_gpu_done(uint32_t which)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + FENCE_AT);

	if (preempt_on) {
		/* the ticket's own ring's fence (its lowest bit); 0 names no submission and is done */
		if (which == 0) {
			return true;
		}
		word = (volatile uint32_t *)(core_scratch.memory + RING_FENCE_AT(which & 1));
	}
	to_memory((vm_offset_t)word, 4);
	return (int32_t)(*word - which) >= 0;
}

uint32_t
dada_gpu_last(void)
{
	return fence;
}

/*
 * With two rings, the last submission says nothing about the other ring's: who waits for all the GPU was given
 * (RedwoodGPU.cpp's clientClose, a CPU_PREP of no buffer) waits for both of these. With one ring, ring 0's is the last.
 */
uint32_t dada_gpu_last_on(uint32_t ring);
uint32_t
dada_gpu_last_on(uint32_t ring)
{
	if (!preempt_on) {
		return ring == 0 ? fence : 0;
	}
	return last_ticket[ring & 1];
}

/* whether there are two rings (boot-arg dadagpu_preempt, and it came up) */
bool dada_gpu_rings(void);
bool
dada_gpu_rings(void)
{
	return preempt_on;
}

/* the ring the next dada_gpu_submit() goes to: 0 backboardd's, 1 everybody else's (RedwoodGPU.cpp, under its lock) */
void dada_gpu_next_ring(uint32_t ring);
void
dada_gpu_next_ring(uint32_t ring)
{
	submit_ring = ring & 1;
}

/* the core writes down the always-on counter (19.2 MHz) at one end of a submission */
static void
ring_time(uint32_t which, uint32_t end)
{
	ring_command(CP_REG_TO_MEM, 3);
	ring(G8_CP_ALWAYS_ON_COUNTER | 2u << 18 | 1u << 30);   /* two words, a 64-bit register */
	ring(CORE_SCRATCH_ADDRESS + TIMES_AT + 16 * (which % TIMES) + 8 * end);
	ring(0);
}

/*
 * Whose submissions the core worked on: the submitting process of each fence's slot, and each process's ticks since the last report
 * (dada_gpu_worked_by: the render server's own GPU time a frame, which the totals mix with the posters' and widgets', g2k30).
 */
extern int proc_selfpid(void);
static int32_t times_pid[TIMES];
/*
 * With two rings each has its own TIMES slots (RING_TIMES_AT), by its own count of submissions, and remembers which
 * ticket and process each slot is: a ring's tickets are not consecutive. A ring 1 submission that was preempted is
 * timed from its start to its end, ring 0's work in between included.
 */
static uint32_t ring_count[RINGS], ring_timed[RINGS], slot_ticket[RINGS][TIMES];
static int32_t slot_pid[RINGS][TIMES];
static struct {
	int32_t pid;
	uint32_t count;
	uint64_t ticks;
} pid_times[8];

static void
pid_add(int32_t pid, uint64_t ticks)
{
	unsigned i, least = 0;

	for (i = 0; i < 8 && pid_times[i].pid != pid; i++) {
		if (pid_times[i].ticks < pid_times[least].ticks) {
			least = i;
		}
	}
	if (i == 8) {
		i = least;              /* the least busy gives way */
		pid_times[i].pid = pid;
		pid_times[i].ticks = 0;
		pid_times[i].count = 0;
	}
	pid_times[i].ticks += ticks;
	pid_times[i].count++;
}

void dada_gpu_worked_by(char *out, size_t size);
void
dada_gpu_worked_by(char *out, size_t size)
{
	size_t at = 0;

	out[0] = 0;
	for (unsigned i = 0; i < 8 && at < size; i++) {
		if (pid_times[i].count != 0) {
			int n = snprintf(out + at, size - at, " pid %d %llu ms/%u", pid_times[i].pid,
			    pid_times[i].ticks * 10 / 192 / 1000, pid_times[i].count);

			at += n > 0 ? (size_t) n : 0;
		}
	}
	bzero(pid_times, sizeof(pid_times));
}

static void
time_taken(uint64_t ticks, uint32_t which, int32_t pid)
{
	static const uint32_t under[6] = { 1920, 4800, 9600, 19200, 38400, 76800 };
	unsigned by = 0;

	if (gpu_log) {
		redwood_tl("tl: q %u took %llu\n", which, ticks * 10 / 192);
	}
	while (by < 6 && ticks >= under[by]) {
		by++;
	}
	timed_by[by]++;
	timed_ticks += ticks;
	busy_ticks += ticks;
	timed_count++;
	pid_add(pid, ticks);
}

static void
times_add_rings(void)
{
	to_memory(core_scratch.memory, 4096);
	for (uint32_t r = 0; r < RINGS; r++) {
		volatile uint64_t *times = (volatile uint64_t *)(core_scratch.memory + RING_TIMES_AT(r));
		uint32_t done = *(volatile uint32_t *)(core_scratch.memory + RING_FENCE_AT(r));

		if (ring_count[r] - ring_timed[r] > TIMES) {
			ring_timed[r] = ring_count[r] - TIMES;
		}
		while (ring_timed[r] != ring_count[r]) {
			uint32_t slot = ring_timed[r] % TIMES;

			if ((int32_t)(done - slot_ticket[r][slot]) < 0) {
				break;
			}
			ring_timed[r]++;
			if (times[2 * slot + 1] > times[2 * slot]) {
				time_taken(times[2 * slot + 1] - times[2 * slot], slot_ticket[r][slot], slot_pid[r][slot]);
			}
		}
	}
}

static void
times_add(void)
{
	volatile uint64_t *times = (volatile uint64_t *)(core_scratch.memory + TIMES_AT);
	uint32_t done;

	if (preempt_on) {
		times_add_rings();
		return;
	}
	to_memory(core_scratch.memory, 4096);
	done = *(volatile uint32_t *)(core_scratch.memory + FENCE_AT);
	if (done - timed > TIMES) {
		timed = done - TIMES;
	}
	while (timed != done) {
		uint32_t slot = ++timed % TIMES;

		if (timed != 0 && times[2 * slot + 1] > times[2 * slot]) {
			static const uint32_t under[6] = { 1920, 4800, 9600, 19200, 38400, 76800 };
			uint64_t ticks = times[2 * slot + 1] - times[2 * slot];
			unsigned by = 0;

			if (gpu_log) {
				redwood_tl("tl: q %u took %llu\n", timed, ticks * 10 / 192);
			}
			while (by < 6 && ticks >= under[by]) {
				by++;
			}
			timed_by[by]++;
			timed_ticks += ticks;
			busy_ticks += ticks;
			timed_count++;
			pid_add(times_pid[slot], ticks);
		}
	}
}

void
dada_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	times_add();
	*microseconds = timed_ticks * 10 / 192;
	*submissions = timed_count;
	memcpy(by, timed_by, sizeof(timed_by));
	bzero(timed_by, sizeof(timed_by));
	timed_ticks = 0;
	timed_count = 0;
}

/* ---- two rings and preemption (boot-arg dadagpu_preempt; see `preempt_on`) ------------------------------ */

/*
 * The memory of it, in the core's address space below DADA_ADDRESSES_START and away from GMEM's 64 MB: ring 1 next to
 * the others, a page of SMMU information a ring, the postamble, and each ring's preemption record, which the command
 * processor saves the ring's state into when it switches away and restores it from when it switches back. Its size is
 * the A830's catalog entry's (a8xx_catalog.c: preempt_record_size = 13536K); Linux maps the records, the SMMU
 * information and the postamble privileged (MSM_BO_MAP_PRIV: the SMMU lets only the CP's own accesses at them, which
 * CP_APRIV_CNTL makes privileged), and so do we (PTE_GMU: our attributes without "unprivileged").
 */
#define CORE_RING1_ADDRESS      0x01500000u     /* as big as ring 0 (CORE_RING_SIZE) */
#define CORE_SMMU_INFO_ADDRESS  0x01600000u     /* + 0x1000 a ring */
#define CORE_POSTAMBLE_ADDRESS  0x01700000u
#define CORE_RECORD_ADDRESS(r)  (0x08000000u + 0x01000000u * (r))
#define PREEMPT_RECORD_SIZE     (13536u * 1024)
#define CP_REG_RMW              0x21
#define CP_WAIT_REG_MEM         0x3c
#define CP_SET_AMBLE            0x55
#define CP_SET_PSEUDO_REG       0x56
#define CP_SET_PROTECTED_MODE   0x5f
#define CP_CONTEXT_SWITCH_YIELD 0x6b
#define PSEUDO_SMMU_INFO        0
#define PSEUDO_NON_SECURE_SAVE_ADDR 1
#define PSEUDO_NON_PRIV_SAVE_ADDR 3
#define PSEUDO_COUNTER          4
#define KMD_AMBLE_TYPE          3
#define MARKER_IFPC_OFF         0x101
#define MARKER_IFPC_ON          0x100
#define MARKER_IB1LIST_START    0x00d
#define MARKER_IB1LIST_END      0x00e
#define SWITCH_STOP             (1u << 0)       /* CP_CONTEXT_SWITCH_CNTL (a6xx_cp_context_switch_cntl) */
#define SWITCH_LEVEL(l)         ((l) << 6)
#define SWITCH_USES_GMEM        (1u << 8)
#define SWITCH_SKIP_SAVE_RESTORE (1u << 9)
#define PREEMPT_RECORD_MAGIC    0xae399d6eu     /* A6XX_PREEMPT_RECORD_MAGIC */
#define SMMU_INFO_MAGIC         0x241350d5u     /* GEN7_CP_SMMU_INFO_MAGIC */
#define PREEMPT_WATCHDOG_S      10              /* Linux's preempt_timer */
#define PREEMPT_POLL_US         250

/* a6xx_gpu.h: struct a6xx_preempt_record and struct a7xx_cp_smmu_info */
struct preempt_record {
	uint32_t magic, info, errno_, data, cntl, rptr, wptr, pad;
	uint64_t rptr_addr, rbase, counter, bv_rptr_addr;
};
struct cp_smmu_info {
	uint32_t magic, pad;
	uint64_t ttbr0;
	uint32_t asid, context_idr, context_bank;
};

/* enum a6xx_preempt_state */
enum { PREEMPT_NONE, PREEMPT_START, PREEMPT_FINISH, PREEMPT_TRIGGERED, PREEMPT_FAULTED, PREEMPT_PENDING };

static TUNABLE(uint32_t, gpu_preempt_ib, "dadagpu_preempt_ib", 1);
static TUNABLE(uint32_t, gpu_preempt_post, "dadagpu_preempt_post", 1);  /* 0: the postamble a NOP (preempt_disable_postamble) */
static struct gpu_block core_record[RINGS], core_smmu_info, core_postamble;
static uint32_t postamble_words;
static uint32_t preempt_state, cur_ring, next_ring;
static bool restore_wptr[RINGS];
static uint64_t preempt_began;                  /* when the switch in flight was asked for (absolute time) */
static lck_grp_t *preempt_group;
static lck_mtx_t *preempt_lock;                 /* around all of the above and the rings; RedwoodGPU's lock may be held */
static int preempt_poller;                      /* the poll thread's event */
static struct {
	uint64_t since, switching, longest;     /* absolute time */
	uint32_t triggered, completed, faulted, submitted[RINGS], full[RINGS];
} preempt_stat;

static void
ring_select(uint32_t r)
{
	ring_block = ring_blocks[r];
	ring_pos = ring_ats[r];
}

static uint32_t
scratch_word(uint32_t at)
{
	volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + at);

	to_memory((vm_offset_t)word, 4);
	return *word;
}

static bool
preempt_in_flight(void)
{
	return !(preempt_state == PREEMPT_NONE || preempt_state == PREEMPT_FINISH);         /* a6xx_in_preempt */
}

/*
 * get_rptr: a8xx has APRIV, so the CP keeps each ring's rptr shadow up to date (RPTR_ADDR, saved and restored with
 * the ring), and Linux reads only that. The current ring's own register is read instead while no switch is in flight:
 * one ring had only ever been read so here, and a shadow that did not keep up would fill the ring.
 */
static uint32_t
ring_rptr(uint32_t r)
{
	if (r == cur_ring && !preempt_in_flight()) {
		return G(G8_CP_RB_RPTR);
	}
	return scratch_word(RPTR_AT(r));
}

/*
 * Nothing in the ring that matters: all of it read, or (Linux: for the current ring only) its last fence back -- what
 * may follow that is the end of a submission (a marker, the yield), read when the ring next runs. For the other ring
 * too here: it cannot cost a switch to a ring whose shadow is behind.
 */
static bool
ring_idle(uint32_t r)
{
	return *ring_ats[r] == ring_rptr(r) || scratch_word(RING_FENCE_AT(r)) == last_ticket[r];
}

/* a6xx_preempt.h get_next_ring: the first ring by priority with something in it */
static int
preempt_next_ring(void)
{
	for (uint32_t r = 0; r < RINGS; r++) {
		if (!ring_idle(r)) {
			return (int)r;
		}
	}
	return -1;
}

/* a6xx_preempt.h update_wptr: the ring's wptr to the core, if a submission came while it could not be */
static void
preempt_update_wptr(uint32_t r)
{
	if (restore_wptr[r]) {
		Gw(G8_CP_RB_WPTR, *ring_ats[r]);
		restore_wptr[r] = false;
	}
}

/* a8xx_preempt_keepalive_vote: the GMU keeps the core on until the switch is over (IFPC is off here; as Linux anyway) */
static void
preempt_keepalive(bool on)
{
	Gw(G8_GMU_PWR_COL_PREEMPT_KEEPALIVE, on ? 1 : 0);
}

/*
 * a8xx_preempt_trigger, with the lock held: if a ring other than the current one should run (ring 0 has work, or the
 * current ring has nothing left and the other has), its record gets its wptr, the core the record's and the SMMU
 * information's addresses, and CP_CONTEXT_SWITCH_CNTL the request (level 1, GMEM used, save/restore skipped: Linux's
 * a6xx_preempt_init defaults). The SMMU information: we have one address space and never switch tables, and the CP
 * here has no aperture onto the SMMU's registers set up for it (Linux's arm-smmu-qcom does that through TrustZone),
 * so with dadagpu_preempt=1 the core is told 0 -- "no table switch", which a8xx_preempt_hw_init writes too; with
 * dadagpu_preempt=2 it is told the record's address, as Linux does every time.
 */
static void
preempt_trigger(void)
{
	int ring;
	uint32_t r;

	if (preempt_state != PREEMPT_NONE) {
		return;                         /* one is in flight: preempt_poll() asks again once it is over */
	}
	preempt_state = PREEMPT_START;
	ring = preempt_next_ring();
	if (ring < 0 || (uint32_t)ring == cur_ring) {
		preempt_state = PREEMPT_FINISH;
		preempt_update_wptr(cur_ring);
		preempt_state = PREEMPT_NONE;
		return;
	}
	r = (uint32_t)ring;
	{
		struct preempt_record *record = (struct preempt_record *)core_record[r].memory;

		record->wptr = *ring_ats[r];
		to_memory((vm_offset_t)record, sizeof(*record));
	}
	/* the core takes this wptr with the record; a later submission sets restore_wptr again */
	restore_wptr[r] = false;
	preempt_keepalive(true);
	Gw64(G8_CP_CONTEXT_SWITCH_SMMU_INFO, gpu_preempt_arg == 2 ? CORE_SMMU_INFO_ADDRESS + 0x1000u * r : 0);
	Gw64(G8_CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR, CORE_RECORD_ADDRESS(r));
	next_ring = r;
	preempt_began = mach_absolute_time();
	preempt_stat.triggered++;
	preempt_state = PREEMPT_TRIGGERED;
	__asm__ volatile ("dsb sy");
	Gw(G8_CP_CONTEXT_SWITCH_CNTL, SWITCH_LEVEL(1u) | SWITCH_USES_GMEM | SWITCH_SKIP_SAVE_RESTORE | SWITCH_STOP);
	(void)G(G8_CP_CONTEXT_SWITCH_CNTL);     /* (arrived before the poll thread, on another core, reads it back) */
}

extern void dada_gpu_report(const char *why);

/*
 * a8xx_preempt_irq without the interrupt, and the a8xx_irq's CACHE_FLUSH_TS -> a8xx_preempt_trigger: with the lock
 * held, every PREEMPT_POLL_US while a switch is in flight or a ring other than the current one has work. Linux takes
 * the CP_SW interrupt as "the switch is over" and a STOP bit still set then as a failure; here the STOP bit going
 * back to 0 is "over", and one still set after PREEMPT_WATCHDOG_S seconds the failure that Linux's preempt timer
 * catches: then nothing is switched or written to WPTR again, the waits for fences run out, and RedwoodGPU does what
 * it does with a core that has stopped (redwood_gpu_report, and dada_gpu_recover(), which has nothing to offer: no
 * reset of the core is written for dada yet; one that is must set both rings up again, as phase_core_start and
 * preempt_setup do). True: look again soon.
 */
static bool
preempt_poll(void)
{
	if (!preempt_on || preempt_halted) {
		return false;
	}
	if (preempt_state == PREEMPT_TRIGGERED) {
		uint64_t now = mach_absolute_time(), ns;
		uint32_t cntl = G(G8_CP_CONTEXT_SWITCH_CNTL);

		absolutetime_to_nanoseconds(now - preempt_began, &ns);
		if (cntl & SWITCH_STOP) {
			if (ns < PREEMPT_WATCHDOG_S * NSEC_PER_SEC) {
				return true;
			}
			preempt_state = PREEMPT_FAULTED;
			preempt_stat.faulted++;
			say("gpu: PREEMPTION FAULTED: the switch from ring %u to ring %u is not over after %u s (cntl %08x, level "
			    "status %08x, int %08x); no more switches\n", cur_ring, next_ring, PREEMPT_WATCHDOG_S, cntl,
			    G(G8_CP_CONTEXT_SWITCH_LEVEL_STATUS), G(G8_RBBM_INT_0_STATUS));
			dada_gpu_report("preemption timed out");
			return false;
		}
		cur_ring = next_ring;
		preempt_state = PREEMPT_FINISH;
		preempt_update_wptr(cur_ring);
		preempt_state = PREEMPT_NONE;
		preempt_keepalive(false);
		preempt_stat.completed++;
		preempt_stat.switching += now - preempt_began;
		if (now - preempt_began > preempt_stat.longest) {
			preempt_stat.longest = now - preempt_began;
		}
		if (gpu_log) {
			redwood_tl("tl: P %u in %llu\n", cur_ring, ns / 1000);
		}
	}
	if (preempt_state != PREEMPT_NONE) {
		return false;                   /* (faulted) */
	}
	/* "retrigger preemption to avoid a deadlock", and the switch back once the current ring has nothing left */
	preempt_trigger();
	if (preempt_state == PREEMPT_TRIGGERED) {
		return true;
	}
	for (uint32_t r = 0; r < RINGS; r++) {
		if (r != cur_ring && !ring_idle(r)) {
			return true;
		}
	}
	return false;
}

static void
preempt_say(void)
{
	uint64_t ns, longest;

	absolutetime_to_nanoseconds(preempt_stat.completed ? preempt_stat.switching / preempt_stat.completed : 0, &ns);
	absolutetime_to_nanoseconds(preempt_stat.longest, &longest);
	say("gpu: preemption, 10 s: %u triggered, %u completed, %u faulted; a switch took %llu us on average, %llu at most; "
	    "submissions %u to ring 0, %u to ring 1 (%u, %u waited for room); now on ring %u%s\n", preempt_stat.triggered,
	    preempt_stat.completed, preempt_stat.faulted, ns / 1000, longest / 1000, preempt_stat.submitted[0],
	    preempt_stat.submitted[1], preempt_stat.full[0], preempt_stat.full[1], cur_ring,
	    preempt_state == PREEMPT_FAULTED ? " (FAULTED: no more switches)" : "");
}

static void
preempt_thread(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint64_t now, ns, deadline;
		bool again;

		lck_mtx_lock(preempt_lock);
		again = preempt_poll();
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now - preempt_stat.since, &ns);
		if (ns >= 10 * NSEC_PER_SEC) {
			if (preempt_stat.submitted[0] + preempt_stat.submitted[1] != 0 || preempt_stat.faulted != 0) {
				preempt_say();
			}
			bzero(&preempt_stat, sizeof(preempt_stat));
			preempt_stat.since = now;
		}
		if (again) {
			nanoseconds_to_absolutetime(PREEMPT_POLL_US * NSEC_PER_USEC, &deadline);
			assert_wait_deadline_with_leeway((event_t)&preempt_poller, THREAD_UNINT, TIMEOUT_URGENCY_SYS_CRITICAL,
			    now + deadline, 0);
		} else {
			assert_wait_timeout((event_t)&preempt_poller, THREAD_UNINT, 10000, NSEC_PER_MSEC);
		}
		lck_mtx_unlock(preempt_lock);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* a6xx_emit_set_pseudo_reg: where the CP saves this ring's state when it is switched away from, and the postamble */
static void
ring_pseudo_regs(uint32_t r)
{
	ring_command(CP_SET_PSEUDO_REG, 12);
	ring(PSEUDO_SMMU_INFO);                 /* "don't save SMMU, we write the record from the kernel instead" */
	ring(0);
	ring(0);
	ring(PSEUDO_NON_SECURE_SAVE_ADDR);      /* privileged and non secure buffer save */
	ring(CORE_RECORD_ADDRESS(r));
	ring(0);
	ring(PSEUDO_NON_PRIV_SAVE_ADDR);        /* user context buffer save, unused by the firmware */
	ring(0);
	ring(0);
	ring(PSEUDO_COUNTER);                   /* 0: off */
	ring(0);
	ring(0);
	ring_command(CP_SET_AMBLE, 3);          /* the postamble, run when switched away from (KMD_AMBLE_TYPE) */
	ring(CORE_POSTAMBLE_ADDRESS);
	ring(0);
	ring(postamble_words | KMD_AMBLE_TYPE << 20);
}

/* a6xx_flush_yield: "yield the floor on command completion" (no address to write, no interrupt) */
static void
ring_yield(uint32_t data)
{
	ring_command(CP_CONTEXT_SWITCH_YIELD, 4);
	ring(0);
	ring(0);
	ring(data);
	ring(0);
}

/* the ring's new wptr to the core if it is the current ring and no switch is in flight (a6xx_flush), else for later */
static void
ring_flush(uint32_t r)
{
	to_memory(ring_blocks[r]->memory, ring_blocks[r]->size);
	if (!preempt_in_flight() && cur_ring == r) {
		Gw(G8_CP_RB_WPTR, *ring_ats[r]);
	} else {
		restore_wptr[r] = true;
	}
}

/*
 * a8xx_patch_pwrup_reglist (and the A830's three lists, a8xx_catalog.c): the registers the command processor writes
 * back after power collapse and preemption, with their values now. ME_INIT was told where this list is (bit 8, "the
 * register init list with the spinlock") and found it empty, which is all it has been without preemption.
 */
static const uint32_t pwrup_ifpc[] = {
	G8_RBBM_NC_MODE_CNTL, G8_RBBM_SLICE_INTERFACE_HANG_INT_CNTL, G8_RBBM_SLICE_NC_MODE_CNTL, G8_SP_NC_MODE_CNTL,
	G8_SP_HLSQ_LPAC_GMEM_RANGE_MIN, G8_SP_HLSQ_LPAC_GMEM_RANGE_MIN + 1, G8_SP_CHICKEN_BITS_1, G8_SP_CHICKEN_BITS_2,
	G8_SP_CHICKEN_BITS_3, G8_SP_PERFCTR_SHADER_MASK, G8_SP_HLSQ_DBG_ECO_CNTL, G8_CP_INTERRUPT_STATUS_MASK_GLOBAL,
	G8_RBBM_PERFCTR_CNTL,
};
static const uint32_t pwrup_preemption[] = {
	G8_UCHE_MODE_CNTL, G8_UCHE_VARB_IDLE_TIMEOUT, G8_UCHE_GBIF_GX_CONFIG, G8_UCHE_CACHE_WAYS, G8_UCHE_CCHE_MODE_CNTL,
	G8_UCHE_CCHE_CACHE_WAYS, G8_UCHE_CCHE_GC_GMEM_RANGE_MIN, G8_UCHE_CCHE_GC_GMEM_RANGE_MIN + 1,
	G8_UCHE_CCHE_LPAC_GMEM_RANGE_MIN, G8_UCHE_CCHE_LPAC_GMEM_RANGE_MIN + 1, G8_UCHE_CCHE_TRAP_BASE,
	G8_UCHE_CCHE_TRAP_BASE + 1, G8_UCHE_CCHE_WRITE_THRU_BASE, G8_UCHE_CCHE_WRITE_THRU_BASE + 1, G8_UCHE_HW_DBG_CNTL,
	G8_UCHE_WRITE_THRU_BASE, G8_UCHE_WRITE_THRU_BASE + 1, G8_UCHE_TRAP_BASE, G8_UCHE_TRAP_BASE + 1, G8_UCHE_CLIENT_PF,
	G8_VSC_KMD_DBG_ECO_CNTL, G8_RB_CMP_NC_MODE_CNTL, G8_SP_HLSQ_TIMEOUT_THRESHOLD_DP, G8_SP_HLSQ_GC_GMEM_RANGE_MIN,
	G8_SP_HLSQ_GC_GMEM_RANGE_MIN + 1, G8_SP_READ_SEL, G8_TPL1_NC_MODE_CNTL, G8_TPL1_DBG_ECO_CNTL, G8_TPL1_DBG_ECO_CNTL1,
};
#define PIPE_AQE0               4
#define PIPE_AQE1               5
#define PIPE_DDE_BR             6
#define ALL_PIPES               (B(PIPE_BR) | B(PIPE_BV) | B(PIPE_LPAC) | B(PIPE_AQE0) | B(PIPE_AQE1) | B(PIPE_DDE_BR) | \
	                         B(PIPE_DDE_BV))
static const struct {
	uint32_t reg, pipes;
} pwrup_dynamic[] = {
	{ G8_CP_HW_FAULT_STATUS_MASK_PIPE, ALL_PIPES },
	{ G8_CP_INTERRUPT_STATUS_MASK_PIPE, ALL_PIPES },
	{ G8_CP_PROTECT_CNTL_PIPE, B(PIPE_BR) | B(PIPE_BV) | B(PIPE_LPAC) },
	{ G8_CP_PROTECT_PIPE(15), B(PIPE_BR) | B(PIPE_BV) | B(PIPE_LPAC) },
	{ G8_GRAS_TSEFE_DBG_ECO_CNTL, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_GRAS_NC_MODE_CNTL, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_GRAS_DBG_ECO_CNTL, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_RB_CCU_CNTL, B(PIPE_BR) },
	{ G8_RB_CCU_DBG_ECO_CNTL, B(PIPE_BR) },
	{ G8_RB_CCU_NC_MODE_CNTL, B(PIPE_BR) },
	{ G8_RB_CMP_NC_MODE_CNTL, B(PIPE_BR) },
	{ G8_RB_RESOLVE_PREFETCH_CNTL, B(PIPE_BR) },
	{ G8_RB_CMP_DBG_ECO_CNTL, B(PIPE_BR) },
	{ G8_RB_GC_GMEM_PROTECT, B(PIPE_BR) },
	{ G8_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE, B(PIPE_BR) },
	{ G8_VPC_FLATSHADE_MODE_CNTL, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_CHICKEN_BITS_1, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_CHICKEN_BITS_2, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_CHICKEN_BITS_3, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_CHICKEN_BITS_4, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_AUTO_VERTEX_STRIDE, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_VIS_STREAM_CNTL, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_PC_CONTEXT_SWITCH_STABILIZE_CNTL_1, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VFD_CB_BV_THRESHOLD, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VFD_CB_BR_THRESHOLD, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VFD_CB_BUSY_REQ_CNT, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VFD_CB_LP_REQ_CNT, B(PIPE_BR) | B(PIPE_BV) },
	{ G8_VFD_DBG_ECO_CNTL, B(PIPE_BR) | B(PIPE_BV) },
};

static void
preempt_pwrup_list(uint32_t *ifpc, uint32_t *preemption, uint32_t *dynamic)
{
	volatile uint32_t *lock = (volatile uint32_t *)core_pwrup.memory;      /* struct cpu_gpu_lock */
	volatile uint32_t *dest = lock + 4;                                    /* its regs[] */

	aperture(PIPE_NONE);
	lock[0] = lock[1] = lock[2] = 0;                                        /* gpu_req, cpu_req, turn */
	*ifpc = sizeof(pwrup_ifpc) / sizeof(pwrup_ifpc[0]) + G8_CP_PROTECT_GLOBAL_LEN;
	for (unsigned i = 0; i < sizeof(pwrup_ifpc) / sizeof(pwrup_ifpc[0]); i++) {
		*dest++ = pwrup_ifpc[i];
		*dest++ = G(pwrup_ifpc[i]);
	}
	for (unsigned i = 0; i < G8_CP_PROTECT_GLOBAL_LEN; i++) {
		*dest++ = G8_CP_PROTECT_GLOBAL(i);
		*dest++ = G(G8_CP_PROTECT_GLOBAL(i));
	}
	*preemption = sizeof(pwrup_preemption) / sizeof(pwrup_preemption[0]) + 20;
	for (unsigned i = 0; i < sizeof(pwrup_preemption) / sizeof(pwrup_preemption[0]); i++) {
		*dest++ = pwrup_preemption[i];
		*dest++ = G(pwrup_preemption[i]);
	}
	for (unsigned i = 0; i < 20; i++) {
		*dest++ = G8_TPL1_BICUBIC_WEIGHTS_TABLE(i);
		*dest++ = G(G8_TPL1_BICUBIC_WEIGHTS_TABLE(i));
	}
	/* (aperture, register, value) for each pipe the register is in; read through slice 0's aperture */
	*dynamic = 0;
	for (uint32_t pipe = PIPE_BR; pipe <= PIPE_DDE_BV; pipe++) {
		for (unsigned i = 0; i < sizeof(pwrup_dynamic) / sizeof(pwrup_dynamic[0]); i++) {
			if (!(pwrup_dynamic[i].pipes & B(pipe))) {
				continue;
			}
			aperture(pipe);
			*dest++ = pipe << 12;
			*dest++ = pwrup_dynamic[i].reg;
			*dest++ = G(pwrup_dynamic[i].reg);
			(*dynamic)++;
		}
	}
	aperture(PIPE_NONE);
	lock[3] = *ifpc | *preemption << 8 | *dynamic << 16;                    /* ifpc_list_len, preemption_list_len, dynamic_list_len */
	to_memory(core_pwrup.memory, core_pwrup.size);
}

/*
 * a6xx_preempt_init + preempt_init_ring, a8xx_preempt_hw_init, the power-up list, and a8xx_preempt_start (the last
 * step of a8xx hw_init, on ring 0: protection off, the pseudo registers, a yield), once the core takes commands and is
 * out of secure mode. Anything failing before the core is told of the second ring leaves one ring, as before
 * (Linux: "preemption init failed, disabling preemption").
 */
static void
preempt_setup(void)
{
	struct cp_smmu_info *info;
	volatile uint32_t *postamble;
	uint32_t ifpc, preemption, dynamic, count = 0;
	thread_t thread;

	say("preemption (dadagpu_preempt=%u): ring 1 at %#x, records at %#x and %#x (%u KB each, privileged), SMMU "
	    "information at %#x (%s), postamble at %#x%s; IB1LIST markers %s\n", gpu_preempt_arg, CORE_RING1_ADDRESS,
	    CORE_RECORD_ADDRESS(0), CORE_RECORD_ADDRESS(1), PREEMPT_RECORD_SIZE / 1024, CORE_SMMU_INFO_ADDRESS,
	    gpu_preempt_arg == 2 ? "given to the core at each switch" : "not given: no table switch", CORE_POSTAMBLE_ADDRESS,
	    gpu_preempt_post ? "" : " (a NOP: dadagpu_preempt_post=0)",
	    gpu_preempt_ib ? "on" : "off (dadagpu_preempt_ib=0: switches between submissions only)");
	if (!block_new(&core_space, &core_ring1, CORE_RING1_ADDRESS, CORE_RING_SIZE, PTE_CORE) ||
	    !block_new(&core_space, &core_smmu_info, CORE_SMMU_INFO_ADDRESS, 0x1000u * RINGS, PTE_GMU | PTE_READ_ONLY) ||
	    !block_new(&core_space, &core_postamble, CORE_POSTAMBLE_ADDRESS, 0x1000u, PTE_GMU | PTE_READ_ONLY) ||
	    !block_new(&core_space, &core_record[0], CORE_RECORD_ADDRESS(0), PREEMPT_RECORD_SIZE, PTE_GMU) ||
	    !block_new(&core_space, &core_record[1], CORE_RECORD_ADDRESS(1), PREEMPT_RECORD_SIZE, PTE_GMU)) {
		say("preemption: no memory for it: one ring, as before\n");
		return;
	}
	space_commit(&core_space);
	preempt_group = lck_grp_alloc_init("dada gpu preemption", LCK_GRP_ATTR_NULL);
	preempt_lock = lck_mtx_alloc_init(preempt_group, LCK_ATTR_NULL);

	for (uint32_t r = 0; r < RINGS; r++) {
		struct preempt_record *record = (struct preempt_record *)core_record[r].memory;

		info = (struct cp_smmu_info *)(core_smmu_info.memory + 0x1000u * r);
		info->magic = SMMU_INFO_MAGIC;
		info->ttbr0 = core_space.tables[0].phys;        /* the one address space's tables */
		info->asid = 0xdecafbad;                        /* (Linux's) */
		info->context_idr = 0;
		record->magic = PREEMPT_RECORD_MAGIC;
		record->info = 0;
		record->data = 0;
		record->rptr = 0;
		record->wptr = 0;
		record->cntl = 12u | 2u << 8;                   /* MSM_GPU_RB_CNTL_DEFAULT, as CP_RB_CNTL */
		record->rbase = r == 0 ? CORE_RING_ADDRESS : CORE_RING1_ADDRESS;
		record->counter = 0;
		record->rptr_addr = CORE_SCRATCH_ADDRESS + RPTR_AT(r);
		record->bv_rptr_addr = CORE_SCRATCH_ADDRESS + RPTR_AT(r) + 0x40;
		to_memory(core_record[r].memory, 4096);
	}
	to_memory(core_smmu_info.memory, core_smmu_info.size);

	/* preempt_prepare_postamble (a8xx): the performance counters' SRAM cleared, and waited for */
	postamble = (volatile uint32_t *)core_postamble.memory;
	postamble[count++] = 7u << 28 | 3 | odd_parity(3) << 15 | CP_REG_RMW << 16 | odd_parity(CP_REG_RMW) << 23;
	postamble[count++] = G8_RBBM_PERFCTR_SRAM_INIT_CMD;
	postamble[count++] = 0;
	postamble[count++] = 1;
	postamble[count++] = 7u << 28 | 6 | odd_parity(6) << 15 | CP_WAIT_REG_MEM << 16 | odd_parity(CP_WAIT_REG_MEM) << 23;
	postamble[count++] = 3;                                 /* WRITE_EQ, of a register */
	postamble[count++] = G8_RBBM_PERFCTR_SRAM_INIT_STATUS;
	postamble[count++] = 0;
	postamble[count++] = 1;                                 /* ref */
	postamble[count++] = 1;                                 /* mask */
	postamble[count++] = 0;                                 /* delay loop cycles */
	postamble_words = count;
	if (!gpu_preempt_post) {
		/* preempt_disable_postamble (Linux: while sysprof is on): "a NOP that covers the whole buffer" */
		postamble[0] = 7u << 28 | (count - 1) | odd_parity(count - 1) << 15 | CP_NOP << 16 | odd_parity(CP_NOP) << 23;
	}
	to_memory(core_postamble.memory, core_postamble.size);

	/* a8xx_preempt_hw_init: each record from the ring as it stands, "no table switch", GMEM save/restore, ring 0 current */
	for (uint32_t r = 0; r < RINGS; r++) {
		struct preempt_record *record = (struct preempt_record *)core_record[r].memory;

		record->wptr = 0;
		record->rptr = 0;
		record->rptr_addr = CORE_SCRATCH_ADDRESS + RPTR_AT(r);
		record->info = 0;
		record->data = 0;
		record->rbase = r == 0 ? CORE_RING_ADDRESS : CORE_RING1_ADDRESS;
		to_memory(core_record[r].memory, 4096);
	}
	Gw64(G8_CP_CONTEXT_SWITCH_SMMU_INFO, 0);
	aperture(PIPE_BR);
	Gw(G8_RB_CONTEXT_SWITCH_GMEM_SAVE_RESTORE_ENABLE, 1);
	aperture(PIPE_NONE);
	preempt_state = PREEMPT_NONE;
	cur_ring = 0;
	ring1_at = 0;

	preempt_pwrup_list(&ifpc, &preemption, &dynamic);
	say("preemption: power-up list: %u IFPC registers, %u preemption registers, %u (pipe, register) pairs\n", ifpc,
	    preemption, dynamic);

	/* a8xx_preempt_start, on ring 0 */
	ring_select(0);
	ring_command(CP_SET_PROTECTED_MODE, 1);
	ring(0);
	ring_pseudo_regs(0);
	ring_yield(0);
	if (!ring_run("take the second ring (a8xx_preempt_start)")) {
		say("preemption: the core did not get through a8xx_preempt_start: one ring, as before (it may be stuck)\n");
		return;
	}
	preempt_stat.since = mach_absolute_time();
	if (kernel_thread_start(preempt_thread, NULL, &thread) != KERN_SUCCESS) {
		say("preemption: no thread to look after it: one ring, as before\n");
		return;
	}
	thread_deallocate(thread);
	preempt_on = true;
	say("TWO RINGS, PREEMPTION ON: ring 0 for backboardd, ring 1 for everybody else (on ring %u; ring 0 rptr %u, its "
	    "shadow %u, wptr %u; ring 1's shadow %u)\n", cur_ring, G(G8_CP_RB_RPTR), scratch_word(RPTR_AT(0)), ring_at,
	    scratch_word(RPTR_AT(1)));
}

/*
 * a7xx_submit with preemption (gpu->nr_rings > 1), to ring `submit_ring`: as dada_gpu_submit below, and also the pseudo
 * registers, the IB1LIST markers around the command buffers (where the core may switch away in the middle of a
 * submission: Linux emits them for a queue with MSM_SUBMITQUEUE_ALLOW_PREEMPT; boot-arg dadagpu_preempt_ib=0 leaves them
 * out, and then the core switches only between submissions), each ring's own fence and timestamps, and a yield at the
 * end; then a8xx_preempt_trigger.
 */
static uint32_t
submit_rings(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t r = submit_ring, wanted = 96 + 4 * count, room = 0, ticket, slot;

	submit_ring = 0;
	lck_mtx_lock(preempt_lock);
	for (int i = 0; i < 20000; i++) {
		room = (ring_rptr(r) + RING_WORDS - *ring_ats[r] - 1) % RING_WORDS;
		if (room >= wanted) {
			break;
		}
		if (i == 0) {
			preempt_stat.full[r]++;
		}
		/* (a ring that is not the current one empties only once the poll thread has switched to it) */
		lck_mtx_unlock(preempt_lock);
		thread_wakeup((event_t)&preempt_poller);
		sleep_us(100);
		lck_mtx_lock(preempt_lock);
	}
	if (room < wanted) {
		lck_mtx_unlock(preempt_lock);
		return 0;
	}
	times_add();
	ticket = fence + 1;
	if ((ticket & 1) != r) {
		ticket++;
	}
	if (ticket == 0) {
		ticket = 2;                     /* (ring 0's after 0xffffffff: 0 names no submission) */
	}
	fence = ticket;
	last_ticket[r] = ticket;
	slot = ring_count[r]++ % TIMES;
	slot_ticket[r][slot] = ticket;
	slot_pid[r][slot] = proc_selfpid();
	preempt_stat.submitted[r]++;

	ring_select(r);
	ring_thread(SYNC_THREADS | THREAD_BR);
	ring_pseudo_regs(r);
	ring_command(CP_REG_TO_MEM, 3);
	ring(G8_CP_ALWAYS_ON_COUNTER | 2u << 18 | 1u << 30);
	ring(CORE_SCRATCH_ADDRESS + RING_TIMES_AT(r) + 16 * slot);
	ring(0);
	ring_thread(THREAD_BOTH);
	ring_command(CP_SET_MARKER, 1);
	ring(MARKER_IFPC_OFF);
	if (gpu_preempt_ib) {
		ring_command(CP_SET_MARKER, 1);
		ring(MARKER_IB1LIST_START);
	}
	for (uint32_t i = 0; i < count; i++) {
		ring_command(CP_INDIRECT_BUFFER, 3);
		ring((uint32_t)addresses[i]);
		ring((uint32_t)(addresses[i] >> 32));
		ring(words[i]);
	}
	if (gpu_preempt_ib) {
		ring_command(CP_SET_MARKER, 1);
		ring(MARKER_IB1LIST_END);
	}
	ring_register(G8_CP_SCRATCH_GLOBAL(2), ticket);
	ring_thread(THREAD_BR);
	ring_command(CP_EVENT_WRITE7, 1);
	ring(CCU_INVALIDATE_DEPTH);
	ring_command(CP_EVENT_WRITE7, 1);
	ring(CCU_INVALIDATE_COLOR);
	ring_thread(THREAD_BV);
	ring_command(CP_EVENT_WRITE7, 4);
	ring(CACHE_CLEAN | 1u << 27);
	ring(CORE_SCRATCH_ADDRESS + RING_FENCE_AT(r) + 8);
	ring(0);
	ring(ticket);
	ring_thread(THREAD_BR);
	ring_command(CP_WAIT_TIMESTAMP, 4);
	ring(0);
	ring(CORE_SCRATCH_ADDRESS + RING_FENCE_AT(r) + 8);
	ring(0);
	ring(ticket);
	ring_command(CP_REG_TO_MEM, 3);
	ring(G8_CP_ALWAYS_ON_COUNTER | 2u << 18 | 1u << 30);
	ring(CORE_SCRATCH_ADDRESS + RING_TIMES_AT(r) + 16 * slot + 8);
	ring(0);
	ring_command(CP_EVENT_WRITE7, 4);
	ring(CACHE_CLEAN | 1u << 27);
	ring(CORE_SCRATCH_ADDRESS + RING_FENCE_AT(r));
	ring(0);
	ring(ticket);
	ring_thread(THREAD_BOTH);
	ring_command(CP_SET_MARKER, 1);
	ring(MARKER_IFPC_ON);
	ring_yield(1);
	ring_select(0);
	ring_flush(r);
	preempt_trigger();
	lck_mtx_unlock(preempt_lock);
	thread_wakeup((event_t)&preempt_poller);
	return ticket;
}

/* Linux's a7xx_submit (which a8xx uses), for one ring and the one address space */
uint32_t
dada_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t wanted = 64 + 4 * count, room = 0;

	if (preempt_on) {
		return submit_rings(addresses, words, count);
	}
	for (int i = 0; i < 20000; i++) {
		room = (G(G8_CP_RB_RPTR) + RING_WORDS - ring_at - 1) % RING_WORDS;
		if (room >= wanted) {
			break;
		}
		sleep_us(100);
	}
	if (room < wanted) {
		return 0;
	}
	times_add();
	if (++fence == 0) {
		fence = 1;
	}
	ring_thread(SYNC_THREADS | THREAD_BR);
	times_pid[fence % TIMES] = proc_selfpid();
	ring_time(fence, 0);
	ring_thread(THREAD_BOTH);
	ring_command(CP_SET_MARKER, 1);
	ring(0x101);                            /* IFPC off */
	for (uint32_t i = 0; i < count; i++) {
		ring_command(CP_INDIRECT_BUFFER, 3);
		ring((uint32_t)addresses[i]);
		ring((uint32_t)(addresses[i] >> 32));
		ring(words[i]);
	}
	ring_register(G8_CP_SCRATCH_GLOBAL(2), fence);
	ring_thread(THREAD_BR);
	ring_command(CP_EVENT_WRITE7, 1);
	ring(CCU_INVALIDATE_DEPTH);
	ring_command(CP_EVENT_WRITE7, 1);
	ring(CCU_INVALIDATE_COLOR);
	ring_thread(THREAD_BV);
	ring_command(CP_EVENT_WRITE7, 4);
	ring(CACHE_CLEAN | 1u << 27);
	ring(CORE_SCRATCH_ADDRESS + BV_FENCE_AT);
	ring(0);
	ring(fence);
	ring_thread(THREAD_BR);
	ring_command(CP_WAIT_TIMESTAMP, 4);
	ring(0);
	ring(CORE_SCRATCH_ADDRESS + BV_FENCE_AT);
	ring(0);
	ring(fence);
	ring_time(fence, 1);
	ring_command(CP_EVENT_WRITE7, 4);
	ring(CACHE_CLEAN | 1u << 27);
	ring(CORE_SCRATCH_ADDRESS + FENCE_AT);
	ring(0);
	ring(fence);
	ring_thread(THREAD_BOTH);
	ring_command(CP_SET_MARKER, 1);
	ring(0x100);                            /* IFPC on (the GMU has it off anyway) */
	to_memory(core_ring.memory, core_ring.size);
	Gw(G8_CP_RB_WPTR, ring_at);
	return fence;
}

void
dada_gpu_report(const char *why)
{
	uint32_t context = smmu_context(&core_space);

	say("gpu: %s: submission %u is the last, done %u (bv %u); rptr %u wptr %u; status %08x int %08x\n", why, fence,
	    *(volatile uint32_t *)(core_scratch.memory + FENCE_AT), *(volatile uint32_t *)(core_scratch.memory + BV_FENCE_AT),
	    G(G8_CP_RB_RPTR), ring_at, G(G8_RBBM_STATUS), G(G8_RBBM_INT_0_STATUS));
	aperture(PIPE_BR);
	say("gpu:   BR: fault %08x int %08x prot %08x ib1 %08x%08x rem %u\n", G(G8_CP_HW_FAULT_STATUS_PIPE),
	    G(G8_CP_INTERRUPT_STATUS_PIPE), G(G8_CP_PROTECT_STATUS_PIPE), G(G8_CP_IB1_BASE + 1), G(G8_CP_IB1_BASE),
	    G(G8_CP_IB1_REM_SIZE));
	aperture(PIPE_BV);
	say("gpu:   BV: fault %08x int %08x prot %08x ib1 %08x%08x rem %u\n", G(G8_CP_HW_FAULT_STATUS_PIPE),
	    G(G8_CP_INTERRUPT_STATUS_PIPE), G(G8_CP_PROTECT_STATUS_PIPE), G(G8_CP_IB1_BASE + 1), G(G8_CP_IB1_BASE),
	    G(G8_CP_IB1_REM_SIZE));
	aperture(PIPE_NONE);
	if (preempt_on) {
		say("gpu:   rings: on %u, switch state %u (to %u), cntl %08x level %08x; ring 0 wptr %u shadow %u fence %u of %u; "
		    "ring 1 wptr %u shadow %u fence %u of %u\n", cur_ring, preempt_state, next_ring,
		    G(G8_CP_CONTEXT_SWITCH_CNTL), G(G8_CP_CONTEXT_SWITCH_LEVEL_STATUS), ring_at, scratch_word(RPTR_AT(0)),
		    scratch_word(RING_FENCE_AT(0)), last_ticket[0], ring1_at, scratch_word(RPTR_AT(1)),
		    scratch_word(RING_FENCE_AT(1)), last_ticket[1]);
	}
	if (space_faulted(&core_space)) {
		wr(smmu + context + SMMU_CB_FSR, SMMU_FSR_ALL);
	}
}

bool
dada_gpu_recover(void)
{
	return false;
}

static bool
all_done(void)
{
	if (preempt_on) {
		return dada_gpu_done(last_ticket[0]) && dada_gpu_done(last_ticket[1]);
	}
	return dada_gpu_done(fence);
}

void
dada_gpu_stop(void)
{
	if (kept) {
		uint32_t waited = 0;

		kept = false;
		/* (with two rings, each ring's last: the last of all says nothing of the other ring's) */
		while (!all_done() && waited < 5000) {
			sleep_us(100);
			waited++;
		}
		if (preempt_on) {
			lck_mtx_lock(preempt_lock);
			preempt_halted = true;          /* (the poll thread touches nothing from here on) */
			lck_mtx_unlock(preempt_lock);
		}
		say("the run is about to end: the GPU is given back (%s)\n", all_done() ? "idle" : "STILL AT WORK");
		gpu_block_stop();
	}
}

/* ---- the way back --------------------------------------------------------------------------------- */

/* nothing of the GPU block may outlive the kernel that started it: its SMMU context points into our memory */
static void
gpu_block_stop(void)
{
	if (core_runs) {
		Gw(G8_CP_SQE_CNTL, 0);
		Gw(G8_GBIF_HALT, 1);
		core_runs = false;
	}
	if (core_powered && gmu_runs) {
		uint32_t empty[2] = { 0, 0 };

		Gw(G8_GMU_HOST2GMU_INTR_SET, 1u << 31);         /* OOB GPU clear */
		sleep_ms(5);
		hfi_send("prepare slumber", HFI_MSG_PREPARE_SLUMBER, empty, 2);
		core_powered = false;
	}
	if (gmu_runs) {
		Gw(G8_GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
		if (!(R(G8_GPU_RSCC_RSC_STATUS0_DRV0) & (1u << 16))) {
			Gw(G8_GMU_RSCC_CONTROL_REQ, 1);
			wait_bits(gpu + RSCC_OFFSET + 4UL * G8_GPU_RSCC_RSC_STATUS0_DRV0, 1u << 16, 1u << 16, 10);
			Gw(G8_GMU_RSCC_CONTROL_REQ, 0);
		}
	}
	if (gmu_space_on) {
		space_off(&gmu_space, GMU_MATCH);
		gmu_space_on = false;
	}
	if (core_space_on) {
		wr(smmu + SMMU_SMR(CORE_LPAC_MATCH), 0);
		wr(smmu + SMMU_S2CR(CORE_LPAC_MATCH), SMMU_S2CR_FAULT);
		space_off(&core_space, CORE_MATCH);
		core_space_on = false;
	}
	if (zap_loaded) {
		uint64_t answer = 0;

		say("stop: the zap shader given up: %d\n", scm(SCM_64 | SCM_SIP | SCM_CALL(2, 6), SCM_ONE_VALUE, PAS_GPU, 0, 0,
		    &answer));
		zap_loaded = false;
	}
	if (rd(gpu + GX_GDSCR_OFFSET) & GDSC_POWER_ON) {
		say("stop: the 3D core keeps its power (gx %08x)\n", rd(gpu + GX_GDSCR_OFFSET));
	}
	if (cx_on() && !cx_off_done) {
		wr(gpucc + CX_GDSCR, rd(gpucc + CX_GDSCR) | GDSC_SW_COLLAPSE);
		cx_off_done = true;
	}
	say("stop: the GPU block given back (cx %08x)\n", rd(gpucc + CX_GDSCR));
}

/* on the way back to the loader, with no time for asking: the GMU out of our memory */
void
hakios_dada_gpu_quiesce(void)
{
	if (gpu == 0) {
		return;
	}
	preempt_halted = true;                  /* (the preemption poll thread: hands off) */
	if (gmu_runs) {
		wr(gpu + 4UL * G8_GMU_CM3_SYSRESET, 1);
		gmu_runs = false;
	}
	if (gmu_space_on) {
		wr(smmu + smmu_context(&gmu_space) + SMMU_CB_SCTLR, 0);
		gmu_space_on = false;
	}
	if (core_runs) {
		wr(gpu + 4UL * G8_CP_SQE_CNTL, 0);
		core_runs = false;
	}
	if (core_space_on) {
		wr(smmu + smmu_context(&core_space) + SMMU_CB_SCTLR, 0);
		core_space_on = false;
	}
}

void
hakios_dada_gpu_init(void)
{
	static bool (*const phases[])(void) = { phase_power, phase_gmu_firmware, phase_gmu_start, phase_core_power,
		phase_core_start, phase_core_commands };
	static const char *const names[] = { "power", "GMU firmware", "GMU start", "3D core power", "3D core start",
		"3D core commands" };
	uint32_t last = 0, keep;

	if (!PE_parse_boot_argn("dadagpu", &last, sizeof(last)) || last == 0) {
		return;
	}
	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	gpucc = ml_io_map(GPUCC_PHYS, GPUCC_SIZE);
	gpu = ml_io_map(GPU_PHYS, GPU_SIZE);
	smmu = ml_io_map(SMMU_PHYS, SMMU_SIZE);
	gpu_speeds_choose();
	say("Adreno 830: phases 1..%u\n", last);
	for (phase = 1; phase <= last && phase <= sizeof(phases) / sizeof(phases[0]); phase++) {
		if (!phases[phase - 1]()) {
			say("phase %u (%s) FAILED\n", phase, names[phase - 1]);
			break;
		}
		say("phase %u (%s) done\n", phase, names[phase - 1]);
	}
	keep = 0;
	PE_parse_boot_argn("dadagpu_keep", &keep, sizeof(keep));
	if (keep && phase > sizeof(phases) / sizeof(phases[0]) && core_runs) {
		va_free[0].start = DADA_ADDRESSES_START;
		va_free[0].size = DADA_ADDRESSES_SIZE;
		va_free_count = 1;
		bzero((void *)core_scratch.memory, 4096);
		to_memory(core_scratch.memory, 4096);
		kept = true;
		if (gpu_preempt_arg != 0) {
			preempt_setup();
		}
		{
			/* the submission path once, with a command buffer of ours: a word written, the fence back */
			volatile uint32_t *ib = (volatile uint32_t *)(core_scratch.memory + 0x2000);
			volatile uint32_t *word = (volatile uint32_t *)(core_scratch.memory + 0x1004);
			uint64_t address = CORE_SCRATCH_ADDRESS + 0x2000;
			uint32_t words = 4, f;

			*word = 0;
			ib[0] = 7u << 28 | 3 | odd_parity(3) << 15 | CP_MEM_WRITE << 16 | odd_parity(CP_MEM_WRITE) << 23;
			ib[1] = CORE_SCRATCH_ADDRESS + 0x1004;
			ib[2] = 0;
			ib[3] = 0x1b0c0de1;
			to_memory(core_scratch.memory, CORE_SCRATCH_SIZE);
			f = dada_gpu_submit(&address, &words, 1);
			for (int i = 0; i < 10000 && !dada_gpu_done(f); i++) {
				sleep_us(100);
			}
			to_memory(core_scratch.memory + 0x1000, 64);
			say("submission %u: %s, the word %08x (%s)\n", f, dada_gpu_done(f) ? "done" : "NOT DONE", *word,
			    *word == 0x1b0c0de1 ? "AN INDIRECT BUFFER RAN" : "not written");
			if (!dada_gpu_done(f)) {
				dada_gpu_report("the first submission");
			}
		}
		say("THE GPU STAYS ON for the system to draw with: the service RedwoodGPU\n");
		RedwoodGPUStart();
		say("done\n");
		return;
	}
	gpu_block_stop();
	say("done\n");
}

/*
 * For the governor (hakios_dada_cpu.c): how long the 3D core has worked in all, in microseconds (each submission's
 * own start and end stamps, as the 5 s lines count them; brought up to date at every submission), the speeds there are,
 * and a new speed for it: a6xx_hfi_set_freq's message to the GMU (GX_BW_PERF_VOTE), which Linux sends whenever
 * devfreq picks another level. The GMU moves the clock and its supply itself.
 */
uint64_t hakios_dada_gpu_busy_us(void);
uint64_t
hakios_dada_gpu_busy_us(void)
{
	return busy_ticks * 10 / 192;
}

uint32_t hakios_dada_gpu_speeds(uint32_t *mhz, uint32_t max);
uint32_t
hakios_dada_gpu_speeds(uint32_t *mhz, uint32_t max)
{
	gpu_speeds_choose();
	for (uint32_t i = 0; i < GPU_SPEEDS && i < max; i++) {
		mhz[i] = gpu_speeds[i].hz / 1000000;
	}
	return GPU_SPEEDS;
}

bool hakios_dada_gpu_set_speed(uint32_t index);
bool
hakios_dada_gpu_set_speed(uint32_t index)
{
	static lck_grp_t *group;
	static lck_mtx_t *lock;
	uint32_t vote[4] = { 0, 1, index, 0 };  /* {header, ack_type blocking, freq index, bw index} (see the first vote) */
	bool ok;

	if (index < 1 || index >= GPU_SPEEDS || !core_powered || !gmu_runs) {
		return false;
	}
	vote[3] = gpu_bus_index(index);         /* a6xx_gmu_set_freq: the OPP's bandwidth's index, with every speed */
	if (lock == NULL) {
		group = lck_grp_alloc_init("dada gpu speed", LCK_GRP_ATTR_NULL);
		lock = lck_mtx_alloc_init(group, LCK_ATTR_NULL);
	}
	lck_mtx_lock(lock);
	hfi_quiet = true;
	ok = hfi_send("a speed", HFI_MSG_GX_BW_PERF_VOTE, vote, 4);
	hfi_quiet = false;
	lck_mtx_unlock(lock);
	return ok;
}
