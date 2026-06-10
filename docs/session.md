# Session State (2026-06-10)

## Branch
`dev`

## Goal
Clean up unused submodules, enable QUIC/HTTP3 in curl, update remaining submodules.

## Current State
- **Submodules reduced**: removed websocketpp and USBMonitor-cpp (9 remain + 5 nested)
- **curl**: updated to a6cece52, QUIC/HTTP3 enabled via `USE_NGTCP2=ON` (pushed to master@dongge0210/curl)
- **h2o**: added (ed7899d, dongge0210/h2o) - HTTP/2+3 server library
- **openssl**: added (13970e2, dongge0210/openssl)
- **zlib**: updated to e3dc0a8
- **FFmpeg**: removed stale entry from .gitmodules

## Commits
| SHA | Message |
|-----|---------|
| `10181e3e` | Revert "chore: sync remaining changes" |
| `4fb392dc` | chore: add h2o and openssl submodules, remove FFmpeg, update zlib |
| `58a2441a` | chore: update submodules and enable HTTP/3 (QUIC) in curl |
| `96baf265` | Remove USBMonitor-cpp submodule |
| `9fe7a7d6` | Remove websocketpp submodule |

## Build (curl with QUIC)
```bash
cmake -B build-curl-quic -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DCURL_USE_LIBPSL=OFF -DUSE_LIBIDN2=OFF \
  -DCMAKE_PREFIX_PATH=$(brew --prefix openssl@3);$(brew --prefix nghttp3)
cmake --build build-curl-quic -j8
```

## User Habits
- Zero warnings policy
- One change per commit
- Prefers sonnet or pro models for code changes
