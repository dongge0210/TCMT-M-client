using TCMT.Avalonia.Models;
using Serilog;

namespace TCMT.Avalonia.Services.Hardware.IPC;

public static class IPCSystemInfoMapper
{
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

            // Power (mW)
            info.CpuPower = reader.ReadFloat64("power/cpu") ?? (double?)reader.ReadFloat32("power/cpu") ?? 0;
            info.GpuPower = reader.ReadFloat64("power/gpu") ?? (double?)reader.ReadFloat32("power/gpu") ?? 0;
            info.AnePower = reader.ReadFloat64("power/ane") ?? (double?)reader.ReadFloat32("power/ane") ?? 0;

            // OS
            info.OsVersion = reader.ReadString("os/version") ?? "";

            // GPU
            info.GpuName = ipc.ReadWString("gpu/0/name") ?? reader.ReadString("gpu/0/name") ?? "";
            info.GpuBrand = ipc.ReadWString("gpu/0/brand") ?? reader.ReadString("gpu/0/brand") ?? "";
            info.GpuMemory = reader.ReadUInt64("gpu/0/memory") ?? 0;
            info.GpuCoreFreq = reader.ReadFloat64("gpu/0/memoryPercent") ?? (double?)reader.ReadFloat32("gpu/0/memoryPercent") ?? 0;
            var gpuUsage = reader.ReadFloat64("gpu/0/usage") ?? (double?)reader.ReadFloat32("gpu/0/usage") ?? 0;
            info.GpuTemperature = reader.ReadFloat64("gpu/0/temperature") ?? (double?)reader.ReadFloat32("gpu/0/temperature") ?? 0;
            info.GpuIsVirtual = reader.ReadBool("gpu/0/isVirtual") ?? false;
            info.GpuFreq = reader.ReadFloat64("gpu/freq") ?? (double?)reader.ReadFloat32("gpu/freq") ?? 0;

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
                        IsVirtual = info.GpuIsVirtual,
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

                    if (total > 0)
                    {
                        info.Disks.Add(new DiskData
                        {
                            Letter = letter,
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

            // Temperatures
            if (reader.HasField("sensor/0/name"))
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
                Serilog.Log.Information("SMART: HasField phys/0/model=true, entering disk loop");
                int idx = 0;
                while (reader.HasField($"phys/{idx}/model") && idx < 8)
                {
                    var model = reader.ReadWString($"phys/{idx}/model") ?? "";
                    var serial = reader.ReadWString($"phys/{idx}/serial") ?? "";
                    var capacity = reader.ReadUInt64($"phys/{idx}/capacity") ?? 0;
                    Serilog.Log.Information("SMART disk[{Idx}]: model='{M}' capacity={C}", idx, model, capacity);
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
                                Serilog.Log.Information("SMART: parsed {Count} attrs from IPC", pd.Attributes.Count);
                            }
                            catch (Exception ex) {
                                Serilog.Log.Warning("SMART JSON parse error: {Err}", ex.Message);
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

            return info;
        }
        catch (Exception ex)
        {
            Log.Error(ex, "IPC: Failed to read SystemInfo from shared memory");
            return null!;
        }
    }
}
