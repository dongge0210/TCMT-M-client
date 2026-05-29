namespace TCMT.Avalonia.Core;

public static class AppConstants
{
    public const string AppName = "TCMT";
    public const string AppDisplayName = "系统硬件监视器";
    public const string AppVersion = "2.0.0";
    
    // UI Constants
    public const double SidebarWidth = 64;
    public const double HeaderHeight = 64;
    public const double CardCornerRadius = 16;
    public const double CardPadding = 24;
    public const double CardSpacing = 16;
    
    // Chart Constants
    public const int MaxChartPoints = 60;
    public const int UpdateIntervalMs = 500;
    
    // Error Handling
    public const int MaxConsecutiveErrors = 5;
    
    // Colors
    public static class Colors
    {
        public const string Primary = "#FF4CAF50";
        public const string Secondary = "#FF2196F3";
        public const string Warning = "#FFFF9800";
        public const string Danger = "#FFF44336";
        public const string Gold = "#FFFFD700";
        public const string Background = "#FF0D0D0D";
        public const string Surface = "#FF1E1E1E";
        public const string SurfaceLight = "#FF2A2A2A";
        public const string Border = "#FF1F1F1F";
        public const string TextPrimary = "#FFFFFFFF";
        public const string TextSecondary = "#FFB3B3B3";
        public const string TextMuted = "#FF666666";
    }
}
