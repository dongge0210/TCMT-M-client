// PowerMonitor.cpp — cross-platform dispatch stub.
// macOS implementation lives in PowerMonitor_mac.mm (compiled only on macOS).
// This file provides the Windows/fallback implementation.
// Both files must NOT be compiled together on the same platform.

#ifndef TCMT_MACOS  // macOS impl is in PowerMonitor_mac.mm

#include "PowerMonitor.h"
#include "../Utils/Logger.h"

PowerMonitor::PowerMonitor() {}
PowerMonitor::~PowerMonitor() { Stop(); }

bool PowerMonitor::Start() { return false; }
void PowerMonitor::Stop() {}
bool PowerMonitor::IsRunning() const { return false; }

void PowerMonitor::SetPowerCallback(PowerEventCallback) {}
void PowerMonitor::SetDiskCallback(DiskEventCallback) {}
void PowerMonitor::SetNetworkCallback(NetworkEventCallback) {}
void PowerMonitor::SetThermalCallback(ThermalEventCallback) {}

double PowerMonitor::GetPCoreFreq() const { return 0.0; }
double PowerMonitor::GetECoreFreq() const { return 0.0; }
double PowerMonitor::GetGpuFreq() const   { return 0.0; }
double PowerMonitor::GetCpuPower() const   { return 0.0; }
double PowerMonitor::GetGpuPower() const   { return 0.0; }
double PowerMonitor::GetAnePower() const   { return 0.0; }
bool PowerMonitor::IsDirectMode() const    { return false; }

// Stubs for platform-specific methods (not called on non-macOS)
void PowerMonitor::SampleLoop() {}
void PowerMonitor::ParsePowerDelta(void*) {}
int64_t PowerMonitor::ExtractChannelValue(void*) { return 0; }
double PowerMonitor::EnergyToPower(void*, int64_t) { return 0.0; }

#endif // !TCMT_MACOS
