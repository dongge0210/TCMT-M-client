// TpmBridge.cpp
#include "TpmBridge.h"
#include "Logger.h"
#include <winternl.h>
#include <tbs.h>
#include <mutex>

// Static member initialization
bool TpmBridge::initialized = false;
bool TpmBridge::tpmPresent = false;

bool TpmBridge::Initialize() {
    static std::once_flag initFlag;
    std::call_once(initFlag, [&]{
        // Check if TPM exists
        tpmPresent = IsTpmPresent();
        initialized = true;
    });
    return initialized;
}

void TpmBridge::Cleanup() {
    initialized = false;
    tpmPresent = false;
}

bool TpmBridge::IsTpmPresent() {
    // Use TBS API to check if TPM exists
    TPM_DEVICE_INFO info = {};
    UINT32 size = sizeof(info);

    TBS_RESULT result = Tbsi_GetDeviceInfo(size, &info);

    if (result == TBS_SUCCESS) {
        // tpmVersion: 1 = 1.2, 2 = 2.0
        return (info.tpmVersion >= 1 && info.tpmVersion <= 2);
    }

    return false;
}

bool TpmBridge::GetTpmInfo(TpmInfo& info) {
    // Initialize return structure
    memset(&info, 0, sizeof(TpmInfo));

    if (!initialized) {
        Initialize();
    }

    if (!tpmPresent) {
        info.isPresent = false;
        info.status = static_cast<uint8_t>(StatusDisabled);
        return true; // Return success but TPM does not exist
    }

    info.isPresent = true;

    // Use TBS to get TPM information
    TBS_HCONTEXT hContext = NULL;

    // Create TBS context (use version 2 to support TPM 2.0)
    TBS_CONTEXT_PARAMS2 params2 = {};
    params2.version = TBS_CONTEXT_VERSION_TWO;
    params2.includeTpm20 = 1;  // Request TPM 2.0 support

    // Use PCTBS_CONTEXT_PARAMS force conversion
    PCTBS_CONTEXT_PARAMS pParams = (PCTBS_CONTEXT_PARAMS)&params2;
    TBS_RESULT result = Tbsi_Context_Create(pParams, &hContext);

    if (result != TBS_SUCCESS) {
        Logger::Warn("Failed to create TBS context");
        info.status = static_cast<uint8_t>(StatusError);
        return false;
    }

    // Get TPM device information
    TPM_DEVICE_INFO tpmInfo = {};
    UINT32 infoSize = sizeof(tpmInfo);
    result = Tbsi_GetDeviceInfo(infoSize, &tpmInfo);

    if (result == TBS_SUCCESS) {
        // TPM version
        if (tpmInfo.tpmVersion == 1) {
            swprintf_s(info.firmwareVersion, L"1.2");
            info.firmwareVersionMajor = 1;
            info.firmwareVersionMinor = 2;
        } else if (tpmInfo.tpmVersion == 2) {
            swprintf_s(info.firmwareVersion, L"2.0");
            info.firmwareVersionMajor = 2;
            info.firmwareVersionMinor = 0;
        }

        // Interface type
        switch (tpmInfo.tpmInterfaceType) {
            case TPM_IFTYPE_HW:
                wcsncpy_s(info.manufacturer, L"Hardware TPM", _TRUNCATE);
                break;
            case TPM_IFTYPE_EMULATOR:
                wcsncpy_s(info.manufacturer, L"Software Emulator", _TRUNCATE);
                break;
            default:
                wcsncpy_s(info.manufacturer, L"Unknown", _TRUNCATE);
                break;
        }

        // Get vendor ID from implementation revision
        info.vendorId = static_cast<uint16_t>(tpmInfo.tpmImpRevision & 0xFFFF);

        // Override manufacturer with actual TPM vendor name (was set to interface type above)
        std::wstring vendorName = GetVendorString(info.vendorId);
        if (!vendorName.empty()) {
            wcsncpy_s(info.manufacturer, vendorName.c_str(), _TRUNCATE);
        }
    }

    // Cleanup context
    if (hContext) {
        Tbsip_Context_Close(hContext);
    }

    // Check TPM status
    info.status = static_cast<uint8_t>(CheckTpmStatus());

    // Set default enabled and active status
    info.isEnabled = (info.status == static_cast<uint8_t>(StatusOk) || info.status == static_cast<uint8_t>(StatusUnknown));
    info.isActive = (info.status == static_cast<uint8_t>(StatusOk));

    // Self-test status - simplified handling
    info.selfTestStatus = (info.status == static_cast<uint8_t>(StatusOk)) ? 0 : 1;

    return true;
}

std::wstring TpmBridge::GetVendorString(uint32_t vendorId) {
    // Extract readable string from vendor ID (low 3 bytes)
    char vendorStr[5] = {};
    vendorStr[0] = static_cast<char>((vendorId >> 16) & 0xFF);
    vendorStr[1] = static_cast<char>((vendorId >> 8) & 0xFF);
    vendorStr[2] = static_cast<char>(vendorId & 0xFF);
    vendorStr[3] = '\0';

    // Convert to common vendor names
    if (strcmp(vendorStr, "AMD") == 0) return L"AMD";
    if (strcmp(vendorStr, "INTE") == 0) return L"Intel";
    if (strcmp(vendorStr, "MSC") == 0) return L"Microsoft";
    if (strcmp(vendorStr, "IFX") == 0) return L"Infineon";
    if (strcmp(vendorStr, "STM") == 0) return L"STMicroelectronics";
    if (strcmp(vendorStr, "NUT") == 0) return L"Nuvoton";
    if (strcmp(vendorStr, "BRC") == 0) return L"Broadcom";
    if (strcmp(vendorStr, "NSC") == 0) return L"National Semiconductor";

    // Try direct conversion to wide character
    wchar_t result[32] = {};
    mbstowcs_s(nullptr, result, vendorStr, _TRUNCATE);
    return result;
}

TpmStatus TpmBridge::CheckTpmStatus() {
    // Use TBS to get TPM device status
    TPM_DEVICE_INFO info = {};
    UINT32 size = sizeof(info);

    TBS_RESULT result = Tbsi_GetDeviceInfo(size, &info);

    if (result != TBS_SUCCESS) {
        return StatusError;
    }

    switch (info.tpmVersion) {
        case 1:
        case 2:
            return StatusOk;
        default:
            return StatusDisabled;
    }
}