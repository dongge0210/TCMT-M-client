#include "LibreHardwareMonitorBridge.h"
#include "Logger.h"
#include <msclr/marshal_cppstd.h>
#include <iostream>
// NOTE: winsock2.h must be included BEFORE windows.h
#include <winsock2.h>
#include <windows.h>

// No need to repeat #using, already included in header file

using namespace LibreHardwareMonitor::Hardware;
using namespace System;
using namespace System::Collections::Generic;
using namespace msclr::interop;

// Define static members
bool LibreHardwareMonitorBridge::initialized = false;
gcroot<Computer^> LibreHardwareMonitorBridge::computer;
gcroot<UpdateVisitor^> LibreHardwareMonitorBridge::visitor;

void LibreHardwareMonitorBridge::Initialize() {
    try {
        if (initialized) return;
        computer = gcnew Computer();
        computer->IsCpuEnabled = true;
        computer->IsGpuEnabled = true;
        computer->IsStorageEnabled = true;
        computer->Open();
        visitor = gcnew UpdateVisitor();
        initialized = true;
    }
    catch (System::IO::FileNotFoundException^ ex) {
        // Use marshal_as to convert .NET string to std::string
        std::string errorMsg = msclr::interop::marshal_as<std::string>(ex->Message);
        Logger::Error("LibreHardwareMonitor initialization failed: " + errorMsg);
    }
    catch (System::Exception^ ex) {
        std::string errorMsg = msclr::interop::marshal_as<std::string>(ex->Message);
        Logger::Error("LibreHardwareMonitor initialization exception: " + errorMsg);
    }
}

void LibreHardwareMonitorBridge::Cleanup() {
    if (!initialized) return;
    computer->Close();
    computer = nullptr;
    visitor = nullptr;
    initialized = false;
}

std::vector<std::pair<std::string, double>> LibreHardwareMonitorBridge::GetTemperatures() {
    std::vector<std::pair<std::string, double>> temps;
    if (!initialized) return temps;

    computer->Accept(visitor);
    for each (IHardware ^ hardware in computer->Hardware) {
        hardware->Update();
        if (hardware->HardwareType == HardwareType::Cpu ||
            hardware->HardwareType == HardwareType::GpuNvidia ||
            hardware->HardwareType == HardwareType::GpuAmd) {
            for each (ISensor ^ sensor in hardware->Sensors) {
                if (sensor->SensorType == SensorType::Temperature && sensor->Value.HasValue) {
                    std::string name = marshal_as<std::string>(sensor->Name);
                    temps.push_back({ name, sensor->Value.Value });
                }
            }
        }
    }
    return temps;
}

std::vector<PhysicalDiskSmartData> LibreHardwareMonitorBridge::GetPhysicalDisks() {
    std::vector<PhysicalDiskSmartData> disks;
    if (!initialized) return disks;

    try {
        computer->Accept(visitor);

        for each (IHardware ^ hardware in computer->Hardware) {
            if (hardware->HardwareType == HardwareType::Storage) {
                hardware->Update();

                PhysicalDiskSmartData diskData = {};
                diskData.smartSupported = false;
                diskData.smartEnabled = false;
                diskData.healthPercentage = 0;
                diskData.temperature = 0.0;
                diskData.attributeCount = 0;

                // Get disk basic information
                std::string hwName = marshal_as<std::string>(hardware->Name);
                wchar_t nameBuffer[128] = {};
                mbstowcs_s(nullptr, nameBuffer, hwName.c_str(), _TRUNCATE);
                wcsncpy_s(diskData.model, nameBuffer, _TRUNCATE);

                // Iterate sensors to get SMART data
                for each (ISensor ^ sensor in hardware->Sensors) {
                    if (!sensor->Value.HasValue) continue;

                    double value = sensor->Value.Value;

                    // Fill data based on sensor type
                    switch (sensor->SensorType) {
                        case SensorType::Temperature:
                            diskData.temperature = value;
                            diskData.smartSupported = true;
                            diskData.smartEnabled = true;
                            break;
                        case SensorType::Level:
                            // Health percentage or available space
                            if (sensor->Name && sensor->Name->Contains("Life")) {
                                diskData.healthPercentage = static_cast<uint8_t>(value);
                            }
                            break;
                        case SensorType::Data:
                            // Write/Read data amount
                            if (sensor->Name && sensor->Name->Contains("Written")) {
                                diskData.totalBytesWritten = static_cast<uint64_t>(value);
                            } else if (sensor->Name && sensor->Name->Contains("Read")) {
                                diskData.totalBytesRead = static_cast<uint64_t>(value);
                            }
                            break;
                        case SensorType::Throughput:
                            // Throughput
                            break;
                        default:
                            break;
                    }
                }

                // Simply set interface type to Unknown (actually need to get from hardware properties)
                wcsncpy_s(diskData.interfaceType, L"Unknown", _TRUNCATE);
                wcsncpy_s(diskData.diskType, L"Unknown", _TRUNCATE);

                // If SMART is supported but no health data, default to 100%
                if (diskData.smartSupported && diskData.healthPercentage == 0) {
                    diskData.healthPercentage = 100;
                }

                disks.push_back(diskData);
            }
        }
    }
    catch (System::Exception^ ex) {
        std::string errorMsg = msclr::interop::marshal_as<std::string>(ex->Message);
        Logger::Warn("Failed to get disk SMART data: " + errorMsg);
    }

    return disks;
}