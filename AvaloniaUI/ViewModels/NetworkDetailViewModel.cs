using System.Collections.ObjectModel;
using AvaloniaUI.Models;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AvaloniaUI.ViewModels;

public partial class NetworkDetailViewModel : ViewModelBase
{
    public ObservableCollection<NetworkAdapterData> NetworkAdapters { get; } = new();

    public override void Update(SystemInfo info)
    {
        NetworkAdapters.Clear();
        foreach (var a in info.Adapters) NetworkAdapters.Add(a);
        OnPropertyChanged(string.Empty);
    }

    public override string ToString() => "Network Details";
}
