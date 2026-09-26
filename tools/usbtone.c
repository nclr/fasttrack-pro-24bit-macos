/* Byte-exact FastTrack Pro output test, bypassing usbaudiod.
 * Seizes output interface 2 (outputs 1-2 / headphone A), selects an alt setting,
 * sets the sample rate on the endpoint, and streams a 440 Hz tone with isochronous
 * writes whose bytes we control.
 *
 *   usbtone <alt> <be|le|b0|b1|b2> <seconds>
 *     HML/MHL/...: byte 0,1,2 carry the High/Mid/Low byte of each sample
 *     b0..b2: 8-bit tone in that byte of each 24-bit sample, other bytes zero
 *     alt 1 = 16-bit, alt 2 = 24-bit (adaptive), both 48 kHz
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RATE = 48000, FRAMES_PER_XFER = 8, NXFER = 8 };

static IOUSBInterfaceInterface650 **g_intf;
static UInt8 g_pipe;
static int g_bytes;       /* bytes per sample: 2 or 3 */
static int g_be;
static const char *g_perm; /* e.g. "MHL": significance of byte 0,1,2 */
static int g_bytepos = -1; /* 0..2: tone as 8-bit value in this byte only */
static double g_phase;
static double g_acc;      /* fractional sample accumulator for 44.1k-style rates */
static UInt64 g_next_frame;
static int g_remaining_ms;
static int g_inflight;
static int g_errors;

typedef struct {
    uint8_t buf[FRAMES_PER_XFER * 49 * 2 * 3];
    IOUSBIsocFrame fl[FRAMES_PER_XFER];
} Xfer;
static Xfer g_x[NXFER];

static void put(uint8_t *p, int32_t s) {
    if (g_bytes == 2) {
        int16_t v = (int16_t)(s >> 8);
        if (g_be) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
        else      { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
    } else if (g_perm) {
        for (int k = 0; k < 3; k++)
            p[k] = (uint8_t)(g_perm[k] == 'H' ? s >> 16 : g_perm[k] == 'M' ? s >> 8 : s);
    } else if (g_bytepos >= 0) {
        p[0] = p[1] = p[2] = 0;
        p[g_bytepos] = (uint8_t)(int8_t)(s >> 16);
    } else {
        if (g_be) { p[0] = (uint8_t)(s >> 16); p[1] = (uint8_t)(s >> 8); p[2] = (uint8_t)s; }
        else      { p[0] = (uint8_t)s; p[1] = (uint8_t)(s >> 8); p[2] = (uint8_t)(s >> 16); }
    }
}

static void fill(Xfer *x) {
    uint8_t *p = x->buf;
    for (int f = 0; f < FRAMES_PER_XFER; f++) {
        g_acc += RATE / 1000.0;
        int n = (int)g_acc;
        g_acc -= n;
        for (int i = 0; i < n; i++) {
            int32_t s = (int32_t)lrint(0.1 * sin(g_phase) * 8388607.0);
            g_phase += 2 * M_PI * 440.0 / RATE;
            if (g_phase > 2 * M_PI) g_phase -= 2 * M_PI;
            put(p, s); p += g_bytes;
            put(p, s); p += g_bytes;
        }
        x->fl[f].frReqCount = (UInt16)(n * 2 * g_bytes);
        x->fl[f].frActCount = 0;
        x->fl[f].frStatus = 0;
    }
}

static void submit(Xfer *x);

static void done(void *refcon, IOReturn result, void *arg0) {
    (void)arg0;
    g_inflight--;
    if (result != kIOReturnSuccess && g_errors++ < 5) fprintf(stderr, "iso write: 0x%x\n", result);
    if (g_remaining_ms > 0) submit((Xfer *)refcon);
    else if (g_inflight == 0) CFRunLoopStop(CFRunLoopGetCurrent());
}

static void submit(Xfer *x) {
    fill(x);
    IOReturn r = (*g_intf)->WriteIsochPipeAsync(g_intf, g_pipe, x->buf, g_next_frame, FRAMES_PER_XFER, x->fl, done, x);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "WriteIsochPipeAsync: 0x%x\n", r);
        if (g_inflight == 0) CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    g_next_frame += FRAMES_PER_XFER;
    g_remaining_ms -= FRAMES_PER_XFER;
    g_inflight++;
}

/* Walk the device's interfaces directly: with matching off they are not registered,
 * so IOServiceGetMatchingService cannot see them. */
static io_service_t find_interface(int num) {
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    int v = 0x0763, p = 0x2012;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberSInt32Type, &v);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberSInt32Type, &p);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), nv);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), np);
    io_service_t devsvc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!devsvc) return 0;
    IOCFPlugInInterface **plug; SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(devsvc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(devsvc);
    if (kr) return 0;
    IOUSBDeviceInterface650 **dev;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&dev);
    (*plug)->Release(plug);
    IOUSBFindInterfaceRequest req = {kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare};
    io_iterator_t it = 0;
    IOReturn r = (*dev)->CreateInterfaceIterator(dev, &req, &it);
    (*dev)->Release(dev);
    if (r) { printf("CreateInterfaceIterator: 0x%x\n", r); return 0; }
    io_service_t s, found = 0;
    while ((s = IOIteratorNext(it))) {
        CFNumberRef n = IORegistryEntryCreateCFProperty(s, CFSTR(kUSBInterfaceNumber), NULL, 0);
        int in = -1;
        if (n) { CFNumberGetValue(n, kCFNumberSInt32Type, &in); CFRelease(n); }
        if (in == num && !found) found = s; else IOObjectRelease(s);
    }
    IOObjectRelease(it);
    return found;
}

int main(int argc, char **argv) {
    if (argc < 4) { puts("usage: usbtone <alt 1|2> <be|le> <seconds>"); return 2; }
    int alt = atoi(argv[1]);
    g_be = !strcmp(argv[2], "be");
    if (strlen(argv[2]) == 3 && strspn(argv[2], "HML") == 3) g_perm = argv[2];
    if (argv[2][0] == 'b' && argv[2][1] >= '0' && argv[2][1] <= '2') g_bytepos = argv[2][1] - '0';
    g_bytes = alt == 1 ? 2 : 3;
    g_remaining_ms = atoi(argv[3]) * 1000;

    io_service_t svc = find_interface(2);
    if (!svc) { puts("Interface 2 not found. Run: ft-config2 claim"); return 1; }
    IOCFPlugInInterface **plug; SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (kr) { printf("plugin: 0x%x\n", kr); return 1; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650), (LPVOID *)&g_intf);
    (*plug)->Release(plug);

    IOReturn r = (*g_intf)->USBInterfaceOpenSeize(g_intf);
    printf("seize interface 2: 0x%x\n", r);
    if (r) return 1;

    r = (*g_intf)->SetAlternateInterface(g_intf, (UInt8)alt);
    printf("alt %d: 0x%x\n", alt, r);

    UInt8 nep = 0;
    (*g_intf)->GetNumEndpoints(g_intf, &nep);
    for (UInt8 i = 1; i <= nep; i++) {
        UInt8 dir, num, type, interval; UInt16 mps;
        (*g_intf)->GetPipeProperties(g_intf, i, &dir, &num, &type, &mps, &interval);
        printf("pipe %u: ep %u dir %u type %u maxpkt %u\n", i, num, dir, type, mps);
        if (dir == kUSBOut && type == kUSBIsoc) g_pipe = i;
    }
    if (!g_pipe) { puts("no iso OUT pipe"); goto out; }

    /* UAC1 SET_CUR SAMPLING_FREQ on endpoint 0x03 */
    UInt8 rate[3] = {RATE & 0xff, (RATE >> 8) & 0xff, (RATE >> 16) & 0xff};
    IOUSBDevRequest req = {
        .bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBEndpoint),
        .bRequest = 0x01, .wValue = 0x0100, .wIndex = 0x03, .wLength = 3, .pData = rate};
    r = (*g_intf)->ControlRequest(g_intf, 0, &req);
    printf("set rate %d: 0x%x\n", RATE, r);

    CFRunLoopSourceRef src;
    (*g_intf)->CreateInterfaceAsyncEventSource(g_intf, &src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);

    AbsoluteTime at;
    (*g_intf)->GetBusFrameNumber(g_intf, &g_next_frame, &at);
    g_next_frame += 10;
    printf("playing %s %d-bit for %s s\n", argv[2], g_bytes * 8, argv[3]);
    fflush(stdout);
    for (int i = 0; i < NXFER; i++) submit(&g_x[i]);
    if (g_inflight) CFRunLoopRun();

out:
    (*g_intf)->SetAlternateInterface(g_intf, 0);
    (*g_intf)->USBInterfaceClose(g_intf);
    (*g_intf)->Release(g_intf);
    puts("released");
    return 0;
}
