// MacLogWindow.mm — in-process native macOS log window (AppKit).
// Compiled with -fobjc-arc. Mirrors the Windows Win32 LogWindow.

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include "MacLogWindow.h"
#include "LogBuffer.h"

@interface TCMTLogWindowController : NSObject
- (instancetype)initWithBuffer:(tcmt::LogBuffer*)buffer;
- (void)refresh:(NSTimer*)timer;
- (void)stop;
@end

@implementation TCMTLogWindowController {
    tcmt::LogBuffer* _buffer;
    NSWindow* _window;
    NSTextView* _textView;
    NSTimer* _timer;
    size_t _lastCount;
    size_t _lastVersion;
}

- (instancetype)initWithBuffer:(tcmt::LogBuffer*)buffer {
    if ((self = [super init])) {
        _buffer = buffer;

        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSWindow* win = [[NSWindow alloc]
            initWithContentRect:NSMakeRect(200, 160, 720, 520)
            styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                     NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
            backing:NSBackingStoreBuffered
            defer:NO];
        win.title = @"TCMT - Log";
        win.releasedWhenClosed = NO;  // app owns the window until Stop()

        NSScrollView* scroll = [[NSScrollView alloc] initWithFrame:win.contentView.bounds];
        scroll.hasVerticalScroller = YES;
        scroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;

        NSTextView* tv = [[NSTextView alloc]
            initWithFrame:NSMakeRect(0, 0, scroll.contentSize.width, scroll.contentSize.height)];
        tv.editable = NO;
        tv.font = [NSFont monospacedSystemFontOfSize:12 weight:NSFontWeightRegular];
        tv.autoresizingMask = NSViewWidthSizable;
        scroll.documentView = tv;
        win.contentView = scroll;
        _window = win;
        _textView = tv;

        [win makeKeyAndOrderFront:nil];
        [app activateIgnoringOtherApps:YES];

        _timer = [NSTimer timerWithTimeInterval:0.5 repeats:YES block:^(NSTimer* t) {
            [self refresh:t];
        }];
        [[NSRunLoop mainRunLoop] addTimer:_timer forMode:NSDefaultRunLoopMode];
    }
    return self;
}

- (void)refresh:(NSTimer*)timer {
    (void)timer;
    if (!_buffer) return;

    const size_t n = _buffer->Size();
    const size_t v = _buffer->Version();
    if (n == _lastCount && v == _lastVersion) return;
    _lastCount = n;
    _lastVersion = v;

    const auto lines = _buffer->GetRecent(tcmt::LogBuffer::MAX_LINES);
    NSMutableString* out = [NSMutableString stringWithCapacity:lines.size() * 96];
    for (const auto& line : lines) {
        [out appendString:[NSString stringWithUTF8String:line.c_str()]];
        [out appendString:@"\n"];
    }
    _textView.string = out;
    [_textView scrollRangeToVisible:NSMakeRange(out.length, 0)];
}

- (void)stop {
    [_timer invalidate];
    _timer = nil;
    [_window close];
    _window = nil;
}

@end

namespace tcmt {
namespace mac {

MacLogWindow::~MacLogWindow() {
    Stop();
}

bool MacLogWindow::Create(LogBuffer* buffer) {
    if (controller_) return true;
    // [NSApplication sharedApplication] SIGABRTs when this process has no
    // WindowServer connection (background/degraded session). CGMainDisplayID()
    // returns 0 in that case, so probe first and let callers fall back to the
    // in-TUI log page instead of crashing.
    if (CGMainDisplayID() == 0) return false;
    @try {
        controller_ = (__bridge_retained void*)[[TCMTLogWindowController alloc]
            initWithBuffer:buffer];
    } @catch (NSException*) {
        controller_ = nullptr;
    }
    return controller_ != nullptr;
}

void MacLogWindow::Stop() {
    if (!controller_) return;
    TCMTLogWindowController* c = (__bridge_transfer TCMTLogWindowController*)controller_;
    [c stop];
    controller_ = nullptr;
}

void MacLogWindow::PumpRunLoop(double seconds) {
    @autoreleasepool {
        [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
                              beforeDate:[NSDate dateWithTimeIntervalSinceNow:seconds]];
    }
}

} // namespace mac
} // namespace tcmt
