# designs · 客户端设计稿

设计稿放在代码旁边，避免设计和实现各存一份、各改一版。

| 文件 | 说明 |
|---|---|
| `design-prototype.html` | 高保真原型。单文件自包含，浏览器直接打开就是设计稿；五个视图，顶栏主机芯片与状态条数据源芯片都可点。 |

## 与代码的对应

视图集合就是设计稿的这五屏：

| 视图 | 文件 |
|---|---|
| 概览 | `Views/DashboardView.axaml` |
| 进程 | `Views/ProcessesView.axaml` |
| 网络 | `Views/NetworkView.axaml` |
| 日志 | `Views/LogView.axaml` |
| 设置 | `Views/SettingsView.axaml` |

- 导航项在 `ViewModels/MainWindowViewModel.cs`，窗口骨架与模板在 `Views/MainWindow.axaml`，DI 在 `App.axaml.cs` —— 加一屏或去一屏都同时动这三处。
- 原来单独的 CPU / GPU / 内存 / 存储 / 温度 五屏已并进概览，视图与 VM 归档在 `_archive/`。
- 阈值只有一张表：`Core/Thresholds.cs`。视图里不写死色值，颜色只给越界值与焦点。

## 尚未落地

- **进程页字段受线格式限制**：IPC 过来的进程条目只有 PID / 名称 / CPU / 内存四项，稿子里演示的用户、线程数、状态、路径、CPU 时间、启动时刻都不在线上，补齐要改采集端（还要和 TUI 一起重编）。
- **概览的逐核竖条量的是温度**：线格式只有每核温度与频率，没有每核占用，所以标签写的是「逐核温度」。
- **结束进程**：没有对应的 host 实现，按钮不出现——「有 handler 才显示」这条同样适用于客户端自己。
