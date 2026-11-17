# 显示负荷与背光 · 调研报告

**2026-11-12 18:2x 北京｜17 线 / 21H461 / Adreno740｜纯只读调研**
**18:3x 追加 §7：复查 sheng 后有重大订正，请先读 §7 再看 §4。**

> ⚠️ **订正提示**：§4 写的「上游没有可抄的背光实现」**是漏查**：只查了 `UniXNU-uke`，没查 `UniXNU-sheng`。
> sheng 有完整的 KTZ8866 背光服务，而且本机 `fw/dtbo.img` 里就有 `ktz,ktz8866` / `ktz8866@11` —— **同一颗芯片、同一个 I2C 地址**。
> §5 第 1 步（从 stock DTB 定位背光节点）已离线完成，§6「本机具体是哪一条尚未取证」已取证。详见 §7。
>
> 范围：① 如何降低屏幕的像素显示密度以减轻 GPU 负荷；② 熄屏后屏幕仍然发光。
> 本轮**未改任何源码 / 镜像 / 板级配置、未刷机、未碰设备**。GPU 主线已移交另一个 AI，
> 本报告与其工作树无交叉，涉及 GPU 的地方只写前提、不动代码。
>
> **证据等级标记**：`[代码]` = 源码静态可复核；`[实测]` = 文档记录的真机测量；`[推测]` = 未验证，
> 不能直接当结论用。这份标记是刻意的——本项目已经吃过「把 plausible 当事实」的亏。

---

## 0. 两问两答

| 问题 | 结论 |
| --- | --- |
| 改哪个旋钮能降 GPU 负荷？ | **当前没有任何现成旋钮能降**。`offline-legacy-unit-scale=2`（当前镜像确实带）只改 UI 逻辑密度，扫描输出仍逐像素拷贝；`sheng-fb-half` 是 half-float 像素格式转换，名字里的 half 不是「半分辨率」。真正的杠杆是**降渲染分辨率**或**降帧率**，前者要改代码，后者有一个代码坑（§3.2）。 |
| 熄屏为什么还发光？ | **不是 bug，是这个 port 从未实现运行时背光 / 面板电源控制**。「熄屏」只是把 GOP framebuffer 整屏填成黑色 `0xff000000`，背光 LED 一直通电。原厂公开的电源请求 API 在这个 port 返回 `Unsupported`，诊断脚本明确记录了 `power_transition_completed=False`。**【18:3x 订正】17 线侧仍成立，但 sheng 有完整可移植的 KTZ8866 背光驱动，本机 dtbo 里就是同芯片同地址 → 见 §7。** |

两个问题有个共同点：**都卡在「XNU 侧没有接管固件点亮的那块面板」**。固件（ABL/UEFI）负责点屏和建 GOP，XNU 只在它的 framebuffer 上写像素，既不能改面板时序，也不能关背光。

---

## 1. 先分清四个容易混为一谈的量

| 量 | 谁决定 | 能否改 | 改了会怎样 |
| --- | --- | --- | --- |
| **物理面板像素** 3200×2136 / PPI | 面板硬件 + ABL 从 DT 配的 HSYNC/VSYNC/DSI 双链路 | ❌ | 改时序直接黑屏；`DPU-PORT.md` §8.3 把「不改 INTF/PHY 时序」当铁律 |
| **扫描输出分辨率**（GOP framebuffer 几何） | UEFI 按面板分辨率建 `Video[0]` | ❌（单独改） | 只改 `splash_region` / GOP 几何，画面会缩在左上角，DPU 仍按 3200×2136 扫 |
| **UI 逻辑密度** `/chosen display-scale` | 显示驱动发布 | ✅ | 只改布局（每逻辑点对应几个物理像素），**不改变每帧像素数** |
| **渲染分辨率**（合成器产出的 IOSurface） | 显示驱动报给 iPadOS 的模式 | ✅ | ⭐ 这才是降 GPU / 带宽负荷的杠杆，但降了必须补一个放大环节 |

数字（uke 在同一块面板上的实测，`DISPLAY-RESOLUTION.md` §2 / `DPU-PORT.md` §8.4）`[实测]`：

| 项 | 3200×2136 | 1600×1068（½×½） |
| --- | --- | --- |
| 像素/帧 | 6,835,200 | 1,708,800（¼） |
| BGRA 字节/帧 | 27,340,800 B（27.34 MB） | 6,835,200 B（6.84 MB） |
| scanout @60 Hz | 1.64 GB/s | 0.41 GB/s |
| scanout @120 Hz | 3.28 GB/s | 0.82 GB/s |

---

## 2. 当前镜像里实际生效的显示参数

`grep -a -o` 直接搜 `fw/vendor_boot_gpu_upper1.img` 二进制 `[代码]`，命中：

```
offline-legacy-unit-scale=2
sheng-vsync=60
```

`board.toml` 另有 `offline-legacy-pm=1`、`offline-legacy-wcomb=1`、`offline-legacy-phase=4`、
`offline-hid-home=1` 等。**没有** `sheng-dpu-scanout`、**没有** `sheng-vsync-fixed`、
**没有** `sheng-fb-half`（后者默认 mode=1，不需要显式写）。

---

## 3. 问题一：降低渲染负荷的可行路径

### 3.1 先排掉两个「看起来像」的旋钮 `[代码]`

**`offline-legacy-unit-scale=<1..3>`** — `LegacyDisplayProbe.cpp:350-375`。
它往 `/chosen` 写 `display-scale` 和 `display-rotation`，源码注释逐字写着：

> *"It changes only the published display-scale; the scanout copy still moves one actual pixel per pixel."*
> *"This is not physical pixel density, panel timing, an Apple device identity or a synthesized mode list."*

⇒ 从 2 改成 1 或 3，UI 元素物理大小变，合成器每帧仍要填 6.84 Mpx。**不会降负荷**，别在这上面花真机轮次。

**`sheng-fb-half=<1|2>`** — `NativeLegacyDisplay.cpp:114-140`。这是把 iPadOS 27 的 RGBA16Half
（`'RGhA'`）表面查表转换成扫描输出 BGRA8 的**颜色编码模式**（mode 2 = 值已 sRGB 编码），
配套 `convertHalfRows()`。与分辨率、缩放完全无关。

### 3.2 方案 A：降帧率（改动最小，但有一个必须知道的坑）

代码依据 `[代码]`：

- `startVsync()`（`:424-433`）：`if(!hz || hz>240 …) return`，**没有下限检查** ⇒ `sheng-vsync=30`
  会建立 30 Hz 的 work-loop timer，vsync 消息按 30 Hz 发。
- `getRefreshAttributes()`（`:546-559`）：`if(hz<=60 || hz>240) return true;` ⇒ **≤60 时直接早退，
  不覆盖上报的 `IOMFBDisplayRefresh`**，仍是原厂 slot 301 的 min=1/60、max=1/24。
- `sheng-vsync-fixed=1` 的 `locked` 判断在这条早退**之后**，所以 ≤60 时它也不生效。

⇒ 直接把 `sheng-vsync` 从 60 改到 30：**vsync tick 降到 30 Hz，但上报的刷新属性仍是 60/24**，
两者不一致，这是代码里没覆盖的低 Hz 路径，效果未经设计也未经实测。**不能宣称「零风险改个数字就行」**。

另外，当前 `sheng-vsync=60` 时注释已经说明：原厂 max interval 是 1/24 s，
render server 本来就有 **24～60 Hz** 的自适应区间（`:543-545`）。也就是说系统空闲时
**理论上已经能自行降帧**，不一定需要改参数。

若要干净地锁 30 Hz，最小改动是让 `getRefreshAttributes` 处理 `24<=hz<=60` 这一段
（去掉 `hz<=60` 早退或改成 `hz<24`），再配 `sheng-vsync-fixed=1` 把 min/max 都钉到 30。
**这是约一行的改动**，但必须实测验证 `IOMFBDisplayRefresh` 与 render server 的 pacing 是否一致。

收益 `[推测，按线性外推]`：帧数减半 ⇒ GPU 组合次数、CPU 帧拷贝和内存带宽近似减半；
对本项目尤其有意义——**无 thermal zone、无 DVFS、`wdt=-1`（`AGENTS.md` §5.6）**，
降帧是最直接的降温手段。代价是 UI 流畅度。

### 3.3 方案 B：软件 2× 放大（不碰 DPU 寄存器，中等成本）`[推测，未实现]`

思路：让显示驱动向 iPadOS **报 1600×1068 模式**，合成器只产出半尺寸 IOSurface；
在现有的 CPU 拷贝路径里做 nearest-neighbor **2× 放大**写进 3200×2136 的 GOP。
这样合成器像素降到 ¼、GPU↔主机内存带宽降到 ¼，而面板时序、GOP 几何、DPU 都不动。

要改的位置 `[代码]`：

| 位置 | 现在做什么 | 要改成 |
| --- | --- | --- |
| `installBootDimensions()` / `getDisplayDimensions()`（`:877-887`） | 从 `PE_Video` 读 GOP 宽高，作为报给系统的显示尺寸 | 报 ½ 尺寸；GOP 实际尺寸单独保留给拷贝用 |
| `native_legacy_surface_bounds()`（`NativeLegacyRequest.h:30-`） | 要求 IOSurface 宽高 **≥** 显示宽高 | 允许半尺寸 surface，按放大后的行宽算边界 |
| BGRA / half 拷贝循环（`:680-720`，`convertHalfRows()`） | 逐行 1:1 拷贝 | 每源像素写 2×2、每行写两遍 |
| blank 路径（`:592-613`） | 整屏填 `0xff000000` | 不变（黑就是黑） |

优势：纯软件、可回滚、不需要 QSEED 寄存器知识。
代价：**CPU 仍要写满 27.34 MB**，而且写放大让随机访问更多；不过 `offline-legacy-wcomb=1`
已把 scanout 映射改成 write-combining（`board.toml:333-336` 注释说 device-nGnRnE 才是当年
卡在几 fps 的原因），且 `DISPLAY-RESOLUTION.md` §3 的 uke 实测判定 CPU 不是瓶颈、
大头是 GPU↔主机内存带宽 ⇒ 这笔账大概率划算，但**必须实测**。

风险：UI 会变「软」；需要三图案（纯色 / 棋盘 / 渐变）色位验收，并和 3200×2136 版逐项对比布局。
**如果 GPU 直扫（另一个 AI 的方向）落地，这条软件放大路径就不适用了**，要改在 DPU/GPU 侧。

### 3.4 方案 C：DPU VIG / QSEED 硬件放大（`DISPLAY-RESOLUTION.md` 的原方案，成本最高）

合成器帧 1600×1068 → VIG0/VIG1 各写一次 scaler 1:2 放大到 3200×2136 扫出，
约 150 行寄存器编程 `[实测/估算]`。现成线索：

- `mdss-cliffs-map.md:50`：VIG 有 qseed 缩放，`sde-vig-qseed-off=0xa00`；
  SSPP 寄存器 `SSPP_SRC_SIZE` 0x00、`SSPP_OUT_SIZE` 0x0c。
- 原厂参考实现：`drivers/gpu/drm/msm/disp/dpu1/dpu_hw_sspp.c: setup_scaler` / `dpu_hw_sspp_scaler.c`。

**前提**：`sheng-dpu-scanout=2` 的 DPU 直扫（`NativeLegacyDisplay.cpp:269` `startDirect()`，
还要求 `native_ufs_dpu_state>>31`、SMMU bank 窗口）。**17 线当前镜像没有这个参数**
（§2 实证），仍是 CPU 拷贝 + WC 映射。

⚠️ **一处容易误读的上游证据**：`DPU-PORT.md` 状态行写着 `C3（1600×1068）✅`，
但该文档 §6 对 C3 的定义是「在同一窗口/管线上换 stride/format，三图案色位验收」，
§8.3 明确不改 INTF/PHY 时序，`DPU-TASKS.md:199` 记录 VIG0/VIG1 各 **1600×2136**
是 dual-DSI 的左右半屏。**上游并没有现成的「降渲染分辨率 + QSEED 放大」实现可以整体搬**，
`DISPLAY-RESOLUTION.md` §5 的「17 线还没有 DPU 直扫、排在后面」结论仍然成立。

### 3.5 三条路的对照

| | 改动量 | 是否碰硬件寄存器 | 降 GPU/带宽 | 主要风险 |
| --- | --- | --- | --- | --- |
| A 降帧率 | 0～1 行 + boot-arg | 否 | 线性（按帧数） | `getRefreshAttributes` 低 Hz 早退，pacing 不一致 |
| B 软件 2× 放大 | ~100 行，4 处 | 否 | ¼（按像素） | CPU 写放大、UI 变软、GPU 直扫落地后失效 |
| C DPU QSEED | ~150 行 | 是 | ¼ + 免 CPU 拷贝 | 改错黑屏；需先有 DPU 直扫 |

**一个必须说清的前提**：13:48 那条路径里 GPU 用户态图形栈整体没启动
（`GPU-HANDOFF.md` §49），当前实际是 CPU 拷贝路径。在 GPU 真正接管合成之前，
「降 GPU 负荷」的收益有限；但 A/B 能立刻减少 CPU 拷贝与内存带宽，
对**发热和续航**是实打实的好处，不依赖 GPU 主线通关。

---

## 4. 问题二：熄屏后仍然发光

### 4.1 根因：只画了黑帧，没有关背光 `[代码]`

证据链，全部在 `UniXNU/drivers/GopFramebuffer/NativeLegacyDisplay.cpp`：

1. **`:465-480` `announcePoweredByFirmware()`** — 只发布**逻辑** powered 状态
   `OriginalLegacyStateRequest {1,0,0,1,1,{}}`，目的是让 user client 的 selector 6
   不再永远回答 `kIOReturnBusy`、合成器才能 present。注释逐字写着
   *"does not implement a panel power transition or change IPC results"*。
2. **`:511`** — `offline-legacy-pm=1` 挂的 IOKit PM 生命周期：
   *"GOP provides no panel power switch: neither state advertises physical power output,
   power control or a hardware power transition."* 它只管**逻辑 scanout 调度**。
3. **`:592-613` blank swap** — 「熄屏」的实际动作是把整个 GOP framebuffer
   `native_legacy_fill_pixels(..., 0xff000000U)` 填成不透明黑，然后 `dsb sy`。
   `NativeLegacyRequest.h:22-28` 的 `native_legacy_blank_state()` 判定「所有 layer 都 disabled、
   背景为黑」就走这条路。**像素黑了，背光 LED 一直通电** ⇒ 肉眼看到的就是「熄屏仍发光的灰黑屏」。
4. **原厂公开 API 也走不通** — `scripts/check-original-framebuffer-power.py` 的 checkpoint
   明确写：`requested_raw_state=1`、`original_api_status='0xe00002c7'`（`kIOReturnUnsupported`）、
   `power_transition_completed=False`、`private_state_modified=False`、`hardware_tested=False`。
   `IOMobileFramebufferRequestPowerChange(display, 1)` 返回 Unsupported。
5. **历史现象一致** `[实测]` — `GPU-ONDEVICE.md` §10/§11 多处记录「背光亮、全黑」；
   `board.toml:572-573` 还提过「synthetic Home press … wakes the panel, so the display does not stay
   blanked after the first lock screen」—— 所谓 wake 也只是恢复写帧，不是开关背光。

⇒ **结论**：系统认为已经熄屏，实际上只是画了黑帧。这个 port 里**没有任何运行时背光或
面板电源控制**，所以「熄屏仍发光」是设计缺口，不是回归 bug，也与 GPU 主线无关。

### 4.2 修法路径：先找到真实的硬件通路

**最有价值的线索** `[代码]` — `board.toml:634-640`：

> *"The stock ABL powers the panel off when the device tree has no splash region, so add it."*

⇒ **ABL 里存在面板电源控制**（启动期按 DT `splash_region` 决定点不点屏）。
把 ABL / stock DTB 里那条电源序列抄出来，比在 XNU 里猜寄存器靠谱得多。

高通常见的三类背光通路 `[推测，需取证]`：

| 通路 | 熄屏动作 | 备注 |
| --- | --- | --- |
| PMIC WLED（SPMI） | 写亮度/使能寄存器为 0 | 最常见，SM8550/cliffs 平台 |
| PMIC GPIO / PWM | 拉低背光使能脚 | 需 pin 复用正确，写错风险高 |
| DSI panel MIPI DCS | `DISPLAY_OFF`(0x10) + `SLEEP_IN`(0x11) | 需要 DSI 命令通道，ABL 已初始化 DSI |

**项目里已有的只读取证基础** `[代码]`（不用从零造轮子）：

- `package/BootResetCore.h` — spmi-pmic-arb v7 寄存器布局、`BR_APID_MAP`、APID 遍历、
  `br_read8()` / `br_write8()`，注释明写「SM8550 devicetree's」寄存器事实。
- `package/ButtonProbeMuyu.cpp` — PMK8550 PON（SPMI SID 0）、PMXR2230 GPIO6（SID 1）、
  扫描所有 APID 找 `PERPH_TYPE 0x10 / subtype 0x13` 的 GPIO 控制器并打印 PPID。
- `board.toml:445-450` — 历史 stage 29/30 做过「read-only SPMI PMIC census：每个 APID 的
  PERPH_TYPE、GPIO mode/enable/level」，**这套 census 可以直接复用来找 WLED 外设**。

⚠️ ~~**上游不能直接抄**：`UniXNU-uke/dts/uke.dts` 里 grep 不到任何
`backlight` / `wled` / `brightness` / `qcom,mdss-dsi` 节点 ⇒ uke 也没做运行时背光控制。~~

> **【18:3x 订正，此段只在 uke 范围内成立】** 本节只查了 `UniXNU-uke`，漏查了 `UniXNU-sheng`。
> sheng 有完整背光驱动（`AppleARMBacklight.cpp` 等 844 行），且本机 `fw/dtbo.img` 明确有
> `ktz8866@11` / `compatible = "ktz,ktz8866"`。下面 4.3 的「找 WLED / PMIC census」路线
> **不再是首选**，请直接看 §7 的 I2C KTZ8866 取证与三级移植路径。

### 4.3 建议的取证顺序（不要跳步）

1. **只读查 stock DTB / ABL**：找 `qcom,wled` / `pwm-bl` / `mdss-dsi-bl-pmic-control-type` /
   `panel-supply` 节点，拿到 SPMI SID、PERPH_TYPE、寄存器偏移或 GPIO 编号。
2. **复用 PMIC census**：用现有 `br_read8` 能力枚举 APID，把 WLED / GPIO 外设的
   `PERPH_TYPE`、`mode`、`enable`、`level` 打到面板上，确认地址和当前值（ABL 点亮后的状态）。
3. **在 blank 路径里先加一个只读探针**：熄屏时打印背光寄存器当前值，确认「系统熄屏时
   这个值确实没变」，把因果链坐实。
4. **再实现动作**：`setBrightness(0)` 或 DCS `DISPLAY_OFF`；唤醒路径对称恢复。
5. **验收**：熄屏后拍照必须完全不发光；按电源键能唤醒且亮度恢复；
   连续 10 次熄/亮不黑屏、不卡在 fastboot。

🔴 **红线**：`board.toml:449-451` 记过 stage 31「对每个未使能的 PMIC GPIO 逐个拉高找 Novatek id」
这类盲写实验，**不要重演**。PMIC/GPIO 写错可能烧背光或让面板永久不亮。
在拿到 stock DTB 的确定地址之前，只做只读。

**临时代价更小的缓解**（不解决发光，但值得记）：blank 时除了填黑，**停止后续向 GOP 写帧**，
可省掉熄屏期间的 CPU 拷贝和内存带宽；背光仍然亮，所以这不是修复。

---

## 5. 建议执行顺序

| 优先级 | 动作 | 依赖设备 | 说明 |
| --- | --- | --- | --- |
| 1 | ~~从 stock DTB / ABL 定位背光节点~~ **已完成（§7.3）** | 否（离线） | 结果：I2C 双 KTZ8866，SE0 `0xa80000` / SE12 `0x890000`，不是 PMIC WLED |
| 2 | **KTZ8866 只读取证**：读回 `0x04/0x05`、SE 状态、GCC vote 现值 | 是 | 先坐实因果，不写任何寄存器；红线见 §7.6（勿照抄 SM8550 vote 位图） |
| 3 | 核实 `sheng-vsync` 低 Hz 的 pacing 行为 | 是 | 一行改动 + 屏照 / 计数器，验证方案 A 是否真降帧 |
| 4 | 方案 B 软件 2× 放大（若 A 不够） | 是 | 需与 GPU 主线的直扫方向对齐，避免白做 |
| 5 | 方案 C DPU QSEED | 是 | 等 DPU 直扫落地后作为独立优化，排在最后 |

**不要做的事**：改 `offline-legacy-unit-scale` 指望降负荷、把 `sheng-fb-half` 当半分辨率、
照搬 uke 的 `C3（1600×1068）✅` 当现成实现、在没拿到 stock DTB 之前写 PMIC/GPIO 寄存器、
改 INTF/PHY 面板时序。

---

## 6. 未验证项与证据边界

- `[推测]` 方案 A 的「帧数减半 ⇒ 带宽近似减半」是线性外推，未在本 port 实测；
  `getRefreshAttributes` 的低 Hz 早退是代码事实，其后果未实测。
- `[推测]` 方案 B 的 CPU 写放大是否划算，依赖 `offline-legacy-wcomb=1` 之后的真实带宽数据；
  `DISPLAY-RESOLUTION.md` §3 的「CPU 不是瓶颈」来自 **uke 27 线**，不是 17 线。
- ~~`[推测]` 背光的三类硬件通路是高通平台常识，**本机具体是哪一条尚未取证**；
  `uke.dts` 无节点不等于本机没有。~~
  **【18:3x 已取证（§7.3）】** `fw/dtbo.img` 的 `fragment@125/126` 明确是 I2C 双 KTZ8866 @ `0x11`；
  这句「`uke.dts` 无节点不等于本机没有」正是被证实的那条留白。仍未验证的是寄存器读回值与 `level=0` 是否真灭灯（§7.6）。
- 本报告所有代码行号基于 2026-11-12 的工作树；GPU 主线由另一个 AI 并行推进，
  若 `LegacyDisplayProbe.cpp` / `board.toml` 被改动，行号需重新核对。

---

## 7. sheng 复查（2026-11-12 18:3x 北京）· 对 §4 的重大订正

> 触发：用户「再看看 sheng 呢」。本轮仍是**纯只读**：只做了 `git ls-remote` / `git fetch`（仅更新 remote ref）/ 导出快照到 `/tmp` / 反编译本地 DTB，
> **未刷机、未碰设备、未构建、未改任何源码 / 镜像 / 板级配置 / GPU 工作树**。

### 7.1 一句话结论

**熄屏发光这条线，从「猜 PMIC/ABL 寄存器」变成了「有明确芯片、明确寄存器、明确驱动参考」。**
上一轮 §4 说「上游没有可抄的实现」是漏查：`UniXNU-uke` 确实没有，但 **`UniXNU-sheng` 有完整的背光驱动**；
更关键的是，本机 `fw/dtbo.img` 的 overlay 里就写着 `compatible = "ktz,ktz8866"` 和 `ktz8866@11` ——
**muyu 与 sheng 用的是同一颗背光 IC、同一个 I2C 地址 0x11**。§6 那句自留的「本机具体是哪一条尚未取证」现在有答案了：
是 **I2C 上的 KTZ8866 双驱动**，不是 PMIC WLED，也不是 DSI DCS。

降像素密度那边结论不变：**sheng 也没有现成实现**（§7.4）。

### 7.2 远端状态与 3 个新提交 `[代码]`

| 项 | 值 |
| --- | --- |
| `git ls-remote origin main` | **`cd73e23f0657c28a6638f165590d04f71ab76419`** |
| 上次缓存 `origin/main` | `e893717`（11-12 00:53 UTC） |
| 本地工作树 HEAD | **`251b291`，未动**（`fetch` 只更新 remote ref，未 checkout / 未 merge / 未改文件） |
| 快照 | 旧 `/tmp/sheng-latest` = `e893717`；新 `/tmp/sheng-cd73e23` = `cd73e23`（`git archive`，502 文件） |
| 变更文件 | 8 个，196 insertions / 22 deletions：`drivers/QcomIris/*`、`package/BootLog.c`、`porting.toml`、`reports/macos27-memory-run139.md` |

三个新提交，**都不是背光/分辨率**，但有一条与 GPU 主线直接相关：

- `b9223ad` 15:03 `feat(video): decode HEVC by default with pictures in decode order`
- `848cc2e` 18:19 `feat(diag): identify retained GPU hangs and fault address candidates`
  —— `BootLog.c` 新增 `[gpu stall] taken at hang N`、`far found` / `far base`（在 stalled job 的 IB1/IB2 里找 faulting address）、`[video vt log]` 64 行环形 trace；
  `porting.toml` 记录「hang check 保留恢复前的 CP 快照直到下一次 hang」。**这条建议转给接手 GPU 的 AI**，与 §49 的 hangcheck/restart 层是同一条线。
- `cd73e23` 18:19 `docs(porting): record macOS compressor, swap and encoding results`（run139 内存压缩/swap 实测）。

### 7.3 本机背光硬件取证 `[代码]`（可复现）

`fw/dtbo.img` 是 Android dtbo v0（`magic 0xd7b7ab1e`，1 个 entry，`dt_size=594788`，`dt_offset=64`）；
拆出后 `dtc -I dtb -O dts` 得 `/tmp/muyu_dtbo.dts`（14,466 行）；主 `fw/dtb_04.dtb` 反编译得 `/tmp/muyu_dtb04.dts`（21,342 行）。

**dtbo `fragment@125` / `fragment@126`（逐字）**：

```
fragment@125 { target = <&qupv3_se0_i2c>;   __overlay__ { status = "ok";
    ktz8866@11 { reg = <0x11>; status = "ok"; compatible = "ktz,ktz8866"; }; }; };
fragment@126 { target = <&qupv3_se12_i2c>;  __overlay__ { status = "ok";
    ktz8866@11 { reg = <0x11>; status = "ok"; compatible = "ktz,ktz8866"; }; }; };
```

主 DTB 里这两个引擎的 `reg` / `interrupts = <0 SPI 0x4>`：

| 项 | sheng（SM8550） | **muyu 17 线（SM8635 cliffs）** | 证据 |
| --- | --- | --- | --- |
| 背光 IC | 2× Kinetic KTZ8866 @ I2C `0x11` | **同**：两颗 `ktz8866@11` | dtbo fragment@125/126 |
| 亮度寄存器 | `BL_BRT_LSB 0x04`(bits 2:0) + `BL_BRT_MSB 0x05`，11-bit level，mainline 写法 | `[推测]` 同 compatible ⇒ 同寄存器，**尚未在本机读回** | `AppleARMBacklight.cpp:2-4, 49, 118, 148` |
| 引擎 A | `0xa84000`（QUP wrap1 se1），SPI 354 | **`0xa80000`**（`qupv3_0` se0），**SPI 353**（`interrupts = <0 0x161 4>`） | dtb_04.dts:14036 / 21223 |
| 引擎 B | `0x988000`（i2c hub engine 2），SPI 466 | **`0x890000`**（`qupv3_1` se12），**SPI 586**（`interrupts = <0 0x24a 4>`） | dtb_04.dts:14582 / 21251 |
| GCC clock vote | `0x52008` 置 bits `18,19,20,21,23,7,8,12`；`kHalt[] = 0x23294…` | **未知**；已知 `0x52008` **bit 26** = S4 SPI branch（`board.toml:420`） | `AppleARMBacklight.cpp:43-48` |
| bootloader 状态 | 注释：GENI I2C firmware 由 boot loader 加载并开好时钟 | dtbo 把 se0/se12 `status` 置 `"ok"`（主 DTB 里 se12 是 `"disabled"`）⇒ `[推测]` 同样已配置 | `AppleARMBacklight.cpp:5-6` |

⇒ **不能直接照抄的是三个常量组**（SE 基址、SPI 号、GCC vote/halt 位图）；**可以直接复用的是芯片协议与驱动逻辑**。
两颗 IC 分属 `qupv3_0` / `qupv3_1` 两个 wrapper，sheng 的两个则都在 hub 侧，这是最大的结构差异。

另外 dtbo 里还有 `qcom,pwm-lpg` / `pmk8550@0/qcom,pwms@e800` / `pmxr2230@1/qcom,leds@ef00` / `backlight_therm` / `mi,max-brightness-clone` 等字符串，
属于 PWM 与热降亮度通路；但**面板背光主器件已由 `ktz8866@11` 明确指定**，不必再猜 WLED。

### 7.4 降像素密度：sheng 的结论与 17 线一致 `[代码]`

- `sheng-latest/package/LegacyDisplayProbe.cpp:455-475` 与 17 线 `UniXNU-muyu-17/package/LegacyDisplayProbe.cpp:348-360`
  **逐字同源**，注释都是 `This is not physical pixel density` / `the scanout copy still moves one actual pixel per pixel`。
  sheng 的 `offline-legacy-unit-scale=2` 同样只改 UI 逻辑密度 ⇒ **§3 的「没有现成旋钮」结论不变**。
- sheng 走的是**相反方向**：`vsync=120` + `vsync-fixed=1`（高刷）、`dpu-scanout=2`（DPU 直扫）、`fb-half=2`（half-float 直通）。
  `porting.toml:182` 说明 `dpu-scanout=2` 是让 display pipes 直接 fetch 合成 surface、flip 等 GPU fence，省掉一次 CPU 拷贝、
  消除闪烁（每帧内容变化 808 → 2），但**不减少像素数**，且要求 8-bit BGRA framebuffer；17 线当前镜像不带该参数（§2）。
- ⇒ **方案 A（降帧率）在 sheng 没有可抄的实测经验**，它只验证过 120 Hz；`getRefreshAttributes()` 低 Hz 早退的坑（§3.2）仍要自己在 17 线验。
  方案 C 的上游线索（`DPU-PORT.md` 的 `C3（1600×1068）✅`）订正判断仍然成立：那是 stride/format 验收 + dual-DSI 半屏，不是降分辨率实现。

### 7.5 熄屏发光：三级移植路径（按 17 线现状排序）

sheng 的背光实现共 844 行：`AppleARMBacklight.cpp` 468 + `BacklightProbe.cpp` 24 + `GeniI2c.c/.h` 352。
`GeniI2c.c` 只 `#include "GeniI2c.h"`、无 IOKit / PE / 中断外部依赖，是纯 FIFO 事件状态机，最容易搬；
`AppleARMBacklight.cpp` 里大量复杂度是 **27 线专属**（CoreBrightness 找服务、`AppleARMBacklight` metaclass 冒名、
backboardd sandbox grant、`brightness-nits` 16.16 转换、int-229/231/236/275/276/277/278/279 一串踩坑记录）。

| | 做法 | 适配点 / 前提 | 对 17 线的适配性 |
| --- | --- | --- | --- |
| **① 最小内核写 0（推荐先做）** | 只搬 `GeniI2c.c/.h` + 一个读/写 KTZ8866 `0x04/0x05` 的小驱动；在 blank swap（`NativeLegacyDisplay.cpp:592-613`）时写 `level=0`，唤醒写回原值 | 两个 SE 基址 `0xa80000`/`0x890000`、两个 SPI `353`/`586`、GCC vote 位图（待取证）；**可先用 polling 模式不接中断** | ✅ 不依赖 CoreBrightness / backboardd / sandbox / metaclass；**17 线当前用户态图形栈未起（§49），只有这条能立刻生效** |
| **② 完整服务** | 搬 `AppleARMBacklight.cpp` + `BacklightProbe.cpp`，`backlight=1` 启动；`backlight-control=0` 可去掉 metaclass 冒名 | `setProperties` **已兼容 iPadOS 17** 的 `brightness` + `FadePeriod`（`:403-414`），`porting.toml:184` 逐字写明「iPadOS 17 uses brightness and FadePeriod; the service accepts both」 | ⚠️ 需要 backboardd/CoreBrightness 发请求；当前没有 ⇒ **搬过来也不会自动关背光**。等用户态图形栈起来后再做，并且要处理 sandbox/AMFI 差异 |
| **③ ABL 整屏断电（备选）** | §4 旧线索：`board.toml:634-640` 的 `splash_region` 面板电源序列 | 需 ABL/DT 取证 | ⚠️ 颗粒度是整屏断电、不可运行时切换，降级为备选 |

**现成的日志接口**：17 线 `BootLog.c:74` 和 `:100` 已经 weak 声明了
`native_backlight_stats[16]` / `native_backlight_request[6]`，**但没有打印行**；
sheng `BootLog.c:750-769` 的 `L st=… req=… lvl=… a=… b=… xerr=…` 与 `L path runs=… mode=… tmo=… done=…`
可以直接搬，屏照上就能判读 level 与 I2C 错误，不用另造轮子。

### 7.6 红线与验收 `[推测]` 部分标清楚

🔴 **不要照抄 SM8550 的 `kVoteBits` 与 `kHalt[]`**。`board.toml:420` 已证 `0x52008` bit 26 在 muyu 是 S4 SPI branch，
与 sheng 的位定义不同；`board.toml:449-451` 的 stage 31「逐个 PMIC GPIO 拉高」盲写实验是前车之鉴。

第一步只允许**只读取证**，把下列值打到屏上，确认后再谈写：

1. `0x52008` 当前值、`kHalt` 对应分支的 CLK_OFF 状态；
2. 两个 SE 的 `GENI_FW_REVISION_RO` / `SE_GENI_STATUS` / `GENI_SER_M_CLK_CFG`（17 线 `TouchProbeMuyu.cpp:162-166` 已有现成读法）；
3. 从两颗 KTZ8866 读回当前 `0x04`/`0x05`（sheng `:138-145` 的读序列），得到 ABL 点亮后的亮度初值。

**验收**：blank 后读回 `level=0` 且肉眼确认背光灭（不是像素变黑）；唤醒能写回原 level；
连续 N 次无 I2C timeout / error（`native_backlight_stats[5] [13] [9]` 保持 0）；屏照含 `MUYU LIVE` 行。

**尚未验证**：本机 KTZ8866 寄存器是否与 sheng/mainline 完全一致（同 compatible 但未读回）；
`level=0` 是否真让 LED 灭（部分背光 IC 有最小电流保护，sheng 注释 `:393` 说「a lit panel never rounds down to off」指的是 nits 换算，不是芯片限制）；
se12 主 DTB 里 `status="disabled"`、由 dtbo 置 `"ok"`，XNU 启动时时钟是否仍保持；
sheng 自己也把「sleep 关背光」列为未验证（`porting.toml:186` limits / `:187` next_action）。

### 7.7 对前文的逐条订正

| 位置 | 原结论 | 订正 |
| --- | --- | --- |
| §0 第二行 | 「从未实现运行时背光控制」 | 仍然成立（17 线侧），但**补上**：sheng 有可移植实现、本机硬件同芯片，见 §7 |
| §4「上游不能直接抄」 | `uke.dts` 无节点 ⇒ 抄不了 | **漏查 sheng**；uke 确实没有，sheng 有完整驱动，本机 dtbo 有 `ktz8866@11` |
| §4「修法最值钱的线索：ABL 面板电源序列」 | 抄 ABL 比猜寄存器靠谱 | 降级为备选 ③；现在有更直接的 I2C 亮度寄存器路径 ① |
| §5 第 1 步 | 「从 stock DTB / ABL 定位背光节点」 | **已离线完成**，结果见 §7.3 |
| §6「本机具体是哪一条尚未取证」 | 三类硬件通路未定 | **已取证**：I2C KTZ8866 双驱动（SE0 `0xa80000` / SE12 `0x890000`） |
| §3 / §5 方案 A、B、C | sheng 可能另有实现 | **不变**：unit-scale 逐字同源；sheng 走 120 Hz + DPU 直扫，方向相反，无可搬的降密度实现 |

---

*两轮均为纯只读调研：未刷机、未碰设备、未构建、未改任何源码 / 镜像 / 板级配置。
唯一写操作是 `git -C UniXNU-sheng fetch origin main`（仅更新 remote ref）与 `/tmp` 下的快照/反编译产物。
`logs/flash-ledger.tsv` 末条仍是 16:24 的原厂刷入。*
