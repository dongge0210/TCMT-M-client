using System;
using System.Collections.ObjectModel;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Serilog;
using AvaloniaUI.Models;
using AvaloniaUI.Services.IPC;

namespace AvaloniaUI.ViewModels;

public partial class MainWindowViewModel : ObservableObject, IDisposable
{
    public bool IsMacOS => OperatingSystem.IsMacOS();
    public bool IsNotMacOS => !OperatingSystem.IsMacOS();
    private IPCService? _ipcService;
    private readonly DispatcherTimer _timer;
    private bool _disposed = false;
    private const int MaxChartPoints = 60;
    private int _consecutiveErrors = 0;
    private const int MaxConsecutiveErrors = 5;

    // Track previous selections
    private string? _previousNetworkKey;
    private string? _previousDiskKey;

    public MainWindowViewModel()
    {
        // Initialize placeholders BEFORE IPC starts — DataReady may fire synchronously
        SelectedGpu = new GpuData { Name = "等待数据..." };
        SelectedNetwork = new NetworkAdapterData { Name = "等待数据..." };
        SelectedDisk = new DiskData { Label = "等待数据..." };

        try
        {
            _ipcService = new IPCService();
            _ipcService.DataReady += () =>
            {
                try
                {
                    if (_ipcService != null && _ipcService.IsMemoryOpen)
                    {
                        var info = IPCSystemInfoMapper.Read(_ipcService);
                        if (info != null)
                        {
                            _consecutiveErrors = 0;
                            IsConnected = true;
                            ConnectionStatus = "已连接 (IPC)";
                            WindowTitle = "系统硬件监视器";
                            UpdateSystemData(info);
                            LastUpdate = DateTime.Now;
                        }
                    }
                }
                catch (Exception ex)
                {
                    Log.Error(ex, "IPC DataReady handler crashed");
                }
            };
            _ipcService.ConnectionStateChanged += (connected, msg) =>
            {
                if (connected)
                    Log.Information("IPC connected: {Msg}", msg);
                else
                {
                    Log.Warning("IPC disconnected: {Msg}", msg);
                    ConnectionStatus = "已断开 (" + msg + ")";
                    IsConnected = false;
                }
            };
            _ipcService.Start();
        }
        catch (Exception ex)
        {
            Log.Error(ex, "IPC service init failed");
            IsConnected = false;
            ConnectionStatus = $"IPC 初始化失败: {ex.Message}";
        }
        _timer = new DispatcherTimer
        {
            Interval = TimeSpan.FromMilliseconds(500)
        };
        _timer.Tick += UpdateTimer_Tick;
    }

    public void Start()
    {
        try
        {
            ConnectionStatus = "连接中...";
            WindowTitle = "系统硬件监视器";
            _timer.Start();
            Log.Information("Started monitoring via IPC");
        }
        catch (Exception ex)
        {
            Log.Error(ex, "Failed to start monitoring");
            IsConnected = false;
            ConnectionStatus = $"错误: {ex.Message}";
        }
    }

    private void ShowDisconnectedState()
    {
        CpuName = "未连接";
        TotalMemory = "未检测到";
        UsedMemory = "未检测到";
        CompressedMemory = "";
        GpuList.Clear();
        NetworkList.Clear();
        DiskList.Clear();
        PhysicalDiskList.Clear();
        SelectedGpu = new GpuData { Name = "未连接" };
        SelectedNetwork = new NetworkAdapterData { Name = "未连接" };
        SelectedDisk = new DiskData { Label = "未连接" };
    }

    [RelayCommand]
    private void Reconnect()
    {
        _consecutiveErrors = 0;
        ConnectionStatus = "重新连接中...";
        _ipcService?.Dispose();
        try
        {
            _ipcService = new IPCService();
            _ipcService.Start();
        }
        catch (Exception ex)
        {
            Log.Error(ex, "Reconnect failed");
            IsConnected = false;
            ConnectionStatus = "重连失败";
        }
    }

    [RelayCommand]
    private void ShowWindow()
    {
        if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.MainWindow?.Show();
            desktop.MainWindow?.Activate();
        }
    }

    [RelayCommand]
    private void Quit()
    {
        Dispose();
        if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.Shutdown();
        }
    }

    private void UpdateTimer_Tick(object? sender, EventArgs e)
    {
        try
        {
            if (_ipcService != null && _ipcService.IsMemoryOpen)
            {
                var info = IPCSystemInfoMapper.Read(_ipcService);
                if (info != null)
                {
                    _consecutiveErrors = 0;
                    if (!IsConnected)
                    {
                        IsConnected = true;
                        ConnectionStatus = "已连接 (IPC)";
                        WindowTitle = "系统硬件监视器";
                    }

                    // Reset placeholders to null so auto‑selection logic in UpdateSystemData works
                    if (SelectedGpu?.Name == "等待数据..." || SelectedGpu?.Name == "未连接")
                        SelectedGpu = null;
                    if (SelectedNetwork?.Name == "等待数据..." || SelectedNetwork?.Name == "未连接")
                        SelectedNetwork = null;
                    if (SelectedDisk?.Label == "等待数据..." || SelectedDisk?.Label == "未连接")
                        SelectedDisk = null;

                    UpdateSystemData(info);
                    LastUpdate = DateTime.Now;
                    return;
                }
            }

            _consecutiveErrors++;
            if (_consecutiveErrors >= MaxConsecutiveErrors)
            {
                if (IsConnected)
                {
                    IsConnected = false;
                    ConnectionStatus = "连接已断开";
                    WindowTitle = "系统硬件监视器 - 已断开";
                    ShowDisconnectedState();
                }
            }
        }
        catch (Exception ex)
        {
            Log.Error(ex, "Error updating data");
            _consecutiveErrors++;
            if (_consecutiveErrors >= MaxConsecutiveErrors)
            {
                IsConnected = false;
                ConnectionStatus = $"错误: {ex.Message}";
            }
        }
    }

    private void UpdateSystemData(SystemInfo info)
    {
        // CPU
        CpuName = string.IsNullOrWhiteSpace(info.CpuName) ? "未知CPU" : info.CpuName;
        PhysicalCores = info.PhysicalCores;
        LogicalCores = info.LogicalCores;
        PerformanceCores = info.PerformanceCores;
        EfficiencyCores = info.EfficiencyCores;
        CpuUsage = ValidateDouble(info.CpuUsage);
        HyperThreading = info.HyperThreading;
        Virtualization = info.Virtualization;
        CpuTemperature = ValidateDouble(info.CpuTemperature);
        CpuFrequency = FormatFrequency(info.PerformanceCoreFreq);
        CpuEfficiencyFrequency = FormatFrequency(info.EfficiencyCoreFreq);
        CpuBaseFreq = info.CpuBaseFreq;

        // Memory
        TotalMemory = info.TotalMemory > 0 ? FormatBytes(info.TotalMemory) : "未检测到";
        UsedMemory = info.UsedMemory > 0 ? FormatBytes(info.UsedMemory) : "未检测到";
        AvailableMemory = info.AvailableMemory > 0 ? FormatBytes(info.AvailableMemory) : "未检测到";
        CompressedMemory = info.CompressedMemory > 0 ? FormatBytes(info.CompressedMemory) : "";
        MemoryPercent = info.TotalMemory > 0 ? (double)info.UsedMemory / info.TotalMemory * 100 : 0;
        AddMemoryHistoryPoint(MemoryPercent);

        // GPU
        UpdateCollection(GpuList, info.Gpus);
        if (info.Gpus.Count > 0)
        {
            if (SelectedGpu == null)
                SelectedGpu = info.Gpus[0];
            else
            {
                var restored = info.Gpus.FirstOrDefault(g => g.Name == SelectedGpu.Name);
                if (restored != null) SelectedGpu = restored;
            }
        }
        if (SelectedGpu == null && GpuList.Count == 0)
        {
            GpuList.Add(new GpuData { Name = "等待数据..." });
            SelectedGpu = GpuList.First();
        }
        else if (SelectedGpu == null && GpuList.Count > 0)
            SelectedGpu = GpuList.First();
        GpuTemperature = ValidateDouble(info.GpuTemperature);

        // Network - preserve selection
        if (SelectedNetwork != null)
            _previousNetworkKey = $"{SelectedNetwork.Name}|{SelectedNetwork.Mac}";
        UpdateCollection(NetworkList, info.Adapters);
        if (_previousNetworkKey != null)
        {
            var restored = NetworkList.FirstOrDefault(a => $"{a.Name}|{a.Mac}" == _previousNetworkKey);
            if (restored != null) SelectedNetwork = restored;
        }
        // Auto-select an active adapter (has IP and speed) when nothing is selected
        if (SelectedNetwork == null && NetworkList.Count > 0)
        {
            SelectedNetwork = NetworkList.FirstOrDefault(a =>
                !string.IsNullOrEmpty(a.IpAddress) && a.IpAddress != "127.0.0.1" && a.IpAddress != "::1" && a.Speed > 0)
                ?? NetworkList.FirstOrDefault(a => !string.IsNullOrEmpty(a.IpAddress))
                ?? NetworkList[0];
        }
        if (SelectedNetwork == null && NetworkList.Count == 0)
        {
            NetworkList.Add(new NetworkAdapterData { Name = "等待数据..." });
            SelectedNetwork = NetworkList.First();
        }
        else if (SelectedNetwork == null && NetworkList.Count > 0)
            SelectedNetwork = NetworkList.First();

        // Physical Disks with SMART
        BuildOrUpdatePhysicalDisks(info);

        // Disk - preserve selection
        if (SelectedDisk != null)
            _previousDiskKey = $"{SelectedDisk.Letter}:{SelectedDisk.Label}";
        UpdateCollection(DiskList, info.Disks);
        if (_previousDiskKey != null)
        {
            var restored = DiskList.FirstOrDefault(d => $"{d.Letter}:{d.Label}" == _previousDiskKey);
            if (restored != null) SelectedDisk = restored;
        }
        if (SelectedDisk == null && DiskList.Count > 0)
            SelectedDisk = DiskList[0];
        if (SelectedDisk == null && DiskList.Count == 0)
        {
            DiskList.Add(new DiskData { Letter = '?' });
            SelectedDisk = DiskList.First();
        }
        else if (SelectedDisk == null && DiskList.Count > 0)
            SelectedDisk = DiskList.First();

        // Temperature charts
        UpdateTemperatureCharts(info.CpuTemperature, info.GpuTemperature);

        // TPM - 确保不为 null
        TpmInfo = info.Tpm ?? new TpmData { Manufacturer = "", Status = "未检测到" };
        OsVersion = string.IsNullOrWhiteSpace(info.OsVersion) ? "未知" : info.OsVersion;
        BatteryPercent = info.BatteryPercent;
        AcOnline = info.AcOnline;
        OnPropertyChanged(nameof(HasBattery));
        OnPropertyChanged(nameof(BatteryLabel));

        // WiFi
        WifiSSID = info.WifiSSID ?? "";
        WifiRSSI = info.WifiRSSI;
        WifiChannel = info.WifiChannel;
        WifiSecurity = info.WifiSecurity ?? "";
        WifiBand = info.WifiBand ?? "";
        WifiGen = info.WifiGen ?? "";
        HasWifiHardware = info.HasWiFi; // C++ wifi/powerOn via IPC
        OnPropertyChanged(nameof(HasWiFi));
        OnPropertyChanged(nameof(WifiDisplay));

        // Bluetooth
        HasBluetooth = info.HasBluetooth;
        BtPowerOn = info.BtPowerOn;
        BtDeviceCount = info.BtDeviceCount;
        OnPropertyChanged(nameof(BtDisplay));

        Log.Debug("TPM 更新: {Manuf}, {Status}", TpmInfo.Manufacturer, TpmInfo.Status);
    }

    private void BuildOrUpdatePhysicalDisks(SystemInfo info)
    {
        try
        {
            var map = PhysicalDiskList.ToDictionary(p => p.Disk?.SerialNumber ?? "", p => p);
            var alive = new HashSet<string>();

            foreach (var pd in info.PhysicalDisks)
            {
                if (!map.TryGetValue(pd.SerialNumber, out var view))
                {
                    view = new PhysicalDiskView { Disk = pd };
                    PhysicalDiskList.Add(view);
                }
                else
                {
                    view.Disk = pd;
                }
                alive.Add(pd.SerialNumber);
            }

            for (int i = PhysicalDiskList.Count - 1; i >= 0; i--)
            {
                if (!alive.Contains(PhysicalDiskList[i].Disk?.SerialNumber ?? ""))
                    PhysicalDiskList.RemoveAt(i);
            }
            // Map logical volumes to physical disks by drive letter (Windows)
            // On Windows each physical disk has its own partitions; on macOS APFS
            // containers share one device, so fall back to first-disk mapping.
            foreach (var phys in PhysicalDiskList)
                phys.Partitions.Clear();

            if (OperatingSystem.IsMacOS())
            {
                // macOS APFS container: all volumes share the first physical disk
                if (PhysicalDiskList.Count > 0)
                {
                    var firstPhys = PhysicalDiskList[0];
                    foreach (var d in DiskList)
                        firstPhys.Partitions.Add(d);
                }
            }
            else
            {
                // Windows: match logical disks to physical disks by drive letter
                var dmap = DiskList.Where(d => d.Letter != ' ')
                                   .ToDictionary(d => d.Letter, d => d);
                foreach (var phys in PhysicalDiskList)
                {
                    if (phys.Disk?.LogicalDriveLetters == null) continue;
                    for (int i = 0; i < phys.Disk.LogicalDriveLetters.Count; i++)
                    {
                        var letter = phys.Disk.LogicalDriveLetters[i];
                        if (dmap.TryGetValue(letter, out var disk))
                            phys.Partitions.Add(disk);
                    }
                }
                // Also attach any disks without letter (network/removable) to first phys
                var unassigned = DiskList.Where(d => d.Letter == ' ').ToList();
                if (unassigned.Count > 0 && PhysicalDiskList.Count > 0)
                {
                    var firstPhys = PhysicalDiskList[0];
                    foreach (var d in unassigned)
                        firstPhys.Partitions.Add(d);
                }
            }

            if (PhysicalDiskList.Count > 0 && SelectedPhysicalDisk == null)
                SelectedPhysicalDisk = PhysicalDiskList[0];
            OnPropertyChanged(nameof(SelectedPhysicalDisk)); // refresh LettersDisplay
        }
        catch (Exception ex)
        {
            Log.Error(ex, "Failed to build physical disks");
        }
    }

    private void UpdateCollection<T>(ObservableCollection<T> collection, List<T> newItems)
    {
        try
        {
            if (newItems == null)
            {
                if (collection.Count > 0) collection.Clear();
                return;
            }

            collection.Clear();
            foreach (var item in newItems)
            {
                collection.Add(item);
            }
        }
        catch (Exception ex)
        {
            Log.Error(ex, "Failed to update collection");
        }
    }

    private void UpdateTemperatureCharts(double cpuTemp, double gpuTemp)
    {
        try
        {
            if (cpuTemp > 0 && cpuTemp < 150)
            {
                CpuTempData.Add(cpuTemp);
            }
            else if (CpuTempData.Count == 0)
            {
                CpuTempData.Add(0);
            }

            if (gpuTemp > 0 && gpuTemp < 150)
            {
                GpuTempData.Add(gpuTemp);
            }
            else if (GpuTempData.Count == 0)
            {
                GpuTempData.Add(0);
            }

            while (CpuTempData.Count > MaxChartPoints)
                CpuTempData.RemoveAt(0);
            while (GpuTempData.Count > MaxChartPoints)
                GpuTempData.RemoveAt(0);
        }
        catch (Exception ex)
        {
            Log.Error(ex, "Failed to update temperature charts");
        }
    }

    private double ValidateDouble(double value)
    {
        if (double.IsNaN(value) || double.IsInfinity(value) || value < 0) return 0;
        return value;
    }

    private string FormatBytes(ulong bytes)
    {
        if (bytes == 0) return "0 B";
        const ulong KB = 1024UL;
        const ulong MB = KB * KB;
        const ulong GB = MB * KB;
        const ulong TB = GB * KB;

        return bytes switch
        {
            >= TB => $"{(double)bytes / TB:F1} TB",
            >= GB => $"{(double)bytes / GB:F1} GB",
            >= MB => $"{(double)bytes / MB:F1} MB",
            >= KB => $"{(double)bytes / KB:F1} KB",
            _ => $"{bytes} B"
        };
    }

    private string FormatFrequency(double frequency)
    {
        if (frequency <= 0) return "N/A";
        return $"{frequency:F0} MHz";
    }

    private string FormatNetworkSpeed(ulong speedBps)
    {
        if (speedBps == 0) return "N/A";
        const ulong Kbps = 1000UL;
        const ulong Mbps = Kbps * Kbps;
        const ulong Gbps = Mbps * Kbps;

        return speedBps switch
        {
            >= Gbps => $"{(double)speedBps / Gbps:F1} Gbps",
            >= Mbps => $"{(double)speedBps / Mbps:F1} Mbps",
            >= Kbps => $"{(double)speedBps / Kbps:F1} Kbps",
            _ => $"{speedBps} bps"
        };
    }

    public void AddMemoryHistoryPoint(double value)
    {
        MemoryHistory.Add(value);
        while (MemoryHistory.Count > MaxChartPoints)
        {
            MemoryHistory.RemoveAt(0);
        }
        OnPropertyChanged(nameof(MemoryHistory));
    }

    #region Properties

    [ObservableProperty]
    private string _windowTitle = "系统硬件监视器";

    [ObservableProperty]
    private bool _isConnected;

    [ObservableProperty]
    private string _connectionStatus = "连接中...";

    // CPU
    [ObservableProperty]
    private string _cpuName = "检测中...";

    [ObservableProperty]
    private int _physicalCores;

    [ObservableProperty]
    private int _logicalCores;

    [ObservableProperty]
    private int _performanceCores;

    [ObservableProperty]
    private int _efficiencyCores;

    [ObservableProperty]
    private double _cpuUsage;

    [ObservableProperty]
    private bool _hyperThreading;

    [ObservableProperty]
    private bool _virtualization;

    [ObservableProperty]
    private double _cpuTemperature;

    [ObservableProperty]
    private string _cpuFrequency = "N/A";

    [ObservableProperty]
    private string _cpuEfficiencyFrequency = "N/A";

    [ObservableProperty]
    private double _cpuBaseFreq;

    // Memory
    [ObservableProperty]
    private string _totalMemory = "检测中...";

    [ObservableProperty]
    private string _usedMemory = "检测中...";

    [ObservableProperty]
    private string _availableMemory = "检测中...";

    [ObservableProperty]
    private string _compressedMemory = "";

    [ObservableProperty]
    private double _memoryUsed;

    [ObservableProperty]
    private double _memoryTotal;

    [ObservableProperty]
    private double _memoryPercent;

    // GPU
    [ObservableProperty]
    private ObservableCollection<GpuData> _gpuList = new();

    [ObservableProperty]
    private GpuData? _selectedGpu;

    [ObservableProperty]
    private double _gpuTemperature;

    // Network
    [ObservableProperty]
    private ObservableCollection<NetworkAdapterData> _networkList = new();

    [ObservableProperty]
    private NetworkAdapterData? _selectedNetwork;

    // Disk
    [ObservableProperty]
    private ObservableCollection<DiskData> _diskList = new();

    [ObservableProperty]
    private DiskData? _selectedDisk;

    // Physical Disks with SMART
    [ObservableProperty]
    private ObservableCollection<PhysicalDiskView> _physicalDiskList = new();

    [ObservableProperty]
    private PhysicalDiskView? _selectedPhysicalDisk;

    [ObservableProperty]
    private bool _smartDetailVisible;

    [RelayCommand]
    private void ToggleSmartDetail()
    {
        SmartDetailVisible = !SmartDetailVisible;
    }

    // TPM
    [ObservableProperty]
    private TpmData? _tpmInfo;

    // OS Version
    [ObservableProperty]
    private string _osVersion = string.Empty;

    // Battery
    [ObservableProperty]
    private int _batteryPercent = -1;

    [ObservableProperty]
    private bool _acOnline;

    public bool HasBattery => BatteryPercent >= 0;
    public string BatteryLabel => AcOnline ? "AC" : "BAT";

    // WiFi hardware presence from C++ IPC (wifi/powerOn)
    [ObservableProperty]
    private bool _hasWifiHardware;

    // WiFi raw data from C++ IPC
    [ObservableProperty]
    private string _wifiSSID = "";

    [ObservableProperty]
    private int _wifiRSSI;

    [ObservableProperty]
    private int _wifiChannel;

    [ObservableProperty]
    private string _wifiSecurity = "";

    [ObservableProperty]
    private string _wifiBand = "";

    [ObservableProperty]
    private string _wifiGen = "";

    // Bluetooth raw data from C++ IPC
    [ObservableProperty]
    private bool _hasBluetooth;

    [ObservableProperty]
    private bool _btPowerOn;

    [ObservableProperty]
    private int _btDeviceCount;

    // WiFi — show when a wireless adapter exists (follow adapter hardware state)
    public bool HasWirelessAdapter => NetworkList.Any(a =>
        !string.IsNullOrEmpty(a.AdapterType) &&
        (a.AdapterType.Contains("Wireless", StringComparison.OrdinalIgnoreCase) ||
         a.AdapterType.Contains("无线")));
    // WiFi hardware presence: prefers C++ wifi/powerOn via IPC.
    // Falls back to wireless adapter check if IPC data not available.
    public bool HasWiFi => HasWifiHardware || HasWirelessAdapter;
    public string WifiDisplay
    {
        get
        {
            bool hasHw = HasWiFi;
            if (!hasHw) return "WiFi: No adapter";
            if (!HasWifiHardware && HasWirelessAdapter)
                return "WiFi: OFF";  // wireless adapter present but C++ module reports radio off
            var extra = "";
            if (!string.IsNullOrEmpty(WifiBand)) extra += "  " + WifiBand;
            if (!string.IsNullOrEmpty(WifiGen)) extra += "  " + WifiGen;
            if (!string.IsNullOrEmpty(WifiSSID))
                return $"WiFi: {WifiSSID}  Ch:{WifiChannel}  RSSI:{WifiRSSI} dBm  {WifiSecurity}{extra}";
            if (WifiRSSI != 0 || WifiChannel != 0)
                return $"WiFi: On  Ch:{WifiChannel}  RSSI:{WifiRSSI} dBm  {WifiSecurity}{extra}";
            return "WiFi: Disconnected";
        }
    }
    public string BtDisplay => HasBluetooth ? $"BT: {(BtPowerOn ? "On" : "Off")} ({BtDeviceCount} devices)" : "";

    // Last update
    [ObservableProperty]
    private DateTime _lastUpdate = DateTime.Now;

    // Chart data
    [ObservableProperty]
    private ObservableCollection<double> _memoryHistory = new();

    #endregion

    #region Temperature Chart Data
    public ObservableCollection<double> CpuTempData { get; } = new();
    public ObservableCollection<double> GpuTempData { get; } = new();
    #endregion

    public void Dispose()
    {
        if (_disposed) return;
        _timer.Stop();
        _ipcService?.Dispose();
        _disposed = true;
        GC.SuppressFinalize(this);
    }

}