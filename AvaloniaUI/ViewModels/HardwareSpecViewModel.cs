using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

/// <summary>
/// 硬件视图里的一页。
///
/// 十个部件共用这一个类，各持一个 <see cref="Part"/> —— 它们本来就是同一种东西（一张规格表），
/// 只是内容不同。写十个 VM 就得配十个 DataTemplate，加一个部件要动四处；这里只动
/// <see cref="HardwarePart"/> 和 <see cref="HardwareSpecBuilder"/>。
/// </summary>
public partial class HardwareSpecViewModel : ViewModelBase
{
    public HardwarePart Part { get; }

    [ObservableProperty]
    private SpecSheet _sheet = SpecSheet.Empty();

    /// <summary>
    /// 最近一次拿到的整机信息。留一份是为了连接态翻转时还能重画 ——
    /// 断了线就再不会有新数据送过来，不留这一份就只能干等下一次采样。
    /// </summary>
    private SystemInfo? _lastInfo;

    public HardwareSpecViewModel(HardwarePart part)
    {
        Part = part;
        Title = HardwareParts.NameOf(part);
        Icon = "hardware";
    }

    public override void Update(SystemInfo info)
    {
        _lastInfo = info;
        Rebuild();
    }

    protected override void OnOnlineChanged(bool online) => Rebuild();

    // 开机就没连上时 _lastInfo 还是 null，给一份空的：Build 照常把行标签铺出来，
    // 值由 online=false 全部盖成「—」—— 这正是没数据时该有的样子。
    private void Rebuild() =>
        Sheet = HardwareSpecBuilder.Build(Part, _lastInfo ?? new SystemInfo(), IsOnline);
}
