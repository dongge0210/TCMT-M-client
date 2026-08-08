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

## P1 — `main.cpp` 拆层 ✅ DONE (2026-08-08)

`main()` 从 ~1080 行拆到 ~120 行，只保留启动编排；各阶段抽成顶层静态函数：
- `EnsureElevated()` — UAC 提权重启
- `InitCom()` — COM 初始化（多线程 → 单线程回退）
- `InitSharedMemoryAndIpc()` — 共享内存 + IPC server 启动
- `InitWmiManager()` — WMI 管理器创建与失败处理
- `InitTemperatureBridge()` — 温度桥初始化（非致命）
- `InitHistoryLogger()` — SQLite 历史库路径 + 初始化
- `RunMonitoringLoop()` — 1s 监控主循环（采集、TUI 更新、共享内存写入、历史记录）

`main()` 现状：bootstrap（SEH/控制台/信号）→ `--json`/`--mcp` 分发 → 提权 → 初始化链 → TUI/USB/历史 → `RunMonitoringLoop` → 退出清理。


## P2 — 三个 main 文件重复逻辑

| 文件 | 行数 | 平台 |
|------|------|------|
| `main.cpp` | 2139 (已拆层) | Windows |
| `main_mac.cpp` | 1461 | macOS |
| `main_linux.cpp` | 735 | Linux |

每个都有自己的 Schema builder、格式化 helper、信号处理，同逻辑抄三份。

## P3 — SharedMemoryManager 双实现

| 文件 | 行数 |
|------|------|
| `SharedMemoryManager_Windows.cpp` | 477 |
| `SharedMemoryManager_macOS.cpp` | 350 |

同一概念两套实现，Windows API vs POSIX shm。

## P4 — 其他大文件

- `TemperatureWrapper.cpp` — 1464行，可拆分
- `IPCServer.cpp` — 583行，8个 `#ifdef`，双实现块塞一个文件（ADR-0005）
