using TCMT.Avalonia.Core;
using TCMT.Avalonia.Models;

namespace TCMT.Avalonia.ViewModels;

/// <summary>
/// One row of a process list: the IPC entry plus the level the threshold table gives its CPU
/// reading, so the view colours the one value that is out of range and nothing else.
/// </summary>
public sealed class ProcessRow
{
    public ProcessRow(ProcessTopEntry entry)
    {
        Entry = entry;
        Level = Thresholds.Evaluate(entry.CpuPercent, Thresholds.SingleProcessCpu);
    }

    public ProcessTopEntry Entry { get; }

    public ThresholdLevel Level { get; }

    public int Pid => Entry.Pid;

    public string Name => Entry.Name;

    public string CpuDisplay => Entry.CpuDisplay;

    public string MemoryDisplay => Entry.MemoryDisplay;

    public bool IsWarning => Level == ThresholdLevel.Warning;

    public bool IsCritical => Level == ThresholdLevel.Critical;

    /// <summary>True whenever the reading is out of range, either tier.</summary>
    public bool HasLevel => Level != ThresholdLevel.Normal;

    /// <summary>Out-of-range readings say so in words — colour is never the only carrier.</summary>
    public string LevelText => Level switch
    {
        ThresholdLevel.Critical => "临界",
        ThresholdLevel.Warning => "警示",
        _ => string.Empty
    };
}
