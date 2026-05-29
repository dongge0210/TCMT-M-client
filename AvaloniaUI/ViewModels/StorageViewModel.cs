using System.Collections.ObjectModel;
using System.Linq;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

public partial class StorageViewModel : ViewModelBase
{
    [ObservableProperty]
    private ObservableCollection<DiskData> _disks = new();

    [ObservableProperty]
    private DiskData? _selectedDisk;

    [ObservableProperty]
    private ObservableCollection<PhysicalDiskView> _physicalDisks = new();

    [ObservableProperty]
    private PhysicalDiskView? _selectedPhysicalDisk;

    [ObservableProperty]
    private bool _smartDetailVisible;

    public StorageViewModel()
    {
        Title = "Storage & SMART";
        Icon = "💾";
    }

    public override void Update(SystemInfo info)
    {
        // Preserve disk selection
        var previousDiskKey = SelectedDisk != null ? $"{SelectedDisk.Letter}:{SelectedDisk.Label}" : null;
        
        Disks.Clear();
        foreach (var disk in info.Disks)
            Disks.Add(disk);

        if (previousDiskKey != null)
        {
            SelectedDisk = Disks.FirstOrDefault(d => $"{d.Letter}:{d.Label}" == previousDiskKey);
        }
        
        if (SelectedDisk == null && Disks.Count > 0)
            SelectedDisk = Disks[0];

        // Update physical disks
        BuildOrUpdatePhysicalDisks(info);
    }

    private void BuildOrUpdatePhysicalDisks(SystemInfo info)
    {
        try
        {
            var map = PhysicalDisks.ToDictionary(p => p.Disk?.SerialNumber ?? "", p => p);
            var alive = new HashSet<string>();

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

            // Remove dead disks
            for (int i = PhysicalDisks.Count - 1; i >= 0; i--)
            {
                if (!alive.Contains(PhysicalDisks[i].Disk?.SerialNumber ?? ""))
                    PhysicalDisks.RemoveAt(i);
            }

            // Map logical volumes
            foreach (var phys in PhysicalDisks)
                phys.Partitions.Clear();

            if (OperatingSystem.IsMacOS())
            {
                if (PhysicalDisks.Count > 0)
                {
                    var firstPhys = PhysicalDisks[0];
                    foreach (var d in Disks)
                        firstPhys.Partitions.Add(d);
                }
            }
            else
            {
                var dmap = Disks.Where(d => d.Letter != '\0')
                               .ToDictionary(d => d.Letter, d => d);
                foreach (var phys in PhysicalDisks)
                {
                    if (phys.Disk?.LogicalDriveLetters == null) continue;
                    foreach (var letter in phys.Disk.LogicalDriveLetters)
                    {
                        if (dmap.TryGetValue(letter, out var disk))
                            phys.Partitions.Add(disk);
                    }
                }
            }

            if (PhysicalDisks.Count > 0 && SelectedPhysicalDisk == null)
                SelectedPhysicalDisk = PhysicalDisks[0];
        }
        catch (Exception)
        {
            // Log error but don't crash
        }
    }

    [RelayCommand]
    private void ToggleSmartDetail()
    {
        SmartDetailVisible = !SmartDetailVisible;
    }
}
