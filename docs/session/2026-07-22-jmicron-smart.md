# Session Notes 2026-07-22 — JMicron 与 SMART

## 今日发现

### 1. NVMe 健康日志偏移量 — 错误 → 修正 → 再修正

NVMe Health Information Log Page 中 **Power On Hours** 的偏移量：

| 阶段 | 偏移量 | 来源 | 结果 |
|------|--------|------|------|
| 初始 | 32 | 猜测 | ❌ 读错数据 |
| 第一次修正 | 11 | 猜测 | ❌ 还是错 |
| 第二次修正 | 128 | 用户 DiskGenius 输出 | ✅ 已确认 |

已通过 DiskGenius 输出确认：通电时间 = 14307 小时 (0x37E3) 位于 **字节 128**。

创建了 `NVMe_HealthLog.h` 存放所有 NVMe 健康日志字段偏移量，支持标准 (56) 和厂商特定 (128) 两种偏移。

**教训**：不要猜测偏移量，用真实工具（DiskGenius, smartctl）确认。

### 2. JMicron USB-SATA 桥 — 无法读取 SMART

**设备**：JMicron JM20329（USB 2.0 硬盘盒，BOT 协议）
**Windows 设备名**：`JMicron Generic SCSI Disk Device`

**根因**：
- JM20329 固件使用厂商特定 SCSI CDB，不是标准 SAT (SCSI/ATA Translation)
- Windows usbstor.sys (BOT) 不转发厂商特定 CDB
- 标准 SAT ATA PASS-THROUGH (12) opcode 0xA1 被 JMicron 拒绝

**尝试过的方案**：

| 方案 | 结果 |
|------|------|
| NVMe admin command (非 USB 桥) | ✅ NVMe 盘工作 |
| SCSI ATA PASS-THROUGH opcode 0x85 | ❌ JMicron 不支持 |
| SCSI ATA PASS-THROUGH opcode 0xA1 | ❌ JMicron 不支持 |
| SMART_RCV_DRIVE_DATA (原生 SATA) | ❌ 被 SCSI 路径抢先 |
| PawnIO 模块 | 理论上可行，但 Pawn 语言太弱，需写 xHCI 驱动 |
| 内核过滤驱动 | 需要 WHQL 签名，个人开发者成本高 |
| Linux smartmontools (`-d usbjmicron`) | 需要 Linux 环境，WSL 无法直通 USB 设备 |

**结论**：JMicron 在 Windows 下无法读取 SMART。用户需换 ASMedia/Realtek 硬盘盒或直接接主板 SATA。

### 3. 代码修复

修复了以下问题：

1. **KINGSTON SATA 误伤** — SCSI 路径抢先，原生 SATA 走不了原生 SMART_RCV_DRIVE_DATA
   - 修复：在 SCSI 路径之后加回原生 SATA 路径作为回退
   
2. **NVMe 全零缓冲区处理** — USB 桥返回成功但全零数据
   - 修复：检测全零，跳过 NVMe 解析，走 SCSI 路径

3. **重复标签** — `try_scsi` 标签重复定义
   - 修复：改为 `try_next_path`，NVMe 全零时跳转

**最终路径顺序**：
```
1. NVMe admin command     → NVMe 盘
2. SCSI ATA passthrough   → USB 桥（SAT 标准）
3. SMART_RCV_DRIVE_DATA   → 原生 SATA（KINGSTON 等）
4. 温度回退               → 仅温度，无 SMART
```

### 4. 新增文件

| 文件 | 说明 |
|------|------|
| `src/core/disk/NVMe_HealthLog.h` | NVMe 健康日志字段偏移量常量 |
| `docs/specs/nvme-health-information-log.md` | NVMe 健康日志布局文档 |
| `docs/specs/jm20329-limitation.md` | JMicron 限制分析和解决表格 |
| `src/core/disk/SmartReader.cpp` | 多路径 SMART 读取 |

### 5. TUI 日志窗口分离 — 已完成

**问题**：Log 面板挤在底部，内容多时被截断，无法同时查看硬件数据和日志。

**解决方案**：拆分为两个独立终端
- Terminal 1: TUI 显示硬件信息（无日志面板）
- Terminal 2: LogViewer 尾随显示日志文件

**新增文件**：
- `src/tui/LogViewer.h` / `LogViewer.cpp` — 日志查看器
- `scripts/start-dual-terminal.bat` — 双终端启动脚本

**使用方式**：
```
scripts\start-dual-terminal.bat
```

---

## 待解决

- [ ] 获取 JMicron 厂商特定 CDB（需 Linux + usbmon）
- [ ] 可能需要 PawnIO 模块支持 JMicron（长期）
- [ ] 验证双终端模式在 Windows 下的稳定性

### 问题

当前 TUI 布局将所有面板挤在一个窗口里：

```
┌─────────────────────────────────────────┐
│                 Header                  │
├──────────────────┬──────────────────────┤
│  CPU/GPU/RAM     │  Disk/Net/TPM/Temp   │
│  (左半部分)       │  (右半部分)          │
├──────────────────┴──────────────────────┤
│  Connections                            │
├─────────────────────────────────────────┤
│  Log (压缩在底部，内容多时被截断)        │
├─────────────────────────────────────────┤
│  System (Uptime/Load/Procs)            │
└─────────────────────────────────────────┘
```

**问题**：
1. Log 区域高度受限（`logSpace = logEnd - contentEnd - 2`），内容多时被截断
2. 无法同时看到硬件数据和日志输出
3. 日志滚动时无法回顾历史

### 建议方案

**方案 A：独立日志窗口**
```
┌─────────────────────────────────────────┐
│                 Header                  │
├──────────────────┬──────────────────────┤
│  CPU/GPU/RAM     │  Disk/Net/TPM/Temp   │
├──────────────────┴──────────────────────┤
│  Connections                            │
├─────────────────────────────────────────┤
│  System                                 │
└─────────────────────────────────────────┘

┌─────────────────────────────────────────┐
│  Log (独立窗口，可滚动/翻页)             │
│  ...                                    │
└─────────────────────────────────────────┘
```

**方案 B：Tab 切换**
```
[硬件监控] [日志]  ← Tab 切换
```

**方案 C：外部日志窗口**
- TUI 内保留最近几行关键日志
- 详细日志输出到文件或 `tmux` split

### 建议

**方案 A 最简单直接**：
- 将 `LogBuffer` 内容渲染到独立的 ncurses 窗口
- 支持上下滚动、翻页
- 按 `L` 键切换显示/隐藏日志窗口

### 文件影响

| 文件 | 改动 |
|------|------|
| `src/tui/TuiApp.cpp` | 重构 `Run()` 中的布局逻辑 |
| `src/tui/TuiApp.h` | 添加日志窗口相关方法 |
| `src/tui/LogBuffer.h` | 可能添加滚动/历史接口 |

---

## 总结

1. NVMe 健康日志偏移量已从 DiskGenius 确认（字节 128）
2. JMicron 在 Windows 下 SMART 不可读是平台限制，不是代码 bug
3. SMART 读取路径已重构为 4 步回退
4. TUI 需要日志窗口分离（待实施）
