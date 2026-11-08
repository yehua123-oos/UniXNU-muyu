# The iPadOS 17 line is a tag, not a board

The repository has one board (`board.toml`, format 2, iPadOS 27.0.1 / 24A446). Until 2026-11-08 it
had two board files and the 27-line one had to be copied over the other before a build, which is
how the 17 line's configuration kept being mistaken for the live one. That is gone: the 17 line
lives at the annotated tag **`line-ipados-17`** (commit `e105fa0`, the last state in which
`board.toml` was the 21H461 board).

## Build it

```bash
git worktree add ../UniXNU-muyu-17 line-ipados-17
cd ../UniXNU && ./unixnu defconfig muyu-userland_defconfig -D ../UniXNU-muyu-17
./unixnu build kernel -D ../UniXNU-muyu-17 --kernelcache out/ipsw/21H461/kernelcache.decompressed
```

The framework checkout is shared: `../UniXNU` after the 2026-11-08 merge is the single checkout for
both lines (`UniXNU-27` is frozen). A format 1 device writes to `out/muyu/`, **not** to
`out/muyu/ipados-27.0.1-24A446/muyu-userland/`, so the two lines cannot overwrite each other's
kernel.

## What is different in that tag

| | 17 line (`line-ipados-17`) | 27 line (main) |
| --- | --- | --- |
| `board.toml` | format 1: no `schema_version`, no `soc_source` / `os_profile` | format 2 |
| Patch series | the build globs `patches/xnu/*.patch`; the directory is the series | four named layers, `device:patches/xnu-27/series` |
| Payload ADT | `[payload.adt] tool = scripts/make-muyu-adt.py` | generated from `dts/muyu.dts` |
| Components | resolved from local `drivers/` copies | `[components] include` against the catalog |
| Output | `out/muyu/` | `out/muyu/<os_profile>/<config>/` |
| Board identity in `porting.toml` | not recorded | `device.os_profile = ipados-27.0.1-24A446` |

`patches/xnu/series` in the main tree lists that line's 16 patches in filename order. It is there so
the layer is stated rather than implied; the format 1 board never reads it, and nothing in it is
applied to a 27-line kernel.

## Why not keep both lines in one board

The framework supports it — `[os_overrides."ipados-17.7.11-21H461"]` plus `[package.defconfig]`
maps each OS profile to its own defconfig, and `--os 17` selects it. The blocker is the ADT path:
format 2 always compiles the hardware tree from `dts/` and generates the ADT, while the 17 line's
working images were built from the `scripts/make-muyu-adt.py` script path. Merging the two means
re-qualifying the 17 line's boot on a DTS-generated ADT, which is real work with a real chance of
losing a booting line. Until someone does that work, the tag is the honest place for it.

## What would bring it back

1. Compile `dts/muyu.dts` and diff the generated ADT against the one `make-muyu-adt.py` produced
   for the last 21H461 image.
2. Add the 17-line values as `[os_overrides."ipados-17.7.11-21H461"]` (kernel archives, boot
   arguments) and `[package.defconfig]` for its defconfig.
3. Re-verify one boot, then record it in `porting.toml` under `configuration` with the commit it
   was seen at.
