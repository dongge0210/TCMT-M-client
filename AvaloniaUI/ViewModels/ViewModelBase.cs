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

    public virtual void Update(SystemInfo info) { }
}
