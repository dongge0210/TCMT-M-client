# ADR-0002: MCP mode is an IPC client, never an IPC server

## Status

Accepted

## Context

TCMT exposes two distinct protocols:

- MCP uses JSON-RPC over stdin/stdout to serve tools to its caller.
- TCMT's internal IPC publishes a schema and hardware snapshot from the regular monitoring process to clients such as Avalonia.

`--mcp` can run alongside the regular monitoring process or by itself. The regular process owns the shared-memory block and the IPC endpoint. Starting another `IPCServer` in MCP mode would compete for that endpoint and make ownership of the published snapshot ambiguous. At the same time, MCP must remain usable when no regular monitoring process is running.

## Decision

MCP mode MUST NOT create shared memory or construct or start an `IPCServer`. It starts only the stdio MCP server and makes one bounded `IPCClient` connection attempt during startup.

- If the connection succeeds, tool handlers read the schema-described shared-memory snapshot. MCP closes the IPC transport after setup, but keeps the mapped snapshot available for reads.
- If the connection fails, tool handlers use their direct hardware-read paths where implemented. MCP does not start a collector or create an IPC endpoint as a fallback.

The choice between these paths is made once at startup; MCP does not reconnect or switch paths if a monitoring process starts or stops later. The direct-read path is tool- and platform-specific and is not required to provide the same fields as the shared-memory path.

## Consequences

- There is one owner for the IPC endpoint and published snapshot; MCP cannot create a competing server or interfere with Avalonia clients.
- When a regular monitoring process is available, MCP reads its existing snapshot instead of starting another collector.
- MCP can run without that process, but direct-read coverage may be incomplete or differ from the shared-memory output.
- MCP and hardware IPC remain separate interfaces: the former serves tool requests over stdio; the latter is an optional data source for those tools.
