using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;
using TCMT.Avalonia.Localization;

namespace TCMT.Avalonia.ViewModels;

/// <summary>
/// One row of a process list: the IPC entry plus the level the threshold table gives its CPU
/// reading, so the view colours the one value that is out of range and nothing else.
///
/// 可以原地更新（<see cref="Update"/>）。两个列表页都靠这个避免整表重建 —— 见
/// <see cref="SyncInto"/> 里对爆闪的说明。
/// </summary>
public sealed class ProcessRow : ObservableObject
{
    private ProcessTopEntry _entry;
    private ThresholdLevel _level;

    public ProcessRow(ProcessTopEntry entry)
    {
        _entry = entry;
        _level = Thresholds.Evaluate(entry.CpuPercent, Thresholds.SingleProcessCpu);
    }

    public ProcessTopEntry Entry => _entry;

    public int Pid => _entry.Pid;

    public string Name => _entry.Name;

    public string CpuDisplay => _entry.CpuDisplay;

    public string MemoryDisplay => _entry.MemoryDisplay;

    public ThresholdLevel Level => _level;

    public bool IsWarning => _level == ThresholdLevel.Warning;

    public bool IsCritical => _level == ThresholdLevel.Critical;

    /// <summary>True whenever the reading is out of range, either tier.</summary>
    public bool HasLevel => _level != ThresholdLevel.Normal;

    /// <summary>Out-of-range readings say so in words — colour is never the only carrier.</summary>
    public string LevelText => _level switch
    {
        ThresholdLevel.Critical => Translator.T("临界"),
        ThresholdLevel.Warning => Translator.T("警示"),
        _ => string.Empty
    };

    /// <summary>
    /// 换掉这一行的读数。行是跨采样周期复用的，所以派生属性必须逐个通知 ——
    /// 通知漏一个，那一格就会停在旧值上。
    /// </summary>
    public void Update(ProcessTopEntry entry)
    {
        _entry = entry;
        _level = Thresholds.Evaluate(entry.CpuPercent, Thresholds.SingleProcessCpu);
        OnPropertyChanged(nameof(Entry));
        OnPropertyChanged(nameof(Name));
        OnPropertyChanged(nameof(CpuDisplay));
        OnPropertyChanged(nameof(MemoryDisplay));
        OnPropertyChanged(nameof(Level));
        OnPropertyChanged(nameof(IsWarning));
        OnPropertyChanged(nameof(IsCritical));
        OnPropertyChanged(nameof(HasLevel));
        OnPropertyChanged(nameof(LevelText));
        // Pid 不用通知：行按 pid 复用，pid 变了那就是另一行了。
    }

    /// <summary>
    /// 按 pid 把 <paramref name="entries"/> 同步进 <paramref name="rows"/>：能复用的行复用，
    /// 只对真正的差异做增 / 删 / 移。
    ///
    /// 为什么不能 Clear + Add：<c>ObservableCollection.Clear</c> 发的是 Reset，ListBox 收到
    /// 就把全部行容器销毁重建，重建期间列表是空的 —— 每个采样周期闪一帧，就是看得见的爆闪。
    /// 这里只做增删移，既有行的容器留着，改的只是属性，所以不闪。
    /// </summary>
    public static void SyncInto(ObservableCollection<ProcessRow> rows, IEnumerable<ProcessTopEntry> entries)
    {
        var existing = new Dictionary<int, ProcessRow>(rows.Count);
        foreach (var row in rows) existing[row.Pid] = row;

        var wanted = new List<ProcessRow>();
        var seen = new HashSet<int>();
        foreach (var entry in entries)
        {
            if (!seen.Add(entry.Pid)) continue;   // 同一 pid 只留第一条
            if (existing.TryGetValue(entry.Pid, out var row)) row.Update(entry);
            else row = new ProcessRow(entry);
            wanted.Add(row);
        }

        // 先移除已经消失的进程
        for (int i = rows.Count - 1; i >= 0; i--)
        {
            if (!seen.Contains(rows[i].Pid)) rows.RemoveAt(i);
        }

        // 再按目标顺序落位：Move 不会重建容器，所以排序变化也不会闪
        for (int i = 0; i < wanted.Count; i++)
        {
            int current = rows.IndexOf(wanted[i]);
            if (current < 0) rows.Insert(i, wanted[i]);
            else if (current != i) rows.Move(current, i);
        }
    }
}
