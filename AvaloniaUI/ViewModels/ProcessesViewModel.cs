using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Globalization;
using System.Linq;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;
using TCMT.Avalonia.Services.Contracts;

namespace TCMT.Avalonia.ViewModels;

/// <summary>
/// 进程页。字段只用 IPC 现有的四个 —— PID / 名称 / CPU / 内存。用户、线程数、状态、路径、
/// CPU 时间、启动时刻都不在线格式里，补齐要改采集端，不在这里编。
/// 结束进程走能力发现：没有 <see cref="IProcessController"/> 实现时按钮不出现，有实现时
/// 也只在拿到回执之后才动列表 —— 失败如实显示，绝不乐观移除。
/// </summary>
public partial class ProcessesViewModel : ViewModelBase
{
    private const int TrendWindow = AppConstants.MaxChartPoints;

    private readonly IProcessController? _controller;
    private readonly List<ProcessTopEntry> _source = new();
    private readonly Dictionary<int, HistoryBuffer> _trends = new();
    private readonly HistoryBuffer _selectedTrend = new(TrendWindow);

    [ObservableProperty]
    private string _filter = string.Empty;

    [ObservableProperty]
    private string _sortKey = "cpu";

    [ObservableProperty]
    private bool _sortDescending = true;

    [ObservableProperty]
    private ProcessRow? _selected;

    [ObservableProperty]
    private string _statusNote = string.Empty;

    [ObservableProperty]
    private bool _isConfirmingEnd;

    [ObservableProperty]
    private bool _isEnding;

    [ObservableProperty]
    private string _trendPoints = string.Empty;

    [ObservableProperty]
    private int _totalCount;

    public ObservableCollection<ProcessRow> Rows { get; } = new();

    /// <summary>False while no host implementation exists — the view keeps the action hidden.</summary>
    public bool CanEndProcess => _controller is not null;

    public bool HasNoProcesses => Rows.Count == 0;

    public bool HasProcesses => Rows.Count > 0;

    /// <summary>True while the confirm pair is not on screen — the plain button shows instead.</summary>
    public bool IsIdle => !IsConfirmingEnd;

    public bool CanPressConfirm => !IsEnding;

    public bool HasStatusNote => StatusNote.Length > 0;

    public string TrendCaption => Selected is null ? "—" : $"近 {TrendWindow} 个采样点";

    public string PidSortGlyph => GlyphFor("pid");

    public string NameSortGlyph => GlyphFor("name");

    public string CpuSortGlyph => GlyphFor("cpu");

    public string MemorySortGlyph => GlyphFor("memory");

    public ProcessesViewModel(IProcessController? controller = null)
    {
        _controller = controller;
        Title = "进程";
        Icon = "processes";
    }

    public override void Update(SystemInfo info)
    {
        _source.Clear();
        _source.AddRange(info.TopProcesses ?? new List<ProcessTopEntry>());

        // Per-process trend windows, pruned as processes disappear so the map cannot grow forever.
        var livePids = new HashSet<int>(_source.Select(e => e.Pid));

        foreach (var pid in _trends.Keys.Where(pid => !livePids.Contains(pid)).ToList())
        {
            _trends.Remove(pid);
        }

        foreach (var entry in _source)
        {
            if (!_trends.TryGetValue(entry.Pid, out var buffer))
            {
                buffer = new HistoryBuffer(TrendWindow);
                _trends[entry.Pid] = buffer;
            }

            buffer.Push(FormatUtils.ValidateDouble(entry.CpuPercent));
        }

        RebuildRows();
    }

    private string GlyphFor(string key) => SortKey == key ? (SortDescending ? "▾" : "▴") : string.Empty;

    private void NotifySortGlyphs()
    {
        OnPropertyChanged(nameof(PidSortGlyph));
        OnPropertyChanged(nameof(NameSortGlyph));
        OnPropertyChanged(nameof(CpuSortGlyph));
        OnPropertyChanged(nameof(MemorySortGlyph));
    }

    partial void OnFilterChanged(string value) => RebuildRows();

    partial void OnIsConfirmingEndChanged(bool value) => OnPropertyChanged(nameof(IsIdle));

    partial void OnIsEndingChanged(bool value) => OnPropertyChanged(nameof(CanPressConfirm));

    partial void OnStatusNoteChanged(string value) => OnPropertyChanged(nameof(HasStatusNote));

    partial void OnSelectedChanged(ProcessRow? value)
    {
        IsConfirmingEnd = false;
        ReloadTrend();
        OnPropertyChanged(nameof(TrendCaption));
    }

    private void RebuildRows()
    {
        var query = (IEnumerable<ProcessTopEntry>)_source;

        var needle = Filter.Trim();
        if (needle.Length > 0)
        {
            query = query.Where(e =>
                e.Name.Contains(needle, StringComparison.OrdinalIgnoreCase) ||
                e.Pid.ToString(CultureInfo.InvariantCulture).Contains(needle, StringComparison.Ordinal));
        }

        query = SortKey switch
        {
            "pid" => SortDescending ? query.OrderByDescending(e => e.Pid) : query.OrderBy(e => e.Pid),
            "memory" => SortDescending ? query.OrderByDescending(e => e.MemoryBytes) : query.OrderBy(e => e.MemoryBytes),
            "name" => SortDescending
                ? query.OrderByDescending(e => e.Name, StringComparer.OrdinalIgnoreCase)
                : query.OrderBy(e => e.Name, StringComparer.OrdinalIgnoreCase),
            _ => SortDescending ? query.OrderByDescending(e => e.CpuPercent) : query.OrderBy(e => e.CpuPercent)
        };

        var previousPid = Selected?.Pid ?? -1;

        Rows.Clear();
        foreach (var entry in query)
        {
            Rows.Add(new ProcessRow(entry));
        }

        TotalCount = _source.Count;
        Selected = Rows.FirstOrDefault(r => r.Pid == previousPid) ?? Rows.FirstOrDefault();

        OnPropertyChanged(nameof(HasNoProcesses));
        OnPropertyChanged(nameof(HasProcesses));
        NotifySortGlyphs();
    }

    private void ReloadTrend()
    {
        _selectedTrend.Clear();

        if (Selected is not null && _trends.TryGetValue(Selected.Pid, out var buffer))
        {
            foreach (var value in buffer.ToArray())
            {
                _selectedTrend.Push(value);
            }
        }

        TrendPoints = Sparkline.Points(_selectedTrend);
    }

    [RelayCommand]
    private void Sort(string? key)
    {
        if (string.IsNullOrEmpty(key))
        {
            return;
        }

        if (SortKey == key)
        {
            SortDescending = !SortDescending;
        }
        else
        {
            SortKey = key;
            SortDescending = key != "name";
        }

        RebuildRows();
    }

    [RelayCommand]
    private void BeginEndProcess() => IsConfirmingEnd = true;

    [RelayCommand]
    private void CancelEndProcess() => IsConfirmingEnd = false;

    [RelayCommand]
    private async Task ConfirmEndProcessAsync()
    {
        var row = Selected;

        if (_controller is null || row is null)
        {
            IsConfirmingEnd = false;
            return;
        }

        IsConfirmingEnd = false;
        IsEnding = true;

        try
        {
            var result = await _controller.EndAsync(row.Pid);

            StatusNote = result.Succeeded
                ? $"已收到回执 · PID {row.Pid} 结束请求已下发"
                : $"失败 · PID {row.Pid} · {result.Message}";
        }
        catch (Exception ex)
        {
            StatusNote = $"失败 · PID {row.Pid} · {ex.Message}";
        }
        finally
        {
            IsEnding = false;
        }
    }
}
