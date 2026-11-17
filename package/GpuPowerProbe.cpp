// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off, staged read-only observation of the Adreno power and clock state. `sheng-gpu-power=<n>`:
//   1  read GCC GPU branches, GPUCC gdscr/branches, GPU SMMU global space 0 and RBBM_STATUS, each only when the previous stage
//      showed its prerequisite on (GpuPowerBoard.h); no register is written
//   2  as 1, plus the one write Linux's gcc-sm8550 probe makes: enable gcc_gpu_cfg_ahb_clk if its enable bit is clear
//   3  as 2, plus, if GPUCC shows the CX power domain off: the gdsc.c enable sequence for gpu_cc_cx_gdsc and the gpu_cc_ahb_clk and
//      gpu_cc_hlos1_vote_gpu_smmu_clk enables, so the SMMU global space can be read; GX and the SMMU are never programmed
//   4  as 3, then, if the SMMU global space read cleanly with CX on and the vote clock running and no SMR valid: map the
//      whole adreno SMMU window and build the GMU's own context through NativeAdreno (NativeAdrenoGmuContextBringup):
//      fixed a6xx_gmu.c payload addresses, native4K TTBR0 tables, context bank 1 / ASID 1, SMR+S2CR slot 2 for the GMU
//      stream 5 (iommus of gmu@3d6a000 at the pinned tree). Bank, ASID and slot are this probe's own choice: Linux gives
//      bank 0 to the GPU and takes the first free SMR. The GMU itself is not booted, so no transaction uses the context.
//      Rows 5 and 7 then show that result instead of the clock words and SMMU IDs mode 3 already recorded (int-005).
//   5  as 4, then, only if the GMU context reached Ready, read-only GMU cold-start prerequisites: the GMU clock tree
//      (gpb_read_gmu_clocks) in row 6; the AOP command DB (reserved-memory aop-cmd-db-region@81c60000, also reserved in
//      the sheng UEFI memory map, so outside the DarwinBoot window) copied with aligned reads, parsed by
//      ShengGmuCommandDbInputs and turned into the HFI vote table by ShengGmuBuildStartup (ACD and IFPC off), plus the
//      GMU reset/retention/RSC/HFI words when gpu_cc_cx_gmu_clk already runs, in row 7. Nothing is written. The version
//      passed to the parser is the word just read, so a pass shows the layout, not a qualified version.
//   6  as 5, then, between the clock snapshot and the GMU state read, the GMU clock bring-up of gpb_gmu_clocks_on (GPLL0
//      GPU votes, gpucc PLL0/PLL1 configuration, PLL1 enable, hub 200 MHz / gmu 220 MHz, the GMU branches). Row 5 shows its
//      result, row 6 the clock tree after it. The GMU itself stays in reset; no GMU or GPU register is written.
//   7  as 6, then, only if the clocks came up (s7 f0), the command DB gave a startup table (p0 s0) and the GMU reads
//      in reset and idle (1/0/0/0): the GMU cold start. a6xx_gmu_resume's interrupt clear/mask, a write-combine CPU view
//      of the five context payloads (the GMU maps them Normal non-cacheable), then ShengGmuColdStart with the embedded
//      gmu_gen70200.bin (hash-checked in full before any write) and the command-DB startup table: RSC/PDC setup,
//      firmware load and readback, CM3 release, FW and HFI ready, then the HFI startup messages with their ACKs. Row 6
//      shows the HFI protocol result, row 7 the load result, row 4 the GMU state before and after.
//   8  as 7, then, only after a complete cold start, the first GPU steps of a6xx_gmu_resume / hw_init: GMU2HOST interrupt
//      clear and mask, the HFI GX_BW_PERF_VOTE (a6xx_hfi_set_freq; perf index 1 = 220 MHz and bandwidth index 1, the
//      lowest real levels of the command-DB tables, where Linux starts at the top one) and the OOB GPU_SET request
//      (set bit 30, ack bit 31) that makes the GMU power GX. Row 5 shows the GX state before/after, and RBBM_STATUS /
//      GBIF_HALT only when GX reports on. Nothing in the GPU register space is written.
//   9  as 8, then, only when GX reports on, the rest of a6xx hw_init up to a7xx_cp_init, in Linux order: the GPU's own
//      native-4K SMMU context (bank 0, SIDs 0/1 on SMR 0/1, as arm-smmu-qcom gives the GPU bank 0) with the SQE, ring,
//      memptrs, power-up list and scratch buffers; the SQE copy (a740_sqe.fw minus its first dword); GBIF halt release,
//      SECVID TSB, hwcg (a740_hwcg), QoS, UCHE, AHB, hang detect, CP protect (a730_protect), APRIV, interrupt mask; SQE
//      base, ring, rptr shadows, SQE start; the power-up list; CP_THREAD_CONTROL + CP_ME_INIT with a timestamp fence;
//      then one CP_MEM_WRITE to the scratch page with a second fence. UBWC and the zap shader / secure-mode exit are
//      not done yet (sheng uses a zap shader, so RBBM_SECVID_TRUST_CNTL is never written). Row 6 shows the GPU
//      context, row 7 the CP result. From mode 8 on, row 4 becomes a heartbeat every two seconds once the run ends.
//      hw_init also programs UBWC (a6xx_set_ubwc_config with the sm8550 qcom_ubwc_cfg_data). After the scratch write:
//      the zap shader (zap_shader_load_mdt / qcom_mdt_load: PAS 13 init_image with the ELF header + hash segment,
//      mem_setup over gpu_micro_code_mem 0x9b09a000, segment copy, auth_and_reset, all as SIP SMC calls) and, only if TZ
//      accepted it, CP_SET_SECURE_MODE 0 with a fence (row 2); then the GPU interrupt line SPI 300 in the GIC
//      distributor before and after RBBM_INT_CLEAR_CMD, and the CP always-on counter rate (row 3).
// The result is drawn in diagnostic extension rows 4-7 (the GPU slots) so one photo carries the whole chain. It runs in a thread
// about ten seconds after the desktop is up so the page already exists if a read faults. Nothing here is validated on the device
// until a boot has shown it.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "GpuPowerBoard.h"
}

#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoUserClient.h"
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoPwrup.h"
#include "NativeAdrenoA740HwInit.h"
/* muyu is Adreno 735 (gen7-11-0, 1.5 MiB GMEM, family ADRENO_7XX_GEN2).
 * The harvested a7xx_catalog.c assigns a740_hwcg to every GEN2 entry and
 * a730_hwcg only to GEN1; the generated "MuyuA735Hwcg" table was the GEN1
 * a730 set (gmu_chipid 0x07110000), not this board's family. */
#include "AdrenoPM4.h"
// Weak: an image that does not link NativeAdreno reports "nolink" instead of failing to link.
extern "C" IOReturn NativeAdrenoGmuContextBringup(volatile uint8_t *smmu, size_t bytes, unsigned bank, unsigned asid,
    unsigned slot, unsigned sid, void (*progress)(uint32_t), NativeAdrenoGmuContextReport *report) __attribute__((weak));
#include "ShengGmuCommandDb.h"
#include "ShengGmu.h"
#include <libkern/crypto/sha2.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
extern "C" ShengGmuStatus ShengGmuColdStart(ShengGmuDevice *, const ShengGmuConfig *, const AdrenoHfiStartupConfig *,
    const ShengGmuStartupLimits *, ShengGmuStartupResult *) __attribute__((weak));
extern "C" ShengGmuStatus ShengGmuBindMmio(ShengGmuMmio *, ShengGmuIo *) __attribute__((weak));
extern "C" IOReturn NativeAdrenoGmuContextPayloads(NativeAdrenoGmuPayload out[5]) __attribute__((weak));
extern "C" IOReturn NativeAdrenoGpuContextBringup(volatile uint8_t *, size_t, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, void (*)(uint32_t), NativeAdrenoGmuContextReport *) __attribute__((weak));
// GpuBufferCount (currently 11) comes from NativeAdrenoGpuContext.h. The framework driver's
// NativeAdrenoGpuContextBuffers() fills every slot, so an older [5] declaration both mismatched
// the prototype (-Warray-parameter under -Werror) and overflowed the caller's array.
extern "C" IOReturn NativeAdrenoGpuContextBuffers(NativeAdrenoGpuBuffer out[GpuBufferCount]) __attribute__((weak));
extern "C" NativeAdrenoVm *NativeAdrenoGpuContextVm() __attribute__((weak));
/* The SMMU's real input size (ID2.UBS, 32/36/40/42/44/48), i.e. the aperture the GPU context bank was programmed with. The published
 * service must carry this number: NativeAdrenoPublish sizes every user connection's address window as 2^input_bits - GpuClientVaStart,
 * so a literal that does not match makes 0 bytes of window and newUserClient refuses every open with kIOReturnNoResources. */
extern "C" unsigned NativeAdrenoGpuContextInputBits() __attribute__((weak));
/* Read by the console dump: [0] input bits published, [1] output bits published. (A bare `extern "C" unsigned x[2];` is only a
 * declaration, so the definition carries the initialiser.) */
extern "C" unsigned muyu_gpu_bits[2] = {0, 0};
/* Read by the console dump: the Metal bridge's last streamed line, which is where "PUB no device: <why>" lands. */
extern "C" char muyu_gpu_reason[64] = {0};
/* fix211 (2026-11-16 20:1x): every archived round prints "Initiating vnode jetsam : 1024 desired" and parks at ~1950
 * numvnodes (3692 occurrences across the whole log archive, never a different number). 1024 is exactly what
 * bsd/dev/unix_startup.c:212 computes when sane_size is smaller than 64 KiB:
 *     desiredvnodes = (int)(sane_size / 65536) + 1024;      and vntblinit() then sets numvnodes_max = desiredvnodes * 2
 * parametrised by kern.vn_dealloc_level, i.e. a 2048-vnode table on a machine with ~4 GB of usable pages. On a healthy
 * boot of this memory size desiredvnodes lands near 66k. A 2k table means every process that maps its own binaries
 * competes for vnodes, which is what the jetsam storms, the "ufs lock: N give-ups" lines and the 160 s frame-producer
 * stall all sit inside. These four globals are read-only here, printed once, so one boot says whether sane_size really
 * collapsed and what the RAM actually is (max_mem_actual), without touching any code path. */
/* vm_param.h is pulled in above without an extern "C" wrapper, so a plain extern "C" redeclaration here is a language-
 * linkage conflict. Bind fresh C++-side names to the C symbols instead; the asm label is the Mach-O name the C side emits. */
extern uint64_t muyu_probe_sane_size __asm__("_sane_size");
extern uint64_t muyu_probe_max_mem __asm__("_max_mem_actual");
extern int      muyu_probe_desiredvnodes __asm__("_desiredvnodes");
extern long     muyu_probe_numvnodes __asm__("_numvnodes");
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t) __attribute__((weak));
extern "C" uint32_t native_adreno_publish_stats(uint32_t *) __attribute__((weak));
// The staged AGXAccelerator (drivers/Adreno740/NativeAdrenoAccelerator.cpp): its static initializer runs only after the
// original IOGPUFamily kext has loaded, because the parent metaclass does not exist before that.
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp") __attribute__((weak));
extern "C" IOService *native_adreno_accelerator_allocate(void) __attribute__((weak));
extern "C" unsigned native_adreno_accelerator_instance_size(void) __attribute__((weak));
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" volatile int native_gpu_addon_stage, native_gpu_addon_status, native_gpu_addon2_stage, native_gpu_addon2_status;
struct NativeAdrenoMetalDiagView {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];
    char reader[3][17];
    char name_reader[17];
    char starter[3][17];
    char report[64];
    char report2[64];
};
extern "C" NativeAdrenoMetalDiagView native_adreno_metal_diag;
struct NativeAdrenoOpenRecordView {char name[20]; uint32_t type, macf, res, attach, foreign;};
extern "C" NativeAdrenoOpenRecordView native_adreno_open_log[8];
extern "C" volatile uint32_t native_adreno_trace_stage;
extern "C" volatile uint32_t native_adreno_open_log_count;
extern "C" int proc_iterate(unsigned flags, int (*callout)(void *, void *), void *arg, int (*filter)(void *, void *), void *filterarg);
extern "C" const char *proc_name_address(void *p);
extern "C" volatile uint64_t armcpu_ipi_interrupts __attribute__((weak));
extern "C" volatile uint64_t armcpu_ipi_sent __attribute__((weak));
extern "C" volatile uint64_t armcpu_ipi_selftest_sent __attribute__((weak));
extern "C" void armcpu_ipi_selftest(uint64_t aff) __attribute__((weak));
extern "C" void armcpu_gic_diag(uint64_t aff, uint32_t *out) __attribute__((weak));
extern "C" volatile uint32_t armcpu_gic_diag_stage __attribute__((weak));
extern "C" void armcpu_sgi_selfprobe(uint64_t *out) __attribute__((weak));
extern "C" volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8], armcpu_gic_boot[8];
extern "C" IOService *native_adreno_published_service() __attribute__((weak));
extern "C" const uint8_t native_adreno_sqe_firmware_image[] __attribute__((weak));
extern "C" const uint64_t native_adreno_sqe_firmware_image_size __attribute__((weak));
/* fix152: embed THIS board's zap shader. The 17 line used to link a weak empty default
 * (size 0), so zapAndSecure() returned Z-1 and the GPU never left secure mode. The board's DT
 * declares a "zap-shader" node on qcom,adreno-gpu-gen7-11-0 and the harvested image is
 * fw/gpu-firmware/gen71100_zap.mbn (12088 B) - NOT the A740 zap that sheng/uke's
 * build-native-adreno-kits.py would embed. MuyuZapFirmware.inc is generated from that blob.
 * Strong definitions win over any weak default left elsewhere in the link. */
extern "C" const uint8_t native_adreno_zap_image[] = {
#include "MuyuZapFirmware.inc"
};
extern "C" const uint64_t native_adreno_zap_image_size = sizeof(native_adreno_zap_image);
extern "C" const uint8_t native_adreno_firmware_image[] __attribute__((weak));
extern "C" const uint64_t native_adreno_firmware_image_size __attribute__((weak));
/* This board's GMU image, emitted by gen-muyu-gmu-source.py from fw/gpu-firmware/gen71100_gmu.bin and compiled into
 * the same archive as this file (MuyuGmuFirmware.o). The native_adreno_* pair above is only defined as a strong symbol
 * by build-native-adreno-kits.py, which belongs to the sheng/uke line and is not part of this kernel's link; there the
 * linker leaves it a weak zero, so gmuColdStart() used to refuse with "S nolink" while the real blob sat unused. */
extern "C" const unsigned char muyu_gmu_firmware[] __attribute__((weak));
extern "C" const uint64_t muyu_gmu_firmware_size __attribute__((weak));
// The board's real a740 SQE microcode, from the same generated object (gen-muyu-gmu-source.py). Weak like the
// GMU pair so a kernel built without that archive still links; a zero size is reported, never guessed at.
extern "C" const unsigned char muyu_sqe_firmware[] __attribute__((weak));
extern "C" const uint64_t muyu_sqe_firmware_size __attribute__((weak));
extern "C" ShengGmuDbStatus ShengGmuCommandDbInputs(const uint8_t *, size_t, uint32_t, ShengGmuVoteInputs *) __attribute__((weak));
extern "C" AdrenoHfiStartupStatus ShengGmuBuildStartup(const ShengGmuVoteInputs *, AdrenoHfiStartupConfig *) __attribute__((weak));
// Both chip ids come from the generated policy headers, never from a literal here: the probe used to pass the A740
// id (0x43050a01) to policy generated for this board's Adreno735 id (0x43030b00), so every ShengGmuBuildStartup and
// every ShengGmuColdStart was refused on its first argument check before touching a register.
/* Rows the console dump prints every pass (defined below, next to setDiag). boot_progress_ext slots 4..7 are
 * overwritten by later stages (metalRows rewrites 5/6 and gpuStart rewrites 5/6/7), and an IOLog line printed once
 * at t+10 s has scrolled away long before anyone photographs the panel, so the vote inputs and the refusal reason
 * get their own retained rows. */

extern "C" {
extern char boot_progress_ext[8][48];
extern char boot_progress_ext_wide[2][112];
struct gpb_result native_gpu_power_result;
struct NativeAdrenoGmuContextReport native_gpu_gmu_context;
volatile uint32_t native_gpu_power_stage;
char muyu_gpu_diag[6][48];
/* See heartbeat(): the CP-stall S/Q/C rows plus the CP's ring configuration and the WPTR/recovery counters,
 * retained for BravearkLastLog (a C file). §38 adds Q5 (CP/GMU halt, aperture, clock status) and
 * Q6 (deeper CP_SQE_STAT words). §43 adds Q7 (three spaced hardware RPTR / SQE-PC samples, refreshed every
 * heartbeat whether or not the ring is stalled) and Q8 (global GFSR, context FSR, VM fault count, Q5's age). */
char muyu_cp_stall_rows[9][64];
extern volatile uint32_t native_adreno_wptr_fence[4];
extern volatile uint32_t native_adreno_unstick[15];
/* §43: read-only count of NativeAdrenoVm::fail() ("the address space was taken out of use"), the status that
 * caused it, and the last global GFSR. Defined in drivers/Adreno740/NativeAdrenoVm.cpp; no behaviour change. */
extern volatile uint32_t native_adreno_global_fault[4];
/* §38: the board's hang restart, counted next to its definition so the console can show whether hangcheck
 * ever actually recovered the GPU. [0] restarts, [1] failing step, [2] its status, [3] RBBM_STATUS after reset. */
uint32_t native_gpu_restart[4];
/* §43 / restart2: snapshots around gpuRestart(), printed as MUYU RT2. Pure observation: no register writes,
 * no control-flow change. [0] zap[0], [1] zapLimit(), [2] entry RPTR, [3] entry WPTR, [4] RPTR after
 * ring->restart(), [5] RPTR after CP_ME_INIT, [6] fenceValue(), [7] final RPTR, [8] final WPTR, [9] global GFSR. */
uint32_t native_gpu_restart2[10];
/* Heartbeat tick at which Q5/Q6 were last refreshed (they are only written inside the stalled branch and never
 * cleared, §41.1); Q8 prints it so a stale Q5 cannot be mistaken for a live sample. */
uint32_t native_gpu_stall_seq;
}

namespace {
unsigned mode() {
    static unsigned cached = ~0u;
    if (cached == ~0u) {unsigned m = 0; PE_parse_boot_argn("sheng-gpu-power", &m, sizeof m); cached = m > 9 ? 9 : m;}
    return cached;
}
void delayUs(void *, unsigned us) {IODelay(us);}
constexpr uint64_t kPhys[5] = {GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE};
constexpr uint64_t kLen[5] = {0x1000, 0x1000, GPB_GPUCC_SIZE, GPB_SMMU_GR0_SIZE, GPB_GPU_WINDOW};
OSSharedPtr<IOMemoryMap> gMaps[5];
uintptr_t gVa[5];

int rd(void *, unsigned w, uint32_t off, uint32_t *out) {
    if (w > 4 || !gVa[w] || off >= kLen[w] || (off & 3)) return -1;
    *out = *reinterpret_cast<volatile uint32_t *>(gVa[w] + off);
    return 0;
}
bool clockWrite(unsigned w, uint32_t off) {
    // Mode 6 only: the registers gpb_gmu_clocks_on names (GCC GPU votes/branches, gpucc PLL0/PLL1, the two RCGs, branches).
    if (w == GPB_WIN_GCC_VOTE) return off == GPB_GCC_PLL_VOTE;
    if (w == GPB_WIN_GCC_GPU) return off == GPB_GCC_DDRSS_GPU_AXI || off == GPB_GCC_GPU_MEMNOC_GFX;
    if (w != GPB_WIN_GPUCC) return false;
    return off < GPB_CC_PLL1 + 0x40 || off == GPB_CC_GMU_RCGR || off == GPB_CC_GMU_RCGR + 4 || off == GPB_CC_HUB_RCGR ||
           off == GPB_CC_HUB_RCGR + 4 || off == GPB_CC_CXO_AON_CLK || off == GPB_CC_DEMET_CLK || off == GPB_CC_CX_GMU_CLK ||
           off == GPB_CC_CXO_CLK || off == GPB_CC_HUB_CX_INT_CLK;
}
int wr(void *, unsigned w, uint32_t off, uint32_t value) {
    // Writes: the GCC cfg AHB enable (mode 2), the GPUCC CX gdsc / clock enables (mode 3), the GMU clocks (mode 6).
    const bool gcc = w == GPB_WIN_GCC_GPU && off == GPB_GCC_GPU_CFG_AHB;
    const bool cc = w == GPB_WIN_GPUCC && (off == GPB_CC_CX_GDSCR || off == GPB_CC_AHB_CLK || off == GPB_CC_SMMU_VOTE_CLK);
    if (w > 4 || !gVa[w] || off >= kLen[w] || (off & 3)) return -1;
    if (!((gcc && mode() >= 2) || (cc && mode() >= 3) || (mode() >= 6 && clockWrite(w, off)))) return -1;
    *reinterpret_cast<volatile uint32_t *>(gVa[w] + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
void setRow(unsigned slot, const char *text) {
    unsigned i = 0;
    for (; i < 47 && text[i]; ++i) boot_progress_ext[slot][i] = text[i];
    for (; i < 47; ++i) boot_progress_ext[slot][i] = ' ';
    boot_progress_ext[slot][47] = 0;
}
/* Same layout as setRow, but into rows only the console dump reads (BravearkLastLog prints them every 5 s), so a
 * later stage that rewrites boot_progress_ext 4..7 cannot erase the evidence a photograph is taken for. */
void setDiag(unsigned slot, const char *text) {
    if (slot >= 6) return;
    unsigned i = 0;
    for (; i < 47 && text[i]; ++i) muyu_gpu_diag[slot][i] = text[i];
    for (; i < 47; ++i) muyu_gpu_diag[slot][i] = ' ';
    muyu_gpu_diag[slot][47] = 0;
}
/* 2026-11-11: the very first boot of this series that reached userspace proved the 5 s printf dump is worthless from
 * then on. That dump's first pass is at muyu-dump-at (t+10 s start + 20 s), and by t+30 s userspace owns the panel:
 * the photo showed the two IOLog lines publish() prints at t+10 s and not one MUYU LIVE/PROBE/X/D row. So the cold
 * start verdict is sent through IOLog the moment it is produced, on the channel that demonstrably lands. */
void logRows(const char *tag) {
    IOLog("MUYU %s X7 %s\n", tag, boot_progress_ext[7]);
    IOLog("MUYU %s X6 %s\n", tag, boot_progress_ext[6]);
    for (unsigned i = 0; i < 6; ++i) IOLog("MUYU %s D%u %s\n", tag, i, muyu_gpu_diag[i]);
}
void publish(const gpb_result &r) {
    char row[64];
    snprintf(row, sizeof row, "GPU m%u gcc=%s cc=%s sm=%s gp=%s io=%d pw%u%u", r.mode, gpb_state_text(r.gcc), gpb_state_text(r.cc),
             gpb_state_text(r.smmu), gpb_state_text(r.gpu), r.io_error, r.powered_cx, r.powered_clocks);
    setRow(4, row);
    snprintf(row, sizeof row, "ahb=%08x>%08x mem=%08x dvm=%08x", r.cfg_ahb, r.cfg_ahb_after, r.memnoc, r.dvm);
    setRow(5, row);
    snprintf(row, sizeof row, "cx=%08x>%08x gx=%08x sv=%08x", r.cx_before, r.cx_gdscr, r.gx_gdscr, r.cc_smmu);
    setRow(6, row);
    snprintf(row, sizeof row, "SM %08x %08x %08x v%u R%08x", r.scr0, r.id0, r.id1, r.smr_valid, r.rbbm_status);
    setRow(7, row);
    // The rows above are overwritten by gmuContext/metalRows, so the raw words also go to the console: the panel overlay
    // only paints while it owns the display, and with no GPU client (o=0) it never does. ahb>ahb2 is the GCC branch
    // before/after the mode-2 enable, en=1 when this run wrote it; cx>cx2 is the CX GDSCR before/after the mode-3 power
    // sequence and hw is the gds_hw_ctrl word the PWR_ON poll read (bit31 = powered). cc_ahb/cc_gmu/pll complete the set.
    IOLog("MUYU PWR gcc=%s cc=%s sm=%s gp=%s io=%d pw%u%u en=%u ahb=%08x>%08x pll=%08x\n",
          gpb_state_text(r.gcc), gpb_state_text(r.cc), gpb_state_text(r.smmu), gpb_state_text(r.gpu), r.io_error,
          r.powered_cx, r.powered_clocks, r.enabled_ahb, r.cfg_ahb, r.cfg_ahb_after, r.pll_vote);
    IOLog("MUYU PWR cx=%08x>%08x hw=%08x gx=%08x cc_ahb=%08x cc_gmu=%08x sv=%08x\n",
          r.cx_before, r.cx_gdscr, r.cx_hwctrl, r.gx_gdscr, r.cc_ahb, r.cc_gmu, r.cc_smmu);
    // Retained copy of the same four states for the 5 s console dump: the IOLog lines above appear once at t+10 s and
    // have scrolled off by the time the panel is photographed.
    char t[48];
    snprintf(t, sizeof t, "P %s %s %s %s io%d pw%u%u", gpb_state_text(r.gcc), gpb_state_text(r.cc),
             gpb_state_text(r.smmu), gpb_state_text(r.gpu), r.io_error, r.powered_cx, r.powered_clocks);
    setDiag(0, t);
    snprintf(t, sizeof t, "X cx%08x>%08x hw%08x gx%08x", r.cx_before, r.cx_gdscr, r.cx_hwctrl, r.gx_gdscr);
    setDiag(4, t);
    snprintf(t, sizeof t, "A ahb%08x>%08x pll%08x mm%08x", r.cfg_ahb, r.cfg_ahb_after, r.pll_vote, r.memnoc);
    setDiag(5, t);
}
constexpr unsigned kGmuBank = 1, kGmuAsid = 1, kGmuSlot = 2, kGmuSid = 5;
constexpr uint64_t kSmmuWindow = 0x40000;   // adreno_smmu reg size at the pinned tree; ID1 says 32 x 4 KiB pages x 2
OSSharedPtr<IOMemoryMap> gSmmuMap;
void gmuProgress(uint32_t step) {
    // Before each step, so a hang or abort leaves the step on screen.
    char row[64];
    snprintf(row, sizeof row, "GMUCTX step %u running", step);
    setRow(5, row);
    native_gpu_power_stage = 0x40 + step;
}
void gmuContext(const gpb_result &r) {
    char row[64];
    const bool ready = r.smmu == GPB_DONE && !r.io_error && gpb_gdsc_on(r.cx_gdscr) && !r.smr_valid;
    if (!ready || !NativeAdrenoGmuContextBringup) {
        snprintf(row, sizeof row, "GMUCTX %s", !NativeAdrenoGmuContextBringup ? "nolink" : "skip prereq");
        setRow(5, row);
        return;
    }
    IODeviceMemory *memory = IODeviceMemory::withRange(GPB_SMMU_BASE, kSmmuWindow);
    if (memory) {
        gSmmuMap = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        memory->release();
    }
    if (!gSmmuMap) {setRow(5, "GMUCTX nomap"); return;}
    NativeAdrenoGmuContextReport &g = native_gpu_gmu_context;
    const IOReturn status = NativeAdrenoGmuContextBringup(reinterpret_cast<volatile uint8_t *>(gSmmuMap->getVirtualAddress()),
        kSmmuWindow, kGmuBank, kGmuAsid, kGmuSlot, kGmuSid, gmuProgress, &g);
    // B s<step> st<IOReturn> k<kit program status>/<kit attach status> i<ID2> p<phase>w<writes>r<readback>e<enabled>
    //   r<route phase>w<writes>v<verified>; spans are always 5 on success and left out for room
    snprintf(row, sizeof row, "B s%u st%x k%u/%u i%x p%uw%ur%ue%u r%uw%uv%u", g.step, static_cast<unsigned>(status),
             g.program_status, g.route_status, g.id2, g.program_phase, g.program_writes, g.program_readback_valid,
             g.program_enabled, g.route_phase, g.route_writes, g.routes_verified);
    setRow(5, row);
    if (g.step == 3 && g.program_phase >= 3) {
        // Program refused after its readback (int-007: kit status 7 with TTBR0 stored): show which context fields
        // differ (mask, NativeAdrenoGmuContextReport) and the read-back values; TTBR0 is in GPU4 of int-007.
        const ArmSmmu4KContextImage &v = g.image;
        snprintf(row, sizeof row, "M%03x a2%x m%x/%x T1%llx", g.mismatch, v.cba2r, v.mair0, v.mair1,
                 static_cast<unsigned long long>(v.ttbr1));
        setRow(6, row);
        snprintf(row, sizeof row, "cb%08x tc%08x t2%08x s%x", v.cbar, v.tcr, v.tcr2, v.sctlr);
        setRow(7, row);
    } else {
        snprintf(row, sizeof row, "T%016llx S%08x C%08x R%08x", static_cast<unsigned long long>(g.ttbr0), g.sctlr,
                 g.s2cr, g.smr);
        setRow(7, row);
    }
    native_gpu_power_stage = status ? 0x4f : 0x47;
}

// Mode 5 (read-only). Physical blocks from the pinned DT: gmu reg 0x3d68000 size 0x37000, rscc 0x3d50000, cx_misc
// 0x3d9e000 (gpu reg-names "cx_mem"); register dword offsets from a6xx_gmu.xml / a6xx.xml. The gmu block starts at dword
// 0x1a000 of the GPU register space (SHENG_GMU_CORE_GPU_DELTA 0x68000 bytes), so CM3_SYSRESET 0x1f800 is byte 0x16000.
// Every one of these used to be the sheng board's pair (0x3d6a000/0x35000 with a 0x6a000 delta), which shifted each read
// by 0x2000 bytes - inside a valid window, so nothing faulted and the rows looked plausible while showing other registers.
constexpr uint64_t kCmdDbBase = 0x81c60000, kCmdDbBytes = 0x20000;
constexpr uint64_t kGmuPhys[3] = {0x03d68000, 0x03d50000, 0x03d9e000};
constexpr uint64_t kGmuLen[3] = {0x37000, 0x10000, 0x1000};
constexpr uint32_t kGmuSysReset = 0x1f800 * 4 - 0x68000, kGmuHfiStatus = 0x1f980 * 4 - 0x68000;
constexpr uint32_t kRsccSeqBusy = 0x101 * 4, kCxMiscTcmRet = 0x39 * 4;
alignas(8) uint8_t gCmdDb[kCmdDbBytes];
ShengGmuVoteInputs gVoteInputs;
AdrenoHfiStartupConfig gStartup;
OSSharedPtr<IOMemoryMap> gCmdDbMap, gGmuMaps[3];

extern "C" {
struct gpb_gmu_clocks native_gpu_gmu_clocks;
struct gpb_gmu_clock_on native_gpu_gmu_clock_on;
uint32_t native_gpu_gmu_state[4];   // SYSRESET, TCM_RET_CNTL, RSCC SEQ_BUSY_DRV0, HFI_CTRL_STATUS
}

volatile uint32_t *mapWords(uint64_t phys, uint64_t len, OSSharedPtr<IOMemoryMap> &keep) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, len);
    if (!memory) return nullptr;
    keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly), OSNoRetain);
    memory->release();
    return keep ? reinterpret_cast<volatile uint32_t *>(keep->getVirtualAddress()) : nullptr;
}
// Mode 7. Windows come from ShengGmuWindowPhysical()/ShengGmuWindowBytes() at run time, not from copies kept here:
// the literals that used to sit at this spot were the sheng board's (core 0x3d6a000/0x35000) while this board's DT
// says 0x3d68000/0x37000, so ShengGmuBindMmio refused every cold start with ShengGmuArgument ("S bind refused").
constexpr uint32_t kAoFatal = (1u << 0) | (1u << 3) | (1u << 5);   // A6XX_GMU_IRQ_MASK: WDOG_BITE, FENCE_ERR, AHB_BUS_ERROR
OSSharedPtr<IOMemoryMap> gColdMaps[5], gPayloadMaps[5];
ShengGmuMmio gColdMmio;
ShengGmuDevice gColdDevice;
ShengGmuConfig gColdConfig;
ShengGmuStartupResult gColdResult;
extern "C" {
ShengGmuStartupResult *native_gpu_gmu_cold = &gColdResult;
}
int coldSha256(void *, const void *data, size_t bytes, uint8_t digest[32]) {
    if (!data || !bytes || !digest) return 1;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, bytes);
    SHA256_Final(digest, &ctx);
    return 0;
}
int coldNow(void *, uint64_t *us) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    *us = ns / 1000;
    return 0;
}
int coldWait(void *, unsigned us) {IODelay(us); return 0;}
void readGmuState(uint32_t out[4]) {
    // The same four words gmuPrerequisites reads, through the cold-start windows.
    volatile uint8_t *core = gColdMmio.windows[0].cpu, *rscc = gColdMmio.windows[1].cpu, *misc = gColdMmio.windows[4].cpu;
    out[0] = *reinterpret_cast<volatile uint32_t *>(core + kGmuSysReset);
    out[1] = *reinterpret_cast<volatile uint32_t *>(misc + kCxMiscTcmRet);
    out[2] = *reinterpret_cast<volatile uint32_t *>(rscc + kRsccSeqBusy);
    out[3] = *reinterpret_cast<volatile uint32_t *>(core + kGmuHfiStatus);
}
// Mode 8. GMU dword offsets from a6xx_gmu.xml; GPU dword offsets from a6xx.xml (A6XX-A7XX variants).
constexpr uint32_t kGmuReg(uint32_t dword) {return dword * 4 - 0x68000;}   // offset inside the 0x3d68000 window
constexpr uint32_t kG2hClr = kGmuReg(0x1f991), kG2hInfo = kGmuReg(0x1f992), kG2hMask = kGmuReg(0x1f993);
constexpr uint32_t kH2gSet = kGmuReg(0x1f994), kGxStatus = kGmuReg(0x1f8d0);   // GMU_SPTPRAC_PWR_CLK_STATUS (A7XX)
// fix193: the HOST2GMU clear register sits right after the set register (GMU2HOST clear/info/mask at 0x1f991-3,
// HOST2GMU set/clear at 0x1f994-5), so withdrawing the OOB_GPU_SET request is a write of bit 30 here.
constexpr uint32_t kH2gClr = kGmuReg(0x1f995);
constexpr uint32_t kRbbmStatus = 0x0210 * 4, kGbifHalt = 0x3c45 * 4;
extern "C" {
uint32_t native_gpu_gx[8];   // vote status, fw error, oob ack polls, gx before, gx after, gx gdscr, RBBM_STATUS, GBIF_HALT
}
// fix187 (2026-11-16): gpuOn() used to be called from inside gmuColdStart(), i.e. BEFORE logRows("COLD"). A hang or a
// bus stall in the GX power-on therefore swallowed the whole COLD block, and power=8/9 looked exactly like "the cold
// start never ran" in all three runs that tried it. Move the call after the COLD rows, and make it switchable so an
// A/B can tell "GX kills the system" from "the cold start is the problem".
int native_gpu_gxstop = 1;        // 1 = run gpuOn() after the COLD rows, 0 = skip it (sheng-gpu-gxstop=0)
int native_gpu_cold_status = -1;  // ShengGmuStatus of the last cold start, for the deferred gpuOn()
// fix189 (2026-11-16): fix188 (power=8 + gxstop=1) booted to the lock screen with a running clock but with the touch
// stream dead (NATIVE-NVT started at 97.8 s and printed no frames= row in 177 s), while fix187 (same kernel,
// gxstop=0) had 18000 frames. gpuOn() is the only difference, so it is split into four numbered sub-steps that can
// be stopped one at a time: sheng-gpu-gxstep=N parks the probe thread after sub-step N. Each sub-step prints its own
// IOLog line before the next one starts, so a stall never hides the step that completed.
int native_gpu_gxstep = 5;        // 1 clear+mask, 2 +HFI set_freq, 3 +H2G ack, 4 +GX status/GDSCR, 5 full
// fix193 (2026-11-16 12:0x): fix191/fix192 with the touch pump's watch counters settled what fix188 could not.
//   gxstop=1 (GX powered):  fresh stopped at 60 frames and never moved again; peek 420/s, valid==peek, err=0,
//                           i.e. the panel answers SPI but has stopped scanning altogether.
//   gxstop=0 (GX off):      frames=3000 contacts=1803 published=1803 errors=0 by 233 s and frames=6000 by 254 s,
//                           i.e. touch works; the 0.7 frames/s in the first 40 s is the chip's idle scan, not a fault.
// So GX power-on stops the touch chip from scanning. This switch asks the next question: is that reversible? With
// sheng-gpu-gxcycle=1 the OOB_GPU_SET request is withdrawn (kH2gClr bit 30) right after the ack and the GX status is
// read back, then gpuOn() returns with GX off and the boot continues. If the desktop and the touch stream come back,
// GX-on is a live state and not a one-time corruption, and the eventual fix is to power GX only while a GPU client
// needs it (or to find what the GMU vote does to the panel/touch rail in the meantime).
int native_gpu_gxcycle = 0;       // 1 = withdraw the GX request after the ack and continue the boot (sheng-gpu-gxcycle=1)
/* fix197 (2026-11-16): ~90 of the ~190 archived boots stop before the first display swap and hard reset inside a
 * 171-180 s window, GX or no GX (fix16/fix22/fix168 all predate the GX code), and the log of those runs is full of
 * apfs_trim_to_prev_fsize and "ufs lock: N give-ups". So the boot stall is a first-boot storage/vnode storm, not a
 * GPU fault - and while it is in progress it hides everything GX does. The whole sequence runs on our own probeThread,
 * so GX can simply be held back until the system is already on its feet: sheng-gpu-gxdelay=N waits N seconds after the
 * GMU cold start (steps 1-2, proven harmless: touch worked parked at step 2) before the GMU_OOB_GPU_SET write. That
 * separates "GX powered during a fragile boot" from "GX powered into a running system", and the user can watch the
 * exact second touch stops. */
int native_gpu_gxdelay = 0;       // seconds to wait before the OOB_GPU_SET write (sheng-gpu-gxdelay=N)
/* fix210 (2026-11-16 20:1x): the GMU cold start is the step that kills the frame producer as soon as the AGX/Metal
 * addon is mounted, and only then: fix174 (power=7, addon) offered 3 frames and then went silent for 536 s, fix207 and
 * fix208 (same config) reproduced it twice from the kernel side - the 5 s MUYU FBW timer showed pending=0 transferred=0
 * gpu=0 bypass=0 held=0 at every sample, i.e. nothing stuck in the framebuffer, nobody upstream submitting. The same
 * cold start without the addon is harmless (fix182/fix185 reach the desktop, 18000 touch frames, 640 swaps), and the
 * addon with the cold start stopped early is harmless too (fix175 coldstop=2, fix176 coldstop=4 both reach the desktop).
 * So the poison is "firmware executing, addon mounted", not either alone.
 * fix197 already found the shape of this class of fault: bringing a rail up into a system that is *already running* is
 * fine (touch init at 90 s, GX at 351 s), while bringing it up during userland bring-up is what breaks it. The cold
 * start currently runs at ~55 s, right in the middle of the boot storm. sheng-gpu-colddelay=N holds it back N seconds
 * so it lands after the UI is up (~160 s), which turns "does the addon survive a live GMU" into a single-variable
 * question with the same image. 0 (absent) keeps today's placement. */
int native_gpu_colddelay = 0;     // seconds to wait before ShengGmuColdStart (sheng-gpu-colddelay=N)
/* fix199 (2026-11-16): fix198 proved the whole power=8 front half is good - GMU cold start, the HFI firmware handshake
 * (X6 P0 g12 k606 e0 t0 n4), 681 s of uptime and 3995 published contacts - as long as kH2gSet bit 30 is never written.
 * fix197 then showed that writing it into an already running system does not stop the touch chip (frames keep coming,
 * published stays 1:1 with contacts, hid climbs) but leaves the UI deaf: 861 contacts were produced while the frame
 * buffer stayed at 22-36 changed channels, i.e. only the lock screen clock was moving. The break is therefore after the
 * driver, on the user-space consumer side (backboardd takes HID events and drives the scene), not in the touch path.
 * These two switches let the probe rebuild that consumer from the kernel once GX is on, so the question "is GX-on a
 * recoverable user-space state or a one-way corruption" gets a direct answer without touching the GPU sequence. */
int native_gpu_respring = 0;      // sheng-gpu-respring=N: N s after GX-on, restart backboardd (0 = off)
int native_gpu_respring_sb = 0;   // sheng-gpu-respring-sb=N: a further N s, then restart SpringBoard (0 = off)
int hfiWait(void *) {IODelay(100); return 0;}
// Mode 9. GPU register access through the cold-start GPU window (dword offsets), and the GMU core window.
uint32_t gpuRd(uint32_t reg) {
    uint32_t v = 0xdeadbeef;
    gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuGpu, size_t(reg) * 4, &v);
    return v;
}
void gpuWr(uint32_t reg, uint32_t value) {
    gColdDevice.io.write32(gColdDevice.io.cookie, ShengGmuGpu, size_t(reg) * 4, value);
}
void gpuWr64(uint32_t reg, uint64_t value) {gpuWr(reg, uint32_t(value)); gpuWr(reg + 1, uint32_t(value >> 32));}
void gmuWr(uint32_t dword, uint32_t value) {gColdDevice.io.write32(gColdDevice.io.cookie, ShengGmuCore, kGmuReg(dword), value);}
bool gxOn() {
    uint32_t v = 3;
    gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuCore, kGxStatus, &v);
    return !(v & 3);
}
/* fix194 (2026-11-16 12:0x): the 172 s reset is our own poller, not GX itself.
 * fix190 parked inside gpuOn (gxstep=3) and heart-beat never started: GX was on and the run lived to 360 s.
 * fix193 returned from gpuOn, so heartbeat() took over and the very same power=8 image hard-reset at 171.85 s;
 * fix166 (power=9) froze at 175.91 s, i.e. the same ~120 s after GX came up. The one thing heartbeat() does that a
 * parked run does not is read the GPU window (ShengGmuGpu) every 2 s - RBBM_STATUS, CP_HW_FAULT, RB_RPTR/WPTR, the
 * GPU FSR and the SMMU GFSR - while a CP that was never initialised sits behind it. So every GPU-window access in
 * heartbeat() is now gated on this flag, which only gpuStart() sets once the CP proved it can run an IB and write the
 * scratch word through its own 4K context. GMU-window reads (kGxStatus/kG2hInfo) stay ungated: they ran unchanged
 * through every stable mode<=7 boot and through the 360 s fix190 run.
 * Consequence for reading the log: at power=8 row 4 now says "gx-on cp-off" instead of a GPU status word dump. */
bool native_gpu_cp_live = false;      // set by gpuStart() when the CP proved it executes (c[0] == 10)
static inline bool gpuPollable() {return native_gpu_cp_live && gxOn();}
int ringWrite(void *, uint32_t reg, uint32_t value) {
    return gColdDevice.io.write32(gColdDevice.io.cookie, ShengGmuGpu, size_t(reg) * 4, value);
}
int ringRead(void *, uint32_t reg, uint32_t *value) {
    return gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuGpu, size_t(reg) * 4, value);
}
int ringBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
/* fix119: the two callbacks NativeAdrenoPreempt wants. gmuWrAt is the GMU's own window by dword offset (the preemption
 * keepalive vote lives at 0x1f8c4, outside the GPU's register window); preemptWrite is a GPU register write that survives the
 * AHB fence: bit 1 of GPU_GMU_AO_GMU_AHB_FENCE_STATUS (GMU dword 0x23b13) reports a dropped context-switch write, so it is
 * retried for at most 2 ms while the GX domain wakes. Both are ports of UniXNU-uke/package/GpuPowerProbe.cpp:362-380. */
int gmuWrAt(void *, uint32_t dword, uint32_t value) {
    return gColdDevice.io.write32(gColdDevice.io.cookie, ShengGmuCore, kGmuReg(dword), value);
}
int preemptWrite(void *, uint32_t reg, uint32_t value) {
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        if (ringWrite(nullptr, reg, value)) return 1;
        ringBarrier(nullptr);
        uint32_t status = 0;
        if (gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuCore, kGmuReg(0x23b13), &status)) return 1;
        if (!(status & 2)) return 0;
        IODelay(10);
    }
    return 1;
}
// The ring, memptrs and lists are Normal non-cacheable on both sides (GPU PTE MAIR 0, CPU write-combine): no cache
// maintenance, only ordering.
int ringPublish(void *, const volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int ringAcquire(void *, volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
uint64_t ringNow(void *) {uint64_t us = 0; coldNow(nullptr, &us); return us;}
int ringIdle(void *) {IODelay(20); return 0;}
OSSharedPtr<IOMemoryMap> gGpuMaps[GpuBufferCount];
volatile uint8_t *gGpuCpu[GpuBufferCount];
NativeAdrenoGpuBuffer gGpuBuffers[GpuBufferCount];
NativeAdrenoRing gRing;
/* fix119: the second ring of the CP's preemption, as uke (sheng-gpu-preempt=2) has it. Nothing is attached to it unless
 * the carrier asks: sheng-tum=...p. The board's command line is full, so the switch rides the render-mode carrier. */
NativeAdrenoRing gRing1;
bool gPreemptWanted;
unsigned gPreemptMode = 2;
NativeAdrenoCpInit gCpInit;
extern "C" {
NativeAdrenoGmuContextReport native_gpu_context;
// stage, copy, cp init status, fence1 result, fence1 seen, mem-write status, fence2 result, scratch word
uint32_t native_gpu_cp[8];
}
void gpuCtxProgress(uint32_t step) {
    char row[64];
    snprintf(row, sizeof row, "Q step %u running", step);
    setRow(6, row);
    native_gpu_power_stage = 0x90 + step;
}
// Context bank 0 ACTLR (MMU-500: bit 0 CMTLB, bit 1 CPRE, bits 9:8 prefetch depth) and sACR. The bootloader leaves ACTLR 0x3 (CPRE on,
// depth 0). Linux clears CPRE in every bank (arm_mmu500_reset, MMU-500 errata 841119/826419: the next-page prefetcher can keep stale
// translations after a TLB invalidation), and the Linux reference session on this tablet read the Adreno bank as 0x301 (CMTLB |
// PREFETCH_DEEP, CPRE off) with Linux's own driver running. The GPU bank is set to that value before any GPU buffer is mapped, then
// its TLB is invalidated (TLBIALL +0x618, TLBSYNC +0x7f0, TLBSTATUS +0x7f4). gActlr keeps the value found, gActlrSet the readback.
uint32_t gActlr = 0xdead, gActlrSet = 0xdead, gSacr = 0xdead;
void gpuSetActlr() {
    const uint32_t id1 = native_gpu_context.id1;
    if (!gSmmuMap || !id1) return;
    const size_t page = (id1 >> 31) ? 0x10000 : 0x1000, bank0 = page << (((id1 >> 28) & 7) + 1);
    if (bank0 + 0x800 > kSmmuWindow) return;
    volatile uint32_t *base = reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress());
    gActlr = base[(bank0 + 0x4) / 4];
    gSacr = base[0x10 / 4];
    base[(bank0 + 0x4) / 4] = 0x301;
    __asm__ volatile("dsb sy" ::: "memory");
    base[(bank0 + 0x618) / 4] = 0;
    base[(bank0 + 0x7f0) / 4] = 0;
    for (unsigned i = 0; i < 1000 && (base[(bank0 + 0x7f4) / 4] & 1); ++i) IODelay(1);
    gActlrSet = base[(bank0 + 0x4) / 4];
}
// Reads one 64-bit word of physical memory through a fresh uncached mapping (page tables and buffer contents as DRAM holds them).
bool physRead64(uint64_t pa, uint64_t *out) {
    IODeviceMemory *m = IODeviceMemory::withRange(pa & ~uint64_t(0xfff), 0x1000);
    if (!m) return false;
    IOMemoryMap *map = m->map(kIOMapAnywhere | kIOMapInhibitCache);
    m->release();
    if (!map) return false;
    *out = *reinterpret_cast<volatile uint64_t *>(map->getVirtualAddress() + (pa & 0xff8));
    map->release();
    return true;
}
// NativeAdrenoVm::sampleWord's DRAM reader (int-070): one aligned word through a fresh uncached mapping.
int dramRead32(uint64_t pa, uint32_t *value) {
    uint64_t w = 0;
    if (!physRead64(pa & ~uint64_t(7), &w)) return 1;
    *value = uint32_t(w >> ((pa & 4) * 8));
    return 0;
}
// fix103: the write side of dramRead32, for the canary the submit path plants in the swap surface. A fresh uncached mapping, so
// the word is in DRAM (not in a line of the CPU's that a later flush would put back over the GPU's writes) before the next pass.
int dramWrite32(uint64_t pa, uint32_t value) {
    IODeviceMemory *m = IODeviceMemory::withRange(pa & ~uint64_t(0xfff), 0x1000);
    if (!m) return 1;
    IOMemoryMap *map = m->map(kIOMapAnywhere | kIOMapInhibitCache);
    m->release();
    if (!map) return 1;
    *reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress() + (pa & 0xffc)) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    map->release();
    return 0;
}
/* fix123.2: the one hook the frame preview needs. A 3200x2136 panel surface is 27 MB and ONE contiguous span, but the
 * imported IOSurface has no kernel mapping and 0xE3940000 is outside the physical aperture phystokv covers (fix123
 * printed "nomap", fix123.1 "noread st=e00002c7"), so every word would otherwise be its own IODeviceMemory. One
 * kIOMapInhibitCache mapping of the whole span is kept for the life of the boot: reads see DRAM as the GPU left it,
 * stores go straight to the panel buffer, which is exactly what a marker needs. Only the first range asked for is
 * mapped - if a later request names another one, it is refused rather than leaking a second 27 MB mapping. */
static IOMemoryMap *gFrameWindow;
static uint64_t gFrameWindowPhysical, gFrameWindowBytes;
int frameWindow(uint64_t physical, size_t bytes, uintptr_t *base) {
    if (!base || !bytes) return 1;
    if (gFrameWindow) {
        if (gFrameWindowPhysical != physical || gFrameWindowBytes < bytes) return 1;
        *base = gFrameWindow->getVirtualAddress();
        return 0;
    }
    IODeviceMemory *m = IODeviceMemory::withRange(physical & ~uint64_t(0xfff), (bytes + (physical & 0xfff) + 0xfff) & ~uint64_t(0xfff));
    if (!m) return 1;
    IOMemoryMap *map = m->map(kIOMapAnywhere | kIOMapInhibitCache);
    m->release();
    if (!map) return 1;
    gFrameWindow = map; gFrameWindowPhysical = physical; gFrameWindowBytes = bytes;
    IOLog("MUYU FBWIN p=%08x%08x bytes=%llx va=%p\n", (uint32_t)(physical >> 32), (uint32_t)physical,
          (unsigned long long)bytes, (void *)map->getVirtualAddress());
    *base = map->getVirtualAddress();
    return 0;
}
/* fix50: the two board hooks the submit path has always expected (int-070/099/100) but this board never installed. Only
 * native_adreno_dram_read32 was registered, so sampleWord() left *cached at ~0u (fix49 read "cffffffff"), auditRange() returned
 * Unsupported, dirty stayed 0 and distrustWriteCombined() never fired - which left publishForDevice() skipping the client BO's
 * CPU cache clean on every submission. The cacheable alias here is the default IODeviceMemory mapping (physRead64 explicitly
 * asks for kIOMapInhibitCache to get DRAM); data caches are physically tagged, so it sees lines another task's view dirtied. */
extern "C" int (*native_adreno_cached_read32)(uint64_t physical, uint32_t *value);
extern "C" int (*native_adreno_line_audit)(uint64_t physical, size_t bytes, uint32_t *dirty, uint32_t *stale);
int cachedRead32(uint64_t pa, uint32_t *value) {
    IODeviceMemory *m = IODeviceMemory::withRange(pa & ~uint64_t(0xfff), 0x1000);
    if (!m) return 1;
    IOMemoryMap *map = m->map(kIOMapAnywhere);
    m->release();
    if (!map) return 1;
    const uintptr_t va = uintptr_t(map->getVirtualAddress()) + (pa & 0xfff);
    *value = *reinterpret_cast<volatile uint32_t *>(va);
    /* Drop the line this read brought in: a stale clean copy would later differ from DRAM and look like a dirty one. */
    __asm__ volatile("dsb sy\n\tdc civac, %0\n\tdsb sy" :: "r"(va & ~uintptr_t(63)) : "memory");
    map->release();
    return 0;
}
int lineAudit(uint64_t pa, size_t bytes, uint32_t *dirty, uint32_t *stale) {
    if (!bytes || ((pa | bytes) & 63) || (pa & 0xfff) + bytes > 0x1000) return 1;
    IODeviceMemory *cm = IODeviceMemory::withRange(pa & ~uint64_t(0xfff), 0x1000);
    IODeviceMemory *dm = IODeviceMemory::withRange(pa & ~uint64_t(0xfff), 0x1000);
    if (!cm || !dm) {if (cm) cm->release(); if (dm) dm->release(); return 1;}
    IOMemoryMap *c = cm->map(kIOMapAnywhere);
    IOMemoryMap *d = dm->map(kIOMapAnywhere | kIOMapInhibitCache);
    cm->release(); dm->release();
    if (!c || !d) {if (c) c->release(); if (d) d->release(); return 1;}
    const uintptr_t cv = uintptr_t(c->getVirtualAddress()) + (pa & 0xfff);
    const uintptr_t dv = uintptr_t(d->getVirtualAddress()) + (pa & 0xfff);
    for (size_t at = 0; at < bytes; at += 64) {
        const volatile uint64_t *cc = reinterpret_cast<const volatile uint64_t *>(cv + at);
        const volatile uint64_t *dd = reinterpret_cast<const volatile uint64_t *>(dv + at);
        uint64_t copy[8];bool differs = false;
        for (unsigned i = 0; i < 8; ++i) {copy[i] = cc[i]; if (copy[i] != dd[i]) differs = true;}
        if (!differs) continue;
        __asm__ volatile("dsb sy\n\tdc civac, %0\n\tdsb sy" :: "r"((cv + at) & ~uintptr_t(63)) : "memory");
        bool written = true;
        for (unsigned i = 0; i < 8; ++i) if (dd[i] != copy[i]) written = false;
        if (written) ++*dirty; else ++*stale;
    }
    c->release(); d->release();
    return 0;
}
// Software walk of the GPU context's TTBR0 table (bank 0 TTBR0 +0x20, TCR +0x30: T0SZ bits 5:0, 4 KiB granule) for one GPU address.
// The SMMU here has no address translation operations (IDR0.ATOSNS), so this shows what the table says, not what the TLB holds.
// Returns the leaf descriptor (0 when absent) and the physical address it maps the address to.
uint64_t gpuWalk(uint64_t va, uint64_t *pa) {
    *pa = 0;
    const uint32_t id1 = native_gpu_context.id1;
    if (!gSmmuMap || !id1) return 0;
    const size_t page = (id1 >> 31) ? 0x10000 : 0x1000, bank0 = page << (((id1 >> 28) & 7) + 1);
    const volatile uint32_t *base = reinterpret_cast<const volatile uint32_t *>(gSmmuMap->getVirtualAddress());
    const uint64_t ttbr = (uint64_t(base[(bank0 + 0x24) / 4]) << 32) | base[(bank0 + 0x20) / 4];
    const unsigned bits = 64 - (base[(bank0 + 0x30) / 4] & 0x3f);
    if (bits < 25 || bits > 48) return 0;
    int level = 4 - int((bits - 12 + 8) / 9);
    uint64_t table = ttbr & 0x0000fffffffff000ull, desc = 0;
    for (; level <= 3; ++level) {
        const unsigned shift = 12 + 9 * unsigned(3 - level);
        if (!physRead64(table + ((va >> shift) & 0x1ff) * 8, &desc) || !(desc & 1)) return 0;
        if (level == 3 || !(desc & 2)) {*pa = (desc & 0x0000fffffffff000ull & ~((uint64_t(1) << shift) - 1)) | (va & ((uint64_t(1) << shift) - 1)); return desc;}
        table = desc & 0x0000fffffffff000ull;
    }
    return 0;
}
uint32_t gpuFsr() {
    // Context bank 0 FSR (ARM SMMUv2: banks follow 2^(NUMPAGENDXB+1) global pages; FSR at +0x58).
    const uint32_t id1 = native_gpu_context.id1;
    if (!gSmmuMap || !id1) return 0xdead;
    const size_t page = (id1 >> 31) ? 0x10000 : 0x1000, bank0 = page << (((id1 >> 28) & 7) + 1);
    if (bank0 + 0x60 > kSmmuWindow) return 0xdead;
    return *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + bank0 + 0x58);
}
void cpRow(const char *what) {
    // C<stage><what> R<RBBM_STATUS> I<INT_0_STATUS> F<CP_HW_FAULT> r<RPTR>/<WPTR> m<shadow rptr>/<fence> s<CB0 FSR>
    char row[64];
    const volatile uint32_t *mem = reinterpret_cast<const volatile uint32_t *>(gGpuCpu[2]);
    snprintf(row, sizeof row, "C%u%s R%x I%x F%x r%x/%x m%x/%x s%x", native_gpu_cp[0], what, gpuRd(A740_RBBM_STATUS),
             gpuRd(A740_RBBM_INT_0_STATUS), gpuRd(A740_CP_HW_FAULT), gpuRd(A740_CP_RB_RPTR), gpuRd(A740_CP_RB_WPTR),
             mem ? mem[0] : 0, mem ? mem[1] : 0, gpuFsr());
    setRow(7, row);
    // 2026-11-14 01:5x, fix38: the CP bring-up either reaches stage 10 (the GPU wrote its scratch word through its own context and
    // signalled the fence) or stops at one of the earlier stages; only stage 10 starts the heartbeat, the interrupt test and the
    // self-test suite. Everything below that gate was invisible in the persistent window, which is why fix37's probe never ran.
    IOLog("MUYU CPROW %s\n", row);
}
// fix39: the driver calls this from a fence wait that is about to report a timeout, so the register row is captured at the failure and
// not only at bring-up. Weak from the driver's side; the board probe owns the registers.
// fix40: fix39 showed the shape of the stall (RPTR 0x36 behind WPTR 0x3d, CB0 FSR 0x80000402, 5 submits / 4 retires), so the context
// bank's fault address is the one number that turns "some IOVA is unmapped" into "this IOVA is unmapped".
// fix41: FSR comes back with MULTI (bit 31) set, which makes FAR unpredictable (it read back zero on fix40), so the records that do name
// the fault are the ring itself - the packet the CP stopped on - and the submit path's own IB log (the IB it was handed, with its first 28
// dwords). Both are dumped once per boot: the fence that hangs is the first one and it stays hung, so a second dump would repeat itself.
extern "C" {extern volatile uint32_t native_adreno_ib_log[4][4+256];extern volatile uint32_t native_adreno_ib_log_count;
extern volatile uint32_t native_adreno_table_switches[8];
/* fix42: the last four BoMaps as [iova lo, iova hi, handle, pid], so a faulting address can be matched against what was mapped. */
extern volatile uint32_t native_adreno_map_log[4][4];extern volatile uint32_t native_adreno_map_log_count;
/* fix48: the submit path's own coherence sample of the first IB word: [1] the kernel's cacheable alias, [2] DRAM before
 * publishMapped(), [3] DRAM after it. fix47 saw DRAM read zero at the translated page while the CP held the real header. */
extern volatile uint32_t native_adreno_coherence[8];
/* fix50: unconditional physical-page clean at Submit: [0] passes, [1] KiB, [2] last status, [3] spans of the last BO. */
extern volatile uint32_t native_adreno_fix50_clean[4];
/* fix50: [0] audits run, [2] dirty lines the board hook wrote back - proves the client's IB really was CPU-cached. */
extern volatile uint32_t native_adreno_wc_audit[6];
/* fix43: the last four submissions the ring wrote, [0] seqno, [1] next, [2] end, [3] total, [4..] the words. */
extern volatile uint32_t native_adreno_ring_log[4][4+112];extern volatile uint32_t native_adreno_ring_log_count;}
extern "C" void native_adreno_cp_state(void) {
    cpRow("evt");
    {
        static unsigned dumped;
        if (!dumped) {
            ++dumped;
            const uint32_t rptr = gpuRd(A740_CP_RB_RPTR), wptr = gpuRd(A740_CP_RB_WPTR);
            const uint32_t words = uint32_t(gGpuBuffers[1].bytes >> 2);
            const volatile uint32_t *rb = reinterpret_cast<const volatile uint32_t *>(gGpuCpu[1]);
            if (rb && words) {
                for (unsigned line = 0; line < 2; ++line) {
                    char dump[128]; size_t n = 0;
                    for (unsigned k = 0; k < 8; ++k)
                        n += size_t(snprintf(dump + n, sizeof dump - n, " %08x", rb[(rptr + line * 8 + k) % words]));
                    IOLog("MUYU RB %x>%x +%x:%s\n", rptr, wptr, line * 8, dump);
                }
            }
            const uint32_t count = native_adreno_ib_log_count;
            const volatile uint32_t *ib = native_adreno_ib_log[count ? (count - 1) & 3 : 0];
            IOLog("MUYU IB n=%u iova=%08x%08x bytes=%x ttbr=%u sw=%u head=%08x,%08x,%08x,%08x,%08x,%08x\n", count,
                  ib[2], ib[1], ib[3], native_adreno_table_switches[3], native_adreno_table_switches[1],
                  ib[4], ib[5], ib[6], ib[7], ib[8], ib[9]);
            /* fix44: the whole IB (fix43 left the ring exonerated: it holds exactly the words the driver meant to write, and four
             * identical fence packets completed before this one). Sixteen dwords a line, 256 in all, so the packet that waits can be
             * named from the log instead of inferred from a stall. */
            for (unsigned line = 0; line < 16; ++line) {
                char dump[192]; size_t k = 0;
                for (unsigned i = 0; i < 16; ++i)
                    k += size_t(snprintf(dump + k, sizeof dump - k, " %08x", ib[4 + line * 16 + i]));
                IOLog("MUYU IB %03x:%s\n", line * 16, dump);
            }
            /* fix43: the ring dump above is the memory the CP reads; the ring log is the words the driver meant to write
             * ([0] seqno, [1] next, [2] end, [3] total, [4..] the words from next). Printing both makes a mismatch - a write
             * that never reached DRAM, or a ring whose IOVA is not the one the CP fetches - visible as a difference instead of
             * an inference. The stalled submission is the last one; the one before it is the shape that completed. */
            const uint32_t rings = native_adreno_ring_log_count;
            for (unsigned back = 1; back <= 2 && back <= rings; ++back) {
                const volatile uint32_t *r = native_adreno_ring_log[(rings - back) & 3];
                IOLog("MUYU RL -%u seq=%u next=%x end=%x total=%x\n", back, r[0], r[1], r[2], r[3]);
                for (unsigned line = 0; line < 2; ++line) {
                    char dump[128]; size_t k = 0;
                    for (unsigned i = 0; i < 8; ++i)
                        k += size_t(snprintf(dump + k, sizeof dump - k, " %08x", r[4 + line * 8 + i]));
                    IOLog("MUYU RL -%u +%u:%s\n", back, line * 8, dump);
                }
            }
        }
    }
    const uint32_t id1 = native_gpu_context.id1;
    if (!gSmmuMap || !id1) return;
    const size_t page = (id1 >> 31) ? 0x10000 : 0x1000, bank0 = page << (((id1 >> 28) & 7) + 1);
    if (bank0 + 0x70 > kSmmuWindow) return;
    const volatile uint32_t *base = reinterpret_cast<const volatile uint32_t *>(gSmmuMap->getVirtualAddress());
    /* fix42: fix41 ran with table switching on (the CPU wrote TTBR0 once) and the same translation fault came back, so the question is
     * no longer whether the switch was attempted but which table the bank holds and whether the client's IB address is in it. CBn_SCTLR,
     * CBn_TCR and the 64-bit CBn_TTBR0/1 say the table; the map log says the last four BoMaps, so the IB's IOVA can be matched against
     * them here instead of in a later round. FSR is read last because the read clears it. */
    {
        static unsigned banks;
        if (banks < 2) {
            ++banks;
            IOLog("MUYU CB sctlr=%x tcr=%x ttbr0=%08x%08x ttbr1=%08x%08x far=%x fsynr=%x fsr=%x\n",
                  base[(bank0 + 0x00) / 4], base[(bank0 + 0x10) / 4], base[(bank0 + 0x24) / 4], base[(bank0 + 0x20) / 4],
                  base[(bank0 + 0x2C) / 4], base[(bank0 + 0x28) / 4], base[(bank0 + 0x5C) / 4], base[(bank0 + 0x68) / 4],
                  base[(bank0 + 0x58) / 4]);
            /* fix142: the line above labels +0x10 as "tcr", but that is CBn_TCR2. The real
             * CBn_TCR (T0SZ/T1SZ, EPD0/EPD1, TG0/1) lives at +0x30 and has never been read.
             * idr2=00005511 gives UBS code 5 -> ias 48, so the client's 4.6 GiB IOVAs should be
             * inside TTBR0, but the live word is the fact. Plus the full fault state: FAR high
             * half, FSYNR1, and the global GFSR/GFSYNR/GFAR. All reads, nothing written. */
            {
                const uint32_t cbTcr   = base[(bank0 + 0x30) / 4];
                const uint32_t cbTcr2  = base[(bank0 + 0x10) / 4];
                const uint32_t cbActlr = base[(bank0 + 0x04) / 4];
                const uint32_t cbCidr  = base[(bank0 + 0x34) / 4];
                const uint32_t cbFsr   = base[(bank0 + 0x58) / 4];
                const uint32_t cbFarL  = base[(bank0 + 0x5c) / 4];
                const uint32_t cbFarH  = base[(bank0 + 0x60) / 4];
                const uint32_t cbSyn0  = base[(bank0 + 0x68) / 4];
                const uint32_t cbSyn1  = base[(bank0 + 0x6c) / 4];
                const uint32_t idr2    = base[0x28 / 4];
                static const unsigned widths[6] = {32, 36, 40, 42, 44, 48};
                const unsigned vcode = (idr2 >> 8) & 15u, ocode = (idr2 >> 4) & 15u;
                IOLog("MUYU CBX tcr=%08x t0sz=%u t1sz=%u epd0=%u epd1=%u tcr2=%08x actlr=%x cid=%x\n",
                      cbTcr, cbTcr & 63u, (cbTcr >> 16) & 63u, (cbTcr >> 7) & 1u, (cbTcr >> 23) & 1u,
                      cbTcr2, cbActlr, cbCidr);
                IOLog("MUYU CBF fsr=%08x far=%08x%08x fsynr=%08x/%08x gfsr=%08x gfsynr=%08x/%08x gfar=%08x%08x idr2=%08x ias=%u oas=%u\n",
                      cbFsr, cbFarH, cbFarL, cbSyn0, cbSyn1,
                      base[0x48 / 4], base[0x50 / 4], base[0x54 / 4], base[0x44 / 4], base[0x40 / 4],
                      idr2, vcode < 5 ? widths[vcode] : 48u, ocode < 6 ? widths[ocode] : 0u);
            }
            const uint32_t maps = native_adreno_map_log_count;
            for (unsigned k = 0; k < 4 && k < maps; ++k) {
                const volatile uint32_t *m = native_adreno_map_log[(maps - 1 - k) & 3];
                IOLog("MUYU MAP -%u iova=%08x%08x h=%x pid=%u\n", k, m[1], m[0], m[2], m[3]);
            }
        }
    }
    /* fix107: every stream this SMMU knows, printed once. The GPU's own masters share one stream (slot 0, sid 1) and the CP's
     * writes land through it, so the question is not whether a table exists but whether the 3D side's traffic is matched at all.
     * On this firmware a BYPASS S2CR reads back as FAULT (SmmuBypass.h), so an unmatched stream faults instead of passing through
     * untranslated - and GFSR has stayed 0 for a hundred frames. That covers FAULT and TRANSLATE, but not "TRANSLATE into some
     * other bank": an S2CR naming a CBAR/SMR the bank does not own routes the transaction somewhere harmless without a fault
     * either. Print SMR/S2CR pairs and the global control words so the route is a fact rather than an inference. */
    {
        static unsigned streams;
        if (!streams) {
            ++streams;
            char row[384]; size_t at = 0;
            at += size_t(snprintf(row + at, sizeof row - at,
                                  "MUYU S2CR scr0=%x scr1=%x scr2=%x idr0=%08x idr1=%08x idr2=%08x idr7=%08x",
                                  base[0], base[0x04 / 4], base[0x0c / 4],
                                  base[0x20 / 4], base[0x24 / 4], base[0x28 / 4], base[0x3c / 4]));
            for (unsigned i = 0; i < 8; ++i)
                at += size_t(snprintf(row + at, sizeof row - at, " | %u smr=%08x s2cr=%08x",
                                      i, base[(0x800 + 4 * i) / 4], base[(0xc00 + 4 * i) / 4]));
            IOLog("%s\n", row);
            /* fix138: the GX-side store still never lands (§69.97). The 8-slot census only
             * covered the streams Linux happens to use; a hidden SMR routing the RB/UCHE
             * master into a different context bank would explain "no fault, no write" without
             * contradicting the CP's own writes. SMMUv2 gives SMR 0x800..0x9ff and S2CR
             * 0xc00..0xdff, so 24 slots stay well inside both ranges and inside the mapped
             * 0x40000 window. Pure observation, once, and no register is written. */
            for (unsigned group = 1; group < 3; ++group) {
                at = 0;
                at += size_t(snprintf(row + at, sizeof row - at, "MUYU S2CR+%u", group));
                for (unsigned i = group * 8; i < (group + 1) * 8; ++i)
                    at += size_t(snprintf(row + at, sizeof row - at, " | %u smr=%08x s2cr=%08x",
                                          i, base[(0x800 + 4 * i) / 4], base[(0xc00 + 4 * i) / 4]));
                IOLog("%s\n", row);
            }
        }
    }
    /* fix109: is the GX domain still up at the moment a frame fails? Everything that has been proven working - the CP fetching and
     * executing its ring, the CP's own CP_EVENT_WRITE7 landing in an imported surface, the RB_DONE_TS stamps that make z1 read
     * b1 e1 - lives on the CX side of the GPU. Everything that has never written a byte lives on the GX side (RB, ROP, UCHE). A
     * powered-down GX domain explains that split exactly: the AHB interface stays alive (so every register read in fix102..fix107
     * returns a coherent, live value) while the datapath behind it is unclocked and produces nothing.
     *
     * The signature that started this: UCHE_CACHE_WAYS (0x0e17) read 0x00800000 at 133.6 s and 0x00000000 from 135.8 s on, while
     * TRAP_BASE / WRITE_THRU_BASE / GMEM_RANGE / MODE_CNTL all kept the values hw_init wrote. A register that loses its value on
     * its own is a domain that went through a power transition; the power-up register list (NativeAdrenoPwrupRegs.h) contains
     * 0x0e17, so a correct GX power-up would have restored it.
     *
     * a7xx_gmu_gx_is_on(): GX is up when A7XX_GMU_SPTPRAC_PWR_CLK_STATUS has neither GX_HM_GDSC_POWER_OFF (bit 0) nor
     * GX_HM_CLK_OFF (bit 1). Also printed: the CPU-side SPTPRAC control pair (0x1a880 clock, 0x1a881 power), GPU_CC_GX_GDSCR,
     * RBBM_STATUS (per-block busy bits), RBBM_STATUS3 bit 24 (SMMU stalled on fault), and 0x0e17 read twice in a row - the second
     * read separates "the value is really 0" from "reading this register clears it". */
    {
        /* 0xdeadbeef in, so a failed window read is not mistaken for "the status says everything is fine": fix109 read st=0 for GX-on
         * while sptpclk/sptppwr also read 0, which is what a closed GMU window looks like. gpuRd() already has that sentinel. */
        uint32_t gxst = 0xdeadbeefu, gxclk = 0xdeadbeefu, gxpwr = 0xdeadbeefu, rbbm = 0, rbbm3 = 0, ways1 = 0, ways2 = 0;
        (void)gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuCore, kGmuReg(0x1f8d0), &gxst);
        (void)gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuCore, kGmuReg(0x1a880), &gxclk);
        (void)gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuCore, kGmuReg(0x1a881), &gxpwr);
        rbbm = gpuRd(0x0210);
        rbbm3 = gpuRd(0x0213);
        ways1 = gpuRd(0x0e17);
        ways2 = gpuRd(0x0e17);
        IOLog("MUYU GX st=%08x on=%u gdscoff=%u clkoff=%u sptpclk=%08x sptppwr=%08x rbbm=%08x rb=%u ccu=%u uche=%u sp=%u gpu=%u s3=%08x stall=%u ways=%08x/%08x\n",
              gxst,(gxst & 3u)?0u:1u,gxst & 1u,(gxst >> 1) & 1u,gxclk,gxpwr,rbbm,
              uint32_t((rbbm >> 7) & 1u),uint32_t((rbbm >> 8) & 1u),uint32_t((rbbm >> 17) & 1u),uint32_t((rbbm >> 18) & 1u),uint32_t((rbbm >> 23) & 1u),
              rbbm3,uint32_t((rbbm3 >> 24) & 1u),ways1,ways2);
    }
    /* fix144: GBIF is the bus gate between the GX domain and DDR. gpuHwInit() clears A740_GBIF_HALT
     * (0x3c45) and A740_RBBM_GBIF_HALT (0x0016) once at bring-up and then never reads them again;
     * every later run has only read RBBM_STATUS3. A halt bit that came back on would produce exactly
     * the fix140 shape - engine reports Completed, no SMMU fault, no stall, not one byte in DRAM -
     * because GX stores are queued at the bus entrance while the CP's CX-side writes still land.
     * Pure observation: read both halt/ack pairs plus the UCHE->GBIF config. Offsets from a6xx.xml
     * (GBIF_HALT 0x3c45 / ACK 0x3c46, RBBM_GBIF_HALT 0x0016 / ACK 0x0017, UCHE_GBIF_GX_CONFIG
     * 0x0e3a, UCHE_CMDQ_CONFIG 0x0e3c, UCHE_FILTER_CNTL 0x0e18, UCHE_MODE_CNTL 0x0e01). No writes. */
    {
        const uint32_t gHalt = gpuRd(A740_GBIF_HALT), gAck = gpuRd(0x03c46);
        const uint32_t rHalt = gpuRd(A740_RBBM_GBIF_HALT), rAck = gpuRd(0x00017);
        const uint32_t ucheGx = gpuRd(A740_UCHE_GBIF_GX_CONFIG), ucheCmdq = gpuRd(A740_UCHE_CMDQ_CONFIG);
        const uint32_t ucheFilt = gpuRd(0x00e18), ucheMode = gpuRd(0x00e01), uchePf = gpuRd(A740_UCHE_CLIENT_PF);
        IOLog("MUYU GBF gh=%08x/%08x rh=%08x/%08x gx=%08x cmdq=%08x filt=%08x mode=%08x pf=%08x\n",
              gHalt,gAck,rHalt,rAck,ucheGx,ucheCmdq,ucheFilt,ucheMode,uchePf);
    }
    /* fix219: this port has no DVFS - native_gpu_dcvs is only ever weakly imported (NativeAdrenoSubmit.cpp:122), no HFI message is
     * ever sent, and it is Linux's a6xx_gmu_set_freq() that raises the GX rate after resume. So the question is what the GPUCC PLLs
     * actually run at: PLL1 is the GMU's own (L 0x16, "rounds to 220 MHz"), and GX takes PLL0. A PLL0 still at the GMU value would
     * make every full-screen pass cost tens of milliseconds - which is exactly the present rate measured in fix218 (165 passes a
     * frame, 36 ms each, 750 MB/s of effective bandwidth). Read-only: MODE/L_VAL/USER_CTL of both PLLs, no writes. */
    if (gVa[GPB_WIN_GPUCC]) {
        auto ccRd=[](uint32_t off)->uint32_t {
            return *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPUCC] + off);
        };
        const uint32_t m0=ccRd(GPB_CC_PLL0+GPB_PLL_MODE), l0=ccRd(GPB_CC_PLL0+GPB_PLL_L_VAL);
        const uint32_t u0=ccRd(GPB_CC_PLL0+GPB_PLL_USER_CTL);
        const uint32_t m1=ccRd(GPB_CC_PLL1+GPB_PLL_MODE), l1=ccRd(GPB_CC_PLL1+GPB_PLL_L_VAL);
        const uint32_t u1=ccRd(GPB_CC_PLL1+GPB_PLL_USER_CTL);
        /* Lucid OLE: VCO = fxo(19.2 MHz) * L, fout = VCO / 2, which is the table's own arithmetic (L 0x16 -> 211, "rounds to 220"). */
        const unsigned long long f0=19200000ull*(l0&0xffu)/2ull/1000000ull;
        const unsigned long long f1=19200000ull*(l1&0xffu)/2ull/1000000ull;
        IOLog("MUYU CLKP pll0 m=%08x l=%08x u=%08x ~%lluMHz | pll1 m=%08x l=%08x u=%08x ~%lluMHz\n",
              m0,l0,u0,f0,m1,l1,u1,f1);
    }
    /* fix118: the BV thread's own view of the world. Every measurement in this port since fix60 has been about the BR thread -
     * CP_RB_RPTR, CP_IB1_BASE, CP_HW_FAULT and CP_SQE_STAT are all BR's registers - while the stall that kills the frame is
     * CP_BV_BR_COUNT_OPS, which is BV's handshake. On a7xx the BV thread shares the ring but keeps its own read pointer
     * (0x0a66), its own IB window (0x0a6d/0x0a70, written by hardware) and, crucially, its own fault and protect-status
     * registers (0x0a64, 0x0a61) that nothing here has ever read. fix117 handed the CP the client's BV/BR stream untouched
     * (strip=0, the configuration sheng and uke have always run) and it still parked at CP_BV_BR_COUNT_OPS on the sixth
     * submission, so the question is no longer what BR does, it is whether BV was ever given an IB at all.
     * memptrs+8 is bv_rptr as the CPU sees it (NativeAdrenoGpuContext.h: +0 rptr, +4 fence, +8 bv_rptr, +12 bv_fence), so a
     * zero there after eleven submissions says BV never consumed a single packet. */
    {
        const uint32_t bvRptr = gpuRd(0x0a66), bvIb1Lo = gpuRd(0x0a6d), bvIb1Hi = gpuRd(0x0a6e);
        const uint32_t bvIb1Rem = gpuRd(0x0a70), bvHw = gpuRd(0x0a64), bvProt = gpuRd(0x0a61);
        const uint32_t bvAvailRb = gpuRd(0x0a8f), bvAvailIb1 = gpuRd(0x0a90), bvAvailIb2 = gpuRd(0x0a91);
        const uint32_t bvChick = gpuRd(0x0ada), csCntl = gpuRd(0x08a0), csRestore = gpuRd(0x08a3);
        const uint32_t brRptr = gpuRd(0x0806), brIb1Lo = gpuRd(0x0928), brIb1Hi = gpuRd(0x0929), brIb1Rem = gpuRd(0x092a);
        const volatile uint32_t *mp = reinterpret_cast<const volatile uint32_t *>(gGpuCpu[2]);
        const uint32_t mpRptr = mp ? mp[0] : 0xffffffffu, mpFence = mp ? mp[1] : 0xffffffffu;
        const uint32_t mpBvRptr = mp ? mp[2] : 0xffffffffu, mpBvFence = mp ? mp[3] : 0xffffffffu;
        IOLog("MUYU BV rptr=%08x ib1=%08x%08x rem=%08x hw=%08x prot=%08x avail=%x/%x/%x chick=%08x cs=%08x rest=%08x | br rptr=%08x ib1=%08x%08x rem=%08x | mp %08x/%08x bv %08x/%08x\n",
              bvRptr,bvIb1Hi,bvIb1Lo,bvIb1Rem,bvHw,bvProt,bvAvailRb,bvAvailIb1,bvAvailIb2,bvChick,csCntl,csRestore,
              brRptr,brIb1Hi,brIb1Lo,brIb1Rem,mpRptr,mpFence,mpBvRptr,mpBvFence);
    }
    /* fix114: whose value 0x0e17 keeps. fix113 wrote UCHE_CACHE_WAYS from the CPU between submissions and caught it at 0x00800000
     * immediately after the write and at 0x00000000 eight submissions later, every single time - so something that runs between
     * passes (a GX power cycle plus its power-up register list, exactly what NativeAdrenoPwrupRegs.h describes) puts it back. The
     * list itself is our own buffer: 16-byte header, then (register, value) pairs, in gGpuCpu[3]. This prints the entry's current
     * value - the value every power-up re-applies - and then forces it to BIT(23), the value hw_init writes, so that from here on
     * the passes run with the ways enabled. Nothing on the GPU is written; the list is host memory the CP reads. */
    {
        volatile uint32_t *list = reinterpret_cast<volatile uint32_t *>(gGpuCpu[3]);
        static unsigned printed;
        unsigned slot = 0xffffu;
        uint32_t was = 0;
        for (unsigned i = 0; i < 16; ++i) if (list[4 + 2 * i] == 0x0e17u) {slot = i; was = list[4 + 2 * i + 1]; break;}
        if (!printed) {
            printed = 1;
            IOLog("MUYU PWL0 hdr=%08x ifpc=%u pre=%u dyn=%u\n", list[3], (unsigned)(list[3] & 0xffu), (unsigned)((list[3] >> 8) & 0xffu),
                  (unsigned)((list[3] >> 16) & 0xffffu));
            for (unsigned i = 0; i < 16; i += 8)
                IOLog("MUYU PWL0 +%u %08x=%08x %08x=%08x %08x=%08x %08x=%08x\n", i,
                      list[4 + 2 * i], list[5 + 2 * i], list[6 + 2 * i], list[7 + 2 * i],
                      list[8 + 2 * i], list[9 + 2 * i], list[10 + 2 * i], list[11 + 2 * i]);
        }
        if (slot != 0xffffu) {
            list[4 + 2 * slot + 1] = 1u << 23;
            __asm__ volatile("dsb sy" ::: "memory");
            IOLog("MUYU PWL slot=%u reg=%08x was=%08x now=%08x\n", slot, list[4 + 2 * slot], was, list[4 + 2 * slot + 1]);
        } else IOLog("MUYU PWL no 0e17 entry in the list\n");
    }
    IOLog("MUYU SMMUFAULT fsr=%x far=%x fsynr=%x gfsr=%x cbfsr=%x\n", base[(bank0 + 0x58) / 4], base[(bank0 + 0x5C) / 4],
          base[(bank0 + 0x68) / 4], base[0x48 / 4], base[(bank0 + 0x6C) / 4]);
    /* fix45: fix44 left the CP stopped at the fence packet of the first submission that carried a real IB, with no SMMU fault, while
     * four identical fence packets had completed before it. Two very different faults look exactly like that - the CP waiting on
     * something inside the IB, and the CP not running at all (a collapse it never woke from) - so ask the hardware instead of
     * guessing: put a one-dword CP_NOP at WPTR, publish the pointer and watch whether RPTR moves. Moving means alive and waiting;
     * frozen means the ring is not being fetched at all, and the next place to look is the GMU/power state, not the IB. */
    {
        static unsigned kicked;
        if (!kicked) {
            ++kicked;
            const uint32_t rptr = gpuRd(A740_CP_RB_RPTR), wptr = gpuRd(A740_CP_RB_WPTR);
            const uint32_t gx = gVa[GPB_WIN_GPUCC] ?
                *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPUCC] + GPB_CC_GX_GDSCR) : 0;
            uint32_t after = rptr;
            /* fix64: READ-ONLY. This probe used to push a one-dword CP_NOP at WPTR and write
             * CP_RB_WPTR itself. That moved the hardware RPTR one dword past the driver's next_,
             * so NativeAdrenoRing::submitWords() computed used=(next_-rptr)&mask == ring_size-1
             * and returned kIOReturnNoResources -> kIOReturnBusy on EVERY later submission.
             * fix63's own "rptr=74 wptr=74 after=75" proved the CP was alive and had drained
             * the ring; the very next frame was the first of an endless e00002d5 loop. Polling
             * RPTR alone answers the same liveness question without corrupting the ring cursor. */
            for (unsigned i = 0; i < 100; ++i) {IODelay(200); if ((after = gpuRd(A740_CP_RB_RPTR)) != rptr) break;}
            IOLog("MUYU CPKICK rptr=%x wptr=%x after=%x gdscr=%08x status=%x status3=%x\n", rptr, wptr, after, gx,
                  gpuRd(A740_RBBM_STATUS), gpuRd(A740_RBBM_STATUS3));
        }
    }
    /* fix46: fix45's kick showed the CP does not fetch at all (RPTR frozen through 20 ms of polls with the GX domain on and no fault), so
     * the ring, the fence packet and the fetch path are all out and the question is where inside the client's IB the CP sits. CP_IB1_BASE
     * and CP_IB1_REM answer that directly - an active IB with a non-zero remainder is a CP stopped *inside* the program, a zero base is a
     * CP stopped between packets.
     */
    {
        static unsigned ibs;
        if (!ibs) {
            ++ibs;
            IOLog("MUYU CPIB ib1=%08x%08x rem=%x ib2=%08x%08x rem2=%x roq=%x/%x\n",
                  gpuRd(0x0929), gpuRd(0x0928), gpuRd(0x092A), gpuRd(0x092D), gpuRd(0x092B), gpuRd(0x092C),
                  gpuRd(0x0949), gpuRd(0x094A));
        }
    }
    /* fix47: fix46 left exactly two readings open, and both are answered from the installed table without touching the CP.
     * (1) Does the table the CP is running map the client's IB? gpuWalk() walks CB0_TTBR0 as it stands right now, so the leaf
     * descriptor, the physical page and the first two dwords of what the GPU would fetch are the translation and the contents
     * the CP sees - "no descriptor" and "descriptor, page zeros" are different root causes that the log so far could not tell
     * apart. (2) Where is the SQE? CP_SQE_STAT_DATA after selecting index 0 is the microcode PC of the BR/BV thread: an idle/WFI
     * loop means the front end was halted (restart is the only way out), a fetcher loop means it is waiting on the memory system.
     * CP_HW_FAULT, CP_SQE_CNTL and CP_APERTURE_CNTL_HOST rule out a packet error, a disabled SQE and a host aperture that points
     * at another hardware context, in that order. Read-only: no register is written except the SQE_STAT index select. */
    {
        static unsigned ibw;
        if (!ibw) {
            ++ibw;
            const uint64_t ib = (uint64_t(gpuRd(0x0929)) << 32) | gpuRd(0x0928);
            uint64_t pa = 0, w0 = 0, w1 = 0;
            const uint64_t desc = ib ? gpuWalk(ib, &pa) : 0;
            if (desc) {physRead64(pa, &w0); physRead64(pa + 4, &w1);}
            gpuWr(0x0908, 0);
            const uint32_t sqe0 = gpuRd(0x0909), sqe1 = gpuRd(0x0909);
            gpuWr(0x0a87, 0);
            const uint32_t bv0 = gpuRd(0x0a88), bv1 = gpuRd(0x0a88);
            IOLog("MUYU IBW d=%llx pa=%llx w=%08x/%08x sqe=%x/%08x bv=%x/%08x hw=%x ctl=%x ap=%x rb=%x st1=%x co=%u c%x u%x v%x fc=%u/%u/%x/%u wca=%u/%u\n",
                  (unsigned long long)desc, (unsigned long long)pa, uint32_t(w0), uint32_t(w1), sqe0, sqe1, bv0, bv1,
                  gpuRd(A740_CP_HW_FAULT), gpuRd(A740_CP_SQE_CNTL), gpuRd(A740_CP_APERTURE_CNTL_HOST),
                  gpuRd(A740_CP_RB_CNTL), gpuRd(0x0825), (unsigned)native_adreno_coherence[0],
                  native_adreno_coherence[1], native_adreno_coherence[2], native_adreno_coherence[3],
                  (unsigned)native_adreno_fix50_clean[0], (unsigned)native_adreno_fix50_clean[1],
                  (unsigned)native_adreno_fix50_clean[2], (unsigned)native_adreno_fix50_clean[3],
                  (unsigned)native_adreno_wc_audit[0], (unsigned)native_adreno_wc_audit[2]);
        }
    }
}
// SIP SMC (qcom_scm-smc.c): x0 function, x1 arginfo, x2.. arguments. QCOM_SCM_INTERRUPTED (1) resumes with x0 = 1 and
// the returned x6 (ARM_SMCCC_QUIRK_QCOM_A6); QCOM_SCM_V2_EBUSY (-12) retries after 30 ms, at most 20 times. WAITQ_SLEEP
// (2) needs the SCM wait-queue interrupt, which is not wired here, so it is returned to the caller as is.
struct ScmRes {int64_t a0; uint64_t a1; uint32_t ms, resumes;};
ScmRes scmCall(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3 = 0) {
    uint64_t first = fn, a6 = 0;
    const uint64_t start = ringNow(nullptr);
    auto done = [&](int64_t a0, uint64_t a1, unsigned resumes) {
        return ScmRes{a0, a1, uint32_t((ringNow(nullptr) - start) / 1000), resumes};
    };
    for (unsigned resume = 0, busy = 0; resume < 10000;) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = arginfo, x2 asm("x2") = p0, x3 asm("x3") = p1;
        register uint64_t x4 asm("x4") = p2, x5 asm("x5") = p3, x6 asm("x6") = a6, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {first = 1; a6 = x6; ++resume; continue;}
        if (int64_t(x0) == -12 && busy++ < 20) {first = fn; a6 = 0; IOSleep(30); continue;}
        return done(int64_t(x0), x1, resume);
    }
    return done(-1000, 0, 10000);
}
// ARM_SMCCC_CALL_VAL(STD, SMC_64, OWNER_SIP, svc << 8 | cmd).
constexpr uint64_t kScmPil(uint32_t cmd) {return 0x42000000u | (0x02u << 8) | cmd;}
constexpr uint32_t kGpuPasId = 13;                                     // a6xx_gpu.h GPU_PAS_ID
constexpr uint64_t kZapPhys = 0x9b09a000, kZapBytes = 0x2000;          // sm8550.dtsi gpu_micro_code_mem
NativeAdrenoMemory gZapMetadata;
/* fix150: K took the msm -ENODEV path (RBBM_SECVID_TRUST_CNTL = 0). gpuRestart() must replay the
 * SAME operation, not CP_SET_SECURE_MODE, or a hang recovery would silently introduce the other
 * experiment's variable into this run. */
static bool gSecK = false;
extern "C" {
// stage, init a0/result, mem_setup a0/result, auth a0/result, secure-mode fence result
int32_t native_gpu_zap[8];
// per SMC (init, mem_setup, auth): milliseconds and INTERRUPTED resumes; free pages before the zap and 2 s after its last step
uint32_t native_gpu_zap_cost[6], native_gpu_zap_free[2];
extern unsigned int vm_page_free_count __attribute__((weak));
}
uint32_t rd32le(const uint8_t *p) {return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;}
uint16_t rd16le(const uint8_t *p) {return uint16_t(p[0] | p[1] << 8);}
unsigned zapLimit() {
    // sheng-gpu-zap: 0 off, 1 through init_image, 2 through mem_setup and the segment copy, 3 through auth_and_reset,
    // 4 (default) also CP_SET_SECURE_MODE 0. int-019 (0) reached the desktop and int-017 (4) did not - but that
    // "desktop" was later proven to be the offline-legacy CPU-copy artefact, not a GPU pixel criterion.
    unsigned zap = 4;
    PE_parse_boot_argn("sheng-gpu-zap", &zap, sizeof zap);
    /* fix152: the command line is at its 1016-byte limit, so the kill switch rides the carrier.
     * 'L' restores the old no-zap behaviour without a rebuild if the TZ sequence misbehaves. */
    char carrier[24] = {};
    if (PE_parse_boot_argn("sheng-tum", carrier, sizeof carrier))
        for (const char *c = carrier; *c; ++c) if (*c == 'L') return 0;
    return zap;
}
void zapRow() {
    // Z<stage>/<limit> i<init a0>.<ms>.<resumes> m<mem_setup ...> a<auth ...> s<secure fence> f<free pages delta>
    char row[64];
    const int32_t *z = native_gpu_zap;
    const uint32_t *c = native_gpu_zap_cost;
    snprintf(row, sizeof row, "Z%d/%u i%d.%u.%u m%d.%u.%u a%d.%u.%u s%d f%d", z[0], zapLimit(), z[1], c[0] & 0xffff,
             c[0] >> 16, z[3], c[1] & 0xffff, c[1] >> 16, z[5], c[2] & 0xffff, c[2] >> 16, z[7],
             int32_t(native_gpu_zap_free[1] - native_gpu_zap_free[0]));
    setRow(2, row);
}
void zapEnd() {
    /* fix152: every exit from zapAndSecure() lands here. Log the exact stage and the SECVID state
     * so a TZ rejection is distinguishable from a successful auth that still left secure mode on. */
    const int32_t *z = native_gpu_zap;
    IOLog("MUYU ZAPEND stage=%d limit=%u init=%d mem=%d auth=%d wait=%d size=%llu trust=%08x tsb=%08x%08x\n",
          z[0], zapLimit(), z[1], z[3], z[5], z[7],
          (unsigned long long)native_adreno_zap_image_size,
          gpuRd(0xf400), gpuRd(0xfc01), gpuRd(0xfc00));
    IOSleep(2000);
    native_gpu_zap_free[1] = &vm_page_free_count ? vm_page_free_count : 0;
    zapRow();
}
void zapAndSecure() {
    int32_t *z = native_gpu_zap;
    native_gpu_zap_free[0] = native_gpu_zap_free[1] = &vm_page_free_count ? vm_page_free_count : 0;
    z[0] = 1; zapRow();
    /* fix152: the image is now a strong definition in this translation unit, so the old
     * weak-symbol probe (&size ? image : nullptr) is always true and -Werror rejects it. */
    const uint8_t *fw = native_adreno_zap_image;
    const uint64_t size = native_adreno_zap_image_size;
    // mdt_header_valid: ELF32 magic, 32-byte program headers inside the file; qcom_mdt_read_metadata: >= 2 headers, the
    // first not PT_LOAD, a hash segment (p_flags type 2 in bits 24..26).
    /* fix150: the 17 line links the weak empty zap default, so this branch is the normal path and
     * CP_SET_SECURE_MODE (0x66) is never emitted: the two senders are zapAndSecure() itself (skipped
     * right here) and gpuRestart() (gated on native_gpu_zap[0] == 9). Linux msm emits 0x66 after a
     * successful zap load to take the CP out of secure mode; a CP left in secure mode would explain
     * GX-side stores vanishing with no SMMU fault, no GBIF halt and no stall, while CX-side fence
     * writes still land. History demoted this line because "zap=0 still reached the desktop", but
     * that desktop was later proven to be a CPU-copy artefact, so it was never tested against a
     * hard pixel criterion. J is a single-variable probe: send 0x66 with NO zap shader and NO TZ/SMC
     * call at all, at exactly the bring-up point msm uses (c[0]==10 scratch write + fence done,
     * before the first client submit). Everything else stays byte-identical to the swCD baseline.
     *   RBBM_SECVID_TRUST_CNTL 0xf400, TSB_STATUS 0xfc00/1 (A7XX-only), TSB_CNTL 0xf803,
     *   TSB_TRUSTED_BASE 0xf800/1, TSB_TRUSTED_SIZE 0xf802 - all inside the 0x40000 GPU window. */
    if (size < 52 || fw[0] != 0x7f || fw[1] != 'E' || fw[2] != 'L' || fw[3] != 'F' || fw[4] != 1) {
        char carrier[24] = {};
        bool secJ = false, secK = false;
        if (PE_parse_boot_argn("sheng-tum", carrier, sizeof carrier))
            for (const char *c = carrier; *c; ++c) {
                if (*c == 'J') secJ = true;
                if (*c == 'K') secK = true;
            }
        const uint32_t trust = gpuRd(0xf400), tsbLo = gpuRd(0xfc00), tsbHi = gpuRd(0xfc01);
        const uint32_t tsbCntl = gpuRd(0xf803), baseLo = gpuRd(0xf800), baseHi = gpuRd(0xf801), baseSz = gpuRd(0xf802);
        IOLog("MUYU SECZ size=%llu J=%u K=%u trust=%08x tsb=%08x%08x cntl=%08x base=%08x%08x sz=%08x\n",
              (unsigned long long)size, secJ ? 1u : 0u, secK ? 1u : 0u,
              trust, tsbHi, tsbLo, tsbCntl, baseHi, baseLo, baseSz);
        if (!secJ && !secK) {z[0] = -1; zapRow(); return;}
        /* msm a6xx_gpu.c:1507-1517: when a6xx_zap_shader_init() returns -ENODEV (this board links
         * the empty weak zap image), the ONLY supported way out of secure mode is a plain CPU write
         * of RBBM_SECVID_TRUST_CNTL = 0. K does exactly that, with no CP packet and no SMC call.
         * J is the other msm branch (1500-1506): CP_SET_SECURE_MODE(0) after a *successful* zap
         * load. Kept as a separate carrier for the next round if K is not sufficient. */
        if (secK) {
            gpuWr(0xf400, 0x0);
            __asm__ volatile("dsb sy" ::: "memory");
            const uint32_t readback = gpuRd(0xf400);
            const uint32_t tsbLo2 = gpuRd(0xfc00), tsbHi2 = gpuRd(0xfc01);
            gSecK = true;
            z[0] = 9;   // gpuRestart() then replays the same *write* (see gSecK), not 0x66
            IOLog("MUYU SECK wr=0 readback=%08x tsb=%08x%08x stage=%d\n", readback, tsbHi2, tsbLo2, z[0]);
            zapRow();
            return;
        }
        uint32_t words[2] = {};
        uint32_t fence = 0, seen = 0;
        if (AdrenoPM4Type7(0x66, 1, &words[0]) != AdrenoPM4Success || gRing.submit(words, 2, &fence)) {z[0] = -11; zapRow(); return;}
        const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
        z[7] = int32_t(gRing.wait(fence, &clock, 200000, &seen));
        z[0] = z[7] ? 8 : 9;   // 9: the CP left secure mode without a zap shader
        const uint32_t trust2 = gpuRd(0xf400), tsbLo2 = gpuRd(0xfc00), tsbHi2 = gpuRd(0xfc01);
        IOLog("MUYU SECJ wait=%d seen=%u stage=%d trust=%08x tsb=%08x%08x\n",
              z[7], seen, z[0], trust2, tsbHi2, tsbLo2);
        zapRow();
        return;
    }
    const uint32_t phoff = rd32le(fw + 28);
    const unsigned phent = rd16le(fw + 42), phnum = rd16le(fw + 44);
    if (phent != 32 || phnum < 2 || phnum > 16 || phoff + uint64_t(phnum) * 32 > size) {z[0] = -2; zapRow(); return;}
    auto ph = [&](unsigned i, unsigned field) {return rd32le(fw + phoff + i * 32 + field * 4);};   // type off va pa filesz memsz flags
    if (ph(0, 0) == 1) {z[0] = -3; zapRow(); return;}
    unsigned hash = 0;
    for (unsigned i = 1; i < phnum && !hash; ++i) if (((ph(i, 6) >> 24) & 7) == 2) hash = i;
    if (!hash) {z[0] = -4; zapRow(); return;}
    uint64_t lo = ~0ull, hi = 0;
    bool relocate = false;
    for (unsigned i = 0; i < phnum; ++i) {
        if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;   // mdt_phdr_loadable
        if (ph(i, 6) & (1u << 27)) relocate = true;
        lo = lo < ph(i, 3) ? lo : ph(i, 3);
        const uint64_t end = (uint64_t(ph(i, 3)) + ph(i, 5) + 0xfff) & ~0xfffull;
        hi = hi > end ? hi : end;
    }
    if (lo >= hi || hi - lo > kZapBytes) {z[0] = -5; zapRow(); return;}
    const uint64_t ehdr = ph(0, 4), hashBytes = ph(hash, 4), hashOff = ph(hash, 1);
    if (ehdr > size || hashOff + hashBytes > size || ehdr + hashBytes > 0x4000) {z[0] = -6; zapRow(); return;}
    // Metadata: physically contiguous, 4K aligned, pushed to the point of coherency (Linux: dma_alloc_coherent).
    z[0] = 2; zapRow();
    const size_t metaBytes = size_t((ehdr + hashBytes + 0xfff) & ~0xfffull);
    if (gZapMetadata.allocate(metaBytes, 40, true, metaBytes) || gZapMetadata.spanCount() != 1) {z[0] = -7; zapRow(); return;}
    uint8_t *meta = static_cast<uint8_t *>(gZapMetadata.mapping());
    for (uint64_t i = 0; i < ehdr; ++i) meta[i] = fw[i];
    for (uint64_t i = 0; i < hashBytes; ++i) meta[ehdr + i] = fw[hashOff + i];
    if (gZapMetadata.publishForDevice(0, metaBytes, true)) {z[0] = -8; zapRow(); return;}
    // qcom_scm_pas_init_image: ARGS(2, VAL, RW).
    z[0] = 3; zapRow();
    ScmRes r = scmCall(kScmPil(0x01), 2 | (2u << 6), kGpuPasId, gZapMetadata.spans()[0].physical, 0);
    z[1] = int32_t(r.a0 ? r.a0 : int64_t(r.a1)); native_gpu_zap_cost[0] = (r.ms & 0xffff) | (r.resumes << 16);
    if (r.a0 || r.a1 || zapLimit() < 2) {zapEnd(); return;}
    if (!relocate) z[3] = -99;   // m-99: no relocatable segment, so mem_setup was not called
    if (relocate) {
        // qcom_scm_pas_mem_setup: ARGS(3).
        z[0] = 4; zapRow();
        r = scmCall(kScmPil(0x02), 3, kGpuPasId, kZapPhys, hi - lo);
        z[3] = int32_t(r.a0 ? r.a0 : int64_t(r.a1)); native_gpu_zap_cost[1] = (r.ms & 0xffff) | (r.resumes << 16);
        if (r.a0 || r.a1) {zapEnd(); return;}
    }
    // qcom_mdt_load_no_init into the reserved region (relocatable: offsets from the lowest segment address).
    z[0] = 5; zapRow();
    {
        IODeviceMemory *m = IODeviceMemory::withRange(kZapPhys, kZapBytes);
        OSSharedPtr<IOMemoryMap> map;
        if (m) {map = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain); m->release();}
        if (!map) {z[0] = -9; zapRow(); return;}
        volatile uint8_t *region = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
        const uint64_t base = relocate ? lo : kZapPhys;
        for (unsigned i = 0; i < phnum; ++i) {
            if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;
            const uint64_t at = uint64_t(ph(i, 3)) - base, file = ph(i, 4), mem = ph(i, 5), off = ph(i, 1);
            if (ph(i, 3) < base || at + mem > kZapBytes || file > mem || off + file > size) {z[0] = -10; zapRow(); return;}
            for (uint64_t b = 0; b < file; ++b) region[at + b] = fw[off + b];
            for (uint64_t b = file; b < mem; ++b) region[at + b] = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }   // the CPU view is gone before TZ takes the region
    if (zapLimit() < 3) {zapEnd(); return;}
    // qcom_scm_pas_auth_and_reset: ARGS(1).
    z[0] = 6; zapRow();
    r = scmCall(kScmPil(0x05), 1, kGpuPasId, 0, 0);
    z[5] = int32_t(r.a0 ? r.a0 : int64_t(r.a1)); native_gpu_zap_cost[2] = (r.ms & 0xffff) | (r.resumes << 16);
    if (r.a0 || r.a1 || zapLimit() < 4) {zapEnd(); return;}
    // hw_init after a successful zap load: CP_SET_SECURE_MODE (0x66) 0, then idle.
    z[0] = 7; zapRow();
    uint32_t words[2] = {};
    uint32_t fence = 0, seen = 0;
    if (AdrenoPM4Type7(0x66, 1, &words[0]) != AdrenoPM4Success || gRing.submit(words, 2, &fence)) {z[0] = -11; zapRow(); return;}
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    z[7] = int32_t(gRing.wait(fence, &clock, 200000, &seen));
    z[0] = z[7] ? 8 : 9;   // 9: TZ authenticated the zap shader and the CP left secure mode
    IOLog("MUYU ZAPOK stage=%d wait=%d seen=%u trust=%08x tsb=%08x%08x\n",
          z[0], z[7], seen, gpuRd(0xf400), gpuRd(0xfc01), gpuRd(0xfc00));
    zapEnd();
}
extern "C" {
// GPU interrupt (row 3): attach status, GICD readback, handled interrupts, OR of the INT_0_STATUS words they cleared
uint32_t native_gpu_irq[6];
IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback,
                           uint64_t *router) __attribute__((weak));
extern volatile uint64_t armcpu_spi_unhandled __attribute__((weak));
}
volatile uint32_t gIrqCount, gIrqLast, gIrqSeen;
void gpuIrq(void *) {
    // a6xx_irq: read RBBM_INT_0_STATUS and clear it through RBBM_INT_CLEAR_CMD before anything else, so the level line drops
    // before the EOI. A storm (no source cleared) masks the GPU interrupts instead of hanging the boot CPU.
    const uint32_t status = gpuRd(A740_RBBM_INT_0_STATUS);
    if (status) gpuWr(A740_RBBM_INT_CLEAR_CMD, status);
    gIrqLast = status;
    gIrqSeen = gIrqSeen | status;
    gIrqCount = gIrqCount + 1;
    if (gIrqCount > 100000) gpuWr(A740_RBBM_INT_0_MASK, 0);
}
bool cpFence(const uint32_t *words, size_t count) {
    uint32_t fence = 0, seen = 0;
    if (gRing.submit(words, count, &fence)) return false;
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    return !gRing.wait(fence, &clock, 200000, &seen);
}
void irqAndTimer() {
    // int-017..022 qualified the always-on counter (19.2 MHz against CNTVCT) and found SPI 300 disabled in GICD: ArmCpu's
    // distributor init disables every SPI. Now the line is attached for real.
    char row[64];
    uint32_t *q = native_gpu_irq;
    // GPU bulk copy: CP_MEMCPY (0x75: dwords, src lo/hi, dst lo/hi) of 1023 dwords of the SQE buffer to scratch+4, then
    // CP_WAIT_MEM_WRITES (0x12) and the fence; the CPU compares every dword. M<matching dwords> or M-1 when the fence failed.
    uint32_t words[8] = {};
    const uint64_t src = gGpuBuffers[0].iova, dst = gGpuBuffers[4].iova + 4;
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gGpuCpu[4]);
    for (unsigned i = 1; i < 1024; ++i) scratch[i] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    int matched = -100;
    if (AdrenoPM4Type7(0x75, 5, &words[0]) == AdrenoPM4Success && AdrenoPM4Type7(0x12, 0, &words[6]) == AdrenoPM4Success) {
        words[1] = 1023; words[2] = uint32_t(src); words[3] = uint32_t(src >> 32); words[4] = uint32_t(dst);
        words[5] = uint32_t(dst >> 32);
        if (!cpFence(words, 7)) matched = -1;
        else {
            const volatile uint32_t *sqe = reinterpret_cast<const volatile uint32_t *>(gGpuCpu[0]);
            matched = 0;
            for (unsigned i = 0; i < 1023; ++i) matched += scratch[1 + i] == sqe[i];
        }
    }
    // SPI 300 (sm8550.dtsi gpu interrupts, IRQ_TYPE_LEVEL_HIGH) = INTID 332. The memcpy fence left CP_CACHE_FLUSH_TS (bit 20)
    // set, so a working line fires as soon as it is enabled; one more fence (CP_NOP) must fire it again.
    uint32_t readback = 0;
    uint64_t router = ~0ull;
    q[0] = armcpu_spi_attach ? uint32_t(armcpu_spi_attach(332, false, gpuIrq, nullptr, &readback, &router)) : 0xdead;
    q[1] = readback;
    IOSleep(20);
    q[2] = gIrqCount;
    uint32_t nop = 0;
    const bool fenced = AdrenoPM4Type7(0x10, 0, &nop) == AdrenoPM4Success && cpFence(&nop, 1);
    IOSleep(20);
    q[3] = gIrqCount;
    q[4] = gIrqSeen;
    q[5] = gpuRd(A740_RBBM_INT_0_STATUS);
    // I a<attach IOReturn> b<group|enabled<<1|edge<<2|prio<<8> r<IROUTER> n<after attach>/<after a fence><fence ok>
    //   l<seen status> s<status now> M<copied dwords>
    snprintf(row, sizeof row, "I a%x b%x r%llx n%u/%u%c l%x s%x M%d", q[0], q[1], (unsigned long long)router, q[2], q[3],
             fenced ? '+' : '-', q[4], q[5], matched);
    setRow(3, row);
}

NativeAdrenoSubmit gSubmit;
NativeAdrenoUserClient gClient;
extern "C" {
// open, query, then per round: alloc/map status, submit, wait, verified words (of 4), cpu map/unmap, frees, GFSR
uint32_t native_gpu_uc[16];
}
volatile uint32_t gUcCalls, gWCalls;
IOReturn ucCall(uint32_t method, const void *in, void *out) {
    // Position marker before each call, so a stall leaves the call on screen: U m<method> n<call number>
    char mark[64];
    gUcCalls = gUcCalls + 1;
    snprintf(mark, sizeof mark, "U run m%u n%u", method, gUcCalls);
    setRow(6, mark);
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    const size_t os = NativeAdrenoUserClient::outputSize(method);
    return gClient.dispatch(method, in, NativeAdrenoUserClient::inputSize(method), os ? out : nullptr, os, &clock);
}
// One round through the user-client dispatch only: two buffer objects (an indirect buffer, a target), mapped at fresh GPU
// addresses, the IB writing three known dwords into the target with CP_MEM_WRITE, submitted, waited on by the CP timestamp
// fence, read back by the CPU, cpu-mapped/unmapped, then unmapped and freed (live SMMU unmap + TLB invalidate).
// Returns the matching dwords (of 4: three written, one untouched) or -1.
int ucRound(unsigned round, uint64_t ibIova, uint64_t tgtIova, uint32_t pattern, IOReturn &err, int &errAt, int &ok, int &n) {
    auto step = [&](IOReturn s) {
        const int idx = n++;
        if (!s) ++ok;
        else if (!err) {err = s; errAt = idx;}
        return !s;
    };
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib{}, tgt{};
    if (!step(ucCall(NativeAdrenoMethodBoAlloc, &alloc, &ib)) || !step(ucCall(NativeAdrenoMethodBoAlloc, &alloc, &tgt))) return -1;
    NativeAdrenoAbiBoMap mapIb{ib.handle, ibIova, 1, 0, 2, 0}, mapTgt{tgt.handle, tgtIova, 3, 0, 2, 0};
    NativeAdrenoAbiBoMapReply r1{}, r2{};
    if (!step(ucCall(NativeAdrenoMethodBoMap, &mapIb, &r1)) || !step(ucCall(NativeAdrenoMethodBoMap, &mapTgt, &r2))) return -1;
    NativeAdrenoMemory *ibMem = gClient.memoryOf(ib.handle), *tgtMem = gClient.memoryOf(tgt.handle);
    if (!ibMem || !tgtMem) {step(kIOReturnNotFound); return -1;}
    volatile uint32_t *iw = static_cast<volatile uint32_t *>(ibMem->mapping()), *tw = static_cast<volatile uint32_t *>(tgtMem->mapping());
    for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
    const uint32_t values[3] = {pattern, ~pattern, 0x600d0000u | round};
    uint32_t words[12];
    for (unsigned i = 0; i < 3; ++i) {
        if (AdrenoPM4Type7(0x3d, 3, &words[i * 4]) != AdrenoPM4Success) {step(kIOReturnInternalError); return -1;}
        words[i * 4 + 1] = uint32_t(tgtIova + i * 4); words[i * 4 + 2] = uint32_t(tgtIova >> 32); words[i * 4 + 3] = values[i];
    }
    for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
    if (!step(ibMem->publishForDevice(0, 4096, false)) || !step(tgtMem->publishForDevice(0, 4096, true))) return -1;
    NativeAdrenoAbiSubmit submit{};
    submit.buffer_count = 1;
    submit.buffers[0] = {ib.handle, ibIova, sizeof words};
    NativeAdrenoAbiSubmitReply sr{};
    int matched = -1;
    if (step(ucCall(NativeAdrenoMethodSubmit, &submit, &sr))) {
        NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
        if (step(ucCall(NativeAdrenoMethodWaitFence, &wait, nullptr)) && step(tgtMem->acquireDeviceWrites(0, 4096))) {
            matched = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
        }
    }
    NativeAdrenoAbiBoCpuMap cm{tgt.handle};
    NativeAdrenoAbiBoCpuMapReply cr{};
    if (step(ucCall(NativeAdrenoMethodBoCpuMap, &cm, &cr))) {
        NativeAdrenoAbiBoCpuUnmap cu{tgt.handle, cr.token, 0};
        step(ucCall(NativeAdrenoMethodBoCpuUnmap, &cu, nullptr));
    }
    NativeAdrenoAbiBoFree f1{ib.handle}, f2{tgt.handle};
    step(ucCall(NativeAdrenoMethodBoFree, &f1, nullptr));
    step(ucCall(NativeAdrenoMethodBoFree, &f2, nullptr));
    return matched;
}
// Limits and paths the Vulkan driver depends on, driven through the same dispatch entry: many live objects (the table was 64),
// one big buffer spread over many physical spans, a burst of submissions with no waits (the kernel has eight job slots and
// must retire finished ones itself; the userspace backend waits on the oldest fence when it sees Busy), and a wait on a fence
// whose job was already retired. Row 7 (48 bytes): "K a<mapped of 200> f<freed> L<large alloc status> s<spans of the 16 MiB BO>
// v<big words ok of 2> J<jobs of 20> y<busy retries> m<burst result matches> r<retired-fence wait status> t<big map ms>".
void ucStress() {
    char row[64];
    constexpr unsigned kMany = 200;
    static uint64_t handles[kMany];
    unsigned mapped = 0, freed = 0;
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    for (unsigned i = 0; i < kMany; ++i) {
        NativeAdrenoAbiBoAllocReply reply{};
        if (ucCall(NativeAdrenoMethodBoAlloc, &alloc, &reply) || !reply.handle) break;
        handles[i] = reply.handle;
        NativeAdrenoAbiBoMap map{reply.handle, 0x30000000ull + uint64_t(i) * 0x2000, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply mr{};
        if (ucCall(NativeAdrenoMethodBoMap, &map, &mr)) {
            NativeAdrenoAbiBoFree f{reply.handle};
            ucCall(NativeAdrenoMethodBoFree, &f, nullptr);
            break;
        }
        ++mapped;
    }
    for (unsigned i = 0; i < mapped; ++i) {
        NativeAdrenoAbiBoFree f{handles[i]};
        if (!ucCall(NativeAdrenoMethodBoFree, &f, nullptr)) ++freed;
    }
    // One 16 MiB buffer: IB writes the first and last dword through the GPU's own translation of a many-span mapping.
    NativeAdrenoAbiBoAlloc bigAlloc{16ull << 20, 12, 0}, small{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply big{}, ib{};
    IOReturn bigStatus = ucCall(NativeAdrenoMethodBoAlloc, &bigAlloc, &big);
    unsigned spans = 0, mapMs = 0;
    int bigWords = -1;
    constexpr uint64_t kBigIova = 0x40000000ull, kIbIova = 0x10010000ull;
    if (!bigStatus && !ucCall(NativeAdrenoMethodBoAlloc, &small, &ib)) {
        NativeAdrenoAbiBoMap mb{big.handle, kBigIova, 3, 0, 2, 0}, mi{ib.handle, kIbIova, 1, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        NativeAdrenoMemory *bm = gClient.memoryOf(big.handle), *im = gClient.memoryOf(ib.handle);
        if (bm) spans = unsigned(bm->spanCount());
        const uint64_t mapStart = ringNow(nullptr);
        const IOReturn bigMap = bm && im ? ucCall(NativeAdrenoMethodBoMap, &mb, &r) : kIOReturnNotReady;
        mapMs = unsigned((ringNow(nullptr) - mapStart) / 1000);
        if (bm && im && !bigMap && !ucCall(NativeAdrenoMethodBoMap, &mi, &r)) {
            volatile uint32_t *bw = static_cast<volatile uint32_t *>(bm->mapping());
            volatile uint32_t *iw = static_cast<volatile uint32_t *>(im->mapping());
            const size_t last = ((16u << 20) / 4) - 1;
            bw[0] = 0xdeadbeef;
            bw[last] = 0xdeadbeef;
            const uint64_t addrs[2] = {kBigIova, kBigIova + (16ull << 20) - 4};
            const uint32_t vals[2] = {0x11112222u, 0x33334444u};
            uint32_t words[8];
            bool built = true;
            for (unsigned i = 0; i < 2; ++i) {
                built = built && AdrenoPM4Type7(0x3d, 3, &words[i * 4]) == AdrenoPM4Success;
                words[i * 4 + 1] = uint32_t(addrs[i]); words[i * 4 + 2] = uint32_t(addrs[i] >> 32); words[i * 4 + 3] = vals[i];
            }
            for (unsigned i = 0; i < 8; ++i) iw[i] = words[i];
            if (built && !im->publishForDevice(0, 4096, false) && !bm->publishForDevice(0, 16u << 20, true)) {
                NativeAdrenoAbiSubmit submit{};
                submit.buffer_count = 1;
                submit.buffers[0] = {ib.handle, kIbIova, sizeof words};
                NativeAdrenoAbiSubmitReply sr{};
                if (!ucCall(NativeAdrenoMethodSubmit, &submit, &sr)) {
                    NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
                    if (!ucCall(NativeAdrenoMethodWaitFence, &wait, nullptr) && !bm->acquireDeviceWrites(0, 16u << 20))
                        bigWords = (bw[0] == vals[0]) + (bw[last] == vals[1]);
                }
            }
        }
    }
    // Burst: n submissions of the same IB with no wait calls in between.
    constexpr unsigned kJobs = 20;
    unsigned submitted = 0, busy = 0;
    int burstMatch = -1;
    IOReturn retiredWait = kIOReturnNotReady;
    NativeAdrenoAbiBoAllocReply tgt{};
    if (!ucCall(NativeAdrenoMethodBoAlloc, &small, &tgt)) {
        constexpr uint64_t kTgtIova = 0x10020000ull;
        NativeAdrenoAbiBoMap mt{tgt.handle, kTgtIova, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        NativeAdrenoMemory *tm = gClient.memoryOf(tgt.handle), *im = gClient.memoryOf(ib.handle);
        if (tm && im && !ucCall(NativeAdrenoMethodBoMap, &mt, &r)) {
            volatile uint32_t *tw = static_cast<volatile uint32_t *>(tm->mapping());
            volatile uint32_t *iw = static_cast<volatile uint32_t *>(im->mapping());
            tw[0] = 0;
            // the IB (still mapped at kIbIova) now writes 0xb0057000 to the burst target; its content is rewritten once
            uint32_t words[4];
            if (AdrenoPM4Type7(0x3d, 3, &words[0]) == AdrenoPM4Success) {
                words[1] = uint32_t(kTgtIova); words[2] = uint32_t(kTgtIova >> 32); words[3] = 0xb0057000u;
                for (unsigned i = 0; i < 4; ++i) iw[i] = words[i];
                if (!im->publishForDevice(0, 4096, false) && !tm->publishForDevice(0, 4096, true)) {
                    NativeAdrenoAbiSubmit submit{};
                    submit.buffer_count = 1;
                    submit.buffers[0] = {ib.handle, kIbIova, sizeof words};
                    uint64_t fences[kJobs] = {};
                    for (unsigned j = 0; j < kJobs; ++j) {
                        NativeAdrenoAbiSubmitReply sr{};
                        IOReturn st = ucCall(NativeAdrenoMethodSubmit, &submit, &sr);
                        for (unsigned tries = 0; st == kIOReturnBusy && tries < 64; ++tries) {
                            ++busy;
                            // the userspace backend's answer to Busy: wait for the oldest fence it still knows
                            NativeAdrenoAbiWaitFence w{fences[j > 8 ? j - 8 : 0], 200000000ull};
                            ucCall(NativeAdrenoMethodWaitFence, &w, nullptr);
                            st = ucCall(NativeAdrenoMethodSubmit, &submit, &sr);
                        }
                        if (st) break;
                        fences[j] = sr.fence;
                        ++submitted;
                    }
                    if (submitted) {
                        NativeAdrenoAbiWaitFence last{fences[submitted - 1], 200000000ull};
                        if (!ucCall(NativeAdrenoMethodWaitFence, &last, nullptr) && !tm->acquireDeviceWrites(0, 4096))
                            burstMatch = tw[0] == 0xb0057000u;
                        // an early fence whose job the kernel retired long ago must still read as complete
                        NativeAdrenoAbiWaitFence early{fences[0], 0};
                        retiredWait = ucCall(NativeAdrenoMethodWaitFence, &early, nullptr);
                    }
                }
            }
        }
    }
    // Drain and free what the stress made (the caller closes the client afterwards).
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    gSubmit.waitAll(&clock, 200000);
    const uint64_t leftovers[3] = {big.handle, ib.handle, tgt.handle};
    for (const uint64_t h : leftovers) {
        if (!h) continue;
        NativeAdrenoAbiBoFree f{h};
        ucCall(NativeAdrenoMethodBoFree, &f, nullptr);
    }
    snprintf(row, sizeof row, "K a%u f%u L%x s%u v%d J%u y%u m%d r%x t%u", mapped, freed, unsigned(bigStatus) & 0xfff, spans,
             bigWords, submitted, busy, burstMatch, unsigned(retiredWait) & 0xfff, mapMs);
    setRow(7, row);
    // Same reason as MUYU USELFTEST above: J jobs submitted, m whether the CP actually wrote the burst target, r the wait status on the
    // oldest fence. m=0 with J=20 is "the ring accepted work nothing executed".
    IOLog("MUYU UCRING %s\n", row);
}
void userClientTest() {
    uint32_t *u = native_gpu_uc;
    char row[64];
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm ? NativeAdrenoGpuContextVm() : nullptr;
    if (!vm) {setRow(6, "U novm"); IOLog("MUYU USELFTEST U novm\n"); return;}
    IOReturn err = 0;
    int errAt = -1, ok = 0, n = 0;
    const IOReturn attached = gSubmit.attach(vm, &gRing);
    /* Same number the service is published with: NativeAdrenoUserClient::open() rejects anything outside 32..48 too, so the old literal
     * 31 was wrong on both sides. */
    const unsigned ias = NativeAdrenoGpuContextInputBits ? NativeAdrenoGpuContextInputBits() : 40;
    const IOReturn opened = attached ? attached : gClient.open(NativeAdrenoAbiVersion, vm, &gSubmit, &gRing, ias, 40);
    u[0] = uint32_t(attached);
    u[1] = uint32_t(opened);
    if (opened) {snprintf(row, sizeof row, "U open a%x o%x", u[0], u[1]); setRow(6, row); IOLog("MUYU USELFTEST %s\n", row); return;}
    NativeAdrenoAbiParams params{};
    if (!ucCall(NativeAdrenoMethodQuery, nullptr, &params)) ok += params.input_bits == ias && params.output_bits == 40;
    ++n;
    const uint32_t irq0 = gIrqCount;
    // Round 2 reuses the IB address with a new object (a stale TLB entry would run the old IB, which writes to the now
    // unmapped round-1 target and faults) and maps its target at a fresh address, so a stale entry cannot write freed memory.
    const int v1 = ucRound(1, 0x10000000, 0x10001000, 0x5e9a7400u, err, errAt, ok, n);
    const int v2 = ucRound(2, 0x10000000, 0x10002000, 0xa740b0b0u, err, errAt, ok, n);
    u[2] = uint32_t(v1);
    u[3] = uint32_t(v2);
    u[4] = uint32_t(err);
    u[5] = uint32_t(errAt);
    u[6] = gIrqCount - irq0;
    u[7] = gSmmuMap ? *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + 0x48) : 0xdead;
    setRow(6, "U ok.. (stress running)");
    ucStress();
    const IOReturn closed = gClient.close();
    u[8] = uint32_t(closed);
    u[9] = uint32_t(gSubmit.stats().completed);
    // U ok<steps ok>/<steps> e<first error>@<step> v<round1>/<round2 dwords of 4> q<GPU irqs> c<close> s<CB0 FSR> G<GFSR>
    snprintf(row, sizeof row, "U ok%d/%d e%x@%d v%d/%d q%u c%x s%x G%x", ok, n, unsigned(err), errAt, v1, v2, u[6],
             unsigned(closed), gpuFsr(), u[7]);
    setRow(6, row);
    // 2026-11-14 01:4x, fix37: the display chain now accepts and scans out the renderer's 10-bit surface (fix34-36), but every surface
    // arrives zero and backboardd's first frame dies in WaitFence (MUYU GFXFAIL m=6). Whether the GPU executes at all was only ever visible
    // on the boot page: q is the GPU interrupt count during this in-kernel submission round, v1/v2 the dwords the CP wrote back. If q stays
    // 0 and v does not match, no counter or ring change below can matter.
    IOLog("MUYU USELFTEST %s\n", row);
}
NativeAdrenoTimer gTimer;
// Constant facts for the Vulkan driver. fix64: this board is an Adreno 735 (DT qcom,gpu-model,
// qcom,chipid 0x43030b00, gen7-11-0, 1.5 MiB GMEM), NOT the A740 the sheng line was cloned from.
// mesa's freedreno_devices.py has GPUId(chip_id=0x43030B00, name="FD735") with num_ccu=3 and its own
// magic_regs (UCHE_CACHE_WAYS=0, PC_UNKNOWN_9E24=0x01585600); reporting 0x43050a01 made Turnip take the
// FD740 entry (num_ccu=6, UCHE_CACHE_WAYS=0x00040004, PC_UNKNOWN_9E24=0x21585600) and write A740
// register values into an A735 part - fix63's MUYU IB dump shows exactly those A740 words.
// fd_dev_info_raw() compares by chip_id for a table entry whose gpu_id is 0 (GPU-HANDOFF.md §35.2),
// so tu_knl_darwin.cc's leftover gpu_id=740 special case does not break device enumeration.
// UBWC 4.0 / swizzle levels 2+3 / bank bit 16 / macrotile from ubwc_config.c sm8550_data; the VA window
// keeps clear of the kernel buffers and the GMEM window and ends at the 2 GiB aperture.
const NativeAdrenoAbiDeviceInfo gDeviceInfo = {0x43030b00u, 0x180000u, 0x1000000ull, 0x1fffffffff000ull, 0x20000000ull,
                                               0x60000000ull, 16, 6, 1, 4, NativeAdrenoDeviceClientIova, 0};
int timerRead(void *, uint32_t reg, uint32_t *value) {*value = gpuRd(reg); return 0;}
const NativeAdrenoTimeIo gTimerIo{nullptr, timerRead};
IOReturn wcall(IOUserClient *uc, uint32_t method, const void *in, void *out) {
    char mark[64];
    gWCalls = gWCalls + 1;
    snprintf(mark, sizeof mark, "P run m%u n%u", method, gWCalls);
    setRow(5, mark);
    IOExternalMethodArguments a{};
    a.version = kIOExternalMethodArgumentsCurrentVersion;
    a.selector = method;
    const size_t is = NativeAdrenoUserClient::inputSize(method), os = NativeAdrenoUserClient::outputSize(method);
    a.structureInput = is ? in : nullptr;
    a.structureInputSize = uint32_t(is);
    a.structureOutput = os ? out : nullptr;
    a.structureOutputSize = uint32_t(os);
    return uc->externalMethod(method, &a, nullptr, nullptr, nullptr);
}
// Exactly the path a userspace client takes, minus the Mach trap: newUserClient, the external-method table and its lock,
// BoCpuMap -> clientMemoryForType -> a write-combined mapping (the GPU's PTEs are non-cacheable, so no cache maintenance),
// the IB written and the result read through that mapping only, then clientClose (drain, unmap, free) and a second
// connection to prove the slot recycles. Returns the matching dwords (of 4) or -1.
int wrapperRound(IOService *svc, int &ok, int &n, IOReturn &err, int &errAt, uint32_t &memStatus) {
    auto step = [&](IOReturn s) {
        const int idx = n++;
        if (!s) ++ok;
        else if (!err) {err = s; errAt = idx;}
        return !s;
    };
    IOUserClient *uc = nullptr;
    if (!step(svc->newUserClient(kernel_task, nullptr, 0, nullptr, &uc)) || !uc) return -1;
    IOUserClient *second = nullptr;
    step(svc->newUserClient(kernel_task, nullptr, 0, nullptr, &second) == kIOReturnBusy ? 0 : kIOReturnError);
    if (second) second->release();
    NativeAdrenoAbiParams params{};
    step(wcall(uc, NativeAdrenoMethodQuery, nullptr, &params));
    NativeAdrenoAbiDeviceInfo info{};
    step(wcall(uc, NativeAdrenoMethodGetDeviceInfo, nullptr, &info));
    /* fix64: the device identity is the board's A735, not the cloned A740. */
    step(info.chip_id == 0x43030b00u && info.gmem_bytes == 0x180000u ? 0 : kIOReturnError);
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib{}, tgt{};
    int matched = -1;
    IOMemoryDescriptor *desc[2] = {};
    IOMemoryMap *maps[2] = {};
    if (step(wcall(uc, NativeAdrenoMethodBoAlloc, &alloc, &ib)) && step(wcall(uc, NativeAdrenoMethodBoAlloc, &alloc, &tgt))) {
        const uint64_t ibIova = 0x10004000, tgtIova = 0x10005000;
        NativeAdrenoAbiBoMap mapIb{ib.handle, ibIova, 1, 0, 2, 0}, mapTgt{tgt.handle, tgtIova, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        NativeAdrenoAbiBoCpuMap cmIb{ib.handle}, cmTgt{tgt.handle};
        NativeAdrenoAbiBoCpuMapReply crIb{}, crTgt{};
        if (step(wcall(uc, NativeAdrenoMethodBoMap, &mapIb, &r)) && step(wcall(uc, NativeAdrenoMethodBoMap, &mapTgt, &r)) &&
            step(wcall(uc, NativeAdrenoMethodBoCpuMap, &cmIb, &crIb)) && step(wcall(uc, NativeAdrenoMethodBoCpuMap, &cmTgt, &crTgt))) {
            IOOptionBits opt[2] = {};
            const uint32_t tokens[2] = {crIb.token, crTgt.token};
            bool mapped = true;
            for (int i = 0; i < 2; ++i) {
                const IOReturn s = uc->clientMemoryForType(tokens[i], &opt[i], &desc[i]);
                if (!memStatus) memStatus = uint32_t(s);
                if (s || !desc[i]) {mapped = false; continue;}
                maps[i] = desc[i]->createMappingInTask(kernel_task, 0, opt[i]);
                if (!maps[i]) {mapped = false; if (!memStatus) memStatus = 0xbad;}
            }
            // type 0 and a stale token must be refused
            IOMemoryDescriptor *none = nullptr;
            IOOptionBits o = 0;
            step(uc->clientMemoryForType(0, &o, &none) != kIOReturnSuccess && !none ? 0 : kIOReturnError);
            step(mapped ? 0 : kIOReturnNoMemory);
            if (mapped) {
                volatile uint32_t *iw = reinterpret_cast<volatile uint32_t *>(maps[0]->getVirtualAddress());
                volatile uint32_t *tw = reinterpret_cast<volatile uint32_t *>(maps[1]->getVirtualAddress());
                for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
                const uint32_t values[3] = {0x0badc0deu, 0xfeed0001u, 0x600d0003u};
                uint32_t words[12];
                bool built = true;
                for (unsigned i = 0; i < 3; ++i) {
                    built = built && AdrenoPM4Type7(0x3d, 3, &words[i * 4]) == AdrenoPM4Success;
                    words[i * 4 + 1] = uint32_t(tgtIova + i * 4); words[i * 4 + 2] = uint32_t(tgtIova >> 32);
                    words[i * 4 + 3] = values[i];
                }
                for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
                __asm__ volatile("dsb sy" ::: "memory");
                NativeAdrenoAbiSubmit submit{};
                submit.buffer_count = 1;
                submit.buffers[0] = {ib.handle, ibIova, sizeof words};
                NativeAdrenoAbiSubmitReply sr{};
                if (step(built ? 0 : kIOReturnInternalError) && step(wcall(uc, NativeAdrenoMethodSubmit, &submit, &sr))) {
                    NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
                    if (step(wcall(uc, NativeAdrenoMethodWaitFence, &wait, nullptr))) {
                        __asm__ volatile("dsb sy" ::: "memory");
                        matched = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
                    }
                }
                NativeAdrenoAbiTimestamp ts{};
                step(wcall(uc, NativeAdrenoMethodGetTimestamp, nullptr, &ts));
            }
        }
    }
    for (auto *m : maps) if (m) m->release();
    for (auto *d : desc) if (d) d->release();
    // clientClose drains the CP, unmaps and frees everything (including the leases still held), then terminates.
    step(uc->clientClose());
    uc->release();
    return matched;
}
/* qcom_scm_set_gpu_smmu_aperture(0): the secure world opens the CP's aperture to the GPU's context bank 0, which is what makes
 * CP_SMMU_TABLE_UPDATE in the ring able to switch the bank's table (per-process address spaces, NativeAdrenoSubmit table mode 2).
 * Without it NativeAdrenoPublish's tableMode() falls back to 1, the CPU writing TTBR0 with the GPU idle - a path msm keeps only for
 * targets without the aperture, and the one the first client submission hung on (RPTR 0x3f frozen, RBBM_STATUS3 SMMU stalled,
 * no fault latched). Asked first whether the call exists (QCOM_SCM_INFO_IS_CALL_AVAIL).
 * native_adreno_cp_aperture: [0] availability (x1, or 0x80000000|x0 on error), [1] the call's x0. */
extern "C" volatile uint32_t native_adreno_cp_aperture[4];
void openCpAperture() {
    // ARM_SMCCC_CALL_VAL(STD, SMC_64, OWNER_SIP, svc << 8 | cmd): SVC_INFO 0x06 / IS_CALL_AVAIL 0x01, SVC_MP 0x0c / CP_SMMU_APERTURE_ID 0x1b.
    const ScmRes avail = scmCall(0x42000601u, 1, 0x02000000u | (0x0cu << 8) | 0x1bu, 0, 0);
    native_adreno_cp_aperture[0] = avail.a0 ? 0x80000000u | uint32_t(avail.a0) : uint32_t(avail.a1);
    if (avail.a0 || !avail.a1) {native_adreno_cp_aperture[1] = 0xffffffffu; return;}
    // args[0] = 0xffff0000 | QCOM_SCM_CP_APERTURE_REG (0) << 8 | context bank; the other three 0xffffffff. ARGS(4), all values.
    const ScmRes r = scmCall(0x42000c1bu, 4, 0xffff0000u | (0u << 8) | 0u, 0xffffffffu, 0xffffffffu, 0xffffffffu);
    native_adreno_cp_aperture[1] = uint32_t(r.a0);
}
void publishAndWrapperTest(bool rounds) {
    char row[64];
    if (!native_adreno_publish || !native_adreno_published_service || !native_adreno_publish_stats) {setRow(5, "P nolink"); return;}
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm ? NativeAdrenoGpuContextVm() : nullptr;
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    // userClientTest attaches the submit object; with the self-tests skipped it still has to be attached before a client opens.
    if (vm) gSubmit.attach(vm, &gRing);
    /* fix119: hand the submit object the switch, when gpuStart built it. Same wiring as uke's publishAndWrapperTest
     * (UniXNU-uke/package/GpuPowerProbe.cpp:1303-1312): the trigger registers through the GPU window (preemptWrite, AHB-fence
     * checked), the GMU keepalive vote by dword offset, each ring's record and SMMU-info page by GPU address, and the clock the
     * switches are timed with. ring_count_ only becomes 2 here, so this is also the switch that makes priority routing real. */
    if (vm && gPreemptWanted) {
        NativeAdrenoSubmit::PreemptSetup setup{};
        setup.io = NativeAdrenoPreemptIo{nullptr, preemptWrite, ringRead, ringBarrier, nullptr, gmuWrAt};
        setup.pages.record[0] = gGpuBuffers[5].iova; setup.pages.record[1] = gGpuBuffers[6].iova;
        setup.pages.smmu_info[0] = gGpuBuffers[7].iova; setup.pages.smmu_info[1] = gGpuBuffers[8].iova;
        setup.pages.record_cpu[0] = const_cast<uint8_t *>(gGpuCpu[5]); setup.pages.record_cpu[1] = const_cast<uint8_t *>(gGpuCpu[6]);
        setup.pages.smmu_cpu[0] = const_cast<uint8_t *>(gGpuCpu[7]); setup.pages.smmu_cpu[1] = const_cast<uint8_t *>(gGpuCpu[8]);
        setup.now = ringNow; setup.clock = nullptr; setup.mode = gPreemptMode;
        const IOReturn on = gSubmit.setPreemption(&gRing, &gRing1, setup, true);
        IOLog("MUYU PREEMPT submit on=%x mode=%u\n", unsigned(on), gPreemptMode);
    }
    native_adreno_dram_read32 = dramRead32;
    /* fix123.2: the panel surface, mapped once. */
    native_adreno_frame_window = frameWindow;
    /* fix50: install the two missing halves of the coherence protocol. */
    native_adreno_cached_read32 = cachedRead32;
    /* fix103: and the write half, for the canary the submit path plants in the swap surface. */
    native_adreno_dram_write32 = dramWrite32;
    native_adreno_line_audit = lineAudit;
    const IOReturn timer = gTimer.attach(&gTimerIo, NativeAdrenoTimeCp);
    /* The aperture the context bank was really programmed with, not a literal: NativeAdrenoPublish hands every user connection the range
     * [GpuClientVaStart, 2^input_bits), so input_bits=31 (the old literal) left no window and each open was refused (mount_fail 3/x1).
     * output_bits stays 40: it only caps BO allocations, which the self-test already proved at this size. */
    const unsigned ias = NativeAdrenoGpuContextInputBits ? NativeAdrenoGpuContextInputBits() : 0;
    muyu_gpu_bits[0] = ias;
    muyu_gpu_bits[1] = 40;
    if (!ias) {setRow(5, "P no ias"); return;}
    /* Before publish: tableMode() reads cp_aperture at that moment, and only an answered aperture call selects the CP-side switch. */
    openCpAperture();
    const IOReturn published = native_adreno_publish(vm, &gSubmit, &gRing, &clock, timer ? nullptr : &gTimer, &gDeviceInfo, ias, 40);
    IOService *svc = published ? nullptr : native_adreno_published_service();
    int ok = 0, n = 0, errAt = -1, v1 = -1, v2 = -1;
    IOReturn err = 0;
    uint32_t memStatus = 0;
    if (svc && rounds) {
        v1 = wrapperRound(svc, ok, n, err, errAt, memStatus);
        v2 = wrapperRound(svc, ok, n, err, errAt, memStatus);   // the slot must recycle; the IOVAs were unmapped by the close
    }
    uint32_t closed = 0;
    const uint32_t opened = native_adreno_publish_stats(&closed);
    // P s<publish IOReturn> w<steps ok>/<steps> e<first error>@<step> v<r1>/<r2 dwords of 4> m<clientMemoryForType> o<opened>/<closed>
    snprintf(row, sizeof row, "P s%x w%d/%d e%x@%d v%d/%d m%x o%u/%u s%x", unsigned(published), ok, n, unsigned(err), errAt, v1, v2,
             memStatus, opened, closed, gpuFsr());
    setRow(5, rounds ? row : "P published (rounds skipped)");
}
// sheng-gpu-metal=1 publishes the Adreno accelerator for the original Metal discovery (the plugin bundle itself arrives through the
// boot-time addon mount, sheng-gpu-addon=1, and the trust cache). sheng-gpu-selftest=1 runs the kernel-driven user-client tests that
// int-024..031 already qualified, which frees the diagnostic rows for the plugin's own result.
unsigned bootArg(const char *name, unsigned fallback) {
    unsigned value = fallback;
    PE_parse_boot_argn(name, &value, sizeof value);
    return value;
}
volatile uint32_t gMetalStage = 0, gMetalRegistered = 0;
volatile int gMetalKext = -99, gMetalStatus = 0;
// Stages: 1 kext load attempted, 2 parent metaclass found, 3 own class registered, 4 object created and initialised, 5 published.
void publishMetal() {
    gMetalStage = 1;
    if (!native_adreno_accelerator_static_init || !native_adreno_accelerator_allocate || !native_adreno_accelerator_instance_size) {
        gMetalStatus = kIOReturnUnsupported;
        return;
    }
    gMetalKext = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (gMetalKext) {gMetalStatus = gMetalKext; return;}
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) {gMetalStatus = kIOReturnNotFound; return;}
    gMetalStage = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {gMetalStatus = kIOReturnNoResources; return;}
    native_adreno_accelerator_static_init();
    gMetalStatus = OSMetaClass::postModLoad(handle);
    if (gMetalStatus) return;
    if (native_adreno_accelerator_instance_size() != 136) {gMetalStatus = kIOReturnInvalid; return;}
    gMetalStage = 3;
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator) {gMetalStatus = kIOReturnNoMemory; return;}
    if (!accelerator->init()) {accelerator->release(); gMetalStatus = kIOReturnError; return;}
    // The two keys the original _MTLRegisterDevices helper reads. The plugin name is ours: the bundle exists only through the
    // addon mount, and no Apple device identity (gpu,t8011 and the like) is claimed.
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("NativeSandboxGrantProcess", "backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    gMetalStage = 4;
    if (!accelerator->attach(IOService::getPlatform())) {accelerator->release(); gMetalStatus = kIOReturnNotAttached; return;}
    accelerator->registerService();
    gMetalRegistered = 1;
    gMetalStage = 5;
}
// Rows refreshed by the heartbeat thread: 6 = the stage chain, 7 = who reached the driver, 5 = the plugin's own report.
// M a<addon stage>.<status> k<kext load> s<publish stage>/<status> r<registered>
// D o<opened>/<busy>/<closed> p<property reads> <latest reader>><latest opener>   R<plugin report>
// Is the launchd job's process alive? proc_iterate over every process, looking for the daemon by name (12 characters, "adrenoprobed").
volatile uint32_t gProcTotal, gDaemonSeen, gDaemonAlive, gDaemonPid;
int scanProcess(void *proc, void *) {
    gProcTotal = gProcTotal + 1;
    const char *name = proc_name_address(proc);
    if (name && !strncmp(name, "adrenoprobed", 12)) {gDaemonAlive = 1; gDaemonSeen = 1; gDaemonPid = gDaemonPid + 1;}   // pid not read: the header's proc_pid prototype differs
    return 0;   // PROC_RETURNED: keep iterating
}
void metalRows() {
    if (!gMetalStage && !native_gpu_addon_stage) return;
    char row[64];
    const NativeAdrenoMetalDiagView &d = native_adreno_metal_diag;
    gProcTotal = 0;
    gDaemonAlive = 0;
    proc_iterate(1 /* PROC_ALLPROCLIST */, scanProcess, nullptr, nullptr, nullptr);
    // Row 6: both overlay mounts (stage.status; 5 = mounted and our own file visible), IOGPUFamily load, publication stage, plugin opens
    // granted/refused/closed, MetalPluginName reads, and the daemon process: d<ever seen><alive now>:<pid> of <process count>.
    snprintf(row, sizeof row, "M a%d.%x/%d.%x k%x s%u/%x r%u o%u/%u/%u n%u d%u%u:%u/%u", native_gpu_addon_stage,
             unsigned(native_gpu_addon_status), native_gpu_addon2_stage, unsigned(native_gpu_addon2_status), unsigned(gMetalKext),
             gMetalStage, unsigned(gMetalStatus), gMetalRegistered, d.opens, d.busy, d.closes, d.name_reads, gDaemonSeen, gDaemonAlive,
             gDaemonPid, gProcTotal);
    setRow(6, row);
    // 2026-11-13 23:1x: the row above only ever reached boot_progress_ext, so a boot that reset at ~197 s said nothing about why no GPU
    // client arrived. Mirror it into msgbuf (rate limited: the heartbeat calls this repeatedly) so the persistent window carries it.
    {
        static unsigned logged = 0;
        if (++logged <= 3 || (logged % 32) == 0) IOLog("MUYU METAL %s\n", row);
    }
    // 2026-11-14 00:5x, history check: the same driver on the physical 17-line boot printed "o3/0/0 n2" (three plugin opens, two
    // MetalPluginName reads, GPU-HANDOFF §30). Every round of this harness since fix15 says "o0/0/0 n0" while backboardd does open the
    // accelerator with type 1. The M row cannot separate "nobody queried the node" from "a connection was granted and then asked for
    // methods", so print both here: p<n property reads of any key> n<MetalPluginName reads> <newest reader>, plus the last selector that
    // reached the driver with its declared sizes and the selector bitmap. Logged only when a counter moves, and capped: a client that
    // polls in a loop must not own the window.
    {
        static uint32_t seen_entries = 0, seen_reads = 0;
        static unsigned logged = 0;
        if (logged < 24 && (d.ext_entries != seen_entries || d.property_reads != seen_reads)) {
            ++logged;
            seen_entries = d.ext_entries; seen_reads = d.property_reads;
            IOLog("MUYU ABI x%u p%u n%u %s s%u/%x in%u out%u b%08x%08x%08x%08x\n",
                d.ext_entries, d.property_reads, d.name_reads, d.reader[0],
                d.ext_selector, d.ext_status, d.ext_in, d.ext_out,
                d.sel_bits[0], d.sel_bits[1], d.sel_bits[2], d.sel_bits[3]);
        }
    }
    // Row 7: the plugin's progress markers (open types 0x4147xxxx), oldest first: 1 started, 2 symbols, 3 opened, 1xxx probe
    // (xxx admitted selectors), 2000/2001 mailbox failed/ok, 3xxx step xxx done, 4xxx step failed (xxx = status), 5xxx reported;
    // after 4F01 the two halves of the failed open's kern_return_t.
    char tail[48] = {};
    const unsigned total = d.marks_total, shown = total < 7 ? total : 7;
    size_t at = 0;
    for (unsigned i = 0; i < shown && at < sizeof tail - 6; ++i)
        at += snprintf(tail + at, sizeof tail - at, " %x", d.marks[(total - shown + i) & 15]);
    snprintf(row, sizeof row, "K%u%s", total, tail);
    setRow(7, row);
    // GPU7/GPU8 (wide): the raw test's report line and the Turnip phase's line (T result or W watchdog sample).
    if (d.report[0]) snprintf(boot_progress_ext_wide[0], sizeof boot_progress_ext_wide[0], "R %.63s", d.report);
    /* The bridge's own last line, for the console dump: when backboardd's plugin gives up at native_bridge_metal_open it streams
     * "PUB no device: <nmb_last_error()>" into report2, and nothing else on screen carries that string. */
    if (d.report2[0]) {memcpy(muyu_gpu_reason, d.report2, sizeof muyu_gpu_reason - 1); muyu_gpu_reason[sizeof muyu_gpu_reason - 1] = 0;}
    if (d.report2[0]) snprintf(boot_progress_ext_wide[1], sizeof boot_progress_ext_wide[1], "%.63s", d.report2);
    {
        // Row 5: the newest four IOServiceOpen attempts as <process><P plugin type|F other type><MACF result of the sandbox>; 0 = admitted.
        char list[48] = {};
        const unsigned count = native_adreno_open_log_count, show = count < 1 ? count : 1;   // one, so the call counters fit the 47 columns
        size_t used = 0;
        for (unsigned i = 0; i < show && used < sizeof list - 12; ++i) {
            const NativeAdrenoOpenRecordView &r = native_adreno_open_log[(count - 1 - i) & 7];
            used += snprintf(list + used, sizeof list - used, " %.6s%c%x", r.name, r.foreign ? 'F' : 'P', r.macf);
        }
        // ... a<attach> x<selectors that reached the driver> c<calls done> f<method running now + 1, 0 = none> l<last method>/<its status>
        // ... c<calls done> f<method running now + 1> l<last method>/<status> g<stage reached in the free/unmap path: 1-4 freeBo, 10-14 VM unmap, 30-34 release>
        snprintf(row, sizeof row, "S%s x%u c%u f%u l%u/%x g%u u%u", list, d.ext_entries, d.calls, d.inflight, d.last_selector,
                 d.last_status & 0xfff, native_adreno_trace_stage, d.forced_unmaps);
        setRow(5, row);
    }
}
// Exec observer: every successful exec is announced to kauth fileop listeners after the image was activated (code signature accepted).
// This shows whether launchd ever started the owner job /private/var/ngpu/bin/adrenoprobed, and the last few programs it started.
extern "C" void *kauth_listen_scope(const char *identifier,
    int (*callback)(void *cred, void *idata, unsigned action, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3), void *idata);
extern "C" const char *vnode_getname(void *vp);
extern "C" void vnode_putname(const char *name);
volatile uint32_t gExecTotal, gExecDaemon;
char gExecNames[6][9];
volatile void *gExecListener;
int execObserver(void *, void *, unsigned action, uintptr_t vnode, uintptr_t, uintptr_t, uintptr_t) {
    if (action != 6 /* KAUTH_FILEOP_EXEC */ || !vnode) return 3;   // KAUTH_RESULT_DEFER
    const char *name = vnode_getname(reinterpret_cast<void *>(vnode));
    if (name) {
        if (!strncmp(name, "adrenoprobed", 12)) gExecDaemon = gExecDaemon + 1;
        const uint32_t slot = gExecTotal % 6;
        strlcpy(gExecNames[slot], name, sizeof gExecNames[slot]);
        vnode_putname(name);
    }
    gExecTotal = gExecTotal + 1;
    return 3;
}
// Vnode authorization observer, from the first moment of the probe thread: names the files the boot chain touches before any process can report.
// e: execute authorizations of adrenoprobed (an exec attempt, before the code signature check), p: any access to our job plist / the shadowed bluetoothd /
// iomfb_fdr_loader plists (did launchd read them), b: accesses to the plugin binary NativeAdrenoMetal / the original AGXMetalA10 binary, with the
// name of the last process that touched the plugin binary.
extern "C" int proc_selfname(char *buf, int size);
volatile uint32_t gVnE, gVnAny, gVnPlist[3], gVnPlugin, gVnOriginal, gVnBundle;
char gVnPluginProc[17], gVnPlistProc[17];
volatile void *gVnodeListener;
int vnodeObserver(void *, void *, unsigned action, uintptr_t, uintptr_t vnode, uintptr_t, uintptr_t) {
    if (!vnode) return 3;
    const char *name = vnode_getname(reinterpret_cast<void *>(vnode));
    if (!name) return 3;
    switch (name[0]) {
    case 'a':
        if (!strcmp(name, "adrenoprobed")) {
            gVnAny = gVnAny + 1;
            if (action & 4 /* KAUTH_VNODE_EXECUTE */) gVnE = gVnE + 1;
        } else if (!strcmp(name, "AGXMetalA10")) gVnOriginal = gVnOriginal + 1;
        break;
    case 'A':
        if (!strcmp(name, "AGXMetalA10")) gVnOriginal = gVnOriginal + 1;
        break;
    case 'o':
        if (!strcmp(name, "org.blu-sharky.adrenoprobed.plist")) {gVnPlist[0] = gVnPlist[0] + 1; proc_selfname(gVnPlistProc, sizeof gVnPlistProc - 1);}
        break;
    case 'c':
        if (!strcmp(name, "com.apple.bluetoothd.plist")) {gVnPlist[1] = gVnPlist[1] + 1; proc_selfname(gVnPlistProc, sizeof gVnPlistProc - 1);}
        else if (!strcmp(name, "com.apple.iomfb_fdr_loader.plist")) {gVnPlist[2] = gVnPlist[2] + 1; proc_selfname(gVnPlistProc, sizeof gVnPlistProc - 1);}
        break;
    case 'N':
        if (!strcmp(name, "NativeAdrenoMetal")) {
            gVnPlugin = gVnPlugin + 1;
            proc_selfname(gVnPluginProc, sizeof gVnPluginProc - 1);
        } else if (!strcmp(name, "NativeAdrenoMetal.bundle")) gVnBundle = gVnBundle + 1;
        break;
    default:
        break;
    }
    vnode_putname(name);
    return 3;
}
void registerExecObserver() {
    if (!gExecListener) gExecListener = kauth_listen_scope("com.apple.kauth.fileop", execObserver, nullptr);
    if (!gVnodeListener) gVnodeListener = kauth_listen_scope("com.apple.kauth.vnode", vnodeObserver, nullptr);
}
// Does the interrupt controller deliver SGI 0 between cores? The kernel sends IPIs through ArmCpu::signalCPU, but every boot page has shown
// ipi=0 received, and the restartable-ranges panic (a thread waiting for an IPI acknowledgement) says delivery may be broken. This sends
// one SGI from this core to each other core with the same ICC_SGI1R_EL1 encoding signalCPU uses and counts what arrives. Row 2:
// IPI k<sent by the kernel> t<self-test sent> r<received in total> m<cores that answered, bit = core index>
extern "C" unsigned int processor_avail_count;
volatile unsigned gIpiRuns;
// Self-SGI probe (system registers only): row 2 ends with s<cores that ran a probe> p<INTIDs 0..15 that became pending as Group 1>/<as Group 0>,
// accumulated over every IPI test run. Row 3 is the boot core's GIC readback after enabling: g<IGROUPR0> e<ISENABLER0> c<GICD_CTLR> t<GICD_TYPER>
// m<IGRPMODR0> and the ICC registers of the last probing core P<PMR> I<IGRPEN1> S<SRE> C<CTLR> R<RPR>.
unsigned gSelfTried, gSelfPend1, gSelfPend0;
uint64_t gSelfLast[10];
void selfProbe() {
    if (!armcpu_sgi_selfprobe) return;
    for (unsigned i = 0; i < 8; ++i) {
        uint64_t out[10] = {};
        armcpu_sgi_selfprobe(out);
        gSelfTried |= 1u << unsigned((out[0] >> 8) & 7);
        gSelfPend1 |= unsigned(out[1]); gSelfPend0 |= unsigned(out[2]);
        for (unsigned k = 0; k < 10; ++k) gSelfLast[k] = out[k];
        IOSleep(25);
    }
}
void gicRow() {
    char row[64];
    // L e<execute authorizations>/<any access of adrenoprobed> p<our plist>/<bluetoothd plist>/<fdr plist reads> b<plugin binary>/<original AGXMetalA10> B<bundle dir> <last plugin toucher>
    snprintf(row, sizeof row, "L e%u/%u p%u/%u/%u %.6s b%u/%u B%u %.6s", gVnE, gVnAny, gVnPlist[0], gVnPlist[1], gVnPlist[2], gVnPlistProc,
             gVnPlugin, gVnOriginal, gVnBundle, gVnPluginProc);
    setRow(3, row);
}
void ipiSelfTest() {
    if (!armcpu_ipi_selftest || !&armcpu_ipi_interrupts) {setRow(2, "IPI nolink"); return;}
    unsigned mask = 0;
    for (unsigned cpu = 0; cpu < 8; ++cpu) {
        const uint64_t received = armcpu_ipi_interrupts;
        armcpu_ipi_selftest(uint64_t(cpu) << 8);
        for (unsigned wait = 0; wait < 20 && armcpu_ipi_interrupts == received; ++wait) IOSleep(1);
        if (armcpu_ipi_interrupts != received) mask |= 1u << cpu;
    }
    char row[64];
    gIpiRuns = gIpiRuns + 1;
    // Row 2: IPI r<received> m<cores that answered> #<round> X<execs> a<adrenoprobed execs> s<tried> p<pending G1>/<pending G0>
    snprintf(row, sizeof row, "IPI k%llu r%llu m%x #%u X%u a%u", (unsigned long long)armcpu_ipi_sent, (unsigned long long)armcpu_ipi_interrupts, mask,
             gIpiRuns, gExecTotal, gExecDaemon);
    setRow(2, row);
    selfProbe();
    snprintf(row, sizeof row, "IPI k%llu r%llu m%x #%u X%u a%u p%x/%x", (unsigned long long)armcpu_ipi_sent, (unsigned long long)armcpu_ipi_interrupts,
             mask, gIpiRuns, gExecTotal, gExecDaemon, gSelfPend1, gSelfPend0);
    setRow(2, row);
    gicRow();
}
void startHeartbeatThread();

/* stage3 pure observation: which hwcg value table and GMEM window hw_init actually wrote.
 * Declared after the includes so uint32_t is visible under -nostdlibinc. */
extern "C" {volatile uint32_t native_adreno_hwinit_probe[8];}

/* §38 (2026-11-12): hw_init extracted from gpuStart's step 3, verbatim, so the hang restart can replay it
 * the way msm's a6xx_recover calls msm_gpu_hw_init (and the way UniXNU-uke/package/GpuPowerProbe.cpp:1560
 * does). Only the step counter c[0]=3 stays in gpuStart. */
void gpuHwInit() {
    gpuWr(A740_GBIF_HALT, 0); (void)gpuRd(A740_GBIF_HALT);
    gpuWr(A740_RBBM_GBIF_HALT, 0); (void)gpuRd(A740_RBBM_GBIF_HALT);
    gpuWr(A740_RBBM_SECVID_TSB_CNTL, 0);
    gpuWr64(A740_RBBM_SECVID_TSB_TRUSTED_BASE, 0);
    gpuWr(A740_RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
    gmuWr(A740_GPU_GMU_AO_GMU_CGC_MODE_CNTL, 0x00020202);
    gmuWr(A740_GPU_GMU_AO_GMU_CGC_DELAY_CNTL, 0x10111);
    gmuWr(A740_GPU_GMU_AO_GMU_CGC_HYST_CNTL, 0x5555);
    if (gpuRd(A740_RBBM_CLOCK_CNTL) != 0x8aa8aa82) {
        for (unsigned i = 0; i < NATIVE_ADRENO_A740_HWCG_COUNT; ++i) gpuWr(NativeAdrenoA740Hwcg[i][0], NativeAdrenoA740Hwcg[i][1]);
        gpuWr(A740_RBBM_CLOCK_CNTL, 0x8aa8aa82);
        native_adreno_hwinit_probe[0]=1;   /* this pass applied the GEN2 family table */
    } else native_adreno_hwinit_probe[0]=2; /* clock already matched, table skipped */
    native_adreno_hwinit_probe[2]=NATIVE_ADRENO_A740_HWCG_COUNT;
    native_adreno_hwinit_probe[3]=1;       /* 1 = GEN2 a740_hwcg; 2 was the GEN1 a730_hwcg mislabelled A735 */
    native_adreno_hwinit_probe[4]=native_adreno_hwinit_probe[4]+1;
    gpuWr(A740_RBBM_GBIF_CLIENT_QOS_CNTL, 0x2120212);
    gpuWr(A740_UCHE_GBIF_GX_CONFIG, 0x10240e0);
    gpuWr(A740_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
    gpuWr64(A740_UCHE_TRAP_BASE, 0x1fffffffff000ull);
    gpuWr64(A740_UCHE_WRITE_THRU_BASE, 0x1fffffffff000ull);
    gpuWr64(A740_UCHE_GMEM_RANGE_MIN, 0x1000000);
    /* fix6 (2026-11-13 20:00): back to the 3 MiB window that stage3 used.
     * Evidence is on-panel and single-variable: stage3 (gr=12fffff, 3 MiB) gave
     * calls=2 then calls=1 with the same GEN2 a740_hwcg table (hn=54, hw=1);
     * fix2/fix4/fix5 (gr=117ffff, the 1.5 MiB window from board.toml) gave
     * calls=0 on every boot. fix4 kept the 10 ms OOB budget and still lost the
     * offer, so the budget is not the variable - this window is.
     * board.toml gmem_bytes stays untouched; it is a description of the board,
     * not the CP's view of the aperture, and it was never moved before fix2. */
    gpuWr64(A740_UCHE_GMEM_RANGE_MAX, 0x1000000 + 0x300000 - 1);
    native_adreno_hwinit_probe[1]=0x1000000u + 0x300000u - 1u;
    native_adreno_hwinit_probe[5]=0x1000000u;
    gpuWr(A740_UCHE_CACHE_WAYS, 1u << 23);
    gpuWr(A740_CP_AHB_CNTL, 0x1);
    gpuWr(A740_RBBM_PERFCTR_CNTL, 0x1);
    gpuWr(A740_RBBM_INTERFACE_HANG_INT_CNTL, (1u << 30) | 0xcfffff);
    gpuWr(A740_UCHE_CLIENT_PF, (1u << 7) | 0x1);
    gpuWr(A740_CP_PROTECT_CNTL, (1u << 0) | (1u << 1) | (1u << 3));
    for (unsigned i = 0; i < NATIVE_ADRENO_A740_PROTECT_COUNT; ++i)
        gpuWr(A740_CP_PROTECT + NativeAdrenoA740Protect[i][0], NativeAdrenoA740Protect[i][1]);
    gpuWr(A740_UCHE_CMDQ_CONFIG, (6u << 16) | (6u << 12) | (9u << 8) | (1u << 3) | (1u << 2) | 2u);
    gpuWr(A740_CP_APRIV_CNTL, 0x6f);
    gpuWr(A740_CP_BV_APRIV_CNTL, 0x0f);
    gpuWr(A740_CP_LPAC_APRIV_CNTL, 0x0f);
    gpuWr(A740_RB_CMP_DBG_ECO_CNTL, gpuRd(A740_RB_CMP_DBG_ECO_CNTL) | (1u << 11));
    gpuWr(A740_RB_NC_MODE_CNTL, (1u << 11) | (1u << 4) | (3u << 1));
    gpuWr(A740_TPL1_NC_MODE_CNTL, 3u << 1);
    gpuWr(A740_SP_NC_MODE_CNTL, (2u << 4) | (3u << 1));
    for (uint32_t pipe = 1; pipe <= 2; ++pipe) {
        gpuWr(A740_CP_APERTURE_CNTL_HOST, pipe << 12);
        gpuWr(A740_GRAS_NC_MODE_CNTL, 3u << 5);
    }
    gpuWr(A740_CP_APERTURE_CNTL_HOST, 0);
    gpuWr(A740_UCHE_MODE_CNTL, 3u << 21);
    gpuWr(A740_RBBM_NC_MODE_CNTL, 1);
    gpuWr(A740_RBBM_INT_0_MASK, 0x33d283c2);
    gpuWr64(A740_CP_SQE_INSTR_BASE, gGpuBuffers[0].iova);
}
extern "C" void native_adreno_set_restart(NativeAdrenoRestart, void *) __attribute__((weak));

/* §38: the 17-line port never registered a restart hook, so NativeAdrenoPublish's hangcheck counted 32 hangs
 * and zero restarts while the ring stayed frozen (H h32 r0 r0). Port of uke's gpuRestart: drain GBIF including
 * the GX side, assert RBBM_SW_RESET_CMD for >100 us, replay hw_init, restart the ring at the completed fence,
 * re-run CP_ME_INIT through the ring and leave secure mode again if the zap shader was authenticated. The GMU
 * keeps its OOB GPU_SET vote; a full GMU power cycle is not available on this board. */
IOReturn gpuRestart(void *, NativeAdrenoRing *ring, uint32_t completed) {
    uint32_t *r = native_gpu_restart;
    uint32_t *d = native_gpu_restart2;
    r[0] = r[0] + 1;
    auto fail = [&](uint32_t step, IOReturn status) {r[1] = step; r[2] = uint32_t(status); return status ? status : kIOReturnIOError;};
    /* §43: read-only snapshots, printed as MUYU RT2. §40 proved restart() succeeds (RT n1 s0 e0) while the fence
     * stays at 4; these four RPTR samples decide whether the CP never moved or moved and stalled again. */
    d[0] = uint32_t(native_gpu_zap[0]);
    d[1] = zapLimit();
    d[2] = gpuRd(A740_CP_RB_RPTR);
    d[3] = gpuRd(A740_CP_RB_WPTR);
    if (!ring) return fail(1, kIOReturnBadArgument);
    if (gxOn()) gpuWr(A740_CP_SQE_CNTL, 3);
    IOReturn status = ring->busHalt(true, 1000000);
    if (status) return fail(2, status);
    gpuWr(A740_RBBM_SW_RESET_CMD, 1); (void)gpuRd(A740_RBBM_SW_RESET_CMD); IODelay(1);
    IODelay(100);
    gpuWr(A740_RBBM_SW_RESET_CMD, 0); (void)gpuRd(A740_RBBM_SW_RESET_CMD); IODelay(1);
    r[3] = gpuRd(A740_RBBM_STATUS);
    gpuHwInit();
    status = ring->restart(completed);
    d[4] = gpuRd(A740_CP_RB_RPTR);
    if (status) return fail(3, status);
    gpuWr64(A740_CP_BV_RB_RPTR_ADDR, gGpuBuffers[2].iova + 8);
    gpuWr(A740_CP_SQE_CNTL, 1);
    const NativeAdrenoRingIo rio{nullptr, ringWrite, ringRead, ringBarrier, nullptr, nullptr};
    const NativeAdrenoCpInitConfig config{gGpuBuffers[0].iova, gGpuBuffers[3].iova, 1};
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    NativeAdrenoCpInit init;
    uint32_t fence = 0;
    status = init.run(&rio, ring, &config, &fence, false);
    d[5] = gpuRd(A740_CP_RB_RPTR);
    if (status) return fail(4, status);
    status = ring->idle(&clock, 1000000);
    if (status) return fail(5, status);
    if (native_gpu_zap[0] == 9) {
        if (gSecK) {
            /* fix150: replay the same msm -ENODEV operation, never the other carrier's 0x66. */
            gpuWr(0xf400, 0x0);
            __asm__ volatile("dsb sy" ::: "memory");
        } else {
            uint32_t words[2] = {};
            if (AdrenoPM4Type7(0x66, 1, &words[0]) != AdrenoPM4Success) return fail(6, kIOReturnInternalError);
            status = ring->submitUnfenced(words, 2);
            if (!status) status = ring->idle(&clock, 1000000);
            if (status) return fail(6, status);
        }
    }
    r[1] = 0; r[2] = 0;
    uint32_t fenceValue = 0;
    (void)ring->fenceValue(&fenceValue);
    d[6] = fenceValue;
    d[7] = gpuRd(A740_CP_RB_RPTR);
    d[8] = gpuRd(A740_CP_RB_WPTR);
    d[9] = gSmmuMap ? *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + 0x48) : 0xdeadbeef;
    return kIOReturnSuccess;
}

void gpuStart() {
    char row[64];
    uint32_t *c = native_gpu_cp;
    if (!NativeAdrenoGpuContextBringup || !NativeAdrenoGpuContextBuffers || !gSmmuMap) {setRow(6, "Q nolink"); return;}
    // 1. The GPU's own context bank and buffers (arm-smmu-qcom: the GPU always gets bank 0).
    // This board's DTS (Mu-Silicium/Resources/DTBs/muyu.dts qcom,kgsl-iommu@3da0000)
    // names gfx3d_user SID 0, gfx3d_secure SID 2, and qcom,gmu@3d69000 SID 5 - NOT
    // uke's "SIDs 0 and 1". Only SID 0 (slot 0 -> bank 0) and SID 5 (slot 2 -> bank 1)
    // are routed; SID 2 is unmatched, but that is NOT a lease we may take back:
    /* fix141 (2026-11-15) HARD HANG, do not retry: leasing a SECOND SMR/S2CR route
     * - even the DTS-correct SID 2 into an empty slot 3 - wedges the SMMU during
     * ArmSmmu4KAttachStreams exactly as the 2026-11-11 slot-1/SID-1 attempt did.
     * 420 s with no watchdog restart, USB vanished, neither adb nor fastboot
     * enumerates, `adb reboot` refuses: only a physical 10-15 s power hold recovers.
     * So the hang is caused by writing a second route at all, not by the SID or slot.
     * The "GX traffic is an unmatched SID 2" hypothesis is therefore UNTESTABLE by
     * adding a route, and §69.99.1 ("do not add SID 1") is upgraded to: never add a
     * second board route on this firmware. Keep exactly one route. */
    static const unsigned kSecondSid = UINT16_MAX;
    NativeAdrenoGmuContextReport &q = native_gpu_context;
    const IOReturn ctx = NativeAdrenoGpuContextBringup(reinterpret_cast<volatile uint8_t *>(gSmmuMap->getVirtualAddress()),
        kSmmuWindow, 0, 2, 0, 0, 1, kSecondSid, gpuCtxProgress, &q);
    // Q s<step> st<IOReturn> k<program>/<attach kit status> p<phase>e<enabled> r<route phase>v<verified> n<spans>
    snprintf(row, sizeof row, "Q s%u st%x k%u/%u p%ue%u r%uv%u n%u", q.step, unsigned(ctx), q.program_status, q.route_status,
             q.program_phase, q.program_enabled, q.route_phase, q.routes_verified, q.spans);
    setRow(6, row);
    if (ctx) return;
    gpuSetActlr();
    c[0] = 1;
    if (NativeAdrenoGpuContextBuffers(gGpuBuffers)) {setRow(7, "C1 buffers refused"); IOLog("MUYU CPROW C1 buffers refused\n"); return;}
    for (unsigned i = 0; i < GpuBufferCount; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(gGpuBuffers[i].physical, gGpuBuffers[i].bytes);
        if (m) {
            gGpuMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gGpuMaps[i]) {setRow(7, "C1 nomap buffer"); IOLog("MUYU CPROW C1 nomap buffer\n"); return;}
        gGpuCpu[i] = reinterpret_cast<volatile uint8_t *>(gGpuMaps[i]->getVirtualAddress());
    }
    // 2. SQE: adreno_fw_create_bo copies the firmware without its first dword.
    // 2026-11-11: gpuStart stopped here with cp=2 and the SQE stage never wrote the buffer. The cause is the
    // same one §13 found for the GMU image: native_adreno_sqe_firmware_image_size only has the weak default
    // from NativeAdrenoFirmwareDefaults.cpp, a zero length, and "size < 8" returns "C2 no sqe image" before a
    // single register write. The board's real a740 SQE microcode is now generated into muyu_sqe_firmware
    // (gen-muyu-gmu-source.py --sqe-firmware), so take that first and keep the driver symbol as the fallback.
    c[0] = 2;
    const uint8_t *sqe = muyu_sqe_firmware_size ? reinterpret_cast<const uint8_t *>(muyu_sqe_firmware)
                                                : (native_adreno_sqe_firmware_image_size ? native_adreno_sqe_firmware_image : nullptr);
    const uint64_t sqe_bytes = muyu_sqe_firmware_size ? (uint64_t)muyu_sqe_firmware_size
                             : (native_adreno_sqe_firmware_image_size ? native_adreno_sqe_firmware_image_size : 0);
    if (!sqe || sqe_bytes < 8 || sqe_bytes - 4 > gGpuBuffers[0].bytes) {
        snprintf(row, sizeof row, "C2 no sqe m%x n%llx b%x", muyu_sqe_firmware_size ? 1 : 0,
                 (unsigned long long)sqe_bytes, (unsigned)gGpuBuffers[0].bytes);
        setRow(7, row);
        return;
    }
    for (uint64_t i = 4; i < sqe_bytes; ++i) gGpuCpu[0][i - 4] = sqe[i];
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint64_t i = 4; i < sqe_bytes; ++i) if (gGpuCpu[0][i - 4] != sqe[i]) {c[1] = uint32_t(i); break;}
    if (c[1]) {snprintf(row, sizeof row, "C2 sqe readback differs at %x", c[1]); setRow(7, row); IOLog("MUYU CPROW %s\n", row); return;}
    // 3. hw_init (a6xx_gpu.c), A740 branches only. OOB GPU_SET is already held (mode 8).
    c[0] = 3;
    gpuHwInit();
    // 4. Ring 0 (MSM_GPU_RB_CNTL_DEFAULT: 32 KiB, block 32; hw_apriv uses the hardware rptr shadow at memptrs+0).
    c[0] = 4;
    // The framework RingIo grew fence_cookie / fence_status (GMU AHB fence checking). Both are
    // optional; leave them null so WPTR writes stay unchecked, exactly as the older probe behaved.
    const NativeAdrenoRingIo rio{nullptr, ringWrite, ringRead, ringBarrier, nullptr, nullptr};
    const NativeAdrenoRingCache cache{nullptr, ringPublish, ringAcquire};
    const IOReturn attached = gRing.attach(&rio, &cache, const_cast<uint8_t *>(gGpuCpu[1]), gGpuBuffers[1].bytes,
        gGpuBuffers[1].iova, const_cast<uint8_t *>(gGpuCpu[2]), gGpuBuffers[2].bytes, gGpuBuffers[2].iova);
    if (attached) {snprintf(row, sizeof row, "C4 ring attach %x", unsigned(attached)); setRow(7, row); IOLog("MUYU CPROW %s\n", row); return;}
    /* fix62: attach() zeroes the whole memptrs BO through its write-combined CPU
     * mapping, but the CP reads the BV rptr shadow (iova + 8, dword 2) from DRAM
     * and ringPublish()/ringAcquire() only issue dsb sy. fix56 proved that WC
     * pages need an explicit clean+invalidate before the device sees their zeros.
     * BV has stayed in its idle loop at PC 0x132 across fix57..fix61; if that is a
     * stale BV rptr, this is where it is fixed. Log both views, then push the first
     * page to the point of coherency. */
    {
        const volatile uint32_t *memptrs = reinterpret_cast<const volatile uint32_t *>(gGpuCpu[2]);
        const uint64_t pa = gGpuBuffers[2].physical;
        uint32_t before = 0xdeadbeefu, after = 0xdeadbeefu;
        (void)dramRead32(pa + 8, &before);
        IODeviceMemory *cm = IODeviceMemory::withRange(pa & ~uint64_t(0xfff), 0x1000);
        if (cm) {
            IOMemoryMap *map = cm->map(kIOMapAnywhere);
            cm->release();
            if (map) {
                const uintptr_t cv = uintptr_t(map->getVirtualAddress());
                for (size_t at = 0; at < 0x1000; at += 64)
                    __asm__ volatile("dc civac, %0" :: "r"((cv + at) & ~uintptr_t(63)) : "memory");
                __asm__ volatile("dsb sy" ::: "memory");
                map->release();
            }
        }
        (void)dramRead32(pa + 8, &after);
        IOLog("MUYU BVMEM cpu=%08x dram_before=%08x dram_after=%08x pa=%08x%08x iova=%08x%08x\n",
              memptrs ? memptrs[2] : 0xdeadbeefu, before, after,
              uint32_t(pa >> 32), uint32_t(pa),
              uint32_t(gGpuBuffers[2].iova >> 32), uint32_t(gGpuBuffers[2].iova));
    }
    gpuWr64(A740_CP_BV_RB_RPTR_ADDR, gGpuBuffers[2].iova + 8);   // rbmemptr(bv_rptr)
    /* fix119: the CP's switch between two rings, ported from uke (which runs it, sheng-gpu-preempt=2, and sheng composites
     * through the GPU). fix118 settled why this is the missing piece: BV's own read pointer (0x0a66) had run all the way to
     * WPTR while CP_BV_IB1_BASE read 0 and the restore address was 0 - BV was executing without ever being given an IB,
     * because nothing was preempting and therefore nothing ever restored one. Both of those fields come from a preemption
     * record, and a record only exists when the board builds one. The command line is full, so the switch rides the same
     * render-mode carrier as everything else: sheng-tum=...p. */
    {
        unsigned preempt = 0;
        if (!PE_parse_boot_argn("sheng-gpu-preempt", &preempt, sizeof preempt)) {
            char carrier[24] = {};
            if (PE_parse_boot_argn("sheng-tum", carrier, sizeof carrier))
                for (const char *c = carrier; *c; ++c) if (*c == 'p') preempt = 2;
        }
        gPreemptWanted = preempt == 1 || preempt == 2;
        gPreemptMode = preempt == 2 ? 2 : 1;
        IOLog("MUYU PREEMPT mode=%u\n", gPreemptWanted ? gPreemptMode : 0u);
        if (gPreemptWanted) {
            const IOReturn second = gRing1.attachSecondary(&rio, &cache, const_cast<uint8_t *>(gGpuCpu[9]), gGpuBuffers[9].bytes,
                gGpuBuffers[9].iova, const_cast<uint8_t *>(gGpuCpu[10]), gGpuBuffers[10].bytes, gGpuBuffers[10].iova);
            if (second) {snprintf(row, sizeof row, "C4 ring1 attach %x", unsigned(second)); setRow(7, row); IOLog("MUYU CPROW %s\n", row); return;}
            const IOReturn p0 = gRing.setPreempt(gGpuBuffers[5].iova, gGpuBuffers[7].iova, gGpuBuffers[2].iova + 8);
            const IOReturn p1 = gRing1.setPreempt(gGpuBuffers[6].iova, gGpuBuffers[8].iova, gGpuBuffers[10].iova + 8);
            if (p0 || p1) {snprintf(row, sizeof row, "C4 preempt %x/%x", unsigned(p0), unsigned(p1)); setRow(7, row); IOLog("MUYU CPROW %s\n", row); return;}
            IOLog("MUYU PREEMPT rings ready rec=%08x/%08x smmu=%08x/%08x\n",
                  uint32_t(gGpuBuffers[5].iova), uint32_t(gGpuBuffers[6].iova),
                  uint32_t(gGpuBuffers[7].iova), uint32_t(gGpuBuffers[8].iova));
        }
    }
    gpuWr(A740_CP_SQE_CNTL, 1);
    // 5. a7xx_patch_pwrup_reglist (preemption list only; IFPC is off) and a7xx_cp_init through the ring.
    c[0] = 5;
    size_t written = 0;
    const IOReturn list = NativeAdrenoPwrupList::buildA740(ringRead, nullptr, const_cast<uint8_t *>(gGpuCpu[3]),
        gGpuBuffers[3].bytes, &written);
    if (list) {snprintf(row, sizeof row, "C5 pwrup list %x", unsigned(list)); setRow(7, row); IOLog("MUYU CPROW %s\n", row); return;}
    __asm__ volatile("dsb sy" ::: "memory");
    const NativeAdrenoCpInitConfig config{gGpuBuffers[0].iova, gGpuBuffers[3].iova, 1};   // base written above, before SQE_CNTL
    uint32_t fence = 0;
    c[2] = uint32_t(gCpInit.run(&rio, &gRing, &config, &fence));
    if (c[2]) {cpRow("ref"); return;}
    c[0] = 6;
    cpRow("sent");
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    c[3] = gRing.wait(fence, &clock, 200000, &c[4]);
    if (c[3]) {cpRow(c[3] == NativeAdrenoRingFenceTimeout ? "to" : "flt"); return;}
    // 6. First real command: CP_MEM_WRITE (opcode 0x3d: address lo/hi, data) into the scratch page, then its fence.
    c[0] = 7;
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gGpuCpu[4]);
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t words[4] = {};
    if (AdrenoPM4Type7(0x3d, 3, &words[0]) != AdrenoPM4Success) {cpRow("pm4"); return;}
    words[1] = uint32_t(gGpuBuffers[4].iova); words[2] = uint32_t(gGpuBuffers[4].iova >> 32); words[3] = 0x5e9a740;
    c[5] = uint32_t(gRing.submit(words, 4, &fence));
    if (c[5]) {cpRow("wref"); return;}
    c[6] = gRing.wait(fence, &clock, 200000, &c[4]);
    c[7] = scratch[0];
    c[0] = c[6] ? 8 : (c[7] == 0x5e9a740 ? 10 : 9);
    // C10 = the GPU wrote the scratch word through its own 4K context and signalled the fence.
    cpRow(c[0] == 10 ? "ok" : c[6] ? "wto" : "wmiss");
    // sheng-gpu-zap=0 skips the zap/secure-mode step (int-019 bisect: int-017 stalled userspace at the logo, int-018 with
    // the probe off reached the desktop). The CP then stays in secure mode, as in int-016.
    if (c[0] == 10) {
        native_gpu_cp_live = true;   // fix194: the CP executed an IB and wrote the scratch word, so GPU-window reads are safe again
        setRow(6, "S zap");
        if (zapLimit()) zapAndSecure();
        else setRow(2, "Z off");
        startHeartbeatThread();
        setRow(6, "S irq");
        irqAndTimer();
        const bool selftest = bootArg("sheng-gpu-selftest", 0) != 0;
        if (selftest) userClientTest();
        setRow(5, "P start");
        /* §38: without this hook NativeAdrenoPublish's hangcheck only counts hangs (H h32 r0 r0) and the
         * ring never recovers. uke registers gpuRestart here; the 17-line port had omitted it. */
        if (native_adreno_set_restart) native_adreno_set_restart(gpuRestart, nullptr);
        publishAndWrapperTest(selftest);
        if (bootArg("sheng-gpu-metal", 0) == 1) publishMetal();
    }
}
void heartbeat();
volatile bool gHeartbeatThread;
void heartbeatThread(void *, wait_result_t) {heartbeat();}
// The heartbeat runs on its own thread from the end of the zap, so the GPU rows keep refreshing even if a self-test below
// stalls (int-027's first BoAlloc never returned and froze rows 4..7).
void startHeartbeatThread() {
    thread_t thread = nullptr;
    if (kernel_thread_start(heartbeatThread, nullptr, &thread) == KERN_SUCCESS) {thread_deallocate(thread); gHeartbeatThread = true;}
}
/* The CP-stall rows are collected in heartbeat() but rows 2/3 of boot_progress_ext are overwritten by later
 * boot stages, and the Q row is overwritten by the coherence row inside the very same branch. The three lines
 * go to the extern "C" muyu_cp_stall_rows above (S = RBBM_STATUS1..3 / CP interrupt / protect / FSR,
 * Q = each CP thread's SQE PC and the header of the packet it sits on, C = the IB coherence sample). */
void heartbeat() {
    // Row 4 from now on: seconds since the run ended, GX state, and (only with GX on) the GPU status words, so a photo
    // tells a live kernel from a hung one and shows whether the GPU stays idle.
    char row[64];
    for (unsigned t = 0;; t += 2) {
        uint32_t info = 0;
        gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuCore, kG2hInfo, &info);
        if (gpuPollable() && gpuRd(A740_CP_RB_RPTR) != gpuRd(A740_CP_RB_WPTR)) {
            native_gpu_stall_seq = t;   /* §43: Q5/Q6 are only refreshed here; Q8 reports this tick to expose their age. */
            // The ring is not drained: where each CP thread is (a6xx.xml A7XX: BV RB_RPTR 0xa66, IB1/IB2 base and remaining dwords
            // 0x928/0x92a and 0x92b/0x92d, BV IB1 0xa6d/0xa70), so a photograph tells which IB and how far into it the CP stopped.
            // Rows are 47 characters, so IB bases show their low 24 bits.
            snprintf(row, sizeof row, "H%u R%x r%x/%x b%x i%x/%x j%x v%x/%x", t, gpuRd(A740_RBBM_STATUS), gpuRd(A740_CP_RB_RPTR),
                     gpuRd(A740_CP_RB_WPTR), gpuRd(0x0a66), gpuRd(0x0928) & 0xffffff, gpuRd(0x092a), gpuRd(0x092d),
                     gpuRd(0x0a6d) & 0xffffff, gpuRd(0x0a70));
            // Row 3 (the load trace, finished by now): RBBM_STATUS1..3 (0x211..0x213; STATUS3 bit 24 SMMU stalled on fault),
            // CP_INTERRUPT_STATUS 0x823, CP_PROTECT_STATUS for BR/BV (0x824 / 0xa61) and the GPU context bank FSR (a terminated
            // translation fault does not stall the SMMU, so STATUS3 alone cannot rule one out).
            char stall[64];
            snprintf(stall, sizeof stall, "S%x/%x/%x I%x P%x/%x f%x a%x/%x c%x", gpuRd(0x0211), gpuRd(0x0212),
                     gpuRd(0x0213), gpuRd(0x0823), gpuRd(0x0824), gpuRd(0x0a61), gpuFsr(), gActlr, gActlrSet, gSacr);
            strlcpy(muyu_cp_stall_rows[0], stall, sizeof muyu_cp_stall_rows[0]);
            setRow(3, stall);
            // Row 2: each thread's SQE PC and the header of the packet it is on. a6xx_cp_hw_err_irq() reads CP_SQE_STAT_DATA after
            // selecting index 1 for the current opcode; index 0 is the PC and successive reads advance (crashdec).
            uint32_t sqe[4];
            gpuWr(0x0908, 0); sqe[0] = gpuRd(0x0909); sqe[1] = gpuRd(0x0909);
            gpuWr(0x0a87, 0); sqe[2] = gpuRd(0x0a88); sqe[3] = gpuRd(0x0a88);
            snprintf(stall, sizeof stall, "Q br %x %08x bv %x %08x", sqe[0], sqe[1], sqe[2], sqe[3]);
            strlcpy(muyu_cp_stall_rows[1], stall, sizeof muyu_cp_stall_rows[1]);
            setRow(2, stall);
            // Once per hang, row 2 then shows instead the last submission's coherence sample (int-070), "C<submissions> c<first IB
            // word, kernel cacheable alias> u<DRAM before the CPU cache maintenance> v<DRAM after it> d<DRAM now>". int-069 found the
            // table entry right and DRAM holding the IB at hang time, yet the CP read a zero header: u=0 with c/v = 0x70970001 means
            // the IB sat in a CPU cache line when the GPU fetched it; all four 0x70970001 point at the GPU side (translation or cache).
            static char coherenceRow[64];
            if (!coherenceRow[0]) {
                const uint64_t ib = (uint64_t(gpuRd(0x0929)) << 32) | gpuRd(0x0928);
                uint64_t pa = 0, w = 0;
                if (gpuWalk(ib, &pa)) physRead64(pa, &w);
                snprintf(coherenceRow, sizeof coherenceRow, "C%u c%x u%x v%x d%x", native_adreno_coherence[0],
                         native_adreno_coherence[1], native_adreno_coherence[2], native_adreno_coherence[3], uint32_t(w));
            }
            strlcpy(muyu_cp_stall_rows[2], coherenceRow, sizeof muyu_cp_stall_rows[2]);
            setRow(2, coherenceRow);
            /* The CP's own ring registers: a zero packet header with valid DRAM words means the CP may be
             * fetching from a ring whose base/control it lost. Expected here: b200000/0 c20c r300000/0. */
            snprintf(muyu_cp_stall_rows[3], sizeof muyu_cp_stall_rows[3],
                     "P b%x/%x c%x r%x/%x s%x f%x t%x", gpuRd(0x0800), gpuRd(0x0801), gpuRd(0x0802),
                     gpuRd(0x0804), gpuRd(0x0805), gpuRd(0x0808), gpuRd(0x0821), gpuRd(0x0825));
            /* WPTR fence retries (writes / dropped once / repeated / dropped after 200 tries) and the stall
             * handler's stages: kick, bus drain, reassert, plus the client/arbiter halt acknowledges. */
            snprintf(muyu_cp_stall_rows[4], sizeof muyu_cp_stall_rows[4],
                     "W%u/%u/%u/%u u%u/%u/%u a%x/%x", (unsigned)native_adreno_wptr_fence[0],
                     (unsigned)native_adreno_wptr_fence[1], (unsigned)native_adreno_wptr_fence[2],
                     (unsigned)native_adreno_wptr_fence[3], (unsigned)native_adreno_unstick[0],
                     (unsigned)native_adreno_unstick[4], (unsigned)native_adreno_unstick[14],
                     (unsigned)native_adreno_unstick[5], (unsigned)native_adreno_unstick[6]);
            /* §38: Q3 proved the ring registers correct, so the remaining question is whether the CP/SQE is
             * halted, power-collapsed (IFPC), or parked in a different hardware context. i = CP_CP2GMU_STATUS
             * (0x812, bit0 IFPC), a = CP_APERTURE_CNTL_HOST (0xa00, host's context select), q = the live
             * CP_SQE_INSTR_BASE, m = CP_MISC_CNTL, r/s/t = the raw RPTR/WPTR, RBBM_STATUS and CP_STATUS_1. */
            snprintf(muyu_cp_stall_rows[5], sizeof muyu_cp_stall_rows[5],
                     "G i%x a%x q%x/%x m%x r%x/%x s%x t%x b%x", gpuRd(0x0812), gpuRd(0x0a00),
                     gpuRd(0x0830), gpuRd(0x0831), gpuRd(0x0840), gpuRd(0x0806), gpuRd(0x0807),
                     gpuRd(0x0210), gpuRd(0x0825), gpuRd(0x0201));
            /* Four words of CP_SQE_STAT per thread, selected from index 0 as a6xx_cp_hw_err_irq does. The
             * first is the microcode PC; the rest show whether the SQE sits in an idle/WFI loop. */
            uint32_t sbr[4] = {}, sbv[4] = {};
            gpuWr(0x0908, 0);
            for (unsigned k = 0; k < 4; ++k) sbr[k] = gpuRd(0x0909);
            gpuWr(0x0a87, 0);
            for (unsigned k = 0; k < 4; ++k) sbv[k] = gpuRd(0x0a88);
            snprintf(muyu_cp_stall_rows[6], sizeof muyu_cp_stall_rows[6],
                     "S br %x/%x/%x bv %x/%x/%x", sbr[1], sbr[2], sbr[3], sbv[1], sbv[2], sbv[3]);
        } else if (gpuPollable()) {
            // Without a stall, row 2 shows the coherence samples, so a passing run still shows whether a CPU cache held an IB.
            if (native_adreno_coherence[0]) {
                char sample[64];
                // C<samples> m<samples with differing values> and the first such sample (else the newest).
                const unsigned at = native_adreno_coherence[4] ? 5 : 1;
                snprintf(sample, sizeof sample, "C%u m%u c%x u%x v%x", native_adreno_coherence[0], native_adreno_coherence[4],
                         native_adreno_coherence[at], native_adreno_coherence[at + 1], native_adreno_coherence[at + 2]);
                setRow(2, sample);
            }
            const volatile uint32_t *mem = reinterpret_cast<const volatile uint32_t *>(gGpuCpu[2]);
            // s<CB0 FSR> G<global GFSR at +0x48>: does the GPU fault through its context after the secure-mode exit?
            const uint32_t gfsr = gSmmuMap ? *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + 0x48) : 0xdead;
            // q<GPU interrupts handled> u<unhandled SPIs>
            snprintf(row, sizeof row, "H%u R%x I%x F%x r%x/%x f%x g%x s%x G%x q%u u%llu", t, gpuRd(A740_RBBM_STATUS),
                     gpuRd(A740_RBBM_INT_0_STATUS), gpuRd(A740_CP_HW_FAULT), gpuRd(A740_CP_RB_RPTR), gpuRd(A740_CP_RB_WPTR),
                     mem ? mem[1] : 0, info, gpuFsr(), gfsr, gIrqCount,
                     (unsigned long long)(&armcpu_spi_unhandled ? armcpu_spi_unhandled : 0));
        } else if (gxOn()) {
            snprintf(row, sizeof row, "H%u gx-on cp-off g%x", t, info);   // fix194: GX powered, CP never initialised, no GPU reads
        } else {
            snprintf(row, sizeof row, "H%u gx-off g%x", t, info);
        }
        /* §46 / restart3: Q7 is now STRICTLY READ-ONLY. restart2 wrote 0x0908 (SQE_STAT index select) and
         * read 0x0909 inside the stalled branch; §45 showed that boot never reproduced the restart1 baseline
         * (RT n0 / H h0 r0 / Z5 f0/0/0/0) while -3 stayed, so that write is the prime suspect for perturbing
         * CP initialisation. Q7 now issues only three 1 ms-spaced CP_RB_RPTR reads and records the ring state:
         * s=0 (drained) means three equal r values are a healthy idle CP, NOT a frozen one (§45.3). Q8 adds
         * t<restart hook count> so a zeroed RT2 can never be mistaken for a real sample. */
        if (gpuPollable()) {
            const uint32_t wptr = gpuRd(A740_CP_RB_WPTR);
            const bool stalled = gpuRd(A740_CP_RB_RPTR) != wptr;
            uint32_t rptr[3] = {};
            for (unsigned k = 0; k < 3; ++k) {
                rptr[k] = gpuRd(A740_CP_RB_RPTR);
                if (k != 2) IODelay(1000);
            }
            snprintf(muyu_cp_stall_rows[7], sizeof muyu_cp_stall_rows[7],
                     "K#%u s%u w%x r%x/%x/%x", t, stalled ? 1 : 0, wptr, rptr[0], rptr[1], rptr[2]);
            const uint32_t gfsr = gSmmuMap ? *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + 0x48) : 0xdeadbeef;
            snprintf(muyu_cp_stall_rows[8], sizeof muyu_cp_stall_rows[8],
                     "M#%u g%x f%x n%u s%x q%x t%u", t, gfsr, gpuFsr(),
                     (unsigned)native_adreno_global_fault[0], (unsigned)native_adreno_global_fault[1],
                     native_gpu_stall_seq, (unsigned)native_gpu_restart[0]);
        }
        setRow(4, row);
        metalRows();
        registerExecObserver();
        if (!(gpuPollable() && gpuRd(A740_CP_RB_RPTR) != gpuRd(A740_CP_RB_WPTR))) gicRow();
        if (t == 4 || t == 40 || t == 120) ipiSelfTest();
        IOSleep(2000);
    }
}

// fix199: restart one user-space process by name so a stalled input consumer can be rebuilt from the kernel side.
// The lookup reuses the pair this file already carries (proc_iterate over PROC_ALLPROCLIST + proc_name_address, as
// scanProcess does) instead of pulling in proc_internal.h, whose proc_iterate/proc_selfname prototypes differ from the
// hand-written ones above (-Werror made that a hard error). The pointer is captured inside the callout, where the list
// lock keeps it valid, and the signal is sent right after proc_iterate returns: backboardd is a long-lived process, so
// the window between the two is a few microseconds. psignal's parameter is the proc_t the callout hands back, declared
// here as void * only because this translation unit has no proc_t typedef. 9 is SIGKILL.
extern "C" void psignal(void *p, int signum);
static void *gRestartHit;
static int restartCallout(void *proc, void *arg) {
    const char *name = proc_name_address(proc);
    if (name && strcmp(name, reinterpret_cast<const char *>(arg)) == 0) {gRestartHit = proc; return 1;}
    return 0;
}
static int muyu_restart_proc(const char *want) {
    if (!want || !want[0]) return -1;
    gRestartHit = nullptr;
    proc_iterate(1 /* PROC_ALLPROCLIST */, restartCallout, const_cast<char *>(want), nullptr, nullptr);
    if (!gRestartHit) return -1;
    psignal(gRestartHit, 9 /* SIGKILL */);
    return 1;
}

void gpuOn() {
    char row[64];
    ShengGmuIo &io = gColdDevice.io;
    uint32_t *g = native_gpu_gx;
    auto rd = [&](unsigned w, uint32_t at) {uint32_t v = 0xdeadbeef; io.read32(io.cookie, w, at, &v); return v;};
    // fix189: one line per sub-step, printed before the next step runs, plus the stop gate.
    auto markPark = [&](unsigned s, uint32_t a, uint32_t b) {
        IOLog("MUYU GX s%u st%x a%x b%x x%x>%x g%x\n", s, native_gpu_power_stage, a, b, g[3], g[4], g[0]);
        if (native_gpu_gxstep <= static_cast<int>(s)) {
            native_gpu_power_stage = 0x8d;
            for (;;) IOSleep(60000);
        }
    };
    native_gpu_power_stage = 0x81;
    // a6xx_gmu_resume, after a successful a6xx_hfi_start: clear GMU2HOST and unmask only CM3_FAULT (A6XX_HFI_IRQ_MASK).
    io.write32(io.cookie, ShengGmuCore, kG2hClr, ~0u);
    io.write32(io.cookie, ShengGmuCore, kG2hMask, ~(1u << 23));
    io.barrier(io.cookie);
    g[3] = rd(ShengGmuCore, kGxStatus);
    markPark(1, 0, 0);
    // a6xx_gmu_set_initial_freq -> a6xx_hfi_set_freq: {ack_type 1 (blocking), freq index, bw index}.
    const uint32_t vote[3] = {1, 1, 1};
    uint32_t out[16], error = 0;
    size_t count = 0;
    g[0] = AdrenoHfiExchange(&gColdDevice.hfi, 30, vote, 3, 1000, hfiWait, out, 16, &count, &error);
    g[1] = error;
    native_gpu_power_stage = 0x82;
    markPark(2, g[0], g[1]);
    // fix190 (2026-11-16): step3 and step4 both leave the kernel able to tick for 400 s while no userspace ever comes
    // up and the touch stream stays at zero frames, i.e. everything that lives on the CX rail slows to a crawl the
    // moment the GMU accepts GMU_OOB_GPU_SET. Read the CX GDSCR and time a fixed batch of MMIO reads right before the
    // request and right after the ack: if the post batch takes far longer than the pre batch in the SAME run, the
    // interconnect/rail corner moved and no amount of further bisection inside gpuOn() will explain it.
    auto bench = [&](const char *tag) {
        const uint64_t t0 = mach_absolute_time();
        uint32_t acc = 0;
        for (unsigned i = 0; i < 4000; ++i) acc += rd(ShengGmuCore, kG2hInfo);
        const uint64_t t1 = mach_absolute_time();
        uint64_t ns0 = 0, ns1 = 0;
        absolutetime_to_nanoseconds(t0, &ns0);
        absolutetime_to_nanoseconds(t1, &ns1);
        uint32_t cx = 0;
        if (gVa[GPB_WIN_GPUCC]) cx = *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPUCC] + GPB_CC_CX_GDSCR);
        IOLog("MUYU GX bench %s us%llu cx%08x acc%x\n", tag, (ns1 - ns0) / 1000, cx, acc);
    };
    if (!g[0]) {
        if (native_gpu_gxdelay > 0) {          // fix197: hold GX back until the boot storm is over.
            IOLog("MUYU GX hold %d s (waiting for the boot storm to end)\n", native_gpu_gxdelay);
            for (int i = 0; i < native_gpu_gxdelay; ++i) IOSleep(1000);
            IOLog("MUYU GX go now\n");
        }
        // hw_init: a6xx_gmu_set_oob(GMU_OOB_GPU_SET), non-legacy bits: request 30, ack 31.
        // The vendor budget is 1 s: a6xx_gmu.c:442 gmu_poll_timeout(GMU2HOST_INTR_INFO, ack, 100 us, 10000).
        // This port waited only 100 x 100 us = 10 ms, so a slow GX/ARC ramp ended the wait with no ack,
        // gx became false, gpuStart() was skipped and the boot showed o=1 with calls=0. The same image has
        // produced calls=1 (o=4, 11-11 20:45) and calls=0, i.e. this is a timing cliff, not a code path.
        bench("pre");
        io.write32(io.cookie, ShengGmuCore, kH2gSet, 1u << 30);
        io.barrier(io.cookie);
        for (g[2] = 1; g[2] <= 10000; ++g[2]) {
            if (rd(ShengGmuCore, kG2hInfo) & (1u << 31)) break;
            IODelay(100);
        }
        io.write32(io.cookie, ShengGmuCore, kG2hClr, 1u << 31);
        io.barrier(io.cookie);
        bench("post");
        if (native_gpu_gxcycle) {   // fix193: give GX back before the rest of the boot runs, and read what it says.
            io.write32(io.cookie, ShengGmuCore, kH2gClr, 1u << 30);
            io.barrier(io.cookie);
            uint32_t off = 0;
            for (off = 1; off <= 10000; ++off) {
                if (!(rd(ShengGmuCore, kG2hInfo) & (1u << 31))) break;
                IODelay(100);
            }
            IOLog("MUYU GX cycle off polls=%u st=%x gdscr=%08x\n", off, rd(ShengGmuCore, kGxStatus),
                gVa[GPB_WIN_GPUCC] ? *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPUCC] + GPB_CC_GX_GDSCR) : 0);
        }
    }
    markPark(3, g[2], 0);
    g[4] = rd(ShengGmuCore, kGxStatus);
    uint32_t gdscr = 0;
    if (gVa[GPB_WIN_GPUCC]) gdscr = *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPUCC] + GPB_CC_GX_GDSCR);
    g[5] = gdscr;
    native_gpu_power_stage = 0x83;
    markPark(4, g[4], gdscr);
    // GX on per a7xx_gmu_gx_is_on: GX_HM_GDSC_POWER_OFF (bit 0) and GX_HM_CLK_OFF (bit 1) both clear.
    // o>10000 means the ack never came inside the vendor-length 1 s budget (the old code printed 101
    // for the same condition out of a 10 ms budget).
    const bool gx = g[2] <= 10000 && !(g[4] & 3);
    if (gx) {
        g[6] = rd(ShengGmuGpu, kRbbmStatus);
        g[7] = rd(ShengGmuGpu, kGbifHalt);
    }
    // X v<vote status>/<fw error> o<ack polls, 101 = none> x<gx before>><after> c<GX GDSCR> R<RBBM_STATUS> H<GBIF_HALT>
    if (gx) snprintf(row, sizeof row, "X v%u/%x o%u x%x>%x c%08x R%08x H%x", g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
    else snprintf(row, sizeof row, "X v%u/%x o%u x%x>%x c%08x gx-off", g[0], g[1], g[2], g[3], g[4], g[5]);
    setRow(5, row);
    IOLog("MUYU COLD %s\n", row);   // fix189: row 5 is set after logRows("COLD") already ran, so print it here
    native_gpu_power_stage = gx ? 0x87 : 0x8f;
    if (gx && mode() >= 9) gpuStart();
    // fix199: with GX on, rebuild the user-space input consumer once and report what the kernel sees. backboardd owns
    // HID event delivery and the scene updates, so a restart is the cheapest way to tell "stalled instance" apart from
    // "the whole I/O complex is wedged". Nothing about the GPU sequence changes here; the restart is a pure add-on.
    if (gx && native_gpu_respring > 0) {
        IOSleep(static_cast<unsigned>(native_gpu_respring) * 1000u);
        /* fix201: a single attempt ten seconds after GX-on always answered -1, because backboardd does
         * not exist yet that early on this port (the first line naming /usr/libexec/backboardd[57] is at
         * 123.9 s, GX came up at 51.6 s). Poll until the name resolves, then kill it once, so the
         * restarted instance is the one that inherits an already-powered GPU. */
        int bb = -1;
        for (unsigned tries = 0; tries < 90u; ++tries) {
            bb = muyu_restart_proc("backboardd");
            if (bb == 1) break;
            IOSleep(1000);
        }
        IOLog("MUYU RESP backboardd r=%d after %d s polled\n", bb, native_gpu_respring);
        if (native_gpu_respring_sb > 0) {
            IOSleep(static_cast<unsigned>(native_gpu_respring_sb) * 1000u);
            const int sb = muyu_restart_proc("SpringBoard");
            IOLog("MUYU RESP SpringBoard r=%d after %d s\n", sb, native_gpu_respring_sb);
        }
    }
    if (gHeartbeatThread) for (;;) IOSleep(60000);
    heartbeat();
}
void gmuColdStart(const gpb_result &r) {
    char row[64];
    native_gpu_power_stage = 0x71;
    if (!ShengGmuColdStart || !ShengGmuBindMmio || !NativeAdrenoGmuContextPayloads) {
        setRow(7, "S nolink");
        return;
    }
    /* Two possible suppliers of the GMU image, in the order that can actually succeed on this board: the muyu blob
     * compiled next to this file, then the native_adreno_* pair that only the sheng/uke archive defines. */
    const uint8_t *fw = muyu_gmu_firmware_size ? reinterpret_cast<const uint8_t *>(muyu_gmu_firmware)
                                                : native_adreno_firmware_image;
    const size_t fw_bytes = muyu_gmu_firmware_size ? static_cast<size_t>(muyu_gmu_firmware_size)
                                                   : static_cast<size_t>(native_adreno_firmware_image_size);
    if (!fw || fw_bytes < 4) {
        snprintf(row, sizeof row, "S nofw m%x n%x", muyu_gmu_firmware_size ? 1 : 0,
                 native_adreno_firmware_image_size ? 1 : 0);
        setRow(7, row);
        return;
    }
    for (unsigned w = 0; w < 5; ++w) {
        const uint64_t phys = ShengGmuWindowPhysical(w), len = ShengGmuWindowBytes(w);
        IODeviceMemory *memory = phys && len ? IODeviceMemory::withRange(phys, len) : nullptr;
        if (memory) {
            gColdMaps[w] = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
            memory->release();
        }
        if (!gColdMaps[w]) {
            snprintf(row, sizeof row, "S nomap w%u p%llx l%llx", w, static_cast<unsigned long long>(phys),
                     static_cast<unsigned long long>(len));
            setRow(7, row);
            return;
        }
        gColdMmio.windows[w] = {reinterpret_cast<volatile unsigned char *>(gColdMaps[w]->getVirtualAddress()), phys, len};
    }
    NativeAdrenoGmuPayload payloads[5];
    const IOReturn got = NativeAdrenoGmuContextPayloads(payloads);
    if (got) {snprintf(row, sizeof row, "S payloads %x", static_cast<unsigned>(got)); setRow(7, row); return;}
    ShengGmuMemory *memory[5] = {&gColdConfig.icache, &gColdConfig.dummy, &gColdConfig.debug, &gColdConfig.log, &gColdConfig.hfi};
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(payloads[i].physical, payloads[i].bytes);
        if (m) {
            gPayloadMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gPayloadMaps[i]) {setRow(7, "S nomap payload"); return;}
        *memory[i] = {reinterpret_cast<volatile uint32_t *>(gPayloadMaps[i]->getVirtualAddress()), payloads[i].bytes,
                      payloads[i].iova};
    }
    ShengGmuDevice &d = gColdDevice;
    d = {};
    if (ShengGmuBindMmio(&gColdMmio, &d.io)) {setRow(7, "S bind refused"); return;}
    d.services = {nullptr, coldSha256, coldNow, coldWait};
    gColdConfig.chip_id = ShengGmuChipId();   // asked from ShengGmuSource.h, never typed here
    gColdConfig.firmware = fw;
    gColdConfig.firmware_bytes = fw_bytes;
    // 1000 x 100 us covers the vendor's 100 ms FW_INIT budget (gmu_poll_timeout(.., 100, 100000)); the old 200
    // could not, and poll() capped every wait at 10 ms anyway, so an alive CM3 would still have looked dead.
    gColdConfig.poll_budget = 1000;
    // a6xx_gmu_resume, before a6xx_gmu_fw_start: clear every AO interrupt and unmask only the fatal ones. The AO
    // block sits at 0x3d8ec10, i.e. 0x23b04 dwords past the 0x3d00000 GPU base; the offset handed to write32() must
    // therefore be measured from the window base the DT actually gives, which used to be a literal 0x6a000 fudge.
    const uint64_t core_base = ShengGmuWindowPhysical(ShengGmuCore);
    const uint64_t ao_clr_abs = 0x03d00000ull + 0x23b04ull * 4, ao_mask_abs = 0x03d00000ull + 0x23b06ull * 4;
    if (core_base && ao_mask_abs + 4 <= core_base + ShengGmuWindowBytes(ShengGmuCore)) {
        d.io.write32(d.io.cookie, ShengGmuCore, static_cast<uint32_t>(ao_clr_abs - core_base), ~0u);
        d.io.write32(d.io.cookie, ShengGmuCore, static_cast<uint32_t>(ao_mask_abs - core_base), ~kAoFatal);
        d.io.barrier(d.io.cookie);
    } else {
        setRow(7, "S ao outside window");   // nothing written; the cold start below will report what it misses
    }
    uint32_t before[4], after[4];
    readGmuState(before);
    const ShengGmuStartupLimits limits = {1000, 100, 50000, 2000000};
    setRow(7, "S running");
    native_gpu_power_stage = 0x72;
    // Bring-up bisect (GPU-HANDOFF.md §69.160): sheng-gpu-coldstop=<n> aborts the cold start the
    // moment phase <n> has completed and leaves its writes in place, so one boot answers which
    // phase of power=7 breaks the UI. 0 (absent) keeps the full cold start untouched.
    unsigned coldstop = 0;
    if (PE_parse_boot_argn("sheng-gpu-coldstop", &coldstop, sizeof coldstop)) sheng_gmu_coldstop_level = (int)coldstop;
    /* fix210 (2026-11-16 20:1x): hold the whole cold start back until the UI is on its feet. See native_gpu_colddelay.
     * The rows on screen keep saying "S running" for the whole hold, and every 30 s this prints the vnode table as a
     * free side channel for fix211 (vn should sit near the ceiling while userland is still spawning). */
    if (native_gpu_colddelay > 0) {
        IOLog("MUYU COLD hold %d s (waiting for the UI)\n", native_gpu_colddelay);
        for (int i = 0; i < native_gpu_colddelay; ++i) {
            IOSleep(1000);
            if (((i + 1) % 30) == 0) {
                IOLog("MUYU COLD held %d/%d s vn=%ld dv=%d\n", i + 1, native_gpu_colddelay, muyu_probe_numvnodes, muyu_probe_desiredvnodes);
            }
        }
        IOLog("MUYU COLD go now\n");
    }
    const ShengGmuStatus status = ShengGmuColdStart(&d, &gColdConfig, &gStartup, &limits, &gColdResult);
    native_gpu_power_stage = status ? 0x7f : 0x77;
    readGmuState(after);
    const ShengGmuResult &l = gColdResult.load;
    const AdrenoHfiStartupResult &p = gColdResult.protocol;
    // GPU1: state before > after (SYSRESET/TCM_RET/RSC busy/HFI status); running firmware reads 0/1/0/1
    snprintf(row, sizeof row, "GPU m7 G%x/%x/%x/%x>G%x/%x/%x/%x", before[0], before[1], before[2], before[3], after[0],
             after[1], after[2], after[3]);
    setRow(4, row);
    // GPU3: P<protocol status> g<stage> k<acknowledged stage mask> e<firmware error> t<transport> n<published commands>
    //   <elapsed ms> s<skipped response packets> h<first skipped header> r<its return header> (int-013: P8 t6 on the
    //   first reply, which the old exact-header match rejected)
    const AdrenoHfi &q = gColdDevice.hfi;
    snprintf(row, sizeof row, "P%u g%u k%x e%x t%u n%u %llums s%u h%x r%x", gColdResult.protocol_status, p.stage,
             p.acknowledged_stages, p.firmware_error, p.transport_status, p.published_commands,
             static_cast<unsigned long long>(p.elapsed_us / 1000), q.skipped, q.skipped_header, q.skipped_ret);
    setRow(6, row);
    // GPU4: S<ShengGmuStatus> ph<load phase> h<hash ok> f<FW_INIT_RESULT> H<HFI status> r<FW ready>/<HFI ready>
    //   a<AO fault status> c<GMU2HOST info> v<reset verified>
    // d<DTCM+0xff8 probe> picks the ready word (0xbabeface when <= 0x20010004, else 0x100), q<polls> is what the
    // FW_INIT wait actually spent - that wait used to stop at a hard 10 ms, a tenth of the vendor's budget.
    snprintf(row, sizeof row, "S%u ph%u h%u f%x H%x r%u%u a%x c%x v%u d%x q%u", status, l.phase,
             l.complete_hash_verified, l.firmware_init_result, l.hfi_status, l.firmware_ready_observed,
             l.hfi_ready_observed, l.ao_fault_status, l.cm3_fault_status, l.reset_verified, l.dtcm_probe, l.poll_reads);
    setRow(7, row);
    native_gpu_cold_status = static_cast<int>(status);   // fix187: gpuOn() now runs after logRows("COLD")
    (void)r;
}
void gmuPrerequisites(const gpb_result &r) {
    char row[64];
    gpb_io io{nullptr, rd, wr, delayUs};
    gpb_result scratch = r;   // gpb_rd records io_error into this copy, not into the published result
    gpb_gmu_clocks &k = native_gpu_gmu_clocks;
    native_gpu_power_stage = 0x51;
    gpb_read_gmu_clocks(&io, &scratch, &k);
    // K p<PLL1 MODE> o<OPMODE> l<L_VAL> g<gmu root off><cfg> h<hub root off><cfg>
    //   b<cx_gmu><cxo><hub_cx_int><demet><gcc axi><gcc memnoc> v<GPLL0 vote bits 15/16>
    if (!k.done) snprintf(row, sizeof row, "K skip");
    else snprintf(row, sizeof row, "p%08x o%x l%08x g%x%03x h%x%03x b%x%x%x%x%x%x v%x", k.pll1_mode, k.pll1_opmode,
                  k.pll1_l, k.gmu_cmd >> 31, k.gmu_cfg & 0x7ff, k.hub_cmd >> 31, k.hub_cfg & 0x7ff,
                  gpb_branch_digit(k.cx_gmu), gpb_branch_digit(k.cxo), gpb_branch_digit(k.hub_cx_int),
                  gpb_branch_digit(k.demet), gpb_branch_digit(k.axi), gpb_branch_digit(k.memnoc), (k.gpll0_vote >> 15) & 3);
    setRow(6, row);
    if (mode() >= 6 && k.done) {
        // C s<last step> f<0 ok, 1 timeout/refused, 2 I/O> l<lock polls> u<hub>/<gmu update polls> then PLL1 MODE/OPMODE
        //   and the hub/gmu CFG low bits after the switch (505 = GPLL0/3, 303 = PLL1/2)
        native_gpu_power_stage = 0x61;
        gpb_gmu_clock_on &o = native_gpu_gmu_clock_on;
        gpb_gmu_clocks_on(&io, &scratch, &k, &o);
        snprintf(row, sizeof row, "C s%u f%u l%u u%u/%u p%08x o%x h%03x g%03x", o.step, o.failed, o.lock_polls, o.hub_polls,
                 o.gmu_polls, o.pll1_mode, o.pll1_opmode, o.hub_cfg & 0x7ff, o.gmu_cfg & 0x7ff);
        setRow(5, row);
        gpb_gmu_clocks after;
        gpb_read_gmu_clocks(&io, &scratch, &after);
        snprintf(row, sizeof row, "p%08x o%x l%08x g%x%03x h%x%03x b%x%x%x%x%x%x v%x", after.pll1_mode, after.pll1_opmode,
                 after.pll1_l, after.gmu_cmd >> 31, after.gmu_cfg & 0x7ff, after.hub_cmd >> 31, after.hub_cfg & 0x7ff,
                 gpb_branch_digit(after.cx_gmu), gpb_branch_digit(after.cxo), gpb_branch_digit(after.hub_cx_int),
                 gpb_branch_digit(after.demet), gpb_branch_digit(after.axi), gpb_branch_digit(after.memnoc),
                 (after.gpll0_vote >> 15) & 3);
        setRow(6, row);
        k.cx_gmu = after.cx_gmu;
    }
    // Command DB: copy with aligned 32-bit reads (the mapping is Device memory), then parse a stable snapshot.
    native_gpu_power_stage = 0x52;
    unsigned parse = 9, build = 9;
    uint32_t version = 0, magic = 0;
    if (volatile uint32_t *db = mapWords(kCmdDbBase, kCmdDbBytes, gCmdDbMap)) {
        for (uint64_t i = 0; i < kCmdDbBytes / 4; ++i) {
            const uint32_t w = db[i];
            memcpy(gCmdDb + 4 * i, &w, 4);
        }
        memcpy(&version, gCmdDb, 4);
        memcpy(&magic, gCmdDb + 4, 4);
        if (ShengGmuCommandDbInputs && ShengGmuBuildStartup) {
            gVoteInputs = {};
            // The id the generated policy was built for, asked from the policy itself. A literal here is how the
            // A740 id (0x43050a01) came to be passed to a header pinned on this board's Adreno735 id (0x43030b00).
            gVoteInputs.chip_id = ShengGmuVotesChipId();
            parse = ShengGmuCommandDbInputs(gCmdDb, kCmdDbBytes, version, &gVoteInputs);
            if (parse == ShengGmuDbSuccess) {
                ShengGmuBuildDiag dg = {};
                // Same archive as ShengGmuBuildStartup below, so this is a direct call, not a weak probe.
                build = ShengGmuBuildStartupDiag(&gVoteInputs, &gStartup, &dg);
                char t[48];
                snprintf(t, sizeof t, "V chip %08x gmxc%u %u/%u/%u/%u", dg.chip_id, dg.gmxc_present, dg.gfx_bytes,
                         dg.cx_bytes, dg.mx_bytes, dg.gmxc_bytes);
                setDiag(1, t);
                snprintf(t, sizeof t, "R gf%u/%u m%u cx%u/%u m%u", dg.gpu_match, dg.gpu_total, dg.gpu_first_missing,
                         dg.gmu_match, dg.gmu_total, dg.gmu_first_missing);
                setDiag(2, t);
                snprintf(t, sizeof t, "B st%u why%x d%u p%u s%u c%08x", build, dg.why, dg.detail, parse, build,
                         gVoteInputs.cn0_address);
                setDiag(3, t);
            }
        } else parse = 8;
    }
    // GMU state words, only with the GMU clock and the GPUCC AHB clock already running (nothing here enables them).
    native_gpu_power_stage = 0x53;
    bool gmu = gpb_gmu_regs_readable(&scratch, &k);
    if (gmu) {
        volatile uint32_t *core = mapWords(kGmuPhys[0], kGmuLen[0], gGmuMaps[0]);
        volatile uint32_t *rscc = mapWords(kGmuPhys[1], kGmuLen[1], gGmuMaps[1]);
        volatile uint32_t *misc = mapWords(kGmuPhys[2], kGmuLen[2], gGmuMaps[2]);
        gmu = core && rscc && misc;
        if (gmu) {
            native_gpu_gmu_state[0] = core[kGmuSysReset / 4];
            native_gpu_gmu_state[1] = misc[kCxMiscTcmRet / 4];
            native_gpu_gmu_state[2] = rscc[kRsccSeqBusy / 4];
            native_gpu_gmu_state[3] = core[kGmuHfiStatus / 4];
        }
    }
    // D v<version> m<magic> p<parse ShengGmuDbStatus> s<build AdrenoHfiStartupStatus> x<GMxC present> then
    //   G<sysreset>/<tcm ret>/<rsc busy>/<hfi status>, or G- when the GMU clock is off
    char gmuText[32];
    if (gmu) snprintf(gmuText, sizeof gmuText, "G%x/%x/%x/%x", native_gpu_gmu_state[0], native_gpu_gmu_state[1],
                      native_gpu_gmu_state[2], native_gpu_gmu_state[3]);
    else snprintf(gmuText, sizeof gmuText, "G-");
    // Row 4 keeps the mode and the power summary and carries the GMU state, so a command-DB diagnostic in row 7 never hides it.
    snprintf(row, sizeof row, "GPU m%u io=%d pw%u%u %s", r.mode, r.io_error, r.powered_cx, r.powered_clocks, gmuText);
    setRow(4, row);
    if (parse != ShengGmuDbSuccess && parse != 9) {
        // H<slv_id>:<cnt> of the slave table (cmd_db_header.header[i], 16 bytes from offset 8) up to the first zero id
        int at = snprintf(gmuText, sizeof gmuText, "H");
        for (unsigned i = 0; i < 8 && at < static_cast<int>(sizeof gmuText) - 8; ++i) {
            uint16_t id, cnt;
            memcpy(&id, gCmdDb + 8 + 16 * i, 2);
            memcpy(&cnt, gCmdDb + 8 + 16 * i + 6, 2);
            if (!id) break;
            at += snprintf(gmuText + at, sizeof gmuText - static_cast<size_t>(at), "%x:%x ", id, cnt);
        }
    }
    if (gmuText[0] == 'H') snprintf(row, sizeof row, "D p%u %s", parse, gmuText);   // int-011 read v1 m0c0330db already
    else snprintf(row, sizeof row, "D v%x m%08x p%u s%u x%u %s", version, magic, parse, build, gVoteInputs.gmxc_present,
                  gmuText);
    setRow(7, row);
    native_gpu_power_stage = 0x5f;
    if (mode() >= 7) {
        const gpb_gmu_clock_on &o = native_gpu_gmu_clock_on;
        const bool idle = gmu && native_gpu_gmu_state[0] == 1 && !native_gpu_gmu_state[1] && !native_gpu_gmu_state[2] &&
                          !native_gpu_gmu_state[3];
        if (o.step == 7 && !o.failed && parse == ShengGmuDbSuccess && build == AdrenoHfiStartupSuccess && idle)
            gmuColdStart(r);
        else {
            snprintf(row, sizeof row, "S skip c%u/%u p%u s%u i%u", o.step, o.failed, parse, build, idle);
            setRow(7, row);
        }
        logRows("COLD");   // IOLog at t+10 s: the 5 s printf dump starts at t+30 s, after userspace has the panel
        // fix187: GX power-on now comes AFTER the COLD rows, so the log always carries the cold-start result even when
        // gpuOn() stalls the bus. sheng-gpu-gxstop=0 skips it entirely.
        if (mode() >= 8 && native_gpu_cold_status == static_cast<int>(ShengGmuSuccess) && native_gpu_gxstop) gpuOn();
    }
}
void probeThread(void *, wait_result_t) {
    registerExecObserver();
    unsigned mode = 0;
    PE_parse_boot_argn("sheng-gpu-power", &mode, sizeof mode);
    if (mode > 9) mode = 9;
    unsigned gxstop = 1;   // fix187: sheng-gpu-gxstop=0 keeps GX off even when the cold start succeeded
    if (PE_parse_boot_argn("sheng-gpu-gxstop", &gxstop, sizeof gxstop)) native_gpu_gxstop = gxstop ? 1 : 0;
    unsigned gxstep = 5;   // fix189: sheng-gpu-gxstep=N parks gpuOn() after sub-step N
    if (PE_parse_boot_argn("sheng-gpu-gxstep", &gxstep, sizeof gxstep)) native_gpu_gxstep = gxstep > 5 ? 5 : static_cast<int>(gxstep);
    unsigned gxcycle = 0;   // fix193: sheng-gpu-gxcycle=1 withdraws the GX request right after the ack
    if (PE_parse_boot_argn("sheng-gpu-gxcycle", &gxcycle, sizeof gxcycle)) native_gpu_gxcycle = gxcycle ? 1 : 0;
    unsigned gxdelay = 0;   // fix197: sheng-gpu-gxdelay=N holds the OOB_GPU_SET write back N seconds
    if (PE_parse_boot_argn("sheng-gpu-gxdelay", &gxdelay, sizeof gxdelay)) native_gpu_gxdelay = gxdelay > 1800 ? 1800 : static_cast<int>(gxdelay);
    unsigned colddelay = 0;   // fix210: sheng-gpu-colddelay=N runs the GMU cold start N seconds later (after the UI is up)
    if (PE_parse_boot_argn("sheng-gpu-colddelay", &colddelay, sizeof colddelay)) native_gpu_colddelay = colddelay > 1800 ? 1800 : static_cast<int>(colddelay);
    unsigned rs = 0;   // fix199: sheng-gpu-respring=N restarts backboardd N seconds after GX comes on
    if (PE_parse_boot_argn("sheng-gpu-respring", &rs, sizeof rs)) native_gpu_respring = rs > 120 ? 120 : static_cast<int>(rs);
    unsigned rsb = 0;   // fix199: sheng-gpu-respring-sb=N then restarts SpringBoard N seconds later
    if (PE_parse_boot_argn("sheng-gpu-respring-sb", &rsb, sizeof rsb)) native_gpu_respring_sb = rsb > 120 ? 120 : static_cast<int>(rsb);
    /* fix211: one read-only line, no code path changes. sane < 65536 means unix_startup's formula produced the 1024 the
     * jetsam lines show; max_mem_actual says how much RAM the VM really got; dv is the table size that formula chose. */
    IOLog("MUYU MEM sane=%llu max=%llu dv=%d vn=%ld mode=%u\n", muyu_probe_sane_size, muyu_probe_max_mem, muyu_probe_desiredvnodes, muyu_probe_numvnodes, mode);
    IOSleep(10000);
    native_gpu_power_stage = 1;
    unsigned have = 0;
    for (unsigned w = 0; w < 5; ++w) {
        IODeviceMemory *memory = IODeviceMemory::withRange(kPhys[w], kLen[w]);
        if (!memory) continue;
        gMaps[w] = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        memory->release();
        if (gMaps[w]) {gVa[w] = static_cast<uintptr_t>(gMaps[w]->getVirtualAddress()); have |= 1u << w;}
    }
    // Rows first show "mapped" so a fault in the sequence still leaves the stage on screen.
    char row[64];
    snprintf(row, sizeof row, "GPU m%u mapped=%02x running", mode, have);
    setRow(4, row);
    native_gpu_power_stage = 2;
    gpb_io io{nullptr, rd, wr, delayUs};
    gpb_run(&io, mode, have, &native_gpu_power_result);
    publish(native_gpu_power_result);
    native_gpu_power_stage = 3;
    if (mode >= 4) gmuContext(native_gpu_power_result);
    if (mode >= 5 && native_gpu_gmu_context.step == NativeAdrenoGmuContext::Ready && !native_gpu_gmu_context.status)
        gmuPrerequisites(native_gpu_power_result);
}
}

extern "C" void sheng_gpu_power_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("sheng-gpu-power", &mode, sizeof mode) || !mode || native_gpu_power_stage) return;
    native_gpu_power_stage = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(probeThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_gpu_power_stage = 0xdead;
}
