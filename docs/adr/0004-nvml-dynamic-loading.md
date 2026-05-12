# ADR-0004: NVML dynamic loading instead of static linking

## Status

Accepted

## Context

NVIDIA Management Library (NVML) provides GPU usage, temperature, VRAM, and clock data not available through WMI `Win32_VideoController`. Statically linking `nvml.lib` would make `nvml.dll` a hard runtime dependency, causing the program to crash during DLL load on any system without an NVIDIA GPU (Intel-only, AMD, or integrated graphics).

## Decision

Load NVML at runtime via `LoadLibraryW(L"nvml.dll")` and resolve every function pointer through `GetProcAddress`. The function pointers are stored in a singleton `NvmlApi` struct with `nullptr` defaults. If `nvml.dll` is missing or any mandatory function (`nvmlInit_v2`, `nvmlShutdown`, `nvmlDeviceGetHandleByIndex_v2`) cannot be resolved, all NVML queries are silently skipped and GPU data falls back to WMI alone. A persistent `NvmlSession` singleton keeps `nvmlInit` active across the process lifetime to avoid per-query overhead.

## Consequences

Easier: zero crash risk on non-NVIDIA systems; identical binary runs on Intel, AMD, and NVIDIA machines; no CUDA/NVML SDK required at build time. Harder: function pointer syntax is verbose (8 typedefs + 8 GetProcAddress calls); version skew risk if `nvml.dll` exports change their ordinal; no NVML fallback on macOS/AMD GPUs.
