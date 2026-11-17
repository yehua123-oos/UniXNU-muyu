# muyu GPU 上机手册（独立 vendor_boot）

> 配套：docs/GPU-UKE-ALIGNMENT.md §3.2–§3.7 / §5。
> 本文件只讲"怎么把 GPU 安全地弄上设备、怎么观察、怎么回退"。

## 0. 上机闸门（一句话）

**上机 = 由 Agent 在本机完成完整构建，把带 Tier-1 参数的 vendor_boot 刷入设备并启动的那一刻。**

在此之前全是离线：驱动构建（§3.2）、GMU pool 三处对齐（§3.3/§3.4）、显示后端 ABI 底座（§3.7）、启动参数起草（§3.5）都已就绪。
**构建与刷机命令都由 Agent 执行**（本机可直接构建、可直接 fastboot 刷机）；需要你配合的只有
"把平板接上 USB 并停在 fastboot"（设备在线状态用 `tools/muyu-flash-mode.sh status` 查），
以及亮屏之后的现象反馈。

---

## 0.1 仓库边界：哪些文件要"双 checkout 一致"，哪些绝不能同步（2026-11-11 核实）

| 目录 | git HEAD | `board.toml` 身份 | 角色 |
| --- | --- | --- | --- |
| `UniXNU-muyu` | `main` @ `5e99d2b` | **27 线**：`schema_version = 2` / iPadOS 27.0.1 (24A446) / 四层 patch 系列 | 共享设备仓主线；我们的 `scripts/` / `docs/` / `patches/` 改动同步到此 |
| `UniXNU-muyu-17` | detached `e105fa0` | **17 线**：format 1 / iPadOS 17.7.11 (21H461) / `[ipsw]` 本地 21H461 | 本轮全部 `build payload -D` 指向的 17 线 worktree，**实际编辑位置** |
| `UniXNU/out/muyu/tree` | — | 组装树（`scripts/` + `kits/muyu/` + `drivers/` + `patches/`） | `tree.py` 按 `[framework, soc, device]` 组装；设备仓 `package/` 落成 `kits/muyu/`，是**构建实际读取的源码副本**，改动后必须同步或重跑组装 |
| `UniXNU-27` | 另一 checkout | 27 线 | **另一个 AI 占用，勿动** |
| `UniXNU-uke` | `main` @ `fcf9fd3` | 参考实现 | **只读**，27 线 GPU 移植来源 |

> 🔴 **`board.toml` 两份不同是设计上的分线，不是遗漏，禁止互相复制。**
> 主 checkout 的 `board.toml` 自 2026-11-08 起未改（`2aed9d8a…`，27 线 53 项配置）；
> worktree 的是 17 线（`9b158cc2…`，45 参 / 934 B）。把 17 线配置复制过去会直接破坏 27 线。
> 同理不要同步 `configs/`、`[adt.chosen.*]`、24A446 root policy 等 27 线专属内容。
>
> ✅ **需要保持一致的范围**：本轮修改的 `scripts/build-arm-driver.py`（`11b13dc9…`）、
> `scripts/build-xnu-4k-baseline.sh`（`ac7a06bd…`）、`scripts/make-muyu-adt.py`（`63d9f272…`）、
> `docs/GPU-UKE-ALIGNMENT.md`（`858ef0e0…`）、`docs/GPU-ONDEVICE.md`（以 `cmp -s` 两处 SAME 为准），
> 以及 `patches/xnu/zzzzzzz-last-panic-print.patch` 的归位状态。
> 三份脚本在 `UniXNU-muyu-17` / `UniXNU-muyu` / `UniXNU/out/muyu/tree` 的 sha 逐项相同。
>
> 🔴 **`package/` 源码不同步到主 checkout**：17 线的显示/GPU 源码只改
> `UniXNU-muyu-17/package/`；组装树 `UniXNU/out/muyu/tree/kits/muyu/` 与它是
> **硬链接**（同 inode，实测 `25239591` / `25239577`），编辑自动生效，构建读取的
> 就是改后的内容。主 checkout `UniXNU-muyu/package/` 属 27 线，保持 `git HEAD`
> 干净。误同步会造成 ` M package/...`，用
> `git -C UniXNU-muyu show HEAD:package/<file> > UniXNU-muyu/package/<file>` 恢复（§14）。

---

## 1. 为什么是"独立镜像"而不是改默认

- 默认 `board.toml [payload] kernel_args` 当前 **45 个参数、934 字节**，跑的是 **offline 软件图形**
  （`offline-legacy-*` + `offline-mobile-graphics-*`），已验证能进桌面。
- 启用原生 GPU = 从 software → native 的切换，可能把已验证桌面弄坏。
- §5 红线：启动确定性（census ≥4/5 进桌面）未关前，别把 GPU 带进默认镜像。
- 因此用构建驱动的 **`--kernel-arg`（可重复）** 注入：`payload.py` 把它们追加进 `kernel_args`，
  写进 `kernel.macho.adt` / `unixnu.toml`，随 payload 进 vendor_boot 的 vendor ramdisk。默认
  `board.toml` 与 17 线基线镜像 `fw/vendor_boot_nofs1.img` **零改动**，GPU 参数只出现在本次构建的
  独立镜像 `fw/vendor_boot_gpu_tier1_adtfix.img` 里；回退即刷回 `nofs1`（注意 §4 的 17 线 UEFI 配套）。
- 注意 `payload.py` 有 `len(line) > 1000` 的硬闸，所以 §2 的字节预算是硬约束。

---

## 2. cmdline 字节预算（payload.py 硬闸 1000，2026-11-11 实测）

| 方案 | 字节 | 结论 |
| --- | --- | --- |
| 基线（offline，45 参） | 934 | 当前桌面 |
| 纯加 Tier-1 4 个（保留全部既有参数） | 1004 | ❌ 第十轮 `build all` 实测失败：`the kernel command line is 1004 bytes; the limit is 1000` |
| **去软件图形 2 参 + Tier-1 4 个** | **940** | ✅ **本次首测采用**（47 参） |
| 去软件图形 2 参 + 全部 6 个 | 976 | ✅ Tier-2 预留空间 |

> 早前手册写的 917/987/923/959 是估算值，与实际 board.toml 差 17 字节；**以本机
> `tomllib` 解析 `[payload] kernel_args` 后的 `' '.join(...)` 实测为准**。
> 6 个参数定义（来自对齐文档 §3.5）：
> `sheng-fb-gpu=1`(14) `sheng-gpu-power=9`(17) `sheng-gpu-preempt=2`(19)
> `sheng-gpu-dcvs=1`(16) `sheng-gpu-metal=1`(17) `sheng-gpu-addon=1`(17)。
> `--kernel-arg` 只能加、不能删，所以"去掉软件图形"必须临时改 board.toml（见 §3/§6）。

---

## 3. 首次上机（Tier-1）—— 去 2 个软件图形参数 + 加 4 个 GPU 参数

**纯加法过不了 1000 字节硬闸**（934 + 70 = 1004），所以首测必须同时临时移除
`offline-mobile-graphics-client=1`、`offline-mobile-graphics-load=1`（见 §6）：

```
# board.toml [payload] kernel_args 中临时注释两行
--kernel-arg sheng-fb-gpu=1 \
--kernel-arg sheng-gpu-power=9 \
--kernel-arg sheng-gpu-preempt=2 \
--kernel-arg sheng-gpu-dcvs=1
```

- `sheng-fb-gpu=1`：显示把手开关（`NativeGOPPresenter.cpp:94` 读此 boot-arg → `gpuMode`）。
  GPU 模式下，合成帧**交给 GPU 私有客户端**而非复制——这是原生 GPU 显示路径的入口。
- 其余 3 个是驱动调优（power 档 / 抢占 / DCVS），非正确性依赖。
- 字节：934 − 64 + 70 = **940**，落在 1000 内（本机 `payload.json` 实测）。
- 移除软件图形顺带消除了"软件图形客户端与 native 把手并存冲突"的风险；
  `offline-legacy-*`（软件 GOP/合成后端）全部保留。

---

## 4. 构建与刷机命令（本机执行，已实跑）

在**工作区根目录**执行；入口是核心仓的 `UniXNU/unixnu`，用 `-D` 指向 17 线 worktree。
下面是**当前基线（Tier-1c / Tier-1d 实跑）的 3 个 GPU 参数**版本：
`board.toml` 保持 45 参 / 934 B 零改动，两个 `offline-mobile-graphics-*` 全保留，
注入 3 参后 **984 B / 48 参**，落在 1000 B 闸内。§3 的「4 参 + 临时删 2 个显示参数 /
940 B」是**已废弃的历史首测**，它关掉了显示注册导致黑屏，见 §10。

```
python3 UniXNU/unixnu build all -D UniXNU-muyu-17 \
  --uefi /Users/diannao/pad7pro-xnu/Mu-Silicium \
  --edk2 /Users/diannao/pad7pro-xnu/Mu-Silicium/Mu_Basecore \
  --data-ramdisk /Users/diannao/pad7pro-xnu/fw/data-seed.img \
  --trust-cache /Users/diannao/pad7pro-xnu/fw/trust-21H461/21H461-original-trust.bundle \
  --unique-chip-id 0000000000000001 \
  --kernel-arg sheng-fb-gpu=1 \
  --kernel-arg sheng-gpu-power=9 \
  --kernel-arg sheng-gpu-dcvs=1 \
  -j 8
```

> - **🔴 `--trust-cache` 不能省**：`board.toml [payload] trust_cache_sha256` 白名单钉的是 21H461 原件
>   `7416ca4f…`（`fw/trust-21H461/21H461-original-trust.bundle`，61,356 B，从 17 线可进桌面镜像
>   `nofs1` 的 FAT 里提取并核对）。不传它 `payload.json` 会是 `trust_cache_sha256: null`，FAT 里
>   没有 `kernel.macho.trustcache`，ADT 也不会带 `--trust-cache`：按 11-06 交接记录，
>   launchd 会以 `SIGKILL (namespace 9 code 0x8)` 被 MAC 策略拒绝，**必挂**。
>   注意 `--trust-cache` 走 board 白名单校验；`--owner-trust-cache` 是另一条（27 线 owner 签名链），
>   17 线不要用。
> - **`--unique-chip-id` 不能省**：`board.toml [adt] unique_chip_id = "required"`，
>   `0000000000000001` 是默认可启动镜像 `fw/vendor_boot_btn2.img` 里 ADT 的实测值
>   （`~/.unixnu-muyu-chip-id` 里的 `0000000063686970` 属于另一条线，别混用）。
> - **不要传 `--out`**：那会新建一棵空的 out 树并强制重拉 feeds。复用现成的
>   `UniXNU/out/feeds`、`UniXNU/out/muyu`、`UniXNU/out/ipsw/21H461`。
> - `defconfig` 名是 `muyu-userland_defconfig`；`.config` 按 out 绑定。
> - cmdline 由 `payload.py` 封进 `kernel.macho` 与 `kernel.macho.adt`，随 60 MiB payload
>   FAT 镜像进 vendor_boot 的 vendor ramdisk；`package-sheng-boot-v4.py` **不是**注入点。
> - **🔴 `--edk2` 必须是 `Mu_Basecore` checkout，不是上游 tianocore/edk2**：
>   `cli.py:28` 的 help 就写着 "a Mu_Basecore checkout"，`package.py:22` 的报错文本同样，
>   `firmware.py` 注释「Mu_Basecore is provided separately」，`board.toml [firmware] packages`
>   的 `{edk2}` 占位也指它。曾误传 `/Users/diannao/pad7pro-xnu/edk2`(@`b9d8871d34`)，
>   与 `Mu-Silicium/Mu_Basecore`(@`3c9c7b2`, release/202608) 版本错配，于是 firmware 依次报
>   缺 13 个 `MdeModulePkg/Library`、再到 `PcdBootManagerInBootOrder` 未在 DEC 声明（F001）。
>   **往上游 edk2 拷库是走错分支的权宜之计，不是解法**；`Mu_Basecore` 自己的
>   `MdePkg/Library/BaseFdtLib/libfdt` 与 `MipiSysTLib/mipisyst` 子模块早已就位，package 预检直接通过。
>   那次尝试在 `edk2/MdeModulePkg/Library/` 留下的 13 个**未跟踪**目录是残留，不影响当前构建路径，
>   可随时清理（为不干扰其它 checkout，本次保留未删）。
> - 本机即可完整构建；此前"沙箱无外网/未导入 unixnu"的说法已作废。真正卡过构建的是
>   主仓 gitlink 指向远端不存在的 `72856360…`（已用 `update-index --cacheinfo` 改指本地
>   `kits/XnuPrelink` @ `d4fbb8b…`）。
> - `build all` **不产 `boot.img`**：`bootimg` 是独立子命令。刷机用的
>   `UniXNU/out/muyu/boot/boot.img`（2.04 MiB，Nov 5）已备份到 `/tmp/muyu-boot-backup-*.img`，
>   不需要为了刷 GPU vendor_boot 重建 firmware。
> - 只补 firmware 时可单步 `python3 UniXNU/unixnu build package firmware -D UniXNU-muyu-17 --uefi … --edk2 … -j 8`，
>   **不要重跑 `build all`**：`board.toml` 已还原离线基线（45 参/934 B），纯加 4 个 GPU 参数会得 1004 B，
>   在 payload 阶段触发 1000 字节硬闸。只改 cmdline 亦可单步 `python3 UniXNU/unixnu payload ...`
>   （同样要带 `--unique-chip-id`），不必重编整个内核。

把 payload 打包成**独立** vendor_boot（保留 DTB/table/bootconfig，不覆盖默认镜像）。
**base 必须是 17 线可进桌面的 `nofs1`**，不是 10-28 的 `btn2`：

```
python3 UniXNU-muyu-17/scripts/patch-vendor-boot.py \
  --base fw/vendor_boot_nofs1.img \
  --payload UniXNU/out/muyu/payload/payload.img \
  --output fw/vendor_boot_gpu_tier1_adtfix.img
```

> ✅ **当前运行镜像是 `fw/vendor_boot_gpu_tier1e2_probe.img`**（`8a0f6ff0…`）：
> 06:57 上机（§15.7）。hand-off 桥已通、presenter 已创建，但 `gpuClient=false`，
> 每帧被拒并回退 CPU copy —— **GPU 仍未接管，下一步是 Tier-2 用户态 scanout client**。
> 它基于 `tier1d_nopost`（`4f6467bd…`，06:11 首次进桌面 / 锁屏，摘掉了唯一新增的
> EARLY_BOOT `source_corecrypto14_bootstrap`），GPU kits 全部保留。
>
> 已上机失败、保留作证据、**不要再刷**的四个：`fw/vendor_boot_gpu_tier1.img`（`9d1ade3b…`，
> UEFI `compromised data`，ADT 缺 `PackedRAMDisk`，§9）、`fw/vendor_boot_gpu_tier1_adtfix.img`
> （`6202aeaa…`，UEFI 交权成功但显示驱动未注册 → 黑屏，§10）、
> `fw/vendor_boot_gpu_tier1c_displayfull.img`（`dd091c88…`，显示参数全恢复仍黑屏 →
> **内核极早期挂死**，§11）、`fw/vendor_boot_gpu_probe_noargs.img`（`9d871dd1…`，
> 零 GPU 参数仍黑屏 ⇒ 与 GPU 参数无关，§11.6）。

> 三个 62 MiB 镜像（`btn2` / `nofs1` / `gpu_tier1`）的 DTB、ramdisk table、bootconfig
> **逐字节相同**（`dtb_sha=63c7e64e…`），差异全在 60 MiB payload 内部；所以用哪个 base
> 对外层结构无影响，但要跟"已验证可进桌面"的那条链保持一致。

### 🔴 17 线启动三件套（2026-11-11 上机实测纠正）

| 组件 | 17 线（21H461，62 MiB 替换式 payload） | 27 线（24A446，96 MiB fragment 链） |
| --- | --- | --- |
| userdata root image | `fw/userdata-backup/userdata-root.sparse` (`4c90d7a7…`, 8.9 GB) | `/tmp/userdata-root-27.sparse` (`88bf0b11…`) |
| vendor_boot 基线 | **`fw/vendor_boot_nofs1.img`** (`c7635cdb…`, no `/filesystems`) | `fw/vendor_boot_27-uNNN.img`（96 MiB） |
| UEFI | **`fw/boot-17line-043f7120.img`** (`043f7120…`, 2.03 MiB, 10-26 构建) | `UniXNU/out/muyu/boot/boot.img` (`c50e76eb…`, 2.04 MiB, 11-05 重建) |

- 账本铁证：`2026-11-03T13:49` 换回 `userdata-root.sparse` → `13:55:10` 刷 `nofs1` →
  `13:55:11` boot `043f7120…`；census 脚本记录的"进过 iPadOS 桌面"就是这三次操作。
- 两版 UEFI **不是小改动**：解压后固件卷同为 3,145,728 B，但 **1,645,303 字节不同**
  （整段代码重排）。11-05 21:16 重建的 `c50e76eb…` 供 27 线 96 MiB fragment 实验用了 201 次；
  `043f7120…` 在 10-29~11-05 用了 110 次。两者混用会启动异常。
- `tools/muyu-flash-mode.sh` 默认 UEFI 仍是 `c50e76eb…`（保证另一条 checkout 不受影响）；
  17 线刷机时显式传 `MUYU_UEFI`。

刷机**唯一合法入口**（校验 `VNDRBOOT` 与 ≥48 MiB）：

```
bash tools/muyu-flash-mode.sh status
MUYU_UEFI=fw/boot-17line-043f7120.img \
  bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier1c_displayfull.img
```

**先恢复基线、再首测**：设备从原厂线刷回来后，先刷
`MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_nofs1.img`
确认进桌面，再上 GPU 镜像；否则 panic 无法归因到 GPU。

---

## 5. 上机观察清单

**成功信号**
- 设备仍进桌面（或至少 kernel 不 panic、UEFI 正常交权）。
- 出现 GPU 把手生效迹象（native 显示路径接管；具体看 `GpuPowerProbe` / 显示计数日志）。
- ramoops（`console-ramoops-0`，recovery adbd 读）有 GPU 初始化日志。

**失败 / 回退**
- 卡 Apple logo、panic、黑屏 → **立即回退到 17 线离线基线**（同样要带 17 线 UEFI）：
  `MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_nofs1.img`。
  彻底回原厂则 `bash tools/muyu-flash-mode.sh restore-stock --yes`（会覆盖 boot/vendor_boot/recovery，
  **不碰 userdata**）。
- ⚠️ 用户刷过原厂 ROM / 线刷后，`userdata` 里的 iPadOS root image 会消失，UEFI 停在启动阶段
  是预期结果，**不是 GPU 驱动问题**。先用
  `bash tools/muyu-flash-mode.sh root-image fw/userdata-backup/userdata-root.sparse --yes`
  写回 8.9 GB（约 3.5 分钟），再按上面顺序恢复。此操作不可逆地抹掉原厂 Android。
- 记录 panic 文本（`braveark-hold-reboot=1` 会停住保留面板文字）或 ramoops 内容。

---

## 6. board.toml 临时改动与还原（首测必需，已实跑）

`--kernel-arg` 只能**加**不能**删**；1000 字节硬闸让"纯加法"不成立，所以：

1. 备份：`cp UniXNU-muyu-17/board.toml /tmp/muyu-board-baseline.toml`。
2. 在 `[payload] kernel_args` 里注释掉 `offline-mobile-graphics-client=1`、
   `offline-mobile-graphics-load=1` 两行（本机实测腾出 64 字节）。
3. 按 §4 跑构建 + 打包；产物是独立的 `fw/vendor_boot_gpu_tier1_adtfix.img`。
4. **还原** `cp /tmp/muyu-board-baseline.toml UniXNU-muyu-17/board.toml`，
   让默认 board.toml 与默认 `fw/vendor_boot_btn2.img` 保持离线基线，回退永远可用。
5. Tier-2 需要再挂 `sheng-gpu-metal=1` / `sheng-gpu-addon=1` 时，同样去掉这 2 行，
   全部 6 参 = **976 字节**，仍在预算内。

> 若不想每次临时改，可直接保留 GPU 版 board.toml；但那样默认离线配置会少两个软件图形参数，
> 与"默认镜像零改动、回退可靠"的取舍冲突，本手册选择用完即还原。

---

## 7. Tier-2（Metal 加速）就绪度 —— 不在首次上机关键路径

2026-11-11 离线核查 + 实跑，推翻了早前"AdrenoMetal 是空 submodule / IOAccel ABI 未移植需暂缓"的判断：

- **`AdrenoMetal` 子模块非空且已 checkout**（`drivers/AdrenoMetal` @ `854f99a`），含完整源码、
  `OriginalAcceleratorABI.h`（11 行抽象桩，`OSDeclareAbstractStructors(IOAcceleratorES)`，无硬编码地址）、
  `CacheExport.h`、`GpuScanout*.h`、`tools/` `bridge/` `shaders/`。**不需要拉取。**
- **IOAccel 基础 ABI 已对齐 21H461、无需 QEMU 重对**：核心仓预置
  `scripts/data/ioaccelerator-base-abi.json`（metaclass `0xfffffff006aeb688`，iOS17 的 `0xfffffff0…` KVA），
  其 4 个入口指令 sha 在 21H461 候选 `native-owner-renderbox-presentation-candidate.macho`（`3461cadb…`）上
  **逐一 MATCH**（`9ed90924…`/`6c4f63d7…`/`456b2a9a…`/`9daad2c6…`）——IOGPUFamily 代码段字节跨链接一致。
  预置 `candidate_sha256=da673101…` 只是另一次整包 kernel hash，不影响地址有效性。
- **`build-adreno740-accelerator.py` 有 seed 机制**：`build/gpu-analysis/ioaccelerator-base-abi.json` 缺失时
  直接从 `scripts/data/` 拷预置记录（"seeds a fresh tree"），**不触发** `audit-target-accelerator-base.py`
  的 QEMU gdb-stop 路径。17 线 `kernel_target.py` 默认 `ARCH=arm64 / IOS_VERSION=17.0` → `FILESET=False`
  → 走默认 json（`-24A446.json` 那份是 27 线 arm64e fileset 专用，带 `collection_slide`）。
- **已端到端实跑 PASS**：`python3 scripts/build-adreno740-accelerator.py` →
  `PASS: staged AGXAccelerator builds against the audited original IOAcceleratorES imports`，
  seed 落地 `build/gpu-analysis/ioaccelerator-base-abi.json`，产出 `libAdreno740Accelerator.late.a`。
- **但 Tier-2 尚未串进默认内核**：`board.toml [kernel] archives` 只有 `build-native-legacy-display.py` +
  `build-arm-driver.py Adreno740`，**没有** accelerator 脚本。完整验证链 `check-accelerator-abi.py` 需要 3 个
  QEMU gdb-stop 观测（positive / missing-base / default-off），且 `make-gpu-addon.py` 的
  `--gpu-addon` / `--owner-trust-cache`（muyu 需自建 owner trust cache，uke 的写死在别人机器上）也待接。
- 参数：`sheng-gpu-metal=1`、`sheng-gpu-addon=1`（各 +17 字节；去软件图形后全部 6 个 = 976 字节，预算够）。

**结论**：Tier-2 的 ABI 底座与 staged 类**离线已就绪**，真正缺的只是"把它接进 board.toml + addon/trust cache +
QEMU 完整验证 + 上机观察"，属于首测回来之后的第二轮工作，不阻塞首次 GPU 上机。

---

## 8. 状态速览

| 项 | 状态 |
| --- | --- |
| §3.2 驱动构建（`libAdreno740.a`：Service + role shim + 28 个 C++ kits + PM4 + 21H461 shims） | ✅ 本机实跑 EXIT=0 |
| 内核链接闭环（41 + 4 个 undefined 全部解析，见 `docs/GPU-UKE-ALIGNMENT.md`） | ✅ 本机实跑，`kernel.macho` 已产出 |
| §3.3 / §3.4 GMU pool 三处对齐（ADT / MemoryMapLib / [gpu]） | ✅ 离线已完成 |
| §3.7 显示后端 ABI 底座（TARGET_ABI 化 + audit 脚本） | ✅ 离线已完成 |
| §3.7 显示后端在 21H461 实跑验证（`build-native-legacy-display.py` EXIT=0） | ✅ 证实不阻塞，`target_abi=None` 回退地址在 21H461 上全部吻合 |
| §3.5 参数集 + 字节预算 + 独立镜像机制 | ✅ 实测 940 B；`fw/vendor_boot_gpu_tier1_adtfix.img` 已产出（61.81 MiB / VNDRBOOT v4） |
| §3.6 / Tier-2：AdrenoMetal 已就位、IOAccel ABI 预置=21H461、staged 类实跑 PASS | ✅ 底座已就绪（未串 board.toml / 未接 addon，属第二轮） |
| **完整 `build all` 全绿** | ✅ 2026-11-11 终版单条命令 `BUILD_EXIT=0`（1m51s）：kernel → package → payload（940 B / 47 参 / 60 MiB）→ firmware 全过；firmware 此前失败的根因是 **`--edk2` 传成了上游 tianocore/edk2**，改传 `Mu-Silicium/Mu_Basecore` 即通过。终版内核 `420700bd…`，FD `SILICIUM_UEFI.fd` 3,145,728 B |
| **UEFI boot 镜像（17 线）** | ✅ `fw/boot-17line-043f7120.img`（2.03 MiB / `043f7120…` / 10-26 构建）已单列，刷机时 `MUYU_UEFI=` 指定。此前误以为 `c50e76eb…` 通用，实测两版固件卷 1,645,303 字节不同且属于不同启动链 |
| **GPU vendor_boot 首测镜像（失败，勿刷）** | ⛔ `fw/vendor_boot_gpu_tier1.img`（64,811,008 B / sha `9d1ade3b…`）：2026-11-11 首测停在 UEFI `compromised data`，ADT 缺 `PackedRAMDisk`，保留作证据（§9） |
| **GPU vendor_boot 修复版（待刷）** | ✅ `fw/vendor_boot_gpu_tier1_adtfix.img`（64,811,008 B / VNDRBOOT v4 / sha `6202aeaa…`）：以 `nofs1` 为 base，带 `7416ca4f…` trust cache、940 B / 47 参、4 个 GPU 参数齐全、软件图形 2 参 absent；FAT 内 ADT 13,940 B 且 `/chosen/memory-map` = `PackedRAMDisk(16) + TrustCache(16)`（§9 离线实证） |
| **默认配置 / 回退** | ✅ `board.toml` 已还原离线基线（45 参 / 934 B / 软件图形 2 参恢复）；`fw/vendor_boot_btn2.img`（`534125e3…`）、`nofs1`（`c7635cdb…`）与 `fw/stock/*` 未改动 |
| **设备底座恢复** | ✅ 2026-11-11 上机：用户线刷原厂后 userdata 被清空；已 `root-image … --yes` 写回 8.9 GB（201.877 s，EXIT=0），窗口布局逐项一致 |
| **上机：基线确认** | ✅ 用户确认「成功开了」（进桌面/锁屏）；按键可用而触摸失效属基线既有独立链路（`offline-key-events` vs `NativeNovatekTouch`），不归因 GPU |
| **上机：GPU 首测** | ⛔ 已刷 `vendor_boot_gpu_tier1.img` + `043f7120` UEFI（`XNU_EXIT=0`），停在 UEFI 显示 `compromised data`；账本确认**没有**刷成 27 线 |
| **ADT 修复版上机** | ✅ 2026-11-11 05:26 刷入 `6202aeaa…`（flash + boot rc=0）；`compromised data` 消失、DarwinBoot 成功交权 → ADT 修复**有效**；但 UEFI 之后「背光亮、全黑」（§10） |
| **首测黑屏根因（只解释首测）** | ✅ 为塞进 1000 B 闸删掉 `offline-mobile-graphics-{client,load}=1`，把 `native_legacy_display_probe()` 与 `native_mobile_display_provider_probe()` 的**全部**调用路径关掉，显示驱动未注册（§10）；**不能解释 Tier-1c** |
| **Tier-1c 上机（05:39）** | ⛔ 显示参数与基线完全一致仍「背光亮、全黑」，且**按键完全无反应** ⇒ 内核在 IOKit/bsd 后期之前就挂死，不是显示消费者缺失（§11.1） |
| **新内核结构差异（离线实证）** | ✅ 新内核独有 `__TEXT,__fips_hmacs` / `crypto-hmac.json` / `_source_corecrypto14_bootstrap`（`EARLY_BOOT, STARTUP_RANK_FIRST`）+ 4 个 Adreno C++ 静态构造器；旧 `nofs1` 是 `prelinked`，全无。FIPS HMAC seal 离线重算**自洽**，`gmu-memory` 无内核消费者（§11.2/§11.3） |
| **挂点定位镜像** | ✅ `fw/vendor_boot_diag_earlyboot.img`（`560d987d…`）：在 `_initialize_screen`（EARLY_BOOT 之后）经 panic renderer 画 `DIAG: EARLY_BOOT and screen passed` 并 park；patched kernel 的 FIPS HMAC 重算仍匹配（§11.4） |
| **离线修复：摘掉 EARLY_BOOT POST** | ✅ `build-xnu-4k-baseline.sh` 只对 `prng_random.o` 加 `-U`（当前 21H461 只有该文件引用该宏）；删陈旧对象强制重编后 `KERNEL_EXIT=0`，新内核 `090db15c…` 的 EARLY_BOOT 表与 `nofs1` 逐项一致（15 项），4 个 Adreno 构造器 / PM4 / dcvs 全保留，`__fips_hmacs` 与 `crypto-hmac.json` 消失（§11.8） |
| **QEMU 离线验证** | ⚠️ 不可判定：真机 53 MB prelinked 内核在 synthetic QEMU DT 下停在 UEFI/loader 物理地址（`TTBR1_EL1=0`），未进 EARLY_BOOT，脚本解析崩溃；不能据此判断 POST 通没通过（§11.8） |
| **Tier-1d 上机（06:11）** | ✅ **当前运行镜像** `fw/vendor_boot_gpu_tier1d_nopost.img`（`4f6467bd…`，984 B / 48 参、3 个 GPU 参数、显示参数全保留、ADT 带 `PackedRAMDisk`）：flash + `fastboot boot 043f7120` 均 `rc=0`，用户确认**进桌面 / 锁屏、画面正常**（§12）。设备证实 EARLY_BOOT POST 是挂死根因 |
| **GPU 运行时验证（离线已判定）** | ❌ **GPU 没接管，桌面是 CPU copy**：只读解包当前运行内核，`nm -m` 证实 `_native_legacy_gpu_offer` 是 weak 空桩、`phase=4` 不创建 handoff presenter；Adreno kits 全在但 hand-off 三环缺。`sheng-gpu-power=9` 落到空桩、`sheng-gpu-dcvs=1` 17 线无消费者。`PREVIOUS XNU LOG` 实测不通、BootProgress 诊断页未启用 ⇒ 当前镜像也拿不到 GPU 证据。现成修法（从 `UniXNU-sheng` 移植 5 参数桥）+ 三步路线图见 **§13**；其中第 1、3 步已在 **§14** 离线实施并产出待刷镜像，设备未写入。失败回退 `MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_nofs1.img` |
| **Tier-1e：补桥 + 面板观测（已上机）** | ✅ 06:43:56Z 刷入 `fw/vendor_boot_gpu_tier1e_handoff.img`（`5dbfb604…`，64,811,008 B，内核 `e886af3d…`，cmdline 983 B / 48 参）：flash + boot 均 `rc=0`，`_native_legacy_gpu_offer` 从 weak 变 **external**，EARLY_BOOT 15 项 / bootstrap 0，ADT 带 `PackedRAMDisk+TrustCache`。**用户确认进了桌面**，`MUYU GPU` 行出现几次但全部为 0（§15.1） |
| **Tier-1e 全零根因（离线定位）** | ✅ `native_legacy_transfer_observed()`（`LegacyDisplayProbe.cpp:249`）在首帧 `kIOReturnSuccess` 时 `vc_set_text_quiet(1)` **park 文本控制台**，诊断行只在首帧之前可见；`muyu-dump-at=20` 又早于合成器出帧 ⇒ 观测窗口与显示链错开，全零**不能**判定桥失败（§15.2） |
| **Tier-1e2：增强诊断（已上机，06:57）** | ✅ **当前运行镜像** `fw/vendor_boot_gpu_tier1e2_probe.img`（`8a0f6ff0…`，64,811,008 B，内核 `b2846224…`，cmdline **996 B / 48 参**）：关 park（`offline-legacy-console-quiet=0`）、dump 60 s、强符号 `muyu_handoff_snapshot()` 一次读全计数、`MUYU OFFER`/`MUYU SWAP` 写 msgbuf。flash + boot 均 `rc=0`，用户读屏 **`calls>0 refused>0 o=0`** ⇒ **桥已通、presenter 已创建，但 `gpuClient=false`，仍 CPU copy**。设备坐实 §13.6 第 2 步（用户态 scanout client）是唯一剩余缺口（§15.7） |
| **Tier-2 接线（离线打通，15:09）** | ✅ 内核 addon mount 已存在（§16.1，早前「patch 被排除」是误读）；修复 legacy `make-muyu-adt.py` 的第二个回归 —— 新增 `--gpu-addon` 与 `GpuAddon(16)`（三树 sha `38c05a62…`）。单步 `build payload` **EXIT=0**：985 B / 48 参、FAT 增 `kernel.macho.gpuaddon`、ADT 13,992 B 含 `GpuAddon`、owner trust 3 模块。证据 `/tmp/muyu-tier2-wiring/`；占位 addon **不可刷**，构建目录已恢复 Tier-1e2。**剩余阻塞 = 外部编译器工具链**（Mesa/Turnip + metal2vulkan Rust + meson/glslang/rustc 本机全缺，§16.4） |

---

## 9. 首测 `compromised data` 复盘（2026-11-11，离线定位 + 修复完成）

**现象**：刷入 `fw/vendor_boot_gpu_tier1.img`（`9d1ade3b…`）并从 RAM 启动 17 线 UEFI 后停在 UEFI，
屏幕 `compromised data`。`logs/flash-ledger.tsv` 证明最后一次 boot 仍是
`fw/boot-17line-043f7120.img`（`043f7120…`），**没有刷成 27 线**；两个内核都是 `MH_EXECUTE`
（filetype 2），fileset 不兼容已排除。

**根因**：`RETURN_COMPROMISED_DATA = 33 | MAX_BIT` 在 `BasePrintLib/PrintLibInternal.c` 里对应字符串
`"Compromised Data"`；DarwinBoot 的 `Test/Validation.c` 对 `missing-ramdisk-adt` / `duplicate-ramdisk` /
`stale-trustcache` / `bad-memory-size` / `bad-adt` 等用例明确期望 `EFI_COMPROMISED_DATA`。
逐文件比对两份 60 MiB FAT payload 后定位：

| FAT 内文件 | `nofs1` 基线（可进桌面） | GPU 首测（失败） |
| --- | --- | --- |
| `kernel.macho.trustcache` | `7416ca4f…` | `7416ca4f…`（相同） |
| `scratch-data.img` | `DBRDISK1` / `476464c3…` | 相同 |
| `kernel.macho.adt` | 13,832 B：`PackedRAMDisk(16) + TrustCache(16)` | 13,888 B：**只有 `TrustCache(16)`** |
| `kernel.macho.crypto-hmac.json` | 无 | 342 B（`seal-source-crypto-kernel.py` 供内核自用；loader 按固定路径读文件，不枚举目录，无害） |

`DarwinBoot.c:AdtNode()` 只在 `/chosen/memory-map` 见到 `RAMDisk` / `PackedRAMDisk`（长度必须为 16）
时才写 `RamdiskOffset`；缺属性 → `RamdiskOffset == MAX_UINTN` → `Request->Ramdisk != NULL` 分支
直接拒绝（10-26 版固件实现为 `EFI_COMPROMISED_DATA`，当前源码是 `DiagHang()`，源码与旧固件不是同一份）。

**回归来源（跨线合并）**：`UniXNU/scripts/unixnu/payload.py` 的 legacy（schema_version != 2）
ADT 命令构造，历史提交 `160ea44` / `1125c2e` 原本是：

```python
if data:
    command.append('--packed-data-ramdisk' if adt.get('packed_data_ramdisk') else '--data-ramdisk')
```

uke 的 27 线整理提交 `e545150` 把它换成 `--persistent-data`（末尾又重复一次）。但 17 线设备仓
`scripts/make-muyu-adt.py` 里的 `--persistent-data` **只抑制 `ephemeral-storage` 属性**，
不生成 memory-map 占位；`board.toml [payload.adt] packed_data_ramdisk = true` 从此无人转发。
schema_version 2 的 `devicetree.py:336` 仍正确处理，所以 27 线不受影响——这正是"17 线换用 27 线框架后
才丢占位"的原因。

**修复（只落设备仓，不碰框架仓 / `UniXNU-27` / `UniXNU-uke`）**：
`scripts/make-muyu-adt.py` 在 `parse_args()` 后加兼容分支——legacy 调用方只给 `--persistent-data`、
没给任何 ramdisk 类型 flag 时，按 17 线 `DBRDISK1` Data seed 推断并声明 `PackedRAMDisk`。
必须是 `PackedRAMDisk` 而非普通 `RAMDisk`：DarwinBoot 对前者设 `KeepPacked=TRUE`，按 packed 字节
原样交付；普通 `RAMDisk` 会让 loader 按 `RamdiskExpandedSize` 分配窗口却交付未解压数据。
三份副本（worktree / 主 checkout / 组装树）sha 一致，`build payload` 的 tree 组装会自动带入。

**离线实证**：用首测那条一模一样的 ADT 命令重跑，`payload-adt.log` 出现
`muyu ADT: legacy payload.py omitted the ramdisk-type flag; declaring PackedRAMDisk…`，
ADT 13,888 → 13,940 B（+52 B = 32 名字 + 4 长度 + 16 零值）。把 `random-seed`
（`os.urandom(64)`，每次本就不同）规范化后逐属性对比：修复版相对首测版
**唯一变化就是新增 `/chosen/memory-map/PackedRAMDisk`**，无删除、无改写，与 `nofs1` 基线结构一致；
三份 ADT 都满足 `Offset == TreeSize`（DarwinBoot 的 ADT 遍历检查通过）。

**产物校验**：`fw/vendor_boot_gpu_tier1_adtfix.img`（64,811,008 B / `VNDRBOOT` v4 /
sha `6202aeaa…`）以 `nofs1` 为 base，ramdisk section 0x1000 处 60 MiB 与 `payload.img`
逐字节相同；cmdline 940 B / 47 参、4 个 GPU 参数齐全、软件图形 2 参 absent；FAT 内
`kernel.macho.adt` 13,940 B / `62f927f8…`，`PackedRAMDisk` 与 `TrustCache` 占位全零，
`darwinboot-memory-size` 保留。`board.toml` 已还原离线基线（45 参 / 934 B）。

**后续**：该镜像已于 2026-11-11 05:26 上机（flash + boot 均 rc=0，ledger
`05:26:25` / `05:26:26`），`compromised data` 消失、UEFI 成功交权，**证明 ADT 修复有效**。
但交权后出现「背光亮、屏幕全黑」，这是**另一个**与 ADT 无关的问题，见 §10。

---

## 10. 交权后黑屏复盘：显示驱动注册被参数裁剪门控（2026-11-11，离线定位完成）

**现象**：刷 `fw/vendor_boot_gpu_tier1_adtfix.img`（`6202aeaa…`）后，UEFI 阶段通过，
随后**背光亮但整个屏幕全黑**；按电源键有背光、无画面。设备既不在 fastboot 也不在 adb，
未自动重启回 bootloader（`braveark-hold-reboot=1` 会停住 panic）。17 线唯一输出通道是面板
（`docs/SCREEN-ONLY.md` / `ABANDONED-CHANNELS.md`：USB CDC ACM 与 Linux/pstore 已拆除），
所以主机侧读不到任何日志，只能从代码与镜像反推。

**根因**：Tier-1 首测为了把 4 个 GPU 参数塞进 `payload.py` 的 1000 B 硬闸，临时删掉了
`offline-mobile-graphics-client=1` 与 `offline-mobile-graphics-load=1`（§6）。
这两个参数不是可有可无的"软件图形开关"，它们是**显示驱动注册入口的门控**：

| 注册函数 | 作用 | 唯一/全部调用点 | 依赖的 boot-arg |
| --- | --- | --- | --- |
| `native_mobile_graphics_load_probe()` | 加载 `com.apple.iokit.IOMobileGraphicsFamily`，再调 `native_legacy_display_probe()` | `bsd_init.c:796` → `:542` | 函数开头无 `offline-mobile-graphics-load` 直接 `return`（`:545`） |
| `native_legacy_display_probe()` | `preModLoad` → `native_legacy_display_static_init()` → **`native_gop_presenter_static_init()`** → `postModLoad` → 分配 `IOMobileFramebufferLegacy` | 仅 `bsd_init.c:555`、`IOUserClient.cpp:6791` | 前者需 `load` + `offline-legacy-register=1`；后者在 client probe 内 |
| `native_mobile_graphics_client_probe()` | 内含 `native_mobile_display_provider_probe()` + `native_legacy_display_probe()` | `IOUserClient.cpp:6752` | 仅 `offline-mobile-graphics-client=1` 时由 `bsd_init.c:560` 调用 |
| `native_mobile_display_provider_probe()` | 注册 `IOMobileFramebuffer` provider | **只有** `IOUserClient.cpp:6787` | 同上，只在 client 路径 |

首测把 `client` 和 `load` 都删了 ⇒ `native_mobile_graphics_load_probe()` 立刻 return、
`native_mobile_graphics_client_probe()` 根本不调用 ⇒ **四个注册入口一个都没跑** ⇒
没有 `IOMobileFramebuffer`，没有任何东西往 scanout 写帧 ⇒ 背光亮、全黑。
系统很可能仍活着，只是没有显示服务。这与 GPU 驱动是否成功**无关**。

**已排除的假设**（避免走回头路）：
- 「17 线没编入 GPU 私有客户端 → `offer()` 被拒 → 黑屏」：`NativeLegacyDisplay.cpp:37,297`
  明确 offer 被拒 / GPU copy 失败会**回退到 CPU copy**；`NativeGOPPresenter.cpp:109` 的
  `if(gpuMode) return kIOReturnUnsupported` 只关 presenter 的用户态 `transfer()` 接口，
  不关显示驱动自身的 CPU copy。
- 「`-bravearktextconsole` 文本/panic 控制台写不进 framebuffer」：
  `zzzzzz-video-console-text.patch` 在 `vcattach()` 里用 `initialize_screen(NULL,kPETextScreen)` +
  `vc_clear_screen()`，直接走内核 `vinfo`，不经过 presenter 的 `transfer()`。
- 「27 线 probe4 说三个运行时 kext 加载无罪，所以删掉没关系」：那条结论针对 27 线
  `bsd_init` 卡点排查；17 线的这两个参数额外门控了**显示驱动静态注册**，不能照搬。

**离线重建的候选镜像**（全部以 `nofs1` 为 base，ADT 都带 `PackedRAMDisk`，trust/data 不变，
`board.toml` 最终都在 45 参 / 934 B 基线）：

| 镜像 | sha | cmdline | 显示参数 | GPU 参数 | 用途 |
| --- | --- | --- | --- | --- | --- |
| `fw/vendor_boot_gpu_tier1_adtfix.img` | `6202aeaa…` | 940 B / 47 参 | 两个都删 | 全 4 个 | ⛔ 已上机、黑屏，保留作证据 |
| `fw/vendor_boot_gpu_probe_noargs.img` | `9d871dd1…` | 934 B / 45 参 | 两个都在 | **0 个** | 变量隔离：能进桌面 ⇒ 新内核无罪，黑屏来自 GPU 参数/显示裁剪 |
| `fw/vendor_boot_gpu_tier1b_displayfix.img` | `2bd4e25a…` | 971 B / 48 参 | 只保留 `load` | 全 4 个 | 半修复：只恢复 `native_legacy_display_probe()`，仍缺 mobile provider |
| **`fw/vendor_boot_gpu_tier1c_displayfull.img`** | **`dd091c88…`** | **984 B / 48 参** | **两个全保留** | **3 个**（去掉纯调优的 `sheng-gpu-preempt=2`） | ✅ **推荐**：显示链与已验证基线完全一致，board.toml 零改动 |

Tier-1c 的字节算法：基线 934 B + `sheng-fb-gpu=1`(14) + `sheng-gpu-power=9`(17) +
`sheng-gpu-dcvs=1`(16) + 3 个分隔空格 = **984 B**，仍在 1000 硬闸内。
`offline-legacy-register=1` 与两个 `offline-mobile-graphics-*` 都在，四个显示注册路径全部可达。

**当前状态（已被 §11 上机结果推翻，保留作推理记录）**：用户选择「先只做离线分析」，
以上四个镜像全部离线就绪。随后 Tier-1c 已上机，**显示参数全部恢复仍黑屏，且按键完全无反应**
——「系统活着但没画面」不成立，见 §11。

---

## 11. Tier-1c 上机：内核极早期挂死（2026-11-11，离线定位完成）

### 11.1 上机事实

| 时间 | 动作 | 结果 |
| --- | --- | --- |
| 05:39:21Z | `flash vendor_boot fw/vendor_boot_gpu_tier1c_displayfull.img`（`dd091c88…`） | `rc=0` |
| 05:39:21Z | `fastboot boot fw/boot-17line-043f7120.img`（17 线 UEFI） | `rc=0` |
| — | 屏幕 | **背光亮、仍全黑** |
| — | 按电源/Home | **完全无反应** |

`offline-key-events=1` 是纯软件注入，在 `nofs1` 基线上按键可用；现在连它都不响应，
说明**内核在 IOKit/bsd 后期之前就挂死了**，不是「缺显示消费者」。显示参数已与基线逐项一致，
§10 的参数裁剪结论只能解释首测，不能解释本次挂死。

### 11.2 新内核 vs `nofs1` 的实测差异（两份 FAT 逐文件 + Mach-O 对比）

| 项 | `nofs1`（可进桌面） | Tier-1c（挂死） |
| --- | --- | --- |
| `kernel.macho` | 52,785,152 B / `fcc29691…` | **53,354,496 B** / `b45eb5ba…`（+569,344） |
| Mach-O | `MH_EXECUTE`，25 load commands | 同 |
| `__TEXT_EXEC,__text` | 8,585,216 B | **8,896,512 B**（+311,296，Adreno kits） |
| `__TEXT,__fips_hmacs` | **无** | **32 B** |
| `kernel.macho.crypto-hmac.json` | **无** | 342 B |
| `_source_corecrypto14_bootstrap` / `_corecrypto_kext_start` / `_fipspost_post` | **全无** | **全有** |
| `__DATA_CONST,__mod_init_func` | 0x320 / 100 项 | **0x340 / 104 项** |

新增的 4 个静态构造器（按符号名规范化 diff，地址整体重排不能按指针比）**全是 Adreno kits**：
`__GLOBAL__sub_I_NativeAdreno{Service,GmuService,Publish,SmmuService}.cpp`；旧内核一个都没有。
`gmu-memory` ADT 节点在内核源码里**没有任何消费者**，排除。

### 11.3 头号嫌疑：`source_corecrypto14_bootstrap` 的 EARLY_BOOT POST

`osfmk/prng/prng_random.c:435-453`：

```c
source_corecrypto14_status = corecrypto_kext_start(&info, NULL);
if (source_corecrypto14_status != KERN_SUCCESS)
    panic("source corecrypto initialization failed: %d", source_corecrypto14_status);
STARTUP(EARLY_BOOT, STARTUP_RANK_FIRST, source_corecrypto14_bootstrap);
```

这是**最早一批** startup hook，比显示注册、比 `offline-key-events` 注入都早得多；早期 panic 时
video console 尚未 attach ⇒ 全黑、按键无反应。旧 `nofs1` 走 `prelinked` crypto bootstrap，
完全没有这条路径。跨线重建内核时 `XNU_SOURCE_CORECRYPTO14=1` + `CRYPTO_BOOTSTRAP_MODE=source`
是 board.toml 的当前配置（`[kernel] archives` 首项 `build-corecrypto-14.py`），但**这套配置
从未在设备上验证过能进桌面**——`nofs1` 是 10-26 的旧构建。

**已排除的旁支**：
- FIPS HMAC seal 是**自洽**的：离线按 `hmac-sha256(key=0x00, __TEXT_EXEC/__text)` 重算，
  `computed == __fips_hmacs 存储值 == crypto-hmac.json`，`code_file_offset=0x2ac000`
  与 `code_bytes=8882732` 都对。所以不是「seal 与改写后的 __text 不匹配」。
  `payload-seal.log` 的 `runtime selftests are still required` 指 POST 自检，仍需设备上定位。
- 4 个新增 Adreno 构造器只是 C++ POD 静态对象（如 `gMailboxOwner=nullptr`），
  `drivers/Adreno740/` 里没有 `STARTUP()` / `__attribute__((constructor))`，无早期硬件访问。
- `kernel.macho.crypto-hmac.json` 是内核自用，DarwinBoot 按固定路径读文件、不枚举目录，无害。

### 11.4 挂点定位镜像（payload-only，不重编内核）

`entry_probe_fill` 在当前 `payload.py` **没有转发**（是注释里的死配置），
但 `entry_probe_fb_after` 可用（`--fb-probe-after` + `panic_screen = true`）。
临时在 `board.toml` 加三行、跑 `build payload`、立刻还原（45 参 / 934 B 已核对）：

```toml
entry_probe_fb_after = "_initialize_screen"
entry_probe_fb_in = "_kernel_bootstrap_thread"
entry_probe_fb_text = "DIAG: EARLY_BOOT and screen passed"
```

`_initialize_screen` 在 `_kernel_bootstrap_thread:0xfffffff007a98fe8`，位于所有
`kernel_startup_initialize_upto`（EARLY_BOOT hooks）**之后**，且 screen 已初始化，
panic renderer 的 `vinfo` 可用、画字成功率最高。另一个更早的锚点是
`_PE_create_console` in `_arm_init`（只证明内核入口跑过，不能区分 POST）。

产物 **`fw/vendor_boot_diag_earlyboot.img`**（`560d987d…`，64,811,008 B / VNDRBOOT v4）：

- cmdline 984 B / 48 参，与 Tier-1c 完全一致（3 个 GPU 参数、两个显示参数全保留）；
- ADT 13,940 B，带 `PackedRAMDisk(16) + TrustCache(16)`，scratch 为 `DBRDISK1`；
- **patched kernel 上的 FIPS HMAC 重算后仍匹配**（`b1aae833…`），诊断镜像不会自己制造 POST 失败；
- `payload-patch.log`：`frame-buffer probe after call #1 to _initialize_screen …
  draws 'DIAG: EARLY_BOOT and screen passed' through the panic screen and parks`。

**判据**：屏幕出现这行字 ⇒ EARLY_BOOT（含 FIPS POST）已过，挂点在 IOKit/bsd 后期
（转查 Adreno 运行时 / 显示注册）；仍全黑 ⇒ 挂在 EARLY_BOOT 或更早，指向
`source_corecrypto14_bootstrap`，需把新内核的 crypto bootstrap 改回 `prelinked` 重编。

### 11.5 下一步（设备需先长按电源回 fastboot）

| 优先 | 镜像 | 目的 |
| --- | --- | --- |
| 1 | `fw/vendor_boot_gpu_probe_noargs.img`（`9d871dd1…`） | 新内核 + **零 GPU 参数** + 完整显示基线；若进桌面 ⇒ GPU 参数（尤其 `sheng-fb-gpu=1`）是罪魁，新内核无罪 |
| 2 | `fw/vendor_boot_diag_earlyboot.img`（`560d987d…`） | 若上一步仍黑屏，用画字定位挂点在 EARLY_BOOT 之前还是之后 |
| 回退 | `fw/vendor_boot_nofs1.img`（`c7635cdb…`） | 随时恢复已验证桌面，配套 `MUYU_UEFI=fw/boot-17line-043f7120.img` |

所有刷机仍走 `tools/muyu-flash-mode.sh xnu <img>` 且必须带 `MUYU_UEFI`。
`UniXNU-27` / `UniXNU-uke` / 框架仓 `payload.py` 本轮均未改动。

### 11.6 `probe_noargs` 上机：确认与 GPU 参数无关（05:49）

| 时间 | 动作 | 结果 |
| --- | --- | --- |
| 05:49:32Z | `flash vendor_boot fw/vendor_boot_gpu_probe_noargs.img`（`9d871dd1…`） | `rc=0` |
| 05:49:32Z | `fastboot boot fw/boot-17line-043f7120.img` | `rc=0` |
| — | 屏幕 / 按键 | **背光亮、仍全黑；完全无反应** |

该镜像是**同一个新内核 + 934 B / 45 参完整基线 cmdline（零 GPU 参数、两个显示参数全保留）**，
结果与 Tier-1c 逐项一致。至此可以排除：GPU 参数（含 `sheng-fb-gpu=1`）、显示注册参数、
ADT `gmu-memory`、trust cache、Data seed、UEFI 版本。
**挂死完全由新内核本身引入，且发生在所有用户可见服务之前。**

### 11.7 构建链线索：source corecrypto bootstrap 从哪来

- 17 线所有 `configs/*_defconfig` 都写 `CONFIG_XNU_CRYPTO_BOOTSTRAP="prelinked"`；
  `UniXNU/scripts/unixnu/kernel.py:xnu_env()` 会把 `CONFIG_XNU_*` 转成 `XNU_*` 环境变量，
  所以 `XNU_CRYPTO_BOOTSTRAP=prelinked` 应当生效。
- 但 `prng_random.c` 的 `source_corecrypto14_bootstrap()` 只由 `-DXNU_SOURCE_CORECRYPTO14=1` 门控。
  `build-xnu-4k-baseline.sh:189-202` 只在 `prelinked && XNU_SOURCE_CORECRYPTO14 != 1` 时给
  `prng_random.o` 加 `-U`；当前 defconfig 同时是 `CONFIG_XNU_SOURCE_CORECRYPTO14=y`，
  因此 **prelinked 也不会消掉这个 bootstrap**。
- `nofs1`（10-26 构建）的内核里 `_source_corecrypto14_bootstrap` / `_corecrypto_kext_start` /
  `_fipspost_post` 全无、`__fips_hmacs` 段也没有 ⇒ 那次构建的 `prng_random.o` 不带该宏。
  即：**本次用当前 board.toml 完整重编内核，是这套 source-provider 路径第一次上设备**。
- 修复方向：让 `prng_random.o` 不带 source provider（单独 `-U`），重新链接内核；
  **不要**只把 `panic` 改掉——`source_corecrypto14_status` 与 PRNG 回调
  仍可能被后续 `early_random` 依赖，绕过 POST 只会把挂点后移。**该修复已在 §11.8 离线完成。**

### 11.8 离线修复：摘掉 EARLY_BOOT POST（2026-11-11，未上机）

**静态依据**（全部离线实证）：
- `___startup_EARLY_BOOT_entry_*` 表精确 diff：`nofs1` 15 项 → 挂死内核 16 项，
  **唯一新增** `___startup_EARLY_BOOT_entry_source_corecrypto14_bootstrap_453`，无删除。
- `XNU_SOURCE_CORECRYPTO14` 在当前 21H461 源码树里**只被 `osfmk/prng/prng_random.c` 引用**；
  `OSKext.cpp` 无引用，`source_corecrypto14_original/start_once` 不存在。
  故 `build-xnu-4k-baseline.sh` 里「不能无条件 `-U`」的历史警告已过时。
- `source_corecrypto14_status` 全内核只有 3 处引用，都在 `prng_random.c` 自己
  （声明 / 赋值 / panic 检查），没有任何其它代码读它。
- QEMU 离线验证**不可判定**：真机 53 MB prelinked 内核在 synthetic QEMU DT 下
  停在 `pc=0x7fd65e04`（UEFI/loader 物理地址，`TTBR1_EL1=0`），未进内核虚拟地址空间，
  脚本随后在解析 `native_hid` 时报错。不能用它证明 POST 通过或失败。
- `static_if` 测试符号（`_static_if_tests_setup` 等）在两个内核里都不存在，
  `crypto-hmac.json` 的 `static_if_sites_prepatched: 0` 属正常；FIPS HMAC 离线重算自洽。
- `0 frame link(s)` 是 panic screen 的**栈回溯帧数**（空间不够从 6 退到 0），
  不是 framebuffer 链接，诊断镜像画字仍有效。

**改动**（只落设备仓，三份副本 sha 一致，可回滚，备份 `/tmp/muyu-nopost-*`）：
`scripts/build-xnu-4k-baseline.sh` 的 crypto 条件块改为无条件
`EXTRA_MAKE_ARGS+=("prng_random.o_CFLAGS_ADD=-UXNU_SOURCE_CORECRYPTO14")`。
`-U` 追加在 `CFLAGS_EXTRA` 的全局 `-D` 之后，只对该对象生效；
其它 archive（含全部 Adreno740 kits）保持 `-D` 不变。
**关键坑**：make 不会因变量变化重编已存在的对象，第一次 `build kernel` 后 bootstrap 仍在；
删除 `build/xnu/ios-obj/.../osfmk/DEVELOPMENT/prng_random.o` 强制重编后才生效。

**重编结果**（`build kernel` KERNEL_EXIT=0）：
新内核 `090db15c…` / 53,035,008 B，`_source_corecrypto14_bootstrap`、
`_source_corecrypto14_status` 全消失，`__fips_hmacs` 段消失、不再生成 `crypto-hmac.json`，
**EARLY_BOOT 表与 `nofs1` 逐项一致（15 项，无增删）**；
4 个 Adreno 构造器、`_AdrenoPM4Type4/7`、`_native_gpu_dcvs` 全部保留，内核结构审计通过。

**待刷镜像 `fw/vendor_boot_gpu_tier1d_nopost.img`**（`4f6467bd…`，64,811,008 B / VNDRBOOT v4）：
- cmdline **984 B / 48 参**：3 个 GPU 参数（`sheng-fb-gpu=1` / `sheng-gpu-power=9` /
  `sheng-gpu-dcvs=1`），两个 `offline-mobile-graphics-*` 全保留，`board.toml` 零改动；
- FAT 只有 4 个文件（无 `crypto-hmac.json`），ADT 13,940 B 带
  `PackedRAMDisk(16) + TrustCache(16)`，trust cache `7416ca4f…`、Data seed `476464c3…`；
- 以 `nofs1` 为 base，payload 与内嵌 ramdisk 逐字节一致。

**当前状态**：按用户「先不刷，继续离线分析」，**设备未再被写入**（账本最后一条仍是
`05:49:32` 的 boot）。授权后执行：

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img \
  bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier1d_nopost.img
```

判据：进桌面 ⇒ 坐实 EARLY_BOOT POST 是挂死根因，且 GPU 内核可用；仍黑屏 ⇒
POST 无罪，改刷 `fw/vendor_boot_diag_earlyboot.img`（`560d987d…`）定位挂点。
失败随时回退 `nofs1` + 同版 17 线 UEFI。**该判据已于 §12 上机命中「进桌面」。**

---

## 12. Tier-1d 上机成功：内核挂死已解决（2026-11-11 06:11）

| 时间 | 动作 | 结果 |
| --- | --- | --- |
| 06:11:05Z | `flash vendor_boot fw/vendor_boot_gpu_tier1d_nopost.img`（`4f6467bd…`） | `rc=0` |
| 06:11:05Z | `fastboot boot fw/boot-17line-043f7120.img`（17 线 UEFI） | `rc=0` |
| — | 屏幕 | ✅ **进桌面 / 锁屏** |
| — | 按键 | 画面正常，无需按 |

刷入的是 `090db15c…` 内核（EARLY_BOOT 表与 `nofs1` 逐项一致、GPU kits 全保留），
cmdline 984 B / 48 参，含 `sheng-fb-gpu=1` / `sheng-gpu-power=9` / `sheng-gpu-dcvs=1`，
两个显示参数全保留，ADT 带 `PackedRAMDisk`。

**结论（已由设备证实）**：新 GPU 内核「背光亮全黑 + 按键无反应」的根因就是
`source_corecrypto14_bootstrap` 的 `EARLY_BOOT/STARTUP_RANK_FIRST` POST；
只给 `prng_random.o` 加 `-U` 把该表项摘掉后，内核正常进桌面。
§9（ADT `PackedRAMDisk`）→ §10（显示注册参数）→ §11（EARLY_BOOT POST）三处启动链
问题至此全部闭环。`UniXNU-27` / `UniXNU-uke` / 框架仓 `payload.py` 全程未改。

### 12.1 GPU 运行时验证：离线已判定「未接管」

**进桌面 ≠ GPU 驱动已接管**，而且这一条现在**不用上机就能判定**：只读解包当前刷入的
`fw/vendor_boot_gpu_tier1d_nopost.img`，`nm -m` 显示 `_native_legacy_gpu_offer` 是
**weak 空桩**（`LegacyDisplayStubs.c:59`，返回 false），`NativeLegacyDisplay.cpp:297`
因此每帧都走 `:311` 的 CPU copy。Adreno740 的 30+ 个 kits 确实编进了内核，
但显示 hand-off 桥没接。完整证据链与现成修法见 **§13**。

同时更正本节初版的两处错误假设：

- `PREVIOUS XNU LOG` **在这台机器上实测不通**：`HANDOFF.md` §28.5、
  `NEXT-PROMPT.md:641`、`_archive/notes/handoffs/HANDOFF-2026-10-25.md:248`
  均记录 pram 日志跨复位丢失（`magic=0 bytes=0`、`last_panic_text_len == 0`）。
  `sheng-log=1` 的 writer 在 `package/BootLog.c`，写进 UFS，但读回依赖 11-08 已放弃的
  Linux log reader（`ABANDONED-CHANNELS.md`），**主机侧拿不到**。
- 「面板 48 行诊断页」在 17 线实刷配置里**没启用**：`SCREEN-ONLY.md` §3 末段明确
  `CONFIG_XNU_BOOT_PROGRESS` 在各 muyu defconfig 里 `is not set`，`boot_progress_ext`
  只是 weak 数组，写了也没人画。当前面板实际只有 XNU 控制台 + `STACK64` 段 +
  `MUYU ...` 探测行。

**当前设备状态**：正常 XNU 桌面，未再写入；账本最后一条是 `06:11:05` 的 boot。
失败回退命令仍是 `MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu
fw/vendor_boot_nofs1.img`。

---

## 13. GPU 运行时离线审计：驱动在内核里，hand-off 链没接（2026-11-11）

### 13.1 结论

**Tier-1d 进桌面是 CPU copy 渲染，GPU 没有接管显示。** 这不是「待上机验证」的悬念，
是对当前运行内核做符号级离线检查得到的确定结论。内核侧 GPU 驱动（30+ 个
`NativeAdreno*.kit.o` / `AdrenoPM4` / `MuyuAdrenoKernelShims`）**全部链接成功**，
但显示 hand-off 的桥、presenter 实例、用户态 GPU 客户端三环都缺。

### 13.2 取证方法（全程只读，未写设备）

`UniXNU/out/muyu/payload/payload.img`（60 MiB FAT）与刷入镜像的 vendor ramdisk
**逐字节一致**（在 `fw/vendor_boot_gpu_tier1d_nopost.img` 偏移 4096 起，
`v[i:i+len(p)] == p`）。只读挂载后取
`/Volumes/MUYUESP/EFI/UniXNU/Payloads/kernel.macho` 做 `strings` / `nm -m`，
完成后已 detach，无残留挂载。

> 注：FAT 里的 `kernel.macho` sha 为 `7383eb7a…`，`payload.json` 的 `kernel_sha256`
> 是 `090db15c…`（构建阶段内核）；payload 阶段会写 cmdline 到内核，故两者不同，
> 不影响结论——以 vendor_boot 里实际字节为准。

`nm -m` 关键结果：

| 符号 | 类型 | 含义 |
| --- | --- | --- |
| `_native_legacy_gpu_offer` | **weak external** | `LegacyDisplayStubs.c:59` 空桩，返回 false ⇒ 逐帧 CPU copy |
| `_native_legacy_gpu_fallback_observed` | **weak external** | 连回退计数都没接 |
| `_sheng_gpu_power_probe` | **weak external** | `package/GpuPowerProbe.cpp` 未链接 |
| `_native_gpu_dcvs` | **weak external** | `MuyuAdrenoKernelShims.c` 的 shim，非真 governor |
| `_native_gop_gpu_offer` | strong external | `NativeGOPPresenter.cpp:204` 在 |
| `_native_gop_presenter_create` | strong external | presenter 工厂在 |
| `native_gop_gpu_{offers,completions,failures,abandoned}` | external | 计数器在，但 offer 走不到，值应为 0 |
| `_muyu_gpu_power_probe` | strong external | `MuyuGpuPowerProbe.cpp` 在，但 `muyu-gpu-power` 未给，不跑 |

`strings` 佐证：`GPU m%u mapped`（`GpuPowerProbe.cpp:1709`）在两个内核里 **0 命中**；
`libAdreno740.a` 的 30+ 个 `.kit.o` 路径全在当前内核里；`sheng-gpu-dcvs` 只出现
1 次（就是 cmdline 本身），**没有解析代码**。

### 13.3 三个 GPU 参数的真实状态

| 参数 | 17 线解析点 | 实际效果 |
| --- | --- | --- |
| `sheng-fb-gpu=1` | `NativeGOPPresenter.cpp:94` → `gpuMode=(v==1)` | ✅ 参数被读，但 presenter 只在 `LegacyDisplayProbe.cpp:511` 的 **`phase==3`** 分支创建，而当前 cmdline 是 `offline-legacy-phase=4` ⇒ **presenter 根本没 start**；再叠加 offer 是 weak 空桩 ⇒ 必然 CPU copy |
| `sheng-gpu-power=9` | `package/GpuPowerProbe.cpp:147,1695,1724` | ⚠️ 源码有解析，但**该文件没编进当前内核**（空桩 + `GPU m%u mapped` 0 命中）⇒ 无效 |
| `sheng-gpu-dcvs=1` | 仅 `UniXNU-uke/package/GpuPowerProbe.cpp:1772`（27 线） | ❌ 17 线无消费者，纯 27 线参数 |

### 13.4 为什么「观测」比「参数」更是瓶颈

`SCREEN-ONLY.md` 决定唯一输出通道是面板，而当前 17 线可进桌面配置里：
BootProgress 48 行整页未启用（`boot_progress_stage` 也是 weak）、
`PREVIOUS XNU LOG` 实测不通、`sheng-log=1` 写进 UFS 但无读回通道、
`MuyuGpuPowerProbe` 的 `IOLog("MUYU GPU POWER …")` 因未给 `muyu-gpu-power` 不跑。
⇒ **即使 GPU 真的接管了，当前镜像也拿不到机器/屏幕可读的证据**。

### 13.5 现成修法：从同源 17 线 `UniXNU-sheng` 移植（已核对 ABI）

`UniXNU-sheng/package/LegacyDisplayProbe.cpp` 有完整实现，且是 **5 参数** ABI，
与 17 线 `NativeGOPPresenter.cpp:204` / `NativeLegacyDisplay.cpp:38` 完全一致
（27 线 uke 是 6 参数带 `format` 的 `:308`，**不能照抄**）：

```cpp
// :24-27
static IOService *gpuHandoffPresenter;
extern "C" bool native_gop_gpu_offer(IOService *,void *,IOMemoryDescriptor *,uint64_t,uint32_t,uint32_t);
extern "C" void native_gop_presenter_set_source(IOService *presenter,IOService *nub);
volatile uint32_t native_legacy_gpu_fallbacks,native_legacy_gpu_last_status;
// :254-259
bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    return gpuHandoffPresenter && native_gop_gpu_offer(gpuHandoffPresenter,display,memory,stride,width,height);
}
void native_legacy_gpu_fallback_observed(int status) { /* 计数 */ }
```
再加 `:545-558` 的逻辑：在 `started && phase==4` 且 `sheng-fb-gpu=1` 时
`native_gop_presenter_create()` → `set_source(handoff,nub)` → `attach(home)` →
`start(home)` → `gpuHandoffPresenter=handoff`。muyu-17 的对应插入点是
`package/LegacyDisplayProbe.cpp:552-557`。

**安全性已核对**：`NativeGOPPresenter::offer()` 第 134 行
`accepted = online && gpuClient && !swap.offered`；没有用户态 GPU 客户端时
`gpuClient=false` ⇒ offer 返回 false ⇒ display 自动回退 CPU copy，**补桥不会黑屏**。

### 13.6 修复路线图（补桥只是第一步）

1. **补内核桥**（§13.5）：让 `native_legacy_gpu_offer` 变 strong、phase 4 创建 presenter。
   安全，但单独做仍看不到 GPU 接管。
2. **接用户态 scanout client**：`gpuClient` 只在 `NativeGOPPresenter::waitSwap()`
   （`:150`）被置位，调用方是 `UniXNU/drivers/AdrenoMetal/GpuScanoutClient.h` 的
   用户态客户端。§8 记录的 Tier-2 现状是「底座就绪、未串 board.toml、未接 addon」，
   **这一步才是 GPU 真正接管的条件**。
3. **建观测通道**：把 `native_gop_gpu_offers/completions` 计数送到面板
   （启用 BootProgress 层风险大，见 `SCREEN-ONLY.md` §3；更轻的是复用
   `entry_probe_fb_*` panic renderer 或 `muyu-gpu-power` 的 `IOLog` 行）。
   没有它，第 2 步做成了也无法证明。

GMU pool `0xC8000000` / 32 MiB 是否空闲、`sheng-gpu-power=9` 能否跑起来，
都要等第 1–3 步之后才有意义。

§13 是**审计快照**（当时未改源码、未写设备）；随后用户选择「补桥 + 观测 + 出镜像」，
第 1、3 步已离线实施，见 **§14**。第 2 步（用户态 scanout client）仍未做。

---

## 14. Tier-1e：补 hand-off 桥 + 面板观测（离线完成，待上机）

### 14.1 改动清单（只落 17 线）

| 文件 | 改动 | 行数 |
| --- | --- | --- |
| `UniXNU-muyu-17/package/LegacyDisplayProbe.cpp` | ① 头部加 `gpuHandoffPresenter`、`native_gop_gpu_offer`（**5 参数**）、`native_gop_presenter_set_source` 声明与 4 个计数变量；② 在 `extern "C" {` 之前加 `native_legacy_gpu_offer` / `native_legacy_gpu_fallback_observed` 的**显式 `extern "C"` 强定义**（替换 `LegacyDisplayStubs.c` 的 weak 空桩）；③ 在 `if(started && phase==4)` 里仿 `UniXNU-sheng:545-558` 创建 handoff presenter | +37 |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | 在 `muyu_dump_later()` 的 5 s 循环里加 6 个 GPU 计数的 `weak_import` extern，并在 `MUYU LIVE` 之后打印一行 `MUYU GPU calls=… refused=… o=… c=… f=… fb=…` | +17 |

**同步策略**（§0.1）：组装树 `UniXNU/out/muyu/tree/kits/muyu/` 与 17 线 `package/`
是**硬链接**（同 inode），编辑自动生效；主 checkout `UniXNU-muyu/package/` 属 27 线，
曾误同步后已用 `git show HEAD:package/...` 恢复，`git status` 两文件 clean。
**未改** 框架仓 `UniXNU/`、`UniXNU-27`、`UniXNU-uke`、`payload.py`、`board.toml`。
备份在 `/tmp/muyu-handoff-1794378882/`。

### 14.2 关键代码

```cpp
// package/LegacyDisplayProbe.cpp（:27-31, :60-70）
static IOService *gpuHandoffPresenter;
extern "C" bool native_gop_gpu_offer(IOService *,void *,IOMemoryDescriptor *,uint64_t,uint32_t,uint32_t);
extern "C" void native_gop_presenter_set_source(IOService *presenter,IOService *nub);
volatile uint32_t native_legacy_gpu_fallbacks,native_legacy_gpu_last_status;
volatile uint32_t native_legacy_gpu_offer_calls,native_legacy_gpu_offer_refused;

extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *memory,uint64_t stride,uint32_t width,uint32_t height) {
    native_legacy_gpu_offer_calls=native_legacy_gpu_offer_calls+1;
    bool accepted=gpuHandoffPresenter && native_gop_gpu_offer(gpuHandoffPresenter,display,memory,stride,width,height);
    if(!accepted)native_legacy_gpu_offer_refused=native_legacy_gpu_offer_refused+1;
    return accepted;
}
```
phase 4 分支（`:580-590`）里 `sheng-fb-gpu=1` 时 `native_gop_presenter_create()`
→ `set_source(handoff,nub)` → `attach(home)` → `start(home)` → `gpuHandoffPresenter=handoff`。
**不挂到 display-subsystem 节点下**（sheng int-075/077：那会让 SpringBoard 找不到 display）。

```c
/* package/BravearkLastLog.c muyu_dump_later() 循环 */
printf("MUYU GPU calls=%u refused=%u o=%llu c=%llu f=%llu fb=%u\n", ...);
```

### 14.3 为什么选 `muyu-dump-at` 做观测

`muyu_dump_later()`（`BravearkLastLog.c:574-636`）在 `at` 秒后
`muyu_console_take_panel()` + `muyu_console_wake()`，先打一次 LASTLOG，
随后**每 5 秒重绘一行 `MUYU LIVE` / `MUYU GPU`**；不 panic、不 reset、不 stackshot，
"屏幕可以慢慢拍照"。这是 17 线现成且实测可用的面板通道，比启用
`CONFIG_XNU_BOOT_PROGRESS`（`muyu-full-plus_defconfig` 编不过）风险低得多。

cmdline 把 17 线无消费者的 `sheng-gpu-dcvs=1` 换成 `muyu-dump-at=20`：
**983 B / 48 参**（934 B 基线 + 3 个注入参数），仍在 1000 B 闸内；
`sheng-fb-gpu=1`、`sheng-gpu-power=9` 与两个 `offline-mobile-graphics-*` 全保留。

### 14.4 构建与离线校验（全绿）

```
python3 UniXNU/unixnu build all -D UniXNU-muyu-17 \
  --uefi /Users/diannao/pad7pro-xnu/Mu-Silicium \
  --edk2 /Users/diannao/pad7pro-xnu/Mu-Silicium/Mu_Basecore \
  --data-ramdisk /Users/diannao/pad7pro-xnu/fw/data-seed.img \
  --trust-cache /Users/diannao/pad7pro-xnu/fw/trust-21H461/21H461-original-trust.bundle \
  --unique-chip-id 0000000000000001 \
  --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-power=9 \
  --kernel-arg muyu-dump-at=20 -j 8
```
`BUILD_EXIT=0`（日志 `/tmp/muyu-handoff-build.log`）。打包前删除了
`LegacyDisplayProbe.o` / `BravearkLastLog.o` 与两个 archive，避开 make 不因变量
变化重编的坑（§11.8 教训）。

| 校验项 | 结果 |
| --- | --- |
| 构建内核 sha | `e886af3d…`（`payload.json` / build 日志） |
| cmdline | **983 B / 48 参**，`sheng-fb-gpu=1` + `sheng-gpu-power=9` + `muyu-dump-at=20` |
| `_native_legacy_gpu_offer` | ✅ 从 **weak external** 变 **external**（强定义生效） |
| `_native_legacy_gpu_fallback_observed` / `_native_legacy_gpu_offer_calls` | ✅ external |
| `_native_gop_gpu_offer` | ✅ external |
| `MUYU GPU calls=…` 字符串 | ✅ 在内核里 |
| EARLY_BOOT 表 / `source_corecrypto14_bootstrap` | ✅ **15 项 / 0 命中**，POST 修复未回退 |
| ADT | 13,940 B，`PackedRAMDisk(16) + TrustCache(16)` + `gmu-memory` |
| trust cache / data | `7416ca4f…` / `476464c3…`，scratch 头 `DBRDISK1` |
| FAT 文件 | 4 个：`kernel.macho` / `.adt` / `.trustcache` / `scratch-data.img` |
| 独立镜像 | `fw/vendor_boot_gpu_tier1e_handoff.img`（64,811,008 B，sha **`5dbfb604…`**），ramdisk 与 `payload.img` 逐字节一致（偏移 4096） |

### 14.5 上机预期与判读

> 🔴 **这个镜像不会进桌面**：约 t+20 s 由 `muyu_console_take_panel()` 接管面板，
> 显示 LASTLOG + 每 5 秒刷新的 `MUYU LIVE` / `MUYU GPU` 诊断行。这是**设计行为**，
> 不是挂死；屏幕会一直亮着、可以拍照。

| `MUYU GPU` 行 | 判读 | 下一步 |
| --- | --- | --- |
| `calls=0` | 桥没被调用（display 没走 offer 路径） | 查 `gpuBypass` / swap 门控 |
| `calls>0 refused>0 o=0` | ✅ 桥通了，但**没有用户态 GPU 客户端**，`offer()` 的 `gpuClient=false` ⇒ 仍 CPU copy | 做 §13.6 第 2 步：接 `AdrenoMetal/GpuScanoutClient` |
| `o>0 c>0 f=0` | ✅ **GPU 真正接管显示** | 再查 GMU pool / power / 稳定性 |
| `f>0` 或 `fb>0` | GPU copy 失败后回退 CPU copy | 查 offer/complete 时序与 SMMU |

### 14.6 上机命令与回退（**尚未执行，需用户授权**）

```
# 设备需先长按电源 10–15 s 回 fastboot，确认在线
MUYU_UEFI=fw/boot-17line-043f7120.img \
  bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier1e_handoff.img
```
回退到已验证可进桌面的 17 线三件套：
```
MUYU_UEFI=fw/boot-17line-043f7120.img \
  bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_nofs1.img
```
账本随后在 `2026-11-11T06:43:56Z` 记入 Tier-1e 的 flash + boot（均 `rc=0`）；
上机结果与全零根因见 **§15**。

---

## 15. Tier-1e 上机：全零根因与 Tier-1e2 增强诊断

### 15.1 上机事实（2026-11-11 06:43）

| 项 | 结果 |
| --- | --- |
| `flash vendor_boot fw/vendor_boot_gpu_tier1e_handoff.img`（`5dbfb604…`） | `rc=0` |
| `fastboot boot fw/boot-17line-043f7120.img` | `rc=0` |
| 屏幕 | ✅ **进了桌面**；`MUYU GPU` 行出现过几次，但**几次全部为 0** |

用户原话：「已经进桌面了，没注意，但 muyu 行是出现了几次的，但几次都为零」。

### 15.2 为什么全零不能判定桥失败

`calls` 是在 `native_legacy_gpu_offer()` **入口第一行**累加的，与 presenter、
GPU 客户端都无关；`calls=0` 只说明 `NativeLegacyDisplay.cpp:297` 没被调用。
真正的根因在显示侧：

```cpp
// package/LegacyDisplayProbe.cpp:249-257
__attribute__((noinline)) void native_legacy_transfer_observed(IOReturn status,...) {
    native_legacy_transfer_calls=native_legacy_transfer_calls+1;
    // A frame reached the scanout, so the render server owns the panel: park the
    // kernel text output so it stops painting over it.
    if(status==kIOReturnSuccess && !native_legacy_console_quiet) {
        unsigned wanted=1;PE_parse_boot_argn("offline-legacy-console-quiet",&wanted,sizeof(wanted));
        if(wanted && &vc_set_text_quiet) {vc_set_text_quiet(1);native_legacy_console_quiet=1;}
    }
    ...
}
```

`muyu_dump_later()` 在 t+20 s 先 `muyu_console_wake()`（unpark）打几行
`MUYU LIVE` / `MUYU GPU`，但**第一帧成功到达 scanout 后立刻 park 文本控制台**，
诊断行随即被桌面覆盖 —— 用户看到的正是「出现几次、都为零、然后进桌面」。
而该机启动很慢（刷机脚本提示 touch 45–95 s），t+20 s 大概率早于首帧合成，
所以那几行里的 `calls` 必然是 0。**这是观测窗口与显示链错开，不是桥接失败。**

次要嫌疑（增强镜像一并排除）：`BravearkLastLog.o` 对 6 个 GPU 计数都是
`(undefined) weak external`；虽然最终内核里都有强定义，仍需要一个
`nolink` 指示来证明运行时确实绑定。

### 15.3 Tier-1e2 的三处针对性改动

1. **关掉 park，让诊断行持续可见**：cmdline 注入 `offline-legacy-console-quiet=0`
   （`:255` 的 `wanted` 变成 0，不再 `vc_set_text_quiet(1)`）。
2. **把 dump 推到显示链启动之后**：`muyu-dump-at=20` → `muyu-dump-at=60`
   （`TOUCH-PORT.md:336` 记录过 `muyu-dump-at=60` 实测可用），让 LASTLOG 首次
   快照里已经有帧传输历史。
3. **消灭 weak_import 绑定歧义 + 留 msgbuf 证据**：
   - `LegacyDisplayProbe.cpp` 新增强符号 `extern "C" void muyu_handoff_snapshot(char*,unsigned)`，
     在**自己的强计数旁边**一次读完全部 10 个值并格式化。
   - `BravearkLastLog.c` 的 `MUYU GPU` 行改为一次 `weak_import` 函数调用；
     函数不在就打印 `nolink`，在就打印快照 + `w=`（旧的直接变量读，作对照）。
   - `native_legacy_gpu_offer` / `native_legacy_transfer_observed` 各加限流
     `IOLog("MUYU OFFER …")` / `IOLog("MUYU SWAP …")`（前 3 次 + 每 64/128 次），
     写进 msgbuf，即使文本控制台再被覆盖，LASTLOG 也能回放历史。

cmdline 用 `offline-legacy-console-quiet=0` 换掉 17 线无消费者的
`sheng-gpu-power=9`：**996 B / 48 参**，仍在 1000 B 闸内；
`sheng-fb-gpu=1` 与两个 `offline-mobile-graphics-*` 全保留。

### 15.4 离线校验（全绿）

`build all` EXIT=0（日志 `/tmp/muyu-tier1e2-build.log`），内核 `b2846224…`：

| 校验 | 结果 |
| --- | --- |
| `_muyu_handoff_snapshot` / `_native_legacy_gpu_offer` | ✅ external（强符号） |
| `MUYU OFFER` / `MUYU SWAP` / `MUYU GPU %s` / `nolink` 字符串 | ✅ 各 1 处 |
| EARLY_BOOT / `source_corecrypto14_bootstrap` | ✅ 15 项 / 0 命中 |
| cmdline | 996 B / 48 参 |
| 镜像 | `fw/vendor_boot_gpu_tier1e2_probe.img`，64,811,008 B，sha **`8a0f6ff0…`**，ramdisk 与 payload 逐字节一致 |

备份：`/tmp/muyu-tier1e2-1794379859/`（Tier-1e 源码）。

### 15.5 上机判读

| 屏幕 | 判读 |
| --- | --- |
| `MUYU GPU nolink` | 跨 archive 函数没链接，之前的 0 是假值 |
| `MUYU GPU calls=… tc=0` | 显示链还没出帧（启动慢/被 take_panel 冻结），继续等几轮 |
| `MUYU GPU calls=0 tc>0` | 帧在走但 offer 路径被跳过 ⇒ 查 `gpuBypass` / `:284-291` 前置检查 |
| `MUYU GPU calls>0 refused>0 o=0` | ✅ 桥通、presenter 在，但 `gpuClient=false` ⇒ 仍 CPU copy，需 §13.6 第 2 步 |
| `MUYU GPU o>0 c>0` | ✅ GPU 真正接管显示 |
| LASTLOG 里出现 `MUYU OFFER` / `MUYU SWAP` | msgbuf 证据链成立，可按行数还原时序 |

### 15.6 命令与回退

设备已由用户长按电源 10–15 s 主动送回 fastboot（`fastboot devices` 可见
`a20248f7`），即本节的上机前置条件已满足：

```
MUYU_UEFI=fw/boot-17line-043f7120.img \
  bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier1e2_probe.img
```
回退：`MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu
fw/vendor_boot_nofs1.img`。

**判读要点**：因为 `offline-legacy-console-quiet=0` 关掉了首帧 park，这次
诊断页会持续可见（不再被桌面覆盖），`MUYU LIVE` / `MUYU GPU` 每 5 s 刷新；
dump 推到 t+60 s，落在合成器出帧之后。请把整行数值抄回来，按 §15.5 判读。

### 15.7 上机结果（2026-11-11 06:57 UTC / 14:57 本地）：桥通，缺用户态客户端

| 项 | 结果 |
| --- | --- |
| `flash vendor_boot fw/vendor_boot_gpu_tier1e2_probe.img`（`8a0f6ff0…`） | `rc=0` |
| `fastboot boot fw/boot-17line-043f7120.img` | `rc=0` |
| 用户读屏 `MUYU GPU` 行 | **`calls>0 refused>0 o=0`** |

判读（§15.5 第 4 行）：

- `calls>0` ⇒ `NativeLegacyDisplay.cpp:297` 的 offer 路径**确实被每帧调用**，
  14 版 weak 空桩已彻底消除；`muyu_handoff_snapshot()` 强符号绑定成功（没有 `nolink`）。
- `refused>0` ⇒ `NativeGOPPresenter::offer()`（`:131-141`）返回 false。它的判据是
  `online && gpuClient && !swap.offered`；presenter 已 `gpuMode/online`（否则
  `sheng-fb-gpu=1` 下根本不会创建），所以 **唯一未满足的是 `gpuClient`**。
- `o=0 / c=0` ⇒ 没有任何帧被 GPU 客户端接收或完成，显示仍是 CPU copy 回退。

**根因链（离线已完整定位）**：`gpuClient` 只在
`NativeGOPPresenter::waitSwap()`（`drivers/GopFramebuffer/NativeGOPPresenter.cpp:150`）
被置 true，而 `waitSwap()` 只能由用户态调用方经
`AGXDeviceUserClient::runTimed()`（`UniXNU/drivers/Adreno740/NativeAdrenoPublish.cpp:750`，
selector `0x7000 + NGP_METHOD_WAIT_SWAP = 0x7002`）→
`native_gop_handoff_call()`（`:240`）→ `handoffCall()`（`:211`）进入。
现成的用户态调用方在
`UniXNU/drivers/AdrenoMetal/NativeMetalBridgePhase.h:120`：
`gpuscanout::Client client; client.open(...); while(client.serveOne(500))`，
它用 `link.method(0x7000+method, ...)` 作 Transport、用 Metal 设备做 GPU copy，
由 `NativeAdrenoMetalPlugin.mm` 在 **Metal 插件进程**里启动。17 线的 root image
是原厂 21H461，这个插件链（Turnip bridge self-test → frontend checks →
scanout phase）**没有在跑**，所以内核侧 `gpuClient` 永远为 false。

**结论**：§13.6 三步路线图的第 1 步（内核 hand-off 桥）与第 3 步（面板观测）
都已由设备验证通过；**第 2 步「接用户态 scanout client」是 GPU 真正接管的唯一剩余缺口**。
这不是内核 bug，也不需要再改 `LegacyDisplayProbe.cpp`。

账本最后一条为 `2026-11-11T06:57:54Z`（flash + boot 均 `rc=0`）；
失败回退 `MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu
fw/vendor_boot_nofs1.img`。

---

## 16. Tier-2 接线离线打通（2026-11-11 15:09，未写设备）

用户选择「离线打通 Tier-2 addon」。结论：**payload / ADT / owner-trust 三段接线已全部
离线跑通并逐项校验；剩余阻塞是外部编译器工具链的重建，不是 17 线内核或 payload 的缺陷。**

### 16.1 内核侧其实已经就绪（纠正早前判断）

`sheng-gpu-addon=1` 的消费者就在当前运行内核里：
`UniXNU/out/muyu/tree/.cache/xnu-work/bsd/kern/kern_exec.c:7691` 的
`native_gpu_addon_prepare()` 检查该 boot-arg，然后 `native_gpu_addon_install()` 把
addon 写进 Data 卷，并把 `/private/var/ngpu/addons` 以只读 union 挂到
`/System/Library/Extensions`（供 `NativeAdrenoMetal.bundle/Info.plist` 被发现）、
把 `/private/var/ngpu/daemons` 挂到 `/System/Library/LaunchDaemons`。
只读解包运行内核可见 `native_gpu_addon_prepare` / `native_gpu_addon_stage`、
`/private/var/ngpu/*`、`NativeAdrenoMetal.bundle/Info.plist` 全部在。
**`zzy-native-gpu-addon-mount.patch` 不是阻塞** —— `build-xnu-4k-baseline.sh:48`
的 `LATE_PATCH` 循环只是「先 reverse 再按名序重 apply」的幂等处理，后面的
`for PATCH in "$ROOT"/patches/xnu/*.patch` 会把设备仓
`zzzz-offline-owner-systemos-bringup.patch` 重新应用。早前把它读成「排除」是错的。

### 16.2 第二个 legacy ADT 回归：`GpuAddon`

`UniXNU/scripts/unixnu/devicetree.py:338`（schema 2）有
`mappings += [('GpuAddon', bytes(16))] if addon else []`，但 17 线走 legacy
`scripts/make-muyu-adt.py`，它既**没有** `--gpu-addon` 参数，也**没有**
`GpuAddon` memory-map 项。`payload.py:198-199` 在 `--gpu-addon` 时会向它传
`--gpu-addon`，argparse 会直接以 `unrecognized arguments` 失败。这与 §9 的
`PackedRAMDisk` 是同一类 uke 27 线整理后的 legacy 回归。

修法仍只落设备仓（框架仓 `UniXNU/scripts/unixnu/*` 保持只读）：

```python
parser.add_argument('--gpu-addon', action='store_true', ...)
...
[node('memory-map', (... PackedRAMDisk ...) +
                    ([('TrustCache', bytes(16))] if args.trust_cache else []) +
                    ([('GpuAddon', bytes(16))] if args.gpu_addon else [])),
```

`make-muyu-adt.py` 三树同步，sha `38c05a62…`（备份
`/tmp/muyu-tier2-adt-1794380920/`）。**默认行为不变**：不传 `--gpu-addon`
时 ADT 与此前逐字节相同。

### 16.3 单步 payload 接线验证（全绿）

用真实 `make-gpu-addon.py --publish --no-daemon` 生成的**占位** `NGPUADD2`
（384 B / 3 文件，明确标记为非真实插件）和 `trust-append-module.py` 从
`21H461-original-trust.bundle`（2 模块）追加一个假 CDHash 得到的 owner trust
（3 模块），跑：

```
python3 UniXNU/unixnu build payload -D UniXNU-muyu-17 \
  --uefi .../Mu-Silicium --edk2 .../Mu-Silicium/Mu_Basecore \
  --data-ramdisk fw/data-seed.img \
  --owner-trust-cache /tmp/muyu-tier2-placeholder/owner-trust.bundle \
  --gpu-addon        /tmp/muyu-tier2-placeholder/gpuaddon.bin \
  --unique-chip-id 0000000000000001 \
  --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-addon=1 \
  --kernel-arg sheng-gpu-metal=1 -j 8
```

`PAYLOAD_EXIT=0`，日志 `/tmp/muyu-tier2-payload.log`：

| 校验 | 结果 |
| --- | --- |
| cmdline | **985 B / 48 参**（board 45 参 / 934 B + 3 个注入参数），1000 B 闸内 |
| FAT 文件 | 5 个：新增 `kernel.macho.gpuaddon`（384 B，头 `NGPUADD2`） |
| ADT | **13,992 B**，含 `PackedRAMDisk` + `TrustCache` + **`GpuAddon`**（比 Tier-1e2 的 13,940 B 恰好多一个 52 B 节点） |
| trust | `kernel.macho.trustcache` 61,408 B / **3 模块**，原 2 模块逐字节保留 |
| data / kernel | `scratch-data.img` 头 `DBRDISK1`、`476464c3…`；内核仍是 `b2846224…`（POST 修复未回退） |
| `payload.json` | 新增 `gpu_addon_sha256 = 6202ea45…` |

证据在 `/tmp/muyu-tier2-wiring/`。**占位 addon 的 payload 不可刷**（假插件会被
AMFI 拒绝）；验证完成后已用备份 `/tmp/muyu-tier2-payload-backup-1794380977/`
把 `UniXNU/out/muyu/payload/` 恢复成设备上正在运行的 Tier-1e2（`cmp -s` 逐字节相同，
996 B / 48 参、`gpu_addon_sha256=None`）。

### 16.4 剩余硬阻塞：外部编译器工具链

`build-adreno-metal-plugin.py --target ios`（默认，**就是 17 线要的
`arm64-apple-ios17.0`**）要求先有 `build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib`；
`build-adreno-metal-bridge.py --target ios` 要求 pinned `metal2vulkan` Rust 源码。
本机盘点：

| 前置 | 本机状态 |
| --- | --- |
| `UniXNU/.cache/mesa-work`（pinned `85fda3b3…` + `patches/mesa/*` 16 个 + `mesa_patch_identity` 严格校验） | ❌ 不存在 |
| `UniXNU/.cache/gpu-reference/metal2vulkan`（pinned translator + 21 个 bridge patches） | ❌ 不存在 |
| `.cache/mesa-venv/bin/meson`、`.cache/mesa-tools/bin`、`glslangValidator` | ❌ 全缺（`brew list` 无 meson/glslang/rust） |
| `rustc` / `cargo` | ❌ 全缺（无 `~/.cargo/bin`） |
| 预编译 Turnip / `libadreno_metal_bridge.a` / `NativeAdrenoMetal.bundle` | ❌ 整机无 |
| Xcode iOS SDK | ✅ `iPhoneOS27.2.sdk`（`-target arm64-apple-ios17.0` 可作 deployment target） |
| 网络 | ⚠️ `gitlab.freedesktop.org` / `static.rust-lang.org` 可达；chromium Mesa mirror 超时 |

也就是说：要在本机从零重建 uke 的外挂编译器链，需要克隆两个 pinned 外部仓库、
打 37 个 recorded patch、装 Rust + meson + glslang，再交叉编译 Mesa/Turnip 与
Rust bridge —— 属于 27 线 Linux 构建机上的数小时级工作，且任一 pin/patch 不匹配
都会被 identity 校验拒绝。**这不是 17 线内核或 payload 的缺陷。**

### 16.5 一旦拿到真实 `NativeAdrenoMetal.bundle` 的最后一公里

接线已验证，所以只需（都不再改内核/ADT 代码）：

```
ADRENO_PLUGIN_DIR=<ios17 bundle 目录> python3 UniXNU/scripts/make-gpu-addon.py \
  --publish --no-daemon fw/gpuaddon-17.bin
python3 UniXNU/scripts/trust-append-module.py \
  fw/trust-21H461/21H461-original-trust.bundle fw/trust-21H461/owner-trust-17.bundle \
  <NativeAdrenoMetal 的 20 字节 CDHash> <libvulkan_freedreno.dylib 的 20 字节 CDHash>
python3 UniXNU/unixnu build payload -D UniXNU-muyu-17 … \
  --owner-trust-cache fw/trust-21H461/owner-trust-17.bundle \
  --gpu-addon fw/gpuaddon-17.bin --unique-chip-id 0000000000000001 \
  --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-addon=1 --kernel-arg sheng-gpu-metal=1 -j 8
python3 UniXNU/out/muyu/tree/scripts/patch-vendor-boot.py \
  --base fw/vendor_boot_nofs1.img --payload UniXNU/out/muyu/payload/payload.img \
  --output fw/vendor_boot_gpu_tier2.img
```

`--owner-trust-cache` 会**替换** `--trust-cache` 并跳过 board allow-list
（`payload.py:125-131`），所以不能两个一起传。真实 CDHash 必须从最终签名后的
bundle / dylib 取（`codesign -dvvv` / 脚本输出的 `trust.json`），占位验证用的假
hash 不能上机。

### 16.6 本机补齐编译工具链（用户选「补齐工具链在本机编译」，进行中）

**系统工具链已全部装齐**（此前本机完全为空）：

| 工具 | 落地 |
| --- | --- |
| Rust | `rustup` 1.98.0（`88d9e12ae`）+ `aarch64-apple-ios` target；`cargo +1.98.0` 可用 |
| meson | `UniXNU/.cache/mesa-venv/bin/meson` 1.12.1（脚本硬编码该路径） |
| bison | `UniXNU/.cache/mesa-tools/bin/bison` 3.8.2（从 `ftp.gnu.org` 源码编译） |
| llvm-as | `/opt/homebrew/opt/llvm/bin/llvm-as`（LLVM 23.1.4） |
| glslangValidator | 16.6.0（brew 单独装；`brew install llvm` 曾卡在 ghcr.io bottle 20 分钟，已停，llvm 23.1.4 本来就够） |

**网络障碍与绕法**（关键，后续复现要用）：

- `github.com` 的 git 协议不通（`ls-remote` 000）、chromium Mesa mirror 超时；
  codeload 直连仅 ~30 KB/s 且**不支持 Range**，900 s 只收到 21.9 MB 即超时 ——
  而 pinned `metal2vulkan` 树有 6693 项 / 1.7 GiB（其中 `*.jsonl` 语料 1.45 GiB）。
- 找到可用代理 `https://gh-proxy.com/https://codeload.github.com/...`（~1.2 MB/s），
  279,087,395 B 的 pinned tarball 5m29s 下完，`gzip -t` 通过、6694 项。
  `gh-proxy.com` / `ghfast.top` 的 git 代理也支持 `ls-remote` / `fetch`。
- Mesa 全量 clone 报 `HTTP/2 stream … PROTOCOL_ERROR / early EOF`；改用
  `git init` + `git -c http.version=HTTP/1.1 fetch --filter=blob:none --depth=1 origin
  85fda3b3…` **19 秒**拿到 pinned commit（不要全量 clone）。

**pinned metal2vulkan 仓库重建成功**（脚本要求 `rev-parse HEAD` 精确等于 pin，
且要 `read-tree`/`apply`/`write-tree`）：

1. tarball 解压 + `git add -A` + `write-tree` ⇒ tree **`942c9483…` 精确匹配** API
   给出的 pinned tree（说明 archive 内容与 git tree 逐字节一致）。
2. 用 API 元数据（`author/committer/message/parents`）手工构造 commit object 得到
   `ce282e89…`，**与 pin 不符** —— 真实 commit 的时间戳带 `+0200` 时区
   （`1793239835 +0200`），API 只给 `…Z`。
3. 改走代理 git 协议 `fetch --filter=blob:none --depth=1 <pin>` 取回**真实 commit
   object**，`update-ref refs/heads/master <pin>`。最终：
   `HEAD=624bab39…` / `tree=942c9483…` / `read-tree` OK / `write-tree=942c9483…` /
   `git status` 干净 / 6369 文件 / 2.1 GiB（含全部 blob）。
   重建脚本 `/tmp/rebuild-m2v-git.py`。

**已实跑通过**：

```
PATH="$HOME/.cargo/bin:/opt/homebrew/opt/llvm/bin:$PATH" \
  python3 UniXNU/scripts/build-adreno-metal-bridge.py --target ios
⇒ BRIDGE_EXIT=0
  PASS: native Metal bridge static library for ios (222607080 bytes);
        translator 624bab3918de
```

产物 `UniXNU/build/adreno-metal-bridge/ios/libadreno_metal_bridge.a`。

**Mesa 侧仍在收尾**：第一次 `git sparse-checkout set $DIRS` 在 zsh 下未做单词拆分，
30 个目录被合成一个畸形 pattern，导致 `src/freedreno` 空白，17 个 recorded patch
应用到第 2 个（`ir3-darwin-build-id.patch`）就报 `ir3_disk_cache.c: No such file`。
已关闭 sparse 并由 promisor remote 批量 lazy-fetch 完整树（12,504 对象）；
待其完成后按 `adreno-compiler-inputs.json` 的 `patches` 顺序重新应用 17 个 patch，
过 `mesa_patch_identity.validate_recorded_patches`，再跑
`build-turnip-darwin.py --target ios`。之后才是
`build-adreno-metal-plugin.py --target ios` → 真实 addon / owner trust → §16.5 打包。

**设备未再写入**，账本最后一条仍是 `06:57:54Z`。
