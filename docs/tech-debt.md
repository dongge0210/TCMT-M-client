# Technical Debt (2026-06-27)

## P0 — 双重 IPC 数据结构 ✅ DONE (2026-06-27)

ADR-0001 决定退役 `SharedMemoryBlock`，迁移到 Schema 驱动的 `IPCDataBlock`。

| 旧 | 新 | 状态 |
|----|----|------|
| `DataStruct.h` (305行) — `SharedMemoryBlock` | `IPCData.h` (214行) — `IPCDataBlock` | ✅ 已迁移 |

**已完成的变更：**
- Windows `SharedMemoryManager` → `IPCDataBlock*`（SHM init, MapViewOfFile, WriteToSharedMemory）
- `BuildWindowsIpcSchema` 从 offsetof(SharedMemoryBlock) 切换到 offsetof(IPCDataBlock)
- WCHAR→char/String, Float64→Float32, Int32→UInt8 类型统一
- macOS/Linux `WriteToSharedMemory` 改为 stub（这些平台用 IPCServer 直接路径）
- `main.cpp` 移除冗余 WiFi/BT 直接写入
- `IPCDataBlock` 补齐 WiFi band/gen + TPM + swapUsed/Total 字段

**待清理：** `DataStruct.h` 中 `SharedMemoryBlock` struct 定义仍存在（无引用，可安全删除）。

## P1 — `main.cpp` 1974行

`main()` 函数 ~1200 行（L727–L1939），内含：
- COM 初始化
- `--json` 模式
- `--mcp` 模式
- TUI 模式
- IPC server
- 硬件监控主循环
- 退出清理

无抽象分层，全塞在一个函数里。

## P2 — 三个 main 文件重复逻辑

| 文件 | 行数 | 平台 |
|------|------|------|
| `main.cpp` | 1974 | Windows |
| `main_mac.cpp` | 1282 | macOS |
| `main_linux.cpp` | 735 | Linux |

每个都有自己的 Schema builder、格式化 helper、信号处理，同逻辑抄三份。

## P3 — SharedMemoryManager 双实现

| 文件 | 行数 |
|------|------|
| `SharedMemoryManager_Windows.cpp` | 477 |
| `SharedMemoryManager_macOS.cpp` | 350 |

同一概念两套实现，Windows API vs POSIX shm。

## P4 — 其他大文件

- `TemperatureWrapper.cpp` — 1571行，可拆分
- `IPCServer.cpp` — 583行，8个 `#ifdef`，双实现块塞一个文件（ADR-0005）
