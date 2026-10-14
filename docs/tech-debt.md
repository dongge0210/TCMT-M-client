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

**待清理 → 已清理（2026-10-14 核对）：** `SharedMemoryBlock` 现在全仓**零命中**（`src` 与 `AvaloniaUI` 都搜过），struct 定义已删除。此条不再待办。

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

**Schema builder 实测（2026-10-14，逐字段比过）**：**只有两份**，`main_linux.cpp` 现在一份都没有
（本文件早先写「三份」，那是过期的）。两份分别是 `main_mac.cpp:79 BuildIPCDataBlockSchema`
（函数体 169 行）与 `main.cpp:636 BuildWindowsIpcSchema`（180 行）：

- **Windows 多**：8 个 `tpm/*` + 循环里的 `letter` / `letterCount`。反方向为 0。
- **顺序不同**：从第 4 个共有字段起分叉。**不影响正确性** —— 消费方按名字查 offset
  （`IPCMemoryReader.FindField` → `field.Offset`），这是 ADR-0001 定下的形态。
- **size 写法不同（26 处）**：mac **手写数字**，win 用 `sizeof(B::x)`。抽 8 个核对过，当前值都对；
  风险在 mac 侧 —— 结构体一改，数字不动也不报错，按旧尺寸静默截断。
- **API 形状不同**：mac 是一组带类型的 lambda（`addS/addU8/addF/...`），win 是单个
  `addField(name, offsetof, size, type)`，type 默认 `Float32`。
- 一致的部分：动态片段两边都用 slot（`B::DiskSlot` / `B::NetSlot`）+ `base + offsetof(...)`；
  `offsetof` 表达式 **0 处不同**；循环上界两边完全相同（4/4/8/6/16）。
- 字段总数是**常量**（循环上界写死），两边都远低于 `IPC_MAX_FIELDS = 300`，今天不存在静默截断。

**合并的形态文档里已经定过**：按 ADR-0005 的先例（`NamedPipeServer` + `IPCServer` → 单个
`IPCServer.cpp`，文件级 `#ifdef _WIN32` 分块 + 公共逻辑共用），C16 也应当是一个文件、文件级守卫。
**合并的坑**：`IPCData.h` 的 `IPCDataBlock` 两边是同一个结构体，`TpmSlot tpm` 在 macOS 上也存在
（只是从不填）—— 所以一旦无条件合并，macOS 会广播出全零的 `tpm/*`，界面显示「TPM 不存在」，
正是本项目被咬过的静默假读数。守卫必须留在表里（`tpm/*` 与 `letter`/`letterCount`），
而且要下到**循环体内部**。尺寸统一到 `sizeof` 是行为变更（C# 按 size 决定字符串读多长），
26 处里还有 18 处未核。

## P3 — SharedMemoryManager 双实现

| 文件 | 行数 |
|------|------|
| `SharedMemoryManager_Windows.cpp` | 477 |
| `SharedMemoryManager_macOS.cpp` | 350 |

同一概念两套实现，Windows API vs POSIX shm。

## P4 — 其他大文件

- `TemperatureWrapper.cpp` — 1464行，可拆分
- `IPCServer.cpp` — 583行，8个 `#ifdef`，双实现块塞一个文件（ADR-0005）
