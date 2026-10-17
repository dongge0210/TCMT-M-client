// PowerMonitor.cpp — cross-platform dispatch stub.
// macOS implementation lives in PowerMonitor_mac.mm (compiled only on macOS).
// On non-macOS this file is an empty stub: Start() returns false and every getter stays zero.
// It provides no readings and no fallback.
// Both files must NOT be compiled together on the same platform.

#ifndef TCMT_MACOS  // macOS impl is in PowerMonitor_mac.mm

#include "PowerMonitor.h"
#include "../Utils/Logger.h"

PowerMonitor::PowerMonitor() {}
PowerMonitor::~PowerMonitor() { Stop(); }

bool PowerMonitor::Start() { return false; }
void PowerMonitor::Stop() {}

bool PowerMonitor::IsPowerAvailable() const { return false; }

// Private methods — stubs on Windows (not called)
void PowerMonitor::SampleLoop() {}
void PowerMonitor::ParsePowerDelta(void*) {}
int64_t PowerMonitor::ExtractChannelValue(void*) { return 0; }
double PowerMonitor::EnergyJoules(void*, int64_t) { return 0.0; }
void PowerMonitor::ReadShmPower() {}

#endif // !TCMT_MACOS
