namespace TCMT.Avalonia.Core;

/// <summary>Available / needs elevation / not available on this platform.</summary>
public enum CapabilityState
{
    Available,
    RequiresElevation,
    Unavailable
}

public enum PlatformKind
{
    MacOS,
    Windows,
    Linux
}

/// <summary>
/// Local-platform capability layer (TUI design #10: show what has a handler, never fake the rest).
///
/// The rule the views must follow: a missing capability never removes a panel. The panel stays
/// where it is, shows <see cref="NotAvailable"/> in the value slot, and says why in one muted
/// line — so the layout stays identical across platforms and a blank never gets misread as
/// "everything is fine".
/// </summary>
public static class PlatformCaps
{
    public const string NotAvailable = "N/A";

    public static PlatformKind Current =>
        OperatingSystem.IsMacOS() ? PlatformKind.MacOS :
        OperatingSystem.IsWindows() ? PlatformKind.Windows :
        PlatformKind.Linux;

    /// <summary>
    /// SSID and signal strength are only readable where the platform exposes location services.
    /// When false the network page must say the value is unreadable, not that it is disconnected.
    /// </summary>
    public static bool ProvidesLocationServices => Current == PlatformKind.MacOS;

    /// <summary>SMART needs elevation on Linux; macOS and Windows hand it out directly.</summary>
    public static CapabilityState SmartAccess => Current switch
    {
        PlatformKind.Linux => CapabilityState.RequiresElevation,
        PlatformKind.MacOS or PlatformKind.Windows => CapabilityState.Available,
        _ => CapabilityState.Unavailable
    };

    // Degradation copy, kept in one place so every panel says the same thing.
    public const string NoBatteryNote = "该主机未报告电池设备";
    public const string NoFanTachometerNote = "该主机未暴露风扇转速";
    public const string SmartNeedsRootNote = "需要 root 权限";
    public const string SsidUnavailableNote = "该系统不提供定位服务，SSID 与信号强度读不到——不是没连上";

    /// <summary>Human-readable state for the capability list in Settings.</summary>
    public static string Describe(CapabilityState state) => state switch
    {
        CapabilityState.Available => "已接入",
        CapabilityState.RequiresElevation => SmartNeedsRootNote,
        _ => "不可用"
    };
}
