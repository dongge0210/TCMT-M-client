using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

public partial class CpuViewModel : ViewModelBase
{
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
    private double _cpuTemperature;

    [ObservableProperty]
    private string _cpuFrequency = "N/A";

    [ObservableProperty]
    private string _cpuEfficiencyFrequency = "N/A";

    [ObservableProperty]
    private double _cpuBaseFreq;

    [ObservableProperty]
    private bool _hyperThreading;

    [ObservableProperty]
    private bool _virtualization;

    [ObservableProperty]
    private string _cpuPowerDisplay = "";

    public CpuViewModel()
    {
        Title = "CPU Details";
        Icon = "🖥️";
    }

    public override void Update(SystemInfo info)
    {
        CpuName = string.IsNullOrWhiteSpace(info.CpuName) ? "未知CPU" : info.CpuName;
        PhysicalCores = info.PhysicalCores;
        LogicalCores = info.LogicalCores;
        PerformanceCores = info.PerformanceCores;
        EfficiencyCores = info.EfficiencyCores;
        CpuUsage = FormatUtils.ValidateDouble(info.CpuUsage);
        CpuTemperature = FormatUtils.ValidateDouble(info.CpuTemperature);
        CpuFrequency = FormatUtils.FormatFrequency(info.PerformanceCoreFreq);
        CpuEfficiencyFrequency = FormatUtils.FormatFrequency(info.EfficiencyCoreFreq);
        CpuBaseFreq = info.CpuBaseFreq;
        HyperThreading = info.HyperThreading;
        Virtualization = info.Virtualization;
        CpuPowerDisplay = FormatUtils.FormatPower(info.CpuPower);
    }
}
