# sm8750/xuanyuan hardware constants vs the redwood (a660) driver

Source: upstream Linux `arch/arm64/boot/dts/qcom/sm8750.dtsi` (fetched 2026-11-09 to
`X:/ios/work/sm8750-refs/sm8750.dtsi`, the phone is `qcom,adreno-44050001` = Adreno 830,
`qcom,adreno-gmu-830.1`).  The driver `bsd/dev/redwood_adreno.c` in this tree is the POCO X5 Pro
(sm7325, Adreno 642L) line; its constants are the "redwood" column.

| Block | redwood (driver default) | sm8750 (xuanyuan) | dtsi node |
|---|---|---|---|
| RPMh command DB (AOP) | `0x80860000` + `0x20000` | **`0x81c60000`** + `0x20000` | `aop-cmd-db@81c60000` |
| GPU (kgsl_3d0) | `0x03d00000` + `0x40000` | `0x03d00000` + **`0x6d000`**, cx_mem `0x03d9e000` + `0x2000` | `gpu@3d00000` |
| GPU GMU | `0x03d6a000` + `0x34000` | **`0x03d6c000`** + `0x32000` | `gmu@3d6c000` |
| GPUCC | `0x03d90000` + `0x9000` | `0x03d90000` + `0x9800` | `clock-controller@3d90000` |
| GX clock control (new on gen8) | - | `0x03d64000` + `0x6000` | `gxclkctl: clock-controller@3d64000` |
| Adreno SMMU | `0x03da0000` + `0x20000` | `0x03da0000` + **`0x40000`** | `iommu@3da0000` |
| GMU PDC | `0x0b290000` | **`0x0b220000`** (`interrupt-controller@b220000`) | `pdc` |
| QFPROM (fuses) | `0x00780000` | **`0x221c8000`** | `efuse@221c8000` |
| tsens1 | `0x0c265000` | **`0x0c229000`** | `thermal-sensor@c229000` |
| GCC | `0x00100000` + `0x80000` | `0x00100000` + `0x1f4200` | `clock-controller@100000` |
| dispcc | `0x0af00000` | `0x0af00000` | `clock-controller@af00000` |
| zap shader memory | - | `0x9b09a000` + `0x2000` (`gpu_micro_code_mem`) | `gpu-micro-code@9b09a000` |
| GPU / GMU interrupts | - | GPU SPI 300; GMU hfi SPI 304, gmu SPI 305 | `interrupts` |
| GPU IRQs through SMMU | - | 26 SPI interrupts, from 674... | `iommu@3da0000` |

The driver now accepts boot-arg overrides for the blocks that differ, so the whole set can be probed
without rebuilding between iterations:

```
adreno=10 adreno_pause=0 adreno_watch=0 cmd_db=0x81c60000 gmu=0x3d6c000 gmu_size=0x32000
smmu_size=0x40000 pdc=0xb220000 qfprom=0x221c8000 tsens1=0xc229000
```

## Firmware names (Linux msm a8xx_catalog.c, fetched under metal-refs/a8xx/)

```
[ADRENO_FW_SQE] = "gen80000_sqe.fw",
[ADRENO_FW_GMU] = "gen80000_gmu.bin",
[ADRENO_FW_AQE] = "gen80000_aqe.fw",
```

which is what `userland/fw-gpu/` already holds (zap image is `gen80000_zap.mbn` from `/odm/firmware`,
the dtsi's `gpu_micro_code_mem` region is where it is loaded to).

## Known remaining divergences after the addresses

- GPUCC register offsets: the driver's `GPUCC_*` offsets are a660's; `drivers/clk/qcom/gpucc-sm8750.c`
  and `gpucc-sm8250.c` are the reference for gen8 (the `gxclkctl` block is new: gen8 splits the GX
  power domain out of GPUCC).
- GMU firmware loading: Linux's `a6xx_gmu.c` carries the A8XX branches (`a8xx_gmu_gx_is_on`,
  `REG_A8XX_*`, HFI versions); `a8xx_gpu.c` has the SQE/AQE and zap init.
- The zap signature path: driver step 12/19 ask TrustZone; the reserved-memory address above is what
  the kernel must point TrustZone at.
