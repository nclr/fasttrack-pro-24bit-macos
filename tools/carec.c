/* Record from the Fast Track Pro's input through the macOS driver (configuration 1) and
 * report level and whether the samples show the repeated-byte pattern seen on raw USB.
 * Needs Microphone permission for the terminal app, or macOS delivers silence.
 *
 *   carec <seconds>
 */
#include <CoreAudio/CoreAudio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { MAXF = 48000 * 30 };
static float g_buf[2][MAXF];
static volatile unsigned g_n;

static OSStatus proc(AudioObjectID d, const AudioTimeStamp *now, const AudioBufferList *in, const AudioTimeStamp *it,
                     AudioBufferList *out, const AudioTimeStamp *ot, void *c) {
    (void)d; (void)now; (void)it; (void)out; (void)ot; (void)c;
    if (!in || !in->mNumberBuffers) return 0;
    const AudioBuffer *b = &in->mBuffers[0];
    UInt32 ch = b->mNumberChannels, n = b->mDataByteSize / (4 * ch);
    const float *p = b->mData;
    for (UInt32 i = 0; i < n && g_n < MAXF; i++, g_n++) {
        g_buf[0][g_n] = p[i * ch];
        g_buf[1][g_n] = ch > 1 ? p[i * ch + 1] : p[i * ch];
    }
    return 0;
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 4;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectID devs[64], dev = 0;
    UInt32 sz = sizeof devs;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, devs);
    for (UInt32 i = 0; i < sz / 4 && !dev; i++) {
        CFStringRef s = NULL;
        UInt32 ss = sizeof s;
        char name[128] = "";
        AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectGetPropertyData(devs[i], &na, 0, NULL, &ss, &s);
        if (s) { CFStringGetCString(s, name, sizeof name, kCFStringEncodingUTF8); CFRelease(s); }
        AudioObjectPropertyAddress sa = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
        UInt32 n = 0;
        AudioObjectGetPropertyDataSize(devs[i], &sa, 0, NULL, &n);
        if (strstr(name, "FastTrack") && n) dev = devs[i];
    }
    if (!dev) { puts("FastTrack Pro input not found (is the macOS driver attached? run: ftconfig release)"); return 1; }
    AudioDeviceIOProcID id;
    AudioDeviceCreateIOProcID(dev, proc, NULL, &id);
    AudioDeviceStart(dev, id);
    printf("recording %d s from FastTrack Pro input...\n", secs);
    fflush(stdout);
    sleep(secs);
    AudioDeviceStop(dev, id);
    unsigned n = g_n;
    for (int c = 0; c < 2; c++) {
        double peak = 0, sum = 0;
        unsigned dup = 0, zero = 0;
        for (unsigned i = 0; i < n; i++) {
            float x = g_buf[c][i];
            peak = fmax(peak, fabs(x));
            sum += (double)x * x;
            int v = (int)lrintf(x * 32768.0f) & 0xffff;
            if (x == 0) zero++;
            else if ((v & 0xff) == (v >> 8)) dup++;
        }
        double rms = n ? sqrt(sum / n) : 0;
        printf("input %d: %u samples, peak %.4f, rms %.5f (%.1f dBFS), exact zeros %u, repeated-byte samples %u\n", c + 1, n, peak,
               rms, rms > 0 ? 20 * log10(rms) : -999.0, zero, dup);
    }
    printf("first samples ch1:");
    for (unsigned i = 0; i < 8 && i < n; i++) printf(" %.5f", g_buf[0][i]);
    printf("\n");
    return 0;
}
