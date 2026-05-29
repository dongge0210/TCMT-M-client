using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

public partial class MemoryViewModel : ViewModelBase
{
    [ObservableProperty]
    private string _totalMemory = "检测中...";

    [ObservableProperty]
    private string _usedMemory = "检测中...";

    [ObservableProperty]
    private string _availableMemory = "检测中...";

    [ObservableProperty]
    private string _compressedMemory = "";

    [ObservableProperty]
    private string _swapUsedDisplay = "";

    [ObservableProperty]
    private string _swapTotalDisplay = "";

    [ObservableProperty]
    private bool _hasSwap;

    [ObservableProperty]
    private double _memoryPercent;

    [ObservableProperty]
    private string _ramType = "";

    [ObservableProperty]
    private uint _ramSpeed;

    public bool HasRamInfo => !string.IsNullOrEmpty(RamType) || RamSpeed > 0;

    public MemoryViewModel()
    {
        Title = "Memory";
        Icon = "🧠";
    }

    public override void Update(SystemInfo info)
    {
        TotalMemory = info.TotalMemory > 0 ? FormatUtils.FormatBytes(info.TotalMemory) : "未检测到";
        UsedMemory = info.UsedMemory > 0 ? FormatUtils.FormatBytes(info.UsedMemory) : "未检测到";
        AvailableMemory = info.AvailableMemory > 0 ? FormatUtils.FormatBytes(info.AvailableMemory) : "未检测到";
        CompressedMemory = info.CompressedMemory > 0 ? FormatUtils.FormatBytes(info.CompressedMemory) : "";
        
        HasSwap = info.SwapTotal > 0;
        SwapUsedDisplay = HasSwap ? FormatUtils.FormatBytes(info.SwapUsed) : "";
        SwapTotalDisplay = HasSwap ? FormatUtils.FormatBytes(info.SwapTotal) : "";
        
        MemoryPercent = info.TotalMemory > 0 ? (double)info.UsedMemory / info.TotalMemory * 100 : 0;
        RamType = info.RamType ?? "";
        RamSpeed = info.RamSpeed;
    }
}
