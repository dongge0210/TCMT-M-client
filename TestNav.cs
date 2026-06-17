using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using AvaloniaUI.ViewModels;

// Minimal reproduction of MainWindowViewModel structure
public class MainWindowViewModel : CommunityToolkit.Mvvm.ComponentModel.ObservableObject
{
    public ObservableCollection<ViewModelBase> Pages { get; } = new();
    public MainWindowViewModel() {
        Pages.Add(new DashboardViewModel());
    }
}
