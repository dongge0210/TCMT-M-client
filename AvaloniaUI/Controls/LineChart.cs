using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using System;
using System.Collections.Generic;
using System.Linq;

namespace AvaloniaUI.Controls;

public class LineChart : Control
{
    public static readonly StyledProperty<IEnumerable<double>?> ValuesProperty =
        AvaloniaProperty.Register<LineChart, IEnumerable<double>?>(nameof(Values));
    public static readonly StyledProperty<IBrush?> LineBrushProperty =
        AvaloniaProperty.Register<LineChart, IBrush?>(nameof(LineBrush), new SolidColorBrush(Color.Parse("#FF4CAF50")));
    public static readonly StyledProperty<IBrush?> FillBrushProperty =
        AvaloniaProperty.Register<LineChart, IBrush?>(nameof(FillBrush), new SolidColorBrush(Color.Parse("#334CAF50")));

    public IEnumerable<double>? Values { get => GetValue(ValuesProperty); set => SetValue(ValuesProperty, value); }
    public IBrush? LineBrush { get => GetValue(LineBrushProperty); set => SetValue(LineBrushProperty, value); }
    public IBrush? FillBrush { get => GetValue(FillBrushProperty); set => SetValue(FillBrushProperty, value); }

    static LineChart()
    {
        AffectsRender<LineChart>(ValuesProperty, LineBrushProperty, FillBrushProperty);
    }

    public override void Render(DrawingContext context)
    {
        var values = Values?.ToList();
        if (values == null || values.Count < 2) return;
        double w = Bounds.Width;
        double h = Bounds.Height;
        if (w <= 0 || h <= 0) return;

        double min = values.Min();
        double max = values.Max();
        if (max == min) { min -= 10; max += 10; }
        double range = max - min;
        int n = values.Count;

        var points = new Point[n];
        for (int i = 0; i < n; i++)
        {
            double x = (double)i / (n - 1) * w;
            double y = h - (values[i] - min) / range * (h - 4) - 2;
            points[i] = new Point(x, y);
        }

        // Fill
        var fillGeo = new StreamGeometry();
        using (var ctx = fillGeo.Open())
        {
            ctx.BeginFigure(points[0], true);
            for (int i = 1; i < n; i++) ctx.LineTo(points[i]);
            ctx.LineTo(new Point(w, h));
            ctx.LineTo(new Point(0, h));
            ctx.EndFigure(true);
        }
        context.DrawGeometry(FillBrush, null, fillGeo);

        // Line
        var lineGeo = new StreamGeometry();
        using (var ctx = lineGeo.Open())
        {
            ctx.BeginFigure(points[0], false);
            for (int i = 1; i < n; i++) ctx.LineTo(points[i]);
            ctx.EndFigure(false);
        }
        context.DrawGeometry(null, new Pen(LineBrush ?? Brushes.Green, 2), lineGeo);
    }
}
