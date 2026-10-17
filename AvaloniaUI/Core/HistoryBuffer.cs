using System.Collections.Generic;

namespace TCMT.Avalonia.Core;

/// <summary>
/// Fixed-capacity ring buffer for chart history. Replaces the add-then-RemoveAt(0) pattern:
/// once full it overwrites the oldest point instead of shifting the whole list every tick.
/// Capacity should come from <see cref="AppConstants.MaxChartPoints"/>.
/// </summary>
public sealed class HistoryBuffer
{
    private readonly double[] _values;
    private int _next;
    private int _count;

    public HistoryBuffer(int capacity)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(capacity);
        _values = new double[capacity];
    }

    public int Capacity => _values.Length;

    public int Count => _count;

    public bool IsEmpty => _count == 0;

    /// <summary>Most recently pushed value, or 0 when empty.</summary>
    public double Latest => _count == 0 ? 0d : _values[(_next - 1 + _values.Length) % _values.Length];

    public void Push(double value)
    {
        _values[_next] = value;
        _next = (_next + 1) % _values.Length;

        if (_count < _values.Length)
        {
            _count++;
        }
    }

    public void Clear()
    {
        System.Array.Clear(_values, 0, _values.Length);
        _next = 0;
        _count = 0;
    }

    /// <summary>Oldest to newest — the order charts draw in.</summary>
    public IReadOnlyList<double> ToArray()
    {
        var result = new double[_count];
        var start = _count == _values.Length ? _next : 0;

        for (var i = 0; i < _count; i++)
        {
            result[i] = _values[(start + i) % _values.Length];
        }

        return result;
    }

    /// <summary>Peak inside the window. Charts normalise by this so the shape stays steady.</summary>
    public double Max()
    {
        var max = 0d;

        for (var i = 0; i < _count; i++)
        {
            if (_values[i] > max)
            {
                max = _values[i];
            }
        }

        return max;
    }
}
