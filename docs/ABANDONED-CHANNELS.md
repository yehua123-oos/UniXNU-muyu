# 已拆除的两条日志通道：USB CDC 与 Linux/pstore

**决定：** 2026-11-08 17:30（维护者），唯一输出通道 = 面板。见 `SCREEN-ONLY.md`。
**拆除：** 2026-11-08 21:xx（本次），代码、脚本、补丁、启动参数、构建钩子全部移除；
本页是唯一保留的存档，`docs/USB-DEBUG-PORT.md` 已随此次删除。

保留本页的理由只有一个：如果哪天要重启其中一条，**别从零开始**——下面这些数字是花掉
约 32 轮真机时间换来的，重启时可以直接用。

---

## 1. 为什么放弃（数据）

| 通道 | 投入 | 产出 |
| --- | --- | --- |
| USB CDC ACM | u128–u159，约 29 轮 | 枚举从未成功；0 个可用结论 |
| Linux init_boot + pstore | 3 个镜像（log-reader v1、v3、v4） | 从未刷过；v1 自造 boot header 被 ABL 拒绝，14:59 回落 recovery |
| 面板 | u143 起 | 每轮都有结论；换观测方式后三轮走到「只剩 DEPCFG」 |

另有一条代价更隐蔽：u136–u147 共 12 轮测的是被自家复位路径挂死的核心，读数无效。
修好观测方式后才发现，所以「USB 差一点就成了」这个印象本身是不可靠的。

## 2. 重启 USB 时该继承的测量值

- DWC3 基址 `0xa600000`（映射 `0xd93c`），QSCRATCH `0xa6f8800`；APPS SMMU `0x15000000`。
- **muyu 的 GCC 是 `0x100000`（cliffs），不是 vermeer 的 `0x139000`**；USB 电源域/时钟
  偏移必须按 `gcc-cliffs.c` 重取。GDSCR 是**绝对地址**不是 base+offset（u135）；
  读 GDSCR 会把总线读挂（u134）。
- GEVNTADRHI 接受 37 GB 环（awidth=64 是真的）；DEPSTARTCFG 能完成；
  **DEPCFG 入队但永不开始执行**；DCTL.CSFTRST 从不自清零（u141 试 3000 次、u147 试 20000 次）。
- 最终失败画像：ConnectSpeed 读到 Full-Speed 而 dcfg DevSpeed=0/High-Speed，UTMI/eUSB2 PHY
  疑似处于 suspend；SUSPHY 清零被排在 endpoint 命令之后，对 DEPCFG 已经太晚（§60.3）。
- 参考实现：`ios-vermeer-lab`（SM8550，456 行自包含 DWC3+CDC）与 `UniXNU-ace5`（SM8650，
  VID:PID `1209:0001`）。同一份驱动跨 SoC 适配是验证过的做法，问题不在驱动而在 PHY 状态。

## 3. 重启 Linux 侧时该继承的测量值

- 该内核：`CONFIG_DEVTMPFS` 关（`/dev` 要 mknod）、`CONFIG_VT` 关（Linux 点不亮面板）、
  `CONFIG_DEVMEM` 关（没有 `/dev/mem`，改不了 splash framebuffer）、`CONFIG_PSTORE_CONSOLE` 开
  → printk 落到 ramoops `0xa7000000`。
- **boot.img 的 ramdisk 无效**（`ramdisk_size = 0`，bootloader 也不认）；ABL 真正交给内核的是
  `init_boot` 的 ramdisk，并把它与 vendor_boot 的 vendor ramdisk 拼接，所以 347 个厂商模块仍可用。
- 自造 boot header 会把 kernel 放在 1584 偏移，而 ABL 从第一页 4096 读 → 起不来并回落 recovery。
  正解不是逆向 header，而是直接复用 `fw/stock/init_boot.img` 的 header（ramdisk 留在 page 1，
  只改 ramdisk 字节与 size 字段，offset 12）。
- 兜底姿态：USB gadget 起不来时按 reboot reason 热重启回 bootloader，**热重启保 ramoops，
  断电丢**。

## 4. 这次删掉了什么

| 路径 | 内容 |
| --- | --- |
| `drivers/MuyuDebug/muyu_usb_debug.c` | 1337 行，27 线在用的 CDC ACM 驱动 |
| `drivers/MuyuDebug/muyu_usb_protocol.h` | 102 行，描述符与 DMA 约束 |
| `package/MuyuUsbDebug.c` | 778 行，17 线逐字节存档副本 |
| `package/linux-log/{init,rb.c,build-initboot-reader.py}` | v3 取日志 init、ramoops 读取器、init_boot 封包器 |
| `package/collect-linux-readonly.py` | Linux 侧只读取证脚本 |
| `scripts/build-muyu-usb-debug.py` | 驱动 archive 构建（引用已不存在的 `kits/muyu/`） |
| `scripts/build-muyu-log-reader.py` | 自造 boot header 的 log-reader 封包器 |
| `patches/xnu/zzzzzzzzzzz-muyu-debug.patch` | `muyu_pstore.c` + `muyu_usb.c` 及 files/arm_init/serial_console 三处挂钩 |
| `docs/USB-DEBUG-PORT.md` | 移植方案原文（结论已并入本页） |

同时改的：`patches/xnu/wip/muyu-screen-wip-20261108.patch` 去掉 `muyu_usb_debug.c` 与
`muyu_usb_protocol.h` 两个整文件段及 `files.arm64` 注册行；`scripts/legacy-10063/build-xnu-4k-baseline.sh`
去掉 `libMuyuUsbDebug.a` 的 force_load 钩子；`board.toml` 去掉
`muyu_usb=1`、`muyu_usb_panic=1`、`muyu-usb-panic-at=<s>` 三组启动参数（本来就只是注释）。

**注意 `muyu_pstore.c` 与本仓的 `sheng-log` 不是一回事。** 前者是被删的 debug 补丁里的
Android pstore 写入器；`"sheng-log=1"` 由 `drivers/ShengUfs/NativeUfs.cpp` 与
`package/BootLog.c` 实现，未受影响。

## 5. 故意留下的尾巴

- `muyu_debug.c` / `muyu_log_ring.h`：控制台环形镜像，面板在用，**与 USB 无关**。
- 面板补丁里 `muyu_vc_status_paint()` 还有三个弱引用
  （`muyu_usb_stage_value/last_id/ring_code`，`__attribute__((weak))` + 空指针判断）。
  驱动删除后它们 resolve 到 0，该函数整行不画。这是**有意保留**的：删掉这三行要重算
  绘制布局（`6*gw + 11*gap`），而那段代码是拍照验证过的，不能盲改。下次真刷面板那一轮，
  应把这三格换成内核自己的 boot stage 数字，一并验证。
- `package/LegacyDisplayProbe.cpp:436`、`package/BravearkLastLog.c:346` 对
  `muyu_usb_debug_start` / `muyu_usb_state_line` 的 `weak_import` 调用：判空后调用，驱动不在即惰性。
- `docs/HANDOFF-2026-11-07.md` §47–§60、`docs/HANDOFF-2026-11-08.md` §4 里的叙述是**日期化记录**，
  按原样保留，不回改。

## 6. 要回滚怎么办

全部内容都在 git 里，删除提交的前一个版本即可取回：

```bash
git log --oneline --diff-filter=D -- drivers/MuyuDebug/muyu_usb_debug.c
git show <删除提交>^:drivers/MuyuDebug/muyu_usb_debug.c > /tmp/muyu_usb_debug.c
```

## 7. 仓库外的本地产物去哪儿了

删代码只是删了仓库里的东西。工作区里还堆着这些路线跑过的痕迹——旧的框架克隆、每次产出的
镜像、早期截图。2026-11-08 23:10 把它们收进 `~/pad7pro-xnu/_archive/`，索引是
`_archive/README.md`。**只移动，不删除**，`_archive` 不在任何 git 仓里。

| 归档位置 | 内容 | 为什么不在原地 |
| --- | --- | --- |
| `_archive/channels/` | 9 个镜像、166M：`init_boot-reader{,-v3,-v4}.img`、`log-reader-muyu.img`、`log-reader-stock.img`、`vendor_boot_{pstore,reader}.img`、`linux-reader/`、`Mu-muyu-debug-20261023.img` | 正是 §4 删除清单对应的产物；0 引用，且从没有过一次刷机结论 |
| `_archive/rounds/vendor_boot_27/` | 128 个历史轮次镜像、12G | `fw/` 里 158 个早期轮次，只保留被文档引用的 30 个（u91/u92/u98/u101 四个真修复、u157–u159 最后三轮、u83–u100 调试密集段等） |
| `_archive/repos/` | 11 个过期目录：`UniXNU-27`（摘链的 worktree，分支 `muyu-27` 仍在框架仓 `7bb3ad9`）、`UniXNU-sm8635-common`、`UniXNU-ace5`、`old-modules`、`ios-vermeer-lab`、`vm-qemu` 等 | 已合并或 0 引用 |
| `_archive/notes/handoffs/` | 15 份 0 引用的旧交接（`HANDOFF-2026-10-25*` 起） | 历史叙述，不进构建 |
| `_archive/captures/root/` | 50 个早期截图/录屏、1.0G | 无引用 |

`logs/` 故意没动：613 项被 `docs/` 和 `porting.toml` 按路径引用，移走会断取证链。
`fw/` 的构建输入（`vendor_boot_lite.img`、`data-seed*`、`ipsw/`、`gpu-firmware/`）也都在原地。

另有一处**从归档里救回来的改动**：`UniXNU-27` 的 worktree 里躺着一份未提交修复
（`scripts/build-xnu-4k-baseline.sh`，25 行），改的是 `prng_random.o_CFLAGS_ADD=-U…` 无条件
生效、抵消 `CFLAGS_EXTRA` 里的 `-DXNU_SOURCE_CORECRYPTO14=1`，导致干净对象目录下
`source_corecrypto14_original` / `start_once` 未定义。`UniXNU` main 里没有这个修复，已存为
`_archive/patches/UniXNU-27-uncommitted-build-xnu-4k-baseline.patch`，需要时 `git apply`。

