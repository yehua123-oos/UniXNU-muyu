# 停用的 launchd 进程与「只有相册/短信能开、设置打不开」

> 2026-11-12 19:5x 北京｜17 线 / 21H461 / Adreno740｜纯只读调研 + 一处安全复位开关
>
> 范围：早期为启动系统而停用的进程，与当前「只有 Photos/Messages 能开、Settings 打不开」症状的因果关系、根因、以及修法。

> ## ⚠️ 2026-11-15 20:2x 更正（GPU-HANDOFF.md §69.134）：本文前提是旧树，不适用于 fix157+
>
> 本文 §0–§5 基于「`sheng-lite=1` 每次开机把 21 项写进持久 Data 卷
> 的 `disabled.plist`」这一前提。零风险复核后确认**当前 muyu-17
> 镜像里这条链路从未生效**：
>
> 1. 生成树 `kern_exec.c:8255` 的调用点受
>    `#if defined(XNU_WATCHDOG_POLICY) && XNU_WATCHDOG_POLICY` 保护，
>    而共享 `UniXNU` 构建脚本**只定义** `XNU_BOOT_PROGRESS`（默认 0）、
>    从不定义 `XNU_WATCHDOG_POLICY`；`CONFIG_XNU_WATCHDOG_POLICY=y`
>    是无人消费的死配置。函数体被链接（`nm` 可见），调用点被编译掉。
> 2. 直接反证：fix160 日志中 `identityservicesd[55]`（清单第 1 项）
>    与 `watchdogd[53]` 都在 `==>` 启动行里带 pid 启动；若禁用生效
>    它们绝不会 launch。`launch policy` / `disabled.plist` 在
>    fix157/159/160 三轮 msgbuf 均 0 命中。
> 3. 因此 §3 的「单删参数无效、旧文件残留继续生效」、§5 的
>    `sheng-lite-reset=1` / `sheng-disable-jobs` 修复路径，对
>    **当前 kernel 都是空操作**；`sheng-lite=1` 已于 fix160
>    从 `board.toml` 删除（死参数清理）。
>
> 用户记得的 CPU 渲染时代现象**不否定**：`UniXNU-sheng` /
> `UniXNU-muyu` 的 legacy 构建脚本确实传
> `-DXNU_WATCHDOG_POLICY=$WATCHDOG_POLICY`，那时 policy 调用生效、
> plist 会被写入。换到共享树后通道断了，历史因果不能套到 fix157+。
>
> 当前「桌面/图标/设置不出现」更接近的另一条机制是
> **`sheng-staged-apps=1` 从未启用**（可删除系统 App 从未从 OS 卷
> 拷进 Data 卷，`datamigrator` 空转），见 GPU-HANDOFF.md §69.134.4。
> 本文以下保留作历史记录。

---

## §0 TL;DR

- **根因（[代码]，确定）**：`board.toml:500` 当前启用 `sheng-lite=1`，它让 `package/BootWatchdogPolicy.c` 在每次开机把 **21 个 launchd 任务**写进 `/private/var/db/com.apple.xpc.launchd/disabled.plist`，且写在**持久 Data 卷**上、每次重放开机都重放。
- 这 21 个主要是**网络 / 云 / 账户 / 蜂窝 / 蓝牙 / 照片分析 / 软件更新**类守护进程。被关掉后，依赖它们的 App 与系统服务大量失效——这是「只有相册/短信能开、设置打不开」最可能的成因（具体因果见 §3，标 [推断]）。
- **一个会让人白改的坑（[代码]，确定）**：那份禁用清单在**持久 Data 卷**上。单把 `sheng-lite=1` 从 `board.toml` 删掉，旧文件**继续残留生效**——因为 `BootWatchdogPolicy.c` 只在「带了 policy 参数」的开机才重写该文件，否则直接跳过。
- **已加的安全修复（[代码]）**：`BootWatchdogPolicy.c` 新增一次性开关 `sheng-lite-reset=1`，本次开机写**空 override 字典**（清空全部禁用）。默认零行为，不影响现有任何构建。
- 真正恢复全部 21 个有**启动卡死风险**（这是当初停它们的原始原因，见 §4），所以建议先走**子集恢复**或**一次性 reset + 观察**。

---

## §1 根因：sheng-lite=1 禁用 21 个 launchd 任务

`board.toml` 的 `kernel_args` 里当前有一行（未注释）：

```
"sheng-lite=1",
```

`BootWatchdogPolicy.c` 的解析（`sheng_boot_watchdog_policy()`）：

- `PE_parse_boot_argn("sheng-lite", &lite, …)`；`lite==1` 时遍历 `lite_jobs` 字符串，把每个 label 作为 `<key>…</key><true/>` 写进 policy。
- policy 落到 `/private/var/db/com.apple.xpc.launchd/disabled.plist`，文件用 `O_TRUNC` 每次重开重写。
- 该文件在 **`/private/var`（持久 Data 卷）**，不是 RAM disk，所以跨重启保留。注释明写 *"With persistent Data the previous boot's file is there: this policy is rewritten in place on every boot"*。

也就是：从这份配置构建出的任何镜像（`nofs1` / `tier1d_nopost` / `upper1` 等都读 `board.toml` 的 `kernel_args`），开机都会把下面 21 个任务禁用。已用二进制直搜确认 `upper1` 携带 `sheng-lite=1`。

### 被停用的 21 个任务（`BootWatchdogPolicy.c:26-34` 的 `lite_jobs`）

| # | label | 类别 |
|----|-------|------|
| 1 | `com.apple.identityservicesd` | 账户 / iMessage/FaceTime |
| 2 | `com.apple.locationd` | 定位 |
| 3 | `com.apple.CommCenter` | 蜂窝 / 基带 |
| 4 | `com.apple.bluetoothd` | 蓝牙 |
| 5 | `com.apple.photoanalysisd` | 照片分析 |
| 6 | `com.apple.mediaanalysisd` | 媒体分析 |
| 7 | `com.apple.mediastream.mstreamd` | 照片流 |
| 8 | `com.apple.icloud.searchpartyd` | 查找网络 |
| 9 | `com.apple.findmy.findmybeaconingd` | 查找 |
| 10 | `com.apple.familycircled` | 家人共享 |
| 11 | `com.apple.financed` | 金融/钱包 |
| 12 | `com.apple.softwareupdateservicesd` | 软件更新 |
| 13 | `com.apple.ndoagent` | 网络诊断 |
| 14 | `com.apple.biomesyncd` | 健康数据同步 |
| 15 | `com.apple.email.maild` | 邮件 |
| 16 | `com.apple.amsengagementd` | App Store 参与 |
| 17 | `com.apple.adid` | 广告标识 |
| 18 | `com.apple.eyereliefd` | 屏幕距离/护眼 |
| 19 | `com.apple.fairplayd.A2` | FairPlay（KeepAlive，CI 里崩溃循环） |
| 20 | `com.apple.passd` | 密码/钥匙串 |
| 21 | `com.apple.mobile.obliteration` | 数据擦除（本机无用） |

> 注意：核心系统守护进程（`cfprefsd`、`configd`、`notifyd`、`distnoted`、`securityd`、`backboardd`、`SpringBoard` 等）**不在**该列表内，仍正常启动。所以系统能进桌面，但大量上层功能缺依赖。

---

## §2 为什么「只有相册/短信能开、设置打不开」（[推断]，待真机验证）

这是基于上述清单与 iOS 依赖关系的推断，不是 100% 静态证明——**真正验证只能靠恢复后观察**。

- **Settings（设置）打不开**：`Preferences.app` 在启动时会加载各功能面板的偏好包，并向其背后的守护进程发 XPC。本机被关掉的正好是 `bluetoothd`（蓝牙面板）、`CommCenter`（蜂窝面板）、`locationd`（隐私/定位面板）、`identityservicesd`（Apple ID/信息面板）。这些 backing daemon 不在，Settings 在加载对应面板时很可能直接崩溃或卡死在 launch——表现为「设置打不开」。
- **大多数 App 也打不开**：上述 21 个里一半是网络/云/账户/同步类，绝大多数第三方与系统 App 启动时会尝试连通 iCloud、账户、定位或网络，缺这些 daemon 后 fail/hang。
- **相册（Photos）和短信（Messages）还能开**：两者核心功能可以离线降级——相册读本地图库、短信收本地 SMS/已下载消息，不强制依赖被关的云/账户 daemon，因此能 launch。符合「只活下来这两个」的现象。

> 置信度：机制（§1）是 [代码] 确定；因果链（本节）是 [推断]。如果恢复后设置仍打不开，说明还有第二因（例如 GPU/Metal 渲染未接管导致重合成型 App 崩溃，见 `GPU-ONDEVICE.md §13` 的「桌面是 CPU copy」），需另行排查。

---

## §3 关键坑：单删 board.toml 参数不会恢复（[代码]，确定）

`BootWatchdogPolicy.c` 早期返回逻辑：

```c
if(!enabled && !jobs[0] && !lite && !have_mask && !lite_reset)return 0;
```

只要**没有任何 policy 参数**（`sheng-disable-watchdogd` 之外），函数直接 `return 0`，**完全不碰** `disabled.plist` 文件。

后果：

- 当前文件里已经写死了 21 个 `<true/>` 禁用键。
- 把 `sheng-lite=1` 从 `board.toml` 删掉 → 下次开机 `lite=0`、无 policy 参数 → 走 early-return → **旧文件原封不动继续禁用那 21 个**。
- 所以任何「恢复」动作都必须**主动重写该文件**，而不是删参数。

这也是为什么 `BootWatchdogPolicy.c` 里提供了两套带值参数（`sheng-disable-jobs=…`、`sheng-lite-mask=…`）和一个新加的一次性 `sheng-lite-reset=1`——它们都是「带 policy 参数 → 触发重写」的路径。

---

## §4 当初为什么要停它们（[代码]+[实测]，理解风险）

`board.toml:491-494` 注释：

> the boot stops creating processes at ~75 s, with mediaanalysisd/photolibraryd/photoanalysisd/softwareupdated among the last ones on an empty photo library and no network. That is exactly the batch sheng disables for bring-up … no boot task ever completed past them.

`sheng` 上游的隔离理由（`BootWatchdogPolicy.c:19-20`）：

> hardware stacks show identityservicesd, contextstored and locationd in a cycle of synchronous XPC waits. A disabled job makes its Mach service lookups fail instead of hang.

即：这批 daemon 在**全新 Data 首次开机**时会和 SpringBoard 抢 CPU/内存，且其中 `identityservicesd`/`locationd` 曾在同步 XPC 等待环里导致启动卡死。禁用它们是为了**先让系统跑起来**。

**风险**：恢复它们（尤其 `identityservicesd`/`locationd`）可能让开机重新卡在 ~75 s。这就是为什么不能无脑全开。

---

## §5 修法（三条路径，按风险从低到高）

### 路径 A：一次性清空（最干净，但有全开卡死风险）

给镜像加一次性参数（或临时在 `board.toml` 加一行后重建）：

```
"sheng-lite-reset=1"
```

- 本次开机 `BootWatchdogPolicy.c` 写**空 override 字典**（不含任何 `Disabled` 键）→ 21 个全部恢复默认（启用）。
- 之后不带任何 policy 参数开机 → early-return → 空文件保留 → **持续全开**。
- 若开机卡在 ~75 s → 先回退到 `sheng-lite=1`（全禁、已知能启动）恢复开机，再改用 `sheng-lite-mask` 按位二分（bit0=identityservicesd）只保留真正卡死的那个 daemon 禁用；不要直接跳路径 B，因为 B 同样会启用那四个嫌疑 daemon。

### 路径 B：子集恢复（推荐先试，但四个嫌疑 daemon 仍会被启用）

只重新启用 Settings 最需要的 4 个（蓝牙/蜂窝/定位/账户类），其余 17 个重型云同步/分析类继续禁用。把 `board.toml` 的 `sheng-lite=1` 换成：

```
"sheng-disable-jobs=com.apple.photoanalysisd,com.apple.mediaanalysisd,com.apple.mediastream.mstreamd,com.apple.icloud.searchpartyd,com.apple.findmy.findmybeaconingd,com.apple.familycircled,com.apple.financed,com.apple.softwareupdateservicesd,com.apple.ndoagent,com.apple.biomesyncd,com.apple.email.maild,com.apple.amsengagementd,com.apple.adid,com.apple.eyereliefd,com.apple.fairplayd.A2,com.apple.passd,com.apple.mobile.obliteration"
```

即**去掉** `identityservicesd`/`locationd`/`CommCenter`/`bluetoothd` 四个。下次开机 policy 重写为这 17 个，四个被恢复 → Settings 的蓝牙/蜂窝/定位/账户面板有 backing daemon。

- 注意（修正）：这 4 个里就包含当初 XPC 等待环的元凶 `identityservicesd`/`locationd`，它们**会被重新启用**，所以"启动卡死"的风险并没有被避开。路径 B 相对路径 A 唯一的优势是：少启用 17 个重型任务，整体 CPU/内存压力更小，卡死概率可能低一些，但不是零。
- 仍残云同步类，但本机无账户/网络，它们空转无害。
- 路径 B 已经把四个嫌疑 daemon 重新启用；若 Settings 仍打不开，说明问题不在它们（可能是 `wifid`/`backboardd`/`SpringBoard` 侧，或某个不在 21 之列的依赖），应去查那些，而**不是**继续盲目启用更多任务。

### 路径 C：保留 sheng-lite，单独补 enable（不推荐）

launchd 的 `Disabled` 键只有「禁用」语义；要显式启用需把键写成 `<false/>`。当前 `BootWatchdogPolicy.c` 只写 `<true/>`。除非扩展代码，否则无法「在禁用全集里挑几个启用」。所以优先用 B/A。

---

## §6 当前状态与红线

- 设备此刻在 **fastboot**（`a20248f7`），归接手 GPU 的 AI 调度；本调研**纯只读 + 一处安全代码新增**，未刷机、未重建镜像、未改 GPU 工作树。
- `logs/flash-ledger.tsv` 末条仍为 `11:21:45Z boot rc=0`（`init1` 引导）。
- 🔴 **改动生效需要一次 payload 重建 + 刷机**，且和 GPU 主线的刷机窗口重叠——请与接手 GPU 的 AI 协调，不要各自独立刷。
- 🔴 改 `board.toml` / `BootWatchdogPolicy.c` 前先 `git diff` 确认只动了目标处；`sheng-lite-reset` 是新加的 opt-in 开关，默认行为完全不变。
- 🔴 回退锚点始终是 `fw/vendor_boot_nofs1.img`（`c7635cdb…`）+ `MUYU_UEFI=fw/boot-17line-043f7120.img`，见 `GPU-ONDEVICE.md §4/§12`。

---

## §7 已落地改动

- `package/BootWatchdogPolicy.c`：新增 `sheng-lite-reset=1` 解析与 early-return 条件 `&& !lite_reset`，并在 `lite_reset` 时写空 override 字典（含头部注释说明持久化坑）。默认零行为。
- `board.toml`：`sheng-lite=1` 下方新增注释，给出 reset / disable-jobs 子集两条现成改法及卡死回退提醒。
- 报告本身：`UniXNU-muyu-17/docs/LAUNCHD-DISABLED.md`（本节）。
