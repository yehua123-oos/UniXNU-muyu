#!/usr/bin/env python3
"""Diagnose / repair the 8 KiB SHNGROOT manifest at the head of `userdata`.

This is the scripted form of the 2026-11-12 19:45 repair (GPU-HANDOFF §57.1,
cold-start note §9 item 6: "do not rely on memory until the fix is scripted").

The iPadOS root image lives in the *userdata* partition as an Android sparse
image. Its logical block 0 is a SHNGROOT manifest. Something external (most
likely stock recovery mounting/formatting /data while charging) has twice
overwritten blocks 0/1. NativeUfs::startRoot() then returns false, no root
media is published, IOFindBSDRoot() waits forever and the panel shows
`NativeUfs: no valid root manifest in userdata (status 1, ...)`.

The fix is NOT a 9 GB full reflash of root-image. Only the first 8192 bytes
are wrong: block 0 is the manifest (copied verbatim from the pristine sparse
backup) and block 1 is 4096 zero bytes (it is already zero in the backup).

SAFETY: this tool is READ-ONLY by default. `prepare` only writes a local file.
`diagnose` only reads. Nothing touches the device unless `fix --apply --yes`
is given explicitly, and even then it writes exactly 8192 bytes to one block
device. It never runs fastboot flash / reboot, and never writes boot/recovery.

Usage (offline, no device needed):
    python3 scripts/repair-userdata-manifest.py prepare
    python3 scripts/repair-userdata-manifest.py prepare --output /tmp/ud_fix.bin
    python3 scripts/repair-userdata-manifest.py diagnose --head logs/<head>.bin
    python3 scripts/repair-userdata-manifest.py verify --readback /tmp/ud_verify.bin
    python3 scripts/repair-userdata-manifest.py commands
    python3 scripts/repair-userdata-manifest.py fix --apply --yes   # DEVICE WRITE
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import subprocess
import sys

BLOCK = 4096
FIX_BYTES = 2 * BLOCK
SPARSE_MAGIC = 0xED26FF3A
CHUNK_RAW = 0xCAC1
EXPECTED_FIX_SHA256 = '1afe7d01a7a7a94d44381ae433171c60f891c92c340f107b4e4b93328c78143d'

ROOT = Path(__file__).resolve().parents[2]          # .../pad7pro-xnu
SPARSE = ROOT / 'fw/userdata-backup/userdata-root.sparse'
CLOBBERED = ROOT / 'logs/2026-11-12-userdata-head-clobbered.bin'
DEVICE_USERDATA = '/dev/block/sda32'               # confirmed under stock recovery, §57.1
OUT_DIR = ROOT / 'UniXNU/out/muyu/tree/build/userdata-manifest'


def die(msg):
    print(f'ERROR: {msg}', file=sys.stderr)
    raise SystemExit(2)


def read_sparse_head(path, nbytes=65536):
    """Logical first `nbytes` of an Android sparse image (chunk0 must be RAW)."""
    with path.open('rb') as f:
        header = f.read(28)
        if len(header) != 28:
            die(f'{path}: truncated sparse header')
        magic, major, minor, hsz, chsz, blk, blocks, chunks, _ = struct.unpack('<IHHHHIIII', header)
        if magic != SPARSE_MAGIC or major != 1 or hsz != 28 or chsz != 12 or blk != BLOCK:
            die(f'{path}: unsupported sparse header (magic={magic:#x} v{major}.{minor} blk={blk})')
        ch = f.read(12)
        if len(ch) != 12:
            die(f'{path}: truncated chunk header')
        kind, _, span, total = struct.unpack('<HHII', ch)
        if kind != CHUNK_RAW:
            die(f'{path}: chunk0 is not RAW (type={kind:#x}); refusing to guess')
        if total < 12 + nbytes or span * BLOCK < nbytes:
            die(f'{path}: chunk0 too small to cover {nbytes} bytes')
        data = f.read(nbytes)
        if len(data) != nbytes:
            die(f'{path}: short chunk0 read')
        return data, {'sparse_magic': hex(magic), 'major': major, 'minor': minor,
                      'block_size': blk, 'total_blocks': blocks, 'chunks': chunks,
                      'chunk0_type': hex(kind), 'chunk0_blocks': span,
                      'chunk0_total_bytes': total}


def parse_manifest(block0):
    """Parse + self-verify one SHNGROOT manifest block."""
    out = {'magic': block0[:8].decode('latin1'), 'valid': False}
    if block0[:8] != b'SHNGROOT':
        out['reason'] = 'magic is not SHNGROOT'
        return out
    _, version, count, image_bytes = struct.unpack_from('<8sIIQ', block0, 0)
    out.update(version=version, windows=count, image_bytes=image_bytes)
    if version != 1 or not (1 <= count <= 4):
        out['reason'] = f'bad version/count ({version}/{count})'
        return out
    wins = []
    for i in range(count):
        name, off, size, _, _, sha = struct.unpack_from('<32sQQII32s', block0, 24 + 128 * i)
        wins.append({'name': name.split(b'\0')[0].decode('latin1'),
                     'offset': off, 'size': size, 'sha256': sha.hex()})
    out['window_list'] = wins
    checksum_ok = hashlib.sha256(block0[:4064]).digest() == block0[4064:4096]
    out['self_checksum_ok'] = checksum_ok
    out['valid'] = checksum_ok
    if not checksum_ok:
        out['reason'] = 'block0 trailing sha256 mismatch'
    return out


def build_payload(sparse_head):
    """The exact 8192 bytes written on 2026-11-12."""
    block0 = sparse_head[:BLOCK]
    info = parse_manifest(block0)
    if not info['valid']:
        die(f'pristine backup manifest is not valid: {info.get("reason")}')
    block1 = sparse_head[BLOCK:FIX_BYTES]
    if any(block1):
        die('pristine backup block1 is not all zero; refusing to emit a payload')
    return block0 + b'\0' * BLOCK, info


def cmd_prepare(args):
    if not SPARSE.is_file():
        die(f'missing pristine backup {SPARSE}')
    head, meta = read_sparse_head(SPARSE)
    payload, info = build_payload(head)
    out = Path(args.output) if args.output else OUT_DIR / 'ud_fix.bin'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(payload)
    sha = hashlib.sha256(payload).hexdigest()
    result = {
        'action': 'prepare (LOCAL FILE ONLY; no device access)',
        'source': str(SPARSE.relative_to(ROOT)),
        'sparse_header': meta,
        'manifest': info,
        'output': str(out),
        'bytes': len(payload),
        'sha256': sha,
        'matches_2026_11_12_fix': sha == EXPECTED_FIX_SHA256,
        'device_written': False,
    }
    if not result['matches_2026_11_12_fix']:
        die(f'payload sha {sha} != recorded 2026-11-12 sha {EXPECTED_FIX_SHA256}')
    (OUT_DIR / 'prepare.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return result


def diagnose_bytes(data, label):
    """Compare a device head dump against the pristine logical image."""
    if not SPARSE.is_file():
        die(f'missing pristine backup {SPARSE}')
    pristine, _ = read_sparse_head(SPARSE, len(data))
    payload, manifest = build_payload(pristine)
    nblocks = min(len(data), len(pristine)) // BLOCK
    differing = [i for i in range(nblocks)
                 if data[i * BLOCK:(i + 1) * BLOCK] != pristine[i * BLOCK:(i + 1) * BLOCK]]
    magic = data[:8]
    status = ('ok' if not differing else
              'manifest-only (blocks 0/1)' if differing == [0, 1] or
              all(i < 2 for i in differing) else 'wider than 8 KiB')
    return {
        'action': 'diagnose (READ-ONLY)',
        'input': label,
        'bytes_examined': len(data),
        'blocks_compared': nblocks,
        'first_8_bytes': magic.hex(),
        'first_8_ascii': magic.decode('latin1'),
        'pristine_manifest': manifest,
        'differing_blocks': differing,
        'blocks_2_to_15_match_pristine': all(
            data[i * BLOCK:(i + 1) * BLOCK] == pristine[i * BLOCK:(i + 1) * BLOCK]
            for i in range(2, min(16, nblocks))),
        'verdict': status,
        'recommended': ('no repair needed; manifest is intact'
                        if status == 'ok' else
                        '8 KiB targeted repair (fix --apply --yes in stock recovery)'
                        if status == 'manifest-only (blocks 0/1)' else
                        'STOP: damage is not limited to blocks 0/1; re-check before any write'),
        'device_written': False,
    }


def cmd_diagnose(args):
    p = Path(args.head)
    if not p.is_file():
        die(f'head dump not found: {p}')
    data = p.read_bytes()
    if len(data) < FIX_BYTES:
        die(f'head dump is only {len(data)} bytes; need >= {FIX_BYTES}')
    result = diagnose_bytes(data[:max(len(data), 65536)][:len(data)], str(p))
    (OUT_DIR / 'diagnose.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    return result


def cmd_verify(args):
    p = Path(args.readback)
    if not p.is_file():
        die(f'readback not found: {p}')
    data = p.read_bytes()
    if not SPARSE.is_file():
        die(f'missing pristine backup {SPARSE}')
    pristine, _ = read_sparse_head(SPARSE)
    payload, manifest = build_payload(pristine)
    result = {
        'action': 'verify readback (READ-ONLY)',
        'input': str(p), 'bytes': len(data),
        'sha256': hashlib.sha256(data).hexdigest(),
        'matches_payload': data == payload,
        'manifest_after': parse_manifest(data[:BLOCK]),
        'device_written': False,
    }
    print(json.dumps(result, indent=2))
    if not result['matches_payload']:
        die('readback does NOT match the payload; do not proceed')
    return result


def cmd_commands(args):
    payload = Path(args.payload) if args.payload else OUT_DIR / 'ud_fix.bin'
    print(f"""# Exact 2026-11-12 19:45 repair (GPU-HANDOFF §57.1). PRINTED ONLY, NOT EXECUTED.
#
# PRECONDITIONS (§2.1 red lines): device in fastboot, stock vendor_boot already
# flashed, stock recovery entered by RAM boot (fastboot boot, never flash boot).
# /data must NOT be mounted in recovery; only /metadata was mounted last time.
# NEVER use 'fastboot reboot'. NEVER write boot/recovery outside restore-stock --yes.

# 0) offline: build the 8192-byte payload (local file only)
python3 UniXNU-muyu-17/scripts/repair-userdata-manifest.py prepare

# 1) put the device in stock recovery (RAM boot; power loss clears it)
bash tools/muyu-flash-mode.sh stock
fastboot boot fw/stock/boot.img
adb wait-for-device
adb shell 'getprop | grep -i recovery'
adb shell 'mount | grep -E "/data|/metadata"'     # /data must be absent
adb shell 'ls -l /dev/block/by-name/userdata'      # confirm {DEVICE_USERDATA}

# 2) READ-ONLY evidence first (§56.7): dump 64 KiB and diagnose before any write
adb exec-out 'dd if={DEVICE_USERDATA} bs=4096 count=16' > /tmp/ud_head_before.bin
python3 UniXNU-muyu-17/scripts/repair-userdata-manifest.py diagnose --head /tmp/ud_head_before.bin

# 3) ONLY if diagnose says "manifest-only (blocks 0/1)": write exactly 8192 B
adb push {payload} /tmp/ud_fix.bin
adb shell 'sha256sum /tmp/ud_fix.bin'              # must equal {EXPECTED_FIX_SHA256}
adb shell 'dd if=/tmp/ud_fix.bin of={DEVICE_USERDATA} bs=4096 count=2 conv=notrunc; sync'
adb exec-out 'dd if={DEVICE_USERDATA} bs=4096 count=2' > /tmp/ud_verify.bin
python3 UniXNU-muyu-17/scripts/repair-userdata-manifest.py verify --readback /tmp/ud_verify.bin

# 4) success criteria on the next XNU panel: BSD s=8 / INIT q=6 / ROOT c=6/0/0
# 5) before charging: bash tools/muyu-flash-mode.sh stock
""")


def cmd_fix(args):
    if not (args.apply and args.yes):
        print('REFUSING: fix requires both --apply and --yes. Nothing was written.', file=sys.stderr)
        print('Re-read `commands` output and confirm the device is in stock recovery.', file=sys.stderr)
        raise SystemExit(3)
    payload = Path(args.payload) if args.payload else OUT_DIR / 'ud_fix.bin'
    if not payload.is_file():
        die(f'payload missing; run prepare first ({payload})')
    head_check = subprocess.run(['adb', 'shell', 'mount'], capture_output=True, text=True)
    if head_check.returncode or '/data ' in head_check.stdout:
        die('adb unavailable or /data is mounted in recovery; refusing to write')
    pushed = subprocess.run(['adb', 'push', str(payload), '/tmp/ud_fix.bin'], capture_output=True, text=True)
    if pushed.returncode:
        die(f'adb push failed: {pushed.stderr.strip()}')
    remote = subprocess.run(['adb', 'shell', 'sha256sum /tmp/ud_fix.bin'],
                            capture_output=True, text=True)
    if EXPECTED_FIX_SHA256 not in remote.stdout:
        die(f'remote payload sha mismatch: {remote.stdout.strip()}')
    cmd = (f"dd if=/tmp/ud_fix.bin of={DEVICE_USERDATA} bs=4096 count=2 conv=notrunc; sync")
    wrote = subprocess.run(['adb', 'shell', cmd], capture_output=True, text=True)
    if wrote.returncode:
        die(f'dd failed: {wrote.stderr.strip()}')
    out = Path(args.readback or '/tmp/ud_verify.bin')
    with out.open('wb') as sink:
        readback = subprocess.run(['adb', 'exec-out', f'dd if={DEVICE_USERDATA} bs=4096 count=2'],
                                  stdout=sink, stderr=subprocess.PIPE)
    if readback.returncode:
        die(f'readback failed: {readback.stderr.decode(errors="replace").strip()}')
    print(f'wrote 8192 B to {DEVICE_USERDATA}; readback at {out}')
    cmd_verify(argparse.Namespace(readback=str(out)))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='command')
    p = sub.add_parser('prepare', help='build the 8192 B payload locally (no device)')
    p.add_argument('--output')
    p.set_defaults(func=cmd_prepare)
    p = sub.add_parser('diagnose', help='read-only comparison of a device head dump')
    p.add_argument('--head', required=True)
    p.set_defaults(func=cmd_diagnose)
    p = sub.add_parser('verify', help='verify an 8192 B readback')
    p.add_argument('--readback', required=True)
    p.set_defaults(func=cmd_verify)
    p = sub.add_parser('commands', help='print the exact repair commands (never executes)')
    p.add_argument('--payload')
    p.set_defaults(func=cmd_commands)
    p = sub.add_parser('fix', help='DEVICE WRITE: requires --apply --yes')
    p.add_argument('--apply', action='store_true')
    p.add_argument('--yes', action='store_true')
    p.add_argument('--payload')
    p.add_argument('--readback')
    p.set_defaults(func=cmd_fix)
    args = ap.parse_args()
    if not getattr(args, 'func', None):
        ap.print_help()
        return
    args.func(args)


if __name__ == '__main__':
    main()
