# Session State (2026-06-10)

## Branch
`dev`

## Goal
Clean up unused submodules, enable QUIC/HTTP3 in curl, update remaining submodules.

## Current State
- **Submodules reduced**: removed websocketpp and USBMonitor-cpp (7 remain)
- **curl**: updated to a6cece52, QUIC/HTTP3 enabled via `USE_NGTCP2=ON` (upstream replaced `USE_OPENSSL_QUIC`)
- **All submodules**: pulled to latest remotes/origin/HEAD (CPP-parsers + 5 nested, PDCurses, TC, curl, tpm2-tss)
- **OpenSSL 3.6.2 + nghttp3**: brew-installed, ready for HTTP/3 builds

## Commits
| SHA | Message |
|-----|---------|
| `58a2441a` | chore: update submodules and enable HTTP/3 (QUIC) in curl |
| `96baf265` | Remove USBMonitor-cpp submodule (src/third_party/USBMonitor-cpp) |
| `9fe7a7d6` | Remove websocketpp submodule (src/third_party/websocketpp) |

## Build (curl with QUIC)
```bash
cmake -B build-curl-quic -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF \
  -DCMAKE_PREFIX_PATH=$(brew --prefix openssl@3);$(brew --prefix nghttp3)
cmake --build build-curl-quic -j8
```
curl is a standalone submodule, not integrated into the project CMake.

## Unstaged
- `TCMT.sln`: unrelated rename (AvaloniaUI.csproj → TCMT.Avalonia.csproj)

## User Habits
- Zero warnings policy
- One change per commit
- Prefers sonnet or pro models for code changes
