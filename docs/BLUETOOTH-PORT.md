# 蓝牙现状、硬件取证与 sheng 对照

**2026-11-12 19:2x 北京｜17 线 / 21H461 / SM8635 cliffs（Xiaomi Pad 7 Pro）｜纯只读调研**

> 范围：17 线蓝牙能不能做、本机是什么硬件/哪条 UART、sheng 有什么能搬、芯片命名矛盾怎么解、现在该不该做。
> 证据等级：`[代码]` 源码逐字、`[DT]` 本机 stock DTB/DTBO 反编译、`[实测]` 真机记录、`[外部]` 公开资料、`[推测]` 未验证推断。
> 本轮**未刷机、未碰设备、未构建、未改任何源码 / 镜像 / 板级配置 / GPU 工作树**。设备当前 `vendor_boot=原厂`，在 rec 充电。

---

## §0 一句话结论

**17 线连一个蓝牙 area 都没有**（`porting.toml` 全文无 `bluetooth` 字样），零代码、零固件、零 ADT 节点、零启动参数；本机硬件是 WCN7850 的 BT 侧 + 一条 HS-UART，sheng 有完整的 `QcaBluetooth`（`device-verified`）。但蓝牙比 WiFi 更难——要多搬 GENI UART DMA、H4、固件/NVM、原生 `bluetoothd` 的 arm64e 二进制适配与签名——而且和 WiFi 一样卡在 §52 的 `PROC n=1 launchd=0`。**排序应在 WiFi 之后。**

| 问题 | 答案 | 等级 |
| --- | --- | --- |
| 17 线做过蓝牙吗？ | **没有**。无独立 area、无源码、无固件、无 ADT `bluetooth` 节点 | `[代码]` |
| 本机 BT 芯片？ | **WCN7850 的 BT 侧**（整机 Wi-Fi 7 + BT 5.4）；DT compatible 却写 `qcom,wcn6750-bt`，是下游遗留命名，见 §4 | `[DT]`+`[外部]` |
| 本机 HCI 走哪条 UART？ | `qupv3_se10_4uart` @ `0x888000`（QUP v3_1 SE10，HS-UART），SPI 584 + GPIO 43 wake | `[DT]` |
| sheng 跑通了吗？ | **跑通**：`id="bluetooth" status="device-verified"`，扫描/配对/重连/存 key/BLE；真机 AirPods Pro 配对 + 重启重连 + A2DP 正常播放、无 panic 无 UART 错误 | `[实测]` |
| 能整套搬吗？ | **不能**。UART 引擎/中断/GPIO/供电/固件/OS 服务五项全不同 | `[DT]`+`[代码]` |
| 有现成可复用的坑位吗？ | **有且重要**：TLMM summary INTID 240 本机已被触摸探针占用，sheng 在这里踩过 worker 自旋 | `[代码]` |
| 现在能做吗？ | **不能**。launchd 未创建，`bluetoothd` 不跑 | `[实测]` §52 |

---

## §1 17 线现状：连一个 area 都没有

`UniXNU-muyu-17/porting.toml` 的章节全表是 `configuration / firmware / cpu / memory / storage / display / input / gpu / network / audio / power / packaging / diagnostics`——**没有 `bluetooth`**，全文 grep 不到 `bluetooth` / `QcaBluetooth` / `IOBluetooth` / `HCI` / `hmtbtfw` 任何字样。`network` 与 `audio` 都是 `status="not-applicable"`（见 `WIFI-PORT.md §1`）。

| 检查项 | 结果 |
| --- | --- |
| `package/` 下蓝牙 / UART / HCI 源文件 | **零** |
| `drivers/` 下 UART 驱动 | 只有 `UniXNU/drivers/QcomGeniSpi/`（**SPI**，不是 UART）；无 `Sm8550GeniUart.h` / `qbt_*` / `WcnPower.h` |
| 蓝牙固件 / NVM（`hmtbtfw*.tlv` / `hmtnv*.bin`） | **零**（全项目只有 GPU `gen71100_*.mbn`） |
| ADT `bluetooth` 节点 | **零**（sheng DTS 有 `unixnu,adt-name = "bluetooth"`） |
| 启动参数 `bluetooth=*` / `sheng-bluetooth=*` | **零**；当前镜像二进制直搜也无 |
| `docs/` 里蓝牙记录 | 只有 `GPU-HANDOFF.md` / `WIFI-PORT.md` 顺带提及，无实现文档 |

**一条容易读错的注释**：`porting.toml:118` 写 `the panel is the primary readout channel (no UART, and the USB console was removed 2026-11-08)`，`board.toml:291` 写 `this device has no wired UART`。这说的是**没有可用的调试串口 / 控制台回读通道**，不是硬件上没有 UART 控制器——本机 DT 里明明有两条 GENI UART（调试用 `qupv3_se5_2uart@a94000`、蓝牙用 `qupv3_se10_4uart@888000`）。17 线 XNU 缺的是**驱动**，不是硬件。

---

## §2 本机硬件取证（stock DTB / DTBO 反编译）

### 2.1 BT 供电/使能节点

`fw/dtb_04.dtb` → `bluetooth: bt_wcn6750`（`/tmp/muyu_dtb04.dts:15640-15665`）：

| 属性 | 值 |
| --- | --- |
| `compatible` | **`qcom,wcn6750-bt`**（命名矛盾见 §4） |
| 别名 | `/chosen` 无；aliases 里 `bluetooth = "/soc/bt_wcn6750"`（`dts:20113`）、`bt_en_sleep`（`dts:20114`） |
| `qcom,bt-vdd-*-config` | **8 组**：ipa-2p2 / rfa2 / rfa1 / rfa-0p8 / rfacmn / dig / aon / io，各带 min/max 电压与标志 |
| `qcom,bt-vdd-*-supply` | 8 个 regulator phandle（`0x345/0x347/0x346/0x348/0x1e0`，其中 4 路共用 `0x348`） |
| `qcom,wl-reset-gpio` | `<&tlmm 0x23>` → **GPIO 35**（与 WiFi 的 `wlan-en-gpio` 同脚） |
| `qcom,bt-sw-ctrl-gpio` | `<&tlmm 0x50>` → **GPIO 80**（与 WiFi 的 `qcom,sw-ctrl-gpio` 同脚） |
| `qcom,bt-reset-gpio` | `<&tlmm 0x9c>` → **GPIO 156**（与 WiFi 的 `qcom,bt-en-gpio` 同脚） |
| `mpm_wake_set_gpios` | `<0x50>` = GPIO 80 |
| `pinctrl-names` | `default`（`bt_en_sleep` 定义在 `dts:10307`） |

**这三条 GPIO 与 `WIFI-PORT.md §2.1` 的 `qcom,cnss-kiwi` 节点逐字相同** ⇒ 两个节点描述的是**同一颗 combo 芯片**的两个 Android 驱动视图（WLAN 侧 CNSS2 / BT 侧 btpower），不是两颗芯片。

音频侧：主 DTB `pineapple_snd` 里 `qcom,wcn-bt = <0x00>`，dtbo overlay 里改成 `qcom,wcn-bt = <0x01>`、`qcom,model = "cliffs-mtp-snd-card"` ⇒ 蓝牙音频路由在 overlay 中是打开的。

### 2.2 HCI UART

`fw/dtb_04.dtb:14523-14540` → `qupv3_se10_4uart: qcom,qup_uart@888000`：

| 属性 | 值 |
| --- | --- |
| `compatible` | `qcom,msm-geni-serial-hs`（高通高速 UART） |
| `reg` | `0x888000 + 0x4000`，`reg-names = "se_phys"` |
| 所属 wrapper | `qcom,qupv3_1_geni_se@8c0000`；alias `hsuart0` 指向它（`dts:439`） |
| `interrupts-extended` | `<GIC_SPI 584 (0x248), level-high>` + `<TLMM GPIO 43 (0x2b), level-high>` |
| `clocks` | `se-clk`（GCC phandle `0x16e` index `0x5a` = 90） |
| `interconnects` | `qup-core` / `qup-config` / `qup-memory` 三路 |
| `pinctrl-names` | `default` / `active` / `sleep` / `shutdown` 四态 |
| `qcom,wakeup-byte` | `0xfd` |
| `status` | **`ok`**（不用 overlay 打开） |

`fw/android-harvest/firmware/listing.txt` 只有 4 个 GPU 文件，`MANIFEST.tsv` 无 wlan/bt 条目，root sparse image 也搜不到 `hmtbtfw` / `hmtnv` / `kiwi_v2` ⇒ **蓝牙固件与 NVM 一个字节都没拿到**（与 `board.toml:461-464` 触摸固件「`/vendor/firmware` 无 unprivileged 路径可读」同类障碍）。

### 2.3 🔴 一个已存在的中断冲突点

`UniXNU-muyu-17/package/TouchProbeMuyu.cpp:1627` 逐字：

```
// TLMM summary SPI 208 that armcpu_spi_attach() already knows how to hook (INTID 240). Register layout is
```

`:1669` 实际调用 `armcpu_spi_attach(32 + TM_TLMM_GIC_SPI, false, touchPinIrq, ...)`。

而 sheng `porting.toml:345` 的 findings 逐字：

> The Bluetooth RX wake pin shares **TLMM summary interrupt 240** with touch and keyboard and its raw status latches while the wake is disarmed; a wake acknowledgement queued behind a stopped UART clock made the worker spin, fixed in QcaBluetooth.

**同一个 INTID 240、同一类共享场景**：sheng 已经在真机/宿主层踩过「BT RX wake 与触摸/键盘共享 summary IRQ、UART 时钟停掉时 wake ack 让 worker 自旋」的坑，17 线的触摸探针现在正占着这条线。将来加蓝牙必须复用 `armcpu_spi_share()` 而不是再 `attach` 一次，并处理时钟停掉时的 ack 排队。这是本机最有价值的一条「别人已经付过学费」的记录。

---

## §3 sheng 有什么（`origin/main = cd73e23`）

`porting.toml:338-410`，`id="bluetooth" status="device-verified"`，`components = ["QcaBluetooth"]`。

**规模与组件**：

| 部分 | 内容 |
| --- | --- |
| 板级 | `package/BluetoothProbe.cpp` **525** / `BluetoothFirmware.c` 52 / `.h` 17（合计 594 行） |
| 协议栈 | `drivers/QcaBluetooth`（host check 说「编译 8 个协议和 SM8550 对象」）；`qbt.h` / `qbt_geni.h` / `qbt_transport.h` / `Sm8550GeniUart.h` / `SmmuBypass.h` / `AppsSmmuLock.h` / `WcnPower.h` |
| 脚本 | 8 个：`build-qca-bluetooth.py` / `build-bluetooth-userspace.py` / `stage-bluetooth-firmware.py` / `check-bluetooth-{board,platform,runtime,shared}.py` / `check-wcn-power.py` |
| 报告 | `reports/ipados27-bluetooth-startup.md`、`ipados27-bluetooth-native.md` |
| ADT | `dts/sheng.dts:17-24`：`compatible = "qcom,wcn7850-bt"`、`unixnu,adt-name = "bluetooth"`、`transport-encoding = <1>`、`transport-speed = <3200000>` |

**硬件配置**（`porting.toml:343` 逐字）：UART14 = QUP2 SE6 @ `0x898000`、GIC INTID 493、wrapper `0x8c0000`、apps-SMMU stream `0x423`；CTS/RTS/TX/RX = GPIO 76/77/78/79，BT_EN = GPIO 81；**3.2 Mbaud**（51.2 MHz 串口源 / 16× 过采样）。

**固件**（`firmware/bluetooth/manifest.json`，来自 `github.com/ianchb/sheng-firmware` @ `053da477`）：

| 文件 | 字节 | SHA-256 |
| --- | --- | --- |
| `hmtbtfw20.tlv` | 231,484 | `e757354e…2720` |
| `hmtnv20.bin` | 9,036 | `8c54ce84…51e4` |

`BluetoothFirmware.c` 只接受 `qca/hmtbtfw??.tlv` / `qca/hmtnv??.bin` 这类名字，从 Data addon 的 VFS 读，`native_gpu_addon_stage < 1` 时返回 `EAGAIN`（**和 WiFi 固件一样，依赖 addon 先安装完**）。

**用户态**：把原厂 24A446 的 arm64e `bluetoothd` 与 BlueTool 做 native adapter 适配，产出 3 个签名的 arm64e 二进制、716 个 launchd jobs、45 个校验文件、176 MiB FAT payload；host 验证含 144 个 AArch64 指令用例（两个 ASLR slide）、58 个 ASan/UBSan HCI/policy 用例。启动参数在 `board.toml` 的 `kernel_args` 里是**裸** `bluetooth=1`（17 线则是 `sheng-` 前缀，见 `POWER-MANAGEMENT.md` 的 OS 差异）；另有 `log-bluetooth=2` 与 `kern.qbt_*` sysctl（如 `kern.qbt_power_on`，`porting.toml:345` 提到 sandbox 拒绝写 sysctl 的处理）。

**`completion`（真机，`[实测]`）**：内核/payload 带独立蓝牙电源与活的 UART DMA/IRQ 启动；原生栈**扫描、配对、重连、存 key、BLE、播放蓝牙音频**。`porting.toml:417` 的 device evidence 更具体：`bluetooth=1 log-bluetooth=2` 下 **AirPods Pro 配对、重启后重连、A2DP 正常音量与音质播放，噪声控制与电量可用，整轮只有一个 `bluetoothd`、无 panic、无 UART 错误**（该镜像早于 `kern.qbt_*` 改名）。`limits`：HFP/SCO 通话与麦克风、sleep/wake、idle power、recovery 未测；依赖 Apple vendor command 的功能受限；从 submodule 解析驱动的构建尚未跑过。

### 3.1 一份对当前主线特别有用的失败记录

`reports/ipados27-bluetooth-startup.md` 开头逐字：

> The runtime overlays reach stage 5, status 0. **PID 1 exits with namespace 7, subcode 1, `No service cache`.** Automatic reset is disabled.

报告里给出打包 cache 与设备 parser 输入的二进制 plist 对比：bytes `1327012` vs `1327067`、FNV-1a `5a19e661…` vs `fcee8a41…`、对象数 `44478` vs `44479`、偏移表 `1149068` vs `1149119`。

**建议转给接手 GPU 的 AI**：17 线 `GPU-HANDOFF.md §52/§53` 当前断点正是 `load_init_program()` 内的 **owner-cache / Data mount 子阶段**，`§53` 预建的 `MUYU ROOT` 只读镜像就是去读 `native_owner_cache_stage/status/mount_status`。sheng 在同类阶段吃过 `No service cache` + cache 二进制长度/hash 不一致的亏，这条对定位关卡 1 可能比 GPU 侧证据更直接。

---

## §4 芯片命名矛盾：`qcom,wcn6750-bt` 为什么不能推翻 WCN7850

上一轮 `WIFI-PORT.md §4` 判定本机 `qcom,cnss-kiwi` = WCN7850，与 sheng 同芯片。本轮发现本机 BT 节点 compatible 是 **`qcom,wcn6750-bt`**，而主线 Linux 的 `qca_bluetooth_of_match` 里 `wcn6750-bt` → `qca_soc_data_wcn6750`、`wcn7850-bt` → `qca_soc_data_wcn7850` 是**两套不同的 soc_data**。所以这不是能随手忽略的别名，必须给证据：

| 方向 | 证据 | 等级 |
| --- | --- | --- |
| **支持 WCN7850** | ① 整机公开规格：Xiaomi Pad 7 Pro / SM8635 是 **Wi-Fi 7 (802.11be) + Bluetooth 5.4**；而 WCN6750 是 **Wi-Fi 6E + BT 5.2**（EverythingRF/主线 ath11k 补丁逐字），WCN7850 才是 Wi-Fi 7 世代 | `[外部]` |
| | ② 本机 dtbo `fragment@55/56` 把 `wpss_mem` / `wpss_pas` 置 `disabled`、`fragment@54` 禁用 `wlan_msa_mem`、`fragment@57` 打开 `pcie0`。**WCN6750 是 WPSS Q6 枚举的 AHB/PCIe hybrid 设备，必须靠 remoteproc 启动 WPSS**；本机走 PCIe0 直挂 `cnss_pci0` + 384 MiB DMA 窗口，是 WCN7850 的 MHI/PCIe 路径 | `[DT]` |
| | ③ `qcom,cnss-kiwi` 节点布局（`0xb0000000+0x10000`、PMU vreg map、`qcom,qmp`）与公开 WCN7850 DT 图谱逐项一致；`kiwi_v2` 固件目录与 OnePlus 12 的 WCN7850 路径同名 | `[DT]`+`[外部]` |
| | ④ 两个节点的 WLAN/BT/SW_CTRL GPIO 完全相同 ⇒ 同一颗 combo 芯片 | `[DT]` |
| **反证** | BT 节点 compatible `qcom,wcn6750-bt` | `[DT]` |

**结论**：`qcom,wcn6750-bt` 是 **Android 下游 BT 供电节点从旧平台复制后未更新的遗留 compatible**（该节点描述的是 `qcom,bt-vdd-*-config` 供电与 `*-gpio`，靠厂商 btpower 驱动消费，不靠主线 `qca_soc_data` 分派）。本机实际是 WCN7850 + BT 5.4，`WIFI-PORT.md §4` 的结论**成立**，但置信度应读作「高置信 + 一条已解释的反证」，而非「DT 直接确认」。

⚠️ 仍有 `[推测]` 成分：**本机 PCIe 端点 `17cb:1107` 与 BT 的 HCI chip version 均未真机读过**。真正的终局证据是 P2 阶段通过 HCI vendor command 读回 chip/ROM version，或在 WiFi 侧读 PCIe config space——在此之前不要在配置里写死 `wcn7850-bt`。

---

## §5 不能直接照搬的差异

| 维度 | sheng（SM8550 / iPadOS 27） | muyu（SM8635 cliffs / iPadOS 17） |
| --- | --- | --- |
| **UART 引擎** | UART14 = QUP2 **SE6 @ `0x898000`**、GIC **INTID 493**、apps-SMMU stream `0x423` | `hsuart0` = QUP v3_1 **SE10 @ `0x888000`**、**GIC_SPI 584**、TLMM GPIO 43 wake；SMMU stream 未取证 |
| **wrapper** | `0x8c0000` | `0x8c0000`（**这一项相同**，是少有的可直接复用的地址） |
| **GPIO** | CTS/RTS/TX/RX 76/77/78/79、BT_EN 81 | 仅见 `wl-reset 35` / `bt-sw-ctrl 80` / `bt-reset 156` / `mpm_wake 80`；**四根 UART 数据脚未在本轮定位到** |
| **速率** | 3.2 Mbaud（51.2 MHz / 16×） | DT 无 `max-speed`；有 `qcom,wakeup-byte = 0xfd` |
| **供电** | `WcnPower` 六路 rail + RPMh mode/voltage/enable，与 WiFi 共享 PMU 协调 | **8 组** `qcom,bt-vdd-*-config` + 8 个 supply phandle；与 WiFi 的 8 组 `pmu_vreg_map` 并存 |
| **中断共享** | TLMM summary INTID 240 与触摸/键盘共享，已修 worker 自旋 | **同 INTID 240 已被 `TouchProbeMuyu.cpp:1669` 占用**，必须走 `armcpu_spi_share()` |
| **固件 / NVM** | `hmtbtfw20.tlv` + `hmtnv20.bin`（hash-pinned，来自 ianchb/sheng-firmware） | **零资产**；`hmtnv20.bin` 是板级 NVM 校准，绝不能照抄 |
| **用户态** | 原厂 24A446 `bluetoothd`/BlueTool + arm64e native adapter + 3 个签名二进制 + 716 launchd jobs | iPadOS 17 `bluetoothd` ABI / 签名 / ADT 条件**未查证**；且 launchd 未创建 |

---

## §6 决定性的前置 blocker

`GPU-HANDOFF.md §52`（接手 GPU 的 AI 于 18:02–18:06 追加）屏照逐字：

```
MUYU LIVE p=24 t=260 ...
PROC n=1 zomb=0 launchd=0- backboardd=0- SpringBoard=0- liveact=0-
```

root mount 已成功，ticker 在 `load_init_program()` 入口，断点在 owner-cache / Data mount 子阶段或 launchd exec（`§53` 已预建 `MUYU ROOT` 只读镜像，未刷）。

蓝牙对这一关的**依赖比 WiFi 更重**：sheng 的完整方案要发布 ADT `bluetooth` 节点、装载签名的 arm64e `bluetoothd`、注册 716 个 launchd job、等 Data addon 安装完才有固件（`native_gpu_addon_stage < 1 → EAGAIN`）。`PROC n=1` 时这些全部不成立。而且 sheng 自己的 `ipados27-bluetooth-startup.md` 就记过一次 `PID 1 ... No service cache` —— 恰好是 17 线现在卡住的同一段启动链（见 §3.1）。

⇒ 蓝牙与 WiFi、`IOPMPowerSource` / `thermal-level` 一样，都排在**关卡 1（launchd）之后**；并且因为要多一层原生二进制适配与签名，工作量明显大于 WiFi。

---

## §7 建议路线（不抢 GPU 主线窗口）

| 优先级 | 动作 | 需要设备 | 验收 / 产出 |
| --- | --- | --- | --- |
| **P0** | 等主线把 `load_init_program()` → launchd 断点定位并修好 | 是（主线占用） | `PROC n>1`、`launchd=<pid>`；**在此之前不投蓝牙用户态** |
| **P0′（离线，可并行）** | ① 从本机 vendor / root 提取 BT 固件与 NVM 并算 SHA；② 把 §2 的 GPIO / 供电 / UART / 中断事实固化成板级表；③ 从 DT 定位 SE10 的 CTS/RTS/TX/RX 四根脚与 pinctrl 组；④ 把 sheng `ipados27-bluetooth-startup.md` 的 `No service cache` 记录转给主线 | 否 | 带 hash 的 manifest + 板级表；**充电期间就能做** |
| **P1** | 最小**只读**探针：读 SE10 UART 寄存器、TLMM GPIO 状态、BT 供电 rail、INTID 240 现状与触摸探针的共享方式 | 是 | 屏照上确认 UART 时钟/引脚状态，**不写任何寄存器** |
| **P2** | 上电 + 使能 UART + HCI 握手，用 vendor command 读 chip/ROM version | 是 | **坐实 §4 的芯片判断**；同时验证 3.2 Mbaud 是否可用 |
| **P3** | 固件 `hmtbtfw*.tlv` / NVM `hmtnv*.bin` 加载 + H4 协议 + GENI DMA | 是 | HCI 起来、能扫描 |
| **P4** | ADT `bluetooth` 节点 + 17 线 `bluetoothd` 适配 + 签名 | 是 | 配对 / BLE / 音频；**必须在 P0 之后** |
| **与 WiFi 的协调** | `WcnPower` 的 PMU rail 协调、PCIe 排除、BT/WLAN SW_CTRL 共脚（都是 GPIO 80） | 是 | 参照 sheng `check-wcn-power.py` 的独立 release / 幂等投票模型 |

**价值判断**：蓝牙对当前调试的杠杆**低于 WiFi**（不能像网络那样直接把日志导出来）。它更偏「成品功能」，建议明确排在 WiFi 之后；但 §2.3 的 INTID 240 冲突是现在就该记下来的，因为它和触摸探针抢同一条线。

---

## §8 红线

1. 🔴 **别照抄 SM8550 的 UART 地址 / INTID / GPIO / SMMU stream**：本机是 SE10 `0x888000` + SPI 584，不是 SE6 `0x898000` + INTID 493；GCC vote 位图同样不能抄（`WIFI-PORT.md §9`）。
2. 🔴 **别用 sheng 的 `hmtnv20.bin`**：NVM 是板级校准（BD address / 功率 / 监管域），必须用本机自己的。
3. 🔴 **别在 INTID 240 上再 `armcpu_spi_attach()` 一次**：`TouchProbeMuyu.cpp:1669` 已经挂了触摸；要用 `armcpu_spi_share()`，并处理 sheng 记的「UART 时钟停掉时 wake ack 让 worker 自旋」。
4. 🔴 **别在 `PROC n=1` 阶段投用户态**：`bluetoothd` / BlueTool / 716 launchd jobs 全依赖 launchd。
5. 🔴 **别把 `qcom,wcn6750-bt` 直接写进任何新配置**：它很可能是遗留命名；P2 读回 HCI chip version 之前，芯片型号一律标 `[推测]`。
6. **别抢刷机窗口**：设备现在 `vendor_boot=原厂` 在 rec 充电，GPU 主线另有调度。
7. **别把「无 UART 控制台」误读成「硬件无 UART」**：`porting.toml:118` 说的是回读通道，DT 里 HS-UART 是 `status = "ok"`。

---

## §9 未验证项清单

| 项 | 等级 | 怎么验 |
| --- | --- | --- |
| 本机 BT 确为 WCN7850（而非 WCN6750） | `[推测]` + 已解释反证 | P2 用 HCI vendor command 读 chip/ROM version；或 WiFi 侧读 PCIe config `17cb:1107` |
| `qcom,wcn6750-bt` 确为遗留命名 | `[推测]` | 比对本机 vendor `btpower` 驱动是否按 compatible 分派 |
| SE10 的 CTS/RTS/TX/RX GPIO 编号 | **未定位** | P0′ 从主 DTB pinctrl 组（`0x23b..0x242`）反查 phandle |
| SE10 的 SMMU stream / DMA 窗口 | **未取证** | DT 反查 + P1 只读寄存器 |
| 本机 3.2 Mbaud / 51.2 MHz / 16× 是否同样适用 | `[推测]` | P2 实测 |
| 本机 BT 固件名与加载格式 | **未取证** | P0′ 提取后比对 `hmtbtfw??.tlv` 命名与格式 |
| 17 线 `bluetoothd` ABI / 签名 / ADT 条件 | **未查证** | P0 完成后才能验 |
| `TouchProbeMuyu.cpp` 的 INTID 240 handler 能否安全共享 | **未验证** | P1 只读 + 参照 sheng `check-bluetooth-shared.py` 的共享 IRQ 模型 |
| `MuyuRpmhRsc` 真机能否发出有效 RPMh 请求 | `porting.toml:209` 明写 readout 未采集 | 需一次真机只读取证 |

---

*本轮纯只读：未刷机、未碰设备、未构建、未改任何源码 / 镜像 / 板级配置 / GPU 工作树。*
*`logs/flash-ledger.tsv` 末条仍为 `2026-11-12T10:51:01Z flash vendor_boot fw/stock/vendor_boot.img rc=0`。*
*相关文档：`WIFI-PORT.md`（同芯片 WiFi 侧取证，§4 结论经本轮 §4 加固）、`POWER-MANAGEMENT.md`（PMU/RPMh 基础）、`DISPLAY-LOAD-BACKLIGHT.md §7`（同类「以为上游没有、实际同芯片」订正）、`GPU-HANDOFF.md §52/§53`（当前 blocker）。*
