# 交接文档 · GPU 上机工程冷启动

**状态快照时间：2026-11-12 20:07 北京（12:07 UTC）｜工作区：`/Users/diannao/pad7pro-xnu`｜线：17 线 / 21H461 / **Adreno735****

> ⚠️ **型号口径（§69.35，别再混）**：本板 muyu 的 GPU 是 **Adreno 735**（`0x43030b00` /
> 1.5 MiB GMEM / `gen7-11-0`）；**Adreno 740 是 `sheng` 的**（`0x43050a01` / 3 MiB）。
> `UniXNU/drivers/Adreno740/`、`libAdreno740.a` 只是**沿用 uke/sheng 的共享 A7xx 驱动框架目录名**
> （寄存器映射相同），不是硬件型号声明，不要改名或分叉。
> ⚠️ **§69.43 更正**：本板 `family=ADRENO_7XX_GEN2`，本机 `a7xx_catalog.c` 给**所有 GEN2 条目**
> 配的就是 `a740_hwcg`(54 项)，`a730_hwcg`(53 项) 只给 GEN1 ⇒ **A740 hwcg 表是本板正确家族，
> 不是欠账**；真正的 A740 遗留只有 GMEM 3 MiB，已由 fix2 改成本机 1.5 MiB。详见 §69.35 / §69.43。

> **这份文档是什么**：给「接手者」（人或 AI）的**唯一入口**。读完这一份即可安全续做，不需要先啃完
> `GPU-HANDOFF.md` 的 3900 行逐轮日志。它只写**当前状态、硬规矩、已证伪清单、下一轮动作、判读格**。
> **它不是历史记录**：每一条结论的推导、屏照逐字转录、代码定位，都在 `GPU-HANDOFF.md` 对应章节，按索引去查。
>
> ⚠️ **顶层 `NEXT-PROMPT.md`、`AGENTS.md` §6、`HANDOFF-2026-11-0*.md` 全部过期**（停在 11-02～11-04 的
> 27 线迁移 spike）。`AGENTS.md` 的 §2 官方优先原则和 §5 硬性禁令**仍然有效**，但 §6「当前下一步」不要执行。
> 以本文档为当前状态基准。

---

## 0. 三十秒读完

| 项 | 现状 |
| --- | --- |
| 目标 | 让 17 线（iPadOS 17 / 21H461）上的 **Adreno735** 真正接管渲染，进桌面 |
| 已完成 | ⭐ **内核启动链已打通 + GPU 内核侧已排除嫌疑**：`MUYU BSD s=8`、`INIT q=6`、`ROOT c=6/0/0 d=4/0`、用户态起来（§57）；`vkd1` 屏照给出 `V op=5 bad=0 cl=4 call=105 sel=6 st=00000000 at=1 tr=14 gr=5`、`O SpringBoard\|SpringBoard\|backboardd` ⇒ **内核没有拒绝任何调用**（§59） |
| **当前 blocker** | **`-3` 落在设备创建这一步，且 uke 在这一步是通的**（§66）：九级内核阶梯、Metal 发布、客户端打开与 105 次隧道调用我们全部通过；`uke` 从未在设备创建上撞到 `-3`（他们一挂 addon 就 `o4/0/2` + Turnip 裸测试通过，卡在下一级 CA 管线 `InvalidDescriptor`）⇒ **uke 的补丁一条都用不上**。本轮同时否掉两处旧推断：`Y` 只打最后四次分配，**不能**据此说"没建到 global BO"；`6a3c` 是 `0x6A00\|bits` 的帧缓冲选择位、每轮无条件打，不是失败标记。`68f2` 与 uke 同名不同因（我们无槽位上限、`bad=0`），仅表示每个候选目录都返回 nil |
| 设备 | 22:08 刷 `vkd7`（`5884a6fe…`）并 RAM boot；22:12 屏照已读（§65），**这一 boot 停在用户态之前，未触达 `-3`**。电量 3836 mV，比上一轮好 |
| 账本 | 末条 `2026-11-12T14:08:27Z boot fw/boot-17line-043f7120.img … rc=0` |
| **主机 harness** | ⭐ **已通**（§67）：`UniXNU/scripts/host-turnip-harness/run.sh <dylib>` 一条命令，真实 Turnip 在 Mac 上 `vkCreateInstance`/`vkEnumeratePhysicalDevices`/`vkCreateDevice` 全部成功；13 次 ABI 调用，2 次 open、三组 `2→3→13`，与板上 `pr=2` 和 `Q` 尾部同形。**只改元数据，不动指令、不动构建产物** |
| **下一个动作** | ① **设备侧（最便宜）**：再拍一张 vkd7，读新增的 `MUYU G`（按进程记 授予/拒绝/关闭/最后拒绝原因）——§67.6 推论说 `-3` 可能只发生在特定进程，`G` 正是判它的行；② **主机侧（零刷机）**：用 §67.10 的方法做**输入→错误码敏感性扫描**，把能产出 `-3` 的字段列成候选表；③ **一次性投入**：在驱动加 4 MiB 环形录制 `(method,in,out,kr)`，之后把真实回放在主机上跑 —— 这一步之后刷机只用于验证结论，不再用于定位 |
| 这一轮的目标 | **只定 `-3` 落在 turnip 设备创建的哪一步**；不要重建 addon / trust / turnip / bundle，也不要同时碰别的假说 |

**不要给时间承诺。** 内核启动链已通（§57），但 GPU 交接仍未完成；每轮之间设备要回 rec 充 1–2 h，
实验窗口被供电硬约束；量级以天计。

---

## 1. 资料地图（按需要读，不要通读）

| 想干什么 | 读哪里 |
| --- | --- |
| 快速进入状态 | 本文档 §0–§6 |
| 某条结论怎么推出来的 | `GPU-HANDOFF.md` 对应章节（下表导航） |
| 刷机脚本能做什么、不能做什么 | `tools/muyu-flash-mode.sh` 文件头注释（权威） |
| 上机手册 / 判读表 / 仓库边界 | `docs/GPU-ONDEVICE.md` |
| 与 uke 27 线的差异 | `docs/GPU-UKE-ALIGNMENT.md` |
| 已拆除的日志通道（别重启） | `docs/ABANDONED-CHANNELS.md`、`docs/SCREEN-ONLY.md` |
| 27 线迁移 / 上游 sheng 方案 | `docs/IPADOS27-MIGRATION.md`、`GPU-HANDOFF.md` §34 / §42 |

`GPU-HANDOFF.md` 章节导航（前 32 节无 `§` 前缀）：

- **§0–§10**：Tier-1 交接、仓库红线、工具链、已知坑、上机闸门（**§1 红线、§7 闸门、§8 坑必读**）
- **§11–§23**：GMU 固件、SMMU attach、上下文到 Ready、`input_bits` 修正、首个 `-3` 出现
- **§24–§33**：换表 / CP ring / fence 整族实验，**结论是整族证伪**（§33 结案）
- **§34–§38**：uke 历史复盘、ABI 订正、WaitFence 链、CP 硬件诊断、reassert1 证伪 + 找到缺 restart hook
- **§39–§43**：restart1 刷入与证伪、离线/虚拟环境研究、上游盘点、restart2 构建
- **§44–§48**：restart2 屏照、原厂刷回、restart3 构建、restart1 对照、13:48 屏照**初判（已被 §49 推翻）**
- **§49**：⭐ **最重要**。用户补充观察后修正为「稳态卡滞」，字段精确解码、三 boot 对比、断点重定位
- **§50**：原厂刷回、充电交接
- **§51**：⭐ 关卡表 + `upper1` 镜像的字段定义与判读格
- **§52**：⭐ `upper1` 屏照判读：uptime 260 s、`PROC n=1`、未见 launchd；启动链离线追查方向
- **§53**：`MUYU ROOT` 镜像的构建记录
- **§54**：⭐⭐ `root1` 实刷与判读 + **「LIVE ticker 不证明 init 入口」更正** + `init1` 判读格
- **§55–§57**：`init1`/`bsd1` 判读 → root 设备发现断点 → ⭐ **8 KiB manifest 修补生效，启动链打通**
- **§58**：`vkd1` 把驱动早就在记、从没上屏的客户端事实打出来（`V`/`O`/`A` 三行）
- **§59**：⭐⭐ `vkd1` 判读：**内核没有拒绝任何调用**，`-3` 回到客户端栈
- **§60**：`vkd2` 判读：publish 模式不跑 Turnip 冒烟，`K` 行 `6xxx` 分支不可达（自我更正）
- **§61**：⭐⭐ `vkd3` 判读：**隧道是通的**（`pr=2 tn=96` 全成功），`C` 行是死指针（自我更正）
- **§62**：⭐ `vkd4` 判读：`WaitFence` 时间线（后经 §63 修正：超时是轮询的正常回答）；`abi=2` 划掉版本不匹配
- **§63**：⭐⭐ `vkd5` 判读：**环形是干净的**（`wf=5/5/5`），栅栏无故障
- **§64**：⭐⭐ `vkd6` 判读（当前基准）：`Q`/`Y` 上屏；**「死在 ir3 compiler」被源码否死**（该函数不可能返回 NULL），`-3` 收窄到 `dw_open_connection()` 的第 1/3 条；下一轮 `vkd7` 打 `mount_fail` / `client_text`
- **§65**：`vkd7` 第一张屏照：**`-3` 未复现**，这一 boot 用户态图形栈未起来（`UP op=0/0`、`V call=0`、`A n=1`）；顺带纠正 `A` 行的 `r=` 是沙箱门返回值
- **§66**：⭐⭐ `uke` mode 9 与我们 `MUYU` 行逐项对照（当前参考）：**九级阶梯 + 隧道我们全过，断点唯一是设备创建**；uke 从未在这一格失败 ⇒ 其补丁一条都用不上；并纠正三处过度推断（`Y` 窗口太窄、`6a3c` 不是失败标记、`68f2` 同名不同因）
- **§67**：⭐⭐⭐ **Turnip 已在 Mac 主机上跑起来**（当前最强工具）：`vtool` 改 `LC_BUILD_VERSION` + `codesign -f -s -` + dyld interpose 8 个 IOKit 符号 ⇒ 真实 dylib 在无 Adreno 服务的主机上跑到 **`vkCreateDevice` 成功**，序列与板上同形（2 次 open、三组 `2→3→13`）。改 `abi_version` 即可定点复现 `-3`；`chip_id` 未知则走 `-9`。脚本 `UniXNU/scripts/host-turnip-harness/run.sh`
- **§68**：⭐ `uke` 的虚拟环境做了什么（当前最重要的一条线索）：它用 **QEMU 整核引导 + 假加速器 broker** 做**归因分离**（拆开「我们的代码 / 苹果的用户态 / 真硬件」三个变量）；四层 = 主机 IR3 编译器 / 主机真 Metal 跑原厂 metallib / QEMU 全系统 + `offline-gpu-broker` / QEMU 只跑机器码。**这套 harness 就在我们树里**：`UniXNU/scripts/probe-darwinboot-xnu.py`（1655 行，含 `--offline-gpu-broker`、`--follow-metal-device`）、`UniXNU/drivers/AdrenoMetal/NativeOfflineGPU.cpp`（312 行 broker）、11 个 `OfflineMetal*Wire.h`、21H461 两卷 dmg 齐全 —— §67 的假 IOKit 只是它的下游
- **§69**：⭐⭐⭐ **XNU 17 已在 QEMU 里跑起来**（当前最强离线工具）：`--offline-gpu-broker` 与 `--follow-target-main` 被源码判为互斥（§68 建议 A 按字面写不出来）；补系统卷挂载 + `launchd` sha 核验 + `kernel.macho.0` staging 后，崩溃点在 **DarwinBoot 控制台**——它把输出镜像到 muyu 静态内存映射里的 `Display_Reserved`（`0xE3940000`+`0x2B00000`，`MemoryMapLib.c:37`），QEMU 1 GiB 里那块不存在。**加 `--qemu-memory-mib 4096` 即 PASS**：`TTBR1=0x4c41d000`、4 KiB 粒度、`stop_symbol=_panic`、12 秒一轮、零刷机。还差「把系统卷挂成 virtio-blk 并认成 root」才能到 launchd（§69.8）
- **§69.9–69.10**：⭐ **「挂 root 只差一个参数」被证伪，QEMU 的真实墙是 CoreTrust**：本板 `[kernel] archives` 没有 `VirtioMmioBlock`（只有 `RamBlock`），探针直接 `Cannot resolve _native_virtio_ready`，补它要 storage-abi 审计 + verified-root 包；`--load-apfs` 也到不了 `_native_apfs_probe_checkpoint`。真正的墙是 **CoreTrust +0x79a4** 的 NULL 虚调用（`ELR 0xfffffff004a92828`、`FAR 0`、`ESR 0x96000005`，五种配置逐位一致）。已排除：trust cache（设备原厂包 7416ca4f… 逐字节相同）、`osenvironment=main`、`--amfi-platform-only`、以及**设备 ADT 全量安全身份**（`certificate-security-mode` / `boot-uuid` / `apfs-preboot-uuid` / `target-type` 等，新加 `--boot-manifest-hash`/`--chosen-property` 两项透传）。HVF 下 QEMU 拒绝软断点 ⇒ 只能 TCG。下一步：查被污染指针的 bits 35:32（`0xfffffff2_…`/`0xfffffff6_…`）
- **§69.11**：⭐⭐ **CoreTrust 那堵墙被打穿 —— 缺的是 `/options` 节点**。先更正 §69.10.4：arm64 的 KVA 是 **64 GiB**（`0xfffffff0_00000000`–`0xffffffff_ffffffff`，TTBR1 窗口本身 256 GiB），所以 `0xfffffff2_…`/`0xfffffff6_…` 都是**合法指针**，「bits 35:32 被污染」不成立；真缺陷是某个设备树节点指针**真是 0**。故障函数所属调用者的报错串给出它要的 5 个节点（`chosen`/`options`/`defaults`/`product`/`chosen/manifest-properties`），而设备 ADT 有、我们的 DT **整个缺 `/options`**（`make-dt.py` 只在 `--nvram-proxy` 时才发）。三组单变量对照：只补 nvram 属性 ⇒ 仍 CoreTrust NULL；只补 `/options` 空节点 ⇒ CoreTrust 过（改撞 `IONVRAMCHRPHandler`，所以 `--nvram-proxy` 两件都要）。越墙后每格 13 秒：AMFI 要 `amfi-only-platform-code`、AppleARMCPU 要 `/cpus/max_cpus` + 每 CPU `interrupts`（用 `no-aic-ipi-required` 如实跳过）。现在内核打印 **86 条**、走到 `bsd_autoconf`/`kminit`/`cpu_start`（Skywalk、DLIL 自检全绿），新墙是 `Platform does not support ACC Fast IPI`（QEMU 无 Apple ACC，不是 DT 数据问题）。工具新增 `--node-property`、`--copy-node-from`、`--log-kprintf`（内核无串口，`_kprintf` 的 x0 就是格式串）
- **§69.12**：⭐⭐⭐ **三处旧结论作废，21H461 内核 GPU 链整条跑通（零改码、每格 14 秒）**。先作废：§69.9.2「`--load-apfs` 到不了 checkpoint」（CoreTrust 一过就到）、§69.11.8「ACC Fast IPI 必须改内核」（实际只是 DT 缺 `/arm-io/intc`，加现成的 `--armcpu` 即消失）、§69.11.6 的最优命令（缺 `--armcpu` + `sheng-pmgr=2`）。`--armcpu` 让 ArmCpu/GICv3 接管，AMFI 改报 `No PMGR?`，用移植层已有 opt-in 仪器 `--kernel-extra-args "sheng-pmgr=2"` 跳过（⚠️ 是主动开启的诊断仪器，不是 DT 修复、不是原厂行为；另一块板的 checker 反而断言必须保留这个原厂拒绝）。内核打印 43→87 条，走到 `IOKitBSDInit` 的 `Failed to mount root device`（`args` 无 `rd=`，本板也没有 `native-user-fixture` / `_native_user_checkpoint` ⇒ md0 root 挂不上，这是新的真墙）。**四个 checkpoint 全通**：`_native_apfs_probe_checkpoint`(…728188)、`_native_iogpu_probe_checkpoint`(…72818c)、`_native_mobile_graphics_probe_checkpoint`(…728190)、`_native_mobile_graphics_client_checkpoint`(`0xfffffff007978084`)；`load_status=0`、`kmod_start` 真命中、入口字节逐字节匹配。客户端：`abstract_null_mask=7`、`client_vptr=0xfffffff005f34dd0`、`client_meta=0xfffffff006b313b0`、`client_size=0x538`、`selector_results=[0xe0000001]×3`、三个抽象工厂 `MetaClass::alloc` 全命中并返回 NULL —— 与另一块已验证板的官方 `check-mobile-graphics-client-checkpoints.py` **逐位一致**，`externalMethod` 的 `x0` 与 `client_object` 同指针（真对象真调用）。报告仍是 `os_boot_demonstrated: false`；`-3 = VK_ERROR_INITIALIZATION_FAILED` 属 EL0 metal2vulkan，还隔 root→launchd→GPU 插件三层，**仍未复现**。产物 `build/db-{apfs-retry,iogpu,mg,mgc,mgc-fact}`，日志 `logs/qemu-mgc*-2026-11-13.log`。**追加旁证**：`--load-apfs --break-symbol _vfs_fsadd` 也到（`pc=0xfffffff00750ec14`，85 条打印），但 `_native_apfs_registered`/`vfsops` 仍是 0 —— 源码 `bsd/kern/bsd_init.c:497-510` 显示这两个全局在 load 后立刻遍历 `vfsconf` 当场定型 ~~⇒ **APFS load ≠ start/register**，这格比补 `rd=` 更靠前，是下一步第一优先~~ ⚠️ **该结论已被 §69.13 推翻**：`registered=0` 只是 probe 太早的时序快照，APFS 的 `vfs_fsadd` 真返回 0、`*handle` 的 `vfc_name='apfs'`；`rd=` 也确实生效但不够，真墙是 `mdevlookup(0)` 找不到 md0
- **§69.13**：⭐⭐ **APFS 其实注册成功，root 墙精确定位到 `md0 未配置`**（零改码、4 格 TCG 各约 14 秒）。静态先发现 `db-vfsfsadd` 的 `frame_chain[0].lr=0xfffffff00566a914` 落在 `com.apple.filesystems.apfs:__TEXT_EXEC`（`0xfffffff005640000+0x1294f0`）⇒ 第一次 `_vfs_fsadd` 就是 APFS 自己调的，不是别的 fs。实验 A（`build/db-vfsfsadd-finish`，`--finish-function --read-entry-region 0:0:40 --read-entry-indirect 1:0:20`）：`vfe_fsname=b'apfs'`、`vfe_vopcnt=6`、`vfe_flags=0x16479`（THREADSAFE 置位）、函数返回 **`x0=0`**、`function_return_stack_matched=True`、`*handle=0xfffffff007b2d848` 的 `vfc_name='apfs'`（`vfstable_add()` 已链入 `vfsconf`）⇒ §69.12.7 作废。实验 B（`--trace-symbol _native_apfs_probe_checkpoint`）坐实时序：apfs `kmod_start` 9.111 → **checkpoint 9.112** → 最后一条打印 9.222 → `vfs_fsadd` 返回；源码 `bsd_init.c:797-799` 是 `vfsinit(); → native_apfs_load_probe();`，两个全局只赋值一次 ⇒ `registered=0` 是**拍照太早**，不是没注册。实验 C/D（`db-root-base` vs `db-root-rdmd0`，唯一变量 `rd=md0`，config 已核对落盘）：panic 串从 `Failed to mount root device` 变成 **`IOFindBSDRoot: specified root memory device, %s, has not been configured`**，两格同为 87 条打印、同 `_panic` PC；源码 `IOKitBSDInit.cpp:846-884` 是 `mdevlookup(0)` 返回 <0 ⇒ 真墙是 **root 内存盘从未配置**，`rd=` 必要但不充分。组件缺口复核：`build/native-user-fixture/` 不存在、`kernel.macho` 无 `_native_user_checkpoint`、全树无 `interface-audit.json`；真实两卷 dmg 在 `UniXNU/out/ipsw/21H461/044-760{34,40}-038.dmg`。下一轮第一优先回到 root 媒体（md0 或 virtio/UDIF IOMedia），判据是 panic 串变成 `BSD root: … major … minor …`；报告仍两个 false、`-3` 未复现。产物 `build/db-{vfsfsadd-finish,apfs-order,root-base,root-rdmd0}`，日志 `logs/qemu-{vfsfsadd-finish,apfs-order,root-base,root-rdmd0}-2026-11-13.log`
- **§69.14**：⭐⭐⭐ **跑一次现成生成器，启动链连过三层**（零改码、零刷机、TCG 每格约 14 秒）。**先更正两处误判**：① `_native_user_checkpoint` 根本不是内核符号，探针 `:559` 从 own fixture 的 `addresses.json['_checkpoint']`（用户态 `0x10000400c`）解析断点，`kernel.macho` 里没有它是设计如此；② §69.12.2/§69.13.4 说的「组件缺失」其实只是**没跑树里现成的 `scripts/build-native-user-fixture.py`**（`xcrun clang/ld/vtool/codesign` 纯本地，产出 `build/native-user-fixture/{launchd.macho,addresses.json,trustcache.bundle}`）。`--native-user` 一次给齐 md0 三要素：DT `/chosen/memory-map/RAMDisk` 16 字节占位（`make-dt.py:149`，DarwinBoot `:328-330` 识别）、`initrd = "Payloads/launchd.macho"`（`:433`）、`rd=md0`（同处）；只有占位没有 initrd 会被 `DarwinBoot.c:759-762` 判 `EFI_UNSUPPORTED`，只有 `rd=` 也会撞 §69.13.4 的 `mdevlookup(0)` panic —— 所以三要素缺一不可。内核 `IOKitBSDInit.cpp:822-840` 见 `RAMDisk` 就 `mdevadd()` ⇒ md0。**三层实测**（`nm` 已核对符号地址）：`db-native-user-root` 命中 `_vfs_mountroot`（`0xfffffff0074e7c2c`）⇒ `setconf()→IOFindBSDRoot()`（`bsd_init.c:1017-1025, :1344`）**没 panic、成功返回 rootdev**；`db-native-user-mount` 加 `--finish-function` ⇒ 返回到 `0xfffffff007728900`、**`x0=0`**、`function_return_stack_matched=True` ⇒ **`vfs_mountroot()` 挂载成功**；`db-native-user-init` 命中 `_load_init_program`（`0xfffffff007753288`）。**新墙（更靠后）**：默认断点（`_native_user_checkpoint`）两格都停在 `_panic`，`argument_memory.x0='offline owner SystemOS mount failed: %d'`，`owner_cache_mount={stage:0,status:80,mount_status:-1,authorized:0,root_recheck:0,selected/mounted_device:-1}`；源码 `kern_exec.c:8244-8248` → `native_owner_cache_prepare()` 开头 `:7139-7144` 要求 `native_verified_block_root_authorized(root_device)`，**md0 不是已验证块设备** ⇒ `EAUTH=80`，`--trust-cache` 加与不加同一 panic（89 条打印）。官方 `check-owner-systemos-mount-checkpoints.py` 的负对照期望 `stage=1/root_recheck=1`，本轮 `stage=0` 比它还早一步，别拿那个断言判本板；正例要求 `native_virtio_verified_devices==2` + `/dev/disk2s1`。下一轮真实输入：内核 `kern_exec.c:7101-7103` 硬编码卷 UUID **`8846FE78-2910-4297-AF97-D59BB6F8FC16`**（与设备 ADT `apfs-preboot-uuid` 逐位一致，不用造值）、21H461 双卷 dmg、`--root-match-volume-uuid`/`--virtio-udif-image`/`--virtio-mmio-fdt`；~~**先复核本板是否有 `_native_virtio_ready`**（§69.9.1 说没有）~~ ⚠️ **§69.15.2 已推翻**：本板链入的是 ShengUfs bring-up 授权函数，不需要 virtio ABI。边界：报告仍两个 false、`native_user_el0_observed=false`、`native_user_memory` 全 `E14`（EL0 未映射）⇒ **EL0 没起来、`-3` 未复现**；`sheng-pmgr=2` 仪器前提不变。产物 `build/native-user-fixture/`、`build/db-native-user-{root,mount,init,el0,el0-plain}`，日志 `logs/qemu-native-user-{root,mount,init,el0,el0-plain}-2026-11-13.log`
- **§69.15**：⭐⭐ **owner 授权墙被一个现成 boot-arg 打穿，下一格精确为「真实 SystemOS 卷 UUID」**（零改码、6 格 TCG）。**先更正 §69.14.5**：`nm -U kernel.macho | grep -ci virtio` 确实是 0、连 `VirtioMmio*`/`AppleVirtIOBlock` 类名都没有，但**本板根本不走 verified VirtIO 版** —— 链入的是 `UniXNU-muyu-17/drivers/ShengUfs/NativeUfs.cpp:1745-1749` 的 `!NATIVE_UFS_SOURCE_API` bring-up 分支 `return gAllowUnverifiedRoot ? 1 : 0;`，全局由 `NativeUfs::start():1969-1970` 的 `PE_parse_boot_argn("native-ufs-allow-unverified", …)` 赋值，且在 `:1973` 的 `return false` **早退之前**。`package/SourceRootBinding.cpp:96` 是 source-storage 版、`UniXNU-27/.../NativeVirtioBlock.cpp:386` 是 27 线，都不是本板。严格单变量 `db-native-user-el0-plain` → `db-owner-unverified`：`owner_cache_mount` 的 **`stage` 0→1、`root_recheck` 0→1**，`status=80`、`selected/mounted=-1`，与官方 `check-owner-systemos-mount-checkpoints.py` 的 `missing` 负对照**逐位对齐**（§69.14.4「不是同一条路径」的判断作废）。⚠️ **但这是编译进去的 bring-up escape hatch**，绕过 owner policy 与块签名验证，`native_virtio_verified_devices==2` 永远达不到 ⇒ 只能用来推链，不能宣称 verified root 通过。墙的位置由源码 + trace 双重坐实：`kern_exec.c:7140-7148` 在 `stage=1` 后调 `native_verified_block_volume_device("8846FE78-…")`，`done:` `:7201-7204` 的 `root_recheck=1` 反证 `root_authorized=1`；`--trace-symbol` 在 9.31 s 命中 `pc=0xfffffff0079ed1f8`/`lr=0xfffffff00775341c`。`--log-registry-strings` 显示 15 条注册表事件里只有 `BSD Name=disk0`、**没有任何 `UUID`**；`native-ufs-root=1` 不是变量。真实资产盘点（不造值）：`launchd.macho` 是裸 Mach-O（md0 里没有 APFS 容器/分区表，所以发不出卷 UUID）；`fw/data-seed.img` 是 200 GiB `DBRDISK1`、`NXSB`、卷 UUID `625F64C3-704B-4BA1-9E82-AFBFD83AE86C`（不是目标），当 `--native-user-ramdisk` 用会被 `DARWINBOOT: … ramdisk 214748364800` → `boot refused: Bad Buffer Size` 拒（窗口仅 256 MiB）；21H461 两卷 dmg 明文 UUID 里没有目标值。脚本已写死 RAM root 与真实磁盘 root 互斥（`probe:265` / `make-dt.py:93,95` / `probe:175`）。下一格只能走真实块设备路径（链入 virtio 存储组件 + `--root-match-volume-uuid`），判据 `stage ≥ 2`；卷 UUID 必须来自真实卷，**不能用 md0 或改 DT 冒充**。边界仍是两个 false、`native_user_el0_observed=false`、**`-3` 未复现**；`sheng-pmgr=2` 与 `native-ufs-allow-unverified=1` 两个 opt-in 仪器前提不变。产物 `build/db-owner-{unverified,registry,ufsroot,apfsram,voldev-trace}`，日志 `logs/qemu-owner-*-2026-11-13.log`
- **§69.16**：⭐ **真实 SystemOS 卷 UUID 取证完成，但 stage 2 的四条路径全部实测堵死**（零改码，**没把 stage 推过 1**）。① **UUID 是真实值**：`044-76034-038.dmg` offset 18608368（block 4543 / 块内 240）有二进制 `8846fe7829104297af97d59bb6f8fc16`，同块含 `APSB` magic 与 `newfs_apfs (2142.41.2.100.2)`/`apfs_kext` 版本串 ⇒ `apfs_vol_uuid`；之前搜明文找不到是因为二进制存储。三方一致：内核 `kern_exec.c:7101-7103`、ADT `apfs-preboot-uuid`、真实固件镜像。② **入参/返回码实测**（`db-owner-voldev-return`，`--break-symbol _native_verified_block_volume_device --finish-function --read-entry-region 0:0:40`）：`entry_arguments[0]=0xfffffff00712a919` 读出 `8846FE78-…`、**返回 `x0=0`**、`function_return_target=0xfffffff00775341c`、`stack_matched=True`；断点在函数内时 `status=-1/root_recheck=0` ⇒ 全局只在 `done:` 赋值，解释 §69.15 的 80/1 都是时序快照。③ **四格硬阻塞**：A 5 个 xnu-prelink candidate 与当前 `kernel.macho` sha 全同（`e627d477…`）、virtio 符号全 0；B QEMU `dumpdtb` 的 FDT 合规（32 个 `virtio,mmio` 节点、`0xa000000+0x200`+`dma-coherent`，`make-dt` 生成 17668 B 成功），但探针 `Cannot resolve _native_virtio_read_checkpoint`（`probe:289-290` 改断点 + `:1020-1022`/`:1527-1530` 无条件依赖 6 个 `_native_virtio_*` 全局；内核唯一 VirtIO 类是 `AppleVirtIONeuralEngineDevice`）；C `packed_ramdisk.pack(3.3GB dmg)` 报 `Nonzero input exceeds packed format bound`（`DBRDISK1` 最多 65536 非零页 = 256 MiB）；D 真实 dmg 作 `--native-user-ramdisk` + 8 GiB RAM + 4 GiB owned window 时 **QEMU vvfat `File …/root.img is larger than 2GB`** 直接拒（3.2 GB ESP 副本已清理）。④ 资产：两个 dmg 都是未加密 **DawnUpdate21H461 单卷**（UDRW 裸 APFS 3.08 GiB / 512 对齐；ULFO+GPT 6.1 GB / `%512=373` 不能作 raw），取证后 `hdiutil info` 残留 0；`fw/data-seed.img` 是 200 GiB Data 卷 UUID `625F64C3-…`，无目标。⑤ 顺带查清 `NativeRamBlock` 是内核现成块驱动（`PackedRAMDisk` 稀疏加载，`DarwinBoot.c:329-332/:824/:853`），但 QEMU `make-dt.py:148-149` 只发 `RAMDisk` 占位，且 `NativeRamBlock.cpp:200` 明确 `both RAMDisk and PackedRAMDisk` 会 panic ⇒ `--node-property` 不能简单叠加。**下一格**只能链入 `NativeVirtioBlock` 组件（改构建）或改离线仪器发 `PackedRAMDisk` 开关 + 合规 seed；从大容器截取元数据子集属拼接镜像，不能冒充完整真实卷。判据仍 `stage ≥ 2`；禁止 md0/改 DT/伪造 UUID 冒充。报告仍两个 false、**`-3` 未复现**。产物 `build/db-{owner-voldev-return,real-disk,real-systemos}`，日志 `logs/qemu-{owner-voldev-return,real-disk,real-systemos}-2026-11-13.log`

- **§69.17**：⭐⭐⭐ **用户放宽项目内工作约定后，定点补丁打穿 owner panic，EL0 真正起来**（仍零刷机、TCG 3 格）。本轮**实际改了**：新增 `scripts/patch-skip-owner-panic.py` + `kernel.skip-owner.{macho,json}`（**新文件**，原 `kernel.macho` sha `e627d477…` 前后不变）；系统/设备级红线（离线 QEMU、不碰真机、不伪造 UUID、不把 escape hatch 说成 verified root）不变。为什么补丁而非重建：`build-xnu-4k-baseline.sh` 是全量 XNU 构建会打乱全部符号/extents，而 `probe:256` 只校验 `sha256(kernel)==candidate_sha256`、符号直接对传入 `--kernel` 跑 `nm` ⇒ 只要字节布局不变，`kernel.json` 的 119 个 extent 全部复用，只需同步一个新 sha。**补丁点**：`load_init_program()` 把 owner/data prepare 全内联，两条 `CBNZ` — `0xfffffff00775352c`(`350034f6`→冷块 `0x753bc8`，`mov w8,#8248` 即 `kern_exec.c:8248` 的 `offline owner SystemOS mount failed`) 和 `0xfffffff007753738`(`35002595`→`0x753be8`，`#8254` 的 `offline scratch Data mount failed`) — 改成 `NOP 0xd503201f`；不能 NOP `bl _panic`（冷块连续排列会落入下一个 panic 块）。脚本四重校验含「`Path.with_suffix()` 会覆盖原件」这个已修掉的坑。**判读格实测**：`db-el0-skip-owner`（无 trust cache）停在 `_panic`，格式串 `unexpected SIGKILL of %s %s with reason -- namespace …` ⇒ AMFI 杀掉未受信任的 launchd；`db-el0-skip-owner-tc` 加 `--trust-cache build/native-user-fixture/trustcache.bundle` ⇒ **`stop_symbol=_native_user_checkpoint`、`pc=0x1000003d4`、`native_user_el0_observed=true`**，`native_user_memory` 从三项 `E14` 变成真实读值 `_user_page_size=0x1000` / `_old_length=0x4000000` / `_results=0100…` ⇒ **trust cache 是 EL0 必要条件**。`db-el0-gpu-chain` 再叠 `--load-iogpu --load-mobile-graphics --mobile-graphics-client-probe` ⇒ 同一格里 EL0 仍命中且 `native_iogpu_load_status=0`、`native_mobile_graphics_load_status=0`、客户端 `selector_results=[0xe0000001]×3` 与 §69.12 逐位一致。**作废/修正**：§69.14.4/§69.15.6/§69.16.7 的「必须链入 virtio / 必须改构建才能往前推」只针对 `stage≥2`（让 owner policy 真通过），本节走的是「不让 owner 通过、只是不再 panic」另一条路，判据不同；§69.16.8 的 `native_user_el0_observed=false` 作废。**`-3` 仍未复现**：`BravearkLastLog.c:730-739` 说它是 Turnip `dw_probe_selector` 探测 0..255 全被 sandbox 拒（`pr=0`）→ `VK_ERROR_INITIALIZATION_FAILED`；缺 dyld cache / vkd3 / sandbox / 真实 GPU 服务发布。`--offline-gpu-broker` 要 `offline-air-init=1` → 构建期 `OWNER_ROOT_TRUST=1`（本内核没编）。下一格：扩展 own fixture 发真实 `io_connect_method` 探测 selector（需重算 trustcache + 发布可连接 user client），或重建 trust 内核走 broker，或引入真实 dyld/Turnip（要解 vvfat 2 GiB 单文件上限）。报告仍两个 false、`owner_cache_mount` 如实 `stage=1/status=80/authorized=0`；回滚只需删两个新文件。产物 `build/db-el0-{skip-owner,skip-owner-tc,gpu-chain}`，日志 `logs/qemu-el0-*-2026-11-13.log`
- **§69.18**：⭐⭐⭐ **`-3` 用真实 Turnip dylib 复现出来了，同时纠正前面 17 节的判据**（零刷机、零格 TCG、零改内核/dylib）。§67 harness 加 opt-in `TU_SELECTOR_POLICY=deny-all`（256 个 selector 全用真常量 `kIOReturnNotPermitted 0xe00002e2` 拒绝）⇒ 真实 driver 打印 `tu_knl_darwin.cc:204: no external-method selector is admitted for this process` + `could not open the kernel client (-3)`，**`vkEnumeratePhysicalDevices -> -3`**。⭐ **但 `vkCreateInstance` 恒返回 0**、`vkCreateDevice` 不可达 ⇒ §69.17.4 / §69.17.6 / `BravearkLastLog.c:730-739` 写的「`vkCreateInstance/Device` 返回 -3」要改成「`vkEnumeratePhysicalDevices` 返回 -3」。6 格矩阵还证明：换 `kIOReturnUnsupported` 仍 -3（只认 `!= KERN_SUCCESS`）、放行任意一个 selector 即恢复（`admit-17` 的 `probes=36` 说明每次 open 都扫一遍）、`NativeAdrenoGpuPublished` direct 形式 `probes=0` 完全绕过 selector 准入；`IOServiceOpen` 被拒是另一条带 `:198` 原文的 -3（加上 §67.10 的 `:216` ABI 出口，共三条，可用 driver 原文判别板上是哪条）。⭐ **`0xe0000001` 是 `kIOReturnInvalid`（EL1 基类越界），不是 sandbox 全拒 `0xe00002e2`** ⇒ §69.12/§69.17「同格 EL0 + selector_results 接近 -3」不成立；`check-turnip-minus3-judgement.py` 对 **40 个 QEMU 格**逐格判 `minus3_reproduced_in_qemu=false`。QEMU 侧精确缺四件：发布可连接 GPU 服务（`registerService`）、EL0 真 Mach MIG `IOServiceOpen/io_connect_method`、sandbox `io_filter_policy_t` 全拒、真 Turnip EL0 客户端（dylib 的 9 个系统依赖不能静态链、vvfat 2 GiB 未解）。边界：host 上 -3 真、IOKit 回包合成、板上 sandbox 未回放、QEMU 未复现、两个 `false` 不变。新增 `run-selector-matrix.py` / `check-turnip-minus3-judgement.py`，改 `fakeiokit.c`；产物 `build/host-turnip-selector/{matrix/matrix.json,judgement.json}`。⚠️ **§69.19 已更正：这里复现的是「tunnel 形式 selector 全拒」的另一条 -3，不是板上 -3**；「抓 driver 原文」路线板上无采集通道、不可行。
- **§69.19**：⚠️ **整条归因已由 §69.25 在内核源码层面证伪**（`IOUserClient.cpp:4566-4567` 把 `kIOReturnUnsupported` 显式转成 success），保留原文仅作历史留证。⭐⭐⭐ **`-3` 归因翻转，找到静态根因候选**（零刷机、零格 TCG、零改内核/dylib）。① 板上 `MUYU L pr=2 tn=105 sb=00000001` + §64.6 + §67.6 ⇒ §69.18 的 host `-3` **不是板上那条**；§69.18.3 第 1 条末句、§69.18.6 第 1/2 条已就地划掉。② **27 格输入敏感性**：`vkCreateDevice -> -3` 是**空集**（设备创建失败只会是 -1/-2/-5/-9 且带 driver 原文）⇒ §67.10「优先怀疑 tu_CreateDevice 内部」被否；**6 格实例扩展**：真实 ICD 不注册 `KHR_display`/`portability`/`layer_settings`（都给 -7、零 IOKit 调用），实例层不可能给 -3；runtime `create.rs` 8 个 `.map_err(vulkan)` 包装点只剩 `:735`。③ ⭐ **根因**：内核 `zzw-native-adreno-sandbox-grant.patch` 的 `native_sandbox_grant(service)` 读 **`service` 上的 `NativeSandboxGrantProcess`**，而 `UniXNU-muyu-17/package/GpuPowerProbe.cpp:1181-1182` **从没设过这个属性**；驱动只在 **client** 上设 `NativeSandboxGrant`（`NativeAdrenoPublish.cpp:1095`），旧 patch 不读它；内核 strings 有 `NativeSandboxGrantProcess`、**无** `NativeSandboxGrantService` ⇒ 未编入 uke 修正补丁 ⇒ 旁路恒 false，MACF/`io_filter_resolver` 三道门全生效 ⇒ `A re00002c7/re00002e2` 的 `IOServiceOpen` 被拒 ⇒ Turnip `:198` ⇒ -3 ⇒ `VkResultCode(-3)`。`V gr=5` 只是驱动自己的计数，**不是内核已放行**（`:4512` 读法作废）。④ **§64.6 的排除有逻辑漏洞**：`B n=0`/`S`/`ex` 都是跨进程全局计数（§67.6），被拒在 user-client 创建之前的 open 一个计数器都不进 ⇒ `:198` 从未被排除。`UniXNU-sheng/package/GpuPowerProbe.cpp:1242` 有正对照，`UniXNU-uke/docs/GPU-PORT.md:566-575` 有「an audit found the mismatch」原文与修正补丁。⑤ **最小单变量修复**：muyu-17 `GpuPowerProbe.cpp` 在 `registerService()` 前补 `setProperty("NativeSandboxGrantProcess", "backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard")`，只动内核侧一个输入，重建 kernel+payload+vendor_boot 回板看 `R` 行。边界：**静态审计 + host 真 dylib 实测，未在板上验证**；两个 `false` / `owner_cache_mount stage=1/status=80` 不变。新增 `run-input-sensitivity.py` / `run-instance-extensions.py` / `UniXNU/scripts/check-grant-mismatch.py`，产物 `build/host-turnip-input/{sensitivity.json,instance-exts/,grant-mismatch.json}`。
- **§69.20**：⭐⭐⭐ **按 §69.19.5 落单变量修复并离线重建完成，镜像已就绪、停在「待上机」**（用户指令「重建，要上机时说」；零刷机、零格 TCG、1m04s）。① 改 `UniXNU-muyu-17/package/GpuPowerProbe.cpp:1182`，在 `registerService()` 前补 `setProperty("NativeSandboxGrantProcess", "backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard")`，与 sheng `:1242` 逐字相同；diff 严格 `+1/-0`。**先钉死编译输入**：`out/muyu/tree/kits/muyu` 是 symlink → `UniXNU-muyu-17/package`，`build-native-legacy-display.py:333` 读的就是这一份（改错副本会白跑）。② 三步链 `build kernel` → `build payload` → `patch-vendor-boot.py`（**boot-arg 一个没加/删**，cmdline 996 B，1000 B 硬闸只剩 4 B；没跑 firmware 步），产出 **`fw/vendor_boot_gpu_grant1.img` 77,393,920 B / sha `6520c152…7364`**，新内核 sha **`c79e8f2f…1237`**（旧 `e627d477…`），日志 `logs/build-{kernel,payload,vendor-boot}-grant1.log`。③ `UniXNU/scripts/check-grant-rebuild.py` 全绿：其它 **14 个输入 sha 逐项不变**、源码 `+1/-0`、`kernel.json.candidate_sha256` 对齐、新内核 strings 含名单且旧内核不含、**无** uke 的 `NativeSandboxGrantService`、payload/vendor_boot 都含名单、三个日志无 error、三个 GPU 参数齐全。④ `check-grant-mismatch.py` 改为**阶段感知**（pre-fix / post-fix 两套断言），冻结 pre-fix 内核作旧 patch 证据，pre-fix 判据归档 `baseline-pre-grant1/grant-mismatch.pre-fix.json`。⑤ **QEMU 基线没坏**：`kernel.skip-owner.*` 未覆盖（sha `c4c1420a…`）、关键符号地址零漂移、两个 `CBNZ` 补丁点字节新旧同为 `0x350034f6/0x35002595` ⇒ §69.17 定点补丁对新内核仍适用；`vkd7` vs `grant1` 镜像 header `0..0x1000` 与 DTB/tail `>=0x4801000` 逐字节相同，差异全在 ramdisk 段。⑥ ⚠️ **下一步需要上机**：只写 `vendor_boot`、不碰 `boot`/`recovery`、设备须在 fastboot、屏照含顶部 `MUYU LIVE`；主判据 `MUYU R` 的 `VkResultCode(-3)` 是否消失，辅以 `MUYU A` 的 `re00002c7/re00002e2`、`MUYU L/V`；充电前必刷 `fw/stock/vendor_boot.img`。**本轮未执行任何刷机命令**。边界：`board_verified=false / flashing=false / device_touched=false / qemu_cells_run=0`，两个 `false`、`owner_cache_mount stage=1/status=80` 不变。回滚：恢复 `baseline-pre-grant1/GpuPowerProbe.cpp.orig` 或删掉新镜像即可。
- **§69.21**：⭐⭐ **已上机：`grant1` 刷入 + RAM 引导成功，屏照待拍**（用户「设备在线」授权；12:47）。只读预检：`fastboot devices=a20248f7`、adb 空、`current-slot=a` 但 **`slot-unbootable:a=yes`**（历史事故态）→ 先 `preflight` 用 `--set-active=a` 修成 `no`；电池 3737 mV。`MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_grant1.img`：**只写 `vendor_boot`**（73.81 MiB / ramdisk 72 MiB / sha `6520c152…7364`），`OKAY 0.034s`；`fastboot boot` 17 线 UEFI `OKAY 0.188s`、**无 `Load Error`**；账本三条 `rc=0`，日志 `logs/flash-grant1.log`；引导后 fastboot/adb 皆空（面板是唯一输出通道）。⚠️ **刷写/引导成功 ≠ 验证**：`board_verified=false / screen_photo_captured=false`，必须等 ticker 3–4 遍后拍**完整屏照（含顶部 `MUYU LIVE` 行）**，主判据 `MUYU R` 的 `VkResultCode(-3)` 是否消失，辅以 `MUYU A` 的 `re00002c7/re00002e2` 与 `MUYU L/V/M`；若仍 -3 则 §69.19 根因在板上被证伪。机器记录 `build/host-turnip-input/grant1-flash.json`。状态变化仅 `flashing=true / device_touched=true`；两个 `false`、`owner_cache_mount stage=1/status=80` 不变。电池偏低，**充电前必刷 `fw/stock/vendor_boot.img`**；`fastboot reboot` 禁用。
- **§69.22**：⚠️ **`userdata` 首块 8 KiB 第二次被写坏；昨天的解决办法已找到并脚本化，本轮零设备动作**（用户「根镜像被写坏了，昨天也有经历」；12:50）。**办法 = 定点写回 8192 B，不是 9 GB 整盘重刷**：载荷是 `fw/userdata-backup/userdata-root.sparse` 的 chunk0 首 8 KiB（`SHNGROOT` manifest + 4 KiB 零），在 stock recovery 里 `dd if=/tmp/ud_fix.bin of=/dev/block/sda32 bs=4096 count=2 conv=notrunc; sync` + 回读校验；载荷 `/tmp/ud_fix.bin` 与 `ud_verify.bin` 本机仍在、sha `1afe7d01…c78143d` 与备份逐字节一致。昨天实测仅块 0/1 坏、块 2–15 完好。成功判据 `BSD s=8 / INIT q=6 / ROOT c=6/0/0`。⭐ **成因收敛**：账本 `2026-11-12T14:48:34–40Z` 的 `restore-stock --yes`（刷回原厂 boot/vendor_boot/recovery）之后长时间墙充，坐实 §56.6 候选 ①「stock recovery 挂/格式化 `/data`」；`cmd_restore_stock()` 自己写着「boot recovery and wipe there」。⇒ **修好后充电前只用 `stock` 模式，别再跑 `restore-stock`**。新增 `UniXNU-muyu-17/scripts/repair-userdata-manifest.py`（`prepare`/`diagnose`/`verify`/`commands` 全只读，`fix` 需 `--apply --yes` 且检查 `/data` 未挂载），用昨天损坏留证回归全绿；关闭 §9 第 6 条「修复脚本化前不要依赖记忆」TODO。⚠️ 修复前**必须先只读 dump 首 64 KiB 并 `diagnose`**，只有 `verdict=manifest-only (blocks 0/1)` 才准写；修好重刷 `grant1` 才能继续 §69.21 屏照判读。边界：本轮 `flashing=false / userdata_written=false / device_state_changed=false / board_manifest_read=false`。
- **§69.23**：⭐ **第二次首块损坏已修复并重新引导，`grant1` 又在板上跑了，只差屏照**（用户「直接修，不管是不是同因」；13:00–13:20）。⭐ **关键差异**：昨天是 **TWRP 天然 root**，本次 `stock` 模式 RAM 引导进的是**官方 rec**（`uid=2000(shell)` / `ro.secure=1` / `adb root` 被 production build 拒 / 无 su / SELinux Permissive），`/dev/block/sda32` 是 `root:root 0600` ⇒ `adb shell dd` / `exec-out dd` 全 `Permission denied`，**§57.1 的 adb dd 路径在官方 rec 下不可复现**（既不能 dump 也不能写）。⇒ 改走 **fastboot 通道、绕开 recovery root**：① 构造只含前 2 块的定点 sparse（`ud_fix_8k.sparse`，12288 B / sha `4cd0a473…`），借 `root-image` 的 manifest 校验 `OKAY`（ledger `05:14:02Z rc=0`），但 `fastboot fetch` 该设备不支持 ⇒ **无法回读**、0.001 s 不可证；② 按「不管是不是同因」整盘刷回 `fw/userdata-backup/userdata-root.sparse`（`OKAY` / `202.047s` / rc=0，窗口与 sha 全对、尾部 Data 窗口未动），覆盖所有可疑区域；③ `MUYU_UEFI=fw/boot-17line-043f7120.img … xnu fw/vendor_boot_gpu_grant1.img` → flash + RAM boot 都 `OKAY`，ledger `05:20:01Z` 三条 rc=0，引导后 fastboot/adb 皆空 ⇒ XNU 在跑、面板唯一输出。`boot`/`recovery` 未动、未用 `fastboot reboot`。**待判读**：完整屏照（含顶部 `MUYU LIVE`）看 `BSD s=8 / INIT q=6 / ROOT c=6/0/0`（根镜像恢复）与 `MUYU R` 的 `VkResultCode(-3)` 是否消失（§69.21 主判据）。边界 `flashing=true / userdata_written=true / device_state_changed=true / board_manifest_read=false / board_verified=false / screen_photo_captured=false`。⚠️ 充电前刷回 `stock` vendor_boot，但**别让设备在 stock 状态长时间墙充**、别再 `restore-stock --yes`。教训：`§57.1` 只在 TWRP 成立；官方 rec 下板上取证需 root，`fastboot fetch` 不可用 ⇒ 要么备好 TWRP，要么 fastboot sparse 定点写 + 整盘兜底。
- **§69.24**：⭐⭐ **`grant1` 首张板上读数：`-3` 仍在 ⇒ §69.19 方案 1（补 `NativeSandboxGrantProcess`）在板上被证伪**（13:24，纯判读、零设备动作）。⚠️ **`MUYU GPU calls/refused/o` 不是 sandbox 拒绝计数**：`LegacyDisplayProbe.cpp:87-98` 证明它是 GOP 帧 hand-off 计数（`calls`=offer 调用、`refused`=presenter 未接受、`o`=GPU offers），与 `IOServiceOpen`/MACF/Vulkan 无关；§6.3.1 与状态表「每条 offer 都被内核拒绝」是过时误读，以 §9 第 2 条为准。`calls=1 refused=0 o=1` 只说明这一帧 hand-off 被接受、相对 §57 的 `7/7/0` 显示链有改善，**不能判 `-3`**。⭐ **方案 1 为什么可能不够**：`IOUserClient.cpp:4339-4360` 的 `native_sandbox_grant()` 只读 service 的 `NativeSandboxGrantProcess` + `proc_selfname()`，三道门（`:4390`/`:4550`/`:4559`）都靠它短路；而 uke `GPU-PORT.md:566-575` 明确「driver sets `NativeSandboxGrant` on the **client object**; an audit found the mismatch」，其修正补丁 `zzzz…uke-gpu-sandbox-client-grant.patch` 另加 client `NativeSandboxGrant` 与 service `NativeSandboxGrantService`（只放行 `0x4147`/`0x4148`）。grant1 **没编入该补丁、没发布 `NativeSandboxGrantService`、名单未必覆盖真实 opener**。⚠️ **下一步先分叉、别直接改内核**：补 `MUYU A` 行 —— 若仍有 `re00002c7/re00002e2` ⇒ 内核仍拒，执行 §69.19.5 **方案 2**（移植 uke 补丁 + 发布 `NativeSandboxGrantService` + 核对 opener 进程名）；若 `A` 已干净 ⇒ 内核已放行、`-3` 在用户态设备创建链，**不要改内核**，回到 §9 第 3 条读 `Q`/`Y` 调用序列。请补完整屏照或至少 `A`/`V`/`R`/`B`/`O` 五行（`O` 行的 opener 名决定名单是否命中）。边界：新增 `board_observation_received=true / grant1_root_cause_falsified=true`，`board_verified=false` 不变。
- **§69.25**：⭐⭐⭐ **`grant1` 完整屏照判读：根镜像恢复已板上确认；`-3` 的真正失败点钉在 `createVm`，§69.19 的 sandbox 链源码级证伪**（13:28，纯判读、零设备动作、零重建）。① ✅ `ROOT c=6/0/0 d=4/0` / `INIT q=6` / `BSD s=8` / `TOC n=127` 与 §69.21 主判据逐项一致 ⇒ §69.23 整盘刷回有效，`userdata_restored_board_verified=true`，§69.22 事故闭环。② ⚠️ **更正 §69.24.4 判据表**：`A` 行 `re00002c7` = `kIOReturnUnsupported`，`IOUserClient.cpp:4566-4567` **显式把它转成 `kIOReturnSuccess`**；`A` 的 `m`/`r` 只是 `NativeAdrenoPublish.cpp:1056-1072` 在 `newUserClient` 里手动模拟两道 gate 后写进日志的值，不是 open 返回；能进 `newUserClient` 已说明 `:4390` 放行，本屏 `m00000000` 说明 `:4550` 也放行 ⇒ **方案 2（移植 uke 内核补丁）暂缓、别改内核**。`gr=7` 是驱动侧 `grants++`，不是内核放行/拒绝证据。`grant1` 名单漏了真实 opener `SpringBoard`（`A` 行 `proc_selfname()`），但这只影响 `:4390` 短路，不改变结果。③ ⭐ **新硬证据**：`MUYU B i48 o40 f1 s=e00002c2 b0 n5` ⇒ `mount_fail[0]=1`（无地址空间）、`[1]=0xe00002c2`（`kIOReturnBadArgument`，`IOReturn.h:147`）、`[3]=5` 次失败；`MUYU G backboardd/o2/r2/…/xe00002d5`、`mediaServerd/o0/r1/…/xe00002d5 n3` ⇒ `last_refusal=kIOReturnBusy`，正是 `NativeAdrenoPublish.cpp:1107` `mount()` 失败后的返回码。链条：`createVm()`（`NativeAdrenoGpuContext.cpp:157-205`）返回 `kIOReturnBadArgument` → `mount()` 失败 → `IOServiceOpen` 返回 Busy → Turnip `dw_open_connection` → `vkEnumeratePhysicalDevices` → `-3`。**§57.6 判读格第 2 行命中**。对照 §59 的 `B f0 s0 n0` 同样有 `-3` ⇒ `createVm` 失败是本次新出现的变量，不能定为唯一根因，只是当前最贴合的可观测失败点。④ **下一版单变量（先离线）**：把 `native_adreno_table_switches[6]/[7]` 打上屏（`BravearkLastLog.c:917-919` 已写明 `[7]` 的 stage 编码 `2 prepare / 3 attach / 4+i map / 0x30|op / 0 done`），纯观测、不改行为，一次重建把 `0xe00002c2` 定位到 `createVm` 的确定一步；同步核对 `i48 o40` 窗口参数（`GpuPowerProbe.cpp:1124-1125`、`1<<48` aperture）。**不要**再改 sandbox 属性 / 移植 uke 补丁 / 重跑 selector 矩阵。边界：`board_verified=false` 保持，新增 `userdata_restored_board_verified=true / sandbox_grant_root_cause_falsified_at_source=true / mount_fail_observed=true / board_screen_captured=true`。⑤ **§69.25.6（用户质疑 `calls=1` 后的复核）**：`t=561`（9 分 21 秒）却 `calls=1`，且 `tc=0 ts=ffffffff` ⇒ **不是采样偏差**，是首次 offer 成功后显示路径真的切走 GPU（§69.24.1「ticker 只跑一遍」已划掉）。`o=1` 的成立条件是 `NativeGOPPresenter.cpp:134` 的 `online && gpuClient && !swap.offered`，`gpuClient` 只在 `waitSwap()`（`:150`）置 true ⇒ **本版第一次出现用户态 GPU 客户端连上 presenter 等帧**，历史 `o=0` 全部是「从没人等帧」。⚠️ 但 `c=0` 说明 `completeSwap()` 从未执行、`transferred` 从未清 ⇒ 是**成功首帧后冻结**，不是「一次成功就收工」（健康路径 `o`/`c` 应一起涨）；与 `R PUB -3` 自洽 ⇒ `-3` 收窄到「拿到帧之后、完成 swap 之前」的设备初始化。相对历史 `o=0` 屏照同时变了 `grant1` + 整盘刷回 userdata 两个输入，§69.21 那次 grant1（坏根）没拍屏照 ⇒ **不能单变量归因**，最可能是根镜像恢复让完整用户态起来；不为坐实再去写坏根镜像。净结论：显示侧 blocker 已清，剩下唯一 blocker 是 GPU 设备创建。新增判读格：下一版 `o>=1 且 c>=1` + `R` 变 `ok` 才算真通；`o=1 c=0` 不变即确认卡在设备创建。

- **§69.26**：⭐⭐ **按 §69.25.4 落纯观测单变量并离线重建完成，`stage1` 镜像已就绪、停在「待上机」**（用户「继续」；13:5x，零刷机、零格 TCG、**零改驱动/内核行为**）。⚠️ **不是修复版**：`-3` 仍在是预期，本轮价值是让下一张屏照把 `0xe00002c2` 定位到 `createVm()` 的确定一步。① 唯一输入变化 `UniXNU-muyu-17/package/BravearkLastLog.c:679`：`MUYU B` 行由 6 字段扩成 `… n%u t6=%x t7=%x ok=%u vm=%u`，即 `native_adreno_table_switches[6]`（失败 IOReturn）/ `[7]`（stage `2 prepare / 3 attach / 4+i map / 0x30|op / 0 done`）/ `[2]`（历史成功次数）/ `[4]`（live VMs）；diff **`+13/-2`、1 hunk、零新增 printf**（屏只容纳一页）、**零写驱动全局**（weak_import 只读）。`nm -a` 已确认 `_native_adreno_table_switches=fffffff007bfaf68` 在新内核可见 ⇒ 不会全打印 `ffffffff`。② ⭐ **静态归因**：`mount()` 的 `f=1` ⇒ `space==nullptr` ⇒ `createVm` 失败；`createVm` 里 `kIOReturnBadArgument` 只能来自 `prepare/attach/map`（`NativeAdrenoGpuContext.cpp:169/179/185 → :188`）；而 `NativeAdrenoSmmuMemory.cpp:135` 把 `ArmSmmu4KLivePrepare` 的**任何非 Capacity 失败**（`layout()` 的 Argument/BadRange/Budget、`LivePrepare` 的 StateMismatch）**统统折成 BadArgument** ⇒ `B s=e00002c2` 单独无法细分，必须靠 `t7`。③ 三步链参数与 §69.20.2 逐字相同（boot-arg 一个没动，cmdline 996 B），产物 **`fw/vendor_boot_gpu_stage1.img` 77,393,920 B / sha `bb7f5b8e…e715af`**、新内核 `c8ca2bc1…5836`、`payload.img` `8d266934…8233`；`fw/vendor_boot_gpu_grant1.img` **未覆盖**（`6520c152…7364`，保留 A/B 对照）。④ 新脚本 `UniXNU/scripts/check-stage-probe-rebuild.py`（+ tree 副本，**只能从 `UniXNU/scripts/` 运行**）全绿：13/14 输入 sha 不变、1 hunk +13/-2、新内核含新串/旧串消失/仍含 grant1 名单/无 uke 补丁、`kernel.json` 对齐、payload+vendor_boot 含新串且大小正确、三日志无 error、cmdline ≤1000 B；判据 JSON `build/host-turnip-input/stage-probe-rebuild.json`。⑤ **QEMU 基线未坏**：`kernel.skip-owner.macho` 仍 `c4c1420a…`，`_load_init_program` / `_native_apfs_probe_checkpoint` / `_vfs_mountroot` / `_native_adreno_table_switches` 四符号地址零漂移。⑥ ⚠️ **待上机**：只写 `vendor_boot`、不碰 `boot`/`recovery`/`userdata`、设备须在 fastboot、`MUYU_UEFI=fw/boot-17line-043f7120.img` RAM 引导；屏照含顶部 `MUYU LIVE`，逐字抄 **`B`/`R`/`GPU`/`A`**。**判读格**：`t7=2` ⇒ SMMU 表页/arena 形状（`ArmSmmu4KLive.c:11-26, :330-340`，查 `i48 o40` 的 `bytes=1<<48` / `budget=2^36`）；`t7=3|0x30|op` ⇒ `vm_.attach`；`t7=4+i` ⇒ `vm_.map`；`t7=0` ⇒ createVm 完成过、回 §9 第 3 条读 `Q`/`Y`。`ok=0` ⇒ 本次启动无一次成功。显示侧沿用 §69.25.6：`o>=1 且 c>=1` 且 `R` 变 `ok` 才算真通。边界：`board_verified=false / flashing=false / device_touched=false / qemu_cells_run=0`，两个 `false` 与 `owner_cache_mount stage=1/status=80` 不变；**设备仍在 13:20 RAM 引导的 `grant1` XNU 上，本轮没碰**。回滚：恢复 `baseline-pre-stage1/BravearkLastLog.c.orig` 或删掉新镜像。⚠️ 充电前刷 `stock` vendor_boot，但别长时间墙充、别再 `restore-stock --yes`。
- **§69.27**：⭐ **`stage1` 已上机（用户「上机」授权，14:04 北京），屏照待拍**（`GPU-HANDOFF.md` §69.27）。只读预检：设备在 fastboot（`a20248f7`）、adb 空、`slot=a / unbootable=no / retry=7 / 3810 mV`、checker 全绿、产物 `bb7f5b8e…e715af` 与判据 JSON 一致；先审 `cmd_xnu()`（`:165-183`）确认只写 `vendor_boot` + RAM `fastboot boot`。`MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_stage1.img` ⇒ flash `OKAY 0.056s`、boot `OKAY 0.189s`、无 `Load Error`、`rc=0`；账本 `06:04:42Z`（flash）/`06:04:43Z`（boot）各一条 `rc=0`。引导后 fastboot/adb 皆空 ⇒ 面板唯一输出。`boot`/`recovery`/`userdata` 未动、未用 `fastboot reboot`。⚠️ **`board_verified=false / screen_photo_captured=false` 保持**：刷写与引导成功 ≠ 板上验证，`t6/t7/ok/vm` 是否打印、`-3` 是否变化必须等屏照。判读格见 §9 第 8 条；`stage1` 零行为改动，`-3` 仍在是预期。`grant1`（`6520c152…7364`）未覆盖，保留 A/B 对照。边界 `flashing=true / device_touched=true / userdata_written=false`。电池 3810 mV 偏低，充电前刷回 `stock` vendor_boot，但别长时间墙充、别再 `restore-stock --yes`。
- **§69.28**：⭐⭐⭐ **`stage1` 屏照判读：`t7=4` 把失败点钉在 `vm_.map(buffers_[0])`，`ok=3 vm=1` 推翻「createVm 全废」**（14:08，纯判读、零设备动作、零重建；`GPU-HANDOFF.md` §69.28）。屏照 `logs/2026-11-13-14-08-35-820.jpg`（`LIVE p=13 t=227`）。① 观测生效：`MUYU B i48 o40 f1 s=e00002c2 b0 n4 t6=e00002c2 t7=4 ok=3 vm=1`，四字段非 `ffffffff` ⇒ weak_import 绑定成立、重建没白跑；`R` 仍 `-3`、`GPU o=1 c=0`、`ROOT c=6/0/0` 不变 ⇒ 零行为改动符合预期，显示链仍冻结首帧。② `t7=4` = `NativeAdrenoGpuContext.cpp:184` 的 `4+i`（i=0）⇒ **prepare/attach 都过了，第一个 buffer 的 `vm_.map` 失败**；`t6=e00002c2` 是 `:188` 写入的 `preflight()` BadArgument ⇒ §69.26 倾向的 `t7=2`（SMMU arena 形状）被板上排除。③ ⭐ **`ok=3 vm=1`**：`[2]` 只在 `createVm` 成功路径 `++`（`:201`）、`[4]=live_vms_` ⇒ 本次启动 **3 次 createVm 成功、1 个 live VM**，§69.27.4「`ok=0` 一次都没成功」预期被否（已划掉）；§69.25.3 链条收窄为「某次之后所有新 VM 都在第一个 map 失败」，配合 `V op=3 call=567` ⇒ 部分客户端/早期时序问题，GPU 未整体不可用。④ **静态最强候选（未确证）**：`buffers_[i]` 是全局共享（`:95` 分配，`:132` 默认 VM / `:185` 每进程 VM 共用），`NativeAdrenoVm::fail():90-101` 会 `mappings_[i].memory->quarantine()`，而 `NativeAdrenoMemory::quarantine()`（`NativeAdrenoMemory.h:122`）只置 `quarantined_=true` **且无复位路径** ⇒ 一次 fault 可永久污染共享 buffer，之后每个新 VM 的第一个 map 都在 `preflight :209` 返回 `0xe00002c2`（`fail()` 自身返回 `IOError`，不是 `0xe00002c2`）。`preflight` 其它 BadArgument 分支（参数/iova/output_bits/span 长度）全是常量，与 `ok=3` 矛盾故排除；② `tables->overlaps`（`:228`）/ alias（`:249`）尚未排除。⑤ **下一版待授权重建**：先落纯观测 `stage2`，`MUYU B` 追加 `q=`（quarantined）/`bk=`（backed）/`dr=`（deviceReferences）/`gf=`（global_fault）确证；坐实后再改 `fail()` 只对 `imported()` quarantine、驱动自有共享 buffer 走 `retireAfterQuiescence()`（改行为，须重核输入/checker/QEMU 符号）。**不要**再动 sandbox / uke 补丁 / selector 矩阵。边界新增 `stage_probe_effective=true / createVm_ever_succeeded=true / minus3_persists=true`，`board_verified=false` 保持；本轮 `flashing=false / device_touched=false`。
- **§69.29**：⭐⭐ **按 §69.28.6 落 `stage2` 纯观测版并离线重建完成，停在「待上机」**（用户「继续」；14:2x，零刷机、零格 TCG、**零行为改动**；`GPU-HANDOFF.md` §69.29）。⚠️ **不是修复版**，`-3` 仍在是预期。① 两个输入变化：`UniXNU/drivers/Adreno740/NativeAdrenoVm.cpp` 新增 `native_adreno_preflight_probe[8]` + 只读 helper（17 行），把 `preflight()` 的 **8 个** `return kIOReturn…;` 换成 `return nativeAdrenoProbeRefusal(N,memory,kIOReturn…);` —— 用 **return 表达式替换**保住所有 `if(cond)return …;` 的单语句体语义，helper 只写 probe 数组并返回原值、不碰 quarantine/fail/poison、不加控制流；`BravearkLastLog.c` 的 `MUYU B` 同一 printf 追加 `pf= q= bk= dr= gf= rn=`（**16 格式符对 16 参数**、零新增 printf、零写驱动全局）。⭐ 比原计划更强：`pf` **直接给出 `preflight()` 命中的 return 点**（1 参数/quarantine、2 iova、3 Busy、4 output_bits、5 span 形状、6 `tables->overlaps`、7 span 总长、8 alias），不用再靠 `q` 反推；`gf` 直接读现成的 `native_adreno_global_fault[0]`（`fail()` 计数，此前屏上从未打印）。② 新 checker `UniXNU/scripts/check-stage2-probe-rebuild.py`（+ tree 副本，**只能从 `UniXNU/scripts/` 运行**）用**逐字节还原**证明零行为改动：切掉新增诊断块 + 把 8 个 return 表达式还原 → 与 `baseline-pre-stage2/NativeAdrenoVm.cpp.orig` 完全相同（不靠统计 diff 行，difflib 会把相邻 `}` 算成删除）。③ 产物 **`fw/vendor_boot_gpu_stage2.img` 77,393,920 B / sha `24961f7e…4470`**、内核 `9a0cda2f…d125e76`、`payload.img` `66514c18…5eb3`、cmdline **996 B 与 stage1 逐字相同**；新符号 `_native_adreno_preflight_probe=fffffff007bfc3fc` / `_native_adreno_global_fault=fffffff007bfc3ec` 可见。④ checker 全绿：17 个受控输入 15 个不变（新增纳入 3 个 Adreno 驱动源码）、屏侧 1 hunk `+17/-2`、新内核含新串/旧串消失/仍有 grant1 名单/无 uke 补丁、`kernel.json` 对齐、三日志无 error、`stage1` 与 `grant1` 对照镜像未覆盖、`skip-owner` 仍 `c4c1420a…`、四个 QEMU 关键符号零漂移。⑤ **判读格**：`pf=1 q=1` ⇒ 共享 buffer 被 `fail():95` quarantine 且无复位，假设坐实 → 落修复（改 `fail()` 只对 `imported()` quarantine，驱动自有共享 buffer 走 `retireAfterQuiescence()`；**改行为，须单独一版并重核 checker/QEMU/DMA 生命周期**）；`pf=6` ⇒ 表重叠，改表隔离；`pf=3/8` ⇒ Busy/alias；`pf=0` 或全 `ffffffff` ⇒ 无拒绝记录/weak_import 异常（`nm` 已确认可见）。显示侧仍 `o>=1 且 c>=1` + `R=ok` 才算真通。⑥ 边界 `board_verified=false / flashing=false / device_touched=false / qemu_cells_run=0`；收尾只读探测设备**已回到 fastboot**（`a20248f7` / `slot=a unbootable=no` / 3791 mV），账本末条仍 `06:04:43Z` ⇒ **本轮零刷机零引导**（状态变化非本轮操作）；上一板上状态是 14:04 RAM 引导的 `stage1` XNU。回滚：恢复 `baseline-pre-stage2/` 两份 `.orig` 并重跑三步链，或删掉新镜像。充电前刷 `stock` vendor_boot，但别长时间墙充、别再 `restore-stock --yes`。
- **§69.30**：⭐ **`stage2` 已上机（用户「上机」授权，14:42 北京），屏照待拍**（`GPU-HANDOFF.md` §69.30）。只读预检：设备在 fastboot（`a20248f7`）、adb 空、`slot=a / unbootable=no / retry=7 / 3699 mV`（脚本两次 3698 mV，触发 `<3700` WARNING 但不阻断）、账本末条仍 `06:04:43Z`（3201 条）、`check-stage2-probe-rebuild.py` 复跑全绿、产物 `24961f7e…4470` 与判据 JSON 一致、UEFI `043f7120…` 在位；再审 `cmd_xnu()`（`:165-183`）确认只写 `vendor_boot` + RAM `fastboot boot`。`MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_stage2.img` ⇒ flash `OKAY 0.057s`、boot `OKAY 0.189s`、无 `Load Error`；账本 `06:42:36Z preflight` / `06:42:38Z flash` / `06:42:38Z preflight` / `06:42:38Z boot` 全 `rc=0`，日志 `logs/flash-stage2.log`。引导后 fastboot/adb 皆空 ⇒ 面板唯一输出。`boot`/`recovery`/`userdata` 未动、未用 `fastboot reboot`；`stage1`/`grant1` 镜像未覆盖保留 A/B 对照。⚠️ **`board_verified=false / screen_photo_captured=false` 保持**：刷写与引导成功 ≠ 板上验证，`pf/q/bk/dr/gf/rn` 是否打印、`-3` 是否变化必须等屏照；`stage2` 零行为改动，`-3` 仍在是预期。判读格见 §9 第 9 条。边界 `flashing=true / device_touched=true / userdata_written=false / qemu_cells_run=0`。电池 3698 mV 偏低，充电前刷回 `stock` vendor_boot，但别长时间墙充、别再 `restore-stock --yes`。
- **§69.31**：⭐⭐⭐ **`stage2` 屏照判读：`pf=1 q=1` 板上坐实共享 buffer quarantine 污染，`-3` 第一次被钉到确定代码行**（14:46，纯判读、零设备动作、零重建；`GPU-HANDOFF.md` §69.31）。屏照 `logs/2026-11-13-14-46-26-183.jpg`（⚠️ **没拍到顶部 `MUYU LIVE` 行**、也没有 `MUYU A` 行，但 `PROBE st=97 cp=10 b=24` + `B`/`R`/`GPU`/`ROOT` 全套齐全 ⇒ 读数可用）。① 逐字：`MUYU B i48 o40 f1 s=e00002c2 b0 n4 t6=e00002c2 t7=4 ok=3 vm=1 pf=1 q=1 bk=1 dr=2 gf=1 rn=4`；`R PUB no device: device: Vulkan(VkResultCode(-3))`；`GPU calls1 refused=0 o=1 c=0 f=0 fb=0 tc=0 ts=ffffffff hold=1 pres=1 l=1 w=1`；`ROOT c=6/0/0 d=4/0`、`INIT q=6`、`BSD s=8`；`V op=3 bad=0 cl=2 call=557 sel=6 st=e00002d6 at=1 lmf=0 mb=0/0 tr=30 gr=7`。② ⭐ **`pf=1`** = `preflight():209` 参数/quarantine 段，**`q=1`** ⇒ `buffers_[0]` 已被 `fail():90-101` 隔离；`bk=1` 排除「buffer 未就绪」；`gf=1` 说明 `fail()` 跑过；`rn=4` 与 `B n=4` 相等 ⇒ 4 次失败同源、无额外拒绝路径。新增 6 字段全非 `ffffffff` ⇒ weak_import 绑定成功、观测版没白跑。**`pf=1` 直接排除 §69.28.5 备选 ② 表重叠（`pf=6`）、③ Busy/alias（`pf=3/8`）与静态常量参数错误**。③ 归因链闭合：一次 GPU fault → `fail()` 对驱动自有共享 `buffers_[0]` 调 `quarantine()` → `NativeAdrenoMemory.h:122` 只置 `quarantined_=true` 且**无复位路径** → 之后每个新 VM 的第一个 `vm_.map` 在 `:209` 返回 `0xe00002c2` → `createVm` 失败 → `mount() f=1` → `IOServiceOpen` Busy（`xe00002d5`）→ Turnip → `-3`；`ok=3 vm=1` ⇒ 是污染后的累积效应，不是 GPU 整体不可用。④ 十个旧字段与 stage1 屏照逐字相同、`R`/`GPU`/`ROOT` 不变 ⇒ **零行为改动板上确认**，`-3` 仍在是预期。⑤ ⚠️ **下一步才满足 §69.29.5 落修复条件**：改 `fail()` 只对 `imported()` quarantine、自有共享 buffer 走 `retireAfterQuiescence()`；**改行为，须单独授权、单独一版**，重核输入/checker/QEMU 符号/DMA 生命周期，先离线重建、上机另行说明。判据 `pf=0`（或 `q=0`）+ `ok/vm` 增长 + `GPU c>=1` + `R=ok`。⑥ 边界 `board_verified=false` 保持（GPU 未接管、桌面未进）、`minus3_persists=true`；新增 `stage2_probe_effective=true / quarantine_hypothesis_confirmed=true / preflight_refusal_site=1 / board_screen_captured=true`；本轮 `flashing=false / device_touched=false / qemu_cells_run=0`，账本末条仍 `06:42:38Z` ⇒ 零刷机零引导。
- **§69.32**：⭐⭐⭐ **按 §69.31.5 落 `fix1`：第一次改驱动行为，离线重建完成、停在「待上机」**（用户「继续」；14:52，零刷机/零引导/零格 QEMU）。① 方案：`fail():112` 曾把 `mappings_` 每个 memory 都 `quarantine()`，而 `buffers_[i]` 由 `GpuContext.cpp:95` `allocate()` 一次、`:132` 内核 VM 与 `:185` 每个进程 VM 共享，`quarantine()` 无复位 ⇒ 一次 fault 永久污染（板上 `pf=1 q=1`）。新增 `isolateAfterFault()` helper：`if(memory->imported() || memory->system())memory->quarantine();`，替换 `fail()` 循环与 `map():344` 两处显式 `quarantine()`；用户导入 IOSurface 与 `allocateSystem()` 的 BO 仍隔离，`regions_[].tables` / poison / fault_streams 全不动。② ⚠️ **刻意偏离 §69.31.5 原计划**：**不**调 `retireAfterQuiescence()` —— 它只 `--device_references_` 不等 DMA，而 `fail()` 路径没有 TLB unmap/静默证据，提前减引用会让 `release()`（`:315`）有机会释放仍被 GPU 写的页；本版**只跳过 quarantine、保留引用**，`release()` 仍 Busy ⇒ DMA 生命周期安全，引用泄漏是既有行为、不新增风险。③ 单变量：`baseline-pre-fix1/` 冻结 17 输入 + 两份 `.orig` + 内核/payload；diff **2 hunk**，17 个输入只有 `NativeAdrenoVm.cpp` 变了；⭐ **屏侧 `BravearkLastLog.c` 逐字节不变**，`pf/q/bk/dr/gf/rn` 全保留供上机判读。④ 三步链参数与 stage2 逐字相同，产物 **`fw/vendor_boot_gpu_fix1.img` 77,393,920 B / sha `a004368b…37b7`**、内核 `2c3bc032…d50d`、payload `ce8d7838…37a`、cmdline **996 B 与 stage2 逐字相同**。⑤ 新 checker `UniXNU/scripts/check-fix1-rebuild.py`（+ tree 副本，只能从 `UniXNU/scripts/` 运行）**全绿**：16/17 输入不变、屏侧逐字节相同、**切掉 helper + 还原两处调用后与 `.orig` 逐字节相同**、helper 仅一个 `if`/一次 `quarantine()`/不含 `retireAfterQuiescence` 或全局写、新内核仍含 16 字段格式串 + grant1 名单 + 无 uke、`kernel.json` 对齐、`skip-owner` 仍 `c4c1420a…`、**6 个关键符号零漂移** ⇒ §69.17 QEMU 补丁仍适用、三对照镜像未覆盖、三日志无 error。判据 JSON `out/muyu/tree/build/host-turnip-input/fix1-rebuild.json`。⑥ ⚠️ **待上机**：只写 `vendor_boot`、17 线 UEFI RAM 引导、屏照含顶部 `MUYU LIVE`；**主判据 `pf=0 q=0`**（不再有 preflight 拒绝），`pf=1 q=1` ⇒ 修复未生效，`t7=3` ⇒ `regions_[].tables->quarantine()` 成为下一 blocker（本版刻意未动），`gf>0` 预期；`o>=1 且 c>=1` + `R=ok` 才算真通。⑦ 边界 `board_verified=false / flashing=false / device_touched=false / qemu_cells_run=0`、`minus3_persists=true` 保持，账本末条仍 `06:42:38Z` ⇒ 零刷机零引导。回滚：覆盖 `baseline-pre-fix1/NativeAdrenoVm.cpp.orig` 重跑三步链，或直接刷回 `stage2` 镜像。
- **§69.33**：⭐⭐⭐ **`fix1` 已上机（用户「上机」授权，15:06 北京），屏照待拍**（`GPU-HANDOFF.md` §69.33）。只读预检：设备在 fastboot（`a20248f7`）、adb 空、`slot=a / unbootable=no / retry=7`、预检 `getvar` 3781 mV（脚本 `preflight` 两次读到 3692 mV，触发 `<3700` WARNING 但不阻断）、账本末条仍 `06:42:38Z`（3209 条）、`check-fix1-rebuild.py` 复跑全绿、产物 `a004368b…37b7` 与 `fix1-rebuild.json` 一致、UEFI `043f7120…` 在位；再审 `cmd_xnu()`（`:165-183`）确认只写 `vendor_boot` + RAM `fastboot boot`。`MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_fix1.img` ⇒ flash `OKAY 0.053s`、boot `OKAY 0.199s`、无 `Load Error`；账本 `07:06:37Z preflight` / `07:06:40Z flash` / `07:06:40Z preflight` / `07:06:40Z boot` 全 `rc=0`，日志 `logs/flash-fix1.log`。引导后 fastboot/adb 皆空 ⇒ 面板唯一输出。`boot`/`recovery`/`userdata` 未动、未用 `fastboot reboot`；`stage2`/`stage1`/`grant1` 三对照镜像 sha 未变。⚠️ **`board_verified=false / screen_photo_captured=false` 保持**：刷写与引导成功 ≠ 板上验证；**`fix1` 是行为改动**，离线全绿不能证明 `-3` 消失，`minus3_persists=true` 也保持到读到屏照。屏照须含 `MUYU LIVE`，逐字抄 `B`（16 字段）/`R`/`GPU`/`A`；**主判据 `pf=0 q=0`**，判读格见 §9 第 10 条 / §69.32.6。边界 `flashing=true / device_touched=true / userdata_written=false / qemu_cells_run=0`。回滚：重刷 `fw/vendor_boot_gpu_stage2.img` 回到 §69.31 已判读状态。电池 3692 mV 偏低，充电前刷回 `stock` vendor_boot，别长时间墙充、别再 `restore-stock --yes`。
- **§69.34**：⭐⭐⭐ **`fix1` 屏照判读：修复板上生效，但 `-3` 仍在、失败点下移到 `core->open()`**（15:11，纯判读、零设备动作；`GPU-HANDOFF.md` §69.34）。屏照 `logs/IMG_20261113_151052.jpg`（**含 `MUYU LIVE p=13 t=230`**）。① 逐字：`B i48 o40 f2 s=e00002c2 b0 n4 t6=0 t7=0 ok=7 vm=1 pf=0 q=0 bk=0 dr=0 gf=1 rn=0`；`R PUB no device: device: Vulkan(VkResultCode(-3))`；`GPU calls=1 refused=0 o=1 c=0 …`；`V op=3 bad=0 cl=2 call=525 sel=6 st=e00002d6 …`；`G mediaserverd/o1/r0/c1/p0/x0 backboardd/o2/r2/c1/p454/xe00002d5 n3`；`UP lg=10/1/1/1 s=1/0 op=3/2 mf=2/4 i=48 r=p`；`ROOT c=6/0/0 d=4/0`、`INIT q=6`、`BSD s=8`。② ⭐ **主判据 `pf=0 q=0 rn=0` 命中**：`t7=4→0`（createVm 完成）、`t6=c2→0`、`ok=3→7`、`bk/dr` 归零是「无拒绝故未采样」⇒ quarantine 修复板上确证生效；`UP op=3/2` 与 `V op=3 cl=2` 印证 **3 个用户客户端成功 mount、2 个已关闭、1 个 live**（`vm=1`），`ok=7 = 4 失败 + 3 成功` 三计数器自洽。③ ❌ **`R` 的 `-3` 与 `GPU o=1 c=0` 逐字未变** ⇒ 全通判据仍未满足。⭐ **新硬证据 `f1→f2` 且 `s=e00002c2` 不变**：`Publish.cpp:381` 的 `space?2:1` 表明 `f=2` 只在 **createVm 成功、下游 `core->open()` 非 0** 时写下；`s=BadArgument` ⇒ 新拒绝点唯一落在 `NativeAdrenoUserClient.cpp:129-131` 的 `!vm/!submit/!ring / bits 越界 / vm|submit|ring->poisoned()`。`i48 o40` 合法、`vm` 是刚成功的新 ProcessVm ⇒ **只剩三个 poison 位候选**；`gf=1` + `H h5 r4 f1`（5 hang / 4 restart / 1 失败 restart）说明确有一次 GPU fault，最强候选是**全局 `submit`/`ring` poison 未复位** ⇒ 部分客户端 `core->open()` 被拒 ⇒ Busy（`xe00002d5`）⇒ Turnip `-3`。⚠️ **未板上确证**：屏上目前无字段暴露这三个 poison 位。④ **结论**：`fix1` 有效但不是全通修复，**不要回滚、不要退回 sandbox/uke**；⚠️ 继续按旧判据表读 `pf/q` 会误判「已解决」。⑤ **下一版应是纯观测 `stage3`**（改行为须另行授权）：补三个 poison 位 + 命中分支 + `g.opened/g.closed` + 失败进程名 + 3 个成功 mount 的客户端身份；零行为改动、同一 printf 追加、重核 checker/QEMU 符号、先离线重建。坐实后再评估 poison 复位（须重核 DMA 静默/TLB unmap，别照搬 `retireAfterQuiescence()`）。⑥ 边界：本轮零刷机/零引导/零重建，账本末条 `07:06:40Z`、源码与镜像 sha 未变；`fix1_board_effective=true / quarantine_blocker_cleared=true / minus3_persists=true`（**板上确认**）/ `board_verified=false` / `downstream_blocker=core_open_BadArgument`。

- **§69.35**：⭐⭐ **型号映射澄清（用户指出）：muyu 是 Adreno 735，740 是 sheng 的**（15:2x，纯静态审计、零设备动作、零源码改动）。① 本板 `board.toml [gpu] model="Adreno735"` / `0x43030b00` / **1.5 MiB GMEM** / `gen7-11-0`；sheng 是 `0x43050a01` / 3 MiB。`GPU-PORT.md` §1、`GPU-GMU-MAP.md:86` 从 10-28 就这么写 ⇒ **不是新发现**；§13 的 chip id 不匹配早已修过。② `drivers/Adreno740/` 是**共享 A7xx 框架目录名**（寄存器映射相同），不是硬件声明，**不要改名/分叉**；profile 表刻意复用 `0x43030b00`（`GPU-UKE-ALIGNMENT.md:21`，只是 `name` 打印成 `Adreno732` 会误导日志）。③ ⭐ **已知欠账 `GPU-PORT.md` §1 第 5 条仍在板上生效**：`GpuPowerProbe.cpp:1378` 写 `NativeAdrenoA740Hwcg[]`（`MuyuA735Hwcg.h` 明说 **9 个寄存器值不同**，而 `MuyuA735ApplyHwcg()` 只有 host tests 用、**未编进内核**）；`:1387` 的 `GMEM_RANGE_MAX` 按 **3 MiB** 写、超 A735 一倍；`:1029` 自检硬编码 A740（只影响探针计数，非 `-3` 来源）。`AdrenoCompilerProbe.c` 的 A740 id 是 host-only，排除。④ ✅ **下游归因全不受影响**：quarantine / poison / `core->open()` 都是纯软件状态机 ⇒ **`fix1` 不回滚、`stage3` 方案照旧**；⚠️ 但上游 `gf=1` + `H h5 r4 f1` 的**反复 fault 可能源自 #③ 的错误 hwcg/GMEM**（静态推断、**未板上确证**，屏上暂无 chip id / GMEM / hwcg 选择字段，§69.34 屏照也没抄 `MUYU F` 的 FSR/FAR）。⑤ **给 `stage3` 追加只读观测**：`profile` 的 chip_id/gmem/gmu_chip_id、实际选用的 hwcg 表、GMEM_RANGE 写入值，下一张屏照**必须抄 `MUYU F`**（FAR 落在 GMEM 附近 ⇒ GMEM 越界坐实）。改这些参数属**另一个行为变量**，须单独授权、不要和 poison 复位混版。⑥ 边界：账本末条 `07:06:40Z`、源码/镜像 sha 未变，`flashing=false / device_touched=false / board_verified=false`；新增 `muyu_gpu_is_adreno735=true / a740_profile_params_still_active=true / chip_model_root_cause_board_verified=false`。

- **§69.36**：⭐⭐ **按 §69.34.4 + §69.35.5 落纯观测 `stage3`，离线重建完成、停在「待上机」**（用户「继续」；15:4x，零刷机/零引导/零格 QEMU、**零行为改动**）。⚠️ **不是修复版**，`-3` 仍在是预期。① **审读后收敛**：§69.34.4 五类观测里三类屏上已有 —— `g.opened/g.closed` 是 `MUYU UP op=`（`Publish.cpp:1301`）、成功 mount 的客户端身份/失败进程名是 `MUYU G` 的 `o/r/c/x`（`Publish.cpp:1117` 的 `record->opens++` 在 `refusal==0` 之后 ⇒ `o` 就是成功次数；15:11 `mediaserverd/o1`+`backboardd/o2`=3 与 `UP op=3` 自洽）、fault 的 FSR/FAR 是现成的 `MUYU F`（**上一张没抄**）⇒ 只补真正缺的。② **四个源码改动、20 受控输入 16 不变**：`NativeAdrenoUserClient.cpp` 加 `native_adreno_open_probe[8]` + `nativeAdrenoOpenRefusal()` helper，把 `open():129-131` 复合 BadArgument 的 return 包一层（§69.29 同手法、单语句语义保住），记 poison 位掩码（1 vm/2 submit/4 ring）、命中分支（1 null/2 input_bits/3 output_bits/4 poison）、bits、次数，**只写自己 probe、原值返回**；`NativeAdrenoService.cpp` 记选中 profile 的 `chip_id/gmem/gmu_chip_id`；`GpuPowerProbe.cpp` 记 `gpuHwInit()` 实际写的 hwcg 表（1=A740）、表项数、GMEM_RANGE；`BravearkLastLog.c` 三个 `weak_import` 声明 + **扩两行现有 printf**（`MUYU B` 16→19 字段加 `ps/br/ob`，`MUYU PROBE` 3→9 字段加 `ci/gm/gi/hw/hn/gr`），**零新增 printf/零新增屏行**。③ ⚠️ 首次构建在 `archive-12` 挂过：probe 全局放在文件首行、`-nostdlibinc` 下 `uint32_t` 不可见（`unknown type name`），已挪到 include 之后，最终三步链全绿、三日志无 error。④ 产物 **`fw/vendor_boot_gpu_stage3.img` 77,393,920 B / sha `75896e43…720c56b`**、内核 `1fb58bf3…cb35`、payload `b376003e…ecf1`、cmdline **996 B 与 fix1 逐字相同**。⑤ 新 checker `UniXNU/scripts/check-stage3-rebuild.py`（+ tree 副本，只能从 `UniXNU/scripts/` 运行）**全绿**：16/20 输入不变、四份源码各自「删白名单诊断行 + 还原 printf/return」后与 `.orig` **逐字节相同**、新增行不得触碰 `quarantine|poison()|fail(|release(|gpuWr(`、屏侧 printf 行数不变、tree 四副本 sha 一致、新内核含两新串 + 保留 16 字段 B 行 + grant1 名单 + 无 uke、`skip-owner` 仍 `c4c1420a…`、三个新 probe 符号可见。⭐ **符号位移如实断言**：3 个**代码**符号零漂移 ⇒ §69.17 QEMU 补丁仍适用；3 个**数据**符号因新增 3×32 B 全局精确平移 `+0x20/+0x40/+0x40`（**不再谎报六符号零漂移**）。判据 JSON `out/muyu/tree/build/host-turnip-input/stage3-rebuild.json`。⑥ ⚠️ **待上机**，判读格见 §9 第 11 条，`MUYU F` 必须抄。⑦ 边界：账本末条仍 `07:06:40Z`，`flashing=false / device_touched=false / board_verified=false / minus3_persists=true`，新增 `stage3_probe_built=true / stage3_board_verified=false`；四个对照镜像 sha 未变。回滚：覆盖 `baseline-pre-stage3/` 五份 `.orig` 重跑三步链，或刷回 `fix1`。
- **§69.37**：⭐ **`stage3` 已上机（用户「在线」授权，15:56 北京），屏照待拍**（`GPU-HANDOFF.md` §69.37）。只读预检：设备在 fastboot（`a20248f7`）、adb 空、`slot=a / unbootable=no / retry=7`、`getvar` 3757 mV（脚本 `preflight` 两次 3670/3668 mV，触发 `<3700` WARNING 但不阻断）、账本末条仍 `07:06:40Z`（3217 条）、`check-stage3-rebuild.py` 复跑全绿、产物 `75896e43…720c56b` 与 `stage3-rebuild.json` 一致、cmdline 996 B 与 fix1 逐字相同、UEFI `043f7120…` 在位；再审 `cmd_xnu()`（`:165-183`）确认只写 `vendor_boot` + RAM `fastboot boot`。`MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_stage3.img` ⇒ flash `OKAY 0.056s`、boot `OKAY 0.189s`、无 `Load Error`；账本 `07:56:27Z preflight` / `07:56:29Z flash` / `07:56:30Z preflight` / `07:56:30Z boot` 全 `rc=0`，日志 `logs/flash-stage3.log`。引导后 fastboot/adb 皆空 ⇒ 面板唯一输出。`boot`/`recovery`/`userdata` 未动、未用 `fastboot reboot`；`fix1`/`stage2`/`stage1`/`grant1` 四对照镜像 sha 未变。⚠️ **`board_verified=false / screen_photo_captured=false` 保持**：刷写与引导成功 ≠ 板上验证；本版**纯观测、零行为改动**，19 字段 `B` 的 `ps/br/ob` 与 9 字段 `PROBE` 的 `ci/gm/gi/hw/hn/gr` 是否打印、weak_import 是否绑定、`-3` 是否变化必须等屏照，`minus3_persists=true` 保持、**`-3` 仍在是预期**。屏照须含 `MUYU LIVE`，逐字抄 `B`(19)/`PROBE`(9)/**`F`(FSR/FAR，上一张漏抄)**/`G`/`UP`/`R`/`GPU`；主判据 `ps!=0 br=4` ⇒ poison 假设坐实（改行为的复位版须单独授权、核 DMA 静默/TLB unmap），`hw=1 hn=54 gr=12fffff` ⇒ §69.35 的 A740 hwcg + 3 MiB GMEM 窗口板上坐实，`ci=43030b00 gm=1536` ⇒ A735 profile 正确。判读格见 §9 第 11 条。⚠️ 改 hwcg/GMEM 与 poison 复位是**两个独立行为变量**，别混版；不要回滚 `fix1`、不要退回 sandbox/uke。边界 `flashing=true / device_touched=true / userdata_written=false / qemu_cells_run=0`，新增 `stage3_board_running=true / stage3_board_verified=false`。回滚：重刷 `fw/vendor_boot_gpu_fix1.img` 回到 §69.34 已判读状态。电池 3668 mV 偏低，充电前刷回 `stock` vendor_boot，别长时间墙充、别再 `restore-stock --yes`。

- **§69.38**：⭐⭐⭐ **16:00 屏照判读：画面最终停在 `verify_swap failed` 刷屏，没翻到 MUYU 面板**（16:1x，纯判读、零设备动作；`GPU-HANDOFF.md` §69.38）。用户「最后停在这里」指的是**屏幕画面最终定格在照片这一屏**，不是让助手停手（上一轮误解，已纠正）。① ⚠️ **两条用户澄清决定方向**：刷屏日志**自第一次 `MUYU GPU o=1` 起就一直存在**、不是 `stage3` 引入，以前刷屏之后都会翻到 `MUYU LIVE` ticker 面板、**这次没翻过去**；⭐ **16:19 再纠正：`o=0` 的早期版本都能看到苹果 logo，只是从第一次 `o=1` 起才看不到**（曾误读成「所有版本从没到过 logo」，已作废）⇒ **`o=1` 是拐点**：刷屏开始 + logo 消失 + `o=1 c=0` 首帧冻结三者同起点，指向「显示 hand-off 首次成功后首帧再没完成」，**不是**「GPU 从未打通」（归因候选、未板上确证）。② 逐字：`logs/IMG_20261113_160026.jpg` 全屏 `NativeLegacyDisplay: verify_swap failed for swap id: 2292…2344` 连续递增、末行 2344 后定格；中间唯一非刷屏行 `tx_flush:1330: disk1 xld 7329 tx stats: # 100 owait 14 7100us finish 113 … close 0us prog … flush 1852us`（APFS tx flush 统计 ⇒ 内核仍在跑 I/O、不是 panic）；**全屏零 `MUYU` 行**（`LIVE`/`B`/`PROBE`/`F`/`G`/`UP`/`R`/`GPU` 一个都没有）。③ ⇒ **§69.36.4 判读格完全无法应用**：`ps/br/ob`、`hw/hn/gr`、`ci/gm/gi`、`F` 的 FSR/FAR、`R` 的 `-3`、`GPU` 的 `o/c` **全部零读数**，`stage3` 本版板上**没有产出任何观测数据**，poison 假设与 §69.35 欠账仍未确证（既没证实也没证伪）。④ 机制早有结论（`HANDOFF-2026-10-27.md` §G.2）：`verify_swap failed for swap id: %d` 是**原厂 family 对每次被拒 swap 打印的一行**，字符串在闭源 kernelcache、不在我们源码；§G.4 正确做法是 `kickOnce()` 丢帧而非拒绝 swap。⭐ **静态待查点**：编译版 `UniXNU-muyu-17/drivers/GopFramebuffer/NativeLegacyDisplay.cpp` 的 `kickOnce():239`/`validateSwap():413` **都 grep 不到 `swap_hold` 读取**，`native_legacy_swap_hold` 只被 `BravearkLastLog.c:453` 写入、被屏上 `hold=` 读 ⇒ 变量可能被写了但驱动没消费（未板上确证，且 §G.4 说的是 `NativeMobileDisplay` 另一份实现）。⑤ ⚠️ **不要把「停在刷屏」归因为 `stage3` 回归**：纯观测零行为改动、checker 全绿、用户明确说刷屏自 `o=1` 就有；区分「翻页失败是 `stage3` 引入」与「随机时序」的唯一单变量实验是 **A/B 刷回 `fix1`**（当时能出 `MUYU LIVE p=13 t=230`），须单独授权上机。⑥ ⚠️ **战略含义**：以前能读到 `MUYU B` 只是文字控制台 dump 恰好赢了刷屏、不是 GPU 接管显示，这次连 dump 都没赢 ⇒ **下一版不该继续往屏上加 probe 字段**，应先让 MUYU ticker 稳定上屏（查 ④ 的 hold 消费点）或换不依赖 scanout 的通道（msgbuf/串口/心跳行）；按 §49.1 不要叫用户重拍同一屏。⑦ 边界：账本末条仍 `07:56:30Z`、四对照镜像与源码 sha 未变，`flashing=false / device_touched=false / board_verified=false / minus3_persists=true`，新增 `stage3_panel_reached=false / stage3_observation_data=false / verify_swap_flood_is_historical=true / o1_is_logo_regression_point=true / apple_logo_visible_when_o0=true`；`screen_photo_captured=true` 但 `muyu_panel_captured=false`。电池 3668 mV，充电前刷回 `stock` vendor_boot。

---

## 2. 硬规矩（每条都有事故或白跑的代价）

### 2.1 设备与刷机红线

1. **只用 `tools/muyu-flash-mode.sh`**，禁止手敲 `fastboot flash boot` / `fastboot flash recovery` / `fastboot reboot`。
   正常模式**只写 `vendor_boot`**；`boot` + `recovery` 只有 `restore-stock --yes` 会动。历史上两次误刷导致整机重刷。
2. **UEFI 是 `fastboot boot` 从 RAM 引导的，断电 / 重启即丢**。每次充电回来继续 UniXNU，**必须重新刷诊断镜像**。
3. **回 rec 充电前必须先刷原厂** `fw/stock/vendor_boot.img`（`bash tools/muyu-flash-mode.sh stock`）。
   该模式不引导 UEFI、不重启、不写 `boot`/`recovery`/`userdata`。fastboot 本身不充电；
   长按电源 10–15 s 关机 → 插**墙充 5V/2A+** 1–2 h（电脑 USB 500 mA 会进「充电图标↔尝试启动」死循环）。
4. **`fastboot getvar` 输出必须先落文件**，不要接 `head` / `grep -m1` —— SIGPIPE 会打死设备端 fastboot 服务。
5. 刷前只读预检：`fastboot devices` 非空、`adb devices` 空、`current-slot=a` / `slot-unbootable:a=no`、
   `battery-voltage`（< 3700 mV 脚本会告警）、镜像 sha 与账本一致。设备不在 fastboot 就**等用户**，不要轮询、不要自旋等待器。
6. **面板是唯一输出通道**（USB CDC ACM 与 Linux pstore 已拆，见 `ABANDONED-CHANNELS.md`；`PREVIOUS XNU LOG` 不跨复位）。
7. **27 / 17 线红线**：`UniXNU-muyu`（27 线）与 `UniXNU-muyu-17`（17 线）的 `board.toml` 禁止互拷；
   ad-hoc 显示 / GPU 改动只落 `UniXNU-muyu-17/package/`。`UniXNU-27` / `UniXNU-uke` / 框架仓 `payload.py` 勿动。
   17 线基线镜像是 `fw/vendor_boot_nofs1.img`（`c7635cdb`），不是 `vendor_boot_27-uNNN.img`。

### 2.2 与用户交互的约定（用词是缩略语，别按字面理解）

| 用户说 | 实际含义 | 该做什么 |
| --- | --- | --- |
| **「在线」/「设备在线」** | 设备已回到 **fastboot** | 先只读预检，再按既定镜像刷入 |
| **「发了」** | 屏照已放进 `logs/` | 找 `logs/` 里最新时间戳的 jpg，判方向 → 逐行转录 → 写章节 |
| **「刷原厂」** | 要去 rec 充电 | 立即 `stock`，不引导 UEFI、不续做实验 |
| **「继续 … 但我也要回 rec 充电」** | 设备让给充电 | 本轮**不刷 UniXNU 镜像**，只做离线准备 / 构建 |

### 2.3 单轮标准作业流程（不要跳步）

```
选一个且只选一个假说 → 写清「若 X 则假说不成立」的判读格 → 只加只读诊断行
→ 备份改动文件 + 记 sha → 三步构建 → 核对 sha / strings / 构建日志无 error
→ 等设备回 fastboot → 预检 → 刷 → 等 ticker 刷 3–4 遍 → 完整屏照
→ 逐行视觉转录（不采信 OCR 十六进制）→ 按事先写好的判读格下结论
→ 追加 GPU-HANDOFF.md 章节 + 今日工作日志 + 核对 ledger
```

**判读格必须在刷机前写好。** 事后找解释是这一个月最大的时间黑洞。

---

## 3. 当前精确状态（截至 19:52 北京）

> ⚠️ 本表是 **11-12 的快照**；11-13 的最新状态（根镜像已恢复、`grant1` 与 `stage1` 都在板上跑过并拍过屏照、
> **14:08 判读 `t7=4 / ok=3 vm=1 / -3` 仍在；14:46 `stage2` 判读 `pf=1 q=1` 污染坐实；
> 15:06 `fix1` 上机、15:11 判读 `pf=0 q=0` 修复生效但 `-3` 仍在，失败点下移到 `core->open()`；
> 15:2x 澄清 muyu=A735、15:4x `stage3` 纯观测版已离线重建；
> **15:56 `stage3` 已上机；16:00 屏照判读：画面停在 `verify_swap failed` 刷屏、没翻到 MUYU 面板，
> 本版零观测数据；16:19 用户澄清 `o=1` 是「看不到苹果 logo」的拐点**）
> **16:3x `fix2` 离线重建完成**（A735 hwcg/GMEM 参数 + `swap_hold` 消费两刀），
> 镜像 `fw/vendor_boot_gpu_fix2.img` sha `a563780a…6bf6b`、checker 全绿，停在「待上机」）
> 以 **§1 导航 §69.23–§69.39** 与 `GPU-HANDOFF.md` §69.39 为准。

| 项 | 值 |
| --- | --- |
| `vendor_boot` | **`bsd1`** `fw/vendor_boot_gpu_bsd1.img`（19:47 刷入，仍在跑，未刷回原厂） |
| 设备连接 | 19:47 前在 fastboot；19:51 拍照后未复查连接；**下次接手先确认是否仍在跑 / 是否要充电** |
| 电池 / 槽位 | 刷前 3890 mV；`slot=a unbootable=no` |
| ledger 末条 | `2026-11-12T11:47:40Z boot fw/boot-17line-043f7120.img … rc=0`（UTC） |
| userdata 首块 | **11-13 13:19 已整盘刷回备份**（`fw/userdata-backup/userdata-root.sparse`，`OKAY / 202.047s / rc=0`，窗口与 sha 全对）；此前另有一次定点 8 KiB sparse 写入（`ud_fix_8k.sparse`，`OKAY` 但 `fastboot fetch` 不支持、无回读）。✅ **11-13 13:28 屏照已确认恢复**：`ROOT c=6/0/0 d=4/0` / `INIT q=6` / `BSD s=8` / `TOC n=127`（§69.25.1）。11-12 那次是 TWRP root 下定点 8 KiB + 回读；本次官方 rec 无 root，该路径不可复现。详见 **§69.22 / §69.23 / §69.25** |
| 源码基线 | `BravearkLastLog.c` = UP / ROOT / INIT / **BSD** 四条诊断行 + census `pass%3`；`bsd_init.c` 插桩已固化为补丁 `patches/xnu/zzzzzzzzzzzzzzzzzzzzz-bsd-stage-probe.patch`；备份 `/tmp/bsd1-backup/` |
| 未动 | `UniXNU-sheng` 工作树、主树、`Adreno740` 子模块、`fw/gpuaddon-17-full.bin`、owner trust、addon、boot-arg |

### 3.1 镜像清单（sha256 已复核，实验镜像均 77,393,920 B，原厂 100,663,296 B）

| 文件 | sha256 | 用途 / 何时用 |
| --- | --- | --- |
| `fw/stock/vendor_boot.img` | `69f1e38c6ab70050ef86e37a36319443c1eddc4355543ab5395b895d0bcb96fd` | 充电前必刷；**当前已刷** |
| `fw/vendor_boot_gpu_restart1.img` | `29963d92794fa51db5927e0d96a1951c558b2af59bc48116fee2331946b99976` | 补 GPU restart hook；§40 已证伪「缺 hook 是断点」 |
| `fw/vendor_boot_gpu_restart2.img` | `bf0a1c33c5306dff516c15e8c96e8e4fc10ea4daf063767b7c684e2f5e23358f` | Q7/Q8 诊断；§45 判 `-3` 与 GPU hang 解耦 |
| `fw/vendor_boot_gpu_restart3.img` | `ca332a0bcbe58984fa9242dd074d7c8e4224e4981c1fbc0bd3da5dbba5312201` | 严格只读 Q7 + `RT2 n0` 哨兵；待命 |
| `fw/vendor_boot_gpu_upper1.img` | `26ef2fb0ce41ce7d31c8f59e2394497a89fe77d7517aa4712c4ebe92cd15fcce` | 18:02–18:06 那轮（`LIVE p=24 t=260`，`PROC n=1`） |
| `fw/vendor_boot_gpu_root1.img` | `118a98e468fa0ed5c3ea9af98ef1a4729d8abc1ddd377e9e9eb12866e566ce9b` | 已刷（18:32）：`ROOT c=0/-1/-1 d=0/-1`，§54 |
| `fw/vendor_boot_gpu_init1.img` | `6300f4c2f2eaa4d6c17edda91b98d56b1d0e11eb96f0d923d05d1b992fd00a55` | 已刷（19:22）：`INIT q=0`，§55 |
| `fw/vendor_boot_gpu_bsd1.img` | `d029545cdb0ba69505cf8e819b7c014425205ba93766c5b0e256c072b4c3b964` | 19:47 刷入：`BSD s=8` / `INIT q=6` / `ROOT c=6/0/0 d=4/0`，§57 |
| **`fw/vendor_boot_gpu_vkd1.img`** | **`9154be66535cd66393e1e0477d611697a402999707db64c987431c4976e2dd9a`** | ⭐ **当前镜像（20:06 刷入）**：+`MUYU V` / `O` / `A` 三行，读驱动的客户端记录，§58 |
| `fw/vendor_boot_gpu_grant1.img` | `6520c152c34b318e062236e0c62f475f50c216ae57775d795184d59a4d907364` | §69.20 单变量修复（+`NativeSandboxGrantProcess`）；**11-13 13:20 已上机并在板上跑过**，`-3` 仍在 ⇒ 已证伪，保留作 A/B 对照（§69.25） |
| **`fw/vendor_boot_gpu_stage3.img`** | **`75896e43b31d1d578012bf1885669cb1b87f7408861a41c7c8d37959e720c56b`** | ⭐⭐⭐ **板上当前运行镜像（§69.36/§69.37/§69.38，15:56 已上机，16:00 屏照已拍但未翻到 MUYU 面板）**：纯观测、零行为改动、**板上零观测数据**；`MUYU B` 16→19 字段加 `ps/br/ob`（poison 位掩码 / BadArgument 命中分支 / 次数），`MUYU PROBE` 3→9 字段加 `ci/gm/gi/hw/hn/gr`（A735 profile 实测 + 实际写入的 A740 hwcg 表与 GMEM 窗口）；判读格见 §9 第 11 条，**`MUYU F` 必须抄**；`-3` 仍在是预期 |
| **`fw/vendor_boot_gpu_fix1.img`** | **`a004368bdf97c648d7a7433abae75d511b2a3d15d755a1e6289361f3254a39b7`** | ⭐⭐⭐ **A/B 对照 + 回滚镜像（§69.32/§69.33/§69.34，15:06 已上机、15:11 屏照判读完成）**：第一次改驱动行为，`fail()` 不再隔离驱动自有共享 `buffers_[i]`；**板上确认 `pf=0 q=0 rn=0`、createVm 跑通、3 次 mount 成功**，但 **`-3` 仍在**，失败点下移到 `core->open()`；**是有效修复、不要回滚**，判读格见 §9 第 10 条；`stage3` 读数异常时重刷此镜像回到已判读状态 |
| `fw/vendor_boot_gpu_stage2.img` | `24961f7e4f2b4174c54f11be11e86e0403a83b3d76c919a608080ac544147e70` | ⭐⭐⭐ **A/B 对照 + 回滚镜像（§69.30/§69.31，14:42 已上机，14:46 屏照判读完成）**：`MUYU B` 加 `pf/q/bk/dr/gf/rn`，实测 `pf=1 q=1` ⇒ quarantine 污染坐实；**不是修复版、零行为改动**，`-3` 仍在是预期；`fix1` 读数异常时重刷此镜像回到已判读状态 |
| `fw/vendor_boot_gpu_stage1.img` | `bb7f5b8ee58f7caa78ce047912737dd8253b4ebd054f290275331a1776e715af` | ⭐ **上一版（14:04 已上机，14:08 屏照判读完成，§69.27/§69.28）**：§69.26 纯观测版，`MUYU B` 加 `t6/t7/ok/vm`；实测 `t7=4 ok=3 vm=1`、weak_import 绑定成功、`-3` 仍在；保留作 A/B 对照 |

UEFI：`fw/boot-17line-043f7120.img`（sha `043f7120…5bd3`），**每次 `xnu` 都要带 `MUYU_UEFI=` 指定 17 线副本**。

### 3.2 关键屏照（`logs/`）

| 文件 | 方向 | 内容 |
| --- | --- | --- |
| `2026-11-12-12-14-41-437.jpg` | **正立**（EXIF=1） | `restart1`：`RT n1`、`-3`、NVT frames → §40 |
| `2026-11-12-13-18-48-138.jpg` | **正立** | `restart2`：全零但 `-3` 仍在、`calls=7` → §45 |
| `2026-11-12-13-48-39-917.jpg` | **180° 倒置但 EXIF=1** | `restart1` 稳态卡滞：**缺最顶部 LIVE 行** → §49 |
| `2026-11-12-18-06-19-393.jpg` | 需顺时针 90° | `upper1`：`LIVE p=24 t=260`、`PROC n=1` 无 launchd → §52 |
| `2026-11-12-18-35-51-844.jpg` | 需**逆时针** 90° | `root1`：`LIVE p=21 t=344`、`ROOT c=0/-1/-1 d=0/-1` → §54 |
| `2026-11-12-19-37-56-454.jpg` | 需**逆时针** 90° | `bsd1`（修 manifest 前）：`BSD s=2`、`INIT q=0` → §56 |
| `IMG_20261112_195112.jpg` | **正立** | ⭐ `bsd1`（修 manifest 后）：`BSD s=8`、`INIT q=6`、`ROOT c=6/0/0 d=4/0`、`GPU calls=7 refused=7`、`R PUB no device: Vulkan(-3)` → §57 |

---

## 4. 技术地图：断点到底在哪

### 4.1 剩余三道关卡（必须按顺序过）

| # | 关卡 | 状态 | 说明 |
| --- | --- | --- | --- |
| **0** | **为何到不了 `load_init_program`** | ✅ **已结案（§56–§57）** | 根因是 **userdata 首块 8 KiB 被改写**（`fd877808 ab9f4456`，非 `SHNGROOT`）⇒ `NativeUfs::startRoot()` 返回 false ⇒ 无 root 媒体 ⇒ `IOFindBSDRoot()` 死等。19:45 定点写回 manifest 后，`BSD s` 2→8、`INIT q` 0→6、`ROOT` 6/0/0 |
| **1** | **GPU 服务拒绝所有客户端** | ~~当前 blocker~~ → **已结案**（§59 / §9 第 2 条） | ⚠️ **旧误读已更正（§69.24）**：`GPU calls=7 refused=7 o=0` 是 **GOP 帧 hand-off 计数**（`LegacyDisplayProbe.cpp:87-98`），不是内核拒绝客户端；`V`/`O` 行证明内核放行。`R` = `PUB no device: Vulkan(VkResultCode(-3))` 与 `B i=48 o=40 f=0` ⇒ 故障在**客户端 Vulkan 初始化**，不在内核窗口分配 |
| 2 | `VK_ERROR_INITIALIZATION_FAILED`（`-3`） | 代码链已定位，**未修** | §45.4：`NativeAdrenoMetalPlugin.mm:400 ← nmb_device_open ← Device::system_default_with`。候选① 17 线 addon 缺 `turnip-darwin-keep-device-after-restart.patch`（需重建 addon）；② 重建 owner-signed `NativeAdrenoMetal.bundle` 补失败上下文（会连带改 bundle 签名 / CDHash / trust / addon **四个输入**） |
| 3 | 提交 / fence / hangcheck / restart 层 | 未验证 | §40 的 `RT n1` + `H h1 r1` + `Z5 f32/4/0/0` 说明这条路径存在，但不是每次复现 |

### 4.2 三 boot 对比（§49.7，读懂这张表才不会走回头路）

| 观测 | §40 `restart1` / 3420 mV | §45 `restart2` / 3861 mV | **13:48 `restart1` / 3883 mV** |
| --- | --- | --- | --- |
| `PROBE st/cp/b` | — | `97 / 10 / 24` | **`97 / 10 / 24`** |
| `GPU calls/refused/tc/ts` | — | `7/7/7/00000000` | **`0/0/0/ffffffff`** |
| `B i` | — | `i48` | **`i40`** |
| `RT / H / Z5` | `n1` / `h1 r1` / `f32/4/0/0` | 全零 | 全零 |
| `X7` / `R` / `NATIVE-NVT` / Q | `68f2` / `-3` / 有 / 有 | `68f2` / `-3` / 5 行 / 有 | **`K0` / 无 / 无 / 无** |

**结论**：同一份 `restart1`、电量只差 22 mV，两次结果不同 ⇒ **非确定性启动分叉，不是电量**。
13:48 那条路径**连 `-3` 都没产生**，比 §45 的 device 创建失败**更靠前**。
⇒ §45 的强结论（「`-3` 与 GPU 空闲无关 → 转 Turnip 补丁」）**不能套用到本分叉**。

### 4.3 屏上关键行的字段解码（速查）

- **`PROBE st=97 cp=10 b=24`** = 内核 GPU bring-up **全绿**：
  `st` 是 `%x`，`0x97 = 0x90+7` → GPU context 七步走到 `Ready`；`cp=10` → C10，CP/ring/fence 全成功、
  publishMetal gate 已过；`b=0x24` 是 `ArmSmmu4KObserveStreams()` 的**只读 observe 正常终态**，
  **不是 attach 卡死**（成功 attach 写 `0x21`、失败写 `0x30`；barrier 只是 `dsb sy`）。
  ⚠️ 别被 11-11 的「attaching CPU never came back」（crumb `0x1b`）先例带偏，本 boot 不是那类 MMIO hang。
- **`GPU calls=0 refused=0 tc=0 ts=ffffffff`**：`ts` 是 transfer status 初值 `-1`，**从未被写过**；
  配合 `tc=0` ⇒ 显示 transfer / frame offer 路径**一次都没进过**，不是「跑了但失败」。
  ⚠️ **这一条只适用于 manifest 修复前**。修复后（§57）是 `calls=7 refused=7 tc=6 ts=00000000`：
  路径**进来了 7 次、全被拒、transfer 状态被写过**。判「有没有用户态」要看 `calls`，不要再看 `tc/ts` 的初值。
- **`hold=1 pres=1`**：`muyu-dump-at=20` 在 t≈20 s 拿走面板、系统继续 headless 启动，**本来就该是一屏文本**；
  §45 也是 `hold=1`。所以「没苹果 logo」本身不是新故障，但「以往这时候早到桌面」说明以往显示客户端能抢回面板。
- **`LIVE p=<pass> t=<uptime>`**：ticker 每 **5 s** 整批重绘所有 `MUYU` 行（`muyu_dump_later()` → `IOSleep(20s)` →
  `for(pass=1;;) IOSleep(5000)`）。用户说「刷了好几遍一样的内容」= **ticker 还活着**（内核没 panic / 没死锁），
  但所有计数器逐遍不动 ⇒ **稳态卡滞**。拍屏照**必须包含这一行**；`p`/`t` 直接给出卡了多久。
  `pass%3==0`（`upper1` 起，15 s）会带 `muyu_proc_census()` 进程普查，可看 launchd / SpringBoard / render server 起没起。
- **`i40` vs `i48`**：`muyu_gpu_bits[0]` = publish 交给 user connection 的 GPU client VA 窗口位数 `2^input_bits`。
  两个 boot 不同 ⇒ 非确定性，**但本 boot `B f0 s0 b0 n0`（`mount_fail` 全零）说明没有 client 尝试 open，
  窗口压根没被检验过** ⇒ 只能列为必测变量，**不能当因果证据**。

---

## 5. 已证伪清单 —— 不要重复投入

| 结论 / 假说 | 判定 | 出处 |
| --- | --- | --- |
| mode 0/1/2 换表、CPU 写 TTBR0、CP aperture 切换、客户端页表、提交时序 | **整族证伪**，别再在换表上投入 | §24–§33 |
| 「上游 `2949667` 判决 mode 2 冻 ring」 | 依据不成立，那只是 15 分钟就被证伪的假设 | §33 / §34 |
| `-3` = `VK_ERROR_DEVICE_LOST` | **订正**：`-3 = VK_ERROR_INITIALIZATION_FAILED`，`-4` 才是 DeviceLost | §33 |
| 「断点就是缺 GPU restart hook」 | 补上 hook 后 `RT n1` 成功但 `-3` 原样不动 → **证伪** | §38 / §40 |
| secure-mode 假说（`CP_SET_SECURE_MODE`） | 降级 | §41.4 / §42.4 |
| 上游「全局 GFSR latch 挂住 render server」 | 13:48 路径未命中（`Q8 g0`、无 R 行、无 `fail()`） | §42.3 / §49 |
| `restart2` 的诊断扰动导致全零 | 降级（13:48 跑的是 `restart1`，同样全零） | §49.7 |
| 电量决定启动分叉 | 排除（3861 vs 3883 mV，差 22 mV） | §49.7 |
| 13:48 是「拍得早 / 更早阶段瞬时快照」 | **撤销**，是稳态卡滞；「再拍一张稳定屏照」建议作废 | §48 → §49.1 |
| 「`R`/`X7` 消失 ⇒ 上层已解」 | 错，是整条链路未启动 | §49.6 |
| **「屏上有 `MUYU LIVE` ⇒ `load_init_program` 已进入」** | **作废**：display probe 在 t+10 s 先起 ticker（`LegacyDisplayProbe.cpp:518`），`muyu_dump_start()` 有 first-caller-wins 守卫，init 里那次是空转 | §54.3 |
| 「`ROOT c=0/-1/-1 d=0/-1` ⇒ 卡在 owner mount 路径」 | **降级为待定**：只能说明两个 helper 未执行到第一句赋值；因上一条，A（未进 init）与 B（进了卡在 1–4）无法区分 | §54.4 |
| **「root mount 已完成」** | **撤销**：原本靠「ticker ⇒ 已进 init」支撑，该前提作废；`init1` 的 `q=0` 进一步说明连 init 入口都没到 ⇒ `vfs_mountroot()` 是否返回**无证据** | §55.4 |
| **「`q=0` / `PROC n=1` / owner-mount 全初值 = 多个独立故障」** | **全部结案为单一根因**：userdata 首块 8 KiB 被改写。修好后 `q=6`、`ROOT 6/0/0 d=4/0`、用户态自起 | §57.4 |
| 「userdata 在 11-11 之后没被写过，所以首块不该坏」 | 事实成立但推论错：首块确实被**外部**改写（非本工程），19:45 已定点修复；该口径到此为止 | §56.6 / §57.1 |
| USB CDC ACM / Linux pstore 日志通道 | 已拆除，约 29 轮零结论，**别重启** | `ABANDONED-CHANNELS.md` |

**两处会话内判读更正（未单列章节，别再犯）**：`%.40s` 并没有截断内容；
`gpuWr(0x0908, 0)` 在 Q0–Q6 stall 分支里原本就有，**不是 restart2 新增**。

**一个诱人的假线索**：`bridge/priming_cache_loader.cpp` / `graphics_cache_loader.cpp` 里的
`VK_ERROR_INITIALIZATION_FAILED` 是 **"Offline only" 的 host 测试 shim**（文件头明示），不是运行时路径，
不要据此推断 shader cache 校验失败（§45）。

---

## 6. `init1` 结果：`q=0`，断点已前移到 `bsd_init()`

### 6.1 已执行（2026-11-12 19:22 北京）

预检通过（fastboot、`slot=a unbootable=no`、3914 mV、adb 空、两个镜像 sha 一致）后刷 `init1` +
RAM boot UEFI，`OKAY`。19:26 屏照 `logs/IMG_20261112_192601.jpg`（sha `5d069d0a…`，**本身正立**）。转录与解释见 §55。

```
MUYU ROOT  c=0/-1/-1 d=0/-1
MUYU INIT  q=0
PROC n=1 zomb=0 launchd=0- backboardd=0- SpringBoard=0- Liveact=0-
MUYU LIVE  p=25 t=260 …
MUYU PROBE st=97 cp=10 b=24
```

### 6.2 结论（已被 §57 覆盖，保留以备追溯）

`init1` 的 `q=0` ⇒ `load_init_program()` 在 t=260 s 时没有被调用。`bsd1` 随后给出 `s=2`，
再结合屏上的 `NativeUfs: no valid root manifest in userdata (status 1, manifest version 0, magic fd877808 ab9f4456)`
⇒ 断点在 `setconf() → IOFindBSDRoot()`，**`vfs_mountroot()` 从未被调用**（详见 §56）。

### 6.3 ⭐ 本轮：8 KiB 修补 + `bsd1` 复测（§57）

19:45 在 stock recovery 里定点写回 userdata 首块（`SHNGROOT` manifest 原样 + 4 KiB 零，8192 B，回读校验通过），
19:47 刷 `bsd1` 复测。屏照 `logs/IMG_20261112_195112.jpg`（sha `378a95e0…`，**本身正立**）：

```
MUYU UP    lg=10/1/1/1 s=1/0 op=5/4 mf=0/0 i=48
MUYU ROOT  c=6/0/0 d=4/0
MUYU INIT  q=6
MUYU BSD   s=8
MUYU GPU   calls=7 refused=7 o=0 c=0 f=0 fb=0 tc=6 ts=00000000 hold=1 pres=1 | w=0
MUYU PROBE st=97 cp=10 b=24
MUYU B     i=48 o=40 f=0 s=0 b=0 n=0
MUYU R     PUB no device: Vulkan(VkResultCode(-3))
tx_flush:1338: disk1 xid 6041 tx stats: # 120 owait 17 4727us finish 123 …
```

⇒ **内核启动链已打通**：`s=8` = `load_init_program()` 已返回；`q=6` = cache prepare 已返回；`c=6/0/0` = cache
挂载成功；`d=4/0` = Data 段成功；`tx_flush` = APFS 在提交事务；`calls=7` = **用户态客户端确实在跑**。

### 6.3.1 下一轮：只追 `-3`（判读格见 §57.6）

`calls=7 refused=7` + `o=0` ⇒ 7 次 **GOP 帧 offer 全被 presenter 拒**（走 CPU copy、scanout 未交出），**不是内核拒绝客户端**（§69.24 更正）；`R` 行给出 `-3`（`VK_ERROR_INITIALIZATION_FAILED`）。⚠️ **§69.25 再更正**：判「连接是否被拒」不能看 `MUYU A` 的 `re00002c7` —— 那是驱动自己模拟 gate 的日志值，且 `IOUserClient.cpp:4566-4567` 把 `kIOReturnUnsupported` 显式转成 success。要看 **`MUYU B` 的 `f/s/n`**（`mount_fail`）与 **`MUYU G` 的 `r`/`x`**（refused / last_refusal）；本轮 `B f1 s=e00002c2 n5` + `G …/xe00002d5` 才是真正的拒绝证据。
但 `B i=48 o=40 f=0`（`native_adreno_mount_fail[0]==0`）说明**内核侧 publish/mount 没记失败**。

⇒ 下一轮只做只读诊断，判两件事之一：`-3` 出在**客户端初始化序列**（驱动枚举 / 版本握手），还是
**内核窗口分配**。**不**重建 addon / trust / turnip，**不**改 GPU 行为。屏照必须含 `R`、`B`、`GPU`、`BSD`、
`INIT`、`ROOT`、`LIVE`。

设备要回 rec 充电时，先 `stock` 再关机（§2.1）。

### 6.4 `MUYU UP` 行格式与判读决策树

```
MUYU UP lg=<stage>/<init>/<start>/<dt> s=<found>/<status> op=<opened>/<closed> mf=<code>/<count> i=<ias> r=<reason|'-'>
```

| 字段 | 来源 / 含义 |
| --- | --- |
| `lg` | `native_legacy_stage` / `init_result` / `start_result` / `dt_attached` → legacy display bring-up 走到哪步（1..10） |
| `s` | `native_legacy_surface_found` / `surface_status` → IOSurface 有没有找到 |
| `op` | `native_adreno_publish_stats()` 的 `opened` / `closed`（纯只读 getter）→ **有没有用户态客户端连过 GPU 服务** |
| `mf` | `native_adreno_mount_fail[0]/[3]` → 连接是否被拒、拒了几次 |
| `i` | `muyu_gpu_bits[0]`（§4.3 的 `i40/i48` 变量） |
| `r` | `muyu_gpu_reason[0]`，`-` = 没写 = 没走到 §45.4 的 `-3` 失败路径 |

所有符号 `weak_import` + 判空，缺链接打印哨兵（`0xffff` / `0xffffffff` / `-2`）而不改变行为。

| 观测 | 结论 | 下一步方向 |
| --- | --- | --- |
| `op=0` + `lg` 停在早期 + census 无用户态进程 | 关卡 1 = **用户态根本没起来** | launchd / root volume / 挂载时序 |
| `op=0` 但 `lg` 已到 10，且 census 显示正常用户进程 | 服务发布了却没有 client | Metal bridge 加载 / bundle 签名 / trust cache |
| `op>0` 且 `mf≠0` | client 连了但被内核拒 | 对照 `mount_fail[0]` 的 1/2/3 与 `i` 窗口大小 |
| `op>0`、`mf=0`、`r≠'-'` | **客户端侧初始化失败**（当前状态：`op=5/4`、`mf=0/0`、`r` = `-3`） | 关卡 2 成主战场：读 `native_bridge_metal_open` 的 `-3` 返回点 |
| `op=0`（无客户端） | 用户态没有客户端来连 | 回头看 launchd / census，而不是改 GPU |

### 6.5 屏照处理规范（这里翻过好几次车）

1. **每轮先判方向**，不要沿用上一轮的结论。12:14 / 13:18 是**正立**；13:48 是 **180° 倒置但 EXIF `orientation=1`**
   （相机倒着拿、没写方向标记）；18:06 需**顺时针** 90°；18:35 需**逆时针** 90°。
2. **判方向用行框长宽比，不要靠肉眼或预览渲染。** 未校正时 Vision 会把每行检成「高瘦」框（w≈60、h≈600–900）
   ⇒ 文字沿竖直方向排布；校正正确后行框变「宽扁」且左对齐。预览面板的呈现方向可能误导，
   本轮为此在 180° 上绕了两圈才定案。`/tmp/ocr3.swift` 可复用（打印 y_top/x/w/h/conf）。
3. 一律带 `ffmpeg -noautorotate`；不带会按 EXIF 自动转竖版导致 crop 越界。
   中间 PNG 若带旋转 metadata 也会越界 ⇒ **原图单条滤镜链一次完成**（若需 90° 校正，先校方向再定 crop）：
   `ffmpeg -noautorotate -i <jpg> -vf "transpose=2,crop=1900:620:280:0,scale=3800:1240" out.png`
4. **zsh 不做变量分词**，`crop` 参数写死，别用 shell 变量拼。
5. 无 PIL，EXIF / 尺寸用 `sips` / `exiftool`。
6. 流程：**Vision OCR 定位行序 → 窄条 3× 放大 → 逐行视觉转录**。
   **不采信 OCR 的十六进制**（`0` 常被读成 `O/D/8`）；关键行必须放大复核。
7. 右侧 `x=2600…3840` 也要单独裁切查一遍，别漏行。**注意控制台会滚屏**：可见窗口可能从批次中段开始，
   例如 18:35 那张的 `PROC` 普查行就落在画面外——拍之前先确认 `p%3==0` 那批在窗口内。

---

## 7. 命令与资产速查

### 7.1 三步构建链（改 `package/*.c|cpp` 后重新出镜像）

```bash
cd /Users/diannao/pad7pro-xnu

python3 UniXNU/unixnu build kernel -D UniXNU-muyu-17 \
  --uefi /Users/diannao/pad7pro-xnu/Mu-Silicium \
  --edk2 /Users/diannao/pad7pro-xnu/Mu-Silicium/Mu_Basecore -j 8

python3 UniXNU/unixnu build payload -D UniXNU-muyu-17 -j 8 \
  --data-ramdisk fw/data-seed.img \
  --owner-trust-cache fw/trust-21H461/owner-trust-17-full.bundle \
  --gpu-addon fw/gpuaddon-17-full.bin \
  --unique-chip-id 0000000000000001 \
  --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-addon=1 --kernel-arg sheng-gpu-metal=1

python3 UniXNU-muyu-17/scripts/patch-vendor-boot.py \
  --base /tmp/base96.img --payload UniXNU/out/muyu/payload/payload.img \
  --output fw/<新镜像名>.img
```

- `/tmp/base96.img` = `fw/vendor_boot_nofs1.img` 复制后 `truncate -s 100663296` 零填充；
  **必须 96 MiB**，否则 72 MiB payload 过不了分区上限检查。
- `--gpu-addon` 要的是 `make-gpu-addon.py` 产出的 **blob**，不是 bundle 目录。
- `--trust-cache` 与 `--owner-trust-cache` **二选一**，后者替换前者、跳过白名单。
- `cmdline > 1000 B` 会被 `build payload` 硬闸拒绝；加参数前先删无消费者的旧参数。
- 构建后必核：`shasum -a 256`、`strings <img> | grep` 新诊断串、构建日志 `grep -i error\|warning`。

### 7.2 刷机脚本模式

| 命令 | 行为 |
| --- | --- |
| `status` | 只读：镜像、设备、槽位、电池 |
| `preflight` | 检查 / 修复启动槽位 |
| `xnu [IMG]` | 刷 UniXNU `vendor_boot` + `fastboot boot` UEFI（需 `MUYU_UEFI=`） |
| `write [IMG]` | 只刷不引导（重复启动普查用） |
| **`stock`** | 原厂 `vendor_boot`，充电用，**不引导 UEFI / 不重启** |
| `restore-stock --yes` | 写 `boot` + `vendor_boot` + `recovery`，仅在 rec 都进不去时用 |
| `ledger` | 打印 `logs/flash-ledger.tsv` |

### 7.3 离线能力（设备充电时可推进，不占真机窗口）

- `UniXNU-muyu-17/scripts/audit-gpu-restart-offline.py` → `build/gpu-restart-offline/report.json`（§41）。
- host 软件 MMIO 测试：`test_ring.cpp` / `test_submit.cpp` / `HostKernelHooks.cpp`。
  ⚠️ 断言失败可能来自 §30/§31 的实验开关，**不能当故障证据**；缺 `native_prelink` / kernel doubles / Preempt 链接时
  用 `UniXNU/` 主树 + shim headers。
- QEMU **没有 A735/A7xx 真 GPU 设备模型**，虚拟环境只能做软件 MMIO 仿真与静态审计，不能验证真机提交路径（也就不写 hwcg / GMEM 范围这些型号相关寄存器，§69.35.4）。
- 上游快照 `/tmp/sheng-latest`（`origin/main=e893717`）；本地 `UniXNU-sheng` 工作树仍 `251b291`。
  17 线缺 `turnip-darwin-keep-device-after-restart.patch`。

---

## 8. 上游与边界

- `sheng` 在 **iPadOS 27 + macOS 27.3** 上已跑通完整方案（signed ZAP firmware + Turnip + metal2vulkan + GMU）。
  但 17 线是 iPadOS 17、**boot-arg 前缀不同**、`Adreno740` 驱动落后上游 8 天 ⇒ §42.5 定案：
  **不整体升级，只单点移植**。
- 27 线（`UniXNU-muyu`）与 17 线 `board.toml` sha 不同（`2aed9d8a` vs `9b158cc2`），分线设计，禁止互拷。
- 工具链已装齐（Rust 1.98 + iOS target、meson 1.12.1、bison 3.8.2、llvm-as 23.1.4、glslangValidator 16.6），
   细节见 `GPU-HANDOFF.md` §2；`Mesa sparse-checkout` 曾被 zsh 单词拆分搞坏，修法见 §6.3。

---

## 9. 未解决 / 开放问题

1. ~~关卡 1（进不了 init）~~ → **已结案**（§56–§57）：根因是 userdata 首块 8 KiB 被外部改写，19:45 定点修复。
2. ~~GPU 服务拒绝全部客户端~~ → **已结案**（§59）：`vkd1` 的 `V` 行 `sel=6 st=00000000 at=1 bad=0`、
   `O` 行 `SpringBoard|SpringBoard|backboardd` ⇒ 内核全部放行，`calls/refused` 是 GOP 帧 offer 计数，不是拒绝。
3. **`-3` 落在设备创建的哪一步（当前 blocker）**：栅栏已排除（§63.2：`wf=5/5/5/0/0/0`，环空）。
   内核侧不能再给答案（`-3` 在最后一个 ABI 调用之后），所以 `vkd6` 改问**调用序列的形状**（`MUYU Q`/`Y`），判读格 §63.3。
   ⚠️ 四个已废通道别再用：`K` 行的 `6xxx` 分支不可达（publish 模式不走冒烟，§60.2）；
   `vkd3` 的 `C` 行读的是从未赋值的 `native_adreno_call_ring`（§61.3）；ABI 版本不匹配已划掉（`abi=2`，§62.2）；
   `WaitFence` 超时不是故障（§63.2）。
   **在 `Q`/`Y` 读出来之前，任何重建 addon / trust / turnip / bundle 的动作都是浪费。**
4. **非确定性分叉的触发条件**：同一份 `restart1`、电量差 22 mV，结果三条链路不同；
   现在有了可用的用户态，这一条可以重测，但**不要和 `-3` 混在一轮**。
5. **屏照覆盖**：现在一屏约 50 行/批，`PROC` 只在 `p%3==0` 的那一批出现，且落在 `BSD` 之后、下一批 `LIVE` 之前。
   **下一张必须同时含 `LIVE`、`R`、`B`、`GPU`、`BSD`、`INIT`、`ROOT`**；要 `PROC` 就等到 `p` 是 3 的倍数。
6. **userdata 首块是单点脆弱点**：被外部改写 8 KiB 就会让整条启动链停在 root 发现（2026-11-12、
   2026-11-13 **两次**）。备份 `fw/userdata-backup/userdata-root.sparse`（sha `4c90d7a7…`）完好；
   ✅ **修复已脚本化**：`UniXNU-muyu-17/scripts/repair-userdata-manifest.py`（`prepare`/`diagnose`/`verify`/`commands`/`fix`），
   但 ⚠️ **`diagnose`/`fix` 都要求 root recovery**：11-12 是 TWRP，11-13 官方 rec 是 `uid=2000(shell)`，
   读/写 `/dev/block/sda32` 都被 DAC 拒。13:00 这轮按「不管是不是同因」跳过诊断，
   改走 fastboot 定点 sparse + 整盘刷回，详见 §69.22 / §69.23。
   ⚠️ **充电前只用 `stock`，别再跑 `restore-stock --yes`**（会刷回原厂 recovery，
   充电时挂/格式化 `/data` 是两次首块改写的共同嫌疑）。
7. 无热管理（无 thermal zone / DVFS、`wdt=-1`），长时间运行注意发热。
8. **上一版（`stage1`，§69.26/§69.27；14:04 已上机）只读判读格**：屏照必须含 `MUYU LIVE`，逐字抄
   `MUYU B` 的 `t6/t7/ok/vm` + `R` + `GPU` + `A`。`t7=2` ⇒ SMMU 表页/arena 形状
   （`ArmSmmu4KLive.c:11-26, :330-340`）；`t7=3` 或 `0x30|op<<8` ⇒ `vm_.attach`；
   `t7=4+i` ⇒ `vm_.map`；`t7=0` ⇒ `createVm` 完成过、回本表第 3 条读 `Q`/`Y`。
   `ok=0` 表示本次启动一次都没成功 —— ⚠️ **14:08 实测 `ok=3 vm=1`，此预期被否**；
   `t7=2` 的倾向也被排除，实测 **`t7=4` = 第一个 `vm_.map(buffers_[0])` 失败**。
   显示侧 `o>=1 且 c>=1` + `R` 变 `ok` 才算真通，实测 `o=1 c=0` 不变 ⇒ 仍卡在设备创建。
   ⚠️ **`stage1` 零行为改动**，`-3` 仍在是预期，不要据此判定重建失败，
   也不要回滚到改 sandbox 属性 / 移植 uke 补丁。`stage2` 已于 14:42 上机（§69.30）、14:46 屏照判读完成（§69.31）。
9. **上一版（`stage2`，§69.29/§69.30；14:42 已上机、14:46 屏照已判读）只读判读格**：屏照必须含 `MUYU LIVE`，逐字抄
   `MUYU B` 的 `pf/q/bk/dr/gf/rn` + `t6/t7/ok/vm` + `R` + `GPU`。`pf` 直接给出
   `preflight()` 命中的 return 点：`1` 参数/quarantine（**`q=1` ⇒ 共享 buffer 被
   `fail():95` quarantine 且无复位，§69.28.5 假设坐实**）、`2` iova、`3` Busy 重叠、
   `4` output_bits、`5` span 形状、`6` `r.tables->overlaps`、`7` span 总长、`8` alias
   冲突、`0` 无拒绝记录。`gf>0` = `fail()` 跑过；`rn` 与 `B n=` 对比。⚠️ **`stage2`
   零行为改动**，`-3` 仍在是预期；**只有 `pf=1 q=1` 才落修复**（改 `fail()` 只对
   `imported()` quarantine、驱动自有共享 buffer 走 `retireAfterQuiescence()`，改行为
   须单独一版并重核 checker/QEMU/DMA 生命周期），`pf=6` 则改表隔离。显示侧判据同第 8 条。
   ⭐ **14:46 实测（§69.31）**：`pf=1 q=1 bk=1 dr=2 gf=1 rn=4`，`t6=e00002c2 t7=4 ok=3 vm=1` 与
   stage1 逐字相同 ⇒ **quarantine 污染假设板上坐实**，`-3` 第一次钉到确定代码行；表重叠 /
   Busy / alias / buffer 未就绪全部排除。**下一步只有改 `fail()` 的修复版，且须单独授权、
   单独一版、先离线重建**；判据 `pf=0`（或 `q=0`）+ `ok/vm` 增长 + `GPU c>=1` + `R=ok`。
10. **上一版（`fix1`，§69.32/§69.33；15:06 已上机、15:11 屏照已判读，§69.34）只读判读格**：
   屏照须含 `MUYU LIVE`，逐字抄 `MUYU B`（16 字段，屏侧源码逐字节未变）+ `R` + `GPU` + `A`
   **+ `MUYU UP` + `MUYU V` + `MUYU G`**。⭐ 主判据 `pf=0 q=0`；`pf=1 q=1` ⇒ 修复未生效
   （刷错镜像或 helper 未编入）；`t7=3` ⇒ `regions_[].tables->quarantine()` 下一 blocker；
   `gf>0` 预期。显示侧唯一全通判据仍是 `o>=1 且 c>=1` + `R=ok`。
   ⭐ **15:11 实测（§69.34）**：`pf=0 q=0 rn=0 bk=0 dr=0 t6=0 t7=0 ok=7 vm=1 gf=1`，
   `UP op=3/2 mf=2/4`、`V op=3 cl=2` ⇒ **quarantine 修复板上生效、createVm 跑通、
   3 次 mount 成功**；但 `R` 仍 `Vulkan(VkResultCode(-3))`、`GPU o=1 c=0` 不变。
   ⚠️ **不要只看 `pf/q` 就宣布解决**：`f` 由 `1` 变 **`2`** 且 `s=e00002c2` 不变 ⇒
   按 `Publish.cpp:381` 的 `space?2:1`，失败点已从 `createVm()` **下移**到
   `core->open()`（`NativeAdrenoUserClient.cpp:129-131`）；`i48 o40` 合法 ⇒
   只剩 `vm/submit/ring->poisoned()` 三个候选，最强是全局 `submit`/`ring` poison 未复位
   （`gf=1` + `H h5 r4 f1`），**尚未板上确证**。⚠️ **不要回滚 `fix1`**、不要退回
   sandbox / uke 补丁；下一版应是**纯观测 `stage3`**：补三个 poison 位 + 命中分支 +
   `g.opened/g.closed` + 失败进程名 + 3 个成功 mount 的客户端身份，改行为须另行授权。
   `board_verified=false / minus3_persists=true`（板上确认）保持。
11. **当前版（`stage3`，§69.36/§69.37/§69.38；15:56 已上机、16:00 屏照已拍）只读判读格**：
    ⚠️ **16:00 那张屏照没有翻到 MUYU 面板、19 字段一个都没读到（§69.38），本判读格尚未用上**；
    屏照须含 `MUYU LIVE`，
    逐字抄 `MUYU B`（**19 字段**）、`MUYU PROBE`（**9 字段**）、**`MUYU F`**、`MUYU G`、
    `MUYU UP`、`R`、`GPU`。
    ⭐ **主判据 `ps!=0 br=4`** ⇒ 三个 poison 位（1 vm / 2 submit / 4 ring）至少一位未复位，
    `core->open()` 的 BadArgument 板上坐实；下一步才是改行为的 poison 复位版，
    **须单独授权**并重核 DMA 静默 / TLB unmap。
    `ps=0 br=1` ⇒ null `vm/submit/ring`（回查 createVm 生命周期）；`br=2/3` ⇒ bits 越界、
    与 `i48 o40` 矛盾先复核读数；`ob=0` ⇒ open() 从未被拒、`f=2` 另有出处。
    `ci=43030b00 gm=1536 gi=70b0000` ⇒ A735 profile 正确；
    **`hw=1 hn=54 gr=12fffff`** ⇒ §69.35 的 A740 hwcg 表 + 3 MiB GMEM 窗口板上坐实，
    是 `gf=1` / `H h5 r4 f1` 反复 fault 的上游嫌疑；配合 `MUYU F` 的 FAR 落在 GMEM 区间
    ⇒ 越界，FSR 指向 clock/hang ⇒ 倾向错误 hwcg。
    ⚠️ 改 hwcg / GMEM 是**另一个行为变量**，不要和 poison 复位混在一版；
    不要回滚 `fix1`、不要退回 sandbox / uke 补丁。
    唯一全通条件仍是 `o>=1 且 c>=1` + `R=ok`。

---

## 10. 给接手 AI 的工作方式约束（这份文档存在的理由）

用户这一个月的实际体会是：**没有移植经验垫底，模型会把看起来 plausible 的路径挨个试一遍**，
烧钱烧时间却没进展。以下约束比换任何模型都有效：

1. **先读后动**：读完本文档 + `GPU-HANDOFF.md` §1 / §7 / §8 / §49 / §51 / §52，再碰任何命令。
   顶层 `NEXT-PROMPT.md` / `AGENTS.md` §6 已过期，别按它执行。
2. **一次只验一个假说**，且**先写判读格**（「看到什么就说明这个假说是错的」）再刷机。
   禁止并行试多条、禁止「顺便也加上」。
3. **区分证据等级**：代码静态推导 / 屏照逐字转录 / 推测，三者权重不同，结论里必须写明来源。
   OCR 的十六进制不算证据。
4. **优先只读诊断**：加打印行不动行为；重建 addon / trust / bundle / turnip 这类改四个输入的大动作，
   必须先有明确判据。
5. **不重走已证伪路线**（§5），尤其别再回到换表、restart hook、secure-mode、USB 日志通道。
6. **设备窗口最贵**：设备不在 fastboot 就等用户，不轮询；充电期间只做离线构建 / 静态审计；
   每轮结束必须写章节 + 日志 + 核 ledger，让下一轮不用重新推导。
7. **不报日期**。GPU 交接完成之前给任何时间估计都是猜。

### 附：可直接粘进新会话开头的冷启动提示词

```
项目 /Users/diannao/pad7pro-xnu，UniXNU-muyu-17（iPadOS 17 / 21H461 / **Adreno735**；
`drivers/Adreno740/` 只是共享 A7xx 驱动框架目录名，不是 GPU 型号）。
先读 UniXNU-muyu-17/docs/HANDOFF-COLD-START.md，再查 docs/GPU-HANDOFF.md（§56/§57 是最新结论）。
顶层 NEXT-PROMPT.md 与 AGENTS.md §6 已过期，不要执行其「下一步」。
硬规矩：设备不在 fastboot 不刷；回 rec 充电前先刷 fw/stock/vendor_boot.img；UEFI 由 fastboot boot
从 RAM 引导、断电即失效，继续前必须重刷诊断镜像；只写 vendor_boot；屏照必须含最顶部 MUYU LIVE 行。
⭐ 内核启动链已打通（2026-11-12 19:51），GPU 内核侧已排除嫌疑（20:09，§59）：
userdata 首块 8 KiB 曾被外部改写（非 SHNGROOT），19:45 在 stock recovery 定点写回 manifest 后，
bsd1 屏照给出 `BSD s=8`、`INIT q=6`、`ROOT c=6/0/0 d=4/0`、APFS tx_flush 活跃。
vkd1 屏照再给出 `V op=5 bad=0 cl=4 call=105 sel=6 st=00000000 at=1 inf=0 mb=0/0 tr=14 gr=5`、
`O SpringBoard|SpringBoard|backboardd`、`A n=6 SpringBoard/t4147/m00000000/re00002c7/a1/f0`
⇒ 内核 accept/attach/method/SMMU 四道全绿，**不是内核拒绝**（`GPU calls/refused` 只是 GOP 帧 offer 计数）。
vkd3 屏照进一步给出 `L` 行 `pr=2 tn=96 ex=98 sb=00000001 st=00000000`
⇒ 沙箱放行了 selector 0，96 次真实 ABI 调用全部成功，`-3` **不是**连接建立失败，Turnip 已进入 `vkCreateDevice` 内部。
vkd5 屏照把栅栏排除掉了（§63）：`W` 行 `wf=5/5/5/0/0/0` ⇒ 环 submitted==retired，最后那次等待零超时立即成功；
`S` 行 `6:34/17` 里的 17 次「失败」是 vkGetFenceStatus 的正常回答，不是故障。整机只有 1 次 Submit（`5:1`），
环把它退休了。也就是说 `-3` 出在**最后一个 ABI 调用之后的纯用户态一步**，内核这边再也看不到。
下一步只做一件事：刷 vkd6 读 `MUYU Q`（方法序列，连续相同压成 mxN，新→旧）与 `MUYU Y`
（最后四次 BoAlloc 的 序号/bytes/align），按 §63.3 判读格定断点。已划掉的支线：ABI 版本不匹配（§62.2）、
publish 模式的 Turnip 冒烟（§60.2）、`native_adreno_call_ring`（§61.3）、WaitFence 超时（§63.2）。
当前待刷镜像 fw/vendor_boot_gpu_vkd6.img，SHA-256 ce7c093f…f039aa9；屏上正在跑的是 vkd5（34670d1a… 那版）。
禁止：一上来重建 addon/trust/turnip、改 GPU 行为、并行试多条假说、重走 §5 已证伪路线。
每轮：写判读格 → 只读诊断 → 构建 → 刷 → 等 ticker 3–4 遍 → 完整屏照 → 逐行判读 → 写章节与日志。
```

---

**最新（§69.43，17:5x）**：`fw/vendor_boot_gpu_fix4.img`（`2e524437…235b`）已离线重建并校验全绿，
相对 fix3 **唯一变量**是 `gpuHwInit()` 的 hwcg 表由 GEN1 `a730_hwcg`(53) 换回本板 GEN2 家族
`a740_hwcg`(54)；本机 1.5 MiB GMEM、220/200 MHz、`validateSwap` hold 旁路、fix1/stage3 探针全保留。
✅ 已核实「完全使用 muyu 配置」的板级层早就是本机值（`board.toml [gpu]`、`gen-muyu-gpu-*`
生成的 GMU/SMMU/OPP、本机 `gen71100_*` 固件、ADT/UEFI GMU Pool、`CONFIG_XNU_NATIVE_ADRENO=y`、
内核里 96 个 `NativeAdrenoUserClient` 符号）；`ShengGmu*` / `Adreno740` 只是共享框架命名。
**本轮零刷机、零引导、零设备动作**，只读预检 `no fastboot device`，账本末条仍 `09:03:00Z`（fix3 boot），
上机须用户明确授权。判读 `PROBE hw=1 hn=54` / `gr=117ffff`；⚠️ 这一刀**不预测 `calls=0` 被修复**
（`calls` 在 display offer 链，`gpuHwInit()` 只由 `gpuStart()` mode>=9 与 hang restart 调用）。
`board_verified=false / minus3_persists=true` 保持不变。

---

*维护约定：每轮真机实验结束后，更新本文档 §0 / §3 / §6 / §9，并在 `GPU-HANDOFF.md` 追加逐轮章节。
本文档只保留「当前有效状态」，历史推导一律不留在这里。*
