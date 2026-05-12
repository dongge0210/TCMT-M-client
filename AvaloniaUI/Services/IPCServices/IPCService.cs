using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Avalonia.Threading;
using Serilog;

namespace AvaloniaUI.Services.IPC;

/// <summary>
/// IPC 统一服务：编排 Pipe（收 Schema）+ SharedMemory（读数据）。
/// 对外暴露连接状态、Schema 版本、数据就绪事件。
/// 版本不匹配时通知 UI 弹窗，不退出。
/// </summary>
public class IPCService : IDisposable
{
    private readonly IPCPipeClient _pipeClient = new();
    private readonly IPCMemoryReader _memoryReader = new();
    private CancellationTokenSource? _cts;
    private bool _disposed;

    // Schema 字段索引缓存，避免每次按名称查找
    private Dictionary<string, FieldDef> _fieldCache = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// 连接状态变化
    /// </summary>
    public event Action<bool, string>? ConnectionStateChanged;

    /// <summary>
    /// Schema 版本不匹配，UI 应弹窗提示用户
    /// </summary>
    public event Action<string>? VersionMismatch;

    /// <summary>
    /// 数据就绪（Schema 已接收 + 共享内存已打开）
    /// </summary>
    public event Action? DataReady;

    public bool IsConnected => _pipeClient.IsConnected;
    public bool IsMemoryOpen => _memoryReader.IsOpen;
    public IPCMemoryReader Reader => _memoryReader;

    /// <summary>
    /// 启动 IPC 服务
    /// </summary>
    public void Start()
    {
        if (_disposed) return;
        _cts = new CancellationTokenSource();

        _pipeClient.OnConnectionChanged = (connected, msg) =>
        {
            if (!connected)
            {
                // Server shutdown → close memory reader so timer stops reading stale data
                _memoryReader.Close();
            }
            Dispatcher.UIThread.Post(() =>
            {
                ConnectionStateChanged?.Invoke(connected, msg);
            });
        };

        _pipeClient.OnSchemaReceived = OnSchemaReceived;

        // 后台运行 Pipe 客户端
        Task.Run(() => _pipeClient.StartAsync(_cts.Token));
    }

    private bool OnSchemaReceived(SchemaMessage schema)
    {
        if (!schema.IsValid)
        {
            Log.Warning("IPC: Invalid schema received");
            Dispatcher.UIThread.Post(() =>
            {
                VersionMismatch?.Invoke($"Schema 无效：magic=0x{schema.Header.Magic:X}");
            });
            return false;
        }

        if (schema.Header.Version != IPCConstants.CurrentVersion)
        {
            Log.Warning("IPC: Schema version mismatch: {Actual} vs {Expected}",
                schema.Header.Version, IPCConstants.CurrentVersion);
            Dispatcher.UIThread.Post(() =>
            {
                VersionMismatch?.Invoke(
                    $"Schema 版本不匹配\n" +
                    $"服务端版本: {schema.Header.Version}\n" +
                    $"客户端版本: {IPCConstants.CurrentVersion}\n" +
                    $"请联系开发者更新");
            });
            // 不断开，等待服务端推送新版本
            return true;
        }

        // 打开共享内存
        if (!_memoryReader.Open(schema))
        {
            Log.Error("IPC: Failed to open shared memory for schema");
            return false;
        }

        // 构建字段索引缓存
        _fieldCache.Clear();
        foreach (var f in schema.Fields)
        {
            if (!string.IsNullOrEmpty(f.Name))
                _fieldCache[f.Name] = f;
        }

        Log.Information("IPC: Schema accepted, {Count} fields cached", _fieldCache.Count);

        // 通知数据就绪
        Dispatcher.UIThread.Post(() =>
        {
            DataReady?.Invoke();
        });

        return true;
    }

    // --- 便捷读取方法（带字段缓存） ---

    public byte? ReadUInt8(string name) => _memoryReader.ReadUInt8(name);
    public ushort? ReadUInt16(string name) => _memoryReader.ReadUInt16(name);
    public uint? ReadUInt32(string name) => _memoryReader.ReadUInt32(name);
    public ulong? ReadUInt64(string name) => _memoryReader.ReadUInt64(name);
    public sbyte? ReadInt8(string name) => _memoryReader.ReadInt8(name);
    public short? ReadInt16(string name) => _memoryReader.ReadInt16(name);
    public int? ReadInt32(string name) => _memoryReader.ReadInt32(name);
    public long? ReadInt64(string name) => _memoryReader.ReadInt64(name);
    public float? ReadFloat32(string name) => _memoryReader.ReadFloat32(name);
    public double? ReadFloat64(string name) => _memoryReader.ReadFloat64(name);
    public bool? ReadBool(string name) => _memoryReader.ReadBool(name);
    public string? ReadString(string name) => _memoryReader.ReadString(name);
    public string? ReadWString(string name) => _memoryReader.ReadWString(name);

    /// <summary>
    /// 检查 Schema 中是否包含指定字段
    /// </summary>
    public bool HasField(string name) => _fieldCache.ContainsKey(name);

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _cts?.Cancel();
        _cts?.Dispose();
        _ = _pipeClient.DisposeAsync();
        _memoryReader.Dispose();
    }
}
