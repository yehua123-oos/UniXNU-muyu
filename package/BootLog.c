// SPDX-License-Identifier: MIT
// Ported from UniXNU-sheng/package/BootLog.c for muyu (SM8635). The board-side dependencies are all weak
// imports, so the sheng-only probes resolve to null here and are skipped; the storage half is already
// present because muyu's NativeUfs provides the same bounded log window (native_ufs_log_open/write/sync,
// last kLogReserveBlocks = 8192 blocks of userdata, 288 MiB reserve). Start it with boot_log_tick(), which
// LegacyDisplayProbe calls once; it only does anything with sheng-log=1.
//
// Diagnostic log on UFS (boot argument sheng-log=1): every 2 s the text the board page would show (the diagnostic page, the live
// banner, the GPU rows, a caught panic message) and the kernel message buffer are written to the last 32 MiB of userdata through
// NativeUfs's bounded log window, so a run can be read back afterwards (scripts/read-sheng-log.py over the Linux log reader).
//
// Window layout (4096-byte blocks): 0 header, 1..32 the kernel message buffer (oldest first), 33.. text slots of 8 blocks in a ring,
// then the export (version 3, int-150): the newest complete blob that user space handed over (boot_log_export_put: the Metal plugin's
// pipeline caches, so the next image can start with them) lies in NativeUfs's export region, the 256 MiB below the window, which the
// reader fetches on its own. Its two halves alternate, so the header always names a blob that is wholly on the medium. (Version 2 kept a
// 16 MiB area at the window's tail; the UI's pipeline caches outgrew it in int-148 and int-149.)
// The header is written last in each round, so a reader never trusts a slot or an export the header does not count yet.
#include <kern/thread.h>
#include <kern/clock.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#include <IOKit/IOLib.h>

// The source-storage archive (-DNATIVE_UFS_SOURCE_API=1, the iPadOS 26+ path) always links
// BootLogStubs.o, so these seven hooks are ordinary references there. A weak reference becomes a
// chained *bind* in an arm64e kernel, and the prelink that turns our kernel into a fileset carries
// rebases across but not binds, so the seven would make the prelink refuse the kernel outright. The
// board's own build keeps the weak references: there the whole component is optional.
#if defined(NATIVE_UFS_SOURCE_API) && NATIVE_UFS_SOURCE_API
#define BOOT_LOG_OPTIONAL
#else
#define BOOT_LOG_OPTIONAL __attribute__((weak))
#endif

extern int native_ufs_log_open(uint64_t *, uint64_t *, const char **) __attribute__((weak));
extern int native_ufs_log_write(uint64_t, uint32_t, const void *) __attribute__((weak));
extern uint64_t native_ufs_export_blocks(void) __attribute__((weak));
extern int native_ufs_export_write(uint64_t, uint32_t, const void *) __attribute__((weak));
extern int native_ufs_log_sync(void) __attribute__((weak));
// What the bounded log write saw (drivers/ShengUfs: hostWrite4k gives up after 100 ms instead of sleeping on the shared host lock).
extern uint32_t native_ufs_log_lock_stalls(void) BOOT_LOG_OPTIONAL;
extern uint32_t native_ufs_log_lock_stall_ms(void) BOOT_LOG_OPTIONAL;
extern unsigned native_ufs_store_gpt_check(void) __attribute__((weak));
extern uint32_t cs_text_fixup_parsed __attribute__((weak));
extern uint32_t cs_text_fixup_applied __attribute__((weak));
extern char boot_progress_page_text[48][120] __attribute__((weak));
extern char boot_progress_banner_text[49][120] __attribute__((weak));
extern volatile unsigned boot_progress_banner_rows __attribute__((weak));
extern char boot_progress_ext[8][48] __attribute__((weak));
extern char boot_progress_ext_wide[2][112] __attribute__((weak));
extern char boot_progress_panic_text[256] __attribute__((weak));
extern volatile uint32_t native_data_persist_state __attribute__((weak));
extern volatile uint32_t native_data_persist_header __attribute__((weak));
extern volatile uint32_t native_data_persist_gpt __attribute__((weak));
extern volatile uint32_t native_data_persist_gpt_after __attribute__((weak));
extern volatile uint64_t native_data_persist_pages __attribute__((weak));
extern volatile uint64_t native_data_persist_loaded __attribute__((weak));
extern volatile uint64_t native_data_persist_writes __attribute__((weak));
extern volatile uint64_t native_data_persist_syncs __attribute__((weak));
extern volatile uint64_t native_data_persist_errors __attribute__((weak));
extern volatile uint64_t native_data_persist_first_lba __attribute__((weak));
extern volatile int native_gpu_addon_files __attribute__((weak));
extern volatile int native_gpu_addon_written __attribute__((weak));
extern volatile int native_gpu_addon_same __attribute__((weak));
extern volatile int native_gpu_addon_failed __attribute__((weak));
extern volatile int native_gpu_addon_error __attribute__((weak));
extern volatile int native_gpu_addon_stage __attribute__((weak));
extern volatile int native_gpu_addon_status __attribute__((weak));
extern volatile int native_gpu_addon2_stage __attribute__((weak));
extern volatile int native_gpu_addon2_status __attribute__((weak));
extern volatile int native_gpu_addon3_stage __attribute__((weak));
extern volatile int native_gpu_addon3_status __attribute__((weak));
extern volatile uint64_t native_data_persist_present __attribute__((weak));
extern volatile int native_staged_apps_stage __attribute__((weak));
extern volatile int32_t native_buttons_stats[10] __attribute__((weak));
extern volatile int32_t native_backlight_stats[16] __attribute__((weak));
extern volatile int native_staged_apps_dirs __attribute__((weak));
extern volatile int native_staged_apps_files __attribute__((weak));
extern volatile int native_staged_apps_mib __attribute__((weak));
extern volatile int native_staged_apps_failed __attribute__((weak));
extern volatile int native_staged_apps_error __attribute__((weak));
extern char native_staged_apps_path[96] __attribute__((weak));
extern const char *boot_progress_tag(void) BOOT_LOG_OPTIONAL;
extern unsigned boot_ui_trace_role_rows(unsigned, char [][120], unsigned, unsigned) __attribute__((weak));
extern unsigned boot_ui_trace_hot_rows(char [][120], unsigned) __attribute__((weak));
extern unsigned long vm_kernel_slide;
extern volatile uint64_t native_gop_wait_stats[7] __attribute__((weak));
extern volatile uint32_t native_adreno_fence_wait[8] __attribute__((weak));
extern volatile uint64_t native_adreno_wait_outer[4] __attribute__((weak));
extern volatile uint32_t native_adreno_export_stats[4] __attribute__((weak));
extern volatile int32_t native_touch_strokes[16][7] __attribute__((weak));
extern volatile uint32_t native_touch_stroke_count __attribute__((weak));
extern volatile uint32_t native_wifi_stats[10] __attribute__((weak));
extern volatile uint32_t native_swipe_stats[16] __attribute__((weak));
extern volatile int32_t native_touch_extent[4] __attribute__((weak));
extern volatile uint32_t native_adreno_method_stats[20][4] __attribute__((weak));
extern volatile uint64_t native_adreno_vm_phase_us[2][8] __attribute__((weak));
extern volatile uint32_t native_adreno_map_fail[7][2] __attribute__((weak));
extern volatile uint32_t native_adreno_pool_stats[8] __attribute__((weak));
extern volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak));
extern volatile uint32_t native_touch_edge_stats[4] __attribute__((weak));
extern volatile int32_t native_backlight_request[6] __attribute__((weak));
extern volatile uint64_t native_adreno_reassert_ticks __attribute__((weak));
extern volatile uint64_t native_adreno_cache_stats[5] __attribute__((weak));
// NativeAdrenoPublish.cpp: per process name that opened the GPU, and the processes that asked the Metal discovery for its plugin.
struct native_adreno_client_record {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
extern struct native_adreno_client_record native_adreno_client_log[8] __attribute__((weak));
struct native_adreno_client_text {char name[20]; char text[64];};
extern struct native_adreno_client_text native_adreno_client_text[4] __attribute__((weak));
extern volatile uint32_t native_adreno_mount_fail[4] __attribute__((weak));
extern volatile uint32_t native_adreno_window_overflow[4][3] __attribute__((weak));
extern volatile uint32_t native_adreno_client_stats[5][6] __attribute__((weak));
struct native_adreno_asker_record {char name[20]; uint32_t reads;};
extern struct native_adreno_asker_record native_adreno_metal_askers[12] __attribute__((weak));
extern char native_gpu_coherence_row[120] __attribute__((weak));
extern size_t boot_progress_profile_rows(char *, size_t) __attribute__((weak));
extern size_t boot_progress_spawn_rows(char *, size_t) __attribute__((weak));
extern volatile uint64_t native_gpu_idle_sleeps __attribute__((weak));
extern volatile uint64_t native_gpu_idle_spins __attribute__((weak));
extern volatile uint32_t native_adreno_coherent_skips[4] __attribute__((weak));
extern volatile uint32_t native_adreno_io_coherent __attribute__((weak));
extern char native_cpufreq_row[120] __attribute__((weak));
extern uint32_t native_gpu_perf_vote[2] __attribute__((weak));
extern volatile uint32_t native_adreno_pool_status __attribute__((weak));
extern volatile uint32_t native_adreno_pool_wc __attribute__((weak));
extern volatile uint64_t native_adreno_fence_phys __attribute__((weak));
extern int (*native_adreno_dram_read32)(uint64_t, uint32_t *) __attribute__((weak));
extern int (*native_adreno_cached_read32)(uint64_t, uint32_t *) __attribute__((weak));
extern volatile uint32_t native_adreno_ring_log[4][4 + 112] __attribute__((weak));
extern volatile uint32_t native_adreno_ring_log_count __attribute__((weak));
extern volatile uint32_t native_adreno_ib_log[4][4 + 28] __attribute__((weak));
extern volatile uint32_t native_adreno_ib_log_count __attribute__((weak));
extern unsigned native_legacy_properties_now(char *, unsigned) __attribute__((weak));
extern unsigned native_legacy_clients_dump(char *, unsigned) __attribute__((weak));
extern unsigned native_legacy_swap_timeline(char *, unsigned) __attribute__((weak));
extern unsigned native_legacy_prepare_report(char *, unsigned) __attribute__((weak));
extern char native_adreno_metal_rows[48][96] __attribute__((weak));
extern unsigned native_legacy_state_words(char *, unsigned) __attribute__((weak));
extern volatile uint32_t native_adreno_view_diag[6] __attribute__((weak));
extern int boot_reset_probe(char *, unsigned) BOOT_LOG_OPTIONAL;
extern int boot_reset_bootloader(char *, unsigned) BOOT_LOG_OPTIONAL;
extern int boot_reset_mark_bootloader(char *, unsigned) BOOT_LOG_OPTIONAL;
extern int boot_reset_watchdog(uint32_t, char *, unsigned) BOOT_LOG_OPTIONAL;
// 1 = the watchdog's ping thread was cut short (this board); 0 = no watchdog thread, so the caller must use PSCI or nothing.
extern int boot_reset_wdt_hurry(void) BOOT_LOG_OPTIONAL;
// fix159: one explicit Gunyah ping from the per-second tick thread; 0 = no watchdog glue on this board.
extern int boot_reset_wdt_ping(void) BOOT_LOG_OPTIONAL;
extern volatile uint64_t native_adreno_import_diag[10] __attribute__((weak));
extern volatile uint32_t native_adreno_import_stage __attribute__((weak));
extern volatile uint64_t native_adreno_import_live[3 + 8 * 4] __attribute__((weak));
extern volatile uint32_t native_adreno_import_persistent[2] __attribute__((weak));
extern volatile uint64_t native_adreno_import_fail_pa __attribute__((weak)), native_adreno_import_fail_offset __attribute__((weak));
// What the reset probe and the autoreset attempt saw, for the [reset] section of the log.
static char boot_log_reset_probe_text[240], boot_log_reset_result[120], boot_log_reset_mark[64], boot_log_reset_watchdog[120];
// The kernel message buffer (bsd/sys/msgbuf.h); only these leading fields are read.
struct boot_log_msgbuf {int msg_magic, msg_size, msg_bufx, msg_bufr; char *msg_bufc;};
extern struct boot_log_msgbuf *msgbufp;

// LogMsgBlocks is the kernel message buffer's capacity in the window: 256 blocks = 1 MiB, the largest the BSD ring can be
// (bsd/sys/msgbuf.h MAX_MSG_BSIZE, subr_log.c log_setsize). The boot argument msgbuf=1048576 raises the ring to match;
// without it the ring stays at its default and only the tail of that fits, which is exactly what a 32-block window held.
enum {LogBlock = 4096, LogMsgFirst = 1, LogMsgBlocks = 256, LogSlotFirst = 257, LogSlotBlocks = 16,
      LogPeriodMs = 2000,
      // 3 was for a dead medium. With bounded writes a give-up is now also what a busy UFS looks like, and three of those in a row
      // would end the record exactly when the system got interesting: 30 consecutive failures (a minute) still means dead.
      LogMaxErrors = 30, LogExportStage = 64};   // export bytes staged per write: 256 KiB
struct boot_log_header {
  char magic[8];                 // "SHLOG001"
  uint32_t version, header_bytes;
  uint64_t boot_id;              // CNTPCT when the writer started: tells one run from the next
  uint64_t first_lba;            // absolute 4096-byte LBA of block 0 on LUN 0
  uint32_t window_blocks, msg_first, msg_blocks, msg_bytes, slot_first, slot_blocks, slot_count, slots_written;
  uint32_t uptime_s, rounds, write_errors, gpt_open, gpt_last, state;
  char tag[32];
  // version 3: the newest complete export at block export_first of the export region (export_blocks long, ending export_below blocks
  // below block 0 of this window); export_bytes 0 = nothing exported yet
  uint32_t export_first, export_blocks, export_bytes, export_generation;
  uint32_t export_below;
  // version 4: the diagnostic file export (BootDiagExport.c) at block diag_first of the export region, diag_bytes long (0 = none)
  uint32_t diag_first, diag_bytes;
  // version 5. uptime_s above counts from the writer thread's start, and that thread does not run until the kernel is ~90 s in
  // (NativeUfs has to match first), so it is not the number the msgbuf timestamps use. kernel_uptime_s is that number: the
  // same clock as the "[ 123.456789]:" prefixes, so "log covers up to kernel_uptime_s" is now a statement about the kernel.
  // msg_blocks is the number of blocks actually written this round (the tail of the region holds an older, longer round).
  uint32_t kernel_uptime_s;
  // Milliseconds the round spent on the medium, last and worst. A round that blocks for tens of seconds is why the log
  // stopped at 174 s on fix25 while the device kept running: the writer was inside a write, not dead.
  uint32_t write_ms_last, write_ms_max;
};
// Writer state for the board page: 0 off, 1 waiting for the window, 2 writing, 3 stopped after errors, 4 stopped (GPT changed).
volatile uint32_t boot_log_state, boot_log_rounds, boot_log_errors;
volatile uint64_t boot_log_first_lba;
static int boot_log_started;
// Shared by the writer loop and boot_log_tick. fix10 showed why the deadline cannot live in the loop alone: the medium writes stopped at
// 122 s and the next round blocked until 183 s, so a 90 s deadline fired 93 s late. boot_log_tick runs once a second from the board page's
// monitor, which does not depend on the medium, so it is the reliable place to notice the deadline.
static uint32_t boot_log_autoreset;
// fix159: the per-second tick thread reads tried/ok from another thread, so they must not be cached.
static volatile int boot_log_reset_ok, boot_log_reset_tried;
static uint32_t boot_log_tick_count;   // diagnostic: how many times boot_log_tick has run
static int boot_log_tick_started;      // 1 = the per-second tick thread exists, -1 = creation failed
static uint8_t *boot_log_buffer;   // LogMsgBlocks blocks: the message buffer, then reused for a slot and the header
static uint32_t boot_log_seen_stalls;   // the driver's give-up count last reported into msgbuf

// The export: boot_log_export_put takes the blob's pieces in order (any thread) and writes them through a staging buffer straight into
// the half of the export region that does not hold the last complete blob; once the last piece is on the medium that blob becomes the
// complete one, and the writer's next header names it. The lock serialises pieces and guards the counters.
static IOLock *export_lock;
static uint8_t *export_stage;
static uint32_t export_region;   // blocks of the export region, 0 = none
static uint32_t export_total, export_filled, export_generation, export_target, export_ready, export_ready_bytes, export_ready_first;
// The diagnostic file export takes the half the ready blob does not hold; from then on the plugin's exports are refused (-7), so neither
// overwrites the other. diag_status: 0 not asked, 1 running, 2 done, negative failed.
static uint32_t diag_first, diag_bytes;
static int diag_owned;
volatile int boot_diag_status;
extern volatile uint32_t boot_diag_files __attribute__((weak));
extern volatile uint32_t boot_diag_skipped __attribute__((weak));
extern volatile uint32_t boot_diag_errors __attribute__((weak));
extern uint64_t boot_diag_scan(uint64_t) BOOT_LOG_OPTIONAL;
extern int boot_diag_stream(int (*)(const void *, uint32_t, void *), void *) BOOT_LOG_OPTIONAL;
int boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset, const void *data, uint32_t size) {
  if (!export_lock || !generation || !total || !data || !size || offset > total || size > total - offset) return -1;
  IOLockLock(export_lock);
  int status = 0;
  const uint32_t half = export_region / 2, stage = LogExportStage * LogBlock;
  if (!half || !&native_ufs_export_write) status = -4;
  else if (total > (uint64_t)half * LogBlock) status = -5;
  else if (diag_owned) status = -7;
  if (!status && !export_stage) export_stage = (uint8_t *)IOMallocAligned(stage, LogBlock);
  if (!status && !export_stage) status = -2;
  if (!status && offset == 0) {
    export_generation = generation; export_total = total; export_filled = 0;
    export_target = export_ready && export_ready_first == 0 ? half : 0;
  }
  if (!status && (generation != export_generation || total != export_total || offset != export_filled)) status = -3;   // only in order
  const uint8_t *from = (const uint8_t *)data;
  while (!status && size) {
    const uint32_t at = export_filled % stage, n = size < stage - at ? size : stage - at;
    memcpy(export_stage + at, from, n);
    export_filled += n; from += n; size -= n;
    const int full = at + n == stage, done = export_filled == export_total;
    if (full || done) {
      const uint32_t used = at + n, blocks = (used + LogBlock - 1) / LogBlock;
      if (used % LogBlock) memset(export_stage + used, 0, blocks * LogBlock - used);
      if (!native_ufs_export_write(export_target + (export_filled - used) / LogBlock, blocks, export_stage)) {
        status = -6; export_generation = 0;   // the next generation starts over
      }
    }
    if (!status && done) {export_ready = export_generation; export_ready_bytes = export_total; export_ready_first = export_target;}
  }
  IOLockUnlock(export_lock);
  return status;
}

// The diagnostic export's writer: the same staging as boot_log_export_put, into its own half.
struct diag_sink {uint8_t *stage; uint32_t target, filled;};
static int diag_flush(struct diag_sink *k) {
  const uint32_t stage = LogExportStage * LogBlock, used = k->filled % stage ? k->filled % stage : stage;
  const uint32_t blocks = (used + LogBlock - 1) / LogBlock;
  if (used % LogBlock) memset(k->stage + used, 0, blocks * LogBlock - used);
  return native_ufs_export_write(k->target + (k->filled - used) / LogBlock, blocks, k->stage) ? 0 : -6;
}
static int diag_emit(const void *data, uint32_t size, void *arg) {
  struct diag_sink *k = arg;
  const uint32_t stage = LogExportStage * LogBlock;
  const uint8_t *from = data;
  while (size) {
    const uint32_t at = k->filled % stage, n = size < stage - at ? size : stage - at;
    memcpy(k->stage + at, from, n);
    k->filled += n; from += n; size -= n;
    if (at + n == stage && diag_flush(k)) return -6;
  }
  return 0;
}
static void diag_thread(void *arg, wait_result_t wait) {
  (void)arg; (void)wait;
  IOLockLock(export_lock);
  const uint32_t half = export_region / 2;
  struct diag_sink k = {NULL, export_ready && export_ready_first == 0 ? half : 0, 0};
  diag_owned = half != 0;
  IOLockUnlock(export_lock);
  int status = -4;
  if (half && &native_ufs_export_write && (k.stage = (uint8_t *)IOMallocAligned(LogExportStage * LogBlock, LogBlock))) {
    const uint64_t stream = boot_diag_scan((uint64_t)half * LogBlock - 16);
    const uint32_t head[4] = {0, 0, (uint32_t)stream, 0};
    if (!stream) status = -8;
    else {
      status = diag_emit("SHEXR001", 8, &k);
      if (!status) status = diag_emit(&head[2], 8, &k);
      if (!status) status = boot_diag_stream(diag_emit, &k);
      if (!status && k.filled % (LogExportStage * LogBlock)) status = diag_flush(&k);
      if (!status && k.filled != stream + 16) status = -9;
    }
    IOFreeAligned(k.stage, LogExportStage * LogBlock);
  }
  IOLockLock(export_lock);
  if (!status) {diag_first = k.target; diag_bytes = k.filled;}
  IOLockUnlock(export_lock);
  boot_diag_status = status ? status : 2;
}

static uint64_t log_ticks(void) {uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v)); return v;}

// Appends a text row without its trailing blanks; returns the new length (never past cap - 1).
static size_t put(char *out, size_t at, size_t cap, const char *text, size_t max) {
  size_t n = strnlen(text, max);
  while (n && (text[n - 1] == ' ' || text[n - 1] == 0)) --n;
  for (size_t i = 0; i < n && at + 2 < cap; ++i) {char c = text[i]; out[at++] = (c >= 32 && c < 127) ? c : '?';}
  if (at + 1 < cap) out[at++] = '\n';
  return at;
}

static size_t compose(char *out, size_t cap, uint32_t slot, unsigned now) {
  /* muyu: the sheng rows gathered here (diagnostic page, banner, GPU, touch, Wi-Fi, backlight, staged apps)
   * have no counterpart on this board, and every one of them was a strong reference to a sheng-only symbol.
   * The kernel message buffer is written to its own blocks by the writer, so the text slot only needs the
   * marker and the run time; read the rows from msgbuf.txt when decoding. */
  size_t at = 0;
  char head[96];
  snprintf(head, sizeof head, "#slot %u t=%us muyu", slot, now);
  at = put(out, at, cap, head, sizeof head);
  return at;
}

// The message buffer, oldest byte first, NUL-padded to LogMsgBlocks blocks; returns the bytes copied.
static uint32_t copy_msgbuf(uint8_t *out) {
  memset(out, 0, LogMsgBlocks * LogBlock);
  struct boot_log_msgbuf *m = msgbufp;
  if (!m || !m->msg_bufc || m->msg_size <= 0) return 0;
  int size = m->msg_size, x = m->msg_bufx;
  if (x < 0 || x >= size) x = 0;
  uint32_t n = 0;
  for (int i = 0; i < size && n < LogMsgBlocks * LogBlock; ++i) {
    char c = m->msg_bufc[(x + i) % size];
    if (c) out[n++] = (uint8_t)c;
  }
  return n;
}

// The clock behind the msgbuf "[ 123.456789]:" prefixes. The board page tags every kprintf line with CNTPCT/cntfrq, which counts from
// power-on (UEFI included), so this must be the raw counter and not clock_get_uptime: that one starts at the kernel's own boot and
// reported 3 s on fix26 while the same round's msgbuf already reached 174 s. With this, header and msgbuf are directly comparable.
static uint32_t kernel_uptime_s(void) {
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  uint64_t c = 0; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(c));
  return (uint32_t)(c / f);
}

static void writer(void *arg, wait_result_t result) {
  (void)arg; (void)result;
  thread_set_thread_name(current_thread(), "boot_log");
  uint64_t first = 0, blocks = 0;
  const char *why = "";
  boot_log_state = 1;
  // The window exists once NativeUfs has read the root layout; until then the open is refused ("no root layout").
  while (!native_ufs_log_open(&first, &blocks, &why)) {
    if (strcmp(why, "no root layout")) {boot_log_state = 3; return;}
    IOSleep(1000);
  }
  boot_log_first_lba = first;
  const uint32_t slot_count = (uint32_t)((blocks - LogSlotFirst) / LogSlotBlocks);
  if (&native_ufs_export_blocks && &native_ufs_export_write) {
    IOLockLock(export_lock);
    export_region = (uint32_t)native_ufs_export_blocks();
    IOLockUnlock(export_lock);
  }
  struct boot_log_header h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, "SHLOG001", 8);
  h.version = 5; h.header_bytes = sizeof h;
  h.export_blocks = export_region; h.export_below = export_region; h.boot_id = log_ticks(); h.first_lba = first; h.window_blocks = (uint32_t)blocks;
  h.msg_first = LogMsgFirst; h.msg_blocks = LogMsgBlocks; h.slot_first = LogSlotFirst; h.slot_blocks = LogSlotBlocks; h.slot_count = slot_count;
  h.gpt_open = h.gpt_last = &native_ufs_store_gpt_check ? native_ufs_store_gpt_check() : 0xff;
  if (&boot_progress_tag) strlcpy(h.tag, boot_progress_tag(), sizeof h.tag);
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  const uint64_t start = log_ticks();
  // The reboot-into-fastboot path, read only: PSCI features and the PMIC cell. sheng-autoreset=<seconds of log time> then writes the cell and
  // restarts once, after that round's log is on the medium, but only when the probe found the cell writable. The probe issues this kernel's
  // first SMC calls, so it runs from the loop after a round is on the medium (see reset_probed below) rather than here: if the call hangs,
  // the log up to that point still exists and the missing [reset] line is itself the answer.
  int reset_probed = 0;
  // sheng-diag-export=<seconds of log time>: export the diagnostic files once then (set it shortly before sheng-autoreset).
  uint32_t diag_at = 0;
  if (&boot_diag_scan && &boot_diag_stream) PE_parse_boot_argn("sheng-diag-export", &diag_at, sizeof diag_at);
  uint32_t errors = 0;
  boot_log_state = 2;
  for (;;) {
    const uint64_t round_start = log_ticks();
    const unsigned now = (unsigned)((round_start - start) / f);
    int ok = 1;
    // Text slot, then the message buffer, then the header that makes both visible.
    memset(boot_log_buffer, 0, LogSlotBlocks * LogBlock);
    compose((char *)boot_log_buffer, LogSlotBlocks * LogBlock, h.slots_written, now);
    ok &= native_ufs_log_write(LogSlotFirst + (uint64_t)(h.slots_written % slot_count) * LogSlotBlocks, LogSlotBlocks, boot_log_buffer);
    h.msg_bytes = copy_msgbuf(boot_log_buffer);
    // Only the blocks that hold text. A fixed 1 MiB every 2 s is 512 KiB/s through this emulated UFS, and on fix25 the round after
    // the 174 s mark never came back from a write, which is what ended the record while the device kept running. The first round
    // still writes the whole region so a longer earlier round's tail cannot be read as this one's.
    const uint32_t used_blocks = (h.msg_bytes + LogBlock - 1) / LogBlock;
    h.msg_blocks = boot_log_rounds || !used_blocks ? LogMsgBlocks : used_blocks;
    ok &= native_ufs_log_write(LogMsgFirst, h.msg_blocks, boot_log_buffer);
    if (ok) ++h.slots_written;
    else ++h.write_errors;
    // What the medium cost, before the header goes down: a round that blocks for tens of seconds shows up here as write_ms_max.
    const uint32_t spent_ms = (uint32_t)((log_ticks() - round_start) / (f / 1000));
    h.write_ms_last = spent_ms;
    if (spent_ms > h.write_ms_max) h.write_ms_max = spent_ms;
    // Whether this round lost the host lock to the guest's own I/O, which is the other explanation for a record that stops early.
    if (&native_ufs_log_lock_stalls) {
      const uint32_t stalls = native_ufs_log_lock_stalls();
      if (stalls != boot_log_seen_stalls) {
        boot_log_seen_stalls = stalls;
        printf("[log] ufs lock: %u give-ups, worst wait %u ms\n", stalls,
               &native_ufs_log_lock_stall_ms ? native_ufs_log_lock_stall_ms() : 0);
      }
    }
    // The newest complete export is already on the medium; the header names it.
    IOLockLock(export_lock);
    h.export_generation = export_ready; h.export_bytes = export_ready_bytes; h.export_first = export_ready_first;
    h.diag_first = diag_first; h.diag_bytes = diag_bytes;
    IOLockUnlock(export_lock);
    if (diag_at && now >= diag_at && !boot_diag_status) {
      thread_t diag;
      boot_diag_status = 1;
      if (kernel_thread_start_priority(diag_thread, NULL, MINPRI_KERNEL, &diag) == KERN_SUCCESS) thread_deallocate(diag);
      else boot_diag_status = -10;
    }
    // uptime_s counts from this thread's start; kernel_uptime_s is the same clock the msgbuf timestamps use, so a reader can say
    // how far into the kernel's life this snapshot reaches without knowing when NativeUfs happened to match.
    h.uptime_s = now; h.kernel_uptime_s = kernel_uptime_s();
    h.rounds = ++boot_log_rounds; h.state = boot_log_state;
    // Every 30 rounds the GPT is read back; any change stops the writer for good.
    if (&native_ufs_store_gpt_check && !(h.rounds % 30)) h.gpt_last = native_ufs_store_gpt_check();
    memset(boot_log_buffer, 0, LogBlock);
    memcpy(boot_log_buffer, &h, sizeof h);
    ok &= native_ufs_log_write(0, 1, boot_log_buffer);
    ok &= native_ufs_log_sync();
    if (!ok) {boot_log_errors = ++errors;} else errors = 0;
    if (errors >= LogMaxErrors) {boot_log_state = 3; return;}
    if (h.gpt_last != h.gpt_open) {boot_log_state = 4; return;}
    // One round is on the medium, so the reset probe can run without cost to the record: it only reads (PSCI version, the arbiter's
    // version and the SDAM's APID owner) and writes nothing. The autoreset trigger below then has its answer in the same round.
    if (!reset_probed && h.rounds >= 1) {
      reset_probed = 1;
      if (&boot_reset_probe) {
        boot_log_reset_ok = boot_reset_probe(boot_log_reset_probe_text, sizeof boot_log_reset_probe_text) == 0;
        printf("[reset] probe: %s\n", boot_log_reset_probe_text);
        PE_parse_boot_argn("sheng-autoreset", &boot_log_autoreset, sizeof boot_log_autoreset);
      }
    }
    // boot_log_tick usually fires this first (it is not blocked by the medium); this copy stays for the boots where the monitor's tick
    // is the one that stalls, and boot_log_reset_tried keeps the two from both running.
    if (boot_log_autoreset && now >= boot_log_autoreset && !boot_log_reset_tried && ok) {
      boot_log_reset_tried = 1;
      if (!boot_log_reset_ok) strlcpy(boot_log_reset_result, "autoreset skipped: the probe found no writable cell", sizeof boot_log_reset_result);
      else if (&boot_reset_wdt_hurry && boot_reset_wdt_hurry()) {
        // The 2026-11-13 probe settled it: PSCI is v1.1 here but SYSTEM_RESET and both SYSTEM_RESET2 variants answer 0, so
        // boot_reset_bootloader() would write the cell and then call an SMC that does nothing. The restart belongs to the
        // hypervisor watchdog armed by boot_log_tick, whose timeout boot_reset_wdt_hurry pulls down to 1 s.
        strlcpy(boot_log_reset_result, "autoreset: pings stopped, the watchdog restarts", sizeof boot_log_reset_result);
      } else {
        // Boards whose PSCI does implement SYSTEM_RESET keep the direct path.
        int rc = boot_reset_bootloader(boot_log_reset_result, sizeof boot_log_reset_result);
        char tail[24]; snprintf(tail, sizeof tail, " (returned %d)", rc);
        strlcat(boot_log_reset_result, tail, sizeof boot_log_reset_result);
      }
      // Into msgbuf as well as the struct: the boots of 2026-11-13 ended before anything read the struct back.
      printf("[reset] %s\n", boot_log_reset_result);
    }
    IOSleep(LogPeriodMs);
  }
}

// Called by the board page's monitor once a second; starts the writer the first time when sheng-log=1.
// fix159: on this board that monitor never called again - the only call site (LegacyDisplayProbe.cpp:541)
// passes a constant 0 exactly once - so the deadline below could never fire and sheng-autoreset had no
// effect on the observation window. The per-second caller is now the dedicated thread defined below.
static void log_tick_thread(void *, wait_result_t);
void boot_log_tick(unsigned now) {
  // fix159 diagnostic: prove the per-second caller is alive and show which clock the deadline sees.
  boot_log_tick_count = boot_log_tick_count + 1u;
  if ((boot_log_tick_count % 30u) == 0u)
    printf("[log] tick %us n=%u deadline=%u tried=%d\n", now, boot_log_tick_count,
           boot_log_autoreset, boot_log_reset_tried);
  // The deadline is watched here first: this runs once a second from the board page's monitor, which does not touch the medium, and on
  // fix10 the writer loop's own copy of the same check fired 93 s late because the writes had stopped and the round had blocked.
  if (boot_log_autoreset && now >= boot_log_autoreset && !boot_log_reset_tried && boot_log_reset_ok) {
    boot_log_reset_tried = 1;
    if (&boot_reset_wdt_hurry && boot_reset_wdt_hurry()) {
      strlcpy(boot_log_reset_result, "autoreset: pings stopped, the watchdog restarts", sizeof boot_log_reset_result);
      printf("[reset] %s (tick %us)\n", boot_log_reset_result, now);
    }
  }
  if (boot_log_started) return;
  boot_log_started = 1;
  // An unattended run must come back however it ends: on the desktop past its deadline (int-229 first suspected) or in a panic seconds
  // after the start, before UFS and this log exist (int-229: a duplicate class at PE_lockdown_iokit). So the first tick, independent of
  // sheng-log, marks the cell (any restart is fastboot) and arms the hypervisor's watchdog, which bites a minute after the pings stop:
  // after the autoreset deadline, or when a panic parks the cores.
  if (PE_parse_boot_argn("sheng-autoreset", &boot_log_autoreset, sizeof boot_log_autoreset) && boot_log_autoreset && &boot_reset_probe) {
    boot_log_reset_ok = boot_reset_probe(boot_log_reset_probe_text, sizeof boot_log_reset_probe_text) == 0;
    if (boot_log_reset_ok) {
      if (&boot_reset_mark_bootloader) boot_reset_mark_bootloader(boot_log_reset_mark, sizeof boot_log_reset_mark);
      if (&boot_reset_watchdog) boot_reset_watchdog(boot_log_autoreset, boot_log_reset_watchdog, sizeof boot_log_reset_watchdog);
      // fix166: these two texts were composed but never printed, so a refused set-time was indistinguishable from an external reset.
      // Output only - no behaviour, no ordering change; the sole behavioural variable of this round stays GunyahWdtTimeoutMs.
      printf("[reset] mark: %s | watchdog: %s\n", boot_log_reset_mark, boot_log_reset_watchdog);
    }
  }
  // fix159: start the per-second caller here, after the watchdog is armed but before the sheng-log
  // early returns below (a build without UFS logging still needs the autoreset deadline to work).
  if (!boot_log_tick_started && &boot_reset_wdt_ping) {
    boot_log_tick_started = 1;
    thread_t tick_thread;
    if (kernel_thread_start_priority(log_tick_thread, NULL, BASEPRI_REALTIME, &tick_thread) == KERN_SUCCESS)
      thread_deallocate(tick_thread);
    else boot_log_tick_started = -1;
  }
  uint32_t mode = 0;
  if (!PE_parse_boot_argn("sheng-log", &mode, sizeof mode) || mode != 1) return;
  if (!&native_ufs_log_open || !&native_ufs_log_write || !&native_ufs_log_sync) return;
  // 1 MiB at the time this runs (the UFS driver is up, so the allocator has pages); keep a plain-contiguous fallback so a
  // failed aligned allocation cannot cost us the log entirely - the writer only memcpy's out of this buffer.
  boot_log_buffer = (uint8_t *)IOMallocAligned(LogMsgBlocks * LogBlock, LogBlock);
  if (!boot_log_buffer) boot_log_buffer = (uint8_t *)IOMalloc(LogMsgBlocks * LogBlock);
  if (!boot_log_buffer) return;
  export_lock = IOLockAlloc();
  thread_t thread;
  // BASEPRI_REALTIME, not the kernel default: fix26 measured the medium at 3 ms a round (write_ms=3/9), so the writer was not slow, it
  // stopped being run. Once SpringBoard starts spinning on this emulated CPU the round never comes back and the record ends at 174 s
  // while the device runs to 203 s. A realtime thread asleep 2 s out of every 2.01 leaves the system alone and still gets the turn.
  if (kernel_thread_start_priority(writer, 0, BASEPRI_REALTIME, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}

// fix159: the "board page monitor calls boot_log_tick once a second" that BootLog.c has always assumed.
// It uses the raw CNTPCT/cntfrq clock - the same one behind the msgbuf "[ 123.456789]:" prefixes - so
// sheng-autoreset=<seconds of log time> means what it says, independent of when NativeUfs happened to
// publish the window. The hypervisor's watchdog timeout is only 1 s, so ping every 200 ms (5x margin,
// the same period the original wdt_thread uses) and only call boot_log_tick once a second. Once the
// deadline has fired, boot_log_reset_tried stops the pings and wdt_hurry() pulls the timeout to 1 s,
// so the restart follows the deadline instead of the ~178 s ping-thread stall seen in fix157/fix158.
static void log_tick_thread(void *arg, wait_result_t wr) {
  (void)arg; (void)wr;
  thread_set_thread_name(current_thread(), "boot_log_tick");
  unsigned n = 0;
  for (;;) {
    if (!boot_log_reset_tried && &boot_reset_wdt_ping) boot_reset_wdt_ping();
    n = n + 1u;
    if (n >= 5u) {n = 0; boot_log_tick(kernel_uptime_s());}
    IOSleep(200);
  }
}
