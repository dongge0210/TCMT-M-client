using System;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Threading;
using Serilog;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;
using TCMT.Avalonia.Services.Contracts;
using TCMT.Avalonia.Services.Hardware.IPC;

namespace TCMT.Avalonia.Services.Hardware;

public class HardwareMonitorService : IHardwareService
{
    private readonly IPCService _ipcService;
    private readonly DispatcherTimer _timer;
    private CancellationTokenSource? _cts;
    private int _consecutiveErrors;
    private bool _disposed;

    public bool IsConnected { get; private set; }
    public string ConnectionStatus { get; private set; } = "连接中...";

    public event EventHandler<SystemInfo>? DataReceived;
    public event EventHandler<bool>? ConnectionStateChanged;
    public event EventHandler<string>? ErrorOccurred;

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
            ConnectionStatus = connected ? $"已连接 ({msg})" : $"已断开 ({msg})";
            ConnectionStateChanged?.Invoke(this, connected);
        };

        _ipcService.DataReady += () =>
        {
            _consecutiveErrors = 0;
            IsConnected = true;
            ConnectionStatus = "已连接 (IPC)";
        };

        _ipcService.VersionMismatch += msg =>
        {
            ErrorOccurred?.Invoke(this, $"版本不匹配: {msg}");
        };
    }

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
        ConnectionStatus = "重新连接中...";
        
        await StopAsync();
        _ipcService.Dispose();
        
        try
        {
            await StartAsync();
        }
        catch (Exception ex)
        {
            Log.Error(ex, "Reconnect failed");
            IsConnected = false;
            ConnectionStatus = "重连失败";
            ErrorOccurred?.Invoke(this, $"重连失败: {ex.Message}");
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
                    _consecutiveErrors = 0;
                    if (!IsConnected)
                    {
                        IsConnected = true;
                        ConnectionStatus = "已连接 (IPC)";
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
                ConnectionStatus = "连接已断开";
                ConnectionStateChanged?.Invoke(this, false);
            }
        }
        catch (Exception ex)
        {
            Log.Error(ex, "Error reading hardware data");
            _consecutiveErrors++;
            
            if (_consecutiveErrors >= AppConstants.MaxConsecutiveErrors)
            {
                IsConnected = false;
                ConnectionStatus = $"错误: {ex.Message}";
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
