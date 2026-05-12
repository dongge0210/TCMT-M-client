#ifdef TCMT_WINDOWS
#include <Objbase.h>
bool InitializeCom() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return SUCCEEDED(hr);
}
void UninitializeCom() {
    CoUninitialize();
}
#else
// macOS / Linux: no COM needed
bool InitializeCom() { return true; }
void UninitializeCom() {}
#endif
