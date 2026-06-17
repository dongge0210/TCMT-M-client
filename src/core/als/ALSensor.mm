// ALSensor.mm — macOS Ambient Light Sensor via IOKit AppleSPUVD6286
#include "ALSensor.h"
#import <IOKit/IOKitLib.h>
#import <Foundation/Foundation.h>

void ALSensor::Detect() {
    data_ = ALSData{};

    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUVD6286");
    if (!matching) return;

    io_iterator_t iterator;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iterator);
    if (kr != KERN_SUCCESS) return;

    io_object_t service;
    while ((service = IOIteratorNext(iterator)) != 0) {
        CFMutableDictionaryRef properties = nullptr;
        kr = IORegistryEntryCreateCFProperties(service, &properties, kCFAllocatorDefault, 0);
        if (kr == KERN_SUCCESS && properties) {
            NSDictionary* dict = (__bridge NSDictionary*)properties;

            NSNumber* lux = dict[@"CurrentLux"];
            if (lux) {
                data_.lux = [lux doubleValue];
                data_.valid = true;
            }

            CFRelease(properties);
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
}
