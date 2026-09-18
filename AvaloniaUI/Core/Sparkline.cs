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

    /// <summary>A zero-length line when the window holds fewer than two points — a line needs two,
    /// and the string still has to parse (see the note in the body).</summary>
    public static string Points(HistoryBuffer buffer, double width = DefaultWidth, double height = DefaultHeight)
    {
        var values = buffer.ToArray();

        if (values.Count < 2)
        {
            // The result is fed to a Polyline. An empty string parses to a null point list, and
            // Polyline throws on null while building its geometry — from Measure *and* from Render,
            // so hiding the shape is not sufficient on its own. Two coincident points parse cleanly
            // and draw nothing, which is what "not enough samples" should look like.
            return "0,0 0,0";
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
