# 电源管理 · 现状盘点与 sheng 对照

**2026-11-12 18:4x 北京｜17 线 / 21H461 / SM8635 cliffs（Xiaomi Pad 7 Pro）｜纯只读调研**

> 范围：XNU 侧电源管理现状、充电/电池链路、热与 DVFS、休眠唤醒，以及与 `UniXNU-sheng`（`cd73e23`）的对照。
> 本轮**未刷机、未碰设备、未构建、未改任何源码 / 镜像 / 板级配置**，与已移交的 GPU 主线**无文件交叉**。
>
> 证据等级：`[代码]` 源码静态可复核｜`[实测]` 文档记录的真机测量｜`[DT]` 本机 stock DTB/DTBO 反编译取证｜`[推测]` 未验证。
> 相关报告：背光与显示负荷见 `DISPLAY-LOAD-BACKLIGHT.md`（尤其 §7 的 KTZ8866 路径）；热传感器细节见 `THERMAL-PORT.md`。

---

## 0. 三问三答

| 问题 | 结论 |
| --- | --- |
| 17 线现在有电源管理吗？ | **基本没有**。`THERMAL-PORT.md` 逐字写着「No throttling, no DVFS, no battery/charging driver」。有的只是：一个**只为满足 AMFI 而发布 nub 的 PMGR ABI 资格验证**、一个**默认关闭且从未在本机验证的 CPU 调频探针**、一个**只读的温度 survey**、显示 blank swap。没有节流、没有 governor、没有电池服务、没有系统 suspend/resume。 |
| 为什么必须回 rec 才能充电？ | 充电**不在 XNU 手里**：本机 DT 把 `qcom,battery_charger` 挂在 `qcom,pmic_glink` 下、保护域写着 `msm/adsp/charger_pd`，是 **ADSP firmware 通过 PMIC GLINK 管**。sheng 的实现注释也逐字写着「Charging itself is the DSP firmware's」，XNU 只负责读状态与设 USB 输入电流上限。fastboot 不加载这套栈，rec（原厂 recovery）里有 ⇒ 「刷原厂 → 关机 → 墙充」的规矩短期内**无法被 XNU 电源管理取代**。`[DT]` + `[实测]` + `[推测]`（fastboot 侧未直接取证） |
| 能立刻改善什么？ | **只能在内核侧直接降耗**，不能靠用户态服务：① 熄屏时把背光 level 写 0（最大头，见 `DISPLAY-LOAD-BACKLIGHT.md` §7）；② CPU 频率上限（EPSS 地址已由本机 DT 确认与探针常量一致）；③ 降帧率 / 降像素密度。`powerd` / `thermalmonitord` 当前都没起来（§49 用户态图形栈整体未启动），搬 `IOPMPowerSource` / `mTPL` 这类服务**没有消费者**。 |

一个必须先说清的前提：**「电源管理」在这个 port 里不等于「让 GPU 更省电」**。当前实际是 CPU 拷贝扫描输出路径，GPU 未接管合成；本节所有降耗收益首先作用于 CPU、内存带宽与背光。

---

## 1. 17 线现状盘点 `[代码]`

| 能力 | 实现 / 参数 | 状态 | 证据 |
| --- | --- | --- | --- |
| PMGR | `package/ShengPlatformPower.cpp`（159 行），`sheng-pmgr=2` **已启用** | ⚠️ **不是电源管理**：头部明写 `No clocks, sleep, reset or secondary-CPU success is advertised`；`enableCPUCore` / `initCPUIdle` / `enterCPUIdle` / cluster 开关等 10 处 override 全部 `unavailable()` → `panic()`。发布 nub 只是为了让 AMFI 不报 `No PMGR?` | `board.toml:79-81, 385`；文件 `:2-4, :34-45` |
| CPU DVFS | `package/CpuFreqProbe.cpp`（**74 行**），`sheng-cpufreq=<n>` 默认关，当前镜像**未带该参数** | ⚠️ 只读/写最高非 turbo 的探针；注释逐字 `Nothing here is validated on the device until a boot has shown it`；寄存器布局来自 **SM8550** EPSS | 文件 `:1-9, :21-27` |
| 温度 | `package/MuyuThermalProbe.c`，`muyu-thermal=<seconds>` 默认关 | ✅ 只读 survey 在真机跑通；**never writes a register**（`:18-20`），注释 `:51` 写「left behind for the throttle that will read it later」——节流没做 | `THERMAL-PORT.md` |
| 热策略 / 节流 | — | ❌ 无。`THERMAL-PORT.md:67`：`no thermal management, no DVFS and wdt=-1`；§6 把节流列为待办 | 同上 |
| 电池 / 充电 | — | ❌ **无驱动**。17 线 `package/` 里没有 `ShengBattery.cpp` / `BatteryProbe.cpp` / `Battmgr` / `QcomGlink` / `QcomQrtr` / ADSP 栈；镜像无 `adsp` / `battery` / `thermal` / `cpufreq` / `gpu-dcvs` 参数 | 文件清单 + 镜像字符串 |
| GPU 电源 / DCVS | `sheng-gpu-power=9` **已启用** | 属于 GPU 主线（bring-up / hangcheck），**本报告不碰**；`gpu-dcvs` 未启用 | `board.toml:557-571` |
| 休眠 / 唤醒 | 仅 display blank swap（整屏填 `0xff000000`）+ synthetic Home press 重置 idle timer | ❌ 无系统 suspend/resume；像素黑了但背光 LED 一直通电 | `NativeLegacyDisplay.cpp:592-613`；`board.toml:572`；`DISPLAY-LOAD-BACKLIGHT.md` §4 |
| 看门狗 | `wdt=-1` + `sheng-disable-watchdogd=1` | ⚠️ 热策略若挂起没有兜底复位；`THERMAL-PORT.md` §6.5 把两者列为同一章 | `board.toml:403-407` |

**两个容易误读的名字**：

- `offline-legacy-pm=1`（当前镜像里确实有）是 **legacy display 的 PM**（`NativeLegacyDisplay.cpp:504`），跟系统电源管理无关。
- `ShengPlatformPower.cpp` 名字里有 Platform/Power，实际是 **ABI 资格验证**（`static_assert(sizeof(...) == 0x4b8a8)`、nub 发布/绑定观察），不是电源策略。

### 1.1 当前镜像实际生效的电源类参数 `[代码]`

从 `fw/vendor_boot_gpu_upper1.img` 二进制直搜，与电源相关的只有：

```
sheng-pmgr=2              # PMGR ABI 资格验证
sheng-gpu-power=9         # GPU bring-up（GPU 主线）
sheng-disable-watchdogd=1 # 关 watchdogd
wdt=-1                    # 关 Apple ARM watchdog timer
offline-legacy-pm=1       # display PM，非系统电源
```

**没有** `sheng-cpufreq`、`muyu-thermal`、`sheng-adsp`、`battery`、`gpu-dcvs`、`usb-sdp-ma`。
（镜像里能搜到 `cpufreq` / `muyu-thermal` 字符串，是编译进去的代码常量，不是启动参数。）

---

## 2. 真机实测：热与电 `[实测]`

### 2.1 温度（`THERMAL-PORT.md`，2026-11-02）

```
MUYU THERM L=29 B=27 G=25 D=26 hot=29 en=111 inv=0     # 冷启动一次性
trend h0=33 h15=55 pk=55                                # muyu-thermal=60
trend h0=33 h15=55 h30=56 h60=59 pk=59 (still 59 …)     # muyu-thermal=180
```

- TSENS 三控制器都被固件留在 enabled（`en=111`）、所有 sensor 有 VALID（`inv=0`）、v2 直接是 deciCelsius ⇒ **寄存器可读、映射正确**。
- 15 秒涨 22 °C（约 1.5 °C/s），60 秒后**平在 59 °C**，远低于 DT 的紧急阶梯（95 °C GPU `tj_cfg` / 110 °C `cpu7-emerg2` / 115 °C `reset-mon`）。
- 文档自己的判断：`the risk is not the UI load; it is a heavier one (GPU, decode, a saturated core)` —— 当前 GPU 主线没真正出负载，所以 59 °C 是「轻载平台」，**不是安全证明**。
- `THERMAL-PORT.md` §6 的待办顺序：① 跑 runbook → ② 1 s 周期采样 + 分钟峰值 → ③ 节流（CPU 频率上限 → idle 注入）→ ④ 紧急断电 → ⑤ watchdog。

### 2.2 电池 / 实验窗口 `[实测]`（`GPU-HANDOFF.md` §40–§51、`HANDOFF-COLD-START.md`）

| 时间（北京） | 电压 | 事件 |
| --- | --- | --- |
| 12:14 | 3420 mV | §40 `restart1` |
| 13:0x | 3749 mV | §46 刷原厂前 |
| 13:18 / 13:43 | 3861 / 3883 mV | §45 / 13:48 屏照 |
| 16:24 | **3668 mV** | §50 刷原厂（脚本告警 `< 3700`） |
| 18:0x | 3924 mV | `upper1` 刷前预检 |

`HANDOFF-COLD-START.md` §2.1 固化了操作规矩：**长按电源 10–15 s 关机 → 墙充 5V/2A+ 1–2 h**；fastboot 本身不充电；**电脑 USB 500 mA 会进「充电图标↔尝试启动」死循环**。
即：每轮实验窗口被供电硬约束，3883 → 3668 mV 约 2.6 h，之后必须让出设备 1–2 h。

> ⚠️ 设备状态：本轮只读查到 `logs/flash-ledger.tsv` 末条是 **`2026-11-12T10:32:11Z`（18:32 北京）`fw/vendor_boot_gpu_root1.img` flash + UEFI `boot rc=0`**，由接手 GPU 的 AI 执行；`fastboot devices` 为空。本报告**不碰设备、不抢窗口**。

---

## 3. 充电链路的硬件取证 `[DT]`

主 `fw/dtb_04.dtb` 反编译（`/tmp/muyu_dtb04.dts`）：

```
qcom,pmic_glink …
    battery_charger: qcom,battery_charger { compatible = "qcom,battery-charger"; }
    charger_partition { compatible = "xiaomi,charger_partition"; }
    qcom,battery_debug / qcom,charger-ulog-glink
    qcom,protection-domain = "tms/servreg", "msm/adsp/charger_pd"
spmi_bus = /soc/qcom,spmi@c42d000 ; spmi1_bus = /soc/qcom,spmi@c432000
```

dtbo overlay（`/tmp/muyu_dtbo.dts`）：`smb1510_present@5d`、`pm7550ba_charger_therm`、`pmxr2230_bcl: bcl@4700 { compatible = "qcom,bcl-v5" }` 及 BCL lvl0/1/2 三级 cooling maps、`charger_therm0` / `display_therm` 等热区。

⇒ 本机充电是 **PMIC GLINK → ADSP charger PD** 的 firmware 服务，PMIC 侧还有 BCL（Battery Current Limit）硬件限流与充电/背光/显示热区。XNU 侧既没有 GLINK/QRTR/SMP2P/ADSP 栈，也没有发布 `IOPMPowerSource`。
这与「刷原厂 vendor_boot 才能进 rec 充电」一致：rec 里是原厂 Android 的 charger 服务在管，fastboot 不加载 ADSP charger PD。

**能改善的不是「让 fastboot 充电」，而是**：XNU 运行时少耗电（延长窗口），以及长期把 ADSP 栈带起来后能读到电量 / 设输入限流（sheng 的 `usb-sdp-ma` 默认 900 mA、`0` 保留固件限制，对应上文「电脑 USB 死循环」的限流问题）。

### 3.1 CPU 调频域：一个有利的意外 `[DT]` + `[代码]`

本机 DT 的 cpufreq 节点居然**与探针的 SM8550 常量完全一致**：

```
cpufreq_hw: qcom,cpufreq-hw {
    compatible = "qcom,cpufreq-epss";
    reg = <0x17d91000 0x1000 0x17d92000 0x1000 0x17d93000 0x1000>;
    reg-names = "freq-domain0", "freq-domain1", "freq-domain2";
    interrupts = <0 0x1e 4  0 0x1f 4  0 0x13 4>;   // dcvsh0/1/2_int
    #freq-domain-cells = <1>;
};
```

`CpuFreqProbe.cpp` 的 `kDomainBase[3] = {0x17d91000, 0x17d92000, 0x17d93000}`、`kDomainSize = 0x1000` **逐字匹配** ⇒ 三域窗口在 SM8635 cliffs 上无需改地址。
**仍需验证**：LUT 行格式（source / core count / L value）、`reg_perf_state 0x320` 写入是否生效、各域 CPU 数（sheng 是 3/4/1）、以及 sheng 独有的 L3 节点 `interconnect@17d90000`（本机 DT 未确认）。探针注释「未在真机验证」仍然成立。

---

## 4. sheng 对照（`cd73e23`）`[代码]` + `[实测]`

`porting.toml` 有 `[[area]] id = "power" status = "implemented"`，电源组件共 1445 行：

| 文件 | 行数 | 作用 |
| --- | --- | --- |
| `ShengBattery.cpp` | 312 | `IOPMPowerSource`，从 ADSP 的 battery manager 经 PMIC GLINK 取值；`powerd` 读容量 / 充电状态 / 电压电流 / AdapterDetails；充电本身是 DSP firmware 做的，唯一写入是 USB 输入电流上限 |
| `BatteryProbe.cpp` | 26 | `battery=1` 启动入口 |
| `CpuFreqProbe.cpp` | **384** | 相对 17 线 74 行裁剪版，多了 **mode 4 governor**（16 ms、busy share、1.25× schedutil margin、90% hispeed）、L3 vote、`cpufreq-cap=<MHz>`、`cpufreq-min`；diff 414 行 |
| `ThermalProbe.cpp` / `ThermalLevel.cpp` | 250 / 232 | `thermal=1` + `thermal-level=1` 发布 `mTPL`，`thermalmonitord` 每秒读一次；按 DT 的 CPU/GPU trip 点带迟滞算压力级 |
| `ShengPlatformPower.cpp` | 159 | **与 17 线同源**：同样 `No clocks, sleep, reset…`、同样 10 处 `unavailable()` ⇒ sheng 的 PMGR 也**不是**真电源管理，它的电源能力在上述独立组件里 |

启用的旋钮（`board.toml` / `porting.toml` power area）：`pmgr=2`、`adsp=2`（`battery=0` 关）、`usb-sdp-ma`（默认 900）、`cpufreq=4`（`cpufreq=3` 固定最高非 turbo）、`gpu-dcvs=1`（`gpu-dcvs-min` 默认 3，`gpu-perf` 固定）、`vsync=120`、`thermal-level=1`。

**真机数据（porting.toml evidence，这是最值钱的部分）**：

- 充电：PPS 适配器 **8.72 V / 1.3 A 输入、电池 +1.99 A、3 分钟涨 1–3 %**；Mac PD **5 V / 1.1 A（1.2 A 限）、电池 +0.25~0.4 A**；温度 31.4–32.9 °C；full 9353 mAh / design 9800 mAh / 172 cycles。
- 调频：`cpufreq=4 gpu-dcvs=1` 下 CPU domain1 **499–2803 MHz**、domain2 **595–2956 MHz**，idle 均值约 1.2 GHz / 620 MHz；GPU **61 % 时间在 680 MHz、mean busy 7 %**，暂停后回 governor floor 348 MHz、唤醒时 boost。
- host：`PASS battery manager: 4074 checks`。

**sheng 自己列的 limits**（不要把「implemented」读成「完整」）：

- 适配器细节一律当 computer port（来自 USB property 6）；**无 fuel gauge / adapter 认证 ⇒ 充电电流卡在 2 A**；PPS 电荷泵与小米适配器认证还没做。
- **低电量关机会 panic**（`vnode_rele_ext` during unmount）。
- `Per-peripheral controls and battery publication have separate coverage from system suspend.` ⇒ **系统 suspend 也未完整覆盖**。
- GPU governor 每秒改几次频率，对帧时的影响未测量；completion 里 `each claimed sleep/resume transition restores its dependent peripherals` 仍是验收项而非已完成项。

---

## 5. 为什么现在不能整套搬 `[推测]` 有代码依据

1. **用户态没起来**（`GPU-HANDOFF.md` §49：内核 GPU bring-up 全绿但用户态图形栈整体未启动、`calls=0`）。
   `ShengBattery.cpp` 的消费者是 `powerd`，`ThermalLevel.cpp` 的消费者是 `thermalmonitord`，`IOPMPowerSource` / `mTPL` 都靠这些 daemon 读取 ⇒ 搬过来**没有消费者**，等于白做。
2. **依赖栈缺失**：17 线 `package/` 里没有 ADSP firmware 加载、GLINK / QRTR / SMP2P / SMEM / `ShengBattmgr.c`，也没有 `adsp=2` 参数。`ShengBattery.cpp` 头部明确 `needs adsp=2`。
3. **版本差异**：sheng 是 iPadOS 27 / macOS 27（21H461 是 iPadOS 17），`IOPMPowerSource` / `AppleSMC` 系列服务、`thermalmonitord` 的 sandbox entitlement、ADT / AMFI 行为都不同；sheng 自己也有一串 27 线专属踩坑记录。
4. **SoC 差异**：sheng 是 SM8550，17 线是 SM8635 cliffs。EPSS 域地址碰巧一致（§3.1），但 L3 节点、CPU 域映射、PMIC SID / charger 型号、GCC vote 位图都不同 —— 与背光驱动那个结论同构：**协议可复用，常量必须重新取证**。

---

## 6. 建议顺序（按「当前用户态未起」的现实排序）

| 优先级 | 动作 | 依赖设备 | 与 GPU 主线冲突 | 说明 |
| --- | --- | --- | --- | --- |
| **P0-1** | 熄屏写 KTZ8866 `level=0`（`DISPLAY-LOAD-BACKLIGHT.md` §7 方案 ①） | 是（但可先离线准备） | 低（显示/背光，非 GPU） | 同时解决「熄屏发光」与最大一块常驻耗电；只搬 `GeniI2c.c/.h` + 小驱动，不依赖用户态 |
| **P0-2** | `sheng-cpufreq=1` **只读取证**（三域 enable / current / requested / 最高 LUT）+ 1 s 温度采样 | 是 | 低 | 本机 EPSS 地址已确认；先验「读得到」再谈写；把 `CPUFREQ` 行与 `MUYU THERM` 行一起打屏 |
| **P0-3** | 低风险频率上限：写更低 LUT index，`domain_state` 读回验证；同步观察 15 s / 60 s 温度斜率 | 是 | 中（会影响 GPU 主线计时） | `THERMAL-PORT.md` §6.3 的首选执行器；**需与接手 GPU 的 AI 约定窗口**，别在它刷机时抢设备 |
| **P0-4** | 降帧率 / 降像素密度（`DISPLAY-LOAD-BACKLIGHT.md` §3 A/B） | 是 | 中 | 少渲像素 = 少 CPU 拷贝 = 少发热；低 Hz 早退坑仍要自己验 |
| **P1** | 等用户态起来后再评估 `thermal-level=1` / `battery=1` 服务 | 是 | 高 | 有 `powerd` / `thermalmonitord` 才有意义 |
| **P2** | ADSP / PMIC GLINK 栈 + battery manager | 是 | 高 | 工作量最大；收益是系统内电量读数与输入限流，**不能让 fastboot 充电** |
| **P3** | 系统 suspend/resume、外设电源门控 | 是 | 高 | sheng 自己都列为未完整覆盖；不要先做 |

🔴 **红线**：

- 不要照抄 sheng 的 L3 节点 `17d90000`、CPU 域 3/4/1 映射或 `kVoteBits` 类常量而不核对本机 DT（背光那边已有同型教训）。
- 不要盲写 PMIC / charger / GPIO 寄存器 —— `board.toml:449-451` stage 31 逐个拉高 PMIC GPIO 的盲写实验是前车之鉴；先只读。
- 不要为了「加保护」顺手改 `wdt=-1` / `disable-watchdogd`：热策略与 watchdog 属同一章（`THERMAL-PORT.md` §6.5），单独改一个会让兜底逻辑不一致。
- 不要碰 GPU 主线的 `sheng-gpu-power=9` / `GpuPowerProbe.cpp` / 刷机窗口；本轮起，设备归另一个 AI 调度。

---

## 7. 未验证项与证据边界

- `[推测]` 「fastboot 不充电是因为不加载 ADSP charger PD」：DT 与 sheng 注释支持该模型，但**没有对 fastboot 侧充电固件做过直接取证**；rec 能充电是文档记录的实测事实。
- `[推测]` 本机 KTZ8866 / EPSS LUT / L3 与 sheng 的寄存器语义一致：compatible 与域地址已核对，但**从未在本机读回**（`CpuFreqProbe.cpp` 自带同样的免责声明）。
- `[实测→外推]` 59 °C 平台来自 boot + 1.4 fps compositor 轻载；GPU 真正接管后的热负载**完全没有数据**，DT 的 95/110/115 °C 阶梯也未在 XNU 侧实现任何动作。
- `[推测]` 「降 CPU 频率 / 降帧率能显著延长实验窗口」是线性外推；sheng 的 `cpufreq=4` 数据来自另一块板、另一套系统，不能直接套到 17 线。
- 本报告所有行号基于 2026-11-12 18:4x 的工作树；接手 GPU 的 AI 可能同时改 `board.toml` / `package/*`，复用前请重新核对。
- sheng 快照：`/tmp/sheng-cd73e23`（`cd73e23`）与 `/tmp/sheng-latest`（`e893717`）；DT 反编译产物 `/tmp/muyu_dtb04.dts`、`/tmp/muyu_dtbo.dts` 均为临时文件，需要长期保留请自行归档。

---

*本轮纯只读：未刷机、未碰设备、未构建、未改任何源码 / 镜像 / 板级配置 / GPU 工作树。
只读查过一次 `fastboot devices`（空）与 `logs/flash-ledger.tsv`（末条 `10:32:11Z` 由 GPU 主线刷入 `root1`）。*
