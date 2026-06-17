using System.Threading.Tasks;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Layout;
using Avalonia.Media;
using TCMT.Avalonia.Services.Contracts;

namespace TCMT.Avalonia.Services;

public class DialogService : IDialogService
{
    private Window? GetMainWindow()
    {
        if (Application.Current?.ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            return desktop.MainWindow;
        }
        return null;
    }

    public async Task ShowInfoAsync(string title, string message)
    {
        var dialog = new Window
        {
            Title = title,
            Width = 400,
            Height = 200,
            WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Content = new StackPanel
            {
                Margin = new Thickness(20),
                Children =
                {
                    new TextBlock { Text = message, TextWrapping = TextWrapping.Wrap },
                    new Button { Content = "OK", HorizontalAlignment = HorizontalAlignment.Center }
                }
            }
        };

        var mainWindow = GetMainWindow();
        if (mainWindow != null)
        {
            await dialog.ShowDialog(mainWindow);
        }
    }

    public Task ShowWarningAsync(string title, string message)
    {
        return ShowInfoAsync(title, message); // Simplified for now
    }

    public Task ShowErrorAsync(string title, string message)
    {
        return ShowInfoAsync(title, message); // Simplified for now
    }

    public Task<bool> ShowConfirmAsync(string title, string message)
    {
        // Simplified implementation
        return Task.FromResult(true);
    }
}