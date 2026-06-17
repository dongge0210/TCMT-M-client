using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;
using TCMT.Avalonia.Themes;

namespace TCMT.Avalonia.ViewModels;

public partial class SettingsViewModel : ViewModelBase
{
    private readonly IThemeService _themeService;

    [ObservableProperty]
    private AppTheme _currentTheme = AppTheme.Dark;

    [ObservableProperty]
    private string _appVersion = AppConstants.AppVersion;

    public SettingsViewModel(IThemeService themeService)
    {
        _themeService = themeService;
        Title = "设置";
        Icon = "⚙️";
        CurrentTheme = _themeService.CurrentTheme;
    }

    partial void OnCurrentThemeChanged(AppTheme value)
    {
        _themeService.SetTheme(value);
    }

    public override void Update(SystemInfo info)
    {
        // Settings doesn't need hardware updates
    }
}
