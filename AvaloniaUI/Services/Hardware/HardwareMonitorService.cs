using System;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Threading;
using Serilog;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;
using TCMT.Avalonia.Services.Contracts;
using TCMT.Avalonia.Services.Hardware.IPC;
using TCMT.Avalonia.Localization;

namespace TCMT.Avalonia.Services.Hardware;

public class HardwareMonitorService : IHardwareService
{
    private readonly IPCService _ipcService;
    private readonly DispatcherTimer _timer;
    private CancellationTokenSource? _cts;
    private int _consecutiveErrors;
    private bool _disposed;

    public bool IsConnected { get; private set; }
    public string ConnectionStatus { get; private set; } = Translator.T("连接中...");

    public event EventHandler<SystemInfo>? DataReceived;
    public event EventHandler<bool>? ConnectionStateChanged;
    public event EventHandler<string>? ErrorOccurred;
    public event EventHandler<string>? ConfigSnapshotReceived;

    public HardwareMonitorService(IPCService ipcService)
    {
        _ipcService = ipcService;
        _timer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(AppConstants.UpdateIntervalMs)
        };
        _timer.Tick += OnTimerTick;

        _ipcService.ConnectionStateChanged += (connected, msg) =>
        {
            IsConnected = connected;
            ConnectionStatus = Translator.T(connected ? "已连接 ({0})" : "已断开 ({0})", msg);
            ConnectionStateChanged?.Invoke(this, connected);
        };

        _ipcService.DataReady += () =>
        {
            _consecutiveErrors = 0;
            IsConnected = true;
            ConnectionStatus = Translator.T("已连接 (IPC)");
        };

        _ipcService.VersionMismatch += msg =>
        {
            ErrorOccurred?.Invoke(this, Translator.T("版本不匹配: {0}", msg));
        };

        _ipcService.ConfigSnapshotReceived += json => ConfigSnapshotReceived?.Invoke(this, json);
    }

    public Task<bool> SendConfigAsync(string json) => _ipcService.SendConfigAsync(json);

    public Task StartAsync()
    {
        if (_disposed) throw new ObjectDisposedException(nameof(HardwareMonitorService));
        
        _cts = new CancellationTokenSource();
        _ipcService.Start();
        _timer.Start();
        
        Log.Information("Hardware monitoring started");
        return Task.CompletedTask;
    }

    public Task StopAsync()
    {
        _timer.Stop();
        _cts?.Cancel();
        return Task.CompletedTask;
    }

    public async Task ReconnectAsync()
    {
        _consecutiveErrors = 0;
        ConnectionStatus = Translator.T("重新连接中...");
        
        await StopAsync();
        // 用 Reset 而不是 Dispose：Dispose 会把 _disposed 置位，之后 Start 直接空转返回，
        // 「重新连接」变成死操作。Reset 换掉内部连接对象，让下面的 StartAsync 能真正重来。
        _ipcService.Reset();

        try
        {
            await StartAsync();
        }
        catch (Exception ex)
        {
            Log.Error(ex, "Reconnect failed");
            IsConnected = false;
            ConnectionStatus = Translator.T("重连失败");
            ErrorOccurred?.Invoke(this, Translator.T("重连失败: {0}", ex.Message));
        }
    }

    private void OnTimerTick(object? sender, EventArgs e)
    {
        try
        {
            if (_ipcService.IsMemoryOpen)
            {
                var info = IPCSystemInfoMapper.Read(_ipcService);
                if (info != null)
                {
                    // 从连续失败里爬出来了，说一声。每次都报就成了每秒两条的噪音 ——
                    // 这个定时器按 UpdateIntervalMs 跑。
                    if (_consecutiveErrors > 0)
                    {
                        Log.Information("硬件数据读取已恢复（连续失败 {Count} 次后）", _consecutiveErrors);
                    }
                    _consecutiveErrors = 0;
                    if (!IsConnected)
                    {
                        IsConnected = true;
                        ConnectionStatus = Translator.T("已连接 (IPC)");
                        ConnectionStateChanged?.Invoke(this, true);
                    }
                    DataReceived?.Invoke(this, info);
                    return;
                }
            }

            _consecutiveErrors++;
            if (_consecutiveErrors >= AppConstants.MaxConsecutiveErrors && IsConnected)
            {
                IsConnected = false;
                ConnectionStatus = Translator.T("连接已断开");
                ConnectionStateChanged?.Invoke(this, false);
            }
        }
        catch (Exception ex)
        {
            _consecutiveErrors++;

            // 按次打 ERROR 是不行的：这里每个刷新周期都被调到，采集端一断，
            // 同一行会把日志页和环形缓冲刷满（实际发生过）。只报第一次，
            // 之后的降到 DEBUG 并带上次数，恢复时上面那条 Information 收尾。
            if (_consecutiveErrors == 1)
            {
                Log.Error(ex, "读取硬件数据失败");
            }
            else
            {
                Log.Debug(ex, "读取硬件数据仍在失败（第 {Count} 次）", _consecutiveErrors);
            }

            // 用 == 而不是 >=：>= 会让这条通知在越过阈值之后的每一个周期都重发一次，
            // 状态栏和日志跟着一起刷。
            if (_consecutiveErrors == AppConstants.MaxConsecutiveErrors)
            {
                IsConnected = false;
                ConnectionStatus = Translator.T("错误: {0}", ex.Message);
                ErrorOccurred?.Invoke(this, ex.Message);
            }
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        
        _timer.Stop();
        _cts?.Cancel();
        _cts?.Dispose();
        _ipcService.Dispose();
        
        GC.SuppressFinalize(this);
    }
}
