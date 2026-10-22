#pragma once

#ifdef _WIN32
#include <windows.h>

// Serializes access to the ISA/Super I/O config ports with every other
// monitoring agent on the machine. OpenHardwareMonitor established the mutex
// name and HWiNFO, LibreHardwareMonitor and nwinfo all take it before touching
// 0x2E/0x4E; two agents walking the shared index port at once is what makes
// sensor reads come back as 0xFF.
namespace tcmt {

class IsaLock {
public:
    IsaLock() {
        handle_ = Mutex();
        held_ = handle_ && WaitForSingleObject(handle_, 2000) == WAIT_OBJECT_0;
    }
    ~IsaLock() {
        if (held_) ReleaseMutex(handle_);
    }
    IsaLock(const IsaLock&) = delete;
    IsaLock& operator=(const IsaLock&) = delete;

    bool held() const { return held_; }

private:
    static HANDLE Mutex() {
        static HANDLE h = [] {
            HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\Access_ISABUS.HTP.Method");
            if (!m) m = CreateMutexW(nullptr, FALSE, L"Global\\Access_ISABUS.HTP.Method");
            return m;
        }();
        return h;
    }

    HANDLE handle_ = nullptr;
    bool held_ = false;
};

}  // namespace tcmt
#endif
