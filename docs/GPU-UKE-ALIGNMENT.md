# GPU：对齐 uke 已验收的 iPadOS 27 基线（2026-11-04，离线）

> 用途：把"muyu 的 GPU 现在在哪、和 uke 那条被用户验收的线差什么、按什么顺序补"写死在一处。
> 本文只记录**核实过的事实**与**待办**；任何"应该能行"的推断都标成假设。

## 1. 基线：uke `c7aefb9`（"Promote the user-accepted iPadOS27 GPU build"）

uke 在 2026-11-03 把 iPadOS 27 的 GPU 栈跑成并验收（baseline CRC `90B6BC70…`）。它的 pin 表在
`feeds.toml` 里，2026-11-04 从上游直接核对（不是从记忆里抄）：

| 角色 | 仓库 | commit | 说明 |
| --- | --- | --- | --- |
| GPU 驱动（我们的 `drivers/Adreno740`） | `https://github.com/Allenwdk/NativeAdreno.git` | `22abfe4` | 上游 main `6fa7bae` + **gen7_11_0 profile** + 有界失败记录 |
| 显示（我们的 `drivers/GopFramebuffer`） | `https://github.com/Allenwdk/NativeMobileDisplay.git` | `f3e29a2` | 上游 main `e47c1b8` + 24A446 requested-state 修复 |
| Metal 前端（我们的 `drivers/AdrenoMetal`） | `https://github.com/Allenwdk/AdrenoMetal.git` | `854f99a` | 上游 main `243d63c` + iOS 27 Core Image 库的条件重试 |
| SoC 层 | `https://github.com/Allenwdk/UniXNU-sm7675-common` | `7f857a8` | SM7675 共享层；muyu 目前**没有**这一层 |
| 核心 | `UniXNU/UniXNU` | `899fdcb` | uke 构建用的核心（我们当前 `44cf475`，落后 1627 行） |
| UEFI | uke 自己的 Silicium（`8a1153e`） | | 我们用的是 Mu-Silicium `muyuPkg` |

**关键事实：`22abfe4` 里 `NativeAdrenoProfileTable` 已经有 `0x43030b00` 这一条**（uke 的 A732 与我们的
A735 是**同一个 chip id**），所以"给 A735 单独加 profile 条目"这件事**不需要做** —— 前提是 OPP 表由板级
输入提供，而不是抄 profile（见 §3）。

## 2. 我们现在的状态

| 项 | muyu 现状 |
| --- | --- |
| 驱动 pin | `drivers/Adreno740` `a9a2cbb`、`drivers/AdrenoMetal` `a0f0287`、`drivers/GopFramebuffer` `19ef3a7`（全部是 uke 基线的**祖先**） |
| 内核里有 GPU 驱动吗 | **构建链已接通（2026-11-10）**：`board.toml [kernel] archives` 现含 4 条 `gen-muyu-gpu-*` 生成器 + `build-arm-driver.py Adreno740`，跑完产出 `libAdreno740.a`；`configs/muyu-userland_defconfig` 已置 `CONFIG_XNU_NATIVE_ADRENO=y`。但**还没烧进默认镜像**（§5 启动确定性未关，按 §5 规矩要等 boot census ≥4/5 进桌面再做独立 vendor_boot），设备端也未验。 |
| `[gpu]` 表 | **本次已加**（生成式，见 §4） |
| ADT `gmu-memory` 节点 | ✅ 已加（`make-muyu-adt.py` 顶层节点，`reg` = `0xc8000000`/`0x2000000`，与 UEFI、[gpu] 三处一致，2026-11-10） |
| UEFI `GMU Pool` 预留 | ✅ 已加（Mu-Silicium `muyuPkg/Library/MemoryMapLib/MemoryMapLib.c`，`GMU_Pool` `0xC8000000`/`32MiB`，2026-11-10） |
| GPU 启动参数 | 缺（`sheng-gpu-power=9` 等一条都没有） |
| GPU addon（Metal 插件等） | 核心**支持** `--gpu-addon`（`payload.py`/`make-gpu-addon.py` 都在），但 muyu 从来没打过 |
| 显示路径 | `drivers/GopFramebuffer/NativeLegacyDisplay.cpp` CPU 逐帧 `memcpy`，~1.4 fps |
| 固件 | `fw/gpu-firmware/gen71100_{sqe.fw,gmu.bin,zap.mbn}` 已从设备抽出；SQE/GMU 与 uke **逐字节相同**，ZAP 不同（`463ba9b7…` vs `12949928…`） |

## 3. 差异清单（按依赖顺序）

1. **板级输入**（本次完成一半）：`[gpu]` 表 + 芯片级常量。剩下的两个数字是**假设**：
   `gmu_core_clock_hz=220 MHz`、`gmu_hub_clock_hz=200 MHz`、`gmu_pool_base/bytes=0xC8000000/32 MiB`
   —— 这些是 uke 的 A732 值（同一个 GPUCC/GCC），不是我们量出来的，表里已注明。
2. **构建链移植**（✅ 已完成，2026-11-10 离线验证）：把 uke 的 `component:Adreno740` + 共享
   `kits/{ArmSmmu4K,AdrenoPM4,AdrenoFirmware}` 接进 17 线内核构建。我们这边没有 `component:` 语法，
   改用 `board.toml [kernel] archives` 串起四条生成器 + `build-arm-driver.py Adreno740`：
   `gen-muyu-gpu-platform.py` → `gen-muyu-gmu-source.py` / `gen-muyu-gmu-votes.py` /
   `gen-muyu-gpu-smmu-source.py`（读 muyu **自己**的 DTB + `~/pad7pro-xnu/fw` 收获物，生成
   `build/gpu/ShengGmu{Source,VotesSource,CommandDbSource}.h` + `ShengGpuSmmuSource.h` +
   `MuyuGmuFirmware.c`；GPU OPP 表是 muyu 的 11 档 @1100 MHz，**不是** uke 的 9 档 @950 MHz）。
   再交给 `build-arm-driver.py Adreno740` 做 unity build：`-include kits/muyu/MuyuGpuRoleCompat.h`
   并把 `MuyuGpuRoleCompat.c`（C）编进同一个 `libAdreno740.a`。21H461 缺 `task_get_gpu_role()`，
   由上述 compat 层补齐（§7.1）。
   **验证**：从组装树跑完整链 → `Adreno740: -Werror build and actual ARM64/iOS 17 object audit
   passed`；产出 `build/native-adreno/libAdreno740.a`（含 `Adreno740.o` + `MuyuGpuRoleCompat.o`，
   后者为 ARM64 object、零 `task_get_gpu_role` 未定义符号）。生成器与构建脚本均已做 tree-safe
   （`board_root()`/`fw_root()` 按脚本实际落点解析，不再写死 `parents[2]` 与 `ROOT/UniXNU-muyu`，
   所以 `tree.py` 不论从主仓还是 `UniXNU-muyu-17` worktree 组装都能编过）。
   **注意**：uke 的 `build-native-legacy-display-27.py` 是 24A446 的显示后端，我们跑 21H461，
   这份要按我们的 `IUISurface`/`IOMobileFramebuffer` ABI 重新对。机制已离线枚举（2026-11-10）：
   生成器随 21H461 自动正确，验证段 17 处 24A446 硬编码地址需重指——详见 §3.7。
3. **ADT `gmu-memory` 节点**（✅ 已完成，2026-11-10）：`make-muyu-adt.py` 顶层加 `node('gmu-memory', [('reg', pack('<QQ', 0xC8000000, 0x2000000))])`，
   与 `make-uke-adt.py:225` 同形。解码确认 `reg` = `0xc8000000`/`0x2000000`。
4. **UEFI `GMU Pool` 预留**（✅ 已完成，2026-11-10）：`Mu-Silicium/Platforms/Xiaomi/muyuPkg/Library/MemoryMapLib/MemoryMapLib.c`
   加 `{"GMU_Pool", 0xC8000000, 0x02000000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN}`，
   落在 `DXE_Heap`(→0xC3A00000) 与 `Display_Demura`(0xD3A00000) 之间的 2–4 GiB 空洞，属性对齐同文件
   `TZApps_Reserved`/`PIL_Reserved`（DRAM reserved 区写法）。与 ADT 节点、`[gpu] gmu_pool_*` 三处一致。
   ⚠️ 仍是**假设地址**（uke 的 A732 值，不是 muyu 量出来的）：已用 muyu 自己的 `dtb_3.dtb`
   `/reserved-memory` 全表核对，该 32 MiB 区间无任何 reserved 占用；但刷机前还需再对 UEFI 实际 carved 区间
   （UEFI 自身 / TZ / hyp 的静态划区有时不进内核 DTB）做一次交叉确认。
5. **启动参数**：`sheng-gpu-power=9`、`sheng-gpu-metal=1`、`sheng-gpu-preempt=2`、`sheng-gpu-dcvs=1`、
   `sheng-fb-gpu=1`、`sheng-gpu-addon=1`（board.toml boot-args 有 ~1000 字节上限，加之前先数）。
（**§3.5 解析 + 本机实测，2026-11-11 终版**：这些 `sheng-*` 由 `cli.py --kernel-arg`（可重复）注入
`board.toml [payload] kernel_args` 生成的 cmdline；`payload.py check_args()` 有 `len(line) > 1000` 硬闸。
本机 `tomllib` 实测：离线基线 **45 参 / 934 B**；纯加 Tier-1 4 参 = **1004 B ❌**（第十轮构建实测报
`the kernel command line is 1004 bytes; the limit is 1000`）；临时去掉
`offline-mobile-graphics-client=1` / `offline-mobile-graphics-load=1` 后 Tier-1 = **940 B ✅**（47 参，本次首测采用），
全部 6 参 = **976 B ✅**。`--kernel-arg` 只能加不能删，所以去参必须临时改 `board.toml`、用完**还原**（默认
`board.toml` 与默认 `fw/vendor_boot_btn2.img` 始终保持离线基线 = 可靠回退，§5 红线据此守住）。
GPU 参数只进独立镜像 `fw/vendor_boot_gpu_tier1_adtfix.img`；`-o` 是 `bootimg` 的输出参数，与 cmdline 注入无关。
详见 `docs/GPU-ONDEVICE.md` §2/§4/§6。）
6. **gpu addon**：`make-gpu-addon.py` + owner trust cache（`--owner-trust-cache`）。
   （**§3.6 定位，2026-11-11**：addon 属 **Metal/IOAccel 插件层（Tier-2）**。
   `make-gpu-addon.py` 在核心 `UniXNU/scripts/`，`--gpu-addon` / `--owner-trust-cache` 由 `cli.py` 接收。
   ⚠️ uke 的 `OWNER_TRUST` 路径写死在 allenwdk 机器（`/home/allenwdk/UniXNU/uke-port/data/owner-trust-cache.bin`），
   muyu 需自己产出 owner trust cache。Tier-2 **不在首次上机关键路径**，暂缓。
   **【2026-11-11 续改纠正】**：`AdrenoMetal` **不是空 submodule、无需拉取**——已 checkout @ `854f99a`，含完整源码；
   IOAccel 基础 ABI 预置 `scripts/data/ioaccelerator-base-abi.json` 的 4 个入口指令 sha 在 21H461 候选上
   逐一 MATCH（= 21H461 对齐），`build-adreno740-accelerator.py` seed 机制无需 QEMU、已实跑 PASS。
   真正待办是"把 accelerator 脚本接进 board.toml [kernel] archives + addon/trust cache + QEMU 完整验证"。详见
   `docs/GPU-ONDEVICE.md` §7。）
7. **ZAP**：我们自己的 `gen71100_zap.mbn`（`463ba9b7…`），不要用 uke 的。

### 3.7 显示后端 ABI 重对（离线枚举，2026-11-10）

显示后端 = `drivers/GopFramebuffer/NativeLegacyDisplay.cpp`（继承 `IOMobileFramebufferLegacy`）。
它的 ABI 契约头 `OriginalLegacyFramebufferABI.h` **不是 vendored 文件，而是 `build-native-legacy-display.py`
在构建期生成的**（`OUT/'OriginalLegacyFramebufferABI.h'`）。生成器先跑 `audit-mobile-graphics-legacy.py` 对
**候选内核**做**静态**校验（读 `native-owner-mobile-graphics-candidate.macho` 的 `IOMobileFramebufferLegacy` 家族
文本 sha + vtable 槽位/实例尺寸 `0x18a0`），据此写头，再 `build-arm-driver.py NativeLegacyDisplay` 编译。
**不需要 QEMU**——之前"QEMU gdb-stop 抓 vtable"是文档误判，实际实现是二进制静态核对（与 `audit-mobile-graphics-default-surface.py`
同机制）。所以**头本身随目标内核自动正确**（只要 audit 吃的是目标内核候选）。

**24A446 写死点（✅ 已收敛，2026-11-11）**：原先 `build-native-legacy-display.py` 验证段（`bindings` 字典 +
`require` 指令断言，约 113–175 行）写死了 **17+ 处** 24A446 内核虚地址（`0xfffffff0057865cc` 等）。现已全部抽成
顶层 `TARGET_ABI` 字典（本板 24A446 默认值），并支持由 audit JSON 的 `target_abi` 键**整体覆盖**：
`TARGET_ABI.update({k: int(v,16) for k,v in audit.get('target_abi',{}).items()})`。**重对 21H461 = 用 21H461 候选
重产 `mobile-graphics-legacy-abi.json`（含新 `target_abi` 地址 + 新家族文本 sha），脚本无需再改代码**。

**显示后端实际触碰的 ABI 面（21H461 必须对）清单**：

- 🟢 `IOMobileFramebufferLegacy` vtable 槽位映射：由生成器从目标内核 audit 产出，自动正确；验证闸门地址已收敛为
  `TARGET_ABI`（可被 audit JSON `target_abi` 覆盖），**不再需要改代码**。
- 🟡 `IOPMPowerState.version`：后端设 `=1`（`NativeLegacyDisplay.cpp:178`）。XNU 该字段长期为 1，21H461 大概率兼容；
  编译期按 21H461 `IOPM.h` 检查即可。
- 🟡 `PE_Video`：后端读 `v_baseAddr`/`v_rowBytes`/`v_height`（`pexpert.h`）。21H461 这三个字段均为 `unsigned long`，
  按名访问，**编译期随 21H461 pexpert.h 自动归位**，低风险。
- 🟢 `StateRequest` 结构体（`NativeLegacyDisplay.cpp:156`，requested-state 载荷）：偏移布局需与 21H461 PM 外部
  方法参数一致，属小结构体核对。
- 🟢 `metaCast("IOSurface")` / `metaCast("IOMemoryDescriptor")` / `getProperty("dot-pitch")`：按字符串匹配
  metaclass/属性，**不依赖 vtable 偏移**，鲁棒。
- 🟢 标准 `IOService`/`IOMemoryDescriptor`/`IOMemoryMap` API（`map`/`getVirtualAddress`/`getLength`/
  `mapDeviceMemoryWithIndex`）：稳定。

**结论（2026-11-11 实跑修正）**：显示侧 ABI 重对的代码底座已完成——`build-native-legacy-display.py` 的目标内核
地址全部 `TARGET_ABI` 化，可由 audit JSON `target_abi` 覆盖；缺失的 `audit-mobile-graphics-legacy.py` 已补写。
**在组装树直接实跑 `build-native-legacy-display.py` → EXIT=0**（`legacy graphics audit: candidate d15dffa81852
matches, 358 methods qualified` / `NativeLegacyDisplay: -Werror build and actual ARM64/iOS 17 object audit passed` /
`Built genuine original Legacy inheritance/dispatch`）。原因：`IOMobileFramebufferLegacy` 是冻结老类，方法体在
iOS17(21H461) 与 iOS26(24A446) 间二进制一致，故 `target_abi=None` 回退的 24A446 地址 + 指令校验门在 21H461
候选上全部吻合 → **§3.7 不是首测阻塞项，无需重产 target_abi JSON**。
IOAccel 侧同理已就绪、且**不是空 submodule**（纠错见下）：`AdrenoMetal` 已 checkout @ `854f99a` 含完整源码，
预置 `scripts/data/ioaccelerator-base-abi.json` 的入口指令 sha 在 21H461 候选上逐一 MATCH，
`build-adreno740-accelerator.py` seed 机制无需 QEMU、实跑 PASS。详见 `docs/GPU-ONDEVICE.md` §7。

## 4. 本次已完成（可复查）

- `scripts/data/adreno-profiles.json`：把 `NativeAdreno@22abfe4` 的
  `NativeAdrenoProfile.cpp` 里 **A740 与 A732 两条 profile 逐字段抄下来**，带仓库/commit/文件 sha256
  （`6d71ee96…`）。这是"六个不知道的字段"的合法来源：它们是**按 chip id 索引的芯片级常量**，不是板级
  测量值，所以同 chip id 的 muyu 可以直接用；`memory` 里那六个字段仍然保持 `null`（那是 DTB 派生区，
  DTB 没有就是没有）。
- `scripts/derive-muyu-gpu-inputs.py`：把这份 pin 合进记录，并做**交叉校验** ——
  `gmu_gpu_delta` 必须等于 DTB 的 `gmu_base - gpu_base`（实测 `0x68000` ✓），`ubwc_version` 必须等于
  DTB 的 `qcom,ubwc-mode`（4 ✓），firmware 名字必须是我们这几个文件。
- 🐛 **修掉一个真 bug**：`smmu_stream()` 原来取 `iommus` 的**最后一格**（mask），于是
  `sid_user/sid_secure/sid_gmu` 全被算成 `0`。真实绑定是 `<&apps_smmu SID MASK>`，SID 在 phandle 之后
  那一格：**0 / 2 / 5**，与 uke 完全一致。这个 bug 如果不修，SMMU context 会用 0 号 stream 建起来。
- `scripts/gen-muyu-gpu-table.py`：从记录生成 `board.toml` 的 `[gpu]` 表（uke 的键名），
  `--check` 验漂移、`--write` 重写块。`check-muyu-gpu-inputs.py` 现在 27 条断言全绿（含表漂移检查）。
- 🟢 **§3.2 构建链已接通并离线绿（2026-11-10）**：`board.toml [kernel] archives` 串起 4 条
  `gen-muyu-gpu-*` 生成器 + `build-arm-driver.py Adreno740`；`configs/muyu-userland_defconfig`
  置 `CONFIG_XNU_NATIVE_ADRENO=y`。`build-arm-driver.py` 修了 Adreno740 include 块（去掉不存在的
  `kits/braveark` / `build/sheng-gmu`，改加 `-I build/gpu`、`-I kits/muyu` 并 force-include
  `MuyuGpuRoleCompat.h`），并在 archive 尾部把 `MuyuGpuRoleCompat.c` 以 C 编进同一 `libAdreno740.a`
  （在 C++ 配方上就地 `-xc++`→`-xc`、丢 C++ 专属 `-std=`/`-fapple-kext`）。四条生成器与
  `build-arm-driver.py` 均已做 tree-safe（`board_root()`/`fw_root()`），主仓与 `UniXNU-muyu-17`
  worktree 两份脚本现已对齐。完整链跑通：`-Werror` 编译 + ARM64/iOS 17 实物体审计通过，产出
  `build/native-adreno/libAdreno740.a`（含 `Adreno740.o` + `MuyuGpuRoleCompat.o`，后者零
  `task_get_gpu_role` 未定义符号）。详见 §3 第 2 项。
- 🟢 **内核链接闭环（2026-11-11，第八/九轮构建）**：第一次完整 `build all` 在 `ld` 阶段暴露
  **41 个 undefined** —— 27 线由独立 `build-adreno740-kits.py` 归档的 `NativeAdreno*.cpp` C++ kits
  在 17 线没有对应 manifest 项，另有 `libBravearkLastLog.a` / `libMuyuThermal.a` /
  `libMuyuUsbDebug.a` 三个设备 archive 因改用核心版 `build-xnu-4k-baseline.sh` 而漏链。修法：
  ① `build-arm-driver.py Adreno740` 把 28 个 kits C++ TU（排除 Client/Service/Accelerator）
  折进同一个 `libAdreno740.a`，`CONFIG_XNU_NATIVE_ADRENO` 一次 `-force_load` 即可；
  ② `build-xnu-4k-baseline.sh` 恢复"文件存在就链接"的三个设备 archive 循环（bootstrap 前无 archive
  也安全）。第二轮 `ld` 只剩 **4 个**：`AdrenoPM4Type4/7`（PM4 编码器，legacy display 只编了
  GMU/HFI/SMMU C kits）与 27 线 XNU patch 专属的 `native_gpu_dcvs` /
  `native_pmap_user_mappings`（21H461 无）。补 `AdrenoPM4.c` 与两个惰性 weak 定义
  （对齐 27 线 `tests/HostKernelHooks.cpp`：DCVS 关、pmap 反查返回 0）后，
  41 + 4 个符号逐个 `nm` 比对全部有定义。注意 `native_gpu_dcvs[1]` 通过数组地址无条件读取，
  **不能只靠 `-U` 留成未定义**，否则运行时访问地址 0。
- 🟢 **§3.3 + §3.4 GMU pool 三处对齐（2026-11-10）**：`[gpu] gmu_pool_*`（0xC8000000/32MiB）→
  `gen-muyu-gmu-source.py` 吐 `SHENG_GMU_POOL_BASE` / `SHENG_GMU_POOL_BYTES` / `SHENG_GPU_POOL_BASE`
  （GPU 区在池内 +0x1600000，符号名与 uke `prepare-uke-gmu.py:544` 一致，从 `board.toml [gpu]` 读取）→
  `make-muyu-adt.py` 顶层 `gmu-memory` 节点（`reg` 0xc8000000/0x2000000，已解码确认）→
  `Mu-Silicium .../MemoryMapLib/MemoryMapLib.c` 的 `GMU_Pool` 预留（属性对齐 `TZApps_Reserved`）。
  重跑生成器+`build-arm-driver.py Adreno740` 仍绿，`ShengGmuSource.h` 已含三连定义。地址仍标假设：
  已用 muyu `dtb_3.dtb` 的 `/reserved-memory` 全表核对无冲突，刷机前需再对 UEFI 实际 carved 区间。
- 🟢 **§3.7 显示后端 ABI 重对底座（2026-11-11）**：补写缺失的 `audit-mobile-graphics-legacy.py`（静态读候选
  macho + 家族文本 sha 校验，**不碰 QEMU**——纠正了文档里"需 QEMU gdb-stop"的误判）；把 `build-native-legacy-display.py`
  验证段 **17+ 处 24A446 硬编码内核虚地址**全抽成顶层 `TARGET_ABI` 字典（本板 24A446 默认），并支持由 audit JSON 的
  `target_abi` 键整体覆盖。整链离线重跑 **PASS**（`NativeLegacyDisplay: -Werror build and actual ARM64/iOS 17
  object audit passed` → `Built genuine original Legacy inheritance/dispatch`）。
  **【2026-11-11 续改纠正】**"剩下唯一动作 = 用 21H461 候选重产 `mobile-graphics-legacy-abi.json`"的判断已被
  实跑推翻：直接在组装树跑 `build-native-legacy-display.py`（`target_abi=None` 回退 24A446 地址）在 21H461
  候选上 EXIT=0 通过，因老类方法体跨版本二进制一致、所有指令/地址校验门吻合。**§3.7 无需重产 JSON、
  不是首测阻塞项**；`TARGET_ABI` 化仍保留（未来换内核可直接覆盖）。

## 7. 可行性探针（2026-11-04，离线编译）

在把 pin 换掉之前先问了一个更便宜的问题：**uke 验收的那份 NativeAdreno 源码，能不能在我们的
21H461 内核树上编过？** 做法：拿本树 `IOPlatformExpert.cpo.json` 的编译配方，把 `/tmp/na`
（`22abfe4`）里全部 `NativeAdreno*.cpp` 逐个 `-fsyntax-only`。

结果：**25 / 33 通过**，失败恰好三类，都是可以定位的：

| 失败 | 文件数 | 原因 | 处理 |
| --- | --- | --- | --- |
| `ShengGmu.h` 找不到 | 4 | 新驱动仍要 `kits/*/ShengGmu.c` 那套 + `build/sheng-gmu/` 生成头 | 移植 `prepare-muyu-gmu.py`（写 `ShengGmu.h` / `ShengGmuSource.h`） |
| `../AdrenoMetal/OriginalAcceleratorABI.h` 找不到 | 1 | `build-adreno740-accelerator.py` 要兄弟目录里的 Metal ABI 头 | 换 pin 时 AdrenoMetal 必须**同一个提交级**跟上 |
| `task_get_gpu_role` / `PRIO_DARWIN_GPU_BACKGROUND` / `_UTILITY` 未声明 | 1 | 这是 **iOS 26/27 的 API**，21H461 没有 | ✅ 已用 `package/MuyuGpuRoleCompat.{h,c}` 解决（见下） |

（另外两个文件是 host 单元测试，要 `cassert`，本来就不进内核。）

### 7.1 17.7.11 的 GPU role 兼容层

驱动在 `NativeAdrenoUserClient::submitPriority()` 里读"这个进程的 GPU role"来决定把两个 Metal 队列
优先级中的哪一个交给 ring。21H461 没有这个 API，但同样的信息存在**任务策略属性**里
（`TASK_POLICY_ROLE` → `proc_task_role_to_darwin_role()` → `PRIO_DARWIN_ROLE_*`，见
`bsd/kern/kern_resource.c` 的 `proc_get_darwin_role()`）。所以兼容层是**读取**既有字段，而不是造值：

- `package/MuyuGpuRoleCompat.h`：给 C++ 的声明 + 两个新常量名（映射到 `PRIO_DARWIN_ROLE_NON_UI` /
  `UI_NON_FOCAL`，**这是假设**，只在选优先级时有影响，不影响正确性）；
- `package/MuyuGpuRoleCompat.c`：C 侧实现，`proc_get_task_policy(task, TASK_POLICY_ATTRIBUTE,
  TASK_POLICY_ROLE)` → 转成 `PRIO_DARWIN_ROLE_*`。必须分离成 C，因为 `kern/task.h`、
  `kern/policy_internal.h` 进不了 C++ TU（`ledger.h:151` 把 `template` 当参数名）。

已验证：`MuyuGpuRoleCompat.c` 用本树 C 配方编过；`NativeAdrenoUserClient.cpp` / `Submit` /
`Publish` / `Ring` / `Gpu` 带上 `-include MuyuGpuRoleCompat.h` 后**全部 0 error**（之前 UserClient 报 5 个
未声明）。接进构建时，kit builder 要给这 5 个 TU 加 `-include`，并把 `.c` 编进同一个 archive。

## 5. 风险 / 不要做的事

- **uke 的验收是 iPadOS 27（24A446），我们跑 17.7.11（21H461）**。驱动内核侧（GMU/CP/SMMU/ring）跟
  系统版本关系不大，但 **Metal 插件与 display 后端是按 24A446 的 ABI 对的**，不能假设 21H461 直接吃下。
  真要做 Metal，得按 17.7.11 的 `NativeMetal`/`MobileDisplay` ABI 重新验。
- **OPP 表不能抄**：uke 9 档顶 950 MHz，我们 11 档顶 1100 MHz，`initial_pwrlevel` 10 vs 8；
  `acd_level` 也不同。抄过去就是给 GPU 上错误的电压/频率组合。
- **不要照抄 uke 的 ZAP**（board-specific）。
- **GPU 编进内核会改启动路径**：在启动确定性（`tools/muyu-boot-census.sh`，5 轮 ≥4 轮进桌面）还没
  关掉之前，不要把 GPU 项加进默认镜像；要试就做独立 vendor_boot（`fw/vendor_boot_gpu*.img`）并单独记台账。
  （**2026-11-11 解析**：`cli.py` 的 `--kernel-arg`（可重复）注入 GPU 启动参数、`-o` 把镜像输出到
  `fw/vendor_boot_gpu.img`，默认 `board.toml` 与默认 `vendor_boot.img` 零改动；这道红线的执行体就是"独立镜像 +
  默认镜像兜底"，无需额外 census 工具即可先守纪律。census 工具（`tools/muyu-boot-census.sh`）仍建议后补以量化确定性。）

## 6. 下一步（离线可做）

```bash
cd UniXNU-muyu
/opt/homebrew/bin/python3.12 scripts/derive-muyu-gpu-inputs.py   # 重新派生（改 DTB 或 pin 后）
/opt/homebrew/bin/python3.12 scripts/check-muyu-gpu-inputs.py    # 27 条断言 + A732 对照表
/opt/homebrew/bin/python3.12 scripts/gen-muyu-gpu-table.py --check
```

然后按 §3 的顺序做 2→7；每做完一项都要能让 `check-muyu-gpu-inputs.py` 与
`check-sheng-gmu-startup.py`（等 port 完之后）保持绿。

### 6.1 上机准备（2026-11-11 解析完成）

§3.2–§3.4、§3.7 已离线绿；剩下"上机"前的离线动作是 §3.5/§3.6 的落地，已写入独立手册
`docs/GPU-ONDEVICE.md`：

- **首测（Tier-1）**：临时裁掉 `offline-mobile-graphics-client/load=1` 两行后，用
  `python3 UniXNU/unixnu build all -D UniXNU-muyu-17 --uefi … --edk2 <Mu_Basecore checkout> --data-ramdisk …
  --unique-chip-id 0000000000000001 --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-power=9
  --kernel-arg sheng-gpu-preempt=2 --kernel-arg sheng-gpu-dcvs=1 -j 8` 构建，再用
  `scripts/patch-vendor-boot.py --base fw/vendor_boot_nofs1.img --payload UniXNU/out/muyu/payload/payload.img
  --output fw/vendor_boot_gpu_tier1_adtfix.img` 产**独立** GPU 镜像（**base 必须是 17 线可进桌面的
  `nofs1`，不是 10-28 的 `btn2`**）；构建完立刻还原 `board.toml`，
  默认 `board.toml` / 默认 `vendor_boot.img` 零改动、可随时回退。**不要传 `--out`**。
- **字节预算（本机实测）**：基线 45 参 / 934 B；纯加 Tier-1 4 参 = 1004 B ❌；去软件图形 2 参后
  Tier-1 = 940 B ✅（本次采用）、全部 6 参 = 976 B ✅。
- **`--edk2` 必须指向 `Mu_Basecore` checkout**（不是上游 tianocore/edk2）：`cli.py:28` / `package.py:22` /
  `firmware.py` 注释与 `board.toml [firmware] packages` 的 `{edk2}` 占位均如此。传错会依次报缺
  `MdeModulePkg/Library` 与 `PcdBootManagerInBootOrder` DEC 未声明（F001）。
- **Tier-2（Metal）—— 底座已离线就绪（2026-11-11 实跑），属第二轮**：`AdrenoMetal` 已就位（非空 submodule）、
  IOAccel 预置 ABI 已对齐 21H461（4 个入口 sha 全 MATCH）、`build-adreno740-accelerator.py` seed 机制无需 QEMU
  且实跑 PASS。真正待办 = 把 accelerator 脚本接进 `board.toml [kernel] archives` + `--gpu-addon` /
  `--owner-trust-cache`（muyu 自建 owner trust cache，不能用 uke 写死路径）+ `check-accelerator-abi.py` 的
  QEMU 完整验证。详见 `docs/GPU-ONDEVICE.md` §7。
- **上机闸门 = 由 Agent 刷 `fw/vendor_boot_gpu_tier1_adtfix.img` 并启动**：
  `MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier1_adtfix.img`
  （17 线 UEFI 必须显式指定；默认 `boot.img` 是 27 线 11-05 重建的 `c50e76eb…`，两者固件卷差
  1,645,303 字节）。本机可构建、Agent 可刷机；用户只需把平板接 USB 并停在 fastboot。
  失败回退 `fw/vendor_boot_nofs1.img` + 同版 17 线 UEFI。
- **2026-11-11 跨线回归（已离线修复，详见 `docs/GPU-ONDEVICE.md` §9）**：uke 27 线整理提交
  `e545150` 把 `UniXNU/scripts/unixnu/payload.py` legacy ADT 命令里的
  `--packed-data-ramdisk / --data-ramdisk` 转发换成了 `--persistent-data`，而设备仓
  `make-muyu-adt.py` 的该 flag 只抑制 `ephemeral-storage`，不生成 memory-map 占位。
  结果是 17 线 GPU 首测 ADT 的 `/chosen/memory-map` 只剩 `TrustCache(16)`，DarwinBoot
  `RamdiskOffset == MAX_UINTN` → `EFI_COMPROMISED_DATA`（屏幕 `compromised data`）。
  修复落在设备仓 `scripts/make-muyu-adt.py`（legacy 调用推断 packed 占位），**不改框架仓**；
  schema2 的 `devicetree.py` 本就正确，故 27 线无感。首测镜像 `9d1ade3b…` 保留勿刷，
  修复版 `6202aeaa…` 已离线重建校验通过、待授权上机。
