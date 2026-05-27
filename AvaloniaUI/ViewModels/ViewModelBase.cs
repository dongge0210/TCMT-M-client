using AvaloniaUI.Models;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AvaloniaUI.ViewModels
{
    public abstract partial class ViewModelBase : ObservableObject
    {
        private string _title = "";
        public string Title { get => _title; set => SetProperty(ref _title, value); }

        private string _icon = "📊";
        public string Icon { get => _icon; set => SetProperty(ref _icon, value); }

        public virtual void Update(SystemInfo info) { }
    }

}
