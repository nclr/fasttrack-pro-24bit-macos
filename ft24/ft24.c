/* ft24: 24-bit playback on the M-Audio Fast Track Pro, bypassing the macOS driver.
 *
 *   apps -> BlackHole 2ch -> ft24 -> USB isochronous -> Fast Track Pro outputs 1-2 and 3-4
 *
 * The card's 24-bit alt settings (configuration 2, interfaces 2 and 3, alt 2) take
 * 3-byte samples laid out as [middle, high, low] -- neither little- nor big-endian,
 * verified by ear with tools/usbtone. The macOS driver can only send little-endian,
 * which the card plays as noise, so ft24 drives the endpoints itself:
 *
 *   1. Re-select USB configuration 2 with interface matching off, so usbaudiod
 *      does not attach.
 *   2. Seize output interfaces 2 and 3, select alt 2 (24-bit, 48 kHz, adaptive),
 *      set the endpoint sample rate.
 *   3. Read BlackHole 2ch input (float) into a ring, convert to 24-bit, stream.
 *      The endpoints are adaptive, so instead of resampling we send 47-49 samples
 *      per 1 ms USB frame to follow BlackHole's clock. Samples are passed unaltered.
 *   4. On exit, return the card to configuration 1 with matching on (the normal
 *      16-bit macOS device) and restore the default output.
 */
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>

#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int mic_permission(void); /* mic.m */

enum {
    RATE = 48000,
    ALT_24BIT = 2,
    FRAMES_PER_SLOT = 4,  /* USB frames (ms) per isochronous transfer */
    NSLOT = 8,            /* transfers in flight: 32 ms queued */
    MAX_PER_MS = 49,
    RING = 16384,         /* frames, power of two */
    TARGET = 1024,        /* ring fill to hold, frames (~21 ms) */
};
static const int OUT_IFACES[] = {2, 3}; /* outputs 1-2 (button A), 3-4 (button B / S/PDIF) */
enum { NIF = 2 };

/* ---------- ring: BlackHole IOProc thread -> USB completion (main run loop) ---------- */

static float g_ringL[RING], g_ringR[RING];
static _Atomic uint64_t g_w, g_r;
static _Atomic unsigned g_peak_milli;

static void ring_push(const float *interleaved, UInt32 ch, UInt32 n) {
    uint64_t w = atomic_load_explicit(&g_w, memory_order_relaxed);
    uint64_t r = atomic_load_explicit(&g_r, memory_order_acquire);
    if (n > RING - 1 - (w - r)) n = (UInt32)(RING - 1 - (w - r)); /* overflow: drop newest */
    float m = 0;
    for (UInt32 i = 0; i < n; i++) {
        float l = interleaved[i * ch], rr = ch > 1 ? interleaved[i * ch + 1] : l;
        g_ringL[(w + i) & (RING - 1)] = l;
        g_ringR[(w + i) & (RING - 1)] = rr;
        if (fabsf(l) > m) m = fabsf(l);
        if (fabsf(rr) > m) m = fabsf(rr);
    }
    atomic_store_explicit(&g_w, w + n, memory_order_release);
    unsigned pm = (unsigned)(m * 1000), cur = atomic_load(&g_peak_milli);
    while (pm > cur && !atomic_compare_exchange_weak(&g_peak_milli, &cur, pm)) {}
}

static OSStatus bh_proc(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in,
                        const AudioTimeStamp *it, AudioBufferList *out, const AudioTimeStamp *ot, void *c) {
    (void)d; (void)now; (void)it; (void)out; (void)ot; (void)c;
    if (in && in->mNumberBuffers > 0 && in->mBuffers[0].mData) {
        UInt32 ch = in->mBuffers[0].mNumberChannels;
        ring_push(in->mBuffers[0].mData, ch, in->mBuffers[0].mDataByteSize / (4 * ch));
    }
    return noErr;
}

/* ---------- 24-bit packing ---------- */

static inline void put24(uint8_t *p, float x) {
    int32_t s = (int32_t)lrintf(x * 8388608.0f);
    if (s > 8388607) s = 8388607;
    if (s < -8388608) s = -8388608;
    p[0] = (uint8_t)(s >> 8);  /* middle */
    p[1] = (uint8_t)(s >> 16); /* high */
    p[2] = (uint8_t)s;         /* low */
}

/* ---------- USB streaming ---------- */

typedef struct {
    uint8_t buf[NIF][FRAMES_PER_SLOT * MAX_PER_MS * 6];
    IOUSBIsocFrame fl[NIF][FRAMES_PER_SLOT];
    int pending;
} Slot;

static Slot g_slot[NSLOT];
static IOUSBInterfaceInterface650 **g_if[NIF];
static UInt8 g_pipe[NIF];
static int g_nif;
static UInt64 g_next_frame;
static double g_acc, g_fill_lp = TARGET;
static int g_primed;
static int g_inflight;
static volatile sig_atomic_t g_stop;
static unsigned g_underruns, g_usb_errors, g_resyncs;
static double g_corr;

static void fill_slot(Slot *s) {
    uint64_t w = atomic_load_explicit(&g_w, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&g_r, memory_order_relaxed);
    uint64_t fill = w - r;

    if (!g_primed && fill >= TARGET) {
        r = w - TARGET;
        fill = TARGET;
        g_primed = 1;
    }
    /* Follow BlackHole's clock: adaptive endpoint, so vary samples per ms slightly. */
    g_fill_lp += 0.02 * ((double)fill - g_fill_lp);
    g_corr = (g_fill_lp - TARGET) / TARGET * 0.002;
    if (g_corr > 0.001) g_corr = 0.001;
    if (g_corr < -0.001) g_corr = -0.001;

    for (int f = 0; f < FRAMES_PER_SLOT; f++) {
        g_acc += RATE / 1000.0 * (1.0 + g_corr);
        int n = (int)g_acc;
        g_acc -= n;
        if (n > MAX_PER_MS) n = MAX_PER_MS;
        uint8_t *p[NIF];
        for (int k = 0; k < g_nif; k++) p[k] = s->buf[k] + f * MAX_PER_MS * 6;
        for (int i = 0; i < n; i++) {
            float L = 0, R = 0;
            if (g_primed && r < w) {
                L = g_ringL[r & (RING - 1)];
                R = g_ringR[r & (RING - 1)];
                r++;
            } else if (g_primed) {
                g_underruns++;
                g_primed = 0; /* re-prime to TARGET */
            }
            uint8_t frame[6];
            put24(frame, L);
            put24(frame + 3, R);
            for (int k = 0; k < g_nif; k++) { memcpy(p[k], frame, 6); p[k] += 6; }
        }
        for (int k = 0; k < g_nif; k++) {
            s->fl[k][f].frReqCount = (UInt16)(n * 6);
            s->fl[k][f].frActCount = 0;
            s->fl[k][f].frStatus = 0;
        }
    }
    /* Frames within a transfer must be contiguous in the buffer. */
    for (int k = 0; k < g_nif; k++) {
        uint8_t *dst = s->buf[k];
        for (int f = 0; f < FRAMES_PER_SLOT; f++) {
            memmove(dst, s->buf[k] + f * MAX_PER_MS * 6, s->fl[k][f].frReqCount);
            dst += s->fl[k][f].frReqCount;
        }
    }
    atomic_store_explicit(&g_r, r, memory_order_release);
}

static void submit(Slot *s);

static void resync(void) {
    AbsoluteTime at;
    UInt64 bus = 0;
    (*g_if[0])->GetBusFrameNumber(g_if[0], &bus, &at);
    if (g_next_frame < bus + 2) {
        g_next_frame = bus + 8;
        g_resyncs++;
    }
}

static void on_done(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    Slot *s = refcon;
    g_inflight--;
    if (result != kIOReturnSuccess && result != kIOReturnUnderrun) {
        g_usb_errors++;
        if (result == kIOReturnNotResponding || result == kIOReturnNoDevice || result == kIOReturnAborted) g_stop = 1;
        else resync();
    }
    if (--s->pending > 0) return;
    if (!g_stop) submit(s);
    if (g_inflight == 0) CFRunLoopStop(CFRunLoopGetMain());
}

static void submit(Slot *s) {
    fill_slot(s);
    for (int attempt = 0; attempt < 2; attempt++) {
        int ok = 1;
        s->pending = 0;
        for (int k = 0; k < g_nif; k++) {
            IOReturn r = (*g_if[k])->WriteIsochPipeAsync(g_if[k], g_pipe[k], s->buf[k], g_next_frame,
                                                          FRAMES_PER_SLOT, s->fl[k], on_done, s);
            if (r == kIOReturnSuccess) { s->pending++; g_inflight++; }
            else { ok = 0; g_usb_errors++; if (r == kIOReturnNoDevice || r == kIOReturnNotOpen) g_stop = 1; }
        }
        if (ok || s->pending > 0 || g_stop) break;
        resync(); /* scheduled in the past: jump ahead and retry once */
    }
    g_next_frame += FRAMES_PER_SLOT;
}

/* ---------- USB device / interface handling ---------- */

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
    if (kr || !plug) return NULL;
    IOUSBDeviceInterface650 **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&dev);
    (*plug)->Release(plug);
    return dev;
}

/* matching=0: configuration 2, nobody attaches (we drive it).
 * matching=1: configuration 1, the macOS driver takes it back as a normal 16-bit device. */
static int set_usb_config(int matching) {
    IOUSBDeviceInterface650 **dev = open_device_iface();
    if (!dev) { fprintf(stderr, "Fast Track Pro (0763:2012) not found on USB.\n"); return 0; }
    IOReturn r = (*dev)->USBDeviceOpenSeize(dev);
    if (r == kIOReturnSuccess) {
        if (matching) {
            r = (*dev)->SetConfigurationV2(dev, 1, true, false);
        } else {
            (*dev)->SetConfigurationV2(dev, 1, false, false);
            r = (*dev)->SetConfigurationV2(dev, 2, false, false);
        }
        (*dev)->USBDeviceClose(dev);
    }
    (*dev)->Release(dev);
    if (r) fprintf(stderr, "USB configuration change failed: 0x%x\n", r);
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
        if (n) { CFNumberGetValue(n, kCFNumberSInt32Type, &in); CFRelease(n); }
        if (in == num && !found) found = s;
        else IOObjectRelease(s);
    }
    IOObjectRelease(it);
    if (!found) return NULL;
    IOCFPlugInInterface **plug = NULL;
    SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(found, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(found);
    if (kr || !plug) return NULL;
    IOUSBInterfaceInterface650 **intf = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650), (LPVOID *)&intf);
    (*plug)->Release(plug);
    if (!intf) return NULL;
    r = (*intf)->USBInterfaceOpenSeize(intf);
    if (r) {
        fprintf(stderr, "Could not open interface %d: 0x%x\n", num, r);
        (*intf)->Release(intf);
        return NULL;
    }
    return intf;
}

static int start_interface(IOUSBInterfaceInterface650 **intf, UInt8 *pipe) {
    IOReturn r = (*intf)->SetAlternateInterface(intf, ALT_24BIT);
    if (r) { fprintf(stderr, "alt setting %d: 0x%x\n", ALT_24BIT, r); return 0; }
    UInt8 nep = 0, epaddr = 0;
    (*intf)->GetNumEndpoints(intf, &nep);
    *pipe = 0;
    for (UInt8 i = 1; i <= nep; i++) {
        UInt8 dir, num, type, interval;
        UInt16 mps;
        (*intf)->GetPipeProperties(intf, i, &dir, &num, &type, &mps, &interval);
        if (dir == kUSBOut && type == kUSBIsoc) { *pipe = i; epaddr = num; }
    }
    if (!*pipe) { fprintf(stderr, "no isochronous OUT endpoint\n"); return 0; }
    UInt8 rate[3] = {RATE & 0xff, (RATE >> 8) & 0xff, (RATE >> 16) & 0xff};
    IOUSBDevRequest req = {.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBEndpoint),
                           .bRequest = 0x01, /* SET_CUR */
                           .wValue = 0x0100, /* SAMPLING_FREQ_CONTROL */
                           .wIndex = epaddr, .wLength = 3, .pData = rate};
    r = (*intf)->ControlRequest(intf, 0, &req);
    if (r) fprintf(stderr, "set sample rate on endpoint %u: 0x%x (continuing)\n", epaddr, r);
    return 1;
}

static void close_interfaces(void) {
    for (int k = 0; k < NIF; k++) {
        if (!g_if[k]) continue;
        (*g_if[k])->SetAlternateInterface(g_if[k], 0);
        (*g_if[k])->USBInterfaceClose(g_if[k]);
        (*g_if[k])->Release(g_if[k]);
        g_if[k] = NULL;
    }
}

/* ---------- Core Audio helpers ---------- */

static AudioObjectPropertyAddress addr(AudioObjectPropertySelector s, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = {s, sc, kAudioObjectPropertyElementMain};
    return a;
}

static int device_name_is(AudioObjectID id, const char *want) {
    AudioObjectPropertyAddress a = addr(kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal);
    CFStringRef s = NULL;
    UInt32 sz = sizeof s;
    char n[256] = "";
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &s) == noErr && s) {
        CFStringGetCString(s, n, sizeof n, kCFStringEncodingUTF8);
        CFRelease(s);
    }
    return strstr(n, want) != NULL;
}

static int has_streams(AudioObjectID id, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyStreams, scope);
    UInt32 sz = 0;
    return AudioObjectGetPropertyDataSize(id, &a, 0, NULL, &sz) == noErr && sz > 0;
}

static AudioObjectID find_device(const char *name, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal);
    AudioObjectID d[128];
    UInt32 sz = sizeof d;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, d)) return 0;
    for (UInt32 i = 0; i < sz / sizeof(AudioObjectID); i++)
        if (device_name_is(d[i], name) && has_streams(d[i], scope)) return d[i];
    return 0;
}

static AudioObjectID get_default_output(void) {
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
    AudioObjectID id = 0;
    UInt32 sz = sizeof id;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &id);
    return id;
}

static void set_default_output(AudioObjectID id) {
    AudioObjectPropertyAddress a = addr(kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal);
    AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, sizeof id, &id);
}

static Float64 get_rate(AudioObjectID id) {
    AudioObjectPropertyAddress a = addr(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    Float64 r = 0;
    UInt32 sz = sizeof r;
    AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &r);
    return r;
}

/* ---------- main ---------- */

static void on_sig(int s) { (void)s; g_stop = 1; }

static void status_timer(CFRunLoopTimerRef t, void *info) {
    (void)t; (void)info;
    static unsigned last_under, last_err;
    uint64_t fill = atomic_load(&g_w) - atomic_load(&g_r);
    unsigned pk = atomic_exchange(&g_peak_milli, 0);
    printf("level %.3f  buffer %3.0f ms  clock %+5.0f ppm  underruns %u  usb errors %u\n",
           pk / 1000.0, fill * 1000.0 / RATE, g_corr * 1e6, g_underruns - last_under, g_usb_errors - last_err);
    fflush(stdout);
    last_under = g_underruns;
    last_err = g_usb_errors;
    if (g_stop) {
        /* stop resubmitting; last completion stops the run loop */
        if (g_inflight == 0) CFRunLoopStop(CFRunLoopGetMain());
    }
}

int main(int argc, char **argv) {
    int a_only = argc > 1 && !strcmp(argv[1], "--a-only");
    if (argc > 1 && !strcmp(argv[1], "--release")) return set_usb_config(1) ? 0 : 1;

    setvbuf(stdout, NULL, _IOLBF, 0);

    AudioObjectID bh = find_device("BlackHole 2ch", kAudioObjectPropertyScopeInput);
    if (!bh) { fprintf(stderr, "BlackHole 2ch not found. Install BlackHole 2ch.\n"); return 1; }

    int mic = mic_permission();
    if (mic != 1) {
        fprintf(stderr,
                "No microphone permission, so macOS would feed silence from BlackHole.\n"
                "Open System Settings > Privacy & Security > Microphone and enable the app\n"
                "you run ft24 from (Terminal, iTerm, ...), then quit and reopen that app.\n");
        return 1;
    }

    AudioObjectID prev_default = get_default_output();
    int prev_was_fasttrack = device_name_is(prev_default, "FastTrack") || device_name_is(prev_default, "BlackHole 2ch");

    /* BlackHole at 48 kHz, as the default output */
    Float64 rate = RATE;
    AudioObjectPropertyAddress ra = addr(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    AudioObjectSetPropertyData(bh, &ra, 0, NULL, sizeof rate, &rate);
    UInt32 bufsz = 256;
    AudioObjectPropertyAddress ba = addr(kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    AudioObjectSetPropertyData(bh, &ba, 0, NULL, sizeof bufsz, &bufsz);

    printf("Taking the Fast Track Pro from macOS (USB configuration 2)...\n");
    if (!set_usb_config(0)) return 1;
    usleep(500000);

    g_nif = a_only ? 1 : NIF;
    for (int k = 0; k < g_nif; k++) {
        g_if[k] = open_interface(OUT_IFACES[k]);
        if (!g_if[k] || !start_interface(g_if[k], &g_pipe[k])) {
            close_interfaces();
            set_usb_config(1);
            return 1;
        }
    }

    set_default_output(bh);

    AudioDeviceIOProcID bhproc = NULL;
    OSStatus st = AudioDeviceCreateIOProcID(bh, bh_proc, NULL, &bhproc);
    if (!st) st = AudioDeviceStart(bh, bhproc);
    if (st) {
        fprintf(stderr, "Could not start BlackHole input: %d\n", (int)st);
        g_stop = 1;
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_sig;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);

    for (int k = 0; k < g_nif; k++) {
        CFRunLoopSourceRef src = NULL;
        (*g_if[k])->CreateInterfaceAsyncEventSource(g_if[k], &src);
        CFRunLoopAddSource(CFRunLoopGetMain(), src, kCFRunLoopDefaultMode);
    }

    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 5, 5, 0, 0, status_timer, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopDefaultMode);
    /* short timer so Ctrl-C is noticed promptly even if USB stalls */
    CFRunLoopTimerRef stopper = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 0.2, 0.2, 0, 0,
                                                                 ^(CFRunLoopTimerRef t) {
                                                                     (void)t;
                                                                     if (g_stop && g_inflight == 0) CFRunLoopStop(CFRunLoopGetMain());
                                                                 });
    CFRunLoopAddTimer(CFRunLoopGetMain(), stopper, kCFRunLoopDefaultMode);

    if (!g_stop) {
        AbsoluteTime at;
        (*g_if[0])->GetBusFrameNumber(g_if[0], &g_next_frame, &at);
        g_next_frame += 10;
        for (int i = 0; i < NSLOT; i++) submit(&g_slot[i]);
        printf("Playing 24-bit / %d Hz to Fast Track outputs 1-2%s. Output device: BlackHole 2ch.\n",
               RATE, g_nif > 1 ? " and 3-4" : "");
        printf("Ctrl-C gives the card back to macOS.\n");
        if (get_rate(bh) != RATE) printf("warning: BlackHole 2ch is at %.0f Hz, expected %d\n", get_rate(bh), RATE);
        CFRunLoopRun();
    }

    printf("\nStopping.\n");
    if (bhproc) {
        AudioDeviceStop(bh, bhproc);
        AudioDeviceDestroyIOProcID(bh, bhproc);
    }
    for (int k = 0; k < g_nif; k++)
        if (g_if[k]) (*g_if[k])->AbortPipe(g_if[k], g_pipe[k]);
    close_interfaces();
    set_usb_config(1);

    /* Put the default output back once the card reappears as a normal device. */
    AudioObjectID back = 0;
    if (prev_was_fasttrack) {
        for (int i = 0; i < 40 && !(back = find_device("FastTrack", kAudioObjectPropertyScopeOutput)); i++) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.25, false);
        }
    } else {
        back = prev_default;
    }
    if (back) set_default_output(back);
    printf("Card returned to macOS (16-bit)%s.\n", back ? ", default output restored" : "");
    return 0;
}
