# PCIe on xuanyuan: the hardware the Wi-Fi path needs (2026-11-09 21:00)

The peach Wi-Fi chip is a PCIe device, and this kernel tree has **no PCIe host controller driver at
all** (no pci/pcie sources in either bsd/dev tree, no conf entries).  Wi-Fi therefore needs a PCIe
root complex brought up first.  The hardware, from the upstream sm8750 dtsi (local copy
`X:/ios/work/sm8750-refs/sm8750.dtsi`, pcie0 at line 5723):

| What | Value |
|---|---|
| compatible | `qcom,pcie-sm8750`, `qcom,pcie-sm8550` |
| PARF | 0x01c00000 + 0x3000 |
| DBI | 0x40000000 + 0xf1d |
| ELBI | 0x40000f20 + 0xa8 |
| ATU | 0x40001000 + 0x1000 |
| config space | 0x40100000 + 0x100000 |
| MHI | 0x01c03000 + 0x1000 |
| IRQs | SPI 141..148 (msi0..7), SPI 140 global, SPI 149/150 legacy |
| lanes | 2 |
| PHY | `qcom,sm8750-qmp-gen3x2-pcie-phy` |
| windows | I/O 0x0..0x100000 at 0x40200000; mem 0x40300000 + 0x23d00000; 64-bit |
| MSI | GIC ITS msi-map (linux,pci-domain 0; msi-map-mask 0xff00) |

Linux reference fetched to `X:/ios/work/metal-refs/pcie/` (`pcie-qcom.c`, `dw-host.c`,
`drivers/pci/controller/dwc/` + `pcie-designware*`).

## Bring-up order for the port

1. **PHY + clocks + resets**: qmp gen3x2 PHY init (serdes PLL lock, register block from the PHY node),
   GCC PCIE clocks (`pcie_*` in gcc-sm8750), resets (parf/pcie), power rails (RPMh `mx`/`cx`/`gfx`).
2. **RC init**: PARF config (PERST, link training controls), DWC core setup via DBI
   (link width/speed, ATU windows for config/mem), wait for link up (PARF link-status).
3. **Config space access**: ECAM/ATU outbound for bus 0 device enumeration; then scan for peach
   (vendor 0x17cb, device id read from the chip's config space).
4. **MSI/MSI-X**: GIC ITS map (msi-map above) or INTx fallback (SPI 149/150).
5. **Hand the port to ath12k**: `hakios_dada_wifi.c` (currently `optional hakios_ath12k` in
   `bsd/conf/files`) then links and runs `dadawifi=1/2/3`; firmware is already in the image
   (`fw-wifi-dada/image-peach`, 93 files, verified `/redwood/fw/bdwlan.*` on device).

## What can be checked without Wi-Fi

- Read PARF/DWC/PHY registers and print them (as the GPU work does) with a `pcie=1` boot arg.
- Link-up status after PHY+RC init is the first binary check; enumerate peach's vendor/device id the
  second.

Firmware and image are ready; the missing piece is this controller driver.
