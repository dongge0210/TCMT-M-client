using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Localization;
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

    // 页头那行副标题、离线提示条、SMART 表头都是写死的文案，语言一变也要跟着变。
    // 做成属性而不是绑资源，是因为这里已经有现成的重画路径（Rebuild），
    // 多一套 DynamicResource 只会多一处要对齐的东西。
    public string PageSubtitle => Translator.T("静态规格与实时读数：采集端没提供、或这台机器没有的项，报 N/A 并说明原因，不编值");

    public string OfflineBanner => Translator.T("未连接采集端 —— 下面的值等连上再填");

    public string ColumnAttribute => Translator.T("属性");
    public string ColumnCurrent => Translator.T("当前");
    public string ColumnWorst => Translator.T("最差");
    public string ColumnThreshold => Translator.T("阈值");
    public string ColumnRaw => Translator.T("原始值");

    public HardwareSpecViewModel(HardwarePart part)
    {
        Part = part;
        Title = HardwareParts.NameOf(part);
        Icon = "hardware";

        // 不解除订阅：十个实例与程序同生命周期，没有退场的时机。
        Translator.LanguageChanged += (_, _) => Rebuild();
    }

    public override void Update(SystemInfo info)
    {
        _lastInfo = info;
        Rebuild();
    }

    protected override void OnOnlineChanged(bool online) => Rebuild();

    // 开机就没连上时 _lastInfo 还是 null，给一份空的：Build 照常把行标签铺出来，
    // 值由 online=false 全部盖成「—」—— 这正是没数据时该有的样子。
    private void Rebuild()
    {
        Sheet = Translator.TranslateSheet(
            HardwareSpecBuilder.Build(Part, _lastInfo ?? new SystemInfo(), IsOnline));

        // 上面那六个属性跟着语言走，得挨个通知一遍
        foreach (var p in new[] { nameof(PageSubtitle), nameof(OfflineBanner),
                                  nameof(ColumnAttribute), nameof(ColumnCurrent),
                                  nameof(ColumnWorst), nameof(ColumnThreshold), nameof(ColumnRaw) })
        {
            OnPropertyChanged(p);
        }
    }
}
