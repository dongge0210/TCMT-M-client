// IPCMemoryReader.cs - 基于 Schema 动态读取共享内存
using System;
using System.Collections.Generic;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Serilog;

namespace AvaloniaUI.Services.IPC;

public class IPCMemoryReader : IDisposable
{
    // Windows: MemoryMappedFile
    private MemoryMappedFile? _mmf;
    private MemoryMappedViewAccessor? _accessor;

    // macOS: POSIX shared memory (primary) or heap fallback
    private IntPtr _shmPtr = IntPtr.Zero;
    private int _shmFd = -1;
    private int _shmSize;
    private bool _shmIsMmap; // true = mmap'd, false = Marshal.AllocHGlobal (fallback)

    // POSIX constants
    private const int O_RDONLY = 0;
    private const int PROT_READ = 1;
    private const int MAP_SHARED = 1;
    private static readonly IntPtr MAP_FAILED = new IntPtr(-1);

    [DllImport("libc", EntryPoint = "shm_open", SetLastError = true)]
    private static extern int shm_open(string name, int oflag, int mode);

    [DllImport("libc", EntryPoint = "mmap", SetLastError = true)]
    private static extern IntPtr mmap(IntPtr addr, IntPtr length, int prot, int flags, int fd, IntPtr offset);

    [DllImport("libc", EntryPoint = "munmap", SetLastError = true)]
    private static extern int munmap(IntPtr addr, IntPtr length);

    [DllImport("libc", EntryPoint = "close", SetLastError = true)]
    private static extern int close(int fd);

    private SchemaMessage? _schema;
    private bool _disposed;
    private readonly object _lock = new();

    public bool IsOpen => OperatingSystem.IsMacOS()
        ? (_shmPtr != IntPtr.Zero && _shmPtr != MAP_FAILED)
        : (_accessor != null);
    public uint TotalSize => _schema?.Header.TotalSize ?? 0;

    public bool Open(SchemaMessage schema)
    {
        lock (_lock)
        {
            try
            {
                Close();
                _schema = schema;

                if (_schema.Header.TotalSize == 0)
                {
                    Log.Error("IPC Memory: Schema totalSize is 0");
                    return false;
                }

                if (OperatingSystem.IsMacOS())
                    return OpenMacOS();
                else
                    return OpenWindows();
            }
            catch (Exception ex)
            {
                Log.Error(ex, "IPC Memory: Failed to open shared memory");
                return false;
            }
        }
    }

    private bool OpenWindows()
    {
        // Match SharedMemoryManager_Windows.cpp fallback: Global → Local → no prefix
        string[] names = { "Global\\SystemMonitorSharedMemory",
                           "Local\\SystemMonitorSharedMemory",
                           "SystemMonitorSharedMemory" };
        try
        {
            var size = (long)_schema!.Header.TotalSize;
            foreach (var name in names)
            {
                try
                {
                    _mmf = MemoryMappedFile.OpenExisting(name, MemoryMappedFileRights.Read);
                    _accessor = _mmf.CreateViewAccessor(0, size, MemoryMappedFileAccess.Read);
                    Log.Information("IPC Memory: Opened {Name}, size={Size}", name, size);
                    return true;
                }
                catch (FileNotFoundException) { continue; }
            }
            Log.Error("IPC Memory: Shared memory not found in any namespace");
            return false;
        }
        catch (Exception ex)
        {
            Log.Error(ex, "IPC Memory: Windows shared memory open failed");
            return false;
        }
    }

    private bool OpenMacOS()
    {
        // C++ IPCServer creates shm via shm_open + mmap
        _shmFd = shm_open(IPCConstants.SharedMemoryPath, O_RDONLY, 0);
        if (_shmFd != -1)
        {
            _shmSize = (int)_schema!.Header.TotalSize;
            _shmPtr = mmap(IntPtr.Zero, (IntPtr)_shmSize, PROT_READ, MAP_SHARED, _shmFd, IntPtr.Zero);

            if (_shmPtr != MAP_FAILED)
            {
                _shmIsMmap = true;
                Log.Information("IPC Memory: Opened POSIX shm via shm_open/mmap, size={Size}", _shmSize);
                return true;
            }

            int err = Marshal.GetLastPInvokeError();
            _shmPtr = IntPtr.Zero;
            Log.Warning("IPC Memory: mmap failed (errno={Err}), falling back to FileStream", err);
            close(_shmFd);
            _shmFd = -1;
        }
        else
        {
            Log.Warning("IPC Memory: shm_open(/tcmt_ipc) failed (errno={Err}), falling back to FileStream",
                Marshal.GetLastPInvokeError());
        }

        // Fallback: try reading as a regular file (compatibility)
        return OpenMacOSFallback();
    }

    private bool OpenMacOSFallback()
    {
        string path = "/tmp/tcmt_shm.dat";
        if (!File.Exists(path))
        {
            Log.Error("IPC Memory: Fallback file {Path} not found either", path);
            return false;
        }

        try
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            var size = (int)Math.Min(fs.Length, (long)_schema!.Header.TotalSize);
            _shmSize = size;
            _shmPtr = Marshal.AllocHGlobal(size);
            _shmIsMmap = false;
            var buf = new byte[size];
            fs.ReadExactly(buf, 0, size);
            Marshal.Copy(buf, 0, _shmPtr, size);
            Log.Warning("IPC Memory: Opened via FileStream fallback, size={Size}", size);
            return true;
        }
        catch (Exception ex)
        {
            Log.Error(ex, "IPC Memory: Fallback FileStream also failed");
            return false;
        }
    }

    /// <summary>
    /// 刷新读取（每次 UI 刷新前调用）— mmap/MemoryMappedFile 模式下数据始终更新，无需刷新
    /// </summary>
    public void Refresh()
    {
        // Data is live via mmap / MemoryMappedFile; no refresh needed.
    }

    /// <summary>
    /// 读取 seqlock 序号（总在 offset 0）
    /// </summary>
    private uint ReadWriteSequence()
    {
        if (OperatingSystem.IsMacOS())
            return _shmPtr != IntPtr.Zero && _shmPtr != MAP_FAILED
                ? (uint)Marshal.ReadInt32(_shmPtr, 0) : 0;
        if (_accessor != null)
            return _accessor.ReadUInt32(0);
        return 0;
    }

    /// <summary>
    /// 在 seqlock 保护下执行读操作：读 seq → 若奇数自旋 → 读数据 → 再读 seq 校验
    /// </summary>
    private bool ReadWithSequence(int offset, Action readAction)
    {
        const int maxAttempts = 3;
        const int maxSpin = 10;
        _ = offset; // 保留参数签名；seq 固定位于 offset 0

        for (int attempt = 0; attempt < maxAttempts; attempt++)
        {
            Thread.MemoryBarrier();
            uint seq1 = ReadWriteSequence();
            Thread.MemoryBarrier();

            if ((seq1 & 1) == 1) // 奇数 = 写入中
            {
                bool becameEven = false;
                for (int spin = 0; spin < maxSpin; spin++)
                {
                    Thread.SpinWait(5);
                    Thread.MemoryBarrier();
                    uint s = ReadWriteSequence();
                    Thread.MemoryBarrier();
                    if ((s & 1) == 0) { seq1 = s; becameEven = true; break; }
                }
                if (!becameEven)
                {
                    Thread.Sleep(0); // 让出时间片再重试
                    continue;
                }
            }

            readAction();

            Thread.MemoryBarrier();
            uint seq2 = ReadWriteSequence();
            Thread.MemoryBarrier();

            if (seq2 == seq1) return true;
        }
        return false;
    }

    public bool HasField(string name)
    {
        if (_schema == null) return false;
        foreach (var f in _schema.Fields)
        {
            if (f.Name.Equals(name, StringComparison.OrdinalIgnoreCase))
                return true;
        }
        return false;
    }

    // --- 基础类型读取 ---

    public byte? ReadUInt8(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        if (field.Size < 1) return null;

        byte? result = null;
        bool ok = ReadWithSequence(0, () => {
            if (OperatingSystem.IsMacOS())
                result = Marshal.ReadByte(_shmPtr, (int)field.Offset);
            else if (_accessor != null)
                result = _accessor.ReadByte((int)field.Offset);
        });
        return ok ? result : null;
    }

    public ushort? ReadUInt16(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        if (field.Size < 2) return null;

        ushort? result = null;
        bool ok = ReadWithSequence(0, () => {
            if (OperatingSystem.IsMacOS())
                result = (ushort)Marshal.ReadInt16(_shmPtr, (int)field.Offset);
            else if (_accessor != null)
                result = _accessor.ReadUInt16((int)field.Offset);
        });
        return ok ? result : null;
    }

    public uint? ReadUInt32(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        if (field.Size < 4) return null;

        uint? result = null;
        bool ok = ReadWithSequence(0, () => {
            if (OperatingSystem.IsMacOS())
                result = (uint)Marshal.ReadInt32(_shmPtr, (int)field.Offset);
            else if (_accessor != null)
                result = _accessor.ReadUInt32((int)field.Offset);
        });
        return ok ? result : null;
    }

    public ulong? ReadUInt64(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        if (field.Size < 8) return null;

        ulong? result = null;
        bool ok = ReadWithSequence(0, () => {
            if (OperatingSystem.IsMacOS())
                result = (ulong)Marshal.ReadInt64(_shmPtr, (int)field.Offset);
            else if (_accessor != null)
                result = _accessor.ReadUInt64((int)field.Offset);
        });
        return ok ? result : null;
    }

    public sbyte? ReadInt8(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        if (field.Size < 1) return null;

        sbyte? result = null;
        bool ok = ReadWithSequence(0, () => {
            if (OperatingSystem.IsMacOS())
                result = (sbyte)Marshal.ReadByte(_shmPtr, (int)field.Offset);
            else if (_accessor != null)
                result = _accessor.ReadSByte((int)field.Offset);
        });
        return ok ? result : null;
    }

    public short? ReadInt16(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        if (field.Size < 2) return null;

        short? result = null;
        bool ok = ReadWithSequence(0, () => {
            if (OperatingSystem.IsMacOS())
                result = Marshal.ReadInt16(_shmPtr, (int)field.Offset);
            else if (_accessor != null)
                result = _accessor.ReadInt16((int)field.Offset);
        });
        return ok ? result : null;
    }

    public int? ReadInt32(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        if (field.Size < 4) return null;

        int? result = null;
        bool ok = ReadWithSequence(0, () => {
            if (OperatingSystem.IsMacOS())
                result = Marshal.ReadInt32(_shmPtr, (int)field.Offset);
            else if (_accessor != null)
                result = _accessor.ReadInt32((int)field.Offset);
        });
        return ok ? result : null;
    }

    public long? ReadInt64(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        if (field.Size < 8) return null;

        long? result = null;
        bool ok = ReadWithSequence(0, () => {
            if (OperatingSystem.IsMacOS())
                result = Marshal.ReadInt64(_shmPtr, (int)field.Offset);
            else if (_accessor != null)
                result = _accessor.ReadInt64((int)field.Offset);
        });
        return ok ? result : null;
    }

    public float? ReadFloat32(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        if (field.Size < 4) return null;

        float? result = null;
        bool ok = ReadWithSequence(0, () => {
            if (OperatingSystem.IsMacOS())
            {
                int bits = Marshal.ReadInt32(_shmPtr, (int)field.Offset);
                result = BitConverter.Int32BitsToSingle(bits);
            }
            else if (_accessor != null)
                result = _accessor.ReadSingle((int)field.Offset);
        });
        return ok ? result : null;
    }

    public double? ReadFloat64(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        if (field.Size < 8) return null;

        double? result = null;
        bool ok = ReadWithSequence(0, () => {
            if (OperatingSystem.IsMacOS())
            {
                long bits = Marshal.ReadInt64(_shmPtr, (int)field.Offset);
                result = BitConverter.Int64BitsToDouble(bits);
            }
            else if (_accessor != null)
                result = _accessor.ReadDouble((int)field.Offset);
        });
        return ok ? result : null;
    }

    public bool? ReadBool(string fieldName)
    {
        var b = ReadUInt8(fieldName);
        return b.HasValue ? b.Value != 0 : null;
    }

    private byte[]? ReadBytes(int offset, int count)
    {
        byte[]? buf = null;
        bool ok = ReadWithSequence(0, () => {
            buf = new byte[count];
            if (OperatingSystem.IsMacOS())
                Marshal.Copy(new IntPtr(_shmPtr.ToInt64() + offset), buf, 0, count);
            else if (_accessor != null)
                _accessor.ReadArray(offset, buf, 0, count);
            else
                buf = null;
        });
        return ok ? buf : null;
    }

    public string? ReadString(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        int maxLen = (int)Math.Min(field.Size, _schema!.Header.TotalSize - field.Offset);
        if (maxLen <= 0) return null;

        var buf = ReadBytes((int)field.Offset, maxLen);
        if (buf == null) return null;

        int len = 0;
        while (len < buf.Length && buf[len] != 0) len++;
        if (len == 0) return string.Empty;
        return Encoding.UTF8.GetString(buf, 0, len);
    }

    // --- WString 支持 ---

    public string? ReadWString(string fieldName)
    {
        var field = FindField(fieldName); if (field == null) return null;
        int maxBytes = (int)Math.Min(field.Size, _schema!.Header.TotalSize - field.Offset);
        if (maxBytes <= 0) return null;

        // macOS: IPCDataBlock uses char[] not WCHAR, fall through to String read
        if (OperatingSystem.IsMacOS())
            return ReadString(fieldName);

        var buf = ReadBytes((int)field.Offset, maxBytes);
        if (buf == null) return null;

        int len = 0;
        while (len + 1 < buf.Length && (buf[len] != 0 || buf[len + 1] != 0)) len += 2;
        if (len == 0) return string.Empty;
        return Encoding.Unicode.GetString(buf, 0, len);
    }

    // --- 按 FieldDef 读取 ---
    public object? ReadField(FieldDef field)
    {
        return (FieldType)field.Type switch
        {
            FieldType.UInt8   => ReadUInt8(field.Name),
            FieldType.Int8    => ReadInt8(field.Name),
            FieldType.UInt16  => ReadUInt16(field.Name),
            FieldType.Int16   => ReadInt16(field.Name),
            FieldType.UInt32  => ReadUInt32(field.Name),
            FieldType.Int32   => ReadInt32(field.Name),
            FieldType.UInt64  => ReadUInt64(field.Name),
            FieldType.Int64   => ReadInt64(field.Name),
            FieldType.Float32 => ReadFloat32(field.Name),
            FieldType.Float64 => ReadFloat64(field.Name),
            FieldType.Bool    => ReadBool(field.Name),
            FieldType.String  => ReadString(field.Name),
            FieldType.WString => ReadWString(field.Name),
            _ => null
        };
    }

    public Dictionary<string, object?> ReadAllFields()
    {
        var result = new Dictionary<string, object?>();
        if (_schema == null) return result;
        foreach (var f in _schema.Fields)
            result[f.Name] = ReadField(f);
        return result;
    }

    private FieldDef? FindField(string name)
    {
        if (_schema == null) return null;
        foreach (var f in _schema.Fields)
            if (f.Name.Equals(name, StringComparison.OrdinalIgnoreCase))
                return f;
        return null;
    }

    public void Close()
    {
        lock (_lock)
        {
            if (OperatingSystem.IsMacOS())
            {
                if (_shmPtr != IntPtr.Zero && _shmPtr != MAP_FAILED)
                {
                    if (_shmIsMmap)
                        munmap(_shmPtr, (IntPtr)_shmSize);
                    else
                        Marshal.FreeHGlobal(_shmPtr);
                    _shmPtr = IntPtr.Zero;
                }
                if (_shmFd != -1)
                {
                    close(_shmFd);
                    _shmFd = -1;
                }
                _shmSize = 0;
            }
            else
            {
                _accessor?.Dispose();
                _mmf?.Dispose();
                _accessor = null;
                _mmf = null;
            }

            _schema = null;
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        Close();
    }
}
