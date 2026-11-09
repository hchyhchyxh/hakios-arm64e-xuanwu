# hakios-arm64e-dada: iOS 26 on the Xiaomi 15

English | [简体中文](README.zh-CN.md)

Boots Apple's open-source XNU kernel (xnu-12377.121.6, the kernel of iOS 26) on the Xiaomi 15 (codename dada,
Snapdragon 8 Elite, SM8750) and runs the iOS 26 arm64e user space on top of it.

Built on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel), with
arm64e, iOS 26 and dada support added.

> Experimental. Requires an unlocked bootloader and can cost you your data or even brick the phone.
> Only try it if you are comfortable tinkering and can accept the risk.

## Status

| Part | Status |
| --- | --- |
| CPU | arm64e runs natively on the Oryon cores: all 8 cores up (prime cores included), hardware PAC, clocked to 3.52 GHz over SCMI, no instruction translation |
| Boot | Our own EFI loader, started from Mu-Silicium UEFI; the iOS image lives in an Android DSU partition |
| System | iOS 26 reaches the home screen; touch works; flash storage read and written on demand |
| GPU | Adreno 830 powered up. Metal goes through metal2vulkan (Metal → Vulkan), then Turnip, then our kernel driver, and runs on the Adreno |
| Liquid Glass | Core shaders translate correctly (compared pixel by pixel against Apple's GPU on a Mac); some issues still need debugging |
| Audio | Speaker playback path (ADSP, AudioReach, CS35L43 amplifiers) works; system sounds and microphones are in progress |
| Battery | Fuel gauge reported to iOS |
| Wi-Fi / Bluetooth | Bluetooth works. Wi-Fi gets as far as WMI, but the firmware dies in most runs. Note: the Wi-Fi driver in the current kernel source is a temporary stand-in |
| HDR, camera, modem | Not started |

## Layout

| Directory | Contents | Licence |
| --- | --- | --- |
| `kernel/` | `xnu-12377.121.6-dada.patch`: every kernel change against Apple's xnu-12377.121.6; `apply.sh` makes the kernel tree | APSL 2.0 |
| `base/` | HakiOS kernel repository with our changes: EFI loader (`xnuloader/`), build and flashing tools (`tools/dada-*.sh`), device configs, docs | HakiOS's (APSL 2.0) |
| `userland/` | HakiOS user space with our changes: iOS 26 bring-up, injection, Metal plug-in, audio HAL plug-in | HakiOS's (APSL 2.0) |
| `uefi-dada/` | dada's UEFI: our patch to 7Night-kqx/uefi-dada (starts our XNU loader), `apply.sh`, build environment Dockerfile | BSD-2-Clause-Patent |
| `gpu2/` | GPU path: patches to metal2vulkan, UniXNU NativeMetal and Mesa Turnip, plus build scripts | Each patch keeps the licence of the project it changes (see NOTICE.md) |
| `tools/` | `export-from-work.sh` re-exports this repository from the working trees; `check-publishable.sh` looks for anything that must not be published before a commit | MIT |

## Not included

This repository contains no binaries or firmware from Apple, Qualcomm, Xiaomi or anyone else.

## Acknowledgements

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel): the foundation of this project
- [UniXNU](https://github.com/UniXNU/UniXNU): NativeAdreno / NativeMetal, Ath12kWifi, Snapdragon platform reference
- [Data A Live](https://github.com/ShueiTsuki/data-a-live): reference for GPU, audio and CPU clocks of iOS on Snapdragon
- [metal2vulkan](https://github.com/steelbrain/metal2vulkan): the Metal-to-Vulkan translation layer
- [Mesa](https://mesa3d.org/) (Turnip / freedreno)
- [Project Silicium / Mu-Silicium](https://github.com/Project-Silicium/Mu-Silicium)
- [uefi-dada](https://github.com/7Night-kqx/uefi-dada): the Mu-Silicium port to the Xiaomi 15
- Linux's msm, ath12k and qcom drivers: reference for register layouts and bring-up order

The drivers are written from Linux's public descriptions. See NOTICE.md for details.

Apple, iOS and Metal are trademarks of Apple Inc. This project is not affiliated with Apple, Qualcomm or Xiaomi.

---

## 继续开发（新电脑从这里开始）

详细步骤见 [docs/SETUP-fresh-clone.md](docs/SETUP-fresh-clone.md)。

当前状态（2026-11-09）：

- Adreno 830 已完整上电：dadagpu=6 dadagpu_keep=1（GMU 固件、HFI、3D 核、RedwoodGPU）。
- Metal：设备层已通（backboardd 创建 Metal 设备、识别 Adreno830），着色器编译器库可编出（Mesa 26.2.5 + K90 rebase-26），差内核侧 GPU 设备接口。
- 触摸：脚本注入可用（	aps=1 可解锁进主屏）；真实手指未通，0xC0 原始帧已能抓取（	ouch_frame_dump=1），待解坐标。
- 锁屏时间：需要 Metal（CASDFLayer）。
- 纯 RAM 运行（astboot boot + stage），不写分区；iOS 镜像/固件/SDK 不入库。

仓库不含：ios26/ 系统镜像素材（39GB）、ase/xnu-deps 主机工具链、~/theos SDK、Mesa 源码包 —— 换机时从旧机器拷或按 SETUP 文档准备。
