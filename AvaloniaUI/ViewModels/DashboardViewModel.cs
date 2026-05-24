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
}
