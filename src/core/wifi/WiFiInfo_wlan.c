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

// dot11TxRate was removed from WLAN_ASSOCIATION_ATTRIBUTES in SDK 10.0.26100+.
// Use a simple thunk so the build works across all SDK versions.
static ULONG GetTxRate(const PWLAN_CONNECTION_ATTRIBUTES pConn) {
    (void)pConn;
    return 0;
}

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
    {
        LONG rssi = 0;
        DWORD rssiSize = sizeof(rssi);
        WLAN_OPCODE_VALUE_TYPE rssiOpCode = wlan_opcode_value_type_query_only;
        dwResult = WlanQueryInterface(hClient, pGuid, wlan_intf_opcode_rssi,
                                      NULL, &rssiSize, (PVOID*)&rssi, &rssiOpCode);
        if (dwResult == ERROR_SUCCESS && rssiSize == 4) {
            // Some drivers return RSSI as a float (4 bytes), not LONG.
            // Check if value looks like a valid negative dBm first;
            // if not, reinterpret the same bytes as float.
            int32_t raw = (int32_t)rssi;
            if (raw < 0 && raw > -100) {
                out->rssi = raw;
            } else {
                float frssi;
                memcpy(&frssi, &rssi, sizeof(frssi));
                if (frssi < 0.0f && frssi > -100.0f)
                    out->rssi = (int32_t)frssi;
            }
        }
        // Fallback: use wlanSignalQuality (0-100%) from connection attributes
        if (out->rssi == 0 && out->isConnected && pConn) {
            ULONG sq = pConn->wlanAssociationAttributes.wlanSignalQuality;
            if (sq <= 100) out->rssi = -100 + (int32_t)sq * 70 / 100;
        }
    }

    // --- Channel ---
    {
        ULONG channel = 0;
        DWORD channelSizeOut = sizeof(channel);
        WLAN_OPCODE_VALUE_TYPE chOpCode = wlan_opcode_value_type_query_only;
        dwResult = WlanQueryInterface(hClient, pGuid, wlan_intf_opcode_channel_number,
                                      NULL, &channelSizeOut, (PVOID*)&channel, &chOpCode);
        if (dwResult == ERROR_SUCCESS && channelSizeOut == sizeof(channel)) {
            if (channel >= 1 && channel <= 255) {
                out->channel = (int32_t)channel;
            } else {
                float fch;
                memcpy(&fch, &channel, sizeof(fch));
                if (fch >= 1.0f && fch <= 255.0f)
                    out->channel = (int32_t)fch;
            }
        }
    }

    // --- Channel fallback via BSS list ---
    if (out->channel == 0 && out->isConnected && out->ssid[0] != '\0') {
        DOT11_SSID dot11Ssid;
        memset(&dot11Ssid, 0, sizeof(dot11Ssid));
        int ssidLen = (int)strlen(out->ssid);
        if (ssidLen > (int)DOT11_SSID_MAX_LENGTH) ssidLen = DOT11_SSID_MAX_LENGTH;
        dot11Ssid.uSSIDLength = (ULONG)ssidLen;
        memcpy(dot11Ssid.ucSSID, out->ssid, ssidLen);

        PWLAN_BSS_LIST pBssList = NULL;
        dwResult = WlanGetNetworkBssList(hClient, pGuid, &dot11Ssid,
                                          dot11_BSS_type_infrastructure,
                                          FALSE, NULL, &pBssList);
        {
            char dbg[128];
            snprintf(dbg, sizeof(dbg),
                "WlanDetect BSS: dwResult=0x%lX items=%lu bssid=%s\n",
                dwResult, pBssList ? pBssList->dwNumberOfItems : 0, out->bssid);
            OutputDebugStringA(dbg);
        }
        if (dwResult == ERROR_SUCCESS && pBssList && pBssList->dwNumberOfItems > 0) {
            if (strlen(out->bssid) == 17) {
                UCHAR target[6];
                for (int i = 0; i < 6; i++) {
                    char h[3] = {out->bssid[i*3], out->bssid[i*3+1], '\0'};
                    target[i] = (UCHAR)strtoul(h, NULL, 16);
                }
                for (DWORD i = 0; i < pBssList->dwNumberOfItems; i++) {
                    if (memcmp(target, pBssList->wlanBssEntries[i].dot11Bssid, 6) == 0) {
                        ULONG freqKhz = pBssList->wlanBssEntries[i].ulChCenterFrequency;
                        // Convert frequency (kHz) to channel number
                        if (freqKhz >= 2412000 && freqKhz <= 2484000)
                            out->channel = (int32_t)((freqKhz / 1000 - 2407) / 5);
                        else if (freqKhz >= 5000000)
                            out->channel = (int32_t)((freqKhz / 1000) / 5);
                        else if (freqKhz > 0)
                            out->channel = (int32_t)freqKhz; // raw value
                        break;
                    }
                }
            }
            WlanFreeMemory(pBssList);
        }
    }

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
