using Avalonia;
using Avalonia.Styling;
using System;

namespace TCMT.Avalonia.Themes;

public enum AppTheme
{
    Dark,
    Light,
    System
}

public interface IThemeService
{
    AppTheme CurrentTheme { get; }
    void SetTheme(AppTheme theme);
    event EventHandler<AppTheme>? ThemeChanged;
}

public class ThemeService : IThemeService
{
    private AppTheme _currentTheme = AppTheme.Dark;

    public AppTheme CurrentTheme => _currentTheme;

    public event EventHandler<AppTheme>? ThemeChanged;

    public void SetTheme(AppTheme theme)
    {
        if (_currentTheme == theme) return;

        _currentTheme = theme;
        ApplyTheme(theme);
        ThemeChanged?.Invoke(this, theme);
    }

    private void ApplyTheme(AppTheme theme)
    {
        var app = Application.Current;
        if (app == null) return;

        var variant = theme switch
        {
            AppTheme.Light => ThemeVariant.Light,
            AppTheme.Dark => ThemeVariant.Dark,
            _ => ThemeVariant.Default
        };

        app.RequestedThemeVariant = variant;
    }
}
