using System;
using CommunityToolkit.Mvvm.ComponentModel;

namespace TCMT.Avalonia.Services.Contracts;

public interface INavigationService
{
    ObservableObject? CurrentViewModel { get; }
    event EventHandler<ObservableObject>? Navigated;
    
    void NavigateTo<T>() where T : ObservableObject;
    void NavigateTo(ObservableObject viewModel);
    bool CanNavigateBack { get; }
    void NavigateBack();
}
