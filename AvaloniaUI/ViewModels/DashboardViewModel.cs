using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

/// <summary>
/// 概览页 —— 设计稿的面板集：CPU / 内存 / 电源 / 温度 / 磁盘 / 网络 / Top Processes。
/// 缺能力的位置保留并报 N/A 加一行弱化说明：面板不消失，数字不编造（TUI #10）。
/// </summary>
public partial class DashboardViewModel : ViewModelBase
{
    private const int MaxSensorRows = 6;
    private const int MaxTopProcessRows = 5;

    private readonly HistoryBuffer _downHistory = new(AppConstants.MaxChartPoints);
    private readonly HistoryBuffer _upHistory = new(AppConstants.MaxChartPoints);

    [ObservableProperty]
    private double _cpuUsage;

    [ObservableProperty]
    private string _cpuUsageDisplay = "—";

    /// <summary>False until the collector has delivered a sample. IPCSystemInfoMapper stamps
    /// LastUpdate on every real sample, so a default value means nothing has arrived yet —
    /// and a KPI showing 0 is a measurement, which the absence of one is not.</summary>
    [ObservableProperty]
    private bool _hasSample;

    [ObservableProperty]
    private string _cpuNote = "// —";

    [ObservableProperty]
    private string _loadDisplay = "—";

    [ObservableProperty]
    private double _memoryPercent;

    [ObservableProperty]
    private string _memoryPercentDisplay = "—";

    [ObservableProperty]
    private string _memoryUsageDisplay = "—";

    [ObservableProperty]
    private string _memoryDetailDisplay = "—";

    [ObservableProperty]
    private string _swapDisplay = "—";

    [ObservableProperty]
    private bool _hasBattery;

    [ObservableProperty]
    private int _batteryPercent;

    [ObservableProperty]
    private string _batteryHealthDisplay = "—";

    [ObservableProperty]
    private string _powerStateDisplay = "—";

    [ObservableProperty]
    private string _batteryNote = string.Empty;

    [ObservableProperty]
    private string _fanDisplay = "—";

    [ObservableProperty]
    private string _sensorOverflowNote = string.Empty;

    [ObservableProperty]
    private string _diskNameDisplay = "—";

    [ObservableProperty]
    private double _diskUsagePercent;

    [ObservableProperty]
    private string _diskUsageDisplay = "—";

    [ObservableProperty]
    private bool _hasSmart;

    [ObservableProperty]
    private string _smartHealthDisplay = "—";

    [ObservableProperty]
    private string _smartTempDisplay = "—";

    [ObservableProperty]
    private string _smartHoursDisplay = "—";

    [ObservableProperty]
    private string _smartNote = "该主机未报告 SMART 数据";

    [ObservableProperty]
    private string _networkNameDisplay = "—";

    [ObservableProperty]
    private string _networkIpDisplay = "—";

    [ObservableProperty]
    private string _downloadDisplay = "—";

    [ObservableProperty]
    private string _uploadDisplay = "—";

    [ObservableProperty]
    private string _trafficPeakDisplay = "—";

    [ObservableProperty]
    private string _downSparkPoints = string.Empty;

    [ObservableProperty]
    private string _upSparkPoints = string.Empty;

    public ObservableCollection<CoreBar> CoreBars { get; } = new();

    public ObservableCollection<SensorRow> Sensors { get; } = new();

    public ObservableCollection<ProcessRow> TopProcesses { get; } = new();

    public bool HasCoreBars => CoreBars.Count > 0;

    public bool HasSensors => Sensors.Count > 0;

    public bool HasSensorOverflow => SensorOverflowNote.Length > 0;

    public bool HasTopProcesses => TopProcesses.Count > 0;

    public bool HasNoTopProcesses => TopProcesses.Count == 0;

    public bool HasNoBattery => !HasBattery;

    public bool HasNoSensors => !HasSensors;

    public bool HasNoSmart => !HasSmart;

    /// <summary>False until the ring buffer holds two points. A line needs two, and the empty
    /// string Sparkline returns for fewer converts to a null Points list — which Polyline throws
    /// on while measuring. The view keeps the lines out of layout instead.</summary>
    public bool HasTrafficSpark => DownSparkPoints.Length > 0;

    public DashboardViewModel()
    {
        Title = "概览";
        Icon = "dashboard";
    }

    partial void OnHasBatteryChanged(bool value) => OnPropertyChanged(nameof(HasNoBattery));

    partial void OnHasSmartChanged(bool value) => OnPropertyChanged(nameof(HasNoSmart));

    public override void Update(SystemInfo info)
    {
        // Every real sample carries a timestamp; while it is still default nothing has arrived.
        HasSample = info.LastUpdate != default;

        UpdateCpu(info);
        UpdateMemory(info);
        UpdatePower(info);
        UpdateThermal(info);
        UpdateDisks(info);
        UpdateNetwork(info);
        UpdateTopProcesses(info);
    }

    private void UpdateCpu(SystemInfo info)
    {
        CpuUsage = FormatUtils.ValidateDouble(info.CpuUsage);
        CpuUsageDisplay = HasSample ? $"{CpuUsage:F1}" : "—";

        var cores = info.LogicalCores > 0 ? info.LogicalCores : info.PhysicalCores;
        CpuNote = cores > 0 ? $"// {cores} 核" : "// —";

        LoadDisplay = info.LoadAvg1 > 0
            ? $"{info.LoadAvg1:F2} / {info.LoadAvg5:F2} / {info.LoadAvg15:F2}"
            : "—";

        CoreBars.Clear();
        foreach (var core in (info.PerCores ?? new List<PerCoreData>()).Where(c => c.Temperature > 0))
        {
            CoreBars.Add(new CoreBar(core.Index, core.Temperature));
        }

        OnPropertyChanged(nameof(HasCoreBars));
    }

    private void UpdateMemory(SystemInfo info)
    {
        var hasMemory = HasSample && info.TotalMemory > 0;

        MemoryPercent = info.TotalMemory > 0 ? (double)info.UsedMemory / info.TotalMemory * 100 : 0;
        MemoryPercentDisplay = hasMemory ? $"{MemoryPercent:F0}" : "—";
        MemoryUsageDisplay = hasMemory
            ? $"{FormatUtils.FormatBytes(info.UsedMemory)} / {FormatUtils.FormatBytes(info.TotalMemory)}"
            : "—";
        MemoryDetailDisplay = hasMemory
            ? $"已压缩 {FormatUtils.FormatBytes(info.CompressedMemory)} · 可用 {FormatUtils.FormatBytes(info.AvailableMemory)}"
            : "—";
        SwapDisplay = info.SwapTotal > 0
            ? $"{FormatUtils.FormatBytes(info.SwapUsed)} / {FormatUtils.FormatBytes(info.SwapTotal)}"
            : "—";
    }

    private void UpdatePower(SystemInfo info)
    {
        HasBattery = info.BatteryIsPresent;
        BatteryNote = HasBattery ? string.Empty : "该主机未报告电池设备";
        BatteryPercent = Math.Max(0, info.BatteryPercent);
        BatteryHealthDisplay = info.BatteryHealthPercent > 0 ? $"{info.BatteryHealthPercent:F0}%" : "—";

        PowerStateDisplay = info.BatteryIsCharging
            ? "充电中"
            : info.AcOnline ? "交流供电" : (info.BatteryIsPresent ? "电池供电" : "—");
    }

    private void UpdateThermal(SystemInfo info)
    {
        var sensors = (info.Temperatures ?? new List<TemperatureData>()).Where(t => t.Temperature > 0).ToList();

        Sensors.Clear();
        foreach (var sensor in sensors.Take(MaxSensorRows))
        {
            Sensors.Add(new SensorRow(sensor.DisplayName, sensor.Temperature));
        }

        // Never a silent cap: what did not fit is stated, not dropped.
        SensorOverflowNote = sensors.Count > MaxSensorRows
            ? $"+{sensors.Count - MaxSensorRows} 个传感器未列出"
            : string.Empty;

        var fans = (info.Fans ?? new List<FanData>()).Where(f => f.Rpm > 0).ToList();
        FanDisplay = fans.Count > 0
            ? $"最高 {fans.Max(f => f.Rpm):F0} RPM · {fans.Count} 个"
            : "该主机未暴露风扇转速";

        OnPropertyChanged(nameof(HasSensors));
        OnPropertyChanged(nameof(HasNoSensors));
        OnPropertyChanged(nameof(HasSensorOverflow));
    }

    private void UpdateDisks(SystemInfo info)
    {
        var disk = (info.Disks ?? new List<DiskData>()).FirstOrDefault();

        if (disk is null)
        {
            DiskNameDisplay = "—";
            DiskUsageDisplay = "该主机未报告分区";
            DiskUsagePercent = 0;
        }
        else
        {
            DiskNameDisplay = disk.DisplayName;
            DiskUsagePercent = FormatUtils.ValidateDouble(disk.UsagePercent);
            DiskUsageDisplay = $"{disk.UsedSpaceDisplay} / {disk.TotalSizeDisplay}";
        }

        var smart = (info.PhysicalDisks ?? new List<PhysicalDiskSmartData>()).FirstOrDefault();

        HasSmart = smart is not null;
        SmartNote = smart is null ? "该主机未报告 SMART 数据" : string.Empty;
        SmartHealthDisplay = smart is not null && smart.HealthPercentage > 0 ? $"{smart.HealthPercentage}%" : "—";
        SmartTempDisplay = smart is not null && smart.Temperature > 0 ? $"{smart.Temperature:F0} °C" : "—";
        SmartHoursDisplay = smart is not null && smart.PowerOnHours > 0 ? $"{smart.PowerOnHours:N0} h" : "—";
    }

    private void UpdateNetwork(SystemInfo info)
    {
        var adapter = (info.Adapters ?? new List<NetworkAdapterData>()).FirstOrDefault();

        NetworkNameDisplay = adapter?.Name ?? "—";
        NetworkIpDisplay = string.IsNullOrWhiteSpace(adapter?.IpAddress) ? "—" : adapter!.IpAddress;
        DownloadDisplay = adapter?.DownloadDisplay ?? "—";
        UploadDisplay = adapter?.UploadDisplay ?? "—";

        // Two series, one shared scale — down and up can only be compared on the same peak.
        _downHistory.Push(adapter?.DownloadSpeed ?? 0);
        _upHistory.Push(adapter?.UploadSpeed ?? 0);

        DownSparkPoints = Sparkline.Points(_downHistory);
        UpSparkPoints = Sparkline.Points(_upHistory);
        OnPropertyChanged(nameof(HasTrafficSpark));
        // Both buffers are pushed together, so one emptiness check covers the pair.
        // An empty window is not a reading of zero — say so rather than printing "0 B/s".
        TrafficPeakDisplay = _downHistory.IsEmpty
            ? "—"
            : $"↓ {FormatUtils.FormatNetworkSpeed((ulong)_downHistory.Max())} · ↑ {FormatUtils.FormatNetworkSpeed((ulong)_upHistory.Max())}";
    }

    private void UpdateTopProcesses(SystemInfo info)
    {
        TopProcesses.Clear();
        foreach (var entry in (info.TopProcesses ?? new List<ProcessTopEntry>())
                     .OrderByDescending(e => e.CpuPercent)
                     .Take(MaxTopProcessRows))
        {
            TopProcesses.Add(new ProcessRow(entry));
        }

        OnPropertyChanged(nameof(HasTopProcesses));
        OnPropertyChanged(nameof(HasNoTopProcesses));
    }
}
