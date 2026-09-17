namespace TCMT.Avalonia.Core;

/// <summary>Severity of a reading relative to its threshold.</summary>
public enum ThresholdLevel
{
    Normal,
    Warning,
    Critical
}

/// <summary>
/// One threshold rule. <paramref name="LowerIsWorse"/> covers battery charge and health,
/// where a smaller number is the bad one.
/// </summary>
public sealed class ThresholdRule
{
    public ThresholdRule(string metric, double warning, double critical, bool lowerIsWorse, string unit)
    {
        Metric = metric;
        Warning = warning;
        Critical = critical;
        LowerIsWorse = lowerIsWorse;
        Unit = unit;
    }

    public string Metric { get; }

    public double Warning { get; }

    public double Critical { get; }

    public bool LowerIsWorse { get; }

    public string Unit { get; }
}

/// <summary>
/// The single threshold table (TUI design #5: thresholds live in one table, colour is only
/// given to out-of-range values). No view, converter or view-model carries its own numbers
/// or its own colours — they ask here and paint with the theme brushes.
/// </summary>
public static class Thresholds
{
    public static readonly ThresholdRule Load = new("CPU / RAM / GPU load", 70, 90, false, "%");

    public static readonly ThresholdRule Temperature = new("Sensor / core temperature", 60, 80, false, "°C");

    public static readonly ThresholdRule SingleProcessCpu = new("Single process CPU", 20, 50, false, "%");

    public static readonly ThresholdRule BatteryCharge = new("Battery charge", 50, 20, true, "%");

    public static readonly ThresholdRule BatteryHealth = new("Battery health", 80, 60, true, "%");

    /// <summary>Normal / Warning / Critical for a reading. Normal reads stay uncoloured.</summary>
    public static ThresholdLevel Evaluate(double value, ThresholdRule rule)
    {
        if (rule.LowerIsWorse)
        {
            if (value <= rule.Critical)
            {
                return ThresholdLevel.Critical;
            }

            return value <= rule.Warning ? ThresholdLevel.Warning : ThresholdLevel.Normal;
        }

        if (value >= rule.Critical)
        {
            return ThresholdLevel.Critical;
        }

        return value >= rule.Warning ? ThresholdLevel.Warning : ThresholdLevel.Normal;
    }
}
