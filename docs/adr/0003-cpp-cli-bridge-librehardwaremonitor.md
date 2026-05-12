# ADR-0003: C++/CLI bridge for LibreHardwareMonitor on Windows

## Status

Accepted

## Context

TCMT needs access to CPU/GPU/storage temperature sensors and SMART data. On Windows, LibreHardwareMonitor (LHM) is the most mature open-source hardware monitoring library, but it is written in C# / .NET Framework 4.7.2. Alternatives were: (a) run LHM in a separate managed process with named-pipe IPC; (b) rewrite temperature sensors in native C++ using direct IO; (c) use C++/CLI mixed-mode assembly to bridge directly.

## Decision

Use a C++/CLI bridge class (`LibreHardwareMonitorBridge`) compiled as part of the main C++/CLI project. The bridge loads LHM via `#using` directive (`src/third_party/LibreHardwareMonitor/bin/Release/net472/LibreHardwareMonitorLib.dll`) and wraps the .NET `Computer` object in `gcroot` pointers. Temperature and SMART queries are forwarded through `TemperatureWrapper`, a pure-C++ singleton that calls the bridge when available.

## Consequences

Easier: zero marshalling overhead for sensor data; single process, no IPC between C++ and .NET; LHM handles 100+ hardware models transparently. Harder: coupling to .NET Framework 4.7.2 (Windows-only); the bridge file must be compiled with `/clr` flag (mixed mode); macOS cannot use LHM and must implement its own temperature sensors via IOKit/SMC.
