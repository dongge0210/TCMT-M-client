# ADR-0001: Schema-driven IPC for cross-platform shared memory

## Status

Accepted

## Context

The initial design used a fixed-offset `SharedMemoryBlock` struct, with both sides compiled from the same `DataStruct.h`. Any field addition, removal, or reorder required a simultaneous rebuild of C++ backend and C# frontend. A second struct `IPCDataBlock` appeared in `IPCData.h` with a different layout, creating two parallel shared memory mechanisms and a maintenance burden.

## Decision

Migrate to a schema-driven IPC protocol. The C++ backend broadcasts a `SchemaHeader` + array of `FieldDef` (80 bytes each, describing offset, type, size, name) over a named pipe / Unix domain socket during the `HelloAck` handshake. The C# frontend parses the schema and reads fields by name from shared memory using the advertised offsets. The schema is versioned (`uint8_t version`), allowing forward and backward compatible changes.

## Consequences

Easier: protocol evolves without recompiling both sides; C# reads only the fields it knows about. Harder: older `SharedMemoryBlock` path (`DataStruct.h`) still coexists and must be retired; C# reader must validate `magic == 0x54434D54` on every schema message; `BuildWindowsIpcSchema()` in `main.cpp` still derives offsets from `SharedMemoryBlock`, which should be replaced with a canonical struct.
