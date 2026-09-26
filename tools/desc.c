/* Dump the FastTrack Pro configuration descriptors (read-only, no device open). */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>

static void dump(const uint8_t *p, int len) {
    for (int i = 0; i < len && p[i];) {
        int l = p[i], t = p[i + 1];
        if (t == 4) printf("\nINTERFACE %d alt %d class %d sub %d eps %d\n", p[i + 2], p[i + 3], p[i + 5], p[i + 6], p[i + 4]);
        else if (t == 5) printf("  ENDPOINT 0x%02x attr 0x%02x maxpkt %d interval %d\n", p[i + 2], p[i + 3], p[i + 4] | p[i + 5] << 8, p[i + 6]);
        else if (t == 0x24 && p[i + 2] == 2 && l >= 8)
            printf("  FORMAT_TYPE %d ch %d subframe %d bits %d rates:", p[i + 3], p[i + 4], p[i + 5], p[i + 6]);
        else if (t == 0x24 && p[i + 2] == 1 && l == 7)
            printf("  AS_GENERAL terminal %d delay %d format 0x%04x\n", p[i + 3], p[i + 4], p[i + 5] | p[i + 6] << 8);
        if (t == 0x24 && p[i + 2] == 2 && l >= 8) {
            int n = p[i + 7];
            for (int k = 0; k < n; k++) { const uint8_t *r = p + i + 8 + 3 * k; printf(" %d", r[0] | r[1] << 8 | r[2] << 16); }
            printf("\n");
        }
        if (t != 4 && t != 5 && !(t == 0x24 && (p[i + 2] == 1 || p[i + 2] == 2))) {
            printf("  desc type 0x%02x len %d:", t, l);
            for (int k = 2; k < l; k++) printf(" %02x", p[i + k]);
            printf("\n");
        }
        i += l;
    }
}

int main(void) {
    int vendor = 0x0763, product = 0x2012;
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &vendor);
    CFNumberRef pr = CFNumberCreate(NULL, kCFNumberSInt32Type, &product);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), v);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), pr);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!svc) { puts("not found"); return 1; }
    IOCFPlugInInterface **plug; SInt32 score;
    IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOUSBDeviceInterface650 **dev;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&dev);
    for (int c = 0; c < 2; c++) {
        IOUSBConfigurationDescriptorPtr d = NULL;
        IOReturn r = (*dev)->GetConfigurationDescriptorPtr(dev, c, &d);
        if (r || !d) { printf("config index %d: 0x%x\n", c, r); continue; }
        printf("\n===== CONFIGURATION value %d (%d interfaces, %d bytes)\n", d->bConfigurationValue, d->bNumInterfaces, USBToHostWord(d->wTotalLength));
        dump((const uint8_t *)d + d->bLength, USBToHostWord(d->wTotalLength) - d->bLength);
    }
    return 0;
}
