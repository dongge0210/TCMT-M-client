using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Models;
using TCMT.Avalonia.Services;
using TCMT.Avalonia.Localization;

namespace TCMT.Avalonia.ViewModels;

/// <summary>
/// Log page. Entries come from <see cref="LogStream"/> (the in-memory Serilog sink) rather
/// than SystemInfo, so this view-model is fed by <see cref="Append"/> instead of Update.
///
/// Follow mode follows the terminal convention: scrolling away from the top pauses following,
/// returning to the top resumes it. The view owns the scroll position and pushes it in here.
/// </summary>
public partial class LogViewModel : ViewModelBase
{
    private const int MaxEntries = 500;

    [ObservableProperty]
    private ObservableCollection<LogEntry> _entries = new();

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(IsPaused))]
    private bool _isFollowing = true;

    public LogViewModel()
    {
        Title = Translator.T("日志");
    }

    public bool IsPaused => !IsFollowing;

    public string CountText => Translator.T("{0} 条", Entries.Count);

    public bool IsEmpty => Entries.Count == 0;

    public bool HasEntries => Entries.Count > 0;

    /// <summary>Call from the LogStream.EntryWritten handler (marshal to the UI thread first).</summary>
    public void Append(LogEntry entry)
    {
        Entries.Add(entry);

        while (Entries.Count > MaxEntries)
        {
            Entries.RemoveAt(0);
        }

        OnPropertyChanged(nameof(CountText));
        OnPropertyChanged(nameof(IsEmpty));
        OnPropertyChanged(nameof(HasEntries));
    }

    /// <summary>Logs do not come from the hardware snapshot; nothing to update from here.</summary>
    public override void Update(SystemInfo info)
    {
    }
}
