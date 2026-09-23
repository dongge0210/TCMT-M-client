namespace TCMT.Avalonia.Core;

public static class AppConstants
{
    public const string AppName = "TCMT";
    public const string AppDisplayName = "系统硬件监视器";
    // 版本号的源头是根 CMakeLists.txt 的 project(TCMT-M VERSION …) —— release-mac.sh 就取那一个，
    // 发出去的包叫 TCMT-M-<那个版本>.tar.gz。改版本时这几处要一起改：
    //   CMakeLists.txt   project(TCMT-M VERSION …)
    //   AvaloniaUI/Core/Constants.cs                 ← 这里
    //   AvaloniaUI/TCMT.Avalonia.csproj              <AssemblyVersion>
    //   src/core/MCP/MCPServer.cpp                   SERVER_VERSION
    //   CLAUDE.md                                    开头那句
    // （C++ 侧不用管：TCMT_VERSION_STR 在 DataStruct.h 里只是兜底，CMake 会覆盖。）
    public const string AppVersion = "0.14.0";
    
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
