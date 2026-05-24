using System.Collections.ObjectModel;
using AvaloniaUI.Models;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AvaloniaUI.ViewModels;

public partial class DashboardViewModel : ViewModelBase
{
    [ObservableProperty]
    private string _title = "System Overview";

    [ObservableProperty]
    private double _cpuUsage;

    [ObservableProperty]
    private double _memoryPercent;

    [ObservableProperty]
    private string _cpuName = "";

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

    public ObservableCollection<DiskData> Disks { get; } = new();
    public ObservableCollection<NetworkAdapterData> NetworkAdapters { get; } = new();
    public ObservableCollection<TemperatureData> Temperatures { get; } = new();

    public override void Update(SystemInfo info)
    {
        CpuUsage = info.CpuUsage;
        MemoryPercent = info.TotalMemory > 0 ? (double)info.UsedMemory / info.TotalMemory * 100 : 0;
        CpuName = info.CpuName;
        RamInfo = $"{FormatUtil.FormatBytes(info.TotalMemory)} {info.RamType}";

        Disks.Clear();
        foreach (var d in info.Disks) Disks.Add(d);
        
        NetworkAdapters.Clear();
        foreach (var a in info.Adapters) NetworkAdapters.Add(a);
        
        Temperatures.Clear();
        foreach (var t in info.Temperatures.Where(t => t.Temperature > 0 && !string.IsNullOrWhiteSpace(t.SensorName))) 
            Temperatures.Add(t);

        CpuPowerDisplay = info.CpuPower > 0 ? $"{info.CpuPower / 1000.0:F2}W" : "";
        GpuPowerDisplay = info.GpuPower > 0 ? $"{info.GpuPower / 1000.0:F2}W" : "";
        AnePowerDisplay = info.AnePower > 0 ? $"{info.AnePower / 1000.0:F2}W" : "";
        var totalMw = info.CpuPower + info.GpuPower + info.AnePower;
        TotalPowerDisplay = totalMw > 0 ? $"{totalMw / 1000.0:F2}W" : "";
        
        // Ensure UI updates are notified
        OnPropertyChanged(string.Empty); 
    }
}
