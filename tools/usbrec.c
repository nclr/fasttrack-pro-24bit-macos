/* Byte-exact capture test: records raw 24-bit samples from an input interface
 * (4 = analog in, 5 = second pair) and reports which byte order gives a smooth signal.
 *
 *   usbrec <interface> <seconds> [out.raw] [alt]   (alt 2 = 24-bit default, 1 = 16-bit)
 *
 * Needs the card in configuration 2 with the macOS driver detached (ftconfig claim, or
 * the plug-in installed: it only holds the output interfaces).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { FRAMES = 8, NX = 8, MAXPKT = 296 };
static int RATE = 48000; /* FT_RATE env: 0 = do not set */

static IOUSBInterfaceInterface650 **g_if;
static UInt8 g_pipe;
static UInt64 g_next;
static int g_remaining, g_inflight, g_errors;
static uint8_t *g_data;
static size_t g_len, g_cap;
static unsigned g_hist[MAXPKT + 1];

typedef struct {
    uint8_t buf[FRAMES * MAXPKT];
    IOUSBIsocFrame fl[FRAMES];
} X;
static X g_x[NX];

static void submit(X *x);

static void done(void *ref, IOReturn r, void *arg) {
    (void)arg;
    X *x = ref;
    g_inflight--;
    if (r && r != kIOReturnUnderrun && g_errors++ < 5) fprintf(stderr, "read: 0x%x\n", r);
    for (int f = 0; f < FRAMES; f++) {
        UInt16 n = x->fl[f].frActCount;
        if (n <= MAXPKT) g_hist[n]++;
        if (g_len + n > g_cap) {
            g_cap = (g_cap + n) * 2;
            g_data = realloc(g_data, g_cap);
        }
        memcpy(g_data + g_len, x->buf + f * MAXPKT, n);
        g_len += n;
    }
    if (g_remaining > 0) submit(x);
    else if (g_inflight == 0) CFRunLoopStop(CFRunLoopGetCurrent());
}

static void submit(X *x) {
    for (int f = 0; f < FRAMES; f++) {
        x->fl[f].frReqCount = MAXPKT;
        x->fl[f].frActCount = 0;
        x->fl[f].frStatus = 0;
    }
    IOReturn r = (*g_if)->ReadIsochPipeAsync(g_if, g_pipe, x->buf, g_next, FRAMES, x->fl, done, x);
    if (r) {
        fprintf(stderr, "ReadIsochPipeAsync: 0x%x\n", r);
        if (g_inflight == 0) CFRunLoopStop(CFRunLoopGetCurrent());
        return;
    }
    g_next += FRAMES;
    g_remaining -= FRAMES;
    g_inflight++;
}

static io_service_t find_interface(int num) {
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    int v = 0x0763, p = 0x2012;
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), CFNumberCreate(NULL, kCFNumberSInt32Type, &v));
    CFDictionarySetValue(m, CFSTR(kUSBProductID), CFNumberCreate(NULL, kCFNumberSInt32Type, &p));
    io_service_t devsvc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!devsvc) return 0;
    IOCFPlugInInterface **plug;
    SInt32 score;
    if (IOCreatePlugInInterfaceForService(devsvc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score)) return 0;
    IOObjectRelease(devsvc);
    IOUSBDeviceInterface650 **dev;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&dev);
    (*plug)->Release(plug);
    IOUSBFindInterfaceRequest req = {kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                     kIOUSBFindInterfaceDontCare};
    io_iterator_t it;
    (*dev)->CreateInterfaceIterator(dev, &req, &it);
    (*dev)->Release(dev);
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
    return found;
}

/* Mean absolute step between consecutive samples of channel 0, relative to the signal's
 * spread. A wrong byte order turns small changes into full-scale jumps. */
static double roughness(const uint8_t *d, size_t frames, const int order[3]) {
    double sum = 0, mean = 0, var = 0;
    int32_t prev = 0;
    for (size_t i = 0; i < frames; i++) {
        const uint8_t *p = d + i * 6;
        uint32_t u = (uint32_t)p[order[0]] << 16 | (uint32_t)p[order[1]] << 8 | p[order[2]];
        int32_t s = (int32_t)(u << 8) >> 8;
        if (i) sum += fabs((double)s - prev);
        prev = s;
        mean += s;
    }
    mean /= frames;
    for (size_t i = 0; i < frames; i++) {
        const uint8_t *p = d + i * 6;
        uint32_t u = (uint32_t)p[order[0]] << 16 | (uint32_t)p[order[1]] << 8 | p[order[2]];
        int32_t s = (int32_t)(u << 8) >> 8;
        var += (s - mean) * (s - mean);
    }
    double sd = sqrt(var / frames);
    return sd > 0 ? (sum / (frames - 1)) / sd : 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        puts("usage: usbrec <interface 4|5> <seconds> [out.raw]");
        return 2;
    }
    int ifnum = atoi(argv[1]);
    int alt = argc > 4 ? atoi(argv[4]) : 2;
    if (getenv("FT_RATE")) RATE = atoi(getenv("FT_RATE"));
    g_remaining = atoi(argv[2]) * 1000;
    io_service_t svc = find_interface(ifnum);
    if (!svc) {
        puts("interface not found (card not in configuration 2?)");
        return 1;
    }
    IOCFPlugInInterface **plug;
    SInt32 score;
    IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650), (LPVOID *)&g_if);
    (*plug)->Release(plug);
    IOReturn r = (*g_if)->USBInterfaceOpenSeize(g_if);
    printf("open interface %d: 0x%x\n", ifnum, r);
    if (r) return 1;
    printf("alt %d: 0x%x\n", alt, (*g_if)->SetAlternateInterface(g_if, (UInt8)alt));
    UInt8 nep = 0, ep = 0;
    (*g_if)->GetNumEndpoints(g_if, &nep);
    for (UInt8 i = 1; i <= nep; i++) {
        UInt8 dir, num, type, interval;
        UInt16 mps;
        (*g_if)->GetPipeProperties(g_if, i, &dir, &num, &type, &mps, &interval);
        printf("pipe %u: ep %u dir %u type %u maxpkt %u\n", i, num, dir, type, mps);
        if (dir == kUSBIn && type == kUSBIsoc) { g_pipe = i; ep = num; }
    }
    UInt8 rate[3] = {RATE & 0xff, (RATE >> 8) & 0xff, RATE >> 16};
    IOUSBDevRequest req = {USBmakebmRequestType(kUSBOut, kUSBClass, kUSBEndpoint), 0x01, 0x0100, (UInt16)(0x80 | ep), 3, rate, 0};
    if (RATE) printf("set rate %d: 0x%x\n", RATE, (*g_if)->ControlRequest(g_if, 0, &req));
    else puts("rate not set");
    {
        UInt8 cur[3] = {0};
        IOUSBDevRequest get = {USBmakebmRequestType(kUSBIn, kUSBClass, kUSBEndpoint), 0x81, 0x0100, (UInt16)(0x80 | ep), 3, cur, 0};
        IOReturn gr = (*g_if)->ControlRequest(g_if, 0, &get);
        printf("GET_CUR rate: 0x%x -> %d\n", gr, cur[0] | cur[1] << 8 | cur[2] << 16);
    }
    CFRunLoopSourceRef src;
    (*g_if)->CreateInterfaceAsyncEventSource(g_if, &src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    AbsoluteTime at;
    (*g_if)->GetBusFrameNumber(g_if, &g_next, &at);
    g_next += 10;
    for (int i = 0; i < NX; i++) submit(&g_x[i]);
    if (g_inflight) CFRunLoopRun();
    (*g_if)->SetAlternateInterface(g_if, 0);
    (*g_if)->USBInterfaceClose(g_if);

    printf("\nbytes per 1 ms packet:");
    for (int n = 0; n <= MAXPKT; n++)
        if (g_hist[n]) printf("  %d x%u", n, g_hist[n]);
    size_t frames = g_len / 6;
    printf("\n%zu stereo frames = %.3f s at %d Hz\n", frames, frames / (double)RATE, RATE);
    if (argc > 3 && strcmp(argv[3], "-")) {
        FILE *f = fopen(argv[3], "wb");
        fwrite(g_data, 1, g_len, f);
        fclose(f);
    }
    if (frames < 1000) return 1;
    /* orders[k] = byte positions of the high, middle and low byte */
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    printf("\nroughness per byte layout, channel 1 (lower = smoother):\n");
    for (int k = 0; k < 6; k++) {
        const char *pos[3];
        for (int j = 0; j < 3; j++) pos[orders[k][j]] = j == 0 ? "high" : j == 1 ? "mid" : "low";
        printf("  [%s, %s, %s]  %.4f\n", pos[0], pos[1], pos[2], roughness(g_data, frames, orders[k]));
    }
    return 0;
}
