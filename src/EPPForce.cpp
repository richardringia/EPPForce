// EPPForce — forces HWP EPP to 0x10 on all logical CPUs after boot and after wake.
//
// Background (see ~/hackintosh/docs/09-cpu-power.md):
//   macOS/XCPM writes HWP_REQUEST (MSR 0x774) and HWP_REQUEST_PKG (0x772) = 0x8000ff01
//   on every core (EPP 0x80, "balanced"). Under the power limit the CPU then throttles
//   P-cores to ~4.3 GHz. Linux uses EPP 0x10 and keeps P-cores at full clocks.
//   CPUFriend's EPP is ignored by XCPM on this CPU; a direct wrmsr is what takes.
//   VoltageShift does that but exposes MSR access to any user; this kext has no
//   userspace interface.
//
// History (all on Core Ultra 7 270K Plus, macOS 26.7.1):
//   v1   wrote from start(): hung at the Apple logo.
//   v2.1 panicked: #GP on an unguarded rdmsr(0x774) in publish() (RCX=0x774, tick+0x1e7).
//        The same panic showed the kext runs ~5 s after power-on, when only the boot
//        CPU is online ("applied on 0 CPUs, 1 not ready") and HWP is still off.
//   v2.2 fixed that read but still died silently: rendezvous while APs were starting.
//   v2.3 therefore never touches an MSR or runs a rendezvous until:
//     1. hw.activecpu == hw.logicalcpu_max (every CPU online), checked via sysctl, and
//     2. a read-only probe rendezvous sees IA32_PM_ENABLE.HWP_ENABLE=1 on all of them.
//   First check is 30 s after start; publish() never touches an MSR.
//   v2.4 adds os_log breadcrumbs (IOLog never reached the unified log).
//
// Value 0x1000ff01 = min_perf 0x01, max_perf 0xff, desired 0x00, EPP 0x10.

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOMessage.h>
#include <libkern/OSAtomic.h>
#include <libkern/sysctl.h>  // sysctlbyname
#include <i386/proc_reg.h>   // wrmsr64 / rdmsr64 (inline)
#include <os/log.h>

// IOLog only reaches the verbose console; os_log is what the unified log persists to
// disk, so after a crash `log show` shows the last step that ran.
#define LOG(fmt, ...) do {                                         \
        IOLog("EPPForce: " fmt "\n", ##__VA_ARGS__);              \
        os_log(OS_LOG_DEFAULT, "EPPForce: " fmt, ##__VA_ARGS__);   \
    } while (0)

// Private kernel symbol (com.apple.kpi.unsupported): run action on every logical CPU
// with interrupts disabled.
extern "C" void mp_rendezvous_no_intrs(void (*action_func)(void *), void *arg);

#define kIA32_PM_ENABLE   0x770
#define kHWP_REQUEST_PKG  0x772
#define kHWP_REQUEST      0x774
#define kEPP_VALUE        0x1000ff01ULL   // EPP 0x10

#define kFirstDelayMS     30000           // well after AP bring-up and XCPM init
#define kRetryDelayMS     5000
#define kMaxAttempts      60              // ~5 min of retries
#define kWakeDelayMS      5000
#define kVerifyDelayMS    10000

struct ApplyCtx {
    bool          writePkg;
    volatile SInt32 applied;    // CPUs examined / written in this pass
    volatile SInt32 notReady;   // CPUs with HWP_ENABLE=0 (no MSR touched there)
    volatile SInt32 eppOk;      // CPUs whose 0x774 reads back EPP byte 0x10
    volatile uint64_t sample;   // one 0x774 value, captured only where HWP is enabled
};

// All three actions run on every CPU with interrupts off: no logging, no allocation,
// no locks. Every HWP MSR access sits below the PM_ENABLE check on that same CPU.

// Read-only: is HWP enabled on every CPU?
static void probeHwpOnThisCPU(void *arg)
{
    ApplyCtx *ctx = static_cast<ApplyCtx *>(arg);
    OSIncrementAtomic(&ctx->applied);
    if (!(rdmsr64(kIA32_PM_ENABLE) & 1ULL))
        OSIncrementAtomic(&ctx->notReady);
}

static void applyEppOnThisCPU(void *arg)
{
    ApplyCtx *ctx = static_cast<ApplyCtx *>(arg);
    if (!(rdmsr64(kIA32_PM_ENABLE) & 1ULL)) {
        OSIncrementAtomic(&ctx->notReady);
        return;
    }
    if (ctx->writePkg)
        wrmsr64(kHWP_REQUEST_PKG, kEPP_VALUE);
    wrmsr64(kHWP_REQUEST, kEPP_VALUE);
    OSIncrementAtomic(&ctx->applied);
    uint64_t rb = rdmsr64(kHWP_REQUEST);
    ctx->sample = rb;
    if (((rb >> 24) & 0xff) == 0x10)
        OSIncrementAtomic(&ctx->eppOk);
}

// Read-only: does every CPU still hold EPP 0x10? (detects XCPM overwriting us)
static void verifyEppOnThisCPU(void *arg)
{
    ApplyCtx *ctx = static_cast<ApplyCtx *>(arg);
    if (!(rdmsr64(kIA32_PM_ENABLE) & 1ULL)) {
        OSIncrementAtomic(&ctx->notReady);
        return;
    }
    OSIncrementAtomic(&ctx->applied);
    uint64_t rb = rdmsr64(kHWP_REQUEST);
    ctx->sample = rb;
    if (((rb >> 24) & 0xff) == 0x10)
        OSIncrementAtomic(&ctx->eppOk);
}

static uint32_t cpuid6Eax(void)
{
    uint32_t a = 6, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return a;
}

static int sysctlInt(const char *name)
{
    int v = 0;
    size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, nullptr, 0) != 0)
        return -1;
    return v;
}

class EPPForce : public IOService {
    OSDeclareDefaultStructors(EPPForce)

public:
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;

private:
    IONotifier         *pmNotifier { nullptr };
    IOWorkLoop         *wl         { nullptr };
    IOTimerEventSource *timer      { nullptr };
    bool                writePkg   { false };
    unsigned            attempts   { 0 };
    unsigned            wakes      { 0 };
    bool                verifyNext { false };

    void schedule(uint32_t ms);
    void tick();
    void retryOrGiveUp(const char *state, const ApplyCtx *c, const char *pass);
    void publish(const char *state, const ApplyCtx *c, const char *pass);

    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    static IOReturn sleepWakeHandler(void *target, void *refCon,
                                     UInt32 messageType, IOService *provider,
                                     void *messageArg, vm_size_t argSize);
};

OSDefineMetaClassAndStructors(EPPForce, IOService)

bool EPPForce::start(IOService *provider)
{
    LOG("start()");
    if (!IOService::start(provider))
        return false;

    uint32_t eax = cpuid6Eax();
    bool hwp = eax & (1u << 7);
    bool epp = eax & (1u << 10);
    writePkg = eax & (1u << 11);
    LOG("CPUID.06H EAX=0x%x hwp=%d epp=%d pkgReq=%d", eax, hwp, epp, writePkg);
    if (!hwp || !epp) {
        LOG("HWP/EPP not supported, doing nothing");
        return false;
    }

    wl = IOWorkLoop::workLoop();
    if (wl) timer = IOTimerEventSource::timerEventSource(this, &EPPForce::timerFired);
    if (!wl || !timer || wl->addEventSource(timer) != kIOReturnSuccess) {
        LOG("could not create timer, doing nothing");
        if (timer) { timer->release(); timer = nullptr; }
        if (wl)    { wl->release();    wl = nullptr; }
        return false;
    }

    pmNotifier = registerPrioritySleepWakeInterest(&EPPForce::sleepWakeHandler, this, nullptr);
    if (!pmNotifier)
        LOG("WARNING no sleep/wake interest; EPP will not re-apply on wake");

    attempts = 0;
    setProperty("EPPVersion", "2.4.0");
    setProperty("EPPState", "waiting");
    schedule(kFirstDelayMS);
    registerService();
    LOG("start() done, first check in %d ms", kFirstDelayMS);
    return true;
}

void EPPForce::stop(IOService *provider)
{
    if (pmNotifier) { pmNotifier->remove(); pmNotifier = nullptr; }
    if (timer) {
        timer->cancelTimeout();
        if (wl) wl->removeEventSource(timer);
        timer->release(); timer = nullptr;
    }
    if (wl) { wl->release(); wl = nullptr; }
    IOService::stop(provider);
}

void EPPForce::schedule(uint32_t ms)
{
    if (timer) timer->setTimeoutMS(ms);
}

void EPPForce::timerFired(OSObject *owner, IOTimerEventSource *)
{
    EPPForce *self = OSDynamicCast(EPPForce, owner);
    if (self) self->tick();
}

void EPPForce::publish(const char *state, const ApplyCtx *c, const char *pass)
{
    // Status only (readable via `ioreg -c EPPForce`, no root needed). No MSR access here.
    setProperty("EPPState", state);
    setProperty("EPPLastPass", pass);
    setProperty("EPPAttempts", (unsigned long long)attempts, 32);
    setProperty("EPPWakes", (unsigned long long)wakes, 32);
    setProperty("EPPCPUsExamined", (unsigned long long)c->applied, 32);
    setProperty("EPPCPUsNotReady", (unsigned long long)c->notReady, 32);
    setProperty("EPPCPUsAtEpp10", (unsigned long long)c->eppOk, 32);
    setProperty("EPPSample0x774", (unsigned long long)c->sample, 64);   // 0 = no sample
}

void EPPForce::retryOrGiveUp(const char *state, const ApplyCtx *c, const char *pass)
{
    if (attempts < kMaxAttempts) {
        publish(state, c, pass);
        schedule(kRetryDelayMS);
    } else {
        publish("gave-up", c, pass);
        LOG("giving up after %u attempts", attempts);
    }
}

void EPPForce::tick()
{
    if (verifyNext) {                        // read-only pass ~10 s after a successful apply
        verifyNext = false;
        ApplyCtx v = { writePkg, 0, 0, 0, 0 };
        LOG("verify: entering rendezvous");
        mp_rendezvous_no_intrs(verifyEppOnThisCPU, &v);
        LOG("verify: %d CPUs examined, %d at EPP 0x10, %d not ready",
              (int)v.applied, (int)v.eppOk, (int)v.notReady);
        publish((v.eppOk == v.applied && v.applied > 0 && v.notReady == 0) ? "ok-held" : "OVERWRITTEN",
                &v, "verify");
        return;
    }

    attempts++;

    // Gate 1: every CPU must be online. No rendezvous while APs are still starting.
    int active = sysctlInt("hw.activecpu");
    int maxCpu = sysctlInt("hw.logicalcpu_max");
    setProperty("EPPCPUsOnline", (unsigned long long)(active < 0 ? 0 : active), 32);
    setProperty("EPPCPUsMax",    (unsigned long long)(maxCpu < 0 ? 0 : maxCpu), 32);
    LOG("attempt %u: %d/%d CPUs online", attempts, active, maxCpu);
    ApplyCtx none = { writePkg, 0, 0, 0, 0 };
    if (active <= 0 || maxCpu <= 0 || active != maxCpu) {
        retryOrGiveUp("waiting-cpus", &none, "gate");
        return;
    }

    // Gate 2: read-only probe. HWP must be enabled on all of them before any write.
    ApplyCtx probe = { writePkg, 0, 0, 0, 0 };
    LOG("probe: entering rendezvous");
    mp_rendezvous_no_intrs(probeHwpOnThisCPU, &probe);
    LOG("probe: %d CPUs, %d with HWP off", (int)probe.applied, (int)probe.notReady);
    if (probe.notReady != 0 || probe.applied != maxCpu) {
        retryOrGiveUp("waiting-hwp", &probe, "probe");
        return;
    }

    ApplyCtx ctx = { writePkg, 0, 0, 0, 0 };
    LOG("apply: entering rendezvous (writing EPP 0x10)");
    mp_rendezvous_no_intrs(applyEppOnThisCPU, &ctx);
    LOG("apply: written on %d CPUs (%d at EPP 0x10), %d not ready",
          (int)ctx.applied, (int)ctx.eppOk, (int)ctx.notReady);

    if (ctx.notReady == 0 && ctx.applied == maxCpu) {
        publish("applied", &ctx, "apply");
        verifyNext = true;
        schedule(kVerifyDelayMS);
    } else {
        retryOrGiveUp("retrying", &ctx, "apply");
    }
}

IOReturn EPPForce::sleepWakeHandler(void *target, void *refCon,
                                    UInt32 messageType, IOService *provider,
                                    void *messageArg, vm_size_t argSize)
{
    if (messageType == kIOMessageSystemHasPoweredOn) {
        EPPForce *self = static_cast<EPPForce *>(target);
        LOG("system woke, scheduling re-apply");
        self->attempts = 0;
        self->wakes++;
        self->verifyNext = false;
        self->schedule(kWakeDelayMS);   // same gates as boot: all CPUs online + HWP enabled
    }
    return kIOReturnSuccess;
}
