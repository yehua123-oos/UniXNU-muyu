# CPU 调度现状调研（17 线 / 21H461 / SM8635 cliffs）

**2026-11-12 19:3x 北京｜纯只读调研｜不碰设备、不碰 GPU 工作树、不构建、不改源码**

> 范围：用户问「CPU 的调度是不是也不正常」。本文给出分层结论、代码/DT/镜像取证、
> sheng 对照、以及**不新增刷机轮次**的取证建议。
> 全篇标证据等级：`[配置]` 镜像/板级实值 · `[DT]` stock DTB 反编译 · `[代码]` 源码行号 ·
> `[实测]` 真机记录（**均为其他板/其他系统**）· `[推测]` 未在本机取证。
> 关联：`POWER-MANAGEMENT.md`（电源/热/DVFS）、`GPU-HANDOFF.md §55`（当前卡点）、
> `UKE-CODE-AUDIT.md §4`（SMP 历史）、`IPADOS27-MIGRATION.md §6.31`（ADT 校验）。

---

## 0. 三十秒结论

| 问题 | 答案 |
| --- | --- |
| CPU 调度也不正常吧？ | **分层看：结构性缺失确实存在，但不是今天卡住的原因，而且现在根本观察不到「调度」。** `[代码][配置]` |
| 当前卡点在哪？ | §55：`MUYU INIT q=0` ⇒ `load_init_program()` **从未被调用**；`PROC n=1 zomb=0 launchd=0`。断点已前移进 `bsd_init()` 内约 270 行的 rootauth / owner-root / `set_rootvnode` / `devfs_kernel_mount` 一带。**进程表里只有一个进程，调度器没有负载可分配。** `[实测]` |
| 最常见的误读 | `porting.toml` 的 `id="cpu"` 写着「单核跑」——那是 **27 线 spike / u150–u159 的历史**（`sheng-smp=0 armcpu-spi-ipi=0`）。17 线当前镜像是 **`sheng-smp=1 armcpu-spi-ipi=1`**，SMP 是开的。别把旧记录当现状。 `[配置]` |
| 真正缺的是什么？ | ① ADT 无 `unixnu,cluster-type` ⇒ 8 核同一 processor set；② `armcpu-edge` 缺省 0 ⇒ Edge 矩阵全禁；③ 无 CPU idle / cluster 电源控制（10 处 override 全 panic）；④ 无 DVFS governor；⑤ 逐核状态**没上屏**。 `[代码][配置]` |
| 现在该做什么？ | **不要修调度。** 把逐核 SMP 行接进主线下一版只读诊断镜像，与 §55 的 `bsd1` 阶段梯合并成一次刷机；顺带回答「secondary core 到底起没起全」——这一点目前**没有任何证据**。 `[推测]` |
| 优先级 | 调度、AMP、governor 全部排在 launchd 之后；用户态没起来之前搬这些**没有消费者**（与 `POWER-MANAGEMENT.md` 的 `IOPMPowerSource`、WiFi 的 `wifid` 同卡一道关）。 |

一句话给用户：**你的直觉指向的东西确实缺，但那不是今天卡住的地方。** 今天卡在 `bsd_init()`，
比调度器更早；等 launchd 起来，这些缺失才会变成真正的性能问题（E/P 不迁移、无调频、无 idle 门控）。

---

## 1. 现状取证

### 1.1 当前镜像实值 `[配置]`

对 `fw/vendor_boot_gpu_upper1.img` 做二进制直搜（非源码注释）：

```
armcpu-spi-ipi=1     sheng-smp=1     sheng-pmgr=2     wdt=-1
sheng-disable-watchdogd=1
```

**没有**：`armcpu-edge`、`armcpu-self-ipi`、`armcpu-diagnostics`、`armcpu-idle-latency-us`、
`sheng-cpufreq`、`timebase-user`、`thermal`、`gpu-dcvs`、`board-clock`。
`board.toml:403-408, 578` 与镜像一致。

⇒ SMP 参数开着；异构调度、逐核诊断、调频、热策略**一个都没启用**。

### 1.2 「单核」这个旧结论的适用范围 `[配置][代码]`

`porting.toml:60-75`：

```
id = "cpu"  status = "implemented"  components = ["ArmCpu"]
paths = ["board.spike27-live.toml"]
inputs = ["Boot args sheng-smp=0 armcpu-spi-ipi=0 (present in every u150-u159 image)."]
configuration = ["Single-CPU runs; ... not executed on muyu."]
limits = ["No multi-core run on the 27 line; no SMP stability data on this SoC for 24A446."]
```

`UKE-CODE-AUDIT.md §4 / §6` 给了 uke 的原文：单核是 **bisect 回退方案**，不是目标；
uke 早期 **SMP 在 t≈117 s 死**，靠 **SPI-IPI 路径 + watchdog 策略**修好，最终验收是
**8 核全起 `sheng-smp=1 armcpu-spi-ipi=1`**。

⇒ 这份 `porting.toml` 记录的是 27 线 spike 的一段历史，不是 17 线当前配置。
**结论：17 线当前 SMP 开着，但「8 个核是否全部成功 bring-up」没有任何真机证据**（§1.5）。

### 1.3 17 线 ADT：8 核、无 E/P 标记 `[代码]`

`UniXNU-muyu-17/dts/muyu.dts:26-41, 80`：

```
// Eight Kryo cores, Affinity1 numbering 0x000..0x700. One compatibility string for every core:
// AppleARMCPU rejects a tree whose CPU nodes do not share it ...
cpu0..cpu7: all "arm,cortex-a720", reg 0x000 / 0x100 / ... / 0x700
...
// The E/P split is not expressed here: the 17 line's working ADT has no cluster-type on its CPU
// nodes either, and AppleARMCPU validates the tree before that hint matters.
```

`IPADOS27-MIGRATION.md §6.31` 记着当年踩坑：给 6 个核写 `cortex-a520`、3 个 `cortex-a720`、
1 个 `cortex-x4` → panic `CPU configuration in device tree is invalid @AppleARMCPU.cpp:111`；
统一 `compatible`、去掉 `cluster-type` 都没用，最后结论是 **17 线能起靠的是让苹果那个驱动不启动**
（`ArmCpu` 的 `IOProbeScore = 2000` 抢走 provider）。

`UniXNU/drivers/ArmCpu/ArmCpu.cpp:132-138`：

```cpp
static cluster_type_t clusterType (uint32_t physical) {
  const ml_topology_info_t *topology = ml_get_topology_info ();
  for (unsigned i = 0; i < topology->num_cpus; i++) {
    if (topology->cpus[i].phys_id == physical) { return topology->cpus[i].cluster_type; }
  }
  return ml_get_boot_cluster_type ();   // ← ADT 无 cluster-type 时全部落到这里
}
```

⇒ 所有核拿到同一个 `cluster_type` ⇒ **单一 processor set**，AMP / Edge 的「跨簇」前提不存在。
这一条是 17 线与 sheng 最本质的差异。 `[推测]`：单 pset 下 Edge 矩阵的实际行为未在本机观测。

### 1.4 本机硬件其实是**三簇** `[DT]`

反编译 `fw/dtb_04.dtb` → `/tmp/muyu_dtb04.dts`：

| 项 | 值 |
| --- | --- |
| CPU 节点 | 8 个 `qcom,kryo`，`reg` 0x000…0x700，`enable-method = "psci"` |
| `cpu-map` | **cluster0 = 3 核、cluster1 = 4 核、cluster2 = 1 核**（`:634-685`） |
| idle-states | `silver-c3` / `silver-cluster0-c4(rail-pc)` / `gold-c3` / `gold-cluster1-c4(rail-pc)` / `gold-plus-c3`，均带 `local-timer-stop`（`:693-740`） |
| cpufreq | `qcom,cpufreq-hw` = **`qcom,cpufreq-epss`**，三域 `0x17d91000 / 92000 / 93000` 各 0x1000（`:11503`） |
| 冷却映射 | `qcom,cluster0 = <cpu4..cpu7>`、`qcom,cluster1 = <cpu7>`（`:15006-15007`） |

sheng 是 **SM8550 两簇 E(3×A510) / P(5)**；本机是 **SM8635 cliffs 三簇 3+4+1**。
⇒ sheng 的 `MAX_CPU_CLUSTERS = 2`（`zzyf-sheng-amp.patch`）与两簇 Edge 矩阵**不能直接搬**。

### 1.5 逐核状态**没上屏** `[代码][配置]`

差点判错的一处：`BootProgress.c:299-309` **确实写了**逐核诊断：

```cpp
if(&armcpu_smp_requested){
  ... "SMP req=%u run=%u cond=%u boot=%x |" ...
  for(unsigned c=1;c<8;++c) if(armcpu_smp_step[c]||armcpu_smp_psci_rc[c])
      ... " c%u=step%u" / "!fail%u" ...;
  line(38,b,...);
}
if(&native_cpufreq_stage && native_cpufreq_stage>=2) line(39,native_cpufreq_row,...);
```

但：

- `BootProgress.c` 是**独立的 opt-in 诊断屏**（头部 `Opt-in diagnostic screen`），不是
  `BravearkLastLog.c` 的 5 s ticker；
- `porting.toml:254` 逐字：`the flashed 17-line builds also ran the BootProgress layer as weak stubs`
  （`LegacyDisplayStubs.c` 头部同义）；`porting.toml:257` 的 `next_action` 是
  「设 `CONFIG_XNU_BOOT_PROGRESS=y` + `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y` 再重新链接」；
- 当前镜像二进制里 **`SMP req=` / `CPU IRQ phase=` 字符串都搜不到**；
- `BootLog.c:120` 只 weak 声明 `native_cpufreq_row[120]`，全文无打印点；且
  `sheng_cpufreq_probe()` 靠 `sheng-cpufreq` 参数起步（`CpuFreqProbe.cpp:36`），该参数未带 ⇒
  `native_cpufreq_stage` 恒 0 ⇒ 就算 BootProgress 在，`line(39)` 也不会出。

⇒ **当前屏照上唯一与 CPU 有关的是 `PROC` 进程普查**（`BravearkLastLog.c:932`，每 3 遍 / 15 s），
没有每核 step/fail、没有 `CPUFREQ` 行、没有 IRQ phase。
**「8 个核是否全起」在本项目至今无证据**，这本身是最值得补的观测。

---

## 2. 「不正常」的六处，逐条定级

| # | 现象 | 证据 | 性质 | 影响 |
| --- | --- | --- | --- | --- |
| A | `PROC n=1 launchd=0`、`q=0` | `GPU-HANDOFF.md §55` | **当前阶段特征**，非 CPU 故障 | 调度器无用户态负载；此时任何 AMP/DVFS 优化都无消费者 |
| B | ADT 无 `cluster-type` ⇒ 单 processor set | `muyu.dts:80`、`ArmCpu.cpp:132-138` | 设计缺口 `[代码]` | 8 核被当成同一类型；无 E/P 亲和、无跨簇迁移语义 |
| C | `armcpu-edge=0`（缺省）⇒ 不调 `ml_cpu_init_completed()` | `ArmCpu.cpp:538-542` | 设计缺口 `[代码][配置]` | Edge 矩阵全禁；sheng **macOS** 实测「无该参数时线程完全不在簇间迁移」（置信度边界见 §3.2） |
| D | CPU idle / cluster 电源开关 10 处全 `unavailable() → panic()` | `ShengPlatformPower.cpp:4, 34-45` | 设计缺口 `[代码]` | `sheng-pmgr=2` 只是发布 nub 让 AMFI 不报 `No PMGR?`，**不是电源管理**（`POWER-MANAGEMENT.md §2` 已定案） |
| E | 无 DVFS governor | `CpuFreqProbe.cpp` 74 行、默认关、镜像无参数 | 设计缺口 `[代码][配置]` | 频率停在固件/ABL 给的值；注释逐字 `Nothing here is validated on the device until a boot has shown it` |
| F | `wdt=-1` + `sheng-disable-watchdogd=1` | `board.toml:403-407` | 刻意的诊断开关 | 卡住不自动复位 —— 既让 §49/§55 的「稳态卡滞」可被观察，也意味着死锁不会被兜底 |

补充两条来自 sheng `porting.toml:99` 的**内核通用坑**（`[代码]`，17 线同样适用但未取证）：

- 核注册时**没有 power gate latency**，`armcpu-idle-latency-us` 默认 0 ⇒
  「every IODelay of a thread blocks on a timer」。17 线镜像没有这个参数。
- XNU 的 work interval 操作 6/7/8 需要 `native-work-interval-instance.patch`；
  「without a performance controller they only succeed」—— 17 线无 performance controller。

**判读要点**：B–E 是「缺功能」，不是「运行时出错」。它们不会造成 `q=0`；
把主线精力转到修调度上，是**错配优先级**。

---

## 3. sheng 对照：跑通了什么，为什么不能整套搬

### 3.1 sheng 的 CPU 章节 `[代码][实测]`

`porting.toml:92-116` `id="cpu" status="implemented"`：

- **8 核全起**，PSCI 走 SMC，GIC distributor `0x17100000` / redistributor `0x17180000` /
  stride `0x40000` / 19.2 MHz timer；DTS 给 `cpu0..2 = "E"`、`cpu3..7 = "P"`
  （`dts/sheng.dts:44-51`，`unixnu,cluster-type`）。
- 参数：`smp=1 armcpu-spi-ipi=1 armcpu-self-ipi=1 cpufreq=4`（`board.toml:233`）。
- 内核补丁：`zzyf-sheng-amp.patch`（`__ARM_AMP__=1`、`MAX_CPU_CLUSTERS=2`、
  UI work 留 P cluster、`coreduetd` 停掉）、`zzzzzzzzzzz-sheng-edge-matrix.patch`、
  `zzye-sheng-cpu-idle.patch`、`native-work-interval-instance.patch`。
- `CpuFreqProbe.cpp` 384 行：**mode 4 governor**（16 ms 窗口、busy share、
  1.25× schedutil margin、90% hispeed、L3 vote、`cpufreq-cap` / `cpufreq-min`）；
  `:233` `IOSleep(16)`，`:382` 起 governor 线程。
- 真机 `[实测]`（`reports/ipados27-touch-frame-gaps.md`，boot `da1dbd496`）：
  6,680 个精确 SpringBoard 主线程采样、6,679 组 PMU 读数、**8,485 个 DVFS 窗口**；
  5 个繁忙 flip 区间的 **82 个 governor 窗口里 performance / prime 域全部请求最高频**；
  131.501 ms 连续输入区间读到 **2,707 / 2,956 MHz**；
  SpringBoard 主线程同步 XPC 等 `iconservicesagent` 4.644 s，与 4.583 s flip 间隔中的
  **4.444 s** 重叠。`POWER-MANAGEMENT.md §4` 另记 idle 均值约 1.2 GHz / 620 MHz。

### 3.2 两处必须说清的置信度边界

1. **`armcpu-edge=1` 在 sheng 也未真机验证**。`porting.toml:99` 的 findings 里那两条
   「`timebase-user=1`」「`armcpu-edge=1` + 两簇矩阵」都标了 **`Not yet run on hardware`**；
   而 `board.toml:221-227` 说明这些是 **macOS-only 的 built-in 参数**
   （`BootWatchdogPolicy.c:60`，包在 `#if XNU_TARGET_OS_OSX` 里）。
   **iPadOS 27 的 `kernel_args`（`board.toml:233`）里没有 `armcpu-edge`**。
   ⇒ sheng 在 iPadOS 侧同样没启用 Edge 矩阵；「无该参数 ⇒ 最忙 E core ≥90% 占 13–28% 行
   （macOS runs 56–61）」是 macOS 实测，**不是 iPadOS 实测**，更不是本机结论。
2. `zzzzzzzzzzz-sheng-edge-matrix.patch` 头部逐字记录了**矩阵设计踩过的坑**：
   P→E 对高 QoS bucket 关闭会违反 `sched_edge_config_verify_non_decreasing_qos_strictness`
   单调性检查；E→P 只开 steal 不开 migration 会导致 `scr_incoming_migration_allowed` 挡住
   P 核取 E 工作。⇒ 即使将来做三簇矩阵，也不是「把 2 改成 3」这么简单。

### 3.3 硬差异 `[DT][代码]`

| | sheng (SM8550 / iPadOS 27) | muyu 17 线 (SM8635 cliffs / iPadOS 17) |
| --- | --- | --- |
| 簇数 | **2**（E 3 + P 5） | **3**（3 + 4 + 1） |
| ADT cluster-type | 有，`cpu0-2=E / cpu3-7=P` | **无**（加了会碰 AppleARMCPU 校验，§6.31 已踩坑） |
| CPU compatible | 各自真实型号 | **8 核统一 `arm,cortex-a720`**（为满足校验） |
| `MAX_CPU_CLUSTERS` | 2（`amp.patch`） | 内核未设置；改 3 需重验 ADT |
| SMP 诊断 | `armcpu-diagnostics=1`、`armcpu-self-ipi=1` | 全未启用，且 BootProgress 层是 weak stub |
| DVFS | `cpufreq=4` 16 ms governor，真机全频域 | 74 行探针默认关、镜像无参数 |
| 用户态 | SpringBoard PID 30 / wifid PID 49 | **`PROC n=1 launchd=0`**（§55） |

---

## 4. 建议顺序（严格不碰 GPU 主线）

### P0 —— 现在唯一值得做的：把逐核状态接进下一版只读诊断镜像

**不新增刷机轮次**：主线 §55 已经在准备 `bsd1` 阶段梯，把下面几项并进同一个镜像即可。

- **最轻方案**（推荐）：在 `BravearkLastLog.c` 的 5 s ticker 里加**一行**，
  weak 引用 `armcpu_smp_requested / running / step[8] / fail[8] / psci_rc[8]`
  （符号已在 `BootProgress.c:104-106` 用 `weak_import` 声明过，格式可直接照抄 `:299-305` 的
  `SMP req=… run=… | c1=step7 …!failN`）。纯读，零行为改动。
- **顺带**：把 `native_cpufreq_row` 打出来（`BootLog.c:120` 已声明），并配 `sheng-cpufreq=1`
  **只读取证**（三域 enable / current / requested / 最高 LUT）——本机 EPSS 三域地址已由
  `POWER-MANAGEMENT.md §3.1` 确认与探针常量一致；先验「读得到」再谈写。
- **可选**：`armcpu-diagnostics=1` / `armcpu-self-ipi=1` 是内核已有开关
  （`ArmCpu.cpp:399-406`），但 `armcpu-self-ipi` 会让每核自送 SGI 0，**属于行为改动，
  不要和只读探针混在同一轮**。
- 若想恢复整屏 `BootProgress`，按 `porting.toml:257` 设
  `CONFIG_XNU_BOOT_PROGRESS=y` + `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y` 重链——成本更大，
  且会改显示路径，不建议在主线卡 `bsd_init()` 的当口做。

**这一轮能回答的三个问题**：8 个 secondary 核是否全部到 `step=7`？有没有 `fail` / PSCI 负返回？
三域频率现在停在多少？——三个都**目前无证据**。

### P1 —— launchd 起来之后

按依赖顺序：`cluster-type` 与 ADT 校验 → 三簇 `MAX_CPU_CLUSTERS=3` →
Edge 矩阵（参考 sheng patch 的 QoS 单调性约束）→ `armcpu-idle-latency-us` →
`cpufreq` mode 4 governor → performance controller / work interval。
用户态没起来之前搬 `IOPMPowerSource` / `mTPL` / `wifid` / `bluetoothd` 同理无消费者。

### P2 —— 长期

PSCI idle / cluster rail-off（本机 DT 已有 `silver-cluster0-c4` / `gold-cluster1-c4`
的 entry/exit latency 与 `min-residency-us`，是现成输入）、热压级别联动、跨簇负载统计。

### 🔴 红线

1. **现在不要动 `armcpu-edge=1` / Edge 矩阵 / AMP patch**：无消费者、需改 ADT、
   且加 `cluster-type` 会触发 `AppleARMCPU::_validateConfiguration` panic（§6.31 已踩）。
2. **别照抄 sheng 的两簇矩阵与 SM8550 CPU 域 3/4/1 映射**；本机是 3/4/1 之外的
   `cpu-map` 3+4+1，L3 节点 `17d90000` 在本机 DT 未确认（`POWER-MANAGEMENT.md §3.1`）。
3. **别单独改 `wdt=-1` / `sheng-disable-watchdogd=1`**：会让当前「稳态卡滞」自动复位，
   破坏 ticker 观察窗口（`board.toml:404-406` 记着它是两个复位源之一）。
4. **别抢刷机窗口**：设备自 §54 起归接手 GPU 的 AI 调度；本轮只读查了一次
   `fastboot devices`（`a20248f7` 在线），未刷机、未引导。

---

## 5. 证据等级与未验证项汇总

| 结论 | 等级 | 出处 |
| --- | --- | --- |
| 17 线当前 `sheng-smp=1 armcpu-spi-ipi=1`，无 `armcpu-edge` / `sheng-cpufreq` | `[配置]` | 镜像二进制直搜 + `board.toml:403-408, 578` |
| `porting.toml` 的「单核」是 27 线 u15x 历史 | `[配置]` | `porting.toml:60-75`、`UKE-CODE-AUDIT.md §4` |
| 17 线 ADT 无 `cluster-type`、8 核统一 compatible | `[代码]` | `dts/muyu.dts:26-41, 80` |
| 本机硬件三簇 3+4+1、三域 EPSS、idle-states 齐全 | `[DT]` | `/tmp/muyu_dtb04.dts:634-740, 11503` |
| `armcpu-edge=0` 不调 `ml_cpu_init_completed()` | `[代码]` | `ArmCpu.cpp:538-542` |
| BootProgress 逐核行存在但当前构建为 weak stub | `[代码][配置]` | `BootProgress.c:299-309`、`porting.toml:254`、镜像无 `SMP req=` 字符串 |
| `q=0` / `PROC n=1` / 断点在 `bsd_init()` | `[实测]` | `GPU-HANDOFF.md §55`（屏照 `IMG_20261112_192601.jpg`） |
| sheng 8 核、governor、8,485 DVFS 窗口、2,707/2,956 MHz | `[实测]` | `porting.toml:92-116`、`reports/ipados27-touch-frame-gaps.md` — **另一块板/另一套系统** |
| `armcpu-edge=1` 组合「未在真机跑过」、iPadOS 参数串未带 | `[代码]` | `porting.toml:99`、`board.toml:221-233` |

**`[推测]` 未验证项**：

- 17 线 8 个 secondary 核是否全部成功 bring-up —— **无任何屏上证据**，P0 就是补这个。
- 单 processor set 下 Edge 矩阵的实际行为；`ml_get_boot_cluster_type()` 的返回值。
- 本机三簇的 E/P 语义（DT 只给了 `cpu-map` 与 silver/gold/gold-plus idle 名，
  未直接给 Apple 侧 cluster type）。
- 当前 CPU 实际运行频率、是否有任何固件侧 DCVS 在动 —— 未读回 EPSS。
- 「修好调度能改善帧间隔/发热」是线性外推，sheng 的数据来自 SM8550 + macOS 27.3，
  不能直接套到 17 线。

---

## 6. 与其他报告的分工 / 给接手 AI 的一句话

- `POWER-MANAGEMENT.md`：**电源/热/充电/DVFS 能力**与 sheng 组件对照（已定案「17 线基本没有」）。
- 本文：**调度拓扑（processor set / Edge / AMP）与逐核可观测性**；两者共用
  `CpuFreqProbe.cpp` 与 `ShengPlatformPower.cpp` 的结论，不重复展开。
- `WIFI-PORT.md` / `BLUETOOTH-PORT.md` / `DISPLAY-LOAD-BACKLIGHT.md §7`：同样卡在
  `PROC n=1 launchd=0`。

> 转给主线：§55 的 `q=0` 说明断点在 `bsd_init()`，**调度优化此刻没有收益**；
> 但在 `bsd1` 里顺手加一行只读 SMP 快照是零成本的，它能给出一个至今空白的答案——
> 「8 个核到底起没起全」。若发现 secondary 核有 `fail` / PSCI 负返回，
> 那才可能和 `bsd_init()` 卡滞有因果关系；若全 `step=7`，就可以彻底排除 CPU bring-up，
> 专注 root mount / owner-cache。

---

*本轮纯只读调研：未刷机、未碰设备、未构建、未改任何源码 / 镜像 / 板级配置 / GPU 工作树，
也未改 `GPU-HANDOFF.md` 与 `HANDOFF-COLD-START.md`（避免与接手 AI 撞文件）。
`logs/flash-ledger.tsv` 末条为 `11:21:45Z boot fw/boot-17line-043f7120.img rc=0`；
设备此刻在 fastboot（`a20248f7`），归 GPU 主线调度。*
