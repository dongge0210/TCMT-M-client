using System;
using System.Globalization;

namespace TCMT.Avalonia.ViewModels;

/// <summary>
/// One per-core column in the overview CPU panel. The IPC carries each core's temperature and
/// frequency — not its load — so the bar measures temperature and the panel says so; a core bar
/// painted from temperature but labelled as load would be a fabricated reading.
/// </summary>
public sealed class CoreBar
{
    /// <summary>Fill height in the view; a core bar is a fraction of this track.</summary>
    public const double TrackHeight = 34;

    public CoreBar(int index, double temperature)
    {
        Index = index;
        Temperature = temperature;
        BarHeight = Math.Clamp(temperature, 0, 100) / 100 * TrackHeight;
    }

    public int Index { get; }

    public double Temperature { get; }

    public double BarHeight { get; }

    public string TempDisplay => Temperature.ToString("F0", CultureInfo.InvariantCulture) + " °C";
}

/// <summary>One sensor line in the overview thermal panel.</summary>
public sealed class SensorRow
{
    public SensorRow(string name, double temperature)
    {
        Name = name;
        Temperature = temperature;
    }

    public string Name { get; }

    public double Temperature { get; }

    public string TempDisplay => Temperature.ToString("F0", CultureInfo.InvariantCulture) + " °C";
}
