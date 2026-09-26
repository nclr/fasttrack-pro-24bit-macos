/* Loads FastTrack24.driver the way coreaudiod does and exercises it without installing:
 * waits for the device, prints its properties, runs simulated IO (silence), switches to
 * 44.1 kHz and back. Usage: testhost [path/to/FastTrack24.driver] */
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
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

static void run_io(double seconds, Float64 rate) {
    float buf[512 * 2];
    AudioServerPlugInIOCycleInfo cycle_info = {0};
    memset(buf, 0, sizeof buf);
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    uint64_t start = mach_absolute_time();
    double period = 512.0 / rate;
    for (int cycle = 0; cycle * period < seconds; cycle++) {
        (*drv)->DoIOOperation(drv, 2, 3, 1, kAudioServerPlugInIOOperationWriteMix, 512, &cycle_info, buf, NULL);
        uint64_t due = start + (uint64_t)((cycle + 1) * period * 1e9 * tb.denom / tb.numer);
        while (mach_absolute_time() < due) usleep(500);
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "FastTrack24.driver";
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), true);
    CFBundleRef b = CFBundleCreate(NULL, url);
    if (!b || !CFBundleLoadExecutable(b)) { puts("could not load bundle"); return 1; }
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
    printf("  output streams: %u, input streams: ", got / 4);
    get(2, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, streams, sizeof streams, &got);
    printf("%u\n", got / 4);
    Float32 db = 0.5f;
    get(4, kAudioLevelControlPropertyConvertScalarToDecibels, kAudioObjectPropertyScopeGlobal, &db, sizeof db, &got);
    printf("  volume 0.5 -> %.1f dB\n", db);

    printf("StartIO: %d, streaming silence for 4 s at 48 kHz\n", (int)(*drv)->StartIO(drv, 2, 1));
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
