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

## 2026-09-05 — TUI UX 升级（7 commits on dev，8ec00460..2efda975）

评审三波方案中用户选定 10→11→3→2→1→5→6 逐条落地，每项独立提交、逐项增量编译通过，最后 PTY 冒烟验证。

1. `refactor(tui)` 6d0d8441 — **#10 平台能力层 PlatformCaps**：native/inline 日志窗口、resize、WiFi Location Services 等能力集中一处 `DetectPlatformCaps()`（本文件唯一 #ifdef 决策点），键位/页面/提示文案全部改读运行时 caps_；内嵌日志页全平台编译、按 `caps_.inlineLogPage` 门控。顺带修复 Linux WiFi 已连接时误显示 macOS "Location not granted / Press R" 引导的真实 bug。
2. `feat(tui)` af31d04d — **#11 NO_COLOR / ASCII 降级**：`NO_COLOR`（非空）跳过 `start_color`/`init_pair`，curses 视 COLOR_PAIR 为惰性，全 UI 零色码；`TCMT_ASCII=1` 或非 UTF-8 locale 时 sparkline `▁..█`→`. :-=+*#`、核心温度 `°`→`C`、合页角 `°`→`deg`。两者在 Run() 启动时读一次。
3. `feat(tui)` 5f54e457 — **#3 stale 指示器**：`UpdateData` 打 steady 时钟时间戳，3 s 无新快照 → 状态行左侧黄色 `stale Ns` 芯片（监控停摆不再静默冻结画面）。冒烟中数据源被环境卡死时正确点亮。
4. `feat(tui)` 76e2ce6a — **#2 `?` 帮助页**：按 Common/Dashboard/Log page 分组列全键位，Esc/q/? 关闭（帮助内 q 不退出程序）；键列表与提示行同一套 caps_+handler 门控。同时修复 80 列下提示行与居中标题相撞而整体消失的问题：提示行右对齐 row 0，窄终端回退 row 1 右侧，更新横幅在 row1 剩余区间居中、stale 芯片锚定左侧，三者互不重叠。
5. `feat(tui)` b686878d — **#1 进程列表焦点**：↑↓ 选中（按 pid 跟踪，列表重排/进程退出自动清理），选中行纯 reverse video（抑制单元格色），Enter 弹进程详情（PID/名称/CPU/内存，Esc/Enter/q 关闭）。
6. `feat(tui)` 48781bdd — **#5 语义色 + 单位口径**：阈值集中成表（HighIsWorsePair/LowIsWorsePair + 命名阈值常量），正常值回默认前景（不再满屏绿），只染越界值（温度/健康/电量/进程 CPU）；新增十进制 `FormatRate`（1000 进制 B/s）替换网络 D:/U: 行的二进制 FormatSize——同一屏 "MB" 不再双基数。
7. `feat(tui)` 2efda975 — **#6 数据栏 + 数值列**：usage bar 彩色填充（正常蓝、越 70/90 黄/红）+ `A_DIM` 空轨（主题默认前景的骨架感）；`DrawUsageBarRow`/`DrawLabeledValue` 让面板内 label 左、值右对齐共一条右边线；Temps 标签定宽补齐。

PTY 冒烟（`script` 驱动，120×40）：默认色模式色码正常（黄/红越界、蓝 bar、dim 轨）；`NO_COLOR=1` 只剩 bold/dim、零色码；C locale 自动 ASCII（`^/v`、`deg`）；`LANG=en_US.UTF-8` 恢复 ↑↓；`?`→Esc→↓→Esc→q 全程无崩溃、退出码 0。未做：Windows PDCurses 构建验证（改动均用通用 curses API + 既有宏）。

## 2026-09-21 — 采集端补全：磁盘 / Wi-Fi / 蓝牙（由设计稿反推的字段修正）

起因：设计稿硬件页画的行，有几处采集端给不出真值。决定是**修采集端，不砍设计**，并要求
「尽量压缩，一个字段能干很多事」。核心结论：**净增 1 个字段**就补齐了 —— 靠复用两个早就存在、
却既没有生产者也没有读取者的死字段。

1. `chore(avalonia)` — `_archive/` 排除编译。`_archive/README.md` 声称「不参与构建」，但 SDK 式
   csproj 默认通配把它连 `.axaml` 一起编了进去；`StorageViewModel` 引用的 `Letter` /
   `LogicalDriveLetters` 正要改签名，先排掉。`.cs` 与 `.axaml` 必须**成对**排除，只排一半会留下
   没有 code-behind 的 XAML。
2. `fix(disk)` — macOS 卷筛选改用挂载标志。原来是 `DiskInfo.cpp` 里一张越加越长的路径名单，
   漏掉 `/Volumes/Recovery`、CoreSimulator 的 cryptex 挂载与模拟器卷。改成 `MNT_LOCAL` + 非
   `MNT_DONTBROWSE`，只显式放行 `/System/Volumes/Data`；并删掉按物理盘去重（它误伤同盘第二卷）、
   一个 `<100MB` 过滤、一个空壳的 `Skip boot partition` 判断。
3. `fix(disk)` — 卷名取真值。原来是挂载路径末段，模拟器卷会变成一长串 UUID。改用
   `getattrlist(ATTR_VOL_NAME)`。**两个坑都踩过**：返回的是 `attrreference_t` 而不是「长度 + 字符串」，
   且 `attr_length` 含结尾 NUL（不剥掉会把整份日志变成 grep 认定的大二进制文件）。实测字节记在
   函数注释里。
4. `feat(disk)` — `mountPoint` 上线。Win32 把盘符、卷 GUID 路径、挂载文件夹都算 volume mount point
   （MS 文档 Naming a Volume），所以「挂载点」不是生造的中性词。`DiskSlot` 新增 `mountPoint[64]`，
   两平台写入 + 两处 schema 注册 + C# 读侧与模型。macOS 启动卷组只报数据卷。
5. `fix(net)` — Wi-Fi `band` / `gen` 补注册。采集端一直在收、写入点一直在写、Windows 侧一直有注册，
   **只有 macOS 的 schema 构建漏了两行**，于是 C# 侧 `?? ""` 永远读到空串。
6. `feat(bluetooth)` — `BtSlot.name` 复活。该槽在 macOS 装的是适配器名、Windows 空着、两端都没人读。
   改成装已连接设备名；拼装逻辑抽到 `BluetoothInfo.h` 的 `JoinConnectedDeviceNames` 由两平台共用
   （逐段拼、装不下就停，不在 UTF-8 字符中间截断）。`SystemInfo` 加 `btDeviceName` 只为 Windows 过路。

验证：macOS `cmake --build build -j8` 0 error 0 warning（仅既存的 Homebrew dylib 版本链接警告）；
`dotnet build` 0 警告 0 错误。实跑 `--debug` 确认磁盘只报 `/System/Volumes/Data` 且卷名为真值 `Data`；
连 `/tmp/tcmt_ipc.sock` 读发布的 schema，确认 `disk/{0..3}/mountPoint`、`wifi/band`、`wifi/gen`、
`bluetooth/name` 都在线上。

**顺带发现**：schema 字段数 **284**，上限 `IPC_MAX_FIELDS = 300` —— **只剩 16 个余量**，超了会在
`IPCClient.cpp:184` **静默截断**（无任何报错）。已在 `IPCServer::UpdateSchema` 加一行 DEBUG 记录字段数，
下次加字段前先看这个数。

**未做**：磁盘「分区」在 macOS 侧需要 APFS 容器（`disk3`）→ 物理盘（`disk0`）映射，得走 IOKit 设备树
或 `diskutil`；Windows 侧 `partitionLabels` 的填充；以及 Windows 本地构建验证（本机编不了，靠 CI）。

## 2026-09-22 — 硬件视图落地、两处 JSON 打包、MCP 浮点读取宽度、网卡修复

### 1 · 「硬件」那一屏搬进应用（设计稿 → AvaloniaUI）

设计稿里的「硬件」分区（左栏可展开组 + 十个部件页）此前**只存在于 HTML**。这次做进应用：

- `Models/HardwareSpec.cs` — `HardwarePart` 枚举 + 显示名/顺序 + `SpecSheet`/`SpecBlock`/`SpecUnit`/`SpecRow`/`SpecTable`
- `Core/HardwareSpecBuilder.cs` — `SystemInfo` + 部件 → 规格表，十页各一个方法
- `ViewModels/HardwareSpecViewModel.cs` — **一个类十个实例**；写十个 VM 要配十个 DataTemplate，加一个部件动四处
- `Views/HardwareSpecView.axaml` — 一个视图
- `MainWindowViewModel` / `MainWindow.axaml` — `NavigationItem` 加 `Children`/`IsExpanded`/`ActivateCommand`；
  子项不在 ListBox 的 Items 里，所以选中走命令。ListBox 只管顶层项

**版式**：一页**一张卡**，块间细分隔线。按块各开一张卡的话，只有三四行的页（电池、内存）会变成飘在空白里的小方框。

### 2 · 传感器与进程都打包成 JSON（字段预算救回来了）

`IPC_MAX_FIELDS = 300`，客户端超了会**静默截断**（`IPCClient.cpp:184`）。原来按槽位开字段，撞墙：

| | 之前 | 之后 |
|---|---|---|
| 传感器 | `TempSlot temperatures[10]`，每槽 2 字段 | `sensorsJson[4096]`，**1 个字段**，上限消失（采集端实际产 20 个，原来砍一半） |
| 进程 | `ProcSlot topProcesses[7]`，每槽 4 字段 | `processesJson[16384]`，**1 个字段**，采集端保留数 7/8 → **128** |

**schema 字段数 284 → 265（传感器）→ 237（进程）**，余量 16 → 63。
装法沿用 SMART 属性表既有的 `attrsJson`。另在 `IPCServer::UpdateSchema` 加了一行 DEBUG 记录字段数 —— 这个上限超了没有任何报错，不主动记就只能靠猜。

**顺带**：进程原来是按**内存**挑前 N、界面按 **CPU** 排，所以"CPU 前几名"其实只是"内存前几名里的 CPU 排序"。**这条没改**，需要时再定口径。

### 3 · MCP 浮点读取宽度 —— 三平台 17 处报给 AI 的都是垃圾

`ReadNumeric<T>` 里是 `memcpy(&val, …, min(sizeof(T), f->size))`。用 `ReadFloat64` 读一个 **4 字节的 float**
不会失败：低 4 字节填 float 的位、高 4 字节留 0，拼出来是个**非规格化 double**（如 `5.49e-315`）。
既不是 null 也不是合理值，`value_or()` 之类兜底完全拦不住。

修前 `get_gpu_status` 的温度是 `5.49e-315`，修后是 `59.65 °C`；`get_cpu_status` 的 usage/温度/频率同理。

修法：加 `IPCClient::ReadFloat()`，按字段实际宽度自适应，17 处换过去。**这是给 AI 消费的接口约定，报垃圾比报错更糟。**

### 4 · 网卡两处

**(a) 类型被截断** —— `char type[16]` 装不下代码自己产出的 `"Wireless adapter"` / `"Ethernet adapter"`
（都恰好 16 字符），界面上少最后一个字母。加宽到 24。

**(b) 硬件 MAC 取不到** —— 采集端用 `getifaddrs` 的 `AF_LINK` 项，而这台系统（macOS 27 / Darwin 27.2）
一律返回 `02:00:00:00:00:00`。改用 **SystemConfiguration** 的
`SCNetworkInterfaceGetHardwareAddressString`（纯 C API，框架本就已链接）。
**坑**：这个 SDK 里**没有独立的 `SCNetworkInterface.h`**，声明被并进了 `SCNetworkConfiguration.h` ——
用伞头能编过、精确 include 编不过。

### 5 · 「当前链路地址」：六条路五条不通，第六条被签名挡住

macOS 的「私有 Wi-Fi 地址」按 SSID 轮换，与硬件地址是两个值。实测：

```
getifaddrs              → 02:00:00:00:00:00
sysctl(NET_RT_IFLIST)   → 02:00:00:00:00:00（沙箱外同样，已排除沙箱）
SIOCGIFADDR             → 给的是 IP，不是链路地址
IOKit IOMACAddress      → 属性不存在
SIOCGIFLLADDR           → 这版 SDK 里没有
SystemConfiguration     → 硬件地址（另一个值，不随 SSID 变）
```

最后一条路是 shell 到 `ifconfig`，**但被签名挡住了**：同一条命令、同一时刻，
Apple 签名的进程链里跑 `ifconfig` 拿到真值，而未签名的本地构建（`cc` 出来的即席签名二进制，
**经 launchd 起也一样** —— 责任进程仍是那个未签名程序）调起 `ifconfig` 只会拿到全零。

**结论**：TCMT 采集端就是未签名的本地构建，这条路对本项目无效，代码里没留 —— 留着只是每 10 秒起一次进程换个空值。
`currentMac` 这条线（线格式字段 → C# → 界面/TUI）**保留**，但 macOS 上保持为空、界面报 N/A，
**不拿硬件地址冒充**。真想要这个值得先把采集端签名。

### 6 · 顺带

- 概览页新增 **GPU 面板**（设计稿与应用的栅格都改过：12 栏那侧变成 `CPU(4) GPU(4) 内存(4)` + `电源(3) 温度(3) 网络(3) 磁盘(3)`）。
  GPU 占用本来就采得到（IOAccelerator 的 `Device Utilization %`），坏的是上面第 3 条的读法。
- 流程修复若干：`_archive/` 排除编译、进程页每 tick 重建导致的爆闪、重连后状态被上一代连接覆盖、
  字体能力标志与实现对齐、每 tick 的 SMART 日志清掉。

## 2026-10-13 — plan.md 审计落地（dev 上 ccb1a2d7..8786b14a）

2026-10-12 的审计报告 plan.md 是临时文件、不入库；此处记录其中「无需再决策」的部分已逐条落地。
`cmake --build build -j8` 连续通过，只有既有的 ncurses ld 版本警告。

### 真 bug（用户能看到的）

- **macOS 功率回退断路**。`ModuleCoordinator` 的温度循环注释写「falls back to powermetrics globals」，
  但 `else` 分支只抄了两个核心频率、没读三个功率，非 direct 模式下界面显示 0 W。
  `GetPmCpuPower/GpuPower/AnePower`（`TemperatureWrapper.cpp`）此前全项目零调用。已接上（单位均为 mW）。
- **`EnergyToPower` 的「T = 1 秒」隐含假设**。函数返回焦耳，调用点乘 1000 当作 mW，前提是采样周期恰好
  1 秒；实测循环是 1.00~1.10 秒，功耗被系统性高估 2~5%（负载越高越偏）。改成 `steady_clock` 实测间隔
  再除，函数更名 `EnergyJoules`，单位写进名字。
- **Intel OOBMSM 每 2 秒 2 条 INFO**。`ReadAll()` 由 `GetTemperaturesImpl()` 每 2 秒调一次，成功路径打
  BAR0 magic 与首个 PMT entry，约 8.6 万行/天。5 条探测类日志全部降到 DEBUG（规范里「探测 = DEBUG」）。
- **`tcmt-powerd` 的 `WriteShm` 吞掉 `write()` 返回值**。写盘失败时守护进程照常报「在发布」、客户端
  `shmValid_` 保持 true，界面是一个冻结的功耗值且无任何信号。现在检查 lseek/write，且一段连续失败里
  只记第一条。
- **`acOnline` / `batteryPercent` 的旧读覆盖新事件**。三个写者（`PowerLoop` 的 2 秒轮询、ETW 电源/电池
  回调、`SystemEventMonitor` 电源回调）纯 last-writer-wins，一次开始于事件之前的读可能落在事件之后，
  把拔插电源的新状态盖回旧值。`PowerLoop` 现在在 `Detect()` 之后检查两个 dirty 标志，若事件在读取期间
  落地就跳过本轮写入（事件回调存的就是变更后的状态，必然更新）。
- **`Platform_macOS.cpp` 伪造 SMC**。`OpenSMCConnection()` 丢掉 `dlopen` 结果、把句柄设成 `(void*)1`
  并 `return true`；`ReadSMCKey()` 对 `"TC0P"` 返回 45.0f、对 `"TG0P"` 返回 55.0f，都带成功返回；
  `GetIOKitService()` 对任何名字给假句柄。调用方无法与真实读数区分。现在一律诚实失败（`false` /
  `nullptr`）。该文件未进任何构建，所以是拆了引信的哑雷；`clang++ -fsyntax-only -DTCMT_MACOS` 干净。

### 平台与结构

- 三个 `.mm`（WiFiInfo / DisplayInfo / BluetoothInfo）够不着的 `#else` stub 换成 `#error`；
  `WiFiInfo_wlan.c` 同样处理（第四例）。
- `IOReportSampler.mm` 移到 `src/core/power/PowerMonitor_mac.mm`。`src/core/ioreport/` 只有它一个文件，
  且它就是 `PowerMonitor` 的 macOS 实现，`PowerMonitor.cpp` 的头注释早就这么指了。空目录已删。
- `LogBuffer.h` 从 `src/tui/` 挪到 `src/core/Utils/`。`Logger.h` include 它、而约 55 个文件 include
  `Logger.h`，整棵 `src/core` 因此传递依赖 `src/tui`（headless 的 `TCMT-M --mcp` 也要链 ncurses）。
  便宜那半做完；贵那半 `Snapshot(SystemInfo&, TuiData&)` 的签名没动，要设计决策。
- 调试设施改为 opt-in（`TCMT_IOREPORT_DEBUG=1`）。原先每次 `Start()` 都无缓冲写固定的 /tmp 文件、
  且 dump 在采样循环内，等于往「功率的分母」里加 write 系统调用。
- 删掉 `ReadShmPower` 里那句 `__sync_synchronize()`（下面的原子写本来就是顺序一致），
  以及 `value == INT64_MIN` 死条件。

### CI

- **`build.yml` 的 `push` 触发被注释掉**，历史 60 次运行全是 `pull_request`。Actions 缓存按 ref 作用域，
  PR 运行写进 `refs/pull/N/merge`，别的分支读不到，于是永远没有分支级缓存，每个新 PR 冷跑多约 9 分钟。
  放出 `push: [dev]`。
- **ccache 路径用了 Linux 默认值**（`~/.cache/ccache`），ccache 4.x 在 macOS 上写 `~/Library/Caches/ccache`，
  所以 restore/save 从未命中（自报 `Cacheable 48/48, Hits 0/48`）。给 macOS job 加 `CCACHE_DIR`。
- **产出 3 个 artifact 应为 2 个**。两个 build job 各自 upload 未打包的中间产物，只为让第三个 job 去 zip。
  改成各自打包上传（`TCMT-Windows-x64-alpha` / `TCMT-macOS-arm64-alpha`），删掉 `package-artifacts` job。
- 3 处 MSVC C4267 收窄（`IPCServer.cpp:360`、`ServerProbe.cpp:77/:129/:131`）加显式 cast，
  就是 Actions 网页上那个「7 warnings」。Windows-only，本机验不到。

### 文档

- `CLAUDE.md`：数据流图与 IPC 那两节把 `IPCDataBlock` 说成「两块、偏移随平台变」。全仓
  `SharedMemoryBlock` 零命中，其实只有一块，平台差异在传输不在布局，恰好把这个项目在 IPC 上做得最对
  的决定说成了反面。`src/DataStruct.h` 路径不存在（真身 `src/core/DataStruct/DataStruct.h`）；
  `AvaloniaUI/Services/IPCServices/` 不存在（是 `Services/Hardware/IPC/`）。
- 删掉整节「CsWin32 / P/Invoke Rules」：LibreHardwareMonitor 子模块已由 `d98e601f` 移除，且全仓已无
  CsWin32 包、无 `NativeMethods.txt`、无 `PInvoke.` 调用。`Key Dependencies` 同步去掉它。
- `IPCSchema.cs` 注释指向不存在的 `IPCSchema.h`，改成 `IPCData.h`。
- 给「每个源文件必须三处登记」补两个例外：`src/tools/` 是按需手编工具、故意不登记；漏登记的失败症状
  （链接错误）只在符号被引用时出现，零引用时是静默的。
- 注明 `accel/` + `als/` + `MotionHTTPServer`（`127.0.0.1:9876/sensors/motion`，`motion_http.enabled`
  默认 false）是实验性、不是产品面，免得下一个人再判它是死的。
- 历史错误表新增 4 行。

### 引用偏差（按「引用有偏差就更新」扫了一遍）

脚本把 `src/`、`AvaloniaUI/`、`docs/`、`tools/`、`.github/` 里注释与文档提到的文件名逐个核对是否
存在，剔除 SDK 系统头之后的真命中：

- **`WMIManager.h` 被三处写成 `WmiManager.h`**（`PowerInfo.cpp:12`、`ModuleCoordinator_io.cpp:8`、
  `GpuInfo.cpp:6`）。macOS / Windows 大小写不敏感，所以从没报错；Linux 大小写敏感会直接编译失败。
  已改回正确大小写。
- `tcmt-powerd_main.cpp:9` 的头注释仍写 `IOReportSampler.mm`（这次移动文件时漏改）。已改。
- `docs/macos27-power-and-tcmt-powerd.md:62` 指向已不在仓库里的
  `.claude/plans/encapsulated-gathering-cookie.md`。指针已删。
- `.github/agents/tcmt-dev.agent.md` 提到的 `.notes/journal.md` / `.notes/insights.md` 是按设计由 agent
  自建、且被 .gitignore 忽略的，不算偏差。

### plan.md 自身的错处（读到哪个记哪个）

- **A4**「回调里的 `batteryPercent` 被丢掉」不成立。`EtwMonitor` 的 `PowerEventCallback` 是
  `void(bool acOnline)`、`BatteryEventCallback` 是 `void(int percent)`，两条独立且都写了值；
  `SystemEventMonitor` 那条 `void(bool, int)` 也写了 percent。只有 `PowerLoop` 存 `batPct` 时不设下限
  （可为 -1），这才是「缝法不一致」的真身。
- **A1** 不是「每 2 秒 5 条」：identity / VID-DID / DVSEC 三条在 `if (!s_probed)` 一次性块内，只有
  BAR0 magic 与 first PMT entry 是每轮，实际约 8.6 万行/天。
- **A3** 说「错整整两秒」也偏高：事件路径会置 dirty 标志、而 `PowerLoop` 每轮结尾 `exchange(false)` 读到
  就跳过 sleep 立刻重读，真实窗口只有一次 `Detect()`。
- **C16** 说「三份 main 的 schema builder」，实际两份（`main_mac.cpp:78`、`main.cpp:635`）；
  `main_linux.cpp`（749 行）里没有任何 schema 代码。
- **D24** 括号里说 `HardwareSpecBuilder.cs` 提到不存在的 `NetworkAdapter.cpp`，但
  `src/core/network/NetworkAdapter.cpp` 存在（31KB，登记在 `CMakeLists.txt:31`）。
- **AC1** 说「升 v2」不可行：`ilammy/msvc-dev-cmd` 没有 v2，最新 tag `v1.13.0` 的 `action.yml` 仍是
  `using: node20`。

### 仍未决策

- `tcmt_sensor_helper.c` 的 `kIOMasterPortDefault`：改 `kIOMainPortDefault` 加守卫（`.c` 里要用
  `__builtin_available`），还是承认 pragma 是既定做法。全仓另有 7 处 pragma 压制，口径要一次定。
- `main` 落后 `dev` 303 个提交（合并 PR #98 才能止血 Dependabot）。
- 两份 schema builder 合一、线格式能否表达列表（设计决策）。
- Linux 还算不算支持目标；`127.0.0.1:9876/sensors/motion` 有没有消费者。
- `docs/repo-directory.md` 是生成物（生成器 `tools/gen-repo-directory.mjs`），里面对 `AGENTS.md`
  的描述要重跑。
- `ilammy/msvc-dev-cmd` 的 node20 弃用：只有替换 action 才能消掉。

## Known Issues
- openssl submodule not yet built into CMake (uses Homebrew openssl@3 on macOS)

## User Habits
- Zero warnings policy
- One change per commit
- Prefers sonnet or pro models for code changes
