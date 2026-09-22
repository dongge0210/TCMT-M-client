using System;
using System.IO;
using System.Text.Json;
using Serilog;
using TCMT.Avalonia.Services.Contracts;
using TCMT.Avalonia.Themes;

namespace TCMT.Avalonia.Services;

/// <summary>
/// 把客户端界面偏好存成用户目录下的一份 JSON。见 <see cref="ClientSettings"/> 关于
/// 「为什么不和采集端配置放一起」的说明。
/// </summary>
public sealed class ClientSettingsStore : IClientSettingsStore
{
    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    private readonly string _path;

    public ClientSettingsStore() : this(DefaultPath()) { }

    public ClientSettingsStore(string path) => _path = path;

    /// <summary>
    /// macOS 走 <c>~/Library/Application Support/TCMT/</c>，Windows 走 <c>%APPDATA%\TCMT\</c>。
    ///
    /// macOS 那半边是写死的：.NET 在 macOS 上把 ApplicationData 映到 <c>~/.config</c>，
    /// 那不是 mac 应用该待的地方，Finder 里也翻不到。
    /// </summary>
    internal static string DefaultPath()
    {
        var root = OperatingSystem.IsMacOS()
            ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                           "Library", "Application Support")
            : Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);

        // 极少数环境拿不到这两个路径（服务账号、裁剪过的运行时）。退回主目录，
        // 存得难看总比存不下强。
        if (string.IsNullOrEmpty(root))
            root = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);

        return Path.Combine(root, "TCMT", "settings.json");
    }

    public ClientSettings Load()
    {
        try
        {
            if (!File.Exists(_path)) return ClientSettings.Default;

            var dto = JsonSerializer.Deserialize<Dto>(File.ReadAllText(_path), Options);
            if (dto is null) return ClientSettings.Default;

            // 逐项兜底而不是整份作废：一个字段写坏了，另外两个还该照用
            return new ClientSettings(
                Enum.TryParse<AppTheme>(dto.Theme, ignoreCase: true, out var theme)
                    ? theme
                    : ClientSettings.Default.Theme,
                string.IsNullOrWhiteSpace(dto.FontFamily)
                    ? ClientSettings.Default.FontFamily
                    : dto.FontFamily,
                dto.FontSize is >= 8 and <= 32
                    ? dto.FontSize
                    : ClientSettings.Default.FontSize);
        }
        catch (Exception ex)
        {
            Log.Warning(ex, "客户端设置读取失败，本次用默认值: {Path}", _path);
            return ClientSettings.Default;
        }
    }

    public void Save(ClientSettings settings)
    {
        try
        {
            var dir = Path.GetDirectoryName(_path);
            if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);

            File.WriteAllText(_path, JsonSerializer.Serialize(
                new Dto(settings.Theme.ToString(), settings.FontFamily, settings.FontSize), Options));
        }
        catch (Exception ex)
        {
            // 存不下不是致命错：本次改动能用，只是下次启动会忘掉。让人知道原因就够了。
            Log.Warning(ex, "客户端设置保存失败: {Path}", _path);
        }
    }

    private sealed record Dto(string? Theme, string? FontFamily, double FontSize);
}
