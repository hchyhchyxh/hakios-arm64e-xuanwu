> 设备代号：**xuanwu（玄武）**。仓库早期调试记录/文档里曾写作 xuanyuan，指的是同一台小米 15 Ultra。

# 新电脑继续开发指南（xuanwu / 小米 15 Ultra 上的 iOS 26）

这份文档的目标：**在一台新电脑上 git clone 本仓库后，能重新编出内核、用户态镜像和 iOS 26 payload，继续修剩下的两个问题**（真实手指触摸、Metal 时钟）。

---

## 0. 目前做到哪一步（2026-11-09 状态）

已在真机（Xiaomi 15 Ultra, xuanwu, SM8750, BL 解锁）上验证：

| 模块 | 状态 |
| --- | --- |
| 内核引导 iOS 26 用户态 | 可用（RAM-only `fastboot boot`，不写任何分区） |
| 显示 / 锁屏 / 主屏 | 可用（OpenGL ES 渲染器，软件 GL；能通过脚本注入手势解锁进主屏） |
| GPU（Adreno 830） | **已完整上电**：`dadagpu=6 dadagpu_keep=1` 时 GMU 固件运行、HFI 队列建立、3D 核初始化、`RedwoodGPU` 服务就绪 |
| Metal | 设备层已通：backboardd 成功创建 Metal 设备（`MetalPluginName=RedwoodMetal`，识别为 Adreno830）；**着色器编译器库已能编出**（`libredwoodair.dylib`，Mesa 26.2.5 + K90 rebase-26）。卡在：内核侧没有 Metal 库期望的 msm 风格 GPU 设备 |
| 锁屏时间 | 已显示 —— 注入层在锁屏上盖了一个每秒刷新的普通时间标签（`clockplain=1`）；iOS 26 原生时钟是 CASDFLayer 玻璃文字，仍需 Metal 才能原生渲染 |
| 触摸（真实手指） | 已通 —— Synaptics S3910（TCM V4）的原生触摸报告正常，注入的 HID 服务把 DOWN/MOVE/UP 送进系统，手指滑动可解锁；脚本触摸（`taps=1`）也可用 |
| Wi-Fi | 固件（peach 家族 93 文件 / 28.6MB）已能进镜像；内核 peach 驱动未移植（内核缺 PCIe 栈） |

**两个待办的具体线索都在本仓库里：**
- 触摸：`xnu-src/.../bsd/dev/hakios_dada_touch.c` 的 `xiaomi_frame()`（0xC0 帧，1664 字节）目前只打日志；需要按定点触摸数据解出坐标，再 `redwood_touch_post()`。抓帧开关：boot-arg `touch_frame_dump=1`（本仓库已改成本文所记的连续记录差异版本）。
- Metal：`libredwoodair.dylib` 已编出（见第 4 节），下一步是在内核里提供 K90 那种 GPU 设备接口（参考本地文档 `docs/xuanwu/metal-air-final-state.md`、`docs/xuanwu/pcie-hardware-and-plan.md`）。

---

## 1. 环境要求

- **Windows 11 + WSL2 Ubuntu 24.04（root）**，磁盘 ≥ 120 GB（iOS 系统镜像 ~5.2 GB，`ios26/` 素材 ~39 GB 本地缓存，不提交到 git）
- **目标机**：Xiaomi 15 Ultra（xuanwu），已解锁 BL，可进 fastboot
- **iOS 26 系统镜像**：本仓库不含（体积/版权原因）。需要本地准备好 `ios26/` 素材与 resident 镜像（本项目里放在 `X:/air6/xiaomi15ultra-hakios/resident/`，参见 `docs/xuanwu/` 中 image/run 相关文档）
- **THEOS + iOS 16.5 SDK**：`~/theos/sdks/iPhoneOS16.5.sdk`（用户态交叉编译用）
- **xnu-deps 预编译主机工具**（`base/xnu-deps/host/bin`、`base/xnu-deps/sdk/iPhoneOS16.5.xnu.sdk`）：二进制依赖，**不入 git**。换机时从旧机器拷 `base/xnu-deps/` 目录即可（也是本项目原本的工作方式）
- Mesa 源码包（不入 git，按需下载）：
  - `mesa-25.0.7.tar.xz`（SHO `592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6`）—— GL/OSMesa 旧线
  - `mesa-26.2.5.tar.xz`（SHA `794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5`）—— 新 AIR 编译库（`libredwoodair.dylib`）用
- Python 虚拟环境依赖：`meson==1.12.1 mako PyYAML packaging`（mesa 构建要）
- 系统包：`git curl python3 build-essential bison flex cmake ninja-build patch llvm`（llvm 提供 `llvm-strip/llvm-nm/llvm-objdump`）

```bash
# 一次性准备
sudo apt-get install -y git curl build-essential bison flex cmake ninja-build patch llvm
python3 -m venv ~/venv-mesa
~/venv-mesa/bin/pip install meson==1.12.1 mako PyYAML packaging
```

---

## 2. 克隆

```bash
git clone <你的私有仓库地址> hakios-arm64e-xuanwu
cd hakios-arm64e-xuanwu
# 上游来源说明：本项目基于 oosexgang/hakios-xnu-kernel + superboyyy/hakios-arm64e-dada，
# 再加小米 15 Ultra（xuanwu）的移植与调试。
```

---

## 3. 构建内核

仓库只跟踪一份内核源码（`xnu-src/xnu-12377.121.6`，约 122 MB）；编译脚本用的是孤立副本，先跑一次脚本生成：

```bash
cd hakios-arm64e-xuanwu
bash docs/xuanwu/tools/make-isolated-tree.sh   # 生成 xnu-src/isolated/xnu-xnu-12377.121.6
```

然后：

# 需要 base/xnu-deps 主机工具链已经在位（见第 1 节）
bash docs/xuanwu/tools/rebuild-verified-kernel.sh
# 产物：X:/air6/xiaomi15ultra-hakios/kernel-build/mach.xuanwu.copyio-fixed
#      （脚本里是 X 盘路径；换机时改脚本顶部的输出路径，或把它指到你的工作目录）
```

编译要点（脚本已处理，了解即可）：源码在 `xnu-src/isolated/xnu-xnu-12377.121.6`，两棵源码树都要改（isolated 是实际参与编译的那棵）。

---

## 4. 构建 Mesa：GL 库 与 AIR 编译器库

### 4.1 AIR 编译器库 `libredwoodair.dylib`（Metal 用，已成功）

配方来自 K90 Pro Max 移植（`iola1999/hackios-redmi-k90-annibale` 的 `tools/k90auto/build-air.py` + `tools/mk-mesa.sh`）。本仓库已把所需的 `tools/mesa/`（51 个新 AIR 前端文件）和 `rebase-26` 补丁集换入。

```bash
cd userland
# Mesa 26.2.5 解包 + 打补丁（PREPARE=1 只准备源码树）
VERSION=26.2.5 SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5 \
REBASE=26 PREPARE=1 bash tools/mk-mesa.sh

# arm64e 交叉构建（iOS 26 进程是 arm64e）：
# 交叉文件模板见 docs/xuanwu/mesa-cross-arm64e.ini（复制到
# userland/xnu-deps/build/mesa/cross-arm64e.ini 并改 SDK 路径）
meson setup build26 userland/xnu-deps/build/mesa/mesa-26.2.5 \
  --cross-file cross-arm64e.ini --prefix=/redwood --libdir=lib --buildtype=release \
  -Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=disabled \
  -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
  -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled \
  -Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= \
  -Dbuild-tests=false \
  '-Dcpp_args=-Dpthread_condattr_setclock(a,b)=0' -Db_lundef=false
ninja -C build26 src/gallium/drivers/freedreno/libannibaleair.dylib
# 产物改名：libredwoodair.dylib（说明见 docs/xuanwu/metal-air-final-state.md）
```

已知注意点：
- 26.2.5 的 OSMesa 在 arm64e 下有 `rwa_*` 未定义符号问题（本仓库的 `inject26.c` 在 `gl=1` 时会先 `dlopen("/redwood/lib/libredwoodair.dylib", RTLD_GLOBAL)` 预加载来绕开）。如果重做 GL 路径，建议继续用 25.0.7 的 `libOSMesa`。
- `tools/mesa/rebase-26/*.patch` 里有 9 个补丁对 26.2.5 打不上（性能探针类），不影响 AIR 库生成。

### 4.2 GL/OSMesa 旧线（可选，工程里已有产物就用产物）

`userland/tools/mk-mesa.sh`（默认 `VERSION=25.0.7`）会在 `userland/xnu-deps/build/mesa/out/` 生成 `libOSMesa.8.dylib`、`gl.xml`、`exported-gl.txt`。`userland/userland/build.sh` 会用它们编 `libredwood-gl.dylib`。**注意本仓库当前把 build.sh 里自动重跑 mk-mesa 的判断改成了 `|| true`（见下节）**，换机后第一次建议手工确认这些产物存在。

---

## 5. 构建用户态镜像 + 上机

```bash
cd userland/userland
# 关键环境：GL=1 启用 OpenGL 渲染器；RADIO=0 不带 Wi-Fi 固件（控制 payload 体积 < 63MB）
GL=1 RADIO=0 BUILD_METAL_PLUGIN=1 METAL=1 REST=1 bash build.sh
# 产物：root.apfs（本仓库的构建脚本会把 libredwoodair.dylib / RedwoodMetal.bundle 带进镜像）

# 打 payload（base/xnuloader/mkpayload.py）
cd ../..
python3 base/xnuloader/mkpayload.py -o /path/to/ios26.bin \
  kernel=/path/to/mach.xuanwu.copyio-fixed ramdisk=userland/userland/root.apfs \
  --cmdline "cpus=4 fb_threads=2 bootcpu=2 -v vm_compression_limit=131072 console_zoom=1 console_pages=0 \
    xuanwutouch=1 touch_half_ns=2000 dadatouch_delay=1 touch_chunked=1 dadabuttons=1 buttons_steps=5 \
    caenv=CA_DISABLE_ASYNCHRONOUS=1 gl=1 clockplain=1 clocka11y=1 sb_wallpaper=0 displaykeep=1 \
    loader_return_after=600 epoch=$(date +%s)"
```

上机（Windows fastboot，loader 命令）：

```text
fastboot oem stop                 # 停当前运行
fastboot stage ios26.bin
fastboot oem run                  # 运行 iOS payload
fastboot oem screenshot + get_staged   # 截图
fastboot oem log / poll-loader.py      # 日志（日志是环形缓冲，抓帧要尽早）
fastboot reboot                   # 回安卓
```

系统镜像上传（每次整机重启后要重做）：`oem ramdisk` + `stage part.01` + `oem ramdisk append` + `stage part.02`，见 `docs/xuanwu/` 内相关文档。

---

## 6. 下一步任务清单

1. **Metal 最终打通**（原生时钟 / 玻璃效果 / GPU 加速）
   - `libredwoodair.dylib` 已可编出（Mesa 26.2.5 + `rebase-26`）；缺的是内核给 Metal 库提供 msm 风格 GPU 设备（参考 K90 内核、`docs/xuanyuan/metal-air-final-state.md`）。
   - 过渡方案已上机验证：`inject26.c` 的 `redwood_clock_overlay` 在锁屏上盖 UILabel 显示时间（`clockplain=1` 时启用，pthread 每秒刷新，18:16 -> 18:17 实测）。
2. **Wi-Fi**（可选，工程量大）：内核补 PCIe 栈后接 peach 驱动；firmware 集已在旧镜像里验证可读。
3. 触摸已收尾：真实手指 + 脚本触摸都可用；`touch_frame_dump=1` 的 0xC0 帧日志保留作诊断。

## 7. 关键文件地图

| 路径 | 内容 |
| --- | --- |
| `xnu-src/isolated/xnu-xnu-12377.121.6/bsd/dev/hakios_dada_touch.c` | 触摸驱动（S3910/TCM V4、0xC0 帧、boot-arg 开关）|
| `.../bsd/dev/hakios_dada_gpu.c` | Adreno 830 完整上电驱动（`dadagpu=6 dadagpu_keep=1`）|
| `.../bsd/dev/hakios_dada_wifi.c` | Wi-Fi（peach/MHI 桩）|
| `.../iokit/Kernel/MiniIOMobileFramebuffer.cpp` | 显示/IOAcceleratorES（`metal=1` 开关）|
| `userland/userland/src/inject26.c` | iOS 26 注入库：GL 渲染器、Metal 插件命名、触摸/按键、脚本触摸 `taps=1`、显示守护 `displaykeep=1` 等 |
| `userland/userland/src/redwoodmetal.m` | Metal 插件实现（`rwa_*` 接口，dlopen `libredwoodair.dylib`）|
| `userland/tools/mesa/` | AIR 前端源码（51 文件）+ Mesa 补丁（含 `rebase-26/`）|
| `userland/tools/mk-mesa.sh` | Mesa 构建脚本（`VERSION/SHA256/REBASE/PREPARE/AIR` 开关）|
| `docs/xuanwu/` | 全部调试记录（GPU/Metal/触摸/PCIe/时钟等 58 篇）|
| `tools/xuanwu-helpers/` | 调试期间用的 Windows/WSL 辅助脚本（抓日志、打包、上机）|

---

## 8. 注意

- 全程 **RAM-only**（`fastboot boot` + `stage`），不改任何分区；系统镜像每次整机重启后要重传。
- 日志是环形缓冲（约 1.3 MB），重要输出要尽早 `poll-loader.py` 抓。
- 内核改动必须同时改 `xnu-src/xnu-12377.121.6` 与 `xnu-src/isolated/xnu-xnu-12377.121.6` 两棵树（编译用 isolated 那棵）。
- 本仓库不含任何厂商二进制（iOS 镜像、固件、SDK 均不入 git）。
