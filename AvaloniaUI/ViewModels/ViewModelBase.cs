using AvaloniaUI.Models;
using CommunityToolkit.Mvvm.ComponentModel;

namespace AvaloniaUI.ViewModels
{
    public abstract class ViewModelBase : ObservableObject
    {
        public virtual void Update(SystemInfo info) { }
    }
}
