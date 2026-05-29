using System;
using System.Collections.ObjectModel;
using System.Threading.Tasks;
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
    private ObservableObject? _currentPage;

    public ObservableCollection<NavigationItem> NavigationItems { get; } = new();

    [ObservableProperty]
    private NavigationItem? _selectedNavigationItem;

    public MainWindowViewModel(
        IHardwareService hardwareService,
        INavigationService navigationService,
        IThemeService themeService,
        DashboardViewModel dashboardVm,
        CpuViewModel cpuVm,
        MemoryViewModel memoryVm,
        NetworkViewModel networkVm,
        StorageViewModel storageVm,
        SettingsViewModel settingsVm)
    {
        _hardwareService = hardwareService;
        _navigationService = navigationService;
        _themeService = themeService;

        // Setup navigation items
        NavigationItems.Add(new NavigationItem("Dashboard", "📊", dashboardVm));
        NavigationItems.Add(new NavigationItem("CPU", "🖥️", cpuVm));
        NavigationItems.Add(new NavigationItem("Memory", "🧠", memoryVm));
        NavigationItems.Add(new NavigationItem("Network", "🌐", networkVm));
        NavigationItems.Add(new NavigationItem("Storage", "💾", storageVm));
        NavigationItems.Add(new NavigationItem("Settings", "⚙️", settingsVm));

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

    partial void OnSelectedNavigationItemChanged(NavigationItem? value)
    {
        if (value?.ViewModel != null)
        {
            _navigationService.NavigateTo(value.ViewModel);
        }
    }

    private void OnHardwareDataReceived(object? sender, SystemInfo info)
    {
        // Broadcast to all view models
        foreach (var item in NavigationItems)
        {
            item.ViewModel.Update(info);
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

public class NavigationItem
{
    public string Title { get; }
    public string Icon { get; }
    public ViewModelBase ViewModel { get; }

    public NavigationItem(string title, string icon, ViewModelBase viewModel)
    {
        Title = title;
        Icon = icon;
        ViewModel = viewModel;
    }
}
