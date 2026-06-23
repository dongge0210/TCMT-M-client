using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

public partial class GpuViewModel : ViewModelBase
{
    [ObservableProperty]
    private string _gpuName = "检测中...";

    [ObservableProperty]
    private string _gpuBrand = string.Empty;

    [ObservableProperty]
    private string _gpuMemory = string.Empty;

    [ObservableProperty]
    private double _gpuUsage;

    [ObservableProperty]
    private double _gpuTemperature;

    [ObservableProperty]
    private string _coreClock = string.Empty;

    [ObservableProperty]
    private string _memoryClock = string.Empty;

    [ObservableProperty]
    private string _gpuPowerDisplay = string.Empty;

    [ObservableProperty]
    private string _driverInfo = string.Empty;

    public ObservableCollection<GpuData> Gpus { get; } = new();

    public GpuViewModel()
    {
        Title = "GPU";
        Icon = "🎮";
    }

    public override void Update(SystemInfo info)
    {
        Gpus.Clear();
        foreach (var gpu in info.Gpus)
        {
            Gpus.Add(gpu);
        }

        var selectedGpu = Gpus.Count > 0
            ? Gpus[0]
            : new GpuData
            {
                Name = string.IsNullOrEmpty(info.GpuName) ? "未知显卡" : info.GpuName,
                Brand = info.GpuBrand,
                Memory = info.GpuMemory,
                CoreClock = info.GpuCoreFreq,
                Temperature = info.GpuTemperature,
                Usage = 0
            };

        GpuName = string.IsNullOrEmpty(selectedGpu.Name) ? "未知显卡" : selectedGpu.Name;
        GpuBrand = selectedGpu.Brand;
        GpuMemory = selectedGpu.MemoryDisplay;
        GpuUsage = selectedGpu.Usage;
        GpuTemperature = selectedGpu.Temperature;
        CoreClock = FormatUtils.FormatFrequency(selectedGpu.CoreClock);
        MemoryClock = FormatUtils.FormatFrequency(info.GpuFreq);
        GpuPowerDisplay = FormatUtils.FormatPower(info.GpuPower);
        DriverInfo = string.IsNullOrEmpty(selectedGpu.Brand) ? "" : selectedGpu.Brand;
    }
}
