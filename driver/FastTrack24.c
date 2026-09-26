/* FastTrack24: Core Audio server plug-in giving the M-Audio Fast Track Pro a working
 * 24-bit output on macOS.
 *
 * The card's 24-bit alt settings take big-endian 3-byte samples, which the macOS USB
 * audio driver cannot produce. The card does not realign to sample boundaries, so that
 * order only holds from a fresh start: stray bytes (a transfer that is not whole
 * samples) shift it for good, until the card is powered up or re-enumerated again. This
 * plug-in runs inside coreaudiod, re-enumerates the card, detaches it from usbaudiod (USB
 * configuration 2 with interface matching off) and streams to output interfaces 2 and 3
 * itself:
 *
 *   apps -> "Fast Track Pro 24-bit" (this plug-in) -> USB isochronous -> outputs 1-2, 3-4
 *
 * The device has two output streams like the card itself: channels 1-2 (interface 2, the
 * headphone jack with the A/B button out) and 3-4 (interface 3, S/PDIF out and the
 * headphone jack with the button in).
 *
 * Device clock: host time at the nominal rate (like any virtual device). The card's
 * 24-bit endpoints are adaptive, so the USB thread sends 44-45 / 47-49 samples per 1 ms
 * frame to follow a FIFO fill target instead of resampling. Samples pass through
 * unaltered at full volume.
 *
 * The device is published only while the card is attached and claimed.
 */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <unistd.h>

#pragma mark - Constants and state

enum {
    kObjPlugIn = kAudioObjectPlugInObject,
    kObjDevice = 2,
    kObjStream = 3,
    kObjVolume = 4,
    kObjMute = 5,
    kObjStream2 = 6,
};

#define kDeviceUID "FastTrack24_UID"
#define kModelUID "FastTrack24_Model"
#define kDeviceName "Fast Track Pro 24-bit"
#define kManufacturer "M-Audio"

enum {
    NCH = 2,
    ZERO_PERIOD = 16384,
    RING = 32768,          /* FIFO frames, power of two */
    TARGET = 1024,         /* FIFO fill to hold */
    ALT_24BIT = 2,         /* 24-bit, 44.1/48 kHz, adaptive */
    FRAMES_PER_SLOT = 4,   /* USB frames (ms) per isochronous transfer */
    NSLOT = 8,             /* transfers queued: 32 ms */
    MAX_PER_MS = 49,
    NIF = 2,
    NOUT = 4,              /* output channels: 1-2 on interface 2, 3-4 on interface 3 */
};
static const int OUT_IFACES[NIF] = {2, 3};
static const Float64 RATES[] = {44100.0, 48000.0};
enum { NRATES = 2 };
static const Float32 MIN_DB = -64.0f;

static os_log_t g_log;
#define LOG(...) os_log(g_log, __VA_ARGS__)

static AudioServerPlugInHostRef g_host;
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static _Atomic Float64 g_rate = 48000.0;
static _Atomic int g_present;      /* card claimed and streaming: device is published */
static _Atomic float g_volume = 1.0f;
static _Atomic int g_mute;

/* IO clock, guarded by g_mutex */
static UInt32 g_io_clients;
static Float64 g_ticks_per_frame;
static UInt64 g_anchor;
static UInt64 g_ts_count;
static Float64 g_host_ticks_per_sec;

#pragma mark - Output ring: HAL IO thread -> USB thread

/* Indexed by device sample time, so the two output streams stay sample-aligned. g_head is
 * one past the newest sample time written; the USB thread reads behind it and clears what
 * it has read. g_epoch changes when IO restarts, because sample time starts over. */
static float g_ring[RING][NOUT];
static _Atomic int64_t g_head;
static _Atomic unsigned g_epoch;

static void ring_write(int64_t t, const float *in, UInt32 n, int ch0, float gain) {
    for (UInt32 i = 0; i < n; i++) {
        float *f = g_ring[(t + i) & (RING - 1)];
        f[ch0] = in[i * 2] * gain;
        f[ch0 + 1] = in[i * 2 + 1] * gain;
    }
    int64_t end = t + n, h = atomic_load_explicit(&g_head, memory_order_relaxed);
    while (end > h && !atomic_compare_exchange_weak_explicit(&g_head, &h, end, memory_order_release, memory_order_relaxed)) {}
}

#pragma mark - USB streaming (runs on the USB thread's run loop)

static inline void put24(uint8_t *p, float x) {
    int32_t s = (int32_t)lrintf(x * 8388608.0f);
    if (s > 8388607) s = 8388607;
    if (s < -8388608) s = -8388608;
    p[0] = (uint8_t)(s >> 16);
    p[1] = (uint8_t)(s >> 8);
    p[2] = (uint8_t)s;
}

typedef struct {
    uint8_t buf[NIF][FRAMES_PER_SLOT * MAX_PER_MS * 6];
    IOUSBIsocFrame fl[NIF][FRAMES_PER_SLOT];
    UInt64 frame;   /* first USB frame of this transfer */
    int todo[NIF];  /* writes not yet accepted by the controller */
    int pending;    /* writes in flight */
    int parked;     /* waiting on a retry timer */
    int refill;     /* retry timer should refill instead of re-issuing */
} Slot;

typedef enum { ST_IDLE, ST_STREAMING, ST_STOPPING } StreamState;
typedef enum { STOP_RATE, STOP_GONE } StopReason;

static Slot g_slot[NSLOT];
static IOUSBInterfaceInterface650 **g_if[NIF];
static CFRunLoopSourceRef g_src[NIF];
static UInt8 g_pipe[NIF];
static CFRunLoopRef g_usb_rl;
static StreamState g_state = ST_IDLE;
static StopReason g_stop_reason;
static Float64 g_stream_rate;
static UInt64 g_next_frame;
static double g_acc, g_fill_lp;
static int64_t g_rpos;
static unsigned g_seen_epoch;
static int g_primed, g_inflight, g_parked, g_claim_tries;
static int g_fresh;     /* re-enumerated since anything else could have streamed to it */
static int g_resetting; /* our re-enumeration is under way: the detach that follows is ours */
static unsigned g_underruns, g_usb_errors, g_retries, g_gaps;

static void notify(AudioObjectID obj, AudioObjectPropertySelector sel) {
    AudioObjectPropertyAddress a = {sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    if (g_host) g_host->PropertiesChanged(g_host, obj, 1, &a);
}

static void set_present(int present) {
    if (atomic_exchange(&g_present, present) == present) return;
    LOG("device %{public}s", present ? "published" : "removed");
    AudioObjectPropertyAddress a[2] = {
        {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        {kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
    };
    if (g_host) g_host->PropertiesChanged(g_host, kObjPlugIn, 2, a);
    notify(kObjDevice, kAudioDevicePropertyDeviceIsAlive);
}

static void fill_slot(Slot *s) {
    int64_t head = atomic_load_explicit(&g_head, memory_order_acquire);
    unsigned epoch = atomic_load(&g_epoch);
    if (epoch != g_seen_epoch) { /* IO restarted: sample time starts over */
        g_seen_epoch = epoch;
        g_primed = 0;
    }
    if (!g_primed && head >= TARGET) {
        g_rpos = head - TARGET;
        g_primed = 1;
    }
    double fill = g_primed ? (double)(head - g_rpos) : TARGET;
    g_fill_lp += 0.02 * (fill - g_fill_lp);
    double corr = (g_fill_lp - TARGET) / TARGET * 0.002;
    if (corr > 0.001) corr = 0.001;
    if (corr < -0.001) corr = -0.001;

    UInt8 *dst[NIF];
    for (int k = 0; k < NIF; k++) dst[k] = s->buf[k];
    for (int f = 0; f < FRAMES_PER_SLOT; f++) {
        g_acc += g_stream_rate / 1000.0 * (1.0 + corr);
        int n = (int)g_acc;
        g_acc -= n;
        if (n > MAX_PER_MS) n = MAX_PER_MS;
        for (int i = 0; i < n; i++) {
            float frame[NOUT] = {0};
            if (g_primed && g_rpos < head) {
                float *src = g_ring[g_rpos & (RING - 1)];
                memcpy(frame, src, sizeof frame);
                memset(src, 0, sizeof frame);
                g_rpos++;
            } else if (g_primed) {
                g_primed = 0; /* ran dry: output silence until TARGET is buffered again */
                g_underruns++;
            }
            for (int k = 0; k < NIF; k++) {
                if (!g_if[k]) continue;
                put24(dst[k], frame[2 * k]);
                put24(dst[k] + 3, frame[2 * k + 1]);
                dst[k] += 6;
            }
        }
        for (int k = 0; k < NIF; k++) {
            s->fl[k][f].frReqCount = (UInt16)(n * 6);
            s->fl[k][f].frActCount = 0;
            s->fl[k][f].frStatus = 0;
        }
    }
}

static void submit(Slot *s);
static void on_done(void *refcon, IOReturn result, void *arg0);
static void finish_stop(void);
static void schedule_claim(double delay);

static UInt64 bus_frame(void) {
    AbsoluteTime at;
    UInt64 bus = 0;
    if (g_if[0]) (*g_if[0])->GetBusFrameNumber(g_if[0], &bus, &at);
    return bus;
}

static void maybe_finish(void) {
    if (g_state == ST_STOPPING && g_inflight == 0 && g_parked == 0) finish_stop();
}

static void begin_stop(StopReason why) {
    if (g_state != ST_STREAMING) {
        if (why == STOP_GONE) g_stop_reason = STOP_GONE; /* upgrade a pending rate change */
        return;
    }
    g_state = ST_STOPPING;
    g_stop_reason = why;
    if (why == STOP_GONE)
        for (int k = 0; k < NIF; k++)
            if (g_if[k]) (*g_if[k])->AbortPipe(g_if[k], g_pipe[k]);
    maybe_finish();
}

/* A transfer's writes have all completed (or been dropped): reuse the slot. */
static void slot_done(Slot *s) {
    if (g_state == ST_STREAMING && g_stream_rate != atomic_load(&g_rate)) begin_stop(STOP_RATE);
    if (g_state == ST_STREAMING) submit(s);
    else maybe_finish();
}

static void issue(Slot *s);

static void retry_timer(CFRunLoopTimerRef t, void *info) {
    (void)t;
    Slot *s = info;
    s->parked = 0;
    g_parked--;
    if (g_state != ST_STREAMING) {
        if (s->pending == 0) maybe_finish();
    } else if (s->refill) {
        s->refill = 0;
        if (s->pending == 0) slot_done(s);
    } else {
        issue(s);
    }
}

static void park(Slot *s, int refill) {
    CFRunLoopTimerContext ctx = {0, s, NULL, NULL, NULL};
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.001, 0, 0, 0, retry_timer, &ctx);
    CFRunLoopAddTimer(g_usb_rl, t, kCFRunLoopDefaultMode);
    CFRelease(t);
    s->parked = 1;
    s->refill = refill;
    g_parked++;
}

/* Hand the slot's writes to the controller. A write for a frame beyond the controller's
 * scheduling window is refused as "too new": keep it and retry shortly, so no audio is lost. */
static void issue(Slot *s) {
    int too_new = 0, gone = 0;
    for (int k = 0; k < NIF; k++) {
        if (!s->todo[k]) continue;
        if (!g_if[k]) {
            s->todo[k] = 0;
            continue;
        }
        IOReturn r = (*g_if[k])->WriteIsochPipeAsync(g_if[k], g_pipe[k], s->buf[k], s->frame, FRAMES_PER_SLOT, s->fl[k], on_done, s);
        if (r == kIOReturnSuccess) {
            s->todo[k] = 0;
            s->pending++;
            g_inflight++;
        } else if (r == kIOReturnIsoTooNew) {
            too_new = 1;
        } else {
            s->todo[k] = 0; /* too late or failed: this transfer is lost */
            g_gaps++;
            if (g_usb_errors++ < 10) LOG("WriteIsochPipeAsync: 0x%x (frame %llu, bus %llu)", r, s->frame, bus_frame());
            if (r == kIOReturnNoDevice || r == kIOReturnNotOpen || r == kIOReturnNotResponding) gone = 1;
        }
    }
    if (too_new) {
        g_retries++;
        park(s, 0);
    } else if (s->pending == 0) {
        park(s, 1); /* nothing in flight: refill on a timer rather than recursing */
    }
    if (gone) begin_stop(STOP_GONE);
}

static void on_done(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    Slot *s = refcon;
    g_inflight--;
    s->pending--;
    if (result != kIOReturnSuccess && result != kIOReturnUnderrun && result != kIOReturnAborted) {
        if (g_usb_errors++ < 10) LOG("iso write completed with 0x%x", result);
        if (result == kIOReturnNoDevice || result == kIOReturnNotResponding || result == kIOReturnNotOpen) begin_stop(STOP_GONE);
    }
    if (s->pending == 0 && !s->parked) slot_done(s);
}

static void submit(Slot *s) {
    UInt64 bus = bus_frame();
    if (g_next_frame < bus + 4) { /* fell behind the bus: skip ahead, leaving a gap */
        g_gaps++;
        g_next_frame = bus + 8;
    }
    fill_slot(s);
    s->frame = g_next_frame;
    g_next_frame += FRAMES_PER_SLOT;
    for (int k = 0; k < NIF; k++) s->todo[k] = g_if[k] != NULL;
    issue(s);
}

static void stats_timer(CFRunLoopTimerRef t, void *info) {
    (void)t;
    (void)info;
    static unsigned last[4];
    static int reported;
    unsigned now[4] = {g_retries, g_gaps, g_underruns, g_usb_errors};
    if (reported && memcmp(now, last, sizeof now) == 0) return; /* quiet while healthy */
    reported = 1;
    LOG("last 30 s: %u retries, %u gaps, %u underruns, %u errors", now[0] - last[0], now[1] - last[1], now[2] - last[2],
        now[3] - last[3]);
    memcpy(last, now, sizeof now);
}

static int set_endpoint_rate(IOUSBInterfaceInterface650 **intf, UInt8 pipe, Float64 rate) {
    UInt8 dir, num, type, interval;
    UInt16 mps;
    (*intf)->GetPipeProperties(intf, pipe, &dir, &num, &type, &mps, &interval);
    UInt32 hz = (UInt32)rate;
    UInt8 data[3] = {hz & 0xff, (hz >> 8) & 0xff, (hz >> 16) & 0xff};
    IOUSBDevRequest req = {.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBEndpoint),
                           .bRequest = 0x01, /* SET_CUR */
                           .wValue = 0x0100, /* SAMPLING_FREQ_CONTROL */
                           .wIndex = num, .wLength = 3, .pData = data};
    IOReturn r = (*intf)->ControlRequest(intf, 0, &req);
    if (r) LOG("set rate %u on endpoint %u: 0x%x", hz, num, r);
    return r == kIOReturnSuccess;
}

static void start_stream(void) {
    g_stream_rate = atomic_load(&g_rate);
    for (int k = 0; k < NIF; k++)
        if (g_if[k]) set_endpoint_rate(g_if[k], g_pipe[k], g_stream_rate);
    g_acc = 0;
    g_fill_lp = TARGET;
    g_primed = 0;
    g_next_frame = bus_frame() + 16; /* first submissions can be slow: leave headroom */
    g_state = ST_STREAMING;
    for (int i = 0; i < NSLOT && g_state == ST_STREAMING; i++) submit(&g_slot[i]);
    LOG("streaming 24-bit / %.0f Hz", g_stream_rate);
}

static IOUSBDeviceInterface650 **open_device_iface(void) {
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    int v = 0x0763, p = 0x2012;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), nv);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), np);
    CFRelease(nv);
    CFRelease(np);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!svc) return NULL;
    IOCFPlugInInterface **plug = NULL;
    SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (kr || !plug) {
        LOG("device plug-in: 0x%x", kr);
        return NULL;
    }
    IOUSBDeviceInterface650 **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&dev);
    (*plug)->Release(plug);
    return dev;
}

/* Re-enumerate the card, which puts its sample alignment back to the power-up state.
 * It comes back as a new device in configuration 1; on_arrive claims it again. */
static int reset_card(void) {
    IOUSBDeviceInterface650 **dev = open_device_iface();
    if (!dev) return 0;
    IOReturn r = (*dev)->USBDeviceOpenSeize(dev);
    if (r == kIOReturnSuccess) {
        r = (*dev)->USBDeviceReEnumerate(dev, 0);
        if (r) (*dev)->USBDeviceClose(dev);
    }
    (*dev)->Release(dev);
    if (r) LOG("re-enumerate: 0x%x", r);
    return r == kIOReturnSuccess;
}

/* Configuration 2 with interface matching off: usbaudiod stays detached. */
static int claim_config(void) {
    IOUSBDeviceInterface650 **dev = open_device_iface();
    if (!dev) return 0;
    IOReturn r = (*dev)->USBDeviceOpenSeize(dev);
    if (r == kIOReturnSuccess) {
        (*dev)->SetConfigurationV2(dev, 1, false, false);
        r = (*dev)->SetConfigurationV2(dev, 2, false, false);
        (*dev)->USBDeviceClose(dev);
    }
    (*dev)->Release(dev);
    if (r) LOG("claim configuration 2: 0x%x", r);
    return r == kIOReturnSuccess;
}

static IOUSBInterfaceInterface650 **open_interface(int num) {
    IOUSBDeviceInterface650 **dev = open_device_iface();
    if (!dev) return NULL;
    IOUSBFindInterfaceRequest req = {kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare};
    io_iterator_t it = 0;
    IOReturn r = (*dev)->CreateInterfaceIterator(dev, &req, &it);
    (*dev)->Release(dev);
    if (r) return NULL;
    io_service_t s, found = 0;
    while ((s = IOIteratorNext(it))) {
        CFNumberRef n = IORegistryEntryCreateCFProperty(s, CFSTR(kUSBInterfaceNumber), NULL, 0);
        int in = -1;
        if (n) {
            CFNumberGetValue(n, kCFNumberSInt32Type, &in);
            CFRelease(n);
        }
        if (in == num && !found) found = s;
        else IOObjectRelease(s);
    }
    IOObjectRelease(it);
    if (!found) return NULL;
    IOCFPlugInInterface **plug = NULL;
    SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(found, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(found);
    if (kr || !plug) {
        LOG("interface %d plug-in: 0x%x", num, kr);
        return NULL;
    }
    IOUSBInterfaceInterface650 **intf = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650), (LPVOID *)&intf);
    (*plug)->Release(plug);
    if (!intf) return NULL;
    r = (*intf)->USBInterfaceOpenSeize(intf);
    if (r) {
        LOG("open interface %d: 0x%x", num, r);
        (*intf)->Release(intf);
        return NULL;
    }
    r = (*intf)->SetAlternateInterface(intf, ALT_24BIT);
    if (r) LOG("interface %d alt %d: 0x%x", num, ALT_24BIT, r);
    return intf;
}

static UInt8 find_iso_out_pipe(IOUSBInterfaceInterface650 **intf) {
    UInt8 nep = 0;
    (*intf)->GetNumEndpoints(intf, &nep);
    for (UInt8 i = 1; i <= nep; i++) {
        UInt8 dir, num, type, interval;
        UInt16 mps;
        (*intf)->GetPipeProperties(intf, i, &dir, &num, &type, &mps, &interval);
        if (dir == kUSBOut && type == kUSBIsoc) return i;
    }
    return 0;
}

static void close_interfaces(void) {
    for (int k = 0; k < NIF; k++) {
        if (g_src[k]) {
            CFRunLoopRemoveSource(g_usb_rl, g_src[k], kCFRunLoopDefaultMode);
            g_src[k] = NULL;
        }
        if (!g_if[k]) continue;
        (*g_if[k])->SetAlternateInterface(g_if[k], 0);
        (*g_if[k])->USBInterfaceClose(g_if[k]);
        (*g_if[k])->Release(g_if[k]);
        g_if[k] = NULL;
    }
}

static void finish_stop(void) {
    if (g_stop_reason == STOP_RATE) {
        LOG("restarting stream for the new sample rate");
        start_stream();
        return;
    }
    g_state = ST_IDLE;
    close_interfaces();
    set_present(0);
    /* If the card is still attached (a transient USB error), claim it again. */
    g_fresh = 0;
    g_claim_tries = 0;
    schedule_claim(2.0);
}

static void try_claim(void) {
    if (g_state != ST_IDLE) return;
    if (!g_fresh) {
        g_fresh = 1; /* reset only once: if it fails, stream anyway */
        if (reset_card()) {
            LOG("re-enumerating the card for a clean start");
            g_resetting = 1;
            schedule_claim(5.0); /* in case the card does not come back as a new device */
            return;
        }
    }
    if (!claim_config()) goto retry;
    usleep(500000); /* let the configuration-2 interfaces appear */
    for (int k = 0; k < NIF; k++) {
        g_if[k] = open_interface(OUT_IFACES[k]);
        if (!g_if[k] || !(g_pipe[k] = find_iso_out_pipe(g_if[k]))) {
            close_interfaces();
            goto retry;
        }
        (*g_if[k])->CreateInterfaceAsyncEventSource(g_if[k], &g_src[k]);
        CFRunLoopAddSource(g_usb_rl, g_src[k], kCFRunLoopDefaultMode);
    }
    g_claim_tries = 0;
    start_stream();
    set_present(1);
    return;
retry:
    if (++g_claim_tries < 6) schedule_claim(1.0);
    else LOG("giving up on the Fast Track Pro until it is plugged in again");
}

static void claim_timer(CFRunLoopTimerRef t, void *info) {
    (void)t;
    (void)info;
    try_claim();
}

static void schedule_claim(double delay) {
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + delay, 0, 0, 0, claim_timer, NULL);
    CFRunLoopAddTimer(g_usb_rl, t, kCFRunLoopDefaultMode);
    CFRelease(t);
}

static void on_arrive(void *refcon, io_iterator_t it) {
    (void)refcon;
    io_service_t s;
    int any = 0;
    while ((s = IOIteratorNext(it))) {
        IOObjectRelease(s);
        any = 1;
    }
    if (any) {
        LOG("Fast Track Pro attached");
        g_resetting = 0;
        g_claim_tries = 0;
        schedule_claim(0.5);
    }
}

static void on_leave(void *refcon, io_iterator_t it) {
    (void)refcon;
    io_service_t s;
    int any = 0;
    while ((s = IOIteratorNext(it))) {
        IOObjectRelease(s);
        any = 1;
    }
    if (any) {
        LOG("Fast Track Pro detached");
        if (!g_resetting) g_fresh = 0;
        begin_stop(STOP_GONE);
        if (g_state == ST_IDLE) {
            close_interfaces();
            set_present(0);
        }
    }
}

static CFMutableDictionaryRef card_matching(void) {
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    int v = 0x0763, p = 0x2012;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), nv);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), np);
    CFRelease(nv);
    CFRelease(np);
    return m;
}

/* The USB thread must resubmit transfers within a few ms of each completion. At normal
 * priority inside the Core Audio helper process it gets starved for 20+ ms and the queue
 * runs dry, so ask for a real-time slot like Core Audio's own IO threads. The work per
 * wake-up is tiny; blocking calls (claiming the card) are fine for a real-time thread. */
static void make_realtime(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    double ticks_per_ms = 1e6 * (double)tb.denom / (double)tb.numer;
    thread_time_constraint_policy_data_t pol = {
        .period = (uint32_t)(FRAMES_PER_SLOT * ticks_per_ms),
        .computation = (uint32_t)(0.5 * ticks_per_ms),
        .constraint = (uint32_t)(2 * ticks_per_ms),
        .preemptible = 1,
    };
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                         (thread_policy_t)&pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr) LOG("real-time scheduling refused: %d", kr);
}

static void *usb_thread(void *arg) {
    (void)arg;
    pthread_setname_np("FastTrack24 USB");
    make_realtime();
    g_usb_rl = CFRunLoopGetCurrent();
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(g_usb_rl, IONotificationPortGetRunLoopSource(port), kCFRunLoopDefaultMode);
    io_iterator_t add_it = 0, rm_it = 0;
    IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, card_matching(), on_arrive, NULL, &add_it);
    IOServiceAddMatchingNotification(port, kIOTerminatedNotification, card_matching(), on_leave, NULL, &rm_it);
    on_leave(NULL, rm_it);
    on_arrive(NULL, add_it);
    CFRunLoopTimerRef stats = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 30, 30, 0, 0, stats_timer, NULL);
    CFRunLoopAddTimer(g_usb_rl, stats, kCFRunLoopDefaultMode);
    CFRunLoopRun();
    return NULL;
}

#pragma mark - Helpers

static int rate_supported(Float64 r) {
    for (int i = 0; i < NRATES; i++)
        if (r == RATES[i]) return 1;
    return 0;
}

static AudioStreamBasicDescription format_at(Float64 rate) {
    AudioStreamBasicDescription f = {
        .mSampleRate = rate,
        .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked,
        .mBytesPerPacket = 4 * NCH,
        .mFramesPerPacket = 1,
        .mBytesPerFrame = 4 * NCH,
        .mChannelsPerFrame = NCH,
        .mBitsPerChannel = 32,
    };
    return f;
}

static UInt32 latency_frames(void) {
    return TARGET + (UInt32)(NSLOT * FRAMES_PER_SLOT * atomic_load(&g_rate) / 1000.0);
}

static Float32 scalar_to_db(Float32 s) {
    if (s <= 0) return MIN_DB;
    Float32 db = 40.0f * log10f(s); /* gain = scalar^2 */
    return db < MIN_DB ? MIN_DB : db;
}

static Float32 db_to_scalar(Float32 db) {
    if (db <= MIN_DB) return 0;
    if (db > 0) db = 0;
    return powf(10.0f, db / 40.0f);
}

static void save_settings(void) {
    if (!g_host) return;
    float v = atomic_load(&g_volume);
    int m = atomic_load(&g_mute);
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberFloat32Type, &v);
    CFNumberRef nm = CFNumberCreate(NULL, kCFNumberIntType, &m);
    g_host->WriteToStorage(g_host, CFSTR("volume"), nv);
    g_host->WriteToStorage(g_host, CFSTR("mute"), nm);
    CFRelease(nv);
    CFRelease(nm);
}

static void load_settings(void) {
    CFPropertyListRef v = NULL;
    if (g_host->CopyFromStorage(g_host, CFSTR("volume"), &v) == 0 && v) {
        float f;
        if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberFloat32Type, &f))
            atomic_store(&g_volume, f < 0 ? 0 : f > 1 ? 1 : f);
        CFRelease(v);
    }
    v = NULL;
    if (g_host->CopyFromStorage(g_host, CFSTR("mute"), &v) == 0 && v) {
        int m;
        if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberIntType, &m)) atomic_store(&g_mute, m != 0);
        CFRelease(v);
    }
    v = NULL;
    if (g_host->CopyFromStorage(g_host, CFSTR("rate"), &v) == 0 && v) {
        double r;
        if (CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberDoubleType, &r) && rate_supported(r))
            atomic_store(&g_rate, r);
        CFRelease(v);
    }
}

#pragma mark - Properties

/* One place that knows every property. out may be NULL (size query / HasProperty).
 * For in-out properties (dB conversions), *io_value carries the input. */
#define PUT(type, value)                                  \
    do {                                                  \
        *size = sizeof(type);                             \
        if (out) *(type *)out = (value);                  \
        return 0;                                         \
    } while (0)
#define PUT_ARRAY(type, arr, count)                                   \
    do {                                                              \
        *size = (UInt32)(sizeof(type) * (count));                     \
        if (out) memcpy(out, (arr), *size);                           \
        return 0;                                                     \
    } while (0)

static OSStatus get_prop(AudioObjectID obj, const AudioObjectPropertyAddress *a, UInt32 qsz, const void *q,
                         void *out, UInt32 *size) {
    int present = atomic_load(&g_present);
    switch (obj) {
    case kObjPlugIn:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR(kManufacturer));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            AudioObjectID d = kObjDevice;
            PUT_ARRAY(AudioObjectID, &d, present ? 1 : 0);
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            AudioObjectID d = kAudioObjectUnknown;
            if (present && qsz == sizeof(CFStringRef) && q && CFEqual(*(CFStringRef *)q, CFSTR(kDeviceUID))) d = kObjDevice;
            PUT(AudioObjectID, d);
        }
        case kAudioPlugInPropertyResourceBundle: PUT(CFStringRef, CFSTR(""));
        }
        break;

    case kObjDevice: {
        int outScope = a->mScope != kAudioObjectPropertyScopeInput;
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjPlugIn);
        case kAudioObjectPropertyName: PUT(CFStringRef, CFSTR(kDeviceName));
        case kAudioObjectPropertyManufacturer: PUT(CFStringRef, CFSTR(kManufacturer));
        case kAudioObjectPropertyOwnedObjects: {
            AudioObjectID o[4] = {kObjStream, kObjStream2, kObjVolume, kObjMute};
            PUT_ARRAY(AudioObjectID, o, outScope ? 4 : 0);
        }
        case kAudioObjectPropertyControlList: {
            AudioObjectID o[2] = {kObjVolume, kObjMute};
            PUT_ARRAY(AudioObjectID, o, 2);
        }
        case kAudioDevicePropertyDeviceUID: PUT(CFStringRef, CFSTR(kDeviceUID));
        case kAudioDevicePropertyModelUID: PUT(CFStringRef, CFSTR(kModelUID));
        case kAudioDevicePropertyTransportType: PUT(UInt32, kAudioDeviceTransportTypeUSB);
        case kAudioDevicePropertyRelatedDevices: {
            AudioObjectID d = kObjDevice;
            PUT_ARRAY(AudioObjectID, &d, 1);
        }
        case kAudioDevicePropertyClockDomain: PUT(UInt32, 0);
        case kAudioDevicePropertyDeviceIsAlive: PUT(UInt32, present ? 1 : 0);
        case kAudioDevicePropertyDeviceIsRunning: {
            pthread_mutex_lock(&g_mutex);
            UInt32 running = g_io_clients > 0;
            pthread_mutex_unlock(&g_mutex);
            PUT(UInt32, running);
        }
        case kAudioDevicePropertyDeviceCanBeDefaultDevice: PUT(UInt32, outScope ? 1 : 0);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: PUT(UInt32, outScope ? 1 : 0);
        case kAudioDevicePropertyLatency: PUT(UInt32, outScope ? latency_frames() : 0);
        case kAudioDevicePropertySafetyOffset: PUT(UInt32, 0);
        case kAudioDevicePropertyStreams: {
            AudioObjectID st[2] = {kObjStream, kObjStream2};
            PUT_ARRAY(AudioObjectID, st, outScope ? 2 : 0);
        }
        case kAudioDevicePropertyNominalSampleRate: PUT(Float64, atomic_load(&g_rate));
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            AudioValueRange r[NRATES];
            for (int i = 0; i < NRATES; i++) r[i].mMinimum = r[i].mMaximum = RATES[i];
            PUT_ARRAY(AudioValueRange, r, NRATES);
        }
        case kAudioDevicePropertyIsHidden: PUT(UInt32, 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: PUT(UInt32, ZERO_PERIOD);
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            UInt32 ch[2] = {1, 2};
            PUT_ARRAY(UInt32, ch, 2);
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            AudioChannelLayout l = {.mChannelLayoutTag = kAudioChannelLayoutTag_DiscreteInOrder | NOUT};
            PUT(AudioChannelLayout, l);
        }
        case kAudioObjectPropertyElementName: {
            static const CFStringRef names[NOUT] = {CFSTR("Output 1"), CFSTR("Output 2"), CFSTR("Output 3"), CFSTR("Output 4")};
            if (!outScope || a->mElement < 1 || a->mElement > NOUT) break;
            PUT(CFStringRef, names[a->mElement - 1]);
        }
        }
        break;
    }

    case kObjStream:
    case kObjStream2:
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyName:
            PUT(CFStringRef, obj == kObjStream ? CFSTR("Outputs 1-2 (headphones A)") : CFSTR("Outputs 3-4 (headphones B, S/PDIF)"));
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjDevice);
        case kAudioObjectPropertyOwnedObjects: PUT_ARRAY(AudioObjectID, NULL, 0);
        case kAudioStreamPropertyIsActive: PUT(UInt32, 1);
        case kAudioStreamPropertyDirection: PUT(UInt32, 0); /* output */
        case kAudioStreamPropertyTerminalType: PUT(UInt32, kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: PUT(UInt32, obj == kObjStream ? 1 : 3);
        case kAudioStreamPropertyLatency: PUT(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: PUT(AudioStreamBasicDescription, format_at(atomic_load(&g_rate)));
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            AudioStreamRangedDescription d[NRATES];
            for (int i = 0; i < NRATES; i++) {
                d[i].mFormat = format_at(RATES[i]);
                d[i].mSampleRateRange.mMinimum = d[i].mSampleRateRange.mMaximum = RATES[i];
            }
            PUT_ARRAY(AudioStreamRangedDescription, d, NRATES);
        }
        }
        break;

    case kObjVolume:
    case kObjMute: {
        int vol = obj == kObjVolume;
        switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass: PUT(AudioClassID, vol ? kAudioLevelControlClassID : kAudioBooleanControlClassID);
        case kAudioObjectPropertyClass: PUT(AudioClassID, vol ? kAudioVolumeControlClassID : kAudioMuteControlClassID);
        case kAudioObjectPropertyOwner: PUT(AudioObjectID, kObjDevice);
        case kAudioObjectPropertyOwnedObjects: PUT_ARRAY(AudioObjectID, NULL, 0);
        case kAudioControlPropertyScope: PUT(AudioObjectPropertyScope, kAudioObjectPropertyScopeOutput);
        case kAudioControlPropertyElement: PUT(AudioObjectPropertyElement, kAudioObjectPropertyElementMain);
        }
        if (vol) {
            switch (a->mSelector) {
            case kAudioLevelControlPropertyScalarValue: PUT(Float32, atomic_load(&g_volume));
            case kAudioLevelControlPropertyDecibelValue: PUT(Float32, scalar_to_db(atomic_load(&g_volume)));
            case kAudioLevelControlPropertyDecibelRange: {
                AudioValueRange r = {MIN_DB, 0};
                PUT(AudioValueRange, r);
            }
            case kAudioLevelControlPropertyConvertScalarToDecibels:
                *size = sizeof(Float32);
                if (out) *(Float32 *)out = scalar_to_db(*(Float32 *)out);
                return 0;
            case kAudioLevelControlPropertyConvertDecibelsToScalar:
                *size = sizeof(Float32);
                if (out) *(Float32 *)out = db_to_scalar(*(Float32 *)out);
                return 0;
            }
        } else if (a->mSelector == kAudioBooleanControlPropertyValue) {
            PUT(UInt32, (UInt32)atomic_load(&g_mute));
        }
        break;
    }
    default:
        return kAudioHardwareBadObjectError;
    }
    return kAudioHardwareUnknownPropertyError;
}

static int settable(AudioObjectID obj, AudioObjectPropertySelector sel) {
    switch (obj) {
    case kObjDevice: return sel == kAudioDevicePropertyNominalSampleRate;
    case kObjStream:
    case kObjStream2:
        return sel == kAudioStreamPropertyVirtualFormat || sel == kAudioStreamPropertyPhysicalFormat ||
               sel == kAudioStreamPropertyIsActive;
    case kObjVolume: return sel == kAudioLevelControlPropertyScalarValue || sel == kAudioLevelControlPropertyDecibelValue;
    case kObjMute: return sel == kAudioBooleanControlPropertyValue;
    }
    return 0;
}

static OSStatus request_rate(Float64 r) {
    if (!rate_supported(r)) return kAudioHardwareIllegalOperationError;
    if (r == atomic_load(&g_rate)) return 0;
    return g_host->RequestDeviceConfigurationChange(g_host, kObjDevice, (UInt64)r, NULL);
}

#pragma mark - Driver interface

static HRESULT QueryInterface(void *drv, REFIID iid, LPVOID *out);
static ULONG AddRef(void *drv);
static ULONG Release(void *drv);

static _Atomic ULONG g_refs = 1;

static OSStatus Initialize(AudioServerPlugInDriverRef drv, AudioServerPlugInHostRef host) {
    (void)drv;
    g_host = host;
    g_log = os_log_create("com.github.nclr.fasttrack24", "driver");
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_host_ticks_per_sec = 1e9 * (Float64)tb.denom / (Float64)tb.numer;
    load_settings();
    g_ticks_per_frame = g_host_ticks_per_sec / atomic_load(&g_rate);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &attr, usb_thread, NULL);
    pthread_attr_destroy(&attr);
    LOG("initialized");
    return 0;
}

static OSStatus CreateDevice(AudioServerPlugInDriverRef d, CFDictionaryRef desc, const AudioServerPlugInClientInfo *ci, AudioObjectID *out) {
    (void)d; (void)desc; (void)ci; (void)out;
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus DestroyDevice(AudioServerPlugInDriverRef d, AudioObjectID dev) {
    (void)d; (void)dev;
    return kAudioHardwareUnsupportedOperationError;
}
static OSStatus AddDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci) {
    (void)d; (void)dev; (void)ci;
    return 0;
}
static OSStatus RemoveDeviceClient(AudioServerPlugInDriverRef d, AudioObjectID dev, const AudioServerPlugInClientInfo *ci) {
    (void)d; (void)dev; (void)ci;
    return 0;
}

static OSStatus PerformDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info) {
    (void)d; (void)info;
    if (dev != kObjDevice) return kAudioHardwareBadObjectError;
    Float64 r = (Float64)action;
    if (!rate_supported(r)) return kAudioHardwareIllegalOperationError;
    pthread_mutex_lock(&g_mutex);
    atomic_store(&g_rate, r);
    g_ticks_per_frame = g_host_ticks_per_sec / r;
    pthread_mutex_unlock(&g_mutex);
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberDoubleType, &r);
    g_host->WriteToStorage(g_host, CFSTR("rate"), n);
    CFRelease(n);
    LOG("sample rate %.0f", r); /* the USB thread restarts the stream on its next completion */
    return 0;
}

static OSStatus AbortDeviceConfigurationChange(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt64 action, void *info) {
    (void)d; (void)dev; (void)action; (void)info;
    return 0;
}

static Boolean HasProperty(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a) {
    (void)d; (void)pid;
    UInt32 size = 0;
    return get_prop(obj, a, 0, NULL, NULL, &size) == 0;
}

static OSStatus IsPropertySettable(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a, Boolean *out) {
    (void)d; (void)pid;
    UInt32 size = 0;
    OSStatus s = get_prop(obj, a, 0, NULL, NULL, &size);
    if (s) return s;
    *out = settable(obj, a->mSelector);
    return 0;
}

static OSStatus GetPropertyDataSize(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                    UInt32 qsz, const void *q, UInt32 *outSize) {
    (void)d; (void)pid;
    return get_prop(obj, a, qsz, q, NULL, outSize);
}

static OSStatus GetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qsz, const void *q, UInt32 inSize, UInt32 *outSize, void *out) {
    (void)d; (void)pid;
    union {
        uint8_t bytes[512];
        Float32 f;
    } tmp;
    UInt32 need = 0;
    OSStatus s = get_prop(obj, a, qsz, q, NULL, &need);
    if (s) return s;
    if (need > sizeof tmp) return kAudioHardwareUnspecifiedError;
    /* in-out conversions carry their input in the caller's buffer */
    if (inSize >= sizeof(Float32)) memcpy(&tmp.f, out, sizeof(Float32));
    get_prop(obj, a, qsz, q, &tmp, &need);
    int is_array = need > 0 && (a->mSelector == kAudioObjectPropertyOwnedObjects || a->mSelector == kAudioPlugInPropertyDeviceList ||
                                a->mSelector == kAudioObjectPropertyControlList || a->mSelector == kAudioDevicePropertyStreams ||
                                a->mSelector == kAudioDevicePropertyRelatedDevices ||
                                a->mSelector == kAudioDevicePropertyAvailableNominalSampleRates ||
                                a->mSelector == kAudioStreamPropertyAvailableVirtualFormats ||
                                a->mSelector == kAudioStreamPropertyAvailablePhysicalFormats);
    if (inSize < need) {
        if (!is_array) return kAudioHardwareBadPropertySizeError;
        need = inSize; /* return as many whole items as fit */
    }
    memcpy(out, &tmp, need);
    *outSize = need;
    return 0;
}

static OSStatus SetPropertyData(AudioServerPlugInDriverRef d, AudioObjectID obj, pid_t pid, const AudioObjectPropertyAddress *a,
                                UInt32 qsz, const void *q, UInt32 inSize, const void *in) {
    (void)d; (void)pid; (void)qsz; (void)q;
    if (!settable(obj, a->mSelector)) {
        UInt32 size = 0;
        OSStatus s = get_prop(obj, a, 0, NULL, NULL, &size);
        return s ? s : kAudioHardwareUnsupportedOperationError;
    }
    switch (obj) {
    case kObjDevice:
        if (inSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
        return request_rate(*(const Float64 *)in);
    case kObjStream:
    case kObjStream2:
        if (a->mSelector == kAudioStreamPropertyIsActive) return 0;
        if (inSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
        {
            const AudioStreamBasicDescription *f = in;
            AudioStreamBasicDescription want = format_at(f->mSampleRate);
            if (f->mFormatID != want.mFormatID || f->mChannelsPerFrame != NCH || f->mBitsPerChannel != 32 ||
                !(f->mFormatFlags & kAudioFormatFlagIsFloat))
                return kAudioDeviceUnsupportedFormatError;
            return request_rate(f->mSampleRate);
        }
    case kObjVolume: {
        if (inSize != sizeof(Float32)) return kAudioHardwareBadPropertySizeError;
        Float32 v = *(const Float32 *)in;
        if (a->mSelector == kAudioLevelControlPropertyDecibelValue) v = db_to_scalar(v);
        atomic_store(&g_volume, v < 0 ? 0 : v > 1 ? 1 : v);
        AudioObjectPropertyAddress ch[2] = {
            {kAudioLevelControlPropertyScalarValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
            {kAudioLevelControlPropertyDecibelValue, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain},
        };
        g_host->PropertiesChanged(g_host, kObjVolume, 2, ch);
        save_settings();
        return 0;
    }
    case kObjMute:
        if (inSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
        atomic_store(&g_mute, *(const UInt32 *)in != 0);
        notify(kObjMute, kAudioBooleanControlPropertyValue);
        save_settings();
        return 0;
    }
    return kAudioHardwareBadObjectError;
}

static OSStatus StartIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)d; (void)client;
    if (dev != kObjDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&g_mutex);
    if (g_io_clients++ == 0) {
        g_anchor = mach_absolute_time();
        g_ts_count = 0;
        memset(g_ring, 0, sizeof g_ring);
        atomic_store(&g_head, 0);
        atomic_fetch_add(&g_epoch, 1);
    }
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

static OSStatus StopIO(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client) {
    (void)d; (void)client;
    if (dev != kObjDevice) return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&g_mutex);
    if (g_io_clients > 0) g_io_clients--;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

static OSStatus GetZeroTimeStamp(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, Float64 *outSample, UInt64 *outHost,
                                 UInt64 *outSeed) {
    (void)d; (void)dev; (void)client;
    pthread_mutex_lock(&g_mutex);
    Float64 ticks_per_period = g_ticks_per_frame * ZERO_PERIOD;
    UInt64 now = mach_absolute_time();
    while ((Float64)(now - g_anchor) >= (Float64)(g_ts_count + 1) * ticks_per_period) g_ts_count++;
    *outSample = (Float64)g_ts_count * ZERO_PERIOD;
    *outHost = g_anchor + (UInt64)((Float64)g_ts_count * ticks_per_period);
    *outSeed = 1;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

static OSStatus WillDoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, Boolean *willDo, Boolean *inPlace) {
    (void)d; (void)dev; (void)client;
    *willDo = op == kAudioServerPlugInIOOperationWriteMix;
    *inPlace = true;
    return 0;
}

static OSStatus BeginIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 frames,
                                 const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)dev; (void)client; (void)op; (void)frames; (void)info;
    return 0;
}

static OSStatus DoIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, AudioObjectID stream, UInt32 client, UInt32 op,
                              UInt32 frames, const AudioServerPlugInIOCycleInfo *info, void *main, void *secondary) {
    (void)d; (void)dev; (void)client; (void)secondary;
    if (op == kAudioServerPlugInIOOperationWriteMix && main && info) {
        float gain = atomic_load(&g_mute) ? 0.0f : atomic_load(&g_volume);
        gain *= gain; /* scalar^2, exactly 1.0 at full volume: bit-perfect */
        ring_write((int64_t)llround(info->mOutputTime.mSampleTime), main, frames, stream == kObjStream2 ? 2 : 0, gain);
    }
    return 0;
}

static OSStatus EndIOOperation(AudioServerPlugInDriverRef d, AudioObjectID dev, UInt32 client, UInt32 op, UInt32 frames,
                               const AudioServerPlugInIOCycleInfo *info) {
    (void)d; (void)dev; (void)client; (void)op; (void)frames; (void)info;
    return 0;
}

static AudioServerPlugInDriverInterface g_iface = {
    NULL, QueryInterface, AddRef, Release, Initialize, CreateDevice, DestroyDevice, AddDeviceClient, RemoveDeviceClient,
    PerformDeviceConfigurationChange, AbortDeviceConfigurationChange, HasProperty, IsPropertySettable, GetPropertyDataSize,
    GetPropertyData, SetPropertyData, StartIO, StopIO, GetZeroTimeStamp, WillDoIOOperation, BeginIOOperation, DoIOOperation,
    EndIOOperation,
};
static AudioServerPlugInDriverInterface *g_iface_ptr = &g_iface;
static AudioServerPlugInDriverRef g_driver = &g_iface_ptr;

static HRESULT QueryInterface(void *drv, REFIID iid, LPVOID *out) {
    (void)drv;
    CFUUIDRef want = CFUUIDCreateFromUUIDBytes(NULL, iid);
    HRESULT r = E_NOINTERFACE;
    if (CFEqual(want, IUnknownUUID) || CFEqual(want, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&g_refs, 1);
        *out = g_driver;
        r = S_OK;
    }
    CFRelease(want);
    return r;
}

static ULONG AddRef(void *drv) {
    (void)drv;
    return atomic_fetch_add(&g_refs, 1) + 1;
}

static ULONG Release(void *drv) {
    (void)drv;
    ULONG r = atomic_load(&g_refs);
    if (r > 0) r = atomic_fetch_sub(&g_refs, 1) - 1;
    return r;
}

void *FastTrack24_Create(CFAllocatorRef alloc, CFUUIDRef type);
void *FastTrack24_Create(CFAllocatorRef alloc, CFUUIDRef type) {
    (void)alloc;
    return CFEqual(type, kAudioServerPlugInTypeUUID) ? g_driver : NULL;
}
