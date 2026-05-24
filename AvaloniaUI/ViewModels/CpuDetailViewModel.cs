using System.Collections.ObjectModel;
using AvaloniaUI.Models;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AvaloniaUI.ViewModels;

public partial class CpuDetailViewModel : ViewModelBase
{
    [ObservableProperty] private string _cpuName = "";
    [ObservableProperty] private int _physicalCores;
    [ObservableProperty] private int _logicalCores;
    [ObservableProperty] private double _cpuUsage;
    [ObservableProperty] private double _cpuTemperature;
    [ObservableProperty] private string _cpuFrequency = "N/A";
    
    public override void Update(SystemInfo info)
    {
        CpuName = info.CpuName;
        PhysicalCores = info.PhysicalCores;
        LogicalCores = info.LogicalCores;
        CpuUsage = info.CpuUsage;
        CpuTemperature = info.CpuTemperature;
        CpuFrequency = $"{info.PerformanceCoreFreq:F0} MHz";
    }

    public override string ToString() => "CPU Details";
}
