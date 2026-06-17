using System;

namespace TCMT.Avalonia.Core;

public static class FormatUtils
{
    private static readonly string[] ByteSizes = { "B", "KB", "MB", "GB", "TB" };
    private static readonly string[] BitSpeeds = { "bps", "Kbps", "Mbps", "Gbps" };
    private static readonly string[] ByteSpeeds = { "B/s", "KB/s", "MB/s", "GB/s" };

    public static string FormatBytes(ulong bytes)
    {
        if (bytes == 0) return "0 B";
        
        double len = bytes;
        int order = 0;
        while (len >= 1024 && order < ByteSizes.Length - 1)
        {
            order++;
            len /= 1024;
        }
        return order == 0 ? $"{len:0} {ByteSizes[order]}" : $"{len:0.##} {ByteSizes[order]}";
    }

    public static string FormatNetworkSpeed(ulong bitsPerSec)
    {
        if (bitsPerSec == 0) return "0 bps";
        
        double len = bitsPerSec;
        int order = 0;
        while (len >= 1000 && order < BitSpeeds.Length - 1)
        {
            order++;
            len /= 1000;
        }
        return $"{len:0.##} {BitSpeeds[order]}";
    }

    public static string FormatSpeed(ulong bytesPerSec)
    {
        if (bytesPerSec == 0) return "0 B/s";
        
        double len = bytesPerSec;
        int order = 0;
        while (len >= 1024 && order < ByteSpeeds.Length - 1)
        {
            order++;
            len /= 1024;
        }
        return $"{len:0.##} {ByteSpeeds[order]}";
    }

    public static string FormatFrequency(double frequencyMhz)
    {
        if (double.IsNaN(frequencyMhz) || frequencyMhz <= 0) return "N/A";
        if (frequencyMhz >= 1000)
            return $"{frequencyMhz / 1000:F2} GHz";
        return $"{frequencyMhz:F0} MHz";
    }

    public static string FormatPower(double powerMw)
    {
        if (powerMw <= 0) return "";
        return $"{powerMw / 1000.0:F2}W";
    }

    public static string FormatPercentage(double value)
    {
        if (double.IsNaN(value) || double.IsInfinity(value)) return "0%";
        return $"{Math.Clamp(value, 0, 100):F1}%";
    }

    public static string FormatTemperature(double temp)
    {
        if (temp <= 0 || temp > 150) return "N/A";
        return $"{temp:F1}°C";
    }

    public static double ValidateDouble(double value)
    {
        if (double.IsNaN(value) || double.IsInfinity(value) || value < 0) 
            return 0;
        return value;
    }
}
