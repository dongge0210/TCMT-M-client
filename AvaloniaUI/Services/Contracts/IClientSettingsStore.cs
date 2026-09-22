using TCMT.Avalonia.Themes;

namespace TCMT.Avalonia.Services.Contracts;

/// <summary>
/// 客户端自己的界面偏好。
///
/// 别和「采集端配置」混起来 —— 上传开关、采样间隔、语言那几项落在采集端的
/// <c>system_monitor.json</c> 里，是这台机器上所有 TCMT 界面共用的；这里这三项只属于
/// 这个桌面程序。混在一份文件里的话，TUI 读到 <c>ui.fontFamily</c> 还得决定忽略它。
/// </summary>
public sealed record ClientSettings(AppTheme Theme, string FontFamily, double FontSize)
{
    /// <summary>没存过配置时的样子，与各服务自己的默认值一致。</summary>
    public static ClientSettings Default { get; } = new(AppTheme.Dark, "Inter", 13);
}

public interface IClientSettingsStore
{
    /// <summary>读不到或读坏了都回默认值 —— 一份坏配置不该拦住启动。</summary>
    ClientSettings Load();

    void Save(ClientSettings settings);
}
