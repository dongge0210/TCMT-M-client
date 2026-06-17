// BatteryHealth.mm — macOS battery health via IOKit AppleSmartBattery
#include "BatteryHealth.h"
#import <IOKit/IOKitLib.h>
#import <Foundation/Foundation.h>

void BatteryHealth::Detect() {
    data_ = BatteryHealthData{};

    CFMutableDictionaryRef matching = IOServiceMatching("AppleSmartBattery");
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

            // Design capacity (mAh) — original factory spec
            data_.designCapacity = [dict[@"DesignCapacity"] intValue];

            // Current max capacity (mAh) — may have degraded
            data_.maxCapacity = [dict[@"AppleRawMaxCapacity"] intValue];

            // Cycle count
            data_.cycleCount = [dict[@"CycleCount"] intValue];

            // Health percentage
            if (data_.designCapacity > 0 && data_.maxCapacity > 0) {
                data_.healthPercent = 100.0 * data_.maxCapacity / data_.designCapacity;
            } else if (data_.designCapacity > 0) {
                // Fallback: MaxCapacity is 0-100 percentage, design is mAh
                // Use NominalChargeCapacity if available
                int nominal = [dict[@"NominalChargeCapacity"] intValue];
                if (nominal > 0)
                    data_.healthPercent = 100.0 * nominal / data_.designCapacity;
            }

            // Temperature in decikelvin (0.1 K) → Celsius
            int tempDK = [dict[@"Temperature"] intValue];
            if (tempDK > 0)
                data_.temperature = (double)tempDK / 10.0 - 273.15;

            // Amperage (mA) — positive = charging, negative = discharging
            data_.amperage = [dict[@"Amperage"] intValue];

            // Voltage (mV)
            data_.voltage = [dict[@"Voltage"] intValue];

            // Charger details (adapter rated wattage)
            NSDictionary* adapter = dict[@"AdapterDetails"];
            if (adapter && [adapter isKindOfClass:[NSDictionary class]]) {
                NSNumber* watts = adapter[@"Watts"];
                if (watts) data_.chargerWatts = [watts doubleValue];
            }

            // Charging / AC status
            data_.isCharging = [dict[@"IsCharging"] boolValue];
            data_.externalConnected = [dict[@"ExternalConnected"] boolValue];
            data_.present = true;
        }
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
}
