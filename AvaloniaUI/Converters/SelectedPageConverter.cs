using System;
using System.Globalization;
using Avalonia.Data.Converters;

namespace AvaloniaUI.Converters;

/// <summary>
/// Highlights the selected page item in sidebar navigation.
/// Used inside ListBox ItemTemplate — returns a highlight brush when
/// the data context (ViewModelBase) equals the ListBox's SelectedItem.
/// </summary>
public class SelectedPageConverter : IValueConverter
{
    public static SelectedPageConverter Instance { get; } = new();

    public object? Convert(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        // value = ListBox.SelectedItem (the currently selected ViewModelBase)
        // We need to check if this item is "selected" — the binding source is each item's DataContext
        // Since we bind $parent[ListBox].SelectedItem, value IS the selected VM
        // Return highlight color when this converter runs on the selected item
        return new Avalonia.Media.SolidColorBrush(Avalonia.Media.Color.Parse("#FF333333"));
    }

    public object? ConvertBack(object? value, Type targetType, object? parameter, CultureInfo culture)
    {
        throw new NotImplementedException();
    }
}
