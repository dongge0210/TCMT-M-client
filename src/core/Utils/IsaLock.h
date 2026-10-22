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
        if (!handle_) return;
        const DWORD r = WaitForSingleObject(handle_, 2000);
        // WAIT_ABANDONED still grants ownership: the previous owner died while
        // holding it, and the kernel hands the mutex to us. Treating it as a
        // failure would leave it owned and never released.
        held_ = (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED);
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
            // MUTEX_MODIFY_STATE is required for ReleaseMutex; SYNCHRONIZE
            // alone would hand out a handle this process could never unlock.
            HANDLE m = OpenMutexW(MUTEX_MODIFY_STATE | SYNCHRONIZE, FALSE,
                                  L"Global\\Access_ISABUS.HTP.Method");
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
