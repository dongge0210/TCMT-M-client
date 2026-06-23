using System;
using System.Globalization;
using Avalonia.Data.Converters;
using Avalonia.Media;
using TCMT.Avalonia.Core;

namespace TCMT.Avalonia.Core.Converters;

internal static class ConverterColors
{
    public static readonly SolidColorBrush Green = new(Color.Parse(AppConstants.Colors.Primary));
    public static readonly SolidColorBrush Blue = new(Color.Parse(AppConstants.Colors.Secondary));
    public static readonly SolidColorBrush Orange = new(Color.Parse(AppConstants.Colors.Warning));
    public static readonly SolidColorBrush Red = new(Color.Parse(AppConstants.Colors.Danger));
    public static readonly SolidColorBrush Gold = new(Color.Parse(AppConstants.Colors.Gold));
    public static readonly SolidColorBrush Gray = new(new Color(255, 128, 128, 128));
    public static readonly SolidColorBrush TextMuted = new(Color.Parse(AppConstants.Colors.TextMuted));
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

public class BoolInvertConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is bool b) return !b;
        return value;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is bool b) return !b;
        return value;
    }
}

public class PercentToAngleConverter : IValueConverter
{
    public double MaxAngle { get; set; } = 260;

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is double percent)
        {
            return Math.Clamp(percent, 0, 100) / 100.0 * MaxAngle;
        }
        return 0.0;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}

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

public class NullToDefaultConverter : IValueConverter
{
    public object? DefaultValue { get; set; } = "N/A";
    
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value == null || (value is string s && string.IsNullOrWhiteSpace(s)))
            return DefaultValue ?? "N/A";
        return value;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}

public class BytesToHumanReadableConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is ulong bytes)
            return FormatUtils.FormatBytes(bytes);
        if (value is long lbytes)
            return FormatUtils.FormatBytes((ulong)Math.Max(0, lbytes));
        return "N/A";
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}

public class FrequencyConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is double freq)
            return FormatUtils.FormatFrequency(freq);
        return "N/A";
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}

public class PowerConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is double mw && mw > 0)
            return $"{mw / 1000.0:F2}W";
        return "";
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}

public class PercentToWidthConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is double percent)
        {
            return Math.Clamp(percent, 0, 100) * 2.4; // 240px max width
        }
        return 0.0;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}

public class NavSelectedToBgConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is bool isSelected && isSelected)
            return new SolidColorBrush(Color.Parse("#3B82F626"));
        return new SolidColorBrush(Colors.Transparent);
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotImplementedException();
}

public class NavSelectedToFgConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is bool isSelected && isSelected)
            return new SolidColorBrush(Color.Parse("#3B82F6"));
        return new SolidColorBrush(Color.Parse("#8B949E"));
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotImplementedException();
}

public class NavSelectedToFontWeightConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is bool isSelected && isSelected)
            return FontWeight.SemiBold;
        return FontWeight.Normal;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotImplementedException();
}

public class GreaterThanZeroToBoolConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is int intValue)
        {
            return intValue > 0;
        }
        if (value is long longValue)
        {
            return longValue > 0;
        }
        if (value is double doubleValue)
        {
            return doubleValue > 0;
        }
        if (value is null)
        {
            return false;
        }
        return false;
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}
