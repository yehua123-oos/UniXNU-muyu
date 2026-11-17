# GPU 上机工程 · 交接文档（2026-11-11 续）

> 本文件是 **Tier-2 Metal 插件接管** 阶段的交接。Tier-1（GPU 内核开机进桌面）已闭环；当前唯一未完成的是
> 让 GPU **真正接管渲染**（需要用户态 scanout 客户端 = AdrenoMetal 插件 addon）。
> 阅读顺序：先看 §0 结论，再按 §6 的「下一步精确命令」直接续做。

> ⚠️ **2026-11-12 16:42 起，冷启动请改读 [`HANDOFF-COLD-START.md`](HANDOFF-COLD-START.md)。**
> 本文件的 **§0 结论、§6 下一步命令、§10 检查清单是 2026-11-11 快照，已过期**；
> 当前状态、硬规矩、已证伪清单和下一轮动作一律以冷启动文档为准。
> 本文件继续作为**逐轮历史记录**使用，其中 §49 / §50 / §51 / §52 是当前有效结论；§52 是最近一次真机屏照的判读。

---

## 0. 一句话结论

- **Tier-1 已完成并上机验证**：设备用新 GPU 内核正常进桌面（当前运行内核 `b2846224`，来自镜像
  `fw/vendor_boot_gpu_tier1e2_probe.img` / `8a0f6ff0`）。开机链三处坑（ADT PackedRAMDisk → 显示注册参数
  → EARLY_BOOT POST）全部闭环。
- **卡点已定位（设备坐实）**：hand-off 桥已通，但 `NativeGOPPresenter::offer()` 判据里 `gpuClient` 为 false
  ⇒ 仍是 CPU copy。`gpuClient` 只由用户态经 `AGXDeviceUserClient::runTimed()` 置位，入口是 **Metal 插件进程
  里的 `NativeAdrenoMetalPlugin.mm`**。17 线从未打过这个 GPU addon。
- **本机工具链已从零装齐**（Rust 1.98 + iOS target、meson 1.12.1、bison 3.8.2、llvm-as 23.1.4、
  glslangValidator 16.6）。Rust Metal bridge 已编译 PASS（222 MB）。
- **当前唯一硬阻塞 = Mesa 源码的 sparse-checkout 被 zsh 单词拆分搞坏**（30 个目录被拼成 1 个畸形 pattern，
  `src/freedreno` 空白）。修复只是几条命令（见 §6.2），不依赖网络重下（源码已浅克隆到本地）。
- **最后一公里是一条命令**：`python3 UniXNU/scripts/build-adreno-metal-plugin.py --target ios`
  （内部自动串 Turnip + bridge + 链接 + 4K 签名 + 吐 trust.json），再走 §6.5 的 addon/trust/payload/镜像。

---

## 1. 仓库布局与红线（必读）

| 路径 | 角色 | 备注 |
| --- | --- | --- |
| `/Users/diannao/pad7pro-xnu/UniXNU` | **核心仓**（脚本/补丁/数据） | `scripts/`、`patches/`、`drivers/`、`kits/` 都在这里；**线无关**，27/17 共用 |
| `UniXNU-muyu` | **27 线**主 checkout | `main @ 5e99d2b`，`schema_version=2`，目标 24A446 |
| `UniXNU-muyu-17` | **17 线** detached checkout | `e105fa0`，format 1，目标 **21H461（本板）** |
| `UniXNU/out/muyu/tree/kits/muyu/` | 组装树 | 与 `UniXNU-muyu-17/package/` **硬链接**（同 inode），改 17 线 `package/` 自动生效 |
| `UniXNU/.cache/mesa-work` | Mesa pinned 浅克隆 | **当前 sparse-checkout 损坏，需修复（§6.2）** |
| `UniXNU/.cache/gpu-reference/metal2vulkan` | metal2vulkan pinned 仓库 | HEAD=`624bab39…`，tree=`942c9483…`，6369 文件，**已就绪** |
| `UniXNU/build/adreno-metal-bridge/ios/libadreno_metal_bridge.a` | Rust bridge 产物 | 222,607,080 B，**已编译 PASS** |

**🔴 红线（踩过多次）**
1. `UniXNU-muyu`（27 线）与 `UniXNU-muyu-17`（17 线）的 `board.toml` **禁止互相复制**
   （sha `2aed9d8a` vs `9b158cc2`，分线设计）。GPU 改动只落 `-17`。
2. 主 checkout `UniXNU-muyu/package/` 是 **27 线**，不要把它 sync 进 17 线（曾误同步后已恢复）。
   改 ad-hoc 显示/GPU 桥只在 `UniXNU-muyu-17/package/`。
3. `UniXNU-27` / `UniXNU-uke` / 框架仓 `payload.py` **AI 勿动**（别的分支/他的人）。
4. `fw/` 下 **17 线**用 `vendor_boot_nofs1.img`(`c7635cdb`) 进桌面；`vendor_boot_27-uNNN.img` 是 24A446，**不是** 17 线证据。

---

## 2. 已装齐的工具链（复现必用，路径写死在脚本里）

| 工具 | 版本 | 路径 | 装法 |
| --- | --- | --- | --- |
| rustc / cargo | **1.98.0** (`88d9e12ae`) | `~/.cargo/bin` | rustup，`cargo +1.98.0` 写死在脚本 |
| Rust target | `aarch64-apple-ios` | — | `rustup target add aarch64-apple-ios` |
| meson | **1.12.1** | `UniXNU/.cache/mesa-venv/bin/meson` | venv，`--target ios` 调用此路径 |
| bison | **3.8.2** | `UniXNU/.cache/mesa-tools/bin/bison` | ftp.gnu.org 源码编译（非 brew） |
| llvm-as | **LLVM 23.1.4** | `/opt/homebrew/opt/llvm/bin/llvm-as` | `brew install llvm`（bottle 已就绪） |
| glslangValidator | **16.6.0** | `/opt/homebrew/bin/glslangValidator` | `brew install glslang`（4 秒） |
| Xcode iPhoneOS SDK | 27.2（可作 17.0 deployment target） | `xcrun --sdk iphoneos` | 系统已有 |

> ⚠️ 曾误 `brew install meson glslang llvm`：llvm 卡在 ghcr.io bottle 20 分钟 → 已停，单独 `brew install glslang` 即可
> （llvm 23.1.4 本来就够）。**不要再跑整组 brew**。

---

## 3. Pinned 版本清单（构建脚本自动校验，错一个就退出）

| 组件 | Pin | Tree | 状态 |
| --- | --- | --- | --- |
| Mesa | `85fda3b36405bcaea8bffc64aa4f2651dc90770d` | `b72ff1eb2138026ed6aba524770899faabf7d7c8` | 浅克隆 OK，**sparse-checkout 损坏待修** |
| metal2vulkan (translator) | `624bab3918de5b978e1101517abca2b4fa426565` | `942c9483…` | ✅ 完整仓库已就绪 |
| Mesa 补丁 | 17 个（`adreno-compiler-inputs.json` → `patches`） | — | 文件齐全，待 apply |
| Translator 补丁 | 30 个（`drivers/AdrenoMetal/bridge/patches/0001-0030`） | — | 由 `build-adreno-metal-bridge.py` 自动 apply |

- Mesa 30 个 sparse 目录在 `adreno-compiler-inputs.json` → `mesa.sparse_directories`。
- Translator commit 在 `scripts/data/gpu-metal-vulkan-inputs.json` → `translator.commit`。

---

## 4. 网络绕法（复现必看，否则再卡 20 分钟）

- `github.com` **git 协议不通**；`chromium.googlesource.com` 的 Mesa mirror **超时**。
- codeload 直连 ~30 KB/s 且**不支持 Range**，900 s 仅下 21.9 MB → 失败。
- **可用通道**：
  - tarball 下载：`https://gh-proxy.com/https://codeload.github.com/<owner>/<repo>/tar.gz/<sha>` ≈ **1.2 MB/s**。
  - git 代理：`gh-proxy.com` / `ghfast.top` 支持 `ls-remote` 与 `fetch`。
- **Mesa 绝不要全量 clone**（报 `HTTP/2 PROTOCOL_ERROR / early EOF`）。正确做法：
  ```bash
  cd UniXNU/.cache/mesa-work
  git init -q && git remote add origin https://gitlab.freedesktop.org/mesa/mesa.git
  git -c http.version=HTTP/1.1 fetch --filter=blob:none --depth=1 origin 85fda3b36405bcaea8bffc64aa4f2651dc90770d
  ```
  （19 s 拿到 pinned commit；已做过，HEAD 正确。）
- metal2vulkan 仓库重建脚本：`/tmp/rebuild-m2v-git.py`（tarball 解压 + 代理 fetch 真实 commit object
  + `update-ref` + `read-tree`/`write-tree` 校验）。**已成功，无需重跑**。

---

## 5. Tier-1 已上机验证的事实（别再返工）

1. **开机链三处坑已闭环**：
   - §9 ADT 必须含 `PackedRAMDisk`，否则 UEFI 报 `compromised data`。
   - §10 显示驱动注册被 `offline-mobile-graphics-{client,load}=1` 门控；首测为塞进 1000 B 闸删了这俩 ⇒ 背光亮全黑。
     现镜像（tier1c+）已恢复两个显示参数。
   - §11 `source_corecrypto14_bootstrap` 的 `EARLY_BOOT/STARTUP_RANK_FIRST` POST ⇒ 内核极早期挂死全黑无反应。
     修复：给 `prng_random.o` 加 `-U XNU_SOURCE_CORECRYPTO14`
     （`scripts/build-xnu-4k-baseline.sh` crypto 条件块改**无条件** `EXTRA_MAKE_ARGS+=("prng_random.o_CFLAGS_ADD=-UXNU_SOURCE_CORECRYPTO14")`）。
     **⚠️ make 不重编已存在对象**：改完必须删 `build/xnu/.../osfmk/.../prng_random.o` 强制重编，否则 POST 仍在。
2. **当前运行内核** `b2846224`（镜像 `fw/vendor_boot_gpu_tier1e2_probe.img`，`8a0f6ff0`）。
3. **GPU 未接管（设备坐实）**：屏读 `MUYU GPU calls>0 refused>0 o=0`
   ⇒ 桥通、presenter 已建，唯一缺 `gpuClient`（用户态 scanout 客户端）。
   完整缺口链见 `GPU-ONDEVICE.md §15.7`。

---

## 6. 下一步精确命令（接手即跑）

### 6.1 先确认现状
```bash
cd /Users/diannao/pad7pro-xnu
git -C UniXNU/.cache/mesa-work rev-parse HEAD          # 应为 85fda3b36405bcaea8bffc64aa4f2651dc90770d
git -C UniXNU/.cache/mesa-work sparse-checkout list | wc -l   # 当前=1（坏）；修后应=30
ls UniXNU/.cache/mesa-work/src/freedreno/ir3/ir3_disk_cache.c  # 当前 MISSING；修后存在
ls UniXNU/build/adreno-metal-bridge/ios/libadreno_metal_bridge.a  # 222607080 B，已就绪
```

### 6.2 修复 Mesa sparse-checkout（关键，用 Python 列表展开避免 zsh 单词拆分）
```bash
cd /Users/diannao/pad7pro-xnu/UniXNU/.cache/mesa-work
python3 - <<'PY'
import json, subprocess
from pathlib import Path
root = Path('/Users/diannao/pad7pro-xnu/UniXNU')
dirs = json.load(open(root/'scripts/data/adreno-compiler-inputs.json'))['mesa']['sparse_directories']
subprocess.run(['git','sparse-checkout','set',*dirs], check=True)   # *dirs = 30 个独立参数
subprocess.run(['git','-c','http.version=HTTP/1.1','checkout','-f','FETCH_HEAD'], check=True)
PY
# 校验
find . -path ./.git -prune -o -type f -print | wc -l     # 应数千
ls src/freedreno/ir3/ir3_disk_cache.c                    # 应存在
du -sh . | cut -f1                                      # 应 ~数百 MiB–1 GiB
```

### 6.3 按 pin 顺序应用 17 个 Mesa 补丁
```bash
cd /Users/diannao/pad7pro-xnu/UniXNU/.cache/mesa-work
python3 - <<'PY'
import json, subprocess
from pathlib import Path
root = Path('/Users/diannao/pad7pro-xnu/UniXNU')
for p in json.load(open(root/'scripts/data/adreno-compiler-inputs.json'))['patches']:
    f = root/p
    subprocess.run(['git','apply','--check',str(f)], check=True, cwd='.')   # 先 dry-run
    subprocess.run(['git','apply',str(f)], check=True, cwd='.')
    print('applied', p)
PY
# 身份校验（build-turnip-darwin.py 会自动调；这里手动确认）
python3 - <<'PY'
import json,sys; sys.path.insert(0,'/Users/diannao/pad7pro-xnu/UniXNU/scripts')
from mesa_patch_identity import validate_recorded_patches
from pathlib import Path
root=Path('/Users/diannao/pad7pro-xnu/UniXNU')
validate_recorded_patches(root, root/'.cache/mesa-work', json.load(open(root/'scripts/data/adreno-compiler-inputs.json')))
print('PASS: mesa patches identity ok')
PY
```

### 6.4 单命令构建插件（内部串 Turnip + bridge + 链接 + 4K 签名 + 吐 trust.json）
```bash
cd /Users/diannao/pad7pro-xnu
export PATH="$HOME/.cargo/bin:/opt/homebrew/opt/llvm/bin:$PATH"
python3 UniXNU/scripts/build-adreno-metal-plugin.py --target ios
# 成功末行：PASS: Adreno plugin bundle and probe daemon built and 4K-signed, CDHashes <bundle> <daemon> <turnip>
# 产物：UniXNU/build/adreno-metal-plugin/NativeAdrenoMetal.bundle + trust.json（含 3 个 40 位 CDHash）
```
> 该脚本会**自动**调 `build-turnip-darwin.py --target ios`（需 §6.2/§6.3 已就绪）与
> `build-adreno-metal-bridge.py --target ios`（已 PASS，会复用，但脚本仍会跑校验）。
> 若只验证 Turnip：`python3 UniXNU/scripts/build-turnip-darwin.py --target ios`。

### 6.5 拿真实 addon / owner-trust / payload / 镜像（§16.5 最后一公里）
拿到 `NativeAdrenoMetal.bundle` 后，**不再改内核/ADT**：
```bash
ADRENO_PLUGIN_DIR=UniXNU/build/adreno-metal-plugin/NativeAdrenoMetal.bundle \
  python3 UniXNU/scripts/make-gpu-addon.py --publish --no-daemon
# 用 trust.json 里的 2 个 20B CDHash（bundle + turnip）追进 21H461 原 trust：
python3 UniXNU/scripts/trust-append-module.py \
  fw/trust-21H461/21H461-original-trust.bundle <out-owner-trust.bundle> <cdhash1> <cdhash2>
# payload（--owner-trust-cache 替换 --trust-cache 且跳过 board allow-list，不能两个同传）：
python3 UniXNU/unixnu payload ... \
  --owner-trust-cache <out-owner-trust.bundle> --gpu-addon <out-addon> \
  --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-addon=1 --kernel-arg sheng-gpu-metal=1
python3 UniXNU/scripts/patch-vendor-boot.py --base nofs1 <payload>
```
> ⚠️ **占位 addon 不可刷**（假插件 AMFI 会拒）。必须先有 §6.4 真实 bundle。
> ⚠️ owner trust 用 21H461 原 trust（`fw/trust-21H461/21H461-original-trust.bundle`，`7416ca4f…`），
> **别混 27 线的 `trust-24A446`**。

---

## 7. 刷机与上机闸门（17 线三件套）

```bash
# 17 线 UEFI（独立副本，别用当前 out/muyu/boot/boot.img，那是 27 线 24A446）
export MUYU_UEFI=fw/boot-17line-043f7120.img
# 基线进桌面（验证用，c7635cdb）
bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_nofs1.img
# GPU 镜像（回退/对照）
bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier1e2_probe.img   # 当前已上机运行（8a0f6ff0）
bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier1d_nopost.img   # 干净进桌面 GPU 内核（4f6467bd）
```
- 三件套：`fw/boot-17line-043f7120.img`(UEFI) + `fw/vendor_boot_nofs1.img`(base) + `userdata-root.sparse`(`4c90d7a7`)。
- 设备需停在 fastboot（`fastboot devices` 非空）；不在则用户长按电源/音量下+电源回到 fastboot。
- 面板是 17 线唯一输出通道（USB CDC ACM 与 Linux pstore 已拆，`PREVIOUS XNU LOG` 不跨复位）。

---

## 8. 已知坑速查（踩过的都在这）

| 坑 | 现象 | 解决 |
| --- | --- | --- |
| Mesa sparse-checkout 单词拆分 | `src/freedreno` 空白，ir3 缺失 | §6.2 Python 列表展开 |
| `prng_random.o` 不重编 | POST 修复"不生效" | 删 `.o` 强制重编 |
| cmdline >1000 B | `build payload` 硬闸失败 | 去无消费者参数（如 `sheng-gpu-dcvs=1`）换 `muyu-dump-at=20` |
| ADT 缺 PackedRAMDisk | UEFI `compromised data` | `make-muyu-adt.py` 兼容推断 PackedRAMDisk |
| `--trust-cache` vs `--owner-trust-cache` | 后者替换前者、跳过白名单 | 二选一，别同传 |
| 27/17 线 `board.toml` 互拷 | 启动链错配 | 禁止；改动只落 `-17` |
| `vendor_boot_27-uNNN` 当 17 线证据 | 误判 | 17 线用 `nofs1`(`c7635cdb`) |
| gh-proxy 不用 | github 卡死 | §4 网络绕法 |
| brew 整组装 llvm | ghcr.io 卡 20 min | 只 `brew install glslang` |

---

## 9. 任务状态

| # | 内容 | 状态 |
| --- | --- | --- |
| #12 | GPU 运行时验证 / Tier-2 接管 | in_progress（设备证明缺用户态客户端） |
| #14 | Tier-2 addon 接线（离线） | done（占位验证全绿，待真实 bundle） |
| #15 | 编译器工具链 | in_progress（工具齐、bridge PASS；Mesa checkout 待修 §6.2） |
| #16 | 真实插件 addon/trust/payload/镜像 | pending（依赖 §6.4 产物） |

---

## 10. 接手检查清单（checklist）

- [ ] `UniXNU/.cache/mesa-work` sparse-checkout 已修（30 目录，`ir3_disk_cache.c` 存在）
- [ ] 17 个 Mesa 补丁已 apply 且 `validate_recorded_patches` PASS
- [ ] `python3 UniXNU/scripts/build-adreno-metal-plugin.py --target ios` 全绿，得 `NativeAdrenoMetal.bundle` + `trust.json`
- [ ] §6.5 真实 addon + owner-trust + payload + 镜像产出
- [ ] 刷 `fw/vendor_boot_nofs1.img` 先确认基线进桌面
- [ ] 刷新 GPU 镜像，面板读 `MUYU GPU` 行：`o>0 c>0` 才是 GPU 真接管
- [ ] 失败回退 `nofs1` + `MUYU_UEFI=fw/boot-17line-043f7120.img`
- [ ] 全程不碰 `UniXNU-muyu/board.toml`、`UniXNU-27`、`UniXNU-uke`、框架仓 `payload.py`

---

*配套文档：`GPU-ONDEVICE.md`（上机手册，含 §0.1 仓库边界 / §9–§16 复盘 / 判读表）、
`GPU-UKE-ALIGNMENT.md`（与 uke 27 线对齐差异）。本文件为 2026-11-11 续 19 之后、Mesa checkout 修复前的快照。*

---

## 11. 续作结果（2026-11-11 17:0x，离线链路全部打通）

§6.2–§6.5 已全部执行完，**唯一剩余的是上机刷入与面板判读**（设备当时停在 fastboot `a20248f7`）。

### 11.1 完成项与关键产物

| 项 | 结果 |
| --- | --- |
| Mesa sparse-checkout | 用 `mesa/mesa` GitHub 镜像经 `gh-proxy.com` 下 pinned tarball（115 MB / 35 s / 3.4 MB/s），把 12742 个 blob 按 git OID 逐一校验后写回本地对象库；cone 模式 25 个目录（30 个中 5 个被父目录合并），3868 文件 / 299 MB，`ir3_disk_cache.c` 存在，全程离线 |
| 17 个 Mesa 补丁 | 全部 apply，`validate_recorded_patches` PASS |
| Turnip iOS17 | `PASS ... (17921872 bytes)`，`build/mesa-tu-ios/.../libvulkan_freedreno.dylib` |
| Rust bridge | 复用 222,607,080 B，`PASS ... translator 624bab3918de` |
| 插件 | `PASS: ... 4K-signed`，`build/adreno-metal-plugin/NativeAdrenoMetal.bundle` + `trust.json` |
| 真实 addon | `fw/gpuaddon-17.bin` 12,108,872 B / 6 文件 / `NGPUADD2`，sha `9b3d8372…` |
| owner trust | `fw/trust-21H461/owner-trust-17.bundle` 3 模块（21H461 原 2 模块逐字节保留），sha `c7364523…` |
| payload | `UniXNU/out/muyu/payload/payload.img` 72 MiB，cmdline **985 B / 48 参**，ADT 13,992 B 含 `PackedRAMDisk/TrustCache/GpuAddon`，内核仍 `b2846224…` |
| 镜像 | `fw/vendor_boot_gpu_tier2.img` 77,393,920 B，sha **`09039a1d…`**，ramdisk@0x1000 与 payload.img 逐字节一致，96 MiB 分区余量 22.2 MiB |

三个真实 CDHash（bundle / daemon / turnip）：
`8d405db4…` / `0d2b839a…` / `66bbfc6c…`；owner trust 追加的是前两个中的 bundle 与 turnip。

### 11.2 本次踩坑与改动（复现必看）

1. **GitHub 镜像仓名是 `mesa/mesa`**，不是 `Mesa3D/mesa`（后者 404）。`gitlab.freedesktop.org` 的 git 通道很慢，别再走 lazy fetch。
2. codeload tarball 有 31 个 `.csv` 被 `.gitattributes` 转成 CRLF、10 个符号链接；写 blob 前须还原 LF / 用 `os.readlink` 取目标，否则 OID 不匹配。
3. `mesa-venv` 缺 `mako / packaging / PyYAML`：`pip install mako packaging PyYAML` 后 meson 配置才过。
4. **`patches/mesa/turnip-frame-diagnostics.patch` 缺 `new file mode 100644`**（git 2.54 把 `/dev/null` 当源文件名，正反 apply 都失败）→ 已补该行，未改任何补丁内容。
5. **缺 `glslc`**：`brew install shaderc` 卡 ghcr.io（4 min 停掉）；改用 `/tmp/glslc-shim/glslc`（`glslangValidator -V --target-env vulkan1.1`）包装，5 个 smoke shader 全部编译并通过 `spirv-val`。
6. `-Werror` 报 `writeTask` / `patchFramebufferBranch` 在 arm64/iOS17 未使用 → 在 `NativeAdrenoMetalPlugin.mm` 给这三个 arm64e-only 辅助函数加 `#if defined(__arm64e__)`。
7. **插件用 `--no-replay`**：replay 是可选诊断，需要额外的 host Mesa + hostsim 全量构建；`__has_include("TurnipReplay.h")` 保证不影响主路径。
8. **`ADRENO_PLUGIN_DIR` 必须指向构建目录**（`build/adreno-metal-plugin`），不是 bundle 本身；§6.5 原文路径是笔误。
9. **`size_mib` 60 → 72**（`UniXNU-muyu-17/board.toml`）：真实 addon 让 FAT 内容到 63.2 MiB，64 MiB 仍因 FAT32 元数据 `mcopy: Disk full`。`patch-vendor-boot.py` 拿 `len(base)` 当分区上限，故按 `IPADOS27-MIGRATION.md §6.27` 的既有做法把 `vendor_boot_nofs1.img` 零填充到 96 MiB 再打包（header/DTB/table/bootconfig 一字节未变）。ADT 的 `PackedRAMDisk` 是 16 B 占位、不写死尺寸，ramdisk 大小来自 vendor_boot 头。
10. `build payload` 前已把已知可启动 Tier-1e2 payload 备份到 `/tmp/muyu-tier1e2-payload-backup-1794387738/`。

### 11.3 上机记录（2026-11-11 17:11–17:24）

```bash
cd /Users/diannao/pad7pro-xnu
export MUYU_UEFI=fw/boot-17line-043f7120.img
bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_nofs1.img        # 基线
bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier2.img    # Tier-2
# 失败回退：
bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_nofs1.img
```

| 时间 (UTC) | 动作 | 结果 |
| --- | --- | --- |
| 09:11:50 | 刷 `nofs1` + 引导 17 线 UEFI | `rc=0`，slot=a `unbootable=no`，**用户确认进桌面/锁屏** |
| 09:15:15 | 刷 `vendor_boot_gpu_tier2.img` 73.81 MiB / ramdisk 72.00 MiB + 引导 | `rc=0`；110 s 后设备未回落 fastboot（仍在 XNU） |
| 09:21:55 | 用户要求重来一次，同镜像再刷再引导 | `rc=0`；150 s 后仍未回落 fastboot |

**结论**：72 MiB ramdisk 在 17 线 UEFI 上**可正常引导、不会立刻回落 fastboot**（此前担心的尺寸失配没有出现）。
**唯一未闭环**：面板 `MUYU GPU` 行的读数未取到（用户没看到）。需再读一次：`o>0 c>0` = GPU 真接管；
仍是 `o=0 / refused>0` = `gpuClient` 未置位，下一步查 Metal 插件进程是否被 launchd/AMFI 加载。

> 面板是 17 线唯一输出通道；USB CDC ACM 与 pstore 已拆。设备当前应停在 XNU（非 fastboot），
> 需要再次操作时长按电源/音量下+电源回 fastboot。

### 11.4 面板「进系统就看不到」根因与加强探针合体镜像（2026-11-11 17:30）

**现象**：Tier-2 两次引导都进了系统，但用户完全看不到 `MUYU GPU` 行。用户提示「之前不是有加强版探测镜像吗」。

**根因（离线坐实，不是启动失败）**：加强版 `fw/vendor_boot_gpu_tier1e2_probe.img`（`8a0f6ff0…`）
靠两个 cmdline 参数让诊断页持续可见（§15.3）：`offline-legacy-console-quiet=0` 关掉
`LegacyDisplayProbe.cpp:259` 在首帧成功后 `vc_set_text_quiet(1)` 的 park，`muyu-dump-at=60`
把 dump 推到合成器出帧之后。§16.5 的「最后一公里」命令只挂了 3 个 GPU 参数，重建出的
Tier-2 cmdline 是 **985 B / 48 参**，正好把这两个参数挤掉（换成 `sheng-gpu-addon=1` /
`sheng-gpu-metal=1`）⇒ 第一帧到达 scanout 即 park 文本控制台，诊断页被桌面覆盖。
**内核没变**：`kernel_sha256` 与加强版同为 `b2846224…`（FAT 内 `kernel.macho` 含
`MUYU GPU` / `MUYU OFFER` / `MUYU SWAP` / `nolink`），所以不需要 `build all`，只补 boot-arg。

**修复**：`UniXNU-muyu-17/board.toml` 的 `[payload] kernel_args` 重新启用
`muyu-dump-at=60` 与 `offline-legacy-console-quiet=0`；按 §6 的字节算法，注释掉两个纯
boot-determinism 心跳 `muyu-data-hb=20`(16 B) / `muyu-data-delay=100`(20 B) 腾 36 B，
最终 **996 B / 48 参**（= 已上机验证的 Tier-1e2 同长，≤1000 B 闸）。勿动
`offline-mobile-graphics-{client,load}=1`：§10 实测删了会背光亮全黑。

**产物**：`fw/vendor_boot_gpu_tier2_probe.img`，77,393,920 B，sha `c9ebb0a4276ce8daf…`。
ramdisk@0x1000 与 `payload.img` 逐字节一致；`gpu_addon_sha256` / `trust_cache_sha256`
与 `fw/gpuaddon-17.bin`、`fw/trust-21H461/owner-trust-17.bundle` 逐一匹配；96 MiB 分区余 22.2 MiB。
旧的无探针 Tier-2 payload 备份在 `/tmp/muyu-tier2-plain-backup-1794389394/`，
改前的 board.toml 在 `/tmp/muyu-board-t2plain.toml`。

```bash
cd /Users/diannao/pad7pro-xnu
export MUYU_UEFI=fw/boot-17line-043f7120.img
bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier2_probe.img
# 纯加强版对照（无 addon/trust，必为 o=0）：
bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier1e2_probe.img
# 回退：
bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_nofs1.img
```

按 §15.5 判读：`o>0 c>0` = GPU 真接管；`calls>0 refused>0 o=0` = 桥通但 `gpuClient` 未置位
（查 Metal 插件进程是否被 launchd/AMFI 加载）；`nolink` = 跨 archive 绑定失败。
因为关掉了 park，`MUYU LIVE` / `MUYU GPU` 会每 5 s 刷新并持续可见，dump 落在 t+60 s。

### 11.5 上机：t+60 dump 卡住，改 t+20 后成功取到面板读数（17:33–17:45）

| 时间 (UTC) | 镜像 | 结果 |
| --- | --- | --- |
| 09:33:27 | `vendor_boot_gpu_tier2_probe.img`（`c9ebb0a4…`，`muyu-dump-at=60`） | 用户看到屏幕「每一行都是 loading 开头」，09:38 设备回落 fastboot ⇒ **启动失败** |
| 09:43:48 | `vendor_boot_gpu_tier2_probe20.img`（`ef3dbc14…`，`muyu-dump-at=20`） | 95 s 未回落 fastboot；**用户读到 `MUYU GPU calls=3 refused=3 o=0 c=0 f=0 fb=0 tc=3`** |

离线取证（失败镜像 vs 已能进桌面的 plain Tier-2）：FAT 里 `kernel.macho` /
`kernel.macho.gpuaddon` / `kernel.macho.trustcache` / `scratch-data.img` **逐字节相同**，
ADT 只差 64 B `random-seed`（每次构建随机）；唯一实质差异是 `EFI/UniXNU/unixnu.toml`
的 `args` 行 994 → 1005 B。而 1005 B 与已上机验证的 `tier1e2_probe` 完全同长 ⇒
**不是 cmdline 长度/格式问题**。嫌疑收敛到 `muyu-dump-at=60`：t+60 s 的 dump 会
`muyu_console_wake()` 强抢文本控制台，正撞上 Tier-2 的 GPU scanout 客户端；换成
Tier-1e 已验证可启动的 `=20` 后一次通过（`quiet=0` 让行持续刷新，早快照不影响读数）。
次要嫌疑是同一版里删掉的 `muyu-data-delay=100`（默认 20 s，会让 mode-2 数据探针提前
80 s 去 rename keybags），保留在本轮配置中，未单独验证。

**判读**（§15.5）：`calls>0 refused>0 o=0 c=0`、`tc=3` = hand-off 桥通、presenter 在、帧在走，
但 `gpuClient=false` ⇒ 仍 CPU copy。与 §15.7 的 Tier-1e2 结论同类，说明**真实 addon +
owner trust + Metal 插件都进包了也没让 GPU 接管**。

### 11.6 `gpuClient=false` 的两个具体缺口（离线定位，修复镜像已就绪、未上机）

1. **`--no-daemon` 让 addon 缺了启动链**。§16.5 用的
   `make-gpu-addon.py --publish --no-daemon` 只打 6 个文件（bundle 5 + `.publish`），
   而 §16.1 明确写：内核 `native_gpu_addon_install()` 会把
   `/private/var/ngpu/daemons` 挂到 `/System/Library/LaunchDaemons`。
   原厂 21H461 的 launchd 里没有任何 job 去拉起插件链 ⇒ 插件进程从未运行。
   构建目录里现成有 `bin/adrenoprobed`（arm64，109 KB）和 3 个
   `RunAtLoad=true` 的 plist（`com.apple.bluetoothd` / `com.apple.iomfb_fdr_loader` /
   `org.blu-sharky.adrenoprobed`），`--no-daemon` 是 27 线的做法（27 线已有 launchd jobs）。
2. **owner trust 漏了第三个 CDHash**。`trust.json` 有 3 个 hash，§16.5 只追加
   bundle + turnip 两个；`adrenoprobed` 的 `daemon_cdhash=0d2b839a…` 不在 trust cache 里，
   即使把 daemon 放进 addon，AMFI 也会拒掉它。

**修复产物（离线全绿，随后已上机，见 §11.7）**：

| 产物 | 值 |
| --- | --- |
| `fw/gpuaddon-17-full.bin` | 10 文件 / 12,132,608 B（bundle + `.publish` + `bin/adrenoprobed` + 3 plist） |
| `fw/trust-21H461/owner-trust-17-full.bundle` | 3 模块，新模块含 **3 个** CDHash；原 2 模块逐字节保留 |
| `fw/vendor_boot_gpu_tier2_full.img` | 77,393,920 B，sha `9ce15fafe581f0b0…`；cmdline 996 B / 48 参（保持 `dump-at=20` + `quiet=0`），内核仍 `b2846224…` |

ramdisk@0x1000 逐字节一致、`gpu_addon_sha256` / `trust_cache_sha256` 与新文件匹配、
FAT 余 8.5 MB、96 MiB 分区余 22.2 MiB。当前可启动镜像的 payload 备份在
`/tmp/muyu-tier2-probe20-backup-1794390652/`，镜像备份 `/tmp/vendor_boot_gpu_tier2_probe20-working.img`。

```bash
cd /Users/diannao/pad7pro-xnu
export MUYU_UEFI=fw/boot-17line-043f7120.img
bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier2_full.img
# 判读：o>0 c>0 = GPU 真接管；仍 refused>0 o=0 = 插件链仍未起来（查 launchd / AMFI）
# 回退：bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_tier2_probe20.img
```

### 11.7 完整镜像上机：仍 `o=0`，缺口收窄到 backboardd 是否加载插件（17:53–17:56）

| 时间 (UTC) | 动作 | 结果 |
| --- | --- | --- |
| 09:53:51 | 刷 `fw/vendor_boot_gpu_tier2_full.img`（`9ce15faf…`）+ 引导 17 线 UEFI | `rc=0`；110 s 后未回落 fastboot |
| ~09:56 | 用户读屏 `MUYU GPU` 行 | **仍是 `o=0 / refused>0`** |

**判读**：补齐 `bin/adrenoprobed` + 3 个 launchd plist + 第三个 CDHash 后
`gpuClient` 依然 false。说明 `adrenoprobed` 只是 probe daemon，**不是** §15.7 里置位
`gpuClient` 的 scanout 客户端；真正要发生的是 backboardd 通过
`/System/Library/Extensions` 的只读 union 发现 `NativeAdrenoMetal.bundle/Info.plist`
里的 `MetalPluginName`，加载 `NativeAdrenoMetalPlugin.mm`，再由
`NativeMetalBridgePhase.h:120` 的 `gpuscanout::Client` 调 `AGXDeviceUserClient::runTimed()`
→ `native_gop_handoff_call()` → `waitSwap()` 置位 `gpuClient`。需要确认的是
**backboardd 有没有碰过这个 bundle**，而不是 daemon 在不在。

**现成观测（在内核里，尚未启用）**：`package/GpuPowerProbe.cpp` 已有 exec/vnode
observer，正好回答这个问题 —— `gicRow()` 的 `L e<exec 授权>/<adrenoprobed 访问>
p<三个 plist 被读次数> b<插件二进制>/<原 AGXMetalA10> B<bundle 目录>`，另有
`gExecDaemon` / `gDaemonAlive` / `gExecNames[6]`。只读解包内核里
`MUYU GPU POWER` 41 处、`adrenoprobed` 1 处、`org.blu-sharky.adrenoprobed` 1 处，
观测代码确实编进去了。但 `registerExecObserver()` 只在 `probeThread()` 里调用，
而 `probeThread` 由 `sheng_gpu_power_probe()` 的 `sheng-gpu-power=<非零>` 门控，
会连带跑 `gpb_run()` / `gmuContext()` / `gmuPrerequisites()` 整套电源时钟探测链
（§13.3 曾判它无效），且 cmdline 已 996 B，加参数需再腾 18 B。
`muyu_proc_census()`（每 60 s 一行 `PROC n=… launchd=… backboardd=…`）只看 4 个进程，
不含 `adrenoprobed`，不足以判定。

**下一步选项**（都需再改镜像 + 上机，尚未执行）：① 加 `sheng-gpu-power` 启用现成
observer，直接看 `p…/b…/B…` 计数；② 更轻的做法是在 `BravearkLastLog.c` 的
`MUYU GPU` 行旁直接暴露 vnode/exec 计数（要重编内核）；③ 先收尾，保留当前
可启动且面板可读的 `vendor_boot_gpu_tier2_probe20.img` / `…_full.img` 作证据。

> ⚠️ **本节判读已由 §11.8 更正**：`MUYU GPU POWER` 41 处来自 muyu 自己的
> `MuyuGpuPowerProbe.cpp`，**不是** sheng 版 `GpuPowerProbe.cpp` 的 exec/vnode
> observer；后者根本没编进 17 线内核。缺口也不在「backboardd 有没有碰过 bundle」，
> 而在更靠前的 IORegistry：内核从未发布 `MetalPluginName=NativeAdrenoMetal` 的
> accelerator，也从未发布可打开的 GPU user client。三个选项里 ①② 因此都是空转。

### 11.8 决定性根因：17 线内核没有 Metal accelerator，`gpuClient` 恒为 false（18:0x，纯离线）

§11.6/§11.7 补的 addon / owner trust / launchd daemon 都是**必要但不充分**。
本轮只读解包当前运行内核（`kernel.macho`，51 MB）+ `nm -m` 全符号表
（70,463 行），把链路从头到尾核了一遍，结论是：**17 线内核里连 Metal discovery
的入口都没有，用户态 scanout client 连设备都打不开**，`gpuClient` 在当前内核下
不可能被置位。这不是 boot-arg、addon 或 trust cache 能修的。

**证据表（当前运行内核实测）**

| 检查项 | 结果 | 含义 |
| --- | --- | --- |
| `sheng-gpu-metal` 字符串 | **0** | 该 boot-arg 空转，无消费者 |
| `publishMetal` / `gpb_run` / `metalRows` / `gicRow` 符号 | **0** | sheng 版 `GpuPowerProbe.cpp` 完全没编进来 |
| `AGXAccelerator` / `native_adreno_accelerator_*` | **0** | 发布 `MetalPluginName` 的 IOAcceleratorES 子类不在 |
| `MetalPluginClassName` 字符串 | **0** | 同上，discovery 的两个属性键都没被设置 |
| `_native_adreno_publish` 定义 | 存在（3） | 但 `g.published=true` 只在该函数里写，而它**无调用者** |
| `NativeGOPPresenter` / `AGXDeviceUserClient` 符号 | 72 | 桥与 user client 类**在**，所以 `waitSwap` 代码路径是好的 |
| `board.toml [kernel] archives` 含 `build-adreno740-accelerator.py` | **0** | §7 早指出，至今未串 |
| `muyu-userland_defconfig`（board.toml 实际选用） | `CONFIG_XNU_NATIVE_ADRENO=y` / `XNU_BUILD_ARCHIVES=y` | **驱动 archive 确实已链接**（`native_adreno_publish` 等符号在），注释写明 Adreno 735 固件已采集。⚠️ `muyu-full_defconfig` 才是 `is not set`，那是另一份、非本次使用的配置 |

**根因链（逐环可复核）**

1. `scripts/build-native-legacy-display.py:263` 注释白纸黑字：
   「NATIVE_ADRENO is off, so the sheng GpuPowerProbe's Adreno and GPU-addon
   imports would be undefined; **omit it from this archive**」。17 线编进内核的是
   muyu 自己的 `MuyuGpuPowerProbe.cpp`（只读 GCC/GPUCC/SMMU 观测，无 Adreno 依赖，
   由 `muyu-gpu-power=<n>` 门控）。§11.7 把它的 41 条 `MUYU GPU POWER` 日志误当成了
   sheng observer 的证据 —— 实际 `gicRow()` / `registerExecObserver()` 符号 0 命中。
2. `publishMetal()`（`GpuPowerProbe.cpp:1038`）是唯一 `setProperty("MetalPluginName",
   "NativeAdrenoMetal")` + `MetalPluginClassName` + `registerService()` 的地方，
   由 `sheng-gpu-metal=1` 触发；它和 `native_adreno_publish()` 的**唯一调用点**
   （`:1011`）同在这个未编入的文件里。
3. `native_adreno_publish()` 虽在内核中定义，但没有调用者 ⇒ `g.published` 恒 false。
   `NativeAdrenoPublish.cpp:901` 的 `native_adreno_new_user_client()` 第一行就是
   `if(!handler || !g.published) return kIOReturnNotReady;` ⇒ **任何用户态进程都拿不到
   `AGXDeviceUserClient`**，`runTimed()` → `native_gop_handoff_call()` → `waitSwap()`
   整条链无从发起，`gpuClient` 恒 false。
4. Metal discovery 侧：`audit-target-metal-discovery.py:226` 证实原厂 personality
   `agx['MetalPluginName'] == 'AGXMetalA10'`，所以 backboardd 只加载
   `/System/Library/Extensions/AGXMetalA10.bundle`。我们 union 上去的
   `NativeAdrenoMetal.bundle` **没有任何 IORegistry 属性指向它**——
   §11.7 说「要确认 backboardd 有没有碰过这个 bundle」，答案是连找都不会去找。
5. staged `AGXAccelerator` 其实离线已构建好（`libAdreno740Accelerator.late.a`
   213 KB，sha `715d4db0…`，`original_instructions_modified=false`），但
   `build-adreno740-accelerator.py:133` 注入的是 `ROOT/build/adreno740/libAdreno740.a`，
   而 17 线实际 archive 在 `ROOT/build/native-adreno/libAdreno740.a`——
   **`build/adreno740/` 目录根本不存在**，`if native.exists()` 为假，静默跳过。
   实测 `libAdreno740.a` 里 `native_adreno_accelerator_*` 符号 0 命中。这是一个额外的
   路径不一致 bug（27 线用 `build/adreno740/`）。

**因此**：`fw/vendor_boot_gpu_tier2_full.img` 上机后 `o=0` 不是启动链缺文件，
而是 17 线内核虽链接了 Adreno 驱动 archive（`NATIVE_ADRENO=y`），却
**没串 staged accelerator、没给 `publishMetal()` 与 `native_adreno_publish()` 任何调用者**，
注入路径还指错目录。§13.6 的「第 2 步接用户态 scanout client」真实范围是
**重编内核 + 接通发布链**（`AGXAccelerator` 入 archive + 一个已编入的发布调用点 +
`native_adreno_publish()` 的 VM/submit/ring/clock 实参），远超加几个 boot-arg；
但比「打开整套 `NATIVE_ADRENO`」轻，因为驱动 archive 本来就在链接清单里。

**修复路径（尚未执行，需重编内核，风险高）**

1. `board.toml [kernel] archives` 增加 `build-adreno740-accelerator.py`（§7 已实跑
   PASS，预置 ABI `ioaccelerator-base-abi.json` metaclass `0xfffffff006aeb688` 已 seed，
   与 21H461 吻合）；同时修注入路径为 `build/native-adreno/libAdreno740.a`。
2. 给 `publishMetal()`（或等价的最小发布者）一个**已编入内核**的调用点和 boot-arg
   消费者；`native_adreno_publish()` 同理要有 VM/submit/ring/clock 实参。
3. ~~`muyu-full_defconfig` 打开 `CONFIG_XNU_NATIVE_ADRENO=y`~~ **不需要**：
   本次实际使用的 `muyu-userland_defconfig:68` 已经是 `y`，且 `XNU_BUILD_ARCHIVES=y`，
   `build-xnu-4k-baseline.sh:668` 的 `-force_load libAdreno740.a` 分支一直在生效
   （内核里 `native_adreno_publish` 等符号就是证据）。早前「配置关了 NATIVE_ADRENO」
   是误看了 `muyu-full_defconfig`（未被 board.toml 选用，其注释说的 A732 固件未采集
   也是旧信息；userland 版注释已改为 Adreno 735 固件已采集）。
   ⚠️ 仍需警惕：§13.3 判过 `sheng-gpu-power=9` 在 17 线是空桩，
   即 `GpuPowerProbe.cpp` 的 GMU 电源探测链没有消费者，不能指望它来触发发布。
4. 完整验证链 `check-accelerator-abi.py` 需要 3 个 QEMU gdb-stop 观测
   （positive / missing-base / default-off），`runtime_qualified` 目前是 `false`。

**本机当前状态**：设备跑着 `vendor_boot_gpu_tier2_full.img`（`9ce15faf…`），
可启动、面板持续可读、`o=0`；本轮**未改任何源码、未重编内核、未刷机**，
仅做只读取证。已知可启动 payload 备份 `/tmp/muyu-tier2-probe20-backup-1794390652/`。
红线未碰（27 线 `board.toml`、`UniXNU-27`、`UniXNU-uke`、框架仓 `payload.py`）。

### 11.9 离线可行性验证：`AGXAccelerator` 能链入 17 线内核，但运行时发布链仍缺（18:2x–18:3x，纯离线）

用户选择「先做离线可行性验证」。目标只有两个：**修注入路径**、**验证内核能编成**。
红线：不刷机、不动设备、不重建 / 不覆盖 `payload.img`、不把验证内核打进 `vendor_boot`。

**改动（3 处，全部可逆）**

| # | 文件 | 改动 |
| --- | --- | --- |
| 1 | `UniXNU/scripts/build-adreno740-accelerator.py` | 注入路径由单一 `build/adreno740/libAdreno740.a` 改为遍历 `build/native-adreno/` + `build/adreno740/`，注入所有存在的 archive；新增 `injected_archives` 报告字段和 `injected into:` 打印。原版在 17 线因目录不存在而**静默跳过** |
| 2 | `UniXNU/out/muyu/tree/scripts/build-adreno740-accelerator.py` | 同步修复版副本。`tree.py::_copy_scripts()` 每次 build 会按内容从框架仓覆盖，所以源脚本必须一起改，否则验证结果无法复现 |
| 3 | `UniXNU-muyu-17/board.toml` `[kernel] archives` | 在 `build-arm-driver.py Adreno740` **之后**插入 `build-adreno740-accelerator.py`（顺序要求：必须先有新鲜的 `libAdreno740.a` 才能注入） |

**验证结果（全部 PASS）**

```text
PASS: staged AGXAccelerator builds against the audited original IOAcceleratorES imports
injected into: build/native-adreno/libAdreno740.a
ACCEL_EXIT=0
== kernel: archive-22-build-adreno740-accelerator
== kernel: .../out/muyu/kernel/kernel.macho sha256 469cf7a26290c3e6cd5ce90e58ebacc7df46c3451a10ee09f89ce627d61a26c6
KERNEL_PROBE_EXIT=0
```

* `libAdreno740.a` 里出现 `__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp` /
  `native_adreno_accelerator_allocate` / `native_adreno_accelerator_instance_size`。
* 重复定义检查：注入前 3 个、注入后 3 个，**本次新增 0 个**，链接面没有被污染。
* `payload.img` 全程未变：`c00bef5cb19844584a56…`（72 MiB）。设备未刷机。
* 备份 `/tmp/muyu-accel-probe-1794392724/`：`board.toml`、两份脚本、原
  `kernel.macho`、注入前 `libAdreno740.a`、`payload/`，以及验证内核
  `kernel-with-accelerator.macho`（`469cf7a2…`）。

**新内核符号实测（`kernel-with-accelerator.macho`）**

| 检查项 | 结果 |
| --- | --- |
| `AGXAccelerator` 系列符号 | **25**（含 metaclass、`newUserClient`、3 个 `copyProperty`） |
| `native_adreno_accelerator_allocate` / `instance_size` | 存在 |
| `__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp` | 存在（`0xfffffff007a014c0`，external） |
| `publishMetal` | **0** |
| `native_adreno_publish` | 有定义，**仍无调用者** |
| `native_adreno_accelerator_static_init` 名字 | 0（脚本把符号 globalize 成 `__GLOBAL__sub_I_...`，发布器要用 `__asm__` 别名引用） |

**关键结论：类进了内核 ≠ 服务会发布。** `build-adreno740-accelerator.py:117` 用
`llvm-objcopy --remove-section=__DATA,__mod_init_func --globalize-symbol=...`
把静态初始化器**摘出** `__mod_init_func`、只导出成一个普通函数；17 线内核里
没有任何代码调用它 ⇒ metaclass 未注册 ⇒ 不会有人 `new AGXAccelerator`、
不会 `setProperty("MetalPluginName", ...)`、不会 `registerService()`。
IORegistry 里依旧没有 `MetalPluginName=NativeAdrenoMetal`，`gpuClient` 仍为 false。
这一步只证明「staged 类可以无冲突地链进 17 线内核」，**没有**改变运行时行为。

**下一阶段可行性（本轮新增取证：发布器依赖在 17 线内核里全部存在）**

| 依赖 | 验证内核中的符号 |
| --- | --- |
| 加载原厂 GPU kext | `_OSKextLoadKextWithIdentifier @ 0xfffffff0078cc17c`；`com.apple.iokit.IOGPUFamily` 字符串 3 处 |
| metaclass 注册 | `__ZN11OSMetaClass10preModLoadEPKc`、`__ZN11OSMetaClass11postModLoadEPv`、`__ZN11OSMetaClass20getMetaClassWithNameEPK8OSSymbol` |
| 挂平台 / kmod 名 | `__ZN9IOService11getPlatformEv`、`_g_kernel_kmod_info` |
| GPU context 实参 | `NativeAdrenoGpuContextVm` / `NativeAdrenoGpuContextBringup` / `NativeAdrenoGpuContextBuffers` |

⇒ 缺的不是符号或 ABI，而是**一个已编入内核、被 boot-arg 门控的发布函数**。
17 线 `build-native-legacy-display.py` 实际编译的是 muyu 自己的
`MuyuGpuPowerProbe.cpp`（由 `muyu-gpu-power=<n>` 门控），它天然是承载点。

**发布链是两段，第 2 段才是重头**

1. **注册 accelerator**（较轻，不依赖 VM）：`GpuPowerProbe.cpp:1038 publishMetal()` 是现成模板 ——
   `OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily")` → 取 `IOAcceleratorES`
   父类并校验 `getClassSize()==136` → `preModLoad()` → `native_adreno_accelerator_static_init()`
   → `postModLoad()` → `allocate()/init()` → `setProperty("MetalPluginName","NativeAdrenoMetal")`
   + `MetalPluginClassName` → `attach(IOService::getPlatform())` → `registerService()`。
2. **置 `g.published=true`**（重）：`NativeAdrenoPublish.cpp:1187` 第一行即要求
   `vm && submit && ring && clock && clock->now_us` 全非空，随后跑
   `NativeAdrenoGpuContextObject()` / `enableSwitching()` / `submit->setTableSwitching()` /
   `checkCpSwitch()`。实参来自 sheng 版 `publishAndWrapperTest()`（`:1002–1011`）：
   `NativeAdrenoGpuContextVm()`、`gSubmit.attach(vm,&gRing)`、`gTimer.attach(...)`、
   `gDeviceInfo` —— **这套代码在 17 线未编入的 `GpuPowerProbe.cpp` 里，需要移植**。

只做第 1 段能让 backboardd 找到 `NativeAdrenoMetal.bundle`，但
`AGXAccelerator::newUserClient()` → `native_adreno_new_user_client()`
（`NativeAdrenoPublish.cpp:901`）仍因 `!g.published` 返回 `kIOReturnNotReady`，
Metal device 照样打不开。**两段都做完才可能让 `gpuClient` 置位。**

**因此**：§11.8 的修复路径第 1 项已离线验证可行，第 2 项仍是完整的一次内核源码
改动。`runtime_qualified` 仍为 `false`，`UniXNU/scripts/check-accelerator-abi.py` 的
3 个 QEMU gdb-stop 观测（positive / missing-base / default-off）依旧未做。

**收尾后的产物状态（重要）**

* `UniXNU/out/muyu/kernel/kernel.macho` **已恢复**为已上机可启动的
  `b284622440fa38011dec…`（`AGXAccelerator` 符号数 0）。理由：验证内核未上机，
  留在标准输出路径会让后续任何一次 `build payload` 悄悄带上未验证内核。
  验证内核只保存在备份目录，随时可取。
* `board.toml` 的 staged entry **暂时保留**（注释已标 `Offline feasibility probe`，
  若不再推进完整 bring-up 应删掉这一行）；`build-adreno740-accelerator.py` 的路径
  bug 修复保留 —— 这是真实 bug，不加入 archive 清单时完全惰性。
* 设备仍跑 `fw/vendor_boot_gpu_tier2_full.img`（`9ce15faf…`），面板 `o=0`。
  本轮**未刷机、未动设备、未重建 payload、未生成任何新的 vendor_boot**。

重新启用验证内核只需重跑一次 `build kernel`（`board.toml` 已串好 archive）：

```bash
cd /Users/diannao/pad7pro-xnu
python3 UniXNU/unixnu build kernel -D UniXNU-muyu-17 \
  --uefi /Users/diannao/pad7pro-xnu/Mu-Silicium \
  --edk2 /Users/diannao/pad7pro-xnu/Mu-Silicium/Mu_Basecore -j 8
# 预期 kernel.macho sha256 = 469cf7a26290c3e6cd5ce90e58ebacc7df46c3451a10ee09f89ce627d61a26c6
# ⚠️ 该值只对 §11.9 当时的构建脚本成立。§11.10 把 GpuPowerProbe.cpp 也编了进来，
#    现在重跑会得到 cae00a82…（见 §11.10）。
```

### 11.10 发布器移植：把 17 线自己的 GpuPowerProbe 编回内核（19:0x，纯离线，未刷机）

§11.9 的结论是「缺一个已编入内核的发布函数」。真正动手时发现**不需要重写**：
17 线 `package/GpuPowerProbe.cpp` 里 `publishMetal()`（`:1038`）和
`publishAndWrapperTest()`（`:1002`，`native_adreno_publish()` 的唯一调用点 `:1011`）
本来就写好了，只是被 `build-native-legacy-display.py:263` 以「NATIVE_ADRENO 关、
Adreno imports 会 undefined」为由排除——而 §11.8 已证实这条理由对 17 线**过时**
（`muyu-userland_defconfig` 有 `CONFIG_XNU_NATIVE_ADRENO=y`，`libAdreno740.a` 被
`-force_load`，`native_adreno_publish` / `native_gpu_addon_stage` 都在内核里）。

**扩展点早就留好了**：`LegacyDisplayStubs.c:47` 用
`__attribute__((weak)) void sheng_gpu_power_probe(void) {}` 提供空桩，
`LegacyDisplayProbe.cpp:507` 无条件调用 `sheng_gpu_power_probe()`。
把强定义编回去，弱桩自动被覆盖，**不需要新增任何调用者**。

**改动（3 个文件）**

| 文件 | 改动 |
| --- | --- |
| `UniXNU-muyu-17/scripts/build-native-legacy-display.py` | ① 把过时注释改成事实；② 新增 `sheng_gpupower_cpp = ROOT/'kits/muyu/GpuPowerProbe.cpp'` 的编译行（`cmd` 模板 + `gmu_include` + `drivers/Adreno740`、`kits/AdrenoPM4`、`kits/muyu` 三个 `-I`）；③ `ar` 成员表加 `sheng_gpupower_obj`；④ `sources` 清单加该 `.cpp` 与 `kits/muyu/GpuPowerBoard.h` |
| `UniXNU-muyu-17/package/GpuPowerProbe.cpp` | 修 3 处**陈旧性 bug**（见下），并把 zap 镜像声明改成弱默认定义 |
| `UniXNU/out/muyu/tree/scripts/build-native-legacy-display.py` | 同步副本（`tree.py::_copy_scripts()` 会按内容覆盖，源脚本必须一起改） |

**修掉的 3 处陈旧性 bug（不是格式问题）**

1. `:89` / `:351`：`NativeAdrenoGpuBuffer out[5]` 与 `gGpuBuffers[5]`。
   框架 `NativeAdrenoGpuContext.h:34` 现在是 `GpuBufferCount=11`，而
   `NativeAdrenoGpuContext.cpp:251` 的 `NativeAdrenoGpuContextBuffers()` **写满 11 个槽**。
   照原样编进去不只是 `-Warray-parameter` 报错，而是 **6 个 `NativeAdrenoGpuBuffer` 的越界写**。
   改为 `[GpuBufferCount]`；映射循环仍是 `i < 5`，`gGpuMaps` / `gGpuCpu` 保持 `[5]`。
2. `:1330`：`NativeAdrenoRingIo rio{nullptr, ringWrite, ringRead, ringBarrier}`。
   框架 `NativeAdrenoRing.h:20` 新增了 `fence_cookie` / `fence_status`（GMU AHB fence 检查）。
   补 `nullptr, nullptr`，保持旧行为（WPTR 写入不检查）。
3. `:132`：`native_adreno_zap_image` / `_size` 只有 `weak` 声明，而
   `NativeAdrenoFirmwareDefaults.cpp:7` 只给 `firmware_image` 和 `sqe_firmware_image` 兜底，
   **17 线内核没有任何 zap 定义** ⇒ 链接期 `Undefined symbols ... zapAndSecure()`。
   改为本地弱默认定义（`[1]={0}`、`size=0`），与 `FirmwareDefaults` 同一模式。
   `zapAndSecure():511` 看到 `size < 52` 会记 `z[0] = -1` 并跳过安全模式，
   `zapLimit()` 仍受 `sheng-gpu-zap` 控制；§11.6 的 int-019 记录里 zap 关掉才进得了桌面，
   所以这个默认值不是妥协，而是这台板上已知更好的配置。

> 注：`build-native-adreno-kits.py` 本可生成真实 `a740_zap.mbn` 对象，但它还会重建
> `libNativeAdreno.a` 并引入固件 blob，超出本次「只接通发布链」的范围，未启用。

**构建结果**

```text
== kernel: archive-21-build-arm-driver        (build-arm-driver.py Adreno740)
== kernel: archive-22-build-adreno740-accelerator  (injected: build/native-adreno/libAdreno740.a)
== kernel: /Users/diannao/pad7pro-xnu/UniXNU/out/muyu/kernel/kernel.macho sha256 cae00a82591a02c55175f458645a74c0398bf14af0f52596d82d64a587b9caf3
KERNEL_EXIT=0
```

**新内核符号实测**

| 检查项 | §11.9 验证内核 | 本节新内核 |
| --- | --- | --- |
| `_sheng_gpu_power_probe` | `weak external`（空桩） | **`external`（强定义，已覆盖桩）** |
| `publishMetal` | 0 | **1** |
| `AGXAccelerator` 系列 | 25 | 25 |
| `native_adreno_accelerator_allocate` / `instance_size` | 各 1 | 各 1 |
| `__GLOBAL__sub_I_NativeAdrenoAccelerator` | 1 | 1 |
| `native_adreno_publish` / `_stats` / `_published_service` | 有定义、无调用者 | 有定义，**调用点已编入**（`P start` / `P nolink` / `Z off` / `C10` / `S irq` 字符串全部就位） |
| `sheng-gpu-metal` / `sheng-gpu-power` / `sheng-gpu-zap` / `sheng-gpu-selftest` 字符串 | 0 | 各 1 |
| `NativeAdrenoMetal` 字符串 | 0 | 1 |
| `__mod_init_func` 大小 | `0x340` | `0x340`（**无新增静态初始化器**） |
| `native_adreno_zap_image` | 无定义 | `weak external`（`__TEXT,__const`） |
| 内核体积 | 53,035,008 B | 53,121,024 B（+86 KB，就是探针代码） |

**默认仍然是 inert**：`sheng_gpu_power_probe():1722` 第一行就是
`if (!PE_parse_boot_argn("sheng-gpu-power", &mode, ...) || !mode ...) return;`。
不带参数开机，这条路径只做一次 boot-arg 解析就返回；`__mod_init_func` 大小未变
也说明文件级 `gRing` / `gCpInit` 没有引入开机构造函数。

**现在缺的只是「把 boot-arg 带上去」——而命令行预算不够**

`pipeline.py:12 MAX_COMMAND_LINE = 1000`，`payload.py:36` 硬校验。当前 Tier-2 命令行
**996 B / 48 参**，只剩 4 B。两个门控参数：

| 参数 | 含分隔符 |
| --- | --- |
| `sheng-gpu-power=9` | 18 B |
| `sheng-gpu-metal=1` | 18 B |

合计 +36 B ⇒ **1032 B，超限 32 B**。所以必须再腾 ~32 B。候选：

* `offline-legacy-console-quiet=0`（31 B）—— 但它是面板持续可读的唯一开关，删掉就没法看读数；
* `muyu-touch-quiet=1`（18 B）、`muyu-data-probe=2`（17 B）—— 与 GPU 无关，可先让位；
* `muyu-dump-at=20`（17 B）—— 删了 `MUYU GPU` 行就没了（`BravearkLastLog.c:673`），不能删。

即：想上机验证发布链，**至少要同时砍掉「quiet=0 + 一个 18 B 级参数」**，
或者接受用 `sheng-gpu-power=9` 单独跑（先只验 GMU/CP 链，暂不开 Metal 发布）。
这一步需要重新生成 payload + 刷机，**未执行**。

**产物状态**：`out/muyu/kernel/kernel.macho` = `cae00a82…`（本节新内核，未上机）；
离线副本 `/tmp/muyu-publisher-probe-1794394449/kernel-with-publisher.macho`，
同目录还留着改前可启动内核 `kernel.macho`（`b2846224…`）、`board.toml`、
改前 `build-native-legacy-display.py` 与 `libNativeLegacyDisplay.late.a`。
`payload.img` 仍 `c00bef5…`（17:51），`fw/` 下无新增 vendor_boot，
设备仍跑 `fw/vendor_boot_gpu_tier2_full.img`。本轮**未刷机、未动设备、未重建 payload**。

### 11.11 候选镜像：`fw/vendor_boot_gpu_publisher.img`（19:0x，未刷机）

用户选择「腾预算，出完整候选镜像」。动手前先解了现有镜像的命令行，发现一个关键事实：
`sheng-gpu-metal=1` **早就在里面了**（`--kernel-arg` 注入，§16.5 一直这么传），
只是因为内核没有消费者而空转。所以只缺 `sheng-gpu-power=9` 一个参数，**只需腾 18 B，
不是先前估的 32 B**。

**命令行调整（`board.toml`，净变化 0 B）**

| 动作 | 参数 | 含分隔符 |
| --- | --- | --- |
| 注释掉 | `muyu-data-probe=2` | −18 B |
| 新增 | `sheng-gpu-power=9` | +18 B |

**保留 `muyu-touch-quiet=1`**：它是把触摸泵的行挡在 2401 B msgbuf 之外、让
`MUYU GPU` / Metal 行活到 dump 的开关，正是这次要读的东西，不能为省字节牺牲。
`muyu-data-probe=2` 属于 Data overlay 调查，与 GPU 无关，是更干净的让位者。
（`muyu-dump-at=20` 仍不可删，`BravearkLastLog.c:673` 是那行输出的唯一开关。）

**产物与校验**

| 项 | 值 |
| --- | --- |
| 命令行 | **996 B / 48 参**（1000 B 上限，余 4 B） |
| `payload.img` | 75,497,472 B，sha `de75bc30…` |
| `kernel_sha256`（payload.json） | **`cae00a82…`**（§11.10 的发布器内核） |
| `gpu_addon_sha256` | `b2dddc51…`（`fw/gpuaddon-17-full.bin`，10 文件） |
| `trust_cache_sha256` | `0c559104…`（`owner-trust-17-full.bundle`，3 模块 / 3 CDHash） |
| 候选镜像 | `fw/vendor_boot_gpu_publisher.img`，77,393,920 B，sha **`c5d7d327…`** |
| ramdisk 一致性 | 镜像 `@0x1000` 起 75,497,472 B 与 `payload.img` **逐字节相同** |
| ADT | 含 `GpuAddon` 节点，addon 头 `NGPUADD2` |

打包用的是已零填充到 96 MiB 的 `fw/vendor_boot_nofs1.img`
（`/tmp/vendor_boot_nofs1-96MiB.img`，§6.27），`patch-vendor-boot.py` 用
`len(base)` 当分区上限，直接用 64.8 MB 的原文件会放不下 72 MiB payload。

**上机预期（尚未验证）**

开机后面板应出现 `GPU m9 ...` 系列（mode 9 的 GMU 冷启动 → GPU SMMU → hw_init →
CP）与 `M a?.? k? s?/? r?` 行。判读顺序：

1. `s?/0` 且 `r1`：`publishMetal()` 成功注册 `AGXAccelerator`，
   IORegistry 首次出现 `MetalPluginName=NativeAdrenoMetal`；
2. `MUYU GPU` 行的 `o` 从 0 变正：`native_adreno_publish()` 置了 `g.published`，
   用户态终于能开 `AGXDeviceUserClient`；
3. 任一环节 `C1…C10` 报错则链在更早的硬件步骤就断了，与 Metal 无关。

⚠️ 风险：这一版会**真的写 GMU/GPUCC/CP 寄存器并加载 GMU 固件**，是 17 线第一次
在设备上跑完整 Adreno bring-up。§11.6 的 int-017/int-019 记录说明这条链曾在 logo
处卡住用户态；`sheng-gpu-zap` 默认 4 但 17 线没有 zap 镜像（§11.10 的弱默认），
所以 `zapAndSecure()` 会记 `Z-1` 后跳过，CP 停在 secure mode——与 int-016 状态一致。

**回滚**：改前可启动镜像仍是 `fw/vendor_boot_gpu_tier2_full.img`
（`9ce15faf…`），改前 payload 在
`/tmp/muyu-publisher-probe-1794394449/payload-before/payload.img`（`c00bef5…`），
改前 `board.toml` 在同目录 `board-before-publisher-args.toml`。

---

### 11.12 上机三次「启动失败」的真因：用了 `fastboot reboot`，不是 `fastboot boot`（19:05–19:14）

> ⚠️ 本节推翻了本节写作过程中一度得出的「根因是 `probeThread` 的 `IOSleep(10000)` 之后写
> GPU 寄存器」这一判断。**那个判断是错的**，因为设备当时根本没跑到内核。记录在此以免重犯。

**事故经过**

| 时间 | 动作 | 现象 |
| --- | --- | --- |
| 19:05:07 | `fastboot flash vendor_boot` + **`fastboot reboot`** | 约 50 s 回落 fastboot |
| 19:06:04 | 同上，第二次 | 复位到小米徽标 |
| 19:10:29 | 刷**已验证可启动**的 `fw/vendor_boot_gpu_tier2_full.img`（`9ce15faf…`）+ **`fastboot reboot`** | 依然重启到小米徽标 |
| 19:12:19 | `tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_ab_control.img`（内部 **`fastboot boot`** UEFI） | ✅ 启动成功，用户确认 |

**判据：同一个文件、同一个哈希，只换启动方式，结果反转。**
第三条尤其关键——`fw/vendor_boot_gpu_tier2_full.img` 是 17:51 已验证能进桌面、面板可读的镜像，
文件 sha256 从未改变。它「失败」只能是启动方式的问题，不可能是镜像内容。

**根因**：17 线的 UEFI **不在 boot 分区里**，`vendor_boot` 只承载 payload。
必须用 `fastboot boot fw/boot-17line-043f7120.img` 从 RAM 临时加载 UEFI，由 UEFI 再去读
vendor_boot 的 ramdisk。`fastboot reboot` 走的是原厂 Android boot 分区，与我们的 vendor_boot
不配套，结果就是小米徽标 / 回 fastboot。

`tools/muyu-flash-mode.sh` 第 15–16 行早就写死了这条规则：

```text
#   - `fastboot reboot` is never used: it boots the stock Android boot partition against a UniXNU
#     vendor_boot and hangs on the mi logo. Booting XNU is always `fastboot boot <uefi image>`.
```

**取证过程中的无效劳动（记录以免重做）**：为了找「镜像损坏」的证据，逐项比对了
vendor_boot v4 头部（全部字段相同）、payload 的 FAT 目录树与文件起始簇（完全相同）、
payload 内各文件哈希（只有 `kernel.macho` / ADT 的 64 B 签名 / `unixnu.toml` 变）、
新旧内核 Mach-O 头（ncmds 25 / sizeofcmds 5776 / 50 sections / flags `0x00218001` 全同）。
结论全部是「镜像没坏」——而这个结论本身是对的，只是当时把原因归错了地方。

**A/B 对照镜像（本节新增，用于排除内核本身）**

`board.toml` 注释掉 `sheng-gpu-power=9`（命令行 978 B / 47 参），内核保持
`cae00a82…` 不变，重建 payload 并打包：

| 项 | 值 |
| --- | --- |
| 镜像 | `fw/vendor_boot_gpu_ab_control.img` |
| sha256 | `f00df77a3f1e183d687a5ecad8fb5e629d40d9fd8e9314ee0a5c395c133ace3c` |
| 大小 | 77,393,920 B（vendor_boot v4，vendor_ramdisk 72.00 MiB，对齐 OK） |
| ramdisk 一致性 | `@0x1000` 起 75,497,472 B 与 `payload.img` 逐字节相同 |
| 命令行 | 978 B / 47 参，**无** `sheng-gpu-power`，保留 `muyu-dump-at=20`、`offline-legacy-console-quiet=0`、`sheng-gpu-addon=1`、`sheng-gpu-metal=1` |

**19:12:19 上机结果（已确认）**

```text
== slot=a unbootable=no retry=6
== fw/vendor_boot_gpu_ab_control.img
   vendor_boot v4 73.81 MiB vendor_ramdisk=72.00 MiB aligned=True
   Writing 'vendor_boot'  OKAY [0.049s]
== booting UEFI from RAM: fw/boot-17line-043f7120.img
   Booting  OKAY [0.188s]
```

19:12:19 起至 19:13:54 共 **95 s 未回落 fastboot**，用户确认**启动成功**。

由此一次性确认三件事：

1. 内核 `cae00a82…`（带 `GpuPowerProbe` 强定义 + `AGXAccelerator` + 发布链）**能正常加载运行**，
   不存在早于 bring-up 的崩溃；
2. 72 MiB payload + 96 MiB base 的打包方式仍然有效；
3. A/B slot 未被标记 unbootable（`retry=6` 是历史累积，不影响启动）。

**仍未被验证的**：`sheng-gpu-power=9` 到底会不会把设备带崩。到现在为止这条链
**一次都没有真正执行过**——三次「失败」都在到达 `probeThread` 之前就结束了。
§11.11 里关于「会真写 GMU/GPUCC/CP 寄存器」的风险描述仍然成立，但它是待验证的风险，
不是已观察到的事故。

**下一次上机必须用**：

```bash
cd /Users/diannao/pad7pro-xnu
MUYU_UEFI=fw/boot-17line-043f7120.img \
  bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_publisher.img
```

**红线（本次教训固化）**：任何情况下都不对这台设备用 `fastboot reboot` 来验证 UniXNU 镜像；
刷写与启动一律走 `tools/muyu-flash-mode.sh`。

---

## 12. 偏移修正后的实测读数与 `ShengGmuBuildStartup` 的芯片 ID 门（2026-11-11 19:4x）

### 12.1 上机读数（`fw/vendor_boot_gpu_offsets.img`，t≈480 s，照片 `logs/2026-11-11-19-41-56-569.jpg`）

| 行 | 原文 | 判读 |
| --- | --- | --- |
| X4 | `GPU m9 io=0 pw11 G1/0/0/0` | **`pw11`**：`powered_cx=1` + `powered_clocks=1`（上一版是 `pw00`）⇒ 修掉的 GPUCC 偏移让 CX 上电序列第一次真的成立；`G1/0/0/0` 是 GMU 四个状态字：SYSRESET=1（已出复位）、TCM_RET=0、RSC busy=0、HFI=0 ⇒ GMU 可读且空闲 |
| X5 | `C s7 f0 l3 u1/1 p00000005 o1 h000 g000` | PLL1/GMU 时钟序列走到 **step 7、failed=0**，锁 3 次轮询锁定，hub/gmu update 各 1 次 |
| X6 | `p00000005 o1 l44400016 g0000 h0000 b131333 v3` | 6 条 branch 全开（`b131333`），`v3` |
| X7 | `S skip c7/0 p0 s1 i1` | 冷启动闸门拒绝：cmd_db 解析成功（`p0`）、GMU 空闲（`i1`），但 `s1` = **`AdrenoHfiStartupInvalid`** |

### 12.2 根因：策略头里的 chip id 与探针里的字面量不一致

`ShengGmuVotes.c` 的第一道检查是 `in->chip_id != SHENG_GMU_VOTES_CHIP_ID`。
该宏由 `scripts/gen-muyu-gmu-votes.py` 从本机 DTB 的 `qcom,chipid` 生成：

```
#define SHENG_GMU_VOTES_CHIP_ID 0x43030b00u /* DT qcom,chipid, Adreno735 */
```

而 `GpuPowerProbe.cpp` 一直传 `0x43050a01`（a6xx_catalog 的 A740 id，sheng 克隆遗留）。
于是**每一次** `ShengGmuBuildStartup()` 都在第一个参数检查就返回 Invalid，一个 vote 都没构建；
`gColdConfig.chip_id` 也是同一个错值，`ShengGmuColdStart()` 的 `validate()` 同样会拒。

这与 §11.12 的「三次失败是启动方式误判」是两件独立的事：那条解释了为什么没进系统，
这条解释了为什么进了系统之后仍停在 `S skip`。

### 12.3 修法（不再允许任何地方重复这个常量）

1. `ShengGmuVotes.c` 新增 `ShengGmuVotesChipId()`，`ShengGmu.c` 新增 `ShengGmuChipId()`；
   探针改成**向策略要 ID**（`gVoteInputs.chip_id = ShengGmuVotesChipId()`、
   `gColdConfig.chip_id = ShengGmuChipId()`），字面量清零。
2. `ShengGmuBuildStartupDiag()` 取代内部实现，多带一份分类：
   `why` 位（1 参数/chip、2 GPU ARC、4 GMU ARC、8 BCM、16 带宽、32 校验）、
   `gpu_match/gpu_total`、`gmu_match/gmu_total`、各自第一个缺失 level、四段 rail 字节数。
   `ShengGmuBuildStartup()` 语义与副作用完全不变（只是转调）。
3. 新增 `muyu_gpu_diag[6][48]`：`publish()`/投票构建处把结论写进去，
   `BravearkLastLog.c` 每 5 s 打印 `MUYU D0..D5`。原因：`boot_progress_ext` 4..7 会被
   `metalRows()`/`gpuStart()` 覆盖，而 `IOLog` 只打一次、拍照时早就滚没了（§11.4 同一个坑）。

### 12.4 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | `f6885cf702a6581a…`（`_ShengGmuBuildStartupDiag` / `_ShengGmuVotesChipId` / `_ShengGmuChipId` / `_muyu_gpu_diag` 全部 external） |
| payload | `cb73675b00c90652…`，cmdline **996 B / 48 参**（与上一版逐字一致） |
| 镜像 | `fw/vendor_boot_gpu_chipid.img`，77,393,920 B，sha256 **`0f79b313…`** |
| 待验证 | 冷启动闸门放行后，GMU 固件加载 + HFI 是否成立（会真的写 GMU/释放 CM3） |

**下一次判读顺序**：先看 `MUYU D3`（`B st… why…`）：

- `st0` → 闸门放行，往下看 `X7`/`X6` 的 `P/S` 行（冷启动协议）；
- `st1 why1` → 仍有参数问题（chip id 或 gmxc 标志）；
- `st1 why2/4` + `D2` 的 `gf/gmu` 匹配数不足 → OPP 表与板子 rail 数据不一致，按 `R` 行重生成表；
- `st1 why8/16/32` → BCM/带宽/校验，按 `D1` 的 rail 字节数与 `cn0` 地址查。

---

## 13. 芯片 ID 修好之后：准入放行，卡在 GMU 固件没有链进内核（2026-11-11 20:0x）

### 13.1 上机读数（`fw/vendor_boot_gpu_chipid.img`，t≈213 s，照片 `logs/2026-11-11-20-01-55-747.jpg`）

```
MUYU D0 P ok ok ok skip io0 pw11
MUYU D1 V chip 43030b00 gmxc0 32/32/32/0
MUYU D2 R gf12/12 m0 cx3/3 m0
MUYU D3 B st0 why0 d0 p0 s0 c0005b30
MUYU D4 X cx0022f801>f822f000 hw00000002 gx0022f801
MUYU D5 A ahb00000001>00000001 pll00006257f mm00000003
MUYU X4 GPU m9 io=0 pw11 G1/0/0/0
MUYU X5 C s7 f0 l2 u1/1 pe0000005 o1 h000 g000
MUYU X6 pe0000005 o1 l44440016 g0000 h0000 b131333 v3
MUYU X7 S nolink
MUYU PROBE st=71 cp=0
MUYU GPU calls=9 refused=9 o=0 c=0 f=0 fb=0 tc=9 ts=00000000 hold=1 pres=1 l=0
```

| 行 | 判读 |
| --- | --- |
| `D1 V chip 43030b00` | §12 的 chip id 修复生效；`gmxc0` = 本板不消费 GMXC rail；四段 rail 字节 `32/32/32/0` |
| `D2 R gf12/12 m0 cx3/3 m0` | GPU 12 个 OPP level 与板子 rail 数据 **12/12 全匹配**，CX 3/3 全匹配，没有缺的 level（`m0`）⇒ OPP/BCM 表本身没问题 |
| `D3 B st0 why0 d0 p0 s0` | **准入终于放行**：`ShengGmuBuildStartupDiag` 返回 `ShengGmuSuccess`，没有任何 `why` 位 |
| `X7 S nolink` | 与上一版的 `S skip` 不同：不再是准入拒绝，而是 `gmuColdStart()` 在**第一道弱符号检查**就返回了（`native_gpu_power_stage=0x71`，与 `MUYU PROBE st=71` 一致） |

### 13.2 根因：探针找的固件符号这个名字，这个内核里没有任何人定义

`gmuColdStart()` 的守卫是：

```cpp
if (!ShengGmuColdStart || !ShengGmuBindMmio || !NativeAdrenoGmuContextPayloads ||
    !native_adreno_firmware_image || native_adreno_firmware_image_size < 4) { setRow(7, "S nolink"); return; }
```

`nm` 显示前三个都是 `external`（`_ShengGmuColdStart`、`_ShengGmuBindMmio`、`_NativeAdrenoGmuContextPayloads`），
而 `_native_adreno_firmware_image` / `_native_adreno_firmware_image_size` 是 **`weak external`，落在 `__TEXT,__const`**，
即链接器给的零值占位 —— 于是 `native_adreno_firmware_image_size == 0 < 4`，函数在映射任何窗口之前就返回。

名字不匹配的来源很清楚：`_native_adreno_firmware_image` 的强定义只由
`UniXNU-muyu-17/scripts/build-native-adreno-kits.py`（sheng/uke 线，`.cache/firmware/sheng-gpu/qcom/gmu_gen70200.bin`）
和 `UniXNU-uke/...` 产出，**都不参与本内核的链接**。
本板真正被编进内核的固件是 `build-native-legacy-display.py` 里
`gen-muyu-gmu-source.py` 生成的 `build/gpu/MuyuGmuFirmware.c`，符号是
`_muyu_gmu_firmware` / `_muyu_gmu_firmware_size`（80,680 B，`__TEXT,__const`，**external**）。
也就是说固件一直在镜像里（`0x7040610`，长度正好顶到 `0x7054138` 的 size 符号），只是探针没去找它。

### 13.3 修法

`gmuColdStart()` 按能成功的顺序取镜像，并给两个来源各自的拒绝态：

```cpp
const uint8_t *fw = muyu_gmu_firmware_size ? (const uint8_t *)muyu_gmu_firmware : native_adreno_firmware_image;
const size_t fw_bytes = muyu_gmu_firmware_size ? (size_t)muyu_gmu_firmware_size : (size_t)native_adreno_firmware_image_size;
if (!fw || fw_bytes < 4) { setRow(7, "S nofw m<有muyu>/n<有native>"); return; }
```

`S nolink` 现在只表示三个 kit 符号真的缺；固件缺失是独立的 `S nofw m0 n0`，下一次拍照能直接区分。

### 13.4 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | 53,121,024 B，`_muyu_gmu_firmware`(external, `__TEXT,__const`) + `S nofw m%x n%x` 字符串均已进镜像 |
| payload | 72 MiB，cmdline 与上一版逐字一致（48 参） |
| 镜像 | `fw/vendor_boot_gpu_gmufw.img`，77,393,920 B，sha256 **`15bda9d2…`** |
| 状态 | 已构建，**未刷** |

### 13.5 下一次判读（`X7` 那一行，现在会变成冷启动的结果行）

- `S0 ph… h1 f… H… r11 a0 …` → 固件加载完成、HFI 握手成功，`gpuOn()` 会接着跑（`mode>=8 && success`）；
- `S<非0> ph<小数字>` → 停在加载阶段，`ph` 就是阶段号；
- `P<协议状态> g<stage> k<ack 掩码> e<固件错误码>` → HFI 协议层，`e` 是 GMU 回报的错误；
- `GPU m7 G1/0/0/0>G0/1/0/1` → 后四位 `0/1/0/1` 才是"固件在跑"的样子（SYSRESET 落下、TCM_RET 起、HFI 置位）。

---

## 14. 固件取到之后：`S bind refused`，窗口基址还是 sheng 板的那一套（2026-11-11 20:1x）

### 14.1 上机读数（`fw/vendor_boot_gpu_gmufw.img`，t≈213 s，照片 `logs/2026-11-11-20-11-52-193.jpg`）

```
MUYU X4 GPU m9 io=0 pw11 G1/0/0/0
MUYU X5 C s7 f0 l3 u1/1 pe0000005 o1 h000 g000
MUYU X6 pe0000005 o1 l44440016 g0000 h0000 b131333 v3
MUYU X7 S bind refused
MUYU D3 B st0 why0 d0 p0 s0 c0005b30
MUYU PROBE st=71 cp=0
MUYU GPU calls=7 refused=7 o=0 c=0 f=0 fb=0 tc=7 ts=00000000 hold=1 pres=1
```

`X7` 从 §13 的 `S nolink` 变成了 **`S bind refused`**：说明固件符号已经取到（否则会是 `S nofw`）、
五个窗口也都映射成功（否则是 `S nomap w…`）、五个 payload 也拿到了（否则是 `S payloads …`），
卡在 `ShengGmuBindMmio()`。

### 14.2 根因：探针里那五个窗口是 sheng 的副本

`ShengGmuBindMmio()`（`package/ShengGmu.c:276`）逐窗口比对：

```c
if (!fits(m,i,0) || w->physical != sheng_gmu_window_physical[i] ||
    w->bytes != sheng_gmu_window_bytes[i]) return ShengGmuArgument;
```

`sheng_gmu_window_*` 由 `gen-muyu-gmu-source.py` 从**本板 DTB** 生成（`build/gpu/ShengGmuSource.h:51-52`）。

| 窗口 | 生成值（DTB） | 探针旧字面量 |
| --- | --- | --- |
| core | `0x3d68000` / `0x37000` | `0x3d6a000` / `0x35000` |
| rscc | `0x3d50000` / `0x10000` | 同 |
| gmu_pdc | `0xb280000` / `0x10000` | 同 |
| gpu | `0x3d00000` / `0x40000` | 同 |
| cx_misc | `0x3d9e000` / `0x1000` | 同 |

于是**每一次** `ShengGmuBindMmio()` 都在第 0 个窗口就返回 `ShengGmuArgument`，冷启动连一次寄存器都没读。

### 14.3 同一个错误还污染了 §12/§13 的读数

模式 5 的读窗口与偏移是同一套 sheng 数字：

```cpp
kGmuPhys[0] = 0x03d6a000;  kGmuLen[0] = 0x35000;
kGmuSysReset = 0x1f800*4 - 0x6a000;   // = 0x14000
kGmuReg(d)   = d*4 - 0x6a000;
```

正确关系是 `GMU 块 = GPU 基址 0x3d00000 + SHENG_GMU_CORE_GPU_DELTA`，生成头里 `DELTA = 0x68000`，
所以 `CM3_SYSRESET(0x1f800)` 的绝对地址是 `0x3d7e000`。旧式 `0x3d6a000 + 0x14000` 算出来是 `0x3d7e000` ——
**绝对地址是对的，但前提是基址也得是 0x3d6a000**。基址一换，偏移就必须跟着换；反过来也一样：
只要两者之一是对的、另一个是 sheng 的，读到的就是**偏移 0x2000 的别的寄存器**，而它仍在合法窗口内，
不会 fault，看上去"有数"。所以 `X4` 的 `G1/0/0/0`、`X5` 的 `C s7 f0 l3`、`pll00006257f` 这些读数
**在新窗口生效前都不能当作 GMU 真实状态**。

### 14.4 修法：不再手抄，只向策略要

1. `ShengGmu.c` 新增 `ShengGmuWindowPhysical(i)` / `ShengGmuWindowBytes(i)`，直接返回生成头里的值；
   `ShengGmu.h` 声明，并注明 "BindMmio 会拒绝任何不一致的窗口，平台自己映射就必须问这里"。
2. `gmuColdStart()` 删掉 `kColdPhys/kColdLen` 两个字面量数组，改为每窗口现取；失败态细化为
   `S nomap w<窗口> p<物理> l<长度>`，把是哪个窗口、哪个地址打出来。
3. 模式 5/8 的读窗口改成 `0x3d68000/0x37000`，`kGmuSysReset`/`kGmuHfiStatus`/`kGmuReg()` 的减数
   从 `0x6a000` 改为 `0x68000`（绝对地址不变，语义与生成头一致）。
4. AO 中断块（`0x23b04` dword，绝对 `0x3d8ec10`）的偏移改为 **按实际窗口基址现算**，并加一条
   "越界就不写"的守卫（`S ao outside window`）：原来那个 `- 0x6a000` 是跟着旧基址走的，换基址就会打到
   `0x3d8cc10` 这种"映射着但不是 AO 块"的地方。

### 14.5 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | 53,121,024 B，sha256 `48837a9f…`；`_ShengGmuWindowPhysical` / `_ShengGmuWindowBytes` external，`S nomap w%u`、`S ao outside window` 均在 |
| payload | 72 MiB，cmdline 48 参不变 |
| 镜像 | `fw/vendor_boot_gpu_windows.img`，77,393,920 B，sha256 **`fac65700…`** |
| 状态 | 已构建，**未刷** |

### 14.6 下一次判读

`X7` 若变成 `S running` 之后的结果行（`S0 ph…` 或 `S<非0> ph…`），表示窗口与固件都过了，进入真正的加载流程；
若仍是 `S nomap w0 p3d68000 l37000`，说明 `IODeviceMemory` 拒绝了这个 range（不是地址错，是可用性/权限）；
若是 `S bind refused`，那就是 `fits()`/`overlap()` 的问题，把这几个窗口的虚拟地址打印出来再判。

---

## 15. 窗口过关之后：CM3 释放复位但 FW_INIT 无回音，卡在 10 ms 的等待（2026-11-11 20:2x）

### 15.1 上机读数（`fw/vendor_boot_gpu_windows.img`，t≈187 s，照片 `logs/2026-11-11-20-20-55-098.jpg`）

```
MUYU X4 GPU m7 G1/0/0/0>G1/0/1/0        ← 前: SYSRESET=1/RSC=0  后: SYSRESET=1/RSC busy=1
MUYU X5 C s7 f0 l3 u1/1 pe0000005 o1 h000 g000
MUYU X6 P1 g0 k0 e0 t0 n0 pe0000005 o1 h000 r0
MUYU X7 S6 ph5 h1 f0 H0 r00 a0 c0 v1
MUYU PROBE st=7f cp=0
MUYU D3 B st0 why0 d0 p0 s0 c0005b30
```

- `X7` 不再是 `S bind refused`：**窗口校验通过**，真加载跑起来了；`PROBE st=7f` 是失败分支。
- `S6` = `ShengGmuTimeout`，`ph5` = 停在"已释放 CM3 复位、等 `FW_INIT_RESULT`"这一步。
- `h1` = 整镜像 SHA-256 通过；`v1` = 失败后重新拉回复位的回读校验通过。
- 关键：`f0` —— `FW_INIT_RESULT` **读到的就是 0**，不是 0x100 也不是 0xbabeface，说明 CM3 一个字节都没写。
- `X4` 的"后"是 `reset_after_failure()` 把 SYSRESET 拉回 1 的结果（`G1/0/1/0`），不是"从没释放过"。

阶段里 1→4 全部走通：SYSRESET=1 已置、`CX_MISC_TCM_RET_CNTL=0`、RSCC 空闲、
19 条 RSC setup 写完、TCM_RET 置 1 并回读、ITCM/DTCM 逐字写入并**逐字回读一致**（否则 phase 3 就会
以 `ShengGmuStateMismatch` 退出）、11 个 boot 配置寄存器回读一致。

### 15.2 根因（高置信）：`poll()` 把每一次等待都截在 10 ms

`poll()` 里那行硬编码：

```c
if(now-start>=10000)return ShengGmuTimeout;   // 10 ms，不管 budget 给多少
```

vendor 的两个等待**不是一个量级**：

| 等待 | vendor `gmu_poll_timeout` | 我们的实际时限 |
| --- | --- | --- |
| `CM3_FW_INIT_RESULT`（CM3 起固件） | `100, 100000` = 100 ms | **10 ms** |
| `HFI_CTRL_STATUS` | `100, 10000` = 10 ms | 10 ms（巧合正确） |

也就是说：即便 CM3 是活的、只是在 30–80 ms 才把 `0x100` 写进去，我们也会先判超时。
探针给的 `poll_budget=200`（200×100 µs = 20 ms）同样盖不住 100 ms。

**注意这条不是"已证明"**：`f0` 只能说明 10 ms 内没写，不能说明 100 ms 内不会写。所以这一版先把这个
变量消掉；若 100 ms 仍是 `f0`，才算"CM3 真的没跑"，那时再查 `GENERAL_10` 的 chip id 打包、
`a6xx_rpmh_start` 的 rail 投票与 GX rail（`pre_hfi`）这几条。

### 15.3 改法

1. `poll()` 增加 `timeout_us` 参数；`FW_INIT` 传 `100000`，`HFI` 传 `10000`，与 vendor 逐字对齐。
2. `gColdConfig.poll_budget` 200 → **1000**（1000×100 µs ≈ 100 ms，`validate()` 上限 1024 内）。
3. `ShengGmuResult` 新增 `dtcm_probe`，记录启动前 `CM3_DTCM_START+0xff8` 的读数——它决定等
   `0xbabeface`（旧格式）还是 `0x100 & 0x1ff`（新格式）。`S` 行扩展为
   `S<st> ph… f… H… r… a… c… v… d<dtcm_probe> q<poll 次数>`：`q` 能直接看出等待是被"轮询耗尽"还是
   "10/100 ms 到点"结束的。

### 15.4 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | sha256 `e7ca77d0…`，`d%x q%u` 格式串在镜像内 |
| 镜像 | `fw/vendor_boot_gpu_fw100ms.img`，77,393,920 B，sha256 **`6a3399df…`** |
| 状态 | 已构建，**未刷** |

### 15.5 下一次判读

- `S0 ph7 … r11` → CM3 起来了、HFI 也握上手，接着 `gpuOn()`；下一步就轮到 GX rail 与 CP。
- `S6 ph5 … f0 … q1000` → 100 ms 真等满了仍是 0：CM3 确实没执行，转查 `GENERAL_10` 的 chip id
  打包公式（vendor：`chipid = (id&0xffff0000) | ((id<<4)&0xf000) | ((id<<8)&0x0f00)`，
  而生成头里写的是 `0x70b0000`）与 `a6xx_rpmh_start` 的 rail 投票。
- `S6 ph5 … f0 … q<小数字>` → 等待被别的东西提前打断（时钟/中断读失败），看 `a`/`c`。
- `S5 ph5` → `ShengGmuStateMismatch`，说明 `FW_INIT_RESULT` 里出现了非预期的中间值，`f` 会带着它。

---

## 16. 第一次进到用户态：控制台换手，5 秒 printf dump 从此失效（2026-11-11 20:3x）

### 16.1 上机读数（`fw/vendor_boot_gpu_fw100ms.img`，照片 `logs/2026-11-11-20-31-22-580.jpg`）

这一屏和前面所有次都不一样：**整屏都是用户态启动日志**，只有最后两行是内核的 `IOLog`。

```
apfs_sysctl_register… dev_init:305/306 …                      ← 开卷
import_boot_firmware_root_hash:349: apfs_extract_root_hash_and_manifest failed with
    error: No such file or directory (2)                       ← 根哈希清单取不到
IONVRAMController: serializeVariables:615 - No nvramController
IONVRAMController: sync:694 - serializeVariables failed, ret=0xe00002d8
NativeFS: root manifest version 1 windows=2 objects=0          ← objects=0
NativeFS: published OS
dev_init:303/306: disk4s1 device_handle block size 4096 block count 1483776
mx_mount:1617: disk4s1 checkpoint search: largest xid 410, best xid 418 @ 3
mx_mount:1644: disk4s1 stable checkpoint indices: desc 2 data 4
NativeFS: OS volume ready                                      ← 最后一条用户态日志
MUYU PWR gc=ok cc=ok sm=ok gp=skip io=0 pw11 en=0 ahb=00000001>00000001 pll=0060257f
MUYU PWR cx=0022f801>f822f000 hw=00000002 gx=0022f801 cc_ahb=00000000 cc_gmu=00002001 sv=00000001
```

- **`X4`–`X7`、`D0`–`D5`、`MUYU LIVE`、`MUYU PROBE` 一行都没有。** 不是探针没跑：`MUYU PWR` 是
  `publish()` 在 **t+10 s** 打的 `IOLog`，它在用户态那几行**之后**，说明探针照常跑完了整轮快照。
- 卡点在 `NativeFS: OS volume ready` **之后、根设备交接之前**：27 线进用户态时的日志顺序是
  `Got boot device = …` → `BSD root: disk5s1, major 1, minor 0` → `apfs_vfsop_mountroot:3156`，
  这三条一条都没出现。**不是卡在 launchd/dyld，是根卷没交给内核。**

### 16.2 为什么这次看不到 GPU 结论：两条输出通道的时序分开了

| 通道 | 首行时刻 | 这一轮 |
| --- | --- | --- |
| `publish()` 的 `IOLog` | **t+10 s**（`probeThread` 先 `IOSleep(10000)`） | ✅ 上屏 |
| `BravearkLastLog` 的 5 s `printf` dump | **t+30 s**（`muyu-dump-at=20`，从 t+10 s 的 `muyu_dump_start()` 起算） | ❌ 不上屏 |

前面每一次都能一直看到 `MUYU LIVE/PROBE/X/D`，是因为那些启动**根本没到用户态**、控制台始终归内核；
这一轮到 t+30 s 时用户态已经接管面板，内核 `printf` 不再落到这块屏上。
`board.toml` 里那条 `muyu-dump-at` 的注释（"t+60 s 的 dump 会在 GPU scanout 客户端拿走面板时
调 `muyu_console_wake()` 解停文本控制台"）说明这条通路本来就与显示所有权纠缠，不能依赖。

**所以：能读到的只有 `IOLog`，而且必须早打。** 冷启动的结论（`X7`/`D3`）这一轮没有拿到，不是没发生。

### 16.3 用户态卡点的两条线索（同一根）

1. `apfs_extract_root_hash:327` 取不到 `system-volume-auth-mb0` blob → `import_boot_firmware_root_hash`
   失败（errno 2）→ `NativeFS: root manifest … objects=0`：**清单是空的**。系统卷是封签的，
   没有清单就无法完成校验，`mountroot` 走不下去。
2. `No nvramController` + `sync … ret=0xe00002d8`：NVRAM 链断着，而这类 auth blob 正常随
   boot 参数/设备树下来，很可能就是上一条的直接原因。

两条都发生在 GPU 探针之前（`MUYU PWR` 在其后），与 GPU 写操作无因果关系。

### 16.4 本轮产物（两个，都未刷）

| 项 | 内容 | 值 |
| --- | --- | --- |
| 内核 | 新增 `logRows()`：冷启动/跳过一结束就用 `IOLog` 打 `X7`、`X6`、`D0`–`D5`（t+10 s，走已被证明能上屏的通道） | `7a6269d2…`，53,121,024 B，`MUYU %s X7 %s` 在镜像内 |
| 镜像 A（诊断） | 同一内核 + `sheng-gpu-power=9`（正常参数） | `fw/vendor_boot_gpu_coldlog.img`，77,393,920 B，**`64f5a005…`** |
| 镜像 B（对照） | 同一内核，**去掉 `sheng-gpu-power=9`**（payload 978 B / 47 参），探针完全不跑 | `fw/vendor_boot_gpu_ab_noprobe.img`，77,393,920 B，**`243e7d9a…`** |

`board.toml` 只临时改过一行，已按备份逐字恢复（`"sheng-gpu-power=9",` 在 571 行）。

### 16.5 判读顺序

1. 先刷 A：`MUYU COLD X7 …` 会给冷启动的结论（`S0 ph…` 成功 / `S6 ph5 f0 q…` 等），
   `MUYU COLD D0..D5` 给电源、投票与拒绝原因。**同时**看它是否仍然停在 `OS volume ready`。
2. 再刷 B：若 B 能走到 `Got boot device`/桌面，则用户态卡点是我们的 GPU 写操作造成的，优先级高于继续移植；
   若 B 也停在 `OS volume ready`，则与 GPU 无关，转查 root manifest / NVRAM。

---

## 17. GMU 起来了：CM3 固件跑通、GX 上电，卡在 GPU SMMU 的 Attach（2026-11-11 20:45）

### 17.1 上机读数（`fw/vendor_boot_gpu_coldlog.img`，照片 `logs/2026-11-11-20-45-00-824.jpg`）

```text
MUYU LIVE p=18 t=211 btn st=3 irq=0 rep=0 msk=0x0 pon=476 gpio=-1 pdc=0x3020 err=0 sp=0 oth=0
MUYU GPU calls=5 refused=5 o=0 c=0 f=0 fb=0 tc=5 ts=00000000 hold=1 pres=1 | w=0
MUYU PROBE st=94 cp=0
MUYU X4 GPU m7 G1/0/0/0>G1/0/1/0
MUYU X5 X v0/0 o4 x3>0 cf8222800 R00000001 H0
MUYU X6 Q step 4 running
MUYU X7 S0 ph7 h1 f100 H1 r11 a0 c1 v0 d4050001 q158
MUYU D0 P ok ok ok skip io0 pw11
MUYU D1 V chip 43030b00 gmxc0 32/32/32/0
MUYU D2 R gf12/12 m0 cx3/3 m0
MUYU D3 B st0 why0 d0 p0 s0 c0005b30
```

### 17.2 `X7 S0 …` —— 冷启动第一次成功

| 字段 | 值 | 含义 |
| --- | --- | --- |
| `S0` | `ShengGmuSuccess` | 整条 `ShengGmuColdStart()` 走完，**CM3 真的在跑我们的 `gen71100_gmu.bin`** |
| `ph7` | 最后一个阶段 | 1–4（RSC/TCM）之后 5（释放复位）、6（等 FW_INIT）、7（HFI）全过 |
| `h1` | 整镜像 SHA-256 通过 | |
| `f100` | `FW_INIT_RESULT = 0x100` | 固件按新格式写了 ready 字，`d4050001` 说明启动前 DTCM+0xff8 的值 ≤ `0x20010004`，等的正是 `0xbabeface` 分支 |
| `H1` | `HFI_CTRL_STATUS = 1` | **HFI 握手成立** |
| `r11` | `firmware_ready_observed=1`、`hfi_ready_observed=1` | |
| `a0`/`c1` | AO 故障 0 / CM3 故障字 1 | |
| `q158` | 158 次轮询 × 100 µs ≈ **15.8 ms** | **§15 的判断被证实**：旧的 `poll()` 把每次等待截在 10 ms，CM3 在 10–16 ms 才写 `0x100`，所以之前永远 `f0`。`poll_budget=1000` + 100 ms 一放开就过了 |

### 17.3 `X5 X v0/0 o4 x3>0 cf8222800 R00000001 H0` —— GX 也上电了

`o4`：`a6xx_gmu_set_oob(GMU_OOB_GPU_SET)` 的 ack 在 **4 次** 100 µs 轮询内到位；
`x3>0`：GX 的 `HM_GDSC_POWER_OFF`/`HM_CLK_OFF` 两位从 `3` 变 `0`，**GX rail 真的起来了**（之前每次都 `gx-off`）；
`c=f8222800`：GX GDSCR 回读 bit31=1（PWR_ON）；`R=00000001`（`RBBM_STATUS`，GPU 不忙、可访问）；
`H0`（`GBIF_HALT` 已放开）。
这是本板第一次同时具备“GMU 固件在跑 + GX 上电 + RBBM 可读”。

### 17.4 新断点：`Q step 4 running`，即 GPU SMMU 的 Attach

`gpuStart()` 里 `NativeAdrenoGpuContextBringup(bank 0, asid 2, slots 0/1, sids 0/1)` 的步骤是
`Allocate(1) → Prepare(2) → Program(3) → Attach(4) → VmAttach(5) → Map(6) → Ready(7)`。
`Q step 4 running` 是 `Attach` **进入前**写的进度行，`PROBE st=94` = `0x90 + 4`，而 t=211 s（18 个 5 s 周期）
仍是这一行 ⇒ **`ArmSmmu4KAttachStreams()` 没有返回**。前 3 步（含 `Program` 的寄存器写入与回读）都成立，
所以 SMMU 本身是通的；卡的是 Attach 里第一组“新”访问：`global()` 的 GR0 读、`ObserveOwnedContext`、
128 个 SMR/S2CR 的 `scan`，或随后每个 slot 的 `S2CR→SMR→回读`序列。

`attach` 里没有无界循环（`n ≤ 8`，`quarantine` 也有限），所以最可能的是**某次寄存器访问永远不返回**
（读未开时钟的块不会 fault，只会停在那里）——一个 CPU 卡住、其它 CPU 照常跑，这正是“屏还在刷、
`MUYU LIVE` 的 `t` 还在涨”的样子。

### 17.5 一条必须避免的误读

这一屏**不能**用来判断“是否进用户态”：`MUYU LIVE` 每 5 s 打约 11 行，18 个周期把之前的内容全顶掉了。
上一轮之所以能看见用户态日志，是因为那一轮探针在 `S6` 处就结束、屏上没有我们的刷屏。

### 17.6 本轮产物

| 项 | 内容 | 值 |
| --- | --- | --- |
| 面包屑 | `ArmSmmu4KStreams.c` 加 `volatile unsigned arm_smmu4k_attach_crumb`（弱定义，纯 store），在 `1/2/3/4`、每个 slot 的 `0x1k..0x1k+6`、`0x20/0x21/0x22/0x23/0x24/0x30` 处落点 | 任一 CPU 可读，卡住的 CPU 也会留下位置 |
| 打印 | `BravearkLastLog` 的 `MUYU PROBE` 行改为 `st=%x cp=%u b=%x` | 每 5 s 一轮 |
| 内核 | `4c26e388…` | `_arm_smmu4k_attach_crumb` 为 weak external、落在 `__DATA,__data` |
| 镜像 | `fw/vendor_boot_gpu_attachcrumb.img`，77,393,920 B | **`d3a245a3…`**，20:52 已上机（读数见 §18） |

### 17.7 判读表（`b=`）

| `b=` | 停在哪 |
| --- | --- |
| `0` | 从未进入 attach（`b=0` 且 `Q step 4` 说明连调用都没发生，看上层） |
| `1` | 进了 attach，卡在 `global()`（GR0/ID0/SCR0） |
| `2` | 卡在 `ArmSmmu4KObserveOwnedContext`（bank 寄存器） |
| `3` | 卡在 `scan`（128 个 SMR/S2CR 全扫） |
| `0x11`–`0x16` | 卡在第 1 条路由（`i=0`，slot 0 / SID 0）的 `S2CR→SMR` 序列（`+1` 写 S2CR、`+2` 回读 S2CR、`+3` 写 SMR/回读、`+4` 校验 SMR、`+5` EXIDS 再写 S2CR） |
| `0x19`–`0x1e` | 卡在第 2 条路由（`i=1`，slot 1 / SID 1）的同序列。**编号是 `0x10+8*i+k`，所以 `0x19` 起才是第二条路由，不是 `0x2?`**（`0x2?` 只在 `n>2` 时出现，会与复核段的 `0x21` 撞号；本板 `n≤2` 不会发生） |
| `0x20` | 卡在 `observe()` 进入前 |
| `0x21`→`0x24` | 卡在复核阶段（global / context / scan） |
| `0x30` | attach 失败并已进 `quarantine`（这次应能看到 `Q s4 st…` 行） |

---

## 18. Attach 卡点定位：第 2 条路由（slot 1 / SID 1），不是 SMMU 不通

### 18.1 上机读数（`fw/vendor_boot_gpu_attachcrumb.img`，照片 `logs/2026-11-11-20-56-13-659.jpg`）

```text
MUYU PROBE st=94 cp=0 b=1b
MUYU X4 GPU m7 G1/0/0/0 XG0/1/0/1
MUYU X5 X v0/0 o4 x3>0 cf8222800 R00000001 H0
MUYU X6 Q step 4 running
MUYU X7 S0 ph7 h1 f100 H1 r11 a0 c1 v0 d4050001 q159
```

`b=1b` 与 `Q step 4 running` 互相印证：`Q step 4` = `NativeAdrenoGpuContext::bringUp` 的
`Step::Attach`（枚举 `Allocate(1)…Ready(7)`），`b=1b` = 该步内部的位置。

### 18.2 `b=1b` 的精确含义（编号是 `0x10 + 8*i + k`）

`0x1b = 0x10 + 8*1 + 3` ⇒ **第 2 条路由（`i=1`，调用方传的是 `slot1=1, sid1=1`），第 3 个落点**。
按 `ArmSmmu4KStreams.c` 的 `CRUMB()` 位置，这意味着：

| 已确认发生 | 说明 |
| --- | --- |
| `global()`、`ObserveOwnedContext`、`scan`（128 个 SMR/S2CR 全扫） | 全过 —— **SMMU 的读通路是通的**，`Program` 的写回读也过 |
| 第 1 条路由（`i=0`，slot 0 / SID 0）的 `S2CR 写→barrier→回读==bank`、`SMR 写→barrier→回读==V\|0`、EXIDS 复查 | 全过（`0x11`→`0x16`） |
| 第 2 条路由的 `S2CR 写→barrier→回读==bank` | **过**（`0x19`→`0x1a`） |
| 随后 `wr(SMR[1], V\|1) → barrier → rd(SMR[1])` | **没回来**（停在 `0x1b`，`0x1c` 从未写下） |

`b=1b` 之后没有任何后续面包屑，`Q step 4` 直到 t=230 s（~40 个 5 s 周期）不变 ⇒
`ArmSmmu4KAttachStreams()` 仍未返回，**执行 attach 的那个 CPU 停在一次寄存器访问上**，
其余 CPU 照常跑（屏仍刷、`MUYU LIVE` 的 `t` 继续涨）。

### 18.3 已排除的三种解释

1. **不是偏移/窗口错**：`Program` 已对 bank 0 完成 `SCTLR/TTBR0/…` 的写回读，且 `scan` 扫过全部 slot。
2. **不是 EXIDS 分支**：本 SMMU `ID0[8]=0`（`v0`），`valid_smr = 1<<31`，走的是非 EXIDS 路径。
3. **不是 `barrier`**：`smmu_barrier()` 只是 `__atomic_thread_fence(__ATOMIC_SEQ_CST)`（编译器屏障），
   不碰硬件，不可能挂。所以卡的是紧接着的那次 **SMR 写或 SMR 回读** 本身。

### 18.4 为什么“第 1 条路由过、第 2 条不过”

非 EXIDS 模式下 `SMR` 一旦带 `V=1`，该 SID 立刻进入匹配集；`S2CR[1]` 已先被写成 CB0（TRANSLATE），
所以 **写 `SMR[1]` 的那一刻就是 SID 1 第一次真正被接到 CB0 上**。若此刻有 master 正在用 SID 1 发事务，
SMMU 会去取上下文描述符；该请求一旦挂在总线上，SMMU 的寄存器从口就被排在它后面，**后续对同一窗口的
一次读永远等不到响应**——这正是“一个 CPU 消失、其它 CPU 正常”的形态。第 1 条路由没有触发，
说明 SID 0 那一侧当时是静的（或它本来就已在该 bank 上）。

这条推断**必须用数据确认**，所以本轮不靠猜：先取只读快照（§18.5 的 `A0`–`A7`），并只挂第 1 条路由。

### 18.5 本轮改动（两处，都为判断而不是“修法”）

| 文件 | 改动 |
| --- | --- |
| `UniXNU/drivers/Adreno740/NativeAdrenoGpuContext.cpp` | 新增 `native_adreno_smmu_audit[8][48]` 与 `smmuAudit(io)`：在 `step(Attach)` **之前**、`SMMU` 尚是固件状态时，只读地记录 `IDR0/IDR1/SCR0/GFSR`、CB0 的 `SCTLR/ACTLR/TTBR0`、`GBPA`、slot 0/1 的 `SMR/S2CR` 原值、有效流数量、以及 **SID 0/1/5 当前分别落在哪个 slot**（`-1` = 没有）。纯读，无写、无锁 |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | 5 s 心跳中加 `MUYU A0..A7`，读上面那个数组。心跳跑在别的 CPU 上，**即使 attach 的 CPU 消失，这 8 行照样上屏** |
| `UniXNU-muyu-17/package/GpuPowerProbe.cpp` | `NativeAdrenoGpuContextBringup(...)` 的 `sid1` 由 `1` 改为 `kSecondSid = UINT16_MAX` ⇒ `attach(routes, 1)`，**只租 slot 0 / SID 0 一条路由**。目的：一次上机把“SID 1 这条路由是卡点”与“整条 attach 路径都不可用”分开。旁边注释写明怎么改回 `1` |

### 18.6 `MUYU A0..A7` 判读表

| 行 | 格式 | 读什么 |
| --- | --- | --- |
| `A0` | `i<IDR0> j<IDR1> s<SCR0>` | `i` 低 8 位 = SMR 槽数、bit27 = 支持 64 位、bit8 = EXIDS；`s` 低 3 位应为 `6`（M\|GFRE\|GFIE，CLIENTPD=0） |
| `A1` | `f<GFSR> cb<CB0.SCTLR> a<CB0.ACTLR>` | `f≠0` = 固件已经留了一条全局 fault；`cb` 的 M 位是 0（我们 Program 的禁用态） |
| `A2` | `n<槽数> v<有效流数> g<GBPA>` | `v` = **固件/引导器事先挂了几条流**；`g` 低 32 位是上下文描述符表基址的低位 |
| `A3` | `0><slot> 1><slot> 5><slot>` | **关键行**：SID 0 / 1 / 5 现在分别被哪个 slot 匹配。`-1` 表示当前没有任何有效 SMR 持有该 SID |
| `A4` | `s0 <SMR>/<S2CR>` | slot 0 的原始值（我们要写的那一格） |
| `A5` | `s1 <SMR>/<S2CR>` | slot 1 的原始值 —— 如果它 `SMR` 带 `V=1` 且 SID 不是 1，说明**这个槽被别的 master 占着**，我们去改它就是在抢别人的路由 |
| `A6` | `t<TTBR0> o<首个有效 slot>` | CB0 的翻译表基址（M=0 时应为 Program 写进去的值） |
| `A7` | `ok` | 只作“读完了”标记；若整块缺失说明 `smmuAudit` 自己没跑到 |

三条互斥结论：

- **`A5` 显示 slot 1 原本有一条别的 SID 的有效流** ⇒ 卡点是我们抢了别人的槽，正解是**换一个空槽**（或用 `A2/A3` 找到 SID 1 已有的槽直接复用），不是硬写 slot 1。
- **`A5` 是 `00000000/00020000`（无效 SMR + S2CR=FAULT）而 `A3` 全是 `-1`** ⇒ SID 1 此前完全没被接上，卡点是“第一次接上时的取描述符”。正解是让 bank 0 保持 `SCTLR.M=0` 不动、**先挂一条路由跑通**（本轮镜像正是在验这一步），之后再单独处理 SID 1。
- **`A*` 整块不出现** ⇒ `NativeAdrenoGpuContext.cpp` 里的 `smmuAudit` 没被编进去（`--local-drivers` 指到了别处的 Adreno740），先查这一层再谈 SMMU。

### 18.7 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`8d18266e7e50ca18…`**（`_native_adreno_smmu_audit` 在 `fffffff007be3ea8`，`S`；`MUYU A%u %s` 在镜像内） |
| payload | 72 MiB，sha `e08f2f26…`，kernel args **996 B / 48 参**（`sheng-gpu-power=9` 在，`muyu-dump-at=20`、`offline-legacy-console-quiet=0` 在） |
| 输入 | `gpu_addon b2dddc51…`、`trust 0c559104…`（与 §17 完全一致，未换） |
| 镜像 | `fw/vendor_boot_gpu_smmuaudit.img`，77,393,920 B，**`08a94351…`**；`@0x1000` 起 75,497,472 B 与 `payload.img` **逐字节相同** |
| 缺陷修正 | §17.7 表把 `0x1?` 记成 slot 0、`0x2?` 记成 slot 1，与 `0x10+8*i+k` 不符，已改 |

---

## 19. 单路由 attach 成立：GPU 上下文 bank 0 走到 Ready；新断点是 SQE 固件

### 19.1 上机读数（`fw/vendor_boot_gpu_smmuaudit.img`，照片 `logs/2026-11-11-21-10-38-695.jpg`）

```text
MUYU LIVE p=12 t=182 btn st=3 irq=0 rep=0 msk=0x0 pon=476 gpio=-1 pdc=0x30200 err=0 sp=0 oth=0
MUYU GPU calls=3 refused=3 o=0 c=0 f=0 fb=0 tc=3 ts=00000000 hold=1 pres=1 | w=0
MUYU PROBE st=97 cp=2 b=24
MUYU A0 i4c017e10 j40000016 s00220406
MUYU A1 f00000000 cb00000167 a0000003
MUYU A2 n16 v1 g00000000
MUYU A3 0>-1 1>-1 5>2
MUYU A4 s0 00000000/00020000
MUYU A5 s1 00000000/00020000
MUYU A6 tcb517000 o00000002
MUYU A7 ok
```

### 19.2 `st=97` 是本轮的主结论

`native_gpu_power_stage = 0x90 + step`，`0x97` = **step 7 = `Step::Ready`**。上一轮同一位置是 `st=94`
（Attach 未返回）。也就是说，**只要不租第 2 条路由，`ArmSmmu4KAttachStreams()` 正常返回，
`bringUp()` 的 7 步（Allocate → Prepare → Program → Attach → VmAttach → Map → Ready）全部走完**：
bank 0 的上下文描述符、页表、11 个 GPU 缓冲区、VM 映射都已建立。

`b=24`（`observe()` 里 `scan` 之后那个落点）不再是“卡住”的证据：它只是**最后一个写面包屑的调用**
留下的值（GMU 那边的 `ShengGpuSmmuObserve` 会周期性把 0x22/0x23/0x24 重写一遍，停在 0x24 属正常）。
只有当 `st` 不再前进时，`b` 才有定位意义。

### 19.3 `MUYU A0..A7` 读出来的原始 SMMU 状态

| 行 | 值 | 含义 |
| --- | --- | --- |
| `A0` | `i4c017e10 j40000016 s00220406` | `IDR0=0x4c017e10`：**16 个 SMR 槽**、bit27=1（64 位）、bit8=0（非 EXIDS）；`IDR1=0x40000016`：CB 大小 4 KiB（bit31=0）、`(IDR1>>28)&7=4` ⇒ **CB0 在 0x20000**；`SCR0=0x00220406`：M\|GFRE\|GFIE 齐、CLIENTPD=0 |
| `A1` | `f00000000 cb00000167 a0000003` | **`GFSR=0`：从冷启动到 attach 全程没有一条全局 fault**；CB0 `SCTLR=0x167`（Program 写的）、`ACTLR=3`（引导器原值，CPRE 开） |
| `A2` | `n16 v1 g00000000` | 16 个槽里**只有 1 条有效流**；`GBPA=0`（CB 基址不走 GBPA，实际仍按 `0x20000+bank*0x1000`，由 `A6` 的 TTBR0 回读证实） |
| `A3` | `0>-1 1>-1 5>2` | **SID 0 和 SID 1 谁都没接过；SID 5 在 slot 2** —— 那是 mode 9 的 `MuyuGmuSmmu` 自己租的 GMU 槽 |
| `A4` / `A5` | `00000000/00020000` | slot 0、slot 1 原本都是**干净**的：`SMR` 无效、`S2CR=FAULT` |
| `A6` | `tcb517000 o00000002` | CB0 的 `TTBR0=0xcb517000` 正是我们 `Program` 写进去的；首个有效槽是 2 |

**由此否掉了“抢别人的槽”这一支**：slot 0/1 都是空的，SID 0/1 此前没有任何路由。所以上一轮
`b=1b` 的挂死点确实就是“**第一次把 SID 1 接到 CB0 上的那一刻**”，而不是踩到了别人的配置。
（至于为什么接 SID 0 不挂、接 SID 1 挂，`A` 行给不出答案；但这一支已经不影响推进 —— 单路由已够用，
SID 1 留给 Metal 阶段再单独处理。）

### 19.4 新断点 `cp=2`：又是“weak 符号零长度”

`cp` 是 `native_gpu_cp[0]`，`gpuStart()` 用它标阶段：`1` = 取缓冲区、`2` = 拷 SQE、
`3` = hw_init、…、`10` = `publishMetal()`。停在 `2` 且 40 多个周期不动，说明卡在 SQE 这一步。

`smmuAudit` 之外的第二个改动没有生效之前，这里的判断是：

```cpp
const uint64_t sqe = &native_adreno_sqe_firmware_image_size ? native_adreno_sqe_firmware_image_size : 0;
if (sqe < 8 || sqe - 4 > gGpuBuffers[0].bytes) {setRow(7, "C2 no sqe image"); return;}
```

`native_adreno_sqe_firmware_image_size` 在本内核里**只有 `NativeAdrenoFirmwareDefaults.cpp` 的弱默认定义，
值是 0**——和 §13 的 GMU 固件（`native_adreno_firmware_image`）是**同一个坑**：符号存在（`nm` 显示 `S`），
值却是零。于是 `sqe=0` ⇒ `< 8` ⇒ 打一行 `C2 no sqe image` 就 `return`，**一个寄存器都没写**，
`cp` 永远停在 2。

### 19.5 改动

| 文件 | 改动 |
| --- | --- |
| `UniXNU-muyu-17/scripts/gen-muyu-gmu-source.py` | 新增 `--sqe-firmware`（默认 `fw/gpu-firmware/gen71100_sqe.fw`，66,036 B，实测 < 0x13000 的 SQE 缓冲区）；把 SQE 字节写进**同一个** `build/gpu/MuyuGmuFirmware.c`（`muyu_sqe_firmware[]` + `muyu_sqe_firmware_size`），因此不需要新增任何构建步骤；头文件加 `SHENG_GPU_SQE_FIRMWARE_BYTES` 与 `sheng_gpu_sqe_firmware_sha256`；报告加 `sqe_firmware` 段 |
| `UniXNU-muyu-17/package/GpuPowerProbe.cpp` | SQE 阶段改为**优先取 `muyu_sqe_firmware`**，取不到才退回 `native_adreno_sqe_firmware_image`；失败行改成 `C2 no sqe m<有muyu> n<字节数> b<缓冲大小>`，把“没有镜像”和“镜像装不下”分开 |

保留 §18 的 `kSecondSid = UINT16_MAX`（单路由）——它是本轮能到 Ready 的直接原因。

### 19.6 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`bc101ba00bc1048f…`**；`_muyu_sqe_firmware` 在 `fffffff0070540f0`、`_muyu_sqe_firmware_size` 在 `fffffff0070642e8`（差 `0x101f8` = 66040 = 66036 + 对齐，符号与尺寸都对得上） |
| payload | 72 MiB，参数与 §18 逐字相同（996 B / 48 参） |
| 镜像 | `fw/vendor_boot_gpu_sqe.img`，77,393,920 B，**`52677ec7…`**，未刷 |

### 19.7 下机预期

`cp` 应从 `2` 继续往 `3`→…→`10` 走；若停在 `10`，说明 `publishMetal()` 之前的硬件链全过，
下一关才是 Metal 注册。若仍停在 `2`，新行会给出原因（`C2 no sqe` 后面的 `m/n/b` 三个数：
`m0` = 生成对象没进内核、`m1 n0` 不会出现、`n66028` 且 `b` 够大则是别的问题）。

---

## 20. GPU 整条命令链打通（cp=10）；新断点：publish 用了错的 input_bits，user client 全部被拒

### 20.1 上机读数（`fw/vendor_boot_gpu_sqe.img`，照片 `logs/2026-11-11-21-20-43-475.jpg`）

```text
MUYU PROBE st=97 cp=10 b=24
MUYU X2 IPI k970423 r966096 wff #3 X470 a0 p0/0
MUYU X3 L e0/1 p3/4/4 nisd b32/0 B27 Spring
MUYU X4 H124 R1 I0 F0 r32/32 f4 g1 s400 G0 q2 u0
MUYU X5 S SpringF0 x0 c0 f0 l0/0 g14 u0
MUYU X6 M a5.0/5.0 k0 s5/0 r1 a0/5/0 n3 d00:0/122
MUYU X7 K4 6800 6801 6a3c 68f2
MUYU D0 P ok ok ok skip lo0 pw11
MUYU LIVE p=13 t=190 ...
MUYU GPU calls=9 refused=9 o=0 c=0 f=0 fb=0 tc=9 ts=00000000 hold=1 pres=1 | w=0
PROC n=122 zomb=0 launchd=1R backboardd=56R SpringBoard=27R
```

### 20.2 `cp=10`：GPU 用自己的 4K 上下文执行了 PM4 并回写内存

`native_gpu_cp[0]` 的 10 是 `gpuStart()` 的最后一级闸门（`GpuPowerProbe.cpp` 的 `c[0] = c[6] ? 8 : (c[7] == 0x5e9a740 ? 10 : 9)`）：
提交一条 `CP_MEM_WRITE`（opcode 0x3d，数据 `0x5e9a740`）到 scratch 页，等 fence，然后**读回 scratch 确认 GPU 自己写进去了**。
所以这一轮确认的不只是"寄存器能读"，而是：SQE 固件已载入、`hw_init` 与 `CP init` 走完、ring attach 成功、
PM4 包被 CP 取到并在 bank 0 的页表下完成了写入与 fence 回报。**Tier-2 的 GPU 命令通路至此打通。**

`X4` 里 `r32/32` 是 `CP_RB_RPTR/WPTR` 相等 ⇒ ring 已被排空；`I0/F0` 无中断、无 HW fault；`s400` 是 bank 的 FSR（一次已终止的
translation fault 记录，全局 `G0`=GFSR 为 0，不致命）。`PROC` 行显示 `backboardd`、`SpringBoard`、`launchd` 都在跑 ⇒ **用户态是活的**，
不再有早期那种"卡在用户态"的现象，屏幕还是文字控制台只是因为显示 handoff 没被接管（`MUYU GPU o=0`）。

### 20.3 Metal 侧：插件走到了最后一步，`0x68f2` = 打不开设备

`X7` 的 `K4` 是插件（在 `backboardd` 里）打的四个 marker，按 `NativeAdrenoMetalPlugin.mm` 的定义：

| marker | 含义 |
| --- | --- |
| `0x6800` | `makeGpu(port)` 成功，插件已连上我们的 accelerator |
| `0x6801` | Turnip 库路径存在且可读 |
| `0x6a3c` | 渲染服务器的深帧缓冲判定位（`0x3c` = MobileGestalt 说支持、字典在、非 root） |
| `0x68f2` | **`native_bridge_metal_open` 对全部 6 个 shader cache 目录都失败** → `PUB no device: <nmb_last_error()>` |

没有 `0x6810+i`，所以六个目录一个都没成。`X5` 的 `o0/5/0`（opens/busy/closes）说明：**newUserClient 被拒了 5 次、放行 0 次**，
而被拒的分支只有一个 —— `mount()` 返回 false（`native_adreno_metal_diag.busy++`）。`X5` 的 `x0 c0` 也印证：连接根本没建立，
所以一个 selector 都没进来（marker 走的是 `IOServiceOpen` 的特殊 type `0x4147xxxx`，不需要连接）。

### 20.4 根因：publish 的 `input_bits` 写死成 31

`mount()` 给每个用户连接划分 GPU 地址窗口时用的是 publish 时传进来的 `input_bits`（`NativeAdrenoPublish.cpp`）：

```cpp
static uint64_t clientWindowBytes(uint32_t input_bits)
{return input_bits>32 && input_bits<=48?(UINT64_C(1)<<input_bits)-GpuClientVaStart:0;}
...
scoped = !g.next_client || !window ? kIOReturnNoResources : core->setScope(base,GpuClientVaStart,window);
```

探针传的是 `native_adreno_publish(..., 31, 40)`。`input_bits=31` ⇒ `window=0` ⇒ `kIOReturnNoResources` ⇒ **每一个用户连接都被拒**。
`31` 与另外两处硬约束互相矛盾，不是"本板真实值"：

- `NativeAdrenoSmmuInputBits(id2)` 只会返回 `{32,36,40,42,44,48}`，不可能是 31；
- `NativeAdrenoUserClient::open()`（用户态 ABI）要求 `input_bits>=32`，31 会被它当场判非法；
- `GpuClientVaStart=0x100000000`（4 GiB）本身要求 aperture 大于 4 GiB 才有位置。

也就是说这条硬编码值一直让 Metal 这条路的**第一次 open 就失败**，与 GPU 硬件无关。之前从没走到 publish（`cp` 停在 2/4），
所以这个坑一直没有暴露。

### 20.5 改动

| 文件 | 改动 |
| --- | --- |
| `UniXNU-muyu-17/package/GpuPowerProbe.cpp` | `publishAndWrapperTest()`/`userClientTest()` 的 `input_bits` 由字面量 `31` 改为 `NativeAdrenoGpuContextInputBits()`（GPU 上下文 bank 实际使用的 ias），自检的期望值同步改；`ias==0` 时打 `P no ias` 直接返回。新增导出 `unsigned muyu_gpu_bits[2]`（[0] ias、[1] 40）供控制台 dump 读 |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | 5 秒 dump 在 `MUYU PROBE` 之后加一行 `MUYU B i<ias> o<out> f<失败点> s<状态> b<窗口基址高位> n<次数>`，读 `native_adreno_mount_fail[4]`（`mount()` 里就地写入：1 无地址空间、2 无 VM、3 窗口被拒） |

### 20.6 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`1a51fc6c7476128e…`**；`_muyu_gpu_bits` 在 `fffffff007bfd3f0`；`MUYU B i%u o%u f%u s%x b%x n%u` 在镜像内 |
| payload | 72 MiB，48 参，与 §19 逐字相同 |
| 镜像 | `fw/vendor_boot_gpu_ias.img`，77,393,920 B，**`7b7a4794…`**；`@0x1000` 起 75,497,472 B 与 `payload.img` 逐字节相同 |

### 20.7 下机判读

- `MUYU B i40 o40 ...` ⇒ ias=40，客户端窗口 `2^40-4GiB`。若 `n` 仍增长、`f3 s10000006`（NoResources）则 40 这个值还不对。
- `MUYU X5` 的 `o` 应从 0 变成 ≥1；`MUYU X7` 应出现 `6810..6815`（哪个 cache 目录成功）或直接 `6802`。
- 若 `X7` 变成 `K5 ... 6802`，说明 `native_bridge_metal_open` 成立，Metal 设备已建，下一关才是渲染。

---

## 21. `input_bits` 修正生效：用户连接放行；新断点是"首个真实提交把 CP 挂住"

### 21.1 上机读数（`fw/vendor_boot_gpu_ias.img`，照片 `logs/2026-11-11-21-34-35-316.jpg`）

```text
MUYU PROBE st=97 cp=10 b=24
MUYU B i48 o40 f0 s0 b0 n0
MUYU X2 C1 cffffffff u70970001 v70970001 d0
MUYU X3 S0/0/1000000 I0 P0/0 f400 a3/301 c1c
MUYU X4 H110 Re00005 r3f/7a b7a i0/0 j0 v0/0
MUYU X5 S SpringP0 x343 c340 f7 l6/2d6 g14 u0
MUYU X6 M a5.0/5.0 k0 s5/0 r1 o5/0/4 n3 d00:0/122
MUYU X7 K4 6800 6801 6a3c 68f2
```

### 21.2 §20 的修正确认成立

`MUYU B i48 o40 f0 s0 b0 n0`：

- **ias 实际是 48**（2^48 = 256 TiB），不是 31，也不是 40。硬编码的 31 是纯粹的错值。
- `f0 s0 n0` = `native_adreno_mount_fail` 全是 0，**没有任何一次 mount 被拒**。
- `X6` 的 `o5/0/4` = opens 5 / busy 0 / closes 4（上一轮是 `o0/5/0`）：连接放行了五次、拒绝零次。
  这说明 §20 的诊断与修法都对：`input_bits=31` 让每个用户连接拿到 0 长度的地址窗口，被 `kIOReturnNoResources` 拒掉。

### 21.3 但 Metal 仍然起不来，而且现在能看出卡在哪一步

`X7` 还是 `K4 6800 6801 6a3c 68f2`。既然 `busy=0`，`0x68f2` **不再是 open 被拒**，而是失败点后移：
连接建起来了（4 次 close 里有一次就是 turnip 自己关的），死在 turnip 的 Vulkan 设备创建里。

`X5` 与 `X4`/`X3` 给出的是同一件事的三面：

| 行 | 读数 | 含义 |
| --- | --- | --- |
| `X5` | `x343 c340 f7 l6/2d6` | 835 个 selector 到达驱动、832 次调用完成，**最后一次是 selector 6 = `NativeAdrenoMethodWaitFence`，返回 `0x2d6` = `kIOReturnTimeout`**；`f7` 表示仍有方法在途 |
| `X4` | `Re00005 r3f/7a` | `CP_RB_RPTR=0x3f` 而 `CP_RB_WPTR=0x7a`：还有 59 个 dword 没被 CP 消费，ring 卡住；`RBBM_STATUS=0xe00005` |
| `X3` | `S0/0/1000000 I0 P0/0 f400` | `RBBM_STATUS3=0x1000000`（bit24，SMMU stalled）；无 CP 中断、无 protect 违规 |

注意 `f400` **不是** 故障：`NativeAdrenoGpuContext::endFault()` 明确把 `0x600` 当 AArch64 格式位掩掉，"only the fault
bits latch one"。所以 bank 的 FSR 里没有任何真正 latched 的 fault —— 上一轮把 `s400` 当 fault 读是错的。
真正的故障信息要么还没落进 FSR，要么根本不在 bank 里。

### 21.4 本轮改动：把驱动一直在记、但从没打印的东西打出来

驱动本身已经有完整的 hangcheck（每 500 ms）与 `endFault()`（读 FSR/FAR/FSYNR0、用 `CB_RESUME.TnR` 终止挂住的事务），
但这些计数器在 `native_adreno_recovery` / `native_adreno_recovery_fault` 里一直没人读。

| 文件 | 改动 |
| --- | --- |
| `UniXNU-muyu-17/package/GpuPowerProbe.cpp` | 新增导出 `char muyu_gpu_reason[64]`，在 `metalRows()` 里把 `native_adreno_metal_diag.report2` 拷出来 —— 这正是 `PUB no device: <nmb_last_error()>` 唯一的去处 |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | 5 秒 dump 新增三行，并把 SMMU 审计行从 8 条裁到 4 条（A4–A7 的原值在 §19 已定论，slot 0/1 干净），腾出屏幕 |

新行格式与含义：

| 行 | 格式 | 读什么 |
| --- | --- | --- |
| `H` | `h<hangs> r<restarts> f<failed> F<hung fence> s<IOReturn> p<replayed> g<guilty> k<kernel-VM hangs>` | hangcheck 有没有判出挂起、有没有重启、重启的返回码 |
| `F` | `n<faults> s<FSR> a<FAR lo>/<FAR hi> y<FSYNR0>` | **被 `endFault` 结束过的故障**：`y` 里有 SID，`a` 是 GPU 当时要的地址 |
| `T` | `a<aperture 可用>/<SCM x0> c<checkCpSwitch> d<最终 tableMode> w<切换状态>/<VM 建到哪一步>` | 按连接切页表的机制：`d2` = CP 在 ring 里切，`d1` = CPU 在 GPU 空闲时写 TTBR0 |
| `R` | `MUYU R <文本>` | 桥自己的最后一行，`PUB no device:` 的原因就在这 |

### 21.5 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`5f2fb10f8a1839d0…`**；`_muyu_gpu_reason` 在 `fffffff007bfd3f8`；四条新格式串均在镜像内 |
| payload | 72 MiB，48 参，与 §20 逐字相同 |
| 镜像 | `fw/vendor_boot_gpu_hang.img`，77,393,920 B，**`a190c616…`**；`@0x1000` 起 75,497,472 B 与 `payload.img` 逐字节相同 |

### 21.6 下机判读

- `MUYU R` 出现 `PUB no device: …` ⇒ 直接读到 turnip 的拒绝原因（设备创建、cache 目录、dlopen、vkCreateDevice 之类）。
- `MUYU F` 的 `n>0` ⇒ 有故障被结束过，`y` 是 SID、`a` 是 IOVA，能定位是哪个流、访问哪里。
- `MUYU H` 的 `h>0` ⇒ hangcheck 判定过挂起；`r>0` 才说明真的重启过（本轮板级没装 restart 钩子，预期 `r0`）。
- `MUYU T` 的 `d` 决定下一步方向：`d2` 说明 CP 内切换在用（那就要看 ring 里的切换包），
  `d1` 说明是 CPU 写 TTBR0（那要查切换时机与 `CB_RESUME`）。

---

## 22. 挂起原因第一手：`PUB no device: Vulkan(VkResultCode(-3))`，且 fault 从来没人结束

### 22.1 上机读数（`fw/vendor_boot_gpu_hang.img`，照片 `logs/2026-11-11-21-44-01-169.jpg`）

```text
MUYU PROBE st=97 cp=10 b=24
MUYU B i48 o40 f0 s0 b0 n0
MUYU H h45 r0 f0 F0 s0 p0 g0 k0
MUYU F n0 s0 a0/0 y0
MUYU T a0/0 c0 d1 w0/0
MUYU R PUB no device: device: Vulkan(VkResultCode(-3))
MUYU X2 C0 c0 u0 v0 d0
MUYU X3 S0/0/1000000 I0 P0/0 f400 a3/301 c1c
MUYU X4 H110 Re00005 r3f/7a b7a i0/0 j0 v0/0
MUYU X5 S SpringP0 x446 c443 f7 l6/2d6 g14 u0
MUYU X6 M a5.0/5.0 k0 s5/0 r1 o5/0/4 n3 d00:0/131
MUYU X7 K4 6800 6801 6a3c 68f2
```

### 22.2 `MUYU R` 一句定性

`native_bridge_metal_open` 的拒绝理由就是这一行：metal2vulkan 的 `Error::Vulkan(VkResultCode(-3))`，
`-3` = `VK_ERROR_INITIALIZATION_FAILED`（`UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/error.rs`）。
也就是说 **turnip 的 Vulkan 设备创建失败**，而失败原因不是参数错，是它初始化时要跑的提交完不成：
`X5` 的 `l6/2d6`（selector 6 = `WaitFence`，`0x2d6` = `kIOReturnTimeout`）。

### 22.3 挂起是“卡在 ring 上”，不是“没提交”

`X4` 在新镜像里换成 stall 版（ring 未排空时）：`Re00005 r3f/7a b7a i0/0 j0 v0/0`

- `r3f/7a`：CP 消费到 `0x3f`，WPTR 已到 `0x7a`，**59 个 dword 再没被取走**（与上一轮同一位置，说明是确定性卡点，不是偶发）
- `i0/0 j0 v0/0`：BR/BV 的 IB1 基址与剩余字数**全是 0** ⇒ CP 卡在“还没有开始执行任何 IB”的时刻，
  即它停在了 ring 上某个包上（取 IB 之前）
- `X3` 的 `S0/0/1000000`：`RBBM_STATUS3` bit24（SMMU stalled）；`f400` 里除格式位 `0x400` 外还带着 `0xf000`，
  即**确实有一条 fault 已经 latch**（上一轮把它当“非故障”读，是因为那时只有 `0x400`）
- `H h45 r0`：hangcheck 判出 45 次挂起，但 `r0` —— 一次也没重启

### 22.4 根因链条（本轮确认的那一环）

`nativeAdrenoHangcheck` 只有在 `g.restart`（板级重启钩子）存在时才调 `endFault()`：

```cpp
if(g.restart) {
    uint32_t fault[4]={};
    if(g.context && !g.context->endFault(fault) && fault[0]) {...}
    ... recover ...
}
```

而 `endFault()` 干的是两件事：读 FSR/FAR/FSYNR0，**以及写 `CB_RESUME.TnR` 终止那条挂住的事务**。
本板没装重启钩子 ⇒ `endFault` 从未执行 ⇒ **一条已经 fault 并 stall 的取数永远没人应答**，
CP 就停在原地等（`RBBM_STATUS3` bit24、RPTR 不动），`WaitFence` 只能一路超时。这解释了全部四个现象：
`h45 r0`、`F n0`、`r3f/7a` 不动、`f400`。

### 22.5 本轮改动（一处修，一处只为看清）

| 文件 | 改动 |
| --- | --- |
| `UniXNU/drivers/Adreno740/NativeAdrenoPublish.cpp` | `endFault()` 从 `if(g.restart)` 里提出来，**每次判出挂起都调**：既记下 FSR/FAR/FSYNR0（F 行从此有值），也用 `CB_RESUME.TnR` 把 stall 的事务终止，让 CP 看到错误响应而不是无限等待。这是修，不只是探针 |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | 新增 `MUYU Z0` / `MUYU Z1`，打印 `NativeAdrenoSubmit::noteStall()` **早就在采、却从没人读**的那份快照（`native_adreno_stall` / `native_adreno_stall_mem`）；为腾行把 SMMU 审计从 4 行裁到 2 行（A2/A3 在 §19 已定论） |

`Z` 两行的含义：

| 行 | 格式 | 读什么 |
| --- | --- | --- |
| `Z0` | `t<超时次数> f<fence> s<FSR> a<FAR 低>/<FAR 高> y<FSYNR0> u<TLB 失效+kick 次数>/<排空调度次数>` | **`a` 就是 GPU 当时要的地址**；`y` 里带 SID；`u` 说明驱动自己已经试过哪些“解开”手段 |
| `Z1` | `r<RPTR>/<WPTR> b<BR IB1 基址低>/<剩余 dword> w<IB 第一个字> h<BR IB1 句柄>/<BV IB1 句柄>/<BV IB2 句柄>` | `h` 是 `handleAt()` 在**该 job 自己的地址空间**里查这三条 IB 的结果，**`ffffffff` = 这个地址在那个进程的页表里根本没映射** —— 那就等于直接指出 fault 的来源 |

### 22.6 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`f65765c0547c8b4d…`**；`_native_adreno_stall` @ `fffffff007bf6eb4`、`_native_adreno_stall_mem` @ `fffffff007bf6f14`、`_native_adreno_unstick` @ `fffffff007bf7020`；两条 `MUYU Z*` 格式串在镜像内 |
| payload | 72 MiB，**996 B / 48 参**，与 §21 逐字相同 |
| 镜像 | `fw/vendor_boot_gpu_stallz.img`，77,393,920 B，**`0a7c85a9…`**；`@0x1000` 起 75,497,472 B 与 `payload.img` 逐字节相同 |

### 22.7 下机判读（两条互斥结论）

- `Z1` 的 `h` 出现 `ffffffff` ⇒ **那条 IB 的 IOVA 不在客户端页表里**，是映射问题；
  接着看 `Z0` 的 `a` 是否正好等于那条 IB 的地址来确认，修法是让 `BoMap`/提交前把 IB 映射进该进程的 VM。
- `h` 三个都是正常索引、`Z0` 的 `a` 落在 `0x100000000` 以上（客户端 arena）而 `s`=译码 fault ⇒
  **表切了但页表里那条映射的权限/层级不对**，下一步查 `NativeAdrenoPrepareSmmuMemory` 给客户端 arena 建的层级。
- 若 `Z0` 的 `t` 一直增、`s` 却变成 `0` ⇒ 说明 `endFault` 的 `TnR` 已经在生效、fault 被反复清掉，
  那么 `r` 是否开始推进（`X4` 的 `H` 值不再冻结）就是新判据。

---

## 23. stall 快照到手：CP 卡在**取 ring 本身**（IB 一条都没开始），且 CB0 无 fault

### 23.1 上机读数（`fw/vendor_boot_gpu_stallz.img`，照片 `logs/2026-11-11-22-00-02-730.jpg`）

```text
MUYU PROBE st=97 cp=10 b=24
MUYU B i48 o40 f0 s0 b0 n0
MUYU H h35 r0 f0 F0 s0 p0 g0 k0
MUYU F n0 s0 a0/0 y0
MUYU Z0 t20 f5 s400 a73b7dfbf/1ffee y170d7b u1/1
MUYU Z1 r3f/7a b0/0 wfffff000 h0/0/0
MUYU T a0/0 c0 d1 w0/0
MUYU R PUB no device: device: Vulkan(VkResultCode(-3))
MUYU X2 C1 cffffffff u0 v0 d0
MUYU X3 S0/0/1000000 I0 P0/0 f400 a3/301 c1c
MUYU X4 H112 Re00005 r3f/7a b7a i0/0 j0 v0/0
MUYU X5 S SpringP0 x384 c381 f7 l6/2d6 g14 u0
MUYU X6 M a5.0/5.0 k0 s5/0 r1 o5/0/4 n3 d00:0/127
MUYU X7 K4 6800 6801 6a3c 68f2
```

### 23.2 `Z1` 给出的第一手结论：CP 停在**取 ring**，不是取 IB

`Z1` 的字段按 `NativeAdrenoRing::stallState` 的寄存器表解码（`stall[2+i]` 是 `words[i]`）：

| 字段 | 值 | 寄存器 | 含义 |
| --- | --- | --- | --- |
| `r` | `3f/7a` | `CP_RB_RPTR/WPTR` | 59 个 dword 未消费，与 §21/§22 **同一位置**（确定性卡点） |
| `b` | `0/0` | `CP_IB1_BASE_LO/HI` | **BR 的 IB1 基址是 0** ⇒ CP 一个 IB 都还没开始取 |
| `w` | `fffff000` | 该地址处读到的第一个 dword | 与 `b=0` 一致：没有 IB 可取，读到的是无效内容 |
| `h` | `0/0/0` | `handleAt(BR IB1 / BV IB1 / BV IB2)` | 三条 IB 都不存在（因为基址是 0） |

结合 `i0/0 j0 v0/0`（X4 里 BR/BV 的 IB1 基址与剩余字数全 0），可以确定：**CP 连第一条取指包都没开始执行，
它卡在从 ring 读下一个 dword 这一步**。也就是说卡点不在 PM4 包语义、不在 IB 映射，而在 ring 页本身的可达性。

### 23.3 `Z0` 说明 CB0 里**没有 latched fault**

`Z0 t20 f5 s400 … u1/1`：

- `t20` 20 次超时、`f5` 5 个不同的 fence 卡过 —— 与 `h35`（hangcheck 判 35 次）、`X5` 的 `l6/2d6` 同一件事
- `s400` = CB0 的 FSR = `0x400`。按 `endFault` 的口径，`0x600` 是 AArch64 格式位，**剩下的 fault code 是 0** ⇒
  卡住的这次访问**没有在 CB0 里留下 fault**
- `a73b7dfbf/1ffee`、`y170d7b` 是 FAR / FSYNR0 的残留：FSR 没有 fault code 时这两个寄存器保留**上一次**fault 的值，
  所以它们描述的不是当前这次卡住（而且 FAR 合成后 `0x1FFEE73B7DFBF` 超过 48 位，落在 GPU 地址空间之外）
- `u1/1`：`unstick` 的两个阶段各只跑了 **1 次**。因为 `stage_due()` 要求 `now` 非零，20 次超时里只有一次拿到时钟，
  其余都没进"先 TLBIALL+kick、再 drainBus"两级 —— 自动解卡基本没生效

`F n0`（`endFault` 从未记下 fault）与 `s400` 一致：`endFault` 每次都调到了（35 次挂起），但每次都读到无 fault 的 FSR 就返回了。

### 23.4 `T` 行：整条路走的是"CPU 写 TTBR0"（mode 1），而 SCM aperture 从没被问过

`T a0/0 c0 d1 w0/0`：

- `a0/0` = `native_adreno_cp_aperture[0]=0`、`[1]=0` ⇒ **`qcom_scm_set_gpu_smmu_aperture` 从来没有被调用过**
- `d1` = `tableMode()` 因此恒为 1：每次换进程地址空间，都是 CPU 在 GPU 空闲时写 TTBR0
- 而 `UniXNU-uke/package/GpuPowerProbe.cpp` 里本来就有 `openCpAperture()`（先 `IS_CALL_AVAIL`、再
  `SVC_MP 0x0c / CP_SMMU_APERTURE_ID 0x1b`），muyu-17 的探针一直没搬过来 —— 于是 CP 侧切表（mode 2，
  `CP_SMMU_TABLE_UPDATE`，a740 SQE 里带 CACHE_FLUSH7 的那种）这条路从未被打开

这也解释了为什么一次客户端提交就把 CP 冻住：客户端的第一条提交正好是**第一次**换表。换表之后 CP 取 ring 卡住、
`RBBM_STATUS3` bit24（SMMU stalled）立住、而 CB0 里没有 fault —— 形态完全符合"换表本身没让 CP 看到一份可用的表"。

### 23.5 本轮改动（一修 + 两处看清）

| 文件 | 改动 |
| --- | --- |
| `UniXNU-muyu-17/package/GpuPowerProbe.cpp` | 新增 `openCpAperture()`（从 uke 那一份搬过来），在 `native_adreno_publish()` **之前**调用 —— `tableMode()` 在 publish 里读 `cp_aperture`，只有 aperture 应答了才会选 mode 2。`scmCall` 补上第四个参数（`ARGS(4)` 需要 x5），默认 0，原调用点行为不变 |
| `UniXNU/drivers/Adreno740/NativeAdrenoGpuContext.cpp` | 新增两个导出：`NativeAdrenoGpuContextReadTtbr0()`（读 bank **当前**的 TTBR0）与 `NativeAdrenoGpuContextKernelTtbr0()`（内核地址空间自己的 TTBR0）。TTBR0 高 16 位是 ASID，低 32 位是根表物理页，两者一比就知道 GPU 此刻在用哪张表 |
| `UniXNU/drivers/Adreno740/NativeAdrenoSubmit.cpp` | `noteStall()` 多记一格 `stall_mem[54] = space->handleAt(GpuRingIova)`：卡住那个 job **自己的地址空间**里，ring 页（`0x200000`）到底映射了没有 |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | `T` 行扩成 `a…/… c… d… w…/… s<CPU 写 TTBR0 次数>/<CP 消费的切表次数> g<bank TTBR0 ASID:根页> k<内核表 ASID:根页>`；`Z1` 末尾加 `R<ring 句柄>` |

### 23.6 新行判读

| 读数 | 结论 |
| --- | --- |
| `T a<非零>/0 d2` | SCM aperture 成立，**CP 自己切表**（mode 2）已启用；`publish` 里的 `checkCpSwitch` 还会当场验一次（`c0` 表示通过） |
| `T a80000000… d1` | aperture 调用不可用（或返回错），只能留在 mode 1；那就得在 CPU 侧补 TLB 失效/换表时序 |
| `T d2` 但 `X5` 的 `l6` 变成别的 selector、`Z1` 的 `r` 开始推进 | 换表这条路通了，CP 能吃 ring，进到下一个断点（大概率是 IB/BO 映射） |
| `Z1 … R0` 或 `Rffffffff` | ring 页在**该进程自己的表**里没有映射 —— 那 mode 1 的换表把 CP 的取指地址指到了空处，问题就在 `createVm` 的 ring 映射/权限 |
| `Z1 … R<正常句柄>` 而 `T g==k` | 换表根本没装上去（bank 还在用内核表），卡点在别处 |
| `T s…/<非零>` | CPU 写了多少次 TTBR0 / CP 消费了多少次切表包，用来判断上面的组合是否自洽 |

### 23.7 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`41bde4b1eb04e40f…`**；`_NativeAdrenoGpuContextReadTtbr0` @ `fffffff0079f84c4`、`_NativeAdrenoGpuContextKernelTtbr0` @ `fffffff0079f8544`；两条新格式串在镜像内 |
| payload | 72 MiB，kernel args 与 §22 逐字相同（`sheng-gpu-power=9` 在） |
| 镜像 | `fw/vendor_boot_gpu_aperture.img`，77,393,920 B，**`4f9c1719…`**；`@0x1000` 起 75,497,472 B 与 `payload.img` 逐字节相同 |

## 24. CP 侧切表（mode 2）真的成立：`a1/0` + `d2` + CPU 零次写 TTBR0；新断点收窄到「切表包之后的那一个包」

### 24.1 上机读数（`fw/vendor_boot_gpu_aperture.img`，照片 `logs/2026-11-11-22-14-45-623.jpg`，t≈150 s）

```text
MUYU PROBE st=97 cp=10 b=24
MUYU B i48 o40 f0 s0 b0 n0
MUYU H h231 r0 f0 F0 s0 p0 g0 k0
MUYU F n0 s0 a0/0 y0
MUYU Z0 t2 f5 s400 a73b7dfbf/1ffee y170d7b u1/1
MUYU Z1 r43/81 b0/0 wffff0000 h0/0/0 R2
MUYU T a1/0 ce00002d6 d2 w0/0 s0/2 g2/cb7df000 k2/cb7df000
MUYU R PUB no device: Vulkan(VkResultCode(-3))
```

周围同屏：`GPU calls=6 refused=6`、`PROC n=127 zomb=0 launchd=1R backboardd=56R SpringBoard=27R liveact=0-`、
`NATIVE-NVT frames=9000 contacts=1249 published=1249 errors=0 hld=1252 slnk=1`。

> 读图提示：这张照片 **不需要旋转**（3840×2160 直接可读），此前几轮「旋转 180°」的做法在这张上会读错。
> `sips -r` 对这张 OPPO 的原图不生效（无 EXIF rotate 标签但会整帧重采样），
> `ffmpeg -noautorotate` 原样缩放才是对的。

### 24.2 三件此前从未同时成立的事

| 读数 | 含义 |
| --- | --- |
| `T a1/0` | `qcom_scm_set_gpu_smmu_aperture` **应答了**，`avail.a0=0`（无错）、`x0=0`（成功）。§23 是 `a0/0` —— 那时这个调用根本没发生 |
| `T d2` | `tableMode()==2`：换进程地址空间由 **CP 在 ring 内自己切**（`CP_SMMU_TABLE_UPDATE`），不再由 CPU 写 `TTBR0` |
| `T s0/2` | CPU 写 `TTBR0` **0 次**、CP 消费切表 **2 次** —— `s0` 是强证据：mode 2 不是「选了但回退」，而是真的全程走 CP |

这三条把 §23.4 那条推断链（「aperture 从没被问过 ⇒ 只能 mode 1 ⇒ 首次换表把 CP 冻住」）**整条否掉**。
换表这条路本身是通的。

### 24.3 `c=e00002d6` 是 `kIOReturnTimeout`，定位到 `waitAll`

`checkCpSwitch()` 的五步各自写 `native_adreno_cp_check[0..4]`：`createVm`、`probeBuffer`、`submit`、
`waitAll(2 s)`、`readTtbr0`。返回值 `0xe00002d6` 在 `IOReturn.h` 里是 **`kIOReturnTimeout`**（`0x2d6`）。

`0x2d6` 只可能来自 `g.submit->waitAll(&g.clock,2000000)` —— 也就是说：`createVm` 建表成功、
`probeBuffer` 映射成功、那条 NOP 提交也进了 ring，**但 fence 在 2 秒内没有被 CP 写完**。
`readTtbr0` 因此根本没跑，`seen != ttbr0` 那条 `kIOReturnUnsupported` 也不是它。

`publish` 里 fallback 到 mode 1 的条件是 `checked && !submit->outstanding()`；这条提交还挂着，
所以 `outstanding()` 非零，**没有回退**，最终 `d2` 保留 —— 这与 `T` 行的 `d2` 自洽。

### 24.4 CP 吃掉了切表包，然后停在那一个包上

| | §23（mode 1） | 本轮（mode 2） |
| --- | --- | --- |
| `Z1` 的 `r`（`CP_RB_RPTR/WPTR`） | `3f/7a`（63 / 122） | **`43/81`**（67 / 129） |

两轮 `WPTR` 差 7、`RPTR` 差 4。`CP_SMMU_TABLE_UPDATE` 是 **1 个 header + 4 个 payload = 5 dwords**，
实测差 **4** —— 说明 CP 把这 4 个 payload dword 读掉之后，**下一个 dword 就不再读了**。

`b0/0`（`CP_IB1_BASE_LO/HI`）全 0 ⇒ 一个 IB 都还没开始取。所以卡点在切表包**紧后面**的那个包，
不是 PM4 包语义、不是 IB 映射。

### 24.5 `R2` 与 `g==k` 同时出现 —— 排除「ring 没映射」

- `R2` = `stall_mem[54]` = `space->handleAt(GpuRingIova)`，**在这个 job 自己的地址空间里** ring 页映射的句柄是 2。
  `createVm` 用 `buffers_[i]`（内核同一份内存对象）直接 `map()` 到同一个 `kIovas[i]`，
  所以新表里 ring 的 IOVA 与物理页都与内核表一致 —— 「切过去之后 ring 指向别的物理页」这个假设不成立。
- `g2/cb7df000` 与 `k2/cb7df000` 完全相同 ⇒ 采样这一刻 bank 正在用**内核表**（ASID 2）。
  空闲回退或切表未保留，两种都可能，但都不影响 24.4 的结论。

两者合起来只剩一个方向：**表切过去了，CP 却没能继续在那张表上取指**。

### 24.6 本轮改动（纯诊断，不动逻辑）

| 文件 | 改动 |
| --- | --- |
| `UniXNU/drivers/Adreno740/NativeAdrenoRing.h` / `.cpp` | 新增 `peekRing(at, back, count, out)`：从 CPU 映射里按环绕下标拷 ring 的 dwords。卡住的 CP 不会消费，所以 `RPTR` 之后的内容**就是它拒绝取的那条包**，这是区分「页不可达」与「内容已不是包」的唯一办法 |
| `UniXNU/drivers/Adreno740/NativeAdrenoSubmit.cpp` | `noteStall()` 填 `native_adreno_stall_ring[48]`（`RPTR-8` 起 40 个 dword）与 `native_adreno_stall_vm[2]`（该 job 的 `TTBR0` 高/低） |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | 新增 `Z2`（4 行 × 12 dword，`Z2 00`/`12`/`24`/`36`，**下标 8 就是 RPTR 本身**）、`Z3 v<高>/<低>`；新增 `W k<cp_check[0]> <[1]> <[2]> <[3]> <[4]> f<fence>` |

### 24.7 下一轮判读表（`fw/vendor_boot_gpu_ringdump.img`）

| 读数 | 结论 |
| --- | --- |
| `W k0 0 e0000…` | `submit` 就失败（`useVm`/`emitPrologue` 被 poison），不用再往下看 ring |
| `W k0 0 0 e00002d6` | `waitAll` 超时 —— 24.3 已定的形态；此时 `Z2` 才是主证据 |
| `Z2` 下标 8 处是一个合法 PM4 type-7 header（如 `c0032100`、`c0024d00` 一类） | ring 内存里有**合法包**，CP 却不取 ⇒ 取指/转译层，方向转 SMMU（stall 而非 fault） |
| `Z2` 下标 8 处全 0 或明显非包内容 | ring 内容被覆盖 / 表把该 IOVA 指到别的内存 ⇒ 方向在 `createVm` 的映射 |
| `Z3 v10/…` | job 的表 ASID = 16（`createVm` 从 16 起分配），根页为这个值；与 `FAR`/`R2` 一起定位 |
| `Z2` 在切表包之后突然全 `00000000` | 切表包本身把 ring 写坏了（`emitPrologue` 的越界/对齐） |

### 24.8 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`a61e984a8dcffd7c059a3a4144ecef18b2ce5b203efdf818f83a21c94ba81065`**；`__ZNK16NativeAdrenoRing8peekRingEjjjPVj` @ `fffffff007a005c0` |
| payload | 72 MiB（75,497,472 B），sha256 见 `UniXNU/out/muyu/payload/payload.img`；kernel args 与 §23 逐字相同（`sheng-gpu-power=9`、`sheng-fb-gpu=1`、`sheng-gpu-addon=1`、`sheng-gpu-metal=1` 均在） |
| 镜像 | `fw/vendor_boot_gpu_ringdump.img`，77,393,920 B，**`f249896c40c93664b687526526f1a7ded05c128113dfd6c5a7ed5f0dc333eb2c`**；`@0x1000` 起 75,497,472 B 与 `payload.img` **逐字节相同** |
| 三条新格式串 | `MUYU Z2 %02u`、`MUYU Z3 v%x`、`MUYU W k%x` 均在镜像内（`strings` 已验） |

### 24.9 构建与打包的可复现命令（此前只存在于会话里，这里固化）

```bash
cd /Users/diannao/pad7pro-xnu

# 1) 内核
python3 UniXNU/unixnu build kernel -D UniXNU-muyu-17 \
  --uefi /Users/diannao/pad7pro-xnu/Mu-Silicium \
  --edk2 /Users/diannao/pad7pro-xnu/Mu-Silicium/Mu_Basecore -j 8

# 2) payload（五个参数缺一不可）
python3 UniXNU/unixnu build payload -D UniXNU-muyu-17 -j 8 \
  --data-ramdisk fw/data-seed.img \
  --owner-trust-cache fw/trust-21H461/owner-trust-17-full.bundle \
  --gpu-addon fw/gpuaddon-17-full.bin \
  --unique-chip-id 0000000000000001 \
  --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-addon=1 --kernel-arg sheng-gpu-metal=1

# 3) vendor_boot（base 必须是零填充到 96 MiB 的 nofs1，否则 72 MiB payload 过不了分区上限检查）
cp fw/vendor_boot_nofs1.img /tmp/base96.img && truncate -s 100663296 /tmp/base96.img
python3 UniXNU-muyu-17/scripts/patch-vendor-boot.py \
  --base /tmp/base96.img --payload UniXNU/out/muyu/payload/payload.img \
  --output fw/vendor_boot_gpu_ringdump.img
```

> `--gpu-addon` 要的是 `scripts/make-gpu-addon.py` 产出的 **blob**（`fw/gpuaddon-17-full.bin`），
> 不是 `UniXNU/build/adreno-metal-plugin/NativeAdrenoMetal.bundle` 目录 —— 传目录会被校验拒掉。

## 25. ring 里根本没有「坏的切表包」：CP 停在 `CP_PERFCTR_SRAM_INIT` 那一格；`s` 的第二个数是软件计数，§24.2 的推断作废

### 25.1 上机读数（`fw/vendor_boot_gpu_ringdump.img`，照片 `logs/2026-11-11-22-36-16-639.jpg`，t≈40 s）

```text
MUYU PROBE st=97 cp=10 b=24
MUYU B i48 o40 f0 s0 b0 n0
MUYU H h231 r0 f0 F0 s0 p0 g0 k0
MUYU F n0 s0 a0/0 y0
MUYU Z0 t2 f5 s400 a73b7dfbf/1ffee y170d7b u1/1
MUYU Z1 r43/81 b0/0 wffff0000 h0/0/0 R2
MUYU T a1/0 ce00002d6 d2 w0/0 s0/2 g2/cb4e9000 k2/cb4e9000
MUYU W k0 0 0 e00002d6 ?? f??
MUYU Z2 00 701f0001 0000000f 70970001 80000003 70460001 00000026 70970001 80000001 40050e81 00000001 78d30004 cb546000
MUYU Z2 12 00010000 00000000 00000000 70970001 80000001 70460001 00000031 703c0006 00000003 0000050f 00000000 00000001
MUYU Z2 24 00000001 00000000 70460001 00000032 70460001 00000033 70260000 70970001 80000003 70c50001 00000101 70bf0003
MUYU Z2 36 00302f00 00000000 00000001 40008581 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000
MUYU Z3 v10000/cb546000
MUYU R PUB no device: Vulkan(VkResultCode(-3))
```

`Z2 36` 只有前 4 个 dword 有值，其余 7 个是 0 —— 与 `peekRing(...,40,...)` 只填 40 个、`stall_ring[48]` 后 8 个保留 0 完全吻合，反证 dump 起点确实是 `RPTR-8`。

### 25.2 48 个 dword 逐包拆开，与 `emitPrologue()` 的 reset 分支一一对上

`peekRing(at,back,count)` 里 `out[i]=ring[(at-back+i)&mask]`，所以 **`Z2 00` 的下标 8 就是 `CP_RB_RPTR` 本身**。

| 下标 | dwords | 包 | 代码来源 |
| --- | --- | --- | --- |
| 0–1 | `701f0001 0000000f` | `CP_RESET_CONTEXT_STATE = 0xf` | `packet(words,n,RingResetContextState,&all,1)` |
| 2–3 | `70970001 80000003` | `CP_THREAD_CONTROL = THREAD_BOTH\|SYNC` | `packet(...,&syncBoth,1)` |
| 4–5 | `70460001 00000026` | `CP_EVENT_WRITE7 LRZ_FLUSH_INVALIDATE` | `packet(...,RingFenceOpcode,&lrz,1)` |
| 6–7 | `70970001 80000001` | `CP_THREAD_CONTROL = THREAD_BR\|SYNC` | `packet(...,&br,1)` |
| **8–9** | **`40050e81 00000001`** | **`CP_PERFCTR_SRAM_INIT (0x50e) = 1`** | **`AdrenoPM4Type4(RingPerfctrSramInit,1,...)`** ← **RPTR = 67 停在这里** |
| 10–14 | `78d30004 cb546000 00010000 0 0` | **`CP_SMMU_TABLE_UPDATE (0x53)`，根页 `0xcb546000`，ASID 0x10000** | `packet(...,RingSmmuTableUpdate,update,4)` |
| 15–16 | `70970001 80000001` | `CP_THREAD_CONTROL BR\|SYNC` | `packet(...,&br,1)` |
| 17–18 | `70460001 00000031` | `CP_EVENT_WRITE7 CACHE_INVALIDATE` | `packet(...,&invalidate,1)` |
| 19–25 | `703c0006 3 0x50f 0 1 1 0` | `CP_WAIT_REG_MEM` 等 `0x50f == 1` | `packet(...,RingWaitRegMem,sram,6)` |
| 26–27 | `70460001 00000032` | `CACHE_FLUSH7` | prologue 公共段 |
| 28–29 | `70460001 00000033` | `CACHE_INVALIDATE7` | 同上 |
| 30 | `70260000` | `CP_WAIT_FOR_IDLE` | 同上 |
| 31–32 | `70970001 80000003` | `THREAD_BOTH`（`both`） | 同上 |
| 33–34 | `70c50001 00000101` | `CP_SET_MARKER = 0x101`（IFPC disable） | `packet(...,RingSetMarker,&marker,1)` |
| 35–38 | `70bf0003 00302f00 0 1` | 下一提交的开始 | `stamps_` 段 |
| 39 | `40008581` | 下一提交里的另一个 type-4 | — |

**结论一：ring 内容 100% 正确，一个坏包都没有。** 切表包的 payload 与 `Z3 v10000/cb546000` 逐字相同 —— 表和 ASID 都对，包也在 ring 里，只是 **CP 永远不会 fetch 它**。

**结论二：`RPTR = 0x43 = 67` 正好落在 perfctr 包的第一个 dword 上。** 也就是说 CP 取指到第 67 个 dword 就停住，`78d30004` 那个切表包头一个字节都没被读。`b0/0`（`CP_IB1_BASE` 全 0）与此一致：本次提交没有 IB，纯 inline 包。

### 25.3 `T s0/2` 的第二个数是**软件计数**，§24.2 的推断必须撤回

`NativeAdrenoRing.cpp:621`：

```cpp
if(table) {table_pending_=false;native_adreno_table_switches[0]=native_adreno_table_switches[0]+1;}
```

`[0]` 是在 **emit 切表包的那一刻**由 CPU 加的，不是 CP 消费的次数。`[1]` 才是真证据（`NativeAdrenoSubmit.cpp:140`，CPU 写 `TTBR0` 的处数）。

| 读数 | 真实含义 | §24 当时的误读 |
| --- | --- | --- |
| `s0` | CPU 一次都没写 `TTBR0` | ✅ 正确，仍然成立 |
| `s2` | **驱动往 ring 里放进了 2 个切表包** | ❌ 曾读成「CP 消费切表 2 次」 |

所以 §24.2 那条「`s0` 是强证据、mode 2 全程走 CP」要收窄为：**驱动选择了 mode 2 并且确实没走 CPU 那条路，但 CP 一个切表包都没执行过**。`d2` 只是「选择了 mode 2」。

### 25.4 于是 §18–§24 的所有读数第一次自洽

- `a1/0` + `d2` = aperture 通了、驱动决定走 CP 切表 → 因此 `s0`。
- 切表包**只在 reset 提交里**（`if(table && !reset) return poison()`）。
- reset 提交在 perfctr 包处被卡住 → 切表包永远不执行 → CP 始终用旧表（内核表，`g2/cb4e9000 == k2/cb4e9000`）。
- 客户端提交的表从没装进去 → fence 永不写 → `checkCpSwitch` 的 `waitAll(2 s)` 超时 `e00002d6` → Turnip `VkResultCode(-3)`。

这也解释了为什么 `cp=10`（§22 的 CP_MEM_WRITE 自测）能过：那是**非 reset 提交**，prologue 里不含 perfctr 包。

### 25.5 本轮改动：perfctr 两个包换成**等长 NOP**

| 位置 | 原 | 现 |
| --- | --- | --- |
| perfctr type-4（2 dword） | `40050e81 00000001` | `packet(...,RingNop,zero1,1)` → `70108001 00000000` |
| `CP_WAIT_REG_MEM`（7 dword） | `703c0006 3 50f 0 1 1 0` | `packet(...,RingNop,zero6,6)` → `70100006 0 0 0 0 0 0` |

用 `static constexpr bool kPerfctrInit=false` 开关，两包**长度不变**，`words[]` 与 `n!=count` 校验全不受影响，两条 ring 的唯一差别就是「有没有写 0x50e」。

新常量 `RingNop = 0x10`（`CP_NOP`）。

### 25.6 下一轮判读表（`fw/vendor_boot_gpu_noperfctr.img`）

| `Z2 00` 下标 8 | `Z1 r` | 结论 |
| --- | --- | --- |
| `70108001`（NOP）且 **RPTR 仍 = 67** | `43/…` | 卡点不在 perfctr，而在它**前面**的 `CP_THREAD_CONTROL BR\|SYNC`（等 BR 线程），下一步清单线程控制 |
| `70108001` 且 RPTR 越过 69、停在下标 10 | `45/…` | 卡在切表包本身，方向转 `CP_SMMU_TABLE_UPDATE` 的 payload 语义 / SQE 固件处理 |
| RPTR 越过 19（`703c0006`）、fence 仍不写 | `4c/…` | 卡在 `CP_WAIT_REG_MEM` 等 `0x50f`，即状态位地址或语义不对（此时 perfctr 确实被跳过，等待条件永不满足） |
| `W` 的 `[3]` 变 0、`T c0`、`R` 行不是 `VkResultCode(-3)` | — | 通了，直接进下一断点（IB/BO 映射） |

### 25.7 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`3deca1bdc2ee0adfa48775ac6fe8b04fcede59e50e4fba622d78e3a5b66e042a`** |
| payload | 75,497,472 B（72 MiB） |
| 镜像 | `fw/vendor_boot_gpu_noperfctr.img`，77,393,920 B，**`774afb581624f46143eca06198723369eeba6701808e0fc92f3fad31b9a384c5`**；`@0x1000` 起 75,497,472 B 与 `payload.img` 逐字节相同（已验 `True`） |

### 25.8 读图方法（这一轮踩到的坑）

`MUYU Z2` 那四行每行 12 个 8 位十六进制数，照片上单字宽约 250 px，**最高位 `8`/`0`、`3c`/`c0`、`50f`/`53f` 都读错过**。正确做法不是整行 OCR，而是：
1. 先按 `Z2 00` 行的横向节距（≈260 px/dword）算每一格的位置；
2. `ffmpeg -noautorotate -vf crop=WxH:x:y,scale=…:flags=lanczos` 单独裁一格；
3. 读出来的值必须能按 `AdrenoPM4Type7/Type4` 的编码自洽（`0x70000000|count|odd_bit(count)<<15|opcode<<16`、`0x40000000|count|odd_bit(count)<<7|reg<<8|odd_bit(reg)<<27`），不自洽就说明读错了。

本轮 `40050e81` 一开始被读成 `4005ba01`、`703c0006` 被读成 `70c00006`，都是靠这条自洽性纠正回来的。

## 26. 把 perfctr 换成等长 NOP 后 RPTR **一格没动**：卡点在它前面的 reset 序列，`Z2` 的下标不等于执行位置

### 26.1 上机读数（`fw/vendor_boot_gpu_noperfctr.img`，照片 `logs/2026-11-11-22-49-02-537.jpg`，t≈40 s）

```text
MUYU Z0 t4 f5 s400 a73b7dfbf/1ffee y170d7b u1/1
MUYU Z1 r43/81 b0/0 wffff0000 h0/0/0 R2
MUYU Z2 00 701f0001 0000000f 70970001 00000003 70460001 00000026 70970001 00000001 70108001 00000000 78d30004 cb5e0000
MUYU Z2 12 00010000 00000000 00000000 70970001 00000001 70460001 00000031 70100006 00000000 00000000 00000000
MUYU Z2 24 00000000 00000000 70460001 00000032 70460001 00000033 70260000 70970001 00000003 70c50001 00000101 70bf0003
MUYU Z2 36 00302f00 00000000 00000001 40008501 00000000 00000000 00000000 00000000 00000000 00000000 00000000
MUYU Z3 v10000/cb5e0000
MUYU T a1/0 ce00002d6 d2 w0/0 s0/2 g2/cc7be000 k2/cc7be000
MUYU W k0 0 0 e00002d6 ffffffff f5
MUYU R PUB no device: Vulkan(VkResultCode(-3))
```

### 26.2 swap 确实生效了，但 CP 没往前走

| | §25（perfctr 存在） | §26（perfctr → NOP） |
| --- | --- | --- |
| 下标 8–9 | `40050e81 00000001`（type-4 写 0x50e） | **`70108001 00000000`**（`CP_NOP`） |
| 下标 19–25 | `703c0006 3 50f 0 1 1 0` | **`70100006 0 0 0 0 0 0`**（`CP_NOP`） |
| `Z1 r` | `43/81`（67 / 129） | **`43/81`（完全相同）** |
| `T c` | `e00002d6` | `e00002d6` |

两处 NOP 都在，包长一 dword 没变，切表包的位置也没变（仍从下标 10 开始，payload `cb5e0000` 与 `Z3 v10000/cb5e0000` 逐字一致）。

**而 `CP_RB_RPTR` 一个 dword 都没前进。** 一条不含 perfctr 的 ring 和一条含 perfctr 的 ring 停在同一位置 —— 这就把 perfctr 从嫌疑名单里划掉了。

### 26.3 重要的方法论修正：`Z2` 的下标不是「CP 执行到哪」

`CP_RB_RPTR` 是 **fetch 指针**，不是执行指针。CP 有 fetch/执行两级流水线：执行停在某个等待包上时，fetch 会继续预取十几到几十个 dword 才停住。所以：

- §25 里「RPTR 停在下标 8 ⇒ CP 卡在 perfctr 包」这个结论**过强**，实际只能说「fetch 到下标 8 为止」。
- 本轮 NOP 证明后半句是错的：真正卡住的是**执行**，而执行位置在 fetch 指针**之前**。

`Z1 b0/0`（`CP_IB1_BASE` 全 0）仍然有效：一个 IB 都没开始取，卡点确实在 prologue 内部。

### 26.4 嫌疑收敛到 reset 段的三个包

第二次 reset 提交的布局（`Z2` 从 `RPTR-8` 起，即 ring[59]）：

| 下标 | 包 | 可能让 CP 停住的理由 |
| --- | --- | --- |
| 0–1 | `CP_RESET_CONTEXT_STATE = 0xf` | 0xf 是「全部上下文状态」，某些位会让 CP 进入 deferred 状态等线程 |
| 2–3 | `CP_THREAD_CONTROL = BOTH\|SYNC` | `SYNC` 位要求 BR/BV 线程都到达同步点，只要一个线程没被调起就永久等 |
| 6–7 | `CP_THREAD_CONTROL = BR\|SYNC` | 同上，且只启用 BR |
| **（下标 0 之前，未 dump）** | **`CP_WAIT_TIMESTAMP`，等 `fence_iova_+4 == seqno_`** | `seqno_` 是**上一次**提交的序号；上一次的 fence 没写进去就永远等不到 |

最后一条第一次变得可疑：`emitPrologue` 用的是 `seqno_`（尚未递增的旧值），而 `fence_[1]` 正是上一次完成号（`NativeAdrenoRing.cpp:496/646/664` 读它）。第一次 reset 提交时 `seqno_=0`、`fence_[1]` 初值 0 立即满足；**第二次就要等第一次真的把 `fence_[1]` 写成 1**。

### 26.5 本轮改动（只加观测，不改行为）

| 文件 | 改动 |
| --- | --- |
| `UniXNU/drivers/Adreno740/NativeAdrenoRing.h` | 新增 `fenceWords()`，把 fence 页首字暴露给诊断 |
| `UniXNU/drivers/Adreno740/NativeAdrenoSubmit.cpp` | `peekRing(words[8],8,…)` → **`peekRing(words[8],24,…)`**（dump 起点前移 16 dword，覆盖 `CP_WAIT_TIMESTAMP`）；`stall_mem` 由 59 扩到 **63**，`[59..62]` 存 `fence_[0..3]` |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | 新增 `Z5 f%x/%x/%x/%x`；`Z2` 注释改成「下标 24 是 RPTR」 |

**`Z2` 的下标含义变了**：新镜像里 **下标 24 = `CP_RB_RPTR`**，下标 0 是 `RPTR-24`。

### 26.6 下一轮判读表（`fw/vendor_boot_gpu_fencedump.img`）

先看 `Z5` 的第二个数（`fence_[1]`，CP 最后完成的 fence）：

| `Z5` 第二个数 | 含义 | 下一步 |
| --- | --- | --- |
| **`0`** | 第一次 reset 提交**从未完成** ⇒ 第二次的 `CP_WAIT_TIMESTAMP` 在等一个永不到来的 1 | 去掉 reset 段的 `CP_WAIT_TIMESTAMP`（换等长 NOP），或把等待目标改成 `seqno_-1` |
| **`1` 或更大** | 上一次提交确实完成过，wait 不是卡点 | 卡在 `CP_RESET_CONTEXT_STATE 0xf` 或 `CP_THREAD_CONTROL SYNC` 上，下一轮把 `RingResetAll` 从 `0xf` 降到单 bit 试 |

再看新 `Z2`（下标 24 = RPTR）里 `RPTR-24 … RPTR` 这段：如果 `CP_WAIT_TIMESTAMP`（`7014****`，5 dword）确实落在 RPTR 之前且内容正确，就说明这段包已经被读过，wait 的 payload 没写错。

### 26.7 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`3592ca52d968426b7c9ba77e21d35b22897db3febe43333ae9ca6f69cc60c0b9`** |
| payload | 75,497,472 B（72 MiB） |
| 镜像 | `fw/vendor_boot_gpu_fencedump.img`，77,393,920 B，**`7932f4988cf2665d1fc896f7617437c24178f0c3b638891d8c4e92433eabda78`**；`@0x1000` 逐字节等于 `payload.img`，`MUYU Z5` 已在镜像内 |

## 27. `fence[1] = 4` —— CP 完成过 4 个 fence，`CP_WAIT_TIMESTAMP` 不是卡点；嫌疑转到 `CP_THREAD_CONTROL` 的 `SYNC` 位

### 27.1 上机读数（`fw/vendor_boot_gpu_fencedump.img`，照片 `logs/2026-11-11-22-56-17-265.jpg`）

```text
MUYU Z0 t2 f5 s400 a73b7dfbf/1ffee y170d7b u1/1
MUYU Z1 r43/81 b0/0 wffff0000 h0/0/0 R2
MUYU Z2 24 701f0001 0000000f 70970001 00000003 70460001 00000026 70970001 00000001 70108001 00000000 78d30004 cb7b0000
MUYU Z2 36 00010000 00000000 00000000 …
MUYU Z3 v10000/cb71e000
MUYU Z5 f40/4/0/0
MUYU T a1/0 ce00002d6 d2 w0/0 s0/2 g2/cb6fd000 k2/cb6fd000
MUYU W k0 0 0 e00002d6 ffffffff f5
MUYU R PUB no device: Vulkan(VkResultCode(-3))
```

### 27.2 `Z5` 直接把 `CP_WAIT_TIMESTAMP` 这条嫌疑排除

`Z5 f40/4/0/0` 就是 `fence_[0..3]`：

| 字 | 值 | 含义 |
| --- | --- | --- |
| `[0]` | `0x40` = 64 | CP 写回的 RPTR 影子 |
| **`[1]`** | **`4`** | **CP 最后完成的 fence = 4** |
| `[2]` `[3]` | 0 | 未用 |

`emitPrologue` 的 wait 等的是 `fence_iova_+4 == seqno_`，而 `seqno_` 停在第 5 个提交上、`fence_[1]` 已经是 4 —— **等待条件早就满足了**。§26.4 表格里最后一条（`CP_WAIT_TIMESTAMP` 死等）作废。

同时这条也说明：**CP 不是完全跑不动**，它已经完整执行完 4 个提交。第 5 个提交（`checkCpSwitch` 的 probe，第一个带 `reset+table` 的 framed 提交）才卡住。

### 27.3 `Z2` 起点前移 16 dword 后，RPTR 落点变了

新 `Z2` 的**下标 24 = `CP_RB_RPTR`**。读数从下标 24 起：

| 下标 | dwords | 包 |
| --- | --- | --- |
| 24–25 | `701f0001 0000000f` | `CP_RESET_CONTEXT_STATE = 0xf` ← **RPTR 落在这里** |
| 26–27 | `70970001 00000003` | `CP_THREAD_CONTROL = BOTH\|SYNC` |
| 28–29 | `70460001 00000026` | `CP_EVENT_WRITE7 LRZ_FLUSH_INVALIDATE` |
| 30–31 | `70970001 00000001` | `CP_THREAD_CONTROL = BR\|SYNC` |
| 32–33 | `70108001 00000000` | `CP_NOP`（原 perfctr，§26 的 swap 仍在） |
| 34–38 | `78d30004 cb7b0000 00010000 0 0` | `CP_SMMU_TABLE_UPDATE`，payload 与 `Z3 v10000/cb71e000` 逐字一致 |

**切表包仍然完好，CP 仍然没有 fetch 到它。** RPTR 停在 `CP_RESET_CONTEXT_STATE` 的 header 上 —— 也就是 fetch 刚越过 `CP_THREAD_CONTROL BOTH|SYNC`（下标 26–27）就停住。

### 27.4 于是嫌疑只剩「线程同步」

第 5 个提交是这个 ring 上**第一个 framed 提交**（前 4 个 fence 由 `checkCpSwitch` 之前的 plain `submit()` 产生，`framed=false`，不带 prologue）。所以第 5 个提交是这些包**第一次**被执行：

| 包 | 第一次执行时的问题 |
| --- | --- |
| `CP_RESET_CONTEXT_STATE 0xf` | a6xx 上是全 bit，a7xx 上应有同样语义，一般不会 halt |
| **`CP_THREAD_CONTROL = THREAD_BOTH\|SYNC`（bit31）** | **`SYNC` 要求所有已启用线程到达 barrier。此前 BV 线程从未被启动过（所有提交都是 inline 包、`Z1 b0/0`、没有任何 IB），CP 就在这里等一个永远不会到达的 BV** |

`SYNC` 位（bit31，`RingSyncThreads`）是 Linux `a7xx_submit()` 每帧都带的，因为它的上下文早就把两个线程跑起来了；这里刚 `requestContextReset()` 完，BV 线程一次都没启动过。

### 27.5 本轮改动：去掉 `SYNC` 位（保留线程选择）

```cpp
static constexpr bool kSyncThreads=false;                 /* 改为 true 即可恢复 Linux 的 BOTH|SYNC */
const uint32_t sync=kSyncThreads?RingSyncThreads:0u;
const uint32_t br=sync|RingThreadBr,syncBoth=sync|RingThreadBoth,both=RingThreadBoth,...
```

prologue 里三个 thread-control 包随之变成：`BR`（0x1）、`BOTH`（0x3）、`BOTH`（0x3），SYNC 位全去掉；`both`（`m=0x101` 前的那个）本来就不带 SYNC，不受影响。包长、`count`、`words[]` 校验全不变。

### 27.6 下一轮判读表（`fw/vendor_boot_gpu_nosync.img`）

先看 `Z1 r`：

| `Z1 r` | 结论 | 下一步 |
| --- | --- | --- |
| 明显越过 `43`（如 `4c`/`50`/更大） | `SYNC` 就是卡点，CP 已经穿过 reset 段 | 看 `T c` 是否变 0；若仍超时要看它停在哪一包（`Z2` 新落点） |
| 仍是 `43/81` | 不是线程同步，卡点在 `CP_RESET_CONTEXT_STATE 0xf` 或更前的 `CP_WAIT_TIMESTAMP` 之后的某个 fence | 下一轮把 `0xf` 降成单 bit（0x1） |
| 倒退或 `r` 为 0 | CP 被彻底停住（更糟） | 回滚本改动，改从 `RingResetAll` 入手 |

同时看 `Z5` 的 `[1]`：若涨到 5，说明第 5 个提交真的完成了，`T c` 应同时变 0。

### 27.7 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`6da327cb71871577b80065e5816993f6062fcb692edb97baf58ff7108516e8ea`** |
| payload | 75,497,472 B（72 MiB） |
| 镜像 | `fw/vendor_boot_gpu_nosync.img`，77,393,920 B，**`d79e4a39c89a1afa7f5cc0afb3e16028599a349f04cfe5d9deaee38bccb6c092`** |

## 28. 去掉 SYNC 也没过：fence 仍是 4，RPTR 反而退到 `0x34`；改用「换表走普通提交」绕开 reset 序列

### 28.1 上机读数（`fw/vendor_boot_gpu_nosync.img`，照片 `logs/2026-11-11-23-04-31-027.jpg`）

```text
MUYU Z0 t2 f5 s400 a73b7dfb/1ffee y170d7b u1/1
MUYU Z1 r34/81 b0/0 wffff0000 h0/0/0 R2
MUYU Z5 f32/4/0/0
MUYU Z3 v100008/cb6b2000
MUYU T a1/0 ce00002d6 d2 w0/0 s0/2 g2/cb4f9000 k2/cb4f9000
MUYU W k0 0 0 e00002d6 ffffffff f5
MUYU R PUB no device: Vulkan(VkResultCode(-3))
```

`Z1 r34/81`、`Z5 f32/4/0/0`、`T …` 三行都是高倍单独裁切复核过的（`/tmp/sr.png`、`/tmp/z5band.png`、`/tmp/st.png`）。

### 28.2 结论一：`SYNC` 不是卡点，去掉它没有换来任何进展

| | §27（`BOTH|SYNC` / `BR|SYNC`） | §28（纯 `BOTH` / `BR`） |
| --- | --- | --- |
| `Z5` 的 `fence_[1]` | `4` | **`4`**（没有第 5 个） |
| `T c`（`checkCpSwitch` 的返回） | `e00002d6` | `e00002d6` |
| `T s` | `0/2` | `0/2` |
| `Z1 r`（`CP_RB_RPTR/WPTR`） | `43/81` | **`34/81`** |

WPTR 一模一样（`0x81` = 129 dword，即这一轮写进 ring 的总量没变），RPTR 却从 `0x43` 退到 `0x34` —— **少了 15 个 dword**。

### 28.3 结论二：`Z2` 这种「按 RPTR 对齐的绝对窗口」不适合跨轮比较

`peekRing(words[8],24,40,…)` 的锚点是 **RPTR 本身**，所以 RPTR 一变，整个窗口就整体平移。上一轮 RPTR=67、这一轮 RPTR=52，两次 dump 的**下标根本不是同一个 ring 位置**，逐下标对比是无效的。

本轮尝试按上一轮的方式逐格读 `Z2`，读出来的 48 个 dword（如 `70758005 000003ff 00102004`）**无法用 PM4 编码自洽**（`0x40088581`、`0x76940004` 一类解出来的 opcode/count 都不成立），说明单行 12 个 dword 已经超出这张照片的读图极限。凡不能用 `0x7…`/`0x4…` 编码反解验证的读数，本文件一律不采信。

**方法论修正**：跨轮比较只看三个不随窗口平移的量 ——

| 量 | 位置 | 为什么可靠 |
| --- | --- | --- |
| `Z5` 的 `fence_[1]` | fence 页第 2 个字 | CP 直接写内存，单一数字 |
| `T c` | `checkCpSwitch` 返回值 | 单一数字，常数可查 |
| `Z1 r` 的 WPTR | `CP_RB_WPTR` | 写入总量，与 CP 行为无关 |

RPTR 只能同轮内使用，不能跨轮比。

### 28.4 结构性事实：reset 提交是唯一从未完成过的一类

到这里可以不管具体是哪个包，先看类别：

| 提交类型 | 是否完成过 | 依据 |
| --- | --- | --- |
| 普通提交（`framed=false`，只有 IB + fence） | **是** | §22 的 `cp=10`：`CP_MEM_WRITE` 写入并 fence 读回，`b=24` 收尾 |
| framed + reset + reset prologue | **否** | §18 以来每一轮：RPTR 冻在 prologue 前 24 个 dword 之内 |
| framed 换表（`table`） | 从未到达 | `useTable()` 在过去会**同时**置 `reset_pending_`，所以换表必然被 reset prologue 包住 |

也就是说：**换表这条路从来没有以「不带 reset」的形式试过一次**。而 reset prologue 里那些包（`CP_WAIT_TIMESTAMP`、`CP_THREAD_CONTROL`、`CP_RESET_CONTEXT_STATE`、`CP_PERFCTR_SRAM_INIT` 及其 WAIT）已经被逐条排除过两次，仍然停在原地。

### 28.5 本轮改动：把换表从 reset 里拆出来

依据是 Linux 的 `a6xx_set_pagetable()` —— 它在**任何上下文切换**时发出，而不是只在 `CP_RESET_CONTEXT_STATE` 之后；reset 只是「重置之后第一次提交恰好也要换表」。我们的代码把二者强行绑在一起，是这段代码自己加的约束。

| 文件 | 改动 |
| --- | --- |
| `NativeAdrenoRing.h` | `useTable()` 不再置 `reset_pending_`，只置 `table_pending_` |
| `NativeAdrenoRing.cpp` `submitWords()` | 新增 `kTableNeedsReset=false`；`table=framed && table_pending_`、`reset=framed && reset_pending_ && (kTableNeedsReset \|\| !table)`；提交成功后 `if(reset \|\| table) reset_pending_=false` |
| `NativeAdrenoRing.cpp` `emitPrologue()` | 换表段从 `if(reset)` 里提出来，成为独立的 `if(table)` 段；`if(table && !reset) return poison()` 删除 |

**reset 分支的字节序完全不变**：原来是 `… perfctr NOP → SMMU_UPDATE → TC BR → CACHE_INVALIDATE → WAIT NOP`，拆分后仍按这个顺序发出（`reset` 段 → `table` 段 → `reset` 段）。`RingResetWords=26`、`RingTableWords=7`、`RingPrologueWords=11` 都不用改。

只有「`table` 且非 `reset`」这条新路径产生新序列，共 18 dword：

```text
CP_THREAD_CONTROL        BR
CP_SMMU_TABLE_UPDATE     ttbr0_lo, ttbr0_hi, 0, 0        (opcode 0x53, count 4)
CP_THREAD_CONTROL        BR
CP_EVENT_WRITE7          CACHE_FLUSH7
CP_EVENT_WRITE7          CACHE_INVALIDATE7
CP_WAIT_FOR_IDLE
CP_THREAD_CONTROL        BOTH
CP_SET_MARKER            0x101
```

### 28.6 下一轮判读表（`fw/vendor_boot_gpu_tablenoreset.img`）

| 读数 | 结论 | 下一步 |
| --- | --- | --- |
| `Z5` 的 `fence_[1]` 变成 **5**，`T c0`，`W k0 0 0 0` | 换表在普通提交里通了 —— 整个 mode 2 机制可用 | 看 `T g/k` 是否还相等；接着查客户端第一条 IB |
| `T c0` 但 `W [4]` 非 0（`readTtbr0` 失败） | 提交完成但 CP 没真的换表 | 查 `CP_SMMU_TABLE_UPDATE` 的 ASID 字段（payload 第 3/4 个 dword） |
| 仍 `e00002d6`，`fence_[1]` 仍 4 | 换表包本身就会让 CP 停 | 下一轮把换表包的 `table_` 改成**内核自己的** ttbr0 再发一次：如果同样卡住，问题在包语义而不是表内容 |
| `R` 行不再是 `VkResultCode(-3)` | 已恢复，直接报结果 | — |

### 28.7 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`004c707a1031e548c2bd448e0fa251210a7f83fb2c5265a2ba658db3921042f9`** |
| payload | 75,497,472 B（72 MiB） |
| 镜像 | `fw/vendor_boot_gpu_tablenoreset.img`，77,393,920 B，**`4ac464ff930bdbd215c785bd7bc3f3323f0080a2540df89a14b3309e0ddb9f46`**；`@0x1000` 起 75,497,472 B 与 `payload.img` 逐字节相同（已验 `True`） |

### 28.8 读图工具的现实约束（记录，省下一轮的时间）

- 这张 OPPO 的照片 3840×2160，文字行高约 34 px、行距 51 px；`MUYU Z2` 一行 12 个 dword 已经读不准。
- 可用做法：先 `ffmpeg -noautorotate -i … -vf format=gray -f rawvideo` 拿原始灰度，用行均值做阈值分割拿到**精确行 y 坐标**（本轮得到 417 / 468 / 520 / 570 / 621 / 672 / 723 …），再按行裁切放大；靠目测估 y 会切到行间并导致整行错读。
- 单行裁切用 `crop=Wx36:0:y,scale=…:flags=lanczos`，宽度不要超过 ~3100 px（再大反而更糊）。

## 29. 上游 uke 仓库给出的判决：**mode 2（CP 侧换表）在第一次 Metal 提交就会冻住 ring**；改回 mode 1 为默认

### 29.1 上游仓库现状（`Allenwdk/UniXNU-uke`，2026-11-11 23:1x 拉取）

| 项 | 结果 |
| --- | --- |
| `origin/main` 相对本地 `fcf9fd3` | **8 个新提交**，全部是 `.github/workflows/build-wifi-supplicant.yml`（CI 里在 macOS 上构建签名的 wpa_supplicant、shallow clone 锁定源）。**没有一个 GPU 提交** |
| `origin/feat/ipados27-gpu-parity` | 相对 main **0 个独有提交**（29 提交落后），已完全并入 |
| `origin/uke-gpu` | 相对 main **0 个独有提交**（90 提交落后），已完全并入 |

也就是说 main 上不存在我们还没看到的 GPU 代码变更。但**历史**里有一条判决性的提交。

### 29.2 `2949667 board: force CPU TTBR0 switching for the A732 31-bit aperture`

`board.toml` 里新增的四行（逐字）：

```toml
  # The A732 SMMU is fused to 31 bits. Force the CPU TTBR0 switch with the
  # GPU idle (mode 1): the CP aperture switch (mode 2) stalled the ring on the
  # first Metal submission in gpucomp57.
  "sheng-gpu-ttbr=1",
```

**这与我们 §18–§28 撞的是同一个钉子，而且是他们先撞完的。** 他们的症状与我们的 `e00002d6` / `fence_[1]` 不涨 / `b0/0` / CB0 无 fault 是同一类：CP 在第一次真正的 Metal 提交上就停住。

他们最终跑通的形态（`docs/DPU-TASKS.md` §28，gpucomp46，连续 918 s）：

```text
UKE GPU: conn switch=0 cp=0 mode=1 mount=0/0 shared=0 tables=10/1 in=31 out=40
UKE GPU3: M a5.0/5.0 k0 s5/0 r1 o2/0/1 n2
```

`mode=1`、`tables=10/1`（CPU 写 TTBR0 10 次、CP 切表 1 次）、`mount=0/0`、`shared=0` —— **每进程地址空间在 mode 1 下是走得通的**，代价是每次换表要等 GPU 空闲。这也是我们 `useVm()` 里 mode 1 分支的写法（`waitAll` → `writeTtbr0` → 记 `table_switches[1]`）。

> 他们当时留下的两个后续疑点，与本板无关但值得记：`>maxDepth` 偶发 panic（每进程地址空间踩坏元类链）、以及 mode 1 下 `writeTtbr0` 与提交/完成、TLB 失效的先后关系。

### 29.3 我们自己的读数本来就指向同一结论

| 轮次 | 模式 | `T` 行 | `checkCpSwitch` |
| --- | --- | --- | --- |
| §23（`stallz`） | **1**（`d1`） | `a0/0 c0 d1 w0/0` | **`c0` = 五步全过** |
| §24–§28 | 2（`d2`） | `a1/0 ce00002d6 d2 …` | `e00002d6` = `waitAll` 超时 |

mode 1 早就让探针那条提交完整跑完过一次；mode 2 之后我们再没见过 `c0`。§24 曾把 `a1/0`+`d2` 读成"突破"，其实是把一条更差的路当成了进步。

### 29.4 本轮改动：`tableMode()` 默认改成 1

```cpp
static unsigned tableMode()
{
    unsigned mode=1;
    PE_parse_boot_argn("sheng-gpu-ttbr",&mode,sizeof mode);
    return mode==2?2:1;
}
```

原来默认值是"aperture 打开就选 2"，现在默认 1，`sheng-gpu-ttbr=2` 仍可强制回 mode 2 做对照。

**为什么改代码而不是加 boot-arg**：`sheng-gpu-ttbr=1` 是 18 字节，而命令行已经 994 B / 1000 B 上限（`scripts/unixnu/payload.py:36`，DarwinBoot 的 1024 B 缓冲区）。为 6 字节余量去删别的诊断参数不划算，改默认值不需要动命令行。

### 29.5 mode 1 下 prologue 会缩到最短（这是本轮真正的实验价值）

`table_pending_` 只由 `useTable()` 置位，而 `useTable()` 只在 `table_mode_==2` 时调用；`requestContextReset()` 现在**没有任何调用者**。所以 mode 1 的 framed 提交里：

- **没有** `CP_SMMU_TABLE_UPDATE` 段
- **没有** reset 段（`CP_WAIT_TIMESTAMP` / `CP_RESET_CONTEXT_STATE` / perfctr 都不发）

prologue 只剩：`TC BR` → `CACHE_FLUSH7` → `CACHE_INVALIDATE7` → `CP_WAIT_FOR_IDLE` → `stamp` → `TC BOTH`。这正是 §26–§28 反复怀疑的那几段被一次性全部拿掉。

### 29.6 下一轮判读表（`fw/vendor_boot_gpu_ttbr1.img`）

| 读数 | 结论 | 下一步 |
| --- | --- | --- |
| `T d1` 且 `c0`，`Z5` 的 `fence_[1]` ≥ 5 | mode 1 恢复，换表回到已验证的路 | 看 `R` 行是否还是 `VkResultCode(-3)`；若仍是，断点已经不在换表，转 `M`/`o` 计数与客户端第一条 IB |
| `T d1` 且 `c0`，但 `fence_[1]` 仍 4 | 换表没问题，卡在 prologue 的 flush/idle 或 IB | 关 `CP_WAIT_FOR_IDLE` / `stamp` 二分 |
| `T d1` 但 `c` 非 0 | mode 1 的 `waitAll` 就超时（`useVm` 里） | 说明 ring 在**更早**的提交上已经不再推进，回到 `Z1`/`Z5` |
| `T d2` | 改动没生效（`sheng-gpu-ttbr=2` 被别处带上） | 查 payload 的 bootconfig 里有没有残留 `sheng-gpu-ttbr` |

### 29.7 本轮产物

| 项 | 值 |
| --- | --- |
| 内核 | **`3066d408530fa871988a74af6a610078b2c716189c6852924a6a437b9fe83be1`** |
| payload | 75,497,472 B（72 MiB），kernel args 与上一轮逐字相同（**没有**新增参数） |
| 镜像 | `fw/vendor_boot_gpu_ttbr1.img`，77,393,920 B，**`248d8e96171635ca4cc5cf0b99ca696b7b539ffbcf8cef2c5d31f13d8e11ef62`**；`@0x1000` 起 75,497,472 B 与 `payload.img` 逐字节相同（已验 `True`） |

> 上一轮的 `fw/vendor_boot_gpu_tablenoreset.img`（`4ac464ff…`）已刷入但**未取读数**。它的假设（换表走普通 framed 提交就能过）被 §29.2 的上游判决间接否掉，即使通也仍然要走 mode 2；这一轮直接越过它。

### 29.8 补记：`tablenoreset`（§28）的实际读数 —— 换表塞进普通提交也没解开

照片 `logs/2026-11-11-23-17-07-793.jpg`（两个屏：上半是滚过的旧 dump，下半是当前 dump；以下取下半，行带用灰度方差定位后逐行高倍裁切）：

```text
MUYU Z1 r34/67 b0/0 wfffff000 h0/0/0 R2
MUYU Z5 f32/4/0/0
MUYU Z3 v100008/cb583000
MUYU T a1/0 ce00002d6 d2 w0/0 s0/1 g2/cc8be000 k2/cc8be000
MUYU W k0 0 0 e00002d6 ffffffff f5
MUYU R PUB no device: device: Vulkan(VkResultCode(-3))
MUYU X4 H118 R800005 r34/67 b67 i0/0 j0 v0/0
MUYU X6 N a5.0/5.0 k0 s5/0 r1 o3/0/0 n2 d00:0/140
```

| 读数 | 与 §27/§28 比 | 含义 |
| --- | --- | --- |
| `s0/1` | 上一轮是 `0/2` | **§28 的拆分生效了**：换表包只发 1 个（原来 reset 与 table 各带一个），CPU 写 TTBR0 仍 0 次 |
| `Z5 f…4…` | 仍是 4 | 第 5 个提交（第一条带 prologue 的）**依然没完成** |
| `T c e00002d6` | 不变 | `checkCpSwitch` 的 `waitAll` 仍超时 |
| `d2` | 不变 | 仍是 CP 侧换表 |
| `r34/67` | §28 是 `34/81` | WPTR 少了 14 dword（`0x81`→`0x67`），正是"换表包从 2 个降到 1 个 + reset 段不再重复发换表"省下的量 |

**结论**：换表包的位置（reset 内 vs 普通 framed 提交内）不是变量 —— 两种放法 fence 都停在 4。这与 §29.2 上游的判决一致：问题在 **mode 2 这条机制本身**，不在包的编排。所以本轮直接改默认到 mode 1，不再在 mode 2 里继续排列组合。

> 附带确认：`reset_pending_` 的成员初值是 false，但 `NativeAdrenoRing::restart()`（`NativeAdrenoRing.cpp:471`）和 `restartSecondary()`（:484）都把它置 true。所以 mode 1 下第一条 framed 提交**仍会带一次 reset 段**（`701f0001 0000000f` 那串），只是不再带换表包。这一点在下一轮判读 `Z2` 时要用到。

---

## 30. mode 1 落地了：换表彻底过关（`c0 d1 s1/0`，bank 已用客户端表），fence 仍停在 4 —— 断点移出换表

照片 `logs/2026-11-11-23-35-06-294.jpg`（3840×2160，下半屏为当前 dump）：

```text
MUYU Z1 r34/74 b0/0 wfffff000 h0/0/0 R2
MUYU Z5 f32/4/0/0
MUYU Z3 v100009/c6b20000
MUYU T a1/0 c0 d1 w0/0 s1/0 g10/c6b20000 k2/cb55f000
MUYU W kffffffff ffffffff ffffffff ffffffff ffffffff fffffffff
MUYU R PUB no device: device: Vulkan(VkResultCode(-3))
MUYU X2 C1 cffffffff u0 v0 d0
MUYU X3 S0/0/0 10 P0/0 f400 a3/301 c1c
MUYU X4 H106 R800005 r34/7a b7a i0/0 j0 v0/0
MUYU X5 S SpringP0 x358 c355 f7 l6/2d6 g14 u0
MUYU X6 N a5.0/5.0 k0 s5/0 r1 o5/0/4 n3 d00:0/122
MUYU X7 K4 6000 6801 6a3c 68f2
MUYU D0 P ok ok ok skip io0 pw11
MUYU D1 V chip 43038b00 gmx c0 32/32/32/0
MUYU D2 R gf12/12 m0 cx3/3 m0
```

### 30.1 先撤回 §29.8 的一处误读：`s` 的两个数字顺序

`BravearkLastLog.c:759` 的格式串是

```c
printf("MUYU T a%x/%x c%x d%u w%x/%x s%u/%u g%x/%x k%x/%x\n", ...,
    native_adreno_table_switches[1], native_adreno_table_switches[0], ...);
```

即 **`s<CPU 写 TTBR0 次数>/<CP 消费的 table update 数>`**，与注释里 `[1]`/`[0]` 的定义一致。§29.8 表格把这两个位置读反了，现更正：

| 轮次 | 读数 | 正确含义 |
| --- | --- | --- |
| §29.8（mode 2） | `s0/1` | CPU 写 TTBR0 **0** 次、CP 消费 **1** 个 update —— mode 2 的正常形态 |
| §30（mode 1） | `s1/0` | CPU 写 TTBR0 **1** 次、CP 消费 **0** 个 update —— mode 1 的正常形态 |

两轮各自"自洽"，§29.8 的结论（换表包放哪都不行）不受影响。

### 30.2 mode 1 的每一格都对上了

| 读数 | 期望（mode 1） | 实测 | 判定 |
| --- | --- | --- | --- |
| `d` | 1 | `d1` | ✅ `tableMode()` 默认改 1 生效 |
| `c` | 0（mode 1 不跑 `checkCpSwitch`） | `c0` | ✅ 与 §23 记录的 `T c0 d1` 逐字一致 |
| `W k…` | 六格全 `ffffffff`（从未执行） | 全 `ffffffff` | ✅ 印证 `NativeAdrenoPublish.cpp:1214` 的 `else if(tableMode()==2)` 没进 |
| `a` | 1/0 | `a1/0` | ✅ SCM aperture 调用成功（但本模式不用它） |
| `w` | 0/0 | `w0/0` | ✅ VM 建立 + attach 全程无错（`[7]=0` = done） |
| `s` | 1/0 | `s1/0` | ✅ CPU 写了一次 TTBR0 |
| `g` vs `k` | 必须不同 | `g10/c6b20000` ≠ `k2/cb55f000` | ✅ **bank 现在用 ASID 0x10 / root `0xc6b20000` 的客户端表，不是内核表** |

`g != k` 是这一轮最关键的一格：地址空间切换**真的装进了 GPU 的 bank**，而且没被还原。加上 `w0/0` 说明 VM 的 prepare/attach/映射全部走完。

**至此换表这一整条线可以结案**：mode 2 冻 ring（上游 uke `2949667` 同判），mode 1 干净通过，CPU 侧写表 + 建 VM 无一处报错。继续在换表上排列组合没有意义。

### 30.3 但 fence 还是 4

`Z5 f32/4/0/0` —— `fence_[1]=4`，第 5 个提交（第一条 framed、带 reset 段的那条）依旧不完成。`R` 行仍是 `VkResultCode(-3)`，`Z1 r34/74`，说明 CP 没往前走。

于是出现一个干净的对照：

- 前 4 个提交**不带 prologue**（kernel/plain NOP），全部完成 → fence 到 4
- 第 5 个提交**带 prologue**，立刻停住 → CP 不取指

`submitWords` 里那句注释正好是这个历史结论：*"The reset prologue is the one stretch this ring never got past, while the plain framed prologue completes"*。

### 30.4 mode 1 + reset=true 时 prologue 实际发了什么

按 `emitPrologue`（`NativeAdrenoRing.cpp:217`）与 `kPerfctrInit=false`、`kSyncThreads=false`、`table=false`，第一条 framed 提交的 16 个包是：

```text
1  CP_THREAD_CONTROL BR
2  CP_WAIT_TIMESTAMP   (fence_iova+4 == seqno_)
3  CP_THREAD_CONTROL BOTH
4  CP_RESET_CONTEXT_STATE(all)
5  CP_THREAD_CONTROL BOTH
6  CP_FENCE_OPCODE(LRZ)
7  CP_THREAD_CONTROL BR
8  CP_NOP(1)                      ← 原 CP_PERFCTR_SRAM_INIT，§26 换掉
9  CP_FENCE_OPCODE(INVALIDATE)
10 CP_NOP(6)                      ← 原 CP_WAIT_REG_MEM，§26 换掉
11 CP_FENCE_OPCODE(CACHE_FLUSH7)
12 CP_FENCE_OPCODE(CACHE_INVALIDATE7)
13 CP_WAIT_FOR_IDLE
14 stamp
15 CP_THREAD_CONTROL BOTH
16 CP_SET_MARKER
```

第 2 条不是卡点：`emitPrologue` 里 `wait[3]` 用的是**成员 `seqno_`**（上一次提交的序号，`submitWords` 末尾才 `seqno_=number`），所以第 5 次提交时它等的是 4==4，立即满足 —— §27 的排除成立，本轮不翻案。

### 30.5 `X6` 的 open 计数变了

| 字段 | §29.8 | §30 | 含义（`GpuPowerProbe.cpp:1189`） |
| --- | --- | --- | --- |
| `o` | `3/0/0` | `5/0/4` | opens/busy/closes —— 5 次尝试、0 挂起、4 次已关闭 |
| `n` | `2` | `3` | MetalPluginName 读取次数 |
| `s` | `5/0` | `5/0` | publish stage 5 / 状态 0 |
| `d` | `00:0/140` | `00:0/122` | `adrenoprobed` 从未出现：seen=0 alive=0 |

进程总数从 140 降到 122（本轮开机更早抓拍），daemon 依然从未启动。插件 side 是"反复 open、反复关"，与 `R` 行的 `-3` 一致。

### 30.6 下一轮的最小实验：把 reset 段摘掉

理由：历史记录（`submitWords` 注释）已经说明 **plain framed prologue 能完成**，唯一的差别就是 reset 段。§27/§28/§29 三轮改的都是 reset 段**内部**的组成（SYNC 位、perfctr、换表包位置），从没做过"整段不发"。

做法：`submitWords` 里给 `reset` 加一个 `kResetPrologue` 常量，置 false 后 framed 提交只走 9 条尾部包（11–16 + stamp），`CP_RESET_CONTEXT_STATE`/`CP_FENCE_OPCODE(LRZ)`/`CACHE_FLUSH7`/`CACHE_INVALIDATE7` 全部不发。

判读点（与 §30 同格式，逐格对比）：

- `Z5 fence_[1]` —— 若 ≥5，reset 段就是卡点，接着二分它内部哪一条；若仍 4，卡点在尾部那 6 条里
- `T s1/0` —— 应保持（换表不受影响）
- `Z1 r34/xx` —— WPTR 会因少了 reset 段而变短约 `RingResetWords`
- `R` —— 若变成别的错误码，说明 CP 真的跑起来了

---

## 31. 把整段 reset 摘掉，fence 仍是 4 —— 卡点不在 prologue 的内容

照片 `logs/2026-11-12-09-02-43-318.jpg`。**这张是倒着拍的**，需先 `ffmpeg -noautorotate -vf "hflip,vflip"` 转正（不能只 `sips -r`；整帧重采样会改变相位）。

```text
MUYU Z0 t21 f5 s400 a33b6dbf/1ffee y170d7b u1/1
MUYU Z1 r34/60 b0/0 wfffff000 h0/0/0 R2
MUYU Z5 f32/4/0/0
MUYU Z3 v100009/ef724000
MUYU T a1/0 c0 d1 w0/0 s1/0 g10/ef724000 k2/cb4f3000
MUYU W kffffffff ffffffff ffffffff ffffffff ffffffff fffffffff
MUYU R PUB no device: device: Vulkan(VkResultCode(-3))
MUYU X4 H106 R800005 r34/60 b60 i0/0 j0 v0/0
MUYU X5 S SpringP0 x338 c337 f7 l6/2e6 g14 u0
MUYU X6 N a5.0/5.0 k0 s5/0 r1 o5/0/4 n3 d00:0/131
```

### 31.1 开关确实生效，但结果没变

| 读数 | §30（有 reset） | §31（无 reset） | 判定 |
| --- | --- | --- | --- |
| `Z1` WPTR | `r34/74` | `r34/60` | 少 `0x14` = 20 dword，正是 `RingResetWords` 里被摘掉的量 → `kResetPrologue=false` 生效 |
| `T` | `c0 d1 w0/0 s1/0`，`g!=k` | 完全相同 | mode 1 没被弄坏，客户端表仍在 bank 里 |
| `Z5 fence_[1]` | 4 | **4** | ❌ 第 5 个提交仍未完成 |
| `R` | `VkResultCode(-3)` | 相同 | 用户态依旧连不上 |
| `Z0` | — | `f5`、`u1/1` | hangcheck 正在等 fence 5，第一级恢复动作跑了 1 次没解开 |

**结论**：prologue 里 reset 那 10 个包（`CP_WAIT_TIMESTAMP`、`RESET_CONTEXT_STATE`、`FENCE(LRZ)`、两个 NOP 替身、`CACHE_INVALIDATE`）整段不是卡点。§30.6 的第一条分支排除。

### 31.2 真正刺眼的量：`CP_RB_RPTR` 跨四轮一动不动

| 轮次 | 改动 | `Z1` RPTR | WPTR |
| --- | --- | --- | --- |
| §28 | 换表进普通提交 | `0x34` | `0x67` |
| §29.8 | tablenoreset | `0x34` | `0x67` |
| §30 | mode 1，有 reset | `0x34` | `0x74` / `0x7a` |
| §31 | mode 1，无 reset | `0x34` | `0x60` |

WPTR 忠实地跟着 prologue 的长度变，RPTR 却**恒为 `0x34`**。按 §26.3 的定义 RPTR 是**取指指针**：CP 从第 4 个提交结束之后就再没取过一个新包。prologue 内容怎么改都碰不到它 —— 这与四轮实验的结果完全自洽。

`BravearkLastLog.c:726` 的注释也给了同一方向："a valid PM4 header here means the page (not the content) is what the CP lost"。

### 31.3 两个待验的根因假设

1. **`emitFramedFence` 的 BV 等待**（`NativeAdrenoRing.cpp:emitFramedFence`）：framed 提交走的是和前 4 个 plain 提交**完全不同**的 fence 路径，其中
   `CP_THREAD_CONTROL(BV) → CP_EVENT_WRITE7(写 BV fence = seqno) → CP_THREAD_CONTROL(BR) → CP_WAIT_TIMESTAMP(等 BV fence == seqno) → CP_EVENT_WRITE7(写主 fence)`。
   而 §26 的注释明确写过 "the BV thread has never been started"。若 BV 线程根本不写那个 fence，这条等待就永远不满足，主 fence 也就永远停在 4。
2. **换表后 ring 的 IOVA 在客户端页表里没映射**：CPU 写 TTBR0 后（`g10/ef724000 != k2/cb4f3000`）CP 用新表取指，如果 ring buffer 的 IOVA 不在新表内，取指直接失效 —— 同样表现为 RPTR 冻结、WPTR 照涨、任何 ring 内容改动都无效。`Z1 h0/0/0` 三个 `handleAt()` 全 0 也值得回头核对。

`Z1 h%x/%x/%x` 按 `BravearkLastLog.c:710` 是 stalled job 自己地址空间里 BR IB1 / BV IB1 / BV IB2 的 `handleAt()`，`0xffffffff` 表示该地址没映射；全 0 是另一种异常，下一轮要结合 `stallState()` 的赋值看。

### 31.4 下一轮：让第 5 个提交和前 4 个完全同构

`submitWords` 加 `kPrologue=false`：framed 提交不再调 `emitPrologue`，并改用普通 `emitFence`（不带 BV 段）。`extra` 的容量估算故意保持保守，`WPTR` 始终取 `emitFence` 自己返回的 `end`，不会多发布。

这是最外层的一次二分：

- **fence ≥5** → 原因在 prologue 或 `emitFramedFence`，接着按 §31.3 的假设 1 单独把 BV 等待换回等长 NOP 验证。
- **fence 仍 4** → ring 内容整片排除，问题在取指机制：查 hangcheck 的 WPTR 重写是否覆盖该场景（`Z0 u1/1` 显示只动了第一级）、CP 线程状态，以及新页表下 ring IOVA 的映射（假设 2）。

镜像 `fw/vendor_boot_gpu_plain5.img`。判读格：`Z5 fence_[1]`、`Z1 r34/xx`、`Z0 f/u`、`T`（应保持 `c0 d1 s1/0 g!=k`）。

---

## 32. 第 5 个提交剥成 plain 后，读指针四轮来第一次动了 —— 但它停在**合法 fence 包**的开头

照片 `logs/2026-11-12-09-26-59-098.jpg`（同样倒着拍，需 `hflip,vflip` 转正）：

```text
MUYU Z0 t20 f5 s400 a7b7dbbf/1ffee y170d7b u1/1
MUYU Z1 r36/3d b20000/1 w0 h0/c/0 R2
MUYU Z2 24 40088501 00000005 70460004 00000031 00300004 00000005 00000000 ...
MUYU Z5 f32/4/0/0
MUYU Z3 v100009/e5fa000
MUYU T a1/0 c0 d1 w0/0 s1/0 g10/e5fa000 k2/cb634000
MUYU W kffffffff ffffffff ffffffff ffffffff ffffffff fffffffff
MUYU R PUB no device: device: Vulkan(VkResultCode(-3))
MUYU X6 N a5.0/5.0 k0 s5/0 r1 o5/0/4 n3 d00:0/129
```

### 32.1 先把 `r` 的含义坐实

`NativeAdrenoRing::stallState()` 的寄存器表里 `regs[8]=RingRptr`、`regs[9]=RingWptr`；`noteStall` 做 `native_adreno_stall[2+i]=words[i]`，所以 `Z1` 打印的 `stall[10]/[11]` 正是 `words[8]/[9]` = **CP_RB_RPTR / CP_RB_WPTR**。同时 `peekRing(words[8],24,40,…)` 用的也是 RPTR，故 **`Z2` 下标 24 就是 RPTR 指向的那个 dword**。

### 32.2 变化与不变

| 读数 | §31 | §32 | 含义 |
| --- | --- | --- | --- |
| `Z1` RPTR | `0x34`（四轮恒定） | **`0x36`** | **CP 终于往前取了指** |
| `Z1` WPTR | `0x60` | `0x3d` | prologue 也摘掉后提交只剩 9 dword |
| `Z5 fence_[1]` | 4 | **4** | 第 5 个 fence 仍未写 |
| `T` | `c0 d1 w0/0 s1/0 g!=k` | 完全相同 | mode 1 与客户端表仍在，未被本轮改动影响 |
| `Z0` | `u1/1` | `u1/1` | hangcheck 第一级动作仍是 1 次 |
| `R` | `VkResultCode(-3)` | 相同 | 用户态仍连不上 |

`Z2` 下标 24（= `ring[0x36]` = RPTR）的内容：

```text
40088501 00000005 70460004 00000031 00300004 00000005 ...
```

这是**一个完全合法的 `emitFence` 包**：`40088501` = type4 `CP_SCRATCH` 写，`00000005` = 本次 seqno 5，`70460004` = `CP_EVENT_WRITE7`。按 PM4 编码自洽性校验能完整反解，不是读错、不是垃圾、不是页丢失。

也就是说：**CP 面前摆着格式正确的包，读指针却停在这里不动。**

### 32.3 到这里为止排除掉的东西

- ❌ reset 段整段（§31）
- ❌ prologue 全部（`kPrologue=false` 本轮）
- ❌ `emitFramedFence` 的 BV 等待（本轮改走普通 `emitFence`，仍 4）
- ❌ ring 内容错误 / 页不可达（Z2 能读到合法 PM4）
- ❌ WPTR 没被 CP 收到：`noteStall` 第一级在 50 ms 时执行了 `vm_->invalidateTlb()` + `stalled->kick()`（`kick()` 就是 `writeWptr(next_)`），`Z0 u1/1` 证明跑过一次，**没解开**
- ❌ fence 页漏映射：`kIovas[]` 的 index 2 是 `GpuMemptrsIova`，客户端 VM 在 `createVm` 里用同一组 `buffers_[]` / `kIovas[]` 全量映射了 11 个 buffer

前 4 个提交和第 5 个提交之间**只剩一个差异**：`useVm()` 在它们中间写了 TTBR0（`T s1/0` 记着这一次，`g != k` 证明它生效了）。

### 32.4 下一轮：mode 0（完全不换表）

`tableMode()` 默认改 **0**。`setTableSwitching(0,…)` 合法（`mode>2` 才拒），`createVm()` 依旧能成功 —— `ttbr0_switched` 是 `enableSwitching()` 置的，与 mode 无关 —— 但 `useVm()` 里 `if(!table_mode_)return kIOReturnSuccess;` 会直接返回，**一次 TTBR0 都不写**。客户端 VM 仍建得出来，只是所有客户端共用内核表（bring-up 阶段可以接受，隔离以后再补）。

这是单变量实验：§32 已经单独测过 `kPrologue=false`（fence=4），本轮保留它，只动 `tableMode`。

- **fence ≥5** → 换表就是根因。可以先用 mode 0 把 Metal 整条链跑通，再回头查「CPU 写 TTBR0 之后 CP 为什么不再取指」。
- **fence 仍 4** → 与换表也无关，转向客户端第一条 IB / CP 线程状态，届时需要在 dump 里增加 `stall[8..9]`、`[16..21]`（0x0a66 等 fault 状态寄存器）与 IB 内容。

镜像 `fw/vendor_boot_gpu_notable0.img`。判读格：`T d0`（应为 0）、`T s0/0`（CPU 写 0 次）、`T g==k`（bank 保持内核表）、`Z5 fence_[1]`、`Z1 r/w`、`R`。

## §33 mode 0：换表整条线结案，断点正式移到用户态 device 创建（2026-11-12 09:43）

镜像 `fw/vendor_boot_gpu_notable0.img`（`44fcb855…`），照片 `logs/2026-11-12-09-43-19-936.jpg`（倒置，须 `hflip,vflip`）。

```text
MUYU Z1 r36/3d b20000/1 w0 h1/c/0 R2
MUYU Z5 f32/4/0/0
MUYU Z3 v100009/c622d000
MUYU T  a1/0 c0 d0 w0/0 s0/0 g2/cb6ff000 k2/cb6ff000
MUYU W  ffffffff ffffffff ffffffff ffffffff ffffffff ffffffff
MUYU R  PUB no device: device: Vulkan(VkResultCode(-3))
MUYU X6 N a5.0/5.0 k0 s5/0 r1 o5/0/4 n3 d00:0/131
MUYU X7 K4 6800 6801 6a3c 68f2
MUYU Z0 t21 f5 s400 a20000/1 y170d01 u1/1
```

### 33.1 mode 0 确实生效

| 格 | 期望 | 实测 | 判定 |
| --- | --- | --- | --- |
| `T d` | 0 | `d0` | `tableMode()==0` |
| `T s` | 0/0 | `s0/0` | 一次 TTBR0 都没写，CP 也没消费 update 包 |
| `T g` vs `k` | 相等 | `2/cb6ff000` == `2/cb6ff000` | context bank 全程保持内核表 |
| `Z3` | 客户端表仍构造 | `v100009/c622d000` | `createVm()` 成功，只是从不 `useVm()` |

`kPrologue=false` 从 §32 保留，本轮只动 `tableMode`，是干净的单变量。

### 33.2 结果：fence 仍是 4 —— 换表全族排除

`Z5 f32/4/0/0`、`Z1 r36/3d`（**与 §32 逐字相同**）、`R` 仍是 `VkResultCode(-3)`、`X6 o5/0/4` 不变。

于是 **mode 0 / 1 / 2、CPU 写 TTBR0、CP aperture 切换、客户端页表、TTBR0 与提交时序，整族排除**。§24–§32 关于「换表把 ring 冻住」的所有假设到此全部作废：mode 2、mode 1、mode 0 结果一致。

这也独立复现了上游 `UniXNU-uke` 的 `af004d4`（2026-10-30 04:48）原文：

> "Device gate is NOT met: gpucomp57/58 show `GPU1 H... R800005 r64/83 (CP ring stopped)` and `GPU8 PUB no device DeviceLost after the first Metal submission`; **`sheng-gpu-ttbr=1` did not change it**."

**订正 §29**：当时把 `2949667` 当作「上游判决、mode 2 冻 ring」而回退 mode 1。实际上 `2949667`（04:33）只是 15 分钟就被证伪的**假设**，其注释写明「A732 SMMU fused to 31 bits，force mode 1」；`5ae72d7`（同日 20:52）换 24A446 时把 `sheng-gpu-ttbr=1` 连同 17 线 GPU 参数一起删掉，当前 uke `board.toml` 已无该参数。我们回退 mode 1 没有害处，但那个「上游判决」的依据不成立。

### 33.3 方向修正：`-3` 是 INITIALIZATION_FAILED，断点是「用户态没拿到 device」

`X7` 是 `boot_progress_ext[7]`，marker 定义见 `drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm:76`：

> `6800 entered, 6801 Turnip found, 6802 device made, 6803 returned to the discovery, 68f1 no Turnip, 68f2 no device.`

`X7 K4 6800 6801 6a3c 68f2` = 插件进了、Turnip 找到了、framebuffer 位读完，然后 **`68f2` = `device = nil`**。`R PUB no device:` 正是 `NativeAdrenoMetalPlugin.mm:402-405` 在 `native_bridge_metal_open()` 全部 6 个 shader-cache 候选都失败后打印的（`nmb_last_error()` 来自 `NativeBridgeMetalFrontend.mm:4571-4581` 的 `nmb_device_open()`）。

**Vulkan 错误码订正**：`-3 = VK_ERROR_INITIALIZATION_FAILED`，`-4` 才是 `VK_ERROR_DEVICE_LOST`。此前几轮把它称作 DeviceLost 是错的。

⇒ **用户态根本没创建出 MTLDevice，根本不存在「客户端第一条 Metal 提交」**。§24–§32 一直假设 ring 第 5 个提交是客户端渲染 IB，这个前提不成立；第 5 个提交是驱动内部 / 设备创建过程中的提交，两侧可能是同一根因的两个表现，也可能互为因果。

### 33.4 与 uke 历史逐字命中的一条

`UniXNU-uke/docs/DPU-PORT.md:142`（§9.1「主线对齐」，2026-10-29）：

> 「`drivers/AdrenoMetal`：重置到上游 `ce5e19e` …… **Turnip 用新驱动 ABI 重编（旧 Turnip 在 `vkCreateDevice` 返回 `VK_ERROR_INITIALIZATION_FAILED`，日志 `K4 6800 6801 6a3c 68f2`）**。」
> 「结果：GPU 合成器真正工作（`o6`、`UKE GPU8 PUB tex`），不再 DeviceLost/硬卡死。」

marker 序列 `K4 6800 6801 6a3c 68f2` 与我们**逐字一致**。uke 给的根因是 **Turnip / Metal 前端与 Adreno740 驱动 ABI 不配套**，解法是「驱动 rebase `5c8c145` + 前端 `ce5e19e` + 用新驱动 ABI 重编 Turnip」。

我们这边的三件配套性从未验证：

| 组件 | 本地版本 / 来源 |
| --- | --- |
| `drivers/AdrenoMetal` | `854f99a` —— 正是 uke **27 线验收发布**的 AdrenoMetal main（`GPU-UKE-ALIGNMENT.md:15`） |
| `libvulkan_freedreno.dylib` | `build/mesa-tu-ios/...`，11-11 为 iOS 17 本地重编（`GPU-HANDOFF.md:271`，17,921,872 B），打进 `fw/gpuaddon-17-full.bin` |
| `Adreno740` 驱动 | muyu-17 由 `gen-muyu-gpu-*-source.py` + `build-arm-driver.py` 从 uke 源码生成（`board.toml:101-113`），且叠了 §24–§33 一堆本地 ring 改动 |

次要候选（uke `docs/GPU-PORT.md:667-672`）：`NativeAdrenoMaxClients=4`，第 5 个连接得 `kIOReturnNoSpace`；`setScope` 时队列仍有 in-flight job 得 `kIOReturnBusy`；**两者都让插件停在 `device = nil` (0x68f2)**。但 `X6 o5/0/4` 的 busy=0 暂不支持 Busy。

### 33.5 下一步（不再改 ring）

1. **离线核对 ABI 契约**：`NativeTurnipSmoke.h:432` 描述的 open type `0x4147` selector / method 表 vs 当前 `NativeAdrenoPublish.cpp` 实际方法表；Turnip 的 CDHash 与构建输入（`GPU-ONDEVICE.md:1099,1127`）；对照 uke `5c8c145` / `ce5e19e` 的 diff。
2. **把失败点打出来**：`NativeTurnipSmoke.h:425` 已有 `fail(stage,result)` → `mark(0x6f00|stage)` / `mark(0x6e00|(-result))` 机制，把 `vkCreateInstance` / `vkEnumeratePhysicalDevices` / `vkCreateDevice` 具体哪一步失败送进 `X7`，一次定位是 loader/实例/物理设备/逻辑设备。
3. **`adrenoprobed` raw user test**：addon 里已有 `bin/adrenoprobed`，uke 用它把「user-client grant / 沙箱」与「Metal 前端」分开证明（`RECOVERY-LOG.md:145-148`）；若 raw test 也拿不到 device，就是 Turnip↔驱动 ABI，若 raw test 通过则是 Metal 前端/插件侧。
4. 把 §24–§33 的实验开关（`kPerfctrInit`、`kSyncThreads`、`kTableNeedsReset`、`kResetPrologue`、`kPrologue`、`tableMode` 默认）**收回干净态**再谈性能，避免开关叠加污染后续判读。

---

## §34 专题：uke 历史上如何解决 Metal 用户态问题（2026-11-12 离线考察）

范围 `UniXNU-uke`（341 commits，2026-10-20 → 11-08；`main` / `origin/uke-gpu` / `origin/feat/ipados27-gpu-parity`），交叉 `git log --grep` / `git show` / `git log -S` 与 `docs/*.md` 原文。

### 34.1 最值钱的方法论：分层断言链

uke 从不把「GPU 不工作」当一个问题，而是每层各有一条独立证据，逐层证明：

| 层 | 证据 | 含义 |
| --- | --- | --- |
| 交付 | `M a5.0/5.0` | Extensions / LaunchDaemons 两个 bindfs 视图挂载，`Info.plist` 可见 |
| user-client | `o<opens>/<busy>/<closes>`，`opens>0` | 插件/daemon 真的打开了 AGX user client（`S` 行的 `backboarddP1` 只是只读 MACF 探测，**不算授权成功**） |
| 插件 / Turnip / 设备 | `K 6800 / 6801 / 6802` vs `68f1 / 68f2` | 进插件 / 找到 Turnip / 建出 device，或失败点 |
| 独立 raw test | `GPU7 R` | `adrenoprobed` 独立于 Metal discovery 的 sandbox-free 用户测试 |
| 呈现 | `GPU8 PUB tex` vs `PUB no device` | 合成器是否真的在出图 |
| ring | `GPU1 H ... R800005 r64/83` | hang 行 + RPTR/WPTR 差值 |

我们的 `X6 / X7 / R / Z1` 正好对应后四层。**当前 `M a5.0/5.0`、`o5/0/4` 都通过，断点在第三层 `68f2`** —— 这比我们之前盯着的第六层要早得多。

### 34.2 时间线（问题 → 根因 → 修复）

| 阶段 | 关键提交 / 文档 | 内容 |
| --- | --- | --- |
| 10-21/22 用户态底座 | `94b3963` `6b38d88` `3a17a7c`；`PROGRESS.md:207-227`；`RECOVERY-LOG.md` | dyld cache 在 SystemOS → `MNT_UNION` 覆盖 `/`；`boot-manifest-hash`；DBRDISK1 packed seed 扩容；`UkeLastLog` ramoops/msgbuf 落地、`CrashLogProbe.c` |
| 10-23 内核侧完成 | `GPU-PORT.md:138-144` | mode 9 `P published`，原文「remaining work is **userland** (NativeMetal plugin and scanout)」 |
| 10-27 addon 交付 | `de087c0` `beafe48` `563022e` `eb02136` `a84b25d` `110e012` `e6c69f5` | 内核内嵌 zlib blob + trust cache；**sandbox grant 属性错配**（只认 service property，驱动却设在 client object 上）；zlib `zalloc/zfree` 缺失；bindfs scratch 与 launchd env 向量冲突；补 WidgetRenderer 客户端 |
| 10-27 判据 | `GPU-PORT.md:827-833` | 「addon pipeline works end to end on hardware … `o4/0/2` … **Turnip's raw user test passes. What remains is entirely on the Metal frontend side**」 |
| **10-29 ABI 教训** | **`DPU-PORT.md:142`（§9.1）** | 驱动 rebase `5c8c145`、前端 `ce5e19e`、**Turnip 用新驱动 ABI 重编** → `o6`、`PUB tex`，GPU 合成器真正工作 |
| 10-30 parity 撞墙 | `2949667`(04:33) → `af004d4`(04:48) → stall dump 链 → `5ae72d7`(20:52) | 强制 mode 1，15 分钟后证伪；`d2bf71a`/`155281a`/`6e34c0d`/`6ad7cef`/`8a7cfd9` 一整天纯取证；换 24A446 + CPU rendering 起步，删掉 17 线 GPU 参数 |
| 11-03 27 线验收链 | `d76f775` `741be11` `ed60178` `0d52c01` **`229bc9a`/`f87eb65`** `242834b` `a89c607`/`2408530` **`c624e8f`** `c7aefb9` | surface track wrapper；只租 user SID0（板级 DT 缺口）；从 `lite_jobs` 移除 IconServices 禁用；`owner-dataprotect=1`；**Turnip `-Wl,-fixup_chains`**（旧 `LC_DYLD_INFO_ONLY` 让 Apple dyld 旧 arm64e 分支在 532 KiB 栈上分配 439304 B，与 IPS `x8=439304` 精确一致）；DeviceLost/CoreImage 空指针取证；撤回失败的全局 VFS source-union；**CoreImage 定向 `newLibraryWithURL` 重试**；用户验收 CRC `90B6BC701210D9DB` |

### 34.3 手法归类与工程纪律

- **真修**：`-Wl,-fixup_chains` 改 dyld 装载格式（不是加大线程栈）；`ed60178` 对齐上游移除禁用项（取代 `sheng-iconservices=1` 绕过）；`c624e8f` 只在「iOS27 + 本前端设备 + 原 nil + 8 个已知 CI 库名」时用**原 API** 重试，不伪造成功、不改 CoreImage 二进制。
- **明确标注的绕过**：临时禁用 iconservicesagent、`owner-dataprotect=1` xattr 兼容 shim、sandbox 只对 0x4147/0x4148 放行。
- **失败必撤回**：`a89c607` Revert + `2408530` 标 `withdrawn/do_not_flash`，并把「source-union 就是全部图标问题」改判为**失败假设**。
- 原则句（`GPU-IOS27.md:360`）：「**未通过关闭抢占、共享 VM 或吞错误规避**」。
- 取证纪律：UkeLastLog 按 boot UUID / 逐 slot 排除旧环（写指针每 boot 从 slot0 起）；IPS 按文件名哈希而非 mtime（设备时钟 1970）；统一日志按 boot UUID 排除上一 boot；**符号表 diff 证明实际构建版本**（`DPU-TASKS.md:565-599`，feeds pin ≠ 实际 commit）；单变量 A/B + 探针镜像先行；dump 必须分阶段塞进一条 16 KiB 记录（200 行一次性会被撕裂丢弃）。
- 验收三段分离：**构建/刷写成功 ≠ 功能通过 ≠ 压力验证**；每轮保留本 boot 现场，最终才写 `user-observation-success.json`。

### 34.4 对本次调试的结论

1. uke 17 线 parity 的 `PUB no device` + `CP ring stopped` 与我们是同一堵墙，且 `sheng-gpu-ttbr=1` 已被上游明确证伪 —— 我们 §33 独立复现，**不要在换表上再投入**。
2. 但 uke **10-29 曾在 17 线靠 ABI 重编让 GPU 合成器真正工作过**（`o6` / `PUB tex`），这是比「11-03 换 27 线」更贴近我们的先例；11-03 的验收是 27 线全新 runtime 的成果，**不能当作 17 线 blocker 已解**。
3. uke 给的 next_action（`af004d4`）：先 `sheng-gpu-metal=0` 单变量隔离，再回已知良好驱动 baseline `c950906` + 当前 display/Metal；`docs/status/gpu-parity-handoff.md` 不在本仓，隔离结果不可查。
4. 我们当前更直接的对应动作见 §33.5：先证明 `68f2` 到底卡在 `vkCreateInstance` / 物理设备 / `vkCreateDevice` 哪一步，再决定是重编 Turnip 还是对齐前端/驱动 ABI。

---

## §35 离线核对：ABI 其实完全配套 —— 订正 §33.4（2026-11-12 10:00）

设备不在 fastboot，本轮做纯离线核对。结论推翻了 §33.4 把 uke 10-29「ABI 版本不匹配」套到本板上的推测。

### 35.1 三者同属一个 ABI 世代（逐项证实）

| 组件 | 实际来源 | 证据 |
| --- | --- | --- |
| 内核驱动 | `drivers/Adreno740` @ **`22abfe4`**（uke 27 线验收 pin） | `git log -1`；`Gen-*` 脚本只生成板级 GMU/SMMU 常量头（`build-arm-driver.py:58,116-126,208-235`），不改 ABI；组装树该目录是符号链接指回同一处 |
| Metal 前端 | `drivers/AdrenoMetal` @ **`854f99a`** | `git log -1`，本地仅 6 行 `__arm64e__` 守卫 |
| Turnip | mesa `85fda3b3…` + 17 个 `patches/mesa/*`；`tu_knl_darwin.cc` sha256 `6ad95369…` | `gpu-build.json` / `builtin-build-profile.json` / `scripts/data/adreno-compiler-inputs.json` 三方一致 |

**决定性证据**：`scripts/build-turnip-darwin.py:42` 传 `-Dtu-darwin-abi-dir=<repo>/drivers/Adreno740`，`patches/mesa/turnip-darwin-kmd.patch:16-19,55` 把它加进 `libtu_includes`。即 **Turnip dylib 与内核驱动编译期 `#include` 同一份 `NativeAdrenoABI.h`**，结构布局/method 号/版本号不可能漂移。

逐项比对（Turnip `tu_knl_darwin.cc` ↔ 内核 `NativeAdrenoPublish.cpp` / `NativeAdrenoUserClient.cpp` / `NativeAdrenoABI.h`）：`IOAcceleratorES`、open type `0x4147`、tunnel magic `0x4e544e4c`、8 字节 tunnel 头、probe `0xffffffff`、18 个 method 编号、`AbiVersion=2`、`AbiParams` 24 B / `DeviceInfo` 64 B、`WaitFence`、BO/映射结构、`Submit` 3104 B、queue 0、全部 `static_assert` —— **全部一致**。`tu_knl_darwin.cc:3` 注释仍写 "version 1"，但代码用了 v2 专属的 `SetPriority`/`Note`，只是过时注释。旧 v1 头只存在于 `_archive/repos/old-modules/`，不在构建链。

⇒ **uke 10-29 的「旧 Turnip(v1) + 新驱动(v2)」在本板不存在**；`DPU-PORT.md:142` 的 marker 相同是**症状相同、根因不同**。§33.4 据此提出的「重编 Turnip / 对齐 ABI」不是当前动作。

### 35.2 `chip_id` 路径也排除

`board.toml:116` `chip_id=0x43030b00`（FD735）。`tu_knl_darwin.cc:1713-1720` 只有 `0x43050a01` 硬编码 `gpu_id=740`，本板算出 `gpu_id=4341`，看似会命中 `tu_device.cc:1733` 的 `VK_ERROR_INITIALIZATION_FAILED`。但生成表 `build/mesa-tu-ios/src/freedreno/common/freedreno_devices.h:105` 是

```c
{ {0, 0x43030b00}, "FD735", &__info21 },   /* __info21.chip = 7 */
```

而 `freedreno_dev_info.c:22-25` 的 `dev_id_compare` 只在 **`ref->gpu_id && id->gpu_id`** 时才按 `gpu_id` 比；表项 `gpu_id=0` ⇒ 走 chip_id 精确匹配 ⇒ `fd_dev_name` 返回 `FD735`、`fd_dev_gen=7` ⇒ 不会进 `default:` 的 `-3`。`gpu_id=4341` 不参与查表。**排除。**

### 35.3 `-3` 的精确落点被缩小

`nmb_device_open` 实际在 Rust 桥 `bridge/NativeMetalObjects.rs:171-199`，核心 `Device::system_default_with(&options).map_err(e("device"))`（`:193`）；`Vulkan(VkResultCode(-3))` 由 `error.rs:2391-2397` 的 `vulkan()` 统一映射。

| 步 | 调用 | 错误串 |
| --- | --- | --- |
| 1 | `vkCreateInstance` | `device: Vulkan(VkResultCode(<raw>))` |
| 2 | `vkEnumeratePhysicalDevices` → `tu_knl_darwin_load` → `dw_open_connection` | **同上** |
| 3 | `vkCreateDevice` → `darwin_device_init` → 第二次 `dw_open_connection` | **同上** |

三步错误串**完全一样**，单看 `R` 行无法区分（这正是 §33.5 第 2 条要加 stage marker 的原因）。但 `tu_CreateInstance` 不碰内核，所以 `-3` 只可能来自 (2) 或 (3)（`tu_knl_darwin.cc:194-217` 的 `IOServiceOpen` / probe selector / `Query` / `GetDeviceInfo` 四处）。

**连接层已通**：`X6 o5/0/4`（busy=0）、`B i48 o40 f0 s0 b0 n0`（`mount_fail` 全 0）、`X5 … c443`（external method 调用 443 次，含 `WaitFence` method 6）⇒ `IOServiceOpen`、selector 探测、`Query`、`GetDeviceInfo`、`mount`、`setScope` 都成功。uke 10-27 的 `NativeAdrenoMaxClients=4` 硬限在当前 `22abfe4` 里也已移除（改成 boot-arg `sheng-gpu-metal-opens`，默认 0 = 无上限）。

⇒ **`-3` 最可能落在 `vkCreateDevice` 阶段**，且与该阶段要建 queue / 提交初始化命令 / 等 fence 高度吻合 —— 这正好接回 ring 第 5 个提交卡住。

### 35.4 从 §33 照片下半段补挖出的硬证据

之前几轮只读了 `Z/T/R/X6`，本轮把 `H`/`F`/`D` 行也读了：

```text
MUYU H h59 r0 f0 f0 s0 g0 k0
MUYU F n1 s00000002 a2000001/1ffee y170d01
MUYU D1 R w f12 chip 43030b00 gmxc0 32/32/32/0
MUYU Z0 t21 f5 s400 a20000/1 y170d01 u1/1
MUYU Z1 r36/3d b20000/1 w0 h1/c/0 R2
```

字段定义（`BravearkLastLog.c:686-725`）：

- `H h59 r0` = 59 次 hangcheck、**0 次重启**（`native_adreno_recovery[0..7]`）。
- `F n1 s2 …` = `endFault()` **执行过 1 次**（§22.4 把 `endFault()` 移出 `if(g.restart)` 的修复确实编入并生效），FSR=`0x2`、FAR=`0x1ffee_a2000001`、FSYNR0=`0x170d01`。
- `Z0 s400` = stall 快照里 bank 的 FSR `0x400`，与 uke `GPU-SHENG-COMPARE.md:32-34` 记的「FSR=0x400、FSC=0、无真实 fault」一致 —— 所以 **`F` 那一次 fault 不是当前 stall 的持续原因**，§22 的「未终止 fault 永远卡住」假说至少不能由 `n1` 支撑（若持续 fault，`endFault` 计数应随 59 次 hangcheck 累加）。
- `Z1 R2` = `space->handleAt(GpuRingIova)` 返回 2，即 **ring 的 IOVA 在 stalled job 自己的地址空间里是映射的**；`b20000/1` 是 BR IB1 base/剩余 dword，`h1/c/0` 是 BV IB1/IB2 的 `handleAt()`。⇒ 不是 ring 页丢失、不是 IB 未映射。

### 35.5 更新后的排除清单与下一步

**已排除**：reset 段、prologue 全部、`emitFramedFence` BV 等待、ring 内容/PM4 合法性、WPTR 投递、fence 页映射、ring/IB IOVA 映射、mode 0/1/2 全族换表、TTBR0 与时序、Turnip↔驱动 ABI 版本/结构/method、`chip_id`/`fd_dev_gen`、客户端连接上限 / NoSpace / Busy、`IOServiceOpen`/`Query`/`GetDeviceInfo`/`mount`/`setScope`、持续 SMMU fault。

**剩下**：CP 本身停住（`Z5 fence_[1]=4`、`Z1 RPTR=0x36` 合法包前不动），而用户态在 `vkCreateDevice` 阶段拿不到结果。

按性价比排序的判决动作：

1. **跑 `adrenoprobed` raw user test** —— addon 里已有 `bin/adrenoprobed` 与 `daemons/org.blu-sharky.adrenoprobed.plist`，但 `X6 d00:0/131` 说明它**从未被 launchd 启动**。uke 用它把「user-client grant」与「Metal 前端」分开证明（`RECOVERY-LOG.md:145-148`）。先查 `GpuPowerProbe.cpp:1248-1270` 的 `e`（exec 授权）/`p`（plist 访问）计数，弄清为何没起；能起就能一次判定 Turnip 能否独立建 device。
2. **加 stage marker** —— 复用 `NativeTurnipSmoke.h:425-430` 的 `fail(stage,result)`→`mark(0x6f00|stage)`/`mark(0x6e00|(-result))`，把 `vkCreateInstance`/enumerate/`vkCreateDevice` 的失败点送进 `X7`；或直接在 `tu_knl_darwin.cc:194-217` 打印 `IOServiceOpen` 的 `kr` 与 `abi_version`。需重编插件或 Turnip + 重打 addon/trust。
3. **上游 next_action 的单变量隔离** —— `af004d4` 建议 `sheng-gpu-metal=0`（关 Metal publish 只留显示路径），看 ring 与 `F` 行是否仍出现，把「内核 bring-up 自身状态」与「用户态连接引入」分开。命令行已满，须改默认值。
4. 无论走哪条，先把 §24–§33 的实验开关（`kPerfctrInit`、`kSyncThreads`、`kTableNeedsReset`、`kResetPrologue`、`kPrologue`、`tableMode` 默认）**收回干净态**，避免开关叠加污染判读。

## §36 10:35 离线：`-3` 的因果链闭合到 WaitFence；把从未上屏的 CP 硬件诊断导出来

设备不在 fastboot，本轮不刷机。做法：补挖 §33 照片里没读的 `X4`/`X5`/上半段，并把 addon、trust、plist、Turnip/驱动代码逐项离线核对。

### 36.1 `X5 l6/2d6 f7`：`-3` 是「等设备创建初始化提交」的结果

`X5` 格式（`GpuPowerProbe.cpp:1219-1223`）是 `S<最近 open 的进程><P|F><MACF 结果> x<ext_entries> c<calls> f<inflight method+1> l<last_selector>/<last_status&0xfff> g<stage> u<forced_unmaps>`。§33 照片精确读数：

```text
MUYU X5 S SpringP0 x410 c407 f7 l6/2d6 g14 u0
MUYU X4 H14 R800005 r36/3d b3d i268000/0 j0 v0/0
MUYU X3 5d/0/0/0 10 P0/0 f400 a3/301 c1c
MUYU X2 (2) c0 d0 v0/0/0
```

- `l6/2d6` = 最后完成的 external method 是 **selector 6 = `NativeAdrenoMethodWaitFence`**（`NativeAdrenoPublish.cpp` method table 第 6 项），返回 `0x2d6` = **`kIOReturnTimeout`**。`f7` = 还有一个 method 6 正在 inflight。与 §24 的 `c=e00002d6` 同一个码。
- Turnip 侧 `tu_knl_darwin.cc:308-336`：`dw_wait_fence_raw` 把 `kIOReturnTimeout` 转成 `VK_TIMEOUT` 并继续 100 ms 轮询（不是 `-3`）；有界等待最终放弃后才由 `Device::system_default_with` 报 `Vulkan(VkResultCode(-3))`。`device:` 前缀来自 `bridge/NativeMetalObjects.rs:195` 的 `.map_err(e("device"))`，是整条设备创建链的标签，**不是** `vkCreateDevice` 的阶段标签。
- ⇒ **§33–§35 把方向改到「用户态 ABI / Turnip 加载」是不成立的**：用户态进程正常进了插件（`6800`）、找到 Turnip（`6801`）、连上了驱动、在反复等 fence；`-3` 是「ring 第 5 个提交永不完成」的上层表现。`6a3c` 只是 framebuffer 诊断位（`NativeAdrenoMetalPlugin.mm:278-295`，`getuid()!=0` + `MGCopyAnswer` 等），与失败无关。
- 唯一的另外三个 `-3` 分支在 `tu_knl_darwin.cc:197/204/215`（`IOServiceOpen` 失败、tunnel selector 全被拒、Query/GetDeviceInfo 失败）。驱动侧 `externalMethod`（`NativeAdrenoPublish.cpp:854-910`）确认 `probes`/`tunnel_calls`/`ext_entries` 都在计数，`X5 x410` 说明有 410 次 method 调用到达 ⇒ selector 已通，这三条排除。

### 36.2 `adrenoprobed` 未启动：交付完整，是加载时序；且 `L` 行永远不打印

- 解包 `fw/gpuaddon-17-full.bin`（`NGPUADD2`，10 项）：`bin/adrenoprobed`（111,408 B Mach-O）与三个 plist 都在；`daemons/com.apple.bluetoothd.plist`、`com.apple.iomfb_fdr_loader.plist` 是「借壳」系统 job，三者 `ProgramArguments` 都指向 `/private/var/ngpu/bin/adrenoprobed`，`RunAtLoad=true`。`GPU-ONDEVICE.md:1029-1031` 记的 addon→`/System/Library/LaunchDaemons` union 挂挂载路径也对。
- CDHash 硬核对：addon 里 3 个 Mach-O 是 `NativeAdrenoMetal 8d405db4…`、`libvulkan_freedreno.dylib 66bbfc6c…`、`adrenoprobed 0d2b839a…`；`fw/trust-21H461/owner-trust-17-full.bundle` 的追加模块（3 项）**逐字包含这三个**。`owner-trust-17.bundle` 只有前两个（旧版缺 daemon），当前构建用的是 full 版，所以**不是 trust cache 漏签名**。
- 注意 `UniXNU/scripts/build-boot-runtime.py:167`：`addon_hashes()` 只在 `--owner-trust-cache` 省略时才会自动补 CDHash；我们传的是预生成 bundle，等于跳过了这层校验 —— 本次靠手工解包确认无误，但这是个流程隐患。
- `d00:0/131` = `d<everSeen><alive>:<pid>/<procCount>`，daemon 从未出现。既然二进制/plist/trust/挂载点都齐，剩下只能是 launchd 加载时序（plist 在 launchd 扫目录之后才挂上）。**但这条线索在照片里查不下去**：`gicRow()` 的调用条件是 `if (!(gxOn() && RPTR != WPTR)) gicRow();`（`GpuPowerProbe.cpp:1576`），ring 一直不 drain ⇒ `L e<exec 授权>/<plist 读取> …` 行**永远不会被写**，row 2/3 又被后续阶段覆盖。§35 下一步第 1 条（查 `L` 行）作废。

### 36.3 关键：CP 停滞的硬件诊断早就在采，但从来没上过屏

`GpuPowerProbe.cpp:1521-1554` 的 stall 分支已经采集三行：

| 行 | 内容 | 为什么从没显示 |
| --- | --- | --- |
| `S%x/%x/%x I%x P%x/%x f%x a%x/%x c%x` | RBBM_STATUS1..3（STATUS3 bit 24 = SMMU stalled on fault）、`CP_INTERRUPT_STATUS` 0x823、BR/BV `CP_PROTECT_STATUS` 0x824/0xa61、bank FSR、ACTLR/SACR | 写进 `setRow(3)`，被后段覆盖 |
| `Q br %x %08x bv %x %08x` | 每个 CP 线程的 SQE PC 和**它当前所在那条包的包头**（0x908/0x909、0xa87/0xa88） | 写进 `setRow(2)` 后**同一分支内立刻被 `C` 行覆盖** |
| `C%u c%x u%x v%x d%x` | IB 缓存一致性采样：`c` = kernel cacheable alias 读到的第一个 IB word，`u/v` = CPU 缓存维护前/后的 DRAM，`d` = 现在 DRAM | 写进 row 2，被后段覆盖 |

`GpuPowerProbe.cpp:1542-1545` 的注释记着 uke 的硬教训（int-069）：「表项对、DRAM 里 IB 也在，但 CP 读到全零头；`u=0` 而 `c/v=0x70970001` 说明 IB 卡在 CPU cache line 里」——**「CPU 写了 ring、CP 读到旧/零数据」正好解释 RPTR 冻结而 WPTR 随 prologue 长度照涨（§31–§33 四轮）**，且解释了为什么改 ring 内容的所有实验都无效。这是目前唯一没被排除、且与全部观测自洽的假说。

### 36.4 本轮改动（只加诊断，不改行为）

- `UniXNU-muyu-17/package/GpuPowerProbe.cpp`：新增全局 `char muyu_cp_stall_rows[3][64]`；stall 分支在每次 `snprintf` 后把 `S`/`Q`/`C` 三行 `strlcpy` 进去（放在 `setRow` 覆盖之前）。
- `UniXNU-muyu-17/package/BravearkLastLog.c`：仿 `muyu_gpu_diag` 的 weak_import 方式，打印 `MUYU Q0/Q1/Q2 <row>`。导出方式与 `GpuPowerProbe.cpp:175 char muyu_gpu_diag[6][48]` 一致。
- 镜像 `fw/vendor_boot_gpu_cpstall1.img`（沿用 §24.9 三步构建链）。**行为零改动**：仍是 mode 0 + `kPrologue=false` 的上一轮基线，方便与 §33 读数逐字对照。

判读格：

- `Q0` 的 `S…` 第三段（STATUS3）bit 24 置位 ⇒ SMMU 真 stall 在 fault 上；`I`（CP interrupt）、`P`（BR/BV protect）非零 ⇒ CP 触发保护错误，转查对应寄存器。
- `Q1` 的 `br <PC> <header>` / `bv <PC> <header>`：**header 全零** ⇒ CP 没读到我们写的包；PC 停在合法包但不动 ⇒ CP 状态机/中断问题。
- `Q2` 的 `C n c<u> u<v> v<w> d<x>`：`u=0` 而 `c/v` 是合法 PM4 头 ⇒ **缓存一致性问题坐实**，下一步在 `submitWords()` 写 WPTR 之前对 ring/IB 做一次 dcache clean-to-PoC；`u/v/d` 都是合法 PM4 头 ⇒ DRAM 有数据，转向 CP 取指使能/`CP_RB_CNTL`/doorbell。

若三行都没打印，说明 `heartbeat()` 的 stall 分支没进入（`gxOn()` 或 RPTR==WPTR），那就得改从 hangcheck 内部导出。

## §37 2026-11-12 10:57 CP 硬件诊断上屏：状态全零、当前包头全零 ⇒ 改查 CP 自己的 ring 配置

镜像 `fw/vendor_boot_gpu_cpstall1.img`（`cfb61563…`），照片 `logs/2026-11-12-10-57-14-713.jpg`（倒置，须 `hflip,vflip`）。新增的三行**都打印出来了**，判读如下。

```text
MUYU Q0 S0/0/0 I0 P0/0 f400 a3/301 c1c
MUYU Q1 Q br 215 00000000 bv 132 00000000
MUYU Q2 C0 c0 u0 v0 d0
MUYU Z0 t21 f5 s400 a200000/1 y170d01 u1/1
MUYU Z1 r36/3d b20000/1 w0 h1/c/0 R2
MUYU Z2 24 40088501 00000005 70460004 00000031 00300004 ...
MUYU Z5 f32/4/0/0
MUYU X7 K4 6800 6801 6a3c 68f2
```

### 37.1 硬件侧完全干净，且两级恢复都做过

- `Q0 S0/0/0` = RBBM_STATUS1/2/3 **全 0**：没有 SMMU stall（STATUS3 bit24=0），没有 CP busy/error。`I0` = `CP_INTERRUPT_STATUS`(0x823)=0，`P0/0` = BR/BV `CP_PROTECT_STATUS`(0x824/0xa61)=0，`f400` = FSR 0x400 且 FSC=0（无终止的翻译 fault）。⇒ **没有任何硬件错误、保护违例或挂起中断在拦 CP**。
- `Q1 br 215 00000000 bv 132 00000000`：`CP_SQE_STAT` 的两个线程微码 PC 非零（BR 0x215 / BV 0x132，说明微码跑过），但**当前 opcode/包头读出全零**。
- `Z0 u1/1` = `native_adreno_unstick[0]`/`[4]`（`BravearkLastLog.c:716`）= 1/1 ⇒ **stage 1（`invalidateTlb()+kick()` 重写 WPTR）和 stage 3（`drainBus()`）都执行过一次**，都没解开。`Z1 r36/3d` 里 WPTR 是 `stallState()` 从 `CP_RB_WPTR` **寄存器读回**的 0x3d，与 CPU 侧 `next_` 一致 ⇒ WPTR 写入确实落地，排除「写丢」。`native_adreno_wptr_fence` 见 §37.5。
- `Q2 C0 c0 u0 v0 d0`：`native_adreno_coherence[0]`=0 ⇒ **采样一次都没跑**。该采样在 `NativeAdrenoSubmit.cpp:465-478` 的 `publishMapped()` 客户端 buffer 提交路径上（每 64 次一次），与 ring 提交是两条路；计数 0 说明**至今没有任何客户端 IB 提交**，与「用户态卡在设备创建、`X5 l6/2d6` WaitFence 超时」一致。⇒ Q2 **不能**用来判定 ring 缓存一致性，§36 的 int-069 假说本轮未获支持也不被否定。

### 37.2 与 uke 的 int-317 对照

`NativeAdrenoSubmit.cpp:608-613` 的原文注释：

> "A fence that stops while the CP sits idle with work in the ring (**int-317 hung with RBBM idle, RPTR short of WPTR, both threads at the same cache clean**) is nearly always **a write pointer the CP never took**: giving it again after a few waits ends it in a couple of milliseconds (i343: 206 of 213 stalls ended at the second stage...)."

前半与我们的 `Q0` 全零 + `r36/3d` 逐字吻合，**但后半不成立**：`kick()` 给过 WPTR、`drainBus()` 也做过，fence 仍 4。⇒ 本板不是 uke 那个「WPTR 没被接到」的成因。

### 37.3 收敛出的新假说：CP 取指用的 ring 基址/控制不对

把三件事实并起来：CPU 侧映射里 RPTR 处是合法 `emitFence`（`40088501 00000005 70460004 …`）；WPTR 寄存器正确、硬件无任何错误；而 CP 的 `CP_SQE_STAT` 当前包头是**全零**。最自洽的解释是 **CP 实际去取指的地址不是我们写入的那块 ring** —— 即 `CP_RB_BASE` / `CP_RB_CNTL` / `CP_RB_RPTR_ADDR` 被覆盖或从未落在正确值。期望值（`NativeAdrenoGpuContext.h:17`）：`CP_RB_BASE=0x200000`、`CP_RB_RPTR_ADDR=0x300000`、`CP_RB_CNTL=0x20c`（BUFSZ=12、BLKSZ=2）。

`program()`（`NativeAdrenoRing.cpp:443`）只在 `attach()` 与 `restart()` 时写这些寄存器；`kick()` 只写 WPTR，`drainBus()` 只碰 GBIF/AXI。**从来没有任何路径在 stall 之后重新声明 ring**。

### 37.4 本轮改动：`reassert()` + 两行配置诊断

1. `NativeAdrenoRing.h/.cpp` 新增 `reassert()`：用 `program()` 的同一组寄存器写（`CP_RB_BASE` lo/hi、`CP_RB_CNTL`、`CP_RB_RPTR_ADDR` lo/hi）重新声明 ring，然后 `writeWptr(next_)`；**不写 WPTR=0、不清 scratch、不 `next_=readRptr()` 回退**，所以不会重放已完成的工作。
2. `NativeAdrenoSubmit.cpp` stage 3 在 `drainBus(ack)` 后调用 `stalled->reassert()`，并把次数记进 `native_adreno_unstick[14]`。
3. `GpuPowerProbe.cpp` 的 `muyu_cp_stall_rows` 扩到 `[5][64]`，新增：
   - `Q3 P b<base_lo>/<base_hi> c<CNTL> r<rptraddr_lo>/<rptraddr_hi> s<SQE_CNTL> f<HW_FAULT> t<STATUS1>` —— **直接对比 0x200000 / 0x20c / 0x300000**；
   - `Q4 W<writes>/<dropped once>/<repeats>/<dropped after 200> u<kick>/<drain>/<reassert> a<client ack>/<arbiter ack>` —— `native_adreno_wptr_fence[4]`（`NativeAdrenoRing.cpp:114`）与 `native_adreno_unstick[15]` 首次上屏。
4. `BravearkLastLog.c` 打印循环扩到 5 行（`MUYU Q0..Q4`）。

镜像 `fw/vendor_boot_gpu_reassert1.img`（沿用 §24.9 三步构建链）。基线仍是 mode 0 + `kPrologue=false`。

### 37.5 判读格

- **`Z5 fence_[1] ≥ 5`** ⇒ `reassert()` 解开了停滞，根因就是 CP 的 ring 配置丢失；接着查是谁覆盖的（`restart()` / GMU 上电 / SMMU 重配）。
- **`Q3` 的 `b`/`c`/`r` 与 0x200000 / 0x20c / 0x300000 不符** ⇒ 假说直接坐实，`reassert()` 若仍无效就查写入被谁改掉或 CP 是否在用 secondary ring 的 record。
- **`Q3` 全对但 fence 仍 4** ⇒ 排除 ring 配置，转查 `Q4`：`W` 的第 2/4 项非零 = GMU AHB fence 丢过 WPTR 写；`u` 第三段应 ≥1 确认 reassert 真的执行。
- **`Q4 u0/0/0`** ⇒ stage 3 没进（`clock->now_us` 为 0 或 `pending` 不足 200 ms），改从 `hangCheck()` 直接强制恢复。
- 对照格：`Z0 u1/1` 应保持，`Q0/Q1` 应与本轮一致，`T` 应保持 `d0 s0/0 g==k`。

### 37.6 待刷期间的离线预分析（2026-11-12 11:1x，设备不在 fastboot）

镜像 `fw/vendor_boot_gpu_reassert1.img` sha256 `03ca2e47f1b9c29c6ba2e504b3bcdb8012a82ceedde604ea57895bc628d44c4e`（77,393,920 B）。三步构建日志 `/tmp/build_k37.log`、`/tmp/build_p37.log`、`/tmp/build_v37.log` 全部成功；`strings` 能在镜像里找到 `__ZN16NativeAdrenoRing8reassertEv` 与 `_native_adreno_reassert_ticks`，而 `cpstall1` 没有 ⇒ 新代码确实进了镜像，不是旧 payload 复用。

离线复核（只读）得到的判读补充：

1. **`reassert()` 与 `program()` 的寄存器集合逐字一致**（`NativeAdrenoRing.cpp:190-204` vs `:460-472`），只少 `CP_RB_WPTR=0` 与 `RingScratch=0` 两个破坏性写，也不做 `next_=readRptr()` 回退。触发点 `NativeAdrenoSubmit.cpp:625-633` 在 stage 3（`pending>=200 ms`）`drainBus()` 之后，`unstick[14]++` ⇒ `Q4 u` 第三段 ≥1 才说明真的执行过。
2. **`Q3` 的寄存器映射**（`GpuPowerProbe.cpp`）：`b`=0x800/0x801 `CP_RB_BASE(_HI)`、`c`=0x802 `CP_RB_CNTL`、`r`=0x804/0x805 `CP_RB_RPTR_ADDR(_HI)`、`s`=0x808 `CP_SQE_CNTL`、`f`=0x821 `CP_HW_FAULT`、`t`=0x825 `CP_STATUS_1`。期望 **`b200000/0 c20c r300000/0 s1 f0`**。
3. **若 `Q3 s0`（SQE 被关）**：这是 `reassert()` 修不了的一类。`CP_SQE_CNTL=1` 全工程只在 `GpuPowerProbe.cpp:1462` 的 hw_init 写过一次，`CP_SQE_INSTR_BASE` 也只在 `:1452` 写；GMU 掉电/IFPC/CP reset 之后没有任何路径重新声明。`reassert()` 只动 ring 三个寄存器 ⇒ 即便 `b/c/r` 全对，SQE 关着 CP 一样不取指。下一轮应把 `CP_SQE_CNTL=1`（必要时连同 `CP_SQE_INSTR_BASE`）加进 `reassert()`，而不是继续改 ring 内容。
4. **硬件上下文（host aperture）线索**：`CP_ME_INIT` 配了 3 个 hardware context（`NativeAdrenoCpInit.cpp:42`），hw_init 期间 `CP_APERTURE_CNTL_HOST`(0xa00) 被按 pipe 1/2 写过、最后置 0（`:1443-1446`）。`Q3`/`stallState()` 都经同一个 host aperture 读，如果 CP 实际跑在另一个 context，我们会看到「寄存器全对但 CP 读到全零头」而毫无察觉。判据是 `Q3` 全对、`s1`、`f0`、`Q4 u..≥1` 且 fence 仍 4 ⇒ 下一条路是显式把 `CP_APERTURE_CNTL_HOST` 设成 CP0 再重读/重写。
5. **`Q4 W` 的语义**：`native_adreno_wptr_fence[0..3]` = WPTR 写次数 / 曾丢一次 / 重试 / 200 次后仍丢。cpstall1 的 `Z0 u1/1` 说明 stage1/stage3 都跑过，所以本轮 `Q4 u` 应为 `1/1/≥1`。
6. 本轮设备既不在 fastboot 也不在 adb，未刷机、未读新照片；已挂一个有界（30 min）的等待器，设备一进 fastboot 就走 `MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_reassert1.img`（只写 `vendor_boot`，UEFI 只从 RAM boot，写前校验 serial 与 sha）。等待器被取消后未重启；11:27 设备进 fastboot，改用同一条命令单次执行，`flash` 与 `boot` 都 OKAY、ledger `rc=0`（电池 3606 mV 偏低）。

## §38 2026-11-12 11:30 reassert1 上屏：**CP ring 配置全对，reassert 无效且把 RPTR 打回 0**；找到 17 线缺的 GPU restart hook

镜像 `fw/vendor_boot_gpu_reassert1.img` `03ca2e47…`，照片 `logs/2026-11-12-11-30-02-986.jpg`（倒置，`ffmpeg -noautorotate -vf "hflip,vflip"` 转正）。新增 `Q3/Q4` 两行都打印了。

```text
MUYU Q0 S0/0/0 I0 P0/0 f400 a3/301 c1c
MUYU Q1 Q br 215 00000000 bv 132 00000000
MUYU Q2 C1 cffffffff u0 v0 d0          (X2 同值)
MUYU Q3 P b200000/0 c20c r300000/0 s1 f0 t00000206
MUYU Q4 W7/0/0/0 u1/1/1 a3/3
MUYU Z0 t21 f5 s4xx(照片模糊) a200000/1 y170d01 u1/1
MUYU Z1 r0/3d b20000/1 wffffff000c h11/c/0 R2
MUYU Z2 24 70460001 00000031 7bc00004 0000014b 00000003 2000000x ...
MUYU Z2 36 70460004 00000031 00300004 00000000 ...
MUYU Z3 v100009/cff42000
MUYU Z5 f0/4/0/0
MUYU T a1/0 c0 d0 w0/0 s0/0 g2/cb531000 k2/cb531000
MUYU W 全 f
MUYU R PUB no device: device: Vulkan(VkResultCode(-3))
MUYU X3 S0/0/0 I0 P0/0 f400 a3/301 c1c
MUYU X4 H112 R800005 r0/3d b3d i28000/0 j0 v0/0
MUYU X5 S SpringP0 x390 c387 f7 l6/2d6 g14 u0
MUYU X6 N a5.0/5.0 k0 s5/0 r1 o5/0/4 n3 d00:0/132
MUYU X7 K4 6800 6801 6a3c 68f2
MUYU H h32 r0 f0 f0 s0 g0 k0
MUYU F n1 s00000002 a200000/1 y170d01
MUYU B i48 o40 f0 s0 b0 n0
MUYU D1 V chip 43030b00 gmxc0 32/32/32/0
```

### 38.1 §37 假说被直接证伪

`Q3` 是本次最重要的一行：`b200000/0` = `CP_RB_BASE` 精确等于 `GpuRingIova=0x200000`，`c20c` = `CP_RB_CNTL`（BUFSZ 12 / BLKSZ 2，与 Linux `MSM_GPU_RB_CNTL_DEFAULT` 一致），`r300000/0` = `CP_RB_RPTR_ADDR` 等于 `GpuMemptrsIova=0x300000`，`s1` = `CP_SQE_CNTL` SQE 已使能，`f0` = `CP_HW_FAULT` 无错，`t00000206` = `CP_STATUS_1`。**期望值逐项全对**，§37.3 的「CP 取指用的 ring 基址/控制被覆盖」不成立。

`Q4 W7/0/0/0 u1/1/1 a3/3` 补上最后两块拼图：7 次 WPTR 写、0 次首次丢失、0 次重试、0 次 200 次后仍丢（`native_adreno_wptr_fence`，经 `GMU_AHB_FENCE_STATUS` 校验，与 Linux `fenced_write` 的 `!(status & BIT(0))` 判据一致）；`u1/1/1` = kick / drainBus / **reassert 各执行一次**，`a3/3` = GBIF client+arbiter halt 都应答。`Q0/Q1` 与 cpstall1 逐字相同（状态全零、SQE PC 0x215/0x132、包头全零），`H h32 r0`、`F n1` ⇒ 无持续 fault、无重启。

⇒ **CP ring 配置、WPTR 投递、SQE 使能、硬件错误四条线全部排除；但 `Z5 f0/4/0/0`，最后一个完成的 fence 仍是 4，reassert 没解开。**

### 38.2 reassert() 的副作用：把 CP 的 RPTR 打回 0

`Z1 r0/3d`（cpstall1 是 `r36/3d`）、`Z5 f0/4/0/0`（cpstall1 是 `f32/4/0/0`）：重写 `CP_RB_CNTL` / `CP_RB_RPTR_ADDR` 之后，硬件 `CP_RB_RPTR`(0x806) 与 memptrs 里的 rptr shadow 双双归零。`stallState()` 读的是**寄存器**不是 shadow（`NativeAdrenoRing.cpp:161-167`），所以这是真实的 RPTR 复位。此时 RPTR=0、WPTR=0x3d、`Z2 24` 处是合法 PM4 字，CP 依然不取指。**reassert 不但无效，还破坏了原有状态**；§38 起从 stage 3 撤掉，恢复可比性。

### 38.3 最值钱的离线发现：17 线从来没有注册 GPU restart hook

`H h32 r0` 里的 `r0` 不是「没发生 hang」，而是**没人实现恢复**：`NativeAdrenoPublish.cpp:214-238` 的 hangcheck 只有在 `g.restart` 非空时才调 `NativeAdrenoSubmit::recover()`，而 `native_adreno_set_restart()` 在本仓库**没有任何调用点**。对照 `UniXNU-uke/package/GpuPowerProbe.cpp:1769`：

```cpp
if (native_adreno_set_restart) native_adreno_set_restart(gpuRestart, nullptr);
```

uke 在 `gpuStart()` 发布前注册了 `gpuRestart`（`:1621-1651`：GBIF/GX halt → `RBBM_SW_RESET_CMD` 拉高 >100 µs → `gpuHwInit()` 重放 hw_init → `ring->restart(completed)` → `CP_SQE_CNTL=1` → 重新跑 `CP_ME_INIT` → `ring->idle()` → 必要时 `CP_SET_SECURE_MODE`），并把 hw_init 抽成 `gpuHwInit()`（`:1560`）供重启复用。**muyu-17 的 `gpuStart()` 把同一段 hw_init 内联，结尾直接 `publishMetal()`，没有 hook。** 这正好解释了为什么 CP 停在那里 32 次 hangcheck 而毫无动作，以及为什么 uke 能走到 `o6/PUB tex` 而我们卡在 `68f2`。

### 38.4 §38 的改动（镜像 `fw/vendor_boot_gpu_restart1.img` sha256 `29963d92…976b99976`，77,393,920 B；三步链 `/tmp/build_{k,p,v}38.log` 全成功，`strings` 可见 `MUYU RT`、`G i%x…`、`_native_gpu_restart` 与 `gpuRestart` 符号）

1. `NativeAdrenoSubmit.cpp` stage 3 撤掉 `reassert()`（§38.2），`unstick[14]` 保持 0 作为「实验已关闭」标记。
2. `GpuPowerProbe.cpp`：把 gpuStart 的 step-3 hw_init 原样抽成 `gpuHwInit()`；移植 uke 的 `gpuRestart()` 并在发布前 `native_adreno_set_restart(gpuRestart, nullptr)`；新增 `native_gpu_restart[4]`（重启次数 / 失败步骤 / 状态 / 复位后 RBBM_STATUS）。
3. 诊断只增不改行为：`muyu_cp_stall_rows` 扩到 `[7][64]`，新增
   - `Q5 G i<0x812 CP_CP2GMU_STATUS/IFPC> a<0xa00 CP_APERTURE_CNTL_HOST> q<0x830/831 SQE_INSTR_BASE> m<0x840 CP_MISC_CNTL> r<0x806 RPTR>/<0x807 WPTR> s<0x210 RBBM_STATUS> t<0x825 CP_STATUS_1> b<0x201 RBBM_INT_0_STATUS>`；
   - `Q6 S br <3 个 SQE_STAT 后续字> bv <...>`，看 SQE 是否停在固定 WFI/idle 循环。
   `BravearkLastLog.c` 循环扩到 7 行，并新增 `MUYU RT n<restarts> s<step> e<status> b<rbbm>`。

### 38.5 判读格

- **`H` 的 `r>0` 且 `RT n>0 s0`、`Z5 fence_[1] ≥ 5`** ⇒ restart hook 解开了停滞，17 线的断点就是「缺恢复路径」；接着看 Metal 是否越过 `6802`。
- **`RT n>0 s≠0`** ⇒ 重启跑到第几步失败（`2` busHalt / `3` ring restart / `4` CP_ME_INIT / `5` idle / `6` secure mode），按 `e` 的 IOReturn 修。
- **`RT n0` 且 `H h.. r0`** ⇒ hook 没注册成功或 hangcheck 没判 hung，转查 `native_adreno_set_restart` 的 weak 链接与 `hangCheck()` 门槛。
- **`Q5 i` bit0=1** ⇒ CP 在 IFPC 掉电态；`a` 非 0 ⇒ host aperture 指向别的硬件上下文；`q` 不是 `100000/0` ⇒ SQE 微码基址被改。
- **`Q6` 两线程四个字恒定** ⇒ SQE 卡在固定 idle/WFI，重启是唯一出路。
- 对照：`Q0/Q1` 应与前两轮一致；`T` 保持 `d0 s0/0 g==k`；`X5 l6/2d6`、`X7 ...68f2` 若消失即上层已解。

## §39 2026-11-12 12:10 交接续做：restart1 离线复核 + 刷入（待本 boot 屏照）

### 39.1 待刷期间的离线复核：镜像可用，下一轮不会白刷

- 镜像 `fw/vendor_boot_gpu_restart1.img` sha256 `29963d92…976b99976`（77,393,920 B）与 §38.4 一致；`describe` = vendor_boot v4 / 73.81 MiB / ramdisk 72.00 MiB / aligned=True。三步链 `/tmp/build_{k,p,v}38.log` 12:02 完成，`error:|warning:|FAILED|undefined` 全空。
- `strings -a` 在 restart1 内能查到 `MUYU RT n%u s%u e%x b%x`、`_native_gpu_restart`、`_native_adreno_set_restart` 与 `gpuRestart` 的 C++ 符号；reassert1 只有 `_native_adreno_set_restart`，没有 `MUYU RT` / `gpuRestart` ⇒ §38 新代码确实进了镜像。
- 注册路径与 uke 一致：`GpuPowerProbe.cpp:1556` 在 `c[0]==10`（bring-up 自检通过）之后、`publishMetal()` 之前注册，uke 在 `:1769` 同一位置。上一轮屏幕已有 `X7 …68f2` 说明 publishMetal 走到过 ⇒ 本轮 `c[0]==10` 成立，hook 会装上。
- `NativeAdrenoPublish.cpp:219/230` 的 hangcheck 每 500 ms 判一次，`H h32` 证明 `hung=true` 已成立 32 次。`NativeAdrenoSubmit::recover()`（`NativeAdrenoSubmit.cpp:802`）**不等 inflight 的 WaitFence**，算出 `fence` 后直接 `restart(cookie, ring(0), hung)` ⇒ `X5 f7` 那个 inflight method 不会把恢复卡死。
- `gpuRestart()`（muyu-17 `:1409-1444`）与 uke `:1621-1651` 逐段一致：busHalt → `RBBM_SW_RESET_CMD` 拉高 100 µs → `gpuHwInit()` → `ring->restart(completed)` → `CP_SQE_CNTL=1` → CP_ME_INIT → idle → 必要时 secure mode。`gpuHwInit()`（`:1356`）是纯寄存器重放，不重挂 ring、不重分配 buffer，可安全重复调用。
- 新增的 `muyu_cp_stall_rows[5]/[6]`（Q5/Q6）与 Q0–Q4 **同在** `heartbeat()` 的 `gxOn() && RPTR!=WPTR` 分支（`:1635/:1646`），reassert1 已证明该分支会进 ⇒ 只要出现停滞，Q5/Q6 就会上屏，不受 §36.2 那个永远不打印的 `L` 行门控影响。

### 39.2 刷机记录（12:10，只写 vendor_boot）

`MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_restart1.img`

| 项 | 值 |
| --- | --- |
| preflight | slot=a / unbootable=no / retry=6 |
| vendor_boot | `OKAY [0.048s]`，sha `29963d92…976b99976` |
| UEFI | `fw/boot-17line-043f7120.img` 从 RAM `Booting OKAY [0.188s]` |
| ledger | `04:10:55Z` flash / boot 两条 `rc=0`；未碰 boot / recovery / userdata |
| ⚠️ 电池 | **3424 → 3420 mV**（上一轮 3606），脚本两次告警 `<3700 mV`；长跑前务必充电 |

刷后 100 s `fastboot devices` 为空 ⇒ 没有回落 fastboot，设备仍在启动/用户态；`logs/` 最新照片仍是 11:30 的 reassert1，本机无 USB 调试串口（boot-args 无 `muyu_usb=1`），本 boot 的读数只能靠屏幕照片。

### 39.3 还差的一步

拍一张本 boot 的屏幕发到 `logs/`（照片照例倒置，转正用 `ffmpeg -noautorotate -vf "hflip,vflip"`）。判读沿用 §38.5，优先三行：

1. `MUYU RT n… s… e… b…` —— `n>0 && s0` 且 `H r>0` ⇒ restart hook 生效；再看 `Z5 fence_[1] ≥ 5`、`X7` 是否越过 `6802`、`R PUB` 是否变成 `tex`。
2. `RT s≠0` ⇒ 按失败步骤（`2` busHalt / `3` ring restart / `4` CP_ME_INIT / `5` idle / `6` secure mode）和 `e` 的 IOReturn 修。
3. `RT n0` ⇒ hook 没装上或 hangcheck 门槛没过，转查 weak 链接；同时用 `Q5 i/a/q` 判 IFPC / aperture / SQE 微码基址，`Q6` 判 SQE 是否停在 idle/WFI。

若重启后 ring 已 drain，`Q0…Q6` 仍保留最后一次停滞的采样（全局数组不清零），可对照 §38 的 `Q0/Q1` 基线。

## §40 2026-11-12 12:14 restart1 上屏：**重启成功却没解开断点**，§38 假说证伪；随后刷回原厂待充电

照片 `logs/2026-11-12-12-14-41-437.jpg`。⚠️ **这张照片是正立的**（EXIF orientation=1），与前几轮「倒置、需 `hflip,vflip` 转正」不同；对它做 180° 反而会倒过来。判读用原图 `-noautorotate` 裁切 + Vision OCR 双重核对。照片方向变了，后续每轮先判方向再读。

### 40.1 完整读数

```text
MUYU RT n1 s0 e0 b1
MUYU H  h1 r1 f0 fS s0 p0 g1 k0
MUYU Z0 t2 f5 s80000402 a28000/1 y170001 u1/1
MUYU Z1 r36/3d b20000/1 w0 h11/c/0 R2
MUYU Z2 24 40088501 00000005 70460004 00000031 00300004 ...
MUYU Z5 f32/4/0/0
MUYU T  a1/0 c0 d0 w0/0 s0/0 g2/cb69e000 k2/cb69e000
MUYU W  ffffffff ...                         MUYU F n1 s00000002 a200000/1 y170d01
MUYU R  PUB no device: device: Vulkan(VkResultCode(-3))
MUYU X7 K4 6800 6801 6a3c 68f2
MUYU X5 S SpringP0 x95 c93 f0 l6/0 g14 u0
MUYU X6 M a5.0/5.0 k0 s5/0 r1 o5/0/4 n3 d00:0/127
MUYU Q0 S0/0/0 I0 P0/0 f400 a3/301 c1c
MUYU Q1 Q br 332 70460004 bv 132 00000000
MUYU Q2 C0 c0 u0 v0 d0
MUYU Q3 P b200000/0 c20c r300000/0 s1 f0 t0
MUYU Q4 W3/0/0/0 u0/0/0 a0/0
MUYU Q5 G i0 a0 q100000/0 m0 r2a/2a s1 t0 b0
MUYU Q6 S br 70460004/0/31 bv 0/70460004/3
MUYU PROBE st=97 cp=10 b=24             MUYU B i48 o40 f0 s0 b0 n0
MUYU GPU calls=7 refused=7 o=0 c=0 f=0 fb=0 tc=7 ts=00000000 hold=1 pres=1 | w=0
```

### 40.2 restart hook 生效了，但**不是断点**

`RT n1 s0 e0 b1` = 重启 1 次、失败步骤 0、IOReturn 0、复位后 `RBBM_STATUS=1`；`H h1 r1`（旧为 `h32 r0`）证明 hangcheck 这次真的走到 `recover()` 并调了 hook。§38 说的「17 线没人实现恢复」已修好。

**但 `Z5 f32/4/0/0`、`R PUB no device`、`X7 …68f2` 三项原样不动。** ⇒ 补上恢复路径没有让用户态拿到 device，**§38「断点就是缺 restart hook」的核心假说被证伪**。`-3` 的根因不在「卡住之后没人救」。

### 40.3 重启确实改变了 CP 的状态，只是没让它推进

| 项 | reassert1（§38） | restart1（本轮） |
| --- | --- | --- |
| `Q1` BR 当前包头 | `00000000` | **`70460004`**（合法 PM4 头） |
| `X5 l<sel>/<status>` | `l6/2d6`（timeout） | **`l6/0`** |
| `X5 c<calls>` | `c387` | **`c93`** |
| `Q6` SQE_STAT | 无 | `br 70460004/0/31 bv 0/70460004/3` |
| `H h/r` | `h32 r0` | `h1 r1` |
| `Z5 fence_[1]` | 4 | **4（未变）** |

⇒ §36 的「CP 读到全零头 / 缓存一致性」假说也随之失效：重启后 CP 面前摆着格式正确的包，SQE 也停在这个包上，但 `Z1 r36/3d` 与 §32 逐字相同，**RPTR 依旧不动**。

### 40.4 §38.5 的三条硬件判据全部排除

`Q5 G i0 a0 q100000/0 m0 r2a/2a s1 t0 b0`：`i0` = 不在 IFPC 掉电态；`a0` = host aperture 指向 context 0；`q100000/0` = `CP_SQE_INSTR_BASE` 与期望值一致。`Q0` 状态寄存器全零、`f400`（FSR 无真实 fault）、`Q2 C0`（coherence 采样 0）、`Q4 W3/0/0/0`（3 次 WPTR 写、0 丢失）、`Q3` ring 基址/控制全对。硬件侧找不到任何「拦住 CP」的理由。

基线干净可比：`T d0 s0/0 g==k`（mode 0 保持）、`Z1 w0` / `Z5 f32`（§38.2 的 reassert 归零副作用已消失）、`Q4 u0/0/0`（三级 stall 恢复没跑，hangcheck 直接走 `recover()`）。

⚠️ `MUYU GPU calls=7 refused=7 …` **不要误读成「Metal device 调用全被拒」**：它是 GOP/legacy display 的帧 offer 桥接计数（`LegacyDisplayProbe.cpp:69-98` 的 `muyu_handoff_snapshot`，`BravearkLastLog.c:649` 打印），`refused` 指 `native_gop_gpu_offer` 没被 presenter 接受，与 `vkCreateDevice` / `-3` 无关。

### 40.5 下一步方向（待验证，勿再改常量）

1. **看 SQE 为何停**：`Q6` 已有 SQE_STAT 后续字，下一轮把它与 `Q1` 的 PC（br 0x332 / bv 0x132）连续采样多次，判是否恒定在 idle/WFI 循环；这是当前唯一「读得到包却不取」的直接证据面。
2. **核准 `Q5 r2a/2a` 的采样时机**（重启瞬间 vs hang 时）——它与 `Z1 r36/3d` 不一致，若两者是同一时刻的硬件 RPTR/WPTR，则说明 RPTR shadow 与硬件寄存器分离，`ring->restart(completed)` 的 `completed` 记账要重查。
3. **secure mode 边界**：`gpuRestart()` 只在 `native_gpu_zap[0]==9` 时补发 `CP_SET_SECURE_MODE`；bring-up 有 zap/secure。查本 boot `native_gpu_zap[0]` 实际值，若不是 9，重启后 CP 可能停在 secure 边界不取指。
4. 最省事的判决：在 `gpuRestart()` 末尾直接打印 `ring->fenceValue()` 与硬件 `CP_RB_RPTR`，或重启后立即再提交一次 fence，看是「重启没把 ring 接回来」还是「重启后又立刻卡住」。

### 40.6 操作状态：已刷回原厂，暂停充电

电池一路掉到 **3335 mV**（本轮 status 3417 → stock 时 3335），按用户要求执行
`tools/muyu-flash-mode.sh stock` → `vendor_boot` 写入 `OKAY [0.084s]`，只写 `vendor_boot`，**boot / recovery / userdata 未动**；ledger `04:21:00Z flash vendor_boot fw/stock/vendor_boot.img 69f1e38c…fd rc=0`。刷完 `fastboot devices` 已空（设备离开 fastboot），可自行进 rec 充电（fastboot 本身不充电）。

⚠️ **继续调试前必须重新刷回 UniXNU 镜像**：`MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu <img>`（`restart1` 为 `fw/vendor_boot_gpu_restart1.img`）。若 rec 进不去、需要连 recovery 一起恢复成原厂，再跑 `tools/muyu-flash-mode.sh restore-stock --yes`（会写 boot + vendor_boot + recovery）。

## §41 2026-11-12 12:3x 设备充电期间的**离线 / 虚拟环境**研究：§40.5 三条定案 + secure-mode 新假说

设备已刷回原厂在 rec 充电，本轮**没有碰 fastboot / adb / 电池，也没有构建内核**。全部结论来自源码静态审计 + 本机软件 MMIO 仿真，可一键复核：
`python3 UniXNU-muyu-17/scripts/audit-gpu-restart-offline.py`（输出 `build/gpu-restart-offline/report.json`）。

### 41.1 虚拟环境能做到哪一步（边界要先划清）

| 层 | 现状 | 能做什么 | 不能做什么 |
| --- | --- | --- | --- |
| `UniXNU-qemu` / `UniXNU/package/Scripts/qemu-run.py` | AArch64 `virt` + AAVMF，串口日志 + QMP 截图 | 跑内核 CPU 侧、发布链、用户态 ABI、控制台渲染 | **没有 Adreno 740 设备模型**：CP/SQE/GMU/SMMU/TZ secure 全不可仿真，`Vulkan(-3)`、`68f2`、RPTR 冻结在这里复现不出来 |
| `UniXNU/drivers/Adreno740/tests/` | 真实驱动源码 + 软件 MMIO 寄存器 peer / SMMU peer（含 hangcheck→recover→restart 用例） | 验证 ring/fence/submit/restart 的**软件状态机**与记账 | 验证不了硬件是否真的取指；peer 的寄存器是数组，不会自己推进 RPTR |
| 离线构建 / 静态审计 | `scripts/*`、`fw/*.img`、`logs/*.jpg` 全在本机 | 构建下一版诊断镜像、复核符号与 boot-args、回放屏照时序 | —— |

⇒ **可以研究，而且能推进大半**；但「CP 为什么不取指」这个物理问题只能靠真机屏照判决。虚拟环境的价值是把下一轮刷机要问的问题收敛到最少。

### 41.2 软件 MMIO 实测：套件能跑，但**当前不是绿基线**

用主树 `UniXNU/`（muyu-17 的 `kits/` 是空挂载，`check-native-adreno-memory.py` 直接跑会因 `native_prelink` 缺失而退出）手工编译，ASan/UBSan 全开：

- `test_ring`（`NativeAdrenoRing.cpp` + `NativeAdrenoPreempt.cpp` + `AdrenoPM4.c` + `HostKernelHooks.cpp` shim）：编译通过、可运行，断言停在 `test_ring.cpp:90`「最后一次写应为 `0x0807`、值 46」。
- `test_submit`（含 `hangCheck/recover/restart` 用例，额外链 `NativeAdrenoMemory/SmmuMemory/Vm/Submit.cpp` + 4 个 `ArmSmmu4K*.c`）：编译通过、可运行，断言停在 `test_submit.cpp:127`「`cp.writes.back()==0x0807 && cp.regs[0x0807]==11+4+31`」。

**原因不是回归**：`NativeAdrenoRing.cpp` 里 §30/§31 的实验开关仍是
`kTableNeedsReset=false`、`kResetPrologue=false`、`kPrologue=false` —— framed prologue / reset stretch 整段被关掉，ring 内容比 host 测试期望的短，WPTR 自然不是 46。⇒ **现有 host 套件与当前驱动不同步**，不能拿它的红灯当故障证据；要恢复成基线，得把这三个开关做成编译期注入（host 测试用标准值）或更新期望值。

### 41.3 §40.5 第 2 条定案：`Q5 r2a/2a` 与 `Z1 r36/3d` **不可比**

- `Q5`（`muyu_cp_stall_rows[5]`）在 `heartbeat()` 的 stall 分支里每 2 s 刷新，进入条件是 `gxOn() && CP_RB_RPTR != CP_RB_WPTR`；`r/s/t` 是对 `0x0806/0x0807` 等寄存器的**实时 MMIO 读**。
- `muyu_cp_stall_rows` 是全局数组，**从不清零**（§39 已注意到），屏上的 Q5 可能是很早一次 stall 留下的陈旧采样 —— 这也解释了它 `r2a/2a`（RPTR==WPTR）与 stall 分支条件表面矛盾。
- `Z1` 来自 `BravearkLastLog.c:721` 打印的 `native_adreno_stall[10]/[11]`，是驱动在 **hang 检测瞬间**写的快照。

⇒ 不同时刻、不同来源，`Q5` 与 `Z1` 不能当成同一硬件状态对比；「RPTR shadow 与硬件寄存器分离」的推论不成立。**下一版 Q5 应带采样序号/时间戳**，否则每次判读都有这个坑。

### 41.4 §40.5 第 3 条定案，并给出比 §38 更强的新假说

审计链（全部可离线复核）：

1. `GpuPowerProbe.cpp:145-146` 的 `native_adreno_zap_image` / `_size` 是本地弱默认 `{0}` / `0`；`.cache/firmware/sheng-gpu/.../a740_zap.mbn` 不存在，`build-native-adreno-kits.py`（唯一能嵌入强定义 zap blob 的脚本）未启用 ⇒ **17 线内核里 zap 镜像是 0 字节**。
2. `zapLimit()` 默认 4，本轮 kernel args（`/tmp/build_p38.log`）**没有** `sheng-gpu-zap=` ⇒ 取默认 4。
3. `zapAndSecure()` 第一步 `size < 52` 就记 `z[0] = -1` 并 return，整个 secure 序列跳过 ⇒ **`native_gpu_zap[0] == -1`**。
4. `gpuRestart():1435` 的条件是 `native_gpu_zap[0] == 9` ⇒ **永不满足**，重启后不补发 `CP_SET_SECURE_MODE(0)`。
5. 全文件只有两处发 `0x66` 包：`zapAndSecure():658`（开机时被 3 跳过）与 `gpuRestart():1437`（被 4 门掉）⇒ **17 线从来没有发出过 secure-mode 退出包**。

文档 §11.10 / §11.6 早已记录「`zapAndSecure()` 记 `Z-1` 后跳过，CP 停在 secure mode——与 int-016 状态一致」，并把无 zap 视为这台板上「已知更好的配置」（int-019 zap 关掉才进桌面）。

**新假说（待真机验证，勿当成结论）**：开机时 CP 能推进（`Z1 r36/3d`、fence 到 4），但 `gpuRestart()` 执行了 `RBBM_SW_RESET_CMD` 全复位；复位后 CP 若回到 secure 边界，而这条线**从未学会发退出包**，就会出现 §40.3 的怪象 —— 面前摆着合法 PM4 头 `70460004`、SQE_STAT 也停在这个包上，`RPTR` 却一动不动。这比 §38「缺恢复路径」更贴近现象：恢复路径补上了（`RT n1 s0 e0 b1`），缺的是复位后的**安全态退出**。

⚠️ 反向证据要记住：§11.6 的 int-019 是「zap 关掉才进得了桌面」，说明无 zap 并不必然卡死；差别可能在于本轮多了 `RBBM_SW_RESET_CMD`。所以下一轮要做的是**加判决打印**，别直接改常量。

### 41.5 §40.5 第 4 条的离线部分：`restart(completed)` 记账是「预期行为」

`NativeAdrenoRing::restart()`：清空 `fence_` → `fence_[1]=completed` → 清空整个 `ring_` → `program()`（写 `CP_RB_WPTR=0`、`CP_RB_RPTR_ADDR` 等，末尾 `next_ = 硬件 CP_RB_RPTR`）→ `seqno_/completed_ = completed`。
⇒ 先前担心的「`next_` 没归零导致投递错位」**不成立**（`program()` 里归了；`restartSecondary()` 另写 `next_=0` 只是次要 ring 的路径）。
`fenceValue()` 读的就是 `fence_[1]`。所以 **`Z5 f32/4/0/0` 在重启后仍是 4 是软件记账的必然结果，不能证明 CP 动没动** —— 必须直接读硬件 `CP_RB_RPTR`。这正是 §40.5 第 4 条要求「在 `gpuRestart()` 末尾打印 fence + 硬件 RPTR」的理由。

### 41.6 充电回来后的最小实验（`restart2`，一次刷机拿全数据）

按 §38 以来「一次只改一个变量」的纪律，建议下一版**只加诊断，不改任何常量/寄存器序列**：

1. `gpuRestart()` 末尾打印 `native_gpu_zap[0]`、`zapLimit()`、`ring->fenceValue()`、硬件 `CP_RB_RPTR`/`CP_RB_WPTR`（`gpuRd(0x0806/0x0807)`），分成 `RT2` 行；这直接判决「重启没把 ring 接回来」还是「接回来又立刻卡住」。
2. `Q5` 加采样序号，连续采 3 次 `Q6` SQE_STAT，判是否恒定在 idle/WFI。
3. 若 `RT2` 显示 RPTR 在重启后仍冻结、且 `zap[0]=-1`，再单独做一个**只补发一次 `CP_SET_SECURE_MODE(0)`**（用新 boot-arg 控制，默认关）的对照镜像 —— 这是验证 41.4 假说的最小改动，不动 WPTR/prologue/zap 流程。

⚠️ 刷任何 UniXNU 镜像前，设备必须已离开 rec 并回到 fastboot；命令仍是
`MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu <img>`。本轮**没有**生成 `restart2` 镜像，也没有改动任何驱动源码。

## §42 2026-11-12 12:4x 上游仓库盘点：**有新提交，且共享 GPU 驱动落后 8 天**；sheng 方案复盘 + §41.4 假说降级

设备仍在 rec 充电，本轮全是联网只读盘点 + 源码比对，未碰设备、未改任何驱动源码。最新 sheng 源码快照取到 `/tmp/sheng-latest`（`e893717` 的 codeload tarball），不改本地 `UniXNU-sheng` 工作树。

### 42.1 有没有新提交：有，而且不止一个仓

| 仓 | 本地 | 远端最新 | 差距 |
| --- | --- | --- | --- |
| `UniXNU/UniXNU-sheng` | HEAD `251b291`（10-23），缓存 `origin/main` `2d4b58b`（11-06 02:21） | **`e893717` 2026-11-12 00:53 UTC**（= 北京 08:53，"fix(video): decode 4K 10-bit HEVC"） | 本地工作树 **落后 502**；相对上次 fetch 又新增 **108 个提交**（11-06→11-12） |
| `UniXNU/UniXNU`（17 线共享的 Adreno740/AdrenoMetal 主树） | `0a83918`（11-08），`ahead 60, behind 133` | `0e2fb2bc`（11-12 02:57 UTC） | 缓存 `origin/main` 停在 `e2fdae4`（11-06） |
| `UniXNU/Adreno740` 子模块 | **`22abfe4` 2026-11-03 23:02** | `031f35cc`（11-11 23:41） | **落后 8 天、15+ 个 GPU 修复**（见 42.3） |

新提交里与 GPU 直接相关的（sheng 仓）：`abfc15a6` governor 把「有 busy 痕迹的空闲窗口」误判为工作、`0e6aaded` `[map met]`、`400b9616` GPU 地址空间失败日志、`81f43eba`/`fecc4c26` macOS 27.3 的 GPU 内存池与 `[gpu jobs]/[gpu kick]` 行、`68940017`/`97d958dc` metal2vulkan 合并记录。另外 **`70186fe2 refactor: drop the sheng- prefix from boot arguments`**：新版 sheng 用 `gpu-power=9` / `gpu-metal=1` / `fb-gpu=1` / `gpu-addon=1` / `gpu-dcvs=1`，17 线仍是 `sheng-gpu-*` 旧名，移植时别照抄参数名。

### 42.2 sheng 的 GPU 解决方案（已可用：README 标 GPU ✅，iPadOS 27 与 macOS 27.3 都跑通）

`porting.toml` 的 `[[area]] id="gpu" status="implemented"` 给出完整链路：

- **内核侧**：`Adreno740` + `ArmSmmu4K` + `AdrenoPM4` + `AdrenoFirmware` + `Sm8550Platform`；输入明确包含 **signed ZAP firmware**、GPU/GMU 寄存器、SMMU streams 0/1 + GMU stream 5、board power votes。
- **用户态**：Turnip(Mesa) + metal2vulkan 桥（当前 pin `555b13d5`，AdrenoMetal 25 个补丁）+ `NativeAdrenoMetal.bundle` + GPU addon（Metal plugin/Turnip/Rust bridge）。
- **显示**：`fb-gpu=1` 把合成器帧交给 render server 的 GPU copy，`dpu-scanout=2` 直扫，`fb-bgra=1` 让系统在内存里渲染、transient attachments 放 GMEM。
- **调度**：双 ring 优先级（`gpu-preempt=2`）、preemption 保存 GMEM/SRAM postamble、**GMU keepalive 覆盖 WPTR 恢复写、板级重试 AHB-fence 丢弃的写**、ring switch watchdog 1 s 后进 recovery。
- **验证纪律**（`procedure`）：先把 power/firmware/SMMU/GMU 逐段 qualify，**再**验证 memory mapping / command completion / 一张已知渲染图，**最后**才 stage Metal plugin；并且 `completion` 要求「known image renders + selected Metal client presents it」。

### 42.3 与 17 线断点直接相关的上游修复（本地**全都没有**）

已核对本地 `UniXNU/drivers/Adreno740/` 里 `notOurFault`、`native_adreno_global_fault`、`native_adreno_stall_kick`、`native_adreno_stall_find` **均为 0 文件**：

| 上游提交 | 修了什么 | 对 17 线的意义 |
| --- | --- | --- |
| `031f35cc` / `13d0779e2c` | **SMMU 全局 fault 状态被 GPU 自己那次 fault latch 住时，恰好下一个做 map/unmap/attach 的地址空间会被判「取出使用」**；实测一次跑挂 7 个进程、含 render server。改法：`NativeAdrenoVm::notOurFault()` 把这种 fault 计数后放行，`endFault()` 清全局 GFSR | ⭐ 与 `R PUB no device: Vulkan(-3)`（=`VK_ERROR_INITIALIZATION_FAILED`，`NativeAdrenoMetalPlugin.mm:404` 在 `native_bridge_metal_open` 失败后打印，`0x68f2`）高度吻合。§38 排除的是「持续 SMMU fault」，**没有排除一次性 latch 的全局 GFSR**；`Q0 f400` 只看了本 context 的 FSR |
| `c72a8a5258` | **Turnip 在 kernel 的 ended-submissions 计数变化后把 device 标成 lost**，Metal runtime 随后拒绝所有既有对象，GPU 已重启但 render server 仍坏；新增 `turnip-darwin-keep-device-after-restart.patch`，device 保持可用并计 `tu_darwin_restarts` | ⭐ 与 §40「`RT n1 s0 e0 b1` 重启成功、但 `-3` 原样不动」同名现象。17 线 addon 用的是旧 Turnip，值得核对 |
| `95789237` / `6b24c06ce4` | stall handler 第一阶段在 fence 等 50 ms 后 drop translations + 重发 WPTR，**即使自己的采样显示 CP 正在动**（四次运行全是长 job 误判）；改成「采样发现 CP 在动就重启计时」 | 17 线 `Q4 u0/0/0`（三级 stall 恢复没跑）与 `native_adreno_wptr_fence` 同源；上游的 `[gpu kick] … moved=%x` 正是 §41.6 想加的「CP 到底动没动」判决，已写好可直接移植 |
| `172f53f6` / `62be7bc542` | ring switch 未完成时不再 blame 任何 job，restart 重放全部 job，client 不丢 device | 17 线 `T d0`（mode 0，单 ring）暂不受益，但 `Z1 R2` 值得复核 |
| 新诊断 | `native_adreno_stall_kick[26]`、`[map met]`、`[gpu vm fail]`、`[gpu global fault]`、`[gpu stall] ib2` | 比 17 线现有 Q0–Q6 更直接；下一版诊断可照抄这几个全局数组，不必自己造 |

### 42.4 ⚠️ §41.4 的 secure-mode 假说要**降级**

sheng 最新 `GpuPowerProbe.cpp:1786-1789` 的注释写着：`gpu-zap=0` 跳过 zap/secure-mode 后 **「The CP then stays in secure mode, as in int-016」**，而 `zapLimit()` 的注释明确 **int-019（zap=0）进了桌面、int-017（zap=4）没有**。sheng 的 `build-adreno740-kits.py` 会把真实 `a740_zap.mbn` 用 `.incbin` 嵌进 `firmware.o`（强定义 + pin 校验），所以它能跑完 `zapAndSecure()` 到 `z[0]=9` 并发出 `CP_SET_SECURE_MODE(0)`；17 线没有 zap 镜像（§41.4），走的是 `z[0]=-1`。

⇒ **「CP 停在 secure mode」并不等价于「CP 不取指」**：sheng 实测 zap 关掉时仍能进桌面。17 线不加载 zap 恰恰是 sheng 记录里「已知更好」的配置。§41.4 只在「`RBBM_SW_RESET_CMD` 全复位之后是否需要重新退出 secure」这一点上仍有价值，**优先级应降到 42.3 的 SMMU/device-lost 线索之后**。

### 42.5 建议（勿整体升级驱动）

17 线是 iPadOS 17、boot arg 仍是 `sheng-` 前缀、本地 `NativeAdrenoRing.cpp` 还有 §30/§31 的三个实验开关（§41.2），而 sheng 已是 iPadOS 27 / macOS 27.3。**直接把上游 Adreno740/AdrenoMetal 拉到本地会冲掉 17 线的定制**，且 ABI 不同。

推荐的下一步顺序：
1. 先把 42.3 里几个**只读诊断全局数组**（`native_adreno_global_fault`、`native_adreno_stall_kick`）与打印行按 17 线风格移植进 `restart2`，与 §41.6 的 `RT2` 合并成一次刷机；这样一次屏照同时判「CP 有没有动」和「有没有 latch 的全局 fault / 地址空间被误杀」。
2. 若屏照显示全局 fault 或地址空间被取出使用，再单独移植 `notOurFault()` / `endFault()` 这个**单点修复**（不碰 ring/submit 主逻辑）。
3. 核对 17 线 addon 里的 Turnip 是否缺 `turnip-darwin-keep-device-after-restart.patch`；缺则补这一个补丁。
4. 之后才考虑 secure mode 的对照实验（§41.6 第 3 步）。

本轮**未 fetch、未 checkout、未改任何仓库文件**；本地 `UniXNU-sheng` 仍是 10-23 的旧镜像，仅 `/tmp/sheng-latest` 有最新快照。若要长期对照，可自行 `git -C UniXNU-sheng fetch origin main`（只更新 remote ref，不动工作树）。
（§43 已执行这条 `fetch`：只更新了 `origin/main` 到 `e893717`，工作树 HEAD 仍是 `251b291`，`behind 610`。）

## §43 2026-11-12 12:5x 充电期间：`restart2` 诊断镜像**已构建、未刷**；§42.5 第 1/3 步离线定案

设备仍在 rec 充电（`fastboot devices` 为空），本轮全程离线：**没有碰 fastboot/adb、没有刷机、没有改任何常量或寄存器序列**。ledger 最后一条仍是 `04:21:00Z` 的原厂 `vendor_boot` 刷入。

### 43.1 改动清单（3 个文件，全部是只读诊断）

按 §41.6「一次只改一个变量」和 §42.5 第 1 步，把 RT2、CP 移动判据、全局 fault 判据合并进一次刷机：

| 文件 | 改动 | 行为影响 |
| --- | --- | --- |
| `UniXNU-muyu-17/package/GpuPowerProbe.cpp` | `muyu_cp_stall_rows[7][64]`→`[9][64]`；新增 `native_gpu_restart2[10]`、`native_gpu_stall_seq`；`gpuRestart()` 在入口 / `ring->restart()` 后 / `CP_ME_INIT` 后 / 末尾各采一次硬件 `CP_RB_RPTR`（另有 WPTR、`fenceValue()`、`zap[0]`、`zapLimit()`、全局 GFSR）；`heartbeat()` 每次（不再只在停滞时）刷新 `Q7`（3 次间隔 1 ms 的 RPTR + BR SQE PC）和 `Q8`（GFSR / FSR / VM fault 计数 / Q5 的刷新时刻） | 只读 MMIO；`gpuWr(0x0908,0)` 的 SQE_STAT index select **仅在 ring 真的停滞时**才写，健康 GPU 只见寄存器读 |
| `UniXNU/drivers/Adreno740/NativeAdrenoVm.cpp` | 新增 `native_adreno_global_fault[4]`，在 `fail()` 里只记 `[0]` 次数、`[1]` 最后 `ArmSmmu4KStatus` | **零行为改动**：不仿 upstream `031f35cc` 的 `notOurFault()` 放行，也不碰 poisoning / quarantine |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | Q 行循环 7→9；新增 `MUYU RT2` 打印 | 只多 3 行屏显 |

没有移植 `native_adreno_stall_kick[26]`，也没有改 `NativeAdrenoSubmit.cpp`：上游 `[gpu kick] moved=%x` 的判决由 `Q7` 的三次硬件连采直接承担，避免动 17 线 §30/§31 的 stall 实验开关。三个源文件改前已备份到 `/tmp/restart2-backup/`。

### 43.2 构建记录（§24.9 三步链，58 s）

```text
/tmp/build_k43.log   kernel        OK
/tmp/build_p43.log   payload       OK
/tmp/build_v43.log   vendor_boot   OK
```

`error:|warning:|FAILED|undefined|Traceback` 在三份日志里**全空**。`fw/vendor_boot_gpu_restart2.img`：
**77,393,920 B，sha256 `bf0a1c33c5306dff516c15e8c96e8e4fc10ea4daf063767b7c684e2f5e23358f`**；
ramdisk 段 75,497,472 B @ `0x1000`、dtb 1,885,961 @ `0x4801000`（与 §38 布局一致）。

符号校验（`strings -a`）：`MUYU RT2`、`native_gpu_restart2`、`native_adreno_global_fault`、`K#%u`、`M#%u` 在 `restart2` 各命中 1 次，在 `restart1` **全部为 0** ⇒ 新代码确实进了 payload，不是旧镜像复用。
`NativeAdrenoVm.cpp` 另用 host clang++ `-fsyntax-only` 通过（`rc=0`）。

### 43.3 屏照判读格（一次刷机同时判三件事）

**新行格式**

```text
MUYU RT2 z<zap[0]> l<zapLimit> f<fence> r<entry>/<post-restart>/<post-CP_ME_INIT>/<exit RPTR> w<entry>/<exit WPTR> g<global GFSR>
MUYU Q7  K#<tick> r<r1>/<r2>/<r3> p<pc1>/<pc2>/<pc3>        （p 仅在停滞时非 0）
MUYU Q8  M#<tick> g<global GFSR> f<context FSR> n<VM fail 次数> s<最后 fault status> q<Q5 最后刷新 tick>
```

`Q7/Q8` 每个 heartbeat（2 s）都刷新，**解决 §41.1「Q0–Q6 从不清零、屏上可能是很旧的采样」**；`Q8 q` 直接给出 Q5/Q6 的年龄。

**判决树**

1. **基线对照**：`restart2` 行为与 `restart1` 完全一致，预期逐字复现 `RT n1 s0 e0 b1`、`H h.. r..`、`Z5 f32/4/0/0`、`R PUB no device: Vulkan(-3)`、`X7 …68f2`。若这些变了，先怀疑新增采样扰动时序（可能性极低）。
2. `RT2 r` **四个值全等** ⇒ 重启从头到尾 CP 都没动过 —— §41.5 的「`fence=4` 只是软件记账」被直接证实，且断点在「重启没把 ring 接回来」。
   - 同时 `g≠0` 或 `Q8 n>0` ⇒ **§42.3 第一条（一次性 latch 的全局 GFSR / 地址空间被 poison）命中**，下一步单点移植 `notOurFault()` + `endFault()`（不改 ring/submit 主逻辑）。
   - 同时 `g=0`、`n=0` 且 `z=-1` ⇒ 回到 §41.4 的降级假说：单独做「只补发一次 `CP_SET_SECURE_MODE(0)`」的对照镜像（用新 boot-arg 控制，默认关）。
   - `RT2 f` 仍等于 4 是预期（`restart(completed)` 把 `fence_[1]` 设为 `completed`），**不要**当作「CP 没动」的证据，看 `r`。
3. `RT2 r` **递增** ⇒ CP 动过、之后又停：看 `Q7 r/p` 是否恒定在 idle/WFI、`Q8 g/n`；这是「长 job 被误判 hang」或 Turnip device-lost 的方向。
4. `Q7 r` 三次递增但 `R PUB no device: Vulkan(-3)` 依旧 ⇒ **CP 在动，断点在用户态/ Metal**。此时优先执行 43.4 的 Turnip 补丁，而不是继续改内核。

### 43.4 §42.5 第 3 步离线定案：17 线**缺** Turnip 重启保活补丁

`c72a8a5258`（`UniXNU/UniXNU` 主仓，不是 sheng 仓）新增 `patches/mesa/turnip-darwin-keep-device-after-restart.patch`，修的正是「kernel 的 ended-submissions 计数变化 → Turnip 把 device 标 lost → render server 在 GPU 已重启后仍坏」。

本机核对：
- `UniXNU/patches/mesa/` 有 17 个补丁，`scripts/data/adreno-compiler-inputs.json` 的 `patches` 数组正好列这 17 个，**没有** `turnip-darwin-keep-device-after-restart.patch`；全仓 `find` 也无此文件。
- `/tmp/sheng-latest`（sheng `e893717` 快照）同样没有该补丁文件 —— 它在主仓，sheng 走 `feeds` 引用。
- `fw/gpuaddon-17-full.bin` 是预构建 blob（`file` = data），`strings` 里查不到 `tu_darwin_restarts`。

⇒ 17 线 addon 用的是**未打该补丁的 Turnip**。这与 §40「`RT n1 s0 e0 b1` 重启成功、`-3` 原样」同名。补补丁需要重建 addon（会改变镜像内容），**本轮按「只加诊断」纪律未做**，列为 43.3 第 4 条命中后的下一步。

### 43.5 仓库对照状态

- `git -C UniXNU-sheng fetch origin main` 已执行（只更新 remote ref）：`origin/main` `2d4b58b` → **`e893717`**（11-12 08:53 "fix(video): decode 4K 10-bit HEVC"）；**工作树 HEAD 仍是 `251b291`（10-23），`behind 610`**，未 checkout、未 merge、未改任何文件。
- 本地 `UniXNU` 主树与 `Adreno740` 子模块**未** fetch / 升级（§42.5 明确不要整体升级驱动）。
- 本轮新增/修改文件：`fw/vendor_boot_gpu_restart2.img`（新）、`UniXNU-muyu-17/package/GpuPowerProbe.cpp`、`UniXNU-muyu-17/package/BravearkLastLog.c`、`UniXNU/drivers/Adreno740/NativeAdrenoVm.cpp`、`UniXNU-muyu-17/docs/GPU-HANDOFF.md`、`/tmp/restart2-backup/*`。

### 43.6 充电回来后的动作（唯一许可命令）

设备离开 rec 并回到 fastboot 后，**单次**执行：

```bash
cd /Users/diannao/pad7pro-xnu
MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_restart2.img
```

只写 `vendor_boot`，UEFI 仅从 RAM boot；写前脚本会校验 serial 与 sha。刷完拍一张屏照放进 `logs/`，按 43.3 判读。⚠️ 上一轮屏照 `logs/2026-11-12-12-14-41-437.jpg` 是**正立**的（§40），别按老习惯翻转。电池上次读到 3335 mV，长跑前先确认已充好。

## §44 2026-11-12 13:13 设备「在线」：restart2 已刷入（待本 boot 屏照）

用户报「在线」后先做只读预检，确认条件齐备才刷：

| 项 | 读数 |
| --- | --- |
| `fastboot devices` | `a20248f7  fastboot`（`adb devices` 空） |
| `status` | `current-slot=a slot-unbootable=no battery=3861 mV` |
| 电池变化 | 3335 mV（§40 刷回原厂时）→ **3861 mV**，+526 mV，已过脚本 3700 mV 告警线，充电有效 |
| 镜像 | `fw/vendor_boot_gpu_restart2.img`，`vendor_boot v4 73.81 MiB / vendor_ramdisk 72.00 MiB / aligned=True`，sha 与 §43.2 一致 |

单次执行 §43.6 的命令：
`Writing 'vendor_boot' OKAY [0.055s]` → `booting UEFI from RAM: fw/boot-17line-043f7120.img` → `Booting OKAY [0.192s]`。
**只写 `vendor_boot`**，`boot` / `recovery` / `userdata` 未动，UEFI 仍从 RAM 启动。ledger：
`05:13:27Z preflight rc=0` → `05:13:29Z flash vendor_boot …bf0a1c33… rc=0` → `05:13:30Z boot …043f7120… rc=0`。

刷后 100 s `fastboot devices` 为空、`adb devices` 为空 ⇒ **没有回落 fastboot**，设备仍在启动/用户态（与 §39 一致，不是启动失败）。`logs/` 最新照片仍是 12:14 的 `restart1`，本机没有 USB 调试串口（boot-args 无 `muyu_usb=1`），本 boot 的读数只能靠屏幕照片。

**待办**：拍一张屏照放进 `logs/`，按 §43.3 判读，重点三行：

```text
MUYU RT2 z… l… f… r<a>/<b>/<c>/<d> w…/… g…
MUYU Q7  K#<tick> r<r1>/<r2>/<r3> p<pc1>/<pc2>/<pc3>
MUYU Q8  M#<tick> g<GFSR> f<FSR> n<VM fail> s<status> q<Q5 最后刷新 tick>
```

- `RT2` 四个 `r` 全等 ⇒ 重启全程 CP 没动；`r` 递增 ⇒ 动过又停。
- `Q8 g≠0` 或 `n>0` ⇒ §42.3 全局 SMMU fault 命中，下一步单点移植 `notOurFault()`/`endFault()`。
- `Q7 r` 三次递增但 `R PUB no device: Vulkan(-3)` 依旧 ⇒ 断点在用户态，转 §43.4 的 Turnip 补丁。
- 基线对照：`RT n1 s0 e0 b1`、`Z5 f32/4/0/0`、`R PUB no device: -3`、`X7 …68f2` 应原样复现；变了要先怀疑新增采样扰动。

⚠️ 判图前先确认方向：上一轮（12:14）是**正立**（EXIF orientation=1），更早几轮是倒置；裁切一律带 `ffmpeg -noautorotate`。

## §45 2026-11-12 13:18 本 boot 屏照判读：**基线未复现**，`-3` 与 GPU hang 解耦

屏照 `logs/2026-11-12-13-18-48-138.jpg`（3840×2160）已收到并判读完毕。本轮**没有再刷机、没有改任何源码或镜像**；设备仍在启动/用户态（`fastboot devices` 空），ledger 最后一条仍是 `05:13:30Z boot rc=0`。

### 45.1 判图方法

照片是**正立**的（原图 Vision OCR 直接可读，`sips` 3840×2160、无旋转元数据），与 12:14 那张一致；更早几轮是倒置。用 `/tmp/gpu_ocr2` 带行序定位，再用 `ffmpeg -noautorotate -vf "crop=2600:<h>:0:<y>,scale=7800:<h*3>"` 生成窄条 3× 放大，逐行视觉转录。所有下表中的关键行都经过放大复核，**不是 OCR 猜测值**。

工具坑（留给下一轮）：zsh 不做变量分词，`crop` 参数必须写死；`ffmpeg` 不带 `-noautorotate` 会按 EXIF 自动转成竖版导致裁切越界；该 python 环境无 PIL，EXIF/尺寸用 `sips` / `exiftool`。

### 45.2 逐行读数（`restart1` 12:14 §40 → `restart2` 13:18 本 boot）

| 行 | `restart1` | **`restart2` 本 boot** | 变化 |
| --- | --- | --- | --- |
| `RT` | `n1 s0 e0 b1` | **`n0 s0 e0 b0`** | ⚠️ restart hook **从未调用** |
| `RT2` | — | `z0 l0 f0 r0/0/0/0 w0/0 g0` | 全零（见 45.3） |
| `Q7` | — | `K#114 r32/32/32 p0/0/0` | 新增 |
| `Q8` | — | `M#114 g0 f400 n0 s0 q0` | 新增 |
| `H` | `h1 r1 F0 F5 s0 p0 g1 k0` | **`h0 r0 F0 F0 s0 p0 g0 k0`** | ⚠️ 零 hangcheck |
| `Z5` | `f32/4/0/0` | **`f0/0/0/0`** | ⚠️ fence 从未推进 |
| `Z1` | `r36/3d …` | **`r0/0 b0/0 w0 h0/0/0 R0`** | ⚠️ |
| `Z0` / `F` | `t2 f5 s80000402…` / `n1 s80000002` | **`t0 f0 s0 a0/0 y0 u0/0`** / **`n0 s0 a0/0 y0`** | ⚠️ |
| `Z2` 四组 | 含 `70108000` / `48088501` | **全 `00000000`** | ⚠️ |
| `W` | — | `kffffff` + 6 组全 `fffffff` | retry 历史槽从未写过 |
| `Q4` | `W3/0/0/0 u0/0/0 a0/0` | **逐字相同** | 3 次 WPTR 写 |
| `Q0` / `Q1` / `Q3` / `Q5` / `Q6` | `f400 a3/301 c1c` / `br 332 70460004` / `b200000/0 c20c` / `i0 a0 q100000/0 r2a/2a` / `br 70460004/0/31` | **全部逐字相同** | 见 45.3 第 3 条 |
| `X4` | `H118 R1 I0 F0 ra/a f5 …` | **`H114 R1 I0 F0 r32/32 f4 g1 s400 G0 q2 u0`** | `RPTR==WPTR` |
| `X5` | `SpringP0 x95 c93 f0 l6/0` | **`backboF1 x0 c0 f0 l0/0 g14 u0`** | 无 WaitFence 累计 |
| `X7` | `K4 6800 6801 6a3c 68f2` | **逐字相同** | `68f2` 仍在 |
| `R` | `PUB no device: device: Vulkan(VkResultCode(-3))` | **逐字相同** | `-3` 仍在 |
| `D0`–`D5` / `R0` / `A1` | `P ok ok ok skip io0 pw11`、`chip 43030b00 gmxc0 32/32/32/0`、`cx0022F801>f822f800`、`14c017e18 j40000016 s00220406`、`f00000000 cb00000167 a00000003` | **基本一致** | power/GMU/AHB 初始化正常 |

### 45.3 三条定案

1. **`RT2` 本 boot 是无效样本**。`RT n0` ⇒ `gpuRestart()` 根本没被调用，`native_gpu_restart2[10]` 是 BSS 清零后的初值，`z0 l0 f0 r0/0/0/0 w0/0 g0` **不是采样结果**。⇒ §43.3 判决树第 2/3 条（四个 RPTR 全等 / 递增）在这一 boot **两条都不能用**。这是 `restart2` 的设计缺口：hook 未触发时应打印哨兵（如 `RT2 --`），而不是全零。
2. **`Q7 r32/32/32` 不代表 CP 冻结**。`p0/0/0` 是 `stalled=false` 的标记（`pc[k]` 只在 `RPTR != WPTR` 时采样，见 `GpuPowerProbe.cpp:1704-1713`），配合 `X4 r32/32` ⇒ `CP_RB_RPTR == CP_RB_WPTR == 0x32`，**ring 已 drain、CP 空闲**；三次连采相同是 idle 的正常表现。§43.3 把「三个 `r` 全等」直接等价于「CP 没动」，隐含前提是停滞，本 boot 不满足。
3. **§42.3 全局 fault 假说本 boot 未命中**。`Q8 g0`（全局 GFSR=0）、`n0`（`NativeAdrenoVm::fail()` 从未调用 ⇒ 没有地址空间被 poison）、`s0`（无 fault status）；`f400` 是两轮一致的 context FSR 静态值（§38 已排除「持续 SMMU fault」）。`q0` ⇒ Q0–Q6 最后一次刷新在 **tick 0**，其后 114 个 tick（≈228 s）再没进过 stall 分支 ⇒ 屏上 Q0–Q6 是**启动瞬间的旧快照**，实证 §41.1；它们与 `restart1` 逐字相同**不能**当作「本 boot 实时状态一致」的证据。

### 45.4 最重要：基线未复现 ⇒ §43.3 第 1 条命中

`H h0 r0`、`RT n0`、`Z5 f0/0/0/0`、`Z1 r0/0`、`F n0`、`Z2` 全零、`W` 全 f ⇒ 这一 boot GPU **从未提交工作、从未 hangcheck、从未 restart、fence 从未推进**；而 `R PUB no device: Vulkan(-3)` 与 `X7 …68f2` **原样仍在**。

- **强读法（若可复现）**：`-3` 在「GPU 完全空闲、无 hang、无全局 SMMU fault、无 VM poison」时照样出现 ⇒ 它不是 CP stall / WaitFence 超时 / 全局 fault 的下游结果，断点在**更上游的设备创建 / 用户态**。§38「缺 restart hook」、§41.4「secure mode」、§42.3「全局 GFSR latch」三条内核侧假说全部降级。

**`-3` 的精确产生链（本轮离线追到，纯只读）**：

```text
NativeAdrenoMetalPlugin.mm:400  device = native_bridge_metal_open(turnip, cache, registryID, …)
  → NativeBridgeMetalFrontend.mm:4574  gDevice = nmb_device_open(loader, cache)
    → NativeMetalObjects.rs:171  Device::system_default_with(&options).map_err(e("device"))?
      ← 失败 → set_error() → nmb_last_error() = "Vulkan(VkResultCode(-3))"
  ← !gDevice → :4581  NBLOG("native-metal: device refused: %s") → return nil
→ :404  gpu->mark(0x68f2) + "PUB no device: %.40s"
```

⇒ `VK_ERROR_INITIALIZATION_FAILED` 是 **Turnip / metal2vulkan 创建 Vulkan device 阶段**的返回码，发生在**任何 GPU 提交之前**；这正好解释本 boot「GPU 从没工作过，但 `-3` 照样在」。

⚠️ **§46 更正**：`PUB no device: %.40s` **没有截断** —— `e("device")`（`NativeMetalObjects.rs:102`，`format!("{what}: {error:?}")`）产出的全文就是 `device: Vulkan(VkResultCode(-3))`（32 字符 < 40），屏显已原样输出。信息不足的根源是 metal2vulkan runtime 的错误类型只有 `VkResultCode(-3)`，没带失败的 Vulkan 调用 / 阶段；要更细原因必须改 runtime 或 `nmb_device_open()` 的上下文并**重建 owner-signed Metal plugin bundle + trust cache + addon**（§46），不是放宽格式串。
⚠️ 排除一个误导：`bridge/priming_cache_loader.cpp` / `graphics_cache_loader.cpp` 里那几处 `VK_ERROR_INITIALIZATION_FAILED` 是 **"Offline only" 的 host 测试 shim**（文件头明示），不是运行时路径，不要据此推断是 shader cache 校验失败。
- **保守读法（按纪律优先）**：基线未复现时不能与 §40 直接对比，需先排除扰动。可能原因：
  a. `restart2` 并非纯只读 —— Q7 在 `stalled` 时会 `gpuWr(0x0908, 0)` 选 SQE_STAT index 并读 `0x0909`；`q0` 说明 t=0 那次 stall 分支确实执行过这次写，可能扰动 CP 初始化时序。⚠️ **§46 下调此假说**：`0x0908` 写**不是 restart2 首创** —— `git HEAD:package/GpuPowerProbe.cpp:1417` 的 Q0–Q6 stall 分支早就在写（restart3 源码仍有 `:1626` / `:1669` 两处）。restart2 相对 restart1 只多了 Q7 在每个 stalled tick 的额外 3 次写，且 `q0` 表明 stall 只在 t=0 发生过一次 ⇒ 扰动可能性**下调但未排除**，环境差异（b）相对上升；
  b. 运行条件差异：电池 **3861 mV**（§40 时为 3420 mV）、冷/温启动、Metal 重试路径不同；
  c. 屏照时刻虽同为 ≈114–118 tick，但启动路径本身已分叉。
  ⇒ **先排除 a，再谈强结论**。

### 45.5 下一步（一次只改一个变量）

**A（优先，零构建）**：设备回 fastboot 后刷回 `restart1` 做对照：

```bash
cd /Users/diannao/pad7pro-xnu
MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_restart1.img
```

- 若 `RT n1 s0 e0 b1` / `H h1 r1` / `Z5 f32/4/0/0` **复现** ⇒ `restart2` 的诊断扰动成立（重点 `0x0908` 写）⇒ 做 `restart3`：Q7 去掉一切寄存器写（只留 3× RPTR 只读）、`RT2` 加「hook 未触发」哨兵、Q8 增加 `stalled` 标志位。
- 若 `restart1` **也全零** ⇒ 与诊断无关，是环境/启动路径差异，45.4 强结论成立 ⇒ 直接转用户态：补 `turnip-darwin-keep-device-after-restart.patch`（§43.4，需重建 addon）并取 `nmb_last_error()` 全文。

**B（不想回退对照时）**：直接构建 `restart3`（纯只读 + 哨兵），一次刷机同时验证「诊断是否扰动」与「RT2 是否有效」。

**C（§46 更正：原建议不成立）**：放宽 `%.40s` **无效** —— `nmb_last_error()` 全文 `device: Vulkan(VkResultCode(-3))` 只有 32 字符，`%.40s` 从未截断。真正缺的是 metal2vulkan runtime 在 device 创建失败时的上下文（哪个 `vk*` 调用、loader / cache 路径、失败阶段），需要改 `NativeMetalObjects.rs` 的 `e("device")` / `nmb_device_open()` 或 runtime 错误类型，并重建 owner-signed `NativeAdrenoMetal.bundle`（`build-adreno-metal-plugin.py` → `make-owner-trust-bundle.py` → `make-gpu-addon.py`）与 `fw/gpuaddon-17-full.bin`。本机 iOS SDK 可用（`iPhoneOS27.2.sdk`），但这会同时改变 **bundle 签名 / CDHash / trust cache / addon** 四个输入，违反「一次只改一个变量」，且与 `restart3` 的板级诊断混在一起无法归因 ⇒ **本轮不做**，列为充电回来后的独立一轮。

### 45.6 本轮状态

- 设备：本 boot 仍在运行，未回落 fastboot；**未再刷机、未改任何源码 / 镜像 / 驱动**。
- 新增：`GPU-HANDOFF.md §45`；判读用裁切 `/tmp/r2_*.png`（临时文件）。
- 判读格修正：**§43.3 的第 2/3/4 条判决依赖 `RT n>=1` 且 `Q7 p!=0`**，下一版必须先打印这两个前提，否则 `RT2` 全零会被误读成「CP 从头到尾没动」。（§46 把 `p` 换成显式的 `s<stalled>`，把 `RT n` 同时写进 `Q8 t`。）

## §46 2026-11-12 13:27–13:32 回 rec 充电期间：原厂已刷回；`restart3` 纯只读诊断镜像**已构建、未刷**

用户「继续，但我也要回 rec 充电」⇒ 设备让给充电，本轮**不刷任何 UniXNU 镜像、不做对照上机**，只做离线准备。按 §40 建立的惯例，先恢复原厂 `vendor_boot` 使设备能进 rec 充电。

### 46.1 原厂刷回（本轮唯一的设备操作）

只读预检：`a20248f7 fastboot`、`adb devices` 空、`slot=a unbootable=no`、**电池 3749 mV**（比 §44 的 3861 略降）。

```text
bash tools/muyu-flash-mode.sh stock
  → Writing 'vendor_boot'  OKAY [0.060s]   (fw/stock/vendor_boot.img, sha 69f1e38c…fd)
```

只写 `vendor_boot`，**boot / recovery / userdata 未动**；ledger `05:28:20Z preflight rc=0` → `05:28:23Z flash vendor_boot …69f1e38c… rc=0`。刷完 `fastboot devices` / `adb devices` 均空 ⇒ 设备已离开 fastboot。脚本提示：**长按电源 10–15 s 关机，再插墙充 1–2 h**（fastboot 本身不充电）。

### 46.2 两处判断修正（推翻 §45 的两个推断）

1. **`PUB no device: %.40s` 没有截断**。`e("device")`（`NativeMetalObjects.rs:102` = `format!("{what}: {error:?}")`）产出全文 `device: Vulkan(VkResultCode(-3))`，**32 字符 < 40**，屏显已原样输出。⇒ §45.5 的「放宽格式串就能拿到真实原因」**不成立**；缺的是 metal2vulkan runtime 在 device 创建失败时的上下文（哪个 `vk*` 调用 / loader / cache 路径 / 失败阶段），要改 runtime 或 `nmb_device_open()` 的 `e(...)` 包装。
2. **`0x0908` 写不是 restart2 首创**。`git HEAD:package/GpuPowerProbe.cpp:1417` 的 Q0–Q6 stall 分支早就在 `gpuWr(0x0908, 0)`，restart3 源码仍保留 `:1626` / `:1669` 两处。restart2 相对 restart1 只多了 Q7 在每个 stalled tick 的额外 3 次写，且 §45 的 `q0` 表明 stall 只在 t=0 发生过一次 ⇒ **§45.4 (a)「诊断扰动」假说力度下调但未排除**，环境差异（电池 3861 mV vs §40 的 3420 mV、冷/温启动、Metal 重试路径）相对上升。

### 46.3 `restart3` 改动清单（2 个板级文件；改前备份 `/tmp/restart3-backup/`）

`restart3` = `restart2` + 三处调整，**全部在 kernel payload 侧，不动 Metal plugin bundle / trust cache / addon / Turnip**：

| 文件 | 改动 | 目的 |
| --- | --- | --- |
| `UniXNU-muyu-17/package/GpuPowerProbe.cpp` | Q7 删掉 `gpuWr(0x0908,0)`、`gpuRd(0x0909)` 与 `pc[3]`，**只留 3 次间隔 1 ms 的 `CP_RB_RPTR` 读**；格式 `K#%u r…/…/… p…/…/…` → **`K#%u s%u w%x r%x/%x/%x`**（`s`=stalled、`w`=WPTR）；Q8 末尾加 **`t<restart 次数>`**（=`native_gpu_restart[0]`） | 让「三个 r 全等」自带前提：`s=0` 是 ring drained 的健康 idle，不是冻结；`t` 让 RT2 全零无法被误读成样本 |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | RT2 在 `native_gpu_restart[0]==0` 时打印 **`MUYU RT2 -- gpuRestart not called (n0)`**，否则才打印采样行；注释补上有效性前提 | §45.3 第 1 条的设计缺口 |
| `UniXNU/drivers/Adreno740/NativeAdrenoVm.cpp` | **未再改动**，沿用 restart2 的 `native_adreno_global_fault[4]` 只读计数 | 保持「一次只改一个变量」 |

仍然**没有**改任何常量、寄存器序列、ring/submit 主逻辑，也没有整体升级驱动。

### 46.4 构建记录（§24.9 三步链，55 s）

```text
/tmp/build_k46.log   kernel        OK
/tmp/build_p46.log   payload       OK
/tmp/build_v46.log   vendor_boot   OK
```

`error:|warning:|FAILED|undefined|Traceback` 三份日志**全空**。`fw/vendor_boot_gpu_restart3.img`：
**77,393,920 B，sha256 `ca332a0bcbe58984fa9242dd074d7c8e4224e4981c1fbc0bd3da5dbba5312201`**；
ramdisk 75,497,472 B @ `0x1000`、dtb 1,885,961 @ `0x4801000`（与 §38/§43 布局一致）。
boot-arg 与 §43 完全相同：`sheng-fb-gpu=1 sheng-gpu-addon=1 sheng-gpu-metal=1`，addon / data seed / owner-trust / UEFI 全部沿用原文件。

**符号校验**（`strings -a`，三版对照）：

| 格式串 | restart1 | restart2 | **restart3** |
| --- | --- | --- | --- |
| `K#%u r%x/%x/%x p`（restart2 旧 Q7，含寄存器写） | 0 | **1** | **0** |
| `K#%u s%u w%x`（纯只读新 Q7） | 0 | 0 | **1** |
| `M#%u … q%x t%u`（Q8 加 restart 计数） | 0 | 0 | **1** |
| `MUYU RT2 --`（hook 未触发哨兵） | 0 | 0 | **1** |

⇒ 新代码确实进了 payload，不是旧镜像复用；三版 sha 互不相同、大小一致。

### 46.5 充电回来后的判读格

```text
MUYU Q7  K#<tick> s<0|1> w<WPTR> r<r1>/<r2>/<r3>
MUYU Q8  M#<tick> g<GFSR> f<FSR> n<VM fail> s<status> q<Q5 刷新 tick> t<restart 次数>
MUYU RT  n<次数> s<失败步> e<status> b<RBBM>
MUYU RT2 -- gpuRestart not called (n0)          ← t=0 时必定是这一行
或
MUYU RT2 z… l… f… r<a>/<b>/<c>/<d> w…/… g…    ← t>0 时才有效
```

1. `Q8 t` 与 `RT n` 应相等；两者为 0 ⇒ RT2 必须是哨兵行，交叉验证通过。
2. **只有 `Q7 s=1`（ring 停滞）时**，`r` 三次全等才能读作「CP 没动」；`s=0` 时全等是正常 idle。
3. `Q8 g≠0` 或 `n>0` ⇒ §42.3 全局 fault；`g=0 n=0` 且基线 `R PUB no device -3` 仍在 ⇒ 支持 §45.4 强结论（断点在 Vulkan device 创建阶段，与 GPU 执行无关）。

### 46.6 建议顺序（⚠️ 充电回来后**必须先重新刷 UniXNU 镜像**，当前 `vendor_boot` 是原厂）

**A（零构建，优先）**：刷回 `fw/vendor_boot_gpu_restart1.img` 做对照，回答 §45 留下的唯一问题 —— 「restart2 全零是常态还是偶发」。

```bash
cd /Users/diannao/pad7pro-xnu
MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_restart1.img
```

- 复现 `RT n1 s0 e0 b1` / `H h1 r1` / `Z5 f32/4/0/0` ⇒ `restart2` 的额外诊断写（或环境）确实改变了启动路径 ⇒ 再刷 `restart3`（纯只读）取稳态读数。
- 仍全零（`RT n0` / `H h0 r0`）⇒ §45.4 强结论成立，转用户态：Turnip 重启保活补丁（§43.4）与 device 创建失败上下文（46.2 第 1 条）。

**B**：若不想回退对照，可直接刷 `restart3`（自带 `s`/`t`/哨兵，一张屏照判更多事），但**无法**单独归因 restart2 的全零。

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_restart3.img
```

**C（独立一轮，本轮未做）**：重建 owner-signed Metal plugin bundle 以拿到 device 创建失败的细因。链路 `build-adreno-metal-plugin.py` → `add-gpu-plugin-data.py` → `make-owner-trust-bundle.py` → `make-gpu-addon.py`，本机 iOS SDK 可用（`iPhoneOS27.2.sdk`），但会同时改 bundle 签名 / CDHash / trust cache / addon 四个输入，**必须与板级诊断分开刷**才能归因。

⚠️ 屏照方向：最近两张（12:14、13:18）都是**正立**；裁切一律带 `ffmpeg -noautorotate`，zsh 不分词、crop 参数写死。

### 46.7 本轮状态

- 设备：已刷回原厂 `vendor_boot` 并离开 fastboot，**可进 rec 充电**；本轮**未刷任何 UniXNU 镜像**。
- ledger 最后一条 `05:28:23Z flash vendor_boot fw/stock/vendor_boot.img … rc=0`。
- 新增 `fw/vendor_boot_gpu_restart3.img`；修改 `GpuPowerProbe.cpp` / `BravearkLastLog.c`（restart2 基础上的增量）、`GPU-HANDOFF.md §45 更正 + §46`；`/tmp/restart3-backup/` 存改前源码。
- `UniXNU-sheng` 工作树仍 `251b291`、主树与 `Adreno740` 子模块未升级；`fw/gpuaddon-17-full.bin` 未改。

## §47 2026-11-12 13:43–13:46 设备「在线」：按 46.6 **A** 刷回 `restart1` 做对照（待本 boot 屏照）

### 47.1 目的

§45 的 `restart2` 本 boot 出现 `RT n0 s0 e0 b0` / `H h0 r0` / `Z5 f0/0/0/0`（GPU 从未提交、从未 hangcheck、从未 restart），但 `R PUB no device: Vulkan(-3)` 与 `X7 …68f2` 原样。§43.3 预设的基线是 §40 的 `restart1`（`RT n1 s0 e0 b1` / `H h1 r1` / `Z5 f32/4/0/0`）。本节用**同一份 `restart1` 镜像**再跑一次，判断「全零是常态还是偶发」，并顺带把电池这个混淆变量固定住。

| boot | 镜像 | 电池 | `RT` | `H` | `Z5` | `R` |
| --- | --- | --- | --- | --- | --- | --- |
| §40（12:14） | `restart1` | 3420 mV | `n1 s0 e0 b1` | `h1 r1` | `f32/4/0/0` | `Vulkan(-3)` / `68f2` |
| §45（13:18） | `restart2` | 3861 mV | **`n0 s0 e0 b0`** | **`h0 r0`** | **`f0/0/0/0`** | `Vulkan(-3)` / `68f2` |
| 本 boot（13:44） | `restart1` | **3883 mV** | 待判读 | 待判读 | 待判读 | 待判读 |

⚠️ 本 boot 电池与 §45 只差 22 mV，与 §40 差 463 mV ⇒ **这次对照几乎把电池变量固定住了**：若复现 `n1`，差异只能归因于 `restart2` 的镜像内容（额外诊断写 / 采样时序）或启动随机性；若仍全零，则 §40 那次 `n1` 更可能与低电量/当时环境有关，而不是镜像差异。

### 47.2 预检与刷机（只写 `vendor_boot`）

- 只读预检：`a20248f7 fastboot`、`adb` 空、`slot=a / unbootable=no`、电池 **3883 mV**（充电前 3749 mV，回升 134 mV，已过 3700 告警线）。ledger 最后一条仍是 `05:28:23Z` 原厂刷入 ⇒ 中间无其他刷机。
- 镜像复核 `shasum -a 256 fw/vendor_boot_gpu_restart1.img` = `29963d92…976b99976`，77,393,920 B，与 §38.4 / §39 / §40 记录**逐字一致**（不是重建、不是 restart2/3）。
- 单次命令：
  ```bash
  MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_restart1.img
  ```
  `Writing 'vendor_boot' OKAY [0.056s]` → `Booting OKAY [0.191s]`（UEFI 仍从 RAM）；describe = vendor_boot v4 / 73.81 MiB / ramdisk 72.00 MiB / aligned=True。**boot / recovery / userdata 未动**。
- ledger `2026-11-12T05:44:14Z`：`flash … 29963d92… rc=0` 与 `boot … 043f7120… rc=0` 两条。
- 刷后 100 s `fastboot devices` / `adb devices` 均空 ⇒ **未回落 fastboot**，设备仍在启动/用户态（与 §39 / §44 同）。

### 47.3 待办：本 boot 屏照判读格

`logs/` 最新仍是 13:18 的 `restart2` 照片，本机无 USB 调试串口 ⇒ 本 boot 读数只能靠屏照。**拍一张放进 `logs/` 发我**。

⚠️ `restart1` 只有 `Q0`–`Q6` 七行，**没有 `RT2` / `Q7` / `Q8`**（这三行是 restart2/3 才有），别期待它们。判读只看：

| 观察 | 结论 | 下一步 |
| --- | --- | --- |
| `RT n1 s0 e0 b1` + `H h1 r1` + `Z5 f32/4/0/0` 复现 | `restart2` 全零是**镜像/偶发**差异，且电池无关 ⇒ 诊断扰动或启动随机性 | 刷 `restart3`（纯只读、自带 `s`/`t`/哨兵）取稳态读数 |
| `RT n0 s0 e0 b0` + `H h0 r0` + `Z5 f0/0/0/0` | 同一份 `restart1` 在 3883 mV 也全零 ⇒ **§45 强结论成立**：`-3` 在 GPU 完全空闲时出现，与 CP/ring/fence/hang 无关 | 转用户态：Turnip 重启保活补丁（§43.4）+ device 创建失败上下文（§46.6 C） |
| 部分复现（如 `RT n1` 但 `Z5` 全零，或反过来） | 提交/记账与 hangcheck 不是同一条路径，需按行拆 | 先记录，再决定是否需要 `restart3` |
| `R PUB no device: Vulkan(-3)` / `X7 …68f2` 消失 | 上层已解，属重大变化 | 立刻单独复核，不要套用上面三条 |

⚠️ 判图方向：最近两张（12:14、13:18）都是**正立**（EXIF orientation=1），更早几轮是倒置；裁切一律带 `ffmpeg -noautorotate`，zsh 不分词、crop 参数写死。

### 47.4 本轮状态

- 本轮**只刷了 `restart1` 一次**，未构建任何镜像、未改任何源码、未动 `UniXNU-sheng` / 主树 / `Adreno740` 子模块 / `fw/gpuaddon-17-full.bin`。
- 当前设备运行 `restart1`，电池 3883 mV；`fw/vendor_boot_gpu_restart3.img`（`ca332a0b…`）仍在，随时可刷。

## §48 2026-11-12 13:48 本 boot 屏照判读：**对照无效** —— 全零相同，但启动阶段不同

屏照 `logs/2026-11-12-13-48-39-917.jpg`（3840×2160）。

### 48.1 方向与判读方法（⚠️ 与前两轮不同）

- 本张是 **180° 倒置**，但 EXIF `orientation=1`（相机倒着拿、没写方向标记）。Vision OCR 会自动转正所以原图直接可读，但**像素是倒的**：直接 `-noautorotate` 裁切得到的是倒置/黑边，必须 `hflip,vflip` 转正。
- 前两轮（12:14、13:18）都是正立。⇒ **每轮都要先判方向，不能沿用上一轮结论**。
- 中间产物 PNG 会被 ffmpeg 二次应用旋转元数据（`crop` 报 `Invalid too big … width 2600`）。正解是**单条滤镜链**从原 JPG 一次完成：
  `ffmpeg -noautorotate -i <jpg> -vf "hflip,vflip,crop=2600:540:0:0,scale=5200:1080" out.png`
- 判读仍用「Vision OCR 行序定位 → 窄条 3× 放大 → 逐行视觉转录」，不采信 OCR 的十六进制（`0` 常被读成 `O/D/8`）。

### 48.2 逐行读数（本 boot 已逐行放大转录）

| 行 | §40 `restart1` 12:14 / 3420 mV | §45 `restart2` 13:18 / 3861 mV | **本 boot `restart1` 13:48 / 3883 mV** |
| --- | --- | --- | --- |
| `RT` | `n1 s0 e0 b1` | `n0 s0 e0 b0` | **`n0 s0 e0 b0`** |
| `H` | `h1 r1` | `h0 r0` | **`h0 r0`** |
| `Z5` | `f32/4/0/0` | `f0/0/0/0` | **`f0/0/0/0`** |
| `F` / `Z0` / `Z1` / `Z2` | 非零 | 全零 | **全零**（`F n0`、`Z0 t0 f0`、`Z1 r0/0`、`Z2 00/12/24/36` 全零 dump） |
| `W` / `T` / `X4` / `D0`–`D5` / `A0` / `A1` | — | 逐字相同 | **逐字相同**（`W` 全 f、`T a1/0 … g2/cb4f6000`、`X4 r32/32 f4 g1 s400`、`D4 cx0022f801>f822f800`） |
| `GPU calls` | `7 refused=7`（§40 记录） | `7 refused=7` | **`0 refused=0`** |
| `X7` | `…68f2` | `K4 6800 6801 6a3c 68F2` | **`K0`**（无 mark 值） |
| `R PUB no device` | `Vulkan(-3)` | `Vulkan(-3)` | **整行不存在** |
| `Q0`–`Q6` | 存在 | 存在（含 `Q8`） | **整组不存在** |
| `NATIVE-NVT frames=…` | 存在 | 5 行（3000→15000） | **一行都没有** |
| `X5` | `l6/2d6` | `l6/0` | `L0/0`（与上两轮均不同） |

### 48.3 代码级定案：缺失的行是「没产生」，不是「被裁掉」

- `BravearkLastLog.c:794`：`if (&muyu_gpu_reason != NULL && muyu_gpu_reason[0]) printf("MUYU R %s\n", …)` ⇒ **R 行只在 Metal bridge 写下失败原因后才打印**。本 boot 无 R 行 ⇒ `native_bridge_metal_open()` 的失败路径（`NativeAdrenoMetalPlugin.mm:404`）**从未执行**。
- `BravearkLastLog.c:813–816`：`if (!row[0] || row[0] == ' ') continue;` ⇒ **Q 行是跳过空行的**。Q0–Q6 只在 `heartbeat()` 的 stall 分支填充（§41.1），本 boot 全组缺失 ⇒ stall 分支从未产生过采样。
- `X7 K0`（mark 计数 0，无 `68f2`）与 `GPU calls=0 refused=0`（`LegacyDisplayProbe.cpp:69-98` 的 GOP 帧 offer 桥接计数）互相印证：`mark(0x68f2)` 从未调用、显示栈没有帧 offer。
- `NATIVE-NVT frames=` 行（上轮 5 行、计数已到 15000）本 boot 完全没有 ⇒ 用户态输入/渲染栈的进度日志尚未出现。
- 右侧 `x=2600…3840` 区域已单独裁切检查：只有 `Z2` 十六进制 dump 的延续，没有遗漏的 `MUYU` 文本行。

⇒ 本 boot 的屏显是**比 §45 更早的启动阶段快照**：系统尚未跑到 device 发布 / 显示 / 用户态阶段。不是相机取景问题，也不是「上层已解」。

### 48.4 结论：§47 的 A 对照**不能据此判定**

§47.3 的两条主要分支本 boot 都**不适用**：

- ❌ 不能套用「仍全零 ⇒ §45 强结论成立」。§45 的强结论建立在「`-3` 在 GPU 空闲时照样出现」；本 boot **根本没有 `-3` 这个观测**，全零只是「还没跑到」，与「跑了但没发生」不等价。
- ❌ 不能套用第 4 条「`R`/`X7` 消失 ⇒ 上层已解，重大变化」。代码级证据显示是阶段未到，不是失败消失。
- ✅ 能确定的只有：**同一份 `restart1` 镜像、在 3883 mV（与 §45 差 22 mV）下，`RT/H/Z5` 仍全零**，因此 §40 那次 `RT n1` 不是每次启动都会复现；低电量 / 启动阶段 / 随机性仍是候选解释，`restart2` 的诊断扰动假说进一步降级（但不是无罪）。

### 48.5 下一步（先补一张有效屏照，零构建、零刷机）

1. **请确认拍摄时机**：这张是刷机后多久拍的？当时屏幕是否还在进度条 / 是否已进入桌面或稳定日志页？
2. **再拍一张稳定后的屏照**（等进度条结束 / 桌面出现后再等 1–2 分钟；参照 §45 那张能看到 `NATIVE-NVT frames=` 与 `R PUB no device`，说明用户态已跑起来）。放进 `logs/` 发我。
   - 若稳定屏照出现 `R PUB no device: … -3` + `X7 …68f2` 而 `RT/H/Z5` 仍全零 ⇒ §45 强结论成立，转 §43.4 Turnip 补丁与 §46.6 C。
   - 若出现 `RT n1`/`H h1 r1`/`Z5 f32/4/0/0` ⇒ §40 路径复现，说明本 boot 只是拍得早，再刷 `restart3` 取纯只读稳态读数。
   - 若稳定后仍无 `R` 行且无 `NATIVE-NVT` ⇒ 才是真正的「上层行为变化」，单独复核启动是否卡在 device 发布之前。
3. ⚠️ 若不方便重拍，也可以先把设备回 fastboot 直接刷 `restart3`（自带 `Q8 t` 与 `RT2` 哨兵，能直接区分「hook 没跑」与「跑了但 CP 没动」），但**它同样受拍摄时机影响**，稳定后再拍屏照仍是前提。

### 48.6 本轮状态

- 本轮**未刷机、未构建、未改任何源码/镜像**；只做了屏照判读与文档记录。设备仍运行 `restart1`（13:44 刷入），ledger 最后一条 `05:44:14Z boot rc=0`。
- 三版镜像 sha 复核无变化：`restart1 29963d92…`、`restart2 bf0a1c33…`、`restart3 ca332a0b…`。
- `UniXNU-sheng` 工作树、主树、`Adreno740` 子模块、`fw/gpuaddon-17-full.bin` 均未动。

## §49 2026-11-12 14:0x 用户补充观察推翻 §48「拍得早」：这是**稳态卡滞**，断点在 GPU bringup 之后、用户态图形栈之前

用户原话：「苹果 logo 都没有，而且我是等了日志刷了好几遍一样的内容才拍的，以往这个时候早到桌面了」。

### 49.1 撤销 §48 的时机归因

- §48.3 判「本 boot 是**更早的启动阶段快照**、还没跑到 device 发布」，§48.5 据此建议「再拍一张稳定后的屏照」。**这个归因错了**：用户已经等了 ticker 刷好几遍、内容完全一致，说明这是**稳态**；再拍一张还是同一屏，§48.5 的第 2 步作废。
- §48.3 里「缺失的行是没产生、不是被裁掉」这条**代码级结论仍然成立**，但正确解读不是「还没跑到」，而是「**系统稳定停在 t≈20–40 s，那条链路一次都没启动**」。
- 「没有苹果 logo / 没到桌面」也不是新的独立故障：`muyu-dump-at=20` 会在 t≈20 s `muyu_console_take_panel()` 把 `native_legacy_swap_hold=1`（屏上 `hold=1`），注释明写 *"The system keeps booting headless"* ⇒ 屏上本来就该是一屏文本。**§45 那张同样是 `hold=1`**。但「以往这个时候早到桌面」说明以往显示客户端能抢回面板，本 boot 连一次 transfer 都没有 ⇒ 卡得更早，且是异常。

### 49.2 代码坐实：这批日志本来就是每 5 秒整批重绘的 ticker

- `board.toml:555` 启用 `muyu-dump-at=20`，`strings fw/vendor_boot_gpu_restart1.img` 确认 `muyu-dump-at=20` + `sheng-log=1` 都在镜像里。
- `LegacyDisplayProbe.cpp:516` 在 t≈10 s 调 `muyu_dump_start_late()`；`BravearkLastLog.c:575 muyu_dump_later()` → `IOSleep(20*1000)` → `muyu_console_take_panel()` → 一次性 `LASTLOG …` → **`for (unsigned pass = 1;; ++pass) { IOSleep(5000); … printf 全部 MUYU 行 … }`**（`:622` 起，循环闭合在 `:875`）。每 5 s 重绘 `LIVE / GPU / PROBE / B / H / F / Z0–Z5 / W / X / RT / RT2 / Q / D / A` 整批；`pass%12==0` 还打印 `muyu_proc_census()`。
- ⇒ 用户看到的「刷了好几遍一样的内容」**就是这个 ticker 的正常行为**。ticker 还活着证明内核线程、时钟、调度都正常，**不是 panic、不是内核死锁**；但所有计数器逐遍不变 ⇒ 图形/用户态链路完全冻结。这是比「一张静态屏照」强得多的证据。

### 49.3 逐字对比：§48 漏记了 `PROBE` / `B` / `GPU` 三行（本次已 4× 放大转录）

| 字段 | §45 `restart2` 13:18 / 3861 mV | **本 boot `restart1` 13:48 / 3883 mV** | 读法 |
| --- | --- | --- | --- |
| `PROBE st/cp/b` | `st=97 cp=10 b=24` | **`st=97 cp=10 b=24`** | 逐字相同 |
| `B i/o/f` | `i48 o40 f0 s0 b0 n0` | **`i40 o40 f0 s0 b0 n0`** | ⭐ `ias` 不同（49.5） |
| `GPU calls/refused/tc/ts` | `7/7/7/00000000 hold=1 pres=1` | **`0/0/0/ffffffff hold=1 pres=1`** | ⭐ transfer 从未执行（49.6） |
| `hold/pres` | `1/1` | `1/1` | 相同 ⇒ hold 不是元凶 |
| `RT / H / Z5` | `n0 / h0 r0 / f0/0/0/0` | `n0 / h0 r0 / f0/0/0/0` | 相同（稳态全零） |
| `X7` / `R` / `NATIVE-NVT` / `Q` | `68f2` / `-3` / 5 行 / 有 | **`K0` / 无 / 无 / 无** | 上层整条未启动 |

### 49.4 `st=97 cp=10 b=24` 精确解码：内核 GPU bringup **全绿**

- `st` 用 `%x`：`0x97` = `gpuCtxProgress(0x90 + 7)`（`:452`）；`Step` 枚举 `Allocate=1,Prepare,Program,Attach,VmAttach,Map,Ready=7`（`NativeAdrenoGpuContext.h:66`）⇒ **七步走完到 `Ready`**。
- `cp` 用 `%u`：`native_gpu_cp[0]=10` 来自 `:1565 c[0] = c[6] ? 8 : (c[7]==0x5e9a740 ? 10 : 9)`，`:1567 cpRow(c[0]==10 ? "ok" : …)`，`:1570 if (c[0]==10)` ⇒ **C10 = CP/ring/fence 全部成功、publishMetal gate 已过**（`BravearkLastLog.c:659` 注释同）。
- `b` 用 `%x`：`0x24` = `ArmSmmu4KStreams.c:98 CRUMB(0x24)`，是 `observe()` 的最后一步；`NativeAdrenoVm::observe()`（`:63–67`）最后一行正是 `ArmSmmu4KObserveStreams`，**成功终态就停在 0x24**；`barrier()` 只是 `dsb sy`（`ArmSmmu4KMmio.c:16`），不会挂。成功 `AttachStreams` 会覆写 `CRUMB(0x21)`、失败会写 `CRUMB(0x30)` ⇒ 0x24 是「最后一次只读 observe 成功」，**不是 attach 卡住**。
- ⚠️ 排除一个很诱人的误判：`ArmSmmu4KStreams.c:3–7` 与 `GpuPowerProbe.cpp:1474–1480` 都记录了 11-11「attaching CPU never came back」（crumb `0x1b`，未上电块的 load 不 fault 也不完成）的先例，crumb 就是为监测它加的。但本 boot `st=Ready` + `cp=10` + 无 fault + observe 成功 ⇒ **不是那类 MMIO hang**；§42.3 上游「全局 GFSR latch 挂住 render server」本 boot 也未命中（`Q8 g0`、无 R 行、无 `fail()` 计数）。
- ⇒ **断点在 GPU 内核 bringup 完成之后**，不在 §38 restart hook、不在 §41.4 secure mode、不在 §42.3 SMMU fault、也不在提交/fence 层。

### 49.5 ⭐ `i48 → i40`：目前唯一的非确定性硬差异（列为必测变量，未定案）

- `GpuPowerProbe.cpp:1124 muyu_gpu_bits[0] = NativeAdrenoGpuContextInputBits()`，`[1]=40` 固定（`:1125`）。注释明写：`NativeAdrenoPublish` 把 **`[GpuClientVaStart, 2^input_bits)`** 交给每个 user connection，旧字面量 31 会因「无窗口」让每次 open 被拒（`mount_fail 3/x1`）。
- 本 boot `i40`、§45 `i48` ⇒ **同一份驱动、不同 boot 拿到不同的 GPU client VA 位数**。若用户态 Metal/Turnip 按 48 位布局，40 位窗口足以让它拒绝连接或创建 device 失败。
- ⚠️ 但本 boot `B f0 s0 b0 n0`（`native_adreno_mount_fail` 全零）⇒ **没有任何 client 尝试 open**，窗口大小根本没被检验过 ⇒ **不能当因果证据**，只能记为下一轮必须一起读的变量。
- `openCpAperture()`（`:1102`）的 SCM/TZ 应答同样可能非确定，会影响 CP-side `tableMode()`，是第二个候选来源。

### 49.6 `calls=0 / tc=0 / ts=ffffffff` 精确含义

- `muyu_handoff_snapshot()`（`LegacyDisplayProbe.cpp:87–98`）：`calls`=`native_legacy_gpu_offer_calls`、`tc`=`native_legacy_transfer_calls`、`ts`=`native_legacy_transfer_status`；后者在 `:122` **初值 `-1`**。
- ⇒ `ts=ffffffff` 是「**初值从未被写过**」，配合 `tc=0`、`calls=0` ⇒ 显示 transfer / frame offer 路径**一次都没进过**，不是「跑了但失败」。§45 是 `7/7/00000000`（跑过 7 次且成功）。
- `BravearkLastLog.c:794` R 行需 `muyu_gpu_reason[0]` 非空；本 boot 无 R ⇒ `native_bridge_metal_open()` 的失败路径（§45.4 的 `-3` 产生链）**从未执行**。无 `NATIVE-NVT frames=` ⇒ 用户态帧泵未起。
- ⇒ 本 boot 的断点**比 §45 的 `VK_ERROR_INITIALIZATION_FAILED` 更靠前**：GPU 内核全绿，但 Metal bridge / display client / 用户态渲染栈整体没启动。§45 的强结论（`-3` 与 GPU 空闲无关、转 Turnip 补丁）**不能套用到本 boot**。

### 49.7 结论修正汇总

- ❌ **撤销** §48.3 的「拍得早 / 更早阶段瞬时快照」归因与 §48.5 的「再拍一张稳定屏照」建议。
- ❌ §47 的 A 对照**仍不能**用来判定 §45 强结论：本 boot 没有 `-3` 观测，属于**另一条启动分叉**，不是「跑了但没发生」。
- ✅ 三 boot 事实：§40 `restart1` / 3420 mV → `RT n1`、有 `-3`、有 NVT frames；§45 `restart2` / 3861 mV → 全零但 `-3` 仍在、NVT frames 5 行、`calls=7`；本 boot `restart1` / 3883 mV → 全零、`-3` 消失、`calls=0`、用户态图形栈未启动。**同一份 `restart1` 两次结果不同** ⇒ 非确定性启动分叉；电池已基本固定（3861 vs 3883 差 22 mV），不是电量。
- ✅ `restart2` 诊断扰动假说进一步降级（本 boot 跑的是 `restart1`，同样全零）。
- ✅ 现在可以确定的只有：同一份 `restart1` 在 3883 mV 下**会出现一条「内核 GPU bringup 成功但用户态图形栈完全未启动」的卡滞路径**。

### 49.8 下一步（本轮零刷机、零构建，先补一个观测）

1. **请拍一张包含屏幕最顶部 `MUYU LIVE p=<pass> t=<uptime>` 行的完整屏照**。本张 13:48 照片第一行就是 `MUYU GPU …`，LIVE 行落在取景外。`p`（ticker 第几遍）/ `t`（uptime 秒）能直接回答「刷了几遍、卡了多久、是否还在推进」；`pass%12==0` 还有 `muyu_proc_census()` 进程普查，能直接看出 launchd / SpringBoard / render server 是否起来了。这比再拍一张同样的日志有用得多。
2. 若 `t` 持续增长而所有 MUYU 计数不变 ⇒ 坐实「用户态图形栈卡死」。下一版诊断应加**上层路径里程碑**（`publishMetal()` 进入/返回、`native_bridge_metal_open()` 进入/返回、`native_legacy_gpu_offer()` 首次调用、launchd 阶段），并固定记录 `ias`；**不要**继续追 Q7/RT2 的 GPU 提交层。
3. ⚠️ **`restart3` 对本 boot 无用**：它诊断「hook 没跑 vs 跑了但 CP 没动」，而本 boot CP bringup 已 C10 成功、根本没到提交层。镜像继续待命，不必急刷。
4. 设备状态：本轮只读检查 `fastboot devices` / `adb devices` **均为空**（设备不在 fastboot/adb），检查已停止、未重试。**未刷机、未构建、未改任何源码/镜像**。若要回 rec 充电，按既定规矩需设备回 fastboot 后先刷原厂 `fw/stock/vendor_boot.img`。

### 49.9 本轮状态

- ledger 最后一条仍是 `05:44:14Z boot rc=0`；三版镜像 sha 未变（`restart1 29963d92…`、`restart2 bf0a1c33…`、`restart3 ca332a0b…`）。
- `UniXNU-sheng` 工作树、主树、`Adreno740` 子模块、`fw/gpuaddon-17-full.bin` 均未动；未重建 owner trust / addon。

## §50 2026-11-12 16:24 用户「刷原厂」：原厂 `vendor_boot` 已刷回，设备交回 rec 充电

用户只说「刷原厂」，按 §40 / §46 既定规矩执行（回 rec 充电必须先恢复原厂配置）。

### 50.1 刷前只读预检

- `fastboot devices` → `a20248f7 fastboot`；`adb devices` 空。
- `battery-voltage` **3668 mV**（脚本 preflight 自读 3666 mV，低于 3700 告警线并打印 `WARNING: battery …; charge before a long run`）；`current-slot=a`；`slot-unbootable:a=no`；`retry=6`。
- 原厂镜像复核：`fw/stock/vendor_boot.img` sha256 **`69f1e38c6ab70050ef86e37a36319443c1eddc4355543ab5395b895d0bcb96fd`**，100,663,296 B，与 §38.4 / §46 账本逐字一致。
- 三版诊断镜像 sha 未变：`restart1 29963d92…`、`restart2 bf0a1c33…`、`restart3 ca332a0b…`（仍待命）。
- `cmd_stock()`（`tools/muyu-flash-mode.sh:191–202`）代码复核：只做 `preflight` + `fastboot flash vendor_boot "$STOCK_VENDOR_BOOT"` + 记 ledger；**不引导 UEFI、不 reboot、不写 `boot` / `recovery` / `userdata`**。`boot`/`recovery` 只有 `restore-stock --yes` 才会动。

### 50.2 执行结果

```
bash tools/muyu-flash-mode.sh stock
== slot=a unbootable=no retry=6
WARNING: battery 3666 mV; charge before a long run
== flashing stock vendor_boot (charging mode, no bring-up)
   Writing 'vendor_boot'                              OKAY [  0.068s]
   Finished. Total time: 2.365s
== now: long-press power 10-15 s to power off, then plug the wall charger in for 1-2 h
```

- ledger 新增：`2026-11-12T08:24:53Z preflight`（`slot=a unbootable=no`）、**`2026-11-12T08:24:56Z flash vendor_boot fw/stock/vendor_boot.img 69f1e38c…fd rc=0`**（UTC，= 北京 16:24:56）。**没有 `boot` 记录**，符合 stock 模式。
- 刷后 `fastboot devices` 仍为 `a20248f7`（脚本不重启，设备仍在 fastboot）。
- 充电操作：按脚本提示**长按电源 10–15 s 关机 → 插墙充 1–2 h**；fastboot 本身不充电，屏上只显示电池图标。电池已从 13:43 的 3883 mV 降到 3668 mV，确实该充了。

### 50.3 交接状态与回来后的注意事项

- 当前 `vendor_boot` = **原厂**，设备可安全在 rec 充电；`UniXNU` 诊断镜像全部未刷、未改。
- ⚠️ 充电回来若要继续 UniXNU：**必须先重新刷诊断镜像**（UEFI 是 `fastboot boot` 从 RAM 引导，断电/重启即丢），命令仍是 `MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu <img>`。
- §49 的待办仍有效但**需要重新取样本**：上一 boot（13:48 `restart1` 稳态卡滞）已经结束，那张缺 `MUYU LIVE p=<pass> t=<uptime>` 行的屏照无法补拍。下次刷 UniXNU 镜像后请拍**包含屏幕最顶部 LIVE 行**的完整屏照，`p`/`t` 能判定卡滞时长、进程普查能判定用户态图形栈是否起来。
- `restart3` 仍按 §49.8 判定为「对 13:48 那条卡滞路径无用」，继续待命；下一版优先考虑上层路径里程碑诊断（publishMetal / nmb_device_open / 首次 gpu_offer / launchd 阶段 + 固定记录 `ias`）。
- 本轮**只刷了原厂一次**；未构建镜像、未改任何源码、未动 `UniXNU-sheng` / 主树 / `Adreno740` 子模块 / `fw/gpuaddon-17-full.bin` / owner trust / addon。

## §51 2026-11-12 16:2x 用户问「GPU 什么时候能用」：关卡判断 + 充电期间预建 `upper1` 上层里程碑诊断镜像

### 51.1 直接回答：不能给日期，且比早上的估计更保守

不给时间承诺，原因是**真正的门槛还没定位到**。今天下午 §49 把早上「只差最后一道关」的判断推翻了：13:48 那次稳态卡滞里，内核 GPU bringup **全绿**（`PROBE st=0x97` = `Ready`、`cp=10` = C10 成功、`b=0x24` = SMMU observe 正常终态），但 `calls=0 / tc=0 / ts=ffffffff`、无 `R` 行、无 `NATIVE-NVT frames=` ⇒ **用户态图形栈整体没启动，连 `-3` 都没产生**。所以现状不是「GPU 差一点」，而是「启动链路都还没走完」。

### 51.2 剩余关卡（必须按顺序通过）

| # | 关卡 | 状态 | 已知候选 / 下一步 |
| --- | --- | --- | --- |
| 1 | 用户态图形栈为何没启动（13:48 路径） | **未定位，当前 blocker** | `upper1` 的 `MUYU UP` 行 + 含 LIVE 行的完整屏照；区分「launchd/用户态没起」/「服务发布了但无 client」/「client 连了被拒」 |
| 2 | `VK_ERROR_INITIALIZATION_FAILED`（-3） | **已定位代码链，未修** | §45.4 链路 `NativeAdrenoMetalPlugin.mm:400 ← nmb_device_open ← Device::system_default_with`；两条候选：① `turnip-darwin-keep-device-after-restart.patch`（§43.4 确认 17 线 addon 里没有，需重建 `fw/gpuaddon-17-full.bin`）；② 重建 owner-signed `NativeAdrenoMetal.bundle` 补 device 创建失败上下文（§46.6 C，会同时改 bundle 签名/CDHash/trust/addon 四个输入） |
| 3 | GPU 提交 / fence / hangcheck / restart 层 | **未验证** | 前两关过了才知道还有没有问题；§40 的 `RT n1 s0 e0 b1` + `H h1 r1` + `Z5 f32/4/0/0` 说明这条路径存在但**不是每次复现**（§47/§49 两次全零） |

### 51.3 节奏约束（为什么是「以天计」）

- 每关至少一轮「改镜像 → 刷 → 等 ticker 稳定 → 屏照 → 逐行判读」。今天 11:30–16:24 做了约 6 轮，推进的是**定位**，没有通关。
- 每轮之间设备必须回 rec 充电 1–2 h（`fastboot` 不充电）；电池 3883 mV（13:43）→ 3668 mV（16:24），实验窗口被供电硬约束。
- 上游 `sheng` 在 iPadOS 27 + macOS 27.3 已跑通完整方案（signed ZAP firmware + Turnip + metal2vulkan + GMU），但 17 线是 iPadOS 17、boot arg 前缀不同、`Adreno740` 驱动落后上游 8 天 ⇒ §42.5 定案**不整体升级，只单点移植**，速度受限。
- 结论：现实量级是**以天计，不是以小时计**；在关卡 1 定位之前给任何日期都是猜。

### 51.4 充电期间预建 `upper1`（零设备操作，回来直接刷）

只改 `UniXNU-muyu-17/package/BravearkLastLog.c` 一个文件（改前备份 `/tmp/restart4-backup/BravearkLastLog.c`，sha 前缀 `221758ce`），基于 `restart3` 基线继续累加，**纯只读、零行为改动**（唯一例外是打印频率）：

1. ticker 循环内新增一行 `MUYU UP`，格式：
   `MUYU UP lg=<stage>/<init>/<start>/<dt> s=<surface_found>/<surface_status> op=<opened>/<closed> mf=<failcode>/<failcount> i=<ias> r=<reason_first_char>`
   - `lg` = `native_legacy_stage` / `init_result` / `start_result` / `dt_attached` ⇒ legacy display bring-up 走到哪一步（`:393–637` 的 1..10）；
   - `s` = `native_legacy_surface_found` / `surface_status` ⇒ IOSurface 是否找到；
   - `op` = `native_adreno_publish_stats()` 的 `g.opened` / `g.closed`（`NativeAdrenoPublish.cpp:1242`，**纯只读 getter**）⇒ 有没有用户态客户端连过 GPU 服务；
   - `mf` = `native_adreno_mount_fail[0]` / `[3]` ⇒ 连接是否被拒、拒了几次；
   - `i` = `muyu_gpu_bits[0]`（`ias`，§49.5 的 `i40`/`i48` 变量）；`r` = `muyu_gpu_reason[0]`（`-` 表示没写，即没走到 §45.4 的 -3 失败路径）。
   - 所有符号 `weak_import` + 判空，缺链接打印哨兵而不改变行为；`native_legacy_surface_status` 按定义的 `volatile int` 声明。
2. `muyu_proc_census` 打印频率 `pass % 12`（60 s）→ **`pass % 3`（15 s）**，让早期卡滞时的屏照也能带上进程普查（能看到 launchd / SpringBoard / render server）。仅影响打印频率。

判读格：
- `op=0` + `lg` 停在早期 + census 无用户态进程 ⇒ 关卡 1 是「用户态根本没起来」，转 launchd / root volume 方向；
- `op=0` 但 `lg` 已到 10（display started）⇒ 服务发布了却没有 client，转 Metal bridge 加载 / bundle 签名 / trust cache；
- `op>0` 且 `mf` 非零 ⇒ client 连了但被拒，直接对照 `mount_fail` 与 `ias` 窗口大小（§49.5）；
- `r` 非 `-` ⇒ 回到 §45.4 的 `-3` 链路，关卡 2 成为主战场。

### 51.5 构建与验证

- §24.9 三步链全 `rc=0`（`/tmp/build_{k,p,v}51.log`，`grep -i error\|warning` 无命中）：kernel → payload（同 §43/§46 的 boot-arg / addon / trust / `--unique-chip-id`）→ `patch-vendor-boot.py --base /tmp/base96.img`。
- 产物 **`fw/vendor_boot_gpu_upper1.img`**：77,393,920 B，sha256 **`26ef2fb0ce41ce7d31c8f59e2394497a89fe77d7517aa4712c4ebe92cd15fcce`**。
- `strings` 对照：`MUYU UP lg=` 只在 `upper1`（1 次）；`MUYU RT2 --` 与 `K#%u s%u w%x` 在 `restart3` 和 `upper1`（证明基于 restart3），`restart1`/`restart2` 均为 0。
- 三版旧镜像 sha 未变：`restart1 29963d92…`、`restart2 bf0a1c33…`、`restart3 ca332a0b…`；原厂 `69f1e38c…fd` 未变。

### 51.6 回来后的唯一动作与状态

1. 设备回 fastboot → `MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_upper1.img`（只写 vendor_boot）。
2. 等 ticker 刷 3–4 遍（约 15–20 s）再拍，**必须包含屏幕最顶部 `MUYU LIVE p=<pass> t=<uptime>` 行**（13:48 那张就是缺这行）。`p`/`t` 判定卡滞时长。
3. ⚠️ 这一轮的目标是**定位关卡 1，不是修好 GPU**；定位之后才能给更准的时间判断。
- 当前 `vendor_boot` = 原厂（§50），设备正在 rec 充电；ledger 最后一条 `08:24:56Z flash stock rc=0`。本轮**未刷机、未碰设备**，只构建了一版镜像，`UniXNU-sheng` / 主树 / `Adreno740` / addon / trust 均未动。

## §52 2026-11-12 18:02–18:06：`upper1` 屏照显示进程表只有 1 个进程，未见 launchd

### 52.1 本轮执行与证据

- 刷前 `tools/muyu-flash-mode.sh status`：fastboot 设备存在，slot `a`、`unbootable=no`、电池 **3924 mV**；`adb devices` 为空。`fw/vendor_boot_gpu_upper1.img` SHA-256 `26ef2fb0…cd15fcce`、UEFI SHA-256 `043f7120…e5bd3` 均匹配。
- 通过唯一入口执行 `MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_upper1.img`。vendor_boot flash 和 RAM boot 均 `OKAY`；台账记录时间 `2026-11-12T10:02:24Z`，没有写 boot/recovery/userdata。
- 屏照 `logs/2026-11-12-18-06-19-393.jpg`（3840×2160，EXIF orientation 缺省），SHA-256 `e703b17e3b50909756ac6ef7b7d1cea7455a65c61879a368f75bf1d3b73b9823`。像素需顺时针旋转 90° 后可读。以下是放大后的人工逐字复核，不以 OCR 十六进制为依据：

```
MUYU LIVE p=24 t=260 ...
MUYU GPU calls=0 refused=0 o=0 c=0 f=0 fb=0 tc=0 ts=ffffffff hold=1 pres=1 | w=0
MUYU PROBE st=97 cp=10 b=24
MUYU UP lg=10/1/1/1 s=1/0 op=0/0 mf=0/0 i=48 r=-
PROC n=1 zomb=0 launchd=0- backboardd=0- SpringBoard=0- liveact=0-
```

### 52.2 结论：当前证据指向 launchd 之前/启动阶段

- `LIVE p=24 t=260`：ticker 仍在工作，屏照时 XNU uptime 260 s；不是刚启动瞬间的早期快照。
- `PROBE st=97 cp=10 b=24`：和 §49.5 一致，GPU 内核 bring-up 七步、CP/ring/fence 与 SMMU observe 都在正常终态。
- `UP lg=10/1/1/1 s=1/0`：legacy display start 已成功到 stage 10，DT attached，IOSurface found/status 0。
- `op=0/0 mf=0/0 i=48 r=-`、GPU `calls=0 tc=0 ts=ffffffff`：没有 GPU publish client、mount fail 或 `-3` 路径证据。
- `PROC n=1` 来自 `MuyuCrashLog.c:muyu_proc_census()` 对 `allproc` 的全表遍历；其余被查进程均不存在（pid 0 / `-`）。所以 **stage 10 只证明 legacy display 路径走完，不证明 launchd 或图形用户态已启动**。这张图不能归类成“服务正常发布、只是 Metal bridge 没连接”；用户进程表只有一个条目，优先解释为何首个用户进程未创建。

这是屏照的单次静态观测；它证明拍照时进程表状态，不单独区分 root mount、`load_init_program`、launchd exec 失败或其他早期启动原因。也没有复现 §45.4 的 `-3`，GPU 提交/fence 仍不是当前主线。

### 52.3 离线源码追踪结果

`MUYU LIVE` ticker 是由 `muyu_dump_start_late()` 产生；当前调用点在 `load_init_program()` 入口。BSD `vfs_mountroot()` 成功后才会调用 `load_init_program()`，因此本轮证据把 root mount 排除在当前断点之前。缺的边界是 `load_init_program()` 内 owner-cache / Data mount 子阶段是否完成，以及是否尝试/完成 launchd exec。

`configs/muyu-userland_defconfig` 明确关闭 `CONFIG_XNU_BOOT_PROGRESS`，因此 XNU patch 的 `boot_progress_stage()` 在此镜像是 no-op；本次未启用整套 BootProgress UI。已从当前构建出的 17 线 `kern_exec.c` 静态确认有可读全局：`native_owner_cache_stage/status/mount_status` 和 `native_owner_data_stage/status`。子阶段编号见代码 `native_owner_cache_prepare()` / `native_owner_data_prepare()`；新 ticker 会读这些现成值，不写它们、不改启动流程。

下一轮判读只看一次性 `MUYU ROOT` 快照值：

| 读数 | 含义 / 下一分支 |
| --- | --- |
| `c=3/<status>/-1` | SystemOS mount 已进入，mount 调用尚无返回状态；卡在 mount 路径 |
| `c=3/<nonzero>/<nonzero>` 或 `c<6/<nonzero>/...` | owner-cache 准备返回失败；与启动后 Metal 无关 |
| `c=6/0/<mount-rc> d=2/-1/-` | SystemOS 已成功，Data mount 正在调用中 |
| `c=6/0/... d=3/-1/...` | Data mount 已返回，卡在 `/private/var` 查找/校验或后续 addon/app 准备 |
| `c=6/0/... d=4/0` | 两个 owner mount 都成功；下一轮才单独追踪 launchd candidate exec |
| `c/d` 均停在 0 或初始 `-1` | 对照完整格式与符号链接；若真实是无符号哨兵，不解读为运行时阶段 |

**暂缓** Turnip、Metal plugin、trust cache、GPU 行为和 restart 层改动。若设备要回 rec 充电，必须先刷原厂 vendor_boot；下一轮回来仍需重新刷诊断镜像和 UEFI。

## §53 2026-11-12：预建 `MUYU ROOT` 只读诊断镜像（未刷）

### 53.1 改动与目的

仅改 `UniXNU-muyu-17/package/BravearkLastLog.c`：每个现有 5 s ticker 周期打印一行：

```
MUYU ROOT c=<native_owner_cache_stage>/<native_owner_cache_status>/<native_owner_cache_mount_status> d=<native_owner_data_stage>/<native_owner_data_status>
```

所有字段均为 weak-import 只读 getter；缺符号打印 `-2`。owner-cache/data 函数的阶段值在当前构建的 XNU 源码里已有定义。无 boot-arg、无 register write、无 root mount 或 exec 行为改动。判读格在 §52.3。

### 53.2 构建证据

- kernel build log `logs/build-kernel-root1.log`：Python 3.12；编译时显式指定 iPhoneOS 27.2 SDK。kernel SHA-256 `9b9dbd2d1391e72d791db97d2bf3e4584a3b3449495e45656fe9cf6587400fd4`。
- payload build log `logs/build-payload-root1.log`；payload SHA-256 `9a994ab994b25ab168b82e809800d0ac16bd8ca427e29293215b7b8af7382a94`。
- vendor_boot build log `logs/build-vendor-boot-root1.log`。产物 `fw/vendor_boot_gpu_root1.img`，77,393,920 B，SHA-256 `118a98e468fa0ed5c3ea9af98ef1a4729d8abc1ddd377e9e9eb12866e566ce9b`。
- `strings` 确认镜像同时包含 `MUYU UP ...` 与 `MUYU ROOT ...` 格式串；三段构建日志的 `error|warning` 检查无命中。
- 首次构建因系统 Python 无 `tomllib` 失败；改用 Python 3.12 后，另一次因编译器默认 macOS SDK 而停止；显式使用 iPhoneOS 27.2 SDK 后构建完成。两个失败都没有修改源码以外的状态。

### 53.3 当前状态 / 唯一设备动作

本镜像**尚未刷机**。屏照时最后已知是 `upper1` 启动；刷后设备/电量状态没有轮询复查。设备回 fastboot 后预检，再执行：

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_root1.img
```

等待至少 3 次 ticker 后拍完整屏照，包含顶端 `MUYU LIVE`，读 `MUYU ROOT`、`MUYU UP`、`PROC`。照片回来后先按 §52.3 判读，不并行加第二个变量。


## §54 2026-11-12 18:32–19:20：`root1` 已实刷并拍到 `MUYU ROOT`；更正「LIVE ticker 证明 init 入口」的推断

### 54.1 先更正事实：`root1` 不是「未刷」

交接文档 §6 / §53.3 写的是「`root1` 尚未刷入」。台账第 3058 行显示实际已在 **2026-11-12T10:32:11Z（18:32）** 刷入
`fw/vendor_boot_gpu_root1.img`（sha `118a98e4…`），同一秒 RAM boot 17 线 UEFI；18:35 拍屏照；**18:51（10:51Z）**
刷回原厂 `fw/stock/vendor_boot.img` 并转 rec 充电。本轮接手时设备在 rec、38%、Charging，属安全状态。

⇒ `root1` 这一轮**已经跑完**，只是没有被判读、没有写章节。

### 54.2 屏照 `logs/2026-11-12-18-35-51-844.jpg`（sha256 `d83bc28b…7d77be`，3840×2160）

**方向判定走了弯路，记录方法以免重犯**：预览渲染看上去像 180° 倒置，先按 180° 处理得到的是镜像文字；改用
`swift` + Vision 取**行框形状**才定案——未校正时行框是「高瘦」型（w≈60、h≈600–900，说明文字沿竖直方向排布），
`transpose=2`（90° 逆时针）后行框变成「宽扁」型且左对齐在 x≈307。**以后判方向用行框长宽比，不要靠肉眼/EXIF。**

校正后完整批次逐行视觉转录（关键数字已 5× 放大复核）：

```
MUYU A1 r00000000 cb00000167 a00000003
MUYU UP lg=10/1/1/1 s=1/0 op=0/0 mf=0/0 i=48 r=-
MUYU ROOT c=0/-1/-1 d=0/-1
MUYU LIVE p=21 t=344 btn st=3 irq=0 rep=0 msk=0x0 pon=476 gpio=-1 pdc=0x30200 err=0 sp=0 oth=0
MUYU GPU calls=0 refused=0 o=0 c=0 f=0 fb=0 tc=0 ts=ffffffff hold=1 pres=1 | w=0
MUYU PROBE st=97 cp=10 b=24
MUYU B i=48 s=0 b=0 n=0
```

与 18:06 的 `upper1` 相比：`t` 从 260 → **344**，其余计数逐字段不动 ⇒ 仍是**稳态卡滞**，不是瞬时快照。
无 `R` 行、无 `-3`。本张照片的可见窗口从批次中段开始，`p=21` 那批的 `PROC` 普查行落在画面外，故本轮**没有**
进程表读数（上一轮 `n=1`、无 launchd 的结论仍以 18:06 那张为准）。

### 54.3 ⭐ 逻辑更正：屏上有 `LIVE` 行**不能**证明 `load_init_program` 已被进入

§52.3 和交接文档 §6.2 都据此推断「ticker 存在 ⇒ 已进入 `load_init_program`」。这个推断不成立：

- `package/LegacyDisplayProbe.cpp:518` 在 `native_legacy_display_late_probe()` 里**无条件**（weak）调用
  `muyu_dump_start_late()`，注释明写这是 2026-10-28 特意从 `load_init_program` 挪过来的，为的是在图形客户端
  切换显示模式**之前**抢面板。
- `package/BravearkLastLog.c:942` 的 `muyu_dump_start()` 有 `static int started` 守卫，**first caller wins**；
  `muyu_dump_start_late()` 只是转调它。
- 本 boot 的 `UP lg=10` 证明 legacy display 路径跑过 ⇒ display probe 先起 ticker ⇒ `load_init_program` 里那次调用
  是空转。

⇒ 「ticker 存活」只证明 display probe 跑过，**对 init 入口没有证明力**。§52.3 该条作废。

### 54.4 `MUYU ROOT c=0/-1/-1 d=0/-1` 能说明什么、不能说明什么

先排除一个会让人白跑一轮的陷阱：若 `XNU_OWNER_CACHE_MOUNT` / `XNU_OWNER_DATA_MOUNT` 在 17 线构建里是关的，
`cache_stage` 永远是 0，读数就没有意义。已核实内核里有 `offline owner SystemOS mount failed` 与
`offline scratch Data mount failed` 两个 panic 串 ⇒ 两段代码**确实被编进去**。符号侧也已用
`nm -gU kernel.macho` 确认 6 个全局符号全部导出（不会落到 `-2` 哨兵）。
初值来自 `kern_exec.c:7076`（`native_owner_cache_stage = 0`），只在 `native_owner_cache_prepare()` 内部被改成 1…6。

所以读数的字面含义是确定的：**两个 `prepare` 都没有执行到第一句赋值**。但 54.3 更正之后，它对应两种解释：

- **A**：`load_init_program` 根本没被调用（断点在 `vfs_mountroot()` 返回之后、init 入口之前）。
- **B**：被调用了，卡在 init 阶段的 1–4 之间（`current_map()` / `mach_vm_allocate_kernel()` / 进入 owner-cache 块前）。

**两者不能用同一张照片区分**，也不能拿「legacy display 到 stage 10」去替代（display probe 与 init 是两条独立路径）。

### 54.5 离线预建 `init1`（设备充电期间完成，零设备操作）

只读诊断：`muyu_init_stage` 覆盖 `load_init_program` 入口到 owner-cache 调用之间的每一段（`kern_exec.c:8218–8246`：
入口=1、`current_map()` 后=2、`vm_map_page_size()` 后=3、`mach_vm_allocate_kernel()` 后=4、进入 cache 块=5、
`native_owner_cache_prepare()` 返回=6）。ticker 增加一行 `MUYU INIT q=%d`，纯读，不改执行顺序。

源码 `BravearkLastLog.c` sha256 `3813da1a…f0d0b84`，改前备份 `/tmp/init1-backup/`；相对 `/tmp/restart4-backup/`
仅 60 行差异，全部为 `MUYU UP` / `MUYU ROOT` / `MUYU INIT` 三条打印行与 `pass%12→pass%3`，无行为改动。

| 阶段 | 产物 | SHA-256 | 日志 |
| --- | --- | --- | --- |
| kernel | `out/muyu/kernel/kernel.macho` | `702ee90542dcfcfafbe5955026f900da5746a987f92c3bb48acc885983bca975` | `logs/build-kernel-init1.log`，EXIT=0 |
| payload | `out/muyu/payload/payload.img`（72 MiB） | `991a16f3c9b60bcc295ddb75abab6ee0f2751d12c53bd012f1efb60382265068` | `logs/build-payload-init1.log`，EXIT=0 |
| vendor_boot | `fw/vendor_boot_gpu_init1.img`（77,393,920 B） | `6300f4c2f2eaa4d6c17edda91b98d56b1d0e11eb96f0d923d05d1b992fd00a55` | `logs/build-vendor-boot-init1.log` |

三段日志 `error|warning` 均为 0 命中；`strings` 确认镜像同时含 `MUYU INIT q=%d`、`MUYU ROOT …`、`MUYU UP lg=…`。

### 54.6 `init1` 判读格（刷机前写定，事后不改）

| `q` | 含义 | 下一分支 |
| --- | --- | --- |
| `-2` | 符号未解析 | 不应出现（nm 已验）；出现则查 prelink 是否裁符号 |
| **`0`** | `muyu_init_stage = 1` 从未执行 | `load_init_program` **未进入**（或卡在 `muyu_dump_start_late()` 内）⇒ 断点在 `vfs_mountroot()` 之后、init 入口之前，转查首个用户进程创建前的 kernel 路径 |
| `1` / `2` | 已进入，卡在 `current_map()` 或 `vm_map_page_size()` | 查 vm_map 侧锁/页大小路径 |
| `3` | 卡在 `mach_vm_allocate_kernel()` | 查内核 map 分配 |
| `4` | 分配已返回，未进入 owner-cache 块 | 窗口极窄；稳态停在此值说明该行之后异常 |
| `5` | 已进入 owner-cache 块，但 prepare 未执行到 `stage=1` | 断点精确到函数入口 |
| `6` | cache prepare 已返回（`cache_status` 应非 -1） | 断点在 Data mount 段或之后 |

**证伪条件**：若 `q ≥ 1`，则解释 A（「未进入 init」）**不成立**；若 `q = 0` 且 `ROOT` 仍全为初值，则解释 B 不成立。
任一结果都只收敛一步，不跳步去碰 Turnip / Metal plugin / trust / addon。

### 54.7 当前状态与唯一设备动作

设备在 rec 充电（38%，Charging），`vendor_boot` 为原厂，`adb` 在线但**不是** fastboot——按 §2.1 第 5 条不轮询、不自行重启。
设备回到 fastboot 并通过只读预检后，唯一动作：

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_init1.img
```

等 ticker 刷 3–4 遍，拍含顶端 `MUYU LIVE`、`ROOT`、`INIT`、`UP`、`PROC` 的完整屏照，按 54.6 判读。

### 54.8 本轮状态

零刷机、零设备行为改动；产出 `init1` 镜像一枚 + 两处文档更正（`root1` 已实刷、ticker 不证明 init 入口）。

## §55 2026-11-12 19:22–19:30：`init1` 刷入与判读——`MUYU INIT q=0`，`load_init_program` 未被调用

### 55.1 刷前只读预检与执行

`bash tools/muyu-flash-mode.sh status`：fastboot 在线，`current-slot=a`、`slot-unbootable=no`、电池 **3914 mV**；
`adb devices` 为空；`fw/vendor_boot_gpu_init1.img` SHA-256 `6300f4c2…92fd00a55`、UEFI `043f7120…e5bd3` 均与记录一致。

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_init1.img
```

`Writing 'vendor_boot' OKAY [1.822s]`；`booting UEFI from RAM OKAY [0.242s]`。未写 `boot` / `recovery` / `userdata`。

### 55.2 屏照 `logs/IMG_20261112_192601.jpg`

sha256 `5d069d0a628ddb767a222567e9ed017b9cc5bb2e905db9f455dcc32424062877`，3840×2160，**本身正立，无需旋转**
（按 §6.5 的行框长宽比判定：h≈39–70、w≈280–1900，宽扁型）。关键行已 10× 放大复核。

```
MUYU ROOT  c=0/-1/-1 d=0/-1
MUYU INIT  q=0
PROC n=1 zomb=0 launchd=0- backboardd=0- SpringBoard=0- Liveact=0-
MUYU LIVE  p=25 t=260 btn st=3 irq=0 rep=0 msk=0x0 pon=476 gpio=-1 pdc=0x30200 err=0 sp=0 oth=0
MUYU GPU   calls=0 refused=0 o=0 c=0 f=0 fb=0 tc=0 ts=ffffffff hold=1 pres=1 | w=0
MUYU PROBE st=97 cp=10 b=24
MUYU B     i=48 s=0 b=0 n=0
```

`t=260` 且 `p=25`（25%3≠0 那批不印 census，但这一批的 `PROC` 行在画面内），与 18:06 / 18:35 两次一致：稳态卡滞。

### 55.3 ⭐ 判读：`q=0` ⇒ `load_init_program` 在 t=260 s 时**根本没有被调用**

`muyu_init_stage` 的 `volatile int` 初值为 0，`load_init_program()` 入口第一件事之后就是 `muyu_init_stage = 1`
（`kern_exec.c:8230`）。`q=0` 说明该赋值从未执行。

- §54.6 判读格：`q=0` ⇒ **解释 A 成立、解释 B（「进了 init 卡在 1–4」）证伪**。
- 剩余的非 `q=0` 解释只有「卡在 `muyu_dump_start_late()` 内部」，但该函数在 display probe 已经起过 ticker 之后
  只是命中 `started` 守卫直接返回（`BravearkLastLog.c:942`），不成立。
- 数值可信度：10× 放大目视确认是封闭环形 `0`，不是 `6`/`8`；OCR 只用来定位行序。

### 55.4 连带撤销：「root mount 已完成」这个前提也没有依据了

§52.3 / 交接文档旧版把「root mount 已完成」建立在「ticker ⇒ 已进入 `load_init_program`」之上，而该推断已在 §54.3
作废。现在 `q=0` 进一步说明**连 init 入口都没到**，因此：

> 到目前为止，**没有任何观测证明 `vfs_mountroot()` 已经返回**。

`load_init_program` 的唯一调用点在 `bsd_init.c:1292`，而 `vfs_mountroot()` 在同一函数 `bsd_init.c:1020`。两者之间有
约 270 行：rootauth / owner-root 决策、`set_rootvnode`、`devfs_kernel_mount`、`microtime`、kext 与 task 相关准备。
**断点已经前移进 `bsd_init()`**，`upper1`/`root1` 关心的 owner-mount 与 launchd 都在这之后。

另外注意：`boot_progress_stage()` 在 17 线是 `((void)0)`（`osfmk/kern/boot_progress.h:25`，`CONFIG_XNU_BOOT_PROGRESS` 关闭），
源码里那一堆现成阶段串全被编译掉，不能指望它们。

### 55.5 下一轮：`bsd_init` 阶段梯（`bsd1`，尚未构建）

沿用 `muyu_init_stage` 那套已验证的做法：新增补丁
`patches/xnu/zzzzzzzzzzzzzzzzzzzzz-bsd-stage-probe.patch`（补丁目录现有最后一个为 `…-init-stage-probe.patch`），
在 `bsd_init.c` 里加 `volatile int muyu_bsd_stage`，只在下列位置做**纯赋值**：

| 值 | 位置 |
| --- | --- |
| 1 | `bsd_init()` 入口 |
| 2 | `inittodr()` 之后、mountroot 循环之前 |
| 3 | `vfs_mountroot()` 返回 0（`break` 处） |
| 4 | `set_rootvnode()` 之后 |
| 5 | rootauth / owner-root 块之后（`autherr` panic 判定之后） |
| 6 | `devfs_kernel_mount()` 之后 |
| 7 | 调用 `load_init_program(p)` 之前 |
| 8 | `load_init_program()` 返回之后 |

ticker 增加 `MUYU BSD s=%d` 一行（弱链接 + 判空，同现有三条）。判读格：

| `s` | 结论 |
| --- | --- |
| `-2` | 符号未解析（先查 `nm -gU`） |
| `0` | `bsd_init()` 未被调用 ⇒ 断点在 kernel bootstrap，比预想更早 |
| `1` | 已进入，卡在 `bsd_init` 前段（`inittodr` 之前） |
| `2` | **卡在 `vfs_mountroot()` 内部** ⇒ 转入 root mount 路径（APFS / owner root 挂载） |
| `3` | mount 已返回，卡在 `IOSecureBSDRoot` / `VFS_ROOT` / `set_rootvnode` 段 |
| `4` | 卡在 rootauth / owner-root 决策段（现有 `native_owner_root_*` 变量可做二级证据） |
| `5` | 卡在 `devfs_kernel_mount()` |
| `6` | 卡在 devfs 之后、init 调用之前（kext / task 准备段） |
| `7` | 到了调用点却没进函数 ⇒ 查调用约定/栈 |

**证伪条件**：若 `s=3` 或更大，则「vfs_mountroot 可能未返回」不成立，断点收窄到后续段；若 `s=0`/`1`/`2`，
则 owner-mount / launchd / Metal 全都不在因果链上。

### 55.6 本轮状态

只刷了 `vendor_boot`；设备在 19:26 拍照时已稳定运行 260 s，之后未再操作。后续设备要回 rec 充电须先 `stock`（§2.1）。

## §56 2026-11-12 19:33–19:45：`bsd1` 判读——`MUYU BSD s=2`；真正的断点在 root 设备发现，不在 init

### 56.1 刷入

`bash tools/muyu-flash-mode.sh status`：fastboot 在线、`slot=a unbootable=no`、**3806 mV**（比 19:29 的 3894 掉了 88 mV，
fastboot 待机会掉电）。`adb devices` 空。镜像 `fw/vendor_boot_gpu_bsd1.img` 77,393,920 B，SHA-256
`d029545cdb0ba69505cf8e819b7c014425205ba93766c5b0e256c072b4c3b964`，`strings` 确认含 `MUYU BSD s=%d` /
`MUYU INIT q=%d` / `MUYU ROOT …` 三条。`nm -gU kernel.macho` 显示 `_muyu_bsd_stage` 与 `_muyu_init_stage`
均为全局（`fffffff007bd0f38 S`），不会落到 `-2`。

刷入 `Writing 'vendor_boot' OKAY [0.057s]`，UEFI `booting from RAM OKAY`，台账 `11:32:52Z flash vendor_boot … rc=0`。

### 56.2 构建记录

| 阶段 | 产物 | SHA-256 | 日志 |
| --- | --- | --- | --- |
| kernel | `out/muyu/kernel/kernel.macho` | `fb796ca1cf1bd25f62d6957da49183ad4871680c5d3eda60727e7bb624b16dc2` | `logs/build-kernel-bsd1.log`，EXIT=0，err/warn 0 |
| payload | `out/muyu/payload/payload.img`（72 MiB） | `160cf3c92510e5484ffe527b7a98c4a4a892d65d7b828ac0e41a7c5b240a2f47` | `logs/build-payload-bsd1.log`，EXIT=0 |
| vendor_boot | `fw/vendor_boot_gpu_bsd1.img` | `d029545c…c3b964` | `logs/build-vendor-boot-bsd1.log` |

插桩以**补丁文件**固化：`UniXNU-muyu-17/patches/xnu/zzzzzzzzzzzzzzzzzzzzz-bsd-stage-probe.patch`（sha256
`7eedc00e…7381ab`），内容为 `bsd_init.c` 8 个纯赋值 + 全局 `volatile int muyu_bsd_stage`；已在 `patch -p1`
下验证可干净应用到"其余补丁已应用、本补丁未应用"的状态。ticker 侧只多一行 `MUYU BSD s=%d`（弱链接 + 判空）。

### 56.3 屏照 `logs/2026-11-12-19-37-56-454.jpg`

sha256 `e35629b787bdc85cb75cb053ab2565ed57b9e6d9bede3b208bae928cd9e93c1d`，3840×2160，需 **90° 逆时针**
（`transpose=2`；行框高瘦 w≈33–67、h≈268–2032 → 旋转后变宽扁 h≈33–68、w≈274–2026，左对齐 x≈597）。

单批结构（自下而上为打印顺序）：

```
MUYU UP   lg=10/1/1/1 s=1/0 op=0/0 mf=0/0 i=48 r=-
MUYU ROOT c=0/-1/-1 d=0/-1
MUYU INIT q=0
MUYU BSD  s=2
```

**两个不同批次都读到 `s=2`**（画面顶部是上一批尾部 `BSD/PROC/LIVE`，底部是当前批 `UP→ROOT→INIT→BSD`），
`q=0` 依旧，`PROC n=1` 依旧。

### 56.4 ⭐ `s=2` + 用户目视到的两行 IOLog ⇒ 断点在 `setconf() → IOFindBSDRoot()`

用户在屏上额外看到两句**不在 ticker 里**的 IOLog：

- `NativeUfs: no valid root manifest in userdata (status 1, manifest version 0, magic fd877808 ab9f4456)`
  （`drivers/ShengUfs/NativeUfs.cpp:2109`）
- `Still waiting for root device`（`iokit/bsddev/IOKitBSDInit.cpp:962`）

调用链已逐行核实：

1. `NativeUfs::startRoot()` 在 `layout->manifestStatus != 0` 时 `rootDiagnosticFailure` +
   `IOLog("NativeUfs: no valid root manifest…")` 然后 **`return false`**（`NativeUfs.cpp:2107–2114`），
   一个 `IOMedia` / 块设备都不发布。
2. `setconf()`（`bsd_init.c:1339`）调用 `IOFindBSDRoot()`（`:1344`），后者在
   `iokit/bsddev/IOKitBSDInit.cpp:957` 的 `do { … } while (!service)` 里等 `IOMedia`，
   等不到就打印 `Still waiting for root device` 并继续等——**没有退出路径**（本构建不落 iOS 的 panic 分支）。
3. `setconf()` 的调用点是 `bsd_init()` 里的 mount 循环 `bsd_init.c:1013`，**位置在 `vfs_mountroot()`（`:1020`）之前**。

⇒ 结论：**`vfs_mountroot()` 在这些启动里从未被调用过**。`bsd1` 的 `s=2`（赋在 `inittodr()` 之后、循环之前）
与这条链完全吻合，可以认为 `setconf()` 就是当前唯一的停点。

### 56.5 连带撤销（比 §55.4 更彻底）

§55.4 只说「没有观测证明 `vfs_mountroot()` 已返回」。现在有了更强的结论：

> **这些启动根本没有走到 `vfs_mountroot()`；root 设备从未出现。**

因此 `owner-cache` / `owner-data` / launchd / Metal plugin / Turnip / `-3` / `ROOT c=0/-1/-1` 理解成
「owner mount 卡住」的整条叙事全部作废——它们位于一个从未被执行的代码段里。`q=0`、`PROC n=1` 都只是
「init 永远没开始」的副产品，不是独立异常。

### 56.6 manifest 为什么会没了（待定，两条候选）

`RootLayout.h:32` 的状态码：`0` ok、`1` 无 magic、`2` 版本/条数、`3` 哈希、`4` 越界、`5` 名字、`6` 重叠、`7` 属性。
现值 `status=1` + `version=0` + 非零 magic ⇒ **userdata 首块不是 `SHNGROOT`，也不是全零**。

已知事实：

- 台账最后一次写 userdata 是 `2026-11-11T04:10:58Z root-image fw/userdata-backup/userdata-root.sparse rc=0`，
  **之后没有任何 userdata 写操作**（`awk` 过滤 `NR>2664` 已确认）。
- 该备份本身完好：sparse v1、4096 块、chunk0 为 RAW、首块 `SHNGROOT`、`version 1`、2 个窗口
  （`OS` off 1048576 size 6077580800；`SystemOS` off 6079643648 size 3303014400）、`imageBytes 9382658048`、
  校验和通过。
- `sheng-data-persist` 的写入窗口在 userdata **尾部**（`NativeUfs.cpp:1069`），不会碰首块 ⇒ 不是它。

⇒ 首块是被**别的东西**改写的。候选：① 回 rec 充电时 stock recovery 挂载/格式化 `/data`；
② 某次原厂恢复/抹除。这一条**没有证据**，下一步要么只读读出首块原始内容、要么查 recovery 日志。

### 56.7 下一步（未执行，等确认）

`root-image` 是整盘覆盖写（`tools/muyu-flash-mode.sh root-image … --yes`），约 9 GB，不做无根据的重写。
先做**只读**验证：进 stock recovery 用 `adb shell dd if=<userdata 块设备> bs=4096 count=1` 取首块，
看它到底是 ext4/f2fs 元数据、sparse 头还是别的；同时看 recovery 是否自己挂了 `/data`。
拿到首块原始内容后再决定是否重刷 root image。

## §57 2026-11-12 19:45–19:52：8 KiB manifest 修补生效 —— `BSD s=8` / `INIT q=6` / `ROOT c=6/0/0 d=4/0`，用户态起来了

### 57.1 修补动作（这是本工程第一次主动写 userdata，必记）

在 **stock recovery**（`fastboot boot fw/stock/boot.img`，此前已刷回 `fw/stock/vendor_boot.img`）中执行：

```bash
adb push /tmp/ud_fix.bin /tmp/ud_fix.bin          # 8192 B = 备份 chunk0 的第 0 块(manifest) + 4 KiB 零
adb shell 'sha256sum /tmp/ud_fix.bin'             # 与宿主一致
adb shell 'dd if=/tmp/ud_fix.bin of=/dev/block/sda32 bs=4096 count=2 conv=notrunc; sync'
adb exec-out 'dd if=/dev/block/sda32 bs=4096 count=2' > /tmp/ud_verify.bin   # 回读
```

- 目标：`userdata` = `/dev/block/sda32`（recovery 下 `by-name` 已确认；`/data` 当时**未挂载**，只挂了 `/metadata`）。
- 载荷来源：`fw/userdata-backup/userdata-root.sparse` 的 chunk0（RAW，1024 块），取首块为 manifest，**原样**写回，
  其后 4 KiB 写零（备份里该块本来就是零）。
- 回读校验通过：首 8 字节恢复为 `SHNGROOT`，`version=1`，2 个窗口（`OS` off 1048576 size 6077580800；
  `SystemOS` off 6079643648 size 3303014400），`imageBytes 9382658048`，块内 sha 自校验通过。
- 破坏范围（修补前实测）：仅 **块 0 / 块 1** 与备份不同，块 2–15 逐字节相同 ⇒ 实际损坏 8192 B，**不是整盘被抹**。
  修补前首块是 `fd877808 ab9f4456…`（与内核报的 magic 一致），即同一份数据两条路径都读到。
- 留证：损坏态首 64 KiB 存为 `logs/2026-11-12-userdata-head-clobbered.bin`。
- ⚠️ **台账口径变更**：此前「11-11 之后没有任何 userdata 写操作」的说法到此为止；本次是**定点 8 KiB**，
  不是 `root-image` 整盘覆盖（9 GB APFS 镜像内容未动，+1 GiB 处仍可读到 iOS 语言数据）。

### 57.2 刷入

`bash tools/muyu-flash-mode.sh status`：fastboot 在线、`slot=a unbootable=no`、**3890 mV**。
`MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_bsd1.img`
⇒ `Writing 'vendor_boot' OKAY`，UEFI RAM boot `OKAY`，台账 `11:47:40Z boot … rc=0`。

### 57.3 屏照 `logs/IMG_20261112_195112.jpg`

sha256 `378a95e058efff9b972fffdab86674db39543fa1b5ae61290f148fd2e2defda0`，3840×2160，**本身正立**
（行框宽扁 h≈28–72、w≈250–1000）。关键行 6× 放大复核。

**屏幕形态与之前几轮完全不同**：console 不再是寥寥 8 行，而是满屏约 50 行/批——因为守着活的 APFS
和用户态，IOLog 与深层诊断行都进了同一个 framebuffer console。单批打印顺序（`Z1…` 起、`F/Z0` 止）：

```
MUYU LIVE  p=12 t=182 btn st=3 irq=0 rep=0 msk=0x0 pon=476 gpio=-1 pdc=0x30200 err=0 sp=0 oth=0
MUYU GPU   calls=7 refused=7 o=0 c=0 f=0 fb=0 tc=6 ts=00000000 hold=1 pres=1 | w=0
MUYU PROBE st=97 cp=10 b=24
MUYU B     i=48 o=40 f=0 s=0 b=0 n=0
MUYU H     h1 r1 …                                    (OCR 不稳，未采信)
MUYU UP    lg=10/1/1/1 s=1/0 op=5/4 mf=0/0 i=48
MUYU ROOT  c=6/0/0 d=4/0
MUYU INIT  q=6
MUYU BSD   s=8
MUYU R     PUB no device: Vulkan(VkResultCode(-3))
tx_flush:1338: disk1 xid 6041 tx stats: # 120 owait 17 4727us finish 123 …
```

批次边界说明：`BSD` 之后紧跟的是**下一批**的 `LIVE`（`LIVE` 是每批第一行），所以这一批的 census 落在
`BSD` 与下一批 `LIVE` 之间；本张照片那一批是 `p=11`（11%3≠0）⇒ **本张无 `PROC` 行**，不是 census 失效。

### 57.4 ⭐ 判读：整条启动链通了

| 读数 | 含义 |
| --- | --- |
| `BSD s=8` | `load_init_program()` **已返回**（8 是我在 `load_init_program(p);` 之后赋的值） |
| `INIT q=6` | `native_owner_cache_prepare()` 已返回 |
| `ROOT c=6/0/0` | cache stage 6、cache status **0（成功）**、cache mount status **0（成功）** |
| `ROOT d=4/0` | data stage 4（`if (!error) stage=4`）、data status **0（成功）** ⇒ Data 段也挂上了 |
| `tx_flush:1338: disk1 xid 6041` | APFS 正在提交事务 ⇒ root 卷是活的，不是只读挂上 |
| `GPU calls=7 refused=7` | 有**用户态客户端**连续 7 次请求把帧交给 GPU，7 次全被拒 |
| `R PUB no device: Vulkan(VkResultCode(-3))` | Metal bridge 自己记录的最后一句：初始化失败（`-3` = `VK_ERROR_INITIALIZATION_FAILED`） |

⇒ **`s=8` 意味着 `bsd_init()` 走完了整个阶梯**，§56 定位的 `setconf() → IOFindBSDRoot()` 死等**已解除**：
manifest 修好后 `NativeUfs::startRoot()` 发布了 root 媒体，`vfs_mountroot()` 返回 0，`s` 从 2 一路到 8。

⇒ **§54–§56 的整条叙事全部结案**：`q=0`、`PROC n=1`、`-3`、owner-mount、launchd 都不是独立故障，
根因就是 userdata 首块那 8 KiB。修好后 `q` 从 0 → 6、`ROOT` 从 `0/-1/-1 d=0/-1` → `6/0/0 d=4/0`。

### 57.5 新的 blocker：GPU 服务拒绝连接（不是 root、不是 init）

`calls=7 refused=7`、`o=0 c=0 f=0`（offers/completions/failures 全零）⇒ 显示路径仍在 CPU 拷贝，
`native_legacy_gpu_offer_*` 把每一次请求都拒了。同时 `muyu_gpu_reason` 记的是
`PUB no device: Vulkan(VkResultCode(-3))`——这是 Metal bridge 流式输出的最后一行（`GpuPowerProbe.cpp:1228`）。

注意区分两条线，**不要混**：

- `MUYU B i=48 o=40 f=0 s=0 b=0 n=0`：`native_adreno_mount_fail[0]` 是 **0**，即 publish/mount 阶段
  **没有**记录失败（不是「没有地址空间 / 没有 VM / 窗口被拒」那三种）。
- `-3` 来自 **客户端侧** Vulkan 初始化，不是内核侧窗口分配失败。

所以下一轮要查的是 `native_bridge_metal_open` 在拿到已发布服务之后，为什么 Vulkan 初始化返回 -3。
`PROBE st=97 cp=10` 与之前完全一致（CP 停在 C10，即那个 `publishMetal()` 的 gate 之前），`tc=6`（上一轮 0）、
`ts=ffffffff` → `00000000` 也是这一轮新变的。

### 57.6 下一轮判读格（`bsd2`/`vk1`，未构建）

这一轮**只**追 `-3` 一件事，判读格先写定：

| 观测 | 结论 | 下一步 |
| --- | --- | --- |
| `R` 行变成 `PUB ... ok`，`calls/refused` 里 refused 不再增长 | 客户端初始化成功 | 看 `o=`（offers）是否 >0，即真的把帧交给 GPU |
| `-3` 不变，`B` 行 `f` 出现 1/2/3 | 内核侧窗口/地址空间问题浮出 | 按 `mount_fail[0]` 的 1/2/3 分支 |
| `-3` 不变，`B` 行仍全零 | 故障在客户端初始化序列（驱动枚举/版本握手），不在内核分配 | 读 `native_bridge_metal_open` 的 -3 返回点 |
| `refused` 继续涨而 `calls` 不涨 | 显示侧先弃权 | 查 `LegacyDisplayProbe` 的 offer 拒绝条件 |

**证伪条件**：若改为 `calls>0 refused=0` 但 `o=0`，则说明请求被接受却没真正提交 ⇒ 问题在 presenter 提交侧，
不在连接建立。

### 57.7 当前状态

设备在跑 `bsd1`（`braveark-hold-reboot=1` 不自动重启），拍屏时 `t=182`。userdata 首块已修好，
`vendor_boot` 为 `bsd1`。设备要回 rec 充电仍须先 `stock`（§2.1）。

## §58 2026-11-12 19:58–20:07：`vkd1` —— 把驱动**早就记着、从没上屏**的客户端事实打出来

### 58.1 为什么是这一步（而不是重建 bundle）

`R` 行只给 `PUB no device: Vulkan(VkResultCode(-3))`（32 字符，`%.40s` 从未截断，§46 已更正）。要拿到更多上下文，
历史上只有两条路，都会同时改 bundle 签名 / CDHash / trust cache / addon **四个输入**（§46.6 C）。

但 `drivers/Adreno740/NativeAdrenoPublish.cpp` 里**已经**在记录客户端行为，只是从来没有打印过：

| 变量 | 内容 |
| --- | --- |
| `native_adreno_metal_diag.calls/last_selector/last_status` | 最后一次 external method **及其 IOReturn** |
| `…opens/bad/closes/attach_status/inflight/grants` | 打开数、`initWithTask` 失败数、关闭数、attach 结果 |
| `…opener[3][17]` | 最近三个连接的**进程名** |
| `native_adreno_open_log[8]` + `count` | 每次 `IOServiceOpen`：进程名、open type、两道 sandbox 判定、结果、attach、foreign |
| `native_adreno_trace_stage` | 驱动内部追踪阶段 |

这些足以回答一个二选一问题：**`-3` 是内核在某个 method 上拒绝的，还是内核全部放行、turnip 自己失败的？**
如果是前者，`last_status` 会非零且能定位到具体方法——那就找到了一条不用重建 bundle 的修法。

⇒ 本版**只加三条打印**，不碰任何行为、不改 addon / trust / bundle / 内核逻辑。

### 58.2 新增三行（`BravearkLastLog.c`，紧接 `MUYU B` 之后）

```
MUYU V op=<opens> bad=<initWithTask 失败> cl=<closes> call=<总调用数> sel=<最后方法号> st=<该方法 IOReturn> at=<attach> inf=<inflight> mb=<maps>/<calls> tr=<trace_stage> gr=<grants>
MUYU O <opener0>|<opener1>|<opener2>
MUYU A n=<总打开次数> <最新一条 open 记录> <次新一条>
```

镜像侧用一个与 `struct NativeAdrenoMetalDiag` 逐字段同序的镜像结构读取（成员全为 `uint32_t` / `char[]`，
无填充），并在 `nm -gU` 下确认 7 个符号全部导出：

```
_native_adreno_metal_diag  _native_adreno_open_log  _native_adreno_open_log_count
_native_adreno_trace_stage _native_adreno_view_diag _native_adreno_metal_rows  _native_adreno_metal_rows_seq
```

**`sel` 取值表**（`NativeAdrenoABI.h`）：
`1` Query、`2` BoAlloc、`3` BoMap、`4` BoFree、`5` Submit、`6` WaitFence、`7` GetTimestamp、`8` GetStatus、
`9` SetMetadata、`10` GetMetadata、`11` CreateQueue、`12` CloseQueue、`13` BoCpuMap、`14` BoCpuUnmap、
`15` GetDeviceInfo、`16` BoImport、`17` SetPriority、`18` Note。

**`st` 取值**（`iokit/IOKit/IOReturn.h`，`iokit_common_err` = `0xe0000000 |`）：
`0` 成功；`e00002bc` Error；`-02bd` NoMemory；`-02be` NoResources；`-02c2` BadArgument；`-02c5` ExclusiveAccess；
`-02c7` Unsupported；`-02cd` NotOpen；`-02d7` Offline；`-02d9` NotAttached；`-02e2` NotPermitted；`-02e6` UnsupportedMode。

**`at` 取值**（`NativeAdrenoPublish.cpp` 的 `attach_state`）：`0` 没有 provider、`1` attach+start 成功、
`2` attach 失败、`3` start 失败。

**`A` 行的 `t`**：`4147`（`'AG'`）= Adreno 插件的 open type；`1` = 原版 IOGPU 库那条路。

### 58.3 构建记录

| 阶段 | 产物 | SHA-256 | 日志 |
| --- | --- | --- | --- |
| kernel | `out/muyu/kernel/kernel.macho` | `97d11ef177a2a11184bcc3a584076bf3e3b1b18d3233d3665ffcce97a89f3050` | `logs/build-kernel-vkd1.log`，EXIT=0，err/warn 0 |
| payload | `out/muyu/payload/payload.img`（72 MiB） | `47aa8850230909d67393d780c4f152d06ae4c7622c0c73c11703f45d39101fff` | `logs/build-payload-vkd1.log`，EXIT=0 |
| vendor_boot | `fw/vendor_boot_gpu_vkd1.img`（77,393,920 B） | `9154be66535cd66393e1e0477d611697a402999707db64c987431c4976e2dd9a` | `logs/build-vendor-boot-vkd1.log` |

`strings` 确认镜像同时含 `MUYU V op=…` / `MUYU O %s|%s|%s` / `MUYU A n=%u`，以及原来的
`BSD s=` / `INIT q=` / `ROOT c=…`。源码备份 `/tmp/vkd1-backup/BravearkLastLog.c`；相对 `bsd1` 仅 +46 行。
刷前 `status`：`slot=a unbootable=no`、**3792 mV**（比 19:47 的 3890 又掉了 ~100 mV，仍高于 3700 门槛）。
`Writing 'vendor_boot' OKAY [0.037s]`，UEFI RAM boot `OKAY`。

### 58.4 判读格（刷前写定）

| `MUYU V` 观测 | 结论 | 下一步 |
| --- | --- | --- |
| `st=00000000`，`sel` 是任一正常方法号 | **内核全部放行**，`-3` 出在 turnip / metal2vulkan 内部（特性协商、内存属性、queue 创建参数） | 放弃「内核拒绝」假说；去读 turnip 的 `tu_device_create` 与 `NativeMetalObjects.rs` 的 options |
| `st≠0` | **内核在某方法上拒绝**，`sel` 指名道姓 | 读该方法的实现，看它在什么条件下返回这个 IOReturn；这类修法可能只改驱动 |
| `at=2` 或 `at=3` | 连接建了但 attach/start 没过 | 查 provider 链与 `start()`，与 turnip 无关 |
| `bad>0` | `initWithTask` 失败（资源/权限） | 与 sandbox / 地址空间相关 |
| `op=0` | 没有客户端连上 | 回到 launchd / 客户端加载，而不是 GPU |
| `O` 行的名字不是 `backboardd` | 来连的是别的进程（探针 / helper） | `-3` 可能只是那个进程的现象，不代表渲染栈 |

**证伪条件**：若 `st=0` 且 `O` 行显示 `backboardd`，则「GPU 服务拒绝客户端」这个**措辞**也不准确——
内核没拒，是 turnip 自己没起来；`calls=7 refused=7` 只是显示侧 offer 被上层放弃，不是内核拒绝。

### 58.5 一处措辞更正

§57.5 写的「GPU 服务拒绝连接」应读作「显示侧 7 次 offer 全部没成功」。`native_adreno_publish_stats` 的
`opened/closed`（`UP` 行 `op=5/4`）说明**服务端接受过 5 次连接**，所以「拒绝」这个词只对 offer 成立。
`vkd1` 的 `V`/`A` 行正是要把这个区别钉死。

---

## §59 2026-11-12 20:09：`vkd1` 屏照 —— 内核**全部放行**，`-3` 落在客户端 Vulkan 栈；`vkd2` 读标记环

### 59.1 逐字转录（`logs/IMG_20261112_200856.jpg`，3840×2160，20:09，同一屏，方向正常）

```
MUYU INIT q=6
MUYU BSD s=8
MUYU LIVE p=11 t=686 btn st=3 lrq=0 rep=0 msk=0x0 pon=476 gplo=-1 pdc=0x30200 err=0 sp=0 oth=0
MUYU GPU calls=8 refused=8 o=0 c=0 f=0 fb=0 tc=8 ts=00000000 hold=1 pres=1 | w=0
MUYU PROBE st=97 cp=10 b=24
MUYU B i48 o40 f0 s0 b0 n0
MUYU V op=5 bad=0 cl=4 call=105 sel=6 st=00000000 at=1 inf=0 mb=0/0 tr=14 gr=5
MUYU O SpringBoard|SpringBoard|backboardd
MUYU A n=6 SpringBoard/t4147/m00000000/re00002c7/a1/f0 SpringBoard/t4147/m00000000/…
MUYU R PUB no device: Vulkan(VkResultCode(-3))
（其余为 §54 起就有的 Z0–Z5 / T / W / D / S 段，本轮不判读）
MUYU UP lp=18/1/1 a=1/0 cp=5/4 af=0/0 l=0 r=0
MUYU ROOT c=6/0/0 d=4/0
```

### 59.2 判读（判读格 §58.4 第一行命中）

| 读数 | 含义（代码定位） |
| --- | --- |
| `sel=6` `st=00000000` | 最后一道 external method 是 `WaitFence`，返回 `kIOReturnSuccess`（`NativeAdrenoPublish.cpp:910` 记 `last_selector/last_status`）⇒ **内核没有拒绝任何调用** |
| `at=1` | `attach()`+`start()` 都成功（同文件 `attach_state`）⇒ provider 链无问题 |
| `bad=0` `op=5` `cl=4` | 5 个连接被接受，`initWithTask` 零失败；6 次 `IOServiceOpen` 里 4 次已关闭 |
| `gr=5` | 5 次走了 `NativeSandboxGrant` 旁路；`A` 行 `re00002c7` = `kIOReturnUnsupported`，是 profile 里没有 AGX 类时的**预期**沙箱判决，旁路正是为它准备的 |
| `tr=14` | 最后一次 SMMU unmap 走完（`NativeAdrenoVm.cpp:373–407` 的 10→14 梯）⇒ 映射/解除映射这条链是通的 |
| `mb=0/0` | 没有 map 失败 |
| `O` 行 | `SpringBoard|SpringBoard|backboardd`；`A` 行 type=`0x4147`（AG 插件）、`foreign=0` ⇒ 来连的是**真客户端**，不是探针 |

**结论（证据等级：屏照逐字转录，§58.4 判读格第 1 行）**：「GPU 服务拒绝客户端」**证伪**。内核全放行，
`-3`（`VK_ERROR_INITIALIZATION_FAILED`）是客户端 Vulkan 栈自己给出的。`calls=8 refused=8` 是 GOP 帧 offer
桥接计数（`LegacyDisplayProbe.cpp`，§57.5 已说明），与 `vkCreateDevice` 无关，不要再用它描述 GPU 状态。

### 59.3 但 `-3` 落在客户端栈的哪一层，这一屏还分不出来

客户端是两层：`metal2vulkan`（`bridge/`，`nmb_*` Rust ABI）→ Turnip（`libvulkan_freedreno.dylib`）。
`PUB no device: Vulkan(VkResultCode(-3))` 由 `NativeAdrenoMetalPlugin.mm:404` 在 `native_bridge_metal_open()`
返回 nil 之后拼接，**只带 VkResult，不带层**。

插件里其实已经有更细的通道，只是被覆盖了：`NativeAdrenoUserTest.h:turnipPhase` 会让 Turnip 单独跑
instance→physical→device→queue→fill→fence→校验，并把 `T s<stage> r<VkResult> v.. c.. g.. <首条 Vulkan 错误>`
写进 `report2`；同时用 `mark()` 打点 `6000`、`6001..600b`、`6fSS`+`6eRR`（RR = -VkResult）、`6fff`/`6ffe`。
之后 bridge 那句 `PUB no device` 又写进同一个 `report2`，屏上就只剩 `-3`。**但 mark 是 16 槽环形数组，
存在内核里，没被覆盖掉。**

### 59.4 `vkd2`：只读打印标记环（`BravearkLastLog.c` +1 行，不动任何行为）

紧接 `MUYU O` 之后新增一行：

```
MUYU K n=<marks_total> <最新 12 个 mark，各 4 位十六进制，新→旧>
```

判读格（刷前写定）：

| `K` 行观测 | 结论 | 下一步 |
| --- | --- | --- |
| 含 `6fff` | Turnip 阶段**全过**（device/queue/fill/compute/render 都成功）⇒ `-3` 是 metal2vulkan 层给的 | 读 `bridge/NativeMetalObjects.rs` 的 device 选项与 `bridge/patches/` 里的特性协商 |
| 含 `6ffe`，且前面有 `6fSS` + `6eRR` | **Turnip 自己**在 stage SS 失败，RR = -VkResult | `SS=5` 就是 `vkCreateDevice`，直接查 `tu_device_create` |
| 有 `6000..600b` 但无 `6fff/6ffe` | 阶段跑到一半就断（进程崩/被杀） | 看 `MUYU PROC` 与 crash 行 |
| 没有 `6xxx`，只有 `68f2`/`6Cnn` | Turnip 阶段根本没跑（前置 fill 测试没过） | 回到 `adrenoUserTest` 的 `verified<4` |

`68f2` = bridge 拿不到 device 的 mark（`NativeAdrenoMetalPlugin.mm:400`）；`6Cnn` = m2vpack 导入。
最新 12 个覆盖得下「Turnip 阶段尾 + bridge 失败 mark」。

### 59.5 构建记录（`vkd2`）

| 阶段 | 产物 | SHA-256 | 日志 |
| --- | --- | --- | --- |
| kernel | `out/muyu/kernel/kernel.macho` | 见日志 | `logs/build-kernel-vkd2.log`，EXIT=0，error 0 |
| payload | `out/muyu/payload/payload.img`（72 MiB） | — | `logs/build-payload-vkd2.log`，EXIT=0 |
| vendor_boot | `fw/vendor_boot_gpu_vkd2.img`（77,393,920 B） | `649546a2c082273c672065185b60cfa4c5b8193d6a62f172316de6cbdc626669` | `logs/build-vendor-boot-vkd2.log` |

`strings` 已确认镜像含 `MUYU K n=%u`，且 `V`/`O`/`A` 三条旧行仍在。相对 `vkd1` 只多这一行打印。
刷入命令（设备回 fastboot 后）：

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_vkd2.img
```

---

## §60 2026-11-12 20:38：`vkd2` 屏照 —— `K` 行只有 4 个 mark，**publish 模式根本不跑 Turnip 冒烟**；`vkd3` 转查选择器准入

### 60.1 逐字转录（`logs/2026-11-12-20-38-52-184.jpg`，3840×2160，20:38）

```
MUYU ROOT c=6/0/0 d=4/0
MUYU INIT q=6
MUYU BSD s=8
MUYU LIVE p=11 t=234 btn st=3 lrq=0 rep=0 msk=0x0 pon=476 gplo=-1 pdc=0x30200 err=0 sp=0 oth=0
MUYU GPU calls=7 refused=7 o=0 c=0 f=0 fb=0 tc=7 ts=00000000 hold=1 pres=1 | w=0
MUYU PROBE st=97 cp=10 b=24
MUYU B i48 o40 f0 s0 b0 n0
MUYU V op=5 bad=0 cl=4 call=93 sel=6 st=00000000 at=1 inf=0 mb=0/0 tr=14 gr=5
MUYU O SpringBoard|SpringBoard|backboardd
MUYU K n=4 68f2 6a3c 6801 6800 0000 0000 0000 0000 0000 0000 0000 0000
MUYU A n=6 SpringBoard/t4147/m00000000/re00002c7/a1/f0 SpringBoard/t4147/m00000000/re00002c7/a1/f0
MUYU R PUB no device: Vulkan(VkResultCode(-3))
```

### 60.2 `K` 行判读：**§59.4 的 `6xxx` 分支不可达**（判读格自我更正）

`marks_total = 4`，即本次开机全部 mark 只有四个，按时间顺序：`6800` → `6801` → `6a3c` → `68f2`。

| mark | 出处 | 含义 |
| --- | --- | --- |
| `0x6800` | `NativeAdrenoMetalPlugin.mm:265` | 进入 publish 路径的 `deviceForPort` |
| `0x6801` | `:268` | Turnip 库存在且可读 |
| `0x6a3c` | `:295` | 帧缓冲诊断位：`4`=MG 深度答 true、`8`=键存在、`16`=压缩能力字典存在、`32`=非 root（`bit0/1`=0，即没有 `CAEnableDeepFramebuffer` 覆盖） |
| `0x68f2` | `:402` | `native_bridge_metal_open()` 返回 nil ⇒ 同处发出 `PUB no device: Vulkan(-3)` |

**关键更正**：`NativeAdrenoMetalPlugin.mm:480` 是 `if (publish::requested()) return publish::deviceForPort(port);`，
它**直接短路**掉了后面的发现路径，而 Turnip 冒烟（`adrenoUserTest` → `turnipPhase`，会打 `6000/6001..600b/6fff/6ffe`）
只在发现路径里跑。所以本线（publish 模式）**永远不会出现 `6xxx`**，§59.4 判读格里那三条分支是空转的。
这条排除本身有价值：它证明"跑一遍 Turnip 冒烟"不是本线可用的读法，得换通道。

### 60.3 `-3` 的静态定位（证据等级：代码静态推导）

`darwin_device_init()`（`tu_knl_darwin.cc:1531`）里唯一的 `-3` 是 `pthread_mutex_init` 失败（不可能），
其余全部来自 `dw_open_connection()`（`:176`）。它只有三个 `-3` 出口：

1. `IOServiceOpen(%s, 0x%x) failed: 0x%x`
2. `no external-method selector is admitted for this process`
3. `NativeAdreno query failed: 0x%x`

`V` 行 `op=5 cl=4`（5 次授予、4 次关闭）与"每轮 open→失败→close"的形状吻合；`(1)` 可排除（open 是成功的）。
剩下 `(2)` 与 `(3)`，二者都与**选择器准入**直接相关：`dw_probe_selector()`（`:161`）要先在 0..255 里
命中一个沙箱放行的 selector，隧道（`DW_TUNNEL_MAGIC`）才成立；驱动侧对任何到达的探针都回 success 并计 `probes`
（`NativeAdrenoPublish.cpp:877`）。所以 **`probes = 0` 就等于 `(2)`**。

### 60.4 `vkd3`：两行新数据（`BravearkLastLog.c`）

```
MUYU L pr=<probes> tn=<tunnel_calls> ex=<ext_entries> es=<ext_selector> st=<ext_status> sb=<sel_bits[0]> <sel_bits[1]>
MUYU C n=<calls> <6 条 方法/状态/pid，新→旧>      （调用环，32 B/条，掩码 262143）
```

判读格（刷前写定）：

| 观测 | 结论 | 下一步 |
| --- | --- | --- |
| `pr=0` | 一个 selector 都没放行 ⇒ `-3` = `(2)` | 修沙箱准入（profile / grant 补丁管到 selector，而不只是 open），不必碰 Turnip |
| `pr>0` 且 `sb=00000000 00000000` | 探针到了但 `sel_bits` 全零，说明探针走的是不记账的早期分支 | 先看 `C` 行有没有 backboardd 的 pid |
| `pr>0`、`sb≠0`、`tn>0` | 隧道通了，`-3` 是 `(3)`（Query/ABI）或更后面 | 用 `C` 行里失败的那条 method/status 定位 |
| `C` 行没有 backboardd 的 pid | backboardd 连一条方法都没送到驱动 | 回到 `(2)`，即使别的进程能用 |
| `C` 行有 backboardd 且最后一条 `st=0` | backboardd 的调用都成功，`-3` 出在 turnip 用户态逻辑 | 需要 turnip 自己的日志（成本高，另议） |

`Z2` 的 CP 环 dump 从 4 行减到 2 行（`b=0` 与 `b=24`，后者正好是 CP 读指针所在），给上面两行腾位置。

### 60.5 构建记录（`vkd3`）

| 阶段 | 产物 | SHA-256 | 日志 |
| --- | --- | --- | --- |
| kernel | `out/muyu/kernel/kernel.macho` | — | `logs/build-kernel-vkd3.log`，EXIT=0 |
| payload | `out/muyu/payload/payload.img`（72 MiB） | — | `logs/build-payload-vkd3.log`，EXIT=0 |
| vendor_boot | `fw/vendor_boot_gpu_vkd3.img`（77,393,920 B） | `44c8f009505055e7fcaa543f68fde19e76b445d101d090631cdea543698f7c5f` | `logs/build-vendor-boot-vkd3.log` |

`strings` 已确认两条新格式串都在镜像里。

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_vkd3.img
```

---

## §61 2026-11-12 20:48：`vkd3` 屏照 —— ⭐ **隧道是通的**（`pr=2 tn=96` 全成功），`-3` 不在连接建立阶段；`C` 行是死通道（自我更正）；`vkd4` 记录方法序列

### 61.1 逐字转录（`logs/2026-11-12-20-48-01-820.jpg`，3840×2160，20:48）

```
MUYU PROBE st=97 cp=10 b=24
MUYU B i48 o40 f0 s0 b0 n0
MUYU V op=5 bad=0 cl=4 call=95 sel=6 st=00000000 at=1 inf=0 mb=0/0 tr=14 gr=5
MUYU O SpringBoard|SpringBoard|backboardd
MUYU K n=4 68f2 6a3c 6801 6800 0000 …
MUYU L pr=2 tn=96 ex=98 es=0 st=00000000 sb=00000001 00000000
MUYU A n=6 SpringBoard/t4147/m00000000/re00002c7/a1/f0  (×2)
MUYU R PUB no device: Vulkan(VkResultCode(-3))
MUYU INIT q=6   MUYU ROOT c=6/0/0 d=4/0   MUYU UP lp=18/1/1 …
```

### 61.2 `L` 行判读：命中 §60.4 第 3 行 —— **隧道成立，且 96 次调用全部成功**

| 读数 | 含义（`NativeAdrenoPublish.cpp:880–906`） |
| --- | --- |
| `ex=98` | `externalMethod()` 被调用 98 次 |
| `pr=2` | 2 次探针（`{magic, 0xffffffff}`）都返回 success ⇒ **`dw_probe_selector()` 成功**，不存在 "no external-method selector is admitted" |
| `tn=96` | 96 次**真正的**隧道调用（98 = 2 探针 + 96） |
| `sb=00000001 00000000` | 只有 selector **0** 被沙箱放行；两次探针都命中 0（bit0 早已置位，故只有 1 位） |
| `st=00000000` | `ext_status` 记录**最后一次**隧道调用（`:896`）的 IOReturn ⇒ 最后一次是 success |
| `op=5 cl=4` | 5 次 open、4 次 close ⇒ 最后一个连接仍开着 |

**结论（证据等级：屏照逐字转录 + 代码定位）**：`-3` **不是**连接建立失败。`dw_open_connection()` 的三个 `-3` 出口里，
「selector 不放行」被 `pr=2` 直接证伪，`IOServiceOpen` 早被 `op=5` 证伪。而且 96 次真实 ABI 调用说明 Turnip
**已经越过** query / get-device-info / set-priority，进到了要 BO 和队列的阶段 —— 也就是 `vkCreateDevice` 内部。

### 61.3 `C` 行为什么整屏没有：**它读的是死指针**（自我更正）

`vkd3` 的 `C` 行读 `native_adreno_call_ring`。全仓只有两处出现这个名字：
`NativeAdrenoPublish.cpp:90` 的声明、`:724` 的 `if(ring=...)` 判空 —— **没有任何地方给它赋值**。
在 16 线它是别处注册的，17 线从来没接过。所以 `C` 行永远不会打印。
与 §60.2 的 `K` 行同一种错误：**通道存在 ≠ 通道会填**。

### 61.4 `vkd4`：在真正被读的那个结构里记调用序列

驱动侧（`NativeAdrenoPublish.cpp`，均为纯记录，不改行为）：

- 结构尾部追加 `abi` / `call_seq` / `call_method[12]` / `call_status[12]` / `call_pid[12]` /
  `first_fail_{method,status,pid,at}`；
- 每次隧道调用**先写记录再执行**（没返回的调用也能看到它是什么），返回后回填 status；
- 第一次非 success 的隧道调用单独留一份，用 `@N` 记它是第几次调用。

屏上新增两行（紧跟 `L`）：

```
MUYU C n=<隧道调用数> ff=<方法>/<IOReturn>/<pid>@<第几次> abi=<驱动ABI版本> seq=<记录数>
MUYU M <方法>/<状态> ×10（新→旧）
```

方法编号（`NativeAdrenoABI.h`）：1 query、2 bo-alloc、3 bo-map、5 submit、6 wait-fence、11 create-queue、
13 bo-cpu-map、15 get-device-info、16 bo-import、17 set-priority。

判读格（刷前写定）：

| 观测 | 结论 | 下一步 |
| --- | --- | --- |
| `ff=0/0/0@0`，`M` 尾部是 1 / 15 / 17（query、device-info、priority） | 连接刚建完就没了 ⇒ Turnip 在拿到 query 结果后自己判失败 | 查 ABI 版本比较（`tu_knl_darwin.cc:208`）；`abi=` 若与 Turnip 期望不符即定案 |
| `ff=0/0/0@0`，`M` 尾部出现 2 / 11 / 13（BO、队列、CPU 映射） | 一路成功到设备创建中段 ⇒ 失败在 Turnip 自己的设备初始化（`tu_device.cc`） | 按尾部方法在 `tu_device_create` 里定位 |
| `ff=<m>/<非0>@N` | 内核在第 N 次调用拒绝了方法 m | 直接查该方法；`m=5`(submit) 与 `m=11`(create-queue) 的处理路径完全不同 |
| `abi=` 不是 2 | 驱动与 Turnip 的 ABI 版本不一致 | 重建 addon 里的 Turnip（`-Dtu-darwin-abi-dir`） |
| `n` 比 96 大很多 | 客户端在重试 | 先看 `ff@`，再考虑重试循环 |

### 61.5 构建记录（`vkd4`）

| 阶段 | 产物 | SHA-256 | 日志 |
| --- | --- | --- | --- |
| kernel | `out/muyu/kernel/kernel.macho` | — | `logs/build-kernel-vkd4.log`，EXIT=0，error 0 |
| payload | `out/muyu/payload/payload.img`（72 MiB） | — | `logs/build-payload-vkd4.log`，EXIT=0 |
| vendor_boot | `fw/vendor_boot_gpu_vkd4.img`（77,393,920 B） | `1438c02d5fed5a88271aef041306d62a90023f5ba2444b3057203b2e349ca4a4` | `logs/build-vendor-boot-vkd4.log` |

`strings` 已确认镜像含 `MUYU C n=%u ff=%u/%x/%d@%u abi=%u seq=%u` 与 `MUYU M`。

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_vkd4.img
```

---

## §62 2026-11-12 21:01：`vkd4` 屏照 —— **首次失败的调用是 WaitFence，返回 `kIOReturnTimeout`**；`vkd5` 读每方法计数与环形位置

### 62.1 逐字转录（`logs/IMG_20261112_210124.jpg`，3840×2160，21:01）

```
MUYU V op=3 bad=0 cl=4 call=99 sel=6 st=00000000 at=1 inf=0 mb=0/0 tr=14 gr=5
MUYU O SpringBoard|SpringBoard|backboardd
MUYU K n=4 68f2 6a3c 6801 6800 0000 0000 0000 0000 0000 0000 0000 0000
MUYU L pr=2 tn=99 ex=101 es=0 st=00000000 sb=00000001 00000000
MUYU C n=99 ff=6/e00002d6/2602@6 abi=2 seq=99
MUYU M 6/0 6/0 6/0 6/0 6/0 6/0 6/0 6/0 6/0 6/0
MUYU A n=6 SpringBoard/t4147/m00000000/re00002c7/a1/f0  (×2)
MUYU R PUB no device: device: Vulkan(VkResultCode(-3))
MUYU INIT q=6   MUYU ROOT c=6/0/0 d=4/0   MUYU BSD s=8
```

（`C` 行照片上 `@` 与后一位数字黏在一起，逐像素放大后读作 `2602@6`；`abi=2` 与驱动头一致，
所以 §61.4 里「ABI 版本不匹配」那一支可以划掉。）

### 62.2 判读

| 读数 | 含义 |
| --- | --- |
| `abi=2` | 驱动回给 Turnip 的 `NativeAdrenoAbiVersion` 是 2，与 `NativeAdrenoABI.h` 一致 ⇒ **不是版本不匹配** |
| `ff=6/e00002d6@6` | **第一次非 success 的隧道调用是方法 6 = `WaitFence`，返回 `kIOReturnTimeout`**（`IOReturn.h:169` = `0xe00002d6`），发生在第 6 次调用 |
| `n=99` `seq=99` | 整机本次开机共 99 次隧道调用，与 `L` 行 `tn=99` 一致（无丢记录） |
| `M` = 10 × `6/0` | **最后 10 次调用全是 `WaitFence` 且全部 success** |
| `V op=3 cl=4` | 本次 3 次 open、4 次 close（比上一轮的 5 次少，客户端重试次数在变） |
| `st=00000000` | 最后一次隧道调用（也是 WaitFence）成功 |

**这一轮拿到的事实（证据等级：屏照逐字转录 + 代码定位）**：
1. `-3` 之前的最后一个失败点是 **WaitFence 超时**，不是 submit 被拒、不是 BO 分配失败；
2. 但**最后一次** WaitFence 是成功的 ⇒ 栅栏在某个时刻确实退位过，`-3` 不是「永远等不到」；
3. 第 6 次调用就等栅栏，说明 Turnip 在设备创建早期就发了一次「提交并等完成」的探针（前 5 次应为
   query(1) / get-device-info(15) / set-priority(17) 与两次 BO）。

由此产生两个互斥假说，`vkd5` 一次分开：
- **A：栅栏要很久才退位**（首轮轮询的超时太短/CP 起步慢）⇒ 之后 9 次等待都立即成功；
- **B：那一次等的是「已提交但 CP 没跑到」的栅栏**（环形指针卡住），后续成功的等待是等别的、更早的栅栏。

### 62.3 用户侧新观察：`share_region … vm shared region start address() failed`

启动过程中出现过一行 `share_region:0x1841991a… vm shared region start address() failed`。

- 该串**不在本仓任何源码里**（`grep -r "share_region"` / `"shared region start address"` 在 `UniXNU`、
  `UniXNU-muyu-17`、`Mu-Silicium` 全部为空）⇒ 是**系统自带组件**（IOGPU/AGX 的 shared region 映射路径）打的，
  不是我们的驱动、也不是 ticker。
- 它描述的是**共享区**（GPU 大窗口）映射失败，而我们这条 ABI 通道走的是 external method + SMMU 表，
  两者不是同一条路；本轮不并入假说，只记录。若 `vkd5` 显示「栅栏提交了但 CP 不退位」，这条才升级为主要线索
  （CP 不退位的一个典型原因是它要访问的内存没映射上）。

### 62.4 `vkd5`：每方法计数 + 栅栏等待的环形位置

驱动侧（`NativeAdrenoPublish.cpp`，纯记录）：

- 结构尾部再追加 `wf_fence / wf_submitted / wf_retired / wf_waited_us / wf_timeout_us / wf_status / wf_count`，
  在 `WaitFence` 分支结束处写入，环形数字用 `native_adreno_fences()` 读；
- 每方法计数**不需要新造**：`NativeAdrenoUserClient.cpp:547` 早就在维护
  `native_adreno_method_stats[20][4]`（0 次数、1 失败数、2 最后状态、3 累计 us），只是从没被打印过。

屏上新增两行（紧跟 `M`）：

```
MUYU S <m>:<次数>[/<失败数>] …（只列被调用过的方法，最多 10 个） us5=<submit 累计us> us6=<waitfence 累计us>
MUYU W wf=<fence>/<submitted>/<retired>/<waited_us>/<timeout_us>/<IOReturn> n=<等待次数>
```

为腾位置，`Z2` 的 CP 环 dump 从 2 块（b=0、b=24）减到 1 块（b=24，正好是 CP 读指针）。

判读格（刷前写定）：

| 观测 | 结论 | 下一步 |
| --- | --- | --- |
| `wf_fence > wf_submitted` | 等的栅栏**根本没提交进环**（submission 丢了） | 查 Submit 路径与队列绑定 |
| `wf_submitted > wf_retired`，且 `wf_waited_us ≈ wf_timeout_us` | 提交了、**CP 没跑到**（环形卡住） | 结合 `share_region` 线索查 GPU 内存映射；看 Z2 的读指针 |
| `wf_submitted == wf_retired` | 环形其实是干净的 ⇒ 上一次等待只是超时太短（假说 A） | 转向 Turnip 侧的超时/重试策略 |
| `S` 行没有 `5:` | 一次 Submit 都没发生 ⇒ 那个 WaitFence 等的是别的连接留下的栅栏 | 查 `ff@6` 的 pid 与 `A` 行对应关系 |
| `S` 行有 `2:` 但无 `11:` | BO 分配了但队列没建起来 | 直接查 `CreateQueue` |
| `us6` 很大（> 10^6） | 等待确实占了大量时间，客户端是被拖死的 | 同上，看 CP |

### 62.5 构建记录（`vkd5`）

| 阶段 | 产物 | SHA-256 | 日志 |
| --- | --- | --- | --- |
| kernel | `out/muyu/kernel/kernel.macho` | — | `logs/build-kernel-vkd5.log`，EXIT=0 |
| payload | `out/muyu/payload/payload.img`（72 MiB） | — | `logs/build-payload-vkd5.log`，EXIT=0 |
| vendor_boot | `fw/vendor_boot_gpu_vkd5.img`（77,393,920 B） | `34670d1a02796b196c7de2e8a3f038ba0ce4c46f7912b2ae80de87648c7056f4` | `logs/build-vendor-boot-vkd5.log` |

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_vkd5.img
```

---

## §63 2026-11-12 21:16：`vkd5` 屏照 —— 环形**是干净的**（submitted=retired=5），`WaitFence` 的 17 次「失败」是零超时轮询的正常回答；`-3` 的位置要换问法

### 63.1 逐字转录（`logs/IMG_20261112_211632.jpg`，3840×2160，21:16）

```
MUYU V op=5 bad=0 cl=4 call=102 sel=6 st=00000000 at=1 inf=0 mb=0/0 tr=14 gr=5
MUYU O SpringBoard|SpringBoard|backboardd
MUYU K n=4 68f2 6a3c 6801 6800 0000 …
MUYU L pr=2 tn=102 ex=104 es=0 st=00000000 sb=00000001 00000000
MUYU C n=102 ff=6/e00002d6/27026@6 abi=2 seq=102
MUYU M 6/0 6/0 6/0 6/0 6/0 6/0 6/0 6/0 6/0 6/0
MUYU S 1:2 2:17 3:17 5:1 6:34/17 8:12 13:15 15:2 us5=2002 us6=1759
MUYU W wf=5/5/5/0/0/0 n=34
MUYU A n=6 SpringBoard/t4147/m00000000/re00002c7/a1/f0  (×2)
MUYU R PUB no device: Vulkan(VkResultCode(-3))
MUYU INIT q=6   MUYU ROOT c=6/0/0 d=4/0   MUYU BSD s=8
```

### 63.2 判读：上一轮的 `ff=6/…@6` 不是故障，是**轮询语义**

| 读数 | 含义（`tu_knl_darwin.cc:633–682`） |
| --- | --- |
| `6:34/17` | `WaitFence` 被调 34 次，其中 17 次返回非 success |
| `wf=5/5/5/0/0/0` | **最后一次**等待：请求 fence 5，环的 `submitted=5`、`retired=5` ⇒ 环已经完全排空；`waited=0us`、`timeout=0us`、status=0 |
| 由上述两条 | 那些「失败」是**零超时轮询**（`poll.timeout_ns=0`）在栅栏还没到时的正常回答 `kIOReturnTimeout`。Turnip 每次 `vkGetFenceStatus` 就是一次这样的调用。**`ff=6/e00002d6@6` 因此不是故障点，§62.2 把它当「首个失败」读过头了** |
| `5:1` | 整机只有 **1 次 `Submit`**，而且环把它退休了（retired=5，其中 4 个来自内核自己的 CP 切换探针） |
| 无 `11:` | Turnip 的 darwin 后端**根本不调 `CreateQueue`**（`tu_knl_darwin.cc` 里没有这个 method），所以「没有 11」是正常，不是缺队列 |
| `2:17 3:17 13:15` | 17 次 BO 分配、17 次 GPU 映射、15 次 CPU 映射——这不是「连接刚建完就没了」的形状，是**真的在建设备和它的内部缓冲** |
| `us5=2002 us6=1759` | 唯一那次 Submit 花 2.0 ms，34 次等待合计 1.8 ms ⇒ 等待没占时间，不存在「等栅栏把客户端拖死」 |

**结论（证据等级：屏照逐字转录 + 代码定位）**：
1. §62 的两个假说（A 太慢 / B 环形卡住）**都排除**：环是空的，`submitted == retired`，最后那次等待立即成功。
2. `-3` 发生在**最后一个 ABI 调用之后**，也就是纯用户态的一步——内核这边已经没有任何调用可看。
3. 因此「再读一个内核计数器」不会再给出答案；要问的是**调用序列的形状**：`-3` 之前那一串方法是什么、每个方法之间隔了什么。

### 63.3 `vkd6`：只读两行——方法序列（按连续段压缩）+ 最后四次 BO 请求

驱动侧（`NativeAdrenoPublish.cpp`，纯记录）：隧道调用每次写一字节进 `hist[96]`；`BoAlloc` 额外把 `bytes/align_log2/调用序号` 记进四深环。

屏上新增（紧跟 `W`）：

```
MUYU Q 6x10 5x1 13x1 3x1 2x1 … n=<总调用数>     ← 方法序列，新→旧，连续相同压成 mxN，最多 12 段
MUYU Y n=<分配次数> <序号>/<bytes>/<align> ×4     ← 最后四次 BO 请求
MUYU X <reports> <report>|<report2>              ← 仅当客户端发过 SetMetadata 文本时才出现
```

为腾位置：`Z5`、`Z3`、`T`（表切换/TTBR0）三行本轮不打，`Z2` 的 CP 环 12 字仍在。

判读格（刷前写定）：

| `Q` 行观测 | 结论 | 下一步 |
| --- | --- | --- |
| 段序为 `… 6xN 5x1 3xN 2xN 17x1 15x1 1x1`（到 `1`/`15` 结束，其后无 2/3） | 设备创建**在队列阶段之后、BO 池之前**停了 ⇒ `vk_meta_device_init` / `ir3_compiler_create` 返回失败（后者正是 `-3`「failed to initialize ir3 compiler」） | 查驱动回的 `chip_id`/`GetDeviceInfo` 与 turnip 的 `dev_id` 是否对得上（`Q` 之后加一行打印 deviceInfo reply） |
| 段序前面还有 `2x.. 3x.. 13x..`，且 `6` 只有一段在**中间** | 队列阶段的等待在中间，`-3` 在更后面的一步（`global BO` / clear-blit / dynamic rendering） | 用 `Y` 行判断：最后一条 alloc 是 global BO（几十 KiB～几百 KiB）即到此为止 |
| `Q` 尾部是 `6x34`（一大段等待，其后什么都没有） | 客户端在等一个**再也不会来的**栅栏，等完就放弃 | 回到 `W` 的 fence 与 `S` 的 `5:`，看是谁提交的 |
| `Y` 里 `n` 只有 4～6 | BO 分配早停 ⇒ 确实死在设备创建中段 | 结合 `Q` 定位到具体方法 |
| `Y` 里出现 >16 MiB 的请求 | 大池（ring/pipeline）分了 ⇒ 创建走到很后面 | 那 `-3` 在 clear-blit / dynamic rendering / pipeline cache |
| 出现 `X` 行 | **客户端自己写了文本**（插件结果或 Turnip 的 `T ` 行）——这是本线第一次可能拿到用户态原文 | 直接按文本判读，不必再猜 |

### 63.4 构建记录（`vkd6`）

| 阶段 | 产物 | SHA-256 | 日志 |
| --- | --- | --- | --- |
| kernel | `out/muyu/kernel/kernel.macho` | — | `logs/build-kernel-vkd6.log`，EXIT=0，error 0 |
| payload | `out/muyu/payload/payload.img`（72 MiB） | — | `logs/build-payload-vkd6.log`，EXIT=0 |
| vendor_boot | `fw/vendor_boot_gpu_vkd6.img`（77,393,920 B） | `ce7c093f4785629c7393e7249467432985521cda0c947883f7948e6d2f039aa9` | `logs/build-vendor-boot-vkd6.log` |

`strings` 已确认 `MUYU Q`、` %ux%u`、`MUYU Y n=%u`、` %u/%u/%u`、`MUYU X %u %s|%s` 五条格式串都在镜像里。

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_vkd6.img
```

---

## §64 2026-11-12 21:54：`vkd6` 屏照 —— `Q`/`Y` 上屏；**「死在 ir3 compiler」被源码否死**，`-3` 收窄到「设备初始化里的一次 INITIALIZATION_FAILED」

`fw/vendor_boot_gpu_vkd6.img`（`ce7c093f…`）21:50 刷入并 RAM boot，账本 `2026-11-12T13:50:33Z`。
屏照 `logs/2026-11-12-21-53-47-450.jpg`（3840×2160，正立）。

### 64.1 逐字转录（关键行，放大 2× 后逐字符确认）

```
MUYU V op=5 bad=0 cl=4 call=105 sel=6 st=00000000 at=1 inf=0 mb=0/0 tr=14 gr=5
MUYU O SpringBoard|SpringBoard|backboardd
MUYU K n=4 68f2 6a3c 6801 6800 0000 …
MUYU L pr=2 tn=105 ex=107 es=0 st=00000000 sb=00000001 00000000
MUYU C n=105 ff=6/e00002d6/2702@6 abi=2 seq=105
MUYU M 6/0 6/0 6/0 6/0 6/0 6/0 6/0 6/0 6/0 6/0
MUYU S 1:2 2:17 3:17 5:1 6:36/19 8:13 13:15 15:2 us5=799 us6=451
MUYU Q 6x13 8x1 6x1 13x1 3x1 2x1 13x1 3x1 2x1 13x1 3x1 2x1 n=105
MUYU Y n=17 88/16384/12 85/16384/12 82/8192/12 79/16384/12
MUYU A n=6 SpringBoard/t4147/m00000000/re00002c7/a1/f0  (×2)
MUYU H h1 r1 f8 f5 s0 p0 g1 k0
MUYU R PUB no device: Vulkan(VkResultCode(-3))
```

方法号对照 `NativeAdrenoABI.h:16–33`：1 Query、2 BoAlloc、3 BoMap、5 Submit、6 WaitFence、
8 **GetStatus**、13 **BoCpuMap**、15 GetDeviceInfo。

转录更正一处：`A` 行在小字号下与 `R` 同形，本轮先读成 `MUYU R n=6 SpringBoard/…`，
放大后确认是 **`A`**（正是 §34.1 第二层 user-client 的记录；`R` 只有 `PUB no device` 那一条）。

### 64.2 判读格（§63.3 刷前写定）逐条对账

| 判读格 | 本轮观测 | 判定 |
| --- | --- | --- |
| 行 1：序列到 `1`/`15` 结束 ⇒ `ir3_compiler_create` 失败 | 尾部是 `6x13`，不是 `1x1`/`15x1` | **形状不符，且结论被源码否死（见 64.3）** |
| 行 2：序列已过 `2/3/13` 且 `Y` 到 global BO ⇒ 死在 clear-blit/pipeline cache | `Y` 最后四次全是 8–16 KiB，整轮**没有任何 MiB 级分配** | 不符：没走到 global BO / ring 池 |
| 行 3：`Q` 尾部一大段等待，其后什么都没有 ⇒ 在等一个不会来的栅栏 | 尾部确为 **13 次连续 `WaitFence`**，其后无任何方法 | **形状命中；但解释要改，见 64.4** |
| 行 4：`Y` 的 `n` 只有 4～6 ⇒ 早停 | `n=17` | 不符 |
| 行 5：`Y` 出现 >16 MiB ⇒ 创建走到很后面 | 最大 16 KiB | 不符 |
| 行 6：出现 `X` 行 ⇒ 拿到客户端原文 | **没有 `X` 行** | 客户端一次 SetMetadata 文本都没发过 |

### 64.3 ⭐ 「ir3 compiler」这条路被源码否死

`tu_device.cc:2984–2991` 唯一会因为编译器失败返回 `-3` 的地方是 `if (!device->compiler)`。
但 `ir3_compiler.c:221` 的 `ir3_compiler_create()`：

```c
struct ir3_compiler *compiler = rzalloc(NULL, struct ir3_compiler);
ir3_compiler_debug_init();
compiler->dev = dev;          /* 立刻解引用 */
...
return compiler;              /* 全函数唯一出口 */
```

`rzalloc` 之后**没有空判**，下一行就解引用；函数体内没有任何 `return NULL`。
⇒ 它只可能在 OOM 时**崩**，不可能**返回 NULL**。`if (!device->compiler)` 是死分支，
**「failed to initialize ir3 compiler」不可能发生**。§63.3 行 1 的结论作废（该格曾是我最看好的一条）。

同理 `tu_CreateDevice` 另外三个 `-3`（`zombie_vmas`、`pthread_condattr_init`、`pthread_cond_init`）
与 `darwin_device_init` 的唯一 `-3`（`pthread_mutex_init`）都是 pthread/分配失败，板上没有可解释的输入。

### 64.4 `-3` 剩下哪几个出口，以及为什么"等不到栅栏"也不成立

`-3` = `VK_ERROR_INITIALIZATION_FAILED`，**不是** `-4`（`VK_ERROR_DEVICE_LOST`）。这一条把
「device lost 导致 -3」直接排掉一半：`vk_device_set_lost()` 返回的是 `-4`。

目前代码里能产出 `-3` 且**可能在本板触发**的出口只剩 `dw_open_connection()`（`tu_knl_darwin.cc:177`）的三处：

1. `IOServiceOpen(%s, 0x%x) failed: 0x%x`
2. `no external-method selector is admitted for this process`
3. `NativeAdreno query failed: 0x%x`（Query 的 ABI 版本不符，或 GetDeviceInfo 失败）

其中第 2 条已被 vkd3 证伪（`sb=00000001`）；第 1、3 条**没法从现有计数器分辨**。

同时 `W wf=5/5/5/2/0/0`（不是 §63 的 `0/0`）：最后一次等待请求 fence 5、环 `submitted=5 retired=5`、
**2 µs 就返回、status=0（成功）**。所以尾部那 13 次连续 `WaitFence` **不是**"等一个不会来的栅栏"，
而是**一串立即返回的轮询**，最后一个还成功了。`us6=451`（36 次等待合计 451 µs）也说明等待没占时间。
⇒ 判读格行 3 的**形状**是对的（尾部纯等待、之后无调用），但**机理**要改：
客户端是"问完、得到肯定答复、然后自己放弃"，不是"等超时"。

**新增事实：`H h1 r1`** —— 这一 boot 里 GPU 挂过 1 次、重启过 1 次。与 §42.3 的 `c72a8a5258`
（重启后 Turnip 把 device 标 lost）在现象上对得上，但因 `-3 ≠ -4`，只能当**加重因素**，不能当因果。

### 64.5 结论

1. 内核侧、隧道、105 次 ABI 调用**全部正常**，与 vkd5 同形；`-3` 仍落在客户端。
2. 调用序列的尾部是**等待**不是**构建**：`(2 BoAlloc → 3 BoMap → 13 BoCpuMap)` 三连出现四次
   （`Y` 记到调用序号 79/82/85/88），随后 `6`、`8 GetStatus`、`6×13`，然后**一个方法都不再调**。
3. 全程没有 MiB 级 BO ⇒ Turnip **没有建到 GPU 的 global BO / ring 池**，即设备创建仍在早期。
4. 设备的 6 次 `IOServiceOpen` 里只有 2 次真正建了隧道（`pr=2`），两次都完整走完
   `Query`+`GetDeviceInfo`（`1:2 15:2`）——和 `dw_open_connection()` 里"枚举探针一次 + 设备初始化一次"
   的结构完全对应（`darwin_device_init` 的分支注释就写着 "The enumeration probe closed its connection;
   take the single slot for this device."）。
5. ⇒ 剩下唯一没被排除的 `-3` 出口，是 `dw_open_connection()` 的第 1/3 条，
   或**某一次 `IOServiceOpen` 根本没到隧道那一层**（`A n=6` 只看到最新两条，前四条未知）。

### 64.6 下一轮：换一个能分辨"哪一句 startup errorf"的只读出口（不重建 turnip）

**先更正一处自己的错**：原计划打 `native_adreno_mount_fail[4]`，但它在 `MUYU B` 行里**早就在打了**
（`i48 o40 f0 s0 b0 n0` ⇒ `[0]=0 [1]=0 [2]=0 [3]=0`）。`n=0` 意味着**挂载从未失败过一次**。
再加上 `S` 的 `1:2` / `15:2` 都不带 `/f`（Query 与 GetDeviceInfo 一次都没失败过），
`dw_open_connection()` 的三条出口就**全部被排掉了**：第 2 条早有 `sb=00000001`，
第 3 条要 Query/GetDeviceInfo 失败（没有），第 1 条要 IOServiceOpen 失败（两次都成功建了隧道）。

⇒ 剩下没读的是**"有没有进程被直接拒掉一个连接、为什么"**。
`NativeAdrenoPublish.cpp:142` 的 `native_adreno_client_log[8]` 正是这份账：按进程名记 授予 / 拒绝 / 关闭 / 最高 GPU 地址 / 最后一次拒绝原因，
同样是**写好了从没上屏**。

`vkd7` 只加一行（紧跟 `Y`，腾位去掉 `M`）：

```
MUYU G <name>/o<授予>/r<拒绝>/c<关闭>/p<peak MiB>/x<最后拒绝原因> … n<行数>
```

判读格（刷前写定）：

| `G` 行观测 | 结论 | 下一步 |
| --- | --- | --- |
| 某进程 `r>0` 且 `x` 非 0 | 该进程**被拒过连接**，`x` 就是拒绝原因 ⇒ `-3` 在 `IOServiceOpen`/沙箱这一层，不在 turnip | 按 `x` 的取值查驱动的拒绝分支（`NativeAdrenoPublish.cpp:1080` 起的类型/沙箱判定） |
| 全部 `r=0`、`c` 与 `o` 配平 | 连接全授予且正常关闭 ⇒ `-3` 发生在一次**没有走到驱动**的调用里（例如 `vkCreateInstance` 阶段就失败） | 转去数 `candidates` 循环：`K` 行 `6810+i` 一个都没有，说明循环里第一次尝试就没建成 device |
| `o` 只有 1 而 `A n=6` | 6 次 open 里只有 1 次算"授予" ⇒ 其余 5 次在类型/沙箱判定处被拒 | 把 `A` 行改成打全部 6 条（现在只打最新 2 条） |

不改 turnip、不改 addon、不动 trust cache：只加一条 `printf`。

### 64.7 `vkd7` 构建记录（2026-11-12 22:06）

| 阶段 | 产物 | 说明 |
| --- | --- | --- |
| kernel | `out/muyu/kernel/kernel.macho`（53,202,944 B） | `logs/build-vkd7.log`，EXIT=0，`error:` 计数 **0** |
| payload | `out/muyu/payload/payload.img`（75,497,472 B / 72 MiB） | 同上 |
| vendor_boot | `fw/vendor_boot_gpu_vkd7.img`（77,393,920 B） | sha `5884a6fef1346bd435d977fcb3f1fef73793ad3218020095992a529e662972bf` |

- 命令行与 `vkd6` **逐字节相同**：`board.toml` 45 参 + 注入 `sheng-fb-gpu=1 sheng-gpu-addon=1 sheng-gpu-metal=1`
  = 48 参 / **996 B**（上限 1000）。`--trust-cache 21H461-original-trust.bundle`、`--gpu-addon gpuaddon-17-full.bin`。
- `strings` 确认 `MUYU G` 与 ` %s/o%u/r%u/c%u/p%u/x%x` 在镜像里，`MUYU M` 已为 0 处。
- 打包底图用 `fw/vendor_boot_gpu_vkd6.img`（`nofs1` 只有 64,811,008 B，装不下 72 MiB payload，脚本会拒绝）。
  版面与 vkd6 完全一致：`ramdisk 75497472 at 0x1000; dtb 1885961 at 0x4801000; table 108; bootconfig 205`；
  逐字节对比两者：差异全部落在 `0x13e8–0x400296a`（payload 区内），**`0x4800000` 之后的 dtb/table/bootconfig 完全相同**。
- 源码备份 `/tmp/vkd7-backup/BravearkLastLog.c`（改前 sha `86a6776b…`）。

---

## §65 2026-11-12 22:12：`vkd7` 第一张屏照 —— **`-3` 没有复现**；这一 boot 的用户态图形栈一次都没起来

`fw/vendor_boot_gpu_vkd7.img`（`5884a6fe…`）22:08:27 刷入并 RAM boot（账本 `2026-11-12T14:08:27Z`，rc=0，电量 3836 mV）。
屏照 `logs/IMG_20261112_221231.jpg`（3840×2160，正立，约 boot 后 4 分钟）。

### 65.1 逐字转录（3× 放大逐字符确认）

```
MUYU PROBE st=37 cp=10 b=27
MUYU B i48 o40 f0 s0 b0 n0
MUYU V op=0 bad=0 cl=0 call=0 sel=0 st=00000000 at=1 inf=0 mb=0/0 tr=0 gr=0
MUYU O backboardd||
MUYU K n=0 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000 0000
MUYU L pr=0 tn=0 ex=0 es=0 st=00000000 sb=00000001 00000000
MUYU C n=0 ff=0/0/000 abi=0 seq=0
MUYU S us5=6 us6=0
MUYU W wf=0/0/0/0/0/0 n=0
MUYU Q n=0
MUYU Y n=0
MUYU G n0
MUYU A n=1 backboardd/t1/m00000001/re00002e2/a1/f1
MUYU H h0 r0 f0 F0 s0 p0 g0 k0
MUYU F n0 s0 a0/0 y0
MUYU Z0 …（全 0）
MUYU RT n0 s0 e0 b0
MUYU RT2 -- gpuRestart not called (n0)
MUYU D0 P ok ok ok skip io0 pw11
MUYU D1 V chip=43030b00 gmaxc=32/32/32/0
MUYU D2 R gf12/12 m0 cx=3/3 m0
MUYU D3 B st0 why0 d0 p0 s0 c00050030
MUYU D4 X cx=0022f801>f822f800 hw=00000202 gx=0022f801
MUYU D5 A ahb00000001>00000001 pll0060257f nm80000003
MUYU A0 i4c017e18 j40000016 s00260406
MUYU A1 f0000000 c00000167 a0000003
MUYU UP lg=10/1/1/1 s=1/0 op=0/0 mf=0/0 i=48 r=-
MUYU ROOT c=6/0/0 d=4/0
MUYU INIT q=6
MUYU BSD s=0
```

`Z/Q/D/A0/A1` 这些块是 `BravearkLastLog.c:869–1014` 里**无条件**打印的（只有 weak_import 判空），
本轮之所以第一次看见它们：页面容量变了（本轮字号更小、可容 48 行），与之对照 vkd6 那张只拍到 12 行，
`R` 之后的行落在取景框外。**不是新代码路径，也不是新状态**。

### 65.2 判读：`-3` 这一轮根本没发生

与 vkd6 逐项对照（同一台机、同一 payload 参数）：

| 行 | vkd6 | vkd7 本轮 | 含义 |
| --- | --- | --- | --- |
| `UP op=` | 5 开 / 4 关 | **0 / 0** | 发布出去的服务**一次都没被打开** |
| `V op/call` | 5 / 105 | **0 / 0** | 加速度器一个方法都没被调过 |
| `L pr/tn` | 2 / 105 | **0 / 0** | 没有 selector 被探过，也没有隧道调用 |
| `A n=` | 6（含 SpringBoard） | **1（只有 backboardd）** | 全机只有 backboardd 试过一次 |
| `R PUB …` | 有 `-3` | **没有这一行** | 客户端没跑，自然没写原因串 |
| `H` | h1 r1 | **h0 r0** | 这一 boot 没挂过 |

⇒ **`dw_open_connection` 的三条出口、`tu_CreateDevice` 的四条、candidates 循环，全都没有被执行到**。
vkd7 这一轮对 `-3` 的位置没有提供任何新证据。

### 65.3 但拿到两件有用的事实

**一、`MUYU A` 的 `r=` 不是 open 的返回值，而是沙箱门的返回值。**
`NativeAdrenoPublish.cpp:1070`：`record.res=resolver;`（`resolver` 是 `io_filter` 判定的结果），
`attach` 是 `attach_state`，`foreign = type != NativeAdrenoPluginOpenType`。所以本行应读作：

> 唯一那次 open 来自 **backboardd、type=1（原版 IOGPU，非本插件）、沙箱返回 `0xe00002e2` = `kIOReturnNotPermitted`、attach 已建立、属 foreign**。

type 1 本来就被 `NativeAdrenoPublish.cpp:1080` 明确拒绝（`kIOReturnUnsupported`），属设计行为，不是故障。
**这条修正推翻一处旧读法**：以前把 `A` 的 `r=` 当成 open 结果，会误判"open 失败"。

**二、这一 boot 停在用户态之前。**
`MUYU INIT q=6` 对应 `kern_exec.c:8246`（`load_init_program` 里 **SystemOS mount 返回**那一步），
`ROOT c=6/0/0 d=4/0` 是 owner cache / owner data 的子步。也就是说：内核把 GPU 服务发布了
（`B i48 o40`、`cp=10` 表示 CP 已走到 `publishMetal()` 那道门），**但 launchd→SpringBoard 这一段没有把图形栈拉起来**，
4 分钟里只有 backboardd 碰过一次（而且是用错误 type 碰的）。

这是**跨 boot 的不确定性**，不是 vkd7 改动引入的：vkd7 相对 vkd6 只多打一行 `G`、少打一行 `M`，
`patch-vendor-boot.py` 的产物除 payload 区外与 vkd6 逐字节相同（§64.7 已核）。

### 65.4 下一步（最便宜的两步，都不需要重建）

1. **再拍一张**（ticker 每 5 s 一整页）。若 `UP op=` / `call=` 开始爬升 ⇒ 只是拍早了，按 §64.6 的 `G` 行判读格继续；
   若仍是 `op=0/0 call=0` ⇒ 这一 boot 卡在 launchd 之前。
2. **同一镜像重刷一次**（`vendor_boot_gpu_vkd7.img` 原样再刷，不用重建）。vkd6 在同一台机上三分半钟就跑到了
   SpringBoard 打开 5 次，所以"用户态起不来"大概率是本 boot 的一次性现象，重刷即可回到可比状态。

---

## §66 2026-11-12 22:25：`uke` mode 9 与我们 `MUYU` 行逐项对照 —— 结论是**借不到补丁，但借到判据**；附带纠正三处过度推断

离线核对 `UniXNU-uke/docs/GPU-PORT.md`（976 行）与 `docs/PROGRESS.md`，目标是回答"uke 走过的哪一步能直接搬过来"。
结论先说：**能借的是判据写法，不是补丁**——因为 uke 从来没在设备创建上撞到 `-3`。

### 66.1 uke 的九级阶梯与证据（GPU-PORT.md:127–142，均为 2026-10-23 实机）

| 级 | 断言 | 证据串 |
| --- | --- | --- |
| 1 | GCC/GPUCC 可读回 | `gcc=ok cc=ok sm=skip gp=skip io=0 pw00` |
| 3 | CX GDSC + SMMU vote clock 上电 | `SM 00290406 4c017e18 40000016 v0 R00000000`（ID0 = sheng MMU-500，24 context bank） |
| 4 | GMU SMMU context `Ready` | `B s7 st0 k0/0 i5511 p5w13r1e1 r4w2v1`、`T00010000c900f000 S00000167 C00000001 R00000005` |
| 6 | PLL1 锁定、hub 在 GPLL0/3 | `C s7 f0 l2 u2/1 pe0000005 o1 h505 g303` |
| 7 | GMU 冷启动 + HFI 握手 | `G1/0/0/0>G0/1/0/1`、`P0 g8 kc6 e0 t0 n4 1ms`、`S0 ph7 h1 f100 H1 r11 a0 c1 v0 p18` |
| 8 | GX 上电 + perf vote 应答 | `X v0/0 o3 x3>0 cf8222800 R1 H8` |
| 9 | GPU SMMU context + SQE/CP 环 + zap/IRQ + Metal publish | `Q s7 st0 k0/0 p5e1 r4v1 n5`、`C6sent`/`C10ok`、`S zap`/`S irq`、`P published`、`M … k0 s5/0 r1` |

mode 9 之后剩下的才轮到用户态，而用户态那一级他们是**通过**的：addon 双视图挂上（`a5.0/5.0`）、
`AGXDeviceUserClient` 被 render server 打开（心跳 `o4/0/2`）、**Turnip 裸用户测试通过**。
他们卡的是再下一级的 CoreAnimation 管线编译（`InvalidDescriptor(ColorAttachment)`，`6a3c`/`69f3` 交替重试）。

### 66.2 ⭐ 最重要的实操提醒：两个仓库用同样的字母表示完全不同的东西

拿 uke 的日志判读格套我们的屏照，会**每行都读错**。逐字母核对如下：

| 字母 | uke 的含义 | 我们的含义 | 同物？ |
| --- | --- | --- | --- |
| `B` | GMU SMMU context 探测（step/spans/…） | 服务发布窗口位 + mount 失败记录（`i/o/f/s/b/n`） | **否** |
| `C` | 时钟与 CP init（`C6sent`/`C10ok`） | 隧道 ABI 调用记录（`n/ff/abi/seq`） | **否** |
| `Q` | GPU SMMU context 探测（`s7 st0 k0/0 p5e1…`） | ABI 方法序列压缩（`6x13 8x1 …`） | **否** |
| `S` | HFI 握手 / `S zap` / `S irq` | 每方法调用计数（`1:2 2:17 …`） | **否** |
| `X` | GX 电源与 perf vote（`X v0/0 o3 x3>0 …`） | 客户端自己写的文本 | **否** |
| `M` | Metal accelerator 发布 + 插件挂载 + opens | 已删（vkd6 及以前是最近 10 次调用） | **否**（语义相近） |
| `P` | publish 阶段（`P start`/`P published`） | 无此行（我们用 `PROBE cp=`） | **否** |
| `K` | 插件标记环（`K68xx`） | 插件标记环（`n=4 68f2 6a3c 6801 6800`） | **是**，同一套 `0x68xx` |

只有 `K` 是同一套。`0x68f2` = `NativeAdrenoMetalPlugin.mm:402` 的 `device = nil` 标记，两边同一个常量、
同一行源码；**但成因不同（见 66.4）**。

### 66.3 我们的行 → uke 阶梯的对应关系（哪些级已过）

| uke 级 | 我们等价的观测 | vkd6 读数 | 判定 |
| --- | --- | --- | --- |
| 电源/时钟（级 1–3、6） | `MUYU D0 P ok ok ok skip io0 pw11`、`D1 V chip=43030b00` | 同形 | **通过** |
| GMU 冷启动 + HFI（级 4、7） | `D2 R gf12/12 m0 cx=3/3 m0`、`D3 B st0`、`PROBE st=37` | 同形 | **通过** |
| GX + perf vote（级 8） | `D4 X cx=0022f801>f822f800`、`D5 A ahb00000001>00000001` | 同形 | **通过** |
| GPU SMMU context + CP 环（级 9） | `PROBE b=27`（`arm_smmu4k_attach_crumb`）、`S 5:1`（1 次 Submit 成功） | 同形 | **通过** |
| zap / IRQ | `H h1 r1`（挂过 1 次、重启 1 次）、`Z0/Z1` 全 0 | 有重启但已恢复 | **通过** |
| Metal accelerator publish（级 9 末） | `PROBE cp=10`（`publishMetal()` 那道门）、`V op=5 bad=0 at=1` | 同形 | **通过** |
| 客户端打开 + 隧道 | `V op=5 cl=4`、`L pr=2 tn=105 ex=107 sb=00000001`、`C abi=2` | 同形 | **通过** |
| 设备创建（uke 这里通过） | `K … 68f2`、`R PUB no device: Vulkan(VkResultCode(-3))` | **失败** | **断点** |
| CA 管线编译（uke 卡这里） | 未到达 | — | 未到达 |

也就是说：**九级阶梯我们全部走完，第一层到第七层（客户端打开与隧道）也全部走完，断在"设备创建"这一格，
而 uke 在这一格是通的。** 这正是为什么 uke 的补丁一条都用不上——他们的补丁全部作用在我们**下一级**。

### 66.4 纠正三处过度推断（本轮从同一批数据里算出来的）

**（一）`Y` 只打最后四次，不能用它断言"全程没有 MiB 级分配"。**
`MUYU Y n=17 88/16384/12 85/16384/12 82/8192/12 79/16384/12` 只覆盖调用序号 79–88 的四次分配。
而 Turnip 的 `global_bo` 是 `tu_CreateDevice` 里**第一次**分配（`tu_device.cc:3041`），
根本不在 `Y` 的窗口内。§64.5 结论 3「全程没有 MiB 级 BO ⇒ 没建到 global BO / ring 池」是**过度推断**，
该结论作废；17 次分配里有一次完全可能就是 global BO。

**（二）`6a3c` 不是失败标记。**
它是 `NativeAdrenoMetalPlugin.mm:295` 的 `gpu->mark(0x6A00 | bits)`，bits 由四个条件拼出来：
bit2 = MobileGestalt 深度帧缓冲答 true、bit3 = 该答案存在、bit4 = 压缩能力字典存在、bit5 = 非 root。
`0x3c` = 这四位全中，是**本机正常状态**，每轮无条件打一次。uke 文档把 `0x6a3c` 记成"渲染尝试的帧缓冲诊断"
（因为他们每轮重试渲染，所以循环出现），**不能据此判断我们在重试渲染**。

**（三）`68f2` 同名不同因。**
uke：驱动 4 个连接槽（`NativeAdrenoMaxClients=4`，槽 0 按名保留给 backboardd），第 5 个连接拿
`kIOReturnNoSpace`、队列范围仍有作业的拿 `kIOReturnBusy`，两者都落到 `device = nil`。
我们：`NativeAdrenoPublish.cpp` 这一版**没有槽位上限**（`opens` 无 cap，每个连接独立 VM + 独立队列段，
`row 0` 只是 backboardd 的统计位），而且 `V bad=0` 证明一次拒绝都没有。
我们的 `68f2` 只有一个成因：`native_bridge_metal_open` 对**每一个**候选缓存目录都返回 nil（`:399` 的循环）。
⇒ **uke 那条"槽位用尽"的解释不适用于我们**，不要照搬。

### 66.5 一个新算术：`S` 里没有方法 4，尾巴 13 次等待因此不能解释成"拆设备"

`MUYU S 1:2 2:17 3:17 5:1 6:36/19 8:13 13:15 15:2`。方法号（`NativeAdrenoABI.h:18–33`）：
1 Query、2 BoAlloc、3 BoMap、**4 BoFree**、5 Submit、6 WaitFence、8 GetStatus、13 BoCpuMap、15 GetDeviceInfo。

**方法 4 一次都没有。** 而 `tu_bo_finish` → `darwin_bo_finish`（`tu_knl_darwin.cc:1095`）只是把 BO 记进退休环，
真正发 `BoFree` 的是 `darwin_release_native`，且只在 fence 已 retired 时（`:769`）。
所以"BoFree=0"本身不排除拆设备。

但接不下去的是 fence 数：`W wf=5/5/5` 说明最后那次等待的 fence 是 5、环 `submitted=retired=5`，
fence 只到 5；而 `darwin_reap_locked` 的 distinct-fence 缓存（`:750–758`）对同一个 fence 只等一次。
⇒ **13 次连续等待 ⇒ 至少 13 个不同的 fence ⇒ 与"fence 只到 5"矛盾。**
⇒ 尾部那 13 次 `WaitFence` **不是** `tu_bo_finish` 的退休扫描。

这一条**不下结论**。它是 vkd8 要回答的第一个问题：现在只记了"最后一次等待的 fence"，缺的是
"每次等待各等的是哪个 fence"。这需要每个方法计数之外再加一个 16 深的 fence 值环。

### 66.6 借得动的两条（都不是补丁）

1. **判据脚本化。** uke 有 `scripts/check-gpu-addon-boot.py`，把"读哪几行、什么算通过"写成程序，
   覆盖 probe/stable、缓存启动、`68f1/68f2/68f3` 各种组合。我们每轮靠人眼看屏 + 逐段放大，
   单轮判读十几分钟，而且已经出过两次误读（K 行空转、C 行死通道）。把现有 `MUYU` 行的判读规则
   写成脚本，是纯收益。
2. **每级一句结论的表。** uke 的九级阶梯每级一行、每行一个断言、断言里带原始寄存器值。
   我们的行数比他们多，但缺"这一级过了没有"的汇总表——§66.3 那张就是补这个。

### 66.7 结论与下一步

1. **九级阶梯 + 客户端隧道，我们全部通过；断点唯一且明确：设备创建返回 `-3`。**
2. **uke 从未在这一格失败**，所以没有可搬的修复；他们的 addon 双视图、blob staging、
   sandbox client grant、CA 描述符四条补丁全部作用在我们的下一级，本轮一个都不用做。
3. 上轮的"没建到 global BO"被推翻（`Y` 窗口太窄）；"ir3 compiler"更早已被源码否死。
   现在 `-3` 的位置比 §64 时更窄，但仍未定死。
4. 下一步两件，都不需要重建：
   - **先读 vkd7 已有的 `MUYU G` 行**（vkd7 比 vkd6 多的就是这一行，屏照还没拍到过它）；
   - 若 `G` 仍判不出来，vkd8 只加一个 16 深 fence 值环，把 66.5 那个矛盾解掉。

---

## §67 2026-11-12 22:45：**Turnip 在 Mac 上跑起来了** —— 假 IOKit 层把真实 dylib 跑到 `vkCreateDevice` 成功，`-3` 从此可在主机上穷举

这一轮回答一个此前只敢想的问题：**能不能不刷机、不拍屏，在主机上拿到 `-3` 的答案。**
结论：**能跑到设备创建成功**，而且用的是板上同一份源码编出的同一份二进制。下面是可复现的方法与边界。

### 67.1 关键前提：这个 dylib 本来就是 macOS 主机编出来的

`gpu-build.json` 的 options 里写着 `-Dplatforms=macos`、`buildtype=release`、`b_ndebug=true`，
`compiler` 是 `Apple clang 21.0.0 Target: arm64-apple-darwin27.2.0`。也就是说 Turnip 侧从来不是在设备上编的。
唯一的差别是 `LC_BUILD_VERSION` 标了 `platform 2`（iOS）、`minos 17.0`，所以 macOS 的 dyld 拒绝加载。

实测报错：

```
dlopen: mach-o file, but incompatible platform (have 'iOS', need 'macOS')
```

### 67.2 三步让它在主机上跑起来（全部只改副本，不动仓库产物）

```bash
# 1. 改平台的元数据（不改一个字节指令）
vtool -set-build-version macos 11.0 27.0 -replace -output /tmp/tu_mac.dylib <原件>
# 2. 重签（原件是 adhoc，改元数据后签名失效）
codesign -f -s - /tmp/tu_mac.dylib
# 3. 注入假 IOKit 层
DYLD_INSERT_LIBRARIES=/tmp/libfakeiokit.dylib /tmp/icdtest /tmp/tu_mac.dylib
```

`dlopen` 随后成功，`vk_icdGetInstanceProcAddr` / `vk_icdNegotiateLoaderICDInterfaceVersion` 都在导出表里
（`nm -gU` 可见），不需要 Vulkan loader。

### 67.3 要替换的面只有 8 个符号、5 处调用点

`nm -u` 显示 dylib 只 weak-import 这 8 个 IOKit 符号，且**全部集中在 `tu_knl_darwin.cc`**：

| 符号 | 调用点 | 语义 |
| --- | --- | --- |
| `IOServiceMatching` / `IOServiceGetMatchingService` | `:189–190` | 找服务（`IOAcceleratorES`） |
| `IOServiceOpen` / `IOServiceClose` / `IOObjectRelease` | `:194–213`、`:303` | 建/关连接 |
| `IOConnectCallStructMethod` | `:149`、`:158`、`:168` | **全部 ABI 调用与 selector 探测** |
| `IOConnectMapMemory64` / `IOConnectUnmapMemory64` | `:1037`、`:581` | BO 的 CPU 映射 |

用 `__DATA,__interpose` 段替换（`DYLD_INTERPOSE` 宏），不需要改 Turnip 一行源码。
隧道报文头也已确认：`DW_TUNNEL_MAGIC = 0x4e544e4c`（`tu_knl_darwin.cc:39`），
所以桩能把 `{magic, method}` 解出来，按方法号回包。

### 67.4 结果：`vkCreateDevice` 在主机上**成功**，序列与板上同形

```
[IOK] IOServiceOpen(type=0x4147) -> success
[IOK] call #1 method=1  (Query)          [IOK] call #2  method=15 (GetDeviceInfo)
[IOK] IOServiceClose
[IOK] IOServiceOpen(type=0x4147) -> success
[IOK] call #3 method=1   #4 method=15
[IOK] call #5 method=2   #6 method=3   #7  method=13   MapMemory
[IOK] call #8 method=2   #9 method=3   #10 method=13   MapMemory
[IOK] call #11 method=2  #12 method=3  #13 method=13   MapMemory
=== 13 calls: 1/8 15/8 1/8 15/8 2/24 3/40 13/16 2/24 3/40 13/16 2/24 3/40 13/16
=== 3 BoAlloc: 32768 131072 16384
[host] vkCreateInstance -> 0 (VK_SUCCESS)
[host] vkEnumeratePhysicalDevices -> 0 n=1
[host] vkCreateDevice -> 0 (VK_SUCCESS)     ← 板上这一步是 -3
```

对照板上 vkd6：

| 观测 | 板上 | 主机 |
| --- | --- | --- |
| 真实隧道数 | `pr=2` | **2 次 open**，同形 |
| `Query` / `GetDeviceInfo` | `1:2` / `15:2` | 2 / 2，**同形** |
| 尾部三连 | `(13x1 3x1 2x1)×3` | **三组 `2→3→13`，同形** |
| BoAlloc 次数 | 17 | 3 |
| `Submit` / `WaitFence` / `GetStatus` | `5:1` / `6:36` / `8:13` | **0 / 0 / 0** |
| 结果 | `-3` | **成功** |

**这条最重要**：主机上没有真实硬件、没有内核驱动、回包全是合成的，设备却建成了。
⇒ `-3` **不是代码逻辑的必然结果**，而是某个**输入条件**不满足。问题从"代码有 bug"缩小成"哪个输入不对"。

### 67.5 半路踩到的坑，正好是一条判据

第一次跑主机上停在 `tu_device.cc:3071 empty shaders (VK_ERROR_OUT_OF_HOST_MEMORY)`，只走了 10 次调用。
根因不在 shader，而在 `tu_suballoc_bo_alloc`（`tu_suballoc.cc:92`）：

```c
VkResult result = tu_bo_map(suballoc->dev, suballoc->bo, NULL);
if (result != VK_SUCCESS) { ...; return VK_ERROR_OUT_OF_HOST_MEMORY; }   /* 一律报 OOM */
```

而 `darwin_bo_map`（`tu_knl_darwin.cc:1037`）的判据是 `length < bo->size` —— 桩把映射长度封顶在 64 KiB，
BO 一大就被判失败，然后被上层统一翻译成 OOM。**这条链说明：Turnip 里很多失败会被改写成 OOM，
所以"OOM"在板上不能当内存不足读。**

### 67.6 ⭐ 副产品：板上 `S 5:1` 的那次 Submit，设备创建路径不可能产生

`NativeAdrenoMethodSubmit` 在整个 `tu_knl_darwin.cc` 里只有一个调用点：`darwin_queue_submit`（`:1439`）。
而 `vkCreateDevice` 的完整路径（`#1`–`#13`，主机上已实测）**一次 Submit 都没有**。

板上却是 `5:1`（Submit 1 次）+ `6:36/19`（WaitFence 36 次、19 次超时）+ `8:13`（GetStatus 13 次）。
这个组合只可能来自一次**真实队列提交之后的等待**。

再由 `native_adreno_method_stats` 是**全局变量**（`NativeAdrenoUserClient.cpp:31`，`extern "C"` 顶层定义，
跨所有 UserClient 累加，不是 per-connection），`S` 的 105 次是**所有连接合计**。

⇒ 推论（待证）：**`-3` 之前，已经有某个 Vulkan 客户端成功建过设备并提交了作业**；
`R PUB no device: Vulkan(VkResultCode(-3))` 是**另一个**设备实例/进程的失败。
这与 `A n=6`（六次 open、只有两次建隧道）是一致的。

如果成立，`-3` 就不是"整条图形栈不通"，而是**特定进程建不出设备** —— 这正是 vkd7 新增的
`MUYU G`（按进程记 授予/拒绝/关闭/最后拒绝原因）要回答的问题。

### 67.7 边界（必须说清，否则会误用）

1. **回包是合成的，不是板上真值。** 主机上成功只证明"这些输入下能成功"，不能证明板上为什么失败。
   它的正确用法是**穷举假设**：把某个字段改成可疑值，看是否复现 `-3`，从而列出候选真凶，回板上只测这几项。
2. **`/tmp/tu_mac.dylib` 是改了 LC_BUILD_VERSION 的副本**，与 `fw/gpuaddon-17-full.bin` 里打包的
   Turnip 未逐一核对（addon 是 `NGPUADD2` 自定义容器，12,132,608 B，不含未压缩的 17,310,976 B dylib）。
   同源性由 `gpu-build.json` 的 `source_id = 5da1a7ca…` 保证，字节级待核。
3. **主机上只走到 13 次调用**（设备创建完就没有后续渲染），板上 105 次含提交与等待。覆盖面不同。
4. 这不能替代设备验证：Vulkan 语义正确 ≠ 真 GPU 能跑。uke 的教训就是"裸测试通过、CoreAnimation 仍崩"。

### 67.8 下一步（按性价比排序）

1. **Record/Replay（最高价值）**：在驱动侧加一个 4 MiB 环形录制，把每次 `IOConnectCallStructMethod`
   的 `(method, in[0..255], out[0..255], kr)` 存下，dump 成文件；主机上让桩按这份 recording 回放。
   **一次刷机，之后全部在主机上迭代**。这是把"虚拟环境"从"假设穷举"升级成"真实复现"的唯一一步。
2. **敏感性扫描（现在就能做，零刷机）**：逐个改回包字段（`gmem_bytes`、`va_size`、`ubwc_v`、`flags`、
   `abi_version`、各 method 的 `kr`），记录哪些会产出 `-3`、哪些产出别的码，形成"输入 → 错误码"表。
3. 把 §67.2–67.4 的三条命令固化成 `scripts/host-turnip-harness.sh`，进版本库。

### 67.9 复现清单

| 文件 | 作用 |
| --- | --- |
| `/tmp/fakeiokit.c` | 8 个 IOKit 符号的 interpose 桩；按方法号回合成包；末尾 dump 调用序列 |
| `/tmp/icdtest.c` | 直接驱动 ICD：`vk_icdGetInstanceProcAddr` → `vkCreateInstance` → `vkEnumeratePhysicalDevices` → `vkCreateDevice` |
| `/tmp/tu_mac.dylib` | `vtool` 改平台 + `codesign -f -s -` 后的 Turnip 副本 |
| `/tmp/libfakeiokit.dylib` | 桩的产物，用 `DYLD_INSERT_LIBRARIES` 注入 |

（这些在 `/tmp`，重启会丢；步骤在 67.2，五分钟可重建。）

### 67.10 判别力验证：改一个字段就能定点产出不同错误码（零刷机）

harness 的价值取决于它能不能"想让它错在哪就错在哪"。实测三个反例：

| 改动的回包字段 | 主机上的结果 | 客户端原文 |
| --- | --- | --- |
| 都不改（合成正常值） | `vkCreateDevice -> 0` | — |
| `Params.abi_version` 2 → **3** | **`-3`** `VK_ERROR_INITIALIZATION_FAILED` | `tu_knl_darwin.cc:216: NativeAdreno query failed: 0xe00002c7`、`could not open the kernel client (-3)` |
| `DeviceInfo.chip_id` → **0x12345678** | **`-9`** `VK_ERROR_INCOMPATIBLE_DRIVER` | `tu_device.cc:1683: device (chip_id = 12345678, gpu_id = 2406) is unsupported` |
| `DeviceInfo.flags` 1 → **0**（去掉客户端自选地址） | `vkCreateDevice -> 0` | — |

三条结论：

1. **`abi_version` 不匹配是能产出 `-3` 的**，且 `dw_open_connection` 的这条出口会带一句原文。
   但板上从来没有 `MUYU X` 文本 —— 这不等于"没有原文"（`X` 抓的是 SetMetadata 路径，见 §64.6），
   只说明**不能用"没看到文本"去排除这条**。
2. **芯片不认识不是我们的问题**：它产出的是 `-9`，不是 `-3`。板上是 `-3`，所以 `0x43030b00` 确实被
   mesa 表认（`freedreno_devices.py:1008` 的 `FD735`）。这条彻底排除。
3. **`flags` 不影响设备创建**（桩的 BoMap 总是回一个 iova，掩盖了客户端选地址与驱动选地址的差别）。
   这说明 harness 的盲区要主动识别：**桩替 Turnip 做的决定，就不能再用来判断 Turnip。**

把这三条并进 §67.4 的判读：主机上能复现 `-3` 但复现不出"无原文"这个特征 ⇒ 板上的 `-3` 与
"abi 不匹配"在**形态上**不同，下一轮应优先怀疑 `tu_CreateDevice` 内部而非 `dw_open_connection`。

---

## §68 2026-11-12 22:50：`uke` 的虚拟环境做了什么 —— 它解决的是**归因**，而且这套东西就在我们自己的树里

### 68.1 它不是一个"模拟器"，是四层，各管一件事

| 层 | 实现 | 跑的是什么 | 解决什么 |
| --- | --- | --- | --- |
| L4 主机编译器 | `UniXNU-uke/scripts/build-adreno-compiler-host.py` | pinned IR3/NIR，`--probe` 用 A740 合成顶点 fixture 往返编译。文件头一句：**"never install a rendering driver"** | shader 能不能编出来，与任何设备无关 |
| L3 主机真 Metal | `package/HostMetalDiagnostic.mm` + `scripts/check-host-metal-originals.py` | 用 **sha 钉死的原厂** `QuartzCore.framework/default.metallib`（`606f913a…`）与 `RenderBox.framework/default.metallib`（`d76883f6…`），在 Mac 自己的 GPU 上跑真 Metal。注释：**"Host-only GPU feasibility instrument; never a sheng deployment backend"** | 苹果那侧的 AIR/描述符/管线本身能不能编、能不能画 —— 与我们的 Adreno 驱动**无关**的那一半 |
| L2 QEMU 全系统 | `scripts/probe-darwinboot-xnu.py` + 原厂 iOS 系统卷（virtio 挂 `.dmg`）+ 真 XNU + 真 launchd/dyld + `offline-gpu-broker` 假加速器 | 真内核、真用户态、真 Metal 框架，只有 **GPU 后端**是假的 | 整条链（内核 publish → user-client → Metal 会话/队列/纹理/栅栏）在**没有 Adreno**的情况下可断点、可读寄存器 |
| L1 QEMU 纯机器码 | `scripts/check-xnu-panic-screen.py` | 只把 panic renderer 的**真实机器码** + fixture 内存塞进 `qemu-system-aarch64 -M virt`，逐像素比 hash、双核抢 panic。注释：**"This is not an XNU boot or a hardware-memory observation"** | 设备上**根本看不到**的东西：panic 屏、竞态、高 VA 的 PAC 位 |

### 68.2 它主要解决三个问题（按重要性）

**一、归因分离 —— 这是核心。** 一次失败有三个可能的责任方：我们的内核/驱动、苹果的用户态、真 GPU。
虚拟环境的作用是**一次只留一个变量**：

- 拿掉真 GPU（L2 的 broker 顶上）⇒ 若还失败，就跟硬件无关；
- 拿掉我们的驱动（L3 用 Mac 真 Metal）⇒ 若原厂 metallib 能编能画，说明失败在我们的驱动侧；
- 拿掉用户态（L1 只跑 renderer 机器码）⇒ 若逐像素正确，说明渲染器本身没问题。

**二、把"不可观测"变成"可观测"。** 设备上 panic 上不了屏、没有串口、不能下断点；
QEMU 里可以 `--break-symbol _exception_triage` 断在原厂二进制上，再
`--read-saved-user-register 13:100` / `14:1000` 把寄存器原样读出来 —— 这是板上无论如何拿不到的。

**三、成本。** 真机一轮 = 刷机 1.8 s + 等 90 s + 拍屏 + 人眼看屏十几分钟（我们已经误读两次：K 行空转、C 行死通道）。
QEMU 一轮 = `--timeout 60` 有界 + hash 比对，全自动、可重复。

### 68.3 三条方法论，比任何一条补丁都值钱

1. **负对照（ablation）**：每个探针跑两遍。
   `probe-original-metal-fences.py:33` 里 `if a.default_off:` 会把
   `--offline-gpu-broker`、`--native-air-init`、`--legacy-display-retire` 全部**移除**、断点改到 `_panic`，
   然后打印 **"no GPU protocol backend started"**。只有随开关变化的那部分才算证据。
2. **输入固化**：跑之前逐个 sha 校验 `test-inputs.json` 里的源码、`qualification.json` 里的后端/诊断程序、
   候选内核的 `candidate_sha256`，任一不符直接 `SystemExit('Exact actual instrument source required')`。
   **"跑的是不是那版代码"是脚本保证的，不是人记得的。**
3. **自我降级标注**：构建报告里写死 `offline_test_only: true`、`native_execution_qualified: false`；
   `PROGRESS.md:1388` 一句 —「主机负对照/并发/满队列/绕环/IRQ、QEMU 竞态/回溯…**都不是实机稳定性证明**」。

### 68.4 ⭐ 关键发现：这套 harness 在我们自己的树里，我们只用了下游

| 组件 | 我们的位置 | 状态 |
| --- | --- | --- |
| 整核 QEMU 探针 | `UniXNU/scripts/probe-darwinboot-xnu.py`（**1655 行**） | 在，且支持 `--offline-gpu-broker`(:129)、`--follow-metal-device`(:23)、`--follow-display-mode`(:24)、`--follow-display-query`(:25)、`--break-symbol`、`--read-saved-user-register`、`--capture-qemu-screen`、`--qemu-accel hvf`、`--load-apfs`、`--load-iogpu` |
| 假加速器（内核侧） | `UniXNU/drivers/AdrenoMetal/NativeOfflineGPU.cpp`（**312 行**） | 在。头部："Explicit offline GPU broker. **No normal Metal/IOAccelerator publication.** The kernel, not request bytes, selects each userclient's host namespace" |
| 线协议 | `OfflineMetal*Wire.h`（11 个） | session(v3)/queue/command/texture/depth-stencil/fence/event/function/vertex/shared-event，`OM_WIRE_MAGIC=0x5550474e` —— 与 uke 的 `GPU_WIRE_MAGIC` **同一个 magic** |
| 原厂系统卷 | `UniXNU/out/ipsw/21H461/044-76040-038.dmg`(4.8 GB) / `044-76034-038.dmg`(3.1 GB) | 在。`UniXNU-muyu-17/.cache/firmware/target-volumes/os-mount` 是指向 `/tmp/muyu-os-mount` 的软链，当前**悬空**，需重新挂载 |
| QEMU | `/opt/homebrew/bin/qemu-system-aarch64` | 已装 |

也就是说：**§67 我做的那套假 IOKit，只是这套 harness 的下游**（只替了 Turnip 的 8 个 IOKit 符号）。
上游那半——整颗内核 + 真 launchd/dyld + 真 Metal 用户态 + broker——一直在树里，我们还没启用过。

### 68.5 两层能覆盖的边界（这是选下一轮动作的依据）

| 观测点 | §67 假 IOKit | darwinboot QEMU |
| --- | --- | --- |
| `dw_open_connection` 的三条出口 | ✅ 能复现、能定点 | ✅ |
| `tu_CreateDevice` 内部 | ✅ | ✅ |
| `io_filter` 沙箱判定（`A` 行的 `r=`） | ❌ 桩直接放行 | ✅ 真内核判 |
| VM/SMMU、`mount` 阶段 | ❌ 全假 | ✅ 真内核 |
| **`MTLCreateSystemDefaultDevice` 的返回值** | ❌ 到不了 | ✅ `--follow-metal-device` 就是观测这个 |
| 按进程的 open 记录（`MUYU G`） | ❌ 单进程 | ✅ 真 launchd 起多进程 |

我们的 `-3`（`R PUB no device: Vulkan(VkResultCode(-3))`）**正落在两层交界**：
报错是 Turnip 报的（下游），但它的输入（连接是否授予、VM 是否映射）来自内核（上游）。
所以下一步正确的顺序不是继续加 printf，而是**用上游那层把同样的形状复现出来**。

### 68.6 代价与坑（uke 自己踩过的，别重踩）

1. **QEMU 的路径 ≠ 设备路径**。`PROGRESS.md:522` 明确写：QEMU board 不设 `offline-legacy-default-surface`、
   也不做 default-surface 收养，而设备上**收养是 compositor 开始 swap 的前提** ——「QEMU 的路径不能直接照搬」。
2. **TCG 看不到 PAC 行为**，要专门构造最小例（`PROGRESS.md:1073`：旧版 `EC=0x1c`，修正版正常返回）。
3. **输入重**：4.8 GB 系统卷 + 3.1 GB Data 卷、匹配的 volume UUID、`--owned-memory-size 0x20000000`。
   第一次跑要先挂载 os-mount，且 `--virtio-udif-image` 读的是 dmg 原件，不便宜。

### 68.7 结论与建议

1. uke 的虚拟环境**不是"跑得快一点的模拟器"，是归因工具**：它把「我们的代码 / 苹果的代码 / 真硬件」
   三个变量拆开，一次只验一个。
2. 它最值钱的不是任何一条补丁，是**负对照 + 输入固化 + 自我降级标注**这三条纪律。
3. 这套 harness 在我们树里完整存在（1655 行探针 + 312 行 broker + 11 个线协议 + 两卷 dmg），
   我们此前只用到了它的下游。
4. 建议下一轮（优先级）：
   - **A（推荐）**：挂载 os-mount，用 `probe-darwinboot-xnu.py --offline-gpu-broker --follow-metal-device`
     跑一次带 broker 的 boot，看 `MTLCreateSystemDefaultDevice` 在 QEMU 里返回什么、
     `MUYU G` 能否复现板上 `A n=6 / pr=2` 的形状。**零刷机**，一次约 60 s。
   - **B**：给 §67 的 harness 补上 uke 那三条纪律（负对照脚本化、`test-inputs.json` sha 固化、
     报告里写 `offline_test_only`）。纯离线，半小时。
   - **C**：录放（§67.8）。仍是最彻底的一步，但要一次刷机，放在 A 之后再决定。

---

## §69 2026-11-12 22:52–23:10：真的把 QEMU 整核跑起来了 —— 它现在卡在 **XNU 之前**，不在 `-3` 上

上一节建议的 A（`--offline-gpu-broker --follow-metal-device`）**按字面写不出来**：
`--offline-gpu-broker` 要求 `--native-air-init`，而 `--native-air-init` 与 `--follow-target-main`
被 `parser.error('Own compiler init and original launchd main are distinct observations')` 判为互斥
（`probe-darwinboot-xnu.py:212`）。两者是**两条互斥的观测路线**：
「我们自己的 init + 假加速器」与「原厂 launchd + 真内核」。所以这一轮先跑**最底层的那一格**
（UEFI → UniXNU → 装载器 → XNU），不涉及 GPU。

### 69.1 先把 §68 留下的悬空链接补上（核过的，不是猜的）

| 项 | 状态 |
| --- | --- |
| 系统卷 | `hdiutil attach -readonly -nobrowse 044-76040-038.dmg` → `/Volumes/DawnUpdate21H461.J120J121J207J208OS`（4.8 GB 那卷） |
| `os-mount` | 两处悬空链接（`UniXNU/.cache/...`、`out/muyu/tree/.cache/...` → `/tmp/muyu-os-mount`）改为指向真实挂载点 |
| `launchd` | sha256 `8cec9a3c44a443b08e977ccc5d577850316ad428c575dc01f09b018dbd5b6a25` = 探针 `:220` 里钉死的那个值，**逐字节对上** |
| 内核 | `out/muyu/kernel/kernel.macho`（53,202,944 B）sha `e627d477…` 与 `kernel.json` 的 `candidate_sha256` 一致，`slide=-0x1000000`，30 个 module entrypoint 齐全 |
| 固件 | `UniXNU.efi` = `out/muyu/package/Build/Out/AARCH64-DEBUG/`；`DarwinBoot.efi` 与 `Mu-Silicium/Build/muyuPkg/.../DarwinBoot.efi` **sha 相同**（`36160c0d…`，90112 B） |

探针的 `ROOT` 必须是**组装树** `UniXNU/out/muyu/tree`（不是 `UniXNU/`）：
`output_dir(ROOT)` 在组装树里才解析到 `out/muyu/package/Build/Out/AARCH64-DEBUG`。

### 69.2 第一跑：`DARWINBOOT: boot refused: Not Found` —— 是 harness 与源码的时间差

```
UNIXNU: xnu: handing off 0 staged kext(s) to loader
DARWINBOOT: boot refused: Not Found; firmware retained
UNIXNU: menu: entry 0 returned Not Found
```
源码定位（`drivers/DarwinBoot/.../DarwinBoot.c:694–712`）：装载器自 2026-11-08 起把内核当
**分段文件**读 —— 先读 `<path>.0`，读不到就 `goto Fail`；`.1` 读不到才认为「未分段」。
探针是 11-01 的，只 stage 了 `kernel.macho`，于是 `Part 0` 直接 `EFI_NOT_FOUND`。
**修法**：把同一份字节额外 stage 成 `kernel.macho.0`（`.1` 留空 = 「未分段内核」）。
已同时落进 `UniXNU/scripts/probe-darwinboot-xnu.py` 与组装树副本，带注释。

### 69.3 补 `.0` 之后：一路走到退出 boot services，然后**在装载器自己的跳转里**数据中止

```
DARWINBOOT: ARM64 va=0xFFFFFFF00427C000 pa=0x4807C000 args=0x4C418000 entry=0x4B268488 owned=0x10000000
DARWINBOOT: video base=0x0 display=0 row=0 width=0 height=0 depth=0
DARWINBOOT: command line: -v serial=3 debug=0x14e kt_fixed_zones=120
DARWINBOOT: inputs validated; exiting boot services
Synchronous Exception at 0x000000007FB26E4C
```
寄存器（`gdb-stop.json`）：

| 寄存器 | 值 | 读法（**⚠️ 此列最初读错，已在 §69.8 更正**） |
| --- | --- | --- |
| `ESR_EL1` | `0x96000005` | EC `0x25` 数据中止、ISS `0x05` = 一级转换失败 ✔ 这半句对 |
| `FAR_EL1` | `0xe643fff8` | 不是内核入口 `/args`；真身是 `Display_Reserved` 末尾（§69.8） |
| `TTBR1_EL1` | `0` | 当时读成「内核映射还没装」——其实**装载器根本没走到装页表**，它死在控制台输出里 |
| `SPSR_EL1` / `ELR_EL1` | `0x200002c5` / `0x7fb26e4c` | EL1h，PC 在固件映像里 ✔ |

⇒ 当时的结论「装载器在装 TTBR1 之前碰了一个内核 VA」**是错的**：反汇编 + map 文件证明 PC 落在
**控制台输出函数**里（`DarwinBoot.map`：`DiagDraw 0x4AE8`、`SerialPortWrite 0x4D78..0x4F98`，
故障点 `0x4E4C` 正在其中；`DarwinEnter` 在 `0x6E000`，一次都没执行）。真根因见 §69.8。

### 69.4 负对照：把 DarwinBoot 摘掉会怎样

`MUYU_NO_DARWINBOOT=1`（本轮新加的开关，默认关闭；只删 toml 里的 `[[driver]]` 行）：
```
UNIXNU: xnu: no loader protocol installed; UniXNU does not provide one
UNIXNU: menu: entry 0 returned Unsupported
```
⇒ UniXNU **自己没有内核装载器**，`type="xnu"` 必须有人提供 loader protocol。
设备上提供它的是 **Mu-Silicium 固件里的 DXE 驱动**（`muyu.dsc` 编了 `DarwinBootPkg`），
不是 ESP 里的文件 —— 这解释了为什么设备 ESP 里看不到 `DarwinBoot.efi` 也能起（§69.5）。

### 69.5 ⚠️ 一个没解决的矛盾（下一轮先打这一枪）

| 事实 | 出处 |
| --- | --- |
| 设备 ESP（`payload_sha256 de27ee57…`，72 MiB）**只有** `kernel.macho`，没有 `kernel.macho.0` | `mdir -a -i payload/payload.img ::/EFI/UniXNU/Payloads`（5 个文件） |
| 源码里 `Part 0` 读不到就 `goto Fail` | `DarwinBoot.c:706–712` |
| 但 `vkd7` 确实起了内核（屏上有 `MUYU INIT q=6`） | §65 屏照 |

三者不可能同时成立。两个 10 分钟能定案的实验：
1. **在设备上打一行**：`ReadFile` 失败时 `Print (L"DARWINBOOT: miss %s %r\n", Path, Status)` ——
   一看就知道设备上到底读的是哪条路径（`vkd8`）。
2. **在 QEMU 侧换一种装法**：不 stage `.0`，而是把 `payload.img` 直接当 virtio 盘挂进去，
   看同样的 `Not Found` 是否复现。若复现，说明设备的 DarwinBoot **二进制不同**（尽管 sha 相同，
   可能被固件以别的 PCD/入口编过），那就是又一条「同名不同物」。

### 69.6 harness 自身的两个毛病（都会把真错误藏起来）

1. **后处理太脆**：停止点不是预期那个时，`report['memory']` 里没有 `_native_hid_*` 的键，
   `probe-darwinboot-xnu.py:1189` 抛 `ValueError: non-hexadecimal number`，
   **把真正的失败（`Not Found` / 数据中止）盖在 traceback 下面**。建议改成
   「缺键就跳过并在报告里写 `unavailable`」。
2. **探针与装载器时序错配**：探针 11-01、装载器 11-08 之后改了内核读取协议，没人同步 —— 
   这正是 §67 那个「harness 只在写它那天成立」的翻版。

### 69.7 结论

1. ~~上游这层卡在装载器装页表之前~~ —— **本节的结论文已作废，见 §69.8**：
   这一节当时把「控制台写显存失败」误读成「装 TTBR1 之前碰内核 VA」。
2. 真正的一格是：DarwinBoot 把控制台镜像到 mu 的**静态** `Display_Reserved`（`0xE3940000` + `0x2B00000`），
   QEMU 默认 1 GiB 内存里那块地址不存在，于是**任何一次打印**都数据中止。
3. 价值判断（仍然成立）：这一步把「QEMU 里到底能不能跑到用户态」从「不知道」变成「差一个环境参数」，
   而不是继续往设备上刷打印 —— §65 已经证明刷机失败模式不可复现。

### 69.8 ⭐⭐ 自我更正 + 真根因 + **XNU 17 在 QEMU 里跑起来了**（PASS）

#### 69.8.1 反汇编把 §69.3 的结论推翻

崩溃 PC `0x7FB26E4C` = DarwinBoot 基址 `0x7FB22000` + RVA `0x4E4C`。拿
`Mu-Silicium/Build/muyuPkg/DEBUG_CLANGPDB/AARCH64/DarwinBootPkg/Driver/DarwinBoot/DEBUG/DarwinBoot.map`
对表：

| RVA | 符号 |
| --- | --- |
| `0x4AE8` | `DiagDraw`（+0x1C8） |
| `0x4CB0` | `AdvanceNewLine` |
| `0x4D78..0x4F98` | **`SerialPortWrite`** ← 故障点 `0x4E4C` 落在里面 |
| `0x587C` | `Print` |
| `0x6E000` | `DarwinEnter`（**一次都没执行**） |

出错指令是 `ldr w9,[x23]`，`x23 = x8-8`，`x8 = x19+x24`；寄存器 `x19=0xE3940000`、`x24=0x02B00000`。
也就是说它在写一块 45 MB 区域的**最后一个 cell**。

#### 69.8.2 这块区域是编译期常量，不是运行期 framebuffer

`Mu-Silicium/Platforms/Xiaomi/muyuPkg/Library/MemoryMapLib/MemoryMapLib.c:37`：

```c
{"Display_Reserved", 0xE3940000, 0x02B00000, AddMem, MEM_RES, SYS_MEM_CAP, Reserv, WRITE_THROUGH_XN},
```

**地址和长度逐位相同。** DarwinBoot 链接了这张 muyu 的静态内存映射表（同一张表也被
`Silicon/Silicium/SiliciumPkg/Drivers/SimpleFbDxe/SimpleFb.c:97-98` 用来找显存，两处都是
`LocateMemoryRegionByName("Display Reserved")` + `("Display_Reserved")` 连查两次）。
所以：**控制台把输出镜像到「真机上存在的显存」，而 QEMU 里这个地址不存在** ⇒ 一打印就中止。

三个旁证：
1. 加不加 `--qemu-ramfb`，`x19/x24` **完全一样**（`0xE3940000/0x02B00000`）；
   若真是 GOP framebuffer，两次就该不同（ramfb 那次 video base 是 `0x13C7A0000`）。
2. 这正是 `` 那条 DEBUG 一个字都没上串口的原因 —— 打印本身炸了，不是串口不通。
3. `0xE3940000..0xE6440000` 完全在 QEMU 默认 1 GiB RAM（`0x40000000..0x80000000`）之外。

#### 69.8.3 修法：只加一个参数 `--qemu-memory-mib 4096`

4 GiB 时 RAM 是 `0x40000000..0x140000000`，`0xE3940000` 落进 RAM，控制台写进去只是脏数据。

```bash
cd UniXNU/out/muyu/tree
python3 scripts/probe-darwinboot-xnu.py --kernel ../kernel/kernel.macho   --prelink-report ../kernel/kernel.json --armvirt --qemu-ramfb   --qemu-memory-mib 4096 --timeout 120 --output build/darwinboot-xnu-4g
```

结果（12 秒，`exit=0`）：

```
PASS: real UEFI -> UniXNU -> DarwinBoot -> active native 4K XNU translation
Observed _panic; functional iPadOS remains unverified
```

| 项 | 值 | 意义 |
| --- | --- | --- |
| `stop_symbol` | `_panic` | **不是超时、不是异常中止**，是 XNU 自己调的 `_panic` |
| `pc` | `0xfffffff007a752e0` | 内核地址空间 |
| `TTBR1_EL1` / `TTBR0_EL1` | `0x4c41d000` / `0x4c41c000` | ⭐ **内核页表已装**，4 KiB 粒度（`TCR 0x22a51a251a`） |
| `ESR_EL1` / `SPSR` | `0x96000005` / `0x60400204` | 发生在 **EL0** 的一次数据中止触发了 panic |
| `x0` | `0xfffffff0070e22ab` | panic 的格式串 `"%s at pc 0x%016llx, lr ..."` |
| `x1` = `ELR_EL1` | `0xfffffff004a92828` | 出事的是用户态那条 pc |
| `_page_shift` / `_PAGE_SHIFT_CONST` | `12` / `12` | 4 KiB 线在 QEMU 里被真正认下来 |

`bounded_timeout_interrupted = False` —— 所以这次是**真跑到内核、真 panic**，不是跑满 120 s 被截断。

#### 69.8.4 这一步买到了什么

1. **`UEFI → UniXNU → DarwinBoot → XNU(4K) → panic` 这条链在 QEMU 里第一次完整成立**，
   而且是「真内核、真 4 KiB 翻译、真 `_panic`」，12 秒一轮、零刷机。
2. §69.6 说的「探针后处理太脆」也随之消失：停止点是预期那个，`_native_hid_*` 有值了。
3. **还没到用户态**：现在 panic 在 EL0 取指/取数（没有 root 设备、没有 launchd）。
   要让 `--follow-target-main --follow-metal-device` 有意义，得先给它一个根：
   `--virtio-udif-image 044-76040-038.dmg` + `--virtio-mmio-fdt`（QEMU 生成的真实 FDT）
   + `--root-match-volume-uuid` + `--load-apfs`。
4. 也就是说：`-3` 的 QEMU 复现**只差「把系统卷挂成 virtio-blk 并让内核认成 root」这一格**，
   不再是「差一个页表安装」。

### 69.9 §69.8.4 的第 3、4 条**当天就被证伪**：root 这一格不是参数，是组件

§69.8 收尾时写「只差把系统卷挂成 virtio-blk」。实测第一步就撞墙，而且墙有三层。

#### 69.9.1 第一层：本板内核里没有 Virtio MMIO 块设备

```
$ python3 scripts/probe-darwinboot-xnu.py ... --virtio-udif-image 044-76040-038.dmg \
    --virtio-mmio-fdt build/fixtures/armvirt-virtio.dtb \
    --root-match-volume-uuid 04769D26-... --load-apfs
Cannot resolve _native_virtio_ready
```

`nm ../kernel/kernel.macho` 里 `_native_virtio_*` **一个都没有**（`_native_apfs_*`、`_native_hid_*`、
`_native_iogpu_*` 都在）。原因在 `board.toml` 的 `[kernel] archives`：本板编了
`build-arm-driver.py RamBlock`（Data seed 的 RAM 块），**没编** `VirtioMmioBlock`。对比：

| 板 | virtio 归档 |
| --- | --- |
| `UniXNU-sheng` | `build-arm-driver.py NativeVirtioBlock` + `build-native-virtio-bindings.py --component NativeVirtioBlock` |
| `UniXNU-muyu-17`（本板） | 无 |

而 `scripts/build-native-virtio-bindings.py` 与 `build-virtio-mmio-bindings.py` 都在组装树里，
两者第一件事都是读 `build/storage-abi/interface-audit.json`：

```python
audit = json.loads((ROOT / 'build/storage-abi/interface-audit.json').read_text())
if not audit['live_native_4k'] or audit['method_slots'] != 190 or ...:
    raise SystemExit('First qualify original live storage metadata and its real base functions')
```

本板 `build/storage-abi/` 只有 `headers.json` + `include/`，**没有 interface-audit.json**，
原因写在 `board.toml` 注释里：「Upstream's build-native-virtio-bindings.py would need the live
storage snapshot, which this board does not have」。要补就得走 `UniXNU-qemu/scripts/build-storage-abi.py`
那条链（三步：本板 kernel 候选 + `_native_apfs_probe_checkpoint` 的 live 观察 + OSMetaClass DWARF），
再补 VerifiedRoot 卷策略包。**这不是「加一个参数」，是一次组件移植。**

#### 69.9.2 第二层：`--load-apfs` 在本内核里根本到不了

去掉磁盘，只加 `--load-apfs`（12 秒）：

```
Observed _panic; functional iPadOS remains unverified
Unexpected _panic before _native_apfs_probe_checkpoint; inspect captured state
```

断点 `_native_apfs_probe_checkpoint` 在 `bsd_init.c` 里，**一次都没执行**。所以「挂 root」这条路的
前半段（APFS kext 加载）在我们当前的内核里也还没打通。

#### 69.9.3 第三层（真正的墙）：CoreTrust +0x79a4 的野指针

五次跑，五个配置，异常点**逐位相同**：

| 配置 | ELR_EL1 | FAR_EL1 | ESR_EL1 |
| --- | --- | --- | --- |
| 4 GiB 基线（§69.8 PASS） | `0xfffffff004a92828` | `0x0` | `0x96000005` |
| + `--load-apfs` | 同上 | 0 | 同上 |
| + `--trust-cache`（设备原厂包） | 同上 | 0 | 同上 |
| + `--osenvironment main` | 同上 | 0 | 同上 |
| + `--amfi-platform-only` | 同上 | 0 | 同上 |

- `0xfffffff004a92828` 落在 `__PLK_TEXT_EXEC`（`va=0xfffffff004920000`），所属模块是
  **`com.apple.kext.CoreTrust`**，偏移 **+0x79a4**（模块起点 `0xfffffff004a8ae84`）。
- 解码（capstone，按文件偏移 `0x1a0f000 + (va - 0xfffffff004920000)`）：

```
0xfffffff004a9280c: stp  x22, x21, [sp, #-0x30]!     ← 函数入口
0xfffffff004a9281c: mov  x19, x3
0xfffffff004a92820: mov  x20, x2
0xfffffff004a92824: ldr  x21, [x3]
0xfffffff004a92828: ldr  x8, [x0]                     ← 故障（FAR=0 ⇒ x0=NULL）
0xfffffff004a9282c: ldr  x8, [x8, #0x118]
0xfffffff004a92830: blr  x8                            ← 虚调用
```

  即：**对一个 NULL 对象做 C++ 虚调用**（先取 vtable，再取 `+0x118` 槽）。
- 该函数有 17 个调用点，**全部在 CoreTrust 自己内部**（`+0x7a80/+0x7b4c/+0x7bb0/+0x7c30/+0x7d58/+0xd91c/…`），
  没有一个是 AMFI 或内核调用进来的。
- `_panic` 的实参也确认了：`x0` = `"%s at pc 0x%016llx, lr 0x%016llx (saved state: %p%s)…"`、
  `x19` = `"Kernel data abort."`，所以串口没字不是「没打印」，是 panic 还没开印。

#### 69.9.4 把断点打在故障指令上，抓到「第一次」现场

用 `gdb-stop.py` 直接把停止点设在 `0xfffffff004a92828`（8 秒）：

```
pc = 0xfffffff004a92828      x0 = 0xfffffff9e43db980     x30 = 0xfffffff004a987a4
x1 = 0xfffffff0042d8cd6      x2 = 0xfffffff0ac2ffae0     x3 = 0xfffffff0ac2ffa88
```

- `x30 = CoreTrust + 0xd920`，与静态扫描命中的 `0xfffffff004a987a0 bl` 一致 —— 调用链闭合。
- 但这一次 `x0` **不是 0**，而是一个高位被污染的指针 `0xfffffff9e43db980`（若按 4 GiB 线性映射取
  0xfffffff0 段，它落在 RAM 之外）。⇒ 这行指令**至少被执行两次**：第一次带这种「非平坦」指针，
  panic 那次才是 `x0=0`。**两者是同一个 bug 的两个实例，不是两个 bug。**

#### 69.9.5 已经排除的四个原因

| 假设 | 测试 | 结果 |
| --- | --- | --- |
| 缺 trust cache（我们 stage 时把它删了） | 补 `--trust-cache fw/trust-21H461/21H461-original-trust.bundle` | 无效，异常点不变 |
| `hw.osenvironment` 不对 | `--osenvironment main` | 无效 |
| AMFI 平台码缺失 | `--amfi-platform-only` | 无效 |
| APFS 加载改变了时序 | 加/不加 `--load-apfs` | 无效 |

顺带查清一条**真实的 staging 缺口**（虽然它不是本次 panic 的原因）：

- 设备 payload（`UniXNU/out/muyu/payload/payload.img`）里 **有** `kernel.macho.trustcache`，61,356 B，
  sha256 `7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7`，与
  `fw/trust-21H461/21H461-original-trust.bundle` **逐字节相同**。
- 探针的默认行为是 `trust_sidecar.unlink(missing_ok=True)`（`probe-darwinboot-xnu.py:325-328`），
  也就是**不传 `--trust-cache` 就不带 trust cache 上机**，与设备的真实 payload 不一致。
  以后所有「要和设备同形」的 QEMU 跑，都必须显式带这个参数。
- DarwinBoot 会校验它（`DarwinBoot.c:763-775`），失败会打 `TC off=… ok=…` 并 `DiagHang`；
  本次串口**没有** `TC` 行 ⇒ trust cache 被接受，`CopyMem`+ADT 两 u64 写入（`:858-862`）走的是成功分支。

#### 69.9.6 这一轮买到什么、没买到什么

买到的：
1. 「root 只差一个参数」被证伪，连**墙在哪一层**都定位了（内核缺组件 → 缺 storage-abi 审计 → 缺 verified-root 包）。
2. QEMU 侧真正的前沿从「挂磁盘」变成了 **CoreTrust 的 NULL 虚调用**，而且它是一个可复现、
   12 秒一轮、五次一致的**单点**。
3. staging 与设备 payload 的差异（trust cache）被查清并固化。

没买到：仍然没有任何 GPU 结论；`-3` 在 QEMU 里依然不可复现，而且现在看**不是差一格，是差两层**
（CoreTrust → APFS/root → launchd → Adreno 客户端）。

#### 69.9.7 下一步的两个候选（按性价比排序）

1. **先打 CoreTrust**（推荐）：它挡住一切下游。做法是把断点设在 `CoreTrust+0xd920`（调用点）而不是
   故障指令，看它从那 17 个调用点里的哪一个进来、`x2/x3` 指向的结构是什么
   （`gdb-stop.py --read-memory`/`--read-indirect` 已支持）。只要拿到「它想 lookup 什么」，
   就能反推缺的是哪个输入（目前最像的是 `AppleKeyStoreState`/SEP 相关的对象 —— QEMU 里没有 SEP）。
2. **再考虑 virtio 组件移植**：即使 CoreTrust 通了，磁盘路还要 storage-abi 审计 + verified-root
   卷策略 + 内核重编。工时可观，而且**在 CoreTrust 未通之前完全不可验证**，所以排在后面。

### 69.10 DT 身份对齐（PASS）+ 仍然撞同一堵墙（负结果）+ HVF 不可用

§69.9 把「缺 trust cache」排除后，剩下的最大嫌疑是**设备树身份不全**：设备 ADT 有一整套安全属性，
我们的 QEMU DT 是 `qemu-entry-fixture`，一个都没有。这一节把它补齐，然后看墙动不动。

#### 69.10.1 设备 ADT 与我们的 ADT 差什么

设备 payload 里 `kernel.macho.adt`（13,992 B）是 Apple 的 ADT 格式，不是 FDT：
节点 = `u32 属性数 + u32 子节点数`，每个属性 = `32 字节名字 + u32 长度 + 值（8 字节对齐）`，
**全部小端**（`DarwinBoot.c:290-340` 的 `AdtNode` 就是这么读的）。按这个格式解析出设备 `/chosen` 的全部属性：

| 属性 | 设备值 | 我们原来 | 说明 |
| --- | --- | --- | --- |
| `osenvironment` | `"main"` | 无（可选参数） | 内核 `hw.osenvironment` |
| `boot-manifest-hash` | `5fd2fc13…6f23faca`（48 B） | **无** | AMFI/信任评估用的启动身份 |
| `boot-uuid` | `04769d262ca04825b443e2e5f88300f2` | 无 | 与挂载的 21H461 系统卷 UUID 一致 |
| `apfs-preboot-uuid` | `8846fe7829104297af97d59bb6f8fc16` | 无 | APFS 预启动卷 |
| `crypto-hash-method` | `"sha2-384"` | 无 | 有独立参数 |
| `certificate-security-mode` | `1` | **无** | ⭐ AMFI 安全模式 |
| `certificate-production-status` | `0` | 无 | ⭐ |
| `effective-security-mode-sep` | `1` | 无 | ⭐ SEP 侧 |
| `effective-production-status-ap` | `0` | 无 | ⭐ AP 侧 |
| `target-type` / `chip-id` | `"J120"` / `0x0207` | 无 | 机型/芯片 |
| `home-button-type` / `sleep-disabled` / `embedded-panic-log-size` | `2` / `1` / `0x380000` | 无 | 外设/电源 |
| `PackedRAMDisk` / `GpuAddon` / `root-matching` / `nvram-*` / `artwork-*` | 有 | 部分有 | 与本次无关 |

结论：我们的 DT 与设备的差距不只是「少了几个键」，而是**整套启动身份**。

#### 69.10.2 新增两个工具开关（都只接受真实输入，不造值）

`scripts/qemu-entry/make-dt.py` 与 `scripts/probe-darwinboot-xnu.py` 同时加（`UniXNU/scripts/` 与组装树两份都改了）：

```python
parser.add_argument('--boot-manifest-hash', help='Actual /chosen/boot-manifest-hash bytes (96 hex characters) taken from the target ADT; the original AMFI trust path reads this identity')
parser.add_argument('--chosen-property', action='append', default=[], metavar='NAME=HEX', help='Extra /chosen property as exact bytes, taken from the target ADT (repeatable; never fabricated)')
```

- 两者都只做**逐字节搬运**：值必须来自 `payload.img` 里那份 ADT，脚本不生成默认值。
- `--boot-manifest-hash` 与旧的 `--conflicting-boot-manifest`（`A5×48` 负对照）**互斥语义**保留：
  给了真实值就不发伪造值。

#### 69.10.3 用设备全量身份再跑一次：墙没动

```
exit=1
Observed _panic; functional iPadOS remains unverified
Unexpected _panic before _native_apfs_probe_checkpoint
ELR 0xfffffff004a92828   FAR 0x0   ESR 0x96000005
```

ADT 里确认已含 `certificate-security-mode` / `apfs-preboot-uuid` / `target-type`
（`strings` 命中 3 个键），异常点与 §69.9 **逐位相同**。

⇒ **启动身份不是这堵墙的原因。** 这是本轮最干净的负结果 —— 现在可以把它从嫌疑名单里划掉，
而不是继续猜。

#### 69.10.4 新的首要线索：寄存器里的「错位半字节」

三次抓现场（故障指令断点 / 调用点断点），被污染的指针有一个共同形状：

```
x0 = 0xfffffff9_e43db980    ← 故障断点（第一次命中）
x0 = 0xfffffff2_420b8980    ← 调用点断点
x2 = 0xfffffff6_351ebae0
x29 = 0xfffffff6_351ebb80    ← 帧指针
```

正常内核指针是 `0xfffffff0_xxxxxxxx`（4 GiB 线性映射）。上面这些**只有 bits 35:32 不对**，
低 32 位看着完全正常（`0x420b8980` 就是 1.03 GB 的物理地址）。而且 `x1` 同时是干净的字符串指针
（`0xfffffff0042d8cd6`，指向 CoreTrust 的 cstring 表 `"boot-type"`）。

这个形状不像「随机踩内存」，更像**某个标记位被写进了指针的中间几位**。
下一步应该查：这几位是不是 `vm_map` 的 tag、还是 `kalloc` 的 zone 标记、
还是我们 4 KiB 变体（`source-4k`，`TCR 0x22a51a251a`）在 `pmap` 里对 PTE 高位做了什么。

#### 69.10.5 HVF 真加速器不可用（工具边界）

想用 HVF 绕开 TCG 的内存模型差异：

```
$ probe ... --qemu-accel hvf --qemu-vmm-present
RuntimeError: QEMU rejected breakpoint
```

两种停止点（`_panic` 与 `_native_apfs_probe_checkpoint`）都被拒。`gdb-stderr.log` 只有
`terminating on signal 15`，即 QEMU 在 HVF 下不接受 gdbstub 的软件断点。
⇒ **本 harness 目前只能在 `-accel tcg` 下工作**，这条不要再试。

#### 69.10.6 修正后的下一步

1. **查「错位半字节」**（新的第一优先）：在故障指令前一条 `ldr x0,[x3]` 处下断点，看 x0 是哪次
   load 带进来的；同时 dump 该指针所在的 `vm_map` 条目。这一步不需要磁盘、不需要刷机、12 秒一轮。
2. 若确认是 pmap/4K 变体的标记位问题，那就回到「4 KiB 内核变体」这条线上查，而不是往 QEMU 环境上找。
3. 磁盘/root 与 virtio 组件移植仍然排在后面 —— 在 CoreTrust 未通之前它们**无法验证**。

### 69.11 ⭐⭐ CoreTrust 那堵墙被打穿了：**缺的是 `/options` 节点**（附 §69.10.4 的自我更正）

§69.10 结尾把线索押在「指针 bits 35:32 被标记位污染」上。这一节先把那条**推翻**，再用一次单变量
实验定位真因，然后顺着它一路把内核推到 `bsd_autoconf`/`cpu_start` —— 本轮结束时墙已经换到
「ACC Fast IPI」，比 CoreTrust 晚了整整两个数量级的内核进度。

#### 69.11.1 先把「错位半字节」这条线关掉（算术错，不是发现）

§69.10.4 的原话是「正常内核指针是 `0xfffffff0_xxxxxxxx`（4 GiB 线性映射）」。**这句是错的**：

```
0xffffffff_ffffffff - 0xfffffff0_00000000 + 1 = 0x10_00000000 = 64 GiB
```

arm64 的 TTBR1 窗口按 `TCR_EL1=0x22a51a251a` 解出来是 `T1SZ=26` ⇒ 顶部 `2^38` = **256 GiB**
（`0xffffffc0_00000000` 起），XNU 的 `VM_MIN_KERNEL_ADDRESS=0xfffffff0_00000000` 只是它里面的一段
**64 GiB** 子区间。所以：

| 值 | §69.10 的判断 | 实际 |
| --- | --- | --- |
| `0xfffffff2_420b8980`（调用点 x0） | 「被污染」 | **合法内核 VA**（`+0x2_420b8980`，KVA 内 9.7 GB 处） |
| `0xfffffff6_351eba60`（sp / x29） | 「被污染」 | 线程内核栈，**帧链自洽**（`sp+0x120=x29`）⇒ 只能是真地址 |
| `0xfffffff9_e43db980`（故障指令 x0） | 「被污染」 | 同样在 KVA 内；它非 0 ⇒ 那次**不是** NULL 解引用 |

⇒ 「bits 35:32 被标记位污染」**不成立**，`pmap`/4 KiB 变体的标记位也不用查了。真正的缺陷是
**某个节点指针真的是 0**，而 §69.9.4 抓到的那次 `x0≠0` 只是同一指令的**另一个实例**（这条当时判断对了）。

#### 69.11.2 从字符串反推 CoreTrust 在干什么

故障函数（`+0x79a4`）所属的调用者 `0xfffffff004a98710` 里，全部报错串是：

```
'panic: failed to get chosen node'
'panic: failed to get options node'
'panic: failed to get defaults node'
'panic: failed to get product node'
'panic: failed to get manifest properties'
```

而它调用的那个 helper（就是我们断到的 `+0x79a4`）签名是 `(node, "boot-type", &out, &len)`：
先 `ldr x8,[node]` 取 vtable、`ldr x8,[x8,#0x118]` 取槽、`blr x8` 做虚调用，再 `safeMetaCast` 到
某个 `OSMetaClass`。⇒ **CoreTrust 在读设备树的 5 个节点**（`chosen`/`options`/`defaults`/`product`/
`chosen/manifest-properties`），helper 就是「对某节点取某属性」。节点指针为 NULL 时它**不检查**，
直接 `ldr x8,[x0]` ⇒ `FAR=0`、`ESR=0x96000005`。

把设备 ADT（`payload.img:EFI/UniXNU/Payloads/kernel.macho.adt`，13,992 B，sha `8d09b938…`，
按 `DarwinBoot.c:290-356` 的格式解：节点 = `u32 属性数 + u32 子节点数`，属性 = `32B 名 + u32 长 + 值`，
**4 字节对齐**、小端）和我们那份 DT 并排一比，差得很干净：

| 节点 | 设备 ADT | 我们（§69.10 全量身份跑） |
| --- | --- | --- |
| `/chosen` | 有（22 个属性） | 有（17 个） |
| `/chosen/memory-map` | 有 | 有 |
| `/chosen/manifest-properties` | **存在但空** | 有（空） |
| `/defaults` | 有（`sleep-disabled`） | 有（空） |
| `/product` | 有（`product-name`/`model`/`artwork-*`…） | 有（2 个属性） |
| **`/options`** | **存在但空** | **整个节点不存在** ❗ |

`make-dt.py` 里 `node('options')` 只在 `--nvram-proxy` 时才发（`:157`），所以**所有**此前的跑都没有
`/options`。设备上 `iBoot` 永远会建这个空节点，CoreTrust 也就从来不检查它。

#### 69.11.3 单变量实验：`/options` 就是那堵墙

为了不让「nvram 属性」和「options 节点」混在一起，做了三组对照（都是 12–13 秒）：

| # | 配置 | 结果 |
| --- | --- | --- |
| A | 全量身份（§69.10） | CoreTrust `ELR 0xfffffff004a92828` / `FAR 0` |
| B | 全量身份 + `nvram-bank-size/count/current-bank`（**只补属性、不加节点**） | **完全同 A**（同 41 条 `_kprintf`，`FAR 0`） |
| C | 全量身份 + `--copy-node-from vendor.adt:options`（只补空节点、不加 nvram 属性） | CoreTrust **过了**；改撞 `IONVRAMCHRPHandler creation failed` |

⇒ **B 排除 nvram 属性，C 证明 `/options` 单独就够让 CoreTrust 过。** 而 C 的新 panic 说明
`/options` 和 nvram 那块是**两件事**：`IODTNVRAM` 要 `/chosen/nvram-proxy-data`（真 CHRP 镜像）才建得出
handler。`--nvram-proxy` 恰好一次把两件都做了（`empty_nvram_proxy()` 既发属性又发节点），这就是它一开
就能越墙的原因。

#### 69.11.4 越墙之后是一串「缺输入」，每格 13 秒（这一轮买到 3 格）

| 顺序 | panic / 现象 | 缺什么 | 处理 |
| --- | --- | --- | --- |
| ① | `AMFI: no amfi-only-platform-code device tree node!`（`AppleMobileFileIntegrity`，`+0x1f884`） | `/chosen/amfi-only-platform-code`（设备值 `01000000`） | 已有开关 `--amfi-platform-only` |
| ② | `CPU configuration in device tree is invalid`（`AppleARMCPU.cpp:111`） | `/cpus/max_cpus`（u32） | 新开关 `--node-property cpus:max_cpus=…` |
| ③ | 同上（更细的 `*** Missing '%s' property for CPU %d`）×2 | 每 CPU 的 `interrupts`（`max_cpus<2` 时长度必须是 8） | 见下：用 `no-aic-ipi-required` 合法跳过 |
| ④ | `Platform does not support ACC Fast IPI` | QEMU 没有 Apple ACC 快速 IPI | **新的墙**（不是 DT 数据问题） |

③ 的处理值得单独说：`AppleARMCPU::_validateConfiguration` 只在 `[cpu+233]`（「用 AIC IPI」）为真时
才校验每 CPU 的 `interrupts`；而 `start()` 里读 CPU 节点的 `no-aic-ipi-required` 属性成功时会把该位清零。
QEMU virt + GICv3 **本来就没有 AIC**，所以发这个属性是**如实描述**、不是造数据：

```
--node-property cpus/cpu0:no-aic-ipi-required=01000000
```

这一格一开，内核打印从 43 条跳到 **86 条**，并一路走完：

```
corecrypto_kext_start completed successfully
FIPSPOST_KEXT … all tests PASSED
SK: netagent is %s by default for this platform
DLIL: running SUM16 self-tests … PASSED
Skywalk: sk_copy64_8 / sk_copy32_8 / sk_zero_128 / memcmp mask … PASSED
bsd_autoconf: calling kminit            ← BSD 栈起来了
[…]pset_find(cluster_id=%d) returned pset %d
[…]cpu_id %p cluster_id %d cpu_number %d is type %d
cpu_start() cpu: %d
```

#### 69.11.5 新增/改动的工具（都只搬真实字节，不造值）

| 位置 | 改动 |
| --- | --- |
| `scripts/unixnu/devicetree.py` | 新增 `parse_adt()` / `build_adt()`（互为逆，`build_adt(parse_adt(x))==x` 自检过）、`set_node_property()`、`find_adt_node()`、`append_adt_node()` |
| `scripts/qemu-entry/make-dt.py` | 新增 `--node-property PATH:NAME=HEX`、`--copy-node-from ADT:PATH`（整枝从设备 ADT 逐字节搬运，重名即报错） |
| `scripts/probe-darwinboot-xnu.py` | 透传上面两项；新增 `--log-kprintf`（把 `_kprintf` 接到既有的 `--cstring-log-address`，上限 128 条） |
| `scripts/qemu-entry/gdb-stop.py` | 未改 |

`--log-kprintf` 是本轮最省时间的一件：内核没有串口控制台（`gdb-uart.log` 停在
`DARWINBOOT: inputs validated; exiting boot services`），但 `_kprintf` 的 x0 就是格式串，
这条通道把「哪一条校验失败」直接读出来（③ 的那两行 `*** Missing '%s'…` 就是这么拿到的）。

#### 69.11.6 现在的最优命令（86 条内核打印，13 秒）

```bash
cd UniXNU/out/muyu/tree   # 唯一合法 ROOT
P=/opt/homebrew/bin/python3.12
$P scripts/probe-darwinboot-xnu.py --kernel ../kernel/kernel.macho \
  --prelink-report ../kernel/kernel.json --armvirt --qemu-ramfb --qemu-memory-mib 4096 \
  --timeout 120 --log-kprintf --nvram-proxy --amfi-platform-only \
  --node-property cpus:max_cpus=01000000 \
  --node-property cpus/cpu0:no-aic-ipi-required=01000000 \
  --osenvironment main --crypto-hash-method sha2-384 \
  --boot-manifest-hash 5fd2fc13…6f23faca \
  --chosen-property boot-uuid=04769d262ca04825b443e2e5f88300f2 \
  --chosen-property apfs-preboot-uuid=8846fe7829104297af97d59bb6f8fc16 \
  --chosen-property certificate-security-mode=01000000 \
  --chosen-property certificate-production-status=00000000 \
  --chosen-property effective-security-mode-sep=01000000 \
  --chosen-property effective-production-status-ap=00000000 \
  --chosen-property target-type=4a31323000 --chosen-property chip-id=07020000 \
  --chosen-property home-button-type=02000000 --chosen-property sleep-disabled=01000000 \
  --chosen-property embedded-panic-log-size=00003800 \
  --output build/db-noaic
```

产物：`build/db-noaic/{gdb-stop.json,gdb-uart.log,esp/…}`；`cstring_events` 86 条即内核打印全序列。

#### 69.11.7 这一轮买到/没买到

买到：

1. §69.10.4 的「污染指针」被证伪（64 GiB KVA 算术），省掉一整条岔路。
2. **CoreTrust 的墙 = 缺 `/options` 节点**，单变量对照三组，因果闭合。
3. 内核在 QEMU 里从「CoreTrust 起来前就死」推进到 `bsd_autoconf` + `cpu_start`（86 条打印，
   含 Skywalk/DLIL 自检全绿），仍然 13 秒一轮、零刷机。
4. 工具补了 4 个（`parse_adt`/`build_adt`、`--node-property`、`--copy-node-from`、`--log-kprintf`）。

没买到：仍然没有任何 GPU 结论。而且新墙 `ACC Fast IPI` **不是 DT 数据能填的**，是 QEMU 缺 Apple
私有加速器特性 —— 要么找到关掉它的 boot-arg（`sheng-*`/`offline-*` 里没有现成的），要么在源码里
改 CPU 启动路径，属"改内核"级别，比前几格的"补输入"贵。

#### 69.11.8 下一步（按性价比）

1. **先找 ACC Fast IPI 的关闭点**（便宜）：`ACC` 相关符号在 `kernel.macho` 里 grep `acc_fast_ipi`/
   `AppleCPUAccelerator`，看它是不是被某个 `PE_*` 变量或 boot-arg 门控；`-no_slto_panic` 那种风格
   的开关若存在，直接加参数即可，不用改码。
2. 若必须改码：优先在 **`offline-*` 诊断路径**里做（本板已有 `offline-legacy-*` 一整套先例），
   不要动真机路径。
3. 磁盘/root 与 virtio 移植仍然排在后面：`_native_apfs_probe_checkpoint` 现在**仍未到达**，
   但至少内核已经过了 BSD 初始化，`--load-apfs` 值得在 ACC 那格通了之后立刻重试。

### 69.12 ⭐⭐⭐ 三处旧结论作废；内核 GPU 链在 XNU 17 线**整条跑通**（零改码、每格 14 秒）

这一节接着 §69.11.8 的「ACC Fast IPI 要改内核」往下做。**那一步没改一行内核代码**，
而且连着推翻了三处自己的旧结论；结束时 `_native_mobile_graphics_client_checkpoint`
在 21H461 的 kernel.macho 里首次到达，原厂 `IOMobileFramebufferUserClient` 对象被真实构造、
`externalMethod` 被真实调用。

#### 69.12.1 作废清单（务必按本节覆盖旧文）

| 旧结论 | 位置 | 实测 |
| --- | --- | --- |
| 「ACC Fast IPI 是 QEMU 缺 Apple 私有加速器，必须改 CPU 启动路径」 | §69.11.8 第 1–2 条 | **错**。只是 DT 缺 `/arm-io/intc`，加现成的 `--armcpu` 即可，ACC 分支根本不再进入 |
| 「`--load-apfs` 在本内核里根本到不了 checkpoint」 | §69.9.2 整节 | **错**。CoreTrust 一过就到，`_native_apfs_load_status=0` |
| 「仍然没有任何 GPU 结论」 | §69.11.7 第 4 条 | 已过时。现在有内核侧 GPU kext/客户端的逐项实测（下文） |
| §69.11.6 的「最优命令」 | §69.11.6 | 缺 `--armcpu` 与 `sheng-pmgr=2`，**已不是最优**，用 §69.12.5 |

顺带一处：§69.11.4 表里把 `no-aic-ipi-required` 当作「跳过每 CPU interrupts」的解法仍然成立，
但那是**绕开 AppleARMCPU** 的路；加了 `--armcpu` 之后走的是 ArmCpu/GICv3 正路，两格都不再需要。

#### 69.12.2 两格配置的真实身份（别把仪器当修复）

上一轮 `db-pmgr` 的 87 条打印里，第 6/7 行是
`pe_arm_map_interrupt_controller: failed to find the interrupt-controller.`。加 `--armcpu` 之后
ACC panic 消失、`ArmCpu` 接管；但 AMFI 随即报 `AMFI: No PMGR?`。处理方式是移植层**已有的 opt-in
仪器**：

```
--kernel-extra-args "sheng-pmgr=2"     # 发布原始 ApplePMGRNub（kits/muyu/ShengPlatformPower.cpp）
```

⚠️ 这是**主动开启的诊断仪器，不是 DT 数据修复，也不是原厂行为**。另一块板的
`check-apfs-component-checkpoints.py` 明确把 `'AMFI: No PMGR?'` 当作「必须保留的原厂拒绝」
来断言；本条链为了往后走主动跳过了它。所有结论都带这个前提。

跳过后内核打印从 43 → **87 条**，一路走完
`corecrypto`/`FIPSPOST`/`DLIL`/`Skywalk` → `bsd_autoconf: calling kminit` → `pset_find`/`cpu_start`
→ `IOSurfaceRoot::installMemoryRegions()`，最后停在 `IOKitBSDInit.cpp:984`：

```
Failed to mount root device @%s:%d
```

根因很朴素：`unixnu.toml` 里 `args = "-v serial=3 debug=0x14e kt_fixed_zones=120 sheng-pmgr=2"`
**没有 `rd=`**。探针的 `rd=md0` 只由 `--native-user` 触发（`:433`），而本板
`build/native-user-fixture/` 不存在、`kernel.macho` 里也没有 `_native_user_checkpoint` 符号
⇒ **当前板根本挂不上 md0 root**。这是新的墙，属「组件缺失」，不是参数漏填。

> ⚠️ **此段已被 §69.14 部分推翻**：`_native_user_checkpoint` 根本不是内核符号，探针从 own fixture
> 的 `addresses.json['_checkpoint']`（用户态 `0x10000400c`）解析断点；`kernel.macho` 里没有它是
> 正常的，不能当故障证据。真正缺的只是没运行树里现成的 `scripts/build-native-user-fixture.py`。
> 生成器一跑，`--native-user` 就能让 md0 挂上、`vfs_mountroot()` 返回 0、走到 `load_init_program`。

#### 69.12.3 四个 checkpoint 全通（各 13–14 秒，TCG，零改码）

| 产物 | 追加参数 | 断点 PC | 关键状态 |
| --- | --- | --- | --- |
| `build/db-apfs-retry` | `--load-apfs` | `0xfffffff007728188` | `_native_apfs_load_status=0`；`registered=0`、`vfsops=0`（只加载未注册） |
| `build/db-iogpu` | `--load-iogpu` + `--break-symbol _native_iogpu_probe_checkpoint` | `0xfffffff00772818c` | `native_iogpu_load_status=0`；`kmod_start @0xfffffff004bde714` 真命中、**入口字节逐字节匹配** |
| `build/db-mg` | `--load-mobile-graphics` + 对应断点 | `0xfffffff007728190` | `native_mobile_graphics_load_status=0`；`kmod_start @0xfffffff0057a2654` 真命中、入口字节匹配 |
| `build/db-mgc` | `--mobile-graphics-client-probe` + `_native_mobile_graphics_client_checkpoint` | `0xfffffff007978084` | 见 69.12.4 |
| `build/db-mgc-fact` | 再 `+--trace-mobile-graphics-factories` | 同上 | 三个抽象工厂 `alloc` 全命中 |

三个 `bsd_init` 断点是**相邻的三条指令** `0xfffffff007728188/18c/190`，每个只在对应
`offline-*-load=1` 打开时执行 —— 所以「断点命中」和「kext 真加载」是两件事，
上表的 `load_status=0` + `actual_start_observed=true` + `original_entry_bytes_match=true`
才是证据。日志：`logs/qemu-mgc-2026-11-13.log`、`logs/qemu-mgc-fact-2026-11-13.log`。

#### 69.12.4 GPU 客户端：与另一块板的官方 checker **逐位一致**

`build/db-mgc/gdb-stop.json` 的 `native_mobile_graphics_client`：

```json
{"abstract_null_mask":7, "client_object":18446744033945591552,
 "client_vptr":18446744005089906128, "client_meta":18446744005102474160,
 "client_size":1336, "client_init":1, "client_released":1,
 "selector_results":[3758096385,3758096385,3758096385]}
```

换算成十六进制并与 `scripts/check-mobile-graphics-client-checkpoints.py` 的断言逐项对照：

| 字段 | 本次 | checker 期望 | 含义 |
| --- | --- | --- | --- |
| `client_vptr` | `0xfffffff005f34dd0` | 同值 | 原厂 vtable |
| `client_meta` | `0xfffffff006b313b0` | 同值 | 原厂 OSMetaClass |
| `client_size` | `0x538` = 1336 | 同值 | `IOMobileFramebufferUserClient` 实例大小 |
| `selector_results` | `[0xe0000001]×3` | 同值 | 三个越界 selector（90 / `0x80000000` / `0xffffffff`）被一致拒绝 |
| `abstract_null_mask` | `7` | 同值 | 三个抽象基类 `allocClassWithName` 都如实返回 NULL |
| `client_init` / `client_released` | `1` / `1` | 同值 | 具体类真构造、真 init、真 release |

`db-mgc-fact` 的 `trace_events` 补齐了工厂证据，三条 PC 与 checker 的 `factories` 表**逐位相同**：

```
0xfffffff00577b420 x0=0xfffffff006b313d8  IOMobileFramebuffer::MetaClass::alloc
0xfffffff00577b620 x0=0xfffffff006b31400  IOMobileFramebufferService::MetaClass::alloc
0xfffffff0057866d4 x0=0xfffffff006b35940  IOMobileFramebufferLegacy::MetaClass::alloc
```

而 `db-mgc` 的 trace 里 `IOMobileFramebufferUserClient::externalMethod` 命中 `x1=0x5a`（selector 90），
`x0=0xfffffff6bde21f00` 与 `client_object` **同一个指针** ⇒ 不是「断点恰好停在仪器上」，
是真实对象上的真实调用。源码在 `.cache/xnu-work/iokit/Kernel/IOUserClient.cpp:6736-6785`。

⇒ **21H461 的原厂 IOMobileFramebuffer 组件与另一块已验证板是同一份二进制，行为一致。**

#### 69.12.5 现在的最优命令（§69.11.6 作废）

```bash
cd UniXNU/out/muyu/tree                    # 唯一合法 ROOT
P=/opt/homebrew/bin/python3.12
FLAGS=$($P /tmp/chosen-flags.py build/db-chosen/esp/EFI/UniXNU/Payloads/kernel.macho.adt /device-tree/chosen)
$P scripts/probe-darwinboot-xnu.py --kernel ../kernel/kernel.macho \
  --prelink-report ../kernel/kernel.json --armvirt --armcpu --qemu-ramfb \
  --qemu-memory-mib 4096 --timeout 120 --log-kprintf --nvram-proxy \
  --amfi-platform-only --kernel-extra-args "sheng-pmgr=2" \
  --load-mobile-graphics --mobile-graphics-client-probe \
  --break-symbol _native_mobile_graphics_client_checkpoint ${=FLAGS} \
  --output build/db-mgc
```

（zsh 里 `$FLAGS` 不分词，必须 `${=FLAGS}`。本轮**没有改任何脚本**，全部是既有参数。）

#### 69.12.6 边界与下一步

诚实边界：报告首部仍是 `hardware_boot_demonstrated: false`、`os_boot_demonstrated: false`，
PASS 的含义是「real UEFI → UniXNU → DarwinBoot → active native 4K XNU translation」。
**`-3` 仍未复现** —— `-3 = VK_ERROR_INITIALIZATION_FAILED`（§22、§33.3），
是 `metal2vulkan` 用户态插件 `native_bridge_metal_open` 的拒绝理由，属 EL0 层；
本轮打通的是 EL1 内核 GPU kext 链，两者之间还隔着 **root mount → launchd → GPU 插件** 三层。

#### 69.12.7 ⚠️ 已被 §69.13 推翻：APFS **load ≠ register**（当时的负结果，优先级判断错误）

`build/db-vfsfsadd`（`logs/qemu-vfsfsadd-2026-11-13.log`）：
`--load-apfs --break-symbol _vfs_fsadd` 也到达了（`pc=0xfffffff00750ec14`，**85 条**打印，
比 checkpoint 那次的 41 条晚得多），但 `_native_apfs_registered` / `_native_apfs_vfsops`
**仍是 0**。这个 0 不是「后来又丢了」——源码把它钉死了：

```c
// .cache/xnu-work/bsd/kern/bsd_init.c:497-510
native_apfs_load_status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
mount_list_lock();
for (struct vfstable *entry = vfsconf; entry; entry = entry->vfc_next) {
    if (!strncmp(entry->vfc_name, "apfs", MFSNAMELEN)) {
        native_apfs_registered = 1;
        native_apfs_vfsops = (uintptr_t)entry->vfc_vfsops;
        break;
    }
}
mount_list_unlock();
native_apfs_probe_checkpoint();
```

⇒ 两个全局在 `offline-apfs-load` 那个 checkpoint 处**当场定型**：`load_status=0`（二进制加载
成功）而 `vfsconf` 里没有 `apfs` 条目，即 **kext 被装进内核但没有 `start`/没调 `vfs_fsadd`**。
`_vfs_fsadd` 那次命中的是别的 fs 注册，不是 APFS。所以「挂 root」前面还压着
**让 APFS 真跑 start（IOKit 匹配/驱动启动）** 这一格，比补 `rd=` 更靠前。

> ⚠️ **以上两段已被 §69.13 逐条推翻，不要按它排优先级。** 实测：`_vfs_fsadd` 的那次调用者
> 返回地址就在 APFS kext 的 `__TEXT_EXEC` 里，`vfe_fsname=b'apfs'`、函数返回 0、
> `*handle` 的 `vfc_name` 也是 `apfs`；`registered=0` 只是 **probe 跑得太早** 的时序快照。
> 保留原文是为了留下「为什么会判错」的判据链。

#### 69.12.8 ⚠️ 已被 §69.13.6 取代：下一步（当时的优先级）

1. **APFS 注册**：查 `OSKextLoadKextWithIdentifier` 与 kext `start` 的分工，看
   `com.apple.filesystems.apfs` 在本板是否需要显式 `IOService` 匹配或 `OSKextStartKext`；
   目标是让 `native_apfs_registered` 从 0 变 1。这格不需要磁盘，最便宜。
2. **root mount**：`args` 缺 `rd=`，且本板没有 `native-user-fixture` /
   `_native_user_checkpoint`。要么移植 md0 组件，要么走 `--virtio-block-image` + 双卷，
   但本板 `kernel.macho` 里也没有 `_native_virtio_read_checkpoint`，§69.9.1 的
   `interface-audit.json` 依旧缺。
3. `-3` 的复现条件没变，仍是 §22/§33.3 那套用户态链（root → launchd → GPU 插件）；
   本轮只是把「内核根本起不来」这个前置障碍清掉了，并把 GPU 客户端这一段的 EL1 证据做实。

---

## §69.13 2026-11-13 10:00：APFS 其实**注册成功了**；root 墙精确定位到 `md0 未配置`

接着 §69.12.8 的第 1 条做，**结果是把它作废**：「让 APFS 真跑 start」是一格伪需求，真正缺的是
root 内存盘。全程零改码、零刷机、TCG、每格约 14 秒，新增四个产物格。

### 69.13.1 先写判读格（跑之前定的）

| 观测 | 结论 |
| --- | --- |
| `_vfs_fsadd` 的 `vfe_fsname=="apfs"`、函数返回 0、`*handle` 的 `vfc_name=="apfs"` | APFS 真调用并成功注册 ⇒ §69.12.7 作废，`registered=0` 是时序快照 |
| `vfe_fsname=="apfs"` 但返回非 0 | 真被拒，查 `vfe_flags` / `vfe_vopcnt` |
| `vfe_fsname!="apfs"` | 命中的是别的 fs，需 `--entry-max-skips` 找后续调用 |

**静态线索（当时就被漏掉）**：`db-vfsfsadd/gdb-stop.json` 的 `frame_chain[0].lr = 0xfffffff00566a914`，
而 `kernel.json` 的 `extents` 里 `com.apple.filesystems.apfs:__TEXT_EXEC` 是
`0xfffffff005640000 + 0x1294f0` ⇒ **第一次 `_vfs_fsadd` 的调用者就是 APFS kext**，
不是「别的 fs 注册」。§69.12.7 这句话当时就是错的。

### 69.13.2 实验 A：把 `_vfs_fsadd` 真正跑完（`build/db-vfsfsadd-finish`）

与 §69.12.5 最优命令只差三处：`--load-apfs --break-symbol _vfs_fsadd --finish-function`，
再加两个**已有**的只读观测参数 `--read-entry-region 0:0:40 --read-entry-indirect 1:0:20`。

> 工具坑：`--read-entry-indirect` 要求 `--finish-function`（`gdb-stop.py:165`）；而脚本
> `:744-745` 那两行藏在 `for request in args.read_entry_user_indirect:` **循环体内**，没传该参数时
> 根本不生效 —— 这就是 `db-vfsfsadd` 的 `entry_indirect_memory: {}` 的由来。从 CLI 显式传即可，
> **不需要改脚本**。

按 `bsd/sys/mount.h:567` 的 `struct vfs_fsentry` 解码 `entry_regions['0:0:40']`（`vfe_fsname` 在
偏移 `0x1c`）：

| 字段 | 实测 | 含义 |
| --- | --- | --- |
| `vfe_vfsops` | `0xfffffff005f2b3c8` | 非 NULL，通过 `kpi_vfs.c:937-940` 的检查 |
| `vfe_vopcnt` | `6` | 在 1..8 合法区间 |
| `vfe_opvdescs` | `0xfffffff006b2d478` | 非 NULL |
| `vfe_fstypenum` | `0` | — |
| **`vfe_fsname`** | **`b'apfs'`** | 调用者提交的文件系统类型名 |
| `vfe_flags` | `0x16479` | bit0 = `VFS_TBLTHREADSAFE`（`mount.h:549`）置位，通过 `kpi_vfs.c:944` |

函数返回态（不是断点入口态）：

| 字段 | 实测 | 含义 |
| --- | --- | --- |
| `x0` | **`0x0`** | `vfs_fsadd` 返回成功 |
| `function_return_target` | `0xfffffff00566aa70` | 落在 APFS `__TEXT_EXEC` 内 |
| `function_return_stack_matched` | `True` / `other_stacks_skipped=0` | 真从该函数返回，非同址误停 |
| `entry_indirect_memory['1:0:20']` | pointer `0xfffffff007b2d848`，bytes `c8b3f205f0ffffff 61706673…` | `*handle` 即 `vfstable_add()` 链入 `vfsconf` 的条目：`vfc_vfsops=0xfffffff005f2b3c8`、`vfc_name="apfs"`、`vfc_typenum=0x18` |

⚠️ 解码 `*handle` 要按内核内部 `struct vfstable`（`vfc_name` 在偏移 **8**），不是
`struct vfsconf` 的偏移 4；照后者解会得到 `\xf0\xff\xff\xffapfs` 这种假串。源码
`kpi_vfs.c:1117`：`*handle = vfstable_add(newvfstbl);` ⇒ **该条目已进 `vfsconf` 链表**。

⇒ 四重独立证据一致：**APFS 真的调了 `vfs_fsadd`、真的返回 0、真的把 `apfs` 链进了 `vfsconf`。**
`db-apfs-retry` / `db-vfsfsadd` 里那两个 0 不是「没注册」。

### 69.13.3 实验 B：坐实时序（`build/db-apfs-order`）

只比实验 A 多一个 `--trace-symbol _native_apfs_probe_checkpoint`：

| elapsed (s) | 事件 | PC |
| --- | --- | --- |
| 9.111 | `com.apple.filesystems.apfs:kmod_start` | `0xfffffff0056401e8` |
| **9.112** | **`_native_apfs_probe_checkpoint`** | `0xfffffff007728188` |
| 9.211 | `AppleFSCompressionTypeZlib:kmod_start` | `0xfffffff005b29464` |
| 9.222 | 最后一条内核打印 `cpu_id … is type %d` | — |
| **> 9.222** | **`_vfs_fsadd` 返回到 `0xfffffff00566aa70`** | — |

源码把顺序钉死：`.cache/xnu-work/bsd/kern/bsd_init.c:797-799` 是
`vfsinit(); → native_apfs_load_probe();`，probe 里的 `vfsconf` 遍历（`:502-510`）**紧跟
`OSKextLoadKextWithIdentifier`**，比 APFS 自己的 `vfs_fsadd` 早约 0.11 s。两个全局
`_native_apfs_registered` / `_native_apfs_vfsops` 只在 probe 里赋值一次，之后不再刷新。

⇒ **§69.12.7 的核心结论作废**：不是「kext 没 start / 没调 `vfs_fsadd`」，而是
**仪器在注册发生之前就拍了照**。`load_status=0` 仍有效（二进制加载成功），
`registered=0` 不再是任何故障证据。§69.12.3 表格 apfs 行的「只加载未注册」措辞同样作废。

### 69.13.4 单变量 root 对照：`rd=md0` 生效，但墙是「内存盘没配置」

两格唯一变量是 `--kernel-extra-args` 里多出的 `rd=md0`，都带 `--break-symbol _panic`：

| 产物 | `unixnu.toml` 的 args | `_panic` 处 `argument_memory.x0` |
| --- | --- | --- |
| `db-root-base` | `-v serial=3 debug=0x14e kt_fixed_zones=120 sheng-pmgr=2` | `Failed to mount root device @%s:%d` |
| `db-root-rdmd0` | 同上 + `rd=md0` | **`IOFindBSDRoot: specified root memory device, %s, has not been configured @%s:%d`** |

两格 `pc` 同为 `_panic 0xfffffff007a752e0`、同为 87 条内核打印、`interrupted=False`、
`ipados_userspace_boot_demonstrated=false`。**config 已落盘核对过**，`rd=md0` 确实进了 boot args
⇒ 不是参数没生效，是内核按参数走了另一条分支后撞到新墙。

源码 `.cache/xnu-work/iokit/bsddev/IOKitBSDInit.cpp:846-884`：`rdBootVar` 匹配 `mdX` 时调
`mdevlookup(xchar)`，`*root < 0` 就 panic 这一句。⇒ **`mdevlookup(0)` 找不到 md0**，
即 root 内存盘从未被配置；这与 §69.12.2 的「本板没有 `native-user-fixture`」是同一件事，
但现在有了精确到源码行的判据。

复核过的组件缺口（本轮未改任何东西）：

- ~~`build/native-user-fixture/` 不存在；`strings kernel.macho | grep -c _native_user_checkpoint` = **0**；
  全树找不到 `interface-audit.json` ⇒ `--native-user` / `--native-user-ramdisk` 这条链走不通。~~
  ⚠️ **已被 §69.14 推翻**：前两条是「没跑生成器」+「误把 own-process 断点当内核符号」，不是组件缺口；
  运行 `scripts/build-native-user-fixture.py` 后 `--native-user` 整条链可用。只有
  `interface-audit.json` / virtio 组件缺口仍然成立（见 §69.14.5）。
- 脚本 `:433` 只在 `--native-user` 时写 `initrd = "Payloads/<root>"\nargs = "rd=md0 …"`；
  `--data-ramdisk`（`:439`）虽然也会写 `initrd = "Payloads/scratch-data.img"`，但要求
  `--root-match-volume-uuid` + `--prelink-report`，且镜像头部必须是真实 `NXSB`
  APFS 容器（`:174-187`），最终仍落到 virtio IOMedia —— §69.9.1 已证本板
  `[kernel] archives` 无 `VirtioMmioBlock`（只有 `RamBlock`）。

⇒ **`rd=` 是必要但不充分条件**；「挂 root 只差补 `rd=`」（§69.8.4 第 3 条）和
「先让 APFS 真注册」（§69.12.8 第 1 条）现在都作废，第一优先回到 root 媒体本身。

### 69.13.5 下一格可用的真实资产（不要造值）

- 21H461 两卷 dmg 确实在：`UniXNU/out/ipsw/21H461/044-76034-038.dmg`、
  `044-76040-038.dmg`（§68 提到的那两份）。
- 现成参数：`--virtio-udif-image` / `--virtio-block-image` / `--virtio-extra-block-image` /
  `--virtio-mmio-fdt` / `--root-volume-uuid` / `--root-bsd-name` / `--root-match-volume-uuid` /
  `--log-registry-strings`（后者可读真实 IOMedia UUID/BSD name，无需假定枚举顺序）。
- ⚠️ 卷 UUID 必须从真实卷解析，不要手填；`--root-volume-uuid` 与 `block_image` 是绑定的
  （脚本 `:265`）。§69.9.1 的 `_native_virtio_ready` 缺口仍在，走 virtio 前先判这一格。

### 69.13.6 更新后的优先级（覆盖 §69.12.8）

1. **root 媒体**（第一优先，已有精确判据）：让 `mdevlookup(0)` 有 md0，或让 virtio/UDIF
   IOMedia 出现在 IORegistry 后走 `rd=uuid` / `rd=diskXsY`。判据：panic 串不再是
   `specified root memory device … has not been configured`，且 `IOFindBSDRoot` 报
   `BSD root: … major … minor …`。这格需要移植/复用组件，**属于改内核/构建 fixture 的范畴，
   本轮按约束没有动**。
2. **launchd / EL0**：`_native_user_checkpoint` 符号缺失，需移植组件。
3. **GPU 用户态插件 → `-3`**：条件没变（§22 / §33.3），EL1 侧证据 §69.12.3/69.12.4 依然成立。

### 69.13.7 边界、产物、命令

诚实边界：报告首部仍是 `hardware_boot_demonstrated: false` / `os_boot_demonstrated: false`，
`-3 = VK_ERROR_INITIALIZATION_FAILED`（EL0 metal2vulkan）**仍未复现**。本轮买到的是
「清掉一格伪需求 + 把真墙从一句模糊的 `Failed to mount root device` 定位到源码行」，
不是把启动链往前推了一层。

产物 `build/db-{vfsfsadd-finish,apfs-order,root-base,root-rdmd0}`；日志
`logs/qemu-{vfsfsadd-finish,apfs-order,root-base,root-rdmd0}-2026-11-13.log`。
**本轮没有改任何脚本或内核代码**，全部为既有参数；`--armcpu` + `sheng-pmgr=2` 的
opt-in 仪器前提（§69.12.2）继续适用。

root 基线命令（下一轮做单变量时从这里改，zsh 需 `${=FLAGS}`）：

```bash
cd UniXNU/out/muyu/tree
P=/opt/homebrew/bin/python3.12
FLAGS=$($P /tmp/chosen-flags.py build/db-chosen/esp/EFI/UniXNU/Payloads/kernel.macho.adt /device-tree/chosen)
$P scripts/probe-darwinboot-xnu.py --kernel ../kernel/kernel.macho \
  --prelink-report ../kernel/kernel.json --armvirt --armcpu --qemu-ramfb \
  --qemu-memory-mib 4096 --timeout 120 --log-kprintf --nvram-proxy \
  --amfi-platform-only --break-symbol _panic --kernel-extra-args "sheng-pmgr=2" \
  ${=FLAGS} --output build/db-root-base
```

---

## §69.14 2026-11-13 10:35–11:00：⭐⭐⭐ 跑一次现成生成器，启动链连过三层

接着 §69.13.6 的第 1 条做。**结论：所谓「root 媒体需要移植组件」是误判** —— 树里就有
`scripts/build-native-user-fixture.py`，跑一次就补齐；随后 `IOFindBSDRoot` → `vfs_mountroot` →
`load_init_program` 连过三层，最后停在**新的、更靠后的**墙：`native_owner_cache_prepare()` 的
`EAUTH=80`。全程零改码、零刷机、TCG，每格约 14 秒。

### 69.14.1 判读格（跑之前定的）

| 观测 | 结论 |
| --- | --- |
| `--native-user` 后断点命中 `_vfs_mountroot`（`IOFindBSDRoot` 之后） | md0 真被配置、`IOFindBSDRoot` 成功返回 ⇒ §69.13.4 的墙过了 |
| `vfs_mountroot()` 返回 0 且 `function_return_stack_matched=True` | root **挂载成功**，不只是设备存在 |
| 命中 `_load_init_program` | 内核走到加载 init |
| 命中 `_native_user_checkpoint`（own fixture EL0）且 4K ABI 断言通过 | EL0 用户态真起来 |
| 停在 `_panic` | 读 `argument_memory.x0` 定位新墙 |

### 69.14.2 先更正两处误判（都很贵）

1. **`_native_user_checkpoint` 不是内核符号**。脚本 `:559` 是
   `elif args.native_user and args.break_symbol == '_native_user_checkpoint': panic = {1: f"{user_addresses['_checkpoint']:x}"}` ——
   断点取自 **own fixture 的 `addresses.json`**（用户态 `_checkpoint = 0x10000400c`）。
   `kernel.macho` 里没有这个名字是设计如此，§69.12.2 / §69.13.4 把它当「挂不上 md0 root」
   的证据是错的。
2. **缺的不是组件，是没跑生成器**。`UniXNU/out/muyu/tree/scripts/build-native-user-fixture.py`
   是现成的 iOS 17 ARM64 静态 EL0 fixture 构建器（`xcrun clang/ld/vtool/codesign`，纯本地）。
   跑一次就产出 `build/native-user-fixture/`：

   ```
   launchd.macho (33,520 B), launchd-static.macho, user.o,
   addresses.json {_checkpoint 0x10000400c, _results, _user_page_size, _old_length, …},
   trustcache.bundle, trustcache.json
   ```

   ⚠️ 这不新增任何组件、不改一行代码，只是**运行树里已有的构建脚本**。

`--native-user`（脚本 `:362-363, :424-435`）一次给齐 md0 的三要素：

| 要素 | 来源 |
| --- | --- |
| DT `/chosen/memory-map/RAMDisk` 16 字节占位 | `make-dt.py:149` 收到 `--ramdisk`；DarwinBoot `:328-330` 识别 |
| `initrd = "Payloads/launchd.macho"` | `probe-darwinboot-xnu.py:433`，loader 把文件装进内存 |
| `args = "rd=md0 -v … sheng-pmgr=2"` | 同一行；`:396` 的 extra-args 仍保留在后面 |

内核侧 `IOKitBSDInit.cpp:822-840`：见 `RAMDisk` 属性就 `mdevadd(-1, base>>12, size>>12, 0)` ⇒ md0 存在；
DarwinBoot `:853-856` 用 `Request->Ramdisk` 把占位填成真实 base/size。反证也明确：只有占位而没有
`initrd` 时，`:759-762` 的 `else if (RamdiskOffset != MAX_UINTN) → EFI_UNSUPPORTED` 会直接挂掉
（所以「只补 `rd=`」或「只注入 DT 属性」都不够，§69.13.4 的判断在这一层是对的）。

### 69.14.3 三格推进的证据链

`--native-user` 会把默认 `--break-symbol _panic` 改写成 `_native_user_checkpoint`（`:434-435`），
所以先用真实内核符号 `_vfs_mountroot` / `_load_init_program` 做阶梯判据（`nm` 已核对地址）：

| 产物 | 断点 | 实测 | 含义 |
| --- | --- | --- | --- |
| `db-native-user-root` | `_vfs_mountroot` | `pc=0xfffffff0074e7c2c`、`interrupted=False`、87 条打印 | `setconf()→IOFindBSDRoot()` **没有 panic**，成功返回 rootdev |
| `db-native-user-mount` | 同上 + `--finish-function` | 返回到 `0xfffffff007728900`、**`x0=0`**、`function_return_stack_matched=True`、0 次异栈返回 | **`vfs_mountroot()` 成功**（`bsd_init.c:1025` 的 `err==0` 分支） |
| `db-native-user-init` | `_load_init_program` | `pc=0xfffffff007753288` | 内核走到加载 init |
| `db-native-user-el0` / `-el0-plain` | 默认 `_native_user_checkpoint` | 停在 `_panic 0xfffffff007a752e0`，89 条打印 | 新墙，见 69.14.4 |

调用顺序有源码保证：`bsd_init.c:1017-1025` 是 `setconf()` → `IOFindBSDRoot()`（`:1344`）
→ `bsd_init_kprintf("vfs_mountroot")` → `vfs_mountroot()`；`IOKitBSDInit.cpp:864/884` 是上一轮
panic 的位置。⇒ **断点命中 `_vfs_mountroot` 本身就证明 `IOFindBSDRoot` 已经成功返回**。

`--trust-cache build/native-user-fixture/trustcache.bundle` 单变量：加与不加**都是同一个 panic**
（同为 89 条打印、同一 `argument_memory.x0`）⇒ trust cache 不是这一格的变量。

### 69.14.4 新墙：`native_owner_cache_prepare()` 返回 `EAUTH=80`

```
argument_memory.x0 = 'offline owner SystemOS mount failed: %d'
owner_cache_mount = {stage:0, status:80, mount_status:-1, readonly:0,
                     authorized:0, root_recheck:0,
                     selected_device:-1, mounted_device:-1, actual_device_path:''}
```

源码 `kern_exec.c:8244-8248`：`XNU_OWNER_CACHE_MOUNT` 打开时，
`load_init_program()` 第一步就调 `native_owner_cache_prepare()`，非 0 直接 panic。
该函数开头（`kern_exec.c:7139-7144`）：

```c
root_authorized = (vfs_flags(root_mount) & MNT_RDONLY) && root_device != NODEV &&
    native_verified_block_root_authorized(root_device);
if (!root_authorized) goto done;
native_owner_cache_stage = 1;
```

⇒ **md0 虽然挂上了，但不是 `native_verified_block_root_authorized()` 认可的「已验证块设备」**，
于是 `stage` 停在 0、`error=EAUTH(80)`。这与 §69.9.1「本板没有 VirtioMmioBlock」、
以及官方 `scripts/check-owner-systemos-mount-checkpoints.py` 的正例期望
（`stage=6`、`status=0`、`/dev/disk2s1`、`native_virtio_verified_devices==2`）完全对上。

⚠️ 官方 checker 的**负对照**期望 `stage=1, status=80, root_recheck=1`；本轮是
`stage=0, root_recheck=0` ⇒ 我们比那个负对照**还早一步**（连 root 授权都没过），
不要拿 checker 的断言来判本板。
**§69.15.3 更正**：这句只在「不加 `native-ufs-allow-unverified`」时成立；加上该
boot-arg 后本板 `stage=1/root_recheck=1`，与负对照逐位对齐，就是同一条路径。

### 69.14.5 下一轮的真实输入（不造值）

`kern_exec.c:7101-7103` 硬编码了目标卷 UUID：

```c
#ifndef XNU_OWNER_CACHE_VOLUME_UUID
#define XNU_OWNER_CACHE_VOLUME_UUID "8846FE78-2910-4297-AF97-D59BB6F8FC16"
#endif
```

它与设备 ADT 的 `apfs-preboot-uuid=8846fe7829104297af97d59bb6f8fc16` **逐位一致**
（`/tmp/chosen-flags.py` 已在透传）⇒ 下一轮 `--root-match-volume-uuid` 有真实依据。

- 真实双卷：`UniXNU/out/ipsw/21H461/044-76034-038.dmg`、`044-76040-038.dmg`；
  `fw/data-seed.img` 是合法 `DBRDISK1` packed RAMDisk（解压首 `NXSB`，逻辑 200 GiB，
  `encoded_sha256=476464c3…`）。
- 现成参数：`--root-match-volume-uuid` / `--virtio-udif-image` / `--virtio-block-image` /
  `--virtio-mmio-fdt` / `--log-registry-strings` / `--trust-cache`。
- ~~前置复核：本板 `kernel.macho` 是否有 `_native_virtio_ready`（§69.9.1 的结论是**没有**）；
  若确实没有，这一格要先把 virtio 存储 ABI 补齐，属改内核/构建组件的范畴。~~
  ⚠️ **已被 §69.15.2 推翻**：「没有 `_native_virtio_ready`」属实，但本板链入的是
  `ShengUfs/NativeUfs.cpp:1745` 的 bring-up 版授权函数，一个现成 boot-arg
  `native-ufs-allow-unverified=1` 就能把 `stage` 0→1，不需要 virtio ABI、也不需要改内核。
  virtio 存储组件只在 §69.15.6 的「真实 SystemOS 卷」那一格才成为前提。
- 判据：`owner_cache_mount.stage ≥ 1` 且 `native_verified_block_root_authorized` 通过；
  最终正例是 `stage=6, status=0, mounted_device=0x01000004`。

### 69.14.6 边界、产物、命令

诚实边界：报告仍是 `hardware_boot_demonstrated: false` / `os_boot_demonstrated: false`；
`native_user_el0_observed=false`，`native_user_memory` 三项全为 `E14`（EL0 地址未映射）
⇒ **EL0 用户态还没起来**，`-3` 当然仍未复现。本轮买到的是把 root 这一格从
「`IOFindBSDRoot` panic」推到「`load_init_program` 里的 owner SystemOS mount」，
中间三层全部有断点/返回值实测，不是推测。`sheng-pmgr=2` 的 opt-in 仪器前提（§69.12.2）继续适用。

产物 `build/native-user-fixture/`、`build/db-{native-user-root,native-user-mount,native-user-init,native-user-el0,native-user-el0-plain}`；
日志 `logs/qemu-native-user-{root,mount,init,el0,el0-plain}-2026-11-13.log`。

复现命令（相对 §69.13.7 只加 `--native-user`，断点换符号；`${=FLAGS}` 是 zsh 分词）：

```bash
cd UniXNU/out/muyu/tree
P=/opt/homebrew/bin/python3.12
$P scripts/build-native-user-fixture.py            # 一次即可，纯本地构建
FLAGS=$($P /tmp/chosen-flags.py build/db-chosen/esp/EFI/UniXNU/Payloads/kernel.macho.adt /device-tree/chosen)
$P scripts/probe-darwinboot-xnu.py --kernel ../kernel/kernel.macho \
  --prelink-report ../kernel/kernel.json --armvirt --armcpu --qemu-ramfb \
  --qemu-memory-mib 4096 --timeout 60 --log-kprintf --nvram-proxy \
  --amfi-platform-only --kernel-extra-args "sheng-pmgr=2" --native-user \
  --break-symbol _vfs_mountroot ${=FLAGS} --output build/db-native-user-root
# 去掉 --break-symbol（默认）即观测 EL0 checkpoint，当前会停在 owner SystemOS mount 那堵新墙
```

**本轮没有改任何脚本或内核代码**；`--finish-function` 那格第一次误加了
`--read-entry-region 0:0:40`（`vfs_mountroot(void)` 的 x0=0，不是内核指针）被 gdb 拒绝，
去掉后重跑成功 —— 是观测参数错误，不是内核失败。

## §69.15 2026-11-13 10:53–11:10：⭐⭐ owner 授权墙被一个现成 boot-arg 打穿；下一格是 SystemOS 卷 UUID

### 69.15.1 判读格（跑之前定的）

严格单变量只在 `--kernel-extra-args` 里加 `native-ufs-allow-unverified=1`，其余与
§69.14.6 的 `--native-user` 基线一致，断点沿用默认 `_native_user_checkpoint`：

- `owner_cache_mount.stage` 0→1 且 `status` 仍 80 ⇒ 授权墙过了，停在「找不到 SystemOS 卷」；
- `stage` 仍 0 ⇒ 链入的是 source-storage 版或 `NativeUfs::start()` 根本没跑，需换路径；
- panic 串变化 / 命中 EL0 checkpoint ⇒ 更靠后的推进。

### 69.15.2 先更正 §69.14.5 的关键误判：本板不需要 virtio 存储 ABI

`nm -U kernel.macho | grep -ci virtio` = **0**，连 `VirtioMmio*` / `AppleVirtIOBlock` /
`IOVirtio*` 类名都没有 —— §69.9.1 说的「没有 `_native_virtio_ready`」属实，**但这不是本板的墙**。
本板链入的是 `UniXNU-muyu-17/drivers/ShengUfs/NativeUfs.cpp` 的
`!NATIVE_UFS_SOURCE_API` bring-up 分支（`:1745-1749`）：

```cpp
extern "C" int native_verified_block_root_authorized (dev_t device)
{
  (void) device;
  return gAllowUnverifiedRoot ? 1 : 0;      // :1748
}
```

而 `gAllowUnverifiedRoot` 由 `NativeUfs::start()` 的 `:1969-1970` 赋值：

```cpp
PE_parse_boot_argn ("native-ufs-allow-unverified", &allowUnverified, sizeof (allowUnverified));
gAllowUnverifiedRoot = allowUnverified == 1;
```

赋值发生在 `:1973` 的 `if ((!probe && !rootMode) || !OSDynamicCast(...)) return false;`
**早退之前** ⇒ 即使 `start()` 不匹配硬件、直接返回 false，全局也已被置 1。
`package/SourceRootBinding.cpp:96` 那个走 `native_ufs_block_authenticates()` 的 verified 版
是 source-storage 构建（`-DNATIVE_UFS_SOURCE_API=1`）用的；
`UniXNU-27/drivers/VirtioMmioBlock/NativeVirtioBlock.cpp:386` 是 27 线的。两者都不是本板。

`nm` 里实际存在的两个符号：`_native_verified_block_root_authorized = 0xfffffff0079ed1e8`、
`_native_verified_block_volume_device = 0xfffffff0079ed1f8`。

> ⚠️ **诚实边界**：`native-ufs-allow-unverified=1` 是内核编译进去的 **bring-up escape hatch**，
> 它让授权函数恒返回 1、**绕过 owner policy 包与块设备签名验证**。所以下面 `stage=1` 的
> 「授权通过」不是真实验证的证据，官方 checker 正例要求的
> `native_virtio_integrity._native_virtio_verified_devices == 2` 在这条路径上永远达不到。
> 它只能用来把启动链往前推，不能据此宣称 owner trust / verified root 通过。
> 与 `sheng-pmgr=2` 一样是主动开启的诊断开关，不是原厂行为（§69.12.2）。

### 69.15.3 六格实测（TCG，零改码、零刷机）

| 产物 | 唯一变化 | 结果 |
| --- | --- | --- |
| `db-native-user-el0-plain` | §69.14 基线 `sheng-pmgr=2` | `stage=0, status=80, root_recheck=0` |
| `db-owner-unverified` | **+ `native-ufs-allow-unverified=1`** | **`stage=1, status=80, root_recheck=1`**，`selected/mounted_device=-1` |
| `db-owner-registry` | + `--log-registry-strings` | 15 条注册表事件：只有 `BSD Name=disk0`，**没有任何 `UUID`** |
| `db-owner-ufsroot` | + `native-ufs-root=1` | 与上一格逐位相同 ⇒ 不是这一格的变量 |
| `db-owner-apfsram` | `--native-user-ramdisk fw/data-seed.img` | `boot refused: Bad Buffer Size`，在 DarwinBoot 阶段就停（见 69.15.5） |
| `db-owner-voldev-trace` | `--trace-symbol _native_verified_block_volume_device` | 9.31 s 命中，`pc=0xfffffff0079ed1f8`、`lr=0xfffffff00775341c`、`x0=0xfffffff00712a919`（UUID 串指针） |

配置落盘已核对：`args = "rd=md0 -v serial=3 debug=0x14e kt_fixed_zones=120 sheng-pmgr=2 native-ufs-allow-unverified=1"`。
所有格同为 89 条打印、同一 `_panic` PC `0xfffffff007a752e0`、同一 panic 串
`offline owner SystemOS mount failed: %d @%s:%d`。

**与官方 `scripts/check-owner-systemos-mount-checkpoints.py` 的 `missing` 负对照逐位对齐**
（`stage=1, status=80, mount_status=-1, selected/mounted=-1, authorized=0, root_recheck=1`）
⇒ §69.14.4 里「我们比负对照还早一步，不是同一条路径」的判断**作废**：加一个 boot-arg
就能走到同一层，差的只是 `verified_devices` 那套真实 I/O 完整性计数。

`db-owner-apfsram` 有个观测器插曲：QEMU 那格在 boot 阶段已拒，随后探针 `:1210`
解析 `_native_hid_*` 页时抛 `ValueError: non-hexadecimal number`（`gdb-stop.json` 只有
74 个键、`bounded_timeout_interrupted=True`、TTBR1=0）—— 是**报告生成**问题，不是内核失败；
真实原因在 `gdb-uart.log` 里（见 69.15.5）。

### 69.15.4 新墙精确位置（源码 + trace 双重坐实）

`kern_exec.c:7140-7148`：

```c
root_authorized = (vfs_flags (root_mount) & MNT_RDONLY) && root_device != NODEV &&
    native_verified_block_root_authorized (root_device);
if (!root_authorized) goto done;
native_owner_cache_stage = 1;
if (!native_verified_block_volume_device (volume, &selected, device_path,
    sizeof (device_path))) goto done;
```

`done:`（`:7201-7204`）`root_recheck = root_authorized && native_verified_block_root_authorized(root_device)`
—— 观测到 `root_recheck=1` **反证 `root_authorized=1`**；`error` 保持 `:7117` 的初值 `EAUTH=80`。
`volume` 是 `:7101-7103` 硬编码的 `8846FE78-2910-4297-AF97-D59BB6F8FC16`。

`NativeUfs.cpp:1751-1800` 的 bring-up 版 `native_verified_block_volume_device()`：
按 `IOService::serviceMatching("IOMedia")` + `IOPropertyMatch{UUID: <volume>}` 找卷，
要求 `UUID` / `kIOBSDNameKey` / `kIOBSDMajorKey` / `kIOBSDMinorKey` 齐全且 `matches == 1`，
`gAllowUnverifiedRoot` 下最多等 100 × `IOSleep(100)`，仍找不到就返回 0。
trace 已证明它**确实被调用**，注册表已证明**没有任何 UUID 属性**。

⇒ 唯一缺口：**IORegistry 里没有带 `8846FE78-…` UUID 的 SystemOS IOMedia**。

### 69.15.5 真实资产盘点（全部实测，不造值）

- `build/native-user-fixture/launchd.macho`：`\xcf\xfa\xed\xfe` **裸 Mach-O**，33520 B，
  既不是 `DBRDISK1` packed RAMDisk 也不按 4 KiB 对齐 ⇒ md0 的内容是一个 Mach-O 可执行文件，
  **没有 APFS 容器、没有分区表**（注册表里 `Content` 依次探测
  `GUID_partition_scheme` / `FDisk_partition_scheme` / `Apple_partition_scheme` 全落空）。
  这解释了为什么连一个卷 UUID 都发布不出来。
- `fw/data-seed.img`：合法 `DBRDISK1`，逻辑 `0x3200000000` = **200 GiB**，
  `encoded_sha256=476464c3…`，展开首 4 KiB 的 `[32:36]=NXSB`（真实 APFS 容器），
  全盘只有一个卷 UUID **`625F64C3-704B-4BA1-9E82-AFBFD83AE86C`**，**不是目标值**。
  拿它当 `--native-user-ramdisk` 时 UART 明确写：
  `DARWINBOOT: used 214819274752 window 268435456 kernel 70885376 ramdisk 214748364800` →
  `DARWINBOOT: boot refused: Bad Buffer Size; firmware retained`（200 GiB 逻辑盘塞不进
  256 MiB 窗口）。⇒ 这条不是「换个镜像就能过」，是尺寸 + UUID 双重不符。
- `UniXNU/out/ipsw/21H461/044-760{34,40}-038.dmg`：3.30 GB / 5.16 GB，`koly` UDIF；
  明文 UUID 集合（2245 / 33 个）里**没有**目标值（载荷压缩，未做 UDIF 解包）。
  目标 UUID 的真实来源仍是内核 `kern_exec.c:7101-7103` 与设备 ADT `apfs-preboot-uuid`
  逐位一致，不是从镜像里硬搜出来的。
- **路径互斥（脚本层面已写死）**：`probe-darwinboot-xnu.py:265` 的
  `--root-volume-uuid` / `--root-bsd-name` / `--root-match-volume-uuid` 都要求
  `block_image` 且**与 `--native-user` 冲突**；`make-dt.py:93` 的
  `--root-match-volume-uuid` 要求 `--virtio-mmio-fdt` 且无 `--ramdisk`，`:95` 的
  `--data-ramdisk` 要求有 UUID 匹配且无 RAM root；`probe-darwinboot-xnu.py:175` 的
  `--data-ramdisk` 与 `--native_user` 直接互斥。⇒ **RAM root 与真实磁盘 root 是两条互斥链**，
  不能靠加参数把 md0「升级」成 SystemOS 卷。
- 本板 `kernel.macho` 无任何 virtio 块驱动类名 ⇒ 官方正例那条
  `--virtio-udif-image` / `--virtio-block-image` + `--virtio-mmio-fdt` 路径需要链入
  `NativeVirtioBlock` 组件（改构建），本轮按「不改内核代码」约束没动。

### 69.15.6 下一格优先级（覆盖 §69.14.5）

> ⚠️ **§69.16 已把下面四条路径逐一实测堵死**（现成 candidate / virtio 磁盘 /
> packed seed / raw md0 initrd），并把目标 UUID 的固件级出处坐实。
> 「只能走真实块设备路径」的结论仍成立，但请先读 §69.16.4 的具体硬阻塞再动手。

1. **让带 `8846FE78-…` UUID 的真实 SystemOS IOMedia 出现**：只能走真实块设备路径
   （链入 virtio 存储组件 → `--virtio-block-image` / `--virtio-udif-image` +
   `--virtio-mmio-fdt` + `--root-match-volume-uuid`）。判据 `owner_cache_mount.stage ≥ 2`
   且 `selected_device != -1`。卷 UUID 必须从真实卷解析，**不能用 md0 或改 DT 属性冒充**。
2. 过 stage 2 之后的阶梯（源码 `:7150-7195`）：`stage=3` 是 `copyout` mount payload、
   `stage=4` 是真 `mount()`、`stage=5` 是 `cache_authorized`（要求 readonly + `apfs` +
   `actual == selected` + 再授权一次）、`stage=6` 才写 dyld 环境向量。
   正例全局：`status=0`、`mounted_device=0x01000004`、`actual_device_path=/dev/disk2s1`。
3. 只有 `stage=6` 且 `load_init_program()` 返回，EL0 `_checkpoint` 才可能命中；
   `-3` 仍在 root → launchd → GPU 插件之后。

### 69.15.7 边界、产物、命令

诚实边界：报告仍是 `hardware_boot_demonstrated: false` / `os_boot_demonstrated: false`，
`native_user_el0_observed=false`，**EL0 没起来、`-3` 未复现**。
本轮买到的是把 owner SystemOS 那一格从 `stage=0`（root 授权失败）推到
`stage=1`（授权通过、缺 SystemOS 卷），并把缺口精确到「IORegistry 需要一个真实卷 UUID」；
不是把启动链推到用户态。`sheng-pmgr=2` 与 `native-ufs-allow-unverified=1`
两个 opt-in 仪器前提都继续适用。

产物：`build/db-owner-{unverified,registry,ufsroot,apfsram,voldev-trace}`；
日志：`logs/qemu-owner-{unverified,registry,ufsroot,apfsram,voldev-trace}-2026-11-13.log`。
**本轮没有改任何脚本或内核代码**，6 格 TCG（含 1 格 boot 阶段负对照）。

复现命令（相对 §69.14.6 只在 `--kernel-extra-args` 加一个 boot-arg；`${=FLAGS}` 是 zsh 分词）：

```bash
cd UniXNU/out/muyu/tree
P=/opt/homebrew/bin/python3.12
FLAGS=$($P /tmp/chosen-flags.py build/db-chosen/esp/EFI/UniXNU/Payloads/kernel.macho.adt /device-tree/chosen)
$P scripts/probe-darwinboot-xnu.py --kernel ../kernel/kernel.macho \
  --prelink-report ../kernel/kernel.json --armvirt --armcpu --qemu-ramfb \
  --qemu-memory-mib 4096 --timeout 90 --log-kprintf --log-registry-strings \
  --nvram-proxy --amfi-platform-only \
  --kernel-extra-args "sheng-pmgr=2 native-ufs-allow-unverified=1" \
  --native-user ${=FLAGS} --output build/db-owner-registry
# 想看墙的调用点：把 --log-registry-strings 换成
#   --trace-symbol _native_verified_block_volume_device
```

## §69.16 2026-11-13 11:10–11:25：⭐ 真实 SystemOS 卷 UUID 取证完成；stage 2 的四条路径全部实测堵死

### 69.16.1 判读格（跑之前定的）

目标只有一个：让 IORegistry 里出现带 `8846FE78-2910-4297-AF97-D59BB6F8FC16`
UUID 的**真实** SystemOS IOMedia，判据 `owner_cache_mount.stage ≥ 2` 且
`selected_device != -1`。禁止用 md0 / 改 DT 属性 / 拼接假 UUID 冒充。

### 69.16.2 硬结论 1：目标 UUID 是真实固件卷，不是造出来的值

`UniXNU/out/ipsw/21H461/044-76034-038.dmg` 的 offset **18608368**
（block 4543、块内偏移 240）存在 16 字节
`8846fe7829104297af97d59bb6f8fc16` —— 正是目标 UUID 的二进制形式。
前后 512 字节窗口里同时有 `APSB`（APFS **卷**超块 magic）以及
`newfs_apfs (2142.41.2.100.2)` / `apfs_kext (2142.41.2.100.2)` 版本串
⇒ 这是 `apfs_vol_uuid`，不是偶然字节。

之前 §69.14.5 / §69.15.5 只在镜像里搜**明文 UUID 字符串**所以找不到；
APFS 卷 UUID 是二进制 16 字节。至此三方一致：内核 `kern_exec.c:7101-7103`
硬编码、设备 ADT `apfs-preboot-uuid`、真实 21H461 固件镜像。
⇒ 「卷 UUID 必须从真实卷解析、不造值」这条规矩 now 有固件级依据。

### 69.16.3 硬结论 2：函数入参与返回码逐字节实测

`build/db-owner-voldev-return`（`--break-symbol
_native_verified_block_volume_device --finish-function
--read-entry-region 0:0:40`，其余同 §69.15.7）：

```
function_entry_arguments = ['0xfffffff00712a919', '0xfffffff048143c00',
                            '0xfffffff048143d60', '0x40', ...]
entry_regions['0:0:40']  = b'8846FE78-2910-4297-AF97-D59BB6F8FC16'
finished_function_entry  = 0xfffffff0079ed1f8
function_return_target   = 0xfffffff00775341c   (native_owner_cache_prepare)
function_return_stack_matched = True
x0 (return)              = 0x0                  ⇒ 没找到卷
```

断点在函数内时 `owner_cache_mount = {stage:1, status:-1, root_recheck:0}`
⇒ 顺带解释了 §69.15 的两个数字：全局 `_native_owner_cache_status` 初值是 -1，
`status=80` 是 `kern_exec.c:7117` 的 `error = EAUTH` 在 `done:` 才写入；
`root_recheck=1` 也是 `done:` `:7202` 才算的。这些都是**时序快照**，不是第二处失败。

### 69.16.4 四条路径的实测边界（本轮 stage 仍停在 1）

| 路径 | 做法 | 实测结果 | 硬阻塞 |
| --- | --- | --- | --- |
| **A 现成 candidate** | 查 `build/xnu-prelink/*.macho` 5 个 + `build/native-prelink/*.macho` 2 个 | 前 5 个与当前 `kernel.macho` **sha 完全相同**（`e627d477…`），后 2 个是 `d15dffa8…`；全部 `nm -U \| grep -ci virtio = 0` | 树里没有已链入 virtio 存储的现成候选内核 |
| **B virtio 真实磁盘** | QEMU `dumpdtb` 生成 FDT + `--virtio-block-image 044-76034-038.dmg` + `--root-match-volume-uuid 8846FE78-…` | `make-dt` **成功**：`Synthetic QEMU DT: 17668 bytes`，32 个 `virtio,mmio` 节点全过校验（`0xa000000+0x200`, `dma-coherent`）；探针随即 `Cannot resolve _native_virtio_read_checkpoint in this kernel` | `probe:289-290` 把断点强制改成该 checkpoint；`:1020-1022` 与 `:1527-1530` 还无条件依赖 6 个 `_native_virtio_*` 全局。内核 IOKit 匹配表只有 `ArmCpu/ArmVirtPlatform/ArmDTPlatform/IOPanicPlatform/IOExclaveProxy/NativeAES/NativeRamBlock/NativeUfs`，唯一 VirtIO 类是 `AppleVirtIONeuralEngineDevice` ⇒ **没有 virtio 存储 transport** |
| **C packed seed** | `packed_ramdisk.pack(044-76034-038.dmg)` → `DBRDISK1` | `ValueError: Nonzero input exceeds packed format bound`（0.1 s 即失败） | `DBRDISK1` 最多 65536 个非零页 = 256 MiB / `MAX_INPUT`；3.3 GB 真实容器装不下 |
| **D raw md0 initrd** | `--native-user-ramdisk 044-76034-038.dmg` + `--qemu-memory-mib 8192 --owned-memory-size 0x100000000` | QEMU **未启动**（`ConnectionRefusedError`、`qemu_exit_code=1`）；`gdb-stderr.log`：`File …/root.img is larger than 2GB` → `Could not read directory …/Payloads` | QEMU **vvfat FAT 单文件 2 GiB 硬限制**。与 4 GiB owned window 无关 —— 窗口根本没机会分配。3.2 GB ESP 副本已清理，保留 `gdb-stderr.log` / `gdb-failure.json` 作证据 |

`--owned-memory-size` 的数学本来是够的：`make-dt.py:78` 允许 256 MiB–4 GiB（2 MiB 对齐），
`DarwinBoot.c:22` 的 `WINDOW_SIZE = SIZE_256MB` 可被 DT `darwinboot-memory-size` 覆盖，
3.15 GiB ramdisk + 70 MB kernel ≈ 3234 MiB < 4096 MiB。是 FAT 层先挡住的。

### 69.16.5 真实资产盘点（只读取证，全部卸载干净）

| 资产 | 实测 | 可用性 |
| --- | --- | --- |
| `044-76034-038.dmg` | 3,303,014,400 B（3.08 GiB），`UDRW` raw、`Encrypted=false`；`hdiutil attach -readonly -nomount` 直接出 `EF57347C…`/`41504653…`（**裸 APFS 容器，无 GPT**），单卷 `DawnUpdate21H461.J12…`、容器 UUID `E886F82C-EE56-4B52-AF5F-E70BF46320C4` | `%512 == 0` ⇒ 可作 `--virtio-block-image`；含目标 `apfs_vol_uuid` |
| `044-76040-038.dmg` | 5,157,406,069 B，`ULFO`/lzfse 压缩只读、未加密；`GUID_partition_scheme` + `Apple_APFS` 6.1 GB，单卷 `DawnUpdate21H461.J120J121J207J208OS`（System 角色） | **`%512 == 373`** ⇒ 不能作 raw 块设备 |
| `fw/data-seed.img` | 200 GiB `DBRDISK1`、`NXSB`、卷 UUID `625F64C3-704B-4BA1-9E82-AFBFD83AE86C`（Data 卷），`encoded_sha256=476464c3…` | 二进制目标 UUID **不在**其中 |

两个 dmg 都是 **DawnUpdate21H461 单卷镜像**，不是「SystemOS + Data 多卷盘」。
明文 UUID 集合分别有 2245 / 33 个，都没有目标值（二进制存储所致）。
`hdiutil attach -readonly -nomount` 取证后 `hdiutil info | grep -c "044-760"` = **0**，
主机上没有任何残留挂载。

### 69.16.6 顺带查清的一条现成能力（还不够用）

`kernel.macho` 里有完整 `NativeRamBlock`
（`__ZN14NativeRamBlock5startEP9IOServiceE`、`SeedDisk`、`persist::Overlay`），
IOKit 匹配表项为 `IOProviderClass=IOResources / IOResourceMatch=IOBSD`。
`UniXNU/drivers/RamBlock/NativeRamBlock.cpp:197-260`：从 `/chosen/memory-map` 读
`RAMDisk` 或 `PackedRAMDisk` 的 16 字节占位；packed 输入经 `ramPackedLayout()` 得到
`logical`，`logical > 0x80000000` 时走 `isBig` 稀疏 `SeedDisk`（只存非零页）。
`DarwinBoot.c:329-332` 见 `PackedRAMDisk` 就置 `KeepPacked=TRUE`，
`:824` 于是 `RamdiskBytes = Request->RamdiskSize`（encoded 大小）、`:853` 直接 `CopyMem`
—— 这就是 §69.15.5 里 200 GiB 盘只占 1.09 MB 的原理。

**但 QEMU 路径用不上**：`make-dt.py:148-149` 只发 `RAMDisk` 占位，没有
`PackedRAMDisk` 开关；`scripts/make-muyu-adt.py:283` / `devicetree.py:425` 的
`packed_data_ramdisk` 是**真实设备 ADT** 路径，不是 QEMU DT。而且
`NativeRamBlock.cpp:200` 明确 `if(packed && range) panic("both RAMDisk and PackedRAMDisk")`
⇒ 不能用现成的 `--node-property` 简单叠加两个占位。要走这条必须改离线仪器并换合规 seed，
本轮按边界没动。

### 69.16.7 下一格优先级（覆盖 §69.15.6）

1. **链入 `NativeVirtioBlock` 组件**（`UniXNU/drivers/VirtioMmioBlock/NativeVirtioBlock.cpp`，
   `UniXNU-27` 有同款），让 `_native_virtio_read_checkpoint` 等 6 个全局存在，然后走路径 B。
   FDT 已现成（QEMU `dumpdtb`，32 节点合规）、dmg 已 512 对齐、`make-dt` 已验证通过。
   判据仍为 `stage ≥ 2` 且 `selected_device != -1`。这是**改构建组件**，不是改参数。
2. 或改 `make-dt.py` 增加 QEMU DT 的 `PackedRAMDisk` 开关，并配一个体积 ≤ 256 MiB、
   含目标 `apfs_vol_uuid` 的真实 seed。⚠️ 若从 3.3 GB 容器截取元数据子集，
   那是**拼接镜像**，不能冒充完整真实卷，结论里必须如实标注；UUID 本身仍是真实值。
3. 禁止：用 md0 / 改 DT 属性 / 伪造 UUID 冒充 SystemOS 卷；这会让 §69.15.2 的
   escape-hatch 边界再叠一层假身份，结论不可信。

### 69.16.8 边界、产物、命令

诚实边界：报告仍是 `hardware_boot_demonstrated: false` / `os_boot_demonstrated: false`，
`native_user_el0_observed=false`，**`-3` 未复现**。本轮**没有把 stage 推过 1**；
买到的是「真实卷 UUID 的固件级取证」+「四条路径各自的硬阻塞精确到行」，
把 §69.15.6 那句「需要 virtio 存储组件」从推测变成四条实测负证据。
`sheng-pmgr=2` 与 `native-ufs-allow-unverified=1` 两个 opt-in 前提不变。
**零改码、零刷机**，QEMU 3 格（1 格成功、1 格启动前拒、1 格 QEMU 未启动）。

产物：`build/db-owner-voldev-return`、`build/db-real-disk`、`build/db-real-systemos`；
日志：`logs/qemu-{owner-voldev-return,real-disk,real-systemos}-2026-11-13.log`；
QEMU FDT `/tmp/qemu-virt-21H461.dtb`（1 MiB，`dtc` 可见 32 个 virtio 节点）。

复现命令（坐实墙 / 生成 FDT / 只读取证）：

```bash
cd UniXNU/out/muyu/tree
P=/opt/homebrew/bin/python3.12
FLAGS=$($P /tmp/chosen-flags.py build/db-chosen/esp/EFI/UniXNU/Payloads/kernel.macho.adt /device-tree/chosen)
# 1) 读 native_verified_block_volume_device 的 UUID 入参与返回码
$P scripts/probe-darwinboot-xnu.py --kernel ../kernel/kernel.macho \
  --prelink-report ../kernel/kernel.json --armvirt --armcpu --qemu-ramfb \
  --qemu-memory-mib 4096 --timeout 90 --log-kprintf --nvram-proxy --amfi-platform-only \
  --kernel-extra-args "sheng-pmgr=2 native-ufs-allow-unverified=1" --native-user \
  --break-symbol _native_verified_block_volume_device --finish-function \
  --read-entry-region 0:0:40 ${=FLAGS} --output build/db-owner-voldev-return
# 2) QEMU 生成的合规 virtio FDT
qemu-system-aarch64 -machine virt,dumpdtb=/tmp/qemu-virt-21H461.dtb \
  -cpu cortex-a710 -m 4096 -smp 1 -accel tcg -nodefaults -nographic
# 3) 真实卷 UUID 的只读取证（用完立刻卸载）
hdiutil attach -readonly -nomount UniXNU/out/ipsw/21H461/044-76034-038.dmg
#    offset 18608368 = 8846fe7829104297af97d59bb6f8fc16；同块含 APSB / newfs_apfs 版本串
hdiutil detach /dev/diskN -quiet
```

---

## §69.17 放宽工作约定后：定点补丁打穿 owner panic，**EL0 真正起来**

### 69.17.0 本轮变更的边界（先写清楚，避免后面误读）

用户在本轮明确放宽了**项目内的工作约定**：不再要求「零改码 / 只用现成参数 / 不碰内核镜像」。
据此本轮**实际改了**：

| 类型 | 具体 |
| --- | --- |
| 新增离线仪器 | `UniXNU/out/muyu/tree/scripts/patch-skip-owner-panic.py`（可复现的定点补丁生成器） |
| 新增内核镜像 | `UniXNU/out/muyu/kernel/kernel.skip-owner.macho`（**新文件**，原件不动） |
| 新增 prelink report | `UniXNU/out/muyu/kernel/kernel.skip-owner.json`（只改 `candidate_sha256` + 追加补丁登记） |

**仍严格保持**：全程本地离线 QEMU TCG、零刷机、不碰真实设备、不伪造 UUID/身份值、
不改真实设备 ADT、不把 bring-up escape hatch 说成「verified root 通过」。
⚠️ **本节的补丁是一个绕过 owner policy 的离线诊断手段，不是修复 owner root，也不是原厂行为**；
`owner_cache_mount` 全局仍如实记录 `stage=1 / status=EAUTH(80) / authorized=0`。

### 69.17.1 为什么是定点二进制补丁，而不是重建内核

`UniXNU-muyu-17/scripts/build-xnu-4k-baseline.sh` 是完整 XNU 构建（git patch series → 全量编译 →
unixnu prelink），重编会**改变全部符号地址与 extents**，必须重新生成 17 MB `kernel.json`、
重新核对 119 个 extent，代价与风险都远大于收益。

关键观察：`load_init_program()` 把 `native_owner_cache_prepare()` 与
`native_owner_data_prepare()` **全部内联**了（`nm` 里两者没有独立符号），而且
`probe-darwinboot-xnu.py:256` 对输入只校验一件事实——
`sha256(kernel.macho) == prelink_report['candidate_sha256']`；符号则通过 `:549`
直接对传入的 `--kernel` 跑 `xcrun nm -U`。所以只要**不改任何字节布局**，
`kernel.json` 的 extents / bindings / 符号地址可以原样复用，只需同步一个新 sha。

### 69.17.2 补丁点：两条 `CBNZ`，四重校验

用树里现成的 VA→文件偏移思路（`kits/XnuPrelink/native_prelink.py` 同款）扫描
`_load_init_program`（`0xfffffff007753288`–`0xfffffff007753c94`），把每个
`bl _panic` 往前配 `adrp x0/add x0` 解析格式串，得到 cold panic 块与源码行号：

| VA | 原指令 | 目标 | 目标首字 | 含义 |
| --- | --- | --- | --- | --- |
| `0xfffffff00775352c` | `350034f6` `CBNZ w22` | `0xfffffff007753bc8` | `52840708` = `mov w8,#8248` | `if (cache_error) panic("offline owner SystemOS mount failed: %d")` (`kern_exec.c:8248`) |
| `0xfffffff007753738` | `35002595` `CBNZ w?` | `0xfffffff007753be8` | `528407c8` = `mov w8,#8254` | `if (data_error) panic("offline scratch Data mount failed: %d")` (`kern_exec.c:8254`) |

替换成 `0xd503201f`（AArch64 `NOP`），**只去掉 panic 副作用、不改控制流结构**。
为什么不直接 NOP `bl _panic`：cold 块是连续排列的（`0x753be4` 后面紧跟下一个块），
NOP 掉 `bl` 会「落入」下一个 panic 块，反而必崩。

`patch-skip-owner-panic.py` 的四重保险：① 输入 kernel 的 sha 必须等于输入 json 的
`candidate_sha256`；② 两个 VA 的原 word 必须逐字节匹配；③ 分支目标必须命中预期冷块首字
（源码行号立即数）；④ 输出用字符串拼后缀 —— `Path.with_suffix()` 会把
`kernel.skip-owner` 当成带后缀文件名而**覆盖原始 `kernel.macho`**，第一次写脚本时踩到并已修掉。

生成结果：原件 `kernel.macho` sha 仍是 `e627d477c73ba048df…`（前后实测一致）；
新镜像 `kernel.skip-owner.macho` sha `c4c1420a6b7cad2b7d…`，
长度同为 53 202 944 B；`nm` 复核 `_load_init_program` / `_panic` /
`_native_verified_block_volume_device` 地址**全部不变**。

### 69.17.3 判读格与三格实测

判读格：跳过两处 panic 后，`load_init_program()` 应继续走到
`load_init_program_at_path("/sbin/launchd", init_envp)`；
- 命中 `_native_user_checkpoint`（用户态 `0x10000400c` 附近）⇒ **EL0 成立**；
- 停在 `_panic` ⇒ 得到下一个真实墙（预期是代码签名 / AMFI）。

| 产物 | 唯一变量 | 结果 |
| --- | --- | --- |
| `db-el0-skip-owner` | 无 `--trust-cache` | `stop_symbol=_panic`，`pc=0xfffffff007a752e0`，格式串 `unexpected SIGKILL of %s %s with reason -- namespace %d code 0x%llx description %.800s` ⇒ **AMFI 杀掉未被信任的 launchd** |
| `db-el0-skip-owner-tc` | 加 `--trust-cache build/native-user-fixture/trustcache.bundle` | `stop_symbol=_native_user_checkpoint`，`pc=0x1000003d4`，**`native_user_el0_observed=true`** |
| `db-el0-gpu-chain` | 再叠加 `--load-iogpu --load-mobile-graphics --mobile-graphics-client-probe` | 仍命中 `_native_user_checkpoint`，且 GPU 内核链全部就绪 |

**EL0 硬证据**（`db-el0-skip-owner-tc`）：`native_user_memory` 从上一轮的三项 `E14`
变成真实用户态读值 —— `_results=0100…`、`_user_page_size=0x1000`（4 KiB）、
`_old_length=0x4000000`（64 MiB），`trust_cache_bundle_sha256=fd055811…`，89 条内核打印。
⇒ **trust cache 是 EL0 的必要条件**，且 own fixture 的 cdhash 必须与镜像匹配
（换 `--native-user-image` 就得重算 bundle）。

**同格 GPU 链**（`db-el0-gpu-chain`）：`native_iogpu_load_status=0`、
`native_mobile_graphics_load_status=0`、
`native_mobile_graphics_client = {abstract_null_mask:7, client_vptr:0xfffffff005f34dd0,
client_meta:0xfffffff006b313b0, client_size:0x538, init:1, released:1,
selector_results:[0xe0000001]×3}` —— 与 §69.12 `db-mgc` **逐位一致**。
也就是现在**同一格里 EL0 已执行 + 内核 GPU 客户端对象已构造并给出 `0xe0000001`**，
比 §69.12 的「EL1 单独成立」更靠近 `-3`。

### 69.17.4 `-3` 的真实语义与剩余缺口（不夸大）

`UniXNU-muyu-17/package/BravearkLastLog.c:730-739` 写明：`-3` 是 Turnip/Vulkan
Darwin 后端 `tu_knl_darwin.cc:161 dw_probe_selector` 探测 selector 0..255，
**一个都没被 sandbox 放行**（`pr=0`）时 `vkCreateInstance/Device` 返回
`VK_ERROR_INITIALIZATION_FAILED`。当前离线板缺的是整个用户态栈：
dyld shared cache、vkd3/Turnip 真实 dylib、sandbox profile、真实 GPU 服务发布。

`--offline-gpu-broker` 看着像桥，但 `probe:457-459` 要求 `--native-air-init`，
而 `build-xnu-4k-baseline.sh:527-529` 要求 `NATIVE_AIR_INIT=1` 时
`OWNER_ROOT_TRUST=1` —— **本内核根本没编这个**，所以那条路仍不可达。
own fixture 是 `build-native-user-fixture.py` 生成的静态 4K ABI 测试进程，
没有 dyld、不会调 `io_connect_method`。

> ⚠️ **本节 `-3` 的 API 表述已被 §69.18 用真实 dylib 实测更正**：`pr=0` 确实产出
> `VK_ERROR_INITIALIZATION_FAILED`，但它出现在 **`vkEnumeratePhysicalDevices`**，
> `vkCreateInstance` 恒返回 `VK_SUCCESS(0)`、`vkCreateDevice` 根本不可达。
> 另外 `0xe0000001` 是 `kIOReturnInvalid`（EL1 基类越界），不是 sandbox 全拒的
> `kIOReturnNotPermitted(0xe00002e2)`。原文保留以便追溯，判读以 §69.18 为准。

### 69.17.5 作废 / 修正清单

- **§69.14.4 / §69.14.5、§69.15.6、§69.16.7 的「必须链入 virtio 组件 / 必须改构建才能往前推」不再成立**：
  它们的目标是「让 `stage ≥ 2`、让 owner policy 真正通过」；本节走的是**另一条路**——
  不让 owner 通过，只是不再因它 panic，从而让启动链继续。两者判据不同，不能混为一谈。
- **§69.16.8「`native_user_el0_observed=false` / EL0 未起」作废**（仅限本补丁内核 + trust cache 这一格）。
- §69.13.4 / §69.14 关于「组件缺失」的误判已在 §69.14、§69.15 各自更正，本节不再重复。
- **不能据本节宣称 `os_boot_demonstrated`**：own fixture 不是真 launchd，报告里
  `ipados_userspace_boot_demonstrated=false`、`functional_ipados_boot_demonstrated=false` 保持。

### 69.17.6 下一格优先级（真实输入已明确）

1. **让 EL0 发真实 IOKit `io_connect_method`**：扩展 own fixture 的静态 Mach-O，
   对 selector 0..255 逐个探测并记录被拒/被放行的返回码（这正是 `-3` 的判据 `pr=0`）。
   前提：重算 trustcache bundle、内核侧把可连接的 IOGPU user client 真正发布出来
   （§69.12 的 `db-mgc` 只构造对象、`client_released=1`，没有发布服务）。
2. 或重建一个 `OWNER_ROOT_TRUST=1` 的内核走 `offline-air-init=1` + `--offline-gpu-broker`
   + `--accelerator-abi-probe` / `--original-client-abi-probe`，用现成 broker 测客户端 ABI。
3. 或把真实 dyld cache / Turnip dylib 引入 md0 root（需要 >2 GiB 的 ESP 单文件上限解法，
   见 §69.16 路径 D 的 vvfat 限制），并配 sandbox profile。

判据只有一个：**EL0 侧观察到 `vkCreateInstance/Device` 返回 `-3`，且能追溯到
selector 全拒**，不是内核侧再打印一个 `0xe0000001`。

> ⚠️ **§69.18 更正**：这句判据里的 API 写错了。真实 Turnip 里 `dw_open_connection()`
> 是在物理设备枚举时调用，所以 `-3` 由 `vkEnumeratePhysicalDevices` 返回；
> `vkCreateInstance` 在 selector 全拒时仍返回 0。新的判据 =
> 「EL0 侧 `vkEnumeratePhysicalDevices` 返回 `-3` + 能追溯到 256 个 selector 全拒」。
> 本节第 1 条「让 EL0 发真实 `io_connect_method`」的方向仍成立，其余两条不变。

### 69.17.7 边界、产物、复现命令

诚实边界：报告仍 `hardware_boot_demonstrated: false` / `os_boot_demonstrated: false`；
`owner_cache_mount` 仍是 `stage=1, status=80, root_recheck=1, authorized=0,
selected/mounted_device=-1`（**owner policy 未通过，如实记录**）；
`-3` **仍未复现**。`sheng-pmgr=2` 与 `native-ufs-allow-unverified=1`
两个 opt-in 前提不变。零刷机、TCG 3 格。

产物：`build/db-{el0-skip-owner,el0-skip-owner-tc,el0-gpu-chain}`；
日志：`logs/qemu-{el0-skip-owner,el0-skip-owner-tc,el0-gpu-chain}-2026-11-13.log`。
回滚：删掉 `kernel.skip-owner.{macho,json}` 与补丁脚本即可，原件从未改动。

```bash
cd UniXNU/out/muyu/tree
P=/opt/homebrew/bin/python3.12
FLAGS=$($P /tmp/chosen-flags.py build/db-chosen/esp/EFI/UniXNU/Payloads/kernel.macho.adt /device-tree/chosen)
# 0) 生成补丁内核（原件不动）
$P scripts/patch-skip-owner-panic.py --kernel ../kernel/kernel.macho \
  --prelink-report ../kernel/kernel.json --output-stem ../kernel/kernel.skip-owner
# 1) EL0（zsh 里带引号的 boot-args 必须用数组，${=FLAGS} 只对纯标志分词）
BASE=(--kernel ../kernel/kernel.skip-owner.macho --prelink-report ../kernel/kernel.skip-owner.json \
  --armvirt --armcpu --qemu-ramfb --qemu-memory-mib 4096 --timeout 90 \
  --log-kprintf --log-registry-strings --nvram-proxy --amfi-platform-only \
  --kernel-extra-args "sheng-pmgr=2 native-ufs-allow-unverified=1" \
  --native-user --trust-cache build/native-user-fixture/trustcache.bundle)
$P scripts/probe-darwinboot-xnu.py "${BASE[@]}" ${=FLAGS} --output build/db-el0-skip-owner-tc
# 2) 同格叠 GPU 内核链
$P scripts/probe-darwinboot-xnu.py "${BASE[@]}" --load-iogpu --load-mobile-graphics \
  --mobile-graphics-client-probe ${=FLAGS} --output build/db-el0-gpu-chain
```

---

## §69.18 2026-11-13 11:45：⭐⭐⭐ **真实 Turnip dylib 把 `-3` 复现出来了**，并纠正判据 —— `-3` 在 `vkEnumeratePhysicalDevices`，不在 `vkCreateInstance`

### 69.18.0 本轮边界（先说清楚哪一层是真的）

用户放宽项目内约定后，本轮选了**性价比最高的一条路**（§67 已铺好）：不再往 QEMU 里堆内核
checkpoint，而是直接让**板上同一份源码编出的同一个真实 Mach-O** 把 `-3` 跑出来，用它反过来
校准前面 17 节的判据。

| 层 | 真假 |
| --- | --- |
| `libvulkan_freedreno.dylib`（17 921 872 B，sha `111136be0c2f90014c…`） | **真实产物，一个字节没改**（`vtool` 只改副本的 `LC_BUILD_VERSION` 以便 macOS dyld 加载，见 §67.2） |
| `dw_probe_selector()` 0..255、`dw_open_connection()`、`vk_startup_errorf()`、`VkResult` | **真实 Turnip 代码路径**，未插桩 |
| 8 个 IOKit 回包（`IOConnectCallStructMethod` 等） | **合成的**，由 own harness interpose；这是唯一受控变量 |
| QEMU / 刷机 / 真实设备 | 全程未碰；本轮零格 TCG、零改内核镜像、零改 dylib |

所以本节的结论强度是：**「真实客户端在这些 IOKit 输入下必然返回什么」**，不是「板上 sandbox 当时
到底拒了什么」。后者仍需板侧 evidence。

### 69.18.1 新增的离线仪器（3 个文件）

| 文件 | 作用 |
| --- | --- |
| `UniXNU/scripts/host-turnip-harness/fakeiokit.c` | 在原有 8 符号 interpose 上加 opt-in `TU_SELECTOR_POLICY`（`admit0` 默认 / `admit-<0..255>` / `deny-all`）、`TU_REFUSAL`、`TU_OPEN_REFUSAL`；记录 `probes / admitted / denied / pr / refusal / sel_bits`。**默认行为与 §67 基线逐位一致**（负对照） |
| `UniXNU/scripts/host-turnip-harness/run-selector-matrix.py` | 6 格单变量矩阵，逐格解析真实 driver 文本与三个 Vulkan 入口的 `VkResult`，写 `matrix.json` 并做 9 条断言 |
| `UniXNU/scripts/check-turnip-minus3-judgement.py` | 把矩阵真值与 **40 个既有 QEMU 格**的 `gdb-stop.json` 逐格对齐，禁止把 `0xe0000001` 当成 `-3` 证据，写 `judgement.json` |

`deny-all` 用的拒绝码是**真实内核常量** `kIOReturnNotPermitted = iokit_common_err(0x2e2) = 0xe00002e2`
（`UniXNU/out/feeds/xnu-3199e8bc/iokit/IOKit/IOReturn.h:183`），与
`check-original-sandbox-iokit.py` 实测的原厂 Sandbox 拒绝码逐位一致；不是随手编的失败码。

### 69.18.2 六格实测（唯一变量 = IOKit 回包策略）

| 格 | 变量 | probes | pr | refusal | opens | `vkCreateInstance` | `vkEnumeratePhysicalDevices` | `vkCreateDevice` |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `admit0` | §67 基线：只放行 selector 0 | 2 | 2 | — | 2 | **0** | 0 | **0** |
| `deny-all` | 256 个 selector 全 `0xe00002e2` | **256** | **0** | `e00002e2` | 1 | **0** | **-3** | 不可达 |
| `admit-17` | 只放行 selector 17 | 36 | 2 | `e00002c1` | 2 | 0 | 0 | 0 |
| `deny-all-unsupported` | 全拒但换成 `kIOReturnUnsupported 0xe00002c7` | 256 | 0 | `e00002c7` | 1 | 0 | **-3** | 不可达 |
| `deny-all-direct` | 加 `TU_DARWIN_SERVICE=NativeAdrenoGpuPublished` | **0** | 0 | — | 2 | 0 | 0 | **0** |
| `open-refused` | `IOServiceOpen` 本身返回 `0xe00002e2` | 0 | 0 | — | 1 | 0 | **-3** | 不可达 |

`deny-all` 格真实 driver 原文（stderr，未插桩）：

```
TU: error: .../tu_knl_darwin.cc:204: no external-method selector is admitted for this process (VK_ERROR_INITIALIZATION_FAILED)
TU: error: NativeAdreno: could not open the kernel client (-3)
[IOK] === selector policy=deny-all probes=256 admitted=0 denied=256 pr=0 refusal=e00002e2 sel_bits=00000000 ...
[host] vkCreateInstance -> 0 (VK_SUCCESS)
[host] vkEnumeratePhysicalDevices(count) -> -3 (VK_ERROR_INITIALIZATION_FAILED) n=0
```

`admit-17` 的 `probes=36`（= 2 次 open × 18 次扫描）顺带坐实：**`dw_probe_selector()` 是每次
`IOServiceOpen` 都跑一遍**，不是进程级一次；`deny-all` 只有 `opens=1` 是因为第一次就失败。

### 69.18.3 ⭐ 判据修正（三处，前面 17 节按此判读）

1. **`-3` 的 API 写错了**：`BravearkLastLog.c:730-739`、§69.17.4 / §69.17.6 写的是
   「`vkCreateInstance/Device` 返回 -3」。真实 Turnip 里 `dw_open_connection()` 在
   **物理设备枚举**（`tu_enumerate_physical_devices` → knl open）时调用：
   `vkCreateInstance` **恒返回 0**（实例不碰 kernel client），`-3` 由
   **`vkEnumeratePhysicalDevices`** 返回，`vkCreateDevice` 根本没机会被调用。
   上层 `R PUB no device: Vulkan(VkResultCode(-3))` 与这条链完全吻合，不矛盾。

> ⚠️ **本条最后一句已被 §69.19 推翻，判读以 §69.19 为准**：这句「完全吻合、不矛盾」
> 是错的。板上 `MUYU L pr=2 tn=105 sb=00000001` 说明 selector 0 被放行、105 次隧道调用
> 成功，§64.6 又排除了 `dw_open_connection` 三条出口，§67.6 判定 `R PUB` 属**另一个进程**，
> §67.4 在主机同形序列下 `vkCreateDevice` 是成功的。⇒ 本节 host `deny-all` 复现的是
> **tunnel 形式 selector 全拒**那一条 `-3`，**不是板上那条 `-3`**。
> 上面第 1 条对真实 Turnip 代码的表述本身仍成立（`vkCreateInstance` 恒 0、`-3` 在枚举），
> 但只能用于描述「全拒格」，不能用于描述板上现象。

2. **`0xe0000001` 不是 sandbox 全拒**：§69.12 / §69.17 的 `selector_results=[0xe0000001]×3`
   来自 EL1 把越界 selector 直接喂给 `IOUserClient::externalMethod` 基类，值是
   `kIOReturnInvalid = iokit_common_err(0x1)`；真实 sandbox 拒绝是
   `kIOReturnNotPermitted = 0xe00002e2`。两者机理、调用层、常量都不同，
   **不能拿 §69.17「同格 EL0 + 0xe0000001」宣称接近 `-3`**。
   `check-turnip-minus3-judgement.py` 已对 40 个 QEMU 格逐格打上
   `minus3_reproduced_in_qemu=false`。
3. **`-3` 有三条可区分的出口**（现在能用 driver 原文判别板上是哪一条，不必再猜）：
   | 出口 | 源码 | 原文 |
   | --- | --- | --- |
   | selector 全拒 | `tu_knl_darwin.cc:204` | `no external-method selector is admitted for this process` |
   | `IOServiceOpen` 被拒 | `:198` | `IOServiceOpen(IOAcceleratorES, 0x4147) failed: 0x…` |
   | ABI 不匹配 | `:216`（§67.10 已测） | `NativeAdreno query failed: 0x…` |
   三条都汇到 `NativeAdreno: could not open the kernel client (-3)`，所以只看上层
   `-3` 分不出来，**必须抓 `tu_knl_darwin.cc` 的行号原文**。

### 69.18.4 因果链（充分/必要条件，全部实测）

- `dw_probe_selector()` 只认 `== KERN_SUCCESS`：把 refusal 从 `0xe00002e2` 换成
  `0xe00002c7` 仍然 `-3` ⇒ **与返回哪个 IOReturn 无关**。
- 只要**任意一个** selector 被放行（`admit0` / `admit-17`），`pr≥1`，设备就能建成 ⇒
  **256 个全非 KERN_SUCCESS 是 `-3` 的充要条件**（在 tunnel 形式下）。
- 服务名改成 `NativeAdrenoGpuPublished` 时 `dw.tunnel=false`，**根本不跑 probe**
  （`probes=0`），全拒策略也照常建成设备 ⇒ selector 准入只在 tunnel 形式下生效。
  这是离线环境里最干净的规避方向；真实设备上能否用取决于 entitlement。

### 69.18.5 QEMU 侧仍差什么（精确到四件事，不再泛泛说「缺 dyld」）

`judgement.json` 对 40 格逐格给出 `reason`；要在 QEMU EL0 复现同一个 `-3`，缺的是：

1. **真正发布**一个可连接的 GPU `IOService`（`registerService()`）。`db-mgc` 只是
   `allocClassWithName + init + externalMethod + release`（`client_released=1`），没有服务、
   没有 `io_connect_t`。
2. EL0 用**真实 Mach MIG** 走 `IOServiceOpen → io_connect_method`。当前 own 静态 fixture
   只有 BSD syscall 和 `thread_switch` Mach trap，零 Mach IPC / IOKit 代码。
3. Sandbox 给该 task 装上 `io_filter_policy_t`，对
   `io_filter_type_external_method` 一律拒（`IOUserClient.cpp:5210-5218` 是原厂真实机制，
   不是离线开关）。
4. EL0 镜像得是**真 Turnip/Vulkan 客户端**（dyld shared cache / IOKit / libc++）；
   `libvulkan_freedreno.dylib` 的 `otool -L` 显示 9 个系统依赖全部只能动态链接，
   **不能静态链进 4K fixture**，且 §69.16 路径 D 的 vvfat 2 GiB 单文件上限仍未解。

另外**观测点也要改**：判据是 `vkEnumeratePhysicalDevices`，不是 `vkCreateInstance`。

### 69.18.6 下一格（按性价比）

1. ~~**先抓 driver 原文**：板上/日志里定位 `tu_knl_darwin.cc:204` vs `:198` vs `:216`~~
   **§69.19 已判此路不可行**：全仓 `tu_*.cc:` 行号原文都来自源码推演或主机实测，板上**从未
   采集过 Turnip stderr**；`MUYU G`（按进程拒绝账）也从未拍到过（§65 那一 boot 图形栈根本没起）。
   而且 §64.6 用全局计数器「排除三条出口」的推理本身有漏洞（§69.19.4）。
2. ~~若是 `:204`（selector 全拒），方向是放行至少一个 selector / direct 形式~~
   ——这是**另一条 `-3`** 的解法，对板上不适用；板的方向见 §69.19.6。
3. QEMU 侧若仍要在 EL0 复现，最小补齐仍是「发布可连接 user client + 真实 Mach IPC +
   sandbox filter」三件套（§69.18.5）；`offline-gpu-broker` 那条仍需
   `OWNER_ROOT_TRUST=1` 全量重建，§69.17.4 的判断不变。

### 69.18.7 边界、产物、复现命令

诚实边界：**`-3` 已在 host 上用真实 dylib 复现，但那只是「tunnel 形式 selector 全拒」这一条，
并非板上那条 `-3`（§69.19）**；`iokit_replies_synthetic=true`、
`board_sandbox_profile_replayed=false`、`qemu_el0_reproduced=false`、
`hardware_boot_demonstrated=false`、`os_boot_demonstrated=false`。
40 个 QEMU 格全部 `minus3_reproduced_in_qemu=false`；`owner_cache_mount` 仍是
`stage=1 / status=80 / authorized=0`（§69.17 如实记录，未改）。
本轮**零刷机、零格 TCG、零改内核镜像、零改 dylib**；只改 own harness 的 IOKit 回包策略。

产物：
- `UniXNU/out/muyu/tree/build/host-turnip-selector/{admit0.log,deny-all.log}`
- `…/matrix/{matrix.json,admit0.log,deny-all.log,admit-17.log,deny-all-unsupported.log,deny-all-direct.log,open-refused.log}`
- `…/judgement.json`（40 格逐格判定）

```bash
cd UniXNU/scripts/host-turnip-harness
P=/opt/homebrew/bin/python3.12
# 6 格矩阵（默认 dylib = ../../build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib）
$P run-selector-matrix.py
# 或只跑决定性的一格
TU_SELECTOR_POLICY=deny-all ./run.sh \
  ../../build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib
# 把真值与所有 QEMU 格对齐
cd .. && $P check-turnip-minus3-judgement.py
```

---

## §69.19 2026-11-13 12:4x：`-3` 归因翻转 —— 上一节复现的是**另一条** `-3`；根因落在 sandbox grant 属性不匹配

本轮**零刷机、零格 TCG、零改内核镜像、零改 dylib**。全部结论来自：真机屏照既有读数、
三个分支的源码对照、以及 host 上真实 `libvulkan_freedreno.dylib`（sha `111136be…` 未改）的
27 + 6 格单变量实验。产物由 `check-grant-mismatch.py` 逐条机器校验。

### 69.19.1 先把两条 `-3` 分开（§69.18 的适用范围更正）

| | §69.18 host `deny-all` | 板上 `R PUB no device: Vulkan(VkResultCode(-3))` |
| --- | --- | --- |
| selector | 256 个全拒，`pr=0` | `MUYU L pr=2 tn=105 sb=00000001` ⇒ selector 0 放行、105 次真实隧道调用 |
| 失败 API | `vkEnumeratePhysicalDevices` | 未知（runtime 只给一个裸 `VkResultCode(-3)`） |
| driver 原文 | `:204 no external-method selector is admitted` | **板上从未采集 Turnip stderr** |
| 连接 | 一次都没建起来 | 至少两个进程建起来并跑完 105 次 ABI 调用 |
| 归属 | 单一 tunnel 全拒格 | §67.6：是**另一个进程/设备实例**的失败 |

⇒ §69.18.3 第 1 条「上层 `R PUB` 与这条链完全吻合，不矛盾」**是错的**，已就地划掉。
§69.18.6 第 1 条「先抓 driver 原文定位板上出口」也**不可行**：全仓 `tu_*.cc:` 行号原文
都来自源码推演或主机实测，板上没有 stderr 采集通道；`MUYU G` 从未拍到（§65 那 boot
`UP op=0/0`、`V call=0`，图形栈根本没起）。

### 69.19.2 两轮 host 单变量扫描：把 `-3` 的可能落点压到只剩一处

**（A）27 格输入敏感性**（`run-input-sensitivity.py`，唯一变量 = 一个合成 IOKit 回包）：
Query 的 `abi_version/in_bits/out_bits/flags/max_bo`、DeviceInfo 的
`chip_id/gmem_bytes/va_start/va_size/hbb/ubwc_sw/macrotile/ubwc_v/flags`、
指定 method 的 IOReturn、指定调用序号之后才失败、MapMemory 长度上限/拒绝、CpuMap bytes。

| 结果类别 | 格 |
| --- | --- |
| `vkCreateDevice -> -3` | **空集**（27 格无一命中） |
| `vkEnumeratePhysicalDevices -> -3` | `abi-version-3`、`fail-query`、`fail-devinfo`（全部带 `:216` 原文） |
| 设备创建阶段其它失败 | `-1`（`map-len-cap-64k`，§67.5 OOM 复刻）、`-2`（`va-size-0`/`va-start-0`/`fail-boalloc`/`fail-bomap`/`fail-boalloc-late`，`tu_device.cc:3041/3071`）、`-5`（`fail-cpumap`/`map-fail-kr`，`:3047`）、`-9`（`chip-id-bogus`，`:1683`） |
| Turnip 完全不敏感的字段 | 15 项，`in/out_bits`、`query_flags`、`max_bo`、`dev_flags`、`va-size-1m`、`gmem_bytes`、`ubwc_v/sw`、`hbb`、`macrotile`、`cpumap_bytes` |

⇒ **§67.10「下一轮应优先怀疑 `tu_CreateDevice` 内部」被实测否定**：单输入异常下该路径只会
产出 `-1/-2/-5/-9`，且每一条都带 driver 原文；`-3` 只从 `dw_open_connection` 出来。

**（B）6 格实例扩展**（`run-instance-extensions.py`，测试「零内核调用的 instance 层 -3」）：
真实 ICD **不注册** `VK_KHR_display` / `VK_KHR_portability_enumeration` / `VK_EXT_layer_settings`
⇒ 三者 `vkCreateInstance -> -7`（`VK_ERROR_EXTENSION_NOT_PRESENT`）、`calls=0 opens=0`；
`VK_EXT_debug_utils` 正常（13 calls、设备建成）。
⇒ `tu_knl_darwin.cc:1688` 的 `I can't KHR_display` 在这份 ICD 里是**死分支**，实例层**不能**产出 `-3`。

**（C）runtime 到底包装了谁**：`NativeMetalObjects.rs:193` 的
`Device::system_default_with(&options).map_err(e("device"))` → `error.rs:2391-2397` 的
`vulkan(result)` 把任意裸 `VkResult` 包成 `Error::Vulkan(VkResultCode(raw))`；`guard()` 在
`Err` 时 `set_error(why)`，所以 `nmb_last_error()` 是**本次** device open 的错误，不是陈旧值。
`create.rs` 共 **8 个**包装点：`:460` 全局扩展枚举、`:588` 全局版本、`:647` `vkCreateInstance`、
`:663` debug messenger、`:679` relay messenger、`:735` `vkEnumeratePhysicalDevices`、
`:1270` 设备扩展枚举、`:1450` `vkCreateDevice`。
按 (A)(B) 结论：`:460/:588/:647` 不可能给 -3，`:663/:679` 实测成功，`:1450` 27 格无一给 -3，
`:1270` 需先有物理设备 ⇒ **只剩 `:735`**。而 `:735` 的 `-3` 唯一来源是
`tu_knl_darwin_load → dw_open_connection` 三条出口。

> 板上 bundle 里**只有** `libvulkan_freedreno.dylib`（ICD），没有独立 Vulkan loader / ICD manifest；
> `NativeBridgeMetalFrontend.mm:4574-4577` 把同一个 `loader` 路径拿去 `dlopen` 找
> `tu_darwin_frame_diagnostic` ⇒ `ash::Entry` 是 ICD 直连，不存在第三方 loader 层。

### 69.19.3 ⭐ 根因候选：内核 patch 读的 property，muyu-17 从来没发布过

| 环节 | 文件 / 行 | 实际内容 |
| --- | --- | --- |
| 内核旁路判据 | `UniXNU/out/muyu/tree/.cache/xnu-work/iokit/Kernel/IOUserClient.cpp:4338-4360` | `native_sandbox_grant(service)` 读 **`service->getProperty("NativeSandboxGrantProcess")`**；`if (!list) return false` |
| 补丁作用点 | `UniXNU/patches/xnu/zzw-native-adreno-sandbox-grant.patch` | 三处 `!native_sandbox_grant(service) && …` 分别门控 `mac_iokit_check_open_service`、`mac_iokit_check_open`、`io_filter_resolver` |
| muyu-17 发布服务 | `UniXNU-muyu-17/package/GpuPowerProbe.cpp:1181-1182` | **只**设 `MetalPluginName` / `MetalPluginClassName`，**没有** `NativeSandboxGrantProcess` |
| 驱动实际做的 | `UniXNU/drivers/Adreno740/NativeAdrenoPublish.cpp:1094-1097` | `connection->setProperty("NativeSandboxGrant", true)`（打在 **client** 上）+ `grants++` |
| 已构建内核 | `kernel.skip-owner.macho` strings | 有 `NativeSandboxGrantProcess`、`NativeSandboxGrant`；**无** `NativeSandboxGrantService` ⇒ 未编入 uke 修正补丁 |
| 正对照（sheng） | `UniXNU-sheng/package/GpuPowerProbe.cpp:1242` | `setProperty("NativeSandboxGrantProcess", "backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard")` |
| 修复对照（uke） | `UniXNU-uke/docs/GPU-PORT.md:566-575` + `zzzz…uke-gpu-sandbox-client-grant.patch` | 原文："**an audit found the mismatch**"，新增 `native_client_sandbox_grant(client)` 读 client 的 `NativeSandboxGrant` 与 `native_service_sandbox_grant(service,type)` |

⇒ **属性名、宿主对象、值类型三项全不匹配**：`native_sandbox_grant()` 在 muyu-17 上**恒为 false**，
三处 MACF / filter 门全部原样生效。`MUYU V gr=5` 只是**驱动自己的** `grants++` 计数
（`NativeAdrenoPublish.cpp:1096`），**不是内核已放行的证据** —— `GPU-HANDOFF.md:4512`
「`gr=5` 5 次走了 `NativeSandboxGrant` 旁路」这句读法要更正。

完整链条：

```
MUYU A re00002c7 / re00002e2（sandbox resolver 拒绝）
  → is_io_service_open_extended 的 MACF/filter 门未旁路 → IOServiceOpen 返回非 0
  → Turnip dw_open_connection:198 "IOServiceOpen(IOAcceleratorES, 0x4147) failed"
  → VK_ERROR_INITIALIZATION_FAILED(-3) 从 vkEnumeratePhysicalDevices 返回
  → metal2vulkan create.rs:735 .map_err(vulkan) → Error::Vulkan(VkResultCode(-3))
  → e("device") → "device: Vulkan(VkResultCode(-3))" → R PUB no device
```

§69.18 矩阵里 **`open-refused` 格已在 host 上实测复现这条**：`TU_OPEN_REFUSAL=0xe00002e2`
⇒ `vkEnumeratePhysicalDevices -> -3`、driver `:198` 原文、`opens=1 calls=0`、
`vkCreateInstance=0`、`vkCreateDevice` 不可达 —— 形状与板上「零隧道调用 + -3」一致。

### 69.19.4 为什么 §64.6 把 `:198` 「排除」掉了：跨进程全局计数的逻辑跳跃

§64.6 用 `B n=0`（挂载从未失败）、`S 1:2/15:2` 不带 `/f`（Query/GetDeviceInfo 零失败）、
`sb=00000001`、`ex=107` 判定三条 `dw_open_connection` 出口全排除。但：

- `native_adreno_method_stats` 是 `extern "C"` **全局、跨所有 UserClient 累加**（§67.6 已证），
  `ext_entries` / `B` 同理；
- 一个被拒在 **user client 创建之前**的 `IOServiceOpen` 根本不会进 `mount()`、不会发任何
  tunnel method、不会触发 `externalMethod` ⇒ 这些计数器**一个都不会变**；
- 105 次成功调用来自被放行的那几次 open（§67.6），与失败进程并存毫无矛盾；
- `A n=6` 里 `re00002c7` / `re00002e2` 正是那几次被拒的 open 的记录。

⇒ 用「全局计数干净」排除「某个进程连接失败」不成立，**`:198` 从未被排除**。
§64.5 结论 5 其实已经写过「或某一次 `IOServiceOpen` 根本没到隧道那一层」，§64.6 又把它划掉了。

与所有既有读数逐项自洽：`pr=2 / tn=105 / ex=107 / sb=00000001 / B n=0 / S 1:2,15:2 /
V gr=5 / A re00002c7 / R PUB -3 / vkCreateInstance=0 / vkCreateDevice 不可达`。

### 69.19.5 修复方向（单变量，回板验证）

1. **最小改动（推荐）**：在 `UniXNU-muyu-17/package/GpuPowerProbe.cpp` 的 `registerService()`
   之前补一行（照 sheng 版 `:1242`）：
   ```cpp
   accelerator->setProperty("NativeSandboxGrantProcess",
       "backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard");
   ```
   只改**内核侧一个输入**，不动 Metal bundle / CDHash / trust cache / addon，符合 §68.3 的单变量纪律；
   重建 kernel + payload + vendor_boot 后回板看 `R` 行是否不再是 `-3`，并核对 `A` 行 `r=` 与 `V gr=`。
   **§69.20 已按这一条执行完毕**（一行 diff、三步离线重建、镜像就绪），目前停在待上机状态。
2. **彻底修法**：移植 `UniXNU-uke/patches/xnu/zzzzzzzzzzzz-uke-gpu-sandbox-client-grant.patch`
   （旁路读 client 的 `NativeSandboxGrant` 与 service 的 `NativeSandboxGrantService`），但要改内核
   patch 并全量重建，变量更多，作为第二步。
3. **不要再做**：放行 selector / 走 direct 形式 / 继续调 GPU 内核链 / 在 QEMU 里堆 checkpoint ——
   那些是 §69.18 那条 `-3` 的解法，对板上这条无效。

### 69.19.6 边界、产物、复现命令

诚实边界：**这是静态审计 + host 真 dylib 实测得到的根因候选，未在板上验证**。
`board_verified=false`、`flashing=false`、`qemu_cells_run=0`、
`hardware_boot_demonstrated=false`、`os_boot_demonstrated=false`；
40 个 QEMU 格仍全部 `minus3_reproduced_in_qemu=false`；
`owner_cache_mount` 仍 `stage=1 / status=80 / authorized=0`（§69.17 如实未改）。
本轮没有重建内核、没有改 dylib 指令字节、没有刷机。

产物（均在 `UniXNU/out/muyu/tree/build/host-turnip-input/`）：
- `sensitivity.json` + 27 个 `*.log`（输入敏感性）
- `instance-exts/instance-exts.json` + 6 个 `*.log`（实例扩展）
- **`grant-mismatch.json`**（根因机器校验判据，含全部源码 sha256）
- 新增脚本：`scripts/host-turnip-harness/run-input-sensitivity.py`、
  `run-instance-extensions.py`、`UniXNU/scripts/check-grant-mismatch.py`；
  改动 `fakeiokit.c`（opt-in 注入）、`icdtest.c`（opt-in 实例扩展），默认值逐位保持 §67 基线。

```bash
cd UniXNU
P=/opt/homebrew/bin/python3.12
D=build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib
$P scripts/host-turnip-harness/run-input-sensitivity.py   --dylib "$D"
$P scripts/host-turnip-harness/run-instance-extensions.py --dylib "$D"
$P scripts/check-grant-mismatch.py          # 逐条断言 + 产出 grant-mismatch.json
# 对照 §69.18 已实测的 :198 出口
cd scripts/host-turnip-harness && TU_OPEN_REFUSAL=0xe00002e2 ./run.sh ../../build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib
```

---

## §69.20 2026-11-13 12:4x：按 §69.19.5 落单变量修复并离线重建 —— **镜像已就绪，停在「待上机」**

用户指令「重建，要上机时说」：执行 §69.19.5 的最小修复与三步离线重建，**不刷机、不碰设备**；
到需要上机的环节停下并告知。全程 1m04s，零格 TCG，零改 dylib / patch / boot-arg。

### 69.20.1 单变量：一行 `setProperty`，编译输入核对过

改 `UniXNU-muyu-17/package/GpuPowerProbe.cpp:1182`，在 `accelerator->registerService()`
之前、两个 `MetalPlugin*` 属性之间，补上与 `UniXNU-sheng/package/GpuPowerProbe.cpp:1242`
**逐字相同**的名单：

```cpp
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
+   accelerator->setProperty("NativeSandboxGrantProcess", "backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
```

- diff 严格 **`+1 / -0`**（`baseline-pre-grant1/GpuPowerProbe.cpp.orig` 对照，机器校验）。
- 先钉死编译输入：`UniXNU/out/muyu/tree/kits/muyu` 是 **symlink → `UniXNU-muyu-17/package`**，
  `build-native-legacy-display.py:333` 的 `ROOT/'kits/muyu/GpuPowerProbe.cpp'` 读的就是这一份
  （两处 sha 同为 `94d1b339…`）。**改错副本会让重建白跑**，这点已排掉。
- 未移植 uke 的 client-grant 补丁（§69.19.5 第 2 条，变量更多），故内核仍只带旧 patch。

### 69.20.2 三步重建（冷启动 §7.1，参数原样）

```bash
cd /Users/diannao/pad7pro-xnu
python3 UniXNU/unixnu build kernel -D UniXNU-muyu-17 \
  --uefi /Users/diannao/pad7pro-xnu/Mu-Silicium \
  --edk2 /Users/diannao/pad7pro-xnu/Mu-Silicium/Mu_Basecore -j 8 \
  > logs/build-kernel-grant1.log 2>&1
python3 UniXNU/unixnu build payload -D UniXNU-muyu-17 -j 8 \
  --data-ramdisk fw/data-seed.img \
  --owner-trust-cache fw/trust-21H461/owner-trust-17-full.bundle \
  --gpu-addon fw/gpuaddon-17-full.bin \
  --unique-chip-id 0000000000000001 \
  --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-addon=1 --kernel-arg sheng-gpu-metal=1 \
  > logs/build-payload-grant1.log 2>&1
python3 UniXNU-muyu-17/scripts/patch-vendor-boot.py \
  --base /tmp/base96.img --payload UniXNU/out/muyu/payload/payload.img \
  --output fw/vendor_boot_gpu_grant1.img > logs/build-vendor-boot-grant1.log 2>&1
```

- **boot-arg 一个都没加/删**：三个 GPU 参数不在 `board.toml`（§7.1 靠 `--kernel-arg` 注入），
  cmdline 实测 **996 B**，距 `build payload` 的 1000 B 硬闸只剩 4 B。
- 产物：`fw/vendor_boot_gpu_grant1.img` 77,393,920 B，
  sha256 **`6520c152c34b318e062236e0c62f475f50c216ae57775d795184d59a4d907364`**；
  新 `kernel.macho` 53,202,944 B，sha256
  **`c79e8f2f7854e147b2744b216486b100c8b5b2128ae05b4681a36aec89201237`**
  （旧 `e627d477…`）。`payload.img` 75,497,472 B（72 MiB）。
- 没跑 `firmware` 步：UEFI / DarwinBoot 未改，无需重建。

### 69.20.3 机器校验（`check-grant-rebuild.py`，全绿）

| 判据 | 结果 |
| --- | --- |
| 其它 14 个输入（board.toml / defconfig / 三个 seed / base96 / 两个构建脚本 / 内核 patch / IOUserClient.cpp / BravearkLastLog.c …） | sha256 **逐项不变** |
| 源码 diff | `+1 / -0`，且新增行含名单 |
| `kernel.json['candidate_sha256']` | == 新 `kernel.macho` sha |
| `strings(kernel.macho)` | 含名单 `backboardd,iconservicesagent,…`；旧 patch 的 `NativeSandboxGrantProcess` 仍在；**无** uke 的 `NativeSandboxGrantService` |
| 旧内核（冻结基线） | **不含**该名单 ⇒ 字符串确实是本次修复带来的 |
| `payload.img` / `vendor_boot` | 都含名单；大小分别 75,497,472 / 77,393,920 B |
| 三个构建日志 | 无 error/failed/fatal；`== kernel:` 报告新 sha |
| cmdline | 996 B，三个 GPU 参数齐全 |

`check-grant-mismatch.py` 改为**阶段感知**：pre-fix 断言「muyu-17 不发布该属性」，
post-fix 断言「发布且名单与 sheng 一致、位置在 `registerService()` 之前、diff 恰好一行」；
冻结的 pre-fix 内核作为「旧 patch 唯一编译版本」的证据。pre-fix 判据已归档为
`baseline-pre-grant1/grant-mismatch.pre-fix.json`。

### 69.20.4 交叉验证：QEMU 基线没被破坏，镜像差异只在 ramdisk

- `kernel.skip-owner.{macho,json}` **未被覆盖**，sha 仍 `c4c1420a…`（§69.17 的 QEMU EL0 链完好）。
- 关键符号地址**零漂移**：`_load_init_program=0xfffffff007753288`、
  `_native_apfs_probe_checkpoint=0xfffffff007728188`、`_vfs_mountroot=0xfffffff0074e7c2c`
  新旧同值；两个 `CBNZ` 补丁点 `0xfffffff00775352c / 0xfffffff007753738` 字节新旧同为
  `0x350034f6 / 0x35002595` ⇒ **若日后要在 QEMU 跑这份新内核，§69.17 的定点补丁仍适用**。
- `vkd7` vs `grant1` 镜像：header `0..0x1000`、DTB/tail `>=0x4801000` **逐字节相同**；
  差异全部落在 ramdisk 段 `0x13e8..0x400296a`；名单字符串只在 `grant1` 出现。

### 69.20.5 ⚠️ 待上机（**本轮到此为止，未执行任何刷机命令**）

镜像已就绪，**下一步需要上机，由用户决定何时执行**。届时按既有纪律：

1. 只写 `vendor_boot`，**不碰 `boot` / `recovery`**（`tools/muyu-flash-mode.sh` 正常模式即只写 vendor_boot）；
   设备必须在 fastboot；UEFI 走 `fastboot boot` RAM 引导，断电即失效。
2. 屏照必须含最顶部 `MUYU LIVE` 行。判据看三处：
   - **`MUYU R`**：`no device: Vulkan(VkResultCode(-3))` 是否消失（主判据）；
   - **`MUYU A`**：`re00002c7` / `re00002e2` 之类的 resolver 拒绝是否消失；
   - **`MUYU L / V`**：`pr`、`tn`、`sb`、`gr=` 与 `MUYU M s<publish stage>` 是否推进。
3. 若 `R` 行仍为 `-3`，则本根因候选**被证伪**，回到 §69.19.2 的剩余落点
   （注意输入敏感性 27 格与实例扩展 6 格已排除 `vkCreateDevice` 与实例层）。
4. 回 rec 充电前**必须先刷回** `fw/stock/vendor_boot.img`（`bash tools/muyu-flash-mode.sh stock`）。

### 69.20.6 边界、产物、回滚

诚实边界：**`board_verified=false`、`flashing=false`、`device_touched=false`、
`qemu_cells_run=0`**；`hardware_boot_demonstrated=false`、`os_boot_demonstrated=false`；
40 个 QEMU 格仍全部 `minus3_reproduced_in_qemu=false`；
`owner_cache_mount stage=1 / status=80 / authorized=0` 如实未改。
修的是**静态审计 + host 真 dylib 实测**得出的根因候选，**板上尚未验证**。

产物：`fw/vendor_boot_gpu_grant1.img`、`logs/build-{kernel,payload,vendor-boot}-grant1.log`、
`build/host-turnip-input/{grant-mismatch.json, grant-rebuild.json}`、
`UniXNU/out/muyu/kernel/baseline-pre-grant1/`（内核 / payload / 原始源码 / pre-fix 判据 / 14 个输入 sha）。
新增脚本 `UniXNU/scripts/check-grant-rebuild.py`；`check-grant-mismatch.py` 改为阶段感知。

回滚：把 `baseline-pre-grant1/GpuPowerProbe.cpp.orig` 覆盖回 `package/GpuPowerProbe.cpp`
并重跑三步链即可；或直接删除 `fw/vendor_boot_gpu_grant1.img`（不刷就没有任何设备影响）。

```bash
cd /Users/diannao/pad7pro-xnu
P=/opt/homebrew/bin/python3.12
$P UniXNU/scripts/check-grant-mismatch.py    # phase=post-fix，源码单变量
$P UniXNU/scripts/check-grant-rebuild.py    # 产物/输入/日志逐项校验
shasum -a 256 fw/vendor_boot_gpu_grant1.img
# 上机命令故意不写在这里：见 §69.20.5，等用户明确同意
```

---

## §69.21 2026-11-13 12:47：上机执行 —— `grant1` 已刷入并从 RAM 引导，**屏照待拍**

用户回复「设备在线」⇒ 明确授权上机。严格按 §2.1 红线执行，**只写 `vendor_boot`**，
`boot` / `recovery` / `userdata` 一字未动，未用 `fastboot reboot`。

### 69.21.1 只读预检（先查再动）

| 项 | 读数 |
| --- | --- |
| `fastboot devices` | `a20248f7  fastboot` ✅ |
| `adb devices` | 空 ✅ |
| `current-slot` | `a` |
| `slot-unbootable:a` | **`yes`** ⚠️（历史事故态：每次启动静默回退 fastboot） |
| `slot-retry-count:a` | 0 |
| `battery-voltage` | 3737 mV（>3700，未告警，但偏低） |

先跑 `tools/muyu-flash-mode.sh preflight`：`--set-active=a` 把
`slot-unbootable` 从 `yes` 修成 **`no`**（`logs/flash-ledger.tsv` 有记录）。
`status` 的 `getvar` 全部走脚本落文件，没有直接 `getvar | grep`（§2.1 第 4 条 SIGPIPE 红线）。

### 69.21.2 刷入 + RAM 引导（一次 `xnu` 完成）

```bash
cd /Users/diannao/pad7pro-xnu
MUYU_UEFI=fw/boot-17line-043f7120.img \
  tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_grant1.img
```

- `MUYU_UEFI` 必须指定 **17 线** UEFI 副本（sha `043f7120…5bd3`）；
  默认的 `UniXNU/out/muyu/boot/boot.img` 是 27 线，固件卷差 1.6 MB。
- 写入：`vendor_boot` v4，73.81 MiB（vendor_ramdisk 72.00 MiB），
  sha256 `6520c152…7364` 与 §69.20 产物一致，`OKAY [0.034s]` / total 1.803s。
- `fastboot boot fw/boot-17line-043f7120.img`：`OKAY [0.188s]` / total 0.237s，
  **无 `Load Error`**（脚本对此有专门拦截，出现就不会继续追刷）。
- 账本三条 `rc=0`：`flash vendor_boot` / `preflight` / `boot`，日志 `logs/flash-grant1.log`。
- 引导后 `fastboot devices` 与 `adb devices` 都变空 ⇒ 设备已离开 fastboot 跑 RAM 引导的 XNU，
  符合预期；**面板是唯一输出通道**（§2.1 第 6 条）。

### 69.21.3 ⭐ 现在只差屏照，判据不变

刷写与引导命令成功 **不等于** 根因被验证。`board_verified` 仍为 `false`：
必须等 ticker 刷 3–4 遍后拍**完整屏照**（含最顶部 `MUYU LIVE` 行），由助手逐行判读。

1. **主判据 `MUYU R`**：`no device: Vulkan(VkResultCode(-3))` 是否消失。
2. **`MUYU A`**：`re00002c7` / `re00002e2` 这类 resolver 拒绝是否消失。
3. **`MUYU L / V / M`**：`pr`、`tn`、`sb`、`gr=` 与 publish stage。
4. 若 `R` 行仍为 `-3` ⇒ §69.19 的 sandbox grant 根因候选**在板上被证伪**，
   回到 §69.19.2 的剩余落点（27 格输入敏感性 + 6 格实例扩展已排除
   `vkCreateDevice` 内部与实例层）。

机器可读的待判读记录：`build/host-turnip-input/grant1-flash.json`
（含预检、镜像/UEFI sha、账本三条、判据与边界）。

### 69.21.4 边界与后续纪律

状态相对 §69.20 的变化：`flashing=true`、`device_touched=true`；
**`board_verified=false`、`screen_photo_captured=false`、
`hardware_boot_demonstrated=false`、`os_boot_demonstrated=false`、`qemu_cells_run=0`** 不变。
两个 `false` 与 `owner_cache_mount stage=1/status=80` 如实不变。
本节只陈述「刷写/引导命令返回 OK」，**不陈述任何板上行为结论**。

- 电池 3737 mV 偏低；**回 rec 充电前必须先刷** `fw/stock/vendor_boot.img`
  （`bash tools/muyu-flash-mode.sh stock`，不引导 UEFI、不重启）。
- `fastboot reboot` 禁用；UEFI 断电即丢，下次继续须重刷诊断镜像。
- `boot` / `recovery` 只有 `restore-stock --yes` 能动，本轮未触发。

---

## §69.22 2026-11-13 12:50：`userdata` 首块 8 KiB **第二次**被写坏 —— 昨天的办法找到、已脚本化，成因收敛到「刷回原厂 recovery 充电」

用户报「根镜像被写坏了，昨天也有经历，找当时的解决办法」。本节**只读**：查文档/账本/留证、
把昨天的修复脚本化并做回归。**没有刷机、没有进 recovery、没有写 userdata、没有碰设备**。
设备只读状态：`fastboot devices = a20248f7 fastboot`、`adb devices` 空、
`slot=a unbootable=no`、`battery=3737 mV`。

### 69.22.1 ⭐ 昨天的解决办法（原样可复用，已验证载荷还在）

不是 `root-image` 整盘重刷（9 GB / ~3.5 min，且**没有证据**支持整盘被抹），
而是**定点写回 8192 B**（§57.1，2026-11-12 19:45，本工程第一次主动写 userdata）：

```bash
# 前置：设备 fastboot；已刷回 fw/stock/vendor_boot.img；从 RAM 引导进 stock recovery
#       （fastboot boot fw/stock/boot.img，不是 flash boot），/data 必须未挂载
adb push /tmp/ud_fix.bin /tmp/ud_fix.bin
adb shell 'sha256sum /tmp/ud_fix.bin'        # 1afe7d01…c78143d
adb shell 'dd if=/tmp/ud_fix.bin of=/dev/block/sda32 bs=4096 count=2 conv=notrunc; sync'
adb exec-out 'dd if=/dev/block/sda32 bs=4096 count=2' > /tmp/ud_verify.bin
```

- 载荷 = 备份 `fw/userdata-backup/userdata-root.sparse` 的 **chunk0 首 8192 B**：
  块 0 是 `SHNGROOT` manifest（version 1、2 窗口、`imageBytes 9382658048`、块内 sha256 自校验通过），
  块 1 全零（备份里本来就是零）。**只搬真实字节，不造值。**
- 目标块设备 `/dev/block/sda32`（recovery 下 `by-name/userdata` 已确认）。
- 昨天实测：**仅块 0/1 与备份不同，块 2–15 逐字节相同** ⇒ 损坏范围就是 8192 B。
- 修好后的判据：`BSD s=8`、`INIT q=6`、`ROOT c=6/0/0 d=4/0`、APFS `tx_flush` 活跃、用户态自起。
- 根因链：`NativeUfs: no valid root manifest in userdata (status 1, version 0, magic fd877808 ab9f4456)`
  → `startRoot()` 返回 false → 不发布 root 媒体 → `IOFindBSDRoot()` 死等 →
  `vfs_mountroot()` 从未被调用 → `INIT q=0` / `PROC n=1` / `ROOT` 全初值（§56）。

**本机资产逐项复核（全部完好，可直接复用）**：

| 资产 | 状态 |
| --- | --- |
| `/tmp/ud_fix.bin` | 8192 B，sha `1afe7d01…c78143d`，与 sparse chunk0 首 8 KiB **逐字节相同** |
| `/tmp/ud_verify.bin` | 与 `/tmp/ud_fix.bin` sha 相同（昨天的回读校验） |
| `fw/userdata-backup/userdata-root.sparse` | 8,946,477,720 B，sparse v1 / 4096 块 / chunk0 RAW 1024 块 / 首块 `SHNGROOT` |
| `logs/2026-11-12-userdata-head-clobbered.bin` | 昨天损坏态首 64 KiB 留证，首 8 字节 `087887fd56449fab` |
| `fw/stock/{boot,vendor_boot,recovery}.img` | 齐全（进 recovery 与充电前刷回用） |

### 69.22.2 ⭐ 成因收敛：昨天 §56.6 的「候选 ①」被这次复发坐实到时间线上

昨天留了两条候选（① 回 rec 充电时 stock recovery 挂载/格式化 `/data`；② 原厂恢复/抹除），
并注明「**没有证据**」。把两次事故与账本对齐后，① 成为唯一与全部事实自洽的解释：

| 时间（UTC） | 账本 / 事件 |
| --- | --- |
| `2026-11-12T11:45Z` | 第一次发现首块被改写（magic `fd877808…`），19:45 本地定点写回 8 KiB 修好 |
| `2026-11-12T14:48:34–40Z` | **`restore-stock --yes`：刷回原厂 `boot` + `vendor_boot` + `recovery` 三分区**（昨天收工充电前，账本三条 `rc=0`） |
| 之后 ~14 h | 长按电源关机 → 墙充；设备有机会进 **stock recovery**，而原厂 recovery 会按 Android 语义看待 `userdata` |
| `2026-11-13T04:47:19Z` | 上机前 `preflight` 发现 **`slot=a unbootable=yes`**（历史事故态：每次启动静默回退 fastboot） |
| `2026-11-13T04:47:29Z` | 刷 `grant1` + RAM 引导 17 线 UEFI（§69.21），屏照待拍 |
| `2026-11-13T04:50Z` | 用户报「根镜像被写坏了」 |

`tools/muyu-flash-mode.sh:218` 的 `cmd_restore_stock()` 自己就写着
「**Android still needs its own userdata: boot recovery and wipe there**」——原厂 recovery
被引导时确实会去挂/格式化 `/data`。而 §57.1 修好时明确记录「`/data` 当时**未挂载**，
只挂了 `/metadata`」，`NativeUfs` 的写入窗口在 userdata **尾部**（`NativeUfs.cpp:1069`），
本工程任何步骤都不碰首块 ⇒ 两次首块改写都发生在**同一个外部环节**。

⚠️ 仍是**时间线推断，不是板上取证**：本节没有读出设备当前首块，也没有 recovery 日志。
要坐实需在 stock recovery 里只读 dump 首 64 KiB 并与 §69.22.3 的诊断结果比对。

### 69.22.3 已脚本化（冷启动 §9 第 6 条 TODO 关闭）：默认只读、回归通过

新增 `UniXNU-muyu-17/scripts/repair-userdata-manifest.py`，五个子命令：

| 子命令 | 行为 | 碰设备 |
| --- | --- | --- |
| `prepare` | 从 sparse 生成 8192 B 载荷到 `build/userdata-manifest/ud_fix.bin`，校验 manifest 版本/窗口/自校验、断言 sha == 昨天现场值 | ❌ |
| `diagnose --head X` | 对只读 dump 的 64 KiB 逐块比对备份，报首 8 字节、损坏块列表、是否仅 0/1 | ❌ |
| `verify --readback X` | 校验回读 8192 B 与载荷一致 | ❌ |
| `commands` | **只打印**完整修复命令，绝不执行 | ❌ |
| `fix --apply --yes` | 推载荷 → 远端 sha 校验 → `/data` 挂载检查 → `dd` 写 8192 B → 回读校验 | ✅ 仅此一处 |

安全闸：`fix` 缺 `--apply --yes` 直接拒绝；执行前检查 `adb shell mount` 里 `/data` 未挂载；
不执行 `fastboot flash/reboot`，不写 `boot`/`recovery`。

**回归（用昨天真实留证）**：

- `prepare` → sha `1afe7d01…c78143d`、`matches_2026_11_12_fix=true`、`device_written=false`；
- `diagnose --head logs/2026-11-12-userdata-head-clobbered.bin` → `first_8_bytes=087887fd56449fab`、
  `differing_blocks=[0,1]`、`blocks_2_to_15_match_pristine=true`、
  `verdict=manifest-only (blocks 0/1)` ⇒ 建议 8 KiB 定点修复（与昨天人工判读逐项一致）；
- `diagnose --head` 用备份自身前 64 KiB → `verdict=ok` ⇒ 建议「无需修复」；
- `verify --readback /tmp/ud_verify.bin` → `matches_payload=true`。

### 69.22.4 待执行（**本轮到此为止**）

如需修复，顺序不可颠倒：

1. **先只读取证**：刷回 `fw/stock/vendor_boot.img` → `fastboot boot fw/stock/boot.img` 进 stock recovery
   → 确认 `/data` 未挂载、`/dev/block/sda32` 存在 → `adb exec-out dd … count=16` dump 64 KiB
   → `diagnose`。**只有 `verdict=manifest-only (blocks 0/1)` 才允许写**；若损坏更宽，
   停下来重新评估，不要盲目重刷 9 GB。
2. 再 `fix --apply --yes`（或按 `commands` 打印的命令手工执行），回读必须 `matches_payload=true`。
3. 成功后**重刷诊断镜像**（`fw/vendor_boot_gpu_grant1.img` + 17 线 UEFI RAM 引导）才能继续 §69.21
   的 `MUYU R / A / L / V / M` 判读 —— 首块坏的时候连 root 媒体都不发布，屏照不会有 GPU 行。
4. **修好后不要再跑 `restore-stock`**：充电前只用 `tools/muyu-flash-mode.sh stock`
   （只写 `vendor_boot`、不引导、不写 `boot`/`recovery`）；若必须进原厂 recovery，
   出来后第一件事就是重跑 `diagnose` 检查首块。

### 69.22.5 边界与产物

`flashing=false`（本轮）、`userdata_written=false`、`device_state_changed=false`、
`board_manifest_read=false`、`screen_photo_captured=false`；
§69.21 的 `board_verified=false` 不变，两个 `false` 与 `owner_cache_mount stage=1/status=80` 如实不变。
本轮只是**找到办法 + 脚本化 + 时间线推断**，没有对设备做任何动作。

产物：`UniXNU-muyu-17/scripts/repair-userdata-manifest.py`、
`UniXNU/out/muyu/tree/build/userdata-manifest/{ud_fix.bin,prepare.json,diagnose.json}`。
文档 `GPU-HANDOFF.md` §69.22、`HANDOFF-COLD-START.md` 导航、今日工作日志。

```bash
cd /Users/diannao/pad7pro-xnu
P=/opt/homebrew/bin/python3.12; S=UniXNU-muyu-17/scripts/repair-userdata-manifest.py
$P $S prepare                                   # 本地生成载荷
$P $S diagnose --head logs/2026-11-12-userdata-head-clobbered.bin   # 用昨天留证回归
$P $S commands                                  # 打印完整修复命令（不执行）
```

## §69.23 2026-11-13 13:00–13:20：第二次首块损坏已修复并重新引导 —— 官方 rec 无 root，改走 fastboot 通道

用户指令「直接修复来看吧，昨天的 rec 是 TWRP、天然 root，但现在是官方 rec」→「直接修，不管是不是同因」：
**跳过 §69.22 的「先只读取证、仅 manifest-only 才写」判据**，直接修。

### 69.23.1 ⭐ §57.1 的 adb dd 路径在官方 rec 下不可复现

刷回 `fw/stock/vendor_boot.img`（`stock` 模式，只写 vendor_boot，ledger `05:05Z`）、
`fastboot boot fw/stock/boot.img` RAM 引导后 `adb devices = a20248f7 recovery`，但：

| 项 | 本次官方 rec | 昨天 TWRP |
|---|---|---|
| `adb shell id` | `uid=2000(shell) … u:r:shell:s0` | root |
| `adb root` | `adbd cannot run as root in production builds` | 不需要 |
| `ro.secure` / `ro.debuggable` / `ro.build.type` | `1` / `0` / `user` | — |
| su 二进制 / `getenforce` | 无 / **Permissive** | 有 |
| `/dev/block/by-name/userdata` | → `/dev/block/sda32`（`brw------- root:root`） | 同 |

⇒ `adb exec-out 'dd if=/dev/block/sda32 bs=4096 count=16'` 与 `adb shell dd` 都是
`Permission denied`（dump 只有 108 B 报错），**既读不了也写不了**。
SELinux Permissive 帮不上忙：这是 DAC。昨天能写是因为 TWRP 天然 root，不是因为镜像或命令不同。
工程内 `~/Downloads/recovery.img`（100 MiB）就是 10-28 事故那个坏 dump
（v4 头 `kernel_size=0` / `ramdisk≈95 MB`），**不可用**，不要拿它 RAM 引导。

### 69.23.2 改走 fastboot：定点 sparse + 整盘兜底，全程唯一入口

绕开 recovery root，用 `tools/muyu-flash-mode.sh root-image … --yes` 的 sparse 校验
（它读 chunk0 首 4096 B 校验 `SHNGROOT` 与窗口布局，所以**定点载荷也能通过**）：

1. **定点 8 KiB**：构造 sparse（`header 28 + RAW chunk 12 + 8192 B`，`total_blks=2 / chunks=1`），
   补零到 12288 B 对齐 → `UniXNU/out/muyu/tree/build/userdata-manifest/ud_fix_8k.sparse`
   （sha `4cd0a473…5ad8de`）。载荷仍是 `ud_fix.bin` sha `1afe7d01…c78143d`。
   `root-image … --yes` → `Writing 'userdata' OKAY [0.001s]`，ledger `05:14:02Z rc=0`。
   ⚠️ **没有回读验证**：`fastboot fetch userdata:0:8192` →
   `Unable to get max-fetch-size. Device does not support fetch command.`（与 PLAN.md §7 一致）。
   0.001 s 无法证明确已落盘。
2. **整盘兜底**（用户「不管是不是同因」）：`root-image fw/userdata-backup/userdata-root.sparse --yes`
   → `Writing 'userdata' OKAY`，`Finished. Total time: 202.047s`，ledger rc=0。
   8.74 GiB / 2237 chunks，`windows OS@0x100000+6077580800 / SystemOS@0x16a600000+3303014400`
   与备份逐字相同，sha `4c90d7a7…19a3`；尾部 persistent Data 窗口未动。
   这一步覆盖所有可疑区域，不依赖第 1 步是否生效。
3. **重刷诊断镜像**：`MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu
   fw/vendor_boot_gpu_grant1.img` → `flash vendor_boot OKAY [0.034s]`、
   `Booting OKAY [0.186s]`，ledger `05:20:01Z` 三条 `rc=0`。引导后 **fastboot/adb 皆空**
   ⇒ 已跑 RAM 引导的 XNU，面板是唯一输出通道。

`boot` / `recovery` 一字未动；未用 `fastboot reboot`；`restore-stock` 未触发。

### 69.23.3 待判读（**本轮到此为止**）

拍**完整屏照（含最顶部 `MUYU LIVE` 行）**，ticker 走 3–4 遍后：

- 根镜像恢复判据：`BSD s=8` / `INIT q=6` / `ROOT c=6/0/0 d=4/0`、
  不再出现 `NativeUfs: no valid root manifest in userdata`。
- §69.21 主判据：`MUYU R` 的 `no device: Vulkan(VkResultCode(-3))` 是否消失
  （`grant1` 已把 `NativeSandboxGrantProcess` 发布到 accelerator）；
  辅以 `MUYU A` 的 `re00002c7/re00002e2`、`MUYU L/V/M`。
- 若仍无 root 媒体：整盘都已刷回，成因不在首块，需另查。

⚠️ **充电**：`fastboot` 本身不充电，充电前须 `tools/muyu-flash-mode.sh stock` 刷回原厂
vendor_boot；但 §69.22 推断「刷回 stock + 长时间墙充」正是本次损坏的成因，
**尽量不要让设备在 stock 状态下长时间墙充**，也不要再跑 `restore-stock --yes`。
电池 3738 mV 偏低。

### 69.23.4 边界与教训

`flashing=true / userdata_written=true / device_state_changed=true`；
`board_manifest_read=false`（官方 rec 读不到首块）、`board_verified=false`、
`screen_photo_captured=false`，两个 `false` 与 `owner_cache_mount stage=1/status=80` 不变。

教训：① `§57.1` 的 adb dd 修复**只在 TWRP 下成立**，冷启动文档不要再把它写成通用办法；
② 官方 rec 下板上取证/回读都要 root，而 `fastboot fetch` 这台设备不支持 —— 以后
要么先备好可用 TWRP，要么用 fastboot sparse 定点写 + 整盘兜底；
③ `root-image` 的 manifest 校验允许「只含前 2 块」的 sparse，可作为定点写通道。

## §69.24 2026-11-13 13:24：`grant1` 首张板上读数 —— **`-3` 仍在，§69.19 方案 1 被证伪**；`calls/refused/o` 语义钉死

用户口述读数（未上传屏照文件）：`MUYU R` 仍 `VkResultCode(-3)`，
`MUYU GPU calls=1 refused=0 o=1`。

### 69.24.1 ⚠️ `calls/refused/o` 不是 sandbox 拒绝计数，不能用来判 `-3`

`LegacyDisplayProbe.cpp:87-98` 的 `muyu_handoff_snapshot()` 源码逐字：

```c
"calls=%u refused=%u o=%llu c=%llu f=%llu fb=%u tc=%u ts=%08x hold=%d pres=%d"
  calls   = native_legacy_gpu_offer_calls      // GOP 帧 offer 调用次数（每帧一次）
  refused = native_legacy_gpu_offer_refused    // native_legacy_gpu_offer() 返回 false 的次数
  o       = native_gop_gpu_offers              // NativeGOPPresenter 实际发出的 GPU offers
  c/f     = completions / failures,  fb = fallbacks, tc/ts = transfer calls/status
  pres    = gpuHandoffPresenter ? 1 : 0
```

`native_legacy_gpu_offer()`（同文件 `:69-79`）只在 `gpuHandoffPresenter && native_gop_gpu_offer(...)`
为假时累加 `refused`。⇒ **整行是显示帧 hand-off 计数，与 `IOServiceOpen` /
MACF / Turnip / Vulkan 无关**。冷启动 §9 第 2 条（§59 结案）、`:416`、`:470` 的解读正确；
§6.3.1 `:294` 与状态表 `:180` 里「`calls=7 refused=7` ⇒ 每条 offer 都被内核拒绝 / GPU 服务拒绝客户端」
是**过时误读**，以本节为准。

读数含义（⚠️ **本段已由 §69.25.6 更正，原判「ticker 只跑一遍、代表性不足」是错的**）：
`refused=0 o=1` = 这次 offer **被接受**且 presenter 发出了 1 次 GPU offer，
相对 §57 的 `calls=7 refused=7 o=0`（7 帧全被拒、走 CPU copy、scanout 没交出去）
是**显示链的实质改善**，但**既不能证明内核放行，也不能解释 `-3`**。

> **§69.25.6 更正**：屏照 `MUYU LIVE p=12 t=561`（已跑 9 分 21 秒）却 `calls=1`，
> 且 `tc=0 ts=ffffffff`（零 CPU transfer、transfer status 仍是初值 -1）⇒
> **不是采样偏差，而是首次 offer 成功后显示路径真的切走了**。
> 但 `c=0` 同时说明 `completeSwap()` 从未发生、`transferred` 从未清除，
> 队列冻结在第一帧 —— 详见 §69.25.6 的完整机制与「这一版改变了什么」。

### 69.24.2 ⭐ 按 §69.21.3 第 4 条：`-3` 未消失 ⇒ §69.19 单变量根因在板上被证伪

`grant1` = §69.19.5 方案 1（只补 `accelerator->setProperty("NativeSandboxGrantProcess", ...)`，
一行 diff，内核/payload/vendor_boot 全量重建并已刷入引导）。板上 `-3` 仍在 ⇒
**「muyu-17 从未发布该属性」不是这条 `-3` 的充分原因**。
`§69.19` 从「根因」降级为「已证伪候选」；`board_verified` 仍 `false`
（没验证出任何正结论），但新增 `board_observation_received=true`。

### 69.24.3 为什么方案 1 可能不够：uke 审计指出的第二个 mismatch 还没修

`IOUserClient.cpp` 里三道门全部由 `native_sandbox_grant(service)` 短路：
`:4390` 入口 `mac_iokit_check_open_service`、`:4550` `mac_iokit_check_open`、
`:4559` `io_filter_resolver`（`re00002c7` / `re00002e2` 的来源）。
而 `native_sandbox_grant()`（`:4339-4360`）**只**读 `service->getProperty("NativeSandboxGrantProcess")`
并与 `proc_selfname()`（16 字节）比对名单。

`UniXNU-uke/docs/GPU-PORT.md:566-575` 原文：「core patch only honors
`NativeSandboxGrantProcess` on the accelerator service, while the driver sets
`NativeSandboxGrant` on the **client object**; **an audit found the mismatch**」。
修正补丁 `UniXNU-uke/patches/xnu/zzzzzzzzzzzz-uke-gpu-sandbox-client-grant.patch` 新增
`native_client_sandbox_grant(client)`（读 client 的 `NativeSandboxGrant` OSBoolean）与
`native_service_sandbox_grant(service,type)`（读 service 的 `NativeSandboxGrantService`，
**只放行 `0x4147` 插件 / `0x4148` hand-off**），插入全部三道门。
`UniXNU-uke/docs/RECOVERY-LOG.md:148`：`o<opens>` 一直 0 时要同时检查两个属性。
⇒ grant1 只补了名单，**没编入 uke 修正补丁、没发布 `NativeSandboxGrantService`、
没覆盖真实 opener 进程名**，仍可能被其中任一门拦下。

### 69.24.4 ⚠️ 下一步必须先分叉，不要直接改内核

缺 `MUYU A` 行就无法区分下列两支，**补屏照前不动内核**（§68.3 单变量纪律）：

| `MUYU A` | 结论 | 下一步 |
|---|---|---|
| 仍有 `re00002c7` / `re00002e2` | 内核三道门仍拒 ⇒ 方案 1 确实没放行 | 执行 §69.19.5 **方案 2**：移植 uke 修正补丁 + `accelerator->setProperty("NativeSandboxGrantService", kOSBooleanTrue)`，并核对 opener 进程名是否在名单内（必要时补名）；全量重建后再上机 |
| 已干净（无 resolver 拒绝） | 内核已放行，`-3` 在用户态设备创建链 | **不要改内核**（白费）；回到冷启动 §9 第 3 条，读 `MUYU Q` / `Y` 的调用序列形状定位 `vk*` 失败步 |

> ⚠️ **本表已由 §69.25.2 更正，不要按它执行**。屏照回来后逐字核对源码：`kIOReturnUnsupported`
> (`0xe00002c7`) 在 `IOUserClient.cpp:4566-4567` 被显式转成 `kIOReturnSuccess`；`A` 行的
> `m`/`r` 只是驱动在 `newUserClient` 里手动模拟两道 gate 后写进日志的值，不是 open 的返回。
> 所以「仍有 `re00002c7`」**不能**推出「三道门仍拒」，方案 2 暂缓。真正被拒的 open 见
> `MUYU B f1 s=e00002c2 n5` 与 `MUYU G …/xe00002d5`（§69.25.3）。

请补：**完整屏照（含顶部 `MUYU LIVE`，ticker 3–4 遍后）**，或至少逐字抄
`MUYU A`、`MUYU V`（`gr=`/`sel=`/`st=`）、`MUYU R`、`MUYU B`、`MUYU O`（opener 名）五行。
`O` 行尤其关键：`native_sandbox_grant()` 用 `proc_selfname()` 比对名单，
真实 opener 若不是 `backboardd/iconservicesagent/WidgetRenderer_Default/PosterBoard`
（例如 own EL0 fixture 的进程名），名单再全也不会命中。

### 69.24.5 边界

本轮**零设备动作、零重建**（纯判读 + 源码/补丁审计）。
`flashing=true / userdata_written=true / device_state_changed=true` 不变；
`board_verified=false / hardware_boot_demonstrated=false / os_boot_demonstrated=false /
qemu_cells_run=0 / owner_cache_mount stage=1/status=80` 如实不变；
新增 `board_observation_received=true`、`grant1_root_cause_falsified=true`。
设备仍在 RAM 引导的 XNU 上（fastboot/adb 皆空）。

---

## §69.25 2026-11-13 13:28：`grant1` 完整屏照判读 —— ⭐ **`B f1 s=e00002c2 n5` 把失败点钉在 `createVm`，sandbox 链源码级证伪**

屏照 `logs/2026-11-13-13-28-19-084.jpg`（3840×2160，13:28 落盘）。逐字转录关键行
（放大分区逐行核对；`i/o/f/s/b/n` 与 `e00002c2` 已二次放大确认）：

```
MUYU LIVE pr=2 tn=1 … t=9m4s …
MUYU GPU  calls=1 refused=0 o=1 c=0 r=0 f=0 tb=0 tr=ffffffff hold=1 pres=1
MUYU PROBE st=97 cp=10 b=24
MUYU B     i48 o40 f1 s=e00002c2 b0 n5
MUYU V     op=2 bad=0 cl=1 call=526 sel=6 st=e00002d6 at=1 inf=0 mb=0/0 tr=30 gr=7
MUYU O     SpringBoard|SpringBoard|backboardd
MUYU A     n=10 SpringBoard/t4147/m00000000/re00002c7/a1/f0  (×2)
MUYU G     backboardd/o2/r2/c1/p…/xe00002d5  mediaServerd/o0/r1/c8/p0/xe00002d5  n3
MUYU R     PUB no device: Vulkan(VkResultCode(-3))
MUYU ROOT  c=6/0/0 d=4/0
MUYU INIT  q=6
MUYU BSD   s=8
TOC        n=127 zomb=0 Launchd=18 backboardd=2358 SpringBoard=24…
```

### 69.25.1 ✅ 根镜像整盘刷回已板上确认

`ROOT c=6/0/0 d=4/0`、`INIT q=6`、`BSD s=8`、`TOC n=127` 与 §69.21 主判据逐项一致，
且无 `NativeUfs: no valid root manifest in userdata`。§69.23 的整盘刷回
（`fw/userdata-backup/userdata-root.sparse`，`OKAY / 202.047s`）**有效**，
`userdata_restored_board_verified=true`。§69.22 那次「首块被写坏」到此闭环。

### 69.25.2 ⚠️ 更正 §69.24.4 的判据表：`re00002c7` 不是「内核门仍拒」

上一轮把「`A` 行仍有 `re00002c7`」写成「内核三道门仍拒 ⇒ 走方案 2（移植 uke 内核补丁）」。
**这条判据是错的**，三条源码事实：

1. `IOUserClient.cpp:4566-4567`：`res = io_filter_resolver(...); if (kIOReturnUnsupported == res) res = kIOReturnSuccess;`
   —— `0xe00002c7`（`kIOReturnUnsupported`，`IOReturn.h:154`）在内核 open 路径里被**显式当作成功**。
2. `NativeAdrenoPublish.cpp:1056-1072`：`A` 行的 `macf` / `res` 是驱动在 **`newUserClient` 里手动调用**
   `mac_iokit_check_open()` 与 `io_filter_resolver()` 后**只写进日志**的值，不是 `IOServiceOpen` 的返回值；
   能进 `newUserClient` 本身已说明 `:4390` 的 `mac_iokit_check_open_service` 放行。
   本屏 `m00000000`（MACF=0）、`:4550` 同式 ⇒ 第二道门也通过。
3. `gr=7` 是 `NativeAdrenoPublish.cpp:1096` 的驱动侧 `grants++`（§69.19.3 已更正过一次），
   既不是内核放行证据，也不是拒绝证据。

⇒ **方案 2 暂缓**：`kIOReturnUnsupported` 本来就不会让 open 失败，改内核 sandbox patch 是白费一次重建+上机。
`grant1` 名单确实漏了真实 opener `SpringBoard`（`A` 行 `proc_selfname()`，名单只有
`backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard`），但这只影响
`native_sandbox_grant()` 对 `:4390` 的短路；既然 `mac_iokit_check_open`=0 且 Unsupported→Success，
短路与否不改变结果。**§69.19 的整条因果链在内核源码层面被证伪**，不只是板上 `-3` 没消失。

### 69.25.3 ⭐ 真正被拒的 open 在驱动 `mount()` 层，不在内核 sandbox 门

| 行 | 读数 | 源码定位与含义 |
|---|---|---|
| `B` | `i48 o40 f1 s=e00002c2 b0 n5` | `native_adreno_mount_fail[0]=1` ⇒ **无地址空间**（`BravearkLastLog.c:672-683`）；`[1]=0xe00002c2` = **`kIOReturnBadArgument`**（`IOReturn.h:147`）；`[3]=5` ⇒ **失败了 5 次** |
| `G` | `backboardd/o2/r2/…/xe00002d5`、`mediaServerd/o0/r1/…/xe00002d5 n3` | `last_refusal=0xe00002d5` = `kIOReturnBusy`，正是 `NativeAdrenoPublish.cpp:1107` `mount()` 失败后 `refusal=kIOReturnBusy` 的返回码；backboardd 2 次、mediaServerd 1 次被拒 |
| `V` | `op=2 bad=0 call=526 at=1 tr=30` | 另有连接成功并跑了 526 次 external method ⇒ 失败是**部分进程/早期时序**，不是全局 GPU 不可用 |
| `R` | `VkResultCode(-3)` | 客户端 `vkEnumeratePhysicalDevices` 仍失败 |

链条：`createVm()`（`NativeAdrenoGpuContext.cpp:157-205`，`i48/o40` 下
`NativeAdrenoPrepareSmmuMemory` / `vm_.attach` / `vm_.map` 返回 `kIOReturnBadArgument`）
→ `mount()` 失败 → `IOServiceOpen` 返回 `kIOReturnBusy` → Turnip `dw_open_connection`
→ `vkEnumeratePhysicalDevices` → `-3` → `create.rs:735 .map_err(vulkan)`
→ `R PUB no device: Vulkan(VkResultCode(-3))`。

这与 §69.19.4 的**形状**一致（"某一次 `IOServiceOpen` 根本没到隧道那一层"），
但**层次错了**：不是 `is_io_service_open_extended` 的 MACF/filter 门，而是驱动的 `mount()`。
`grant1` 补的 service 属性完全不在这条链上 ⇒ `-3` 不动是必然的。

**§57.6 判读格第 2 行命中**（`-3` 不变 + `B` 行 `f` 出现 1/2/3 ⇒ 内核侧窗口/地址空间问题浮出，
按 `mount_fail[0]` 的 1/2/3 分支）。对照 §59 那张 `B i48 o40 f0 s0 b0 n0`（mount 从未失败）
同样有 `-3` ⇒ `createVm` 失败是**本次新出现的**变量（重建/整盘刷回后的启动时序），
不能定为唯一根因，只能定为**当前最贴合的可观测失败点**。

### 69.25.4 下一版单变量（先离线，不重建、不上机）

1. 把 `native_adreno_table_switches[6]/[7]` 打上屏（`BravearkLastLog.c:917-919` 注释已写明：
   `[6]` = VM 构建失败原因，`[7]` = 最后 VM stage，`2 prepare / 3 attach / 4+i mapping buffer i / 0x30|op / 0 done`）。
   这是**纯观测**改动，不改内核/驱动行为，一次重建即可把 `0xe00002c2` 定位到 `createVm` 的确定一步。
2. 同步核对 `i48 o40` 窗口参数：`GpuPowerProbe.cpp:1124-1125` 的
   `NativeAdrenoGpuContextInputBits()` 与固定 40，`1<<48` 的 aperture / `aperture/4096` 页数
   是 `NativeAdrenoPrepareSmmuMemory` 的入参，§21.2 已证 `31` 会因「无窗口」被拒。
3. **不要**再改 sandbox 属性、不要移植 uke 补丁、不要重跑 §69.18 的 selector 矩阵。

### 69.25.5 边界

本轮**零设备动作、零重建**（拍屏 + 逐字判读 + 源码审计）。
`board_verified=false` 保持，新增 `userdata_restored_board_verified=true`、
`sandbox_grant_root_cause_falsified_at_source=true`、`mount_fail_observed=true`、`board_screen_captured=true`。
设备仍在 RAM 引导的 `grant1` XNU 上（fastboot/adb 皆空）。

### 69.25.6 ⭐ 用户质疑「`calls=1` 有代表性吗」后的复核：是质变，但机制是「成功首帧后冻结」

用户指出：此前几十次 `calls` 全部 `refused` / `o=0`，所以 `calls=1` 不该被当成
「ticker 只跑一遍」，而应理解为「一次成功就不必继续 call」。**这个质疑成立**，
§69.24.1 那句「建议等 3–4 遍再拍」已就地划掉。逐字核对与源码：

```
MUYU LIVE p=12 t=561 …
MUYU GPU calls=1 refused=0 o=1 c=0 f=0 fb=0 tc=0 ts=ffffffff hold=1 pres=1   ← 本版
MUYU GPU calls=8 refused=8 o=0 c=0 f=0 fb=0 tc=8 ts=00000000 hold=1 pres=1   ← §59 历史（N=3..9 全同形）
```

| 证据 | 判读 |
|---|---|
| `t=561`（9 分 21 秒）而 `calls=1` | **不是采样不足**；跑了 9 分钟只有一次 offer 调用 |
| `tc=0` + `ts=ffffffff`（初值 -1） | **零 CPU transfer**，历史 `tc=N ts=00000000` 是「每次 offer 失败都补一次 CPU copy」⇒ 显示路径真的切到 GPU 了 |
| `refused=0 o=1` | 首次 offer 即被 `NativeGOPPresenter::offer()` 接受（`:131-142`） |
| `f=0 fb=0 hold=1 pres=1` | 零 failure / 零 fallback，presenter 一直在（`pres=1` 历史也是 1，所以差异不在 presenter 是否存在） |

**`o=1` 的成立条件比「显示链改善」精确得多**（`NativeGOPPresenter.cpp:134`）：
`accepted = online && gpuClient && !swap.offered`，而 `gpuClient` **只在 `waitSwap()` 里置 true**
（`:150`）。⇒ 本版第一次出现了**一个用户态 GPU 客户端真的连上 presenter 并调用 `waitSwap` 等帧**；
历史所有 `o=0` 都意味着从没有人等帧。这是「这一版改变了什么」的实质答案。

⚠️ **但机制不是「一次成功就收工」**：`c=0` 说明 `completeSwap()`（`:179-187`）从未执行，
因此 `native_legacy_gpu_complete()` → `finishGpuSwap()` 从未跑，`transferred` 从未清、
`kickSwapQueue()` 从未触发（`NativeLegacyDisplay.cpp:365-383`；成功路径 `status==0` 会清
`transferred` 并继续下一帧，此时 `calls`/`o`/`c` 都应继续涨）。
⇒ `calls=1 o=1 c=0` 是 **hand-off 成功后卡死在第一帧**，健康的 GPU 接管应是
`o>=1 且 c>=1`。这恰好与 `R PUB -3` 自洽：客户端拿到帧却没有可用的 GPU 设备去完成 copy。
**`-3` 的位置由此收窄到「拿到帧之后、完成 swap 之前」的设备/上下文初始化**，
而不是连接建立，也不是帧 hand-off。

**这一版究竟改了什么（不能单变量归因，如实写）**：相对所有历史 `o=0` 屏照，同时变了两个输入 ——
① `vendor_boot = grant1`（唯一 diff `+NativeSandboxGrantProcess`）；② `userdata` 整盘刷回
（根镜像从坏→好，`ROOT c=6/0/0`、`TOC n=127`、SpringBoard/backboardd 均在）。
§69.21 那次 `grant1` 上机时根镜像仍是坏的且**没拍屏照** ⇒ 缺一个「grant1 + 坏根」对照格。
按 §69.25.2 的源码证据，① 不影响 open 门；② 让完整用户态（含 GPU 客户端）起来，
是「第一次有人调 `waitSwap`」的最可能成因，但**不能排除** ① 改变了某进程的启动路径。
不为了坐实这一点再去写坏根镜像 —— 代价远大于收益。

⇒ 净结论：**本版清掉了显示侧 blocker（`o=1`），暴露出剩下唯一的 blocker 是 GPU 设备创建
（`-3` / `B f1 s=e00002c2 n5` 的 `createVm`）**。这支持 §69.25.4 的下一版方向不变，
并新增一条判读格：下一版若 `o>=1 且 c>=1` 且 `R` 变 `ok` 才算真通；
若 `o=1 c=0` 不变，就确认卡在设备创建而非 hand-off。

---

## §69.26 2026-11-13 13:5x：按 §69.25.4 落纯观测单变量并离线重建 —— **`stage1` 镜像已就绪，停在「待上机」**

用户指令「继续」（承接 §69.25 末尾「要我现在按这条做离线重建吗」）：执行 §69.25.4 的
**纯观测**改动与三步离线重建，**不刷机、不碰设备、不改任何驱动/内核行为**；到上机环节停下。
⚠️ **这一版不是修复版**：`-3` 大概率仍在，本轮的全部价值是让下一张屏照把
`0xe00002c2` 定位到 `createVm()` 的确定一步。

### 69.26.1 单变量：`MUYU B` 行追加四个只读字段

改 `UniXNU-muyu-17/package/BravearkLastLog.c:679`（`kits/muyu` symlink 指向这一份，
由 `build-boot-log.py` 编成 `libBravearkLastLog.a` 链入内核），把原有的 6 字段行

```c
printf("MUYU B i%u o%u f%u s%x b%x n%u\n", ...);
```

改为

```c
printf("MUYU B i%u o%u f%u s%x b%x n%u t6=%x t7=%x ok=%u vm=%u\n", ...,
    have_vm_stage ? (unsigned)native_adreno_table_switches[6] : 0xffffffffu,
    have_vm_stage ? (unsigned)native_adreno_table_switches[7] : 0xffffffffu,
    have_vm_stage ? (unsigned)native_adreno_table_switches[2] : 0xffffffffu,
    have_vm_stage ? (unsigned)native_adreno_table_switches[4] : 0xffffffffu);
```

| 字段 | 语义（`NativeAdrenoGpuContext.cpp:157-205`） |
| --- | --- |
| `t6` | `[6]` = `createVm` 最后一次失败的 IOReturn（板上预期 `e00002c2`） |
| `t7` | `[7]` = 停在哪一步：`2` prepare / `3` attach 进入 / `0x30\|op<<8` attach 失败 / `4+i` 映射 buffer i / `0` 完成 |
| `ok` | `[2]` = 历史成功 `createVm` 次数（`0` ⇒ 本次启动一个都没成功） |
| `vm` | `[4]` = 当前存活 VM 数 |

- diff 严格 **`+13 / -2`、1 个 hunk**，全部落在 `MUYU B` 那一个 printf 块；
  **零新增 `printf`**（控制台只容纳一页，新增一行会把别的行挤掉）、
  **零写入驱动全局**（`weak_import` 只读，符号未链接时打印 `ffffffff`）。
- 先验证符号真的能绑上：`nm -a kernel.macho` 里
  `_native_adreno_table_switches = fffffff007bfaf68 S`，与
  `_native_adreno_mount_fail` / `_muyu_gpu_bits` 同格可见 ⇒ 四个字段不会全是 `ffffffff`。
- `GpuPowerProbe.cpp` 未动 ⇒ §69.20 的 `grant1` 名单保留；未移植 uke 补丁；
  boot-arg 一个没加/删。

### 69.26.2 静态归因：`0xe00002c2` 只可能来自 `prepare` / `attach` / `map`

`mount()`（`NativeAdrenoPublish.cpp:366-381`）里 `f=1` 是 `space?2:1`，
即 `space==nullptr` ⇒ `createVm(&space)` 失败且没有产出 VM。`createVm` 内
`kIOReturnBadArgument` 的**唯一可写路径**是 `:187-188` 的
`native_adreno_table_switches[6]=uint32_t(status)`，其 `status` 来自
`:169` `NativeAdrenoPrepareSmmuMemory` / `:179` `vm_.attach` / `:185` `vm_.map`。
（`if(!out)return kIOReturnBadArgument` 不可能触发；`!ready_` 返回的是 `NotReady`。）

⭐ **为什么 `t7` 必需**：`NativeAdrenoSmmuMemory.cpp:134-135` 把
`ArmSmmu4KLivePrepare` 的**任何非 `ArmSmmu4KCapacity` 失败**都折成
`kIOReturnBadArgument`：

```c
ArmSmmu4KStatus status=ArmSmmu4KLivePrepare(arena,state);
if(status!=ArmSmmu4KSuccess)return status==ArmSmmu4KCapacity?kIOReturnNoResources:kIOReturnBadArgument;
```

`layout()`（`ArmSmmu4KLive.c:11-26`）的 `Argument` / `BadRange` / `Budget`，
以及 `ArmSmmu4KLivePrepare:330-340` 的 `Argument` / `StateMismatch`（root 页非零），
**全部收敛成同一个 `0xe00002c2`**。⇒ 单靠 `B s=` 无法区分，`t7=2` 才能把它
钉在「SMMU 表页 / arena 形状准备」而不是 sandbox grant。

### 69.26.3 三步重建（§69.20.2 参数原样）

```bash
cd /Users/diannao/pad7pro-xnu
python3 UniXNU/unixnu build kernel -D UniXNU-muyu-17 \
  --uefi /Users/diannao/pad7pro-xnu/Mu-Silicium \
  --edk2 /Users/diannao/pad7pro-xnu/Mu-Silicium/Mu_Basecore -j 8 \
  > logs/build-kernel-stage1.log 2>&1
python3 UniXNU/unixnu build payload -D UniXNU-muyu-17 -j 8 \
  --data-ramdisk fw/data-seed.img \
  --owner-trust-cache fw/trust-21H461/owner-trust-17-full.bundle \
  --gpu-addon fw/gpuaddon-17-full.bin \
  --unique-chip-id 0000000000000001 \
  --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-addon=1 --kernel-arg sheng-gpu-metal=1 \
  > logs/build-payload-stage1.log 2>&1
python3 UniXNU-muyu-17/scripts/patch-vendor-boot.py \
  --base /tmp/base96.img --payload UniXNU/out/muyu/payload/payload.img \
  --output fw/vendor_boot_gpu_stage1.img > logs/build-vendor-boot-stage1.log 2>&1
```

| 产物 | 值 |
| --- | --- |
| `kernel.macho` | 53,202,944 B，sha **`c8ca2bc1c5ccb90d22e3d1bc3cc6932abc7ddbf90f43b0a0823bd273b9875836`**（`grant1` 是 `c79e8f2f…1237`） |
| `payload.img` | 75,497,472 B，sha `8d266934…8233` |
| **`fw/vendor_boot_gpu_stage1.img`** | **77,393,920 B，sha `bb7f5b8ee58f7caa78ce047912737dd8253b4ebd054f290275331a1776e715af`** |
| cmdline | **996 B**（1000 B 硬闸剩 4 B），三个 GPU 参数与 `native-ufs-allow-unverified=1` 全在 |

`strings(kernel.macho)` 已见 `MUYU B ... t6=%x t7=%x ok=%u vm=%u`；
`fw/vendor_boot_gpu_grant1.img` **未被覆盖**（sha 仍 `6520c152…7364`），A/B 对照格保留。

### 69.26.4 机器校验（`check-stage-probe-rebuild.py`，全绿）

新增 `UniXNU/scripts/check-stage-probe-rebuild.py`（同步到 `out/muyu/tree/scripts/` 一份，
两份 sha 相同；**只能从 `UniXNU/scripts/` 运行**，其 `ROOT` 依赖 `__file__.parents[1]`）。
基线冻结在 `UniXNU/out/muyu/kernel/baseline-pre-stage1/`（grant1 内核 / payload /
`BravearkLastLog.c.orig` / `GpuPowerProbe.cpp.stage1` / 14 输入 sha）。

| 判据 | 结果 |
| --- | --- |
| 14 个构建输入 | **13 个 sha 逐项不变**，唯一变化是 `BravearkLastLog.c` |
| 源码 diff | 1 个 hunk、`+13/-2`、零新增 printf、零写驱动全局、不涉及 sandbox 属性 |
| 新内核 | 含新格式串、旧格式串消失、仍含 `grant1` 名单、**无** `NativeSandboxGrantService` |
| weak import | `nm` 确认 `_native_adreno_table_switches` 可见（真绑定） |
| `kernel.json` | `candidate_sha256` == 新内核 sha |
| payload / vendor_boot | 都含新串，大小 75,497,472 / 77,393,920 B |
| 三个构建日志 | 无 error/failed/fatal；kernel 日志报告新 sha |
| QEMU 基线 | `kernel.skip-owner.macho` sha 仍 `c4c1420a…`；`_load_init_program` / `_native_apfs_probe_checkpoint` / `_vfs_mountroot` / `_native_adreno_table_switches` **四个符号地址零漂移** |

判据 JSON：`out/muyu/tree/build/host-turnip-input/stage-probe-rebuild.json`。

### 69.26.5 ⚠️ 待上机（本轮到此为止，未执行任何刷机命令）

1. 只写 `vendor_boot`，**不碰 `boot` / `recovery` / `userdata`**；设备须在 fastboot；
   `MUYU_UEFI=fw/boot-17line-043f7120.img`，`fastboot boot` RAM 引导，断电即失效。
2. 屏照必须含最顶部 `MUYU LIVE` 行，ticker 跑几遍后拍；逐字抄 **`MUYU B`、`R`、`GPU`、`A`**。
3. **判读格**：

| `t7` | 结论 | 下一版方向 |
| --- | --- | --- |
| `2` | `ArmSmmu4KLivePrepare` 失败：SMMU 表页 / arena 形状（`layout` 或 root 页非零） | 查 `NativeAdrenoTablePages` 分配与 `iova/bytes/budget`（`i48 o40`，`bytes=1<<48`、`budget=2^36`） |
| `3` / `0x30\|op<<8` | `vm_.attach` 失败 | 读 `op`，查 `ArmSmmu4KAttachStreams` |
| `4+i` | `vm_.map` buffer i 失败 | 查 `kIovas[i]` / `GpuBufferCount` |
| `0` | `createVm` 完成过，`-3` 在更下游客户端链 | 回 §9 第 3 条读 `Q`/`Y` |

- `ok=0` ⇒ 本次启动**没有**任何 `createVm` 成功；`ok>0` ⇒ 失败只发生在部分进程/早期时序。
- 显示侧判据沿用 §69.25.6：**`o>=1 且 c>=1` 且 `R` 变 `ok` 才算真通**；
  `o=1 c=0` 不变即确认 blocker 是设备创建、不是帧 hand-off。
- `-3` 仍在是**预期结果**（本版零行为改动），不要因此判定重建失败。

### 69.26.6 边界、产物、回滚

诚实边界：**`board_verified=false`、`flashing=false`、`device_touched=false`、
`qemu_cells_run=0`**；`hardware_boot_demonstrated=false`、`os_boot_demonstrated=false`；
40 个 QEMU 格仍全部 `minus3_reproduced_in_qemu=false`；`owner_cache_mount stage=1/status=80`
如实未改。设备仍在 13:20 那次 RAM 引导的 `grant1` XNU 上（本轮**没有**碰它）。
⚠️ 电池 3738 mV 偏低：充电前须刷回 `stock` vendor_boot，但**别让设备在 stock 状态
长时间墙充**、别再 `restore-stock --yes`（§69.22/§69.23 两次首块损坏的共同嫌疑）。

产物：`fw/vendor_boot_gpu_stage1.img`、`logs/build-{kernel,payload,vendor-boot}-stage1.log`、
`build/host-turnip-input/stage-probe-rebuild.json`、
`UniXNU/out/muyu/kernel/baseline-pre-stage1/`、
`UniXNU/scripts/check-stage-probe-rebuild.py`（+ tree 副本）。

回滚：把 `baseline-pre-stage1/BravearkLastLog.c.orig` 覆盖回
`UniXNU-muyu-17/package/BravearkLastLog.c` 并重跑三步链；或直接删掉
`fw/vendor_boot_gpu_stage1.img`（不刷就没有任何设备影响）。

```bash
cd /Users/diannao/pad7pro-xnu
P=/opt/homebrew/bin/python3.12
$P UniXNU/scripts/check-stage-probe-rebuild.py   # 必须全绿
shasum -a 256 fw/vendor_boot_gpu_stage1.img
# 上机已于 11-13 14:04 北京经用户「上机」授权执行，见 §69.27
```

---

## §69.27 2026-11-13 14:04：`stage1` 已上机 —— 只写 `vendor_boot`，屏照待拍

用户「上机」授权（14:03）。**只写 `vendor_boot`**，`boot` / `recovery` / `userdata` 一字未动，
未用 `fastboot reboot`；引导方式为 RAM `fastboot boot` 17 线 UEFI。

### 69.27.1 只读预检

| 项 | 值 |
| --- | --- |
| 设备通道 | `fastboot devices = a20248f7`，`adb devices` 空 ⇒ 用户已把设备从 13:20 的 XNU 拉回 fastboot |
| `status` | `current-slot=a slot-unbootable=no retry=7 battery=3810 mV`（比 13:20 的 3738 mV 高 72 mV） |
| 账本 | 末条仍是 `05:20:01Z` 的 `grant1` 引导，无新增记录 |
| 产物 | `fw/vendor_boot_gpu_stage1.img` 77,393,920 B / `bb7f5b8e…e715af`，与 checker 判据 JSON 一致 |
| checker | `check-stage-probe-rebuild.py` 全绿（`rc=0`，13/14 输入不变、1 hunk、cmdline 996 B） |
| 入口审计 | `cmd_xnu()`（`tools/muyu-flash-mode.sh:165-183`）= `flash_vendor_boot` + `preflight` + `fb boot "$UEFI"`，**无其它分区写入** |

### 69.27.2 执行与台账

```bash
cd /Users/diannao/pad7pro-xnu
MUYU_UEFI=fw/boot-17line-043f7120.img \
  bash tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_stage1.img
```

```
== slot=a unbootable=no retry=7
   vendor_boot v4 73.81 MiB vendor_ramdisk=72.00 MiB aligned=True
   sha256 bb7f5b8ee58f7caa78ce047912737dd8253b4ebd054f290275331a1776e715af
   Writing 'vendor_boot'                              OKAY [  0.056s]
== booting UEFI from RAM: fw/boot-17line-043f7120.img
   Booting                                            OKAY [  0.189s]
xnu_rc=0
```

`logs/flash-ledger.tsv` 新增（`2026-11-13T06:04:42Z` / `06:04:43Z`，北京 14:04）：

```
flash  vendor_boot  fw/vendor_boot_gpu_stage1.img  bb7f5b8e…e715af   rc=0
boot   -             fw/boot-17line-043f7120.img   043f7120…e5bd3      rc=0
```

引导后 `fastboot devices` 与 `adb devices` 皆空（1 s 内离开 fastboot）⇒ XNU 已运行，
**面板是唯一输出通道**。无 `Load Error`、无 `preflight` 清 slot 动作（本次 `unbootable` 本来就是 `no`）。

### 69.27.3 ⚠️ 成功 ≠ 验证

`board_verified=false / screen_photo_captured=false` 保持。刷写与引导成功只说明镜像落地、
UEFI 被接受；`t6/t7/ok/vm` 是否打印出来、`-3` 是否变化，**必须等屏照**。
`flashing=true / device_touched=true`；本轮 `userdata_written=false`。

### 69.27.4 屏照判读格（沿用 §69.26，逐字抄 `B` / `R` / `GPU` / `A`）

屏照须含最顶部 `MUYU LIVE`，建议 ticker 走 3–4 遍后拍。

| `MUYU B` 新字段 | 结论 |
| --- | --- |
| `t7=2` | `prepare` 失败 → SMMU 表页/arena 形状（`ArmSmmu4KLive.c:11-26, :330-340`），查 `i48 o40` 下 `bytes=1<<48` / `budget=2^36` |
| `t7=3` 或 `0x30\|op<<8` | `vm_.attach` 失败 |
| `t7=4+i` | `vm_.map` buffer i 失败 |
| `t7=0` | `createVm` 完成过，`-3` 在更下游客户端链 ⇒ 回 §9 第 3 条读 `Q`/`Y` |
| `t6=e00002c2 ok=0` | 与 `B s=e00002c2` 自洽，本次启动一次都没成功 —— ⚠️ **§69.28 实测 `ok=3`，此格预期被否** |
| 四个字段全 `ffffffff` | weak_import 未绑上（重建时 `nm` 已确认符号可见，若出现说明另有链接问题）—— ⚠️ **§69.28 实测四字段全部有效，绑定成功** |

显示侧：`o>=1 且 c>=1` + `R` 变 `ok` 才算真通；`o=1 c=0` 不变即确认卡在设备创建而非 hand-off。
⚠️ **`stage1` 零行为改动**，`R` 仍有 `-3` 是预期结果，不要据此判定重建失败，也不要回滚到
改 sandbox 属性 / 移植 uke 补丁。

### 69.27.5 回滚与电池

- A/B 对照镜像 `fw/vendor_boot_gpu_grant1.img`（`6520c152…7364`）**未覆盖**；回滚只需再刷它并 RAM 引导。
- 电池 3810 mV 仍偏低：充电前须刷回 `fw/stock/vendor_boot.img`，但**别让设备在 stock 状态长时间墙充**、
  **别再 `restore-stock --yes`**（§69.22/§69.23 两次 `userdata` 首块损坏的共同嫌疑）。
- `fastboot reboot` 禁用。

---

## §69.28 2026-11-13 14:08：`stage1` 屏照判读 —— ⭐⭐⭐ **`t7=4` 把失败点钉在 `vm_.map(buffers_[0])`，`ok=3 vm=1` 推翻「createVm 全废」**

屏照 `logs/2026-11-13-14-08-35-820.jpg`（3840×2160，14:08，`MUYU LIVE p=13 t=227`，
即引导后约 3 分 47 秒）。分区放大逐行核对；本轮**零设备动作、零重建**。

### 69.28.1 精确读数

```
MUYU ROOT c=6/0/0 d=4/0                         根镜像仍完好（§69.25.1 判据）
MUYU GPU  calls=1 refused=0 o=1 c=0 f=0 fb=0 …  与 13:28 逐字一致（后半未逐字复核）
MUYU B    i48 o40 f1 s=e00002c2 b0 n4 t6=e00002c2 t7=4 ok=3 vm=1
MUYU R    PUB no device: device: Vulkan(VkResultCode(-3))
MUYU V    op=3 bad=0 cl=2 call=567 sel=6 st=e00002… （左半幅）
MUYU O    SpringBoard|Springboard|backboardd …   opener 名字
```

`i48 o40 f1 s=e00002c2` 与 13:28 相同，只有 `n5 → n4`（mount 失败次数少 1）。

### 69.28.2 观测生效，`-3` 仍在是预期

`t6/t7/ok/vm` 四个字段全部是真实值、没有出现 `ffffffff` ⇒ §69.26 用 `nm` 预判的
`_native_adreno_table_switches=fffffff007bfaf68` weak_import 绑定成立，这次重建没有白跑。
`R` 仍是 `-3`、`GPU o=1 c=0` 不变、`ROOT c=6/0/0` 完好 ⇒ **stage1 零行为改动完全符合预期**：
显示链仍冻结在首帧（`§69.25.6`），不能据此判定重建失败，也不要回滚到改 sandbox 属性 /
移植 uke 补丁。

### 69.28.3 ⭐ `t7=4` 的确切含义

`NativeAdrenoGpuContext.cpp:168/172/180/184/198` 的 stage 写入逐条对照：

| t7 | 源码 | 状态 |
| --- | --- | --- |
| `2` | `:168` prepare 进入 | 未停留 |
| `3` / `0x30\|op<<8` | `:172` / `:180` attach | 未停留 |
| **`4+i`** | **`:184` `p->vm_.map(&buffers_[i],0,kIovas[i],i?3u:5u,0,2,…)`** | **实测 `4` ⇒ `i=0`，第一个 buffer 的 map 失败** |
| `0` | `:198` done | 未到达（本次调用） |

⇒ **SMMU prepare 成功、attach 成功，失败发生在第一个 map**。`t6=e00002c2` 由 `:188`
`native_adreno_table_switches[6]=uint32_t(status)` 写入，是 `NativeAdrenoVm::preflight()`
（`:206-251`）返回的 `kIOReturnBadArgument`。§69.26 静态分析倾向的 `t7=2`（SMMU 表页/arena
形状）**被板上读数排除**。

### 69.28.4 ⭐⭐ `ok=3 vm=1` 推翻「createVm 全废」

`[2]` 只在 `createVm` 成功路径 `++`（`:201`），`[4]=live_vms_`（`:202` / `destroyVm :223`）。
实测 `ok=3 vm=1` ⇒ **本次启动有 3 次 `createVm` 完全成功，且仍有 1 个 live VM**。

- §69.27.4 判读格里「`t6=e00002c2 ok=0` ⇒ 一次都没成功」的预期**被否**，已就地划掉；
- §69.25.3「链条：createVm 返回 BadArgument → mount 失败 → … → -3」需收窄为
  **「某次之后的所有新 VM 都在第一个 map 失败」**，不是开机即全废；
- 与 `V op=3 call=567`（有连接成功并跑了 567 次调用）、`G` 行部分进程被拒一起看：
  是**部分客户端 / 早期时序**的问题，GPU 并未整体不可用。

### 69.28.5 静态归因：共享 buffer 被一次性 quarantine（最强候选，未板上确证）

逐条排除 `preflight()` 的 BadArgument 分支：

- `:209-211` 的参数段静态合法：`i=0` ⇒ `permissions=5`（含 `ArmSmmu4KRead`、`&~15u==0`）、
  `writeback=0`（`>1` 才拒）、`shareability=2`（`!=2 && !=3` 才拒），`kIovas[0]` 与 arena
  `output_bits` 都是常量 ⇒ 若命中则一次都不会成功，与 `ok=3` 矛盾；
- `:213-214`（iova 对齐/越界）、`:221`（output_bits 范围）、`:231`（span 总长）同理是常量；
- **能随时间变化、且能解释「先 3 次成功、之后 4 次持续失败」的只剩两类**：
  ① `:209` 的 `!memory->backed() || memory->quarantined()`；
  ② `:228` 的 `r.tables->overlaps(span.physical,span.bytes)` / `:249` 的 alias 冲突。

关键结构：`buffers_[i]` 是 `NativeAdrenoGpuContext` 的**全局共享 buffer**（`:95` 分配），
`:132`（默认 VM）与 `:185`（**每个**进程 VM）映射的是同一组对象。而
`NativeAdrenoVm::fail()`（`:90-101`）在任意 fault 时会执行
`mappings_[i].memory->quarantine()`；`NativeAdrenoMemory::quarantine()`
（`NativeAdrenoMemory.h:122`）**只置 `quarantined_=true`，头文件里没有复位路径**。

⇒ 假设：**某次 `fail()` 把共享 `buffers_[0]` 永久隔离，此后每个新 `createVm`
的第一个 map 都在 `preflight :209` 返回 `0xe00002c2`**（`fail()` 自身返回
`kIOReturnIOError`，不是 `0xe00002c2`，所以 `t6` 确系 `preflight` 而非 `fail()`）。
这与 `ok=3 / vm=1 / n4` 的时序形状自洽，但**仍是静态推断**：② 未被排除，屏上也没有
`native_adreno_global_fault` 的打印（`BravearkLastLog.c` 只打了 `recovery_fault` ⇒ `MUYU F`）。

### 69.28.6 下一版（用户「继续」已授权落 `stage2`，见 §69.29）

1. **纯观测 `stage2`（先确证，避免白跑一次上机）**：`MUYU B` 同一行追加
   `q=`（`quarantined()`）、`bk=`（`backed()`）、`dr=`（`deviceReferences()`）、
   `gf=`（`native_adreno_global_fault[0]`），**另加 `pf=` 分支码与 `rn=` 拒绝次数**
   —— 比原计划更强：不再靠 `q` 反推，屏上直接给出 `preflight()` 命中的 return 点。
   仍是零行为改动、零新增 printf。判读表见 §69.29.4。
2. **坐实后再落修复**：`fail()` 的 quarantine 循环区分 `memory->imported()`（用户导入的
   IOSurface，需隔离）与驱动自有共享 buffer（改走 `retireAfterQuiescence()`，不置
   `quarantined_`）。⚠️ 这改变驱动行为，须重新核对编译输入 / checker / QEMU 符号漂移，
   且要评估 DMA 生命周期，不可与观测版合并成一版。
3. **不要**再动 `NativeSandboxGrantProcess`、不要移植 uke 补丁、不要重跑 selector 矩阵
   （§69.25 已源码级证伪）。

显示侧判据不变：`o>=1 且 c>=1` + `R` 变 `ok` 才算真通。

### 69.28.7 边界

新增 `stage1_board_observation_received=true / stage_probe_effective=true /
createVm_ever_succeeded=true / minus3_persists=true`。
`board_verified=false` 保持（-3 未解决）；本轮 `flashing=false / device_touched=false /
qemu_cells_run=0`。设备仍在 14:04 RAM 引导的 `stage1` XNU 上（fastboot/adb 皆空）。
电池 3810 mV 偏低，充电前刷回 `stock` vendor_boot，但别长时间墙充、
别再 `restore-stock --yes`。

---

## §69.29 2026-11-13 14:2x：按 §69.28.6 落 `stage2` —— **`pf=` 直接给出 `preflight()` 的失败分支**

用户「继续」授权（14:21）。**离线重建完成，停在「待上机」，全程没碰设备**。
⚠️ **不是修复版，零行为改动**：`-3` 仍在是预期；本版价值是让下一张屏照把
`0xe00002c2` 从「`preflight()` 的 BadArgument」细化到**确定的 return 点**。

### 69.29.1 两个输入变化

| 文件 | 改动 |
| --- | --- |
| `UniXNU/drivers/Adreno740/NativeAdrenoVm.cpp` | 新增 `extern "C" volatile uint32_t native_adreno_preflight_probe[8]` + 只读 helper `nativeAdrenoProbeRefusal()`（17 行）；把 `preflight()` 的 **8 个** `return kIOReturn…;` 换成 `return nativeAdrenoProbeRefusal(N,memory,kIOReturn…);`。**用 return 表达式替换而非插入语句**，保住所有 `if(cond)return …;` 的单语句体语义；helper 只写 probe 数组并返回原值，不碰 quarantine / fail / poison 状态、不加任何控制流 |
| `UniXNU-muyu-17/package/BravearkLastLog.c` | `MUYU B` 同一 printf 追加 6 个只读字段 `pf= q= bk= dr= gf= rn=`；**16 格式符对 16 参数**、零新增 printf、零写驱动全局 |

分支码（`preflight()` 的 8 个 return 点，按源码顺序）：`1`=`:209` 参数/quarantine/backed、
`2`=`:213` iova、`3`=`:219` Busy 重叠、`4`=`:221` output_bits、`5`=`:225-227` span 形状、
`6`=`:228` `r.tables->overlaps`、`7`=`:231` span 总长、`8`=`:249` alias 冲突。
`gf` 直接读现成的 `native_adreno_global_fault[0]`（`NativeAdrenoVm.cpp:18`，`fail()` 计数，
屏上此前从未打印）⇒ **该字段零驱动改动**。

⭐ **比 §69.28.6 原计划更强**：原计划只打 `q/bk/dr/gf`，仍需从状态反推分支；
现在 `pf` 直接给出命中点，`q` 只用来在 `pf=1` 时区分「quarantined / unbacked / 参数」。

### 69.29.2 校验器对「零行为改动」的硬证明

`UniXNU/scripts/check-stage2-probe-rebuild.py`（+ tree 副本，**只能从 `UniXNU/scripts/` 运行**）
不靠统计 diff 行数（difflib 会把相邻 `}` 上下文算成删除），改用逐字节还原：

> 从新源码里**切掉新增诊断块** → 把 8 个 `nativeAdrenoProbeRefusal(N,memory,kIOReturnX);`
> **还原成 `kIOReturnX;`** → 要求结果与 `baseline-pre-stage2/NativeAdrenoVm.cpp.orig` **完全相同**。

这同时证明 8 个替换没有改变任何条件、括号或返回码。

### 69.29.3 产物与校验

| 产物 | 值 |
| --- | --- |
| `fw/vendor_boot_gpu_stage2.img` | 77,393,920 B，sha `24961f7e…4470` |
| `kernel.macho` | `9a0cda2f…d125e76`（stage1 是 `c8ca2bc1…5836`） |
| `payload.img` | `66514c18…5eb3`，75,497,472 B |
| cmdline | **996 B，与 stage1 逐字相同**（boot-arg 一字未动） |
| 新符号 | `_native_adreno_preflight_probe=fffffff007bfc3fc`、`_native_adreno_global_fault=fffffff007bfc3ec`、`_native_adreno_table_switches=fffffff007bfaf68` |

`check-stage2-probe-rebuild.py` **全绿（rc=0）**：17 个受控输入中 15 个 sha 不变
（新增把 `NativeAdrenoVm.cpp` / `GpuContext.cpp` / `Memory.cpp` 纳入清单）；
屏侧 1 hunk `+17/-2`、16 格式符、零新增 printf；驱动 8 处 return 替换还原后逐字节等于基线；
新内核含新格式串、旧 stage1 串消失、仍含 grant1 名单、无 uke 补丁；
`kernel.json` 对齐；payload/vendor_boot 含新串且大小正确；三日志无 error；
`stage1`（`bb7f5b8e…e715af`）与 `grant1`（`6520c152…7364`）**对照镜像未覆盖**；
`kernel.skip-owner.macho` 仍 `c4c1420a…`，`_load_init_program` / `_native_apfs_probe_checkpoint` /
`_vfs_mountroot` / `_native_adreno_table_switches` 四符号地址**零漂移** ⇒ §69.17 QEMU 定点补丁仍适用。
判据 JSON `out/muyu/tree/build/host-turnip-input/stage2-probe-rebuild.json`。

### 69.29.4 上机判读格（屏照须含顶部 `MUYU LIVE`）

逐字抄 `MUYU B`（本版 10→16 字段）、`R`、`GPU`、`A`：

| `pf` | 命中点 | 结论 / 下一步 |
| --- | --- | --- |
| **`1` + `q=1`** | `:209` 的 `memory->quarantined()` | ⭐ **污染假设坐实**：共享 `buffers_[0]` 被 `fail():95` quarantine 且无复位。落 §69.29.5 修复 |
| `1` + `q=0 bk=0` | `:209` 的 `!backed()` | buffer 已释放/未就绪，查生命周期而非 quarantine |
| `1` + `q=0 bk=1` | `:209` 的参数段 | 与 `ok=3` 矛盾（静态常量），需复核读数 |
| `2` / `4` / `5` / `7` | iova / output_bits / span 形状 / 总长 | 静态量，查 `i48 o40` 与 arena config |
| `3` | `:219` Busy 重叠 | 新 VM 已有映射，查 `attach()` 是否带入了 kernel 表 |
| **`6`** | `:228` `r.tables->overlaps(span.physical,span.bytes)` | 新 VM 的表已映射这些物理页 ⇒ 与 §69.28.5 的 ② 备选一致，改表隔离而非 quarantine |
| `8` | `:249` alias 冲突 | 另一 cache/shareability class 的映射冲突 |
| `0` / 全 `ffffffff` | 无拒绝记录 / weak_import 未绑上 | `nm` 已确认符号可见；若全 F 说明链接另有异常 |

辅助：`gf>0` 说明 `fail()` 至少跑过一次；`rn` 与 `B n=` 对比可判断拒绝次数是否同源；
显示侧沿用 `o>=1 且 c>=1` + `R` 变 `ok` 才算真通，`o=1 c=0` 不变即仍卡在设备创建。

### 69.29.5 坐实后的修复方向（**本轮未做**）

仅当 `pf=1 q=1` 才落：改 `NativeAdrenoVm::fail():90-101` 的 quarantine 循环，
对 `memory->imported()`（用户导入的 IOSurface，必须隔离）保留 `quarantine()`，
对**驱动自有、跨 VM 共享的 `buffers_[i]`** 改走 `retireAfterQuiescence()`（只减
`device_references_`，不置 `quarantined_`）。⚠️ 这**改变驱动行为**：须重新核对编译输入、
写新 checker、验证 QEMU 符号漂移，并评估 DMA 生命周期（共享页被 GPU 写时的安全性）；
按单变量原则单独一版，**不要与本观测版合并**。

### 69.29.6 边界与回滚

`board_verified=false / flashing=false / device_touched=false / qemu_cells_run=0`；
两个 `false` 与 `owner_cache_mount stage=1/status=80` 如实未改。
**收尾只读探测：设备已回到 fastboot**（`a20248f7` / `slot=a unbootable=no` / 3791 mV），
账本末条仍是 `06:04:43Z` ⇒ **本轮无任何刷机/引导记录**，这次状态变化不是本轮操作；
上一板上状态是 14:04 RAM 引导的 `stage1` XNU（14:08 屏照即来自它）。
回滚：把 `baseline-pre-stage2/{BravearkLastLog.c.orig,NativeAdrenoVm.cpp.orig}` 覆盖回去
并重跑三步链；或直接删掉 `fw/vendor_boot_gpu_stage2.img`（不刷就没有任何设备影响）。
电池 3791 mV 偏低：充电前须刷回 `stock` vendor_boot，但**别让设备在 stock 状态长时间墙充**、
别再 `restore-stock --yes`；`fastboot reboot` 禁用。

```bash
cd /Users/diannao/pad7pro-xnu
P=/opt/homebrew/bin/python3.12
$P UniXNU/scripts/check-stage2-probe-rebuild.py   # 必须全绿
shasum -a 256 fw/vendor_boot_gpu_stage2.img
# 用户 14:42「上机」已授权；实际命令与结果见 §69.30
```

---

## §69.30 2026-11-13 14:42：`stage2` 已上机（用户「上机」授权），屏照待拍

> ⚠️ **只写 `vendor_boot` + RAM 引导，`fastboot reboot` 禁用**。
> ⚠️ **刷写/引导成功 ≠ 板上验证**：`board_verified=false / screen_photo_captured=false` 保持，
> `pf/q/bk/dr/gf/rn` 是否打印、`-3` 是否变化必须等屏照。`stage2` 零行为改动，`-3` 仍在是预期。

### 69.30.1 上机前只读预检

- 设备：`fastboot devices = a20248f7  fastboot`，`adb devices` 空；`current-slot=a`、
  `slot-unbootable:a=no`、`retry=7`、`battery-voltage=3699 mV`（脚本内两次读到 3698 mV，
  触发 `<3700` 的 WARNING，但不阻断）。
- `tools/muyu-flash-mode.sh status`：账本 3201 条，末条仍 `06:04:43Z`（`stage1` 的 boot）⇒
  上机前本轮零刷机零引导。
- 产物：`fw/vendor_boot_gpu_stage2.img` 77,393,920 B，header `VNDRBOOT`，
  73.81 MiB / ramdisk 72.00 MiB，sha `24961f7e4f2b4174c54f11be11e86e0403a83b3d76c919a608080ac544147e70`，
  与 `stage2-probe-rebuild.json` 一致；UEFI `fw/boot-17line-043f7120.img`
  sha `043f7120f75f50c832b5a49f9eedbfcc594e17825b6ba56d4c2d33bb6a3e5bd3`。
- `UniXNU/scripts/check-stage2-probe-rebuild.py` 复跑**全绿**：15/17 输入 sha 不变、
  屏侧 1 hunk `+17/-2`、16 格式符对 16 参数、8 处 return 替换、内核
  `9a0cda2f…d125e76`、cmdline 996 B 与 stage1 逐字相同 ⇒ 零行为改动证据未失效。
- 再审 `tools/muyu-flash-mode.sh:165-183` 的 `cmd_xnu()`：只有
  `flash_vendor_boot` → `preflight` → `fastboot boot "$UEFI"`，**无 `fastboot reboot`**、
  不写 `boot`/`recovery`/`userdata`。

### 69.30.2 执行与结果

```bash
cd /Users/diannao/pad7pro-xnu
MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_stage2.img \
  2>&1 | tee logs/flash-stage2.log
```

- flash `vendor_boot`：**`OKAY [0.057s]`** / total 1.831s；boot 17 线 UEFI：
  **`OKAY [0.189s]`** / total 0.239s，**无 `Load Error`**。
- 账本四条全 `rc=0`：`06:42:36Z preflight` / `06:42:38Z flash` /
  `06:42:38Z preflight` / `06:42:38Z boot`。日志 `logs/flash-stage2.log`。
- 引导后 `fastboot devices` 空、`adb devices` 空 ⇒ 设备已离开 fastboot 在跑 RAM 引导的 XNU，
  **面板是唯一输出通道**。
- `boot`/`recovery`/`userdata` 未动，未用 `fastboot reboot`；
  `stage1`（`bb7f5b8e…e715af`）与 `grant1`（`6520c152…7364`）镜像均未覆盖，保留 A/B 对照。

### 69.30.3 边界与设备纪律

`flashing=true / device_touched=true / userdata_written=false / board_verified=false /
screen_photo_captured=false / qemu_cells_run=0`；两个 `false` 与
`owner_cache_mount stage=1/status=80` 未改。电池 3698 mV 偏低：充电前须刷回
`fw/stock/vendor_boot.img`（`tools/muyu-flash-mode.sh stock`），但**别让设备在 stock 状态长时间
墙充**、别再 `restore-stock --yes`；`fastboot reboot` 禁用。

### 69.30.4 待办：屏照与判读

等 ticker 刷新 3–4 遍后拍**完整屏照（必须含顶部 `MUYU LIVE` 行）**，逐字抄
`MUYU B`（本版 16 字段）、`R`、`GPU`、`A`。判读格见 **§69.29.4** 与冷启动 §9 第 9 条：
`pf=1 q=1` ⇒ 共享 buffer 被 `fail():95` quarantine 且无复位，假设坐实，才落 §69.29.5 修复；
`pf=6` ⇒ 表重叠改表隔离；`pf=3/8` ⇒ Busy/alias；`pf=0` 或全 `ffffffff` ⇒
无拒绝记录 / weak_import 异常。辅助 `gf>0`、`rn` 与 `B n=` 对比；
显示侧仍以 `o>=1 且 c>=1` + `R=ok` 才算真通。

### 69.30.5 回滚

板上：重新 `xnu fw/vendor_boot_gpu_stage1.img`（A/B 对照）或 `stock`；
源码侧：把 `baseline-pre-stage2/{BravearkLastLog.c.orig,NativeAdrenoVm.cpp.orig}` 覆盖回去
并重跑三步链。本轮没有新增源码或镜像改动。

---

## §69.31 2026-11-13 14:46：`stage2` 屏照判读 —— ⭐⭐⭐ **`pf=1 q=1` 板上坐实共享 buffer quarantine 污染**

> 屏照 `logs/2026-11-13-14-46-26-183.jpg`（3840×2160，14:46）。
> **纯判读：零设备动作、零刷机、零重建、零格 QEMU。**

### 69.31.1 逐字转录（放大裁剪复核过，见 `/tmp/muyu_*` 裁图方法）

```
MUYU GPU calls1 refused=0 o=1 c=0 f=0 fb=0 tc=0 ts=ffffffff hold=1 pres=1 l=1 w=1
MUYU PROBE st=97 cp=10 b=24
MUYU B i48 o40 f1 s=e00002c2 b0 n4 t6=e00002c2 t7=4 ok=3 vm=1 pf=1 q=1 bk=1 dr=2 gf=1 rn=4
MUYU V op=3 bad=0 cl=2 call=557 sel=6 st=e00002d6 at=1 lmf=0 mb=0/0 tr=30 gr=7
MUYU O SpringBoard|SpringBoard|backboardd
MUYU R PUB no device: device: Vulkan(VkResultCode(-3))
MUYU G mediaServerd/o1/r0/c1/p0/xd backboardd/r2/c1/p4445/xe00002d5 n3
MUYU ROOT c=6/0/0 d=4/0
MUYU INIT q=6
MUYU BSD s=8
```

⚠️ **本屏没有拍到顶部 `MUYU LIVE` 行**（照片从 `MUYU GPU` 行起），但 `MUYU PROBE st=97 cp=10 b=24`
与 §69.28 的 stage1 屏照一致、`MUYU B`/`R`/`GPU`/`ROOT` 全套字段齐全 ⇒ ticker 在跑、读数可用；
下一张仍建议连顶部一起拍。本屏也**没有出现 `MUYU A` 行**（第二行是 `MUYU PROBE`），不影响判读。

### 69.31.2 ⭐ 主判据：`pf=1 q=1` ⇒ §69.28.5 的 quarantine 假设坐实

| 字段 | 值 | 含义 |
| --- | --- | --- |
| `pf` | **1** | `preflight()` 命中的是第 1 个 return 点，即 `NativeAdrenoVm.cpp:209` 的参数/quarantine 段；**不是** `:219` Busy（3）、`:228` `tables->overlaps`（6）、`:249` alias（8） |
| `q` | **1** | `memory->quarantined()` 为真 ⇒ `buffers_[0]` 已被 `fail():90-101` 隔离 |
| `bk` | 1 | `backed()` 为真 ⇒ 排除 §69.29.4 判读格里「buffer 已释放/未就绪」那一支，是**真隔离**不是生命周期失效 |
| `dr` | 2 | `deviceReferences` 计数 |
| `gf` | **1** | `native_adreno_global_fault[0]` ⇒ `fail()` 至少跑过一次，与 quarantine 来源自洽 |
| `rn` | **4** | 拒绝次数与 `B n=4` 相等 ⇒ 4 次 mount 失败与 4 次 preflight 拒绝**同源**，没有别的拒绝路径 |

新增 6 字段全部非 `ffffffff` ⇒ `weak_import` 绑定成立、`native_adreno_preflight_probe[8]` 命中，
§69.29 的观测版没有白跑。`pf=1` 直接排除 §69.28.5 的备选 ② 表重叠（`pf=6`）与 ③ Busy/alias
（`pf=3/8`），也排除「静态常量参数错误」（`bk=1` + `ok=3`）。

### 69.31.3 归因链闭合（第一次把 `-3` 钉到确定代码行）

一次 GPU fault → `NativeAdrenoVm::fail():90-101` 对**驱动自有、跨 VM 共享**的 `buffers_[0]`
调 `quarantine()` → `NativeAdrenoMemory::quarantine()`（`NativeAdrenoMemory.h:122`）只置
`quarantined_=true` 且**无复位路径** → 之后每个新 VM 的第一个 `vm_.map(buffers_[0])` 在
`preflight():209` 返回 `kIOReturnBadArgument (0xe00002c2)` → `createVm()` 失败（`:188` 写 `t6`、
`t7=4`）→ `mount()` `f=1` → `IOServiceOpen` 返回 Busy（`G …/xe00002d5`）→ Turnip
`vkEnumeratePhysicalDevices` → **`Vulkan(VkResultCode(-3))`**。

`ok=3 vm=1` 说明早期 3 次 `createVm` 是成功的 ⇒ 是**污染之后的累积效应**，不是 GPU 整体不可用；
与 §69.25.6 的 `o=1 c=0`（首帧冻结）自洽。

### 69.31.4 与 stage1 逐字对比 ⇒ 零行为改动在板上确认

`i48 o40 f1 s=e00002c2 b0 n4 t6=e00002c2 t7=4 ok=3 vm=1` 十个旧字段与 §69.28 的 stage1 屏照
**完全相同**（仅 `n4` 与 §69.25 的 `n5` 差一次采样），`R` 仍 `-3`、`GPU o=1 c=0`、
`ROOT c=6/0/0 d=4/0`、`INIT q=6`、`BSD s=8` 全部不变。
⇒ §69.29 / §69.30 声称的「纯观测、零行为改动」板上成立；**`-3` 仍在是预期，不是重建失败**。

### 69.31.5 ⚠️ 下一步：改行为的修复版（**本轮未做，须单独授权**）

现在才满足 §69.29.5 的落修复条件。方案（单变量、单独一版、**不与观测版合并**）：
改 `NativeAdrenoVm::fail():90-101` 的 quarantine 循环 —— 对 `memory->imported()`
（用户导入的 IOSurface，必须隔离）保留 `quarantine()`；对**驱动自有、跨 VM 共享**的
`buffers_[i]` 改走 `retireAfterQuiescence()`（只减 `device_references_`，不置 `quarantined_`）。
⚠️ 这**改变驱动行为**：必须重新核对编译输入、写新 checker、验证 QEMU 关键符号零漂移，
并评估 **DMA 生命周期**（共享页仍被 GPU 写时的安全性）；先离线重建，**上机须另行说明**。
判据：修复版屏照应见 `pf=0`（或 `q=0`）、`ok/vm` 增长、`GPU c>=1` 且 `R` 变 `ok`。

### 69.31.6 边界

`board_verified=false` 保持（GPU 未接管、桌面未进）、`minus3_persists=true`；
新增 `stage2_probe_effective=true / quarantine_hypothesis_confirmed=true /
preflight_refusal_site=1 / board_screen_captured=true`。
本轮 `flashing=false / device_touched=false / qemu_cells_run=0`；设备仍在 14:42 RAM 引导的
`stage2` XNU 上，账本末条仍是 `06:42:38Z` ⇒ **零刷机零引导**。
电池 3698 mV 偏低：充电前须刷回 `stock` vendor_boot，别长时间墙充、别再
`restore-stock --yes`；`fastboot reboot` 禁用。

---

## §69.32 2026-11-13 14:52–15:0x：⭐⭐⭐ 按 §69.31.5 落 **`fix1`** —— 第一次改驱动行为，离线重建完成、停在「待上机」

用户「继续」授权。**全程离线：零刷机、零引导、零格 QEMU、零设备动作**。
⚠️ 与 §69.26–§69.31 的纯观测版不同，**这一版改变 `NativeAdrenoVm::fail()` 的行为**，
是第一次真正的修复尝试。

### 69.32.1 方案：撤销对共享 buffer 的隔离，但**不**动设备引用

板上 `pf=1 q=1` 已坐实：`fail():112` 把 `mappings_` 里每个 memory 都 `quarantine()`，
而 `buffers_[i]` 是**驱动自有、跨 VM 共享**的（`NativeAdrenoGpuContext.cpp:95`
`allocate()` 一次，`:132` 内核 VM 与 `:185` 每个进程 VM 都 map），
`NativeAdrenoMemory::quarantine()`（`NativeAdrenoMemory.h:122`）只置
`quarantined_=true` 且**无复位路径** ⇒ 一次 fault 永久污染它们。

新增 helper（只隔离用户内存）：

```cpp
static void isolateAfterFault(NativeAdrenoMemory *memory)
{
    if(memory->imported() || memory->system())memory->quarantine();
}
```

两处调用：`fail():112` 的循环、以及 `map():344` 无空闲 mapping slot 时的显式
`memory->quarantine()`。**用户导入的 IOSurface 与 `allocateSystem()` 的 BO 仍按原语义隔离**；
`regions_[].tables->quarantine()` / `state->poisoned` / `fault_streams_` 全部不动。

### 69.32.2 ⚠️ 与 §69.31.5 原计划的重要偏离（源码复核后的工程判断）

原计划让自有共享 buffer 改走 `retireAfterQuiescence()`。**本版刻意没有这么做**：

- `retireAfterQuiescence()`（`NativeAdrenoMemory.cpp:378-383`）只做
  `--device_references_`，并不等待 DMA；`release()`（`:315`）在
  `device_references_ || quarantined_` 时返回 Busy。
- `fail()` 路径**没有** `ArmSmmu4KLiveUpdateContextBatch` 的 TLB unmap，也没有任何
  DMA 静默证据（正常 unmap 在 `:417-426` 完成表事务之后才 retire）。
  在此提前减引用，会让 `release()` 有机会释放**仍可能被 GPU 写**的页。
- 因此本版选择：**只跳过 `quarantine()`，保留 `device_references_`**。
  `release()` 仍返回 Busy ⇒ 页不会被提前释放，DMA 生命周期安全；
  引用计数泄漏是 `fail()` 的既有行为（原版 quarantine 也让 `release()` Busy），**不新增风险**。
  后续 VM 的 `preflight():226` 不再因 `quarantined()` 拒绝，`createVm()` 可以继续。

### 69.32.3 单变量与编译输入

`baseline-pre-fix1/` 冻结了 `NativeAdrenoVm.cpp.orig`、`BravearkLastLog.c.orig`、
`inputs.pre-fix1.json`（17 个输入）、`kernel.macho`、`payload.img`。
改动后 diff 严格 **2 个 hunk**：helper 块（含注释）+ 两处调用替换，
17 个受控输入里**只有 `UniXNU/drivers/Adreno740/NativeAdrenoVm.cpp` 的 sha 变了**。
⭐ **屏侧 `BravearkLastLog.c` 与基线逐字节相同**：`pf/q/bk/dr/gf/rn` 六个观测字段
全部保留，修复版上机后能直接读 `pf`/`q` 判断修复是否生效。

### 69.32.4 三步链与产物（§69.20.2 参数逐字相同）

```bash
python3 UniXNU/unixnu build kernel  -D UniXNU-muyu-17 --uefi …/Mu-Silicium \
  --edk2 …/Mu-Silicium/Mu_Basecore -j 8 > logs/build-kernel-fix1.log 2>&1
python3 UniXNU/unixnu build payload -D UniXNU-muyu-17 -j 8 \
  --data-ramdisk fw/data-seed.img --owner-trust-cache fw/trust-21H461/owner-trust-17-full.bundle \
  --gpu-addon fw/gpuaddon-17-full.bin --unique-chip-id 0000000000000001 \
  --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-addon=1 --kernel-arg sheng-gpu-metal=1 \
  > logs/build-payload-fix1.log 2>&1
python3 UniXNU-muyu-17/scripts/patch-vendor-boot.py --base /tmp/base96.img \
  --payload UniXNU/out/muyu/payload/payload.img --output fw/vendor_boot_gpu_fix1.img \
  > logs/build-vendor-boot-fix1.log 2>&1
```

| 产物 | 值 |
| --- | --- |
| **`fw/vendor_boot_gpu_fix1.img`** | **77,393,920 B，sha `a004368bdf97c648d7a7433abae75d511b2a3d15d755a1e6289361f3254a39b7`** |
| `kernel.macho` | 53,202,944 B，sha `2c3bc032…d50d`（stage2 `9a0cda2f…d125e76`） |
| `payload.img` | 75,497,472 B，sha `ce8d7838…37a` |
| cmdline | **996 B，与 stage2 / stage1 逐字相同**，boot-arg 一字未动 |

### 69.32.5 新 checker 全绿（`check-fix1-rebuild.py`）

`UniXNU/scripts/check-fix1-rebuild.py`（+ `out/muyu/tree/scripts/` 副本，
**只能从 `UniXNU/scripts/` 运行**），判据 JSON
`out/muyu/tree/build/host-turnip-input/fix1-rebuild.json`。因为这是行为改动，
证明方式比观测版更严：

1. 17 个输入里 16 个 sha 不变、屏侧源码逐字节相同；
2. ⭐ **逐字节还原**：切掉 helper 块 → 把两处 `isolateAfterFault(...)`
   还原成 `quarantine()` → 与 `baseline-pre-fix1/NativeAdrenoVm.cpp.orig`
   **完全相同**（证明没有任何夹带改动）；
3. helper 只含一个 `if`、`quarantine()` 恰好一次、
   **不含 `retireAfterQuiescence` / `release(` / 任何全局写**（DMA 约束机器化）；
4. 新内核仍含 stage2 的 16 字段格式串、grant1 名单、无 uke 补丁；
   `kernel.json.candidate_sha256` 对齐；`kernel.skip-owner.macho` 仍 `c4c1420a…`；
   `_load_init_program` / `_native_apfs_probe_checkpoint` / `_vfs_mountroot` /
   `_native_adreno_table_switches` / `_native_adreno_preflight_probe` /
   `_native_adreno_global_fault` **六符号地址相对 stage2 零漂移** ⇒ §69.17 QEMU 定点补丁仍适用；
5. payload / vendor_boot 大小正确、sha 变化、仍含 stage2 格式串；
   `stage2`（`24961f7e…4470`）/ `stage1`（`bb7f5b8e…e715af`）/
   `grant1`（`6520c152…7364`）三个对照镜像**均未覆盖**；
6. 三个构建日志无 error，cmdline ≤1000 B 且与 stage2 逐字相同，三个 GPU 参数齐全。

> 备注：`kernel.macho` 重建本身有大量非确定性差异（stage1→stage2 7.6 MB / 4054 个 4K 页，
> stage2→fix1 1.2 MB / 3098 页），**不要**把差异字节数当作「改动大小」的判据；
> 工程判据一直是输入 sha + 源码还原 + 关键符号零漂移，本轮全部满足。
> `isolateAfterFault()` 是 `static` 小函数，编译后被内联（`nm` 无符号），属预期。

### 69.32.6 ⚠️ 待上机：判读格

只写 `vendor_boot`、`MUYU_UEFI=fw/boot-17line-043f7120.img` RAM 引导、
**`fastboot reboot` 禁用**；屏照须含顶部 `MUYU LIVE`，逐字抄
`MUYU B`（16 字段）、`R`、`GPU`、`A`。

| 读数 | 结论 |
| --- | --- |
| **`pf=0 q=0`** | ⭐ **主判据命中**：不再有 preflight 拒绝，共享 buffer 污染已清 |
| `pf=1 q=1` | 修复未生效（`quarantined()` 仍为真），回查是否刷错镜像 / helper 未编入 |
| `pf=1 q=0 bk=1` | 静态参数分支，与 `ok>=3` 矛盾，先复核读数 |
| **`t7=3`** | `fail():113` 的 `regions_[].tables->quarantine()` 成为下一 blocker（本版刻意未动），下一版再处理 |
| `gf>0` | 预期：`fail()` 仍会跑，只是不再污染共享 buffer |
| **`o>=1 且 c>=1` + `R=ok`** | 唯一的全通判据；`o=1 c=0` 不变 ⇒ 仍卡设备创建/首帧 |

⚠️ 离线校验**不能**证明 `-3` 消失；`board_verified=false` 保持。

### 69.32.7 边界与回滚

`board_verified=false / flashing=false / device_touched=false / qemu_cells_run=0`，
`minus3_persists=true` 保持（板上尚未验证修复版）；新增
`fix1_rebuilt=true / quarantine_fix_offline_verified=true /
shared_buffer_references_kept=true`。账本末条仍是 `06:42:38Z` ⇒ **本轮零刷机零引导**，
设备仍在 14:42 RAM 引导的 `stage2` XNU 上。

回滚：源码侧把 `baseline-pre-fix1/NativeAdrenoVm.cpp.orig` 覆盖回去并重跑三步链；
板上直接刷回 `fw/vendor_boot_gpu_stage2.img`（A/B 对照）。不刷就没有任何设备影响。
电池 3698 mV 偏低，充电前须刷回 `stock` vendor_boot，别长时间墙充、
别再 `restore-stock --yes`。

> 后续：本节「待上机」状态已被 **§69.33（15:06 已上机）** 取代，本节保留为离线重建记录。

---

## §69.33 2026-11-13 15:05–15:07：⭐⭐⭐ **`fix1` 已上机（用户「上机」授权），屏照待拍**

### 69.33.1 只读预检

| 项 | 读数 |
| --- | --- |
| `fastboot devices` | `a20248f7  fastboot`；`adb devices` 空 |
| slot | `current-slot=a`、`slot-unbootable:a=no`、`slot-retry-count:a=7` |
| 电池 | 预检 `getvar battery-voltage` = **3781 mV**；脚本 `preflight` 两次读到 **3692 mV**，触发 `<3700` WARNING 但**不阻断**（`muyu-flash-mode.sh:145`） |
| 镜像 | `fw/vendor_boot_gpu_fix1.img` 77,393,920 B / sha **`a004368bdf97c648d7a7433abae75d511b2a3d15d755a1e6289361f3254a39b7`**，与 `fix1-rebuild.json` 一致；header `VNDRBOOT` |
| checker | `UniXNU/scripts/check-fix1-rebuild.py` 复跑全绿（16/17 输入不变、逐字节还原成立、6 符号零漂移、cmdline 996 B 与 stage2 逐字相同） |
| UEFI | `fw/boot-17line-043f7120.img` sha `043f7120…a5bd3` 在位 |
| 账本 | 末条仍 `06:42:38Z boot`（`stage2`），共 3209 条 |
| 脚本审计 | `cmd_xnu()`（`:165-183`）只 `flash_vendor_boot` + `preflight` + `fastboot boot <UEFI>`；**无 `fastboot reboot`**，`boot`/`recovery`/`userdata` 三个分区零写入 |

### 69.33.2 上机执行（唯一入口）

```bash
MUYU_UEFI=fw/boot-17line-043f7120.img \
  tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_fix1.img
```

`logs/flash-fix1.log`：

```
== slot=a unbootable=no retry=7
WARNING: battery 3692 mV; charge before a long run
== fw/vendor_boot_gpu_fix1.img
   vendor_boot v4 73.81 MiB vendor_ramdisk=72.00 MiB aligned=True
   sha256 a004368bdf97c648d7a7433abae75d511b2a3d15d755a1e6289361f3254a39b7
   Writing 'vendor_boot'     OKAY [  0.053s]
== booting UEFI from RAM: fw/boot-17line-043f7120.img
   Booting                 OKAY [  0.199s]
```

账本新增四条，**全 `rc=0`**：`07:06:37Z preflight` / `07:06:40Z flash vendor_boot fw/vendor_boot_gpu_fix1.img a004368b…37b7` /
`07:06:40Z preflight` / `07:06:40Z boot fw/boot-17line-043f7120.img 043f7120…a5bd3`。
**无 `Load Error`**；引导后 `fastboot devices` 与 `adb devices` 皆空 ⇒ 设备已离开 fastboot 在跑 XNU，
**面板是唯一输出通道**。

### 69.33.3 边界与判读格

`flashing=true / device_touched=true / userdata_written=false / qemu_cells_run=0`；
⚠️ **`board_verified=false / screen_photo_captured=false` 保持**：刷写与引导成功 **≠** 板上验证。
`minus3_persists=true` 也保持（尚未读到本版屏照），**本轮不得宣称修复生效**。
`stage2` / `stage1` / `grant1` 三个对照镜像 sha 未变，保留 A/B 对照；`boot`/`recovery`/`userdata` 未动。

屏照须含顶部 `MUYU LIVE`，逐字抄 `MUYU B`（16 字段）、`R`、`GPU`、`A`。判读格见 **§69.32.6**
与冷启动 §9 第 10 条：**主判据 `pf=0 q=0`**；`pf=1 q=1` ⇒ 修复未生效（查刷错镜像 / helper 未编入）；
`t7=3` ⇒ `fail():113` 的 `regions_[].tables->quarantine()` 成为下一 blocker（本版刻意未动）；
`gf>0` 是预期。唯一全通判据 `o>=1 且 c>=1` + `R=ok`。

⚠️ 与前三版不同：`fix1` **是行为改动**，离线 checker 全绿不能证明 `-3` 消失；若屏照读数异常，
先回查是否刷错镜像，再做 A/B（刷回 `stage2`），**不要**据此继续叠加新改动。

### 69.33.4 回滚与充电

板上直接重刷 `fw/vendor_boot_gpu_stage2.img`（`24961f7e…4470`）即可回到 §69.31 的已判读状态；
源码侧把 `baseline-pre-fix1/NativeAdrenoVm.cpp.orig` 覆盖回去并重跑三步链。
电池脚本读数 3692 mV 偏低：**充电前先刷回 `stock` vendor_boot**，别长时间墙充、
别再 `restore-stock --yes`；`fastboot reboot` 禁用。

---

## §69.34 2026-11-13 15:11：⭐⭐⭐ **`fix1` 屏照判读：quarantine 修复板上生效，`-3` 仍在，失败点下移到 `core->open()`**

### 69.34.1 逐字读数（分段放大复核）

屏照 `logs/IMG_20261113_151052.jpg`，**含顶部 `MUYU LIVE` 行**（`p=13 t=230`，ticker 在跑）。
整图缩小会把 `0`/`8` 与 `f`/`t` 字段看错，以下均由 3x 紧裁剪逐行复核：

```
MUYU LIVE p=13 t=230 btn st=9 irq=0 rep=0 msk=0x0 pon=476 gplo=-1 pdc=0x30200 err=0 sp=0 oth=0
MUYU B i48 o40 f2 s=e00002c2 b0 n4 t6=0 t7=0 ok=7 vm=1 pf=0 q=0 bk=0 dr=0 gf=1 rn=0
MUYU R PUB no device: device: Vulkan(VkResultCode(-3))
MUYU GPU calls=1 refused=0 o=1 c=0 f=0 fb=0 tc=0 ts=ffffffff hold=1 pres=1 l=1 w=1
MUYU V op=3 bad=0 cl=2 call=525 sel=6 st=e00002d6 at=1 lmf=0 mb=0/0 tr=14 gr=7
MUYU G mediaserverd/o1/r0/c1/p0/x0 backboardd/o2/r2/c1/p454/xe00002d5 n3
MUYU UP lg=10/1/1/1 s=1/0 op=3/2 mf=2/4 i=48 r=p
MUYU ROOT c=6/0/0 d=4/0    MUYU INIT q=6    MUYU BSD s=8
MUYU A n=10 SpringBoard/4147/00000000/re00002c7/a1/f0 …（同 §69.25，模拟 gate，非 open 拒绝）
```

### 69.34.2 ⭐ 主判据命中：`fix1` 在板上确实生效

| 字段 | stage2（14:46） | **fix1（15:11）** | 含义 |
| --- | --- | --- | --- |
| `pf` | `1` | **`0`** | `preflight()` 不再记录任何拒绝分支 |
| `q` | `1` | **`0`** | 共享 `buffers_[0]` 不再处于 quarantine |
| `rn` | `4` | **`0`** | 零次 preflight 拒绝（原 4 次全同源） |
| `t7` | `4` | **`0`** | `createVm()` **完成**（`GpuContext.cpp:198` 的 stage 0），不再卡第一个 map |
| `t6` | `e00002c2` | **`0`** | 最后一次 createVm 失败码清零 |
| `ok` | `3` | **`7`** | createVm 累计成功 3 → **7** 次 |
| `bk/dr` | `1/2` | **`0/0`** | 只在拒绝时才采样，无拒绝故为 0，**不是** buffer 失效 |
| `gf` | `1` | `1` | `fail()` 仍跑过一次，符合预期（本版只跳过隔离） |

归因链前半段闭合：`fail()` 不再对驱动自有共享 buffer 调 `quarantine()` ⇒
`preflight():209` 不再返回 `0xe00002c2` ⇒ `createVm()` 从「每次都失败」变成「能完成」。
`UP op=3/2` 与 `V op=3 cl=2` 互相印证：**已有 3 个用户客户端成功 mount、2 个已关闭、1 个仍 live**（`vm=1`）。
`ok=7 = 4 次 mount 失败 + 3 次成功`，三个计数器完全自洽。

### 69.34.3 ❌ 但 `-3` 未消失：**失败点已从 `createVm()` 下移到 `core->open()`**

`R PUB no device: device: Vulkan(VkResultCode(-3))` 逐字未变，`GPU o=1 c=0` 逐字未变，
`ROOT/INIT/BSD` 全不变 ⇒ **全通判据 `o>=1 且 c>=1` + `R=ok` 仍未满足**，首帧依旧冻结。

⭐ **新硬证据 `f1 → f2`，而 `s=e00002c2` 未变**：`f` = `native_adreno_mount_fail[0]`，
`NativeAdrenoPublish.cpp:381` 写的是 `space?2:1`：

```cpp
opened = g.context ? g.context->createVm(&space) : kIOReturnNotReady;
vm = space ? space->vm() : nullptr;
if(!opened) opened = core->open(NativeAdrenoAbiVersion,vm,g.submit,g.ring,g.input_bits,g.output_bits);
if(opened != kIOReturnSuccess) {mount_fail[0] = space ? 2 : 1; mount_fail[1] = (uint32_t)opened; ...}
```

`createVm()` 成功才把 `*out=space` 写出（失败路径 `*out=nullptr`），因此 **`f=2` 只可能是
「createVm 成功、但下游 `core->open()` 返回非 0」**；`f=1`（旧值）才是「连 address space 都没有」。
结合 `s=e00002c2 = kIOReturnBadArgument`，新拒绝点唯一落在
`NativeAdrenoUserClient.cpp:129-131` 的 `open()`：

```cpp
if(!vm || !submit || !ring || input_bits<31 || input_bits>48 ||
   output_bits<32 || output_bits>48 || vm->poisoned() || submit->poisoned() ||
   ring->poisoned()) return kIOReturnBadArgument;
```

`B i48 o40` 两个 bit 数都在合法区间、`vm` 来自刚成功的新 ProcessVm ⇒
**只剩 `vm/submit/ring->poisoned()` 三个候选**。`gf=1`、`H h5 r4 f1`（5 次 hang /
4 次 restart / 1 次 restart 失败）说明本次 boot 确有一次 GPU fault 与恢复流程，
最可能是**全局 `submit`/`ring` 的 poison 位没有复位**，使后续部分客户端的
`core->open()` 被拒 ⇒ `mount()` 失败 ⇒ `IOServiceOpen` 返回 Busy（`G` 行 `xe00002d5 n3`）
⇒ Turnip `-3`。⚠️ **这是最强候选，尚未板上确证**：屏上目前**没有**任何字段暴露这三个 poison 位。

### 69.34.4 结论与下一步

- ✅ `fix1` **是有效修复，但不是全通修复**：它清掉了 §69.31 钉死的 quarantine 阻塞，
  让 createVm 和 3 次 mount 真正跑通；不要再回滚它，也不要回退到 sandbox / uke 补丁。
- ❌ `-3` 板上仍在，且**原因已换**：不再是共享 buffer 污染，而是 `core->open()` 的
  BadArgument。⚠️ 若继续按 §69.31 的老判据表读 `pf/q`，会误判为「已解决」。
- **下一版应为纯观测 `stage3`**（改行为须另行授权）：在屏上补
  ① `open()` 的三个 poison 位与命中分支；② `g.opened/g.closed` 与失败进程名；
  ③ 3 个成功 mount 的客户端是谁、`backboardd` 是否在其中。
  零行为改动、同一 printf 追加字段、重核 checker/QEMU 符号，先离线重建、上机另行说明。
  观测坐实后，再评估「poison 复位 / 恢复路径」是否安全（须重核 DMA 静默与 TLB unmap，
  不要直接照搬 `retireAfterQuiescence()`）。

### 69.34.5 边界

本轮**纯判读**：零刷机、零引导、零重建、零设备动作。账本末条仍 `07:06:40Z`，
源码与镜像 sha 未变，`flashing=false / device_touched=false / qemu_cells_run=0`。
状态更新：`fix1_board_effective=true / quarantine_blocker_cleared=true /
minus3_persists=true`（**板上确认，不再是「保持」**）/ `board_verified=false` /
`downstream_blocker=core_open_BadArgument`。回滚仍可刷回 `stage2`（`24961f7e…4470`）。
电池 3692 mV 偏低，充电前刷回 `stock` vendor_boot，别长时间墙充、别再 `restore-stock --yes`。

---

## §69.35 2026-11-13 15:2x：型号映射澄清 —— muyu 是 Adreno 735，740 是 sheng 的；同时定位一批仍在板上生效的 A740 参数

用户指出「我们的 muyu 的 GPU 是 Adreno735，740 是 sheng 的」。**正确**，本轮做纯静态审计（零刷机、
零引导、零重建、零源码改动），把这条事实与当前归因链的关系固定下来。⚠️ 这**不是新发现**：
`GPU-PORT.md` §1 自 2026-10-28 就写明，`GPU-GMU-MAP.md:86` 有对照表；本轮的新增价值是
**把它与板上反复出现的 GPU fault/hang 关联起来，并列出仍未换掉的 740 参数**。

### 69.35.1 准确映射（工程内交叉验证）

| 项 | **muyu（17 线，本板）** | **sheng（27 线）** |
| --- | --- | --- |
| GPU | **Adreno 735**（SM8635 cliffs，`gen7-11-0`） | **Adreno 740**（SM8550，`ADRENO_7XX_GEN2`） |
| `qcom,chipid` | **`0x43030b00`**（`board.toml:116`，DT `qcom,gpu-model`） | **`0x43050a01`** |
| GMEM | **1.5 MiB / `0x180000`**（`board.toml:131`） | 3 MiB / `0x300000` |
| `gmu_chip_id` | `0x70b0000` | `0x7020100` |
| Mesa/freedreno 名 | `FD735`（`freedreno_devices.h:105` 表项 `{0,0x43030b00}`） | `FD740` |

工程依据：`UniXNU-muyu-17/board.toml [gpu] model = "Adreno735"`（由
`scripts/gen-muyu-gpu-table.py` 从 `fw/dtb_04.dtb` 生成，`--check` 可验）；
`GPU-HANDOFF.md:914` `SHENG_GMU_VOTES_CHIP_ID 0x43030b00u /* DT qcom,chipid, Adreno735 */`；
`GPU-PORT.md:8-9`「官方组件 `Adreno740` 是给 A740 的 ⇒ 不能直接用，需要一层 A735 后端」；
`GPU-GMU-MAP.md:86`。§13 曾就 chip id 不匹配（`ShengGmuVotes.c` 首道检查）落过
`fw/vendor_boot_gpu_chipid.img` 并上机读数，那一处**已经修过**。

### 69.35.2 `drivers/Adreno740/` 是目录名，不是硬件声明

`UniXNU/drivers/Adreno740/` 与 `board.toml` 的 `build-arm-driver.py Adreno740`、
`libAdreno740.a` 沿用 uke/sheng 的**共享 A7xx 驱动框架名**：A735 与 A740 的 dword
寄存器映射相同（`MuyuA735HwInit.h:10`「register offsets are the same A7XX map as A740」），
所以共用目录本身**没有错**，也不该改名或分叉。

profile 表刻意不新增 A735 条目：`GPU-UKE-ALIGNMENT.md:21-22` 说明 uke 的 A732 与我们
的 A735 是**同一个 chip id**。`NativeAdrenoProfile.cpp:24` 把 `0x43030b00` 命名为
`"Adreno732"`、`gmem_bytes=0x180000`（与 A735 实测一致）⇒ 数值正确，但
`NativeAdrenoService.cpp:274` 的 IOLog 会打印 `Adreno732 chip=…`，**读日志时别误判成型号错配**。

### 69.35.3 ⭐ 已知但未闭环的欠账：板上仍在写 A740 参数

`GPU-PORT.md` §1 第 5 条欠账原文：「**A735 硬初始化**：现有 `NativeAdrenoA740HwInit.h`
是逐型号的寄存器序列，需要 A735 版本」。本轮核到板上实际生效的三处：

| # | 位置 | 现状 | 与 A735 的差 |
| --- | --- | --- | --- |
| 1 | `GpuPowerProbe.cpp:1377-1380` | `hw_init` 里写 **`NativeAdrenoA740Hwcg[]`** | `MuyuA735Hwcg.h:6-8` 明写「A740 table … is the **wrong value set for A735: nine registers carry different clock-gating values**」。`MuyuA735ApplyHwcg()` 已实现且过 host tests，但**只有 HostTests 引用**，board.toml archives / build scripts / build logs 全无 ⇒ **未编进内核** |
| 2 | `GpuPowerProbe.cpp:1386-1387` | `A740_UCHE_GMEM_RANGE_MAX = 0x1000000 + 3*0x100000 - 1` | 按 **3 MiB** 写；A735 只有 **1.5 MiB** ⇒ 上限超出物理 GMEM 一倍 |
| 3 | `GpuPowerProbe.cpp:1029` | `wrapperRound()` 自检 `info.chip_id==0x43050a01 && gmem==3MiB` | 板上必然记一次 `kIOReturnError`。⚠️ 仅影响探针自检的 `ok/n/err` 计数，**不是硬件路径、不是 `-3` 来源** |

排除项：`AdrenoCompilerProbe.c:22` 的 `0x43050a01 /* pinned sheng DTS/catalog */` 属
`scripts/build-adreno-compiler-host.py` 的 **host-only** 编译器探针，不进设备内核；
`MuyuA735Hwcg.h:20` 的 `MUYU_A735_GMU_CHIP_ID 0x07110000` 与 board.toml `0x70b0000`
不一致，但该头未编入 ⇒ 两者当前都无板上影响。

### 69.35.4 对现有归因链的影响（关键，别过度反应）

- ✅ **下游结论全部不变**：quarantine 污染（§69.31 `pf=1 q=1`）、`fix1` 修复生效
  （§69.34 `pf=0 q=0`、`ok 3→7`）、以及现在的 `f=2` / `core->open()` BadArgument /
  三个 poison 位候选，都是 **VM/内存/用户客户端的纯软件状态机**，与 GPU 型号、
  GMEM 大小、hwcg 值表无关 ⇒ **不要回滚 `fix1`，不要退回 sandbox / uke 补丁，
  `stage3` 纯观测方案照旧**。
- ⚠️ **上游「为什么会 fault」可能是型号根因**：§69.34 的 `gf=1` 与
  `H h5 r4 f1`（5 次 hang / 4 次 restart / 1 次失败）说明本次 boot 确有一次 GPU fault。
  #1 的错误时钟门控值 + #2 的 GMEM 范围越界，是 plausible 的 fault 触发源；
  若成立，poison 复位只是治标，fault 还会再来。
- ❌ **但这是静态推断，未板上确证**：屏上目前**没有**字段暴露实际 `chip_id`、
  `profile_->name`、`gmem_bytes`、GMEM_RANGE 写入值或 hwcg 表选择；§69.34 屏照转录
  也没有 `MUYU F`（`BravearkLastLog.c:892`，含 fault 的 FSR/FAR/FSYNR0）⇒
  无法区分这次 fault 是 **SMMU 地址 fault** 还是 **GPU hang / 时钟问题**。
- 这也补强上一轮「虚拟机为什么没问题」的答案：uke/QEMU 的 `offline-gpu-broker`
  根本不写这些寄存器，#1/#2 是**真机独有路径**。

### 69.35.5 给 `stage3` 追加的观测项（仍属零行为改动）

在原有「三个 poison 位 + 命中分支 + `g.opened/g.closed` + 失败进程名 + 成功 mount 身份」
之外，同一 printf 追加只读字段：① `profile_->chip_id / gmem_bytes / gmu_chip_id`；
② `hw_init` 实际选用的 hwcg 表标记（A740 还是 A735）；③ `A740_UCHE_GMEM_RANGE_MAX`
的实际写入值；④ 下一张屏照**必须逐字抄 `MUYU F` 行**（FSR/FAR/FSYNR0 能区分 fault 类型）。
判据：若 FAR 落在 GMEM 区间附近 ⇒ #2 坐实；若 FSR 指向 clock/hang 而非地址 ⇒ 倾向 #1。
**改这些参数属于改行为（另一个变量），须单独授权、单独一版，不要和 poison 复位混在一版里。**

### 69.35.6 边界

本轮纯静态审计：零刷机、零引导、零重建、零源码改动、零设备动作。账本末条仍
`07:06:40Z`，源码 / 镜像 sha 未变，`flashing=false / device_touched=false /
board_verified=false / minus3_persists=true` 保持。新增状态：
`muyu_gpu_is_adreno735=true / a740_profile_params_still_active=true /
chip_model_root_cause_board_verified=false`。

---

## §69.36 2026-11-13 15:4x：⭐⭐ 按 §69.34.4 + §69.35.5 落纯观测 `stage3`，离线重建完成、停在「待上机」

用户「继续」。**仍是零行为改动**、零刷机、零引导、零格 QEMU。本版一次把
`core->open()` 的三个 poison 位、命中分支，以及 §69.35 的 A740 参数欠账
全部打上屏，供下一张屏照定性。⚠️ **不是修复版**，`-3` 仍在是预期。

### 69.36.1 审读后收敛掉的「不必新增」项

§69.34.4 原本列了五类观测，源码核对后发现**三类屏上已经有了**，不再重复造字段：

| 需求 | 现成来源 |
| --- | --- |
| `g.opened/g.closed` | `MUYU UP op=%u/%u`（`BravearkLastLog.c:1063` 调 `native_adreno_publish_stats()`，`Publish.cpp:1301`） |
| 失败进程名 / 成功 mount 的客户端身份 | `MUYU G` 逐进程 `o/r/c/p/x`（`Publish.cpp:1117` 的 `record->opens++` 发生在 `refusal==0` 之后 ⇒ `o` 就是**成功 mount 次数**；15:11 屏照 `mediaserverd/o1` + `backboardd/o2` = 3，与 `UP op=3` 自洽，`x` 是 last_refusal） |
| fault 的 FSR/FAR/FSYNR0 | `MUYU F`（`BravearkLastLog.c:891`），**上一张屏照没抄**，下一张必须逐字抄，无需改代码 |

⇒ 本版只补真正缺的：poison 位/分支、profile 实测值、hw_init 实际写入的表与 GMEM 窗口。

### 69.36.2 四个源码改动（20 受控输入中 16 个不变）

在 `baseline-pre-stage3/` 冻结 20 项输入（17 项沿用 `pre-fix1`，新增
`NativeAdrenoUserClient.cpp` / `NativeAdrenoService.cpp` / `GpuPowerProbe.cpp`
三项）+ 五份 `.orig` + 内核/payload。

| 文件 | 新增 |
| --- | --- |
| `NativeAdrenoUserClient.cpp` | `native_adreno_open_probe[8]` + `nativeAdrenoOpenRefusal()` helper；把 `open():129-131` 的复合 `return kIOReturnBadArgument` 包成 `return nativeAdrenoOpenRefusal(...)`（与 §69.29 同一手法，**单语句语义保住**）。helper 记录 poison 位掩码（1 vm / 2 submit / 4 ring）、命中分支（1 null / 2 input_bits / 3 output_bits / 4 poison）、bits、次数，**只写自己的 probe 数组、原值返回** |
| `NativeAdrenoService.cpp` | `native_adreno_profile_probe[8]`；`start()` 选定 profile 后记 `chip_id / gmem_bytes / gmu_chip_id / 次数` |
| `GpuPowerProbe.cpp` | `native_adreno_hwinit_probe[8]`；`gpuHwInit()` 记实际写入的 hwcg 表（1 = A740）、表项数、GMEM_RANGE_MIN/MAX、调用次数 |
| `BravearkLastLog.c` | 三个 `weak_import` 只读声明；**扩两行现有 printf**：`MUYU B` 16→19 字段加 `ps/br/ob`，`MUYU PROBE` 3→9 字段加 `ci/gm/gi/hw/hn/gr`。**零新增 printf、零新增屏行**（控制台只容纳一页） |

⚠️ 第一次构建在 `archive-12` 挂了：probe 全局定义放在文件首行，`-nostdlibinc`
下 `uint32_t` 尚未可见 ⇒ `error: unknown type name 'uint32_t'`。已把两处定义
挪到 include 之后重跑，三步链最终全绿，**三份日志无 error**。

### 69.36.3 产物与判据

三步链参数与 `fix1` **逐字相同**，cmdline **996 B 逐字相同**：

| 产物 | 值 |
| --- | --- |
| `fw/vendor_boot_gpu_stage3.img` | 77,393,920 B，sha **`75896e43…720c56b`** |
| `kernel.macho` | **`1fb58bf3…cb35`** |
| `payload.img` | **`b376003e…ecf1`** |

新 checker `UniXNU/scripts/check-stage3-rebuild.py`（+ tree 副本，**只能从
`UniXNU/scripts/` 运行**）**全绿**，判据 JSON
`out/muyu/tree/build/host-turnip-input/stage3-rebuild.json`：

- 20 项输入 **16 项不变、恰好 4 项按预期变化**；
- ⭐ **四份源码各自「删掉白名单诊断行 + 还原被改的 printf/return 行」后与 `.orig` 逐字节相同**（difflib 逐 opcode 校验，任何非诊断改写 / 删行 / 触碰 `quarantine|poison()|fail(|release(|gpuWr(` 的新增行都直接 fail）；
- helper 只写 `native_adreno_open_probe`、只 `return result`；屏侧 `printf("MUYU ` 行数不变；
- tree 四份副本 sha 与主源码逐一相同 ⇒ 构建真的用了改动；
- 新内核含两个新格式串、保留 stage2 的 16 字段 B 行与 grant1 名单、无 uke 补丁、`kernel.json` 对齐；`skip-owner` 仍 `c4c1420a…`；
- ⭐ **符号位移如实断言**：三个**代码**符号（`_load_init_program` / `_native_apfs_probe_checkpoint` / `_vfs_mountroot`）**零漂移** ⇒ §69.17 QEMU 定点补丁仍适用；三个**数据**符号因新增 3×32 B probe 全局按「前面有几个新 probe」精确平移 `+0x20 / +0x40 / +0x40`，checker 逐个验算，**不再谎报「六符号零漂移」**；
- 三个新 probe 符号 `nm` 可见（`open=fffffff007bfc038` / `profile=fffffff007bf7db8` / `hwinit=fffffff007c01da8`）⇒ 屏上不会全打印 `ffffffff`；
- `fix1`/`stage2`/`stage1`/`grant1` 四个对照镜像 sha 未变、三日志无 error。

### 69.36.4 ⚠️ 上机判读格（本版唯一用途）

屏照**必须含顶部 `MUYU LIVE`**，逐字抄 `MUYU B`（**19 字段**）、`MUYU PROBE`（**9 字段**）、
`MUYU F`、`MUYU G`、`MUYU UP`、`R`、`GPU`。

| 读数 | 结论 |
| --- | --- |
| **`ps!=0 br=4`** | ⭐ poison 假设**板上坐实**：全局 `vm`/`submit`/`ring` 的某一位没复位，`core->open()` 因此拒。下一步才是改行为的 poison 复位版，**须单独授权**并重核 DMA 静默 / TLB unmap |
| `ps=0 br=1` | null `vm/submit/ring`，不是 poison ⇒ 回查 `createVm()` 的生命周期 |
| `ps=0 br=2/3` | bits 越界，与 `i48 o40` 矛盾 ⇒ 先复核读数 |
| `ob=0` | `open()` 从未被拒，`f=2` 另有出处 ⇒ 回查 `mount()` 的其它分支 |
| `ci=43030b00 gm=1536 gi=70b0000` | A735 profile 选中正确（§69.35.1） |
| **`hw=1 hn=54 gr=12fffff`** | ⭐ §69.35 的 A740 hwcg 值表 + 3 MiB GMEM 窗口**板上坐实**：这就是 `gf=1` / `H h5 r4 f1` 反复 fault 的上游嫌疑 |
| `MUYU F` 的 FAR 落在 GMEM 区间 | GMEM 越界 ⇒ ② 成立；FSR 指向 clock/hang 而非地址 ⇒ 倾向 ① 错误 hwcg |
| `o>=1 且 c>=1` + `R=ok` | 唯一全通条件 |

⚠️ **改 hwcg 表 / GMEM 范围是另一个行为变量，不要和 poison 复位混在同一版**；
也不要因为 `ps` 命中就回滚 `fix1` 或退回 sandbox / uke 补丁。

### 69.36.5 边界

本轮离线重建：零刷机、零引导、零格 QEMU、零设备动作。账本末条仍
`07:06:40Z`（`fix1` boot），`flashing=false / device_touched=false /
board_verified=false / minus3_persists=true` 保持；新增
`stage3_probe_built=true / stage3_board_verified=false`。
回滚：覆盖 `baseline-pre-stage3/` 五份 `.orig` 重跑三步链，或直接刷回
`fw/vendor_boot_gpu_fix1.img`（`a004368b…37b7`）。
电池 3692 mV 偏低，充电前先刷回 `stock` vendor_boot，**上机须另行说明**。

> 后续：本节「待上机」状态已被 **§69.37（15:56 已上机）** 取代，本节保留为离线重建记录。


## §69.37 2026-11-13 15:56：`stage3` 纯观测版已上机（用户「在线」授权）—— **屏照待拍**

### 69.37.1 只读预检（全绿）

| 项 | 读数 |
| --- | --- |
| 设备 | `fastboot devices` ⇒ `a20248f7  fastboot`；`adb devices` 空 |
| slot | `current-slot=a` / `slot-unbootable:a=no` / `slot-retry-count:a=7` |
| 电池 | `getvar battery-voltage` **3757 mV**；脚本 `preflight` 两次读到 **3670 / 3668 mV**，触发 `<3700` WARNING 但**不阻断** |
| 镜像 | `fw/vendor_boot_gpu_stage3.img` 77,393,920 B，sha **`75896e43…720c56b`**，与 `out/muyu/tree/build/host-turnip-input/stage3-rebuild.json` 一致；头 8 B `VNDRBOOT` |
| checker | `UniXNU/scripts/check-stage3-rebuild.py` 复跑全绿：16/20 输入不变、四份源码还原后与 `.orig` 逐字节相同、3 个代码符号零漂移、cmdline **996 B 与 fix1 逐字相同** |
| UEFI | `fw/boot-17line-043f7120.img` 2,129,920 B / `043f7120…` 在位 |
| 账本 | 末条仍 `07:06:40Z`（`fix1` boot），`status` 显示 3217 条 |
| 脚本边界 | 再审 `cmd_xnu()`（`:165-183`）：只 `flash_vendor_boot` + `preflight` + `fastboot boot <UEFI>`，**无 `fastboot reboot`** |

### 69.37.2 执行与结果

```
MUYU_UEFI=fw/boot-17line-043f7120.img tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_stage3.img
```

- `Writing 'vendor_boot'` **OKAY [0.056s]**；`Booting` **OKAY [0.189s]**，**无 `Load Error`**。
- 日志 `logs/flash-stage3.log`；账本新增 `07:56:27Z preflight` / `07:56:29Z flash` /
  `07:56:30Z preflight` / `07:56:30Z boot`，**四条全 `rc=0`**。
- 引导后 `fastboot devices` / `adb devices` **皆空** ⇒ 设备已离开 fastboot 在跑 XNU，
  **面板是唯一输出通道**。
- 只写了 `vendor_boot`；`boot` / `recovery` / `userdata` 未动，未用 `fastboot reboot`。
- `fix1` / `stage2` / `stage1` / `grant1` 四个对照镜像 sha 未变（A/B 对照与回滚保留）。

### 69.37.3 ⚠️ 刷写成功 ≠ 板上验证

`board_verified=false / screen_photo_captured=false` **保持**。本版是
**纯观测、零行为改动**：19 字段 `MUYU B` 的 `ps/br/ob` 与 9 字段 `MUYU PROBE` 的
`ci/gm/gi/hw/hn/gr` 是否真的打印、weak_import 是否绑定、`-3` 是否变化，**必须等屏照**。
`minus3_persists=true` 保持；**`-3` 仍在是预期，不是重建或刷写失败**。

### 69.37.4 请拍屏照 / 判读格（详见 §69.36.4、§9 第 11 条）

等 ticker 刷 3–4 遍后拍完整屏，**必须含顶部 `MUYU LIVE` 行**，逐字抄
`MUYU B`（19 字段）、`MUYU PROBE`（9 字段）、**`MUYU F`（FSR/FAR，上一张漏抄）**、
`MUYU G`、`MUYU UP`、`R`、`GPU`。

| 读数 | 结论 |
| --- | --- |
| **`ps!=0 br=4`** | ⭐ poison 假设板上坐实 ⇒ 下一步才是改行为的复位版，**须单独授权**并重核 DMA 静默 / TLB unmap |
| `ps=0 br=1` / `br=2/3` / `ob=0` | null 对象 / bits 越界 / open 从未被拒（各自回查） |
| `ci=43030b00 gm=1536 gi=70b0000` | A735 profile 选中正确 |
| **`hw=1 hn=54 gr=12fffff`** | ⭐ A740 hwcg 表 + 3 MiB GMEM 窗口板上坐实 ⇒ §69.35 欠账成立；配 `F` 行 FAR 判越界还是时钟 |
| 全 `ffffffff` | weak_import 绑定异常，先查符号而非下结论 |

⚠️ **改 hwcg 表 / GMEM 范围与 poison 复位是两个独立行为变量，不要混在一版**；
不要因为 `ps` 命中就回滚 `fix1` 或退回 sandbox / uke 补丁。
唯一全通条件仍是 **`o>=1 且 c>=1` + `R=ok`**。

### 69.37.5 边界

`flashing=true / device_touched=true / userdata_written=false / qemu_cells_run=0`；
账本末条 `07:56:30Z`；`board_verified=false / screen_photo_captured=false /
minus3_persists=true` 保持，新增 `stage3_board_running=true / stage3_board_verified=false`。
回滚：直接重刷 `fw/vendor_boot_gpu_fix1.img`（`a004368b…37b7`）回到 §69.34 已判读状态。
电池脚本读数 3668 mV 偏低：**充电前先刷回 `stock` vendor_boot**，别长时间墙充、
别再 `restore-stock --yes`；`fastboot reboot` 禁用。

> 后续：本节「屏照待拍」已被 **§69.38（16:00 屏照判读）** 取代；但屏照**不是** `MUYU LIVE` 面板，
> 本节 §69.36.4 的 19 字段判读格**一个字段都没读到**，详见 §69.38。


## §69.38 2026-11-13 16:00 屏照判读 —— ⚠️ 画面最终停在 `verify_swap failed` 刷屏，没翻到 MUYU 面板

### 69.38.1 用户澄清（本节的立论前提）

用户「最后停在这里」被上一轮误解成「让助手停手」，实际指的是 **设备屏幕上的画面最终定格在照片里这一屏**。
用户先后两条原话（⭐ 决定判读方向；第二条是对第一条的修正）：

> ①「大概是从第一次 muyu gpu 的 o=1 开始就有这些刷屏日志，不过只是这次停在了这里，
>    以前都是过了这些就到 muyu 面板」
> ②「**只是从 muyu gpu 中 o=1 开始没看到苹果 logo，其余均能看到**」（16:19 再纠正）

⚠️ 曾把 ① 里的「都没能到看到苹果 logo」误读成「所有版本从没到过 logo」—— **错的，已作废**。
**最终口径：`o=0` 的早期版本都能看到苹果 logo；从第一次 `MUYU GPU o=1` 起才看不到。**

⇒ 板上事实（**用户观察，不是推断**）：
1. `verify_swap failed` 刷屏**不是 `stage3` 引入的**，从第一次 `MUYU GPU o=1` 起就一直存在；
2. **变化的只有一点**：以前刷屏之后会翻到 `MUYU LIVE` ticker 面板，**这次没有翻过去，画面停在刷屏这一屏**；
3. ⭐ **`o=1` 是拐点**：`o=0` 期间能看到苹果 logo；从第一次 `o=1` 起，**刷屏出现且苹果 logo 消失**，
   之后所有版本（`stage1`/`stage2`/`fix1`，`o` 恒为 1、`c=0`）都如此。
   三条现象同一起点 ⇒ 把责任指向「**显示 hand-off 首次成功之后首帧没完成**」，
   而**不是**「显示链从未打通」（因果属推断，未板上确证）。

### 69.38.2 逐字读数（`logs/IMG_20261113_160026.jpg`，16:00:26）

3x 分段紧裁剪复核（整图缩小会把 `verify_swap` 看成 `verify_map`，已纠正）：

```
首行：NativeLegacyDisplay: verify_swap failed for swap id: 2292
…（连续逐行递增，无跳号、无其它文本）…
末行：NativeLegacyDisplay: verify_swap failed for swap id: 2344   ← 画面最终定格于此
```

- 中间**唯一一行非刷屏日志**：
  `tx_flush:1330: disk1 xld 7329 tx stats: # 100 owait 14 7100us finish 113 bar2 100 f 7 enter 85 fq 30 1186 450us close 0us prog … flush 1852us`
  （APFS transaction flush 的耗时统计，末尾两个 us 数值在宽裁剪里略有歧义，不影响判读）
- **全屏零 `MUYU` 行**：`LIVE` / `B` / `PROBE` / `F` / `G` / `UP` / `R` / `GPU` / `ROOT` / `H` 一个都没有。
- 末行之后**没有任何文字、光标、进度条、图标**（本张照片里也没有苹果 logo）⇒ 不是「还没跑到」，
  是稳态定格。⚠️ 这只是 `stage3` 这一 boot 的画面；**`o=0` 期间的各版是能看到苹果 logo 的**（§69.38.1 ②）。
- 因此 **§69.36.4 的判读格完全无法应用**：`ps/br/ob`（poison）、`hw/hn/gr`（A740 hwcg / GMEM）、
  `ci/gm/gi`（A735 profile）、`MUYU F` 的 FSR/FAR、`R` 的 `-3`、`GPU` 的 `o/c` —— **全部零读数**。
  ⚠️ **`stage3` 这一版在板上没有产出任何观测数据**；poison 假设与 §69.35 的 A740 参数欠账
  **依然未板上确证**，既没证实也没证伪。

### 69.38.3 这批刷屏的机制（工程内早有结论，**不是新发现**）

`HANDOFF-2026-10-27.md` §G.2 原文：把「冻屏」做成**拒绝所有 swap**
（`validateSwap()` 里 return false）后，**原厂 family 代码对每一次被拒的 swap 都 printf 一行**
`verify_swap failed for swap id: %d`，字符串在闭源 `kernelcache.decompressed` 里、
**不在我们源码中**（所以全工程 grep 不到实现），一秒几十行，直接把 dump 刷没。
§G.4 的正确做法是：**校验照常通过，只在搬运阶段丢帧**（`kickOnce()` 里
`if(native_legacy_swap_hold) return;`），既不打印任何东西，又让 scanout 停在文字上。

本板 cmdline（996 B，与 `fix1` 逐字相同）里同时有：
`offline-legacy-validate=1`（启用严格 `validateSwap()`）、`muyu-dump-at=20`
（t≈20 s `muyu_console_take_panel()` → 置 `native_legacy_swap_hold=1` → `muyu_console_wake()`
→ **每 5 s 重绘整批 MUYU 行**，§49.2）、`muyu-touch-quiet=1`、`offline-legacy-transfer=1`。

⭐ **待查点（不是板上结论）**：实际编译的 `UniXNU-muyu-17/drivers/GopFramebuffer/NativeLegacyDisplay.cpp`
（与 `out/muyu/tree/` 副本 sha 完全一致）里 **grep 不到 `swap_hold` 的读取**；
`kickOnce():239` 与 `validateSwap():413` 都没有 §G.4 那个丢帧分支。
`native_legacy_swap_hold` 目前只在 `LegacyDisplayProbe.cpp:134` 定义、被
`BravearkLastLog.c:453` 写入、被屏上 `hold=` 字段读取 ⇒ **变量被写了，但编译版显示驱动可能没消费它**。
若属实，就能解释「刷屏一直在、翻页只靠时序/运气」。⚠️ 这是静态推断，**尚未板上确证**，
且旧 §G.4 说的是 `NativeMobileDisplay` 另一份实现，不要直接当成回归结论。

### 69.38.4 ⚠️ 结论：本轮真正暴露的是**观测通道本身不可用**

1. `-3` 是否变化**无法从这张照片判断**（`R` / `GPU` 行都没有）；`minus3_persists=true` 保持，
   但**不新增**任何关于 `-3` 的板上证据。
2. **不要**把「停在刷屏」归因为 `stage3` 回归：`stage3` 是纯观测、零行为改动，
   `check-stage3-rebuild.py` 全绿（四份源码还原后与 `.orig` 逐字节相同、3 个代码符号零漂移），
   新增 3×32 B 全局只让数据符号按预期平移 `+0x20/+0x40/+0x40`；而用户明确说刷屏**自首次 `o=1` 就有**。
   要区分「`stage3` 引入翻页失败」与「随机时序」，唯一单变量实验是 **A/B 刷回 `fix1`**
   （板上已判读、当时能出 `MUYU LIVE p=13 t=230` 面板），**须单独授权上机**。
3. ⭐ **本轮最有价值的新线索：`o=1` 是拐点**（用户 16:19 纠正，非推断）。
   `o=0` 期间**能看到苹果 logo、没有这批刷屏**；从第一次 `MUYU GPU o=1` 起
   **刷屏开始 + 苹果 logo 消失**，`stage1`/`stage2`/`fix1` 一直如此。
   结合 §69.34 板上 `o=1 c=0`（hand-off 首次成功、首帧冻结、从未 `completeSwap`）与
   §69.28 的 `GPU calls=1 refused=0 o=1 c=0`：三者同起点，指向
   **「显示 hand-off 第一次成功之后，首帧再没完成」**这条链，而不是「GPU 从未打通」。
   ⚠️ 这是**归因候选、未板上确证**：屏上目前无字段区分「首帧未完成导致 swap 被拒」
   与「GPU 设备故障导致首帧未完成」，也解释不了为何 `o=0` 时反而能到 logo。
   但它解释了为什么 `fix1` 把 `pf/q` 清零、`f` 下移之后画面仍冻在首帧。
4. ⚠️ **战略含义（观测通道）**：以前能读到 `MUYU B` 只是**文字控制台 dump 恰好赢了刷屏**，
   不是 GPU 接管显示；这次连 dump 都没赢。**下一版不该继续往屏上加 probe 字段**：
   面板没接管时 19 字段 / 9 字段一律读不到，加了也是白加。应先让 `MUYU` ticker 稳定上屏
   （查 §69.38.3 的 hold 消费点），或改用不依赖 scanout 的输出通道
   （`msgbuf` / 串口 / 心跳行），再谈 poison 判读。
5. 按 §49.1 的教训：用户已明确「最终停在这里」= 稳态，**不要叫用户再拍同一屏**。

### 69.38.5 边界

本轮**纯判读**：零刷机、零引导、零重建、零源码改动、零设备动作。
账本末条仍 `07:56:30Z`（`stage3` boot），四个对照镜像与源码 sha 未变；
`flashing=false / device_touched=false / board_verified=false / minus3_persists=true` 保持，
新增 `stage3_panel_reached=false / stage3_observation_data=false /
verify_swap_flood_is_historical=true / o1_is_logo_regression_point=true /
apple_logo_visible_when_o0=true`。
`screen_photo_captured=true` 但 **`muyu_panel_captured=false`**。
电池 3668 mV 偏低，充电前先刷回 `stock` vendor_boot，别长时间墙充、别再 `restore-stock --yes`。

## §69.39 2026-11-13 16:3x：`fix2` —— 上游 A735 参数 + 观测通道一起修（离线重建完成，待上机）

用户「我只想切实解决问题」⇒ 不再加纯观测字段，直接落两个行为修复，一次重建、一次上机。

### 69.39.1 为什么是这两刀

1. **上游**：§69.35 的欠账板上一直没确证，但源码是确定的 —— muyu 是 A735
   （`board.toml [gpu] model="Adreno735"` / `0x43030b00` / **1.5 MiB GMEM**），
   而 `GpuPowerProbe.cpp::gpuHwInit()` 写的是 sheng 的 `NativeAdrenoA740Hwcg[]`
   （54 项，9 个寄存器值不同）和 `0x1000000 + 3*0x100000 - 1`（3 MiB 上限）。
   `fix1` 之后 `gf=1` / `H h5 r4 f1` 仍在 ⇒ 确有一次 GPU fault；错误时钟门控 +
   GMEM 窗口超物理一倍正是 §69.35 明确列的 plausible fault 源。
2. **观测通道**：§69.38 的「零 MUYU 行」在源码里坐实 —— 全工程
   `*NativeLegacyDisplay.cpp` **grep 不到 `native_legacy_swap_hold` 的任何读取**：
   变量只在 `LegacyDisplayProbe.cpp:134` 定义、被 `BravearkLastLog.c:453` 写入、
   被屏上 `hold=` 读 ⇒ **写了没人消费**。t+20 s 的 dump 因此没拿到面板，而
   `validateSwap():413` 严格校验继续拒绝 GPU 帧，原厂 family 每拒一次打印一行
   `verify_swap failed for swap id: %d`，把每 5 s 的重绘冲掉。
   `HANDOFF-2026-10-27.md §G.2/§G.4` 早有正确做法：校验照常通过、只在搬运阶段丢帧。

⚠️ 两刀分属不同层：#1 是 GPU 行为变量（直指 `-3`），#2 是显示/观测变量
（不改 GPU 状态机）；**不动 poison 复位**（§69.36.4 要求参数与 poison 分开成版），
**不回滚 `fix1`**、不退回 sandbox / uke 补丁。

### 69.39.2 源码改动（2 文件 / 6 hunk / +29 行）

| 文件 | 改动 |
| --- | --- |
| `UniXNU-muyu-17/package/GpuPowerProbe.cpp` | include `MuyuA735HwInit.h`；hwcg 循环改 `MuyuA735Hwcg[]` / `MUYU_A735_HWCG_COUNT`(53)；`GMEM_RANGE_MAX` 改 `0x1000000 + 0x180000 - 1`；`hwinit_probe[1..3]` 同步记 A735 值 |
| `UniXNU-muyu-17/drivers/GopFramebuffer/NativeLegacyDisplay.cpp` | 加 `extern "C" volatile int native_legacy_swap_hold`（强符号、同 archive）；`kickOnce():239` 开头 `if(native_legacy_swap_hold)return;`；`validateSwap():413` 在 hold 时 `addValidated()` + 观测计数 + `return true`（不拒绝 ⇒ 不刷屏） |

### 69.39.3 三步链与产物（cmdline 996 B 与 stage3/fix1 逐字相同）

| 产物 | 值 |
| --- | --- |
| `fw/vendor_boot_gpu_fix2.img` | 77,393,920 B，sha **`a563780a…6bf6b`** |
| `kernel.macho` | `d4709165…10` |
| `payload.img` | `9b8733df…ae23` |

`baseline-pre-fix2/` 冻结两份 `.orig` + 改前内核；三份日志
`logs/build-{kernel,payload,vendor-boot}-fix2.log` 零 error。

### 69.39.4 checker（`UniXNU/scripts/check-fix2-rebuild.py` + tree 副本，全绿）

判据 `out/muyu/tree/build/host-turnip-input/fix2-rebuild.json`：

- 两份源码「反向替换白名单块」后与 `.orig` **逐字节相同**（+12 / +17 行，新增行
  全部落在白名单）；
- 20 项受控输入中 16 项自 pre-stage3 未变，stage3 的四份里三份仍与
  `stage3-rebuild.json` 的 sha 逐字一致 ⇒ **只有 `GpuPowerProbe.cpp` 被 fix2 改**；
  `NativeLegacyDisplay.cpp` 作为第 21 项按预期变化；
- ⭐ 编译产物实证：`GpuPowerProbe.o` 的 `nm` 含 `__ZL12MuyuA735Hwcg`、
  **不含** `NativeAdrenoA740Hwcg`，反汇编含 `0x117ffff`（1.5 MiB 上限）；
  `NativeLegacyDisplay.o` 含 `_native_legacy_swap_hold` 引用；
- 新内核保留 stage3 的 19 字段 `B` / 9 字段 `PROBE` 格式串 + grant1 名单、
  无 uke `NativeSandboxGrantService`，`kernel.json.candidate_sha256` 对齐；
- 三个 QEMU 代码符号**零漂移**（§69.17 skip-owner 补丁仍适用），`skip-owner.macho`
  仍 `c4c1420a…`，五个对照镜像 sha 未变；cmdline 与 stage3 逐字相同。

### 69.39.5 上机判读格（本版唯一用途）

屏照须含顶部 `MUYU LIVE`；逐字抄 `B`(19) / `PROBE`(9) / **`F`(FSR/FAR)** / `R` / `GPU` / `G` / `UP`。

| 读数 | 结论 |
| --- | --- |
| **刷屏消失、`MUYU` 面板回来** | hold 消费修复生效，观测通道恢复 |
| **`hw=2 hn=53 gr=117ffff`** | A735 值表 + 1.5 MiB 窗口**板上坐实**，`ci=43030b00 gm=1536` 应同步 |
| **`gf=0` / `H` 的 fault 计数不再涨** | ⭐ 上游 fault 消失 ⇒ 参数错配就是 `gf=1` 的根因 |
| **`ps=0 br=0` + `f=0` + `R=ok` + `GPU c>=1`** | ⭐⭐ `-3` 消失、首帧完成 ⇒ **问题解决**，`board_verified` 才能翻 |
| `gf>0` 且 `ps!=0 br=4` | 参数不是唯一 fault 源，poison 假设板上坐实 ⇒ 下一版再做**单独的** poison 复位，重核 DMA 静默 / TLB unmap |
| `gf>0` 且 `F` 的 FAR 落在 `0x1000000..0x117ffff` 之外 | GMEM 越界或别处地址错误，按 FAR 定位 |

⚠️ 刷屏消失**不等于** `-3` 消失；`R` 行与 `GPU c` 才是主判据。
⚠️ 若仍停在刷屏，则 hold 分支没生效（可能 `muyu_console_take_panel()` 根本没跑到），
下一步查 `muyu-dump-at=20` 线程是否被首帧冻结挡住。

### 69.39.6 边界

本轮**零刷机、零引导、零设备动作**：账本末条仍 `07:56:30Z`（stage3 boot），
`flashing=false / device_touched=false / board_verified=false / minus3_persists=true` 保持。
新增 `fix2_rebuilt=true / a735_hwcg_active_in_build=true / swap_hold_consumer_restored=true /
fix2_board_verified=false`。上机仍须用户明确授权「设备在线 / 上机」；只写 `vendor_boot`
+ 17 线 UEFI RAM boot，不碰 `boot`/`recovery`/`userdata`，不用 `fastboot reboot`。
回滚：刷回 `fw/vendor_boot_gpu_fix1.img`（`a004368b…37b7`）或 `stage3`
（`75896e43…720c56b`）。电池 3668 mV 偏低，充电前刷回 `stock` vendor_boot，
别长时间墙充、别再 `restore-stock --yes`。

## 69.40 fix2 首次上机（2026-11-13 16:50 CST / 08:50 UTC）

用户「上机」（输入为「上级」）。只读预检：fastboot `a20248f7` 在线，
`current-slot=a / slot-unbootable=no`，电池 3750 mV（脚本两次读到 3656–3658 mV）。

```
MUYU_UEFI=fw/boot-17line-043f7120.img \
  tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_fix2.img
```

`EXIT=0`；`flash vendor_boot` OKAY [1.815 s]，`boot` OKAY [0.234 s]，
`preflight` 两次均 `slot=a unbootable=no`。账本新增：

| UTC | 动作 | 对象 | sha / 结果 |
| --- | --- | --- | --- |
| 08:50:15Z | flash | `fw/vendor_boot_gpu_fix2.img` | `a563780a…6bf6b` / rc=0 |
| 08:50:16Z | boot | `fw/boot-17line-043f7120.img` | `043f7120…5bd3` / rc=0 |

日志 `logs/flash-fix2.log`。**只写 `vendor_boot` + UEFI RAM boot**；
`boot`/`recovery`/`userdata` 未动，未用 `fastboot reboot`。
`device_touched=true / fix2_board_verified=false`；
`board_verified=false / minus3_persists=true` 在拿到屏照前保持不变。

判读格仍是 §69.39.5：**主判据是 `R` 行与 `GPU c`，不是刷屏是否消失**。
屏照需含 `MUYU LIVE`，逐字抄 `B` / `PROBE` / `F`(FSR/FAR) / `R` / `GPU`。
电池偏低，本轮屏照拍完即刷回 `stock` vendor_boot 再充电。

## 69.41 fix2 板上判读：`-3` 消失是假阴性；fix3 撤销永久丢帧

### 69.41.1 用户回报（16:55 CST）

> 刷屏消失，`-3` 消失，但 `muyu gpu calls=0`，且未见苹果 logo。

⚠️ **不能据此判定问题解决**。三条源码证据：

1. `LegacyDisplayProbe.cpp:69-98`（`muyu_handoff_snapshot`）中 `calls` =
   `native_legacy_gpu_offer_calls`，**只在 `kickOnce()` 真正走到
   `native_legacy_gpu_offer()` 时 +1**（`:70`）。
2. `BravearkLastLog.c:448-457`：`muyu_console_take_panel()` 在
   `muyu-dump-at=20` 的 t≈20 s 置 `native_legacy_swap_hold=1`；
   全工程 grep `native_legacy_swap_hold` 只有 `:453` 的置 1 与
   `LegacyDisplayProbe.cpp:134` 的定义 `=0`，**没有任何清零**。
   ⇒ fix2 按 `HANDOFF-2026-10-27 §G.4` 加进 `kickOnce()` 的
   `if(native_legacy_swap_hold)return;` 是**永久丢帧**，不是「dump 期间」。
3. §49.6 已记录过完全同型的一次 boot：`calls=0 tc=0 ts=ffffffff`、
   无 `R` 行、无 NATIVE-NVT frames、`-3` 消失，定性为「用户态图形栈整体没启动，
   断点比 `-3` 更靠前」。**`-3` 消失 = 没跑到 Vulkan 枚举，不是修好**。

⇒ 板上三条现象一次性解释：刷屏消失（`validateSwap` 的 hold 旁路生效，
validator 不再拒帧 ⇒ 原厂 family 不再打印 `verify_swap failed`）；
`calls=0` / logo 不见（transfer 与 GPU offer 被永久掐断）。
§3766 另已写明：`hold=1` 的 headless dump 下屏上本就应是一屏文本，
「未见苹果 logo」在此不是新的独立回归。

**结论：fix2 的 A735 参数那一刀这一轮根本没有被检验到。**
把「A735 参数」与「hold 消费」塞进同一版是设计失误 —— 无法归因。

### 69.41.2 fix3：只撤一刀

`NativeLegacyDisplay.cpp::kickOnce()` 删掉 3 行（2 行注释 +
`if(native_legacy_swap_hold)return;`），换成说明性注释；
**保留** `validateSwap()` 的 hold 分支（刷屏修复的真正来源）与
`GpuPowerProbe.cpp` 的 A735 值表 / 1.5 MiB GMEM（逐字节未动）。
5 s ticker 每遍重绘 dump，单帧被 present 不会把文字永久抹掉。

产物 `fw/vendor_boot_gpu_fix3.img` 77,393,920 B /
sha **`3de636d8…c4115`**；kernel `fa292e0c…9a6`、
payload `de09caa3…ec24`；cmdline 与 fix2 / stage3 逐字相同。

新增 `UniXNU/scripts/check-fix3-rebuild.py`（+ tree 副本）全绿，判据
`out/muyu/tree/build/host-turnip-input/fix3-rebuild.json`：
唯一变量经**反向替换逐字节还原 fix2 文件**证明；`GpuPowerProbe.cpp`
与 fix2 基线相同；`kickOnce` 内已无 hold 早退、validateSwap 分支仍在；
编译产物仍含 `MuyuA735Hwcg` / `0x117ffff` / swap_hold 引用、不含 A740 表；
三个 QEMU 符号零漂移、六个对照镜像（含 fix2）未覆盖、三份日志无 error。

### 69.41.3 状态与判读

`fix2_board_verified=false`、`minus3_persists=true` 保持；
新增 `fix2_minus3_absence_is_false_negative=true /
fix2_permanent_frame_drop=true / fix3_rebuilt=true / fix3_board_verified=false`。
fix3 构建轮**零刷机、零引导、零设备动作**；用户随后于 17:03 CST 授权上机，
见 §69.42。回滚：fix2 `a563780a…`、fix1 `a004368b…`、stage3 `75896e43…`。

fix3 上机主判据（顺序）：
① `calls>=1` ⇒ transfer 路径恢复，用户态图形栈真的起来了；
② 此时再看 `R` 行，**`-3` 仍在且 `gf>0 / ps!=0`** ⇒ A735 参数不是唯一 fault 源，
   下一版单独做 poison 复位；**`R=ok` + `GPU c>=1`** ⇒ `-3` 真消失，问题解决；
③ `PROBE hw=2 hn=53 gr=117ffff` ⇒ A735 参数板上落地（内核侧，与 calls 无关，
   fix2 那张屏照现在就能读到，可先确认）。

## 69.42 fix3 上机（2026-11-13 17:03 CST / 09:03 UTC）

用户「上机」。只读预检：fastboot `a20248f7` 在线，
`current-slot=a / slot-unbootable=no`，电池 3743 mV（本次无低电警告）。

```
MUYU_UEFI=fw/boot-17line-043f7120.img \
  tools/muyu-flash-mode.sh xnu fw/vendor_boot_gpu_fix3.img
```

`EXIT=0`；`flash vendor_boot` OKAY [1.828 s]，`boot` OKAY [0.237 s]，
`preflight` 两次 `slot=a unbootable=no`。账本：

| UTC | 动作 | 对象 | sha / 结果 |
| --- | --- | --- | --- |
| 09:03:00Z | flash | `fw/vendor_boot_gpu_fix3.img` | `3de636d8…c4115` / rc=0 |
| 09:03:00Z | boot | `fw/boot-17line-043f7120.img` | `043f7120…5bd3` / rc=0 |

日志 `logs/flash-fix3.log`。**只写 `vendor_boot` + UEFI RAM boot**；
`boot`/`recovery`/`userdata` 未动，未用 `fastboot reboot`。
`device_touched=true / fix3_board_verified=false`；
`board_verified=false / minus3_persists=true` 在拿到屏照前保持不变。

判读顺序即 §69.41.3：先看 `MUYU GPU calls`（≥1 才是图形栈真起来），
**再看 `R` 行**；`R=ok` + `GPU c>=1` 才翻 `board_verified`。
屏照需含 `MUYU LIVE p/t`、`GPU` 整行、`PROBE`、`B`、`F`、`R`。

## §69.43 fix4：按本机 GEN2 家族修正 hwcg 表（17:5x，离线重建完成、待上机）

用户「试一下吧」→ 核查「能否完全使用 muyu 配置」。结论：**板级层早已是 muyu**，
唯一实质缺口是 fix2 装错家族的 hwcg 表。

**本机证据（全部来自 `fw/` 收获物，非推断）**
- `board.toml [gpu] family = ADRENO_7XX_GEN2` / `gmem_bytes=0x180000` /
  `gmu_chip_id=0x70b0000`；`a7xx_catalog.c` 里 `0x70b0000` **无条目**。
- `a7xx_catalog.c`：`a730_hwcg`（53 项）只用于 GEN1（`0x07030001`、
  `0x43020100` w/ `gmu_chipid 0x07110000`、固件 `gen71700_*`）；
  **`a740_hwcg`（54 项）用于所有 GEN2**（`0x43050a01`、`0x43050c01`、
  `0x43030c00` w/ 固件 `gen71500_*`，本板 `firmware_sqe` 列表也含它）。
  ⇒ `gen-muyu-a735-hwinit.py` 生成的 `MuyuA735Hwcg[]` 是 GEN1 的 a730 表，
  不是本板家族；fix2 把它写进 `gpuHwInit()` 是**把 GEN1 表装到 GEN2 板上**。
- 三个「假设值」实为本机值，无需改：`fw/muyu_dt.txt` 的
  `qcom,gmu-freq-table` 大端解出 `[220000000,0x40,550000000,0x80]`，
  与 `gpucc-cliffs.c` `ftbl_gpu_cc_gmu_clk_src` 的 `F(220000000)`/`F(550000000)`
  一致；hub 的 `ftbl_gpu_cc_ff_clk_src` 只有 `F(200000000)`。
  GMU pool `0xC8000000/32 MiB` 是 Darwin 侧自留区，已核对 DTB reserved 无冲突。

**改动（相对 fix3 唯一变量）**：`GpuPowerProbe.cpp::gpuHwInit()` 的 hwcg 表
`MuyuA735Hwcg[]`(53) → `NativeAdrenoA740Hwcg[]`(54)，去掉
`MuyuA735HwInit.h` include，probe[3]=1 / probe[2]=54。**保留**本机 1.5 MiB GMEM
窗口、220/200 MHz、`validateSwap` hold 旁路、fix1、stage3 全部屏上 probe。
`NativeLegacyDisplay.cpp` / `BravearkLastLog.c` 逐字节未变。

| 产物 | 值 |
| --- | --- |
| `fw/vendor_boot_gpu_fix4.img` | 77,393,920 B / sha `2e524437…235b` |
| kernel / payload | `91585783…3c53` / `0f26f3c7…9e2` |
| cmdline | 与 fix3 / fix2 / stage3 逐字相同 |

`UniXNU/scripts/check-fix4-rebuild.py`（+ tree 副本）**全绿**，判据
`out/muyu/tree/build/host-turnip-input/fix4-rebuild.json`：三段白名单反向替换后
与 `baseline-pre-fix4/GpuPowerProbe.cpp.fix3` 逐字节相同；编译产物含
`NativeAdrenoA740Hwcg`、不含 `MuyuA735Hwcg`、仍含 `0x117ffff`；三个 QEMU
符号零漂移、七个对照镜像（含 fix3）未覆盖、三份构建日志零 error；
checker 另从 `a7xx_catalog.c` / `gpucc-cliffs.c` / DTB 频率表重算家族证据。

**边界**：构建轮**零刷机、零引导、零设备动作**；只读预检 `no fastboot device`，
账本末条仍 `09:03:00Z`（fix3 boot）。上机须用户明确授权。
判读：`PROBE hw=1 hn=54` ⇒ GEN2 表板上落地；`gr=117ffff` ⇒ 本机 GMEM 保留。

### §69.44 fix4 上机（2026-11-13T10:03Z）

授权：用户「连上了，不管结果如何，试一试」。首次只读预检 `no fastboot device`
（USB 侧完全不可见，`adb` / `fastboot` / `system_profiler` 三处皆空），本轮未做
任何设备动作；用户重连后第二次预检通过。

| 项 | 值 |
| --- | --- |
| 设备 | fastboot `a20248f7`，`slot=a`，`unbootable=no`，`retry=7`，电池 3602–3605 mV（工具给出 low-battery 警告） |
| 写入 | `vendor_boot` ← `fw/vendor_boot_gpu_fix4.img`，`2e524437…235b`，OKAY [1.825 s] |
| 引导 | `fw/boot-17line-043f7120.img` RAM boot，`043f7120…bd3`，OKAY [0.237 s] |
| 账本 | `10:03:29Z flash rc=0` / `10:03:30Z boot rc=0`，日志 `logs/flash-fix4.log` |
| 边界 | 只写 `vendor_boot` + UEFI RAM boot；`boot`/`recovery`/`userdata` 未动，无 `fastboot reboot` |

**待判读**（屏照需含 `MUYU LIVE p/t`、`GPU` 整行、`PROBE`、`B`、`R`）：
1. `calls>=1` 才是图形栈真起来的唯一证据；`calls=0` ⇒ 后面读数一概不作数。
2. `calls>=1` 后再看 `R`；`R=ok` + `GPU c>=1` 才翻 `board_verified`。
3. `PROBE hw=1 hn=54` ⇒ GEN2 家族表板上落地（fix4 唯一变量）；
   `gr=117ffff` ⇒ 本机 1.5 MiB GMEM 窗口保留。
4. 若 `calls>=1` 且 `-3` 回来，这是**首次真正检验到 hwcg 家族表**，不是倒退。

**诚实预期**：按 §69.41/§69.42 因果分析，本版不预测 `calls=0` 被修复；
其价值是排除最后一个代码变量、确认 `1→0` 是否启动分叉（§49.7 有同镜像
`calls=7` vs `calls=0` 前例）。`board_verified=false / minus3_persists=true` 保持。

### §69.45 板上结果：fix4 `calls` 仍为 0 ⇒ 代码变量已穷尽

用户回报：**`MUYU GPU ... calls=0` 依旧**（`-3` 未再出现，同 §69.42 的假阴性形态）。

判读：fix4 唯一变量是 hwcg 表（GEN2 `NativeAdrenoA740Hwcg` 54 项），
`GpuPowerProbe.cpp` 之外两份源码逐字节未变、cmdline 逐字未变
（`check-fix4-rebuild.py` 全绿）。结果 `calls` 仍为 0 ⇒

1. **`gpuHwInit()` 的参数不可能影响 `calls`** —— 与 §69.41 的因果分析一致
   （`calls` 属 display offer 链，`gpuHwInit()` 只由 `gpuStart()` mode>=9 与
   hang restart 调用），本轮得到板上实证而非推断；
2. 截至 fix4，**所有已改的行为变量（grant1 properties / fix1 quarantine /
   fix2-3 swap_hold / fix2+fix4 hwcg+GMEM）都无法让 `calls` 回到 >=1**；
3. `1→0` 因此**不能归因于任何一次代码改动**，指向启动分叉（§49.7 前例）或
   **设备侧累积状态**（连续非正常关机会在 userdata 上留下未提交的 APFS
   事务 / 失效 manifest，§69.36 已经因 manifest 损坏整盘刷回过一次）。
   若 userdata 挂载或首次启动失败，用户态到不了提交 swap 的那一步，
   `calls=0` 与「比 `-3` 更靠前的断点」就同时成立。

**决策（用户指令）**：下一轮做**全量重刷**，先充电。范围与时序见 §69.46。

### §69.46 全量重刷方案（待充电后执行）

设备状态：`a20248f7`，`slot=a`，`unbootable=no`，`max-download-size=805306368`，
电量 **3590 mV 且在下降**（3750→3656→3605→3590）。fastboot 自身不充电。

**备份可用性（已离线校验，只读）**：`fw/userdata-backup/userdata-root.sparse`
Android sparse 头合法（major=1, blk=4096, blocks=2290688, chunks=2237，
首块 RAW `0xCAC1`），`SHNGROOT` v1，2 个窗口
`OS@0x100000+6077580800` / `SystemOS@0x16a600000+3303014400`，
`image_bytes=9382658048`（8.74 GiB），**清单 sha256 校验通过** ⇒ 回滚路径成立。

执行顺序（用户已勾选项 ①，②待确认）：

1. ✅ `restore-stock --yes` —— 从 `fw/stock` 写回 `boot` + `vendor_boot` + `recovery`
   （`fw/stock/boot.img` / `vendor_boot.img` / `recovery.img`，均存在）；
2. ⏳ **待确认**：`root-image fw/userdata-backup/userdata-root.sparse --yes --new-installation`
   —— 整盘重写 userdata（8.74 GiB，工具内 `MUYU_FB_TIMEOUT=1200`），
   以镜像自身布局替换现有安装与末尾 Data 窗口。**这是本工程里「格式化所有数据」
   的等价动作**；`fastboot erase userdata` 不建 f2fs，工具明确不推荐；
3. ✅ `xnu fw/vendor_boot_gpu_fix4.img` —— 写 fix4 `vendor_boot`（覆盖步骤 1 的 stock 值）
   + `fw/boot-17line-043f7120.img` RAM boot。

不碰：`super` / `dtbo`（无镜像，不参与 XNU 引导）、`init_boot`（用户本轮未选；
补刷 stock 会失去 Android root）。

**风险**：步骤 2 需连续写 20 分钟以上，3590 mV 下掉电会造成 userdata 半写
（可再充好电重写，但当次无法继续）。**故本轮不执行，等充满 1–2 h。**

### §69.47 回原厂完成，recovery 已恢复（10:14Z）

用户报告「进不去 rec」。确认：**本轮先前没有执行任何写操作**，设备上仍是 fix4 的
`vendor_boot`（`2e524437…235b`）。根因即此 —— Android 13+ 的 recovery 也要靠
`vendor_boot` 里的 vendor ramdisk 起 first-stage init，而 fix4 把 72 MiB 的 XNU
ramdisk 写进了 `vendor_boot`，recovery 因此起不来。

执行 `restore-stock --yes`（日志 `logs/restore-stock-20261113-1813.log`）：

| 分区 | 镜像 | sha256 | 结果 |
| --- | --- | --- | --- |
| `boot` | `fw/stock/boot.img` | `bf68c1be…3ce8` | rc=0, 2.405 s |
| `vendor_boot` | `fw/stock/vendor_boot.img` | `69f1e38c…96fd` | rc=0, 2.395 s |
| `recovery` | `fw/stock/recovery.img` | `c40dcb96…b53e` | rc=0, 2.460 s |

随后 `fastboot reboot recovery`，25 s 后 `adb devices` 返回 **`a20248f7 recovery`**
⇒ **recovery 可用**，原厂三方全部回到 stock。账本 `10:13:58Z / 10:14:00Z / 10:14:03Z`。

**重要含义**：设备现在停在官方 recovery，正是工具注释所说的正确位置 ——
「Android still needs its own userdata: boot recovery and wipe there
(`fastboot erase userdata` does not create f2fs)」。即**真正的「格式化所有数据」
应在 recovery 里做 factory reset**（会建 f2fs），而不是 fastboot 擦除。
若走这条路，userdata 的 `SHNGROOT` 窗口（`OS` / `SystemOS`）会一并消失，
必须再用 `root-image fw/userdata-backup/userdata-root.sparse --yes --new-installation`
把 iPadOS 根卷写回，然后 `xnu fw/vendor_boot_gpu_fix4.img` 才会回到 XNU。
整条链仍是可逆的（备份已校验，§69.46）。**该破坏性最强的一步须用户明确确认。**

### §69.48 社区交叉验证 + 自我纠正：`gx` 门在 `gpuStart()` 之前

用户指出 `unixnu.com/devices/muyu` 就是本仓自己的记录，无新信息。改查同为
SM8635 的**其他**机型与官方线：

| 来源 | 关键事实 |
| --- | --- |
| `tattwamashi/UniXNU-braveark`（SM8635，同 SoC） | 「GPU: Adreno 735 (`qcom,adreno-gpu-gen7-11-0`); **the native GPU driver knows only the Adreno 740**」；状态只到「XNU reaches BSD init on all 8 CPUs / root in progress」，**无 GPU 用户态** |
| `kvfsrl/UniXNU-peridot`（SM8635） | 「work-in-progress; **No hardware result is documented**」 |
| sheng（SM8550 / A740，官方维护） | 社区索引记「已实现 Adobe 740 原生 Metal GPU 驱动和系统 UI 渲染」——**唯一有 GPU 用户态的机型** |
| 本仓 `porting.toml` GPU 区 | 「GPU work is paused (2026-11-02)」「`CFG_GDSCR`(0x9038) bit16 从不置位、parent 是 RPMh ARC voter、OOB `GPU_SET` 无 ACK」「`MuyuRpmhRsc` sender host-tested，**on-device panic readout never collected**」 |

⇒ 交叉结论：**SM8635 上没有任何人把 GPU 用户态跑起来过**；`-3` 不是本机独有的
userland bug，而是「GX 未上电 ⇒ 无 CP ⇒ 无 Metal」的下游表现。

**自我纠正（源码级，比上面更硬）**：`GpuPowerProbe.cpp:1791` 是

```cpp
if (gx && mode() >= 9) gpuStart();
```

其中 `gx = g[2] <= 100 && !(g[4] & 3)`（`g[2]`=OOB ack 轮询数，`g[4]`=GMU `kGxStatus`）。
即 **`gpuStart()` → `gpuHwInit()` 之前还有一道 `gx` 电闸**。此前 §69.39/§69.43
只说「由 `gpuStart()`(mode>=9) 调用」，漏了这一层，现更正：

- 若 GX 未上电：`gpuHwInit()` **一次都不执行** ⇒ fix2（A735 表 + 1.5 MiB）与
  fix4（GEN2 表）**在板上根本没生效**，`native_adreno_hwinit_probe[]` 保持全 0
  ⇒ 屏上 `PROBE hw=0 hn=0 gr=0`。这解释了为什么两版改参数毫无效果；
- 同时：无 GPU 设备 ⇒ display 走 CPU composition ⇒ `native_legacy_gpu_offer()`
  不被调用 ⇒ **`calls=0`**。而 fix1 那次 `calls=1 o=1` 说明那一次 **GX 是通的**。

**判据（一条就能定性，零源码改动）**：屏上第 5 行 `X` 行
`X v<vote>/<fw> o<ack polls> x<before>><after> c<GX GDSCR> [R.. H.. | gx-off]`
- `gx-off` ⇒ GX 未上电，`calls`/`-3` 全部是下游症状，整个 GPU 用户态方向在当前
  电源状态下不可达；真正的阻塞点是 RPMh ARC vote / OOB 握手（`GPU-PROBE-RUNBOOK.md`
  §12–§13，probe 17），而**设备端读数至今未采集**；
- 有 `R`/`H` 字段 ⇒ GX 已上电，此时 `PROBE hw=` 才有意义，`calls=0` 需另找原因。



⚠️ 按 §69.41/§69.42 的因果分析，这一刀**不预测 `calls=0` 的修复** ——
`calls` 在 display offer 链，`gpuHwInit()` 只由 `gpuStart()`(mode>=9) 与
hang restart 调用。本版是「把本机配置真正用对」的单变量实验，不是 `-3` 的既定解。
回滚：fix3 `3de636d8…`、fix2 `a563780a…`、fix1 `a004368b…`。

## §69.49 全清重刷：recovery factory reset + 根卷写回 + fix4（2026-11-13 18:35）

**动机**：`calls` 在 fix1 为 1、fix2/fix3/fix4 均为 0，四类代码变量（sandbox grant、
quarantine、swap_hold 消费、hwcg 家族表）全部被排除 ⇒ 剩下的候选是**设备侧累积状态**
（连续十余次非正常关机，APFS 事务 / manifest 残留）。用户决定「完全格式化再全量重刷」。

**执行顺序（全部 rc=0）**：

1. `preflight`：`slot=a unbootable=yes retry=0` → `--set-active=a` → `unbootable=no`；
2. `fastboot reboot recovery` → 用户在原厂 recovery 内**手动** factory reset（格式化 userdata）；
3. 回 fastboot（`a20248f7`，`3768 mV`）：
   `root-image fw/userdata-backup/userdata-root.sparse --yes --new-installation`
   ⇒ userdata 8.74 GiB 写回，202.015 s，`rc=0`；
   镜像 sha `4c90d7a7…19a3`（与备份逐字节一致），窗口
   `OS@0x100000+6077580800` / `SystemOS@0x16a600000+3303014400`；
4. `xnu fw/vendor_boot_gpu_fix4.img`：`vendor_boot` 写入 OKAY 1.833 s，
   UEFI `fw/boot-17line-043f7120.img` RAM boot OKAY 0.237 s。

**边界**：只写 `vendor_boot` + RAM boot；`boot`/`recovery` 未被改写（recovery 用
§69.47 的 `restore-stock` 版本）；userdata 由用户手动格式化后按备份重建；
未碰 `super`/`dtbo`/`init_boot`。

**账本**：`10:35:28Z preflight` / `10:41:06Z root-image` / `10:41:15Z boot`；
日志 `logs/root-image-after-format-20261113.log`、`logs/flash-fix4-after-format.log`。

**判读（本轮同时回答两个问题）**：
- 屏上第 5 行 `X`：`gx-off` ⇒ GX 从未上电（§69.48），格式化不可能改变它；
  有 `R`/`H` ⇒ GX 通了，`PROBE hw=1 hn=54` 才有意义；
- `MUYU GPU calls`：全清后仍 `0` ⇒ **与 userdata 累积状态无关**，是启动分叉或
  display 提交侧；回到 ≥1 ⇒ 上一轮 `1→0` 归因于设备状态，`-3` 才有机会重测。

## §69.50 GX 才是上游：`GMU_OOB_GPU_SET` 的 1 秒预算被写成了 10 ms（fix5）

**触发**：用户指出「`calls=0` 而 GX 根本没上电，谈 Metal 用户态没意义」。按此把
`GPU-VENDOR-GX-CLOCK.md`、`GPU-PROBE-RUNBOOK.md` §12–§13、`HANDOFF-2026-10-31.md`
§7–§13、`STATUS.md`、`GPU-HANDOFF.md` §17/§19/§49 全部重读，结论如下。

### 69.50.1 GX 不是未解，而是**逐次随机**

| 事实 | 出处 |
| --- | --- |
| 11-11 20:45 一次 boot：`X v0/0 o4 x3>0 cf8222800 R00000001 H0` —— GX 真上电 | §17.3 |
| 同一镜像随后 `st=0x97`（Ready）、`cp=10`、CP 环 `S 5:1` 有一次 Submit | §19 |
| 但 §49 又记「内核全绿、`calls=0`」，`STATUS.md` 把 GX 列为「not implemented」 | §49 / STATUS |
| 同一镜像两次 boot 出现 `calls=7` vs `calls=0` | §49.7 |

⇒ 「GX 有没有上电」不是代码路径问题，是**时序**问题。

### 69.50.2 根因候选：OOB ACK 预算只有厂商的 1/100

`GpuPowerProbe.cpp::gpuOn()` 用 OOB `GPU_SET` 要 GX：

```
write HOST2GMU_INTR_SET |= 1<<30
for (g[2]=1; g[2]<=100; ++g[2]) { if (GMU2HOST_INTR_INFO & (1<<31)) break; IODelay(100); }
gx = (g[2] <= 100) && !(kGxStatus & 3);      // 不成立就不会调用 gpuStart()
```

厂商 `a6xx_gmu_set_oob()`（`fw/android-harvest/msm-drv/a6xx_gmu.c:442`）：

```c
gmu_poll_timeout(gmu, REG_A6XX_GMU_GMU2HOST_INTR_INFO, val,
                 val & (1 << ack), 100, 10000);   /* 100 µs × 10000 = 1 s */
```

**我们 10 ms，厂商 1 s。** GX 上电要等 ARC 电压爬坡；10 ms 内不来 ACK 就
`g[2]=101` → `gx=false` → `gpuStart()` 整段跳过 → 没有 GPU 设备 → CoreAnimation
走 CPU 合成 → `native_legacy_gpu_offer()` 一次都不进 → **`calls=0`**，且 `PROBE st`
停在 `0x8f`（`gpuOn()` 里 `gx ? 0x87 : 0x8f`）。这与 11-11 `o4` 成功、其它 boot
失败完全一致，也解释了为什么 grant1 / fix1 / fix2 / fix3 / fix4 五刀都改不动
`calls` —— 它们全部在 `gpuStart()` **之后**。

### 69.50.3 fix5 的唯一改动

| 项 | 值 |
| --- | --- |
| 文件 | `UniXNU-muyu-17/package/GpuPowerProbe.cpp`，`gpuOn()` |
| 改动 | `g[2] <= 100` → `g[2] <= 10000`；`gx` 判据同步 |
| 保留 | hwcg GEN2 表、1.5 MiB GMEM、`swap_hold`、fix1 quarantine、grant1 名单、stage3 探针 |
| 镜像 | `fw/vendor_boot_gpu_fix5.img`，77,393,920 B，sha **`656ac055…950f`** |
| kernel / payload | `048b2707…02d4` / `90eb5e61…cd62` |
| cmdline | 与 fix4 / fix3 / fix2 / stage3 **逐字相同**（996 B 有效，闸 1000 B） |

校验 `UniXNU/scripts/check-fix5-rebuild.py`（+ tree 副本）全绿，判据
`build/host-turnip-input/fix5-rebuild.json`：两段白名单反向替换后与 fix4 逐字节
相同；另两份源码逐字节未变；`GpuPowerProbe.o` 仍含 GEN2 表、`0x117ffff`、且反汇编
出现 `0x2710`（10000）；三个 QEMU 符号零漂移；八个对照镜像未覆盖；三份日志零 error。

### 69.50.4 上机判据（改的是等待，不是寄存器）

- `X v0/0 o<小数值> x3>0 c… R… H…` ⇒ 预算足够，GX 上电 → `PROBE st=97`；
  此时 `calls` 应回 ≥1，`-3` 才重新成为可测对象；
- `X v0/0 o10001 x.. c…` ⇒ 1 s 内 GMU 也不 ACK，那 **不是预算问题**，转
  `HANDOFF-2026-10-31.md` §8.5/§9 的 ARC 电平与 `mx/mxc` 资源结论；
- `X v<非0>/…` ⇒ HFI `0x1e` 投票就失败了，OOB 根本没发，先看 `v`。

**诚实预期**：这一刀让「GX 该起的时候能起来」，不保证 GMU 一定 ACK。若
`o10001`，说明 GX 的阻塞在 GMU 固件侧，下一步回到 §13 的 HFI 语义/ARC 电平。

## §69.51 fix64→fix68：10BIT 表面全零的定性，以及「10-bit 帧从不 offer」

### 69.51.1 已坐实的分界线

| 版本 | 10BIT 结果 | 显示线程 | GPU 侧 | 结论 |
| --- | --- | --- | --- | --- |
| fix64 | `nz=0` ×448，稳定 448 swaps | 正常 | fence 推进、`e00002d5` 消失 | 剩余症状收敛为表面全零 |
| fix65 | `10BIT=0 SWAP=0 IBNZ=2` | 楔死 | CP 已 drain | 全平面 `dc ivac` + uncached 大面积转换太慢 |
| fix66 | `S1/S2` @134.366 s，`S3` 到 390 s 未出现，`SWAP=0` | 楔死 | `SYNC/SB/HC` 跑到 390 s | 8342 次 uncached 读 + `dc ivac` 同样楔死 |
| fix67 | `S1–S4` 各 6，`10BIT` 30，`SWAP` 4，`IBNZ` 170 | 正常 | 153.585 s 出现 `HC hang n=1` | 回归解除，拿到定性数据 |

**共同项排除法**：fix64 只有 cached census（448 帧正常）；fix65/fix66 唯一的
共同新增是对 34 MiB `IOMemoryDescriptor` 的 `kIOMapInhibitCache` 别名做 bulk
访问。⇒ 非缓存别名不是可用的批量读路径，不要再往这个方向加扫描。

### 69.51.2 cache 一致性假设被证伪

fix67 判据（`logs/vendor_boot_gpu_fix67-logs/muyu-674f19c7`）：

```
MUYU 10BIT S2 n=1 raw=1 dram=1 len=34177024
MUYU 10BIT S3 n=1 nzu=0 cb=00000000 ca=00000000 uh=00000000,00000000,00000000,00000000
MUYU 10BIT S4 n=1 nz=0 first=0 heads=00000000,00000000,00000000 alpha0=00
MUYU 10BIT 3200x2136 ... nz=0 nzu=0 cb=00000000 ca=00000000 ... n=1 .. n=128
```

`raw=1 dram=1` 证明 uncached 映射有效且长度正确；单次 `dc civac` 前后首 word
都是零（`cb=ca=0`），16 个 uncached 采样全零（`nzu=0`），alpha 平面首字节也是
`00`。⇒ **不是 CPU 持有 zero-fill stale cache line 而 GPU 写到了 DRAM**。这块
`'b3a8'` 表面的 DRAM 从未被任何生产者写入非零像素。后续不要再修显示侧 cache
维护，要查生产者 / 提交路径。

### 69.51.3 10-bit 分支绕过了 GPU hand-off

`NativeLegacyDisplay.cpp` 的 `tenBit` 分支由 fix34 插在 `gpuBypass` 判断**之前**，
无条件做 CPU 转换 + `finishTransfer`，从不调用 `native_legacy_gpu_offer()`。
而 `GpuScanoutClient::serveOne()`（`UniXNU/drivers/AdrenoMetal/GpuScanoutClient.h:75`）
只在 `NGP_METHOD_WAIT_SWAP` 上等帧，拿到后 `Presenter::present()` 把帧 blit /
compute 到 firmware scanout（`GpuScanoutPresenter.h:55`），**自己不产生像素**。

对应日志：fix64/fix66 的 `MUYU OFFER` 计数都是 **0**；fix67 在
`HC hang n=1` → `NativeAdreno recovery fence=170 status=e00002ca` → backboardd
重启改提交 BGRA 表面之后，才出现 `MUYU OFFER n=1 acc=1 pres=1 tc=183`，且仍无
`completeSwap`。这与此前板上 `o=1 c=0` 的历史状态同源，也符合「GPU 曾接受首帧，
只是仍然失败」的判断——不要再说 GPU 从未接手首帧。

### 69.51.4 fix68 与判据

`'b3a8'` plane0 每像素一个 32-bit word、stride `width*4`，与 `NGP_BGRX8888`
的 pitch 一致，`Presenter::present()` 的边界检查能通过（`frame.bytes=34177024`
≥ `stride*(height-1)+row=27340800`）。因此在 10BIT 分支按 BGRA 分支原样接入
`native_legacy_gpu_offer(this,memory,width*4,width,height)` +
`native_legacy_swap_stage(1)` / `gpuRequest` / `retireSwap` / `return`，
CPU 转换保留为 offer 被拒时的兜底，新增 `MUYU 10BIT HANDOFF n=`。

上机判据：`OFFER`/`HANDOFF` 计数、`o>=1 c>=1`（completeSwap 是否终于发生）、
`GFXFAIL m=6 st=e00002d6`、`HC hang`、`10BIT nz`。若 offer 成功但 surface 仍全零，
GPU copy 只会把零搬到 scanout——那就说明必须继续追「backboardd 为什么没把像素
写进这个 surface」，而不是继续在显示侧打转。

## §69.52 fix68 板上结果：GPU 接手首帧、完成一次 completeSwap，然后冻结

### 69.52.1 `m=28674` 不是「无关的外部 selector」

`28674 = 0x7002 = NativeAdrenoHandoffBase(0x7000) + NGP_METHOD_WAIT_SWAP(2)`
（`NativeAdrenoPublish.cpp:76`，`GPU-ONDEVICE.md:999`）。`GFXFAIL m=28674
st=e00002d6 pid=58` 就是 **backboardd 在 `waitSwap()` 上超时**；`m=6` 是
`NativeAdrenoMethodWaitFence`。两个 timeout 同时存在，且 pid 都是 58：同一个
进程既提交 10-bit 表面，又作为 GPU scanout 客户端等 display offer 帧。
这把 §69.51.3 的「互等」从源码推断升级成板上事实，也再次印证「GPU 曾接受
首帧，只是仍然失败」，不能说 GPU 从未接手。

### 69.52.2 fix68 时间线（`logs/vendor_boot_gpu_fix68-logs/muyu-674bc1ec`）

```
135.611  OFFER n=1 acc=0 pres=1 tc=0    → 10BIT n=1 CPU 转换, SWAP n=1
136.545  OFFER n=2 acc=0 pres=1 tc=1    → 10BIT n=2 CPU 转换, SWAP n=2
136.919  OFFER n=3 acc=1 pres=1 tc=2    → 10BIT HANDOFF n=3
137.417  SWAP n=3 st=0                   ← finishGpuSwap(0): completeSwap 成功
137.467  10BIT S1 n=4
138.024  10BIT HANDOFF n=4               ← 第二帧也交给 GPU
138.299  CPROW r129/129 (CP drain)
139.183  GFXFAIL m=28674 timeout         ← waitSwap 又开始超时
171.887  HC tick=240 hangs=0 restarts=0
```

- ✅ **GPU 第一次接手并完成 `completeSwap`**（`SWAP n=3` 来自
  `finishGpuSwap(0)`，不是 `transfer` 的 CPU 完成）；fix64 以来第一次 `acc=1`。
- ❌ **第二帧接手后再无 `SWAP n=4`**，10BIT 分支总共只进来 4 次就冻结；
  `transferred` 一直没清 ⇒ 回到历史 `o>=1 c=0` 的「首帧后冻结」形态。
- `HC hang=0`（fix67 有 1 次），说明不是 GPU 硬挂；`CPROW r==w` 说明 CP 一直
  drain；`sub=ret=15`、`d=f,0,2` 说明 driver 认为所有 job 都 retire、fence word
  已写到 15。
- `10BIT nz=0 / nzu=0 / cb=ca=0` 在 fix68 依然成立 ⇒ surface 仍全零。

### 69.52.3 收敛到 fence 语义

`fencePending(fence)` = `fence<=submitted && retired<fence`；`sub=ret=15` 时
对任何 `fence<=15` 都为假 → `runCore` 立即 dispatch 一次零超时 WaitFence →
core 的 `NativeAdrenoSubmit::wait()` 对无活跃 job 的 fence 走
`observeFence(fence)`；若 `fence > fence word(15)` 就返回 timeout。
`runCore` 再被 `g.clock.idle()` 提前 break，于是「等」其实只 poll 一次。
⇒ 客户端很可能在等一个 **超出当前 timeline 的 fence**。`MUYU W wf=…` 只上
BravearkLastLog 屏照、不进 msgbuf，所以 fix69 把 `NativeAdrenoAbiWaitFence`
的两个 payload word 与 `wf_fence/wf_submitted/wf_retired` 直接打进
`GFXFAIL` 行（纯观测、零行为改动），保留 fix68 的 handoff。

判据：`req=<fence>/<timeout_ns>` 与 `sub/ret` 的大小关系。若 `req > sub` ⇒
fence 编号/Turnip timeline 错配，下一步修 `wait()` 对「未来 fence」的语义；
若 `req <= sub` 而 `wf` 仍 timeout ⇒ 是 `observeFence()` / fence 内存可见性，
回到 ring fence 路径。

## §69.53 fix69：`GFXFAIL` 长期误判更正，以及 b3a8 平面行距候选

### 69.53.1 `GFXFAIL m=6` 主要是正常的非阻塞 poll，不是失败链

fix69 镜像 `4b536362…0504`，日志
`logs/vendor_boot_gpu_fix69-logs/muyu-67448004`：

```
MUYU GFXFAIL m=6 ... sub=7  ret=6  d=6,0,2 req=7/0 wf=7/7/6
MUYU GFXFAIL m=6 ... sub=8  ret=7  d=7,0,2 req=8/0 wf=8/8/7
...
MUYU GFXFAIL m=6 ... sub=16 ret=14 d=e,0,2 req=f/0 wf=15/16/14
```

`req=<fence>/<timeout_ns>`：**fence 恒等于 `sub`，`timeout_ns` 恒为 0**；
`d[0]`（ring fence word）与 `ret` 恒为 `sub-1`。出处是
`tu_knl_darwin.cc:734`「All fence polls are nonblocking」与 `:756`
`dw_wait_fence_raw(dev, bo->fence, 0)` —— 这是 Turnip 在 BO retire 缓存里
做的**零超时非阻塞 poll**，`VK_TIMEOUT` 就是「还没完成」的正常应答。
`m=28674 (0x7002 WAIT_SWAP)` 的 timeout 同样被
`GpuScanoutClient::serveOne():77` 当作 `++timeouts; return true`。
⇒ **`GFXFAIL` 是 fix36 加的「所有非零 tunnel 返回」诊断日志，不等于故障**。
历史把它当核心失败链是误判；不要再为消除 `e00002d6` 改 driver。
`HC hang=0`（fix68/fix69）也支持这一点。

### 69.53.2 剩余唯一硬事实

`10BIT nz=0 / nzu=0 / cb=ca=0 / alpha0=00` 在所有版本成立，census 覆盖
`packed->getLength()=34177024`（plane0 27340800 + plane1 6835200），
**两个平面 DRAM 都全零**。fix67 已排除 CPU cache 一致性；fix68 已让 GPU
接手并完成过一次 `completeSwap`，仍全零。⇒ 卡点在生产者：谁应该把像素
写进这块 `'b3a8'` 表面。

### 69.53.3 具体候选（与「A735 ≠ A732」直接相关）

`UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm:1273-1289` 的
`b3a8Planes()` 注释写明：这些端口上 `b3a8` 的
`IOSurfaceGetBytesPerRowOfPlane` 返回 0 时，纹理曾在 **step 3 被拒、图层
直接不绘制**（"its layer left undrawn"），修法就是按 64 字节对齐推算
`row0=w*4, row1=w`。但它第 1279 行有硬前提：

```objc
if(IOSurfaceGetBytesPerRowOfPlane(surface,0) &&
   IOSurfaceGetBytesPerRowOfPlane(surface,1)) return false;
```

而本板 fix33 实测 `w=3200 h=2136 fmt='b3a8' planes=2 stride=3200
alloc=34177024`，且 `NativeLegacyDisplay.cpp:553` 明确记着
「`'b3a8' reports its plane stride in **pixels** rather than bytes」。
若本板 plane row 报的是 3200（非 0 的像素数），`b3a8Planes()` 会直接
返回 false，纹理 `rowBytes` 就变成 3200 而不是 12800，图层不绘制 ——
与「两平面全零」吻合，也正好是 uke/A732（plane row = 0）与 muyu/A735
**相似但不相同**的地方。

**验证/修复路径**：`gSurfaceRefusals / gSurfacePlanar / gSurfaceB3a8`
已由 `native_bridge_metal_formats_locked():4643` 汇成 `tex x… s…/… p… b…`
遥测行，但 `GLOG`=NSLog 不进 kernel msgbuf（fix68/fix69 日志 `tex x` 计数 0）。
下一轮要么把该行经 SetMetadata 送进 msgbuf，要么放宽 1279 行（当
`row < w*4` 即判定为像素单位、仍用推算值）后重编
`NativeAdrenoMetal.bundle` 并重建 `fw/gpuaddon-17-full.bin`
（需同步 owner trust cache；脚本 `UniXNU/scripts/build-adreno-metal-plugin.py`、
`make-gpu-addon.py` 已在树内）。

### 69.53.4 不要回头的方向

- 不要再对 34 MiB `kIOMapInhibitCache` 别名做 bulk 读 / 全平面 `dc ivac`
  （fix65/fix66 两次楔死显示线程）；
- 不要再改显示侧 cache 维护（fix67 已证伪）；
- 不要再为 `GFXFAIL e00002d6` 改 fence/wait（fix69 已定性为正常 poll）；
- `fsr=400 far=0` 仍是无真实 latched fault，别当 SMMU 故障。

## §69.54 fix70：统一 Metal 前端与显示侧的 `'b3a8'` 两平面布局（A735 专属）

### 69.54.1 为什么改用户态 addon

`NativeLegacyDisplay.cpp:299` 的 `memory=native_legacy_surface_descriptor(surface)`
直接来自 swap request 里的 `IOSurface` —— **生产者和消费者是同一块内存**，
不是两块 buffer 之间缺一次 copy。所以 §69.53.2 的「两平面 DRAM 全零」只能由
生产者侧解释。本板 fix33 实测 `w=3200 h=2136 fmt='b3a8' planes=2
stride=3200 alloc=34177024`，而显示侧按 `row0=w*4=12800 B`、
`plane1@row0*h=27340800`、`row1=w=3200 B` 切分，总计 34176000 B。
`b3a8Planes()` 的推算公式与此**逐字节吻合**，但旧代码有两道门把它挡住：

```objc
// 1279: 两个 plane 都报了非零 row 就直接放弃推算
if(IOSurfaceGetBytesPerRowOfPlane(surface,0) &&
   IOSurfaceGetBytesPerRowOfPlane(surface,1)) return false;
// 4187 / 1290: 只在 row == 0（uke/A732 的情形）时才调用推算
if(!row && plane<2 && b3a8Planes(...)) { ... }
```

A735 的 plane row 报的是**像素数 3200**，既不是 0 也不是 12800：于是纹理
拿到 `rowBytes=3200`，图层不往显示侧读的那块布局里画。这正是用户反复强调的
「A735 与 A732 相似但不相同」。

### 69.54.2 三处改动（`UniXNU/drivers/AdrenoMetal/NativeBridgeMetalFrontend.mm`）

1. `b3a8Planes()` 去掉 1279 的双非零硬前提，**总是**按 w/h 推算规范布局；
   GLOG 行追加 `reported %lu/%lu`，把本板真实报告值带出来。
2. `textureOnSurface()` 的 `chromaOffset/chromaRow` 无条件用推算值。
3. `newTextureWithDescriptor:iosurface:plane:` 在 `plane<2` 时无条件调用
   `b3a8Planes()`，只要 `start/row` 与推算不符就覆盖。

格式/plane 数/allocation 校验仍在，非 `'b3a8'` 表面一律 `return false`，行为不变。

### 69.54.3 重建链（第一次重编用户态 addon，务必照抄）

`--gpu-addon` 携带的 `NativeAdrenoMetal.bundle` 是 ad-hoc 签名的，改它必须同步
改 owner trust cache，否则 AMFI 拒绝加载：

```bash
# 1. 重编 plugin + daemon；产物在 UniXNU/build/adreno-metal-plugin/{bundle,trust.json,adrenoprobed}
python3 UniXNU/scripts/build-adreno-metal-plugin.py --no-replay
# 2. 用新 trust.json 的 3 个 CDHash 替换 owner-trust-17-full.bundle 最后一个
#    (owner) module 的 3 条 hash，保持两个 Apple module 字节不变（已验证可逐字节复现基线）
# 3. 重新打包，--publish 不能丢（基线 addon 含 addons/NativeAdrenoMetal.publish）
python3 UniXNU/scripts/make-gpu-addon.py --publish fw/gpuaddon-17-fix70.bin
# 4. 三步构建时把两个输入换掉
... --owner-trust-cache fw/trust-21H461/owner-trust-17-fix70.bundle \
    --gpu-addon fw/gpuaddon-17-fix70.bin --kernel-arg sheng-tum=s
```

**坑**：`build-adreno-metal-plugin.py` 硬依赖 `glslc`/`spirv-val`（不在 Xcode 工具链里），
且开头会 `shutil.rmtree(build/adreno-metal-plugin)`。本机缺 glslc 时，可临时把
`gen-turnip-smoke-shaders.py` 那步改成「`build/turnip-smoke/NativeTurnipShaders.h`
已存在就跳过」（GLSL 源码未变时产物等价），跑完恢复脚本原样；重编前务必先备份
`build/adreno-metal-plugin/`。`--no-replay` 必需：基线 plugin 不含 `TurnipReplay.h`，
且 `build/turnip-darwin-host/plugin_smoke` 不存在。

**校验**：重编后 `trust.json` 里 `turnip_cdhash` 应保持不变（Turnip dylib 内容未改、
ad-hoc 签名确定性），只有 plugin / daemon 两个 hash 变；新 addon 必须仍是 10 个文件、
含 publish，且 `strings` 能查到 `native-metal: b3a8 ... layout reported`。

### 69.54.4 本轮产物与判据

`fw/vendor_boot_gpu_fix70.img`（cmdline 1011 B、含 `sheng-tum=s`），
plugin CDHash `1434f0c3…` / daemon `c539e065…`，trust bundle sha
`911602cb…`，addon `fw/gpuaddon-17-fix70.bin`。

判据只看 `MUYU 10BIT S4 ... nz=`：
- `nz>0` ⇒ 布局统一后生产者开始写像素，首帧有内容，继续看 `SWAP` / 桌面；
- `nz=0` ⇒ 生产者根本没写（或 Metal plugin 没被加载进 backboardd，fix69 的
  `MUYU METAL M ... o0/0/0 n0` 值得复查），下一步应把
  `native_bridge_metal_formats_locked()` 的 `tex x… s…/… p… b…` 经
  `NativeAdrenoMethodSetMetadata` 送进 msgbuf，而不是继续猜布局。

### 69.54.5 fix70 板上结果（`logs/vendor_boot_gpu_fix70-logs/muyu-67326d33`）

413 s 自复位、日志完整到 173 s。新 addon / trust **确实生效**：
`attempting to load 3 external trust cache modules` → 三个 module 全部 loaded，
`isCdhashInTrustCache ... Returning IOReturn 0x0`，`payload.json` 的
`trust_cache_sha256=911602cb…` / `gpu_addon_sha256=2b19eb95…` 都是新产物，
`MUYU METAL M a5.0/5.0`（addon mount stage 5）。没有 AMFI 拒绝记录。

```
135.234  MUYU 10BIT S1 n=1 ... S4 nz=0 ... OFFER n=1 acc=0 pres=1 ... SWAP n=1 st=0
135.990  MUYU 10BIT S1 n=2 ... S4 nz=0 ... OFFER n=2 acc=0 ... SWAP n=2 st=0
136.231  MUYU 10BIT S1 n=3 ... S4 nz=0 ... OFFER n=3 acc=0 ... SWAP n=3 st=0
136.480  MUYU 10BIT S1 n=4 ... S4 nz=0
136.524  MUYU 10BIT HANDOFF n=4
136.534+ GPU 仍持续提交到 142 s (sub→17+, CPROW r==w drains)，但显示路径再无 10BIT
```

`nz=0 / nzu=0 / cb=ca=0 / uh=0,…` 一直全零 ⇒ **统一纹理 rowBytes 没让生产者写进
surface**。相对 fix68 的改善只有 `GFXFAIL m=28674`（WAIT_SWAP）从有变 0，
以及 `OFFER acc` 从 1 变 0（用户态行为确实被新 addon 改变了，但没转成像素）。

**新发现的结构性阻塞**：10BIT 分支里 fix68 的 hand-off 成功时直接
`transferred=true; return`，而 `NativeLegacyDisplay.cpp:262` 的
`if(enabled!=1 || transferred || ...) return` 会拒绝之后每一帧，直到
`finishGpuSwap()` 清掉它。scanout 客户端对两平面 `'b3a8'` 表面不回
`completeSwap`，于是**显示路径在第 4 帧永久冻结**（fix64/fix67 的纯 CPU 路径
能跑 448+ 帧）。这个冻结同时也掩盖了 fix70 的布局修复在长帧流下是否有效。

### 69.54.6 fix71：回退 hand-off + 打出 surface 真实元数据

`UniXNU-muyu-17/drivers/GopFramebuffer/NativeLegacyDisplay.cpp` 两处：

1. 删除 fix68 的 10BIT `native_legacy_gpu_offer(...)` 块，恢复 fix64/fix67 已验证的
   CPU 转换 + `finishTransfer` 帧流（GPU hand-off 等 surface 真的有像素、
   且 swap 完成链搞清楚之后再加回来）；
2. `MUYU 10BIT S4` 追加 `stride=%llu alloc=%llu fmt=%08x planes=%u`，直接打印
   `native_legacy_surface_stride/allocation/format/planes` 的**板上真实值** ——
   这一条能一锤定音验证 §69.53.3 的「A735 把 plane stride 报成像素数 3200」假设。

addon / trust / cmdline 全部沿用 fix70 不变，所以 fix71 是干净的单变量实验。
判据：
- `stride=3200` ⇒ 假设成立，布局已统一却仍 `nz=0`，下一步必须查 Metal 前端纹理
  是否被拒（把 `tex x… s…/… p… b…` 经 SetMetadata 送进 msgbuf）；
- `stride=12800` ⇒ 假设证伪，回查 `b3a8Planes()` 的 `allocation` 门槛和纹理拒绝 step；
- `nz>0` ⇒ 布局修复有效，首帧有内容，继续接 GPU hand-off。

## §69.55 fix71：A735 的 plane stride 假设坐实；fix72 把 Metal 前端遥测送进 msgbuf

### 69.55.1 fix71 板上结果（`logs/vendor_boot_gpu_fix71-logs/muyu-674eae47`）

202 s 自复位，日志到 173 s。回退 hand-off 后帧流**恢复正常**
（`S4` 打到 `n=128+`、`IBNZ=169`、`HANDOFF=0`），但：

```
MUYU 10BIT S4 n=1   nz=0 ... stride=3200 alloc=34177024 fmt=62336138 planes=2
MUYU 10BIT S4 n=128 nz=0 ... stride=3200 alloc=34177024 fmt=62336138 planes=2
```

⇒ **`stride=3200`（像素数）与 §69.53.3 的推测逐字吻合**，A735 与 uke/A732
（plane row = 0）确实不同；`alloc=34177024 = 3200*2136*5 + 对齐`，也印证
显示侧「plane0 = 4 B/px、plane1 = 1 B/px」的切分是对的。
**但即使 fix70 已把 Metal 纹理 rowBytes 统一成 12800，且帧流稳定跑了 128+ 帧，
两个平面 DRAM 仍一个非零像素都没有。** 布局统一不足以让生产者写入。

### 69.55.2 sandbox 不是当前阻塞点（勿再回头）

日志里有 `Sandbox: backboardd(58) deny(1) iokit-open-user-client
AGXDeviceUserClient`，但同一时刻：

```
NSG gpu svc=AGXAccelerator class=AGXAccelerator proc=backboardd
  len17 list=1 verdict=1 text=backboardd,iconservicesagent,...
```

`native_sandbox_grant()` 的 `verdict=1` 说明补丁覆盖了 `mac_iokit_check_open_service`；
133–136 s 的 `MUYU OPEN type=1 / 16711 / 16712` 都**没有**跟 `MUYU OPEN refused`，
即 backboardd 的 GPU user client 打开成功（只有 156 s HC hang 后重启的
backboardd(227) 出现 `refused e00002d5`，是后段 `mount()` 失败）。
另外 `MUYU METAL M ... o0/0/0 n0` 全部是 53/55/57/**116 s** 的快照，
早于 132 s 的 open，**不能**当成全程「plugin 从未连接」的证据。

### 69.55.3 fix72：让 Metal 前端自己说话

`native-metal:` / `tex x…` 是 `GLOG=NSLog`，从不进 kernel msgbuf。现成的
userspace→kernel 文本通道是 `NativeAdrenoUserTest.h:187 streamText(Gpu&,text)`：
它把一行拆成 `0x7fff` + 每字符 `0x70cc` + `0x7000` 的 progress marker，
由 `NativeAdrenoPublish.cpp:1073-1102` 拼进 `report2`。三处改动：

1. `NativeBridgeMetalFrontend.mm`：`gB3a8Reported0/1/gB3a8Calls` 三个 atomic
   记录本板 surface 真正报上来的两个 plane row；`native_bridge_metal_formats_locked()`
   的格式串加 `r%llu/%llu c%llu`。
2. `NativeAdrenoMetalPlugin.mm::deviceForPort()`：include `NativeBridgeMetalFrontend.h`，
   起一个 detached 线程，每 3 s 调 `native_bridge_metal_formats()`，**只在计数变化时**
   用 `streamText(*gpu, "F ...")` 发一行（上限 24 行，不刷屏）。
3. `NativeAdrenoPublish.cpp`：`value==0x7000`（一行完成）时把 `F ` 开头的行
   `IOLog("MUYU USTEXT %s\n", line)`（上限 40）。

判据（终于可以直接看到用户态真相）：
- `s=0 b=0 c=0` ⇒ Metal 前端根本没被调用（plugin 没加载/没走到纹理创建）；
- `x>0` 或 `s<第二个数>` ⇒ 纹理被拒，看 `refuseSurface` 的 step；
- `c>0 r=3200/...` ⇒ 前端确实看到 A735 的像素行距，且 fix70 的布局覆盖已执行，
  但仍 `nz=0` ⇒ 渲染目标不是这块 IOSurface，转查 import/VkImage 绑定；
- `nz>0` ⇒ 首帧终于有像素。

### 69.55.4 构建链维护

`UniXNU/scripts/build-adreno-metal-plugin.py` 的 shader 生成改为「`NativeTurnipShaders.h`
比所有 `smoke_*` GLSL 新就复用」，使离线重建不再硬依赖本机没有的 `glslc`/`spirv-val`；
GLSL 一旦改动仍会照旧重新生成，不会用到陈旧产物。
`/tmp/fix70-baseline/rebuild-trust.py` 现在接受输出路径参数，便于每轮生成新的
owner trust bundle。每轮重编 addon 必须同步：新 `trust.json` → owner module 的
3 个 CDHash → `make-gpu-addon.py --publish` → 三步构建换两个输入。

## §69.56 fix72 板上结果：遥测线程没起来（attr 已销毁）；fix73 修正

fix72 镜像 `1a0e86c8…`，414 s 自复位，日志窗口到 **388 s**（比前几轮长一倍），
`logs/vendor_boot_gpu_fix72-logs/muyu-67498753`。帧流稳定到 `S4 n=192`，
`stride=3200` 全程一致、`nz=0` 全程一致，但 **`MUYU USTEXT` 0 行**、
`F tex` / `PUB no device` / `NativeAdreno client` 也都是 0。

不是通道问题：`GFXFAIL m=28674`（WAIT_SWAP）出现 1 次，说明
`publish::deviceForPort()` 里的 `bridgephase::serveScanout()` 线程确实起来了，
`streamText()` 这条 progress-marker 文本流本身也是历史一直在用的。
真正原因是**我把 telemetry 线程插在 `pthread_attr_destroy(&attributes)` 之后**
（`NativeAdrenoMetalPlugin.mm` 的 primer 线程之后就销毁了 attr），
`pthread_create` 拿到已销毁的 attr 返回 `EINVAL`，线程根本没创建。

**fix73**：给 telemetry 用独立的 `pthread_attr_t`（init + DETACHED + destroy），
并在 `gpu->mark(0x6803)` 之前**同步发一行**，这样即便线程再出问题也至少能看到
初始的 `tex x0 s0/0 p0 b0 r0/0 c0`。kernel / Frontend 不动，只重编 plugin
并照 §69.55.4 走一遍 trust + addon + 三步构建。

## §69.57 fix73：**A735 平面行距假设被证伪**，纹理全部创建成功

fix73 镜像 `71212741…`，197 s 自复位，日志
`logs/vendor_boot_gpu_fix73-logs/muyu-6731a5f1`。`MUYU USTEXT` 终于出现：

```
MUYU USTEXT F tex x0 s0/0 p0 b0 r0/0 c0 f
MUYU USTEXT F tex x0 s3/0 p3 b6 r12800/3200 c6 f115 550 554 80 43 23 25 55
MUYU USTEXT F tex x0 s4/0 p4 b8 r12800/3200 c8 f115 550 554 80 43 23 25 55
```

字段（`NativeBridgeMetalFrontend.mm::native_bridge_metal_formats_locked()`）：
`x`=纹理拒绝数、`s`=surface 纹理数/表面拒绝数、`p`=planar、`b`=b3a8 布局调用、
`r`=**IOSurface 实际报告的两个 plane row**、`c`=布局调用次数、`f`=见过的像素格式。

### 69.57.1 结论（推翻 §69.53.3 / §69.54 的主假设）

- **`r12800/3200`**：本板 `IOSurfaceGetBytesPerRowOfPlane` 报的本来就是**正确的
  字节行距**（12800 B / 3200 B），与显示侧 `NativeLegacyDisplay.cpp` 的
  `width*4` / `width` 切分逐字节一致，也与 `b3a8Planes()` 的推算一致。
- kernel 侧 `native_legacy_surface_stride(surface)=3200` 是**另一个字段**
  （IOMFB swap request 里按像素计的 stride），不能拿来推断 IOSurface 的
  plane row。§69.53.3「A735 把 plane row 报成像素数」的推论是错的。
- ⇒ **fix70 的布局统一实际是 no-op**（`wantRow==row`、`wantStart==start`，
  从未触发覆盖）。uke/A732 与 muyu/A735 在这个点上没有差异。
- `x0`、`s4/0`：4 个 surface 纹理全部创建成功、**零拒绝**，`b3a8Planes()`
  被调用 8 次。Metal 前端确实在跑。
- 但 `10BIT nz=0` 全程不变 ⇒ **问题不在纹理创建/导入/布局，而在渲染目标
  是否绑定、或 command buffer 是否提交/完成**。

### 69.57.2 fix74：把提交与 draw 遥测接进同一通道

`native_bridge_metal_status()`（`Frontend.mm:4653`）给出
`c<commits>/<failures> d<completed> p<pipelines> x<problems> R<passes>
D<draws> F<fullscreen> M<biggestKiB> C<dispatches> B<blits>`，前 63 字符
正好覆盖全部计数字段（streamText 每行上限 63 字符）。改动：

1. `NativeAdrenoMetalPlugin.mm`：同步与周期线程各发**两行** ——
   `F <formats>` 与 `S <status>`，两个字符串任一变化才重发（上限 30 轮）。
2. `NativeAdrenoPublish.cpp`：`0x7000` 行完成时把 `F ` / `S ` 开头都打进
   `MUYU USTEXT`（上限 64 行）。

判据：
- `c0/0` ⇒ 渲染服务器从未 commit command buffer，卡在 Metal 层之上
  （回查 `NativeAdrenoMetalDevice` 发现路径与 drawable 获取）；
- `c>0 D>0 B0` ⇒ 有 draw 但没 blit 到 display surface，渲染目标没绑定；
- `c>0 B>0` 而 `nz=0` ⇒ 有 blit 但 GPU 写没落到这块内存（查 IOSurface
  import 的 BO/IOVA 绑定与 §56 的 WC 一致性）；
- `d<completed>` 远小于 `c` ⇒ 提交后 fence 没完成，回到 ring/CP 侧。

## §69.58 fix74：**渲染链完全活跃**；fix75 复验整块 DRAM

fix74 镜像 `0d0a0738…`，203 s 自复位，日志
`logs/vendor_boot_gpu_fix74-logs/muyu-674655b0`。`MUYU USTEXT` 六行：

```
132.780  F tex x0 s0/0 p0 b0 r0/0 c0 f
132.790  S c0/0 d0 p0 x6 R0 D0 F0 M0 C0 B0 native-metal: no device answe...
135.806  F tex x0 s3/0 p3 b6 r12800/3200 c6 f115 550 554 80 43 23 25 55
135.806  S c2/0 d1 p27 x7 R21 D5 F3 M6675 C0 B5 native-metal: binary arc...
138.812  F tex x0 s4/0 p4 b8 r12800/3200 c8 f...
138.812  S c25/0 d24 p29 x7 R43 D73 F25 M6675 C0 B5 native-metal: binary...
```

### 69.58.1 结论：Metal 真的在画

`c25/0`（25 次 commit、0 失败）、`d24`（24 个完成）、`R43`（43 个 render
pass）、`D73`（73 次 draw）、`F25`（25 次全屏 pass，最大 6675 KiB）、
`B5`（5 次 blit）、`p29`（29 条 pipeline）。**金属渲染链、命令提交与完成
全部是活的**，desktop/wallpaper 级别的全屏绘制确实发生了。
`x7` 的 problem 文本早期是 `native-metal: no device answered`（132.79 s，
一次早期 device 创建失败，随后 135.8 s 已成功）。

但 `10BIT S4 ... nz=0` 全程不变、`10BIT S3 nzu=0`。⇒ 像素被画到了
**别处**，或者画到了这块物理内存但 kernel 侧的 view 看不到。

### 69.58.2 重新打开的 cache 分叉

fix67 当年用 `dc civac` 只重读 plane0 的**首行**，uncached 采样也只有
**前 64 KiB 的 16 个点**；而 fix66 证明 8342 次 uncached 读 / 全平面
`dc ivac` 会楔死显示线程，所以一直没做过覆盖整块 34 MiB 的稀疏 DRAM 复验。
现在既然确认有 25 次 commit + 73 次 draw，这个复验的价值变了：

**fix75**：`NativeLegacyDisplay.cpp` 的 `S3` 把 16 个连续 uncached 采样改成
**64 个均匀分布、覆盖整个 `raw->getLength()`** 的采样点（64 次读不会楔死）。
其余不变，addon/trust 沿用 fix74。

判据：
- `nzu>0` ⇒ GPU 确实写了 DRAM，只是 CPU cached view 持有 zero-fill 陈旧行
  ⇒ 修 cache maintenance（避开 fix65/66 的批量操作，考虑分段 + `dc civac`
  或让转换走 uncached 但分批）；
- `nzu=0` ⇒ 这块物理内存真没被写，转查 IOSurface → VkImage/BO 的 import
  绑定：Metal 前端 import 的 `IOSurface` 与 display 侧 `IOMemoryDescriptor`
  是否指向同一组物理页，以及 render target 是否是另一个中间纹理（`B5`
  的 blit 目标）。

## §69.59 fix75：**cache 一致性彻底排除**，物理页从未被写

fix75 镜像 `a3909d3b…`，168 s 自复位，日志
`logs/vendor_boot_gpu_fix75-logs/muyu-6741f15d`。64 个均匀分布在整块
34,177,024 B 上的 `kIOMapInhibitCache` 采样点，每一帧（n=1..4）都是：

```
MUYU 10BIT S3 n=1 nzu=0 cb=00000000 ca=00000000 uh=00000000,00000000,00000000,00000000
MUYU 10BIT S4 n=1 nz=0 ... stride=3200 alloc=34177024 fmt=62336138 planes=2
```

同一时刻用户态：
```
MUYU USTEXT S c41/0 d38 p29 x7 R59 D120 F41 M6675 C0 B5
```

即 **41 次 command buffer commit、38 个完成、59 个 render pass、120 次 draw、
41 次全屏 pass、5 次 blit**，渲染比 fix74 更活跃，但这块 surface 的
cached / `dc civac` 后 / uncached 三视图**全部为零**。

### 69.59.1 本轮全部排除项（勿再回头）

| 假设 | 结论 | 依据 |
| --- | --- | --- |
| CPU stale cache line | **排除** | fix67 + fix75 全块 64 点 uncached + `dc civac` 重读全零 |
| GPU/CP/fence 冻结 | **排除** | fix64 起 `CPROW r==w`、fence 递增、resync=0、`HC hang` 偶尔但可恢复 |
| `GFXFAIL e00002d6` 是故障 | **排除** | fix69：Turnip 零超时非阻塞 poll，`VK_TIMEOUT` 是正常应答 |
| sandbox 拒绝 GPU user client | **排除** | `NSG gpu verdict=1`，133–136 s 的 open 无 refused |
| A735 平面行距是像素数 | **证伪** | fix73 `r12800/3200`，IOSurface 报的是正确字节行距 |
| 纹理被拒 / 布局错 | **排除** | fix73 `x0 s4/0`，零拒绝、`b3a8Planes()` 与 surface 报告一致 |
| Metal 未渲染 / 未提交 | **排除** | fix74/fix75 `c41/0 d38 D120 R59 B5` |
| `fsr=400` 是 SMMU fault | **排除** | 历史结论，FSC=0 无 latched fault |

### 69.59.2 唯一剩下的根因方向

**Metal 前端画到了别处**：display 侧 `native_legacy_surface_descriptor(surface)`
拿到的那组物理页，从头到尾没有任何非零 word；而用户态确实完成了 41 次
commit / 120 次 draw。下一步只查两件事：

1. **import 绑定**：`NativeBridgeMetalFrontend.mm::surfaceBuffer()` /
   `textureOnSurface()` 把 `IOSurface` 的 `IOSurfaceGetBaseAddress()` pages
   import 成 `NBBuffer` → Turnip `VkImage` 时，用的物理页是否就是 display
   swap request 里 `IOMemoryDescriptor` 的那组页。可在内核侧打印
   `memory->getPhysicalSegment(0,…)`，在用户态打印 import 的 base address /
   IOVA 做比对（`MUYU SB` 已有 `base/ciova`，可复用）。
2. **render target / blit 目标**：`s4/0` 只有 4 个 surface 纹理，而全屏 pass
   有 41 次 —— 大部分绘制用的是普通纹理。查 `B5` 这 5 次 blit 的目标是不是
   display surface；若不是，则是 `CAMetalLayer` drawable 与 IOMFB swap
   surface 之间的桥接缺失。

建议下一轮把 `native_bridge_metal_telemetry()` 的 `DF`/`V` 行（draw 目标
统计）或直接在 `blit` 编码处记一条「目标是否 IOSurface 纹理」送进
`MUYU USTEXT`，一次定性。

## §69.60 fix76：**「Metal 画到别处」被证伪**；根因收敛到 import 页绑定

镜像 `79ae4917…`，203 s 自复位，日志
`logs/vendor_boot_gpu_fix76-logs/muyu-674a30a1`（msgbuf 到 175 s，332794 B，
`S4` 跑到 n=64 帧流正常）。addon `d3d33c4e…`、trust `09f3ffde…`、
kernel `988f2e06…`，cmdline 1011 B。

### 69.60.1 四处改动（全部纯观测，零行为变化）

1. `NativeBridgeMetalFrontend.mm`：`SurfaceNote` 环记录每个 import 的
   IOSurface（id / w×h / 格式 / plane 数 / allocation / 用户态 base VA，最多 8 个）；
   新增 `gSurfacePasses`/`gPlainPasses`（render pass 的 color0 是否 IOSurface
   纹理）、`gSurfaceBlitTargets`/`gBlitTargets`、`gDrawableTextures`；导出
   `native_bridge_metal_surface_line()` 与 `native_bridge_metal_target_line()`。
2. `NativeAdrenoMetalPlugin.mm`：同步与周期各发 4 行 `F` / `S` / `U` / `D`。
3. `NativeAdrenoPublish.cpp`：USTEXT 前缀放宽到 `F|S|U|D`，上限 160 行
   （4 行 × (1 次同步 + 30 次变化) = 124）。
4. `NativeLegacyDisplay.cpp`：10BIT 新增 `S5`，打印 swap surface 的
   `IOMemoryDescriptor::getPhysicalSegment(0,&segLen)`、surface 对象指针与长度。

> 小瑕疵：frontend 返回的行自带 `U `/`D ` 前缀，plugin 又加了 `"U %s"` /
> `"D %s"`，所以日志里是 `U U n2 …` / `D D P…`。不影响判读，下一轮顺手去掉。

### 69.60.2 决定性遥测

```
138.464  U U n2 i2 w3200x2136 f62336138 p2 a34177024 b10800000
138.464  D D P8/18 B0/16 W0
162.775  D D P90/18 B0/16 W0
136.817  MUYU 10BIT S5 n=1 phys=0000000937d62000 seg=4096 surf=b8b18428 len=34177024
137.749  MUYU 10BIT S5 n=2 phys=000000094ca8c000 seg=4096 surf=b8b187e0 len=34177024
144.538  MUYU 10BIT S4 n=64 nz=0 ... stride=3200 alloc=34177024 fmt=62336138 planes=2
162.742  S c90/0 d65 p29 x7 R108 D268 F90 M6675 C0 B5
```

### 69.60.3 结论：对象链完整，缺的是物理页

1. **Q1「是不是同一块 surface」= 是。** Metal import 的最大 IOSurface 是
   `3200x2136`、`'b3a8'`、2 planes、`34,177,024` B，与 IOMFB swap surface
   **逐字段一致**；`n2` 与 `S5` 的两个交替 surface 指针
   （`b8b18428` / `b8b187e0`，双缓冲）数量吻合。用户态 base VA `0x10800000`。
2. **Q2「render target 是不是 IOSurface」= 是。** `P90/18`：90 个 render pass
   的 color0 是 IOSurface 纹理，只有 18 个不是。`W0` ⇒ 本板根本不用
   `isDrawable` 纹理，走的是直接 IOSurface 纹理，不是 CAMetalLayer drawable。
   `B0/16` ⇒ 16 次 blit 目标都不是 IOSurface 纹理，但 blit 不是主路径。
3. ⇒ §69.59.2 的「Metal 前端画到了别处」**证伪**。对象层面链路完整：
   IOSurface import → 纹理 → render target → 90 次 commit / 65 次完成 →
   fence，且 `nz=0` 持续到 n=64 帧。
4. **唯一剩下的解释**：Turnip 把 `IOSurfaceGetBaseAddress()` 的用户态页
   import 成 GPU BO 时，GPU 实际写的物理页不是 IOMFB `IOMemoryDescriptor`
   描述的那组页。`S5` 显示它是**散页 descriptor**（首段仅 4096 B），首段 PA
   在两个 buffer 间交替 `0x937d62000` / `0x94ca8c000`；若 GPU BO 是新分配的
   零页或另一份 WC BO，CPU 侧 surface 就永远全零，与全部观测吻合。
5. 辅助证据：`MUYU SYNC ... wcz=370/144722/0/0` ⇒ KMD 累计分配 370 个
   WC BO、144722 个 4 KiB 页（≈566 MiB），维护失败 0；`MUYU SB` 246 条
   最大 `bbytes=0x80000`（512 KiB），**submit 的 IB 列表里没有任何 34 MiB
   级 BO**（IB 主要是 16 KiB command buffer，纹理 BO 是否走同一列表待查）。

### 69.60.4 排除项更新（叠加在 §69.59.1 之上）

| 假设 | 结论 | 依据 |
| --- | --- | --- |
| Metal 画到另一个尺寸/格式的 surface | **排除** | fix76 `U n2 … w3200x2136 f62336138 p2 a34177024` 与 swap 逐字段一致 |
| render pass 没绑定 IOSurface 纹理 | **排除** | `P90/18` |
| CAMetalLayer drawable 桥接缺失 | **排除** | `W0`，本板不用 drawable |
| 像素必须靠 blit 送到 display | **排除** | `B0/16`，主路径是 render pass |

### 69.60.5 fix77 只查一件事

**import 的物理页绑定**：`nmb_buffer_new_no_copy_releasing(gDevice, base,
bytes, …)` → Turnip Darwin KMD 的 BO import/allocate 路径。在 KMD 侧打印这个
BO 的 `getPhysicalSegment(0,…)`、长度与 IOVA，与 `S5` 的
`phys=0x937d62000` / `0x94ca8c000` 比对；同时确认是否曾创建
`bytes≈0x2094000`（34 MiB）的 BO。若物理页不同（或 BO 是新零页），即为根因，
修 import 而非继续查渲染链。

### 69.60.6 import 链路（离线调查，fix77 埋点依据）

```
surfaceBuffer()                     NativeBridgeMetalFrontend.mm:1022
  nmb_buffer_new_no_copy_releasing  bridge/NativeMetalObjects.h:95
                                    NativeMetalObjects.rs:776 / :875 (no_copy)
  new_buffer_with_bytes_no_copy → Memory::import_host
  vkAllocateMemory + VkImportMemoryHostPointerInfoEXT
                                    patches/mesa/turnip-darwin-kmd.patch:135-144
  tu_bo_init_host → darwin_bo_init_host
                                    patches/mesa/turnip-darwin-kmd.patch:1153-1194
  dw_call(NativeAdrenoMethodBoImport=16)
                                    NativeAdrenoABI.h:31 / NativeAdrenoPublish.cpp:917
  importBo(task=current_task())     NativeAdrenoUserClient.cpp:294-323
  NativeAdrenoMemory::importTask()  NativeAdrenoMemory.cpp:254-307
```

`importTask` **不分配新页**：`IOMemoryDescriptor::withAddressRange(address,
bytes, kIODirectionInOut|kIOMemoryPersistent, task)` + `prepare()` 把调用进程的
用户态页 wire 住，逐页 `getPhysicalSegment()` 填 `spans_[]`（`:290-298`），
GPU 的 SMMU PTE 映射的就是这些页（`NativeAdrenoPublish.cpp:356-357`）。

另分配零页的是 `BoAlloc` / `allocateSystem()`
（`NativeAdrenoMemory.cpp:126-159, 197-237`）：
`IOBufferMemoryDescriptor::inTaskWithOptions(nullptr,…)` 拿全新 VM 零页，并计入
`native_adreno_wc_zero_fill[]` —— 这解释了 fix76 的 `wcz=370/144722`，那些全是
系统 BO，不是 surface import。

**两个关键分支**：
- `importTask():272-280`：写 pin 返回 `KERN_PROTECTION_FAILURE` 时改用
  `kIODirectionOut` 并置 `read_only_=true`；`mapBo():349` 随之剥掉写权限
  （`permissions&~2u`）⇒ GPU 永远写不进 surface，与「commit/draw 全活但
  cached/`dc civac`/uncached 三视图全零」高度吻合。
- `mapBo():353` 的 `coherent` 只在 imported && `!(permissions&2)` 时为真；
  可写的 import 被映射成非 coherent（`writeback=request->writeback=0`）。

fix77 只改 `NativeAdrenoUserClient.cpp`（复用 fix76 的 addon/trust，不动签名链），
在 `importBo` / `mapBo` / `allocBoInner` / import 失败分支加
`MUYU IMP` / `MUYU IMPM` / `MUYU BOA` / `MUYU IMPF`，打印首物理页、bytes、
IOVA、`ro`、`perm`、`coh`、`pid`，与 `MUYU 10BIT S5 phys=` 直接比对。判据：

1. `IMP phys` == S5 的 `0x937d62000` / `0x94ca8c000` 且 `ro=0 perm=3`
   ⇒ 页与权限都对，回查渲染/缓存/提交；
2. `IMP ro=1` 或 `IMPM perm=1` ⇒ **只读回退即根因**；
3. 只有 `BOA bytes≈0x2098000 sys=1 wc=1` 而无同级 `IMP`
   ⇒ 纹理是新分配的零页 WC BO；
4. 只有 `IMPF` ⇒ import 全失败，前端 U 行不可能有记录（与 fix76 矛盾，需复查）。

> `MUYU VIEW` 只覆盖前 24 条且 fix76 全部是 `sys=1`，看不到 import BO，
> 因此必须新增上述埋点，不能靠既有日志。

## §69.61 fix77：**import 物理页与 IOMFB 页完全相同**；只读回退 / 新零页 BO 均排除

镜像 `1d4e6593…`（复用 fix76 的 addon/trust，只改 KMD），414 s 自复位，
日志 `logs/vendor_boot_gpu_fix77-logs/muyu-673701ac`（到 173 s，362597 B，
`S4`/`S5` 跑到 n=192，帧流稳定）。

### 69.61.1 改动（只有 `UniXNU/drivers/Adreno740/NativeAdrenoUserClient.cpp`）

- `importBo()` 成功 / 失败加 `MUYU IMP` / `MUYU IMPF`：handle、用户 VA、bytes、
  首物理页 `spans[0].physical`、段长、span 数、`readOnly()`、`ownerUncached()`、pid。
- `mapBo()` 加 `MUYU IMPM`：IOVA、bytes、首物理页、`ro`、`perm`、`coherent`。
- `allocBoInner()` 加 `MUYU BOA`：只对 ≥ 32 MiB 的系统 BO 打印
  bytes / sys / wc / spans / 首物理页。
全部限流（32 条），纯观测，不改行为；因此不需要重编 plugin / trust / addon。

### 69.61.2 数据

```
138.395 MUYU IMP  h=3c va=110800000 bytes=2098000 phys=000000094382f000 seg=4096 spans=6776 ro=0 unc=1 pid=58
139.088 MUYU IMP  h=4d va=120057000 bytes=2098000 phys=000000095c07c000 seg=4096 spans=6531 ro=0 unc=1 pid=58
        MUYU IMPM h=3c iova=0000000101d34000 bytes=2098000 phys=000000094382f000 ro=0 perm=3 coh=0
        MUYU IMPM h=4d iova=0000000110cce000 bytes=2098000 phys=000000095c07c000 ro=0 perm=3 coh=0
139.063 MUYU 10BIT S5 n=1   phys=000000094382f000 seg=4096 surf=a4966428 len=34177024
139.878 MUYU 10BIT S5 n=2   phys=000000095c07c000 seg=4096 surf=a49667e0 len=34177024
158.372 MUYU 10BIT S5 n=192 phys=000000095c07c000 ...
158.358 MUYU 10BIT S4 n=192 nz=0 ...
158.877 MUYU USTEXT D D P197/18 B0/16 W0
```

`MUYU IMPF` **0 条**；`MUYU BOA` 10 条全部是 `0x2800000` / `0x3494000` /
`0x3800000`（40 / 55 / 56 MiB 系统 WC BO），**没有 `0x2098000` 的 34 MiB 系统 BO**。
`MUYU SYNC ... tc=2055 bv=369`（累积剥离的 THREAD_CONTROL / BV-BR 握手包）。

### 69.61.3 结论

1. 两个 `bytes=0x2098000`（34,177,024 B）的 import，首物理页
   `0x94382f000` / `0x95c07c000`，与**同轮** `S5` 的两个 swap surface 首物理页
   **逐字相同**（双缓冲交替）。`spans=6776 / 6531` 是连续页合并后的段数。
2. `ro=0`、`IMPM perm=3`（读写）、`coh=0`、`IMPF=0` ⇒ import 成功、GPU 有写
   权限、**没有**走 §69.60.6 的只读回退分支。
3. 没有 34 MiB 系统 BO ⇒ 纹理不是新分配的零页 WC BO。
4. ⇒ **§69.59.2 / §69.60.5 的「import 物理页绑定错误」整条主线排除**。
   物理页、权限、对象链、提交/完成全部正确，而 `nz=0` 持续到 n=192 帧，
   同期 `P197/18`（197 个 render pass 的 color0 是 IOSurface 纹理）。
5. 剩下唯一解释：**GPU 从未把像素 store 到这块 sysmem**。最可能是
   GMEM/binning pass 的 resolve 没有执行 —— `bv=369` 说明 fix61 起
   `stripThreadSync()` 把全部 THREAD_CONTROL 与 BV/BR 握手包改写成 NOP；
   `mapBo()` 的注释也记载「sysmem pass 用 write-back inner-shareable 映射时
   GPU 会停在 end-of-pass cache clean（BR/BV idle at the clean events）」。

### 69.61.4 排除项再更新

| 假设 | 结论 | 依据 |
| --- | --- | --- |
| import BO 与 swap descriptor 是不同物理页 | **排除** | fix77 `IMP phys` == `S5 phys` |
| 只读回退导致 GPU 无写权限 | **排除** | `ro=0 perm=3 IMPF=0` |
| 表面纹理走 BoAlloc 新零页 | **排除** | 无 `BOA bytes=0x2098000` |

### 69.61.5 fix78

把 Turnip **已有**的 `tu_render_mode_stats` / `tu_gmem_side_stats` 累计计数
（telemetry 的 `RM` / `GM` 行）经新的 `R ` 行送进 `MUYU USTEXT`：
`g<GMEM> a<sys-auto> s<sys-other> e<exact> t<3D store> u<2D store>`。
判据：
- `g>0 && t=u=0` ⇒ 像素从未离开 tile memory，修 resolve / strip 策略；
- `g=0 && a/s>0` ⇒ 全是 SYSMEM，另查 store 目标；
- `t>0` ⇒ 有 store，查 store 写入的 BO 是不是 import BO。

顺手修掉 fix76 的 `U U` / `D D` 双前缀（frontend 返回值不再自带前缀）。

## §69.62 fix78：**Turnip render-mode 计数全零**，但原因未定；fix79 补符号判据

镜像 `d260bc2d…`（addon `1ee84d03…`、trust `a1d781b8…`），197 s 自复位，
日志 `logs/vendor_boot_gpu_fix78-logs/muyu-674aa5e3`（到 173 s，318434 B）。
USTEXT 前缀扩展到 `F/S/U/D/R`，`U`/`D` 双前缀已修好。

### 69.62.1 数据

```
MUYU USTEXT R g0 a0 s0 e0 t0 u0        (×5，全程全零)
MUYU USTEXT U n2 i2 w3200x2136 f62336138 p2 a34177024 b0f800000
MUYU USTEXT D P116/18 B0/16 W0
MUYU USTEXT S c116/0 d115 p30 x7 R134 D347 F116 M6675 C0 B5
135.091 MUYU IMP  h=3c ... bytes=2098000 phys=0000000923827000 ro=0 unc=1
        MUYU IMPM h=3c iova=0000000101d34000 ... phys=0000000923827000 ro=0 perm=3 coh=0
135.668 MUYU 10BIT S5 n=1 phys=0000000923827000 ... len=34177024
142.815 MUYU 10BIT S4 n=64 nz=0 ...
```

同轮再次复现 fix77：`IMP phys` == `S5 phys`（`0x923827000` /
`0x938ee5000`）、`ro=0 perm=3`。`MUYU IBNZ nz=166 of=220`（IB 非空，
含 `70d58003` / `48b2c085` 等 PM4 包），`MUYU SYNC ... tc=1097 bv=195`。
`MUYU SMMUFAULT fsr=400 far=0` 16 条 —— 仍是 §69.59.1 已排除的
无 latched fault（FAR=0），不是写失败。

### 69.62.2 全零行不能直接判读

本机 `nm -gU` 确认 addon 的 `libvulkan_freedreno.dylib` **确实导出**
`_tu_render_mode_stats` / `_tu_gmem_side_stats` / `_tu_pass_ring_read`
（`T` 符号）。`tu_note_render_mode()` 在 Turnip 的 render-pass 收尾处累加
（`patches/mesa/turnip-render-mode-for-attachment-memory.patch:110-145`）。
所以 `R` 全零有两种互斥解释，**fix78 无法区分**：

1. `dlopen(gLoaderPath, RTLD_NOLOAD)` 没匹配到已加载的 Turnip image
   ⇒ 两个 dlsym 都是 nullptr，行只是零初始化数组（观测失效）；
2. 符号拿到了，但 Turnip 的 render-pass 收尾路径**从未执行**
   ⇒ GPU 命令流里没有真正的 render pass，与「116 次 commit / 347 次 draw
   却 surface 全零」高度吻合，指向 metal2vulkan 运行时的编码/提交链。

### 69.62.3 fix79（一次补齐两个未知）

- `R` 行追加 `x<turnip?> m<modeStats?> d<sideStats?>`：直接区分上面两种解释。
- 新增 `E` 行：统计**目标为 IOSurface 纹理的 color attachment 0** 的
  `loadAction` / `storeAction` 分布
  （`n<passes> l<DontCare/Load/Clear> s<DontCare/Store/Resolve/StoreAndResolve>`）。
  若大量 `store=DontCare`，Turnip 会合法地把像素留在 tile memory 不写回
  display surface —— 这是比 GMEM resolve 更靠上游的解释。
- `attachment()` 里 `out.store=a.storeAction` 直接透传（`:3012`），
  前端没有强制 Store；`E` 行能一次定性。
- USTEXT 前缀再加 `E`，上限 240 行。

判据：
- `x=0` ⇒ 观测失效，换用 `tu_pass_ring_read` 或 KMD 侧包统计；
- `x=1 m=1` 且 `g/a/s=0` ⇒ Turnip 端没有 render pass，查运行时编码链；
- `E ... s<0>/<1>/...` 中 DontCare 占多数 ⇒ 强制 `MTLStoreActionStore`
  即可让 GPU 写回 display surface（下一步的行为修复）。

## §69.63 fix79：storeAction 全是 Store；R 行全零（当时误判为「Turnip 端没有 render pass」）

> ⚠️ **本节 §69.63.2–§69.63.5 的核心判读已于 fix80 作废，保留原文仅供追溯。**
> fix78/79 的 `R g0 a0 s0 e0 t0 u0 x1 m1 d1` **不是**「Turnip 没有执行 render pass」，
> 而是**诊断开关从未打开造成的观测假象**：Turnip 的 `tu_cmd_render()` 里
> `tu_note_render_mode()` 被 `tu_pass_trace_on()`（`getenv("SHENG_DIAG")`）gate
> （`patches/mesa/turnip-gmem-mutable-swap.patch:520`）；plugin 只在 boot arg
> `sheng-diag=1` 或 `sheng-gpu-trace=1` 时才 `setenv("SHENG_DIAG","1",1)`
> （`NativeAdrenoMetalPlugin.mm:251`，fix80 前）。而 muyu-17 的 cmdline 已是
> **1011 B / 上限 1016 B**，这两个参数从未进入过 cmdline，所以
> `tu_render_mode_counts[8]` 从头到尾没有写入者，`x1 m1 d1` 只证明 dlsym 成功。
> 同理 `c171/0 d171` 是前端 ObjC 自己的计数：`submit()` 在 `encodeFailure` 或
> `nmb_command_buffer_commit()` 被拒时仍会 `self.submitted=YES` 并 dispatch
> 一个伪造的 `finish`（`NativeBridgeMetalFrontend.mm:3250-3252`），
> 因此「171 次 commit / 171 次完成」也不能证明 runtime 真的提交过。
> 详见 §69.64。

镜像 `cec26d6b…`，413 s 自复位，日志
`logs/vendor_boot_gpu_fix79-logs/muyu-674253d1`（到 173 s，343765 B）。
addon/trust 为 fix79 全链重建，cmdline 1011 B。

### 69.63.1 数据

```
MUYU USTEXT R g0 a0 s0 e0 t0 u0 x1 m1 d1        (×8，全程)
MUYU USTEXT E n171 l0/169/2 s0/171/0/0
MUYU USTEXT D P171/18 B0/16 W0
MUYU USTEXT S c171/0 d171 p29 x106 R189 D511 F171 M6675 C0 B5
MUYU 10BIT S4 n=128 nz=0 ... stride=3200 alloc=34177024 fmt=62336138 planes=2
134.717 MUYU IMP  h=3c va=110000000 bytes=2098000 phys=000000092e188000 ro=0 unc=1
135.574 MUYU 10BIT S5 n=1 phys=000000092e188000 surf=4eba0428 len=34177024
135.590 MUYU IMP  h=4e va=11ecbe000 bytes=2098000 phys=0000000948530000 ro=0 unc=1
136.774 MUYU 10BIT S5 n=2 phys=0000000948530000 surf=4eba07e0 len=34177024
```

### 69.63.2 判读

1. **`x1 m1 d1`**：`dlopen(gLoaderPath, RTLD_NOLOAD)` 命中 Turnip image，
   `tu_render_mode_stats` 与 `tu_gmem_side_stats` 都 dlsym 成功。
   ⇒ fix78 的「观测失效」解释**排除**，`R` 的全零是真实计数。
2. **`E ... s0/171/0/0`**：171 个以 IOSurface 纹理为 color0 的 render pass
   **全部要求 `MTLStoreActionStore`**（DontCare 索引 0 = 0，Store 索引 1 = 171）。
   `l0/169/2` = 169 次 `Load`、2 次 `Clear`。
   ⇒ 「storeAction=DontCare 让像素留在 tile memory」假设**排除**，
   强制 Store 的行为修复无意义，不要做。
3. **`R g0 a0 s0 e0 t0 u0`**：Turnip 的 `tu_note_render_mode()`
   （GMEM / SYSMEM / GMEM store 的累加点）**一次都没被调用**。
   即没有任何 render pass 走到 Turnip 的 render-pass 收尾路径。
4. 同轮再次复现：`IMP phys` == `S5 phys`（`0x92e188000` / `0x948530000`）、
   `ro=0 perm=3`、`S4 nz=0` 到 n=128。

### 69.63.3 结论（本阶段收敛点）

在 fix66–fix79 逐项排除 CPU cache、CP/fence、GFXFAIL 语义、sandbox、
A735 平面行距、纹理拒绝/布局、Metal 未渲染、SMMU fault、import 物理页、
只读回退、新零页 BO、storeAction 之后，剩下的唯一方向是：

> **Metal 前端编码了 171 个 render pass（171 次 commit / 171 次完成 /
> 511 次 draw），但这些 pass 从未进入 Turnip 的 render-pass 执行路径**，
> 因此 GPU 从未向 display surface 写入任何一个非零 word。

`c171/0 d171` 只证明 fence 被触发，`MUYU IBNZ nz=166/220` 只证明 IB
非空（可以是缓存维护 / 同步 / marker 包），都不等价于 draw 被执行。
`MUYU SYNC tc/bv` 持续增长也只说明 IB 被 strip 过。

### 69.63.4 排除项总表（fix66–fix79）

| 假设 | 结论 | 依据 |
| --- | --- | --- |
| CPU stale cache line | 排除 | fix67 + fix75 全块 64 点 uncached + `dc civac` |
| GPU/CP/fence 冻结、GFXFAIL 是故障 | 排除 | fix64/69 |
| sandbox 拒绝 GPU user client | 排除 | `NSG gpu verdict=1` |
| A735 plane row 是像素数、纹理被拒/布局错 | 排除 | fix73 `x0 s4/0 r12800/3200` |
| Metal 未渲染 / 未提交 | 排除 | fix74–fix79 `c171/0 d171 D511` |
| `fsr=400` 是写失败 | 排除 | FAR=0，无 latched fault |
| Metal 画到另一个 surface | 排除 | fix76 `U n2 … 3200x2136/b3a8/34177024` |
| render target 不是 IOSurface / drawable 桥接缺失 | 排除 | fix76 `P171/18 W0` |
| import BO 物理页 ≠ IOMFB 页 | 排除 | fix77/78/79 `IMP phys == S5 phys` |
| 只读回退 / 新零页 WC BO | 排除 | `ro=0 perm=3 IMPF=0`、无 34 MiB BOA |
| `storeAction=DontCare` | 排除 | fix79 `s0/171/0/0` |
| **Turnip 执行了 render pass** | **否** | fix79 `R … x1 m1 d1` 但 `g/a/s/e/t/u` 全 0 |

### 69.63.5 fix80 只查编码链，不再查 GPU/内存

1. **runtime 编码队列**：`NativeMetalObjects.rs` 的 `ENCODE` 异步队列、
   `nmb_render_encoder_new` / `nmb_encoder_end` / draw 命令是否真的 flush
   并进入 `vkQueueSubmit`；可用现成的 `nmb_stage_ring_read`（type 12）与
   `tu_darwin_submit_ring_read`（type 8）frame-trace 记录做计数。
2. **Turnip image 唯一性**：确认 `gLoaderPath` 的 image 就是实际执行
   render pass 的那个 `libvulkan_freedreno.dylib`（若进程里有两个副本，
   `R` 全零会是假象；可在 dlsym 后打印 `dladdr` 的 `dli_fbase`）。
3. **KMD 侧直接证明**：统计 CP IB 中 `CP_DRAW_*` / `CP_SET_DRAW_STATE` /
   `CP_EVENT_WRITE` 等 draw/render-pass 包的数量。若为 0，坐实命令流里
   没有 draw；若 >0，则问题只在 Turnip 的 pass 记录路径。
4. **plugin 线程**：`NativeAdrenoMetalPlugin.mm` 的 primer / tester /
   编码线程启动条件（fix72 曾因 `pthread_attr_destroy` 导致 telemetry 线程
   完全没起来，同一类问题可能落在编码线程上）。

## §69.64 fix80：**打开 SHENG_DIAG + 全 IB PM4 包普查 + 真实 commit 计数**

### 69.64.1 方向修正（离线调查，无需刷机）

对 §69.63.5 的第 1/3/4 项做只读代码调查后，先否掉了 fix79 的结论本身：

- metal2vulkan 的 `ENCODE` 队列在 `NativeBridgeMetalFrontend.mm:2016-2098`
  （`EncodeThread` / `encodeLaterRaw` / `encodeFlush`），**不在** `NativeMetalObjects.rs`
  —— §69.63.5 记错了位置。Rust runtime 在
  `UniXNU/.cache/gpu-reference/metal2vulkan/crates/metal2vulkan-runtime/src/`，
  是「延迟录制」：encoder 调用只生成 segments，真正的 `vkCmdBeginRenderPass` /
  `vkCmdDraw*` / `vkQueueSubmit` 在 commit 之后的 scheduler host step 里
  （`scheduler/mod.rs:1092`、`command_buffer/group.rs:501`）。
- **决定性发现**：Turnip 的 `tu_note_render_mode()`（唯一写
  `tu_render_mode_counts[8]` 的地方）在 `tu_cmd_render()` 中被
  `tu_pass_trace_on()` gate，后者只读 `getenv("SHENG_DIAG")`
  （`turnip-gmem-mutable-swap.patch:347-355, 520`）。runtime 的
  `stage_trace::mark` 同样被 `SHENG_DIAG` gate（`0024` patch）。
  plugin 原代码 `NativeAdrenoMetalPlugin.mm:251` 只在
  `sheng-diag=1` / `sheng-gpu-trace=1` 时 setenv，而 fix76–fix79 的 cmdline
  **两个参数都没有**（1011 B / 1016 B 预算）。
  ⇒ **`R` 全零只是统计从未累加**；fix78/79「Turnip 端一个 render pass 都没有」
  是错的，不能据此把根因收敛到编码链。
- 另一个假象：前端 `submit()` 的 `encodeFailure` / commit 被拒路径
  （`:3229`、`:3248-3252`）会伪造 `finish` 完成回调，而 `gCommits`(`:3203`) /
  `gCompleted`(`:3307`) 照常增长，所以 `S c171/0 d171` **不是** runtime
  真实提交次数。

### 69.64.2 fix80 的三组改动（观测，不改行为）

1. **开诊断而不动 cmdline**：`NativeAdrenoMetalPlugin.mm` 改为无条件
   `setenv("SHENG_DIAG","1",1)`。cmdline 保持 1011 B 原样，避免触碰 1016 B 上限。
2. **`Q` 遥测行**（`MUYU USTEXT Q ...`）：前端新增
   `native_bridge_metal_submit_line()`，输出
   `h<diag?> q<commit ok>/<attempts> r<fake-completed> k<parked> p<produced> c<consumed> f<flushed>`；
   计数埋在 `encodeLaterRaw` / worker `batch->run()` / `encodeFlush` /
   `submit()` 的真实 commit 分支与伪造完成分支。plugin 同步块 + 周期线程各发一行，
   `NativeAdrenoPublish.cpp` 的 USTEXT 白名单加 `Q`、上限 240→320。
3. **`MUYU PKTS` 全 IB PM4 普查**（KMD 侧，完全独立、不受 SHENG_DIAG 影响）：
   `NativeAdrenoVm::scanIbPackets()` 在 `stripThreadSync()` **之前**只读按包边界遍历
   整个客户端 IB（type7 `1+count`、type4 `1+count`，带 opcode/count 奇校验），
   累计 8 个桶 + 128 项原始 opcode 直方图；`NativeAdrenoSubmit.cpp` 每 16 个
   buffer 限流打印一行。分桶以 pinned `adreno_pm4.xml` 为准：
   draw `{0x22,0x24,0x28,0x29,0x2a,0x38,0x7f}`、event `0x46`、
   set-draw-state `0x43`、indirect `{0x3f,0x37,0x57}`、blit `0x2c`。
   `MUYU IBNZ` 只数前 256 个 word 的非零数；fix71 的样例 `70d58003` 是
   `CP_SET_AMBLE`(0x55)、`48b2c085` 是 type4 REG 写（reg `0x0b2c0`, cnt 5），
   `70438003` 才是 `CP_SET_DRAW_STATE`(0x43)。

### 69.64.3 构建

镜像 `fw/vendor_boot_gpu_fix80.img` sha `bf7fa405…`，addon `50587ea4…`、
trust `b25da734…`，cmdline 1011 B 且五个必需参数齐全，镜像内含
`MUYU PKTS` / `MUYU USTEXT` / `MUYU 10BIT S5`，plugin bundle 含
`h%d q%llu/%llu ...` 与 `SHENG_DIAG`。

### 69.64.4 判据

| 观察 | 结论 |
| --- | --- |
| `Q h=0` | 开关仍未生效（plugin 的 setenv 在诊断缓存之后），需再查调用时序 |
| `Q h=1` 且 `R g/a/s>0` | **fix79 结论正式作废**：render pass 一直在跑；回到 GMEM store / 写回路径排查 |
| `Q h=1` 且 `R` 仍全 0 | 真没有 render pass 进入 `tu_cmd_render()`，查 runtime scheduler / image 唯一性 |
| `Q q<171` 或 `r>0` | 断点在 `encodeFailure` / `nmb_command_buffer_commit` 被拒，查 `lastError()` |
| `Q p≈c` 但 `q=0` | ENCODE 跑了但从未 commit，与 Turnip 零 pass 自洽 |
| `MUYU PKTS draw>0` | CP 确实收到 draw 包，排除「命令流里没有 draw」 |
| `MUYU PKTS draw=0, op43>0` | 发了 render-pass 状态但没发 draw |
| `MUYU PKTS bad` 很大 | walker 解析失效，本行作废 |

### 69.64.5 fix80 判读（413 s 自复位）

日志 `logs/vendor_boot_gpu_fix80-logs/muyu-6732da31`，msgbuf 303590 B
到 378 s；**GPU/USTEXT 活动窗口只有 133.0–143.0 s**（28 行 USTEXT）。

```
MUYU USTEXT Q h1 q68/68 r0 k0 p1714 c1690 f161
MUYU USTEXT R g0 a0 s77 e0 t0 u0 x1 m1 d1
MUYU USTEXT S c69/0 d66 p30 x7 R87 D206 F69 M6675 C0 B5
MUYU USTEXT D P69/18 B0/16 W0     E n69 l0/67/2 s0/69/0/0
MUYU PKTS n=128 draw=0 ev=2092 sds=184 ind=136 blit=9300 oth=3049
           t4=90251 bad=1 op22=0 op38=0 op2a=0 op43=184 op46=2092 op3f=136 op55=468
MUYU 10BIT S4 n=64 nz=0 ...   IMP phys=0x922c01000/0x93ad7e000 == S5 phys, ro=0, IMPF=0
```

1. **`h1`**：plugin 的 `setenv("SHENG_DIAG","1",1)` 生效，诊断不再是盲区。
2. **`s77`**：Turnip **确实执行了 77 个 render pass**，全部走 SYSMEM（GMEM `g=0`、
   autotune `a=0`、GMEM store `t/u=0`）。**§69.63「Turnip 端一个 render pass
   都没有」正式作废**，fix79 把根因收敛到编码链是错的。
3. **`q68/68 r0 k0`，`p1714 c1690 f161`**：68 次 `nmb_command_buffer_commit()`
   全部真实成功，**无一次伪造完成、无一次 wallpaper parked**；ENCODE 队列
   生产/消费基本相等（差 24 为在途）、161 个 batch 被 flush。
   ⇒ 「Metal→runtime 编码链断裂」假设**排除**。
4. **`draw=0`，`bad=1`**：按包边界的只读 walker 解析有效（`op43=sds`、
   `op46=ev`、`op3f=ind` 三项自洽），主 IB **顶层一个 `CP_DRAW_*` 都没有**；
   有 184 个 `CP_SET_DRAW_STATE`、136 个 `CP_INDIRECT_BUFFER`、9300 个
   `CP_BLIT`、90251 个 REG 写。11 个 `bbytes=80000` 的大 BO 与
   `cbytes=3fff8` 的 256 KiB 提交解释了 blit 集中在前 16 个 buffer。
   ⚠️ **观测局限**：A7XX 的 draw 命令可能位于 `CP_SET_DRAW_STATE` 指向的
   draw-state buffer 或 `CP_INDIRECT_BUFFER` 指向的 IB2；
   `scanIbPackets()` 只扫 IB1 顶层，**没有递归跟进二级 IB**，因此
   `draw=0` 只能定性「主 IB 无 draw」，**不能**定性「GPU 从未收到 draw」。
5. **全 SYSMEM 的两个候选 reason 无法区分**：`tu_note_render_mode()` 把
   「Autotune selected sysmem」放 `rm[2]`，**其余所有 reason 都放 `rm[4]`**，
   包括 `sheng-tum=s` 的 `TU_DEBUG(SYSMEM)` 和 **`"Render area is empty"`**
   （`tu_cmd_buffer.cc:1416-1426`）。后者会让 pass 走 SYSMEM 却不产生任何
   几何，正好解释「有 pass / 无 draw / surface 全零」。区分只需 `rm[5]`
   （千像素总数）：空 render area ⇒ `rm[5]=0`。
6. 物理页绑定第五次复现：`IMP phys == S5 phys`、`ro=0`、`IMPF=0`。
   `SMMUFAULT`/`GFXFAIL` 各 16 条仍是历史已排除的非 latched fault / 零超时 poll。

### 69.64.6 排除项总表增补（fix80）

| 假设 | 结论 | 依据 |
| --- | --- | --- |
| 诊断统计可用（fix78/79 的 R 全零是真值） | **否，观测假象** | `SHENG_DIAG` 从未设置；fix80 `h1 s77` |
| Turnip 没有执行 render pass | **排除** | `R s77` |
| ENCODE 队列 / `nmb_command_buffer_commit` 断裂 | **排除** | `Q q68/68 r0 k0 p≈c` |
| 171 次 commit 都是真实提交 | 修正：fix80 窗口内 68 次真实且全成功 | `Q` |
| 主 IB 顶层有 draw 包 | **否** | `MUYU PKTS draw=0, bad=1` |
| render area 非空 | **待定** | 需 `rm[5]` / pass-ring `width,height,draws` |

## §69.65 fix81：把 pass ring 送进 msgbuf，一次判定 render area 与 draws

`tu_pass_ring_read(from,out,max,&newest)` 是**按 seq 随机读、不消费**
（`turnip-gmem-mutable-swap.patch:461-489`），所以遥测可以安全地窥视最新记录，
不干扰 frame-trace 线程的 `passFrom` 游标。`tu_pass_rec`（72 B）含
`width/height/mode/colors/flags/swap/draws/format/kpx/gpu_begin/gpu_end`。

fix81 三处观测改动（不改行为、不改 Turnip、不改 Rust）：

1. `native_bridge_metal_pass_ring_line()`：新增 `P` 行
   `z<dlsym?> n<newest seq> s<seq> w<W>x<H> d<draws> m<mode> f<flags> k<kpx> c<colors> x<swap>`。
2. `R` 行追加 `k<rm[1]>/<rm[3]>/<rm[5]>`（GMEM / autotune SYSMEM / 其它 SYSMEM
   的千像素总数）。
3. plugin 同步块 + 周期线程各发第 8 行 `P `；`NativeAdrenoPublish.cpp`
   USTEXT 白名单加 `P`、上限 320→400。

判据：
- `P z0` ⇒ `tu_pass_ring_read` 没拿到（SHENG_DIAG 未传到 Turnip image）；
- `R k0/0/0` 或 `P w0x0` / `k0` ⇒ **render area 为空**，这就是「有 pass、
  无 draw、surface 全零」的根因，下一步查 `render_areas[0]` 为何为 0；
- `P d0` 且 `w×h` 正常 ⇒ Turnip 记录了 pass 但 `draws=0`，查
  `tu6_draw_common` 的发射条件与 draw-state buffer；
- `P d>0` 而 `MUYU PKTS draw=0` ⇒ draw 在二级 IB，需递归扫描
  `CP_SET_DRAW_STATE` / `CP_INDIRECT_BUFFER` 的目标地址；
- `P f` 的 bit 4（16 = stored）若为 0，则 SYSMEM pass 连 store 都没做。

### 69.65.1 fix81 判读（413 s 自复位）

镜像 `ef987500…`，addon `c8327056…`、trust `d49aa30a…`，cmdline 1011 B；
日志 `logs/vendor_boot_gpu_fix81-logs/muyu-6757a797`（msgbuf 332482 B，到 175 s）。

```
MUYU USTEXT P z1 n142 s142 w3200x2136 d3 m2 f26 k6835 c2 x0
MUYU USTEXT R g0 a0 s142 e0 t0 u0 x1 m1 d1 k0/0/909199
MUYU USTEXT Q h1 q133/133 r0 k0 p3250 c3250 f289
MUYU PKTS n=240 draw=0 ev=4059 sds=361 ind=266 blit=9365 oth=5946
           t4=100147 bad=0 op22=0 op38=0 op2a=0 op43=361 op46=4059 op3f=266 op55=916
MUYU 10BIT S4 n=128 nz=0 ...
```

1. **render area 非空**：`w3200x2136`、`k6835`（= 3200×2136/1000）、累计
   `R k0/0/909199` ⇒ **"Render area is empty" 排除**。
2. **`d3`**：Turnip 为**每个 pass 记录了 3 次 draw**；`f26` = 2 (linear image)
   + 8 (loaded) + 16 (stored)，有 load 有 store；`c2` 双 color attachment、
   `x0` swap=0、`m2` SYSMEM(other reason)。
3. **`Q q133/133 r0 k0 p3250 c3250`**：133 次真实 commit 全成功、ENCODE
   生产与消费**完全相等**、无伪造完成 ⇒ 编码/提交链健康再次确认。
4. **`draw=0` 是观测局限，不是故障**（离线代码定性）：Turnip 的
   `tu_CmdDraw` / `tu_CmdDrawMultiEXT` / `tu_CmdDrawIndexed` 全部把
   `CP_DRAW_INDX_OFFSET`(0x38) emit 到 **`cmd->draw_cs`**
   （`tu_cmd_buffer.cc:9080, 9104, 9147`）；`tu_cmd_render_sysmem()` 用
   `tu_cs_emit_call(&cmd->cs,&cmd->draw_cs)`（`:4062`）生成
   `CP_INDIRECT_BUFFER`(0x3f) 指向它。KMD 的 `scanIbPackets()` 只走 IB1
   顶层，**没有跟进二级 IB**，所以顶层 `op38=0` 正是预期结果，
   不能据此说 GPU 没收到 draw。
5. 于是剩下的真实未知是：**142 个 3200×2136、3 draws、带 load/store 的
   SYSMEM pass，为什么没有在 display surface 留下一个非零 word**。

### 69.65.2 被遗忘的行为变量：`sheng-tum=s`

`tu_sheng_render_mode()`（`turnip-gmem-swap-trace.patch:183-193`）从 boot arg
读 `sheng-tum=g|s`，注释原文：

> *Boot-arg sheng-tum=g renders every pass that can in GMEM, sheng-tum=s every
> pass in system memory (transient attachments too): the colour runs of the
> GMEM swap check, set by the boot so their command lines stay short.*

它是 **fix70 为「GMEM swap trace 的颜色对照」临时加的纯诊断开关**，
之后 66–81 每一轮的构建命令都原样带着 `--kernel-arg sheng-tum=s`。
`use_sysmem_rendering()` 里 `sheng_mode=='s'` ⇒ `forced_sysmem=true`
⇒ reason `TU_DEBUG(SYSMEM)` ⇒ `rm[4]`（`R` 行的 `s`）。
即：**本板从 fix70 起所有 render pass 都被人为强制成 SYSMEM，从未跑过
GMEM / autotune 路径**。SYSMEM 依赖 `tu6_emit_mrt()` 把 color attachment 的
GPU 地址直接写进 MRT 寄存器，而 display surface 是 `b3a8` 双平面 explicit
layout；GMEM 路径则走 tile store 按 texel 写回，两者的布局假设不同。

## §69.66 fix82：去掉强制 SYSMEM + 递归扫描 draw_cs

两个改动合成一轮（fix81 为唯一对照，其它条件全同）：

1. **行为实验**：payload 构建**去掉** `--kernel-arg sheng-tum=s`，cmdline
   1011→999 B。`tu_sheng_render_mode()` 返回 0，`forced_sysmem=false`，
   Turnip 回到 autotune / GMEM 选择。
2. **观测补齐**：`NativeAdrenoVm::scanIbPacketsAt(iova,bytes,depth)` 在遇到
   `CP_INDIRECT_BUFFER`(0x3f) 时解析 3-word payload
   `{addr_lo, addr_hi, size_in_dwords}`，递归跟进 `draw_cs`（深度 1、
   每个主 IB 最多 4 个目标），把二级 IB 里的 draw 计入 `native_adreno_pkts[0]`
   并单列 `ib2/d2/b2`。`NativeAdrenoSubmit.cpp` 的 `MUYU PKTS` 加三列。
   纯只读，不改 strip 行为。

判据：
- `R g>0`（出现 GMEM pass）且 `S4 nz>0` ⇒ **强制 SYSMEM 就是根因，首帧像素链打通**；
- `R g>0` 但 `S4 nz=0` ⇒ GMEM store 没写回，回到 `tu_attachment_unpadded` /
  tile-store 布局（§69.59 的老方向，但这次有真实 GMEM pass 可查）；
- `R` 仍 `s>0` ⇒ 还有别的强制 SYSMEM 原因，最可能是
  `!vsc->binning_possible && vsc->binning_useful`（`tu_cmd_buffer.cc:1465`，
  与 fix61 剥离 BV/BR 握手包、`TU_DEBUG=nocb` 有关）；
- `PKTS d2>0` ⇒ draw 确实进了 GPU 命令流（在 `draw_cs` 里），
  坐实「IB1 顶层无 draw」只是结构而非故障；
- `PKTS d2=0` 且 `b2` 大 ⇒ 二级 IB 地址解析失败或目标未映射，查 IOVA。

### 69.66.1 fix82 判读（197 s 自复位）

镜像 `ef567ee7…`，cmdline **999 B 且不含 `sheng-tum=s`**，日志
`logs/vendor_boot_gpu_fix82-logs/muyu-67306e11`（msgbuf 263508 B，到 171 s）。

```
MUYU USTEXT R g0 a1 s0 e0 t0 u0 x1 m1 d1 k0/0/0
MUYU USTEXT R g10 a1 s0 e0 t0 u0 x1 m1 d1 k13814/0/0     ← GMEM 10 passes
MUYU USTEXT P z1 n11 w3200x2136 d3 m0 f16 k6835 c2 x0     ← m0 = GMEM
MUYU USTEXT Q h1 q2/2 r0 k0 p106 c106 f27
MUYU PKTS n=16 draw=15 ev=229 sds=62 ind=492 blit=9251 oth=2284
           t4=82035 bad=0 ib2=16 d2=15 b2=0
           op22=0 op38=15 op2a=0 op43=62 op46=229 op3f=492 op55=20
MUYU 10BIT S4 n=1..4 nz=0 ...
MUYU CPROW C10evt R800005 I0 F0 r28/54 m28/8 s400         ← RPTR 卡住
```

1. **去掉 `sheng-tum=s` 确实让 GMEM 恢复**：`R g10 a1 s0`（10 个 GMEM pass、
   13814 kpx，1 个 autotune SYSMEM），`P m0` 也确认最新 pass 是 GMEM。
2. **IB2 递归扫描生效且坐实 fix81 的判断**：`ib2=16 d2=15 b2=0
   op38=15` —— 15 个 `CP_DRAW_INDX_OFFSET` **全部在 `draw_cs` 二级 IB 里**，
   没有一个解析失败。所以「IB1 顶层 draw=0」纯属结构，GPU 命令流里有 draw。
3. **但 GMEM 路径在本板立刻挂住**：`CPROW r28/54`（RPTR 28 落后 WPTR 54），
   `CPIB rem=b5b rem2=1`，帧流只有 `q2/2`、4 个 swap 帧，197 s 自复位。
   对照 **fix80/81（SYSMEM）的 `CPROW r102/102`、`r124/124`、
   `CPIB rem=0 roq=0/0`** —— RPTR==WPTR，命令被完整消费，CP 健康。
   ⇒ **`sheng-tum=s` 不是根因，而是绕开 GMEM/BV-BR 卡死的必要 workaround**
   （与 §69.59、fix61 剥离 concurrent-binning 握手包的结论一致）。不要删它。
4. `t/u=0` 需谨慎：`tu_gmem_side_stats` 的 site 0/2 只在
   `tu_gmem_transfer_swap_fixup()` 路径累加（patch 27-51），不是所有 GMEM
   store 都会走，不能单独定性「没有 store」。
5. 结论：**真正的谜题是 SYSMEM 路径** —— CP 正常消费完含 draw 的整个命令流、
   142 个 3200×2136 pass 带 load+store，display surface 却一个非零 word 都没有。

## §69.67 fix83：GPU 到底执行了 pass 吗

`tu_pass_rec.gpu_begin/gpu_end`（`turnip-gmem-mutable-swap.patch:319-335, 479-486`）
是 GPU 执行 pass 时由 `tu_pass_stamp()` 发射的
`CP_EVENT_WRITE7(RB_DONE_TS)` 写入 timestamp slot 的值；`tu_pass_ring_read()`
只在 `begin && end` 非零时填入。**CPU 记录了一个 pass 而 GPU 从未执行它，
这两个字段保持 0** —— 这是区分「命令流没到 GPU」与「GPU 执行了但写错地方」
的决定性判据，也是 fix66–fix82 一直缺的那一环。

fix83 两处改动：

1. `P` 行去掉冗余的 `s<seq>`，追加 `b<gpuBegin?1:0> e<gpuEnd?1:0>
   v<first colour attachment VkFormat>`。`v` 可与 `b3a8`（62336138）比对，
   检查 GMEM/SYSMEM 下真正绑定的附件格式。
2. 恢复 `--kernel-arg sheng-tum=s`（cmdline 回到 1011 B），回到 CP 健康的
   稳定 SYSMEM 路径；保留 fix82 的 IB2 递归扫描与全部 USTEXT 行。

判据：
- `b1 e1` ⇒ **GPU 确实执行了 pass**，根因在 attachment 绑定 / MRT 地址 /
  store 目标（下一步要比对**整块 34 MiB** 的物理页映射，fix77 只比了首段
  `getPhysicalSegment(0)`）；
- `b0 e0` ⇒ GPU 从未执行 pass（但须先排除 Turnip 未分配 `tu_pass_ts_bo`
  导致 timestamp 恒 0 的观测局限），回到 CP 提交链；
- `v` 与 `62336138` 不符 ⇒ 渲染目标根本不是 `b3a8` display surface。

### 69.67.1 fix83 判读（413 s 自复位）

镜像 `80ed0762…`，cmdline 1011 B（恢复 `sheng-tum=s`），日志
`logs/vendor_boot_gpu_fix83-logs/muyu-67306f73`（msgbuf 387569 B，到 389 s）。

```
MUYU USTEXT P z1 n12 ... w3200x2136 d3 m2 f26 k6835 c2 x0 b0 e0 v98
MUYU USTEXT P z1 n37 ... b1 e1 v98      ← GPU 写了 pass 时间戳
MUYU USTEXT P z1 n72 ... b0 e0 v98
MUYU USTEXT P z1 n96 ... b1 e1 v98      ← 再次
MUYU USTEXT R g0 a0 s96 ... k0/0/594789
MUYU USTEXT Q h1 q87/87 r0 k0 p2146 c2146 f197
MUYU PKTS n=224 draw=398 ... ib2=268 d2=398 b2=0 op38=398 bad=0
MUYU 10BIT S4 n=128 nz=0 ...
MUYU CPROW ... r1a4/1a4 ...             ← RPTR==WPTR
```

1. **`b1 e1` 确实出现**：`tu_pass_ts_bo` 存在且 GPU 写出了
   `RB_DONE_TS` 时间戳 ⇒ **GPU 真的执行了 render pass**。
   `b0/e0` 与 `b1/e1` 交替是「刚被 CPU 记录、GPU 尚未跑到」的正常时序
   （patch 注释：last 300 ms 的 pass 可能还没运行），不是执行失败。
2. **命令流完整且被消费**：`draw=398 op38=398 d2=398 b2=0` —— 398 个
   `CP_DRAW_INDX_OFFSET` 全在 `draw_cs`，零解析失败；`r1a4/1a4` CP 无积压；
   `q87/87` 87 次真实 commit 全成功。
3. **因此 fix66–fix83 的全部「没执行」类假设都排除**：GPU 执行了 96 个
   3200×2136 pass、每个 3 draws、有 load+store，CP 消费完整个命令流，
   但 display surface 三个视图仍是一个非零 word 都没有。
   根因只能在**渲染结果的落地/写回**这一环。
4. **`v98 = VK_FORMAT_R32_UINT`**（按仓库 `vulkan_core.h` 枚举逐项解析确认）。
   runtime 的 `ExtendedRange::Planes`（即 `RGB10A8_2P_XR10`）实际用
   **`VK_FORMAT_R32G32_UINT`**（8 B/texel，color word + alpha byte），
   而 98 = `R32_UINT` 对应 `ExtendedRange::Bgr10 | Rgb10A2Srgb`
   （`metal/format/mod.rs:400-412`）。两者不符，值得继续核对 Turnip 看到的
   第一个 color attachment 究竟是哪一个 view。

### 69.67.2 离线调查：全零不是「合法黑」，且存在 copy-back 分水岭

- **`PlanesCopy::R32`（sample_planes.rs:1240）与 XR10 双平面无关**——它是
  MSAA framebuffer-fetch 把每个 sample 拆成 2D-array 层时用的 copy 格式。
  先前「runtime 把两个平面拆成 2 个 R32Uint attachment」的猜测不成立。
- **`RGB10A8_2P_XR10` 是偏置编码**：颜色 0 的 10-bit code 是 **384**，
  「清零到黑」的 texel 约为 `[0x18060180, 0, 0, 0]`，**非零**；
  全零位读出来是 `-384/510 = -0.7529`
  （`clear_reach.rs:188-197, 808-830`、`metal/format/mod.rs:708-721`）。
  ⇒ display surface 全零**不是被清成黑**，而是连 clear 都没写进 IOSurface，
  或写进了 shadow image 却没搬回来。
- **两条互斥的落地路径**（必须先用计数区分）：
  - `Storage::Alias`：`NativeMetalObjects.rs:185` 的
    `HostPageTexturePolicy::InPlaceWrites` 让 texture 直接在 IOSurface 字节上
    建 image（`texture.rs:2235-2242`），**没有 shadow、没有 copy back**；
    此时问题在 Turnip 的 SYSMEM 写回。
  - `Storage::Shadow`：双平面 XR10 由 `new_surface_planes_texture()`
    （`buffer_texture.rs:687-694` → `texture/surface_two_planes.rs:168-268`）
    建成 `R32G32_UINT` shadow image，渲染结果必须在 encoder 结束时
    `ShadowScheduler::finish()`（`shadow.rs:3076-3083`）copy back 到 IOSurface。
    该链**不**被 `SHENG_DIAG` / `framebuffer_fetch` / `TU_DEBUG` gate；
    主要 gate 是 `storeAction == DontCare`（`render/shadows.rs:459-464, 519-541`
    → `shadow.rs:3037-3050`），而 fix79 已确认本板 96 个 surface pass 全是
    `Store`（`s0/96/0/0`），所以理论上不该跳过。
- **`nmb_device_texture_counts()`（`NativeMetalObjects.rs:353-370`）已导出这
  8 个计数且不受 SHENG_DIAG 影响**，前端遥测表已有
  `row("L inp ... sh ... cp ... bk ... rt ...")`（`NativeBridgeMetalFrontend.mm:4990-4995`），
  但 `L` 行只进 HUD/遥测表，**不在 msgbuf**，所以此前从未被读到。

## §69.68 fix84：把 texture copy census 送进 msgbuf

零重编 Rust runtime 的最小改动：新增
`native_bridge_metal_texture_line()` 直接调 `nmb_device_texture_counts(gDevice,tc)`，
输出 `i<in-place> p<pitched> r<refused/…> s<shadowed> c<copies-in>
b<texels-copied-back> t<shadowed render targets>`；plugin 同步块 + 周期线程各发
第 9 行 `L `，`NativeAdrenoPublish.cpp` 白名单加 `L`、上限 400→480。
其余条件与 fix83 完全相同（含 `sheng-tum=s`、SHENG_DIAG、IB2 递归扫描）。

判据：
- `i>0, s=0, b=0` ⇒ **in-place 直写，根本没有 copy-back 链**，问题在
  Turnip 的 SYSMEM imported-plane store。下一步试 `TU_DEBUG=UNALIGNED_STORE`
  / `3D_LOAD`（强制 `tu_attachment_unpadded` 的 texels-alone store）或
  `sheng-tum=g` 走 GMEM tile store。
- `s>0, c>0, b=0` ⇒ shadow 有 copy in 但 **copy back 从未发生**，
  查 `shadow.rs:3076 finish()` 的 `dirty` 与 `ShadowUse.writes`；
  这是最可能直接解释「GPU 画了、surface 全零」的一条。
- `b>0` ⇒ 写回发生了，改查写回目标 IOVA / staging buffer 是否就是
  IOSurface import BO（`surface_two_planes.rs:220-251` 的 `texels.buffer`）。

> 改动 Rust runtime 的代价：`.cache/gpu-reference/metal2vulkan/` 下任何 `.rs`
> 手改都会被 `build-adreno-metal-bridge.py:93-101` 的 git-tree 前缀校验拒绝，
> 必须做成新的 `NNNN-*.patch` 追加进 `PATCHES` 并全量 cargo 重编；
> 因此 fix84 一律走「只加 ObjC/C++ 观测 + boot arg 实验」的路线。

### 69.68.1 fix84 判读（414 s 自复位）—— **收敛点：copy back 从未发生**

镜像 `7ff4595a…`，cmdline 1011 B，日志
`logs/vendor_boot_gpu_fix84-logs/muyu-674384d7`（msgbuf 409293 B，到 390 s）。

```
MUYU USTEXT L i0  p0  r0/0 s0 c0  b0 t0
MUYU USTEXT L i7  p5  r2/0 s2 c6  b0 t0
MUYU USTEXT L i26 p24 r2/0 s2 c33 b0 t0     ← 33 copies IN, ZERO copied BACK
MUYU USTEXT P z1 n13 w3200x2136 d3 m2 f26 k6835 c2 x0 b1 e1 v98
MUYU USTEXT R g0 a0 s40 ... k0/0/212029    Q h1 q31/31 r0 k0 p795 c795 f85
MUYU PKTS n=304 draw=472 ... ib2=318 d2=472 b2=0 op38=472 bad=0
MUYU 10BIT S4 n=128 nz=0 ...
```

- **`s2`** 恰好对应两个 swap buffer 的 shadow（`S5 surf=...428 / ...7e0`）；
- **`c33`**：33 次 copy in（把 IOSurface 字节搬进 shadow 供 load）；
- **`b0`**：`shadow_texels_copied_back == 0` —— **一次写回都没有**；
- **`t0`**：`host_texture_shadows_render_target == 0` —— 没有任何 shadow 被登记为
  render target，这与 `ShadowUse.writes` 不置位 → `shadow.rs:3037` 的
  `filter(|s| s.writes)` 落空 → `shadow.rs:3076 finish()` 不 copy back 一致；
- 同时 `i26 / p24`：多数 host-page texture 走 `Storage::Alias` in-place，
  `r2/0` 有 2 个因 pitch 被拒建 shadow。

**结论**：fix66–fix84 逐项排除后，根因收敛为
> **渲染结果停留在中间 image（shadow / 或 in-place 路径未被正确落地），
> 没有任何字节被搬回 IOMFB 的 display IOSurface**。
GPU 侧（身份、CP/fence、物理页、权限、import、draw 编码、pass 执行、
CP 消费）全部健康；问题在 metal2vulkan runtime 的
`Storage::Shadow` copy-back 调度（或 in-place image 与 IOSurface BO 的绑定）。

### 69.68.2 fix85 方向（按优先级）

1. **查 `t0` 为何为 0**：`work_counts.host_texture_shadows_render_target`
   的累加点（`work_counts.rs:328-363`）与 `render/shadows.rs:534-541` 的
   `ShadowUse.writes`；`Store` 已在 fix79 证实（`s0/96/0/0`），所以更可能是
   双平面 texture 根本没被登记为 shadow render target（走的是
   `new_surface_planes_texture()` 的另一条分支）。
2. **零重编的低风险行为实验**：`sheng-tu-debug=3D_LOAD` 或
   `=UNALIGNED_STORE`（plugin 已支持 `sheng-tu-debug=<flags>` → `TU_DEBUG`），
   强制 `tu_attachment_unpadded()` 的 texels-alone load/store；
   或 `sheng-fb-bgra=1` 换成 8-bit BGRA 单平面，绕开整个 XR10 双平面链。
3. **比对写回目标**：`surface_two_planes.rs:220-251` 的 `texels.buffer`
   与 `MUYU IMP ... va=`（import BO 的 IOVA）是否同一个 BO。
4. 若必须改 Rust runtime：新增 numbered patch 追加到
   `build-adreno-metal-bridge.py:26-78` 的 `PATCHES`，全量 cargo 重编；
   不要手改 `.cache` 下的 `.rs`（git-tree 前缀校验会直接拒绝）。





## §69.69 fix85：把 stand-in / 内存 / 未对齐 / CPU 侧读回四行送进 msgbuf

fix84 的 `L i26 p24 r2/0 s2 c33 b0 t0` 把问题逼到「写了但没写回」，
但 `b=0` 有三种互斥解释，必须先分开：走 shadow（copy back 缺失）、
走 stand-in（attachment 的替身镜像）、或根本就是 in-place（没有写回链）。
离线读 runtime 源码得到的关键事实：

- `two_planes.rs:33`：`RGB10A8_2P_XR10` 与 `_sRGB` **整体持有一张
  `R32G32_UINT` 镜像**（"each texel's color word, then its alpha byte in the
  second word"），8 B/texel，而 surface 的字节是 4 B + 1 B；
- `render.rs:116`：双平面 texture 作为颜色附件时渲染进 **stand-in 镜像**
  （"image the planes' copies cannot read and write renders into a stand-in
  that they can"），只有 stand-in 的 copy 才把 texel 放进 plane；
- `work_counts.rs:4401-4435`：`host_texture_shadows_refused_*` 不是「拒绝建
  shadow」，而是「**因为**某原因**才**建 shadow」，`r=2/0` 表示 2 个纹理因
  pitch 而建 shadow；
- `buffer.rs:2035` + `fresh_pages.rs:62`：runtime 自己的大 buffer 会落在
  **新 mmap 的匿名页**上再按指针 import（`fresh_page_buffers_made`）——
  这类页天然读零，且与调用方的页无关。

于是 fix85 只做观测（零行为改动），把四行接进 USTEXT：

| 行 | 内容 | 判据 |
| --- | --- | --- |
| `M` | `nmb_device_pass_counts`：pass 以 instance 计、stand-in 个数/MiB、stand-in copy 次数/其中 at-draw | `sti>0, cp=0/0` ⇒ stand-in 写回缺失；`sti=0` ⇒ 根本不走 stand-in |
| `N` | `nmb_device_memory_counts`：pool/empty/dedicated/**host imports**/alloc/live blocks | `imp` 里是否含 34 MiB；是否出现 ~52 MiB 的私有替身 |
| `Y` | `gUnaligned*`：行距/偏移无法被线性镜像承载的 host 纹理 | `n=0` ⇒ surface 走 in-place，不是被拒后 shadow |
| `Z` | 前端在**自己的 mapping** 上对最大 import 做 64 点读（先 `sys_dcache_flush`） | `nz=0` ⇒ CPU 侧也读不到，非缓存假象 |

### 69.69.1 fix85 判读（414 s 自复位，日志到 390 s）

镜像 `55015bb3…`，cmdline 1011 B，addon/trust 与 fix85 一致，
日志 `logs/vendor_boot_gpu_fix85-logs/muyu-6734cb45`（377460 B）。

```
MUYU USTEXT L i75 p73 r2/0 s2 c93 b0 t0
MUYU USTEXT M ins99 inl0 bar0 tile0 ml0 pend0 sti0/0M cp0/0
MUYU USTEXT N pool12(8) emp16 ded484 imp94 alloc512 blk3/10 gp0-0
MUYU USTEXT Y n0
MUYU USTEXT Z n2 nz0/64 w0=00000000 wl=00000000 f1
MUYU 10BIT S4 n=64 nz=0 ... stride=3200 alloc=34177024 fmt=62336138 planes=2
MUYU IMP h=3d va=110000000 bytes=2098000 phys=0000000924417000 seg=4096 spans=7532 ro=0 unc=1
MUYU IMP h=5d va=12b644000 bytes=1a13000 phys=00000000e3940000 seg=27340800 spans=1 ro=0 unc=1
```

四条结论：

1. **`Y n0`**：没有任何「行距无法承载」的纹理 ⇒ display surface **走 in-place**
   （`i75 p73` 里那 73 个就是按 Metal 行距的线性镜像），不是被拒后转 shadow。
2. **`M sti0/0M cp0/0`**：**完全没有 stand-in**，双平面的替身路径也没走。
3. **`Z nz0/64`（`f1` = `sys_dcache_flush` 已解析并调用）**：前端自己的
   mapping 上 64 点全零，排除「CPU 读到旧 cache」。这与 display 侧
   `S4 nz=0` 互相印证：两块 mapping 看到的是同一组**从未被写**的页。
4. **`MUYU IMP h=5d`**：一块 `bytes=0x1a13000 = 27,340,800 = 3200×2136×4`
   （正好 plane 0 字节数）的 import，**单段连续**、物理 `0xE3940000`，
   与 surface 的散页 import（`h=3d/4e`，7532/8042 spans）无关。
   它不是 `surfaceBuffer()` 造的（那里 `bytes` 恒为整块 allocation 34 MiB），
   所以另有来源：可能是 runtime 的 `fresh_page_buffers_made`、或是
   `newBufferWithBytesNoCopy` 的整屏 scratch。**fix86 必须先把它归因**。

于是 fix85 把范围压到一句话：
> 渲染目标被规划为「surface 页上的线性镜像」（`i75 p73`、`Y n0`、`M sti0`），
> 但 GPU 执行完 398 个 draw 后，**这批页在 CPU 与 display 两侧都读零**。
> 需要在内核侧、绕过 Metal/Turnip 的一切簿记，直接问 DRAM。

## §69.70 fix86：内核侧的 DRAM 硬判据（`MUYU IMW`）

`NativeAdrenoVm::sampleLargestImport()`：遍历本 VM 的 mapping，取最大的
**imported** BO（surface 的 34 MiB 就是它），用 `sampleWord()` 的
`native_adreno_dram_read32`（**uncached DRAM**，不经 CPU cache、不经
Turnip）在它的字节上取 64 点，输出 `nz/of first last bytes phys iova`。
提交路径每 8 个 buffer 打一行，和 `MUYU PKTS`/`SB` 同窗口。

判据：

- `IMW nz>0` ⇒ GPU 的写**确实进了** import BO 的物理页；那么 CPU 侧 `Z nz=0`
  与 display `S4 nz=0` 就说明我们读的不是同一组页（或写的是另一块 BO），
  下一步查 IOVA→phys 与 `Z`/`S4` 的页组差异。
- `IMW nz=0` 且 `phys` 与 `S5 phys` 相同 ⇒ 同一组页、GPU 从未写入；
  那 398 个 draw 落到了别的 BO，需按 `h=5d` 那条线归因。
- `IMW` 的 `bytes/phys` 若与 `S5` 不同 ⇒ 最大的 import 不是 swap surface，
  需要把 `MUYU IMP` 全表与 `S5` 逐一对齐。

### 69.70.1 fix86 判读（197 s 自复位，日志到 173 s）—— **同一组页、DRAM 全零**

镜像 `4af96226…`，复用 fix85 的 addon/trust，日志
`logs/vendor_boot_gpu_fix86-logs/muyu-671fe219`（297158 B）。

```
MUYU IMW nz=0/64 first=00000000 last=00000000 bytes=2098000 phys=0000000930744000 iova=0000000101d36000   ×14
MUYU 10BIT S5 n=1 phys=0000000930744000 seg=4096 surf=eb89e428 len=34177024
MUYU IMP h=3d va=110000000 bytes=2098000 phys=0000000930744000 spans=6013 ro=0 unc=1
MUYU 10BIT S4 n=4 nz=0 ... stride=3200 alloc=34177024 fmt=62336138 planes=2
```

三条对齐：

1. `IMW` 取样的是**最大的 imported BO**，`bytes=0x2098000`、`phys=0x930744000`
   与 `S5 n=1 phys` 完全相同 ⇒ 它就是 IOMFB 的 swap surface，不是别的 BO；
2. `nz=0/64`：提交路径每 8 个 buffer 取一次，14 次采样贯穿整个运行窗口，
   **经 uncached DRAM 读**，全部为零；
3. 同轮 `S4 nz=0`（display 侧）与 `Z nz=0`（fix85，CPU 侧）一致。

结论（fix66 以来最硬的一条）：
> **页组相同、GPU 命令流消费完毕、DRAM 仍为零。**
> 于是只剩两种可能，互斥且用一次实验即可分开：
> (a) 那 398 个 draw **没有指向这块 BO**（目标 image 在别处）；
> (b) draw 指向了它，但 GPU 对 system memory 的写**停在缓存里没有到 DRAM**。

fix86 还顺带否掉了一个 fix85 的候选：本轮 `MUYU IMP` 只有 8 条，**没有**那条
`bytes=0x1a13000` 的 27 MiB 连续 import，`BOA` 里也没有 27 MiB 级分配，
而 DRAM 依旧全零 ⇒ 那块 27 MiB BO 是偶发（`fresh_page_buffers_made` 一类的
scratch），不是渲染目标。

## §69.71 fix87：见证写（witness clear）—— 分开 (a) 与 (b)

做法：前端在每条 command buffer 的**最后一个 encoder**位置（`-submit` 开头，
此时 app 的 encoder 已全部结束）插入一个我们自己的 render pass：
`color0 = 该 surface 纹理`、`loadAction=Clear`、`storeAction=Store`、
`clearColor = (1,0,1,1)`（品红）。它不画任何东西，只清。

- 若 DRAM 首字变成 `0xC3FF03FF`（RGBA10A2 的品红）⇒ **GPU 的写能到 DRAM**，
  那么「app 的 draw 出来是零」⇒ (a) 不成立、问题在 app 的绘制内容/目标；
- 若 DRAM 仍为零 ⇒ **连我们自己的 clear 都落不下去** ⇒ (b)：GPU 对
  system memory 的写在缓存里，缺 flush/cache 维护；
- 若 `W e0`（编码数为 0）⇒ 见证 pass 根本没进 runtime，是观测失效
  （fix72 在 telemetry 线程上踩过同类坑）。

开关零成本：cmdline 1011→1012 B，把 `sheng-tum=s` 改成 `sheng-tum=sw`。
Turnip 的 `tu_sheng_render_mode()` 只读 `at[10]`（`'s'`），末位 `w` 被忽略，
因此 SYSMEM 行为不变；plugin 用 `bootArgSet("sheng-tum=sw")` 设
`SHENG_GPU_WITNESS=1`，前端 `witnessOn()` 读它。
新行 `W a1 e<n> r<n> t3200x2136` 报告武装状态、编码/拒绝次数与目标尺寸。

### 69.71.1 fix87 判读（414 s，**观测污染主链，实验作废**）

镜像 `df0cc385…`，cmdline 1012 B，日志
`logs/vendor_boot_gpu_fix87-logs/muyu-673e2798`。

```
MUYU USTEXT W a1 e0 r46   t3200x2136
MUYU USTEXT W a1 e0 r1349 t3200x2136
MUYU USTEXT Q h1 q0/0 r1239 k0 p29794 c29794 f2501
MUYU USTEXT S c595/595 ... (commits/failed)
MUYU 10BIT S4 n=1344 nz=0 ...
```

- `W a1` 武装成功、目标尺寸正确，但 **`e0`（一次都没编码成功）而 `r1349`**：
  见证 pass 每次都被前端拒绝；
- 更糟的是 `Q q0/0 r1239`：**这条 run 里没有任何 command buffer 到达
  runtime**。`[self refuse:]` 会把拒绝写成该 command buffer 的 `encodeFailure`，
  于是我们的见证 pass 把 app 自己的整个提交也判成了失败。实验数据因此全废。

修法（fix88）：

1. **见证目标改为 app 自己绑定的 color0**（在 `attachment()` 通过校验、
   `surfaceTarget && big` 的同一处捕获 `t0`），而不是 import 时缓存的纹理对象——
   后者被 runtime 判为 foreign；
2. `witnessClear` 保存并在失败时恢复 `self.encodeFailure`（它是 `NSString *`，
   不是 BOOL），保证见证失败不会污染主人的 command buffer；
3. `W` 行加 `s<stage>`：1 无 descriptor、2 无 attachment、3 encoder 被拒，
   下次失败能直接定位到哪一步。

### 69.71.2 fix88–fix93：见证写为什么一次次失败，以及它最终说了什么

| 轮次 | 改动 | 结果 | 说明 |
| --- | --- | --- | --- |
| fix88 | 见证目标换成 app 自己绑定的 color0；失败时恢复 `encodeFailure`；`W` 加 stage | `W a1 e0 r49 s3`，`Q q49/49` | encoder 仍被拒，但**主链不再被污染**（fix87 的 `q0/0` 消失） |
| fix89 | 改用 blit `fillBuffer` 写 `0xCD` | `W a1 e0 r128 s4` | `texture.buffer` 为 nil：app 绑定的常是 **view**，view 只继承 `.surface`（`NativeBridgeMetalFrontend.mm:1217`） |
| fix90 | 改为在 surface **导入处**记住 `NBBuffer` | `W a1 e0 r19 s5` | `blitCommandEncoder` 返回 nil |
| fix91 | 把 runtime 的拒绝文本尾部带进 `W m%.24s` | `mblitCommandEncoder after commit` | **根因**：`-commit` 先 `self.committed=YES` 再调 `-submit`，而前端对所有 `committed` 之后的 encoder 一律拒绝（`:2967`） |
| fix92 | 见证改用**自己的 command buffer**（`nmb_command_buffer_new(queue)`），并移到 app 提交成功之后 | `W a1 e85 r0` | 编码/提交全成功，85 次 |
| fix93 | 同上，稳定一轮 | `W e85`、`IMW nz=0/64`、`S4 nz=0` | **GPU blit fill 的 0xCD 既没进 DRAM，也没进 display 的读** |

fix93 的意义：这不是「draw 目标不对」，因为 blit fill 不经过 render pass、
不经过 tile memory、不需要管线状态——它只写 buffer。它照样没落地。
于是「app 的 draw 画到别处」这一支被排除，剩下的要么是
**IOVA 与物理页的映射不是同一组**，要么是**GPU 对 system memory 的写不落 DRAM**。

fix93 还有一个必须补的观测漏洞：本板**同时导入两块 34 MiB swap surface**
（`S5 surf=…428 / …7e0`），而 `IMW` 只采样「最大的一块」。见证写到的可能是
另一块，于是「全零」可能只是采样错了对象。fix94 把两边都改成覆盖多块：
内核 `sampleImports()` 输出前 4 大 import 各自的 `nz/w0/phys/iova`，
用户态对捕获到的**每一块** surface BO 都做 64 B 的 `0xCD` 填充。

## §69.72 fix94 判读 + fix95：**GPU 连自己分配的 buffer 都写不进去**

fix94（`logs/vendor_boot_gpu_fix94-logs/muyu-674ec74e`）把「采样对象错」这条也排除了：

```
W a1 e56 r0 s0 b2 t3200x2136 m
MUYU IMW n=4 | i0 nz=0/64 w0=00000000 b=2098000 p=000000091f831000 iova=0000000101d36000
             | i1 nz=0/64 w0=00000000 b=2098000 p=000000093511d000 iova=0000000110cd0000
             | i2 nz=7/64 b=1a13000 p=00000000e3940000 | i3 nz=7/64 b=160000 p=000000091d189000
```

两块 swap surface 都对之做了 `fillBuffer(0xCD)`（`b2`），两块都是 `nz=0 w0=0`；
而同一行里别的 import（`i2/i3`）能看到既有数据（`nz=7~10/64`）⇒ 内核的采样读法本身正常。
`MUYU 10BIT S5 n=1/n=2 phys=` 与 `i0/i1` 逐字一致，display 侧与 KMD 侧看的是同一组物理页。

### 69.72.1 fix95 设计：把「内存对象」这个变量整个拿掉

前几轮的对照组都还在 imported surface 上，因此始终留着一种解释：**只有 host 页（import）
的写不落地**。fix95 在同一个 blit pass 里加了两份对照：

1. `p`：对**本设备自己分配的 16 KiB buffer**（`nmb_buffer_new(gDevice,…)`，GPU 自有 BO、
   走 `NativeAdrenoMemory` 的系统页而不是 import）做同样的 `fillBuffer(0xCD)`；
2. `c`：把刚填过的 surface 前 4 KiB **copy** 进同一 buffer 的 8192 偏移（拷的是 GPU 读到的内容）。

读回在**下一轮**见证 pass 开头做（上一轮的 fence 早已 signal，不存在竞态），
`sys_dcache_flush` 后取 16 个字，`W` 行加 `p<非零数>/<word0> c<非零数>/<word0> R<轮数>`。

判据：`p>0` ⇒ GPU 的写能进它自己拥有的普通系统内存，问题只在 import 页；
`p=0` ⇒ 引擎的写根本不落任何内存，与内存对象无关。

### 69.72.2 结果：`p=0`，问题与内存对象无关

`logs/vendor_boot_gpu_fix95-logs/muyu-674db708`（kernel 173 s，msgbuf 尾部缺 21 s）：

```
W a1 e0  r0  s0 b0 t0x0        m p0/00000000 c0/00000000 R0
W a1 e4  r0  s0 b2 t3200x2136  m p0/00000000 c0/00000000 R4
W a1 e14 r0  s0 b2 t3200x2136  m p0/00000000 c0/00000000 R14
W a1 e27 r0  s0 b2 t3200x2136  m p0/00000000 c0/00000000 R27
W a1 e43 r0  s0 b2 t3200x2136  m p0/00000000 c0/00000000 R43
```

43 轮、每轮两次 fill + 一次 copy，**设备自有 BO 的 DRAM 也是 0**，copy 过去的 4 KiB 也是 0。
同时 `IMW i0/i1 nz=0/64`、`Z n2 nz0/64` 不变。

结论（本轮最硬的一条）：**GPU 的 blit/光栅引擎在整条 run 里没有向任何内存写出过一个字节**，
既不是 import 页的问题，也不是 display 读的问题，更不是 CPU cache 的问题。
与之对照，**CP 自己写的 fence seqno 是能落地的**（`CP_MEM_WRITE`→`fence_[1]`，
`wait()` 一直返回 `NativeAdrenoRingFenceComplete`，否则提交路径早就停了）。
⇒ 写通路在「CP」这一侧是好的，在「3D/blit 引擎」这一侧整体失效。

### 69.72.3 顺带否掉的一条：coherent / write-back 映射

`NativeAdrenoUserClient::mapBo` 的 `coherent` 分支对 imported BO 会强制
`writeback=1 shareability=3`（`ArmSmmu4K.c:76` 把它翻成 MAIR index 1 = `0xFF`，Normal WB IS）。
本轮实测日志里所有 import 都是 `coh=0`：

```
MUYU IMPM h=3d iova=0000000101d36000 bytes=2098000 phys=000000091f831000 ro=0 perm=3 coh=0
```

原因是本板根本没有代码去把 `native_adreno_io_coherent` 置 1（`UniXNU-uke` / `UniXNU-sheng`
的 `GpuPowerProbe.cpp:1272` 才有，muyu-17 没有）。而用户态 `tu_knl_darwin.cc:977`
申请映射时一律 `writeback=0 shareability=2`（**Normal Non-cacheable**）。
所以「被强制成 WB 可缓存导致写不落 DRAM」这条**在本板不成立**，它没有发生。

## §69.73 fix96 / fix97：BV/BR 线程切换在本板**任何形式都不可用**

fix95 之后，`stripThreadSync` 成为唯一剩下的行为变量。它在 fix61 里把
`CP_THREAD_CONTROL`(0x17) 与 `CP_BV_BR_COUNT_OPS`(0x1b) **整类**改写成 NOP，
理由是「SYNC 位要等一个从未启动的 BV 线程」。但如果只是为了躲 SYNC，那么
不带 SYNC 的纯线程切换（`CP_SET_THREAD_BR/BV`）本该无害。fix96/97 就是把这
两类分开剥除的对照实验，开关用 `sheng-tum=` 的尾字符携带（cmdline 已到 1016 B
上限，`sheng-tum=sw` 的读者只看 `at[10]`，末位可零成本带一个数字）：

```
MUYU STRIP mode=<1|2|3>      mode 1 = 全剥（fix61..fix95 的默认）
                             mode 2 = 只剥 SYNC 形式的 0x17，0x1b 仍剥
                             mode 3 = 只剥 SYNC 形式的 0x17，0x1b 保留
```

| 轮次 | mode | 结果 | 说明 |
| --- | --- | --- | --- |
| fix96 | 3（`sheng-tum=sw3`） | `MUYU STRIP mode=3`，全部 `n=0 tc=0 bv=0`；**只提交 11 次**就 `MUYU HC hang n=2 ticks=169 hook=1`，`NativeAdreno recovery: fence=8 … fsr=0`；`MUYU PKTS` 一行都没打出来（<16 次提交） | 完整 BV/BR 协议一开就停，和 fix61 的 `CP_BV_BR_COUNT_OPS` park 一致，`fsr=0` 说明不是翻译错 |
| fix97 | 2（`sheng-tum=sw2`） | `mode=2`，`tc=0`（0x17 一个都没剥）、`bv=3→5`（0x1b 仍剥）；`W a1 e0`、`z1 n1 w1x1 d4 … b0 e0`；**2 次 `HC hang`** | 连「只保留线程切换、剥掉计数握手」也停 |

结论：本板 CP 只要真的执行 `CP_THREAD_CONTROL` 就停（不管带不带 SYNC），
**BV/BR 双线程路径整体不可用**，`stripThreadSync` 的整类剥离是必需的，
不是可以优化的保守选择。Binning 这条路在 CP 侧就是关死的，别再从 turnip 的
`nocb` / `forcecb` 上找。

对应地，fix83 的 `z1 … b1 e1` 已经在 fix95 里给出了另一半事实：
pass 的 `CP_EVENT_WRITE7(RB_DONE_TS)` 起止时间戳**都被写进了内存**（b=1 e=1），
即 GPU 认为这个 pass 跑完了、`d3` 个 draw 也在流里、`k6835` kpx 覆盖整块
3200×2136，但 DRAM 零字节。fix98 因此把问题从「引擎」移到「内存系统」：
让 CP 用**已经证明能落地**的那条路径（fence 自己的 `CP_EVENT_WRITE7`）往
surface 的 IOVA 写一个哨兵 `0x5A5A5A5A`，看这块页收不收 GPU 的写。

### 69.73.1 fix98：CP 哨兵证明了「内存系统没问题，引擎才是问题」

fix98 让 ring 的 fence 包里多带一条 `CP_EVENT_WRITE7`，编码与 fence 自己那条
**逐字节相同**，只把目标换成最大 import（swap surface）的 IOVA、值换成
`0x5A5A5A5A`。这样写进那块页的路径就是「已经证明能落地」的那条。

`logs/vendor_boot_gpu_fix98-logs/muyu-6b702b2d`（177 s）：

```
MUYU IMW n=4 | i0 nz=1/64 w0=5a5a5a5a b=2098000 p=0000000934c71000 iova=0000000101d36000 | i1 nz=0/64 …
Z n2 nz1/64 w0=5a5a5a5a wl=00000000 f1
```

两个**互相独立**的读者（内核按 mapping 的物理页直接读 DRAM、plugin 用自己的
CPU 视图读）都读到 `0x5A5A5A5A`。这一条同时锁死四件事：

1. IOVA → 物理页的映射是**对的**（否则写不到我们采样的那块页）；
2. 该页对 GPU 的写**是允许的**（`perm=3` 不是纸面值）；
3. GPU → DRAM 的写通路**是通的**（NC 映射下直达）；
4. `nz` 从 `0` 变成 `1`，说明采样点确实覆盖到了 offset 0。

因此：「GPU 的写不落 DRAM」这个说法必须收窄为
**「3D/blit 引擎的写不落 DRAM」**——CP 侧一切正常。
配合 fix95（连设备自建 buffer 都写不进）与 fix83（`b1 e1`，RB 报告 pass 跑完），
剩下的可能只有两类：**光栅阶段把像素全剔除了**，或**引擎把像素写到了别处**。

## §69.74 fix99：UBWC / LRZ / binning 一次全排除，并与 `b0` 并列

`TU_DEBUG` 是 Turnip 侧唯一「零成本」的行为开关（env，不占 cmdline），而能解释
「pass 跑完、DRAM 零字节」的正好有三个：`noubwc`（压缩布局）、`nolrz`（低精度 Z
把像素全剔掉）、`nobin`（根本不进 binning）。用 `sheng-tum=` 的尾字符携带（`4`）：

```
setenv("TU_DEBUG","nocb,noubwc,nolrz,nobin",1)   // sheng-tum=sw4
```

`logs/vendor_boot_gpu_fix99-logs/muyu-6745e2db`（143 s，无 hang）：

```
R g0 a0 s20 e0 t0 u0 x1 m1 d1 k0/0/75329       ← 20 个 SYSMEM pass，没有 GMEM/autotune
z1 n20 w3200x2136 d3 m2 f26 k6835 c2 x0 b1 e1 v98
MUYU IMW n=4 | i0 nz=1/64 w0=5a5a5a5a b=2098000 p=… iova=0000000101933000
Z n2 nz1/64 w0=5a5a5a5a wl=00000000 f1
W a1 e5 r0 s0 b2 t3200x2136 m p0/00000000 c0/00000000 R5
```

哨兵仍在（`w0=5a5a5a5a`）、pass 仍 `b1 e1`、`d3` 个 draw、整块覆盖，
但 `nz` 依然是 `1/64`（只有哨兵那一个点）。⇒ **压缩、LRZ、binning 三者都不是原因**。

### 69.74.1 同一轮里的另一条线索：shadow 的回写从未被规划

`L` 行（`nmb_device_texture_counts`，7 个字）在这一轮是：

```
L i20 p18 r2/0 s2 c13 b0 t0
```

字段依次是 `host_textures_in_place=20`、`pitched_host_images=18`、
`shadows_refused_pitch=2 / refused_image=0`、`shadowed_textures=2`、
`shadow_copies=13`、`shadow_texels_copied_back=0`、`shadows_render_target=0`。

`shadow_texels_copied_back` 是在**规划时**由 `shadow_copies_planned(n, texels_in, texels_back)`
求和的（`work_counts.rs:2205`，`texels_back` 只对 `Step::Back` 非零，`shadow.rs:1189`），
所以 `b0` 不是「dispatch 走了另一条路」，而是**一次回写都没有被计划过**：
13 次 copy 全是 `Into`（host 页 → shadow），没有一次 `Back`。

这和 fix95 的结论**方向相反但互补**：如果 surface 走 shadow，渲染会落进私有镜像，
只有 `Step::Back` 才能把像素送回 host 页；而运行时认为「没有人会读这块 truth」，
就不会计划回写。也就是说这台机器上 `b3a8` surface 的像素**有一条完整的
「渲染 → 私有镜像 → 永不回写」的通路**，与 `nz=1/64` 完全吻合。
是不是这台 surface 就在那 `s2` 里，下一轮用 `t=`（`shadows_render_target`）
和 `i/s` 的分布一起看。

## §69.75 fix100：fence 前加 `CACHE_FLUSH7(0x32)`，仍然零像素

fix99 排除 UBWC/LRZ/binning 之后，唯一还能解释「CP 写落地、引擎写消失」的机制是**缓存**：
CP 的 memory write 直接出门，而 3D/blit 引擎的 store 停在 UCHE 里等 submission 结束时的
clean——那次 clean 就是本 ring 的 fence 事件 `0x31`（CACHE_CLEAN）。SQE 固件自己在换表前
发的是 `0x32`（CACHE_FLUSH7，见 `NativeAdrenoRing.cpp` 的 `RingCacheFlush7` 注释）。

fix100 让 fence 包在时间戳**之前**多发一个 `CP_EVENT_WRITE7`，事件号取 `0x32`，由
`sheng-tum=sw5` 的尾字符 `5` 打开（命令行已到 1016 B 上限，carrier 见 §69.73）。

`logs/vendor_boot_gpu_fix100-logs/muyu-6739920d`（173 s）：

```
MUYU IMW n=4 | i0 nz=1/64 w0=5a5a5a5a b=2098000 p=0000000939790000 iova=0000000110cd4000
Z n2 nz1/64 w0=5a5a5a5a wl=00000000 f1
```

哨兵仍在（`5a5a5a5a`，说明页面收 GPU 写），但 `nz` 仍是 `1/64`——**只有哨兵那一个点**。
⇒ 显式的 `0x32` flush 没有把任何引擎数据送进 DRAM。

⚠️ 这条实验的置信度要打折：`0x32` 在本驱动里是按 1 个 payload dword 发的
（`CP_EVENT_WRITE7(0x32, 1, …)`），而固件里那一发的 payload 布局没有第二个来源核对，
所以「0x32 本身没生效」与「payload 编错」两种解释没被分开。再往下走这条路要先解 `0x32`
的字段，暂不作为主线。

### 69.75.1 顺带确认：这块板**本来就是单 ring**

`ringForPriority()` 的第一行是 `if(!preemption() || preempt_dead_ || table_mode_!=2) return 0;`，
而 `preemption()` 等于 `preempt_on_ && ring_count_>1`；`ring_count_` 只有
`NativeAdrenoSubmit::setPreemption()` 会改成 2，**本树里除了 `tests/` 没有任何生产代码调用它**
（`grep -rn "Preemption" UniXNU/drivers/Adreno740` 只有 Submit.h/Submit.cpp 与 tests）。
`board.toml` 里也只剩 `preempt_record_bytes = "0x225000"`，没有 `sheng-gpu-preempt`。

⇒ 本板实际运行就是 sheng 文档里那个 **mode 0（一个 ring）**，`MUYU SYNC i=1` 的 `i` 是
提交里第几个 buffer，不是 ring 号。也就是说「sheng 用 mode0 反而进桌面」这条经验，
对本板**已经是既成事实**，不能再当成待做的开关。

## §69.76 fix101：回到「换表」——用 `sheng-gpu-ttbr=0` 复验 sheng 的记录

用户提示：sheng 的记录里，**换表那部分用 mode 0 反而进了桌面**。本树 §33 曾把 mode 0 判死
（fix39/40：CP 停在 RPTR 0x36 / WPTR 0x3d，CB0 FSR `0x80000402`，客户端 IB 在它没选中的表里）。
但那时客户端还没有现在这条「每连接一个 VM + 每次换表后 TLBIALL」的路径，且 fix95 之后
我们已经知道**引擎写一个字节都不落**——比翻译错误更坏。所以值得再验一次。

`tableMode()` 现在支持载体式开关（命令行 1000/1016 B，加不下 20 字节的独立参数）：

```
sheng-tum=swt0   -> table_mode 0（完全不写 TTBR0，GPU 一直跑内核表）
sheng-tum=swt1   -> 1（默认：CPU 在 GPU 空闲时写 TTBR0 + TLBIALL）
sheng-tum=swt2   -> 2（CP 在 ring 里换表）
```

载体不与现有读者冲突：Turnip 的 `tu_sheng_render_mode()` 只看 `at[10]`（`s`），
`stripThreadSync` 只认数字 `1..3`，`0x32` flush 只认 `5`，`t` + 一位数字对三者都不可见。
启动时会打一行 `MUYU TTBR mode=<n>` 作为判据。

判据：
- 打不出 `MUYU TTBR mode=0` ⇒ 载体没被解析，本轮作废；
- `mode=0` 且出现 SMMU 翻译故障 / `GFXFAIL st=e00002…` 在第一次客户端提交 ⇒ 复现 §33，mode 0 再次判死；
- `mode=0` 且 `IMW nz>1` 或 `Z nz>1` ⇒ 换表就是根因，立刻把「一个表」做成正式路径。

### 69.76.1 fix101 结果：mode 0 **确实能进桌面**，因为它一个像素都不写

`logs/vendor_boot_gpu_fix101-logs/muyu-675c99af`（kernel 143 s，`msgbuf=149721 B`，27 轮）：

```
MUYU TTBR mode=0                      ← 载体生效（sheng-tum=swt0）
（整轮 0 行 MUYU TSW）                ← 一次表切换都没有，按设计
Z n0
Z n2 nz0/64 w0=00000000 wl=00000000 f1
W a1 e2 r0 s0 b2 t3200x2136 m p0/00000000 c0/00000000 R2
MUYU IMW n=4 | i0 nz=0/64 w0=00000000 b=2098000 p=0000000922f28000 iova=0000000101d36000
HC hang ×4，GFXFAIL ×16
```

**最硬的一条**：`w0` 从 `5a5a5a5a` 变成 `00000000`。哨兵是 CP 用**已经证明能落地**的那条
`CP_EVENT_WRITE7` 写进 surface IOVA 的；mode 0 下它也不见了 ⇒ CP 现在跑在**内核表**里，
而 surface 的 IOVA 只存在于客户端表 ⇒ 这一次写落进「没有映射的地址」，被丢弃。

也就是说 mode 0 的机制是：**GPU 侧所有工作都停在客户端的表外面**。客户端 IB 取不到、
像素写不进、连哨兵都没了，随后 4 次 `HC hang`。§33（fix39/40 判 mode 0 为死路）**复验成立**。

### 69.76.2 为什么 sheng 的记录说 mode 0「反而进桌面」

这两件事不矛盾，而且互相印证：

- mode 0 下 GPU **一个字节都不写**（本轮实测），但**桌面照样出现**；
- 因为本板的桌面本来就是 `offline-legacy-*` 的 **CPU 拷贝合成**（§69.11：`Tier-1d 进桌面是
  CPU copy 渲染，GPU 没有接管显示`）。

⇒ 「进桌面」在这台板上**不是** GPU 渲染的证据，mode 0 的「成功」恰恰是「GPU 被完全旁路」
的结果。sheng 的记录因此不能当作 mode 0 可用的证据；换表必须留在 **mode 1**
（CPU 在 GPU 空闲时写 TTBR0 + TLBIALL），本轮结束后已回到默认。

反过来，这条也把判据钉死了：以后任何一轮，只有 `IMW nz` / `Z nz` 从 `1/64` 涨起来才算
「引擎开始写像素」；屏幕上有桌面、有图标、能滑动，都不算。

## §69.77 fix102：把「ROP 到底指向哪里」读出来——指向的正是那块读作零的页

到这里，内存侧的解释已经被 fix95..fix101 逐条关掉，但**渲染状态本身从来没被读过**。fix102 给 ring 加了一个
只读的寄存器窗口（`NativeAdrenoRing::regRead`，走的是 ring 自己的 MMIO），在每次提交前把最后一次 pass 留在
RB 里的状态读出来。pass 结束不会清寄存器，所以下一个提交到达时，寄存器里还是上一个 pass 的值：

```
MUYU RB0 ctl=00000080 fmt=0000004b pitch=200 base=0000000101d36000 gm=00000000 dep=0000000000000000 probe=0000000101d36000
MUYU RB0 ctl=00000080 fmt=0000004b pitch=200 base=0000000101d36000 gm=00000000 dep=0000000000000000 probe=0000000101d36000
...（五轮同上）
MUYU RB0 ctl=00000080 fmt=0000004b pitch=200 base=0000000110cd0000 gm=00000000 dep=0000000000000000 probe=0000000101d36000
```

字段含义（`a6xx.xml` 的 `RB_MRT` array，`offset=0x8820 stride=8`）：
`0x8820 CONTROL`、`0x8822 BUF_INFO`、`0x8823 PITCH(shr=6)`、`0x8825/0x8826 BASE`、`0x8827 BASE_GMEM`；`0x8875` 是 depth base。

| 读到的值 | 解码 | 结论 |
| --- | --- | --- |
| `base=0000000101d36000`，与 `probe` **逐字相同** | `MRT_BASE(0)` 就是那块 import surface 的 IOVA | ROP 的输出地址**完全正确** |
| `pitch=200` | `PITCH` 寄存器 shr=6 ⇒ 200×64 = **12800 B** | 正是 `w*4 = 3200×4`，与 `MUYU IMP` 报的行距一致 |
| `fmt=0000004b` | `COLOR_FORMAT=0x4b=75` = `FMT6_32_UINT`，`COLOR_TILE_MODE=0` = `TILE6_LINEAR` | 线性布局，4 B/texel，与该 surface 的 plane 0 吻合 |
| `ctl=00000080` | `COMPONENT_ENABLE=1`（只有 R 分量使能），blend/ROP 位全 0 | 单通道 32 位目标的一致状态 |
| `gm=00000000` | `BASE_GMEM` 未用 | 不是往 tile memory 里画 |
| `dep=0` | 没有深度目标 | 正常 |
| 最后一行 `base` 换成 `110cd000` | 两块 swap surface 轮流 | 寄存器跟着双缓冲在动，说明读的是**活**状态 |

所以 fix102 关掉了「画到别处去了」这条：**ROP 被正确地指向了那块读作零的页，行距和格式也对**。
（注意 `ctl` 的 `COMPONENT_ENABLE=1` 与 `fmt=32_UINT` 一起看，最后一次落在这个寄存器组上的目标是一个
单通道 32 位视图——即对 plane 0 按字处理的那条路径；无论它是合成还是拷贝，地址都是这块 surface。）

## §69.78 fix103：往 surface 里埋一个只有 CPU 写得出的值——**64/64 全部存活**

一个零字无法区分两件事：「引擎从没写过这一页」和「引擎往这一页写了个零」。而 fix86 以来的整条推理都压在这个
区别上。fix103 就把它分开：内核通过**和 DRAM 读完全相同的那条 uncached 映射**往 surface 中间写 64 个字
`0xA5A5A5A5`（`NativeAdrenoVm::poisonWord` + 板级新钩子 `native_adreno_dram_write32`），位置在 `iova+0x1000`
——在 fence 哨兵的 `+0` 之后，且在每一个 pass 的渲染区之内。然后每 8 次提交把这 64 个字按 uncached 读回来数。

`logs/vendor_boot_gpu_fix103-logs/muyu-6749bfd9`（175 s，43 轮）：

```
MUYU CAN arm i0 at=0000000101d37000 n=64 bytes=2098000 b=101d36000
MUYU CAN arm i1 at=0000000110a65000 n=64 bytes=160000 b=110a64000
MUYU CAN t=17  | i0 alive=64/64 v0=a5a5a5a5 | i1 alive=64/64 v0=a5a5a5a5
MUYU CAN t=25  | i0 alive=64/64 v0=a5a5a5a5 | i1 alive=64/64 v0=a5a5a5a5
...
MUYU CAN t=161 | i0 alive=64/64 v0=a5a5a5a5 | i1 alive=64/64 v0=a5a5a5a5
```

20 轮、约 160 次提交、上百个 pass（同轮 `R g0 a0 s44`、`z1 n47 … d3 … b1 e1`）之后，**两个 surface 的 canary
一个字都没被动过**。这不是「清成了黑色」——清成 0 也会把 canary 抹掉；这是**引擎根本没有向这些页写过任何东西**。

### 69.78.1 顺带排除总线 halt

同一轮把 GBIF 的 halt 状态也读了（`busHalt(gx_off)` 会置 `RBBM_GBIF_HALT`，而它在本树里**从不清除该位**——
`NativeAdrenoRing::busHalt` 结尾只写 `GBIF_HALT=0`，不像 msm 的 `a6xx_bus_clear_pending_transactions` 那样
最后再清一次 `RBBM_GBIF_HALT`。如果它被调用过，所有 GX 客户端的交易都会被丢掉而 CP 自己的照常——正好是这个形状）：

```
MUYU BUS rbcntl=03000000 mode=0 loc=0 halt=0 ack=3 gxhalt=0
```

`halt=0 / gxhalt=0`，`mode=0`（RENDERING_PASS）。**没有 halt**，而且本树里 `busHalt()` 根本没有调用点
（只有 `drainBus()`，它只碰 `GBIF_HALT` 且会清回 0）。这条也排除了。

### 69.78.2 现在的形状

同一块页、同一次 run：

| 写入者 | 是否落地 | 证据 |
| --- | --- | --- |
| CP（`CP_EVENT_WRITE7` 写 u32） | **落地** | fix98/99/100/102/103 的 `w0=5a5a5a5a`，两个独立读者一致 |
| CPU（uncached 写） | **落地** | fix103 的 `alive=64/64`，同一路径读回 |
| 3D/blit 引擎（clear、draw、blit fill） | **全部不落地** | fix103 canary 全存活、fix95 `p=0` |

而 fix102 证明 ROP 的地址、行距、格式、GMEM 都没有错，fix99 证明 UBWC/LRZ/binning 都不是原因，
fix96/97 证明 BV/BR 线程在本板不可用、fix101 证明 mode 0 是「所有工作都在客户端表外面」。

⇒ 收敛到最后一处：**引擎的 store 在离开引擎之前的那一级被吞掉了**。sysmem 渲染目标的所有 store 都要经过
**color cache（CCU）**，只有 `CCU_CLEAN_COLOR`(`0x21`) 能把它推进内存；而本 ring 的 fence 事件是
`CACHE_CLEAN`(`0x31`)，清的是 UCHE，**不碰 CCU**。fix104 就沿这条走：既统计流里到底有没有 `0x21`，
也在 fence 里亲手补一个（`sheng-tum=swc`）。

## §69.79 fix104：CCU clean 本来就在流里（275 次），canary 还是全活

沿 §69.78.2 的推理，fix104 做了两件事：**统计** `CP_EVENT_WRITE7` 到底带了哪些事件，以及**亲手补一个**
`CCU_CLEAN_COLOR`。事件码直方图在走包时按 `payload[0] & 0x3f` 记账（`native_adreno_event_code[64]`）。

`logs/vendor_boot_gpu_fix104-logs/muyu-674ad964`（174 s，42 轮）：

```
MUYU EVT c18=241 c19=311 c1a=35 c21=275 c26=33 c31=173 c32=0 c33=323 c3c=0
MUYU CAN t=153 | i0 alive=64/64 v0=a5a5a5a5 | i1 alive=64/64 v0=a5a5a5a5
```

- `c21=275`：`CCU_CLEAN_COLOR` 在本轮**本来就有 275 次**（我们的 fence 每提交加一次，所以其中一部分是我们加的），
  另外 `c1a=35`（`CCU_RESOLVE_CLEAN`）、`c19=311`（`CCU_INVALIDATE_COLOR`）、`c31=173`（`CACHE_CLEAN`）、
  `c33=323`（`CACHE_INVALIDATE7`）。**流里该有的清理事件一个不缺。**
- 结果 canary 依旧 `alive=64/64`。

⇒ 「color cache 没清」这条**排除**。清理事件发了、CP 也执行了（否则 fence 不会前进），像素还是没进内存。

### 69.79.1 顺带确认的两件事

1. `z1` 行的 `v` 字段是**第一颜色附件的 VkFormat**，本轮一直是 `v98` = `VK_FORMAT_R32_UINT`，`c2` = 两个颜色附件。
   这与 fix102 从 RB 读到的 `fmt=0x4b (FMT6_32_UINT)`、`pitch=200 (×64=12800)` **逐项吻合**：
   这个 pass 就是把 `b3a8` 双平面 surface 的 plane 0 当**32 位字**画。状态与意图完全一致。
2. `MUYU SMMUFAULT … gfsr=0`：SMMU 的全局故障寄存器始终为 0。SMMU 的 `sCR0` 在本板被校验为
   `(scr0&0x407)==0x406`，其中 bit10 就是 `USFCFG`（不匹配的 stream **报故障**而不是静默丢弃），
   而 `GFSR=0` ⇒ **不存在「GX 侧的 StreamID 没被路由、交易被静默丢弃」这条**。SMMU 只有一条路由
   （`sid0=1`，第二条是 `UINT16_MAX`）在本板也够用，因为 GPU 的全部 master 走同一个 stream。

## §69.80 fix105：FS 被允许写、导出两个 MRT、全分量使能、格式一致——还是没有字节

fix102 读了画到哪，fix104 读了有没有清理事件，那么剩下的就是「ROP 有没有被交给片元」。fix105 把这一侧的寄存器一次读全：

```
MUYU PS rctl=00000000 fsdis=0 out=00000000 mrt=00000002 mask=0000000f rt0=15 sp0=0000024b rbmode=00000010
```

| 寄存器 | 值 | 含义 |
| --- | --- | --- |
| `0x8801 RB_RENDER_CNTL` | `0`，`FS_DISABLE=0` | **FS 没有被关掉**（binning pass 才会置这一位） |
| `0x880b RB_PS_OUTPUT_CNTL` | `0` | 不写 Z / sample mask / stencilref——纯颜色 pass，正常 |
| `0x880c RB_PS_MRT_CNTL` | `2` | **向 2 个 MRT 导出**，与 `z1 c2` 一致 |
| `0x880d RB_PS_OUTPUT_MASK` | `0xf`，`rt0=15` | **RT0 四个分量全部使能** |
| `0xa996 SP_PS_MRT(0)` | `0x24b` = `COLOR_FORMAT 0x4b (FMT6_32_UINT)` + `COLOR_UINT(bit9)` | 导出格式与目标格式逐位一致 |
| `0x8811 RB_MODE_CNTL` | `0x10` | — |
| `RB_MRT_CONTROL` | `0x80` | `COMPONENT_ENABLE=1`：单通道 32 位目标，与 `R32_UINT` 一致 |

同一轮 `z1` 也补全了：`mode=2` = **sysmem（非 autotuner 选的）**，`flags=26` = `2|8|16` = **线性 + 已加载 + 会 store**，
`v98` = `VK_FORMAT_R32_UINT`，`c2` = 两个颜色附件。

也就是说：**pass 是 sysmem、线性、load+store；FS 开着，导出 2 个 MRT，RT0 四分量全开，导出格式就是 32 位无符号；
目标地址、行距、格式全部正确。** 而 fix103 埋在同一块 surface 里的 canary 依旧 `alive=64/64`。

到这里，凡是「配置错了」能解释的东西都已经被逐个读出来并排除了。剩下的只有两类，而且必须靠**别的**证据分开：
1. **draw 根本没产生片元**（顶点/图元没到 ROP），于是 load+store 的 store 因为「没有任何修改」被硬件省掉；
2. **ROP 的 store 通路本身是死的**（任何写都出不去）。

`tu_pass_rec` 里正好有 `verts`（该 pass 非间接 draw 的顶点数）和 `fs_instrs`/`fs_regs`/`fs_waves`，
fix106 把它们打进 `z1`，同时读 `RB_CCU_CNTL(0x8e07)` / `RB_CCU_CACHE_CNTL(0x88e5)`——如果画进去了，`verts>0`
且 FS 有指令；如果 CCU 的颜色缓存尺寸/偏移指向 GMEM 孔径，那 store 的去向也就有了名字。

## §69.81 fix106 判读：CCU 是 sysmem 口味，`vt/fi` 只是驱动侧记账——而 `b1 e1` 是硬的

`logs/vendor_boot_gpu_fix106-logs/muyu-673b9bb1`（kernel 141 s，msgbuf 152210 B，26 轮，`hangs=0`、
`MUYU TTBR mode=1`）。这一轮同时做三件事：读 CCU 的两个寄存器、把 `tu_pass_rec` 的 `verts`/`fs_*` 打进 `z1`、
把 `RB_BUFFER_CNTL` 之外的所有 RB 状态继续读一遍。

### 69.81.1 CCU 不是「指向 GMEM」，它就是 turnip 的 sysmem 分支

```
MUYU CCU cntl=00000068 cache=60000000 csize=0 coff=c0 dsize=0 doff=0
```

先纠正一处**我自己读错**的字段语义：`a6xx_ccu_cache_size` 的枚举是**反的**——

| 值 | 名字 |
| --- | --- |
| `0` | `CCU_CACHE_SIZE_FULL` |
| `1` | `CCU_CACHE_SIZE_HALF` |
| `2` | `CCU_CACHE_SIZE_QUARTER` |
| `3` | `CCU_CACHE_SIZE_EIGHTH`（depth 侧 3 = THREE_QUARTER） |

所以 `csize=0` 是 **FULL**，不是「零尺寸」。对照 `tu_cmd_buffer.cc:664-676`（`RB_CCU_CACHE_CNTL` 发射处）：

```c
const struct fd6_gmem_config *cfg = gmem ? &dev->physical_device->config_gmem
                                          : &dev->physical_device->config_sysmem;
uint32_t color_offset = cfg->color_ccu_offset;      /* sysmem 配置里这一项 = 768 KiB = 0xC0000 */
...
.color_cache_size = color_cache_size,               /* !gmem ⇒ CCU_CACHE_SIZE_FULL */
.color_offset = color_offset,                       /* 0xC0 << 12 = 0xC0000 */
```

`coff=c0` 正是 `0xC0 << 12 = 786432 = 768 KiB`，与 `config_sysmem.color_ccu_offset` 一致；
`cntl=0x68` 解出 `CONCURRENT_RESOLVE_MODE=2`、`CONCURRENT_UNRESOLVE_MODE=FULL`（`tu6_init_static_regs`
给 a7xx 的静态值，与 `RB_CMP_DBG_ECO_CNTL`/`UCHE_CMDQ_CONFIG` 一样是 msm 口径）。

⇒ **CCU 侧全部正确**：这是 turnip 在**sysmem** pass 上会发出的那一组值，不是 GMEM 口味；
再叠加 fix104 的 `c21=275`（`CCU_CLEAN_COLOR` 本来就在流里），§69.78.2 那条「CCU 没清」彻底关闭。

### 69.81.2 `vt/fi/fw/fr` 不是 GPU 证据——它们是录制期的计数器

`z1` 新增四个字段后能读到的确实是「有顶点、FS 有指令」：

```
z1 n1  w1x1      d4 m2 f2  k0    c1 x0 b0 e0 v37 vt6  fi1  fw16 fr-1
z1 n10 w3200x2136 d2 m2 f16 k6835 c2 x0 b1 e1 v101 vt12 fi938 ...
z1 n13 w3200x2136 d3 m2 f26 k6835 c2 x0 b1 e1 v98  vt18 fi748 ...
```

但回到 mesa 源码，这四个数全部是**驱动在录制命令流时算出来的**，与 GPU 执行无关：

```c
if (tu_pass_trace_on()) {
   rp->fs_instrs = info->instrs_count;   /* ir3 编译产物：指令数 */
   rp->fs_waves  = info->max_waves;      /* ir3 编译产物：每核波数 */
   rp->fs_regs   = info->max_reg;        /* ir3 编译产物：最高 GPR */
   rp->verts += draw_count;              /* 录制时把 draw 的顶点数累加 */
}
```

`draw_count` 是 `tu_emit_draw` 拿到的顶点/索引数，`ir3_info` 是编译期统计。**它们证明不了「GPU 跑了这些 draw」**，
所以 fix106 **没有**分开 §69.80 末尾提的那两类。这一条要写死，避免下一轮再把它们当证据。

### 69.81.3 同一行里的 `b1 e1` 才是硬的：pass 边界的 `RB_DONE_TS` 是 GPU 写进去的

`b/e` 是 `gpu_begin/gpu_end`。`tu_pass_ring_read()`（`tu_cmd_buffer.cc:4267-4275`）只有在**时间戳 BO 里两个 64 位字都非零**时才填它们：

```c
const volatile uint64_t *words = ... bo->map + (s % 512) * 2;
const uint64_t begin = words[0], end = words[1];
if (begin && end) { rec.gpu_begin = ...; rec.gpu_end = ...; }
```

而那两个字是流里 `CP_EVENT_WRITE7(.event = RB_DONE_TS, .write_src = EV_WRITE_ALWAYSON, .write_dst = EV_DST_RAM)`
（或 `CP_EVENT_WRITE` + `TIMESTAMP`）写进去的——**只有 GPU 执行到那一条才会变非零**。

`b1 e1`（n10、n13 两个 3200×2136 的 pass）⇒ **pass 的 begin/end 事件对都执行了**，即这段命令流确实被 CP 跑到了。
这是 fix98 之后第一次拿到「流被执行」的正向证据（此前 `w0=5a5a5a5a` 只能证明 CP 的 memory write 落地）。

⇒ §69.80 的两类收敛为：**流执行了（b1 e1），但像素没有**。而 fix95 的 CP_BLIT fill（不经过顶点/片元）
同样一个字节都没落地 ⇒ 「draw 没产生片元」解释不了 blit，**store 通路本身是死的**是唯一还站得住的一类。

### 69.81.4 顺手纠正 §69.77 的 pitch 解码

`RB_MRT(i).PITCH` 是 `shr=6` 的字段，读到 `pitch=200` 要 **`0x200 << 6 = 32768`**，不是 §69.77 表里写的 `12800`。
（同一个换算在 mesa 自己的 a750 参考 trace 里可以校准：`RB_MRT[0].PITCH: 2048` 对应 raw `0x20`，`0x20<<6 = 2048`。）

于是 fix102 的那张表要改一行：ROP 被告知的行距是 **32 KiB**，而这块 3200 宽、4 B/texel 的 plane 0 真实行距是
12800。这不改变结论（行 0 仍然覆盖 `base+0..32767`，canary 在 `+0x1000` 无论如何都该被写掉），但它是一个
**必须记下来的异常**：32 KiB 的行距只对应 8192 宽的 4 B/texel 目标。

### 69.81.5 剩下的两条，和 fix107 的选择

现在的事实表：

| 事实 | 证据 |
| --- | --- |
| 命令流被执行 | `b1 e1`（GPU 写的时间戳） |
| CP 的 memory write 落地 | `w0=5a5a5a5a`、`MUYU 10BIT S3/S4` |
| ROP 地址/格式/分量/FS 导出/CCU 全部正确 | fix102/104/105/106 |
| 引擎的 store 一个字节都没落地 | fix103 canary `alive=64/64`、fix95 `p=0` |

「地址对了、格式对了、清理发了、流也跑了，而 store 不落地」只剩一个还没被读过的东西：
**ROP 到底被允许往系统内存写吗**——在 a7xx 上这不是 `RB_CNTL` 的字段（`BUFFERS_LOCATION` 只声明给 A6XX），
而是 **`RB_BUFFER_CNTL(0x8812)` 的 `RTn_SYSMEM` 位**。fix102 那行 `loc=0` 是照 a6xx 位域解出来的，在 a7xx 上
从来没有意义。fix107 就补这一条，同时读 `CP_PROTECT_STATUS`（`hw_init` 打开了
`ACCESS_FAULT_ON_VIOL_EN`，a730 的 46 段保护覆盖 `0x8630..0x8d6b` 整个 RB 块——被 CP 丢掉的写不会在别处留痕）。

## §69.82 fix107：`RB_BUFFER_CNTL` 与 `CP_PROTECT_STATUS`

全只读，不改任何行为。`NativeAdrenoSubmit.cpp` 每次提交（每 8 次一次）加一行：

```
MUYU CPRT st=%08x <read|write> addr=%05x hw=%08x pcntl=%x bufcntl=%08x rt0sys=%u clr=%08x
```

| 寄存器 | 偏移 | 期望/含义 |
| --- | --- | --- |
| `CP_PROTECT_STATUS` | `0x0824` | bit20 = 方向（1=读），bits17:0 = 被拒的寄存器地址。非零就点名了「谁被 CP 扔掉」 |
| `CP_HW_FAULT` | `0x0821` | CP 侧硬故障码 |
| `CP_PROTECT_CNTL` | `0x084f` | 应为 `1\|2\|8`（PROT_EN、FAULT_ON_VIOL、LAST_SPAN_INF），与 msm 一致 |
| `RB_BUFFER_CNTL` | `0x8812` | bit `2+n` = `RTn_SYSMEM`。sysmem pass 应为 `0x3ff`（`tu7_emit_sysmem_render_begin_regs`；mesa 的 a750 参考 trace 里就是这个值） |
| `RB_CLEAR_TARGET` | `0x88e4` | `CLEAR_MODE`，与上面成对（`tu7_emit_tile/sysmem_render_begin_regs`） |

三种判读：

1. `bufcntl=0`（或 `rt0sys=0`）⇒ ROP 仍在 tile memory 口径，`MRT_BASE` 里的 `0x101d36000` 被当成 GMEM 偏移，
   落在 `0x1000000..0x12fffff` 之外的写**被丢弃**：没有 SMMU 交易、没有总线错误、没有 fault 报告，
   而 CP 自己的写照常出门——正是现在的形状。下一步就是找「这一条写为什么没生效」：
   `st` 非零说明是 CP 的 `CP_PROTECT` 扔的（`addr=08812`），`st=0` 说明 turnip 那条路没走到。
2. `bufcntl=3ff` 且 `st=0` ⇒ 「sysmem 允许、保护没拦、流也跑了，store 仍然不出门」，
   那就把怀疑整体移到 **UCHE → GBIF(GX)** 这一级（hw_init 的 `UCHE_TRAP_BASE`/`WRITE_THRU_BASE` 与 msm
   的 `uche_trap_base=0x1fffffffff000` 逐字一致，但**读回**从未做过）。
3. `st` 非零且 `addr` 是别的地址 ⇒ 直接拿到「被保护寄存器」的名单，逐个对 `a730_protect` 表。

顺带 `GpuPowerProbe.cpp` 加一行一次性的 SMMU 路由快照（读 `sSMR(0..7)`/`sS2CR(0..7)` 与 `sCR0/1/2`）：

```
MUYU S2CR scr0=... scr1=... scr2=... | 0 smr=... s2cr=... | ...
```

用途是把「GX 侧的 stream 被静默旁路」这条彻底变成事实：本板固件下 BYPASS 的 S2CR 读回是 FAULT
（`SmmuBypass.h`），而未匹配的 stream 会报故障而不是穿过去——但 `TRANSLATE` 到一个**不属于本 bank 的
CBAR/ASID** 同样不会报错，只是把交易送到别处。只有把 SMR/S2CR 打出来，这条才算关掉。

## §69.83 fix107 判读：`RB_BUFFER_CNTL=0x3ff`，CP 什么都没拦——嫌疑整体移到 UCHE

`logs/vendor_boot_gpu_fix107-logs/muyu-67596edd`（kernel 174 s，42 轮，`hangs=0`）。21 行 `MUYU CPRT`：

```
MUYU CPRT st=00000000 write addr=00000 hw=00000000 pcntl=b bufcntl=00000000 rt0sys=0 clr=00000000
MUYU CPRT st=00000000 write addr=00000 hw=00000000 pcntl=b bufcntl=00000000 rt0sys=0 clr=00000000
MUYU CPRT st=00000000 write addr=00000 hw=00000000 pcntl=b bufcntl=000003ff rt0sys=1 clr=00000000
...（其后 19 行全部 bufcntl=000003ff rt0sys=1）
```

| 读到的值 | 结论 |
| --- | --- |
| `bufcntl=000003ff`、`rt0sys=1` | **ROP 确实在 sysmem 模式**：`Z/S/RT0..RT7_SYSMEM` 十位全开，正是 `tu7_emit_sysmem_render_begin_regs` 的值，也是 mesa a750 参考 trace 里那个 `0x3ff` |
| 前两行 `bufcntl=0` | 第一个 sysmem pass 还没开始时它是 0，随后被写上——**读的是活状态**，不是常量 |
| `st=00000000`、`hw=00000000` | `CP_PROTECT_STATUS` 空：`ACCESS_FAULT_ON_VIOL_EN` 打开的情况下，**没有任何一条寄存器写被 CP 拒绝** |
| `pcntl=b` | `0b1011` = `ACCESS_PROT_EN\|ACCESS_FAULT_ON_VIOL_EN\|LAST_SPAN_INF_RANGE`，与 `a6xx_gpu.c:734` 逐位一致 |
| `clr=00000000` | `RB_CLEAR_TARGET.CLEAR_MODE` = 0 |

⇒ §69.82 的第 1 种判读**排除**。于是「ROP 允许往系统内存写、地址/格式/分量/CCU/清理事件/流执行全对，
canary 仍然 64/64」这句话里，**再也没有一个渲染状态能再改**。

### 69.83.1 这轮看到的另一件事：两块 34 MB 轮换 surface

```
MUYU CAN arm i0 at=0000000101d37000 n=64 bytes=2098000 b=101d36000
MUYU CAN arm i1 at=0000000110a65000 n=64 bytes=160000 b=110a64000
MUYU IMW n=4 | i0 nz=1/64 w0=5a5a5a5a b=2098000 p=0000000949660000 iova=0000000110cd4000
             | i1 nz=1/64 w0=5a5a5a5a b=2098000 p=00000009247ba000 iova=0000000101d36000
             | i2 nz=62/64 w0=ff69a596 b=1a13000 p=00000000e3940000 iova=000000011fb12000
             | i3 nz=7/64  w0=00000000 b=160000  p=000000092359e000 iova=0000000110a64000
```

- **两块 34 MB（`0x2098000`）的 swap surface 在轮换**：`0x110cd4000` 与 `0x101d36000`，两块都只有 `+0` 那个
  CP 哨兵 `0x5a5a5a5a`，其余全零。fix102 里 `base` 在两者之间跳，就是双缓冲。
- `0x11fb12000`（`0x1a13000` = 27,340,800 = `3200×2136×4`）物理地址 `0xe3940000` 与 fix106 的 i2 **同一个物理页**，
  内容是**真图像**（`w0=ff69a596`、`nz=62/64`）。它是唯一有内容的 surface，且跨轮稳定——这是 CPU 侧合成/壁纸的产物。
- 本轮的 `probe` 换成了 `0x110cd4000`（`sampleImports` 的 `s[0]` 顺序变了），但 canary 仍在
  `0x101d36000+0x1000`，而 `RB0 base=0x101d36000`——**canary 依旧埋在 ROP 的落点里**，`alive=64/64` 仍然有效。

### 69.83.2 现在的链路图

| 环节 | 状态 | 证据 |
| --- | --- | --- |
| CP 取指/执行 | 正常 | `b1 e1`、PKTS 直方图、fence 前进 |
| ROP 地址/格式/行距/分量/sysmem | 正常 | fix102/105/107 |
| CCU 清理事件 | 正常 | fix104 `c21=275` |
| ROP store → UCHE | **未知（唯一没读过的一段）** | — |
| UCHE → GBIF(GX) → SMMU → DRAM | **未知** | SMMU `GFSR=0`、`fsr=0x400` 只是 AArch64 格式位 |
| CP 自己的写 → DRAM | 正常 | `w0=5a5a5a5a` |

fix108 就补这一段：`RBBM_INT_0_STATUS`（`UCHE_TRAP_INTR` bit25、`UCHE_OOB_ACCESS` bit24、`RBBM_HANG_DETECT` bit23）、
`RBBM_STATUS`（RB/CCU/UCHE/SP/HLSQ busy 位）、以及 **UCHE 的 64 位 `TRAP_BASE`/`WRITE_THRU_BASE`/
`GMEM_RANGE_MIN/MAX` 回读**。最锋利的一条是 `TRAP_BASE`：hw_init 写的是 `0x1fffffffff000`，
**如果 64 位写只落了低 32 位，它就变成 `0xfff000`**——而本 app 的每一个 IOVA（`0x101d36000`）都在它之上，
于是一次都不落地、没有 SMMU 交易、没有总线错误、没有故障位，与观测完全同形。

## §69.84 fix108 判读：UCHE 的配置全是 msm 口径，但有一个寄存器**自己变成了 0**

`logs/vendor_boot_gpu_fix108-logs/muyu-672bfd89`（kernel 175 s，43 轮，202 s 自复位，`hangs=1`）。

```
MUYU UCHE mode=00600000 trap=0001fffffffff000 thru=0001fffffffff000 gmin=0000000001000000 gmax=00000000012ff000 ways=00800000 pf=00000081 gx=010240e0 cmdq=0006690e
MUYU UCHE mode=00600000 trap=0001fffffffff000 thru=0001fffffffff000 gmin=0000000001000000 gmax=00000000012ff000 ways=00000000 pf=00000081 gx=010240e0 cmdq=0006690e   ← 之后每一行都是 ways=0
```

| 寄存器 | 读回 | 结论 |
| --- | --- | --- |
| `UCHE_TRAP_BASE` / `WRITE_THRU_BASE` | `0001fffffffff000` | **64 位写没问题**，§69.83 那条「只落了低半」排除 |
| `UCHE_MODE_CNTL` | `0x00600000` = `3<<21` | 与 `hbb_lo=3` 一致 |
| `UCHE_GMEM_RANGE_MIN/MAX` | `0x1000000` / `0x12ff000` | 低 12 位被硬件清零（我们写的是 `0x12fffff`），范围仍是 3 MiB 减一页 |
| `UCHE_CLIENT_PF` | `0x81` | 与 `BIT(7)\|1` 一致 |
| `UCHE_GBIF_GX_CONFIG` | `0x010240e0` | 与 msm a7xx 的 `0x10240e0` 逐位一致 |
| `UCHE_CMDQ_CONFIG` | `0x0006690e` | `6<<16\|6<<12\|9<<8\|BIT(3)\|BIT(2)\|2`，逐位一致 |
| **`UCHE_CACHE_WAYS` (0x0e17)** | 第一次 `0x00800000`，**之后全是 `0`** | **异常** |

```
MUYU ERR ist=00000000 trap=0 oob=0 hang=0 gpc=0 cphw=0 ccufl=0 | busy=00000001 rb=0 ccu=0 uche=0 sp=0 hlsq=0 gpu=0 | s1=0 s2=0 s3=00000000 stallfault=0
```

`RBBM_INT_0_STATUS` 全 0：**没有** `UCHE_TRAP_INTR`（bit25）、没有 `UCHE_OOB_ACCESS`、没有 `RBBM_HANG_DETECT`、
没有 `CP_HW_ERROR`、`RBBM_STATUS3.SMMU_STALLED_ON_FAULT=0`。UCHE 这一跳没有留下任何错误。

### 69.84.1 `ways=0x800000 → 0` 这一条为什么重要

`UCHE_CACHE_WAYS` 在 `NativeAdrenoPwrupRegs.h` 的 16 条上电寄存器表里（`0x00000e17`），也就是说
**GX 域真上电时它会被恢复**。它在 133.6 s 还是对的，135.8 s 起变 0，而同表里的
`TRAP_BASE`/`WRITE_THRU_BASE`/`GMEM_RANGE`/`MODE_CNTL` 全部保持正确值——**只有一个寄存器丢值**。

这有两种解释，fix109 一次性分开：

1. **读它就会清零**（某些 `usage=init` 寄存器是写侧触发器）——那么第一次读到 `0x800000`、第二次开始读到 0 是正常的，
   与上电无关，`ways=0` 全程无害。
2. **GX 域经历了一次掉电/上电**，而上电寄存器表没有真正把它恢复回去（或者表里打包的是全 0 的旧值）。
   如果是这一条，那么掉电这件事本身就能解释全部现象：**CP 在 CX 侧（取指、执行、`CP_EVENT_WRITE7` 落地、
   `RB_DONE_TS` 时间戳）一切正常，而 GX 侧（RB / ROP / UCHE）整块没在跑**——AHB 从属接口留在 CX，
   所以 fix102..fix107 读到的每一个渲染状态都是"活的、连贯的"，但背后没有任何数据通路在工作。

第 2 条与 §69.83.2 的链路图完全吻合：**已经证明能工作的全在 CX 侧，从来没有写过一个字节的全在 GX 侧。**
`MuyuGpuPowerProbe.cpp:1051` 早就记过 `GMU_SPTPRAC_PWR_CLK_STATUS=0x2`（bit1 `GX_HM_CLK_OFF`），
而 `a7xx_gmu_gx_is_on()` 判的就是这一位。

## §69.85 fix109：在**帧失败的那一刻**读 GX 域状态

前面所有的「GX 是开的」都是**冷启动时**的读数。真正要回答的是：跑到第 130 秒、一帧一帧失败的时候，GX 还在不在。
`GpuPowerProbe.cpp` 里那个 SMMU 故障报点每次 GFXFAIL 都被调用（fix106 一轮 12 次），就在那里加一行：

```
MUYU GX st=%08x on=%u gdscoff=%u clkoff=%u sptpclk=%08x sptppwr=%08x rbbm=%08x rb=%u ccu=%u uche=%u sp=%u gpu=%u s3=%08x stall=%u ways=%08x/%08x
```

| 读的东西 | 地址 | 说明 |
| --- | --- | --- |
| `GMU_SPTPRAC_PWR_CLK_STATUS` | GMU `0x1f8d0` | bit0 `GX_HM_GDSC_POWER_OFF`、bit1 `GX_HM_CLK_OFF`；两位都 0 才算 GX 在（`a7xx_gmu_gx_is_on`） |
| `GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL` | GMU `0x1a880` | CPU 侧对 GX 时钟的控制字 |
| `GMU_GX_SPTPRAC_POWER_CONTROL` | GMU `0x1a881` | `a6xx_sptprac_enable` 写 `0x778000` 的那一个 |
| `RBBM_STATUS` | GPU `0x0210` | `RB`(7)/`CCU`(8)/`UCHE`(17)/`SP`(18)/`GPU_BUSY_IGN_AHB`(23) 忙位 |
| `RBBM_STATUS3` | GPU `0x0213` | bit24 `SMMU_STALLED_ON_FAULT` |
| `UCHE_CACHE_WAYS` ×2 | GPU `0x0e17` | **连读两次**：两次都 0 ⇒ 真的被清；第一次非 0 ⇒ 读它自己会清零 |

判读表：

- `on=0`（或 `clkoff=1`）⇒ **GX 域在跑帧的时候是关的**，这才是「引擎一个字节都写不出来」的物理原因；
  下一步不是渲染，是**把 GX 钉住**（CPU 侧 SPTPRAC 控制 + 阻止 GMU 的空闲塌陷），并把上电寄存器表核对成 msm 的 16 条。
- `on=1` 且 `ways` 连读两次都 0 ⇒ 寄存器自己会清零，这条线索作废，回到「GX 在跑但 store 出不去」。
- `on=1` 且 `ways` 第一次 `0x800000` ⇒ 也是读清零，同上。
- `rb=1`/`ccu=1` 至少出现过一次 ⇒ RB/CCU 确实被时钟驱动过（`busy` 全 0 只是采样点在提交边界）。

## §69.86 fix109 判读：`on=1`，但这一行有一个哨兵漏洞

`logs/vendor_boot_gpu_fix109-logs/muyu-67517e06`（kernel 175 s，203 s 自复位，17 行）：

```
MUYU GX st=00000000 on=1 gdscoff=0 clkoff=0 sptpclk=00000000 sptppwr=00000000 rbbm=00000001 rb=0 ccu=0 uche=0 sp=0 gpu=0 s3=00000000 stall=0 ways=00000000/00000000
```

| 字段 | 读到的值 | 说明 |
| --- | --- | --- |
| `st` | `0` | 按 `a7xx_gmu_gx_is_on` 解：`GX_HM_GDSC_POWER_OFF`(bit0)、`GX_HM_CLK_OFF`(bit1) 都为 0 ⇒ GX 在 |
| `sptpclk`/`sptppwr` | `0` / `0` | `0x1a880`/`0x1a881`，两个都 0 —— **可疑** |
| `rbbm` | `00000001` | 只有 `CP_AHB_BUSY_CX_MASTER`，其余块忙位全 0 |
| `ways` | `0/0` | 连读两次都是 0 ⇒ **不是「读它自己会清零」**，是真的 0 |

**这一行的漏洞**：`st`/`sptpclk`/`sptppwr` 的变量初值是 0，而 `gColdDevice.io.read32` 失败时不会改它。
`gxOn()` 之所以可信，是因为它把初值设成 3。所以 `st=0` 完全可能是**读失败**，而 `sptpclk=sptppwr=0`
两个同表寄存器一起为 0，正是「GMU 窗口在冷启动之后已经不可访问」的样子。**fix109 因此没有证明任何事**，
fix111 把这三个初值改成 `0xdeadbeef`。

`ways=0/0` 则是可信的（`gpuRd()` 有 `0xdeadbeef` 哨兵，读到的是 0 而不是 `0xdeadbeef`）：
`UCHE_CACHE_WAYS` 确实停在 0，而同一张上电表里的其它 15 个寄存器都还是正确值。要么上电表打包时这一项就是 0，
要么它在某次 GX 域事件之后被清成了 0。**先记下来，不作为结论。**

## §69.87 fix110：把块忙位放在「提交的那一刻」采——采样窗口选错了

思路：`RBBM_STATUS(0x0210)` 的块忙位（RB 7、CCU 8、UCHE 17、SP 18、HLSQ 21）在被复位钳住或没有时钟时永远是 0，
而在正常执行时会被置起。fix109 是从**故障报点**读的（帧已经失败 2 秒），那和空闲 GPU 长得一模一样，什么也说明不了。
fix110 改成在 `NativeAdrenoRing::wait()` 的自旋里采样，理由是「那是唯一在 GPU 执行时自旋的地方」。

结果（`logs/vendor_boot_gpu_fix110-logs/muyu-674bea49`）：

```
MUYU BUSY n=7  or=00000000 rb=0 ccu=0 uche=0 sp=0 ...
MUYU BUSY n=3  or=00000000 ...
MUYU BUSY n=2  or=00000000 ...
MUYU BUSY n=11 or=00000000 ...
```

**`n` 只有 2–11**：这个自旋循环每次只转几圈就退出（`clock->idle` 在超时之前就放弃），而采样门是「每 64 圈一次」，
所以**一次都没采到**，`or=00000000` 是空值。这一轮什么也没测到——采样点选错了，不是结论。

正确的位置是 `writeWptr()`：`CP_RB_WPTR` 写完的那一瞬，正是 CP 立刻取走提交的那一刻。fix111 就挪到那里。

## §69.88 fix111：提交那一瞬的 24 次块忙位采样

`writeWptr()` 里每 8 次提交做一串采样：24 次 `RBBM_STATUS`，间隔 25 µs（共 600 µs），OR 进
`native_adreno_busy_or`，由提交侧打出一行 `MUYU BUSY`。顺带把 fix109 那三个 GMU 读改成 `0xdeadbeef` 初值。

判读：

- `rb=1` 或 `ccu=1` 出现过 ⇒ RB/CCU 真的被时钟驱动过，问题在 store 出不去，不在块没跑。
- 24 次采样全是 0、`or=0`、而且换了多个窗口都如此 ⇒ **GX 的块在 CP 交出工作时一次都没有忙过**：
  复位钳住或时钟未开，此时该查的是 `GPUCC_GPU_CC_GX_BCR`/`GFX3D_AON_BCR`/`GX_GDSCR` 与 `SMMU_STALLED_ON_FAULT`，
  而不是 turnip 的任何状态。

### 69.88.1 fix111 判读（`logs/vendor_boot_gpu_fix111-logs/muyu-6740ffc9`，kernel 390 s，150 轮）——**块是忙的**

```
MUYU BUSY n=1 or=00c00015 rb=0 ccu=0 uche=0 sp=0 ... gpu=1
MUYU BUSY n=1 or=00ef0df7 rb=1 ccu=1 uche=1 sp=1 tse=1 ras=1 hlsq=1 tpl1=1 vpc=1 vfd=0 gpu=1
MUYU BUSY n=1 or=00ef0df7 rb=1 ccu=1 uche=1 sp=1 tse=1 ras=1 hlsq=1 tpl1=1 vpc=1 vfd=0 gpu=1
```

（`n=0` 的那些行是这一窗没跑到 burst 的门，`or` 自然是空值，忽略。）

`or=00ef0df7` 逐位解：

| 位 | 名字 | 值 |
| --- | --- | --- |
| 0/1/2 | `CP_AHB_BUSY_CX_MASTER`/`CP_AHB_BUSY_CP_MASTER`/`CP_BUSY` | 1 |
| 4 | `VBIF_BUSY` | 1 |
| 5/6/7/8 | `TSE` / `RAS` / **`RB`** / **`CCU`** | 1 |
| 9 | **`A2D`**（2D 引擎，CP_BLIT 的执行者） | 1 |
| 10/11 | `LRZ` / `COM_DCOM` | 1 |
| 16/17/18/19 | **`VPC`** / **`UCHE`** / **`SP`** / `TPL1` | 1 |
| 21/22/23 | `HLSQ` / `GPU_BUSY_IGN_AHB_CP` / `GPU_BUSY_IGN_AHB` | 1 |
| 15 / 20 | `VFD` / `VSC` | **0** |

⇒ **整条 3D 数据通路（RB、CCU、UCHE、SP、TPL1、HLSQ、A2D）在 CP 交出工作的那一刻确实是忙的**，
而且同一批窗口里也出现过只有 `0x00c00015`（纯 CP）的行——说明这些位是**动态**的，不是恒定置位。
`VFD=0`、`VSC=0` 与 `MUYU PKTS … draw=0 blit=9300` 完全对上：**这个 app 的负载里没有顶点抓取，全是 2D/blit。**

于是「GX 域没上电 / 块被复位钳住」这条**排除**。§69.83.2 的表要改一行：

| 环节 | 状态 | 证据 |
| --- | --- | --- |
| ROP/A2D 的执行 | **在跑** | `or=00ef0df7` 的 RB/CCU/UCHE/A2D 忙位 |
| ROP/A2D 的 store 是否出得去 | **出不去** | canary `alive=64/64`（t=1121，111 个 pass 之后） |

## §69.89 fix112：把 `CACHE_FLUSH7` 真的发一次（fix100 的测试其实没生效）

§69.75 的 fix100 用 `sheng-tum=sw5` 在 fence 前加 `CACHE_FLUSH7(0x32)`，结论是「仍然零像素」。
但 fix104 之后有了事件直方图，fix106/107/111 的每一行都是：

```
MUYU EVT c18=45 c19=59 c1a=7 c21=51 c26=5 c31=33 c32=0 c33=57 c3c=0
```

**`c32=0`** —— 这几轮的载体是 `sw`，`5` 这一位没带，所以 `0x32` 本来就不该出现。
也就是说 fix100 那一轮**从来没有人验证过 `0x32` 真的进过流里**：如果当时的解析/发射有问题，
「加了 flush 也没用」这个结论就是空的。而 `CACHE_CLEAN(0x31)` 清的是 UCHE 的**脏行**，
`CACHE_FLUSH7(0x32)` 在 a7xx 上是**clean+invalidate 的合并动作**，两者不是一回事。

fix112 只改载体：`sheng-tum=sw5`，不重编译，其余全部不变。判据两条：

1. `MUYU EVT … c32>0` ⇒ 事件确实发出去了（先证明测试本身有效）；
2. 同一轮 `MUYU CAN … alive` 从 64 掉下去 ⇒ **store 一直在，只是从没被推回内存**，那么修复方向就是补对清理事件；
   仍然是 64/64 ⇒ 「引擎的 store 出不去」在「清理事件正确」的前提下也成立，方向转到 GBIF/QoS/属性。

### 69.89.1 fix112 结果：`c32` 还是 0——但这个 0 不是结论

`logs/vendor_boot_gpu_fix112-logs/muyu-67455451`（kernel 390 s，150 轮，镜像内确认带 `sheng-tum=sw5`）：

```
MUYU EVT c18=185 c19=238 c1a=27 c21=212 c26=25 c31=134 c32=0 c33=247 c3c=0
MUYU CAN t=113 | i0 alive=64/64 v0=a5a5a5a5 | i1 alive=64/64 v0=a5a5a5a5
```

`c32` 仍然 0。**先纠正一处方法论错误**：`native_adreno_event_code[]` 是
`NativeAdrenoVm::scanIbPacketsAt()` 在扫**客户端 IB** 时记账的，ring 自己 fence 里的包**根本不会被扫**。
所以 `c32=0` 既不能证明「载体没生效」，也不能证明「0x32 没发出去」；同理 §69.79 里
「`c21=275` 其中一部分是我们加的」这句是错的——那 275 次全部来自客户端流。

要验证载体只好在 `emitFence` 里直接打一行。这一条留到下一轮，不再用它当证据。

fix112 唯一干净的结论：**在 fence 里多发一个 `CACHE_FLUSH7(0x32)`，canary 依旧 64/64**——
与 fix100 一致，与「清理事件不够」这条一致地否掉。

## 69.90 fix113：`UCHE_CACHE_WAYS` 写进去、被八次提交之内抹掉；附带一次「假阴性」事故

### 69.90.1 先记事故：拿错 trust bundle，两轮日志全部作废

fix113 的第一次启动（`muyu-67291202`）和第二次（`muyu-67315f40`）都是
**`MUYU SB i=` 0 行、`MUYU OFFER … acc=0`、`MUYU UCHE/CAN/CCU/BUSY` 全无**，
而 `MUYU PWR` / `CPROW` / `TTVR` / `METAL M a5.0/5.0` 与 fix112 逐字相同。也就是说
内核 GPU 初始化那一段一模一样，但**用户态的 GPU 客户端从没启动**（`acc=0` 意味着
`NativeGOPPresenter::offer()` 里 `gpuClient` 一直是 false，即没人等在 `waitSwap()`）。

原以为「只有内核变了、不可能影响客户端」，于是在原地绕了很久。真正的原因在**打包参数**：

```bash
# fix113 前两轮（错）
--owner-trust-cache fw/trust-21H461/owner-trust-17-fix99.bundle --gpu-addon fw/gpuaddon-17-fix107.bin
# fix112 镜像里真正用的是（对）
--owner-trust-cache fw/trust-21H461/owner-trust-17-fix107.bundle --gpu-addon fw/gpuaddon-17-fix107.bin
```

两个 bundle 长度相同（61456 B），**只差 40 字节 = 正好两个 20 字节 cdhash**——
插件在 fix107 重建过之后，`adrenoprobed` / 插件二进制的 cdhash 换了两条，而 fix99 的
bundle 里存的还是旧的。payload 里 addon 的 12 140 552 字节**逐字节相同**（`0x95000` 起、
2048 页全等），唯一差别就是 trust cache 尾页那 40 字节，以及 300 多字节的内核。
AMFI 于是拒绝加载那两个二进制，Metal 自检线程起不来 → 没有 submit、没有扫描输出、
显示继续走 CPU 拷贝（`SWAP n=640 st=0`，`OFFER tc=703`）。**内核完全无辜。**

三个可复用的判据，写在这里免得下次再花两轮：

1. **`MUYU OFFER … acc=0` 且 `MUYU SB i=` 为 0** ⇒ GPU 客户端没起，先查打包参数，
   不要查内核。`acc=1` 出现一次就说明客户端在跑。
2. **离线先对齐 trust**：`python3 - <<'PY'` 把 `fw/trust-21H461/*.bundle` 逐个
   `img.find(bundle)` 到待刷镜像里，命中的那个才是这一轮真正用的 bundle；再比对
   `owner-trust-17-fixNNN.bundle` 与 `--gpu-addon` 的编号必须一致。
3. `ls fw/trust-21H461/ | tail -12` **会漏掉** fix106/fix107 这两个 bundle（按名字排序，
   `fix99` 排在 `fix106` 之后），上次就是这么误判成「fix99 是最新」的。

改正后同一份内核（`8e7b4c1d…` 之前那版，`d17396b9…`）立刻恢复：`MUYU SB i=` 234 行。

### 69.90.2 fix113 结果：写进去、八次提交之内被抹掉，canary 仍是 64/64

`logs/vendor_boot_gpu_fix113-logs/muyu-6725541e`（kernel 173 s，234 次提交）：

```
MUYU UCW before=00800000 after=00800000      # 133.3 s，第一次（还没开跑）
MUYU UCW before=00000000 after=00800000      # 135.5 s 起，31 次全部是这个形状
…
MUYU CAN t=233 | i0 alive=64/64 v0=a5a5a5a5 | i1 alive=64/64 v0=a5a5a5a5
```

三条硬事实：

1. **写是落地的**：每次写完立刻复读都是 `00800000`，说明 CPU 侧 AHB 通路、CP_PROTECT
   都没拦，`regWrite()` 这条路可用。
2. **有人把它抹回去**：下一次采样（8 次提交之后）`before` 必然是 0，31/31 次，没有例外。
   抹它的不是读清零（fix109 已用连读两次排除）。它发生在**两次提交之间**，也就是
   **pass 与 pass 之间**——恰好是 GX 掉电/上电 + 上电寄存器表重新施加的位置。
3. **即使中间被设成 `00800000`，canary 依然 64/64**。但这一条**不能**当作
   「`UCHE_CACHE_WAYS` 不是原因」的证据：我们的写只保证「两个 pass 之间的某一瞬是
   `00800000`」，如果每个 pass 开头由上电表重新施加 0，那么**整个 pass 期间它仍然是 0**，
   等于什么都没变。要判它，必须改上电表本身（见 §69.91）。

同轮其它读数与 fix112 一致：`MUYU BUSY n=1 or=00ef0df7`（RB/CCU/UCHE/SP/TSE/RAS/HLSQ/
TPL1/VPC 全忙）与 `n=0 or=00000000`（连 CP_AHB 都不忙）交替；`z1 … b1 e1`；
`PKTS blit=9524 op38=158 bad=2`；`GX … rbbm=00e10df7 s3=04f20007 stall=0 ways=0/0`。

### 69.90.3 上电寄存器表：`0x0e17` 本来就在表里

`NativeAdrenoPwrupRegs.h`（由 `scripts/derive-a740-pwrup.py` 从固定 XML 生成）第 9 项就是
`0x00000e17`，`buildA740()` 用 `ringRead()` **读当时的值**打包成 16 对 (reg, value)，
再由 `NativeAdrenoCpInit` 交给 CP，注释写明「preemption list only; IFPC is off」。
也就是说：**这个值每轮上电都会被重新施加一次**，而 fix113 测到它 8 次提交之内必回 0，
两者是同一件事的两种看法。判据因此变成一句：**表里那一项是 0 还是 `00800000`**。

### 69.90.4 fix114：上电表里那一项**本来就是** `00800000` —— 这条线索结案

fix114 在 `MUYU GX` 那个块里把上电表（`gGpuCpu[3]`：16 B 头 + 16 对 (reg, value)）整张打出来，
并试着把 `0x0e17` 那一项写成 `1<<23`。结果（`logs/vendor_boot_gpu_fix114-logs/muyu-674f0264`）：

```
MUYU PWL0 hdr=00001000 ifpc=0 pre=16 dyn=0
MUYU PWL0 +0 00000e09=fffff000 00000e0a=0001ffff 00000e07=fffff000 00000e08=0001ffff
MUYU PWL0 +8 00000e17=00800000 00000e01=00600000 00008e08=00000816 00008e28=00004800
MUYU PWL slot=8 reg=00000e17 was=00800000 now=00800000     ← 原来就是对的，改不动
MUYU UCW before=00000000 after=00800000                     ← 硬件寄存器仍然是 0
MUYU CAN t=209 | i0 alive=64/64 | i1 alive=64/64            ← canary 依旧全活
```

三件事同时成立：**上电表里是 `00800000`**、**硬件寄存器读出来是 0**、**canary 一个都没死**。
再加上 fix113 的时序（`MUYU UCW` 在 `MUYU IBNZ` 之前 100 µs，也就是在 `CP_RB_WPTR`
踢之前就把值写进去了，见 133.581 / 133.651 两个时间戳），可以定论：

> **`UCHE_CACHE_WAYS` 不是 store 出不去的原因。** 写进去、踢之前生效、pass 跑完，
> canary 依然 64/64。它读 0 是因为本板的 GX 域在 pass 之间频繁复位/掉电
> （同一时刻 `MUYU RB0 ctl=00000000`、`MUYU CCU cntl=00000000` 也是 0，整套 RB 状态都在复位态），
> 而上电表只在上电时施加一次、`UCHE_CACHE_WAYS` 又只由 `hw_init` 写过一次。
> 这是一个**观测窗口落在复位态**造成的假异常，不是配置错误。

这条线索是 §69.84 之后唯一测到的异常，现在结案。剩下的约束没变：

| 已排除 | 依据 |
| --- | --- |
| CP 取指 / ring 内容 / fence | fix43/44、`b1 e1` |
| R 状态、格式、pitch、分量、MRT 指向 | fix102、§69.77 |
| sysmem 模式 | fix107 `RB_BUFFER_CNTL=0x3ff rt0sys=1` |
| CP 保护、SMMU、总线 halt、GX 未上电 | fix107/108/109 |
| CCU 口味与清理 | fix106、fix112 |
| **UCHE 配置（含 CACHE_WAYS）** | **fix113 + fix114（本节）** |
| 引擎是否在跑 | fix111 `or=00ef0df7`（SP/HLSQ/TPL1/VPC 全忙） |

fix111 唯一没解释的一位是 `VFD=0`、`VSC=0`：VSC 是 binning 的可见性压缩器，sysmem 下空闲正常；
但 **VFD=0（没有顶点抓取）** 与 158 个 draw 并存，只有两种可能——采样窗口（踢后 600 µs）
落在 draw 真正开始之前，或者 draw 根本没有抓顶点。后者指向一个从来没被单独变过的变量：
`sheng-gpu-strip`（见下一节）。

## 69.91 真正的写者找到了：`0x0e17=0` 是 turnip 自己每轮发射的

### 69.91.1 fix115（strip=3）先排除掉一条路

`logs/vendor_boot_gpu_fix115-logs/muyu-674a31c4`，载体 `sheng-tum=sw3`：

```
MUYU STRIP mode=3
MUYU SB i=        11            ← 只提交了 11 次就停了
MUYU GFXFAIL … st=e00002d6 …    ← 第一次真正的提交就超时
MUYU CAN t       0              ← canary 根本没机会布置
MUYU USTEXT P z1 … b0 e0        ← 没有任何 RB_DONE_TS 时间戳
```

strip=3（BV/BR 协议按客户端原样执行）**立刻挂死**，与 fix60/61 的记录一致：CP 卡在
`CP_BV_BR_COUNT_OPS` 等一个永远不会被启动的 BV 线程。strip 只能留在 1，这条路走不通。

### 69.91.2 但同一次启动的 IB dump 把 `0x0e17` 的写者照出来了

fix115 的 `MUYU IB 000/010/020…` 是**客户端 IB 的逐 dword dump**。第 010 行：

```
MUYU IB 010: 400e1701 00000000 40b60001 13100000 48b60201 00040724 40ae0301 10001402 …
```

`40XXYY01 VALUE` 这一族 dword 就是 **type4 的寄存器写**（reg = bits 27:8）：
`409e0001 00100000` = `PC_DBG_ECO_CNTL=0x00100000`、`40ae0001 10000000` = `SP_DBG_ECO_CNTL=0x10000000`、
前一行 `408e0701 00000068` = `RB_CCU_CNTL=0x68`（与 fix106 的读数完全对上）。于是

> **`400e1701 00000000` = 客户端自己写 `UCHE_CACHE_WAYS(0x0e17) = 0`。**

对照 mesa 源码：`tu6_init_static_regs()`（`tu_cmd_buffer.cc:2163` 起）把
`phys_dev->info->magic_raw[]` 逐条 `tu_cs_emit_write_reg()` 发出去，而
`freedreno_devices.py` 里 **FD735 / FD750 两条设备表都是 `[UCHE_CACHE_WAYS, 0x00000000]`**，
只有 a730 是 `0x00840004`、a740 是 `0x00040004`。本板走的是 FD735 那条（同表里
`UCHE_UNKNOWN_0E11=0` 与 dump 一致）。

**于是 fix113 的「写进去又被抹掉」彻底解释清楚**：CPU 侧写的是 `0x800000`，但同一个 IB 在
几百微秒后就把它写成 0，所以「八次提交之后必回 0」——不是掉电、不是上电表、不是读清零，
而是**客户端每一轮都亲自把它清零**。这也让 §69.90.4 的结论更硬：上电表没错，错的是流里的值。

### 69.91.3 因此「零 ways 的 UCHE 会吞掉写」这条假设**从来没被验证过**

fix113 的 CPU 写被 IB 覆盖，fix114 改的是上电表（与本问题无关）。要验证它，必须改**流里那个值**。
fix116 就在内核的包改写器里做：`NativeAdrenoVm::stripThreadSync()` 本来逐 dword 走一遍客户端 IB
（为了 strip fix61 的两个 opcode），在那里加一条

```
((h>>28)&0xf)==4 && ((h>>8)&0xfffff)==0x0e17 && (h&0xff)==1 && 值==0   →   值 = 0x00840004
```

并在改写后按 strip 的既有做法 `dsb sy` + `NativeAdrenoMaintainPhysicalPages(..., Clean)`。
取 `0x00840004`（a730 的值：BIT(2)/BIT(18)/BIT(23)）是为了让这个 pass 真的带上「有 ways」的配置；
判据只有一个：**`MUYU CAN alive` 是否开始掉**、`MUYU IMW nz` 是否从 1/64 涨起来。

### 69.91.4 fix116 结果：ways 真的开起来了，store 依然一个字节不落地

`logs/vendor_boot_gpu_fix116-logs/muyu-672ec01c`，载体 `sheng-tum=sw5`（strip=1，与 fix112/114 可比）：

```
MUYU STRIP mode=1
MUYU UCWP patched UCHE_CACHE_WAYS 0 -> 00840004 at 0000000100028000   ← 改写生效
MUYU UCW before=00840004 after=00800000      ← 之后每次采样，流里写进去的就是 00840004
MUYU UCW before=00840004 after=00800000
…
MUYU CAN t=153 | i0 alive=64/64 v0=a5a5a5a5 | i1 alive=64/64 v0=a5a5a5a5
MUYU IMW … i0 nz=1/64 w0=5a5a5a5a … i2 nz=58/64 …
```

`before=00840004` 是关键：它证明**这个 pass 执行期间 UCHE 的 ways 是开着的**（不再是我在 CPU 侧抢在踢之前
写的那一瞬），而这正是 §69.91.3 想要的那次验证。结果：

> **零 ways 不是原因。** `CAN alive` 64/64、`IMW nz` 1/64 一动不动，与 fix112/113/114 完全一致。

同轮其它读数照旧：`PKTS blit=9456 op38=109`、`BUSY n=1 or=00c00015`、`OFFER acc=1 tc=39`。
唯一副作用是 `PKTS bad` 从 2 涨到 49——被改写的值 dword 落在 census 的扫描区间里，被当成坏头计数了，
所以这条 patch 已经在源码里**回退**（注释保留结论），不再带在车上。

### 69.91.5 现在还剩什么

`UCHE_CACHE_WAYS` 这条线（fix108 → fix116，共 5 轮）至此**整条关闭**：

| 尝试 | 结论 |
| --- | --- |
| fix108/109：观测到 0x800000 → 0 | 只是采样落在 GX 复位态 |
| fix113：CPU 侧写 1<<23 | 写落地，但被同一 IB 几百 µs 后覆盖 |
| fix114：改上电表 | 表里本来就是 0x00800000，不是它 |
| fix115：strip=3 走 BV 协议 | 第一次真提交就 `e00002d6` 超时，BV 线程起不来 |
| **fix116：改流里的值 → 0x00840004** | **ways 开着跑完 pass，canary 仍 64/64** |

所以「引擎的 store 出不去」与 UCHE 的任何配置都无关。剩下两条互斥的可能，必须用实验分开：

1. **ROP 确实什么都没写**（片元没产生/被丢弃）——与 fix111 的 `SP/HLSQ/TPL1/VPC` 忙并不矛盾：
   一个被 strip 掉 BV 协议的 binning/render 序列，完全可能把顶点和着色跑完却进不到 ROP 的写回。
2. **ROP 写了，但写去了别处**（TTBR/GBIF 端口/属性路由），SMMU 无 fault 也解释得通——写到了
   另一个仍然合法的映射里。

分开它们的下一次实验：**在一份「确定会被 ROP 写」的最小 IB 上做，而不是在 turnip 的流上做**——
即内核自己发一个 `CP_BLIT`（copy，不是 fill）把 canary 区拷到一个清零区，再用 CP 的 mem-write 读回。
copy 落地 ⇒ 引擎的读写通路都活着，问题在 turnip 的 3D 路径（第 1 种）；
copy 也不落地 ⇒ 引擎写通路上游就断了（第 2 种），该查 TTBR/GBIF 而不是 turnip。

## 69.92 对照 uke / sheng 的历史：本板唯一结构性疾病是「strip 过 BV/BR 协议」，而这两块板都没有它

用户要求先把 uke 与 sheng 的历史对照一遍，再决定下一步。对照结果如下（全部是可复核的事实，不是推断）。

### 69.92.1 三块板的 GPU 起点是同一份上游代码

| 件 | sheng (SM8550/A740) | uke (SM7675/A732) | muyu-17 (A735) |
| --- | --- | --- | --- |
| 内核驱动 | `UniXNU/drivers/Adreno740` | 同一份 | 同一份 |
| hw_init 寄存器表 | `NativeAdrenoA740HwInit.h`（`derive-a740-hwinit.py` 生成） | 同一份 | 同一份 |
| 本板 hw_init 实现 | `GpuPowerProbe.cpp` 内联（1457-1510） | 同结构（1695 起） | `gpuHwInit()`（1722-1780，§38 抽出来给 restart 复用） |
| GMU cold start / OOB GPU_SET / `CP_BV_RB_RPTR_ADDR` | 有 | 有 | 有 |
| `[gpu]` 表 | **没有**，走上游 catalog 默认 | A732（`chip_id 0x43030b00`） | **抄 uke 的 A732 值**，只把 `model` 改成 Adreno735、频率表加了两档 |

逐寄存器 diff 了 sheng 的内联 hw_init 与本板 `gpuHwInit()`：**除本板多出的打点（`native_adreno_hwinit_probe`）和 GMEM 窗口注释外，16 项寄存器写与顺序完全一致**。hw_init 不是差异。

### 69.92.2 真正的差异：`stripThreadSync()` 只存在于本树，且默认剥掉 BV/BR 协议

```
$ cd UniXNU/drivers/Adreno740 && git log -S stripThreadSync --oneline
（空）
$ git status --porcelain | grep Vm
 M NativeAdrenoVm.cpp      # +395 行，strip 就在其中
```

⇒ **sheng 与 uke 的树里没有这个函数**，客户端 IB 是原样执行到 CP 的；本板每次提交都在 `NativeAdrenoSubmit` 里先跑
`vm_->stripThreadSync()`，把 `CP_THREAD_CONTROL(0x17)` 与 `CP_BV_BR_COUNT_OPS(0x1b)` 全部改写成 `CP_NOP`。

fix96 的注释其实已经把后果写清了：

> with the count ops gone the visibility pass and the render pass never agree, the BV thread is never started,
> no bin data is produced and **the rasterizer has nothing to write**

这与 fix111 实测的 `VFD=0`（无顶点抓取）、`VSC=0`（可见性压缩器空闲）**完全吻合**，也是「引擎在忙、无 fault、
store 一个字节不落地、CP 自己的 fence 写正常」这一组现象的**唯一自洽解释**。

### 69.92.3 但 fix60/61 为什么当初必须 strip，而 sheng 不用？

fix60 的记录：不 strip 时 CP 停在 ring 上不进 IB1/IB2；剥掉 `CP_THREAD_CONTROL` 后能进 IB1（`CPIB ib1=100028000
rem=bdc`、`CPKICK rptr 3c->63`），然后停在 `sqe=f87/709b0001` = `CP_BV_BR_COUNT_OPS`，**BV 线程 PC 停在 0x132（idle）**。

strip=3（fix115）保留非 SYNC 的线程切换、仍剥 count ops，结果一模一样：11 次提交后 `st=e00002d6` 挂死。

两种可能，从未被分开：

1. **BV 线程需要一个 `CP_BV_BR_COUNT_OPS` 才会被真正启动**，而 strip=3 把它剥了 ⇒ 必然挂死，与「BV 要不要 preempt」无关；
2. BV 线程依赖 preemption（`CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR` 里的 `CP_BV_IB1_BASE`），
   而本板没有 preemption（§69.75.1：`ring_count_` 恒为 1）⇒ 无论 strip 怎么调都起不来。

**sheng 有 `sheng-gpu-preempt=2`（双 ring + `NativeAdrenoPreempt::attach`），本板没有**——这是第二个差异，
但它只有在第 1 种可能被否掉之后才需要动。

### 69.92.4 因此 fix117：只改一个变量——`strip=0`，让 BV/BR 协议原样执行

载体仍是 `sheng-tum=`（命令行已满）。新增字母 `k`（keep）= strip 0，与现有读者不冲突：
`tu_sheng_render_mode()` 只看 `at[10]`（`=` 后的第一个字符 `s`），TTBR 模式只认 `t`+digit，flush 只认 `5`。

判据（两分，必须互斥）：

- 越过 `CP_BV_BR_COUNT_OPS` 且 `MUYU SB i=` 持续增长、`b1 e1` 出现：
  ⇒ strip 就是原因。再看 `MUYU CAN` / `IMW nz` 是否开始动，动=store 通路恢复，不动=第 2 种可能（preempt）。
- 仍然 `st=e00002d6` 挂死、BV PC 依旧 0x132：
  ⇒ 第 1 种可能被否，BV 线程确实依赖 preemption，转去把 uke 的双 ring + `setPreemption` 移植过来。

### 69.92.5 fix117 结果：strip=0 也死，BV/BR 协议被原样交给 CP 的第一天就挂

`logs/vendor_boot_gpu_fix117-logs/muyu-674adeb3`，载体 `sheng-tum=swk`（新字母 `k` = keep，strip 0）：

```
MUYU STRIP mode=0                      ← 载体生效
MUYU SB i=        11                   ← 与 strip=3 的 fix115 完全一样的提交数
MUYU GFXFAIL m=6 st=e00002d6 …         ← 第 6 次提交超时
MUYU CPROW C10evt R800005 I0 F0 r40/49 m40/5 s400   ← RPTR 停在 0x40，WPTR 已到 0x49
MUYU IBW … sqe=11e/70970001 bv=132/00000000
```

`70970001` = `CP_THREAD_CONTROL`，payload `0x08000001`：THREAD=1（`CP_SET_THREAD_BR`）、
bit27 = `CONCURRENT_BIN_DISABLE`。也就是说 BR 停在 turnip 的 `tu7_set_thread_br_patchpoint`
那条包上，而 **BV 的 PC 停在 0x132、读不出指令**——与 fix60 完全同一个死法。

⇒ §69.92.3 的第 1 种可能（"只是 sync 被剥"）**被否掉**：协议原样交上去，一样死。

### 69.92.6 fix118：终于读了 BV 线程自己的寄存器，它确实在跑，但从来没有 IB

此前从 fix60 到 fix117，所有读数（`CP_RB_RPTR`、`CP_IB1_BASE`、`CP_HW_FAULT`、`CP_SQE_STAT`）
都是 **BR 线程**的，而死的是 BV 的握手。fix118 补上 BV 自己的六个视图
（`0x0a66` rptr、`0x0a6d/0x0a6e` IB1、`0x0a70` rem、`0x0a64` HW_FAULT、`0x0a61` PROTECT_STATUS、
`0x0ada` CHICKEN，外加 `0x08a0/0x08a3` 上下文切换对），并同时打 CPU 侧 memptrs 的四个字：

```
MUYU BV rptr=00000049 ib1=0000000000000000 rem=00000000 hw=00000000 prot=00000000
        avail=0/0/0 chick=00000000 cs=00000000 rest=00000000
   | br rptr=00000040 ib1=0000000100028000 rem=00000000
   | mp 00000040/00000005 bv 00000000/00000000
```

（`logs/vendor_boot_gpu_fix118-logs/muyu-6730c587`，strip=0，16 次采样逐字相同）

四条硬事实：

1. **BV 不是没启动**：`CP_BV_RB_RPTR = 0x49`，正好等于 WPTR，BV 把 ring 里能取的全取完了。
   所以「BV 线程 idle 不跑」这个从 fix60 沿用了一个月的判断**是错的**——它跑过，而且比 BR 跑得远。
2. **BV 从来没有 IB**：`CP_BV_IB1_BASE = 0`，而 BR 的是 `0x100028000`（客户端 IB）。
   `CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR`（`rest`）也是 0：**没有 preemption record**。
3. **BV 没有任何错误**：`CP_BV_HW_FAULT=0`、`CP_BV_PROTECT_STATUS=0`、`CP_BV_CHICKEN_DBG=0`、
   ROQ avail 三个 0、`CP_CONTEXT_SWITCH_CNTL=0`（STOP 没置）。BV 既没被拦，也没被挂起。
4. CPU 侧的 BV rptr 影子（memptrs+8）**始终是 0**，寄存器 0xa66 却是 0x49 ⇒ 两者不是一个东西：
   0xa66 是 BV 的取指指针（硬件推进），+8 是给 preemption restore 用的影子（没人写它，因为没 record）。

把 1+2+4 放在一起，形状就唯一了：

> **BV 在跑，但它是"空跑"——它没有被授予任何 IB，只是在 ring 上追 BR 的取指；
> 而 turnip 的并发 binning 要求 BV 通过 onchip timestamp/barrier 与 BR 握手。
> BR 停在 `CP_THREAD_CONTROL(BR | CONCURRENT_BIN_DISABLE)` 上等 BV 收尾，BV 停在 0x132 等 BR 的信号，
> 两边互等 —— 这正是 turnip 源码里自己写下的那个坑：
> `tu7_wait_onchip_val` 的注释说 "a firmware bug which makes CP_WAIT_TIMESTAMP on BV deadlock with preemption"。**

### 69.92.7 对照结论：本板缺的是 uke/sheng 都有的那一套 preemption，不是 strip

| 差异 | sheng | uke | muyu-17 | 已否掉？ |
| --- | --- | --- | --- | --- |
| `stripThreadSync` | 无 | 无 | 有（本地未提交） | **fix117 否掉**：strip=0 一样死 |
| `sheng-gpu-preempt` | `=2`（双 ring） | `=2`（双 ring） | **没有**，`ring_count_` 恒 1 | ← 剩下的 |
| preemption record / SMMU info 页 | 有（`NativeAdrenoPreempt`） | 有 | **没有**（`bv ib1=0`、`rest=0` 就是它） | ← 剩下的 |
| `[gpu]` 表 | 上游默认 A740 | A732 | 抄 A732（`chipid 0x43030b0` 与原厂 DTS 一致，UBWC 4.0/hbb 16 也一致） | 已核对，非差异 |
| SQE / GMU / zap 固件 | gen71100 系 | gen71100 系 | gen71100（本机 vendor 提取） | 无差异 |
| 呈现 | GPU blit（`sheng-fb-gpu=1`） | **CPU 拷贝到 GOP** | CPU 拷贝 | uke 从未证明引擎写像素，不能当标杆 |

⚠️ 关于 uke 的一条纠正，写在这里免得以后误判：`UniXNU-uke/docs/PROGRESS.md:131-132` 明确写着
`presentBase(frameSurfaceId)` 是「校验几何后**同步拷贝到 GOP**」，帧率 3-7 fps 的原因是
「3200×2136 **CPU 拷贝** 150-300ms/帧」。也就是说 **uke 的屏幕内容始终是 CPU 搬过去的**，
它的 GPU 只是把提交链路跑到不 hang。**uke 不能用来证明"引擎 store 落地"，只有 sheng 能。**

因此 fix119 的方向是确定的：把 uke 已验证的双 ring + `NativeAdrenoPreempt::attach` 移植过来，
让 BV 拿到属于它的 preemption record（`CP_BV_IB1_BASE` 由 CP 在 restore 时填入），
再看并发 binning 能否走完、canary 是否开始死。

### 69.92.8 fix119：preemption 整套移植成功，行为一个字节都没变

把 uke 的双 ring 与 `NativeAdrenoPreempt` 完整搬过来（`gGpuCpu/gGpuMaps` 5→11、`gRing1`、
`attachSecondary`、两条 ring 的 `setPreempt`、`setPreemption`、`preemptWrite`（AHB fence 检查）
与 `gmuWrAt`），载体 `sheng-tum=swkp`。机器上确实装上了：

```
MUYU PREEMPT mode=2
MUYU PREEMPT rings ready rec=10000000/10418000 smmu=20000000/20001000
MUYU PREEMPT submit on=0 mode=2         ← kIOReturnSuccess，ring_count_ = 2
MUYU STRIP mode=0
```

结果（`logs/vendor_boot_gpu_fix119-logs/muyu-673a2e28`）：

```
MUYU SB i=        11       ← 和没有 preempt 时一模一样
MUYU CAN t        0
MUYU BV rptr=00000049 ib1=0000000000000000 … cs=00000000 rest=00000000 | br rptr=00000040 ib1=0000000100028000
MUYU IBW … sqe=11e/70970001 bv=132/00000000
```

`rest`（`CP_CONTEXT_SWITCH_PRIV_NON_SECURE_RESTORE_ADDR`）仍是 0 是合理的——它只在真正发生一次
switch 时由触发器写入，而本板的工作全在 ring 0，从没 trigger 过。但 `bv ib1` 也仍是 0，
说明 **`CP_BV_IB1_BASE` 不是由 preemption record 填的**（它是 CP 执行 `CP_INDIRECT_BUFFER` 时给 BV
的窗口）。§69.92.6 里那条「BV 缺 record」的推断**被这一轮否掉**。

到这一步，四条互斥的机制已经全部被实验排除：

| 假设 | 实验 | 结果 |
| --- | --- | --- |
| strip 剥掉 BV/BR 协议导致零像素 | fix117 `strip=0` | 第 6 次提交即挂死，零像素 |
| BV 线程从未启动 | fix118 读 `0x0a66` | 跑到 WPTR，跑过 |
| BV 缺 preemption record / 没有 IB | fix119 双 ring + attach | 装上了，行为不变 |
| BV 有 fault / 被 CP_PROTECT 拦 / 被 STOP 挂起 | fix118 `0a64/0a61/08a0` | 全是 0 |

剩下唯一还没被碰过、又能单独解释「引擎忙、无 fault、零 DRAM」的，是 **ROP 的 resolve 目的地**：
turnip 在 A7XX 上把 `RB_CCU_CNTL` 设成 `CONCURRENT_RESOLVE_MODE_2` + `UNRESOLVE_FULL`
（正好是 fix106 读到的 `0x68`），而这两个模式的用途是「BV 还在 binning 时让 ROP 并发 resolve」。
BV 不工作 ⇒ bin 数据留在 GMEM、永远不 resolve ⇒ 现象全部吻合。fix120 就把它关掉。

## 69.93 fix120：`RB_CCU_CNTL` 的并发 resolve 就是根因 —— 引擎第一次写出全帧像素

### 69.93.1 判据与改动

只改一个寄存器值，而且是客户端自己写的那一条（`RB_CCU_CNTL 0x8e07`，IB 里就是 `408e0701 00000068`）：

```
value &= ~0x6c      /* bits 3:2 CONCURRENT_RESOLVE_MODE, bits 6:5 CONCURRENT_UNRESOLVE_MODE -> DISABLED */
```

载体 `sheng-tum=swc`（`c` = no concurrent resolve；`k`=strip 0、`p`=preempt 已在前两轮用过，互不冲突）。
`strip` 保持 1，preempt 关闭——这一轮**只动 CCU**。

### 69.93.2 结果：三条独立读数同时变了

`logs/vendor_boot_gpu_fix120-logs/muyu-6757cb97`：

```
MUYU CCUW mode=1
MUYU CCUW patched RB_CCU_CNTL 0000006c -> 00000000 (resolve/unresolve disabled)
MUYU SB i=        32          ← 从 fix61 以来一直是 11（第 6 次提交即挂死），现在不挂了
MUYU CAN t=2 / t=17 / t=25    ← canary 第一次有机会连续采样
MUYU PKTS n=16 draw=5 ev=164 sds=26 ind=6 blit=5877 oth=202 t4=50343 bad=0 …
MUYU EVT c18=22 c19=29 c1a=4 c21=25 c26=2 c31=17 c32=0 c33=26 c3c=0
MUYU IMW n=4 | i0 nz=0/64 w0=00000000 b=2098000 iova=110cd8000
             | i1 nz=1/64 w0=5a5a5a5a b=2098000 iova=101d36000
             | i2 nz=46/64 w0=ff69a596 b=1a13000 iova=11c570000
             | i3 nz=7/64  w0=00000000 b=160000  iova=110a64000
```

三条变化的含义：

1. **`SB i=32`**：提交不再在第 6 次超时。`CP_BV_BR_COUNT_OPS` 那条死锁随 resolve 模式一起消失了
   —— 这也反过来说明，BR 卡在 `CP_THREAD_CONTROL` 上等的其实是 **resolve 的完成**，不是 BV 的 idle。
2. **`c21=25`**：`CP_EVENT_WRITE7` 的 `CCU_CLEAN_COLOR`（0x21）是 sysmem render target 的 store
   离开 color cache 的**唯一**出口。fix104 之后它长期是"客户端流里根本没发过"的状态，现在有了 25 次。
3. **`i2 nz=46/64`，`b=1a13000`**：0x1a13000 = **27,340,800 = 3200×2136×4**，正好是一整帧 BGRA。
   前半轮（fix117/118/119）`IMW` 只有 `n=2`（`i0 nz=0/64`、`i1 nz=7/64 w0=0`），**这块全帧 surface
   从来没被创建过**；fix120 之后它出现了，64 点跨行采样里 46 行有非零首字，值 `0xff69a596`
   （alpha=0xff 的不透明像素），两次采样 46→41 说明内容在变。

### 69.93.3 为什么 canary 一直是 64/64：它盯错了 surface

`MUYU CAN` 的哨兵种在 `gFenceProbeIova = s[0].iova`，即**最大的** import：
`b=0x2098000`（34,111,488 B）——那正是 b3a8 十位 surface（`MUYU 10BIT … len=34177024`）。
而 GPU 这一轮真正渲染的是 `b=0x1a13000` 的 **BGRA 全帧**。

⇒ 「引擎忙 + canary 全活 + `IMW nz=1/64`」这三件事长期并存，**不是**因为引擎什么都没写，
而是因为**探针种在一块 GPU 从来不画的 surface 上**。这条要在 fix121 里纠正：
哨兵必须种到「本轮 draw 的 render target」上，而不是「最大的 import」上。

### 69.93.4 与前四轮假设的关系

| 轮次 | 假设 | 结果 | 现在怎么解释 |
| --- | --- | --- | --- |
| fix117 | strip 剥掉 BV/BR | 仍挂死 | strip 只是让 BV 协议不完整，真正等的是 resolve |
| fix118 | BV 从未启动 | BV 跑到 WPTR | BV 跑了，但它等的东西在 CCU |
| fix119 | BV 缺 preemption record | 装上，行为不变 | preempt 与这条通路无关 |
| **fix120** | **CCU 并发 resolve 等一个不存在的 BV** | **不挂了、c21 有了、全帧非零** | **成立** |

机制上说得通：`CONCURRENT_RESOLVE_MODE_2` 的设计前提是「BV 还在 bin 下一个 tile 时，ROP 就把上一个
bin resolve 出去」。本板 BV 的 bin 数据永远不产生（`CP_BV_IB1_BASE=0`、PC 0x132），resolve 就永远等不到
触发条件；ROP 把结果留在 GMEM/color cache 里，不 fault、引擎照样忙、DRAM 一个字节没有。
把它关掉之后走的是「顺序 resolve」，ROP 在每个 pass 结束时自己清 CCU —— `c21=25` 就是这些清理。

⚠️ 置信度说明：`i2` 非零是**新出现的事实**（前四轮没有这块 surface），`c21` 也是新的；
但"这 46/64 行确定是 GPU 写进去的"还差最后一步直接证据——把 canary 种到 `i2` 上（fix121）。
在那之前不要把 §69.93 当成"首帧已闭环"。

### 69.93.5 fix121：把 canary 扩到四块，仍然是 64/64 —— 因为 arm 得太早

`logs/vendor_boot_gpu_fix121-logs/muyu-67610bab`（同样 `sheng-tum=swc`）：

```
MUYU CAN arm i0 at=0000000101d37000 n=64 bytes=2098000 b=101d36000    ← 34 MB 的 b3a8
MUYU CAN arm i1 at=0000000110a63000 n=64 bytes=160000  b=110a62000    ← 1.4 MB
MUYU CAN t=17 | i0 alive=64/64 | i1 alive=64/64
MUYU IMW n=4 (140.6 s) | … | i2 nz=34/64 w0=ff69a596 b=1a13000 iova=11fc76000
```

全帧 BGRA（`b=1a13000`）**又出现了，34/64 非零**——fix120 的现象稳定复现。
但 arming 发生在 137.1 s，那一瞬 `sampleImports` 只返回两块（34 MB + 1.4 MB）：
3200×2136 的 frame 要到 140.6 s 才存在。**一次性 arming 锁在了错误的 buffer 上。**

于是 fix122 把 arming 改成「每次 tick 重扫，凡是 ≥4 MiB 且还没种过的 import 立刻种」，
并把槽位扩到 8。这样 BGRA frame 一出现就会被种上哨兵，下一 tick 就能读到它是否被 pass 覆盖。

## 69.94 fix122：**判据成立 —— 引擎第一次覆盖掉内核种在 3200×2136 全帧上的哨兵**

`logs/vendor_boot_gpu_fix122-logs/muyu-6737df2b`，载体 `sheng-tum=swc`（CCU 并发 resolve 关闭，strip=1，preempt 关）：

```
MUYU CAN arm i0 at=0000000101d37000 n=64 bytes=2098000 b=101d36000 t=9
MUYU CAN arm i1 at=0000000110cd1000 n=64 bytes=2098000 b=110cd0000 t=17
MUYU CAN arm i2 at=0000000118d69000 n=64 bytes=1a13000 b=118d68000 t=17     ← 全帧，27,340,800 B

MUYU CAN t=129 n=3 | i0 alive=64/64 v0=a5a5a5a5 | i1 alive=64/64 v0=a5a5a5a5 | i2 alive=0/64 v0=ff965a69
MUYU CAN t=137 n=3 | i0 alive=64/64 | i1 alive=64/64 | i2 alive=0/64 v0=ff965a69
MUYU CAN t=145 n=3 | i0 alive=64/64 | i1 alive=64/64 | i2 alive=0/64 v0=ff965a69
MUYU CAN t=153 n=3 | i0 alive=64/64 | i1 alive=64/64 | i2 alive=0/64 v0=ff965a69

MUYU IMW i2 nz=53/64 w0=ff69a596 b=1a13000 iova=118d68000   (171.8 s)
MUYU IMW i2 nz=57/64 w0=ff69a596 b=1a13000 iova=118d68000   (173.7 s)
MUYU SB i= 154 次提交 / MUYU GFXFAIL 16 次
```

### 69.94.1 为什么这一次算数

判据是 §69.44 定下、之后十一轮反复引用、**从未满足过**的那一条：
「canary `alive` 从 64 掉下去」。

- 哨兵由**内核**通过 `poisonWord()` 写进 DRAM（`0xa5a5a5a5`，64 个字，从 `iova+0x1000` 起）。
  CPU 侧没有任何代码会碰这块 import——它是客户端的 BO。
- 唯一能把它覆盖掉的，是 GPU 上跑过的一个 pass（clear 或 draw）落在这块 surface 的这一段上。
- 同一时刻 `i0`（34 MB b3a8）、`i1`（另一块 34 MB）**仍然 64/64 全活**，
  说明不是"整片内存被清"、也不是读路径坏了：**只有 i2 那块被写**。
- `i2 alive=0/64` 是**全部 64 个点**都被覆盖，不是边缘的几个。
- `v0=ff965a69` 稳定不变（alpha=0xff 的不透明像素），`IMW` 的跨行采样从 46 → 34 → 53 → 57
  一直在变，说明内容是被反复重画的真实画面，不是残留。

⇒ **GPU 的 ROP 真的把像素写进 DRAM 里那块 3200×2136 的 BGRA frame 了。**
这是自 fix1 以来第一次，也是 §69.75 之后一直缺的那半个闭环。

### 69.94.2 因果链（三轮实验的共同结果）

```
fix118  BV 的 CP_BV_IB1_BASE=0、PC 0x132            → BV 线程在这块板上不产生 bin 数据
fix120  RB_CCU_CNTL: RESOLVE_MODE_2 → DISABLED      → 不再等 BV 的并发 resolve
        ⇒ 提交不再挂死（11 → 32 → 154），c21=25，全帧 surface 出现
fix122  canary 种在 1a13000 那块上                  → alive 0/64，写入成立
```

`CONCURRENT_RESOLVE_MODE_2` 的语义就是「BV 还在 bin 下一个 tile 时，ROP 把上一个 bin resolve 出去」。
BV 不工作时这个 resolve 永远等不到触发条件，ROP 的写留在 GMEM/color cache 里：不 fault、引擎忙、
DRAM 一个字节没有。关掉它之后走顺序 resolve，pass 结束时 ROP 自己清 CCU（`c21`），像素落到 DRAM。
**这条因果链上每一环都有独立读数。**

### 69.94.3 还没闭环的部分（不要把这一节当成"桌面已显示"）

- `MUYU OFFER` 一次都没出现，`acc=` 也没有 ⇒ **presenter 的 GPU client 仍然没注册**，
  `NativeGOPPresenter::offer()` 的 `gpuMode` 分支没走到。
- `MUYU SWAP n=1..3 st=0` 只有三次，而提交有 154 次 ⇒ 呈现节奏与渲染脱节。
- `MUYU 10BIT … nz=1 nzu=0` ⇒ b3a8 十位通路仍是空的（那是 CPU 拷贝/DPU 那条路）。

也就是说：**渲染通了，扫描输出（scanout）还没接上**。下一步是把这块被写成的 BGRA frame
交给 `GpuScanoutClient`（sheng 的做法：`sheng-fb-gpu=1` 时插件在 GPU 上 blit 到 GOP 并回调 retire），
或者让 presenter 直接拿它做一次 CPU 拷贝以验证内容。

## 69.95 fix123–fix128：**把「屏幕显示的那块内存」本身读出来 —— 69.94 的结论要被推翻一半**

这一节的全部内容来自「直接读面板缓冲」这一件事。以前所有判据都是间接的（IB 流、寄存器、
CPU 拷贝计数），69.94 因此得出「渲染通了、scanout 没接上」。fix123 起能在核心里按像素读回
**面板真正在扫描的那块物理内存**，结论反过来了：

> **scanout 是接上的。黑屏的原因是那块内存里装的就是黑的。**
> 而真正的渲染目标（`MUYU RB0` 一直指着的 34 MB 十位表面）**四个哨兵点全程 64/64 存活** ——
> ROP 一次都没往它里面写。

### 69.95.1 面板缓冲就是 GPU 的 import（先把靶子钉死）

```
[   40.907925] initialize_screen: b=E3940000, w=00000C80, h=00000858, r=00003200
[   90.211140] BRAVEARK: display surface … dims=3200x2136 mode=42475241/1/4 sid=1
[  137.388341] MUYU IMP h=50 va=11e800000 bytes=1a13000 phys=00000000e3940000 seg=27340800 spans=1 ro=0 unc=1 pid=58
```

- `w=0xC80`=3200、`h=0x858`=2136、`r=0x3200` 步长 ⇒ 3200×2136×4 = `0x1a13000` = 27340800 B，**完全吻合**。
- 引导程序 `initialize_screen` 报的基址 `0xE3940000` = import 的首个物理页；`spans=1`（整块连续）。
- 所以从 137 s 起，backboardd 交给 GPU 的那块 27 MB 表面**就是面板在扫的那块内存**。

这条把「往哪儿看」变成确定的事：以后任何「桌面有没有出来」都可以直接读这块内存判定，
不再依赖 `MUYU SWAP`/`MUYU OFFER` 这些间接计数。

### 69.95.2 三个读取器，两条死路，一条通的

| 轮次 | 读法 | 结果 |
|---|---|---|
| fix123 | `peekWords()`（import 自己的 kernel mapping） | `MUYU FB nomap` —— **import 没有 kernel mapping** |
| fix123.1 | `physicalDword()`（`phystokv` 物理孔径） | `MUYU FB noread st=e00002c7` —— `0xE3940000` 在孔径之外 |
| fix123.2 | 板级 `frameWindow()`：`IODeviceMemory::withRange(0xe3940000, 0x1a13000).map(kIOMapInhibitCache)` 一次映射 | **通**，`MUYU FBWIN p=00000000e3940000 bytes=1a13000` |

fix125 用 `MUYU FBCHK` 把「窗口读」和「老路径 `sampleWord()`（每字一个页映射）」在**同一 tick、
同一地址**上对读四次：

```
MUYU FBCHK 0:w=ff69a596/d=ff69a596 1:w=00ffffff/d=00ffffff 2:w=ff000000/d=ff000000 3:w=00000000/d=00000000
MUYU FBCHK 0:w=ff69a596/d=ff69a596 1:w=ff000000/d=ff000000 …
```

八次对照、三十二个地址，`w` 与 `d` **逐字相同** ⇒ 窗口映射读到的是同一份 DRAM，读数可信。

### 69.95.3 面板里装的是什么：先看到「内核控制台」，再看到「纯黑」

fix125 的 96×24 亮度图（每个 tick 打一次）：

```
MUYU FB|   @@@  @  @  @@@
MUYU FB|   @@ @   @ @ @ @ @ @
MUYU FB| @@@@          @@   @@@   @@@   @@@
```

白字、黑底、只在左边约三分之一、约二十行 —— **那是内核自己的启动日志**。决定性的是
`MUYU FBC` 的白色像素数在每次采样间变化（115 → 89 → 51 → 63 → 78），也就是说**控制台一直在重刷
这块缓冲**。根因是 `offline-legacy-console-quiet=0`：`native_legacy_transfer_observed()` 里那句

```cpp
unsigned wanted=1;PE_parse_boot_argn("offline-legacy-console-quiet",&wanted,sizeof wanted);
if(wanted && &vc_set_text_quiet) {vc_set_text_quiet(1);…}      // wanted==0 ⇒ 永不 park
```

fix126 把它改成 `=1`（同字节数，命令行不变），控制台从 138.8 s 起停手，同一块缓冲立刻变成：

```
MUYU FBCHK 0:w=ff000000/d=ff000000 1:w=ff000000/d=ff000000 2:w=ff000000/d=ff000000 3:w=ff000000/d=ff000000
MUYU FB nz=2304/2304 distinct=1
MUYU FBC 000=2304
```

**2304 个采样点全是同一个值、只有一种颜色：不透明纯黑 `0xFF000000`。**

⇒ 屏幕黑不是「扫描输出没接上」，是**面板缓冲里真的什么都没有**。

### 69.95.4 谁在往面板缓冲里写：不是 ROP

fix128 每次提交都读一次 `RB_BUF_BASE/PITCH/FORMAT`（0x8825/0x8826/0x8823/0x8822），做一张直方图：

```
MUYU TGT | 0000000000000000 p=0   f=0   x17
         | 0000000103e0a000 p=400 f=b68 x11
         | 00000001147b7000 p=400 f=b68 x3
         | 0000000101d36000 p=200 f=4b  x63     ← 34 MB 十位表面（i0）
         | 00000001126e3000 p=200 f=4b  x50     ← 34 MB 十位表面（i1）
```

- `pitch=200` 且单位是 64 B ⇒ 12800 B/行 = 3200 px，`fmt=0x4b`（UBWC）。
- 面板缓冲 `0x110cd0000` **一次都没出现在这张表里** ⇒ ROP 从来不是往面板缓冲里画。
- 面板缓冲里的那层黑，来自「把空表面搬过去」的那条路（`native_legacy_transfer` 的 CPU 拷贝：
  十位空表面 → 8 位 BGRA，alpha 补 `0xff` ⇒ 全屏 `0xFF000000`），这也解释了为什么 `distinct=1`。

### 69.95.5 决定性一击：**ROP 的渲染目标上，四个哨兵点全程 64/64**

fix127 把 canary 从「一个点 +0x1000」改成「四点：+0x1000、25%、50%、75%」。
理由是 UBWC 表面开头可能是 flag/metadata 区，只种一个点会问错问题。结果：

```
MUYU CAN arm i0 spots=4/4 at=0000000101d36000+826000 b=2098000
MUYU CAN arm i1 spots=4/4 at=0000000110cd0000+826000 b=2098000
MUYU CAN arm i2 spots=4/4 at=0000000112d68000+684c00 b=1a13000
MUYU CAN t=25 n=3 | i0 ok=4/4 v0=a5a5a5a5 | i1 ok=4/4 v0=a5a5a5a5 | i2 ok=0/4 v0=ff965a69
MUYU CAN t=89 n=3 | i0 ok=4/4 v0=a5a5a5a5 | i1 ok=4/4 v0=a5a5a5a5 | i2 ok=0/4 v0=ff965a69
```

- **i0（`0x101d36000`，正是 `MUYU TGT` 里 63 次提交指向的渲染目标）：4/4 存活，一直活到 89 tick。**
- **i1（另一块 34 MB）：同样 4/4 存活。**
- **i2（面板缓冲）：4/4 全灭**，但它在 `TGT` 里一次都没被 ROP 指过 ⇒ 它是被别人写的（CPU 拷贝）。

**所以 69.94 的核心判据要修正**：fix122 那句「canary i2 alive=0 ⇒ 引擎开始写像素」站不住 ——
那个哨兵种在**面板缓冲**上，而面板缓冲是被 CPU 拷贝/清屏写的，不是被 ROP 写的。
fix127 把哨兵种到 ROP 真正的目标上以后，答案很清楚：

> **ROP 的存储一个字节都没落到渲染目标里。**
> `RB` 被正确指向了那块 34 MB UBWC 表面（base/pitch/format 三项都对得上），
> `CCU_CLEAN_COLOR(0x21)` 每次都发（c21=275），提交也不挂死（`SB i` 能到 150+），
> 但 DRAM 里没有任何一页被写。

### 69.95.6 现在的问题只有一个，而且范围很小

**「RB 存储不落地」** —— 目标地址正确、状态正确、事件正确、fence 基本能 retire，
唯独存储没进内存。可疑项按优先级：

1. **`RB_CNTL` 的 buf location**：`MUYU BUS rbcntl=00000101 mode=0 loc=0`。若 `loc` 那两位表示
   「GMEM vs sysmem」，`loc=0` + `RB0 gm=00000000` 就意味着 ROP 全写进 GMEM（地址 0，不存在），
   而把 GMEM resolve 到 sysmem 的那一步从来没发生。这与「一个字节都不落」完全一致。
2. **UBWC（`fmt=0x4b`）的 flag buffer 地址**没被编程 ⇒ 压缩目标被丢弃/挂起。
3. a7xx 的 RB 寄存器布局与 a6xx 不同（本树很多读数沿用 a6xx 偏移），需要对照 turnip `a7xx` 路径复核。

下一步（fix129）就从 1 开始：把 `RB_CNTL` 的每一位拆开打印，并试一次「关掉 UBWC / 强制 sysmem 目标」
的注入，看哨兵点会不会当场死掉。那是唯一能一句话证伪/证实的方向。

### 69.95.7 本轮顺手的两个副作用（都是真实收益）

- **`offline-legacy-console-quiet=1`**：内核日志不再一遍遍刷面板缓冲。这台机器之后任何
  「屏幕上有没有画面」的观察都不再被控制台干扰。
- **`frameWindow()`**：核心里第一次有了「面板缓冲的 CPU 指针」（一次映射 27 MB，uncached）。
  读一次 96×24 的亮度图和写 40000 个字的标记都变成微秒级，这是后面所有肉眼判据的基础设施。

## 69.96 fix129–fix130：**把「ROP 没写」从采样巧合变成硬结论**

69.95 留下一个可以翻案的空子：fix127 的四个 canary 点种在 `+0x1000 / 25% / 50% / 75%`
（对应 row 0 / 663 / 1326 / 1989），而 IMW 只采 64 个散布点。如果最后一次绘制只画一小块，
两种采样都可能正好错过。这一节把那个空子堵死。

### 69.96.1 先纠正三个沿用很久的寄存器误读（都是查 a6xx.xml / mesa 源码得到的）

| 之前的说法 | 实际定义 | 影响 |
|---|---|---|
| `MUYU BUS loc=0` ⇒ ROP 写 GMEM（`rbcntl` bits 22:23） | `BUFFERS_LOCATION` 是 **variants="A6XX"**，A7XX 上这两位无定义；bits 24:26 才是 `LRZ_FEEDBACK_ZMODE_MASK`。`0x03000000` 正好是 `lrzfb=3` + `RENDER_MODE=0`(rendering) | 69.95.6 的「GMEM 假设」**作废** |
| `CCU csize=0` ⇒ color cache 空，写全丢 | `a6xx_ccu_cache_size` 里 **`CCU_CACHE_SIZE_FULL = 0x0`**；turnip `tu6_emit_ccu_*` 在 sysmem pass 明确发 FULL | 这个假设**作废** |
| `UCHE_CACHE_WAYS=0` 是缺配置 | `freedreno_devices.py` 的 **FD735 raw_magic_regs 就是 `0x00000000`**（A740 才是 `0x00040004`，A730 是 `0x00840004`） | fix108–116 整条打靶线本来就是照抄了别的芯 |

同时 `RB_MRT[0]` 的读数也对齐了：`CONTROL(0x8820)=0x80` = R32 单通道写掩码、
`BUF_INFO(0x8822)=0x4b` = **`FMT6_32_UINT`**（a6xx_enums.xml）、`PITCH(0x8823)=0x200` shr6
= 12800 B = 3200×4。全部与 fix104 的 pass trace 一致。

### 69.96.2 fix129：fragment 是有的，引擎也是跑的

新增 `MUYU VP`，读 `GRAS_CL_VIEWPORT[0] (0x8010..0x8015)`、两组 scissor 和 `GRAS_CL_CNTL`：

```
[137.3] MUYU VP vp=44c80000/44c80000 44858000/c4858000 00000000/3f800000
        | vsc=00000000,08570c7f | ssc=00000000,08570c7f dis=0 | gras=e0 arr=0
[140.5] ... | ssc=04c004c0,04df07bf dis=0 ...
```

- `0x44c80000 = 1600.0f`、`0x44858000 = 1068.0f`、`0xc4858000 = -1068.0f`、`0x3f800000 = 1.0f`
  ⇒ **3200×2136、Y 翻转的标准 viewport，完全正确**。
- `vsc TL=0 BR=0x08570c7f` ⇒ (0,0)..(3199,2135) 全屏。
- `ssc` 后来变成 `TL=0x04c004c0 BR=0x04df07bf` ⇒ **(1216,1216)..(1983,1247)，一个 768×32 的脏条**。
  `dis=0` 表示 screen scissor 是开的。
- `MUYU BUSY or=00ef0df7`：`rb=1 ccu=1 uche=1 sp=1 tse=1 ras=1 hlsq=1 tpl1=1 vpc=1 gpu=1`
  —— **GX 域整条 3D 管线都断言过 busy**，不是「引擎没时钟 / 没退出复位」。
- `MUYU ERR ist=00000000`、`stallfault=0`；16 条 `MUYU SMMUFAULT fsr=400 far=0 fsynr=170d7b
  gfsr=0 cbfsr=fbff` 全集中在 136–140 s 且 `far=0/gfsr=0`，是初始化残留，不是每帧的写故障。

⇒ **目标对、格式对、viewport 对、scissor 对、引擎在跑、没有 fault，唯独存储不出现。**

### 69.96.3 fix130：把 34 MB 表面整个走一遍，包括脏条内部

先补一个基础设施事实：`MUYU IMP h=3d bytes=2098000 phys=… spans=7326 seg=8192`——
**34 MB 渲染目标是 7326 个分散的 4 KiB 页**，而面板缓冲是 `spans=1`。所以 fix123.2 的
`frameWindow()`（一次映射整段）只对面板缓冲有效，对渲染目标根本不能用；对它的所有读写
都必须走 `sampleWord()` / `poisonWord()` 的逐字页映射。

`MUYU SCAN` 因此用 `sampleWord()` 走两张网格：全屏 48×16 = 768 点，再加 **live screen
scissor 内部 64×8 = 512 点**，共 1280 点，每 8 次提交一遍。scissor 从寄存器现读，不是硬编码。

```
[141.4] MUYU SCAN n=4  iova=110cd4000 b=2098000 ssc=1216,1216..1983,1247
        | n=1280 zero=1275 canary=4 other=1 distinct=1 first=5a5a5a5a at=110cd4000 fail=0
[172.7] MUYU SCAN n=19 ... 完全相同的读数
```

 nineteen 次、每次 1280 点、**fail=0**：

- `zero=1275` —— 零。
- `canary=4` —— 我们自己在 +0x1000 等处种的 `0xa5a5a5a5`，**一个都没被覆盖**。
- `other=1` —— 唯一的非 pattern 字是 `0x5a5a5a5a`，位置 `at=110cd4000`（**IOVA 偏移 0**），
  那是 fix95–101 的 **CP_EVENT_WRITE7 自己写的哨兵**，不是像素。
- 512 个密集点全部落在 (1216,1216)..(1983,1247) 内部，**全是 0**。

> **结论（硬）**：ROP 的存储一个字节都没有落到渲染目标里，与采样位置、canary 位置、
> 表面是否连续都无关。§69.94 撤回 fix122 的判断是对的，而且现在比 fix127 更确定。
> 同时这也解释了面板缓冲为什么是 `0xff000000` 满屏黑：CPU 合成路径搬的是一块全 0 的
> 34 MB 十位表面，alpha 补 `0xff` 后正好是纯黑。

### 69.96.4 顺带观测到的两件真事

- **`MUYU OFFER n=1 acc=1 pres=1 tc=117`（280.45 s）**：GPU presenter 在这轮**接受过一次
  offer**。fix122/126/127 的日志只到 174 s，fix128 到 375 s 但没有 OFFER，所以这是
  首次抓到。代价是 `native_legacy_transfer_calls=117`——绝大多数帧仍走 CPU 拷贝。
  以后每轮要等到 390 s 才能判断这个分支。
- **面板缓冲并非永远全黑**：`MUYU FBC 000=2300 956=4`（138 s）→
  `000=2216 fff=85 956=3`（145 s），白点数在变；fix126 那个 `distinct=1 000=2304`
  是某一个 tick 的瞬时状态，不能当成整轮结论。

### 69.96.5 fix131 的实验（下一节填结果）

既然 34 MB 表面是 7326 个分散页、面板缓冲是 1 个连续段，而两者几何**完全兼容**
（pitch 12800 B、2136 行、4 B/px、`BUF_INFO=0x4b` 且 `LOSSLESSCOMPEN=0` 即线性写），
最直接的判别是**把 ROP 的目标直接换成面板正在扫的那块内存**：

- 包改写器在客户端 IB 里识别 type4 寄存器写 `RB_MRT[0].BASE (0x8825/0x8826)`，
  把值改成面板 import 的 IOVA（`bytes=0x1a13000 && spanCount()==1`）。
- cmdline 已满 1016 B，不能加字符：载体从 `swc` 换成等长的 `swm`，`m` 同时打开
  「禁并发 resolve」（fix120）和「MRT 重定向」。fix130 就是同 cmdline 的 `swc` 基线。
- 判据：`MUYU MRTW patched …` 是否出现、`MUYU TGT` 的 base 是否变成面板 IOVA、
  `MUYU FBC` 是否出现大量非 `ff000000` 的像素。
  - 若出现 ⇒ 问题在「34 MB 分散页表面」的 UCHE/SMMU 通路，而且**屏幕当场有画面**。
  - 若仍全黑 ⇒ 问题与表面无关，在 `ROP → CCU → UCHE` 这一跳本身，下一轮从
    `UCHE_UNKNOWN_0E11`（FD735 表要 `0x80`，硬件读回 `0`）和 GBIF halt ACK 下手。

## 69.97 fix131–fix133：**三次失败都是干净的，边界现在落在 CX / GX 之间**

### 69.97.1 想把 ROP 直接画到面板上，但找不到那条写（fix131 / fix132）

思路（§69.96.5）：34 MB 渲染目标是 7326 个分散页，面板缓冲是 1 个连续段，
**两者几何完全相同**（pitch 12800 B、2136 行、4 B/px、`BUF_INFO=0x4b` 且 `LOSSLESSCOMPEN=0`
线性写）。若把 `RB_MRT[0].BASE (0x8825/0x8826)` 改成面板 IOVA，ROP 的像素就直接落在
屏幕正在扫的内存上，黑屏当场解决。cmdline 已满 1016 B，用等长载体 `swc → swm`，
`m` 同时开启「禁并发 resolve」和「MRT 重定向」，fix130 就是同 cmdline 的基线。

```
fix131  MUYU MRTW mode=1                     ← 开关生效
        MUYU CCUW patched 0000006c -> 0     ← fix120 仍生效
        MUYU MRTW patched ...                ← 一次都没有
        MUYU TGT | 0000000101d36000 p=200 f=4b x64 | 0000000110cd4000 ... x57   ← base 没变
```

fix132 改用已验证的 `sampleImports()` 取面板 IOVA，并给每个 type4 header 做命中普查：

```
MUYU MRTW diag panel=0000000110cd4000 t4=8192  hit=0
...  t4=90112 hit=0
```

> **扫过 9 万多个 type4 寄存器写，一个 `0x8825/0x8826` 都没有。**
> `MUYU PKTS t4=94342 sds=552 op43=552` 与之吻合：MRT base 不是直接寄存器写，
> 而是通过 **`CP_SET_DRAW_STATE`（opcode 0x43）的间接 draw-state 表**下发的 ——
> 那张表是另一块 GPU 内存，从来没有作为 submit buffer 交给 `stripThreadSync()`，
> 所以线性扫描够不着它。**要改它必须先解析 SDS 包、跟到那张表的 IOVA 再改表。**

### 69.97.2 A735 ≠ A740 的 UCHE magic，也不是原因（fix133）

用户从一开始就提醒本板是 **A735**，而整个驱动树叫 `Adreno740`、hw_init 用 a740 表。
`freedreno_devices.py` 的 FD735 `raw_magic_regs` 要
`REG_A7XX_UCHE_UNKNOWN_0E11 = 0x00000080`，a740 表要 `0x00000000`；
`MUYU UCHE` 每轮都读 `u=00000000/00000000/00000000`。UCHE 是 ROP 的下一跳，
是这个形状里最后一个没试过的配置位。

fix133 在包改写器里 patch 客户端 static regs 的 `0x0e11` 写，并在 submit 里用
`regWrite` 兜底，两者都读回验证：

```
[132.9] MUYU UCHEW patched 0e11 00000000 -> 00000080 hits=1
[170.9] MUYU UCHE ... u=00000000/00000080/00000000      ← 值写进去了并且保持住
[170.9] MUYU SCAN n=16 ... zero=1275 canary=4 other=1  ← 一个像素都没多
[171.0] MUYU CAN t=129 | i0 ok=4/4 | i1 ok=4/4 | i2 ok=0/4
```

> 注入成功、值保持、`SB i` 142 次提交不挂死，**但 ROP 仍然一个字节都不写**。
> A735/A740 的这一位不是原因，可以从清单里划掉。

### 69.97.3 现在剩下的，是一条架构分界

到 fix133 为止，被**逐条证伪**的假设：

| 假设 | 证伪轮次 |
|---|---|
| stripThreadSync 破坏 BV/BR 协议 | fix117 (strip=0 照样挂) |
| BV 没启动 | fix118 (BV rptr 跑过，ib1=0 空跑) |
| BV 缺 preemption record | fix119 (完整双 ring 移植无效) |
| `RB_CCU_CNTL` 并发 resolve 等 BV | fix120（**有效**：提交 11→150+，但只解决挂死，不解决写） |
| UCHE_CACHE_WAYS 零 ways | fix108–116 + freedreno_devices.py FD735 表本来就是 0 |
| 上电表没写 | fix114 |
| strip / 采样窗口 / trust bundle 编号 | fix113 事故已结案 |
| 引擎没时钟、GX 没退出复位 | fix129 `BUSY or=00ef0df7` 全 1 |
| viewport / scissor 剪掉全部 fragment | fix129 `1600/1600, 1068/-1068, 1.0` 全对 |
| canary 种错位置 / 采样盲区 | fix130 1280 点含 scissor 内 512 点，全 0 |
| SMMU fault 吞掉写 | fix129 `ist=0 stallfault=0`，16 条 fault 全是 `far=0 gfsr=0` 初始化残留 |
| `RB_CNTL.BUFFERS_LOCATION` / GMEM | a6xx.xml: variants=A6XX，A7XX 上无此位 |
| `CCU_CACHE_SIZE=0` | `CCU_CACHE_SIZE_FULL = 0x0` |
| `UCHE_UNKNOWN_0E11` A735 值 | fix133 注入成功且无效 |

**全部「往哪儿写」和「有没有 fragment」的问题都已排除。剩下的形状只剩一个：**

> CP（CX 域）的命令取指、寄存器写、`CP_EVENT_WRITE7` 写内存**全部正常**；
> RB / ROP / UCHE（GX 域）**寄存器能读回正确值、busy 位会断言、事件会发，
> 但发起的 store 从不抵达 DRAM**。

`GpuPowerProbe.cpp` 里现成的两条线索指向同一处：

- `openCpAperture()` 的注释：安全世界打开的是 **「CP's aperture」** 到 context bank 0，
  让 `CP_SMMU_TABLE_UPDATE` 能切表。没有任何一步为 GX 侧的 master 开 aperture。
- `MUYU S2CR`（fix107 写下的普查）：
  ```
  scr0=220406 | 0 smr=80000000 s2cr=00000000 | 1 smr=0 s2cr=00020000
              | 2 smr=80000005 s2cr=00000001 | 3..7 smr=0 s2cr=00020000
  ```
  只有 slot 0 / 2 是有效 SMR，其余都是 `s2cr=00020000`。fix107 自己的注释已指出：
  > "an S2CR naming a CBAR/SMR the bank does not own routes the transaction somewhere
  > harmless **without a fault either**" —— 这正好解释「没有 fault、也没有写」。

**下一步（fix135+）按优先级**：

1. **肉眼确认 scanout**：fix134 无条件把 8 条 400 px 全高彩条写进 `0xE3940000`
   面板缓冲（每 tick 重画）。若屏幕上真的出现彩条 ⇒ §69.95「scanout 接上了」坐实，
   全案收敛到 GX store；若仍黑 ⇒ §69.95 的那半句也要撤回，方向重来。
2. **查 GX masters 的 SID / S2CR 路由**：把 SMMU 的 `SMR`/`S2CR` 每个 slot、
   `CBAR`/`CBFRSYNRA`、以及 GX 侧各 master 的 stream ID 全部读出来，确认 UCHE/RB
   的事务是否被路由到与 CP 同一个、拥有正确 TTBR0 的 context bank。
3. **解析 `CP_SET_DRAW_STATE`**：这既能让 §69.97.1 的 MRT 重定向真正落地
   （一条「哪怕 ROP 只能写连续内存也能出画面」的旁路），也能直接读 turnip 下发的
   完整 RB 状态，而不必再猜哪个寄存器是谁写的。

### 69.97.4 本轮新增的、可复用的东西

- `MUYU VP`：viewport / 两组 scissor / GRAS 状态（a6xx.xml 核对过的偏移）。
- `MUYU SCAN`：对任意分散页表面做双网格密集扫描（全屏 + live scissor 内），
  1280 点、`fail=0`，是现在判断「引擎有没有写」的唯一硬判据。
- `MUYU TGT`：每提交一次 RB_MRT0 base/pitch/format 的直方图。
- `MUYU MRTW diag` / `MUYU UCHEW`：包改写器侧的可验证注入 + 命中普查，
  以后任何「patch 没生效」都能一眼区分「开关没开」「找不到目标」「写进去又被覆盖」。
- 载体字母表：`sw5` / `sw3` / `swk` / `swkp` / `swc` / `swm`（`m` = c + MRT/UCHE 注入，等长）。

## 69.98 fix134–fix135：**黑屏的直接作者找到了 —— 不在 GPU 驱动，在显示驱动的 10-bit 分支**

这一节改变了问题的性质。§69.95 说「scanout 接上了，是内容黑」，但没说**谁把内容刷成黑**。
fix134 的整屏彩条把那个写者逼出来了。

### 69.98.1 fix134：彩条写进去了，1 秒后就没了

fix134 去掉 fix124 的 `distinct>=3` 护栏，无条件把 **8 条 400 px 全高彩条**写进
`physical == 0xE3940000` 的面板缓冲（`MUYU FBMARK panel=00000000e3940000 8 bars x 400px`，
每 tick 重画）。结果：

```
[137.935] MUYU FBMARK ...                        ← 8 条彩条写入
[139.218] MUYU FBCHK 0:w=ff69a596 ...            ← 第一个字不是彩条
[139.273] MUYU FBC 000=2300 956=4               ← 2300/2304 个采样点是纯黑
[139.222] MUYU FB| (24 行全空)                   ← 整屏彩条消失
```

同期 `MUYU SWAP n=1/2/3` 在 **137.70 / 139.04 / 140.03 s** —— 几乎正好每秒一次。
所以有一个高频 CPU 写者在覆盖面板缓冲。

### 69.98.2 那个写者：`NativeLegacyDisplay.cpp` 的 `if(tenBit)`

```
UniXNU-muyu-17/drivers/GopFramebuffer/NativeLegacyDisplay.cpp
  305  surfaceFormat = 'b3a8' (62336138), planes=2  ->  tenBit = true
  310  if(...) { ...; return; }                       // validator
  316  // 10-bit scanout: the GPU offer and every copy helper below are BGRA-only,
  317  // so plane 0 is converted here and the frame is scanned out like a copied one
  318  if(tenBit) {
  322     packed = memory->map(...)                    // 34 MiB b3a8 surface
  379     nz = 每 4096 字节采一个字，非零计数
  412     if(nz)                                      // ← nz = 4..5，恒为真
             for y,x: out[y*stride+x] = 0xff000000 | ...
  436     native_legacy_transfer_observed(kIOReturnSuccess, ...)
  438     return;                                     // ← 永远走不到 445 行的 GPU offer
  440  }
  445  if(!gpuBypass && native_legacy_gpu_offer(...)) { ... GPU 路径 ... }
```

`MUYU 10BIT S4` 坐实了输入：

```
[136.8] MUYU 10BIT S4 n=1 nz=4  first=4096 heads=00000000,...
        stride=3200 alloc=34177024 fmt=62336138 planes=2
[140.0] MUYU 10BIT 3200x2136 plane0=27340800 len=34177024 nz=5 nzu=1
        cb=5a5a5a5a ca=5a5a5a5a heads=5a5a5a5a,00000000,... alpha0=00
```

- `fmt=62336138` = `'b3a8'`，两平面 10-bit packed BGRA + alpha，34 MB。
- 表面**几乎全 0**；`nz=4..5` 这几个非零字就是 **我们自己的 canary（`a5a5a5a5`）**
  和 **CP 自己的 `0x5a5a5a5a` 哨兵**（`first=0` 时 heads[0] 就是它）。
- 于是 `if(nz)` 恒为真，每帧把 3200×2136 个像素**全部**写成 `0xff000000`
  —— 这正是 §69.95.3 反复读到的「2304 个采样点只有一种颜色：不透明纯黑」。
- 而且这个 `return` 在 `native_legacy_gpu_offer()` **之前**，所以只要交换表面是 `b3a8`，
  **GPU 永远接不到这一帧**。这就是 `MUYU OFFER` 在 390 s 里只出现 1 次
  （fix129，280.45 s，那一帧大概不是 tenBit）的结构性原因。

> **所以要修正之前的回答**：黑屏的直接可见原因**不在 GPU 驱动**。
> GPU 侧的问题（ROP 存储不落地）仍然是真的、也是上游病根，但「屏幕为什么是纯黑」
> 是 `NativeLegacyDisplay.cpp` 的 10-bit CPU 转换每帧把空表面刷成黑、
> 并且把 GPU offer 分支整个短路造成的。两者叠加，才有了 130 多轮的僵局。

### 69.98.3 历史教训：不能简单地把 tenBit 改成走 GPU offer

同文件 fix71 的注释记录了 fix68/fix70 已经试过：

> "It did let the GPU accept a frame (HANDOFF n=4), but on BOTH runs the display path then
> froze permanently: the branch returns with `transferred=true`, and swap() (line 262)
> refuses every later frame until `finishGpuSwap` clears it. With no `completeSwap` coming
> from the scanout client for a two-plane 'b3a8' surface, the frame stream stops at frame 4."

也就是说，10-bit 走 GPU offer 需要**同时**解决两件事：presenter 认识两平面 `b3a8`
并回调 completeSwap，以及 `transferred` 的正确清理。

`UniXNU-uke/package/LegacyDisplayProbe.cpp` 给出了对照：uke 的
`native_legacy_gpu_offer(..., uint32_t format)` **多带一个 format 参数**，注释写得很明确：

> "The frame's format has to reach the presenter: the display offers NGP_DIRECT for a frame
> the pipes scan themselves ... and NGP_BGRX8888 / NGP_RGBA16F_* for one it should copy.
> This forwarder used to drop the argument, so the client read a register's leftovers,
> refused every offer and the display fell back to scanning frames the GPU was still writing."

muyu-17 的 `NativeGOPPresenter::offer()` 没有 format 参数，也不检查格式
（`accepted = online && gpuClient && !swap.offered`）——它能不能处理 `b3a8`
从未被验证过。

### 69.98.4 fix135 的实验与下一步

fix135 只做一件事：给 tenBit 的像素写入加一个等长开关（`sheng-tum=swC` 里的 `C`），
**跳过 `out[]` 的 27 MB 写入，保留全部 census 和 transfer 回调**。判据：

- 彩条若留住（`MUYU FBC` 出现 8 种颜色、`distinct≈8`）⇒ 面板真的在扫 `0xE3940000`，
  且这个转换是唯一的覆盖者 ⇒ **scanout 端到端坐实**，用户肉眼也应该能看到 8 条彩条。
- 彩条仍消失 ⇒ 还有第二个写者，需要继续追。

确认之后的正路（按风险从低到高）：

1. 把 `if(nz)` 换成「表面真的有像素」的判据（现在被 canary / CP 哨兵污染，
   4 个非零字就触发全屏刷黑）。至少不再主动把屏幕刷黑。
2. 给 muyu-17 的 `native_legacy_gpu_offer` / `NativeGOPPresenter::offer` 补上 uke 的
   `format` 参数，让 presenter 区分 `NGP_DIRECT` 与需要拷贝的帧，并让 tenBit 分支
   在 GPU client 在线时走 offer、配合正确的 `finishGpuSwap`。
3. 或者让 render server 提交 BGRA 而非 `b3a8` 交换表面（格式协商在 IOMFB 侧），
   直接绕开 tenBit 分支 —— 这也是 sheng「mode0 能进桌面」可能走的路。

### 69.98.5 fix135 结果：**彩条留住了，覆盖者唯一，面板缓冲可稳定控制**

```
[136.818] MUYU TENB skip=1
[137.295] MUYU FBMARK panel=00000000e3940000 8 bars x 400px
[137.465] MUYU FB iova=...12310a000 b=1a13000 p=00000000e3940000 nz=2304/2304 distinct=8
[137.489] MUYU FBC fff=288 00f=288 0f0=288 f00=288 0ff=288 f0f=288 ff0=288 222=288
[137.466] MUYU FB| @@@@@@@@@@@@            ++++++++++++::::::::::::************------------%%%%%%%%%%%%............
[138.773] MUYU FB ... distinct=8          ← 一直保持到最后一次采样
```

288 × 8 = 2304 = 96×24 网格，**八种颜色严格均分、整屏无空洞**，两个读取器
（`FBCHK w=ffffffff/d=ffffffff`）也逐字一致。

> **结论**：`NativeLegacyDisplay.cpp` 的 `if(tenBit)` 像素转换是**唯一的**面板缓冲覆盖者。
> 关掉它以后，CPU 写进 `0xE3940000` 的内容可以一直保持。
> §69.95「scanout 是接上的」现在有了正面证据（此前只有「读到的内容一致」的间接证据）。
> 结合 `initialize_screen b=E3940000`，屏幕上应当能看到 8 条彩色竖带 —— 这是唯一还需要
> 人眼确认的一环。

**这也意味着之前关于「还是不是 GPU 驱动问题」的回答要再修正一次**：

- 上游病根确实在 GPU 侧：ROP 存储不落地（§69.96 硬证）。
- 但**「屏幕为什么是纯黑」的直接原因是显示驱动**：`b3a8` 交换表面每帧被 CPU 转成
  `0xff000000` 刷满面板，且这个 `return` 把 GPU offer 分支整个短路。
- 两条叠加：GPU 没写出像素 → 34 MB 表面全 0 → 10-bit 转换把「全 0」转成「全屏黑」
  → 同时 GPU 永远接不到帧。**修任何一条都能改变现状，但只有两条都修才有桌面。**

### 69.98.6 当前状态与下一步（交接）

已闭环：黑屏机制、scanout 通路、ROP 不写、面板缓冲可写、控制台停刷。
未闭环：ROP 存储仍不落地；GPU offer 仍几乎不触发；屏幕是否有彩条需肉眼确认。

下一轮建议顺序：

1. **请用户确认**：fix135 那一轮（13:06–13:09 刷机窗口）屏幕上是否出现过 8 条彩色竖带。
   出现 ⇒ scanout 端到端坐实，后续所有「为什么没画面」都只归因于内容。
2. **给 muyu-17 补 uke 的 `format` 参数**（`native_legacy_gpu_offer` /
   `NativeGOPPresenter::offer` / `NGPSwap`），并在 tenBit 分支里当 GPU client 在线时
   走 offer；同时按 fix71 的教训正确处理 `transferred` / `finishGpuSwap`，否则帧流
   会在第 4 帧冻结。
3. **或者改格式协商**：让 IOMFB 提交 BGRA 交换表面而不是 `b3a8`，直接绕开 tenBit
   分支 —— 这与 sheng「mode0 能进桌面」的历史线索可能同源。
4. 上游的 CX/GX 问题（§69.97.3）仍然独立存在：SMMU 的 `S2CR` 只有 slot 0/2 有效，
   `openCpAperture()` 只开了 CP 的 aperture。即使显示层修好，ROP 不写仍需解决
   才能有真正的 GPU 合成画面。

## 69.99 sheng「mode 0 反而进桌面」正式结案 + fix136：面板缓冲的静止与第二个写者

### 69.99.1 用户提示的历史线索，逐条对上号

用户两次提示：「我记得 sheng 用 mode0 能进桌面」「就是换表的那部分，sheng 记录用
mode0 反而进桌面」。这一轮把整条线从 sheng / uke 文档一直追到本树源码，结论是：
**线索是真的，但它不能当开关用 —— 本板已经跑在 mode 0，而且 mode 0 的「进桌面」
恰恰是 GPU 被完全旁路的结果。**

三处原文：

| 位置 | 原文要点 |
| --- | --- |
| `UniXNU/docs/devices/sheng.md` 及 5 份 `sheng-*.md` | `sheng-gpu-preempt defaults to 2 ... Mode 1 sends only low/background to ring 1; mode 0 selects one ring.` |
| `UniXNU/drivers/Adreno740/NativeAdrenoPublish.cpp:199-201` | `commit 2949667 ("Force mode 1 on uke") ... Mode 2 is the only mode that has ever worked on all devices.` —— 上游自己的注释把 mode 0/1 判为特例 |
| 本树 `docs/GPU-HANDOFF.md` §69.75.1 / §69.76 | `ringForPriority()` 第一行 `if(!preemption() \|\| preempt_dead_ \|\| table_mode_!=2) return 0;`，本树生产代码**没有任何** `setPreemption()` 调用 ⇒ 「本板实际运行就是 sheng 文档里那个 mode 0（一个 ring）」 |

「换表」在本树文档里另有一义，指 `tableMode()` / `CP_SMMU_TABLE_UPDATE` 的三种页表切换
（§23–§33、§69.76）。载体 `sheng-tum=swt0|swt1|swt2` 分别选 mode 0/1/2。
§69.76 的 fix101 已经用 `swt0` 复验过 sheng 这条记录：

```
MUYU TTBR mode=0        （载体生效）
Z n2 nz0/64 w0=00000000 wl=00000000 f1
MUYU IMW n=4 | i0 nz=0/64 w0=00000000 b=2098000 ...
HC hang ×4，GFXFAIL ×16
```

最硬的一格是 **`w0` 从 `5a5a5a5a` 变成 `00000000`**：哨兵本来由 CP 用
`CP_EVENT_WRITE7` 写进 surface IOVA，mode 0 下它也不见了 ⇒ GPU 跑在内核表里，
而 surface 的 IOVA 只存在于客户端表 ⇒ 这次写落进未映射地址被丢弃。
§69.76.2 因此钉死：

> mode 0 下 GPU 一个字节都不写，但桌面照样出现，因为本板桌面本来就是
> `offline-legacy-*` 的 **CPU 拷贝合成**。所以「进桌面」不是 GPU 渲染的证据，
> mode 0 的「成功」恰恰是 GPU 被完全旁路的结果。

**本轮实测再次确认**：fix136 日志 `MUYU PREEMPT mode=0`、`MUYU TTBR mode=1`。
单 ring 已是既成事实；换表必须留在 mode 1（CPU 在 GPU 空闲时写 TTBR0 + TLBIALL）。
A735 与 A732(uke) / A740(sheng) 在这里还有一个具体差别：本板原厂 DTS 的
`qcom,smmu-kgsl-cb` 里 `gfx3d_user` 是 SID 0、`gfx3d_secure` 是 SID 2、GMU 是 SID 5，
而 uke 的 `NativeAdrenoGpuContextBringup` 注释写的是「DT iommus SIDs 0 and 1」。
`MUYU S2CR` 实测 `slot 0 smr=80000000`（SID 0）、`slot 2 smr=80000005`（SID 5），
与本板 DTS 一致 ⇒ **不能照 uke 的 SID 1 去补第二条路由**（`kSecondSid=UINT16_MAX`
那条 TODO 也就此结案，不是待办）。

### 69.99.2 fix136 做了什么

fix135 的彩条是**每 tick 重画 27 MB** 的，这既证明了面板可控制，也把 142.34 s 之后
写进面板的东西全抹掉了（§69.98.5 只记到「彩条一直保持到最后一次采样」，其实
日志尾段已经有 `000=638` 的黑点混进来）。fix136 只改三处：

| 文件 | 改动 |
| --- | --- |
| `UniXNU/drivers/Adreno740/NativeAdrenoSubmit.cpp` `framePreview()` | 彩条**只画一次**（`MUYU FBMARK once`）；新增 `MUYU FBD ch=<n>/2304 rows=<a>..<b>`，把本 tick 的 96×24 网格与上一 tick 逐字比较 |
| `UniXNU-muyu-17/drivers/GopFramebuffer/NativeLegacyDisplay.cpp` | blank swap 的全屏 `0xff000000` solid-fill 分支加 `MUYU BLANK n=` 计数，用来判定第二个写者 |
| 载体 | 仍 `sheng-tum=swC`（`C` = 禁 CCU 并发 resolve + 跳过 tenBit 刷屏），单变量 |

镜像 `fw/vendor_boot_gpu_fix136.img`，sha256
`afd055a0c817a077255895cedeb88823abd842c12aadb3452d929c766639133c`。
离线校验：trust/addon 都是 fix107（`6fc1914b…` / `410685f6…`）、镜像里能找到
`sheng-tum=swC`，fix99 的 trust/addon 均未混入。
日志 `logs/vendor_boot_gpu_fix136-logs/muyu-675cb800`（kernel 173 s、42 轮、gap=0）。

### 69.99.3 判读：面板静止 4.7 秒，然后被一次性覆盖

```
[136.372] MUYU FBMARK once panel=00000000e3940000 8 bars x 400px
[136.802] MUYU FBD ch=2304/2304 rows=0..23      ← 彩条写入，整屏变化
[137.100] MUYU FBD ch=0/2304                    ← 静止开始
...       （137.1–140.5 s 共 12 次采样，全为 ch=0，FBC 8 色各 288）
[140.473] MUYU FBD ch=0/2304
[141.671] MUYU FB  nz=1932/2304 distinct=9      ← 第 9 种颜色出现
[142.062] MUYU FBC fff=228 000=372 00f=140 0f0=179 f00=237 0ff=284 f0f=288 ff0=288
[142.076] MUYU FBD ch=453/2304 rows=0..23
[142.215] MUYU SCAN n=20 ... zero=1275 canary=4 other=1 fail=0
[142.559] MUYU HC hang n=1 ticks=177
[143.473] MUYU OFFER n=1 acc=1 pres=1 tc=38
[144.434] MUYU ABI ... SpringBoard s2/e00002d6
```

三条硬事实：

1. **只画一次也能保持**。`ch=0` 持续 4.7 s、8 色严格各 288，说明 `swC` 关掉
   tenBit 刷屏后面板缓冲是**真正静止**的，不存在周期性 CPU 刷屏。
   §69.98.5「10-bit 转换是唯一覆盖者」在静止期内成立。
2. **141.67 s 有一个此前没见过的写者**。它把左半屏（前 4 条彩带、x≈0..1600）
   写成了带结构的黑色图案，`MUYU FB|` 的 24 行里 `@`/`+`/`:` 变成空格的排布
   像字符/图标而不是矩形填充；右半屏 4 条彩带一字未动。372 个采样点变成
   `000`，`FBCHK` 两个读取器仍逐字一致，`CAN i2 ok=2/4`（面板缓冲上的 canary
   被改掉 2 个点）。**不是 blank swap**（整轮 0 行 `MUYU BLANK`）、
   **不是 tenBit 转换**（`MUYU TENB skip=1`）、**不是 GPU offer**
   （`OFFER n=1` 在 1.8 s 之后，且 n=1 是第一次调用）。
3. **随后 GPU 挂了，ROP 仍一个字节没写**。`SCAN zero=1275 canary=4 other=1 fail=0`
   与 fix130 逐字相同；`HC hang`、两行 `OPEN refused e00002d5`、
   SpringBoard `s2/e00002d6`。142.076 之后不再有 `MUYU FB` 行，因为
   `framePreview()` 挂在 GPU 提交路径上，提交一停采样也停 —— 这是当前读取器的
   结构性盲区。

⇒ **§69.98.5 那句「覆盖者唯一」要收窄**：在 GPU 正常提交的静止期内唯一；
GPU 进入 hang/客户端重启的那一瞬，还有第二个写者会往面板写内容（形态是文字状
黑块，不是桌面）。要判它是内核 dump 控制台、GPU presenter 的 transfer，
还是 backboardd 自己的帧，必须把面板采样从 GPU 提交路径里摘出来。

### 69.99.4 下一步（按优先级）

1. **把面板采样改成不依赖 GPU 提交**：在显示驱动的 vsync/5 s ticker 上直接读
   `bootMapping`，即使 GPU hang 也继续出 `MUYU FB`/`FBD`。同时把 `MUYU BLANK`
   改成前 3 次也打（现在第 16 次才打，会漏掉一次性事件），并给 GPU presenter 的
   `transfer`/`completeSwap` 加计数 —— 这样 141.67 s 那个写者可以一次定位。
2. **修 `if(tenBit)` 的结构性短路**：补 uke 的 `format` 参数
   （`native_legacy_gpu_offer` / `NativeGOPPresenter::offer` / `NGPSwap`），
   让 tenBit 帧在 GPU client 在线时走 offer；必须同时按 fix71 的教训处理
   `transferred` / `finishGpuSwap`，否则帧流在第 4 帧冻结。注意本板是 21H461，
   sheng 的 `patchFramebufferBranch()` 硬编码 24A446 的 `0x184461928`，
   **不能直接照搬**（该函数会先校验 `word==0x37000608`，不匹配时安全返回
   `0x6E41` 不打补丁，所以也不会写坏内存，但格式不会变）。
3. 上游 GX store 仍是最终瓶颈：`CP_EVENT_WRITE7` 落地、3D/blit 全不落地、
   无 fault、`SCAN` 19 次全零。`CP_SET_DRAW_STATE`（`PKTS op43=5`、`sds=563`）
   的间接表是唯一还没读过的地方，也是让 fix131 的 MRT 重定向真正落地的前提。
4. 用户肉眼确认仍然缺一环：fix135/fix136 两轮的 `0xE3940000` 都稳定画着
   8 条全高彩带，屏幕上是否看见过它们。看见 ⇒ scanout 端到端坐实，
   之后所有「没画面」都只归因于内容。

## 69.100 fix137：把面板采样从 GPU 提交路径摘出来，两个读取器互证

### 69.100.1 改动

`fix136` 之后 142.076 s 起所有 `MUYU FB` 行消失，不是面板停止变化，而是
`framePreview()` 只从 `NativeAdrenoSubmit::submit()` 调用 —— GPU 一 hang 就瞎了。
fix137 在显示驱动里加一条**完全独立**的读取器：

| 文件 | 改动 |
| --- | --- |
| `UniXNU-muyu-17/drivers/GopFramebuffer/NativeLegacyDisplay.cpp` | 新增文件级 `panelPreview(IOMemoryMap *)`：直接读 `bootMapping->getVirtualAddress()`，48×16 稀疏网格（768 点），每 32 次 tick 打一次（60 Hz vsync ⇒ ~2 Hz），纯 volatile 读、不做 cache invalidate（fix66 的教训）。打印 `MUYU PFB nz/distinct/ch`、`MUYU PFBC` 颜色直方图、变化时打印 `MUYU PFB|` ASCII。在 `kickOnce()` 与 `vsyncTick()` 开头各挂一次，共用限流计数 |
| 同上 | `MUYU BLANK` 改成前 3 次也打（原来只在第 16 次打，会漏掉一次性 blank swap） |
| 其他 | 载体仍 `swC`；`FBMARK once` / `FBD` 保持 fix136 不变 |

镜像 `fw/vendor_boot_gpu_fix137.img`，sha256
`b46886341de5fccefbb2d357a6cb09add37a4ffe65cc02a3eea16c22a7c66162`。
构建后离线校验 trust/addon 均 fix107、`swC` 在镜像内、无 fix99 混入。
日志 `logs/vendor_boot_gpu_fix137-logs/muyu-6736bba0`（kernel 143 s、27 轮、gap=0）。

### 69.100.2 两个独立读取器逐字一致

```
[138.425] MUYU PFB  n=1 nz=768/768  distinct=8 ch=0/768
[138.429] MUYU PFBC fff=96 00f=96 0f0=96 f00=96 0ff=96 f0f=96 ff0=96 222=96
[138.476] MUYU FB   nz=2304/2304 distinct=8      （GPU 提交路径，96×24 网格）
[138.500] MUYU FBC  fff=288 00f=288 ... 222=288
[139.532] MUYU PFB  n=2 ... ch=0/768
[141.624] MUYU PFB  n=3 ... ch=0/768            ← 最后一行 PFB
```

96×8=768、288×8=2304，两个网格的**颜色分布严格等比**；`MUYU FBCHK` 的
`windowFor()` 与 `sampleWord()` 也一直逐字相同。至此三条互不共享映射的读法
（板级 27 MB `frameWindow`、逐页 `sampleWord`、显示驱动 `bootMapping`）指向同一块
`0xE3940000`，§69.95「面板扫的就是这块内存」不再有任何间接性。
`MUYU TENB skip=1`、`MUYU PREEMPT mode=0`、`MUYU TTBR mode=1` 同 fix136。

### 69.100.3 第二个写者与显示驱动无关

```
[141.624] MUYU PFB n=3                         ← 显示驱动采样在此后停止
[142.413] MUYU FB  nz=2210/2304 distinct=9     ← 面板开始被覆盖
[142.950] MUYU FBC fff=287 000=94 ...          ← 黑点增长
[142.950] MUYU FBD ch=174/2304 rows=0..23
[143.029] MUYU FB  nz=1828/2304 distinct=9     ← 最后一帧
[143.098] MUYU ABI x1404 p50 n2 deleted s2/e00002d6
```

- `PFB` 停在 141.624，而面板在 142.413 之后才变 ⇒ **写者不是 display work loop**：
  `kickOnce()` / `vsyncTick()` 都已经不跑了，它仍在写物理内存。
- 整轮 **0 行 `MUYU BLANK`**（改成前 3 次也打之后依然为 0）⇒ 不是 blank swap 的
  `0xff000000` solid-fill；**0 行 `MUYU OFFER`** ⇒ 不是 GPU offer；`skip=1` ⇒
  不是 tenBit 转换。
- 尾段只有 `watchdogd CRITEXIT`（142.210）与 GPU client 的 `e00002d6`（143.098），
  没有 panic / Debugger 行。最可能的两个来源是 bring-up 的 5 s dump 控制台
  （`BravearkLastLog.c` 的 ticker 本来就会重画面板，且 `native_legacy_swap_hold`
  会停掉 swap），或 GPU client 通过 `NGP_MEMORY_SCANOUT_RW` 直接写 scanout；
  两者都解释「左半屏文字状黑块 + 显示工作循环停摆」。下一轮把 `MUYU DUMP`
  与 `PFB` 对齐即可定名。
- **`MUYU SCAN` 28 次全是 `zero=1275 canary=4 other=1 fail=0`**，与 fix130/136
  逐字相同 ⇒ ROP 仍一个字节没写。

### 69.100.4 结论与订正

> **§69.99.3 的「第二个写者」要再收窄**：它不是显示合成路径的写者，而是
> GPU 进入 hang / 客户端重启那一瞬的 dump 或 scanout 直写。
> 在 GPU 正常提交的 5.5 s 窗口里（fix136 的 137.1–140.5 s、fix137 的
> 136.9–141.6 s），两个独立读取器都报 `ch=0`、8 色严格均分 ——
> **`swC` 之后面板缓冲完全静止，没有任何周期性覆盖者**。

所以当前的因果链是干净的三段：

1. `NativeLegacyDisplay.cpp` 的 `if(tenBit)` 每帧刷全屏黑并 `return` 短路 GPU offer
   —— 已由 `swC` 关掉（§69.98）。
2. 关掉之后，面板缓冲就是被扫描的那块内存、可以稳定控制、静止不动
   —— fix135/136/137 三个读取器互证，**且用户已肉眼确认彩条上屏（§69.101）**。
3. **唯一剩下的病根是 GX 侧 store 不落地**：CP 的 `CP_EVENT_WRITE7` 能写、
   3D/blit 全不能写、无 SMMU fault、`SCAN` 上百分样本全零（§69.97）。

⇒ 显示层再怎么修都不会凭空产生桌面；要让 GPU 合成出画面，必须先让
`RB/ROP/UCHE` 的 store 抵达 DRAM。

### 69.100.5 下一步（收敛后只剩两条）

1. **GX store（真正的瓶颈）**：解析 `CP_SET_DRAW_STATE`（`PKTS op43=5`、
   `sds=563`）的间接表，读出 turnip 下发的完整 RB 状态；同时把 GX 域的
   SMMU 侧普查从 8 个 slot 扩到全部 `SMR/S2CR` + `CBAR/CBFRSYNRA` +
   `TTBR0`，确认 UCHE/RB 事务与 CP 落在同一个 context bank。
   §69.99.1 已排除「照 uke 补 SID 1」这条路，不要再走。
2. **显示层的结构性短路**（可以并行、但不会单独带来桌面）：补 uke 的
   `format` 参数并让 tenBit 帧走 `native_legacy_gpu_offer`，按 fix71 的教训
   同时处理 `transferred` / `finishGpuSwap`；21H461 不能照搬 24A446 的
   `patchFramebufferBranch()`。
3. ~~用户肉眼确认~~ **已完成，见 §69.101**。

## §69.101 用户肉眼确认彩条上屏：scanout 端到端结案（2026-11-15 13:50）

> 用户回复「看见了」，对应 fix135 / fix136 / fix137 三轮在 `0xE3940000`
> 画的 8 条全高彩色竖带（白/蓝/绿/红/青/品/黄/深灰）。这是本项目第一次
> **由人眼、从真实面板**确认内核写入的物理内存被扫描出来，不再依赖任何
> 内部计数器或 ASCII 采样。

### 69.101.1 由此一次性排除的假设（结案，不再投入）

| 假设 | 判定 |
| --- | --- |
| scanout 没接上 / DPU 扫的不是 `0xE3940000` | **证伪**：CPU 写这块内存，人眼在屏上看到 |
| 面板格式/`b3a8` 双平面布局导致控制器解不出画面 | **证伪**：ARGB32 直写即可显示，无格式转换 |
| stride / 宽高 / plane 偏移配错 | **证伪**：彩条按 8 等分全高出现，几何正确 |
| framebuffer 是另一块 shadow buffer，需要一次回写才能上屏 | **证伪**：无回写，写入即刻可见 |
| cache / DSB 导致 CPU 写没到 DRAM | **证伪**：`dsb sy` 后即上屏 |
| IOMFB / DCP 需要一次 swap 提交才刷新内容 | **证伪**：没有 swap 也持续可见 |

⇒ 之前所有「黑屏 = 显示后端接线问题」的分支整族关闭。
**黑屏从现在起只有一个解释：面板缓冲里装的是黑/空内容。**

### 69.101.2 因果链收敛为两段

1. **内容来源被掐断**（显示层）：tenBit 分支每帧把空的 `b3a8` 表面转成
   全屏 `0xff000000` 并在 `native_legacy_gpu_offer()` 之前 `return`。
   `swC` 只跳过了 `out[]` 写入，**没有跳过那个 `return`** ⇒ fix135–137 里
   面板保持彩条静止、`MUYU OFFER` 几乎不出现，正说明 GPU offer 通路仍被短路。
2. **GPU 侧 store 不落地**（§69.97）：CP 能写、3D/blit 全不写、
   无 SMMU fault、`SCAN` 全零。即使 offer 接通，ROP 不写就没有合成内容。

两段都要修，但**第 1 段是本树自己的代码、可立即验证**，而第 2 段是上游
KMD/Turnip 状态。下一轮先接通 offer：只有 offer 真的发生，
`MUYU SCAN` 的零结果才有意义（否则可能在扫一块没被提交过的表面）。

### 69.101.3 判据纪律不变

用户看见的是**彩条**，不是桌面。按 §69.76.2 钉死的判据，这**不算 GPU 接管
首帧**；「进桌面 / 有图标 / 能滑动」同样不算。GPU 渲染成立的唯一判据仍是
`IMW nz` / `Z nz` 上升、canary `alive` 下降、`SCAN` 上引擎写入的样本出现。
本次确认只坐实**显示后端**，不触碰 GPU 判据。

### 69.101.4 下一步（fix138）

1. 用等长载体 `sheng-tum=swD` 在 `swC` 基础上**去掉 tenBit 分支的 `return`**，
   让帧继续走到 `native_legacy_gpu_offer`；补 uke `LegacyDisplayProbe.cpp`
   的 `format` 参数，区分 `NGP_DIRECT` / `NGP_BGRX8888`；按 fix71 教训同时
   处理 `transferred` / `finishGpuSwap`，避免第 4 帧冻结。
2. 同时做纯只读诊断：解析 `CP_SET_DRAW_STATE`（`PKTS op43=5`、`sds=563`）
   的间接表，读出 turnip 下发的完整 RB/MRT 状态；把 GX 域 SMMU 普查从
   8 个 slot 扩到全部 `SMR/S2CR` + `CBAR/CBFRSYNRA` + `TTBR0`。
3. 保留 `swC` 作为可回退对照；**不要补 SID 1 路由**（§69.99.1 已结案）。

## §69.102 fix138：offer 通路接通，但 `stride` 让客户端立刻拒帧

镜像 `fw/vendor_boot_gpu_fix138.img`（sha256
`9d28aa33414e94dbfa5d470b08a5ad8c89e7392693fee0b9a808cf88b2068aa6`），
载体 `sheng-tum=swCD`（1014/1016 B），日志
`logs/vendor_boot_gpu_fix138-logs/muyu-67315b97`（175 s、43 轮、gap=0）。

### 69.102.1 改动（只有显示层 + 纯只读普查）

| 文件 | 改动 |
| --- | --- |
| `UniXNU-muyu-17/drivers/GopFramebuffer/NativeLegacyDisplay.cpp` | 载体字母 `D`：保留 `C` 的全部行为（跳过 `out[]` 刷黑、CCU patch 不变），额外在 tenBit 分支收尾前**每帧发起一次真的 `native_legacy_gpu_offer`**，打印 `MUYU OFF10 n/acc/total/fmt/planes/stride/bytes`；**仍走 `finishTransfer` 的 CPU 收尾**，不改成 GPU 尾部，避开 fix71 第 4 帧冻结 |
| `UniXNU-muyu-17/package/GpuPowerProbe.cpp` | `MUYU S2CR` 普查从 8 slot 扩到 24 slot（分 3 行）+ `IDR0/1/2/7`，全部只读 |

`C` 同时被 `NativeAdrenoVm.cpp:890` 当作 CCU 并发-resolve 开关，所以用 `swCD`
而不是 `swD`：与 fix135–137 相比**只多一个变量**。`MUYU CCUW mode=1`、
`patched 0000006c -> 00000000` 与前三轮一致。

### 69.102.2 offer 真的接通了

```
[135.727] MUYU TENB skip=1 offer=1 carrier=swCD
[135.727] MUYU OFFER n=1 acc=0 pres=1 tc=0     ← GPU client 尚未进 waitSwap
[136.286] MUYU OFFER n=2 acc=0 pres=1 tc=1
[136.415] MUYU OFFER n=3 acc=1 pres=1 tc=2     ← 客户端开始等帧
[136.415] MUYU OFF10 n=3 acc=1 total=1 fmt=62336138 planes=2 stride=3200 bytes=34177024
[136.560] MUYU OFF10 n=4 acc=1 total=2
[136.606] MUYU OFF10 n=5 acc=1 total=3
[136.743] MUYU OFF10 n=6 acc=1 total=4
```

`acc` 连续为 1 说明 `NativeGOPPresenter::offer()` 的
`online && gpuClient && !swap.offered` 每帧都成立 —— 也就是客户端**每帧都走完了
`waitSwap → present → completeSwap`**，presenter 的 swap 被正常清空，
没有复现 fix71 的队列冻结。

⇒ **§69.98 的「`return` 短路 GPU offer」已修复并验证**：tenBit 分支不再掐断
hand-off。但 `MUYU FBC` 从 136.65 到 139.3 始终是 8 色各 288、`FBD ch=0`、
`PFB nz=768/768 distinct=8 ch=0`，面板一字节没变；`MUYU SCAN` 12 次仍全是
`zero=1275 canary=4 other=1`。

### 69.102.3 为什么 present 没写：一个几何参数

`GpuScanoutClient.h::serveOne()` 把 `NGPSwap` 交给
`GpuScanoutPresenter.h::present()`，后者第一行检查：

```cpp
const size_t row = (size_t)frame.width * 4;   // 3200*4 = 12800
if (... || frame.stride < row || ...) return kBadFrame;
```

而 offer 传下去的是 `sourceStride = 3200`（`MUYU OFF10 stride=3200`）——
这是 `b3a8` **plane 1 的每行字节数 / 像素数**，不是 plane 0 的字节 pitch。
`3200 < 12800` ⇒ `present()` 在编码任何 compute copy 之前就返回 `kBadFrame`，
`NGPComplete` 带 `0xe00002bc` 回去，`completeSwap` 清空 swap，下一帧重复。

显示驱动自己为 tenBit 算的 `checkStride = width*4 = 12800` 才是 plane 0 的字节
pitch，且 `12800*2135 + 12800 = 27,340,800 ≤ 34,177,024`（descriptor 长度）——
`present()` 的全部边界条件都能过。⇒ **fix139 只需把 offer 的 stride 换成
`checkStride`**，就能第一次让 `scanout_opaque` compute shader 真正对面板编码写。

### 69.102.4 SMMU 全 slot 普查结案

```
MUYU S2CR scr0=260406 scr1=0 scr2=0 idr0=4c017e18 idr1=40000016 idr2=00005511 idr7=00000024
 | 0 smr=80000000 s2cr=00000000 | 1 smr=00000000 s2cr=00020000
 | 2 smr=80000005 s2cr=00000001 | 3..23 全部 smr=00000000 s2cr=00020000
```

24 个 slot 里**只有 slot 0 与 slot 2 有效**，其余全是 `s2cr=00020000`（FAULT 类型），
`smr=0`。没有隐藏 stream 把 RB/UCHE 引到别的 context bank。
⇒ §69.99.1「不要照 uke 补 SID 1」**彻底坐实**；「GX 事务被悄悄路由到别处」
这条假设结案，不要再投入。SMMU 路由层面已无可查。

### 69.102.5 收敛后的状态

1. scanout 端到端已由人眼坐实（§69.101）。
2. 黑屏的两段因果，**第 1 段已修好一半**：tenBit 刷黑关掉、GPU offer 接通、
   客户端每帧完整往返。
3. 剩下的是第 1 段的最后一个几何 bug（stride），以及第 2 段 GX store。
   fix139 修 stride 后，`present()` 的 compute copy 将第一次真正尝试写面板 ——
   这是对 §69.97「GX 侧 store 不落地」最直接的独立检验：它用的是
   `newBufferWithBytesNoCopy(scanout.base)` 作为 MTLBuffer target 的 compute
   shader，与 RB/ROP 的 render-target store 不是同一条路径。

## §69.103 fix139：stride 修正后仍未写面板，第二个写者定名

镜像 `fw/vendor_boot_gpu_fix139.img`（sha256
`5cdf9ec71a470b22dde15c0f6236c37e09d40ce68e509d6488f2a2272a9baba0`），
载体仍 `swCD`，日志 `logs/vendor_boot_gpu_fix139-logs/muyu-6735c0b0`
（173 s、42 轮、gap=0、msgbuf 598781 B）。

### 69.103.1 改动

| 文件 | 改动 |
| --- | --- |
| `NativeLegacyDisplay.cpp` | tenBit 分支的 `native_legacy_gpu_offer(...,stride,...)` 从 `sourceStride`(3200) 改成 `checkStride`(=width*4=12800)，`MUYU OFF10` 增打 `raw=` 便于对照 |
| 同上 | `panelPreview()` 限流 32 → 8 tick（~7 Hz），ASCII 图阈值 `&7` → `&31`，避免刷屏 |

载体不变、CCU/UCHE/MRT 全不动，与 fix138 **只差 stride 与采样密度**。

### 69.103.2 几何修好了，但面板仍静止 5 秒

```
[137.177] MUYU OFF10 n=3 acc=1 ... stride=12800 raw=3200 bytes=34177024
[137.260] MUYU FBMARK once panel=00000000e3940000 8 bars x 400px
[137.472] MUYU FBD ch=2304/2304        ← 彩条画上
[137.644 .. 142.105] FBD ch=0 / PFB ch=0 / FBC 8 色各 288  ← 连续 5 s 完全静止
[141.999] MUYU OFF10 n=32 acc=1 total=19   ← 32 次 offer 中 19 次被接受
[142.989] MUYU FBD ch=395/2304 rows=0..23 ← 第二个写者出现
```

`present()` 的边界条件现在逐条可过（`12800 ≥ 3200*4`、
`12800*2135+12800 = 27,340,800 ≤ 34,177,024`、descriptor 页对齐），
`sourceFor()` 会把 surface 直接包成 MTLBuffer。**但 137.3–142.1 s 面板一个字节
没变** ⇒ 修正 stride 并没有让 `scanout_opaque` compute copy 落到 DRAM。

与 fix138 的差别：`acc` 从「连续为 1」变成**交替**（n=3/5/32 为 1，
n=4/6 为 0）。`NativeGOPPresenter::offer()` 的三个条件
`online && gpuClient && !swap.offered` 至少一个在交替，说明客户端拿到帧后的
行为变了（不再每帧立刻 completeSwap）。`MUYU GFXFAIL st=e00002d6` 从 136.677 s
起密集出现（早于第一帧 offer），`MUYU ABI ... deleted` 是精确 2.006 s 周期的
系统服务轮询、不是客户端崩溃。⇒ fix140 直接打印 presenter 的
`offer/wait/complete` 状态机与客户端 verdict。

### 69.103.3 第二个写者定名：bring-up dump 控制台 ticker ★

fix139 加密的 `MUYU PFB|` ASCII 图第一次拍到了它的形状（159.95 s）：

```
    @    @@      +  :  :  *  *- -- -% %%%% .....
@@@@@@      ++++++::::::******------%%%%%%......
             +        :: ** * ----- %%% %% .....
```

- 右半屏 `:: ** * --- - %% %% ......` 是**规律彩条残带**（8 色 ramp 字符）；
- 左半屏是 `@`（亮字）与空白（黑底）组成的**文字状字形**；
- 157.65/157.71/157.77/157.82 s 连出 4 个不同位置的块，逐帧滚动。

⇒ **§69.99.3 / §69.100.3 悬了两轮的「第二个写者」结案**：
就是 bring-up 的 5 s dump 控制台 ticker（`BravearkLastLog.c` /
`muyu_console_take_panel()`）在重画左半屏文字，与 GPU offer、tenBit、blank
swap 都无关。它只在 GPU 进入挂起/重启窗口后出现，只影响左半屏，
**不构成桌面内容，也不是 GPU 写像素的证据**。判据纪律不变。

### 69.103.4 仍未变的硬结论

`MUYU SCAN` 40 次全是 `zero=1275 canary=4 other=1 distinct=1 first=5a5a5a5a`，
与 fix130/136/137/138 逐字相同 ⇒ **ROP 依旧一个字节没写 34 MB surface**。
SMMU 24-slot 普查与 fix138 完全一致（只有 slot 0/2 有效）。
`MUYU HC hang n=1` 在 157.555 s，而面板变化从 142.989 s 就开始了 ——
再次印证写者是控制台而不是 GPU 提交路径。

因果链因此收窄到**一个**未知：GPU 侧的 store（无论 RB/ROP render target
还是 `present()` 的 compute copy）为什么都不落地。fix140 先把客户端 verdict
钉死，区分「帧在编码前被拒」与「编码了但 GPU 没写」。

## §69.104 fix140：客户端报告 `kOk`，DRAM 却零变化 ★ 决定性判据 ★

镜像 `fw/vendor_boot_gpu_fix140.img`（sha256
`7fb8caa343a0cf244859f3a4351db1f7d01cdfb3920ad4d555a022a67155d4c3`），
载体仍 `swCD`，日志 `logs/vendor_boot_gpu_fix140-logs/muyu-673174cb`
（139 s、25 轮、gap=0、msgbuf 493902 B）。

### 69.104.1 改动（纯诊断）

`UniXNU-muyu-17/drivers/GopFramebuffer/NativeGOPPresenter.cpp` 加 `MUYU SWAPD`
三处限流日志：`offer`（三个门控条件快照）、`wait`（客户端收到的几何）、
`done`（客户端 `NGPComplete.status` 的 verdict）。不写任何寄存器。

### 69.104.2 完整状态机（第一次逐帧可见）

```
[133.506] MUYU SWAPD offer n=1 acc=0 online=1 client=0 busy=0 seq=0 stride=12800
   ... n=1..7 全部 acc=0，唯一原因是 client=0（GPU 客户端尚未进 waitSwap）
[134.556] MUYU SWAPD offer n=8 acc=1 online=1 client=1 busy=0
[134.556] MUYU SWAPD wait  n=1 seq=1 addr=000000011696c000 bytes=34177024
                             stride=12800 3200x2136 fmt=1
[134.899] MUYU SWAPD done  n=1 seq=1 status=00000000 map=1
[135.103] MUYU SWAPD done  n=2 seq=2 status=00000000 map=1
[135.375] MUYU SWAPD done  n=3 seq=3 status=00000000 map=1
[135.824] MUYU SWAPD done  n=4 seq=4 status=00000000 map=1
[136.183] MUYU SWAPD done  n=5 seq=5 status=00000000 map=1
[138.479] MUYU SWAPD offer n=32 acc=1 ... seq=16
```

- **几何完全正确**：`stride=12800 = 3200*4`、`3200x2136`、`fmt=1(NGP_BGRX8888)`、
  `bytes=34177024` 页对齐。`present()` 的所有 `kBadFrame` 条件都过了。
- **`status=00000000` = `kOk`**：`GpuScanoutClient.h` 只在
  `commands.status == MTLCommandBufferStatusCompleted` 时发 0；
  `kBadFrame/kNoResource/kGpuFailed` 会发 `0xe00002bc`。
  ⇒ `scanout_opaque` compute shader **编码、commit、waitUntilCompleted 全部成功**，
  客户端连做 5 帧以上，没有一帧失败。`map=1` 说明 surface 映射也在。
- `offer` 的 `busy=0` 说明 swap 每帧被正常清空，fix71 的队列冻结没有复现；
  前 7 次 `acc=0` 只是客户端还没启动，不是错误。

### 69.104.3 但是：面板一个字节没变

```
[134.648] MUYU FBMARK once panel=00000000e3940000 8 bars x 400px
[134.830 .. 139.542] FBC 8 色各 288 / FBD ch=0 / PFB nz=768 distinct=8 ch=0
[132.998 .. 139.631] MUYU SCAN 31 次全 zero=1275 canary=4 other=1 distinct=1
```

即：`present()` 报告成功把 34 MB surface compute-copy 进了 scanout，
可 `0xE3940000`（人眼已确认就是被扫描的那块 DRAM）仍是那 8 条彩带，
而 GPU 的 render-target surface 也依旧全零。

### 69.104.4 由此排除 / 坐实

| 假设 | 判定 |
| --- | --- |
| 帧在编码前被客户端拒绝（`kBadFrame`） | **证伪**：`status=0`，几何全对 |
| GPU 客户端崩溃 / swap 卡住 / fix71 冻结 | **证伪**：连续 5+ 帧 `wait→done`，`busy=0` |
| compute shader 编译或资源失败 | **证伪**：`kOk` 要求 commandBuffer Completed |
| GPU 挂死导致没写完 | **证伪**：这几帧早于 `HC hang`，且 wait 正常返回 |
| CPU 读的是 stale cache | **证伪**：§69.101 人眼确认 + 三个独立映射读法互证 |
| **GX 侧 store 根本没抵达 DRAM** | **坐实**，且范围从 RB/ROP 扩大到 compute/blit |

⇒ 现在只有一种形状能同时解释「CP 的 CX 侧写能落地（`5a5a5a5a` 哨兵）、
所有 GX 侧写（render target、compute copy、blit）都报告成功但 DRAM 不变、
且 SMMU 无 fault」：**GX 事务要么被路由到了另一块物理页，要么停在
CCU/UCHE 里从未 flush 到 DRAM**。§69.97.3 的架构分界不再是推断，
而是有客户端 `kOk` 与面板零变化的直接对照。

### 69.104.5 下一步（按优先级）

1. **查 GPU scanout BO 的 IOVA→物理映射**：`present()` 用
   `newBufferWithBytesNoCopy:scanout.base` 建 `target_`，Metal bridge 把它 import
   成 GPU BO 并分配 IOVA。核对这个 IOVA 在 GPU 页表里指向的物理页是否真是
   `0xE3940000`；若指向别处，GPU 一直在往另一块 DRAM 写，`kOk` 与面板不动
   就同时成立。这是目前唯一能解释全部现象、且尚未读过的状态。
2. **解析 `CP_SET_DRAW_STATE`（`op43`、`sds≈563`）间接表**：见
   `adreno_pm4.xml:1093`，每个 group 是 3 dword
   `{COUNT|DIRTY|DISABLE|BINNING|GMEM|SYSMEM|GROUP_ID, ADDR_LO, ADDR_HI}`，
   指向一段 type4 寄存器流 —— fix131/132 找不到的 `RB_MRT0_*` 就在里面。
   复用 `NativeAdrenoVm::scanIbPacketsAt()` 的 `physicalDword()` 逐页读即可。
3. **`RB_CCU_CNTL` 重开的风险**：§69.93 曾把 `0x6c` 清成 0 来解决挂死
   （提交 11→150+），§69.97.3 判定「只解决挂死、不解决写」。清掉
   concurrent resolve 是否本身就阻止了 store flush，至今**没有可比实验**
   （fix120 之前 GPU 挂死、没有 SCAN 数据）。但开回去会重新挂死，
   不要单独回退；要试必须同时处理 BV 等待。
4. **用户肉眼确认（2026-11-15 14:18）**：「左半屏文字/黑块、右半屏仍有彩条，
   但没有出现过桌面或图标」。⇒ §69.103.3 把 142 s 写者定名为 bring-up dump
   控制台 ticker 得到人眼坐实；更关键的是 **面板缓冲里从来没有任何桌面/
   图标合成内容**，与 `FBC/FBD/PFB` 的 8 色彩带逐字一致。至此
   「scanout 通、内容空」被两端独立坐实，黑屏原因 100% 落在 GPU 侧 store
   不落地，显示层已无可修。

### 69.104.6 补做 §69.104.5 第 1 项：IOVA / 页表侧读数（离线，fix140 日志）

不刷机，直接读 fix140 已有的日志，把「GPU 一直在往另一块物理页写」压到只剩
PTE 属性一种可能：

```
[133.552] MUYU CB    sctlr=167 tcr=38071 ttbr0=0010000909017000 fsr=400
[131.232] MUYU TSW   ttbr=0010000909017000 tlb=0 asid=10
[134.549] MUYU IMPM  h=7d iova=0000000126f5e000 bytes=1a13000
                     phys=00000000e3940000 ro=0 perm=3 coh=0
[134.619] MUYU FBWIN p=00000000e3940000 bytes=1a13000 va=0xfffffff393b0a000
[133.555] MUYU S2CR  slot0 smr=80000000 s2cr=00000000   (SID 0 -> bank 0)
                     slot2 smr=80000005 s2cr=00000001   (SID 5 -> bank 1 / GMU)
                     slot1,3..23 全 smr=0 s2cr=00020000 (FAULT)
```

- `newBufferWithBytesNoCopy:scanout.base` 的 import **就是** `h=7d`：IOVA
  `0x126f5e000` → 物理 `0xE3940000`，长度 `0x1a13000`，`ro=0 perm=3`（可写），
  `coh=0`。⇒ **不是「GPU 在往另一块 DRAM 写」**：目标页和 CPU 彩条页是同一块，
  且 import 时明确授予写权限。
- CPU 写入的 TTBR0（`MUYU TSW`）与 SMMU bank 实际持有的 TTBR0（`MUYU CB`）
  **逐字相同**（`0010000909017000`，ASID `0x10`）⇒ 换表 mode 1 生效，GPU
  跑的正是包含 `h=7d` 的那张客户端表，「页表切错 / GPU 用内核表」也排除。
- 剩余未读状态只剩**叶子 PTE 的属性位**（`coh=0` 的 writeback / shareability /
  是否标成 read-only 或 device-nGnRnE）与 **TLB 是否装了这条翻译**。二者都要
  页表 walk 才能定，属于纯诊断，不改变上面的「store 不落地」结论。

## §69.105 fix141：补第二条 SMMU 路由 = 硬挂死（2026-11-15 14:33）

### 69.105.1 动机与改动

本板 DTS `qcom,kgsl-iommu@3da0000` 明确写 `gfx3d_user` SID 0、
`gfx3d_secure` **SID 2**、`qcom,gmu@3d69000` SID 5 —— 而
`GpuPowerProbe.cpp::gpuStart()` 的旧注释抄的是 uke 的「SIDs 0 and 1」，
且 `kSecondSid=UINT16_MAX` 只租了第一条路由。24 slot 普查（§69.102）显示
只有 SID 0（slot 0 → bank 0）与 SID 5（slot 2 → bank 1）有 SMR，
**SID 2 完全未匹配**。若 GX 侧事务走 SID 2，`SCR0.USMFEN=0` 时未匹配流
未翻译直通，就能同时解释「CP(SID 0) 写落地、GX store 报 kOk 但 DRAM 不变、
GFSR=0」。

fix141 用等长载体 `sheng-tum=swCDE` 新增字母 `E`：把第二条路由改成
**本板正确的 SID 2**、放进普查里确认为空的 **slot 3**、
`S2CR=0`（bank 0），由 `NativeAdrenoGpuContextBringup(..., slot1=3, sid1=2)`
走 kit 的 `ArmSmmu4KAttachStreams` 完整 scan / 写 / 读回校验。

### 69.105.2 结果：整轮没有日志，因为设备彻底挂死

镜像 `fw/vendor_boot_gpu_fix141.img`（sha `06bde495…`，77,393,920 B），
离线校验全过（trust/addon 均 fix107、`swCDE`、`console-quiet=1`、
kernel_args 1015 B）。14:33:20 刷入 + RAM boot，14:33:24 起等待自复位：

```
ERROR: no restart after 420s: the watchdog did not bite. The tablet is hung;
       hold power 10-15 s and rerun.
[420 s 后] adb devices        -> 空
           fastboot devices   -> 空
           adb reboot bootloader -> "no devices/emulators found"
           system_profiler SPUSBDataType -> 无 iPad/Apple Mobile 设备
```

连 USB 都从总线上消失，`braveark-hold-reboot=1` + `sheng-autoreset=300`
的 watchdog 也没咬 —— 比 fix9..fix140 任何一次 GPU hang 都深，是
`ArmSmmu4KAttachStreams` 写第二个 SMR 当场锁死 SMMU/总线，与 2026-11-11
记录的「breadcrumb 0x1b: S2CR accepted, SMR never returned, CPU never came back」
完全同形。本轮无 recovery、无日志窗口（设备从未回来），**挂死本身即判据**。

### 69.105.3 结案

| 假设 | 判定 |
| --- | --- |
| 第二条路由挂死是因为 SID 用了 uke 的 1 / slot 1 冲突 | **证伪**：换成本板 DTS 正确的 SID 2 + 空闲 slot 3，仍挂死 |
| 补 SID 2 路由能让 GX store 落地 | **不可测**：补路由必然硬挂死，无日志可判读 |
| SMMU 路由层还能再动 | **结案：永远只留一条路由** |

⇒ 挂死原因是「写第二条 SMR/S2CR」这个动作本身，与 SID / slot 无关。
§69.99.1 的「不要补 SID 1」升级为 **「本固件不得添加任何第二条板级路由」**。
代码已回退为 `static const unsigned kSecondSid = UINT16_MAX;`（与 fix140
逐字一致），并在 `gpuStart()` 钉上 fix141 的硬挂死注释，防止后人再试。
载体 `E` **永久废弃**，下一轮回到 `swCD`。

「GX 走未匹配 SID 2」这一假设既不能证实也不能证伪，但不构成可修路径：
即便成立，修复手段本身会让设备无法启动。结合 §69.104.6（import 目标页
正确、TTBR0 一致），后续不再投入 SMMU 路由，火力集中在
**叶子 PTE 属性 / TLB** 与 **`CP_SET_DRAW_STATE` 间接表里的完整 RB/MRT 状态**。

### 69.105.4 设备恢复（需人工一步）

fix141 的 `vendor_boot` 仍在槽 a，设备硬挂。请**长按电源 10–15 s** 直到
出现 fastboot，然后执行：

```bash
cd /Users/diannao/pad7pro-xnu
/opt/homebrew/bin/fastboot devices          # 应列出 a20248f7
bash tools/muyu-flash-mode.sh stock         # 还原原厂 vendor_boot
/opt/homebrew/bin/fastboot reboot recovery  # 回 recovery
```

还原后设备恢复充电与常规闭环。（下一轮的实际载体见 §69.106.5：`swCDF`。）

## §69.106 离线复核（fix141 之后、设备硬挂期间，未刷机）

设备仍在等待人工长按电源，本轮**只做只读日志/代码分析**，不写设备。
目的是把「叶子 PTE / TLB」这条待办缩到最小，并找出唯一还没做过的
单变量对照。

### 69.106.1 `idr2=00005511` ⇒ `input_bits=48`，4.6 GiB 的 IOVA 合法

fix138/139/140 三轮 `MUYU S2CR ... idr2=00005511` 完全一致：

```
UBS = (idr2>>8)&15 = 5  -> NativeAdrenoSmmuInputBits() = 48 (TTBR0 aperture)
OAS = (idr2>>4)&15 = 1  -> 36-bit 输出
```

`GpuClientVaStart=0x100000000`(4 GiB)、scanout BO `0x126f5e000`(4.6 GiB)、
source `0x11696c000`(4.37 GiB) 全在 48-bit TTBR0 内。
⇒ 「客户端 IOVA 超出 aperture」「地址落在 TTBR0/TTBR1 空洞」**排除**。

### 69.106.2 fault / stall / 电源全部排除

- `MUYU CB ... fsr=400 fsynr=1f0d7b far=0`：kit 的 `FSR_FAULT=0xc00001fe`
  不含 bit10，`0x400` 不是 fault；两次快照（133.55 / 133.96 s）相同，是残留态。
- `MUYU GX ... stall=0`（RBBM_STATUS3 bit24 = SMMU stalled on fault）三轮 16 次全 0；
  `on=1 gdscoff=0 clkoff=0`，GX 电源/时钟都在（§69.99 已排除电源）。
- 24-slot 普查只有 SID 0/5 有路由、其余 FAULT；GFSR 长期 0；SMMU 路由层已结案。
- 页表 leaf 由 `ArmSmmu4KLive.c::descriptor()` 构造：`AF=1`、`nG=1`、
  `AP` 给出非特权读写、`SH=2/3`、`AttrIndx=0`（`coh=0` → MAIR0 `0x44`
  = Normal Non-Cacheable）；`writeback=1` 才是 `0xff`。属性本身没有只读位，
  也没有 Device 类型。`MUYU TSW` 与 `MUYU CB ttbr0` 逐字相同（§69.104.6），
  TLBIALL 在每次切表时执行。⇒ 「PTE 属性写保护 / 页表切错」无证据。

### 69.106.3 「清理事件不够」整族早已证伪，别再补 flush

`NativeAdrenoRing.cpp:356-394` 的注释记录 fix95/fix104 已经识别并试过：

| 载体 | fence 追加 | 结果 |
| --- | --- | --- |
| 基线 | `CACHE_CLEAN(0x31)` | 基线 |
| `sw5` | `CACHE_FLUSH7(0x32)` | §69.89.1 fix112：canary 仍 64/64 |
| `swc` | `CCU_CLEAN_COLOR(0x21)` | §69.95.6 fix127 后 c21=275，每次都发，ROP 仍不写 |

⇒ 不是「像素停在 CCU/UCHE 从没被推回内存」。`present()` 的 compute copy
报告 `kOk` 而面板不动，也不能再用这条解释。

### 69.106.4 诊断盲区（不是病根，但要补）

- `MUYU CB` 打印的 `tcr=38071` **读的是 `+0x10` = CBn_TCR2**（kit
  `ArmSmmu4KProgram.c:10-11` 确认 TCR2 在 `+0x10`、真 `CBn_TCR` 在 `+0x30`）。
  真 TCR 的 `T0SZ/T1SZ/EPD0/EPD1` 从来没被读过。
- 同一行只读了 FAR 低 32 位和 FSYNR0；FAR 高半、FSYNR1、
  全局 GFSR/GFSYNR0/1/GFAR64 都没读。
- `MUYU GX` 的 `UCHE_CACHE_WAYS(0x0e17)` 三轮每轮 16 次全是
  `ways=00000000/00000000`，同时 `uche=0`（UCHE 从不 busy）。fix113 写进去
  被抹掉、fix116 强行开起来后 store 仍不落地（旧 canary 判据，可在精确
  面板判据下复核，但优先级低于下面的对照）。

### 69.106.5 fix142：已编译、离线校验通过、**未刷机**

镜像 `fw/vendor_boot_gpu_fix142.img`（77,393,920 B），载体
`sheng-tum=swCDF`（与 `swCDE` 等长，kernel_args 1015 B；`E` 已废弃）。
校验：trust/addon 均 fix107、`swCDF` 存在、无 `swCDE`/fix99 残留、
`console-quiet=1`。

改动一（**唯一的行为变量**）：`F` 让 `NativeAdrenoVm.cpp` 的 `ccuMode` 归零，
即**不 patch** `RB_CCU_CNTL(0x8e07)` 的 `0x6c` 位。fix120 把这两组
`CONCURRENT_RESOLVE/UNRESOLVE` 位清成 DISABLED 后，引擎不再死锁、开始产出
整帧 surface（§69.93.2）；但此后 ROP store 一个字节都没落地。A735 上这两位
同时也是 GMEM→sysmem resolve 通路，「**把死锁换成静默不写**」是从未做过的
单变量对照。`C`（跳过 tenBit 刷黑）/ `D`（接通 offer）全部保留，判据仍是
`FBC/FBD/PFB/SCAN/IMW`。若关掉 patch 后回到 fix113–119 的 GFXFAIL/HC hang，
那本身也是结论：fix120 是必要的，store 不落地另有原因；该配置历史上
watchdog 都能自复位，不会重演 fix141 硬挂。

改动二（纯只读，不写任何 SMMU/GPU 寄存器）：`MUYU CBX` 读真 TCR `+0x30`
并解出 T0SZ/T1SZ/EPD0/EPD1，加 ACTLR/CONTEXTIDR；`MUYU CBF` 补 FAR 高半、
FSYNR1、GFSR/GFSYNR0/1/GFAR64、`idr2` 与实算 `ias/oas`。
原有的 `MUYU CB` 行保持原样，历史判读脚本不变。

### 69.106.6 设备恢复流程

fix141 的 `vendor_boot` 仍在槽 a。请**长按电源 10–15 s** 直到出现 fastboot；
后台守护会在设备重新枚举时自动执行 `tools/muyu-flash-mode.sh stock` +
`fastboot reboot recovery`。若守护超时，手动执行 §69.105.4 的三条命令。
设备回来后直接上 `fw/vendor_boot_gpu_fix142.img`（无需重新构建）。

## §69.107 fix142 结果：CCU 对照证伪，真 TCR 补齐，病根收窄到 Turnip 渲染模式

镜像 `fw/vendor_boot_gpu_fix142.img`（sha `7f63e28e…`），日志
`logs/vendor_boot_gpu_fix142-logs/muyu-67532d47`（kernel 174 s、42 轮、
gap=1 s）。设备由守护自动还原原厂并回到 recovery 后开跑；**watchdog 203 s
正常自复位**（对比 fix141 的 420 s 硬挂），说明关掉 CCU patch 只是普通 GPU hang。

### 69.107.1 对照生效，但 store 一点没变

```
MUYU CCUW mode=0 off=1           ← F 生效，RB_CCU_CNTL 0x6c 保持客户端原值
MUYU TENB skip=1 offer=1 carrier=swCDF
MUYU SCAN n=1..46  zero=1275 canary=4 other=1 distinct=1   ← 与 fix130/140 逐字相同
MUYU SWAPD done n=1..32 status=00000000 map=1               ← compute copy 仍全 kOk
MUYU FBC 8 色各 288 / FBD ch=0 静止，151 s 后 000/222 黑块灰字（dump ticker）
GFXFAIL m=6 st=e00002d6 从 135.2 s 密集，HC hang n=1 在 161.5 s
```

关掉 fix120 的 `0x6c` 清零后回到 fix113–119 的 GFXFAIL/HC hang 形态，
但**引擎并没有因此更会写**：`SCAN` 仍全零、`present()` 仍报 kOk 而彩条不动。
⇒ **「fix120 用静默不写换掉了死锁」证伪**，`RB_CCU_CNTL` 的
CONCURRENT_RESOLVE/UNRESOLVE 位不是病根。fix120 是必要的（否则提交死锁），
但不是 store 不落地的原因。

### 69.107.2 `MUYU CBX/CBF` 把 SMMU 最后一块盲区补上

```
MUYU CBX tcr=80803510 t0sz=16 t1sz=0 epd0=0 epd1=1 tcr2=00038071 actlr=301 cid=9effd9fb
MUYU CBF fsr=00000400 far=73b7dfbf00000000 fsynr=001f0d7b/0000fbff
          gfsr=00000000 gfsynr=00000000/00000000 gfar=0000000000000000
          idr2=00005511 ias=48 oas=36
```

- 真 `CBn_TCR` 的 `T0SZ=16` ⇒ TTBR0 覆盖 `2^48`，`EPD0=0` 启用、
  `EPD1=1`（TTBR1 禁用）。4 GiB / 4.37 GiB / 4.6 GiB 的客户端 IOVA
  全部在 TTBR0 内，**aperture 彻底排除**（原来那行 `tcr=38071` 是 TCR2）。
- `GFSR/GFSYNR/GFAR` 全 0；`FSR=0x400` 不在 kit 的 `FSR_FAULT=0xc00001fe`
  掩码内（bit10 是 SS 残留态），`GX stall=0`。FAR 高半/FSYNR1 的非零值
  是未发生 fault 时的残留，不构成活跃故障。
- ⇒ SMMU 的 aperture / fault / stall / 路由 / TTBR / PTE 属性全部排除，
  **SMMU 这一整层结案**。

### 69.107.3 新线索：`TU_DEBUG` 从来只有 `nocb`，Turnip 一直在 GMEM 模式

`UniXNU/drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm:244-248`：

```objc
// Every pass renders in system memory with the 8-bit BGRA framebuffer (sheng-fb-bgra=1) ...
setenv("TU_DEBUG", bootArgSet("sheng-fb-bgra=1") ? "sysmem,nocb" : "nocb", 0);
```

本板 cmdline 里**没有** `sheng-fb-bgra=1`（uke 默认开了），所以每个 Metal
进程拿到的是 `nocb`（只关并发 binning），**Turnip 仍按 GMEM/tile 模式编码
render pass**。fix120 只在内核里 patch 了 `RB_CCU_CNTL` 的两个 resolve 位，
并没有让 Turnip 真正进入 sysmem 路径；fix142 证明把这两个位放回去也一样
不写。两边合起来 ⇒ 可疑点从「CCU 开关」移到「**Turnip 的 pass 到底有没有
发出 sysmem store**」。

注入通道有两条，都不需要重编 Mach-O：
1. `sheng-tu-debug=<flags>`（同文件 :232-243，从 `kern.bootargs` 读并
   `setenv(...,1)` 覆盖），或
2. `/private/var/ngpu/tu_debug` 文件（:227-231，`setenv(...,1)` 覆盖）；
   addon 是 `NGPUADD2` 容器，每个文件 zlib 压缩，可以只加一个文本文件。

### 69.107.4 顺带核实：compute pipeline 的符号都在

解包 `fw/gpuaddon-17-fix107.bin`（10 个文件）后，
`addons/NativeAdrenoMetal.bundle/NativeAdrenoMetal` 里同时存在
`scanout_opaque`、`scanout_convert`、`nmb_scanout_convert_air`、
`native_bridge_metal_library_with_air`、`newBufferWithBytesNoCopy`。
⇒ `GpuScanoutPresenter::pipeline()` 具备加载 AIR 的条件，`present()` 有资格
走 compute shader 而不是退到 blit；`status=kOk` 不是「空编码」的假象。

### 69.107.5 fix143（下一步）

单变量：把 `UniXNU-muyu-17/board.toml:623` 的
`ml-timeout-lock-panic-scale=40`（只影响 panic 锁超时，与 GPU 判据无关）
换成等长量级的 `sheng-tu-debug=sysmem,nocb`，让所有 Metal 进程强制
sysmem render。只重建 payload，不动 kernel / addon / trust。
判据不变：`FBC/FBD/PFB` 是否被 GPU 写入覆盖、`SCAN canary` 是否下降、
`SWAPD status`。若彩条被桌面内容覆盖 ⇒ 病根是 Turnip 的 GMEM pass；
若仍全零 ⇒ 连 sysmem pass 的 store 都出不去，下一步解析
`CP_SET_DRAW_STATE` 间接表拿完整 RB/MRT 状态。

## §69.108 fix143 结果：**Turnip 一直就在 sysmem**，这条假设整族证伪

镜像 `fw/vendor_boot_gpu_fix143.img`（sha `ec8149d9…`），日志
`logs/vendor_boot_gpu_fix143-logs/muyu-c455b090`（kernel 254 s、42 轮、
gap=0）。离线校验：`kernel_args` 1010 B，含
`sheng-tu-debug=sysmem,nocb`、不含 `ml-timeout-lock-panic-scale=40`、
载体 `swCD`、trust/addon 均 fix107。watchdog 202 s 正常自复位。

### 69.108.1 注入生效，但 store 与基线逐字相同

```
MUYU USTEXT R g0 a0 s14 e0 t0 u0 x1 m1 d1 k0/0/34319   <- 0 GMEM / 14 sysmem pass
MUYU SCAN n=1..11  zero=1275 canary=4 other=1 distinct=1
MUYU FBC fff=288 00f=288 ... 222=288 / FBD ch=0       <- 8 色彩条全程静止
MUYU SWAPD done n=1..5 status=00000000 map=1
MUYU SWAPD done n=6 status=e00002bc (kGpuFailed) + HC hang n=1  <- GPU hang 时确实会报错
MUYU CCUW mode=1 off=0 patched RB_CCU_CNTL 0000006c -> 00000000
MUYU GX  ways=00000000/00000000 uche=0 stall=0 on=1   <- 每轮都是 0
```

`status=e00002bc` 是本轮新出现的一条：GPU hang 时 `present()` 确实返回
`kGpuFailed`，反过来证明前面 5 次 `status=0` 是真的 `kOk`，不是状态机
永远报成功。面板仍然一个字节没变。

### 69.108.2 关键纠正：`g0 a0 sN` 在**所有**历史轮次都成立

把五轮的 `MUYU USTEXT R` 摆在一起（`NativeBridgeMetalFrontend.mm:4852-4857`：
`g`=GMEM pass、`a`=autotune sysmem、`s`=sysmem/empty、`k`=GMEM/autotune/
other-sysmem 千像素）：

| 轮次 | 最后一行 | GMEM 千像素 | sysmem 千像素 |
| --- | --- | --- | --- |
| fix138 | `g0 a0 s12 k0/0/20649` | 0 | 20649 |
| fix139 | `g0 a0 s56 k0/0/321389` | 0 | 321389 |
| fix140 | `g0 a0 s35 k0/0/177854` | 0 | 177854 |
| fix142（CCU off） | `g0 a0 s61 k0/0/355564` | 0 | 355564 |
| fix143（sysmem 注入） | `g0 a0 s14 k0/0/34319` | 0 | 34319 |

**五轮全部 `g=0`、`a=0`、第三个 `k` 非零** ⇒ Turnip 从来没有进过
GMEM/tile 模式，每个 pass 都是带真实渲染面积的 sysmem pass。
`sheng-fb-bgra=1` / `sheng-tu-debug=sysmem,nocb` 对这块板**是空操作**；
§69.107.3 把「Turnip 仍在 GMEM」当成嫌疑是错的，实验把它排除了。
（fix143 的 `s`/`k` 更小只是因为 GPU 在 219 s 就 hang、pass 数没跑满。）

### 69.108.3 结案后的病根描述

以下整族已排除：SMMU（aperture 48-bit / fault / stall / 双路由 /
TTBR / PTE 属性）、GX 电源时钟、flush 事件（0x31/0x32/0x21）、CCU
并发 resolve 开关（fix142 对照）、Turnip 渲染模式（fix143）、显示层
（tenBit 刷黑 + offer 短路 + stride，fix135/138/139）、scanout 通路
（用户肉眼确认彩条）。

剩下的唯一形状：**Turnip 编码了 sysmem render pass，GPU 的 fence 完成、
`present()` 的 compute copy 报 kOk，CP 的 CX 侧写能落地，但 GX 侧
（RB/ROP/UCHE/compute）的 store 一个字节都没到 DRAM，且无 fault、无 stall。**

持续异常只剩一个读数：`UCHE_CACHE_WAYS(0x0e17)` 在每一轮每一次
`MUYU GX` 快照里都是 `0/0`、`uche=0`。fix113 写入后被抹掉、fix116 强行
开起来后旧 canary 判据说「store 仍不落地」——但那是 canary 时代的判据，
值得在现有精确面板判据（FBC/FBD/PFB/SCAN）下重做一次。

### 69.108.4 收尾

`UniXNU-muyu-17/board.toml:623` 已恢复为 `ml-timeout-lock-panic-scale=40`。
设备每轮自动还原原厂 `vendor_boot`，现停在 recovery。下一轮按优先级：
① 在精确判据下重做 UCHE ways 的持续写入（`0x0e17`），并读
`GBIF_HALT`/`RBBM_GBIF_CNTL`/`RBBM_STATUS3` 的 GX 侧 halt 位；
② 解析 `CP_SET_DRAW_STATE`（op43=5、sds=563）间接表，拿完整
`RB_MRT0_*` 与 `GRAS/RB` 状态；③ 核对 A735 与 A740 在
`UCHE_*`/`GBIF_*` 寄存器表上的取值差异（不照抄 uke/sheng）。

## §69.109 A735 的 `UCHE_CACHE_WAYS` 值冲突：离线取证（fix144 前半）

### 69.109.1 「0 ways」不是缺配置，是**内核与用户态各写各的**

用户从一开始提醒的「A735 与 uke A732 / sheng A740 相似但不相同」，
在 `UCHE_CACHE_WAYS(0x0e17)` 上有一个此前从未对齐过的芯片级冲突：

| 来源 | 值 | 适用 |
| --- | --- | --- |
| Linux msm `a6xx_gpu.c:1285-1286` | **`0x00800000` (BIT(23))** | **所有 `adreno_is_a7xx()`**，含 A735 |
| 本板 power-up list（`MUYU PWL0`） | **`0x00800000`** | 每次 GX 上电重放，日志逐字可见 |
| mesa `freedreno_devices.py:1027` FD735 | `0x00000000` | 本板 Turnip 走的正是 `chip_id 0x43030B00 / "FD735"` |
| mesa `:923` FD740 | `0x00040004` | sheng（A740） |
| mesa `:874` a730 | `0x00840004` | uke 家族；**fix116 当年只试过这个** |

`tu6_init_static_regs()` 把 `raw_magic_regs` 逐条发成 type4 写，
所以客户端每个 IB 里都有 `400e1701 00000000`（§69.91.2），
把 hw_init / 上电表写好的 BIT(23) 抹成 0。这解释了
`MUYU UCHE` 每轮都是「启动早期 `ways=00800000`、之后 `00000000`」、
以及 `MUYU GX` 每次都是 `ways=00000000/00000000`、`uche=0`。

⇒ **fix116 的结论不能外推**：它试的是 a730 的
`BIT(23)|BIT(18)|BIT(2)`，且用的是 canary 时代的旧判据；
**「恢复本板内核期望的 BIT(23)」从未在精确面板判据下测过**。

### 69.109.2 GBIF halt 与 UCHE config：本轮离线已排除

翻历史日志发现这两个数其实一直都在读，不需要新探针：

```
MUYU BUS   halt=0 ack=3 gxhalt=0
MUYU UCHE  mode=00600000 gx=010240e0 cmdq=0006690e pf=00000081
```

- `NativeAdrenoSubmit.cpp:838-846`：`halt` = `0x3c45 GBIF_HALT`、
  `ack` = `0x3c46 GBIF_HALT_ACK`、`gxhalt` = `0x0016 RBBM_GBIF_HALT`。
  每轮每帧都是 `halt=0`，**GBIF 没有挡住 GX 事务**。
- `gx=010240e0` / `cmdq=0006690e` 与
  `UniXNU-uke/package/GpuPowerProbe.cpp:1576,1591`、
  `UniXNU-sheng/package/GpuPowerProbe.cpp:1474,1489` 的写入值
  **逐字相同** ⇒ A735 在 `UCHE_GBIF_GX_CONFIG` / `UCHE_CMDQ_CONFIG`
  上与 A740/A732 无差异，§69.108.4 的待办 ③ 结案。
- `MUYU GX` 已有 `RBBM_STATUS3(0x0213)` 与 `stall=0`。

⇒ §69.108.4 待办 ① 里的「读 GBIF halt 位」**降级为交叉验证**，
真正未测的只剩 `0x0e17` 的 A7XX 值。

### 69.109.3 fix144 的单变量设计

- 载体 `sheng-tum=swCDG`（`G` 未占用），baseline 仍是 `swCD`。
- `NativeAdrenoVm.cpp` 的 IB 改写器在 type4 写 `0x0e17` 且值为 0 时，
  改写成 **`0x00800000`**（只加 BIT(23)，不是 a730 的 `0x00840004`），
  与 fix120 的 CCU patch 同位置、同样的 `dsb sy` + 页 clean，
  日志 `MUYU UCWB mode=1` / `MUYU UCWB patched 0e17 … -> 00800000 hits=N`。
- `GpuPowerProbe.cpp` 纯只读补一行 `MUYU GBF`（两对 halt/ack +
  `UCHE_FILTER_CNTL 0x0e18` / `UCHE_MODE_CNTL 0x0e01`，这两个从未读过）。
- 判据不变且不放宽：`MUYU SCAN zero` 是否从 1275 下降、
  `FBD ch` / `PFB ch` 是否非 0、`FBC` 是否不再是 8 色各 288、
  用户肉眼是否出现桌面/图标。彩条由 `swC` 保持，一眼可对照。

### 69.109.4 `CP_SET_DRAW_STATE` 间接表的确切格式（下一轮备料）

`adreno_pm4.xml.h:1473-1515` + `tu_cmd_buffer.cc:1274-1278`：
opcode `0x43` 的 payload **每 3 个 dword 一个 state group** ——

```
dword0  COUNT(bits 0..15, 单位 dword) | DIRTY 0x10000 | DISABLE 0x20000
        | DISABLE_ALL_GROUPS 0x40000 | LOAD_IMMED 0x80000
        | BINNING 0x100000 | GMEM 0x200000 | SYSMEM 0x400000
        | GROUP_ID(bits 24..29)
dword1  state.iova 低 32 位
dword2  state.iova 高 32 位
```

`scanIbPacketsAt()` 目前只 follow `CP_INDIRECT_BUFFER(0x3f)`，
把 `0x43` 整个 payload 跳过了（§69.95 的 `hit=0` 就是这么来的）。
下一轮在此处按 3-dword 步长取 iova，再用已有的
`NativeAdrenoVm::peekWordsUncached()` 读 state object，
即可拿到 `RB_MRT0_BASE` 等完整渲染状态。

### 69.109.5 fix144 结果：BIT(23) 恢复并全程保持，store 仍不落地

镜像 `fw/vendor_boot_gpu_fix144.img`（sha `c29f16b6…`），日志
`logs/vendor_boot_gpu_fix144-logs/muyu-6745f566`（kernel 389 s、149 轮、
gap=0；414 s 自复位，只是观察窗口更长，不是修复）。载体 `swCDG`。

```
MUYU UCWB mode=1
MUYU UCWB patched 0e17 00000000 -> 00800000 hits=1 … 192   ← 改写持续生效
MUYU GX   ways=00800000/00800000                            ← 第一次全程不再被抹成 0
MUYU UCHE ways=00800000 pf=00000081 gx=010240e0 cmdq=0006690e
MUYU GBF  gh=00000000/00000003 rh=00000000/00000000
          gx=010240e0 cmdq=0006690e filt=00000080 mode=00600000 pf=00000081
MUYU BUSY or=00ef8df7 rb=1 ccu=1 uche=1 sp=1 …              ← 引擎确实在跑
MUYU SCAN n=2..31 zero=1275 canary=4 other=1 distinct=1 fail=0
                                                              ← 与 fix130/140 逐字相同
```

**判据（不放宽）：ROP 一个字节都没落 34 MB 表面。**

`FBC/FBD/PFB` 在 142.775 s 后的变化**不是 GPU**：

| 时间 | present 状态 | 面板 |
| --- | --- | --- |
| 137.9–140.8 s | `SWAPD done n=1..8 status=00000000 map=1` | `FBD ch=0/2304`、`PFB ch=0/768`、8 色各 288/96 **完全静止** |
| 142.775 s 起 | `SWAPD` 行消失（GPU 侧停了） | `PFB distinct=9`、`FBC 000=567→879`、`FBD ch=69..149/2304` 逐帧滚动 |
| 158.1 s | — | `PFB distinct=2 000=764 fff=4` 黑底白字 |

与 present 完成**反相关**，起点 142.775 s 与基线轮的 141.67/142.99 s 同源，
形状 = bring-up 的 5 s dump 控制台 ticker（§69.103.3，用户已肉眼确认
「左半屏文字/黑块、右半屏彩条、从无桌面图标」）。

⇒ **`UCHE_CACHE_WAYS` 整族结案**（fix108→113→116→144）：
FD735 的 0、a730 的 `0x00840004`、**A7XX 内核/本板上电表的
`0x00800000`** 三种值都试过，store 一律不落地。``0x0e17`` 不再是变量。

### 69.109.6 顺带把 GBIF 与「未匹配 stream」也钉死

- `MUYU GBF` 两对 halt/ack 全程 `0`（`gh=0/3`、`rh=0/0`），
  `UCHE_FILTER_CNTL=0x80`、`UCHE_MODE_CNTL=0x00600000` 首次读到；
  `gx/cmdq/pf` 与 uke/sheng 的写入值逐字相同。**GBIF 总闸整层排除**。
- `MUYU S2CR scr0=2a0406` ⇒ `(scr0&0x407)=0x406`，bit10 `USFCFG=1`
  （Linux `arm-smmu.c:1699`：未匹配 stream **报故障**，不是静默直通），
  而 `MUYU CBF gfsr=0`。§69.79.1 已据此排除「GX SID 未匹配被 bypass」；
  **fix141 的动机（`USMFEN=0`）本身就是错的**，它的硬挂死更不能当作
  「补路由可行」的证据。SMMU 路由层至此再无空间。

## §69.110 fix145：解析 `CP_SET_DRAW_STATE` 间接表（唯一未读的上游状态）

- 载体回到 **`sheng-tum=swCD`**（`G` 保留在源码里可回退，但已证伪不再启用）。
- `NativeAdrenoVm::scanIbPacketsAt()` 遇到 opcode `0x43` 时按 3-dword 步长
  取出每个 state group 的 `COUNT / GROUP_ID / IOVA`，用已有的
  `peekWordsUncached()` 最多读 256 个对象的前 32 dword，普查其中的
  type4 头，特别是 `RB_MRT0_BASE/BASE_HI (0x8825/0x8826)`；
  新增 `MUYU SDS grp=… live=… fail=… t4=… mrt=… dw=… sys=…`
  与前 8 个对象的首 4 dword 原文。**纯只读，不改任何 GPU 状态。**
- 这一轮只回答两个问题：间接表里的 IOVA 是否落在已映射的客户端 BO、
  以及 MRT 状态究竟写在哪。fix132 的 `hit=0` 有望第一次得到解释。

### 69.110.1 fix145 结果：间接表全部可读，MRT 仍不在前 32 dword

镜像 `fw/vendor_boot_gpu_fix145.img`（sha `1016bb69…`），日志
`logs/vendor_boot_gpu_fix145-logs/muyu-674fd10b`（174 s、42 轮、gap=0），
载体 `swCD`（回到 baseline，`MUYU UCWB mode=0`）。

```
MUYU SDS g=15 cnt=9  iova=0000000110bce100 d0=0f200009 w=70348003 01124000 10bce000 00000001
MUYU SDS g=0  cnt=25 iova=0000000101923d00 d0=00700019 w=70dc8018 0000ab00 00000005 0000ab1f
MUYU SDS g=3  cnt=2  iova=0000000100006000 d0=03710002 w=40a00201 0000fcfc 00000000 00000000
…
MUYU SDS grp=3476 live=2687 fail=0 t4=633 mrt=0 dw=3169 sys=2524
```

- **`fail=0`**：3476 个 state group 里 2687 个带有效 `(iova,count)`，
  IOVA **全部落在已映射的客户端 BO**，一个都没读到空。
  ⇒ 命令流地址/页表侧再无嫌疑，SDS 包格式解析也与 mesa 定义吻合
  （`d0` 的 `COUNT/GROUP_ID` 与 `w[0]` 的 type7 头自洽）。
- **`sys=2524`**：绝大多数 group 带 `CP_SET_DRAW_STATE__0_SYSMEM`，
  与 §69.108「Turnip 一直跑 sysmem」互相印证。
- **`mrt=0`**：所读的前 32 dword 内**没有** `RB_MRT0_BASE/BASE_HI
  (0x8825/0x8826)` 的 type4 写。但 fix145 只读了每个对象前 32 dword、
  且只 follow 了 256 个对象（`dw=3169` 第 3 次采样后就不再增长），
  所以这**还不足以**判定「MRT 不在 SDS 里」——fix146 读全。
- 判据与基线逐字相同：`SCAN zero=1275 canary=4 other=1`、
  `SWAPD done n=1..8 status=00000000 map=1`、`FBD ch=0`，
  142 s 后的变化仍是 dump 控制台 ticker。纯只读解析无副作用。

### 69.110.2 顺带发现一个真实的采样盲区（但结论不变）

`MUYU TGT` 末行显示 GPU 在**两个 34 MiB render target 间交替**：

```
| 0000000101d36000 p=200 f=4b x130 | 0000000110cd4000 p=200 f=4b x119 |
```

而 `frameScan()` 的调用点是
`if(s[i].bytes==0x2098000) {frameScan(...); break;}` —— **永远只扫第一个**。
fix144 全扫 `0x101d36000`、fix145 全扫 `0x110cd4000`，两轮各看一半。
`SWAPD wait` 又用了另外两个 34 MiB 地址（`0x120213000` / `0x111000000`）。

但 `MUYU CAN arm` 明确给两个 render target **都种了 canary**：

```
MUYU CAN arm i0 spots=4/4 at=0000000101d36000+826000 b=2098000
MUYU CAN arm i0 spots=4/4 at=0000000110cd4000+826000 b=2098000
MUYU CAN t=273 n=3 | i0 ok=4/4 v0=a5a5a5a5 | i1 ok=4/4 … | i2 ok=1/4 …
```

末段仍是 `4/4 | 4/4` 全存活 ⇒ **两个 target 都没被写过**，
§69.97 的「ROP 一个字节不写」结论**不受盲区影响**。

真正的隐患在更上游：`sampleImports(s,4,&n)` 只给 4 个槽，
实现内部还有 `pick[8] / if(max>8)max=8` 的硬上限，而 `MUYU IMPM`
明明列出 18 个 import。任何排在前 4/前 8 之外的 surface 从未被
canary 或 SCAN 看到 —— 这个洞必须在下一轮堵上。

## §69.112 fix146：堵死采样盲区 + 读全 state object（纯只读）

- 载体仍 `sheng-tum=swCD`，不改任何 GPU 状态。
- `NativeAdrenoVm::sampleImports()`：`pick[8] → pick[24]`、cap `8 → 24`；
  两个调用点 `ImportSample s[4] → s[24]`、`sampleImports(...,24,&n)`。
- IMW tick 的 target 扫描去掉 `break`，改为最多扫 4 个
  `bytes==0x2098000` 的 import；canary 槽上限 `4 → 8`。
- SDS follow 上限 `256 → 1024` 个对象、每对象 `32 → 256` dword，
  并新增 `native_adreno_sds[7]` 统计落在 `0x8800..0x88ff`（RB/MRT 块）
  的 type4 头，打印前 8 条 `MUYU SDSR g=… reg=… hdr=… val=…`。
- 判据：`mrt/rb` 是否终于非 0、多个 `MUYU SCAN n=… iova=…` 行是否
  出现不同的 34 MiB 目标、`MUYU CAN` 是否出现 `ok<4/4`。

### 69.112.1 fix146 结果：盲区堵死，结论不变；读到 417 个 RB 寄存器写

镜像 `fw/vendor_boot_gpu_fix146.img`（sha `b74b229c…`），日志
`logs/vendor_boot_gpu_fix146-logs/muyu-6741ba56`（174 s、42 轮、gap=0），
载体 `swCD`。

```
MUYU SDS grp=1469 live=1131 fail=0 t4=3445 mrt=0 dw=17523 sys=1062 rb=417
MUYU SDSR g=8  off=46 reg=08810 hdr=40881001 val=00000000
MUYU SDSR g=28 off=4  reg=08820 hdr=40882002 val=00000180
MUYU SDSR g=24 off=0  reg=08880 hdr=40888001 val=00700700
…
MUYU SCAN iova=0000000101d36000 x16 / 0000000110cd4000 x15   ← 两个 target 都扫了
MUYU CAN arm …101d36000 … / …110cd4000 … / …123106000 …
MUYU CAN t=129 n=3 | i0 ok=4/4 | i1 ok=4/4 | i2 ok=4/4
FBD ch=0/2304（全程）  SWAPD done n=1..32（末次 e00002bc）
```

- **采样盲区正式关闭**：两个 34 MiB render target 现在都被 SCAN 覆盖，
  且 canary 对两者都是 `4/4` 全程存活；`zero=1275 canary=4 other=1`
  对两个表面逐字相同。⇒ **「ROP 一个字节不写」不再有任何采样借口**。
- SDS 读全后 `rb=417`：state object 里确实有成百的 `0x88xx` RB/MRT
  寄存器写，`0x8820 (RB_MRT0_CONTROL)` 也找到了（`val=0x180`）。
  但 **`mrt` 仍为 0** —— 没有任何独立的 `0x8825/0x8826` 头。

### 69.112.2 这解释了 fix131/132 为什么 `hit=0`

`a6xx.xml:2390`：

```xml
<array offset="0x8820" name="RB_MRT" stride="8" length="8" usage="draw">
   +0 CONTROL   +1 BLEND_CONTROL  +2 BUF_INFO  +3 PITCH
   +4 ARRAY_PITCH  +5 BASE (reg64)  +7 BASE_GMEM
</array>
```

`RB_MRT0.BASE` 是 **`reg+5`/`reg+6` = 0x8825/0x8826**，但它是
`0x8820` 起始的**一个 8-dword 块写**里的 payload，寄存器号并不单独出现
在包头。fix131/132 在整条 IB 里线性找 `0x8825/0x8826` 头，当然
`hit=0`；fix128 从硬件寄存器读到的 base 是对的，二者并不矛盾。

⇒ 重定向 MRT 到面板的实验（fix131 的原始意图）**当年根本没打到目标**，
这条线值得在正确位置重做。

## §69.114 fix147：读出块写里的 `RB_MRT0.BASE`（纯只读）

- 载体仍 `swCD`。在 fix146 的 SDS object 遍历里，遇到
  `reg ∈ {0x8820,0x8828,…,0x8858}`（8 对齐）且 `count≥7` 的 type4 头，
  取 payload dword 6/7 为 BASE lo/hi，打印前 12 条
  `MUYU MRTB g=… cnt=… info=… pitch=… base=… gmem=…`，并计入 `mrt`。
- 目的：第一次拿到客户端**实际写给 ROP 的目标地址**，与 `MUYU TGT`
  的硬件读数、import 表里的 34 MiB surface / panel IOVA 三方对照。
  若一致 ⇒ 渲染状态完全正确，病根 100% 在 store 通路本身；
  若不一致 ⇒ 找到了可直接 patch 的错误地址。

### 69.114.1 fix147 结果：`MRTB` 零输出，但这是**解析盲区**不是「MRT 没发」

镜像 `fw/vendor_boot_gpu_fix147.img`（sha `ca75dff8…`），日志
`logs/vendor_boot_gpu_fix147-logs/muyu-6745b132`（141 s、26 轮、gap=0；
167 s 自复位、还原原厂、回 recovery）。

```
MUYU SDS grp=1341 live=1037 fail=0 t4=3441 mrt=0 dw=17629 sys=974 rb=416
```

`MUYU MRTB` **一条都没有**；`MUYU SCAN` / `FBD` / `SWAPD` 与基线逐字相同，
末段仍 `HC hang` / `GFXFAIL`。SDS object 里能找到 `40882002`
（只写 CONTROL/BLEND_CONTROL 两个 dword 的短 type4 包），但找不到任何
`0x8820` 起始、count≥7 的 8-dword 块写。⇒ §69.112.2 的「Turnip 用
**一个** `40882008` 块写发完整个 RB_MRT」推断对本板客户端**不成立**。

## §69.115 fix148：A7XX 的 MRT 走 `CP_CONTEXT_REG_BUNCH`（type7 0x5c），三方对照全一致

### 69.115.1 离线取证：MRT 不是 type4 块写，是 (regid,value) 对

`mesa-work` 里查到确切发射路径：

- `tu_cmd_buffer.cc:850 tu6_emit_mrt()` → `tu_crb crb = cs->crb(10*MAX_RTS+6)`，
  对 0 号 attachment 依次 `crb.add()`：
  `RB_MRT_BUF_INFO(0x8822)`、`PITCH(0x8823)`、`ARRAY_PITCH(0x8824)`、
  `BASE(0x8825/0x8826, reg64)`、`BASE_GMEM(0x8827)`、
  `SP_PS_MRT_REG`、`RB_COLOR_FLAG_BUFFER_ADDR/PITCH`。
- `tu_cs.h:760 struct tu_crb : tu_pkt` 用 **`CP_CONTEXT_REG_BUNCH`（opcode 0x5c）**：
  `add(reg32)` → `append(reg.reg); append(reg.value);`（2 dword），
  `add(reg64)` → 4 dword `(reg,lo),(reg+1,hi)`。
  payload 是成对的 **(regid, value)**，regid 就是 `0x8825` 这种绝对寄存器号。
- `decode/cffdec.c:3188 cp_context_reg_bunch()` 佐证：
  `for (i=0;i<sizedwords;i+=2) reg_set(dwords[i],dwords[i+1])`。
  `0x5d CP_NON_CONTEXT_REG_BUNCH`（A7XX 新增）前面多 2 dword 前缀。
- `tu6_emit_mrt()` 的两个调用点（`tu_cmd_buffer.cc:7169/7537`）都发到
  **`cmd->draw_cs`**，即由 `CP_INDIRECT_BUFFER(0x3f)` 跟随的次级 IB。
  `scanIbPacketsAt()` 会 follow 到 depth=1，但它的 SDS object 遍历只认
  `((obj[w]>>28)&0xf)==4`，type7 包只被计数、payload 从不解析
  ⇒ **fix131/132/147 的 `hit=0` / `mrt=0` 全是盲区**。
- `stripThreadSync()` 只改写 opcode 0x17/0x1b，**没有误伤 0x5c**。

### 69.115.2 fix148 设计（纯只读，载体仍 `swCD`）

在 `NativeAdrenoVm::scanIbPacketsAt()` 的 type7 分支里，对 `code==0x5c/0x5d`
按 2 dword 步进解析 (regid,value) 对，打印前 24 条 `MUYU CRB` 明细，
并把 `0x8822..0x8827` 汇总成 `MUYU CRBB`（含拼出的 64-bit BASE）。
`MUYU PKTS` 加 `op5c/op5d`，新增 `MUYU CRBS` 计数行。不改任何 GPU 状态。

镜像 `fw/vendor_boot_gpu_fix148.img`（sha `ad418f3b…`），离线校验
`sheng-tum=swCD` / `console-quiet=1` / trust+addon 均 fix107（sha 一一对应）、
无 `fix99`/`swCDE`/`swCDF`/`swCDG`/`sheng-tu-debug` 残留，
`kernel_args` 1014 B。日志
`logs/vendor_boot_gpu_fix148-logs/muyu-67377121`（173 s、42 轮、gap=0、
203 s watchdog 正常自复位、errors=1），设备自动还原原厂并回 recovery。

### 69.115.3 结果：CRB 在次级 IB 里，BASE 三方逐字一致 ★

```
MUYU PKTS ... op5c=160 op5d=0
MUYU CRBS c5c=160 c5d=0 pairs=3000 rb=1800 lo=30 hi=30 base=30 bad=0
MUYU CRB d=1 op=5c off=6 reg=08825 val=03e0a000
MUYU CRBB d=1 base=0000000101d36000 info=0000004b pitch=000000c8 arr=000684c0 gmem=0
MUYU CRBB d=1 base=0000000110cd4000 info=0000004b pitch=000000c8 arr=000684c0 gmem=0
MUYU CRBB d=1 base=0000000103e0a000 info=00000b68 pitch=00000190 arr=000d1600 gmem=0
MUYU CRBB d=1 base=0000000112da8000 info=00000b68 pitch=00000190 arr=000d1600 gmem=0
MUYU TGT | ... 0000000103e0a000 p=400 f=b68 | 0000000112da8000 p=400 f=b68
        | 0000000101d36000 p=200 f=4b x114 | 0000000110cd4000 p=200 f=4b x71
MUYU IMPM h=3d iova=0000000101d36000 bytes=2098000 phys=091ba4d000
MUYU IMPM h=4f iova=0000000110cd4000 bytes=2098000 phys=093d673000
```

- **`op5c=160`、`d=1`**：160 个 `CP_CONTEXT_REG_BUNCH` 包全在次级 IB
  (`draw_cs`)，`0x5d` 从未出现；30 组 `0x8825/0x8826` 都成对、`bad=0`。
- **三方一致**：客户端 CRB 发出的 `RB_MRT0.BASE` = `MUYU TGT` 的
  `regRead(0x8825/0x8826)` **硬件 MMIO 读数**（`NativeAdrenoSubmit.cpp:706`）
  = `MUYU IMPM` 的 import IOVA。四块目标（两块 34 MiB `f=4b p=200`、
  两块 `f=b68 p=400`）全部逐一对应。
- `BUF_INFO=0x4b`（FMT6_32_UINT / linear / `LOSSLESSCOMPEN=0`）、
  `PITCH=0xc8`（shr6 ⇒ 12800 B = 3200×4）、
  `ARRAY_PITCH=0x684c0`（shr6 ⇒ 27,340,800 B = 3200×2136×4
  = 面板缓冲大小）与 §69.95 的几何结论完全一致。
- **因为 TGT 是硬件寄存器读数**，三方一致还额外证明：CP 确实把
  A7XX 的 context-register bunch **应用进了 RB 硬件**，
  不是「写进 context bank 但从未加载」。

### 69.115.4 判据照旧：纯只读，store 仍然零字节

`SWAPD done n=1..8 status=00000000 map=1`（真 kOk）、
`SCAN` 51 次 `zero=1275 canary=4 other=1`、
`FBD ch=0` 静止期后 142 s 起 dump 控制台 ticker 滚动、
`PFBC` 8 色各 96、`FBC` 8 色各 288、`HC hang`×1、`GFXFAIL`×16
—— 与 fix138–147 基线逐字一致。**彩条/桌面判据没有任何放宽**。

### 69.115.5 整族证伪与病根收窄

- 证伪：「CP 没发 MRT」「MRT 地址错/指向别处」「A7XX `MUTABLEEN`
  / descriptor / context register bank 未生效」「`RB_MRT` 需要 type4 块写」
  「fix131/132 的重定向失败是目标选错」——**当年根本没打到正确位置**。
- 顺带纠正注释：`NativeAdrenoProfile.cpp` 里 `0x43030b00` 的 profile 名是
  `Adreno732`、SQE 名 `gen71100_sqe.fw`；`GpuPowerProbe.cpp:159` 注释写的
  「board's real a740 SQE microcode」是过时描述。
  `gen-muyu-gmu-source.py --sqe-firmware` 默认指向
  `fw/gpu-firmware/gen71100_sqe.fw`（66036 B，gen7_11_0 同代），
  **不是** A740 微码。SQE/芯片代际不是病根方向，别再投入。
- **病根现在只剩一句**：CP 已把完全正确的 MRT 目标/几何/格式写进 RB
  硬件，Turnip 编码带真实渲染面积的 sysmem pass，fence 完成、
  compute copy 报 kOk、CP 的 CX 侧写能落地；但 **GX 侧
  (RB/ROP/UCHE/compute) 的 store 一个字节都不到 DRAM，且无 fault/
  无 stall/无 GX halt**。
- **下一步（fix149）唯一有判别力的实验**：第一次把 MRT 重定向
  **打在正确位置** —— 在 `CP_CONTEXT_REG_BUNCH` 的 `(0x8825,0x8826)`
  对里，把 BASE 改成面板 import 的 GPU IOVA（运行时用
  `sampleImports()` 找 `bytes==0x1a13000 && phys==0xe3940000`，
  本轮是 `0x11fc72000`，每轮会变，不能硬编码）。几何完全一致
  (`ARRAY_PITCH` shr6 = 面板字节数)，`LOSSLESSCOMPEN=0` 无需 flag buffer。
  若面板出现 GPU 写入 ⇒ 目标属性/路由问题；若仍零 ⇒ GX store 通路本身
  是死的，与渲染状态彻底无关。  `scanIbPackets()` 与 `stripThreadSync()`
  都在提交前（`NativeAdrenoSubmit.cpp:682/684`），patch 来得及生效。
  仍不补 SMMU 路由、不换表、不放宽判据。

## §69.116 fix149：**ROP 明确指向面板后，仍一个字节不写** ★★

### 69.116.1 设计（`sheng-tum=swCDH`，等长 cmdline 1015 B）

`H` 是本轮唯一的行为变量。在 `NativeAdrenoVm::scanIbPacketsAt()` 的
`CP_CONTEXT_REG_BUNCH` (regid,value) 解析循环里，当 `BUF_INFO==0x4b`
（34 MiB FMT6_32_UINT 目标）且遇到 `(0x8825, val)` / `(0x8826, val)` 时，
把 value 改成运行时 `sampleImports()` 找到的面板 import IOVA
（`bytes==0x1a13000 && physical==0xe3940000`，同尺寸兜底）。
**禁止硬编码**：该 IOVA 每轮都变（fix147 `0x1265d4000`、fix148
`0x11fc72000`、fix149 `0x116568000`）。只改 MRT0，
两块 `0xb68` 目标保持原样；改写后 `dsb sy` + `NativeAdrenoMaintainPhysicalPages`
clean，幂等。新增 `MUYU MRTP` 明细、`MUYU CRBS` 的 `mrtp`/`rt4b` 计数。
`scanIbPackets()` 在提交前（`NativeAdrenoSubmit.cpp:682`），CP 会读到改写后的值。

镜像 `fw/vendor_boot_gpu_fix149.img`（sha `307c4cfc…`），离线校验
`swCDH` / trust+addon 均 fix107 / 无残留 / `kernel_args` 1015 B。
日志 `logs/vendor_boot_gpu_fix149-logs/muyu-6740e7d1`
（**414 s 自复位、391 s 日志窗口、150 轮、gap=1 s**，
设备自动还原原厂并回 recovery；本轮比常见的 203 s 活得更久，无硬挂）。

### 69.116.2 patch 生效，且被硬件 MMIO 独立确认 ★

```
MUYU MRTP mode=1
MUYU MRTP n=1 reg=08825 01d36000 -> 16568000 panel=0000000116568000 info=0000004b
MUYU CRBS ... c5c=126 lo=22 hi=22 base=22 bad=0 mrtp=20 rt4b=20
MUYU CRBB d=1 base=0000000116568000 info=0000004b pitch=000000c8 arr=000684c0 gmem=0
MUYU IMPM h=51 iova=0000000116568000 bytes=1a13000 phys=00000000e3940000 ro=0 perm=3 coh=0
MUYU TGT | ... 0000000103e0a000 p=400 f=b68 | 0000000112da4000 p=400 f=b68
        | 0000000116568000 p=200 f=4b x147
```

- 20 个 34 MiB RT 的 CRB 包全部命中（`mrtp=20 rt4b=20`），
  `0x8826` 的高 32 位本来就相同（`0x00000001`）故无需改写。
- **`MUYU TGT` 是 `regRead(0x8825/0x8826)` 的硬件读数**：它从
  `0x101d36000`/`0x110cd4000` 变成了 **`0x116568000`**，即面板 import IOVA。
  ⇒ 这是本项目第一次 **ROP 被硬件确认指向 DPU 正在扫描的那块连续 DRAM**。
- 几何完全一致：`PITCH=0xc8` shr6 = 12800 B = 3200×4，
  `ARRAY_PITCH=0x684c0` shr6 = 27,340,800 B = 面板字节数；
  `BUF_INFO=0x4b` 的 `LOSSLESSCOMPEN=0`、`TILE_MODE=0` ⇒ 无需 UBWC
  flag buffer，`BASE_GMEM=0` ⇒ sysmem 直写。

### 69.116.3 判据：面板零变化，store 依旧是零字节

`SWAPD done n=1..8 status=00000000 map=1`（138.70–144.11 s，真 kOk）期间：

```
MUYU FBD ch=0/2304（139.01→143.48 s 连续 9 次）
MUYU FBC  fff=288 00f=288 0f0=288 f00=288 0ff=288 f0f=288 ff0=288 222=288（恒定）
MUYU PFB  n=1 nz=768/768 distinct=8 ch=0/768（显示驱动独立读取器）
MUYU SCAN 43 次 zero=1275/1276 canary=4 other=0/1 fail=0
MUYU CAN  i0 ok=4/4 | i1 ok=4/4（哨兵全存活）
```

144.22 s 起 `FBD ch=476…95`、`FBC` 出现 `000=386…696`、`PFB distinct=9
ch=276` —— 与最后一次 `SWAPD done`(144.11 s) **反相关**，是 bring-up
dump 控制台 ticker（用户已肉眼确认「左半屏文字/黑块、右半屏仍有彩条，
没有出现过桌面或图标」）。`HC hang` 在 160.69 s，`GFXFAIL` 16 次。
**没有放宽任何判据**。

### 69.116.4 整族彻底证伪、病根定点

- 现在连「ROP 写到了别的地址 / 目标属性不对 / 页表指向别处 /
  UBWC 元数据缺失 / 渲染状态没进硬件 / 几何不匹配」都不成立了：
  目标已经换成**唯一能被 DPU 直接看见的连续 DRAM**，且硬件寄存器
  独立确认生效，面板依然一字节未动。
- **病根定点（最终形态）**：Turnip 编码带真实渲染面积的 sysmem pass，
  CP 把完全正确的 MRT 目标/几何/格式写进 RB 硬件，fence 完成、
  compute copy 报 kOk、CP 的 CX 侧写（`5a5a5a5a`）能落地，
  SMMU 无 fault/stall、GBIF 无 halt、GX 电源/时钟开、
  `RBBM_STATUS` 引擎位全忙；但 **GX 侧的 store 事务
  (RB/ROP/CCU/UCHE/compute) 根本没有抵达 DRAM**。
- **方向纠正（重要）**：继续在客户端/Turnip/渲染状态/KMD 提交层
  打补丁已无收益 —— 这些层现在全部有直接证据是「对的」。
  下一阶段必须转向 **GX 侧 store 通路本身的硬件使能与门控**：
  RB/RBBM 的 GX 时钟与 power domain、`RB_RENDER_CNTL`/`RB_CNTL`
  的 render mode 与 binning 使能、A7XX 新增的
  `CP_RESET_CONTEXT_STATE(0x1f)` / `CP_SCOPE_CNTL(0x6c)` /
  CCHE invalidate 对 context state 的影响，以及
  `stripThreadSync()` 自 fix61 起一直 strip=1 的 BV/BR 协议
  （fix115/117 恢复原协议会挂死，需在**不挂死**的前提下重新设计）。
  注意 A735 与 A732/A740 的取值差异，不要照抄。
- 保留 `swCD` 为基线；`H` 只用于本对照实验，不进默认载体。
  仍不补 SMMU 路由、不换表、不放宽判据。

## §69.117 fix150：**GPU 从未退出 secure mode** ★★★ 上游病根定位

### 69.117.1 离线取证：Linux msm 的 secure-mode exit 是两条互斥路径

`fw/android-harvest/msm-drv/a6xx_gpu.c:1492-1520`（本地 harvest 的
上游源码，不是推测）：

```
/*
 * Try to load a zap shader into the secure world. If successful
 * we can use the CP to switch out of secure mode. If not then we
 * have no resource but to try to switch ourselves out manually. If we
 * guessed wrong then access to the RBBM_SECVID_TRUST_CNTL register will
 * be blocked and a permissions violation will soon follow.
 */
ret = a6xx_zap_shader_init(gpu);
if (!ret) {
        OUT_PKT7(gpu->rb[0], CP_SET_SECURE_MODE, 1);
        OUT_RING(gpu->rb[0], 0x00000000);
        a6xx_flush(gpu, gpu->rb[0]);
        if (!a6xx_idle(gpu, gpu->rb[0])) return -EINVAL;
} else if (ret == -ENODEV) {
        dev_warn_once(..., "Zap shader not enabled - using SECVID_TRUST_CNTL instead\n");
        gpu_write(gpu, REG_A6XX_RBBM_SECVID_TRUST_CNTL, 0x0);
        ret = 0;
} else return ret;
```

即：**有 zap shader → CP 发 `CP_SET_SECURE_MODE(0x66) 0`；
没有 zap shader（-ENODEV）→ CPU 直接写 `RBBM_SECVID_TRUST_CNTL(0xf400)=0`**。
`CP_SET_SECURE_MODE` 的定义在 `adreno_pm4.xml:404`（0x66，
"Used to switch GPU between secure and non-secure modes"）。

### 69.117.2 17 线恰好两条路都没走（A735 ≠ A732/A740 的实质落点）

1. `GpuPowerProbe.cpp:149-150` 的 `native_adreno_zap_image` /
   `_size` 是 **weak 空默认 `{0}` / `0`**。`build-native-adreno-kits.py`
   属于 sheng/uke 线、不在本内核链接里，且它要嵌的是
   `qcom/sm8550/sheng/a740_zap.mbn`（**A740 的 zap，本机根本不存在**）。
   `audit-gpu-restart-offline.py:77` 原文：
   「the only two 0x66 senders are zapAndSecure() (skipped) and
   gpuRestart() (gated off): **the 17 line never emitted this packet**」。
2. `GpuPowerProbe.cpp:40` 的注释照抄 sheng 的假设：
   「sheng uses a zap shader, so **RBBM_SECVID_TRUST_CNTL is never written**」。
   msm `hw_init` 的 SECVID 三条（TSB_CNTL/BASE/SIZE=0）在
   `:1872-1874` 确实照抄了，唯独漏了 `-ENODEV` 分支的 `TRUST_CNTL=0`。
3. 于是本板既无 zap 认证、又不发 0x66、又不写 TRUST_CNTL ——
   **GPU 一直停在 secure mode**。这与「SMMU 无 fault / GBIF 无 halt /
   fence 完成 / CP 的 CX 侧写能落地 / GX 侧 store 一个字节不到 DRAM」
   的形状完全吻合：secure 侧的写被 SECVID 静默丢弃。
4. **本板其实有正确的 zap 镜像**：`fw/gpu-firmware/gen71100_zap.mbn`
   （12088 B，gen7_11_0 同代；`derive-muyu-gpu-inputs.py:265` 明确列为
   板级 firmware，`gen71100_zap.mbn` 的注释是「the ZAP image differs
   per device」）。按 ELF32/MDT 校验全部通过：`phoff=0x34`、
   `phent=32`、`phnum=3`、ph0 非 PT_LOAD、hash 段在 ph2
   （flags `0x2000000`）、可重定位段 `lo=0x1000..hi=0x2000`
   跨度 `0x1000 ≤ kZapBytes`、`ehdr=0x94`、`hashOff+hashBytes ≤ size`、
   `ehdr+hashBytes ≤ 0x4000`。⇒ 不能照抄 A740 zap，要用这块板的。
5. **历史降级理由失效**：§3333-3335 曾以「zap=0 也能进桌面、
   int-019 更好」把 secure mode 降级，但那个「进桌面」后来已证实是
   `offline-legacy-*` CPU 拷贝合成的假成功（同 §69.76/fix101 的
   mode0）。这条线索**从来没有被硬判据检验过**。

### 69.117.3 设计（载体 `swCDK`，单变量，纯 CPU 寄存器写）

- `zapAndSecure()` 的 `size<52` 分支（原本 `Z-1` 直接 return）里加
  载体字母：`K` = msm 的 `-ENODEV` 路径，只做一次
  `gpuWr(0xf400, 0)` + `dsb sy` + 读回；`J` = msm 的 zap 成功路径，
  只发一次 `CP_SET_SECURE_MODE(0)` + fence（**不加载 zap、不碰 TZ/SMC**）。
  两者互斥，都是 bring-up 阶段 `c[0]==10`（scratch 写+fence 成功）之后、
  客户端首个提交之前，与 msm 顺序一致。
- `gSecK` 让 `gpuRestart()` 在 hang recovery 时**重放同一条写**而不是
  0x66，避免第二个载体混进同一轮。
- `NativeAdrenoSubmit.cpp` 加 `MUYU SEC`（每 8 次提交只读一次
  `0xf400`/`0xfc00/1`/`0xf803`/`0xf800/1`/`0xf802`/`0x0824`），
  把 secure 状态从 bring-up 一直盯到客户端阶段。所有偏移都在
  `0x40000` GPU 窗口内（`0xf400*4=0x3d000`、`0xfc00*4=0x3f000`）。
- 镜像 `fw/vendor_boot_gpu_fix150.img` sha `2f9d9555…`，kernel
  `619c0f0b…`；日志 `logs/vendor_boot_gpu_fix150-logs/muyu-674d8bdf`
  （202 s 自复位、175 s 窗口、43 轮、gap=0、无硬挂）。

### 69.117.4 结果：`trust=1` 且 CPU 写 0 被硬件拒绝 ★

```
[ 51.594595] MUYU SECZ size=0 J=0 K=1 trust=00000001 tsb=0000000000000000 cntl=00000000 base=0000000000000000 sz=00000000
[ 51.596006] MUYU SECK wr=0 readback=00000001 tsb=0000000000000000 stage=9
[136.420940] MUYU SEC trust=00000001 ... prt=00000000
[137.147035] MUYU SEC trust=00000001 tsb=020000010011d740 ...
```

- `RBBM_SECVID_TRUST_CNTL` **可读且为 1**（不是 `deadbeef`，访问没被挡死），
  说明 GPU 确实处于 trusted/secure 状态；**CPU 写 0 后读回仍是 1**，
  即这个位在 secure world 之外清不掉 —— 正是 msm 注释里
  「If we guessed wrong then access … will be blocked」的情形。
- 客户端阶段 29 次采样 `trust` 恒为 1；`RBBM_SECVID_TSB_STATUS`
  （A7XX 新增 0xfc00）从 137 s 起为 `020000010011d740`，说明 TSB
  有活动，但 `TSB_CNTL/BASE/SIZE` 全 0、`CP_PROTECT_STATUS=0`。
- **像素判据照旧全零（未放宽）**：`SWAPD done n=1..8 status=0`
  （138.038–141.545 s）期间 `FBD ch=0` 连续、`FBC` 8 色各 288 恒定、
  `SCAN` 全零、canary `4/4` 全存活；`HC hang`×1、`GFXFAIL`×16。
  本轮 `op5c=168`、`draw=95` 与基线一致。

### 69.117.5 结论与下一步

- **K 路径（CPU 直接写 TRUST_CNTL）在本板证伪**，而且证伪方式本身
  给出了机制证据：secure 位只能由 secure world 清除，CPU 写无效。
- 病根方向从「GX store 通路的某个门控」进一步上移到
  **secure-mode exit 从未发生**。这是本项目第一次找到「有一个明确的、
  与所有现象自洽的、且从未被执行过的上游初始化步骤」。
- fix151 用**同一个 kernel**（sha `619c0f0b…` 完全相同）、只把 cmdline
  从 `swCDK` 换成 `swCDJ`，验证单独发 `CP_SET_SECURE_MODE(0)` 是否足够。
  若不够 ⇒ 必须加载本板 `gen71100_zap.mbn` 走完整 TZ 认证
  （`zapLimit()` 默认已是 4 = 全流程），这才是 msm 在「有 zap」平台上
  唯一的官方路径；嵌入方式复用 `MuyuGmuFirmware.c` 的
  `const unsigned char[]` 生成模式，给 `native_adreno_zap_image` /
  `_size` 提供强定义覆盖 weak 空默认。
- 仍不补 SMMU 路由、不换表、不放宽判据；A735 不照抄 A732/A740 取值。

## §69.118 fix151：单独发 `CP_SET_SECURE_MODE(0)` 也不够 ★

- **理想的单变量对照**：`fw/vendor_boot_gpu_fix151.img` 与 fix150 的
  **kernel 完全相同**（sha `619c0f0b…`），只把 cmdline 从 `swCDK` 换成
  `swCDJ`。日志 `logs/vendor_boot_gpu_fix151-logs/muyu-67212e4b`
  （202 s 自复位、174 s 窗口、42 轮、gap=0、无硬挂）。
- 结果：
  ```
  [ 51.477667] MUYU SECZ size=0 J=1 K=0 trust=00000001 ...
  [ 51.682190] MUYU SECJ wait=1 seen=2 stage=8 trust=00000001 tsb=0000000000000000
  [135.286839] MUYU SEC trust=00000001 ...
  ```
  **`gRing.wait(fence)` 返回 1（超时）**，`seen=2`，`trust` 仍是 1；
  客户端阶段 27 次采样恒为 1。⇒ 未经 TZ 认证 zap shader 时，
  **CP 无权执行 `CP_SET_SECURE_MODE`**，这条包被挂住而不是被忽略。
- 像素判据照旧全零（未放宽）：`SWAPD done n=1..8 status=0`
  （137.04–140.01 s）期间 `FBD ch=0`、`FBC` 8 色恒定、`SCAN`
  `zero=1275 canary=4`、`HC hang`×2 / `GFXFAIL`×16。
- **J 与 K 一起把 msm 那段注释验证完**：
  「Try to load a zap shader into the secure world. **If successful we can
  use the CP to switch out of secure mode**」——
  CPU 直接写 TRUST_CNTL 被硬件拒绝（K），CP 自己切也需要先有 zap
  认证（J）。剩下唯一未试过的官方路径就是 **加载本板 zap shader**。
- fix152 因此把 `fw/gpu-firmware/gen71100_zap.mbn`（12088 B，本板
  gen7_11_0，`derive-muyu-gpu-inputs.py:265` 指定）生成
  `package/MuyuZapFirmware.inc`，以**强定义**覆盖
  `GpuPowerProbe.cpp:149-150` 的 weak 空默认；`zapLimit()` 默认 4
  = 完整 `init_image → mem_setup → 段拷贝 → auth_and_reset → 0x66`。
  `zapEnd()` 新增 `MUYU ZAPEND` 打印每个退出点的 stage/SMC 结果/
  `trust`，成功路径另打 `MUYU ZAPOK`。载体 `L` 作为不改构建的
  关 zap 回退开关，本轮不带。载体 `swCD`（基线）。
- 本板 DT `fw/stock/dtb_3.dtb` 里确实有 `zap-shader` 节点，设备串
  `qcom,adreno-gpu-gen7-11-0`，进一步印证「这块板需要 zap」。

## §69.119 fix152：**加载本板 zap shader 后 GPU 开始写像素** ★★★★ 病根确认并修复

### 69.119.1 实现

- `fw/gpu-firmware/gen71100_zap.mbn`（12088 B，本板 gen7_11_0，
  sha256 `463ba9b7d2e3f03a`）→ 生成
  `UniXNU-muyu-17/package/MuyuZapFirmware.inc`（62946 B，16 字节/行）。
- `GpuPowerProbe.cpp:149-150` 的 weak 空默认改成**强定义**：
  ```cpp
  extern "C" const uint8_t native_adreno_zap_image[] = {
  #include "MuyuZapFirmware.inc"
  };
  extern "C" const uint64_t native_adreno_zap_image_size = sizeof(native_adreno_zap_image);
  ```
  构建树里 `out/muyu/tree/kits/muyu` 是指向 `package/` 的符号链接，
  所以 `#include` 直接可见。`zapAndSecure()` 里原来的 weak-symbol 探测
  `&size ? image : nullptr` 因地址恒真触发
  `-Werror,-Wpointer-bool-conversion`，改成直接用本地强定义。
- `zapLimit()` 默认 4 = 完整 `init_image → mem_setup → 段拷贝 →
  auth_and_reset → CP_SET_SECURE_MODE(0)`；新增载体 `L` 作关 zap 回退。
  `zapEnd()` 新增 `MUYU ZAPEND`（每个退出点打印 stage/SMC 结果/
  `trust`/`tsb`），成功路径加 `MUYU ZAPOK`。载体 `swCD`（基线），
  `size` 成编译期常量后 `size<52` 分支被静态消除，必然走完整路径。
- 镜像 `fw/vendor_boot_gpu_fix152.img` sha `c39e0484…`，kernel
  `9f31861b…`；离线校验确认 zap blob 前 32 B 编进镜像、
  `ZAPEND`/`ZAPOK` 在内核、trust/addon 均 fix107、无残留。
  日志 `logs/vendor_boot_gpu_fix152-logs/muyu-675a3f90`
  （**203 s 正常自复位、176 s 窗口、43 轮、gap=0、无硬挂**；
  TZ SMC 没有挂死设备）。

### 69.119.2 GPU 第一次真正退出 secure mode ★

```
[ 51.656720] MUYU ZAPOK  stage=9 wait=0 seen=3 trust=00000000 tsb=0000000000000000
[ 51.658244] MUYU ZAPEND stage=9 limit=4 init=0 mem=0 auth=0 wait=0 size=12088 trust=00000000 ...
[136.573119] MUYU SEC trust=00000000 tsb=0000000000000000 cntl=0 base=0 sz=0 prt=0
```

`stage=9`、`init/mem/auth/wait` 全 0 ⇒ TZ 三个 SMC 全部接受，
`CP_SET_SECURE_MODE(0)` 的 fence **完成**（对比 fix151 的 `wait=1` 超时）。
`RBBM_SECVID_TRUST_CNTL` **从 1 变成 0**，客户端阶段 25 次采样恒为 0，
`TSB_STATUS` 也从 `020000010011d740` 变成 0。

### 69.119.3 GPU 开始写像素 —— 硬判据全部翻转 ★★★★

| 判据 | fix135–fix151 基线（149 轮） | fix152 |
|---|---|---|
| `SCAN` 34 MB target | 全程 `zero=1275/1280 canary=4` | **47/49 行 `zero=0 other=1280 distinct=4..6`** |
| `CAN` canary | 全程 `ok=4/4 v0=a5a5a5a5` | **137.69 s 起 `ok=0/4 v0=18060180`，末态 `d8060180`/`ff060180`** |
| `FBC` 面板直方图 | 8 色各 288 恒定 | **`008=2287 ef7=16 555=1`（15 次稳定）** |
| `PFB` 独立读取器 | 彩条 8 色 | **`nz=768/768 distinct=2`、`PFBC 008=764 ef7=4`** |
| `FBD` | `ch=0/2304`（done 期间） | `138.358 s ch=2304/2304 rows=0..23` 整帧翻转 |

- `SCAN` 只有最初两块 target 的首次采样（n=1、n=3，136.7–137.6 s）
  还是 `zero=1280`；从 `n=2`（137.597 s）起目标表面被写满真实数据，
  且 `distinct=4..6` 逐帧变化 —— 不是一次 CPU 填充，是持续渲染。
- canary 是种在 GPU render target 里的 `a5a5a5a5`，**被 GPU 写成
  `18060180` / `d8060180` / `ff060180`**，这是最硬的「GX 侧 store
  抵达 DRAM」证据；此前 149 轮全程 4/4 存活。
- `SWAPD done n=1..8 status=00000000` 仍为 kOk，`MUYU TGT` 目标
  `0x101d36000 / 0x110cd0000` 与基线一致 —— 变化**只由 secure-mode
  exit 引起**，不是渲染状态或地址变了。

### 69.119.4 病根结论（正式关闭「GX store 不落地」这条主线）

> **muyu-17 一直没退出 GPU secure mode**：内核链接了空的 weak zap 镜像，
> `zapAndSecure()` 返回 `Z-1`，`CP_SET_SECURE_MODE(0x66)` 从未发出；
> 注释又照抄 sheng（有 zap）的假设，连 msm `-ENODEV` 分支的
> `RBBM_SECVID_TRUST_CNTL=0` 也没写。CPU 写该寄存器被硬件拒绝
> （fix150 K），未经 TZ 认证时 CP 发 0x66 会 fence 超时（fix151 J）；
> 只有加载**本板** `gen71100_zap.mbn` 走完整 TZ 认证才把
> `trust` 清成 0（fix152），GX 侧 store 随即抵达 DRAM。
>
> 这解释了此前全部反常形状：SMMU 无 fault / GBIF 无 halt / GX 电源
> 时钟开 / 引擎 busy 位亮 / fence 完成 / compute copy kOk /
> CP 的 CX 侧写能落地，唯独 RB/ROP/CCU/UCHE/compute 的 store
> 一个字节不到 DRAM —— **secure 侧的写被 SECVID 静默丢弃**。
> 也解释了为什么 A735 不能照抄 A732/A740：那两条线有 zap 镜像，
> 17 线没有，却沿用了「有 zap」的全部假设。

### 69.119.5 仍未解决（不放宽判据）

- 面板现在是 `008` 近单色（2287/2304 采样点），**不是桌面/图标**。
  按用户判据只承认「GPU 写像素」已达成，**不宣称桌面/首帧成功**。
- 出现新的下游形态：`GFXFAIL m=6 st=e00002d6`（16 次），
  `e00002d6 = kIOReturnTimeout`（§24.3，method 6
  `NativeAdrenoMethodWaitFence` 的 `waitAll(2 s)` 超时），
  但 `wf=8/8/7`、`10/10/9` 说明 fence 仍在推进，不是完全卡死；
  `MUYU HC hang n=1 ticks=181 hook=1` 在 150.32 s 仍在。
- 143.58 s 起的 `FBD ch=426..144` / `FBC 000=...` 与最后一次
  `SWAPD done` 反相关，仍是已由用户肉眼确认的 dump 控制台 ticker，
  **不计入 GPU 成功**。
- 闭环按长期授权自动还原原厂 `vendor_boot` 并回 recovery，因此
  fix152 的画面没有保留给用户肉眼确认；如需肉眼核对，下一轮需
  临时不走「自动还原」这一步（用户确认后再做）。

### 69.119.6 下一步方向（与旧路线完全不同）

- **默认行为已改变**：本板 zap 现在是内核强定义，`zapLimit()` 默认 4，
  后续所有构建自动带 secure-mode exit；`L` 可一键回退做对照。
- 主线从「GX store 不落地」转到 **GPU 能写之后的下游问题**：
  `m=6 / e00002d6` 的 waitFence 超时、`HC hang`、以及面板为何是
  `008` 近单色而不是桌面内容（首帧/合成路径/清屏色）。
- 不再回头查 SMMU 路由、页表换表、MRT/CRB、UCHE、CCU、
  BV/BR 协议 —— 这些层在 secure 状态下全部被误判过，现已有
  「退出 secure 后同一套代码立刻写出像素」的对照证据。

## §69.120 用户肉眼反馈：整屏蓝色 + Apple logo + 进度条（fix152 画面判读更正）

2026-11-15 17:10 用户原话：

> 「我看到整屏只有蓝色，苹果logo下方开始有进度条，这是此前从来没有的」

这是项目里**第一次**出现彩条 / dump 控制台之外的真实 Darwin 启动画面，
也是第一次肉眼反馈与内核硬判据逐字互证。以下更正 §69.119 的三处判读。

### 69.120.1 `008` 不是「近单色故障」，就是整屏蓝色

`NativeAdrenoSubmit.cpp:547` 的颜色桶 key：

```cpp
const uint32_t key=((p>>20)&0xfu)<<8|((p>>12)&0xfu)<<4|((p>>4)&0xfu);
```

即 BGRA 下 `[R 高4][G 高4][B 高4]`。因此：

| key | 含义 | fix152 读数 |
|---|---|---|
| `008` | R≈0x0x, G≈0x0x, B≈0x8x ⇒ **蓝色** | `2287/2304`（138.358–143.5 s 稳定 15 次） |
| `ef7` | R≈0xe0, G≈0xf0, B≈0x70 ⇒ logo 亮像素/抗锯齿 | `16` 点（96×24 稀疏网格只打得中 logo 这么多） |
| `555` | 中灰，边缘过渡 | `1` 点 |
| `000` / `fff` | 黑 / 白，143.566 s 后逐步增加到 `363`/`63` | 控制台文字与进度条区域 |

`MUYU PFBC`（显示驱动独立读取器）同步给出 `008=764 ef7=4`，与
`MUYU FBC` 一致 ⇒ 不是单一读取器的假象。用户「整屏只有蓝色」=
`008=2287`，「苹果 logo」= `ef7=16` 那 16 个亮点，「进度条开始有」
= 143.566 s 起 `000`/`fff` 进入。`FBD ch=2304/2304`（138.358 s）
是彩条 → 蓝色启动屏的整帧翻转，不是 CPU 刷黑。

⇒ §69.119.5 把 `008` 判成「近单色、不是桌面」的方向错了：
它确实是**启动屏背景**，内容真实存在，只是**进度条没走完、桌面/图标
仍未出现**。按用户「不要放宽判据」的约束，仍**不宣称首帧/桌面成功**。

### 69.120.2 `GFXFAIL m=6 e00002d6` 不是新故障（纠正 §69.119.5）

§69.53 早已定性：`req=<fence>/0` 的 `timeout_ns` 恒为 0，是
`tu_knl_darwin.cc:734`「All fence polls are nonblocking」的零超时
非阻塞 poll，`VK_TIMEOUT` 是「还没完成」的**正常应答**。
fix152 里 `sub=8 ret=7 d=7 req=8/0 wf=8/8/7`、
`sub=10 ret=9 d=9 req=a/0` 全部符合该形状，`req` 恒等于 `sub`、
`d[0]`/`ret` 恒为 `sub-1`，fence 序列持续推进（8→10→14→…→34）。
⇒ **不要再为消除 `e00002d6` 改 driver**，它不是进度条卡住的原因。

### 69.120.3 「8 帧上限」与 `HC hang` 是旧形态，不是 zap 引入的

四轮对照（同一份日志口径）：

| 轮次 | `SWAPD done` | `offer` | `HC hang` | recovery | `FBC` 蓝色行 | canary 被覆盖行 |
|---|---|---|---|---|---|---|
| fix149 | 8 | 9 | 1 @160.69 s | `fence=81 e00002ca` | 0 | 0/23 |
| fix150 | 8 | 9 | 1 @158.48 s | `fence=107 e00002ca` | 0 | 0/29 |
| fix151 | 8 | 8 | 2 | `fence=3 status=0 replay=2` | 0 | 0/27 |
| **fix152** | **8** | **9** | 1 @150.32 s | `fence=94 e00002ca` | **22** | **24/25** |

`NativeGOPPresenter.cpp:148/194/213` 三处日志节流都是
`if(n<=8 || !(n&31u))`：**前 8 次全打，之后每 32 次打一次**。
`offer n=32`（150.485 s）出现而 `done n=32` 没有 ⇒ offer 累计 ≥32 次，
`completeSwap()` 只回到 8 次，第 9 帧起 outstanding swap 不再完成。
`HC hang` / recovery 形状（`flags=2 dead=0 fsr=0 far=0 syn=0 replay=0`）
四轮一致，与 secure mode 无关。

⇒ fix152 独有的**只有** GPU 真的写出像素（蓝色 + logo + 进度条）；
剩余阻塞是「完成 8 帧后 `present()`/`waitUntilCompleted` 不再返回」这个
**旧瓶颈**，现在才成为通往桌面的唯一障碍。佐证：
`backboardd[58]` 在 148.769 s 被记为 1721 wakes/s（45567 次/26 s）的
忙等；`SWAPD offer` 从第 9 次起大量 `busy=1 acc=0`；154.932 s
`SpringBoard s2/0` 已启动，154 s 后仍有 `CoreAnalyticsHub` 心跳到
176 s，203 s 自复位，但桌面从未出现。

### 69.120.4 闭环与肉眼核对

§69.119.5 说「fix152 画面没保留给用户肉眼确认」不成立：用户在
闭环窗口内已经看到了该画面（蓝色 + logo + 进度条）。因此**不需要**
为肉眼核对改动「自动还原原厂」这一步，闭环流程保持不变。

### 69.120.5 下一步（fix153 方向）

- 主线目标：突破第 9 帧起的 present 停摆，让进度条走完到桌面。
- 不做：改 fence/wait 消除 `e00002d6`、改 SMMU/MRT/CRB/UCHE/CCU、
  换表、加第二条路由、放宽判据。
- 先放开 `offer/wait/done` 三处日志节流并打印累计
  `native_gop_gpu_offers/completions/failures/abandoned`，
  判明第 9 帧是「waitSwap 超时」「present 未返回」还是
  「completeSwap 未调用」，再决定改哪一层。

## §69.121 fix153：§69.120.3 的「8 帧上限」也被证伪，真阻塞是 recovery 把 submit poison 了

镜像 `fw/vendor_boot_gpu_fix153.img`（kernel sha `fd24effc…`，
载体 `swCD`），日志
`logs/vendor_boot_gpu_fix153-logs/muyu-673a4bca`：

```
tag=muyu kernel=389s msgbuf_reaches=388s gap=1s writer=298s
rounds=149 slots=147 errors=2 msgbuf=659217 bytes
```

闭环 17:18:39 刷入、17:25:35 自复位（**413 s**，此前各轮 ~203 s），
设备自动还原原厂并回 recovery，无硬挂。

### 69.121.1 改动（纯诊断，零行为变化）

`UniXNU-muyu-17/drivers/GopFramebuffer/NativeGOPPresenter.cpp`：

- `offer` / `wait` / `done` 三处节流从 `if(n<=8 || !(n&31u))`
  改成 `if(n<=24 || !(n&15u))`；`offer` 行追加累计
  `o= c= f= a= w= to=`（`native_gop_gpu_offers/completions/
  failures/abandoned` 与 `native_gop_wait_stats[0]/[1]`）。
- `waitSwap()` 超时分支新增 `MUYU SWAPT`；
  `completeSwap()` 的拒绝分支新增 `MUYU SWAPX`。

### 69.121.2 判据：present 从来没有停在第 8 帧

```
MUYU SWAPD done n=1..21  status=00000000 map=1   (137.386 → 162.893 s)
MUYU SWAPT n=1 ns=500653489 offered=0 delivered=0 o=12 c=12 w=13 to=1
MUYU SWAPT n=2 ns=500163333 offered=0 delivered=0 o=18 c=18 w=20 to=2
MUYU SWAPX count=0
```

`o` 与 `c` 恒等（12/12、18/18），`SWAPX=0`（没有一次
completeSwap 被拒），`SWAPT` 三次全是 `offered=0` 的 500 ms 正常空等。
⇒ **§69.120.3 把「done 只打 8 行」当成「第 9 帧起 present 停摆」是
第三次误判**：`<=8 || !(n&31u)` 在 n=9..31 之间一行都不打，
fix152 的 done 实际落在 9..31，日志看不见而已。swap 状态机全程健康。

### 69.121.3 画面确实在持续刷新，不是冻结

`FBD` 35 行里 22 行非零（`ch=564/2304`、`495`、`622`…）；
`FBC` 从 `008=2287 ef7=16 555=1` 演变为
`008=1547..2110 000=152..661 fff=54..88 ef7=14..16` 逐帧波动；
`PFBC` 独立读取器同步波动；`SCAN` 73 行全部
`zero=0 other=1280 distinct=5..6`、`CAN ok=0/4`。
即蓝色启动屏 + logo + 进度条/文字在 137–165 s 之间**持续重绘**，
与用户「进度条开始有」一致。仍**不是桌面/图标**，不宣称首帧成功。

### 69.121.4 真阻塞：166 s 的 hang → recovery 返回 `kIOReturnIOError` = `poison()`

```
[166.159587] MUYU HC hang n=1 ticks=194 hook=1 f=0/0 far=0/0 fc=0
[166.177622] NativeAdreno recovery: n=1 fence=140 status=e00002ca
             replay=0 flags=2 dead=0 info=0/91/0/0 fsr=0 far=0/0 syn=0
```

- 165.063 s 之后 `FBC`/`FBD`/`PFBC`/`SCAN`/`SWAPD` **全部消失**，
  而内核心跳（`CoreAnalyticsHub`）一直跑到 388.58 s ⇒
  **窗口长度不是瓶颈**（389 s 也没能进桌面），是 GPU 侧在 166 s 被关掉。
- `e00002ca = kIOReturnIOError`（`IOReturn.h:157`），在本工程里正是
  `NativeAdrenoSubmit::poison()`（`NativeAdrenoSubmit.cpp:116-119`）的返回值。
  `recover()` 里共有 6 处 `return poison()`，其中
  `restart(cookie,ring(0),hung)` 失败是最可能的一处
  （`:1634-1635`）；一旦 poison，`hangCheck()` 立刻
  `if(!attached_ || poisoned_) return kIOReturnNotReady`，此后所有提交被拒。
- **硬件没有 hang**：全程 `MUYU ERR ist=00000000 trap=0 oob=0
  hang=0 gpc=0 cphw=0 ccufl=0 stallfault=0`，`fsr=0 far=0 syn=0`，
  `dead=0`。`hangCheck()` 每 500 ms 判一次，条件只是
  「fence 连续两次不变 + `fence<last` + `madeProgress()==false`」
  ≈ **1 s 无 fence 推进即判 hang**；闭环脚本自己写着
  「The progress bar is not a measurement (1.4 fps)」，每帧约 0.7 s，
  阈值偏激进，存在误判空间。
- `flags=2` 表示 recovery 找到了 guilty job，`replay=0` 表示一帧都没重放。

### 69.121.5 下一步（fix154）

主线改为 **「别让一次可疑的 hang 把 submit 永久 poison」**：

1. 把 `native_adreno_recover_trace[0..3]` 与
   `native_adreno_poison_site[0]` 打进 msgbuf，定位究竟哪一处
   `return poison()` 触发（`restart` 失败 / `fenceValue` /
   `write_table_` / `resetExecutedTable` / replay）。
2. 用新载体把 hang 判定从「连续 2 个 500 ms tick」放宽到
   「连续 N 个 tick」做单变量对照；**只在硬件 `hang=0/ist=0/
   stallfault=0/fsr=0` 的前提下放宽软件阈值，不放宽任何
   GPU 写像素判据**。
3. 若 `restart` 本身失败，修的是 restart 路径而不是 hangcheck。
4. 仍不改 fence/wait 消除 `e00002d6`、不换表、不加 SMMU 路由、
   不回头查 MRT/CRB/UCHE/CCU。

## §69.122 fix154：poison 现场定位到 `recover()` 的 replay 死锁

镜像 `fw/vendor_boot_gpu_fix154.img`（kernel sha `92fc727a…`，`swCD`），
日志 `logs/vendor_boot_gpu_fix154-logs/muyu-67244d39`：
202 s 自复位 / 175 s 窗口 / 43 轮 / gap=0，自动还原原厂回 recovery。

### 69.122.1 改动（纯诊断）

`UniXNU/drivers/Adreno740/NativeAdrenoSubmit.cpp`：

- `poison()` 打印 `MUYU POIS site=<__LINE__> n=<次数> tr=<recover_trace[0..3]>`；
- `hangCheck()` 判 hang 前打印 `MUYU HANGJ last/fence/hf/retries`。

### 69.122.2 决定性读数

```
MUYU HANGJ last=97 fence=91 hf=91 retries=1
MUYU POIS  site=1680 n=1 tr=5/3758097090/91/97
MUYU HC hang n=1 ticks=174 hook=1 f=0/0 far=0/0 fc=0
NativeAdreno recovery: n=1 fence=92 status=e00002ca replay=0
                       flags=2 dead=0 info=0/61/0/0 fsr=0 far=0/0 syn=0
```

- `last=97`、`fence=91` ⇒ CP 完成的 fence 停在 91，客户端已提交到 97，
  **6 个 outstanding job**；`hf=91` 说明上一个 tick 也是 91。
- `tr[0]=5` ⇒ `recover()` 第 5 号失败分支，即 replay 循环里的
  `status=useVm(vm,clock); if(status) {trace[0]=5;...return poison();}`
  （当前文件 `:1680`，加注释前行号约 `:1667`）。
- `tr[1]=3758097090=0xe00002c2` = **`kIOReturnBadArgument`**
  （`IOReturn.h:147`）。`tr[2..3]=91/97` 即 fence/last。
- `status=e00002ca` 仍是 `poison()` 的 `kIOReturnIOError`，
  `replay=0` 印证**一帧都没能重放**。

### 69.122.3 这是一个逻辑死锁，不是硬件 hang

`useVm()`（`:177-190`）在 `table_mode_==1` 下先 `waitAll(clock,2000000)`
等**所有 outstanding job 退休**，再写 TTBR0。而 hang 的成因正是
这 6 个 job 没退休 ⇒ `waitAll`/`wait()` 拿不到进展、返回
`kIOReturnBadArgument` ⇒ `recover()` 立刻 `poison()` ⇒
`poisoned_=true` 后 `hangCheck()` 直接 `kIOReturnNotReady`、
所有后续提交被拒 ⇒ **GPU 被永久关掉**。

所以「一次可疑的 hang」必然触发「必然失败的 recovery」，而 recovery
的失败不是偶发，是结构决定的。硬件侧证据仍然全零：
`MUYU ERR ist=0 trap=0 oob=0 hang=0 ccufl=0 stallfault=0`、
`fsr=0 far=0 syn=0 dead=0`。`SWAPD done` 一直成功到 141.757 s
（n=16），hang 在 142.981 s，中间仅 1.2 s；闭环脚本自注进度条
约 1.4 fps（每帧 ~0.7 s），而 hangcheck 每 500 ms 一判、
约 1 s 无 fence 推进即判 hang。

### 69.122.4 下一步（fix155）

主线明确为 **「别让一次软件误判把 submit 永久 poison」**，两条候选：

1. 单变量放宽 hang 判定：要求连续 N 个 500 ms tick 同 fence 且
   `madeProgress()==false` 才判 hang（N 从 2 提到 8~20），
   用新载体字母控制，与 `swCD` 基线对照。
   **只在硬件 hang=0/ist=0/fsr=0 的前提下放宽软件阈值，
   不放宽任何 GPU 写像素判据。**
2. 修 `recover()` 的 replay：不要在 replay 里用会等待
   outstanding job 的 `useVm(vm,clock)`；或让该失败分支不
   `poison()`，保持 submit 可用。

判据仍以 `SCAN/FBD/FBC/PFB/IMW` 与用户肉眼为准；桌面/图标
出现之前不宣称首帧成功。

## §69.123 fix155：修掉 `recover()` replay 死锁，GPU 不再被永久 poison

镜像 `fw/vendor_boot_gpu_fix155.img`（sha `d83f3630…`，kernel
`ac0c14a3…`，载体 `swCD`，trust/addon 仍 fix107），
日志 `logs/vendor_boot_gpu_fix155-logs/muyu-67507589`：
202 s 自复位 / 175 s 窗口 / 43 轮 / gap=0，自动还原原厂回 recovery。
构建时 `MUYU RCVM` 在 kernel.macho 中 4 处；trust/addon 的
`img.find()` 分别为 12751872 / 610304，与 fix154 完全同参。

### 69.123.1 改动（只修 bug，不放宽任何判据）

`UniXNU/drivers/Adreno740/NativeAdrenoSubmit.cpp::recover()` 的 replay 循环：

- `table_mode_==1 && vm!=table_vm_` 时不再走 `useVm(vm,clock)`。
  它的第一句 `waitAll(clock,2000000)` 等的正是 `jobs_` 里那些
  **即将被 replay、且因 hang 没有退休的 job** ⇒ 必然超时
  `0xe00002c2`（fix154 的 `tr=5` / `POIS site=1680`）。
  restart() 已经让 CP 空闲，改为直接
  `write_table_(...,vm->ttbr0())` + `invalidateTlb()` + `table_vm_=vm`。
- 只有需要**第二次**切表时才等待，且只等本轮 replay 刚提交的那个
  fence（`ring(0)->wait(replay_wait,clock,200000)`），完全不碰
  `jobs_`，避免一个 job 跑在下一个 job 的表下面。
- 新增 `MUYU RCVM`（切表 TTBR0/TLB 结果）与 `MUYU RCVMW`
  （二次切表前的等待）；`poison()` 各分支、`HANGJ`、
  全部像素硬判据原样保留。
- 载体、boot-arg、zap、trust/addon、SMMU/MRT/CRB/UCHE/CCU 一律不动。

### 69.123.2 决定性读数：`POIS=0`，4 次 recovery 全部成功

```
[137.714623] MUYU HANGJ last=11 fence=8 hf=8 retries=1
[137.1007..] MUYU RCVM f=10 ttbr=001000091147c000 tlb=ok
[137.1008..] NativeAdreno recovery: n=1 fence=9  status=0 replay=2 flags=2 dead=0 fsr=0
[157.287040] ... n=2 fence=96  status=0 replay=4
[162.774122] ... n=3 fence=110 status=0 replay=5
[173.512588] ... n=4 fence=138 status=0 replay=6
MUYU POIS count=0     MUYU RCVMW count=0     MUYU HC hang count=4
```

- `status=0` 取代 fix154 的 `e00002ca`，`replay=2/4/5/6` 取代
  `replay=0`；`poison()` 一次都没进 ⇒ **一次可疑 hang 不再把
  submit 永久关掉**，§69.122 的逻辑死锁已解除。
- `RCVMW=0`：四次 replay 的 job 全属于同一个 client VM，整轮只切
  一次表、零等待，死锁路径彻底消失。
- 硬件仍全零：`ERR ist=00000000 trap=0 oob=0 hang=0 ccufl=0
  stallfault=0`（另见 `busy=00800005 gpu=1` 的正常忙碌态）、
  `fsr=0 far=0 syn=0 dead=0`；`ZAPOK trust=00000000` 且
  `SEC trust=0` 一直保持到 175.287 s。四次 hang 都是软件阈值
  误判，不是硬件 hang。

### 69.123.3 像素硬判据延续到窗口末端（fix154 在 142 s 就断）

| 判据 | fix155 末次读数 | 时间 |
| --- | --- | --- |
| `FBC` 37 行 | `008=1526 000=669 fff=94 ef7=14 555=1` | 174.554 s |
| `FBD` 37 行 | `ch=136/2304 rows=0..23` | 174.566 s |
| `PFBC` 11 行 | `008=455 000=275 fff=35 ef7=3` | 173.522 s |
| `SCAN` 75 行 | `zero=0 other=1280 distinct=5 first=cdcdcdcd fail=0` | 174.804 s |
| `CAN` 38 行 | `t=305 ok=0/4`（canary 已被 GPU 覆盖） | 175.020 s |
| `SWAPD done` | `n=1..24 status=00000000`（fix153 21 / fix154 16） | 168.993 s |

- FBC 全程仍是启动屏配色 `008=1526..2129 000=103..669
  fff=55..94 ef7=14..16 555=1` 逐帧波动，即蓝色 + logo +
  进度条/文字持续重绘；`MUYU FB|` ASCII 采样到 175.685 s 仍有内容。
- 窗口在 175 s 被既有 `sheng-autoreset=300` + Gunyah watchdog
  自复位切断，**不是 GPU 停写**。仍**没有桌面/图标**，
  不宣称首帧/桌面成功（判据未放宽）。

### 69.123.4 剩余瓶颈与下一步（fix156）

- 四次 hang 在 137 / 157 / 162 / 173 s，平均约 12 s 一次，每次都
  触发 `restart()` + replay。虽然全部成功，但反复重置 CP 会打断
  启动动画推进，且窗口只有 175 s。
- 下一步只做 §69.122.4 的候选 ①：把 hang 判定从「连续 2 个
  500 ms tick」放宽为「连续 8 个 tick（4 s）fence 与 CP 状态都不变」，
  用独立计数；**仅在 `ist=0 hang=0 stallfault=0 fsr=0` 前提下
  放宽软件超时**，`SCAN/FBC/FBD/PFBC/CAN` 等写像素硬判据一个
  都不动、不放宽。与 fix155 镜像做单变量对照，看 restart 次数
  是否归零、画面是否越过启动屏。
- 独立线索（本轮未动）：`backboardd(58) deny(1)
  iokit-open-user-client AGXDeviceUserClient`、`SpringBoard[27]
  3727 wakes/s` 仍出现在 132–137 s；桌面进程是否真正完成启动
  需要与 GPU 写像素判据分开判断。

> **fix156 更正**：`backboardd(58) deny(1)
> iokit-open-user-client AGXDeviceUserClient` 这条线索**已在
> §69.55.2 被排除**（`verdict=1`、`MUYU OPEN` 均未跟 `refused`，
> user client 实际打开成功）。fix156 日志里的 9 次 deny 是 sandbox
> 日志噪音，不是阻塞点，不再回头查 MACF/filter 三道门。

## §69.124 fix156：放宽软件 hang 超时的单变量对照

镜像 `fw/vendor_boot_gpu_fix156.img`（sha `6723b81a…`，kernel
`25d60fdc…`，载体 `swCD`，trust/addon fix107），
日志 `logs/vendor_boot_gpu_fix156-logs/muyu-6748bfd7`：
202 s 自复位 / 175 s 窗口 / 43 轮 / gap=0，自动还原原厂回 recovery。
与 fix155 **唯一差别**是 hang 判定时机。

### 69.124.1 改动（软件超时，硬判据一个不动）

- `NativeAdrenoSubmit.h` 新增 `hang_stall_ticks_`；
  `hangCheck()` 在 fence 变化 / `fence>=last` / `madeProgress()` 时清零，
  只有连续 `HangStallTicks=8` 个 500 ms tick（4 s）都无进展才判 hang
  （原来约 1 s）。`HANGJ` 增加 `stall=` 字段。
- `recover()` 成功时一并清零该计数。
- **`SCAN/FBC/FBD/PFBC/CAN/SWAPD`、zap、载体、boot-arg、
  SMMU/MRT/CRB/UCHE/CCU、fix155 的 replay 修复全部原样保留**；
  放宽的只是软件超时，不是任何「GPU 是否写了像素」的判据。
  依据仍是 fix155 的硬件全零（`ist=0 hang=0 stallfault=0 fsr=0`）
  和 4 次 recovery 全部成功。

### 69.124.2 对照结果

| 指标 | fix155 | fix156 |
| --- | --- | --- |
| `MUYU HANGJ` / `HC hang` / `recovery` | 4 / 4 / 4 | **1 / 1 / 1** |
| `NativeAdreno recovery` | `status=0 replay=2/4/5/6` | `status=0 replay=12` |
| `MUYU POIS` | 0 | **0** |
| `MUYU RCVM` | 4 | 1 |
| `FBC` / `FBD` | 37 / 37（末 `ch=136/2304`） | **38 / 38（末 `ch=537/2304`）** |
| `PFBC` / `SCAN n=` / `CAN` | 11 / 75 / 38 | **13 / 77 / 39** |
| `SWAPD done` | 24（到 168.993 s） | 23（到 173.749 s） |
| 末次硬判据时间 | FBC 174.554 / SCAN 174.804 / CAN 175.020 | **FBC 174.602 / SCAN 174.856 / CAN 175.060** |
| 桌面 / 图标 | 无 | **无** |

```
[145.783624] MUYU HANGJ last=68 fence=55 hf=55 retries=1 stall=8
[145.783641] MUYU HC hang n=1 ticks=178 hook=1 f=0/0 far=0/0 fc=0
[145.784117] MUYU RCVM f=57 ttbr=00100008f9ba5000 tlb=ok
[145.786596] NativeAdreno recovery: n=1 fence=56 status=0 replay=12
             flags=2 dead=0 fsr=0 far=0/0 syn=0
```

- `stall=8` 直接证明新阈值生效：只有 145.783 s 那一次真的连续
  4 s 无 fence/CP 进展；其余三次 fix155 会误判的 1–2 s 停顿
  都自行恢复，不再触发 `restart()`。
- 唯一一次 recovery 仍 `status=0`（fix155 的修复继续有效），
  `replay=12` 只是积压更多帧，`POIS=0` 保持。
- 像素判据**不减反增**：SCAN 77 行全 `zero=0 other=1280`、
  `distinct=5 fail=0`，CAN `t=313 ok=0/4`，FBD 末行变化量
  `ch=537/2304`（fix155 为 136），FBC 仍是
  `008=1565..2008 000=198..664 fff=60..83 ef7=14 555=1` 的
  启动屏配色逐帧波动，`MUYU FB|` ASCII 到 175.745 s 仍有内容。
- 硬件仍全零：`ERR ist=0 trap=0 oob=0 hang=0 ccufl=0
  stallfault=0`、`fsr=0 far=0 syn=0 dead=0`。

### 69.124.3 结论：restart 频繁不是桌面未出现的原因

- §69.122 的 poison 死锁（fix155）与 §69.121 的「软件误判把 GPU
  关掉」两条线**都已结案**：GPU 现在从 136 s 一直写像素到
  175 s 窗口被 watchdog 切断，全程无硬件 hang / fault / poison。
- 把 restart 从 4 次压到 1 次后，像素判据只略微变好，
  **画面仍停在蓝色启动屏 + logo + 进度条/文字，没有桌面或图标**。
  ⇒ 反复 `restart()` 不是阻塞；判据未放宽，仍不宣称首帧/桌面成功。
- 主战场因此从「GPU 能否写像素」转到「**175 s 窗口内客户端是否
  提交过桌面内容 / 系统启动是否推进到桌面**」。日志里
  `SpringBoard[27]` 123 s 已启动、`launchd Early boot complete`
  121 s，但 142 s 后基本只剩 CoreAnalytics 心跳与 sandbox 噪音；
  FBC 始终是 `008` 蓝底，没有图标多色分布。这需要新的硬判据
  来区分「客户端没提交新内容」与「提交了但合成/显示没换页」。

### 69.124.4 下一步（fix157，待选）

1. **纯诊断轮（推荐）**：不改任何行为，给 swap/帧提交打上
   「客户端进程 / 提交序号 / 目标表面 IOVA / FBC 配色指纹」的
   时间线，把 136–175 s 的每次提交与用户态启动事件对齐，
   判明桌面内容是否曾经到达 GPU。判据仍只用直接读面板/目标表面
   的硬数据，肉眼画面只作现象。
2. **长窗口对照**：历史上 fix144/fix153 出现过 413 s 自复位 /
   389 s 内核窗口，但 `BootReset.c:48` 注释明确
   「174–246 s 后重启，无论 `sheng-autoreset` 设多少，因为
   deadline 检查落在会停止运行的代码路径里」，触发条件不稳定。
   在 GPU 已不再 poison 的前提下值得再取一次长窗口，但要接受
   可能失去自动复位、需长按电源键 10–15 s 人工干预的风险。
3. **不做**：再放宽 hang 阈值、改 fence/wait 消 `e00002d6`、
   换表、SMMU 路由、MRT/CRB/UCHE/CCU、MACF/sandbox 三道门
   （§69.55.2 已排除）、照抄 uke A732 / sheng A740 的 mode0
   或 CPU 拷贝路径。

## §69.125 fix157：用户态遥测心跳 —— 客户端一直在渲染，只是内容不变

镜像 `fw/vendor_boot_gpu_fix157.img`（sha `bc7ea7b2…`，kernel
`0f393111…`，载体 `swCD`，trust/addon fix107），
日志 `logs/vendor_boot_gpu_fix157-logs/muyu-675ab055`：
202 s 自复位 / 175 s 窗口 / 43 轮 / gap=0，自动还原原厂回 recovery。

### 69.125.1 改动（纯诊断，零行为改动）

`UniXNU/drivers/Adreno740/NativeAdrenoPublish.cpp`：

- 把 USTEXT 的 `static unsigned telemetry` 暴露为
  `native_adreno_ustext_count`（构建时 `-Werror` 拦截了
  `volatile` 的 `++`，改为显式 `+1u`），记录 `0xC000` 行通道的
  `native_adreno_metal_rows_last`。
- hangcheck 每 10 个 500 ms tick（5 s）打一行 `MUYU USHB`：
  `rows/last/marks/calls/abi/hist/wf/txt/op/infl/sel/st` +
  最近一行遥测文本。
- **不改任何提交/恢复/切表/显示行为，不放宽任何写像素判据。**

### 69.125.2 证伪 §69.124.4 的前提：USTEXT 停止不是普遍现象

| 读数 | fix156 | fix157 |
| --- | --- | --- |
| `MUYU USTEXT` 行数 / 末次 | 56 / **142.073 s** | **168 / 173.943 s** |
| `MUYU USHB` 行数 | — | 20（58.7→168.2 s） |
| `POIS` / hang / recovery | 0 / 1 / 1 | **0 / 2 / 2（全 `status=0 replay=10/14`）** |

- USHB 全程递进：`t=170`(140 s) `calls=389 abi=392 hist=392
  wf=27 txt=28` → `t=180` `778/798/798/129/56` → `t=190`
  `1222/1245/1246/381/98` → `t=200`(168 s) `1574/1605/1605/498/140`。
  `sel=5 st=0 infl=0 op=4/3` 恒定：最后一个 selector 是 submit、
  状态成功、没有卡在飞的 method。
- ⇒ **用户态 Turnip/Metal 客户端在 142 s 之后一直在调用驱动并等待
  fence**，fix156 的「用户态文本通道停摆」是那一轮的个别现象
  （第四次误判），不是阻塞。GPU 内核提交心跳 `SYNC/SB/IBNZ`
  同步持续到 175 s 也印证这一点。
- `rows=0 last=0` 全程为 0 ⇒ **48 行遥测表（`0x8000` marker
  通道）从未被用户态使用**，这条通道作废，以后不要再依赖它。

### 69.125.3 决定性读数：渲染循环在推进，渲染内容不变

USTEXT 时间序列（fix157，133→174 s，12 组）：

```
Q h1 q0/0  p0    c0    f0    → q51/51 p1066 c1058 f125
W a1 e0    R0               → e51    R51
S c0/0 d0  R0 D0 F0        → c51/0 d44 R56 D111 F38
R g0 a0 s1 k0/0/0          → s46    k0/0/253039
P z1 n1 w1x1 d4 fi1        → n46 w3200x2136 d3 m2 f26
                              k6835 c2 v98 vt18 fi748（自第二组起恒定）
```

- `Q`（queue/commits/frames）、`W`（witness pass）、`S`、`R`、`E`
  的计数**全部单调递增到 173.9 s** ⇒ 合成循环健康、没有停摆。
- `P` 行的 `n` 是 `tu_pass_ring` 的 newest 序号（1→46，约 1.1
  pass/s，与 1.4 fps 相符）；其余字段是**该 pass 的形状**：
  `w3200x2136`(render area)、`d3`(draws)、`v98`(VkFormat)、
  `vt18`(verts)、`fi748`(**fsInstrs，片段着色器指令数，不是帧号**)。
  它们从启动屏建立后完全不变 ⇒ 每个 pass 都用同一个 shader、
  同一渲染区域、同样 3 个 draw，**持续重绘同一张启动屏**。
- 硬判据互证：`MUYU TGT` 全程只有 4 个固定目标
  （`0x101d36000` / `0x1126e3000` 两个面板双缓冲 `p=200 f=4b`
  计数 `x113/x97` 持续增长，另两个 `p=400 f=b68` 停在 `x10/x11`），
  **没有出现任何新的渲染目标**；`SCAN` 59 行全
  `zero=0 other=1280 fail=0`、`CAN` canary 被覆盖；`FBC` 到
  175.014 s 仍是 `008=1527..2287 000/fff/ef7/555` 启动屏配色。

### 69.125.4 结论：GPU 链路结案，瓶颈在用户态启动/合成内容

- 从 fix152（zap → GPU 真正写像素）→ fix155（poison 死锁）
  → fix156（软件误判）→ fix157（客户端心跳）四轮之后，
  **驱动、submit/recovery、fence、页表、scanout、用户态
  Turnip 客户端全部用硬数据证明持续健康**；硬件
  `ERR ist=0 hang=0 stallfault=0`、`fsr=0`、`SEC trust=0`。
- 桌面/图标没出现的原因不是 GPU 不写、不是客户端死了、不是
  restart、不是换页：**用户态合成器自始至终只提交同一个启动屏
  pass，从未产生桌面图层内容**。这条已超出 GPU 驱动范畴。
- 仍**不放宽判据、不宣称首帧/桌面成功**；肉眼画面（蓝屏 +
  logo + 进度条）只作现象，与 FBC/TGT/CAN 一致。

### 69.125.5 下一步

1. **长窗口对照（现在最值得做）**：GPU 侧已无任何已知故障，
   唯一没验证过的变量是时间。历史上 fix144/fix153 出现过
   413 s 自复位 / 389 s 内核窗口；当时 GPU 在 166 s 被 poison，
   长窗口被浪费了。现在值得再取一次，看启动进度条能否走完。
   风险仍在 `BootReset.c:48`：174–246 s 的 deadline 落在会停止
   运行的代码路径，触发不稳定，可能失去自动复位、需长按电源
   10–15 s 人工干预 —— **需用户确认后执行**。
2. **转向用户态启动阶段**（host 侧先做、零风险）：查
   `backboardd`/`SpringBoard` 为何一直只提交启动屏图层，
   重点看 `P` 行的 `tu_pass_rec` 生产者与 surface/pass 绑定，
   以及 142 s 之后 launchd 是否还有 boot task 推进。
3. **不做**：再放宽 hang 阈值、改 fence/wait、换表、SMMU
   路由、MRT/CRB/UCHE/CCU、MACF/sandbox 三道门、
   `0x8000` 遥测行通道（本轮证明从未使用）、照抄
   uke A732 / sheng A740 的 mode0 或 CPU 拷贝路径。

## §69.126 fix158：长窗口对照（`sheng-autoreset` 300→600）—— 窗口未拉长，deadline 不是控制变量

§69.125.5 候选①（长窗口对照）经用户确认后执行。镜像
`fw/vendor_boot_gpu_fix158.img`（77,393,920 B，sha
`bbbd4b87eaf1bb93…`，内核 `6acd4cf5334bd0a2…`，载体 `swCD`、
trust/addon fix107），日志
`logs/vendor_boot_gpu_fix158-logs/muyu-675521ad`：**202 s 自复位 /
178 s 内核窗口 / 44 轮 / gap=1 s**，msgbuf 685,166 B、
`write_ms=9/221`。host 侧 wait 由 420 s 提到 900 s。设备按时自动
还原原厂并回到 recovery，**无硬挂、无需长按电源人工干预**。
闭环输出 `logs/autoloop-fix158.log`。

### 69.126.1 改动（单变量，逐字对照 fix157）

- 只改 `UniXNU-muyu-17/board.toml:580`：
  `"sheng-autoreset=300"` → `"sheng-autoreset=600"`。
  **字节数不变**（`payload.json kernel_args` 两版都是 1014 B），
  GPU 代码、载体 `swCD`、zap、trust/addon、`SCAN/FBC/FBD/
  PFBC/CAN/SWAPD/USHB` 判据一个都没动。
- 三步链与 §69.20.2 参数逐字相同，构建日志
  `logs/build-{kernel,payload,vendor-boot}-fix158.log`，
  kernel 0 error。
- **离线单变量校验**：`cmp -l` 两版镜像只有 **288 字节**差异，
  语义改动只有 1 字节（同一偏移 `67209258` 的 `300`→`600`）；
  其余是每次构建都会变的 `chosen/random-seed`（62 B）、
  `_PrelinkKCID`（21 B）、ADT/prelink 校验字节与 ramdisk 校验和。
  kernel sha 从 `0f393111…` 变 `6acd4cf5…` 只由这些随机字段造成，
  **不是代码或行为改动**。镜像内 trust/addon 逐字节命中
  （`6fc1914b…` / `410685f6…`），`MUYU USHB`/`MUYU RCVM` 诊断串、
  `sheng-tum=swCD` 均在，且不含 `sheng-autoreset=300`。

### 69.126.2 决定性读数：窗口没有变化

| 指标 | fix157（300） | fix158（600） | fix153（历史偶发长窗口） |
| --- | --- | --- | --- |
| 自复位 / 内核窗口 | 202 s / 175 s | **202 s / 178 s** | 413 s / 389 s |
| 轮数 / msgbuf | 43 / 566,841 B | 44 / 685,166 B | 149 / 659,217 B |
| `FBC` / `SCAN` / `CAN` | 29 / 59 / 30 | **42 / 89 / 45** | 35 / 73 / 37（判据止于 165 s） |
| `SWAPD done` / `USTEXT` / `USHB` | 14 / 168 / 20 | 24 / 197 / 20 | 21 / 42 / 0 |
| `HANGJ` / recovery / `POIS` | 2 / 2 / 0 | **1** / 1 / 0 | 0 / 1（166 s poison） |
| 桌面 / 图标 | 无 | **无** | 无 |

- 硬判据全部延续到窗口末端：`FBC` 137.369→**177.044 s**、
  `SCAN n=` 136.295→**177.335 s**、`CAN t=` 136.532→
  **177.604 s**、`SYNC/PKTS/CRBS/SDS` 到 **177.931 s**；
  `FBC` 仍是 `008=1566..2287 000/fff/ef7/555` 的蓝色启动屏配色
  逐帧波动。
- 硬件全零：`ERR ist=00000000 hang=0 stallfault=0`、
  `SEC trust=0` 到 177.885 s、`POIS=0`；唯一一次
  `HANGJ ... stall=8` → `recovery status=0 replay=12`，
  `RCVM tlb=ok`，比 fix157 少一次。
- 内容仍冻结：`TGT` 全程只有 4 个固定目标
  （`0103e0a000` / `0112da4000` / `0101d36000` / `0110cd0000`
  + 空目标），`x` 计数递进但**无新表面**；`USTEXT P` 行
  `n1→65` 全恒定在
  `w3200x2136 d3 m2 f26 k6835 c2 v98 vt18 fi748`（与 fix157
  的 `n1→46` 同一 shader/区域/draw）；`USHB` 到 167.701 s
  `calls=1989 abi=2039 wf=709 txt=154 sel=5 st=0 infl=0`，
  用户态客户端持续活跃。窗口末端只有 `SpringBoard` wake 噪音
  与 sandbox 日志，**没有桌面/图标/新图层**。

### 69.126.3 源码级根因：deadline 分支从未触发

- fix158 的 `[reset]` 日志只有 90.301 s 的 `probe:` 一行，
  **没有** `autoreset: pings stopped`、`watchdog:` 或
  `tick %us` 行 ⇒ `boot_reset_wdt_hurry()` 从未被调用，
  自复位不是 deadline 触发的。
- `boot_log_tick(unsigned now)` 的唯一调用点是
  `package/LegacyDisplayProbe.cpp:541` 的
  `if (&boot_log_tick) boot_log_tick(0);` —— 传的是**常数 0**，
  且只调用一次；`BootLog.c:474` 的
  `now >= boot_log_autoreset` 因此永远不成立（而且
  `boot_log_autoreset` 要到第一次 tick 的 `boot_log_started`
  之后才由 `PE_parse_boot_argn` 赋值）。
- 所以 `sheng-autoreset` 实际上只作为 `boot_reset_watchdog(
  boot_log_autoreset, …)` 的 `wdt_deadline` 参数交给
  `wdt_thread`；重启由该 ping 线程自身到期/停摆 + 1 s
  hypervisor timeout 决定，与 boot-arg 数值不成比例。
  fix144/fix153 的 389 s 是这条时钟路径的**偶发表现**，
  不是可配置出来的窗口；`BootReset.c:48` 注释里
  「174–246 s 后重启，无论 `sheng-autoreset` 设多少」由此
  得到源码级解释。

### 69.126.4 结论（不放宽判据、不过度推论）

1. **`sheng-autoreset` 不是观察窗口的控制变量**：300→600 后
   窗口只从 175 s 变 178 s、自复位仍是 202 s。
   §69.125.5 设想的「用 boot-arg 再取一次长窗口」路径**不成立**，
   以后不要再调这个数值。
2. **「时间是不是瓶颈」这一假设本轮仍未被检验**：窗口只多了
   3 s，属于同一观察区间，不能据此断言时间不是变量。
   长窗口对照的目标（看进度条能否走完）没有达成。
3. 单变量对照本身干净：GPU 侧健康度不降反升（hang 2→1、
   判据行数 +30~50%），判据延续到窗口末端，内容仍是启动屏，
   与 §69.125 的「用户态合成器从未提交桌面图层」一致。
   仍**不宣称首帧/桌面成功**。
4. 源码基线已回退到 `sheng-autoreset=300`（与 fix157 一致），
   fix158 镜像冻结在 `fw/` 供复现；`board.toml` 注释保留本轮
   阴性结论，避免以后重复试探。

### 69.126.5 下一步

1. **若仍要检验时间假设**：修
   `LegacyDisplayProbe.cpp:541`，让 `boot_log_tick` 每秒收到
   真实 uptime（或直接改 `wdt_thread` 的 deadline 语义），
   deadline 才会真正生效。这是**会改变自复位路径的行为改动**，
   风险与 §69.125.5 同级（可能失去自动复位、需长按电源
   10–15 s），**需用户确认后执行**。
2. **host 侧零风险**：查 `backboardd`/`SpringBoard` 为何只
   提交启动屏图层，重点看 `P` 行的 `tu_pass_rec` 生产者、
   surface/pass 绑定，以及 142 s 之后 launchd 是否还有
   boot task 推进（§69.125.5 候选②，一直没做）。
3. **不做**：继续调 `sheng-autoreset` 数值（本轮证伪）、
   再放宽 hang 阈值、改 fence/wait、换表、SMMU 路由、
   MRT/CRB/UCHE/CCU、MACF/sandbox 三道门、
   `0x8000` 遥测行通道、照抄 uke A732 / sheng A740 的
   mode0 或 CPU 拷贝路径。

## §69.127 fix159 — 每秒真实 uptime tick（时钟修复生效；含落盘日志截断说明）

§69.126.5 候选①，用户 2026-11-15 明确确认后执行。目标是让
`boot_log_tick` 每秒收到真实 uptime，使 `sheng-autoreset`
deadline 真正生效、拿到一次长窗口。

> **更正（用户 18:59，以现场观察为准）**：**窗口延长了，设备也
> 自动复位了。** 本节初版把「落盘日志末端 175 s」误当成「内核
> 窗口长度」，据此写下的「窗口没延长 / 回归 / 失去自动复位 /
> 需长按电源」**全部作废**。落盘日志只到 175 s 是 `writer` 在
> 用户态 CPU 自旋下停止刷盘造成的**记录截断**，不是窗口终点；
> 300 s 的 tick 与 `wdt_hurry()` 发生在截断之后，因此
> `autoreset:` 行没有落盘。以下 69.127.2–69.127.4 的证据数字
> （header、tag 计数、判据时间戳）本身有效，但只描述
> **前 175 s**；其中有关窗口长度与复位的定性语句一律以本更正
> 为准。修正后的结论见 69.127.5a。

### 69.127.5a 更正后的结论

1. **修 tick 生效**：时钟正确已在 119 s / 149 s 证实，现场确认
   窗口延长且自动复位成功 ⇒ `sheng-autoreset` 在 tick 修好后
   **确实是观察窗口的控制变量**。§69.126.4 的「不是控制变量」
   只在 tick 未修好（`boot_log_tick(0)` 常数 0）的前提下成立，
   须按此限定理解。
2. **「时间是不是瓶颈」得到强阴性证据**：窗口延长后画面
   **仍是蓝色启动屏**（用户现场「还是卡在启动页面」），落盘
   判据也显示内容冻结（`TGT` 4 个固定目标无新增、
   `USTEXT P` 恒定）。给足时间也没有桌面 ⇒ 瓶颈在用户态
   合成内容，不在观察时长；§69.125 的结论进一步被证实。
3. 源码仍保持回退到 fix158 基线（三文件逐字还原、
   `board.toml` 仍 300），fix159 镜像 / 日志 /
   `logs/fix159-tick.patch` 冻结供复现；**不再为此上机**。
4. 下一步仍是 host 侧零风险分析：CPU 自旋来源、142 s 后
   launchd boot task、`tu_pass_rec` 生产者与 surface/pass
   绑定。不再调 `sheng-autoreset` 数值。

### 69.127.1 改动与产物（单变量）

- 只改 3 个文件；GPU 驱动、像素判据、`board.toml`
  （仍 `sheng-autoreset=300`，cmdline 1014 B）、载体 `swCD`、
  trust/addon fix107、zap 全未动：
  - `package/BootLog.c`：新增 `log_tick_thread`
    （`BASEPRI_REALTIME`），每 200 ms 调
    `boot_reset_wdt_ping()` 喂 1 s timeout 的 Gunyah watchdog，
    每 5×200 ms 调 `boot_log_tick(kernel_uptime_s())`；
    `kernel_uptime_s()` 读 `cntpct_el0/cntfrq_el0`，与 msgbuf
    `[ 123.456789]` 前缀同源；每 30 次打一行
    `[log] tick %us n=%u deadline=%u tried=%d`。
  - `package/BootReset.c`：导出 `boot_reset_wdt_ping()`
    （一次 `GunyahWdtPing` SMC + 计数），不动 `wdt_thread`
    的 deadline 语义。
  - `package/BootLogStubs.c`：weak 兜底。
- 编译零 error，`nm` 确认 `_boot_reset_wdt_ping` 是强定义
  （不是 weak 兜底）。产物
  `fw/vendor_boot_gpu_fix159.img`
  （sha `e71b365f…`，kernel `2464f70e…`），日志
  `logs/vendor_boot_gpu_fix159-logs/muyu-67440eb1`，改动冻结在
  `logs/fix159-tick.patch`（109 行）供复现。

### 69.127.2 上机：自动复位失效

- 18:44:59 刷入 + UEFI RAM boot，18:45:03 起 host 等 900 s。
  **6+ 分钟无 fastboot、USB 离线、屏幕仍在启动页**；
  用户长按电源 10–15 s 后 18:52:16 fastboot 出现，自动接管
  还原原厂 `vendor_boot` → reboot recovery → 拉 32 MiB 窗口解码。
  设备 `a20248f7` 现停 recovery，原厂/充电正常。
- header：`kernel_uptime_s=175`、`msg_bytes=613997`、
  `rounds=43`、`slots_written=40`、`write_errors=3`、
  `write_ms=3/218`、`state=2`、`uptime_s=85`。即与
  fix157（175 s）/ fix158（178 s）**同一观察区间**，
  300 s deadline 从未到达。

### 69.127.3 证据：时钟修好了，deadline 仍未触发

- `[log] tick` 仅有的两行：
  `459:[ 119.169649] [log] tick 119s n=30 deadline=300 tried=0`、
  `2772:[ 149.267514] [log] tick 149s n=60 deadline=300 tried=0`。
  间隔 30 s、n 增 30 ⇒ **每秒一次且拿到真实 uptime**；
  `deadline=300` 说明 boot-arg 已解析。§69.126.3 的
  「常数 0」障碍确实被修好。
- 但 `[reset]` 仍只有 90.243 s 的 `probe:` 一行，
  **无** `autoreset: pings stopped`、**无** `watchdog:`，
  `tried=0` 到最后可见行；179 s 应有的 `n=90` 行也不存在，
  因为 writer 在 175 s 已停止落盘。
- 三版共同的 ~175 s 停摆（与 watchdog / boot-arg 无关）：

  | | fix157 | fix158 | fix159 |
  | --- | --- | --- | --- |
  | launchd wake 超限 | 104.9 s | 103.4 s | 101.3 s |
  | SpringBoard | 132.8 s | 137.9 s | 136.1 s |
  | backboardd | 149.7 s | 141.0 s | 137.5 s |
  | 记录末端 | 175.407 s | 177.931 s | 175.908 s |

  三版都是 ~45,000 wakes / 300 s 的用户态 CPU 自旋；
  `BootLog.c` 的 fix26 注释早已记录「SpringBoard 在这颗模拟
  CPU 上自旋后，连 `BASEPRI_REALTIME` 的 writer 都拿不到调度，
  记录停在 174 s 而设备跑到 203 s」。**175 s 是系统级 CPU
  饿死，不是 GPU、不是 watchdog、不是 boot-arg。**
- 推断（不宣称定论）：175 s 后 writer 与新增 tick 线程一起
  停摆，硬件时钟虽在走却无人调 `boot_log_tick`，deadline
  分支无人触发；同时新增的每 200 ms 显式 ping 抹掉了
  fix157/158 赖以在 202 s 自复位的「ping 线程停摆 →
  1 s timeout 咬断」路径，设备既不前进也不重启。落盘日志在
  175 s 截断，**无法区分**「tick 线程停摆」与
  「hurry 未生效/被拒」，不再为此单独上机。

### 69.127.4 判据对照（GPU 侧健康、内容仍冻结）

| | fix157 | fix158 | fix159 |
| --- | --- | --- | --- |
| 窗口 / 自复位 | 175 s / 202 s | 178 s / 202 s | **175 s / 无（人工）** |
| FBC / SCAN / CAN | 29 / 59 / 30 | 42 / 89 / 45 | 36 / 72 / 36 |
| HANGJ / recovery | 2 / 2 | 1 / 1 | 2 / 2 |
| POIS / ZAPOK | 0 / 1 | 0 / 1 | 0 / 1 |

- 硬件全零：`ERR` 两种状态 `busy=00000001` /
  `00800005` 都是 `ist=0 hang=0 stallfault=0`，
  `SEC trust=00000000`，`POIS=0`。
- 判据延续到末端：FBC 137.269→174.321 s、SCAN
  136.568→174.645 s、CAN 136.819→174.987 s、
  USTEXT 133.941→175.146 s、USHB 58.669→169.695 s
  (`calls=1947 abi=1988 wf=690 txt=168 sel=13 infl=0`)。
- 内容依旧是蓝色启动屏：`TGT` 4 个固定目标
  （`0103e0a000` / `01147b7000` / `0101d36000` /
  `0110cd0000` + 空目标）`x` 计数递增但**无新表面**；
  `USTEXT P` `n1→54` 恒定在
  `w3200x2136 d3 m2 f26 k6835 c2 v98 vt18 fi748`；
  窗口末端只有 `CoreAnalyticsHub` / sandbox /
  `MUYU ABI deleted`。**桌面/图标/进度条推进仍无，
  不宣称首帧成功。**

### 69.127.5 结论与下一步（初版判读，已被 69.127.5a 取代）

> ⚠️ 以下 1 / 2 / 4 与 69.127.2–69.127.4 中有关窗口长度、
> 自复位的定性语句均为初版误判，仅作证据链保留，
> **请勿据此决策**；以 §69.127 开头的更正块与 69.127.5a 为准。

1. **修 tick 的目标只达成一半**：时钟正确已在 119 s /
   149 s 证实，但窗口 175 s 未延长、桌面未出现，
   「时间是不是瓶颈」**依然未检验**（且现在更强地指向
   用户态 CPU 自旋，而非观察时间）。
2. **这是回归**：失去自动复位、需人工长按电源。三文件已
   逐字回退到 fix158 基线（`BootLog.c` `5256d261…` /
   `BootReset.c` `1ccc9758…` / `BootLogStubs.c`
   `5380763d…`），`board.toml` 仍 300；fix159 镜像、日志、
   `logs/fix159-tick.patch` 冻结供复现，**这版不再上机**。
3. **默认下一步（host 侧零风险）**：三版一致的
   launchd/SpringBoard/backboardd CPU 自旋是明确靶点 ——
   查 45k wakes/300 s 的来源、142 s 后 launchd boot task
   是否推进、`tu_pass_rec` 生产者与 surface/pass 绑定，
   即 §69.125.5 候选② / §69.126.5 候选②（一直没做）。
4. **不做**：再改 `boot_log_tick` / `wdt_thread` /
   watchdog ping（本节证伪且引入回归）、再调
   `sheng-autoreset` 数值（§69.126 证伪）、放宽 hang 阈值、
   换表、SMMU、MRT/CRB/UCHE/CCU、MACF/sandbox、
   `0x8000`、照抄 uke A732 / sheng A740 的 mode0 或
   CPU 拷贝路径。

## §69.128 host 侧零风险分析：合成内容冻结发生在哪一层

§69.125.5 / §69.126.5 / §69.127.5a 候选②，2026-11-15 执行。
**纯 host 侧只读**：只读 fix157/158/159 落盘 msgbuf 与本地源码，
不刷机、不改内核、不碰设备（全程 `a20248f7` 停 recovery）。
目标是把「用户态合成器从未提交桌面图层」推进到具体一层。

> **更正（用户 2026-11-15 19:22 后的 §69.129 复核，请优先读 §69.129）**
> 本节的**原始读数全部保留**，但三处定性结论已被证伪或弱化，请勿据此决策：
> ①「142 s 后 boot task 不再推进」是 175 s 日志截断造成的采样偏差
>   —— 390 s 长窗口里 142 s 之后每 20 s 仍有 2 条 `==>` 直到末端；
> ②「TGT 集合冻结 / USTEXT 字段恒定 ⇒ 没有新图层树」不成立
>   —— 4–6 项目标是 31 轮的常态，390 s 轮次里 `U n6`、`S c95`、
>   `P n104`、backboardd 在 278 s 重启并重新 import surface；
> ③ 69.128.5 列的三个待查点（watchdogd CRITEXIT / sandbox deny /
>   `b=e=0`）已在 §69.129 全部查清，均非瓶颈。
> **修正后的瓶颈层级见 §69.129.4**：问题不在时间、不在用户态是否
> 提交、也不在 GPU 是否健康，而在**合成结果没有落地到 scanout
> surface**（与 §69.96 的历史硬结论同形状）。

### 69.128.1 USTEXT 通道与字段语义（源码定位）

- `MUYU USTEXT` 不是内核判据，是**用户态 Metal 前端的遥测
  快照**：`NativeAdrenoMetalPlugin.mm:502-551` 组装 14 行
  （`F/S/U/D/R/E/Q/P/L/M/N/Y/Z/W`），经 `streamText` →
  marker → `NativeAdrenoPublish.cpp:1153` 的
  `IOLog("MUYU USTEXT %s")`。`NativeAdrenoMetalPlugin.mm:495`
  明确「**只在计数器变化时发送**」，`NativeAdrenoPublish.cpp:1149`
  为「每组 14 行、最多 30 组」，故 USTEXT **不是每帧一条**。
- `P` 行 = `NativeBridgeMetalFrontend.mm:4900` 的
  `native_bridge_metal_pass_ring_line()`，通过
  `tu_pass_ring_read` 读 Turnip 的 `tu_pass_rec`（72 B，
  `UniXNU/patches/mesa/turnip-gmem-mutable-swap.patch:324`，
  由 `tu_note_pass()` 在 `tu_note_render_mode()` 每次 render
  pass 时写入，patch:410/500）的**最新一条**：

  | 字段 | 含义 | fix159 值 |
  | --- | --- | --- |
  | `n` | pass ring 的 newest **seq** | 20→54 |
  | `w` | render area | `3200x2136`（全屏） |
  | `d/m/f/k/c/x` | draws / mode / flags / kpx / colors / swap | `3 / 2 / 26 / 6835 / 2 / 0` |
  | `b/e` | `gpuBegin`/`gpuEnd` 非零？ | 140 s 前 `1/1`，143 s 后多为 `0/0` |
  | `v/vt/fi/fw/fr` | format / verts / fsInstrs / fsWaves / fsRegs | `98 / 18 / 748 / 16 / -1` |

  `NativeBridgeMetalFrontend.mm:4895-4898` 注释：`b=e=0`
  表示「CPU 记了 pass 但命令流没到 GPU」，除非 Turnip 从未分配
  `tu_pass_ts_bo`（本轮出现过 `b=e=1`，故 BO 已分配）。
- `MUYU TGT` = `NativeAdrenoSubmit.cpp:804` 的**内核侧 ROP
  目标表**（`p=`pitch、`f=`fmt、`x=`提交次数）。
  `MUYU USHB` = `NativeAdrenoPublish.cpp:275` 的 hangcheck
  心跳，`calls` 是用户态 marker 调用数。

### 69.128.2 三重内容冻结证据（fix159，对照三版）

1. **表面状态冻结**：USTEXT `F` 行 140.142 s 之后 11 组
   一字不变 `tex x0 s4/0 p4 b8 r12800/3200 c8 f115 550 554
   80 43 23 25 55`；`USTEXT` 共 196 行 = 14 组 × 14 行，
   133.941→175.038 s 每 ~3 s 一组，**用户态前端全程活着**。
2. **render pass 参数冻结**：`P` 行 `n` 从 20（140.142 s）
   增到 54（175.097 s）—— 35 s 内 Turnip 确实记录了
   **34 个新 render pass**（约 1/s，与 1.4 fps 进度条吻合），
   但 `w/d/m/f/k/c/v/vt/fi/fw/fr` **全部恒定**：同一个
   3200×2136、3 draws、SYSMEM mode 2、2 colors、18 verts、
   748 指令的同一 shader。`USHB calls` 0(≤130 s) → 80
   (133.9 s) → 761 (141.1 s) → 1543 (160.1 s) → 1947
   (169.7 s)，用户态客户端持续高频调用。
3. **内核目标冻结**：`TGT` 在 137.223 s 为 2 项、
   137.945 s 为 4 项、**138.510 s 达 5 项后集合再不变**，
   `x` 计数从 7 递增到 92（173.623 s）；三版首次到 5 项
   分别 141.295 / 137.317 / 138.510 s。内核一直把同一组
   surface 交给 ROP。

⇒ **三层都在跑，但内容源没有变化**：GPU 硬件/内核提交
（判据到 175 s、`ERR` 全零、`TGT x` 递增）、用户态
Metal/Turnip 前端（USTEXT 14 组、pass seq 递增、
`USHB calls` 递增）、系统心跳（`CoreAnalyticsHub` 每 0.5 s）。
缺失的是**新的图层树 / drawable 内容**。

### 69.128.3 142 s 后 boot 推进停滞（三版共性）

| | fix157 | fix158 | fix159 |
| --- | --- | --- | --- |
| `CRITEXIT ... comm watchdogd ppid 1 sig 5` | 142.741 s (pid 209) | 143.608 s (pid 210) | 142.002 s 控制台 (pid 212) |
| `PMRD: System sleep prevented by kPMSystemRestartBootingInProgress` | 129.112 s | 130.299 s | 130.445 s |
| `AGXDeviceUserClient` sandbox deny 次数 | 9（132.763 s 起） | 7（134.073 s 起） | 6（133.260 s 起） |
| `[log] ufs lock: N give-ups` | 1→2 | 1→5 | 1→4 |
| 142 s 后非心跳非 MUYU 行 | 32 | 30 | 21 |

- fix159 服务启动标记 `==>` 计数：**<130 s 22 条、
  130–142 s 57 条、>142 s 仅 12 条**；142 s 后非 MUYU
  行几乎只有 `CoreAnalyticsHub::handleNagTimerExpiry`
  每 0.5 s 一次（30→31 events 恒定）与少量 sandbox deny /
  `tx_flush`。**boot task 不再推进**，系统仍挂在
  `kPMSystemRestartBootingInProgress`（启动未完成标志）。
- `watchdogd` 三版都在 ~142 s 以 `sig 5` 退出，此前有
  `watchdogd[..] Corpse allowed 1 of 5`；这是三版最一致的
  确定性事件，与「142 s 后服务启动骤停」同刻。
  **因果未证实**：它可能是启动停滞的原因，也可能是结果。
- `AGXDeviceUserClient` deny（`res=e00002e2 macf=1
  foreign=0`，涉及 mediaserverd / backboardd，type
  16711/16712）三版都有，但同期 GPU 提交未中断，
  故**不是全局阻塞**；是否阻断某类桌面图层创建待查。

### 69.128.4 结论与对 §69.127 的修订

1. **瓶颈层级确定**：不在 GPU 硬件、不在驱动 submit/scanout、
   不在观察时长，而在 **Turnip/Metal 前端之上**——
   `backboardd`/`SpringBoard` 没有提交新的图层树；合成器
   只能反复重绘 138.5 s 已建立的启动屏 pass。§69.125 的
   「用户态合成器从未提交桌面图层」得到源码级定位。
2. **修正 §69.127.3 的「175 s 系统级 CPU 饿死」**：
   135–175 s 每 5 s 仍有 **230–290 行** MUYU 日志、
   USTEXT 每 3 s 一组，是**持续重绘同一帧的空转**，
   不是整体停摆；CPU wake 超限（launchd ~101–105 s /
   SpringBoard ~133–138 s / backboardd ~137–150 s）是
   自旋症状而非全局死锁。`writer` 停止刷盘由
   `[log] ufs lock: N give-ups, worst wait 100 ms`
   （N 逐轮递增）解释，与 §69.127 更正块的「记录截断」一致。
3. **待解释的矛盾**：`P` 行 143 s 后 `b=e=0` 占 9/11 组，
   按源码注释意味着命令流未到 GPU，但内核 `SYNC/SB/IBNZ/
   FBC/SCAN/CAN` 同时活跃到 175 s。二者并存，可能是 pass
   ring 记录领先于执行、时间戳写回路径未生效，或提交的是
   另一条 scanout/zap 命令流。**不断言，列为待查点**。

### 69.128.5 下一步（仍为 host 侧零风险）

1. **最具体、可证伪的靶点**：查 `watchdogd` 的
   `CRITEXIT sig 5` 来源与 142 s 后 launchd boot task
   为何停滞 —— 提取 CRITEXIT 前后 corpse 报告、
   `kPMSystemRestartBootingInProgress` 的清除条件、
   142 s 后 `launchd[1]` 是否还在派发 job。
2. 查 `AGXDeviceUserClient` sandbox deny
   （type 16711/16712、`res=e00002e2`）是否只挡了某类
   桌面图层 client；对照 `NSG gpu ... proc=backboardd
   verdict=1` 的白名单放行范围。
3. 解释 `b=e=0` 与内核 submit 判据并存（pass ring 时间戳
   BO 写回路径 / 命令流归属）。
4. **不做**：GPU 驱动、换表、SMMU、MRT/CRB/UCHE/CCU、
   放宽 hang 阈值、`0x8000` 通道、`boot_log_tick` /
   `wdt_thread` / watchdog ping、`sheng-autoreset` 数值、
   照抄 uke A732 / sheng A740 的 mode0 或 CPU 拷贝路径。

## §69.129 host 侧零风险复核：三个待查点结案与瓶颈层级修订

§69.128.5 的三个候选，2026-11-15 19:22 后继续做。
**仍是纯 host 侧只读**：只读全工程落盘 msgbuf 与本地源码，
不刷机、不改内核、不碰设备（`a20248f7` 全程停 recovery）。
本节同时修订 §69.128 的三处过度推论，原读数全部保留。

### 69.129.1 待查点①：`CRITEXIT watchdogd sig 5` 是固有事件，不是本轮变量

| 证据 | 读数 |
| --- | --- |
| 出现范围 | `logs/*/muyu-*/msgbuf.txt` 中 **fix15 → fix143 → fix159 共 40+ 轮**都有，含 fix64/fix100–fix143 |
| 时刻 | 141.70 / 143.61 / 142.00 s（fix157/158/159），fix64 141.70 s、fix113 142.15/143.38 s |
| 形态 | watchdogd 重启 3 次（pid 53 → 143/144/141 → 209/210/212），前两次 `Corpse allowed 1 of 5`，第三次 `CRITEXIT ... ppid 1 sig 5 code 0 reason` |
| 紧随其后 | `DEV /dev/disk0..5` + `DEV /dev: 10 disk nodes` + `CONSOLE TAIL (1023 bytes):` |

- 这三行**不是设备 panic 的新信号**，是项目自加的诊断链：
  `HANDOFF-2026-11-02.md:747-803` 的
  `zzzzzzzzzzzzzzz-critexit-reason.patch` 在
  `proc_handle_critical_exit()` 的 panic 之前打印 CRITEXIT；
  `HANDOFF-2026-11-04.md:84-86` 的
  `muyu_panic_with_log()` 把 msgbuf 尾部（`CONSOLE TAIL`）
  与 `DEV /dev/disk*` 列表打到 panic 屏。判据是 userland 在
  `posix_spawn` 时设的 `p_crash_behavior`（`kern_exit.c:1698`），
  **不是 init**。
- `braveark-hold-reboot=1` 仍在 `board.toml:324`：失败轮停在
  panic 画面而非重启。同文档 §4.1.3 还写明
  `p_crash_behavior_deadline` 过期就不再 panic，
  「跑得慢只会留下进度条不动的**静默挂起**」——正是这些年
  观察到的现象，两类表现是**同一类故障**。
- ⇒ **结论**：一个跨 40+ 版本、固定落在 141–144 s 的既有事件，
  与 §69.128.3 说的「142 s 后服务骤停」只是同刻，不是新因，
  也不能解释 fix157+ 的画面冻结。**不再把 watchdogd 列为靶点。**

### 69.129.2 待查点①续：142 s 不是「launchd 停派」，是日志截断的采样偏差

§69.128.3 用 fix159 的 175 s 截断日志统计出
「<130 s 22 条 / 130–142 s 57 条 / >142 s 仅 12 条 `==>`」。
拿 390 s 长窗口复核：

| 轮次 | 末端 | `==>` 142 s 后的分布 | 其它推进 |
| --- | --- | --- | --- |
| fix113 `muyu-67291202` | 390.32 s | 140–160 s 16 条，之后**每 20 s 恒定 2 条**一直到 380–400 s | `Early boot complete. Continuing system boot.` @122.08 s |
| fix129 `muyu-67680518` | 390.66 s | 持续派发 | `MTLAssetUpgraderD` @184.70 s；**backboardd 重启 @278.53 s（pid 268）**并重新 `MUYU OPEN` @280.11 s |
| fix64 `muyu-6738c8a1` | 185.71 s | after=14 | — |

⇒ 服务派发**没有停止**，只是速率从 ~2.7 条/s 降到 ~0.1 条/s；
`kPMSystemRestartBootingInProgress` 未清除是结果不是闸门。
**§69.128.3「142 s 是真正的分界点」作废。**

### 69.129.3 待查点②③：sandbox deny 不阻断、`b=e=0` 是采样相位

**（a）`AGXDeviceUserClient` deny**

- fix159 共 25 行 `deny(`，按客户端：backboardd(58) 7、
  findmydeviced 5、其余各 1；涉及 AGX 的**只有 backboardd**，
  type 16711(0x4147)/1/16712(0x4148)，`res=e00002e2 macf=1`。
- `NativeAdrenoPublish.cpp:1206-1245`：`res` 是
  `io_filter_resolver` 的意见、`macf` 是 `mac_iokit_check_open`
  的，**两者都在驱动决定之前取**；驱动对非 kernel 客户端设
  `NativeSandboxGrant` 属性放行（:1242-1245）。
  类名故意叫 `AGXDeviceUserClient` 就是为了过原始 sandbox
  profile（:11-13）。`UniXNU/docs/zh/devices/alioth.md:457/459`
  早已定性：「res NotPermitted with refused 0 means the filter
  commented and the open was served anyway」。
- 三版实测 `MUYU OPEN refused=0`、`MUYU OPEN foreign=0`
  （OPEN type= 行 5/5/4 条），即**全部 open 都被服务**。
  ⇒ **不阻断任何桌面 client，结案。**

**（b）`b=e=0` 与内核 submit 并存**

- `NativeBridgeMetalFrontend.mm:4900` 打印的是
  `p.gpuBegin?1:0, p.gpuEnd?1:0`（布尔化的 GPU 回写时间戳）。
  三版取值集合：fix157 `9×b0 e0 + 3×b1 e1`、
  fix158 `12×0 + 2×1`、fix159 `11×0 + 3×1`。
  ⇒ `b1 e1` 出现过，`tu_pass_ts_bo` 确已分配并执行过
  `CP_EVENT_WRITE7(RB_DONE_TS)`（patch:358-403），
  源码注释里「never allocated ⇒ 永远 0」的 caveat **不成立**。
- `P` 行 `n` 增速约 **1 个新 pass/s**（fix159 20→54 / 35 s），
  而 USTEXT 组每 **~3 s** 一次，且每组只取
  `passRead(newest-3, recs, 4)` 的**最新一条**（:4892-4894）。
  快照几乎必然抓到一个刚被 CPU 记录、尚未被 CP 执行完的
  in-flight pass ⇒ `b=e=0` 是**采样相位**，不是「命令流未到 GPU」。
  旁证：`native_bridge_metal_status` 的 `c`(commits)/`d`(completed)
  同步增长（fix159 63/56、fix129 95/95），命令确实在被消费。
  ⇒ **矛盾消解，不再列为待查点。**

**（c）顺带关掉两个伪线索**

- `S` 行末尾 `native-metal: binary archive refused` /
  `no device answer` 是
  `NativeBridgeMetalFrontend.mm:531-537` 的 `harmlessProblem()`
  白名单，源码原话「Notes every process makes and that
  **change nothing it draws**」，不是失败原因。
- `TGT` 集合 4–6 项在 **31 轮**里是常态（4 项 1 轮、5 项 29 轮、
  6 项 1 轮），不能当「内容冻结」的证据；fix129 在 207.89 s
  才出现第 6 项，且 390 s 里 `U n6`、`S c95`、`P n104`，
  长窗口下用户态**会** import 新 surface / 重启合成器，
  但**仍然不进桌面**。⇒ §69.128.2 的「缺新图层树」不是硬结论。

### 69.129.4 修正后的瓶颈层级：结果不落地，而非没人提交（**已证伪，见 §69.130**）

> **更正（§69.130 复核，请勿据本段决策）**：本段把
> `cdcdcdcd` 当成「canary / 合成结果没落地」是**误读**。
> `0xCD` 正是 witness `fillBuffer` 写入的期望值，
> `p16/cdcdcdcd c16/cdcdcdcd` 是 **GPU 写入成功抵达 DRAM**
> 的正面证据；`D P46/18` 也证明 render pass 目标就是
> IOSurface；`B0/16`、`W0` 都是本板架构下的预期值。
> §69.96 的「GX store 不抵 DRAM」主线在 **fix152 已被
> §69.119 正式关闭**（加载本板 zap shader 退出 secure mode）。
> 三条「证据」全部作废，修正后的层级见 §69.130.5。
> 原读数保留作证据链。

三层都健康且长时间窗口下仍在跑，三个候选全部排除后，
剩下的是**合成结果没有落地到 scanout surface**：

| 判据（fix159 末组 @175 s） | 读数 | 含义 |
| --- | --- | --- |
| `S c63/0 d56 p31 ... F46` | 63 次 commit / 0 失败 / 56 完成 / 46 个全屏 pass | 用户态持续提交且被消费 |
| `U n2 ... w3200x2136 a34177024` | 2 个已 import surface，34 MiB 全屏 | 交换链就位 |
| `D P46/18 B0/16 W0` | 46 个 pass 打在 surface，但 **0 个 blit 目标命中 surface / 0 drawable texture** | 写入目标可疑 |
| `W a1 e63 r0 ... p16/cdcdcdcd c16/cdcdcdcd R63` | witness 63 轮全 encoded / 0 refused，但自有 buffer 与从 surface 拷回的首 word **都是 `cdcdcdcd` canary** | 不是 fix95 预期的 magenta `0xC3FF03FF` |
| 内核 `FBC/SCAN/CAN/SWAPD` | 延续到 174–175 s，`ist=0 hang=0` | GPU 侧健康 |

- 这与 §69.96（fix129 时代）的历史硬结论**同形状**：
  「CP（CX 域）取指/寄存器写/`CP_EVENT_WRITE7` 写内存全部正常；
  RB/ROP/UCHE（GX 域）寄存器读回正确、busy 会断言、事件会发，
  **但发起的 store 从不抵达 DRAM**」。
- **边界必须写清**：§69.96 是 fix129 时代（GPU 还会 poison、
  `SCAN`/`SWAPD`=0），fix157+ 已修好 poison 且判据全活跃，
  **不能直接照搬**；`cdcdcdcd` 也可能是 witness 填充初值而非
  「没写」。本轮只把层级收窄，不宣称找到根因。
- §69.121（14795 行）早已记过「窗口长度不是瓶颈（389 s 也没能
  进桌面）」，但当时理由是 GPU 在 166 s 被 poison 关掉；
  fix157–159 无 poison、fix159 窗口也已延长，**时间假设这次
  是在 GPU 健康的前提下被关掉的**——与 §69.127.5a 一致。
- **桌面/图标/进度条仍无，不宣称首帧成功。**

### 69.129.5 下一步（仍为 host 侧零风险，可证伪）

1. 用现有落盘日志对照 fix129 / fix144 / fix153（389–390 s）
   与 fix157–159 的 `W` / `D` / `SCAN` / `FBC` 判据，确认
   `cdcdcdcd` 是否为同一现象、fix157+ 是否出现过非零首 word。
2. 查 `gProbeOwnWord` 为何是 `cdcdcdcd` 而不是 magenta：
   `W a1 e63 r0`（witness 全 encoded、0 refused）是否说明
   pass 只被编码、没有被 CP 执行；对应 fix95 的实验设计
   （`NativeBridgeMetalFrontend.mm:180-185`）。
3. 查 `D B0/16` 与 `W0`（drawable textures = 0）：合成器
   render pass 的目标是否根本不是 IOMFB swap surface。
4. **不做**：上机、改内核、改 GPU 驱动、换表、SMMU、
   MRT/CRB/UCHE/CCU、放宽 hang 阈值、`0x8000` 通道、
   `boot_log_tick` / `wdt_thread` / watchdog ping、
   `sheng-autoreset` 数值、MACF/sandbox 三道门、
   照抄 uke A732 / sheng A740 的 mode0 或 CPU 拷贝路径；
   watchdogd / CRITEXIT / sandbox deny / `b=e=0` 不再复查。

## §69.130 host 侧复核：`cdcdcdcd` 是成功标志，瓶颈回到用户态

§69.129.5 的三个方向，2026-11-15 19:35 后继续。
**仍是纯 host 侧只读**：全工程落盘 msgbuf + 本地源码，
不刷机、不改内核、不碰设备（`a20248f7` 停 recovery）。
**本节证伪 §69.129.4 的核心结论**，原读数保留。

### 69.130.1 方向①：全 67 轮 witness 只有两种取值，且分界恰在 fix152

| `W` 行 `p…/… c…/…` | 行数 | 轮次 |
| --- | --- | --- |
| `p0/00000000 c0/00000000` | 480 | fix95–fix151 共 59 轮（含 fix129/fix113 的 390 s 长窗口） |
| `p16/cdcdcdcd c16/cdcdcdcd` | 49 | **fix152–fix159 共 8 轮，8/8** |

- fix159 时间序列：133.948 s `e0 R0 p0/00000000` →
  137.116 s `e1 R1 p0/00000000` → **140.144 s 起
  `e17 R17 p16/cdcdcdcd c16/cdcdcdcd`** 恒定到 175.146 s
  `e63 R63`；fix157 末 `e51`、fix158 末 `e84` 同样形状。
- **fix129（390 s 长窗口）到 232.570 s 仍是 `e95 R95
  p0/00000000`** —— 长窗口本身不会让 0xCD 落地，
  转折是版本（fix152），不是时间。

### 69.130.2 方向②：`cdcdcdcd` = `fillBuffer(0xCD)` 的期望值 ★

- `NativeBridgeMetalFrontend.mm:3285-3291` 当前实现：
  `nmb_blit_fill_buffer(e, surfaceBO, 0, 64, 0xCD)`、
  同一 pass 里对**本设备自有** 16 KiB buffer 也填 `0xCD`，
  再把 surface 前 4 KiB `copy_buffer` 到其 8192 偏移。
  按字节填 `0xCD` ⇒ 32-bit word 就是 `0xcdcdcdcd`。
- `:3266-3269` 读回逻辑：`own/copy` 是 16 个采样 word 中
  非零的个数，`gProbeOwnWord/CopyWord` 是 `p[0]/p[2048]`。
  ⇒ `p16/cdcdcdcd c16/cdcdcdcd` = **自有 buffer 与
  imported host-page surface 两边都被 GPU 写满 0xCD**。
- 读回在**下一轮** witness pass 开头、上一轮 fence 早已
  signal 之后，且先 `sys_dcache_flush`（:3259-3264），
  不存在 CPU cache / 竞态假阳性。`e63 r0 s0` =
  63 次编码成功、0 拒绝、无 stage 错误。
- §69.129.4 引用的 magenta `0xC3FF03FF` 是
  `:173` 的 **fix87 早期注释**；fix89 起已改成
  `fillBuffer(0xCD)`（§69.89 表、§69.72 明确写
  「两块 swap surface 都做 `fillBuffer(0xCD)`」），
  fix95 又加了 p/c 双对照。**我上一轮把旧注释当成
  当前实现，因此把成功判据读成了失败判据。**

### 69.130.3 关键：这条主线 fix152 已经正式关闭

§69.119（★★★★「病根确认并修复」）记的是同一件事：

- 加载本板 `fw/gpu-firmware/gen71100_zap.mbn`（12088 B）
  走完整 TZ 认证 → `CP_SET_SECURE_MODE(0)` fence 完成、
  `RBBM_SECVID_TRUST_CNTL` **1 → 0**（`ZAPOK …
  trust=00000000`）。此前内核链接的是空 weak zap 镜像，
  GPU 一直没退出 secure mode。
- §69.119.3 判据翻转：`SCAN` 从 `zero=1275/1280`
  变成 `zero=0 other=1280 distinct=4..6`；GPU 把种在
  render target 里的 canary `a5a5a5a5` **写成
  `18060180`/`d8060180`**；`FBC 008=2287`、
  `FBD ch=2304/2304`、`PFB nz=768/768`。
- §69.120 用户肉眼确认 fix152 画面是
  **整屏蓝色 + Apple logo + 进度条** —— 这正是 GPU
  画出的真实像素，不是黑屏/未落地。
- ⇒ §69.96（fix129 时代）「GX 侧 store 从不抵达 DRAM」
  的结论**在 fix152 后已失效**；§69.129.4 把它套用到
  fix157+ 是**跨时代的错误类比**。`cdcdcdcd` 与
  `SCAN first=cdcdcdcd` 都是这次修复的后续表现。

### 69.130.4 方向③：`B0/16` 与 `W0` 都是预期值

| 字段 | 源码判据 | 说明 |
| --- | --- | --- |
| `P46/18` | `:3123-3125` `surfaceTarget`（color attachment 0 是 `NBTexture` 且 `.surface != nil`） | **46 个 render pass 确实打在 IOSurface 上**，目标就是 swap surface |
| `B0/16` | `:2669/:2700` 只在 `copyFromTexture:toTexture:` / `copyFromBuffer:…toTexture:` 里累加 `gBlitTargets`，`gSurfaceBlitTargets` 还要求目标是 `NBTexture` | witness 用的是 `fill_buffer`/`copy_buffer`（目标 **buffer**），根本不进这两个计数；16 个 texture blit 目标不是 surface ⇒ `B0` 是预期 |
| `W0` | `:3126` 只在 `textureFlag(t0,"isDrawable")` 为真时累加 | 本板走 IOMFB/IOSurface 而非 CAMetalLayer drawable ⇒ `W0` 是架构预期 |

⇒ §69.129.4 表格里「写入目标可疑」「不是预期 magenta」
两行**全部作废**；`P46/18` 反而是正面证据。

### 69.130.5 修正后的瓶颈层级（第三次收敛）

1. **GPU 侧全链路健康**：fix152 起 secure mode 已退出、
   GX store 抵达 DRAM、witness 0xCD 双边落地、
   `FBC/FBD/PFBC/SCAN/CAN/SWAPD` 活跃到 174–175 s、
   `ist=0 hang=0 stallfault=0`、`POIS=0`。
2. **时间不是瓶颈**：fix129/fix144/fix153 的 389–390 s
   长窗口也没进桌面；fix159 已延长窗口且自动复位，
   画面仍是启动屏（§69.127.5a）。
3. **⇒ 屏幕上那张蓝色启动屏就是 GPU 真实画出来的**，
   瓶颈回到**用户态合成内容未推进到桌面图层树**——
   与 §69.128 的定性一致，但论据完全更换：
   §69.128 的 TGT/USTEXT「冻结」证据已在 §69.129.3
   证伪，现在的论据是「GPU 已能写像素却一直只画启动屏」。
4. **仍未回答**：为什么 `backboardd`/`SpringBoard`
   在 GPU 完全健康、可持续 390 s 的情况下一直重绘
   同一启动屏帧。不宣称首帧成功。

### 69.130.6 新发现（可证伪候选）：launchd boot task 队列从未走完

> **已证伪，见 §69.131.1，请勿据此决策。** `Doing boot task` 是
> 异步派发记录，不是「开始且必须结束」；`Finished` 只对
> launchd 同步等待的 task 回写。原计数保留作证据链，
> 「25 个从未完成 ⇒ 启动闸门」的定性结论作废。

四个轮次（fix157 / fix159 / fix113-390 s / fix129-390 s）
逐字一致：

| 事件 | 计数 |
| --- | --- |
| `[Event: X] Doing boot task` | **27** |
| `[Event: X] Finished boot task` | **1**（仅 `fixup-mobile-tmp`） |
| `optional boot task not present` | 1（`restore-datapartition`） |
| **从未 Finished** | **25**，含 `mount-phase-1/2`、`data-protection`、`keybag`、`init-with-data-volume`、`fips`、`usermanagerd`、`tzinit`、`sysstatuscheck` |

- 跨全 logs：`Early boot complete. Continuing system boot.`
  **150 轮**出现（~122 s），`kPMSystemRestartBootingInProgress`
  **151 轮**出现（129–130 s），而 **"System boot complete"
  一次都没出现**；该 PM assertion 也从未见清除。
- 390 s 长窗口里 25 个仍未完成 ⇒ 不是「还没轮到」。
- **但不断言因果**：`Doing/Finished` 可能只是 launchd
  日志级别差异（异步 job 不回写 Finished），需先确认语义
  再谈闸门。这是比 watchdogd CRITEXIT 更稳定、
  更贴近「UI 不切换」的共同状态，列为下一轮首选靶点。

### 69.130.7 下一步（仍为 host 侧零风险）

1. 先确认 `Doing boot task` / `Finished boot task` 的
   语义与回写条件（本地 launchd 源码/补丁、System Event
   日志格式），再判断 25 个未完成是否真是挂起；
   找 `kPMSystemRestartBootingInProgress` 的持有者与清除路径。
2. 若语义成立，查 `SpringBoard` 是否在等
   `PostBoot`/`BootComplete` 之类的 launchd 通知才
   切换到桌面图层树（对照 122 s `Early boot complete`
   之后的 XPC/notify 行）。
3. **不做**：上机、改内核、改 GPU 驱动、zap/secure mode
   （§69.119 已修好）、换表/SMMU/MRT/CRB/UCHE/CCU、
   放宽 hang 阈值、`0x8000`、`boot_log_tick`/`wdt_thread`/
   watchdog ping、`sheng-autoreset` 数值、MACF/sandbox、
   照抄 uke A732 / sheng A740 的 mode0 或 CPU 拷贝路径；
   witness 0xCD、`B0/16`、`W0`、watchdogd CRITEXIT、
   sandbox deny、`b=e=0` **均不再复查**。

## §69.131 boot task / PM 闸门 / SpringBoard 等通知：三条候选全排除

§69.130.7 列的三个待查点，纯 host 侧只读复核（未上机、未改
内核、未碰设备 `a20248f7`）。结论：**§69.130.6 的「launchd
boot task 队列从未走完」是语义误读，作废**；PM 清除路径
是观测盲区；SpringBoard 不是等通知，是高频自旋。

### 69.131.1 `Doing/Finished boot task` 的真实语义 ★

这些行是 Apple launchd 的 `os_log` System Event，格式固定为
`[System Event] [id] [INFO] [Subsystem: launchd] [Event: X]`；
工程源码（`UniXNU/`、`UniXNU-muyu-17/`、`tools/`）里
**没有**这些字串，不是项目 shim 或补丁打的日志。

fix159 的 120.262–121.562 s 完整时序：

| 时刻 | 事件 |
| --- | --- |
| 120.262–121.019 s | 27 条 `Doing boot task`，`exclaves-boot` → `init-exclavekit` |
| 120.355020 s | `restore-datapartition` `optional boot task not present` |
| 120.389905 s | `fixup-mobile-tmp` Doing |
| 120.401393 s | 同 task 打 `already exists, skipping` |
| 120.413909 s | 同 task **Finished**（24 ms，唯一同步等待的任务） |
| **121.276551 s** | `[Event: boot] Early boot complete. Continuing system boot.` |
| 121.561684 s | `SpringBoard[27] ==> (apply-protobox)` 启动 |

- `Doing` = 派发记录，不是「开始且必须结束」。26 个是异步
  fork/exec，launchd 不等回执；紧随其后的 `==>` 服务启动行
  （SpringBoard、keybagd、watchdogd、timed、logd…）就是它们
  落地的证据。
- `Finished` 只对 launchd **同步等待**的 task 回写；
  `fixup-mobile-tmp` 是唯一一条，且 24 ms 内正常完成。
- 跨全 logs：**149 轮每轮恰好 27 个 `Doing`**、
  150 轮每轮 1 次 `Early boot complete`（~121–122 s）、
  **0 轮**在 `Early boot complete` 之后还有 `Doing`。
  ⇒ 27 个 task 是 early boot 阶段的固定清单，全部在该阶段
  结束前派发完毕；这是**正常启动流程**，不是队列卡住。
- 工程侧 `BootWatchdogPolicy.c:18` 与
  `check-launchd-watchdog-policy.py:12` 把
  `com.apple.watchdogd` 标成 disabled；`board.toml:83-85`
  说明其目的是「让 boot-task check-in 缺失不再重置板子」。
  这是为早期 `mount-phase-1 exit(66)`
  （`HANDOFF-2026-11-02.md:962` 的 `nopanic1` 时代）准备的
  兜底，**不能反推**当前 25 个 task 处于阻塞。

⇒ **§69.130.6 作废**：「25 个从未 Finished」= 把异步派发
日志读成阻塞任务；「390 s 仍未完成所以不是还没轮到」也随之
失去前提。原计数保留作证据链，定性结论不再使用。

### 69.131.2 PM assertion：清除路径无观测通道，不可证

`kPMSystemRestartBootingInProgress` 在 fix159 的 130.445102 s
只出现一次，形态是 PM 的一次 idle-sleep 尝试被挡住：

```
130.444912  setAggressiveness(0) kPMMinutesToSleep = 2147483647
130.445020  idle sleep timer enabled / idle timer set for 0 ms
130.445090  sleep timer expired
130.445102  System sleep prevented by kPMSystemRestartBootingInProgress
130.529842  idle sleep timer disabled
```

- 全 logs：**151 轮每轮恰好 1 次** `System sleep prevented by`
  + 1 次 `sleep timer expired`；150 轮每轮 1 次
  `Early boot complete`；`System boot complete` **0 次**。
- 但 `kPMSystemRestartBootingInProgress` 是 IOPM 内部
  assertion，清除时不会往 msgbuf 打日志；`System boot complete`
  属 powerd 用户态日志，本通道本就不含。⇒ **「assertion
  从未清除」不可证**，不能当启动闸门论据。
- 这一对事件是跨全部轮次的固定标配（含 390 s 长窗口），
  不是 fix157+ 特有异常。§69.130.6 的「PM assertion 也从未
  见清除」应读作「无观测通道」，不是「确认未清除」。

### 69.131.3 SpringBoard 不等通知，是高频自旋（证伪）

- SpringBoard 在 `Early boot complete` 后 **0.3 s**
  （121.561684 s）已启动，不需要等 `PostBoot`/`BootComplete`。
- 全 logs 中 SpringBoard 的 `waiting` / `blocked` /
  `BootComplete` 状态串 **0 条**。
- 相反，四轮共性（45,000 wakes / 300 s 超限）：

| 进程 | fix157 | fix159 | fix113(390s) | fix129(390s) |
| --- | --- | --- | --- | --- |
| launchd[1] | 104.9 s / 3643/s | 101.3 s / 5062/s | 101.2 s / 4764/s | 103.1 s / 4108/s |
| SpringBoard[27] | 132.8 s / 4324/s | 136.1 s / 3115/s | 137.1 s / 3028/s | 135.1 s / 3231/s |
| backboardd[58] | 149.7 s / 1663/s | 137.5 s / 2870/s | — | 221.2 s / 454/s |
| lsd[99] | — | — | — | 205.2 s / 569/s |

390 s 窗口里 205 s、221 s 仍继续超限 ⇒ 不是启动瞬态，
是**持续到窗口末端的高频自旋 / 忙等**。与 §69.128
「135–175 s 每 5 s 仍 230–290 行 MUYU、持续重绘同一帧」
及 §69.130「GPU 已能写像素却一直只画启动屏」同形状。

### 69.131.4 顺带排除两个新候选（零成本）

- **Data 卷满导致 UI 反复重试**：全 logs **0 轮**出现
  `ENOSPC` / `No space left`；`ustate: caching container path`
  是正常容器路径缓存；`magazine write failed; may be expected: 30`
  （41 s，Image4）与 `apfs_find_named_root_snapshot_xid ...
  No such file or directory`（90 s）都在启动早期且自带
  "may be expected"。无证据。
- **IOMFB 临时 surface 反复建销**：整轮 IOMFB 日志只有
  `driver commit`（90.252532 s）与
  `surface_complete : Freeing up Default Temp surface`
  （136.905768 s，每轮 1 次，跨 fix100–fix159 恒定），
  无创建/释放循环。无证据。

### 69.131.5 修正后的瓶颈层级（第四次收敛 = 三条闸门全排除）

boot task 语义、PM assertion、SpringBoard 等通知三条
**都不是瓶颈**。加上 §69.130 已确认的「GPU 全链路健康、
secure mode 已退出、0xCD 双边落地、判据活跃到 175 s、
时间不是瓶颈（389–390 s 长窗口同样不进桌面）」，剩下的
仍是**用户态合成内容未推进到桌面图层树**；本轮新增的
行为特征是它表现为 3000–5000 wakes/s 的忙等自旋，
而非阻塞在通知/闸门上。*为什么* 仍未回答。
桌面/图标仍无，**不宣称首帧成功**。

### 69.131.6 下一步（仍为 host 侧零风险）

1. 定位自旋来源：统计 133 s 后 `FBC/SCAN/CAN/USHB` 的
   相邻间隔分布，看是否有 ~1 kHz 的固定节奏；查
   `NativeBridgeMetalFrontend.mm` 是否存在高频轮询循环
   （`IOSleep` / `sched_yield` / `while` 等）。
2. 查 `CRITEXIT`（141–144 s）之后
   `p_crash_behavior_deadline` 是否让 SpringBoard 进入
   `HANDOFF-2026-11-02.md` §4.1.3 描述的「进度条不动的
   静默挂起」。
3. 查 133–175 s `ustate` / `lsd` / `iconservicesagent`
   的容器枚举是否在反复重跑（对照 141.6 s 的大批
   PluginKit 路径缓存）。
4. **不做**：上机、改内核、改 GPU 驱动、zap/secure mode、
   换表/SMMU/MRT/CRB/UCHE/CCU、放宽 hang 阈值、`0x8000`、
   `boot_log_tick`/`wdt_thread`/watchdog ping、
   `sheng-autoreset` 数值、MACF/sandbox、照抄 uke A732 /
   sheng A740 的 mode0 或 CPU 拷贝路径；
   `Doing/Finished boot task`、`kPMSystemRestartBootingInProgress`、
   `System boot complete`、ENOSPC、Default Temp surface
   **均不再复查**。

## §69.132 用户线索：`sheng-lite=1` 全禁 21 个守护进程，158 轮从未对照

> **更正（2026-11-15 20:15，见 §69.133）★ 本节开头的定性已被上机
> 实验证伪，请勿据此决策**：「停用机制至今仍在生效」与「唯一一个
> 从未做过对照的启动变量」都只在 **boot-arg 字符串存在** 与
> `board.toml` 注释层面成立，没有验证调用点。§69.133 证明当前
> kernel 的 `XNU_BOOT_PROGRESS=0`，`zzz-boot-progress.patch:214`
> 的 `#if` 把 `sheng_boot_watchdog_policy()` 调用整个编译掉了：
> **21 项禁用从未写入 `disabled.plist`，`sheng-lite=1` 是死参数**。
> 本节以下清单、计数与历史记录仍然有效（是配置事实），但
> 「删除它能改变运行时」的推论作废。原证据数字全部保留。

用户 2026-11-15 19:48 提示「项目早期，为了能成功启动系统，
曾停用多个守护进程」。纯 host 侧只读核对确认：**停用机制至今
仍在生效**，而且是本项目**唯一一个从未做过对照的启动变量**。
本轮未上机、未改内核、未碰设备 `a20248f7`。

### 69.132.1 停用清单与配置载体 ★

- `board.toml:502` `"sheng-lite=1"` 未注释；当前
  `payload.json`（1014 B）、fix157/fix159 镜像 cmdline、
  **全部 158 个 `fw/vendor_boot_gpu_fix*.img` 无一例外**都带。
- 由 `package/BootWatchdogPolicy.c` 的 `lite_jobs`（**21 项**，
  与 `board.toml:506` 的 `0x1fffff = all 21` 一致；C 字符串
  换行拼接会产生 4 个空段，勿误数为 25）写入
  `/private/var/db/com.apple.xpc.launchd/disabled.plist`，
  并建 `disabled.migrated` marker。另有
  `"sheng-disable-watchdogd=1"`（`board.toml:405`）追加
  `com.apple.watchdogd` + `com.apple.watchdog` 两个 key。

| 位 | 组 | 标签 |
| --- | --- | --- |
| 0–3 | 账户/定位/无线 | `identityservicesd`、`locationd`、`CommCenter`、`bluetoothd` |
| 4–11 | 云同步/媒体分析 | `photoanalysisd`、`mediaanalysisd`、`mstreamd`、`searchpartyd`、`findmybeaconingd`、`familycircled`、`financed`、`softwareupdateservicesd` |
| 12–17 | 其它后台 | `ndoagent`、`biomesyncd`、`maild`、`amsengagementd`、`adid`、`eyereliefd` |
| 18–20 | crash/respawn 环 | `fairplayd.A2`、`passd`、`mobile.obliteration` |

- `sheng-data-persist=2` ⇒ Data 卷持久。`fw/data-seed.img`
  （1.0 MiB）里 **0 次**出现 `disabled.plist` / 任一 job 标签
  ⇒ 该 plist 不是出厂写好的，是内核在每次带 policy 参数的
  开机时运行时写入的。
- **关键统计**：158 个镜像中 `lite=True` 158 轮，
  `lite-reset` / `lite-mask` / `disable-jobs` **各 0 轮**。
  ⇒ 至今所有「窗口长度 / 时间不是瓶颈 / GPU 全链路健康」
  的结论，全都建立在**同一份全禁清单**之上，
  「启用守护进程」这个变量从未被检验过。

### 69.132.2 当年为什么停用（配置注释原文）

- `board.toml:494-495`（2026-10-27 pm）：启动在 ~75 s
  **停止创建进程**，`mediaanalysisd/photolibraryd/
  photoanalysisd/softwareupdated` 是最后一批，且
  「no boot task ever completed past them」→ 禁用该批
  才让启动继续。这正是用户所述「为了能成功启动」。
- `board.toml:514-516`（2026-11-12）：
  `identityservicesd`/`locationd` 曾在**同步 XPC 等待环**
  里导致启动卡死；恢复后若卡在 ~75 s，先回退
  `sheng-lite=1`（全禁、已知能启动），再用
  `sheng-lite-mask` 按位二分，只保留真正卡死的那个。
- `GPU-HANDOFF.md:2836`（11-03 27 线验收链）：
  「从 `lite_jobs` 移除 IconServices 禁用」；源码注释亦明确
  「禁用 `iconservicesagent` 会让图标缺失（硬件上见过），
  所以保持启用」⇒ 项目**已知道**停用项会直接影响 UI 内容。
- `board.toml:533`（10-26 22:50）：official skip-setup path
  连 boot logo 都没到，已弃用；`sheng-setup=1` /
  `sheng-data-reset=2`（395 行）当前均注释。

### 69.132.3 与「UI 不进桌面」的关联（可证伪，不断言因果）

- bit0 `identityservicesd` 被禁，而 130.030021 s 启动链里
  有 `akd[116]` / `securityd[117]` / `mobileactivationd[118]`；
  全 logs 出现 `activation-lockedSandbox:` **111 次**、
  `activationd` 相关 300+ 次；150.984884 s
  `findmydeviced(119) deny ... fm-spstatus`。
- **候选假设**：账户/激活 XPC 链因被禁 daemon 不完整，
  SpringBoard 停在启动/激活屏；与「不禁用则 ~75 s 卡死」
  构成两难。但日志中**没有** `waiting for activation` 类直接
  证据，`purplebuddy` 相关 0 命中 ⇒ **仅列候选，不断言因果**。
- 与 §69.131.3 的 3000–5000 wakes/s 自旋也可能同源
  （XPC 查找失败后忙重试），同样未证实。

### 69.132.4 配置注释与源码逻辑的一处不一致（影响实验设计）

`board.toml:508-510` 注释称「单删 `sheng-lite=1` 不会恢复
它们，陈旧 `disabled.plist` 残留继续生效」。但
`BootWatchdogPolicy.c:sheng_boot_watchdog_policy()` 的提前
返回条件是：

```
if(!enabled && !jobs[0] && !lite && !have_mask && !lite_reset)return 0;
```

`enabled` 来自 `sheng-disable-watchdogd=1`（**当前 cmdline 一直在**）。
⇒ 当前配置下单删 `sheng-lite=1`，函数**不会**提前返回，
会把 plist 重写成仅含 `watchdogd`/`watchdog` 两个 key，
**21 个 job 反而被清空**，且 watchdogd 保持禁用（正好避免
boot-task check-in 重置）。注释大概针对「移除所有 policy
参数」的情形，与当前 cmdline 不符。**以源码为准**，
但不据此单独上机，实验前必须明确选路径。

### 69.132.5 §69.131.6 的 ①②③ 结案

| 候选 | 结果 |
| --- | --- |
| ① ~1 kHz 固定节奏 | **证伪**：fix159 中 FBC 35 条间隔中位数 795 ms、SCAN 70 条 205 ms、CAN 36 条 693 ms、USHB ~9 s；`NativeBridgeMetalFrontend.mm` 仅 `usleep(250000)`（4 Hz，`:398`）与内存压力 `dispatch_source`（`:1034`），无高频轮询循环 |
| ② `p_crash_behavior_deadline` 静默挂起 | msgbuf 内该符号 **0 命中**，无观测通道；但发现激活链（69.132.3） |
| ③ 容器枚举反复重跑 | **证伪**：857 个不同 key，仅 2 个重复（各 2 次）；<133 s 214 条 / ≥133 s 645 条，是一次性枚举完而非循环 |

⇒ 3000–5000 wakes/s 的自旋源仍未定位，但**不在** GPU 判据
节奏、不在 Metal 前端显式循环、不在容器枚举重跑。

### 69.132.6 下一步（**需用户确认才上机**）

这是目前唯一能直接检验「用户态合成内容不推进」的单变量，
且项目已备好工具。三条路径互斥：

1. **A（最小改动，推荐）**：删 `sheng-lite=1`，保留
   `sheng-disable-watchdogd=1` → 21 项清空、watchdogd 仍禁；
   按 §69.20.2 三步链重建 payload/vendor_boot，标准闭环。
2. **B（语义最明确、风险最高）**：加 `sheng-lite-reset=1`
   写空 override 字典，连 watchdogd 一起全开——当年禁用它
   正是为避免 boot-task check-in 重置板子。
3. **C（二分）**：`sheng-lite-mask=<hex>` 按位只恢复部分
   （bit0=`identityservicesd`），一次只改一个数，
   成本仅一次 payload 构建。

硬判据：桌面/图标是否出现、`TGT` 是否新增目标、启动是否
卡回 ~75 s、`==>` 服务派发是否继续越过 142 s、
`HANGJ/POIS` 与自动复位是否正常。

**风险**：恢复 `identityservicesd`/`locationd` 可能重现
~75 s 同步 XPC 等待卡死；按 `board.toml:515` 回退
`sheng-lite=1` 即可恢复已知可启动状态。

**不做**：不动 GPU 驱动/换表/SMMU/zap（§69.119 已修好）、
不改 `boot_log_tick`/`wdt_thread`/watchdog ping、
不调 `sheng-autoreset` 数值、不放宽任何判据、
不照抄 uke A732 / sheng A740 的 mode0 或 CPU 拷贝路径。

---

## §69.133 fix160 — 删 `sheng-lite=1` 上机对照（阴性，死参数结案）

用户 2026-11-15 19:54 明确要求「这个禁用进程的问题反正是
一定要解决的」，并补充历史现象：**CPU 渲染时代因停用太多
守护进程，仅三个系统应用能正常启动，连「设置」都起不来**。
本轮按 §69.132 候选 A 上机：单删 `sheng-lite=1`、保留
`sheng-disable-watchdogd=1`。目标是把 21 项禁用清空并检验
系统应用/桌面是否推进。

### 69.133.1 单变量与离线校验

- `board.toml:502` 的 `"sheng-lite=1",` 改为注释并加 fix160
  说明；其余逐字同 fix157/158 基线（源码已回退 fix159）。
- `payload.json` cmdline **1014 → 1001 B**，
  `sheng-lite=1` 消失、`sheng-disable-watchdogd=1` 仍在，
  无 `lite-reset` / `lite-mask`；trust/addon（fix107）、
  载体 `swCD`、`sheng-autoreset=300`、全部像素判据与诊断串原样。
- `cmp` fix157↔fix160：666 字节 / 98 簇，其中 cmdline 区域
  唯一语义差异是删掉 `sheng-lite=1 ` 共 13 B；其余为每次构建
  都变的 `chosen/random-seed`、`_PrelinkKCID` 与校验和。
  `[log] tick` 在 fix160 为 0（源码回退基线），故正确对照是
  fix157/158 而非 fix159。
- 三步链产出 `fw/vendor_boot_gpu_fix160.img`
  sha `82e05f8e…`，kernel `eb116e6d…`，编译 0 error。

### 69.133.2 上机结果：窗口正常，但一切与 fix159 逐字相同

`tools/muyu-autoloop.sh ... 900` → **203 s 自动复位 /
178 s 内核窗口 / 44 轮**，设备自动还原原厂回 recovery，
**没有卡回 ~75 s**，无需人工干预。日志
`logs/vendor_boot_gpu_fix160-logs/muyu-675503ea`。

| 判据 | fix157 | fix159 | **fix160** |
| --- | --- | --- | --- |
| 自复位 / 窗口 | 202/175 s | 人工/175 s | **203/178 s** |
| `==>` / 唯一进程 | 194 / 184 | 195 / 184 | **194 / 184** |
| `.app` 数 / `Preferences.app` | 5 / 0 | 5 / 0 | **5 / 0** |
| 21 项 label 的 `==>` 计数 | 逐项同 | 逐项同 | **逐项完全一致** |
| FBC / SCAN / CAN | 29/59/30 | 36/72/36 | **33/71/35** |
| HANGJ / recovery / POIS | 2/2/0 | 2/2/0 | **3/3/0** |
| 桌面 / 图标 / 设置 | 无 | 无 | **无** |

GPU 仍健康（`ist=0 hang=0 stallfault=0`、`SEC trust=0`、
`POIS=0`，判据延续到 177.8 s）；`TGT` 仍 4 项、
`USTEXT P` `n1→57` 恒定在 `w3200x2136 d3 m2 f26 … fi748`、
USHB 到 174.6 s。**设置/桌面依旧没有，不宣称首帧成功。**

### 69.133.3 根因：`XNU_BOOT_PROGRESS=0`，policy 从未被调用 ★

> **2026-11-15 20:2x 更正（详见 §69.134.1）**：结论「policy 从未被调用」
> 成立，但宏名写错了。生成树调用点受 `XNU_WATCHDOG_POLICY` 控制，
> 不是 `XNU_BOOT_PROGRESS`；后者为 0 与该调用点无关。本节第 1 条和
> 「阶段字符串 count=0 ⇒ 调用点被编译掉」的推论请改按 §69.134.1 理解
> （字符串本来就被 patch 删掉了，count=0 不能单独当判据）。

`sheng-lite=1` 不是「生效但没对照」，而是**空转参数**：

1. 调用点 `UniXNU/patches/xnu/zzz-boot-progress.patch:214-220`
   是 `#if defined(XNU_BOOT_PROGRESS) && XNU_BOOT_PROGRESS`
   包住的 `sheng_boot_watchdog_policy(p, scratch_addr)`；
   `build-xnu-4k-baseline.sh:828` 用
   `-DXNU_BOOT_PROGRESS=$BOOT_PROGRESS`、默认 `0`。
2. `nm` 显示 `_sheng_boot_watchdog_policy` 与全部诊断串
   **确实编进了 kernel.macho**（源文件被链接），但阶段字符串
   `temporary watchdog launch policy enter` / `load_init_program enter`
   在 fix157/159/160 三个镜像里 **count=0** ⇒ 调用点被编译掉。
3. 全 157 轮日志 `sheng bring-up watchdogd launch policy` 与
   阶段标记 **0 命中**（msgbuf 从 40.1 s 起就有行，覆盖该时段）。
4. `disabled.plist` 从未被写入：recovery 下 `sda32` f2fs
   挂载失败（Invalid argument），改用只读 `dd` 扫 0–1 GiB，
   `com.apple.identityservicesd` 等命中**全是各 daemon 自己的
   `.plist` 文件名/目录项**；密集 `<true/>` 块是 Quartz
   Composer 配置（`assetManifest`/`multitouchEnabled`），
   **找不到 21 项 `<key>…<true/>` 禁用字典**。

因此 fix160 与 fix159 逐项完全一致是必然的：两轮都没有
写 plist、也都没有可清的陈旧残留。`board.toml:508-510`
「单删本行不会恢复、陈旧文件残留」的注释针对的是
**带 policy 调用的旧配置**，对当前 kernel 不适用。

### 69.133.4 与用户历史线索的关系（不过度推论）

用户记得的「仅三个系统应用能启动、设置不能启动」属于
**CPU 渲染时代**：那时 `XNU_BOOT_PROGRESS=1`（或另一套
kernel 配置），policy 调用生效、`disabled.plist` 会被写入，
`GPU-HANDOFF.md:2836`（11-03）「从 `lite_jobs` 移除
IconServices 禁用」正是那种环境下的现场观察。当前 GPU
时代镜像里这条链路已关闭，**不能把历史因果套到 fix157+**。
本轮既不证实也不否定用户当年的观察，只钉死当前状态。

### 69.133.5 结论与状态

1. **配置已清理**：`sheng-lite=1` 从 `board.toml` 删除并保留
   （死参数不再误导后续冷启动）；若将来重开 policy 通道，
   此配置会写出「仅 2 个 watchdogd key」的 plist，21 项全开。
2. **「清空禁用清单」对当前运行时无可观测影响**，
   设置/桌面不推进**另有原因**，与 21 项禁用无关。
   §69.131 的 3000–5000 wakes/s 忙等自旋仍是最近的行为特征。

> **2026-11-15 21:5x 更正（详见 §69.141）**：本条「与 21 项禁用
> 无关」**过强，作废**。fix160 删 `sheng-lite=1` 时 policy 调用点
> 已被编译掉，删参数等于什么都没做 —— fix159 与 fix160 **都是
> 「21 项全开」**，该对照从未检验过「停 21 项 vs 全开」。
> fix161/162/163 全部继承同一全开状态。CPU 渲染时代能进桌面
> 的两份镜像（`vendor_boot_lite.img`、`vendor_boot_nofs1.img`）
> 都带 `sheng-lite=1` 且老树有 `-DXNU_WATCHDOG_POLICY=1`；
> 换共享树后该宏丢失 ⇒ **「重新停掉那 21 项能否恢复桌面」是
> 当前最大的未测变量**，不能写成已排除。此处只保留
> 「清空清单无可观测影响」这一事实描述。
3. 不回退 fix160 改动：已验证启动正常、自动复位正常、
   所有 GPU 判据与原厂一致；镜像/日志冻结供复现。
4. **不做**：不改 `XNU_BOOT_PROGRESS` 宏或 kernel 调用点
   （行为改动、会引入整个 boot progress 层，需单独确认）、
   不擅自跑 `sheng-lite-reset=1`（policy 不调用时同样是空操作）、
   不动 GPU 驱动/表/SMMU/zap、不改 tick/watchdog、
   不调 `sheng-autoreset`、不放宽判据、不照抄 uke/sheng。

### 69.133.6 下一步（零风险优先）

① 继续追 SpringBoard/launchd 的 3000–5000 wakes/s 忙等自旋
来源（§69.131），解释「容器枚举了 179 个 bundle id 却只有
5 个 `.app` 进程启动」；② 查 `Preferences.app` 从未被派发的
直接证据（launchd job 请求 / SpringBoard 图标树）；
③ 若用户坚持要彻底消除「陈旧 plist 残留」的最后一丝可能，
再单独确认是否重开 `XNU_BOOT_PROGRESS=1` + `sheng-lite-reset=1`
上一轮——这是行为改动，本轮未做。

> 2026-11-15 20:2x：上面 ③ 的宏名同样有误，且 ②/③ 都已由
> §69.134 的零风险复核结案，不必再单独上机。

---

## §69.134 零风险复核 — 「禁用进程」结案与真正缺口（staged system apps）

用户 19:54 强调「禁用进程的问题反正是一定要解决的」。§69.133
把 `sheng-lite=1` 判为死参数后，本轮不刷机、不改内核，纯 host
侧重查三条：调用点真实宏、**运行时是否真有禁用生效**、以及
「仅少数系统应用可用」是否另有机制。结论：**用户诉求在当前 GPU
镜像里已经不成立，但桌面/图标/设置缺失另有结构性缺口。**

### 69.134.1 更正 §69.133.3：宏名是 `XNU_WATCHDOG_POLICY`，不是 `XNU_BOOT_PROGRESS`

实际构建用生成树 `/Users/diannao/pad7pro-xnu/UniXNU/out/muyu/tree`。
其 `.cache/xnu-work/bsd/kern/kern_exec.c:8255` 是：

```c
#if defined(XNU_WATCHDOG_POLICY) && XNU_WATCHDOG_POLICY
 extern int sheng_boot_watchdog_policy(void *, uint64_t);
 int policy_error=sheng_boot_watchdog_policy(p, (uint64_t)scratch_addr);
 if (policy_error) printf("sheng watchdog launch policy: %d\n", policy_error);
#endif
```

1. `UniXNU-muyu-17/patches/xnu/zzzzzzzz-braveark-watchdog-policy.patch`
   把 `UniXNU/patches/xnu/zzz-boot-progress.patch` 的调用点宏从
   `XNU_BOOT_PROGRESS` 改成 `XNU_WATCHDOG_POLICY`，并**删掉**
   `boot_progress_stage("temporary watchdog launch policy enter/ready")`，
   改成仅失败时 printf。两个 patch 都在，生成树里是改后的版本。
2. 共享构建脚本 `UniXNU/scripts/build-xnu-4k-baseline.sh:860`
   只传 `-DXNU_BOOT_PROGRESS=$BOOT_PROGRESS`（默认 0），
   **从不定义** `XNU_WATCHDOG_POLICY`。
   `configs/muyu-userland_defconfig:67` 的
   `CONFIG_XNU_WATCHDOG_POLICY=y` 在整个构建链里**零消费点**
   （全项目 grep `XNU_WATCHDOG_POLICY` 在 `UniXNU/scripts|tools`
   下无命中）⇒ 该 `#if` 为假，调用点被编译掉。
3. 三重交叉证据（`kernel.macho` sha `eb116e6d…`、未 strip
   `mach.development.bcm2837.unstripped.noctf`）：
   - `nm` 有 `_sheng_boot_watchdog_policy` / `_load_init_program`
     / `_boot_progress_stage`；
   - `sheng bring-up watchdogd launch policy` = **1**（函数体链接进来了）；
   - `sheng watchdog launch policy: %d` = **0**（调用点编掉了）；
   - `temporary watchdog launch policy enter` = **0**，但这是 patch
     删掉的，**不能**像 §69.133.3 那样反推调用点。
   - fix157/159/160 msgbuf 中 `launch policy` / `disabled.plist` 均 0 命中。

结论（policy 从未调用、`disabled.plist` 从未写入）**不变**，但根因是
**搬运 patch 时配置与实现脱节**：函数体编进 kernel 造成「符号还在」
的假象，`XNU_BOOT_PROGRESS=0` 只是恰好同向，不是原因。

### 69.134.2 当前运行时没有任何禁用生效（直接反证，非推断）

fix160 `msgbuf.txt` 的 `==>` 启动行里，带 pid 的 `.app` 进程只有四个：

| 进程 | pid | 与 21 项清单的关系 |
| --- | --- | --- |
| `SpringBoard.app/SpringBoard` | 27 | 不在清单 |
| `IDS.framework/identityservicesd.app/identityservicesd` | **55** | **清单第 1 项、当初 XPC 等待环元凶** |
| `IMCore.framework/imagent.app/imagent` | 64 | 不在清单 |
| `SocialLayer.framework/sociallayerd.app/sociallayerd` | 172 | 不在清单 |

- `identityservicesd[55]` 若 `disabled.plist` 生效（无论本次写入还是
  陈旧残留），绝不会 launch。`watchdogd[53]` 同样出现在 `==>` 行，
  而 `sheng-disable-watchdogd=1` 本应禁掉它 —— **两个独立反证**。
- 21 个 `com.apple.*` label 在 msgbuf 里 16 个 0 次、5 个各 1 次；
  这 5 次全在 `ustate: caching container path` 行（容器路径缓存），
  **没有一条**落在 `==>` 启动行。
- 所以 §69.132 / `docs/LAUNCHD-DISABLED.md` 的「陈旧 plist 残留
  继续生效」假设对 fix157+ **不成立**；§69.133.6 ③ 的
  `XNU_BOOT_PROGRESS=1` + `sheng-lite-reset=1` 一轮已无必要。
- 用户记得的「CPU 渲染时代仅三个系统应用能启动、设置打不开」
  **不否定**，但它不是当前桌面/图标/设置缺失的原因。

### 69.134.3 历史现象为何成立：换树把 policy 通道整个弄丢了

- `UniXNU-sheng` / `UniXNU-muyu` / `UniXNU-braveark` 的
  `scripts/legacy-10063/build-xnu-4k-baseline.sh:831` 确有
  `-DXNU_WATCHDOG_POLICY=$WATCHDOG_POLICY`，其 defconfig 有
  `CONFIG_XNU_WATCHDOG_POLICY=y`，同名 patch 也用该宏 ⇒ 那时 policy
  **调用生效**、`disabled.plist` 每次开机写 21 项。
- muyu-17 改用共享 `UniXNU` 树后，该宏没有接上：调用点被 `#if`
  砍掉，函数体仍链接。这同时解释了 §69.132 的怪现象——
  **158 个历史镜像全带 `sheng-lite=1` 却从无对照差异**，以及
  fix160 单删它完全阴性。
- 因此「解决禁用进程」不是去 reset/mask 清单，而是：
  **`sheng-lite=1` 已删（fix160），且若将来重开
  `-DXNU_WATCHDOG_POLICY=1`，policy 因 `sheng-disable-watchdogd=1`
  仍在而只会写「仅 2 个 watchdogd key」的 plist，21 项全开**
  ——正是用户要的结果。当前宏下 `sheng-lite-reset=1` 也是空操作。

### 69.134.4 真正对应「系统应用起不来」的缺口：`sheng-staged-apps=1` 从未启用

`UniXNU/patches/xnu/zzy-staged-system-apps.patch`（已应用，
`profiles/arch/arm64/patches/xnu/series:54`）说明另一套机制：

> 可删除系统 App（Weather/Notes/Calendar…）在 **OS 卷**
> `/private/var/staged_system_apps`；首次开机需 boot arg
> `sheng-staged-apps=1` 把它拷进 Data 卷，再由 `SystemAppMigrator`
> 安装。**在此之前只有 `/System/Library/AppPlaceholders` 占位符，
> 那些 App 的小组件是空白的。**

- 当前 `board.toml` 全文 **0 处** `sheng-staged-apps`；fix160
  `payload.json` 的 `kernel_args` = **1001 B**，其中没有该参数。
  全项目只有 `UniXNU-sheng/board.toml:188` 启用过它；
  `logs/u88–u139-worktree.diff` 里的命中全是 patch 注释文本，
  **不是 cmdline**（精确搜 `"sheng-staged-apps=1"` 零命中）。
- `board.toml:686-687` 注释写「first-boot data migration
  (datamigrator) then runs and the staged system apps install」，
  漏了前提：`sheng-data-persist=2` 只让 Data 卷持久
  （`native_owner_data_prepare()` 挂 `/dev/md0` 的 persistent
  sparse disk），**并不复制 OS 卷里的 App**。
- fix160 日志佐证：`staged_system_apps` / `AppPlaceholders` /
  `SystemAppMigrator` / `.sheng-staged-apps` 全部 **0 命中**；
  `com.apple.datamigrator[132]` 与
  `com.apple.migrationpluginwrapper[171]` **确实启动**（migration
  在跑，但源目录为空）；同一时段反复出现
  `NSG gpu ... text=backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard`
  （§69.131 忙等自旋）。
- `Preferences.app` / `Settings.app` 在日志中 **0 次**；2 次
  `Preferences` 命中分别是 `OSAnalytics/Preferences` 路径与
  `IntlPreferences.framework`，与设置 App 无关。
- **[推断，待上机]** 图标树/可删除 App bundle 缺失，比 launchd
  禁用清单更接近「容器枚举 179 个 bundle id 却只有 5 个 `.app`
  进程」与「桌面一直是蓝色启动屏」的现场。**不宣称**它就是
  Settings 打不开的唯一原因：`Preferences.app` 属 OS 卷核心 App，
  不依赖 staged 目录，仍需单独查其 launchd 派发。

> **2026-11-15 20:4x 更正（§69.135.4）：本段推断已削弱。**
> 离线提取 fix160 的 799 个唯一 bundle id 后发现 `mobilesafari` /
> `Music` / `mobilemail` / `weather` / `mobilenotes` / `Maps` /
> `AppStore` / `MobileSMS` / `facetime` / `Health` **全部在册**，
> 说明 staged apps 未复制**并没有清空 App 注册表**；唯独
> `com.apple.Preferences` 特异性缺席。因此「加
> `sheng-staged-apps=1` 就能恢复图标/设置」不再是首要候选，
> §69.134.5 的下一步 ① 相应降级。

### 69.134.5 状态、边界与下一步

1. **用户诉求的正面答复**：当前 GPU 镜像里没有 launchd 禁用生效，
   「因禁用太多进程导致设置打不开」这条因果**不适用于 fix157+**；
   `sheng-lite=1` 已从 `board.toml` 删除并保留，无需再为它做
   reset / mask 实验。真正待解的是 §69.134.4 的 App 缺口与
   §69.131 的自旋。
2. **本轮零风险，未刷机、未改内核、未改任何 boot arg**。
   三源码文件仍等于 fix158 基线；设备 `a20248f7` 在 recovery，
   原厂 vendor_boot 已还原。
3. ~~**下一步 ①（推荐，行为改动，需用户确认）**：在 `kernel_args`
   加 `sheng-staged-apps=1` 重建 payload 并上机一轮~~
   **2026-11-15 20:4x 降级**：§69.135.4 证明 App 注册表并未
   因 staged apps 未复制而清空，该一轮不再作为首要候选；
   若仍要做，约束不变（cmdline 1001 B 已贴约 1000 B 上限、
   需腾 ~20 B；首次复制数百 MiB 到持久 Data 卷，patch 注释
   记录了 int-220 脏页 stall 与 int-221 递归爆栈，需长窗口）。
   改以 §69.135.5 的零风险方向优先。
   **2026-11-15 20:5x 再更正（§69.136）**：该降级所依据的
   「799 个 id = App 注册表」前提不成立（实为容器路径缓存）；
   root 卷实测 29 个 staged bundle 齐全、其中 28 个被默认图标
   布局引用，且布局里明确有 `com.apple.Preferences`。此项
   **恢复为候选 B**，目标改为验证图标树/Setup 是否推进；
   优先级仍低于 §69.136.6 的候选 A（换 skipsetup seed）。
4. **下一步 ②（零风险，可继续）**：离线检查 Data seed / root
   image 里 `staged_system_apps` 的实际内容与体积，核对
   `SystemAppMigrator` 触发条件，评估 ① 是否值得。
5. **不做**：不改 `XNU_WATCHDOG_POLICY` 宏或调用点（会引入整条
   policy 通道，需单独确认）、不改 boot_log_tick / watchdog /
   `sheng-autoreset`、不动 GPU 驱动/表/SMMU/zap、不放宽判据、
   不照抄 uke/sheng 的 CPU 拷贝路径。

### 69.134.6 本轮文档更正

- 本节；§69.133.3 顶部更正块；§69.133.6 末尾更正块。
- `docs/LAUNCHD-DISABLED.md` 顶部加更正块（其 §0–§5 的前提
  「当前镜像每次开机写 21 项 plist」对 fix157+ 不成立）。
- `board.toml:505-510` fix160 注释更正：原文断言
  「`BootWatchdogPolicy.c` does NOT take the early return and
  rewrites `disabled.plist` on every boot」，因 policy 从未被调用
  而不成立；仅保留「删掉 `sheng-lite=1`」这一已验证动作。

---

## §69.135 零风险复核（二）— Data seed 离线解码与 bundle id 特异性

继续 §69.134.5 的零风险方向：不刷机、不改内核、不动任何 boot
arg。用 `packed_ramdisk.regions()` / `unpack-data-seed.py`
离线解出 fix160 的真实 Data seed 并对照四个变体；同时提取
日志中的 bundle id 全集检验 §69.134.4 的推断。**结果是又一次
自我证伪：staged apps 假设大幅削弱，问题收敛为「设置被单独
排除」+「Setup/migration 未被跳过」。**

### 69.135.1 fix160 实际用的是 base seed，不是 skipsetup 变体

- `payload.json` 的 `data_ramdisk_sha256` = `476464c3…`
  = `fw/data-seed.img` = `fw/data-seed/muyu-data-200g.dbr`。
- `scripts/unpack-data-seed.py` 从 `payload.img` 的 FAT32 卷
  （`FAT32 at 0x0`）提出 `SCRATC~1.IMG` **1,093,760 B**，
  sha256 逐字一致；根目录里**只有这一个 Data seed**，
  `data-skip-setup-476464c34955.dbr`（sha `539d7d…`）只是
  payload 目录下的产物，**没有进 FAT 卷、没有被这次开机使用**。
- `payload.json` 的 **`skip_setup = False`**。
- FAT 卷其它条目：`KERNEL~1.MAC` 53,284,864 B、
  `KERNEL~1.GPU` 12,140,552 B、`KERNEL~1.TRU` 61,456 B、
  `KERNEL~1.ADT` 13,992 B、`NGPUADD2.` 420 B。

### 69.135.2 四个 Data seed 变体的离线明文扫描

`packed_ramdisk.regions()` 解出 200.0 GiB 逻辑卷 / 6 个 extent /
1,093,632–1,327,104 B 非零数据，直接扫明文：

| 变体 | `SetupDone` | `com.apple.springboard` | `com.apple.migration` | `purplebuddy` | `disabled.plist` | `com.apple.Preferences` | `staged_system_apps` |
| --- | --- | --- | --- | --- | --- | --- | --- |
| **base（fix160 实际用）** | 0 | 0 | 0 | 0 | **0** | 0 | 0 |
| skipsetup（payload 目录） | 1 | 4 | 3 | 5 | 0 | 0 | 0 |
| fw skipsetup | 1 | 4 | 3 | 5 | 0 | 0 | 0 |
| minmig | 0 | 0 | 3 | 0 | 0 | 0 | 0 |

- **四个变体全都没有** `disabled.plist`、21 项 `<true/>` 字典、
  `staged_system_apps`、`AppPlaceholders`、`com.apple.Preferences`。
  base seed 里 `<key>` 509 次但 `<true/>` **0 次**（前者是
  APFS 目录项/偏好域文件名）。
- `com.apple.xpc.launchd` 的 4 次命中是 **APFS 目录项名**
  （上下文并列 `mDNSResponder` / `sysdiagnose` / `krmd` /
  `krms` 等 daemon 名），不是 plist 内容 —— 目录存在，
  禁用文件不存在。
- base seed 里只有 `bplist00` 的 MobileAsset / SoundScapes /
  AppleLocale 等预置，以及一条
  `placeholder file to force creation of /private/var/OOPJit`。
  ⇒ 禁用清单**不在任何基础 Data 镜像里**。

### 69.135.3 persistent window 位置更正（不影响既有结论）

- `RamBlock/PersistOverlay.h`：window = 1 个 header block
  + `bitmapBlocks(pages)` + `pages`；`NativeRamBlock.cpp:91-99`
  用 `pages=(big/sparse.bytes)/4096` 调
  `native_ufs_store_open(persist::windowBlocks(pages), &first, …)`，
  `first` 是 userdata 上的起始 LBA。
- `board.toml:386` / `:695` 明写 window 在
  **「the end of userdata」**。
- 因此 §69.133.3 用只读 `dd` 扫 **userdata 0–1 GiB（开头）**
  找残留 `disabled.plist`，**区间不对**；那段「找不到 21 项
  禁用字典」不能作为独立证据。
- **但 §69.133 / §69.134 的结论不受影响**：§69.134.1 的
  生成树宏证据、kernel 字符串三重比对，以及
  `identityservicesd[55]` / `watchdogd[53]` 的启动反证都独立
  成立；本节又补上「四个 seed 变体均无 plist」。此处只是把
  证据强度重新标正，不翻案。

### 69.135.4 ★ 799 个 bundle id：设置是特异性缺席，不是全局缺失

从 fix160 `ustate: caching container path` 行提取出
**799 个唯一 bundle id**（60 行，每行多 id）：

- 核心/可删除系统 App **全部在册**：
  `com.apple.mobilesafari`、`Music`、`mobilemail`、`weather`、
  `mobilenotes`、`Maps`、`AppStore`、`MobileSMS`、`facetime`、
  `Health`，以及大量 extension / view service。
- **唯独** `com.apple.Preferences`、`com.apple.preferences`、
  `com.apple.settings` **全部 0 次**（大小写不敏感也是 0）。
  含 `preference|setting` 的 7 个 id 都是无关组件
  （`AXSettingsShortcuts`、`AutoSettings`、
  `FocusSettingsUI` …），没有一个是设置 App 本体。
- 全部 11 月以来的历史 `msgbuf.txt` 中，`com.apple.Preferences`
  与 `Preferences.app` **从未出现过**（不是 GPU 时代新退化的）。
- `payload.img` 里那 1 次 `/Applications/Preferences.app` 和
  1 次 `com.apple.Preferences` 属**静态表**：前者在 sandbox /
  容器规则表里（并列 `/Library/IdentityServices`、
  `/Library/Caches/com.apple.nsurlsessiond/…`），后者在
  cfprefs 偏好域名表里（并列 `com.apple.ConfigServer`、
  `com.apple.mobilemail`、`NanoPreferencesSync` 等），
  **都不是运行时 App 注册**。

⇒ **§69.134.4 的推断被削弱**：staged apps 从未复制属实，
但 799 个 App 照样注册了，说明 App 注册表/图标树的来源
不是那个目录。「staged apps 缺失 ⇒ 图标树为空 ⇒ 只有 5 个
`.app`」这条因果不成立。设置 App 的缺席是**针对单个 bundle
的特异性现象**，与 launchd 21 项禁用、与 staged apps、
与 GPU 判据都无直接关系。§69.134.5 的下一步 ① 已降级。

> **2026-11-15 20:5x 二次更正（§69.136）：本段的推论前提有误。**
> ① `ustate: caching container path` 是**容器路径缓存**（被查询才
> 产生），不是 App 注册表；它上面「799 个 App 照样注册了」不成立，
> 因而「staged apps 与图标树无关」也不能由它推出。
> ② 只读挂载 root 卷后实测：`/private/var/staged_system_apps`
> 里有 **29 个真实 App bundle**（不是空目录），
> `System/Library/AppPlaceholders` 有 29 个同名占位符；
> 其中 **28 个** 的 `CFBundleIdentifier` 出现在
> `SpringBoard.app/DefaultIconState~ipad.plist` 的默认图标布局里。
> ③ 同一张布局里明确列着 `com.apple.Preferences`。
> ④ `Preferences.app` 本体、可执行文件与 trust cache 签名
> **全部在系统卷/payload 里**（§69.136.1/2）。
> ⇒ 「设置被单独过滤」降级为 [推断]；更保守的解释是
> **SpringBoard 从未完成图标树/桌面构建**，所以没有进程去查询
> Settings 的容器，也没有派发它。§69.134.5 下一步 ① 因此
> **重新抬升为候选，但理由完全变了**：不是补 Settings 文件，
> 而是让默认布局引用的 28 个 bundle 拿到真实可执行体。
> 详见 §69.136。

### 69.135.5 新的收敛方向与下一步（零风险优先）

1. **Setup/migration 未被跳过**：`skip_setup=False` + base seed
   无 `SetupDone` / `purplebuddy` / `com.apple.springboard.plist`
   / `com.apple.migration.plist` ⇒ 每次开机都要走完整
   Setup/migration；日志里 `com.apple.datamigrator[132]` 与
   `com.apple.migrationpluginwrapper[171]` 确实启动。带这些
   plist 的 skipsetup 变体**从未被 fix160 使用**。
   `board.toml:391` 还记录过历史上「Swipe up to upgrade」
   gate 卡住。需查 Setup 是否真正 Finished、SpringBoard
   图标状态从哪来。

> **2026-11-15 21:0x 更正（§69.137）：本条已解答。**
> fix161 换 skipsetup seed 后 `com.apple.migrationpluginwrapper`
> 不再被派发（migration 确实跳过）、`SetupAssistant.framework/budd[137]`
> 启动、无「Swipe up to upgrade」卡死，历史 10-26「连 logo
> 都没到」未复现；**但设置与桌面仍未出现**。所以「每次开机
> 走完整 Setup」这条观察成立，却不是瓶颈。

2. ~~**root image 里是否真有 `Preferences.app`**~~
   **§69.136 已解答（假设证伪）**：只读挂载取证确认
   `/Applications/Preferences.app` 在系统卷里完整存在，
   且 `com.apple.Preferences` 明确列在默认图标布局中。
3. ~~**`com.apple.Preferences` 是否被单独过滤**~~
   **§69.136 已解答（trust cache 侧排除）**：CDHash 在
   OS trust cache 第 361 条、并逐字内嵌进 fix160 payload；
   与 MobileSafari 同为 flags `0e`，无降级。剩下的
   LaunchServices / 图标树构建问题已改由 §69.136.6 承接。
4. **§69.131 的 3000–5000 wakes/s 忙等自旋**仍是最靠近
   「一直停在蓝色启动屏」的行为特征（§69.136.4 复核：
   137.23 s 报 `45076 wakes / ~15 s = 2941 wakes/s`）。
   ~~`iconservicesagent` 在日志中出现 13 次，值得与自旋同一
   时段对齐~~ **§69.136.5 更正**：13 次全部落在
   `NSG gpu … text=backboardd,iconservicesagent,…` 这段
   白名单字符串里，不是进程启动，该建议作废。
5. **不做**：不刷机、不加 `sheng-staged-apps=1`（已降级）、
   不改 `XNU_WATCHDOG_POLICY` 宏或调用点、不跑
   `sheng-lite-reset=1`、不改 tick/watchdog/`sheng-autoreset`、
   不动 GPU 驱动/表/SMMU/zap、不放宽判据。

### 69.135.6 状态

- 本轮纯 host 侧只读：解 seed、扫明文、比 hash、读源码与日志；
  **未刷机、未改内核、未改任何 boot arg、未挂载任何磁盘镜像**。
- 三源码文件仍等于 fix158 基线；`board.toml` 本轮**零改动**
  （上一轮的注释更正保留）；设备 `a20248f7` 仍在 recovery，
  原厂 vendor_boot 已还原。
- 文档：新增本节；§69.134.4 加更正块；§69.134.5 下一步 ①
  标注降级。

---

## §69.136 零风险复核（三）— root 卷离线取证：设置 App、签名、图标布局全都在

承接 §69.135.5 第 2 条。`hdiutil attach -readonly -nobrowse`
只读挂载 `UniXNU/out/ipsw/21H461/044-76040-038.dmg`
（ULFO/lzfse、`Encrypted=false`、GUID + Apple_APFS），
取证后 `hdiutil detach /dev/disk4` 清理干净。**不刷机、不改内核、
不动任何 boot arg、不写任何镜像**。

**结论先行：§69.135.5 ② 的假设被直接证伪。** 设置 App 本体、
它的代码签名、以及「把设置放在默认桌面第一屏」的布局
**全部在系统卷与 fix160 payload 里**。设置起不来不是缺文件、
不是签名/信任链不通、也不是 21 项 launchd 禁用；本节把问题
进一步收敛到 **SpringBoard 图标树/Setup 链没有推进**。

### 69.136.1 系统卷实测 [已验证]

| 项 | 实测 |
| --- | --- |
| `/Applications` | 168 项；`Preferences.app` **存在**（57 项） |
| `Preferences.app/Info.plist` | `CFBundleIdentifier=com.apple.Preferences`、`CFBundleName=Settings`、`CFBundleExecutable=Preferences`、`MinimumOSVersion=17.7` |
| 可执行文件 | 1,546,176 B，Mach-O 64-bit arm64，带 `_CodeSignature/CodeResources` |
| `/Applications` 里**缺席** | `MobileMail` / `Weather` / `Maps` / `MobileNotes` / `Health` / `FaceTime` / `MobilePhone` / `Music` / `SpringBoard`（后者在 CoreServices） |
| `/private/var/staged_system_apps` | **29 个真实 App bundle**（AppleTV、Books、Contacts、FaceTime、Files、FindMy、Fitness、Freeform、Health、Home、Magnifier、Maps、Measure、MobileCal、MobileMail、MobileNotes、MobileStore、MobileTimer、Music、News、Photo Booth、Podcasts、Reminders、SequoiaTranslator、Shortcuts、Stocks、Tips、VoiceMemos、Weather） |
| `/System/Library/AppPlaceholders` | 29 个同名占位符；`Music.app`/`Weather.app`/`MobileMail.app` 的 Info.plist 带**相同 bundle id**，无可执行文件 |
| `SpringBoard.app` | 在 `/System/Library/CoreServices/`；含 `DefaultIconState.plist`、`DefaultIconState~ipad.plist` |
| `/Applications/Setup.app` | **存在，19 MB**，`CFBundleIdentifier=com.apple.purplebuddy` |
| `com.apple.springboard.plist` | 系统卷内 0 命中（该 plist 属 Data 卷/skipsetup seed） |

⇒ §69.134.4 当时「staged 源目录是空的」推断**不成立**：
29 个真实 bundle 一直在 OS 卷里，只是因 `sheng-staged-apps=1`
未启用而从未交给 Data 卷 / `SystemAppMigrator`。

### 69.136.2 trust cache：Preferences 签名就在 fix160 payload 里 [已验证]

`044-76040-038.dmg.trustcache`（59,519 B）是 IM4P(`trst`) 包裹：

- ASN.1/OCTET STRING payload 从 `0x17` 起；header 24 B：
  `version=2`、uuid `c0fb7965…226d55`、`num_entries=2478`（`@0x2b`）；
- entries 从 `0x2f` 起、**24 B/条**（20 B cdhash + 4 B flags），
  `0x2f + 2478*24 = 59519` = 文件长度；
- 用 `codesign -d -vvv` 取四个 App 的 CDHash，按前 20 B 查找：

| App | CDHash (前 20 B) | entry idx | 24 B 对齐 | flags4 | payload.img 偏移 |
| --- | --- | --- | --- | --- | --- |
| **Preferences** | `26e77bc6fdd61cf7cc4e50ff070b4e8107b52dd1` | 361 | ✅ | `02000e00` | 12,756,480 |
| SpringBoard | `8c139abc7a69cfc5dd1cef94f092035731b5e237` | 1339 | ✅ | `02000100` | 12,779,952 |
| MobileSafari | `d654e4b35d821d4df02838c7b8a0920c72e7d4ba` | 2083 | ✅ | `02000e00` | 12,797,808 |
| Weather (staged) | `e93fc97b596c590942c3380ebe3d7ec1fabdfcc0` | 2250 | ✅ | `02001000` | 12,801,816 |

- Preferences 与 MobileSafari **flags 同为 `0e`**，同级别、无降级；
- `payload.img` 内嵌同一 trust cache blob：四个哈希的
  `(payload_off - OS_off)` **恒为 12,747,769**；
- Cryptex 的 `044-76034-038.dmg.trustcache`（1,871 B）四个都 0 命中，
  符合它只管 cryptex。

⇒ **AMFI / trust cache / 代码签名不是设置起不来的原因**，
§69.135.5 ③ 的这一半可以结案。

### 69.136.3 ★ 28/29 个未安装 App 与 Settings 绑在同一张默认布局里 [已验证]

完整遍历两个 `DefaultIconState*.plist` 的全部字符串（第一遍只收
`bundleIdentifier` 键值会漏掉 `iconLists` 里的裸字符串数组）：

- `DefaultIconState~ipad.plist`：50 个 `com.apple.*` id；
  `DefaultIconState.plist`：51 个；**两者的 `iconLists` 都明确
  含 `com.apple.Preferences`**（默认桌面第一屏）；
- 逐个读 29 个 staged app 的 Info.plist，
  **28 个** 的 `CFBundleIdentifier` 出现在这张 ipad 布局里
  （唯一例外 `Fitness.app` / `com.apple.Fitness`）；
- 布局的 `buttonBar` 里就有 `Music`、`mobilemail`、
  `mobilenotes`，`iconLists` 里还有 `Maps`、`weather`、
  `Health`、`facetime`、`iBooks`、`podcasts`、`tv`、`findmy`、
  `stocks`、`shortcuts`、`tips`、`Translate`、`VoiceMemos` 等。

⇒ SpringBoard 若按这张默认布局构建图标树，会同时遇到
「已在 `/Applications` 的不可删除 App（含 Settings）」与
「28 个只有占位符、真实 bundle 还在 OS 卷 staged 目录」的条目。
**[推断]** 这让 `sheng-staged-apps=1` 重新成为有价值的单变量，
但理由与 §69.134 完全不同：不是为了补 Settings 文件，而是
让布局引用的 bundle 拿到真实可执行体，看图标树/Setup 能否推进。
**不宣称** SpringBoard 自旋一定是在等它们——`AppPlaceholders`
在正常 iOS 上也是合法状态。

### 69.136.4 运行时侧：Setup 从未启动，SpringBoard 自旋 [已验证]

| 观察 | fix160 msgbuf |
| --- | --- |
| `Early boot complete` | 121.645 s，launchd 正常（与 §69.131 一致） |
| `SpringBoard.app/SpringBoard[27]` | 121.932 s 启动 |
| 自旋告警 | 137.234 s `caught waking the CPU 45076 times over ~15 seconds, averaging 2941 wakes / second` |
| 带 pid 的 `.app` 主进程 | 仅 4 个：`SpringBoard[27]`、`identityservicesd[55]`、`imagent[64]`、`sociallayerd[172]`；另有 1 个 `.appex` 扩展 |
| Setup 痕迹 | `Setup.app` / `SetupAssistant` / `SetupDone` / `purplebuddy` / `Swipe up` / `setupd` **全部 0 次**；`Buddy` 2 次均为 `SOSBuddy`/`HealthENBuddy` 容器名 |
| `com.apple.Preferences` | **精确 0 次**；`Preferences.app` 0 次；`Preferences` 仅 2 次，且都是 `OSAnalytics/Preferences` 与 `IntlPreferences.framework` 路径片段 |
| 容器缓存行 | 859 条 / 60 行，集中在 124.903 s（169 条）与 141+ s（2130 行 252 条等）；打印源码**不在本工程源码树**（iOS 闭源组件） |
| 日志尾部 | 177.5–177.99 s 全是 `MUYU FB/SCAN/RB0/…` GPU 诊断，无 userland 打印（注意 §69.127 的刷盘截断，不能反推全局静默） |

`skip_setup=False` + base seed 无 `SetupDone` / `purplebuddy` ⇒
按设计每轮都要走完整 Setup，但 `Setup.app` 从未出现在启动行。

### 69.136.5 对既有结论的三处更正

1. **§69.134.4 / §69.135.4**：`ustate: caching container path`
   是「被查询才产生的容器路径缓存」，**不是 App 注册表**；
   799 个 id 不能证明「App 都注册了」，因此
   「`com.apple.Preferences` 针对单个 bundle 被特异性过滤」
   降级为 [推断]。更保守的解释是 SpringBoard 图标树/桌面
   从未构建完成，没有任何进程去查询它的容器。
2. **§69.135.5 ④**：`iconservicesagent` 13 次命中全部在
   `NSG gpu svc=AGXAccelerator … text=backboardd,iconservicesagent,
   WidgetRenderer_Default,PosterBoard` 白名单字符串里
   （`backboardd` verdict=1、`mediaserverd` verdict=0），
   **不是进程启动**；「与自旋同一时段对齐」作废。
3. **§69.134.5 ③**：`sheng-staged-apps=1` 从「已降级」改回
   **候选 B**（见下），但目标从「恢复 Settings 文件」改成
   「验证图标树/Setup 是否因此推进」；§69.134.4 的原文推断
   仍以更正块保留，不覆盖历史。

### 69.136.6 单变量候选（需用户点头，本轮一律未做）

| 候选 | 改什么 | 验证什么 | 代价 / 风险 |
| --- | --- | --- | --- |
| **A（优先）** | 换成已存在的 skipsetup Data seed `fw/data-seed/muyu-data-200g-skipsetup.dbr`（含 `SetupDone` / `com.apple.springboard.plist` / `purplebuddy` / migration 标记） | Setup gate 是不是瓶颈：Setup 能否 Finished、SpringBoard 能否脱离自旋、图标与设置是否出现 | **不动 kernel、不动 cmdline**；避开 1001 B 上限与数百 MiB 复制，单变量最干净 |
| **B** | 加 `sheng-staged-apps=1` | 28 个布局引用的 bundle 拿到真实可执行体后图标树是否推进 | 需先腾 ~20 B cmdline；首次复制数百 MiB 到持久 Data 卷；patch 注释记录 int-220 脏页 stall / int-221 递归爆栈，需长窗口 |
| **C（零风险）** | 离线查 payload 构建如何选 Data seed、`SystemAppMigrator` 触发条件、`Setup.app` 的 launchd 派发 plist | A 是否可行、有没有更直接的 gate | 无 |

**不宣称** A/B 任一能解决设置；两者都是单变量行为改动，
需明确确认后才动。A 前需先做 C 确认 seed 选择路径。

> **2026-11-15 21:0x 更新（§69.137）：A 已执行并证伪。**
> 用户选 A，fix161 只换 `--data-ramdisk` 上机一轮：migration
> plugin 确实被跳过（A 生效）、窗口 233 s / 59 轮、GPU 健康，
> 但 `com.apple.Preferences` 仍 0 次、`.app` 主进程 4→3、
> 自旋仍在（且新增 `lsd[103]` 62 s 自旋）、桌面/图标完成标记
> 仍 0。**Setup gate 不是（唯一）瓶颈**，候选 B 升为首选。

### 69.136.7 状态

- 只读取证：`hdiutil attach -readonly -nobrowse` →
  `/Volumes/DawnUpdate21H461.J120J121J207J208OS` →
  `hdiutil detach /dev/disk4`；
  `hdiutil info | grep -c 044-76040` = **0**，`/Volumes` 仅剩
  `MUYUQEMU` / `Macintosh HD` / `Recovery`；原有挂载的
  `fw/ipsw/24A446/043-68950-668.dmg` **未触碰、未卸载**。
- **未刷机、未改内核、未改任何 boot arg**；`board.toml` 本轮
  零改动；三源码文件仍等于 fix158 基线；设备 `a20248f7`
  仍在 recovery，原厂 vendor_boot 已还原。
- 文档：新增本节；§69.135.4 加二次更正块；§69.135.5 ②③④
  标注已解答/作废；§69.134.5 ③ 的状态由「降级」改为「候选 B」。

---

## §69.137 fix161 上机（候选 A）— Setup/migration gate 跳过了，桌面仍没来

用户 20:49 选 **A**：换 skipsetup Data seed，验证 Setup gate 是不是瓶颈。
本节是行为改动 + 一轮上机。**结论：gate 确实被跳过了，但设置 App
与桌面仍不出现；Setup gate 不是（唯一）瓶颈。阴性，但把下一步
指向了 LaunchServices。**

### 69.137.1 单变量设计：只换 `--data-ramdisk`，不跑 seed 编辑脚本

- 前置（§69.136.6 C）：
  - `payload.py:135-144`：`--data-ramdisk` 直接作为 FAT 卷的
    `scratch-data.img`；`--skip-setup` 才会调
    `skip_setup_seed()` → `seed-data-skip-setup.py`（该脚本会
    `hdiutil attach` **读写**挂载一个 200 GiB raw 镜像并重写 plist）。
  - `payload.json` 的 `skip_setup` 字段**只有写入者、没有消费者**
    （grep 全树），仅是元数据。
  - `board.toml:549` 记着 2026-10-26「official skip-setup path
    (this + `--skip-setup`) did not even reach the boot logo」，当时
    是配合 `sheng-disable-jobs=…` 一起用的；本轮不启用该参数，
    且**完全避开**读写挂载 seed 的路径。
  - 两个 skipsetup 变体同源（`base_sha256` 都是 `508c7c94…`，
    即 base seed 逻辑展开后的哈希）：
    `fw/data-seed/muyu-data-200g-skipsetup.dbr`
    （1,327,264 B，sha `f0da862f…`，`no_auto_dim=true`）与
    payload 目录里的缓存版（sha `539d7d99…`，`no_auto_dim=false`）。
    **选 fw 版**：额外 `SBDisableAutoDim` /
    `SBDontDimOrLockOnAC` / `SBDisableIdleSleep`，对无输入板子
    不会 30 s 后熄屏。
- 复现 fix160 命令：从 `board.toml` 的 `[payload] kernel_args`
  反推出 extra 是 4 项（`sheng-fb-gpu=1`、`sheng-gpu-addon=1`、
  `sheng-gpu-metal=1`、**`sheng-tum=swCD`**），模拟拼接后与
  fix160 `payload.json` 的 `kernel_args` **逐字节相同（1001 B）**。
- 构建（仅 `--data-ramdisk` 一个输入不同）：
  ```
  python3 UniXNU/unixnu build payload -D UniXNU-muyu-17 -j 8 \
    --data-ramdisk fw/data-seed/muyu-data-200g-skipsetup.dbr \
    --owner-trust-cache fw/trust-21H461/owner-trust-17-fix107.bundle \
    --gpu-addon fw/gpuaddon-17-fix107.bin \
    --unique-chip-id 0000000000000001 \
    --kernel-arg sheng-fb-gpu=1 --kernel-arg sheng-gpu-addon=1 \
    --kernel-arg sheng-gpu-metal=1 --kernel-arg sheng-tum=swCD
  ```
- **备份**：`/tmp/fix160-payload.img`、`/tmp/fix160-payload.json`、
  `/tmp/board.toml.fix160-baseline`；fix160 镜像未覆盖。

### 69.137.2 离线校验（全部通过）

| 项 | 结果 |
| --- | --- |
| `payload.json` | 13 个字段中 **10 个全同**；差异只有 `data_ramdisk_sha256`（`476464c3…`→`f0da862f…`）、`payload_sha256`、`adt_sha256` |
| `adt_sha256` 为什么变 | `make-muyu-adt.py:233` 写 `('random-seed', os.urandom(64))`；实测两个 ADT **恰好 64 字节**不同（offset 372–435），且不含任何 seed 哈希 ⇒ 每次构建必变，与单变量无关（§69.133.1 已记过同类现象） |
| FAT 逐文件 sha | `kernel.macho` `d05d633c…`、GPU addon `410685f6…`、trust cache `6fc1914b…`、`unixnu.toml` `2655cec3…` **全部 SAME**；仅 `scratch-data.img` 1,093,760→1,327,264 B 不同 |
| FAT 空闲 | 8,291,328→8,057,856 B（差 233,472 B = seed 增量按簇对齐）；`payload.img` 27,703,567 字节差只是 FAT 簇重排把 53 MB kernel 整体搬移 |
| vendor_boot | `fw/vendor_boot_gpu_fix161.img` 77,393,920 B，sha `0503ee85…`；**header 4 KiB、DTB+table+bootconfig 1,892,352 B 与 fix160 逐字节 SAME**，仅 payload 段不同 |
| cmdline | 1001 B，与 fix160 逐字相同 |
| seed 内容（离线解码） | `SetupDone`=1、`com.apple.springboard`=4、`com.apple.purplebuddy`=5、`com.apple.migration`=3、`SBDisableAutoDim`=1；`disabled.plist`=0、`com.apple.Preferences`=0 |

> **元数据陷阱**：因故意不传 `--skip-setup`，新 `payload.json`
> 里 `skip_setup` 仍是 `false`，但放进 FAT 卷的 `scratch-data.img`
> **已经是标记 Setup 完成的 seed**。后续读 `payload.json` 时
> 不要拿这个字段判断本轮行为。

### 69.137.3 上机结果：闭环正常，窗口更长

`tools/muyu-autoloop.sh fw/vendor_boot_gpu_fix161.img 900`，
日志 `logs/vendor_boot_gpu_fix161-logs/muyu-675723b9`：

| 判据 | fix160 | **fix161** |
| --- | --- | --- |
| 自动复位 / 窗口 / 轮数 | 203 s / 178 s / 44 | **233 s / 207 s / 59** |
| 需人工干预 | 无 | **无**（自动还原原厂 vendor_boot、回 recovery） |
| `Early boot complete` / `Finished boot task` | 1 / 1 | **1 / 1** |
| `MUYU ERR` 异常位 / POIS / trust | 全 0 / 0 / 正常 | **全 0 / 0 / 正常**（`trap/oob/hang/gpc/ccufl/stallfault` 均 0） |
| 日志时间戳范围 | 40.10–177.99 s | **41.00–207.14 s** |
| 最后一条 `==>` 启动 | 173.41 s `bluetoothd` | **202.37 s `UARPUpdaterServiceUSBPD`** |
| `MUYU` 诊断行数 | 3,340 | 1,111 |

GPU 诊断串减少**不是 GPU 故障**：msgbuf 被后段大量
`ustate: caching container path`（`PluginKitPlugin` 扩展枚举）
挤掉，与 §69.127 / `board.toml:541` 记录的「probe rows 被挤出
msgbuf」同款；`MUYU ERR` 全 0、`POIS=0`、`trust=` 9 次，判据健康。

### 69.137.4 ★ A 确实生效（不是被 persistent overlay 覆盖的无效实验）

- fix160 的 `migration` 2 次命中**全是**
  `DataMigration.framework/…/com.apple.migrationpluginwrapper[171]`
  的启动行；**fix161 该进程 0 次、字符串 0 次**。
- `com.apple.datamigrator` 在两轮都启动（各 2 次）：daemon 在，
  但 migration plugin 不再被派发 —— 正是 skipsetup seed 里
  `com.apple.migration LastSystemVersion = 21H461`
  「no data migration on every boot」的预期效果。
- `SetupAssistant.framework/budd[137]` 在 fix161 **启动**（fix160
  完全没有 `SetupAssistant` 字符串）；但 `Setup.app` /
  `SetupDone` / `com.apple.purplebuddy` / `Swipe up` 仍全 0。
  ⇒ Setup 框架有组件被拉起，**不能**据此说 Setup UI 已完成。

§69.135.5 第 1 条「Setup/migration 未被跳过」由此有了答案：
换 seed 后 migration 确实跳过了，且没有触发 `board.toml:391`
记录过的「Swipe up to upgrade」gate；历史 10-26「连开机 logo
都没到」的现象在 fix161 **没有复现**。

### 69.137.5 阴性结论：gate 不是瓶颈

| 判据 | fix160 | **fix161** |
| --- | --- | --- |
| `com.apple.Preferences` / `Preferences.app` / `Settings.app` | 0 / 0 / 0 | **0 / 0 / 0** |
| 带 pid 的 `.app` 主进程 | 4（SpringBoard、identityservicesd、imagent、sociallayerd） | **3**（少 sociallayerd） |
| `desktop` / `IconState` / `Setup complete` / `Setup Finished` | 0 | **0** |
| `==>` 启动行 / 唯一进程 | 194 / 185 | 188 / 177 |
| 容器缓存行 / 唯一 id | 859 / 852 | 523 / 517 |
| `staged_system_apps` / `AppPlaceholders` / `SystemAppMigrator` | 0 | **0** |
| `CRITEXIT` / `[reset]` | 2 / 1 | 2 / 1 |

**跳过了 migration、多跑了 30 s、userland 一直推进到 202 s，
设置 App 仍从未被查询/派发，桌面/图标完成标记仍 0。**
⇒ §69.135.5 ① 与 §69.136.6 A 的假设**证伪**：Setup gate
不是「设置打不开 / 桌面不出现」的（唯一）瓶颈。**不宣称**它
完全无关——只是单独跳它不足以解锁。

### 69.137.6 新线索：自旋从 backboardd 转到 `lsd`

| fix160 | fix161 |
| --- | --- |
| `launchd[1]` 4263/s @~99 s、`SpringBoard[27]` 2941/s @137 s、`backboardd[58]` 2596/s | `launchd[1]` **6431/s** @98.99 s、`logd_helper[91]` **10590/s** @128.21 s、`SpringBoard[27]` 1795/s @148.57 s、**`lsd[103]` 715/s 持续 62 s @202.25 s** |

- `lsd`（LaunchServices）字符串 7→33 次，且在窗口最末端仍在
  自旋；`SpringBoard` 自旋速率下降但没消失。
- fix161 尾部批量枚举 `PluginKitPlugin` 扩展容器
  （`UIUnderstanding`、`WallpaperKit`、`WorkflowUI`、`AuthKitUI` …），
  是 fix160 尾部没有的阶段。
- **[推断]** 结合 §69.136.3「28/29 个未安装的 staged App 被
  `DefaultIconState~ipad.plist` 引用、只有 `AppPlaceholders`
  占位符（同 bundle id、无可执行文件）」，LaunchServices 在
  构建图标/插件索引时可能卡在这些无法解析的条目上。
  **不宣称因果**，也不排除是 msgbuf 刷盘截断造成的观测偏差。
- 这让 §69.134.5 ③ / §69.136.6 的**候选 B（`sheng-staged-apps=1`）
  成为下一轮最值得做的单变量**：让 28 个布局引用的 bundle 拿到
  真实可执行体，看 `lsd` 自旋与图标树是否推进。

### 69.137.7 下一步（需用户点头，本轮未做）

> **2026-11-15 21:2x 更正（§69.138）**：下面 ① 的候选 B 已由用户
> 选中并完整执行（fix162），结果为**阴性**；② 的零风险前置也已
> 做完（1016 B 硬闸、patch 可达、231 MiB、marker 不存在）。
> 新的首选是 §69.138.5 的 D。本小节保留作为当时的决策记录。

1. **B（现为首选）**：加 `sheng-staged-apps=1`。约束不变：
   cmdline 已 1001 B / 上限 1016 B，加 ~20 B 需先腾出等量参数；
   首次复制数百 MiB 到持久 Data 卷，`zzy-staged-system-apps.patch`
   注释记录过 int-220 脏页 stall 与 int-221 递归爆栈，需长窗口。
2. **零风险前置**：离线核对 `SystemAppMigrator` 的触发条件、
   staged 目录复制的总字节数、以及能否在不改 cmdline 的情况下
   把 29 个 bundle 预置进 Data seed（例如用现成 seed 变体），
   避免 B 的 cmdline 预算与复制风险。
3. **不做**：不改 `XNU_WATCHDOG_POLICY` 宏/调用点、不加
   `sheng-lite=1` / `lite-reset`、不改 tick/watchdog/
   `sheng-autoreset`、不动 GPU 驱动/表/SMMU/zap、不放宽判据、
   不重跑 `seed-data-skip-setup.py` 的读写挂载路径。

### 69.137.8 状态

- `board.toml` 本轮**零改动**；三源码文件仍等于 fix158 基线；
  kernel `eb116e6d…` 未重建（沿用 fix160）。
- 冻结产物：`fw/vendor_boot_gpu_fix161.img`（sha `0503ee85…`）、
  `logs/vendor_boot_gpu_fix161-logs/muyu-675723b9`、
  `logs/build-payload-fix161.log`、
  `logs/build-vendor-boot-fix161.log`、
  `logs/autoloop-fix161.log`、
  `logs/vendor_boot_gpu_fix161-window.bin`；
  备份 `/tmp/fix160-payload.{img,json}`、
  `/tmp/board.toml.fix160-baseline`。
- 设备 `a20248f7` 在 recovery，原厂 vendor_boot 已还原，
  无遗留进程；`hdiutil info` 对 044-76040 计数 0。
- 文档：新增本节；§69.136.6 候选 A 标注已验证（阴性）；
  §69.135.5 ① 标注已解答。

---

## §69.138 fix162 上机（候选 B）— 231 MiB 复制确实发生，桌面/设置仍没来

用户 2026-11-15 21:10 选择 **B**。行为改动 + 一轮上机闭环。
基线 = fix161（skipsetup Data seed），目标变量 = 加
`sheng-staged-apps=1`。结论：**复制确实发生（+11.9 s 内核窗口、
231 MiB），但 29 个 App 没有被注册/安装，设置、图标、桌面仍不出现。**

### 69.138.1 前置离线核查（纠正上一轮两处预算误述）

- `UniXNU/scripts/unixnu/payload.py:38-39` 才是 cmdline 硬闸：
  **1016 B**（DarwinBoot 的 1024 B 缓冲区减余量），不是文档里
  反复写的「约 1000 B」。1001 + 20 = 1021，**只需腾 ≥5 B**，
  不是「需腾 ~20 B」。
- 生成树 `kern_exec.c:7963` 有 `native_staged_apps_prepare`，
  `:8085` 在 `native_owner_data_prepare` 里 `if (!error)` 无条件调用；
  `kernel.macho` 里 `sheng-staged-apps` 2 次、
  `/private/var/db/.sheng-staged-apps` 1 次；未 strip kernel 有 7 个
  `_native_staged_apps_*` 符号。`kern_exec.c:8254` 的
  `offline scratch Data mount failed` 字符串在二进制里存在 ⇒
  `XNU_OWNER_DATA_MOUNT=1`。**这次不像 §69.134 的
  `XNU_WATCHDOG_POLICY` 宏坑：patch 是活的、调用链可达。**
- OS 卷 `/private/var/staged_system_apps`：29 个 bundle、
  **4214 文件 / 6575 目录 / 231 MiB**，无 symlink / 特殊文件
  （patch 只处理目录 + 普通文件，匹配）。只读挂载取证后已 detach。
- 两个 Data seed 里 `.sheng-staged-apps` marker 均 0 命中 ⇒
  复制不会被 `stage=3` 跳过。
- `BootLog.c:310-311`：muyu 板不打印 sheng 诊断行，
  `native_staged_apps_stage/dirs/files/mib` 是 weak 引用且被跳过 ⇒
  **日志里不会有成功计数器**，只能靠时间线与间接判据判定。

### 69.138.2 单变量实现与离线校验

- 为腾 cmdline，注释掉 `sheng-disable-watchdogd=1`（26 B 含空格）。
  其唯一消费者 `BootWatchdogPolicy.c` 的调用点在生成树
  `kern_exec.c:8255` 被 `#if XNU_WATCHDOG_POLICY` 编译掉（§69.134），
  fix160/161 里 `watchdogd[53]` 照常启动 ⇒ **语义等价的死参数**。
  这也清掉了「禁用进程」时代最后一个 boot arg（用户要求解决的问题）。
- CLI 追加第 5 个 `--kernel-arg sheng-staged-apps=1`；
  cmdline **995 B**，与 fix161 差异仅 `+sheng-staged-apps=1` /
  `-sheng-disable-watchdogd=1` 两个 token，余量 21 B。
- 其余输入与 fix161 逐字相同：skipsetup seed `f0da862f…`、
  kernel `eb116e6d…`、trust fix107、GPU addon fix107、chip id、
  4 个 GPU arg。kernel 未重建。
- FAT 逐文件哈希：kernel `d05d633c`、addon `410685f6`、
  trust `6fc1914b`、`scratch-data.img` `f0da862f` 全 SAME；
  `unixnu.toml` 1392→1386 B（只差 cmdline 一行）；
  ADT 13992 B 仅 offset 372..435 的 64 B `os.urandom(64)`
  （`make-muyu-adt.py:233`，每次构建必变）。
- `fw/vendor_boot_gpu_fix162.img` sha `a6b39a93…`；
  header / DTB / table / bootconfig 与 fix161 逐字节 SAME，
  payload 区仅 **509 B** 差异。

### 69.138.3 上机结果：复制发生的硬证据

`212 s 自动复位 / 187 s 窗口 / 49 轮`，无需人工干预，
原厂 vendor_boot 已还原、设备回 recovery。
日志 `logs/vendor_boot_gpu_fix162-logs/muyu-67423cd9`
（kernel=187 s、msgbuf_reaches=187 s、writer=97 s、rounds=49、
errors=2）。GPU 判据健康：`MUYU ERR` 异常位全 0、`POIS=0`、
`stallfault=0`；`[log] ufs lock` 1–2 give-ups 与三轮一致。

| 里程碑 | fix160 | fix161 | fix162 |
| --- | --- | --- | --- |
| `MOUNT pid 1 /private/var` | 91.41 s | 91.36 s | 90.93 s |
| `load_init_program /sbin/launchd` | 91.96 s | 91.92 s | **102.83 s** |
| **这一段耗时** | 0.55 s | 0.56 s | **11.90 s** |
| `shared_region map_and_slide ENTER` | 115.94 | 115.56 | 125.90 |
| `Early boot complete` | 121.65 | 123.24 | 132.24 |

+11.3 s 精确落在 Data mount 之后、launchd 之前的内核态窗口，
且之后所有里程碑整体平移 ~+10.3 s（**一次性偏移，不是持续变慢**）；
231 MiB / 11.3 s ≈ 20 MiB/s，与 4214 次逐文件 `VNOP_FSYNC` 的
量级吻合 ⇒ `native_staged_apps_prepare` 真的跑了并完成复制
（marker 写入后，同 seed 再开机会走 `stage=3`）。
**int-220 脏页 stall / int-221 递归爆栈未复现**：patch 的逐文件
fsync 与迭代 walk 生效，长窗口（1200 s）未用满。

### 69.138.4 阴性结论与两处自我更正

| 判据 | fix160 | fix161 | fix162 |
| --- | --- | --- | --- |
| `com.apple.Preferences` / `Preferences.app` / `Settings.app` | 0/0/0 | 0/0/0 | **0/0/0** |
| 带 pid 的 `.app` 主进程 | 4 | 3 | **3** |
| `IconState` / `desktop` / `Setup complete` | 0 | 0 | **0** |
| `staged_system_apps` / `SystemAppMigrator` / `AppPlaceholders` | 0 | 0 | **0** |
| `lsd[100]` 自旋 | – | 715/s × 62 s | **915/s × 49 s** |
| SpringBoard 自旋 | 2941/s | 1795/s | 3058/s |
| 容器 id 全集 | 852 | 517 | 534 |

fix162 相对 fix161 新增的 17 个容器 id **全是 `PluginKitPlugin`
扩展**（AuthKit / Calendar / Siri / AWD / SoftwareUpdateServices…），
28 个 staged App 的 bundle id 一个都没被枚举；
`installcoordinationd` 只出现在 `ustate: caching container path`
缓存行里，不是进程。

- ⇒ **§69.137.6 的 [推断] 被削弱**：真实可执行体到位后
  `lsd` 自旋仍在、图标树仍没建起来，所以「占位符无可执行文件导致
  LaunchServices 卡住」不是（唯一）原因。
- ⇒ **§69.137.7 ① 的候选 B 假设证伪**：复制本身不足以让系统 App
  被注册，也不足以让设置出现。

**最可能的新解释 [推断]**：patch 注释明确写
「`SystemAppMigrator` installs them during the first-boot data
migration」。fix162 沿用 fix161 的 **skipsetup seed**（预置
`SetupDone` / `com.apple.migration LastSystemVersion`），A 已证明
`migrationpluginwrapper` 不启动（fix160 2 次 → fix161/162 0 次）。
迁移被跳过 ⇒ 复制进 `/private/var/staged_system_apps` 的 29 个
bundle 无人搬进 `/Applications`、无人注册。不宣称已证实。

**观测限制**：fix162 窗口 187 s 比 fix161 短 20 s，且
`Early boot complete` 晚 9 s ⇒ userland 有效观察 **55 s vs 84 s**。
不能排除「再给 30 s 才推进」，但 fix161 跑到 207 s 也无设置/图标，
倾向不是纯时间问题。

### 69.138.5 下一步（需用户点头，本轮未做）

> **2026-11-15 21:4x 更正（§69.139）**：下面 ① 的候选 D 已由用户
> 选中并完整执行（fix163），结果是**观测失败**（writer 在内核
> ~151 s 停止刷盘），不是干净的阴性；② 的零风险前置也已做完，
> 且其中「从 recovery 扫 persistent overlay 找 marker 产物」被
> **原理证伪**（§69.139.1）。本小节保留作为当时的决策记录。

1. **D（首选）**：回到 **base Data seed**（`fw/data-seed.img`，
   即 fix160 的输入）+ 保留 `sheng-staged-apps=1`，让 first-boot
   data migration 真正运行。相对 fix160 仍是单变量（只加目标
   boot arg + 删死参数腾空间），相对 fix162 则是把 A 的换 seed
   回退；这样能把「复制」与「安装」两步分开。
   `sheng-autoreset` 不动。
2. **零风险前置**：离线查 `SystemAppMigrator` 的触发 plist /
   launchd job 与 skipsetup seed 里 `com.apple.migration` 的键值，
   确认它是否真的短路 migrator；也可从 recovery 只读扫
   persistent overlay window（fix162 日志 L403
   `NativeUfs: store open blocks=52430401 first=8d41ba`）
   找 `.sheng-staged-apps` 与 App 文件名，把「复制完成」从
   时间线推断升级为产物证据。
3. **不做**：不改 `XNU_WATCHDOG_POLICY`、不加 `sheng-lite=1` /
   `lite-reset`、不动 tick / watchdog / `sheng-autoreset`、
   不动 GPU 驱动 / 表 / SMMU / zap、不放宽判据、不重跑
   `seed-data-skip-setup.py` 的读写挂载路径。

### 69.138.6 状态与产物

- `board.toml`：**仅注释 + 注释掉一行死参数**
  （`sheng-disable-watchdogd=1`），TOML 解析通过；
  备份 `/tmp/board.toml.fix161-baseline`。
- 三源码文件仍等于 fix158 基线；kernel 未重建。
- 冻结：`fw/vendor_boot_gpu_fix162.img`（sha `a6b39a93…`）、
  `logs/vendor_boot_gpu_fix162-logs/muyu-67423cd9`、
  `logs/build-payload-fix162.log`、
  `logs/build-vendor-boot-fix162.log`、`logs/autoloop-fix162.log`；
  备份 `/tmp/fix161-payload.{img,json}`、
  `/tmp/fix161-vendor-boot.img`。
- 设备 `a20248f7` 在 recovery，原厂 vendor_boot 已还原；
  root DMG 只读挂载已 detach（`hdiutil info` 对 044-76040 = 0）；
  无遗留进程。
- 文档：新增本节；§69.137.7 加执行状态更正块。

---

## §69.139 fix163 上机（候选 D）— 231 MiB 复制再次发生，但 log writer 停摆，判据不可判

用户 2026-11-15 21:2x 选择 **D**。基线 = fix162，目标变量 =
只把 `--data-ramdisk` 从 skipsetup seed 换回 base seed
（`fw/data-seed.img`），保留 `sheng-staged-apps=1`，让 first-boot
data migration 真正运行。结论：**复制再次发生（+11.93 s 内核窗口，
硬证据），但这一轮日志在内核 ~151 s 就被 writer 截断，
核心判据（设置 / 图标 / 桌面 / migrator）全部不可判；
不能写成「D 阴性」。**

### 69.139.1 零风险前置：persistent overlay 语义更正（★ 证伪 §69.138.5 ②）

> **2026-11-15 21:5x 限定更正（§69.140）**：下面「fix161/162 的
> 231 MiB 与 marker 从未落盘」只适用于 **`Kept` 轮次**；fix163
> 第一轮是 `Resumed`（overlay `active=true`），它写的 marker
> **确实**持久化了，所以第二轮直接走 `stage=3` 跳过复制
> （§69.140.3 硬证据）。本节的 `Kept` 语义本身不变。

- `UniXNU/drivers/RamBlock/PersistOverlay.h:6-16`：窗口头部
  `SHNGDATA` 带 **seed 的 SHA-256**；`:62-63` 的 `checkHeader()`
  在 seed 不符时返回 3。`:88-100`：没有 `reset` 时
  `if (!reset) return Open::Kept;`，bitmap 清零、`active=false`、
  窗口一个字节都不写。
- `UniXNU/drivers/RamBlock/NativeRamBlock.cpp:112-116` 的
  `Kept` 分支明确打印「Data runs from the seed」，
  `overlay.active=false`；`PersistOverlay.h:131` 的
  `writeRun()` 在 `!active` 时直接返回 false ⇒ **Kept 轮次里
  Data 卷的改动不进持久窗口，只在 RAM sparse page 里**。
- ⇒ 推论链（+11.93 s 实证见 §69.139.3）：fix161/162 换成
  skipsetup seed 后是 `Kept`，它们复制的 231 MiB 与
  `/private/var/db/.sheng-staged-apps` **从未落盘到 userdata
  末尾的持久窗口**；窗口里一直留着 fix160 时代的 base-seed
  overlay。fix163 换回 base seed 时头部重新匹配，会 `Resumed`
  fix160 的 Data 状态，但那里面没有 marker。
- ⇒ **§69.138.5 ② 的「从 recovery 只读扫 persistent overlay
  找 `.sheng-staged-apps` 与 App 文件名」从原理上不成立**，
  不必再做；§69.138.3 的「marker 写入后同 seed 再开机会走
  `stage=3`」也须限定为 overlay `active` 的轮次。
- 32 MiB log window 快照（fix160/161/162/163）扫
  `LastSystemVersion` / `SetupDone` / `.sheng-staged-apps` /
  `Preferences.app` 全 0 命中；只有 fix160 有 2 次
  `com.apple.migration`。这些快照是 **log window，不是 Data
  overlay**，所以「无命中」不构成独立证据，以上以源码为准。
- 另确认 `UniXNU/scripts/make-data-seed.py:76-87`：只有
  `--skip-setup` 分支才写 `com.apple.migration LastSystemVersion`
  与 `SetupDone`；`fw/data-seed.img`（base）没有这些键
  （§69.135 离线解码已独立确认）。`seed-data-skip-setup.py:71-80`
  则明确写 `LastSystemVersion = 21H461`，即 skipsetup seed
  短路 data migration 的机制。D 换回 base seed 就是为了去掉这个短路。

### 69.139.2 单变量实现与离线校验（全部通过）

- 构建命令与 fix162 **逐字相同**，只把
  `--data-ramdisk fw/data-seed/muyu-data-200g-skipsetup.dbr`
  换回 `--data-ramdisk fw/data-seed.img`；五个 `--kernel-arg`
  （含 `sheng-staged-apps=1`）不变。**`board.toml` 本轮零改动**，
  kernel 未重建，三源码仍等于 fix158 基线。
- 校验：`payload.json` 13 字段 10 SAME，差异仅
  `data_ramdisk_sha256`（`f0da862f…`→`476464c3…`）、
  `adt_sha256`、`payload_sha256`；`kernel_args` **995 B 逐字节相同**；
  FAT 逐文件 sha `kernel.macho` `d05d633c` / addon `410685f6` /
  trust `6fc1914b` / `unixnu.toml` 全 SAME，仅
  `scratch-data.img` 1,327,264→1,093,760 B；
  ADT 13992 B 仅 offset 372..435 的 64 B `os.urandom(64)`；
  vendor_boot header 4 KiB 与 `[0x4801000:]` 尾部与 fix162
  **逐字节 SAME**。
- 产物 `fw/vendor_boot_gpu_fix163.img` 77,393,920 B，
  sha `e98e44abadf451636f9d4c818f9bedced6cac6b2cfb87f49b69f5c037cc2fd2a`。

### 69.139.3 上机：复制再次发生（阳性硬证据）

`tools/muyu-autoloop.sh fw/vendor_boot_gpu_fix163.img 900`：
**237 s 自动复位** / 内核窗口 155 s / 33 轮，无需人工干预，
原厂 vendor_boot 已还原、设备回 recovery。
日志 `logs/vendor_boot_gpu_fix163-logs/muyu-67614d09`。

| 里程碑 (s) | fix160 | fix161 | fix162 | **fix163** |
| --- | --- | --- | --- | --- |
| `MOUNT pid 1 /private/var` | 91.41 | 91.36 | 90.93 | **91.15** |
| `load_init_program /sbin/launchd` | 91.93 | 91.88 | 102.81 | **103.08** |
| **这一段耗时** | 0.52 | 0.52 | 11.88 | **11.93** |
| `Early boot complete` | 121.65 | 123.24 | 132.24 | **132.75** |

+11.41 s 精确落在 Data mount 之后、launchd 之前的内核态窗口，
之后里程碑整体平移 ~+11.1 s（一次性偏移），与 fix162 的
11.88 s / +10.3 s 同量级 ⇒ `native_staged_apps_prepare` 在
base seed 下**再次完成 231 MiB 复制**。这同时实证了
§69.139.1：换 seed 后 marker 不在，复制没有被 `stage=3` 跳过。
`int-220` / `int-221` 仍未复现。

### 69.139.4 ★ 本轮是观测失败：writer 在内核 ~151 s 停摆

| 指标 | fix160 | fix161 | fix162 | **fix163** |
| --- | --- | --- | --- | --- |
| 设备自动复位 | 203 s | 233 s | 212 s | **237 s** |
| `kernel_uptime_s` | 178 | 207 | 187 | **155** |
| `writer uptime_s` | 87 | 117 | 97 | **65** |
| `msg_bytes` | 607 KB | 303 KB | 248 KB | **92 KB** |
| 日志行数 | 4086 | 1888 | 1439 | **620** |
| `==>` 服务启动行 | 77 | 78 | 66 | **25** |
| 唯一容器 id | 857 | 523 | 541 | **15** |
| `write_errors` | 5 | 2 | 2 | **4** |
| `write_ms last / max` | 3 / 179 | 8 / 168 | 2 / 174 | **184 / 184** |

- `slots.txt` 最后一个 slot 是 `#slot 28 t=61s`，即 writer
  起点（内核 ~90 s）后 61 s = **内核 ~151 s** 之后再无落盘；
  设备却在 237 s 才复位 ⇒ **约 86 s 完全无日志**。
- `msgbuf.txt` 时间戳只到 151.43 s，末尾是整片 `\0` 填充；
  `write_ms_last=184` 是四轮唯一「最后一次写入也达到最大值」
  的一轮。`[log] ufs lock` 只有 1→2 give-ups（fix162 是 2→3），
  所以**不是** UFS 锁竞争加剧；更像 writer 自身拿不到调度或
  写放大拖死。
- fix162 同样做了 231 MiB 复制却 `writer=97 s`、`errors=2`、
  248 KB ⇒ 退化**不是复制本身造成的**。D 相对 fix162 唯一的
  变量是 base seed（无 `SetupDone` / `LastSystemVersion`），
  与「first-boot migration 真正运行、产生大量 Data 写入把 log
  writer 挤掉」一致。**[推断]，不宣称因果**；这是本轮唯一的
  阳性间接信号。

### 69.139.5 判据表：全 0，但不可作阴性结论

| 判据 | fix160 | fix161 | fix162 | **fix163** |
| --- | --- | --- | --- | --- |
| `migrationpluginwrapper` | 2（135.12 s） | 0 | 0 | **0** |
| `com.apple.datamigrator[pid]` | `[132]` @132.06 | @140.09 | @135.85 | **`[104]` @148.78** |
| `SystemAppMigrator` / `staged_system_apps` / `AppPlaceholders` | 0 | 0 | 0 | **0** |
| `com.apple.Preferences` / `Preferences.app` / `Settings.app` | 0 | 0 | 0 | **0** |
| `IconState` / `desktop` / `Setup complete` | 0 | 0 | 0 | **0** |
| `MUYU ERR` / `SCAN` / `FBC` / `POIS` | 37 / 有 | 8 / 有 | 4 / 有 | **0 / 0 / 0 / 0** |

- **userland 有效观察只有 132.75→151.43 s ≈ 18.7 s**
  （fix162 55.6 s、fix161 84 s），且 `==>` 只剩 25 行、
  容器枚举只跑了 15 个 id（其他轮 523–857）。fix160 的
  `migrationpluginwrapper` 在 135.12 s，按 +11 s 平移后约
  146 s，恰好落在窗口边缘，而这段日志大量丢失。
  ⇒ 上表所有 0 **不能写成「D 证伪」**，只能写「本轮未观测到」。
- GPU 同理：`MUYU` 行只剩 47 条且全是前端/USHB 类，
  ERR/SCAN/FBC/POIS 诊断串被 userland 刷屏挤光；
  `ZAPOK=1` 只说明 zap 成功。**不宣称 GPU 健康或不健康**。
- `com.apple.datamigrator[104]` 四轮都启动（每轮 1 个 pid），
  它**不是**区分 migration 是否短路的判据；区分点是
  `migrationpluginwrapper`（§69.137.3）。
- 原始 `window.bin` / `log.bin` 里同样 0 命中
  `migrationpluginwrapper` / `SystemAppMigrator` /
  `Preferences.app` / `IconState`，排除了「decode 丢 slot」
  造成漏读的可能 ⇒ 这些串确实没有进入任何已落盘的日志。

### 69.139.6 下一步（需用户点头，本轮未做）

> **2026-11-15 21:5x 更正（§69.140）**：下面 ① 的候选 E 已由
> 用户选中并执行（fix163 第二轮），结论是**同镜像 ≠ 同状态**，
> E 的原定目标（判 writer 退化是否偶发）**未达成**；但拿到了
> `stage=3` 首次实证与 D 的正式阴性结论。本小节保留作决策记录。

1. **E（首选，零代码改动）**：同一张
   `fw/vendor_boot_gpu_fix163.img` 再上机一轮，判定
   §69.139.4 的 writer 退化是偶发还是 D 的稳定特征。
   若仍退化，问题就从「migration 是否运行」变成
   「换观测通道」（例如 `muyu-dump-at` 提前快照、压掉
   userland 刷屏、或加大 msgbuf），而不是继续加变量。
2. **F（行为改动）**：`sheng-data-reset=2`
   （`NativeRamBlock.cpp:79/82`，Install and boot 语义）让 D
   从全新 overlay 开始，排除 fix160 残留 Data 状态的影响。
   `payload.py:38` 的硬闸是 `len(line) > 1016`，现 995 B
   + 21 B = 1016 B **恰好通过、零余量**，需先确认再动。
3. **不做**：不改 `XNU_WATCHDOG_POLICY`、不加 `sheng-lite=1` /
   `lite-reset`、不动 tick / watchdog / `sheng-autoreset`、
   不动 GPU 驱动 / 表 / SMMU / zap、不放宽判据、
   不重跑 `seed-data-skip-setup.py` 的读写挂载路径。

### 69.139.7 状态与产物

- `board.toml` **本轮零改动**（仍 fix162 状态：无未注释
  `sheng-lite=1`、无活跃 `sheng-disable-watchdogd=1`、
  `sheng-autoreset=300`）；三源码文件仍等于 fix158 基线；
  kernel `eb116e6d…` 未重建。
- 冻结：`fw/vendor_boot_gpu_fix163.img`（sha `e98e44ab…`）、
  `logs/vendor_boot_gpu_fix163-logs/muyu-67614d09`、
  `logs/vendor_boot_gpu_fix163-window.bin`、
  `logs/build-payload-fix163.log`、
  `logs/build-vendor-boot-fix163.log`、`logs/autoloop-fix163.log`；
  备份 `/tmp/fix162-payload.{img,json}`、
  `/tmp/fix162-vendor-boot.img`、`/tmp/board.toml.fix162-baseline`。
- 设备 `a20248f7` 在 recovery，原厂 vendor_boot 已还原；
  `hdiutil info` 对 044-76040 计数 0；无遗留挂载 / 进程。
- 文档：新增本节；§69.138.5 加执行状态更正块。

---

## §69.140 fix163 第二轮（候选 E）— 同镜像 ≠ 同状态：`stage=3` 首次实证，D 判据正式阴性

用户 2026-11-15 21:4x 选择 **E**：同一张 fix163 镜像再上机一轮，
判定 §69.139.4 的 writer 退化是偶发还是稳定。结论：
**E 的原定目标未达成**——R1 改写了持久 Data 卷，R2 是在 R1
之后的状态上跑的，两轮不是受控重复；但意外拿到了
**`stage=3` 跳过复制的首次实证**，以及观测质量足够的
**D 正式阴性结论**。

### 69.140.1 前置：不覆盖 R1 产物，镜像字节完全相同

- 用 `ln` 建立 `fw/vendor_boot_gpu_fix163-r2.img`（与 R1
  **同一 inode 的硬链接**，sha 同为
  `e98e44abadf451636f9d4c818f9bedced6cac6b2cfb87f49b69f5c037cc2fd2a`，
  `cmp` SAME）；TAG 变成 `vendor_boot_gpu_fix163-r2`，
  R1 的 window / autoloop / decode 产物一个都不覆盖。
- R1 额外备份：`logs/vendor_boot_gpu_fix163-round1-window.bin`、
  `logs/autoloop-fix163-round1.log`、
  `logs/vendor_boot_gpu_fix163-round1-logs/`。
- `board.toml` 与 `/tmp/board.toml.fix162-baseline` `cmp` SAME；
  三源码 sha 仍 `5256d261…` / `1ccc9758…` / `5380763d…`
  （fix158 基线）；kernel 未重建。设备起始在 recovery。

### 69.140.2 上机结果

`tools/muyu-autoloop.sh fw/vendor_boot_gpu_fix163-r2.img 900`：
**167 s 自动复位** / 内核窗口 141 s / 26 轮，无需人工干预，
原厂 vendor_boot 已还原、设备回 recovery。
日志 `logs/vendor_boot_gpu_fix163-r2-logs/muyu-673c10f2`
（`msg_bytes=300061`、`writer=51s`、`kernel_uptime_s=141`、
`rounds=26`、`slots=23`、`errors=3`、`write_ms=8/214`）。

### 69.140.3 ★ 决定性差异：R2 没有复制（`stage=3` 首次实证）

| 里程碑 (s) | fix160 | fix162 | R1 | **R2** |
| --- | --- | --- | --- | --- |
| `MOUNT pid 1 /private/var` | 91.41 | 90.93 | 91.15 | **91.30** |
| `load_init_program /sbin/launchd` | 91.93 | 102.81 | 103.08 | **91.83** |
| **这一段耗时** | 0.52 | 11.88 | **11.93** | **0.53** |
| `Early boot complete` | 121.65 | 132.24 | 132.75 | **121.96** |

- `UniXNU/patches/xnu/zzy-staged-system-apps.patch:253-263`：
  `native_staged_apps_prepare()` 第一件事就是
  `vnode_lookup("/private/var/db/.sheng-staged-apps")`，
  **命中即 `native_staged_apps_stage = 3; return;`**，
  整个 231 MiB 复制被跳过；复制完成后 `:293-300` 才用
  `vnode_open(O_CREAT|O_TRUNC|FWRITE)` 写 marker。
- 证据链：R1 是 base seed 的 `Resumed`（§69.140.3 与
  §69.139.1 的 overlay 语义一致，`active=true`），复制
  11.93 s 并把 marker 写进 Data 卷 ⇒ 持久化到 userdata 窗口；
  R2 同 seed `Resumed` ⇒ marker 可见 ⇒ `stage=3` ⇒ 0.53 s，
  所有里程碑回到 fix160 基线（121.96 vs 121.65 s）。
- ⇒ **§69.138.3 的「marker 写入后同 seed 再开机会走
  `stage=3`」首次实证**；§69.139.1 的「231 MiB 与 marker
  从未落盘」**只适用于 fix161/162 的 `Kept` 轮次**，
  对 `Resumed` 轮次不成立，已在该节加限定更正。
- ⇒ **E 的方法论结论：同镜像 ≠ 同状态**。persistent overlay
  让 R2 变成「复制已跳过」的另一个配置，所以两轮差异
  **不能归因于偶发性**。以后任何「同镜像重跑」都必须先
  固定 Data 卷代际，否则不是受控重复。

### 69.140.4 writer 对照：R2 健康，但不能判「偶发」

| 指标 | R1 | **R2** |
| --- | --- | --- |
| 设备自动复位 | 237 s | **167 s** |
| `kernel` / `writer` uptime | 155 / 65 s | **141 / 51 s** |
| msgbuf / 行数 | 92 KB / 621 行 | **300 KB / 1966 行** |
| `==>` 服务启动行 / 容器 id | 25 / 15 | **59 / 497** |
| `MUYU` 诊断行 | 47 | **1340** |
| `write_errors` | 4 | **3** |
| `write_ms last / max` | **184 / 184** | **8 / 214** |
| msgbuf 末尾 | 整片 `\0` 填充 | 正常 `MUYU FBC 008=2287` / `MUYU FBD` |
| 最后 slot | `t=61s`（内核 ~151 s 后断） | `t=51s`（= 内核 141 s，全程连续） |
| `[log] ufs lock` give-ups | 1→2 | 2→4→5（与 fix160 3→4→6 同量级） |
| 复制 | 11.93 s | 0.53 s（跳过） |

- R1 的 writer 卡死（`write_ms_last=max`、末尾 `\0`）**没有**
  在 R2 复现：R2 writer 一直写到内核窗口末端、诊断串齐全、
  末行是正常 GPU 行。但两轮 Data 状态不同，故 **E 的原定
  目标（判偶发/稳定）没有达成**。
- 能统一四轮的 **[推断]**：「231 MiB 复制」与
  「first-boot migration」单独出现都不拖死 writer
  （fix162 有复制、skipsetup seed 无 migration → 健康；
  R2 有 migration plugin、复制被跳过 → 健康），
  只有 R1 两者叠加才 `184/184` 卡死 ⇒ 写放大叠加。
  只有 1 个 R1 样本，**不宣称因果**。
- R2 窗口 141 s / 复位 167 s 比 fix160 的 178 / 203 s 还短，
  仍是本轮未解释的现象；不归因、不放宽判据。

### 69.140.5 ★ D 的正式判据：观测质量足够，结论阴性

- R2 GPU 判据齐全且健康：`MUYU ERR` 11 条全
  `ist=00000000 trap=0 oob=0 hang=0 gpc=0 stallfault=0`、
  `SEC trust=00000000`、`ZAPOK=1`、`SCAN` 211、`FBC` 21、
  `POIS=0`，判据延续到 141.48 s。R1 里「GPU 诊断串全 0」
  确认是日志通道问题，不是 GPU 故障。
- **`migrationpluginwrapper` 2 次 @135.77 s**
  （fix160 2 次 @135.12 s；fix161/162 各 0）⇒ **base seed
  确实恢复了 migration plugin 的启动**，D 的设计意图在
  进程层面生效，是本轮最硬的阳性证据。
- 但 `SystemAppMigrator` / `staged_system_apps` /
  `AppPlaceholders` / `com.apple.Preferences` /
  `Preferences.app` / `Settings.app` / `IconState` /
  `desktop` / `Setup complete` 仍**全 0**；`.app` 主进程
  仍 3 个（`SpringBoard[27]` / `identityservicesd[55]` /
  `imagent[64]`，fix160 是 4），`installcoordinationd[pid]`
  仍 0（只在 `ustate: caching container path` 缓存行）。
- ⇒ **「让 first-boot migration 运行」不足以让 staged App
  被注册/安装，也不足以让设置、图标、桌面出现。**
  D 到此给出正式阴性结论（R1 的那些 0 不再单独算证据）。
- **观测限制**：R2 userland 有效观察 121.96→141.48 s
  ≈ 19.5 s；但 fix160 178 s / fix161 207 s / fix162 188 s
  三个更长窗口同样无设置/图标，倾向不是纯时间问题。
- `com.apple.datamigrator[pid]` 五轮都启动，**不是**区分
  migration 是否短路的判据（区分点是
  `migrationpluginwrapper`，§69.137.3）。

### 69.140.6 下一步（需用户点头，本轮未做）

1. **零风险首选（本轮未做）**：只读挂载
   `044-76040-038.dmg`，查 `SystemAppMigrator` 的 launchd
   job / 触发 plist 与 `DataMigration.framework` 的迁移条件，
   解释「`migrationpluginwrapper` 已启动、SystemAppMigrator
   却 0 命中」。工程树全库 grep `SystemAppMigrator` 只出现在
   `zzy-staged-system-apps.patch` 的注释里（§69.139.1 已核），
   所以必须查 OS 卷；同时核对
   `DefaultIconState~ipad.plist` 的运行期读取路径。
2. **F（受控重跑，行为改动）**：`sheng-data-reset=2`
   （`NativeRamBlock.cpp:79/82`，Install and boot 语义）让
   每轮从全新 overlay 开始，才能把 R1 的 writer 卡死做成
   受控重复。cmdline 995 + 21 = 1016 B，`payload.py:38`
   硬闸是 `len(line) > 1016`，**恰好通过、零余量**，
   动手前需再核对。
3. **G（零代码改动，收益低）**：再跑 R3，确认 `stage=3`
   稳定、marker 持续存在，并看窗口是否拉长。
4. **不做**：不改 `XNU_WATCHDOG_POLICY`、不加
   `sheng-lite=1` / `lite-reset`、不动 tick / watchdog /
   `sheng-autoreset`、不动 GPU 驱动 / 表 / SMMU / zap、
   不放宽判据、不重跑 `seed-data-skip-setup.py` 的读写挂载路径。

### 69.140.7 状态与产物

- `fw/vendor_boot_gpu_fix163-r2.img` 是
  `fw/vendor_boot_gpu_fix163.img` 的**硬链接**（同一文件，
  sha 相同），不是新构建；R1 镜像未被改动。
- R2 冻结：`logs/vendor_boot_gpu_fix163-r2-logs/muyu-673c10f2`、
  `logs/vendor_boot_gpu_fix163-r2-window.bin`、
  `logs/autoloop-fix163-r2.log`；R1 备份见 §69.140.1。
- `board.toml` **本轮零改动**；三源码仍等于 fix158 基线；
  kernel `eb116e6d…` 未重建。
- 设备 `a20248f7` 在 recovery，原厂 vendor_boot 已还原；
  `hdiutil info` 对 044-76040 计数 0；无遗留挂载 / 进程。
- 文档：新增本节；§69.139.1 加 `Resumed` 限定更正、
  §69.139.6 加执行状态更正块。

---

## §69.141 用户提问「CPU 渲染时代能进桌面，GPU 也驱动了却进不了」—— 换树回归分析

> **2026-11-15 22:1x 执行结果（§69.142）**：H 已上机。
> 「换树丢了 `XNU_WATCHDOG_POLICY` 宏」这一**事实判断成立**
> （重开后 21 项禁用完整生效）；但本节 §69.141.4–69.141.7
> 的 **[推断]「停 21 项是桌面开关 / 桌面消失是换树新增回归」
> 被证伪**：桌面、图标、设置仍全 0，自旋未消失，
> 相同时间截面下 userland 也没推进得更快。
> 阅读本节结论时请以 §69.142 为准。

用户 2026-11-15 21:54 提问。本轮**不刷机、不改代码、不重建
kernel**，纯 host 侧只读取证。结论：**差异不在渲染后端，而在
CPU 时代赖以进桌面的「启动收尾守护进程抑制」在换 GPU 用的
共享内核树时整条丢失**；GPU 显示链本身健康。

### 69.141.1 三份镜像的 boot-arg 明文（直接字节扫描）

| 参数 | CPU · lite（10-27 进桌面） | CPU · nofs1（11-03 进桌面） | GPU · fix163（当前） |
| --- | --- | --- | --- |
| `sheng-lite=1` | ✅ 1 | ✅ 1 | ❌ 0 |
| `sheng-disable-watchdogd=1` | ✅ 1 | ✅ 1 | ❌ 0 |
| `sheng-aks=1` | ✅ 1 | ✅ 1 | ✅ 1 |
| `sheng-data-persist=2` | ✅ 1 | ✅ 1 | ✅ 1 |
| `sheng-fb-gpu=1` / `gpu-metal=1` / `tum=swCD` | ❌ 0 | ❌ 0 | ✅ 各 1 |
| `sheng-staged-apps=1` / `autoreset=300` | ❌ 0 | ❌ 0 | ✅ 各 1 |

两份 CPU 时代镜像都是 `64811008 B`，当前 fix163 是
`77393920 B`。`sheng-lite-mask` / `sheng-lite-reset`
在 fix163 里的单次命中是 **kernel 代码字符串，不在 cmdline**
（`payload.json` 逐字列出，无这两项），不能误读为参数还在。

### 69.141.2 构建链差异（★ 分叉点）

- 老树 `UniXNU-muyu/scripts/legacy-10063/build-xnu-4k-baseline.sh`
  `:130,:781,:831`：`WATCHDOG_POLICY=${XNU_WATCHDOG_POLICY:-0}`、
  `-DXNU_WATCHDOG_POLICY=$WATCHDOG_POLICY` ⇒ `kern_exec.c`
  的 `sheng_boot_watchdog_policy(p, scratch)` 真被调用，每次开机
  写 `/private/var/db/com.apple.xpc.launchd/disabled.plist`。
- muyu-17 改用共享 `UniXNU` 树：
  `UniXNU/scripts/build-xnu-4k-baseline.sh:828` 只传
  `-DXNU_BOOT_PROGRESS=$BOOT_PROGRESS`，**从不定义**
  `XNU_WATCHDOG_POLICY`；`zzzzzzzz-braveark-watchdog-policy.patch`
  把调用点宏改成它 ⇒ `#if` 为假、调用点编译掉，函数体仍链接
  （`nm` 有符号造成假象，§69.134.1）。
- `board.toml` 现状：`sheng-lite=1`（:396/:508/:526）与
  `sheng-disable-watchdogd=1`（:413）全部注释/删除；
  `sheng-aks=1`（:399）、`sheng-data-persist=2`（:390）仍在。
- `sheng-aks=1` 仍在 ⇒ CPU 时代另一个根因（PreBoard 密码墙）
  **没有回归**。

### 69.141.3 运行时反证：21 项确实全开

fix160 / fix162 / R2 三轮 msgbuf 中带 pid 的 job：
`identityservicesd[55]`、`watchdogd[53]`、`locationd` 3 个 pid、
`bluetoothd` 最多 6 个 pid、`CommCenter`、`adid`、
`searchpartyd`、`familycircled`、`biomesyncd`、
`amsengagementd`、`passd`。CPU 时代这些应被 `disabled.plist`
拦下。§69.133.3 已在 recovery 只读扫 0–1 GiB，找不到
21 项禁用字典。

### 69.141.4 历史因果：桌面本来就靠停这 21 项

`HANDOFF-2026-10-27.md:1-16,46-56`：能进桌面有两个根因，
上午是 AKS，下午是 **`sheng-lite=1` 停掉 21 个空转守护进程**；
「少了会怎样」一栏直接写 **「启动停在 ~75 秒，桌面永远不来」**。
`package/BootWatchdogPolicy.c:28-31` 说明这些 job 在无网络、
无账号、空照片库的板子上空转，与 SpringBoard 抢 CPU/内存，
前四个是同步 XPC 等待环；禁用让 Mach service 查找 fail
而不是 hang。`K.7`（:656-660）另记：CPU 时代启动本身非确定，
唯一可靠配置就是**全停 21 个**的 lite 镜像。
当前 `launchd[1]` 6431/s、`SpringBoard[27]` ~3000/s、
`lsd[100]` 915/s × 49 s（§69.137.6 / §69.138）与「启动收尾
不收敛」同形；而 GPU 的 `ERR/SCAN/FBC/POIS/trust` 全健康且
延续到窗口末端 ⇒ 显示链不是瓶颈。

### 69.141.5 两个症状必须拆开

| | 启动收尾 | 系统 App 注册/安装 |
| --- | --- | --- |
| CPU 时代（全停） | 跑完 ⇒ **有桌面** | 仍缺 ⇒ 仅 3 个 App、设置打不开 |
| GPU 时代（全开） | 不收敛 ⇒ **卡启动页** | 仍缺（fix161/162/163 全阴性） |

⇒ 用户记忆里的「设置打不开、只有 3 个系统 App」是**两个时代
共有的旧缺口**，与禁用无关；「进不了桌面」是**换树新增的回归**。
fix161（Setup gate）、fix162（staged apps 复制）、
fix163/R2（first-boot migration）攻的都是前者，而挡路的可能是后者。

### 69.141.6 诚实边界

- 以上是**强相关 + 历史因果**的 [推断]，未在当前 GPU 树上做过
  「重开 policy + 停 21 项」的对照，不宣称已证明。
- 当前**没有**复现 CPU 时代「~75 s 后完全停止创建进程」：
  现在 `==>` 服务派发持续到 141–178 s，只是伴随高频自旋。
  形态相似但不相同。
- 不能完全排除 GPU 后端引入额外阻塞，但自旋集中在
  launchd / SpringBoard / lsd，不在 GPU 线程。
- `K.7` 的非确定性意味着即使恢复抑制，也须 3–5 轮取多数。

### 69.141.7 若要验证（H，需用户点头，本轮未做）

1. **仅重开宏不够**：`BootWatchdogPolicy.c:89-106` 在
   `enabled/jobs/lite/have_mask/lite_reset` 全缺省时
   `return 0`，什么都不写。当前两个 policy 参数都不在
   cmdline ⇒ 必须同时显式加回 `sheng-lite=1`
   （或 `sheng-lite-mask=0x1fffff`）。
2. 需要**重建 kernel**（`-DXNU_WATCHDOG_POLICY=1`），
   这是行为改动，不是 payload 级单变量。
3. cmdline 预算：当前 995 B + `sheng-lite=1 ` 13 B = **1008 B**
   ≤ `payload.py:38` 的 `> 1016` 硬闸，有 8 B 余量。
4. 判据：桌面/图标出现、`==>` 越过收尾、
   `lsd` / SpringBoard 自旋消失；至少 3–5 轮取多数。
   同时留意禁用会让设置依旧打不开（旧缺口不因此变好）。

### 69.141.8 状态

本轮纯 host 侧只读；`board.toml`、三源码、kernel、镜像、
设备状态**零改动**；设备 `a20248f7` 仍在 recovery，
原厂 vendor_boot 已还原。文档新增本节，§69.133.5 结论 2
加更正块。

---

## §69.142 fix164 — 候选 H 上机：重开 policy + `sheng-lite=1`（机制阳性，假设阴性）★

用户 22:01 选 `H`。目标：检验 §69.141 的推断
「CPU 时代能进桌面靠停掉 21 个守护进程；GPU 树把这条链
弄丢了，重新停 21 项应能恢复桌面」。

### 69.142.1 前置：零文件改动的宏注入方式（★ 可复用）

- `UniXNU/scripts/unixnu/kernel.py:193` `env = dict(os.environ)`
  ⇒ 环境变量透传给生成树脚本；`:202` `env.update(xnu_env(ctx))`
  只注入 defconfig 的 `CONFIG_XNU_*`，而
  `UniXNU-muyu-17/configs/*` **没有** `CONFIG_XNU_EXTRA_CFLAGS`
  ⇒ 不会被覆盖。
- 生成树 `UniXNU/out/muyu/tree/scripts/build-xnu-4k-baseline.sh:289`
  `EXTRA_CFLAGS=${XNU_EXTRA_CFLAGS:-<7 个 -Wno flags>}`，
  `:860` 再追加 `-DXNU_BOOT_PROGRESS=0`（不冲突）。
  因此用 `XNU_EXTRA_CFLAGS='<原 7 个 -Wno flags> -DXNU_WATCHDOG_POLICY=1'`
  前缀即可，**不改共享脚本、不改 patch、不改 defconfig、不改 board.toml**。
- `make` 不跟踪 CFLAGS 变化 ⇒ 构建前删除
  `build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o`
  强制重编，否则会拿到旧对象。
- 调用点 `kern_exec.c:8255` 嵌在 `XNU_OWNER_DATA_MOUNT` 内，
  Data mount 已启用；`payload.py` 只把 trust bundle **copy** 进 FAT
  （`:173-174`）、`check_native_root` 只用 `nm` 查符号 ⇒
  重建 kernel **不牵动 owner trust cache**。

### 69.142.2 单变量与离线校验

| 项 | fix163 R2 | **fix164** |
| --- | --- | --- |
| kernel sha | `eb116e6d…` | **`2d5815cb…`** |
| `sheng watchdog launch policy: %d` 串 | 0 | **1** |
| cmdline | 995 B | **1008 B**（恰好 `+ sheng-lite=1`） |
| FAT `kernel.macho` / `unixnu.toml` | — | **DIFF（预期）** |
| FAT trustcache / gpuaddon / vroot / scratch-data | — | **逐文件 SAME** |
| FAT `kernel.macho.adt` | — | 仅 64 B @372–435 随机种子 |
| vendor_boot 非 payload 区 | — | **逐字节 SAME** |

`payload.json` 中 `kernel_sha256` / `payload_kernel_sha256` /
`adt_sha256` 变化，`data_ramdisk_sha256`、`gpu_addon_sha256`、
`trust_cache_sha256`、`skip_setup`、`personality_rewrites` 全 SAME。
`payload.py:38` 硬闸 `>1016`，1008 B 通过、余 8 B。
产物 `fw/vendor_boot_gpu_fix164.img`（77,393,920 B）
sha **`51ae3d22…`**，日志 `logs/build-kernel-fix164.log` /
`logs/build-payload-fix164.log` / `logs/build-vendor-boot-fix164.log`。
`board.toml`、生成树脚本、四源码
（`BootLog.c` / `BootReset.c` / `BootLogStubs.c` /
`BootWatchdogPolicy.c`）经 `cmp`/sha 复核**零改动**。

### 69.142.3 ★ 机制阳性：policy 真的执行并成功了

patch 把成功路径的 printf 删了，`kern_exec.c` 只在
`policy_error != 0` 时打 `sheng watchdog launch policy: %d`
⇒ **日志 0 命中 = 返回 0**。更硬的是 21 项 job 的带 pid 启动数：

| job | fix160 | fix162 | R2 | **fix164** |
| --- | --- | --- | --- | --- |
| `identityservicesd` | 1 | 1 | 1 | **0** |
| `locationd` | 3 | 3 | 3 | **0** |
| `CommCenter` | 1 | 2 | 1 | **0** |
| `bluetoothd` | 6 | 6 | 2 | **0** |
| `searchpartyd` / `biomesyncd` / `passd` | 各 1 | 各 1 | 各 1 | **各 0** |
| **`watchdogd`（内部对照）** | 5 | 5 | 4 | **5** |

`watchdogd` 未加 `sheng-disable-watchdogd=1`，policy 本就不该
禁它 ⇒ 仍 5 个，证明「全 0」不是全局服务停止或日志截断，
而是 `disabled.plist` 精准生效。**§69.141 关于「换树丢了
`XNU_WATCHDOG_POLICY` 宏」的事实判断至此坐实。**

### 69.142.4 ★ 假设阴性：桌面仍不来，观测质量却是历轮最好

上机闭环 `218 s` 自动复位 / `190 s` 内核窗口 / `50` 轮 /
msgbuf **844 KB / 6237 行 / 96 条 `==>` / 725 容器 id**，
无需人工干预，原厂 vendor_boot 已还原、设备回 recovery。
日志 `logs/vendor_boot_gpu_fix164-logs/muyu-674d131a`。

- `IconState` / `desktop` / `com.apple.Preferences` /
  `Preferences.app` / `Settings.app` / `AppPlaceholders` /
  `staged_system_apps` / `System boot complete` / `Setup complete`
  **全 0**；`.app` 主进程仍 3 个
  （`SpringBoard[27]` / `imagent[62]` / `sociallayerd[184]`）。
  原始 `window.bin` 同样 0 命中，排除 decode 丢 slot。
- GPU 全绿：`MUYU ERR` 65 条全 `ist=0 hang=0 stallfault=0`、
  `trust=0` 67、`ZAPOK=1`、`SCAN` 319 / `FBC` 148，
  判据延续到 190 s。
- **自旋未消失，还扩散到 installd**：
  `launchd[1]` 5273/s @100.61 s、`SpringBoard[27]`
  3279/s @134.95 s、`lsd[95]` 859/s × 52 s @177.86 s、
  **`installd[176]` 3810/s @190.11 s**。
- **排除「窗口更长＝推进更远」的错觉**：相同时间截面
  `t≤141 s` 的 `==>` / 容器 id 为
  fix164 **54 / 497**、R2 **58 / 497**、fix160 **57 / 213**；
  `t≤178 s` 为 fix164 **64 / 713**、fix160 **75 / 857**。
  ⇒ 21 项被禁**没有**让启动收尾跑得更快，`installd` /
  `installcoordinationd[198]` / `trustd[209]` / `budd[180]` /
  `storekitd[213]` 落在 178–190 s，只是别轮窗口没覆盖到
  （fix160 也有 `installd` 与 `installcoordinationd` 各 1）。
- 时间线：`MOUNT /private/var` 91.11 s → `load_init_program`
  91.40 s（**0.29 s**，stage=3 跳过复制，与 R2 一致）；
  `Early boot complete` 120.91 s；`migrationpluginwrapper` 5 次
  @135.45 s；`budd[180]`（Setup Assistant）@179.64 s，
  `Setup complete` 仍 0。`isCdhashInTrustCache` 30 次均返回
  `0x0`，`AppPlaceholders` / `IconState` 仍 0 ⇒ 安装/图标链
  仍未推进。

⇒ **§69.141.4–69.141.7 的 [推断] 证伪**：单独恢复 21 项
禁用不足以让桌面出现；「桌面消失是换树新增回归」的归因
不成立（宏丢失是真的，但不是桌面消失的充分原因）。
§69.133.5 原结论 2「与 21 项禁用无关」在**桌面层面**
反而是对的，但理由是这次实测，不是 fix160 那次无效对照。

### 69.142.5 下一轮候选（需用户点头，本轮未做）

1. **I（首选，单变量、零 kernel 改动）**：fix164 基线
   保留 policy 宏 + `sheng-lite=1`，仅把 `--data-ramdisk`
   换回 `fw/data-seed/muyu-data-200g-skipsetup.dbr`。
   在「21 项真被禁」的新基线上重测 A/fix161 的 Setup gate；
   `budd` 179.64 s 才启动、`Setup complete` 0，是最靠近的
   未闭合门。cmdline 不变 1008 B。
2. **J（零风险、不上机）**：只读挂载 root DMG 查
   `SystemAppMigrator` 的 launchd 触发 plist 与
   `DefaultIconState~ipad.plist` 的加载条件，解释
   `AppPlaceholders` / `IconState` 为何始终 0 命中。
3. **K（观测通道）**：把窗口拉过 300 s，检验
   「`budd` 179.6 s 才起、Setup 只是还没走完」；当前
   218 s 就复位，需先查复位来源（不是 `sheng-autoreset=300`）。
4. **不做**：不动 GPU 驱动 / 表 / SMMU / zap、不改 tick /
   watchdog / `sheng-autoreset`、不放宽判据。

### 69.142.6 状态与产物

- 备份：`/tmp/fix163-kernel.macho`、`/tmp/fix163-payload.{img,json}`、
  `/tmp/board.toml.fix163-baseline`、
  `/tmp/build-xnu-4k-baseline.sh.tree-fix163`、
  `/tmp/fix164-vendor-boot.img`；fix163/R2 镜像与日志未改动。
- 当前 `UniXNU/out/muyu/kernel/kernel.macho` = **`2d5815cb…`**
  （带 policy 调用点），`out/muyu/payload` = fix164。
  若要回到 fix163 基线，用 `/tmp/fix163-kernel.macho` 覆盖即可。
- 设备 `a20248f7` 在 recovery，原厂 vendor_boot 已还原，
  无残留挂载 / 进程。
- 文档：新增本节；§69.141 顶部加「H 已执行、推断证伪」
  更正块。

---

## §69.143 用户方案「先让能进桌面的配置加载完，再切回当前配置」—— 可行性核查

> **2026-11-15 22:47 执行状态更正**：L1 已按本节 §69.143.4 路径 1 执行，
> 但 **Step 1 阴性、Step 2 主动拦截未执行**（详见 §69.144）。
> 机制核查（同一 seed、同一窗口、跨配置可继承）仍然成立且有设备级实证；
> 不成立的是「CPU 配置一定能在这份 Data 卷上进桌面」这一前提。

用户 2026-11-15 22:17 提出：随便找一份能进桌面的配置，让系统**真正加载完**，
然后切回当前 GPU 配置；依据是「系统跑在 RAM，但设置是持久的」。
本轮**纯 host 侧只读 + 设备只读 dd**，零刷机、零文件改动（除备份与文档）。
**结论：机制成立，且有设备级硬证据；但「真正加载完」含一个人工交互门，
需用户选路径后再执行。**

### 69.143.1 决定性前提：两个时代共用同一把 Data 窗口钥匙

| 证据 | 结果 |
| --- | --- |
| `mtype` 逐字节抽 FAT `scratch-data.img`（lite / nofs1 / fix163 / fix164） | 四份**逐字节相同** = `fw/data-seed.img`，1,093,760 B，sha `476464c3…` |
| 设备只读 `dd sda32 skip=6295994 count=1` 的窗口头 | `SHNGDATA` v1、pages=52,428,800、generation=5、CRC 有效；**seed = `508c7c94…`** |
| `508c7c94…` 的身份 | 正是本代 seed 的 seedIdentity（§69.135「两个 skipsetup 变体同源」；HANDOFF-2026-11-07:2126/2194/2253 记「种子内容变了 ⇒ identity 由 `508c7c94…` 变 `6a68665e…`」）。**不是文件 sha**：`NativeRamBlock.cpp:240-254` 的 `seedHash` 是对解包后逻辑卷逐 4096 页做 SHA256 |
| `checkHeader` 判定 | `seed` 与 `pages` 全匹配 ⇒ 返回 0 ⇒ `Open::Resumed`、`active=true`、`direct=true`（mode 2），窗口读写生效 |
| `sheng-data-persist=2` | 四份镜像 unixnu.toml 均有 ⇒ store mode 2（读写），非只读探针 |
| 窗口几何 | 两代内核 `native_ufs_store_open` 函数体与 `kLogReserveBlocks=8192` / `kExportBlocks=65536` / `kDefaultReserveMiB=1024` **逐字相同**；`blocks=52430401` 全历史一致；设备实测 header 落在 LBA 6,295,994（由日志窗口 58,980,347 反推 `last+1-reserve-blocks` 精确命中）|
| 兼容层 | autoloop 默认 UEFI `fw/boot-17line-043f7120.img` 正是 17-line / 62 MiB payload 专用版（`muyu-flash-mode.sh:41-45`），CPU 时代镜像 61.81 MiB ⇒ **同一 UEFI 即可**；三份镜像 ADT 均无 `filesystems` 节点；原始根镜像 `fw/userdata-backup/userdata-root.sparse`（8.9 GB）仍在 |

⇒ **CPU 配置写进 Data 卷的状态，GPU 配置开机时会 `Resumed` 原样继承。**
用户对「设置是持久的」的观察得到设备级证实：bitmap 显示
**197,784 个 present 页 = 772.6 MiB** 已改写数据（备份见 `logs/datastore-backup-20261115/`）。
这也再次限定 §69.139.1：`Kept` 只发生在换 seed 的 fix161/162 轮次；
fix163 R1/R2 与 fix164 都是 `Resumed`。

### 69.143.2 为什么这条路值得试：它绕开了 GPU 配置未闭合的门

- GPU 配置至今 `Setup complete`=0、`budd` 最晚 179.64 s 才起、
  `IconState` / `desktop` / `Preferences` 全 0（§69.142）。
- fix161（skipsetup seed）当时窗口是 `Kept`、**Data 纯从 seed 跑**，
  所以「Setup gate」实验其实从未真正测过「继承一份跑完 Setup 的 Data 卷」。
- 用户方案等价于：把「Setup 完成 / 图标布局 / 系统 App 注册」这些
  **一次性初始化**交给已验证能进桌面的 CPU 配置做完，GPU 配置只继承结果，
  不再自己跑 first-boot。

### 69.143.3 执行门：Setup Assistant 需要人工点（或换 seed）

> **23:03 更正 / 23:12 再更正（详见 §69.145.3、§69.146）**：23:03 曾据
> `UniXNU-muyu/docs/GPU-ONDEVICE.md:300`（nofs1 基线「按键可用而触摸失效」
> 是两条独立链路）判本节的交互门「不可执行、不得串联 Step 2」——
> **这个结论下得太绝对，已撤回**。用户 23:12 指出：11 号那次同样报触摸
> 用不了，**第二轮上机触摸又自己好了**（原因不明），与 §K.7 的启动
> 非确定性同源 ⇒ 单轮触摸阴性不能当稳定前提。且 Setup 状态存在 Data 卷里、
> 跨配置会 `Resumed` 继承，**不要求必须在 CPU 配置里走完 Setup**：
> Stage 2 起来后若触摸恢复，可在 GPU 配置下继续点完。
> 故按用户决定直接试 Stage 2（§69.146）。

- `HANDOFF-2026-10-27.md` §H/I：`sheng-lite=1` 后 **Hello 白底黑字出现**
  → 需触摸点「继续」→ Setup 问「抹掉还是保留」→ **选【保留】** → 走完 Setup → 桌面。
  `make-data-seed.py:76-87` 确认 base seed **没有** `SetupDone` /
  `SetupFinishedAllSteps` / `com.apple.migration`（只在 `--skip-setup` 分支写）。
  ⇒ **autoloop 无人值守跑不完这一步**；`sheng-lite` 还停掉了
  `mobile.obliteration`，UI 上点「抹掉」会卡死（§I.1）。
- 若改用 skipsetup seed 自动跳过 Setup：`seedIdentity` 会变，
  窗口变 `Kept`，GPU 配置**必须同步换成同一 skipsetup seed** 才能继承
  （即重建 fix164 的 payload，不再逐字等于「当前配置」）。
- 旧树 `BootLog.c:340-409` 有 `sheng-autoreset` 链路（写 PMIC cell +
  arm hypervisor watchdog），nofs1 kernel 内 `sheng-autoreset` 字符串 1 次、
  `bootloader` 6 次；给 CPU 镜像 cmdline 加 `sheng-autoreset=240`
  （934 → 958 B）可让 autoloop 自动闭环，无需长按电源。
  该参数只改 FAT 内 `unixnu.toml`，kernel / ADT / trust / seed 全不动。

### 69.143.4 两条待选路径（本轮均未执行）

1. **L1（保真，需一次人工走 Setup）**：`fw/vendor_boot_nofs1.img`
   （11-03 用户确认进桌面/锁屏、稳定 >1 min、无自动重启）+ base seed
   → 触摸走完 Setup（选【保留】）→ 自动复位 / 还原 →
   刷 `fw/vendor_boot_gpu_fix164.img`（**逐字等于当前配置**）→ `Resumed` 继承。
   单变量：只有 vendor_boot 与 Data 卷状态。
2. **L2（全自动）**：CPU 配置 + skipsetup seed 跑完 → 窗口 identity 变
   → GPU 配置也换 skipsetup seed 重建 payload → 继承。
   全程 autoloop 无人值守，但改动面更大，且继承了 fix161 的旧变量。

### 69.143.5 风险与状态

- Step 1 会**覆盖当前窗口里 772.6 MiB 的 GPU 时代半成品 Data**。
  **已完整备份、可逐段写回**：bitmap 只有 **15 个连续段**，
  `logs/datastore-backup-20261115/` 内 `header.bin`（4 KiB）、
  `bitmap.bin`（6.25 MiB）、`meta.json`、`runs.json`（每段的
  page / lba / 字节数 / sha256）与 `pages/run_00..14.bin`
  共 **810,123,264 B 全部读全**（`complete=true`）。
  写回公式：绝对 LBA = `6295994 + 1 + 1600 + page`。
  另可用 `sheng-data-reset=2` 回干净 seed 作为兜底。
- CPU 时代启动本身非确定（§K.7：3–5 轮取多数），L1 可能需要 2–3 轮。
- 即使继承成功，「设置 App 打不开」是两个时代共有的旧缺口，不因此变好。
- 本轮零改动：`board.toml`、四个源码、kernel、`out/muyu/payload`、
  全部镜像、设备状态均未变；设备 `a20248f7` 仍在 recovery，
  原厂 vendor_boot 已还原，无残留挂载 / 进程。

## §69.144 L1 Stage 1 上机：假进度条复现与「CPU 配置也没跑完启动收尾」（2026-11-15 22:31–22:47）

用户 22:40 反馈「stage1 进度条在走，但更像以前提到的假动作」。
确认后于 22:42 主动拦截 Stage 2，避免继承一份未走完 Setup 的 Data 卷。

### 69.144.1 假进度条 = 已知现象，本轮拿到设备级实证

- `HANDOFF-2026-10-27.md:499-500`、
  `_archive/notes/handoffs/HANDOFF-2026-10-28.md:317`、
  `UniXNU-muyu-17/AGENTS.md:156`、`tools/muyu-flash-mode.sh:182`：
  进度条按帧推进，显示只有 ~1.4 fps，跑满约 20 分钟，
  与启动进度无关；10-28 的 D1–D7 二分因此「不可信」。
- Stage 1 日志实证：`MUYU DATA HB` 的 `sw` 从 `3866@450`
  一路涨到 `10588@850`（+6722 帧 / 400 s），而同一批心跳里
  `n=203~204`、`aks=90/616`、`sbp=144`、`mig=122`、`dm=0`、`pb=0`
  **全部冻结**。⇒ 用户判断正确：进度条在走是恒定动画，不是判据。
- 唯一可信的屏幕判据是 Setup Assistant 的白底 Hello；
  用户确认 **从未出现 Hello，只有进度条 / Apple logo**。

### 69.144.2 Stage 1 日志（观测质量足够，排除截断）

- `logs/vendor_boot_stage1-nofs1-logs/muyu-66c966b9`：
  `SHLOG001` v4、writer=768 s、msgbuf_reaches=850 s、
  slots=385/385、rounds=385、`write_errors=0`、`gpt=0/0`、
  state=2（正常写）、msgbuf 131072 B / 213 行 / ts 128.01–850.13。
  msgbuf 是循环缓冲，<128 s 的早期启动行已被覆盖，
  故「store open / seed 校验」类判据不从 msgbuf 取，改由窗口头取证。
- 进程趋势：`MUYU DATA new` 最后一批 @189.83 s（PID 216–225），
  之后 660 s 无新增；心跳 `n=204` 稳定、`ld=R bb=R sb=R`；
  `==>` 仅 39 行，最后一条 @752.12 s（`betaenrollmentd[246]`、
  `powerdatad[245]`）。`audiomxd[229]` @639.6 s 被 jetsam 杀
  （`InactiveHard 50 MB`）。⇒ 与 10-27 §H.1「n=198 之后 20+ 分钟
  不再创建进程」同形：**启动收尾真的停住，不是日志丢失**。

### 69.144.3 ★ 反直觉：禁用了、budd 也起来了，但 Hello 没出现

- nofs1 kernel 里 `sheng watchdog launch policy: %d`、
  `disabled.plist`、`sheng lite mask: 0x%x` 各 1 次 ⇒ 旧树
  `XNU_WATCHDOG_POLICY` 宏确实编入（与 §69.141 的 GPU 树相反）；
  Stage 1 cmdline 含 `sheng-lite=1`（954 B，相对 nofs1 仅多
  `sheng-autoreset=600`）。日志里 21 项 lite job 几乎全 0
  （仅 `fairplayd` 字符串 2 次、无 pid）。⇒ **21 项确被禁用**。
- 但 `/System/Library/PrivateFrameworks/SetupAssistant.framework/budd[233]`
  @446.20 s 确实被派发（`==> (apply-protobox)`）。
  ⇒ Setup Assistant 进程起来了，UI 却没渲染出来，与 GPU 时代
  `IconState` / `desktop` / `AppPlaceholders` 全 0 可能同源
  （系统 App 注册 / 图标布局），但本轮无直接证据，标 **[推断]**。
- ⇒ **这次卡住不是「禁用进程没做」造成的**。对照 §69.141/§69.142：
  GPU 树丢宏是真实回归，但重开宏也没能出桌面；
  现在 CPU 树上 policy 生效、`sheng-lite=1` 生效，同样卡住。
  最强嫌疑 [推断] 是「从 GPU 轮次写过的 772.6 MiB 半成品 Data 卷启动」
  （10-27 成功时是从较干净的卷启动）；另不能排除 §K.7 的启动
  非确定性（单轮阴性不定论）。

### 69.144.4 ★ 继承机制成立（设备级），但继承到的是失败状态

- 只读读回窗口头：`SHNGDATA` v1、generation **5 不变**、
  seed `508c7c94…` 不变、CRC 有效、header sha 与备份一致。
- bitmap：present 页 **197,784 → 217,659**（772.6 → **850.2 MiB**，
  **+77.6 MiB**），2493 个字节不同。
  ⇒ 旧树确实在读写**同一份**窗口，跨配置继承机制成立，
  不是 §69.139.1 的 `Kept` 只读路径；generation 不变说明
  它只在新建窗口时递增，与 `Resumed` 一致。
- 但写进去的是「卡在 Setup 之前」的半成品，不是「走完 Setup」的成熟状态。
  ⇒ 此时跑 Stage 2 只会继承更脏的 Data，无判据价值。
  故 22:42 把 `fw/vendor_boot_gpu_fix164-inherit.img` 临时改名
  `.img.HOLD` 主动拦截；22:46:59 `stage2 rc=1` / `L1 done`，
  **Stage 2 未执行**。22:47 已撤销改名，两文件 sha 同为 `51ae3d22…`。

### 69.144.5 autoreset=600 未生效（新发现）

- 22:31:31 启动 → 22:45:51 回 fastboot 共 861 s，期间设备既不在
  fastboot 也不在 adb；设备**由用户长按电源 10–15 s** 才复位。
  日志里 `[reset]` / `autoreset` / watchdog 相关行 0 命中，
  decode 的 `== reset lines` 为空。
- 旧树 `UniXNU-muyu/package/BootLog.c:340-409` 与 `boot_log_tick`
  的 autoreset / watchdog 都以 `boot_reset_probe() == 0` 为前提；
  probe 失败时 writer 只写 `autoreset skipped`、watchdog 也不 arm。
  ⇒ **[推断] PMIC cell 探测失败**。后续跑 CPU 镜像要么人工复位，
  要么先修 probe，不能依赖无人值守闭环。
- host wait 1200 s 原本会在 22:51:31 die 并提示长按电源。

### 69.144.6 结论与下一轮候选（需用户点头）

- **L1 Step 1 阴性**：CPU 配置 `nofs1 + sheng-lite=1` 在
  「被 GPU 轮次写过的 Data 卷」上同样没跑完启动收尾，Hello 从未出现
  ⇒ 用户方案的第一步「先让能进桌面的配置加载完」在本轮状态下
  **未达成**，整个 L1 尚未被检验（Step 2 未执行）。
- **L1′（首选）**：Stage 1 加 `sheng-data-reset=2`（954+21=**975 B**，
  硬闸 1016 B 余 41 B），从干净 overlay 开始，排除
  772.6 / 850.2 MiB 半成品 Data 这个干扰变量，看能否复现 10-27 的 Hello。
  代价：窗口被重置；L1 之前的完整备份（**810,123,264 B / 15 段**）仍在
  `logs/datastore-backup-20261115/`，可逐段写回
  （绝对 LBA = `6295994 + 1 + 1600 + page`）。
- **J（零风险不上机）**：只读 root DMG 查 `SetupAssistant` /
  `IconState` / 默认图标布局的加载条件，解释「budd 启动却无 UI」。
- **L1 重跑**：不动参数再跑 1–2 轮验证 §K.7 非确定性，
  但每轮需人工复位、约 14 分钟，成本高。

### 69.144.7 状态

- 设备 `a20248f7` 在 recovery，原厂 vendor_boot 已还原，
  无残留挂载 / 进程；L1 后台脚本已结束。
- 零改动：`board.toml`、四个源码、kernel、payload、所有既有镜像
  （Stage 1 镜像是本轮新建的单变量产物）均未变；`cmp` + sha 复核通过。
- 新增产物：`fw/vendor_boot_stage1-nofs1.img`（sha `6a4d56d2…`，
  相对 `nofs1` 仅 `unixnu.toml` 多 `sheng-autoreset=600`，
  cmdline 934→954 B，kernel/trust/seed/ADT 逐字节 SAME）、
  `logs/autoloop-l1.log`、`logs/vendor_boot_stage1-nofs1-window.bin`、
  `logs/vendor_boot_stage1-nofs1-logs/muyu-66c966b9`、
  `logs/datastore-backup-20261115/stage1-after/{header.bin,bitmap.bin,meta.json}`。
- 文档：新增本节；§69.143 顶部加执行状态更正块。

## §69.145 用户现场反馈「看到 Hello 了，但触摸掉了，却按键可用」—— L1′ 机制阳性、交互门阴性（2026-11-15 23:03）

### 69.145.1 ★ 阳性：Hello 首次出现，干净 overlay 假设成立

- Stage 1′ 22:55:19 刷入 `fw/vendor_boot_stage1p-nofs1.img`
  （sha `18356079…`；cmdline 982 B，相对 Stage 1 只多
  `sheng-data-reset=2`），23:03 用户确认 Setup Assistant 的
  **白底 Hello 页面出现**。这是今晚第一次拿到「Setup UI 真的渲染」
  的设备级证据。
- §69.144.3 里标 `[推断]` 的「budd 起来了却无 UI，嫌疑是被 GPU
  轮次写过的 772.6 / 850.2 MiB 半成品 Data 卷」得到强支持：
  `UniXNU/drivers/RamBlock/NativeRamBlock.cpp:79-82` 明确
  `sheng-data-reset=1/2` 触发 `discardRequested()`，旧 overlay 页
  被丢弃（`Open::Fresh`，从干净 seed 重建窗口）；同时
  `UniXNU-muyu/package/BootWatchdogPolicy.c:92-99` 只 unlink 两份
  purplebuddy plist 并打印 `sheng setup flags reset: Hello page on this boot`。
- **待复位取证（本轮尚未读回，不写成结论）**：窗口头
  generation / seed 是否变化、`pb` 是否仍 0、`MUYU DATA new`
  是否越过 Stage 1 的 189.83 s、`sheng setup flags reset`
  是否出现在 msgbuf、触摸泵与 `IOHIDEventDriver` 计数。

### 69.145.2 ★ 触摸掉不是本轮回归，是两条独立链路的基线属性

- 按键与触摸在这套移植里本来就分开：
  - 按键 `muyu-buttons=1` + `offline-key-events=1`（软件注入，
    `ButtonProbeMuyu.cpp`，`HANDOFF-2026-10-28.md:310` 实测两键可用）；
  - 触摸 `muyu-touch-drv=1` / `sheng-touch-hid=1` /
    `offline-hid-input=1`（`NativeNovatekTouch`，SPI/Novatek）。
- Stage 1′ cmdline 逐字含全部触摸开关（943 B 基线里就有），
  `muyu-touch-quiet=1` **只静音触摸泵日志、不停泵**
  （`HANDOFF-2026-10-26-LOCKSCREEN.md:86`、
  `HANDOFF-2026-10-28.md`）。⇒ 不是 1016 B 硬闸挤掉了参数，
  也不是 `sheng-data-reset=2` 关掉了触摸。
- `UniXNU-muyu/docs/GPU-ONDEVICE.md:300`（11-11，**用户自己确认**）
  已定性：「按键可用而触摸失效属基线既有独立链路
  （`offline-key-events` vs `NativeNovatekTouch`），不归因 GPU」；
  `:447` 亦记 `offline-key-events=1` 在 `nofs1` 基线上按键可用。
  ⇒ 今天的观测与该记录一致，**不是 L1′ 引入的回归**。
  **但 23:12 用户补充**：11 号那次报「触摸用不了」时，
  当时的 AI 没有纠结、直接做下一步，**第二轮上机触摸又能用了**，
  原因不明 ⇒ 该记录本身也只是单轮观测，不能当稳定属性，
  与 §K.7「同镜像既成功又失败」同形。
- 但 10-26 触摸曾实测可用（`HANDOFF-2026-10-25-TOUCH4.md:234`
  `disp=1217/1217`、`HANDOFF-2026-10-26-LOCKSCREEN.md:5`
  锁屏上滑弹 Enter Passcode；配置 `sheng-touch-hid=1` 且
  **不要** `sheng-touch-split=1`）。⇒ 退化发生在 10-26→11-11
  之间，或与 §K.7 的启动非确定性同源；**本轮无法区分**，
  标 `[推断]`，需同镜像复测。

### 69.145.3 ★ 方法论更正：L1 / L1′ 的执行门漏了一个前提

- §69.143.3 / §69.143.4 把 Step 1 定义成「Hello → 需触摸点
  『继续』→ 选【保留】→ 走完 Setup → 桌面」，只核对了
  「skipsetup seed 会让窗口变 `Kept`」，**没有核对这台设备的
  触摸能否用于交互**。而   `GPU-ONDEVICE.md:300` 是 §69.144 之前
  就存在的记录。⇒ 23:03 曾据此判「交互门从设计上不可通过」，
  **23:12 撤回**：用户指出 11 号同形态的触摸失效在**第二轮上机
  自行恢复**（原因不明，与 §K.7 启动非确定性同源），说明触摸
  可用性是逐轮变量，单轮阴性不构成稳定前提。保留的事实只有：
  这一轮没点成。
- 直接后果：`pb` 判据是
  `mu_data_size("/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist") >= 0`
  （`UniXNU-muyu/package/MuyuDataProbe.c:349`），Setup 走不完
  就不会写；**本轮 `pb=0→1` 预期拿不到**，成熟 Data 状态仍未达成。
  23:03 曾据此写「**Stage 2 不应串联**」，**23:12 撤回**：用户
  「完不成设置也不一定 stage2 不能开展」成立 —— Setup 进度、
  图标布局、系统 App 注册都是 Data 卷状态，GPU 配置 `Resumed`
  后原样继承；即使 Stage 1′ 停在 Hello，Stage 2 继承的是一份
  **从干净 seed 真正跑到 Setup UI 的数据**，比 §69.144 的
  850.2 MiB 半成品更接近成熟，值得一试；且若 Stage 2 这一轮
  触摸恢复，可直接接着点完 Setup。故按用户决定执行 Stage 2。
- 历史死路复核（`HANDOFF-2026-10-26-MIGRATION.md:94`、
  `HANDOFF-2026-10-26-LOCKSCREEN.md:24-63`、
  `HANDOFF-2026-10-26.md:228-240`）：改 Data 种子的
  migration / skip-setup 偏好、`--skip-setup` 官方路径、
  skipsetup seed 在这台设备上分别卡苹果 logo / 连 logo 都没到；
  §69.143.4 的 L2 还带 fix161 的 `Kept` 变量。
  ⇒ 不能用「换个 seed 跳过 Setup」简单绕过。
  **但 `reset=2 Hello` 当年也被列为死路、今天被本轮推翻**，
  说明这份清单的结论需按当前 Data 状态重新检验，不能照抄。

### 69.145.4 当前状态与下一步候选（需用户点头，本轮未执行）

- 设备 `a20248f7` 23:03 仍在 Stage 1′ 的 RAM 引导里（Hello 页），
  `adb devices` / `fastboot devices` 均无设备。后台
  `tools/muyu-autoloop.sh fw/vendor_boot_stage1p-nofs1.img 1500`
  （PID 53455，22:55:19 起）仍在等内核自己回 fastboot，
  约 23:20:19 超时。旧树 `sheng-autoreset=600` 未生效
  （§69.144.5 PMIC cell probe 失败），**仍需用户长按电源
  10–15 s 复位**才能拿到 msgbuf。
- 候选（推荐顺序）：
  1. **A（零风险，先做）**：现场顺手试 Home / 电源键三击
     （`offline-hid-home=1` 已编入，`HidInputProbe.cpp:169`），
     看 Hello 页有无「Press Home to continue」或 VoiceOver 分支；
     随后长按电源复位，读回 `logs/autoloop-l1p.log` + msgbuf +
     窗口头，把 §69.145.1 的「待取证」项全部落实。
  2. **B（正解，需一轮镜像）**：修触摸，定位失效层
     （内核泵 / `IOHIDEventDriver` 派发 / backboardd 消费），
     以 10-26 的 `disp=1217/1217` 配置为已知好基线；
     修好才能真正走完 Setup 拿到 `pb=1`。
  3. **C（绕过交互，判据打折）**：host 侧往 Fresh overlay 写
     purplebuddy 等四份 Setup 完成偏好（复用 `PersistOverlay`
     逐段写回，不改 seed），避开「改 seed 卡 logo」；
     但属于伪造 Setup 完成，且历史标注四份偏好「没有完成
     本机激活状态处理」。
  4. **D**：§69.143.4 的 L2 全自动，改动面最大，历史死路最多。
- 本轮零改动：未刷机、未改 `board.toml` / 源码 / kernel /
  payload / 任何既有镜像；未串联 Stage 2；仅新增本节与
  §69.143.3 顶部的更正块。
- **23:12 用户决定覆盖上述候选 A/B 的顺序**：直接试 Stage 2（§69.146）。

## §69.146 Stage 2 试跑：GPU 配置继承 Stage 1′ 的干净 Setup 数据（2026-11-15 23:12 起）

### 69.146.1 用户纠正与决定（本节的动机）

- 用户原话要点：①「完不成设置也不一定 stage2 不能开展」；
  ② 11 号报「触摸用不了」时那个 AI 没在意、直接做下一步，
  **第二轮上机触摸又能用了，原因不明**；③「总之，试一下 stage2」。
- ⇒ 撤回 §69.145.3 / §69.143.3 更正块里
  「交互门从设计上不可通过 / Stage 2 不应串联」的绝对结论。
  触摸可用性是**逐轮变量**（与 §K.7 非确定性同形），
  单轮阴性不构成稳定前提。
- 更关键的逻辑：Setup 进度、图标布局、系统 App 注册都是
  **Data 卷状态**，不是配置状态；Stage 2 `Resumed` 后原样继承。
  所以「走完 Setup」不一定要在 CPU 配置里完成 ——
  Stage 2 起来后若触摸恢复，可在 GPU 配置下接着点完。
  这正是用户原始方案（§69.143）比 §69.143.3 执行门更宽的形态。

### 69.146.2 Stage 2 的定义与前置（离线复核）

| 项 | 值 |
| --- | --- |
| Stage 2 镜像 | `fw/vendor_boot_gpu_fix164-inherit.img`，77,393,920 B，sha `51ae3d22…`；与 `fw/vendor_boot_gpu_fix164.img` 是**同一硬链接**（22:47 已撤销 `.img.HOLD` 拦截，两文件 sha 相同） |
| 相对当前 GPU 配置 | **逐字相同**（§69.143.4 路径 1 要求「逐字等于当前配置」） |
| seed 共用 | §69.143.1：lite / nofs1 / fix163 / fix164 四份 `scratch-data.img` 逐字节相同 = `fw/data-seed.img`（1,093,760 B，sha `476464c3…`），seedIdentity `508c7c94…`；设备窗口头 CRC 有效、`checkHeader` 返回 0 ⇒ `Open::Resumed`、`direct=true` |
| `sheng-data-persist=2` | 两代均有 ⇒ store mode 2 读写 |
| UEFI | 同一份 `fw/boot-17line-043f7120.img`（61.81 / 77.39 MiB 两代 payload 通用，§69.143.1） |
| `sheng-data-reset` | **Stage 2 没有** ⇒ 不会再 `discardRequested()`，不会二次清窗口 |
| 单变量 | 只有 vendor_boot 与 Data 卷状态 |

### 69.146.3 串联方式：等 Stage 1′ 收尾，再自动刷 Stage 2

- 23:12 设备 `a20248f7` 仍在 Stage 1′ 的 RAM 引导（Hello 页）：
  `adb devices` / `fastboot devices` 均无设备。
  Stage 1′ autoloop（PID 53455，22:55:19 起，timeout 1500 s）
  仍在 `waiting for the kernel's own restart into fastboot`。
- `tools/muyu-autoloop.sh` 的行为（已逐行核对）：
  1. 开头若不在 fastboot 但 adb 有 recovery ⇒ `adb reboot bootloader`；
  2. flash + RAM boot → 等设备离开 fastboot → 等内核自己回 fastboot；
  3. **捕获 fastboot 后自动**：还原 stock vendor_boot → `reboot recovery`
     → 拉 32 MiB 日志窗口 → 解码 → 打印 `[reset]` 行 → 结束。
- ⇒ 用户长按电源 10–15 s 复位（旧树 `sheng-autoreset=600`
  不生效，§69.144.5）后，Stage 1′ autoloop 会**自动完成取证**
  并把设备放回 recovery；此时再启动 Stage 2 autoloop，
  开头会自动 `adb reboot bootloader`，**无缝衔接**。
- 已挂后台串联门（`logs/autoloop-l1p-stage2.log`）：
  等 PID 53455 退出 → 等 `adb devices | grep -qw recovery`
  → `tools/muyu-autoloop.sh fw/vendor_boot_gpu_fix164-inherit.img 1500`。
  前置不满足时脚本报错退出，**不会瞎刷**。
  若用户不复位、Stage 1′ 在 23:20:19 超时 die，串联门会因
  等不到 recovery 而退出（安全失败），需人工复位后重跑。

### 69.146.4 判据（不预设 Stage 1′ 必须 `pb=1`）

1. **窗口身份**：Stage 2 起来后窗口头 generation / seed 应与
   Stage 1′ 复位后**一致**（`Resumed`，不是 `Fresh` / `Kept`）。
   若变 `Fresh` ⇒ seed 或 pages 变了，继承没发生。
2. **GPU 配置的老缺口**（§69.142：`Setup complete`=0、
   `IconState` / `desktop` / `Preferences` 全 0）：
   Stage 2 的 `MUYU DATA HB` 里 `pb` / `n` / `aks` / `sbp` /
   `mig` 是否推进，屏幕能否进桌面 / 锁屏、图标与设置 App 是否出现。
3. **Setup 可继续性**：Hello 页是否还在（Data 继承），
   **触摸这一轮是否恢复**（11 号先例）；若恢复，用户可直接在
   GPU 配置下点「继续」→ 选【保留】→ 走完 Setup，看 `pb=0→1`。
4. **观测质量**：writer / slots / `write_errors` / msgbuf 覆盖范围，
   排除把日志截断误判成阴性（§69.128 的教训）。

### 69.146.5 风险与诚实边界

- 硬复位可能来不及 flush 内存脏页；但 mode 2 `direct` 是
  改页直写窗口、不走 RAM 缓存，大部分应已落盘 —— **[推断]**，
  以窗口头 CRC 与 bitmap 实际读回为准。
- 本轮**只回答「继承一份跑到 Setup UI 的干净 Data 后，GPU 配置
  能否更进一步」**，不回答「修触摸」「禁用进程」「系统 App 注册」
  这些各自的根因。
- 单轮阴性不定论（§K.7：CPU 时代 3–5 轮取多数；11 号触摸
  第二轮自愈是先例），必要时同镜像重跑。
- 23:12 状态：**尚未刷入 Stage 2**，等用户长按电源触发串联门。
  源码 / kernel / payload / 既有镜像一律未改动。
  （23:16 已刷入，见 §69.146.7。）

### 69.146.6 Stage 1′ 收尾取证（23:15:36–23:15:58，用户长按电源复位后自动完成）

- `logs/autoloop-l1p.log`：`back in fastboot after 1218s` →
  restore stock → reboot recovery → pull 32 MiB 窗口 → decode，
  **`rc=0`**。`logs/vendor_boot_stage1p-nofs1-logs/muyu-66acc8e5`：
  `SHLOG001` v4、writer=1126 s、msgbuf_reaches=1213 s、
  slots=564（kept 509）、rounds=564、`write_errors=0`、
  `gpt=0/0`、state=2 ⇒ **观测质量足够，排除截断**。
- ★ **反直觉：`pb=1` 全程**（30 帧 HB，t=489–1209 s，无 0）。
  对照 Stage 1（§69.144.1）`pb=0` / `sbp=144` / `mig=122` /
  `aks=90/616` / `n=204`；Stage 1′ `pb=1` / `sbp=1965` /
  `mig=190` / `aks` 147/1622→154/1753 / `n=119–128`。
  ⇒ 干净 seed 上的 Data 卷内容与 850.2 MiB 半成品**显著不同**：
  purplebuddy.plist 存在、springboard.plist 144→1965 B、
  migration.plist 122→190 B、AKS 活动近乎翻倍、进程数少近一半。
  **这直接支持用户「完不成设置也值得试 Stage 2」的判断** ——
  继承的确实是一份不同的、跑到 Setup UI 的数据。
- **但 `pb=1` 不能当「Setup 完成」**：用户 23:03 明确停在
  Hello 页、触摸点不动、没点过「继续」。`pb` 的实现只是
  `mu_data_size("...com.apple.purplebuddy.plist") >= 0`
  （文件存在），`budd` 起来写进度文件即可置 1。
  `sheng_boot_setup_reset()` 的 unlink 属早期行为，其打印行
  `sheng setup flags reset` 已被循环缓冲覆盖（msgbuf 仅 165 行、
  起点 >128 s），**本轮取不到，不写成「没执行」**。
  ⇒ **判据修正**：Setup 完成需 `pb=1` **且** Setup UI 消失 /
  进桌面或锁屏；单独 `pb=1` 只说明 Setup Assistant 跑起来了。
  §69.144 起把 `pb` 当完成硬判据的用法须按此收窄。
- 进程侧：`sw` 3894@489 → 7493@1209，但 7465@887 冻结约
  300 s 后才又动；`n` 稳定 119–128 ⇒ 与 Stage 1 的
  「启动收尾停住 / 假进度条」同形，只是 Data 内容不同。
- `MUYU DATA new`、`IOHIDEventDriver` / digitizer、
  `sheng watchdog launch policy`、`persistent store` / `Fresh`
  等早期行**全部被循环缓冲覆盖**，本轮无法判读；不能据此
  断言触摸泵没跑、policy 没执行或窗口没重建。

### 69.146.7 Stage 2 已刷入（23:16:06，运行中）

- 串联门 23:16:00 检测到 `adb recovery` 后自动启动；
  `logs/autoloop-l1p-stage2.log`：`vendor_boot_gpu_fix164-inherit`
  v4 73.81 MiB / ramdisk 72.00 MiB、sha `51ae3d22…`、
  `Writing 'vendor_boot' OKAY`、UEFI `043f7120` `Booting OKAY`，
  23:16:09 起 `waiting for the kernel's own restart into fastboot
  (max 1500s)`。设备已离开 fastboot ⇒ XNU 已接管。
- 待判：窗口头 generation / seed 是否与 Stage 1′ 一致
  （确认真 `Resumed`）、GPU 老缺口（`IconState` / `desktop` /
  `Preferences` 全 0）是否推进、Hello 是否还在、
  **触摸这一轮是否恢复**、屏幕能否进桌面 / 锁屏。
- GPU 配置历史上窗口会延长并自动复位（用户 11-15 纠正），
  若本轮同样自动回 fastboot，autoloop 会自行收尾并解码，
  无需人工长按电源；否则仍需长按 10–15 s。
  （已自动复位，结果见 §69.147。）

## §69.147 Stage 2 上机结果：继承阳性，「能否进桌面」因 207 s 窗口不足不可判（2026-11-15 23:16–23:20）

### 69.147.1 闭环与观测质量

- `logs/autoloop-l1p-stage2.log`：**`rc=0`**，23:16:06 flash + boot，
  23:19:35 `back in fastboot after 207s, unattended`（GPU 配置自动
  复位生效），还原 stock → recovery → 拉窗口 → 解码。
- `logs/vendor_boot_gpu_fix164-inherit-logs/muyu-672687ee`：
  `SHLOG001` **v5**、header_bytes=160、msg_bytes=394273 B / 2135 行、
  `kernel_uptime_s=178`、writer `uptime_s=87`、`rounds=44`、
  `slots_written=40`、`write_errors=4`、`write_ms_max=157`、
  `gpt=0/0`、state=2。msgbuf 覆盖 41–178 s，**早期行完整**
  （比 Stage 1′ 的循环缓冲覆盖好），但 `errors=4` 需留意。
- `[reset] probe: psci v10001 ... wdt=0 last=8ms | spmi ...
  apid=481 owner=0 sdam48=04 read=0 ok=1` @90.15 s。
  **只有这一条 `[reset]` 行**，没有 `autoreset: pings stopped`
  （`BootLog.c:478` 会打 `(tick %us)`）。

### 69.147.2 ★ 继承判据阳性（设备级）

| 项 | Stage 1 结束（§69.144.4） | Stage 2 结束（本轮只读读回） |
| --- | --- | --- |
| 窗口头 magic / version | `SHNGDATA` v1 | 同 |
| seed | `508c7c94…` | **逐字节相同** |
| generation | **5** | **6** |
| pages | 52,428,800 | 同 |
| bitmap present 页 | 197,784（772.6 MiB） | **288,696（1127.7 MiB）**，+90,912 页 / **+355.1 MiB**，11,721 字节不同 |
| store open | — | `NativeUfs: store open blocks=52430401 first=8d41ba waited=0ms why= ok=1` @90.18 s |

- Stage 2 cmdline（1017 B 的 `args = "..."` 串）**没有**
  `sheng-data-reset` ⇒ 不会二次 `discardRequested()`。
  ⇒ generation 5→6 只能来自 Stage 1′ 的 `sheng-data-reset=2`
  （`NativeRamBlock.cpp:79-82` 新建窗口递增）；Stage 2 保持 6
  ⇒ `checkHeader` 的 seed / pages 匹配 ⇒ **`Open::Resumed`**，
  与 §69.143.1 的机制一致。
- bitmap 仍在增长（1127.7 MiB present）⇒ `active=true`、
  两轮确实往**同一个窗口**写页，不是 `Kept` 纯从 seed 跑。
- 诚实边界：Stage 1′ 收尾时**没有**单独读窗口头，所以
  「gen 6 由 Stage 1′ 递增」是机制推断，不是两次设备读数的
  直接差分；但 seed 不变 + Stage 2 无 reset 足以排除
  「Stage 2 又 Fresh 了一次」。

### 69.147.3 ★ 「能否进桌面」本轮不可判：窗口只有 178 s

- Stage 1′ 用户看到 Hello 是在刷入后约 8 分钟（22:55→23:03），
  Stage 1 的 `budd[233]` 在 446 s；而 Stage 2 **kernel 只活 178 s**。
  `budd[230]` @162.30 s 才被派发，距复位仅 16 s。
- 进程仍在**活跃创建**：113 条 `==>`，最后
  `findmydeviced[267]` @176.53 s，`memorystatus` / vnode jetsam
  持续到 177.74 s ⇒ 与 Stage 1 的「`n` 冻结、启动收尾停住」
  **不同形**，是被复位打断，不是自己卡死。
- 无 `IconState` / `desktop` / `AppPlaceholders` 命中；
  `MUYU PFB` 有像素（`nz=470/768 distinct=4`）但形态像诊断 / 噪点页。
- ⇒ **不是阴性结果，是观测窗口不足**。要判「继承成熟 Data 后
  GPU 配置能否进桌面」，必须让这一轮活到 ≥600 s。

### 69.147.4 复位不是 `sheng-autoreset=300` 触发的

> **23:26 更正（详见 §69.148.2）**：本节末条把 `writer=87s` /
> `write_errors=4` 当作「writer 提前停摆」的线索是**误判**，撤回：
> `uptime_s` 从 writer 线程启动算，该线程 kernel ~91 s 才起，
> 87 + 91 = 178 s，writer 连续 44 轮活到最后一秒。另外
> 「改大 300 未必能延长」应升级为**已实证无效**（§69.126
> fix158 300→600，窗口 175→178 s），但有一个必要前提：
> tick 未修好。§69.127.5a 用户 18:59 更正，fix159 修好 tick 后
> 窗口延长且自动复位成功 ⇒ 修好 tick 后该参数恢复为控制变量。

- Stage 2 cmdline 含 `sheng-autoreset=300`、`msgbuf=1048576`，
  但 kernel 178 s 就复位，`300` 的 deadline 远未到；日志里
  **没有** `autoreset: pings stopped` 行。
- `BootLog.c:471-491` / `BootReset.c:40-49`：`boot_reset_watchdog()`
  在首个 tick arm Gunyah watchdog（1 s timeout）并 ping 到 deadline，
  ping 停后约 1 s 复位。本轮提前复位 ⇒ **ping 线程 / tick 提前停了**，
  或 hypervisor watchdog 自己咬了，原因未定 **[推断]**。
- `tools/muyu-autoloop.sh` 头部注释：GPU 轮次 restart 实测
  **145–246 s**（fix9–fix14，6/6），与 207 s 一致 ⇒ 这是 GPU
  配置的固有窗口，不是本轮异常。
- ⇒ **单纯把 `sheng-autoreset` 从 300 改大未必能延长窗口**；
  需先查 ping 为什么提前停（`write_errors=4`、writer `uptime_s=87`
  与 kernel 178 s 的差值是线索）。**〔已被上方更正块撤回〕**

### 69.147.5 UI 侧线索（本轮有，但不足以定论）

- `IOHIDEventDriver:... keyboard: 232 digitizer: 10` @90.13 s
  ⇒ HID 设备（含 digitizer 集合）**注册正常**；这只能证明内核层，
  不能证明事件派发到 UIKit（10-25 TOUCH4 的教训）。
- `backboardd(246)` 起来了，但 `Sandbox: backboardd(246) deny(1)
  iokit-open-user-client AGXDevice` @168.99 s；
  `NSG gpu svc=AGXAccelerator ... proc=backboardd ... verdict=1` @143.93 s。
- `iconservicesagent[100]` @162.46 s 被 jetsam 杀（vnode-limit 10）
  ⇒ 与 §69.141「禁用它图标会缺」的旧缺口同形。
- `MUYU ABI x1073 ... SpringBoard s2/e00002d6 ...` @171.63 s、
  `MUYU HC tick=240 hangs=0 restarts=0` @176.42 s。
- Stage 2 cmdline **没有** `muyu-data-hb` / `muyu-data-probe`
  ⇒ 本轮拿不到 `pb` / `n` / `aks` / `sbp` / `mig` 心跳，
  与 Stage 1′ 的判据**不可直接比较**；不要拿「HB 0 条」当阴性。

### 69.147.6 待用户确认与下一步

> **23:26 屏幕判据已到（§69.148.1）**：用户答复**「只有 logo」**——
> 178 s 全程 Apple logo，无 Hello / 桌面 / 锁屏 / 可交互 UI。
> ⇒ 本节「需按屏幕证据修正」的条件**未触发**；§69.147.3 的
> 「窗口不足」判断得到屏幕证据支持。触摸这轮**无法判**
> （没有可交互目标），不能据此说触摸仍失效。

- **屏幕判据缺失**：本文件只有日志，用户尚未反馈 Stage 2 这轮
  屏幕上看到什么（Hello / 桌面 / 锁屏 / 全黑），以及
  **触摸是否恢复**（11 号第二轮自愈先例）。这是最高优先级。
  **〔已由上方更正块关闭〕**
- 候选：
  1. **A（先问屏幕）**：若已看到 Hello 或桌面，则 §69.147.3 的
     「窗口不足」判断要按屏幕证据修正；若触摸恢复，可直接在
     GPU 配置下点完 Setup。
  2. **B（延长窗口重跑）**：查 ping 提前停的原因，让 Stage 2
     活到 ≥600 s 再判「能否进桌面」。单变量改 `unixnu.toml` 的
     `sheng-autoreset`（cmdline 仍有余量），kernel / payload 不动。
  3. **C（同镜像重跑一轮）**：§K.7 非确定性 + 11 号触摸第二轮
     自愈，成本最低，先排除单轮偶然。
- 本轮零改动：源码 / kernel / payload / 既有镜像一律未变；
  设备已回 recovery、原厂 vendor_boot 已还原，无残留进程。

## §69.148 屏幕判据「只有 logo」与 178 s 窗口根因收敛（2026-11-15 23:24–23:28）

### 69.148.1 屏幕判据落地：支持「窗口不足」，不是阴性

- 用户 23:24 答复 Stage 2 这轮屏幕 **「只有 logo」**：178 s 全程
  Apple logo，**无 Hello / 桌面 / 锁屏 / 可交互 UI**。
- 与日志时间线自洽：`budd[230]` @162.30 s 才被派发，距 178 s
  复位仅 16 s；Stage 1′ 的 Hello 出现在刷入后约 **8 分钟
  （22:55→23:03）**。⇒ §69.147.3「不是阴性，是观测窗口不足」
  **得到屏幕证据支持**，不需要按「若看到 Hello/桌面则修正」的
  分支改写。
- **触摸这轮无法判定**：logo 页没有可交互目标，既不能说触摸
  仍失效，也不能说它恢复。11 号「第一轮失效 / 第二轮自愈」
  的先例仍成立，但本轮不构成新证据。
- ⇒ 真正卡点不是 UI 内容，而是**观察窗口**：要判「继承成熟
  Data 后 GPU 配置能否进桌面」，窗口必须越过 Hello 的 ~480 s。

### 69.148.2 撤回两处：writer 没停摆；deadline 数值有前提

1. **`writer=87s` 不是停摆线索**。按 `BootLog.c:179-183`
   （version 5 注释）与 `:424-427`，`uptime_s` 从 **writer 线程
   启动**算，而该线程要等 NativeUfs publish，kernel ~91 s 才起
   （本轮 `store open ... @90.18 s`）。87 + 91 = 178 s ⇒
   **writer 连续 44 轮活到最后一秒**，`rounds=44 / 87 s`
   正好 2 s 一轮。`write_errors=4` 是 44 轮里 4 次
   `hostWrite4k` 100 ms 放弃（`BootLog.c:41`），`write_ms=2/157`
   在正常区间。§69.147.4 末条把它列为线索是误判。
2. **「改大 300 未必能延长」应升级为「已实证无效，但有前提」**。
   §69.126 fix158 把 300→600，窗口只从 175 s 变 178 s；
   源码级根因是 `LegacyDisplayProbe.cpp:541` 的
   `boot_log_tick(0)` —— **常量 0、且只调用一次**，所以
   `BootLog.c:474` 的 `now >= boot_log_autoreset` 永不成立。
   **但 §69.127.5a（用户 18:59 现场更正）明确**：fix159 修好
   tick 后「窗口延长了，设备也自动复位了」⇒ **tick 修好后
   `sheng-autoreset` 恢复为观察窗口的控制变量**。
   ⇒ 「不要调这个数值」只在 **tick 未修好** 的前提下成立。

### 69.148.3 ★ 两树窗口差异的机制（host 侧源码级，新）

| | 旧树 `UniXNU-muyu`（Stage 1′，活 1126 s） | GPU 树 `UniXNU-muyu-17`（Stage 2，活 178 s） |
| --- | --- | --- |
| `BootReset.c` Gunyah watchdog | **没有**（`grep GunyahWdt` 0 命中），`BootLogStubs.c` weak stub 返回 -1 | **有**：`GunyahWdtTimeoutMs=1000`、`GunyahWdtPingMs=200`、独立 ping 线程 |
| `[reset] probe` | 历史记录 PMIC cell probe 失败 ⇒ `boot_log_reset_ok=0` | `... wdt=0 last=8ms ... ok=1` @90.15 s |
| 首个 tick | 不 `mark`、不 `arm` | `boot_reset_watchdog(deadline, …)` **arm 了 1 s timeout** |
| 结果 | 无 hypervisor 咬断，靠用户长按电源，writer 跑到 1126 s | 145–246 s 固有窗口（autoloop 注释，fix9–fix14 6/6） |

- ⇒ GPU 树短窗口的**直接来源**是被 arm 的 Gunyah watchdog，
  不是 Data 继承、不是本轮异常、也不是 `sheng-autoreset` 数值。
  这解释了「同一份 Data，CPU 树能活 20 分钟，GPU 树只活 3 分钟」。
- 178 s 那一口**为什么**咬仍未定 **[推断]**：ping 线程提前停 /
  hypervisor 忽略 set-time（`muyu-autoloop.sh` 头部注释：
  「timeout did not follow the set-time value in ms」）/
  某次 SMC 停顿 >1 s，三者本轮区分不了。
- 本轮系统**仍在前进**，与 fix157/158/159 的 ~175 s CPU 饿死
  **不同形**：writer 44 轮连续到 177 s、113 条 `==>`、
  最后 `findmydeviced[267]` @176.53 s、jetsam 到 177.74 s、
  `MUYU HC tick=240 hangs=0 restarts=0` @176.42 s。

### 69.148.4 重要边界：旧「给足时间也没有桌面」不能挡掉本轮

- §69.127.5a 结论 2「窗口延长后仍是蓝色启动屏 ⇒ 瓶颈在用户态
  合成内容，不在观察时长」是在**旧 Data**（850.2 MiB 半成品、
  `pb=0`）下得出的。
- 本轮 Stage 2 继承的是 Stage 1′ 干净 seed 上**跑到 Setup UI**
  的卷（§69.146.6：`pb=1` 全程、`sbp` 1965 B、`aks` 154/1753、
  `mig` 190），窗口头 gen 6 / seed 不变 / 1127.7 MiB。
- ⇒ 用户「换 Data 也许能进桌面」的假设**尚未被检验**，
  拿旧结论挡掉重验是过度推论。

### 69.148.5 候选（按推荐排序，等用户选字母）

1. **A（推荐）拣回 fix159 tick 补丁 + `sheng-autoreset=600`**
   - 现成资产：`logs/fix159-tick.patch`（109 行 / 3 文件）。
     **已 dry-run**：在 `UniXNU-muyu-17/package/` 下
     `patch --dry-run -p0` 三文件全部 `patching file`、无 hunk
     失败；当前 `BootLog.c 5256d261…` / `BootReset.c 1ccc9758…`
     / `BootLogStubs.c 5380763d…` **逐字 = fix158 基线**。
   - 改动：3 源码 + `board.toml:612` 一个数值（300→600），
     三步链重建（需重编 kernel），**Data 窗口不动**
     ⇒ 继续 gen 6 `Resumed`，继承链不破。
   - 效果：tick 每秒真实 uptime ⇒ deadline 生效；每 200 ms
     显式 ping ⇒ 不再被 1 s timeout 误咬；600 s 后 `wdt_hurry()`
     自动回 fastboot。目标窗口 **≥600 s > Hello ~480 s**。
   - 风险：改动面最大；§69.127.3 记载 175 s 后 tick/writer
     可能被用户态 CPU 自旋饿死，最坏退化为「不自动复位、
     需长按电源 10–15 s」（与 Stage 1′ 操作相同，不会更糟）。
2. **B 关掉 watchdog：`sheng-autoreset=0`**（只改 board.toml +
   三步链，kernel **代码**不变）
   - `PE_parse_boot_argn` 返回 0 ⇒ 首个 tick 既不
     `mark_bootloader` 也不 `arm`，从机制上绕开咬断；
     旧树 1126 s 是强先例。窗口由人工控制：看到 Hello /
     桌面后再长按电源 10–15 s。
   - 代价：失去自动复位与 bootloader cell 标记。Stage 1′ 旧树
     同样未 mark，用户长按电源后 autoloop 仍抓到 fastboot，
     是可行性先例，但 GPU 树未验证 **[推断]**。
   - 风险：上一轮 hypervisor watchdog 已 `Enable`，跨 boot
     是否遗留 enabled 状态未定 **[推断]**。
   - **不能**等长二进制 patch cmdline：§69.126.1 记载重建会
     同时改 `chosen/random-seed`、`_PrelinkKCID` 与 **ramdisk
     校验和**（两版 `cmp -l` 共 288 B 差异），必须走三步链。
3. **C 同镜像重跑一轮**（零改动，成本最低）
   - `fix164-inherit` 原样再刷，排除 §K.7 单轮偶然 + 11 号
     触摸自愈先例。`board.toml:609` 注释记 fix32 曾有
     writer 690 s / fastboot 712 s 的长窗口先例。
   - 但期望窗口仍落在 145–246 s（6/6 历史），大概率还是
     「只有 logo」。
4. **明确排除**：只把 300 改 600 而**不修 tick** ——
   §69.126 已实证无效，不要重复试探。

### 69.148.6 本轮状态

- 零改动：**未刷机、未改源码 / board.toml / kernel / payload /
  任何既有镜像 / Data 窗口**；仅新增本节与 §69.147.4、
  §69.147.6 的更正块，工作日志同步追加。
- 设备 `a20248f7` 在 recovery，原厂 vendor_boot 已还原，
  无残留 autoloop 进程；Stage 2 日志与窗口读回产物保留在
  `logs/vendor_boot_gpu_fix164-inherit-logs/muyu-672687ee`。

## §69.149 fix165：拣回 fix159 tick 补丁 + `sheng-autoreset=600`（A 方案，2026-11-15 23:52–）

### 69.149.1 改动、备份与构建

- 用户 23:50 选 **A**（§69.148.5 候选 1）。完整备份
  `/tmp/fix165-baseline/`：`BootLog.c` / `BootReset.c` /
  `BootLogStubs.c`（三者逐字 = fix158 基线）、`board.toml`、
  `kernel.macho`（fix164 `2d5815cb…`）、`payload.img` / `payload.json`。
- 应用 `logs/fix159-tick.patch`（109 行 / 3 文件），
  在 `UniXNU-muyu-17/package/` 下 `patch -p0 --batch` 三文件全通过。
  `board.toml` 的 `"sheng-autoreset=300"` → `"600"`（**等长**），
  并加注释写明「不要调这个数值」只在 tick 未修好的前提下成立。
- 三步链，`XNU_EXTRA_CFLAGS='<7 个 -Wno flags> -DXNU_WATCHDOG_POLICY=1'`
  （§69.142.1，保住 H 的 policy 宏；删 `kern_exec.o` 强制重编）：
  `logs/build-kernel-fix165.log` / `build-payload-fix165.log` /
  `build-vendor-boot-fix165.log`，**三步 rc=0、零 error/warning**。

### 69.149.2 ★ 构建回归被离线校验拦下（错误镜像未上机）

- 第一次 `build payload` 误用了 §69.20.2 的**模板参数**，
  产物 `kernel_args` 1008 → **960 B**，丢了
  `sheng-tum=swCD` / `sheng-staged-apps=1` / `sheng-lite=1`
  （合计 48 B），且 `trust_cache_sha256` / `gpu_addon_sha256`
  变成 `-full` 版本（`0c559104…` / `b2dddc51…`）。
- 根因：**三步链模板 ≠ 当前 fix164 配置**。从
  `logs/build-payload-fix164.log` 的 kernel args 与
  `payload.json` 的 sha 反推，fix164 的真实参数是：
  - `--owner-trust-cache fw/trust-21H461/owner-trust-17-fix107.bundle`
    （`6fc1914b…`）、`--gpu-addon fw/gpuaddon-17-fix107.bin`
    （`410685f6…`）——**不是** `-full`；
  - **6 个** `--kernel-arg`：`sheng-fb-gpu=1 sheng-gpu-addon=1
    sheng-gpu-metal=1 sheng-tum=swCD sheng-staged-apps=1 sheng-lite=1`。
    `payload.py:120-123` 把 extra 追加在 spec 之后 ⇒ cmdline 尾部
    6 个参数正是这个列表。模板只有前 3 个。
- 错误镜像归档 `logs/vendor_boot_gpu_fix165-badargs.img`
  （sha `21f3def9…`），**禁刷**。
- 教训（写死）：以后每轮重建必须先从**基线构建日志 +
  `payload.json`** 反推真实参数，再用 `kernel_args` 的
  difflib 与 trust/addon sha 做离线单变量校验；只照抄文档模板
  会静默丢参数，且 `build payload` 的 1016 B 硬闸**不会**报错
  （960 B 照样通过）。

### 69.149.3 正确重建与离线校验（全绿）

- 用 fix164 真实参数重跑 `build payload` + `patch-vendor-boot.py`
  （kernel 不重编），日志 `logs/build-payload-fix165b.log` /
  `build-vendor-boot-fix165b.log`。
  产物 **`fw/vendor_boot_gpu_fix165-longwin.img`**，
  77,393,920 B，sha **`3be18c54737f3248b761fd143e7c97ace587433d2bbc9f2202ebc284346c58f1`**。

| 判据 | fix164-inherit | fix165-longwin | 结论 |
| --- | --- | --- | --- |
| `kernel_args` | 1008 B | **1008 B**，唯一 delta `300→600` | ✓ 单变量，硬闸余 8 B |
| trust / addon / data seed | `6fc1914b…` / `410685f6…` / `476464c3…` | **全 SAME** | ✓ 继承链不断 |
| `skip_setup` / personality_rewrites | False / `[]` | **SAME** | ✓ |
| kernel / ADT / payload sha | `2d5815cb…` / `b7e6f023…` / `e0e30870…` | `c5739a45…` / `f0e137d9…` / `dffdbd54…` | 预期（tick 补丁 + ADT 随机 seed） |
| 镜像 diff | — | **7,136,225 B 全在 payload 区**，非 payload 区 **0** | ✓ |
| cmdline 明文 | 1017 B | **1017 B**，尾部 3 参数全在 | ✓ |
| `sheng-data-reset` / `muyu-data-hb` | 无 / 无 | **仍无** | ✓ 与 Stage 2 一致，不二次清窗 |
| FAT `scratch-data.img` | `476464c3…` | 逐字节 == `fw/data-seed.img` | ✓ |
| `nm` 关键符号 | — | `_boot_reset_wdt_ping` **强定义**、`_boot_log_tick` / `_boot_reset_watchdog` / `_sheng_boot_watchdog_policy` 全在 | ✓ tick 生效、policy 宏保留 |

- 补丁语义复核（`logs/fix159-tick.patch:74`）：
  `log_tick_thread` 每 200 ms `boot_reset_wdt_ping()`，但
  `if (!boot_log_reset_tried)` ⇒ `wdt_hurry()` 置位后**停止喂狗**；
  原 `wdt_thread` 也因 `wdt_deadline=0` 退出 ⇒ 600 s deadline
  能真正触发复位，不会卡死不重启。
- **上机前窗口基线**（只读读回）：header 与 Stage 2 结束
  **逐字节相同**（`ef8378a0…`、seed `508c7c94…`、gen 6），
  bitmap 288,696 页 / 1127.7 MiB 也逐字节相同 ⇒ 上机后若
  gen→7 / seed 不变 / bitmap 增长 = 真 `Open::Resumed`。

### 69.149.4 上机（23:56:43–00:00:34，`rc=0`）

- `tools/muyu-autoloop.sh fw/vendor_boot_gpu_fix165-longwin.img 1500`
  → `logs/autoloop-fix165.log`；23:56:48 flash + boot、23:56:51
  RAM boot OK、23:56:52 XNU 接管。
- **00:00:13 自动回 fastboot（202 s 墙钟，unattended）**，
  00:00:16 还原原厂 `vendor_boot`、00:00:33 回 recovery 拉 32 MiB
  窗口、00:00:34 解码：`logs/vendor_boot_gpu_fix165-longwin-logs/muyu-67488230`
  （`tag=muyu kernel=175s msgbuf_reaches=175s gap=0s writer=85s
  rounds=43 slots=40 errors=3 write_ms=2/174 msgbuf=377615 B`）。
- 判据结果：`[log] tick 119s n=30 deadline=600 tried=0` /
  `149s n=60 deadline=600 tried=0` —— **tick 每秒真实 uptime
  与 `deadline=600` 均生效**；但 `tried=0` 全程、无
  `autoreset: pings stopped`、无 hurry ⇒ **600 s deadline 从未
  触发**，窗口止于 175 s kernel / 202 s 墙钟，与 fix164 的
  178 s / 207 s 同形。**完整分析见 §69.150**。
- 更正 69.149.3 末尾笔误：窗口头「gen→7」不是 `Resumed` 的
  判据；`Resumed` 应保持 generation 不变（只有 `Fresh` 才新建
  代数）。本轮 header 与上机前**逐字节相同**（gen 6）才是
  `Open::Resumed` 的正确证据。

## §69.150 fix165 上机结果：tick 修复阳性、A 方案阴性、复位源与 deadline 无关（2026-11-16 00:00–00:10）

### 69.150.1 闭环与观测质量

- `logs/autoloop-fix165.log` **rc=0**，全流程无人值守：
  23:56:48 flash + boot → 23:56:52 XNU 接管 →
  **00:00:13 自动回 fastboot（202 s 墙钟，unattended）** →
  00:00:16 还原原厂 `vendor_boot` → 00:00:33 回 recovery 拉
  32 MiB 窗口 → 00:00:34 解码。设备现停 recovery，无残留进程。
- 上轮担心的「最坏情况需人工长按电源」**没有发生**：自动复位
  能力保留。但窗口是 202 s 墙钟 / **kernel 175 s**，不是 600 s。
- 解码 `logs/vendor_boot_gpu_fix165-longwin-logs/muyu-67488230`：
  `msgbuf=377615 B` / 2001 行，覆盖 **40.96 → 175.286 s**，
  **早期行完整**（比 Stage 1′ 只有 165 行、起点 >128 s 好）；
  `writer=85s rounds=43 slots=40 errors=3 write_ms=2/174`、
  `gap=0s`、`kernel_uptime_s=175`。writer 从 ~90 s（UFS publish）
  连续跑到最后一秒，**没有提前停摆**。观测质量足以支撑判据。

### 69.150.2 ★ tick 修复本身是阳性的（补丁与 cmdline 都落地）

- `[log] tick 119s n=30 deadline=600 tried=0` @119.18 s、
  `[log] tick 149s n=60 deadline=600 tried=0` @149.23 s。
  `log_tick_thread` 每 200 ms 循环、每 5 次调一次
  `boot_log_tick(kernel_uptime_s())`，每 30 次打一行 ⇒
  **每秒一次真实 uptime、每 30 s 一行**，起点 ≈89 s（与
  writer 的 90 s 一致）。119→149 恰差 30 s / 30 tick。
- `deadline=600` 被正确读到 ⇒ `sheng-autoreset=600` 已进 kernel、
  `PE_parse_boot_argn` 生效。`tried=0` 全程且 `now` 最大 175
  ⇒ `now >= 600` **从未成立**，逻辑自洽，不是「deadline 没生效」。
- 下一行本应 `179s n=90`，但 kernel 175 s 结束 ⇒ 没出现是
  **正常**，不能据此推断 tick 线程在 149 s 后停了。
- `[reset] probe` @90.258 s：`wdt=0 last=3ms … ok=1`（fix164
  是 `wdt=0 last=8ms ok=1`）⇒ Gunyah watchdog status 0、XBL
  已 arm、cell 可写，`boot_log_reset_ok=1`，arm 路径具备条件。
- `sheng bring-up watchdogd launch policy: 0` @91.90 s ⇒
  H 的 policy 宏仍在构建里（`-DXNU_WATCHDOG_POLICY=1` 保住）。
- ⇒ §69.127.5a「修 tick 生效」在本轮**再次被设备级证实**；
  §69.149 的离线校验（强定义符号、单变量 cmdline）与运行一致。

### 69.150.3 ★ A 方案阴性：窗口没延长，且复位源与 deadline 无关

- 目标 ≥600 s，实测 **175 s kernel / 202 s 墙钟**，与 fix164
  （178 s / 207 s）、fix157/158（175/178 s）、fix9–fix14
  （145–246 s）**同一区间**。`sheng-autoreset` 300→600 **没有
  改变窗口长度**，§69.126「改数值无效」在 tick 已修好的前提下
  依然成立。
- **决定性证据**：`tried=0` 全程、`autoreset: pings stopped`
  **0 次**、hurry **0 次**、`log.bin` 里 `watchdog: armed` /
  `not armed` / `bootloader` 文本 **0 命中**。⇒ 这次自动复位
  **不是 tick deadline 触发的**，而是另一条更早的路径。
- 收窄上一轮的推断：§69.148.4「ping 线程 / tick 提前停了，或
  hypervisor watchdog 自己咬了」现在可以进一步排除「deadline
  提前触发」，但**仍无法区分**以下三者，一律标 **[推断]**：
  1. Gunyah watchdog 1 s timeout 因喂狗停摆而咬；
  2. hypervisor 忽略 `GunyahWdtSetTime(1000)` / `GunyahWdtPing`，
     XBL 自带的 timeout 在 ~175 s 到期（`wdt=0` 表明 XBL 已 arm）；
  3. 用户态 CPU 自旋把 `BASEPRI_REALTIME` 的 tick 线程饿死
     （`BootLog.c` 注释记 fix26 的 174 s 记录截断即此形态）。
     反证：`MUYU HC tick=240 hangs=0 restarts=0` @175.286 s、
     `MUYU USHB t=240` @175.275 s、writer 活到最后一秒，
     内核线程并未整体冻结。
- 日志**没有** panic / `CRITEXIT` 系统级崩溃 / `autoreset:`，
  最后一行是 175.286 s 的 HC tick ⇒ 急性硬复位，不是软停机。

### 69.150.4 与 fix159 的差异不可比，单轮阴性不定论

- fix159（同为 tick 补丁，`deadline=300`）落盘日志也只有
  `119s n=30` / `149s n=60`、也止于 175 s，但现场 **6+ 分钟
  无 fastboot、USB 离线**，用户 18:59 明确更正「窗口延长了，
  也自动复位了」，18:52:16 才由人工长按结束（≥433 s）。
- fix165 是 **202 s 自动回 fastboot**。两者行为不同，但
  **单变量不干净**：fix165 的 kernel 累积了 H policy 宏 /
  staged apps / lite / fix107 trust+addon，Data 是 gen 6 的
  1127.7 MiB（fix159 时是另一份），deadline 600 ≠ 300。
- ⇒ 不能据此判定「tick 补丁退化」，也不能推翻 §69.127.5a；
  按 §K.7 启动非确定性 + 用户 11 号「第二轮触摸自己好了」
  的先例，**单轮阴性不定论**，需同镜像重跑排除偶然。

### 69.150.5 Data 继承第三次阳性（只读读回，非推断）

| 判据 | fix165 上机前 | fix165 上机后 | 结论 |
| --- | --- | --- | --- |
| 窗口头 | `ef8378a0…`，magic `SHNGDATA`，seed `508c7c94…`，gen 6 | **逐字节相同**（first diff `None`） | ✓ `Open::Resumed`，未新建代数 |
| bitmap present 页 | 288,696（1127.7 MiB） | **297,690（1162.8 MiB）** | ✓ +8,994 页 / **+35.1 MiB**，真写入 |
| bitmap 差异字节 | — | 1,135 B 不同 | ✓ 同一窗口增量 |

- 加上 Stage 2 的 gen 5→6 / +355.1 MiB，这是**第三次**设备级
  确认跨配置继承成立。fix165 没有 `sheng-data-reset`，
  `checkHeader` 匹配 ⇒ `Resumed` + `active=true`。
- ⇒ 用户 23:10「让能进桌面的配置加载完，再切当前配置，设置是
  持久的」这条机制**已充分验证**；剩下的问题不是继承，而是
  GPU 配置下这份 Data 能否走到桌面。

### 69.150.6 屏幕判据缺失与 UI 线索

- **用户尚未反馈本轮屏幕**（最高优先级、零成本）：是否仍只有
  logo、有没有 Hello、触摸是否像 11 号第二轮那样恢复。
- 日志侧：`budd[229]` @**148.46 s**（fix164 是 162.30 s，
  **提前 14 s**）、SpringBoard 首次 @121.04 s（fix164
  @133.72 s，提前 12.7 s）⇒ 启动推进整体比 fix164 快
  ~13 s，可能与 gen 6 数据更成熟有关，也可能只是 §K.7 **[推断]**。
- 但 `IconState` / `desktop` / `AppPlaceholders` / `Hello` /
  `SetupComplete` / `purplebuddy` **全 0 命中**；
  `backboardd(240) deny(1) iokit-open-user-client AGXDeviceUserClient`、
  `MUYU OPEN refused e00002d5`、`WidgetRenderer_Default deny`
  仍在 ⇒ GPU 用户态访问被拒的老缺口**没有推进**。
- Stage 1′ 的 Hello 在刷入后约 8 分钟（~480 s）才出现，而本轮
  175 s 就结束 ⇒ 按上一轮的「窗口不足」解释，屏幕上大概率
  仍只有 logo；但**必须以现场判据为准**。

### 69.150.7 下一步候选（等用户给字母）

1. **C（推荐，零改动）**：同镜像
   `fw/vendor_boot_gpu_fix165-longwin.img` 重跑一轮，排除
   §K.7 单轮偶然。若这轮窗口自己变长（≥600 s）就直接拿到
   长窗口，成本最低；同时补屏幕判据。
2. **B（机制性判定）**：`sheng-autoreset=0`，不 arm、不 mark，
   只改 `board.toml` + 三步链（kernel 代码不动），直接检验
   「175 s 复位是否来自我们自己的 Gunyah watchdog 路径」。
   风险：可能失去自动复位 / 复位后不回 fastboot，需人工长按，
   autoloop 1500 s 安全超时。
3. **D（定位而非绕过，需一轮镜像）**：在 `log_tick_thread` /
   `wdt_thread` 加喂狗计数与 timeout 返回值诊断打印（复用
   `boot_reset_wdt_stats[0..3]`），把「ping 是否停、SMC 是否被
   接受、谁在 175 s 复位」变成可读判据，再决定改哪里。
- **排除**：继续调 `sheng-autoreset` 数值（300→600 已证伪）、
  再堆 Data 内容（继承已三次阳性）。
- 若屏幕判据显示本轮已越过 logo 到 Hello/桌面，则以上
  「窗口不足」的定性须按现场证据修正，优先级重排。

### 69.150.8 本轮改动边界

- 源码 / `board.toml` / kernel / payload / 镜像 / Data **零改动**
  （改动都发生在 §69.149 的构建阶段）；设备自动完成还原，
  现停 recovery；仅新增 §69.149.4 结果、§69.149.3 笔误更正、
  本节与工作日志。

## §69.151 fix165 R2：同镜像重跑未出现长窗口；复位锚点无法用被动观测判定（2026-11-16 07:29–07:34）

### 69.151.1 闭环与产物

- 用户 07:29 选 **C**（§69.150.7 候选 1）。上机前设备曾整体
  脱离 USB（`ioreg -p IOUSB` 无任何子设备、`system_profiler
  SPUSBDataType` 为空），挂等待门 `oEv5Lu` 后 07:29:51 设备出现
  在 fastboot，门自动开跑。
- `tools/muyu-autoloop.sh fw/vendor_boot_gpu_fix165-longwin.img 1500`
  → `logs/autoloop-fix165-r2.log`，**同一镜像、sha
  `3be18c54…`、零改动**：07:29:51 flash + boot、07:29:55 XNU
  接管、**07:33:17 自动回 fastboot（203 s 墙钟）**、07:33:19
  还原原厂 `vendor_boot`、07:33:37 回 recovery、07:33:38 解码。
  `rc=0`，无人工干预。设备现停 recovery。
- 解码 `logs/vendor_boot_gpu_fix165-longwin-logs/muyu-72f9661a`：
  `kernel=185s msgbuf_reaches=185s gap=0s writer=85s rounds=43
  slots=39 errors=4 write_ms=2/163 msgbuf=308902 bytes`，msgbuf
  覆盖 **51.17 → 185.795 s**（134.6 s），早期行完整。

### 69.151.2 §K.7 的作用被限定：同一镜像重跑落在**同一个**窗口档位

| | R1（00:00） | R2（07:33） |
| --- | --- | --- |
| 墙钟（`fastboot boot` → 回 fastboot） | **202 s** | **203 s** |
| `kernel_uptime_s` | 175 s | **185 s** |
| UEFI→XNU 接管耗时 | 27 s | 18 s |
| msgbuf 覆盖 | 40.96→175.286 s | 51.17→185.795 s |

- 同一镜像 sha `3be18c54…` 连跑两轮，墙钟 202/203 s、`rc=0`、
  都无人值守 ⇒ **这一轮没有出现「同镜像自己变长」的偶然**，
  C 轮的问题（这次会不会像 11 号触摸那样自愈）答案是否定的。
- 但**不能说「窗口是确定性的」**：历史 86 轮的墙钟呈多档分簇
  （167–168 s ×6、197–198 s ×21、**202–203 s ×30**、
  207–237 s ×9、413–414 s ×24、861/1218 s 各 1）。202/203
  是**最大的一簇**，两轮都落在这里是符合众数的结果，不足以
  证明确定性；C 轮只说清了「本轮没抽到长窗口那一档」。
- 其中 413–414 s 那一档（24 轮）正是 §69.148 的 writer 截断
  形态：设备活到 414 s，只是记录停在 ~175 s。⇒ 长窗口在
  历史上**出现过**，不是不可能，只是概率事件。**[推断]**

### 69.151.3 复位锚点：两轮数据不足以区分「墙钟」与「kernel」；历史 86 轮给的是反证

> **本节初版曾写「复位锚在墙钟、不在 kernel 内 [强证据]」，用历史
> 86 轮配对后**已撤回**为「不可区分」。原文结论是只凭 R1/R2
> 两轮推的，属于过度推断，请勿据此决策。**

- R1/R2 的表面现象：墙钟 202/203 s（差 1 s），`kernel_uptime_s`
  175/185（差 10 s），而 UEFI→XNU 接管耗时 27 s vs 18 s。
  两轮差 9 s 恰好抵消 kernel 的 10 s 差。
- **但这个抵消是设计上的不可辨识**：
  `wall = UEFI 段 + kernel uptime`。若锚在墙钟，kernel 应随
  handoff 反向变化（R2: 203−18=185 ✓）；若锚在 kernel，wall
  应恒为 `handoff + kernel`（R1: 27+175=202 ✓）。
  **两种假设都能同时拟合两轮**，两轮数据无法区分。
- 历史 86 轮（`logs/autoloop-*.log` × `header.txt` 按 artifact
  精确配对）的 handoff 分布：**23–30 s 占 74 轮**（中位 27），
  另有 56/58/61/62/82 s 少数。handoff 基本恒定 ⇒ 在这 74 轮里
  **wall 与 kernel 同步变化**（167↔141、197↔171–173、
  202/203↔175、414↔389–391），**倾向 kernel 侧计时**，与
  「锚在墙钟」相反。
- 另有一类必须单独看待：**wall=414 但 kernel=173–175**
  （`fix41/42/68/64/77/113b` 等，handoff≈239–241）。这类是
  §69.148 记载的 **writer 在 ~175 s 停摆导致记录截断**，
  设备实际活到 414 s ⇒ 说明 175 s 之后系统仍在跑，
  「175 s 是复位点」只在 writer 也同时停的那类轮次成立。
- ⇒ 结论降级为：**复位点呈离散档位**（kernel
  ~141 / ~173–175 / ~389–391 / 690），不是连续随机，也不是
  单一固定值；R2 的 185 s 是唯一一次 18 s handoff 的异常配对，
  不能作为锚点证据。要判定锚点在哪一层，必须做**主动实验**
  （见 69.151.7 的 B），不能再靠被动观测反推。

### 69.151.4 tick 修复再次阳性（与 R1 一致）

- R2：`[log] tick 129s n=30 deadline=600 tried=0` @129.40 s、
  `159s n=60 deadline=600 tried=0` @159.50 s。与 R1 的
  119/149 s 整体后移 10 s，正是 UEFI 段快 9 s 的镜像。
- `tried=0` 全程、`autoreset:` / hurry / `watchdog: armed`
  文本 **0 命中**；`[reset] probe … wdt=0 last=3ms … ok=1`
  @100.48 s ⇒ 结论与 R1 完全一致：**复位不经过 deadline 路径**。

### 69.151.5 Data 继承第四次阳性

| 判据 | R1 后 | R2 后 |
| --- | --- | --- |
| 窗口头 sha | `ef8378a040860b2f` | **逐字节相同** |
| seed / generation | `508c7c94…` / 6 | **不变** |
| bitmap present | 297,690 页（1162.9 MiB） | **299,393 页（1169.5 MiB）**，+1,703 页 / **+6.7 MiB** |
| bitmap 差异 | — | 219 B |

- ⇒ 仍是 `Open::Resumed` 增量写；四轮（Stage 1 / Stage 2 /
  fix165 R1 / R2）连续确认继承机制，Data 侧已无待验项。
- 另注：R2 只写 +6.7 MiB，远小于 R1 的 +35.1 MiB，且
  jetsam 0 次 vs 83 次 ⇒ 两轮的用户态活跃度差异很大，
  但窗口档位相同（202/203 s）⇒ **窗口长度与用户态负载
  无关**，这一条是两轮对照里唯一可以确定的因果排除。

### 69.151.6 屏幕与 UI：R2 比 R1 更早期、但同样停在 logo

- **R2 的 `budd` 命中是假阳性**：`SetupAssistant.framework/budd`
  在 R2 **0 次**（R1 有 1 次 @148.46 s），唯一含 "budd" 的
  行是 `extensionkitservice` 拼接行；两轮的 "Hello" 也都是
  `com.apple.HelloPoster.HelloPosterExtension` 容器路径，
  **不是 Hello 页**。⇒ 不能据此说 R2 出了 Setup UI。
- R2 的 UI 侧总体比 R1 **更不活跃**：jetsam kills **0**（R1 83）、
  `AGXDeviceUserClient` deny **6**（R1 15）、容器枚举 10 行
  （R1 11）。R2 日志到 185.8 s 仍在稳定刷 `MUYU ABI …
  deleted s2/e00002d6`、`MUYU SWAPT`、`tx_flush`，**没有**
  R1 那种 163 s 起的 jetsam 风暴。
- 但 `IconState` / `desktop` / `AppPlaceholder` / `SetupComplete`
  **两轮全 0**；`backboardd deny AGXDeviceUserClient`、
  `MUYU OPEN refused e00002d5` 依旧。⇒ 两轮都没越过
  logo，与用户 R1 现场「只有 logo」一致。
- **R2 屏幕判据待用户确认**（本轮是否同样只有 logo、
  触摸有无变化）。若 R2 屏幕出现任何 R1 没有的内容，
  上面关于窗口档位与 UI 的定性要按现场证据修正。

### 69.151.7 结论与下一步

1. **C 轮达成它自己的目的**：同一镜像连跑两轮都落在 202/203 s
   这一档，没有出现「自愈成长窗口」的偶然。但**窗口不是单一
   确定值**（历史 5 档、167→1218 s 都有），所以不能据此宣布
   「窗口固定 202 s」，只能说本轮没抽到长窗口档。
2. **可以下的硬结论**：fix158（300→600）/ fix159（tick）/
   fix165 R1+R2（tick+600 双轮）——四次都停在
   ~175 s（或 185 s）档，`tried=0` 全程、`autoreset:` 0 命中
   ⇒ **调 `sheng-autoreset` 数值与修 tick，都不改变本档窗口**。
   这条不需要再验。
3. **下一步最有效的单变量是 B：`sheng-autoreset=0`**（不 arm、
   不 mark）。它把「我们的 watchdog 是否参与复位」从推断变成
   可测：若窗口仍 ~175 s ⇒ 复位源完全在我们代码之外，kernel
   侧 watchdog 工作可以停；若窗口变成 413 s 档或更长 ⇒ 我们的
   arm 反而在**缩短**窗口（`SetTime(1000)` 可能把 XBL 的
   长 timeout 改短了）。**这是本轮之后信息量最大的一步。**
4. **D（诊断打印）** 作为 B 的补充：把
   `boot_reset_wdt_stats[0..3]`（status / set-time / enable /
   ping 计数）打进 msgbuf，一轮就能看到 `GunyahWdtSetTime`
   的返回值——若非 0 就实证「1 s timeout 没被接受」。B 与 D
   可以在同一轮里做（D 只加打印、不改行为），但会变成两变量，
   建议 D 单独一轮或只在 B 阳性后再做。
5. **屏幕判据优先**：若 R2 屏幕上出现了 Hello/桌面，则
   「只有 logo」不成立，需重排优先级。
6. 不再重复：同镜像重跑（C 已完成）、继续调 autoreset 数值、
   再堆 Data 内容。

### 69.151.8 本轮改动边界

- 镜像 / 源码 / `board.toml` / kernel / payload / Data
  **零改动**（R1 的构建产物原样复用，sha 核对一致）；设备由
  autoloop 自动还原，现停 recovery，无残留进程；仅新增本节
  与工作日志。

## §69.152 R2 屏幕=只有 logo；撤回 B（autoreset=0），改判「1 s 看门狗被瞬时饿死」为首要嫌疑（2026-11-16 07:40）

### 69.152.1 观测（用户现场）

- R2 全程**只有 logo**，无 Hello / 桌面 / 锁屏，与 R1 完全相同。
- 与 §69.151.4 的「两轮窗口档位相同、用户态活跃度差极大」互为印证：
  **窗口长度不由 UI 负载决定**。
- 触摸本轮**无法判定**（logo 页没有可点目标，既非失效也非恢复）。
- ⇒ §69.151 的「确定性」表述不需要再改；R1/R2 在屏幕层与时间层都同形。

### 69.152.2 ★ 撤回 §69.151.7 的 B（`sheng-autoreset=0`）

源码级两个理由，均在动手前拦下：

1. **它测不到「喂狗停摆」。** `BootLog.c:545`
   `if (!boot_log_reset_tried && &boot_reset_wdt_ping) boot_reset_wdt_ping();`
   与 `sheng-autoreset` **无关**，tick 线程照旧每 200 ms 喂狗。
   `autoreset=0` 只去掉 `SetTime/Enable/mark`，不改变喂狗节奏 ⇒ 若真凶
   是 ping 停摆，窗口不会变，**实验无信息量**。
2. **它拆掉安全网且危及 Data。** `BootLog.c:501` 的整块
   （probe → `mark_bootloader` → `boot_reset_watchdog`）全部 gate 在
   `boot_log_autoreset` 非零上。`autoreset=0` ⇒ 不写 reboot-reason cell
   ⇒ 复位后**不回 fastboot**，可能直接起原厂 Android 并挂 `/data`；而
   我们的 Data 窗口（gen 6、seed `508c7c94…`）就在该分区 ~24 GB 偏移处，
   原生系统挂载/重整 userdata 有覆盖风险。这是唯一一份跑到 Setup UI 的卷，
   不能拿去赌。

### 69.152.3 ★ 新证据：看门狗在 90/100 s 已 arm，且 3 ms 前刚被喂过

- R1 `90.258 s` / R2 `100.476 s`：
  `[reset] probe: psci v10001 reset=0 ... | wdt=0 last=3ms | ... ok=1`
- `BootReset.c:97-99`：`status 0` = **已 arm**（XBL 就 arm 的）；`last`
  = 距上次 ping 的毫秒数。
- ⇒ 我们的 `SetTime(1000) + Enable` 成功，tick 线程的 200 ms ping
  **真在喂**；`600 s` deadline 从未到达、`hurry` 从未调用（`tried=0`，
  连续 4 轮）⇒ **1 s 超时是唯一还上膛的扳机**。
- 时间上：窗口终点与 writer 记录终点逐秒重合（R1 `175.286 s` ≈ kernel
  175 s，R2 `185.795 s` ≈ kernel 185 s）⇒ **同一瞬间既停日志又断喂狗**。
- ⇒ 首要嫌疑 **[推断]**：SpringBoard 起转后 `BASEPRI_REALTIME` tick 线程
  被饿死 >1 s，1 s 超时立即咬。`BootLog.c:533` 注释里 writer 在 174 s
  被同类饿死，是同一机制的既有先例。

### 69.152.4 首选单变量：`GunyahWdtTimeoutMs` 1000 → 60000

- 只改 `BootReset.c:50` **一个常量**；cmdline 1008 B 不变，
  `mark` / `arm` / `enable` / 自动回 fastboot 全部保留。
- 判读：窗口越过 ~235 s（甚至走到 600 s deadline）⇒ 真凶是我们自己的
  1 s 看门狗被瞬时饿死；窗口仍在 175–202 s ⇒ 复位源在 kernel 之外，
  kernel 侧看门狗工作可以整体停止。
- `hurry` 路径不受影响：deadline 触发时 `wdt_hurry()` 仍把超时拉回 1 s。
- 60 s 是**历史已接受值**（`BootReset.c:47-49` 注释记载旧 60 s 版本实测
  174–246 s 复位），不是新引入的未知量。

### 69.152.5 本轮改动边界

- 仅新增本节 + 工作日志；源码 / `board.toml` / kernel / payload /
  镜像 / Data **零改动**（撤回的 B 未落地，故无回退动作）。
- 设备停 recovery，原厂 vendor_boot 已还原，无残留进程。

## §69.153 fix166：看门狗 arm 超时 1 s → 60 s（单变量），并补上 arm 结果打印（2026-11-16 07:42–）

### 69.153.1 假设与判读

- 假设 **[推断]**：fix165 R1/R2 的 175/185 s 复位，是我们自己把 Gunyah
  看门狗的超时设成 1 s 后，`BASEPRI_REALTIME` 的 ping 线程被 SpringBoard
  自旋瞬时饿死 >1 s 立即被咬（§69.152.3 的证据链）。
- 判读矩阵：
  - 窗口越过 ~235 s（或直接走到 600 s deadline）⇒ 假设成立，
    真凶是 1 s 超时；后续正解是把超时定在合理值（本轮的 60 s 若够用即定案）。
  - 窗口仍在 175–202 s ⇒ 复位源在 kernel 之外（XBL/hypervisor 自有计时器），
    kernel 侧 watchdog 这条线可以整体收工。

### 69.153.2 改动（唯一行为变量 + 一项纯打印）

| # | 文件 | 改动 | 性质 |
| --- | --- | --- | --- |
| 1 | `BootReset.c:50` | `GunyahWdtTimeoutMs` 1000 → **60000** | **唯一行为变量** |
| 2 | `BootLog.c:505` 后 | 新增 `printf("[reset] mark: %s \| watchdog: %s\n", …)` | 仅输出，不改行为/顺序 |

- `boot_reset_wdt_hurry()` 用的是**硬编码 1000**（`BootReset.c:141`），
  不受常量影响 ⇒ deadline 触发后仍在 1 s 内复位，安全网保留。
- 第 2 项的动机：这两个文本一直「composed 但从不打印」，导致
  「`SetTime` 被拒」与「外部复位」无法区分，是本轮之前的最大诊断盲点。
- 其余：`BootLog.c` 的 tick 线程、`board.toml`（`sheng-autoreset=600`）、
  Data seed、trust cache、gpu addon、6 个 `--kernel-arg` 全部未变。

### 69.153.3 离线校验（构建后、上机前）

- 三步链 `rc=0/0/0`，kernel / payload 日志 **0 error 0 warning**。
- `BootReset.c` 反汇编（`BootReset.o`）：arm 路径 `mov w1,#0xea60` /
  `mov w2,#0xea60`（=60000）紧接 `smc`（fid 0x86000008）；`hurry`
  路径仍是 `mov w1,#0x3e8`（=1000）⇒ 机器码级确认单变量成立。
- 最终 kernel 里 `0xea60` 的 `movz w1/w2` 编码 3 处命中（两处相邻 =
  arm 的 w1/w2），`#1000` 编码仍 78 处（hurry 及其它调用者）。
- `payload.json`：`kernel_args` 1008 B **逐字相同**（硬闸余 8 B）、
  `data_ramdisk_sha256` / `trust_cache_sha256` / `gpu_addon_sha256` 全 SAME。
- 镜像 `fw/vendor_boot_gpu_fix166-wdt60.img`（77,393,920 B，
  sha `fc52fa5949741cd4c4b71094679eeb004b391edb3912f2bc7963ba27470f51f6`）：
  与 fix165 差异 **733,208 B 全在 payload 区，非 payload 区 0**；
  cmdline 1017 B 明文逐字相同（`sheng-autoreset=600`、尾部三参数全在）；
  FAT 内 `scratch-data.img` 与 `fw/data-seed.img` 逐字节相同
  （`476464c3…`）。
- 上机前窗口头与 R2 结束**逐字节相同**（`ef8378a0…`、gen 6、
  seed `508c7c94…`），bitmap 299,393 页 / 1169.5 MiB ⇒ 继承链未断。
- 备份：`/tmp/fix166-baseline/`（fix165 的三份源码 + board.toml +
  kernel.macho + payload.img/json），可一键回退。

### 69.153.4 上机（07:45:47 起，结果待补）

- `tools/muyu-autoloop.sh fw/vendor_boot_gpu_fix166-wdt60.img 1500`
  → `logs/autoloop-fix166.log`；07:45:52 flash + UEFI RAM boot，
  07:45:56 起 XNU 接管，等内核自复位。
- 对照组：fix165 R1 墙钟 202 s / kernel 175 s；R2 203 s / 185 s。
  若机制如假设成立，应看到 `[reset] mark: … | watchdog: armed 0,
  pings until 600s, bite 60000 ms later …` 这行（首次可见），
  且 msgbuf / writer 记录越过 175–185 s。

### 69.153.5 上机结果（07:45:52 刷入 → 07:49:48 回 fastboot，233 s）

- `[reset] mark: mark: cell 00 -> 04 rc=0 | watchdog: watchdog: armed 0,
  pings until 600s, bite 60000 ms later (last ping 88252 ms ago)`
  —— **这行第一次可见**（§69.153.2 的打印），三点同时确认：
  `SetTime(60000)` 被接受（`armed 0`）、deadline 仍是 600 s、
  reboot cell 写入 `00 -> 04 rc=0`（复位仍回 fastboot）。
- 窗口 **233 s**（对照 fix165 R1 202 s / R2 203 s）⇒ 只改看门狗超时
  就让窗口移动 ⇒ **复位执行者就是我们自己的 GunyahWdt arm**。
  §69.152.4 的判定矩阵落在第一支。
- 但**算术不闭合**：1→60 s 只换来 +30 s，不是 +60 s；三轮的
  `wall - 最后一行日志` 分别是 26.7 / 17.2 / 57.1 s。⇒ 「最后一次
  ping + 超时」不是 hypervisor 的真实行为（可能是 ping 停摆时刻早于
  日志停摆、或 hypervisor 对 set-time 有粒度/clamp）。标 **[未解]**，
  不影响「执行者=我们」这一定性。
- `kernel_uptime_s=176`、msgbuf 止于 `175.909 s`——与 fix165 的
  175.286 / 185.795 同档。

### 69.153.6 ★★ 真正的卡点：GPU 配置在 175–186 s 全核冻结

| 轮次 | msgbuf 覆盖 | 结论 |
| --- | --- | --- |
| CPU 配置 Stage 1′ | 475.3 → **1213.4 s** | 一直输出到 1213 s |
| GPU R1 | 41.0 → 175.3 s | 175 s 后**再无任何 printf** |
| GPU R2 | 51.2 → 185.8 s | 185 s 后**再无任何 printf** |
| GPU fix166 | 40.1 → 175.9 s | 175 s 后**再无任何 printf** |

- `msg_bytes=345566`，1 MiB 环形缓冲**远未写满**、未回绕 ⇒ 不是覆盖
  导致的"看起来停了"，而是 **printf 真的停止**。
- 冻结时连 `[log] tick`（每 30 s 一行）与 `MUYU HC tick`（每 ~5 s）
  都不再出现 ⇒ **不是某个线程被饿死，而是全部内核线程停止**。
- fix166 多活的 30 s 全花在**已冻结的内核**里（窗口 233 s，日志仍止于
  175.9 s）⇒ **加长超时只买到"尸体多躺一会儿"，买不到进桌面**。
- 排除一个嫌疑：R2 **0 次** vnode jetsam 却也停在 185.8 s；R1/fix166
  有 79–83 次 jetsam（集中在 162–163 s）⇒ jetsam 风暴是**结果不是原因**。
- 量化：三轮 `caching container path` 均 ~866 次（止于 150–152 s），
  CPU 配置 626 次但活到 1213 s ⇒ 容器枚举本身不足以解释冻结。

### 69.153.7 结论与下一步候选

1. **看门狗这条线可以收工**：执行者已定性，60 s 版本可保留作为安全网
   （复位仍回 fastboot、cell 写入 rc=0）。
2. **新的唯一目标 = 解掉 175–186 s 的 GPU 配置全核冻结**。冻结点与
   SpringBoard 起 UI 同档（`MUYU ABI … SpringBoard s2/e00002d6` @163 s、
   `MUYU OPEN refused e00002d5` @164.3 s、backboardd 的 AGX deny），
   嫌疑集中在 GPU/Metal 合成路径。
3. 候选（按成本排序）：
   - **B**：同一镜像去掉 `sheng-gpu-metal=1`（只改 payload 的一个
     kernel-arg），单变量直指 Metal/AGX 是否是冻结源；其余全部不变。
   - **A**：用 CPU 渲染配置 + 当前成熟 Data（gen 6）跑一轮，回答
     「Data 内容是否足以出桌面」这一自 §69.127.5a 起悬着的问题。
   - **C**：加一个每秒打印的实时探针，把「全核冻结」与「printf 锁被
     持住」区分开（诊断，不解决问题）。

## §69.154 fix167：去掉 `sheng-gpu-metal=1`（B 方案）——Metal/AGX 被证伪，冻结依旧

用户 07:52 选 **b**（§69.153.7 候选 B）。单变量：把 Metal 客户端这条链
整段摘掉，其余（`sheng-fb-gpu=1`、`sheng-gpu-addon=1`、`sheng-gpu-power=9`、
600 s autoreset、60 s 看门狗、Data seed gen 6）全部不动。

### 69.154.1 改动与构建

- `board.toml:642` 的 `"sheng-gpu-metal=1",` 注释掉（附 fix167 注释块），
  构建时也不再传该 `--kernel-arg`。两个位置都要处理：`payload.py:120-123`
  把 extra 追加在 spec 之后，只删一边的任一位置都会被另一边重新加回来。
- 三步链（同 §69.149.1 的 `XNU_EXTRA_CFLAGS` + `-DXNU_WATCHDOG_POLICY=1`，
  删 `kern_exec.o`）：`logs/build-kernel-fix167.log` / `build-payload-fix167.log`
  / `build-vendor-boot-fix167.log`，三步 rc=0、零 error。
- 镜像 `fw/vendor_boot_gpu_fix167-nometal.img`（77,393,920 B，
  sha `50f4bc71336d51b57c5b58359e73d8ded0c1d60c631fb37a1c55ec5dbb80e005`）。
- 备份 `/tmp/fix167-baseline/`（`board.toml`、fix166 的 `payload.img`/`.json`、
  `GPU-HANDOFF.md`）。

### 69.154.2 离线校验

| 项 | fix166 | **fix167** |
| --- | --- | --- |
| `kernel_args` | 1008 B | **990 B**（−18 B = `sheng-gpu-metal=1` + 分隔空格） |
| 参数尾部 | `… sheng-gpu-addon=1 sheng-gpu-metal=1 sheng-tum=swCD …` | **`… sheng-gpu-addon=1 sheng-tum=swCD sheng-staged-apps=1 sheng-lite=1`** |
| `data_ramdisk_sha256` / `gpu_addon_sha256` / `trust_cache_sha256` | — | **全 SAME**（`476464c3…` / `410685f6…` / `6fc1914b…`） |
| `dtb+table+bootconfig`（1,892,352 B） | — | **逐字节 SAME** |
| payload 区差异 | — | **490 B**（参数串 18 B + ADT 随机 64 B + kernel 重编带来的零散字节） |
| 镜像内 `sheng-gpu-metal` 字面量 | 3 处 | **2 处**（只剩 kernel 内的参数名解析表，unixnu.toml 里已无） |

> **构建非确定性（新发现，影响以后的单变量论证方式）**：同一份源码、
> 同一套 flags 连编两次，`kernel.macho` sha 分别是 `a17cdf2e…` 与
> `ddb95603…`（`logs/build-kernel-fix167-verify.log`）。因此
> **kernel sha 变化不能当作"源码变了"的证据**，反之也不能。本轮改用
> 功能判据确认内核与 fix166 同构：`0xea60` 的 `movz w1/w2` 编码 **3 处
> 命中**（两处相邻 = arm 路径）、`sheng watchdog launch policy` 串 **1 处**。

### 69.154.3 上机（07:58:27 刷入 → 08:02:24 回 fastboot）

`tools/muyu-autoloop.sh fw/vendor_boot_gpu_fix167-nometal.img 1500`
（中途宿主进程被中断，收尾由 `/tmp/fix167-tail.sh` 接力完成：还原原厂
vendor_boot、回 recovery、拉窗口）。产物
`logs/vendor_boot_gpu_fix167-nometal-logs/muyu-67474f6c`。

| 判据 | fix166 | **fix167** |
| --- | --- | --- |
| 墙钟窗口 | 233 s | **233 s**（XNU 07:58:31 → 08:02:24） |
| `kernel_uptime_s` | 176 | **176** |
| msgbuf 末行 | 175.91 s | **174.48 s** |
| `MUYU METAL` 行 | `s5/0 r1`（publishMetal 跑了） | **`s0/0 r0`（跳过）** |
| `MUYU ABI` 行 | 16 | **0** |
| `MUYU OPEN refused e00002d5` | 14 | **0** |
| AGX（`AGXDeviceUserClient` deny 等） | 41 | **0** |
| vnode jetsam | 80 | **0** |
| `caching container` | 866 | **865** |
| `IconState` / `desktop` / `AppPlaceholder` | 0 / 0 / 0 | **0 / 0 / 0** |
| `[reset]` 行 | 90.25 s 两条 | **90.25 s 两条，逐字同形**（600 s pings / 60000 ms bite） |

- **变量确实生效**：`MUYU METAL M … s0/0 r0` 是 `publishMetal()` 未调用
  的直接证据；`MUYU ABI` 16 → 0 说明连 ABI 自测（`GpuPowerProbe.cpp:1784`，
  内部 `uc->clientClose()` 的 PM4 提交）也整段没跑 ⇒ 本轮是"**完全没有
  Metal 加速器与用户客户端**"的配置，比"只是不注册 AGX"更强。
- **结论（阴性，但是干净的单变量阴性）**：Metal/AGX 客户端**不是**
  175 s 冻结的原因。冻结在 174.48 s 依旧发生，与 fix166 的 175.91 s、
  fix165 R1 的 175.29 s、R2 的 185.80 s 同一档。

### 69.154.4 ★ 复位算术这次闭合了：冻结是真的（不是 printf 锁）

- fix167：最后一行日志 174.48 s，墙钟复位 233 s ⇒ **+58.5 s**。
- fix166：175.91 → 233 s ⇒ **+57.1 s**。超时值 60000 ms。
- 差 ≤3 s，第一次和"最后一次 ping + 超时"对得上（§69.153.5 的「+30 而非
  +60」不闭合由此得到解释：当时用的口径是"最后一行日志"，而 1 s 那一档的
  咬合明显不按 deadline 走）。
- 为什么这很重要：喂狗 ping 由内核线程每 200 ms 发一次，**ping 停 = 内核
  停**。复位时刻正好比最后活动晚一个完整超时 ⇒ 内核在 174.5 s 真的不再
  执行任何线程 ⇒「全核冻结」这个判断成立，**不需要**再用 C 方案的每秒
  探针去区分"printf 锁被持住"（那只解释 printf，解释不了 ping）。
- fix165（`GunyahWdtTimeoutMs=1000`）是唯一例外：175.29 + 1 = 176.3，实际
  202 s（+26.7 s）⇒ hypervisor 侧对 set-time 有**最小咬合粒度**（约 26-30 s
  量级），1 s 这种小值不可信；60 s 档落在可观测的线性区。

### 69.154.5 冻结瞬间的现场（fix167）

```
[ 174.064521] MUYU USHB t=240 rows=0 last=0 marks=0 calls=0 ...
[ 174.065431] MUYU HC tick=240 hangs=0 restarts=0
[ 174.190802] MUYU PFB n=1 nz=768/768 distinct=2 ch=0/768 rows=0..0
[ 174.191308] MUYU PFBC 000=764 fff=4
[ 174.475092] ustate: caching container path {'com.apple.PosterBoard' : ...}
             /Applications/PosterBoard.app/PosterBoard[239] ==> container   ← 到此为止
```

- `MUYU HC tick` 每 5 s 一条、`[log] tick` 每 30 s 一条，`t=240` 之后
  **下一跳（179 s）没有出现** ⇒ 不是某一个线程被饿死。
- 与 fix166 的尾部形态不同（那轮以 `iokit-set-properties` deny 收尾），
  但停止时刻同档 ⇒ 冻结由**时间/负载**决定，不由某一条代码路径决定。
- fix167 没有 GPU 客户端、没有 AGX deny、没有 jetsam，`caching container`
  仍 865 次（fix166 866 次）⇒ 容器枚举量不是变量（§69.153.6 的结论保持）。

### 69.154.6 下一步（候选重排）

Metal 被证伪后，剩下的差异只有"GPU 接管扫描输出 + GPU 上电/冷启动"这一块：

- **A′**：去 `sheng-gpu-power=9`（保留 `sheng-fb-gpu=1` + addon）。问
  「GMU 冷启动 / CP / GPU SMMU 接管留下的状态是否在 175 s 反噬」。
  GPU 上电链在 51 s 就跑完了（`PWR gcc=ok cc=ok sm=ok gp=skip pw11`、
  `CPROW C6/C10 ok`），之后闲置 120 s 才炸，所以这条不是首选，但它的
  阴性结果能把嫌疑压到显示路径上。
- **B′（首选）**：去 `sheng-fb-gpu=1`（保留 GPU 上电 + addon）。让扫描输出
  回到 CPU 拷贝。历史（2026-11-13 fix15 注释）记录过"无 fb-gpu ⇒ pres=0、
  SpringBoard 37 s 内 45086 wakes、~197 s 复位"，**那是另一条失败模式，
  但当时的其它变量与现在差很多**，值得重测：若这轮能活过 200 s 甚至出
  桌面，冻结就锁定在 GPU 扫描输出/offer 路径；若仍在 175 s，则冻结与
  显示无关，要回到 mobile-graphics client 或内存/热。
- **C**：降级为可选。§69.154.4 已经把"printf 锁"这一支排除，探针只在
  需要"冻结前最后 1 s 里哪个核还在跑"时才有价值。

## §69.155 ★ fix169：去掉 `sheng-gpu-power=9` —— 第一次进桌面、触摸可用、Setup 完成

用户 08:13 前选 A′。单变量：`board.toml` 注释掉 `"sheng-gpu-power=9",`，
构建时也不再传该 `--kernel-arg`。其余与 fix168 完全相同（Metal 已去、fb-gpu 已去、
addon 保留、600 s autoreset、60 s 看门狗、Data seed gen 6）。

### 69.155.1 结果：这是项目第一次真正越过 logo

| 判据 | fix168（power=9） | **fix169（power 关闭 = 0）** |
| --- | --- | --- |
| 墙钟窗口 | 233 s（175 s 冻结） | **不再冻结** |
| 屏幕 | 只有 logo | **Hello → 设置流程 → 桌面** |
| 触摸 | 无响应（logo 页无可判据） | **可用** |
| Setup | 从未到达 | **完成**（语言/网络/Apple ID 全过） |
| 复位后 | — | **直接进桌面，跳过 Setup** |

- 08:13:43 刷入 `fw/vendor_boot_gpu_fix169-nogpupwr.img`，08:13:46 XNU 接管。
- `sheng-autoreset=600` 在 **08:23:44** 精确咬合（08:13:43 + 601 s），
  守护脚本 `/tmp/fix169-reboot.sh` 在 fastboot 出现的 3 s 内 `fastboot boot`
  同一份 UEFI（不重刷、不动 vendor_boot），08:23:47 `Booting OKAY`。
- **08:26 用户确认：重启后直接进桌面，不再走 Setup** ⇒ `PersistOverlay`
  的 `Open::Resumed` 在真实 Setup 产物上首次被端到端验证（此前四轮只验证到
  `Resumed` + generation 递增，没有验证到"桌面状态被接住"）。

### 69.155.2 因果链闭合

三次阴性 + 一次阳性，单变量逐层剥离，全部同源同镜：

| 轮次 | 变量 | 冻结时刻 | 屏幕 |
| --- | --- | --- | --- |
| fix166 | Metal + fb-gpu + power=9 | 175.91 s | logo |
| fix167 | − `sheng-gpu-metal=1` | 174.48 s | logo |
| fix168 | − `sheng-fb-gpu=1`（`pres=0` 已证） | 175.56 s | logo |
| **fix169** | **− `sheng-gpu-power=9`** | **不冻结** | **Hello → 桌面** |

⇒ **175 s 全核冻结的唯一来源是 `sheng-gpu-power` 这条 GPU 上电/冷启动链**。
Metal 客户端（fix167）、扫描输出（fix168）各自被独立证伪，二者都不是原因。

### 69.155.3 机制候选（待分阶段验证，不预付结论）

该链在 **51 s** 就跑完（`PWR gcc=ok cc=ok sm=ok gp=skip pw11`、`CPROW C6/C10 ok`），
之后**闲置 124 s** 才炸，所以不是"执行时崩"，而是"留下的状态在后续被某个访问触及"。
三个候选：

1. **SMMU context 半配置**：mode 4 起我们给 GMU 建了 bank1/ASID1/SMR slot2，
   mode 9 又给 GPU 建了 bank0/SID 0/1 on SMR 0/1。若 SMR/S2CR 的 VALID 位与
   CBAR/TTBR 不一致，任何走该 stream 的访问会触发 SMMU fault → 若未正确上报而
   变成挂死，互联层（NOC）上的事务永不返回 ⇒ 所有核停摆（这正是"ping 也停"、
   只有 hypervisor 侧定时器还能咬合的现象）。
2. **GX 上电后的电源/时钟反噬**：GX GDSC 开了但 GMU 的 BW vote / RSC 未持续维护，
   长时间后 GDSC 塌陷或 NOC 超时。
3. **CP ring 闲置后 fence 超时**：mode 9 起了 CP 但无负载，hang detect 触发后
   走我们未实现的 recovery 路径。

### 69.155.4 ★ 关键工具：`sheng-gpu-power` 是 1→9 的递进阶梯，不是开关

`GpuPowerProbe.cpp:1-40` 的头部注释给出了完整语义，这条阶梯把"冻结在哪一级"
变成可直接二分的问题（此前只能整体开/关）：

| 值 | 新增动作 | 写寄存器 | 碰 GPU 本体 |
| --- | --- | --- | --- |
| 1 | 只读 GCC GPU branches / GPUCC / SMMU GR0 / RBBM_STATUS | 无 | 否 |
| 2 | + 开 `gcc_gpu_cfg_ahb_clk`（Linux gcc-sm8550 probe 唯一写） | 有 | 否 |
| 3 | + CX GDSC 上电 + `gpu_cc_ahb_clk` + `hlos1_vote_gpu_smmu_clk` | 有 | 否（GX 不动） |
| 4 | + 建 GMU 的 SMMU context（bank1 / ASID1 / SMR+S2CR slot2，stream 5） | 有 | 否（GMU 不启动） |
| 5 | + 只读 GMU 冷启动前置（GMU 时钟树、AOP cmd DB `81c60000`、HFI 表） | 无 | 否 |
| 6 | + GMU 时钟 bring-up（GPLL0 votes、PLL0/PLL1、hub 200 / gmu 220 MHz） | 有 | 否（GMU 仍在 reset） |
| 7 | + **GMU 冷启动**（`gmu_gen70200.bin`、RSC/PDC、CM3 release、HFI startup） | 有 | 开始 |
| 8 | + **GX 上电**（HFI `GX_BW_PERF_VOTE` + OOB `GPU_SET` bit30/ack31） | 有 | 是 |
| 9 | + **hw_init → a7xx_cp_init**（GPU bank0 SID0/1、SQE、ring、memptrs、CP_ME_INIT、zap shader） | 有 | 全部 |

**二分计划（每轮 ~6 min：构建 1 min + 上机 4 min + 收尾）**：

- **第一轮 `power=3`**：只到 CX 上电 + SMMU vote clk，完全不碰 GMU/GX/CP。
  - 不冻结 ⇒ 冻结在 4..9，进入下一轮二分。
  - 仍冻结 ⇒ 冻结在 CX 上电/SMMU vote 这一小段，嫌疑立刻缩到 GDSC 或 vote clk。
- 之后按 6 → 8 → 9 收敛（先粗后细），预期 3 轮定位到具体一级。
- **注意**：这条阶梯只决定"哪一步开始有害"，不直接带来 GPU 加速——
  要真正用上 GPU 至少需要 8/9。定位后不是简单回退，而是修那一级
  （SMR/S2CR 配对、GDSC 维持、CP hang 处理）。

### 69.155.5 fix170：3600 s 长窗口（已构建，待换）

`board.toml:618` `sheng-autoreset=600` → **`3600`**，其余与 fix169 完全相同
（`gpu-power` 仍关闭）。三步链 rc=0：
`logs/build-kernel-fix170.log` / `build-payload-fix170.log` /
`build-vendor-boot-fix170.log`。

- `fw/vendor_boot_gpu_fix170-longsetup.img`，sha
  `a1d8d8ded925fc9047d796d81d6f5e07f1cd197a7464a549b24f4e3466b85c31`，
  `kernel_args` **958 B**，`autoreset=['sheng-autoreset=3600']`，`power off: True`。
- 守护 `/tmp/fix170-swap.sh`：等 08:33:50 那次 autoreset 复位 → `muyu-flash-mode.sh xnu`
  刷入 fix170 → 自动 boot，之后一小时窗口。
- **为什么不用 `autoreset=0`**：`BootLog.c:501` 整块（probe → `mark_bootloader` → arm）
  gate 在 `boot_log_autoreset` 非零；`0` 会让它跳过 `mark_bootloader`，复位后不回
  fastboot，危及 Data（§69.151 已判）。3600 是安全上限内的实用值。

## §69.156 fix171：`sheng-gpu-power=3`（阶梯第 1 级）—— 不冻结，毒在 4~9

承接 §69.155.4 的阶梯二分。单变量：在 fix169（power 全关）基础上只把
`sheng-gpu-power=3` 加回来，其余不变（Metal off、fb-gpu off、addon on、
autoreset 3600）。

### 69.156.1 ★★ 先说一个差点误导结论的操作陷阱：UEFI 启动器有两个

`tools/muyu-flash-mode.sh:44-45`：

```
# MUYU_UEFI for 17-line boots; the default stays unchanged so the other checkout is not disturbed.
UEFI=${MUYU_UEFI:-UniXNU/out/muyu/boot/boot.img}
```

- 默认值 `UniXNU/out/muyu/boot/boot.img`（2,134,016 B，Nov 5，sha `c50e76eb…`）
  **在这台设备上起不来**：屏幕停在 UEFI 最后一帧
  `ssusb_phy_init_success lane:0*0`，USB 完全不枚举（fastboot/adb 都看不到），
  必须长按电源 12–15 s 硬复位。
- 17-line 专用启动器 `fw/boot-17line-043f7120.img`（2,129,920 B，Nov 11，
  sha `043f7120…`）才是可用的，**必须显式传 `MUYU_UEFI`**。

事故过程（08:34–08:40，连错三次）：08:33:50 的换镜守护脚本写成
`muyu-flash-mode.sh xnu <img>`，漏了 `MUYU_UEFI`，于是 08:34:14 / 08:36:59 /
08:39:49 三次都用了默认启动器 → 三次全部卡 UEFI。期间还误判为 "fix171 有毒"、
误判为 "设备/分区损坏"，并为了让"设备健康"而刷回 fix169 —— 结果 fix169 **也**
卡在同一行，这才暴露真因是启动器而不是镜像。

> **判据教训**：先看 `logs/flash-ledger.tsv` 的 `boot` 行（它记录每次启动用的
> 镜像与 sha），再怀疑镜像。台账里三行 `UniXNU/out/muyu/boot/boot.img` 与
> 08:13 那行 `fw/boot-17line-043f7120.img` 摆在一起，一眼就能定位。
> **另一个反直觉点**：卡 UEFI 时既无 fastboot 也无 adb，和"XNU 正在跑"
> 完全无法区分，只能靠用户看屏幕或看台账。

### 69.156.2 结果：不冻结（250 s+），但触摸失效

| 判据 | fix169（power 关） | **fix171（power=3）** |
| --- | --- | --- |
| 冻结（175 s 全核死） | 无 | **无**（08:44:50 起 250 s+，fastboot 未回） |
| 屏幕 | 桌面 | **锁屏**（越过 logo，走到 UI 后期） |
| 触摸 | 可用 | **失效 → 无法上划解锁** |
| 桌面可用性 | 卡、Settings 黑屏、respring | 未测到（卡在锁屏） |

- 刷入：`MUYU_UEFI=fw/boot-17line-043f7120.img muyu-flash-mode.sh xnu
  fw/vendor_boot_gpu_fix171-pwr3.img`，`slot=a unbootable=no retry=7`，
  vendor_boot v4 73.81 MiB / vendor_ramdisk 72.00 MiB，sha `b3911755…`，
  args **976 B**（`sheng-autoreset=3600` + `sheng-gpu-power=3`）。
- **结论**：阶梯 1→3（只读 + `gcc_gpu_cfg_ahb_clk` + CX GDSC 上电 +
  `gpu_cc_ahb_clk` + `hlos1_vote_gpu_smmu_clk`）**不产生 175 s 冻结**。
  ⇒ 冻结源在 **4~9**：GMU 的 SMMU context / GMU 只读前置 / GMU 时钟
  bring-up / GMU 冷启动 / GX 上电 / hw_init+CP 六段之中。

### 69.156.3 触摸失效：两条互斥假设，尚未区分

1. **power=3 的副作用**：CX GDSC 上电或 `hlos1_vote_gpu_smmu_clk` 的使能
   改变了某个与触摸共享的电源/时钟域（骁龙平台触摸常挂在 CX 或共享 LDO 上）。
2. **偶发**：项目里有先例（用户 11 号那次"触摸用不了"，下一轮自动恢复），
   触摸本身在这个 port 上就是逐轮变量，与 power 无关。

区分方法（成本最低）：**同一个 fix171 镜像再启动一次**。
触摸恢复 ⇒ 假设 2（偶发）；仍失效 ⇒ 假设 1（副作用，且需要单独定位到
CX GDSC 还是 smmu vote clock）。

### 69.156.4 二分图（更新）

```
power=0  ✅ 桌面可用（fix169）        power=9  ❌ 175 s 冻结（fix166/167/168）
power=3  ✅ 不冻结（触摸失效）
         └─ 毒 ∈ [4,9]
            下一步 power=6（GMU 时钟 bring-up，仍不启动 GMU、不碰 GX/CP）
            ├─ 6 冻结  ⇒ 毒 ∈ [4,6]，再试 4 或 5
            └─ 6 不冻结 ⇒ 毒 ∈ [7,9]，再试 8 或 7
```

## §69.157 fix172：`sheng-gpu-power=6`（阶梯第 2 级）—— 不冻结，且触摸恢复

单变量：在 fix171 基础上把 `sheng-gpu-power=3` → **`6`**，其余不变
（Metal off、fb-gpu off、addon on、autoreset 3600、17-line UEFI）。

### 69.157.1 结果

| 判据 | power=0（fix169） | power=3（fix171） | **power=6（fix172）** |
| --- | --- | --- | --- |
| 175 s 冻结 | 无 | 无 | **无**（08:51:50 起 251 s，fastboot 未回） |
| 屏幕 | 桌面 | 锁屏（触摸死，卡住） | **锁屏 → 上划 → 桌面** |
| 触摸 | 可用 | **失效** | **可用** ✅ |

- 刷入：`MUYU_UEFI=fw/boot-17line-043f7120.img muyu-flash-mode.sh xnu
  fw/vendor_boot_gpu_fix172-pwr6.img`，`slot=a unbootable=no retry=7`，
  vendor_boot v4 73.81 MiB / vendor_ramdisk 72.00 MiB，sha
  `ddc93ab25c3d790a8bd26e8d05e304b714fd06e27696d022ec7121afff38551b`，
  args 976 B（`sheng-autoreset=3600` + `sheng-gpu-power=6`），44 个参数。

### 69.157.2 ★ 触摸失效定位到 power=3 这一档，且是可自愈的瞬时态

三点对照构成一条清晰的因果：

- power=0 触摸可用（fix169，用户完成整个 Setup）
- **power=3 触摸失效**（fix171，卡在锁屏无法上划）
- **power=6 触摸恢复**（fix172，解锁进桌面）

⇒ 触摸失效**不是**阶梯的通病，也**不是**偶发（§69.156.3 的假设 2 被否掉）：
它精确落在“CX GDSC 与 `hlos1_vote_gpu_smmu_clk` 已上电、但 GMU 时钟树
（GPLL0 votes / PLL0-PLL1 / hub 200 MHz / gmu 220 MHz / GMU branches）尚未
起来”这个**中间态**上。一种合理解释：该中间态下 CX 域的供电/复位尚未被
GMU 侧的时钟约束固定，触摸控制器（与 GPU 共享 CX 域或共享 LDO 的常见接法）
被复位线拖住；mode 6 把 GMU 时钟树拉起来之后，域进入稳定态，触摸自愈。

**实践含义**：这解释了历史现象——“11 号那轮触摸用不了，下一轮又好了”
（用户原话）。若当时也处在类似的中间态，逐轮变量的表观随机性就有了解释。
后续若再遇到“触摸掉”，先看那一轮 `MUYU PWR` / `pw` 位与 GMU 时钟行，
而不是先怀疑触摸驱动本身。

### 69.157.3 二分图（更新）

```
power=0  ✅ 桌面（fix169）             power=9  ❌ 175 s 冻结（fix166/167/168）
power=3  ⚠️ 不冻结，触摸失效（瞬时态）
power=6  ✅ 不冻结，触摸可用（fix172）
         └─ 毒 ∈ [7,9]
            power=7 = GMU 冷启动（firmware 加载 / CM3 release / HFI startup）
            power=8 = + GX 上电（HFI GX_BW_PERF_VOTE + OOB GPU_SET）
            power=9 = + hw_init → a7xx_cp_init（GPU SMMU bank0 / SQE / CP / zap）
            下一步 power=8（中点）：8 冻结 ⇒ 毒 ∈ [7,8]，再试 7；
                                    8 不冻结 ⇒ 毒 = 9（CP/hw_init 那段）
```

## §69.158 ★★ fix173：`sheng-gpu-power=8` —— 175 s 冻结的来源被锁定到 hw_init/CP 段

单变量：fix172 的 `sheng-gpu-power=6` → **`8`**（只差这一级），其余完全
相同：无 `sheng-gpu-metal`、无 `sheng-fb-gpu`、addon on、autoreset 3600、
17-line UEFI。sha `9d91036e…`，args 976 B，44 参数。

### 69.158.1 结果：卡 logo，但 300 s 不冻结

| 判据 | fix172（power=6） | **fix173（power=8）** |
| --- | --- | --- |
| 屏幕 | 锁屏 → 桌面 | **卡 logo** |
| 触摸 | 可用 | 未到 UI（无判据） |
| 175 s 冻结 | 无 | **无**（08:58:51 起 301 s，fastboot 未回） |

- 新增的两级：7 = GMU 冷启动（`gmu_gen70200.bin`、RSC/PDC、CM3 release、
  HFI startup）；8 = + GX 上电（HFI `GX_BW_PERF_VOTE` + OOB `GPU_SET`）。
- **8 引入了一个新的失败模式**：UI 完全起不来（停在 logo），但系统**没有**
  冻结——看门狗仍在喂，说明内核一直在跑，只是 SpringBoard/backboardd 那条
  UI 链没走完。

### 69.158.2 ★★★ 关键对比：冻结需要的正是 power=9 独有的那一段

把 fix168 与 fix173 并排——两者**除了 power 值以外配置逐项相同**
（都无 metal、无 fb-gpu、addon 相同、autoreset 相同、屏幕状态都是 logo）：

| 轮次 | power | 屏幕 | 冻结 |
| --- | --- | --- | --- |
| **fix168** | **9** | logo | **175.56 s 全核冻结** |
| **fix173** | **8** | logo | **无（301 s+）** |

**唯一变量 = power 9 相对 8 多出来的 `hw_init → a7xx_cp_init` 段。**

⇒ **175 s 全核冻结的确切来源锁定在这一段**（此前只知道在 1..9 的整条链里）：

1. **GPU 自己的 SMMU context**：bank 0、SID 0/1 挂在 SMR 0/1（Linux 里
   arm-smmu-qcom 给 GPU 的固定分配）+ SQE / ring / memptrs / power-up list /
   scratch buffers；
2. SQE copy（`a740_sqe.fw` 去掉首 dword）、GBIF halt release、SECVID TSB、
   hwcg（`a740_hwcg`）、QoS、UCHE、AHB、hang detect、CP protect
   （`a730_protect`）、APRIV、中断屏蔽；
3. SQE base / ring / rptr shadows / SQE start；power-up list；
4. `CP_THREAD_CONTROL` + `CP_ME_INIT` + 时间戳 fence；一次 `CP_MEM_WRITE`
   到 scratch page 加第二道 fence；
5. zap shader（PAS 13 init_image、`mem_setup` over `gpu_micro_code_mem`
   0x9b09a000、segment copy、`auth_and_reset`，全走 SIP SMC）与
   `CP_SET_SECURE_MODE 0`；SPI 300 的 GIC 配置；CP always-on counter 速率。

**首要嫌疑（按机制贴合度排序）**：

- **GPU SMMU bank 0 的 SMR/S2CR 配对**：若 SID 0/1 的 SMR 已 VALID 而 CBAR/
  TTBR 或 S2CR 的翻译配置不一致，任何走该 stream 的访问会 SMMU fault；这类
  fault 若未被正确上报而变成总线上永不返回的事务，就是"所有核不退休指令、
  只有 hypervisor 定时器还能咬合"——与 §69.154.4 测到的 +57~58 s 咬合完全吻合。
  注意 mode 4（GMU context，bank 1/slot 2）**不冻结**，mode 8（GX 上电）
  **也不冻结**，说明 4 段建的 context 本身无害；差异出在 9 段对 **bank 0**
  的配置上，这两者本来就不同（bank/ASID/SID 全不一样）。
- **zap shader / secure mode**：`sheng` 走 zap shader，`RBBM_SECVID_TRUST_CNTL`
  从不写。若 TZ 拒绝 `auth_and_reset` 而代码仍继续做 `CP_SET_SECURE_MODE 0`，
  CP 处于非安全模式却持有 secure 资源，后续访问可能被 SMMU/CP 侧挡死。
- **CP hang detect**：CP 起来但无负载，hang detect 触发后走未实现的 recovery
  路径（此前 §69.154 已把它列为候选之一）。

> 这条对比同时否掉了两个更早的猜测：冻结**不需要** UI 起来（fix168 在 logo
> 就冻），也**不需要** Metal 客户端（fix167 已证）、**不需要** GPU 扫描输出
> （fix168 已证）。它是 hw_init 段自身留下的状态。

### 69.158.3 现在有两个独立问题要修，不是一个

1. **power 7/8 → UI 起不来**（卡 logo，系统活着）：GMU 冷启动或 GX 上电破坏了
   UI 链。**要能用 GPU，这个必须先解决**，否则 9 永远进不了桌面。
2. **power 9 → 175 s 全核冻结**：hw_init/CP 段（上面的首要嫌疑）。

### 69.158.4 下一步

- **power=7**（本轮）：把"UI 起不来"定位到 7 还是 8。
  - 7 也卡 logo ⇒ GMU 冷启动本身破坏 UI（firmware/HFI/RSC 一侧）
  - 7 能进桌面 ⇒ GX 上电（8）破坏 UI ⇒ 嫌疑落在 `GX_BW_PERF_VOTE` / OOB `GPU_SET`
    或 GX GDSC 上电后 framebuffer 所在内存的电源/映射
- 之后对 9 段做**代码级分段**（power 只有整档，1..9 已是末级，无法再用参数细分）：
  在 `GpuPowerProbe.cpp` 的 hw_init 路径上加一个 `sheng-gpu-hwinit=<n>` 之类
  的临时分段开关，逐步放行（先只建 GPU SMMU context 不碰 CP → 再 SQE/ring
  → 再 CP_ME_INIT → 最后 zap），用 175 s 判据逐段定位。

## §69.159 fix174：`sheng-gpu-power=7` —— UI 断链的临界点精确定在 6→7（GMU 冷启动）

单变量：fix173 的 `sheng-gpu-power=8` → **`7`**，其余完全相同（无 metal、
无 fb-gpu、addon on、autoreset 3600、17-line UEFI）。sha `cc4ab302…`，
args 976 B。

参数口径澄清：`payload.json` 报 **47** 个 token，`board.toml` 只声明 **44** 个，
差的 3 个是命令行补的 `sheng-tum=swCD` / `sheng-staged-apps=1` / `sheng-lite=1`
（`payload.py:120-123` 把 extra 追加在 spec 之后）。**以后核对参数一律以
`payload.json` 为准，44/47 不是异常。**

### 69.159.1 结果：卡 logo，300 s 不冻结

| 判据 | power=6（fix172） | **power=7（fix174）** | power=8（fix173） |
| --- | --- | --- | --- |
| 屏幕 | 锁屏 → 桌面 | **卡 logo** | 卡 logo |
| 触摸 | 可用 | 无判据（未到 UI） | 无判据 |
| 175 s 冻结 | 无 | **无**（09:06:25 起 301 s，fastboot 未回） | 无 |

### 69.159.2 ★ UI 断链的临界点 = 6 → 7

| power | 新增动作 | UI | 冻结 |
| --- | --- | --- | --- |
| 0 | 无 | ✅ 桌面 | 无 |
| 3 | CX GDSC + SMMU vote clk | ⚠️ 锁屏（触摸死，瞬时态） | 无 |
| 6 | GMU 时钟树（PLL0/PLL1、hub/gmu 频率） | ✅ 桌面（触摸恢复） | 无 |
| **7** | **+ GMU 冷启动**（fw 加载 / RSC-PDC / CM3 release / HFI startup） | **❌ 卡 logo** | 无 |
| 8 | + GX 上电（HFI `GX_BW_PERF_VOTE` + OOB `GPU_SET`） | ❌ 卡 logo | 无 |
| 9 | + `hw_init → a7xx_cp_init` | ❌ 卡 logo | **❌ 175 s 全核冻结** |

⇒ **UI 断链由 GMU 冷启动本身引入**，与 GX 上电无关（8 相对 7 没有变差）；
**冻结由 9 独有的 hw_init/CP 段引入**（§69.158.2）。两个问题彼此独立，
必须分别修。

### 69.159.3 `gmuColdStart` 的结构与日志指纹（`GpuPowerProbe.cpp:2449`）

冷启动在 `probeThread` 里、`gpb_run`（含 mode 6 时钟 bring-up）之后被调用，
前置门槛很严（`:2660`）：

```c
const bool idle = gmu && state[0]==1 && !state[1] && !state[2] && !state[3];
if (o.step == 7 && !o.failed && parse == ShengGmuDbSuccess && build == AdrenoHfiStartupSuccess && idle)
    gmuColdStart(r);
else { setRow(7, "S skip c<step>/<failed> p<parse> s<build> i<idle>"); }
logRows("COLD");
```

- 若门槛不满足，**根本不会进冷启动**，row7 打 `S skip …`；这本身也是一种
  "卡 logo" 的成因（因为 mode 7 的其他副作用？不，门槛不过就等于 mode 6 的行为，
  所以**前提是这轮确实进了冷启动**——要靠日志确认）。
- 进得去之后的失败点都有独立行文本：`S nolink` / `S nofw` / `S nomap w<u>` /
  `S payloads <err>` / `S nomap payload` / `S bind refused` / `S ao outside window`，
  然后 `S running` → `ShengGmuColdStart()` → 结果落在 row4/6/7。
- **`logRows("COLD")` 的日志指纹**（`GpuPowerProbe.cpp:261`）：
  `MUYU COLD X7 <row7>`、`MUYU COLD X6 <row6>`、`MUYU COLD D0..D5 <diag>`。
  其中 X7 是 `S<status> ph<phase> h<hash> f<FW_INIT> H<HFI> r<fwready><hfready>
  a<AO> c<CM3> v<reset> d<dtcm> q<polls>`，X6 是
  `P<proto> g<stage> k<ackmask> e<fwerr> t<transport> n<cmds> <ms> s<skip> h<> r<>`。
- ⇒ **拉日志、grep `MUYU COLD`，就能读出冷启动走到哪一步、成功还是失败。**
  这是下一步唯一的判据来源，不需要先猜。

### 69.159.4 下一步：先取证据，再改代码

设备此刻停在 logo、系统活着（看门狗在喂），`autoreset=3600` 会在 **10:06:20**
自己复位 —— 不必等它，硬复位后用既有流程取窗口：

1. 硬复位 → fastboot；
2. `muyu-flash-mode.sh stock` 还原原厂 vendor_boot；
3. `fastboot reboot recovery` → `dd if=/dev/block/sda32 bs=4096 skip=58980347 count=8192`
   → `read-sheng-log.py --decode`；
4. 读 `MUYU COLD` 三行 + `MUYU GPU` / `MUYU PWR`，判两件事：
   - **是否进了冷启动**（X7 是 `S skip …` 还是 `S<status> …`）；
   - **status 是 0（成功）还是非 0（失败在哪一段）**。

两条分岔的修法完全不同：

| 日志显示 | 含义 | 修法方向 |
| --- | --- | --- |
| `S skip …`（没进冷启动） | 卡 logo 与冷启动无关，是 mode 7 独有的**门槛判定**本身 | 查 `o.step==7`/`idle` 判定为何不成立，以及 skip 路径的副作用 |
| `S<0> ph…`（成功） | GMU firmware 跑起来了，**是"GMU 在运行"这件事破坏 UI** | 查 RSC/PDC 接管与 AO 中断；考虑冷启动后立即 hold 住 GX/CX 供电，或把 HFI 心跳补上 |
| `S≠0`（失败在某阶段） | 冷启动失败但 GMU 被留在半启动的坏状态 | 按 phase 定位（fw 加载 / FW_INIT / HFI handshake / reset verify），失败时**回滚 GMU 到 reset 并断电** |

## §69.160 ★ 更正 + fix174 日志取证：power=7 的 GMU 冷启动**成功**，卡的是启动进度不是显示

### 69.160.1 先更正一处表述（用户 09:20 指正）

§69.159 里我写"卡 logo"，容易被读成"停在 UEFI 那帧画面"。**实际是苹果 logo**，
两者必须分清，因为含义完全不同：

| 现象 | 特征 | 含义 |
| --- | --- | --- |
| **UEFI 卡死**（08:34 / 08:36 / 08:39 三次） | 屏幕停在 `ssusb_phy_init_success lane:0*0` 文字，USB 全不枚举 | XNU 根本没起来，§69.156.1 的启动器用错 |
| **苹果 logo 卡住**（fix173 / fix174） | 苹果 logo + 进度条，内核在跑、touch 探针在跑、`[log] tick` 在走 | XNU 起来了、**苹果 logo 是内核自己画上屏的**、显示扫描输出正常 |

⇒ **苹果 logo 能上屏这件事本身就证明：显示通路（framebuffer + 扫描输出 + 画面更新）
在 power=7 下是好的。** 卡住的是**启动流程**，不是显示。§69.159.4 里
"MDSS 丢了扫描输出"那一支可以整支划掉。

### 69.160.2 fix174（power=7）取证：冷启动完全成功，系统活了 9 分钟

`logs/pwr7-fix174-logs/muyu-8dc0bc0a`（header：`kernel=544s msgbuf_reaches=536s`）。

```
[  85.101050] MUYU PWR gcc=ok cc=ok sm=ok gp=skip io=0 pw11 en=0 ahb=00000001>00000001 pll=0060257f
[  85.102932] MUYU PWR cx=0022f801>f822f800 hw=80000202 gx=0022f801 cc_ahb=00000000 cc_gmu=80002001 sv=00000001
[  85.179237] MUYU COLD X7 S0 ph7 h1 f100 H1 r11 a0 c1 v0 d40050001 q157
[  85.180130] MUYU COLD X6 P0 g12 k606 e0 t0 n4 1ms s0 h0 r0
[  85.181023] MUYU COLD D0 P ok ok ok skip io0 pw11
[  85.182803] MUYU COLD D2 R gf12/12 m0 cx3/3 m0
```

逐字段读法（对照 §69.159.3 的指纹）：

| 字段 | 值 | 含义 |
| --- | --- | --- |
| `S0` | `ShengGmuSuccess` | **冷启动整体成功** |
| `ph7` | phase 7 | 走完 1..7 全部阶段（RSC → fw 装载 → 配置 → 释放复位 → FW_INIT → HFI） |
| `h1` | hash verified | 固件 SHA-256 校验通过 |
| `f100` | FW_INIT_RESULT = 0x100 | CM3 报 FW_INIT 就绪 |
| `H1` / `r11` | HFI status 1，fw+hfi 双 ready | HFI 会话建立 |
| `a0` / `c1` | AO fault 0 / CM3 fault 1 | 无 AO 故障 |
| `P0 g12 k606 e0 t0 n4 1ms` | 协议成功、12 阶段、ackmask 0x606、固件 0 错误、transport 0、4 条命令、1 ms | HFI 协议干净 |
| `gx=0022f801` | bit31 = 0 | **GX 未上电**（符合 power=7 的定义，8 才拉 GX） |

**同时它没冻结**：`[log] tick 423s / 454s / 514s`（间隔 31/27/33 s，规律），
最后一条日志 536.67 s。⇒ §69.158.2 的结论加强：**175 s 全核冻结只由 power=9
独有的 hw_init/CP 段引入**，与冷启动无关。

### 69.160.3 卡住的证据：合成员从 200 s 起就不再提交帧

```
[ 193.777042] MUYU OFFER n=1 acc=0 pres=0 tc=0
[ 197.424214] MUYU OFFER n=2 acc=0 pres=0 tc=1
[ 200.098965] MUYU OFFER n=3 acc=0 pres=0 tc=2
```

- 这条打印是限速的：`n<=3` 每次打，之后每 64 次打一次（`LegacyDisplayProbe.cpp:70`）。
  9 分钟里**只出现到 n=3** ⇒ 整个 536 s 内 offer 调用 **< 64 次**，而正常 UI
  每秒都会提帧 ⇒ **合成员（WindowServer/backboardd）从 200 s 后就基本不再产帧**。
- `tc` 依次 0/1/2 递增 ⇒ CPU 拷贝通路本身是通的（不是显示驱动坏了），
  只是**上游没人喂帧**。
- 全日志 `SpringBoard` 仅 5 次、`backboardd` 4 次、`IOGPU`/`AGX`/`Metal` 全 0；
  `IconState`/`desktop`/`AppPlaceholder` 全 0 ⇒ 从未进入图标/桌面构建阶段。

### 69.160.4 顺手排掉一个假线索

`process SpringBoard[26] caught waking the CPU 45242 times over ~12 s` 看起来很像
"SpringBoard 死循环"，但四轮全都有，量级一致：

| 轮次 | waking 计数 |
| --- | --- |
| fix166（power=9，金属开） | 47252 / 45007 / 45030 |
| fix167（power=9，金属关） | 47169 / 45104 / 45046 |
| fix168（power=9，无 fb-gpu） | 46868 / 45015 / 45239 |
| fix174（power=7） | 45578 / 45242 / 45015 |

⇒ 属于这个 port 的**基线噪声**（大概是 SpringBoard 在等功能时的忙等），
与 GPU 电源阶梯无关，**不能当判据**。同理 `vnode jetsam` 在 §69.153.6 已判为结果而非原因。

### 69.160.5 新增工具：`sheng-gpu-coldstop=<n>`（冷启动内部分段）

`power` 只能整档 1..9，7 是"整个冷启动"，无法再细分。为此加了一个**只用于
bring-up 的临时开关**，让一次启动就能回答"冷启动的哪一段把 UI 弄死"：

- `ShengGmu.h`：`extern int sheng_gmu_coldstop_level;`（详细契约写在声明上方）。
- `ShengGmu.c`：`coldstop_reached(phase,r)`，在各阶段末尾
  **`return ShengGmuBusy` 且不走 `reset_after_failure()`** —— 故意的：
  实验要看的正是"这一段写完之后留下的状态"，回滚了就等于测别的问题。
- 阶段划分（`ShengGmuLoadAndStartHfi`）：

| 值 | 停在哪 | 已写入硬件的东西 |
| --- | --- | --- |
| 2 | RSC/PDC 设置写完 + FAL/TCM_RET 握手 + AO 掩码之后 | 19 条 `sheng_gmu_rsc_setup`、CX_MISC TCM_RET、FALNEXT/FAL |
| 3 | 15 个固件块装入 ITCM/DTCM/icache 并逐字回读校验之后 | + TCM 固件内容、payload 内存清零 |
| 4 | 启动配置寄存器写入并回读校验之后（CM3 仍在复位） | + HFI QTBL、CM3_CFG、chip id、log iova、AHB fence、GBIF QSB |
| 5 | 释放 CM3 复位且 FW_INIT 已观察之后（尚未发 HFI_CTRL_INIT） | + CM3 真的在执行固件 |
| 0/缺省 | 完整冷启动（现状） | 全部 + HFI 会话 + 4 条命令 |

- `GpuPowerProbe.cpp` 在调用 `ShengGmuColdStart()` 前解析
  `sheng-gpu-coldstop`，结果会体现为 row7 的 `S3 ph<n>`（`3 = ShengGmuBusy`）。
- **`sheng-gpu-coldstop` 与 `sheng-gpu-power` 是正交的**：前者只在后者 ≥7 时有意义。

### 69.160.6 本轮动作

- **对照组（power=6）**：09:21:30 刷 `fw/vendor_boot_gpu_fix172-pwr6.img`，
  目的是拿到项目**第一份"能进桌面"的日志**（此前所有留存的窗口日志全是失败轮：
  fix32/111-149 那一批 `IconState` 也全是 0）。判据：`MUYU OFFER` 的 n 是否
  远超 64、`IconState`/`desktop` 是否出现、200 s 附近在做什么。
- **fix175**：`power=7` + `sheng-gpu-coldstop=2`，sha `5201be1a…`，args **997 B**
  （1016 B 硬闸内）。三链 rc=0（`logs/build-*-fix175.log`）。
  判据：**如果 UI 恢复正常（进桌面），凶手就在阶段 2 那批写里（RSC/PDC 或
  AO 掩码）；如果仍然卡苹果 logo，凶手在 3..5（固件装载 / CM3 执行 / HFI）**。
