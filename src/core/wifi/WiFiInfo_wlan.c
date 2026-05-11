// WiFiInfo_wlan.c — Windows Wi-Fi detection via Native Wi-Fi API (wlanapi.h)
// Pure C; compiled only on Windows.

#include "WiFiInfo_wlan.h"

#ifdef _WIN32

#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <wlanapi.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "wlanapi.lib")

// dot11TxRate was removed from WLAN_ASSOCIATION_ATTRIBUTES in newer SDKs.
// Provide a fallback so the code compiles across SDK versions.
#ifndef WLAN_ASSOC_ATTR_HAS_DOT11TXRATE
static ULONG GetTxRate(const PWLAN_CONNECTION_ATTRIBUTES pConn) {
    // The field exists in older SDKs but not in 10.0.26100+.
    // Use __if_exists for MSVC; GCC/Clang compilation on Windows is unlikely.
#ifdef _MSC_VER
    __if_exists(pConn->wlanAssociationAttributes.dot11TxRate) {
        return pConn->wlanAssociationAttributes.dot11TxRate;
    }
#endif
    return 0;
}
#else
static ULONG GetTxRate(const PWLAN_CONNECTION_ATTRIBUTES pConn) {
    return pConn->wlanAssociationAttributes.dot11TxRate;
}
#endif

bool WlanDetect(WlanData* out) {
    if (!out) return false;
    memset(out, 0, sizeof(WlanData));

    HANDLE hClient = NULL;
    DWORD version = 0;
    DWORD dwResult = WlanOpenHandle(2, NULL, &version, &hClient);
    if (dwResult != ERROR_SUCCESS) return false;

    PWLAN_INTERFACE_INFO_LIST pIfList = NULL;
    dwResult = WlanEnumInterfaces(hClient, NULL, &pIfList);
    if (dwResult != ERROR_SUCCESS || !pIfList || pIfList->dwNumberOfItems == 0) {
        if (pIfList) WlanFreeMemory(pIfList);
        WlanCloseHandle(hClient, NULL);
        return false;
    }

    const PWLAN_INTERFACE_INFO pIfInfo = &pIfList->InterfaceInfo[0];
    const GUID* pGuid = &pIfInfo->InterfaceGuid;

    out->powerOn = (pIfInfo->isState != wlan_interface_state_not_ready);

    // --- Current connection (SSID, BSSID, security, tx rate) ---
    PWLAN_CONNECTION_ATTRIBUTES pConn = NULL;
    WLAN_OPCODE_VALUE_TYPE opCode = wlan_opcode_value_type_query_only;
    DWORD connSize = 0;

    dwResult = WlanQueryInterface(
        hClient, pGuid,
        wlan_intf_opcode_current_connection,
        NULL, &connSize,
        (PVOID*)&pConn, &opCode);

    if (dwResult == ERROR_SUCCESS && pConn) {
        out->isConnected = (pConn->isState == wlan_interface_state_connected);

        if (out->isConnected) {
            // SSID
            const DOT11_SSID* pSsid = &pConn->wlanAssociationAttributes.dot11Ssid;
            if (pSsid->uSSIDLength > 0 && pSsid->uSSIDLength <= DOT11_SSID_MAX_LENGTH) {
                size_t len = pSsid->uSSIDLength < WLAN_SSID_MAX_LEN - 1
                             ? pSsid->uSSIDLength : WLAN_SSID_MAX_LEN - 1;
                memcpy(out->ssid, pSsid->ucSSID, len);
                out->ssid[len] = '\0';
            }

            // BSSID
            snprintf(out->bssid, WLAN_BSSID_MAX_LEN,
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     pConn->wlanAssociationAttributes.dot11Bssid[0],
                     pConn->wlanAssociationAttributes.dot11Bssid[1],
                     pConn->wlanAssociationAttributes.dot11Bssid[2],
                     pConn->wlanAssociationAttributes.dot11Bssid[3],
                     pConn->wlanAssociationAttributes.dot11Bssid[4],
                     pConn->wlanAssociationAttributes.dot11Bssid[5]);

            // Tx rate (500 bps units -> Mbps); field absent in newer SDKs
            out->txRate = GetTxRate(pConn) / 2000.0;

            // Security
            switch (pConn->wlanSecurityAttributes.dot11AuthAlgorithm) {
            case DOT11_AUTH_ALGO_80211_OPEN:       snprintf(out->security, WLAN_SECURITY_MAX_LEN, "Open"); break;
            case DOT11_AUTH_ALGO_80211_SHARED_KEY: snprintf(out->security, WLAN_SECURITY_MAX_LEN, "WEP"); break;
            case DOT11_AUTH_ALGO_WPA:              snprintf(out->security, WLAN_SECURITY_MAX_LEN, "WPA-Enterprise"); break;
            case DOT11_AUTH_ALGO_WPA_PSK:          snprintf(out->security, WLAN_SECURITY_MAX_LEN, "WPA-Personal"); break;
            case DOT11_AUTH_ALGO_WPA_NONE:         snprintf(out->security, WLAN_SECURITY_MAX_LEN, "WPA-None"); break;
            case DOT11_AUTH_ALGO_RSNA:             snprintf(out->security, WLAN_SECURITY_MAX_LEN, "WPA2-Enterprise"); break;
            case DOT11_AUTH_ALGO_RSNA_PSK:         snprintf(out->security, WLAN_SECURITY_MAX_LEN, "WPA2-Personal"); break;
            // DOT11_AUTH_ALGO_WPA3 and DOT11_AUTH_ALGO_WPA3_SAE may share the
            // same value in newer SDKs (see wlantypes.h); handle with one case.
#if DOT11_AUTH_ALGO_WPA3 != DOT11_AUTH_ALGO_WPA3_SAE
            case DOT11_AUTH_ALGO_WPA3:
#endif
            case DOT11_AUTH_ALGO_WPA3_SAE:         snprintf(out->security, WLAN_SECURITY_MAX_LEN, "WPA3-SAE"); break;
            case DOT11_AUTH_ALGO_WPA3_ENT_192:     snprintf(out->security, WLAN_SECURITY_MAX_LEN, "WPA3-Enterprise-192"); break;
            case DOT11_AUTH_ALGO_OWE:              snprintf(out->security, WLAN_SECURITY_MAX_LEN, "OWE"); break;
            default:
                snprintf(out->security, WLAN_SECURITY_MAX_LEN, "Auth-0x%X",
                         pConn->wlanSecurityAttributes.dot11AuthAlgorithm);
                break;
            }
        }
        WlanFreeMemory(pConn);
    }

    // --- RSSI (dBm) ---
    LONG rssi = 0;
    DWORD rssiSize = sizeof(rssi);
    dwResult = WlanQueryInterface(hClient, pGuid, wlan_intf_opcode_rssi,
                                  NULL, &rssiSize, (PVOID*)&rssi, &opCode);
    if (dwResult == ERROR_SUCCESS) out->rssi = (int32_t)rssi;

    // --- Channel ---
    ULONG channel = 0;
    DWORD channelSize = sizeof(channel);
    dwResult = WlanQueryInterface(hClient, pGuid, wlan_intf_opcode_channel_number,
                                  NULL, &channelSize, (PVOID*)&channel, &opCode);
    if (dwResult == ERROR_SUCCESS) out->channel = (int32_t)channel;

    WlanFreeMemory(pIfList);
    WlanCloseHandle(hClient, NULL);
    return true;
}

#else
// Stub for non-Windows
bool WlanDetect(WlanData* out) {
    (void)out;
    return false;
}
#endif
