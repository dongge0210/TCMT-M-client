# TCMT UI 问题修复设计文档

## 概述

本文档描述 TCMT 项目中 4 个 UI 问题的修复设计：
1. GPU 综合使用率无数据显示
2. TPM 信息没地方显示
3. 存储设备多出无效磁盘
4. 多分区磁盘无法显示驱动器名称

## 问题1: GPU 使用率无数据显示

### 问题描述
GPU 使用率 (Usage) 属性存在于 GpuData 类中，但 C++ 端没有收集该数据。

### 解决方案
使用 NVIDIA NVML API 获取 GPU 利用率。

### 实现方案
在 `src/core/gpu/GpuInfo.cpp` 的 `QueryNvidiaGpuInfo()` 方法中添加：

```cpp
// 获取 GPU 利用率
nvmlUtilization_t utilization;
result = nvmlDeviceGetUtilizationRates(device, &utilization);
if (NVML_SUCCESS == result) {
    gpuList[index].usage = utilization.gpu;  // 需要在 GpuData 结构中添加
}
```

### 修改文件
- `src/core/gpu/GpuInfo.h` - 添加 `usage` 字段到 GpuData 结构
- `src/core/gpu/GpuInfo.cpp` - 添加 NVML 利用率查询代码
- `AvaloniaUI/Models/SystemInfo.cs` - GpuData 类已有 `Usage` 属性
- `AvaloniaUI/Views/MainWindow.axaml` - 已绑定 Usage 属性

## 问题2: TPM 信息没地方显示

### 问题描述
TpmBridge.cpp 已实现 TPM 数据收集，但 Avalonia UI 没有显示位置。

### 解决方案
在 UI 中添加 TPM 信息显示区块。

### 实现方案
1. **数据结构**: 在 SharedMemoryBlock 中添加 TpmInfo（如果尚未添加）
2. **C++ 端**: 已有 TpmBridge 实现，验证数据写入共享内存
3. **Avalonia UI**: 
   - 在 SharedMemoryService.cs 中添加 TpmInfo 结构映射
   - 在 SystemInfo.cs 中添加 TpmData 类
   - 在 MainWindowViewModel.cs 中添加 Tpm 属性
   - 在 MainWindow.axaml 中添加 TPM 信息显示区块

### UI 布局
```
TPM 信息
┌─────────────────────────────────────┐
│ 制造商: XXX                         │
│ 固件版本: X.X.X                     │
│ 状态: 就绪/未就绪                    │
│ 自检状态: 成功/失败                  │
│ 已启用: 是/否                        │
└─────────────────────────────────────┘
```

### 修改文件
- `src/core/DataStruct/DataStruct.h` - 确认 TpmInfo 结构
- `src/core/Utils/TpmBridge.cpp` - 验证 TPM 数据收集
- `AvaloniaUI/Services/SharedMemoryService.cs` - 添加 TpmInfo 结构
- `AvaloniaUI/Models/SystemInfo.cs` - 添加 TpmData 类
- `AvaloniaUI/ViewModels/MainWindowViewModel.cs` - 添加 Tpm 属性
- `AvaloniaUI/Views/MainWindow.axaml` - 添加 TPM 显示区块

## 问题3: 存储设备多出无效磁盘

### 问题描述
物理磁盘列表显示了很多无效/空的磁盘（容量为0、型号为"Unknown"）。

### 解决方案
在 WMI 查询后添加过滤逻辑。

### 实现方案
在 `src/core/disk/DiskInfo.cpp` 的 `CollectPhysicalDisks()` 方法中，查询完成后添加过滤：

```cpp
// 过滤无效磁盘
std::vector<PhysicalDiskSmartData> validDisks;
for (const auto& disk : tempDisks) {
    // 跳过无效磁盘
    if (disk.second.capacity == 0) continue;
    if (disk.second.model[0] == L'\0') continue;
    if (wcsstr(disk.second.model, L"Unknown") != nullptr) continue;
    if (disk.second.serialNumber[0] == L'\0') continue;
    
    validDisks.push_back(disk.second);
}
```

### 修改文件
- `src/core/disk/DiskInfo.cpp` - 添加磁盘过滤逻辑

## 问题4: 多分区磁盘无法显示驱动器名称

### 问题描述
当同一个物理磁盘有多个分区时，无法显示各驱动器的卷标名称。

### 解决方案
WMI 查询 `Win32_LogicalDiskToPartition` 时同时获取逻辑驱动器的卷标信息。

### 实现方案
修改 `src/core/disk/DiskInfo.cpp` 的 `CollectPhysicalDisks()` 方法：

1. 先查询所有逻辑驱动器获取卷标
2. 在 `Win32_LogicalDiskToPartition` 查询时同时获取驱动器卷标
3. 将卷标信息存储到 `PhysicalDiskSmartData` 结构

### 数据结构扩展
在 `PhysicalDiskSmartData` 中添加分区卷标数组：
```cpp
wchar_t partitionLabels[8][32];  // 每个分区的卷标
```

### 修改文件
- `src/core/DataStruct/DataStruct.h` - 添加分区卷标字段
- `src/core/disk/DiskInfo.cpp` - 修改分区查询逻辑
- `AvaloniaUI/Models/SystemInfo.cs` - 添加分区卷标属性
- `AvaloniaUI/Views/MainWindow.axaml` - 显示分区卷标

## 实现顺序

1. GPU 使用率（优先级：高）
2. 存储设备过滤（优先级：高）
3. 多分区显示（优先级：中）
4. TPM 信息显示（优先级：中）

## 验收标准

1. **GPU 使用率**: NVIDIA GPU 显示综合使用率百分比
2. **TPM 显示**: 显示制造商、版本、状态等信息
3. **存储过滤**: 不再显示容量为0或型号为Unknown的磁盘
4. **多分区显示**: 物理磁盘显示所有关联分区的卷标