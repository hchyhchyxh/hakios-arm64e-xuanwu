# Wi-Fi bring-up kernel tree (2026-11-10, 22:10)

State on the phone at the time of writing: the peach chip (PCI 17cb:110e) is enumerated over
PCIe, the MHI boot loader runs (`waiting for READY`, `contexts`, `loading the image`) and the
7,000,128-byte `amss.bin` is accepted by BHIe; the firmware does not reach mission mode within
the 40 s the loop allows (`error 6 in waiting for mission mode`, state 2, BHI status
`0x80005eed` = success). That is the one step left in the kernel before the ath12k driver
(stage 4, `dadawifi=4`) can come up.

## How this tree is built

1. `docs/xuanyuan/tools/make-isolated-tree.sh` (a fresh copy of the tracked source).
2. copy `docs/xuanyuan/recovered-20261109/` over it (the GPI touch driver and friends), with
   `redwood_input.c`'s `HAKIOS_ENABLE_ATH12K` guard changed to `!defined(HAKIOS_NO_ATH12K)`.
3. apply `xnu-framebuffer-stop-text-console.patch` (or the kernel console paints over the UI).
4. in `bsd/conf/files`: `hakios_dada_wifi.c` and `hakios_dada_wlan.c` are `standard` (not
   `optional hakios_ath12k`), `redwood_goodix.c` is replaced by `redwood_goodix_stub.c`.
5. `iokit/IOKit/IOUserServer.h` gets `Panic_Impl` and `kIOUserServrMaxPanicReasonLength` (the
   iig stand-in on Linux cannot generate them for xnu-12377).
6. apply `build-tree-vs-tracked.diff` (this file) for the Wi-Fi driver changes.
7. build `ath12k.o` with `base/tools/mk-ath12k.sh`, then
   `docs/xuanyuan/tools/rebuild-verified-kernel.sh`.

The Wi-Fi driver changes in the diff are the ones found by running it on the phone:

- `dma_zero`/`dma_copy`: bzero/DC ZVA and unaligned or merged stores to the write-combining DMA
  window fault ("Unaligned kernel data abort") -- plain single-aligned 32-bit stores.
- MHI contexts and `events_ack`: 64-bit fields sit at 4-byte-aligned offsets in the same window;
  write/read them as two 32-bit accesses in inline asm (the compiler merges 4+4 stores into an
  8-byte store otherwise).
- `mhi_boot_open`: BHI (0x100) and BHIe (0x224) live inside the 2 MB BAR while REGLEN is 0x100;
  check the offsets against the BAR window, not REGLEN.
- the RPMh TCS registers are only valid once the audio DSP bring-up has run the dada sequencer
  setup: boot with `adsp=1 dadaadsp=1` (without them the first `hakios_rpmh_regulator` call
  panics with an SError).
- progress prints left in (`dwifi: link: ...`, `fw step: ...`) - they are how this was debugged.

## Boot arguments used

    cpus=6 fb_threads=4 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 \
      xuanyuantouch=1 dadatouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 \
      dadabuttons=1 buttons_steps=5 caenv=CA_DISABLE_ASYNCHRONOUS=1 clocka11y=1 \
      dadagpu=6 dadagpu_keep=1 metal=1 metal_procs=backboardd metal_arch=0x01000017,0xc camtl=1 \
      adsp=1 dadaadsp=1 dadawifi=3

Kernel binary of this state: `X:/ios/work/kernel-backup/mach.xuanyuan.wifi-mhi-20261110`.

## MHI: where exactly it stops (2026-11-10, 22:25)

Both firmware revisions behave the same (`dadawifi_fw=1` amss.bin 7,000,128 bytes and
`dadawifi_fw=2` amss20.bin 6,930,496 bytes):

    the boot loader asked for (execution environment 0, state 0): loading the boot loader
    waiting for READY (execution environment 0, state 0, status 0x80005eed)
    contexts (execution environment 1, state 1, status 0x80005eed)
    loading the image (execution environment 1, state 1, status 0x80005eed)
    waiting for mission mode (execution environment 1, state 2, status 0x80005eee)
    the end: failed (error 6 in waiting for mission mode; execution environment 1, state 2,
      BHI status 0x80005eed error 0; 3 event rings given back)

so: BHI loads the SBL, BHIe accepts the whole image (transfer status is SUCCESS, seq increments),
but the device stays in execution environment 1 (SBL) and never reports AMSS/EE 2, which is what
the loop waits for (MhiWaitMission -> MhiDone only on EE 2/3).

Hypotheses, in order of how cheap they are to test:

1. **Timeout**: the loop allows 8000 * 5 ms = 40 s; try a longer wait (and poll `MHI_STATUS`
   too, which the log's "state" already decodes).
2. **Image table shape for peach**: `mhi_bhie_table` splits by `SEGMENT`; check the SBL's
   accepted entry count/size against what Linux's mhi does for WCN7850-class devices (BHIe
   table entries may need 4 KB-multiple lengths or a segment count limit).
3. **Something the host must write after the image transfer**: compare Linux's
   `mhi_pci_boot` (WCN7850) and UniXNU's MhiBoot for the step between BHIe success and AMSS
   execution (EE transition can depend on MHI_CTRL/MHICTRL or a CHDB doorbell).
4. The old project logs (`wifi30`-`wifi42`, 28 October) mention the firmware "dying at its first
   radio work", which implies it did reach AMSS on this phone once; if those logs are still on
   disk, diff their MHI sequence against this one.

## Mission mode: reached (2026-11-10, 22:40)

The loop's success condition was what held it. The phone's log shows the device moving state
1 -> 2 right after BHIe accepts the image, and in MHI state 2 is M0 (mission mode); the
execution-environment register keeps reporting 1 (SBL). mhi_boot_step only accepted
EE==AMSS/WFW, so a healthy firmware was declared a timeout. Accepting state == MhiStateM0
turns the sequence into:

    loading the image (...) -> done (execution environment 1, state 2) -> the end: done

Stage 4 (dadawifi=4) then enters the ath12k driver: it reads SoC hardware version
0x40210200 (revision 2 -> amss20.bin), maps BAR 1024 KiB, finds the MSI capability at 0x50,
the PCIe capability at 0x70, and hands the driver the 124 MB DMA window, then the kernel hits
Kernel instruction fetch abort at pc 0x50f39970075c0ec4 -- the pc is PAC-tagged, i.e. a
function-pointer call across the ath12k.o / kernel boundary that is not being authenticated
the way the pointer was signed. Next: check mk-ath12k.sh's compile flags (arm64e vs arm64,
-mkernel) and the nw_wifi/platform-ops call sites for the arm64e ABI.
