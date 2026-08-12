using System.Collections.ObjectModel;
using System.Windows.Input;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;
using TCMT.Avalonia.Services.Contracts;
using TCMT.Avalonia.Themes;

namespace TCMT.Avalonia.ViewModels;

public partial class SettingsViewModel : ViewModelBase
{
    private readonly IThemeService _themeService;
    private readonly IFontService _fontService;

    [ObservableProperty]
    private AppTheme _currentTheme = AppTheme.Dark;

    [ObservableProperty]
    private string _appVersion = AppConstants.AppVersion;

    // ── Font selection ──

    public ObservableCollection<string> SystemFonts { get; } = new();

    [ObservableProperty]
    private string _selectedFontFamily = "Inter";

    [ObservableProperty]
    private double _fontSize = 13;

    [ObservableProperty]
    private string _fontPreviewText = "系统硬件监视器 TCMT 0123456789";

    [ObservableProperty]
    private bool _showFontPicker;

    public bool IsNativeFontDialogSupported => _fontService.IsNativeDialogSupported;

    public bool ShowNativeDialogButton => OperatingSystem.IsWindows();

    public ICommand PickSystemFontCommand { get; }

    public SettingsViewModel(IThemeService themeService, IFontService fontService)
    {
        _themeService = themeService;
        _fontService = fontService;
        Title = "设置";
        Icon = "settings";
        CurrentTheme = _themeService.CurrentTheme;

        PickSystemFontCommand = new AsyncRelayCommand(PickSystemFontAsync);

        LoadSystemFonts();
    }

    private void LoadSystemFonts()
    {
        SystemFonts.Clear();
        foreach (var font in _fontService.GetSystemFonts())
            SystemFonts.Add(font);
    }

    partial void OnSelectedFontFamilyChanged(string value)
    {
        _fontService.ApplyAppFont(value, FontSize);
    }

    partial void OnFontSizeChanged(double value)
    {
        _fontService.ApplyAppFont(SelectedFontFamily, value);
    }

    private async Task PickSystemFontAsync()
    {
        var current = new FontSelection(SelectedFontFamily, FontSize, false, false);
        var result = await _fontService.PickFontAsync(current);
        if (result != null)
        {
            SelectedFontFamily = result.Family;
            FontSize = result.Size;
        }
    }

    public override void Update(SystemInfo info)
    {
        // Settings doesn't need hardware updates
    }
}
