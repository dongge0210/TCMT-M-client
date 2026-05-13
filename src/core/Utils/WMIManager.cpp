#include "WmiManager.h"
#include "Logger.h"
#include <comdef.h>

WmiManager::WmiManager() : initialized(false), pLoc(nullptr), pSvc(nullptr) {
    Initialize();
}

WmiManager::~WmiManager() {
    Cleanup();
}

void WmiManager::Initialize() {
    // COM is already initialized by main.cpp, only need to verify security here
    HRESULT hres = CoInitializeSecurity(
        NULL,
        -1,
        NULL,
        NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,
        EOAC_NONE,
        NULL
    );

    // Allow case where security initialization is already complete
    if (FAILED(hres) && hres != RPC_E_TOO_LATE) {
        Logger::Error("Security initialization failed: 0x" + std::to_string(hres));
        return;
    }

    // Create WMI locator
    hres = CoCreateInstance(
        CLSID_WbemLocator,
        0,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        (LPVOID*)&pLoc
    );

    if (FAILED(hres)) {
        Logger::Error("Failed to create WMI locator: 0x" + std::to_string(hres));
        return;
    }

    // Connect to WMI service
    hres = pLoc->ConnectServer(
        _bstr_t(L"ROOT\\CIMV2"),
        NULL,
        NULL,
        0,
        NULL,
        0,
        0,
        &pSvc
    );

    if (FAILED(hres)) {
        Logger::Error("Failed to connect to WMI namespace: 0x" + std::to_string(hres));
        Cleanup();
        return;
    }

    // Set proxy security level
    hres = CoSetProxyBlanket(
        pSvc,
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        NULL,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL,
        EOAC_NONE
    );

    if (FAILED(hres)) {
        Logger::Error("Failed to set proxy security: 0x" + std::to_string(hres));
        Cleanup();
        return;
    }

    initialized = true;
}

void WmiManager::Cleanup() {
    if (pSvc) {
        pSvc->Release();
        pSvc = nullptr;
    }
    if (pLoc) {
        pLoc->Release();
        pLoc = nullptr;
    }
    initialized = false;
}

bool WmiManager::IsInitialized() const {
    return initialized;
}

IWbemServices* WmiManager::GetWmiService() const {
    if (!initialized) {
        Logger::Error("WMI manager not initialized when attempting to get WMI service");
        return nullptr;
    }
    return pSvc;
}

HRESULT STDMETHODCALLTYPE WmiManager::QueryInterface(REFIID riid, void** ppvObject) {
    if (riid == IID_IUnknown || riid == IID_IServiceProvider) {
        *ppvObject = static_cast<IServiceProvider*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE WmiManager::AddRef() {
    return InterlockedIncrement(&m_refCount);
}

ULONG STDMETHODCALLTYPE WmiManager::Release() {
    ULONG refCount = InterlockedDecrement(&m_refCount);
    if (refCount == 0) delete this;
    return refCount;
}

HRESULT STDMETHODCALLTYPE WmiManager::QueryService(REFGUID guidService, REFIID riid, void** ppvObject) {
    if (guidService == IID_IWbemServices) {
        if (!pSvc) {
            return E_FAIL;
        }
        return pSvc->QueryInterface(riid, ppvObject);
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}