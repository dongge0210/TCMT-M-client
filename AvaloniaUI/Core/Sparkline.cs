using System;
using System.Globalization;
using System.Text;

namespace TCMT.Avalonia.Core;

/// <summary>
/// Builds the point list a <c>Polyline</c> draws. Both series of a chart are normalised to the
/// window peak, so two lines sharing one box stay comparable — a shared scale is the only way
/// down and up can be read against each other.
/// </summary>
public static class Sparkline
{
    public const double DefaultWidth = 100;
    public const double DefaultHeight = 40;

    /// <summary>Empty when the window holds fewer than two points — a line needs two.</summary>
    public static string Points(HistoryBuffer buffer, double width = DefaultWidth, double height = DefaultHeight)
    {
        var values = buffer.ToArray();

        if (values.Count < 2)
        {
            return string.Empty;
        }

        var peak = buffer.Max();
        if (peak <= 0)
        {
            peak = 1;
        }

        var builder = new StringBuilder(values.Count * 12);

        for (var i = 0; i < values.Count; i++)
        {
            var x = i * (width / (values.Count - 1));
            var y = height - (values[i] / peak * (height - 2)) - 1;

            if (i > 0)
            {
                builder.Append(' ');
            }

            builder.Append(x.ToString("0.##", CultureInfo.InvariantCulture))
                   .Append(',')
                   .Append(y.ToString("0.##", CultureInfo.InvariantCulture));
        }

        return builder.ToString();
    }
}
