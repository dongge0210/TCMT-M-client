// EtwMonitor.cpp — Windows ETW session manager

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

#pragma comment(lib, "tdh.lib")
#pragma comment(lib, "advapi32.lib")

namespace tcmt { namespace etw {

// ====================================================================
// Provider GUIDs
// ====================================================================
static const GUID kKernelPower     = {0x331C3B3A, 0x2005, 0x44C2, {0xAC, 0x5E, 0x77, 0x22, 0x0C, 0x37, 0xD6, 0xB4}};
static const GUID kTcpip           = {0x2F07E2EE, 0x15DB, 0x40F1, {0x90, 0xEF, 0x9D, 0x7B, 0xA2, 0x82, 0x18, 0x8A}};
static const GUID kWlanAutoConfig  = {0x9580D7DD, 0x0379, 0x4650, {0x88, 0xB2, 0x3F, 0x78, 0x37, 0x4F, 0x8E, 0x8A}};
static const GUID kBthport         = {0x4638EEFC, 0x8C97, 0x4930, {0xB8, 0xAE, 0x29, 0xE9, 0xC5, 0x67, 0xC0, 0x03}};
static const GUID kUsbUcx          = {0x36DA592D, 0x54B5, 0x4B16, {0x80, 0x45, 0x2E, 0x5E, 0x92, 0x83, 0x70, 0x0F}};
static const GUID kKernelProcPower = {0x0F67E49F, 0x51E5, 0x4F36, {0x91, 0x14, 0x3D, 0x8E, 0x36, 0xB3, 0x1C, 0x6C}};

static const GUID* kProviders[]   = {&kKernelPower, &kTcpip,  &kWlanAutoConfig,
                                     &kBthport,      &kUsbUcx, &kKernelProcPower};
static const char* kProviderNames[] = {"Kernel-Power", "TCPIP", "WLAN-AutoConfig",
                                       "BTHPORT", "USB-UCX", "Kernel-Processor-Power"};

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
EtwMonitor::~EtwMonitor() { Stop(); }

// ====================================================================
// Start
// ====================================================================
bool EtwMonitor::Start() {
    if (running_.load(std::memory_order_acquire)) return true;

    sessionName_ = MakeSessionName();

    // ---------- Allocate EVENT_TRACE_PROPERTIES ----------
    const ULONG nameBytes = static_cast<ULONG>((sessionName_.size() + 1) * sizeof(wchar_t));
    const ULONG propSize  = sizeof(EVENT_TRACE_PROPERTIES) + nameBytes + 1024;
    std::vector<unsigned char> buf(propSize, 0);
    auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());

    props->Wnode.BufferSize    = propSize;
    props->Wnode.Flags         = WNODE_FLAG_TRACED_GUID;
    props->Wnode.ClientContext = 4; // QPC timestamps
    props->BufferSize          = 64;  // KB
    props->MinimumBuffers      = 2;
    props->MaximumBuffers      = 8;
    props->LogFileMode         = EVENT_TRACE_REAL_TIME_MODE;
    props->LoggerNameOffset    = sizeof(EVENT_TRACE_PROPERTIES);
    props->FlushTimer          = 1;

    wcscpy_s(reinterpret_cast<wchar_t*>(buf.data() + sizeof(EVENT_TRACE_PROPERTIES)),
             sessionName_.size() + 1, sessionName_.c_str());

    // ---------- StartTrace ----------
    ULONG status = StartTraceW(reinterpret_cast<TRACEHANDLE*>(&sessionHandle_),
                               sessionName_.c_str(), props);
    if (status == ERROR_ALREADY_EXISTS) {
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
        ULONG enStatus = EnableTraceEx2(reinterpret_cast<TRACEHANDLE>(sessionHandle_),
                                        kProviders[i],
                                        EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                                        TRACE_LEVEL_INFORMATION,
                                        0xFFFFFFFFFFFFFFFF,
                                        0, 0, nullptr);
        if (enStatus != ERROR_SUCCESS && enStatus != ERROR_NO_SYSTEM_RESOURCES) {
            Logger::Warn(std::string("EtwMonitor: EnableTraceEx2 failed for ") +
                         kProviderNames[i] + " (err=" + std::to_string(enStatus) + ")");
        }
    }

    // ---------- OpenTrace ----------
    EVENT_TRACE_LOGFILEW logfile{};
    logfile.LoggerName      = sessionName_.data();
    logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logfile.Context         = this;
    logfile.EventRecordCallback = [](PEVENT_RECORD evt) {
        auto* self = static_cast<EtwMonitor*>(evt->UserContext);
        if (self) self->DispatchEvent(evt);
    };

    TRACEHANDLE th = OpenTraceW(&logfile);
    if (th == reinterpret_cast<TRACEHANDLE>(INVALID_HANDLE_VALUE)) {
        ULONG err = ::GetLastError();
        std::ostringstream oss;
        oss << "OpenTraceW failed: " << err;
        lastError_ = oss.str();
        Logger::Warn("EtwMonitor::Start - " + lastError_);
        ControlTraceW(reinterpret_cast<TRACEHANDLE>(sessionHandle_), nullptr,
                      props, EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = nullptr;
        return false;
    }
    traceHandle_ = reinterpret_cast<void*>(th);

    // ---------- Launch background thread ----------
    running_.store(true, std::memory_order_release);
    traceThread_.reset(new tcmt::compat::JThread([this](tcmt::compat::StopToken /*st*/) {
        TRACEHANDLE thLocal = reinterpret_cast<TRACEHANDLE>(this->traceHandle_);
        ULONG result = ProcessTrace(&thLocal, 1, nullptr, nullptr);
        if (result != ERROR_SUCCESS && result != ERROR_CANCELLED) {
            Logger::Warn("EtwMonitor: ProcessTrace returned " + std::to_string(result));
        }
        this->running_.store(false, std::memory_order_release);
    }));

    Logger::Info("EtwMonitor: session started");
    return true;
}

// ====================================================================
// Stop
// ====================================================================
void EtwMonitor::Stop() {
    if (!running_.load(std::memory_order_acquire)) return;

    if (sessionHandle_) {
        EVENT_TRACE_PROPERTIES stopProps{};
        stopProps.Wnode.BufferSize = sizeof(EVENT_TRACE_PROPERTIES);
        stopProps.LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        ControlTraceW(reinterpret_cast<TRACEHANDLE>(sessionHandle_),
                      sessionName_.c_str(), &stopProps, EVENT_TRACE_CONTROL_STOP);
        sessionHandle_ = nullptr;
    }

    if (traceHandle_) {
        CloseTrace(reinterpret_cast<TRACEHANDLE>(traceHandle_));
        traceHandle_ = nullptr;
    }

    // JThread destructor auto-joins; delete the unique_ptr to trigger it
    traceThread_.reset();

    Logger::Debug("EtwMonitor: session stopped");
}

// ====================================================================
// DispatchEvent — route by ProviderId
// ====================================================================
void EtwMonitor::DispatchEvent(const void* pEvent) {
    const auto* evt = static_cast<const EVENT_RECORD*>(pEvent);
    if (!evt) return;

    const GUID& pid = evt->EventHeader.ProviderId;

    try {
        if (IsEqualGUID(pid, kKernelPower)) {
            unsigned short id = evt->EventHeader.EventDescriptor.Id;
            if (id == 105 || id == 107) {  // PowerSourceChange
                if (powerCb_) powerCb_(id == 107);
            } else if (id == 106 && batteryCb_) {
                batteryCb_(-1); // placeholder
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
        Logger::Error(std::string("EtwMonitor::DispatchEvent: ") + e.what());
    } catch (...) {
        Logger::Error("EtwMonitor::DispatchEvent: unknown exception");
    }
}

unsigned short EtwMonitor::GetEventId(const void* pEvent) {
    return static_cast<const EVENT_RECORD*>(pEvent)->EventHeader.EventDescriptor.Id;
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
