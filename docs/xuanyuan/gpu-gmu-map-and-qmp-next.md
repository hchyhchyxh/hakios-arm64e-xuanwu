# GPU stage two: where the GMU bring-up stands (2026-11-09 20:25)

## Facts gathered

1. **RPMh rail vote works** (commit `64faf82`): RSC 0x16520000, id 0x30800, v3 TCS layout; a three
   command batch votes `gfx.lvl` 0x30050 / `cx.lvl` 0x30000 / `mx.lvl` 0x30010 at their highest
   listed corners and is ACKed.
2. **The GPU block is still not clocked**: the first read of the GMU window at offset 0 (0x3d6c000)
   hangs the bus; the a660 offsets inside the window read zero.  `gcc_qmip_gpu_ahb_clk` is
   QMP-managed on sm8750, so a QMP vote is missing.
3. **The a660 GMU register offsets are not gen8's**: upstream `a6xx_gmu.xml` puts
   `GMU_CM3_SYSRESET` at 0x1f800 words (= 0x7e000 bytes) while the driver uses 0x5000 words
   (= 0x14000); the sm8750 GMU window is only 0x32000 long, so gen8 relocates the registers again.
   The public kernel tree does not carry an a8xx register XML yet (`a8xx.xml` 404 on master, drm-next
   and linux-next snapshots checked; the a6xx XML marks the status registers `A8XX-`).

## The two data sources still missing

- **gen8 GMU register map** (offsets of CM3_SYSRESET, BOOT_CONFIG, FW_INIT_RESULT, HFI_*, AON
  counter): the vendor kernel source for SM8750 (Xiaomi/Qualcomm GPL drop or LineageOS
  `android_kernel_xiaomi_sm8750`) has `drivers/gpu/msm/adreno/a8xx_gmu.h`/`a8xx_reg.h` - that is
  the authoritative source, and it is on GitHub.
- **QMP clock vote for the GPU AHB**: this tree already has a full QMP client in
  `bsd/dev/redwood_wifi.c` (AOSS mailbox 0x0c300000, "LIAM" handshake, IPCC doorbell, `mailbox_send`),
  which can be reused; the GPU driver just needs the QMP clock-vote message for
  `gcc_qmip_gpu_ahb_clk`.

## Next steps (autonomous)

1. Fetch the SM8750 vendor kernel source (Xiaomi GPL / LineageOS) and extract the a8xx GMU register
   map; put the offsets into `redwood_adreno.c` behind the `adreno_soc=sm8750` (or gen8) switch.
2. Reuse the QMP client from `redwood_wifi.c` to vote the GPU AHB clock; then repeat the GDSC
   retrigger + clock enables and the GMU window sweep - the crystal counter (19.2 MHz) is the check.
3. Continue steps 7-15 (RPMh sequencer, SMMU, GMU memory, TrustZone zap, GMU firmware with the
   gen80000 images already in `/redwood/fw`, HFI start).  The a660 zap/firmware loader in the driver
   will need the gen80000 file names and format checks from the vendor source too.

Everything so far is RAM-only; the phone is recoverable from the loader at any time.
