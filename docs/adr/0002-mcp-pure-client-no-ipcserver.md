# ADR-0002: MCP server as pure client (no IPCServer)

## Status

Accepted

## Context

The MCP server (`MCPServer`) runs on stdin/stdout per the MCP specification. When TCMT is launched with `--mcp`, there is no guarantee a main TCMT process is already running. Starting a second `IPCServer` inside the MCP process would create a duplicate IPC endpoint, confusing clients (Avalonia) and creating double shared-memory ownership.

## Decision

The `--mcp` mode MUST NOT start its own `IPCServer`. Instead, the MCP handler first attempts to connect as an `IPCClient` to an already-running TCMT instance. If a running instance is found, all tool calls read from shared memory via the schema protocol (`ReadString`, `ReadFloat64`, etc.). If no instance is found (connect timeout), the MCP mode falls back to direct hardware reads (`CpuInfo`, `MemoryInfo`, `OSInfo` constructors) with no IPC whatsoever.

## Consequences

Easier: single `IPCServer` per machine, no port/pipe conflicts; MCP mode is lightweight (no shared memory creation). Harder: GPU data in fallback mode requires a WMI manager, which is not initialized by default in `--mcp` mode (skipped in the current implementation).
