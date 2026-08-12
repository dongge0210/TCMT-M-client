# JM20329 USB-SATA Bridge — SMART 读取限制

## 芯片规格

| 特性 | 说明 |
|------|------|
| USB 接口 | USB 2.0 High-Speed (480Mbps) |
| SATA 接口 | SATA II (3Gbps) |
| 传输协议 | Bulk-Only Transport (BOT)，**不支持 UASP** |
| ATA 支持 | ATA/ATAPI PACKET command set, LBA48, 4K 扇区 |
| 封装 | 48LQFP / 48QFN |

## Windows 下 SMART 不可读的原因

**根因**：JM20329 固件实现的是**厂商特定协议**（非 SAT SCSI/ATA Translation）。

- 标准 USB Mass Storage BOT 协议只暴露 `INQUIRY`, `READ`, `WRITE`, `TEST_UNIT_READY` 等基础命令
- SMART 需要**厂商特定 SCSI CDB** 才能穿过 BOT 到达 ATA 层
- Linux 有 `usb-storage` 内核模块的 `jmicron` quirk driver，能发厂商特定 CDB
- Windows `usbstor.sys` **没有 JMicron quirk**，无法发送厂商特定命令

## 解决方案

| 方案 | 可行性 | 说明 |
|------|--------|------|
| 换 ASMedia 硬盘盒 | ✅ 推荐 | ASMedia ASM1153E 支持标准 SAT，15-30 元 |
| 换 Realtek RTL9210 | ✅ 推荐 | 支持 UASP + SAT，20-50 元 |
| 直接接主板 SATA | ✅ 最佳 | 原生 SATA，无桥接芯片延迟 |
| 写 Windows filter driver | ❌ 不现实 | 需要 WHQL 签名，复杂度高 |
| 在 Linux 下用 smartctl | ✅ 跨平台 | `smartctl -d usbjmicron,0 /dev/sdX` |
| Wine + Linux smartctl | ⚠️ 可能 | 未测试 |

## 受此影响的设备

- JMicron JM20329（USB 2.0 硬盘盒，最便宜的那些都是）
- JMicron JM20337
- JMicron JMS56x (部分固件版本)

**识别方法**：设备管理器 → 磁盘驱动器 → 属性 → "JMicron" 或 "USB TO SATA" 或 "Generic SCSI Disk Device"
