/* SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * Weak fallbacks for the sheng legacy display probes when the first-console
 * configuration disables the BootProgress layer and the Adreno GPU probe.
 * The full sheng configuration links the real strong definitions; a weak
 * definition here is then discarded by the linker.
 *
 * boot_progress_prepare_scanout returns 1 in the real layer when boot
 * progress is disabled, which is the "no diagnostic writer" answer the
 * display probe expects.
 */
#include <stdbool.h>
#include <stdint.h>

__attribute__((weak)) int
boot_progress_pulse_anchor (uintptr_t base, uint64_t bytes, unsigned width,
                            unsigned height, unsigned row_bytes)
{
  (void)base;
  (void)bytes;
  (void)width;
  (void)height;
  (void)row_bytes;
  return 0;
}

__attribute__((weak)) int
boot_progress_prepare_scanout (void)
{
  return 1;
}

__attribute__((weak)) void
boot_progress_cancel_scanout (void)
{
}

__attribute__((weak)) void
boot_progress_stage (const char *text)
{
  (void)text;
}

__attribute__((weak)) char boot_progress_ext[8][48];
__attribute__((weak)) char boot_progress_ext_wide[2][112];

__attribute__((weak)) void
sheng_gpu_power_probe (void)
{
}

/*
 * NativeLegacyDisplay always references the GPU hand-off hooks, even when
 * CONFIG_XNU_NATIVE_ADRENO is off and GpuPowerProbe.cpp is not compiled.
 * The weak definitions keep the display-only kernel linkable; the Adreno
 * configuration supplies the strong definitions and discards these.
 */
__attribute__((weak)) bool
native_legacy_gpu_offer (void *display, void *memory, uint64_t stride,
                         uint32_t width, uint32_t height)
{
  (void)display;
  (void)memory;
  (void)stride;
  (void)width;
  (void)height;
  return false;
}

__attribute__((weak)) void
native_legacy_gpu_fallback_observed (int status)
{
  (void)status;
}

__attribute__((weak)) void
native_legacy_swap_stage (unsigned stage)
{
  (void)stage;
}

/* 2026-11-14 01:2x, fix33: with the Adreno plugin finally loading (fix31, owner trust cache) the renderer hands validateSwap a real
 * IOSurface and every frame is refused *after* the request checks pass: reason 4 means the six surface properties are what reject it.
 * This stub received exactly those six values and threw them away, so nothing in any log said which one was wrong. Record them
 * (native_legacy_srej_values, read back by the GPU probe rows) and print the first few, rate limited so a 60 Hz reject loop cannot
 * own the window: 409 "verify_swap failed" lines in the fix32 boot. */
extern void IOLog (const char *format, ...);
volatile uint64_t native_legacy_srej_values[7];
volatile uint32_t native_legacy_srej_calls;

__attribute__((weak)) void
native_legacy_surface_rejected (uint64_t width, uint64_t height, uint32_t format,
                                uint32_t planes, uint64_t stride, uint64_t allocation,
                                uint64_t length)
{
  native_legacy_srej_values[0] = width;
  native_legacy_srej_values[1] = height;
  native_legacy_srej_values[2] = format;
  native_legacy_srej_values[3] = planes;
  native_legacy_srej_values[4] = stride;
  native_legacy_srej_values[5] = allocation;
  native_legacy_srej_values[6] = length;
  uint32_t n = ++native_legacy_srej_calls;
  if (n <= 6 || (n % 128u) == 0) {
    IOLog ("MUYU SREJ w=%llu h=%llu fmt=%x planes=%u stride=%llu alloc=%llu len=%llu n=%u\n",
           (unsigned long long)width, (unsigned long long)height, (unsigned)format, (unsigned)planes,
           (unsigned long long)stride, (unsigned long long)allocation, (unsigned long long)length, (unsigned)n);
  }
}
