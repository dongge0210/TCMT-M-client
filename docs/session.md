# Session State (2026-08-08)

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
