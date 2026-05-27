using CommunityToolkit.Mvvm.ComponentModel;

namespace AvaloniaUI.ViewModels;

public partial class SettingsViewModel : ViewModelBase
{
    [ObservableProperty] private string _appVersion = "v0.14";
    [ObservableProperty] private string _ipcStatus = "Unknown";

    public override string ToString() => "Settings";
}
