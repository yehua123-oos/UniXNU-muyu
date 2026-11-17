# WiFi 现状、硬件取证与 sheng 对照

**2026-11-12 19:1x 北京｜17 线 / 21H461 / SM8635 cliffs（Xiaomi Pad 7 Pro）｜纯只读调研**

> 范围：17 线 WiFi 能不能做、本机是什么硬件、sheng 有什么能搬、现在该不该做。
> 证据等级：`[代码]` 源码逐字、`[DT]` 本机 stock DTB/DTBO 反编译、`[实测]` 真机记录、`[外部]` 公开资料、`[推测]` 未验证推断。
> 本轮**未刷机、未碰设备、未构建、未改任何源码 / 镜像 / 板级配置 / GPU 工作树**。设备当前 `vendor_boot=原厂`，在 rec 充电。

---

## §0 一句话结论

**17 线目前一行 WiFi 代码都没有，`porting.toml` 把它标成 `not-applicable`；但本机用的就是 sheng 那颗 WCN7850，硬件同源。真正拦住的不是驱动难度，而是 §52 的 `PROC n=1 launchd=0` —— 用户态连第一个进程都没起来，wifid / airportd / wpa_supplicant 全都不会跑。所以现在做 WiFi 等于给一个还没通电的房子装路由器。**

| 问题 | 答案 | 等级 |
| --- | --- | --- |
| 17 线做过 WiFi 吗？ | **没有**。`id="network" status="not-applicable"`、`components=[]`、`paths=[]`，逐字写着 `No network work on either line` | `[代码]` |
| 本机 WiFi 芯片？ | **WCN7850（Wi-Fi 7）**，DT 节点 `qcom,cnss-kiwi@b0000000`；`kiwi` 是该芯片代号 | `[DT]`+`[外部]` |
| 与 sheng 同芯片吗？ | **是**。sheng 端点 `17cb:1107`，代码 `if (gCfg[0] != 0x110717cb) return;` | `[代码]`+`[推测]` |
| sheng 跑通了吗？ | **跑通**：WPA2 join、DHCP、Safari、`wifid` 扫描 5–13 个网络；吞吐 12/24 Mbit/s | `[实测]` |
| 能整套搬吗？ | **不能**。GPIO、PMU rail、GCC/PCIe 位图、固件、OS 服务四项全不同 | `[DT]`+`[代码]` |
| 现在能做吗？ | **不能**。launchd 未创建，用户态网络栈没有消费者 | `[实测]` §52 |
| 值不值得做？ | **值得，但要排队**。通了以后可走网络取日志，直接消掉这个月多次屏照 OCR 误判 | `[推测]` |

---

## §1 17 线现状：官方标为「不适用」

`UniXNU-muyu-17/porting.toml:172-184` 逐字：

```toml
[[area]]
id = "network"
status = "not-applicable"
components = []
paths = []
inputs = []
configuration = []
procedure = []
findings = ["No network work on either line; WiFi was only ever a uke-side topic (uke branch fix/wifi-apple-cache-20261108)."]
completion = []
limits = ["No intent to bring network up in the current spike."]
next_action = []
evidence = []
```

逐项盘点结果（`[代码]` / `[DT]`）：

| 检查项 | 结果 |
| --- | --- |
| `package/` 下 WiFi/PCIe/MHI 源文件 | **零**（只有 `AdrenoFirmwareProbe.c` 名字沾 firmware） |
| `drivers/` / `kits/` 下 PCIe / MHI / QRTR / QMI / 网络目录 | **零** |
| `board.toml` / `porting.toml` 里 `sheng-wifi` 等启动参数 | **零**；当前镜像 `fw/vendor_boot_gpu_upper1.img` 二进制直搜也无 WiFi 参数 |
| `WCN7850` 固件 `amss.bin` / `m3.bin` / `board-2.bin` | **零**（全项目只有 GPU 的 `gen71100_*.mbn`） |
| ADT 的 `/arm-io/wlan` / `mac-address-wifi0` | **零** |
| `wifid` / `airportd` / `IO80211` / `Apple80211` 记录 | `docs/` 里仅一处提及 uke 的 CI 提交，无实现 |

**uke 分支那条线索的边界**（别高估）：`GPU-HANDOFF.md:2372` 记着 uke `origin/main` 的 8 个新提交**全部是** `.github/workflows/build-wifi-supplicant.yml` —— 在 macOS 上用 iOS SDK 构建、ad-hoc 签名 Mach-O `wpa_supplicant` 并上传 CDHash 的 CI 作业（`a1b6a2c` 的 commit message 逐字说明「Linux build host has no iPhoneOS userland SDK」）。**它是打包工具，不是 17 线的驱动移植**。主树的 `UniXNU/docs/drivers/ath12k-wifi.md` 是组件说明文档，同样不是 17 线实现。

---

## §2 本机硬件取证（stock DTB / DTBO 反编译）

取证方式（本轮实际执行的只读命令）：

```bash
dtc -I dtb -O dts -o /tmp/muyu_dtb04.dts fw/dtb_04.dtb
python3  # 解析 Android dtbo header，抽出 /tmp/dtbo_parts/00.dtb
dtc -I dtb -O dts -o /tmp/muyu_dtbo.dts /tmp/dtbo_parts/00.dtb
```

### 2.1 WLAN 主控节点

`fw/dtbo.img` → `fragment@60` → `wlan_kiwi: qcom,cnss-kiwi@b0000000`（`/tmp/muyu_dtbo.dts:5016-5060`）：

| 属性 | 值 | 含义 |
| --- | --- | --- |
| `compatible` | `qcom,cnss-kiwi` | Android CNSS2 平台设备 |
| `reg` / `reg-names` | `0xb0000000 0x10000` / `smmu_iova_ipa` | 固定 IOVA 窗口，PCI 用 |
| `qcom,wlan-rc-num` | `0` | 挂在 RC0（PCIe0） |
| `wlan-en-gpio` | tlmm `<0x23 0>` → **GPIO 35** | WLAN_EN |
| `qcom,sw-ctrl-gpio` | tlmm `<0x50 0>` → **GPIO 80** | SW_CTRL |
| `qcom,bt-en-gpio` | tlmm `<0x9c 0>` → **GPIO 156** | BT_EN |
| `qcom,wlan-sw-ctrl-gpio` | tlmm `<0x22 0>` → **GPIO 34** | 另有 `mpm_wake_set_gpios = <0x22 0x50>` |
| `pinctrl-names` | `wlan_en_active` / `wlan_en_sleep` | fragment@58 定义 |
| `qcom,qmp` | `&aoss_qmp` | AOSS QMP（PMU 通信） |
| `cnss-enable-self-recovery` / `qcom,wlan-cbc-enabled` | 存在 | 自恢复 / CBC |
| `qcom,iommu-*` | `cnss_pci0` 子节点 | 见 2.2 |

**PMU 供电**（`[DT]`，这是 sheng 那里最容易踩空的一项）：

- `qcom,pmu_vreg_map` 共 **8 组**：`VDD095_PMU→s3e`、`VDD_PMU_AON_I→s3b`、`VDD095_PMU_BT→s3b`、`VDD09_PMU_RFA_I→s3b`、`VDD13_PMU_PCIE_I→s2b`、`VDD13_PMU_RFA_I→s2b`、`VDD19_PMU_PCIE_I→s1b`、`VDD19_PMU_RFA_I→s1b`。
- `vdd-*-supply` 共 8 路：`S3E`(vdd-wlan)、`S3B`(dig)、`S1B`(rfa1)、`S2B`(rfa2)、`L5B`(io12)、`L7B`(io)、`L4E`(ant-share) 及 `qcom,vdd-wlan-ant-share-config`。
- `qcom,pdc_init_table` 11 条、`qcom,vreg_pdc_map` 4 组，含 `s3b.v upval 976` / `s1b.v upval 1864` / `s2b.v upval 1316`（mV 级 PDC 配置）。
- 另有 `qcom,bus-bw-cfg` 9 档、`interconnects` 指向 `pcie_anoc` / `gem_noc` / `mc_virt`。

### 2.2 PCIe 路径

`fw/dtb_04.dtb` → `pcie0: qcom,pcie@1c00000`（`/tmp/muyu_dtb04.dts:14701-14763`）：

| 属性 | 值 |
| --- | --- |
| `compatible` | **`qcom,pci-msm`**（Android MSM PCIe 驱动），不是 sheng 的 `qcom,pcie-sm8550` |
| `reg` | `parf 0x1c00000+0x3000`、`phy 0x1c06000+0x2000`、`dm_core`、`elbi`、`iatu 0x60100000+0x100000`、`conf`、`pcie_sm 0x1d07000+0x4000` |
| `status` | 主 DTB `disabled` → **dtbo `fragment@57` 改成 `ok`** |
| `perst-gpio` / `wake-gpio` | `<0x16b 0x21>` → GPIO **33**；`<0x16b 0x51>` → GPIO **81** |
| `interrupts` | SPI **140 / 149 / 150 / 151 / 152**（`int_global_int` + int_a..d） |
| MSI | 独立控制器 `qcom,pcie0_msi@0x17110040`；`msi-map` → `0x1a9 / 0x1400` |
| `qcom,smmu-sid-base` | `0x1400` |
| `qcom,boot-option` | `1`（bootloader 已枚举） |
| `qcom,pcie-phy-ver` | `0x6b` |
| `pcie_sm` 参数 | `pcie-sm-branch-offset 0x1000`、`start-offset 0x1090`、`pcie-sm-seq` 13 words（bootargs 里 `pci-msm-drv.pcie_sm_regs=0x1D07000,0x1040,0x1048,0x3000,0x1` 与之对应） |

`fragment@61` → `cnss_pci0`（挂在 `pcie0_rp` 下）：

- `reg = <0 0 0 0 0>`（bus0/dev0/func0）；`memory-region = &cnss_wlan_mem`。
- `qcom,iommu-geometry = <0x98000000 0x18010000>`、`iommu-dma-addr-pool = <0x98000000 0x18000000>`（**384 MiB DMA 窗口**）、`iommu-msi-size = 0x1000`、`iommu-pagetable = "coherent"`、`iommu-faults = "stall-disable", "HUPCF", "non-fatal"`。
- `fragment@54/55/56` 把 `wlan_msa_mem` / `wpss_mem` / `wpss_pas` 置 `disabled` —— 本机走 **PCIe 直挂**，不是传统的 MSA / WPSS 远程加载路径。

### 2.3 固件路径线索

主 DTB `/chosen bootargs:18` 逐字含：

```
firmware_class.path=/vendor/firmware_mnt/image,...,/vendor/firmware,/vendor/firmware/wlan/qca_cld/kiwi_v2
```

即 Android 侧用 **qcacld 闭源驱动 + `kiwi_v2` 固件目录**。本机 `fw/android-harvest/firmware/` 只有 GPU 的 `gen71100_*`，root sparse image 里也搜不到 `amss.bin` / `board-2.bin` / `ath12k` / `kiwi_v2` 字符串 ⇒ **WCN7850 固件目前完全没拿到**（与 `board.toml:461-464` 记的触摸固件「`/vendor/firmware` 无 unprivileged 路径可读」是同一类障碍）。

---

## §3 sheng 有什么（`origin/main = cd73e23`）

`/tmp/sheng-cd73e23/porting.toml:443-480`，`id="network" status="implemented"`，`components = ["Ath12kWifi"]`。

**代码规模**（仅板级部分，不含 `drivers/Ath12kWifi` 里从 Linux 移植的 ath12k/mac80211/cfg80211/MHI/QMI/QRTR 主体）：

| 文件 | 行数 | 职责 |
| --- | --- | --- |
| `package/WifiProbe.cpp` | **1772** | 上电、PCIe0 link train、QMP PHY 表、PARF、PERST#、LTSSM、MSI、固件分段 DMA、MHI SBL 启动 |
| `package/WifiNet.cpp` | **1037** | `en0` / `Apple80211`、系统 WiFi 请求、user client 授权 |
| `package/MhiBoot.c/.h` | 151+ | MHI BHI/BHIE 固件下载 |
| `package/WifiFirmware.c/.h` | 45+16 | 从 Data addon VFS 读 `amss.bin`/`m3.bin`/`board-2.bin`/`join.txt` |
| `package/WifiPcieTables.h` | 37 | QMP PHY 序列表 |
| `scripts/*wifi*`、`*mhi*` | 13 个 | host 检查、固件打包、supplicant 构建、association 验证 |
| `firmware/wcn7850/manifest.json` | — | hash-pinned 三件套（二进制留在本地，仓库只有 manifest） |

**固件**（`manifest.json` 逐字，`[代码]`）：

| 文件 | SHA-256 | 字节 |
| --- | --- | --- |
| `amss.bin` | `43aadfd3…da68` | 6,090,816 |
| `m3.bin` | `0e72f44d…1055` | 299,660 |
| `board-2.bin` | `6a98237c…00fd` | 2,431,968 |

`provenance = "Matches installed sheng WFW1 firmware, verified by read-only SHA-256 comparison on 2026-11-04."` 加载路径 `/private/var/ngpu/firmware/ath12k/WCN7850/hw2.0/`。

**启动参数与 ADT**：`sheng-wifi=5` 才启动模块，`sheng-wifi-aj=0/1/2` 控制 join；`dts/sheng.dts:11-15` 导出 `/arm-io/wlan`（`compatible = "qcom,wcn7850"`、`unixnu,adt-name = "wlan"`），用来满足 27 线 `com.apple.wifid` 的 `_LimitLoadToDeviceTree` 检查。用户态还有签名的 `wpa_supplicant 2.12`（`/usr/libexec/unixnu-wpa_supplicant`）。

**真机结果**（`UniXNU/docs/drivers/ath12k-wifi.md:47-62`、`porting.toml` evidence，`[实测]`）：

- PCIe0 达 L0 Gen2 x1，端点 **`17cb:1107`**（`WifiProbe.cpp:538` 硬编码比对 `0x110717cb`）。
- 六路 PMU rail 每路都要 RPMh **mode + voltage + enable** 三种请求，缺 mode 则两路不上电、link 不 train。
- 固件 `c2-00259` 在 demand pager 挂死，`c7-00108` 可用；固件上报 MAC 全零，由 chip ID 生成。
- MSI 走 PCIe0 自己的 MSI receiver → **SPI 141，16 vectors**，空闲时几十次/秒；否则只能 1 ms 轮询。
- WPA2 join + four-way handshake ≈100 ms，DHCP 约 31 s；5 GHz/80 MHz/-73 dBm 下 **12 Mbit/s down / 24 up**，<1% TX loss。
- 系统 `wifid` 接管扫描返回 5–13 个网络；keychain 存不住密码（`-25291`）；吞吐瓶颈疑似 `ifnet_input` 每帧一次拷贝。

---

## §4 关键结论：`kiwi` = WCN7850，硬件同源

这是本轮最值钱的一条，也是**对我最初判断的订正**。看到 `qcom,cnss-kiwi` + `qca_cld/kiwi_v2` 时，第一反应是「这是 ath11k 世代、和 sheng 的 ath12k/WCN7850 不同」。外部资料推翻了它：

- `[外部]` 公开 DT 硬件图谱逐字：`Qualcomm WCN7850 (kiwi, Wi-Fi 7)`，节点同为 `/soc/qcom,cnss-kiwi@b0000000`，`reg 0xb0000000+0x10000`，PCI 端点挂在 `pcie@1c00000/pcie0_rp/cnss_pci0` — **与本机 DT 逐项一致**。
- `[外部]` XDA 对 OnePlus 12（SM8550）的讨论把该路径写成 `cfg80211 → qca_cld3 → WMI → WCN7850 firmware`，并称其为 `kiwi v2`。
- `[代码]` sheng 侧 PCI ID `17cb:1107` 被 Linux 主线 `pwrseq-pcie-m2` 明确对应 `qcom,wcn7850-bt`。

⇒ **`kiwi` 是芯片代号，`kiwi_v2` 是 Android qcacld 固件目录名；两者不表示不同芯片。** 本机与 sheng 用同一颗 WCN7850、同一条 PCIe0 通路，sheng 的驱动架构（ath12k + MHI + 固件分段 DMA）在**原理上适用**。

⚠️ 证据等级边界：本机 DT **没有明写** PCI ID `17cb:1107`，这一条是「同芯片代号 + 同节点布局」的推断，标 `[推测]`，必须靠真机读 PCIe config space 坐实（`WifiProbe.cpp:1657` 的 `(config.va[0] & 0xffff) == 0x17cb` 就是现成的只读探针）。

> **2026-11-12 19:2x 补充（`BLUETOOTH-PORT.md §4`，不推翻本结论，但补一条必须解释的反证）**
> 本机主 DTB 另有 `bluetooth: bt_wcn6750` / `compatible = "qcom,wcn6750-bt"`，而主线 Linux 的
> `qca_bluetooth_of_match` 里 `wcn6750-bt` 与 `wcn7850-bt` 是**两套不同的 soc_data**，不能当别名忽略。
> 三条证据仍指向 WCN7850：① Pad 7 Pro / SM8635 整机规格是 **Wi-Fi 7 + BT 5.4**，WCN6750 是 Wi-Fi 6E + BT 5.2；
> ② dtbo `fragment@55/56` 禁用 `wpss_mem`/`wpss_pas`（WCN6750 必须靠 WPSS Q6/remoteproc 启动），改走 PCIe0 直挂；
> ③ BT 节点的三条 GPIO（35/80/156）与 `cnss-kiwi` 节点完全相同，是同一颗 combo 芯片的两个驱动视图。
> ⇒ `qcom,wcn6750-bt` 判为 Android 下游 BT 供电节点的**遗留 compatible**。本 §结论成立，但应读作
> 「高置信 + 一条已解释的反证」，不是「DT 直接确认」；终局证据要靠 HCI vendor command 读 chip version
> 或 PCIe config space。详见 `BLUETOOTH-PORT.md §4/§9`。

---

## §5 不能直接照搬的四组差异

| 维度 | sheng（SM8550 / iPadOS 27） | muyu（SM8635 cliffs / iPadOS 17） | 后果 |
| --- | --- | --- | --- |
| **GPIO** | WLAN_EN 80 / BT_EN 81 / SW_CTRL 82 / PERST# 94 / WAKE# 96 | WLAN_EN **35** / BT_EN **156** / SW_CTRL **80** / wlan-sw-ctrl 34 / PERST# **33** / WAKE# **81** | 照抄会拉错引脚；`board.toml` 记的 stage 31 盲写教训就在这类地方 |
| **PMU** | 六路 rail + RPMh mode/voltage/enable；PHY L1E 0.88 V / L3E 1.2 V 与 UFS 共用 | **8 组 `pmu_vreg_map` + 8 路 `vdd-*-supply`**，另有 11 条 `pdc_init_table` 和 `vreg_pdc_map` |  rail 数量、命名、电压表全要重新取证 |
| **GCC / PCIe 寄存器** | `qcom,pcie-sm8550`；GDSC `0x6b004` bit31；clock votes `0x52008`（bits 0..6）/`0x52000`（12/19/22）；QMP 表在 `WifiPcieTables.h` | `qcom,pci-msm`；`pcie_sm@0x1d07000` + `pcie-sm-seq` 13 words；`qcom,cliffs-pcie-pdc` / `qcom,pcie-crm`；`pcie-phy-ver 0x6b` | 🔴 **`0x52008` bit26 在本机是 GENI SE4 的 S4 时钟分支**（`board.toml:420-421` 逐字：`the S4 branch is OFF at BDS (0x52008 bit 26 clear ...)`，投 bit26 才打开）——同一寄存器位定义不同，vote 位图绝不能照抄 |
| **固件 / 用户态** | ath12k `amss.bin`/`m3.bin`/`board-2.bin` + `wpa_supplicant 2.12` + `/arm-io/wlan` + 27 线 `wifid` | Android `qca_cld/kiwi_v2`；**仓库里没有任何 WCN7850 固件、无 ADT wlan 节点、17 线 `wifid` 行为未查证** | `board-2.bin` 是板级校准数据，必须用本机自己的；OS ABI/`_LimitLoadToDeviceTree` 条件在 17 线未验证 |

另外 MSI 也不同：sheng 是 SPI 141 / 16 vectors，本机是 `qcom,pcie0_msi@0x17110040` + PCIe0 的 SPI 140/149–152；DMA 窗口本机 384 MiB，sheng 128 MiB。

---

## §6 决定性的前置 blocker：launchd 还没起来

`GPU-HANDOFF.md §52`（接手 GPU 的 AI 于 18:02–18:06 追加）屏照逐字：

```
MUYU LIVE p=24 t=260 ...
MUYU UP lg=10/1/1/1 s=1/0 op=0/0 mf=0/0 i=48 r=-
PROC n=1 zomb=0 launchd=0- backboardd=0- SpringBoard=0- liveact=0-
```

- `t=260` 说明不是「拍得早」；root mount 已成功，ticker 调用点在 `load_init_program()` 入口，断点在 **owner-cache / Data mount 子阶段或 launchd exec**（§53 已预建 `MUYU ROOT` 只读诊断镜像，未刷）。
- sheng 能验 WiFi，是因为它的用户态完整：`SpringBoard PID 30`、`wifid PID 49`、`backboardd PID 61`（`porting.toml:610`）。
- 17 线 `PROC n=1` ⇒ `launchd`、`wifid`、`airportd`、`wpa_supplicant`、`configd` **一个都不存在**。就算把 `en0` 和 `IO80211` 对象发布出来，也没有消费者；Settings/WiFi 开关、扫描、DHCP 全部无从谈起。

⇒ **WiFi 是「启动链路通了以后」的收益，不是打通启动链路的手段**。它和上一轮 `POWER-MANAGEMENT.md` 里 `IOPMPowerSource` / `thermal-level` 的处境完全相同，都卡在同一道关卡 1。

---

## §7 17 线已有的、能复用的基础

| 资产 | 状态 | 用途 |
| --- | --- | --- |
| `package/MuyuRpmhRsc.c/.h`（16.5 KB + 7.5 KB） | `[实测]` host tests 通过；刷过但**真机 panic readout 未采集**（`porting.toml:209`） | PMU rail 的 RPMh mode/voltage/enable 请求编码，是 WiFi 上电的必备前置 |
| `package/BootResetCore.h` | `[代码]` SPMI / PMIC arbiter v7 / APID 遍历 / `br_read8()` | 只读扫描 PMU / GPIO 状态，不写寄存器 |
| `package/BravearkLastLog.c` 5 s ticker + `MUYU` 行 | `[实测]` 稳定工作 | 照 `WifiProbe` 的 `native_wifi_cfg[]` 思路加只读里程碑行，屏照判读零成本 |
| sheng `WifiProbe.cpp` 的 **stage 1**（`wifi=1`） | `[代码]` 逐字：「read-only look at what the boot loaders left of the WCN7850 path」 | 最值得先搬的是这个**只读探针范式**，不是上电代码 |
| sheng 的 13 个 `check-wifi-*` / `check-mhi-*` host 脚本 | `[代码]` | host 侧先验证编码，符合项目「先 host 后真机」的规矩 |

**uke 侧**：8 个 CI 提交只产出签名的 `wpa_supplicant` Mach-O + CDHash，可以作为将来用户态打包的参考，但依赖 iOS SDK、且是 27 线目标，不能直接用于 17 线。

---

## §8 建议路线（按依赖排序，不抢 GPU 主线窗口）

| 优先级 | 动作 | 需要设备 | 验收 / 产出 |
| --- | --- | --- | --- |
| **P0** | 等 GPU 主线把 `load_init_program()` → launchd 的断点定位并修好（§52/§53） | 是（主线占用） | `PROC n>1`、`launchd=<pid>`。**在此之前不投入任何 WiFi 用户态工作** |
| **P0′（可并行，离线）** | ① 从本机 vendor / root 提取 `kiwi_v2` 固件并算 SHA；② 把 §2 的 GPIO / PMU / PCIe / MSI 事实固化成一张板级表；③ 核对 sheng 三件套与本机的兼容性 | 否 | 一份带 hash 的 manifest + 差异表；**充电期间就能做** |
| **P1** | 最小**只读**探针：仿 stage 1，打印 PCIe0 GDSC / 时钟分支 / LTSSM / config space vendor:device / 端点 ID | 是 | 屏照上看到 `17cb:1107` 才坐实 §4 的 `[推测]`；**不写任何寄存器** |
| **P2** | PMU rail 上电 + PCIe0 link train（复用 `MuyuRpmhRsc`，位图重新取证） | 是 | link 到 L0、能稳定读 config；失败即回 P1 只读 |
| **P3** | MHI BHI/BHIE 固件下载 + SMMU stream + MSI 路由 | 是 | 固件起来并回 HFI；此步才用得上 sheng 的 `MhiBoot.c` |
| **P4** | `en0` / `Apple80211` / ADT `/arm-io/wlan` / 17 线 `wifid` 与 supplicant | 是 | 扫描 → WPA2 join → DHCP；必须在 P0 完成后 |

**价值判断**：一旦通网，日志可以走网络导出，不必再靠屏照 OCR——这个月至少 4 次返工（方向误判、EXIF 裁切、`%.40s`、`0x0908`）都源自屏照判读。所以 P4 对主线是**杠杆**，但它是结果不是路径。

---

## §9 红线

1. 🔴 **别照抄 SM8550 的 GCC vote 位图 / GDSC / QMP 表**：本机 `0x52008` bit26 是 GENI SE4 的 S4 时钟分支，位定义不同；`board.toml` 记着 stage 31 盲写 PMIC GPIO 的教训。
2. 🔴 **别用 sheng 的 `board-2.bin`**：它是板级校准（`provenance` 写明「installed sheng WFW1 firmware」），MAC / 功率表 / 监管域都绑定 sheng 板。
3. 🔴 **别在 `PROC n=1` 的阶段投用户态**：`wifid` / supplicant / keychain 全都依赖 launchd，先做等于空转。
4. 🔴 **别抢刷机窗口**：设备现在 `vendor_boot=原厂` 在 rec 充电，GPU 主线另有调度；所有真机动作要等主线明确让出设备。
5. **别把「同芯片」当成「同驱动」**：Android 走 CNSS2 + qcacld，sheng 走主线 ath12k；协议栈、固件命名、加载时序都不同，只有芯片与 PCIe 通路共用。
6. **PCI ID `17cb:1107` 在本机仍是 `[推测]`**，P1 只读探针验证前不要写进任何配置。

---

## §10 未验证项清单

| 项 | 当前等级 | 怎么验 |
| --- | --- | --- |
| 本机端点确为 `17cb:1107` | `[推测]` | P1 只读 PCIe config space |
| `kiwi_v2` 固件能否喂 sheng 的 ath12k 加载器 | `[推测]` | P0′ 拿到固件后比对格式 / BHI 头 |
| 本机 `pcie_sm` 序列与 sheng QMP 表的对应关系 | `[推测]` | DT 序列表 + 真机只读寄存器回读 |
| 17 线 `com.apple.wifid` 是否也有 `_LimitLoadToDeviceTree` 条件 | **未查证** | iPadOS 17 launchd plist / 27 线做法仅为参照 |
| 17 线用户态 `IO80211` / `Apple80211` ABI 可用 | **未查证** | P0 完成后才能验 |
| `MuyuRpmhRsc` 真机是否真能发出有效 RPMh 请求 | `porting.toml:209` 明写「on-device panic readout 未采集」 | 需一次真机只读取证 |
| 充电 / 热对 WiFi 上电的影响 | **未评估** | `POWER-MANAGEMENT.md`：本机无 DVFS、无节流，平台温度轻载已 59 °C |

---

*本轮纯只读：未刷机、未碰设备、未构建、未改任何源码 / 镜像 / 板级配置 / GPU 工作树。*
*`logs/flash-ledger.tsv` 末条仍为 `2026-11-12T10:51:01Z flash vendor_boot fw/stock/vendor_boot.img rc=0`。*
*相关文档：`DISPLAY-LOAD-BACKLIGHT.md` §7（背光同为「以为上游没有、实际同芯片」的同类订正）、`POWER-MANAGEMENT.md`（PMU / RPMh 基础）、`GPU-HANDOFF.md` §52/§53（当前 blocker）、`UniXNU/docs/drivers/ath12k-wifi.md`（sheng 组件说明）。*
