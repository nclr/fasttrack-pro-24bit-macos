/* Loads FastTrack24.driver the way coreaudiod does and exercises it without installing:
 * waits for the device, prints its properties, runs simulated IO (silence), switches to
 * 44.1 kHz and back. Usage: testhost [path/to/FastTrack24.driver] */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static AudioServerPlugInDriverRef drv;
static int g_changes;

static OSStatus PropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress *a) {
    (void)h;
    for (UInt32 i = 0; i < n; i++) printf("  [host] object %u changed '%.4s'\n", obj, (char *)&(UInt32){CFSwapInt32HostToBig(a[i].mSelector)});
    g_changes++;
    return 0;
}
static OSStatus CopyFromStorage(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef *out) {
    (void)h; (void)k;
    *out = NULL;
    return kAudioHardwareUnknownPropertyError;
}
static OSStatus WriteToStorage(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef v) {
    (void)h; (void)k; (void)v;
    return 0;
}
static OSStatus DeleteFromStorage(AudioServerPlugInHostRef h, CFStringRef k) {
    (void)h; (void)k;
    return 0;
}
static OSStatus RequestChange(AudioServerPlugInHostRef h, AudioObjectID dev, UInt64 action, void *info) {
    (void)h;
    printf("  [host] configuration change requested: %llu\n", action);
    return (*drv)->PerformDeviceConfigurationChange(drv, dev, action, info);
}
static AudioServerPlugInHostInterface host = {PropertiesChanged, CopyFromStorage, WriteToStorage, DeleteFromStorage, RequestChange};

static OSStatus get(AudioObjectID o, AudioObjectPropertySelector sel, AudioObjectPropertyScope sc, void *out, UInt32 size, UInt32 *got) {
    AudioObjectPropertyAddress a = {sel, sc, kAudioObjectPropertyElementMain};
    return (*drv)->GetPropertyData(drv, o, 0, &a, 0, NULL, size, got, out);
}

static void str(AudioObjectID o, AudioObjectPropertySelector sel, const char *label) {
    CFStringRef s = NULL;
    UInt32 got;
    char b[128] = "?";
    if (!get(o, sel, kAudioObjectPropertyScopeGlobal, &s, sizeof s, &got) && s) CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8);
    printf("  %s: %s\n", label, b);
}

/* Simulates Core Audio's IO cycles: WriteMix on both output streams with advancing sample
 * time. Silence by default; FT_TONE=1 plays 440 Hz on outputs 1-2 and 660 Hz on 3-4. */
static Float64 g_sample_time = 1024;

static void run_io(double seconds, Float64 rate) {
    float a[512 * 2], b[512 * 2];
    int tone = getenv("FT_TONE") != NULL;
    static double pa, pb;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t start = mach_absolute_time();
    double period = 512.0 / rate;
    for (int cycle = 0; cycle * period < seconds; cycle++) {
        for (int i = 0; i < 512; i++) {
            float x = tone ? 0.1f * (float)sin(pa) : 0, y = tone ? 0.1f * (float)sin(pb) : 0;
            pa += 2 * M_PI * 440 / rate;
            pb += 2 * M_PI * 660 / rate;
            a[2 * i] = a[2 * i + 1] = x;
            b[2 * i] = b[2 * i + 1] = y;
        }
        AudioServerPlugInIOCycleInfo info = {0};
        info.mOutputTime.mSampleTime = g_sample_time;
        (*drv)->DoIOOperation(drv, 2, 3, 1, kAudioServerPlugInIOOperationWriteMix, 512, &info, a, NULL);
        (*drv)->DoIOOperation(drv, 2, 6, 1, kAudioServerPlugInIOOperationWriteMix, 512, &info, b, NULL);
        g_sample_time += 512;
        uint64_t due = start + (uint64_t)((cycle + 1) * period * 1e9 * tb.denom / tb.numer);
        while (mach_absolute_time() < due) usleep(500);
    }
}

int main(int argc, char **argv) {
    char path[PATH_MAX];
    if (!realpath(argc > 1 ? argv[1] : "FastTrack24.driver", path)) { perror("bundle path"); return 1; }
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), true);
    CFBundleRef b = CFBundleCreate(NULL, url);
    CFErrorRef err = NULL;
    if (!b || !CFBundleLoadExecutableAndReturnError(b, &err)) {
        printf("could not load bundle %s", path);
        if (err) CFShow(err);
        puts("");
        return 1;
    }
    void *(*create)(CFAllocatorRef, CFUUIDRef) = CFBundleGetFunctionPointerForName(b, CFSTR("FastTrack24_Create"));
    drv = create(NULL, kAudioServerPlugInTypeUUID);
    if (!drv) { puts("factory returned NULL"); return 1; }
    printf("Initialize: %d\n", (int)(*drv)->Initialize(drv, &host));

    UInt32 got = 0;
    AudioObjectID devs[4];
    for (int i = 0; i < 40; i++) {
        get(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, devs, sizeof devs, &got);
        if (got) break;
        usleep(250000);
    }
    if (!got) { puts("device never appeared (card not claimed)"); return 1; }
    printf("device list: %u device(s)\n", got / 4);
    str(2, kAudioObjectPropertyName, "name");
    str(2, kAudioDevicePropertyDeviceUID, "uid");
    Float64 rate = 0;
    get(2, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, &rate, sizeof rate, &got);
    UInt32 lat = 0;
    get(2, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput, &lat, sizeof lat, &got);
    AudioValueRange rates[4];
    get(2, kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal, rates, sizeof rates, &got);
    printf("  rate %.0f, latency %u frames, %u available rates\n", rate, lat, got / (UInt32)sizeof(AudioValueRange));
    AudioObjectID streams[2];
    get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, streams, sizeof streams, &got);
    UInt32 nout = got;
    (void)nout;
    printf("  output streams: %u (", got / 4);
    for (UInt32 i = 0; i < got / 4; i++) {
        CFStringRef n = NULL; UInt32 g2; char nb[64] = "?"; UInt32 ch = 0;
        get(streams[i], kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &n, sizeof n, &g2);
        if (n) CFStringGetCString(n, nb, sizeof nb, kCFStringEncodingUTF8);
        get(streams[i], kAudioStreamPropertyStartingChannel, kAudioObjectPropertyScopeGlobal, &ch, sizeof ch, &g2);
        printf("%s\"%s\" from channel %u", i ? ", " : "", nb, ch);
    }
    printf("), input streams: ");
    get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, streams, sizeof streams, &got);
    printf("%u\n", got / 4);
    Float32 db = 0.5f;
    get(4, kAudioLevelControlPropertyConvertScalarToDecibels, kAudioObjectPropertyScopeGlobal, &db, sizeof db, &got);
    printf("  volume 0.5 -> %.1f dB\n", db);

    printf("StartIO: %d, streaming for 4 s at 48 kHz\n", (int)(*drv)->StartIO(drv, 2, 1));
    Float64 zs; UInt64 zh, seed;
    run_io(4, 48000);
    (*drv)->GetZeroTimeStamp(drv, 2, 1, &zs, &zh, &seed);
    printf("  zero timestamp sample %.0f\n", zs);

    Float64 r441 = 44100;
    AudioObjectPropertyAddress ra = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    (*drv)->StopIO(drv, 2, 1);
    printf("set 44.1 kHz: %d\n", (int)(*drv)->SetPropertyData(drv, 2, 0, &ra, 0, NULL, sizeof r441, &r441));
    (*drv)->StartIO(drv, 2, 1);
    run_io(3, 44100);
    (*drv)->StopIO(drv, 2, 1);
    Float64 r48 = 48000;
    printf("back to 48 kHz: %d\n", (int)(*drv)->SetPropertyData(drv, 2, 0, &ra, 0, NULL, sizeof r48, &r48));
    sleep(1);
    get(kAudioObjectPlugInObject, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, devs, sizeof devs, &got);
    printf("device still present: %s\n", got ? "yes" : "no");
    return 0;
}
