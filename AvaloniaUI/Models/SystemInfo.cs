using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Runtime.CompilerServices;

namespace AvaloniaUI.Models
{
    public static class FormatUtil
    {
        public static string FormatBytes(ulong bytes)
        {
            string[] sizes = { "B", "KB", "MB", "GB", "TB" };
            double len = bytes;
            int order = 0;
            while (len >= 1024 && order < sizes.Length - 1)
            {
                order++;
                len /= 1024;
            }
            return order == 0 ? $"{len:0} {sizes[order]}" : $"{len:0.##} {sizes[order]}";
        }

        // 网络速度格式化 (值已经是 bps，直接格式化)
        public static string FormatNetworkSpeed(ulong bitsPerSec)
        {
            if (bitsPerSec == 0) return "0 B/s";
            string[] sizes = { "bps", "Kbps", "Mbps", "Gbps", "Tbps" };
            double len = bitsPerSec;
            int order = 0;
            while (len >= 1000 && order < sizes.Length - 1)
            {
                order++;
                len /= 1000;
            }
            return $"{len:0.##} {sizes[order]}";
        }

        public static string FormatSpeed(ulong bytesPerSec)
        {
            if (bytesPerSec == 0) return "N/A";
            string[] sizes = { "B/s", "KB/s", "MB/s", "GB/s" };
            double len = bytesPerSec;
            int order = 0;
            while (len >= 1024 && order < sizes.Length - 1)
            {
                order++;
                len /= 1024;
            }
            return order == 0 ? $"{len:0} {sizes[order]}" : $"{len:0.##} {sizes[order]}";
        }
    }

    public class SystemInfo
    {
        public string CpuName { get; set; } = string.Empty;
        public int PhysicalCores { get; set; }
        public int LogicalCores { get; set; }
        public double CpuUsage { get; set; }
        public int PerformanceCores { get; set; }
        public int EfficiencyCores { get; set; }
        public double PerformanceCoreFreq { get; set; }
        public double EfficiencyCoreFreq { get; set; }
        public bool HyperThreading { get; set; }
        public bool Virtualization { get; set; }
        
        public ulong TotalMemory { get; set; }
        public ulong UsedMemory { get; set; }
        public ulong AvailableMemory { get; set; }
        public ulong CompressedMemory { get; set; }
        
        public List<GpuData> Gpus { get; set; } = new();
        public string GpuName { get; set; } = string.Empty;
        public string GpuBrand { get; set; } = string.Empty;
        public ulong GpuMemory { get; set; }
        public double GpuCoreFreq { get; set; }
        public bool GpuIsVirtual { get; set; }
        
        public List<NetworkAdapterData> Adapters { get; set; } = new();
        public string NetworkAdapterName { get; set; } = string.Empty;
        public string NetworkAdapterMac { get; set; } = string.Empty;
        public string NetworkAdapterIp { get; set; } = string.Empty;
        public string NetworkAdapterType { get; set; } = string.Empty;
        public ulong NetworkAdapterSpeed { get; set; }
        
        public List<DiskData> Disks { get; set; } = new();
        public List<PhysicalDiskSmartData> PhysicalDisks { get; set; } = new();
        
        public List<TemperatureData> Temperatures { get; set; } = new();
        public TpmData? Tpm { get; set; }
        public string OsVersion { get; set; } = string.Empty;
        public int BatteryPercent { get; set; } = -1;
        public bool AcOnline { get; set; }
        public double CpuTemperature { get; set; }
        public double GpuTemperature { get; set; }
        public double CpuUsageSampleIntervalMs { get; set; }
        public DateTime LastUpdate { get; set; }

        // WiFi — populated by C++ side, not via IPC schema yet
        public bool HasWiFi { get; set; }
        public string WifiSSID { get; set; } = "";
        public int WifiRSSI { get; set; }
        public int WifiChannel { get; set; }
        public string WifiSecurity { get; set; } = "";
        public string WifiBand { get; set; } = "";
        public string WifiGen { get; set; } = "";

        // Bluetooth — populated by C++ side, not via IPC schema yet
        public bool HasBluetooth { get; set; }
        public bool BtPowerOn { get; set; }
        public int BtDeviceCount { get; set; }
    }

    public abstract class NotifyBase : INotifyPropertyChanged
    {
        public event PropertyChangedEventHandler? PropertyChanged;
        protected bool SetProperty<T>(ref T field, T value, [CallerMemberName] string? name = null)
        {
            if (EqualityComparer<T>.Default.Equals(field, value)) return false;
            field = value;
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
            return true;
        }
        protected void NotifyPropertyChanged(string name) =>
            PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }

    public class GpuData : NotifyBase
    {
        private string _name = string.Empty;
        private string _brand = string.Empty;
        private ulong _memory;
        private double _coreClock;
        private bool _isVirtual;
        private double _temperature;
        private double _usage;

        public string Name { get => _name; set => SetProperty(ref _name, value); }
        public string Brand { get => _brand; set => SetProperty(ref _brand, value); }
        public ulong Memory { get => _memory; set => SetProperty(ref _memory, value); }
        public double CoreClock { get => _coreClock; set => SetProperty(ref _coreClock, value); }
        public bool IsVirtual { get => _isVirtual; set => SetProperty(ref _isVirtual, value); }
        public double Temperature { get => _temperature; set => SetProperty(ref _temperature, value); }
        public double Usage { get => _usage; set => SetProperty(ref _usage, value); }
        public string DisplayName => string.IsNullOrEmpty(Name) ? "未知显卡" : (IsVirtual ? $"{Name} (虚拟)" : Name);
        public string MemoryDisplay => FormatUtil.FormatBytes(Memory);
        public string MemoryUsedDisplay => FormatUtil.FormatBytes((ulong)(Memory * CoreClock / 100.0));
        public override string ToString() => DisplayName;
    }

    public class NetworkAdapterData : NotifyBase
    {
        private string _name = string.Empty;
        private string _mac = string.Empty;
        private string _ipAddress = string.Empty;
        private string _adapterType = string.Empty;
        private ulong _speed;
        private ulong _downloadSpeed;
        private ulong _uploadSpeed;

        public string Name { get => _name; set => SetProperty(ref _name, value); }
        public string Mac { get => _mac; set => SetProperty(ref _mac, value); }
        public string IpAddress { get => _ipAddress; set => SetProperty(ref _ipAddress, value); }
        public string AdapterType { get => _adapterType; set => SetProperty(ref _adapterType, value); }
        public ulong Speed { get => _speed; set => SetProperty(ref _speed, value); }
        public ulong DownloadSpeed { get => _downloadSpeed; set { if (SetProperty(ref _downloadSpeed, value)) NotifyPropertyChanged(nameof(DownloadDisplay)); } }
        public ulong UploadSpeed { get => _uploadSpeed; set { if (SetProperty(ref _uploadSpeed, value)) NotifyPropertyChanged(nameof(UploadDisplay)); } }
        public string DisplayName => string.IsNullOrEmpty(Name) ? "未知网卡" : $"{Name} ({IpAddress})";
        public string SpeedDisplay => FormatUtil.FormatNetworkSpeed(Speed);
        public string DownloadDisplay => FormatUtil.FormatNetworkSpeed(DownloadSpeed);
        public string UploadDisplay => FormatUtil.FormatNetworkSpeed(UploadSpeed);
        public override string ToString() => DisplayName;
    }

    public class DiskData : NotifyBase
    {
        private char _letter;
        private string _label = string.Empty;
        private string _fileSystem = string.Empty;
        private ulong _totalSize;
        private ulong _usedSpace;
        private ulong _freeSpace;
        private int _physicalDiskIndex = -1;

        public char Letter { get => _letter; set => SetProperty(ref _letter, value); }
        public string Label { get => _label; set => SetProperty(ref _label, value); }
        public string FileSystem { get => _fileSystem; set => SetProperty(ref _fileSystem, value); }
        public ulong TotalSize { get => _totalSize; set => SetProperty(ref _totalSize, value); }
        public ulong UsedSpace { get => _usedSpace; set => SetProperty(ref _usedSpace, value); }
        public ulong FreeSpace { get => _freeSpace; set => SetProperty(ref _freeSpace, value); }
        public int PhysicalDiskIndex { get => _physicalDiskIndex; set => SetProperty(ref _physicalDiskIndex, value); }

        public double UsagePercent => TotalSize > 0 ? (double)UsedSpace / TotalSize * 100 : 0;
        public string DisplayName => string.IsNullOrEmpty(Label) ? "未知分区"
            : (Letter == '\0' ? Label : $"{Letter}: {Label}");
        public string TotalSizeDisplay => FormatUtil.FormatBytes(TotalSize);
        public string UsedSpaceDisplay => FormatUtil.FormatBytes(UsedSpace);
        public string FreeSpaceDisplay => FormatUtil.FormatBytes(FreeSpace);
        public string UsageDisplay => $"{UsagePercent:0.#}%";
        public override string ToString() => DisplayName;
    }

    public class SmartAttributeData
    {
        public byte Id { get; set; }
        public byte Current { get; set; }
        public byte Worst { get; set; }
        public byte Threshold { get; set; }
        public ulong RawValue { get; set; }
        public string Name { get; set; } = string.Empty;
        public string Description { get; set; } = string.Empty;
        public bool IsCritical { get; set; }
        public double PhysicalValue { get; set; }
        public string Units { get; set; } = string.Empty;
    }

    public class PhysicalDiskSmartData
    {
        public string Model { get; set; } = string.Empty;
        public string SerialNumber { get; set; } = string.Empty;
        public string FirmwareVersion { get; set; } = string.Empty;
        public string InterfaceType { get; set; } = string.Empty;
        public string DiskType { get; set; } = string.Empty;
        public ulong Capacity { get; set; }
        public double Temperature { get; set; }
        public byte HealthPercentage { get; set; }
        public bool IsSystemDisk { get; set; }
        public bool SmartEnabled { get; set; }
        public bool SmartSupported { get; set; }
        public ulong PowerOnHours { get; set; }
        public ulong PowerCycleCount { get; set; }
        public ulong ReallocatedSectorCount { get; set; }
        public ulong CurrentPendingSector { get; set; }
        public ulong UncorrectableErrors { get; set; }
        public double WearLeveling { get; set; }
        public ulong TotalBytesWritten { get; set; }
        public ulong TotalBytesRead { get; set; }
        public List<char> LogicalDriveLetters { get; set; } = new();
        public List<string> PartitionLabels { get; set; } = new();
        public List<SmartAttributeData> Attributes { get; set; } = new();
    }

    public class TemperatureData
    {
        public string SensorName { get; set; } = string.Empty;
        public double Temperature { get; set; }
        public string DisplayName => string.IsNullOrEmpty(SensorName) ? "未知传感器" : SensorName;
        public string TemperatureDisplay => $"{Temperature:0.#}°C";
        public override string ToString() => $"{DisplayName}: {TemperatureDisplay}";
    }

    public class PhysicalDiskView : NotifyBase
    {
        private PhysicalDiskSmartData? _disk;
        public PhysicalDiskSmartData? Disk
        {
            get => _disk!;
            set
            {
                if (SetProperty(ref _disk, value))
                {
                    NotifyPropertyChanged(nameof(DisplayName));
                    NotifyPropertyChanged(nameof(LettersDisplay));
                }
            }
        }
        public ObservableCollection<DiskData> Partitions { get; } = new();
        public string LettersDisplay {
        get {
            if (Partitions.Count > 0)
                return "卷: " + string.Join(", ", Partitions.Select(p => p.Label));
            if (Disk?.LogicalDriveLetters != null && Disk.LogicalDriveLetters.Count > 0)
                return string.Join(", ", Disk.LogicalDriveLetters.Select(l => l + ":"));
            return "";
        }
    }
        public string DisplayName => Disk == null ? "未知磁盘" : $"{Disk.Model} ({LettersDisplay})";
    }

    public class TpmData : NotifyBase
    {
        private string _manufacturer = string.Empty;
        private string _firmwareVersion = string.Empty;
        private string _status = string.Empty;
        private string _selfTestStatus = string.Empty;
        private bool _isEnabled;
        private bool _isActive;

        public string Manufacturer { get => _manufacturer; set => SetProperty(ref _manufacturer, value); }
        public string FirmwareVersion { get => _firmwareVersion; set => SetProperty(ref _firmwareVersion, value); }
        public string Status { get => _status; set => SetProperty(ref _status, value); }
        public string SelfTestStatus { get => _selfTestStatus; set => SetProperty(ref _selfTestStatus, value); }
        public bool IsEnabled { get => _isEnabled; set => SetProperty(ref _isEnabled, value); }
        public bool IsActive { get => _isActive; set => SetProperty(ref _isActive, value); }
        public string DisplayName => string.IsNullOrEmpty(Manufacturer) ? "未检测到 TPM" : $"{Manufacturer} TPM";
    }
}