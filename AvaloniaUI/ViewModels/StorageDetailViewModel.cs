using System.Collections.ObjectModel;
using System.Linq;
using AvaloniaUI.Models;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;

namespace AvaloniaUI.ViewModels;

public partial class StorageDetailViewModel : ViewModelBase
{
    public ObservableCollection<PhysicalDiskView> PhysicalDisks { get; } = new();
    
    [ObservableProperty] private PhysicalDiskView? _selectedPhysicalDisk;
    [ObservableProperty] private bool _smartDetailVisible;

    [RelayCommand]
    private void ToggleSmartDetail() => SmartDetailVisible = !SmartDetailVisible;

    public override void Update(SystemInfo info)
    {
        // Logic mirrored from original MainWindowViewModel to ensure stability
        var map = PhysicalDisks.ToDictionary(p => p.Disk?.SerialNumber ?? "", p => p);
        var alive = new System.Collections.Generic.HashSet<string>();

        foreach (var pd in info.PhysicalDisks)
        {
            if (!map.TryGetValue(pd.SerialNumber, out var view))
            {
                view = new PhysicalDiskView { Disk = pd };
                PhysicalDisks.Add(view);
            }
            else
            {
                view.Disk = pd;
            }
            alive.Add(pd.SerialNumber);
        }

        for (int i = PhysicalDisks.Count - 1; i >= 0; i--)
        {
            if (!alive.Contains(PhysicalDisks[i].Disk?.SerialNumber ?? ""))
                PhysicalDisks.RemoveAt(i);
        }

        if (PhysicalDisks.Count > 0 && SelectedPhysicalDisk == null)
            SelectedPhysicalDisk = PhysicalDisks[0];
    }

    public override string ToString() => "Storage & SMART";
}
