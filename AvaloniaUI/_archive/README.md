# _archive · 已并进概览的五屏

设计稿把客户端收敛为五屏（概览 / 进程 / 网络 / 日志 / 设置），原来单独的 **CPU / GPU / 内存 / 存储 / 温度** 五屏内容并进了概览，视图与 VM 移到这里。

**这份归档不参与编译** —— `TCMT.Avalonia.csproj` 里显式排除了 `_archive/` 下的 `.cs` 与 `.axaml`。
不排除的话 SDK 式工程会默认通配把它们编进去（Avalonia 也会编译项目目录下所有 `.axaml`），
而归档不该跟着主线改字段。排除必须 **成对** —— 只排 `.cs` 会留下没有 code-behind 的 XAML，编译失败。

导航不受影响（`MainWindowViewModel` 不引用它们）。归档文件本身没删，随时能移回去。

## 恢复其中一屏

1. 把 `Views/` 与 `ViewModels/` 里对应的一对文件移回上一层；
2. `ViewModels/MainWindowViewModel.cs` —— 构造参数加回该 VM，`NavigationItems` 加回一项；
3. `App.axaml.cs` —— ViewModels / Views 两组里各注册一行；
4. `Views/MainWindow.axaml` —— `Window.DataTemplates` 里加回对应的 `DataTemplate`。

四处缺一处就有一处不生效，所以按顺序来。

## 为什么归档而不是删除

删掉只剩 git 历史；归档能随时移回来，也不参与导航与构建。
