/*
 * The Xiaomi 15's (dada) sound: what iOS plays (/dev/redwood-audio, redwood_audio.c) through the audio DSP to the
 * two speaker amplifiers (hakios_dada_amp.c).
 *
 * The path, as UniXNU found it for the same SoC family (UniXNU-sheng, SM8650: ShengAudioReach.c, AudioProbe.cpp,
 * SmmuBypass.h) and Android's device tree for dada confirms (sec_tdm_pinctrl: gpio121 clock, gpio123 frame,
 * gpio122 data in, gpio124 data out, "i2s1" functions of the TLMM):
 *
 *   our memory  64 KiB below 4 GB (the DSP takes 32-bit addresses; the loader keeps 4 MB there for us: boot-arg
 *               lowdma=), which the DSP reads as stream 0x1001 of the application processor's SMMU. The
 *               hypervisor does not let us translate for it, so the stream passes through: the last context
 *               bank left untranslating (SCTLR 0, CBAR "stage 1 translate, stage 2 bypass") and the stream
 *               matched to it. Addresses go to the DSP as address | 1 << 32 (the stream's low four bits).
 *   the DSP     two AudioReach graphs through its graph manager (APM, over GPR, redwood_adsp.c): a device
 *               subgraph, I2S_SINK on the secondary MI2S, data line 1, the LPASS the clock master (16-bit
 *               stereo, 48 kHz: a 1.536 MHz bit clock); and a stream subgraph, WR_SHARED_MEM_EP -> PCM_DEC ->
 *               PCM_CNV -> the sink. Periods of 20 ms are handed to the endpoint one by one
 *               (DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2); each comes back done.
 *   the amps    clock slaves on that I2S, the bottom one the left slot, the top one the right; powered while
 *               there is sound, down after a few seconds of silence.
 *
 * Boot-args: dadaaudio=1, a look at the SMMU only (reading); dadaaudio=2, the whole path, a short quiet test tone
 * (1 kHz, -40 dBFS, half a second) to see the DSP take periods and the amplifiers' PLLs lock, then what iOS plays.
 * Needs adsp=1 dadaadsp=1 (the DSP up) and the loader's lowdma=.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/redwood_audio.h>
#include <sys/uio.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>

extern bool hakios_rpmh_regulator(const char *name, uint32_t mv, bool enable, uint32_t mode);
#define say(...) redwood_tl("daudio: " __VA_ARGS__)
extern void redwood_tl(const char *format, ...) __printflike(1, 2);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);

extern bool hakios_adsp_gpr(uint32_t port, uint32_t opcode, const void *payload, uint32_t length, bool with_header, uint32_t ms,
    uint32_t *answer_opcode, uint32_t *words, uint32_t nwords);
extern bool hakios_adsp_gpr_raw(const void *packet, uint32_t length, uint32_t ms, uint32_t *answer_opcode, uint32_t *words,
    uint32_t nwords);
extern void (*hakios_adsp_gpr_event)(uint32_t opcode, uint32_t token, const uint32_t *words, uint32_t length);
extern bool hakios_dada_amp_setup(void);
extern void hakios_dada_amp_power(bool on);
extern void hakios_dada_amp_status(const char *when);

#define APPS_SMMU_PHYS          0x15000000UL
#define APPS_SMMU_SIZE          0x100000UL
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_IDR2               0x028
#define SMMU_CBAR(n)            (0x1000 + 4 * (n))
#define SMMU_CBA2R(n)           (0x1800 + 4 * (n))
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_SMR_VALID          (1u << 31)
#define AUDIO_STREAM            0x1001u
#define AUDIO_MASK              0x80u

#define TLMM_PHYS               0x0f100000UL

/* GPR and AudioReach (Linux audioreach.h, q6apm.c) */
#define GPR_DOMAIN_ADSP         2
#define GPR_DOMAIN_APPS         3
#define OUR_PORT                0x2001
#define APM_IID                 1
#define APM_CMD_GET_SPF_STATE   0x01001021
#define APM_CMD_RSP_GET_SPF_STATE 0x02001007
#define GPR_BASIC_RSP_RESULT    0x02001005
#define OP_GRAPH_OPEN           0x01001000
#define OP_GRAPH_PREPARE        0x01001001
#define OP_GRAPH_START          0x01001002
#define OP_SET_CFG              0x01001006
#define OP_MEM_MAP              0x0100100c
#define OP_MEM_MAP_DONE         0x02001001
#define OP_WRITE                0x0400100a      /* DATA_CMD_WR_SH_MEM_EP_DATA_BUFFER_V2 */
#define OP_WRITE_DONE           0x05001004
#define MOD_WR_SHMEM            0x07001000
#define MOD_PCM_CNV             0x07001003
#define MOD_PCM_DEC             0x07001005
#define MOD_I2S_SINK            0x0700100a
#define SG_PLAY                 0x4001
#define SG_SPEAKER              0x4005
#define IID_PLAY_SHMEM          0x6001
#define IID_PLAY_DEC            0x6002
#define IID_PLAY_CNV            0x6003
#define IID_SPEAKER_I2S         0x6051

#define RATE                    48000
#define CHANNELS                2
#define PERIOD_FRAMES           960             /* 20 ms */
#define PERIOD_BYTES            (PERIOD_FRAMES * CHANNELS * 2)
#define SLOTS                   8
#define BUFFER_BYTES            0x10000
#define CMD_OFFSET              0xc000          /* out-of-band command payloads (after the periods) */
#define IN_FLIGHT               4               /* 80 ms ahead of the DSP */
#define RING_FRAMES             (RATE / 2)      /* what iOS has written and the DSP not yet taken: half a second */
#define QUIET_PERIODS           (5 * RATE / PERIOD_FRAMES)      /* the amplifiers off after 5 s of silence */

static vm_offset_t smmu;
static uint64_t buffer_pa, dsp_address;
static volatile uint8_t *buffer;
static uint32_t map_handle;
static uint32_t token = 100;

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

/* ---- the SMMU: a look (dadaaudio=1), and the stream passed through ----------------------------------------------- */

static void
smmu_look(void)
{
	uint32_t idr0, idr1, scr0, smrs, banks, page, pages, used = 0;
	char taken[600];
	int at = 0;

	scr0 = rd(smmu + SMMU_SCR0);
	idr0 = rd(smmu + SMMU_IDR0);
	idr1 = rd(smmu + SMMU_IDR1);
	smrs = idr0 & 0xff;
	banks = idr1 & 0xff;
	page = (idr1 >> 31) ? 0x10000 : 0x1000;
	pages = 1u << (((idr1 >> 28) & 7) + 1);
	say("the application processor's SMMU: sCR0 %#x, IDR0 %#x (%u stream matches), IDR1 %#x (%u context banks from %#x), "
	    "IDR2 %#x\n", scr0, idr0, smrs, idr1, banks, pages * page, rd(smmu + SMMU_IDR2));
	for (uint32_t n = 0; n < smrs && n < 256; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if (!(smr & SMMU_SMR_VALID)) {
			continue;
		}
		used++;
		if (at < (int)sizeof(taken) - 24) {
			at += snprintf(taken + at, sizeof(taken) - (size_t)at, " %u:%x>%x", n, smr & 0x7fffffff, s2cr);
		}
	}
	taken[at] = 0;
	say("%u stream matches taken:%s\n", used, taken);
}

/* stream -> the last context bank, left untranslating (Linux arm-smmu-qcom's bypass quirk; UniXNU SmmuBypass.h) */
static const char *
smmu_bypass(uint16_t stream)
{
	uint32_t idr0 = rd(smmu + SMMU_IDR0), idr1 = rd(smmu + SMMU_IDR1);
	uint32_t count = MIN(idr0 & 0xff, 128u), banks = idr1 & 0xff, pages = 1u << (((idr1 >> 28) & 7) + 1);
	uint32_t bank = banks - 1, slot = ~0u, cb;
	bool have = false;

	if ((idr1 >> 31) || banks < 2 || pages > 128) {
		return "an SMMU of another shape";
	}
	cb = (pages + bank) * 0x1000;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t smr = rd(smmu + SMMU_SMR(i)), s2cr, mask;

		if (!(smr & SMMU_SMR_VALID)) {
			if (slot == ~0u) {
				slot = i;
			}
			continue;
		}
		s2cr = rd(smmu + SMMU_S2CR(i));
		mask = (smr >> 16) & 0x7fff;
		if (((smr ^ stream) & 0xffff & ~mask) == 0) {
			if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) == bank) {
				have = true;
				slot = i;
				break;
			}
			say("stream %#x is matched already (%u: %#x -> %#x)\n", stream, i, smr, s2cr);
			return "the stream is taken";
		}
	}
	if (rd(smmu + cb) & 1) {
		return "the last context bank translates";
	}
	if (rd(smmu + cb) != 0 || rd(smmu + SMMU_CBAR(bank)) != 0x10000 || rd(smmu + SMMU_CBA2R(bank)) != 1) {
		wr(smmu + cb, 0);
		wr(smmu + SMMU_CBA2R(bank), 1);
		wr(smmu + SMMU_CBAR(bank), 0x10000);
	}
	if (rd(smmu + cb) != 0 || rd(smmu + SMMU_CBA2R(bank)) != 1 || rd(smmu + SMMU_CBAR(bank)) != 0x10000) {
		return "the context bank did not take its settings";
	}
	if (slot == ~0u) {
		return "no free stream match";
	}
	if (!have) {
		wr(smmu + SMMU_S2CR(slot), bank);
		if (rd(smmu + SMMU_S2CR(slot)) != bank) {
			return "the stream's S2CR did not take";
		}
		wr(smmu + SMMU_SMR(slot), SMMU_SMR_VALID | stream);
	}
	if (rd(smmu + SMMU_SMR(slot)) != (SMMU_SMR_VALID | stream) || rd(smmu + SMMU_S2CR(slot)) != bank) {
		return "the stream's match did not take";
	}
	say("stream %#x passes through: stream match %u, context bank %u%s\n", stream, slot, bank, have ? " (as it was)" : "");
	return NULL;
}

/* ---- GPR packets (UniXNU ShengAudioReach.c) --------------------------------------------------------------------- */

struct pkt {
	uint8_t b[1024];
	uint32_t n;
};

static void
put32(struct pkt *p, uint32_t v)
{
	if (p->n + 4 <= sizeof(p->b)) {
		memcpy(p->b + p->n, &v, 4);
		p->n += 4;
	}
}

static void
put16(struct pkt *p, uint32_t v)
{
	if (p->n + 2 <= sizeof(p->b)) {
		p->b[p->n] = (uint8_t)v;
		p->b[p->n + 1] = (uint8_t)(v >> 8);
		p->n += 2;
	}
}

static void
put8(struct pkt *p, uint32_t v)
{
	if (p->n < sizeof(p->b)) {
		p->b[p->n++] = (uint8_t)v;
	}
}

static void
set32(struct pkt *p, uint32_t at, uint32_t v)
{
	memcpy(p->b + at, &v, 4);
}

static void
pad_to(struct pkt *p, uint32_t end)
{
	while (p->n < end && p->n < sizeof(p->b)) {
		p->b[p->n++] = 0;
	}
}

static void
begin(struct pkt *p, uint32_t src, uint32_t dst, uint32_t opcode)
{
	p->n = 0;
	put32(p, 0);
	put32(p, GPR_DOMAIN_ADSP | GPR_DOMAIN_APPS << 8);
	put32(p, src);
	put32(p, dst);
	put32(p, ++token);
	put32(p, opcode);
}

static uint32_t
finish(struct pkt *p)
{
	set32(p, 0, (6u << 4) | p->n << 8);
	return p->n;
}

static uint32_t
cmd_begin(struct pkt *p)
{
	uint32_t at = p->n;

	put32(p, 0);
	put32(p, 0);
	put32(p, 0);
	put32(p, 0);
	return at;
}

static void
cmd_end(struct pkt *p, uint32_t at)
{
	set32(p, at + 12, p->n - at - 16);
}

static uint32_t
param_begin(struct pkt *p, uint32_t iid, uint32_t id)
{
	uint32_t at = p->n;

	put32(p, iid);
	put32(p, id);
	put32(p, 0);
	put32(p, 0);
	return at;
}

static void
param_end(struct pkt *p, uint32_t at)
{
	pad_to(p, at + ((p->n - at + 7) & ~7u));
	set32(p, at + 8, p->n - at - 16);
}

struct module {
	uint32_t module, iid, inputs, outputs;
};
struct link {
	uint32_t from, to;
};

static void graph_open_ex(struct pkt *p, uint32_t sg, uint32_t direction, uint32_t scenario, uint32_t position,
    const struct module *m, unsigned modules, const struct link *l, unsigned links);

static void
graph_open(struct pkt *p, uint32_t sg, uint32_t direction, const struct module *m, unsigned modules, const struct link *l,
    unsigned links)
{
	graph_open_ex(p, sg, direction, 1, 1, m, modules, l, links);
}

static void
graph_open_ex(struct pkt *p, uint32_t sg, uint32_t direction, uint32_t scenario, uint32_t position, const struct module *m,
    unsigned modules, const struct link *l, unsigned links)
{
	uint32_t cmd, at;

	begin(p, OUR_PORT, APM_IID, OP_GRAPH_OPEN);
	cmd = cmd_begin(p);
	at = param_begin(p, APM_IID, 0x08001001);       /* subgraph: low latency, direction, playback */
	put32(p, 1);
	put32(p, sg); put32(p, 3);
	put32(p, 0x0800100e); put32(p, 4); put32(p, 2);
	put32(p, 0x0800100f); put32(p, 4); put32(p, direction);
	put32(p, 0x08001010); put32(p, 4); put32(p, scenario);
	param_end(p, at);
	at = param_begin(p, APM_IID, 0x08001000);       /* its container: generic, in the stream position, 8 KiB, the ADSP */
	put32(p, 1);
	put32(p, sg); put32(p, 4);
	put32(p, 0x08001011); put32(p, 8); put32(p, 1); put32(p, 0x0b001001);
	put32(p, 0x08001012); put32(p, 4); put32(p, position);
	put32(p, 0x08001013); put32(p, 4); put32(p, 8192);
	put32(p, 0x08001014); put32(p, 4); put32(p, 2);
	param_end(p, at);
	at = param_begin(p, APM_IID, 0x08001002);       /* its modules */
	put32(p, 1);
	put32(p, sg); put32(p, sg); put32(p, modules);
	for (unsigned i = 0; i < modules; i++) {
		put32(p, m[i].module);
		put32(p, m[i].iid);
	}
	param_end(p, at);
	at = param_begin(p, APM_IID, 0x08001003);       /* their ports */
	put32(p, modules);
	for (unsigned i = 0; i < modules; i++) {
		put32(p, m[i].iid); put32(p, 1); put32(p, 0x08001015); put32(p, 8); put32(p, m[i].inputs); put32(p, m[i].outputs);
	}
	param_end(p, at);
	at = param_begin(p, APM_IID, 0x08001004);       /* their connections: output port 1 to input port 2 */
	put32(p, links);
	for (unsigned i = 0; i < links; i++) {
		put32(p, l[i].from); put32(p, 1); put32(p, l[i].to); put32(p, 2);
	}
	param_end(p, at);
	cmd_end(p, cmd);
	finish(p);
}

static void
pcm_output(struct pkt *p, uint32_t iid, uint32_t interleaving)
{
	uint32_t at = param_begin(p, iid, 0x08001008);

	put32(p, 1); put32(p, 0x09001000); put32(p, (16u + CHANNELS + 3u) & ~3u);
	put16(p, 16); put16(p, 1); put16(p, 16); put16(p, 15); put16(p, 1); put16(p, interleaving); put16(p, 0);
	put16(p, CHANNELS);
	put8(p, 1); put8(p, 2);         /* front left, front right */
	param_end(p, at);
}

/* A packet sent and its answer waited for: true for a basic result of status 0 (or a map's handle). */
static bool
command(struct pkt *p, const char *what)
{
	uint32_t opcode = 0, words[4] = { 0 }, sent;

	memcpy(&sent, p->b + 20, 4);
	/*
	 * The DSP's receive buffers on this link are 128 bytes: an APM command that does not fit has its payload in
	 * our mapped memory instead ("out of band": the command header's address, map handle and size), as SPF allows.
	 */
	if (p->n > 128) {
		uint32_t size = p->n - 40, header[4];
		uint64_t at = dsp_address + CMD_OFFSET;

		if (map_handle == 0 || size > BUFFER_BYTES - CMD_OFFSET) {
			say("%s: %u bytes, too big for the link and no memory of ours mapped\n", what, p->n);
			return false;
		}
		for (uint32_t i = 0; i < size; i++) {
			buffer[CMD_OFFSET + i] = p->b[40 + i];
		}
		__builtin_arm_dsb(0xf);
		header[0] = (uint32_t)at;
		header[1] = (uint32_t)(at >> 32);
		header[2] = map_handle;
		header[3] = size;
		memcpy(p->b + 24, header, sizeof(header));
		p->n = 40;
		finish(p);
	}
	{
		uint64_t t0 = mach_absolute_time(), ns;
		bool answered = hakios_adsp_gpr_raw(p->b, p->n, 2000, &opcode, words, 4);

		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		if (ns > 100 * NSEC_PER_MSEC) {
			say("%s: the DSP took %llu ms\n", what, ns / NSEC_PER_MSEC);
		}
		if (!answered) {
			say("%s: no answer\n", what);
			return false;
		}
	}
	if (opcode == OP_MEM_MAP_DONE) {
		map_handle = words[0];
		say("%s: handle %#x\n", what, map_handle);
		return true;
	}
	if (opcode != GPR_BASIC_RSP_RESULT || words[0] != sent || words[1] != 0) {
		say("%s: answer %#x: %#x %#x\n", what, opcode, words[0], words[1]);
		return false;
	}
	return true;
}

static bool
graph_command(uint32_t opcode, uint32_t sg, const char *what)
{
	struct pkt p;
	uint32_t cmd, at;

	begin(&p, OUR_PORT, APM_IID, opcode);
	cmd = cmd_begin(&p);
	at = param_begin(&p, APM_IID, 0x08001005);
	put32(&p, 1);
	put32(&p, sg);
	param_end(&p, at);
	cmd_end(&p, cmd);
	finish(&p);
	return command(&p, what);
}

static bool
graphs_up(void)
{
	static const struct module speaker[] = { { MOD_I2S_SINK, IID_SPEAKER_I2S, 1, 0 } };
	static const struct module play[] = { { MOD_WR_SHMEM, IID_PLAY_SHMEM, 0, 1 }, { MOD_PCM_DEC, IID_PLAY_DEC, 1, 1 },
		                              { MOD_PCM_CNV, IID_PLAY_CNV, 1, 1 } };
	static const struct link links[] = { { IID_PLAY_SHMEM, IID_PLAY_DEC }, { IID_PLAY_DEC, IID_PLAY_CNV },
		                             { IID_PLAY_CNV, IID_SPEAKER_I2S } };
	struct pkt p;
	uint32_t cmd, at, body;

	/* the map: the region right after the GPR header, the command header's room at the end (as the DSP's own client) */
	begin(&p, OUR_PORT, APM_IID, OP_MEM_MAP);
	put16(&p, 3); put16(&p, 1); put32(&p, 0);
	put32(&p, (uint32_t)dsp_address); put32(&p, (uint32_t)(dsp_address >> 32)); put32(&p, BUFFER_BYTES);
	pad_to(&p, p.n + 16);
	finish(&p);
	if (!command(&p, "our memory mapped for the DSP")) {
		return false;
	}
	graph_open(&p, SG_SPEAKER, 1, speaker, 1, NULL, 0);
	if (!command(&p, "the speaker device graph")) {
		return false;
	}
	begin(&p, OUR_PORT, APM_IID, OP_SET_CFG);
	cmd = cmd_begin(&p);
	at = param_begin(&p, IID_SPEAKER_I2S, 0x08001019);      /* LPAIF, secondary, data line 1, our frame clock */
	put32(&p, 0); put32(&p, 1); put16(&p, 2); put16(&p, 1);
	param_end(&p, at);
	at = param_begin(&p, IID_SPEAKER_I2S, 0x08001017);      /* 48 kHz, 16 bits, 2 channels, fixed point */
	put32(&p, RATE); put16(&p, 16); put16(&p, CHANNELS); put32(&p, 1);
	param_end(&p, at);
	at = param_begin(&p, IID_SPEAKER_I2S, 0x08001018);      /* frame size factor 1 */
	put32(&p, 1);
	param_end(&p, at);
	cmd_end(&p, cmd);
	finish(&p);
	if (!command(&p, "the I2S sink's configuration")) {
		return false;
	}
	graph_open(&p, SG_PLAY, 2, play, 3, links, 3);
	if (!command(&p, "the playback stream graph")) {
		return false;
	}
	begin(&p, OUR_PORT, APM_IID, OP_SET_CFG);
	cmd = cmd_begin(&p);
	pcm_output(&p, IID_PLAY_DEC, 1);
	pcm_output(&p, IID_PLAY_CNV, 3);
	cmd_end(&p, cmd);
	finish(&p);
	if (!command(&p, "the decoder's and converter's formats")) {
		return false;
	}
	begin(&p, OUR_PORT, IID_PLAY_SHMEM, OP_SET_CFG);
	cmd = cmd_begin(&p);
	at = param_begin(&p, IID_PLAY_SHMEM, 0x0800100c);
	body = (12u + 16u + CHANNELS + 7u) & ~7u;
	put32(&p, 1); put32(&p, 0x09001000); put32(&p, body + 16u - 12u);
	put32(&p, RATE); put16(&p, 16); put16(&p, 1); put16(&p, 16); put16(&p, 15); put16(&p, 1); put16(&p, CHANNELS);
	put8(&p, 1); put8(&p, 2);
	param_end(&p, at);
	cmd_end(&p, cmd);
	finish(&p);
	if (!command(&p, "the endpoint's media format")) {
		return false;
	}
	return true;

}

/* ---- the periods ----------------------------------------------------------------------------------------------- */

static volatile uint32_t done_count, done_bad, last_bad_status;
static uint32_t sent_count;
static int done_event;

static volatile uint32_t mic_done, mic_bytes_last, mic_status_last;
static int mic_event;

static void
gpr_event(uint32_t opcode, uint32_t tok, const uint32_t *words, uint32_t length)
{
	if (opcode == 0x05001005) {     /* RD_SH_MEM_EP_DATA_BUFFER_DONE_V2: status, address, handle, size, offset */
		mic_status_last = length >= 4 ? words[0] : 0xffffffffu;
		mic_bytes_last = length >= 20 ? words[4] : 0;
		mic_done++;
		wakeup(&mic_event);
		return;
	}
	if (opcode == OP_WRITE_DONE) {
		if (length >= 16 && words[3] != 0) {
			done_bad++;
			last_bad_status = words[3];
		}
		done_count++;
		wakeup(&done_event);
	}
	(void)tok;
}

static bool
send_period(uint32_t slot)
{
	struct pkt p;
	uint64_t address = dsp_address + (uint64_t)slot * PERIOD_BYTES;

	__builtin_arm_dsb(0xf);
	begin(&p, OUR_PORT, IID_PLAY_SHMEM, OP_WRITE);
	put32(&p, (uint32_t)address); put32(&p, (uint32_t)(address >> 32)); put32(&p, map_handle); put32(&p, PERIOD_BYTES);
	put32(&p, 0); put32(&p, 0); put32(&p, 0);
	put32(&p, 0); put32(&p, 0); put32(&p, 0); put32(&p, 0);
	finish(&p);
	if (!hakios_adsp_gpr_raw(p.b, p.n, 0, NULL, NULL, 0)) {
		return false;
	}
	sent_count++;
	return true;
}

/* what iOS has written, until the DSP takes it */
static int16_t *ring;
static uint32_t ring_head, ring_tail;   /* frames, written at head, taken at tail (free running) */
static LCK_GRP_DECLARE(daudio_group, "dada audio");
static LCK_SPIN_DECLARE(ring_lock, &daudio_group);

static uint32_t
sink(const int16_t *frames, uint32_t count)
{
	uint32_t taken = 0;

	lck_spin_lock(&ring_lock);
	while (taken < count && ring_head - ring_tail < RING_FRAMES) {
		uint32_t at = ring_head % RING_FRAMES;

		ring[2 * at] = frames[2 * taken];
		ring[2 * at + 1] = frames[2 * taken + 1];
		ring_head++;
		taken++;
	}
	lck_spin_unlock(&ring_lock);
	return taken;
}

/* a period from the ring (silence where it runs short): true if it is not silence */
static bool
fill_period(uint32_t slot)
{
	volatile int16_t *out = (volatile int16_t *)(buffer + slot * PERIOD_BYTES);
	bool loud = false;

	lck_spin_lock(&ring_lock);
	for (uint32_t i = 0; i < PERIOD_FRAMES; i++) {
		int16_t l = 0, r = 0;

		if (ring_tail != ring_head) {
			uint32_t at = ring_tail % RING_FRAMES;

			l = ring[2 * at];
			r = ring[2 * at + 1];
			ring_tail++;
		}
		out[2 * i] = l;
		out[2 * i + 1] = r;
		loud |= (l > 8 || l < -8 || r > 8 || r < -8);
	}
	lck_spin_unlock(&ring_lock);
	return loud;
}

/* a quiet tone: 1 kHz at -40 dBFS (a sixth-order table of one period, 48 samples) */
static void
tone_period(uint32_t slot)
{
	static const int16_t sine48[48] = {
		0, 43, 85, 125, 164, 200, 232, 260, 284, 303, 317, 325, 328, 325, 317, 303, 284, 260, 232, 200, 164, 125, 85, 43,
		0, -43, -85, -125, -164, -200, -232, -260, -284, -303, -317, -325, -328, -325, -317, -303, -284, -260, -232, -200,
		-164, -125, -85, -43
	};
	volatile int16_t *out = (volatile int16_t *)(buffer + slot * PERIOD_BYTES);

	for (uint32_t i = 0; i < PERIOD_FRAMES; i++) {
		out[2 * i] = sine48[i % 48];
		out[2 * i + 1] = sine48[i % 48];
	}
}

static void
wait_done(uint32_t ms)
{
	(void)tsleep(&done_event, PZERO, "daudio", MAX(1, (int)(ms * hz / 1000)));
}

static void
feeder(void)
{
	uint32_t next = 0, quiet = QUIET_PERIODS, reported = 0;
	bool amps_on = false;
	uint64_t last_report = mach_absolute_time(), now, interval;

	nanoseconds_to_absolutetime(10ull * NSEC_PER_SEC, &interval);
	for (;;) {
		while (sent_count - done_count < IN_FLIGHT) {
			bool loud = fill_period(next % SLOTS);

			quiet = loud ? 0 : quiet + 1;
			if (loud && !amps_on) {
				hakios_dada_amp_power(true);
				amps_on = true;
				say("sound: the amplifiers on\n");
			}
			if (!send_period(next % SLOTS)) {
				say("a period could not be sent: the feeder stops\n");
				return;
			}
			next++;
		}
		if (amps_on && quiet >= QUIET_PERIODS) {
			hakios_dada_amp_power(false);
			amps_on = false;
			say("silence: the amplifiers off\n");
		}
		wait_done(100);
		now = mach_absolute_time();
		if (now - last_report > interval && done_count != reported) {
			say("%u periods played (%u refused, last status %#x)\n", done_count, done_bad, last_bad_status);
			reported = done_count;
			last_report = now;
		}
	}
}

/* ---- the whole path -------------------------------------------------------------------------------------------- */

static void
pins(void)
{
	static const unsigned i2s[] = { 121, 122, 123, 124 };

	for (unsigned i = 0; i < sizeof(i2s) / sizeof(i2s[0]); i++) {
		vm_offset_t cfg = ml_io_map(TLMM_PHYS + 0x1000UL * i2s[i], 0x1000);
		uint32_t old = rd(cfg);

		/* function 1 (i2s1_*), 8 mA, no pull; the eGPIO pads given to the application processor */
		wr(cfg, (old & (1u << 11)) | ((old & (1u << 11)) ? 1u << 12 : 0) | 1u << 2 | 3u << 6);
		say("gpio%u: %#x -> %#x\n", i2s[i], old, rd(cfg));
	}
}

static bool
dsp_ready(void)
{
	for (unsigned tries = 0; tries < 180; tries++) {
		uint32_t opcode = 0, words[2] = { 0 };

		if (hakios_adsp_gpr(APM_IID, APM_CMD_GET_SPF_STATE, NULL, 0, true, 500, &opcode, words, 2) &&
		    opcode == APM_CMD_RSP_GET_SPF_STATE && words[0] == 1) {
			say("the DSP's audio framework is ready (after %u tries)\n", tries);
			return true;
		}
		delay_for_interval(1000, NSEC_PER_MSEC);
	}
	say("the DSP's audio framework never said it was ready\n");
	return false;
}

/* ---- the microphones (boot-arg dadamic=1, after the speaker's test tone) ----------------------------------------------
 *
 * dada's microphones are digital (DT dmic01/23/45/67 on the LPASS LPI pins) and go into the VA macro, as on the Xiaomi Pad
 * 8 Pro (SM8750, mainline: VA DEC0 <- DMIC0, codec DMA VA TX0). Here: the DSP's PRM votes the LPASS macros and the digital
 * codec and runs the VA core clock; the DMIC0/1 pins (LPI gpio6 clock, gpio7 data, function 1, 8 mA) and the VA macro
 * (Linux lpass-va-macro.c: MCLK and FS counter, DMIC0 clock /8 of 19.2 MHz = 2.4 MHz, decimator 0 from DMIC0 at 48 kHz,
 * HPF, +40 dB) are set; two graphs (device: CODEC_DMA_SOURCE on VA TX0; stream: PCM convert, PCM encode, shared memory
 * read endpoint) run, and two seconds of 20 ms mono periods are read into our memory after the speaker's periods. Said:
 * how many came back, their level.
 */
#define PRM_IID                 2
#define LPI_TLMM_PHYS           0x07760000UL
#define VA_MACRO_PHYS           0x07660000UL
#define SG_MIC                  0x4007
#define SG_CAPTURE              0x4003
#define IID_MIC_DMA             0x6070
#define IID_CAP_CNV             0x6022
#define IID_CAP_ENC             0x6023
#define IID_CAP_SHMEM           0x6024
#define MIC_OFFSET              0x8000
#define MIC_PERIOD              (PERIOD_FRAMES * 2)     /* mono, 16-bit */
#define MIC_SLOTS               4

static bool
prm_request(uint32_t param, const uint32_t *v, unsigned n, const char *what)
{
	struct pkt p;
	uint32_t cmd, at, opcode = 0, words[4] = { 0 };

	begin(&p, PRM_IID, PRM_IID, 0x0100100f);
	cmd = cmd_begin(&p);
	at = param_begin(&p, PRM_IID, param);
	for (unsigned i = 0; i < n; i++) {
		put32(&p, v[i]);
	}
	set32(&p, at + 8, p.n - at - 16);
	cmd_end(&p, cmd);
	finish(&p);
	if (!hakios_adsp_gpr_raw(p.b, p.n, 2000, &opcode, words, 4)) {
		say("mic: %s: no answer\n", what);
		return false;
	}
	say("mic: %s: answer %#x %#x %#x\n", what, opcode, words[0], words[1]);
	return (opcode == 0x02001002 && words[1] == 0) || (opcode == GPR_BASIC_RSP_RESULT && words[1] == 0);
}

static bool
mic_read(uint32_t slot)
{
	struct pkt p;
	uint64_t address = dsp_address + MIC_OFFSET + (uint64_t)slot * MIC_PERIOD;

	begin(&p, OUR_PORT, IID_CAP_SHMEM, 0x0400100b);
	put32(&p, (uint32_t)address); put32(&p, (uint32_t)(address >> 32)); put32(&p, map_handle); put32(&p, MIC_PERIOD);
	put32(&p, 0); put32(&p, 0); put32(&p, 0); put32(&p, 0);
	finish(&p);
	return hakios_adsp_gpr_raw(p.b, p.n, 0, NULL, NULL, 0);
}

static void
pcm_output_mono(struct pkt *p, uint32_t iid)
{
	uint32_t at = param_begin(p, iid, 0x08001008);

	put32(p, 1); put32(p, 0x09001000); put32(p, (16u + 1u + 3u) & ~3u);
	put16(p, 16); put16(p, 1); put16(p, 16); put16(p, 15); put16(p, 1); put16(p, 1); put16(p, 0);
	put16(p, 1);
	put8(p, 1);
	param_end(p, at);
}

/* the capture graphs on a codec DMA interface (lpaif type, index), then each of `count' inputs (select(k)) listened to */
static vm_offset_t mic_va;

static void
mic_select_dmic(unsigned k)
{
	wr(mic_va + 0x0100, (rd(mic_va + 0x0100) & ~0xf0u) | (k + 1) << 4);    /* decimator 0 from DMIC k */
}

static bool
mic_graphs_start(uint32_t type, uint32_t index)
{
	static const struct module dev[] = { { 0x07001024, IID_MIC_DMA, 0, 1 } };
	static const struct module cap[] = { { MOD_PCM_CNV, IID_CAP_CNV, 1, 1 }, { 0x07001004, IID_CAP_ENC, 1, 1 },
		                             { 0x07001001, IID_CAP_SHMEM, 1, 0 } };
	static const struct link links[] = { { IID_MIC_DMA, IID_CAP_CNV }, { IID_CAP_CNV, IID_CAP_ENC }, { IID_CAP_ENC, IID_CAP_SHMEM } };
	struct pkt p;
	uint32_t cmd, at;

	graph_open_ex(&p, SG_MIC, 1, 2, 4, dev, 1, NULL, 0);
	if (!command(&p, "mic: the microphone device graph")) {
		return false;
	}
	graph_open_ex(&p, SG_CAPTURE, 1, 2, 4, cap, 3, links, 3);
	if (!command(&p, "mic: the capture stream graph")) {
		return false;
	}
	begin(&p, OUR_PORT, APM_IID, OP_SET_CFG);
	cmd = cmd_begin(&p);
	at = param_begin(&p, IID_MIC_DMA, 0x08001017);           /* hardware endpoint media format */
	put32(&p, RATE); put16(&p, 16); put16(&p, 1); put32(&p, 1);
	param_end(&p, at);
	at = param_begin(&p, IID_MIC_DMA, 0x08001018);           /* frame size factor */
	put32(&p, 1);
	param_end(&p, at);
	at = param_begin(&p, IID_MIC_DMA, 0x08001063);           /* codec DMA: the interface, channel 0 */
	put32(&p, type); put32(&p, index); put32(&p, 1);
	param_end(&p, at);
	at = param_begin(&p, IID_MIC_DMA, 0x08001176);           /* power mode */
	put32(&p, 0);
	param_end(&p, at);
	cmd_end(&p, cmd);
	finish(&p);
	if (!command(&p, "mic: the device graph's configuration")) {
		return false;
	}
	begin(&p, OUR_PORT, APM_IID, OP_SET_CFG);
	cmd = cmd_begin(&p);
	pcm_output_mono(&p, IID_CAP_CNV);
	pcm_output_mono(&p, IID_CAP_ENC);
	cmd_end(&p, cmd);
	finish(&p);
	if (!command(&p, "mic: the capture graph's configuration") ||
	    !graph_command(OP_GRAPH_PREPARE, SG_MIC, "mic: the device graph prepared") ||
	    !graph_command(OP_GRAPH_PREPARE, SG_CAPTURE, "mic: the capture graph prepared") ||
	    !graph_command(OP_GRAPH_START, SG_MIC, "mic: the device graph started") ||
	    !graph_command(OP_GRAPH_START, SG_CAPTURE, "mic: the capture graph started")) {
		return false;
	}
	return true;
}

static void
mic_graphs_stop(void)
{
	(void)graph_command(0x01001003, SG_CAPTURE, "mic: the capture graph stopped");
	(void)graph_command(0x01001003, SG_MIC, "mic: the device graph stopped");
	/* and closed (APM_CMD_GRAPH_CLOSE), so that the next recording can open them again */
	(void)graph_command(0x01001004, SG_CAPTURE, "mic: the capture graph closed");
	(void)graph_command(0x01001004, SG_MIC, "mic: the device graph closed");
}

static void
mic_capture(uint32_t type, uint32_t index, void (*select)(unsigned), unsigned count, const char *name)
{
	uint32_t ms = 500;

	(void)PE_parse_boot_argn("dadamic_ms", &ms, sizeof(ms));
	if (!mic_graphs_start(type, index)) {
		return;
	}
	for (unsigned k = 0; k < count; k++) {
		uint32_t asked = 0, want = RATE * ms / 1000 / PERIOD_FRAMES, peak = 0, nonzero = 0;
		int32_t lo = 32767, hi = -32768;
		uint64_t sum = 0, samples = 0, start, ns = 0;

		select(k);
		delay_for_interval(20, NSEC_PER_MSEC);
		mic_done = 0;
		start = mach_absolute_time();
		while (asked < MIC_SLOTS) {
			(void)mic_read(asked % MIC_SLOTS);
			asked++;
		}
		while (mic_done < want && ns < (uint64_t)ms * 4 * NSEC_PER_MSEC) {
			uint32_t seen = mic_done;

			(void)tsleep(&mic_event, PZERO, "dmic", MAX(1, (int)(100 * hz / 1000)));
			while (seen < mic_done) {
				const volatile int16_t *x = (const volatile int16_t *)(buffer + MIC_OFFSET + (seen % MIC_SLOTS) * MIC_PERIOD);

				for (uint32_t i = 0; i < PERIOD_FRAMES; i++) {
					int32_t a = x[i] < 0 ? -x[i] : x[i];

					peak = MAX(peak, (uint32_t)a);
					lo = MIN(lo, (int32_t)x[i]);
					hi = MAX(hi, (int32_t)x[i]);
					sum += (uint64_t)a;
					nonzero += a != 0;
				}
				samples += PERIOD_FRAMES;
				seen++;
				if (asked < want) {
					(void)mic_read(asked % MIC_SLOTS);
					asked++;
				}
			}
			absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
		}
		say("mic: %s%u: %u periods back, peak %u, mean %llu, range %d..%d, non-zero %u of %llu%s\n", name, k + 1, mic_done, peak,
		    samples ? sum / samples : 0, lo, hi, nonzero, samples, peak > 50 ? " -- it hears" : "");
		for (unsigned i = 0; i < 6 && mic_done < asked; i++) {
			(void)tsleep(&mic_event, PZERO, "dmic", MAX(1, (int)(50 * hz / 1000)));
		}
	}
	mic_graphs_stop();
}

/*
 * dadamic=2: the phone's real microphones. They are analog, on the WCD9395 codec (DT wcd939x-codec; routing AMIC1..5 with
 * MIC BIAS1..4), whose ADCs go out over SoundWire to the master in the VA region (0x07630000, the DT's va_swr_master with
 * wcd939x-tx-slave, mainline sm8750's swr2 "TX") and into the TX macro's decimators, then TX_CODEC_DMA_TX_3. As mainline
 * Linux has it for sm8750-mtp (lpass-tx-macro.c, soundwire/qcom.c v2.0, wcd939x.c, wcd939x-sdw.c): the codec's TX port 1
 * (ADC1..4 on channels 1..4) on the master's port 2, lane 2. Here ADC1 alone, its input (AMIC1..5, TX_NEW_CH12_MUX) swept,
 * all four mic biases on; decimator 0 from SWR_MIC0.
 * Boot-args: dadamic_lane (2), dadamic_si (1), dadamic_mport (2), dadamic_gain (ADC1 gain 0..20, 10), dadamic_mode (3 normal),
 * dadamic_rate (WCD TX clock rate field, 0 = 9.6 MHz), dadamic_div2 (0), dadamic_vol (decimator gain in dB, signed, 20).
 */
#define TX_MACRO_PHYS           0x06ae0000UL
#define SWRM_PHYS               0x07630000UL
#define SWRM_COMP_HW_VERSION    0x0000
#define SWRM_COMP_CFG           0x0004
#define SWRM_COMP_STATUS        0x0014
#define SWRM_LINK_MANAGER_EE    0x0018
#define SWRM_COMP_PARAMS        0x0100
#define SWRM_CMD_FIFO_CFG       0x0314
#define SWRM_ENUMERATOR_CFG     0x0500
#define SWRM_DEV_ID_1(m)        (0x0530 + 0x8 * (m))
#define SWRM_DEV_ID_2(m)        (0x0534 + 0x8 * (m))
#define SWRM_FRAME_CTRL(b)      (0x101c + 0x40 * (b))
#define SWRM_MCP_CFG            0x1048
#define SWRM_MCP_STATUS         0x104c
#define SWRM_SLV_STATUS         0x1090
#define SWRM_DP_CTRL(n, b)      (0x1024 + 0x100 * (n) + 0x40 * (b))
#define SWRM_DP_CTRL2(n, b)     (0x1028 + 0x100 * (n) + 0x40 * (b))
#define SWRM_DP_BLOCK_CTRL1(n)  (0x102c + 0x100 * (n))
#define SWRM_DP_HCTRL(n, b)     (0x1034 + 0x100 * (n) + 0x40 * (b))
#define SWRM_INT_STATUS         0x5000
#define SWRM_INT_CPU_EN         0x5004
#define SWRM_INT_CLEAR          0x5008
#define SWRM_WR_CMD             0x5020
#define SWRM_RD_CMD             0x5024
#define SWRM_RD_FIFO            0x5040
#define SWRM_FIFO_STATUS        0x5050
#define SWRM_CLK_CTRL           0x5060
#define SWRM_LINK_STATUS        0x5064
/* the codec's SoundWire registers (SCP, its data port 1) */
#define SWRS_SCP_CONTROL        0x0044
#define SWRS_SCP_FRAME_CTRL(b)  (0x0060 + 0x10 * (b))
#define SWRS_SCP_DIV2(b)        (0x00e0 + 0x10 * (b))
#define SWRS_DP1_BLOCK_CTRL1    0x0103
#define SWRS_DP1_CH_EN(b)       (0x0120 + 0x10 * (b))
#define SWRS_DP1_SAMPLE1(b)     (0x0122 + 0x10 * (b))
#define SWRS_DP1_SAMPLE2(b)     (0x0123 + 0x10 * (b))
#define SWRS_DP1_OFFSET1(b)     (0x0124 + 0x10 * (b))
#define SWRS_DP1_OFFSET2(b)     (0x0125 + 0x10 * (b))
#define SWRS_DP1_LANE(b)        (0x0128 + 0x10 * (b))
/* the codec's own (wcd939x.h) */
#define WCD_ANA_BIAS            0x3001
#define WCD_ANA_TX_CH1          0x300e
#define WCD_ANA_TX_CH2          0x300f
#define WCD_ANA_MICB(n)         ((n) == 1 ? 0x3022 : (n) == 2 ? 0x3023 : (n) == 3 ? 0x3025 : 0x3026)
#define WCD_MICB_TEST_CTL_1(n)  (0x306b + 3 * ((n) - 1))
#define WCD_MICB_TEST_CTL_2(n)  (0x306c + 3 * ((n) - 1))
#define WCD_TX_3_4_TEST_BLK_EN2 0x308f
#define WCD_TX_NEW_CH12_MUX     0x3127
#define WCD_FE_ICTRL_STG2MAIN_ULP 0x31df
#define WCD_FE_ICTRL_STG2CASC_ULP 0x31e1
#define WCD_CHIP_ID0            0x3401
#define WCD_SWR_TX_CLK_RATE     0x3405
#define WCD_CDC_ANA_CLK_CTL     0x3408
#define WCD_CDC_DIG_CLK_CTL     0x3409
#define WCD_CDC_TX_ANA_MODE_0_1 0x3410
#define WCD_CDC_ANA_TX_CLK_CTL  0x3417
#define WCD_CDC_REQ_CTL         0x3457
#define WCD_CDC_DMIC_CTL        0x345b
#define WCD_INTR_LEVEL_0        0x3474
#define WCD_EFUSE_REG_0         0x34b0

static vm_offset_t swrm, txm;
static uint8_t swr_wid, swr_rid, wcd_dev;
static bool swr_broken;

static void
us_wait(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
swr_write(uint8_t dev, uint16_t reg, uint8_t value)
{
	uint8_t id;

	if (dev == 0xf) {
		id = 0xf;
	} else {
		swr_wid = swr_wid < 14 ? swr_wid + 1 : 0;
		id = swr_wid;
	}
	for (int i = 0; i < 40 && ((rd(swrm + SWRM_FIFO_STATUS) >> 8) & 0x1f) >= 8; i++) {
		us_wait(250);
	}
	wr(swrm + SWRM_WR_CMD, reg | (uint32_t)id << 16 | (uint32_t)dev << 20 | (uint32_t)value << 24);
	us_wait(dev == 0xf ? 10000 : 160);
}

static bool
swr_read(uint8_t dev, uint16_t reg, uint8_t *value)
{
	uint32_t answer = 0;

	swr_rid = swr_rid < 14 ? swr_rid + 1 : 0;
	wr(swrm + SWRM_RD_CMD, reg | (uint32_t)swr_rid << 16 | (uint32_t)dev << 20 | 1u << 24);
	for (int tries = 0; tries < 40; tries++) {
		us_wait(250);
		if (((rd(swrm + SWRM_FIFO_STATUS) >> 16) & 0x1f) == 0) {
			continue;
		}
		answer = rd(swrm + SWRM_RD_FIFO);
		if (((answer >> 8) & 0xf) == swr_rid) {
			*value = (uint8_t)answer;
			return true;
		}
	}
	say("mic: SoundWire: device %u register %#x: no answer (fifo status %#x, last %#x, interrupts %#x)\n", dev, reg,
	    rd(swrm + SWRM_FIFO_STATUS), answer, rd(swrm + SWRM_INT_STATUS));
	return false;
}

/* a codec register's bits set, read back */
static bool
wcd_bits(uint16_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now = 0, back = 0;

	if (swr_broken || !swr_read(wcd_dev, reg, &now)) {
		swr_broken = true;
		return false;
	}
	swr_write(wcd_dev, reg, (uint8_t)((now & ~mask) | (value & mask)));
	if (!swr_read(wcd_dev, reg, &back) || (back & mask) != (value & mask)) {
		say("mic: the codec's %#x <- %#x (mask %#x) but it reads %#x: stopping\n", reg, value, mask, back);
		swr_broken = true;
		return false;
	}
	return true;
}

static uint32_t mic_mport = 2, mic_si = 1, mic_lane = 2, mic_div2;

/* the codec's DP1 and the master's port in bank b (channel 1 on, or off) */
static void
swr_ports(uint8_t b, bool on)
{
	if (mic_div2) {
		swr_write(0xf, SWRS_SCP_DIV2(b), 1);
	}
	swr_write(wcd_dev, SWRS_DP1_SAMPLE1(b), (uint8_t)mic_si);
	swr_write(wcd_dev, SWRS_DP1_SAMPLE2(b), (uint8_t)(mic_si >> 8));
	swr_write(wcd_dev, SWRS_DP1_OFFSET1(b), 0);
	swr_write(wcd_dev, SWRS_DP1_OFFSET2(b), 0);
	swr_write(wcd_dev, SWRS_DP1_LANE(b), (uint8_t)mic_lane);
	swr_write(wcd_dev, SWRS_DP1_CH_EN(b), on ? 1 : 0);
	wr(swrm + SWRM_DP_CTRL(mic_mport, b), (on ? 1u : 0u) << 24 | (mic_si & 0xff));
	wr(swrm + SWRM_DP_CTRL2(mic_mport, b), mic_lane);
	wr(swrm + SWRM_DP_HCTRL(mic_mport, b), 0xf0);
}

static void
mic_select_amic(unsigned k)
{
	/* ADC1's input: AMIC k+1 (TX_NEW_CH12_MUX bits 2:0) */
	(void)wcd_bits(WCD_TX_NEW_CH12_MUX, 0x07, (uint8_t)(k + 1));
	delay_for_interval(150, NSEC_PER_MSEC);
}

static bool
mic_swr_up(void)
{
	vm_offset_t lpi, tlmm, va;
	uint32_t v[5], gain = 10, mode = 3, rate = 0, vol = 20, hw, cfg, chen = 0;
	uint8_t id[4] = { 0 }, b, scp = 0;
	bool ok, linked = false;

	(void)PE_parse_boot_argn("dadamic_lane", &mic_lane, sizeof(mic_lane));
	(void)PE_parse_boot_argn("dadamic_si", &mic_si, sizeof(mic_si));
	(void)PE_parse_boot_argn("dadamic_mport", &mic_mport, sizeof(mic_mport));
	(void)PE_parse_boot_argn("dadamic_div2", &mic_div2, sizeof(mic_div2));
	(void)PE_parse_boot_argn("dadamic_gain", &gain, sizeof(gain));
	(void)PE_parse_boot_argn("dadamic_mode", &mode, sizeof(mode));
	(void)PE_parse_boot_argn("dadamic_rate", &rate, sizeof(rate));
	(void)PE_parse_boot_argn("dadamic_vol", &vol, sizeof(vol));
	(void)PE_parse_boot_argn("dadamic_chen", &chen, sizeof(chen));

	/* the DSP's votes and clocks: macros, digital codec, the TX core clock (0x30c, the TX macro's and its SoundWire's) and VA's */
	v[0] = 1;
	ok = prm_request(0x08001032, v, 1, "the macros vote");
	v[0] = 2;
	ok = prm_request(0x08001032, v, 1, "the digital codec vote") && ok;
	v[0] = 1; v[1] = 0x30c; v[2] = 19200000; v[3] = 1; v[4] = 0;
	ok = prm_request(0x0800102c, v, 5, "the TX core clock at 19.2 MHz") && ok;
	v[0] = 1; v[1] = 0x307; v[2] = 19200000; v[3] = 1; v[4] = 0;
	ok = prm_request(0x0800102c, v, 5, "the VA core clock at 19.2 MHz") && ok;
	if (!ok) {
		say("mic: the LPASS clocks are not all given: stopping\n");
		return false;
	}
	/* the codec's supplies (DT/mainline: L15B 1.8 V buck/rxtx/io, L2I 1.2 V px, BOB1 mic bias) */
	say("mic: supplies: L15B %s, L2I %s, BOB1 %s\n", hakios_rpmh_regulator("ldob15", 1800, true, 7) ? "on" : "NOT VOTED",
	    hakios_rpmh_regulator("ldoi2", 1200, true, 7) ? "on" : "NOT VOTED",
	    hakios_rpmh_regulator("bobb1", 0, true, 0) ? "on" : "NOT VOTED");
	delay_for_interval(5, NSEC_PER_MSEC);
	/* its reset, gpio101: gpio, 16 mA, output; low 20 us, then high */
	tlmm = ml_io_map(TLMM_PHYS + 0x1000 * 101, 0x1000);
	wr(tlmm + 4, 0);
	wr(tlmm, 7u << 6 | 1u << 9);
	us_wait(30);
	wr(tlmm + 4, 2);
	us_wait(30);
	say("mic: the codec's reset (gpio101) released: cfg %#x io %#x\n", rd(tlmm), rd(tlmm + 4));
	/* the master's pins: LPI gpio0 clock (function 1, 2 mA, no pull, slew 1), gpio1/2/14 data (same, keeper) */
	lpi = ml_io_map(LPI_TLMM_PHYS, 0x10000);
	{
		static const unsigned pins[4] = { 0, 1, 2, 14 };

		for (unsigned k = 0; k < 4; k++) {
			wr(lpi + 0x1000 * pins[k], (k ? 2u : 0u) | 1u << 2 | 1u << 11);
			wr(lpi + 0x1000 * pins[k] + 4, 0);
		}
		say("mic: SoundWire pins: gpio0 %#x gpio1 %#x gpio2 %#x gpio14 %#x\n", rd(lpi), rd(lpi + 0x1000), rd(lpi + 0x2000),
		    rd(lpi + 0xe000));
	}
	/* the VA macro's frame-sync generator (the DT's qcom,fs-gen-sequence), the TX macro's clocks, its SoundWire clock */
	va = ml_io_map(VA_MACRO_PHYS, 0x1000);
	wr(va + 0x0000, rd(va + 0x0000) | 1);
	wr(va + 0x0004, rd(va + 0x0004) | 3);
	wr(va + 0x0004, (rd(va + 0x0004) & ~3u) | 1);
	wr(va + 0x0080, rd(va + 0x0080) | 2);
	wr(va + 0x0008, rd(va + 0x0008) | 2);                   /* (the VA macro's own SoundWire clock too) */
	wr(va + 0x0008, rd(va + 0x0008) | 1);
	wr(va + 0x0008, rd(va + 0x0008) & ~2u);
	say("mic: VA core ID %02x %02x %02x; SWR_MIC_CTL %#x %#x %#x\n", rd(va + 0xc0), rd(va + 0xc4), rd(va + 0xc8), rd(va + 0xd0),
	    rd(va + 0xd4), rd(va + 0xd8));
	for (unsigned k = 0; k < 3; k++) {                     /* va_macro_probe() with a SoundWire master: the mics' clocks, divider 1 */
		wr(va + 0xd0 + 4 * k, (rd(va + 0xd0 + 4 * k) & ~0xeeu) | 0xcc);
	}
	txm = ml_io_map(TX_MACRO_PHYS, 0x1000);
	wr(txm + 0x0090, rd(txm + 0x0090) | 1);                 /* FREQ_MCLK: 9.6 MHz */
	wr(txm + 0x0000, rd(txm + 0x0000) | 1);                 /* MCLK */
	wr(txm + 0x0004, rd(txm + 0x0004) | 1);                 /* FS counter */
	wr(txm + 0x0008, rd(txm + 0x0008) | 2);                 /* SoundWire: reset, clock, reset over */
	wr(txm + 0x0008, rd(txm + 0x0008) | 1);
	wr(txm + 0x0008, rd(txm + 0x0008) & ~2u);
	say("mic: TX macro: mclk %#x fs %#x swr %#x freq %#x; VA: mclk %#x fs %#x top %#x swr %#x\n", rd(txm), rd(txm + 4),
	    rd(txm + 8), rd(txm + 0x90), rd(va), rd(va + 4), rd(va + 0x80), rd(va + 8));
	/* the master, as qcom_swrm_init() starts a v2.0 one: frame 50 x 2 (rows index 1, columns index 0) */
	swrm = ml_io_map(SWRM_PHYS, 0x6000);
	hw = rd(swrm + SWRM_COMP_HW_VERSION);
	wr(swrm + SWRM_FRAME_CTRL(0), (rd(swrm + SWRM_FRAME_CTRL(0)) & ~0xffu) | 1u << 3 | 0u);
	wr(swrm + SWRM_ENUMERATOR_CFG, 1);
	cfg = (rd(swrm + SWRM_MCP_CFG) & ~0x3e0000u) | 0x1fu << 17;
	wr(swrm + SWRM_MCP_CFG, cfg);
	wr(swrm + SWRM_LINK_MANAGER_EE, 1);
	wr(swrm + SWRM_CLK_CTRL, 1);
	wr(swrm + SWRM_CMD_FIFO_CFG, 7u | 1u << 31);
	wr(swrm + SWRM_COMP_CFG, 1);
	wr(swrm + SWRM_COMP_CFG, 2);
	wr(swrm + SWRM_INT_CLEAR, 0xffffffff);
	wr(swrm + SWRM_INT_CPU_EN, 0);                          /* (polled here) */
	wr(swrm + SWRM_COMP_CFG, 3);
	for (int i = 0; i < 100 && !linked; i++) {
		linked = rd(swrm + SWRM_LINK_STATUS) & 1;
		if (!linked) {
			us_wait(1000);
		}
	}
	say("mic: the SoundWire master (version %#x, params %#x): link %s (link status %#x, comp status %#x)\n", hw,
	    rd(swrm + SWRM_COMP_PARAMS), linked ? "UP" : "NOT UP", rd(swrm + SWRM_LINK_STATUS), rd(swrm + SWRM_COMP_STATUS));
	if (!linked) {
		return false;
	}
	/* the codec's TX device enumerated: ID ...01170223 (the DT's wcd939x-tx-slave 0x0e 0x01170223) */
	for (int waited = 0; waited < 300 && wcd_dev == 0; waited++) {
		for (uint8_t d = 1; d <= 11 && wcd_dev == 0; d++) {
			uint32_t lo = rd(swrm + SWRM_DEV_ID_1(d)), st = (rd(swrm + SWRM_SLV_STATUS) >> (2 * d)) & 3;

			if (lo == 0x01170223 && (st == 1 || st == 2)) {
				wcd_dev = d;
				say("mic: the codec's TX device is device %u (ID %#x%08x, status %u) after %d ms\n", d,
				    rd(swrm + SWRM_DEV_ID_2(d)), lo, st, waited);
			}
		}
		if (wcd_dev == 0) {
			delay_for_interval(1, NSEC_PER_MSEC);
		}
	}
	if (wcd_dev == 0) {
		say("mic: the codec did not show up on the bus: slave status %#x, interrupts %#x\n", rd(swrm + SWRM_SLV_STATUS),
		    rd(swrm + SWRM_INT_STATUS));
		for (uint8_t d = 0; d <= 11; d++) {
			if (rd(swrm + SWRM_DEV_ID_1(d)) != 0) {
				say("mic:   device %u: ID %#x%08x\n", d, rd(swrm + SWRM_DEV_ID_2(d)), rd(swrm + SWRM_DEV_ID_1(d)));
			}
		}
		return false;
	}
	for (unsigned i = 0; i < 4; i++) {
		(void)swr_read(wcd_dev, WCD_CHIP_ID0 + i, &id[i]);
	}
	(void)swr_read(wcd_dev, WCD_EFUSE_REG_0, &scp);
	say("mic: the codec's chip ID %02x %02x %02x %02x, variant %u (WCD9395 is 5)\n", id[0], id[1], id[2], id[3], (scp >> 1) & 0xf);
	/* wcd939x_bind(): the mic biases' 2750 mV (DT qcom,cdc-micbiasN-mv); wcd939x_io_init(); interrupts as edges */
	for (unsigned n = 1; n <= 4; n++) {
		(void)wcd_bits(WCD_ANA_MICB(n), 0x3f, (2750 - 1000) / 50);
	}
	(void)wcd_bits(WCD_ANA_BIAS, 0x80, 0x80);
	(void)wcd_bits(WCD_ANA_BIAS, 0x40, 0x40);
	delay_for_interval(10, NSEC_PER_MSEC);
	(void)wcd_bits(WCD_ANA_BIAS, 0x40, 0x00);
	(void)wcd_bits(WCD_CDC_DMIC_CTL, 0x02, 0x02);
	(void)wcd_bits(WCD_FE_ICTRL_STG2CASC_ULP, 0xff, 0x14);
	(void)wcd_bits(WCD_FE_ICTRL_STG2MAIN_ULP, 0x1f, 0x08);
	for (unsigned n = 1; n <= 4; n++) {
		(void)wcd_bits(WCD_MICB_TEST_CTL_1(n), 0xe0, 0xe0);
	}
	(void)wcd_bits(WCD_TX_3_4_TEST_BLK_EN2, 0x01, 0x00);
	for (unsigned i = 0; i < 3; i++) {
		swr_write(wcd_dev, WCD_INTR_LEVEL_0 + i, 0);
	}
	/* the mic biases on (wcd939x_micbias_control(MICB_ENABLE)), all four */
	(void)wcd_bits(WCD_CDC_DIG_CLK_CTL, 0xf0, 0xf0);
	(void)wcd_bits(WCD_CDC_ANA_CLK_CTL, 0x10, 0x10);
	(void)wcd_bits(WCD_CDC_ANA_TX_CLK_CTL, 0x01, 0x01);
	for (unsigned n = 1; n <= 4; n++) {
		(void)wcd_bits(WCD_MICB_TEST_CTL_2(n), 0x07, 0x01);
	}
	for (unsigned n = 1; n <= 4; n++) {
		(void)wcd_bits(WCD_ANA_MICB(n), 0xc0, 0x40);
	}
	delay_for_interval(1, NSEC_PER_MSEC);
	/* ADC1 (wcd939x_codec_enable_adc(), its gain, wcd939x_tx_swr_ctrl()'s TX clock rate in both banks) */
	(void)wcd_bits(WCD_ANA_TX_CH1, 0x1f, (uint8_t)gain);
	(void)wcd_bits(WCD_CDC_ANA_CLK_CTL, 0x08, 0x08);
	(void)wcd_bits(WCD_CDC_ANA_CLK_CTL, 0x10, 0x10);
	(void)wcd_bits(WCD_SWR_TX_CLK_RATE, 0xff, (uint8_t)(rate | rate << 4));
	/* ADC1's request (wcd939x_adc_enable_req()) */
	(void)wcd_bits(WCD_CDC_REQ_CTL, 0x02, 0x02);
	(void)wcd_bits(WCD_CDC_REQ_CTL, 0x01, 0x00);
	(void)wcd_bits(WCD_ANA_TX_CH2, 0x40, 0x40);
	(void)wcd_bits(WCD_CDC_TX_ANA_MODE_0_1, 0x0f, (uint8_t)mode);
	(void)wcd_bits(WCD_CDC_DIG_CLK_CTL, 0x10, 0x10);
	(void)wcd_bits(WCD_ANA_TX_CH2, 0x40, 0x00);
	if (chen) {                                             /* (a test: the TX channels' enable bits by hand) */
		(void)wcd_bits(WCD_ANA_TX_CH2, 0x80, 0x80);
		(void)wcd_bits(0x3011, 0x80, 0x80);
	}
	if (swr_broken) {
		return false;
	}
	/* the stream: ports in the bank not in use, frame 50 x 16 there, the banks switched, then the other bank too */
	b = (rd(swrm + SWRM_MCP_STATUS) & 1) ? 0 : 1;
	swr_write(wcd_dev, SWRS_DP1_BLOCK_CTRL1, 0);            /* word length 1 */
	wr(swrm + SWRM_DP_BLOCK_CTRL1(mic_mport), 0);
	swr_ports(b, true);
	wr(swrm + SWRM_FRAME_CTRL(b), (rd(swrm + SWRM_FRAME_CTRL(b)) & ~0xffu) | 1u << 3 | 7u);
	swr_write(0xf, SWRS_SCP_FRAME_CTRL(b), 1u << 3 | 7u);
	say("mic: banks switched: the master is in bank %u (asked for %u)\n", rd(swrm + SWRM_MCP_STATUS) & 1, b);
	swr_ports(b ? 0 : 1, true);
	wr(swrm + SWRM_FRAME_CTRL(b ? 0 : 1), (rd(swrm + SWRM_FRAME_CTRL(b ? 0 : 1)) & ~0xffu) | 1u << 3 | 7u);
	(void)swr_read(wcd_dev, SWRS_SCP_CONTROL, &scp);
	{
		uint8_t ch = 0, si = 0, lane = 0;

		(void)swr_read(wcd_dev, SWRS_DP1_CH_EN(b), &ch);
		(void)swr_read(wcd_dev, SWRS_DP1_SAMPLE1(b), &si);
		(void)swr_read(wcd_dev, SWRS_DP1_LANE(b), &lane);
		say("mic: the codec's DP1 (bank %u): channels %#x, interval %u, lane %u; SCP status %#x; master port %u ctrl %#x lane %#x; "
		    "interrupts %#x\n", b, ch, si, lane, scp, mic_mport, rd(swrm + SWRM_DP_CTRL(mic_mport, b)),
		    rd(swrm + SWRM_DP_CTRL2(mic_mport, b)), rd(swrm + SWRM_INT_STATUS));
	}
	/* the TX macro's decimator 0 from SWR_MIC0 (an analog mic), 48 kHz; on, muted; high-pass 150 Hz; unmuted */
	wr(txm + 0x0404, rd(txm + 0x0404) & ~0x87u);            /* not DMIC, mode 0 */
	wr(txm + 0x0100, (rd(txm + 0x0100) & ~0x0fu) | 1);      /* SMIC MUX0: SWR_MIC0 */
	wr(txm + 0x0104, (rd(txm + 0x0104) & ~0x03u) | 1);      /* DEC0 MUX: SWR_MIC */
	wr(txm + 0x0400, (rd(txm + 0x0400) & ~0x0fu) | 4);
	wr(txm + 0x0400, rd(txm + 0x0400) | 0x10);
	wr(txm + 0x0400, rd(txm + 0x0400) | 0x20);
	wr(txm + 0x0404, (rd(txm + 0x0404) & ~0x60u) | 2u << 5);
	wr(txm + 0x0418, (rd(txm + 0x0418) & ~3u) | 2);
	wr(txm + 0x0418, (rd(txm + 0x0418) & ~3u) | 1);
	delay_for_interval(6, NSEC_PER_MSEC);
	wr(txm + 0x040c, vol);
	delay_for_interval(100, NSEC_PER_MSEC);
	wr(txm + 0x0400, rd(txm + 0x0400) & ~0x10u);
	say("mic: TX decimator 0: mux %#x/%#x ctl %#x cfg0 %#x sec2 %#x vol %#x\n", rd(txm + 0x100), rd(txm + 0x104), rd(txm + 0x400),
	    rd(txm + 0x404), rd(txm + 0x418), rd(txm + 0x40c));
	{
		static const uint16_t regs[] = { 0x3001, 0x300e, 0x300f, 0x3010, 0x3011, 0x3012, 0x3013, 0x3022, 0x3023, 0x3025, 0x3026,
			                         0x3127, 0x3128, 0x3405, 0x3406, 0x3408, 0x3409, 0x3410, 0x3417, 0x3457, 0x345a, 0x34d0 };
		char line[300];
		int n = 0;

		for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
			uint8_t x = 0xee;

			(void)swr_read(wcd_dev, regs[i], &x);
			n += snprintf(line + n, sizeof(line) - (size_t)n, " %04x=%02x", regs[i], x);
		}
		say("mic: codec:%s\n", line);
	}
	return true;
}

static void
mic_swr(void)
{
	if (mic_swr_up()) {
		mic_capture(1, 4, mic_select_amic, 5, "AMIC");
		say("mic: afterwards: SoundWire interrupts %#x, slave status %#x\n", rd(swrm + SWRM_INT_STATUS), rd(swrm + SWRM_SLV_STATUS));
	}
}

/*
 * dadamic=3: the microphone for iOS. /dev/redwood-mic gives RedwoodAudio.driver's input device "Codec"
 * (userland/src/audio_plugin.c) what the microphone hears: 16-bit samples, one channel, 48 kHz, as redwood_mic.c gave it on
 * redwood. The path (mic_swr_up(): the codec, SoundWire, the TX macro) comes up the first time iOS opens the device and stays
 * up; the DSP's graphs run only while the device is open, so the microphone is listened to only while iOS records. A read
 * never waits: it gives what has come, keeping the reader about 40 ms behind the DSP; the plug-in makes up the rest with silence.
 * Boot-arg dadamic_amic=N: which of the codec's analog inputs (1..5) is iOS's microphone (1).
 */
#define MRING_FRAMES            16384           /* 341 ms; a power of two */
#define MRING_KEEP              1920            /* 40 ms behind the DSP */
#define MRING_MOST              4800            /* more than 100 ms waiting: the oldest is skipped */

static int16_t *mring;
static volatile uint64_t mring_written;
static uint64_t mring_read, mring_skipped, mring_dry;
static bool mring_primed;
static volatile uint32_t mic_ios_wants;

static int
mic_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint64_t written = __atomic_load_n(&mring_written, __ATOMIC_ACQUIRE), avail, want;
	int error = 0;

	if (mring == NULL || uio_resid(uio) < 2) {
		return 0;
	}
	want = (uint64_t)uio_resid(uio) / 2;
	avail = written - mring_read;
	if (avail > MRING_MOST) {
		mring_skipped += avail - MRING_KEEP;
		mring_read = written - MRING_KEEP;
		avail = MRING_KEEP;
	}
	if (!mring_primed) {
		if (avail < MRING_KEEP) {
			return 0;
		}
		mring_primed = true;
	}
	if (avail < want) {
		mring_dry++;
		mring_primed = false;
	}
	while (want > 0 && avail > 0 && error == 0) {
		uint64_t at = mring_read % MRING_FRAMES, n = want < avail ? want : avail;

		n = n < MRING_FRAMES - at ? n : MRING_FRAMES - at;
		error = uiomove((caddr_t)&mring[at], (int)(n * 2), uio);
		mring_read += n;
		want -= n;
		avail -= n;
	}
	return error;
}

static int
mic_dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	mic_ios_wants = 1;
	wakeup(&mic_ios_wants);
	return 0;
}

static int
mic_dev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	mic_ios_wants = 0;
	return 0;
}

static const struct cdevsw mic_cdevsw = {
	.d_open = mic_dev_open, .d_close = mic_dev_close, .d_read = mic_dev_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
mic_ios_thread(void *arg __unused, wait_result_t wr_ __unused)
{
	uint32_t amic = 1, recordings = 0;
	bool up = false;

	(void)PE_parse_boot_argn("dadamic_amic", &amic, sizeof(amic));
	for (;;) {
		uint32_t asked = 0, seen = 0, peak = 0;
		uint64_t began, ns, frames = 0;

		while (!mic_ios_wants) {
			(void)tsleep(&mic_ios_wants, PZERO, "micwait", MAX(1, (int)(200 * hz / 1000)));
		}
		recordings++;
		if (!up) {
			up = mic_swr_up();
			if (!up) {
				say("mic: the microphone's path did not come up: iOS hears silence\n");
				return;
			}
			if (amic != 1) {
				mic_select_amic(amic - 1);
			}
			say("mic: the path is up (AMIC%u is iOS's microphone)\n", amic);
		}
		began = mach_absolute_time();
		if (!mic_graphs_start(1, 4)) {
			say("mic: iOS records (%u), but the DSP's graphs did not start\n", recordings);
			delay_for_interval(1000, NSEC_PER_MSEC);
			continue;
		}
		mring_primed = false;
		mring_skipped = mring_dry = 0;
		mring_read = __atomic_load_n(&mring_written, __ATOMIC_ACQUIRE);
		mic_done = 0;
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		say("mic: iOS records (%u): the graphs started in %llu ms\n", recordings, ns / NSEC_PER_MSEC);
		began = mach_absolute_time();
		while (asked < MIC_SLOTS) {
			(void)mic_read(asked % MIC_SLOTS);
			asked++;
		}
		while (mic_ios_wants) {
			(void)tsleep(&mic_event, PZERO, "micios", MAX(1, (int)(100 * hz / 1000)));
			while (seen < mic_done) {
				const volatile int16_t *x = (const volatile int16_t *)(buffer + MIC_OFFSET + (seen % MIC_SLOTS) * MIC_PERIOD);
				uint64_t at = mring_written;

				for (uint32_t i = 0; i < PERIOD_FRAMES; i++) {
					int32_t a = x[i] < 0 ? -x[i] : x[i];

					peak = MAX(peak, (uint32_t)a);
					mring[(at + i) % MRING_FRAMES] = x[i];
				}
				__atomic_store_n(&mring_written, at + PERIOD_FRAMES, __ATOMIC_RELEASE);
				frames += PERIOD_FRAMES;
				(void)mic_read(seen % MIC_SLOTS);
				seen++;
			}
		}
		for (unsigned i = 0; i < 6 && mic_done < seen + MIC_SLOTS; i++) {
			(void)tsleep(&mic_event, PZERO, "micios", MAX(1, (int)(50 * hz / 1000)));
		}
		mic_graphs_stop();
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
		say("mic: iOS stopped recording (%u) after %llu ms: %llu ms heard, peak %u; %llu frames skipped, %llu reads short\n", recordings,
		    ns / NSEC_PER_MSEC, frames * 1000 / RATE, peak, mring_skipped, mring_dry);
	}
}

/* dadamic_selftest=N: as if iOS recorded twice for N seconds (the device opened and closed), what came read as the plug-in would */
static void
mic_selftest_thread(void *arg __unused, wait_result_t wr_ __unused)
{
	uint32_t secs = 3;

	(void)PE_parse_boot_argn("dadamic_selftest", &secs, sizeof(secs));
	for (unsigned round = 1; round <= 2; round++) {
		uint64_t got = 0, start, ns = 0;
		int16_t chunk[480];
		uint32_t peak = 0;

		delay_for_interval(2000, NSEC_PER_MSEC);
		start = mach_absolute_time();
		(void)mic_dev_open(0, 0, 0, NULL);
		while (ns < (uint64_t)secs * NSEC_PER_SEC) {
			uio_t uio;

			delay_for_interval(10, NSEC_PER_MSEC);
			uio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);
			if (uio == NULL) {
				break;
			}
			uio_addiov(uio, (user_addr_t)(uintptr_t)chunk, sizeof(chunk));
			(void)mic_dev_read(0, uio, 0);
			for (user_ssize_t i = 0; i < ((user_ssize_t)sizeof(chunk) - uio_resid(uio)) / 2; i++) {
				peak = MAX(peak, (uint32_t)(chunk[i] < 0 ? -chunk[i] : chunk[i]));
			}
			got += (uint64_t)((user_ssize_t)sizeof(chunk) - uio_resid(uio)) / 2;
			uio_free(uio);
			absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
		}
		(void)mic_dev_close(0, 0, 0, NULL);
		say("mic: self-test round %u: %llu frames read in %u s (%llu%%), peak %u\n", round, got, secs,
		    got * 100 / ((uint64_t)secs * RATE), peak);
	}
}

static void
mic_for_ios(void)
{
	int major = cdevsw_add(-1, &mic_cdevsw);
	thread_t thread;

	mring = kalloc_data(MRING_FRAMES * sizeof(int16_t), Z_WAITOK | Z_ZERO);
	if (mring == NULL || major < 0 ||
	    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "redwood-mic") == NULL) {
		say("mic: /dev/redwood-mic cannot be made\n");
		return;
	}
	if (kernel_thread_start(mic_ios_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		say("mic: /dev/redwood-mic is iOS's microphone: it listens only while iOS records\n");
	}
	if (PE_parse_boot_argn("dadamic_selftest", &major, sizeof(major)) &&
	    kernel_thread_start(mic_selftest_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

static void
mic_test(void)
{
	vm_offset_t lpi, va;
	uint32_t v[5], mclk = 0, div_index;
	bool ok;

	/* the DSP's votes: LPASS macros (1), digital codec (2), then the VA core clock (0x307) */
	v[0] = 1;
	ok = prm_request(0x08001032, v, 1, "the macros vote");
	v[0] = 2;
	ok = prm_request(0x08001032, v, 1, "the digital codec vote") && ok;
	if (!ok) {
		say("mic: no PRM votes: the LPASS registers stay unclocked, stopping\n");
		return;
	}
	for (unsigned i = 0; i < 2 && mclk == 0; i++) {
		v[0] = 1; v[1] = 0x307; v[2] = i == 0 ? 19200000 : 9600000; v[3] = 1; v[4] = 0;
		if (prm_request(0x0800102c, v, 5, i == 0 ? "the VA core clock at 19.2 MHz" : "the VA core clock at 9.6 MHz")) {
			mclk = v[2];
		}
	}
	if (mclk == 0) {
		say("mic: no VA core clock\n");
		return;
	}
	/* the pins: gpio6 DMIC0/1 clock (function 1, 8 mA, output), gpio7 its data (function 1, 8 mA) */
	lpi = ml_io_map(LPI_TLMM_PHYS, 0x17000);
	{
		/* all four pairs (clock, data): DMIC0/1 gpio6/7, DMIC2/3 gpio8/9, DMIC4/5 gpio12/13, DMIC6/7 gpio17/18 */
		static const uint32_t pairs[4][2] = { { 6, 7 }, { 8, 9 }, { 12, 13 }, { 17, 18 } };

		for (unsigned k = 0; k < 4; k++) {
			wr(lpi + 0x1000 * pairs[k][0], 1u << 2 | 3u << 6 | 1u << 9);
			wr(lpi + 0x1000 * pairs[k][0] + 4, 2);
			wr(lpi + 0x1000 * pairs[k][1], 1u << 2 | 3u << 6);
		}
		say("mic: pins: gpio6 %#x gpio7 %#x gpio8 %#x gpio12 %#x gpio17 %#x\n", rd(lpi + 0x6000), rd(lpi + 0x7000),
		    rd(lpi + 0x8000), rd(lpi + 0xc000), rd(lpi + 0x11000));
	}
	/* the VA macro */
	va = ml_io_map(VA_MACRO_PHYS, 0x1000);
	mic_va = va;
	wr(va + 0x0000, rd(va + 0x0000) | 1);                   /* MCLK */
	wr(va + 0x0004, rd(va + 0x0004) | 3);                   /* FS counter on, cleared */
	wr(va + 0x0004, (rd(va + 0x0004) & ~2u) | 1);
	wr(va + 0x0080, rd(va + 0x0080) | 2);                   /* FS broadcast */
	wr(va + 0x0094, rd(va + 0x0094) & ~0x80u);              /* DMICs out of reset (DMIC_CFG bit 7, set at reset) */
	div_index = mclk == 19200000 ? 4 : 2;                   /* divide by 8 or 4: 2.4 MHz */
	for (unsigned k = 0; k < 4; k++) {                     /* the DMIC clocks of all four pairs (0x84 .. 0x90) */
		wr(va + 0x0084 + 4 * k, (rd(va + 0x0084 + 4 * k) & ~0xfu) | div_index << 1 | 1);
	}
	wr(va + 0x0100, (rd(va + 0x0100) & ~0xf0u) | 1u << 4);  /* decimator 0 from DMIC0 */
	wr(va + 0x0104, rd(va + 0x0104) & ~1u);                 /* ... a VA DMIC, not SoundWire */
	wr(va + 0x0404, rd(va + 0x0404) | 0x80);                /* TX0: DMIC */
	wr(va + 0x0400, (rd(va + 0x0400) & ~0xfu) | 4);         /* 48 kHz */
	wr(va + 0x0400, rd(va + 0x0400) | 1u << 5);             /* TX0 clock */
	wr(va + 0x0418, rd(va + 0x0418) & ~1u);                 /* HPF zero gate */
	delay_for_interval(1, NSEC_PER_MSEC);
	wr(va + 0x0404, (rd(va + 0x0404) & ~0x60u) | 2u << 5);  /* HPF 150 Hz */
	wr(va + 0x0418, rd(va + 0x0418) | 2);
	delay_for_interval(1, NSEC_PER_MSEC);
	wr(va + 0x0418, rd(va + 0x0418) & ~2u);
	delay_for_interval(1, NSEC_PER_MSEC);
	wr(va + 0x0418, rd(va + 0x0418) | 1);                   /* no gate */
	delay_for_interval(6, NSEC_PER_MSEC);
	wr(va + 0x040c, 40);                                    /* +40 dB */
	say("mic: VA macro: dmic_cfg %#x mclk %#x fs %#x top %#x dmic0 %#x mux %#x/%#x tx0 ctl %#x cfg0 %#x sec2 %#x vol %#x\n",
	    rd(va + 0x94), rd(va + 0), rd(va + 4), rd(va + 0x80), rd(va + 0x84), rd(va + 0x100), rd(va + 0x104), rd(va + 0x400), rd(va + 0x404),
	    rd(va + 0x418), rd(va + 0x40c));
	mic_capture(3, 1, mic_select_dmic, 8, "DMIC");
}

static void
audio_thread(void *arg __unused, wait_result_t wr_ __unused)
{
	const char *e;

	hakios_adsp_gpr_event = gpr_event;
	if (!dsp_ready()) {
		return;
	}
	for (unsigned i = 0; i < 3; i++) {
		static const uint16_t streams[] = { 0x1001, 0x1081, 0x1061 };

		if ((e = smmu_bypass(streams[i])) != NULL) {
			say("stream %#x: %s\n", streams[i], e);
			if (i == 0) {
				return;
			}
		}
	}
	pins();
	if (!hakios_dada_amp_setup()) {
		say("no amplifier: no sound\n");
		return;
	}
	if (!graphs_up()) {
		return;
	}
	/* prepared and started device first, then the stream */
	if (!graph_command(OP_GRAPH_PREPARE, SG_SPEAKER, "the device graph prepared") ||
	    !graph_command(OP_GRAPH_PREPARE, SG_PLAY, "the stream graph prepared") ||
	    !graph_command(OP_GRAPH_START, SG_SPEAKER, "the device graph started") ||
	    !graph_command(OP_GRAPH_START, SG_PLAY, "the stream graph started")) {
		return;
	}
	say("the graphs run\n");
	hakios_dada_amp_status("before");
	hakios_dada_amp_power(true);
	{
		uint32_t next = 0, periods = RATE / 2 / PERIOD_FRAMES;
		uint64_t start = mach_absolute_time(), end, ns;

		while (next < periods) {
			while (next < periods && sent_count - done_count < IN_FLIGHT) {
				tone_period(next % SLOTS);
				if (!send_period(next % SLOTS)) {
					say("the test tone: a period could not be sent\n");
					return;
				}
				next++;
			}
			wait_done(200);
			if (done_count == 0 && mach_absolute_time() - start > 0) {
				absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
				if (ns > 2 * NSEC_PER_SEC) {
					break;
				}
			}
		}
		for (unsigned i = 0; i < 20 && done_count < sent_count; i++) {
			wait_done(100);
		}
		end = mach_absolute_time();
		absolutetime_to_nanoseconds(end - start, &ns);
		say("the test tone: %u periods sent, %u done (%u refused, status %#x) in %llu ms (%llu ms of sound)\n", sent_count,
		    done_count, done_bad, last_bad_status, ns / NSEC_PER_MSEC, (uint64_t)done_count * PERIOD_FRAMES * 1000 / RATE);
	}
	hakios_dada_amp_status("with the tone");
	hakios_dada_amp_power(false);
	if (done_count == 0) {
		say("the DSP took no period: what iOS plays does not go to it\n");
		return;
	}
	{
		uint32_t mic = 0;

		if (PE_parse_boot_argn("dadamic", &mic, sizeof(mic)) && mic) {
			if (mic == 3) {
				mic_for_ios();
			} else if (mic == 2) {
				mic_swr();
			} else {
				mic_test();
			}
		}
	}
	ring = (int16_t *)kalloc_data(RING_FRAMES * CHANNELS * 2, Z_WAITOK | Z_ZERO);
	if (ring == NULL) {
		return;
	}
	redwood_audio_set_sink("the speakers (the audio DSP, dada)", sink);
	feeder();
}

void hakios_dada_audio_init(void);

void
hakios_dada_audio_init(void)
{
	uint32_t on = 0;
	uint64_t low = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("dadaaudio", &on, sizeof(on)) || on == 0) {
		return;
	}
	smmu = ml_io_map(APPS_SMMU_PHYS, APPS_SMMU_SIZE);
	smmu_look();
	if (on < 2) {
		return;
	}
	if (!PE_parse_boot_argn("lowdma", &low, sizeof(low)) || low == 0 || (low & 0xfff) || low + BUFFER_BYTES > 0x100000000ULL) {
		say("no memory below 4 GB from the loader (lowdma=): no sound\n");
		return;
	}
	buffer_pa = low;
	dsp_address = low | 1ull << 32;
	buffer = (volatile uint8_t *)ml_io_map_wcomb(low, BUFFER_BYTES);
	for (uint32_t i = 0; i < BUFFER_BYTES; i += 4) {
		*(volatile uint32_t *)(buffer + i) = 0;
	}
	say("our memory for the DSP: %#llx (the DSP's %#llx)\n", buffer_pa, dsp_address);
	if (kernel_thread_start(audio_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
