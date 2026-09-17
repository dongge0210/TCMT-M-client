# 设计口径与改动记录

客户端与终端共用的一套设计口径，以及本次移植的落地情况。
生成于设计工作区，**未在此运行过构建**——落地后请自行 `dotnet run` 验一遍。

---

## 一、已落地

| 文件 | 改动 |
|---|---|
| `Core/Thresholds.cs` | **新增**。全应用唯一的阈值表：占用 70/90、温度 60/80 °C、单进程 CPU 20/50、电量 50/20 与健康 80/60（后两者方向相反）。附 `Evaluate()` 返回 Normal / Warning / Critical。 |
| `Core/HistoryBuffer.cs` | **新增**。定长环形缓冲，提供 `Push` / `ToArray`（旧→新）/ `Max`（窗口峰值归一）。 |
| `Core/PlatformCaps.cs` | **新增**。本机平台能力层与降级文案常量。规则是「有对应 handler 才显示，没有的不假装」。 |
| `Services/LogStream.cs` | **新增**。Serilog sink：把日志尾部留在内存里（容量 500，线程安全，带 `EntryWritten` 事件）。 |
| `ViewModels/LogViewModel.cs` | **新增**。日志页视图模型：级别、时间戳、来源、跟随模式。 |
| `Views/LogView.axaml` + `.axaml.cs` | **新增**。日志页：级别收成一段色块（INFO 弱化 / WARN 黄 / ERROR 红，**颜色只给级别，正文不着色**）；跟随模式为「滚离顶部暂停、回到顶部恢复」。 |
| `Program.cs` | logger 挂上 `InMemoryLog` sink；`Program.InMemoryLog` 暴露给 DI。 |
| `App.axaml.cs` | 注册 `LogStream` 与 `LogViewModel`（后者 **Singleton**——导航项与日志源必须是同一个实例）；注册 `LogView`；在启动时把 `EntryWritten` 接到 `LogViewModel.Append`，并 `Dispatcher.UIThread.Post` 切回 UI 线程。 |
| `ViewModels/MainWindowViewModel.cs` | 构造函数注入 `LogViewModel`，侧栏加「日志」一项（排在「存储」与「设置」之间）。 |
| `Views/MainWindow.axaml` | DataTemplates 加 `LogViewModel → LogView`；两处写死的 `#238636` / `#3B82F6` 改为主题画刷（渲染不变）。 |
| `Core/Constants.cs` | `Colors` 十二个色值与 `Themes/DarkTheme.axaml` 对齐（原先代码里 `#4CAF50`、XAML 里 `#238636`，两套绿同时存在）；`SidebarWidth` 64→200。 |
| `Core/Converters/ValueConverters.cs` | `PercentToWarningColorConverter` 阈值 80/95 → 读 `Thresholds.Load`（70/90）；新增 `ThresholdToColorConverter`（在范围内中性色，越界才转黄/红，规则用 `ConverterParameter` 选）。 |
| `Views/DashboardView.axaml` | 四个大数字不再用写死颜色：CPU 温度 / GPU 温度 / 内存都改由阈值表决定颜色。原先 GPU 那张卡写死橙色，**0 °C 也报橙——颜色在说谎**。 |
| `Views/*.axaml`（7 个文件、31 处） | 写死的 `#0D1117` / `#3B82F6` / `#238636` / `#1a1a26` / `#30363D` 全部换成对应主题画刷，**渲染结果不变**，只是不再各写各的。 |
| `ViewModels/NetworkViewModel.cs` + `Views/NetworkView.axaml` | 缺无线网卡时保留区块并说明原因；新增 `WifiStateNote` 四态引导句（已连接无需说明时不占位）。 |
| `docs/design-prototype.html` | **新增**。设计原型（单文件 HTML，浏览器直接打开即可看设计稿），与本工程同源。 |

两条纪律：**只加不改**（同名文件冲突以现有实现为准）、**只用现有色值**（没有引入新配色）。

## 二、还没做（需要你拍板或需要上下文）

1. **仪表盘历史改用环形缓冲**。`DashboardViewModel.AddMemoryHistoryPoint` 现在是
   `while (MemoryHistory.Count > AppConstants.MaxChartPoints) RemoveAt(0)`——每帧搬 60 个元素。
   `Core/HistoryBuffer.cs` 已就位，但 `MemoryHistory` 是绑在图表控件上的 `ObservableCollection<double>`，
   换类型会同时动到图表接口，而那个控件我没看过，所以没有盲改。改法：

   ```csharp
   private readonly HistoryBuffer _memoryHistory = new(AppConstants.MaxChartPoints);

   private void AddMemoryHistoryPoint(double value) => _memoryHistory.Push(value);

   // 图表改绑快照属性，写指针推进时通知一次
   public IReadOnlyList<double> MemoryHistoryPoints => _memoryHistory.ToArray();
   ```

2. **接口表的 L/D/U 列**。`NetworkAdapterData` 里只有 `Speed` / `DownloadSpeed` / `UploadSpeed`
   （对应 `SpeedDisplay` / `DownloadDisplay` / `UploadDisplay`），没有收发字节总量与丢包数——
   这两个字段我没有编，所以要补「逐接口列表」得先确认数据层能不能给。
   另外侧边那排适配器标签目前是 `ItemsControl`，**点了不会切换**（`SelectedAdapter` 只由代码自动选），
   要能手动选，把它换成绑 `SelectedItems` 的 `ListBox`，或给每个标签加一条选择命令。

3. **调色板的单一来源**。`DarkTheme.axaml` 与 `Core/Constants.cs` 的值现已一致，但仍是**两份**：
   XAML 资源没法被编译期常量读取（`ConverterColors` 要 parse 字符串），这份重复暂时消不掉。
   约定：**改色先改主题，再同步 Constants**（`Constants.Colors` 上方已写明）。

## 三、跑起来验这四件事

```sh
cd ~/TCMT/TCMT-M-client/avaloniaUI
dotnet run
```

1. **日志页**：侧栏应有「日志」；停几秒，启动与 IPC 的事件会自己出现并贴底，右上写「跟随最新」；
   往上滚变成「已暂停跟随」，滚回顶部恢复。
2. **阈值**：占用跨过 70% 就该变橙（原先是 80%），跨 90% 变红。
3. **颜色一致性**：转换器画的进度色与 XAML 主题色现在应当一致（原先两套绿）。
4. **网络页**：停用无线网卡时卡片保留并写「未检测到无线网卡——无线信息不可读」；
   连着但读不到 SSID 时写「已连接，但 SSID 读不到——系统需要开启定位服务才会返回 SSID」。

## 四、另外两处顺手记下的不一致

- `TCMT.Avalonia.csproj` 里 `Avalonia.Diagnostics` 是 **11.3.20**，其余 Avalonia 包是 **12.1.1**。
  只在 Debug 生效，但版本不一致迟早咬人。
- 工程开着 `AvaloniaUseCompiledBindingsByDefault=true`——**绑定路径写错即编译错误**，改 XAML 时留意。
- **根命名空间与框架同名**：`RootNamespace` 是 `TCMT.Avalonia`，在 `namespace TCMT.Avalonia` 内部写 `Avalonia.Threading.X`
  会被解析成 `TCMT.Avalonia.Threading`（外层命名空间优先于框架命名空间），报 `CS0234`。
  写法二选一：顶部 `using Avalonia.Threading;` 后直接用 `Dispatcher`，或者写 `global::Avalonia.Threading.Dispatcher`。
  同样的坑在 `Avalonia.Controls`、`Avalonia.Media` 上都会复现——**别用 `Avalonia.` 全限定名**。
- Views 里还剩两个刻意的半透明值：`#23863626`（DashboardView 的 Running 徽标底）与 `#161B2226`（StorageView 的一块底）。
  主题里没有对应令牌，我没有擅自新增——要收的话在 `DarkTheme.axaml` 加两条带 alpha 的 Color 即可。
