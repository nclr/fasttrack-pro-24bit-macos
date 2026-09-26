/* Switch the Fast Track Pro's USB configuration.
 *
 *   ftconfig status    print the current configuration
 *   ftconfig claim     configuration 2 (24-bit alt settings) with interface matching off,
 *                      so the macOS driver stays detached and tools can drive the card
 *   ftconfig release   configuration 1 with matching on: the normal 16-bit macOS device
 *   ftconfig claim1    configuration 1 with matching off (16-bit class-compliant mode, for tests)
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *cmd = argc > 1 ? argv[1] : "";
    int claim = !strcmp(cmd, "claim"), claim1 = !strcmp(cmd, "claim1"), release = !strcmp(cmd, "release"), status = !strcmp(cmd, "status");
    if (!claim && !claim1 && !release && !status) {
        fprintf(stderr, "usage: ftconfig status|claim|claim1|release\n");
        return 2;
    }

    int vendor = 0x0763, product = 0x2012;
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &vendor);
    CFNumberRef p = CFNumberCreate(NULL, kCFNumberSInt32Type, &product);
    CFDictionarySetValue(m, CFSTR(kUSBVendorID), v);
    CFDictionarySetValue(m, CFSTR(kUSBProductID), p);
    CFRelease(v);
    CFRelease(p);
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, m);
    if (!svc) {
        fprintf(stderr, "Fast Track Pro (0763:2012) not found\n");
        return 1;
    }
    IOCFPlugInInterface **plug = NULL;
    SInt32 score = 0;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (kr || !plug) {
        fprintf(stderr, "IOCreatePlugInInterfaceForService: 0x%x\n", kr);
        return 1;
    }
    IOUSBDeviceInterface650 **dev = NULL;
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&dev);
    (*plug)->Release(plug);
    if (!dev) {
        fprintf(stderr, "QueryInterface failed\n");
        return 1;
    }

    UInt8 cfg = 0;
    (*dev)->GetConfiguration(dev, &cfg);
    if (status) {
        printf("configuration %u\n", cfg);
        (*dev)->Release(dev);
        return 0;
    }

    IOReturn r = (*dev)->USBDeviceOpenSeize(dev);
    if (r == kIOReturnSuccess) {
        if (claim) {
            (*dev)->SetConfigurationV2(dev, 1, false, false);
            r = (*dev)->SetConfigurationV2(dev, 2, false, false);
        } else if (claim1) {
            (*dev)->SetConfigurationV2(dev, 2, false, false);
            r = (*dev)->SetConfigurationV2(dev, 1, false, false);
        } else {
            r = (*dev)->SetConfigurationV2(dev, 1, true, false);
        }
        (*dev)->GetConfiguration(dev, &cfg);
        (*dev)->USBDeviceClose(dev);
    }
    (*dev)->Release(dev);
    if (r) {
        fprintf(stderr, "failed: 0x%x\n", r);
        return 1;
    }
    printf("configuration %u, macOS driver %s\n", cfg, claim || claim1 ? "detached" : "attached");
    return 0;
}
