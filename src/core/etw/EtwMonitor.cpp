// EtwMonitor.cpp — Windows ETW session manager
// Compiled only when TCMT_WINDOWS is defined.

#include "EtwMonitor.h"
#include "../Utils/Logger.h"
#include "../Utils/JThreadCompat.h"

#ifdef TCMT_WINDOWS

#include <winsock2.h>
#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>
#include <tdh.h>
#include <vector>
#include <sstream>
#include <iomanip>
#include <memory>

#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

namespace tcmt { namespace etw {

// ====================================================================
// Provider GUIDs
// ====================================================================
static const GUID kKernelPower       = {0x331C3B3A, 0x2005, 0x44C2, {0xAC, 0x5E, 0x77, 0x22, 0x0C, 0x37, 0xD6, 0xB4}};
static const GUID kTcpip             = {0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xEF, 0x9D, 0x7B, 0xA2, 0x82, 0x18, 0x8A}};
static const GUID kWlanAutoConfig    = {0x9580D7DD, 0x0379, 0x4650, {0x88, 0xB2, 0x3F, 0x78, 0x37, 0x4F, 0x8E, 0x8A}};
static const GUID kBthport           = {0x4638EEFC, 0x8C97, 0x4930, {0xB8, 0xAE, 0x29, 0xE9, 0xC5, 0x67, 0xC0, 0x03}};
static const GUID kUsbUcx            = {0x36DA592D, 0x54B5, 0x4B16, {0x80, 0x45, 0x2E, 0x5E, 0x92, 0x83, 0x70, 0x0F}};
static const GUID kKernelProcPower   = {0x0F67E49F, 0x51E5, 0x4F36, {0x91, 0x14, 0x3D, 0x8E, 0x36, 0xB3, 0x1C, 0x6C}};

static const GUID* kProviders[] = {&kKernelPower, &kTcpip, &kWlanAutoConfig, &kBthport, &kUsbUcx, &kKernelProcPower};
static const char* kProviderNames[] = {"Kernel-Power", "TCPIP", "WLAN-AutoConfig", "BTHPORT", "USB-UCX", "Kernel-Processor-Power"};

// ====================================================================
// Helper: build session name with PID
// ====================================================================
static std::wstring MakeSessionName() {
    return L"TCMT-Monitor-ETW-" + std::to_wstring(GetCurrentProcessId());
}

// ====================================================================
// Construction / Destruction
// ====================================================================
EtwMonitor::EtwMonitor() = default;

EtwMonitor::~EtwMonitor() {
    Stop();
}

// ====================================================================
// Start — create session, enable providers, launch ProcessTrace thread
// ====================================================================
bool EtwMonitor::Start() {
    if (running_.load(std::memory_order_acquire)) return true;

    sessionName_ = MakeSessionName();

    // ---------- Allocate EVENT_TRACE_PROPERTIES ----------
    const ULONG propSize = sizeof(EVENT_TRACE_PROPERTIES) + (sessionName_.size() + 1) * sizeof(wchar_t) + 1024;
    auto propsBuf = std::make_unique<unsigned char[]>(propSize);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(propsBuf.get());
    ZeroMemory(props, propSize);

    props->Wnode.BufferSize    = propSize;
    props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 4; // 4-byte (QPC) timestamps
    props->BufferSize          = 64;  // KB
    props->MinimumBuffers      = 2;
    props->MaximumBuffers      = 8;
    props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_PRIVATE_IN_PROC | EVENT_TRACE_SYSTEM_LOGGER_MODE;
    props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
    props->FlushTimer          = 1; // seconds

    wcscpy_s(reinterpret_cast<wchar_t*>(propsBuf.get() + sizeof(EVENT_TRACE_PROPERTIES)),
             sessionName_.size() + 1, sessionName_.c_str());

    // ---------- StartTrace ----------
    ULONG status = StartTraceW(reinterpret_cast<TRACEHANDLE*>(&sessionHandle_),
                               sessionName_.c_str(), props);
    if (status == ERROR_ALREADY_EXISTS) {
        // Clean up stale session from a previous crashed instance
        EVENT_TRACE_PROPERTIES stopProps{};
        stopProps.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
        stopProps.LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(0, sessionName_.c_str(), &stopProps, EVENT_TRACE_CONTROL_STOP);
        status = StartTraceW(reinterpret_cast<TRACEHANDLE*>(&sessionHandle_),
                             sessionName_.c_str(), props);
    }

    if (status != ERROR_SUCCESS) {
        std::ostringstream oss;
        oss << "StartTraceW failed: " << status;
        lastError_ = oss.str();
        Logger::Warn("EtwMonitor::Start - " + lastError_);
        return false;
    }

    // ---------- Enable providers ----------
    for (int i = 0; i < 6; ++i) {
        status = EnableTraceEx2(reinterpret_cast<TRACEHANDLE>(sessionHandle_),
                                kProviders[i],
                                EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                TRACE_LEVEL_INFORMATION,
                                0xFFFFFFFFFFFFFFFF, // all keywords
                                0, 0, nullptr);
        if (status != ERROR_SUCCESS) {
            Logger::Info(std::string("EtwMonitor: EnableTraceEx2 failed for ") +
                         kProviderNames[i] + " (err=" + std::to_string(status) + ")");
            // Non-fatal: continue with remaining providers
        } else {
            Logger::Debug(std::string("EtwMonitor: enabled ") + kProviderNames[i]);
        }
    }

    // ---------- OpenTrace ----------
    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName           = sessionName_.data();
    logfile.ProcessTraceMode     = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.EventRecordCallback  = [](PEVENT_RECORD evt) {
        auto* self = static_cast<EtwMonitor*>(evt->UserContext);
        if (self) self->DispatchEvent(evt);
    };
    logfile.Context = this;

    TRACEHANDLE th = OpenTraceW(&logfile);
    if (th == INVALID_PROCESSTRACE_HANDLE_VALUE) {
        status = GetLastError();
        std::ostringstream oss;
        oss << "OpenTraceW failed: " << status;
        lastError_ = oss.str();
        Logger::Warn("EtwMonitor::Start - " + lastError_);
        ControlTraceW(reinterpret_cast<TRACEHANDLE>(sessionHandle_), nullptr, props, EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = nullptr;
        return false;
    }
    traceHandle_ = reinterpret_cast<void*>(static_cast<uintptr_t>(th));

    // ---------- Launch background thread ----------
    running_.store(true, std::memory_order_release);
    traceThread_ = tcmt::compat::JThread([this](tcmt::compat::StopToken /*st*/) {
        Logger::Info("EtwMonitor: ProcessTrace started");
        TRACEHANDLE thLocal = reinterpret_cast<TRACEHANDLE>(
            reinterpret_cast<uintptr_t>(this->traceHandle_));
        ULONG result = ProcessTrace(&thLocal, 1, nullptr, nullptr);
        if (result != ERROR_SUCCESS && result != ERROR_CANCELLED) {
            Logger::Warn("EtwMonitor: ProcessTrace returned " + std::to_string(result));
        }
        this->running_.store(false, std::memory_order_release);
        Logger::Info("EtwMonitor: ProcessTrace exited");
    });

    Logger::Info("EtwMonitor: session started — " +
                 std::to_string(sessionName_.size()) + " bytes name");
    return true;
}

// ====================================================================
// Stop — signal session to stop, join background thread
// ====================================================================
void EtwMonitor::Stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    // ControlTrace(STOP) causes ProcessTrace to return
    if (sessionHandle_) {
        EVENT_TRACE_PROPERTIES stopProps{};
        stopProps.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
        stopProps.LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(reinterpret_cast<TRACEHANDLE>(sessionHandle_),
                      sessionName_.c_str(), &stopProps, EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = nullptr;
    }

    if (traceHandle_) {
        CloseTrace(reinterpret_cast<TRACEHANDLE>(reinterpret_cast<uintptr_t>(traceHandle_)));
        traceHandle_ = nullptr;
    }

    // JThread destructor auto-joins
    // We need to manually join since JThread is a member
    // The thread will have exited because ProcessTrace returned
    Logger::Debug("EtwMonitor: session stopped");
}

// ====================================================================
// DispatchEvent — route by ProviderId to registered callbacks
// ====================================================================
void EtwMonitor::DispatchEvent(const void* pEvent) {
    const auto* evt = static_cast<const EVENT_RECORD*>(pEvent);
    if (!evt) return;

    const GUID& pid = evt->EventHeader.ProviderId;

    try {
        if (IsEqualGUID(pid, kKernelPower)) {
            unsigned short id = evt->EventHeader.EventDescriptor.Id;
            if (id == 105 && powerCb_) {
                // PowerSourceChange
                powerCb_(false); // Simplified: AC off
            } else if (id == 107 && powerCb_) {
                powerCb_(true); // AC on
            } else if (id == 106 && batteryCb_) {
                // BatteryPercentageChange — try to extract value
                batteryCb_(-1); // Placeholder; actual parsing via TDH if needed
            }
        } else if (IsEqualGUID(pid, kTcpip)) {
            if (networkCb_) networkCb_();
        } else if (IsEqualGUID(pid, kWlanAutoConfig)) {
            if (wifiCb_) wifiCb_();
        } else if (IsEqualGUID(pid, kBthport)) {
            if (btCb_) btCb_();
        } else if (IsEqualGUID(pid, kUsbUcx)) {
            if (usbCb_) usbCb_();
        } else if (IsEqualGUID(pid, kKernelProcPower)) {
            if (cpuFreqCb_) cpuFreqCb_();
        }
    } catch (const std::exception& e) {
        Logger::Error(std::string("EtwMonitor::DispatchEvent exception: ") + e.what());
    } catch (...) {
        Logger::Error("EtwMonitor::DispatchEvent unknown exception");
    }
}

void EtwMonitor::EventRecordCallback(void* context, const void* pEvent) {
    auto* self = static_cast<EtwMonitor*>(context);
    if (self) self->DispatchEvent(pEvent);
}

bool EtwMonitor::GetEventId(const void* pEvent, unsigned short* outId) {
    if (!pEvent || !outId) return false;
    *outId = static_cast<const EVENT_RECORD*>(pEvent)->EventHeader.EventDescriptor.Id;
    return true;
}

std::string EtwMonitor::FormatProviderName(const void* pEvent) {
    const auto* evt = static_cast<const EVENT_RECORD*>(pEvent);
    if (!evt) return "null";
    const GUID& pid = evt->EventHeader.ProviderId;
    for (int i = 0; i < 6; ++i) {
        if (IsEqualGUID(pid, *kProviders[i])) return kProviderNames[i];
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(8) << pid.Data1 << "-"
        << std::setw(4) << pid.Data2 << "-"
        << std::setw(4) << pid.Data3;
    return oss.str();
}

} // namespace etw
} // namespace tcmt

#endif // TCMT_WINDOWS
