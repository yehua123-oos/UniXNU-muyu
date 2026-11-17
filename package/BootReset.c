// SPDX-License-Identifier: MIT
// Board glue for BootResetCore.h on muyu (SM8635): PSCI feature probe, a read-only look at the PMIC arbiter and the reboot-reason cell,
// and the guarded "restart into fastboot" (boot argument sheng-autoreset=<seconds>, see BootLog.c). Ported from
// UniXNU-sheng/package/BootReset.c; every constant below was re-checked against this board's own device tree before it was used:
//   soc/qcom,spmi@c42d000  reg <0xc42d000 0x4000 0xc400000 0x3000 0xc500000 0x400000 0xc440000 0x80000 ...>,
//                          reg-names "cnfg" "core" "chnls" "obsrvr" "intr", qcom,ee = <0>
//   soc/qcom,spmi@c42d000/qcom,pmk8550@0/sdam@7100   compatible "qcom,spmi-sdam", reg = <0x7100 0x100>
//     .../restart@48                                  reg = <0x48 0x1>, bits = <1 7>   (mask 0xfe, shift 1)
// so the reboot-reason byte the stock bootloader reads is SDAM byte 0x48 of the PMIC on SID 0, and "bootloader" is 2 in its top 7 bits.
// Nothing here runs without BootLog's boot arguments: without sheng-autoreset the probe only reports and never writes.
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <arm/cpu_data_internal.h>
#include <mach/machine.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <kern/sched.h>
#include <pexpert/pexpert.h>
#include "BootResetCore.h"

/* fix223: the "log went quiet" wall, censused instead of guessed.
 *
 * Every unattended round ends the same way: the kernel message buffer stops growing at 142-184 s, the boot log writer
 * stops with it, and the hypervisor resets the tablet a minute later. fix219 is the exception that makes it legible -
 * there the GPU path died at 293 s, the system fell back to CPU rendering, and the log kept going to 479 s. So the halt
 * tracks "the GPU path keeps submitting", not time, and it looks like a priority inversion: something runs at a priority
 * that starves the writer (BootReset.c's own note calls it "the SpringBoard spin that starves the writer at ~174 s").
 *
 * A stack needs a panic, and a panic stops the writer that would have to persist it, so the stack would be lost with the
 * message buffer. What is cheap and still survives is the seconds *before* the halt: the writer is healthy then, so a
 * periodic census printed from the watchdog's own thread lands on disk while the system is still degrading. Each line
 * carries the boot log's round counter and its age, plus the thread that each CPU is running at that moment, so the last
 * lines before the silence say who was spinning and how long the writer had already been starved. */
extern volatile uint32_t boot_log_rounds;
extern volatile uint32_t boot_log_errors;
extern volatile uint32_t native_adreno_submit_ibs[4];
static void stall_census(unsigned long long uptime_s) {
  static uint32_t seen_round, quiet_ticks;
  const uint32_t round = boot_log_rounds;
  if (round != seen_round) {seen_round = round; quiet_ticks = 0;} else ++quiet_ticks;
  /* fix227: which thread each CPU is running, because four identical rounds all stopped at 175.4-175.6 s while the log
   * was still perfectly healthy (age=0 right up to the cut). The census itself cannot survive the freeze, so what it has
   * to capture is the state a second before: the last line then names the threads that were running when the system
   * stopped scheduling, which separates "one user thread spinning with the others waiting" from "every CPU in a spinlock".
   * fix229: the same pid is now counted for how many consecutive seconds its CPU ran it, and a run that reaches five
   * seconds prints one line with the process name. The first census already showed the shape: CPU 4 held pid 1966 for
   * the last 34 seconds of the run while every other CPU changed threads about once a second, so "who spins" is a
   * question with an answer — it just needs the name. proc_name() is a bsd symbol; declared here like proc_selfname. */
  static int last_pid[8]; static unsigned run_len[8];
  char line[192];
  int at = snprintf(line, sizeof line, "MUYU STALL t=%llus round=%u age=%us err=%u sub=%u/%u",
                    uptime_s, round, quiet_ticks, (unsigned)boot_log_errors,
                    (unsigned)native_adreno_submit_ibs[0], (unsigned)native_adreno_submit_ibs[1]);
  const unsigned cpus = machine_info.logical_cpu_max < 8 ? machine_info.logical_cpu_max : 8;
  for (unsigned i = 0; i < cpus && at > 0 && at < (int)sizeof line - 16; ++i) {
    const thread_t t = cpu_datap(i) ? cpu_datap(i)->cpu_active_thread : NULL;
    const int pid = t ? (int)thread_tid(t) : 0;
    if (pid && pid == last_pid[i]) ++run_len[i]; else {last_pid[i] = pid; run_len[i] = 0;}

    at += snprintf(line + at, sizeof line - (size_t)at, " %u:%d/%u", i, pid, run_len[i]);
  }
  printf("%s\n", line);
  /* fix232: no name lookup at all. fix230 called proc_name_address() here and this very thread stopped at 140 s; fix231 called
   * proc_name() and the tablet then hung so hard that even the hypervisor watchdog never fired (700 s, no restart) — a call that
   * sleeps on a process lock must not live in the thread whose job is to keep pinging. The run length is the measurement that
   * survives: "CPU 7 held tid 2045 for 32 s" already names the spinning thread, and the id can be matched in the log elsewhere. */
  for (unsigned i = 0; i < cpus; ++i) {
    if (run_len[i] != 5 || !last_pid[i]) continue;
    printf("MUYU STUCK cpu=%u tid=%d sec=%u\n", i, last_pid[i], run_len[i]);
  }
}

// Physical windows from this board's devicetree (spmi@c42d000 reg, reg-names core/chnls/obsrvr/cnfg).
enum {SpmiCore = 0xc400000, SpmiCoreSize = 0x3000, SpmiObsrvr = 0xc440000, SpmiChnls = 0xc500000, SpmiCnfg = 0xc42d000, SpmiCnfgSize = 0x4000};
enum {PsciVersion = 0x84000000u, PsciFeatures = 0x8400000au, PsciSystemReset = 0x84000009u, PsciSystemReset2_32 = 0x84000012u,
      PsciSystemReset2_64 = 0xc4000012u};

// SMCCC call with two arguments; x0 and x2 come back (the Gunyah watchdog's status reports the time since the last ping in x2).
static uint64_t smc2(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t *r2) {
  register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = a1, x2 __asm__("x2") = a2, x3 __asm__("x3") = 0;
  __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                   "x14", "x15", "x16", "x17");
  if (r2) *r2 = x2;
  return x0;
}

static uint64_t psci(uint64_t fid, uint64_t a1) {
  register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = a1, x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                   "x14", "x15", "x16", "x17");
  return x0;
}

// The hypervisor's watchdog (Gunyah, SMCCC vendor hypervisor calls 5 control, 6 status, 7 ping, 8 set time in ms, as Linux gunyah_wdt
// uses them). An unattended run arms it with a 60 s timeout and pings it from its own thread until <deadline> seconds after the start, then
// stops pinging: the hypervisor restarts the tablet even when the kernel hangs or panics, and the cell above makes that restart fastboot.
// Declared before the probe because the probe reports its status: on the 2026-11-13 boot PSCI came back v1.1 with SYSTEM_RESET unsupported
// (reset=0), so this watchdog is the only restart path that does not need the PMIC's reset pin.
enum {GunyahWdtControl = 0x86000005u, GunyahWdtStatus = 0x86000006u, GunyahWdtPing = 0x86000007u, GunyahWdtSetTime = 0x86000008u,
      GunyahWdtEnable = 3, GunyahWdtDisable = 2,
      // 1 s, with the ping interval below at 200 ms. Measured on fix9-fix13: the old 60 s timeout plus a 10 s ping period put the restart
      // 174-246 s after boot no matter what sheng-autoreset said, because the deadline check lived in code paths that stop running. With
      // a 1 s timeout the restart follows the last ping within a second, so the deadline alone decides when the run ends.
      // 2026-11-16 07:4x fix166 (GPU-HANDOFF.md §69.153): 1 s -> 60 s for the ARM path only. fix165 R1/R2 proved the arm works
      // ([reset] probe wdt=0 last=3ms at 90.258/100.476 s = armed by XBL and pinged 3 ms ago) and that sheng-autoreset never fired
      // (tried=0, the 600 s deadline was never reached), so the only armed trigger left is this timeout: a >1 s starvation of the
      // BASEPRI_REALTIME ping thread (the SpringBoard spin that starves the writer at ~174 s) bites within a second. At 60 s a
      // transient stall no longer restarts the box. boot_reset_wdt_hurry() keeps its own hard-coded 1000, so the sheng-autoreset
      // deadline still pulls the timeout down and restarts within a second of firing.
      GunyahWdtTimeoutMs = 60000, GunyahWdtPingMs = 200};

// ml_io_map hands out permanent mappings (io_submap, KMA_PERMANENT) that ml_io_unmap cannot free: its kmem_free on kernel_map panics with the
// map lock held (int-105 hung launchd that way). Mappings that are unmapped again must be unmappable ones (flags 7 = VM_WIMG_IO).
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);
static volatile uint32_t *map(uint64_t phys, uint64_t size) {return (volatile uint32_t *)ml_io_map_unmappable((vm_offset_t)phys, (vm_size_t)size, 7);}

struct reset_maps {
  struct br_arbiter a;
  struct br_found f;
  volatile uint32_t *obs, *chn;
  uint64_t obs_size, chn_size;
  int find;   // br_find's result, -1 before it ran
};
static void unmap_all(struct reset_maps *m) {
  if (m->a.core) ml_io_unmap((vm_offset_t)m->a.core, SpmiCoreSize);
  if (m->a.cnfg) ml_io_unmap((vm_offset_t)m->a.cnfg, SpmiCnfgSize);
  if (m->obs) ml_io_unmap((vm_offset_t)m->obs, (vm_size_t)m->obs_size);
  if (m->chn) ml_io_unmap((vm_offset_t)m->chn, (vm_size_t)m->chn_size);
  memset(m, 0, sizeof *m);
}
// Maps the arbiter, finds the SDAM's APID and maps its observer (and, when asked, read/write) channels. 0 = ready.
static int open_maps(struct reset_maps *m, int want_write) {
  memset(m, 0, sizeof *m);
  m->find = -1;
  m->a.ee = 0;   // qcom,ee = <0>
  m->a.core = map(SpmiCore, SpmiCoreSize);
  m->a.cnfg = map(SpmiCnfg, SpmiCnfgSize);
  if (!m->a.core || !m->a.cnfg) return 1;
  m->find = br_find(&m->a, &m->f);
  if (m->find) return 2;
  m->obs_size = ((uint64_t)BR_OBS_APID_STRIDE * ((uint64_t)m->f.apid + 1) + 0xfff) & ~0xfffull;
  m->obs = map(SpmiObsrvr, m->obs_size);
  if (!m->obs) return 3;
  if (want_write) {
    m->chn_size = (uint64_t)BR_CHNL_APID_STRIDE * ((uint64_t)m->f.apid + 1);
    m->chn = map(SpmiChnls, m->chn_size);
    if (!m->chn) return 4;
  }
  return 0;
}

// The arbiter and the SDAM cell, read only. Fills text; returns 0 when the cell can be written (version known, APID found, owned by this EE).
int boot_reset_probe(char *text, unsigned cap) {
  const uint64_t ver = psci(PsciVersion, 0);
  const int64_t f_reset = (int64_t)psci(PsciFeatures, PsciSystemReset), f2_32 = (int64_t)psci(PsciFeatures, PsciSystemReset2_32),
                f2_64 = (int64_t)psci(PsciFeatures, PsciSystemReset2_64);
  // The hypervisor's watchdog. status 0 means it is already armed (XBL arms it) and the SMCCC control calls below will be honoured; any
  // other value means this board has no Gunyah watchdog service and the restart has to come from somewhere else.
  uint64_t wdt_since = 0;
  const int64_t wdt = (int64_t)smc2(GunyahWdtStatus, 0, 0, &wdt_since);
  struct reset_maps m;
  uint8_t cell = 0xff;
  int open_rc = open_maps(&m, 0), read_rc = -1;
  if (!open_rc) read_rc = br_read8(m.obs, &m.a, m.f.apid, BR_REBOOT_CELL, &cell);
  const int writable = !open_rc && !read_rc && m.f.owner == m.a.ee;
  snprintf(text, cap, "psci v%x reset=%lld reset2_32=%lld reset2_64=%lld | wdt=%lld last=%llums | spmi v%08x apids=%u open=%d find=%d apid=%d owner=%d sdam48=%02x read=%d ok=%d",
           (unsigned)ver, (long long)f_reset, (long long)f2_32, (long long)f2_64, (long long)wdt, (unsigned long long)wdt_since,
           m.a.core ? m.f.version : 0, m.a.core ? m.f.apids : 0, open_rc,
           m.find, m.find ? -1 : m.f.apid, m.find ? -1 : m.f.owner, cell, read_rc, writable);
  unmap_all(&m);
  return writable ? 0 : 1;
}

// Writes "bootloader" into the reboot-reason cell and verifies it, without restarting. Done once early in an unattended run, it sends every
// later restart to fastboot: the timed one below, a watchdog bite, or a long press of the power key.
int boot_reset_mark_bootloader(char *text, unsigned cap) {
  struct reset_maps m;
  uint8_t before = 0xff, after = 0xff;
  int rc = open_maps(&m, 1);
  if (rc) {snprintf(text, cap, "mark refused: open=%d find=%d", rc, m.find); unmap_all(&m); return rc;}
  if (m.f.owner != m.a.ee) {snprintf(text, cap, "mark refused: apid %d is owned by EE %d", m.f.apid, m.f.owner); unmap_all(&m); return 5;}
  rc = br_set_bootloader(m.obs, m.chn, &m.a, m.f.apid, &before, &after);
  snprintf(text, cap, "mark: cell %02x -> %02x rc=%d", before, after, rc);
  unmap_all(&m);
  return rc ? 10 + rc : 0;
}

// The Gunyah watchdog SMC ids are declared above the probe, which reports their status.
volatile int32_t boot_reset_wdt_stats[4];   // [0] status call result, [1] set-time result, [2] enable result, [3] pings sent
static uint32_t wdt_deadline;
// Cuts the ping loop short: the caller has decided the run is over and the restart should be the watchdog's, now rather than at the
// deadline. The thread wakes from its 10 s sleep within one period and exits, and the hypervisor's timeout then runs out (~26 s measured
// on 2026-11-13, which is why this is a real mode and not just a tidier version of the deadline). Safe before boot_reset_watchdog ran:
// there is no thread to stop and the deadline is simply already 0.
int boot_reset_wdt_hurry(void) {
  wdt_deadline = 0;
  // Pull the hypervisor's own timeout down too. Stopping the pings is not enough to be quick: the thread can sit in its 10 s sleep, and
  // the measured delay from the last ping to the restart was ~60 s (fix10: pings stopped at 183 s, fastboot at 246 s). At 1 s the
  // restart follows within a couple of seconds of the deadline. If the set-time is refused the pings are still stopped, which is the
  // 60 s path, so this cannot make things worse.
  boot_reset_wdt_stats[1] = (int32_t)smc2(GunyahWdtSetTime, 1000, 1000, NULL);
  boot_reset_wdt_stats[2] = (int32_t)smc2(GunyahWdtControl, GunyahWdtEnable, 0, NULL);
  return 1;   // the ping thread exists and will exit; 0 from the stub means this board has no watchdog thread
}
// fix159: one explicit ping for the per-second tick thread in BootLog.c. The hypervisor timeout is 1 s,
// so a caller that only ticks once a second sits exactly on the edge; that thread pings every 200 ms and
// calls boot_log_tick(real uptime) once a second instead. After hurry() the caller checks
// boot_log_reset_tried and stops pinging, so the watchdog is free to bite. Query-only: no state change
// beyond the ping counter, the wdt_thread's own deadline is left alone.
int boot_reset_wdt_ping(void) {
  smc2(GunyahWdtPing, 0, 0, NULL);
  boot_reset_wdt_stats[3] = boot_reset_wdt_stats[3] + 1;
  return 1;
}
static void wdt_thread(void *arg, wait_result_t wr) {
  (void)arg; (void)wr;
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  const uint64_t start = mach_absolute_time();
  unsigned ticks = 0;
  while ((mach_absolute_time() - start) / f < wdt_deadline) {
    smc2(GunyahWdtPing, 0, 0, NULL);
    boot_reset_wdt_stats[3] = boot_reset_wdt_stats[3] + 1;
    IOSleep(GunyahWdtPingMs);
    // Every fifth 200 ms tick (1 s): one census line, so the seconds before the log goes quiet are on disk already.
    if (!(++ticks % 5)) stall_census((unsigned long long)(mach_absolute_time() / f));
  }
}
int boot_reset_watchdog(uint32_t deadline, char *text, unsigned cap) {
  uint64_t since = 0;
  const int64_t status = (int64_t)smc2(GunyahWdtStatus, 0, 0, &since);
  boot_reset_wdt_stats[0] = (int32_t)status;
  if (status != 0) {snprintf(text, cap, "watchdog: status %lld, not armed", (long long)status); return 1;}
  smc2(GunyahWdtControl, GunyahWdtDisable, 0, NULL);
  boot_reset_wdt_stats[1] = (int32_t)smc2(GunyahWdtSetTime, GunyahWdtTimeoutMs, GunyahWdtTimeoutMs, NULL);
  if (boot_reset_wdt_stats[1]) {snprintf(text, cap, "watchdog: set time %d, not armed", boot_reset_wdt_stats[1]); return 2;}
  wdt_deadline = deadline;
  thread_t thread;
  if (kernel_thread_start_priority(wdt_thread, NULL, MAXPRI_KERNEL, &thread) != KERN_SUCCESS) {snprintf(text, cap, "watchdog: no thread"); return 3;}
  thread_deallocate(thread);
  boot_reset_wdt_stats[2] = (int32_t)smc2(GunyahWdtControl, GunyahWdtEnable, 0, NULL);
  snprintf(text, cap, "watchdog: armed %d, pings until %us, bite %u ms later (last ping %llu ms ago)", boot_reset_wdt_stats[2], deadline,
           GunyahWdtTimeoutMs,
           (unsigned long long)since);
  return boot_reset_wdt_stats[2] ? 4 : 0;
}

// Writes "bootloader" into the reboot-reason cell, verifies it, then PSCI SYSTEM_RESET. Returns only when something refused (text says what);
// the cell is left as it was in every refusal before the write.
int boot_reset_bootloader(char *text, unsigned cap) {
  struct reset_maps m;
  uint8_t before = 0xff, after = 0xff;
  int rc = open_maps(&m, 1);
  if (rc) {
    snprintf(text, cap, "reset refused: open=%d find=%d", rc, m.find);
    unmap_all(&m);
    return rc;
  }
  if (m.f.owner != m.a.ee) {
    snprintf(text, cap, "reset refused: apid %d is owned by EE %d", m.f.apid, m.f.owner);
    unmap_all(&m);
    return 5;
  }
  rc = br_set_bootloader(m.obs, m.chn, &m.a, m.f.apid, &before, &after);
  snprintf(text, cap, "reset: cell %02x -> %02x rc=%d", before, after, rc);
  unmap_all(&m);
  if (rc) return 10 + rc;
  psci(PsciSystemReset, 0);
  return 99;   // SYSTEM_RESET returned
}
