#include "WiFiInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
// winsock2.h must precede windows.h
#include <winsock2.h>
#include <windows.h>
#include <wlanapi.h>
#include <wlantypes.h>
#include <cstdio>

#pragma comment(lib, "wlanapi.lib")

void WiFiInfo::Clear() {
    data_ = WiFiData{};
}

void WiFiInfo::Detect() {
    Clear();

    HANDLE hClient = nullptr;
    DWORD version = 0;
    DWORD dwResult = WlanOpenHandle(2, nullptr, &version, &hClient);
    if (dwResult != ERROR_SUCCESS) {
        Logger::Warn("WiFiInfo: WlanOpenHandle failed (0x" +
                     std::to_string(dwResult) + ")");
        return;
    }

    // Enumerate wireless interfaces
    PWLAN_INTERFACE_INFO_LIST pIfList = nullptr;
    dwResult = WlanEnumInterfaces(hClient, nullptr, &pIfList);
    if (dwResult != ERROR_SUCCESS || !pIfList || pIfList->dwNumberOfItems == 0) {
        if (pIfList) {
            WlanFreeMemory(pIfList);
        }
        WlanCloseHandle(hClient, nullptr);
        Logger::Info("WiFiInfo: No WLAN interfaces found");
        return;
    }

    // Use the first Wi-Fi adapter
    const PWLAN_INTERFACE_INFO pIfInfo = &pIfList->InterfaceInfo[0];
    const GUID& guid = pIfInfo->InterfaceGuid;

    // Determine if the radio is powered on
    data_.powerOn = (pIfInfo->isState != wlan_interface_state_not_ready);

    // --- Query current connection (SSID, BSSID, security, tx rate) ---
    PWLAN_CONNECTION_ATTRIBUTES pConn = nullptr;
    WLAN_OPCODE_VALUE_TYPE opCode = wlan_opcode_value_type_query_only;
    DWORD connSize = 0;

    dwResult = WlanQueryInterface(
        hClient, &guid,
        wlan_intf_opcode_current_connection,
        nullptr, &connSize,
        reinterpret_cast<PVOID*>(&pConn), &opCode);

    if (dwResult == ERROR_SUCCESS && pConn) {
        data_.isConnected =
            (pConn->isState == wlan_interface_state_connected);

        if (data_.isConnected) {
            // --- SSID ---
            const DOT11_SSID& dot11Ssid = pConn->wlanAssociationAttributes.dot11Ssid;
            if (dot11Ssid.uSSIDLength > 0 &&
                dot11Ssid.uSSIDLength <= DOT11_SSID_MAX_LENGTH) {
                data_.ssid.assign(
                    reinterpret_cast<char*>(dot11Ssid.ucSSID),
                    dot11Ssid.uSSIDLength);
            }

            // --- BSSID (MAC address of the access point) ---
            const auto& bssidBytes = pConn->wlanAssociationAttributes.dot11Bssid;
            char bssidBuf[18] = {};
            std::snprintf(bssidBuf, sizeof(bssidBuf),
                          "%02X:%02X:%02X:%02X:%02X:%02X",
                          bssidBytes[0], bssidBytes[1], bssidBytes[2],
                          bssidBytes[3], bssidBytes[4], bssidBytes[5]);
            data_.bssid = bssidBuf;

            // --- Transmit rate (units of 500 bps -> Mbps) ---
            data_.txRate =
                pConn->wlanAssociationAttributes.dot11TxRate / 2000.0;

            // --- Security / authentication algorithm ---
            switch (pConn->wlanSecurityAttributes.dot11AuthAlgorithm) {
            case DOT11_AUTH_ALGO_80211_OPEN:
                data_.security = "Open";
                break;
            case DOT11_AUTH_ALGO_80211_SHARED_KEY:
                data_.security = "WEP";
                break;
            case DOT11_AUTH_ALGO_WPA:
                data_.security = "WPA-Enterprise";
                break;
            case DOT11_AUTH_ALGO_WPA_PSK:
                data_.security = "WPA-Personal";
                break;
            case DOT11_AUTH_ALGO_WPA_NONE:
                data_.security = "WPA-None";
                break;
            case DOT11_AUTH_ALGO_RSNA:
                data_.security = "WPA2-Enterprise";
                break;
            case DOT11_AUTH_ALGO_RSNA_PSK:
                data_.security = "WPA2-Personal";
                break;
            case DOT11_AUTH_ALGO_WPA3:
            case DOT11_AUTH_ALGO_WPA3_SAE:
                data_.security = "WPA3-SAE";
                break;
            case DOT11_AUTH_ALGO_WPA3_ENT_192:
                data_.security = "WPA3-Enterprise-192";
                break;
            case DOT11_AUTH_ALGO_OWE:
                data_.security = "OWE";
                break;
            default: {
                char buf[32] = {};
                std::snprintf(buf, sizeof(buf), "Auth-0x%X",
                              pConn->wlanSecurityAttributes.dot11AuthAlgorithm);
                data_.security = buf;
                break;
            }
            }
        }
        WlanFreeMemory(pConn);
        pConn = nullptr;
    }

    // --- RSSI (signal strength in dBm) ---
    LONG rssi = 0;
    DWORD rssiSize = sizeof(rssi);
    dwResult = WlanQueryInterface(
        hClient, &guid,
        wlan_intf_opcode_rssi,
        nullptr, &rssiSize,
        reinterpret_cast<PVOID*>(&rssi), &opCode);
    if (dwResult == ERROR_SUCCESS) {
        data_.rssi = static_cast<int>(rssi);
    }

    // --- Channel number ---
    ULONG channel = 0;
    DWORD channelSize = sizeof(channel);
    dwResult = WlanQueryInterface(
        hClient, &guid,
        wlan_intf_opcode_channel_number,
        nullptr, &channelSize,
        reinterpret_cast<PVOID*>(&channel), &opCode);
    if (dwResult == ERROR_SUCCESS) {
        data_.channel = static_cast<int>(channel);
    }

    // Cleanup
    WlanFreeMemory(pIfList);
    WlanCloseHandle(hClient, nullptr);
}

const WiFiData& WiFiInfo::GetData() const { return data_; }

#else
// ==================== Stub for non-Windows platforms ====================
void WiFiInfo::Detect() {
    // No-op: WiFi monitoring is Windows-only via WLAN API.
}
const WiFiData& WiFiInfo::GetData() const { return data_; }
#endif // TCMT_WINDOWS
