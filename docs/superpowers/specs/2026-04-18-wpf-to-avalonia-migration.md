# WPF → Avalonia UI Migration Design

> **For agentic workers:** This is a migration specification. Use as reference for implementation planning.

**Goal:** 将 WPF-UI1 完全迁移到 Avalonia UI 框架

**Architecture:** 使用 Avalonia 11+ 完全重写 UI 层，保留 Model/Service 层，添加双向通信支持

**Tech Stack:**
- Avalonia 11+ (跨平台 .NET UI 框架)
- OxyPlot.Avalonia (图表)
- CommunityToolkit.Mvvm (MVVM)
- Serilog (日志)
- Microsoft.Extensions (DI/Hosting)

---

## Project Overview

### 项目配置

| 项目 | 配置值 |
|------|--------|
| 项目目录 | `Avalonia-UI1/` |
| 项目名称 | `TCMT` |
| 根命名空间 | `TCMT` |
| 目标框架 | `net472` (非 Windows 特定) |
| 输出类型 | WinExe (可扩展为跨平台) |

### 项目结构

```
TCMT-Windows-client/
├── Avalonia-UI1/              # 新 Avalonia 项目
│   ├── TCMT.csproj
│   ├── App.axaml
│   ├── App.axaml.cs
│   ├── MainWindow.axaml
│   ├── MainWindow.axaml.cs
│   ├── Models/
│   │   └── SystemInfo.cs      # 从 WPF-UI1 复制
│   ├── Services/
│   │   ├── SharedMemoryService.cs  # 保留并扩展
│   │   └── HttpService.cs     # 新增：HTTP/TCP 服务
│   ├── ViewModels/
│   │   └── MainWindowViewModel.cs  # 适配
│   └── Converters/
│       └── ValueConverters.cs  # 适配
│
├── WPF-UI1/                  # 保留原版
│   └── ...
│
├── TCMT.sln                  # 已存在
├── TCMT.vcxproj              # 已存在
└── ...
```

---

## Dependency Changes

### NuGet 包替换

| 原 WPF 包 | 新 Avalonia 包 | 用途 |
|----------|---------------|------|
| `LiveChartsCore.SkiaSharpView.WPF` | `OxyPlot.Avalonia` | 图表 |
| `MaterialDesignThemes` | `Avalonia.Themes.Fluent` | Fluent 主题 |
| `MaterialDesignColors` | — | 不需要 |
| — | `Avalonia.Desktop` | 桌面支持 |
| — | `Avalonia.Win32` | Windows 特定 |

### 保留的包（无需修改）

| 包 | 用途 |
|---|---|
| `CommunityToolkit.Mvvm` | MVVM |
| `Serilog` | 日志 |
| `Serilog.Sinks.File` | 文件日志 |
| `Serilog.Sinks.Console` | 控制台日志 |
| `Microsoft.Extensions.DependencyInjection` | DI |
| `Microsoft.Extensions.Hosting` | Hosting |
| `Microsoft.Extensions.Logging` | 日志抽象 |

---

## UI Migration Mapping

### 文件扩展名

| WPF | Avalonia |
|-----|---------|
| `.xaml` | `.axaml` |
| `.xaml.cs` | `.axaml.cs` |

### 控件映射

| WPF 控件 | Avalonia 控件 | 迁移说明 |
|----------|--------------|----------|
| `Window` | `Window` | 直接迁移 |
| `Grid` | `Grid` | 直接迁移 |
| `StackPanel` | `StackPanel` | 直接迁移 |
| `ScrollViewer` | `ScrollViewer` | 直接迁移 |
| `TextBlock` | `TextBlock` | 直接迁移 |
| `Button` | `Button` | 直接迁移 |
| `ProgressBar` | `ProgressBar` | 直接迁移 |
| `ComboBox` | `ComboBox` | 直接迁移 |
| `Expander` | `Expander` | 需要调整样式 |
| `materialDesign:Card` | `Border` + Style | 需要自定义样式 |
| `materialDesign:PackIcon` | `PathIcon` 或 `Image` | 需要替换图标方式 |
| DataBinding | DataBinding | 语法相同 |

### 主题迁移

```xml
<!-- WPF (当前) -->
<Window Background="{DynamicResource MaterialDesignPaper}"
      FontFamily="{DynamicResource MaterialDesignFont}"
      TextElement.Foreground="{DynamicResource MaterialDesignBody}"

<!-- Avalonia (迁移后) -->
<Window Background="{DynamicResource ThemeBackground}"
      FontFamily="{DynamicResource ThemeFont}"
      Foreground="{DynamicResource ThemeForeground}"
```

---

## Communication (双向管道)

### 1. 共享内存（保留）

```csharp
// 保留现有 SharedMemoryService.cs 的实现
// 位置：Services/SharedMemoryService.cs
public class SharedMemoryService
{
    // 连接 Local\SystemMonitorSharedMemory
    // 使用 Marshal 读取结构
}
```

### 2. HTTP/TCP（新增）

```csharp
// 新增服务用于远程监控
// 位置：Services/HttpService.cs
public class HttpService : IDisposable
{
    // HTTP 服务器监听配置端口
    // 返回 JSON 格式的 SystemInfo
}
```

---

## Implementation Phases

### Phase 1: 项目搭建
1. 创建 `Avalonia-UI1/` 目录
2. 创建 `TCMT.csproj` 项目文件
3. 添加 NuGet 包引用
4. 验证项目可编译

### Phase 2: 基础迁移
1. 创建基础 App.axaml/App.axaml.cs
2. 创建 MainWindow.axaml/MainWindow.axaml.cs
3. 复制 Models/SystemInfo.cs
4. 创建基本 ViewModel

### Phase 3: 服务层迁移
1. 复制 SharedMemoryService.cs
2. 创建 HttpService.cs（新增）
3. 配置依赖注入

### Phase 4: UI 迁移
1. 重写 MainWindow.axaml
2. 迁移 Converters
3. 添加 Fluent 主题
4. 测试

### Phase 5: 收尾
1. 更新 TCMT.sln 包含新项目
2. 清理测试
3. 文档更新
4. 提交

---

## Files to Modify/Create

### Create (新文件)
```
Avalonia-UI1/TCMT.csproj
Avalonia-UI1/App.axaml
Avalonia-UI1/App.axaml.cs
Avalonia-UI1/MainWindow.axaml
Avalonia-UI1/MainWindow.axaml.cs
Avalonia-UI1/Services/HttpService.cs
```

### Copy (从 WPF-UI1 复制后修改)
```
Avalonia-UI1/Models/SystemInfo.cs
Avalonia-UI1/Services/SharedMemoryService.cs
Avalonia-UI1/ViewModels/MainWindowViewModel.cs
Avalonia-UI1/Converters/ValueConverters.cs
```

### Delete (原 WPF 文件，不删除)
```
# WPF-UI1/ 保留不动
```

---

## Success Criteria

- [ ] Avalonia-UI1 项目可编译
- [ ] 运行显示与当前 WPF 相似的 UI
- [ ] 共享内存连接正常工作
- [ ] HTTP 服务可返回数据
- [ ] 温度图表显示正确
- [ ] TCMT.sln 包含新项目

---

## Notes

- WPF-UI1 保留，不删除
- Avalonia 版本作为并行版本
- 后续可选择完全替换或保留双版本
- 跨平台支持在 Windows 版本稳定后添加