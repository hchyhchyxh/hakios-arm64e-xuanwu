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

## ath12k start: the call back into the platform (2026-11-10, 22:45)

With mission mode reached, `dadawifi=4` prints

    dwifi: ath12k: SoC hardware version 0x40210200: revision 2, firmware ..20
    dwifi: ath12k: starting the driver: BAR 1024 KiB, MSI capability 0x50, PCIe capability 0x70, DMA 124 MB at 0xc7bd4000, sink 0xc7bd4000

and then panics with `Kernel instruction fetch abort at pc 0x50f39970075c0ec4`. Masking the
low 32 bits gives 0xfffffff0075c0ec4, which is exactly `_cfg_read` -- the first time the ath12k
object calls one of the platform's function pointers (`board.cfg_read`), the pc ends up with
junk in its upper bits. So the failure is at the boundary where the ath12k object calls back
into the kernel's function pointers, not in the PCIe/MHI code.

Two things checked: the `lx_platform`/`nw_board` layout in
`base/xnu-deps/build/ath12k/ath12kwifi/include/nw_abi.h` is field-for-field identical to the
copy in `hakios_dada_wifi.c`, and the ath12k objects are compiled by the Ubuntu clang 18 (which
rejects every `-fptrauth-*` flag, so they carry no pointer-authentication metadata at all;
signing mismatch through clang options is ruled out -- but it also means the interworking is
decided by `ld -r -arch arm64e` and by how the pointers are used).

Next to try, cheapest first:

1. dump the `board` structure bytes right before `nw_wifi_start` and compare the seven pointers
   with the addresses of `cfg_read`/`cfg_write`/`msi_target`/`map_phys`;
2. make `cfg_read` log its arguments and see whether it is even entered (if it is, the abort is
   inside it and the pc value is a red herring);
3. build the ath12k object with the same xnu-clang the kernel uses
   (`base/xnu-deps/host/bin/xnu-clang`) and the kernel's own flags, in case arm64e code
   generation differs from the Ubuntu clang's.

## QMI worked (2026-11-10, 22:55) -- the last fix that unlocked it

Two fixes on top of mission mode:

1. **Function pointers across the ath12k object**: the kernel is arm64e and signs a function
   pointer when it is stored into a typed field (the dumped board showed
   cfg_read=0x699ad1f0075c0fec where the real one is 0xfffffff0075c0fec). The ath12k object is
   built by the Ubuntu clang 18, which has no pointer authentication at all, so its plain lr
   jumped to the signed address (Kernel instruction fetch abort at pc ...075c0fec = _cfg_read).
   Fix: platform_setup/the board fill use ptrauth_strip on the function, copy the *bit
   pattern* with memcpy (a plain assignment is signed again), and mk-ath12k.sh keeps using the
   plain clang. Both sides then agree on an unsigned address.

2. **dwlan en0** comes up while the Wi-Fi thread runs (dwlan: en0 made, the radio behind it).

With that, dadawifi=4 talks to the firmware:

    chip_id 0x2 chip_family 0x4 board_id 0xff soc_id 0x40210200
    fw_version 0x100b802f fw_build_timestamp 2026-06-23 13:03
      fw_build_id QC_IMAGE_VERSION_STRING=WLAN.GNG.1.0.c2-00047-QCAGNGSWPL_V1.0_V2.0_SILICONZ-1.92400.35.101296.92
    qmi BDF download sequence completed          (regdb.bin)
    board data: board id 0xff, bdwlan.elf -> bd_o3.elf 125280 bytes
    qmi BDF download sequence completed
    boot firmware request .../m3.bin size 290816
    boot firmware request .../aux_ucode.bin size 61440
    ath12k: stage 2, scans 0, networks seen 0, error 0

so the firmware answers QMI, takes the board data and the M3 image and the driver reaches its
stage 2. The session then ends with a secondary Halt/Restart Timed Out @IOPlatformExpert.cpp:887
(no first-cause text reached the loader's log). Next session: capture the first panic (boot with
the debug flag that keeps it in the debugger / raise the poll rate), then watch stage 2 -> ready
and the scan.

Kernel of this state: kernel-backup/mach.xuanyuan.wifi-rawfn-20261110.

## Next test prepared: MSI programming and panic first-cause capture (22:56)

Two additions wait for the phone to come back:

1. msi_program(pa) -- the endpoint's MSI capability (found at 0x50) is now pointed at the DMA
   sink page and enabled, called from firmware() right after endpoint_setup(). Theory: stage 3
   alone dies right after 'the end: done' (no ath12k involved), which fits the firmware's first
   MHI interrupt being an MSI write to a capability we never programmed. If the theory holds,
   stage 3 should now stay up.
2. Panic first-cause capture: debug.c calls a weak hakios_panic_note() immediately after its
   own PE_panic_hook (both panic paths), and the Wi-Fi driver keeps
   hakios_wifi_progress/hakios_wifi_where (1 = link up, 10+phase = firmware, 100 =
   nw_wifi_start, 200 = nw_wifi_step). The hook prints "dadapanic: <reason> | wifi: progress N
   where ..." to the console, which the loader does keep.

Kernel of this state: kernel-backup/mach.xuanyuan.wifi-panichook-20261110; diffs of both files
against the tracked tree in this directory (diff-wifi-mhi-msi-pac.diff,
diff-debug-panicnote.diff).
