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

        data_.adapter.powerOn = ([ctrl powerState] == kBluetoothHCIPowerStateON);
        NSString* addr = [ctrl addressAsString];
        if (addr) data_.adapter.address = [addr UTF8String];
        NSString* name = [ctrl nameAsString];
        if (name) data_.adapter.name = [name UTF8String];

        // Paired + connected devices via ObjC API
        NSArray* paired = [IOBluetoothDevice pairedDevices];
        if (paired) {
            for (IOBluetoothDevice* dev in paired) {
                if (![dev isConnected]) continue;
                BluetoothDeviceData d;
                if ([dev name]) d.name = [[dev name] UTF8String];
                if ([dev addressString]) d.address = [[dev addressString] UTF8String];
                d.connected = true;
                data_.devices.push_back(std::move(d));
            }
        }

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
