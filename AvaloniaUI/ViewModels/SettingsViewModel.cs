using System.Collections.ObjectModel;
using System.Text.Json;
using System.Windows.Input;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Serilog;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;
using TCMT.Avalonia.Services.Contracts;
using TCMT.Avalonia.Themes;

namespace TCMT.Avalonia.ViewModels;

public partial class SettingsViewModel : ViewModelBase
{
    private readonly IThemeService _themeService;
    private readonly IFontService _fontService;
    private readonly IHardwareService _hardwareService;

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

    /// <summary>驱动「系统字体面板」按钮的可见性。走服务，不在这层自己判断平台 ——
    /// 之前这里另写了一个 OperatingSystem.IsWindows() 的属性，和服务的判断各说各话。</summary>
    public bool IsNativeFontDialogSupported => _fontService.IsNativeDialogSupported;

    public ICommand PickSystemFontCommand { get; }

    public SettingsViewModel(
        IThemeService themeService,
        IFontService fontService,
        IHardwareService hardwareService)
    {
        _themeService = themeService;
        _fontService = fontService;
        _hardwareService = hardwareService;
        Title = "设置";
        Icon = "settings";
        CurrentTheme = _themeService.CurrentTheme;

        PickSystemFontCommand = new AsyncRelayCommand(PickSystemFontAsync);

        LoadSystemFonts();

        _hardwareService.ConfigSnapshotReceived += (_, json) => ApplySnapshot(json);
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

    // ── 采集端配置 ────────────────────────────────────────────────────────
    //
    // 这些键不属于客户端，它们落在采集端的 system_monitor.json 里，和 TUI 按 S 打开的
    // 是同一份 —— 两个界面改的是同一个东西，不是两份。
    //
    // 每次只发改动的那一个键，不是整份配置：快照可能在手里放了很久，整份发回去会把
    // 期间别人（比如 TUI）改过的键一起盖掉。

    /// <summary>采集端有没有在推数据。没连上时这一整张卡只能看不能动。</summary>
    public bool CollectorEditable => IsOnline;

    [ObservableProperty] private bool _collectorUploadEnabled;

    [ObservableProperty] private string _collectorUrl = "";

    [ObservableProperty] private bool _collectorInsecure;

    [ObservableProperty] private int _collectorIntervalSec = 2;

    [ObservableProperty] private int _collectorSampleIntervalMs = 500;

    /// <summary>语言在配置里是 BCP-47 短码，界面上给人看的是名字。就两种，不做查表。</summary>
    public IReadOnlyList<string> Languages { get; } = ["中文", "English"];

    [ObservableProperty] private string _collectorLanguage = "中文";

    // 界面在往回调写：正在把收到的快照铺开。铺开的时候不能再往回发，
    // 否则每收到一份快照就回一份，两边对着刷。
    private bool _applyingSnapshot;

    /// <summary>最近一份快照的原文。发不出去时拿它把界面退回采集端的真实状态。</summary>
    private string? _lastSnapshot;

    protected override void OnOnlineChanged(bool online) => OnPropertyChanged(nameof(CollectorEditable));

    private void ApplySnapshot(string json)
    {
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;

            _applyingSnapshot = true;
            _lastSnapshot = json;

            // 取不到的键保持原样：老版本采集端可能只推一部分，不该把界面清空
            if (root.TryGetProperty("server.enabled", out var en) && en.ValueKind is JsonValueKind.True or JsonValueKind.False)
                CollectorUploadEnabled = en.GetBoolean();
            if (root.TryGetProperty("server.url", out var url) && url.ValueKind == JsonValueKind.String)
                CollectorUrl = url.GetString() ?? "";
            if (root.TryGetProperty("server.insecure", out var ins) && ins.ValueKind is JsonValueKind.True or JsonValueKind.False)
                CollectorInsecure = ins.GetBoolean();
            if (root.TryGetProperty("server.intervalSec", out var iv) && iv.TryGetInt32(out var ivv))
                CollectorIntervalSec = ivv;
            if (root.TryGetProperty("sampling.intervalMs", out var sm) && sm.TryGetInt32(out var smv))
                CollectorSampleIntervalMs = smv;
            if (root.TryGetProperty("ui.language", out var lg) && lg.ValueKind == JsonValueKind.String)
                CollectorLanguage = lg.GetString() == "en" ? "English" : "中文";
        }
        catch (JsonException ex)
        {
            Log.Warning(ex, "设置：配置快照不是合法 JSON，忽略");
        }
        finally
        {
            _applyingSnapshot = false;
        }
    }

    partial void OnCollectorUploadEnabledChanged(bool value) => Push("server.enabled", value);
    partial void OnCollectorUrlChanged(string value) => Push("server.url", value);
    partial void OnCollectorInsecureChanged(bool value) => Push("server.insecure", value);
    partial void OnCollectorIntervalSecChanged(int value) => Push("server.intervalSec", value);
    partial void OnCollectorSampleIntervalMsChanged(int value) => Push("sampling.intervalMs", value);
    partial void OnCollectorLanguageChanged(string value) => Push("ui.language", value == "English" ? "en" : "zh-Hans");

    private void Push(string key, object value)
    {
        if (_applyingSnapshot) return;
        _ = PushAsync(key, value);
    }

    private async Task PushAsync(string key, object value)
    {
        var payload = JsonSerializer.Serialize(new Dictionary<string, object> { [key] = value });
        try
        {
            if (await _hardwareService.SendConfigAsync(payload)) return;
        }
        catch (Exception ex)
        {
            Log.Warning(ex, "设置：{Key} 发送失败", key);
        }

        // 没送出去就把界面退回采集端报的值 —— 留着用户刚敲的数会让人以为改成功了。
        // 采集端应用成功时会自己推一份新快照回来，那条路不需要这里做什么。
        Log.Warning("设置：{Key} 没能送到采集端，界面回退", key);
        if (_lastSnapshot is not null) ApplySnapshot(_lastSnapshot);
    }
}
