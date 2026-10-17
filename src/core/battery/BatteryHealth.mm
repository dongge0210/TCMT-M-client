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

            // On Apple Silicon the capacity/cycle/temperature fields live in
            // the nested "BatteryData" dictionary (top-level only has a few,
            // like CycleCount). Prefer BatteryData, fall back to top level.
            NSDictionary* bd = dict[@"BatteryData"];
            if (![bd isKindOfClass:[NSDictionary class]]) bd = nil;
            auto readInt = [&](NSString* key) -> int {
                if (bd) {
                    id v = bd[key];
                    if ([v respondsToSelector:@selector(intValue)]) return [v intValue];
                }
                id v = dict[key];
                return [v respondsToSelector:@selector(intValue)] ? [v intValue] : 0;
            };

            // Design capacity (mAh) — original factory spec
            data_.designCapacity = readInt(@"DesignCapacity");

            // Current max capacity (mAh) — may have degraded
            data_.maxCapacity = readInt(@"AppleRawMaxCapacity");
            if (data_.maxCapacity <= 0)
                data_.maxCapacity = readInt(@"FullChargeCapacity");

            // Cycle count
            data_.cycleCount = readInt(@"CycleCount");

            // Health percentage
            if (data_.designCapacity > 0 && data_.maxCapacity > 0) {
                data_.healthPercent = 100.0 * data_.maxCapacity / data_.designCapacity;
            } else if (data_.designCapacity > 0) {
                // Fallback: raw max absent — use NominalChargeCapacity
                int nominal = readInt(@"NominalChargeCapacity");
                if (nominal > 0)
                    data_.healthPercent = 100.0 * nominal / data_.designCapacity;
            }

            // Temperature in decikelvin (0.1 K) → Celsius
            int tempDK = readInt(@"Temperature");
            if (tempDK > 0)
                data_.temperature = (double)tempDK / 10.0 - 273.15;

            // Amperage (mA) — positive = charging, negative = discharging
            data_.amperage = readInt(@"Amperage");

            // Voltage (mV)
            data_.voltage = readInt(@"Voltage");

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
