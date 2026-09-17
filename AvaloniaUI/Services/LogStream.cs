using System.Collections.Generic;
using Serilog.Core;
using Serilog.Events;

namespace TCMT.Avalonia.Services;

/// <summary>One event in the in-memory log ring.</summary>
public sealed class LogEntry
{
    public LogEntry(System.DateTimeOffset timestamp, LogEventLevel level, string source, string message, string? exception)
    {
        Timestamp = timestamp;
        Level = level;
        Source = source;
        Message = message;
        Exception = exception;
    }

    public System.DateTimeOffset Timestamp { get; }

    public LogEventLevel Level { get; }

    /// <summary>Originating context (the logger's SourceContext), a technical identifier — not translated.</summary>
    public string Source { get; }

    public string Message { get; }

    public string? Exception { get; }

    public string TimeText => Timestamp.ToLocalTime().ToString("HH:mm:ss");

    public string LevelText => Level switch
    {
        LogEventLevel.Warning => "WARN",
        LogEventLevel.Error or LogEventLevel.Fatal => "ERROR",
        _ => "INFO"
    };

    public bool IsWarn => Level == LogEventLevel.Warning;

    public bool IsError => Level is LogEventLevel.Error or LogEventLevel.Fatal;

    public bool IsInfo => !IsWarn && !IsError;
}

/// <summary>
/// Serilog sink that keeps the tail of the log in memory so the client can show a log page
/// without reading today's log file back from disk.
///
/// Wire it up in Program.cs next to the existing sinks:
/// <code>
/// var logStream = new LogStream();
/// Log.Logger = new LoggerConfiguration()
///     .WriteTo.Sink(logStream)
///     .WriteTo.File(...)
///     .CreateLogger();
/// </code>
/// The sink is safe for concurrent writers; <see cref="EntryWritten"/> is raised on the
/// calling thread, so a view should marshal to the UI thread itself.
/// </summary>
public sealed class LogStream : ILogEventSink
{
    private const int Capacity = 500;

    private readonly object _gate = new();
    private readonly Queue<LogEntry> _entries = new(Capacity);

    public event System.EventHandler<LogEntry>? EntryWritten;

    public void Emit(LogEvent logEvent)
    {
        if (logEvent is null)
        {
            return;
        }

        var source = logEvent.Properties.TryGetValue("SourceContext", out var value)
            ? value.ToString().Trim('"').Split('.').LastOrDefault() ?? "app"
            : "app";

        var entry = new LogEntry(
            logEvent.Timestamp,
            logEvent.Level,
            source,
            logEvent.RenderMessage(),
            logEvent.Exception?.Message);

        lock (_gate)
        {
            _entries.Enqueue(entry);

            while (_entries.Count > Capacity)
            {
                _entries.Dequeue();
            }
        }

        EntryWritten?.Invoke(this, entry);
    }

    /// <summary>Oldest to newest.</summary>
    public IReadOnlyList<LogEntry> Snapshot()
    {
        lock (_gate)
        {
            return _entries.ToArray();
        }
    }
}
