/*
 * The Xiaomi 15's (dada, SM8750) CPU clocks. The CPUCP firmware sets them; Android asks it over SCMI (Arm's System
 * Control and Management Interface, performance protocol 0x13) on the "cpucp" mailbox: dada's device tree
 * qcom,scmi (mboxes cpucp channel 0 to send), the request area cpu_scp_lpri at 0x17b4e000 (arm,scmi-shmem), the
 * doorbell in cpucp's "tx" registers at 0x17830000 (Linux qcom-cpucp-mbox.c: 0x100 + channel * 8). Nobody asks
 * when this kernel runs, so the cores stay at whatever the boot loader left them at.
 *
 * Done as Data A Live does it for SM8850 (ShueiTsuki/data-a-live wsl/pandora_cpu.c, the same firmware family):
 *   dadacpu=1  a look: the protocol, its domains, their levels and limits, the level now, and each domain's clock
 *              as measured on its first running CPU (a chain of dependent adds)
 *   dadacpu=2  ... and every domain set to its sustained level (the highest that is not a boost level), measured
 *              again: the system's speed until there is a governor
 *   dadacpu=3  ... and the governor (governor_thread below): each domain follows its busiest CPU, the 3D core
 *              follows its own work, and both are held down when the SoC is hot
 *   dadamemlat=1  (with any of these, or alone) CPUCP's own memory governor configured from dada's tree and started
 *              before the governor: DDR, LLCC and DDR's QoS level follow the CPUs (memlat_start below); off by default
 */
#include <kern/clock.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#define say(...) redwood_tl("dcpu: " __VA_ARGS__)
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern processor_t thread_bind(processor_t processor);
extern processor_t cpu_to_processor(int cpu);
extern unsigned int ml_get_cpu_count(void);
extern wait_result_t thread_block(thread_continue_t continuation);

#define CPUCP_TX_PHYS           0x17830000UL
#define SCMI_SHMEM_PHYS         0x17b4e000UL
#define SCMI_PERF               0x13u
#define SCMI_QCOM               0x80u           /* Qualcomm's generic vendor protocol: dt qcom,scmi protocol@80 */

static vm_offset_t mbox, shmem;
static uint32_t token;

static inline uint32_t
rd(vm_offset_t a)
{
	return *(volatile uint32_t *)a;
}

static inline void
wr(vm_offset_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

/*
 * one SCMI command to protocol proto, polled (shared memory: status +4, flags +0x10, length +0x14, header +0x18,
 * payload +0x1c). Every protocol shares the one channel (dt: protocol@80 has no shmem or mboxes of its own), and
 * only cpu_thread -- which becomes the governor -- ever calls this, so commands never overlap.
 */
static int
scmi(uint32_t proto, uint32_t msg, const uint32_t *in, uint32_t nin, uint32_t *out, uint32_t maxout, uint32_t *nout)
{
	uint64_t deadline;
	uint32_t t, len, words;

	*nout = 0;
	clock_interval_to_deadline(30, NSEC_PER_MSEC, &deadline);
	while (!(rd(shmem + 4) & 1)) {
		if (mach_absolute_time() > deadline) {
			return -1;
		}
	}
	t = (++token) & 0x3ff;
	wr(shmem + 4, 0);
	wr(shmem + 0x10, 0);
	wr(shmem + 0x14, 4 + nin * 4);
	wr(shmem + 0x18, (msg & 0xff) | (proto & 0xff) << 10 | t << 18);
	for (uint32_t i = 0; i < nin; i++) {
		wr(shmem + 0x1c + i * 4, in[i]);
	}
	__builtin_arm_dsb(0xf);
	wr(mbox + 0x100 + 4, t | 1);
	wr(mbox + 0x100, t | 1);
	clock_interval_to_deadline(50, NSEC_PER_MSEC, &deadline);
	for (;;) {
		uint32_t st = rd(shmem + 4);

		if ((st & 3) && ((rd(shmem + 0x18) >> 18) & 0x3ff) == t) {
			if (st & 2) {
				return -3;
			}
			break;
		}
		if (mach_absolute_time() > deadline) {
			return -2;
		}
	}
	len = rd(shmem + 0x14);
	words = len >= 4 ? (len - 4) / 4 : 0;
	words = MIN(words, (0x200u - 0x1c) / 4);
	for (uint32_t i = 0; i < words && i < maxout; i++) {
		out[i] = rd(shmem + 0x1c + i * 4);
	}
	*nout = MIN(words, maxout);
	return 0;
}

/*
 * MEMLAT (boot-arg dadamemlat=1). Nothing scales the memory while this kernel runs: DDR, the last-level cache (LLCC) and
 * DDR's QoS level stay where the boot left them, or at the one static vote further down (dadaddr). Linux (Sibi Sankar's
 * "QCOM SCMI Generic Vendor Extensions": PATCH V4 for x1e80100, RFC v7 for Glymur/Hamoa, June 2026) hands that job to
 * CPUCP instead: through Qualcomm's generic vendor protocol (0x80) it describes, once, the memory groups, which CPUs each
 * monitor watches and a CPU-clock -> memory-clock table per monitor, then starts the "MEMLAT" algorithm; from then on
 * CPUCP samples the CPUs' counters every few ms and votes DDR / LLCC / DDR_QOS itself. No thread or polling here.
 *
 * Every vendor command is ext_id (0), the algorithm string "MEMLAT" (0x4d454d4c4154: low word, high word), a param_id
 * and that parameter's payload; the answer is an SCMI status (and for GET_PARAM the value after it). The sequence and
 * the payloads are the reference client's (qcom_scmi_memlat_client.c in V4, scmi-qcom-memlat-devfreq.c in v7):
 *   SET_PARAM 18 COMMON_EV_MAP   { 5 events, hw 0xff, instructions, cycles, const cycles, fe stall, be stall }
 *   for each group:
 *     SET_PARAM 16 SET_MEM_GROUP { CPUs 0xff, hw, 0, 0, name[20] all 0 }
 *     SET_PARAM 19 GRP_EV_MAP    { 3 events, hw, miss, writeback, access, 0, 0 }
 *     for each monitor:
 *       SET_PARAM 17 SET_MONITOR { CPUs, hw, type (1: compute), index, name[20] }
 *       SET_PARAM 23 IPM_CEIL    { hw, index, ceiling }
 *       SET_PARAM 32 MON_FREQ_MAP { hw, index, rows, 10 x (u16 CPU MHz | u16 memory MHz or QoS level << 16) }
 *       SET_PARAM 33/34 SET_MIN_FREQ / SET_MAX_FREQ { hw, index, kHz or level }
 *   SET_PARAM 31 SAMPLE_MS { 4 }, SET_PARAM 39 EFFECTIVE_FREQ_METHOD { 1: cycles over the window }
 *   START_ACTIVITY 36 START_TIMER (no payload)
 * (V4 put kHz in the map's u16 memory column, which cannot hold 547000; v7 sends MHz, and its 10-row map: Hamoa's
 * firmware took both V4's 7 rows and v7's 10, so it reads the row count, not the length.)
 *
 * The groups, monitors, CPU lists and tables are dada's own device tree (qcom,memlat: ddr, llcc, ddrqos; the floors and
 * ceilings are the first and last entries of qcom,dcvs's ddr/llcc/ddrqos freq tables). The counter numbers are CPUCP's:
 * dt qcom,pmu qcom,pmu-events-tbl's fourth column -- ARM event 0x08 instructions -> 2, 0x11 cycles -> 0, 0x17 L2 refill
 * -> 4, Qualcomm's 0x1000 -> 8 -- which is Linux's numbering too (EV_CPU_CYCLES 0, EV_INST_RETIRED 2).
 *
 * What dada's tree does NOT give (Android runs memlat on its own CPUs here: every monitor is "qcom,sampling-enabled"),
 * so taken from Linux's x1e80100/Hamoa values: the IPM ceiling, instructions per miss at or under which a CPU counts as
 * waiting on memory -- 20000000, so in practice every CPU counts and the memory follows the CPUs' clocks through the
 * tables (boot-arg dadamlipm=); the sampling period, 4 ms (dadamlms=); DDR_QOS's hw_type, 2 in the protocol's
 * documentation but 3 in dada's qcom,dcvs (dcvs-hw-type: DDR 0, LLCC 1, L3 2, DDRQOS 3) -- 2 first, 3 if 2 is refused.
 */
#define MEMLAT_ALGO             0x4d454d4c4154ull       /* "MEMLAT" */
#define QCOM_SET_PARAM          0x10u
#define QCOM_GET_PARAM          0x11u
#define QCOM_START_ACTIVITY     0x12u
#define ML_SET_MEM_GROUP        16u
#define ML_SET_MONITOR          17u
#define ML_SET_COMMON_EV_MAP    18u
#define ML_SET_GRP_EV_MAP       19u
#define ML_IPM_CEIL             23u
#define ML_SAMPLE_MS            31u
#define ML_MON_FREQ_MAP         32u
#define ML_SET_MIN_FREQ         33u
#define ML_SET_MAX_FREQ         34u
#define ML_GET_CUR_FREQ         35u
#define ML_START_TIMER          36u
#define ML_EFFECTIVE_FREQ       39u
#define ML_MAP_ROWS             10u             /* v7's MAX_MAP_ENTRIES: the map message always carries this many */
#define ML_NONE                 0xffu           /* no counter */
#define ML_GROUPS               3u

static const struct ml_group {
	const char *name;
	uint32_t hw_type, miss_ev, min, max, mons;
	struct ml_mon {
		const char *name;
		uint32_t cpus;          /* bit n: cpu n (qcom,cpulist phandles 0x10..0x17 are cpu@0..7: cluster0 0-5, cluster1 6-7) */
		uint32_t compute;       /* qcom,compute-mon */
		uint32_t rows;
		uint32_t khz[7][2];     /* qcom,cpufreq-memfreq-tbl: CPU kHz, memory kHz (DDR_QOS: level) */
	} mon[4];
} ml_group[ML_GROUPS] = {
	{ "DDR", 0, 8, 547000, 4761000, 4, {
		  { "gold", 0x3f, 0, 6, { { 748800, 547000 }, { 960000, 1353000 }, { 1363200, 1555000 }, { 1785600, 2092000 },
			{ 2572800, 3187000 }, { 3072000, 3686000 } } },
		  { "prime", 0xc0, 0, 7, { { 614400, 547000 }, { 1017600, 1353000 }, { 1689600, 2092000 }, { 2246400, 3187000 },
			{ 2841600, 3686000 }, { 3513600, 4224000 }, { 4185600, 4761000 } } },
		  { "gold-compute", 0xff, 1, 2, { { 2227200, 547000 }, { 4185600, 2092000 } } },
		  { "prime-latfloor", 0xc0, 0, 2, { { 2841600, 547000 }, { 4185600, 4224000 } } } } },
	{ "LLCC", 1, 4, 350000, 1211000, 3, {
		  { "gold", 0x3f, 0, 4, { { 960000, 350000 }, { 1785600, 533000 }, { 2572800, 806000 }, { 3072000, 933000 } } },
		  { "prime", 0xc0, 0, 6, { { 1017600, 350000 }, { 1689600, 533000 }, { 2246400, 806000 }, { 2841600, 933000 },
			{ 3513600, 1066000 }, { 4185600, 1211000 } } },
		  { "gold-compute", 0xff, 1, 2, { { 2227200, 350000 }, { 4185600, 533000 } } } } },
	{ "DDR_QOS", 2, 8, 0, 1, 3, {
		  { "gold", 0xff, 0, 2, { { 2227200, 0 }, { 4185600, 1 } } },
		  { "prime", 0xc0, 0, 2, { { 1689600, 0 }, { 4185600, 1 } } },
		  { "prime-latfloor", 0xc0, 0, 2, { { 2246400, 0 }, { 4185600, 1 } } } } },
};

static uint32_t memlat_on;                      /* boot-arg dadamemlat */
static bool memlat_running, memlat_readback;
static uint32_t ml_hw[ML_GROUPS];               /* the hw_type CPUCP took for each group */
static bool ml_ok[ML_GROUPS];

/* scmi()'s own failures (-1 channel never free, -2 no answer, -3 channel error) and an empty answer, as statuses */
#define ML_NO_ANSWER(r)         (-100 + (r))
#define ML_EMPTY                (-104)

static const char *
scmi_status(int32_t s)
{
	static const char *const names[] = { "SUCCESS", "NOT_SUPPORTED", "INVALID_PARAMETERS", "DENIED", "NOT_FOUND",
		                             "OUT_OF_RANGE", "BUSY", "COMMS_ERROR", "GENERIC_ERROR", "HARDWARE_ERROR", "PROTOCOL_ERROR" };

	if (s <= 0 && s >= -10) {
		return names[-s];
	}
	switch (s) {
	case ML_NO_ANSWER(-1):
		return "no answer (the channel never came free)";
	case ML_NO_ANSWER(-2):
		return "no answer (timed out)";
	case ML_NO_ANSWER(-3):
		return "channel error";
	case ML_EMPTY:
		return "empty answer";
	default:
		return "unknown status";
	}
}

/* one vendor command for "MEMLAT": its status, and the first word after it in *value */
static int32_t
memlat_cmd(uint32_t msg, uint32_t param, const uint32_t *buf, uint32_t n, uint32_t *value)
{
	uint32_t in[4 + 16], out[4], got = 0;
	int r;

	n = MIN(n, 16u);
	in[0] = 0;                                      /* ext_id */
	in[1] = (uint32_t)MEMLAT_ALGO;
	in[2] = (uint32_t)(MEMLAT_ALGO >> 32);
	in[3] = param;
	for (uint32_t i = 0; i < n; i++) {
		in[4 + i] = buf[i];
	}
	r = scmi(SCMI_QCOM, msg, in, 4 + n, out, 4, &got);
	if (r) {
		return ML_NO_ANSWER(r);
	}
	if (got < 1) {
		return ML_EMPTY;
	}
	if (value) {
		*value = got > 1 ? out[1] : 0;
	}
	return (int32_t)out[0];
}

/* SET_PARAM, logged with its payload */
static int32_t
memlat_set(const char *what, uint32_t param, const uint32_t *buf, uint32_t n)
{
	int32_t s = memlat_cmd(QCOM_SET_PARAM, param, buf, n, NULL);
	char words[160];
	int at = 0;

	words[0] = 0;
	for (uint32_t i = 0; i < n && at < (int)sizeof(words) - 12; i++) {
		at += snprintf(words + at, sizeof(words) - (size_t)at, " %x", buf[i]);
	}
	say("memlat: SET_PARAM %u %s {%s }: %s\n", param, what, words, scmi_status(s));
	return s;
}

/* each configured monitor's vote as CPUCP has it now (GET_PARAM 35 GET_CUR_FREQ { hw, index, 0 }), in one line */
static void
memlat_votes(const char *when)
{
	char line[240];
	int at = 0;

	line[0] = 0;
	for (uint32_t g = 0; g < ML_GROUPS; g++) {
		if (!ml_ok[g] || at >= (int)sizeof(line) - 24) {
			continue;
		}
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %s", ml_group[g].name);
		for (uint32_t m = 0; m < ml_group[g].mons && at < (int)sizeof(line) - 16; m++) {
			uint32_t buf[3] = { ml_hw[g], m, 0 }, v = 0;
			int32_t s = memlat_cmd(QCOM_GET_PARAM, ML_GET_CUR_FREQ, buf, 3, &v);

			if (s == 0) {
				at += snprintf(line + at, sizeof(line) - (size_t)at, " %u", ml_group[g].max > 1 ? v / 1000 : v);
			} else {
				at += snprintf(line + at, sizeof(line) - (size_t)at, " (%d)", s);
				if (s <= ML_NO_ANSWER(0)) {
					memlat_readback = false;        /* (a silent channel would hold the governor up 80 ms a call) */
				}
			}
		}
	}
	say("memlat: votes %s, per monitor (MHz; DDR_QOS level):%s\n", when, line);
}

/* configure CPUCP's memlat from dada's tree and start it; once, from cpu_thread, before the governor */
static void
memlat_start(void)
{
	uint32_t out[4], n = 0, in[1], buf[16], ipm = 20000000, sample_ms = 4, groups = 0, version, method = 1;
	const char *msgs[4] = { "?", "?", "?", "?" };
	char what[48];
	int32_t s;
	int r;

	PE_parse_boot_argn("dadamlipm", &ipm, sizeof(ipm));
	PE_parse_boot_argn("dadamlms", &sample_ms, sizeof(sample_ms));

	/* the protocol: there at all, and which of its commands this firmware has */
	r = scmi(SCMI_QCOM, 0, NULL, 0, out, 4, &n);
	if (r || n < 2 || out[0] != 0) {
		say("memlat: vendor protocol 0x80: no version (%d, %u words, status %s); memlat not started\n", r, n,
		    n ? scmi_status((int32_t)out[0]) : "-");
		return;
	}
	version = out[1];
	r = scmi(SCMI_QCOM, 1, NULL, 0, out, 4, &n);
	say("memlat: vendor protocol 0x80 version %#x (Linux knows 0x10000), attributes %#x (%d, status %s: %u algorithm "
	    "strings, %u agents)\n", version, n > 1 ? out[1] : 0, r, n ? scmi_status((int32_t)out[0]) : "-",
	    n > 1 ? out[1] & 0xff : 0, n > 1 ? (out[1] >> 8) & 0xff : 0);
	for (uint32_t m = 0; m < 4; m++) {
		in[0] = QCOM_SET_PARAM + m;
		r = scmi(SCMI_QCOM, 2, in, 1, out, 4, &n);
		msgs[m] = r ? scmi_status(ML_NO_ANSWER(r)) : n ? scmi_status((int32_t)out[0]) : "empty answer";
	}
	say("memlat: PROTOCOL_MESSAGE_ATTRIBUTES: SET_PARAM %s, GET_PARAM %s, START_ACTIVITY %s, STOP_ACTIVITY %s\n",
	    msgs[0], msgs[1], msgs[2], msgs[3]);

	/* the counters every sample reads on every CPU: instructions 2, cycles 0, the rest none (V4's choice) */
	buf[0] = 5;
	buf[1] = ML_NONE;
	buf[2] = 2;
	buf[3] = 0;
	buf[4] = buf[5] = buf[6] = ML_NONE;
	if (memlat_set("COMMON_EV_MAP", ML_SET_COMMON_EV_MAP, buf, 7) != 0) {
		say("memlat: CPUCP took no common events; memlat not started\n");
		return;
	}

	for (uint32_t g = 0; g < ML_GROUPS; g++) {
		const struct ml_group *grp = &ml_group[g];

		ml_hw[g] = grp->hw_type;
		for (;;) {
			memset(buf, 0, sizeof(buf));
			buf[0] = 0xff;                  /* every CPU may feed the group (v7: cpu_possible_mask) */
			buf[1] = ml_hw[g];
			snprintf(what, sizeof(what), "SET_MEM_GROUP %s hw %u", grp->name, ml_hw[g]);
			s = memlat_set(what, ML_SET_MEM_GROUP, buf, 9);
			if (s != 0 && g == 2 && ml_hw[g] == 2) {
				ml_hw[g] = 3;           /* (dada's dcvs-hw-type for DDRQOS) */
				continue;
			}
			break;
		}
		if (s != 0) {
			continue;
		}
		buf[0] = 3;
		buf[1] = ml_hw[g];
		buf[2] = grp->miss_ev;
		buf[3] = buf[4] = ML_NONE;              /* no writeback or access counters */
		buf[5] = buf[6] = 0;
		snprintf(what, sizeof(what), "GRP_EV_MAP %s", grp->name);
		if (memlat_set(what, ML_SET_GRP_EV_MAP, buf, 7) != 0) {
			continue;                       /* (a group without its miss counter cannot vote) */
		}
		ml_ok[g] = true;
		groups++;
		for (uint32_t m = 0; m < grp->mons; m++) {
			const struct ml_mon *mon = &grp->mon[m];
			char name[20] = { 0 };

			buf[0] = mon->cpus;
			buf[1] = ml_hw[g];
			buf[2] = mon->compute;
			buf[3] = m;
			strlcpy(name, mon->name, sizeof(name));
			memcpy(&buf[4], name, sizeof(name));
			snprintf(what, sizeof(what), "SET_MONITOR %s %s", grp->name, mon->name);
			if (memlat_set(what, ML_SET_MONITOR, buf, 9) != 0) {
				continue;
			}
			buf[0] = ml_hw[g];
			buf[1] = m;
			buf[2] = mon->compute ? 0 : ipm;
			snprintf(what, sizeof(what), "IPM_CEIL %s %s", grp->name, mon->name);
			(void)memlat_set(what, ML_IPM_CEIL, buf, 3);
			memset(buf, 0, sizeof(buf));
			buf[0] = ml_hw[g];
			buf[1] = m;
			buf[2] = mon->rows;
			for (uint32_t i = 0; i < mon->rows && i < ML_MAP_ROWS; i++) {
				uint32_t mem = grp->max > 1 ? mon->khz[i][1] / 1000 : mon->khz[i][1];

				buf[3 + i] = (mon->khz[i][0] / 1000) | mem << 16;
			}
			snprintf(what, sizeof(what), "MON_FREQ_MAP %s %s", grp->name, mon->name);
			(void)memlat_set(what, ML_MON_FREQ_MAP, buf, 3 + ML_MAP_ROWS);
			buf[0] = ml_hw[g];
			buf[1] = m;
			buf[2] = grp->min;
			snprintf(what, sizeof(what), "SET_MIN_FREQ %s %s", grp->name, mon->name);
			(void)memlat_set(what, ML_SET_MIN_FREQ, buf, 3);
			buf[2] = grp->max;
			snprintf(what, sizeof(what), "SET_MAX_FREQ %s %s", grp->name, mon->name);
			(void)memlat_set(what, ML_SET_MAX_FREQ, buf, 3);
		}
	}
	if (groups == 0) {
		say("memlat: CPUCP took no memory group; memlat not started\n");
		return;
	}
	(void)memlat_set("SAMPLE_MS", ML_SAMPLE_MS, &sample_ms, 1);
	(void)memlat_set("EFFECTIVE_FREQ_METHOD", ML_EFFECTIVE_FREQ, &method, 1);
	s = memlat_cmd(QCOM_START_ACTIVITY, ML_START_TIMER, NULL, 0, NULL);
	say("memlat: START_ACTIVITY %u START_TIMER: %s; %u of %u groups configured\n", ML_START_TIMER, scmi_status(s),
	    groups, ML_GROUPS);
	memlat_running = s == 0;
	if (memlat_running) {
		memlat_readback = true;
		delay_for_interval(1000, NSEC_PER_MSEC);
		memlat_votes("1 s after the start");
	}
}

static uint32_t
clock_here_mhz(void)
{
	uint64_t span, start, now, x = 1, rounds = 0, ns;

	nanoseconds_to_absolutetime(10 * NSEC_PER_MSEC, &span);
	start = mach_absolute_time();
	do {
		for (int i = 0; i < 1000; i++) {
			__asm__ volatile ("add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                          "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                          "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n"
                                          "add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n add %0, %0, #1\n" : "+r"(x));
		}
		rounds += 1000;
		now = mach_absolute_time();
	} while (now - start < span);
	absolutetime_to_nanoseconds(now - start, &ns);
	return ns ? (uint32_t)(rounds * 16 * 1000 / ns) : 0;
}

static uint32_t
clock_on(int cpu)
{
	processor_t p = cpu_to_processor(cpu);
	uint32_t mhz;

	if (p == PROCESSOR_NULL) {
		return 0;
	}
	thread_bind(p);
	thread_block(THREAD_CONTINUE_NULL);
	mhz = clock_here_mhz();
	thread_bind(PROCESSOR_NULL);
	thread_block(THREAD_CONTINUE_NULL);
	return mhz;
}

static struct {
	uint32_t levels, sustained_khz, top_sustained, now;
	uint32_t khz[64];
} dom[4];
static uint32_t domains;

static void
clocks(const char *when)
{
	char line[200];
	int at = 0;

	for (unsigned c = 0; c < (unsigned)ml_get_cpu_count() && c < 5; c++) {   /* (the prime cores, 5 and 6, may not be running: a bind to one would wait for ever) */
		at += snprintf(line + at, sizeof(line) - (size_t)at, " cpu%u %u", c, clock_on((int)c));
	}
	line[at] = 0;
	say("%s: MHz (dependent adds, about the clock):%s\n", when, line);
}

static void governor_thread(void *arg, wait_result_t w);

static void
cpu_thread(void *arg, wait_result_t w __unused)
{
	uint32_t stage = (uint32_t)(uintptr_t)arg, out[64], n = 0, in[3], version;
	int r;

	delay_for_interval(15000, NSEC_PER_MSEC);
	mbox = ml_io_map(CPUCP_TX_PHYS, 0x1000);
	shmem = ml_io_map(SCMI_SHMEM_PHYS, 0x1000);
	if (stage == 0) {
		memlat_start();                                 /* (dadamemlat=1 alone: memlat and nothing else) */
		return;
	}
	clocks("as the boot left them");
	r = scmi(SCMI_PERF, 0, NULL, 0, out, 64, &n);
	if (r || n < 2) {
		say("SCMI performance protocol: no answer (%d, %u words; channel status %#x)\n", r, n, rd(shmem + 4));
		return;
	}
	version = out[1];
	r = scmi(SCMI_PERF, 1, NULL, 0, out, 64, &n);
	domains = (!r && n >= 2) ? MIN(out[1] & 0xffff, 4u) : 0;
	say("SCMI performance protocol %#x: %u domains\n", version, domains);
	for (uint32_t d = 0; d < domains; d++) {
		uint32_t index = 0, entry = (version >> 16) >= 4 ? 5 : 3;
		char name[17] = { 0 };

		in[0] = d;
		r = scmi(SCMI_PERF, 3, in, 1, out, 64, &n);
		if (!r && n >= 9) {
			memcpy(name, &out[5], 16);
		}
		dom[d].sustained_khz = n > 3 ? out[3] : 0;
		for (int guard = 0; guard < 64; guard++) {
			in[0] = d;
			in[1] = index;
			r = scmi(SCMI_PERF, 4, in, 2, out, 64, &n);
			if (r || n < 2 || out[0] != 0) {
				break;
			}
			uint32_t got = out[1] & 0xfff, left = out[1] >> 16;

			for (uint32_t i = 0; i < got && 2 + (i + 1) * entry <= n && index + i < 64; i++) {
				const uint32_t *e = &out[2 + i * entry];

				dom[d].khz[index + i] = entry == 5 ? e[3] : e[0];
			}
			index += got;
			if (!left || !got) {
				break;
			}
		}
		dom[d].levels = MIN(index, 64u);
		dom[d].top_sustained = 0;
		for (uint32_t l = 0; l < dom[d].levels; l++) {
			if (dom[d].khz[l] <= dom[d].sustained_khz || dom[d].sustained_khz == 0) {
				dom[d].top_sustained = l;
			}
		}
		in[0] = d;
		r = scmi(SCMI_PERF, 8, in, 1, out, 64, &n);
		dom[d].now = (!r && n > 1) ? out[1] : ~0u;
		say("domain %u '%s': %u levels, %u .. %u kHz, sustained %u kHz (level %u), level now %d\n", d, name,
		    dom[d].levels, dom[d].khz[0], dom[d].levels ? dom[d].khz[dom[d].levels - 1] : 0, dom[d].sustained_khz,
		    dom[d].top_sustained, (int)dom[d].now);
	}
	if (memlat_on) {
		memlat_start();
	}
	if (stage < 2) {
		return;
	}
	for (uint32_t d = 0; d < domains; d++) {
		in[0] = d;
		in[1] = dom[d].top_sustained;
		r = scmi(SCMI_PERF, 7, in, 2, out, 8, &n);
		in[0] = d;
		(void)scmi(SCMI_PERF, 8, in, 1, out + 4, 4, &n);
		say("domain %u asked for level %u (%u kHz): %d/%d, level now %d\n", d, dom[d].top_sustained,
		    dom[d].khz[dom[d].top_sustained], r, (int)out[0], (int)out[5]);
	}
	delay_for_interval(50, NSEC_PER_MSEC);
	clocks("at the sustained levels");
	/*
	 * The memory's bandwidth: nobody votes for it while this kernel runs (the GPU's own bus table has a single
	 * "off" level), so DDR stays at its floor and every full-screen pass of the compositor is held up by it --
	 * 2 ms a submission whatever the GPU's clock. A BCM vote (Linux BCM_TCS_CMD: commit, valid, AB in [27:14],
	 * IB in [13:0], in the BCM's own units) for the memory controller (MC0) and the shared hub (SH0); boot-arg
	 * dadaddr=N, IB (default 0x1000; 0: none). With memlat running CPUCP votes the memory itself, so there is no
	 * static vote unless dadaddr= is given (then it is a floor: a BCM takes the highest of its voters). If memlat was
	 * asked for but did not start, the static vote stays, so the memory is never left at its floor.
	 */
	{
		extern bool hakios_rpmh_write(const char *name, uint32_t value);
		uint32_t ib = 0x1000;

		if (!PE_parse_boot_argn("dadaddr", &ib, sizeof(ib)) && memlat_running) {
			say("memory bandwidth: memlat votes it, no static vote (dadaddr= for one)\n");
			ib = 0;
		}
		if (ib != 0) {
			uint32_t v = 1u << 30 | 1u << 29 | (ib & 0x3fff) << 14 | (ib & 0x3fff);

			say("memory bandwidth: MC0 %s, SH0 %s (vote %#x)\n", hakios_rpmh_write("MC0", v) ? "voted" : "NOT voted",
			    hakios_rpmh_write("SH0", v) ? "voted" : "NOT voted", v);
		}
	}
	if (stage >= 3) {
		governor_thread(NULL, THREAD_AWAKENED);         /* (this thread becomes the governor) */
	}
}

/*
 * THE GOVERNOR (dadacpu=3). With every core at its top sustained level and the 3D core at 734 MHz for good (dadacpu=2),
 * the phone ran hot in the hand. As Data A Live does it for SM8850 (pandora_cpu.c, patches 0201-0213), which is
 * Linux's schedutil:
 *
 *   CPU   every 20 ms, each domain's busiest CPU (idle time from the scheduler's own stamps: hakios_cpu_idle.c), and
 *         the lowest level that gives 1.25 x what it used at the clock it ran at; nearly always busy (over 85 %), at
 *         least twice that clock. Up at once, down when four windows in a row want less. Never above the sustained
 *         level (the boost levels are left alone) nor the thermal cap.
 *   GPU   every 20 ms the share of the window the 3D core spent on submissions (hakios_dada_gpu.c); over 75 % a speed
 *         up, under 35 % for ten windows a speed down; between 342 MHz and 832 MHz, and the thermal cap.
 *   heat  every 100 ms the hottest of the SoC's 47 sensors (qcom,tsens-v2: four controllers, DT tsens0-3; sensor S's
 *         reading at TM + 0xa0 + 4 S, bits 0-11 signed tenths of a degree, bit 21 valid). Over 85 C every cap one level
 *         down, under 75 C one back up; over 95 C every cap straight to half. Android's own trips are near 95-105 C;
 *         these are lower, for a phone that is held.
 *
 * Every 2 s: each domain's level and clock, its busiest CPU, the GPU's speed and busyness, the hottest sensor, the caps.
 */
extern uint64_t hakios_cpu_idle_mach(int cpu);
extern int hakios_cpu_cluster(int cpu);
extern uint64_t hakios_dada_gpu_busy_us(void);
extern uint32_t hakios_dada_gpu_speeds(uint32_t *mhz, uint32_t max);
extern bool hakios_dada_gpu_set_speed(uint32_t index);

#define WINDOW_MS               20
#define MAX_CPUS                8
#define GPU_LOW                 3               /* 342 MHz: the floor, so a frame that comes is not drawn slowly */
#define GPU_HIGH                10              /* 832 MHz (dadatherm=0; split: the table's fastest, gpu_high) */
#define HOT                     850             /* tenths of a degree */
#define COOL                    750
#define TOO_HOT                 950
/*
 * ... unless boot-args dadahot=, dadacool=, dadatoohot= (tenths of a degree) say otherwise: the hottest of 47 sensors is a
 * core's junction, at 80-90 C whenever the phone works, so with 85/75 the GPU sat at 342-443 MHz through most of a run
 * (g2k34). hot_where says which sensor it was (controller * 100 + sensor), in the 2 s line.
 */
static int32_t hot = HOT, cool = COOL, too_hot = TOO_HOT;
static uint32_t hot_where;
/*
 * dadatherm=1 (the default): each domain by its own sensors and at the limits Qualcomm's device tree gives this SoC (dt
 * thermal-zones, thermal-hal-config): the 3D core by gpuss-0..7 (tsens2 sensors 1-8), passive from 95 C; the CPU domains by
 * their cores' and cluster's sensors (tsens0 1-14: the six, tsens1 1-6: the two), passive from 120 C there -- here from 105,
 * the hardware's own limits (LMh) act on the CPUs too. Over 110 C anywhere every cap halves, as before. The thresholds of
 * the 3D core are hot/cool/too_hot (95/90/105 C unless the boot-args say otherwise), the CPUs' ten degrees higher.
 * dadatherm=0: the hottest of all the sensors for everything, at 85/75/95 C (before 29 October).
 */
static uint32_t therm_split = 1;
/*
 * dadaboost=MS (default 1500): for that long after a finger touches the screen, the 3D core is kept at gpu_boost (default 9,
 * 734 MHz) or above and every CPU domain at its top sustained level -- within the heat caps. The governor otherwise climbs one
 * speed per 20 ms window once the GPU is busy, and the first frames of a swipe met it at 342-443 MHz: the stutter as a finger
 * starts to move (Android's touch boost does the same). 0: off.
 */
extern uint64_t redwood_touch_last;
static uint32_t touch_boost_ms = 1500, gpu_boost = 9;
static int32_t any_too_hot = 950;   /* any sensor over this halves every cap (dadaanyhot, tenths; was 110 C) */
#define ANY_TOO_HOT             any_too_hot

/* the hottest valid sensor of controller c, sensors from..to, in tenths of a degree, or INT32_MIN */
static int32_t hottest_of(unsigned c, uint32_t from, uint32_t to);

static const uint64_t tsens_tm[] = { 0x0c228000, 0x0c229000, 0x0c22a000, 0x0c22b000 };
static const uint32_t tsens_count[] = { 15, 7, 16, 9 };
static vm_offset_t tsens_va[4];

static int32_t
hottest_of(unsigned c, uint32_t from, uint32_t to)
{
	int32_t best = INT32_MIN;

	if (tsens_va[c] == 0) {
		tsens_va[c] = ml_io_map(tsens_tm[c], 0x1000);
	}
	for (uint32_t s = from; tsens_va[c] != 0 && s <= to && s < tsens_count[c]; s++) {
		uint32_t v = rd(tsens_va[c] + 0xa0 + 4 * s);
		int32_t t = (int32_t)(v << 20) >> 20;

		if ((v & (1u << 21)) && t > best && t < 1500) {
			best = t;
		}
	}
	return best;
}

/* the hottest valid sensor, in tenths of a degree, or INT32_MIN */
static int32_t
hottest(void)
{
	int32_t best = INT32_MIN;

	for (unsigned c = 0; c < 4; c++) {
		if (tsens_va[c] == 0) {
			tsens_va[c] = ml_io_map(tsens_tm[c], 0x1000);
		}
		for (uint32_t s = 0; tsens_va[c] != 0 && s < tsens_count[c]; s++) {
			uint32_t v = rd(tsens_va[c] + 0xa0 + 4 * s);
			int32_t t = (int32_t)(v << 20) >> 20;

			if ((v & (1u << 21)) && t > best && t < 1500) {
				best = t;
				hot_where = c * 100 + s;
			}
		}
	}
	return best;
}

static bool
set_level(uint32_t d, uint32_t level)
{
	uint32_t in[2] = { d, level }, out[4], n = 0;

	return scmi(SCMI_PERF, 7, in, 2, out, 4, &n) == 0 && n >= 1 && out[0] == 0;
}

static void
governor_thread(void *arg __unused, wait_result_t w __unused)
{
	uint64_t idle_was[MAX_CPUS] = { 0 }, at_was, gpu_was, window;
	uint32_t level[4], cap[4], down[4] = { 0 }, cpus = MIN(ml_get_cpu_count(), (unsigned)MAX_CPUS);
	uint32_t gpu_mhz[16] = { 0 }, gpu_speeds = hakios_dada_gpu_speeds(gpu_mhz, 16), gpu = 9, gpu_cap = GPU_HIGH;
	uint32_t gpu_down = 0, ticks = 0, busy_shown[4] = { 0 }, gpu_busy_shown = 0, cpu_busy_sum[MAX_CPUS] = { 0 };
	int cluster[MAX_CPUS];
	int32_t temp = INT32_MIN, gpu_temp = INT32_MIN;
	uint32_t gpu_high = GPU_HIGH;

	nanoseconds_to_absolutetime((uint64_t)WINDOW_MS * NSEC_PER_MSEC, &window);
	PE_parse_boot_argn("dadatherm", &therm_split, sizeof(therm_split));
	PE_parse_boot_argn("dadaboost", &touch_boost_ms, sizeof(touch_boost_ms));
	PE_parse_boot_argn("dadaboost_gpu", &gpu_boost, sizeof(gpu_boost));
	if (therm_split) {
		gpu_high = gpu_speeds > 1 ? gpu_speeds - 1 : GPU_HIGH;
		gpu_cap = gpu_high;
		hot = 750;              /* the GPU's; the CPU domains' are 10 C higher (the phone ran too hot at 95/105, 30 Oct) */
		cool = 700;
		too_hot = 850;
	}
	PE_parse_boot_argn("dadaanyhot", &any_too_hot, sizeof(any_too_hot));
	PE_parse_boot_argn("dadahot", &hot, sizeof(hot));
	PE_parse_boot_argn("dadacool", &cool, sizeof(cool));
	PE_parse_boot_argn("dadatoohot", &too_hot, sizeof(too_hot));
	for (uint32_t c = 0; c < cpus; c++) {
		cluster[c] = hakios_cpu_cluster((int)c);
		idle_was[c] = hakios_cpu_idle_mach((int)c);
	}
	/* frequency ceilings, for heat: every CPU domain at most dadacpu_khz (0: none), the GPU at most dadagpu_mhz */
	{
		uint32_t max_khz = 2400000, max_mhz = 660;

		PE_parse_boot_argn("dadacpu_khz", &max_khz, sizeof(max_khz));
		PE_parse_boot_argn("dadagpu_mhz", &max_mhz, sizeof(max_mhz));
		for (uint32_t d = 0; max_khz != 0 && d < domains; d++) {
			while (dom[d].top_sustained > 0 && dom[d].khz[dom[d].top_sustained] > max_khz) {
				dom[d].top_sustained--;
			}
		}
		while (max_mhz != 0 && gpu_high > GPU_LOW && gpu_mhz[gpu_high] > max_mhz) {
			gpu_high--;
		}
		gpu_cap = MIN(gpu_cap, gpu_high);
		gpu = MIN(gpu, gpu_cap);
		(void)hakios_dada_gpu_set_speed(gpu);  /* (a dadagpu_speed= start may be above the ceiling) */
		say("the governor: ceilings: CPU %u kHz, GPU %u MHz (dadacpu_khz, dadagpu_mhz)\n", max_khz, gpu_mhz[gpu_high]);
	}
	for (uint32_t d = 0; d < domains; d++) {
		level[d] = dom[d].top_sustained;
		cap[d] = dom[d].top_sustained;
	}
	at_was = mach_absolute_time();
	gpu_was = hakios_dada_gpu_busy_us();
	say("the governor: heat caps over %d, back under %d, halved over %d (tenths of a degree)\n", hot, cool, too_hot);
	say("the governor: %u CPUs (clusters %d %d %d %d %d %d %d %d), %u domains, GPU %u speeds\n", cpus, cluster[0],
	    cluster[1], cluster[2], cluster[3], cluster[4], cluster[5], cluster[6], cluster[7], domains, gpu_speeds);
	for (;;) {
		uint64_t now, elapsed, gpu_now;
		uint32_t busy[4] = { 0 }, gpu_busy;

		delay_for_interval(WINDOW_MS, NSEC_PER_MSEC);
		now = mach_absolute_time();
		elapsed = now - at_was;
		if (elapsed < window / 2) {
			continue;
		}
		/* each domain's busiest CPU, in thousandths */
		for (uint32_t c = 0; c < cpus; c++) {
			uint64_t idle = hakios_cpu_idle_mach((int)c), spent = idle - idle_was[c];
			uint32_t b = spent >= elapsed ? 0 : (uint32_t)((elapsed - spent) * 1000 / elapsed);

			idle_was[c] = idle;
			cpu_busy_sum[c] += b;
			if (cluster[c] >= 0 && (uint32_t)cluster[c] < domains && b > busy[cluster[c]]) {
				busy[cluster[c]] = b;
			}
		}
		gpu_now = hakios_dada_gpu_busy_us();
		{
			uint64_t ns;

			absolutetime_to_nanoseconds(elapsed, &ns);
			gpu_busy = ns ? (uint32_t)MIN((gpu_now - gpu_was) * 1000000 / ns, 1000ull) : 0;
		}
		gpu_was = gpu_now;
		at_was = now;

		/* heat, every fifth window */
		if (ticks % 5 == 0) {
			temp = hottest();
			for (uint32_t d = 0; d < domains; d++) {
				/* (split: the six by tsens0 1-14, the two by tsens1 1-6; more domains go by everything) */
				int32_t t = !therm_split ? temp : d == 0 ? hottest_of(0, 1, 14) : d == 1 ? hottest_of(1, 1, 6) : temp;
				int32_t h = therm_split ? hot + 100 : hot, k = therm_split ? cool + 100 : cool;
				int32_t th = therm_split ? too_hot + 100 : too_hot;

				if (t > th || (therm_split && temp > ANY_TOO_HOT)) {
					cap[d] = MIN(cap[d], dom[d].top_sustained / 2);
				} else if (t > h && cap[d] > 0) {
					cap[d]--;
				} else if (t < k && cap[d] < dom[d].top_sustained) {
					cap[d]++;
				}
			}
			gpu_temp = therm_split ? hottest_of(2, 1, 8) : temp;
			if (gpu_temp > too_hot || (therm_split && temp > ANY_TOO_HOT)) {
				gpu_cap = GPU_LOW;
			} else if (gpu_temp > hot && gpu_cap > GPU_LOW) {
				gpu_cap--;
			} else if (gpu_temp < cool && gpu_cap < gpu_high) {
				gpu_cap++;
			}
		}

		/* touch boost (dadaboost) */
		bool boosted = false;
		if (touch_boost_ms != 0 && redwood_touch_last != 0) {
			uint64_t since;

			absolutetime_to_nanoseconds(now - redwood_touch_last, &since);
			boosted = since < (uint64_t)touch_boost_ms * NSEC_PER_MSEC;
		}

		/* the CPU domains */
		for (uint32_t d = 0; d < domains; d++) {
			uint64_t want_khz = (uint64_t)dom[d].khz[level[d]] * busy[d] * 125 / 100000;
			uint32_t want = 0;

			if (busy[d] > 850) {
				want_khz = MAX(want_khz, 2ull * dom[d].khz[level[d]]);
			}
			while (want < dom[d].top_sustained && dom[d].khz[want] < want_khz) {
				want++;
			}
			if (boosted) {
				want = dom[d].top_sustained;
			}
			want = MIN(want, cap[d]);
			if (want > level[d] || level[d] > cap[d]) {
				down[d] = 0;
			} else if (want < level[d] && ++down[d] >= 4) {
				down[d] = 0;
			} else {
				want = level[d];
			}
			if (want != level[d] && set_level(d, want)) {
				level[d] = want;
			}
			busy_shown[d] = MAX(busy_shown[d], busy[d]);
		}

		/* the 3D core */
		{
			uint32_t want = gpu;

			if (gpu_busy > 750 && gpu < gpu_cap) {
				want = gpu + 1;
				gpu_down = 0;
			} else if (gpu_busy < 350 && gpu > GPU_LOW) {
				if (++gpu_down >= 10) {
					want = gpu - 1;
					gpu_down = 0;
				}
			} else {
				gpu_down = 0;
			}
			if (boosted && want < gpu_boost && gpu_boost < gpu_speeds) {
				want = gpu_boost;
				gpu_down = 0;
			}
			want = MIN(want, gpu_cap);
			if (want != gpu && hakios_dada_gpu_set_speed(want)) {
				gpu = want;
			}
			gpu_busy_shown = MAX(gpu_busy_shown, gpu_busy);
		}

		if (++ticks % (2000 / WINDOW_MS) == 0) {
			say("governor: cpu %u kHz (level %u/%u, busiest %u%%) prime %u kHz (level %u/%u, busiest %u%%); GPU %u MHz "
			    "(cap %u MHz, busiest %u%%); hottest %d.%d C (sensor %u), the GPU's %d C\n", dom[0].khz[level[0]], level[0], cap[0],
			    busy_shown[0] / 10, domains > 1 ? dom[1].khz[level[1]] : 0, domains > 1 ? level[1] : 0,
			    domains > 1 ? cap[1] : 0, busy_shown[1] / 10, gpu_mhz[gpu], gpu_mhz[gpu_cap], gpu_busy_shown / 10,
			    temp / 10, (temp < 0 ? -temp : temp) % 10, hot_where, gpu_temp / 10);
			{
				char each[96];
				int at = 0;

				for (uint32_t c = 0; c < cpus; c++) {
					at += snprintf(each + at, sizeof(each) - (size_t)at, " %u", cpu_busy_sum[c] / (2000 / WINDOW_MS) / 10);
					cpu_busy_sum[c] = 0;
				}
				say("governor: each CPU busy %% over 2 s:%s\n", each);
			}
			if (memlat_running && memlat_readback) {
				memlat_votes("now");
			}
			busy_shown[0] = busy_shown[1] = busy_shown[2] = busy_shown[3] = 0;
			gpu_busy_shown = 0;
		}
	}
}

void hakios_dada_cpu_init(void);

void
hakios_dada_cpu_init(void)
{
	uint32_t stage = 0;
	thread_t thread;

	(void)PE_parse_boot_argn("dadacpu", &stage, sizeof(stage));
	(void)PE_parse_boot_argn("dadamemlat", &memlat_on, sizeof(memlat_on));
	if (stage == 0 && memlat_on == 0) {
		return;
	}
	if (kernel_thread_start(cpu_thread, (void *)(uintptr_t)stage, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
