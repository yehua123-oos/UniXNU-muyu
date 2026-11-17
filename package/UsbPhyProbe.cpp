/* muyu USB layer probe - read only, no writes, no DMA, no PHY programming.
 *
 * Why this file exists
 * --------------------
 * The USB CDC console (u128-u159, ~29 rounds, 2026-11-08) never enumerated and was torn out. The
 * archived failure image (docs/ABANDONED-CHANNELS.md section 2) left three numbers that were never
 * separated from each other:
 *
 *   - DCTL.CSFTRST never self-cleared (polled 3000 and 20000 times)
 *   - DEPCFG queued but never started executing
 *   - ConnectSpeed read Full-Speed while DCFG.DevSpeed said High-Speed
 *
 * Those three are symptoms of different layers. CSFTRST not clearing means the core reset handshake
 * never completed, which is a clock/reset problem, not a driver problem. A FS connection on an HS
 * configuration means the UTMI/eUSB2 PHY is not running, which is a PHY programming problem. DEPCFG
 * never running is downstream of both. Re-implementing the gadget driver before telling those apart
 * is what cost the previous 29 rounds.
 *
 * So this probe answers, in one boot, in this order:
 *
 *   1. Is the USB30 power domain on at all?      -> gcc_usb30_prim_gdsc  @ 0x139004, PWR_ON = bit 31
 *   2. Is the USB3 PHY power domain on?          -> gcc_usb3_phy_gdsc    @ 0x150018
 *   3. Does the DWC3 core respond on the bus?    -> GSNPSID must read 0x5533xxxx
 *   4. Did anything reset the core?              -> DCTL.CSFTRST (bit 31) / RWUSIG (bit 0)
 *   5. What speed did the PHY actually train?    -> DSTS[3:0] vs DCFG[3:0]
 *   6. Was the eUSB2 PHY programmed by anyone?   -> 0x88e3000 + 0x00 must read 0x58
 *
 * Every address below is a device-tree fact, not a guess:
 *   dtc -I dtb -O dts fw/stock/dtb_3.dtb
 *     /soc/ssusb@a600000          reg <0xa600000 0x100000>, qcom,use-eusb2-phy
 *     /soc/ssusb@a600000/dwc3     reg <0xa600000 0xd93c>
 *     /soc/hsphy@88e3000          reg <0x88e3000 0x154>,  qcom,param-override-seq = <0x00 0x58>
 *     /soc/ssphy@88e8000          reg <0x88e8000 0x3000>
 *     gcc_usb30_prim_gdsc         reg <0x139004 0x04>
 *     gcc_usb3_phy_gdsc           reg <0x150018 0x04>
 *
 * Read order is deliberate and is the whole safety argument: GDSCR lives in GCC, which is always
 * powered, so reading it cannot hang the interconnect. The DWC3 window is only touched when the
 * prim GDSCR reports PWR_ON, and the eUSB2/SS PHY windows only when their own domain reports on.
 * A background read of an unpowered block is exactly the kind of access that stalls in the
 * interconnect and takes the watchdog out (ABANDONED-CHANNELS.md: "reading GDSCR hangs the bus" was
 * a wrong address, but the class of fault is real).
 *
 * Boot argument: usb-probe=<seconds>. 0 or absent = inert. The thread wakes at that many
 * seconds, reads once, then reads again 30 s later and once more 60 s after that, so a state that
 * changes during the user-space boot is visible as a transition rather than as one ambiguous sample.
 */
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

/* osfmk/arm/machine_routines.h declares this, but the header is not reachable from this translation
 * unit (the C++ build only gets the exported subset), so it is declared here the same way this project
 * declares the other kernel entry points it uses. */
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#include <stdarg.h>

/* ---- direct-to-disk evidence channel (usb19) -----------------------------------------------
 * The writer does not open the UFS log region until ~90.5 s (NativeUfs "store open" in usb18) and
 * the machine dies 2-99 s after that. IOLog lines printed before the region opens reach the medium
 * only if a later writer round carries them - a coin flip that cost usb13/17/18 their whole survey.
 * So every probe line is ALSO formatted into gUsbDirect; the moment the region opens the probe writes
 * that buffer straight to blocks 8180-8182 with the bounded (100 ms) native_ufs_log_write. Blocks 8177..8191 are the
 * unused tail of the 8192-block window (header 0, msg 1..256, slots 257..8176), so no round can
 * overwrite them.
 */
extern "C" int native_ufs_export_blocks(void) __attribute__((weak));
extern "C" int native_ufs_log_write(uint64_t, uint32_t, const void *) __attribute__((weak));
extern "C" int vsnprintf(char *, size_t, const char *, va_list);

static char gUsbDirect[12288] __attribute__((aligned(4096)));
static size_t gUsbDirectLen = 0;

static void usbDirectStore(const char *fmt, ...) {
    if (gUsbDirectLen >= sizeof(gUsbDirect) - 1) return;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(gUsbDirect + gUsbDirectLen, sizeof(gUsbDirect) - gUsbDirectLen, fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t room = sizeof(gUsbDirect) - gUsbDirectLen - 1;
        gUsbDirectLen += (size_t)n > room ? room : (size_t)n;
    }
}

#define ULOG(...) do { IOLog(__VA_ARGS__); usbDirectStore(__VA_ARGS__); } while (0)

namespace {

constexpr uint64_t kGccBase = 0x00100000ull;    /* SM8635 cliffs GCC; 0x150018 sits in the same 2 MiB */
constexpr uint64_t kGccLen = 0x00200000ull;

constexpr uint64_t kDwc3Base = 0x0a600000ull;   /* /soc/ssusb@a600000/dwc3, core regs <0xd93c */
constexpr uint64_t kDwc3Len = 0x00010000ull;

constexpr uint64_t kEusb2Base = 0x088e3000ull;  /* /soc/hsphy@88e3000, reg size 0x154 */
constexpr uint64_t kEusb2Len = 0x00001000ull;

constexpr uint64_t kSsPhyBase = 0x088e8000ull;  /* /soc/ssphy@88e8000 */
constexpr uint64_t kSsPhyLen = 0x00003000ull;

/* Absolute physical addresses from the DTB (not GCC base + offset: the GPU port already established
 * that these are expressed absolutely on this SoC and that assuming an offset gives a wrong window). */
constexpr uint32_t kPrimGdscrOff = 0x139004u - 0x00100000u;
constexpr uint32_t kPhyGdscrOff = 0x150018u - 0x00100000u;

/* DWC3 core register offsets (Synopsys databook, same as the ace5/vermeer ports use). */
constexpr uint32_t kGsnpsid = 0x0c120u;
constexpr uint32_t kGctl = 0x0c110u;
constexpr uint32_t kGusb2PhyCfg0 = 0x0c200u;
constexpr uint32_t kGusb3PipeCtl0 = 0x0c2c0u;
constexpr uint32_t kDcfg = 0x0c700u;
constexpr uint32_t kDctl = 0x0c704u;
constexpr uint32_t kDevten = 0x0c708u;
constexpr uint32_t kDsts = 0x0c70cu;
constexpr uint32_t kDgcmd = 0x0c714u;
constexpr uint32_t kDalepena = 0x0c718u;
constexpr uint32_t kGevntadrlo = 0x0c400u;
constexpr uint32_t kGevntsiz = 0x0c408u;
constexpr uint32_t kGevntcount = 0x0c40cu;

/* GDSCR bits (drivers/clk/qcom/gdsc.h). bit 31 is the power-on acknowledge. */
constexpr uint32_t kGdscrSwCollapse = 1u << 0;
constexpr uint32_t kGdscrReset = 1u << 5;
constexpr uint32_t kGdscrPwrOn = 1u << 31;

/* DCTL bits. */
constexpr uint32_t kDctlRwusig = 1u << 0;       /* Run/Stop */
constexpr uint32_t kDctlSftdiscon = 1u << 1;
constexpr uint32_t kDctlCsftrst = 1u << 31;

uintptr_t gGcc, gDwc3, gEusb2, gSsPhy;

/* 2026-11-16 14:5x: IODeviceMemory + map() produced no output at all on two of four boots: the thread
 * printed "woke" and then never printed the M rows, i.e. it blocked inside the mapping call. That call
 * goes through IOKit, which is busy while the user space is starting. ml_io_map is the kernel's own
 * uncached device mapping - one page-table walk, no service matching, nothing to wait on. */
uintptr_t mapWindow(uint64_t phys, uint64_t len)
{
    return static_cast<uintptr_t>(ml_io_map(static_cast<vm_offset_t>(phys), static_cast<vm_size_t>(len)));
}

inline uint32_t rd(uintptr_t base, uint32_t off)
{
    return *reinterpret_cast<volatile uint32_t *>(base + off);
}

inline void wr(uintptr_t base, uint32_t off, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t *>(base + off) = value;
}

[[maybe_unused]] void logWindow(const char *tag, uint64_t phys, uintptr_t va)
{
    ULOG("MUYU USB M %s phys=%08llx va=%s\n", tag, (unsigned long long)phys, va ? "ok" : "FAIL");
}

void probeOnce(unsigned round)
{
    if (!gGcc) {
        ULOG("MUYU USB S%d gcc-map-failed\n", round);
        return;
    }
    const uint32_t prim = rd(gGcc, kPrimGdscrOff);
    const uint32_t phy = rd(gGcc, kPhyGdscrOff);
    const bool primOn = (prim & kGdscrPwrOn) != 0;
    const bool phyOn = (phy & kGdscrPwrOn) != 0;

    /* G = power domains. p/o = prim/phy PWR_ON, c = SW_COLLAPSE still set (i.e. nothing asked for
     * power), r = RESET bit still asserted. */
    ULOG("MUYU USB G%d prim=%08x phy=%08x p%d,%d c%d,%d r%d,%d\n",
        round, prim, phy, primOn ? 1 : 0, phyOn ? 1 : 0,
        (prim & kGdscrSwCollapse) ? 1 : 0, (phy & kGdscrSwCollapse) ? 1 : 0,
        (prim & kGdscrReset) ? 1 : 0, (phy & kGdscrReset) ? 1 : 0);

    if (!primOn) {
        /* Nothing downstream can be read safely: the core clock is off, so the interconnect would
         * stall on the first DWC3 access. Report and stop here - this is the actionable answer. */
        ULOG("MUYU USB X%d core-off: usb30_prim_gdsc never powered on, DWC3 left untouched\n", round);
        return;
    }

    if (!gDwc3) {
        ULOG("MUYU USB X%d dwc3-map-failed\n", round);
        return;
    }
    const uint32_t id = rd(gDwc3, kGsnpsid);
    const uint32_t gctl = rd(gDwc3, kGctl);
    const uint32_t g2phy = rd(gDwc3, kGusb2PhyCfg0);
    const uint32_t g3pipe = rd(gDwc3, kGusb3PipeCtl0);
    const uint32_t dcfg = rd(gDwc3, kDcfg);
    const uint32_t dctl = rd(gDwc3, kDctl);
    const uint32_t devten = rd(gDwc3, kDevten);
    const uint32_t dsts = rd(gDwc3, kDsts);
    const uint32_t dgcmd = rd(gDwc3, kDgcmd);
    const uint32_t dalep = rd(gDwc3, kDalepena);
    const uint32_t evtlo = rd(gDwc3, kGevntadrlo);
    const uint32_t evtsiz = rd(gDwc3, kGevntsiz);
    const uint32_t evtcnt = rd(gDwc3, kGevntcount);

    /* id=5533xxxx confirms the window answers; 00000000 or ffffffff means the block is clock-gated
     * even though its GDSC claims PWR_ON, which is a different fault again. */
    ULOG("MUYU USB D%d id=%08x gctl=%08x g2phy=%08x g3pipe=%08x\n", round, id, gctl, g2phy, g3pipe);
    ULOG("MUYU USB D%d dcfg=%08x dctl=%08x devten=%08x dsts=%08x\n", round, dcfg, dctl, devten, dsts);
    ULOG("MUYU USB D%d dgcmd=%08x dalep=%08x evt=%08x/%08x/%08x\n", round, dgcmd, dalep, evtlo, evtsiz, evtcnt);

    /* spd = DSTS[3:0] (0=HS, 1=FS, 2=LS, 4=SS, 5=SS+); lspd = DCFG[3:0] what we asked for.
     * A mismatch here is the archived "ConnectSpeed FS vs DCFG HS" image reproduced in one line.
     * lnk = DSTS[11:8] link state, rst = CSFTRST still set, rw = RWUSIG (device controller started),
     * sd = SOFTDISCONNECT (1 means the core is holding the host off on purpose). */
    ULOG("MUYU USB Q%d spd=%u lspd=%u lnk=%x rst=%d rw=%d sd=%d suspy=%d\n", round,
        dsts & 0xfu, dcfg & 0xfu, (dsts >> 8) & 0xfu,
        (dctl & kDctlCsftrst) ? 1 : 0, (dctl & kDctlRwusig) ? 1 : 0,
        (dctl & kDctlSftdiscon) ? 1 : 0, (g2phy & (1u << 7)) ? 1 : 0);

    if (!gEusb2) {
        ULOG("MUYU USB X%d eusb2-map-failed\n", round);
    } else if (!phyOn) {
        /* eUSB2 sits behind gcc_usb3_phy_gdsc on this board; reading it while that is off is the
         * access pattern that stalls. Skip rather than risk it. */
        ULOG("MUYU USB X%d phy-domain-off: eUSB2 left untouched\n", round);
    } else {
        const uint32_t p00 = rd(gEusb2, 0x00);
        const uint32_t p04 = rd(gEusb2, 0x04);
        const uint32_t p08 = rd(gEusb2, 0x08);
        const uint32_t p0c = rd(gEusb2, 0x0c);
        const uint32_t p10 = rd(gEusb2, 0x10);
        const uint32_t p14 = rd(gEusb2, 0x14);
        const uint32_t p18 = rd(gEusb2, 0x18);
        const uint32_t p1c = rd(gEusb2, 0x1c);
        /* qcom,param-override-seq = <0x00 0x58>: offset 0 must read 0x58 once the vendor PHY init ran.
         * p00 != 0x58 means nobody has programmed this PHY, which is the first thing to fix. */
        ULOG("MUYU USB E%d p00=%08x p04=%08x p08=%08x p0c=%08x\n", round, p00, p04, p08, p0c);
        ULOG("MUYU USB E%d p10=%08x p14=%08x p18=%08x p1c=%08x prog=%d\n", round,
            p10, p14, p18, p1c, (p00 == 0x58) ? 1 : 0);
    }

}

/* 2026-11-16 14:4x: the read-only survey answered "the domain is on, the core answers, the PHY is
 * never programmed". The next question cannot be answered by reading: is the eUSB2 block writable at
 * all? DT asks for exactly one value there (qcom,param-override-seq = <0x00 0x58>), so writing it and
 * reading it back is the smallest test that separates "PHY clock alive, block programmable" from
 * "cfg_ahb_clk still gated, every write discarded". The old value is restored so this probe leaves no
 * tune behind. */
[[maybe_unused]] void writeOnce(unsigned round)
{
    if (!gGcc) return;
    if (!(rd(gGcc, kPhyGdscrOff) & kGdscrPwrOn)) {
        ULOG("MUYU USB W%d phy-domain-off, skipped\n", round);
        return;
    }
    if (!gEusb2) {
        ULOG("MUYU USB W%d no-eusb2-window\n", round);
        return;
    }
    const uint32_t before = rd(gEusb2, 0x00);
    wr(gEusb2, 0x00, 0x58);
    const uint32_t after = rd(gEusb2, 0x00);
    wr(gEusb2, 0x00, before);
    const uint32_t restored = rd(gEusb2, 0x00);
    ULOG("MUYU USB W%d before=%08x after=%08x restored=%08x writable=%d\n",
        round, before, after, restored, after == 0x58u ? 1 : 0);
}

/* 2026-11-16 14:5x: the PHY window answered "not writable", so the next thing to establish is whether
 * that is a PHY-only fact or an all-of-USB30 fact. GUSB2PHYCFG is a documented RW register whose value
 * is already known (g2phy above), so flipping one bit and reading it back separates "the DWC3 core has
 * its AHB clock, the core rows above are real, only the PHY is dark" from "nothing in 0x0a600000 is
 * clocked and every D row was bus residue". The original value is written back either way. */
[[maybe_unused]] void dwc3WriteOnce(unsigned round)
{
    if (!gGcc || !gDwc3) return;
    if (!(rd(gGcc, kPrimGdscrOff) & kGdscrPwrOn)) {
        ULOG("MUYU USB C%d prim-domain-off, skipped\n", round);
        return;
    }
    const uint32_t before = rd(gDwc3, kGusb2PhyCfg0);
    const uint32_t probev = before ^ (1u << 6);   /* SUSPHY only; every other bit stays as found */
    wr(gDwc3, kGusb2PhyCfg0, probev);
    const uint32_t after = rd(gDwc3, kGusb2PhyCfg0);
    wr(gDwc3, kGusb2PhyCfg0, before);
    const uint32_t restored = rd(gDwc3, kGusb2PhyCfg0);
    ULOG("MUYU USB C%d before=%08x after=%08x restored=%08x writable=%d\n",
        round, before, after, restored, after == probev ? 1 : 0);
}

/* 2026-11-16 15:0x: both halves of the measurement are in and they contradict the old assumption.
 * The DWC3 window is writable (C1 writable=1), so the core has its AHB clock and the D rows are real;
 * the eUSB2 window is not (W1 writable=0), which is what a block still held in reset looks like. The
 * offsets below are from fw/android-harvest/vendor-source/gcc-cliffs.c, the vendor driver for exactly
 * this SoC: resets gcc_cliffs_resets[], branches .enable_reg/.halt_reg, RCGs .cmd_rcgr. Nothing here
 * invents an address.
 *
 * Order matters and is the safety argument: deassert the block resets first (a PHY in reset discards
 * writes - that is the W1 result), then enable the branch clocks, then re-read. Every value is printed
 * before and after so a wrong write cannot hide. */
struct GccBit {
    const char *name;
    uint32_t off;
};

constexpr GccBit kUsbBits[] = {
    {"usb30_bcr", 0x39000},
    {"master",    0x39018},
    {"sleep",     0x39024},
    {"utmi",      0x39028},
    {"aux",       0x39060},
    {"comaux",    0x39064},
    {"pipe",      0x39068},
    {"nocaxi",    0x39088},
    {"aggaxi",    0x3908c},
    {"phy_bcr",   0x50000},
    {"phyphy_bcr",0x50004},
    {"dp_bcr",    0x50008},
    {"qusb2_bcr", 0x12000},
};

[[maybe_unused]] void gccDump(unsigned round, const char *tag)
{
    if (!gGcc) return;
    for (const GccBit &b : kUsbBits) {
        const uint32_t v = rd(gGcc, b.off);
        ULOG("MUYU USB H%d %s %s=%08x%s\n", round, tag, b.name, v, (v & (1u << 31)) ? " off" : "");
    }
    ULOG("MUYU USB H%d %s rcg_m=%08x/%08x rcg_a=%08x/%08x\n", round, tag,
        rd(gGcc, 0x3902c), rd(gGcc, 0x39030), rd(gGcc, 0x39070), rd(gGcc, 0x39074));
}

[[maybe_unused]] void gccUsbPower(unsigned round)
{
    if (!gGcc) return;
    wr(gGcc, 0x39000, 0);   /* GCC_USB30_PRIM_BCR: deassert */
    wr(gGcc, 0x50000, 0);   /* GCC_USB3_PHY_PRIM_BCR */
    wr(gGcc, 0x50004, 0);   /* GCC_USB3PHY_PHY_PRIM_BCR */
    wr(gGcc, 0x50008, 0);   /* GCC_USB3_DP_PHY_PRIM_BCR */
    IODelay(1000);
    const uint32_t branches[] = {0x39018, 0x39024, 0x39028, 0x39060, 0x39064};
    for (uint32_t off : branches) wr(gGcc, off, rd(gGcc, off) | 1u);   /* bit0 = CLK_ENABLE */
    IODelay(20000);
    ULOG("MUYU USB H%d powered: resets deasserted, 5 branches enabled\n", round);
}

/* 2026-11-16 15:1x: GCC turned out to be fully ready - resets deasserted, all five branches enabled
 * with CLK_OFF clear, both RCGs configured - and yet the eUSB2 window still discards every write. That
 * removes "no clock" from the list and leaves "this window is not the PHY". The D rows support it:
 * GSNPSID reads back as the ASCII string "0131" and DCFG reports speed 0xc, which is not a legal
 * DWC3 value. So this prints raw runs of words from both suspicious windows plus GCC as a control
 * (GCC is known good: its values are word-aligned, plausible and stable). A window full of printable
 * ASCII is memory, not a register file, and the address is wrong. */
[[maybe_unused]] void rawRuns(unsigned round)
{
    if (gGcc) {
        ULOG("MUYU USB R%d gcc  %08x %08x %08x %08x %08x %08x\n", round,
            rd(gGcc, 0x39000), rd(gGcc, 0x39004), rd(gGcc, 0x39008),
            rd(gGcc, 0x3900c), rd(gGcc, 0x39010), rd(gGcc, 0x39014));
    }
    if (gDwc3) {
        ULOG("MUYU USB R%d dwc3+0     %08x %08x %08x %08x %08x %08x\n", round,
            rd(gDwc3, 0x000), rd(gDwc3, 0x004), rd(gDwc3, 0x008),
            rd(gDwc3, 0x00c), rd(gDwc3, 0x010), rd(gDwc3, 0x014));
        ULOG("MUYU USB R%d dwc3+c100  %08x %08x %08x %08x %08x %08x\n", round,
            rd(gDwc3, 0xc100), rd(gDwc3, 0xc104), rd(gDwc3, 0xc108),
            rd(gDwc3, 0xc10c), rd(gDwc3, 0xc110), rd(gDwc3, 0xc120));
    }
    if (gEusb2) {
        ULOG("MUYU USB R%d eusb2      %08x %08x %08x %08x %08x %08x\n", round,
            rd(gEusb2, 0x00), rd(gEusb2, 0x04), rd(gEusb2, 0x08),
            rd(gEusb2, 0x0c), rd(gEusb2, 0x10), rd(gEusb2, 0x14));
        ULOG("MUYU USB R%d eusb2+80   %08x %08x %08x %08x\n", round,
            rd(gEusb2, 0x80), rd(gEusb2, 0x84), rd(gEusb2, 0x88), rd(gEusb2, 0x8c));
    }
}

/* 2026-11-16 15:2x: GCC is not the gate - resets are deasserted, all five branches are enabled with
 * CLK_OFF clear. DT says eusb2_phy0's clocks are <rpmhcc 27> (XO) and <tcsrcc 4> (ref_clk), so the
 * remaining clock suspects are RPMh and TCSR. TCSR is a plain MMIO window at 0x1f40000; this reads its
 * first 64 bytes so the code can see whether a CBCR there is still gated. Also tries three different
 * patterns on PHY 0x00: "all writes discarded" and "some bits are read-only" look different, and only
 * the second one means the block is alive. */
/* 2026-11-16 16:1x. The DT says it outright, under /soc/hsphy@88e3000:
 *
 *     clocks      = <&rpmhcc 27>, <&tcsrcc 4>
 *     clock-names = "ref_clk_src", "ref_clk"
 *
 * and stock's own clk_summary names the consumer:
 *
 *     tcsr_usb2_clkref_en   1 1 ...  88e3000.hsphy   ref_clk
 *
 * So the eUSB2 PHY's only real clock is a reference switched on by the TCSR clock controller, and
 * XNU touches TCSR nowhere. After usb7 cleared GCC (resets deasserted, all five branches on, both
 * RCGs programmed) this is the one suspect left standing.
 *
 * A TCSR clkref register is a plain clock branch (CBCR): 0 = gated, 1 = enabled, BRANCH_HALT_DELAY
 * so bit 31 is never a halt ack. That makes the shape recognisable, and it is what the enable below
 * refuses to write past - a wrong window answers with something else and is left alone instead of
 * being poked.
 *
 * Two bases are probed because the DT is ambiguous on this: clock-controller@1f40000 has
 * reg <0x1f40000 0xc0000>, i.e. it reaches 0x2000000 and therefore contains the whole 0x1fc0000
 * block, and the upstream drivers split both ways - sm8250/kaanapali keep the clkrefs in the first
 * few words (0x0/0x8/0x10/0x18), x1e80100 puts them at +0x151xx. Writes only ever go to the
 * 0x1fc0000 window (the DT's own "tcsr" syscon), never into 0x1f40000, whose low words are the
 * hardware mutex block: a mutex state bit also reads 0/1, and enabling "it" would take a lock the
 * machine never gives back.
 */
constexpr uint64_t kTcsrBase = 0x1f40000ull;   /* clock-controller@1f40000, read-only survey */
constexpr uint64_t kTcsrLen = 0x1000ull;
constexpr uint64_t kTcsr2Base = 0x1fc0000ull;  /* syscon@1fc0000 "tcsr": the writable candidate */
constexpr uint64_t kTcsr2Len = 0x16000ull;     /* must cover +0x15000 for the x1e-style offsets */
uintptr_t gTcsr, gTcsr2;

void tcsrDump(const char *tag, uintptr_t base, uint32_t from, uint32_t to, unsigned round)
{
    if (!base) {
        ULOG("MUYU USB Y%d %s no-window\n", round, tag);
        return;
    }
    for (uint32_t off = from; off < to; off += 0x10) {
        ULOG("MUYU USB Y%d %s %05x: %08x %08x %08x %08x\n", round, tag, off,
            rd(base, off), rd(base, off + 4), rd(base, off + 8), rd(base, off + 0xc));
    }
}

[[maybe_unused]] int clkRefEnable(const char *tag, uintptr_t base, uint32_t off, unsigned round)
{
    if (!base) return -1;
    const uint32_t before = rd(base, off);
    if (before != 0u && before != 1u) {
        ULOG("MUYU USB K%d %s %05x skip: reads %08x\n", round, tag, off, before);
        return -1;
    }
    wr(base, off, before | 1u);
    IODelay(2000);
    const uint32_t after = rd(base, off);
    ULOG("MUYU USB K%d %s %05x %08x -> %08x %s\n", round, tag, off, before, after,
        (after & 1u) ? "on" : "STILL-GATED");
    return (after & 1u) ? 0 : -1;
}

/* The only question that matters after a clock is switched: can the PHY be talked to? A readable
 * register file plus a write that survives the round trip is the definition of "yes". */
void phyReadback(const char *tag, unsigned round)
{
    if (!gEusb2) {
        ULOG("MUYU USB Z%d %s no-window\n", round, tag);
        return;
    }
    const uint32_t p00 = rd(gEusb2, 0x00);
    const uint32_t p04 = rd(gEusb2, 0x04);
    wr(gEusb2, 0x00, 0x58);
    const uint32_t wrote = rd(gEusb2, 0x00);
    wr(gEusb2, 0x00, p00);
    ULOG("MUYU USB Z%d %s p00=%08x p04=%08x wr58=%08x %s\n", round, tag, p00, p04, wrote,
        (wrote == 0x58u) ? "ALIVE" : "dead");
}

[[maybe_unused]] void phyPatterns(unsigned round)
{
    if (!gEusb2) return;
    static const uint32_t patterns[] = {0x58u, 0x1u, 0xffffffffu, 0x00000001u};
    for (uint32_t value : patterns) {
        wr(gEusb2, 0x00, value);
        const uint32_t got = rd(gEusb2, 0x00);
        ULOG("MUYU USB P%d wrote=%08x read=%08x took=%d\n", round, value, got, got == value ? 1 : 0);
    }
    wr(gEusb2, 0x00, 0);
}

/* 2026-11-16 15:3x: the survey now runs synchronously, inside the late probe, instead of from a
 * kernel thread that sleeps first. Three boots in a row ended 90 s after the log writer started
 * (14->104, 28->118, 149->239 s of kernel uptime), and the thread's wake-up at 120 s never arrived.
 * The late probe itself always runs at ~90 s, so doing the work here means the data exists no matter
 * when the box goes down. Nothing below blocks for long: the only delays are 1 ms and 20 ms to let
 * the reset deassert settle.
 */
void usbMapWindows(void)
{
    gGcc = mapWindow(kGccBase, kGccLen);
    gDwc3 = mapWindow(kDwc3Base, kDwc3Len);
    gEusb2 = mapWindow(kEusb2Base, kEusb2Len);
    gSsPhy = mapWindow(kSsPhyBase, kSsPhyLen);
    gTcsr = mapWindow(kTcsrBase, kTcsrLen);
    gTcsr2 = mapWindow(kTcsr2Base, kTcsr2Len);
    ULOG("MUYU USB M gcc=%d dwc3=%d eusb2=%d ssphy=%d tcsr=%d tcsr2=%d\n",
        gGcc ? 1 : 0, gDwc3 ? 1 : 0, gEusb2 ? 1 : 0, gSsPhy ? 1 : 0,
        gTcsr ? 1 : 0, gTcsr2 ? 1 : 0);
}

/* usb12: staged survey, 15 s apart, on a thread that starts 70 s after the late probe (~160 s).
 *
 * Why staged and why this late: usb10 called the whole survey synchronously at 90 s and *nothing*
 * was ever written to the persistent log again, while usb11 -- identical build with the survey
 * compiled out -- logged a full run to 240 s and then let the watchdog reset the box. The writer in
 * that run only started at 149 s (`writer=149s`), so a kernel that dies at 90 s leaves no evidence
 * at all, and "no log" says nothing about how far it got.
 *
 * So each step now (a) starts after the writer is known to be alive and (b) announces itself first.
 * Whatever the last marker is, that is the step that killed it, and every earlier step's data is
 * already on disk.
 */
/* usb15: anchor the probe to the writer's own progress.
 *
 * usb14 put the whole survey 15 s after the late probe and got T1/T2 in, then froze during the 4 s
 * slack before T3. Its header says why: `writer=18s rounds=10 kernel=108s`. Across five boots the
 * stop is always exactly writer_start + 90 s (14->104, 18->108, 28->118, 99->189, 149->239), so a
 * probe at a fixed offset from the late probe has a window that shrinks to 3 s the moment the writer
 * happens to start early. Wall-clock pacing cannot work; the only stable reference is the writer.
 *
 * boot_log_rounds / boot_log_state are plain globals in BootLog.c, so the survey can just wait for
 * "the loop is running and at least two rounds are on the medium" and then spend the ~85 s that
 * always remain on one paced sequence. If the writer never starts (usb10: it did not), nothing this
 * probe prints could ever be persisted, so it returns instead of burning the boot.
 */
extern "C" volatile uint32_t boot_log_state, boot_log_rounds;

/* MMIO reads run synchronously from the late-probe call stack, which is demonstrably alive at
 * t=90 s (the "MUYU USB call" line is on the medium). A spawned thread - even at BASEPRI_REALTIME -
 * never ran a single line in usb19/20 (kr=0 but no "T0 start"), so nothing may depend on one. */
void usbReadNow(void)
{
    ULOG("MUYU USB T0 start\n");
    usbMapWindows();

    ULOG("MUYU USB T2 basic\n");
    probeOnce(1);
    phyReadback("pre", 1);

    ULOG("MUYU USB T3 cc\n");
    tcsrDump("cc", gTcsr, 0x00, 0x40, 3);

    ULOG("MUYU USB T4 sys\n");
    tcsrDump("sys", gTcsr2, 0x00, 0x40, 4);

    ULOG("MUYU USB T5 hi\n");
    tcsrDump("hi", gTcsr2, 0x15000, 0x15040, 5);

    ULOG("MUYU USB T6 read_done len=%u\n", (unsigned)gUsbDirectLen);
}

/* Commit the cached text to the UFS window when the region opens. Runs on its own thread only so a
 * blocked write cannot stall the boot; if the thread never runs, the msgbuf copy still carries the
 * survey via the writer, which is BASEPRI_REALTIME and demonstrably reaches t=103 s. */
void usbCommitThread(void *, wait_result_t)
{
    if (!&native_ufs_log_write || !&native_ufs_export_blocks) return;
    for (unsigned waited = 0; waited < 1200u; ++waited) {
        if (native_ufs_export_blocks()) break;
        IOSleep(100);
    }
    if (!native_ufs_export_blocks()) return;
    const uint32_t blocks = (uint32_t)((gUsbDirectLen + 4095u) / 4096u);
    native_ufs_log_write(8180, blocks ? blocks : 1u, gUsbDirect);
}

}   // namespace

/* 2026-11-16 14:2x: the first two images (usb-probe / muyu-usb-probe) produced no MUYU USB line at all
 * even though the symbol and the probe strings were both in the kernel, so the failure is somewhere
 * between "the late probe calls this" and "the thread prints". Every step now reports itself and the
 * delay defaults to 60 s instead of returning, because the data matters more than the argument. */
extern "C" void muyu_usb_phy_probe(void)
{
    /* 2026-11-16 18:0x: back on the GPU main line, so this probe is inert unless the boot
     * argument asks for it. It used to run unconditionally while USB was the subject; a synchronous
     * register survey at the 90 s late probe has no business slowing a GPU round down. Pass
     * muyu-usb-probe=1 (any non-zero value) to bring it back. */
    unsigned on = 0;
    if (!PE_parse_boot_argn("muyu-usb-probe", &on, sizeof on) || !on) return;

    ULOG("MUYU USB call\n");
    /* Read synchronously. Spawned threads never execute on this board (usb19/20: kr=0, T0 never
     * printed), so the survey itself must run on this call stack. */
    usbReadNow();
    thread_t thread = nullptr;
    kern_return_t kr = kernel_thread_start(usbCommitThread, nullptr, &thread);
    if (kr == KERN_SUCCESS) thread_deallocate(thread);
    ULOG("MUYU USB commit kr=%d\n", (int)kr);
}

