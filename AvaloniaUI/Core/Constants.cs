namespace TCMT.Avalonia.Core;

public static class AppConstants
{
    public const string AppName = "TCMT";
    public const string AppDisplayName = "系统硬件监视器";
    public const string AppVersion = "2.0.0";
    
    // UI Constants
    /// <summary>Sidebar width. MainWindow.axaml currently hardcodes 200 in its column definition —
    /// bind it to this constant (or change both together) so the two cannot disagree.</summary>
    public const double SidebarWidth = 200;
    public const double HeaderHeight = 64;
    public const double CardCornerRadius = 16;
    public const double CardPadding = 24;
    public const double CardSpacing = 16;
    
    // Chart Constants
    public const int MaxChartPoints = 60;
    public const int UpdateIntervalMs = 500;
    
    // Error Handling
    public const int MaxConsecutiveErrors = 5;
    
    // Colors.
    //
    // These mirror Themes/DarkTheme.axaml — the theme dictionary is the single source of truth.
    // They are duplicated here only because XAML resources cannot be read by compile-time
    // constants (ConverterColors in Core/Converters/ValueConverters.cs parses them).
    // Change the theme first, then mirror the change here; never let the two drift.
    public static class Colors
    {
        public const string Primary = "#FF238636";
        public const string Secondary = "#FF3B82F6";
        public const string Warning = "#FFD97706";
        public const string Danger = "#FFE5534B";
        public const string Gold = "#FFFFD700";
        // Neutral fallback: an unknown / not-applicable reading — never a failure state.
        public const string Gray = "#FF808080";
        public const string Background = "#FF0D1117";
        public const string Surface = "#FF161B22";
        public const string SurfaceLight = "#FF1A1A26";
        public const string Border = "#FF30363D";
        public const string TextPrimary = "#FFE6EDF3";
        public const string TextSecondary = "#FF8B949E";
        public const string TextMuted = "#FF555555";
    }
}
