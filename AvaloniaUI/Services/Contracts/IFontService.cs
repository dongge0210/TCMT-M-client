namespace TCMT.Avalonia.Services.Contracts;

public record FontSelection(string Family, double Size, bool IsBold, bool IsItalic);

public interface IFontService
{
    /// <summary>
    /// Open the system font panel (macOS NSFontPanel, Windows ChooseFont).
    /// Returns the selected font, or null if the user cancelled.
    /// </summary>
    Task<FontSelection?> PickFontAsync(FontSelection? current = null);

    /// <summary>
    /// Return all available system font families.
    /// </summary>
    IReadOnlyList<string> GetSystemFonts();

    /// <summary>
    /// Whether the platform supports native font dialog.
    /// (Linux typically returns false — fontconfig doesn't have a standard UI.)
    /// </summary>
    bool IsNativeDialogSupported { get; }

    /// <summary>
    /// Apply a font family globally to the running application.
    /// </summary>
    void ApplyAppFont(string family, double size = 13);

    /// <summary>
    /// Current application font family, or null if default.
    /// </summary>
    string? CurrentFontFamily { get; }

    /// <summary>
    /// Raised when the app font is changed.
    /// </summary>
    event Action<string, double>? FontChanged;

    /// <summary>
    /// Get a platform-appropriate CJK fallback font name.
    /// macOS: PingFang SC / Windows: Microsoft YaHei / Linux: Noto Sans CJK SC
    /// </summary>
    string GetCjkFallbackFont();
}
