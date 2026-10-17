using System;
using System.Threading.Tasks;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.Services.Contracts;

public interface IHardwareService : IDisposable
{
    bool IsConnected { get; }
    string ConnectionStatus { get; }
    
    event EventHandler<SystemInfo>? DataReceived;
    event EventHandler<bool>? ConnectionStateChanged;
    event EventHandler<string>? ErrorOccurred;

    /// <summary>采集端推来的配置快照，JSON 对象文本（详见 IPCService.ConfigSnapshotReceived）。</summary>
    event EventHandler<string>? ConfigSnapshotReceived;

    Task StartAsync();
    Task StopAsync();
    Task ReconnectAsync();

    /// <summary>把配置改动发给采集端。返回 false 表示没送出去 —— 没连着，或写失败。</summary>
    Task<bool> SendConfigAsync(string json);
}
