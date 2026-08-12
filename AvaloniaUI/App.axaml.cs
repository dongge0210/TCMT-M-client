using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Media;
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
                
                // Handle startup
                desktop.Startup += async (_, _) => await mainVm.InitializeAsync();
                desktop.Exit += (_, _) =>
                {
                    fontService.FontChanged -= onFontChanged;
                    OnExit();
                };
                
                // Tray icon support
                SetupTrayIcon(mainVm);
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

        // ViewModels - Transient
        services.AddTransient<MainWindowViewModel>();
        services.AddTransient<DashboardViewModel>();
        services.AddTransient<CpuViewModel>();
        services.AddTransient<MemoryViewModel>();
        services.AddTransient<NetworkViewModel>();
        services.AddTransient<StorageViewModel>();
        services.AddTransient<GpuViewModel>();
        services.AddTransient<TemperaturesViewModel>();
        services.AddTransient<SettingsViewModel>();

        // Views - Transient
        services.AddTransient<MainWindow>();
        services.AddTransient<DashboardView>();
        services.AddTransient<CpuView>();
        services.AddTransient<MemoryView>();
        services.AddTransient<NetworkView>();
        services.AddTransient<StorageView>();
        services.AddTransient<GpuView>();
        services.AddTransient<TemperaturesView>();
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
