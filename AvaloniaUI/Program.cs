using Avalonia;
using Serilog;
using System;
using System.IO;
using TCMT.Avalonia.Services;

namespace TCMT.Avalonia;

internal sealed class Program
{
    /// <summary>In-memory tail of the log. Registered in DI so the log page binds to the same instance.</summary>
    public static LogStream InMemoryLog { get; } = new();

    [STAThread]
    public static void Main(string[] args)
    {
        var logPath = Path.Combine(AppContext.BaseDirectory, "logs", "tcmt-.log");
        Directory.CreateDirectory(Path.GetDirectoryName(logPath)!);
        
        var logConfig = new LoggerConfiguration()
            .MinimumLevel.Debug()
            .WriteTo.Sink(InMemoryLog)
            .WriteTo.File(logPath, rollingInterval: RollingInterval.Day, retainedFileCountLimit: 7);
#if DEBUG
        logConfig.WriteTo.Debug();
#endif
        Log.Logger = logConfig.CreateLogger();

        try
        {
            Log.Information("TCMT Avalonia v{Version} starting...", typeof(Program).Assembly.GetName().Version);
            BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        }
        catch (Exception ex)
        {
            Log.Fatal(ex, "Application terminated unexpectedly");
            throw;
        }
        finally
        {
            Log.CloseAndFlush();
        }
    }

    public static AppBuilder BuildAvaloniaApp()
    {
        // UsePlatformDetect() 来自 Avalonia.Desktop 元包，会顺带把三套后端都编进产物。
        // 现在 csproj 按宿主只引一个后端，这里也只调对应的那一个。
        var builder = AppBuilder.Configure<App>()
            .UseHarfBuzz();
#if TCMT_MACOS
        builder = builder.UseAvaloniaNative();
#elif TCMT_WINDOWS
        builder = builder.UseWin32();
#else
        builder = builder.UseX11();
#endif
        return builder
            .UseSkia()
            .WithInterFont()
            .LogToTrace();
    }
}
