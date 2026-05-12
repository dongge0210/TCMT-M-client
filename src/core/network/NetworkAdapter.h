#pragma once

#include <string>
#include <vector>
#include <cstdint>

#ifdef TCMT_WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <unknwn.h>  // IUnknown
#else
// Define DWORD for non-Windows platforms
typedef uint32_t DWORD;
#endif

#ifdef TCMT_MACOS
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <map>
#endif

class WmiManager;

class NetworkAdapter {
public:
    struct AdapterInfo {
        std::string name;
        std::string mac;
        std::string ip;
        std::string description;
        std::string adapterType; // Wireless/Ethernet
        bool isEnabled;
        bool isConnected;
        uint64_t speed;
        uint64_t downloadSpeed = 0;
        uint64_t uploadSpeed = 0;
        std::string speedString;
    };

#ifdef TCMT_WINDOWS
    explicit NetworkAdapter(WmiManager& manager);
#elif defined(TCMT_MACOS)
    NetworkAdapter();
#endif
    ~NetworkAdapter();

    const std::vector<AdapterInfo>& GetAdapters() const;
    void Refresh();

private:
    void Initialize();
    void Cleanup();
    void QueryAdapterInfo();
    void UpdateAdapterAddresses();
    std::string FormatMacAddress(const unsigned char* address, size_t length) const;
    std::string FormatSpeed(uint64_t bitsPerSecond) const;
    bool IsVirtualAdapter(const std::string& name) const;
    bool IsVirtualAdapter(const std::wstring& name) const;

#ifdef TCMT_WINDOWS
    std::string DetermineAdapterType(const std::wstring& name, const std::wstring& description, DWORD ifType) const;
    void QueryWmiAdapterInfo();
    void SafeRelease(IUnknown* pInterface);
    WmiManager& wmiManager;
#endif

    std::vector<AdapterInfo> adapters;
    bool initialized;
};
