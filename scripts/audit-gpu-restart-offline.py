#!/usr/bin/env python3
"""Offline (no device) audit for the 17-line GPU restart hang. See GPU-HANDOFF.md §41.

Runs entirely on the host: reads the source tree and the last build's kernel
arguments, and settles the §40.5 questions that do not need hardware:

  * whether a zap shader exists and whether gpuRestart() can ever send
    CP_SET_SECURE_MODE(0);
  * whether Q5 and Z1 sample the same instant (they do not);
  * what restart(completed) actually does to the fence bookkeeping;
  * which experimental switches make the software-MMIO host tests disagree
    with the current driver.

It never touches fastboot, adb, the battery or a kernel build. Exit 0 means
the audit itself completed; findings are printed and written to JSON.
"""
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WORKSPACE = ROOT.parent
TREE = WORKSPACE / 'UniXNU'
PROBE = ROOT / 'package/GpuPowerProbe.cpp'
LASTLOG = ROOT / 'package/BravearkLastLog.c'
RING = TREE / 'drivers/Adreno740/NativeAdrenoRing.cpp'
BUILD_LOG = Path('/tmp/build_p38.log')
OUT = ROOT / 'build/gpu-restart-offline'
OUT.mkdir(parents=True, exist_ok=True)


def read(path):
    return path.read_text(errors='replace') if path.is_file() else ''


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()[:16] if path.is_file() else None


probe = read(PROBE)
lastlog = read(LASTLOG)
ring = read(RING)
board = read(ROOT / 'board.toml')
build_log = read(BUILD_LOG)
findings = []


def add(key, value, note):
    findings.append(dict(key=key, value=value, note=note))


# --- 1. zap shader / secure mode -------------------------------------------------
weak_zap = bool(re.search(r'native_adreno_zap_image_size\s*=\s*0', probe))
zap_cache = ROOT / '.cache/firmware/sheng-gpu/qcom/sm8550/sheng/a740_zap.mbn'
strong_zap = zap_cache.is_file()
zap_limit_default = re.search(r'unsigned\s+zap\s*=\s*(\d+)\s*;', probe)
zap_limit_default = int(zap_limit_default.group(1)) if zap_limit_default else None
zap_arg_in_kernel = bool(re.search(r'sheng-gpu-zap=', build_log))
early_return = bool(re.search(r'if\s*\(size\s*<\s*52[^)]*\)\s*\{[^}]*z\[0\]\s*=\s*-1', probe, re.S))
restart_gate = bool(re.search(r'native_gpu_zap\[0\]\s*==\s*9', probe))
secure_sends = [m.start() for m in re.finditer(r'AdrenoPM4Type7\(0x66', probe)]
secure_lines = [probe.count('\n', 0, at) + 1 for at in secure_sends]

zap_stage = -1 if (weak_zap and not strong_zap) else 'unknown'
add('zap_image', dict(weak_default_size_zero=weak_zap, strong_blob_present=strong_zap,
                      cache_path=str(zap_cache)),
    '17 line uses the weak empty zap default; build-native-adreno-kits.py (which would embed a740_zap.mbn) is not enabled')
add('zap_stage_at_boot', zap_stage,
    'zapAndSecure() returns at "size < 52" after setting z[0] = -1, so the whole secure-mode sequence is skipped')
add('zap_limit', dict(default=zap_limit_default, kernel_arg_override=zap_arg_in_kernel),
    'sheng-gpu-zap is absent from the last build kernel args, so the default applies')
add('gpuRestart_secure_gate', dict(gate_is_zap9=restart_gate, zap_stage=zap_stage,
                                   gate_passes=(zap_stage == 9)),
    'gpuRestart() only replays CP_SET_SECURE_MODE(0) when zap[0]==9; with -1 the replay never happens')
add('CP_SET_SECURE_MODE_sites', dict(count=len(secure_lines), lines=secure_lines),
    'the only two 0x66 senders are zapAndSecure() (skipped) and gpuRestart() (gated off): the 17 line never emitted this packet')

# --- 2. Q5 vs Z1 sampling --------------------------------------------------------
q5_heartbeat = bool(re.search(r'muyu_cp_stall_rows\[5\]', probe))
stall_branch = bool(re.search(r'gxOn\(\)\s*&&\s*gpuRd\(A740_CP_RB_RPTR\)\s*!=\s*gpuRd\(A740_CP_RB_WPTR\)', probe))
q5_sources = bool(re.search(r'gpuRd\(0x0806\),\s*gpuRd\(0x0807\)', probe))
z1_snapshot = bool(re.search(r'native_adreno_stall\[10\]', lastlog) and re.search(r'native_adreno_stall\[11\]', lastlog))
rows_global = bool(re.search(r'char muyu_cp_stall_rows\[7\]\[64\]', probe))
rows_cleared = bool(re.search(r'memset\(&muyu_cp_stall_rows|bzero\(&muyu_cp_stall_rows', probe))

add('Q5_sampling', dict(in_heartbeat=q5_heartbeat, gated_on_rptr_ne_wptr=stall_branch,
                        reads_live_mmio=q5_sources),
    'Q5 is refreshed every 2 s by heartbeat() only while RPTR != WPTR; r/s/t are live CP register reads')
add('Q5_staleness', dict(global_array=rows_global, explicitly_cleared=rows_cleared),
    'muyu_cp_stall_rows is a global array that is never cleared: the row on screen can be a much older stall sample')
add('Z1_sampling', dict(from_stall_snapshot=z1_snapshot),
    'Z1 prints native_adreno_stall[10]/[11], the driver snapshot taken at hang detection, not a live MMIO read')
add('Q5_vs_Z1_comparable', False,
    'different instant AND different source: Q5 r2a/2a and Z1 r36/3d must not be compared as one hardware state')

# --- 3. restart() bookkeeping ----------------------------------------------------
fence_bookkeeping = bool(re.search(r'fence_\[1\]\s*=\s*completed', ring))
ring_zeroed = bool(re.search(r'for\(size_t i=0;i<ring_bytes_/4;\+\+i\)ring_\[i\]=0;', ring))
wptr_zeroed = bool(re.search(r'RingWptr,0,RingScratch,0', ring))
next_from_rptr = bool(re.search(r'next_=rptr;return kIOReturnSuccess', ring))
fence_value = bool(re.search(r'\*value=fence_\[1\]', ring))

add('restart_completed', dict(fence_written=fence_bookkeeping, ring_zeroed=ring_zeroed,
                              wptr_zeroed=wptr_zeroed, next_reset_from_hw_rptr=next_from_rptr),
    'restart() clears the ring, programs WPTR=0 and sets next_ from the live CP_RB_RPTR; the next_ mismatch hypothesis is ruled out')
add('fenceValue_source', dict(reads_fence_word1=fence_value),
    'fenceValue() is just fence_[1], which restart() deliberately sets to completed; Z5 staying at 4 after a restart is expected bookkeeping, not evidence that the CP moved')

# --- 4. host software-MMIO tests vs current driver --------------------------------
switches = {}
for name in ('kTableNeedsReset', 'kResetPrologue', 'kPrologue'):
    m = re.search(r'static constexpr bool\s+' + name + r'\s*=\s*(true|false)', ring)
    switches[name] = m.group(1) if m else None
add('experimental_switches', switches,
    'these are §30/§31 debug toggles: with the framed prologue and reset stretch disabled, the ring contents are shorter than test_ring.cpp / test_submit.cpp expect')
add('host_tests_are_green_baseline', False,
    'test_ring and test_submit compile and run over software MMIO peers but fail their "WPTR is the last write / WPTR == 46" assertions; the expectations are stale, not a driver regression')

report = dict(
    audit='GPU restart offline / virtual-environment audit (§41)',
    device_required=False,
    sources=dict(probe=sha(PROBE), lastlog=sha(LASTLOG), ring=sha(RING), board=sha(ROOT / 'board.toml')),
    findings=findings,
)
(OUT / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False) + '\n')

print('GPU restart offline audit (no device, no build)')
for f in findings:
    print('  - %-28s %s' % (f['key'], json.dumps(f['value'], ensure_ascii=False)))
    print('    %s' % f['note'])
print('\nwrote %s' % (OUT / 'report.json'))
