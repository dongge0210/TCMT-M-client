#include "NetworkAdapter.h"
#include "../Utils/Logger.h"
#include "../Utils/WinUtils.h"

#ifdef TCMT_WINDOWS
#include "WmiManager.h"
// ======================== Windows Implementation ========================
// NOTE: winsock2.h must be included BEFORE windows.h
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <comutil.h>
#include <wbemcli.h>
#include <wbemprov.h>
#include <sstream>
#include <unknwn.h>  // IUnknown
#include <iomanip>
#include <algorithm>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#include <netioapi.h>
#include <map>
#include <chrono>
#include <tuple>

// Static cache for network throughput delta calculation (key: normalized MAC address)
// Stores {prev_rx_bytes, prev_tx_bytes, timestamp_ms}
static std::map<std::string, std::tuple<uint64_t, uint64_t, uint64_t>> g_throughputCache;

static void UpdateThroughput(std::vector<NetworkAdapter::AdapterInfo>& adapters) {
    PMIB_IF_TABLE2 table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR) return;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    for (size_t idx = 0; idx < adapters.size(); idx++) {
        const auto& adapter = adapters[idx];
        if (adapter.mac.empty() || adapter.mac == "00:00:00:00:00:00") continue;

        // Normalize MAC: remove separators, lowercase
        std::string macKey;
        for (char c : adapter.mac) {
            if (c != ':' && c != '-') macKey += static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
        if (macKey.length() != 12) continue;

        // Find matching interface by physical address
        for (UINT i = 0; i < table->NumEntries; i++) {
            auto& row = table->Table[i];
            if (row.PhysicalAddressLength == 0) continue;

            // Convert row physical address to normalized string
            std::string rowMac;
            for (UINT b = 0; b < row.PhysicalAddressLength; b++) {
                char hex[3];
                snprintf(hex, sizeof(hex), "%02x", row.PhysicalAddress[b]);
                rowMac += hex;
            }
            if (rowMac != macKey) continue;

            uint64_t rx = row.InOctets;
            uint64_t tx = row.OutOctets;

            auto it = g_throughputCache.find(macKey);
            if (it != g_throughputCache.end()) {
                uint64_t prevRx = std::get<0>(it->second);
                uint64_t prevTx = std::get<1>(it->second);
                uint64_t prevTime = std::get<2>(it->second);
                uint64_t dt = now - prevTime;
                if (dt > 0) {
                    adapters[idx].downloadSpeed = (rx - prevRx) * 1000 / dt; // bytes/sec
                    adapters[idx].uploadSpeed = (tx - prevTx) * 1000 / dt;
                }
            }
            g_throughputCache[macKey] = {rx, tx, now};
            break;
        }
    }
    FreeMibTable(table);
}

NetworkAdapter::NetworkAdapter(WmiManager& manager)
    : wmiManager(manager), initialized(false) {
    Initialize();
}

NetworkAdapter::~NetworkAdapter() { Cleanup(); }

void NetworkAdapter::Initialize() {
    if (wmiManager.IsInitialized()) {
        QueryAdapterInfo();
        initialized = true;
    } else {
        Logger::Error("WMI not initialized, cannot get network info");
    }
}

void NetworkAdapter::Cleanup() {
    adapters.clear();
    initialized = false;
}

void NetworkAdapter::Refresh() {
    Cleanup();
    Initialize();
}

void NetworkAdapter::QueryAdapterInfo() {
    QueryWmiAdapterInfo();
    UpdateAdapterAddresses();
    UpdateThroughput(this->adapters);
}

bool NetworkAdapter::IsVirtualAdapter(const std::wstring& name) const {
    const std::wstring virtualKeywords[] = {
        L"VirtualBox", L"Hyper-V", L"Virtual", L"VPN",
        L"Bluetooth", L"VMware", L"Loopback", L"Microsoft Wi-Fi Direct"
    };
    for (const auto& kw : virtualKeywords)
        if (name.find(kw) != std::wstring::npos) return true;
    return false;
}

void NetworkAdapter::QueryWmiAdapterInfo() {
    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hres = wmiManager.GetWmiService()->ExecQuery(
        bstr_t("WQL"),
        bstr_t("SELECT * FROM Win32_NetworkAdapter WHERE PhysicalAdapter = True"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator);

    if (FAILED(hres)) return;

    ULONG uReturn = 0;
    IWbemClassObject* pclsObj = nullptr;
    while (pEnumerator && pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn) == S_OK) {
        AdapterInfo info;
        info.isEnabled = false;
        VARIANT vtName, vtDesc, vtStatus;
        VariantInit(&vtName); VariantInit(&vtDesc); VariantInit(&vtStatus);

        if (SUCCEEDED(pclsObj->Get(L"Name", 0, &vtName, 0, 0)) && vtName.vt == VT_BSTR)
            info.name = WinUtils::WstringToString(vtName.bstrVal);
        if (SUCCEEDED(pclsObj->Get(L"Description", 0, &vtDesc, 0, 0)) && vtDesc.vt == VT_BSTR)
            info.description = WinUtils::WstringToString(vtDesc.bstrVal);
        if (SUCCEEDED(pclsObj->Get(L"NetEnabled", 0, &vtStatus, 0, 0)))
            info.isEnabled = (vtStatus.boolVal == VARIANT_TRUE);

        VariantClear(&vtName); VariantClear(&vtDesc); VariantClear(&vtStatus);

        std::wstring wname = WinUtils::Utf8ToWstring(info.name);
        if (IsVirtualAdapter(wname) || IsVirtualAdapter(WinUtils::Utf8ToWstring(info.description))) {
            pclsObj->Release(); continue;
        }

        VARIANT vtMac; VariantInit(&vtMac);
        if (SUCCEEDED(pclsObj->Get(L"MACAddress", 0, &vtMac, 0, 0)) && vtMac.vt == VT_BSTR)
            info.mac = WinUtils::WstringToString(vtMac.bstrVal);
        VariantClear(&vtMac);

        info.adapterType = "Unknown";
        if (!info.name.empty() && !info.mac.empty())
            adapters.push_back(info);

        pclsObj->Release();
    }
    pEnumerator->Release();
}

std::string NetworkAdapter::FormatMacAddress(const unsigned char* address, size_t length) const {
    std::stringstream ss;
    for (size_t i = 0; i < length; ++i) {
        if (i > 0) ss << ":";
        ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(address[i]);
    }
    return ss.str();
}

void NetworkAdapter::UpdateAdapterAddresses() {
    ULONG bufferSize = 15000;
    std::vector<BYTE> buffer(bufferSize);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(&buffer[0]);

    DWORD result = GetAdaptersAddresses(AF_INET,
        GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS, nullptr, pAddresses, &bufferSize);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufferSize);
        pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(&buffer[0]);
        result = GetAdaptersAddresses(AF_INET,
            GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_INCLUDE_GATEWAYS, nullptr, pAddresses, &bufferSize);
    }

    if (result != NO_ERROR) {
        Logger::Error("Failed to get network adapter addresses: " + std::to_string(result));
        return;
    }

    for (PIP_ADAPTER_ADDRESSES adapter = pAddresses; adapter; adapter = adapter->Next) {
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD &&
            adapter->IfType != IF_TYPE_IEEE80211) continue;

        std::string mac = FormatMacAddress(adapter->PhysicalAddress, adapter->PhysicalAddressLength);
        for (auto& ai : adapters) {
            if (ai.mac == mac) {
                ai.isConnected = (adapter->OperStatus == IfOperStatusUp);
                if (ai.isConnected) {
                    ai.speed = adapter->TransmitLinkSpeed;
                    ai.speedString = FormatSpeed(adapter->TransmitLinkSpeed);
                } else {
                    ai.speed = 0;
                    ai.speedString = "Disconnected";
                }
                ai.adapterType = DetermineAdapterType(
                    WinUtils::Utf8ToWstring(ai.name),
                    WinUtils::Utf8ToWstring(ai.description),
                    adapter->IfType);

                if (ai.isConnected) {
                    PIP_ADAPTER_UNICAST_ADDRESS address = adapter->FirstUnicastAddress;
                    while (address) {
                        if (address->Address.lpSockaddr->sa_family == AF_INET) {
                            char ipStr[INET_ADDRSTRLEN];
                            sockaddr_in* ipv4 = reinterpret_cast<sockaddr_in*>(address->Address.lpSockaddr);
                            inet_ntop(AF_INET, &(ipv4->sin_addr), ipStr, INET_ADDRSTRLEN);
                            ai.ip = std::string(ipStr);
                            break;
                        }
                        address = address->Next;
                    }
                } else {
                    ai.ip = "Disconnected";
                }
                break;
            }
        }
    }
}

std::string NetworkAdapter::FormatSpeed(uint64_t bitsPerSecond) const {
    const double GB = 1000000000.0, MB = 1000000.0, KB = 1000.0;
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (bitsPerSecond >= GB) ss << (bitsPerSecond / GB) << " Gbps";
    else if (bitsPerSecond >= MB) ss << (bitsPerSecond / MB) << " Mbps";
    else if (bitsPerSecond >= KB) ss << (bitsPerSecond / KB) << " Kbps";
    else ss << bitsPerSecond << " bps";
    return ss.str();
}

std::string NetworkAdapter::DetermineAdapterType(const std::wstring& name, const std::wstring& description, DWORD ifType) const {
    if (ifType == IF_TYPE_IEEE80211) return "Wireless adapter";
    else if (ifType == IF_TYPE_ETHERNET_CSMACD) return "Ethernet adapter";
    std::wstring combined = name + L" " + description;
    std::wstring lower = combined;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const std::wstring wireless[] = {L"wi-fi", L"wifi", L"wireless", L"802.11", L"wlan"};
    const std::wstring ethernet[] = {L"ethernet", L"gigabit", L"fast ethernet", L"lan"};
    for (auto& kw : wireless) if (lower.find(kw) != std::wstring::npos) return "Wireless adapter";
    for (auto& kw : ethernet) if (lower.find(kw) != std::wstring::npos) return "Ethernet adapter";
    return "Unknown type";
}

void NetworkAdapter::SafeRelease(IUnknown* pInterface) {
    if (pInterface) pInterface->Release();
}

const std::vector<NetworkAdapter::AdapterInfo>& NetworkAdapter::GetAdapters() const { return adapters; }

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
// Forward declarations for macOS
static void UpdateThroughput(std::vector<NetworkAdapter::AdapterInfo>& adapters);
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <tuple>

static std::string FormatMacAddr(const unsigned char* addr, size_t len) {
    std::stringstream ss;
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) ss << ":";
        ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(addr[i]);
    }
    return ss.str();
}

static std::string FormatSpd(uint64_t bitsPerSecond) {
    const double GB = 1000000000.0, MB = 1000000.0, KB = 1000.0;
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (bitsPerSecond >= GB) ss << (bitsPerSecond / GB) << " Gbps";
    else if (bitsPerSecond >= MB) ss << (bitsPerSecond / MB) << " Mbps";
    else if (bitsPerSecond >= KB) ss << (bitsPerSecond / KB) << " Kbps";
    else ss << bitsPerSecond << " bps";
    return ss.str();
}

NetworkAdapter::NetworkAdapter() : initialized(false) { Initialize(); }
NetworkAdapter::~NetworkAdapter() { Cleanup(); }

void NetworkAdapter::Initialize() {
    QueryAdapterInfo();
    initialized = true;
}

void NetworkAdapter::Cleanup() {
    adapters.clear();
    initialized = false;
}

void NetworkAdapter::Refresh() {
    Cleanup();
    Initialize();
}

bool NetworkAdapter::IsVirtualAdapter(const std::string& name) const {
    const std::string virtualKeywords[] = {
        "lo", "utun", "ap", "bridge", "awdl", "llw",
        "Virtual", "VPN", "Loopback", "docker"
    };
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& kw : virtualKeywords)
        if (lower.find(kw) != std::string::npos) return true;
    return false;
}

std::string NetworkAdapter::FormatMacAddress(const unsigned char* addr, size_t len) const {
    return FormatMacAddr(addr, len);
}

std::string NetworkAdapter::FormatSpeed(uint64_t bitsPerSecond) const {
    return FormatSpd(bitsPerSecond);
}

void NetworkAdapter::QueryAdapterInfo() {
    // Map BSD name -> AdapterInfo (for MAC addresses)
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        Logger::Error("NetworkAdapter: getifaddrs failed");
        return;
    }

    std::map<std::string, AdapterInfo> bsdMap;

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;

        std::string bsdName(ifa->ifa_name);
        if (IsVirtualAdapter(bsdName)) continue;

        struct ifaddrs* ifa2 = ifa;
        // MAC address
        struct sockaddr_dl* sdl = reinterpret_cast<struct sockaddr_dl*>(ifa2->ifa_addr);
        if (sdl->sdl_alen != 6) continue;
        unsigned char* mac = reinterpret_cast<unsigned char*>(LLADDR(sdl));

        AdapterInfo info;
        info.name = bsdName;
        info.mac = FormatMacAddr(mac, 6);
        info.isEnabled = true;
        info.isConnected = (ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING);
        info.speed = 0;
        info.speedString = "Unknown";
        info.adapterType = "Ethernet adapter"; // default; will update below

        // Try to determine wireless vs wired
        if (bsdName == "en0" || bsdName.find("wl") != std::string::npos
            || bsdName.find("wifi") != std::string::npos)
            info.adapterType = "Wireless adapter";
        else if (bsdName.find("awdl") != std::string::npos || bsdName.find("llw") != std::string::npos)
            info.adapterType = "Wireless adapter";
        else
            info.adapterType = "Ethernet adapter";

        bsdMap[bsdName] = info;
    }

    // Now get IP addresses
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        std::string bsdName(ifa->ifa_name);
        auto it = bsdMap.find(bsdName);
        if (it != bsdMap.end()) {
            char ipStr[INET_ADDRSTRLEN];
            struct sockaddr_in* ipv4 = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
            inet_ntop(AF_INET, &(ipv4->sin_addr), ipStr, INET_ADDRSTRLEN);
            it->second.ip = std::string(ipStr);
            it->second.isConnected = (ifa->ifa_flags & IFF_RUNNING) != 0;
        }
    }

    // Try to get speed via if_data (BSD style)
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
        std::string bsdName(ifa->ifa_name);
        auto it = bsdMap.find(bsdName);
        if (it != bsdMap.end() && ifa->ifa_data) {
            struct if_data* ifData = reinterpret_cast<struct if_data*>(ifa->ifa_data);
            // ifData->ifi_baudrate may be 0 on some systems, or garbage (e.g. 679 PB/s on Apple Silicon Wi-Fi)
            uint64_t baudrate = ifData->ifi_baudrate;
            // Sanity check: ignore obviously wrong values (> 100 Gbps for typical adapters)
            if (baudrate > 0 && baudrate <= 100ULL * 1000ULL * 1000ULL * 1000ULL) {
                it->second.speed = baudrate;
                it->second.speedString = FormatSpd(baudrate);
            }
        }
    }

    freeifaddrs(ifaddr);

    // Fallback: get Wi-Fi link speed via CoreWLAN (ifi_baudrate is garbage on macOS)
    // Uses a quick swift one-liner to read CWInterface.transmitRate
    // Note: swift takes ~2s to JIT on first call; cached for subsequent calls
    static int64_t cachedWifiSpeed = -1; // -1 = not yet queried
    if (cachedWifiSpeed < 0) {
        Logger::Debug("NetworkAdapter: querying CoreWLAN for Wi-Fi speed...");
        FILE* fp = popen(
            "swift -e 'import CoreWLAN; "
            "let c = CWWiFiClient.shared().interface(); "
            "if let r = c?.transmitRate() { print(Int64(r * 1_000_000)) } "
            "else { print(\"0\") }' 2>/dev/null",
            "r");
        if (fp) {
            char buf[64] = {0};
            if (fgets(buf, sizeof(buf), fp)) {
                cachedWifiSpeed = std::atoll(buf);
            } else {
                cachedWifiSpeed = 0;
            }
            pclose(fp);
        } else {
            cachedWifiSpeed = 0;
        }
    }
    if (cachedWifiSpeed > 0) {
        // Apply to first wireless adapter that has speed == 0
        for (auto& kv : bsdMap) {
            bool isWireless = (kv.second.adapterType.find("\xe6\x97\xa0\xe7\xba\xbf") != std::string::npos
                         || kv.second.adapterType.find("wireless") != std::string::npos
                         || kv.second.adapterType.find("Wi-Fi") != std::string::npos);
            if (isWireless && kv.second.speed == 0) {
                kv.second.speed = static_cast<uint64_t>(cachedWifiSpeed);
                kv.second.speedString = FormatSpd(static_cast<uint64_t>(cachedWifiSpeed));
                Logger::Debug("NetworkAdapter: Wi-Fi speed "
                            + std::to_string(cachedWifiSpeed / 1'000'000) + " Mbps via CoreWLAN");
                break;
            }
        }
    }

    // Move to adapters vector
    for (auto& kv : bsdMap) {
        adapters.push_back(std::move(kv.second));
    }

    Logger::Debug("NetworkAdapter: found " + std::to_string(adapters.size()) + " adapters");
    UpdateThroughput(this->adapters);
}

// macOS throughput delta static cache
static std::map<std::string, std::tuple<uint64_t, uint64_t, uint64_t>> g_throughputCacheMac;

static void UpdateThroughput(std::vector<NetworkAdapter::AdapterInfo>& adapters) {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) return;

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    for (auto& adapter : adapters) {
        if (adapter.name.empty()) continue;

        // Find matching interface by name for AF_LINK to get byte counters
        for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_LINK) continue;
            if (adapter.name != ifa->ifa_name) continue;
            if (!ifa->ifa_data) continue;

            struct if_data* ifData = reinterpret_cast<struct if_data*>(ifa->ifa_data);
            uint64_t rx = ifData->ifi_ibytes;
            uint64_t tx = ifData->ifi_obytes;

            auto it = g_throughputCacheMac.find(adapter.name);
            if (it != g_throughputCacheMac.end()) {
                uint64_t prevRx = std::get<0>(it->second);
                uint64_t prevTx = std::get<1>(it->second);
                uint64_t prevTime = std::get<2>(it->second);
                uint64_t dt = now - prevTime;
                if (dt > 0) {
                    adapter.downloadSpeed = (rx - prevRx) * 1000 / dt; // bytes/sec
                    adapter.uploadSpeed = (tx - prevTx) * 1000 / dt;
                }
            }
            g_throughputCacheMac[adapter.name] = {rx, tx, now};
            break;
        }
    }
    freeifaddrs(ifaddr);
}

void NetworkAdapter::UpdateAdapterAddresses() {
    // Already done in QueryAdapterInfo for macOS
}

const std::vector<NetworkAdapter::AdapterInfo>& NetworkAdapter::GetAdapters() const { return adapters; }

#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/if_link.h>
#include <sys/ioctl.h>
#include <linux/sockios.h>
#include <cstring>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <map>
#include <chrono>
#include <tuple>

static std::string FormatMacAddr(const unsigned char* addr, size_t len) {
    std::stringstream ss;
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) ss << ":";
        ss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(addr[i]);
    }
    return ss.str();
}

static std::string FormatSpd(uint64_t bitsPerSecond) {
    const double GB = 1000000000.0, MB = 1000000.0, KB = 1000.0;
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (bitsPerSecond >= GB) ss << (bitsPerSecond / GB) << " Gbps";
    else if (bitsPerSecond >= MB) ss << (bitsPerSecond / MB) << " Mbps";
    else if (bitsPerSecond >= KB) ss << (bitsPerSecond / KB) << " Kbps";
    else ss << bitsPerSecond << " bps";
    return ss.str();
}

NetworkAdapter::NetworkAdapter() : initialized(false) { Initialize(); }
NetworkAdapter::~NetworkAdapter() { Cleanup(); }

void NetworkAdapter::Initialize() {
    QueryAdapterInfo();
    initialized = true;
}

void NetworkAdapter::Cleanup() {
    adapters.clear();
    initialized = false;
}

void NetworkAdapter::Refresh() {
    Cleanup();
    Initialize();
}

bool NetworkAdapter::IsVirtualAdapter(const std::string& name) const {
    const std::string virtualKeywords[] = {
        "lo", "docker", "veth", "br-", "virbr", "tun", "tap",
        "Virtual", "VPN", "Loopback"
    };
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (const auto& kw : virtualKeywords)
        if (lower.find(kw) != std::string::npos) return true;
    return false;
}

std::string NetworkAdapter::FormatMacAddress(const unsigned char* addr, size_t len) const {
    return FormatMacAddr(addr, len);
}

std::string NetworkAdapter::FormatSpeed(uint64_t bitsPerSecond) const {
    return FormatSpd(bitsPerSecond);
}

static std::string ReadSysFsString(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return "";
    std::string val;
    std::getline(ifs, val);
    val.erase(0, val.find_first_not_of(" \t\n\r"));
    val.erase(val.find_last_not_of(" \t\n\r") + 1);
    return val;
}

void NetworkAdapter::QueryAdapterInfo() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        Logger::Error("NetworkAdapter: getifaddrs failed");
        return;
    }

    std::map<std::string, AdapterInfo> nameMap;

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;
        std::string ifname(ifa->ifa_name);
        if (IsVirtualAdapter(ifname)) continue;

        auto it = nameMap.find(ifname);
        if (it == nameMap.end()) {
            AdapterInfo info;
            info.name = ifname;
            info.isEnabled = (ifa->ifa_flags & IFF_UP) != 0;
            info.isConnected = (ifa->ifa_flags & IFF_RUNNING) != 0;
            info.speed = 0;
            info.speedString = "Unknown";

            if (ifname.find("wl") == 0 || ifname.find("wlan") == 0)
                info.adapterType = "Wireless adapter";
            else if (ifname.find("en") == 0 || ifname.find("eth") == 0)
                info.adapterType = "Ethernet adapter";
            else
                info.adapterType = "Unknown";

            nameMap[ifname] = info;
            it = nameMap.find(ifname);
        }

        if (ifa->ifa_addr) {
            if (ifa->ifa_addr->sa_family == AF_INET) {
                char ipStr[INET_ADDRSTRLEN];
                struct sockaddr_in* ipv4 = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
                inet_ntop(AF_INET, &(ipv4->sin_addr), ipStr, INET_ADDRSTRLEN);
                it->second.ip = std::string(ipStr);
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                char ipStr[INET6_ADDRSTRLEN];
                struct sockaddr_in6* ipv6 = reinterpret_cast<struct sockaddr_in6*>(ifa->ifa_addr);
                inet_ntop(AF_INET6, &(ipv6->sin6_addr), ipStr, INET6_ADDRSTRLEN);
                if (it->second.ip.empty())
                    it->second.ip = std::string(ipStr);
            }
        }

        it->second.isEnabled = (ifa->ifa_flags & IFF_UP) != 0;
        it->second.isConnected = (ifa->ifa_flags & IFF_RUNNING) != 0;
    }

    freeifaddrs(ifaddr);

    for (auto& kv : nameMap) {
        const std::string& ifname = kv.first;

        std::string mac = ReadSysFsString("/sys/class/net/" + ifname + "/address");
        if (!mac.empty()) {
            std::transform(mac.begin(), mac.end(), mac.begin(), ::toupper);
            kv.second.mac = mac;
        }

        std::string speedStr = ReadSysFsString("/sys/class/net/" + ifname + "/speed");
        if (!speedStr.empty() && speedStr != "-1") {
            try {
                uint64_t speedMbps = std::stoull(speedStr);
                kv.second.speed = speedMbps * 1000000ULL;
                kv.second.speedString = FormatSpd(kv.second.speed);
            } catch (...) {
                kv.second.speed = 0;
                kv.second.speedString = "Unknown";
            }
        }

        std::string operState = ReadSysFsString("/sys/class/net/" + ifname + "/operstate");
        kv.second.isConnected = (operState == "up");
    }

    for (auto& kv : nameMap)
        adapters.push_back(std::move(kv.second));

    Logger::Debug("NetworkAdapter: found " + std::to_string(adapters.size()) + " adapters on Linux");

    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();

    for (auto& adapter : adapters) {
        const std::string& ifname = adapter.name;
        std::string rxStr = ReadSysFsString("/sys/class/net/" + ifname + "/statistics/rx_bytes");
        std::string txStr = ReadSysFsString("/sys/class/net/" + ifname + "/statistics/tx_bytes");
        if (rxStr.empty() || txStr.empty()) continue;

        uint64_t rxBytes = 0, txBytes = 0;
        try {
            rxBytes = std::stoull(rxStr);
            txBytes = std::stoull(txStr);
        } catch (...) {
            continue;
        }

        auto rxIt = prevRxBytes.find(ifname);
        auto txIt = prevTxBytes.find(ifname);
        if (rxIt != prevRxBytes.end() && txIt != prevTxBytes.end() && prevSampleTimeMs > 0) {
            uint64_t dt = now - prevSampleTimeMs;
            if (dt > 0) {
                uint64_t rxDelta = (rxBytes >= rxIt->second) ? (rxBytes - rxIt->second) : 0;
                uint64_t txDelta = (txBytes >= txIt->second) ? (txBytes - txIt->second) : 0;
                adapter.downloadSpeed = rxDelta * 1000 / dt;
                adapter.uploadSpeed = txDelta * 1000 / dt;
            }
        }

        prevRxBytes[ifname] = rxBytes;
        prevTxBytes[ifname] = txBytes;
    }
    prevSampleTimeMs = now;
}

void NetworkAdapter::UpdateAdapterAddresses() {
}

const std::vector<NetworkAdapter::AdapterInfo>& NetworkAdapter::GetAdapters() const { return adapters; }

#else
#error "Unsupported platform"
#endif
