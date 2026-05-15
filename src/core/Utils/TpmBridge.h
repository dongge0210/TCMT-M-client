// TpmBridge.h
#pragma once
#ifdef TCMT_WINDOWS
// winsock2.h must be before windows.h
#include <winsock2.h>
#include <windows.h>
#else
// Define Windows types for non-Windows platforms
typedef uint32_t DWORD;
typedef uint8_t LPBYTE;
typedef int64_t HRESULT;
#endif

#include <vector>
#include "../DataStruct/DataStruct.h"

// TPM status codes
enum TpmStatus : uint8_t {
    StatusUnknown = 0,
    StatusOk = 1,
    StatusError = 2,
    StatusDisabled = 3
};

// TPM vendor ID (actually stored as uint32_t)
enum TpmVendorId : uint32_t {
    VendorUnknown = 0,
    VendorAMD = 0x414D44,      // "AMD"
    VendorIntel = 0x494E5445,  // "INTE"
    VendorMicrosoft = 0x4D5343, // "MSC"
    VendorInfineon = 0x494648, // "IFX"
    VendorSTMicro = 0x53544D,  // "STM"
    VendorNuvoton = 0x4E5554,  // "NUT"
    VendorBroadcom = 0x425243, // "BRC"
    VendorNationalSemiconductor = 0x4E5343 // "NSC"
};

class TpmBridge {
public:
    // Initialize TPM bridge
    static bool Initialize();

    // Cleanup TPM bridge
    static void Cleanup();

    // Get TPM information
    static bool GetTpmInfo(TpmInfo& info);

    // Check if TPM exists
    static bool IsTpmPresent();

private:
    static bool initialized;
    static bool tpmPresent;

    // Internal methods
    static HRESULT GetTpmProperty(DWORD propertyId, LPBYTE buffer, DWORD bufferSize);
    static std::wstring GetVendorString(uint32_t vendorId);
    static TpmStatus CheckTpmStatus();
};