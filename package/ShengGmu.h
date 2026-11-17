/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_GMU_H
#define SHENG_GMU_H
#include "AdrenoHfiStartup.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {ShengGmuCore,ShengGmuRscc,ShengGmuPdc,ShengGmuGpu,ShengGmuCxMisc,ShengGmuWindows};
typedef enum {
    ShengGmuSuccess,ShengGmuArgument,ShengGmuFirmware,ShengGmuBusy,
    ShengGmuIoError,ShengGmuStateMismatch,ShengGmuTimeout,ShengGmuClockError,
    ShengGmuPoisoned,ShengGmuFault,ShengGmuProtocolError
} ShengGmuStatus;
typedef struct {
    void *cookie;
    size_t bytes[ShengGmuWindows];
    int (*read32)(void *,unsigned window,size_t byte_offset,uint32_t *);
    int (*write32)(void *,unsigned window,size_t byte_offset,uint32_t);
    int (*barrier)(void *);
} ShengGmuIo;
typedef struct {
    void *cookie;
    int (*sha256)(void *,const void *,size_t,uint8_t digest[32]);
    int (*now_us)(void *,uint64_t *);
    int (*wait_us)(void *,unsigned);
} ShengGmuServices;
typedef struct {volatile uint32_t *cpu;size_t bytes;uint64_t iova;} ShengGmuMemory;
typedef struct {
    uint32_t chip_id;
    const uint8_t *firmware;size_t firmware_bytes;
    ShengGmuMemory icache,dummy,debug,log,hfi;
    unsigned poll_budget;
    /* Optional: run between the FW_INIT observation and HFI_CTRL_INIT. The vendor driver raises the
     * GX rail here (a6xx_gmu_gfx_rail_on: BOOT_SLUMBER_OPTION=0, GX/MX vote indices, then the
     * BOOT_SLUMBER OOB) before it starts HFI. 0 = success. */
    int (*pre_hfi)(void *cookie);
    void *pre_hfi_cookie;
} ShengGmuConfig;
typedef struct {
    ShengGmuIo io;ShengGmuServices services;AdrenoHfi hfi;
    uint32_t poisoned,boot_attempted;
    AdrenoHfiStartup startup;
} ShengGmuDevice;
typedef struct {
    uint32_t phase,write_attempts,read_attempts,poll_reads;
    uint32_t complete_hash_verified,memory_changed,reset_verified;
    uint32_t firmware_init_result,hfi_status,firmware_ready_observed,hfi_ready_observed;
    uint32_t ao_fault_status,cm3_fault_status;
    uint32_t resources_must_retain;
    uint32_t dtcm_probe;   /* REG_GMU_CM3_DTCM_START+0xff8 before boot: picks the 0xbabeface or 0x100 ready word */
    uint8_t complete_sha256[32];
} ShengGmuResult;

/* Board-specific COLD start through firmware/HFI controller readiness.
 * This is NOT a power/clock, SMMU ownership or production boot entry point.
 * Caller must establish exclusive device/stream ownership; hold CX/GX,
 * core/hub clocks at the verified A740 boot rates, RPMh/interconnect votes,
 * fault interrupts and secure/platform prerequisites; install actual native4K
 * GMU SMMU mappings; prove coherent CPU/GMU visibility and pin all storage.
 * The GMU must already be held in reset and RSC idle, with retention cleared.
 * This function verifies those readable states before mutating anything.
 * It does not read silicon identity; chip_id must be from trusted discovery.
 * It validates the complete pinned firmware hash before register writes,
 * installs RSC/PDC setup and all payloads, verifies TCM payload readback,
 * initializes HFI memory, configures/releases CM3 and polls fresh FW/HFI
 * status with elapsed-time AND iteration bounds. No substituted ACK exists.
 * The required hash/time/wait services must perform real operations.
 * Memory extents are byte sizes, mapped with native4K pages, never 16K pages.
 * Config/input/device/result/IO/services and all storage must be disjoint,
 * immutable where applicable and retained for the device's lifetime.
 * Virtual extent checks do not prove physical ownership or alias isolation;
 * inspect actual CPU/GPU tables independently. A device allows one attempt.
 * Use ColdStart below to also exchange the modern HFI startup messages.
 * Debug draining, GPU submission and fault recovery are separate operations.
 * Any failure after mutation attempts reset assertion and poisons the session.
 * Even verified reset does NOT drain SMMU/bus traffic or authorize release;
 * resources_must_retain stays true on all paths. No automatic retry is safe. */
/* Result readiness fields are observations, never current-running assertions:
 * a later failure may already have reasserted reset. */
ShengGmuStatus ShengGmuLoadAndStartHfi(ShengGmuDevice *,const ShengGmuConfig *,ShengGmuResult *);
/* Chip id pinned by this build's generated ShengGmuSource.h; platform callers must use it rather than a literal. */
uint32_t ShengGmuChipId(void);
/* Physical base and size of window <ShengGmuCore..ShengGmuCxMisc>, as the same generated header pins them.
 * ShengGmuBindMmio() refuses any window that differs, so a platform that maps them itself must ask here. */
uint64_t ShengGmuWindowPhysical(unsigned);
size_t ShengGmuWindowBytes(unsigned);
/* 2026-11-16 (GPU-HANDOFF.md §69.160) bring-up bisect switch, default 0 = off.
 * Non-zero N makes ShengGmuLoadAndStartHfi() return ShengGmuBusy the moment internal
 * phase N has completed, WITHOUT any rollback: whatever MMIO that phase already wrote
 * stays in place. One boot therefore answers "does the hardware state left behind by
 * phase N kill the UI?" for the sheng-gpu-power=7..9 ladder, which is otherwise a
 * single indivisible step. Phases: 2 = RSC/AO setup written (GMU still in reset),
 * 3 = firmware copied into TCM and verified, 4 = boot config registers verified,
 * 5 = CM3 released and FW_INIT observed (still no HFI). Set by the boot argument
 * sheng-gpu-coldstop=<n> in GpuPowerProbe.cpp. */
extern int sheng_gmu_coldstop_level;

typedef struct {
    unsigned polls_per_command,wait_interval_us;
    uint64_t command_timeout_us,total_timeout_us;
} ShengGmuStartupLimits;
typedef struct {
    ShengGmuResult load;
    AdrenoHfiStartupResult protocol;
    AdrenoHfiStartupStatus protocol_status;
    ShengGmuStatus health_status;
} ShengGmuStartupResult;
/* Full cold firmware/HFI startup sequence, under all LoadAndStartHfi platform
 * preconditions above. Validate protocol/limits before any hardware mutation,
 * then load firmware and exchange every required command with actual ACKs.
 * Health checks reread reset/HFI status and fatal registers around commands.
 * Supplied OPP/ARC/BCM/ACD data must be platform-derived; this function neither
 * invents command-db values nor sets power, clocks, SMMU or AOSS policy.
 * Any protocol failure after the load attempts reset and permanently poisons
 * the device. A verified reset still does not authorize releasing mappings.
 * Success covers firmware startup exchanges, NOT SQE/ZAP/GPU initialization,
 * shader execution, submission, presentation or a complete hardware driver. */
ShengGmuStatus ShengGmuColdStart(ShengGmuDevice *,const ShengGmuConfig *,
    const AdrenoHfiStartupConfig *,const ShengGmuStartupLimits *,ShengGmuStartupResult *);

typedef struct {volatile unsigned char *cpu;uint64_t physical;size_t bytes;} ShengGmuMmioWindow;
typedef struct {ShengGmuMmioWindow windows[ShengGmuWindows];} ShengGmuMmio;
/* Bind only already owned/powered Device mappings of exact pinned DT ranges.
 * Uses direct AArch64 volatile 32-bit I/O + dsb sy. No map/allocation/power,
 * pointer ownership proof or external-abort recovery is provided. */
ShengGmuStatus ShengGmuBindMmio(ShengGmuMmio *,ShengGmuIo *);
#ifdef __cplusplus
}
#endif
#endif
