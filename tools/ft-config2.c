#include <stdio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

/* usage: ft-config2 [claim|release]
 *   (none)   switch to configuration 2, macOS driver attaches (only if not already 2)
 *   claim    re-select configuration 2 with interface matching OFF: usbaudiod stays away
 *   release  re-select configuration 2 with matching ON: macOS driver takes the card back
 */
#include <string.h>
int main(int argc, char **argv) {
    int force = argc > 1, matching = !(argc > 1 && !strcmp(argv[1], "claim"));
    int vendor = 0x0763, product = 0x2012;
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &vendor);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberSInt32Type, &product);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), v);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), p);
    CFRelease(v); CFRelease(p);

    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!svc) { fprintf(stderr, "FastTrack Pro not found\n"); return 1; }

    IOCFPlugInInterface **plug = NULL; SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(
        svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (kr || !plug) { fprintf(stderr, "plugin 0x%x\n", kr); return 1; }

    IOUSBDeviceInterface650 **dev = NULL;
    HRESULT hr = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&dev);
    (*plug)->Release(plug);
    if (hr || !dev) { fprintf(stderr, "QueryInterface failed\n"); return 1; }

    UInt8 cfg = 0;
    (*dev)->GetConfiguration(dev, &cfg);
    printf("before: %u\n", cfg);

    IOReturn r = (*dev)->USBDeviceOpenSeize(dev);
    printf("USBDeviceOpenSeize: 0x%x\n", r);
    if (r == kIOReturnSuccess && (force || cfg != 2)) {
        if (force) (*dev)->SetConfigurationV2(dev, 1, false, false);
        r = (*dev)->SetConfigurationV2(dev, 2, matching, false);
        printf("SetConfigurationV2(2, matching=%d): 0x%x\n", matching, r);
        (*dev)->GetConfiguration(dev, &cfg);
        printf("after: %u\n", cfg);
    }
    if (r == kIOReturnSuccess) (*dev)->USBDeviceClose(dev);
    (*dev)->Release(dev);
    return r == kIOReturnSuccess ? 0 : 1;
}
