// BluetoothInfo.mm — macOS Bluetooth monitoring via IOBluetooth.framework
// Objective-C++ — compiled only when TCMT_MACOS is defined

#include "BluetoothInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_MACOS

#import <IOBluetooth/IOBluetooth.h>

// Simple delegate for IOBluetoothDeviceInquiry
@interface TCMTInquiryDelegate : NSObject <IOBluetoothDeviceInquiryDelegate>
@property BOOL done;
@end

@implementation TCMTInquiryDelegate
- (void)deviceInquiryComplete:(IOBluetoothDeviceInquiry *)sender error:(IOReturn)error aborted:(BOOL)aborted {
    _done = YES;
}
@end

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

        // Inquiry scan for nearby/discoverable devices (~3s)
        TCMTInquiryDelegate* delegate = [[TCMTInquiryDelegate alloc] init];
        delegate.done = NO;
        IOBluetoothDeviceInquiry* inquiry = [IOBluetoothDeviceInquiry inquiryWithDelegate:delegate];
        inquiry.inquiryLength = 3;
        inquiry.updateNewDeviceNames = YES;
        [inquiry start];

        // Spin run loop until complete or 5s timeout
        NSDate* timeout = [NSDate dateWithTimeIntervalSinceNow:5.0];
        while (!delegate.done && [timeout timeIntervalSinceNow] > 0) {
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode
                                      beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        }
        [inquiry stop];

        NSArray* found = [inquiry foundDevices];
        if (found) {
            for (IOBluetoothDevice* dev in found) {
                NSString* addr = [dev addressString];
                if (!addr) continue;
                std::string addrStr = [addr UTF8String];
                // Skip if already in paired list
                bool dup = false;
                for (const auto& d : data_.devices) {
                    if (d.address == addrStr) { dup = true; break; }
                }
                if (dup) continue;
                BluetoothDeviceData d;
                if ([dev name]) d.name = [[dev name] UTF8String];
                d.address = addrStr;
                d.connected = [dev isConnected];
                d.remembered = false;
                if ([dev isConnected]) d.rssi = (int)[dev RSSI];
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
