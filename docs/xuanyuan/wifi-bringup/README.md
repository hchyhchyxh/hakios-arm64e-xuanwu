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

## MSI programmed and the panic hook works (2026-11-10, 23:05)

msi_program() now runs: msi: capability 0x50 programmed to 0xc8310000: control 0x32b
(one message, enabled; the target is the DMA sink page), and the whole firmware sequence still
ends 	he end: done. Two unaligned configuration-space accesses had to be fixed on the way (a
32-bit store at cap+2 for the 16-bit message control, and a 32-bit read at the same offset in the
debug print): both fault as 'Unaligned kernel data abort'.

The panic hook reaches the loader now (kernel printf does not; redwood_tl does):

    dwifi: dadapanic: Halt/Restart Timed Out @%s:%d | wifi: progress 18 where "done"

progress 18 = firmware phase 8 (done). So at the time the panic text is raised the firmware has
already finished; the panic is the platform expert's halt/restart path timing out
(IOPlatformExpert.cpp:887), not the Wi-Fi code itself. The next question is who asks for that
restart (the log's own 'psci: cpu 1 ends the system: the return to the boot loader is the boot
processor's to make' line is the same path), and the several repeated firmware cycles in the
captured log are probably successive boots after that restart.

Next: find the halt/restart caller (PEHaltRestart / PE_cpu_halt / the loader's side), and check
whether the panic is a consequence of the firmware/MSI still raising interrupts after the Wi-Fi
thread has finished -- e.g. mask MSI in the capability before returning, and see if the restart
disappears.

## The firmware sits in M0, alive, never finishing boot (2026-11-11, 00:44)

Periodic MHI/BHI state reads from the driver loop, after the whole QMI handshake (host cap,
request mem, respond mem, fw mem ready, regdb, board BDF, m3 = phy_ucode20.elf, aux, qdss):

    nw mhi state: status 0x201 ctrl 0x200 bhi 0x100 0x224     (constant)

status 0x201: MHI state 2 = M0, READY bit set, SYS_ERR clear. So the endpoint is up and in
mission mode; it simply never raises the readiness indication. A raw qmi rx log in the QMI library
shows only two firmware indications ever arrive: 0x0035 (request memory) and 0x0037 (fw memory
ready). No 0x0038 (fw_ready) and no fw_init_done class indication either.

Vendor cnss2 confirmation (sources in cnss2-ref/): the request order and the file choices above
are exactly Android's, and wlan cfg/mode are sent by qcacld only after readiness -- so our
early-send error 90 was a wrong-state artifact, not the blocker.

Remaining differences being chased: (a) cnss2 enables fw_init_done instead of fw_ready ("replaced
by fw_init_done in HST/HSP"); ath12k enables both, so the class is covered; (b) qdss trace config
is missing on our side and cannot be pulled from the phone (vendor/firmware unreadable to shell);
(c) host-cap fields cnss2 also sets: wake_msi, host_build_type, platform_name, pcie_link_info,
ddr_range. Kernel of this state: mach.xuanyuan.wifi-mhistate-20261111
(sha256 f4d948f99f4ffac257b991592284d45f47d73080f15e921c52708d2787724996).

## cal_done, and five minutes of waiting (2026-11-11, 01:06)

Two more checks against the parked-in-M0 firmware:

- Host-cap cal_done forced to 1 (instead of the fresh-boot 0): the firmware behaves exactly the
  same (only raw QMI indications 0x0035 and 0x0037, MHI status stays 0x201 = M0). So the missing
  readiness is not the cold-boot-calibration state.
- Same image observed for 5 minutes (poll every 2 s): no new QMI indication, no MHI state change,
  no SYS_ERR. The endpoint stays in M0 forever, serving the QMI requests it does answer.

Also tried reading the phone's own firmware to compare with the image we extracted from the
Xiaomi 15 vendor files: /vendor/firmware is shell-unreadable, and /vendor/firmware_mnt could not
be inspected because the device dropped off USB again during the attempt (needs a replug).
Kernel of this state: mach.xuanyuan.wifi-caldone-20261111.

## BREAKTHROUGH: the firmware was waiting for qdss_trace_config (2026-11-11, 02:12)

### Getting the phone's own firmware without root
The loader has "oem read NAME OFFSET LENGTH" (a GPT partition into the staged buffer) plus
"fastboot get_staged"; 2 MB chunks reassemble a whole partition. The peach firmware lives in the
FAT16 partition "modem_b" under image/peach/ (95 files, 28 MB). Dump: docs/.../tools/dump-partition.ps1,
list: tools/fat-ls.py (or 7z). All files are byte-identical to the Xiaomi 15 vendor copy we already
had -- the only files we lacked were qdss_trace_config_v1.cfg and qdss_trace_config_v2.cfg.

### The missing piece
Put qdss_trace_config_v2.cfg into the root's /redwood/fw (build.sh installs it when the file is in
../fw-wifi-dada/image-peach) and the QMI handshake finishes:

    qmi qdss: trace configuration sent, 6241 bytes in 2 pieces
    nw qmi rx: type 4 msg 0038 txn 3 len 18     <- FW_READY, finally
    nw qmi event: 4

So the firmware had been withholding init-done because the host never sent the QDSS trace
configuration; nothing else was wrong. With FW_READY the driver runs ath12k_core_start_firmware,
CE pipes, DP init, HTC services (Control/HTT/WMI), WMI service ready, hw modes, hardware
registered with 101 channels, vdev create, en0 made, station attached (stage 5).

### After FW_READY
- Cold-boot calibration path runs (cal_done=0, no cal data on this port) and power-cycles MHI;
  boot with dadawifi_nocbc=1 to skip it (the kernel's internal ab->qmi.cal_done stays 0).
- A PAC fault appeared once MAC registration completed: hakios_dada_wlan.c handed four kernel
  callbacks to the ath12k object in a read-only static const struct; storing stripped pointers into
  it faulted (pc hakios_dada_raw_fn+4). Made the struct writable and filled the pointers with
  hakios_dada_raw_fn() (ptrauth_strip + memcpy), like the platform/board structures.

### Where it stops now
The first WMI command after vdev create makes the firmware stop returning WMI credits and then
assert (RDDM): "WMI vdev id 0x0 set param 48 value 2" = WMI_VDEV_PARAM_TX_ENCAP_TYPE, value 2 =
ATH12K_HW_TXRX_ETHERNET, followed by RX_DECAP_TYPE. Credit experiments: raising the offered HTC
credits from 2 to 32 makes the firmware assert at once; lowering to 1 (Android's gWmiCreditCount=1)
gives the same stall. A kernel with the two encap/decap WMI writes skipped is built and a payload is
packaged; that test is the next thing to run.

Kernels: mach.xuanyuan.wifi-fwready-20261111 (sha256 f08deacb...).

## THE RADIO SCANS: 14 networks (2026-11-11, 02:35)

With qdss_trace_config in the root, dadawifi_nocbc=1 and -- the last piece -- dadawifi_nonss=1
(skip the post-create NSS set_param; Android's cld3 does not send it either), the WMI sequence runs
all the way through the first scan:

    wifi: scan starts on 41 channels
    wifi: scan 1 finished, 14 networks
    ath12k: stage 6, scans 1, networks seen 14, error 0

The earlier stall was exactly the NSS set_param: WMI_VDEV_SET_PARAM (0x5008) with param 34 value 2
right after vdev create. The firmware consumes that command and stops returning WMI credits, then
asserts (RDDM). Skipping encap/decap alone did not help (the next set_param stalled the same way);
skipping the NSS write is what lets vdev start and the scan go through. HTC offers only 2 WMI TX
credits; forcing more makes the firmware assert immediately, and 1 (Android's gWmiCreditCount=1)
behaves the same as 2.

Commands that produce this state:

    fastboot stage ios26-wifi-s4-nonss.bin && fastboot oem run
    # boot args: adsp=1 dadaadsp=1 dadanoflash=1 dadawifi_nocbc=1 dadawifi_credits=1 dadawifi_nonss=1

Next: trigger a join (nw_wifi_connect now reached through the Apple80211 path) so the four-way
handshake, DHCP and en0 traffic can be checked; the station code already handles WPA2-Personal and
hands the link state to redwood_net.

Kernel: mach.xuanyuan.wifi-noencap-20261111 (sha256 f08deacb...; same image also holds the
skip-encap change, harmless with dadawifi_nonss set).

## The real WMI serial killer: the STA power-save block (2026-11-11, 03:35)

Skipping the post-create NSS set_param made one boot scan 14 networks, but most boots still died
right after vdev create. The good and bad boots have the same vdev create, then
WMI_STA_POWERSAVE_PARAM (set sta ps, param 0 value 0); in the good boot the credit came back, in
the bad ones it never did and the firmware asserted (RDDM). That command comes from
ath12k_mac_op_add_interface's WMI_VDEV_TYPE_STA branch, which sends three
WMI_STA_POWERSAVE_PARAM commands and a WMI_PDEV_SET_PS_MODE right after create. Skipping that
whole block (new hook hakios_dada_wifi_skip_sta_ps, boot-arg dadawifi_staps=1 sends it again)
takes the first of them out of the sequence.

Also added on the kernel side (wlan.c):
- the scan list is printed: one line per BSS (SSID, BSSID, MHz, dBm, open or secure);
- an automatic join after the first scan with results: dadawifi_ssid=NAME + dadawifi_psk=PASS,
  or dadawifi_joinopen=1 for the first open network; the join result is logged;
- a failed scan (empty BSS list) is retried up to six times from the scan-done event.

Test credentials for the home network are kept out of this repository; they are passed as boot
arguments on the fastboot command line only.
