using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Media;
using Avalonia.Threading;
using Microsoft.Extensions.DependencyInjection;
using Serilog;
using System;
using TCMT.Avalonia.Services;
using TCMT.Avalonia.Services.Contracts;
using TCMT.Avalonia.Services.Hardware;
using TCMT.Avalonia.Services.Hardware.IPC;
using TCMT.Avalonia.Themes;
using TCMT.Avalonia.ViewModels;
using TCMT.Avalonia.Views;

namespace TCMT.Avalonia;

public partial class App : Application
{
    public new static App Current => (App)Application.Current!;
    
    public IServiceProvider Services { get; private set; } = null!;

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        try
        {
            Services = ConfigureServices();
            
            if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            {
                var mainWindow = Services.GetRequiredService<MainWindow>();
                var mainVm = Services.GetRequiredService<MainWindowViewModel>();
                
                mainWindow.DataContext = mainVm;
                desktop.MainWindow = mainWindow;

                // Listen for font changes from Settings
                var fontService = Services.GetRequiredService<IFontService>();
                var cjkFallback = fontService.GetCjkFallbackFont();
                Action<string, double> onFontChanged = (family, _) =>
                {
                    mainWindow.FontFamily = new FontFamily($"{family}, {cjkFallback}");
                };
                fontService.FontChanged += onFontChanged;

                // 上次的界面偏好。放在挂完 FontChanged 之后 —— 靠那次事件把字体落到窗口上，
                // 不另开一条应用路径，否则「启动时」和「改设置时」就成了两套行为。
                var clientSettings = Services.GetRequiredService<IClientSettingsStore>().Load();
                Services.GetRequiredService<IThemeService>().SetTheme(clientSettings.Theme);
                fontService.ApplyAppFont(clientSettings.FontFamily, clientSettings.FontSize);

                // Handle startup
                desktop.Startup += async (_, _) => await mainVm.InitializeAsync();
                desktop.Exit += (_, _) =>
                {
                    fontService.FontChanged -= onFontChanged;
                    OnExit();
                };
                
                // Tray icon support
                SetupTrayIcon(mainVm);

                // Feed the log page from the in-memory Serilog sink. Log events are raised on the
                // writing thread, so marshal to the UI thread before touching the collection.
                var logVm = Services.GetRequiredService<LogViewModel>();
                Program.InMemoryLog.EntryWritten += (_, entry) =>
                    Dispatcher.UIThread.Post(() => logVm.Append(entry));
            }

            base.OnFrameworkInitializationCompleted();
        }
        catch (Exception ex)
        {
            Log.Fatal(ex, "Failed to initialize application");
            throw;
        }
    }

    private IServiceProvider ConfigureServices()
    {
        var services = new ServiceCollection();

        // Services - Singleton
        services.AddSingleton<IThemeService, ThemeService>();
        services.AddSingleton<INavigationService, NavigationService>();
        services.AddSingleton<IDialogService, DialogService>();
        services.AddSingleton<IHardwareService, HardwareMonitorService>();
        services.AddSingleton<IPCService>();
        services.AddSingleton<IFontService, FontService>();
        services.AddSingleton<IClientSettingsStore, ClientSettingsStore>();
        // The same instance Program hands to Serilog, so the log page sees the live tail.
        services.AddSingleton(Program.InMemoryLog);

        // ViewModels - Transient
        services.AddTransient<MainWindowViewModel>();
        services.AddTransient<DashboardViewModel>();
        // No IProcessController is registered: the collector exposes no process control, so the
        // page keeps the action hidden instead of showing a button that could not work.
        services.AddTransient<ProcessesViewModel>(sp => new ProcessesViewModel(sp.GetService<IProcessController>()));
        services.AddTransient<NetworkViewModel>();
        // Singleton on purpose: the nav item and the log sink feed must resolve to one instance.
        services.AddSingleton<LogViewModel>();
        services.AddTransient<SettingsViewModel>();

        // Views - Transient
        services.AddTransient<MainWindow>();
        services.AddTransient<DashboardView>();
        services.AddTransient<ProcessesView>();
        services.AddTransient<NetworkView>();
        services.AddTransient<LogView>();
        services.AddTransient<SettingsView>();

        return services.BuildServiceProvider();
    }

    private void SetupTrayIcon(MainWindowViewModel mainVm)
    {
        // Tray icon will be configured in MainWindow
    }

    private void OnExit()
    {
        if (Services is IDisposable disposable)
        {
            disposable.Dispose();
        }
        Log.Information("Application shutting down");
    }
}
