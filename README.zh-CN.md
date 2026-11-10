# hakios-arm64e-xuanwu：在小米 15 Ultra 上跑 iOS 26

[English](README.md) | 简体中文

在小米 15 Ultra（代号 **xuanwu（玄武）**，骁龙 8 Elite / SM8750）上引导 Apple 开源的 XNU 内核
（xnu-12377.121.6，即 iOS 26 的内核），并在其上运行 iOS 26 的 arm64e 用户态。

本项目建立在 [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) 和本树来源的小米 15
（`dada`，同 SoC）移植之上。仓库早期调试记录里曾把代号写作 `xuanyuan`，指的是同一台设备。

> 实验性项目。需要解锁 Bootloader，可能丢数据甚至变砖。只适合愿意折腾、能承担风险的人。

全程 **RAM-only**：用 `fastboot boot` + staged payload 启动，不写任何分区，重启即回到安卓。

## 当前状态（2026-11-09）

| 模块 | 状态 |
| --- | --- |
| CPU | arm64e 跑在 Oryon 核上，8 核、硬件 PAC，iOS 26 用户态能启动 |
| 引导 | 我们的 loader；iOS 镜像在 RAM 里，不写分区 |
| 系统 | iOS 26 能启动；SpringBoard 能画锁屏，通过脚本注入触摸可以进主屏 |
| GPU | **Adreno 830 已完整上电**：`dadagpu=6 dadagpu_keep=1` —— GMU 固件运行、HFI 队列建立、3D 核初始化、`RedwoodGPU` 服务就绪 |
| Metal | 设备发现已通（backboardd 创建 Metal 设备，`MetalPluginName=RedwoodMetal`，识别为 `Adreno830`）；着色器编译器库 `libredwoodair.dylib` 可以编出（Mesa 26.2.5 + K90 的 `rebase-26` 补丁）。未完成：内核侧还没有 Metal 库需要的 msm 风格 GPU 设备 |
| 锁屏时间 | 已显示：注入层在锁屏上盖了一个每秒刷新的普通时间标签（clockplain=1）。iOS 26 原生时钟是 CASDFLayer 玻璃文字、只有 Metal 能画，这个标签是 Metal 打通前的过渡方案 |
| 触摸 | 芯片（Synaptics S3910，TCM V4）已识别，0xC0 原始帧已能抓取；真实手指坐标尚未解码。脚本触摸可用（`taps=1` + `/redwood/taps.txt`），能解锁进主屏 |
| Wi-Fi | 固件（peach 家族，93 文件 / 28.6MB）可以放进镜像；内核 peach 驱动未移植（内核缺 PCIe 栈） |
| 音频、相机、基带 | 未开始 |

两个待办任务的数据与下一步写在 [`docs/SETUP-fresh-clone.md`](docs/SETUP-fresh-clone.md) 第 6 节。

## 换电脑继续开发

仓库里包含重新编出全部内容所需的东西。先读
[`docs/SETUP-fresh-clone.md`](docs/SETUP-fresh-clone.md)：要装什么、内核/GL 库/AIR 编译器库/镜像的构建命令、
怎么上机跑 payload，以及当前任务。

```bash
git clone git@github.com:hchyhchyxh/hakios-arm64e-xuanwu.git
cd hakios-arm64e-xuanwu
bash docs/xuanyuan/tools/make-isolated-tree.sh
bash docs/xuanyuan/tools/rebuild-verified-kernel.sh
```

## 目录

| 目录 | 内容 |
| --- | --- |
| `xnu-src/xnu-12377.121.6/` | 本移植运行的内核源码：HakiOS/dada 血统 + xuanwu 的上电代码（显示、触摸、GPU 电源、`RedwoodGPU`、`IOAcceleratorES`） |
| `userland/` | HakiOS 用户态：iOS 26 注入库（`inject26.c`）、Metal 插件（`redwoodmetal.m`）、`eagl`/OSMesa GL 渲染器、镜像构建脚本 |
| `userland/tools/mesa/` | 新代 AIR 前端与 Mesa 补丁集（来自 K90 Pro Max 移植）；`mk-mesa.sh` 构建 `libredwoodair.dylib` |
| `docs/SETUP-fresh-clone.md` | 换机继续开发的完整说明 |
| `docs/xuanyuan/` | 调试记录（58 篇）：GPU 上电步骤、GMU/HFI、Metal 状态、触摸协议、PCIe 笔记、时钟排查 |
| `tools/xuanyuan-helpers/` | 调试期间用到的辅助脚本 |
| `kernel/`、`base/`、`gpu2/`、`uefi-dada/` | 本移植所基于的 HakiOS/dada 材料 |

## 不包含的内容

不含任何厂商二进制或固件：iOS 系统镜像、固件、SDK、预编译主机工具链都不在仓库里
（见 `.gitignore` 和 setup 文档）。

## 致谢

- [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel) —— 项目基础
- [hakios-arm64e-dada](https://github.com/superboyyy/hakios-arm64e-dada) —— 小米 15（同 SoC）移植，本树由它而来
- [hackios-redmi-k90-annibale](https://github.com/iola1999/hackios-redmi-k90-annibale) —— K90 Pro Max 工作区，本移植复用了它的 AIR/Mesa 配方（Mesa 26.2.5 + `rebase-26`）
- [UniXNU](https://github.com/UniXNU/UniXNU)、[metal2vulkan](https://github.com/steelbrain/metal2vulkan)、Mesa（Turnip/freedreno）—— GPU 参考
- Linux 的 msm、ath12k、qcom 触摸驱动 —— 寄存器布局与上电顺序参考

Apple、iOS、Metal 是 Apple Inc. 的商标。本项目与 Apple、Qualcomm、Xiaomi 无关。

