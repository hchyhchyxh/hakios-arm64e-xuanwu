# hakios-arm64e-xuanwu: iOS 26 on the Xiaomi 15 Ultra

English | [简体中文](README.zh-CN.md)

Boots Apple's open-source XNU kernel (xnu-12377.121.6, the kernel of iOS 26) on the
Xiaomi 15 Ultra (codename **xuanwu (玄武)**, Snapdragon 8 Elite, SM8750) and runs the iOS 26
arm64e user space on top of it.

Built on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) and on the Xiaomi 15
(`dada`, same SoC) port this tree started from. The earlier debugging notes in this
repository spell the codename `xuanyuan`; it is the same device as `xuanwu`.

> Experimental. Requires an unlocked bootloader and can cost you your data or even brick the
> phone. Only try it if you are comfortable tinkering and can accept the risk.

Everything here is **RAM-only**: the phone is booted with `fastboot boot` + staged payloads,
no partition is written, and a reboot returns to Android.

## Status (2026-11-09)

| Part | Status |
| --- | --- |
| CPU | arm64e on the Oryon cores; 8 cores, hardware PAC, iOS 26 user space boots |
| Boot | Our own loader; the iOS image lives in RAM, no partition writes |
| System | iOS 26 boots; SpringBoard draws the lock screen, and through the scripted touch the home screen |
| GPU | **Adreno 830 powered up**: `dadagpu=6 dadagpu_keep=1` — GMU firmware running, HFI queues up, the 3D core initialised, the `RedwoodGPU` service is up |
| Metal | Device discovery works (backboardd creates the Metal device, `MetalPluginName=RedwoodMetal`, reported as `Adreno830`); the shader compiler library `libredwoodair.dylib` can be built (Mesa 26.2.5 + the K90 `rebase-26` patch set). Open: the kernel side has no msm-style GPU device for the Metal library yet |
| Lock screen clock | Shown by an injected plain label over the cover sheet, refreshed every second (clockplain=1). The real iOS 26 clock is a CASDFLayer glass text that only the Metal renderer draws; the label is the interim until Metal works |
| Touch | The chip (Synaptics S3910, TCM V4) is identified and its 0xC0 raw frames are captured; real-finger touches are not decoded yet. Scripted touches work (`taps=1` + `/redwood/taps.txt`) and can unlock into the home screen |
| Wi-Fi | Firmware (peach family, 93 files / 28.6 MB) can be put into the image; the peach kernel driver is not ported (the kernel has no PCIe stack) |
| Audio, camera, modem | Not started |

The two open tasks, with the captured data and the exact next steps, are in
[`docs/SETUP-fresh-clone.md`](docs/SETUP-fresh-clone.md) section 6.

## Continue on another machine

The repository is what you need to rebuild everything. Read
[`docs/SETUP-fresh-clone.md`](docs/SETUP-fresh-clone.md): it lists what to install, the build
commands for the kernel, the GL library, the AIR compiler library and the image, how to run a
payload, and the current tasks.

```bash
git clone git@github.com:hchyhchyxh/hakios-arm64e-xuanwu.git
cd hakios-arm64e-xuanwu
bash docs/xuanyuan/tools/make-isolated-tree.sh
bash docs/xuanyuan/tools/rebuild-verified-kernel.sh
```

## Layout

| Directory | Contents |
| --- | --- |
| `xnu-src/xnu-12377.121.6/` | The kernel source this port runs: HakiOS/dada lineage plus the xuanwu bring-up (display, touch, GPU power, `RedwoodGPU`, `IOAcceleratorES`) |
| `userland/` | HakiOS user space: iOS 26 injection library (`inject26.c`), the Metal plug-in (`redwoodmetal.m`), `eagl`/OSMesa GL renderer, the image build scripts |
| `userland/tools/mesa/` | The new-generation AIR frontend and Mesa patch set (from the K90 Pro Max port); `mk-mesa.sh` builds `libredwoodair.dylib` |
| `docs/SETUP-fresh-clone.md` | Everything needed to continue on a new machine |
| `docs/xuanyuan/` | The debugging record (58 notes): GPU bring-up steps, GMU/HFI, Metal state, touch protocol, PCIe notes, clock investigation |
| `tools/xuanyuan-helpers/` | Helper scripts from the debugging sessions |
| `kernel/`, `base/`, `gpu2/`, `uefi-dada/` | The HakiOS/dada material this port is built from |

## Not included

No binaries or firmware from Apple, Qualcomm, Xiaomi or anyone else: the iOS system image,
the firmware blobs, the SDK and the prebuilt host toolchain are kept out of the repository
(see `.gitignore` and the setup guide).

## Acknowledgements

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) — the foundation
- [hakios-arm64e-dada](https://github.com/superboyyy/hakios-arm64e-dada) — the Xiaomi 15 (same SoC) port this tree started from
- [hackios-redmi-k90-annibale](https://github.com/iola1999/hackios-redmi-k90-annibale) — the K90 Pro Max workspace whose AIR/Mesa recipes (Mesa 26.2.5 + `rebase-26`) this port reuses
- [UniXNU](https://github.com/UniXNU/UniXNU), [metal2vulkan](https://github.com/steelbrain/metal2vulkan), Mesa (Turnip/freedreno) — GPU references
- Linux's msm, ath12k and qcom touch drivers — register layouts and bring-up order

Apple, iOS and Metal are trademarks of Apple Inc. This project is not affiliated with Apple,
Qualcomm or Xiaomi.

