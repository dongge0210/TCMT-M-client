using AvaloniaUI.Models;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AvaloniaUI.ViewModels
{
    public abstract partial class ViewModelBase : ObservableObject
    {
        [ObservableProperty]
        private string _title = "";

        public string Title { get => _title; set => SetProperty(ref _title, value); }
        
        public virtual void Update(SystemInfo info) { }
    }
}
