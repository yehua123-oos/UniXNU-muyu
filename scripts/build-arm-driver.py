#!/usr/bin/env python3
"""Compile the generic platform expert against the actual source-kernel ABI."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def board_root():
    # Resolve the board repository (UniXNU-muyu) regardless of where this script lives.
    #  * assembled tree:  ROOT is the tree, the package is kits/muyu (a symlink to the repo).
    #  * board repo:      ROOT is UniXNU-muyu itself.
    #  * sibling layout:  ROOT is the workspace parent; the repo is ROOT/UniXNU-muyu.
    candidate = ROOT / 'kits' / 'muyu'
    if candidate.is_symlink():
        return candidate.resolve().parent
    br = ROOT / 'UniXNU-muyu'
    if (br / 'board.toml').is_file():
        return br
    return ROOT


def fw_root():
    # Harvested firmware / devicetrees live in the shared fw/ checkout beside the board repository.
    br = board_root()
    fw = br / 'fw'
    if fw.is_dir():
        return fw
    shared = br.parent / 'fw'
    if shared.is_dir():
        return shared
    return fw
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('component', choices=['ArmVirtPlatform', 'ArmDTPlatform', 'ArmCpu', 'SoftwareAES', 'VirtioMmioBlock', 'RamBlock', 'GopFramebuffer', 'VirtioMmioConsole', 'NativeLegacyDisplay', 'NativeGOPPresenter', 'NativeOfflineGPU', 'NativeOfflineAccelerator', 'NativeIOGPUProviderProbe', 'ShengUfs', 'ShengHidInput', 'AppleKeyStoreState', 'Adreno740'])
parser.add_argument('--negative-aes-kat', action='store_true', help='Isolated own-source wrong-answer refusal control; never changes original inputs')
parser.add_argument('--verified-root', action='store_true', help='Authenticate all actual disk reads against an independently signed immutable policy')
parser.add_argument('--verified-root-bundle', action='store_true', help='Select exactly one capacity-matching policy per device, then independently authenticate it')
parser.add_argument('--negative-verified-block', action='store_true', help='Negative own-buffer control: corrupt only the completed DMA snapshot')
parser.add_argument('--unverified', action='store_true', help='ShengUfs: compile the bring-up transport without the verified-root policy bundle')
args = parser.parse_args()
if args.verified_root_bundle and not args.verified_root:
    parser.error('A policy bundle requires verified block mode')
if args.verified_root_bundle and args.negative_verified_block:
    parser.error('Select either bundle policy qualification or the isolated DMA snapshot fault')
if (args.verified_root or args.negative_verified_block) and args.component != 'VirtioMmioBlock':
    parser.error('Verified block mode requires the concrete read-only block transport')
if args.negative_verified_block and not args.verified_root:
    parser.error('The isolated input fault requires verified block mode')
if args.unverified and args.component != 'ShengUfs':
    parser.error('--unverified currently applies to the ShengUfs bring-up transport only')
component = args.component
recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(recipe.read_text().strip().rstrip(','))
source = ROOT / 'drivers' / ('GopFramebuffer' if component in ('NativeLegacyDisplay','NativeGOPPresenter') else 'AdrenoMetal' if component in ('NativeOfflineGPU','NativeOfflineAccelerator','NativeIOGPUProviderProbe') else component) / {'Adreno740':'NativeAdrenoService.cpp','SoftwareAES':'NativeAES.cpp','RamBlock':'NativeRamBlock.cpp','VirtioMmioBlock':'NativeVirtioBlock.cpp','VirtioMmioConsole':'NativeVirtioConsole.cpp','ShengUfs':'NativeUfs.cpp','AppleKeyStoreState':'NativeKeyStoreEvents.cpp','ShengHidInput':'NativeHidInput.cpp'}.get(component, component + '.cpp')
out = ROOT / 'build' / {'ArmVirtPlatform': 'armvirt-platform', 'ArmDTPlatform': 'armdt-platform', 'ArmCpu': 'armcpu', 'SoftwareAES': 'native-aes',
                        'VirtioMmioBlock': 'native-virtio-block', 'RamBlock': 'native-ram-block',
                        'GopFramebuffer': 'native-mobile-display', 'VirtioMmioConsole': 'native-virtio-console',
                        'NativeLegacyDisplay': 'native-legacy-display', 'NativeGOPPresenter': 'native-gop-presenter', 'NativeOfflineGPU':'native-offline-gpu','NativeOfflineAccelerator':'native-offline-accelerator','NativeIOGPUProviderProbe':'native-original-provider', 'ShengUfs':'native-ufs', 'ShengHidInput':'native-hid-input', 'AppleKeyStoreState':'native-keystore-events', 'Adreno740':'native-adreno'}[component]
if args.verified_root:
    out = ROOT / ('build/native-virtio-block-verified-fault' if args.negative_verified_block else 'build/native-virtio-block-verified')
    if args.verified_root_bundle:
        out = ROOT / 'build/native-virtio-block-verified-bundle'
out.mkdir(parents=True, exist_ok=True)
if args.negative_aes_kat:
    if component != 'SoftwareAES':
        parser.error('The own-source KAT control requires SoftwareAES')
    out = ROOT / 'build/native-aes-kat-negative'
    out.mkdir(parents=True, exist_ok=True)
    text = source.read_text()
    marker = '0x76,0x49,0xab,0xac'
    if text.count(marker) != 1:
        raise SystemExit('The exact own-source NIST answer was not uniquely identified')
    (out / 'OriginalAESABI.h').write_bytes(source.with_name('OriginalAESABI.h').read_bytes())
    source = out / 'SoftwareAES.cpp'
    source.write_text(text.replace(marker, '0x77,0x49,0xab,0xac'))
destination = out / (component + '.o')
arguments = record['arguments']
command = []
index = 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(destination)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-incompatible-sysroot', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier', '-I', str(ROOT / '.cache/xnu-work/osfmk')])
if component in ('VirtioMmioBlock', 'RamBlock', 'ShengUfs'):
    command += ['-I', str(ROOT / 'build/storage-abi/include')]
    if args.verified_root:
        command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-DNATIVE_VERIFIED_BLOCK=1']
    if args.negative_verified_block:
        command += ['-DNATIVE_VERIFIED_BLOCK_INJECT_FAULT=1']
    if args.verified_root_bundle:
        command += ['-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1']
if component == 'ShengUfs':
    command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-I', str(ROOT / 'drivers/VirtioMmioBlock'),
                '-mgeneral-regs-only']
    if not args.unverified:
        command += ['-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1']
if component=='NativeOfflineGPU':
    command += ['-I',str(ROOT/'drivers/VirtioMmioConsole')]
if component in ('NativeLegacyDisplay','ShengHidInput'):
    command += ['-I', str(out)]
if component == 'Adreno740':
    # SM8635 (muyu) native Adreno: the shared Adreno740 driver plus the board's pinned GMU/SMMU
    # sources. The generated ShengGmu*.h headers live in build/gpu (gen-muyu-gmu-*-source.py); the
    # umbrella ShengGmu.h / ShengGmuCommandDb.h and the iOS 17 GPU-role shim live in the board
    # package (kits/muyu). 21H461 lacks the iOS 26/27 task_get_gpu_role() API, so we force-include
    # MuyuGpuRoleCompat.h and compile MuyuGpuRoleCompat.c into the same archive.
    command += ['-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'kits/AdrenoPM4'),
                '-I', str(ROOT / 'kits/AdrenoFirmware'),
                '-I', str(board_root() / 'build/gpu'),
                '-I', str(ROOT / 'kits/muyu')]
    command += ['-include', str(ROOT / 'kits/muyu/MuyuGpuRoleCompat.h')]
# The XNU line decides which of a few changed kernel APIs the driver may use (see UfsXnuCompat.h).
version = os.environ.get('UNIXNU_XNU_VERSION', '')
if version:
    command.append('-DXNU_MUYU_XNU_VERSION=' + version.split('.', 1)[0])
(out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(out / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-3500:])
data = destination.read_bytes()
magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
# arm64 is subtype 0, arm64e is subtype 2; the selected OS profile decides which one this build produces.
ARM64E_SUBTYPE = 2
wanted_subtype = ARM64E_SUBTYPE if os.environ.get('UNIXNU_XNU_ARCH') == 'arm64e' else 0
# arm64e objects carry the pointer-auth ABI bits in the subtype's high byte (0xC0000002 on this build,
# i.e. CPU_SUBTYPE_ARM64E | PTRAUTH_ABI | a kernel flag), so only the low byte names the base ABI.
if (magic, cpu, subtype & 0x00ffffff, kind) != (0xfeedfacf, 0x100000c, wanted_subtype, 1):
    raise SystemExit(f'Unexpected platform object architecture {cpu:#x}/{subtype} '
                     f'(wanted subtype {wanted_subtype} for {os.environ.get("UNIXNU_XNU_ARCH", "arm64")})')
offset = 32
deployment = None
for _ in range(count):
    cmd, length = struct.unpack_from('<II', data, offset)
    if cmd == 0x32:
        deployment = struct.unpack_from('<II', data, offset + 8)
    offset += length
# The deployment target comes from the OS profile (-miphoneos-version-min=X.Y in the recipe), so it is read
# back from the recorded command instead of being pinned to iOS 17.
# The OS profile is authoritative; the recorded recipe can be older than the tree it was copied into.
minimum = os.environ.get('UNIXNU_IOS_VERSION') or next(
    (argument.split('=', 1)[1] for argument in command
     if argument.startswith('-miphoneos-version-min=')), '17.0')
major, _, minor = minimum.partition('.')
wanted_deployment = (2, (int(major) << 16) | (int(minor or 0) << 8))
if deployment != wanted_deployment:
    raise SystemExit(f'Unexpected platform deployment {deployment} (wanted {wanted_deployment} '
                     f'from -miphoneos-version-min={minimum})')
archive = out / ('lib' + component + '.a')
archive.unlink(missing_ok=True)
objects = [destination]
if component == 'Adreno740':
    # Compile the iOS 17 GPU-role shim (C) against the same kernel ABI and add it to the archive.
    # The C++ recipe drives a .cpp; we retarget it to C in place (-xc++ -> -xc), drop the C++-only
    # std and -fapple-kext, and point it at kits/muyu/MuyuGpuRoleCompat.c.
    compat_src = ROOT / 'kits/muyu/MuyuGpuRoleCompat.c'
    compat_obj = out / 'MuyuGpuRoleCompat.o'
    ccmd = []
    idx = 0
    while idx < len(arguments):
        v = arguments[idx]
        if v == record['file']:
            ccmd.append(str(compat_src))
        elif v == '-o':
            ccmd.extend(['-o', str(compat_obj)]); idx += 1
        elif v == '-Xclang' and idx + 1 < len(arguments) and arguments[idx + 1].startswith('-W'):
            idx += 1
        elif v == '-xc++':
            ccmd.append('-xc')
        elif v.startswith('-std=') and '++' in v:
            pass
        elif v == '-fapple-kext':
            pass
        elif not v.startswith('-W'):
            ccmd.append(v)
        idx += 1
    ccmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
             '-I', str(ROOT / '.cache/xnu-work/osfmk')]
    cr = subprocess.run(ccmd, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (out / 'MuyuGpuRoleCompat-compile.log').write_text(cr.stdout)
    if cr.returncode:
        raise SystemExit(cr.stdout[-3500:])
    objects.append(compat_obj)
    # NativeAdrenoService.cpp is only the IOService front end. Every implementation it calls
    # (board/VM/ring/GMU/firmware/SMMU/PM4/user-client) lives in the other driver TUs; the 27
    # line archives them separately with build-adreno740-kits.py. On this 17 line there is no
    # kits manifest entry, so fold them into the same archive: CONFIG_XNU_NATIVE_ADRENO then
    # needs just the one -force_load of libAdreno740.a. NativeAdrenoFirmwareDefaults.cpp also
    # supplies the weak empty firmware images the service declares, so a link without the
    # harvested blobs still resolves (and validate() refuses a zero-length image).
    # NativeAdrenoClient.cpp is the userland client, NativeAdrenoAccelerator.cpp is staged
    # Tier-2 (its initializer must not run at boot), NativeAdrenoService.cpp is destination.
    kit_sources = sorted(p for p in (ROOT / 'drivers/Adreno740').glob('NativeAdreno*.cpp')
                         if p.name not in ('NativeAdrenoClient.cpp', 'NativeAdrenoService.cpp',
                                           'NativeAdrenoAccelerator.cpp'))
    for kit_src in kit_sources:
        kit_obj = out / (kit_src.stem + '.kit.o')
        kit_command = []
        index = 0
        while index < len(command):
            value = command[index]
            if value == str(source):
                kit_command.append(str(kit_src))
            elif value == '-o':
                kit_command.extend(['-o', str(kit_obj)])
                index += 1
            elif value in ('-MF', '-MJ'):
                index += 1
            elif value in ('-MD', '-MP'):
                pass
            else:
                kit_command.append(value)
            index += 1
        kit_command += ['-I', str(ROOT / 'drivers/Adreno740')]
        kit_result = subprocess.run(kit_command, cwd=record['directory'], stdout=subprocess.PIPE,
                                    stderr=subprocess.STDOUT, text=True)
        (out / (kit_src.stem + '-compile.log')).write_text(kit_result.stdout)
        if kit_result.returncode:
            raise SystemExit(kit_src.name + ':\n' + kit_result.stdout[-3500:])
        objects.append(kit_obj)
    # The kits call into the PM4 packet encoder, which the 17-line legacy display
    # archive does not compile (it only needs the GMU/HFI/SMMU C kits). Add just
    # AdrenoPM4.c: it is self-contained, its helpers are static.
    #
    # Two kernel hooks supplied by the iPadOS 27 XNU patch series do not exist in
    # 21H461. The driver declares them weak_import, but a static kernel link still
    # needs a definition, and native_gpu_dcvs[1] is read through an array address
    # unconditionally, so leaving it unresolved would fault at runtime. These inert
    # weak defaults match drivers/Adreno740/tests/HostKernelHooks.cpp on the 27 line:
    # DCVS stays off and the pmap reverse lookup reports no user mapping.
    shim_source = out / 'MuyuAdrenoKernelShims.c'
    shim_source.write_text('''/* 21H461 kernel shims for the Adreno740 kits (see build-arm-driver.py). */
#include <stdint.h>
__attribute__((weak)) volatile uint32_t native_gpu_dcvs[15] = {0};
__attribute__((weak)) unsigned int
native_pmap_user_mappings(unsigned int ppnum, void **first_pmap,
                          uint64_t *first_va, char *first_name)
{
    (void)ppnum;
    (void)first_pmap;
    (void)first_va;
    (void)first_name;
    return 0;
}
''')
    c_recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json'
    c_record = json.loads(c_recipe.read_text().strip().rstrip(','))
    c_base = []
    index = 0
    while index < len(c_record['arguments']):
        value = c_record['arguments'][index]
        if value == '-Xclang' and index + 1 < len(c_record['arguments']) \
                and c_record['arguments'][index + 1].startswith('-W'):
            index += 1
        elif value in ('-MF', '-MJ'):
            index += 1
        elif value in ('-MD', '-MP') or value.startswith('-W'):
            pass
        else:
            c_base.append(value)
        index += 1
    c_base += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
               '-Wno-incompatible-sysroot', '-I', str(ROOT / '.cache/xnu-work/osfmk'),
               '-I', str(ROOT / 'kits/AdrenoPM4')]
    for c_src, c_obj in ((ROOT / 'kits/AdrenoPM4/AdrenoPM4.c', out / 'AdrenoPM4.kit.o'),
                         (shim_source, out / 'MuyuAdrenoKernelShims.o')):
        c_command = []
        index = 0
        while index < len(c_base):
            value = c_base[index]
            if value == c_record['file']:
                c_command.append(str(c_src))
            elif value == '-o':
                c_command.extend(['-o', str(c_obj)])
                index += 1
            else:
                c_command.append(value)
            index += 1
        c_result = subprocess.run(c_command, cwd=c_record['directory'], stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True)
        (out / (c_src.stem + '-compile.log')).write_text(c_result.stdout)
        if c_result.returncode:
            raise SystemExit(c_src.name + ':\n' + c_result.stdout[-3500:])
        objects.append(c_obj)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
print(f'{component}: -Werror build and actual ARM64/iOS 17 object audit passed')
