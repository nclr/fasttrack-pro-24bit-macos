/* Read-only: list Core Audio devices, their streams and formats. */
#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <stdlib.h>

static void name(AudioObjectID id, char *b, size_t n) {
    AudioObjectPropertyAddress a = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef s = NULL; UInt32 sz = sizeof(s); b[0] = 0;
    if (AudioObjectGetPropertyData(id, &a, 0, NULL, &sz, &s) == noErr && s) {
        CFStringGetCString(s, b, n, kCFStringEncodingUTF8); CFRelease(s);
    }
}

static void fmt(const char *tag, const AudioStreamBasicDescription *f) {
    printf("      %s %.0f Hz %u ch %u bit %u B/frame flags 0x%x%s\n", tag, f->mSampleRate,
           f->mChannelsPerFrame, f->mBitsPerChannel, f->mBytesPerFrame, f->mFormatFlags,
           (f->mFormatFlags & kAudioFormatFlagIsBigEndian) ? " BE" : "");
}

static void streams(AudioObjectID dev, AudioObjectPropertyScope scope, const char *label) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &sz) || !sz) return;
    AudioObjectID st[16]; if (sz > sizeof(st)) sz = sizeof(st);
    AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, st);
    for (UInt32 i = 0; i < sz / sizeof(AudioObjectID); i++) {
        printf("    %s stream %u (id %u)\n", label, i + 1, st[i]);
        AudioStreamBasicDescription f; UInt32 fs = sizeof(f);
        AudioObjectPropertyAddress p = {kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        if (!AudioObjectGetPropertyData(st[i], &p, 0, NULL, &fs, &f)) fmt("physical:", &f);
        p.mSelector = kAudioStreamPropertyVirtualFormat; fs = sizeof(f);
        if (!AudioObjectGetPropertyData(st[i], &p, 0, NULL, &fs, &f)) fmt("virtual: ", &f);
        p.mSelector = kAudioStreamPropertyAvailablePhysicalFormats;
        UInt32 rs = 0;
        if (!AudioObjectGetPropertyDataSize(st[i], &p, 0, NULL, &rs) && rs) {
            AudioStreamRangedDescription *r = malloc(rs);
            AudioObjectGetPropertyData(st[i], &p, 0, NULL, &rs, r);
            for (UInt32 k = 0; k < rs / sizeof(*r); k++) fmt("avail:   ", &r[k].mFormat);
            free(r);
        }
    }
}

int main(void) {
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0;
    AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, NULL, &sz);
    AudioObjectID *d = malloc(sz);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, d);
    AudioObjectID defOut = 0; UInt32 ds = sizeof(defOut);
    AudioObjectPropertyAddress da = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &ds, &defOut);
    for (UInt32 i = 0; i < sz / sizeof(AudioObjectID); i++) {
        char n[256]; name(d[i], n, sizeof n);
        Float64 rate = 0; UInt32 rsz = sizeof(rate);
        AudioObjectPropertyAddress r = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectGetPropertyData(d[i], &r, 0, NULL, &rsz, &rate);
        pid_t hog = 0; UInt32 hs = sizeof(hog);
        r.mSelector = kAudioDevicePropertyHogMode;
        AudioObjectGetPropertyData(d[i], &r, 0, NULL, &hs, &hog);
        printf("device %u \"%s\" %.0f Hz hog %d%s\n", d[i], n, rate, hog, d[i] == defOut ? " [default output]" : "");
        streams(d[i], kAudioObjectPropertyScopeOutput, "out");
        streams(d[i], kAudioObjectPropertyScopeInput, "in ");
    }
    return 0;
}
