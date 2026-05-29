using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

public partial class DashboardViewModel : ViewModelBase
{
    [ObservableProperty]
    private double _cpuUsage;

    [ObservableProperty]
    private double _memoryPercent;

    [ObservableProperty]
    private string _cpuName = "检测中...";

    [ObservableProperty]
    private string _ramInfo = "";

    [ObservableProperty]
    private string _cpuPowerDisplay = "";

    [ObservableProperty]
    private string _gpuPowerDisplay = "";

    [ObservableProperty]
    private string _anePowerDisplay = "";

    [ObservableProperty]
    private string _totalPowerDisplay = "";

    [ObservableProperty]
    private string _connectionStatus = "连接中...";

    [ObservableProperty]
    private bool _isConnected;

    public ObservableCollection<DiskData> Disks { get; } = new();
    public ObservableCollection<NetworkAdapterData> NetworkAdapters { get; } = new();
    public ObservableCollection<TemperatureData> Temperatures { get; } = new();
    public ObservableCollection<double> MemoryHistory { get; } = new();

    public DashboardViewModel()
    {
        Title = "Dashboard";
        Icon = "📊";
    }

    public override void Update(SystemInfo info)
    {
        CpuUsage = FormatUtils.ValidateDouble(info.CpuUsage);
        MemoryPercent = info.TotalMemory > 0 
            ? (double)info.UsedMemory / info.TotalMemory * 100 
            : 0;
        CpuName = string.IsNullOrWhiteSpace(info.CpuName) ? "未知CPU" : info.CpuName;
        RamInfo = $"{FormatUtils.FormatBytes(info.TotalMemory)} {info.RamType}".Trim();

        // Power
        CpuPowerDisplay = FormatUtils.FormatPower(info.CpuPower);
        GpuPowerDisplay = FormatUtils.FormatPower(info.GpuPower);
        AnePowerDisplay = FormatUtils.FormatPower(info.AnePower);
        var totalMw = info.CpuPower + info.GpuPower + info.AnePower;
        TotalPowerDisplay = totalMw > 0 ? FormatUtils.FormatPower(totalMw) : "";

        // Update collections
        UpdateCollection(Disks, info.Disks);
        UpdateCollection(NetworkAdapters, info.Adapters);
        
        Temperatures.Clear();
        foreach (var t in info.Temperatures.Where(t => t.Temperature > 0))
            Temperatures.Add(t);

        // Memory history chart
        AddMemoryHistoryPoint(MemoryPercent);
    }

    private void AddMemoryHistoryPoint(double value)
    {
        MemoryHistory.Add(value);
        while (MemoryHistory.Count > AppConstants.MaxChartPoints)
            MemoryHistory.RemoveAt(0);
    }

    private static void UpdateCollection<T>(ObservableCollection<T> collection, List<T> newItems)
    {
        collection.Clear();
        foreach (var item in newItems)
            collection.Add(item);
    }
}
