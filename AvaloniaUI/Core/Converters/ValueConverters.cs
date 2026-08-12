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

public class IconToGeometryConverter : IValueConverter
{
    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        if (value is not string key) return null;
        var path = key switch
        {
            "dashboard"   => "M3 3h7v7H3V3zm11 0h7v7h-7V3zM3 14h7v7H3v-7zm11 0h7v7h-7v-7z",
            "cpu"         => "M9 2v2H7v2H5v2H3v8h2v2h2v2h2v2h6v-2h2v-2h2v-2h2V8h-2V6h-2V4h-2V2H9zm-2 4h10v12H7V6zm3 3h4v6h-4V9zm2 1v4",
            "memory"      => "M5 4h14a2 2 0 012 2v12a2 2 0 01-2 2H5a2 2 0 01-2-2V6a2 2 0 012-2zm0 2v12h14V6H5zm2 2h3v3H7V8zm4 0h3v3h-3V8zm-4 5h3v3H7v-3zm4 0h3v3h-3v-3z",
            "gpu"         => "M1 5a2 2 0 012-2h18a2 2 0 012 2v10a2 2 0 01-2 2h-8v2h5v2H4v-2h5v-2H3a2 2 0 01-2-2V5zm2 0v10h18V5H3zm3 3h2v2H6V8zm3 0h6v4H9V8z",
            "temperature" => "M12 3a4 4 0 00-4 4v7a5 5 0 1010 0V7a4 4 0 00-4-4zm-2 4a2 2 0 114 0v7h-4V7zm2 8a2 2 0 100 4 2 2 0 000-4z",
            "network"     => "M12 2a10 10 0 100 20 10 10 0 000-20zM5.6 7.3A8 8 0 0112 4.2V7h-2.8a15 15 0 00-3.6.3zm.2 3.7a8 8 0 00.2 2h2.4a12 12 0 010-2H5.8zm3.4 0H12v2H9.2a12 12 0 010-2zM12 9V7h2.5a15 15 0 013.9.3A8 8 0 0112 9zm4.6 2a12 12 0 010 2H19a8 8 0 00-1.8-2h-.6zm-5.8 0H12v2h-1.2a12 12 0 010-2zm-4.6 4h1.8a15 15 0 002.5 3.7A8 8 0 016.2 15zm3.2 0H12v3.8a8 8 0 01-2.6-1.1 15 15 0 01-2.2-2.7zm3.8 3.8V15h2.7a15 15 0 01-2.3 2.7A8 8 0 0112 18.8zm4.2-3.8H19a8 8 0 01-4.2 3.8 15 15 0 002.4-3.8h-1z",
            "storage"     => "M12 2a8 8 0 00-8 8 8 8 0 003.2 6.4L5.5 20h2.2l1.2-2.4A8 8 0 0012 18a8 8 0 008-8 8 8 0 00-8-8zm0 2a6 6 0 016 6 6 6 0 01-6 6 6 6 0 01-6-6 6 6 0 016-6zm0 3a3 3 0 100 6 3 3 0 000-6zm0 2a1 1 0 110 2 1 1 0 010-2z",
            "settings"    => "M12 16a4 4 0 100-8 4 4 0 000 8zm7.7-3.6l1.3-.2a1 1 0 00.8-1.2l-.3-1.5a1 1 0 00-1.2-.8l-1.6.3c-.26-.14-.5-.27-.8-.4l-.2-1.7a1 1 0 00-1-.9h-1.5a1 1 0 00-1 .9l-.2 1.7c-.3.13-.54.26-.8.4l-1.6-.3a1 1 0 00-1.2.8l-.3 1.5a1 1 0 00.8 1.2l1.3.2-.02.6-1.3.2a1 1 0 00-.8 1.2l.3 1.5a1 1 0 001.2.8l1.6-.3c.26.14.5.27.8.4l.2 1.7a1 1 0 001 .9h1.5a1 1 0 001-.9l.2-1.7c.3-.13.54-.26.8-.4l1.6.3a1 1 0 001.2-.8l.3-1.5a1 1 0 00-.8-1.2l-1.3-.2.02-.6zm-5.7 2.6a3 3 0 110-6 3 3 0 010 6z",
            _             => null
        };
        if (path == null) return null;
        return StreamGeometry.Parse(path);
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
        => throw new NotImplementedException();
}
