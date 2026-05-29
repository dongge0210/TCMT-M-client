// DisplayInfo.mm — macOS display/monitor info via NSScreen + CGDisplay
// Objective-C++ (.mm) — compiled only when TCMT_MACOS is defined

#include "DisplayInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_MACOS

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

// Forward declaration for newer macOS APIs (deployment target is 11.0)
@interface NSScreen (TCMTFallback)
- (float)maximumPotentialFrameRate API_AVAILABLE(macos(14.0));
@end

void DisplayInfo::Detect() {
    Clear();

    @autoreleasepool {
        NSArray<NSScreen*>* screens = [NSScreen screens];
        if (!screens || screens.count == 0) {
            Logger::Debug("DisplayInfo: no screens found");
            return;
        }

        for (NSScreen* screen in screens) {
            DisplayInfoData dd;

            // --- Screen name ---
            // Use localizedName (macOS 14+), fall back to device description
            if ([screen respondsToSelector:@selector(localizedName)]) {
                dd.name = [screen.localizedName UTF8String];
            } else {
                dd.name = [[screen.deviceDescription objectForKey:@"NSScreenNumber"] stringValue].UTF8String;
            }

            // --- Resolution (points → pixels via backingScaleFactor) ---
            NSRect frame = [screen frame];
            double scale = [screen backingScaleFactor];
            dd.backingScale = scale;
            dd.width  = (int)(frame.size.width  * scale);
            dd.height = (int)(frame.size.height * scale);

            // --- Built-in (laptop panel) ---
            // On Apple Silicon, the built-in display has vendorNumber from IOKit
            NSDictionary* desc = [screen deviceDescription];
            NSNumber* screenID = [desc objectForKey:@"NSScreenNumber"];
            if (screenID) {
                CGDirectDisplayID displayID = [screenID unsignedIntValue];
                dd.isBuiltin = (CGDisplayIsBuiltin(displayID) != 0);
            }

            // --- Refresh rate ---
            if (screenID) {
                CGDirectDisplayID displayID = [screenID unsignedIntValue];
                CGDisplayModeRef mode = CGDisplayCopyDisplayMode(displayID);
                if (mode) {
                    dd.refreshRate = (int)CGDisplayModeGetRefreshRate(mode);
                    CGDisplayModeRelease(mode);
                }
                // Fallback: maximumFramesPerSecond (macOS 12+)
                if (dd.refreshRate == 0) {
                    if (@available(macOS 12.0, *)) {
                        if ([screen respondsToSelector:@selector(maximumFramesPerSecond)]) {
                            dd.refreshRate = (int)[screen maximumFramesPerSecond];
                        }
                    }
                }
                // Fallback: maximumPotentialFrameRate (macOS 14+)
                if (dd.refreshRate == 0) {
                    if (@available(macOS 14.0, *)) {
                        if ([screen respondsToSelector:@selector(maximumPotentialFrameRate)]) {
                            dd.refreshRate = (int)[screen maximumPotentialFrameRate];
                        }
                    }
                }
            }

            // --- HDR (EDR support) ---
            // Screen supports EDR if maximumExtendedDynamicRangeColorComponentValue > 1.0
            if ([screen respondsToSelector:@selector(maximumExtendedDynamicRangeColorComponentValue)]) {
                dd.isHDR = ([screen maximumExtendedDynamicRangeColorComponentValue] > 1.0);
            }

            displays_.push_back(dd);

            Logger::Debug("DisplayInfo: " + dd.name +
                          " " + std::to_string(dd.width) + "x" + std::to_string(dd.height) +
                          " @" + std::to_string(dd.refreshRate) + "Hz" +
                          " scale=" + std::to_string(dd.backingScale) +
                          " builtin=" + (dd.isBuiltin ? "1" : "0") +
                          " HDR=" + (dd.isHDR ? "1" : "0"));
        }
    }
}

void DisplayInfo::Clear() {
    displays_.clear();
}

const std::vector<DisplayInfoData>& DisplayInfo::GetDisplays() const {
    return displays_;
}

#else
// ======================== Non-macOS stub ========================

void DisplayInfo::Detect() {
    Logger::Debug("DisplayInfo: not implemented on this platform");
}

void DisplayInfo::Clear() {}

const std::vector<DisplayInfoData>& DisplayInfo::GetDisplays() const {
    static std::vector<DisplayInfoData> empty;
    return empty;
}

#endif
