using System;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Input;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Serilog;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;
using TCMT.Avalonia.Services.Contracts;
using TCMT.Avalonia.Themes;

namespace TCMT.Avalonia.ViewModels;

public partial class MainWindowViewModel : ObservableObject, IDisposable
{
    private readonly IHardwareService _hardwareService;
    private readonly INavigationService _navigationService;
    private readonly IThemeService _themeService;
    private bool _disposed;

    [ObservableProperty]
    private string _windowTitle = $"{AppConstants.AppName} - {AppConstants.AppDisplayName}";

    [ObservableProperty]
    private string _connectionStatus = "连接中...";

    [ObservableProperty]
    private bool _isConnected;

    [ObservableProperty]
    private string _appVersion = "0.14.0";

    [ObservableProperty]
    private ObservableObject? _currentPage;

    public ObservableCollection<NavigationItem> NavigationItems { get; } = new();

    [ObservableProperty]
    private NavigationItem? _selectedNavigationItem;

    public MainWindowViewModel(
        IHardwareService hardwareService,
        INavigationService navigationService,
        IThemeService themeService,
        DashboardViewModel dashboardVm,
        ProcessesViewModel processesVm,
        NetworkViewModel networkVm,
        LogViewModel logVm,
        SettingsViewModel settingsVm)
    {
        _hardwareService = hardwareService;
        _navigationService = navigationService;
        _themeService = themeService;

        // Setup navigation items
        // Five views, matching the design draft: 概览 / 进程 / 网络 / 日志 / 设置.
        // CPU / GPU / 内存 / 温度 / 存储 五屏的内容已并进概览，原视图与 VM 归档在 _archive/。
        NavigationItems.Add(new NavigationItem("概览", "dashboard", dashboardVm));
        NavigationItems.Add(new NavigationItem("进程", "processes", processesVm));
        NavigationItems.Add(new NavigationItem("网络", "network", networkVm));
        NavigationItems.Add(new NavigationItem("日志", "log", logVm));
        NavigationItems.Add(BuildHardwareGroup());
        NavigationItems.Add(new NavigationItem("设置", "settings", settingsVm));

        // Wire up navigation
        _navigationService.Navigated += (_, vm) => CurrentPage = vm;
        
        // Wire up hardware events
        _hardwareService.DataReceived += OnHardwareDataReceived;
        _hardwareService.ConnectionStateChanged += OnConnectionStateChanged;
        _hardwareService.ErrorOccurred += OnErrorOccurred;

        // Initial navigation
        SelectedNavigationItem = NavigationItems[0];
        CurrentPage = dashboardVm;
    }

    public async Task InitializeAsync()
    {
        try
        {
            await _hardwareService.StartAsync();
            Log.Information("MainWindow initialized and hardware monitoring started");
        }
        catch (Exception ex)
        {
            Log.Error(ex, "Failed to initialize hardware monitoring");
            ConnectionStatus = $"初始化失败: {ex.Message}";
        }
    }

    /// <summary>
    /// 「硬件」那一组：十个部件是同一种页面，各配一个 <see cref="HardwareSpecViewModel"/>。
    /// 组本身没有自己的页面 —— 它的 ViewModel 直接借第一个子项的，所以点分组等于进第一页
    /// （或上次停的那一页）。这样不必让 ViewModel 变成可空，也不必为分组造一个空壳 VM。
    /// </summary>
    private NavigationItem BuildHardwareGroup()
    {
        var children = HardwareParts.Ordered
            .Select(part => new NavigationItem(
                HardwareParts.NameOf(part), "hardware", new HardwareSpecViewModel(part)))
            .ToList();

        var group = new NavigationItem("硬件", "hardware", children[0].ViewModel);

        foreach (var child in children)
        {
            // 子项不在 ListBox 的 Items 里，选中它没有别的路可走
            child.ActivateCommand = new RelayCommand(() => ActivateChild(group, child));
            group.Children.Add(child);
        }

        // 分组行只在点箭头时收起 —— 展开由"选中该分组"负责
        group.ActivateCommand = new RelayCommand(() => group.IsExpanded = !group.IsExpanded);
        return group;
    }

    /// <summary>
    /// 切到分组里的某一项。
    /// 父项保持高亮（它是当前分区），子项另标一个"当前"—— 两者是两件事，不能共用一个 IsSelected。
    /// </summary>
    private void ActivateChild(NavigationItem group, NavigationItem child)
    {
        foreach (var sibling in group.Children)
        {
            sibling.IsSelected = sibling == child;
        }

        group.ActiveChild = child;
        _navigationService.NavigateTo(child.ViewModel);
    }

    partial void OnSelectedNavigationItemChanged(NavigationItem? value)
    {
        foreach (var item in NavigationItems)
        {
            item.IsSelected = item == value;
            // 换到别的分区时，别把上一个分区的子项高亮留在那儿
            foreach (var child in item.Children)
            {
                child.IsSelected = false;
            }
        }

        if (value is null) return;

        if (value.HasChildren)
        {
            value.IsExpanded = true;
            ActivateChild(value, value.ActiveChild ?? value.Children[0]);
            return;
        }

        _navigationService.NavigateTo(value.ViewModel);
    }

    private void OnHardwareDataReceived(object? sender, SystemInfo info)
    {
        // Update app version from IPC
        if (!string.IsNullOrEmpty(info.Version))
            AppVersion = info.Version;

        // Broadcast to all view models
        foreach (var item in NavigationItems)
        {
            if (item.HasChildren)
            {
                // 分组自己没页面，它的 ViewModel 就是第一个子项那个 —— 只走子项，
                // 否则同一份规格表每个采样周期会被重建两遍
                foreach (var child in item.Children)
                {
                    child.ViewModel.Update(info);
                }
            }
            else
            {
                item.ViewModel.Update(info);
            }
        }
    }

    private void OnConnectionStateChanged(object? sender, bool connected)
    {
        IsConnected = connected;
        ConnectionStatus = connected ? "已连接" : "已断开";
        WindowTitle = connected 
            ? $"{AppConstants.AppName} - {AppConstants.AppDisplayName}"
            : $"{AppConstants.AppName} - {AppConstants.AppDisplayName} (已断开)";
    }

    private void OnErrorOccurred(object? sender, string error)
    {
        Log.Warning("Hardware service error: {Error}", error);
        ConnectionStatus = $"错误: {error}";
    }

    [RelayCommand]
    private async Task Reconnect()
    {
        ConnectionStatus = "重新连接中...";
        await _hardwareService.ReconnectAsync();
    }

    [RelayCommand]
    private void ShowWindow()
    {
        if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.MainWindow?.Show();
            desktop.MainWindow?.Activate();
        }
    }

    [RelayCommand]
    private void Quit()
    {
        Dispose();
        if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.Shutdown();
        }
    }

    [RelayCommand]
    private void ToggleTheme()
    {
        var newTheme = _themeService.CurrentTheme == AppTheme.Dark ? AppTheme.Light : AppTheme.Dark;
        _themeService.SetTheme(newTheme);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        _hardwareService.DataReceived -= OnHardwareDataReceived;
        _hardwareService.ConnectionStateChanged -= OnConnectionStateChanged;
        _hardwareService.ErrorOccurred -= OnErrorOccurred;
        
        _hardwareService.Dispose();
        GC.SuppressFinalize(this);
    }
}

/// <summary>
/// 一个导航项。顶层项平铺在左栏；带子项的顶层项是一个可展开的分组（目前只有「硬件」）。
///
/// 子项不放进 ListBox 的 Items —— 它嵌在父项的模板里，所以 ListBox 的选中态管不到它，
/// 点它得走 <see cref="ActivateCommand"/>。
/// </summary>
public class NavigationItem : ObservableObject
{
    public string Title { get; }
    public string Icon { get; }
    public ViewModelBase ViewModel { get; }

    /// <summary>分组里的子项。空表示这是一个普通页面。</summary>
    public ObservableCollection<NavigationItem> Children { get; } = new();

    public bool HasChildren => Children.Count > 0;

    private bool _isSelected;
    public bool IsSelected
    {
        get => _isSelected;
        set => SetProperty(ref _isSelected, value);
    }

    private bool _isExpanded;
    public bool IsExpanded
    {
        get => _isExpanded;
        set => SetProperty(ref _isExpanded, value);
    }

    /// <summary>
    /// 分组内当前停在哪一项。切回这个分组时回到这里，而不是每次都被弹回第一项。
    /// </summary>
    public NavigationItem? ActiveChild { get; set; }

    /// <summary>
    /// 这一项被点时要做什么。由 MainWindowViewModel 注入 —— "当前显示哪一页"归它管，
    /// 导航项自己不知道。
    /// </summary>
    public ICommand? ActivateCommand { get; set; }

    public NavigationItem(string title, string icon, ViewModelBase viewModel)
    {
        Title = title;
        Icon = icon;
        ViewModel = viewModel;
    }
}
