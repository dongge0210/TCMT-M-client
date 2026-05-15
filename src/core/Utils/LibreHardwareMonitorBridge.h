#pragma once

#include <string>
#include <vector>
#include <utility>
#include <vcclr.h>
#include "../DataStruct/DataStruct.h"

// Relative path from TCMT.vcxproj (project root) to the .NET assembly
// "src\third_party\LibreHardwareMonitor\bin\Release\net472\LibreHardwareMonitorLib.dll"
#using "src\\third_party\\LibreHardwareMonitor\\bin\\Release\\net472\\LibreHardwareMonitorLib.dll"

// Add forward declaration for .NET types
namespace LibreHardwareMonitor {
    namespace Hardware {
        ref class Computer; // Forward declaration of Computer
    }
}

// Define UpdateVisitor class
ref class UpdateVisitor : public LibreHardwareMonitor::Hardware::IVisitor {
public:
    virtual void VisitComputer(LibreHardwareMonitor::Hardware::IComputer^ computer) {
        computer->Traverse(this);
    }
    virtual void VisitHardware(LibreHardwareMonitor::Hardware::IHardware^ hardware) {
        hardware->Update();
        hardware->Traverse(this);
    }
    virtual void VisitSensor(LibreHardwareMonitor::Hardware::ISensor^ /*sensor*/) {}
    virtual void VisitParameter(LibreHardwareMonitor::Hardware::IParameter^ /*parameter*/) {}
};

class LibreHardwareMonitorBridge {
public:
    static void Initialize();
    static void Cleanup();
    // Log semantic optimization: temperature sensor count
    static std::vector<std::pair<std::string, double>> GetTemperatures();
    // Added: Get physical disk SMART data
    static std::vector<PhysicalDiskSmartData> GetPhysicalDisks();

private:
    static bool initialized;
    static gcroot<LibreHardwareMonitor::Hardware::Computer^> computer; // Use gcroot to wrap Computer
    static gcroot<UpdateVisitor^> visitor; // Use gcroot to wrap UpdateVisitor
};