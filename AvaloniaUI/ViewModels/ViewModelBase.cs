using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

public abstract partial class ViewModelBase : ObservableObject
{
    [ObservableProperty]
    private string _title = "";

    [ObservableProperty]
    private string _icon = "dashboard";

    [ObservableProperty]
    private bool _isLoading;

    /// <summary>
    /// 采集端连上没有。放在基类是因为连不连得上不是某一页的事，所有页面同一个答案。
    /// 目前只有硬件规格页按它改画法 —— 离线时那十个部件的读数无从谈起，整页改画占位符。
    /// </summary>
    [ObservableProperty]
    private bool _isOnline;

    partial void OnIsOnlineChanged(bool value) => OnOnlineChanged(value);

    /// <summary>连接态翻转时叫一次。默认什么都不做。</summary>
    protected virtual void OnOnlineChanged(bool online) { }

    public virtual void Update(SystemInfo info) { }
}
