using TCMT.Avalonia.Models;
using Serilog;

namespace TCMT.Avalonia.Services.Hardware.IPC;

public static class IPCSystemInfoMapper
{
    // ── 一份会重复犯的错只报一次 ─────────────────────────────────────────
    //
    // Read 每个刷新周期跑一次。这里的日志要是按次打，采集端一断、
    // 或者某段 JSON 一直不合规，同一行就会以每秒两次的速度把日志页和环形缓冲刷满。
    // 按约定，循环体里的失败本来就该压到 DEBUG，而"第一次"是真正的状态变化，值得说。
    //
    // 所以：第一次出错报 WARN，之后不再重复；恢复了（或者上报成功过一次）把钥匙销掉，
    // 下一次再坏还能报出来。
    private static readonly HashSet<string> ReportedFailures = new();
    private static readonly object ReportedLock = new();

    private static void LogFailureOnce(string key, string what, Exception ex)
    {
        lock (ReportedLock)
        {
            if (!ReportedFailures.Add(key)) return;
        }
        Log.Warning(ex, "{What} —— 后续同样的失败不再重复上报，恢复后重新计", what);
    }

    /// <summary>整份读成功一次，之前报过的失败就算过去了，下次再坏要能重新报出来。</summary>
    private static void ClearFailures()
    {
        lock (ReportedLock)
        {
            ReportedFailures.Clear();
        }
    }

    public static SystemInfo Read(IPCService ipc)
    {
        var reader = ipc.Reader;
        if (!reader.IsOpen)
            return null!;

        try
        {
            var info = new SystemInfo
            {
                LastUpdate = DateTime.Now
            };

            // CPU
            info.CpuName = ipc.ReadWString("cpu/name") ?? reader.ReadString("cpu/name") ?? "";
            info.PhysicalCores = reader.ReadInt32("cpu/cores/physical") ?? reader.ReadUInt8("cpu/cores/physical") ?? 0;
            info.LogicalCores = reader.ReadInt32("cpu/cores/logical") ?? reader.ReadUInt8("cpu/cores/logical") ?? 0;
            info.PerformanceCores = reader.ReadInt32("cpu/cores/performance") ?? reader.ReadUInt8("cpu/cores/performance") ?? 0;
            info.EfficiencyCores = reader.ReadInt32("cpu/cores/efficiency") ?? reader.ReadUInt8("cpu/cores/efficiency") ?? 0;
            info.CpuUsage = reader.ReadFloat64("cpu/usage") ?? (double?)reader.ReadFloat32("cpu/usage") ?? 0;
            info.PerformanceCoreFreq = reader.ReadFloat64("cpu/freq/pCore") ?? (double?)reader.ReadFloat32("cpu/freq/pCore") ?? 0;
            info.EfficiencyCoreFreq = reader.ReadFloat64("cpu/freq/eCore") ?? (double?)reader.ReadFloat32("cpu/freq/eCore") ?? 0;
            info.CpuBaseFreq = reader.ReadFloat64("cpu/freq/base") ?? (double?)reader.ReadFloat32("cpu/freq/base") ?? 0;
            info.HyperThreading = reader.ReadBool("cpu/hyperThreading") ?? false;
            info.Virtualization = reader.ReadBool("cpu/virtualization") ?? false;
            info.CpuTemperature = reader.ReadFloat64("cpu/temperature") ?? (double?)reader.ReadFloat32("cpu/temperature") ?? 0;
            info.CpuPcoreTemperature = reader.ReadFloat64("cpu/pcore/temperature") ?? (double?)reader.ReadFloat32("cpu/pcore/temperature") ?? 0;
            info.CpuEcoreTemperature = reader.ReadFloat64("cpu/ecore/temperature") ?? (double?)reader.ReadFloat32("cpu/ecore/temperature") ?? 0;
            info.CpuUsageSampleIntervalMs = reader.ReadFloat64("cpu/sampleIntervalMs") ?? 500;

            // Memory
            info.TotalMemory = reader.ReadUInt64("memory/total") ?? 0;
            info.UsedMemory = reader.ReadUInt64("memory/used") ?? 0;
            info.AvailableMemory = reader.ReadUInt64("memory/available") ?? 0;
            info.CompressedMemory = reader.ReadUInt64("memory/compressed") ?? 0;
            info.SwapUsed = reader.ReadUInt64("memory/swapUsed") ?? 0;
            info.SwapTotal = reader.ReadUInt64("memory/swapTotal") ?? 0;
            info.RamSpeed = reader.ReadUInt32("memory/ramSpeed") ?? 0;
            info.RamType = ipc.ReadWString("memory/ramType") ?? reader.ReadString("memory/ramType") ?? "";

            // Battery
            info.BatteryPercent = reader.ReadInt32("battery/percent") ?? -1;
            info.AcOnline = reader.ReadBool("battery/acOnline") ?? false;

            // ─── 7. Battery detail ───
            info.BatteryCycleCount = reader.ReadInt32("battery/cycleCount") ?? 0;
            info.BatteryDesignCapacity = reader.ReadInt32("battery/designCapacity") ?? 0;
            info.BatteryMaxCapacity = reader.ReadInt32("battery/maxCapacity") ?? 0;
            info.BatteryHealthPercent = reader.ReadFloat64("battery/healthPercent") ?? (double?)reader.ReadFloat32("battery/healthPercent") ?? 0;
            info.BatteryTemperature = reader.ReadFloat64("battery/temperature") ?? (double?)reader.ReadFloat32("battery/temperature") ?? 0;
            info.BatteryAmperage = reader.ReadInt32("battery/amperage") ?? 0;
            info.BatteryVoltage = reader.ReadInt32("battery/voltage") ?? 0;
            info.BatteryChargerWatts = reader.ReadFloat64("battery/chargerWatts") ?? (double?)reader.ReadFloat32("battery/chargerWatts") ?? 0;
            info.BatteryIsCharging = reader.ReadBool("battery/isCharging") ?? false;
            info.BatteryIsPresent = reader.ReadBool("battery/isPresent") ?? false;

            // ─── 4. Fan speeds ───
            if (reader.HasField("fan/0/name"))
            {
                int idx = 0;
                while (reader.HasField($"fan/{idx}/name") && idx < 6)
                {
                    var fName = reader.ReadString($"fan/{idx}/name") ?? "";
                    var rpm = reader.ReadFloat32($"fan/{idx}/rpm") ?? 0f;
                    if (rpm > 0)
                    {
                        info.Fans.Add(new FanData { Name = fName, Rpm = rpm });
                    }
                    idx++;
                }
            }

            // ─── 5. 进程列表 ───
            // 新服务端整份打包成一行 JSON；旧服务端是 proc/N/* 槽位，而槽位只有 7 个，
            // 采集端枚举了全部进程却在写入时被砍到 7。两条都认：服务端与客户端不一定同时升级。
            if (reader.HasField("proc/json"))
            {
                var procsJson = reader.ReadString("proc/json") ?? "";
                if (!string.IsNullOrEmpty(procsJson))
                {
                    try
                    {
                        var doc = System.Text.Json.JsonDocument.Parse(procsJson);
                        foreach (var el in doc.RootElement.EnumerateArray())
                        {
                            var pName = el.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "";
                            if (string.IsNullOrEmpty(pName)) continue;
                            info.TopProcesses.Add(new ProcessTopEntry
                            {
                                Pid = el.TryGetProperty("pid", out var p) && p.TryGetInt32(out var pid) ? pid : 0,
                                Name = pName,
                                MemoryBytes = el.TryGetProperty("mem", out var m) && m.TryGetUInt64(out var mem) ? mem : 0UL,
                                CpuPercent = el.TryGetProperty("cpu", out var c) && c.TryGetDouble(out var cpu) ? (float)cpu : 0f
                            });
                        }
                    }
                    catch (Exception ex)
                    {
                        LogFailureOnce("proc-json", "进程列表 JSON 解析失败", ex);
                    }
                }
            }
            else if (reader.HasField("proc/0/name"))
            {
                int idx = 0;
                while (reader.HasField($"proc/{idx}/name") && idx < 7)
                {
                    var pName = reader.ReadString($"proc/{idx}/name") ?? "";
                    if (string.IsNullOrEmpty(pName)) { idx++; continue; }
                    info.TopProcesses.Add(new ProcessTopEntry
                    {
                        Pid = reader.ReadInt32($"proc/{idx}/pid") ?? 0,
                        Name = pName,
                        MemoryBytes = reader.ReadUInt64($"proc/{idx}/memory") ?? 0,
                        CpuPercent = reader.ReadFloat32($"proc/{idx}/cpu") ?? 0f
                    });
                    idx++;
                }
            }

            // ─── 6. Per-core sensors ───
            var coreCount = reader.ReadUInt8("core/count") ?? 0;
            for (int i = 0; i < coreCount && i < 16; i++)
            {
                var temp = reader.ReadFloat32($"core/{i}/temp") ?? 0f;
                var freq = reader.ReadFloat32($"core/{i}/freq") ?? 0f;
                if (temp > 0 || freq > 0)
                {
                    info.PerCores.Add(new PerCoreData { Index = i, Temperature = temp, Frequency = freq });
                }
            }

            // ─── System info ───
            info.LoadAvg1 = reader.ReadFloat32("system/loadAvg1") ?? 0f;
            info.LoadAvg5 = reader.ReadFloat32("system/loadAvg5") ?? 0f;
            info.LoadAvg15 = reader.ReadFloat32("system/loadAvg15") ?? 0f;
            info.ProcessCount = reader.ReadInt32("system/processCount") ?? 0;
            info.UptimeSeconds = reader.ReadUInt64("system/uptime") ?? 0;

            // Power (mW)
            info.CpuPower = reader.ReadFloat64("power/cpu") ?? (double?)reader.ReadFloat32("power/cpu") ?? 0;
            info.GpuPower = reader.ReadFloat64("power/gpu") ?? (double?)reader.ReadFloat32("power/gpu") ?? 0;
            info.AnePower = reader.ReadFloat64("power/ane") ?? (double?)reader.ReadFloat32("power/ane") ?? 0;

            // OS
            info.OsVersion = reader.ReadString("os/version") ?? "";
            /* 采集端按窄字符串注册（addS("os/model", …)），宽串读不到再回落窄串。 */
            info.HardwareModel = ipc.ReadWString("os/model") ?? reader.ReadString("os/model") ?? "";

            // GPU
            info.GpuName = ipc.ReadWString("gpu/0/name") ?? reader.ReadString("gpu/0/name") ?? "";
            info.GpuBrand = ipc.ReadWString("gpu/0/brand") ?? reader.ReadString("gpu/0/brand") ?? "";
            info.GpuMemory = reader.ReadUInt64("gpu/0/memory") ?? 0;
            info.GpuCoreFreq = reader.ReadFloat64("gpu/freq") ?? (double?)reader.ReadFloat32("gpu/freq") ?? 0;
            info.GpuMemoryPercent = reader.ReadFloat64("gpu/0/memoryPercent") ?? (double?)reader.ReadFloat32("gpu/0/memoryPercent") ?? 0;
            var gpuUsage = reader.ReadFloat64("gpu/0/usage") ?? (double?)reader.ReadFloat32("gpu/0/usage") ?? 0;
            info.GpuTemperature = reader.ReadFloat64("gpu/0/temperature") ?? (double?)reader.ReadFloat32("gpu/0/temperature") ?? 0;
            info.GpuIsVirtual = reader.ReadBool("gpu/0/isVirtual") ?? false;
            // 老版本采集端没这个字段 —— 缺席就是「不是统一内存」，Windows 上也一直是 false
            info.GpuIsUnified = reader.ReadBool("gpu/0/unified") ?? false;

            if (!string.IsNullOrEmpty(info.GpuName))
            {
                info.Gpus = new List<GpuData>
                {
                    new GpuData
                    {
                        Name = info.GpuName,
                        Brand = info.GpuBrand,
                        Memory = info.GpuMemory,
                        Usage = gpuUsage,
                        CoreClock = info.GpuCoreFreq,
                        MemoryPercent = info.GpuMemoryPercent,
                        IsVirtual = info.GpuIsVirtual,
                        IsUnified = info.GpuIsUnified,
                        Temperature = info.GpuTemperature
                    }
                };
            }

            // Network
            if (reader.HasField("net/0/name"))
            {
                int idx = 0;
                while (reader.HasField($"net/{idx}/name") && idx < 4)
                {
                    var name = ipc.ReadWString($"net/{idx}/name") ?? reader.ReadString($"net/{idx}/name") ?? "";
                    var ip = reader.ReadWString($"net/{idx}/ip") ?? "";
                    var mac = reader.ReadWString($"net/{idx}/mac") ?? "";
                    // 当前生效的链路地址。旧服务端没这个字段，HasField 守卫后留空。
                    var macCurrent = reader.HasField($"net/{idx}/macCurrent")
                        ? (reader.ReadWString($"net/{idx}/macCurrent") ?? "")
                        : "";
                    var type = reader.ReadWString($"net/{idx}/type") ?? "";
                    var speed = reader.ReadUInt64($"net/{idx}/speed") ?? 0;
                    var dl = reader.ReadUInt64($"net/{idx}/downloadSpeed") ?? 0;
                    var ul = reader.ReadUInt64($"net/{idx}/uploadSpeed") ?? 0;

                    if (!string.IsNullOrEmpty(ip) || !string.IsNullOrEmpty(mac))
                    {
                        info.Adapters.Add(new NetworkAdapterData
                        {
                            Name = name,
                            IpAddress = ip,
                            Mac = mac,
                            CurrentMac = macCurrent,
                            AdapterType = type,
                            Speed = speed,
                            DownloadSpeed = dl,
                            UploadSpeed = ul
                        });
                    }
                    idx++;
                }
            }

            // Legacy flat fields (fallback)
            if (info.Adapters.Count == 0)
            {
                info.NetworkAdapterName = ipc.ReadWString("net/0/name") ?? reader.ReadWString("net/0/name") ?? "";
                info.NetworkAdapterMac = ipc.ReadWString("net/0/mac") ?? reader.ReadWString("net/0/mac") ?? "";
                info.NetworkAdapterIp = ipc.ReadWString("net/0/ip") ?? reader.ReadWString("net/0/ip") ?? "";
                info.NetworkAdapterType = ipc.ReadWString("net/0/type") ?? reader.ReadWString("net/0/type") ?? "";
                info.NetworkAdapterSpeed = reader.ReadUInt64("net/0/speed") ?? 0;

                if (!string.IsNullOrEmpty(info.NetworkAdapterName))
                {
                    info.Adapters.Add(new NetworkAdapterData
                    {
                        Name = info.NetworkAdapterName,
                        Mac = info.NetworkAdapterMac,
                        IpAddress = info.NetworkAdapterIp,
                        AdapterType = info.NetworkAdapterType,
                        Speed = info.NetworkAdapterSpeed
                    });
                }
            }

            // Disks
            if (reader.HasField("disk/0/label"))
            {
                int idx = 0;
                while (reader.HasField($"disk/{idx}/label") && idx < 4)
                {
                    var label = reader.ReadWString($"disk/{idx}/label") ?? "";
                    var total = reader.ReadUInt64($"disk/{idx}/total") ?? 0;
                    var letter = (char)(reader.ReadUInt8($"disk/{idx}/letter") ?? 0);
                    var used = reader.ReadUInt64($"disk/{idx}/used") ?? 0;
                    var free = reader.ReadUInt64($"disk/{idx}/free") ?? 0;
                    var fs = reader.ReadWString($"disk/{idx}/fs") ?? "";
                    // 挂载点。旧服务端没这个字段，HasField 守卫后留空。
                    var mountPoint = reader.HasField($"disk/{idx}/mountPoint")
                        ? (reader.ReadWString($"disk/{idx}/mountPoint") ?? "")
                        : "";

                    if (total > 0)
                    {
                        info.Disks.Add(new DiskData
                        {
                            Letter = letter,
                            MountPoint = mountPoint,
                            Label = label,
                            FileSystem = fs,
                            TotalSize = total,
                            UsedSpace = used,
                            FreeSpace = free,
                            PhysicalDiskIndex = -1
                        });
                    }
                    idx++;
                }
            }

            // 温度传感器。
            // 新服务端把整份打包成一行 JSON；旧服务端是 sensor/N/* 的槽位字段 —— 而槽位只有
            // 10 个，采集端实际产出 20 个，一半会被砍掉。两条都认：服务端与客户端不一定同时升级。
            if (reader.HasField("sensors/json"))
            {
                var sensorsJson = reader.ReadString("sensors/json") ?? "";
                if (!string.IsNullOrEmpty(sensorsJson))
                {
                    try
                    {
                        var doc = System.Text.Json.JsonDocument.Parse(sensorsJson);
                        foreach (var el in doc.RootElement.EnumerateArray())
                        {
                            info.Temperatures.Add(new TemperatureData
                            {
                                SensorName = el.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "",
                                Temperature = el.TryGetProperty("value", out var v) && v.TryGetDouble(out var d) ? d : 0
                            });
                        }
                    }
                    catch (Exception ex)
                    {
                        LogFailureOnce("sensors-json", "温度传感器 JSON 解析失败", ex);
                    }
                }
            }
            else if (reader.HasField("sensor/0/name"))
            {
                int idx = 0;
                while (reader.HasField($"sensor/{idx}/name") && idx < 10)
                {
                    var name = reader.ReadWString($"sensor/{idx}/name") ?? "";
                    var temp = reader.ReadFloat64($"sensor/{idx}/value") ?? (double?)reader.ReadFloat32($"sensor/{idx}/value") ?? 0;
                    info.Temperatures.Add(new TemperatureData
                    {
                        SensorName = name,
                        Temperature = temp
                    });
                    idx++;
                }
            }

            // Physical disks (SMART)
            if (reader.HasField("phys/0/model"))
            {
                int idx = 0;
                while (reader.HasField($"phys/{idx}/model") && idx < 8)
                {
                    var model = reader.ReadWString($"phys/{idx}/model") ?? "";
                    var serial = reader.ReadWString($"phys/{idx}/serial") ?? "";
                    var capacity = reader.ReadUInt64($"phys/{idx}/capacity") ?? 0;
                    var iface = reader.ReadWString($"phys/{idx}/interface") ?? "";
                    var temp = reader.ReadFloat64($"phys/{idx}/temperature") ?? 0;
                    var health = (byte)(reader.ReadFloat32($"phys/{idx}/health") ?? 0f);
                    var supported = reader.ReadBool($"phys/{idx}/smartSupported") ?? false;

                    // Read logical drive letters
                    var driveLetters = new System.Collections.Generic.List<char>();
                    int letterCount = reader.ReadInt32($"phys/{idx}/letterCount") ?? 0;
                    for (int j = 0; j < 8; j++)
                    {
                        byte b = reader.ReadUInt8($"phys/{idx}/letter{j}") ?? 0;
                        if (b >= (byte)'A' && b <= (byte)'Z') driveLetters.Add((char)b);
                    }

                    if (capacity > 0)
                    {
                        var pd = new PhysicalDiskSmartData
                        {
                            Model = model,
                            SerialNumber = serial,
                            Capacity = capacity,
                            InterfaceType = iface,
                            Temperature = temp,
                            HealthPercentage = health,
                            SmartSupported = supported,
                            LogicalDriveLetters = driveLetters
                        };

                        // Read SMART attributes JSON
                        var attrCount = reader.ReadInt32($"phys/{idx}/attrCount") ?? 0;
                        var attrsJson = reader.ReadString($"phys/{idx}/attrsJson") ?? "";
                        if (attrCount > 0 && !string.IsNullOrEmpty(attrsJson))
                        {
                            try
                            {
                                var doc = System.Text.Json.JsonDocument.Parse(attrsJson);
                                foreach (var el in doc.RootElement.EnumerateArray())
                                {
                                    pd.Attributes.Add(new SmartAttributeData
                                    {
                                        Id = el.TryGetProperty("id", out var id) ? (byte)id.GetUInt32() : (byte)0,
                                        Current = el.TryGetProperty("cur", out var cur) ? (byte)cur.GetUInt32() : (byte)0,
                                        Worst = el.TryGetProperty("worst", out var worst) ? (byte)worst.GetUInt32() : (byte)0,
                                        RawValue = el.TryGetProperty("raw", out var raw) ? raw.GetUInt64() : 0UL,
                                        Name = el.TryGetProperty("name", out var name) ? name.GetString() ?? "" : "",
                                        Description = el.TryGetProperty("desc", out var desc) ? desc.GetString() ?? "" : ""
                                    });
                                }
                            }
                            catch (Exception ex) {
                                LogFailureOnce("smart-json", "SMART 属性 JSON 解析失败", ex);
                            }
                        }

                        info.PhysicalDisks.Add(pd);
                    }
                    idx++;
                }
            }

            // WiFi (optional fields — may not exist in older schema)
            if (reader.HasField("wifi/powerOn"))
            {
                info.HasWiFi = reader.ReadBool("wifi/powerOn") ?? false;
                info.WifiSSID = ipc.ReadWString("wifi/ssid") ?? reader.ReadString("wifi/ssid") ?? "";
                info.WifiRSSI = reader.ReadInt32("wifi/rssi") ?? 0;
                info.WifiChannel = reader.ReadInt32("wifi/channel") ?? 0;
                info.WifiSecurity = ipc.ReadWString("wifi/security") ?? reader.ReadString("wifi/security") ?? "";
                info.WifiBand = ipc.ReadWString("wifi/band") ?? reader.ReadString("wifi/band") ?? "";
                info.WifiGen = ipc.ReadWString("wifi/gen") ?? reader.ReadString("wifi/gen") ?? "";
            }

            // Bluetooth (optional fields — may not exist in older schema)
            if (reader.HasField("bluetooth/powerOn"))
            {
                info.HasBluetooth = reader.ReadBool("bluetooth/powerOn") ?? false;
                info.BtPowerOn = reader.ReadBool("bluetooth/powerOn") ?? false;
                info.BtDeviceCount = reader.ReadInt32("bluetooth/deviceCount") ?? 0;
                // 已连接设备名。旧服务端这个槽要么没写、要么装的是适配器名 —— 都留空处理。
                info.BtDeviceName = reader.HasField("bluetooth/name")
                    ? (ipc.ReadWString("bluetooth/name") ?? reader.ReadString("bluetooth/name") ?? "")
                    : "";
            }

            // TPM
            if (reader.HasField("tpm/count")) {
                var tpmCount = reader.ReadUInt8("tpm/count") ?? 0;
                if (tpmCount > 0) {
                    var manu = reader.ReadWString("tpm/manufacturer") ?? "";
                    var fw = reader.ReadWString("tpm/firmwareVersion") ?? "";
                    var status = reader.ReadUInt8("tpm/status") ?? 0;
                    var selfTest = reader.ReadUInt8("tpm/selfTestStatus") ?? 0;
                    var enabled = reader.ReadBool("tpm/isEnabled") ?? false;
                    var active = reader.ReadBool("tpm/isActive") ?? false;
                    info.Tpm = new TpmData {
                        Manufacturer = manu,
                        FirmwareVersion = fw,
                        Status = status == 1 ? "正常" : (status == 3 ? "禁用" : "未知"),
                        SelfTestStatus = selfTest == 0 ? "通过" : (selfTest == 1 ? "失败" : "未测试"),
                        IsEnabled = enabled,
                        IsActive = active,
                    };
                }
            }

            // App version (from IPC, fallback to default)
            var appVersion = reader.ReadString("app/version") ?? ipc.ReadWString("app/version");
            if (!string.IsNullOrEmpty(appVersion))
                info.Version = appVersion;

            ClearFailures();
            return info;
        }
        catch (Exception ex)
        {
            // 这不是 ERROR 级，也不是按次该报的：它每个刷新周期都会失败一次，
            // 而"读不到共享内存"由 HardwareMonitorService 那边统一判连接状态并上报。
            // 这里留 WARN-once 是为了留一条线索，不至于连为什么读不到都查不到。
            LogFailureOnce("read", "从共享内存读取整机信息失败", ex);
            return null!;
        }
    }
}
