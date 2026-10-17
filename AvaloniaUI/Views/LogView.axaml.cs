using System.Collections.Specialized;
using Avalonia.Controls;
using Avalonia.Interactivity;
using TCMT.Avalonia.ViewModels;

namespace TCMT.Avalonia.Views;

public partial class LogView : UserControl
{
    private ScrollViewer? _scroll;
    private LogViewModel? _viewModel;

    public LogView()
    {
        InitializeComponent();

        Loaded += OnViewLoaded;
        Unloaded += OnViewUnloaded;
    }

    private void OnViewLoaded(object? sender, RoutedEventArgs e)
    {
        _scroll = this.FindControl<ScrollViewer>("LogScroll");
        _viewModel = DataContext as LogViewModel;

        if (_viewModel is not null)
        {
            _viewModel.Entries.CollectionChanged += OnEntriesChanged;
        }
    }

    private void OnViewUnloaded(object? sender, RoutedEventArgs e)
    {
        if (_viewModel is not null)
        {
            _viewModel.Entries.CollectionChanged -= OnEntriesChanged;
        }

        _viewModel = null;
        _scroll = null;
    }

    /// <summary>While following, stick to the newest entry; while paused, stay where the user is reading.</summary>
    private void OnEntriesChanged(object? sender, NotifyCollectionChangedEventArgs e)
    {
        if (_viewModel is { IsFollowing: true })
        {
            _scroll?.ScrollToEnd();
        }
    }

    /// <summary>Leaving the top pauses following; coming back to the top resumes it.</summary>
    private void OnLogScrollChanged(object? sender, ScrollChangedEventArgs e)
    {
        if (_viewModel is null || _scroll is null)
        {
            return;
        }

        _viewModel.IsFollowing = _scroll.Offset.Y <= 1d;
    }
}
