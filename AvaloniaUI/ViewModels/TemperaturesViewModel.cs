using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

public partial class TemperaturesViewModel : ViewModelBase
{
    [ObservableProperty]
    private string _batteryInfo = string.Empty;

    [ObservableProperty]
    private string _systemInfo = string.Empty;

    [ObservableProperty]
    private string _lastUpdated = string.Empty;

    public ObservableCollection<TemperatureData> CpuGpuSensors { get; } = new();
    public ObservableCollection<TemperatureData> MemoryStorageSensors { get; } = new();
    public ObservableCollection<TemperatureData> MotherboardSensors { get; } = new();
    public ObservableCollection<TemperatureData> BatteryMiscSensors { get; } = new();

    public TemperaturesViewModel()
    {
        Title = "温度";
        Icon = "temperature";
    }

    public override void Update(SystemInfo info)
    {
        UpdateGroup(CpuGpuSensors, info.Temperatures.Where(IsCpuGpuSensor));
        UpdateGroup(MemoryStorageSensors, info.Temperatures.Where(IsMemoryStorageSensor));
        UpdateGroup(MotherboardSensors, info.Temperatures.Where(IsMotherboardSensor));
        UpdateGroup(BatteryMiscSensors, info.Temperatures.Where(IsBatteryMiscSensor));

        BatteryInfo = info.BatteryPercent >= 0
            ? $"Battery {info.BatteryPercent}% · {(info.AcOnline ? "AC Online" : "DC") }"
            : "Battery 未知";

        SystemInfo = string.IsNullOrEmpty(info.OsVersion) ? "" : info.OsVersion;
        LastUpdated = info.LastUpdate == default ? "" : info.LastUpdate.ToString("HH:mm:ss");
    }

    private static void UpdateGroup(ObservableCollection<TemperatureData> group, IEnumerable<TemperatureData> sensors)
    {
        group.Clear();
        foreach (var sensor in sensors)
        {
            group.Add(sensor);
        }
    }

    private static bool IsCpuGpuSensor(TemperatureData data)
    {
        var name = data.SensorName.ToLowerInvariant();
        return name.Contains("cpu") || name.Contains("gpu") || name.Contains("hotspot") || name.Contains("vram") || name.Contains("package");
    }

    private static bool IsMemoryStorageSensor(TemperatureData data)
    {
        var name = data.SensorName.ToLowerInvariant();
        return name.Contains("dimm") || name.Contains("ram") || name.Contains("ssd") || name.Contains("nvme") || name.Contains("swap") || name.Contains("memory");
    }

    private static bool IsMotherboardSensor(TemperatureData data)
    {
        var name = data.SensorName.ToLowerInvariant();
        return name.Contains("pch") || name.Contains("vrm") || name.Contains("board") || name.Contains("ambient") || name.Contains("cache") || name.Contains("lid");
    }

    private static bool IsBatteryMiscSensor(TemperatureData data)
    {
        var name = data.SensorName.ToLowerInvariant();
        return name.Contains("battery") || name.Contains("imu") || name.Contains("heart") || name.Contains("light") || name.Contains("gyro");
    }
}
