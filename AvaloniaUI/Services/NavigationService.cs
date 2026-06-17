using System;
using System.Collections.Generic;
using CommunityToolkit.Mvvm.ComponentModel;
using Microsoft.Extensions.DependencyInjection;
using TCMT.Avalonia.Services.Contracts;

namespace TCMT.Avalonia.Services;

public class NavigationService : ObservableObject, INavigationService
{
    private readonly IServiceProvider _serviceProvider;
    private readonly Stack<ObservableObject> _history = new();
    private ObservableObject? _currentViewModel;

    public ObservableObject? CurrentViewModel
    {
        get => _currentViewModel;
        private set
        {
            if (SetProperty(ref _currentViewModel, value))
            {
                Navigated?.Invoke(this, value!);
            }
        }
    }

    public bool CanNavigateBack => _history.Count > 0;

    public event EventHandler<ObservableObject>? Navigated;

    public NavigationService(IServiceProvider serviceProvider)
    {
        _serviceProvider = serviceProvider;
    }

    public void NavigateTo<T>() where T : ObservableObject
    {
        var vm = _serviceProvider.GetRequiredService<T>();
        NavigateTo(vm);
    }

    public void NavigateTo(ObservableObject viewModel)
    {
        if (CurrentViewModel != null)
        {
            _history.Push(CurrentViewModel);
        }
        CurrentViewModel = viewModel;
        OnPropertyChanged(nameof(CanNavigateBack));
    }

    public void NavigateBack()
    {
        if (_history.Count > 0)
        {
            CurrentViewModel = _history.Pop();
            OnPropertyChanged(nameof(CanNavigateBack));
        }
    }

    public void ClearHistory()
    {
        _history.Clear();
        OnPropertyChanged(nameof(CanNavigateBack));
    }
}