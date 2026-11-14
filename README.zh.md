# HakiOS

[English](README.md) · 中文 · [Українська](README.uk.md)

**把 Apple 开源的 Darwin 内核（XNU，xnu-8796.121.2）运行在搭载高通 SoC 的 Android 手机上**，就像 Android 是 Linux 在手机上的
适配一样：有自己的引导程序、自己的驱动、自己的配置系统和自己的版本号。同一个内核也能在 QEMU 里运行，不需要任何 Apple 的东西，
也不需要任何手机的东西（`make qemu-open`）。

**文档：[docs/](docs/index.md)**，项目网站（`hakios-web`）上也有。可以从[入门](docs/getting-started/index.md)、
[移植设备](docs/porting/index.md)或[架构](docs/concepts/architecture.md)开始看。

## 能用什么

| | POCO X5 Pro 5G（`redwood`） | QEMU `virt` |
|---|---|---|
| 锁屏、主屏、控制中心、系统自带应用 | 可以 | 可以（慢：CPU 是模拟的） |
| 触摸、多点触控、按键 | 可以 | 可以 |
| 全部 8 个 CPU 核心，大核 2.4 GHz | 可以 | 否 |
| Adreno GPU 渲染（经 Mesa 的 OpenGL ES） | 可以 | 否 |
| 扬声器、麦克风、电池电量 | 可以 | 仅声音 |
| Wi-Fi、摄像头、振动 | 可以 | 仅网络 |
| `/private/var` 跨重启保留、Sileo、SSH | 可以 | 否 |
| Metal | 部分：有一个 Metal 能加载的设备，缓冲区和库可用，还不能绘制 | 部分 |
| 蜂窝网络 | 部分：调制解调器已启动、SIM 已绑定、能看到网络，尚未注册 | 否 |
| 蓝牙、本内核下的充电、Apple 的服务 | 否 | 否 |

每台设备各自的 `device.conf` 记录了每一项：[设备](docs/devices/index.md)。关于锁屏和应用的几行，描述的是你自己加上下文第 2 层之后的情况。

小米 10（UMI）的移植放在 [devices/xiaomi-mi-10/](devices/xiaomi-mi-10/README.md)，包含设备专用的驱动和构建覆盖层，状态见下一节。

## 小米 10（UMI，骁龙 865）：移植状态

截至 **2026-11-07**。下面每一项都是在手机上的内存会话里验证的（`fastboot boot` 进内存，不刷写任何分区，重启后手机回到它自己的
Android）。默认启动参数在 [`devices/xiaomi-mi-10/boot.args`](devices/xiaomi-mi-10/boot.args)；构建和运行方法见
[devices/xiaomi-mi-10/README.md](devices/xiaomi-mi-10/README.md)。

### 已经可用（真机验证）

| 功能 | 启动参数 | 说明 |
|---|---|---|
| iOS 16.5 用户空间：锁屏、主屏、系统自带应用、简体中文资源 | — | 第 2 层，用你自己的 IPSW 构建 |
| 触摸屏（FTS）、多点触控、电源键和音量键 | `umi_touch=1 umi_keys=1` | |
| 全部 8 个 CPU 核心及调频 | `cpus=8 cpufreq=1` | 需要通过 USB 实时取日志时改用 `bootcpu=7 cpus=7` |
| Adreno 650 GPU 渲染（经 Mesa 的 OpenGL ES） | `gpu=1` | 原厂最高频率 587 MHz |
| AMOLED 亮度调节 | — | DSI DCS `0x51`，配合 backboardd 挂钩 |
| 扬声器和麦克风 | `adsp=1 adsp_umi=1 sound=4 mic=4` | 经音频 DSP |
| 电池电量显示、UEFI 充电协议 | `battery=1` | |
| 振动 | `umi_haptic=5` | |
| 手电筒 | `torch=3` | 只用通道 0，最大 125 mA，连续 30 秒或电池 45 °C 自动关闭（[FLASHLIGHT.md](devices/xiaomi-mi-10/FLASHLIGHT.md)） |
| **Wi-Fi**（QCA6390，PCIe/MHI）：扫描、连接、上网 | `wifi=1` | **2026-11-07 修复**，见下文 |
| **指南针**（传感器 DSP（SLPI）后面的 ak0991x 磁力计） | `slpi=1 slpi_attach=1` | **2026-11-07 可用**，限制见下文 |
| 最大 8 GiB 内存的机型 | — | |

### 部分可用或实验性

| 功能 | 现状 | 还缺什么 |
|---|---|---|
| 后置主摄（S5KHMX，`cam=5`） | 能看到真实预览，照片能存进相册；源图 3008×2256 RAW10，AK7377 反差对焦在保守、未校准的范围内工作 | 前置摄像头（S5K3T2）和其他后置模组；校准过的对焦；全分辨率照片"保存后再打开"尚未单独确认（[CAMERA.md](devices/xiaomi-mi-10/CAMERA.md)）。`cam=5` 还没有进入已提交的 `boot.args` |
| Metal（`metal=1`） | 原生缓冲区、命令队列、拷贝/填充已验证；隔离测试中一个 AIR 着色器在 Adreno 650 上正确执行 | 渲染管线和 Metal 合成器：主屏仍然走 OpenGL ES（[METAL.md](devices/xiaomi-mi-10/METAL.md)） |
| 蓝牙（QCA6390，H4，`umi_bt=1`） | 控制器固件能加载，H4 端点就绪，iOS 的 bluetoothd 在扫描 | 配对和音频尚未验证 |
| 指南针精度 | 表盘会跟着手机转动；平放转一整圈完成硬铁校准 | 传感器 X/Y 轴到手机坐标系的映射还需修正；拿不到 SLPI 融合姿态时，航向按手机平放计算 |

### 尚未完成

- 传感器 DSP 后面的其他传感器（加速度计、陀螺仪、光线、距离、气压）：DSP 已经读取它们的 registry，但还没有提供给 iOS，所以没有
  自动旋转，也没有水平仪。
- UMI 上的蜂窝网络（[docs/cellular.md](docs/cellular.md) 里的调制解调器工作是 POCO X5 Pro 的）。
- Apple 的服务：不支持，也不是目标。

### 近期修复

| 日期 | 问题 | 原因 | 修复 | 提交 |
|---|---|---|---|---|
| 2026-11-07 | Wi-Fi 显示为开启，但扫描不到网络、也关不掉；2026-11-05 以后的每个会话都这样 | `cbcal_restart_fw` 被改成了 `false`，固件在冷启动校准（mode 7）和正常工作模式（mode 0）之间没有重启；MHI 报 EIO，ath11k 在启动 15 秒后放弃 | 改回 `cbcal_restart_fw = true`：ath11k 的断电/上电会执行 UMI 的端点重置和校准后固件重启 | `2df9ad2` |
| 2026-11-07 | 普通启动时指南针没有数据 | `boot.args` 里缺 `slpi_attach=1`，传感器 DSP 的 FastRPC 会话没有启动 | 加入 `boot.args` | `93596c5` |
| 2026-11-07 | 全新检出的仓库编不过内核 | 代码引用了 `umi_sensor_registry.inc`，但生成它的脚本没有入库 | 新增 `tools/embed-sensor-registry.py`，由 `build-kernel.sh` 调用 | `93596c5` |
| 2026-11-07 | 硬铁校准可能被半圈或来回晃动"骗过" | 只按各轴跨度取中心 | 闭环校准：磁场必须离开起点、走过足够路径、围出面积再回到起点 | `93596c5` |
| 2026-11-06 | 指南针一打开就闪退（SIGSEGV，地址 `0x8`） | 自己构造的 `CMDeviceMotion` 没有内部状态 | 改用 App 自带的 `CompassTestDeviceMotion`，只通过它自己的 `CompassMotionManager` 返回 | `479e05a` |
| 2026-11-05/06 | 磁力计始终不出现（`mag` SUID 为空） | DSP 以目录形式请求持久化 registry，而这个目录没有提供；FastRPC 的 intent 在第 65 次回调时耗尽；QRTR 流控被忽略 | 实现 apps_std 的 `stat`/`opendir`/`readdir`，提供本机自己的 187 个 registry 文件，`DONE_REUSE` 用正确的 GLINK 通道，回复 `RESUME_TX`，订阅 SNS 数据流 | `866d151`、`e70070a`、`479e05a` |
| 2026-10-29 | 普通启动里手电筒不可用 | 启动参数里缺 `torch=3` | 加入 | `818e0fc` |
| 2026-10-29 | 亮度、电池 | — | DSI DCS `0x51`；电池电量显示和 UEFI 充电协议 | `3343a83`、`77fa050` |

### 已知问题

1. **滑动掉帧、卡顿。** 显示拷贝已经用满整个安全的 DMA 空闲区间，并且在主动扫描期间休眠（`403e230`），但滑动仍然不流畅。未解决。
2. **loader 的 USB 链路会断。** 长会话之后，或内核返回 loader 之后（屏幕显示 "the session is over"），`fastboot devices`
   仍能看到手机，但任何命令都报 `Write to device failed (no link)`，电脑既取不到日志也无法让它重启。上传 2.6 GB 系统镜像时也偶尔
   失败（`xl: upload failed`），手机会停在 loader。这两种情况都要长按电源键约 15 秒。`loader_reboot_after=1` 能让 loader 在内核返回
   60 秒后自己重启回 Android（另有硬件看门狗兜底）；这个改动还在工作区，**尚未提交**，而且不覆盖上传失败的情况。
3. **长会话中实时日志会中断。** 手机一直在正常运行，但电脑通过 USB 定时取日志会停下来，于是电脑端误报"内核无应答"。
4. **指南针的限制**：轴映射和倾斜补偿，见上文；每次开机后都需要平放转一整圈完成校准。
5. **摄像头的限制**：见上文。

### 下一步

1. 提交 loader 的自动恢复，并扩展到上传失败和 loader 长时间空闲的情况，这样会话可以无人值守地运行。
2. 修正指南针的轴映射；把加速度计提供给 iOS，用于倾斜补偿、水平仪和自动旋转。
3. 蓝牙配对和音频。
4. 显示路径的滑动性能。
5. 前置摄像头、校准过的对焦，以及把 `cam=5` 放进已提交的默认参数。

## 三个层次

1. **开放核心**：内核移植、引导程序、驱动、工具。全部是这些仓库里的源码。
2. **你自己放在上面的东西**：内核可以启动你自己提供的用户空间。项目的构建方式使它能够启动 iOS 16.5 用户空间，但那是你自己用从
   Apple 下载的文件在本机整理出来的；项目不提供其中任何内容，也不绕过任何限制去获取它。你是否可以这样做、是否愿意这样做，由你决定。
3. **Apple 的服务**（激活、Apple ID、iCloud、App Store）：不支持，也不是目标。

默认情况下，引导程序通过 `fastboot boot` 进入内存，系统在内存里运行一个会话，重启后手机回到它自己的 Android。默认只写一样东西：
loader 自己的日志（几 KB），写入 `logdump` 分区；开启 `ufs_write=1` 时，内核只会写入你自己创建的 `darwin` 分区。这是一个研究用的
试验台，不是 ROM。

## 快速开始

```sh
git clone https://github.com/oosexgang/hakios.git hakios
cd hakios
make doctor           # 检查本机缺什么，以及该怎么办
make qemu-open        # 在 QEMU 里运行开放内核：不含任何 Apple 的东西，也不含任何手机的东西
```

在 QEMU 里运行 iOS（Linux，12 GB 磁盘，使用你自己获取的 IPSW），以及在手机上运行一个会话：
[QEMU](docs/getting-started/qemu.md)、[在手机上](docs/getting-started/phone.md)。

```sh
make redwood_defconfig   # 手机的配置：通用内核 + SC7280 的 + 手机自己的
make kernel              # 按这个配置构建内核和它包含的驱动
make phone SECS=600      # 在手机上内存运行 10 分钟
```

小米 10 用设备目录里的脚本构建和运行：

```sh
devices/xiaomi-mi-10/build-wifi.sh          # Wi-Fi 驱动对象（ath11k.o）
devices/xiaomi-mi-10/build-kernel.sh -j8 -k # 内核（同时生成 SLPI registry 表）
devices/xiaomi-mi-10/build-loader.sh
devices/xiaomi-mi-10/build-userland.sh
devices/xiaomi-mi-10/run.sh ramdisk         # 上传系统镜像（常驻手机内存）
XL_SECS=600 devices/xiaomi-mi-10/run.sh run # 一个 10 分钟的会话
```

## 仓库：通用、SoC、设备、UEFI

| 仓库 | 内容 |
|---|---|
| [hakios](https://github.com/oosexgang/hakios) | **通用**：内核移植、所有通用驱动、引导程序、用户空间、工具、这份文档，以及 `devices/qemu` |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G：它的 defconfig、状态、UEFI 平台、固件脚本，以及它的 SoC 层（`soc/`：高通 SC7280 系列） |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | HakiOS 的 UEFI：带 HakiOS 启动路径、横幅和名称的 Mu-Silicium |

通用仓库里的 `devices/manifest.conf` 列出每个仓库及其版本；`tools/hakios-device.py sync` 会把它们克隆到 `devices/` 和 `uefi/`。
每台设备一个仓库，自带它的 SoC 层；`tools/hakios-device.py new` 可以新建一个。[这些仓库如何组合](docs/porting/repositories.md)。

```sh
git clone https://github.com/oosexgang/hakios.git && cd hakios
tools/hakios-device.py sync      # 按 manifest 指定的版本取 SoC、设备和 UEFI 仓库
```

你自己的设备是叠在通用仓库上的一个仓库：`tools/hakios-device.py new` 从手机自己的设备树生成它，所有手机驱动都先留空，等移植后再加。
每个驱动都是内核配置里的一个选项（`CONFIG_HAKIOS_*`，即 XNU 的 `options` 和 `conf/files`），所以设备的 defconfig 只构建它拥有的
驱动。[移植设备](docs/porting/index.md)、[配置中的驱动](docs/drivers-config.md)。

## 仓库里有什么，以及永远不会有什么

有的：只有我们自己的代码和数据：对 Apple 开源 XNU 的补丁（`xnu/hakios.patch`，APSL 2.0）和我们的文件、引导程序、我们的程序和库、
工具、`prebuilt/`（用这些源码构建；其中的 loader **不含**任何 Apple 数据：能启动 iOS 的那个由 `make loader` 在你的机器上用你自己的
IPSW 生成），以及设备和 SoC 目录。

永远不会有的：任何 iOS 文件（系统镜像由你在自己机器上用你自己获取的 IPSW 构建，交给 `sysimage/unpack.sh`；不替你下载、不解密、
不绕过任何东西）；手机的固件（从你自己的手机取到 `fw-*/`，git 忽略这些目录）；密钥、证书、激活，或任何让手机在 Apple 面前冒充
iPhone 的东西。

## 独立项目，核心不依赖 Apple

HakiOS 是一个独立的研究项目，**与 Apple 没有任何关联，也未获得 Apple 认可**；提到 "iOS"、"iPhone"、"XNU" 和 "Apple"，只是为了
说明代码与什么打交道。核心内核、引导程序和开放工具在不含任何 Apple 东西的情况下就能构建和运行：

```sh
make qemu-open      # 开放内核 + 占位 loader + 一个很小的、直接调用系统调用的用户空间，在 QEMU 里运行
```

不需要 IPSW、iOS 用户空间或固件。启动原版 iOS 用户空间（`make qemu`、`make phone`）是可选路径，需要 IPSW 和手机固件，由你从自己的
设备提供。

## 许可与法律

我的代码遵循它所修改文件的许可：XNU 补丁为 Apple Public Source License 2.0（Apple 以该许可发布 XNU；条款已满足：修改以源码形式
发布，保留了声明），其余以各文件头为准。iOS、其镜像、Apple 商标和手机固件属于各自的所有者，不在本仓库中，也不由我分发；每个人从
自己的手机和自己的 IPSW 取得它们。提到 "iOS"、"iPhone" 和 "Apple"，只是为了说明代码与什么打交道。若你认为这里的某个文件侵犯了
任何人的权利，请开一个 issue，我会删除它。

细节在单独的文件里：`LEGAL.md`（分发什么、不分发什么，以及这些声明的边界）、`COMPLIANCE.md`（APSL 2.0 和其他条款如何满足，以及
尚待核实的事项）、`THIRD_PARTY_LICENSES.md`（每个第三方组件的来源和许可）、`NOTICE`（简短的正式清单）。以上都不构成法律意见。

hakios 是一个实验性项目，目标是把 iOS 带到 Android 硬件上，并让 hakios 免费、人人可用。

维护者：
@oodze - 创始人
@netusd - 推广 / ceo-dev
@wannqn - 开发
@hxfuxyyXD - 开发
Rhaenja Hun（杭润嘉） — 开发 / 赞助

感谢 sicilium 提供的源码！
