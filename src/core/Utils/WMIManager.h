#pragma once

// NOTE: winsock2.h must be included BEFORE windows.h and wbemidl.h
#include <winsock2.h>
#include <windows.h>

#include <wbemidl.h>
#include <comdef.h>
#include <servprov.h>
#include <string>

class WmiManager : public IServiceProvider {
public:
    WmiManager();
    ~WmiManager();
    bool IsInitialized() const;

    // IServiceProvider interface implementation
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE QueryService(REFGUID guid, REFIID riid, void** ppv) override;

    IWbemServices* GetWmiService() const;

private:
    void Initialize();
    void Cleanup();

    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    bool initialized = false;
    LONG m_refCount = 1;
};