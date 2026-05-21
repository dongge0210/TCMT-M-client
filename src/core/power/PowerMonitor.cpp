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

// Private methods — stubs on Windows (not called)
void PowerMonitor::SampleLoop() {}
void PowerMonitor::ParsePowerDelta(void*) {}
int64_t PowerMonitor::ExtractChannelValue(void*) { return 0; }
double PowerMonitor::EnergyToPower(void*, int64_t) { return 0.0; }

#endif // !TCMT_MACOS
