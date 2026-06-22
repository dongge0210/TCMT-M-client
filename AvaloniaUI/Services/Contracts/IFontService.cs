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
}
