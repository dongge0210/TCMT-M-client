// BluetoothInfo.mm — macOS Bluetooth monitoring via IOBluetooth.framework
// Objective-C++ — compiled only when TCMT_MACOS is defined

#include "BluetoothInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_MACOS

#import <IOBluetooth/IOBluetooth.h>

void BluetoothInfo::Detect() {
    Clear();

    @autoreleasepool {
        IOBluetoothHostController* ctrl = [IOBluetoothHostController defaultController];
        if (!ctrl) {
            Logger::Debug("BluetoothInfo: no controller");
            return;
        }

        data_.adapter.detected = true;
        data_.adapter.powerOn = ([ctrl powerState] == kBluetoothHCIPowerStateON);
        NSString* addr = [ctrl addressAsString];
        if (addr) data_.adapter.address = [addr UTF8String];
        NSString* name = [ctrl nameAsString];
        if (name) data_.adapter.name = [name UTF8String];

        // Paired devices — show all, not just connected
        NSArray* paired = [IOBluetoothDevice pairedDevices];
        if (paired) {
            for (IOBluetoothDevice* dev in paired) {
                BluetoothDeviceData d;
                if ([dev name]) d.name = [[dev name] UTF8String];
                if ([dev addressString]) d.address = [[dev addressString] UTF8String];
                d.connected = [dev isConnected];
                d.remembered = true;
                // RSSI only valid when connected
                if ([dev isConnected]) d.rssi = (int)[dev RSSI];
                data_.devices.push_back(std::move(d));
            }
        }
        // NOTE: Inquiry scan (IOBluetoothDeviceInquiry) is NOT performed here
        // because it blocks for 3-5 seconds per call and Detect() runs every
        // ~3 seconds.  If a nearby-device scan is needed it should be a
        // separate one-shot API, not part of the periodic collection loop.

        Logger::Debug("BluetoothInfo: " + data_.adapter.name +
                      " devices=" + std::to_string(data_.devices.size()));
    }
}

void BluetoothInfo::Clear() { data_ = BluetoothData{}; }
const BluetoothData& BluetoothInfo::GetData() const { return data_; }

#else
void BluetoothInfo::Detect() {}
void BluetoothInfo::Clear() { data_ = BluetoothData{}; }
const BluetoothData& BluetoothInfo::GetData() const { return data_; }
#endif
