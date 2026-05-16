using AvaloniaUI.Models;
using Serilog;

namespace AvaloniaUI.Services.IPC;

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
            info.HyperThreading = reader.ReadBool("cpu/hyperThreading") ?? false;
            info.Virtualization = reader.ReadBool("cpu/virtualization") ?? false;
            info.CpuTemperature = reader.ReadFloat64("cpu/temperature") ?? (double?)reader.ReadFloat32("cpu/temperature") ?? 0;
            info.CpuUsageSampleIntervalMs = reader.ReadFloat64("cpu/sampleIntervalMs") ?? 500;

            // Memory
            info.TotalMemory = reader.ReadUInt64("memory/total") ?? 0;
            info.UsedMemory = reader.ReadUInt64("memory/used") ?? 0;
            info.AvailableMemory = reader.ReadUInt64("memory/available") ?? 0;
            info.CompressedMemory = reader.ReadUInt64("memory/compressed") ?? 0;

            // Battery
            info.BatteryPercent = reader.ReadInt32("battery/percent") ?? -1;
            info.AcOnline = reader.ReadBool("battery/acOnline") ?? false;

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
                int idx = 0;
                while (reader.HasField($"phys/{idx}/model") && idx < 8)
                {
                    var model = reader.ReadWString($"phys/{idx}/model") ?? "";
                    var serial = reader.ReadWString($"phys/{idx}/serial") ?? "";
                    var capacity = reader.ReadUInt64($"phys/{idx}/capacity") ?? 0;
                    var iface = reader.ReadWString($"phys/{idx}/interface") ?? "";
                    var temp = reader.ReadFloat64($"phys/{idx}/temperature") ?? 0;
                    var health = reader.ReadUInt8($"phys/{idx}/health") ?? 0;
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
                        info.PhysicalDisks.Add(new PhysicalDiskSmartData
                        {
                            Model = model,
                            SerialNumber = serial,
                            Capacity = capacity,
                            InterfaceType = iface,
                            Temperature = temp,
                            HealthPercentage = health,
                            SmartSupported = supported,
                            LogicalDriveLetters = driveLetters
                        });
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
                Log.Debug("IPC WiFi: hasField powerOn={Pwr} ssid={Ssid} rssi={Rssi} ch={Ch}",
                    info.HasWiFi, info.WifiSSID, info.WifiRSSI, info.WifiChannel);
            }
            else
            {
                Log.Warning("IPC WiFi: wifi/powerOn field NOT in schema!");
            }

            // Bluetooth (optional fields — may not exist in older schema)
            if (reader.HasField("bluetooth/powerOn"))
            {
                info.HasBluetooth = reader.ReadBool("bluetooth/powerOn") ?? false;
                info.BtPowerOn = reader.ReadBool("bluetooth/powerOn") ?? false;
                info.BtDeviceCount = reader.ReadInt32("bluetooth/deviceCount") ?? 0;
            }

            return info;
        }
        catch (Exception ex)
        {
            Log.Error(ex, "IPC: Failed to read SystemInfo from shared memory");
            return null!;
        }
    }
}
