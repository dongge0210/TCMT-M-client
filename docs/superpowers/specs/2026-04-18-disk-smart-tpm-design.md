# Disk SMART 和 TPM 2.0 监控设计文档

## 概述

本文档描述 TCMT Windows 客户端的磁盘 SMART 数据和 TPM 2.0 监控功能实现。

### 目标

1. **磁盘 SMART**: 使用 LibreHardwareMonitor 收集真实的磁盘健康数据
2. **TPM 2.0**: 使用 tpm2-tss 监控 TPM 芯片状态

---

## 方案选择

### 磁盘 SMART: LibreHardwareMonitor

理由:
- 项目已有 LibureHardwareMonitor 子模块依赖
- 已有成熟的 .NET SMART 实现
- 支持 SATA/NVMe/SSD/HDD
- 新增代码量最少

### TPM 2.0: tpm2-tss + TBS TCTI

理由:
- 使用已有的 tpm2-tss 子模块
- 标准 TPM 2.0 软件栈
- TBS (TPM Base Service) 是 Windows 原生 TPM 接口

---

## 数据结构变更

### 1. 添加 TPM 数据结构

在 `DataStruct.h` 中添加:

```cpp
// TPM 信息
struct TpmInfo {
    wchar_t manufacturer[32];           // TPM 制造商名称
    uint16_t vendorId;                  // 供应商 ID
    wchar_t firmwareVersion[32];        // 固件版本
    uint8_t firmwareVersionMajor;
    uint8_t firmwareVersionMinor;
    uint8_t firmwareVersionBuild;
    uint32_t supportedAlgorithms;       // 支持的算法
    uint32_t activeAlgorithms;          // 激活的算法
    uint8_t status;                     // TPM 状态 (0=OK, 1=ERROR, 2=DISABLED)
    uint8_t selfTestStatus;              // 自检状态
    uint64_t totalVotes;                // 总投票数
    bool isPresent;                    // TPM 是否存在
    bool isEnabled;                   // TPM 是否启用
    bool isActive;                     // TPM 是否激活
};
```

### 2. 更新 SharedMemoryBlock

在 `SharedMemoryBlock` 中添加:

```cpp
// TPM 信息（单个 TPM）
TpmInfo tpm;

// TPM 数量
uint8_t tpmCount;
```

### 3. 更新 SystemInfo

添加 TPM 向量:

```cpp
std::vector<TpmInfo> tpms;
```

---

## 实现细节

### 磁盘 SMART 实现

#### 方案: 通过 LibreHardwareMonitor 扩展

1. **扩展 LibreHardwareMonitorBridge**
   
   新增方法:
   ```cpp
   static std::vector<PhysicalDiskSmartData> GetPhysicalDisks();
   ```

2. **实现流程**
   ```
   C++ 调用 → LibreHardwareMonitorBridge → .NET Hardware → Storage Sensors → SMART 数据
   ```

3. **关键传感器类型**
   - Temperature (温度)
   - Level (健康百分比)
   - Data (写入/读取字节数)

#### 现有代码集成

当前 `DiskInfo.cpp` 已实现:
- Win32_DiskDrive 查询（基本磁盘信息）
- 盘符映射逻辑

新实现将:
- 保留现有 WMI 代码获取磁盘基本信息
- 调用 LibreHardwareMonitor 获取 SMART 数据
- 合并到 PhysicalDiskSmartData 结构

---

### TPM 2.0 实现

#### 方案: tpm2-tss + TBS TCTI

1. **TCTI 选择**
   - Windows: 使用 `tss2-tcti-tbs.dll`
   - 备选: `tss2-tcti-device.dll`

2. **关键 API**
   - `Tss2_Sys_Initialize()` - 初始化系统上下文
   - `Tss2_Sys_GetCapability()` - 获取 TPM 能力
   - `Tss2_Sys_SelfTest()` - 执行自检

3. **数据获取**
   | 属性 | TPM2_RC 值 |
   |------|----------|
   | 制造商 | TPM2_CAP_MANUFACTURER |
   | 固件版本 | TPM2_CAP_VERSION |
   | 算法 | TPM2_CAP_ALGS |
   | 状态 | TPM2_CAP_PROP |
   | 自检 | TPM2_SelfTest |

4. **错误处理**
   - TPM 不存在: RC = TPM2_RC not found
   - TPM 禁用: 设置 status = 2
   - 自检失败: 设置 selfTestStatus = 1

---

## 文件结构

```
src/
├── core/
│   ├── Utils/
│   │   ├── LibreHardwareMonitorBridge.h/cpp  (扩展)
│   │   └── TpmBridge.h/cpp                   (新增)
│   ├── disk/
│   │   └── DiskInfo.cpp                       (修改)
│   └── DataStruct/
│       └── DataStruct.h                       (修改)
```

---

## 实现步骤

### 步骤 1: 修改 DataStruct.h

1. 添加 `TpmInfo` 结构
2. 更新 `SharedMemoryBlock`
3. 更新 `SystemInfo`

### 步骤 2: 实现 TPM Bridge

创建 `src/core/Utils/TpmBridge.h/cpp`:
- 初始化 tpm2-tss 上下文
- 获取 TPM 状态和属性
- 错误处理和状态检查

### 步骤 3: 扩展 LibreHardwareMonitorBridge

添加方法:
- `GetPhysicalDisks()` - 返回磁盘 SMART 数据

### 步骤 4: 修改 DiskInfo.cpp

集成 SMART 数据:
- 调用 LibreHardwareMonitor 获取 SMART
- 合并到现有结构

### 步骤 5: 更新主循环

在数据收集循环中添加:
- TPM 数据收集
- 磁盘 SMART 数据收集

---

## 测试计划

### 磁盘 SMART 测试

1. 有 SMART 支持的 NVMe SSD
2. 有 SMART 支持的 SATA SSD
3. HDD (如果可用)

### TPM 测试

1. 有 TPM 2.0 的系统
2. 无 TPM 的系统 (错误处理)
3. TPM 禁用状态

---

## 兼容性

- **Windows 最低版本**: Windows 10 1809+
- **TPM**: TPM 2.0 (TBS 要求)
- **.NET**: .NET Framework 4.7.2+

---

## 后续工作

1. UI 显示磁盘 SMART 详细信息
2. UI 显示 TPM 状态
3. 健康告警阈值配置