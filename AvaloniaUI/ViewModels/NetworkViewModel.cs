using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
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

    public string BtDisplay => HasBluetooth ? $"蓝牙：{(BtPowerOn ? "已开启" : "已关闭")} ({BtDeviceCount} 台设备)" : "蓝牙未检测到";

    public NetworkViewModel()
    {
        Title = "网络";
        Icon = "🌐";
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

        OnPropertyChanged(nameof(WifiDisplay));
        OnPropertyChanged(nameof(BtDisplay));
    }
}
