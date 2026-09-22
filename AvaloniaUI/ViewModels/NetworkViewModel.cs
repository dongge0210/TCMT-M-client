using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Localization;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

public partial class NetworkViewModel : ViewModelBase
{
    [ObservableProperty]
    private ObservableCollection<NetworkAdapterData> _adapters = new();

    [ObservableProperty]
    private NetworkAdapterData? _selectedAdapter;

    [ObservableProperty]
    private string _wifiSSID = "";

    [ObservableProperty]
    private int _wifiRSSI;

    [ObservableProperty]
    private int _wifiChannel;

    [ObservableProperty]
    private string _wifiSecurity = "";

    [ObservableProperty]
    private string _wifiBand = "";

    [ObservableProperty]
    private string _wifiGen = "";

    [ObservableProperty]
    private bool _hasWiFi;

    [ObservableProperty]
    private bool _hasBluetooth;

    [ObservableProperty]
    private bool _btPowerOn;

    [ObservableProperty]
    private int _btDeviceCount;

    /// <summary>已连接设备名，多个用 " · " 连接。空表示服务端没提供（旧版本）或当前无已连接设备。</summary>
    [ObservableProperty]
    private string _btDeviceName = "";

    public string WifiDisplay
    {
        get
        {
            if (!HasWiFi) return "WiFi：未检测到适配器";
            var extra = "";
            if (!string.IsNullOrEmpty(WifiBand)) extra += "  " + WifiBand;
            if (!string.IsNullOrEmpty(WifiGen)) extra += "  " + WifiGen;
            if (!string.IsNullOrEmpty(WifiSSID))
                return $"WiFi：{WifiSSID}  频道:{WifiChannel}  信号:{WifiRSSI} dBm  {WifiSecurity}{extra}";
            if (WifiRSSI != 0 || WifiChannel != 0)
                return $"WiFi：已连接  频道:{WifiChannel}  信号:{WifiRSSI} dBm  {WifiSecurity}{extra}";
            return "WiFi：已断开";
        }
    }

    // 有已连接设备名就报名字 —— 比"3 台设备"有用得多，也正是设计稿要显示的东西。
    // 名字来自采集端的 JoinConnectedDeviceNames；旧服务端没这个字段时回落到数量。
    public string BtDisplay => !HasBluetooth
        ? "蓝牙未检测到"
        : $"蓝牙：{(BtPowerOn ? "已开启" : "已关闭")}" +
          (string.IsNullOrEmpty(BtDeviceName) ? $" ({BtDeviceCount} 台设备)" : $" {BtDeviceName}");

    /// <summary>Inverse of HasWiFi, for the "capability missing" note in NetworkView.
    /// The section stays in place and explains itself instead of collapsing to nothing.</summary>
    public bool HasNoWiFi => !HasWiFi;

    /// <summary>
    /// Degradation copy in the terminal's four-state wording. Shown only when it says something
    /// the SSID row does not already say — a connected adapter with a readable SSID needs no note.
    /// The state is written out in words, so it never depends on colour to be understood.
    /// </summary>
    public string WifiStateNote
    {
        get
        {
            if (!HasWiFi || !string.IsNullOrEmpty(WifiSSID))
            {
                return "";
            }

            return WifiRSSI != 0 || WifiChannel != 0
                ? "已连接，但 SSID 读不到——系统需要开启定位服务才会返回 SSID"
                : "未连接——在系统设置里选择网络，这里会跟着变";
        }
    }

    public bool HasWifiNote => HasWiFi && WifiStateNote.Length > 0;

    /// <summary>No adapter reported at all — the interface table says so instead of staying blank.</summary>
    public bool HasNoAdapters => Adapters.Count == 0;

    /// <summary>
    /// 「3 适配器」那种带数字的短句。做成属性而不是 XAML 里的 StringFormat：
    /// 数字和量词的顺序中英不一样，StringFormat 拼不出来。
    /// </summary>
    public string AdapterCountText => Translator.T("{0} 适配器", Adapters.Count);

    public NetworkViewModel()
    {
        Title = Translator.T("网络");
        Icon = "network";
    }

    public override void Update(SystemInfo info)
    {
        // Preserve selection
        var previousKey = SelectedAdapter != null ? $"{SelectedAdapter.Name}|{SelectedAdapter.Mac}" : null;
        
        Adapters.Clear();
        foreach (var adapter in info.Adapters)
            Adapters.Add(adapter);

        // Restore selection
        if (previousKey != null)
        {
            SelectedAdapter = Adapters.FirstOrDefault(a => $"{a.Name}|{a.Mac}" == previousKey);
        }
        
        // Auto-select active adapter
        if (SelectedAdapter == null && Adapters.Count > 0)
        {
            SelectedAdapter = Adapters.FirstOrDefault(a =>
                !string.IsNullOrEmpty(a.IpAddress) && 
                a.IpAddress != "127.0.0.1" && 
                a.IpAddress != "::1" && 
                a.Speed > 0)
                ?? Adapters.FirstOrDefault(a => !string.IsNullOrEmpty(a.IpAddress))
                ?? Adapters[0];
        }

        // WiFi
        HasWiFi = info.HasWiFi;
        WifiSSID = info.WifiSSID ?? "";
        WifiRSSI = info.WifiRSSI;
        WifiChannel = info.WifiChannel;
        WifiSecurity = info.WifiSecurity ?? "";
        WifiBand = info.WifiBand ?? "";
        WifiGen = info.WifiGen ?? "";

        // Bluetooth
        HasBluetooth = info.HasBluetooth;
        BtPowerOn = info.BtPowerOn;
        BtDeviceCount = info.BtDeviceCount;
        BtDeviceName = info.BtDeviceName ?? "";

        OnPropertyChanged(nameof(WifiDisplay));
        OnPropertyChanged(nameof(BtDisplay));
        OnPropertyChanged(nameof(HasNoWiFi));
        OnPropertyChanged(nameof(WifiStateNote));
        OnPropertyChanged(nameof(HasWifiNote));
        OnPropertyChanged(nameof(HasNoAdapters));
        OnPropertyChanged(nameof(AdapterCountText));
    }
}
