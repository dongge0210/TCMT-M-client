using System.Threading.Tasks;

namespace TCMT.Avalonia.Services.Contracts;

/// <summary>
/// The result of one end-process request, as reported back by the host.
/// </summary>
/// <param name="Succeeded">True only when the host acknowledged the request.</param>
/// <param name="Message">What the host said — shown to the user as-is, success or failure.</param>
public sealed record ProcessEndResult(bool Succeeded, string Message);

/// <summary>
/// Process control, kept separate from the read-only <see cref="IHardwareService"/> on purpose.
/// The process page shows the action only when a host actually provides one — the same
/// "show what has a handler, never fake the rest" rule the platform capability layer follows.
/// No implementation is registered today: the collector exposes no process control, so the
/// button stays hidden rather than pretending.
/// </summary>
public interface IProcessController
{
    /// <summary>Sends SIGTERM (or SIGKILL when <paramref name="force"/> is set) and waits for the reply.</summary>
    Task<ProcessEndResult> EndAsync(int pid, bool force = false);
}
