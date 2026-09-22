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

    public HardwareSpecViewModel(HardwarePart part)
    {
        Part = part;
        Title = HardwareParts.NameOf(part);
        Icon = "hardware";
    }

    public override void Update(SystemInfo info) => Sheet = HardwareSpecBuilder.Build(Part, info);
}
