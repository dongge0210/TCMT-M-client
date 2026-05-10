# ADR-0005: Unified IPCServer with platform #ifdef (not separate classes)

## Status

Accepted

## Context

TCMT originally had `NamedPipeServer` (Windows-only) and `IPCServer` (macOS UDS + POSIX shared memory) as separate classes with near-identical APIs (`Start`, `Stop`, `UpdateSchema`, `SerializeSchema`). Keeping them separate doubled maintenance: every schema change had to be ported to two files, and the common protocol logic (handshake, schema broadcast, keep-alive) was duplicated.

## Decision

Merge `NamedPipeServer` into `IPCServer` using `#ifdef _WIN32` guards in a single source file (`IPCServer.cpp`). The header (`IPCServer.h`) declares one class with platform-conditional member variables (`HANDLE` vs `int listenFd_`; `PipeClientInfo` vs `ClientInfo`). The implementation file is split into two large blocks at the file level: `#ifdef _WIN32` for the Windows NamedPipe implementation, and `#else` for the macOS UDS + POSIX shm implementation. Common functions (`SendSchemaToPeer`, `SerializeSchema`, `UpdateSchema`) are defined in both blocks but share identical logic. The old `NamedPipeServer.h/.cpp` are retained but no longer instantiated.

## Consequences

Easier: single `IPCServer` header and implementation to maintain; protocol constants and message types (`IPCData.h`) are shared; Windows clients migrating to schema-driven IPC reuse the same `FieldDef` serialization. Harder: the `#ifdef`-heavy file is 544 lines with two parallel implementation blocks; `ClientType` enum supports both Avalonia and MCP clients transparently; the macOS path manages its own shared memory (`shm_open`/`mmap`) while Windows delegates to `SharedMemoryManager`.
