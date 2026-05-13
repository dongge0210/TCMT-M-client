using System;
using System.IO;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using AvaloniaUI.DiagnosticsSupport;
using AvaloniaUI.ViewModels;
using AvaloniaUI.Views;

namespace AvaloniaUI;

public partial class App : Application
{
    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            var viewModel = new MainWindowViewModel();
            var mainWindow = new MainWindow
            {
                DataContext = viewModel,
            };

            desktop.MainWindow = mainWindow;

            // Set data context for tray icon commands
            this.DataContext = viewModel;

            // Handle window closing to minimize to tray
            mainWindow.Closing += OnMainWindowClosing;

            // Attach DevTools for previewer
            this.AttachDeveloperTools(o =>
            {
                o.AutoConnectFromDesignMode = true;
            });

            // Start monitoring
            viewModel.Start();
        }

        base.OnFrameworkInitializationCompleted();
    }

    private void OnMainWindowClosing(object? sender, WindowClosingEventArgs e)
    {
        // Cancel close and hide to tray instead
        e.Cancel = true;
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.MainWindow?.Hide();
        }
    }
}