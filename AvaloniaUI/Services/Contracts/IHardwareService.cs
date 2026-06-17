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
    
    Task StartAsync();
    Task StopAsync();
    Task ReconnectAsync();
}
