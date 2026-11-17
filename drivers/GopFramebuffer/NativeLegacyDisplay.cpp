#include "NativeLegacyRequest.h"
// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reuse the unchanged original generic framebuffer implementation. Unknown
// backend leaves stop; this class is not yet a published system display.
#include "OriginalLegacyFramebufferABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
/* bsd/vfs/vfs_subr.c: the vnode pool size. fix209 prints it so the climb towards the 1950 ceiling
 * (144 s in fix208, where every later jetsam line reads 0 free) can be dated against the boot. */
extern "C" int numvnodes;
#include <kern/clock.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMpowerState.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
extern "C" __attribute__((noreturn)) void native_legacy_backend_guard(unsigned,void *,void *);
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *,uint32_t,uint32_t);
extern "C" void native_legacy_registration_observed(IOService *);
extern "C" void native_legacy_swap_stage(unsigned);
extern "C" void native_legacy_client_enter(task_t,uint32_t);
extern "C" void native_legacy_client_observed(IOReturn,IOUserClient *);
extern "C" void native_legacy_pm_observed(unsigned,IOReturn,unsigned long);
extern "C" IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *);
extern "C" uint64_t native_legacy_surface_width(OSObject *),native_legacy_surface_height(OSObject *);
extern "C" uint64_t native_legacy_surface_stride(OSObject *),native_legacy_surface_allocation(OSObject *);
extern "C" uint32_t native_legacy_surface_format(OSObject *),native_legacy_surface_planes(OSObject *);
extern "C" void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t minx,uint64_t miny,uint64_t maxx,uint64_t maxy);
extern "C" void native_legacy_frame_colors_observed(const uint32_t *colors,const uint64_t *counts,uint64_t other);
extern "C" void native_legacy_swap_validation_observed(bool,unsigned,const void *,const void *);
// The surface of a rejected swap, as the validator read it: width, height, format, planes, stride, allocation, descriptor length.
extern "C" void native_legacy_surface_rejected(uint64_t,uint64_t,uint32_t,uint32_t,uint64_t,uint64_t,uint64_t);
extern "C" bool native_legacy_prepare_scanout(void);
extern "C" void native_legacy_transfer_observed(IOReturn,const void *,uint64_t);
extern "C" void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes);
extern "C" void native_legacy_retirement_observed(IOService *);
// GPU hand-off (boot-arg sheng-fb-gpu=1): the frame is offered to the privileged GPU client instead of being copied; the swap retires when the
// client reports the GPU copy done (native_legacy_gpu_complete). A refused offer or a failed copy uses the CPU copy below and is counted.
extern "C" bool native_legacy_gpu_offer(void *display,IOMemoryDescriptor *,uint64_t stride,uint32_t width,uint32_t height);
extern "C" void native_legacy_gpu_fallback_observed(int status);
// Bring-up freeze: BravearkLastLog.c sets this when the 5 s dump takes the panel. It is a strong
// symbol in kits/muyu/LegacyDisplayProbe.cpp, compiled into the same archive. Rejecting swaps while
// it is set makes the original family print one "verify_swap failed" line per frame and floods the
// console; instead validate normally and drop the frame in kickOnce() (HANDOFF-2026-10-27 §G.4).
extern "C" volatile int native_legacy_swap_hold;
// Swaps retired when the GPU client took the frame (before its copy finished), and copies that then failed after such a retirement.
extern "C" {volatile uint64_t native_legacy_early_retires,native_legacy_early_failures;}
// Display refresh tick (boot-arg sheng-vsync=<Hz>, 0 or absent: off). The original family sends the compositor's vsync message (type 5)
// through native_legacy_vsync_notify, the unchanged original sender, but only a platform subclass calls it from the panel's vblank and
// that subclass is not part of this port: without it the render server never saw a vsync. The GOP scanout has no vblank interrupt, so a
// work-loop timer stands in for it while at least one client has vsync notifications enabled. Events: 0 armed, 1 sent, 2 idle (no client),
// 3 late (deadline passed, cadence restarted), 4 started, 5 refresh properties reported at that rate.
extern "C" void native_legacy_vsync_notify(void *display,uint64_t timestamp,uint64_t continuousTime,uint64_t interval);
// The unchanged original setDisplayRefreshProperties: publishes IOMFBDisplayRefresh from slot 301 (getRefreshAttributes below).
extern "C" bool native_legacy_set_refresh_properties(void *display);
extern "C" void native_legacy_vsync_observed(unsigned event,unsigned hz);
extern "C" uint64_t mach_continuous_time(void);
/* fix137: a panel reader that does NOT ride on a GPU submission. fix136 lost every
 * MUYU FB row after 142.076 s: framePreview() is called from NativeAdrenoSubmit::submit(),
 * so as soon as the GPU hung (HC hang at 142.559) the panel went unobserved - exactly
 * when the second writer appeared (141.671). The display work loop keeps running, so
 * sample the GOP mapping from kickOnce()/vsyncTick() instead. Sparse 48x16 grid,
 * ~2 Hz at sheng-vsync=60, plain volatile loads and no cache invalidation: fix66
 * proved that bulk traffic through a kIOMapInhibitCache alias wedges this thread. */
static void panelPreview(IOMemoryMap *mapping)
{
    if(!mapping || !mapping->getVirtualAddress() || mapping->getLength()<0x1a13000ull)return;
    static unsigned ticks,prints;
    /* fix139: fix138 only produced ONE PFB row before the GPU hung; at 32 ticks (~2 Hz)
     * that is too coarse to catch a one-shot GPU compute copy. Sample every 8 ticks
     * (~7 Hz at sheng-vsync=60) and keep the ASCII dump rate-limited separately. */
    if((++ticks & 7u))return;
    const uint32_t width=3200u,height=2136u,rows=16u,cols=48u;
    volatile uint32_t *frame=reinterpret_cast<volatile uint32_t *>(mapping->getVirtualAddress());
    static uint32_t grid[16][48],prev[16][48];
    static bool havePrev=false;
    static const char ramp[]=" .:-=+*#%@";
    char body[16][49];
    uint32_t bucket[16],count[16];
    unsigned bucketN=0,changed=0,nonzero=0,firstRow=16u,lastRow=0u;
    for(unsigned r=0;r<rows;++r) {
        const uint32_t y=(uint32_t)(((uint64_t)(2*r+1)*height)/(2*rows));
        for(unsigned c=0;c<cols;++c) {
            const uint32_t x=(uint32_t)(((uint64_t)(2*c+1)*width)/(2*cols));
            const uint32_t p=frame[(size_t)y*width+x];
            grid[r][c]=p;
            if(p)++nonzero;
            if(havePrev && p!=prev[r][c]) {++changed;if(r<firstRow)firstRow=r;if(r>lastRow)lastRow=r;}
            const uint32_t sum=((p>>16)&0xffu)*77u+((p>>8)&0xffu)*151u+(p&0xffu)*28u;
            body[r][c]=ramp[((sum>>8)*9u)/255u];
            const uint32_t key=((p>>20)&0xfu)<<8|((p>>12)&0xfu)<<4|((p>>4)&0xfu);
            unsigned slot=bucketN;
            for(unsigned k=0;k<bucketN;++k)if(bucket[k]==key) {slot=k;break;}
            if(slot==bucketN && bucketN<16) {bucket[bucketN]=key;count[bucketN]=0;++bucketN;}
            if(slot<bucketN)++count[slot];
        }
        body[r][cols]=0;
    }
    IOLog("MUYU PFB n=%u nz=%u/%u distinct=%u ch=%u/%u rows=%u..%u\n",++prints,nonzero,rows*cols,bucketN,
          changed,rows*cols,changed?firstRow:0u,changed?lastRow:0u);
    {
        char row[192];size_t n=0;
        n+=snprintf(row+n,sizeof row-n,"MUYU PFBC");
        for(unsigned k=0;k<bucketN && k<8 && n<sizeof row-24;++k)
            n+=snprintf(row+n,sizeof row-n," %03x=%u",bucket[k],count[k]);
        IOLog("%s\n",row);
    }
    if(changed || !(prints&31u))for(unsigned r=0;r<rows;++r)IOLog("MUYU PFB| %s\n",body[r]);
    for(unsigned r=0;r<rows;++r)for(unsigned c=0;c<cols;++c)prev[r][c]=grid[r][c];
    havePrev=true;
}
// 32-bit BGRA, plain ('BGRA') or in Apple's lossless-compressed spelling ('&BGA', one plane): the GPU that renders the surface here writes plain rows
// either way (no compression is implemented), so the bytes are the same.
static inline bool nativeBgra(uint32_t format,uint32_t planes) {
    return (format==0x42475241 && !planes) || (format==0x26424741 && planes<=1);
}
// 2026-11-14 01:2x, fix34: with the Adreno plugin loaded (fix31) the renderer stops producing 32-bit BGRA and hands the swap path the
// panel's own 10-bit surface instead, so every frame was refused at the surface check and nothing was ever scanned out (fix33 measured
// w=3200 h=2136 fmt='b3a8' planes=2 stride=3200 alloc=34177024, with a 409-frame reject loop behind it). 34177024 is exactly 4+1 bytes
// per pixel over 3200x2136: plane 0 is one 32-bit packed 10-10-10-2 word per pixel, plane 1 one alpha byte per pixel. This path converts
// plane 0 to BGRA; the alpha plane is not read yet, frames are scanned out opaque.
enum {NativeTenBitRgb = 0x62336138u};
static inline bool nativeTenBit(uint32_t format,uint32_t planes) {
    return format==NativeTenBitRgb && planes<=2;
}
class NativeLegacyDisplay : public IOMobileFramebufferLegacy {
    OSDeclareDefaultStructors(NativeLegacyDisplay)
    IOMemoryMap *bootMapping=nullptr;
    uint32_t scanBudget=32;
    void *shadow=nullptr;uint64_t shadowBytes=0;bool shadowValid=false;
    bool primary=false;
    bool logicalPM=false;
    bool transferred=false;
    // Every request the validator accepted and the family has not retired yet. A client may submit swap N+1 while swap N is still on the GPU
    // (int-131: swap 3 was validated during swap 2's GPU copy and one remembered pointer lost swap 2, which then never retired and left
    // every wait on it busy), so more than one can be pending.
    const OriginalLegacySwapRequest *validated[8]={};
    bool isValidated(const OriginalLegacySwapRequest *request) const {
        if(!request)return false;
        for(auto entry: validated)if(entry==request)return true;
        return false;
    }
    void removeValidated(const OriginalLegacySwapRequest *request) {for(auto &entry: validated)if(entry==request)entry=nullptr;}
    void addValidated(const OriginalLegacySwapRequest *request) {
        if(isValidated(request))return;
        for(auto &entry: validated)if(!entry){entry=request;return;}
        validated[0]=request;
    }
    const OriginalLegacySwapRequest *gpuRequest=nullptr;
    uint64_t gpuLength=0;
    bool gpuBypass=false;
    // The swap of the frame the GPU client is copying already retired (boot-arg sheng-early-retire, default 1).
    bool gpuRetired=false;
    // The refresh tick runs on the family's work loop, the loop of the command gate that registers and removes the vsync clients, so the
    // client queue at +5592 never changes while the sender walks it.
    IOTimerEventSource *vsyncTimer=nullptr;
    uint64_t vsyncInterval=0,vsyncNext=0;
    unsigned vsyncHz=0;
    bool vsyncArmed=false;
    bool vsyncWanted() const {
        const void *head=nullptr;memcpy(&head,reinterpret_cast<const unsigned char *>(this)+5592,sizeof(head));return head;
    }
    // Selector 72's registration of a vsync client ends in kickSwapQueue (slot 290), which arms the tick; the tick stops by itself once
    // the last client is removed.
    void armVsync() {
        if(!vsyncTimer || vsyncArmed || !vsyncWanted())return;
        vsyncArmed=true;vsyncNext=mach_absolute_time()+vsyncInterval;
        vsyncTimer->wakeAtTime(vsyncNext);native_legacy_vsync_observed(0,vsyncHz);
    }
    static void vsyncFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->vsyncTick();}
    /* 2026-11-16 fix207: the compositor stops submitting frames 150-200 s after the GMU cold start
     * (fix174: <=63 offers in 536 s, fix205 stopped at 5, while fix175 with the cold start aborted at
     * ph2 kept going past 192). Two different faults look identical from the offer counter: nobody
     * upstream submits, or a submitted swap sits here un-retired. The vsync timer cannot answer it -
     * armVsync() only runs once a client asks for vsync, and at power=7 no client ever does (this
     * round printed no MUYU FBW at all from the tick). So this timer is unconditional, 5 s, and
     * prints only: it never touches the swap state. */
    IOTimerEventSource *stateTimer=nullptr;
    uint64_t stateInterval=0;
    static void stateFired(OSObject *owner,IOTimerEventSource *) {static_cast<NativeLegacyDisplay *>(owner)->stateTick();}
    void stateTick() {
        static unsigned fbwPrinted;
        const OriginalLegacySwapRequest *pending=pendingSwapRequest();
        unsigned held=0;
        for(auto &entry: validated)if(entry)++held;
        IOLog("MUYU FBW n=%u primary=%u pending=%u transferred=%u gpu=%u bypass=%u held=%u want=%u armed=%u vn=%d\n",
              ++fbwPrinted,primary?1u:0u,pending?1u:0u,transferred?1u:0u,gpuRequest?1u:0u,gpuBypass?1u:0u,held,
              vsyncWanted()?1u:0u,vsyncArmed?1u:0u,numvnodes);
        if(stateTimer)stateTimer->wakeAtTime(mach_absolute_time()+stateInterval);
    }
    void startStateTimer() {
        if(stateTimer||!primary||!bootMapping)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::stateFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        nanoseconds_to_absolutetime(5000000000ull,&stateInterval);
        stateTimer=timer;
        timer->wakeAtTime(mach_absolute_time()+stateInterval);
    }
    void stopStateTimer() {
        if(!stateTimer)return;
        stateTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(stateTimer);
        OSSafeReleaseNULL(stateTimer);
    }

    void vsyncTick() {
        panelPreview(bootMapping);   /* fix137: sample the panel even with no GPU submission */
        vsyncArmed=false;
        if(!vsyncTimer)return;
        if(!vsyncWanted()){native_legacy_vsync_observed(2,vsyncHz);return;}
        const uint64_t now=mach_absolute_time();
        native_legacy_vsync_notify(this,now,mach_continuous_time(),vsyncInterval);
        native_legacy_vsync_observed(1,vsyncHz);
        vsyncNext+=vsyncInterval;
        if(vsyncNext<=now){vsyncNext=now+vsyncInterval;native_legacy_vsync_observed(3,vsyncHz);}
        vsyncArmed=true;vsyncTimer->wakeAtTime(vsyncNext);
    }
    void startVsync() {
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(!hz || hz>240 || !primary || !bootMapping || vsyncTimer)return;
        IOWorkLoop *loop=getWorkLoop();
        if(!loop)return;
        IOTimerEventSource *timer=IOTimerEventSource::timerEventSource(this,&NativeLegacyDisplay::vsyncFired);
        if(!timer)return;
        if(loop->addEventSource(timer)!=kIOReturnSuccess){timer->release();return;}
        nanoseconds_to_absolutetime(1000000000ull/hz,&vsyncInterval);
        vsyncHz=hz;vsyncTimer=timer;native_legacy_vsync_observed(4,hz);
        // A client may already have registered: arm under the gate, like the registration does.
        loop->runAction([](OSObject *owner,void *,void *,void *,void *)->IOReturn {
            static_cast<NativeLegacyDisplay *>(owner)->armVsync();return kIOReturnSuccess;
        },this);
    }
    void stopVsync() {
        if(!vsyncTimer)return;
        vsyncTimer->cancelTimeout();
        if(IOWorkLoop *loop=getWorkLoop())loop->removeEventSource(vsyncTimer);
        OSSafeReleaseNULL(vsyncTimer);vsyncArmed=false;
    }
public:
    // Called by the GPU client's completion (any thread): the swap is finished on the framebuffer's work loop.
    void gpuComplete(int status) {
        struct Args {NativeLegacyDisplay *display;int status;} args={this,status};
        IOWorkLoop *loop=getWorkLoop();
        auto action=[](OSObject *,void *a0,void *,void *,void *)->IOReturn {
            Args *a=static_cast<Args *>(a0);a->display->finishGpuSwap(a->status);return kIOReturnSuccess;
        };
        if(loop)loop->runAction(action,this,&args);else finishGpuSwap(status);
    }
    void free() override {stopVsync();stopStateTimer();if(logicalPM){PMstop();logicalPM=false;}primary=false;OSSafeReleaseNULL(bootMapping);IOMobileFramebufferLegacy::free();}
    void stop(IOService *provider) override {
        stopVsync();stopStateTimer();
        transferred=false;for(auto &entry: validated)entry=nullptr;gpuRequest=nullptr;gpuRetired=false;
        if(logicalPM){PMstop();logicalPM=false;}
        primary=false;IOMobileFramebufferLegacy::stop(provider);OSSafeReleaseNULL(bootMapping);
    }
    // The panel is already lit by the firmware's GOP framebuffer, which is the one thing the original code's power-state word (display+5840,
    // the "powered" half of the state pair next to +5836) is meant to say; the platform subclass that normally writes it is not part of this
    // port, so without it the user client's wait (selector 6) answers kIOReturnBusy forever and the compositor never presents. This is the
    // original state setter (virtual 354, the one the original code calls with this same request shape) with only the powered word named.
    void announcePoweredByFirmware() {
        struct StateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];} request={1,0,0,1,1,{}};
        typedef int (*Setter)(void *,StateRequest *,bool);
        Setter setter=reinterpret_cast<Setter *>(*reinterpret_cast<void **>(this))[354];
        setter(this,&request,false);
    }
    bool start(IOService *provider) override {
        if(!startDisplay(provider))return false;
        startVsync();startStateTimer();return true;
    }
    bool startDisplay(IOService *provider) {
        if(!IOMobileFramebufferLegacy::start(provider))return false;
        if(primary && bootMapping)announcePoweredByFirmware();
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-pm",&enabled,sizeof(enabled));
        if(!enabled)return true;
        unsigned phase=0;PE_parse_boot_argn("offline-legacy-phase",&phase,sizeof(phase));
        if(enabled!=1 || phase!=4 || !provider || !bootMapping || !primary) {
            native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;
        }
        // Actual IOKit PM lifecycle for logical scanout scheduling only. GOP
        // provides no panel power switch: neither state advertises physical
        // power output, power control or a hardware power transition.
        static IOPMPowerState states[2]={};
        states[0].version=states[1].version=1;
        states[1].capabilityFlags=kIOPMDeviceUsable;
        PMinit();logicalPM=initialized;
        if(!logicalPM){native_legacy_pm_observed(1,kIOReturnNotReady,0);IOMobileFramebufferLegacy::stop(provider);return false;}
        provider->joinPMtree(this);
        IOReturn status=registerPowerDriver(this,states,2);native_legacy_pm_observed(1,status,0);
        if(status==kIOReturnSuccess){status=changePowerStateToPriv(1);native_legacy_pm_observed(2,status,1);}
        if(status!=kIOReturnSuccess){PMstop();logicalPM=false;IOMobileFramebufferLegacy::stop(provider);return false;}
        return true;
    }
    IOReturn setPowerState(unsigned long ordinal,IOService *device) override {
        if(!logicalPM)return IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        if(ordinal>1 || !bootMapping){native_legacy_pm_observed(3,kIOReturnUnsupported,ordinal);return kIOReturnUnsupported;}
        native_legacy_pm_observed(3,kIOReturnSuccess,ordinal);
        IOReturn status=IOMobileFramebufferLegacy::setPowerState(ordinal,device);
        native_legacy_pm_observed(4,status,ordinal);return status;
    }
    // The explicit offline system-client experiment publishes only the real
    // GOP mode. Unknown backend calls retain their stop-with-caller guards.
    void registerService(IOOptionBits options=0) override {
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-register",&enabled,sizeof(enabled));
        if(enabled!=1)native_legacy_backend_guard(84,this,__builtin_return_address(0));
        // Nothing in the family publishes IOMFBDisplayRefresh (its platform subclass does), and without it the render server keeps its own
        // 1/60 s heartbeat (int-210: refresh=0). Publish it before any client can find the display, only when sheng-vsync asks for more.
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(primary && hz>60 && hz<=240)native_legacy_set_refresh_properties(this);
        IOService::registerService(options);native_legacy_registration_observed(this);
    }
    bool isMainDisplay() override {return primary;}
    // The render server paces its display timer by the minimum refresh interval in IOMFBDisplayRefresh (int-209: no vsync client ever
    // registered, frames came every 16.7 ms), and the generic slot 301 fixes it at 1/60 s. With sheng-vsync=<Hz> it reports the panel's
    // own rate instead; the maximum interval and the step keep the original values. Words in 32.32 seconds, then their mach times.
    // The original maximum interval is 1/24 s (int-240), which offers the render server a 24 to sheng-vsync Hz range. The panel has no
    // variable refresh, so sheng-vsync-fixed=1 reports the maximum interval equal to the minimum: one fixed rate.
    bool getRefreshAttributes(uint64_t *words,bool fixed) override {
        if(!IOMobileFramebufferLegacy::getRefreshAttributes(words,fixed) || !words)return false;
        unsigned hz=0;PE_parse_boot_argn("sheng-vsync",&hz,sizeof(hz));
        if(hz<=60 || hz>240)return true;
        unsigned locked=0;PE_parse_boot_argn("sheng-vsync-fixed",&locked,sizeof(locked));
        const uint64_t interval=(1ull<<32)/hz;
        uint64_t machTime=0;nanoseconds_to_absolutetime(interval*1000000000ull>>32,&machTime);
        words[0]=interval;words[1]=machTime;
        if(fixed || locked==1){words[2]=interval;words[3]=machTime;}
        native_legacy_vsync_observed(5,hz);
        return true;
    }
    // A swap the client submitted while the previous one was still on its way (transferred) is not kicked by the family again: without the
    // re-kick after each retirement it waited for the next submission (int-143: every offer came one frame late, a still screen showed the
    // previous frame until the user touched it again). A retirement inside a kick only asks the running kick to loop.
    bool kicking=false,rekick=false;
    void kickSwapQueue() override {
        armVsync();
        if(kicking){rekick=true;return;}
        kicking=true;
        do {
            rekick=false;
            kickOnce();
            gpuBypass=false;
        } while(rekick && !transferred);
        kicking=false;
    }
    void kickOnce() {
        panelPreview(bootMapping);   /* fix137: independent of the GPU submit path */
        // fix3: do NOT drop frames here. native_legacy_swap_hold is set once at t+20 s by
        // muyu_console_take_panel() and is never cleared anywhere in this tree, so an early
        // return here permanently kills the transfer / GPU-offer path: calls=0, tc=0,
        // ts=ffffffff, userspace graphics never starts and the missing -3 is a false negative
        // (GPU-HANDOFF.md section 49.6). validateSwap()'s hold branch is what actually stops the
        // "verify_swap failed" flood; keep that and let swaps flow. The 5 s ticker repaints the
        // dump, so a presented frame cannot erase the text for good.
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-transfer",&enabled,sizeof(enabled));
        if(enabled!=1 || transferred || !logicalPM || !bootMapping || !primary)return;
        const OriginalLegacySwapRequest *request=pendingSwapRequest();
        if(!request || !isValidated(request))return;
        // This bounded first-frame transfer preserves the original queue's
        // resource ownership. Separately gated retirement follows the copy;
        // neither path signals a hardware fence, announces a vblank or
        // claims general producer synchronization.
        // The owned GPU instrument completes its work before submission.
        // The separate original software-renderer experiment uses completed
        // synchronous CPU surfaces, with no GPU or display fence fabricated.
        OSObject *surface=nullptr;
        memcpy(&surface,reinterpret_cast<const unsigned char *>(request)+2624,sizeof(surface));
        if(!surface) {
            /* fix136: this branch solid-fills the whole GOP with 0xff000000. fix135 showed the
             * 8 colour bars being overwritten from 142.34 s onwards even though the 10-bit
             * conversion was skipped, so there is a SECOND writer. Count blank swaps here to
             * decide whether it is this fill or the display client's own frame. */
            static unsigned blankN;
            ++blankN;
            /* fix137: the old condition only printed on the 16th call, so a one-shot
             * blank swap was invisible. Print the first three as well. */
            if(blankN<=3 || !(blankN&15u))IOLog("MUYU BLANK n=%u\n",blankN);
            // Validated blank swap (all layers disabled over black): solid-fill the actual GOP.
            PE_Video blank={};
            uint32_t bw=0,bh=0;getDisplayDimensions(&bw,&bh);
            uint64_t blankLength=(uint64_t)bw*bh*4;
            if(PE_current_console(&blank) || blank.v_baseAddr!=bootMapping->getPhysicalAddress() ||
               blank.v_width!=bw || blank.v_height!=bh || blank.v_depth!=32 || blank.v_rotate ||
               !native_legacy_surface_bounds(bw,bh,blank.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
                native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
            if(!native_legacy_fill_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),blank.v_rowBytes,bw,bh,0xff000000U)) {
                native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            __asm__ volatile("dsb sy" ::: "memory");
            shadowValid=false;
            transferred=true;
            native_legacy_frame_observed(1,0,0,0,0,0);
            {const uint32_t colors[4]={0xff000000U,0,0,0};const uint64_t counts[4]={(uint64_t)bw*bh,0,0,0};native_legacy_frame_colors_observed(colors,counts,0);}
            native_legacy_transfer_observed(kIOReturnSuccess,request,blankLength);
            finishTransfer(request);
            return;
        }
        if(!surface->metaCast("IOSurface"))return;
        IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
        PE_Video video={};
        uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
        uint64_t length=(uint64_t)width*height*4;
        uint64_t sourceStride=native_legacy_surface_stride(surface);
        uint64_t allocation=native_legacy_surface_allocation(surface);
        const uint32_t surfaceFormat=native_legacy_surface_format(surface),surfacePlanes=native_legacy_surface_planes(surface);
        const bool tenBit=nativeTenBit(surfaceFormat,surfacePlanes);
        // Same block the validator checked: for 'b3a8' the two planes are one block of five bytes per pixel (see nativeTenBit).
        const uint64_t checkStride=tenBit?(uint64_t)width*4:sourceStride;
        const uint64_t checkAllocation=tenBit?(uint64_t)width*height*5:allocation;
        if(!memory || !memory->metaCast("IOMemoryDescriptor") || !native_legacy_surface_bounds(width,height,checkStride,checkAllocation,memory->getLength()) ||
           native_legacy_surface_width(surface)<width || native_legacy_surface_height(surface)<height ||
           !(tenBit || nativeBgra(surfaceFormat,surfacePlanes)) ||
           PE_current_console(&video) || video.v_baseAddr!=bootMapping->getPhysicalAddress() ||
           video.v_width!=width || video.v_height!=height || video.v_depth!=32 || video.v_rotate ||
           !native_legacy_surface_bounds(width,height,video.v_rowBytes,bootMapping->getLength(),bootMapping->getLength())) {
            native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        // 10-bit scanout (see nativeTenBit): the GPU offer and every copy helper below are BGRA-only, so plane 0 is converted here and the
        // frame is then scanned out exactly like a copied one. The packed word names its channels from the most significant, as the fourcc
        // does ('b3a8' = b,g,r,a): bits 20-29 blue, 10-19 green, 0-9 red. Alpha is left opaque until the second plane is confirmed.
        if(tenBit) {
            IOMemoryMap *packed=memory->map(kIOMapAnywhere|kIOMapReadOnly);
            if(!packed || !packed->getVirtualAddress() || packed->getLength()<checkAllocation) {
                OSSafeReleaseNULL(packed);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
            }
            if(!native_legacy_prepare_scanout()) {
                OSSafeReleaseNULL(packed);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
            }
            const uint8_t *plane0=reinterpret_cast<const uint8_t *>(packed->getVirtualAddress());
            volatile uint32_t *out=reinterpret_cast<volatile uint32_t *>(bootMapping->getVirtualAddress());
            const uint64_t outStride=video.v_rowBytes/4;
            const uint64_t planeBytes=(uint64_t)width*height*4;
            /* fix67: fix65 AND fix66 both wedged the display thread for the rest of the boot
             * (fix66: S1/S2 at 134.366 s, no S3 through 390 s, SWAP=0, while the GPU kept
             * submitting). Their only common addition over the healthy fix64 (448 swaps) is
             * BULK traffic through the kIOMapInhibitCache alias: 6.8M pixel loads in fix65,
             * 8342 uncached loads plus 8342 dc ivac in fix66. A non-cacheable alias of a
             * 34 MiB IOMemoryDescriptor is simply not a usable bulk read path here.
             * So: restore fix64's proven cached census verbatim, and reduce the cache
             * question to (a) ONE dc civac + reread of the first line, and (b) SIXTEEN
             * uncached words. All logs are emitted before/around those tiny probes. */
            static unsigned diagFrames;
            const unsigned frame=++diagFrames;
            const bool loud=(frame<=4 || (frame%64)==0);
            uint32_t nzu=0,wordBefore=0,wordAfter=0;
            uint32_t uncachedHeads[4]={};
            if(loud) {
                IOLog("MUYU 10BIT S1 n=%u\n",frame);
                IOMemoryMap *raw=memory->map(kIOMapAnywhere|kIOMapReadOnly|kIOMapInhibitCache);
                const uint8_t *dram=(raw && raw->getVirtualAddress() && raw->getLength()>=checkAllocation)?
                    reinterpret_cast<const uint8_t *>(raw->getVirtualAddress()):nullptr;
                IOLog("MUYU 10BIT S2 n=%u raw=%d dram=%d len=%llu\n",frame,raw?1:0,dram?1:0,
                    raw?(unsigned long long)raw->getLength():0ULL);
                /* fix75: fix67 only sampled the first 64 KiB of a 34 MiB surface, and
                 * fix74 proved the Metal frontend really commits/draws/blits
                 * (c25/0 d24 R43 D73 B5) while nz stays 0. Spread 64 uncached
                 * samples across the WHOLE allocation instead: 64 reads are harmless
                 * (fix66 wedged at 8342), but they can tell a stale-CPU-cache surface
                 * from one the GPU never wrote at all. */
                if(dram) {
                    const uint64_t span=(uint64_t)raw->getLength(),step=span/64;
                    for(unsigned k=0;k<64;++k) {
                        const uint64_t off=((uint64_t)k*step)&~uint64_t(3);
                        if(off+4>span)break;
                        const uint32_t value=*reinterpret_cast<const uint32_t *>(dram+off);
                        if(value)++nzu;
                        if(k<4)uncachedHeads[k]=value;
                    }
                }
                wordBefore=reinterpret_cast<const uint32_t *>(plane0)[0];
                __asm__ volatile("dc civac, %0"::"r"(reinterpret_cast<uintptr_t>(plane0)&~uintptr_t(63)):"memory");
                __asm__ volatile("dsb sy":::"memory");
                wordAfter=reinterpret_cast<const uint32_t *>(plane0)[0];
                IOLog("MUYU 10BIT S3 n=%u nzu=%u cb=%08x ca=%08x uh=%08x,%08x,%08x,%08x\n",
                    frame,nzu,wordBefore,wordAfter,uncachedHeads[0],uncachedHeads[1],
                    uncachedHeads[2],uncachedHeads[3]);
                OSSafeReleaseNULL(raw);
            }
            const uint64_t censusLength=(uint64_t)packed->getLength();
            uint32_t nz=0;
            uint64_t first=0;
            for(uint64_t off=0;off+4<=censusLength;off+=4096)
                if(*reinterpret_cast<const uint32_t *>(plane0+off)) {if(!nz)first=off;++nz;}
            if(loud)
                IOLog("MUYU 10BIT S4 n=%u nz=%u first=%llu heads=%08x,%08x,%08x alpha0=%02x stride=%llu alloc=%llu fmt=%08x planes=%u\n",
                    frame,nz,(unsigned long long)first,
                    reinterpret_cast<const uint32_t *>(plane0)[0],reinterpret_cast<const uint32_t *>(plane0)[1],
                    reinterpret_cast<const uint32_t *>(plane0)[2],(unsigned)plane0[planeBytes],
                    (unsigned long long)sourceStride,(unsigned long long)allocation,surfaceFormat,surfacePlanes);
            /* fix76: the physical pages behind the IOMFB swap surface. The Metal
             * frontend now prints the largest IOSurface it imported (U row); this
             * lets the next round compare the GPU's imported BO pages with the pages
             * the display is reading. Pure observation, once per loud frame. */
            if(loud) {
                IOByteCount segLen=0;
                const uint64_t phys=memory->getPhysicalSegment(0,&segLen);
                IOLog("MUYU 10BIT S5 n=%u phys=%08x%08x seg=%llu surf=%08x len=%llu\n",frame,
                    (uint32_t)(phys>>32),(uint32_t)phys,(unsigned long long)segLen,
                    (uint32_t)(uintptr_t)surface,(unsigned long long)memory->getLength());
            }
            /* fix71: REVERTED the fix68 10-bit hand-off. It did let the GPU accept a
             * frame (fix68/fix70: HANDOFF n=4), but on BOTH runs the display path then
             * froze permanently: the branch returns with transferred=true, and
             * swap() (line 262) refuses every later frame until finishGpuSwap clears
             * it. With no completeSwap coming from the scanout client for a two-plane
             * 'b3a8' surface, the frame stream stops at frame 4 (fix64/fix67 CPU path
             * ran 448+ frames). That freeze also hid whether fix70's userspace layout
             * fix ever produced pixels. Restore the proven CPU path so the census can
             * run over the whole boot; re-introduce GPU hand-off only once the surface
             * actually carries pixels and the swap completion path is understood. */
            uint64_t convertStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(convertStart));
            /* fix135: this conversion is the direct author of the black screen. `nz` counts one word every 4096
             * bytes of the 34 MiB b3a8 surface; our canaries and the CP's own 0x5a5a5a5a witness make it 4..5,
             * so the branch runs every frame and repaints ALL 3200x2136 pixels as 0xff000000 from a surface the
             * ROPs never wrote. It also returns BEFORE native_legacy_gpu_offer below, which is why MUYU OFFER
             * appears once in 390 s. fix134 painted eight full-height colour bars into the same panel buffer at
             * 137.9 s; the 138.9 s conversion erased them. Skip only the pixel stores (keep every census and the
             * transfer callback) and see whether the bars survive - that is the missing proof that the panel
             * really scans 0xE3940000. cmdline is full, so 'C' (same length as 'c') carries this switch. */
            char carrier[24]={};
            PE_parse_boot_argn("sheng-tum",carrier,sizeof(carrier));
            bool skipPaint=false,offerFrame=false;
            for(const char *c=carrier;*c;++c) {
                if(*c=='C')skipPaint=true;
                /* fix138: the user saw the eight colour bars on the real panel (§69.101), so
                 * scanout is closed and the black screen is a CONTENT problem. `swC` stopped
                 * the all-black repaint but kept the unconditional `return` at the end of this
                 * branch, which has short-circuited native_legacy_gpu_offer() ever since the
                 * 10-bit CPU conversion was added - `MUYU OFFER n=1` in 390 s. The GPU scanout
                 * client is meanwhile parked in waitSwap() (GpuScanoutClient.h::serveOne) with
                 * nothing to serve; an offer would run presenter_->present(), a compute copy
                 * from the compositor surface straight into the scanout mapping.
                 * 'D' (one more byte in the 1013/1016-B cmdline) keeps the pixel stores off AND
                 * re-connects one offer per frame. The frame still takes the PROVEN CPU tail
                 * (finishTransfer) below: fix71 showed that replacing it with the GPU tail
                 * wedges the swap queue at frame 4 when no completeSwap comes back. The boot
                 * arg is `swCD` - the C is kept so NativeAdrenoVm's CCU patch stays exactly as
                 * in fix135..137 and the offer is the only changed variable. */
                if(*c=='D'){skipPaint=true;offerFrame=true;}
            }
            {
                static unsigned tenbN;
                if(!tenbN++)IOLog("MUYU TENB skip=%u offer=%u carrier=%s\n",
                                  skipPaint?1u:0u,offerFrame?1u:0u,carrier);
            }
            if(nz && !skipPaint) {
                for(uint32_t y=0;y<height;++y) {
                    const uint32_t *row=reinterpret_cast<const uint32_t *>(plane0+(uint64_t)y*width*4);
                    volatile uint32_t *line=out+(uint64_t)y*outStride;
                    for(uint32_t x=0;x<width;++x) {
                        // Each 10-bit field keeps its top eight bits: blue 20-29, green 10-19, red 0-9 -> B,G,R,A in memory.
                        const uint32_t value=row[x];
                        line[x]=0xff000000U|(((value>>22)&0xffU)<<16)|(((value>>12)&0xffU)<<8)|((value>>2)&0xffU);
                    }
                }
                __asm__ volatile("dsb sy" ::: "memory");
                shadowValid=false;
            }
            {uint64_t convertEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(convertEnd));
             native_legacy_copy_timing_observed(convertEnd-convertStart,length);
             if(loud)
                 IOLog("MUYU 10BIT %ux%u plane0=%llu len=%llu nz=%u nzu=%u cb=%08x ca=%08x heads=%08x,%08x,%08x alpha0=%02x ticks=%llu n=%u\n",
                     width,height,(unsigned long long)planeBytes,(unsigned long long)packed->getLength(),
                     nz,nzu,wordBefore,wordAfter,
                     reinterpret_cast<const uint32_t *>(plane0)[0],reinterpret_cast<const uint32_t *>(plane0)[1],
                     reinterpret_cast<const uint32_t *>(plane0)[2],(unsigned)plane0[planeBytes],
                     (unsigned long long)(convertEnd-convertStart),frame);}
            OSSafeReleaseNULL(packed);
            if(offerFrame) {
                /* fix138: one real offer per frame, then the proven CPU tail. The presenter
                 * retains `memory` itself before returning; its completion calls
                 * native_legacy_gpu_complete -> gpuComplete -> finishGpuSwap, which returns
                 * immediately because gpuRequest is null on this path, so the swap queue cannot
                 * wedge. `MUYU OFFER` (LegacyDisplayProbe.cpp) counts the same calls. */
                static unsigned offN,offAcc;
                /* fix139: fix138 proved the offer path is live (OFF10 n=3..6 acc=1) but the
                 * panel never changed. GpuScanoutPresenter.h::present() rejects a frame with
                 * `frame.stride < width*4`; the offered stride was sourceStride=3200 (the
                 * b3a8 plane-1 row count, not a byte pitch) while width*4=12800, so the GPU
                 * client returned kBadFrame before encoding a single compute copy. `checkStride`
                 * is the same width*4 plane-0 byte pitch the validator bounds-checks with, and
                 * 12800*2135+12800=27340800 fits the 34177024-byte descriptor. Offer that. */
                const bool acc=native_legacy_gpu_offer(this,memory,checkStride,width,height);
                ++offN;if(acc)++offAcc;
                if(offN<=6 || !(offN&31u))
                    IOLog("MUYU OFF10 n=%u acc=%u total=%u fmt=%08x planes=%u stride=%llu raw=%llu bytes=%llu\n",
                          offN,acc?1u:0u,offAcc,surfaceFormat,surfacePlanes,
                          (unsigned long long)checkStride,(unsigned long long)sourceStride,
                          (unsigned long long)memory->getLength());
            }
            transferred=true;
            native_legacy_transfer_observed(kIOReturnSuccess,request,length);
            finishTransfer(request);
            return;
        }
        // The diagnostic renderer repaints the page over the scanout until it is told the system owns the display; the GPU path needs that as much
        // as the CPU copy (int-093: accepted swaps were GPU-copied under a page that kept drawing).
        if(!gpuBypass) {
            if(!native_legacy_prepare_scanout()) {native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;}
        }
        if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height)) {
            // The GPU writes the scanout memory behind the CPU's back: the differential copy's picture of it is stale from now on.
            native_legacy_swap_stage(1);
            shadowValid=false;gpuRequest=request;gpuLength=length;transferred=true;
            // The compositor renders the next frame only after this swap retires, so retiring at the end of the copy serialized render and
            // copy (int-243: about 4.4 ms render then about 3 ms copy per frame). Retire now: the presenter holds its own reference to this
            // frame's memory, the previous frame's copy has finished (transferred held this one back), and the next swap still waits for
            // the copy because transferred stays set until finishGpuSwap.
            unsigned early=1;PE_parse_boot_argn("sheng-early-retire",&early,sizeof(early));
            if(early==1 && retireSwap(request,true)) {
                gpuRetired=true;native_legacy_early_retires=native_legacy_early_retires+1;
            }
            return;
        }
        IOMemoryMap *source=memory->map(kIOMapAnywhere|kIOMapReadOnly);
        if(!source || !source->getVirtualAddress() || source->getLength()<allocation) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNoMemory,request,0);return;
        }
        if(!native_legacy_copy_layout(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
            bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
            source->getLength(),sourceStride,width,height)) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
        }
        // The two content scans read every pixel again; they only describe the first frames (the probe keeps 32).
        if(scanBudget) {
            --scanBudget;
            uint64_t scan[5];
            if(native_legacy_frame_scan(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,scan))
                native_legacy_frame_observed(0,scan[0],scan[1],scan[2],scan[3],scan[4]);
            uint32_t colors[4];uint64_t counts[4],other=0;
            if(native_legacy_frame_colors(reinterpret_cast<const void *>(source->getVirtualAddress()),source->getLength(),sourceStride,width,height,colors,counts,&other))
                native_legacy_frame_colors_observed(colors,counts,other);
        }
        if(!native_legacy_prepare_scanout()) {
            OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnNotReady,request,0);return;
        }
        uint64_t copyStart;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyStart));
        uint64_t stored=UINT64_MAX;
        {
            // Store only what changed since the previous frame (the scanout memory is slow device memory).
            uint64_t need=(uint64_t)width*height*4;
            // Differential copy is opt-in (sheng-fb-diff=1): on hardware it reads the whole source every frame and was not faster
            // than the plain copy (143-285 ms per swap either way), and the first boots with it were followed by an unresponsive
            // lock screen that disappeared with the plain copy (int-005).
            static int diffOff=-1;
            if(diffOff<0){unsigned v=0;PE_parse_boot_argn("sheng-fb-diff",&v,sizeof v);diffOff=(v!=1);}
            if(!diffOff && !shadow && need<=(64ull<<20)){shadow=IOMallocAligned((vm_size_t)need,16384);if(shadow){shadowBytes=need;shadowValid=false;}}
            if(!diffOff && shadow && shadowBytes>=need)
                stored=native_legacy_copy_diff(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                    bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                    source->getLength(),sourceStride,width,height,shadow,shadowValid);
            if(stored!=UINT64_MAX)shadowValid=true;
        }
        if(stored==UINT64_MAX) {
            shadowValid=false;
            if(!native_legacy_copy_pixels(reinterpret_cast<volatile void *>(bootMapping->getVirtualAddress()),
                bootMapping->getLength(),video.v_rowBytes,reinterpret_cast<const void *>(source->getVirtualAddress()),
                source->getLength(),sourceStride,width,height)) {
                OSSafeReleaseNULL(source);native_legacy_transfer_observed(kIOReturnBadArgument,request,0);return;
            }
            stored=length;
        }
        __asm__ volatile("dsb sy" ::: "memory");
        {uint64_t copyEnd;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(copyEnd));native_legacy_copy_timing_observed(copyEnd-copyStart,stored);}
        OSSafeReleaseNULL(source);transferred=true;
        native_legacy_transfer_observed(kIOReturnSuccess,request,length);
        finishTransfer(request);
    }
    // The GPU client reported its copy (status 0) or lost it. Success retires the swap like a finished copy; anything else uses the CPU copy.
    void finishGpuSwap(int status) {
        const OriginalLegacySwapRequest *request=gpuRequest;
        if(!request)return;
        gpuRequest=nullptr;
        if(gpuRetired) {
            // The swap retired when the client took it; its copy ending only releases the next frame. A failed copy loses this frame
            // (the next one takes the CPU copy).
            gpuRetired=false;
            if(status==0)native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            else {native_legacy_early_failures=native_legacy_early_failures+1;native_legacy_gpu_fallback_observed(status);gpuBypass=true;}
            transferred=false;kickSwapQueue();return;
        }
        if(status==0) {
            native_legacy_transfer_observed(kIOReturnSuccess,request,gpuLength);
            finishTransfer(request,true);return;
        }
        native_legacy_gpu_fallback_observed(status);
        transferred=false;gpuBypass=true;kickSwapQueue();
    }
    // Hands the pending head to the unchanged family's completion. Leaves transferred alone: the callers decide when the next frame may go.
    bool retireSwap(const OriginalLegacySwapRequest *request,bool force) {
        unsigned retire=0,cpuPresent=0;
        PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
        PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
        if(!(retire==1 || cpuPresent==1 || force))return false;
        if(!activateOnlyPendingSwap(request))return false;
        removeValidated(request);
        IOMobileFramebufferLegacy::completeCurrentSwap();
        native_legacy_retirement_observed(this);
        return true;
    }
    // Separately gated retirement after a real scanout store, shared by copied and blank frames.
    void finishTransfer(const OriginalLegacySwapRequest *request,bool force=false) {
        // Only after the real copy and store barrier: select the actual pending head and let the unchanged family release it and wake
        // waiters. This reports memory scanout completion, not a panel vblank or hardware fence.
        if(!retireSwap(request,force))return;
        // The unchanged family has retired this generation and released its current reference. Permit the next independently validated
        // single pending frame. Keep the latch set while completion can reenter this method, and whenever retirement remains disabled.
        transferred=false;
        kickSwapQueue();
    }
    IOReturn collectCompletionReports(uint32_t,void *,uint32_t *count,uint32_t) override {
        // The original completion helper passes null optional outputs and
        // ignores this IOReturn. GOP exposes no hardware completion telemetry.
        if(count)*count=0;
        return kIOReturnUnsupported;
    }
    bool validateSwap(const OriginalLegacySwapRequest *request) override {
        // HANDOFF-2026-10-27 §G.2/§G.4: while the dump owns the panel, pass validation
        // and let kickOnce() drop the frame. Returning false here floods the console with
        // "verify_swap failed for swap id: N" from the closed-source family and buries
        // every MUYU ticker line before it can be photographed.
        if(native_legacy_swap_hold && request) {
            addValidated(request);
            native_legacy_swap_validation_observed(true,0,request,nullptr);
            return true;
        }
        // Qualified original request fields are packed; use memcpy for each
        // access. Accept only one full-frame, untransformed, nonplanar BGRA
        // surface with real original backing memory. No generic capability
        // or success is supplied for other layer/format/geometry contracts.
        auto bytes=reinterpret_cast<const unsigned char *>(request);
        auto word=[&](unsigned offset){uint32_t value=0;memcpy(&value,bytes+offset,4);return value;};
        OSObject *surface=nullptr;unsigned reason=1;bool accepted=false;
        unsigned enabled=0;PE_parse_boot_argn("offline-legacy-validate",&enabled,sizeof(enabled));
        if(enabled==1 && logicalPM && primary && bootMapping && request) {
            reason=2;OSObject *surfaces[4]={};memcpy(surfaces,bytes+2624,sizeof(surfaces));surface=surfaces[0];
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            if(surface && surface->metaCast("IOSurface") && !surfaces[1] && !surfaces[2] && !surfaces[3] &&
               native_legacy_single_layer_state(word(792),word(796),word(800))) {
                reason=3;bool rectangles=true;
                const unsigned rectOffsets[]={17,81};
                for(unsigned offset: rectOffsets) {
                    uint32_t rect[4]={};memcpy(rect,bytes+offset,sizeof(rect));
                    rectangles=rectangles && rect[0]==0 && rect[1]==0 && rect[2]==width && rect[3]==height;
                }
                bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
                if(width && height && rectangles && options && word(816)==width && word(820)==height) {
                    reason=4;
                    const uint32_t surfaceFormat=native_legacy_surface_format(surface),surfacePlanes=native_legacy_surface_planes(surface);
                    const bool tenBit=nativeTenBit(surfaceFormat,surfacePlanes);
                    // 'b3a8' reports its plane stride in pixels rather than bytes, so the two planes are checked as one block of five
                    // bytes per pixel: that is what the allocation is, and kickOnce converts exactly that block.
                    const uint64_t checkStride=tenBit?(uint64_t)width*4:native_legacy_surface_stride(surface);
                    const uint64_t checkAllocation=tenBit?(uint64_t)width*height*5:native_legacy_surface_allocation(surface);
                    IOMemoryDescriptor *memory=native_legacy_surface_descriptor(surface);
                    // A surface larger than the display (rows or lines padded to the GPU's alignment) is accepted: only the visible rectangle,
                    // which the request's rectangles already name as the whole display, is copied.
                    if(native_legacy_surface_width(surface)>=width && native_legacy_surface_height(surface)>=height &&
                       (tenBit || nativeBgra(surfaceFormat,surfacePlanes)) &&
                       memory && memory->metaCast("IOMemoryDescriptor") &&
                       native_legacy_surface_bounds(width,height,checkStride,checkAllocation,memory->getLength())) {
                        reason=0;accepted=true;
                    } else {
                        native_legacy_surface_rejected(native_legacy_surface_width(surface),native_legacy_surface_height(surface),
                            native_legacy_surface_format(surface),native_legacy_surface_planes(surface),native_legacy_surface_stride(surface),
                            native_legacy_surface_allocation(surface),memory?memory->getLength():0);
                    }
                }
            }
        }
        if(!accepted && reason==2 && !surface && enabled==1) {
            // No surface in any slot and every updated layer disabled over black: present a blank screen.
            OSObject *none[4]={};memcpy(none,bytes+2624,sizeof(none));
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            bool options=true;for(unsigned offset=804;offset<=812;offset+=4)options=options && word(offset)==0;
            if(!none[0] && !none[1] && !none[2] && !none[3] && width && height && options &&
               word(816)==width && word(820)==height && native_legacy_blank_state(word(792),word(796),word(800))) {
                reason=5;accepted=true;
            }
        }
        if(accepted)addValidated(request);else removeValidated(request);
        native_legacy_swap_validation_observed(accepted,reason,request,surface);return accepted;
    }
    // The original slot-188 caller passes a uint32_t output and consumes an
    // IOReturn. Its implementation assumes an AppleARMIODevice provider.
    // Query our actual generic provider instead; GOP supplies no dot pitch.
    IOReturn getDotPitch(uint32_t *pitch) override {
        if(!pitch)return kIOReturnBadArgument;
        *pitch=0;IOService *provider=getProvider();if(!provider)return kIOReturnNotReady;
        OSObject *property=provider->getProperty("dot-pitch");
        if(!property)return kIOReturnNotFound;
        OSData *data=OSDynamicCast(OSData,property);
        if(!data || data->getLength()!=sizeof(*pitch))return kIOReturnBadArgument;
        memcpy(pitch,data->getBytesNoCopy(),sizeof(*pitch));return kIOReturnSuccess;
    }
    // Selector 74 enumerates optional frame-info trace keys, not display
    // modes. This backend implements no trace-key subscription contract.
    IOReturn supportedFrameInfo(uint32_t,uint32_t *count,char *key,uint32_t *mask) override {
        if(!count || !key || !mask)return kIOReturnBadArgument;
        *count=0;*mask=0;bzero(key,64);return kIOReturnUnsupported;
    }
    IOReturn newUserClient(task_t task,void *security,UInt32 type,IOUserClient **client) override {
        native_legacy_client_enter(task,type);
        IOReturn status=IOMobileFramebufferLegacy::newUserClient(task,security,type,client);
        native_legacy_client_observed(status,status==kIOReturnSuccess && client?*client:nullptr);return status;
    }
    bool startHardware() override {
        unsigned enabled=0;
        PE_parse_boot_argn("offline-legacy-gop",&enabled,sizeof(enabled));
        if(enabled!=1)return false;
        PE_Video video={};IOService *provider=getProvider();
        if(!provider || PE_current_console(&video) || !video.v_baseAddr ||
           !video.v_width || !video.v_height || video.v_depth!=32 || video.v_rotate ||
           video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
           video.v_rowBytes>UINT64_MAX/video.v_height)return false;
        IOMemoryMap *mapping=provider->mapDeviceMemoryWithIndex(0,kIOMapInhibitCache);
        if(!mapping)return false;
        bool valid=mapping->getVirtualAddress() && mapping->getPhysicalAddress()==video.v_baseAddr &&
                   mapping->getLength()==video.v_rowBytes*video.v_height;
        if(valid) {
            installBootDimensions((uint32_t)video.v_width,(uint32_t)video.v_height);
            uint32_t width=0,height=0;getDisplayDimensions(&width,&height);
            valid=width==video.v_width && height==video.v_height;
            if(valid) {
                OSSafeReleaseNULL(bootMapping);bootMapping=mapping;mapping=nullptr;primary=video.v_display==1;
                valid=native_legacy_gop_mapping_observed(this,bootMapping,width,height);
            }
        }
        OSSafeReleaseNULL(mapping);return valid;
    }
#define LEAF(n) __attribute__((noreturn,noinline)) void opaque##n() override { native_legacy_backend_guard(n,this,__builtin_return_address(0)); }
    LEAF(211)
    LEAF(245)
    LEAF(246)
    LEAF(306)
    LEAF(307)
    LEAF(338)
    LEAF(345)
#undef LEAF
};
OSDefineMetaClassAndStructors(NativeLegacyDisplay,IOMobileFramebufferLegacy)
IOService *native_legacy_display_allocate(void) {return new NativeLegacyDisplay;}
extern "C" void native_legacy_gpu_complete(void *display,int status) {if(display)static_cast<NativeLegacyDisplay *>(display)->gpuComplete(status);}

// muyu bring-up: the sheng/uke device kit supplies these helpers; the braveark
// kit this device repository derives from does not. Weak no-op definitions keep
// the first-console link honest until the real implementations are ported.
extern "C" {
__attribute__((weak)) void native_legacy_vsync_observed(unsigned, unsigned) {}
__attribute__((weak)) void native_buttons_set_sink(void (*)(void *, uint32_t), void *) {}
}
