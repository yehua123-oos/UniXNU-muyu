// SPDX-License-Identifier: MIT
// Keep a persistent dump of the kernel message buffer in the reserved pram.
// The low half keeps the embedded-panic FUNK record that the next XNU boot
// prints; the high half is a ramoops-compatible console ring
// (persistent_ram_buffer, sig 0x43474244) that a reader can expose at
// /sys/fs/pstore/console-ramoops-0. Enabled with sheng-log=1. Only writes the
// pram mapping the device tree already reserved; it never allocates, changes
// translation, or touches a device register.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <sys/appleapiopts.h>
#include <mach/vm_types.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/sched.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <sys/msgbuf.h>
#include <IOKit/IOLib.h>

extern vm_offset_t gPanicBase;
extern unsigned int gPanicSize;

#define BRAVEARK_LAST_LOG_INTERVAL_MS 500

// The 4 MiB pram region is split in two halves. The low half keeps the
// embedded-panic FUNK record for the XNU console; the high half is a Linux
// ramoops console zone (persistent_ram_buffer, no ECC), which the uke Linux
// ramoops backend exposes at /sys/fs/pstore/console-ramoops-0.
#define BRAVEARK_FUNK_LOG_SIZE   0x200000u
#define BRAVEARK_RAMOOPS_OFFSET  0x200000u
#define BRAVEARK_RAMOOPS_SIZE    0x200000u
#define BRAVEARK_RAMOOPS_MAGIC   0x43474244u /* "DBGC", see Linux pstore_ram.h */

struct braveark_ramoops_buffer {
	uint32_t sig;
	uint32_t start;
	uint32_t size;
	uint8_t data[];
};

static uint32_t braveark_ramoops_last_write;
static int braveark_ramoops_initialized;

static void
braveark_ramoops_append(struct braveark_ramoops_buffer *buffer, size_t capacity,
    const uint8_t *src, size_t count)
{
	size_t start, size, first;

	if (count == 0) {
		return;
	}
	if (count > capacity) {
		src += count - capacity;
		count = capacity;
	}
	start = buffer->start;
	size = buffer->size;
	if (start >= capacity) {
		start = 0;
	}
	if (size > capacity) {
		size = capacity;
	}
	first = capacity - start;
	if (first > count) {
		first = count;
	}
	memcpy(buffer->data + start, src, first);
	if (count > first) {
		memcpy(buffer->data, src + first, count - first);
	}
	size += count;
	if (size > capacity) {
		size = capacity;
	}
	start = (start + count) % capacity;
	// The Linux reader runs after a reset, but publish the bytes before the
	// header so a concurrent reader never sees a size that covers stale data.
	__asm__ volatile("dsb sy" ::: "memory");
	buffer->size = (uint32_t)size;
	buffer->start = (uint32_t)start;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
braveark_ramoops_write(struct msgbuf *m)
{
	struct braveark_ramoops_buffer *buffer;
	size_t capacity, size, start, first, tail;
	int write;

	if (gPanicBase == 0 || gPanicSize < BRAVEARK_RAMOOPS_OFFSET + BRAVEARK_RAMOOPS_SIZE) {
		return;
	}
	buffer = (struct braveark_ramoops_buffer *)(uintptr_t)(gPanicBase + BRAVEARK_RAMOOPS_OFFSET);
	capacity = BRAVEARK_RAMOOPS_SIZE - sizeof(*buffer);
	write = m->msg_bufx;
	if (write <= 0) {
		return;
	}
	size = (size_t)m->msg_size;
	if (!braveark_ramoops_initialized) {
		buffer->sig = BRAVEARK_RAMOOPS_MAGIC;
		buffer->start = 0;
		buffer->size = 0;
		__asm__ volatile("dsb sy" ::: "memory");
		braveark_ramoops_initialized = 1;
		// Seed the ring with the current msgbuf tail: the display/root work
		// runs before the first 500 ms tick, so a delta-only start would lose
		// the interesting beginning of the boot.
		tail = capacity;
		if (tail > size) {
			tail = size;
		}
		if (tail > (size_t)write) {
			tail = (size_t)write;
		}
		start = ((size_t)write + size - tail) % size;
		first = size - start;
		if (first > tail) {
			first = tail;
		}
		braveark_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + start, first);
		if (tail > first) {
			braveark_ramoops_append(buffer, capacity, (const uint8_t *)m->msg_bufc,
			    tail - first);
		}
		braveark_ramoops_last_write = (uint32_t)write;
		return;
	}
	if ((uint32_t)write == braveark_ramoops_last_write) {
		return;
	}
	if ((uint32_t)write > braveark_ramoops_last_write) {
		braveark_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + braveark_ramoops_last_write,
		    (size_t)write - braveark_ramoops_last_write);
	} else {
		braveark_ramoops_append(buffer, capacity,
		    (const uint8_t *)m->msg_bufc + braveark_ramoops_last_write,
		    size - braveark_ramoops_last_write);
		braveark_ramoops_append(buffer, capacity, (const uint8_t *)m->msg_bufc,
		    (size_t)write);
	}
	braveark_ramoops_last_write = (uint32_t)write;
}

static void
braveark_last_log_write(void)
{
	struct embedded_panic_header *hdr;
	struct msgbuf *m = msgbufp;
	size_t size, cap, start, first;
	int write;

	if (gPanicBase == 0 || gPanicSize <= sizeof(struct embedded_panic_header) + 1) {
		return;
	}
	if (m == NULL || m->msg_bufc == NULL || m->msg_size <= 1 || m->msg_magic != MSG_MAGIC) {
		return;
	}
	write = m->msg_bufx;
	if (write <= 0) {
		return;
	}

	hdr = (struct embedded_panic_header *)(uintptr_t)gPanicBase;
	size = (size_t)m->msg_size;
	cap = BRAVEARK_FUNK_LOG_SIZE - sizeof(*hdr) - 1;
	if (cap > size) {
		cap = size;
	}

	// msg_bufr only advances when a reader drains the log, so it may point at
	// stale bytes after the ring wraps. The newest cap bytes before msg_bufx
	// are the ones a reset needs.
	start = ((size_t)write + size - cap) % size;
	first = size - start;
	if (first > cap) {
		first = cap;
	}
	memcpy((void *)(hdr + 1), m->msg_bufc + start, first);
	if (cap > first) {
		memcpy((char *)(hdr + 1) + first, m->msg_bufc, cap - first);
	}
	((char *)(hdr + 1))[cap] = '\0';

	hdr->eph_magic = EMBEDDED_PANIC_MAGIC;
	hdr->eph_crc = 0;
	hdr->eph_version = EMBEDDED_PANIC_HEADER_CURRENT_VERSION;
	hdr->eph_panic_flags = 0;
	hdr->eph_panic_log_offset = (uint32_t)sizeof(*hdr);
	hdr->eph_panic_log_len = (uint32_t)cap;
	hdr->eph_stackshot_offset = 0;
	hdr->eph_stackshot_len = 0;
	hdr->eph_other_log_offset = 0;
	hdr->eph_other_log_len = 0;
	__asm__ volatile("dsb sy" ::: "memory");

	braveark_ramoops_write(m);
}

/* Filtered snapshot of the live message buffer, shared by the timed dump, the marker dump and the
 * critical-exit hook. Returns the number of bytes written to out (always NUL terminated). */
#define MUYU_FLAT_BYTES 65536
static char muyu_flat[MUYU_FLAT_BYTES];

static int
muyu_keep(const char *line, size_t len)
{
	char list[200];
	char probe[400];
	size_t copy = len < sizeof(probe) - 1 ? len : sizeof(probe) - 1;
	const char *token;

	if (!PE_parse_boot_argn("muyu-dump-keep", list, (int)sizeof(list))) {
		return -1;
	}
	memcpy(probe, line, copy);
	probe[copy] = '\0';
	for (token = list; *token;) {
		const char *end = token;
		size_t n;
		char needle[64];

		while (*end && *end != ',') {
			end++;
		}
		n = (size_t)(end - token);
		/* The token must be terminated: searching with the rest of the comma-separated list still
		 * attached never matches (that bug made every filtered dump report 0 lines). */
		if (n > 0 && n < sizeof(needle)) {
			memcpy(needle, token, n);
			needle[n] = '\0';
			if (strnstr(probe, needle, copy) != NULL) {
				return 1;
			}
		}
		token = *end ? end + 1 : end;
	}
	return 0;
}

static int
muyu_noise(const char *line, size_t len)
{
	static const char *skip[] = { "CoreAnalyticsHub", "messageClients of", "LiveActivities",
	    "For-debug:", "temporary sandbox", "deny(1)", "apfs_gpm", "APFS: disk",
	    /* 2026-11-02: with the MUYU-only filter removed, these two filled the whole 2.4 KiB
	     * window and pushed the interesting tail (the launchd exec + exit that ends the boot)
	     * out of it. Both are high-volume and say nothing about why the boot stopped. */
	    "spaceman_", "IOPlatformProcessor_register" };
	char probe[256];
	size_t copy = len < sizeof(probe) - 1 ? len : sizeof(probe) - 1;

	memcpy(probe, line, copy);
	probe[copy] = '\0';
	for (size_t i = 0; i < sizeof(skip) / sizeof(skip[0]); i++) {
		if (strnstr(probe, skip[i], copy) != NULL) {
			return 1;
		}
	}
	return 0;
}

size_t
muyu_build_dump(char *out, size_t cap)
{
	struct msgbuf *m = msgbufp;
	size_t used = 0, at, want, size, start, first;
	if (out == NULL || cap < 2) {
		return 0;
	}
	out[0] = '\0';
	if (m == NULL || m->msg_bufc == NULL || m->msg_size <= 1 || m->msg_magic != MSG_MAGIC) {
		return 0;
	}
	size = (size_t)m->msg_size;
	want = (size_t)m->msg_bufx;
	if (want == 0) {
		return 0;
	}
	want = want < MUYU_FLAT_BYTES ? want : MUYU_FLAT_BYTES;
	start = ((size_t)m->msg_bufx + size - want) % size;
	first = size - start;
	if (first > want) {
		first = want;
	}
	memcpy(muyu_flat, m->msg_bufc + start, first);
	if (want > first) {
		memcpy(muyu_flat + first, m->msg_bufc, want - first);
	}
	at = want;
	while (at > 0 && used + 1 < cap) {
		size_t line_start = at;
		size_t len;

		while (line_start > 0 && muyu_flat[line_start - 1] != '\n') {
			line_start--;
		}
		len = at - line_start;
		while (len > 0 && (muyu_flat[line_start + len - 1] == '\r' ||
		    muyu_flat[line_start + len - 1] == '\0')) {
			len--;
		}
		if (len > 0) {
			int keep = muyu_keep(muyu_flat + line_start, len);
			if (keep == 1 || (keep < 0 && !muyu_noise(muyu_flat + line_start, len))) {
				if (used + len + 1 >= cap) {
					break;
				}
				memmove(out + len + 1, out, used);
				memcpy(out, muyu_flat + line_start, len);
				out[len] = '\n';
				used += len + 1;
			}
		}
		if (line_start == 0) {
			break;
		}
		at = line_start - 1;
	}
	/* Bring-up: the display path's counters go on top of every dump. Its prints scroll out of the
	 * window within a minute, but the counters say whether the compositor was still submitting and
	 * whether the last swap retired, and the timestamps say when the path stopped. */
	{
		extern char *native_legacy_state_line(char *, unsigned) __attribute__((weak_import));
		extern void muyu_proc_census(char *, size_t) __attribute__((weak_import));
		char line[192];
		size_t length;

		if (&muyu_proc_census != NULL) {
			muyu_proc_census(line, sizeof(line));
			length = strlen(line);
			if (length > 0 && length + used < cap) {
				memmove(out + length, out, used + 1);
				memcpy(out, line, length);
				used += length;
			}
		}
		extern void muyu_usb_state_line(char *, size_t) __attribute__((weak_import));
		if (&muyu_usb_state_line != NULL) {
			muyu_usb_state_line(line, sizeof(line));
			length = strlen(line);
			if (length > 0 && length + used < cap) {
				memmove(out + length, out, used + 1);
				memcpy(out, line, length);
				used += length;
			}
		}
		if (&native_legacy_state_line != NULL) {
			native_legacy_state_line(line, (unsigned)sizeof(line));
			length = strlen(line);
			if (length > 0 && length + used < cap) {
				memmove(out + length, out, used + 1);
				memcpy(out, line, length);
				used += length;
			}
		}
	}
	out[used] = '\0';
	return used;
}

/* Snapshot text taken when the marker first appears (the buffer wraps within seconds on this board,
 * so the text must be copied then, not after the delay). */
#define MUYU_SAVED_BYTES 2401
static char muyu_saved[MUYU_SAVED_BYTES];
static size_t muyu_saved_len;

void muyu_console_wake(void);

/* muyu-dump-on=<substring> (+ muyu-dump-after=<seconds>) fires the msgbuf dump a fixed time after that
 * substring first appears in the newest part of the buffer. Timing-based dumps miss the moment: the
 * board's boot is slow, and a panic screen drawn while SpringBoard still presents is overpainted by
 * the next frame, so the dump must wait until the display client is gone. */
static int
muyu_dump_on_seen(void)
{
	char wanted[96];
	static unsigned fired = 0;
	uint32_t after = 0;
	struct msgbuf *m = msgbufp;
	char probe[512];
	size_t size, window, start, first, copy;
	int write;

	if (fired) {
		return 0;
	}
	if (!PE_parse_boot_argn("muyu-dump-on", wanted, (int)sizeof(wanted)) || wanted[0] == '\0') {
		return 0;
	}
	if (m == NULL || m->msg_bufc == NULL || m->msg_size <= 1 || m->msg_magic != MSG_MAGIC) {
		return 0;
	}
	write = m->msg_bufx;
	if (write <= 0) {
		return 0;
	}
	size = (size_t)m->msg_size;
	window = 2048;
	if (window > (size_t)write) {
		window = (size_t)write;
	}
	start = ((size_t)write + size - window) % size;
	first = size - start;
	if (first > window) {
		first = window;
	}
	copy = first < sizeof(probe) - 1 ? first : sizeof(probe) - 1;
	memcpy(probe, m->msg_bufc + start, copy);
	probe[copy] = '\0';
	if (window > first && copy < sizeof(probe) - 1) {
		size_t more = window - first;
		if (more > sizeof(probe) - 1 - copy) {
			more = sizeof(probe) - 1 - copy;
		}
		memcpy(probe + copy, m->msg_bufc, more);
		probe[copy + more] = '\0';
	}
	if (strnstr(probe, wanted, copy) == NULL) {
		return 0;
	}
	muyu_saved_len = muyu_build_dump(muyu_saved, sizeof(muyu_saved));
	if (!PE_parse_boot_argn("muyu-dump-after", &after, (int)sizeof(after))) {
		after = 0;
	}
	if (after) {
		IOSleep(after * 1000);
	}
	fired = 1;
	return 1;
}

void muyu_panic_with_log(void);
void muyu_panic_crash_report(void);

/* Bring-up: take the panel away from the display client before the dump prints. The client keeps
 * presenting frames for minutes after the boot's last useful one, so a late dump used to be painted
 * over within a frame. See native_legacy_swap_hold. */
void
muyu_console_take_panel(void)
{
	extern volatile int native_legacy_swap_hold __attribute__((weak_import));

	if (&native_legacy_swap_hold != NULL) {
		native_legacy_swap_hold = 1;
		/* Let a swap that is already past the validator finish, so its copy cannot land on the text. */
		IOSleep(150);
	}
}

static void
braveark_last_log_writer(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;

	for (;;) {
		braveark_last_log_write();
		if (muyu_dump_on_seen()) {
			unsigned int crash = 0;

			/* muyu-dump-crash=1: userland logs live in logd and in ReportCrash's files on the
			 * Data volume, not in msgbuf, so render the newest crash report instead. */
			if (PE_parse_boot_argn("muyu-dump-crash", &crash, (int)sizeof(crash)) && crash) {
				muyu_panic_crash_report();
			}
			muyu_console_take_panel();
			muyu_console_wake();
			panic("LASTLOG %u bytes, snapshot at marker (filtered):\n%s",
			    (unsigned)muyu_saved_len, muyu_saved);
		}
		IOSleep(BRAVEARK_LAST_LOG_INTERVAL_MS);
	}
}

static void
braveark_last_log_start(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("sheng-log", &enabled, (int)sizeof(enabled)) || enabled != 1) {
		return;
	}
	if (gPanicBase == 0 || gPanicSize <= sizeof(struct embedded_panic_header) + 1) {
		return;
	}
	if (kernel_thread_start_priority(braveark_last_log_writer, NULL, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, braveark_last_log_start);


/* Bring-up: the kernel text console is parked once the render server owns the scanout (the console
 * otherwise paints over the frame buffer until the panel shows nothing but text). Every panic path
 * unparks it first, so a dump is still readable. */
void
muyu_console_wake(void)
{
	extern void vc_set_text_quiet(int) __attribute__((weak_import));

	if (&vc_set_text_quiet != NULL) {
		vc_set_text_quiet(0);
	}
}

/* Critical-exit hook: build the filtered dump and panic with it (see kern_exit.c). */
void
muyu_panic_with_log(void)
{
	static char out[MUYU_SAVED_BYTES];

	muyu_console_take_panel();
	muyu_console_wake();
	{
		/* muyu touch bring-up: run the probe here, not during the display bring-up, so a bus fault in the
		 * probe cannot cost the dump and its rows always land in the same panic text. */
		extern void native_touch_muyu_probe(void) __attribute__((weak_import));
		/* The touch frame pump (package/TouchProbeMuyu.cpp) logs a row every couple of seconds; its IOLog
		 * lines repaint the console and cover this panic screen with small text (measured 2026-10-25 21:13).
		 * Tell it to go quiet before the dump is built, so the rows it already logged survive on the panel. */
		extern volatile unsigned int native_touch_muyu_log_off __attribute__((weak_import));
		if (&native_touch_muyu_log_off != NULL) {
			native_touch_muyu_log_off = 1;
		}
		if (&native_touch_muyu_probe != NULL) {
			native_touch_muyu_probe();
		}
	}
	/* 2026-11-03: the boot tasks now fail looking for APFS disk nodes; list the ones /dev really has
	 * (only "disk*", a couple of hundred bytes) so the dump answers the question. */
	{
		extern void muyu_list_dev(void) __attribute__((weak_import));
		if (&muyu_list_dev != NULL) {
			muyu_list_dev();
		}
	}
	/* 2026-11-03: userland helpers write their failures to stderr (/dev/console); the video console
	 * only replays msgbuf, so push the console ring into the log first. It becomes the newest msgbuf
	 * lines and therefore part of the dump below - the only way to see why mount(8) exited 66. */
	{
		extern size_t muyu_console_tail(char *, size_t) __attribute__((weak_import));
		if (&muyu_console_tail != NULL) {
			static char tail[1024];
			size_t have = muyu_console_tail(tail, sizeof(tail) - 1);
			if (have > 0) {
				tail[have] = '\0';
				printf("CONSOLE TAIL (%u bytes):\n%s\n", (unsigned)have, tail);
			}
		}
	}
	muyu_build_dump(out, sizeof(out));
	panic("LASTLOG %u bytes (filtered):\n%s", (unsigned)strlen(out), out);
}

/* muyu-dump-at=<seconds>, started from load_init_program so the delay uses the calibrated clock.
 *
 * 2026-10-27 pm: this used to panic at the requested time. On this port a panic in a late boot does not
 * leave the text on the panel (the display client's frames come back, and the panic may reset), so the
 * dump was captured only by luck. It now takes the panel (native_legacy_swap_hold), unparks the text
 * console and prints the same snapshot every five seconds. Nothing paints over it, and if anything
 * still does, the next pass repaints it: the screen can be photographed at leisure, and no panic means
 * no reset and no stackshot. */
static void
muyu_dump_later(void *arg, wait_result_t result)
{
	uint32_t at = (uint32_t)(uintptr_t)arg;
	static char out[MUYU_SAVED_BYTES];
	(void)result;

	IOSleep(at * 1000);
	muyu_console_take_panel();
	muyu_console_wake();
	{
		/* Same quieting the panic path does, so the touch pump's rows cannot push the dump off. */
		extern volatile unsigned int native_touch_muyu_log_off __attribute__((weak_import));
		extern void native_touch_muyu_probe(void) __attribute__((weak_import));
		if (&native_touch_muyu_log_off != NULL) {
			native_touch_muyu_log_off = 1;
		}
		if (&native_touch_muyu_probe != NULL) {
			native_touch_muyu_probe();
		}
	}
	muyu_build_dump(out, sizeof(out));
	printf("LASTLOG %u bytes (filtered):\n%s\n", (unsigned)strlen(out), out);
	/* Bring-up measurement: after the one-shot snapshot, one short line every five seconds. This page is
	 * the instrument for the physical buttons. native_buttons_stats[] is otherwise only visible as the
	 * "MUYU BTN:" lines printed during the first seconds of the boot, which have scrolled out of the
	 * framebuffer console long before anyone can read them, so a driver that armed nothing and a driver
	 * that works both looked like "no reaction". Pressing a button must move irq/rep and t must keep
	 * climbing, so one photo of this screen is evidence either way.
	 * Rebuilding the whole dump inside this loop was tried and rejected: it appends ~30 lines per pass and
	 * the console scrolls the page off faster than it can be photographed, so the passes are one line. */
	for (unsigned pass = 1;; ++pass) {
		extern volatile int native_buttons_stats[10] __attribute__((weak_import));
		extern unsigned native_legacy_uptime_seconds(void) __attribute__((weak_import));
		extern void muyu_proc_census(char *, size_t) __attribute__((weak_import));
		/* 2026-11-11 GPU hand-off observation (GPU-ONDEVICE.md §13). calls/refused
		 * count the 17-line bridge in LegacyDisplayProbe.cpp; offers/completions/
		 * failures are NativeGOPPresenter's counters. offers>0 means the display is
		 * really handing frames to a GPU client instead of using the CPU copy. */
		extern volatile uint32_t native_legacy_gpu_offer_calls __attribute__((weak_import)),
		    native_legacy_gpu_offer_refused __attribute__((weak_import)),
		    native_legacy_gpu_fallbacks __attribute__((weak_import));
		extern volatile uint64_t native_gop_gpu_offers __attribute__((weak_import)),
		    native_gop_gpu_completions __attribute__((weak_import)),
		    native_gop_gpu_failures __attribute__((weak_import));
		unsigned now = 0, irq = 0, rep = 0, msk = 0, oth = 0, pdc = 0, sp = 0;
		int st = 0, pon = -1, gpio = -1, err = 0;

		IOSleep(5000);
		if (&native_legacy_uptime_seconds != NULL) {
			now = native_legacy_uptime_seconds();
		}
		if (&native_buttons_stats != NULL) {
			st = native_buttons_stats[0];
			irq = (unsigned)native_buttons_stats[1];
			rep = (unsigned)native_buttons_stats[2];
			msk = (unsigned)native_buttons_stats[3];
			pon = native_buttons_stats[4];
			gpio = native_buttons_stats[5];
			oth = (unsigned)native_buttons_stats[6];
			pdc = (unsigned)native_buttons_stats[7];
			err = native_buttons_stats[8];
			sp = (unsigned)native_buttons_stats[9];
		}
		printf("MUYU LIVE p=%u t=%u btn st=%d irq=%u rep=%u msk=0x%x pon=%d gpio=%d pdc=0x%x err=%d sp=%u oth=%u\n",
		    pass, now, st, irq, rep, msk, pon, gpio, pdc, err, sp, oth);
		{
			char gpu[192];
			extern void muyu_handoff_snapshot(char *, unsigned) __attribute__((weak_import));
			gpu[0] = '\0';
			/* One strong cross-archive call reads every counter next to its definition,
			 * so a printed zero is real. "nolink" means the helper was not linked. */
			if (&muyu_handoff_snapshot != NULL) {
				muyu_handoff_snapshot(gpu, sizeof(gpu));
			}
			printf("MUYU GPU %s | w=%llu\n", gpu[0] ? gpu : "nolink",
			    (unsigned long long)(&native_gop_gpu_offers ? native_gop_gpu_offers : 0));
		}
		{
			/* The GPU probe's result rows (C1..C10, M, P, H, S) are written into boot_progress_ext and are only
			 * painted by the panel overlay, which needs a display it owns: ext_overlay_locked() bails on
			 * !display.bound and crash_banner() on !boot_progress_display_owned. With no GPU client taking the
			 * scanout (MUYU GPU o=0) the console keeps the panel, the boot page never binds, and those rows are
			 * invisible no matter how far the probe got. Print them here, on the one surface that is being drawn.
			 * The button/stage globals are read straight from their definitions so a blank row still says how far
			 * the bring-up went: st<> is native_gpu_power_stage and cp<> is the CP step in native_gpu_cp[0]
			 * (10 = C10, the gate that calls publishMetal()). */
			extern volatile uint32_t native_gpu_power_stage __attribute__((weak_import));
			extern uint32_t native_gpu_cp[8] __attribute__((weak_import));
			extern char boot_progress_ext[8][48] __attribute__((weak_import));
			/* Written by ArmSmmu4KAttachStreams (UniXNU/kits/ArmSmmu4K) at each register group, so a CPU stuck on a
			 * load that never completes still leaves a readable trail here. Zero means attach was never entered. */
			extern volatile unsigned arm_smmu4k_attach_crumb __attribute__((weak_import));
			/* 69.35/69.36 stage3 read-only probes: [profile] chip_id / gmem / gmu_chip_id;
			 * [hwinit] hwcg table applied / GMEM_RANGE_MAX / table entries / which table;
			 * [open] three poison bits / which BadArgument sub-condition / refusal count.
			 * All three are defined by the driver or GpuPowerProbe.cpp and only ever written
			 * by diagnostic code; this file reads them. No new console row. */
			extern volatile uint32_t native_adreno_profile_probe[8] __attribute__((weak_import));
			extern volatile uint32_t native_adreno_hwinit_probe[8] __attribute__((weak_import));
			extern volatile uint32_t native_adreno_open_probe[8] __attribute__((weak_import));
			const int have_profile = &native_adreno_profile_probe != NULL;
			const int have_hwinit = &native_adreno_hwinit_probe != NULL;
			const int have_open = &native_adreno_open_probe != NULL;
			unsigned stage = &native_gpu_power_stage != NULL ? (unsigned)native_gpu_power_stage : 0;
			unsigned cp = &native_gpu_cp != NULL ? native_gpu_cp[0] : 0;
			unsigned crumb = &arm_smmu4k_attach_crumb != NULL ? (unsigned)arm_smmu4k_attach_crumb : 0;
			/* 69.36: append the selected profile + hw_init facts to this short row instead of
			 * adding a console row. ci chip_id, gm GMEM in KiB, gi gmu_chip_id,
			 * hw which hwcg table (1 = A740), hn that table's entry count, gr GMEM_RANGE_MAX. */
			/* 69.51: which NativeAdrenoSubmit::poison() call site fired. pz = __LINE__ of the first
			 * poison, pc = how many times it ran. Read-only view of a diagnostic global. */
			extern volatile uint32_t native_adreno_poison_site[4] __attribute__((weak_import));
			const int have_poison = &native_adreno_poison_site != NULL;
			printf("MUYU PROBE st=%x cp=%u b=%x ci=%x gm=%u gi=%x hw=%u hn=%u gr=%x pz=%u pc=%u\n",
			    stage, cp, crumb,
			    have_profile ? (unsigned)native_adreno_profile_probe[0] : 0xffffffffu,
			    have_profile ? (unsigned)(native_adreno_profile_probe[1] >> 10) : 0xffffu,
			    have_profile ? (unsigned)native_adreno_profile_probe[2] : 0xffffffffu,
			    have_hwinit ? (unsigned)native_adreno_hwinit_probe[3] : 0xffffu,
			    have_hwinit ? (unsigned)native_adreno_hwinit_probe[2] : 0xffffu,
			    have_hwinit ? (unsigned)native_adreno_hwinit_probe[1] : 0xffffffffu,
			    have_poison ? (unsigned)native_adreno_poison_site[0] : 0xffffffffu,
			    have_poison ? (unsigned)native_adreno_poison_site[1] : 0xffffffffu);
			{
				/* Why the published service refuses user connections. native_adreno_mount_fail is written next to its definition
				 * (NativeAdrenoPublish.cpp mount()): [0] 1 no address space, 2 no VM, 3 the connection's GPU window was refused,
				 * [1] that step's IOReturn, [2] the window base's high word, [3] how many times. i/o are the input/output bits
				 * the service was published with (the window is 2^i - GpuClientVaStart, so i<=32 means no window at all). */
				extern unsigned muyu_gpu_bits[2] __attribute__((weak_import));
				extern volatile uint32_t native_adreno_mount_fail[4] __attribute__((weak_import));
				/* 69.26: a read-only view of createVm()'s last failure. table_switches[6] is its
				 * IOReturn (0xe00002c2 = BadArgument on the board), [7] the stage it stopped at
				 * (2 prepare, 3 attach, 4+i mapping buffer i, 0x30|op<<8 attach failed, 0 done),
				 * [2] how many createVm calls ever succeeded, [4] how many VMs are live.
				 * Appended to this row instead of adding one: the console holds one pass. */
				extern volatile uint32_t native_adreno_table_switches[8] __attribute__((weak_import));
				const int have_vm_stage = &native_adreno_table_switches != NULL;
				/* 69.29: preflight()'s exact refusal branch + the shared buffer's state at that moment.
				 * pf branch: 1 params/quarantine, 2 iova, 3 Busy overlap, 4 output_bits, 5 span shape,
				 * 6 tables->overlaps, 7 span total, 8 alias conflict; 0 = no refusal recorded.
				 * q quarantined (2 = null memory), bk backed, dr deviceReferences,
				 * gf fail() count, rn preflight refusal count. Read-only, no new console row. */
				extern volatile uint32_t native_adreno_preflight_probe[8] __attribute__((weak_import));
				extern volatile uint32_t native_adreno_global_fault[4] __attribute__((weak_import));
				const int have_preflight = &native_adreno_preflight_probe != NULL;
				const int have_fault = &native_adreno_global_fault != NULL;
				if (&muyu_gpu_bits != NULL)
					printf("MUYU B i%u o%u f%u s%x b%x n%u t6=%x t7=%x ok=%u vm=%u pf=%x q=%u bk=%u dr=%u gf=%u rn=%u ps=%x br=%u ob=%u\n", muyu_gpu_bits[0], muyu_gpu_bits[1],
					    &native_adreno_mount_fail != NULL ? (unsigned)native_adreno_mount_fail[0] : 0xffff,
					    &native_adreno_mount_fail != NULL ? (unsigned)native_adreno_mount_fail[1] : 0xffffffffu,
					    &native_adreno_mount_fail != NULL ? (unsigned)native_adreno_mount_fail[2] : 0,
					    &native_adreno_mount_fail != NULL ? (unsigned)native_adreno_mount_fail[3] : 0,
					    have_vm_stage ? (unsigned)native_adreno_table_switches[6] : 0xffffffffu,
					    have_vm_stage ? (unsigned)native_adreno_table_switches[7] : 0xffffffffu,
					    have_vm_stage ? (unsigned)native_adreno_table_switches[2] : 0xffffffffu,
					    have_vm_stage ? (unsigned)native_adreno_table_switches[4] : 0xffffffffu,
					    have_preflight ? (unsigned)native_adreno_preflight_probe[0] : 0xffffffffu,
					    have_preflight ? (unsigned)native_adreno_preflight_probe[2] : 0xffffffffu,
					    have_preflight ? (unsigned)native_adreno_preflight_probe[3] : 0xffffffffu,
					    have_preflight ? (unsigned)native_adreno_preflight_probe[4] : 0xffffffffu,
					    have_fault ? (unsigned)native_adreno_global_fault[0] : 0xffffffffu,
					    have_preflight ? (unsigned)native_adreno_preflight_probe[6] : 0xffffffffu,
					    /* 69.36: ps poison bitmask (1 vm / 2 submit / 4 ring),
					     * br BadArgument branch (1 null, 2 input_bits, 3 output_bits, 4 poison),
					     * ob how many open() refusals were recorded. */
					    have_open ? (unsigned)native_adreno_open_probe[0] : 0xffffffffu,
					    have_open ? (unsigned)native_adreno_open_probe[1] : 0xffffu,
					    have_open ? (unsigned)native_adreno_open_probe[4] : 0xffffu);
			}
			{
				/* vkd1: the accelerator already records what its clients did (NativeAdrenoPublish.cpp: opens / bad /
				 * attach state / the last external method with its IOReturn / the three latest opener names), but none of
				 * it ever reached the console. A Metal client that gives up *inside* this driver leaves a non-zero
				 * last_status behind on a specific selector; one that gives up after the driver returned leaves both
				 * clean. That is the difference between "the kernel refused a call" and "turnip failed on its own",
				 * which the 32-character PUB line cannot tell apart. Mirror of struct NativeAdrenoMetalDiag; every
				 * member is uint32_t or char[] so the layout carries no padding. */
				struct muyu_metal_diag {
					uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type,
					    name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status,
					    ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total,
					    marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status,
					    inflight, inflight_since_calls, forced_unmaps;
					char opener[3][17], reader[3][17], name_reader[17], starter[3][17], report[64], report2[64];
					uint32_t grants;
					/* vkd4: the tunneled ABI calls the driver actually served, in order (see the driver's struct for why the
					 * older native_adreno_call_ring stays empty), and the first one that did not return success. */
					uint32_t abi, call_seq, call_method[12], call_status[12], call_pid[12];
					uint32_t first_fail_method, first_fail_status, first_fail_pid, first_fail_at;
					/* vkd5: the last fence wait in full (see the driver's struct). */
					uint32_t wf_fence, wf_submitted, wf_retired, wf_waited_us, wf_timeout_us, wf_status, wf_count;
					/* vkd6: the method history and the last BoAlloc requests (see the driver's struct). */
					uint32_t hist_at, alloc_seq, alloc_bytes[4], alloc_align[4], alloc_at[4];
					uint8_t hist[96];
				};
				extern struct muyu_metal_diag native_adreno_metal_diag __attribute__((weak_import));
				extern volatile uint32_t native_adreno_trace_stage __attribute__((weak_import));
				if (&native_adreno_metal_diag != NULL) {
					const struct muyu_metal_diag *d = &native_adreno_metal_diag;
					printf("MUYU V op=%u bad=%u cl=%u call=%u sel=%u st=%08x at=%u inf=%u mb=%u/%u tr=%u gr=%u\n",
					    d->opens, d->bad, d->closes, d->calls, d->last_selector, d->last_status, d->attach_status,
					    d->inflight, d->mb_maps, d->mb_calls,
					    &native_adreno_trace_stage != NULL ? native_adreno_trace_stage : 0xffffffffu, d->grants);
					printf("MUYU O %s|%s|%s\n", d->opener[0], d->opener[1], d->opener[2]);
					/* vkd2: the accelerator's 16-word marker ring, newest first (slot = marks_total & 15). The plugin's
					 * Turnip phase leaves 6000, 6001..600b (stage progress), then 6fff (every test passed) or 6ffe (a
					 * Vulkan step failed) and, on a setup failure, 6fSS + 6eRR (stage SS, RR = -VkResult). 0x68f2 is the
					 * bridge's device-nil mark and 0x6Cnn the m2vpack import. So this row alone separates "Turnip's own
					 * device creation failed" from "Turnip is fine and the bridge refused the device": the 32-character
					 * PUB line cannot (GPU-HANDOFF §58.4). Read-only: no driver call, no behaviour change. */
					printf("MUYU K n=%u", d->marks_total);
					for (unsigned i = 1; i <= 12; i++)
						printf(" %04x", (unsigned)(d->marks[(d->marks_total - i) & 15] & 0xffffu));
					printf("\n");
					/* vkd3: whether the sandbox admits the selectors Turnip needs, and what the calls themselves
					 * returned. Turnip's Darwin backend can use only selector numbers the sandbox profile lets
					 * through (tu_knl_darwin.cc:161 dw_probe_selector); it probes 0..255 and every probe that reaches
					 * this object returns success and counts in `probes`. So pr=0 means NOT ONE selector was admitted
					 * and vkCreateInstance/Device ends in VK_ERROR_INITIALIZATION_FAILED with "no external-method
					 * selector is admitted for this process" - exactly the -3 on the PUB line. sel_bits[0]/[1] are the
					 * selectors 0..31 / 32..63 that ever arrived; es/st are the last non-tunnel selector and its
					 * IOReturn. Read-only. */
					printf("MUYU L pr=%u tn=%u ex=%u es=%u st=%08x sb=%08x %08x\n", d->probes, d->tunnel_calls,
					    d->ext_entries, d->ext_selector, d->ext_status, d->sel_bits[0], d->sel_bits[1]);
					/* vkd4: the tunneled calls the driver served, in order. vkd3's row read a separate ring
					 * (native_adreno_call_ring) that nothing in this tree ever points anywhere, so it printed nothing
					 * at all - the same mistake the K row made a round earlier: a channel that exists is not a channel
					 * that fills. These records are written by the driver itself, in the struct that already carries
					 * V/L. "method/status" newest first; method ids are NativeAdrenoABI.h's (1 query, 15
					 * get-device-info, 17 set-priority, 2 bo-alloc, 11 create-queue, 5 submit). ff is the first tunneled
					 * call that did not return success, with the call number it happened at. Read-only. */
					printf("MUYU C n=%u ff=%u/%x/%d@%u abi=%u seq=%u\n", d->tunnel_calls, d->first_fail_method,
					    d->first_fail_status, (int)d->first_fail_pid, d->first_fail_at, d->abi, d->call_seq);
					/* vkd7: M is off this round to make room for G below. It printed the last ten calls' method/status; Q gives
					 * the same sequence as runs and S gives per-method counts plus the failure count, so nothing is lost that
					 * the two of them do not already say. */
					/* vkd5: the driver's own per-method tally (NativeAdrenoUserClient.cpp:547 keeps calls / failures / last status /
					 * total us for every ABI method), and the last fence wait with the ring's numbers beside it. "m:c/f" only where
					 * the method was called at all; f is present only if something failed. */
					{
						extern volatile uint32_t native_adreno_method_stats[20][4] __attribute__((weak_import));
						if (&native_adreno_method_stats != NULL) {
							unsigned shown = 0;
							printf("MUYU S");
							for (unsigned m = 1; m < 20 && shown < 10; m++) {
								const unsigned calls = native_adreno_method_stats[m][0];
								if (!calls) continue;
								if (native_adreno_method_stats[m][1])
									printf(" %u:%u/%u", m, calls, native_adreno_method_stats[m][1]);
								else
									printf(" %u:%u", m, calls);
								shown++;
							}
							printf(" us5=%u us6=%u\n", native_adreno_method_stats[5][3], native_adreno_method_stats[6][3]);
						}
					}
					printf("MUYU W wf=%u/%u/%u/%u/%u/%x n=%u\n", d->wf_fence, d->wf_submitted, d->wf_retired,
					    d->wf_waited_us, d->wf_timeout_us, d->wf_status, d->wf_count);
					/* vkd6: the shape of the traffic. Q is the method history as runs, newest first ("6x10 5x1 13x1 ..."): the run
					 * that opens the list is how the client stopped, the ones behind it are the phase it was in. Y is the last four
					 * BoAlloc requests as "callindex/bytes/align" (newest first), so a global-BO-sized last allocation says the device
					 * was built to the end (>16 MiB = the big ring/pipeline pools, 4 KiB-1 MiB = the tail of tu_CreateDevice). */
					{
						unsigned runs = 0;
						printf("MUYU Q");
						if (d->hist_at) {
							const uint32_t total = d->hist_at < 96u ? d->hist_at : 96u;
							uint32_t i = 0;
							while (i < total && runs < 12) {
								const uint8_t m = d->hist[(d->hist_at - 1u - i) % 96u];
								uint32_t n = 1;
								while (i + n < total && runs < 12 && d->hist[(d->hist_at - 1u - i - n) % 96u] == m) n++;
								printf(" %ux%u", m, n);
								i += n;
								runs++;
							}
						}
						printf(" n=%u\n", d->hist_at);
						printf("MUYU Y n=%u", d->alloc_seq);
						for (unsigned k = 1; k <= 4 && k <= d->alloc_seq; k++) {
							const uint32_t at = (d->alloc_seq - k) % 4u;
							printf(" %u/%u/%u", d->alloc_at[at], d->alloc_bytes[at], d->alloc_align[at]);
						}
						printf("\n");
						/* Only when the client actually sent a text line (SetMetadata): the plugin's own result and Turnip's "T " line.
						 * Empty on a normal boot, so it costs no console row then. */
						if (d->reports && (d->report[0] || d->report2[0]))
							printf("MUYU X %u %s|%s\n", d->reports, d->report, d->report2);
					}
				}
			}
			{
				/* vkd7: the per-process open ledger (NativeAdrenoPublish.cpp:142). vkd6 left the -3 with every driver-side counter
				 * clean: mount never failed (B's n=0) and Query/GetDeviceInfo never failed (S's 1:2 and 15:2 carry no /f) - so
				 * neither the published service nor dw_open_connection's "NativeAdreno query failed" exit is the answer. What is
				 * still unread is whether a process was *refused* a connection outright, and why: this record counts opens
				 * granted / refused / closed per process name and keeps the last refusal's reason. Only rows that saw traffic are
				 * printed. Read-only. */
				struct muyu_client_record { char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal; };
				extern struct muyu_client_record native_adreno_client_log[8] __attribute__((weak_import));
				if (&native_adreno_client_log != NULL) {
					printf("MUYU G");
					unsigned rows = 0, seen = 0;
					for (unsigned i = 0; i < 8; i++) {
						const struct muyu_client_record *c = &native_adreno_client_log[i];
						if (!c->name[0]) continue;
						seen++;
						if (rows < 2) {
							printf(" %s/o%u/r%u/c%u/p%u/x%x", c->name, c->opens, c->refused, c->closes,
							    c->peak_mib, c->last_refusal);
							rows++;
						}
					}
					printf(" n%u\n", seen);
				}
			}
			{
				/* vkd1: one entry per IOServiceOpen of the accelerator (who, which open type, what the two sandbox
				 * gates said, and the attach result). Newest first. Type 0x4147 is the Adreno plugin's 'AG'; type 1
				 * is the original IOGPU library, and the driver treats them differently. */
				struct muyu_open_record { char name[20]; uint32_t type, macf, res, attach, foreign; };
				extern struct muyu_open_record native_adreno_open_log[8] __attribute__((weak_import));
				extern volatile uint32_t native_adreno_open_log_count __attribute__((weak_import));
				if (&native_adreno_open_log != NULL && &native_adreno_open_log_count != NULL) {
					uint32_t n = native_adreno_open_log_count;
					printf("MUYU A n=%u", n);
					for (uint32_t i = 0; i < 2 && i < n; i++) {
						const struct muyu_open_record *r = &native_adreno_open_log[(n - 1 - i) & 7];
						printf(" %s/t%x/m%08x/r%08x/a%u/f%u", r->name, r->type, r->macf, r->res, r->attach,
						    r->foreign);
					}
					printf("\n");
				}
			}
			{
				/* GPU hangs and the fault behind them, recorded by the driver but never printed. native_adreno_recovery:
				 * [0] hangs, [1] restarts, [2] failed restarts, [3] the last hung fence, [4] the restart's IOReturn,
				 * [5] submissions replayed, [6] hangs with a guilty job, [7] hangs of the kernel's own VM.
				 * native_adreno_recovery_fault: [0] how many faults were ended before a restart, then that fault's FSR,
				 * FAR low, FAR high and FSYNR0 — FSYNR0 carries the SID, FAR the address the GPU asked for. */
				extern volatile uint32_t native_adreno_recovery[8] __attribute__((weak_import));
				extern volatile uint32_t native_adreno_recovery_fault[5] __attribute__((weak_import));
				if (&native_adreno_recovery != NULL)
					printf("MUYU H h%u r%u f%u F%x s%x p%u g%u k%u\n",
					    (unsigned)native_adreno_recovery[0], (unsigned)native_adreno_recovery[1],
					    (unsigned)native_adreno_recovery[2], (unsigned)native_adreno_recovery[3],
					    (unsigned)native_adreno_recovery[4], (unsigned)native_adreno_recovery[5],
					    (unsigned)native_adreno_recovery[6], (unsigned)native_adreno_recovery[7]);
				if (&native_adreno_recovery_fault != NULL)
					printf("MUYU F n%u s%x a%x/%x y%x\n", (unsigned)native_adreno_recovery_fault[0],
					    (unsigned)native_adreno_recovery_fault[1], (unsigned)native_adreno_recovery_fault[2],
					    (unsigned)native_adreno_recovery_fault[3], (unsigned)native_adreno_recovery_fault[4]);
			}
			{
				/* The CP snapshot noteStall() takes once a fence has stopped for 50 ms and again after 1 s. This is the only place the
				 * GPU's own view of a stalled submission is kept, so it answers the two questions a hung ring leaves open: which address
				 * the context faulted on, and whether the IB the CP was fetching is mapped at all.
				 * stall_mem [0..3]: the bank's FSR, FAR low, FAR high and FSYNR0 at that moment.
				 * stall [2+i] is stallState's word i, so [10]/[11] are CP_RB_RPTR/WPTR, [12] the BR IB1 base low, [13] its remaining dwords.
				 * stall_mem [55]/[56]/[57]: handleAt() of the BR IB1, the BV IB1 and the BV IB2 in the stalled job's own address space -
				 * 0xffffffff means that address is not mapped there, which would be the fault itself. */
				extern volatile uint32_t native_adreno_stall[24] __attribute__((weak_import));
				extern volatile uint32_t native_adreno_stall_mem[63] __attribute__((weak_import));
				if (&native_adreno_stall != NULL && &native_adreno_stall_mem != NULL) {
					extern volatile uint32_t native_adreno_unstick[15] __attribute__((weak_import));
					printf("MUYU Z0 t%u f%u s%x a%x/%x y%x u%u/%u\n", (unsigned)native_adreno_stall[1],
					    (unsigned)native_adreno_stall[0], (unsigned)native_adreno_stall_mem[0],
					    (unsigned)native_adreno_stall_mem[1], (unsigned)native_adreno_stall_mem[2],
					    (unsigned)native_adreno_stall_mem[3], &native_adreno_unstick != NULL ? (unsigned)native_adreno_unstick[0] : 0,
					    &native_adreno_unstick != NULL ? (unsigned)native_adreno_unstick[4] : 0);
					printf("MUYU Z1 r%x/%x b%x/%x w%x h%x/%x/%x R%x\n", (unsigned)native_adreno_stall[10],
					    (unsigned)native_adreno_stall[11], (unsigned)native_adreno_stall[12], (unsigned)native_adreno_stall[13],
					    (unsigned)native_adreno_stall_mem[4], (unsigned)native_adreno_stall_mem[56],
					    (unsigned)native_adreno_stall_mem[57], (unsigned)native_adreno_stall_mem[58],
					    (unsigned)native_adreno_stall_mem[54]);
					/* The ring's words from 24 dwords before the CP's read pointer, 48 of them: the packet the CP refuses to fetch is
					 * still in memory, so a valid PM4 header here means the page (not the content) is what the CP lost, and garbage
					 * means the table switch pointed the ring's IOVA at other memory. Index 24 of the dump is the read pointer itself. */
					extern volatile uint32_t native_adreno_stall_ring[48] __attribute__((weak_import));
					extern volatile uint32_t native_adreno_stall_vm[2] __attribute__((weak_import));
					/* One block of 12 dwords, starting AT the CP's read pointer, from vkd5: the console holds one pass of this page and the
					 * S/W rows have to fit too. Index 0 is the read pointer itself, so a valid PM4 header here means the page (not the
					 * content) is what the CP lost, and garbage means the table switch pointed the ring's IOVA at other memory. */
					if (&native_adreno_stall_ring != NULL) {
						for (unsigned b = 24; b < 48; b += 24)
							printf("MUYU Z2 %02u %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x\n", b,
							    (unsigned)native_adreno_stall_ring[b + 0], (unsigned)native_adreno_stall_ring[b + 1],
							    (unsigned)native_adreno_stall_ring[b + 2], (unsigned)native_adreno_stall_ring[b + 3],
							    (unsigned)native_adreno_stall_ring[b + 4], (unsigned)native_adreno_stall_ring[b + 5],
							    (unsigned)native_adreno_stall_ring[b + 6], (unsigned)native_adreno_stall_ring[b + 7],
							    (unsigned)native_adreno_stall_ring[b + 8], (unsigned)native_adreno_stall_ring[b + 9],
							    (unsigned)native_adreno_stall_ring[b + 10], (unsigned)native_adreno_stall_ring[b + 11]);
						/* The fence page's first four words and the VM words are no longer printed (vkd6): the console holds one pass of
						 * this page and the client's call history (Q/Y) is what this round reads. */
					}
				}
			}
			{
				/* Per-process address-space switching, the mechanism every user connection depends on: cp_aperture
				 * [0] whether the SCM aperture call answered, [1] its x0 (0 = success), [2] checkCpSwitch's IOReturn,
				 * [3] the table mode finally used (2 = the CP switches inside the ring, 1 = the CPU writes TTBR0).
				 * table_switches [6] is why switching was refused or the VM build failed, [7] the last VM stage
				 * (2 prepare, 3 attach, 4+i mapping buffer i, 0x30|op attach failed, 0 done), [1] how many times the CPU wrote
				 * TTBR0, [0] how many table updates the CP consumed. */
				extern volatile uint32_t native_adreno_cp_aperture[4] __attribute__((weak_import));
				extern volatile uint32_t native_adreno_table_switches[8] __attribute__((weak_import));
				extern int32_t NativeAdrenoGpuContextReadTtbr0(uint64_t *) __attribute__((weak_import));
				extern uint64_t NativeAdrenoGpuContextKernelTtbr0(void) __attribute__((weak_import));
				if (&native_adreno_cp_aperture != NULL) {
					/* The table-switch line (cp_aperture / table_switches / TTBR0 g,k) is not printed from vkd6: this round reads the
					 * client's call history, and the console holds one pass of the page. checkCpSwitch's five steps still follow. */
					/* checkCpSwitch's five steps, each 0 when it passed: [0] createVm, [1] probeBuffer, [2] submit,
					 * [3] waitAll, [4] readTtbr0; [5] the fence it submitted. The first non-zero word is where the
					 * "does a CP-side switch actually land" probe stopped, which is the whole question of mode 2. */
					extern volatile uint32_t native_adreno_cp_check[6] __attribute__((weak_import));
					if (&native_adreno_cp_check != NULL)
						printf("MUYU W k%x %x %x %x %x f%x\n", (unsigned)native_adreno_cp_check[0],
						    (unsigned)native_adreno_cp_check[1], (unsigned)native_adreno_cp_check[2],
						    (unsigned)native_adreno_cp_check[3], (unsigned)native_adreno_cp_check[4],
						    (unsigned)native_adreno_cp_check[5]);
				}
			}
			{
				/* The Metal bridge's last streamed line: "PUB no device: <why>" is the only place native_bridge_metal_open's
				 * refusal reason reaches this console. */
				extern char muyu_gpu_reason[64] __attribute__((weak_import));
				if (&muyu_gpu_reason != NULL && muyu_gpu_reason[0]) printf("MUYU R %s\n", muyu_gpu_reason);
			}
			if (&boot_progress_ext != NULL) {
				for (unsigned slot = 2; slot < 8; ++slot) {
					const char *row = boot_progress_ext[slot];
					if (!row[0] || row[0] == ' ') continue;
					printf("MUYU X%u %s\n", slot, row);
				}
			}
			/* CP-stall hardware rows, collected in heartbeat() every time the ring is not drained. They live outside
			 * boot_progress_ext because its rows 2/3 are overwritten by later boot stages (and the Q row is overwritten
			 * by the C row in the same branch). S = status/protect/FSR, Q = each CP thread's PC and packet header,
			 * C = the IB coherence sample, P = the CP's live ring base/control/rptr-shadow registers,
			 * W = WPTR fence retries plus the stall handler's kick/drain/reassert counts. §38 adds
			 * G = CP/GMU halt + aperture + clock status and S = deeper CP_SQE_STAT words. §43 adds
			 * K = three spaced hardware RPTR / SQE-PC samples (live every heartbeat) and
			 * M = global GFSR / context FSR / VM fault count / Q5's last refresh tick. */
			extern char muyu_cp_stall_rows[9][64] __attribute__((weak_import));
			if (&muyu_cp_stall_rows != NULL) {
				for (unsigned slot = 0; slot < 9; ++slot) {
					const char *row = muyu_cp_stall_rows[slot];
					if (!row[0] || row[0] == ' ') continue;
					printf("MUYU Q%u %s\n", slot, row);
				}
			}
			/* §38: did the board's hang restart hook ever run, and where did it stop? uke registers one,
			 * the 17-line port did not, which is why H shows hangs but zero restarts. */
			extern uint32_t native_gpu_restart[4] __attribute__((weak_import));
			if (&native_gpu_restart != NULL)
				printf("MUYU RT n%u s%u e%x b%x\n", (unsigned)native_gpu_restart[0],
				    (unsigned)native_gpu_restart[1], (unsigned)native_gpu_restart[2],
				    (unsigned)native_gpu_restart[3]);
			/* §43 / restart2, §46 / restart3: read-only snapshots around gpuRestart(). z=zap[0] (signed),
			 * l=zapLimit(), f=fenceValue(), r=RPTR at entry / after ring->restart() / after CP_ME_INIT /
			 * at exit, w=WPTR at entry / exit, g=global GFSR at exit. Valid ONLY when the hook ran: all four
			 * r equal => the CP never moved; r climbing => it moved and stalled again. z=-1 means secure mode
			 * was never left (§41.4); g!=0 or Q8's n>0 points at the latched global SMMU fault (§42.3).
			 * §45.3: when RT n0 the array is zeroed BSS, so print a sentinel instead of a fake sample. */
			extern uint32_t native_gpu_restart2[10] __attribute__((weak_import));
			if (&native_gpu_restart2 != NULL && &native_gpu_restart != NULL && native_gpu_restart[0] == 0)
				printf("MUYU RT2 -- gpuRestart not called (n0)\n");
			else if (&native_gpu_restart2 != NULL)
				printf("MUYU RT2 z%d l%u f%x r%x/%x/%x/%x w%x/%x g%x\n",
				    (int)native_gpu_restart2[0], (unsigned)native_gpu_restart2[1],
				    (unsigned)native_gpu_restart2[6], (unsigned)native_gpu_restart2[2],
				    (unsigned)native_gpu_restart2[4], (unsigned)native_gpu_restart2[5],
				    (unsigned)native_gpu_restart2[7], (unsigned)native_gpu_restart2[3],
				    (unsigned)native_gpu_restart2[8], (unsigned)native_gpu_restart2[9]);
			/* Rows the probe keeps for this dump: GPUCC power states, the command-db vote inputs, whether the pinned
			 * OPP table matched the board's own rail data, and which ShengGmuBuildStartup check refused. Those slots
			 * of boot_progress_ext are overwritten by later stages, and a one-shot IOLog has scrolled away by the
			 * time the panel is photographed. */
			extern char muyu_gpu_diag[6][48] __attribute__((weak_import));
			if (&muyu_gpu_diag != NULL) {
				for (unsigned slot = 0; slot < 6; ++slot) {
					const char *row = muyu_gpu_diag[slot];
					if (!row[0] || row[0] == ' ') continue;
					printf("MUYU D%u %s\n", slot, row);
				}
			}
			/* Read-only SMMU snapshot the Adreno GPU context takes right before it writes its own streams.
			 * Filled while the SMMU is still the firmware's, so these rows show which slots are already live
			 * and where SIDs 0/1/5 sit even when the attaching CPU never returns. */
			extern char native_adreno_smmu_audit[8][48] __attribute__((weak_import));
			if (&native_adreno_smmu_audit != NULL) {
				/* 0..1 only: IDR0/IDR1/SCR0 and GFSR with the bank's SCTLR. The slot census (A2) and where SIDs 0/1/5 sit (A3)
				 * were settled when slot 0/1 turned out clean, and the two stall rows need their lines more. */
				for (unsigned slot = 0; slot < 2; ++slot) {
					const char *row = native_adreno_smmu_audit[slot];
					if (!row[0] || row[0] == ' ') continue;
					printf("MUYU A%u %s\n", slot, row);
				}
			}
		}
		{
			/* §51 upper-layer milestones — read-only, one line per 5 s ticker pass. The 13:48 boot
			 * had the GPU kernel bring-up green (PROBE st=97 cp=10 b=24) yet calls=0/ts=ffffffff,
			 * no R row and no NATIVE-NVT frames: userspace graphics never started. lg= legacy display
			 * stage/init/start/DT, s= IOSurface found/status, op= opened/closed user connections of
			 * the published GPU service, mf= mount fail code/count, i= input VA bits, r= first char
			 * of the Metal bridge reason ('-' = none). Every symbol is weak_import and only read. */
			extern volatile uint32_t native_legacy_stage __attribute__((weak_import));
			extern volatile uint32_t native_legacy_init_result __attribute__((weak_import));
			extern volatile uint32_t native_legacy_start_result __attribute__((weak_import));
			extern volatile uint32_t native_legacy_dt_attached __attribute__((weak_import));
			extern volatile uint32_t native_legacy_surface_found __attribute__((weak_import));
			extern volatile int native_legacy_surface_status __attribute__((weak_import));
			extern volatile uint32_t native_adreno_mount_fail[4] __attribute__((weak_import));
			extern unsigned muyu_gpu_bits[2] __attribute__((weak_import));
			extern char muyu_gpu_reason[64] __attribute__((weak_import));
			extern uint32_t native_adreno_publish_stats(uint32_t *) __attribute__((weak_import));
			uint32_t closed = 0, opened = 0;

			if (&native_adreno_publish_stats != NULL)
				opened = native_adreno_publish_stats(&closed);
			printf("MUYU UP lg=%u/%x/%x/%u s=%u/%d op=%u/%u mf=%u/%u i=%u r=%c\n",
			    &native_legacy_stage != NULL ? (unsigned)native_legacy_stage : 0xffffu,
			    &native_legacy_init_result != NULL ? (unsigned)native_legacy_init_result : 0xffffffffu,
			    &native_legacy_start_result != NULL ? (unsigned)native_legacy_start_result : 0xffffffffu,
			    &native_legacy_dt_attached != NULL ? (unsigned)native_legacy_dt_attached : 0xffffu,
			    &native_legacy_surface_found != NULL ? (unsigned)native_legacy_surface_found : 0xffffu,
			    &native_legacy_surface_status != NULL ? (int)native_legacy_surface_status : -2,
			    opened, closed,
			    &native_adreno_mount_fail != NULL ? (unsigned)native_adreno_mount_fail[0] : 0xffffu,
			    &native_adreno_mount_fail != NULL ? (unsigned)native_adreno_mount_fail[3] : 0xffffu,
			    &muyu_gpu_bits != NULL ? muyu_gpu_bits[0] : 0,
			    (&muyu_gpu_reason != NULL && muyu_gpu_reason[0]) ? muyu_gpu_reason[0] : '-');
		}
		{
			/* §52: load_init_program entry is confirmed by this ticker, but the process census
			 * still has no launchd. Expose the existing owner-cache/Data mount substep counters so
			 * the next photo can distinguish a mount stall from a later launchd exec stall. */
			extern volatile int native_owner_cache_stage __attribute__((weak_import));
			extern volatile int native_owner_cache_status __attribute__((weak_import));
			extern volatile int native_owner_cache_mount_status __attribute__((weak_import));
			extern volatile int native_owner_data_stage __attribute__((weak_import));
			extern volatile int native_owner_data_status __attribute__((weak_import));
			printf("MUYU ROOT c=%d/%d/%d d=%d/%d\n",
			    &native_owner_cache_stage != NULL ? (int)native_owner_cache_stage : -2,
			    &native_owner_cache_status != NULL ? (int)native_owner_cache_status : -2,
			    &native_owner_cache_mount_status != NULL ? (int)native_owner_cache_mount_status : -2,
			    &native_owner_data_stage != NULL ? (int)native_owner_data_stage : -2,
			    &native_owner_data_status != NULL ? (int)native_owner_data_status : -2);
		}
		{
			/* root1 establishes that neither owner-mount helper was entered. init1 narrows the
			 * preceding load_init_program boundaries without changing their execution. */
			extern volatile int muyu_init_stage __attribute__((weak_import));
			printf("MUYU INIT q=%d\n", &muyu_init_stage != NULL ? (int)muyu_init_stage : -2);
		}
		{
			/* init1 returned q=0, i.e. load_init_program was never entered, so the stall is inside
			 * bsd_init(). bsd1 exposes a read-only ladder over that function's milestones. */
			extern volatile int muyu_bsd_stage __attribute__((weak_import));
			printf("MUYU BSD s=%d\n", &muyu_bsd_stage != NULL ? (int)muyu_bsd_stage : -2);
		}
		/* §51: every third pass (15 s) instead of every twelfth (60 s), so a photo taken
		 * during the early stall still carries the process census. Print-only change. */
		if ((pass % 3) == 0 && &muyu_proc_census != NULL) {
			char census[160];
			muyu_proc_census(census, sizeof(census));
			printf("%s", census);
		}
	}
}

static void
muyu_dump_start(void)
{
	uint32_t at = 0;
	thread_t thread;
	/* First caller wins: the display probe calls this at ~t+10 s (early enough to take the panel before the
	 * graphics client rotates and shrinks the console) and load_init_program calls it again later. Without
	 * the guard the second call would start a second ticker on the same panel. */
	static int started;

	if (started) {
		return;
	}
	if (!PE_parse_boot_argn("muyu-dump-at", &at, (int)sizeof(at)) || at < 5) {
		return;
	}
	started = 1;
	if (kernel_thread_start_priority(muyu_dump_later, (void *)(uintptr_t)at, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

void
muyu_dump_start_late(void)
{
	muyu_dump_start();
}
