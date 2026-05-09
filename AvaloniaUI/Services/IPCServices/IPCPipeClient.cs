using System;
using System.IO;
using System.IO.Pipes;
using System.Net.Sockets;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Serilog;

namespace AvaloniaUI.Services.IPC;

/// <summary>
/// Pipe 客户端：连接 C++ 端的 Named Pipe(Windows) / Unix Domain Socket(macOS/Linux)
/// 接收 Schema 消息并触发回调。
/// </summary>
public class IPCPipeClient : IAsyncDisposable
{
    private CancellationTokenSource? _cts;
    private bool _disposed;
    private bool _serverShutdown;

    /// <summary>
    /// 收到 Schema 时的回调。返回 true 表示接受，false 表示拒绝（版本不匹配等）
    /// </summary>
    public Func<SchemaMessage, bool>? OnSchemaReceived { get; set; }

    /// <summary>
    /// 连接状态变化回调
    /// </summary>
    public Action<bool, string>? OnConnectionChanged { get; set; }

    public bool IsConnected { get; private set; }
    public string LastError { get; private set; } = string.Empty;

    public async Task StartAsync(CancellationToken ct = default)
    {
        if (_disposed) return;
        _cts = CancellationTokenSource.CreateLinkedTokenSource(ct);

        while (!_cts.Token.IsCancellationRequested)
        {
            if (_serverShutdown) break;
            try
            {
                if (OperatingSystem.IsWindows())
                    await ConnectWindowsAsync(_cts.Token);
                else
                    await ConnectUnixAsync(_cts.Token);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (Exception ex)
            {
                LastError = ex.Message;
                Log.Warning(ex, "Pipe connection lost, reconnecting in 1s...");
                IsConnected = false;
                OnConnectionChanged?.Invoke(false, LastError);
                await Task.Delay(1000, _cts.Token);
            }
        }
    }

    // --- Windows: Named Pipe ---
    private async Task ConnectWindowsAsync(CancellationToken ct)
    {
        var pipeName = IPCConstants.PipeName;
        Log.Debug("IPC: Connecting to Windows pipe: {Name}", pipeName);

        using var client = new NamedPipeClientStream(
            ".", pipeName,
            PipeDirection.InOut,
            PipeOptions.Asynchronous);

        await client.ConnectAsync(5000, ct);

        IsConnected = true;
        OnConnectionChanged?.Invoke(true, "已连接");
        Log.Information("IPC: Connected to Windows pipe: {Name}", pipeName);

        await ReceiveLoopAsync(client, ct);
    }

    // --- macOS/Linux: Unix Domain Socket ---
    private async Task ConnectUnixAsync(CancellationToken ct)
    {
        var socketPath = IPCConstants.UnixSocketPath;
        try
        {
            using var client = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            var ep = new UnixDomainSocketEndPoint(socketPath);
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            cts.CancelAfter(TimeSpan.FromSeconds(5));

            await client.ConnectAsync(ep, cts.Token);

            IsConnected = true;
            OnConnectionChanged?.Invoke(true, "已连接");
            Log.Information("IPC: Connected to Unix socket: {Path}", socketPath);

            await using var ns = new NetworkStream(client, ownsSocket: false);
            await ReceiveLoopAsync(ns, ct);
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            Log.Debug("IPC: Unix socket connect timeout");
        }
        catch (Exception ex)
        {
            Log.Debug("IPC: Unix socket connect failed: {Msg}", ex.Message);
        }
    }

    private const int MsgHeaderSize = 4;  // PipeMessage: type(1) + version(1) + payloadSize(2)

    private async Task ReceiveLoopAsync(Stream stream, CancellationToken ct)
    {
        int n;
        var msgBuf = new byte[MsgHeaderSize];

        if (!OperatingSystem.IsWindows())
        {
            // === macOS/Linux: Send HELLO (with client type = Avalonia) ===
            var hello = new byte[MsgHeaderSize + 1]; // header + 1-byte payload
            hello[0] = 0x01;  // Hello
            hello[1] = IPCConstants.CurrentVersion;
            hello[2] = 1;      // payloadSize (LE): 1 byte — client type
            hello[3] = 0;
            hello[4] = 0x01;  // ClientType::Avalonia
            await stream.WriteAsync(hello, ct);
            await stream.FlushAsync(ct);
            Log.Debug("IPC: HELLO sent (Avalonia)");

            // Receive HELLO_ACK
            n = await ReadFullAsync(stream, msgBuf, MsgHeaderSize, ct);
            if (n == 0 || msgBuf[0] != 0x02) // HelloAck
            {
                Log.Warning("IPC: Expected HELLO_ACK, got type={Type}", msgBuf[0]);
                return;
            }
            Log.Debug("IPC: HELLO_ACK received");
        }
        // Windows: NamedPipe fire-and-forget — schema arrives immediately, no handshake.

        // Read schema header
        int schemaHeaderSize = IPCConstants.SchemaHeaderSize;
        int maxFields = IPCConstants.MaxFields;
        int fieldDefSize = IPCConstants.FieldDefSize;
        var headerBuf = new byte[schemaHeaderSize];
        n = await ReadFullAsync(stream, headerBuf, schemaHeaderSize, ct);
        if (n < schemaHeaderSize) { Log.Warning("IPC: Incomplete schema header"); return; }

        ushort fieldCount = BitConverter.ToUInt16(headerBuf, 6);
        uint totalSize = BitConverter.ToUInt32(headerBuf, 8);
        int allFieldsSize = fieldCount * fieldDefSize;
        // Read ALL field bytes from socket (drain buffer), cache up to maxFields
        var fieldBuf = new byte[allFieldsSize];
        if (allFieldsSize > 0)
            await ReadFullAsync(stream, fieldBuf, allFieldsSize, ct);

        var raw = new byte[schemaHeaderSize + allFieldsSize];
        Buffer.BlockCopy(headerBuf, 0, raw, 0, schemaHeaderSize);
        if (allFieldsSize > 0) Buffer.BlockCopy(fieldBuf, 0, raw, schemaHeaderSize, allFieldsSize);
        var schema = SchemaMessage.Parse(raw);

        if (schema.Header.Magic != IPCConstants.Magic)
        {
            Log.Warning("IPC: Invalid magic 0x{Actual:X}", schema.Header.Magic);
            return;
        }
        Log.Information("IPC: Schema received: {Count} fields, totalSize={Size}", schema.Header.FieldCount, schema.Header.TotalSize);

        if (!(OnSchemaReceived?.Invoke(schema) ?? true))
        {
            Log.Warning("IPC: Schema rejected");
            return;
        }

        // === Phase 3: Send ACK ===
        var ack = new byte[MsgHeaderSize];
        ack[0] = 0x03; // Ack
        ack[1] = IPCConstants.CurrentVersion;
        await stream.WriteAsync(ack, ct);
        await stream.FlushAsync(ct);
        Log.Debug("IPC: ACK sent");

        // === Phase 4: Keep-alive loop ===
        while (!ct.IsCancellationRequested)
        {
            try
            {
                n = await ReadFullAsync(stream, msgBuf, MsgHeaderSize, ct);
                if (n == 0) break;
                byte msgType = msgBuf[0];
                if (msgType == 0x05) // Pong
                {
                    Log.Debug("IPC: PONG received");
                }
                else if (msgType == 0x07) // Shutdown
                {
                    Log.Information("IPC: Server shutting down");
                    OnConnectionChanged?.Invoke(false, "服务器已关闭");
                    _serverShutdown = true;
                    break;
                }
                else if (msgType == 0x08) // SchemaUpdate
                {
                    Log.Information("IPC: Schema update — re-reading...");
                    n = await ReadFullAsync(stream, headerBuf, schemaHeaderSize, ct);
                    if (n < schemaHeaderSize) break;
                    fieldCount = BitConverter.ToUInt16(headerBuf, 6);
                    allFieldsSize = Math.Min(fieldCount, maxFields) * fieldDefSize;
                    if (allFieldsSize > 0) await ReadFullAsync(stream, fieldBuf, allFieldsSize, ct);
                    raw = new byte[schemaHeaderSize + allFieldsSize];
                    Buffer.BlockCopy(headerBuf, 0, raw, 0, schemaHeaderSize);
                    if (allFieldsSize > 0) Buffer.BlockCopy(fieldBuf, 0, raw, schemaHeaderSize, allFieldsSize);
                    var newSchema = SchemaMessage.Parse(raw);
                    if (newSchema.IsValid)
                        OnSchemaReceived?.Invoke(newSchema);
                }
            }
            catch (IOException) { break; }
            catch (OperationCanceledException) { break; }
        }
        // Keep-alive ended — treat as disconnect (SHUTDOWN, EOF, or crash)
        if (!_serverShutdown)
        {
            Log.Warning("IPC: Connection lost (timeout/eof)");
            OnConnectionChanged?.Invoke(false, "连接已断开");
        }
        Log.Debug("IPC: Keep-alive loop ended");
    }

    private static async Task<int> ReadFullAsync(Stream stream, byte[] buf, int len, CancellationToken ct)
    {
        int offset = 0;
        while (offset < len)
        {
            int n = await stream.ReadAsync(buf.AsMemory(offset, len - offset), ct);
            if (n == 0) return offset;
            offset += n;
        }
        return offset;
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed) return;
        _disposed = true;
        _cts?.Cancel();
        _cts?.Dispose();
    }
}
