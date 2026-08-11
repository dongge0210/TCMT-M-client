# Session State (2026-08-08)

## 2026-08-11 — macOS 适配（dev 拉取后恢复构建）

### 背景
- `git pull` 拉到 dev 一批 Windows 侧重构（PID 监控面板、独立 Win32 日志窗口、NVMe 健康日志、SMART 多路径等），macOS 构建被破坏：`main_mac.cpp` 仍调用已被删除的 `TuiApp::SetLogBuffer`
- 上游 commit `21ee2299` 把 CPP-parsers 子模块指针升到 `ed3ee062`，但该 commit 未推送（远端只有 `6c484696`）——本地已提交指针回退 `22a27ae4`

### 修复（本机已验证）
1. **TUI 日志页恢复（非 Windows）**：Windows 重构删除了 TUI 内日志页（改用 Win32 LogWindow）。macOS/Linux 无此窗口，按原需求恢复 `SetLogBuffer`/`RenderLogPage`/`Tab`/`l` 切换，用 `#ifndef TCMT_WINDOWS` 隔离，Windows 行为不变
2. **退出崩溃**：`TuiApp::Stop()` 在 `running_==false` 时直接 return 不 join 线程，`q` 退出后 `~std::thread` 对仍 joinable 的线程 `std::terminate()`。改为无条件 join
3. **退出延迟 26s → ~2s**：`TemperatureWrapper` powermetrics 后台线程退出时 join，卡在 30s sleep 或 10s 采集中。sleep 改为 condition_variable 可中断等待，Cleanup 时 notify；顺带修复 SIGALRM 处理器提前恢复导致 `alarm(15)` 超时会杀死进程的隐患
4. **构建架构**：根 CMakeLists 硬编码 `arm64;x86_64` 通用架构，但 Homebrew ncurses 仅 arm64 → x86_64 切片链接失败。本地 build 目录改用 `-DCMAKE_OSX_ARCHITECTURES=arm64`

### 2026-08-11 追加 — 功能对齐 Windows + 原生日志窗口
- **同进程 AppKit 日志窗口（`MacLogWindow.mm`，对应 Windows LogWindow）**：TUI 启动后同进程创建原生 “TCMT - Log” 窗口，读同一个内存 `LogBuffer`（新增 `Version()` 检测滚动/清空），0.5s 定时刷新。监控循环移到后台线程，主线程跑 `[NSApp run]` 事件循环，窗口完全可交互（滚动/选中/复制）；补齐最小主菜单（Edit → Copy/Select All）让 Cmd+C/Cmd+A 可用。无 WindowServer 会话（`CGMainDisplayID()==0`）自动回退 TUI 内日志页，不崩溃
- **AppKit 注册 abort 修复**：退化/后台会话中 `[NSApplication sharedApplication]`（WiFiInfo 旧代码）与 `[NSScreen screens]`（DisplayInfo）会经 HIServices `_RegisterApplication` SIGABRT 杀死整个程序。WiFiInfo 改为不强制 NSApplication（CoreLocation 授权状态/请求无需它，system_profiler 兜底 SSID）；DisplayInfo 用 `CGMainDisplayID()` 探测后跳过
- **功能差距审计**：macOS 已覆盖 topProcesses（PID 面板，CPU%/内存）、NVMe 健康（SSD 97% 33C 1713h）、per-core、IPC/MCP、HistoryLogger 等；`wifiBand/wifiGen` 已补齐（CoreWLAN channelBand + activePHYMode 推导，TUI 与 IPC 共享内存同步）；`cpuBaseFreq`/`gpuFans`/`tpmInfo` 在 macOS 无对应数据源（Apple Silicon 无 TPM/独立 GPU 风扇）

### 验证
- `cmake --build build -j8` 全绿；`TCMT-M.app` 正常生成（未 codesign）
- `--json` 单次快照正常输出（CPU/磁盘等）
- TUI 双页面（dashboard / log Tab+l）正常，进程面板、磁盘健康（SSD 97% 33C 1713h）正常
- `q` 退出干净：`Exiting` → `HistoryLogger stopped` 同秒完成，进程 2.6s 退出 code 0
- 本机 exec 环境无 WindowServer（CGMainDisplayID=0）：日志窗口与显示信息自动降级，TUI 持续运行 12s+ 无崩溃，`q` 退出 code 0（AppKit 窗口本体需用户真实终端会话验证）

### 待办/注意
- 通用架构构建（`arm64;x86_64`）仍需 universal ncurses 或按机器覆盖架构
- `src/main_linux.cpp:466` 同款 `SetLogBuffer` 调用已由 TuiApp 修复覆盖，Linux 应能编译（未实测）
- 上游 CPP-parsers 坏指针待上游修复（push `ed3ee062` 或回退）

---

## TUI 重做 — dashboard / log 双页面（都在主程序内，不走 IPC）

- 需求（最终版）：TUI 与 LOG 都是主程序的一部分，**不做独立进程、不走任何 IPC、不读日志文件**；默认启用两个页面
- 方案：
  - `TuiApp` 单进程内双页面：Dashboard（硬件面板）+ Log（全屏滚动日志页）
  - 按键 `Tab` / `l` 切换页面；Log 页 `f`=跟随、方向键/PgUp/PgDn/Home/End 滚动、`q` 退出
  - Log 页直接读进程内 Logger 日志缓冲（`LogBuffer`），容量 500 → 2000 行
  - dashboard 移除底部压缩日志面板（原遗留问题，2026-07-22 会话已记录）
- 废弃：之前实现的 `LogPipe`（命名管道/Unix socket）、`--tui-log` 独立进程方案已整体删除
- 其他：全仓库文本文件统一转 UTF-8（原 6 个 UTF-16LE + 4 个 UTF-8 BOM 已转换）
- 注意：本机全量 msbuild 需加 `/p:WholeProgramOptimization=false`（LTCG 代码生成在当前会话环境会 ICE）；`/MP` 已从 TCMT.vcxproj 移除


### 2026-08-08 完成
- `fix(disk)` d8001522 — NVMe Power On Hours 改读标准偏移 128（NVMe 计数器为 128-bit 字段；与 DiskGenius 实测 14307h 吻合），补建 `NVMe_HealthLog.h` 偏移常量，修正规范文档布局表
- `refactor(win)` 008afc63 — main.cpp 拆层：`main()` 由 ~1080 行降到 ~120 行，抽出 `EnsureElevated`/`InitCom`/`InitSharedMemoryAndIpc`/`InitWmiManager`/`InitTemperatureBridge`/`InitHistoryLogger` + `RunMonitoringLoop`
- `chore` — 移除 LibreHardwareMonitor：子模块、TCMT.sln 项目、CMake 引用、空桥接文件、ADR-0003、文档与注释中的 LHM 引用全部清除（TemperatureWrapper 早已不用 LHM）
- `feat(tui)` — 独立日志窗口：控制台全屏 dashboard，主程序同进程创建 Win32 日志窗口读 LogBuffer（无 IPC/文件/子进程）；移除 Tab/l 切换日志页
- 待办池：Linux WiFi 编译修复（无硬件，暂缓）、MCP 工具扩展、P2 三份 main 文件重复逻辑

---

## 2026-07-22（历史）

## Branch
`dev` (24 commits ahead of origin/dev)

## Today — JMicron USB-SATA bridge investigation + SMART path refactor

### JMicron JM20329 — SMART unreadable on Windows
- JMicron uses vendor-specific SCSI CDB, not standard SAT
- Windows usbstor.sys (BOT) blocks vendor-specific CDBs
- Standard SAT ATA PASS-THROUGH (0xA1) rejected by JMicron firmware
- **Not fixable on Windows** without kernel filter driver + WHQL signature
- User needs ASMedia/Realtek USB enclosure or direct SATA connection

### NVMe Health Log offset — confirmed via DiskGenius
- Power On Hours at **byte 128** (not 11 or 32 as previously guessed)
- `NVMe_HealthLog.h` now exists with all field offsets as constexpr (was referenced in docs but missing from tree)
- Byte 128 is the *standard* offset: NVMe counters are 128-bit fields; byte 56 is Host Write Commands (earlier "standard 56" belief was wrong)
- Fixed `SmartReader.cpp` top-level `powerOnHours` (was reading byte 32 = Data Units Read)

### SMART path order (final)
1. NVMe admin command (NVMe drives)
2. SCSI ATA passthrough (USB bridges with SAT)
3. SMART_RCV_DRIVE_DATA (native SATA — KINGSTON etc.)
4. Temperature fallback

### Files added/modified
- `src/core/disk/NVMe_HealthLog.h` — NVMe field offsets
- `src/core/disk/SmartReader.cpp` — multi-path SMART reading
- `docs/specs/nvme-health-information-log.md` — NVMe layout doc
- `docs/specs/jm20329-limitation.md` — JMicron analysis
- `docs/session/2026-07-22-jmicron-smart.md` — full session notes

### TUI concern noted
- Log panel compressed at bottom of single window
- Need to split into separate scrollable window (not yet implemented)

### Issues fixed
1. **F3 toggle brace fix** — reverted to `if (batteryAmperage != 0 && batteryVoltage > 0)` with proper bracket pair (fc573fa3)
2. **`maxContentRow` undefined** — moved divider loop after panels but forgot to define variable; added `int maxContentRow = rows - 5;`
3. **Arrow garbled** → `Log [^ N]` ASCII-safe for Windows console (23c8c25)
4. **NVMe powerOnHours wrong** (Samsung PM961 reporting 1.8M hours) — ATA translation layer returned garbage for attribute 0x09; fixed by trying NVMe health log FIRST, then ATA fallback
5. **Disk panel "Unknown"** — two causes:
   - `diskType = "Unknown"` when MediaType WMI property missing → infer from model name (SSD/HDD)
   - Display gated all data behind `smartSupported` → show health%/temp when available, "N/A" when not
6. **Disk display too minimal** — only showed health%; now shows temp, hours, wear leveling (c5ba5e03)

### Architecture insight
- Current DiskInfo.cpp uses Win32_DiskDrive WMI only (no MSFT_PhysicalDisk/CM API on Windows)
- SmartReader::Read called for each PhysicalDriveN
- ATA HDDs behind RAID/HBA controller on server fail SMART read → smartSupported stays false
- Without admin rights, `\\.\PhysicalDriveN` GENERIC_WRITE access also fails

## Known Issues
- openssl submodule not yet built into CMake (uses Homebrew openssl@3 on macOS)

## User Habits
- Zero warnings policy
- One change per commit
- Prefers sonnet or pro models for code changes
