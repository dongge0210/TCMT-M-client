using System;
using System.Globalization;
using Avalonia.Data.Converters;
using Avalonia.Media;

namespace AvaloniaUI.Converters;

internal static class ConverterColors
{
    public static readonly SolidColorBrush Green  = new SolidColorBrush(Color.Parse("#FF4CAF50"));
    public static readonly SolidColorBrush Orange = new SolidColorBrush(Color.Parse("#FFFF9800"));
    public static readonly SolidColorBrush Red    = new SolidColorBrush(Color.Parse("#FFF44336"));
    public static readonly SolidColorBrush Gray   = new SolidColorBrush(Colors.Gray);
}

/// <summary>
/// Converts null to default value, useful for binding errors
/// </summary>
public class NullToDefaultConverter : IValueConverter
{
    public object? DefaultValue { get; set; } = "N/A";
    
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value == null)
            return DefaultValue ?? "N/A";
        return value;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}

public class PercentToAngleConverter : IValueConverter
{
    public double MaxAngle { get; set; } = 260;

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is double percent)
        {
            return (percent / 100.0) * MaxAngle;
        }
        return 0.0;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}

public class BoolToColorConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is bool isConnected)
        {
            return isConnected ? ConverterColors.Green : ConverterColors.Red;
        }
        return ConverterColors.Gray;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}

/// <summary>
/// Converts percentage value to warning color (green -> yellow -> red)
/// </summary>
public class PercentToWarningColorConverter : IValueConverter
{
    public double WarningThreshold { get; set; } = 80;
    public double CriticalThreshold { get; set; } = 95;

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is double percent)
        {
            if (percent >= CriticalThreshold)
                return ConverterColors.Red;
            if (percent >= WarningThreshold)
                return ConverterColors.Orange;
            return ConverterColors.Green;
        }
        return ConverterColors.Gray;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}

