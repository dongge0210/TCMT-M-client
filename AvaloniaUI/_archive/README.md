# _archive · 已并进概览的五屏

设计稿把客户端收敛为五屏（概览 / 进程 / 网络 / 日志 / 设置），原来单独的 **CPU / GPU / 内存 / 存储 / 温度** 五屏内容并进了概览，视图与 VM 移到这里。

**它们仍然参与编译** —— Avalonia 会编译项目目录下的所有 `.axaml`，所以视图与 VM 必须成对存在，别只删一半。删掉也不影响导航（`MainWindowViewModel` 已经不引用它们），但恢复就得靠 git 了。

## 恢复其中一屏

1. 把 `Views/` 与 `ViewModels/` 里对应的一对文件移回上一层；
2. `ViewModels/MainWindowViewModel.cs` —— 构造参数加回该 VM，`NavigationItems` 加回一项；
3. `App.axaml.cs` —— ViewModels / Views 两组里各注册一行；
4. `Views/MainWindow.axaml` —— `Window.DataTemplates` 里加回对应的 `DataTemplate`。

四处缺一处就有一处不生效，所以按顺序来。

## 为什么归档而不是删除

删掉只剩 git 历史；归档能随时移回来，也不参与导航与构建。
