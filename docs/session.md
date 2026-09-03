# Session State (2026-08-08)

## 2026-08-12 — 前后端分离：server / viewer 移出 client 仓库

- 结构变更：本仓库的 `server/`（Node 中转）提升为独立仓库/目录 `TCMT-M-server`
  （仓库根：`/Users/huangzhaoming/TCMT/`）；viewer 独立为 `TCMT-M-viewer`
  （纯前端，通过 CORS 调 API）。本仓库不再包含 server/viewer 代码。
- 后端升级（TCMT-M-server，零依赖）：SQLite 落盘（`node:sqlite`，
  默认 `~/.tcmt/server.db`）、`--retention-days`（默认 30 天）、
  `/history` 支持 `bucket` 秒级即时降采样、`--cors-origin`（默认 `*`）；
  不再托管静态页面（`/` 返回服务信息）；旧 `data/devices.json` 首次启动自动迁移。
- 前端（TCMT-M-viewer）：`config.js` 配置 API 地址（localStorage `tcmt_api` 可覆盖）；
  历史曲线支持 1h / 6h / 24h / 7d 时间范围 + 最多三条字段叠加，后端降采样不卡。
- C++ `TCMT-M-server` v0.1.0 原型已由 Node 实现替换（删除源码与已入库的 build 产物，
  git 历史保留，可随时找回）。

## 2026-08-11 追加 — server + viewer（跨设备，纯网络）

### 目标
- 角色划分：client（TCMT-M）负责采集 → **server** 中转/整理/托管页面 → **viewer** 纯展示
- 跨设备、仅通过网络连接（client/server/viewer 可以在不同机器）

### 新增（两个独立新文件夹）
- `server/`（Node.js，零依赖）：兼容现有 `ServerProbe` 协议（`POST /api/register` +
  `/api/ingest`，端口 8080），另加：
  - 设备幂等注册（同主机名复用同一设备，重启不膨胀列表）
  - 内存环形历史（每设备 1800 条 ≈1h @1Hz）、字段索引、派生 summary/temperatures
  - REST：`/ping` `/api/devices` `/api/devices/:id[/latest|summary|fields|history|temperatures|任意字段]`
  - WebSocket `/ws`：设备列表 0.5s 广播 + snapshot 实时推送（stdlib 实现 RFC-6455）
  - 默认监听 `0.0.0.0`，启动打印 LAN URL；`--host`/`--port`/`--data-dir` 可配
- `viewer/`（原生 HTML/CSS/JS，无构建）：多设备总览条（同屏所有设备：在线/CPU/内存/GPU/温度，
  实时更新），点击卡片或下拉切换详情；详情含仪表盘、运动传感器、温度列表、字段索引、历史曲线
- 结构定稿：三个端（client / server / viewer）都在 client 主仓库内；**viewer 是独立展示端，
  随 server 保留在 `server/viewer/`**（server 自带托管，`server.js` 默认从 `./viewer` 找页面，
  `--static-dir` 可覆盖）

### client 改动（macOS）
- `ServerProbe` 不再硬编码 127.0.0.1:8080：解析 URL（支持 IP/主机名/base path），
  `RawPost` 改用 `getaddrinfo` 支持主机名
- `main_mac.cpp` 新增 `--server <url>` 参数（环境变量 `TCMT_SERVER` 兜底）
- **修复推送全 0 bug**：`data` 每帧重建，CPU/内存/GPU 只在 heavy frame（1Hz）填充，
  而 ServerProbe/HistoryLogger 每帧都推 → 大部分是 0；两处均改为 `isHeavyFrame` 时才推

### 验证（本机）
- 模拟两设备（MacBook/Desktop）注册+推送，REST/WS/静态页面全部通过；token 不入 latest
- 真实链路：`./build/src/TCMT-M --http --server http://192.168.226.125:8080`
  注册成功（复用 dev_06b876）、快照实时更新（CPU 31.7% / 内存 11.5/16GB / lid 112° / hb 206）
- NAT/公网：server 支持 `--auth-token`（读接口+WS）、`--tls-cert/--tls-key`
  （HTTPS/WSS）、`--public-url`；client 支持 `https://`（CMake 自动找 OpenSSL 3.6.2）、
  `--server-insecure`、8s 超时、401 自动重注册、`~/.tcmt/client.json` 持久化身份
  （clientKey，不依赖 IP/主机名）。实测：HTTPS 端到端 + wss 握手 + 重启复用同设备 id
- 注意：读接口默认无鉴权，公网务必开 `--auth-token` + TLS

### 待办/注意
- Windows `main.cpp` 未接入 ServerProbe（需 Winsock 移植）
- 历史目前仅在内存（重启清空）；需要长期落盘可加 JSONL/SQLite
- TLS 仅在 macOS/Linux 客户端编译（`find_package(OpenSSL)` 未命中时退化为纯 http）
- `--json`/MCP 等模式不受影响

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
- **电池健康修复**：Apple Silicon 上 `AppleSmartBattery` 的容量/温度字段在嵌套 `BatteryData` 子字典里，顶层读不到导致 Health 0%。改为优先读 `BatteryData`（顶层回退），`AppleRawMaxCapacity` 缺失时用 `FullChargeCapacity`——M2 Air 实测 252 循环 Health 82.4%
- **SSID 定位授权流程（先判断后请求）**：去掉自动 `requestWhenInUseAuthorization`（会绕过用户直接授权并读取 SSID）。现在 TUI 先显示判断/引导（英文：Location not granted, SSID hidden / Press R to request / System Settings 路径），用户按 `R` 才触发一次性授权请求；授权前 CoreWLAN 与 system_profiler 的 SSID 读取全部禁用

### 验证
- `cmake --build build -j8` 全绿；`TCMT-M.app` 正常生成（未 codesign）
- `--json` 单次快照正常输出（CPU/磁盘等）
- TUI 双页面（dashboard / log Tab+l）正常，进程面板、磁盘健康（SSD 97% 33C 1713h）正常
- `q` 退出干净：`Exiting` → `HistoryLogger stopped` 同秒完成，进程 2.6s 退出 code 0
- 本机 exec 环境无 WindowServer（CGMainDisplayID=0）：日志窗口与显示信息自动降级，TUI 持续运行 12s+ 无崩溃，`q` 退出 code 0（AppKit 窗口本体需用户真实终端会话验证）
- WindowServer 恢复后实测：启动时无 SSID + 引导显示；按 `R` 授权后 SSID 正常出现（Pattison B_5G）

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

## 2026-09-03 — TUI 评审修复（按优先级，7 commits on dev）

评审来源：OpenDesign 会话对 `src/tui/TuiApp.cpp` 的完整设计评审（布局/配色/UX/可发现性/地板尺寸）。

1. `fix(tui)` 2032efab — **80×24 地板越界覆盖**：面板画入保留底部带（Connections/System/底边框）的残留行被清除后再重绘保留带，底边框最后重绘；分隔线限定在内容区（rows 2..rows-8）；右列重排 Temps/Power 优先于 Displays/Accel/TPM。
2. `fix(tui)` b2a88049 — **日志级别着色**：按固定位置解析 `[ts][LEVEL]` 级别标签（原来整行子串匹配，CRITICAL/FATAL 显示为绿色）；INFO 恢复默认前景色。
3. `fix(tui)` bbfac28f — **Esc 语义**：Log 页 Esc 返回 dashboard（不再退出整个程序），dashboard Esc 惰性；Home=最旧条目、End=最新跟随。
4. `perf(tui)` 06e89338 — **Log 页脏检查**：用 `LogBuffer::Version()`（原未被 TUI 使用）+ 终端尺寸 + 按键门控重绘；设置页 10ms→30ms。
5. `fix(tui)` baea3c58 — **按键提示平台化**：Windows 不再提示死键 L；U/R 有 handler 才提示；标题去掉 `[WxH]` 调试残留。
6. `fix(tui)` b532eb45 — **Temps 网格自适应**：行数随可用空间（上限 8 行），够高不翻页；翻页索引 shrink guard。
7. `style(tui)` b1c5a336 — 删顶部双横线；进程行只染 CPU% 单元格；WiFi 无适配器显示 `n/a`；`Processes (PID Monitor)` → `Top Processes`；日志空状态文案统一英文。

验证：macOS `cmake --build build --target TCMT-M` 全量通过（仅 openssl 版本无关警告）。未做：Windows PDCurses 构建验证（改动均用通用 curses API）、`?` 帮助页、NO_COLOR/ASCII 开关、per-core 宽度问题（待后续）。

## Known Issues
- openssl submodule not yet built into CMake (uses Homebrew openssl@3 on macOS)

## User Habits
- Zero warnings policy
- One change per commit
- Prefers sonnet or pro models for code changes
